#include "steam_devices.h"

#include <windows.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <string>

#pragma comment(lib, "crypt32.lib")

namespace rcs::audio {

int InstallSteamDevices() {
    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);
    if (systemInfo.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64)
        return ERROR_NOT_SUPPORTED;
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
