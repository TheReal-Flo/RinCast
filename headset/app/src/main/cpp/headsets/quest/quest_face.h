// Quest face and eye tracking, for avatars on the PC (VRCFaceTracking). Quest Pro tracks the face
// and eyes with its cameras; Quest 3 and 3S estimate the mouth and jaw from the microphone.
// The eyes also steer foveated encoding.
#pragma once

#include "rc/protocol.h"
#include "../../xr/xr_context.h"

namespace rv {

// The eye tracker, shared by face tracking and foveated encoding.
class QuestEyes {
   public:
    ~QuestEyes() { Stop(); }

    // Quest Pro (the extension is there on headsets without eye cameras too).
    static bool Supported(const XrContext& xr);

    // Starts tracking when needed; fails until the permission is granted. Directions in the
    // head's space; rc::GazeFlags in `valid`.
    bool Read(XrContext& xr, XrTime time, float gaze[2][3], uint8_t confidence[2], uint8_t& valid);
    void Stop();

   private:
    XrEyeTrackerFB m_eyes = XR_NULL_HANDLE;
    PFN_xrDestroyEyeTrackerFB m_destroyEyes = nullptr;
    PFN_xrGetEyeGazesFB m_getGazes = nullptr;
    int m_retryFrames = 0;
};

class QuestFace {
   public:
    explicit QuestFace(QuestEyes& eyes) : m_eyes(eyes) {}
    ~QuestFace() { Stop(); }

    static bool Supported(const XrContext& xr);

    // Starts tracking when needed. False while nothing is tracked, or the permission is missing.
    bool Read(XrContext& xr, XrTime time, rc::FaceTrackingPayload& out);
    void Stop();  // the face; the eyes are QuestEyes'

   private:
    QuestEyes& m_eyes;
    XrFaceTracker2FB m_face = XR_NULL_HANDLE;
    PFN_xrDestroyFaceTracker2FB m_destroyFace = nullptr;
    PFN_xrGetFaceExpressionWeights2FB m_getWeights = nullptr;
    int m_retryFrames = 0;
};

}  // namespace rv
