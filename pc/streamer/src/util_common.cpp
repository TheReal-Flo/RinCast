// The portable part of util.h, for Windows and Linux alike.
#include <cstring>
#include <fstream>

#include "util.h"

namespace rcs {

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t n = (uint32_t(p[i]) << 16) | (i + 1 < size ? uint32_t(p[i + 1]) << 8 : 0) |
                           (i + 2 < size ? uint32_t(p[i + 2]) : 0);
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += i + 1 < size ? kB64[(n >> 6) & 63] : '=';
        out += i + 2 < size ? kB64[n & 63] : '=';
    }
    return out;
}

std::vector<uint8_t> Base64Decode(const std::string& s) {
    std::vector<uint8_t> out;
    uint32_t buf = 0;
    int bits = 0;
    for (char c : s) {
        const char* pos = strchr(kB64, c);
        if (c == '=' || !pos || !c) continue;
        buf = (buf << 6) | uint32_t(pos - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(buf >> bits));
        }
    }
    return out;
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    out.resize(size_t(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
    return bool(f);
}

}  // namespace rcs
