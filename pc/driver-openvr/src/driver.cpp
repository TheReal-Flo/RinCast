// RinCast SteamVR driver.
//
// Exposes the streamed headset to SteamVR as an HMD ("virtual display": SteamVR
// composites and hands us the final side-by-side backbuffer) plus two tracked
// controllers using SteamVR's native Pico or Oculus input profiles and models.
// Everything is exchanged
// with the RinCast Streamer through the shared-memory VR bridge.
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "controller_models.h"
#include "native_model_cache.h"
#include "hand_poses.h"
#include "nlohmann/json.hpp"
#include "openvr/openvr_driver.h"
#include "vr_bridge.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

namespace {

using namespace rincast::controllers;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (vr::VRDriverLog()) {
        std::string line = std::string("[RinCast] ") + buf + "\n";
        vr::VRDriverLog()->Log(line.c_str());
    }
}

// For what can go wrong on every frame: each reason at most once every 5 seconds, with how
// often it happened since it was last logged, so a stuck picture shows up in vrserver.txt
// without flooding it.
void LogEvery5s(const char* reason) {
    static std::mutex mutex;
    static std::map<const char*, std::pair<int64_t, uint32_t>> seen;  // last logged, count since
    std::lock_guard lock(mutex);
    auto& [last, count] = seen[reason];
    count++;
    const int64_t now = rcvr::QpcNowNs();
    if (last && now - last < 5'000'000'000LL) return;
    Log("%s (%u times)", reason, count);
    last = now;
    count = 0;
}

// Without a universe and room setup, SteamVR has no standing or play space and OpenXR games
// lose their stage space. The Quest keeps its own boundary, so this is a default 2 m square
// around the floor origin that is never drawn. SteamVR keeps any room setup already saved for
// this universe. The id only has to differ from other drivers', whose rooms these are not.
constexpr uint64_t kUniverseId = 38710;

std::string DefaultChaperoneJson() {
    return R"({"jsonid":"chaperone_info","version":5,"time":"2026-11-04T00:00:00.000Z","universes":[{)"
           R"("collision_bounds":[[[1,0,1],[1,2,1],[-1,2,1],[-1,0,1]],[[-1,0,1],[-1,2,1],[-1,2,-1],[-1,0,-1]],)"
           R"([[-1,0,-1],[-1,2,-1],[1,2,-1],[1,0,-1]],[[1,0,-1],[1,2,-1],[1,2,1],[1,0,1]]],)"
           R"("play_area":[2,2],"seated":{"translation":[0,0,0],"yaw":0},"standing":{"translation":[0,0,0],"yaw":0},)"
           R"("setup_standing2":{"translation":[0,0,0],"yaw":0},"universeID":)" +
           std::to_string(kUniverseId) + "}]}";
}

// ---------------------------------------------------------------------------
// Math helpers
// ---------------------------------------------------------------------------
vr::DriverPose_t MakePose(const rcvr::BridgePose& p, bool valid, bool connected, double timeOffset, bool withVelocity,
                          const Quat* localRot = nullptr, const double* localPos = nullptr) {
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation.w = 1;
    pose.qDriverFromHeadRotation.w = 1;
    Quat q{p.orientation[3], p.orientation[0], p.orientation[1], p.orientation[2]};
    double pos[3] = {p.position[0], p.position[1], p.position[2]};
    double offsetWorld[3]{};
    if (localPos) {
        Rotate(q, localPos, offsetWorld);
        pos[0] += offsetWorld[0];
        pos[1] += offsetWorld[1];
        pos[2] += offsetWorld[2];
    }
    if (localRot) q = Mul(q, *localRot);
    pose.qRotation = {q.w, q.x, q.y, q.z};
    const auto velocity = OffsetLinearVelocity(p, offsetWorld);
    for (int i = 0; i < 3; i++) {
        pose.vecPosition[i] = pos[i];
        pose.vecVelocity[i] = withVelocity ? velocity[i] : 0;
        pose.vecAngularVelocity[i] = withVelocity ? p.angularVelocity[i] : 0;
    }
    pose.poseTimeOffset = timeOffset;
    pose.poseIsValid = valid;
    pose.deviceIsConnected = connected;
    pose.result = valid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
    pose.willDriftInYaw = false;
    pose.shouldApplyHeadModel = false;
    return pose;
}

// ---------------------------------------------------------------------------
// Shared state between devices
// ---------------------------------------------------------------------------
struct Shared {
    rcvr::BridgeClient bridge;
    std::mutex mutex;
    rcvr::TrackingSnapshot frameTracking{};  // newest tracking handed to SteamVR
    // The last poses handed to SteamVR: a submitted frame says which head pose it was
    // rendered with, and is matched against these to find its tracking sample.
    std::deque<rcvr::TrackingSnapshot> history;
    bool haveTracking = false;
};
Shared* g_shared = nullptr;

// Normalized lerp along the shorter way: the bones only ever move a little per step, and this
// is called for 2 * 31 bones per controller per frame.
vr::VRBoneTransform_t BlendBones(const vr::VRBoneTransform_t& a, const vr::VRBoneTransform_t& b, float t) {
    vr::VRBoneTransform_t r{};
    for (int i = 0; i < 3; i++) r.position.v[i] = a.position.v[i] + (b.position.v[i] - a.position.v[i]) * t;
    r.position.v[3] = 1;
    const auto& p = a.orientation;
    const auto& q = b.orientation;
    const float sign = p.w * q.w + p.x * q.x + p.y * q.y + p.z * q.z < 0 ? -1.f : 1.f;
    const float w = p.w + (sign * q.w - p.w) * t, x = p.x + (sign * q.x - p.x) * t, y = p.y + (sign * q.y - p.y) * t,
                z = p.z + (sign * q.z - p.z) * t;
    const float n = std::sqrt(w * w + x * x + y * y + z * z);
    r.orientation = n > 0 ? vr::HmdQuaternionf_t{w / n, x / n, y / n, z / n} : p;
    return r;
}

// ---------------------------------------------------------------------------
// Local native model resources
// ---------------------------------------------------------------------------
std::optional<std::string> PrepareNativePicoModel(int hand, const std::string& sourceModel) {
    namespace fs = std::filesystem;
    rcvr::TrackingSnapshot tracking{};
    if (hand < 0 || hand > 1 || !g_shared || !g_shared->bridge.shared() || !rcvr::ReadTracking(g_shared->bridge.shared(), tracking) ||
        !tracking.controllers[hand].aimValid)
        return {};
    try {
        const auto sourceName = sourceModel.substr(sourceModel.find('}') + 1);
        const auto resource = "{vrlink}/rendermodels/" + sourceName + "/" + sourceName + ".json";
        char sourcePath[4096]{}, cachePath[4096]{};
        auto* resources = vr::VRResources();
        const auto sourceSize = resources->GetResourceFullPath(resource.c_str(), "", sourcePath, sizeof(sourcePath));
        const auto cacheSize = resources->GetResourceFullPath("{rincast_native}/rendermodels", "", cachePath, sizeof(cachePath));
        if (!sourceSize || sourceSize > sizeof(sourcePath) || !cacheSize || cacheSize > sizeof(cachePath) || !*sourcePath || !*cachePath) {
            LogEvery5s("Native Pico resources unavailable; register RinCast and restart SteamVR");
            return {};
        }
        const auto source = fs::path(std::u8string(reinterpret_cast<const char8_t*>(sourcePath)));
        const auto root = fs::path(std::u8string(reinterpret_cast<const char8_t*>(cachePath)));
        const auto name = CacheNativeModel(source, root, hand, tracking.controllers[hand]);
        Log("Native PICO model calibrated from headset grip and aim: %s", name.c_str());
        return "{rincast_native}" + name;
    } catch (const std::exception& error) {
        Log("Native PICO model preparation failed: %s", error.what());
        return {};
    }
}

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------
void ToBones(const rcvr::HandSnapshot& h, vr::VRBoneTransform_t* bones) {
    for (int i = 0; i < rcvr::kHandBones; i++) {
        const auto& b = h.bones[i];
        bones[i].position = {b.position[0], b.position[1], b.position[2], 1};
        bones[i].orientation = {b.orientation[3], b.orientation[0], b.orientation[1], b.orientation[2]};
    }
    bones[0] = {{0, 0, 0, 1}, {1, 0, 0, 0}};
}

class Controller : public vr::ITrackedDeviceServerDriver {
   public:
    Controller(int hand, std::string serial, ModelNames names)
        : m_hand(hand), m_serial(std::move(serial)), m_names(std::move(names)) {}

    bool PrepareResources() {
        std::lock_guard stateLock(m_stateMutex);
        return !m_names.pico || ApplyNativeModel();
    }

    vr::EVRInitError Activate(uint32_t id) override {
        std::lock_guard stateLock(m_stateMutex);
        m_id = vr::k_unTrackedDeviceIndexInvalid;
        if (m_names.pico && !ApplyNativeModel()) return vr::VRInitError_Driver_Failed;
        auto* props = vr::VRProperties();
        const auto c = props->TrackedDeviceToPropertyContainer(id);
        const bool left = m_hand == 0;
        ApplyIdentity(c);
        props->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32,
                                left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        props->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);
        props->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);

        auto* in = vr::VRDriverInput();
        auto boolean = [&](const char* path) {
            vr::VRInputComponentHandle_t h{};
            in->CreateBooleanComponent(c, path, &h);
            return h;
        };
        auto scalar = [&](const char* path, vr::EVRScalarUnits units) {
            vr::VRInputComponentHandle_t h{};
            in->CreateScalarComponent(c, path, &h, vr::VRScalarType_Absolute, units);
            return h;
        };
        m_system = boolean("/input/system/click");
        m_systemTouch = boolean("/input/system/touch");
        m_btn1 = boolean(left ? "/input/x/click" : "/input/a/click");
        m_btn1Touch = boolean(left ? "/input/x/touch" : "/input/a/touch");
        m_btn2 = boolean(left ? "/input/y/click" : "/input/b/click");
        m_btn2Touch = boolean(left ? "/input/y/touch" : "/input/b/touch");
        m_triggerClick = boolean("/input/trigger/click");
        m_triggerTouch = boolean("/input/trigger/touch");
        m_trigger = scalar("/input/trigger/value", vr::VRScalarUnits_NormalizedOneSided);
        m_gripClick = boolean("/input/grip/click");
        m_gripTouch = boolean("/input/grip/touch");
        m_grip = scalar("/input/grip/value", vr::VRScalarUnits_NormalizedOneSided);
        m_stickX = scalar("/input/joystick/x", vr::VRScalarUnits_NormalizedTwoSided);
        m_stickY = scalar("/input/joystick/y", vr::VRScalarUnits_NormalizedTwoSided);
        m_stickClick = boolean("/input/joystick/click");
        m_stickTouch = boolean("/input/joystick/touch");
        if (!m_names.pico) m_thumbrest = boolean("/input/thumbrest/touch");
        in->CreateHapticComponent(c, "/output/haptic", &m_haptic);
        if (m_names.pico) {
            CreateNativePoseComponents(c);
            UpdateAim(nullptr, nullptr);
        }
        // Estimated, as Touch controllers are: the fingers come from what the buttons feel.
        // VRChat reads Partial as Index controllers (fingers curl with the grip, no fist,
        // its Touch gestures off) and only uses a skeleton itself when it's Full, which the
        // tracked hands are, on devices of their own (HandDevice).
        in->CreateSkeletonComponent(c, left ? "/input/skeleton/left" : "/input/skeleton/right",
                                    left ? "/skeleton/hand/left" : "/skeleton/hand/right", "/pose/raw",
                                    vr::VRSkeletalTracking_Estimated, nullptr, 0, &m_skeleton);
        // Games that read the skeleton before the first pose would otherwise get a frozen hand.
        UpdateControllerSkeleton({});
        m_id = id;
        return vr::VRInitError_None;
    }

    void Deactivate() override {
        std::lock_guard stateLock(m_stateMutex);
        m_id = vr::k_unTrackedDeviceIndexInvalid;
        m_profileKey.clear();
    }
    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* response, uint32_t size) override {
        if (size) response[0] = 0;
    }
    vr::DriverPose_t GetPose() override {
        std::lock_guard stateLock(m_stateMutex);
        return m_lastPose;
    }

    uint32_t Id() const {
        std::lock_guard stateLock(m_stateMutex);
        return m_id;
    }
    vr::VRInputComponentHandle_t HapticHandle() const {
        std::lock_guard stateLock(m_stateMutex);
        return m_id == vr::k_unTrackedDeviceIndexInvalid ? 0 : m_haptic;
    }

    void SelectReportedProfile(const rcvr::DisplayConfig& config) {
        std::lock_guard stateLock(m_stateMutex);
        if (m_id == vr::k_unTrackedDeviceIndexInvalid) return;
        const std::string key = std::string(config.controllerProfiles[m_hand]) + (config.touchControllers ? ":touch" : ":native");
        if (key == m_profileKey) return;
        const auto names = ControllerNamesFor(config.headsetModel, config.controllerProfiles[m_hand], config.touchControllers != 0);
        m_profileKnown = names.has_value();
        if (!names) {
            m_profileKey = key;
            Log("Unsupported controller profile reported by headset: %s", config.controllerProfiles[m_hand]);
            return;
        }
        m_names = *names;
        if (m_names.pico && !ApplyNativeModel()) {
            m_profileKnown = false;
            return;
        }
        m_profileKey = key;
        if (m_id == vr::k_unTrackedDeviceIndexInvalid) return;
        auto* props = vr::VRProperties();
        const auto c = props->TrackedDeviceToPropertyContainer(m_id);
        ApplyIdentity(c);
        auto* input = vr::VRDriverInput();
        if (m_names.pico && !m_tipPose) {
            CreateNativePoseComponents(c);
        }
        if (!m_names.pico && !m_thumbrest) input->CreateBooleanComponent(c, "/input/thumbrest/touch", &m_thumbrest);
        if (m_tipPose) UpdateAim(nullptr, nullptr);
    }

    void Update(const rcvr::TrackingSnapshot& t, const rcvr::HandSnapshot* tracked, bool headsetConnected, double timeOffset) {
        std::lock_guard stateLock(m_stateMutex);
        if (m_id == vr::k_unTrackedDeviceIndexInvalid) return;
        const auto& s = t.controllers[m_hand];
        // While the hand is tracked instead, the hand's own device stands in (VRChat's advice
        // for streaming drivers: one device per kind of input, switched by connected/valid).
        const bool connected = headsetConnected && m_profileKnown && (s.flags & 1) && !(s.flags & 4);
        const bool valid = connected && (s.flags & 2);
        // OpenXR grip pose -> the selected SteamVR model's raw pose.
        const RigidPose l = GripToDevice(m_hand, m_names.pico);
        m_lastPose = MakePose(s.pose, valid, connected, timeOffset, true, &l.q, l.t);
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_id, m_lastPose, sizeof(m_lastPose));
        if (!connected) return;

        auto* in = vr::VRDriverInput();
        if (m_names.pico) UpdateAim(valid && s.aimValid ? &s : nullptr, &l);
        const auto boolean = [&](vr::VRInputComponentHandle_t handle, bool value, double offset) {
            if (handle) in->UpdateBooleanComponent(handle, value, offset);
        };
        const auto scalar = [&](vr::VRInputComponentHandle_t handle, float value, double offset) {
            if (handle) in->UpdateScalarComponent(handle, value, offset);
        };
        const uint32_t b = s.buttons;
        const bool left = m_hand == 0;
        auto bit = [&](uint32_t mask) { return (b & mask) != 0; };
        boolean(m_system, left && bit(1u << 4), 0);  // menu button on left = system
        boolean(m_systemTouch, left && bit(1u << 4), 0);
        boolean(m_btn1, bit(left ? 1u << 2 : 1u << 0), 0);
        boolean(m_btn1Touch, bit(left ? 1u << 18 : 1u << 16), 0);
        boolean(m_btn2, bit(left ? 1u << 3 : 1u << 1), 0);
        boolean(m_btn2Touch, bit(left ? 1u << 19 : 1u << 17), 0);
        scalar(m_trigger, s.trigger, 0);
        boolean(m_triggerClick, s.trigger > 0.85f, 0);
        boolean(m_triggerTouch, bit(1u << 21) || s.trigger > 0.05f, 0);
        scalar(m_grip, s.grip, 0);
        boolean(m_gripClick, s.grip > 0.85f, 0);
        boolean(m_gripTouch, s.grip > 0.1f, 0);
        scalar(m_stickX, s.thumbstick[0], 0);
        scalar(m_stickY, s.thumbstick[1], 0);
        boolean(m_stickClick, bit(1u << 6), 0);
        boolean(m_stickTouch, bit(1u << 20) || bit(1u << 6), 0);
        if (m_thumbrest) boolean(m_thumbrest, bit(1u << 22), 0);
        if (tracked) UpdateTrackedSkeleton(*tracked);
        else UpdateControllerSkeleton(s);
    }

   private:
    void ApplyIdentity(vr::PropertyContainerHandle_t c) {
        auto* props = vr::VRProperties();
        const bool left = m_hand == 0;
        props->SetStringProperty(c, vr::Prop_TrackingSystemName_String, m_names.pico ? "pico" : "oculus");
        props->SetStringProperty(c, vr::Prop_ManufacturerName_String, m_names.pico ? "PICO" : "Oculus");
        props->SetStringProperty(c, vr::Prop_ModelNumber_String,
                                 (m_names.hmd + (left ? " (Left Controller)" : " (Right Controller)")).c_str());
        props->SetStringProperty(c, vr::Prop_SerialNumber_String, m_serial.c_str());
        props->SetStringProperty(c, vr::Prop_RegisteredDeviceType_String, ((m_names.pico ? "pico/" : "oculus/") + m_serial).c_str());
        props->SetStringProperty(c, vr::Prop_RenderModelName_String, (left ? m_names.controllerLeft : m_names.controllerRight).c_str());
        props->SetStringProperty(c, vr::Prop_ControllerType_String, m_names.pico ? "pico_controller" : "oculus_touch");
        props->SetStringProperty(c, vr::Prop_InputProfilePath_String,
                                 m_names.pico ? "{rincast}/input/rincast_pico_profile.json" : "{oculus}/input/touch_profile.json");
        if (m_names.pico) {
            const std::string icon =
                std::string("{vrlink}/icons/") + (left ? "left_" : "right_") + (m_names.picoUltra ? "pico4u_" : "pico4_");
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceOff_String, (icon + "off.png").c_str());
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReady_String, (icon + "ready.png").c_str());
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearching_String, (icon + "searching.gif").c_str());
        } else {
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceOff_String,
                                     left ? "{oculus}/icons/cb_left_controller_off.png" : "{oculus}/icons/cb_right_controller_off.png");
            props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReady_String,
                                     left ? "{oculus}/icons/cb_left_controller_ready.png" : "{oculus}/icons/cb_right_controller_ready.png");
            props->SetStringProperty(
                c, vr::Prop_NamedIconPathDeviceSearching_String,
                left ? "{oculus}/icons/cb_left_controller_searching.gif" : "{oculus}/icons/cb_right_controller_searching.gif");
        }
    }

    bool ApplyNativeModel() {
        const auto source = m_hand == 0 ? m_names.controllerLeft : m_names.controllerRight;
        if (!m_nativeModel.empty() && source == m_nativeModel) return true;
        if (source != m_sourceModel) {
            const auto prepared = PrepareNativePicoModel(m_hand, source);
            if (!prepared) return false;
            m_sourceModel = source;
            m_nativeModel = *prepared;
        }
        m_names.controllerLeft = m_names.controllerRight = m_nativeModel;
        return true;
    }

    void CreateNativePoseComponents(vr::PropertyContainerHandle_t container) {
        auto* input = vr::VRDriverInput();
        input->CreatePoseComponent(container, "/pose/tip", &m_tipPose);
        input->CreatePoseComponent(container, "/pose/openxr_aim", &m_aimPose);
        input->CreatePoseComponent(container, "/pose/grip", &m_gripPose);
        input->CreatePoseComponent(container, "/pose/openxr_grip", &m_openxrGripPose);
        input->CreatePoseComponent(container, "/pose/handgrip", &m_handgripPose);
    }

    void Publish(const vr::VRBoneTransform_t* withController, const vr::VRBoneTransform_t* withoutController) {
        if (!m_skeleton) return;
        auto* in = vr::VRDriverInput();
        vr::VRBoneTransform_t nativeWith[rcvr::kHandBones], nativeWithout[rcvr::kHandBones];
        if (m_names.pico) {
            RebaseControllerBones(withController, nativeWith, m_hand);
            RebaseControllerBones(withoutController, nativeWithout, m_hand);
            withController = nativeWith;
            withoutController = nativeWithout;
        }
        in->UpdateSkeletonComponent(m_skeleton, vr::VRSkeletalMotionRange_WithController, withController, rcvr::kHandBones);
        in->UpdateSkeletonComponent(m_skeleton, vr::VRSkeletalMotionRange_WithoutController, withoutController, rcvr::kHandBones);
    }

    // Real fingers around the controller ("Finger tracking with controllers"), for games that
    // read the skeleton themselves.
    void UpdateTrackedSkeleton(const rcvr::HandSnapshot& h) {
        vr::VRBoneTransform_t bones[rcvr::kHandBones];
        ToBones(h, bones);
        Publish(bones, bones);
    }

    // The fingers the controller can feel: thumb on a surface, index on the trigger, the rest on the grip.
    void UpdateControllerSkeleton(const rcvr::BridgeController& s) {
        constexpr uint32_t kThumb = (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19) | (1u << 20) | (1u << 22);
        const bool thumb = (s.buttons & kThumb) != 0, index = (s.buttons & (1u << 21)) != 0 || s.trigger > 0;
        auto advance = [](bool& last, bool current, float& progress) {
            progress = last == current ? 0 : progress + 1.f / 15;
            if (progress > 1) {
                progress = 0;
                last = current;
            }
        };
        advance(m_lastThumb, thumb, m_thumbProgress);
        advance(m_lastIndex, index, m_indexProgress);
        vr::VRBoneTransform_t with[rcvr::kHandBones], without[rcvr::kHandBones];
        HeldPose(true, s, thumb, index, with);
        HeldPose(false, s, thumb, index, without);
        Publish(with, without);
    }

    void HeldPose(bool withController, const rcvr::BridgeController& s, bool thumb, bool index, vr::VRBoneTransform_t* out) const {
        const bool left = m_hand == 0;
        vr::VRBoneTransform_t a[rcvr::kHandBones], b[rcvr::kHandBones];
        out[0] = {{0, 0, 0, 1}, {1, 0, 0, 0}};
        out[1] = left ? vr::VRBoneTransform_t{{-0.034038f, 0.036503f, 0.164722f, 1}, {-0.055147f, -0.078608f, -0.920279f, 0.379296f}}
                      : vr::VRBoneTransform_t{{0.034038f, 0.036503f, 0.164722f, 1}, {-0.055147f, -0.078608f, 0.920279f, -0.379296f}};
        hand_poses::GetThumbBoneTransform(withController, left, m_lastThumb, a);
        hand_poses::GetThumbBoneTransform(withController, left, thumb, b);
        for (int i = 2; i < 6; i++) out[i] = BlendBones(a[i], b[i], m_thumbProgress);
        if (s.trigger > 0) {
            hand_poses::GetTriggerBoneTransform(withController, left, true, 0, a);
            hand_poses::GetTriggerBoneTransform(withController, left, true, 1, b);
            for (int i = 6; i < rcvr::kHandBones; i++) out[i] = BlendBones(a[i], b[i], s.trigger);
        } else {
            hand_poses::GetTriggerBoneTransform(withController, left, m_lastIndex, 0, a);
            hand_poses::GetTriggerBoneTransform(withController, left, index, 0, b);
            for (int i = 6; i < rcvr::kHandBones; i++) out[i] = BlendBones(a[i], b[i], m_indexProgress);
        }
        if (s.grip > 0) {
            hand_poses::GetGripClickBoneTransform(withController, left, b);
            for (int i = 11; i < 26; i++) out[i] = BlendBones(out[i], b[i], s.grip);
            for (int i = 28; i < rcvr::kHandBones; i++) out[i] = BlendBones(out[i], b[i], s.grip);
        }
    }

    void UpdateAim(const rcvr::BridgeController* controller, const RigidPose* deviceInGrip) {
        RigidPose tip, aim;
        if (controller && deviceInGrip) {
            tip = aim = AimInDevice(controller->pose, controller->aim, *deviceInGrip);
            if (!m_receivedAim) {
                Log("Using headset OpenXR aim pose for native PICO %s controller", m_hand == 0 ? "left" : "right");
                m_receivedAim = true;
            }
        } else {
            // Old headset builds and lost aim tracking retain the stock model offsets.
            tip.q = FromEuler(m_names.picoUltra ? -30.382 : -37.4, m_names.picoUltra ? (m_hand == 0 ? -8 : 8) : 0, 0);
            tip.t[0] = m_names.picoUltra ? 0 : (m_hand == 0 ? 0.016694 : -0.016694);
            tip.t[1] = m_names.picoUltra ? -0.04 : -0.02522;
            tip.t[2] = m_names.picoUltra ? 0.02 : 0.024687;
            aim = tip;
            if (!m_names.picoUltra) {
                aim.q = FromEuler(-39.4, 0, 0);
                aim.t[0] = m_hand == 0 ? 0.007 : -0.007;
                aim.t[1] = -0.03894766;
                aim.t[2] = 0.00949694;
            }
        }
        const auto tipMatrix = PoseMatrix(tip), aimMatrix = PoseMatrix(aim);
        const auto gripMatrix = PoseMatrix(Invert(GripToDevice(m_hand, m_names.pico)));
        auto* input = vr::VRDriverInput();
        if (m_tipPose) input->UpdatePoseComponent(m_tipPose, &tipMatrix, 0);
        if (m_aimPose) input->UpdatePoseComponent(m_aimPose, &aimMatrix, 0);
        for (const auto component : {m_gripPose, m_openxrGripPose, m_handgripPose})
            if (component) input->UpdatePoseComponent(component, &gripMatrix, 0);
    }

    mutable std::mutex m_stateMutex;
    int m_hand;
    std::string m_serial;
    ModelNames m_names;
    uint32_t m_id = vr::k_unTrackedDeviceIndexInvalid;
    vr::DriverPose_t m_lastPose{};
    vr::VRInputComponentHandle_t m_system{}, m_systemTouch{}, m_btn1{}, m_btn1Touch{}, m_btn2{}, m_btn2Touch{}, m_triggerClick{},
        m_triggerTouch{}, m_trigger{}, m_gripClick{}, m_gripTouch{}, m_grip{}, m_stickX{}, m_stickY{}, m_stickClick{}, m_stickTouch{},
        m_thumbrest{}, m_haptic{}, m_skeleton{}, m_tipPose{}, m_aimPose{}, m_gripPose{}, m_openxrGripPose{}, m_handgripPose{};
    bool m_lastThumb = false, m_lastIndex = false;
    bool m_receivedAim = false;
    bool m_profileKnown = true;
    std::string m_profileKey;
    std::string m_sourceModel, m_nativeModel;
    float m_thumbProgress = 0, m_indexProgress = 0;
};

