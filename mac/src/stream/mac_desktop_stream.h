#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "../capture/mac_capture.h"
#include "../capture/mac_monitors.h"
#include "../encode/vt_encoder.h"
#include "nlohmann/json.hpp"
#include "rc/connection.h"
#include "rc/protocol.h"
#include "rc/video_transport.h"

namespace rcm {

using json = nlohmann::json;

struct DesktopStreamConfig {
    rc::VideoCodec codec = rc::VideoCodec::HEVC;
    uint32_t width = 0;
    uint32_t height = 0;
    float fps = 60.0f;
    uint32_t bitrateKbps = 25000;
    int fecPercent = 5;
    bool pace = false;
};

class MacDesktopStream {
   public:
    MacDesktopStream(rc::Connection* connection, uint8_t streamId, const MacMonitorInfo& monitor,
                     const DesktopStreamConfig& config);
    ~MacDesktopStream();

    bool Start();
    void Stop();

    void RequestKeyframe();
    void OnFeedback(const rc::VideoFeedbackPayload& fb);
    void SetActive(bool active);

    json Describe() const;
    const MacMonitorInfo& Monitor() const { return m_monitor; }
    uint8_t StreamId() const { return m_streamId; }

   private:
    void HandleFrame(CVPixelBufferRef pixelBuffer, uint64_t timestampNs);

    uint8_t m_streamId = 0;
    MacMonitorInfo m_monitor;
    DesktopStreamConfig m_config;

    rc::VideoPacketizer m_packetizer;
    VideoToolboxEncoder m_encoder;
    MacDesktopCapture m_capture;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_active{true};
    std::atomic<bool> m_forceKeyframe{true};
    std::atomic<uint32_t> m_frameIndex{0};
};

}  // namespace rcm
