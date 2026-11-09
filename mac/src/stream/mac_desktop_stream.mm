#include "mac_desktop_stream.h"

#include "rc/platform.h"

namespace rcm {

MacDesktopStream::MacDesktopStream(rc::Connection* connection, uint8_t streamId, const MacMonitorInfo& monitor,
                                   const DesktopStreamConfig& config, AudioCallback onAudio)
    : m_streamId(streamId),
      m_monitor(monitor),
      m_config(config),
      m_audioCallback(std::move(onAudio)),
      m_packetizer(connection) {
    m_packetizer.SetFecPercent(config.fecPercent);
    if (config.pace) {
        m_packetizer.SetPacingKbps(static_cast<uint32_t>(config.bitrateKbps * 1.5));
    }
}

MacDesktopStream::~MacDesktopStream() {
    Stop();
}

json MacDesktopStream::Describe() const {
    return {
        {"streamId", m_streamId},
        {"monitorId", m_monitor.id},
        {"name", m_monitor.name},
        {"index", m_monitor.index},
        {"width", m_config.width},
        {"height", m_config.height},
        {"fps", m_config.fps},
        {"codec", static_cast<int>(m_config.codec)},
        {"primary", m_monitor.primary}
    };
}

bool MacDesktopStream::Start() {
    if (m_running) return true;

    if (!m_encoder.Init(m_config.codec, m_config.width, m_config.height, m_config.fps, m_config.bitrateKbps)) {
        RC_LOGE("Failed to initialize VideoToolboxEncoder for monitor %s", m_monitor.name.c_str());
        return false;
    }

    auto onFrame = [this](CVPixelBufferRef pixelBuffer, uint64_t timestampNs) {
        HandleFrame(pixelBuffer, timestampNs);
    };

    if (!m_capture.Init(m_monitor, m_config.width, m_config.height, m_config.fps, onFrame, m_audioCallback)) {
        RC_LOGE("Failed to initialize MacDesktopCapture for monitor %s", m_monitor.name.c_str());
        m_encoder.Shutdown();
        return false;
    }

    if (!m_capture.Start()) {
        RC_LOGE("Failed to start MacDesktopCapture");
        m_encoder.Shutdown();
        return false;
    }

    m_forceKeyframe = true;
    m_running = true;
    RC_LOGI("MacDesktopStream started (stream %u, monitor %s, %ux%u @ %.1f fps%s)",
            m_streamId, m_monitor.name.c_str(), m_config.width, m_config.height, m_config.fps,
            m_audioCallback ? ", with audio" : "");
    return true;
}

void MacDesktopStream::Stop() {
    if (!m_running) return;
    m_running = false;
    m_capture.Stop();
    m_encoder.Shutdown();
    RC_LOGI("MacDesktopStream stopped (stream %u)", m_streamId);
}

void MacDesktopStream::RequestKeyframe() {
    m_forceKeyframe = true;
    m_encoder.RequestKeyframe();
}

void MacDesktopStream::OnFeedback(const rc::VideoFeedbackPayload& fb) {
    if (fb.requestKeyframe) {
        RequestKeyframe();
    }
}

void MacDesktopStream::SetActive(bool active) {
    m_active = active;
}

void MacDesktopStream::HandleFrame(CVPixelBufferRef pixelBuffer, uint64_t timestampNs) {
    if (!m_running) return;

    bool forceKey = m_forceKeyframe.exchange(false);
    EncodedFrame frame;

    if (!m_encoder.Encode(pixelBuffer, timestampNs, forceKey, frame)) {
        return;
    }

    rc::DesktopFrameMetadata meta{};
    meta.width = m_config.width;
    meta.height = m_config.height;
    meta.pcCaptureNs = timestampNs;
    meta.pcEncodeEndNs = rc::NowNs();
    meta.cursorX = -1;  // Baked into the frame by ScreenCaptureKit
    meta.cursorY = -1;

    uint8_t flags = 0;
    if (frame.keyframe) {
        flags |= rc::kVideoKeyframe;
    }

    uint32_t index = ++m_frameIndex;
    m_packetizer.SendFrame(
        m_streamId,
        m_config.codec,
        flags,
        index,
        &meta,
        sizeof(meta),
        frame.data.data(),
        frame.data.size()
    );
}

}  // namespace rcm
