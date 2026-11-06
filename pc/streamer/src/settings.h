// Persistent streamer settings (%LOCALAPPDATA%\RinCast\settings.json).
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "util.h"

namespace rcs {

struct TrustedDevice {
    std::string id;
    std::string name;
    std::string model;
    int64_t firstSeen = 0;  // unix seconds
    int64_t lastSeen = 0;
    std::string publicKey;  // hex; pinned the first time the headset proves one
};

struct Settings {
    // Identity
    std::string serverId;     // stable random id for this PC
    std::string pcName;

    // General
    bool startWithWindows = true;
    bool startMinimized = true;
    bool preventSleep = true;
    bool requireApproval = true;   // ask before a new headset may connect locally
    std::vector<TrustedDevice> trustedDevices;

    // Video
    std::string codec = "auto";    // auto | h264 | hevc | av1
    bool tenBit = false;
    int vrBitrateMbps = 150;
    int desktopBitrateMbps = 40;
    bool adaptiveBitrate = true;
    int fecPercent = 5;
    bool foveatedEncoding = true;  // VR: fewer bits towards the lens edges
    bool adaptiveQuantization = true;
    bool twoPassEncoding = false;  // VR only; desktop streams always use two passes
    int desktopMaxHeight = 0;      // 0 = native
    int desktopFps = 0;            // 0 = follow headset refresh rate (capped at 90)

    // Audio
    bool streamAudio = true;
    bool micPassthrough = true;
    bool switchDefaultAudio = true;
    bool playOnPcToo = false;
    std::string audioDevices = "auto";  // auto | rincast | steam | system

    // VR
    std::string openxrRuntime = "steamvr";  // rincast | steamvr (what we make active while streaming)
    bool autoRegisterSteamVrDriver = true;

    // Cloud
    std::string cloudUrl = "https://rincast.justfeli.dev";
    bool allowRemote = true;
    std::string cloudRefreshTokenProtected;  // DPAPI
    std::string cloudUserName;
    std::string cloudEmail;

    json ToJson(bool forUi) const;
    void FromJson(const json& j);
};

class SettingsStore {
   public:
    static SettingsStore& Get();

    Settings Snapshot() const;
    void Update(const std::function<void(Settings&)>& mutate);  // mutates + saves
    void Load();

   private:
    void SaveLocked();
    mutable std::mutex m_mutex;
    Settings m_settings;
};

}  // namespace rcs
