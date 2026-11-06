#include "pico_face.h"

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

constexpr int kPicoExpressions = 52;  // XrFaceExpressionBD

// For each PICO expression, the Quest's that move the same muscles (-1: none). ARKit's
// expressions are the FACS action units the Quest's are named after; the left eye looking "in"
// looks right.
struct Map {
    XrFaceExpressionBD from;
    XrFaceExpression2FB to[4];
    int count;
};
#define TO1(a) {XR_FACE_EXPRESSION2_##a##_FB}, 1
#define TO2(a, b) {XR_FACE_EXPRESSION2_##a##_FB, XR_FACE_EXPRESSION2_##b##_FB}, 2
#define TO4(a, b, c, d) {XR_FACE_EXPRESSION2_##a##_FB, XR_FACE_EXPRESSION2_##b##_FB, XR_FACE_EXPRESSION2_##c##_FB, XR_FACE_EXPRESSION2_##d##_FB}, 4
const Map kMap[] = {
    {XR_FACE_EXPRESSION_BROW_DROP_L_BD, TO1(BROW_LOWERER_L)},
    {XR_FACE_EXPRESSION_BROW_DROP_R_BD, TO1(BROW_LOWERER_R)},
    {XR_FACE_EXPRESSION_BROW_INNER_UPWARDS_BD, TO2(INNER_BROW_RAISER_L, INNER_BROW_RAISER_R)},
    {XR_FACE_EXPRESSION_BROW_OUTER_UPWARDS_L_BD, TO1(OUTER_BROW_RAISER_L)},
    {XR_FACE_EXPRESSION_BROW_OUTER_UPWARDS_R_BD, TO1(OUTER_BROW_RAISER_R)},
    {XR_FACE_EXPRESSION_EYE_BLINK_L_BD, TO1(EYES_CLOSED_L)},
    {XR_FACE_EXPRESSION_EYE_LOOK_DROP_L_BD, TO1(EYES_LOOK_DOWN_L)},
    {XR_FACE_EXPRESSION_EYE_LOOK_IN_L_BD, TO1(EYES_LOOK_RIGHT_L)},
    {XR_FACE_EXPRESSION_EYE_LOOK_OUT_L_BD, TO1(EYES_LOOK_LEFT_L)},
    {XR_FACE_EXPRESSION_EYE_LOOK_UPWARDS_L_BD, TO1(EYES_LOOK_UP_L)},
    {XR_FACE_EXPRESSION_EYE_LOOK_SQUINT_L_BD, TO1(LID_TIGHTENER_L)},
    {XR_FACE_EXPRESSION_EYE_LOOK_WIDE_L_BD, TO1(UPPER_LID_RAISER_L)},
    {XR_FACE_EXPRESSION_EYE_BLINK_R_BD, TO1(EYES_CLOSED_R)},
    {XR_FACE_EXPRESSION_EYE_LOOK_DROP_R_BD, TO1(EYES_LOOK_DOWN_R)},
    {XR_FACE_EXPRESSION_EYE_LOOK_IN_R_BD, TO1(EYES_LOOK_LEFT_R)},
    {XR_FACE_EXPRESSION_EYE_LOOK_OUT_R_BD, TO1(EYES_LOOK_RIGHT_R)},
    {XR_FACE_EXPRESSION_EYE_LOOK_UPWARDS_R_BD, TO1(EYES_LOOK_UP_R)},
    {XR_FACE_EXPRESSION_EYE_LOOK_SQUINT_R_BD, TO1(LID_TIGHTENER_R)},
    {XR_FACE_EXPRESSION_EYE_LOOK_WIDE_R_BD, TO1(UPPER_LID_RAISER_R)},
    {XR_FACE_EXPRESSION_NOSE_SNEER_L_BD, TO1(NOSE_WRINKLER_L)},
    {XR_FACE_EXPRESSION_NOSE_SNEER_R_BD, TO1(NOSE_WRINKLER_R)},
    {XR_FACE_EXPRESSION_CHEEK_PUFF_BD, TO2(CHEEK_PUFF_L, CHEEK_PUFF_R)},
    {XR_FACE_EXPRESSION_CHEEK_SQUINT_L_BD, TO1(CHEEK_RAISER_L)},
    {XR_FACE_EXPRESSION_CHEEK_SQUINT_R_BD, TO1(CHEEK_RAISER_R)},
    {XR_FACE_EXPRESSION_MOUTH_CLOSE_BD, TO1(LIPS_TOWARD)},
    {XR_FACE_EXPRESSION_MOUTH_FUNNEL_BD, TO4(LIP_FUNNELER_LB, LIP_FUNNELER_LT, LIP_FUNNELER_RB, LIP_FUNNELER_RT)},
    {XR_FACE_EXPRESSION_MOUTH_PUCKER_BD, TO2(LIP_PUCKER_L, LIP_PUCKER_R)},
    {XR_FACE_EXPRESSION_MOUTH_L_BD, TO1(MOUTH_LEFT)},
    {XR_FACE_EXPRESSION_MOUTH_R_BD, TO1(MOUTH_RIGHT)},
    {XR_FACE_EXPRESSION_MOUTH_SMILE_L_BD, TO1(LIP_CORNER_PULLER_L)},
    {XR_FACE_EXPRESSION_MOUTH_SMILE_R_BD, TO1(LIP_CORNER_PULLER_R)},
    {XR_FACE_EXPRESSION_MOUTH_FROWN_L_BD, TO1(LIP_CORNER_DEPRESSOR_L)},
    {XR_FACE_EXPRESSION_MOUTH_FROWN_R_BD, TO1(LIP_CORNER_DEPRESSOR_R)},
    {XR_FACE_EXPRESSION_MOUTH_DIMPLE_L_BD, TO1(DIMPLER_L)},
    {XR_FACE_EXPRESSION_MOUTH_DIMPLE_R_BD, TO1(DIMPLER_R)},
    {XR_FACE_EXPRESSION_MOUTH_STRETCH_L_BD, TO1(LIP_STRETCHER_L)},
    {XR_FACE_EXPRESSION_MOUTH_STRETCH_R_BD, TO1(LIP_STRETCHER_R)},
    {XR_FACE_EXPRESSION_MOUTH_ROLL_LOWER_BD, TO2(LIP_SUCK_LB, LIP_SUCK_RB)},
    {XR_FACE_EXPRESSION_MOUTH_ROLL_UPPER_BD, TO2(LIP_SUCK_LT, LIP_SUCK_RT)},
    {XR_FACE_EXPRESSION_MOUTH_SHRUG_LOWER_BD, TO1(CHIN_RAISER_B)},
    {XR_FACE_EXPRESSION_MOUTH_SHRUG_UPPER_BD, TO1(CHIN_RAISER_T)},
    {XR_FACE_EXPRESSION_MOUTH_PRESS_L_BD, TO1(LIP_PRESSOR_L)},
    {XR_FACE_EXPRESSION_MOUTH_PRESS_R_BD, TO1(LIP_PRESSOR_R)},
    {XR_FACE_EXPRESSION_MOUTH_LOWER_DROP_L_BD, TO1(LOWER_LIP_DEPRESSOR_L)},
    {XR_FACE_EXPRESSION_MOUTH_LOWER_DROP_R_BD, TO1(LOWER_LIP_DEPRESSOR_R)},
    {XR_FACE_EXPRESSION_MOUTH_UPPER_UPWARDS_L_BD, TO1(UPPER_LIP_RAISER_L)},
    {XR_FACE_EXPRESSION_MOUTH_UPPER_UPWARDS_R_BD, TO1(UPPER_LIP_RAISER_R)},
    {XR_FACE_EXPRESSION_JAW_FORWARD_BD, TO1(JAW_THRUST)},
    {XR_FACE_EXPRESSION_JAW_L_BD, TO1(JAW_SIDEWAYS_LEFT)},
    {XR_FACE_EXPRESSION_JAW_R_BD, TO1(JAW_SIDEWAYS_RIGHT)},
    {XR_FACE_EXPRESSION_JAW_OPEN_BD, TO1(JAW_DROP)},
    {XR_FACE_EXPRESSION_TONGUE_OUT_BD, TO1(TONGUE_OUT)},
};
#undef TO1
#undef TO2
#undef TO4
static_assert(sizeof(kMap) / sizeof(kMap[0]) == kPicoExpressions);

}  // namespace

