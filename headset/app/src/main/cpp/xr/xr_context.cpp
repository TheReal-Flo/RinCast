#include "xr_context.h"

#ifdef __ANDROID__
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <ctime>
#include <unistd.h>

#include "rc/platform.h"

#define XR_CHECK(call)                                                                   \
    do {                                                                                 \
        const XrResult r_ = (call);                                                      \
        if (XR_FAILED(r_)) {                                                             \
            RC_LOGE("%s failed: %s (%s:%d)", #call, XrResultString(r_), __FILE__, __LINE__); \
            return false;                                                                \
        }                                                                                \
    } while (0)

namespace rv {

namespace {

// How hot the headset is, as Android rates it: "none", "light", "moderate", "severe"...;
// "unknown" before Android 11 or without the thermal service. Meta's first answer to heat is
// lowering a refresh rate above 72 Hz to 72.
const char* ThermalStatus() {
#ifdef __ANDROID__
    using Acquire = void* (*)();
    using Status = int (*)(void*);
    static void* lib = dlopen("libandroid.so", RTLD_NOW);
    static auto acquire = lib ? reinterpret_cast<Acquire>(dlsym(lib, "AThermal_acquireManager")) : nullptr;
    static auto status = lib ? reinterpret_cast<Status>(dlsym(lib, "AThermal_getCurrentThermalStatus")) : nullptr;
    static void* manager = acquire ? acquire() : nullptr;
    if (!manager || !status) return "unknown";
    static const char* const kNames[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
    const int s = status(manager);
    return s >= 0 && s < int(std::size(kNames)) ? kNames[s] : "unknown";
#else
    return "unknown";
#endif
}

// A developer setting (adb, SideQuest, Meta Quest Developer Hub) that sets the display's
// rate whatever an app asks for. Empty when not set.
std::string RefreshRateOverride() {
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.oculus.refreshRate", value);
    return value;
#else
    return {};
#endif
}

}  // namespace

const char* XrResultString(XrResult r) {
    switch (r) {
        case XR_SUCCESS: return "XR_SUCCESS";
        case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
        case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
        case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
        case XR_ERROR_CALL_ORDER_INVALID: return "XR_ERROR_CALL_ORDER_INVALID";
        case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
        case XR_SESSION_LOSS_PENDING: return "XR_SESSION_LOSS_PENDING";
        default: {
            static thread_local char buf[32];
            snprintf(buf, sizeof(buf), "XrResult(%d)", int(r));
            return buf;
        }
    }
}

template <typename T>
static T GetProc(XrInstance instance, const char* name) {
    PFN_xrVoidFunction f = nullptr;
    xrGetInstanceProcAddr(instance, name, &f);
    return reinterpret_cast<T>(f);
}

bool XrContext::HasExtension(const char* name) const {
    return std::find(m_extensions.begin(), m_extensions.end(), name) != m_extensions.end();
}

bool XrContext::InitEgl() {
    m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(m_display, nullptr, nullptr);
    const EGLint attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                              EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_SAMPLES, 0,
                              EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(m_display, attribs, &m_config, 1, &count) || count == 0) {
        RC_LOGE("eglChooseConfig failed");
        return false;
    }
    const EGLint ctxAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, ctxAttribs);
    if (m_context == EGL_NO_CONTEXT) {
        RC_LOGE("eglCreateContext failed");
        return false;
    }
    const EGLint pbAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    m_surface = eglCreatePbufferSurface(m_display, m_config, pbAttribs);
    if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
        RC_LOGE("eglMakeCurrent failed");
        return false;
    }
    RC_LOGI("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return true;
}

bool XrContext::CreateInstance(android_app* app, const std::vector<const char*>& extra) {
    if (!xrloader::Load()) return false;
#ifdef __ANDROID__
    auto initLoader = GetProc<PFN_xrInitializeLoaderKHR>(XR_NULL_HANDLE, "xrInitializeLoaderKHR");
    if (initLoader) {
        XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        info.applicationVM = app->activity->vm;
        info.applicationContext = app->activity->clazz;
        initLoader(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&info));
    }
#endif

    uint32_t count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());

    const char* wanted[] = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
