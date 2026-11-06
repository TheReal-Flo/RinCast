// RinCast SteamVR driver.
//
// Exposes the streamed headset to SteamVR as an HMD ("virtual display": SteamVR
// composites and hands us the final side-by-side backbuffer) plus two Quest
// Touch Plus controllers that reuse SteamVR's built-in Oculus Touch input
// profile, so games pick their native Quest bindings. Everything is exchanged
// with the RinCast Streamer through the shared-memory VR bridge.
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hand_poses.h"
#include "openvr/openvr_driver.h"
#include "vr_bridge.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

namespace {

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
struct Quat {
    double w, x, y, z;
};
Quat Mul(const Quat& a, const Quat& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
void Rotate(const Quat& q, const double v[3], double out[3]) {
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
RigidPose Compose(const RigidPose& a, const RigidPose& b) {  // a * b: b expressed in a's frame
    RigidPose r;
    r.q = Mul(a.q, b.q);
    Rotate(a.q, b.t, r.t);
    for (int i = 0; i < 3; i++) r.t[i] += a.t[i];
    return r;
}
RigidPose Invert(const RigidPose& p) {
    RigidPose r;
    r.q = {p.q.w, -p.q.x, -p.q.y, -p.q.z};
    Rotate(r.q, p.t, r.t);
    for (double& v : r.t) v = -v;
    return r;
}

// OpenXR grip pose -> the device ("raw") pose SteamVR expects for this controller.
// SteamVR's render model (resources/rendermodels/oculus_quest_plus_controller_left)
// says where the OpenXR grip sits relative to that pose: its "openxr_grip" component,
// 10.2 cm behind the origin and pitched 20.6 degrees (mirrored for the right hand). That
// is what SteamVR itself reports as the grip pose to OpenXR games, so mapping the
// headset's grip pose through it puts the controller exactly where the hand is.
// (VDXR's calibration, used before, relates grip to Virtual Desktop's own controller
// pose, not to SteamVR's: 5 cm and 39 degrees off.)
RigidPose GripToOculus(int hand) {
    RigidPose gripInDevice;
    const double half = 20.6 * 3.14159265358979 / 360;
    gripInDevice.q = {std::cos(half), std::sin(half), 0, 0};
    gripInDevice.t[0] = hand == 0 ? 0.007 : -0.007;
    gripInDevice.t[1] = -0.00182941;
    gripInDevice.t[2] = 0.1019482;
    return Invert(gripInDevice);  // device pose in grip space
}

Quat FromEuler(double pitchDeg, double yawDeg, double rollDeg) {
    const double p = pitchDeg * 3.14159265358979 / 360, y = yawDeg * 3.14159265358979 / 360,
                 r = rollDeg * 3.14159265358979 / 360;
    const Quat qx{std::cos(p), std::sin(p), 0, 0}, qy{std::cos(y), 0, std::sin(y), 0}, qz{std::cos(r), 0, 0, std::sin(r)};
    return Mul(Mul(qy, qx), qz);
}

vr::DriverPose_t MakePose(const rcvr::BridgePose& p, bool valid, bool connected, double timeOffset, bool withVelocity,
                          const Quat* localRot = nullptr, const double* localPos = nullptr) {
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation.w = 1;
    pose.qDriverFromHeadRotation.w = 1;
    Quat q{p.orientation[3], p.orientation[0], p.orientation[1], p.orientation[2]};
    double pos[3] = {p.position[0], p.position[1], p.position[2]};
    if (localPos) {
        double off[3];
        Rotate(q, localPos, off);
        pos[0] += off[0];
        pos[1] += off[1];
        pos[2] += off[2];
    }
    if (localRot) q = Mul(q, *localRot);
    pose.qRotation = {q.w, q.x, q.y, q.z};
    for (int i = 0; i < 3; i++) {
        pose.vecPosition[i] = pos[i];
        pose.vecVelocity[i] = withVelocity ? p.linearVelocity[i] : 0;
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
// Controller
// ---------------------------------------------------------------------------
class Controller : public vr::ITrackedDeviceServerDriver {
   public:
    explicit Controller(int hand, std::string serial) : m_hand(hand), m_serial(std::move(serial)) {}

    vr::EVRInitError Activate(uint32_t id) override {
        m_id = id;
        auto* props = vr::VRProperties();
        const auto c = props->TrackedDeviceToPropertyContainer(id);
        const bool left = m_hand == 0;
        props->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "oculus");
        props->SetStringProperty(c, vr::Prop_ManufacturerName_String, "Oculus");
        props->SetStringProperty(c, vr::Prop_ModelNumber_String,
                                 left ? "Oculus Quest3 (Left Controller)" : "Oculus Quest3 (Right Controller)");
        props->SetStringProperty(c, vr::Prop_SerialNumber_String, m_serial.c_str());
        props->SetStringProperty(c, vr::Prop_RegisteredDeviceType_String, ("oculus/" + m_serial).c_str());
        props->SetStringProperty(c, vr::Prop_RenderModelName_String,
                                 left ? "oculus_quest_plus_controller_left" : "oculus_quest_plus_controller_right");
        props->SetStringProperty(c, vr::Prop_ControllerType_String, "oculus_touch");
        props->SetStringProperty(c, vr::Prop_InputProfilePath_String, "{oculus}/input/touch_profile.json");
        props->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32,
                                left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        props->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);
        props->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
        props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceOff_String,
                                 left ? "{oculus}/icons/cb_left_controller_off.png" : "{oculus}/icons/cb_right_controller_off.png");
        props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceReady_String,
                                 left ? "{oculus}/icons/cb_left_controller_ready.png" : "{oculus}/icons/cb_right_controller_ready.png");
        props->SetStringProperty(c, vr::Prop_NamedIconPathDeviceSearching_String,
                                 left ? "{oculus}/icons/cb_left_controller_searching.gif" : "{oculus}/icons/cb_right_controller_searching.gif");

        auto* in = vr::VRDriverInput();
        auto boolean = [&](const char* path) {
            vr::VRInputComponentHandle_t h;
            in->CreateBooleanComponent(c, path, &h);
            return h;
        };
        auto scalar = [&](const char* path, vr::EVRScalarUnits units) {
            vr::VRInputComponentHandle_t h;
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
        m_thumbrest = boolean("/input/thumbrest/touch");
        in->CreateHapticComponent(c, "/output/haptic", &m_haptic);
        in->CreateSkeletonComponent(c, left ? "/input/skeleton/left" : "/input/skeleton/right",
                                    left ? "/skeleton/hand/left" : "/skeleton/hand/right", "/pose/raw",
                                    vr::VRSkeletalTracking_Partial, nullptr, 0, &m_skeleton);
        // Games that read the skeleton before the first pose would otherwise get a frozen hand.
        UpdateControllerSkeleton({});
        return vr::VRInitError_None;
    }

    void Deactivate() override { m_id = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* response, uint32_t size) override {
        if (size) response[0] = 0;
    }
    vr::DriverPose_t GetPose() override { return m_lastPose; }

    uint32_t Id() const { return m_id; }
    vr::VRInputComponentHandle_t HapticHandle() const { return m_haptic; }

    void Update(const rcvr::TrackingSnapshot& t, const rcvr::HandSnapshot* tracked, bool headsetConnected, double timeOffset) {
        if (m_id == vr::k_unTrackedDeviceIndexInvalid) return;
        const auto& s = t.controllers[m_hand];
        const bool connected = headsetConnected && (s.flags & 1);
        const bool valid = connected && (s.flags & 2);
        // OpenXR grip pose -> SteamVR Oculus Touch "raw" pose.
        static const RigidPose toOculus[2] = {GripToOculus(0), GripToOculus(1)};
        const RigidPose& l = toOculus[m_hand];
        // A tracked hand's pose is already the controller's: see rc::HandDevicePose.
        m_lastPose = tracked && (s.flags & 4) ? MakePose(s.pose, valid, connected, timeOffset, false)
                             : MakePose(s.pose, valid, connected, timeOffset, true, &l.q, l.t);
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_id, m_lastPose, sizeof(m_lastPose));

        auto* in = vr::VRDriverInput();
        const uint32_t b = s.buttons;
        const bool left = m_hand == 0;
        auto bit = [&](uint32_t mask) { return (b & mask) != 0; };
        in->UpdateBooleanComponent(m_system, left && bit(1u << 4), 0);  // menu button on left = system
        in->UpdateBooleanComponent(m_systemTouch, left && bit(1u << 4), 0);
        in->UpdateBooleanComponent(m_btn1, bit(left ? 1u << 2 : 1u << 0), 0);
        in->UpdateBooleanComponent(m_btn1Touch, bit(left ? 1u << 18 : 1u << 16), 0);
        in->UpdateBooleanComponent(m_btn2, bit(left ? 1u << 3 : 1u << 1), 0);
        in->UpdateBooleanComponent(m_btn2Touch, bit(left ? 1u << 19 : 1u << 17), 0);
        in->UpdateScalarComponent(m_trigger, s.trigger, 0);
        in->UpdateBooleanComponent(m_triggerClick, s.trigger > 0.85f, 0);
        in->UpdateBooleanComponent(m_triggerTouch, bit(1u << 21) || s.trigger > 0.05f, 0);
        in->UpdateScalarComponent(m_grip, s.grip, 0);
        in->UpdateBooleanComponent(m_gripClick, s.grip > 0.85f, 0);
        in->UpdateBooleanComponent(m_gripTouch, s.grip > 0.1f, 0);
        in->UpdateScalarComponent(m_stickX, s.thumbstick[0], 0);
        in->UpdateScalarComponent(m_stickY, s.thumbstick[1], 0);
        in->UpdateBooleanComponent(m_stickClick, bit(1u << 6), 0);
        in->UpdateBooleanComponent(m_stickTouch, bit(1u << 20) || bit(1u << 6), 0);
        in->UpdateBooleanComponent(m_thumbrest, bit(1u << 22), 0);
        if (tracked) UpdateTrackedSkeleton(*tracked);
        else UpdateControllerSkeleton(s);
    }

   private:
    void Publish(const vr::VRBoneTransform_t* withController, const vr::VRBoneTransform_t* withoutController) {
        auto* in = vr::VRDriverInput();
        in->UpdateSkeletonComponent(m_skeleton, vr::VRSkeletalMotionRange_WithController, withController, rcvr::kHandBones);
        in->UpdateSkeletonComponent(m_skeleton, vr::VRSkeletalMotionRange_WithoutController, withoutController, rcvr::kHandBones);
    }

    void UpdateTrackedSkeleton(const rcvr::HandSnapshot& h) {
        vr::VRBoneTransform_t bones[rcvr::kHandBones];
        for (int i = 0; i < rcvr::kHandBones; i++) {
            const auto& b = h.bones[i];
            bones[i].position = {b.position[0], b.position[1], b.position[2], 1};
            bones[i].orientation = {b.orientation[3], b.orientation[0], b.orientation[1], b.orientation[2]};
        }
        bones[0] = {{0, 0, 0, 1}, {1, 0, 0, 0}};
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

    int m_hand;
    std::string m_serial;
    uint32_t m_id = vr::k_unTrackedDeviceIndexInvalid;
    vr::DriverPose_t m_lastPose{};
    vr::VRInputComponentHandle_t m_system{}, m_systemTouch{}, m_btn1{}, m_btn1Touch{}, m_btn2{}, m_btn2Touch{},
        m_triggerClick{}, m_triggerTouch{}, m_trigger{}, m_gripClick{}, m_gripTouch{}, m_grip{}, m_stickX{},
        m_stickY{}, m_stickClick{}, m_stickTouch{}, m_thumbrest{}, m_haptic{}, m_skeleton{};
    bool m_lastThumb = false, m_lastIndex = false;
    float m_thumbProgress = 0, m_indexProgress = 0;
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
float4 ps(V i) : SV_Target { return tex.Sample(samp, i.uv); }
)";

class Hmd : public vr::ITrackedDeviceServerDriver, public vr::IVRDisplayComponent,
            public vr::IVRDriverDirectModeComponent {
   public:
    explicit Hmd(const rcvr::DisplayConfig& cfg) : m_cfg(cfg) {
        m_serial = cfg.headsetSerial[0] ? std::string("RINCAST-") + std::string(cfg.headsetSerial).substr(0, 12)
                                        : "RINCAST-HMD";
        m_vsyncPeriodNs = int64_t(1e9 / (cfg.refreshRate > 1 ? cfg.refreshRate : 90.f));
        m_vsyncEpochNs = rcvr::QpcNowNs();
    }

    const std::string& Serial() const { return m_serial; }
    uint32_t Id() const { return m_id; }

    vr::EVRInitError Activate(uint32_t id) override {
        m_id = id;
        auto* props = vr::VRProperties();
        const auto c = props->TrackedDeviceToPropertyContainer(id);
        props->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "oculus");
        props->SetStringProperty(c, vr::Prop_ManufacturerName_String, "Oculus");
        props->SetStringProperty(c, vr::Prop_ModelNumber_String, "Oculus Quest3");
        props->SetStringProperty(c, vr::Prop_SerialNumber_String, m_serial.c_str());
        props->SetStringProperty(c, vr::Prop_RegisteredDeviceType_String, ("oculus/" + m_serial).c_str());
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
        return vr::VRInitError_None;
    }

    void Deactivate() override { m_id = vr::k_unTrackedDeviceIndexInvalid; }
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
    vr::DriverPose_t GetPose() override { return m_lastPose; }

    // ---- IVRDisplayComponent ----
    void GetWindowBounds(int32_t* x, int32_t* y, uint32_t* w, uint32_t* h) override {
        *x = 0;
        *y = 0;
        *w = m_cfg.eyeWidth * 2;
        *h = m_cfg.eyeHeight;
    }
    bool IsDisplayOnDesktop() override { return false; }
    bool IsDisplayRealDisplay() override { return false; }
    void GetRecommendedRenderTargetSize(uint32_t* w, uint32_t* h) override {
        *w = m_cfg.eyeWidth;
        *h = m_cfg.eyeHeight;
    }
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h) override {
        *x = eye == vr::Eye_Left ? 0 : m_cfg.eyeWidth;
        *y = 0;
        *w = m_cfg.eyeWidth;
        *h = m_cfg.eyeHeight;
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
        std::lock_guard lock(m_layerMutex);
        if (m_layers.size() < 8) m_layers.push_back({perEye[0], perEye[1]});
    }

