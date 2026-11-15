// Mouse and keyboard from the headset on Linux: XTest on X11; on Wayland the RemoteDesktop
// portal (libei), or /dev/uinput (docs/LINUX.md). Not there yet: input is ignored.
#include "input/input_injector.h"

namespace rcs::input {

void MoveMouse(const MonitorInfo&, float, float) {}
void MouseButton(int, bool) {}
void MouseWheel(float, float) {}
void Key(unsigned short, bool) {}
void Text(const std::wstring&) {}
void ReleaseAll() {}
void WakeDisplays() {}

}  // namespace rcs::input
