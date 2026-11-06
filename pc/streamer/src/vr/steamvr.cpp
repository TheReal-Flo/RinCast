#include "steamvr.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <fstream>
#include <regex>
#include <sstream>

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

void Launch() { ShellExecuteW(nullptr, L"open", L"steam://rungameid/250820", nullptr, nullptr, SW_SHOWNORMAL); }

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
