#include "quest_body.h"

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

// Same order as rc::BodyTracker. Each limb joint starts at the named body part: the lower
// arm at the elbow, the lower leg at the knee, the upper arm at the shoulder.
const XrFullBodyJointMETA kJoints[rc::kBodyTrackerCount] = {
    XR_FULL_BODY_JOINT_HIPS_META,
    XR_FULL_BODY_JOINT_CHEST_META,
    XR_FULL_BODY_JOINT_LEFT_FOOT_ANKLE_META,
    XR_FULL_BODY_JOINT_RIGHT_FOOT_ANKLE_META,
    XR_FULL_BODY_JOINT_LEFT_LOWER_LEG_META,
    XR_FULL_BODY_JOINT_RIGHT_LOWER_LEG_META,
    XR_FULL_BODY_JOINT_LEFT_ARM_LOWER_META,
    XR_FULL_BODY_JOINT_RIGHT_ARM_LOWER_META,
    XR_FULL_BODY_JOINT_LEFT_ARM_UPPER_META,
    XR_FULL_BODY_JOINT_RIGHT_ARM_UPPER_META,
};

}  // namespace

bool QuestBody::Supported(const XrContext& xr) {
    return xr.HasExtension(XR_FB_BODY_TRACKING_EXTENSION_NAME) && xr.HasExtension(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME);
}

void QuestBody::Stop() {
    if (m_tracker && m_destroy) m_destroy(m_tracker);
    m_tracker = XR_NULL_HANDLE;
    m_retryFrames = 0;
}

bool QuestBody::Locate(XrContext& xr, XrTime time, rc::BodyTrackingPayload& out) {
    if (!m_tracker) {
        // Fails until the permission is granted.
        if (m_retryFrames-- > 0) return false;
        m_retryFrames = 120;
        auto create = Proc<PFN_xrCreateBodyTrackerFB>(xr.Instance(), "xrCreateBodyTrackerFB");
        m_destroy = Proc<PFN_xrDestroyBodyTrackerFB>(xr.Instance(), "xrDestroyBodyTrackerFB");
        m_locate = Proc<PFN_xrLocateBodyJointsFB>(xr.Instance(), "xrLocateBodyJointsFB");
        if (!create || !m_destroy || !m_locate) return false;
        XrBodyTrackerCreateInfoFB ci{XR_TYPE_BODY_TRACKER_CREATE_INFO_FB};
        ci.bodyJointSet = XR_BODY_JOINT_SET_FULL_BODY_META;
        const XrResult r = create(xr.Session(), &ci, &m_tracker);
        if (XR_FAILED(r)) {
            RC_LOGW("Body tracking did not start (%d)", int(r));
            m_tracker = XR_NULL_HANDLE;
            return false;
        }
        RC_LOGI("Body tracking started");
        // Uses the Quest 3's downward cameras for the upper body, where it can.
        if (xr.HasExtension(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME)) {
            if (auto fidelity = Proc<PFN_xrRequestBodyTrackingFidelityMETA>(xr.Instance(), "xrRequestBodyTrackingFidelityMETA"))
                fidelity(m_tracker, XR_BODY_TRACKING_FIDELITY_HIGH_META);
        }
    }
    XrBodyJointLocationFB joints[XR_FULL_BODY_JOINT_COUNT_META];
    XrBodyJointLocationsFB locations{XR_TYPE_BODY_JOINT_LOCATIONS_FB};
    locations.jointCount = XR_FULL_BODY_JOINT_COUNT_META;
    locations.jointLocations = joints;
    XrBodyJointsLocateInfoFB info{XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB};
    info.baseSpace = xr.AppSpace();  // the same space as the head and the controllers
    info.time = time;
    if (XR_FAILED(m_locate(m_tracker, &info, &locations)) || !locations.isActive) return false;
    out = {};
    constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    for (int i = 0; i < rc::kBodyTrackerCount; i++) {
        const XrBodyJointLocationFB& j = joints[kJoints[i]];
        if ((j.locationFlags & kValid) != kValid) continue;
        out.validMask |= uint16_t(1u << i);
        rc::Pose& p = out.poses[i];
        p.orientation[0] = j.pose.orientation.x;
        p.orientation[1] = j.pose.orientation.y;
        p.orientation[2] = j.pose.orientation.z;
        p.orientation[3] = j.pose.orientation.w;
        p.position[0] = j.pose.position.x;
        p.position[1] = j.pose.position.y;
        p.position[2] = j.pose.position.z;
    }
    return out.validMask != 0;
}

}  // namespace rv
