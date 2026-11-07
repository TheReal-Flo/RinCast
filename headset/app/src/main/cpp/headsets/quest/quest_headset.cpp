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

    std::vector<ControllerProfile> Controllers() const override {
        return {{"/interaction_profiles/oculus/touch_controller", nullptr, true, true}};
    }

    bool BodySupported(const XrContext& xr) const override { return QuestBody::Supported(xr); }
    bool LocateBody(XrContext& xr, XrTime time, rc::BodyTrackingPayload& out) override { return m_body.Locate(xr, time, out); }
    void StopBody() override { m_body.Stop(); }
    bool BodyExcludesFingers() const override { return true; }
    std::vector<std::string> BodyPermissions() const override { return {"com.oculus.permission.BODY_TRACKING"}; }

    bool FaceSupported(const XrContext& xr) const override { return QuestFace::Supported(xr); }
    bool ReadFace(XrContext& xr, XrTime time, rc::FaceTrackingPayload& out) override { return m_face.Read(xr, time, out); }
    void StopFace() override { m_face.Stop(); }
    std::vector<std::string> FacePermissions() const override {
        return {"com.oculus.permission.FACE_TRACKING", "com.oculus.permission.EYE_TRACKING"};
    }

   private:
    QuestBody m_body;
    QuestFace m_face;
};

}  // namespace

std::unique_ptr<Headset> CreateQuestHeadset() { return std::make_unique<QuestHeadset>(); }

}  // namespace rv
