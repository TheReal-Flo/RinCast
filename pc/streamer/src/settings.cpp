#include "settings.h"

#include <algorithm>
#include <cstdlib>
#include <functional>

#include <shlobj.h>

#include "rc/platform.h"

namespace rcs {

std::string HeadsetName(const std::string& model) {
    if (model.empty()) return {};
    // PICO sells under model codes; these are the ones their own manuals and certificates use.
    static const struct {
        const char* code;
        const char* name;
    } kModels[] = {
        {"A8110", "PICO 4"},        // PICO 4, 4 Pro and 4 Enterprise
        {"A9210", "PICO 4 Ultra"},  // also sold as PICO 4S in some markets
        {"A7H10", "PICO Neo 3"},
        {"A7Q10", "PICO G3"},
    };
    for (const auto& m : kModels)
        if (model == m.code) return m.name;
    if (model.rfind("A8", 0) == 0 || model.rfind("A9", 0) == 0 || model.rfind("A7", 0) == 0) return "PICO " + model;
    return model;
}

std::string DefaultScreenshotDir() {
    // The shell knows where Pictures is: it may be moved, renamed or on another drive.
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &known))) {
        std::string dir = Narrow(known) + "/RinCast";
        CoTaskMemFree(known);
        return dir;
    }
    const char* profile = getenv("USERPROFILE");
    return std::string(profile ? profile : "") + "/Pictures/RinCast";
}

json Settings::ToJson(bool forUi) const {
    json devices = json::array();
    for (const auto& d : trustedDevices) {
        const std::string shown = HeadsetName(d.model).empty() ? d.name : HeadsetName(d.model);
        devices.push_back({{"id", d.id}, {"name", shown}, {"model", d.model}, {"firstSeen", d.firstSeen},
                           {"lastSeen", d.lastSeen}, {"publicKey", d.publicKey}});
    }
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
        {"screenshotDir", screenshotDir},
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
    get("screenshotDir", screenshotDir);
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
            const std::string mapped = HeadsetName(td.model);
            if (!mapped.empty()) td.name = mapped;
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
    // An empty folder used to mean "the default" quietly. Say the default instead, so
    // what the UI shows and where screenshots land can never disagree.
    if (screenshotDir.empty()) screenshotDir = DefaultScreenshotDir();
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
