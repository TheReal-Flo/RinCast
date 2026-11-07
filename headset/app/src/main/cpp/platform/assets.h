#pragma once

#ifdef __ANDROID__
#include <android/asset_manager.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace rv {

// The files under app/src/main/assets: inside the APK on Android, next to the program in
// assets/ on Linux.
class Assets {
   public:
#ifdef __ANDROID__
    explicit Assets(AAssetManager* manager) : m_manager(manager) {}
#else
    Assets();
#endif
    bool Read(const char* path, std::vector<uint8_t>& out) const;
    std::string ReadText(const char* path) const;

   private:
#ifdef __ANDROID__
    AAssetManager* m_manager;
#else
    std::string m_root;
#endif
};

}  // namespace rv
