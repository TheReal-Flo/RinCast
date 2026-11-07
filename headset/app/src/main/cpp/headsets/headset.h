// What differs between headsets. The app itself is the same on every headset; each family adds
// what only it has here, in headsets/<family>: its controllers, body trackers, face tracking and
// the permissions they need. A runtime that isn't a known family gets the plain OpenXR parts.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../xr/xr_context.h"
#include "app/strings.h"
#include "rc/protocol.h"

namespace rv {

// An interaction profile the app suggests bindings for.
struct ControllerProfile {
    const char* path;
    const char* extension = nullptr;  // that the runtime must have, or nullptr for a core profile
    bool thumbrest = false;           // /input/thumbrest/touch
    bool touchSensors = true;         // a, b, x, y, trigger and thumbstick touch
};

class Headset {
   public:
    virtual ~Headset() = default;

    // Every extension of every family; the context turns on those the runtime has.
    static std::vector<const char*> Extensions();
    // The family of the runtime the instance was created with.
    static std::unique_ptr<Headset> Create(XrContext& xr);

    virtual const char* Family() const { return "OpenXR"; }
    // Where the user allows permissions, for hints: "Quest settings".
    virtual const char* SettingsName() const { return "the headset's settings"; }

    // Controllers, in order of preference. Every family has the Khronos simple profile too.
    virtual std::vector<ControllerProfile> Controllers() const;

    // The stage rather than the local floor as the app's space, where the family's trackers
    // are placed in the stage.
    virtual bool PreferStageSpace() const { return false; }
    // Some runtimes report velocities in the wrong space; then they're worked out from poses.
    virtual bool VelocitiesFromPoses() const { return false; }

    // Body trackers for SteamVR, in rc::BodyTracker order.
    virtual bool BodySupported(const XrContext&) const { return false; }
    virtual const char* BodyHint() const { return nullptr; }
    // A short status line for the settings, e.g. how many trackers are connected; empty for none.
    virtual std::string BodyStatus() const { return {}; }
    // Starts tracking when needed. False while the body isn't tracked.
    virtual bool LocateBody(XrContext&, XrTime, rc::BodyTrackingPayload&) { return false; }
    virtual void StopBody() {}
    // The Quest can't track the body and fingers on controllers at the same time.
    virtual bool BodyExcludesFingers() const { return false; }
    // A button beside the setting, such as "Calibrate": its label, or nullptr for none.
    virtual const char* BodyAction() const { return nullptr; }
    virtual void RunBodyAction(XrContext&) {}

    // Face and eyes, for avatars (VRCFaceTracking on the PC).
    virtual bool FaceSupported(const XrContext&) const { return false; }
    virtual const char* FaceHint() const { return Tr("device.faceHint"); }
    virtual bool ReadFace(XrContext&, XrTime, rc::FaceTrackingPayload&) { return false; }
    virtual void StopFace() {}

    // Where the eyes look, for foveated encoding: the family's own eye tracking. Elsewhere the
    // app asks XR_EXT_eye_gaze_interaction (XrInput), with GazePermissions() all the same.
    virtual bool GazeSupported(const XrContext&) const { return false; }
    virtual bool ReadGaze(XrContext&, XrTime, rc::GazePayload&) { return false; }
    virtual void StopGaze() {}
    virtual std::vector<std::string> GazePermissions() const { return {}; }

    // Asked for when the app starts: what basic input needs (Android XR's hand tracking).
    virtual std::vector<std::string> StartupPermissions() const { return {}; }
    // Asked for the first time the feature is turned on.
    virtual std::vector<std::string> BodyPermissions() const { return {}; }
    virtual std::vector<std::string> FacePermissions() const { return {}; }
};

std::unique_ptr<Headset> CreateQuestHeadset();
std::unique_ptr<Headset> CreatePicoHeadset(XrContext& xr);
std::unique_ptr<Headset> CreateAndroidXrHeadset();
// Enabled where the runtime has it, to recognize Android XR (Samsung Galaxy XR).
inline constexpr const char* kAndroidXrMarkerExtension = "XR_ANDROID_unbounded_reference_space";

}  // namespace rv
