// Encodes frames produced through the VR bridge (SteamVR or OpenXR) and
// sends them with their render pose so the headset can reproject.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

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
    float foveaSize = 0.45f;  // the full-detail share of each eye (Foveator)
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
    // A converted frame waiting for the encoder. Conversion and encoding run on threads of
    // their own: one frame converts while the one before it encodes. Run in sequence, a
    // frame took both times, and every frame the game finished meanwhile was replaced.
    struct EncodeJob {
        ComPtr<ID3D11Texture2D> yuv;  // kept alive here: the converter may move on to a new size
        DXGI_FORMAT format = DXGI_FORMAT_NV12;
        uint32_t width = 0, height = 0;  // encoded size
        uint32_t srcWidth = 0, srcHeight = 0;
        bool halfRate = false;
        bool foveated = false;  // squeezed around the gaze (foveaStart)
        float foveaStart[2][2] = {};
        uint64_t frameIndex = 0;
        uint64_t headsetTargetNs = 0;
        int64_t submitPcNs = 0;
        float renderOrientation[4] = {}, renderPosition[3] = {};
        float fovTan[2][4] = {};
        std::vector<uint8_t> motion;  // the motion field, already in its wire form
    };

    // An encoded frame waiting to be packetized and sent. On a thread of its own, so the
    // encoder starts on the next frame while this one goes out (paced, on internet paths).
    struct SendJob {
        EncodedFrame frame;
        std::vector<uint8_t> meta;
        uint8_t flags = 0;
        double encodeMs = 0;
        bool recovery = false;
    };

    void Run();
    void RunEncoder();
    void RunSender();
    // Waits until the encoder is about to want the next frame (see Run).
    void WaitForEncoder(int64_t convertAvgNs);
    // Where frames went, every 10 s while streaming (see Run).
    void LogFrameFlow(int64_t now);
    // Where each eye's full-density square starts for this frame, from the gaze.
    void PlaceFovea(const float fovTan[2][4], float start[2][2]);

    rc::Connection* m_connection;
    VrBridgeHost* m_bridge;
    VrStreamConfig m_config;
    VideoSender m_sender;
    StateHandler m_onState;
    std::thread m_thread;
    std::thread m_encodeThread;
    std::thread m_sendThread;
    std::mutex m_sendMutex;
    std::condition_variable m_sendCv;  // a frame to send, or room for one
    std::optional<SendJob> m_send;     // one at most: frames are never dropped, the encoder waits
    std::mutex m_jobMutex;
    std::condition_variable m_jobReady;
    std::condition_variable m_jobTaken;
    std::optional<EncodeJob> m_job;  // the newest converted frame, not yet taken by the encoder
    ID3D11Texture2D* m_encoding = nullptr;  // the YUV target the encoder is reading
    // When the encoder started its frame and how long a frame usually takes it: the conversion
    // thread takes the game's next frame just in time for the encoder (see Run).
    std::atomic<int64_t> m_encodeStartNs{0}, m_encodeAvgNs{0};
    // Frame flow since the last report: given by the game, replaced in the bridge before
    // conversion, replaced while waiting for the encoder, encoded; conversion and encoding time,
    // and how long the game's frames were held back so the encoder could keep up.
    std::atomic<uint32_t> m_flowGiven{0}, m_flowReplacedBridge{0}, m_flowReplacedQueue{0}, m_flowEncoded{0};
    std::atomic<int64_t> m_flowConvertNs{0}, m_flowEncodeNs{0}, m_flowHeldNs{0};
    int64_t m_flowSinceNs = 0;
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
