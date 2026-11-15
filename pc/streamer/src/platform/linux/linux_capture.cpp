// Which capture a Linux session uses (linux_capture.h).
#include "platform/linux/linux_capture.h"

#include <cstdlib>
#include <cstring>

namespace rcs {

bool IsWaylandSession() {
    static const bool wayland = [] {
        if (const char* forced = std::getenv("RINCAST_CAPTURE"); forced && *forced) return std::strcmp(forced, "wayland") == 0;
        if (const char* display = std::getenv("WAYLAND_DISPLAY"); display && *display) return true;
        const char* type = std::getenv("XDG_SESSION_TYPE");
        return type && std::strcmp(type, "wayland") == 0;
    }();
    return wayland;
}

std::unique_ptr<LinuxCapture> CreateCapture() { return IsWaylandSession() ? CreateWaylandCapture() : CreateX11Capture(); }

}  // namespace rcs
