// Windows helpers used across the streamer.
#pragma once

#include <windows.h>
#include <wrl/client.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace rcs {

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);

// %LOCALAPPDATA%\RinCast (created on demand).
std::filesystem::path DataDir();
// Directory of the running executable.
std::filesystem::path ExeDir();

std::optional<std::wstring> RegReadString(HKEY root, const wchar_t* key, const wchar_t* value);
std::optional<DWORD> RegReadDword(HKEY root, const wchar_t* key, const wchar_t* value);
bool RegWriteString(HKEY root, const wchar_t* key, const wchar_t* value, const std::wstring& data);
bool RegDeleteValue(HKEY root, const wchar_t* key, const wchar_t* value);

// DPAPI (current user) protection for secrets at rest.
std::string ProtectString(const std::string& plain);    // returns base64
std::string UnprotectString(const std::string& b64);

std::string Base64Encode(const void* data, size_t size);
std::vector<uint8_t> Base64Decode(const std::string& s);

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out);
bool WriteFileAtomic(const std::filesystem::path& path, const std::string& data);

std::string ComputerName();
std::string WindowsVersion();
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

}  // namespace rcs
