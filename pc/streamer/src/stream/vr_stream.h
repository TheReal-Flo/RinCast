// Encodes frames produced through the VR bridge (SteamVR or OpenXR) and
// sends them with their render pose so the headset can reproject.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "../encode/color_convert.h"
#include "../encode/encoder.h"
#include "rc/platform.h"
#include "rc/protocol.h"
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
    // Foveated encoding around where the eyes look (see Foveator): the size each eye is
    // encoded at, 0 = off. eyeWidth x eyeHeight stays the size the headset shows.
    uint32_t foveatedEyeWidth = 0;
    uint32_t foveatedEyeHeight = 0;
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
    bool Paused() const { return m_paused; }
    void OnFeedback(const rc::VideoFeedbackPayload& fb) { m_sender.OnFeedback(fb); }
    // Where the eyes look, as the headset tracked it (foveated encoding).
    void SetGaze(const rc::GazePayload& gaze) {
        std::lock_guard lock(m_gazeMutex);
        m_gaze = gaze;
        m_gazeNs = rc::NowNs();
    }
    VideoSenderStats Stats() { return m_sender.Stats(); }
    const VrStreamConfig& Config() const { return m_config; }
    bool Running() const { return m_streaming; }

   private:
    void Run();
    // Where each eye's full-density square starts for this frame, from the gaze.
    void PlaceFovea(const float fovTan[2][4], float start[2][2]);

    rc::Connection* m_connection;
    VrBridgeHost* m_bridge;
    VrStreamConfig m_config;
    VideoSender m_sender;
    StateHandler m_onState;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_streaming{false};
    std::atomic<bool> m_paused{false};
    std::mutex m_gazeMutex;
    rc::GazePayload m_gaze{};
    int64_t m_gazeNs = 0;
    float m_foveaStart[2][2] = {};  // encoding thread only
    bool m_foveaPlaced = false;
};

}  // namespace rcs
