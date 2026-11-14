#include "steam_devices.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <softpub.h>
#include <urlmon.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <filesystem>
#include <string>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "wintrust.lib")

namespace rcs::audio {

namespace {

// Valve's own link to the installer, the one store.steampowered.com/about offers.
constexpr wchar_t kSteamSetupUrl[] = L"https://cdn.fastly.steamstatic.com/client/installer/SteamSetup.exe";

std::filesystem::path SteamDriverDir() {
    PWSTR common = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFilesCommonX86, 0, nullptr, &common)))
        dir = std::filesystem::path(common) / L"Steam" / L"drivers" / L"Windows10" / L"x64";
    CoTaskMemFree(common);
    return dir;
}

// A valid Authenticode signature whose signer is Valve. Anything else isn't started.
bool SignedByValve(const std::wstring& file) {
    WINTRUST_FILE_INFO fileInfo{sizeof(fileInfo)};
    fileInfo.pcwszFilePath = file.c_str();
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool valve = false;
    if (WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data) == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        if (cert && cert->pCert) {
            wchar_t org[256] = {};
            CertGetNameStringW(cert->pCert, CERT_NAME_ATTR_TYPE, 0, const_cast<char*>(szOID_ORGANIZATION_NAME), org,
                               ARRAYSIZE(org));
            valve = std::wstring(org) == L"Valve Corp.";
        }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return valve;
}

}  // namespace

bool SteamDevicesSupported() {
    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);
    return systemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
}

bool SteamDriverPackagesPresent() {
    const auto dir = SteamDriverDir();
    std::error_code ec;
    return !dir.empty() && std::filesystem::exists(dir / L"SteamStreamingSpeakers.inf", ec) &&
           std::filesystem::exists(dir / L"SteamStreamingMicrophone.inf", ec);
}

int DownloadAndStartSteamSetup() {
    wchar_t temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp)) return int(GetLastError());
    const std::filesystem::path dir = std::filesystem::path(temp) / L"RinCast";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::wstring file = (dir / L"SteamSetup.exe").wstring();
    // Never start an older or foreign copy that happens to sit there.
    DeleteFileW(file.c_str());
    if (const HRESULT hr = URLDownloadToFileW(nullptr, kSteamSetupUrl, file.c_str(), 0, nullptr); FAILED(hr))
        return int(hr);
    if (!SignedByValve(file)) {
        DeleteFileW(file.c_str());
        return int(TRUST_E_NOSIGNATURE);
    }
    // Steam's setup asks for administrator rights itself and shows Steam's own terms.
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.lpVerb = L"open";
    sei.lpFile = file.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return int(GetLastError());
    return 0;
}

int InstallSteamDevices() {
    if (!SteamDevicesSupported()) return ERROR_NOT_SUPPORTED;
    // No downloaded scripts, user-supplied paths or command interpolation. Resolve
    // the Steam service's shared driver directory inside the elevated process.
    static constexpr wchar_t script[] = LR"PS(
$ErrorActionPreference = 'Stop'
try {
    $dir = Join-Path ${env:CommonProgramFiles(x86)} 'Steam\drivers\Windows10\x64'
    $files = @('SteamStreamingSpeakers.inf', 'SteamStreamingMicrophone.inf')
    foreach ($file in $files) {
        if (!(Test-Path -LiteralPath (Join-Path $dir $file))) { exit 2 }
    }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SteamAudioInstaller {
    [DllImport("newdev.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool DiInstallDriverW(IntPtr hwnd, string inf, uint flags,
        [MarshalAs(UnmanagedType.Bool)] out bool reboot);
}
'@
    $needsReboot = $false
    foreach ($file in $files) {
        $reboot = $false
        if (![SteamAudioInstaller]::DiInstallDriverW([IntPtr]::Zero, (Join-Path $dir $file), 0, [ref]$reboot)) {
            exit [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        }
        $needsReboot = $needsReboot -or $reboot
    }
    if ($needsReboot) { exit 3010 }
    exit 0
} catch { exit 1 }
)PS";
    DWORD size = 0;
    const auto* bytes = reinterpret_cast<const BYTE*>(script);
    const DWORD byteCount = sizeof(script) - sizeof(wchar_t);
    if (!CryptBinaryToStringW(bytes, byteCount, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size))
        return int(GetLastError());
    std::wstring encoded(size, L'\0');
    if (!CryptBinaryToStringW(bytes, byteCount, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded.data(), &size))
        return int(GetLastError());
    encoded.resize(size);
    if (!encoded.empty() && encoded.back() == L'\0') encoded.pop_back();
    wchar_t system[MAX_PATH];
    if (!GetSystemDirectoryW(system, MAX_PATH)) return int(GetLastError());
    const std::wstring exe = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    const std::wstring args = L"-NoProfile -NonInteractive -EncodedCommand " + encoded;
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return int(GetLastError());
    if (!sei.hProcess) return ERROR_INVALID_HANDLE;
    // Stay on the worker until this installer exits, even if the UI request
    // times out: another click must not start a second elevated installation.
    const DWORD waited = WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = ERROR_GEN_FAILURE;
    if (waited == WAIT_OBJECT_0) GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return int(code);
}

}  // namespace rcs::audio