    void Present(vr::SharedTextureHandle_t syncTexture) override {
        std::vector<std::array<SubmitLayerPerEye_t, 2>> layers;
        std::vector<std::array<ComPtr<ID3D11Texture2D>, 2>> textures;
        {
            std::lock_guard lock(m_layerMutex);
            layers.swap(m_layers);
            for (const auto& l : layers) {
                textures.push_back({FindSwapTexture(l[0].hTexture), FindSwapTexture(l[1].hTexture)});
            }
        }
        if (!m_device || layers.empty() || !g_shared->bridge.shared()) return;
        rcvr::Shared* s = g_shared->bridge.shared();
        // SteamVR's rendering into the layers is done once we hold the sync texture.
        ComPtr<ID3D11Texture2D> sync = OpenBackbuffer(syncTexture);
        ComPtr<IDXGIKeyedMutex> syncMutex;
        if (sync) sync.As(&syncMutex);
        if (syncMutex && !rcvr::AcquireKeyed(syncMutex.Get(), 100)) return;
        const int slot = g_shared->bridge.AcquireSlot();
        ID3D11Texture2D* dst = slot >= 0 ? SlotTexture(slot) : nullptr;
        ComPtr<IDXGIKeyedMutex> dstMutex;
        if (dst) ComPtr<ID3D11Texture2D>(dst).As(&dstMutex);
        if (!dst || (dstMutex && !rcvr::AcquireKeyed(dstMutex.Get(), 20))) {
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
    }

    // Paces SteamVR to the headset's refresh rate: block until the next (virtual) vsync.
    void PostPresent(const Throttling_t*) override {
        const int64_t now = rcvr::QpcNowNs();
        const int64_t next = NextVsync(now);
        const int64_t coarse = next - 1'500'000;
        if (coarse > now) Sleep(DWORD((coarse - now) / 1'000'000));
        while (rcvr::QpcNowNs() < next) YieldProcessor();
    }
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming* timing) override { timing->m_nReprojectionFlags = 0; }

