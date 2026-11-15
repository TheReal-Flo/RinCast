// What the session layer asks of Windows-only services, answered on Linux and macOS: the
// firewall (none managed here), VRCFaceTracking's shared memory (a Windows program), and
// SteamVR (no driver can take frames from it there). Steam's libraries are real: the games
// list reads them.
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

#include "net/firewall.h"
#include "rc/platform.h"
#include "util.h"
#include "vr/face_share.h"
#include "vr/monado.h"
#include "vr/steamvr.h"

namespace rcs {

namespace firewall {

// Nothing to open: Linux distributions ship with incoming traffic allowed (ufw or firewalld,
// where on, are the user's to set), and macOS asks the user itself the first time.
Status Query(const std::wstring&) { return {}; }
bool Set(const std::wstring&, bool) { return true; }
bool AllowUsbAdapter(const std::wstring&, const std::string&) { return true; }

}  // namespace firewall

FaceShare& FaceShare::Get() {
    static FaceShare share;
    return share;
}
FaceShare::~FaceShare() = default;
bool FaceShare::Open() { return false; }
void FaceShare::Publish(const rc::FaceTrackingPayload&) {}
void FaceShare::Clear() {}
void FaceShare::Write(const rc::FaceTrackingPayload*) {}

namespace steamvr {

namespace fs = std::filesystem;

std::optional<fs::path> SteamPath() {
    const char* home = std::getenv("HOME");
    if (!home) return std::nullopt;
    for (const char* sub : {"Library/Application Support/Steam", ".local/share/Steam", ".steam/steam",
                            ".var/app/com.valvesoftware.Steam/data/Steam"}) {
        const fs::path p = fs::path(home) / sub;
        if (fs::exists(p / "steamapps")) return p;
    }
    return std::nullopt;
}

std::vector<fs::path> SteamLibraries() {
    std::vector<fs::path> libs;
    auto steam = SteamPath();
    if (!steam) return libs;
    libs.push_back(*steam);
    std::ifstream f(*steam / "steamapps" / "libraryfolders.vdf");
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    static const std::regex pathRe("\"path\"\\s+\"([^\"]+)\"");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pathRe); it != std::sregex_iterator(); ++it) {
        const fs::path lib((*it)[1].str());
        bool dup = false;
        for (const auto& l : libs) {
            std::error_code ec;
            if (fs::equivalent(l, lib, ec)) dup = true;
        }
        if (!dup && fs::exists(lib)) libs.push_back(lib);
    }
    return libs;
}

// SteamVR on Linux and macOS can't hand frames to a driver, so RinCast doesn't use it there.
std::optional<fs::path> SteamVrPath() { return std::nullopt; }
bool IsRunning() { return false; }
void Launch() {}
void WatchForcedDriver() {}
void Shutdown() {}
void Restart() {}
fs::path DriverDirectory() { return {}; }
bool IsDriverRegistered() { return false; }
bool RegisterDriver() { return false; }

}  // namespace steamvr

namespace openxr {

namespace fs = std::filesystem;

namespace {

// ~/.config/openxr/1/active_runtime.json: the user's own choice of OpenXR runtime on Linux, a
// file or a link to the runtime's manifest (the system's in /etc/xdg is the fallback).
fs::path ActiveRuntimeFile() {
    fs::path config;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) config = xdg;
    else if (const char* home = std::getenv("HOME"); home && *home) config = fs::path(home) / ".config";
    else return {};
    return config / "openxr" / "1" / "active_runtime.json";
}

}  // namespace

fs::path RinCastManifest() { return {}; }
std::optional<fs::path> SteamVrManifest() { return std::nullopt; }

std::wstring ActiveRuntime() {
    const fs::path file = ActiveRuntimeFile();
    std::error_code ec;
    if (file.empty() || !fs::exists(file, ec)) return {};
    const fs::path target = fs::is_symlink(file, ec) ? fs::read_symlink(file, ec) : file;
    return Widen(target.string());
}

bool IsRinCastAvailable() { return false; }
bool IsRinCastActive() { return false; }
bool IsSteamVrActive() { return false; }

#ifdef __linux__
bool IsMonadoAvailable() { return monado::IsAvailable(); }

bool IsMonadoActive() {
    std::error_code ec;
    const fs::path file = ActiveRuntimeFile();
    return monado::IsAvailable() && !file.empty() && fs::equivalent(file, monado::Manifest(), ec);
}

// Points the user's active runtime at RinCast's Monado. What was there before is kept next to it
// (active_runtime.json.before-rincast), to go back to by hand.
bool SetActiveRuntime(const std::string& target) {
    if (target != "monado" || !monado::IsAvailable()) return false;
    if (IsMonadoActive()) return true;
    const fs::path file = ActiveRuntimeFile();
    if (file.empty()) return false;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (fs::exists(fs::symlink_status(file, ec))) {
        fs::path backup = file;
        backup += ".before-rincast";
        fs::remove(backup, ec);
        fs::rename(file, backup, ec);
        if (ec) return false;
    }
    fs::create_symlink(monado::Manifest(), file, ec);
    if (ec) RC_LOGW("OpenXR: couldn't make RinCast the active runtime: %s", ec.message().c_str());
    return !ec;
}
#else
// macOS: no OpenXR runtime to stream from.
bool IsMonadoAvailable() { return false; }
bool IsMonadoActive() { return false; }
bool SetActiveRuntime(const std::string&) { return false; }
#endif

}  // namespace openxr

}  // namespace rcs
