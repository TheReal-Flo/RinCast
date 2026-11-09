// Meta Quest: body tracking (XR_FB_body_tracking) and face tracking (XR_FB_face_tracking2), with
// Touch controllers.
#include "../headset.h"

#include "app/strings.h"
#include "quest_body.h"
#include "quest_face.h"

namespace rv {

namespace {

class QuestHeadset : public Headset {
   public:
    const char* Family() const override { return "Quest"; }
    const char* SettingsName() const override { return Tr("device.quest.settings"); }

    bool PanelSize(uint32_t& width, uint32_t& height) const override {
        const std::string device = SystemProperty("ro.product.device");
        if (device == "monterey") width = 1440, height = 1600;                       // Quest 1
        else if (device == "hollywood" || device == "panther") width = 1832, height = 1920;  // Quest 2, 3S
        else if (device == "seacliff") width = 1800, height = 1920;                  // Quest Pro
        else if (device == "eureka") width = 2064, height = 2208;                    // Quest 3
        else return false;
        return true;
    }

    std::vector<ControllerProfile> Controllers() const override {
        return {{"/interaction_profiles/oculus/touch_controller", nullptr, true, true}};
    }

    bool BodySupported(const XrContext& xr) const override { return QuestBody::Supported(xr); }
    bool LocateBody(XrContext& xr, XrTime time, rc::BodyTrackingPayload& out) override { return m_body.Locate(xr, time, out); }
    void StopBody() override { m_body.Stop(); }
    bool BodyExcludesFingers() const override { return true; }
    std::vector<std::string> BodyPermissions() const override { return {"com.oculus.permission.BODY_TRACKING"}; }

    bool FaceSupported(const XrContext& xr) const override { return QuestFace::Supported(xr); }
    bool ReadFace(XrContext& xr, XrTime time, rc::FaceTrackingPayload& out) override {
        m_faceOn = true;
        return m_face.Read(xr, time, out);
    }
    void StopFace() override {
        m_face.Stop();
        m_faceOn = false;
        if (!m_gazeOn) m_eyes.Stop();
    }
    std::vector<std::string> FacePermissions() const override {
        return {"com.oculus.permission.FACE_TRACKING", "com.oculus.permission.EYE_TRACKING"};
    }

    bool GazeSupported(const XrContext& xr) const override { return QuestEyes::Supported(xr); }
    bool ReadGaze(XrContext& xr, XrTime time, rc::GazePayload& out) override {
        m_gazeOn = true;
        uint8_t confidence[2] = {};
        return m_eyes.Read(xr, time, out.gaze, confidence, out.flags);
    }
    void StopGaze() override {
        m_gazeOn = false;
        if (!m_faceOn) m_eyes.Stop();
    }
    std::vector<std::string> GazePermissions() const override { return {"com.oculus.permission.EYE_TRACKING"}; }

   private:
    QuestBody m_body;
    QuestEyes m_eyes;  // before m_face, which uses it
    QuestFace m_face{m_eyes};
    bool m_faceOn = false, m_gazeOn = false;
};

}  // namespace

std::unique_ptr<Headset> CreateQuestHeadset() { return std::make_unique<QuestHeadset>(); }

}  // namespace rv