// A hand the headset tracks without a controller, as its own device: VRChat (and SteamVR's
// own guidance for streaming drivers) wants one device per kind of input. Its fingers are
// measured, so the skeleton is Full: VRChat then animates them as they are, makes its gestures
// from them (a fist is a fist) and does its own pinches. Pinch and fist are also a trigger and
// a grip, which games made for Touch controllers get through resources/input's remapping.
// Connected only while the hand is the input; the controller is the rest of the time.
class HandDevice : public vr::ITrackedDeviceServerDriver {
   public:
    HandDevice(int hand, std::string serial) : m_hand(hand), m_serial(std::move(serial)) {}

    vr::EVRInitError Activate(uint32_t id) override {
        std::lock_guard stateLock(m_stateMutex);
        m_id = id;
        auto* props = vr::VRProperties();
        const auto c = props->TrackedDeviceToPropertyContainer(id);
        const bool left = m_hand == 0;
        // The controllers' tracking system: the same space, for Space Calibrator too.
        props->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "oculus");
        props->SetStringProperty(c, vr::Prop_ManufacturerName_String, "RinCast");
        props->SetStringProperty(c, vr::Prop_ModelNumber_String, left ? "RinCast Hand (Left)" : "RinCast Hand (Right)");
        props->SetStringProperty(c, vr::Prop_SerialNumber_String, m_serial.c_str());
        props->SetStringProperty(c, vr::Prop_RegisteredDeviceType_String, ("rincast/" + m_serial).c_str());
        props->SetStringProperty(c, vr::Prop_ControllerType_String, "rincast_hand");
        props->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{rincast}/input/rincast_hand_profile.json");
        props->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32,
                                left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        props->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);
        props->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
        // Below the controller, for when both happen to be connected at once.
        props->SetInt32Property(c, vr::Prop_ControllerHandSelectionPriority_Int32, -1);

        auto* in = vr::VRDriverInput();
        in->CreateBooleanComponent(c, "/input/system/click", &m_system);
        in->CreateBooleanComponent(c, "/input/trigger/click", &m_triggerClick);
        in->CreateScalarComponent(c, "/input/trigger/value", &m_trigger, vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
        in->CreateBooleanComponent(c, "/input/grip/click", &m_gripClick);
        in->CreateScalarComponent(c, "/input/grip/value", &m_grip, vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided);
        in->CreateHapticComponent(c, "/output/haptic", &m_haptic);
        in->CreateSkeletonComponent(c, left ? "/input/skeleton/left" : "/input/skeleton/right",
                                    left ? "/skeleton/hand/left" : "/skeleton/hand/right", "/pose/raw",
                                    vr::VRSkeletalTracking_Full, nullptr, 0, &m_skeleton);
        rcvr::BridgePose none{};
        none.orientation[3] = 1;
        m_lastPose = MakePose(none, false, false, 0, false);
        return vr::VRInitError_None;
    }

    void Deactivate() override {
        std::lock_guard stateLock(m_stateMutex);
        m_id = vr::k_unTrackedDeviceIndexInvalid;
    }
    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* response, uint32_t size) override {
        if (size) response[0] = 0;
    }
    vr::DriverPose_t GetPose() override {
        std::lock_guard stateLock(m_stateMutex);
        return m_lastPose;
    }

    void Update(const rcvr::TrackingSnapshot& t, const rcvr::HandSnapshot* bones, bool headsetConnected, double timeOffset) {
        std::lock_guard stateLock(m_stateMutex);
        if (m_id == vr::k_unTrackedDeviceIndexInvalid) return;
        const auto& s = t.controllers[m_hand];
        const bool connected = headsetConnected && (s.flags & 1) && (s.flags & 4);
        const bool valid = connected && (s.flags & 2);
        // The pose is already where a controller in that hand would be: see rc::HandDevicePose.
        m_lastPose = MakePose(s.pose, valid, connected, timeOffset, false);
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_id, m_lastPose, sizeof(m_lastPose));
        if (!connected) return;
        auto* in = vr::VRDriverInput();
        in->UpdateBooleanComponent(m_system, m_hand == 0 && (s.buttons & (1u << 4)), 0);
        in->UpdateScalarComponent(m_trigger, s.trigger, 0);
        in->UpdateBooleanComponent(m_triggerClick, s.trigger > 0.85f, 0);
        in->UpdateScalarComponent(m_grip, s.grip, 0);
        in->UpdateBooleanComponent(m_gripClick, s.grip > 0.85f, 0);
        if (bones) {
            vr::VRBoneTransform_t b[rcvr::kHandBones];
            ToBones(*bones, b);
            in->UpdateSkeletonComponent(m_skeleton, vr::VRSkeletalMotionRange_WithController, b, rcvr::kHandBones);
            in->UpdateSkeletonComponent(m_skeleton, vr::VRSkeletalMotionRange_WithoutController, b, rcvr::kHandBones);
        }
    }

   private:
    mutable std::mutex m_stateMutex;
    int m_hand;
    std::string m_serial;
    uint32_t m_id = vr::k_unTrackedDeviceIndexInvalid;
    vr::DriverPose_t m_lastPose{};
    vr::VRInputComponentHandle_t m_system{}, m_triggerClick{}, m_trigger{}, m_gripClick{}, m_grip{}, m_haptic{}, m_skeleton{};
};

