// Quest body tracking, for the emulated SteamVR trackers.
#pragma once

#include <cstdint>

#include "rc/protocol.h"
#include "../../xr/xr_context.h"

namespace rv {

class QuestBody {
   public:
    ~QuestBody() { Stop(); }

    static bool Supported(const XrContext& xr);

    // Starts tracking when needed. False while the body isn't tracked, or the permission is missing.
    bool Locate(XrContext& xr, XrTime time, rc::BodyTrackingPayload& out);
    void Stop();

   private:
    XrBodyTrackerFB m_tracker = XR_NULL_HANDLE;
    PFN_xrDestroyBodyTrackerFB m_destroy = nullptr;
    PFN_xrLocateBodyJointsFB m_locate = nullptr;
    int m_retryFrames = 0;
};

}  // namespace rv
