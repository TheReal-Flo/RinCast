// PICO 4, PICO 4 Ultra and Neo3: their controllers, the Motion Trackers and face tracking.
#include "../headset.h"

#include "app/strings.h"
#include "pico_body.h"
#include "pico_face.h"
#include "rc/platform.h"

namespace rv {

namespace {

class PicoHeadset : public Headset {
   public:
    explicit PicoHeadset(XrContext& xr) : m_xr(xr) {
        m_body.Init(xr);
        m_face.Init(xr);
    }

    const char* Family() const override { return "PICO"; }
    const char* SettingsName() const override { return Tr("device.pico.settings"); }

    std::vector<ControllerProfile> Controllers() const override {
        // No thumbrest on any of them. The PICO 4 Ultra's controllers have two profiles: PICO's
        // own and the registered one.
        return {
            {"/interaction_profiles/bytedance/pico_ultra_controller_bd", XR_BD_ULTRA_CONTROLLER_INTERACTION_EXTENSION_NAME, false, true},
            {"/interaction_profiles/bytedance/pico4s_controller", XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME, false, true},
            {"/interaction_profiles/bytedance/pico4_controller", XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME, false, true},
            {"/interaction_profiles/bytedance/pico_neo3_controller", XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME, false, true},
        };
    }

    // The Motion Trackers are placed on the stage. PICO's velocities are in another space than
    // its poses (ALVR works them out from poses too).
    bool PreferStageSpace() const override { return true; }
    bool VelocitiesFromPoses() const override { return true; }

    bool BodySupported(const XrContext&) const override { return m_body.Supported(); }
    const char* BodyHint() const override { return Tr("device.pico.bodyHint"); }
    std::string BodyStatus() const override {
        // Asked every frame while the settings show; the trackers don't change that fast.
        const int64_t now = rc::NowNs();
        if (now - m_statusAt > 500'000'000) {
            m_status = m_body.Status(m_xr);
            m_statusAt = now;
        }
        return m_status;
    }
    bool LocateBody(XrContext& xr, XrTime time, rc::BodyTrackingPayload& out) override { return m_body.Locate(xr, time, out); }
    void StopBody() override { m_body.Stop(); }
    const char* BodyAction() const override { return m_body.CanCalibrate() ? "Calibrate" : nullptr; }
    void RunBodyAction(XrContext& xr) override { m_body.Calibrate(xr); }

    bool FaceSupported(const XrContext&) const override { return m_face.Supported(); }
    bool ReadFace(XrContext& xr, XrTime time, rc::FaceTrackingPayload& out) override { return m_face.Read(xr, time, out); }
    void StopFace() override { m_face.Stop(); }
    std::vector<std::string> FacePermissions() const override {
        // The microphone too: PICO's face tracking listens to it.
        return {"com.picovr.permission.FACE_TRACKING", "com.picovr.permission.EYE_TRACKING", "android.permission.RECORD_AUDIO"};
    }

   private:
    XrContext& m_xr;
    mutable PicoBody m_body;
    PicoFace m_face;
    mutable std::string m_status;
    mutable int64_t m_statusAt = 0;
};

}  // namespace

std::unique_ptr<Headset> CreatePicoHeadset(XrContext& xr) { return std::make_unique<PicoHeadset>(xr); }

}  // namespace rv
