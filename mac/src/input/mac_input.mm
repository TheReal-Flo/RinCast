#include "mac_input.h"

#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <set>
#include <vector>

namespace rcm {
namespace input {

static CGPoint g_lastMousePos = CGPointZero;
static bool g_mouseButtonsDown[5] = {false, false, false, false, false};
static std::set<CGKeyCode> g_keysDown;
// Caps Lock is a toggle, not a held modifier. Track the latch ourselves: otherwise
// the key-up event clears AlphaShift and macOS immediately undoes the toggle.
static bool g_capsLock = false;

// CGEventCreateKeyboardEvent(nullptr, ...) copies the system's *current* modifier
// flags into the event. If a Bluetooth key-up is dropped, Command stays latched in
// macOS and every injected letter inherits it: 'e', 'd' and 'f' become Cmd+E/D/F.
// Drive the flags from the keys we actually know are down instead.
static CGEventFlags TrackedFlags() {
    CGEventFlags f = 0;
    if (g_keysDown.count(kVK_Command) || g_keysDown.count(kVK_RightCommand)) f |= kCGEventFlagMaskCommand;
    if (g_keysDown.count(kVK_Shift) || g_keysDown.count(kVK_RightShift)) f |= kCGEventFlagMaskShift;
    if (g_keysDown.count(kVK_Option) || g_keysDown.count(kVK_RightOption)) f |= kCGEventFlagMaskAlternate;
    if (g_keysDown.count(kVK_Control) || g_keysDown.count(kVK_RightControl)) f |= kCGEventFlagMaskControl;
    if (g_capsLock) f |= kCGEventFlagMaskAlphaShift;
    return f;
}

static void PostKey(CGKeyCode code, bool down) {
    CGEventRef ev = CGEventCreateKeyboardEvent(nullptr, code, down);
    if (ev) {
        CGEventSetFlags(ev, TrackedFlags());
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
    }
}

void MoveMouse(const MacMonitorInfo& monitor, float u, float v) {
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);

    CGPoint pt = CGPointMake(monitor.x + u * monitor.width, monitor.y + v * monitor.height);
    g_lastMousePos = pt;

    CGEventType type = kCGEventMouseMoved;
    if (g_mouseButtonsDown[0]) {
        type = kCGEventLeftMouseDragged;
    } else if (g_mouseButtonsDown[1]) {
        type = kCGEventRightMouseDragged;
    } else if (g_mouseButtonsDown[2]) {
        type = kCGEventOtherMouseDragged;
    }

    CGEventRef ev = CGEventCreateMouseEvent(nullptr, type, pt, kCGMouseButtonLeft);
    if (ev) {
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
    }
}

void MouseButton(int button, bool down) {
    CGEventType type = kCGEventNull;
    CGMouseButton mouseBtn = kCGMouseButtonLeft;

    if (button >= 0 && button < 5) {
        g_mouseButtonsDown[button] = down;
    }

    switch (button) {
        case 0:  // Left
            type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
            mouseBtn = kCGMouseButtonLeft;
            break;
        case 1:  // Right
            type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
            mouseBtn = kCGMouseButtonRight;
            break;
        case 2:  // Middle
            type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
            mouseBtn = kCGMouseButtonCenter;
            break;
        default:
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

void MouseWheel(float dx, float dy) {
    int32_t wheelY = static_cast<int32_t>(dy * 10);
    int32_t wheelX = static_cast<int32_t>(dx * 10);

    CGEventRef ev = CGEventCreateScrollWheelEvent2(nullptr, kCGScrollEventUnitPixel, 2, wheelY, wheelX, 0);
    if (ev) {
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
    }
}

static CGKeyCode WinVkToMac(uint16_t vk) {
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

void Key(uint16_t winVk, bool down) {
    CGKeyCode macCode = WinVkToMac(winVk);
    if (macCode == 0xFFFF) return;

    if (macCode == kVK_CapsLock) {
        // Toggle on the press, then keep the resulting latch on the release too.
        // Both events carry the same AlphaShift state, so macOS doesn't undo it.
        if (down) {
            g_capsLock = !g_capsLock;
            g_keysDown.insert(macCode);
        } else {
            g_keysDown.erase(macCode);
        }
        PostKey(macCode, down);
        return;
    }

    if (down)
        g_keysDown.insert(macCode);
    else
        g_keysDown.erase(macCode);
    PostKey(macCode, down);
}

void Text(const std::string& utf8Text) {
    NSString* nsText = [NSString stringWithUTF8String:utf8Text.c_str()];
    if (!nsText || nsText.length == 0) return;

    NSUInteger len = nsText.length;
    std::vector<UniChar> buffer(len);
    [nsText getCharacters:buffer.data() range:NSMakeRange(0, len)];

    CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 0, true);
    if (down) {
        CGEventSetFlags(down, 0);  // text is already composed; never apply a stuck modifier
        CGEventKeyboardSetUnicodeString(down, len, buffer.data());
        CGEventPost(kCGHIDEventTap, down);
        CFRelease(down);
    }
    // The old code left a key-down with no key-up, which latches a key in macOS.
    PostKey(0, false);
}

void ReleaseAll() {
    for (CGKeyCode code : g_keysDown) {
        PostKey(code, false);
    }
    g_keysDown.clear();

    // A dropped Bluetooth key-up, or a streamer killed mid-key, leaves Command latched
    // in macOS's global modifier state (CGEvent key-downs persist until a key-up is
    // posted). A key-up for a key that isn't down is harmless, so clear every modifier
    // unconditionally: this is what stops a later 'e', 'd' or 'f' from becoming Cmd+E/D/F.
    // Caps Lock is a toggle, not a held key: posting an unconditional key-up for it
    // here would switch the user's Caps Lock off on session close/startup.
    static const CGKeyCode kModifiers[] = {
        kVK_Command, kVK_RightCommand, kVK_Shift, kVK_RightShift,
        kVK_Option, kVK_RightOption, kVK_Control, kVK_RightControl,
    };
    for (CGKeyCode code : kModifiers) {
        PostKey(code, false);
    }

    for (int i = 0; i < 5; ++i) {
        if (g_mouseButtonsDown[i]) {
            MouseButton(i, false);
        }
    }
}

}  // namespace input
}  // namespace rcm