void PicoFace::Init(XrContext& xr) {
    if (!xr.HasExtension(XR_BD_FACIAL_SIMULATION_EXTENSION_NAME)) return;
    XrSystemFacialSimulationPropertiesBD face{XR_TYPE_SYSTEM_FACIAL_SIMULATION_PROPERTIES_BD};
    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    props.next = &face;
    m_modes = Proc<PFN_xrEnumerateFacialSimulationModesBD>(xr.Instance(), "xrEnumerateFacialSimulationModesBD");
    m_create = Proc<PFN_xrCreateFaceTrackerBD>(xr.Instance(), "xrCreateFaceTrackerBD");
    m_destroy = Proc<PFN_xrDestroyFaceTrackerBD>(xr.Instance(), "xrDestroyFaceTrackerBD");
    m_get = Proc<PFN_xrGetFacialSimulationDataBD>(xr.Instance(), "xrGetFacialSimulationDataBD");
    m_supported = XR_SUCCEEDED(xrGetSystemProperties(xr.Instance(), xr.System(), &props)) && face.supportsFaceTracking &&
                  m_modes && m_create && m_destroy && m_get;
    RC_LOGI("PICO face tracking: %d", m_supported);
}

void PicoFace::Stop() {
    if (m_face && m_destroy) m_destroy(m_face);
    m_face = XR_NULL_HANDLE;
    m_retryFrames = 0;
}

