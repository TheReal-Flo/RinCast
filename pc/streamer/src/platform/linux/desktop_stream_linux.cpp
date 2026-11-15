// Desktop streaming on Linux: X11 (XShm) or Wayland (screen-cast portal + PipeWire) capture,
// encoded with FFmpeg (docs/LINUX.md), paced by the shared DesktopPacer. Not there yet: the
// stream says so and ends.
#include "stream/desktop_stream.h"

#include "rc/platform.h"

namespace rcs {

void DesktopStream::Run() {
    RC_LOGW("Desktop streaming isn't implemented on Linux yet (monitor %s)", m_monitor.name.c_str());
    m_failed = true;
}

}  // namespace rcs
