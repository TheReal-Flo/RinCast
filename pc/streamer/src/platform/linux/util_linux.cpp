// util.h on Linux: the XDG folders, autostart and the system's name. What Linux and macOS share
// is in platform/posix/util_posix.cpp.
#include <sys/stat.h>
#include <sys/utsname.h>

#include <cstdlib>
#include <fstream>

#include "util.h"

namespace rcs {

std::filesystem::path DataDir() {
    static const std::filesystem::path dir = [] {
        std::filesystem::path p;
        if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) p = std::filesystem::path(xdg) / "rincast";
        else if (const char* home = std::getenv("HOME"); home && *home) p = std::filesystem::path(home) / ".local/share/rincast";
        else p = std::filesystem::temp_directory_path() / "rincast";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        chmod(p.c_str(), 0700);  // keys and tokens live here
        return p;
    }();
    return dir;
}

std::filesystem::path ExePath() {
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path() / "rincast-streamer" : exe;
}

std::filesystem::path ExeDir() { return ExePath().parent_path(); }

// XDG_PICTURES_DIR from user-dirs.dirs where the desktop set it, ~/Pictures otherwise.
std::filesystem::path PicturesDir() {
    const char* home = std::getenv("HOME");
    const std::filesystem::path homeDir = home && *home ? home : "/tmp";
    std::filesystem::path config;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) config = xdg;
    else config = homeDir / ".config";
    std::ifstream f(config / "user-dirs.dirs");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("XDG_PICTURES_DIR=", 0) != 0) continue;
        std::string v = line.substr(17);
        if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
        if (v.rfind("$HOME", 0) == 0) return homeDir.string() + v.substr(5);
        if (!v.empty() && v.front() == '/') return v;
    }
    return homeDir / "Pictures";
}

// An XDG autostart entry, which every desktop on Linux reads at login.
bool SetStartAtLogin(bool enable) {
    std::filesystem::path dir;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) dir = xdg;
    else if (const char* home = std::getenv("HOME"); home && *home) dir = std::filesystem::path(home) / ".config";
    else return false;
    dir /= "autostart";
    const auto file = dir / "rincast-streamer.desktop";
    std::error_code ec;
    if (!enable) {
        std::filesystem::remove(file, ec);  // already gone is fine
        return true;
    }
    std::filesystem::create_directories(dir, ec);
    return WriteFileAtomic(file, "[Desktop Entry]\nType=Application\nName=RinCast Streamer\nExec=\"" + ExePath().string() +
                                     "\" --minimized\nX-GNOME-Autostart-enabled=true\n");
}

// "Ubuntu 24.04.1 LTS", from os-release; the kernel's name and release without it.
std::string WindowsVersion() {
    for (const char* file : {"/etc/os-release", "/usr/lib/os-release"}) {
        std::ifstream f(file);
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
            std::string v = line.substr(12);
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) v = v.substr(1, v.size() - 2);
            return v;
        }
    }
    utsname u{};
    uname(&u);
    return std::string(u.sysname) + " " + u.release;
}

// TODO (window step): org.freedesktop.ScreenSaver.Inhibit / logind's sleep inhibitor over D-Bus.
void KeepAwake(bool) {}

}  // namespace rcs

#include "platform/posix/platform_main.h"

namespace rcs {

// Nothing to ask for on Linux yet: X11 lets a user's programs capture and type; the Wayland
// portals ask when the desktop step uses them.
bool PreparePlatform() { return true; }

}  // namespace rcs
