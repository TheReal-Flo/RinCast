// Encodes frames produced through the VR bridge (SteamVR or OpenXR) and
// sends them with their render pose so the headset can reproject.
#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include "../encode/color_convert.h"
#include "../encode/encoder.h"
#include "video_sender.h"

namespace rcs {

class VrBridgeHost;

struct VrStreamConfig {
    rc::VideoCodec codec = rc::VideoCodec::HEVC;
    bool tenBit = false;
    uint32_t eyeWidth = 0;
    uint32_t eyeHeight = 0;
    float fps = 90;
    uint32_t bitrateKbps = 150000;
    int fecPercent = 5;
    bool adaptive = true;
    uint32_t startBitrateKbps = 0;  // adaptive: where to start (0 = at bitrateKbps)
    bool pace = false;              // internet paths: no bursts
    bool foveated = true;
    bool adaptiveQuant = true;
    bool twoPass = false;
    bool motionField = false;       // for frame generation on the headset
};

class VrStream {
   public:
    // state: "waiting" (armed, no frames) / "running"; producer: steamvr/openxr
    using StateHandler = std::function<void(const std::string& state, const std::string& producer, const std::string& app)>;

    VrStream(rc::Connection* connection, VrBridgeHost* bridge, const VrStreamConfig& config, StateHandler onState);
    ~VrStream();

    bool Start();
    void Stop();
    void RequestKeyframe() { m_sender.RequestKeyframe(); }
    // The headset left the VR view: nothing is encoded or sent, but the game's frames still
    // count for Running(). Resuming starts with a keyframe.
    void SetPaused(bool paused) {
        if (m_paused.exchange(paused) && !paused) m_sender.RequestKeyframe();
    }
    void OnFeedback(const rc::VideoFeedbackPayload& fb) { m_sender.OnFeedback(fb); }
    VideoSenderStats Stats() { return m_sender.Stats(); }
    const VrStreamConfig& Config() const { return m_config; }
    bool Running() const { return m_streaming; }

   private:
    void Run();

    rc::Connection* m_connection;
    VrBridgeHost* m_bridge;
    VrStreamConfig m_config;
    VideoSender m_sender;
    StateHandler m_onState;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_streaming{false};
    std::atomic<bool> m_paused{false};
};

}  // namespace rcs
