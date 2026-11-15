// macOS helpers the platform files share.
#pragma once

#include <cstdint>

#include "capture/monitors.h"

namespace rcs {

// The CGDirectDisplayID behind a monitor from EnumerateMonitors (its id is "display-<id>").
uint32_t DisplayIdOf(const MonitorInfo& monitor);

}  // namespace rcs
