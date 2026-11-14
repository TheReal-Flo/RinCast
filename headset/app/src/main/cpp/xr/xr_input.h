// Controller (and hand tracking) input via OpenXR actions.
#pragma once

#include <vector>

#include "../headsets/headset.h"
#include "rc/protocol.h"
#include "xr_context.h"

namespace rv {

struct HandInput {
    bool active = false;        // controller or tracked hand present
    bool isHand = false;        // tracked hand (no controller)
    bool held = false;          // a controller in this hand while its fingers are tracked too
    bool aimValid = false;
    bool gripValid = false;
    Pose aim;                   // pointing ray: origin + forward (-Z)
    Pose grip;
    Vec3 gripLinearVelocity, gripAngularVelocity;
    float trigger = 0;
    float squeeze = 0;
    Vec2 thumbstick;
    bool thumbstickClick = false;
    bool primary = false;       // A / X
    bool secondary = false;     // B / Y
    bool menu = false;          // left hand menu button
    bool touchPrimary = false, touchSecondary = false, touchThumbstick = false, touchTrigger = false,
         touchThumbrest = false;
    float pinch = 0;            // hand tracking pinch strength
    float pinchMiddle = 0, pinchRing = 0;
};

class XrInput {
   public:
    bool Init(XrContext& xr, const std::vector<ControllerProfile>& profiles);
    // Sync actions and locate poses at `time` (pass a future time for prediction).
    void Update(XrTime time);
    // Locate grip poses at another time (used for the stream's predicted poses).
    void Locate(XrTime time, HandInput hands[2]);
    // One controller's grip at `time`, from any thread (XrContext::LocateSpace locks).
    bool LocateGrip(int hand, XrTime time, Pose& pose, Vec3* linVel, Vec3* angVel) const {
        return m_gripSpace[hand] && m_xr->LocateSpace(m_gripSpace[hand], time, pose, linVel, angVel);
    }
    const HandInput& Hand(int i) const { return m_hands[i]; }
    rc::BatteryState ControllerBattery(int hand) const;
    // A tracked hand's joints at `time`, in app space. False if the hand isn't tracked then.
    bool LocateHandJoints(int hand, XrTime time, rc::Pose joints[rc::kHandJointCount]) const;
    // Tracks hands and controllers together. The Quest's runtime fails if body tracking is
    // running at the same time, so stop that first. Retries by itself while `on`.
    void SetSimultaneous(bool on);
    bool SimultaneousSupported() const { return m_resumeSimultaneous && m_pauseSimultaneous; }
    void Vibrate(int hand, float amplitude, float durationS, float frequency = XR_FREQUENCY_UNSPECIFIED);
    // Where the eyes look (XR_EXT_eye_gaze_interaction): one direction for both, in the head's
    // space (-z ahead). False while they aren't tracked, or the permission is missing.
    bool GazeSupported() const { return m_gazeSpace != XR_NULL_HANDLE; }
    bool LocateGaze(XrTime time, float direction[3]) const;

   private:
    void UpdateHandTracking(XrTime time);

    XrContext* m_xr = nullptr;
    XrActionSet m_set = XR_NULL_HANDLE;
    XrPath m_handPath[2] = {};
    XrAction m_aimPose = XR_NULL_HANDLE, m_gripPose = XR_NULL_HANDLE;
    XrAction m_trigger = XR_NULL_HANDLE, m_squeeze = XR_NULL_HANDLE, m_stick = XR_NULL_HANDLE;
    XrAction m_stickClick = XR_NULL_HANDLE, m_primary = XR_NULL_HANDLE, m_secondary = XR_NULL_HANDLE,
             m_menu = XR_NULL_HANDLE;
    XrAction m_touchPrimary = XR_NULL_HANDLE, m_touchSecondary = XR_NULL_HANDLE, m_touchStick = XR_NULL_HANDLE,
             m_touchTrigger = XR_NULL_HANDLE, m_touchThumbrest = XR_NULL_HANDLE;
    XrAction m_haptic = XR_NULL_HANDLE;
    XrAction m_battery[2] = {};
    XrSpace m_aimSpace[2] = {}, m_gripSpace[2] = {};
    XrAction m_gazePose = XR_NULL_HANDLE;
    XrSpace m_gazeSpace = XR_NULL_HANDLE;
    HandInput m_hands[2];

    // Hand tracking (XR_EXT_hand_tracking + XR_FB_hand_tracking_aim)
    XrHandTrackerEXT m_trackers[2] = {};
    PFN_xrLocateHandJointsEXT m_locateJoints = nullptr;
    // Where hand tracking needs a permission granted while the app runs (Android XR), creating the
    // trackers fails until then: tried again once a second.
    PFN_xrCreateHandTrackerEXT m_createTracker = nullptr;
    XrSession m_session = XR_NULL_HANDLE;
    int64_t m_handRetryNs = 0;
    bool CreateHandTrackers();

    // Simultaneous hands and controllers (XR_META_simultaneous_hands_and_controllers)
    PFN_xrResumeSimultaneousHandsAndControllersTrackingMETA m_resumeSimultaneous = nullptr;
    PFN_xrPauseSimultaneousHandsAndControllersTrackingMETA m_pauseSimultaneous = nullptr;
    std::vector<XrPath> m_controllerProfiles;  // those the runtime took
    bool m_simultaneous = false;
    int m_simultaneousRetry = 0;
};

}  // namespace rv
