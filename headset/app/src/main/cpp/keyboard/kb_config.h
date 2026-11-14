// What the keyboard keeps between sessions (BetterFrame's keyboard settings and its overlay.json),
// stored in the headset's settings.json under "keyboard".
#pragma once

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace rv::kb {

struct Config {
    std::string layout = "minimal";   // minimal | full (Standard) | advanced (Full)
    std::string lang = "en-US", fmt = "default";
    std::string theme = "steam";
    int backdrop = 100;               // percent
    std::string bar = "top";          // top | bottom | off
    std::string bottom = "default";   // default | laptop
    std::string hold = "repeat";      // repeat | shift
    std::string sound = "deep";       // deep (Original) | light | off
    bool tips = true, preview = true, words = false, swipe = false, curved = true, panel = true;
    float scale = 0.6f;               // set by the resize corner
    int tone = -1;
    std::vector<std::string> recent, fav;

    nlohmann::json ToJson() const {
        return {{"layout", layout}, {"lang", lang}, {"fmt", fmt}, {"theme", theme}, {"backdrop", backdrop}, {"bar", bar},
                {"bottom", bottom}, {"hold", hold}, {"sound", sound}, {"tips", tips}, {"preview", preview}, {"words", words},
                {"swipe", swipe}, {"curved", curved}, {"panel", panel}, {"scale", scale}, {"tone", tone}, {"recent", recent},
                {"fav", fav}};
    }
    void FromJson(const nlohmann::json& j) {
        if (!j.is_object()) return;
        auto get = [&](const char* k, auto& field) {
            if (j.contains(k)) {
                try {
                    field = j[k].get<std::decay_t<decltype(field)>>();
                } catch (...) {
                }
            }
        };
        get("layout", layout);
        get("lang", lang);
        get("fmt", fmt);
        get("theme", theme);
        get("backdrop", backdrop);
        get("bar", bar);
        get("bottom", bottom);
        get("hold", hold);
        get("sound", sound);
        get("tips", tips);
        get("preview", preview);
        get("words", words);
        get("swipe", swipe);
        get("curved", curved);
        get("panel", panel);
        get("scale", scale);
        get("tone", tone);
        get("recent", recent);
        get("fav", fav);
    }
};

}  // namespace rv::kb
