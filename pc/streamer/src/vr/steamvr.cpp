#include "steamvr.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#include "../settings.h"
#include "../util.h"
#include "rc/platform.h"

namespace rcs::steamvr {

namespace fs = std::filesystem;

std::optional<fs::path> SteamPath() {
    if (auto p = RegReadString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath")) {
        fs::path path(*p);
        if (fs::exists(path)) return path.make_preferred();
    }
    if (auto p = RegReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath")) {
        if (fs::exists(*p)) return fs::path(*p);
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
        std::string p = (*it)[1].str();
        // VDF escapes backslashes.
        std::string unescaped;
        for (size_t i = 0; i < p.size(); i++) {
            if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') i++;
            unescaped += p[i];
        }
        fs::path lib = fs::path(Widen(unescaped));
        bool dup = false;
        for (const auto& l : libs) {
            std::error_code ec;
            if (fs::equivalent(l, lib, ec)) dup = true;
        }
        if (!dup && fs::exists(lib)) libs.push_back(lib);
    }
    return libs;
}

std::optional<fs::path> SteamVrPath() {
    for (const auto& lib : SteamLibraries()) {
        const auto p = lib / "steamapps" / "common" / "SteamVR";
        if (fs::exists(p / "bin" / "win64" / "vrserver.exe")) return p;
    }
    return std::nullopt;
}

bool IsRunning() { return IsProcessRunning(L"vrserver.exe") || IsProcessRunning(L"vrmonitor.exe"); }

static fs::path OpenVrPathsFile();
static void ForceRinCastDriver();
static void StartForcedDriverWatch();
static std::atomic<int64_t> g_launchNs{0};

void Launch() {
    g_launchNs = rc::NowNs();
    ForceRinCastDriver();
    ShellExecuteW(nullptr, L"open", L"steam://rungameid/250820", nullptr, nullptr, SW_SHOWNORMAL);
}

void Shutdown() {
    static const wchar_t* procs[] = {L"vrmonitor.exe", L"vrcompositor.exe", L"vrdashboard.exe",
                                     L"vrwebhelper.exe", L"vrstartup.exe", L"vrserver.exe"};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        for (auto* name : procs) {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID)) {
                    TerminateProcess(h, 0);
                    CloseHandle(h);
                }
            }
        }
    }
    CloseHandle(snap);
}

void Restart() {
    Shutdown();
    for (int i = 0; i < 50 && IsRunning(); i++) Sleep(100);
    Sleep(500);
    Launch();
}

fs::path DriverDirectory() {
    // Installed layout: <install>\RinCast Streamer.exe and <install>\steamvr\rincast\driver.vrdrivermanifest
    const fs::path installed = ExeDir() / "driver" / "rincast";
    if (fs::exists(installed / "driver.vrdrivermanifest")) return installed;
    // Development build layout.
    const fs::path dev = ExeDir() / ".." / "driver" / "rincast";
    if (fs::exists(dev / "driver.vrdrivermanifest")) return fs::weakly_canonical(dev);
    return installed;
}

static fs::path OpenVrPathsFile() {
    wchar_t buf[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return fs::path(buf) / "openvr" / "openvrpaths.vrpath";
}

bool IsDriverRegistered() {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(OpenVrPathsFile(), bytes)) return false;
    try {
        const json j = json::parse(bytes.begin(), bytes.end());
        const fs::path want = DriverDirectory();
        for (const auto& d : j.value("external_drivers", json::array())) {
            std::error_code ec;
            if (fs::equivalent(fs::path(Widen(d.get<std::string>())), want, ec)) return true;
        }
    } catch (...) {
    }
    return false;
}

bool RegisterDriver() {
    if (IsDriverRegistered()) return true;
    const fs::path dir = DriverDirectory();
    if (!fs::exists(dir / "driver.vrdrivermanifest")) {
        RC_LOGW("SteamVR driver not found at %s", Narrow(dir.wstring()).c_str());
        return false;
    }
    // Preferred: vrpathreg, which also handles SteamVR's own bookkeeping.
    if (auto svr = SteamVrPath()) {
        const fs::path vrpathreg = *svr / "bin" / "win64" / "vrpathreg.exe";
        if (fs::exists(vrpathreg)) {
            RunHidden(L"\"" + vrpathreg.wstring() + L"\" adddriver \"" + dir.wstring() + L"\"");
            if (IsDriverRegistered()) return true;
        }
    }
    // Fallback: edit openvrpaths.vrpath directly.
    std::vector<uint8_t> bytes;
    json j = json::object();
    if (ReadFileBytes(OpenVrPathsFile(), bytes)) {
        try {
            j = json::parse(bytes.begin(), bytes.end());
        } catch (...) {
        }
    }
    auto drivers = j.value("external_drivers", json::array());
    if (!drivers.is_array()) drivers = json::array();
    drivers.push_back(Narrow(dir.wstring()));
    j["external_drivers"] = drivers;
    std::error_code ec;
    fs::create_directories(OpenVrPathsFile().parent_path(), ec);
    return WriteFileAtomic(OpenVrPathsFile(), j.dump(1, '\t'));
}

