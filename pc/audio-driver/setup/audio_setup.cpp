// rincast-audio-setup: creates/removes the root-enumerated "RinCast Virtual
// Audio" device and installs its driver (what `devcon install` does).
//
//   rincast-audio-setup install <path\to\RinCastAudio.inf>
//   rincast-audio-setup remove
//   rincast-audio-setup status
//
// Must run elevated. Exit code 0 on success, 3010 when a reboot is required.
#include <windows.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")

static const wchar_t kHardwareId[] = L"ROOT\\RinCastAudio\0";  // REG_MULTI_SZ
static const GUID kMediaClass = {0x4d36e96c, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

static bool HasHardwareId(HDEVINFO set, SP_DEVINFO_DATA& dev) {
    wchar_t buf[512] = {};
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, &type, reinterpret_cast<BYTE*>(buf), sizeof(buf) - 4,
                                           nullptr))
        return false;
    for (const wchar_t* p = buf; *p; p += wcslen(p) + 1)
        if (_wcsicmp(p, L"ROOT\\RinCastAudio") == 0) return true;
    return false;
}

static int CountDevices(bool remove) {
    HDEVINFO set = SetupDiGetClassDevsW(&kMediaClass, nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    SP_DEVINFO_DATA dev{sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        if (!HasHardwareId(set, dev)) continue;
        count++;
        if (remove) {
            if (!SetupDiCallClassInstaller(DIF_REMOVE, set, &dev))
                std::printf("Remove failed: %lu\n", GetLastError());
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return count;
}

// Owns the device info set, and removes the device node it registered unless told to keep it.
struct NewDevice {
    HDEVINFO set = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA dev{sizeof(dev)};
    bool registered = false;

    ~NewDevice() {
        if (registered && !SetupDiCallClassInstaller(DIF_REMOVE, set, &dev))
            std::printf("Could not remove the new device: %lu\n", GetLastError());
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    }
};

static int Install(const wchar_t* infPath) {
    wchar_t fullInf[MAX_PATH];
    GetFullPathNameW(infPath, MAX_PATH, fullInf, nullptr);

    BOOL reboot = FALSE;
    NewDevice created;
    if (CountDevices(false) == 0) {
        created.set = SetupDiCreateDeviceInfoList(&kMediaClass, nullptr);
        if (created.set == INVALID_HANDLE_VALUE) {
            std::printf("SetupDiCreateDeviceInfoList failed: %lu\n", GetLastError());
            return 1;
        }
        if (!SetupDiCreateDeviceInfoW(created.set, L"MEDIA", &kMediaClass, nullptr, nullptr, DICD_GENERATE_ID, &created.dev)) {
            std::printf("SetupDiCreateDeviceInfo failed: %lu\n", GetLastError());
            return 1;
        }
        if (!SetupDiSetDeviceRegistryPropertyW(created.set, &created.dev, SPDRP_HARDWAREID,
                                               reinterpret_cast<const BYTE*>(kHardwareId), sizeof(kHardwareId))) {
            std::printf("Set hardware id failed: %lu\n", GetLastError());
            return 1;
        }
        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, created.set, &created.dev)) {
            std::printf("Register device failed: %lu\n", GetLastError());
            return 1;
        }
        created.registered = true;
    }
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, L"ROOT\\RinCastAudio", fullInf, INSTALLFLAG_FORCE, &reboot)) {
        const DWORD err = GetLastError();
        std::printf("Driver installation failed: 0x%08lx%s\n", err,
                    err == 0xE0000247 || err == ERROR_FILE_NOT_FOUND
                        ? " (driver signature rejected: enable test signing or use a Microsoft-signed package)"
                        : "");
        return 2;  // ~NewDevice removes the device node this run created
    }
    created.registered = false;  // installed: the device stays
    std::printf("RinCast Virtual Audio installed%s\n", reboot ? " (reboot required)" : "");
    return reboot ? 3010 : 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && _wcsicmp(argv[1], L"install") == 0) return Install(argv[2]);
    if (argc >= 2 && _wcsicmp(argv[1], L"remove") == 0) {
        const int n = CountDevices(true);
        std::printf("Removed %d device(s)\n", n);
        return 0;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"status") == 0) {
        const int n = CountDevices(false);
        std::printf("%d RinCast audio device(s) present\n", n);
        return n > 0 ? 0 : 1;
    }
    std::printf("usage: rincast-audio-setup install <inf> | remove | status\n");
    return 1;
}