bool PicoFace::Read(XrContext& xr, XrTime time, rc::FaceTrackingPayload& out) {
    if (!m_supported) return false;
    if (!m_face) {
        // Fails until the permissions are granted.
        if (m_retryFrames-- > 0) return false;
        m_retryFrames = 120;
        XrFacialSimulationModeBD modes[8];
        uint32_t count = 0;
        if (XR_FAILED(m_modes(xr.Session(), 8, &count, modes))) count = 0;
        // The cameras with the microphone's help where there are cameras, else the microphone.
        XrFacialSimulationModeBD mode = XR_FACIAL_SIMULATION_MODE_DEFAULT_BD;
        bool found = false;
        for (XrFacialSimulationModeBD want : {XR_FACIAL_SIMULATION_MODE_COMBINED_AUDIO_BD, XR_FACIAL_SIMULATION_MODE_DEFAULT_BD,
                                              XR_FACIAL_SIMULATION_MODE_COMBINED_AUDIO_WITH_LIP_BD,
                                              XR_FACIAL_SIMULATION_MODE_ONLY_AUDIO_WITH_LIP_BD}) {
            for (uint32_t i = 0; i < count && !found; i++)
                if (modes[i] == want) {
                    mode = want;
                    found = true;
                }
            if (found) break;
        }
        XrFaceTrackerCreateInfoBD ci{XR_TYPE_FACE_TRACKER_CREATE_INFO_BD};
        ci.mode = mode;
        const XrResult r = m_create(xr.Session(), &ci, &m_face);
        if (XR_FAILED(r)) {
            RC_LOGW("PICO face tracking did not start (%d)", int(r));
            m_face = XR_NULL_HANDLE;
            return false;
        }
        m_audioOnly = mode == XR_FACIAL_SIMULATION_MODE_ONLY_AUDIO_WITH_LIP_BD;
        RC_LOGI("PICO face tracking started, mode %d", int(mode));
    }
    float weights[kPicoExpressions] = {};
    XrFacialSimulationDataBD data{XR_TYPE_FACIAL_SIMULATION_DATA_BD};
    data.faceExpressionWeightCount = kPicoExpressions;
    data.faceExpressionWeights = weights;
    XrFacialSimulationDataGetInfoBD info{XR_TYPE_FACIAL_SIMULATION_DATA_GET_INFO_BD};
    info.time = time;
    out = {};
    if (XR_FAILED(m_get(m_face, &info, &data)) || (!data.isUpperFaceDataValid && !data.isLowerFaceDataValid)) return false;
    float quest[rc::kFaceExpressionCount] = {};
    for (const Map& m : kMap)
        for (int i = 0; i < m.count; i++) quest[m.to[i]] = std::max(quest[m.to[i]], weights[m.from]);
    out.flags = rc::kFaceTrackedValid | (m_audioOnly ? rc::kFaceTrackedFromAudio : 0);
    for (int i = 0; i < rc::kFaceExpressionCount; i++) out.expressions[i] = Byte(quest[i]);
    out.upperFaceConfidence = data.isUpperFaceDataValid ? 255 : 0;
    out.lowerFaceConfidence = data.isLowerFaceDataValid ? 255 : 0;
    return true;
}

}  // namespace rv