// ---------------------------------------------------------------------------
// HMD (direct mode)
// ---------------------------------------------------------------------------
static const char* kBlitShader = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
cbuffer Layer : register(b0) { float4 rect; };  // uMin, vMin, uMax, vMax of the source
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    float2 c = float2((id << 1) & 2, id & 2);
    V o; o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = lerp(rect.xy, rect.zw, c); return o;
}
// SteamVR usually renders larger than the stream (its resolution setting, 150% on a fast GPU).
// One bilinear sample per pixel would then skip most of the game's pixels: fine detail shimmers
// and edges crawl. Shrinking, each pixel averages the ones it covers instead, from a 4x4 grid
// of bilinear samples spread over them.
float4 ps(V i) : SV_Target {
    float w, h;
    tex.GetDimensions(w, h);
    const float2 covered = float2(abs(ddx(i.uv.x)) * w, abs(ddy(i.uv.y)) * h);  // source pixels per pixel
    if (covered.x <= 1.05 && covered.y <= 1.05) return tex.Sample(samp, i.uv);
    const float2 step = max(covered, 1.0) / (4.0 * float2(w, h));
    float4 sum = 0;
    [unroll] for (int y = 0; y < 4; y++)
        [unroll] for (int x = 0; x < 4; x++)
            sum += tex.SampleLevel(samp, i.uv + (float2(x, y) - 1.5) * step, 0);
    return sum / 16;
}
)";

