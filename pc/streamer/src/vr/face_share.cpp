#include "face_share.h"

#include <cstring>

#include "rc/platform.h"

namespace rcs {

FaceShare& FaceShare::Get() {
    static FaceShare share;
    return share;
}

FaceShare::~FaceShare() {
    if (m_state) UnmapViewOfFile(m_state);
    if (m_mapping) CloseHandle(m_mapping);
}

bool FaceShare::Open() {
    if (m_state) return true;
    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(rcvr::FaceTrackingState),
                                   rcvr::kFaceTrackingName);
    if (!m_mapping) return false;
    m_state = static_cast<rcvr::FaceTrackingState*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(rcvr::FaceTrackingState)));
    if (!m_state) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
        return false;
    }
    std::memset(m_state, 0, sizeof(*m_state));
    m_state->magic = rcvr::kFaceTrackingMagic;
    m_state->version = rcvr::kFaceTrackingVersion;
    RC_LOGI("Face tracking: shared with other programs as \"RinCastFaceTracking\"");
    return true;
}

void FaceShare::Publish(const rc::FaceTrackingPayload& face) {
    std::lock_guard lock(m_mutex);
    if (Open()) Write(&face);
}

void FaceShare::Clear() {
    std::lock_guard lock(m_mutex);
    if (m_state) Write(nullptr);
}

void FaceShare::Write(const rc::FaceTrackingPayload* face) {
    auto& s = *m_state;
    InterlockedIncrement64(&s.sequence);  // odd: being written
    MemoryBarrier();
    s.updatedQpcNs = rc::NowNs();
    s.flags = 0;
    if (face) {
        if (face->flags & rc::kFaceTrackedValid) s.flags |= rcvr::kFaceValid;
        if (face->flags & rc::kFaceTrackedFromAudio) s.flags |= rcvr::kFaceFromAudio;
        if (face->flags & rc::kFaceLeftEyeValid) s.flags |= rcvr::kLeftEyeValid;
        if (face->flags & rc::kFaceRightEyeValid) s.flags |= rcvr::kRightEyeValid;
        s.upperFaceConfidence = face->upperFaceConfidence / 255.f;
        s.lowerFaceConfidence = face->lowerFaceConfidence / 255.f;
        for (int i = 0; i < rcvr::kFaceExpressions; i++) s.expressions[i] = face->expressions[i] / 255.f;
        std::memcpy(s.leftGaze, face->gaze[0], sizeof(s.leftGaze));
        std::memcpy(s.rightGaze, face->gaze[1], sizeof(s.rightGaze));
        s.leftGazeConfidence = face->gazeConfidence[0] / 255.f;
        s.rightGazeConfidence = face->gazeConfidence[1] / 255.f;
    }
    MemoryBarrier();
    InterlockedIncrement64(&s.sequence);  // even again: complete
}

}  // namespace rcs
