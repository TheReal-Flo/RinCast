#include "settings.h"

#include <algorithm>
#include <functional>

#include "rc/platform.h"

namespace rcs {

json Settings::ToJson(bool forUi) const {
    json devices = json::array();
    for (const auto& d : trustedDevices)
        devices.push_back({{"id", d.id}, {"name", d.name}, {"model", d.model}, {"firstSeen", d.firstSeen},
                           {"lastSeen", d.lastSeen}, {"publicKey", d.publicKey}});
    json j{
        {"serverId", serverId},
        {"pcName", pcName},
        {"startWithWindows", startWithWindows},
        {"startMinimized", startMinimized},
        {"preventSleep", preventSleep},
        {"requireApproval", requireApproval},
        {"desktopInput", desktopInput},
        {"trustedDevices", devices},
        {"codec", codec},
        {"tenBit", tenBit},
        {"vrBitrateMbps", vrBitrateMbps},
        {"desktopBitrateMbps", desktopBitrateMbps},
        {"adaptiveBitrate", adaptiveBitrate},
        {"fecPercent", fecPercent},
        {"foveatedEncoding", foveatedEncoding},
        {"adaptiveQuantization", adaptiveQuantization},
        {"twoPassEncoding", twoPassEncoding},
        {"desktopMaxHeight", desktopMaxHeight},
        {"desktopFps", desktopFps},
        {"streamAudio", streamAudio},
        {"micPassthrough", micPassthrough},
        {"switchDefaultAudio", switchDefaultAudio},
        {"playOnPcToo", playOnPcToo},
        {"audioDevices", audioDevices},
        {"openxrRuntime", openxrRuntime},
        {"autoRegisterSteamVrDriver", autoRegisterSteamVrDriver},
        {"cloudUrl", cloudUrl},
        {"allowRemote", allowRemote},
        {"cloudUserName", cloudUserName},
        {"cloudEmail", cloudEmail},
    };
    if (!forUi) j["cloudRefreshTokenProtected"] = cloudRefreshTokenProtected;
    return j;
}

void Settings::FromJson(const json& j) {
    auto get = [&](const char* key, auto& field) {
        if (j.contains(key) && !j[key].is_null()) {
            try {
                field = j[key].get<std::decay_t<decltype(field)>>();
            } catch (...) {
            }
        }
    };
    get("serverId", serverId);
    get("pcName", pcName);
    get("startWithWindows", startWithWindows);
    get("startMinimized", startMinimized);
    get("preventSleep", preventSleep);
    get("requireApproval", requireApproval);
    get("desktopInput", desktopInput);
    get("codec", codec);
    get("tenBit", tenBit);
    get("vrBitrateMbps", vrBitrateMbps);
    get("desktopBitrateMbps", desktopBitrateMbps);
    get("adaptiveBitrate", adaptiveBitrate);
    get("fecPercent", fecPercent);
    get("foveatedEncoding", foveatedEncoding);
    get("adaptiveQuantization", adaptiveQuantization);
    get("twoPassEncoding", twoPassEncoding);
    get("desktopMaxHeight", desktopMaxHeight);
    get("desktopFps", desktopFps);
    get("streamAudio", streamAudio);
    get("micPassthrough", micPassthrough);
    get("switchDefaultAudio", switchDefaultAudio);
    get("playOnPcToo", playOnPcToo);
    get("audioDevices", audioDevices);
    get("openxrRuntime", openxrRuntime);
    get("autoRegisterSteamVrDriver", autoRegisterSteamVrDriver);
    get("cloudUrl", cloudUrl);
    get("allowRemote", allowRemote);
    get("cloudRefreshTokenProtected", cloudRefreshTokenProtected);
    get("cloudUserName", cloudUserName);
    get("cloudEmail", cloudEmail);
    if (j.contains("trustedDevices") && j["trustedDevices"].is_array()) {
        trustedDevices.clear();
        for (const auto& d : j["trustedDevices"]) {
            TrustedDevice td;
            td.id = d.value("id", "");
            td.name = d.value("name", "");
            td.model = d.value("model", "");
            td.firstSeen = d.value("firstSeen", int64_t(0));
            td.lastSeen = d.value("lastSeen", int64_t(0));
            td.publicKey = d.value("publicKey", "");
            if (!td.id.empty()) trustedDevices.push_back(td);
        }
    }
    // Clamp to sane ranges.
    vrBitrateMbps = std::clamp(vrBitrateMbps, 10, 500);
    desktopBitrateMbps = std::clamp(desktopBitrateMbps, 5, 200);
    fecPercent = std::clamp(fecPercent, 0, 25);
    if (codec != "auto" && codec != "h264" && codec != "hevc" && codec != "av1") codec = "auto";
    if (openxrRuntime != "rincast" && openxrRuntime != "steamvr") openxrRuntime = "steamvr";
    if (audioDevices != "auto" && audioDevices != "rincast" && audioDevices != "steam" && audioDevices != "system")
        audioDevices = "auto";
}

SettingsStore& SettingsStore::Get() {
    static SettingsStore store;
    return store;
}

void SettingsStore::Load() {
    std::lock_guard lock(m_mutex);
    std::vector<uint8_t> bytes;
    if (ReadFileBytes(DataDir() / L"settings.json", bytes)) {
        try {
            m_settings.FromJson(json::parse(bytes.begin(), bytes.end()));
        } catch (const std::exception& e) {
            RC_LOGW("settings.json is corrupt (%s), using defaults", e.what());
        }
    }
    bool dirty = false;
    if (m_settings.serverId.empty()) {
        uint8_t id[12];
        rc::RandomBytes(id, sizeof(id));
        m_settings.serverId = rc::HexEncode(id, sizeof(id));
        dirty = true;
    }
    if (m_settings.pcName.empty()) {
        m_settings.pcName = ComputerName();
        dirty = true;
    }
    if (dirty) SaveLocked();
}

Settings SettingsStore::Snapshot() const {
    std::lock_guard lock(m_mutex);
    return m_settings;
}

void SettingsStore::Update(const std::function<void(Settings&)>& mutate) {
    std::lock_guard lock(m_mutex);
    mutate(m_settings);
    SaveLocked();
}

void SettingsStore::SaveLocked() {
    if (!WriteFileAtomic(DataDir() / L"settings.json", m_settings.ToJson(false).dump(2)))
        RC_LOGW("Failed to save settings");
}

}  // namespace rcs
