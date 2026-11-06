#include "assets.h"

#include "rc/platform.h"

namespace rv {

bool Assets::Read(const char* path, std::vector<uint8_t>& out) const {
    AAsset* asset = AAssetManager_open(m_manager, path, AASSET_MODE_BUFFER);
    if (!asset) {
        RC_LOGW("Asset not found: %s", path);
        return false;
    }
    const off_t size = AAsset_getLength(asset);
    out.resize(size_t(size));
    const int read = AAsset_read(asset, out.data(), out.size());
    AAsset_close(asset);
    return read == size;
}

std::string Assets::ReadText(const char* path) const {
    std::vector<uint8_t> data;
    if (!Read(path, data)) return {};
    return std::string(data.begin(), data.end());
}

}  // namespace rv
