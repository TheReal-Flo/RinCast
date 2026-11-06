// Publishes the headset's face and eye tracking in shared memory (pc/common/face_tracking.h),
// where VRCFaceTracking's RinCast module reads it.
#pragma once

#include <windows.h>

#include <mutex>

#include "face_tracking.h"
#include "rc/protocol.h"

namespace rcs {

class FaceShare {
   public:
    static FaceShare& Get();
    // Network thread; creates the block on the first call.
    void Publish(const rc::FaceTrackingPayload& face);
    // The headset stopped (disconnected): readers see nothing tracked.
    void Clear();

   private:
    ~FaceShare();
    bool Open();
    void Write(const rc::FaceTrackingPayload* face);

    std::mutex m_mutex;
    HANDLE m_mapping = nullptr;
    rcvr::FaceTrackingState* m_state = nullptr;
};

}  // namespace rcs
