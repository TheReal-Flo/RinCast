// SteamVR / Steam / OpenXR runtime integration helpers.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rcs::steamvr {

std::optional<std::filesystem::path> SteamPath();
std::vector<std::filesystem::path> SteamLibraries();
std::optional<std::filesystem::path> SteamVrPath();

bool IsRunning();
void Launch();
void Shutdown();
void Restart();

// Directory containing driver.vrdrivermanifest (installed next to the streamer).
std::filesystem::path DriverDirectory();
bool IsDriverRegistered();
bool RegisterDriver();

}  // namespace rcs::steamvr

namespace rcs::openxr {

std::filesystem::path RinCastManifest();
std::optional<std::filesystem::path> SteamVrManifest();
std::wstring ActiveRuntime();
bool IsRinCastAvailable();  // public builds don't include RinCast XR
bool IsRinCastActive();
bool IsSteamVrActive();
// Requires elevation (UAC prompt). target: "rincast" | "steamvr"
bool SetActiveRuntime(const std::string& target);

}  // namespace rcs::openxr
