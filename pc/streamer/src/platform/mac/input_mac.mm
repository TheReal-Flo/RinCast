// Mouse and keyboard from the headset on macOS, as CGEvents (needs Accessibility permission,
// which main asks for). The headset sends Windows virtual-key codes; they map to the Mac's key
// positions, so the Mac's own keyboard layout decides the characters.
#include "input/input_injector.h"

#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <IOKit/pwr_mgt/IOPMLib.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>
#include <vector>

#include "util.h"

namespace rcs::input {

namespace {

// Input comes from the session's network thread and its worker; one at a time.
std::mutex g_mutex;
CGPoint g_lastMousePos = CGPointZero;
bool g_mouseButtonsDown[5] = {false, false, false, false, false};
std::set<CGKeyCode> g_keysDown;
// Caps Lock is a toggle, not a held modifier. Track the latch ourselves: otherwise
// the key-up event clears AlphaShift and macOS immediately undoes the toggle.
bool g_capsLock = false;
float g_wheelX = 0, g_wheelY = 0;  // fractions of a scroll step not sent yet

// CGEventCreateKeyboardEvent(nullptr, ...) copies the system's *current* modifier
// flags into the event. If a Bluetooth key-up is dropped, Command stays latched in
// macOS and every injected letter inherits it: 'e', 'd' and 'f' become Cmd+E/D/F.
// Drive the flags from the keys we actually know are down instead.
CGEventFlags TrackedFlags() {
    CGEventFlags f = 0;
    if (g_keysDown.count(kVK_Command) || g_keysDown.count(kVK_RightCommand)) f |= kCGEventFlagMaskCommand;
    if (g_keysDown.count(kVK_Shift) || g_keysDown.count(kVK_RightShift)) f |= kCGEventFlagMaskShift;
    if (g_keysDown.count(kVK_Option) || g_keysDown.count(kVK_RightOption)) f |= kCGEventFlagMaskAlternate;
    if (g_keysDown.count(kVK_Control) || g_keysDown.count(kVK_RightControl)) f |= kCGEventFlagMaskControl;
    if (g_capsLock) f |= kCGEventFlagMaskAlphaShift;
    return f;
}

void PostKey(CGKeyCode code, bool down) {
    CGEventRef ev = CGEventCreateKeyboardEvent(nullptr, code, down);
    if (ev) {
        CGEventSetFlags(ev, TrackedFlags());
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
    }
}

void PostButton(int button, bool down) {
    CGEventType type;
    CGMouseButton mouseBtn;
    switch (button) {
        case 0:
            type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
            mouseBtn = kCGMouseButtonLeft;
            break;
        case 1:
            type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
            mouseBtn = kCGMouseButtonRight;
            break;
        case 2:
            type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
            mouseBtn = kCGMouseButtonCenter;
            break;
        default:  // back (3) and forward (4): other buttons 3 and 4, as macOS numbers them
            type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
            mouseBtn = static_cast<CGMouseButton>(button);
            break;
    }
    CGEventRef ev = CGEventCreateMouseEvent(nullptr, type, g_lastMousePos, mouseBtn);
    if (ev) {
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
    }
}

CGKeyCode WinVkToMac(uint16_t vk) {
    switch (vk) {
        // Letters
        case 'A': return kVK_ANSI_A;
        case 'B': return kVK_ANSI_B;
        case 'C': return kVK_ANSI_C;
        case 'D': return kVK_ANSI_D;
        case 'E': return kVK_ANSI_E;
        case 'F': return kVK_ANSI_F;
        case 'G': return kVK_ANSI_G;
        case 'H': return kVK_ANSI_H;
        case 'I': return kVK_ANSI_I;
        case 'J': return kVK_ANSI_J;
        case 'K': return kVK_ANSI_K;
        case 'L': return kVK_ANSI_L;
        case 'M': return kVK_ANSI_M;
        case 'N': return kVK_ANSI_N;
        case 'O': return kVK_ANSI_O;
        case 'P': return kVK_ANSI_P;
        case 'Q': return kVK_ANSI_Q;
        case 'R': return kVK_ANSI_R;
        case 'S': return kVK_ANSI_S;
        case 'T': return kVK_ANSI_T;
        case 'U': return kVK_ANSI_U;
        case 'V': return kVK_ANSI_V;
        case 'W': return kVK_ANSI_W;
        case 'X': return kVK_ANSI_X;
        case 'Y': return kVK_ANSI_Y;
        case 'Z': return kVK_ANSI_Z;

        // Numbers
        case '0': return kVK_ANSI_0;
        case '1': return kVK_ANSI_1;
        case '2': return kVK_ANSI_2;
        case '3': return kVK_ANSI_3;
        case '4': return kVK_ANSI_4;
        case '5': return kVK_ANSI_5;
        case '6': return kVK_ANSI_6;
        case '7': return kVK_ANSI_7;
        case '8': return kVK_ANSI_8;
        case '9': return kVK_ANSI_9;

        // Controls
        case 0x08: return kVK_Delete;        // VK_BACK
        case 0x09: return kVK_Tab;           // VK_TAB
        case 0x0D: return kVK_Return;        // VK_RETURN
        case 0x10: return kVK_Shift;         // VK_SHIFT
        case 0x11: return kVK_Control;       // VK_CONTROL
        case 0x12: return kVK_Option;        // VK_MENU / Alt
        case 0x14: return kVK_CapsLock;      // VK_CAPITAL
        case 0x1B: return kVK_Escape;        // VK_ESCAPE
        case 0x20: return kVK_Space;         // VK_SPACE
        case 0x25: return kVK_LeftArrow;     // VK_LEFT
        case 0x26: return kVK_UpArrow;       // VK_UP
        case 0x27: return kVK_RightArrow;    // VK_RIGHT
        case 0x28: return kVK_DownArrow;     // VK_DOWN
        case 0x2E: return kVK_ForwardDelete; // VK_DELETE

        // Function keys
        case 0x70: return kVK_F1;
        case 0x71: return kVK_F2;
        case 0x72: return kVK_F3;
        case 0x73: return kVK_F4;
        case 0x74: return kVK_F5;
        case 0x75: return kVK_F6;
        case 0x76: return kVK_F7;
        case 0x77: return kVK_F8;
        case 0x78: return kVK_F9;
        case 0x79: return kVK_F10;
        case 0x7A: return kVK_F11;
        case 0x7B: return kVK_F12;

        // Win key -> Mac Command
        case 0x5B: return kVK_Command;
        case 0x5C: return kVK_RightCommand;

        // The left and right modifiers, which some keyboards send instead of the plain ones
        case 0xA0: return kVK_Shift;
        case 0xA1: return kVK_RightShift;
        case 0xA2: return kVK_Control;
        case 0xA3: return kVK_RightControl;
        case 0xA4: return kVK_Option;
        case 0xA5: return kVK_RightOption;

        // OEM keys. These are physical US-layout positions: the Mac's own input layout
        // decides the character, so on ABNT2 they produce the accent dead keys. Dropping
        // them (as 0xFFFF) made every punctuation/accent key do nothing on the Mac.
        case 0xBA: return kVK_ANSI_Semicolon;   // ; / Ç on ABNT2
        case 0xBB: return kVK_ANSI_Equal;
        case 0xBC: return kVK_ANSI_Comma;
        case 0xBD: return kVK_ANSI_Minus;
        case 0xBE: return kVK_ANSI_Period;
        case 0xBF: return kVK_ANSI_Slash;
        case 0xC0: return kVK_ANSI_Grave;
        case 0xDB: return kVK_ANSI_LeftBracket;   // [ / ´ on ABNT2
        case 0xDC: return kVK_ANSI_Backslash;     // \ / ] on ABNT2
        case 0xDD: return kVK_ANSI_RightBracket;
        case 0xDE: return kVK_ANSI_Quote;         // ' / ~ on ABNT2
        case 0xE2: return kVK_ISO_Section;        // the extra 102nd ABNT2/ISO key

        // Navigation keys
        case 0x21: return kVK_PageUp;
        case 0x22: return kVK_PageDown;
        case 0x23: return kVK_End;
        case 0x24: return kVK_Home;
        case 0x2D: return kVK_Help;              // Insert

        // Numpad
        case 0x60: return kVK_ANSI_Keypad0;
        case 0x61: return kVK_ANSI_Keypad1;
        case 0x62: return kVK_ANSI_Keypad2;
        case 0x63: return kVK_ANSI_Keypad3;
        case 0x64: return kVK_ANSI_Keypad4;
        case 0x65: return kVK_ANSI_Keypad5;
        case 0x66: return kVK_ANSI_Keypad6;
        case 0x67: return kVK_ANSI_Keypad7;
        case 0x68: return kVK_ANSI_Keypad8;
        case 0x69: return kVK_ANSI_Keypad9;
        case 0x6A: return kVK_ANSI_KeypadMultiply;
        case 0x6B: return kVK_ANSI_KeypadPlus;
        case 0x6D: return kVK_ANSI_KeypadMinus;
        case 0x6E: return kVK_ANSI_KeypadDecimal;
        case 0x6F: return kVK_ANSI_KeypadDivide;

        default:
            return 0xFFFF;
    }
}

}  // namespace

// u, v across the monitor's area in global points (desktopRect), which is what mouse events take
// on every Mac, Retina or not.
void MoveMouse(const MonitorInfo& monitor, float u, float v) {
    std::lock_guard lock(g_mutex);
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const RECT& r = monitor.desktopRect;
    const CGPoint pt = CGPointMake(r.left + u * (r.right - r.left - 1), r.top + v * (r.bottom - r.top - 1));
    g_lastMousePos = pt;

    CGEventType type = kCGEventMouseMoved;
    CGMouseButton button = kCGMouseButtonLeft;
    if (g_mouseButtonsDown[0]) {
        type = kCGEventLeftMouseDragged;
    } else if (g_mouseButtonsDown[1]) {
        type = kCGEventRightMouseDragged;
        button = kCGMouseButtonRight;
    } else if (g_mouseButtonsDown[2]) {
        type = kCGEventOtherMouseDragged;
        button = kCGMouseButtonCenter;
    }
    CGEventRef ev = CGEventCreateMouseEvent(nullptr, type, pt, button);
    if (ev) {
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
    }
}

void MouseButton(int button, bool down) {
    std::lock_guard lock(g_mutex);
    if (button < 0 || button >= 5) return;
    g_mouseButtonsDown[button] = down;
    PostButton(button, down);
}

// Notches as the headset sends them (1.0 = one wheel detent, often less from a thumbstick):
// fractions add up until a whole pixel step is due, so slow scrolling isn't lost.
void MouseWheel(float dx, float dy) {
    std::lock_guard lock(g_mutex);
    constexpr float kPixelsPerNotch = 10.f;
    g_wheelX += dx * kPixelsPerNotch;
    g_wheelY += dy * kPixelsPerNotch;
    const int32_t x = int32_t(std::trunc(g_wheelX)), y = int32_t(std::trunc(g_wheelY));
    if (!x && !y) return;
    g_wheelX -= float(x);
    g_wheelY -= float(y);
    CGEventRef ev = CGEventCreateScrollWheelEvent2(nullptr, kCGScrollEventUnitPixel, 2, y, x, 0);
    if (ev) {
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
    }
}

void Key(unsigned short vk, bool down) {
    std::lock_guard lock(g_mutex);
    const CGKeyCode code = WinVkToMac(vk);
    if (code == 0xFFFF) return;
    if (code == kVK_CapsLock) {
        // Toggle on the press, then keep the resulting latch on the release too.
        // Both events carry the same AlphaShift state, so macOS doesn't undo it.
        if (down) g_capsLock = !g_capsLock;
    }
    if (down) g_keysDown.insert(code);
    else g_keysDown.erase(code);
    PostKey(code, down);
}

// Composed text (an emoji, a swiped word), typed as Unicode: a CGEvent carries at most 20
// UTF-16 units, so longer text goes in pieces.
void Text(const std::wstring& text) {
    std::lock_guard lock(g_mutex);
    NSString* ns = [NSString stringWithUTF8String:Narrow(text).c_str()];
    if (!ns.length) return;
    std::vector<UniChar> units(ns.length);
    [ns getCharacters:units.data() range:NSMakeRange(0, ns.length)];
    for (size_t start = 0; start < units.size();) {
        size_t n = std::min<size_t>(20, units.size() - start);
        // Don't split a surrogate pair between two events.
        if (n == 20 && CFStringIsSurrogateHighCharacter(units[start + n - 1])) n--;
        for (bool down : {true, false}) {
            CGEventRef ev = CGEventCreateKeyboardEvent(nullptr, 0, down);
            if (!ev) continue;
            CGEventSetFlags(ev, 0);  // text is already composed; never apply a stuck modifier
            CGEventKeyboardSetUnicodeString(ev, n, units.data() + start);
            CGEventPost(kCGHIDEventTap, ev);
            CFRelease(ev);
        }
        start += n;
    }
}

void ReleaseAll() {
    std::lock_guard lock(g_mutex);
    for (CGKeyCode code : g_keysDown)
        if (code != kVK_CapsLock) PostKey(code, false);
    g_keysDown.clear();
    // A dropped Bluetooth key-up, or a streamer killed mid-key, leaves Command latched
    // in macOS's global modifier state (CGEvent key-downs persist until a key-up is
    // posted). A key-up for a key that isn't down is harmless, so clear every modifier
    // unconditionally: this is what stops a later 'e', 'd' or 'f' from becoming Cmd+E/D/F.
    // Caps Lock is a toggle, not a held key: posting an unconditional key-up for it
    // here would switch the user's Caps Lock off on session close/startup.
    static const CGKeyCode kModifiers[] = {
        kVK_Command, kVK_RightCommand, kVK_Shift, kVK_RightShift, kVK_Option, kVK_RightOption, kVK_Control, kVK_RightControl,
    };
    for (CGKeyCode code : kModifiers) PostKey(code, false);
    for (int i = 0; i < 5; ++i)
        if (g_mouseButtonsDown[i]) {
            g_mouseButtonsDown[i] = false;
            PostButton(i, false);
        }
    g_wheelX = g_wheelY = 0;
}

// Asleep displays deliver no pictures: user activity wakes them, as a mouse nudge does on Windows.
void WakeDisplays() {
    IOPMAssertionID id = kIOPMNullAssertionID;
    if (IOPMAssertionDeclareUserActivity(CFSTR("RinCast headset connected"), kIOPMUserActiveLocal, &id) == kIOReturnSuccess)
        IOPMAssertionRelease(id);
}

}  // namespace rcs::input
