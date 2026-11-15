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

// TODO (VR step): the RinCast Monado runtime's manifest, and making it the active runtime
// (~/.config/openxr/1/active_runtime.json).
fs::path RinCastManifest() { return {}; }
std::optional<fs::path> SteamVrManifest() { return std::nullopt; }
std::wstring ActiveRuntime() { return {}; }
bool IsRinCastAvailable() { return false; }
bool IsRinCastActive() { return false; }
bool IsSteamVrActive() { return false; }
bool SetActiveRuntime(const std::string&) { return false; }

}  // namespace openxr

}  // namespace rcs
