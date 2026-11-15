// Shared plumbing for desktop and VR streams: packetizing, keyframe requests,
// adaptive bitrate (AIMD on loss / queuing delay) and statistics.
#pragma once

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

#include "../encode/encoder.h"
#include "rc/connection.h"
#include "rc/video_transport.h"

namespace rcs {

struct VideoSenderStats {
    double fps = 0;
    double encodeMs = 0;
    double mbps = 0;
    uint32_t bitrateKbps = 0;
    uint32_t framesLost = 0;
    double clientDecodeMs = 0;
    double clientLatencyMs = 0;
    double queueMs = -1;  // queueing delay on the way to the headset, -1 = unknown
};

class VideoSender {
   public:
    VideoSender(rc::Connection* connection, uint8_t streamId, rc::VideoCodec codec, uint32_t maxBitrateKbps,
                int fecPercent, bool adaptive);

    // Sends one encoded frame with metadata.
    void Send(const EncodedFrame& frame, const void* metadata, uint16_t metadataSize, uint8_t extraFlags,
              double encodeMs) {
        Send(frame, metadata, metadataSize, extraFlags, encodeMs, TakeRecovery());
    }
    // For sending on another thread than the encoding: whether the frame BeginFrame was just
    // called for answers a loss (kVideoRecovery). Take it right after encoding, pass it to Send.
    bool TakeRecovery() { return std::exchange(m_nextIsRecovery, false); }
    void Send(const EncodedFrame& frame, const void* metadata, uint16_t metadataSize, uint8_t extraFlags,
              double encodeMs, bool recovery);

    void RequestKeyframe() { m_keyframeRequested = true; }
    // True if the headset is waiting for a frame (keyframe or loss recovery), so one
    // should be encoded even if the picture didn't change.
    bool WantsFrame();
    // Call right before encoding; returns whether to force a keyframe. A loss the
    // headset reported is answered by invalidating the lost frames in the encoder when
    // it can, so the next frame is a normal-sized one flagged kVideoRecovery.
    bool BeginFrame(VideoEncoder& encoder);

    void OnFeedback(const rc::VideoFeedbackPayload& fb);
    // Bitrate the encoder should use now (changes when adaptive).
    uint32_t TargetBitrateKbps() const { return m_targetKbps.load(); }
    void SetMaxBitrate(uint32_t kbps);
    // Internet paths: spread each frame over time instead of sending it in one burst.
    void SetPacing(bool pace) { m_pace = pace; }
    // Adaptive only: begin below the maximum and probe upward.
    void StartAt(uint32_t kbps) {
        if (m_adaptive) m_targetKbps = std::min(kbps, m_maxKbps);
    }

    VideoSenderStats Stats();
    uint8_t StreamId() const { return m_streamId; }

   private:
    rc::Connection* m_connection;
    rc::VideoPacketizer m_packetizer;
    const uint8_t m_streamId;
    const rc::VideoCodec m_codec;
    std::atomic<bool> m_keyframeRequested{true};
    std::atomic<uint32_t> m_targetKbps;
    uint32_t m_maxKbps;
    bool m_adaptive;
    bool m_pace = false;
    double m_lossAvg = 0;
    uint32_t m_frameIndex = 0;

    // Loss recovery (OnFeedback runs on the network thread, the rest on the stream thread).
    std::mutex m_lossMutex;
    bool m_lossPending = false;
    uint32_t m_lossGoodFrame = 0;      // newest frame the headset decoded cleanly
    uint32_t m_lastKeyframe = 0;       // frame index of the newest keyframe sent
    uint32_t m_lastRecovery = 0;       // newest keyframe or recovery frame sent
    int64_t m_lastRecoveryNs = 0;
    bool m_nextIsRecovery = false;
    struct SentFrame {
        uint32_t index = 0;
        uint64_t timestamp = 0;  // EncodedFrame::timestamp
    };
    SentFrame m_sent[64];

    std::mutex m_statsMutex;
    int64_t m_windowStartNs = 0;
    uint32_t m_windowFrames = 0;
    uint64_t m_windowBytes = 0;
    double m_windowEncodeMs = 0;
    VideoSenderStats m_stats;
    int64_t m_lastIncreaseNs = 0;
    int64_t m_lastDecreaseNs = 0;
    double m_baselineNetworkMs = -1;
    double m_queueMs = -1;            // smoothed queueing delay reported by the headset
    double m_queueAtDecreaseMs = 0;
    double m_receivedKbps = 0;        // smoothed rate that actually arrives
};

}  // namespace rcs
