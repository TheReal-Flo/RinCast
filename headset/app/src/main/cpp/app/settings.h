// Persistent headset settings (<files dir>/settings.json).
#pragma once

#include <string>

#include "nlohmann/json.hpp"

namespace rv {

struct HeadsetSettings {
    // Environment
    std::string environment = "space-violet";
    float skyBrightness = 1.0f;

    // Streaming (sent to the PC)
    std::string vrQuality = "high";     // low | medium | high | ultra | godlike
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
    std::string cloudServer = "https://rincast.justfeli.dev";
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

    static HeadsetSettings Load(const std::string& path);
    void Save(const std::string& path) const;
};

}  // namespace rv
