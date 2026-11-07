// Android XR (Samsung Galaxy XR): its controllers answer to the Touch profile, and hand tracking,
// most people's main input there, is a permission the user grants at run time.
#include "../headset.h"

namespace rv {

namespace {

class AndroidXrHeadset : public Headset {
   public:
    const char* Family() const override { return "Android XR"; }

    // Hands are how most people use the Galaxy XR, so this is asked for right away rather than
    // when a feature is turned on. The hand trackers start once it's granted (XrInput retries).
    std::vector<std::string> StartupPermissions() const override { return {"android.permission.HAND_TRACKING"}; }
    // Its eye tracking is XR_EXT_eye_gaze_interaction's.
    std::vector<std::string> GazePermissions() const override { return {"android.permission.EYE_TRACKING_FINE"}; }
};

}  // namespace

std::unique_ptr<Headset> CreateAndroidXrHeadset() { return std::make_unique<AndroidXrHeadset>(); }

}  // namespace rv