    int64_t NextVsync(int64_t now) const {
        const int64_t period = m_vsyncPeriodNs;
        const int64_t epoch = VsyncEpochNs();
        return epoch + ((now - epoch) / period + 1) * period;
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
    uint32_t m_id = vr::k_unTrackedDeviceIndexInvalid;
    vr::DriverPose_t m_lastPose{};
    std::atomic<int64_t> m_vsyncPeriodNs;
    int64_t m_vsyncEpochNs;
    uint64_t m_frameIndex = 0;

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

    void Deactivate() override { m_id = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* response, uint32_t size) override {
        if (size) response[0] = 0;
    }
    vr::DriverPose_t GetPose() override { return m_lastPose; }

    void Update(const rcvr::BridgePose& pose, bool valid) {
        if (m_id == vr::k_unTrackedDeviceIndexInvalid) return;
        m_lastPose = MakePose(pose, valid, valid, 0.0, false);
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_id, m_lastPose, sizeof(m_lastPose));
    }

   private:
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
        if (m_hmd) return;
        // Retry opening the bridge at most twice a second.
        const int64_t now = rcvr::QpcNowNs();
        if (now - m_lastAttemptNs < 500'000'000LL) return;
        m_lastAttemptNs = now;
        if (!g_shared->bridge.Open()) return;
        rcvr::Shared* s = g_shared->bridge.shared();
        if (s->config.eyeWidth == 0 || s->config.eyeHeight == 0) return;  // never saw a headset
        m_hmd = std::make_unique<Hmd>(s->config);
        vr::VRServerDriverHost()->TrackedDeviceAdded(m_hmd->Serial().c_str(), vr::TrackedDeviceClass_HMD, m_hmd.get());
        for (int i = 0; i < 2; i++) {
            m_controllers[i] = std::make_unique<Controller>(i, m_hmd->Serial() + (i == 0 ? "_Controller_Left" : "_Controller_Right"));
            vr::VRServerDriverHost()->TrackedDeviceAdded(
                (m_hmd->Serial() + (i == 0 ? "_Controller_Left" : "_Controller_Right")).c_str(),
                vr::TrackedDeviceClass_Controller, m_controllers[i].get());
        }
        strncpy_s(s->applicationName, "SteamVR", _TRUNCATE);
        Log("Added HMD %ux%u per eye @ %.0f Hz", s->config.eyeWidth, s->config.eyeHeight, s->config.refreshRate);
    }

    void AddBodyTrackers() {
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
        int64_t lastPushed = 0;
        LONG configGeneration = -1;
        while (m_running) {
            if (!m_hmd || !g_shared->bridge.shared()) {
                Sleep(50);
                continue;
            }
            rcvr::Shared* s = g_shared->bridge.shared();
            if (const LONG gen = s->configGeneration; gen != configGeneration) {
                if (configGeneration != -1) m_hmd->ApplyLiveConfig(s->config);
                configGeneration = gen;
            }
            const int64_t period = m_hmd->VsyncPeriodNs();
            const int64_t now = rcvr::QpcNowNs();
            const int64_t epoch = m_hmd->VsyncEpochNs();
            const int64_t nextVsync = epoch + ((now - epoch) / period + 1) * period;
            // Wake ~1 ms before vsync so the pose is in place when the compositor samples it.
            const int64_t wake = nextVsync - 1'000'000;
            if (wake > now) Sleep(DWORD(std::max<int64_t>(0, (wake - now) / 1'000'000)));
            while (rcvr::QpcNowNs() < wake) YieldProcessor();

            rcvr::TrackingSnapshot t;
            const bool have = rcvr::ReadTracking(s, t);
            const bool connected = s->headsetConnected != 0 && g_shared->bridge.StreamerAlive();
            if (have && t.sequence != uint64_t(lastPushed)) {
                lastPushed = int64_t(t.sequence);
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
                m_hmd->Update(t, connected, clamped);
                for (int i = 0; i < 2; i++) {
                    if (!m_controllers[i]) continue;
                    // Bones from a hand the headset still sees; otherwise the controller's own.
                    rcvr::HandSnapshot hand;
                    const bool tracked = rcvr::ReadHand(s, i, hand) &&
                                         rcvr::QpcNowNs() - hand.receivedPcNs < 200'000'000LL;
                    m_controllers[i]->Update(t, tracked ? &hand : nullptr, connected, clamped);
                }
            } else if (!connected && have) {
                m_hmd->Update(t, false, 0);
            }
            UpdateBodyTrackers(s, connected);
            // Direct mode: SteamVR paces its frames by these.
            while (rcvr::QpcNowNs() < nextVsync) YieldProcessor();
            vr::VRServerDriverHost()->VsyncEvent(0.0);
            InterlockedExchange64(&s->producerHeartbeatNs, rcvr::QpcNowNs());
        }
    }

    std::unique_ptr<Hmd> m_hmd;
    std::unique_ptr<Controller> m_controllers[2];
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
