// What the X11 capture, monitor list and input share: opening a display connection of their own,
// noticing X errors on it, and the monitors as RandR describes them.
#pragma once

#include <X11/Xlib.h>

#include <string>
#include <vector>

#include "capture/monitors.h"

namespace rcs::x11 {

// A connection of the caller's own ($DISPLAY), or null. Errors on it are recorded (LastError)
// instead of ending the process, which is what Xlib does by default.
Display* Open();
void Close(Display* display);
// The last X error code seen on `display` since the previous call (0 = none), and clears it.
int TakeError(Display* display);

// The monitors as RandR outputs with a CRTC: id is the output's name ("DP-1"), the name is the
// EDID's monitor name, desktopRect is the CRTC's area in root coordinates.
std::vector<MonitorInfo> Monitors(Display* display);

}  // namespace rcs::x11
