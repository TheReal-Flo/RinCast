// RinCast's Monado service (pc/monado): an OpenXR runtime for games, rendering into the VR
// bridge like the SteamVR driver. On Linux it's how PC VR works; on Windows it's a choice next to
// SteamVR. The streamer starts it when a headset launches VR with it chosen (on Windows its
// OpenXR runtime also starts it when a game opens it).
#pragma once

#include <filesystem>
#include <string>

namespace rcs::monado {

// The folder with the service, its OpenXR runtime and the runtime's manifest: "monado" next to
// the streamer, or in the build tree (build/monado/monado).
std::filesystem::path Directory();
std::filesystem::path ServicePath();
// rincast-monado.json, what OpenXR's active runtime points at.
std::filesystem::path Manifest();
bool IsAvailable();

bool IsRunning();
// Starts the service if it isn't running, for the headset display `display` describes (any text
// that changes with it). Monado can't change its display while it runs: one running for another
// display is restarted. False if it couldn't be started.
bool Start(const std::string& display);
// Ends the one this streamer started (the streamer's own images change, or it quits).
void Stop();

}  // namespace rcs::monado