// ---- Making sure SteamVR started by RinCast runs RinCast's driver ----
//
// SteamVR loads the first registered driver of each name and skips later ones, and picks
// whichever driver finds a headset first unless steamvr.vrsettings names one ("forcedDriver").
// With a second RinCast copy registered (an older install, a development build) or another
// streaming app's driver, it could load a driver that doesn't match this streamer, or none of
// ours. Before RinCast launches it: this streamer's driver goes first in the list, and the
// setting names it. The setting is put back once SteamVR has closed, so SteamVR started
// without RinCast picks its headset as before.

static std::mutex g_forceMutex;  // the two files below, and the note of what was there before

static fs::path ForcedNoteFile() { return DataDir() / L"steamvr-forced-driver.json"; }

static fs::path SteamVrSettingsFile() {
    std::vector<uint8_t> bytes;
    if (ReadFileBytes(OpenVrPathsFile(), bytes)) {
        try {
            const json j = json::parse(bytes.begin(), bytes.end());
            const auto config = j.value("config", json::array());
            if (config.is_array() && !config.empty() && config[0].is_string())
                return fs::path(Widen(config[0].get<std::string>())) / "steamvr.vrsettings";
        } catch (...) {
        }
    }
    if (auto steam = SteamPath()) return *steam / "config" / "steamvr.vrsettings";
    return {};
}

static bool ReadJsonFile(const fs::path& path, json& out) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, bytes)) return false;
    try {
        out = json::parse(bytes.begin(), bytes.end());
        return out.is_object();
    } catch (...) {
        return false;
    }
}

static std::string DriverName(const fs::path& dir) {
    json manifest;
    if (!ReadJsonFile(dir / "driver.vrdrivermanifest", manifest)) return {};
    return manifest.value("name", std::string());
}

// This streamer's driver first among the registered ones (registered if it isn't). Others stay
// registered: a duplicate after it is skipped by SteamVR, and their own streamer moves its copy
// first again when it launches SteamVR.
static void PutOurDriverFirst() {
    const fs::path ours = DriverDirectory();
    if (!fs::exists(ours / "driver.vrdrivermanifest")) return;
    if (!IsDriverRegistered() && !RegisterDriver()) return;
    json j;
    if (!ReadJsonFile(OpenVrPathsFile(), j)) return;
    auto drivers = j.value("external_drivers", json::array());
    if (!drivers.is_array()) return;
    json reordered = json::array();
    std::string ourEntry;
    int ourIndex = -1, firstRinCast = -1;
    for (size_t i = 0; i < drivers.size(); i++) {
        if (!drivers[i].is_string()) continue;
        const fs::path dir(Widen(drivers[i].get<std::string>()));
        std::error_code ec;
        if (fs::equivalent(dir, ours, ec)) {
            ourIndex = int(i);
            ourEntry = drivers[i].get<std::string>();
        } else if (firstRinCast < 0 && DriverName(dir) == "rincast") {
            firstRinCast = int(i);
        }
    }
    if (ourIndex < 0 || firstRinCast < 0 || ourIndex < firstRinCast) return;  // already the one loaded
    reordered.push_back(ourEntry);
    for (size_t i = 0; i < drivers.size(); i++)
        if (int(i) != ourIndex) reordered.push_back(drivers[i]);
    j["external_drivers"] = reordered;
    if (WriteFileAtomic(OpenVrPathsFile(), j.dump(1, '\t')))
        RC_LOGI("SteamVR: RinCast's driver at %s now comes before the other RinCast copy registered",
                Narrow(ours.wstring()).c_str());
}

static void ForceRinCastDriver() {
    StartForcedDriverWatch();
    if (IsRunning()) return;  // it keeps its settings in memory and would write them back
    std::lock_guard lock(g_forceMutex);
    if (SettingsStore::Get().Snapshot().autoRegisterSteamVrDriver) PutOurDriverFirst();
    if (!IsDriverRegistered()) return;  // forcing a driver it can't find leaves it without a headset
    const fs::path file = SteamVrSettingsFile();
    if (file.empty()) return;
    json settings = json::object();
    ReadJsonFile(file, settings);  // missing: SteamVR never ran with settings changed
    json& steamvr = settings["steamvr"];
    if (!steamvr.is_object()) steamvr = json::object();
    // What to put back: noted only once, so a launch after one that wasn't cleaned up (the
    // streamer closed while SteamVR ran) doesn't take our own value for the user's.
    if (!fs::exists(ForcedNoteFile())) {
        const json previous = steamvr.contains("forcedDriver") ? steamvr["forcedDriver"] : json();
        if (!WriteFileAtomic(ForcedNoteFile(), json{{"previous", previous}}.dump())) return;
    }
    steamvr["forcedDriver"] = "rincast";
    // A driver SteamVR blocked after a crash (safe mode) or that was turned off stays off.
    json& ours = settings["driver_rincast"];
    if (!ours.is_object()) ours = json::object();
    ours["enable"] = true;
    ours["blocked_by_safe_mode"] = false;
    if (WriteFileAtomic(file, settings.dump(3))) RC_LOGI("SteamVR: starting with RinCast's driver forced");
}

