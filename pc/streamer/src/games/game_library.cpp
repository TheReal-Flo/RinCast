#include "game_library.h"

#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

#include "../vr/steamvr.h"
#include "rc/platform.h"

namespace rcs {

namespace fs = std::filesystem;

static fs::path FindArt(const fs::path& steam, const std::string& appId) {
    const fs::path cache = steam / "appcache" / "librarycache";
    // Classic layout: <appid>_library_600x900.jpg, newer: <appid>/library_600x900.jpg (possibly nested).
    for (const char* name : {"_library_600x900.jpg", "_header.jpg"}) {
        const fs::path p = cache / (appId + name);
        if (fs::exists(p)) return p;
    }
    const fs::path dir = cache / appId;
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        fs::path header;
        for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
            const auto fn = e.path().filename().string();
            if (fn == "library_600x900.jpg") return e.path();
            if (fn == "header.jpg") header = e.path();
        }
        if (!header.empty()) return header;
    }
    return {};
}

static std::map<std::string, int64_t> LastPlayedTimes(const fs::path& steam) {
    // userdata/<id>/config/localconfig.vdf contains "LastPlayed" per app.
    std::map<std::string, int64_t> out;
    std::error_code ec;
    for (const auto& user : fs::directory_iterator(steam / "userdata", ec)) {
        std::ifstream f(user.path() / "config" / "localconfig.vdf");
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        // Hand-rolled scan (the file can be several MB; std::regex would be slow).
        static const std::string kKey = "\"LastPlayed\"";
        for (size_t pos = text.find(kKey); pos != std::string::npos; pos = text.find(kKey, pos + 1)) {
            size_t v = text.find('"', pos + kKey.size());
            if (v == std::string::npos) break;
            const size_t vEnd = text.find('"', v + 1);
            if (vEnd == std::string::npos) break;
            const std::string value = text.substr(v + 1, vEnd - v - 1);
            // Walk back to the '{' opening this block, skipping nested blocks.
            int depth = 0;
            size_t i = pos;
            while (i > 0) {
                i--;
                if (text[i] == '}') depth++;
                if (text[i] == '{') {
                    if (depth == 0) break;
                    depth--;
                }
            }
            size_t q2 = text.rfind('"', i);
            if (q2 == std::string::npos || q2 == 0) continue;
            size_t q1 = text.rfind('"', q2 - 1);
            if (q1 == std::string::npos) continue;
            const std::string appId = text.substr(q1 + 1, q2 - q1 - 1);
            if (appId.empty() || value.empty() || !std::all_of(appId.begin(), appId.end(), ::isdigit) ||
                !std::all_of(value.begin(), value.end(), ::isdigit))
                continue;
            auto& slot = out[appId];
            slot = std::max(slot, int64_t(std::stoll(value)));
        }
    }
    return out;
}

std::vector<Game> GameLibrary::Scan() {
    std::vector<Game> games;
    auto steam = steamvr::SteamPath();
    if (!steam) return games;
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(*steam / "config" / "steamapps.vrmanifest", bytes)) return games;
    const auto lastPlayed = LastPlayedTimes(*steam);
    try {
        const json j = json::parse(bytes.begin(), bytes.end());
        for (const auto& app : j.value("applications", json::array())) {
            const std::string key = app.value("app_key", "");
            if (key.rfind("steam.app.", 0) != 0) continue;
            const std::string appId = key.substr(10);
            if (appId == "250820") continue;  // SteamVR itself
            Game g;
            g.id = key;
            g.source = "steam";
            g.launchUrl = app.value("url", "steam://launch/" + appId + "/VR");
            const auto strings = app.value("strings", json::object());
            if (strings.contains("en_us")) g.name = strings["en_us"].value("name", "");
            if (g.name.empty() && !strings.empty()) g.name = strings.begin()->value("name", "");
            if (g.name.empty()) continue;
            g.image = FindArt(*steam, appId);
            if (auto it = lastPlayed.find(appId); it != lastPlayed.end()) g.lastPlayed = it->second;
            games.push_back(std::move(g));
        }
    } catch (const std::exception& e) {
        RC_LOGW("steamapps.vrmanifest parse error: %s", e.what());
    }
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
        if (a.lastPlayed != b.lastPlayed) return a.lastPlayed > b.lastPlayed;
        return a.name < b.name;
    });
    m_cache = games;
    return games;
}

bool GameLibrary::Launch(const std::string& id) {
    if (m_cache.empty()) Scan();
    for (const auto& g : m_cache) {
        if (g.id == id) {
            RC_LOGI("Launching %s", g.name.c_str());
            return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", Widen(g.launchUrl).c_str(), nullptr,
                                                           nullptr, SW_SHOWNORMAL)) > 32;
        }
    }
    return false;
}

std::vector<uint8_t> GameLibrary::Image(const std::string& id) {
    if (m_cache.empty()) Scan();
    std::vector<uint8_t> bytes;
    for (const auto& g : m_cache)
        if (g.id == id && !g.image.empty()) ReadFileBytes(g.image, bytes);
    return bytes;
}

}  // namespace rcs
