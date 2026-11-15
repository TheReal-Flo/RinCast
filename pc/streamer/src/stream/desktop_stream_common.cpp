// The part of DesktopStream that is the same on every OS. Run(), which captures and encodes, is
// the platform's: desktop_stream.cpp on Windows, platform/mac/desktop_stream_mac.mm on macOS,
// platform/linux/desktop_stream_linux.cpp on Linux.
#include "desktop_stream.h"

namespace rcs {

DesktopStream::DesktopStream(rc::Connection* connection, uint8_t streamId, const MonitorInfo& monitor,
                             const DesktopStreamConfig& config)
    : m_connection(connection),
      m_streamId(streamId),
      m_monitor(monitor),
      m_config(config),
      m_sender(connection, streamId, config.codec, config.bitrateKbps, config.fecPercent, config.adaptive) {
    if (config.startBitrateKbps) m_sender.StartAt(config.startBitrateKbps);
}

DesktopStream::~DesktopStream() { Stop(); }

bool DesktopStream::Start() {
    m_running = true;
    m_thread = std::thread([this] { Run(); });
    return true;
}

void DesktopStream::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

json DesktopStream::Describe() const {
    return {{"streamId", m_streamId},
            {"monitorId", m_monitor.id},
            {"name", m_monitor.name},
            {"index", m_monitor.index},
            {"width", m_config.width},
            {"height", m_config.height},
            {"fps", m_config.fps},
            {"codec", int(m_config.codec)},
            {"primary", m_monitor.primary}};
}

EncoderConfig DesktopStream::EncoderSettings() const {
    EncoderConfig ec;
    ec.codec = m_config.codec;
    ec.width = m_config.width;
    ec.height = m_config.height;
    ec.fps = m_config.fps;
    ec.bitrateKbps = m_sender.TargetBitrateKbps();
    ec.tenBit = m_config.tenBit;
    ec.desktop = true;
    ec.adaptiveQuant = m_config.adaptiveQuant;
    // Latency matters less than sharp text here, so always.
    ec.twoPass = true;
    return ec;
}

}  // namespace rcs
