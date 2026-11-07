// OpenXR session management for the RinCast headset app (OpenGL ES).
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <time.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

#include "xr_loader.h"  // OpenXR itself, from the loader picked at run time

#include <string>
#include <vector>

#include "../gfx/gl.h"
#include "../gfx/math.h"

struct android_app;

namespace rv {

inline Pose ToPose(const XrPosef& p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
            {p.position.x, p.position.y, p.position.z}};
}
inline XrPosef ToXr(const Pose& p) {
    XrPosef x;
    x.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    x.position = {p.position.x, p.position.y, p.position.z};
    return x;
}

// A swapchain with one FBO (+ shared depth) per image.
struct XrSwapchainGl {
    XrSwapchain handle = XR_NULL_HANDLE;
    int width = 0, height = 0;
    std::vector<GLuint> images;
    std::vector<GLuint> fbos;
    GLuint depth = 0;
    uint32_t acquired = 0;
};

struct EyeView {
    Pose pose;
    XrFovf fov;
    Mat4 projection;
    Mat4 view;
};

struct FrameState {
    XrTime predictedDisplayTime = 0;
    XrDuration period = 0;
    bool shouldRender = false;
    EyeView eyes[2];
    Pose head;
    bool headValid = false;
};

class XrContext {
   public:
    // In two steps, so the headset's family is known before the session starts: `extensions`
    // are turned on where the runtime has them.
    bool Init(android_app* app, const std::vector<const char*>& extensions);
    // The stage first instead of the local floor; velocities from poses rather than the runtime's.
    bool StartSession(bool preferStage, bool velocitiesFromPoses);
    void Shutdown();
    const std::string& RuntimeName() const { return m_runtimeName; }

    // Returns false if the app should exit.
    bool PollEvents();
    bool Running() const { return m_running; }
    bool Focused() const { return m_state == XR_SESSION_STATE_FOCUSED; }
    bool Visible() const { return m_state == XR_SESSION_STATE_VISIBLE || m_state == XR_SESSION_STATE_FOCUSED; }

    bool BeginFrame(FrameState& frame);
    // Renders into the eye swapchain image; returns its FBO bound.
    void BindEye(int eye);
    void EndFrame(const FrameState& frame, const std::vector<const XrCompositionLayerBaseHeader*>& underlays,
                  bool projectionHasAlpha, const std::vector<const XrCompositionLayerBaseHeader*>& overlays,
                  bool eyeLayer = true);

    bool CreateSwapchain(int width, int height, int64_t format, int arraySize, bool withDepth, XrSwapchainGl& out,
                         int mipCount = 1);
    void DestroySwapchain(XrSwapchainGl& sc);
    GLuint AcquireImage(XrSwapchainGl& sc);   // returns FBO
    void ReleaseImage(XrSwapchainGl& sc);

    bool LocateSpace(XrSpace space, XrTime time, Pose& pose, Vec3* linVel = nullptr, Vec3* angVel = nullptr);

    // Display refresh rate (XR_FB_display_refresh_rate)
    std::vector<float> RefreshRates() const { return m_refreshRates; }
    float RefreshRate() const;
    void SetRefreshRate(float hz);

    // Passthrough (XR_FB_passthrough)
    bool PassthroughSupported() const { return m_passthrough != XR_NULL_HANDLE || m_alphaBlend; }
    void SetPassthrough(bool enable);
    // Sustained CPU and GPU clocks while streaming; decoding and networking can't catch up
    // after a clock drop without visible hitches.
    void SetStreamingPerformance(bool streaming);
    bool PassthroughActive() const { return m_passthroughActive; }

    XrInstance Instance() const { return m_instance; }
    XrSession Session() const { return m_session; }
    XrSpace AppSpace() const { return m_appSpace; }
    // The stage rather than the local floor, when the runtime has one. Between frames only.
    void SetPreferStage(bool stage);
    XrSpace ViewSpace() const { return m_viewSpace; }
    XrSystemId System() const { return m_system; }
    int EyeWidth() const { return m_eyes[0].width; }
    int EyeHeight() const { return m_eyes[0].height; }
    uint32_t RecommendedWidth() const { return m_recommendedWidth; }
    uint32_t RecommendedHeight() const { return m_recommendedHeight; }
    bool HasExtension(const char* name) const;
    bool CylinderLayers() const { return m_cylinder; }
    bool LayerSettings() const { return m_layerSettings; }
    bool HandTracking() const { return m_handTracking; }
    // Recentering / boundary changes: incremented whenever the app space moves.
    int SpaceGeneration() const { return m_spaceGeneration; }
    int64_t ToMonotonicNs(XrTime t) const;
    XrTime FromMonotonicNs(int64_t ns) const;
    bool HeadsetMounted() const { return m_mounted; }

   private:
    bool InitEgl();
    bool CreateInstance(android_app* app, const std::vector<const char*>& extra);
    bool CreateSession(bool preferStage);
    bool CreateAppSpace();
    void HandleStateChange(XrSessionState state);

    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
    EGLSurface m_surface = EGL_NO_SURFACE;
    EGLConfig m_config = nullptr;

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_system = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_appSpace = XR_NULL_HANDLE;
    XrReferenceSpaceType m_appSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    bool m_preferStage = false;
    XrSpace m_viewSpace = XR_NULL_HANDLE;
    XrSessionState m_state = XR_SESSION_STATE_UNKNOWN;
    bool m_running = false;
    bool m_mounted = true;
    int m_spaceGeneration = 0;
    std::vector<std::string> m_extensions;
    std::string m_runtimeName;
    uint32_t m_recommendedWidth = 0, m_recommendedHeight = 0;
    XrSwapchainGl m_eyes[2];
    std::vector<float> m_refreshRates;
    float m_requestedHz = 0;
    bool m_cylinder = false;
    // Linux: the runtime takes the GL ES context under XR_KHR_opengl_enable (no ES extension).
    bool m_glNotEs = false;
    bool m_layerSettings = false;
    bool m_handTracking = false;
    bool m_velocitiesFromPoses = false;

    XrPassthroughFB m_passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB m_passthroughLayer = XR_NULL_HANDLE;
    bool m_passthroughActive = false;
    // Runtimes without XR_FB_passthrough (Android XR) show the cameras behind a frame submitted
    // with the alpha-blend environment mode instead.
    bool m_alphaBlend = false;

    PFN_xrConvertTimespecTimeToTimeKHR m_fromTimespec = nullptr;
    PFN_xrConvertTimeToTimespecTimeKHR m_toTimespec = nullptr;
};

const char* XrResultString(XrResult r);

}  // namespace rv
