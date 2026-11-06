// PICO Motion Trackers, as SteamVR trackers.
//
// Two ways, the first the headset has:
// - Trackers: each Motion Tracker as it is, through PICO's XR_BD_motion_tracking. The trackers
//   don't say where they're worn, so each gets a body part by where it is the first time it's
//   seen: on the floor a foot, at knee height a knee, else the waist or the chest.
// - Body: the whole body, estimated from the trackers by the headset (XR_BD_body_tracking),
//   after calibrating in PICO's Motion Tracker app.
#pragma once

#include <array>
#include <string>

#include "../../xr/xr_context.h"
#include "pico_extensions.h"
#include "rc/protocol.h"

namespace rv {

class PicoBody {
   public:
    ~PicoBody() { Stop(); }

    void Init(XrContext& xr);
    bool Supported() const { return m_trackers || m_bodySupported; }
    bool Locate(XrContext& xr, XrTime time, rc::BodyTrackingPayload& out);
    void Stop();

    std::string Status(XrContext& xr);
    bool CanCalibrate() const { return m_calibrate != nullptr; }
    void Calibrate(XrContext& xr);

   private:
    bool LocateTrackers(XrContext& xr, XrTime time, rc::BodyTrackingPayload& out);
    bool LocateWholeBody(XrContext& xr, XrTime time, rc::BodyTrackingPayload& out);
    int AssignRole(XrContext& xr, XrTime time, const XrPosef& pose) const;
    int ConnectedTrackers(XrContext& xr);

    // Trackers
    PFN_xrGetMotionTrackerConnectStateBD m_connectState = nullptr;
    PFN_xrGetMotionTrackerLocationsBD m_locations = nullptr;
    PFN_xrSetConfigPICO m_setConfig = nullptr;
    bool m_trackers = false;
    bool m_originSet = false;
    struct Assigned {
        std::string serial;
        int role = -1;  // rc::BodyTracker
    };
    std::array<Assigned, kMotionTrackerMax> m_assigned{};

    // Whole body
    bool m_bodySupported = false;
    XrBodyTrackerBD m_body = XR_NULL_HANDLE;
    PFN_xrCreateBodyTrackerBD m_create = nullptr;
    PFN_xrDestroyBodyTrackerBD m_destroy = nullptr;
    PFN_xrLocateBodyJointsBD m_locate = nullptr;
    PFN_xrGetBodyTrackingStateBD m_state = nullptr;
    PFN_xrStartBodyTrackingCalibrationAppBD m_calibrate = nullptr;
    int m_retryFrames = 0;
};

}  // namespace rv