#ifdef __ANDROID__
                            XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                            XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME,
#else
                            XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
                            XR_MNDX_EGL_ENABLE_EXTENSION_NAME,
#endif
                            XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME,
                            XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
                            XR_FB_PASSTHROUGH_EXTENSION_NAME,
                            XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME,
                            XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME,
                            XR_FB_COMPOSITION_LAYER_ALPHA_BLEND_EXTENSION_NAME,
                            XR_EXT_HAND_TRACKING_EXTENSION_NAME,
                            XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME,
                            XR_META_SIMULTANEOUS_HANDS_AND_CONTROLLERS_EXTENSION_NAME,
                            XR_META_DETACHED_CONTROLLERS_EXTENSION_NAME,
                            XR_EXT_LOCAL_FLOOR_EXTENSION_NAME,
                            XR_FB_COLOR_SPACE_EXTENSION_NAME,
                            XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME};
    std::vector<const char*> all(std::begin(wanted), std::end(wanted));
    all.insert(all.end(), extra.begin(), extra.end());
    std::vector<const char*> enabled;
    for (const char* w : all) {
        if (HasExtension(w)) continue;  // named twice
        for (const auto& p : props) {
            if (strcmp(p.extensionName, w) == 0) {
                enabled.push_back(w);
                m_extensions.push_back(w);
                break;
            }
        }
    }
#ifdef __ANDROID__
    if (!HasExtension(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
        RC_LOGE("Runtime lacks XR_KHR_opengl_es_enable");
        return false;
    }
#else
    {
        std::string all;
        for (const auto& p : props) all += std::string(" ") + p.extensionName;
        RC_LOGI("Runtime extensions:%s", all.c_str());
    }
    // RinCast draws with OpenGL ES (and its video shaders need ES's external textures). An
    // EGL context reaches the runtime through XR_MNDX_egl_enable; if the runtime has no GL ES
    // extension, the same context goes under XR_KHR_opengl_enable.
    if (!HasExtension(XR_MNDX_EGL_ENABLE_EXTENSION_NAME)) {
        RC_LOGE("Runtime lacks XR_MNDX_egl_enable: it can't take an EGL context");
        return false;
    }
    m_glNotEs = !HasExtension(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
    if (m_glNotEs && !HasExtension(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME)) {
        RC_LOGE("Runtime has neither XR_KHR_opengl_es_enable nor XR_KHR_opengl_enable");
        return false;
    }
    // Only one graphics API per instance.
    const char* unused = m_glNotEs ? XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME : XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
    std::erase_if(enabled, [&](const char* e) { return !strcmp(e, unused); });
    std::erase_if(m_extensions, [&](const std::string& e) { return e == unused; });
    RC_LOGI("Graphics: OpenGL ES context through XR_MNDX_egl_enable, as %s",
            m_glNotEs ? "XR_KHR_opengl_enable (the runtime has no GL ES extension)" : "XR_KHR_opengl_es_enable");
#endif
    m_cylinder = HasExtension(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    m_layerSettings = HasExtension(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    m_handTracking = HasExtension(XR_EXT_HAND_TRACKING_EXTENSION_NAME);

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
#ifdef __ANDROID__
    XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android.applicationVM = app->activity->vm;
    android.applicationActivity = app->activity->clazz;
    ci.next = &android;
#else
    (void)app;
#endif
    strcpy(ci.applicationInfo.applicationName, "RinCast");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "RinCast");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ci.enabledExtensionCount = uint32_t(enabled.size());
    ci.enabledExtensionNames = enabled.data();
    XR_CHECK(xrCreateInstance(&ci, &m_instance));
    if (!xrloader::LoadInstance(m_instance)) return false;
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &ip))) m_runtimeName = ip.runtimeName;
    RC_LOGI("OpenXR runtime: %s", m_runtimeName.c_str());

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(m_instance, &sgi, &m_system));

    if (HasExtension(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME)) {
        m_fromTimespec = GetProc<PFN_xrConvertTimespecTimeToTimeKHR>(m_instance, "xrConvertTimespecTimeToTimeKHR");
        m_toTimespec = GetProc<PFN_xrConvertTimeToTimespecTimeKHR>(m_instance, "xrConvertTimeToTimespecTimeKHR");
    }
    RC_LOGI("OpenXR instance created with %zu extensions", enabled.size());
    return true;
}

