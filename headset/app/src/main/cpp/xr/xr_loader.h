// The OpenXR loader is picked when the app starts, not when it's built: Khronos' standard one;
// PICO's own, which PICO headsets on firmware before PICO OS 5.9 need to find their runtime; or,
// in the Quest 1 build, Meta's own, which the Quest 1's last Horizon OS needs.
// Both name themselves libopenxr_loader.so, and Android treats a library with that name as
// already loaded, so only one of them can be used in a run.
//
// The app calls OpenXR through the pointers below, filled from that loader. The rest of the
// code calls them as usual (xrCreateInstance(...)): XR_NO_PROTOTYPES keeps openxr.h from
// declaring them as functions.
#pragma once

#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#ifdef __ANDROID__
#define XR_USE_PLATFORM_ANDROID
#else
// Linux (Steam Frame): an EGL context handed to the runtime through XR_MNDX_egl_enable.
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL
#endif
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_TIMESPEC
#include <EGL/egl.h>
#ifdef __ANDROID__
#include <jni.h>
#endif
#include <time.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// Core functions the app calls directly. Extension functions are looked up where they're used.
#define RV_XR_GLOBAL_FUNCTIONS(X)            \
    X(xrEnumerateInstanceExtensionProperties) \
    X(xrCreateInstance)

#define RV_XR_INSTANCE_FUNCTIONS(X)        \
    X(xrDestroyInstance)                   \
    X(xrGetInstanceProperties)             \
    X(xrPollEvent)                         \
    X(xrStringToPath)                      \
    X(xrGetSystem)                         \
    X(xrGetSystemProperties)               \
    X(xrEnumerateViewConfigurationViews)   \
    X(xrEnumerateEnvironmentBlendModes)    \
    X(xrCreateSession)                     \
    X(xrDestroySession)                    \
    X(xrBeginSession)                      \
    X(xrEndSession)                        \
    X(xrEnumerateReferenceSpaces)          \
    X(xrCreateReferenceSpace)              \
    X(xrCreateActionSpace)                 \
    X(xrLocateSpace)                       \
    X(xrDestroySpace)                      \
    X(xrCreateSwapchain)                   \
    X(xrDestroySwapchain)                  \
    X(xrEnumerateSwapchainImages)          \
    X(xrAcquireSwapchainImage)             \
    X(xrWaitSwapchainImage)                \
    X(xrReleaseSwapchainImage)             \
    X(xrWaitFrame)                         \
    X(xrBeginFrame)                        \
    X(xrEndFrame)                          \
    X(xrLocateViews)                       \
    X(xrCreateActionSet)                   \
    X(xrCreateAction)                      \
    X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets)           \
    X(xrGetCurrentInteractionProfile)      \
    X(xrGetActionStateBoolean)             \
    X(xrGetActionStateFloat)               \
    X(xrGetActionStateVector2f)            \
    X(xrGetActionStatePose)                \
    X(xrSyncActions)                       \
    X(xrApplyHapticFeedback)

#define RV_XR_DECLARE(name) extern PFN_##name name;
RV_XR_DECLARE(xrGetInstanceProcAddr)
RV_XR_GLOBAL_FUNCTIONS(RV_XR_DECLARE)
RV_XR_INSTANCE_FUNCTIONS(RV_XR_DECLARE)
#undef RV_XR_DECLARE

namespace rv::xrloader {

// Opens the loader and fills the functions that work without an instance.
bool Load();
// Fills the rest, once there is an instance.
bool LoadInstance(XrInstance instance);
// Which loader is in use, for the log.
const char* Name();
// The original Quest hardware, regardless of which APK flavor was installed.
bool IsQuest1();

}  // namespace rv::xrloader