static void RestoreForcedDriver(int64_t quietNs) {
    std::lock_guard lock(g_forceMutex);
    // Launched again meanwhile: that launch needs it.
    if (IsRunning() || rc::NowNs() - g_launchNs.load() < quietNs) return;
    json note;
    if (!ReadJsonFile(ForcedNoteFile(), note)) return;
    const fs::path file = SteamVrSettingsFile();
    json settings;
    if (!file.empty() && ReadJsonFile(file, settings) && settings.contains("steamvr") && settings["steamvr"].is_object()) {
        json& steamvr = settings["steamvr"];
        // Changed since (in SteamVR's settings, by the user): theirs stays.
        if (steamvr.value("forcedDriver", std::string()) == "rincast") {
            const json previous = note.value("previous", json());
            if (previous.is_null()) steamvr.erase("forcedDriver");
            else steamvr["forcedDriver"] = previous;
            if (!WriteFileAtomic(file, settings.dump(3))) return;  // the note stays: tried again
            RC_LOGI("SteamVR closed: its driver choice is back to what it was");
        }
    }
    std::error_code ec;
    fs::remove(ForcedNoteFile(), ec);
}

// One thread for the streamer's lifetime: once SteamVR started by RinCast has closed (or never
// came up), the forced driver is put back. Also cleans up after a streamer that closed while
// SteamVR ran.
static void StartForcedDriverWatch() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            bool seenRunning = false;
            for (;;) {
                Sleep(2000);
                if (!fs::exists(ForcedNoteFile())) {
                    seenRunning = false;
                    continue;
                }
                if (IsRunning()) {
                    seenRunning = true;
                    continue;
                }
                // Steam takes a while to start it; and a restart (Restart) closes it first.
                const int64_t quiet = seenRunning ? 20'000'000'000LL : 120'000'000'000LL;
                if (rc::NowNs() - g_launchNs.load() < quiet) continue;
                RestoreForcedDriver(quiet);
                seenRunning = false;
            }
        }).detach();
    });
}

void WatchForcedDriver() { StartForcedDriverWatch(); }

}  // namespace rcs::steamvr

namespace rcs::openxr {

namespace fs = std::filesystem;
static const wchar_t* kKey = L"SOFTWARE\\Khronos\\OpenXR\\1";

fs::path RinCastManifest() {
    const fs::path installed = ExeDir() / "openxr" / "rincast-openxr.json";
    if (fs::exists(installed)) return installed;
    return fs::weakly_canonical(ExeDir() / ".." / "openxr" / "rincast-openxr.json");
}

std::optional<fs::path> SteamVrManifest() {
    if (auto svr = steamvr::SteamVrPath()) {
        const auto p = *svr / "steamxr_win64.json";
        if (fs::exists(p)) return p;
    }
    return std::nullopt;
}

std::wstring ActiveRuntime() { return RegReadString(HKEY_LOCAL_MACHINE, kKey, L"ActiveRuntime").value_or(L""); }

static bool SamePath(const std::wstring& a, const fs::path& b) {
    std::error_code ec;
    return !a.empty() && fs::equivalent(fs::path(a), b, ec);
}

bool IsRinCastAvailable() { return fs::exists(RinCastManifest()); }

bool IsRinCastActive() { return SamePath(ActiveRuntime(), RinCastManifest()); }

bool IsSteamVrActive() {
    auto m = SteamVrManifest();
    return m && SamePath(ActiveRuntime(), *m);
}

bool SetActiveRuntime(const std::string& target) {
    fs::path manifest;
    if (target == "rincast") {
        manifest = RinCastManifest();
        // Public builds don't include it. Pointing Windows at a missing manifest would
        // break OpenXR for every app.
        if (!fs::exists(manifest)) return false;
    } else if (auto m = SteamVrManifest()) {
        manifest = *m;
    } else {
        return false;
    }
    if (SamePath(ActiveRuntime(), manifest)) return true;
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring args = L"add \"HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\" /v ActiveRuntime /t REG_SZ /d \"" +
                              manifest.wstring() + L"\" /f /reg:64";
    RunElevated(std::wstring(system) + L"\\reg.exe", args, true);
    return SamePath(ActiveRuntime(), manifest);
}

}  // namespace rcs::openxr