class Hmd : public vr::ITrackedDeviceServerDriver, public vr::IVRDisplayComponent,
            public vr::IVRDriverDirectModeComponent {
   public:
    explicit Hmd(const rcvr::DisplayConfig& cfg) : m_cfg(cfg) {
        m_serial = cfg.headsetSerial[0] ? std::string("RINCAST-") + std::string(cfg.headsetSerial).substr(0, 12)
                                        : "RINCAST-HMD";
        m_touchCompatibility = cfg.touchControllers != 0;
        m_vsyncPeriodNs = int64_t(1e9 / (cfg.refreshRate > 1 ? cfg.refreshRate : 90.f));
        m_vsyncEpochNs = rcvr::QpcNowNs();
    }

    const std::string& Serial() const { return m_serial; }
    bool TouchCompatibility() const { return m_touchCompatibility; }
    bool Ready() const { return m_ready.load(); }
    uint32_t Id() const { return m_id; }

    vr::EVRInitError Activate(uint32_t id) override {
        m_id = id;
        auto* props = vr::VRProperties();
        const auto c = props->TrackedDeviceToPropertyContainer(id);
        const bool nativePico = NamesFor(m_cfg.headsetModel).pico && !m_cfg.touchControllers;
        props->SetStringProperty(c, vr::Prop_TrackingSystemName_String, nativePico ? "pico" : "oculus");
        props->SetStringProperty(c, vr::Prop_ManufacturerName_String, nativePico ? "PICO" : "Oculus");
        props->SetStringProperty(c, vr::Prop_ModelNumber_String, NamesFor(m_cfg.headsetModel).hmd.c_str());
        props->SetStringProperty(c, vr::Prop_SerialNumber_String, m_serial.c_str());
        props->SetStringProperty(c, vr::Prop_RegisteredDeviceType_String, ((nativePico ? "pico/" : "oculus/") + m_serial).c_str());
        props->SetStringProperty(c, vr::Prop_RenderModelName_String, "generic_hmd");
        props->SetStringProperty(c, vr::Prop_DriverVersion_String, "RinCast 1.0");
        props->SetFloatProperty(c, vr::Prop_DisplayFrequency_Float, m_cfg.refreshRate);
        props->SetFloatProperty(c, vr::Prop_UserIpdMeters_Float, m_cfg.ipd);
        props->SetFloatProperty(c, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
        props->SetFloatProperty(c, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.f);
        props->SetFloatProperty(c, vr::Prop_SecondsFromPhotonsToVblank_Float, 0.f);
        uint64_t luid = (uint64_t(uint32_t(m_cfg.adapterLuid.HighPart)) << 32) | m_cfg.adapterLuid.LowPart;
        props->SetUint64Property(c, vr::Prop_GraphicsAdapterLuid_Uint64, luid);
        props->SetUint64Property(c, vr::Prop_CurrentUniverseId_Uint64, kUniverseId);
        props->SetStringProperty(c, vr::Prop_DriverProvidedChaperoneJson_String, DefaultChaperoneJson().c_str());
        props->SetBoolProperty(c, vr::Prop_DriverProvidedChaperoneVisibility_Bool, false);
        props->SetBoolProperty(c, vr::Prop_IsOnDesktop_Bool, false);
        props->SetBoolProperty(c, vr::Prop_DisplayDebugMode_Bool, false);
        props->SetBoolProperty(c, vr::Prop_HasDisplayComponent_Bool, true);
        props->SetBoolProperty(c, vr::Prop_HasVirtualDisplayComponent_Bool, false);
        props->SetBoolProperty(c, vr::Prop_HasCameraComponent_Bool, false);
        props->SetBoolProperty(c, vr::Prop_HasDriverDirectModeComponent_Bool, true);
        props->SetBoolProperty(c, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, true);
        props->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
        props->SetBoolProperty(c, vr::Prop_ContainsProximitySensor_Bool, false);
        props->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_HMD);
        props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceOff_String, "{oculus}/icons/quest_headset_off.png");
        props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReady_String, "{oculus}/icons/quest_headset_ready.png");
        props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearching_String,
                                 "{oculus}/icons/quest_headset_searching.gif");
        props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceStandby_String, "{oculus}/icons/quest_headset_standby.png");
        props->SetBoolProperty(c, vr::Prop_DoNotApplyPrediction_Bool, false);
        InitGraphics();
        m_ready = true;
        return vr::VRInitError_None;
    }

    void Deactivate() override {
        m_ready = false;
        m_id = vr::k_unTrackedDeviceIndexInvalid;
    }
    void EnterStandby() override {}
    void* GetComponent(const char* name) override {
        if (!strcmp(name, vr::IVRDisplayComponent_Version)) return static_cast<vr::IVRDisplayComponent*>(this);
        if (!strcmp(name, vr::IVRDriverDirectModeComponent_Version))
            return static_cast<vr::IVRDriverDirectModeComponent*>(this);
        return nullptr;
    }
    void DebugRequest(const char*, char* response, uint32_t size) override {
        if (size) response[0] = 0;
    }
    vr::DriverPose_t GetPose() override {
        std::lock_guard poseLock(m_poseMutex);
        return m_lastPose;
    }

    // ---- IVRDisplayComponent ----
    // What SteamVR renders an eye at before its own resolution setting. Usually the stream's
    // size; matching SteamVR's resolution, the headset's own and the stream follows SteamVR.
    uint32_t RenderWidth() const { return m_cfg.renderEyeWidth ? m_cfg.renderEyeWidth : m_cfg.eyeWidth; }
    uint32_t RenderHeight() const { return m_cfg.renderEyeHeight ? m_cfg.renderEyeHeight : m_cfg.eyeHeight; }
    void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* w, uint32_t* h) override {
        *x = 0;
        *y = 0;
        *w = RenderWidth() * 2;
        *h = RenderHeight();
    }
    bool IsDisplayOnDesktop() override { return false; }
    bool IsDisplayRealDisplay() override { return false; }
    void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) override {
        *w = RenderWidth();
        *h = RenderHeight();
    }
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h) override {
        *x = eye == vr::Eye_Left ? 0 : RenderWidth();
        *y = 0;
        *w = RenderWidth();
        *h = RenderHeight();
    }
    void GetProjectionRaw(vr::EVREye eye, float* left, float* right, float* top, float* bottom) override {
        // SteamVR's vertical convention is flipped: "top" takes the (negated) DOWN
        // tangent and "bottom" the UP tangent (same as ALVR / OpenComposite). Swapping
        // them renders with the vertical field of view upside down, which for the
        // Quest 3's asymmetric FOV (up 0.97, down 1.43) shifts the horizon ~25 degrees.
        std::lock_guard lock(m_cfgMutex);
        const vr::HmdRect2_t r = ProjectionRect(m_cfg.fovTan[eye == vr::Eye_Left ? 0 : 1]);
        *left = r.vTopLeft.v[0];
        *right = r.vBottomRight.v[0];
        *top = r.vTopLeft.v[1];
        *bottom = r.vBottomRight.v[1];
    }
    static vr::HmdRect2_t ProjectionRect(const float f[4]) {
        vr::HmdRect2_t r{};
        r.vTopLeft.v[0] = -f[0];
        r.vBottomRight.v[0] = f[1];
        r.vTopLeft.v[1] = -f[3];
        r.vBottomRight.v[1] = f[2];
        return r;
    }

    // The streamer reconfigured the bridge (new headset, refresh rate or FOV). SteamVR
    // keeps its render resolution until restarted, but refresh rate, field of view and
    // IPD can change live, so a different refresh rate never leaves SteamVR pacing at
    // the old rate and the frames are always rendered with the FOV they are tagged with.
    void ApplyLiveConfig(const rcvr::DisplayConfig& c) {
        if (m_id == vr::k_unTrackedDeviceIndexInvalid) return;
        rcvr::DisplayConfig old;
        {
            std::lock_guard lock(m_cfgMutex);
            old = m_cfg;
            std::memcpy(m_cfg.fovTan, c.fovTan, sizeof(m_cfg.fovTan));
            m_cfg.ipd = c.ipd;
            m_cfg.refreshRate = c.refreshRate;
        }
        auto* props = vr::VRProperties();
        const auto container = props->TrackedDeviceToPropertyContainer(m_id);
        if (c.refreshRate > 1 && c.refreshRate != old.refreshRate) {
            m_vsyncPeriodNs = int64_t(1e9 / c.refreshRate);
            props->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, c.refreshRate);
        }
        if (std::memcmp(c.fovTan, old.fovTan, sizeof(c.fovTan)) != 0)
            vr::VRServerDriverHost()->SetDisplayProjectionRaw(m_id, ProjectionRect(c.fovTan[0]), ProjectionRect(c.fovTan[1]));
        if (c.ipd > 0.04f && c.ipd != old.ipd) {
            props->SetFloatProperty(container, vr::Prop_UserIpdMeters_Float, c.ipd);
            vr::HmdMatrix34_t l{}, r{};
            for (int i = 0; i < 3; i++) l.m[i][i] = r.m[i][i] = 1.f;
            l.m[0][3] = -c.ipd / 2;
            r.m[0][3] = c.ipd / 2;
            vr::VRServerDriverHost()->SetDisplayEyeToHead(m_id, l, r);
        }
    }
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye, float u, float v) override {
        // The headset's own compositor handles lens distortion.
        vr::DistortionCoordinates_t d{};
        d.rfRed[0] = d.rfGreen[0] = d.rfBlue[0] = u;
        d.rfRed[1] = d.rfGreen[1] = d.rfBlue[1] = v;
        return d;
    }
    bool ComputeInverseDistortion(vr::HmdVector2_t* result, vr::EVREye, uint32_t, float u, float v) override {
        result->v[0] = u;
        result->v[1] = v;
        return true;
    }

    // ---- IVRDriverDirectModeComponent ----
    // SteamVR renders every layer (the game's eyes, overlays such as the dashboard) into
    // textures handed out here and submits them together with the head pose each was
    // rendered with. That pose tags the frame for the headset, which reprojects it to
    // where the head is when it is shown; a frame tagged with any other pose (the
    // virtual display interface this replaces doesn't say which one was used, and was
    // one sample off on most frames) lands in the wrong place: jitter while turning.
    void CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t* desc, SwapTextureSet_t* out) override {
        std::memset(out, 0, sizeof(*out));
        if (!m_device) return;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = desc->nWidth;
        td.Height = desc->nHeight;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT(desc->nFormat);
        td.SampleDesc.Count = desc->nSampleCount ? desc->nSampleCount : 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        auto set = std::make_shared<SwapSet>();
        set->pid = pid;
        for (int i = 0; i < 3; i++) {
            ComPtr<IDXGIResource> res;
            HANDLE h = nullptr;
            if (FAILED(m_device->CreateTexture2D(&td, nullptr, &set->tex[i])) || FAILED(set->tex[i].As(&res)) ||
                FAILED(res->GetSharedHandle(&h))) {
                Log("CreateSwapTextureSet %ux%u format %u failed", td.Width, td.Height, desc->nFormat);
                std::memset(out, 0, sizeof(*out));
                return;
            }
            set->handles[i] = vr::SharedTextureHandle_t(h);
            out->rSharedTextureHandles[i] = set->handles[i];
        }
        std::lock_guard lock(m_layerMutex);
        for (auto h : set->handles) m_swapSets[h] = set;
        Log("SteamVR created frame textures %ux%u, format %u, %u samples, for process %u", td.Width, td.Height,
            desc->nFormat, td.SampleDesc.Count, pid);
    }
    void DestroySwapTextureSet(vr::SharedTextureHandle_t handle) override {
        std::lock_guard lock(m_layerMutex);
        auto it = m_swapSets.find(handle);
        if (it == m_swapSets.end()) return;
        const auto set = it->second;
        for (auto h : set->handles) m_swapSets.erase(h);
    }
    void DestroyAllSwapTextureSets(uint32_t pid) override {
        std::lock_guard lock(m_layerMutex);
        for (auto it = m_swapSets.begin(); it != m_swapSets.end();)
            it = it->second->pid == pid ? m_swapSets.erase(it) : std::next(it);
    }
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) override {
        std::lock_guard lock(m_layerMutex);
        for (int e = 0; e < 2; e++) {
            auto it = m_swapSets.find(handles[e]);
            if (it == m_swapSets.end()) {
                (*indices)[e] = 0;
            } else if (e == 1 && handles[1] == handles[0]) {
                (*indices)[1] = (*indices)[0];  // both eyes in one set
            } else {
                it->second->next = (it->second->next + 1) % 3;
                (*indices)[e] = it->second->next;
            }
        }
    }
    void SubmitLayer(const SubmitLayerPerEye_t (&perEye)[2]) override {
        m_layersSubmitted++;
        std::lock_guard lock(m_layerMutex);
        if (m_layers.size() < 8) m_layers.push_back({perEye[0], perEye[1]});
    }

    void Present(vr::SharedTextureHandle_t syncTexture) override {
        m_presents++;
        std::vector<std::array<SubmitLayerPerEye_t, 2>> layers;
        std::vector<std::array<ComPtr<ID3D11Texture2D>, 2>> textures;
        {
            std::lock_guard lock(m_layerMutex);
            layers.swap(m_layers);
            for (const auto& l : layers) {
                textures.push_back({FindSwapTexture(l[0].hTexture), FindSwapTexture(l[1].hTexture)});
            }
        }
        if (!m_device) return LogEvery5s("Frame dropped: no graphics device (see the error when the headset was added)");
        if (layers.empty()) return LogEvery5s("Frame dropped: SteamVR presented a frame without layers");
        if (!g_shared->bridge.shared()) return LogEvery5s("Frame dropped: not connected to RinCast Streamer");
        for (const auto& t : textures)
            if (!t[0] || !t[1]) LogEvery5s("A layer's texture isn't one SteamVR created through this driver; that layer is left out");
        rcvr::Shared* s = g_shared->bridge.shared();
        // SteamVR's rendering into the layers is done once we hold the sync texture.
        ComPtr<ID3D11Texture2D> sync = OpenBackbuffer(syncTexture);
        ComPtr<IDXGIKeyedMutex> syncMutex;
        if (sync) sync.As(&syncMutex);
        if (syncMutex && !rcvr::AcquireKeyed(syncMutex.Get(), 100))
            return LogEvery5s("Frame dropped: SteamVR kept its frame locked for over 100 ms");
        const int slot = g_shared->bridge.AcquireSlot();
        ID3D11Texture2D* dst = slot >= 0 ? SlotTexture(slot) : nullptr;
        ComPtr<IDXGIKeyedMutex> dstMutex;
        if (dst) ComPtr<ID3D11Texture2D>(dst).As(&dstMutex);
        if (!dst || (dstMutex && !rcvr::AcquireKeyed(dstMutex.Get(), 20))) {
            LogEvery5s(slot < 0   ? "Frame dropped: no free frame slot, RinCast Streamer isn't taking frames"
                       : !dst     ? "Frame dropped: couldn't open RinCast Streamer's texture"
                                  : "Frame dropped: RinCast Streamer kept its texture locked for over 20 ms");
            if (slot >= 0) g_shared->bridge.AbandonSlot(slot);
            if (syncMutex) syncMutex->ReleaseSync(0);
            return;
        }
        D3D11_TEXTURE2D_DESC dd;
        dst->GetDesc(&dd);
        for (int eye = 0; eye < 2; eye++) {
            const D3D11_VIEWPORT vp{float(eye * dd.Width / 2), 0, float(dd.Width / 2), float(dd.Height), 0, 1};
            for (size_t i = 0; i < layers.size(); i++)
                if (textures[i][eye]) DrawLayer(textures[i][eye].Get(), layers[i][eye].bounds, dst, vp, i > 0);
        }
        if (dstMutex) dstMutex->ReleaseSync(0);
        m_ctx->Flush();
        if (syncMutex) syncMutex->ReleaseSync(0);

        // The size the game's eye images come in, for streaming at SteamVR's resolution.
        if (textures[0][0]) {
            D3D11_TEXTURE2D_DESC sd;
            textures[0][0]->GetDesc(&sd);
            const vr::VRTextureBounds_t& b = layers[0][0].bounds;
            s->renderedEyeWidth = LONG(sd.Width * std::fabs(b.uMax - b.uMin) + 0.5f);
            s->renderedEyeHeight = LONG(sd.Height * std::fabs(b.vMax - b.vMin) + 0.5f);
        }
        const rcvr::TrackingSnapshot t = RenderedWith(layers[0][0].mHmdPose);
        rcvr::FrameSlot& fs = s->slots[slot];
        fs.frameIndex = ++m_frameIndex;
        fs.trackingSequence = t.sequence;
        fs.headsetTargetNs = t.headsetTargetNs;
        fs.submitPcNs = rcvr::QpcNowNs();
        fs.renderPose = t.hmd;
        {
            std::lock_guard lock(m_cfgMutex);
            std::memcpy(fs.fovTan, m_cfg.fovTan, sizeof(fs.fovTan));
        }
        s->producerKind = LONG(rcvr::ProducerKind::SteamVR);
        s->producerPid = LONG(GetCurrentProcessId());
        InterlockedExchange64(&s->producerHeartbeatNs, rcvr::QpcNowNs());
        g_shared->bridge.PublishSlot(slot);
        m_heldSlot = slot;
        m_heldFrame = fs.frameIndex;
        if (!m_framesPublished++) Log("First frame handed to RinCast Streamer (%ux%u)", dd.Width, dd.Height);
        m_lastPublishNs = rcvr::QpcNowNs();
    }

    // For the tracking thread's report when no picture is getting through.
    struct FrameCounts {
        uint32_t presents, layers, published;
        size_t textureSets;
        int64_t lastPublishNs;
    };
    FrameCounts Counts() {
        std::lock_guard lock(m_layerMutex);
        return {m_presents, m_layersSubmitted, m_framesPublished, m_swapSets.size() / 3, m_lastPublishNs};
    }

    // Paces SteamVR to the headset's refresh rate: block until the next (virtual) vsync. And
    // when the encoder can't keep up with that rate, to the encoder: a frame the streamer
    // hasn't taken yet would only be replaced, and rendering it takes GPU time the encoding
    // needs. Held that way, a frame that is late already goes on without waiting for a vsync.
    void PostPresent(const Throttling_t*) override {
        const int64_t now = rcvr::QpcNowNs();
        const int64_t next = NextVsync(now, m_pacedVsync);
        m_pacedVsync = next;
        if (WaitForStreamer() && rcvr::QpcNowNs() >= next) return;
        const int64_t coarse = next - 1'500'000;
        if (coarse > now) Sleep(DWORD((coarse - now) / 1'000'000));
        while (rcvr::QpcNowNs() < next) YieldProcessor();
    }
    // Until the streamer has taken the frame just presented; false if it didn't need waiting
    // for. A streamer that doesn't take frames (stuck, or not streaming) stops the holding for
    // a few seconds, so SteamVR never stalls on it.
    bool WaitForStreamer() {
        const int slot = std::exchange(m_heldSlot, -1);
        auto& bridge = g_shared->bridge;
        if (slot < 0 || !bridge.Streaming() || rcvr::QpcNowNs() < m_holdOffUntilNs) return false;
        if (!bridge.Untaken(slot, m_heldFrame)) return false;
        if (!bridge.WaitTaken(slot, m_heldFrame, std::max<int64_t>(3 * m_vsyncPeriodNs, 40'000'000))) {
            m_holdOffUntilNs = rcvr::QpcNowNs() + 3'000'000'000LL;
            LogEvery5s("RinCast Streamer didn't take a frame for several refreshes; not holding SteamVR for it for 3 s");
        }
        return true;
    }

    void GetFrameTiming(vr::DriverDirectMode_FrameTiming* timing) override { timing->m_nReprojectionFlags = 0; }

    // The first vsync after now, and at least half a period after `last`, the one used before:
    // the streamer moves the epoch a little at a time, and moved later, the vsync just passed
    // comes up again (measured: 160-190 vsync events a second at 120 Hz).
    int64_t NextVsync(int64_t now, int64_t last = 0) const {
        const int64_t period = m_vsyncPeriodNs;
        const int64_t epoch = VsyncEpochNs();
        int64_t next = epoch + ((now - epoch) / period + 1) * period;
        if (last && next - last < period / 2) next += period;
        return next;
    }

    int64_t VsyncPeriodNs() const { return m_vsyncPeriodNs; }
    // The streamer moves the vsync so frames reach the headset just before it shows them;
    // without a streamer, the driver keeps time on its own.
    int64_t VsyncEpochNs() const {
        const rcvr::Shared* s = g_shared->bridge.shared();
        if (s && s->vsyncEpochNs && g_shared->bridge.StreamerAlive()) return s->vsyncEpochNs;
        return m_vsyncEpochNs;
    }

    void Update(const rcvr::TrackingSnapshot& t, bool headsetConnected, double timeOffset) {
        std::lock_guard poseLock(m_poseMutex);
        if (m_id == vr::k_unTrackedDeviceIndexInvalid) return;
        const bool valid = headsetConnected && (t.flags & 1);
        // The headset already predicted this pose for the moment the frame is shown, and
        // tags each frame with it. SteamVR must render exactly this pose: with velocities
        // it would extrapolate a second time, and the headset would place the image for
        // a different pose than it was rendered with (swimming / zooming while moving).
        (void)timeOffset;
        m_lastPose = MakePose(t.hmd, valid, true, 0.0, false);
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_id, m_lastPose, sizeof(m_lastPose));
    }

   private:
    void InitGraphics() {
        ComPtr<IDXGIFactory1> factory;
        ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
                DXGI_ADAPTER_DESC1 d;
                adapter->GetDesc1(&d);
                if (d.AdapterLuid.LowPart == m_cfg.adapterLuid.LowPart && d.AdapterLuid.HighPart == m_cfg.adapterLuid.HighPart)
                    break;
                adapter.Reset();
            }
        }
        const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        ComPtr<ID3D11Device> device;
        if (FAILED(D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION, &device, nullptr,
                                     &m_ctx))) {
            Log("D3D11CreateDevice failed");
            return;
        }
        device.As(&m_device);
        // The copy of each frame for RinCast Streamer queues behind the game's own GPU work
        // otherwise: with a busy GPU it waits most of a frame, and the stream loses frames.
        // A relative priority, within this process's class: no privilege needed.
        ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(device.As(&dxgi))) {
            const HRESULT hr = dxgi->SetGPUThreadPriority(7);
            Log("GPU priority for the frame copy: %s", SUCCEEDED(hr) ? "raised" : "unchanged (refused)");
        }
        ComPtr<ID3DBlob> vs, ps, err;
        D3DCompile(kBlitShader, strlen(kBlitShader), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err);
        D3DCompile(kBlitShader, strlen(kBlitShader), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err);
        if (!vs || !ps) {
            Log("Shader compile failed");
            return;
        }
        m_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs);
        m_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        m_device->CreateSamplerState(&sd, &m_sampler);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        m_device->CreateBuffer(&bd, nullptr, &m_rectBuffer);
        D3D11_BLEND_DESC bl{};
        auto& rt = bl.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        m_device->CreateBlendState(&bl, &m_blend);
    }

    ComPtr<ID3D11Texture2D> OpenBackbuffer(vr::SharedTextureHandle_t handle) {
        if (auto it = m_backbuffers.find(handle); it != m_backbuffers.end()) return it->second;
        if (m_backbuffers.size() > 8) m_backbuffers.clear();
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(m_device->OpenSharedResource(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&tex)))) {
            Log("OpenSharedResource(backbuffer) failed");
            return nullptr;
        }
        m_backbuffers[handle] = tex;
        return tex;
    }

    ID3D11Texture2D* SlotTexture(int slot) {
        rcvr::Shared* s = g_shared->bridge.shared();
        if (m_textureGeneration != s->configGeneration || !m_slots[slot]) {
            if (m_textureGeneration != s->configGeneration) {
                for (auto& t : m_slots) t.Reset();
                m_textureGeneration = s->configGeneration;
            }
            if (FAILED(m_device->OpenSharedResourceByName(s->textureNames[slot], DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                          IID_PPV_ARGS(&m_slots[slot])))) {
                Log("OpenSharedResourceByName failed");
                return nullptr;
            }
        }
        return m_slots[slot].Get();
    }

    // Draws one eye of a layer into its half of the bridge slot. The first layer (the
    // game) covers the eye; later ones (overlays) are blended over it. Views are chosen so
    // the stored values pass through unchanged: an sRGB texture is decoded on sampling
    // and encoded again on writing, plain 8/10-bit ones are copied as they are, and float
    // (linear) ones are encoded to sRGB.
    void DrawLayer(ID3D11Texture2D* src, const vr::VRTextureBounds_t& b, ID3D11Texture2D* dst, const D3D11_VIEWPORT& vp,
                   bool blend) {
        D3D11_TEXTURE2D_DESC sdesc;
        src->GetDesc(&sdesc);
        if (sdesc.SampleDesc.Count > 1) return;  // SteamVR resolves before handing layers over
        DXGI_FORMAT srvFormat = sdesc.Format;
        bool srgbOut = false;
        switch (sdesc.Format) {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM; break;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: srvFormat = DXGI_FORMAT_B8G8R8A8_UNORM; break;
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: srvFormat = DXGI_FORMAT_R10G10B10A2_UNORM; break;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: srvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT; srgbOut = true; break;
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R11G11B10_FLOAT:
            case DXGI_FORMAT_R32G32B32A32_FLOAT: srgbOut = true; break;
            default: break;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format = srvFormat;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(m_device->CreateShaderResourceView(src, &srvd, &srv))) return;
        D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
        rtvd.Format = srgbOut ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(m_device->CreateRenderTargetView(dst, &rtvd, &rtv))) return;
        const float rect[4] = {b.uMin, b.vMin, b.uMax, b.vMax};
        m_ctx->UpdateSubresource(m_rectBuffer.Get(), 0, nullptr, rect, 0, 0);
        m_ctx->RSSetViewports(1, &vp);
        m_ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        m_ctx->OMSetBlendState(blend ? m_blend.Get() : nullptr, nullptr, 0xFFFFFFFF);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->IASetInputLayout(nullptr);
        m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
        m_ctx->VSSetConstantBuffers(0, 1, m_rectBuffer.GetAddressOf());
        m_ctx->PSSetShader(m_ps.Get(), nullptr, 0);
        m_ctx->PSSetShaderResources(0, 1, srv.GetAddressOf());
        m_ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
        m_ctx->RSSetState(nullptr);
        m_ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        m_ctx->PSSetShaderResources(0, 1, &nullSrv);
        ID3D11RenderTargetView* nullRtv = nullptr;
        m_ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
    }

    // Caller holds m_layerMutex.
    ComPtr<ID3D11Texture2D> FindSwapTexture(vr::SharedTextureHandle_t handle) {
        auto it = m_swapSets.find(handle);
        if (it == m_swapSets.end()) return nullptr;
        for (int i = 0; i < 3; i++)
            if (it->second->handles[i] == handle) return it->second->tex[i];
        return nullptr;
    }

    // The tracking sample whose head pose SteamVR rendered the frame with: the closest of
    // the poses handed to it recently (they are identical up to float rounding).
    rcvr::TrackingSnapshot RenderedWith(const vr::HmdMatrix34_t& m) {
        std::lock_guard lock(g_shared->mutex);
        if (g_shared->history.empty()) return g_shared->frameTracking;
        const rcvr::TrackingSnapshot* best = nullptr;
        double bestScore = 1e30;
        for (const auto& t : g_shared->history) {
            const vr::DriverPose_t p = MakePose(t.hmd, true, true, 0, false);
            const auto& q = p.qRotation;
            // Rotation matrix of q, as SteamVR builds the device-to-absolute matrix.
            const double r[3][3] = {{1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w)},
                                    {2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w)},
                                    {2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y)}};
            double score = 0;
            for (int i = 0; i < 3; i++) {
                score += std::fabs(m.m[i][3] - p.vecPosition[i]);
                for (int j = 0; j < 3; j++) score += std::fabs(m.m[i][j] - r[i][j]);
            }
            if (score < bestScore) {
                bestScore = score;
                best = &t;
            }
        }
        if (bestScore > 0.01 && m_poseMismatchLogs < 5) {
            m_poseMismatchLogs++;
            Log("Frame pose matches no recent tracking sample closely (difference %.4f)", bestScore);
        }
        return *best;
    }

    rcvr::DisplayConfig m_cfg;
    std::mutex m_cfgMutex;  // fov / ipd / refresh can change live (ApplyLiveConfig)
    std::string m_serial;
    bool m_touchCompatibility = false;
    mutable std::mutex m_poseMutex;
    std::atomic<bool> m_ready{false};
    std::atomic<uint32_t> m_id{vr::k_unTrackedDeviceIndexInvalid};
    vr::DriverPose_t m_lastPose{};
    std::atomic<int64_t> m_vsyncPeriodNs;
    int64_t m_vsyncEpochNs;
    uint64_t m_frameIndex = 0;
    // The frame Present just handed over, for PostPresent to wait on (see WaitForStreamer).
    int m_heldSlot = -1;
    uint64_t m_heldFrame = 0;
    int64_t m_holdOffUntilNs = 0;
    int64_t m_pacedVsync = 0;  // the vsync PostPresent last held SteamVR to

    ComPtr<ID3D11Device1> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11SamplerState> m_sampler;
    std::map<vr::SharedTextureHandle_t, ComPtr<ID3D11Texture2D>> m_backbuffers;
    ComPtr<ID3D11Texture2D> m_slots[rcvr::kFrameSlots];
    LONG m_textureGeneration = -1;

    struct SwapSet {
        uint32_t pid = 0;
        ComPtr<ID3D11Texture2D> tex[3];
        vr::SharedTextureHandle_t handles[3]{};
        uint32_t next = 0;
    };
    std::mutex m_layerMutex;  // swap sets and layers (SteamVR calls from its own threads)
    std::map<vr::SharedTextureHandle_t, std::shared_ptr<SwapSet>> m_swapSets;
    std::vector<std::array<SubmitLayerPerEye_t, 2>> m_layers;
    std::atomic<uint32_t> m_presents{0}, m_layersSubmitted{0}, m_framesPublished{0};
    std::atomic<int64_t> m_lastPublishNs{0};
    ComPtr<ID3D11Buffer> m_rectBuffer;
    ComPtr<ID3D11BlendState> m_blend;
    int m_poseMismatchLogs = 0;
};

