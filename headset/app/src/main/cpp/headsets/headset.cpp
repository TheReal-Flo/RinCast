#include "headset.h"

#include <algorithm>
#include <cctype>

#include "pico/pico_extensions.h"
#include "rc/platform.h"

namespace rv {

std::vector<const char*> Headset::Extensions() {
    return {
        // Quest
        XR_FB_BODY_TRACKING_EXTENSION_NAME,
        XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME,
        XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME,
        XR_FB_FACE_TRACKING2_EXTENSION_NAME,
        XR_FB_EYE_TRACKING_SOCIAL_EXTENSION_NAME,
        // PICO
        XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME,
        XR_BD_ULTRA_CONTROLLER_INTERACTION_EXTENSION_NAME,
        XR_BD_BODY_TRACKING_EXTENSION_NAME,
        XR_BD_BODY_TRACKING_AUXILIARY_METRICS_EXTENSION_NAME,
        XR_BD_FACIAL_SIMULATION_EXTENSION_NAME,
        kBdMotionTrackingExtension,
        kPicoConfigurationExtension,
        // Android XR
        kAndroidXrMarkerExtension,
        // Any: where the eyes look, for foveated encoding
        XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME,
    };
}

std::vector<ControllerProfile> Headset::Controllers() const {
    // A runtime of no known family most likely knows Touch controllers.
    return {{"/interaction_profiles/oculus/touch_controller", nullptr, true, true}};
}

std::unique_ptr<Headset> Headset::Create(XrContext& xr) {
    std::string runtime = xr.RuntimeName();
    std::transform(runtime.begin(), runtime.end(), runtime.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const auto has = [&](const char* s) { return runtime.find(s) != std::string::npos; };
    if (has("oculus") || has("meta")) return CreateQuestHeadset();
    if (has("pico") || has("bytedance") || xr.HasExtension(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME)) return CreatePicoHeadset(xr);
    if (has("android") || xr.HasExtension(kAndroidXrMarkerExtension)) return CreateAndroidXrHeadset();
    RC_LOGI("Unknown OpenXR runtime \"%s\": plain OpenXR only", xr.RuntimeName().c_str());
    return std::make_unique<Headset>();
}

}  // namespace rv
