// Helpers used across the streamer. The registry, COM and process helpers are Windows only;
// the rest is implemented for Linux and macOS in platform/posix/util_posix.cpp,
// platform/linux/util_linux.cpp and platform/mac/util_mac.mm.
#pragma once

#include "win_compat.h"

#ifdef _WIN32
#include <wrl/client.h>
#endif

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace rcs {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;
#endif
using json = nlohmann::json;

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);

// %LOCALAPPDATA%\RinCast; on Linux $XDG_DATA_HOME/rincast, on macOS
// ~/Library/Application Support/RinCast (created on demand).
std::filesystem::path DataDir();
// The running executable, and its directory.
std::filesystem::path ExePath();
std::filesystem::path ExeDir();
// The user's Pictures folder (wherever the system keeps it), where screenshots go by default.
std::filesystem::path PicturesDir();

// Opens a URL (also steam://) or a folder with what the system uses for it.
bool OpenExternal(const std::string& target);
// Starts the streamer, minimized, when the user logs in: the registry's Run key on Windows, an
// XDG autostart entry on Linux, a launch agent on macOS.
bool SetStartAtLogin(bool enable);
// Keeps the PC and its displays from sleeping while on (a headset streams from it). On Windows
// per thread, as SetThreadExecutionState is: call it again where it was turned on.
void KeepAwake(bool on);

// Protection for secrets at rest: DPAPI (current user) on Windows.
std::string ProtectString(const std::string& plain);    // returns base64
std::string UnprotectString(const std::string& b64);

std::string Base64Encode(const void* data, size_t size);
std::vector<uint8_t> Base64Decode(const std::string& s);

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out);
bool WriteFileAtomic(const std::filesystem::path& path, const std::string& data);

std::string ComputerName();
// The operating system and its version, for the headset's PC info ("Windows 11 24H2", "Ubuntu 24.04",
// "macOS 15.3").
std::string WindowsVersion();

#ifdef _WIN32
std::optional<std::wstring> RegReadString(HKEY root, const wchar_t* key, const wchar_t* value);
std::optional<DWORD> RegReadDword(HKEY root, const wchar_t* key, const wchar_t* value);
bool RegWriteString(HKEY root, const wchar_t* key, const wchar_t* value, const std::wstring& data);
bool RegDeleteValue(HKEY root, const wchar_t* key, const wchar_t* value);

std::string HResultString(HRESULT hr);

// Runs a process elevated (UAC prompt). Returns exit code or -1.
int RunElevated(const std::wstring& exe, const std::wstring& args, bool wait);
// Runs a process hidden and waits. Returns exit code or -1.
int RunHidden(const std::wstring& commandLine, DWORD timeoutMs = 30000);

bool IsProcessRunning(const wchar_t* exeName);

class ScopedCoInit {
   public:
    explicit ScopedCoInit(DWORD model = COINIT_MULTITHREADED) { m_hr = CoInitializeEx(nullptr, model); }
    ~ScopedCoInit() {
        if (SUCCEEDED(m_hr)) CoUninitialize();
    }

   private:
    HRESULT m_hr;
};
#endif

}  // namespace rcs