bool XrContext::CreateSession(bool preferStage) {
#ifdef __ANDROID__
    auto getReqs = GetProc<PFN_xrGetOpenGLESGraphicsRequirementsKHR>(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR");
    XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHECK(getReqs(m_instance, m_system, &reqs));

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = m_display;
    binding.config = m_config;
    binding.context = m_context;
#else
    // The runtime insists on being asked for its graphics requirements first.
    if (m_glNotEs) {
        auto getReqs = GetProc<PFN_xrGetOpenGLGraphicsRequirementsKHR>(m_instance, "xrGetOpenGLGraphicsRequirementsKHR");
        XrGraphicsRequirementsOpenGLKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
        XR_CHECK(getReqs(m_instance, m_system, &reqs));
    } else {
        auto getReqs = GetProc<PFN_xrGetOpenGLESGraphicsRequirementsKHR>(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR");
        XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        XR_CHECK(getReqs(m_instance, m_system, &reqs));
    }
    XrGraphicsBindingEGLMNDX binding{XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
    binding.getProcAddress = reinterpret_cast<PFN_xrEglGetProcAddressMNDX>(eglGetProcAddress);
    binding.display = m_display;
    binding.config = m_config;
    binding.context = m_context;
#endif
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = m_system;
    XR_CHECK(xrCreateSession(m_instance, &sci, &m_session));

    m_preferStage = preferStage;
    if (!CreateAppSpace()) return false;
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    rs.poseInReferenceSpace = {};
    rs.poseInReferenceSpace.orientation.w = 1;
    XR_CHECK(xrCreateReferenceSpace(m_session, &rs, &m_viewSpace));

    // Eye swapchains.
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                      nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
                                      &viewCount, views.data());
    m_recommendedWidth = views[0].recommendedImageRectWidth;
    m_recommendedHeight = views[0].recommendedImageRectHeight;
    // Supersample the UI slightly for crisper text (within runtime limits).
    const int w = int(std::min<float>(views[0].maxImageRectWidth, m_recommendedWidth * 1.2f));
    const int h = int(std::min<float>(views[0].maxImageRectHeight, m_recommendedHeight * 1.2f));
    for (int eye = 0; eye < 2; eye++)
        if (!CreateSwapchain(w, h, GL_SRGB8_ALPHA8, 1, true, m_eyes[eye])) return false;
    RC_LOGI("Eye buffers %dx%d (recommended %ux%u)", w, h, m_recommendedWidth, m_recommendedHeight);

    if (HasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
        auto enumRates = GetProc<PFN_xrEnumerateDisplayRefreshRatesFB>(m_instance, "xrEnumerateDisplayRefreshRatesFB");
        uint32_t rc = 0;
        enumRates(m_session, 0, &rc, nullptr);
        m_refreshRates.resize(rc);
        enumRates(m_session, rc, &rc, m_refreshRates.data());
        m_refreshRates.resize(rc);
        std::string list;
        for (float r : m_refreshRates) list += (list.empty() ? "" : ", ") + std::to_string(int(std::lround(r)));
        if (m_refreshRates.empty()) RC_LOGW("Refresh rates: the headset listed none, so only 72 Hz is offered");
        else RC_LOGI("Refresh rates: %s Hz", list.c_str());
        if (const std::string forced = RefreshRateOverride(); !forced.empty())
            RC_LOGW("Refresh rate: the developer setting debug.oculus.refreshRate = %s sets the display's rate, "
                    "whatever this app asks for", forced.c_str());
    }
    if (m_refreshRates.empty()) m_refreshRates = {72.f};

    if (HasExtension(XR_FB_COLOR_SPACE_EXTENSION_NAME)) {
        // Content is authored in sRGB/Rec.709; let the runtime convert to the panel.
        auto setCs = GetProc<PFN_xrSetColorSpaceFB>(m_instance, "xrSetColorSpaceFB");
        if (setCs) setCs(m_session, XR_COLOR_SPACE_REC709_FB);
    }

    if (HasExtension(XR_FB_PASSTHROUGH_EXTENSION_NAME)) {
        auto create = GetProc<PFN_xrCreatePassthroughFB>(m_instance, "xrCreatePassthroughFB");
        XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        if (create && XR_SUCCEEDED(create(m_session, &pci, &m_passthrough))) {
            auto createLayer = GetProc<PFN_xrCreatePassthroughLayerFB>(m_instance, "xrCreatePassthroughLayerFB");
            XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
            lci.passthrough = m_passthrough;
            lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
            if (!createLayer || XR_FAILED(createLayer(m_session, &lci, &m_passthroughLayer))) {
                m_passthroughLayer = XR_NULL_HANDLE;
            }
        } else {
            m_passthrough = XR_NULL_HANDLE;
        }
    }
    if (!m_passthroughLayer) {
        uint32_t n = 0;
        xrEnumerateEnvironmentBlendModes(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
        std::vector<XrEnvironmentBlendMode> modes(n);
        if (n) xrEnumerateEnvironmentBlendModes(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, modes.data());
        m_alphaBlend = std::find(modes.begin(), modes.end(), XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) != modes.end();
        if (m_alphaBlend) RC_LOGI("Passthrough through the alpha-blend environment mode");
    }
    return true;
}

bool XrContext::Init(android_app* app, const std::vector<const char*>& extensions) {
    return InitEgl() && CreateInstance(app, extensions);
}

// Prefer a floor-level space so SteamVR games get correct heights. The local floor follows
// the user: recentering (or the runtime doing it) moves it, and with it everything SteamVR
// sees. The stage stays with the play area, which other tracking systems calibrated against
// the headset (Lighthouse trackers through Space Calibrator) need.
bool XrContext::CreateAppSpace() {
    uint32_t n = 0;
    xrEnumerateReferenceSpaces(m_session, 0, &n, nullptr);
    std::vector<XrReferenceSpaceType> spaces(n);
    xrEnumerateReferenceSpaces(m_session, n, &n, spaces.data());
    auto has = [&](XrReferenceSpaceType t) { return std::find(spaces.begin(), spaces.end(), t) != spaces.end(); };
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    if (m_preferStage && has(XR_REFERENCE_SPACE_TYPE_STAGE))
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    else if (has(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT))
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
    else if (has(XR_REFERENCE_SPACE_TYPE_STAGE))
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    else {
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rs.poseInReferenceSpace.position.y = -1.6f;  // approximate floor
    }
    if (m_appSpace && rs.referenceSpaceType == m_appSpaceType) return true;
    XrSpace space = XR_NULL_HANDLE;
    XR_CHECK(xrCreateReferenceSpace(m_session, &rs, &space));
    std::lock_guard lock(m_spaceMutex);
    if (m_appSpace) {
        xrDestroySpace(m_appSpace);
        m_spaceGeneration++;  // like a recenter: windows are placed again
    }
    m_appSpace = space;
    m_appSpaceType = rs.referenceSpaceType;
    RC_LOGI("App space: %s", m_appSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? "stage" : m_appSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT ? "local floor" : "local");
    return true;
}

void XrContext::SetPreferStage(bool stage) {
    if (!m_session || stage == m_preferStage) return;
    m_preferStage = stage;
    CreateAppSpace();
}

bool XrContext::StartSession(bool preferStage, bool velocitiesFromPoses) {
    m_velocitiesFromPoses = velocitiesFromPoses;
    return CreateSession(preferStage);
}

void XrContext::Shutdown() {
    for (auto& e : m_eyes) DestroySwapchain(e);
    if (m_passthroughLayer) GetProc<PFN_xrDestroyPassthroughLayerFB>(m_instance, "xrDestroyPassthroughLayerFB")(m_passthroughLayer);
    if (m_passthrough) GetProc<PFN_xrDestroyPassthroughFB>(m_instance, "xrDestroyPassthroughFB")(m_passthrough);
    if (m_appSpace) xrDestroySpace(m_appSpace);
    if (m_viewSpace) xrDestroySpace(m_viewSpace);
    if (m_session) xrDestroySession(m_session);
    if (m_instance) xrDestroyInstance(m_instance);
    if (m_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_surface != EGL_NO_SURFACE) eglDestroySurface(m_display, m_surface);
        if (m_context != EGL_NO_CONTEXT) eglDestroyContext(m_display, m_context);
        eglTerminate(m_display);
    }
}

void XrContext::HandleStateChange(XrSessionState state) {
    m_state = state;
    RC_LOGI("Session state %d", int(state));
    if (state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (XR_SUCCEEDED(xrBeginSession(m_session, &bi))) m_running = true;
#ifdef __ANDROID__
        if (HasExtension(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME)) {
            if (auto set = GetProc<PFN_xrSetAndroidApplicationThreadKHR>(m_instance, "xrSetAndroidApplicationThreadKHR"))
                set(m_session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, gettid());
        }
#endif
        // Rate requests before the session runs may be ignored.
        if (m_requestedHz > 0) SetRefreshRate(m_requestedHz);
        SetPerformanceLevels();
    } else if (state == XR_SESSION_STATE_STOPPING) {
        xrEndSession(m_session);
        m_running = false;
    }
}

bool XrContext::PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(m_instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                HandleStateChange(e->state);
                if (e->state == XR_SESSION_STATE_EXITING || e->state == XR_SESSION_STATE_LOSS_PENDING) return false;
                break;
            }
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                const auto* e = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
                const XrVector3f& p = e->poseInPreviousSpace.position;
                RC_LOGI("Reference space %d moved by %.2f m (recentered)%s", int(e->referenceSpaceType),
                        std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z), e->referenceSpaceType == m_appSpaceType ? ", SteamVR's too" : "");
                m_spaceGeneration++;
                break;
            }
            case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
                const auto* e = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&ev);
                // Away from the rate asked for, without being asked: the headset's own doing.
                m_rateLoweredBySystem = m_targetHz > 0 && std::fabs(e->fromDisplayRefreshRate - m_targetHz) < 0.5f &&
                                        e->toDisplayRefreshRate < m_targetHz - 0.5f;
                if (m_rateLoweredBySystem)
                    RC_LOGW("Refresh rate: the headset lowered it from %.0f to %.0f Hz itself (heat: %s; Meta's first step "
                            "against heat is 72 Hz)", e->fromDisplayRefreshRate, e->toDisplayRefreshRate, ThermalStatus());
                else
                    RC_LOGI("Refresh rate changed from %.0f to %.0f Hz (heat: %s)", e->fromDisplayRefreshRate,
                            e->toDisplayRefreshRate, ThermalStatus());
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                return false;
            default:
                // XR_TYPE_EVENT_DATA_USER_PRESENCE_CHANGED_EXT would land here on newer runtimes.
                break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return true;
}

