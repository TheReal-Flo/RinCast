// PICO face tracking (XR_BD_facial_simulation): the face from the cameras on the PICO 4 Pro and
// Enterprise, or the mouth from the microphone. Its 52 expressions (ARKit's) are sent as the
// Quest's 70, so the PC and VRCFaceTracking handle one set.
#pragma once

#include "../../xr/xr_context.h"
#include "rc/protocol.h"

namespace rv {

class PicoFace {
   public:
    ~PicoFace() { Stop(); }

    void Init(XrContext& xr);
    bool Supported() const { return m_supported; }
    bool Read(XrContext& xr, XrTime time, rc::FaceTrackingPayload& out);
    void Stop();

   private:
    bool m_supported = false;
    XrFaceTrackerBD m_face = XR_NULL_HANDLE;
    bool m_audioOnly = false;
    PFN_xrEnumerateFacialSimulationModesBD m_modes = nullptr;
    PFN_xrCreateFaceTrackerBD m_create = nullptr;
    PFN_xrDestroyFaceTrackerBD m_destroy = nullptr;
    PFN_xrGetFacialSimulationDataBD m_get = nullptr;
    int m_retryFrames = 0;
};

}  // namespace rv
