// The PC's sound on Linux: the default output's monitor recorded through the PulseAudio API
// (PipeWire's too), Opus-encoded like on every OS (docs/LINUX.md). Not there yet.
#include "audio/audio.h"

#include "rc/platform.h"

namespace rcs {

bool AudioStreamer::Start(rc::Connection* connection, const wchar_t*, int bitrate) {
    m_connection = connection;
    m_bitrate = bitrate;
    RC_LOGW("Streaming the PC's audio isn't implemented on Linux yet");
    return false;
}

void AudioStreamer::Stop() { m_running = false; }
void AudioStreamer::Run() {}

}  // namespace rcs
