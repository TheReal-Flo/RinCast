// PC VR where there's no runtime to stream from yet: macOS, and Linux until the RinCast Monado
// runtime is done (docs/LINUX.md). The stream says it's waiting and never gets a frame.
#include "stream/vr_stream.h"

#include "rc/platform.h"
#include "vr/vr_bridge_host.h"

namespace rcs {

VrStream::VrStream(rc::Connection* connection, VrBridgeHost* bridge, const VrStreamConfig& config,
                   StateHandler onState)
    : m_connection(connection),
      m_bridge(bridge),
      m_config(config),
      m_sender(connection, rc::kVrStreamId, config.codec, config.bitrateKbps, config.fecPercent, config.adaptive),
      m_onState(std::move(onState)) {
    if (config.startBitrateKbps) m_sender.StartAt(config.startBitrateKbps);
}

VrStream::~VrStream() { Stop(); }

bool VrStream::Start() {
    RC_LOGW("VR streaming isn't available on this system yet");
    m_running = true;
    if (m_onState) m_onState("waiting", "", "");
    return true;
}

void VrStream::Stop() { m_running = false; }

}  // namespace rcs
