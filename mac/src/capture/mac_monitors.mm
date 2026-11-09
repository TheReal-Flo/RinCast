#include "mac_monitors.h"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

namespace rcm {

std::vector<MacMonitorInfo> EnumerateMacMonitors() {
    std::vector<MacMonitorInfo> list;

    uint32_t maxDisplays = 16;
    CGDirectDisplayID activeDisplays[16];
    uint32_t displayCount = 0;

    CGError err = CGGetActiveDisplayList(maxDisplays, activeDisplays, &displayCount);
    if (err != kCGErrorSuccess || displayCount == 0) {
        return list;
    }

    CGDirectDisplayID mainDisplay = CGMainDisplayID();

    for (uint32_t i = 0; i < displayCount; ++i) {
        CGDirectDisplayID dId = activeDisplays[i];
        CGRect bounds = CGDisplayBounds(dId);

        MacMonitorInfo info;
        info.index = static_cast<int>(i);
        info.displayId = dId;
        info.id = "display-" + std::to_string(dId);
        info.x = bounds.origin.x;
        info.y = bounds.origin.y;
        info.width = static_cast<int>(CGDisplayPixelsWide(dId));
        info.height = static_cast<int>(CGDisplayPixelsHigh(dId));
        info.primary = (dId == mainDisplay);
        info.rotation = static_cast<int>(CGDisplayRotation(dId));

        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(dId);
        if (mode) {
            double rate = CGDisplayModeGetRefreshRate(mode);
            if (rate > 0.0) {
                info.refreshHz = rate;
            } else {
                info.refreshHz = 60.0;
            }
            CGDisplayModeRelease(mode);
        } else {
            info.refreshHz = 60.0;
        }

        // Try getting screen localized name from NSScreen
        info.name = "Display " + std::to_string(i + 1);
        for (NSScreen* screen in [NSScreen screens]) {
            NSDictionary* desc = [screen deviceDescription];
            NSNumber* screenIdNum = [desc objectForKey:@"NSScreenNumber"];
            if (screenIdNum && [screenIdNum unsignedIntValue] == dId) {
                if (@available(macOS 10.15, *)) {
                    NSString* screenName = [screen localizedName];
                    if (screenName && screenName.length > 0) {
                        info.name = [screenName UTF8String];
                    }
                }
                break;
            }
        }

        list.push_back(info);
    }

    // Sort: primary first, then left-to-right
    std::sort(list.begin(), list.end(), [](const MacMonitorInfo& a, const MacMonitorInfo& b) {
        if (a.primary != b.primary) return a.primary > b.primary;
        return a.x < b.x;
    });

    for (size_t i = 0; i < list.size(); ++i) {
        list[i].index = static_cast<int>(i);
    }

    return list;
}

}  // namespace rcm
