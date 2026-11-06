#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace rcs {

struct MonitorInfo {
    int index = 0;              // stable order: primary first, then left-to-right
    std::string id;             // \\.\DISPLAY1
    std::string name;           // "DELL U2720Q"
    RECT desktopRect{};         // virtual desktop coordinates
    int width = 0;              // native mode, unrotated
    int height = 0;
    bool primary = false;
    LUID adapterLuid{};
    double refreshHz = 60;
    int rotation = 0;           // 0/90/180/270
};

std::vector<MonitorInfo> EnumerateMonitors();

}  // namespace rcs
