// The parts of util.h that are the same on Linux and macOS. Where they differ (folders, the
// executable's path, starting at login, keeping awake) is in util_linux.cpp and util_mac.mm.
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>

#include "util.h"

namespace rcs {

// wchar_t is UTF-32 on Linux and macOS.
std::wstring Widen(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const auto c = uint8_t(s[i]);
        uint32_t cp;
        int n;
        if (c < 0x80) cp = c, n = 1;
        else if ((c >> 5) == 6) cp = c & 0x1F, n = 2;
        else if ((c >> 4) == 14) cp = c & 0x0F, n = 3;
        else if ((c >> 3) == 30) cp = c & 0x07, n = 4;
        else {
            out += L'�';
            i++;
            continue;
        }
        if (i + n > s.size()) break;
        for (int k = 1; k < n; k++) cp = (cp << 6) | (uint8_t(s[i + k]) & 0x3F);
        out += wchar_t(cp);
        i += n;
    }
    return out;
}

std::string Narrow(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (wchar_t wc : s) {
        const auto cp = uint32_t(wc);
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// The system's opener (xdg-open, macOS's open), in a grandchild so nothing is left to reap and
// the streamer doesn't wait on it.
bool OpenExternal(const std::string& target) {
#ifdef __APPLE__
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        if (fork() == 0) {
            execlp(opener, opener, target.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return true;
}

// No DPAPI: the data folder is the user's alone (0700), which is what DPAPI's per-user key
// amounts to for another process of the same user. Kept as base64 so the settings file's format
// matches. TODO: libsecret on Linux, the Keychain on macOS.
std::string ProtectString(const std::string& plain) {
    return plain.empty() ? std::string() : Base64Encode(plain.data(), plain.size());
}

std::string UnprotectString(const std::string& b64) {
    const auto bytes = Base64Decode(b64);
    return std::string(bytes.begin(), bytes.end());
}

bool WriteFileAtomic(const std::filesystem::path& path, const std::string& data) {
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), std::streamsize(data.size()));
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

std::string ComputerName() {
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0 || !name[0]) return "PC";
    std::string s = name;
    // macOS adds ".local" (its Bonjour name); the headset shows the plain name.
    if (s.size() > 6 && s.compare(s.size() - 6, 6, ".local") == 0) s.resize(s.size() - 6);
    return s;
}

}  // namespace rcs
