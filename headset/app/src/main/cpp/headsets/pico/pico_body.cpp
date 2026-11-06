#include "pico_body.h"

#include <algorithm>
#include <cstring>

#include "rc/platform.h"

namespace rv {

namespace {

template <typename T>
T Proc(XrInstance instance, const char* name) {
    PFN_xrVoidFunction f = nullptr;
    xrGetInstanceProcAddr(instance, name, &f);
    return reinterpret_cast<T>(f);
}

using rc::BodyTracker;
constexpr int Role(BodyTracker t) { return int(t); }

// Same order as rc::BodyTracker. Each limb joint is the joint itself: the knee, the elbow, the
// shoulder; a foot is its ankle.
const XrBodyJointBD kJoints[rc::kBodyTrackerCount] = {
    XR_BODY_JOINT_PELVIS_BD,
    XR_BODY_JOINT_SPINE3_BD,
    XR_BODY_JOINT_LEFT_ANKLE_BD,
    XR_BODY_JOINT_RIGHT_ANKLE_BD,
    XR_BODY_JOINT_LEFT_KNEE_BD,
    XR_BODY_JOINT_RIGHT_KNEE_BD,
    XR_BODY_JOINT_LEFT_ELBOW_BD,
    XR_BODY_JOINT_RIGHT_ELBOW_BD,
    XR_BODY_JOINT_LEFT_SHOULDER_BD,
    XR_BODY_JOINT_RIGHT_SHOULDER_BD,
};

void Put(rc::Pose& p, const XrPosef& x) {
    p.orientation[0] = x.orientation.x;
    p.orientation[1] = x.orientation.y;
    p.orientation[2] = x.orientation.z;
    p.orientation[3] = x.orientation.w;
    p.position[0] = x.position.x;
    p.position[1] = x.position.y;
    p.position[2] = x.position.z;
}

bool Valid(const XrPosef& p) {
    const auto& q = p.orientation;
    return q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w > 0.5f;
}

}  // namespace

void PicoBody::Init(XrContext& xr) {
    if (xr.HasExtension(kBdMotionTrackingExtension) && xr.HasExtension(kPicoConfigurationExtension)) {
        m_connectState = Proc<PFN_xrGetMotionTrackerConnectStateBD>(xr.Instance(), "xrGetMotionTrackerConnectStateBD");
        m_locations = Proc<PFN_xrGetMotionTrackerLocationsBD>(xr.Instance(), "xrGetMotionTrackerLocationsBD");
        m_setConfig = Proc<PFN_xrSetConfigPICO>(xr.Instance(), "xrSetConfigPICO");
        m_trackers = m_connectState && m_locations && m_setConfig;
    }
    if (xr.HasExtension(XR_BD_BODY_TRACKING_EXTENSION_NAME)) {
        XrSystemBodyTrackingPropertiesBD body{XR_TYPE_SYSTEM_BODY_TRACKING_PROPERTIES_BD};
        XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
        props.next = &body;
        m_create = Proc<PFN_xrCreateBodyTrackerBD>(xr.Instance(), "xrCreateBodyTrackerBD");
        m_destroy = Proc<PFN_xrDestroyBodyTrackerBD>(xr.Instance(), "xrDestroyBodyTrackerBD");
        m_locate = Proc<PFN_xrLocateBodyJointsBD>(xr.Instance(), "xrLocateBodyJointsBD");
        m_bodySupported = XR_SUCCEEDED(xrGetSystemProperties(xr.Instance(), xr.System(), &props)) &&
                          body.supportsBodyTracking && m_create && m_destroy && m_locate;
    }
    // Only the registered extension's versions: older PICO builds have functions of the same
    // name that take other arguments.
    if (xr.HasExtension(XR_BD_BODY_TRACKING_AUXILIARY_METRICS_EXTENSION_NAME)) {
        m_state = Proc<PFN_xrGetBodyTrackingStateBD>(xr.Instance(), "xrGetBodyTrackingStateBD");
        m_calibrate = Proc<PFN_xrStartBodyTrackingCalibrationAppBD>(xr.Instance(), "xrStartBodyTrackingCalibrationAppBD");
    }
    RC_LOGI("PICO body: trackers %d, whole body %d, calibration %d", m_trackers, m_bodySupported, m_calibrate != nullptr);
}

void PicoBody::Stop() {
    if (m_body && m_destroy) m_destroy(m_body);
    m_body = XR_NULL_HANDLE;
    m_retryFrames = 0;
    // Turned on again, the trackers may be worn somewhere else.
    m_assigned = {};
}

bool PicoBody::Locate(XrContext& xr, XrTime time, rc::BodyTrackingPayload& out) {
    out = {};
    if (m_trackers) return LocateTrackers(xr, time, out);
    if (m_bodySupported) return LocateWholeBody(xr, time, out);
    return false;
}

int PicoBody::ConnectedTrackers(XrContext& xr) {
    if (!m_trackers) return 0;
    MotionTrackerConnectStateBD state{};
    if (XR_FAILED(m_connectState(xr.Instance(), &state))) return 0;
    return std::clamp(int(state.trackerCount), 0, kMotionTrackerUsed);
}

int PicoBody::AssignRole(XrContext& xr, XrTime time, const XrPosef& pose) const {
    Pose head;
    const bool haveHead = xr.LocateSpace(xr.ViewSpace(), time, head);
    const Vec3 p{pose.position.x, pose.position.y, pose.position.z};
    const float headY = haveHead ? head.position.y : 1.65f;
    // Which side of the head, looking ahead on the level.
    bool left = false;
    if (haveHead) {
        Vec3 right = head.Right();
        right.y = 0;
        left = Dot(p - head.position, Normalize(right)) < 0;
    }
    int order[rc::kBodyTrackerCount];
    int n = 0;
    const auto want = [&](BodyTracker t) { order[n++] = Role(t); };
    if (p.y < 0.3f) {
        want(left ? BodyTracker::LeftFoot : BodyTracker::RightFoot);
        want(left ? BodyTracker::RightFoot : BodyTracker::LeftFoot);
    } else if (p.y < 0.65f) {
        want(left ? BodyTracker::LeftKnee : BodyTracker::RightKnee);
        want(left ? BodyTracker::RightKnee : BodyTracker::LeftKnee);
    } else if (p.y > headY - 0.55f) {
        want(BodyTracker::Chest);
    }
    want(BodyTracker::Waist);
    want(BodyTracker::Chest);
    for (BodyTracker t : {BodyTracker::LeftFoot, BodyTracker::RightFoot, BodyTracker::LeftKnee, BodyTracker::RightKnee})
        want(t);
    for (int i = 0; i < n; i++) {
        bool taken = false;
        for (const Assigned& a : m_assigned) taken |= a.role == order[i];
        if (!taken) return order[i];
    }
    return -1;
}

bool PicoBody::LocateTrackers(XrContext& xr, XrTime time, rc::BodyTrackingPayload& out) {
    if (!m_originSet) {
        // Puts the trackers' poses in the stage, the app's space on PICO (see PreferStageSpace).
        const XrResult r = m_setConfig(xr.Session(), kPicoConfigTrackingOrigin, "1");
        if (XR_FAILED(r)) RC_LOGW("PICO tracking origin not set (%d)", int(r));
        m_originSet = true;
    }
    MotionTrackerConnectStateBD state{};
    if (XR_FAILED(m_connectState(xr.Instance(), &state))) return false;
    const int count = std::clamp(int(state.trackerCount), 0, kMotionTrackerUsed);
    for (int i = 0; i < count; i++) {
        MotionTrackerLocationsBD loc{};
        if (XR_FAILED(m_locations(xr.Instance(), time, &state.serials[i], &loc)) || !Valid(loc.localPose.pose)) continue;
        const std::string serial(state.serials[i].serial, strnlen(state.serials[i].serial, sizeof(state.serials[i].serial)));
        Assigned* slot = nullptr;
        for (Assigned& a : m_assigned)
            if (a.serial == serial) slot = &a;
        if (!slot) {
            for (Assigned& a : m_assigned)
                if (a.serial.empty() && !slot) slot = &a;
            if (!slot) continue;
            slot->role = AssignRole(xr, time, loc.localPose.pose);
            slot->serial = serial;
            RC_LOGI("Motion Tracker %s worn as body part %d", serial.c_str(), slot->role);
        }
        if (slot->role < 0) continue;
        out.validMask |= uint16_t(1u << slot->role);
        Put(out.poses[slot->role], loc.localPose.pose);
    }
    return out.validMask != 0;
}

bool PicoBody::LocateWholeBody(XrContext& xr, XrTime time, rc::BodyTrackingPayload& out) {
    if (!m_body) {
        if (m_retryFrames-- > 0) return false;
        m_retryFrames = 120;
        XrBodyTrackerCreateInfoBD ci{XR_TYPE_BODY_TRACKER_CREATE_INFO_BD};
        ci.jointSet = XR_BODY_JOINT_SET_FULL_BODY_JOINTS_BD;
        const XrResult r = m_create(xr.Session(), &ci, &m_body);
        if (XR_FAILED(r)) {
            RC_LOGW("PICO body tracking did not start (%d)", int(r));
            m_body = XR_NULL_HANDLE;
            return false;
        }
        RC_LOGI("PICO body tracking started");
    }
    XrBodyJointLocationBD joints[XR_BODY_JOINT_COUNT_BD];
    XrBodyJointLocationsBD locations{XR_TYPE_BODY_JOINT_LOCATIONS_BD};
    locations.jointLocationCount = XR_BODY_JOINT_COUNT_BD;
    locations.jointLocations = joints;
    XrBodyJointsLocateInfoBD info{XR_TYPE_BODY_JOINTS_LOCATE_INFO_BD};
    info.baseSpace = xr.AppSpace();
    info.time = time;
    if (XR_FAILED(m_locate(m_body, &info, &locations))) return false;
    constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    for (int i = 0; i < rc::kBodyTrackerCount; i++) {
        const XrBodyJointLocationBD& j = joints[kJoints[i]];
        if ((j.locationFlags & kValid) != kValid) continue;
        out.validMask |= uint16_t(1u << i);
        Put(out.poses[i], j.pose);
    }
    return out.validMask != 0;
}

std::string PicoBody::Status(XrContext& xr) {
    if (m_trackers) {
        const int n = ConnectedTrackers(xr);
        if (n == 0) return "Pair Motion Trackers in PICO's tracker app";
        return std::to_string(n) + (n == 1 ? " Motion Tracker" : " Motion Trackers") +
               ". Turn it on standing up";
    }
    if (m_state) {
        XrBodyTrackingStateBD state{XR_TYPE_BODY_TRACKING_STATE_BD};
        if (XR_SUCCEEDED(m_state(xr.Session(), &state))) {
            switch (state.message) {
                case XR_BODY_TRACKING_MESSAGE_TRACKER_NOT_CALIBRATED_BD: return "The Motion Trackers need calibrating";
                case XR_BODY_TRACKING_MESSAGE_TRACKER_NUM_NOT_ENOUGH_BD: return "Connect more Motion Trackers";
                default: break;
            }
            if (state.status == XR_BODY_TRACKING_STATUS_VALID_BD) return "Tracking the whole body";
        }
    }
    return "With PICO Motion Trackers";
}

void PicoBody::Calibrate(XrContext& xr) {
    if (!m_calibrate) return;
    const XrResult r = m_calibrate(xr.Session());
    if (XR_FAILED(r)) RC_LOGW("PICO calibration app did not open (%d)", int(r));
}

}  // namespace rv
