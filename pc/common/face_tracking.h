// Face and eye tracking from the headset, for VRCFaceTracking and any other program on the PC.
//
// RinCast Streamer keeps a named shared memory block, "RinCastFaceTracking" (in the session's
// own namespace, so programs of the same user see it), and updates it at the headset's frame
// rate while face tracking is on. Its layout is fixed below; every field sits at the offset
// given, for readers in other languages (see pc/vrcft-module).
//
// Reading: copy the block, and use it if `sequence` was even and the same before and after the
// copy (it's odd while being written). `updatedQpcNs` tells how fresh it is: older than half a
// second means the headset stopped sending, e.g. it disconnected or face tracking was turned off.
#pragma once

#include <cstdint>

namespace rcvr {

inline constexpr wchar_t kFaceTrackingName[] = L"RinCastFaceTracking";
inline constexpr uint32_t kFaceTrackingMagic = 0x54464352;  // "RCFT"
inline constexpr uint32_t kFaceTrackingVersion = 1;
inline constexpr int kFaceExpressions = 70;  // XR_FB_face_tracking2's set

enum FaceFlags : uint32_t {
    kFaceValid = 1 << 0,       // expressions are tracked
    kFaceFromAudio = 1 << 1,   // and come from the microphone (Quest 3 / 3S: mouth and jaw only)
    kLeftEyeValid = 1 << 2,    // eye gaze (Quest Pro)
    kRightEyeValid = 1 << 3,
};

#pragma pack(push, 4)
struct FaceTrackingState {
    uint32_t magic;                     // 0
    uint32_t version;                   // 4
    volatile int64_t sequence;          // 8: odd while being written
    int64_t updatedQpcNs;               // 16: QueryPerformanceCounter time of the update, in ns
    uint32_t flags;                     // 24: FaceFlags
    float upperFaceConfidence;          // 28: 0..1
    float lowerFaceConfidence;          // 32
    float expressions[kFaceExpressions];  // 36: 0..1, in the order of XrFaceExpression2FB
    float leftGaze[3];                  // 316: direction in the head's space: +x right, +y up, -z ahead
    float rightGaze[3];                 // 328
    float leftGazeConfidence;           // 340
    float rightGazeConfidence;          // 344
};                                      // 348 bytes
#pragma pack(pop)
static_assert(sizeof(FaceTrackingState) == 348);

}  // namespace rcvr
