// Steam's virtual speakers and microphone (Steam Streaming Speakers / Microphone), which
// RinCast uses when its own audio driver isn't installed. They exist for x64 Windows only;
// elsewhere every call says so.
#pragma once

#include <string>

namespace rcs::audio {

// What an install did, as the Audio page shows it: an "audio.*" text key (empty when it
// worked) and the system's own error code for the details.
struct SteamSetupResult {
    int code = 0;
    std::string error;
    bool installed = false;  // the drivers went in (perhaps needing a restart): wait for the devices
};

#ifdef _WIN32
// Whether this PC can install Steam's devices at all: they ship for x64 Windows only.
bool SteamDevicesSupported();
// Whether Steam's own driver packages are on this PC (they come with Steam).
bool SteamDriverPackagesPresent();
// Elevates only PowerShell, never the streamer.
SteamSetupResult InstallSteamDevices();
// Downloads Steam's installer from Valve, checks it is signed by Valve and starts it.
// Doesn't wait for it: Steam's setup runs on its own.
SteamSetupResult DownloadAndStartSteamSetup();
#else
inline bool SteamDevicesSupported() { return false; }
inline bool SteamDriverPackagesPresent() { return false; }
inline SteamSetupResult InstallSteamDevices() { return {0, "audio.installUnsupported", false}; }
inline SteamSetupResult DownloadAndStartSteamSetup() { return {0, "audio.installUnsupported", false}; }
#endif

}  // namespace rcs::audio
