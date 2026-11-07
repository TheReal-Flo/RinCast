#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rcm {

struct MacMonitorInfo {
    int index = 0;
    uint32_t displayId = 0;      // CGDirectDisplayID
    std::string id;              // e.g. "display-1"
    std::string name;            // e.g. "Built-in Display"
    double x = 0;
    double y = 0;
    int width = 0;               // Pixel width
    int height = 0;              // Pixel height
    bool primary = false;
    double refreshHz = 60.0;
    int rotation = 0;
};

std::vector<MacMonitorInfo> EnumerateMacMonitors();

}  // namespace rcm