bool XrContext::CreateSwapchain(int width, int height, int64_t format, int arraySize, bool withDepth,
                                XrSwapchainGl& out, int mipCount) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = uint32_t(width);
    ci.height = uint32_t(height);
    ci.faceCount = 1;
    ci.arraySize = uint32_t(arraySize);
    ci.mipCount = uint32_t(mipCount);
    XR_CHECK(xrCreateSwapchain(m_session, &ci, &out.handle));
    uint32_t n = 0;
    xrEnumerateSwapchainImages(out.handle, 0, &n, nullptr);
    // OpenGL and OpenGL ES swapchain images have the same layout, only their type differs.
    std::vector<XrSwapchainImageOpenGLESKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
#ifndef __ANDROID__
    if (m_glNotEs)
        for (auto& i : imgs) i.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
#endif
    xrEnumerateSwapchainImages(out.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    out.width = width;
    out.height = height;
    if (withDepth) {
        glGenRenderbuffers(1, &out.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, out.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    }
    for (const auto& img : imgs) {
        out.images.push_back(img.image);
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, img.image, 0);
        if (withDepth) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, out.depth);
        out.fbos.push_back(fbo);
        if (mipCount > 1) {
            glBindTexture(GL_TEXTURE_2D, img.image);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void XrContext::DestroySwapchain(XrSwapchainGl& sc) {
    if (!sc.handle) return;
    if (!sc.fbos.empty()) glDeleteFramebuffers(GLsizei(sc.fbos.size()), sc.fbos.data());
    if (sc.depth) glDeleteRenderbuffers(1, &sc.depth);
    xrDestroySwapchain(sc.handle);
    sc = {};
}

GLuint XrContext::AcquireImage(XrSwapchainGl& sc) {
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index = 0;
    xrAcquireSwapchainImage(sc.handle, &ai, &index);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc.handle, &wi);
    sc.acquired = index;
    return sc.fbos[index];
}

void XrContext::ReleaseImage(XrSwapchainGl& sc) {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc.handle, &ri);
}

