// Streams one monitor: capture -> colour convert -> encode -> send.
#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "../capture/desktop_capture.h"
#include "../encode/color_convert.h"
#include "../encode/encoder.h"
#include "video_sender.h"

namespace rcs {

struct DesktopStreamConfig {
    rc::VideoCodec codec = rc::VideoCodec::HEVC;
    bool tenBit = false;
    uint32_t width = 0;
    uint32_t height = 0;
    float fps = 60;
    uint32_t bitrateKbps = 20000;
    int fecPercent = 5;
    bool adaptive = true;
    uint32_t startBitrateKbps = 0;  // adaptive: where to start (0 = at bitrateKbps)
    bool pace = false;              // internet paths: no bursts
    bool adaptiveQuant = true;
};

class DesktopStream {
   public:
    DesktopStream(rc::Connection* connection, uint8_t streamId, const MonitorInfo& monitor,
                  const DesktopStreamConfig& config);
    ~DesktopStream();

    bool Start();
    void Stop();
    void RequestKeyframe() { m_sender.RequestKeyframe(); }
    void OnFeedback(const rc::VideoFeedbackPayload& fb) { m_sender.OnFeedback(fb); }
    // Inactive monitors (not looked at) drop to a low frame rate.
    void SetActive(bool active) { m_active = active; }

    json Describe() const;
    VideoSenderStats Stats() { return m_sender.Stats(); }
    const MonitorInfo& Monitor() const { return m_monitor; }
    bool Failed() const { return m_failed; }

   private:
    void Run();

    rc::Connection* m_connection;
    uint8_t m_streamId;
    MonitorInfo m_monitor;
    DesktopStreamConfig m_config;
    VideoSender m_sender;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_active{true};
    std::atomic<bool> m_failed{false};
};

}  // namespace rcs