// ---------------------------------------------------------------------------
// Body trackers: Vive trackers to SteamVR, so games that support full body tracking
// find them. Added the first time the headset sends that joint.
// ---------------------------------------------------------------------------
struct TrackerInfo {
    const char* name;
    const char* controllerType;
    const char* role;
};
// Same order as rc::BodyTracker.
const TrackerInfo kTrackers[rcvr::kBodyTrackers] = {
    {"Waist", "vive_tracker_waist", "TrackerRole_Waist"},
    {"Chest", "vive_tracker_chest", "TrackerRole_Chest"},
    {"LeftFoot", "vive_tracker_left_foot", "TrackerRole_LeftFoot"},
    {"RightFoot", "vive_tracker_right_foot", "TrackerRole_RightFoot"},
    {"LeftKnee", "vive_tracker_left_knee", "TrackerRole_LeftKnee"},
    {"RightKnee", "vive_tracker_right_knee", "TrackerRole_RightKnee"},
    {"LeftElbow", "vive_tracker_left_elbow", "TrackerRole_LeftElbow"},
    {"RightElbow", "vive_tracker_right_elbow", "TrackerRole_RightElbow"},
    {"LeftShoulder", "vive_tracker_left_shoulder", "TrackerRole_LeftShoulder"},
    {"RightShoulder", "vive_tracker_right_shoulder", "TrackerRole_RightShoulder"},
};

