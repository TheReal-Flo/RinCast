// Quest face and eye tracking, for avatars on the PC (VRCFaceTracking). Quest Pro tracks the face
// and eyes with its cameras; Quest 3 and 3S estimate the mouth and jaw from the microphone.
#pragma once

#include "rc/protocol.h"
#include "../../xr/xr_context.h"

namespace rv {

class QuestFace {
   public:
    ~QuestFace() { Stop(); }

    static bool Supported(const XrContext& xr);

    // Starts tracking when needed. False while nothing is tracked, or the permission is missing.
    bool Read(XrContext& xr, XrTime time, rc::FaceTrackingPayload& out);
    void Stop();

   private:
    XrFaceTracker2FB m_face = XR_NULL_HANDLE;
    XrEyeTrackerFB m_eyes = XR_NULL_HANDLE;
    PFN_xrDestroyFaceTracker2FB m_destroyFace = nullptr;
    PFN_xrGetFaceExpressionWeights2FB m_getWeights = nullptr;
    PFN_xrDestroyEyeTrackerFB m_destroyEyes = nullptr;
    PFN_xrGetEyeGazesFB m_getGazes = nullptr;
    int m_retryFrames = 0;
};

}  // namespace rv
