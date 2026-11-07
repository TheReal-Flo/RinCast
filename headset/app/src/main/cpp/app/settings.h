// Persistent headset settings (<files dir>/settings.json).
#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "nlohmann/json.hpp"

namespace rv {

inline constexpr float kMinVrScale = 0.5f, kMaxVrScale = 1.3f;

struct HeadsetSettings {
    // Environment
    std::string environment = "space-violet";
    float skyBrightness = 1.0f;

    // Interface
    std::string language = "auto";      // auto | en | zh-CN | de | es | fr | ja

    // Streaming (sent to the PC)
    float vrScale = 0.9f;               // pixels per eye against the headset's own, kMinVrScale..kMaxVrScale
    bool matchSteamVr = false;          // stream at the resolution SteamVR renders at instead
    bool gazeFoveation = false;         // foveated encoding: full detail only where the eyes look
    std::string vrQuality = "high";     // the preset nearest vrScale, for PCs before 1.1.0
    float refreshRate = 90;
    std::string codec = "auto";         // auto | hevc | av1 | h264
    bool tenBit = false;
    int vrBitrateMbps = 0;              // 0 = PC default
    bool adaptiveBitrate = true;
    int desktopFps = 0;                 // 0 = match refresh rate
    bool upscaling = false;             // Snapdragon GSR on the VR picture
    bool frameGeneration = false;       // make a frame from motion when the next is late
    bool bodyTracking = false;          // send the body to SteamVR as trackers
    bool faceTracking = false;          // send the face (and eyes) to the PC, for VRCFaceTracking
    bool handsInGames = false;          // play PC games with tracked hands
    bool fingersWithControllers = false;  // real fingers while holding controllers; not with bodyTracking

    // Desktop
    float curvature = 0.35f;            // 0 flat .. 1 strongly curved
    bool sharpLayers = true;            // monitors as compositor layers
    float screenDistance = 1.35f;

    // Audio
    bool streamAudio = true;
    bool micPassthrough = true;
    float volume = 1.0f;
    float micVolume = 1.0f;             // 0 .. 2
    bool music = true;                  // while no PC is connected

    // Controls
    bool haptics = true;
    float scrollSpeed = 1.0f;
    bool showStats = false;
    float uiScale = 1.0f;               // window size: 1, 1.25 or 1.5
    bool highContrast = false;
    bool uiSounds = true;
    bool readAloud = false;             // speak what the pointer is on
    int vrMenuHints = 0;                // times the "how to open the menu" hint was shown

    // Account
    std::string cloudServer = "https://rincast.com";
    std::string cloudRefreshToken;
    std::string cloudUserName;
    std::string lastPc;
    std::string lastPcAddress;          // where it answered, for networks that drop broadcasts
    bool autoConnect = false;           // connect to lastPc as soon as it's found
    bool usbCable = false;              // use a USB cable to the PC when one is plugged in
    std::string manualPc;               // for networks that block discovery
    std::string deviceSecret;           // identity seed (hex), made on first start

    nlohmann::json ToJson() const;
    void FromJson(const nlohmann::json& j);
    nlohmann::json StreamSettingsJson() const;  // what the PC needs
    void SetVrScale(float scale) {              // and vrQuality to match
        vrScale = std::clamp(std::round(scale * 20.f) / 20.f, kMinVrScale, kMaxVrScale);  // 5% steps
        static const char* presets[] = {"low", "medium", "high", "ultra", "godlike"};
        static const float scales[] = {0.6f, 0.75f, 0.9f, 1.f, 1.2f};
        int nearest = 0;
        for (int i = 1; i < 5; i++)
            if (std::fabs(scales[i] - vrScale) < std::fabs(scales[nearest] - vrScale)) nearest = i;
        vrQuality = presets[nearest];
    }

    static HeadsetSettings Load(const std::string& path);
    void Save(const std::string& path) const;
};

}  // namespace rv
