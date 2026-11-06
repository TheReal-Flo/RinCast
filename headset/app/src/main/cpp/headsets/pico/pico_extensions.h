// PICO's own OpenXR functions for the Motion Trackers, which aren't in the Khronos headers.
// The declarations follow ALVR's (alvr/client_openxr/src/extra_extensions/motion_tracking_bd.rs),
// which reads the trackers this way on PICO 4 and PICO 4 Ultra with the Khronos loader.
#pragma once

#include <cstdint>

#include "../../xr/xr_context.h"

namespace rv {

inline constexpr const char* kBdMotionTrackingExtension = "XR_BD_motion_tracking";
inline constexpr const char* kPicoConfigurationExtension = "XR_PICO_configuration";

inline constexpr int kMotionTrackerMax = 6;
// PICO doesn't connect more than three trackers for now.
inline constexpr int kMotionTrackerUsed = 3;

struct MotionTrackerSerialBD {
    char serial[24];
};

struct MotionTrackerConnectStateBD {
    int32_t trackerCount;
    MotionTrackerSerialBD serials[kMotionTrackerMax];
};

struct MotionTrackerLocationBD {
    XrPosef pose;
    XrVector3f angularVelocity;
    XrVector3f linearVelocity;
    XrVector3f angularAcceleration;
    XrVector3f linearAcceleration;
};

struct MotionTrackerLocationsBD {
    MotionTrackerSerialBD serial;
    MotionTrackerLocationBD localPose;  // in the tracking origin's space, on the floor (see below)
    int32_t confidence;
    MotionTrackerLocationBD globalPose;
};
static_assert(sizeof(MotionTrackerLocationsBD) == 180);

typedef XrResult(XRAPI_PTR* PFN_xrGetMotionTrackerConnectStateBD)(XrInstance, MotionTrackerConnectStateBD*);
typedef XrResult(XRAPI_PTR* PFN_xrGetMotionTrackerLocationsBD)(XrInstance, XrTime, const MotionTrackerSerialBD*,
                                                                MotionTrackerLocationsBD*);
// Config 1 is the tracking origin; "1" puts it on the floor.
typedef XrResult(XRAPI_PTR* PFN_xrSetConfigPICO)(XrSession, int32_t config, const char* value);
inline constexpr int32_t kPicoConfigTrackingOrigin = 1;

}  // namespace rv
