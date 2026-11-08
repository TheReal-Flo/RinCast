#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "rc/platform.h"

namespace rv {

using json = nlohmann::json;

json HeadsetSettings::ToJson() const {
    return {{"environment", environment},
            {"skyBrightness", skyBrightness},
            {"language", language},
            {"vrQuality", vrQuality},
            {"vrScale", vrScale},
            {"matchSteamVr", matchSteamVr},
            {"gazeFoveation", gazeFoveation},
            {"autoConnect", autoConnect},
            {"usbCable", usbCable},
            {"refreshRate", refreshRate},
            {"codec", codec},
            {"tenBit", tenBit},
            {"vrBitrateMbps", vrBitrateMbps},
            {"adaptiveBitrate", adaptiveBitrate},
            {"desktopFps", desktopFps},
            {"curvature", curvature},
            {"sharpLayers", sharpLayers},
            {"desktopInput", desktopInput},
            {"upscaling", upscaling},
            {"frameGeneration", frameGeneration},
            {"bodyTracking", bodyTracking},
            {"faceTracking", faceTracking},
            {"fixedPlaySpace", fixedPlaySpace},
            {"handsInGames", handsInGames},
            {"fingersWithControllers", fingersWithControllers},
            {"screenDistance", screenDistance},
            {"streamAudio", streamAudio},
            {"micPassthrough", micPassthrough},
            {"volume", volume},
            {"micVolume", micVolume},
            {"haptics", haptics},
            {"scrollSpeed", scrollSpeed},
            {"showStats", showStats},
            {"uiScale", uiScale},
            {"highContrast", highContrast},
            {"uiSounds", uiSounds},
            {"music", music},
            {"readAloud", readAloud},
            {"vrMenuHints", vrMenuHints},
            {"manualPc", manualPc},
            {"deviceSecret", deviceSecret},
            {"cloudServer", cloudServer},
            {"cloudRefreshToken", cloudRefreshToken},
            {"cloudUserName", cloudUserName},
            {"lastPc", lastPc},
            {"lastPcAddress", lastPcAddress},
            {"keyboard", keyboard.ToJson()}};
}

void HeadsetSettings::FromJson(const json& j) {
    auto get = [&](const char* k, auto& field) {
        if (j.contains(k)) {
            try {
                field = j[k].get<std::decay_t<decltype(field)>>();
            } catch (...) {
            }
        }
    };
    get("environment", environment);
    get("skyBrightness", skyBrightness);
    get("language", language);
    get("vrQuality", vrQuality);
    // Settings from before the resolution slider: the preset's scale.
    if (!j.contains("vrScale")) {
        static const char* presets[] = {"low", "medium", "high", "ultra", "godlike"};
        static const float scales[] = {0.6f, 0.75f, 0.9f, 1.f, 1.2f};
        for (int i = 0; i < 5; i++)
            if (vrQuality == presets[i]) vrScale = scales[i];
    }
    get("vrScale", vrScale);
    SetVrScale(vrScale);
    get("matchSteamVr", matchSteamVr);
    get("gazeFoveation", gazeFoveation);
    get("autoConnect", autoConnect);
    get("usbCable", usbCable);
    get("refreshRate", refreshRate);
    get("codec", codec);
    get("tenBit", tenBit);
    get("vrBitrateMbps", vrBitrateMbps);
    get("adaptiveBitrate", adaptiveBitrate);
    get("desktopFps", desktopFps);
    get("curvature", curvature);
    get("sharpLayers", sharpLayers);
    get("desktopInput", desktopInput);
    get("upscaling", upscaling);
    get("frameGeneration", frameGeneration);
    get("bodyTracking", bodyTracking);
    get("faceTracking", faceTracking);
    get("fixedPlaySpace", fixedPlaySpace);
    get("handsInGames", handsInGames);
    get("fingersWithControllers", fingersWithControllers);
    get("screenDistance", screenDistance);
    get("streamAudio", streamAudio);
    get("micPassthrough", micPassthrough);
    get("volume", volume);
    get("micVolume", micVolume);
    micVolume = std::clamp(micVolume, 0.f, 2.f);
    get("haptics", haptics);
    get("scrollSpeed", scrollSpeed);
    get("showStats", showStats);
    get("uiScale", uiScale);
    get("highContrast", highContrast);
    get("uiSounds", uiSounds);
    get("music", music);
    get("readAloud", readAloud);
    get("vrMenuHints", vrMenuHints);
    get("manualPc", manualPc);
    get("deviceSecret", deviceSecret);
    get("cloudServer", cloudServer);
    get("cloudRefreshToken", cloudRefreshToken);
    get("cloudUserName", cloudUserName);
    get("lastPc", lastPc);
    get("lastPcAddress", lastPcAddress);
    if (j.contains("keyboard")) keyboard.FromJson(j["keyboard"]);
}

json HeadsetSettings::StreamSettingsJson() const {
    return {{"codec", codec},
            {"tenBit", tenBit},
            {"vrQuality", vrQuality},
            {"vrScale", vrScale},
            {"matchSteamVr", matchSteamVr},
            {"gazeFoveation", gazeFoveation},
            {"autoConnect", autoConnect},
            {"frameGeneration", frameGeneration},
            {"refreshRate", refreshRate},
            {"vrBitrateMbps", vrBitrateMbps},
            {"adaptiveBitrate", adaptiveBitrate},
            {"desktopFps", desktopFps},
            {"streamAudio", streamAudio},
            {"micPassthrough", micPassthrough}};
}

HeadsetSettings HeadsetSettings::Load(const std::string& path) {
    HeadsetSettings s;
    std::ifstream f(path);
    if (!f) return s;
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        s.FromJson(json::parse(ss.str()));
    } catch (...) {
        RC_LOGW("settings.json unreadable, using defaults");
    }
    return s;
}

void HeadsetSettings::Save(const std::string& path) const {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << ToJson().dump(2);
    }
    std::rename(tmp.c_str(), path.c_str());
}

}  // namespace rv
