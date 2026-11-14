// Controller frames, model selection and calibration shared by the driver and tests.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <stdexcept>
#include "nlohmann/json.hpp"
#include "openvr/openvr_driver.h"
#include "vr_bridge.h"

namespace rincast::controllers {
struct Quat {
    double w, x, y, z;
};
inline Quat Mul(const Quat& a, const Quat& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
inline void Rotate(const Quat& q, const double v[3], double out[3]) {
    const Quat p{0, v[0], v[1], v[2]};
    const Quat c{q.w, -q.x, -q.y, -q.z};
    const Quat r = Mul(Mul(q, p), c);
    out[0] = r.x;
    out[1] = r.y;
    out[2] = r.z;
}
// Rigid transform: rotation q, then translation t.
struct RigidPose {
    Quat q{1, 0, 0, 0};
    double t[3] = {0, 0, 0};
};
inline RigidPose Compose(const RigidPose& a, const RigidPose& b) {  // a * b: b expressed in a's frame
    RigidPose r;
    r.q = Mul(a.q, b.q);
    Rotate(a.q, b.t, r.t);
    for (int i = 0; i < 3; i++) r.t[i] += a.t[i];
    return r;
}
inline RigidPose Invert(const RigidPose& p) {
    RigidPose r;
    r.q = {p.q.w, -p.q.x, -p.q.y, -p.q.z};
    Rotate(r.q, p.t, r.t);
    for (double& v : r.t) v = -v;
    return r;
}

inline std::array<double, 3> OffsetLinearVelocity(const rcvr::BridgePose& grip, const double offsetWorld[3]) {
    const auto& w = grip.angularVelocity;
    return {grip.linearVelocity[0] + w[1] * offsetWorld[2] - w[2] * offsetWorld[1],
            grip.linearVelocity[1] + w[2] * offsetWorld[0] - w[0] * offsetWorld[2],
            grip.linearVelocity[2] + w[0] * offsetWorld[1] - w[1] * offsetWorld[0]};
}

// OpenXR grip -> native SteamVR device frame. Pico 4 and Ultra use the model's
// handgrip frame; the Pico 4 model also contains a legacy Touch openxr_grip
// transform, which must not define a native Pico device's raw pose.
inline RigidPose GripToDevice(int hand, bool nativePico = false) {
    RigidPose gripInDevice;
    const double half = (nativePico ? 5.037 : 20.6) * 3.14159265358979 / 360;
    gripInDevice.q = {std::cos(half), std::sin(half), 0, 0};
    gripInDevice.t[0] = hand == 0 ? 0.007 : -0.007;
    gripInDevice.t[1] = -0.00182941;
    gripInDevice.t[2] = 0.1019482;
    if (nativePico) {
        gripInDevice.t[0] = 0;
        gripInDevice.t[1] = 0.003;
        gripInDevice.t[2] = 0.097;
    }
    return Invert(gripInDevice);  // device pose in grip space
}

inline Quat FromEuler(double pitchDeg, double yawDeg, double rollDeg) {
    const double p = pitchDeg * 3.14159265358979 / 360, y = yawDeg * 3.14159265358979 / 360, r = rollDeg * 3.14159265358979 / 360;
    const Quat qx{std::cos(p), std::sin(p), 0, 0}, qy{std::cos(y), 0, std::sin(y), 0}, qz{std::cos(r), 0, 0, std::sin(r)};
    return Mul(Mul(qy, qx), qz);
}

inline RigidPose BridgeRigidPose(const rcvr::BridgePose& p) {
    Quat q{p.orientation[3], p.orientation[0], p.orientation[1], p.orientation[2]};
    const double length = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (!std::isfinite(length) || length < 1e-6) throw std::runtime_error("Invalid controller orientation");
    q = {q.w / length, q.x / length, q.y / length, q.z / length};
    for (float value : p.position)
        if (!std::isfinite(value)) throw std::runtime_error("Invalid controller position");
    return {q, {p.position[0], p.position[1], p.position[2]}};
}

inline RigidPose AimInDevice(const rcvr::BridgePose& grip, const rcvr::BridgePose& aim, const RigidPose& deviceInGrip) {
    return Compose(Invert(Compose(BridgeRigidPose(grip), deviceInGrip)), BridgeRigidPose(aim));
}

inline vr::HmdMatrix34_t PoseMatrix(const RigidPose& p) {
    vr::HmdMatrix34_t m{};
    for (int col = 0; col < 3; ++col) {
        double axis[3]{}, rotated[3];
        axis[col] = 1;
        Rotate(p.q, axis, rotated);
        for (int row = 0; row < 3; ++row) m.m[row][col] = float(rotated[row]);
    }
    for (int row = 0; row < 3; ++row) m.m[row][3] = float(p.t[row]);
    return m;
}

inline void RebaseControllerBones(const vr::VRBoneTransform_t* source, vr::VRBoneTransform_t* target, int hand) {
    std::copy_n(source, rcvr::kHandBones, target);
    const auto oldInNew = Compose(Invert(GripToDevice(hand, true)), GripToDevice(hand, false));
    // Wrist and auxiliary tips are children of the raw device root. Finger joints
    // remain relative to their parents. This preserves every bone's world pose.
    for (int i : {1, 26, 27, 28, 29, 30}) {
        const auto& b = source[i];
        const auto transformed = Compose(oldInNew, {{b.orientation.w, b.orientation.x, b.orientation.y, b.orientation.z},
                                                    {b.position.v[0], b.position.v[1], b.position.v[2]}});
        target[i].orientation = {float(transformed.q.w), float(transformed.q.x), float(transformed.q.y), float(transformed.q.z)};
        for (int j = 0; j < 3; ++j) target[i].position.v[j] = float(transformed.t[j]);
    }
}

struct ModelNames {
    std::string hmd;
    std::string controllerLeft;  // render models
    std::string controllerRight;
    bool pico = false;
    bool picoUltra = false;
};

inline ModelNames NamesFor(const std::string& model, bool touch = false) {
    std::string lower = model;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    // Android models include marketing names and domestic PICO 4 codenames.
    const bool pico4 = lower.find("pico 4") != std::string::npos || lower.find("pico4") != std::string::npos || lower == "a8110" ||
                       lower == "a81x0" || lower == "a8110 pro" || lower == "phoenix" || lower == "phoenix pro";
    if (pico4 && !touch) {
        if (lower.find("ultra") != std::string::npos)
            return {model, "{vrlink}pico_4_ultra_controller_left", "{vrlink}pico_4_ultra_controller_right", true, true};
        return {model, "{vrlink}pico_4_controller_left", "{vrlink}pico_4_controller_right", true};
    }
    if (model == "Quest") return {"Oculus Quest", "oculus_quest_controller_left", "oculus_quest_controller_right"};
    if (model == "Quest 2") return {"Oculus Quest2", "oculus_quest2_controller_left", "oculus_quest2_controller_right"};
    if (model == "Quest Pro") return {"Meta Quest Pro", "oculus_quest_plus_controller_left", "oculus_quest_plus_controller_right"};
    if (model == "Quest 3S") return {"Oculus Quest3S", "oculus_quest_plus_controller_left", "oculus_quest_plus_controller_right"};
    return {model.empty() || model == "Quest 3" ? "Oculus Quest3" : model, "oculus_quest_plus_controller_left",
            "oculus_quest_plus_controller_right"};
}

// A runtime profile identifies the controller independently of the headset model.
// Unknown or absent profiles are never silently substituted with Pico or Touch.
inline std::optional<ModelNames> ControllerNamesFor(const std::string& headset, const std::string& profile, bool touch) {
    if (touch) return NamesFor(headset, true);
    if (profile == "/interaction_profiles/bytedance/pico4_controller") return NamesFor("PICO 4");
    if (profile == "/interaction_profiles/bytedance/pico4s_controller" ||
        profile == "/interaction_profiles/bytedance/pico_ultra_controller_bd")
        return NamesFor("PICO 4 Ultra");
    if (profile == "/interaction_profiles/oculus/touch_controller") {
        // The common Touch profile does not identify a hardware generation. Use a
        // Quest headset's matching visual model only after the runtime reports Touch.
        auto names = NamesFor(headset.rfind("Quest", 0) == 0 ? headset : "Quest 3", true);
        return names;
    }
    if (profile == "/interaction_profiles/facebook/touch_controller_pro") return NamesFor("Quest Pro", true);
    if (profile == "/interaction_profiles/meta/touch_pro_controller") return NamesFor("Quest Pro", true);
    if (profile == "/interaction_profiles/meta/touch_controller_plus") return NamesFor("Quest 3", true);
    if (profile == "/interaction_profiles/meta/touch_plus_controller") return NamesFor("Quest 3", true);
    return std::nullopt;
}

inline bool HasLiveInputSample(const rcvr::TrackingSnapshot& tracking, int hand, bool trackedHand, int64_t now) {
    if (hand < 0 || hand > 1 || tracking.receivedPcNs <= 0 || now < tracking.receivedPcNs || now - tracking.receivedPcNs >= 500'000'000LL)
        return false;
    const auto flags = tracking.controllers[hand].flags;
    return (flags & 3) == 3 && ((flags & 4) != 0) == trackedHand;
}

inline nlohmann::json ModelComponent(const RigidPose& pose) {
    const auto& q = pose.q;
    constexpr double deg = 180 / 3.141592653589793;
    const auto rounded = [](double value, double scale) {
        const double result = std::round(value * scale) / scale;
        return result == 0 ? 0.0 : result;  // canonicalise negative zero for stable cache keys
    };
    const double x = atan2(2 * (q.w * q.x + q.y * q.z), 1 - 2 * (q.x * q.x + q.y * q.y)) * deg;
    const double y = asin(std::clamp(2 * (q.w * q.y - q.z * q.x), -1.0, 1.0)) * deg;
    const double z = atan2(2 * (q.w * q.z + q.x * q.y), 1 - 2 * (q.y * q.y + q.z * q.z)) * deg;
    return {{"component_local",
             {{"origin", {rounded(pose.t[0], 1e4), rounded(pose.t[1], 1e4), rounded(pose.t[2], 1e4)}},
              {"rotate_xyz", {rounded(x, 1e3), rounded(y, 1e3), rounded(z, 1e3)}}}}};
}

}  // namespace rincast::controllers
