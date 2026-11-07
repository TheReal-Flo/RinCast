// Assets on Linux: the files of app/src/main/assets, copied next to the program as assets/.
#include "../assets.h"

#include <unistd.h>

#include <climits>
#include <fstream>
#include <iterator>

#include "rc/platform.h"

namespace rv {

Assets::Assets() {
    char exe[PATH_MAX] = {};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    std::string dir = n > 0 ? std::string(exe, size_t(n)) : std::string(".");
    dir = dir.substr(0, dir.find_last_of('/'));
    m_root = dir + "/assets/";
}

bool Assets::Read(const char* path, std::vector<uint8_t>& out) const {
    std::ifstream f(m_root + path, std::ios::binary);
    if (!f) {
        RC_LOGW("Asset not found: %s%s", m_root.c_str(), path);
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::string Assets::ReadText(const char* path) const {
    std::vector<uint8_t> data;
    if (!Read(path, data)) return {};
    return std::string(data.begin(), data.end());
}

}  // namespace rv
