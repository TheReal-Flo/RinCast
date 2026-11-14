#pragma once

namespace rcs::audio {
// Elevates only PowerShell, never the streamer. Returns a Win32 error (0 on success).
int InstallSteamDevices();
}
