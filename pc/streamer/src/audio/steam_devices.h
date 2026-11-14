#pragma once

namespace rcs::audio {
// Whether this PC can install Steam's devices at all: they ship for x64 Windows only.
bool SteamDevicesSupported();
// Whether Steam's own driver packages are on this PC (they come with Steam).
bool SteamDriverPackagesPresent();
// Elevates only PowerShell, never the streamer. Returns a Win32 error (0 on success).
int InstallSteamDevices();
// Downloads Steam's installer from Valve, checks it is signed by Valve and starts it.
// Doesn't wait for it: Steam's setup runs on its own. Returns a Win32 error (0 = started).
int DownloadAndStartSteamSetup();
}
