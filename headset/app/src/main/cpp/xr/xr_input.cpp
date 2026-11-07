#include "xr_input.h"

#include <algorithm>
#include <cstring>

#include "rc/platform.h"

namespace rv {

namespace {

XrAction CreateAction(XrActionSet set, XrActionType type, const char* name, const char* localized, XrPath* subPaths,
                      uint32_t subCount) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    strcpy(ci.actionName, name);
    strcpy(ci.localizedActionName, localized);
    ci.countSubactionPaths = subCount;
    ci.subactionPaths = subPaths;
    XrAction action = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateAction(set, &ci, &action))) RC_LOGE("xrCreateAction %s failed", name);
    return action;
}

XrPath Path(XrInstance instance, const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(instance, s, &p);
    return p;
}

}  // namespace

bool XrInput::Init(XrContext& xr, const std::vector<ControllerProfile>& profiles) {
    m_xr = &xr;
    XrInstance inst = xr.Instance();
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(si.actionSetName, "rincast");
    strcpy(si.localizedActionSetName, "RinCast");
    if (XR_FAILED(xrCreateActionSet(inst, &si, &m_set))) return false;
    m_handPath[0] = Path(inst, "/user/hand/left");
    m_handPath[1] = Path(inst, "/user/hand/right");

    m_aimPose = CreateAction(m_set, XR_ACTION_TYPE_POSE_INPUT, "aim", "Aim", m_handPath, 2);
    m_gripPose = CreateAction(m_set, XR_ACTION_TYPE_POSE_INPUT, "grip", "Grip", m_handPath, 2);
    m_trigger = CreateAction(m_set, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", m_handPath, 2);
    m_squeeze = CreateAction(m_set, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Squeeze", m_handPath, 2);
    m_stick = CreateAction(m_set, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbstick", m_handPath, 2);
    m_stickClick = CreateAction(m_set, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click", "Thumbstick click", m_handPath, 2);
    m_primary = CreateAction(m_set, XR_ACTION_TYPE_BOOLEAN_INPUT, "primary", "A / X", m_handPath, 2);
    m_secondary = CreateAction(m_set, XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary", "B / Y", m_handPath, 2);
    m_menu = CreateAction(m_set, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", m_handPath, 2);
    m_touchPrimary = CreateAction(m_set, XR_ACTION_TYPE_BOOLEAN_INPUT, "touch_primary", "Touch A / X", m_handPath, 2);
    m_touchSecondary = CreateAction(m_set, XR_ACTION_TYPE_BOOLEAN_INPUT, "touch_secondary", "Touch B / Y", m_handPath, 2);
    m_touchStick = CreateAction(m_set, XR_ACTION_TYPE_BOOLEAN_INPUT, "touch_thumbstick", "Touch thumbstick", m_handPath, 2);
    m_touchTrigger = CreateAction(m_set, XR_ACTION_TYPE_BOOLEAN_INPUT, "touch_trigger", "Touch trigger", m_handPath, 2);
    m_touchThumbrest = CreateAction(m_set, XR_ACTION_TYPE_BOOLEAN_INPUT, "touch_thumbrest", "Touch thumbrest", m_handPath, 2);
    m_haptic = CreateAction(m_set, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Haptic", m_handPath, 2);

    // Eye gaze, where the runtime has it and the headset can track the eyes.
    if (xr.HasExtension(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) {
        XrSystemEyeGazeInteractionPropertiesEXT gaze{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
        XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
        props.next = &gaze;
        if (XR_SUCCEEDED(xrGetSystemProperties(inst, xr.System(), &props)) && gaze.supportsEyeGazeInteraction) {
            m_gazePose = CreateAction(m_set, XR_ACTION_TYPE_POSE_INPUT, "eye_gaze", "Eye gaze", nullptr, 0);
            const XrActionSuggestedBinding b{m_gazePose, Path(inst, "/user/eyes_ext/input/gaze_ext/pose")};
            XrInteractionProfileSuggestedBinding eb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            eb.interactionProfile = Path(inst, "/interaction_profiles/ext/eye_gaze_interaction");
            eb.suggestedBindings = &b;
            eb.countSuggestedBindings = 1;
            if (XR_FAILED(xrSuggestInteractionProfileBindings(inst, &eb))) {
                RC_LOGW("Eye gaze bindings rejected");
                m_gazePose = XR_NULL_HANDLE;
            }
        }
    }

    // Every family's controllers have the Touch layout: A/B and X/Y, a trigger, a grip and a stick.
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    for (const ControllerProfile& profile : profiles) {
        if (profile.extension && !xr.HasExtension(profile.extension)) continue;
        std::vector<XrActionSuggestedBinding> b;
        auto bind = [&](XrAction a, const char* path) { b.push_back({a, Path(inst, path)}); };
        for (const char* side : {"left", "right"}) {
            const std::string h = std::string("/user/hand/") + side;
            bind(m_aimPose, (h + "/input/aim/pose").c_str());
            bind(m_gripPose, (h + "/input/grip/pose").c_str());
            bind(m_trigger, (h + "/input/trigger/value").c_str());
            bind(m_squeeze, (h + "/input/squeeze/value").c_str());
            bind(m_stick, (h + "/input/thumbstick").c_str());
            bind(m_stickClick, (h + "/input/thumbstick/click").c_str());
            if (profile.touchSensors) {
                bind(m_touchStick, (h + "/input/thumbstick/touch").c_str());
                bind(m_touchTrigger, (h + "/input/trigger/touch").c_str());
            }
            if (profile.thumbrest) bind(m_touchThumbrest, (h + "/input/thumbrest/touch").c_str());
            bind(m_haptic, (h + "/output/haptic").c_str());
        }
        bind(m_primary, "/user/hand/left/input/x/click");
        bind(m_secondary, "/user/hand/left/input/y/click");
        bind(m_menu, "/user/hand/left/input/menu/click");
        bind(m_primary, "/user/hand/right/input/a/click");
        bind(m_secondary, "/user/hand/right/input/b/click");
        if (profile.touchSensors) {
            bind(m_touchPrimary, "/user/hand/left/input/x/touch");
            bind(m_touchSecondary, "/user/hand/left/input/y/touch");
            bind(m_touchPrimary, "/user/hand/right/input/a/touch");
            bind(m_touchSecondary, "/user/hand/right/input/b/touch");
        }
        const XrPath path = Path(inst, profile.path);
        sb.interactionProfile = path;
        sb.suggestedBindings = b.data();
        sb.countSuggestedBindings = uint32_t(b.size());
        if (XR_FAILED(xrSuggestInteractionProfileBindings(inst, &sb))) RC_LOGW("Bindings for %s rejected", profile.path);
        else m_controllerProfiles.push_back(path);
    }

    // Hand tracking via the Khronos simple profile (aim pose + select = pinch).
    std::vector<XrActionSuggestedBinding> hb;
    for (const char* side : {"left", "right"}) {
        const std::string h = std::string("/user/hand/") + side;
        hb.push_back({m_aimPose, Path(inst, (h + "/input/aim/pose").c_str())});
        hb.push_back({m_gripPose, Path(inst, (h + "/input/grip/pose").c_str())});
        hb.push_back({m_trigger, Path(inst, (h + "/input/select/click").c_str())});
    }
    sb.interactionProfile = Path(inst, "/interaction_profiles/khr/simple_controller");
    sb.suggestedBindings = hb.data();
    sb.countSuggestedBindings = uint32_t(hb.size());
    xrSuggestInteractionProfileBindings(inst, &sb);

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &m_set;
    if (XR_FAILED(xrAttachSessionActionSets(xr.Session(), &ai))) return false;

    for (int i = 0; i < 2; i++) {
        XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.poseInActionSpace.orientation.w = 1;
        sci.subactionPath = m_handPath[i];
        sci.action = m_aimPose;
        xrCreateActionSpace(xr.Session(), &sci, &m_aimSpace[i]);
        sci.action = m_gripPose;
        xrCreateActionSpace(xr.Session(), &sci, &m_gripSpace[i]);
    }
    if (m_gazePose) {
        XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.poseInActionSpace.orientation.w = 1;
        sci.action = m_gazePose;
        if (XR_SUCCEEDED(xrCreateActionSpace(xr.Session(), &sci, &m_gazeSpace))) RC_LOGI("Eye gaze available");
        else m_gazeSpace = XR_NULL_HANDLE;
    }

    if (xr.HandTracking() && xr.HasExtension(XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME) &&
        xr.HasExtension(XR_META_DETACHED_CONTROLLERS_EXTENSION_NAME)) {
        XrSystemSimultaneousHandsAndControllersPropertiesMETA simultaneous{XR_TYPE_SYSTEM_SIMULTANEOUS_HANDS_AND_CONTROLLERS_PROPERTIES_META};
        XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
        props.next = &simultaneous;
        if (XR_SUCCEEDED(xrGetSystemProperties(inst, xr.System(), &props)) && simultaneous.supportsSimultaneousHandsAndControllers) {
            xrGetInstanceProcAddr(inst, "xrResumeSimultaneousHandsAndControllersTrackingMETA",
                                  reinterpret_cast<PFN_xrVoidFunction*>(&m_resumeSimultaneous));
            xrGetInstanceProcAddr(inst, "xrPauseSimultaneousHandsAndControllersTrackingMETA",
                                  reinterpret_cast<PFN_xrVoidFunction*>(&m_pauseSimultaneous));
        }
    }

    if (xr.HandTracking()) {
        auto create = reinterpret_cast<PFN_xrCreateHandTrackerEXT>([&] {
            PFN_xrVoidFunction f = nullptr;
            xrGetInstanceProcAddr(inst, "xrCreateHandTrackerEXT", &f);
            return f;
        }());
        PFN_xrVoidFunction f = nullptr;
        xrGetInstanceProcAddr(inst, "xrLocateHandJointsEXT", &f);
        m_locateJoints = reinterpret_cast<PFN_xrLocateHandJointsEXT>(f);
        m_createTracker = create;
        m_session = xr.Session();
        if (!CreateHandTrackers()) RC_LOGI("Hand tracking isn't available yet (permission?); trying again");
    }
    return true;
}

bool XrInput::CreateHandTrackers() {
    for (int i = 0; i < 2 && m_createTracker; i++) {
        if (m_trackers[i]) continue;
        XrHandTrackerCreateInfoEXT ci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        ci.hand = i == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        if (XR_FAILED(m_createTracker(m_session, &ci, &m_trackers[i]))) m_trackers[i] = XR_NULL_HANDLE;
    }
    return m_trackers[0] && m_trackers[1];
}

static bool GetBool(XrSession s, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(s, &gi, &st)) && st.isActive && st.currentState;
}

static float GetFloat(XrSession s, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(s, &gi, &st)) && st.isActive ? st.currentState : 0.f;
}

void XrInput::Update(XrTime time) {
    XrActiveActionSet active{m_set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    xrSyncActions(m_xr->Session(), &si);

    const XrSession s = m_xr->Session();
    for (int i = 0; i < 2; i++) {
        HandInput& h = m_hands[i];
        const XrPath sub = m_handPath[i];
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = m_aimPose;
        gi.subactionPath = sub;
        XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
        xrGetActionStatePose(s, &gi, &ps);
        h.active = ps.isActive;
        h.trigger = GetFloat(s, m_trigger, sub);
        h.squeeze = GetFloat(s, m_squeeze, sub);
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        gi.action = m_stick;
        xrGetActionStateVector2f(s, &gi, &v);
        h.thumbstick = v.isActive ? Vec2{v.currentState.x, v.currentState.y} : Vec2{};
        h.thumbstickClick = GetBool(s, m_stickClick, sub);
        h.primary = GetBool(s, m_primary, sub);
        h.secondary = GetBool(s, m_secondary, sub);
        h.menu = GetBool(s, m_menu, sub);
        h.touchPrimary = GetBool(s, m_touchPrimary, sub);
        h.touchSecondary = GetBool(s, m_touchSecondary, sub);
        h.touchThumbstick = GetBool(s, m_touchStick, sub);
        h.touchTrigger = GetBool(s, m_touchTrigger, sub);
        h.touchThumbrest = GetBool(s, m_touchThumbrest, sub);
        h.held = false;
        if (m_simultaneous) {
            // A free hand gets another profile, even with a controller lying on the desk.
            XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
            h.held = XR_SUCCEEDED(xrGetCurrentInteractionProfile(s, sub, &profile)) &&
                     std::find(m_controllerProfiles.begin(), m_controllerProfiles.end(), profile.interactionProfile) != m_controllerProfiles.end();
        }
    }
    Locate(time, m_hands);
    UpdateHandTracking(time);
}

bool XrInput::LocateGaze(XrTime time, float direction[3]) const {
    if (!m_gazeSpace) return false;
    XrSpaceLocation l{XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    if (XR_FAILED(xrLocateSpace(m_gazeSpace, m_xr->ViewSpace(), time, &l)) || (l.locationFlags & need) != need) return false;
    const XrQuaternionf& q = l.pose.orientation;  // -z rotated by it
    direction[0] = -2.f * (q.w * q.y + q.x * q.z);
    direction[1] = 2.f * (q.w * q.x - q.y * q.z);
    direction[2] = -1.f + 2.f * (q.x * q.x + q.y * q.y);
    return true;
}

void XrInput::Locate(XrTime time, HandInput hands[2]) {
    for (int i = 0; i < 2; i++) {
        HandInput& h = hands[i];
        h.aimValid = m_xr->LocateSpace(m_aimSpace[i], time, h.aim);
        h.gripValid = m_xr->LocateSpace(m_gripSpace[i], time, h.grip, &h.gripLinearVelocity, &h.gripAngularVelocity);
    }
}

void XrInput::UpdateHandTracking(XrTime time) {
    if (!m_locateJoints) return;
    if ((!m_trackers[0] || !m_trackers[1]) && m_createTracker) {
        const int64_t now = rc::NowNs();
        if (now - m_handRetryNs > 1'000'000'000LL) {
            m_handRetryNs = now;
            if (CreateHandTrackers()) RC_LOGI("Hand tracking started");
        }
    }
    for (int i = 0; i < 2; i++) {
        if (!m_trackers[i]) continue;
        if (m_hands[i].held) {  // the controller is what plays; only its fingers are wanted
            m_hands[i].isHand = false;
            continue;
        }
        XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
        XrHandJointLocationsEXT locs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locs.jointCount = XR_HAND_JOINT_COUNT_EXT;
        locs.jointLocations = joints;
        XrHandTrackingAimStateFB aim{XR_TYPE_HAND_TRACKING_AIM_STATE_FB};
        if (m_xr->HasExtension(XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME)) locs.next = &aim;
        XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        li.baseSpace = m_xr->AppSpace();
        li.time = time;
        if (XR_FAILED(m_locateJoints(m_trackers[i], &li, &locs)) || !locs.isActive) {
            m_hands[i].isHand = false;
            continue;
        }
        HandInput& h = m_hands[i];
        h.isHand = true;
        h.active = true;
        if (aim.status & XR_HAND_TRACKING_AIM_VALID_BIT_FB) {
            h.aim = ToPose(aim.aimPose);
            h.aimValid = true;
            h.pinch = aim.pinchStrengthIndex;
            h.pinchMiddle = aim.pinchStrengthMiddle;
            h.pinchRing = aim.pinchStrengthRing;
            h.trigger = aim.pinchStrengthIndex;
            h.menu = (aim.status & XR_HAND_TRACKING_AIM_MENU_PRESSED_BIT_FB) != 0;
        } else if (!m_xr->HasExtension(XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME)) {
            // Without the runtime's pinch (PICO): from how close each fingertip is to the thumb's.
            constexpr XrSpaceLocationFlags kPos = XR_SPACE_LOCATION_POSITION_VALID_BIT;
            const auto pinch = [&](int tip) {
                const auto& a = joints[XR_HAND_JOINT_THUMB_TIP_EXT];
                const auto& b = joints[tip];
                if (!(a.locationFlags & kPos) || !(b.locationFlags & kPos)) return 0.f;
                const Vec3 d{a.pose.position.x - b.pose.position.x, a.pose.position.y - b.pose.position.y,
                             a.pose.position.z - b.pose.position.z};
                return Clamp01(1.f - (Length(d) - 0.015f) / 0.03f);  // touching at 1.5 cm, open from 4.5 cm
            };
            h.pinch = pinch(XR_HAND_JOINT_INDEX_TIP_EXT);
            h.pinchMiddle = pinch(XR_HAND_JOINT_MIDDLE_TIP_EXT);
            h.pinchRing = pinch(XR_HAND_JOINT_RING_TIP_EXT);
            h.trigger = h.pinch;
        }
        const auto& palm = joints[XR_HAND_JOINT_PALM_EXT];
        if (palm.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
            h.grip = ToPose(palm.pose);
            h.gripValid = true;
        }
    }
}

bool XrInput::LocateHandJoints(int hand, XrTime time, rc::Pose out[rc::kHandJointCount]) const {
    static_assert(rc::kHandJointCount == XR_HAND_JOINT_COUNT_EXT);
    if (!m_locateJoints || !m_trackers[hand]) return false;
    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
    XrHandJointLocationsEXT locs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
    locs.jointCount = XR_HAND_JOINT_COUNT_EXT;
    locs.jointLocations = joints;
    XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
    li.baseSpace = m_xr->AppSpace();
    li.time = time;
    if (XR_FAILED(m_locateJoints(m_trackers[hand], &li, &locs)) || !locs.isActive) return false;
    constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    for (int i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++) {
        if ((joints[i].locationFlags & kValid) != kValid) return false;
        const XrPosef& p = joints[i].pose;
        out[i] = {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}};
    }
    return true;
}

void XrInput::SetSimultaneous(bool on) {
    if (on == m_simultaneous) {
        m_simultaneousRetry = 0;
        return;
    }
    if (!m_resumeSimultaneous || !m_pauseSimultaneous) return;
    if (!on) {
        XrSimultaneousHandsAndControllersTrackingPauseInfoMETA info{XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_PAUSE_INFO_META};
        m_pauseSimultaneous(m_xr->Session(), &info);
        m_simultaneous = false;
        RC_LOGI("Simultaneous hands and controllers off");
        return;
    }
    if (m_simultaneousRetry > 0) {
        m_simultaneousRetry--;
        return;
    }
    XrSimultaneousHandsAndControllersTrackingResumeInfoMETA info{XR_TYPE_SIMULTANEOUS_HANDS_AND_CONTROLLERS_TRACKING_RESUME_INFO_META};
    const XrResult r = m_resumeSimultaneous(m_xr->Session(), &info);
    m_simultaneous = XR_SUCCEEDED(r);
    if (m_simultaneous) RC_LOGI("Simultaneous hands and controllers on");
    else {
        RC_LOGW("Simultaneous hands and controllers failed: %d", int(r));
        m_simultaneousRetry = 120;
    }
}

void XrInput::Vibrate(int hand, float amplitude, float durationS, float frequency) {
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amplitude;
    v.duration = XrDuration(durationS * 1e9);
    v.frequency = frequency;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = m_haptic;
    hi.subactionPath = m_handPath[hand];
    xrApplyHapticFeedback(m_xr->Session(), &hi, reinterpret_cast<XrHapticBaseHeader*>(&v));
}

}  // namespace rv
