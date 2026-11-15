#include "util.h"

#include <dpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <fstream>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")

namespace rcs {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::filesystem::path DataDir() {
    static const std::filesystem::path dir = [] {
        PWSTR path = nullptr;
        std::filesystem::path p;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
            p = std::filesystem::path(path) / L"RinCast";
            CoTaskMemFree(path);
        } else {
            p = std::filesystem::temp_directory_path() / L"RinCast";
        }
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p;
    }();
    return dir;
}

std::filesystem::path ExePath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf);
}

std::filesystem::path ExeDir() { return ExePath().parent_path(); }

std::filesystem::path PicturesDir() {
    // The shell knows where Pictures is: it may be moved, renamed or on another drive.
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &known))) {
        std::filesystem::path dir(known);
        CoTaskMemFree(known);
        return dir;
    }
    const char* profile = getenv("USERPROFILE");
    return std::filesystem::path(profile ? profile : "") / "Pictures";
}

bool OpenExternal(const std::string& target) {
    return reinterpret_cast<INT_PTR>(
               ShellExecuteW(nullptr, L"open", Widen(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

void KeepAwake(bool on) {
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED : ES_CONTINUOUS);
}

bool SetStartAtLogin(bool enable) {
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (enable)
        return RegWriteString(HKEY_CURRENT_USER, key, L"RinCast Streamer", L"\"" + ExePath().wstring() + L"\" --minimized");
    RegDeleteValue(HKEY_CURRENT_USER, key, L"RinCast Streamer");  // already gone is fine
    return true;
}

std::optional<std::wstring> RegReadString(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD size = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
        return std::nullopt;
    std::wstring out(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, out.data(), &size) !=
        ERROR_SUCCESS)
        return std::nullopt;
    out.resize(wcslen(out.c_str()));
    return out;
}

std::optional<DWORD> RegReadDword(HKEY root, const wchar_t* key, const wchar_t* value) {
    DWORD data = 0, size = sizeof(data);
    if (RegGetValueW(root, key, value, RRF_RT_REG_DWORD, nullptr, &data, &size) != ERROR_SUCCESS) return std::nullopt;
    return data;
}

bool RegWriteString(HKEY root, const wchar_t* key, const wchar_t* value, const std::wstring& data) {
    return RegSetKeyValueW(root, key, value, REG_SZ, data.c_str(), DWORD((data.size() + 1) * sizeof(wchar_t))) ==
           ERROR_SUCCESS;
}

bool RegDeleteValue(HKEY root, const wchar_t* key, const wchar_t* value) {
    return RegDeleteKeyValueW(root, key, value) == ERROR_SUCCESS;
}

std::string ProtectString(const std::string& plain) {
    if (plain.empty()) return {};
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"RinCast", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string b64 = Base64Encode(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return b64;
}

std::string UnprotectString(const std::string& b64) {
    if (b64.empty()) return {};
    auto bytes = Base64Decode(b64);
    DATA_BLOB in{DWORD(bytes.size()), bytes.data()};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

bool WriteFileAtomic(const std::filesystem::path& path, const std::string& data) {
    const auto tmp = path.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), std::streamsize(data.size()));
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::string ComputerName() {
    wchar_t buf[256];
    DWORD size = 256;
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &size)) return Narrow(buf);
    return "PC";
}

std::string WindowsVersion() {
    const auto build = RegReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                                     L"CurrentBuildNumber")
                           .value_or(L"");
    const int buildNumber = _wtoi(build.c_str());
    return std::string(buildNumber >= 22000 ? "Windows 11" : "Windows 10") + " (" + Narrow(build) + ")";
}

std::string HResultString(HRESULT hr) {
    char buf[32];
    sprintf_s(buf, "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

int RunElevated(const std::wstring& exe, const std::wstring& args, bool wait) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return -1;
    DWORD code = 0;
    if (wait && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 60000);
        GetExitCodeProcess(sei.hProcess, &code);
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return int(code);
}

int RunHidden(const std::wstring& commandLine, DWORD timeoutMs) {
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = DWORD(-1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}

bool IsProcessRunning(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exeName) == 0) {
            found = true;
            break;
        }
    }
    CloseHandle(snap);
    return found;
}

}  // namespace rcs
