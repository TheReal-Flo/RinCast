#include "quest_face.h"

#include <algorithm>
#include <cmath>

#include "rc/platform.h"

namespace rv {

namespace {

template <typename T>
T Proc(XrInstance instance, const char* name) {
    PFN_xrVoidFunction f = nullptr;
    xrGetInstanceProcAddr(instance, name, &f);
    return reinterpret_cast<T>(f);
}

uint8_t Byte(float v) { return uint8_t(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); }

// Where an eye looks: -z rotated by its orientation, in the space the gaze was asked in.
void Forward(const XrQuaternionf& q, float out[3]) {
    out[0] = -2.f * (q.w * q.y + q.x * q.z);
    out[1] = 2.f * (q.w * q.x - q.y * q.z);
    out[2] = -1.f + 2.f * (q.x * q.x + q.y * q.y);
}

}  // namespace

bool QuestEyes::Supported(const XrContext& xr) {
    static int supported = -1;
    if (supported < 0) {
        XrSystemEyeTrackingPropertiesFB eyes{XR_TYPE_SYSTEM_EYE_TRACKING_PROPERTIES_FB};
        XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
        props.next = &eyes;
        supported = xr.HasExtension(XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME) &&
                    XR_SUCCEEDED(xrGetSystemProperties(xr.Instance(), xr.System(), &props)) && eyes.supportsEyeTracking;
    }
    return supported == 1;
}

void QuestEyes::Stop() {
    if (m_eyes && m_destroyEyes) m_destroyEyes(m_eyes);
    m_eyes = XR_NULL_HANDLE;
    m_retryFrames = 0;
}

bool QuestEyes::Read(XrContext& xr, XrTime time, float gaze[2][3], uint8_t confidence[2], uint8_t& valid) {
    valid = 0;
    if (!m_eyes) {
        // Fails until the permission is granted.
        if (!Supported(xr) || m_retryFrames-- > 0) return false;
        m_retryFrames = 120;
        auto create = Proc<PFN_xrCreateEyeTrackerFB>(xr.Instance(), "xrCreateEyeTrackerFB");
        m_destroyEyes = Proc<PFN_xrDestroyEyeTrackerFB>(xr.Instance(), "xrDestroyEyeTrackerFB");
        m_getGazes = Proc<PFN_xrGetEyeGazesFB>(xr.Instance(), "xrGetEyeGazesFB");
        XrEyeTrackerCreateInfoFB ci{XR_TYPE_EYE_TRACKER_CREATE_INFO_FB};
        if (!create || !m_destroyEyes || !m_getGazes || XR_FAILED(create(xr.Session(), &ci, &m_eyes))) {
            m_eyes = XR_NULL_HANDLE;
            return false;
        }
        RC_LOGI("Eye tracking started");
    }
    // Relative to the head: what avatars need, and what the picture is drawn for.
    XrEyeGazesInfoFB gi{XR_TYPE_EYE_GAZES_INFO_FB};
    gi.baseSpace = xr.ViewSpace();
    gi.time = time;
    XrEyeGazesFB g{XR_TYPE_EYE_GAZES_FB};
    if (XR_FAILED(m_getGazes(m_eyes, &gi, &g))) return false;
    for (int eye = 0; eye < 2; eye++) {
        if (!g.gaze[eye].isValid) continue;
        valid |= eye == 0 ? rc::kGazeLeftValid : rc::kGazeRightValid;
        Forward(g.gaze[eye].gazePose.orientation, gaze[eye]);
        confidence[eye] = Byte(g.gaze[eye].gazeConfidence);
    }
    return valid != 0;
}

bool QuestFace::Supported(const XrContext& xr) { return xr.HasExtension(XR_FB_FACE_TRACKING2_EXTENSION_NAME); }

void QuestFace::Stop() {
    if (m_face && m_destroyFace) m_destroyFace(m_face);
    m_face = XR_NULL_HANDLE;
    m_retryFrames = 0;
}

bool QuestFace::Read(XrContext& xr, XrTime time, rc::FaceTrackingPayload& out) {
    if (!m_face) {
        // Fails until the permission is granted.
        if (m_retryFrames-- > 0) return false;
        m_retryFrames = 120;
        auto create = Proc<PFN_xrCreateFaceTracker2FB>(xr.Instance(), "xrCreateFaceTracker2FB");
        m_destroyFace = Proc<PFN_xrDestroyFaceTracker2FB>(xr.Instance(), "xrDestroyFaceTracker2FB");
        m_getWeights = Proc<PFN_xrGetFaceExpressionWeights2FB>(xr.Instance(), "xrGetFaceExpressionWeights2FB");
        if (!create || !m_destroyFace || !m_getWeights) return false;
        // The cameras where there are some, the microphone where not: the headset picks.
        XrFaceTrackingDataSource2FB sources[] = {XR_FACE_TRACKING_DATA_SOURCE2_VISUAL_FB, XR_FACE_TRACKING_DATA_SOURCE2_AUDIO_FB};
        XrFaceTrackerCreateInfo2FB ci{XR_TYPE_FACE_TRACKER_CREATE_INFO2_FB};
        ci.faceExpressionSet = XR_FACE_EXPRESSION_SET2_DEFAULT_FB;
        ci.requestedDataSourceCount = 2;
        ci.requestedDataSources = sources;
        const XrResult r = create(xr.Session(), &ci, &m_face);
        if (XR_FAILED(r)) {
            RC_LOGW("Face tracking did not start (%d)", int(r));
            m_face = XR_NULL_HANDLE;
            return false;
        }
        RC_LOGI("Face tracking started");
    }
    out = {};
    float weights[XR_FACE_EXPRESSION2_COUNT_FB] = {};
    float confidences[XR_FACE_CONFIDENCE2_COUNT_FB] = {};
    XrFaceExpressionWeights2FB w{XR_TYPE_FACE_EXPRESSION_WEIGHTS2_FB};
    w.weightCount = XR_FACE_EXPRESSION2_COUNT_FB;
    w.weights = weights;
    w.confidenceCount = XR_FACE_CONFIDENCE2_COUNT_FB;
    w.confidences = confidences;
    XrFaceExpressionInfo2FB info{XR_TYPE_FACE_EXPRESSION_INFO2_FB};
    info.time = time;
    if (XR_SUCCEEDED(m_getWeights(m_face, &info, &w)) && w.isValid) {
        out.flags |= rc::kFaceTrackedValid;
        if (w.dataSource == XR_FACE_TRACKING_DATA_SOURCE2_AUDIO_FB) out.flags |= rc::kFaceTrackedFromAudio;
        static_assert(rc::kFaceExpressionCount == XR_FACE_EXPRESSION2_COUNT_FB);
        for (int i = 0; i < rc::kFaceExpressionCount; i++) out.expressions[i] = Byte(weights[i]);
        out.upperFaceConfidence = Byte(confidences[XR_FACE_CONFIDENCE2_UPPER_FACE_FB]);
        out.lowerFaceConfidence = Byte(confidences[XR_FACE_CONFIDENCE2_LOWER_FACE_FB]);
    }
    uint8_t eyes = 0;
    m_eyes.Read(xr, time, out.gaze, out.gazeConfidence, eyes);
    if (eyes & rc::kGazeLeftValid) out.flags |= rc::kFaceLeftEyeValid;
    if (eyes & rc::kGazeRightValid) out.flags |= rc::kFaceRightEyeValid;
    return out.flags != 0;
}

}  // namespace rv