class BodyTrackerDevice : public vr::ITrackedDeviceServerDriver {
   public:
    BodyTrackerDevice(const std::string& hmdSerial, const TrackerInfo& info)
        : m_serial(hmdSerial + "_Tracker_" + info.name), m_info(info) {}

    const std::string& Serial() const { return m_serial; }

    vr::EVRInitError Activate(uint32_t id) override {
        std::lock_guard stateLock(m_stateMutex);
        m_id = id;
        auto* props = vr::VRProperties();
        const auto c = props->TrackedDeviceToPropertyContainer(id);
        const std::string registered = "htc/vive_tracker" + m_serial;
        props->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "oculus");
        props->SetStringProperty(c, vr::Prop_ManufacturerName_String, "HTC");
        props->SetStringProperty(c, vr::Prop_ModelNumber_String, "Vive Tracker Pro MV");
        props->SetStringProperty(c, vr::Prop_SerialNumber_String, m_serial.c_str());
        props->SetStringProperty(c, vr::Prop_RegisteredDeviceType_String, registered.c_str());
        props->SetStringProperty(c, vr::Prop_ResourceRoot_String, "htc");
        props->SetStringProperty(c, vr::Prop_RenderModelName_String, "{htc}vr_tracker_vive_1_0");
        props->SetStringProperty(c, vr::Prop_ControllerType_String, m_info.controllerType);
        props->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{htc}/input/vive_tracker_profile.json");
        props->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32, vr::TrackedControllerRole_OptOut);
        props->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_GenericTracker);
        props->SetUint64Property(c, vr::Prop_CurrentUniverseId_Uint64, kUniverseId);
        props->SetBoolProperty(c, vr::Prop_WillDriftInYaw_Bool, false);
        props->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
        // SteamVR's "Manage Trackers" setting: what the tracker is worn on.
        vr::VRSettings()->SetString("trackers", ("/devices/" + registered).c_str(), m_info.role);
        return vr::VRInitError_None;
    }

    void Deactivate() override {
        std::lock_guard stateLock(m_stateMutex);
        m_id = vr::k_unTrackedDeviceIndexInvalid;
    }
    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* response, uint32_t size) override {
        if (size) response[0] = 0;
    }
    vr::DriverPose_t GetPose() override {
        std::lock_guard stateLock(m_stateMutex);
        return m_lastPose;
    }

    void Update(const rcvr::BridgePose& pose, bool valid) {
        std::lock_guard stateLock(m_stateMutex);
        if (m_id == vr::k_unTrackedDeviceIndexInvalid) return;
        m_lastPose = MakePose(pose, valid, valid, 0.0, false);
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_id, m_lastPose, sizeof(m_lastPose));
    }

   private:
    mutable std::mutex m_stateMutex;
    std::string m_serial;
    TrackerInfo m_info;
    uint32_t m_id = vr::k_unTrackedDeviceIndexInvalid;
    vr::DriverPose_t m_lastPose{};
};

