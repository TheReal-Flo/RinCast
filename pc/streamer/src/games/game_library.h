// Lists installed PC VR games (Steam VR manifest) and launches them.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "../util.h"

namespace rcs {

struct Game {
    std::string id;        // "steam.app.620980"
    std::string name;
    std::string source;    // steam
    std::string launchUrl;
    std::filesystem::path image;  // header/capsule art, may be empty
    int64_t lastPlayed = 0;
};

class GameLibrary {
   public:
    std::vector<Game> Scan();
    bool Launch(const std::string& id);
    // JPEG/PNG bytes of the game's capsule art (empty if none).
    std::vector<uint8_t> Image(const std::string& id);

   private:
    std::vector<Game> m_cache;
};

}  // namespace rcs
