#pragma once

#include <android/asset_manager.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rv {

class Assets {
   public:
    explicit Assets(AAssetManager* manager) : m_manager(manager) {}
    bool Read(const char* path, std::vector<uint8_t>& out) const;
    std::string ReadText(const char* path) const;

   private:
    AAssetManager* m_manager;
};

}  // namespace rv