// ---------------------------------------------------------------------------
// Provider
// ---------------------------------------------------------------------------
class Provider : public vr::IServerTrackedDeviceProvider {
   public:
    vr::EVRInitError Init(vr::IVRDriverContext* context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
        g_shared = new Shared();
        timeBeginPeriod(1);
        Log("RinCast driver loaded");
        TryAddDevices();
        m_running = true;
        m_thread = std::thread([this] { TrackingLoop(); });
        return vr::VRInitError_None;
    }

    void Cleanup() override {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
        m_hmd.reset();
        m_controllers[0].reset();
        m_controllers[1].reset();
        m_hands[0].reset();
        m_hands[1].reset();
        for (auto& t : m_trackers) t.reset();
        delete g_shared;
        g_shared = nullptr;
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }

    const char* const* GetInterfaceVersions() override { return vr::k_InterfaceVersions; }

    void RunFrame() override {
        TryAddDevices();
        AddBodyTrackers();
        // Haptics from games -> headset.
        vr::VREvent_t ev;
        while (vr::VRServerDriverHost()->PollNextEvent(&ev, sizeof(ev))) {
            if (ev.eventType == vr::VREvent_Input_HapticVibration) {
                const auto& h = ev.data.hapticVibration;
                std::lock_guard lock(m_deviceMutex);
                for (int i = 0; i < 2; i++) {
                    if (m_controllers[i] && h.componentHandle == m_controllers[i]->HapticHandle())
                        g_shared->bridge.PushHaptic(uint32_t(i), h.fDurationSeconds, h.fFrequency, h.fAmplitude);
                }
            }
        }
    }

    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

   private:
    void TryAddDevices() {
        std::lock_guard deviceLock(m_deviceMutex);
        // Retry opening the bridge at most twice a second.
        const int64_t now = rcvr::QpcNowNs();
        if (now - m_lastAttemptNs < 500'000'000LL) return;
        m_lastAttemptNs = now;
        if (!g_shared->bridge.Open()) return;
        rcvr::Shared* s = g_shared->bridge.shared();
        rcvr::DisplayConfig config{};
        if (!rcvr::ReadDisplayConfig(s, config)) return;
        if (config.eyeWidth == 0 || config.eyeHeight == 0) return;  // never saw a headset
        const bool newHmd = !m_hmd;
        if (newHmd) {
            m_hmd = std::make_unique<Hmd>(config);
            vr::VRServerDriverHost()->TrackedDeviceAdded(m_hmd->Serial().c_str(), vr::TrackedDeviceClass_HMD, m_hmd.get());
        }
        config.touchControllers = m_hmd->TouchCompatibility();
        rcvr::TrackingSnapshot tracking{};
        const bool live = s->headsetConnected != 0 && g_shared->bridge.StreamerAlive() && rcvr::ReadTracking(s, tracking);
        for (int i = 0; i < 2; i++) {
            if (!m_controllers[i] && live && HasLiveInputSample(tracking, i, false, now)) {
                const auto names = ControllerNamesFor(config.headsetModel, config.controllerProfiles[i], config.touchControllers != 0);
                if (names) {
                    const auto serial = m_hmd->Serial() + (i == 0 ? "_Controller_Left" : "_Controller_Right");
                    auto controller = std::make_unique<Controller>(i, serial, *names);
                    if (!controller->PrepareResources()) continue;
                    m_controllers[i] = std::move(controller);
                    if (!vr::VRServerDriverHost()->TrackedDeviceAdded(serial.c_str(), vr::TrackedDeviceClass_Controller,
                                                                      m_controllers[i].get())) {
                        m_controllers[i].reset();
                        continue;
                    }
                    Log("Added %s controller from headset profile %s%s", i == 0 ? "left" : "right", config.controllerProfiles[i],
                        config.touchControllers ? " (Touch compatibility)" : "");
                }
            }
            rcvr::HandSnapshot bones{};
            if (m_hands[i] || !live || !HasLiveInputSample(tracking, i, true, now) || !rcvr::ReadHand(s, i, bones) ||
                bones.receivedPcNs <= 0 || now < bones.receivedPcNs || now - bones.receivedPcNs >= 200'000'000LL)
                continue;
            const std::string hand = m_hmd->Serial() + (i == 0 ? "_Hand_Left" : "_Hand_Right");
            m_hands[i] = std::make_unique<HandDevice>(i, hand);
            vr::VRServerDriverHost()->TrackedDeviceAdded(hand.c_str(), vr::TrackedDeviceClass_Controller, m_hands[i].get());
            Log("Added tracked %s hand after receiving live tracking and skeleton", i == 0 ? "left" : "right");
        }
        strncpy_s(s->applicationName, "SteamVR", _TRUNCATE);
        if (newHmd)
            Log("Added HMD %s, rendering %ux%u per eye @ %.0f Hz", NamesFor(config.headsetModel).hmd.c_str(), m_hmd->RenderWidth(),
                m_hmd->RenderHeight(), config.refreshRate);
    }

