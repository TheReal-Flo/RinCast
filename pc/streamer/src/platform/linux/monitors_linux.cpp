// Monitors on Linux: X11 RandR outputs, or on Wayland the screen-cast portal's streams
// (docs/LINUX.md). Not there yet: none are listed, so the headset offers no desktop.
#include "capture/monitors.h"

#include "rc/platform.h"

namespace rcs {

std::vector<MonitorInfo> EnumerateMonitors() {
    static bool said = false;
    if (!said) RC_LOGW("Listing monitors isn't implemented on Linux yet");
    said = true;
    return {};
}

}  // namespace rcs
