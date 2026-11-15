// The parts of the Linux input (input_linux.cpp): what the session's calls turn into, for X11
// (XTest, input_linux.cpp) or Wayland (the RemoteDesktop portal's libei, or /dev/uinput:
// input_wayland.cpp). Keys and buttons are evdev codes (linux/input-event-codes.h) on both: the
// headset's Windows virtual-key codes name key positions, and evdev codes are positions too, so
// the PC's own keyboard layout decides the characters, as on Windows and macOS.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "capture/monitors.h"

namespace rcs::input {

class InputBackend {
   public:
    virtual ~InputBackend() = default;
    // All calls come with InputMutex() held.
    virtual void Move(const MonitorInfo& monitor, float u, float v) = 0;
    virtual void Button(int evdevButton, bool down) = 0;  // BTN_LEFT, ...
    // In 1/120 of a notch: y > 0 scrolls up (away from the user), x > 0 right, as on Windows.
    virtual void Wheel(int32_t x120, int32_t y120) = 0;
    // evdev KEY_* code; the Windows virtual-key code too, for an X server without evdev keycodes.
    virtual void Key(int evdevKey, uint16_t vk, bool down) = 0;
    virtual void Text(const std::u32string& text) = 0;
    virtual void Wake() {}
    virtual void Prepare() {}
};

// Input calls come from the session's network thread and its worker, and the libei connection
// has a thread of its own: one at a time.
std::mutex& InputMutex();

std::unique_ptr<InputBackend> CreateX11Input();
std::unique_ptr<InputBackend> CreateWaylandInput();

// A Windows virtual-key code's evdev key (KEY_*), 0 when there is none.
int VkToEvdev(uint16_t vk);
// The key and Shift that type `c` on a US layout (for text where the keymap can't be read);
// false when it has none.
bool UsKeyFor(char32_t c, int& evdevKey, bool& shift);

}  // namespace rcs::input
