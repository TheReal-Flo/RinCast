// Monitors on macOS, from CoreGraphics. desktopRect is the display in global points (what mouse
// events take); width and height are its pixels as shown (a Retina display's backing pixels, so
// text streams sharp), like the desktop size on Windows.
#include "capture/monitors.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "platform/mac/mac_display.h"

namespace rcs {

uint32_t DisplayIdOf(const MonitorInfo& monitor) {
    constexpr char kPrefix[] = "display-";
    if (monitor.id.rfind(kPrefix, 0) != 0) return 0;
    return uint32_t(std::strtoul(monitor.id.c_str() + sizeof(kPrefix) - 1, nullptr, 10));
}

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    CGDirectDisplayID ids[16];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(16, ids, &count) != kCGErrorSuccess) return monitors;
    const CGDirectDisplayID main = CGMainDisplayID();
    for (uint32_t i = 0; i < count; ++i) {
        const CGDirectDisplayID id = ids[i];
        if (CGDisplayIsInMirrorSet(id) && CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay) continue;  // shown elsewhere
        const CGRect bounds = CGDisplayBounds(id);
        MonitorInfo m;
        m.id = "display-" + std::to_string(id);
        m.desktopRect = {LONG(bounds.origin.x), LONG(bounds.origin.y), LONG(bounds.origin.x + bounds.size.width),
                         LONG(bounds.origin.y + bounds.size.height)};
        m.primary = id == main;
        m.rotation = int(CGDisplayRotation(id)) % 360;
        double scale = 1.0;
        if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id)) {
            const size_t points = CGDisplayModeGetWidth(mode);
            if (points) scale = double(CGDisplayModeGetPixelWidth(mode)) / double(points);
            const double hz = CGDisplayModeGetRefreshRate(mode);
            if (hz > 0) m.refreshHz = hz;  // 0 for built-in panels: they run at 60
            CGDisplayModeRelease(mode);
        }
        m.width = int(std::lround(bounds.size.width * scale));
        m.height = int(std::lround(bounds.size.height * scale));
        m.name = m.primary ? "Primary display" : "Display";
        for (NSScreen* screen in [NSScreen screens]) {
            NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
            if (number && number.unsignedIntValue == id) {
                if (@available(macOS 10.15, *)) {
                    if (screen.localizedName.length) m.name = screen.localizedName.UTF8String;
                }
                break;
            }
        }
        monitors.push_back(m);
    }
    // Primary first, then left to right, as on Windows.
    std::stable_sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.primary != b.primary) return a.primary;
        return a.desktopRect.left < b.desktopRect.left;
    });
    for (size_t i = 0; i < monitors.size(); i++) monitors[i].index = int(i);
    return monitors;
}

}  // namespace rcs
