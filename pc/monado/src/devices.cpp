// The headset and its controllers as Monado devices, from the streamer's tracking (the bridge's
// TrackingSnapshot). The headset already predicted every pose for when the frame will be shown,
// so a pose is the same whatever time is asked for: the frame goes back with the pose it was
// rendered with, and the headset places it for that pose (as with SteamVR).
//
// Controllers are Oculus Touch controllers to games, which nearly every OpenXR game binds. Hand
// tracking and body trackers aren't offered yet: the headset sends hands in SteamVR's skeleton.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "math/m_api.h"
#include "math/m_mathinclude.h"
#include "rc/protocol.h"
#include "rincast_monado.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_space_overseer.h"

namespace rcm {

namespace {

constexpr auto kAllValid = xrt_space_relation_flags(
    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

xrt_space_relation Relation(const rcvr::BridgePose& p, bool valid) {
    xrt_space_relation r{};
    r.pose = ToXrt(p);
    // A zero quaternion (nothing sent yet) isn't a rotation.
    if (r.pose.orientation.x == 0 && r.pose.orientation.y == 0 && r.pose.orientation.z == 0 && r.pose.orientation.w == 0)
        r.pose.orientation.w = 1;
    r.linear_velocity = {p.linearVelocity[0], p.linearVelocity[1], p.linearVelocity[2]};
    r.angular_velocity = {p.angularVelocity[0], p.angularVelocity[1], p.angularVelocity[2]};
    r.relation_flags = valid ? kAllValid : xrt_space_relation_flags(0);
    return r;
}

// ---------------------------------------------------------------- headset

struct Hmd {
    xrt_device base;
};

xrt_result_t HmdTrackedPose(xrt_device* xdev, xrt_input_name name, int64_t, xrt_space_relation* out) {
    if (name != XRT_INPUT_GENERIC_HEAD_POSE) return XRT_ERROR_INPUT_UNSUPPORTED;
    rcvr::TrackingSnapshot t{};
    const bool have = Hub::Get().Tracking(t);
    // Velocities stay out: the pose is already predicted, extrapolating again would move it twice.
    rcvr::BridgePose p = t.hmd;
    std::memset(p.linearVelocity, 0, sizeof(p.linearVelocity));
    std::memset(p.angularVelocity, 0, sizeof(p.angularVelocity));
    *out = Relation(p, have && (t.flags & rc::kTrackHmdValid));
    if (!have) {
        // No headset yet: somewhere sensible, so the compositor has a pose to render with.
        out->pose = {{0, 0, 0, 1}, {0, 1.6f, 0}};
        out->relation_flags = xrt_space_relation_flags(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT);
    }
    // What the compositor renders the final image with goes back to the headset with the frame.
    if (Hub::Get().OnCompositorThread()) Hub::Get().SetRenderedWith(t);
    return XRT_SUCCESS;
}

xrt_result_t HmdViewPoses(xrt_device* xdev, const xrt_vec3*, int64_t at, xrt_view_type, uint32_t viewCount,
                          xrt_space_relation* outHead, xrt_fov* outFovs, xrt_pose* outPoses) {
    HmdTrackedPose(xdev, XRT_INPUT_GENERIC_HEAD_POSE, at, outHead);
    const rcvr::DisplayConfig& d = Hub::Get().Display();
    const float ipd = d.ipd > 0.04f && d.ipd < 0.09f ? d.ipd : 0.063f;
    for (uint32_t i = 0; i < viewCount && i < 2; i++) {
        outPoses[i] = {{0, 0, 0, 1}, {i == 0 ? -ipd / 2 : ipd / 2, 0, 0}};
        outFovs[i] = xdev->hmd->distortion.fov[i];
    }
    return XRT_SUCCESS;
}

xrt_result_t HmdBattery(xrt_device*, bool* present, bool* charging, float* charge) {
    rcvr::TrackingSnapshot t{};
    const bool have = Hub::Get().Tracking(t);
    const rcvr::BridgeBattery& b = t.battery[0];
    *present = have && (b.flags & rc::kBatteryValid);
    *charging = (b.flags & rc::kBatteryCharging) != 0;
    *charge = b.level;
    return XRT_SUCCESS;
}

void HmdDestroy(xrt_device* xdev) { u_device_free(xdev); }

xrt_device* CreateHmd(xrt_tracking_origin* origin) {
    auto* hmd = U_DEVICE_ALLOCATE(Hmd, U_DEVICE_ALLOC_HMD, 1, 0);
    u_device_populate_function_pointers(&hmd->base, HmdTrackedPose, HmdDestroy);
    hmd->base.get_view_poses = HmdViewPoses;
    hmd->base.get_battery_status = HmdBattery;
    hmd->base.tracking_origin = origin;
    hmd->base.name = XRT_DEVICE_GENERIC_HMD;
    hmd->base.device_type = XRT_DEVICE_TYPE_HMD;
    hmd->base.supported.orientation_tracking = true;
    hmd->base.supported.position_tracking = true;
    hmd->base.supported.battery_status = true;
    hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
    hmd->base.inputs[0].active = true;
    hmd->base.hmd->view_count = 2;

    const rcvr::DisplayConfig& d = Hub::Get().Display();
    std::snprintf(hmd->base.str, sizeof(hmd->base.str), "RinCast (%s)", d.headsetModel[0] ? d.headsetModel : "headset");
    std::snprintf(hmd->base.serial, sizeof(hmd->base.serial), "RINCAST-%.12s", d.headsetSerial[0] ? d.headsetSerial : "HMD");

    u_device_simple_info info{};
    info.display.w_pixels = d.eyeWidth * 2;
    info.display.h_pixels = d.eyeHeight;
    info.display.w_meters = 0.13f;
    info.display.h_meters = 0.07f;
    info.lens_horizontal_separation_meters = 0.065f;
    info.lens_vertical_position_meters = 0.035f;
    info.fov[0] = info.fov[1] = float(100.0 * M_PI / 180.0);
    if (!u_device_setup_split_side_by_side(&hmd->base, &info)) {
        HmdDestroy(&hmd->base);
        return nullptr;
    }
    // The headset's own field of view, per eye and asymmetric: the frame must cover exactly what
    // the headset shows.
    for (int eye = 0; eye < 2; eye++) {
        const float* t = d.fovTan[eye];  // left, right, up, down (positive tangents)
        xrt_fov& f = hmd->base.hmd->distortion.fov[eye];
        f.angle_left = -std::atan(t[0]);
        f.angle_right = std::atan(t[1]);
        f.angle_up = std::atan(t[2]);
        f.angle_down = -std::atan(t[3]);
    }
    hmd->base.hmd->screens[0].nominal_frame_interval_ns = Hub::Get().PeriodNs();
    u_distortion_mesh_set_none(&hmd->base);
    return &hmd->base;
}

// ---------------------------------------------------------------- controllers

enum TouchInput {
    // Left: X, Y, menu. Right: A, B, system.
    kFirstClick = 0,
    kFirstTouch,
    kSecondClick,
    kSecondTouch,
    kMenuOrSystem,
    kSqueeze,
    kTriggerTouch,
    kTriggerValue,
    kStickClick,
    kStickTouch,
    kStick,
    kThumbrest,
    kGripPose,
    kAimPose,
    kInputCount
};

struct Controller {
    xrt_device base;
    int hand;  // 0 left, 1 right
};

bool ControllerState(int hand, rcvr::BridgeController& out, rcvr::TrackingSnapshot& t) {
    if (!Hub::Get().Tracking(t)) return false;
    out = t.controllers[hand];
    return (out.flags & rc::kCtrlConnected) != 0;
}

xrt_result_t ControllerUpdate(xrt_device* xdev) {
    auto* c = reinterpret_cast<Controller*>(xdev);
    rcvr::TrackingSnapshot t{};
    rcvr::BridgeController s{};
    ControllerState(c->hand, s, t);
    const int64_t now = rcvr::QpcNowNs();
    xrt_input* in = xdev->inputs;
    const bool left = c->hand == 0;
    auto button = [&](int i, uint32_t bit) {
        in[i].timestamp = now;
        in[i].value.boolean = (s.buttons & bit) != 0;
    };
    button(kFirstClick, left ? rc::kBtnX : rc::kBtnA);
    button(kFirstTouch, left ? rc::kTouchX : rc::kTouchA);
    button(kSecondClick, left ? rc::kBtnY : rc::kBtnB);
    button(kSecondTouch, left ? rc::kTouchY : rc::kTouchB);
    button(kMenuOrSystem, left ? rc::kBtnMenu : rc::kBtnSystem);
    button(kTriggerTouch, rc::kTouchTrigger);
    button(kStickClick, rc::kBtnThumbstick);
    button(kStickTouch, rc::kTouchThumbstick);
    button(kThumbrest, rc::kTouchThumbrest);
    in[kSqueeze].timestamp = now;
    in[kSqueeze].value.vec1.x = s.grip;
    in[kTriggerValue].timestamp = now;
    in[kTriggerValue].value.vec1.x = s.trigger;
    in[kStick].timestamp = now;
    in[kStick].value.vec2.x = s.thumbstick[0];
    in[kStick].value.vec2.y = s.thumbstick[1];
    return XRT_SUCCESS;
}

xrt_result_t ControllerTrackedPose(xrt_device* xdev, xrt_input_name name, int64_t, xrt_space_relation* out) {
    auto* c = reinterpret_cast<Controller*>(xdev);
    rcvr::TrackingSnapshot t{};
    rcvr::BridgeController s{};
    const bool connected = ControllerState(c->hand, s, t);
    const bool valid = connected && (s.flags & rc::kCtrlPoseValid);
    if (name == XRT_INPUT_TOUCH_GRIP_POSE) {
        *out = Relation(s.pose, valid);
    } else if (name == XRT_INPUT_TOUCH_AIM_POSE) {
        // The headset's own aim pose where it sent one; the grip pose otherwise (close enough for
        // menus, and what older headset apps give).
        *out = Relation(s.aimValid ? s.aim : s.pose, valid);
    } else {
        return XRT_ERROR_INPUT_UNSUPPORTED;
    }
    return XRT_SUCCESS;
}

xrt_result_t ControllerOutput(xrt_device* xdev, xrt_output_name name, const xrt_output_value* value) {
    auto* c = reinterpret_cast<Controller*>(xdev);
    if (name != XRT_OUTPUT_NAME_TOUCH_HAPTIC || value->type != XRT_OUTPUT_VALUE_TYPE_VIBRATION) return XRT_SUCCESS;
    const auto& v = value->vibration;
    // OpenXR's "minimum duration" and "unspecified frequency" come as 0: a short tap at the
    // controller's own frequency.
    const float seconds = v.duration_ns > 0 ? float(double(v.duration_ns) / 1e9) : 0.01f;
    Hub::Get().Bridge().PushHaptic(uint32_t(c->hand), seconds, v.frequency, v.amplitude);
    return XRT_SUCCESS;
}

xrt_result_t ControllerBattery(xrt_device* xdev, bool* present, bool* charging, float* charge) {
    auto* c = reinterpret_cast<Controller*>(xdev);
    rcvr::TrackingSnapshot t{};
    const bool have = Hub::Get().Tracking(t);
    const rcvr::BridgeBattery& b = t.battery[1 + c->hand];
    *present = have && (b.flags & rc::kBatteryValid);
    *charging = (b.flags & rc::kBatteryCharging) != 0;
    *charge = b.level;
    return XRT_SUCCESS;
}

void ControllerDestroy(xrt_device* xdev) { u_device_free(xdev); }

xrt_binding_input_pair g_simpleInputs[] = {
    {XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_TOUCH_TRIGGER_VALUE},
    {XRT_INPUT_SIMPLE_MENU_CLICK, XRT_INPUT_TOUCH_MENU_CLICK},
    {XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_TOUCH_GRIP_POSE},
    {XRT_INPUT_SIMPLE_AIM_POSE, XRT_INPUT_TOUCH_AIM_POSE},
};
xrt_binding_output_pair g_simpleOutputs[] = {{XRT_OUTPUT_NAME_SIMPLE_VIBRATION, XRT_OUTPUT_NAME_TOUCH_HAPTIC}};
xrt_binding_profile g_profiles[] = {{XRT_DEVICE_SIMPLE_CONTROLLER, g_simpleInputs, 4, g_simpleOutputs, 1}};

xrt_device* CreateController(xrt_tracking_origin* origin, int hand) {
    auto* c = U_DEVICE_ALLOCATE(Controller, U_DEVICE_ALLOC_TRACKING_NONE, kInputCount, 1);
    c->hand = hand;
    u_device_populate_function_pointers(&c->base, ControllerTrackedPose, ControllerDestroy);
    c->base.update_inputs = ControllerUpdate;
    c->base.set_output = ControllerOutput;
    c->base.get_battery_status = ControllerBattery;
    c->base.tracking_origin = origin;
    c->base.name = XRT_DEVICE_TOUCH_CONTROLLER;
    c->base.device_type = hand == 0 ? XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER : XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;
    c->base.supported.orientation_tracking = true;
    c->base.supported.position_tracking = true;
    c->base.supported.battery_status = true;
    std::snprintf(c->base.str, sizeof(c->base.str), "RinCast %s controller", hand == 0 ? "left" : "right");
    std::snprintf(c->base.serial, sizeof(c->base.serial), "RINCAST-%s", hand == 0 ? "LEFT" : "RIGHT");
    const xrt_input_name names[kInputCount] = {
        hand == 0 ? XRT_INPUT_TOUCH_X_CLICK : XRT_INPUT_TOUCH_A_CLICK,
        hand == 0 ? XRT_INPUT_TOUCH_X_TOUCH : XRT_INPUT_TOUCH_A_TOUCH,
        hand == 0 ? XRT_INPUT_TOUCH_Y_CLICK : XRT_INPUT_TOUCH_B_CLICK,
        hand == 0 ? XRT_INPUT_TOUCH_Y_TOUCH : XRT_INPUT_TOUCH_B_TOUCH,
        hand == 0 ? XRT_INPUT_TOUCH_MENU_CLICK : XRT_INPUT_TOUCH_SYSTEM_CLICK,
        XRT_INPUT_TOUCH_SQUEEZE_VALUE,
        XRT_INPUT_TOUCH_TRIGGER_TOUCH,
        XRT_INPUT_TOUCH_TRIGGER_VALUE,
        XRT_INPUT_TOUCH_THUMBSTICK_CLICK,
        XRT_INPUT_TOUCH_THUMBSTICK_TOUCH,
        XRT_INPUT_TOUCH_THUMBSTICK,
        XRT_INPUT_TOUCH_THUMBREST_TOUCH,
        XRT_INPUT_TOUCH_GRIP_POSE,
        XRT_INPUT_TOUCH_AIM_POSE,
    };
    for (int i = 0; i < kInputCount; i++) {
        c->base.inputs[i].name = names[i];
        c->base.inputs[i].active = true;
    }
    c->base.outputs[0].name = XRT_OUTPUT_NAME_TOUCH_HAPTIC;
    c->base.binding_profiles = g_profiles;
    c->base.binding_profile_count = 1;
    return &c->base;
}

// ---------------------------------------------------------------- the system

struct System {
    xrt_system_devices base;
    xrt_tracking_origin origin;
};

xrt_result_t GetRoles(xrt_system_devices*, xrt_system_roles* out) {
    xrt_system_roles roles = XRT_SYSTEM_ROLES_INIT;
    roles.generation_id = 1;
    roles.left = 1;  // xdevs: head, left, right
    roles.right = 2;
    roles.left_profile = XRT_DEVICE_TOUCH_CONTROLLER;
    roles.right_profile = XRT_DEVICE_TOUCH_CONTROLLER;
    *out = roles;
    return XRT_SUCCESS;
}

void DestroySystem(xrt_system_devices* xsysd) {
    auto* s = reinterpret_cast<System*>(xsysd);
    Hub::Get().StopHeartbeat();
    for (size_t i = 0; i < s->base.xdev_count; i++) xrt_device_destroy(&s->base.xdevs[i]);
    free(s);
}

}  // namespace

xrt_result_t CreateSystemDevices(xrt_session_event_sink* broadcast, xrt_system_devices** out_xsysd,
                                 xrt_space_overseer** out_xso) {
    auto* s = U_TYPED_CALLOC(System);
    s->base.get_roles = GetRoles;
    s->base.destroy = DestroySystem;
    s->origin.type = XRT_TRACKING_TYPE_OTHER;
    s->origin.initial_offset = {{0, 0, 0, 1}, {0, 0, 0}};
    std::snprintf(s->origin.name, sizeof(s->origin.name), "RinCast headset");

    xrt_device* head = CreateHmd(&s->origin);
    if (!head) {
        free(s);
        return XRT_ERROR_DEVICE_CREATION_FAILED;
    }
    s->base.xdevs[s->base.xdev_count++] = head;
    s->base.xdevs[s->base.xdev_count++] = CreateController(&s->origin, 0);
    s->base.xdevs[s->base.xdev_count++] = CreateController(&s->origin, 1);
    s->base.static_roles.head = head;

    // The headset tracks in its floor-level space: that is the stage, and "local" sits at
    // standing eye height above it, as Monado's other headsets have it.
    u_space_overseer* uso = u_space_overseer_create(broadcast);
    const xrt_pose localOffset = {{0, 0, 0, 1}, {0, 1.6f, 0}};
    u_space_overseer_legacy_setup(uso, s->base.xdevs, uint32_t(s->base.xdev_count), head, &localOffset, false, false);

    Hub::Get().StartHeartbeat();
    *out_xsysd = &s->base;
    *out_xso = reinterpret_cast<xrt_space_overseer*>(uso);
    return XRT_SUCCESS;
}

}  // namespace rcm