    void AddBodyTrackers() {
        std::lock_guard deviceLock(m_deviceMutex);
        rcvr::Shared* s = g_shared->bridge.shared();
        rcvr::BodySnapshot b;
        if (!m_hmd || !s || !rcvr::ReadBody(s, b)) return;
        for (int i = 0; i < rcvr::kBodyTrackers; i++) {
            if (m_trackerAdded[i] || !(b.validMask & (1u << i))) continue;
            m_trackers[i] = std::make_unique<BodyTrackerDevice>(m_hmd->Serial(), kTrackers[i]);
            vr::VRServerDriverHost()->TrackedDeviceAdded(m_trackers[i]->Serial().c_str(), vr::TrackedDeviceClass_GenericTracker,
                                                         m_trackers[i].get());
            m_trackerAdded[i] = true;  // the tracking thread may use it from here on
            Log("Added body tracker %s", kTrackers[i].name);
        }
    }

    void UpdateBodyTrackers(const rcvr::Shared* s, bool connected) {
        std::lock_guard deviceLock(m_deviceMutex);
        rcvr::BodySnapshot b;
        if (!rcvr::ReadBody(s, b)) return;
        // Turned off on the headset, or the headset left VR: the trackers go offline.
        const bool fresh = connected && rcvr::QpcNowNs() - b.receivedPcNs < 500'000'000LL;
        for (int i = 0; i < rcvr::kBodyTrackers; i++) {
            if (!m_trackerAdded[i]) continue;
            const bool valid = fresh && (b.validMask & (1u << i));
            if (valid || m_trackerWasValid[i]) m_trackers[i]->Update(b.poses[i], valid);
            m_trackerWasValid[i] = valid;
        }
    }

    // Pushes poses once per (virtual) vsync, right before SteamVR starts a frame,
    // and remembers which sample that frame will be rendered with.
    void TrackingLoop() {
        int64_t lastPushed = 0, lastVsync = 0;
        LONG configGeneration = -1;
        rcvr::TrackingSnapshot previous{};
        int jumpLogs = 0;
        int64_t lastFrameReportNs = rcvr::QpcNowNs();
        int64_t lastPoseReportNs = lastFrameReportNs;
        double poseWaitMs = 0, poseWaitMaxMs = 0;
        int poseWaits = 0, poseRepeats = 0;
        while (m_running) {
            Hmd* hmd = nullptr;
            {
                std::lock_guard deviceLock(m_deviceMutex);
                hmd = m_hmd.get();
            }
            if (!hmd || !hmd->Ready() || !g_shared->bridge.shared()) {
                Sleep(50);
                continue;
            }
            rcvr::Shared* s = g_shared->bridge.shared();
            rcvr::DisplayConfig config{};
            if (!rcvr::ReadDisplayConfig(s, config)) continue;
            config.touchControllers = hmd->TouchCompatibility();
            if (const LONG gen = s->configGeneration; gen != configGeneration) {
                if (configGeneration != -1) hmd->ApplyLiveConfig(config);
                configGeneration = gen;
            }
            const int64_t period = hmd->VsyncPeriodNs();
            const int64_t now = rcvr::QpcNowNs();
            const int64_t nextVsync = hmd->NextVsync(now, lastVsync);
            lastVsync = nextVsync;
            // Wake ~1 ms before vsync so the pose is in place when the compositor samples it.
            const int64_t wake = nextVsync - 1'000'000;
            if (wake > now) Sleep(DWORD(std::max<int64_t>(0, (wake - now) / 1'000'000)));
            while (rcvr::QpcNowNs() < wake) YieldProcessor();

            rcvr::TrackingSnapshot t;
            const bool have = rcvr::ReadTracking(s, t);
            const bool connected = s->headsetConnected != 0 && g_shared->bridge.StreamerAlive();
            if (connected && have && t.sequence == uint64_t(lastPushed)) poseRepeats++;
            if (have && t.sequence != uint64_t(lastPushed)) {
                lastPushed = int64_t(t.sequence);
                // How long this sample waited here for the vsync: time the headset could have
                // sampled later by, had it known when SteamVR takes it.
                const double waitedMs = double(rcvr::QpcNowNs() - t.receivedPcNs) / 1e6;
                if (waitedMs >= 0 && waitedMs < 100) {
                    poseWaitMs += waitedMs;
                    poseWaitMaxMs = std::max(poseWaitMaxMs, waitedMs);
                    poseWaits++;
                }
                // Headset positions that jump: no head moves 10 cm between two samples a few ms
                // apart, so the headset's space moved (a recenter, or its tracking catching up).
                // Lighthouse trackers calibrated against the headset slide out of place then.
                if ((t.flags & 1) && (previous.flags & 1) && t.receivedPcNs - previous.receivedPcNs < 100'000'000) {
                    const double dx = t.hmd.position[0] - previous.hmd.position[0], dy = t.hmd.position[1] - previous.hmd.position[1],
                                 dz = t.hmd.position[2] - previous.hmd.position[2];
                    const double moved = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (moved > 0.10 && jumpLogs < 50) {
                        jumpLogs++;
                        Log("Headset position jumped %.0f cm between two samples %.0f ms apart", moved * 100,
                            double(t.receivedPcNs - previous.receivedPcNs) / 1e6);
                    }
                }
                previous = t;
                // Controllers carry velocities (games need them, e.g. for throwing), so SteamVR
                // extrapolates them from the time poseTimeOffset says they are for to the
                // time it predicts for when a game asks. The headset already predicted them
                // for when it shows the frame. Labelled with that time, SteamVR rewound them
                // to the render moment: games drew the hands 25-40 ms behind. Label them with
                // the time SteamVR predicts for instead, so games get exactly the headset's
                // prediction. Measured with tools/pose-probe + headset-sim --move (direct mode):
                // games using this pose ask for about 2 vsyncs after the one following the push,
                // plus 2 ms (90 Hz: 26 ms, 72 Hz: 28 ms).
                const int64_t steamVrPredicts = nextVsync + 2 * period + 2'000'000;
                const double clamped = std::clamp(double(steamVrPredicts - rcvr::QpcNowNs()) / 1e9, 0.0, 0.1);
                {
                    std::lock_guard lock(g_shared->mutex);
                    g_shared->frameTracking = t;
                    g_shared->history.push_back(t);
                    while (g_shared->history.size() > 32) g_shared->history.pop_front();
                }
                hmd->Update(t, connected, clamped);
                std::lock_guard deviceLock(m_deviceMutex);
                for (int i = 0; i < 2; i++) {
                    // Bones from a hand the headset still sees; otherwise the controller's own.
                    rcvr::HandSnapshot hand;
                    const bool tracked = rcvr::ReadHand(s, i, hand) &&
                                         rcvr::QpcNowNs() - hand.receivedPcNs < 200'000'000LL;
                    if (m_controllers[i]) {
                        m_controllers[i]->SelectReportedProfile(config);
                        m_controllers[i]->Update(t, tracked ? &hand : nullptr, connected, clamped);
                    }
                    if (m_hands[i]) m_hands[i]->Update(t, tracked ? &hand : nullptr, connected, clamped);
                }
            } else if (!connected && have) {
                hmd->Update(t, false, 0);
                std::lock_guard deviceLock(m_deviceMutex);
                for (int i = 0; i < 2; ++i) {
                    if (m_controllers[i]) m_controllers[i]->Update(t, nullptr, false, 0);
                    if (m_hands[i]) m_hands[i]->Update(t, nullptr, false, 0);
                }
            }
            UpdateBodyTrackers(s, connected);
            // While a headset is connected but no picture reaches the streamer, say every 10 s how
            // far SteamVR got: no textures, no layers, no presents, or frames dropped on the way.
            if (connected && rcvr::QpcNowNs() - lastFrameReportNs > 10'000'000'000LL) {
                lastFrameReportNs = rcvr::QpcNowNs();
                const auto c = hmd->Counts();
                if (lastFrameReportNs - c.lastPublishNs > 10'000'000'000LL)
                    Log("No frame for RinCast Streamer in 10 s. Since SteamVR started: %zu texture sets, %u layers, "
                        "%u presents, %u frames handed over",
                        c.textureSets, c.layers, c.presents, c.published);
            }
            // Every 30 s: how old the headset's poses were when they went to SteamVR. Close to a
            // whole vsync means the headset samples just after SteamVR took one; a vsync without
            // a new one renders with the pose of the vsync before.
            if (rcvr::QpcNowNs() - lastPoseReportNs > 30'000'000'000LL) {
                lastPoseReportNs = rcvr::QpcNowNs();
                if (poseWaits > 100)
                    Log("Headset poses waited %.1f ms for their vsync on average (longest %.1f ms, one vsync is %.1f ms); "
                        "%d of %d vsyncs had no new pose",
                        poseWaitMs / poseWaits, poseWaitMaxMs, double(period) / 1e6, poseRepeats, poseWaits + poseRepeats);
                poseWaitMs = poseWaitMaxMs = 0;
                poseWaits = poseRepeats = 0;
            }
            // Direct mode: SteamVR paces its frames by these.
            while (rcvr::QpcNowNs() < nextVsync) YieldProcessor();
            vr::VRServerDriverHost()->VsyncEvent(0.0);
            InterlockedExchange64(&s->producerHeartbeatNs, rcvr::QpcNowNs());
        }
    }

    std::unique_ptr<Hmd> m_hmd;
    std::unique_ptr<Controller> m_controllers[2];
    std::mutex m_deviceMutex;
    std::unique_ptr<HandDevice> m_hands[2];
    std::unique_ptr<BodyTrackerDevice> m_trackers[rcvr::kBodyTrackers];
    std::atomic<bool> m_trackerAdded[rcvr::kBodyTrackers]{};
    bool m_trackerWasValid[rcvr::kBodyTrackers]{};
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    int64_t m_lastAttemptNs = 0;
};

Provider g_provider;

}  // namespace

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* interfaceName, int* returnCode) {
    if (strcmp(interfaceName, vr::IServerTrackedDeviceProvider_Version) == 0) return &g_provider;
    if (returnCode) *returnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
