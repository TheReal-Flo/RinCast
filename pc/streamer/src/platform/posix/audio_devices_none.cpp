// Audio devices on Linux and macOS: no virtual speaker or microphone of RinCast's own yet, so
// sound comes from the default output and the headset microphone has nowhere to go
// (docs/LINUX.md plans a PulseAudio null sink for it). Streaming the PC's sound is per platform:
// audio_linux.cpp, audio_mac.mm.
#include "audio/audio.h"

namespace rcs {

namespace audio {

const DeviceNames& Names(DeviceSet) {
    static const DeviceNames names{L"RinCast Speaker", L"RinCast Microphone", L"RinCast Microphone Feed"};
    return names;
}

DriverStatus QueryDriver(DeviceSet) { return {}; }

Route ResolveRoute(const std::string&) { return {}; }

void SwitchDefaults(const Route&, bool, bool) {}
void RestoreDefaults() {}

}  // namespace audio

bool MicPlayer::Start(const wchar_t*, const wchar_t*) { return false; }
void MicPlayer::Stop() { m_running = false; }
void MicPlayer::PushPacket(const uint8_t*, size_t) {}
void MicPlayer::Run() {}

}  // namespace rcs
