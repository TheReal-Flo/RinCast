// Injects mouse and keyboard input from the headset with SendInput.
#pragma once

#include <string>

#include "../capture/monitors.h"

namespace rcs {

namespace input {

// u, v in [0, 1] across the monitor's logical area.
void MoveMouse(const MonitorInfo& monitor, float u, float v);
// button: 0 left, 1 right, 2 middle, 3 back, 4 forward
void MouseButton(int button, bool down);
// Wheel deltas in notches (1.0 = one detent). Fractions are accumulated.
void MouseWheel(float dx, float dy);
// Windows virtual key code. Extended keys (arrows, etc.) are detected automatically.
void Key(unsigned short vk, bool down);
void Text(const std::wstring& text);
// Releases every button/key we may have left pressed (on disconnect).
void ReleaseAll();

}  // namespace input

}  // namespace rcs

namespace rcs::input {
// Wakes sleeping displays (a 1-pixel nudge that returns the cursor) so DWM
// resumes composition and desktop duplication delivers frames.
void WakeDisplays();
}  // namespace rcs::input