bool XrContext::LocateSpace(XrSpace space, XrTime time, Pose& pose, Vec3* linVel, Vec3* angVel) {
    XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (linVel || angVel) loc.next = &vel;
    std::lock_guard lock(m_spaceMutex);
    if (!m_appSpace || XR_FAILED(xrLocateSpace(space, m_appSpace, time, &loc))) return false;
    const bool valid = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                       (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
    if (!valid) return false;
    pose = ToPose(loc.pose);
    if ((linVel || angVel) && m_velocitiesFromPoses) {
        // From where it will be a little later, in the app's space like the pose.
        constexpr XrDuration kStep = 5'000'000;
        XrSpaceLocation later{XR_TYPE_SPACE_LOCATION};
        Pose next = pose;
        if (XR_SUCCEEDED(xrLocateSpace(space, m_appSpace, time + kStep, &later)) &&
            (later.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (later.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
            next = ToPose(later.pose);
        const float dt = float(kStep) * 1e-9f;
        if (linVel) *linVel = (next.position - pose.position) * (1.f / dt);
        if (angVel) {
            Quat d = Normalize(next.orientation * pose.orientation.Conjugate());
            if (d.w < 0) d = {-d.x, -d.y, -d.z, -d.w};
            const float s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            const float angle = 2.f * std::atan2(s, d.w);
            *angVel = s > 1e-6f ? Vec3{d.x, d.y, d.z} * (angle / s / dt) : Vec3{};
        }
        return true;
    }
    if (linVel) *linVel = (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
                              ? Vec3{vel.linearVelocity.x, vel.linearVelocity.y, vel.linearVelocity.z}
                              : Vec3{};
    if (angVel) *angVel = (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
                              ? Vec3{vel.angularVelocity.x, vel.angularVelocity.y, vel.angularVelocity.z}
                              : Vec3{};
    return true;
}

bool XrContext::BeginFrame(FrameState& frame) {
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XR_CHECK(xrWaitFrame(m_session, nullptr, &fs));
    XR_CHECK(xrBeginFrame(m_session, nullptr));
    frame.predictedDisplayTime = fs.predictedDisplayTime;
    frame.period = fs.predictedDisplayPeriod;
    frame.shouldRender = fs.shouldRender;

    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = m_appSpace;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    xrLocateViews(m_session, &vli, &vs, 2, &count, views);
    for (int eye = 0; eye < 2; eye++) {
        EyeView& e = frame.eyes[eye];
        e.pose = ToPose(views[eye].pose);
        e.fov = views[eye].fov;
        e.projection = Mat4::Projection(std::tan(e.fov.angleLeft), std::tan(e.fov.angleRight), std::tan(e.fov.angleUp),
                                        std::tan(e.fov.angleDown), 0.05f, 500.f);
        e.view = Mat4::FromPose(e.pose.Inverse());
    }
    frame.headValid = LocateSpace(m_viewSpace, fs.predictedDisplayTime, frame.head);
    if (!frame.headValid) frame.head = {frame.eyes[0].pose.orientation, (frame.eyes[0].pose.position + frame.eyes[1].pose.position) * 0.5f};
    return true;
}

void XrContext::BindEye(int eye) {
    const GLuint fbo = AcquireImage(m_eyes[eye]);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, m_eyes[eye].width, m_eyes[eye].height);
}

void XrContext::EndFrame(const FrameState& frame, const std::vector<const XrCompositionLayerBaseHeader*>& underlays,
                         bool projectionHasAlpha, const std::vector<const XrCompositionLayerBaseHeader*>& overlays,
                         bool eyeLayer) {
    for (int eye = 0; eye < 2; eye++)
        if (frame.shouldRender && eyeLayer) ReleaseImage(m_eyes[eye]);

    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (int eye = 0; eye < 2; eye++) {
        pv[eye].pose = ToXr(frame.eyes[eye].pose);
        pv[eye].fov = frame.eyes[eye].fov;
        pv[eye].subImage.swapchain = m_eyes[eye].handle;
        pv[eye].subImage.imageRect.extent = {m_eyes[eye].width, m_eyes[eye].height};
    }
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    proj.space = m_appSpace;
    proj.viewCount = 2;
    proj.views = pv;
    if (projectionHasAlpha) proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;

    std::vector<const XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerPassthroughFB pt{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    if (m_passthroughActive && m_passthroughLayer) {
        pt.layerHandle = m_passthroughLayer;
        pt.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&pt));
    }
    if (frame.shouldRender) {
        layers.insert(layers.end(), underlays.begin(), underlays.end());
        if (eyeLayer) layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj));
        layers.insert(layers.end(), overlays.begin(), overlays.end());
    }
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = frame.predictedDisplayTime;
    fei.environmentBlendMode = m_passthroughActive && m_alphaBlend && !m_passthroughLayer ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                                                                                         : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = uint32_t(layers.size());
    fei.layers = layers.data();
    const XrResult r = xrEndFrame(m_session, &fei);
    if (XR_FAILED(r)) RC_LOGW("xrEndFrame: %s", XrResultString(r));
}

float XrContext::RefreshRate() const {
    if (!HasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) return 72.f;
    auto get = GetProc<PFN_xrGetDisplayRefreshRateFB>(m_instance, "xrGetDisplayRefreshRateFB");
    // 0 when it can't be read: a failed read reported as 72 would send the PC to 72 for nothing.
    float hz = 0;
    if (!get || XR_FAILED(get(m_session, &hz))) return 0;
    return hz;
}

// Sustained low, always. It is a range the headset scales within, not a fixed clock: on a Quest
// CPU levels 2 to 4 and GPU 1 to 4, raised as soon as the work needs it. Sustained high pins the
// CPU at 4 and keeps the GPU at 3 or more, the same ceilings with a far higher floor, and the
// headsets ran hot with it while streaming. It is also what a Quest app gets that asks for
// nothing, so this is asked for as soon as the session runs.
void XrContext::SetPerformanceLevels() {
    if (!HasExtension(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME) || !m_running) return;
    auto set = GetProc<PFN_xrPerfSettingsSetPerformanceLevelEXT>(m_instance, "xrPerfSettingsSetPerformanceLevelEXT");
    if (!set) return;
    set(m_session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT);
    set(m_session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT);
    RC_LOGI("Performance level: sustained low");
}

void XrContext::SetRefreshRate(float hz) {
    if (!HasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) return;
    m_requestedHz = hz;
    // Pick the closest supported rate.
    float best = m_refreshRates.empty() ? hz : m_refreshRates[0];
    for (float r : m_refreshRates)
        if (std::fabs(r - hz) < std::fabs(best - hz)) best = r;
    m_targetHz = best;
    auto set = GetProc<PFN_xrRequestDisplayRefreshRateFB>(m_instance, "xrRequestDisplayRefreshRateFB");
    if (set) {
        const XrResult r = set(m_session, best);
        if (XR_FAILED(r)) RC_LOGW("Refresh rate %.0f Hz refused: %s", best, XrResultString(r));
        else RC_LOGI("Refresh rate: %.0f Hz asked for, %.0f Hz now", best, RefreshRate());
    }
}

// A request the runtime took but never applied leaves the display at its default 72 Hz while
// the PC renders at the rate asked for: 90 frames a second into 72 shows four in five, a
// stutter whenever the view moves. Look every two seconds and ask again.
void XrContext::KeepRefreshRate() {
    if (!m_running || m_targetHz <= 0) return;
    const int64_t now = rc::NowNs();
    // Lowered by the headset itself (heat): it gives the rate back when it cools down, so ask
    // rarely; asking every two seconds only fills the log.
    const int64_t interval = m_rateLoweredBySystem ? 30'000'000'000LL : 2'000'000'000LL;
    if (now - m_refreshCheckNs < interval) return;
    m_refreshCheckNs = now;
    const float hz = RefreshRate();
    if (hz <= 0 || std::fabs(hz - m_targetHz) < 0.5f) {
        if (hz > 0) m_rateLoweredBySystem = false;
        return;
    }
    auto set = GetProc<PFN_xrRequestDisplayRefreshRateFB>(m_instance, "xrRequestDisplayRefreshRateFB");
    const XrResult r = set ? set(m_session, m_targetHz) : XR_ERROR_FUNCTION_UNSUPPORTED;
    const std::string forced = RefreshRateOverride();
    RC_LOGW("Refresh rate %.0f Hz instead of %.0f, asked again: %s (heat: %s%s%s)", hz, m_targetHz, XrResultString(r),
            ThermalStatus(), forced.empty() ? "" : "; developer setting debug.oculus.refreshRate = ", forced.c_str());
}

void XrContext::SetPassthrough(bool enable) {
    if (enable == m_passthroughActive) return;
    if (!m_passthroughLayer && m_alphaBlend) {  // nothing to start: the frame's blend mode says it
        m_passthroughActive = enable;
        return;
    }
    if (!m_passthrough || !m_passthroughLayer) return;
    if (enable) {
        GetProc<PFN_xrPassthroughStartFB>(m_instance, "xrPassthroughStartFB")(m_passthrough);
        GetProc<PFN_xrPassthroughLayerResumeFB>(m_instance, "xrPassthroughLayerResumeFB")(m_passthroughLayer);
    } else {
        GetProc<PFN_xrPassthroughLayerPauseFB>(m_instance, "xrPassthroughLayerPauseFB")(m_passthroughLayer);
        GetProc<PFN_xrPassthroughPauseFB>(m_instance, "xrPassthroughPauseFB")(m_passthrough);
    }
    m_passthroughActive = enable;
}

int64_t XrContext::ToMonotonicNs(XrTime t) const {
    if (m_toTimespec) {
        timespec ts{};
        if (XR_SUCCEEDED(m_toTimespec(m_instance, t, &ts))) return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
    }
    return t;
}

XrTime XrContext::FromMonotonicNs(int64_t ns) const {
    if (m_fromTimespec) {
        timespec ts{time_t(ns / 1000000000LL), long(ns % 1000000000LL)};
        XrTime t = 0;
        if (XR_SUCCEEDED(m_fromTimespec(m_instance, &ts, &t))) return t;
    }
    return ns;
}

}  // namespace rv
