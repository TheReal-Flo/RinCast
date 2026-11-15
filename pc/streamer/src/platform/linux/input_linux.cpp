// Mouse and keyboard from the headset on Linux: XTest on X11 (here); on Wayland the RemoteDesktop
// portal's libei, or /dev/uinput (input_wayland.cpp). The headset sends Windows virtual-key codes;
// they map to evdev key positions (linux_input.h).
#include "input/input_injector.h"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/dpms.h>
#include <X11/keysym.h>
#include <linux/input-event-codes.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "platform/linux/linux_capture.h"
#include "platform/linux/linux_input.h"
#include "platform/linux/x11_util.h"
#include "rc/platform.h"

namespace rcs::input {

namespace {

constexpr int kLetters[26] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
                              KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
constexpr int kDigits[10] = {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9};
constexpr int kNumpad[10] = {KEY_KP0, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9};
constexpr int kFunction[24] = {KEY_F1,  KEY_F2,  KEY_F3,  KEY_F4,  KEY_F5,  KEY_F6,  KEY_F7,  KEY_F8,
                               KEY_F9,  KEY_F10, KEY_F11, KEY_F12, KEY_F13, KEY_F14, KEY_F15, KEY_F16,
                               KEY_F17, KEY_F18, KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24};
// The headset's buttons 0..4: left, right, middle, back, forward.
constexpr int kButtons[5] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA};

std::set<uint16_t> g_vksDown;        // the virtual keys held down
bool g_buttonsDown[5] = {};
float g_wheelX = 0, g_wheelY = 0;    // in 1/120 notch, not sent yet

InputBackend& Backend() {
    static std::unique_ptr<InputBackend> backend = IsWaylandSession() ? CreateWaylandInput() : CreateX11Input();
    return *backend;
}

// The keysym a character is typed with: Latin-1 directly, the rest as Unicode keysyms.
KeySym CharKeysym(char32_t c) {
    switch (c) {
        case '\n':
        case '\r': return XK_Return;
        case '\t': return XK_Tab;
        case '\b': return XK_BackSpace;
    }
    if ((c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xFF)) return KeySym(c);
    return KeySym(0x01000000 | c);
}

// For an X server whose keycodes aren't evdev's (an old one with the kbd driver): the key's
// keysym, which XKeysymToKeycode turns into whatever keycode the server has for it.
KeySym VkToKeysym(uint16_t vk) {
    if (vk >= 'A' && vk <= 'Z') return XK_a + (vk - 'A');
    if (vk >= '0' && vk <= '9') return XK_0 + (vk - '0');
    if (vk >= 0x60 && vk <= 0x69) return XK_KP_0 + (vk - 0x60);
    if (vk >= 0x70 && vk <= 0x87) return XK_F1 + (vk - 0x70);
    switch (vk) {
        case 0x08: return XK_BackSpace;
        case 0x09: return XK_Tab;
        case 0x0D: return XK_Return;
        case 0x10: case 0xA0: return XK_Shift_L;
        case 0xA1: return XK_Shift_R;
        case 0x11: case 0xA2: return XK_Control_L;
        case 0xA3: return XK_Control_R;
        case 0x12: case 0xA4: return XK_Alt_L;
        case 0xA5: return XK_ISO_Level3_Shift;
        case 0x13: return XK_Pause;
        case 0x14: return XK_Caps_Lock;
        case 0x1B: return XK_Escape;
        case 0x20: return XK_space;
        case 0x21: return XK_Prior;
        case 0x22: return XK_Next;
        case 0x23: return XK_End;
        case 0x24: return XK_Home;
        case 0x25: return XK_Left;
        case 0x26: return XK_Up;
        case 0x27: return XK_Right;
        case 0x28: return XK_Down;
        case 0x2C: return XK_Print;
        case 0x2D: return XK_Insert;
        case 0x2E: return XK_Delete;
        case 0x5B: return XK_Super_L;
        case 0x5C: return XK_Super_R;
        case 0x5D: return XK_Menu;
        case 0x6A: return XK_KP_Multiply;
        case 0x6B: return XK_KP_Add;
        case 0x6D: return XK_KP_Subtract;
        case 0x6E: return XK_KP_Decimal;
        case 0x6F: return XK_KP_Divide;
        case 0x90: return XK_Num_Lock;
        case 0x91: return XK_Scroll_Lock;
        case 0xBA: return XK_semicolon;
        case 0xBB: return XK_equal;
        case 0xBC: return XK_comma;
        case 0xBD: return XK_minus;
        case 0xBE: return XK_period;
        case 0xBF: return XK_slash;
        case 0xC0: return XK_grave;
        case 0xDB: return XK_bracketleft;
        case 0xDC: return XK_backslash;
        case 0xDD: return XK_bracketright;
        case 0xDE: return XK_apostrophe;
        case 0xE2: return XK_less;
    }
    return NoSymbol;
}

// --- X11: XTest ----------------------------------------------------------------------------------

class X11Input : public InputBackend {
   public:
    ~X11Input() override { x11::Close(m_display); }

    void Move(const MonitorInfo& monitor, float u, float v) override {
        if (!Ready()) return;
        const RECT& r = monitor.desktopRect;
        const int x = r.left + int(std::lround(u * float(r.right - r.left - 1)));
        const int y = r.top + int(std::lround(v * float(r.bottom - r.top - 1)));
        XTestFakeMotionEvent(m_display, DefaultScreen(m_display), x, y, CurrentTime);
        XFlush(m_display);
    }

    void Button(int evdevButton, bool down) override {
        if (!Ready()) return;
        unsigned button = 0;
        switch (evdevButton) {
            case BTN_LEFT: button = 1; break;
            case BTN_MIDDLE: button = 2; break;
            case BTN_RIGHT: button = 3; break;
            case BTN_SIDE: button = 8; break;
            case BTN_EXTRA: button = 9; break;
            default: return;
        }
        XTestFakeButtonEvent(m_display, button, down, CurrentTime);
        XFlush(m_display);
    }

    // X wheels are buttons, one click per notch (4 up, 5 down, 6 left, 7 right): fractions add
    // up here until a whole notch is due.
    void Wheel(int32_t x120, int32_t y120) override {
        if (!Ready()) return;
        m_wheelX += x120;
        m_wheelY += y120;
        auto clicks = [&](int32_t& acc, unsigned positive, unsigned negative) {
            while (acc >= 120 || acc <= -120) {
                const unsigned button = acc > 0 ? positive : negative;
                XTestFakeButtonEvent(m_display, button, True, CurrentTime);
                XTestFakeButtonEvent(m_display, button, False, CurrentTime);
                acc += acc > 0 ? -120 : 120;
            }
        };
        clicks(m_wheelY, 4, 5);
        clicks(m_wheelX, 7, 6);
        XFlush(m_display);
    }

    void Key(int evdevKey, uint16_t vk, bool down) override {
        if (!Ready()) return;
        KeyCode code = 0;
        if (m_evdevKeycodes) code = KeyCode(evdevKey + 8);  // X keycodes are evdev's plus 8
        else if (KeySym sym = VkToKeysym(vk); sym != NoSymbol) code = XKeysymToKeycode(m_display, sym);
        if (!code) return;
        XTestFakeKeyEvent(m_display, code, down, CurrentTime);
        XFlush(m_display);
    }

    // Each character through a key that has its keysym in the current layout (with Shift when it
    // is on the second level). A character no key has is put on a spare keycode for the moment,
    // as xdotool does: XChangeKeyboardMapping, the key, and the old mapping back once the
    // applications have had time to read the key with the new one.
    void Text(const std::u32string& text) override {
        if (!Ready()) return;
        Drain();
        XkbStateRec state{};
        XkbGetState(m_display, XkbUseCoreKbd, &state);
        const KeyCode shift = XKeysymToKeycode(m_display, XK_Shift_L);
        std::vector<std::pair<KeyCode, KeySym>> remapped;
        for (char32_t c : text) {
            const KeySym sym = CharKeysym(c);
            KeyCode code = XKeysymToKeycode(m_display, sym);
            int level = -1;
            if (code) {
                for (int l = 0; l < 2 && level < 0; l++)
                    if (XkbKeycodeToKeysym(m_display, code, state.group, l) == sym) level = l;
            }
            if (level < 0) {
                code = 0;
                for (const auto& [k, s] : remapped)
                    if (s == sym) code = k;
                if (!code) {
                    const std::vector<KeyCode>& spares = Spares();
                    if (spares.empty()) continue;
                    if (remapped.size() == spares.size()) RestoreSpares(remapped);
                    code = spares[remapped.size()];
                    KeySym syms[2] = {sym, sym};
                    XChangeKeyboardMapping(m_display, code, 2, syms, 1);
                    XSync(m_display, False);
                    remapped.emplace_back(code, sym);
                }
                level = 0;
            }
            if (level == 1 && shift) XTestFakeKeyEvent(m_display, shift, True, CurrentTime);
            XTestFakeKeyEvent(m_display, code, True, CurrentTime);
            XTestFakeKeyEvent(m_display, code, False, CurrentTime);
            if (level == 1 && shift) XTestFakeKeyEvent(m_display, shift, False, CurrentTime);
        }
        XSync(m_display, False);
        RestoreSpares(remapped);
    }

    // The screen saver and DPMS both count XTest input as activity, but waking needs a nudge
    // when nothing comes from the headset yet (the stream waits for its first picture).
    void Wake() override {
        if (!Ready()) return;
        XResetScreenSaver(m_display);
        XForceScreenSaver(m_display, ScreenSaverReset);
        int eventBase = 0, errorBase = 0;
        if (DPMSQueryExtension(m_display, &eventBase, &errorBase) && DPMSCapable(m_display)) DPMSForceLevel(m_display, DPMSModeOn);
        XFlush(m_display);
    }

   private:
    // The display, opened on first use (and again a while after it couldn't be).
    bool Ready() {
        if (m_display) return true;
        const int64_t now = rc::NowNs();
        if (m_lastTry && now - m_lastTry < 5'000'000'000LL) return false;
        m_lastTry = now;
        m_display = x11::Open();
        if (!m_display) {
            RC_LOGW("Input: can't open the X display; mouse and keyboard from the headset are ignored");
            return false;
        }
        int eventBase = 0, errorBase = 0, major = 0, minor = 0;
        if (!XTestQueryExtension(m_display, &eventBase, &errorBase, &major, &minor)) {
            RC_LOGW("Input: the X server has no XTest; mouse and keyboard from the headset are ignored");
            x11::Close(m_display);
            m_display = nullptr;
            return false;
        }
        // Every current X server (Xorg with libinput or evdev, XWayland) numbers its keys as
        // evdev + 8; Home tells.
        m_evdevKeycodes = XKeysymToKeycode(m_display, XK_Home) == KEY_HOME + 8;
        if (!m_evdevKeycodes) RC_LOGI("Input: the X server's keycodes aren't evdev's; mapping keys by their symbols");
        return true;
    }

    // Reads what the server sent (keyboard mapping changes, which keep Xlib's view of the layout
    // current), so it doesn't pile up on a connection that otherwise never reads events.
    void Drain() {
        while (XPending(m_display)) {
            XEvent event;
            XNextEvent(m_display, &event);
            if (event.type == MappingNotify) XRefreshKeyboardMapping(&event.xmapping);
        }
    }

    // Keycodes with no keysym at all, from the top: the ones a layout leaves free.
    const std::vector<KeyCode>& Spares() {
        if (m_sparesKnown) return m_spares;
        m_sparesKnown = true;
        int minCode = 0, maxCode = 0;
        XDisplayKeycodes(m_display, &minCode, &maxCode);
        int perCode = 0;
        KeySym* map = XGetKeyboardMapping(m_display, KeyCode(minCode), maxCode - minCode + 1, &perCode);
        if (!map) return m_spares;
        for (int code = maxCode; code >= minCode && m_spares.size() < 8; code--) {
            bool empty = true;
            for (int i = 0; i < perCode && empty; i++)
                if (map[(code - minCode) * perCode + i] != NoSymbol) empty = false;
            if (empty) m_spares.push_back(KeyCode(code));
        }
        XFree(map);
        if (m_spares.empty()) RC_LOGW("Input: no free keycode; characters the keyboard layout lacks can't be typed");
        return m_spares;
    }

    void RestoreSpares(std::vector<std::pair<KeyCode, KeySym>>& remapped) {
        if (remapped.empty()) return;
        // Applications look the key up when they handle it, which can be after we're done.
        usleep(50'000);
        for (const auto& [code, sym] : remapped) {
            KeySym none[2] = {NoSymbol, NoSymbol};
            XChangeKeyboardMapping(m_display, code, 2, none, 1);
        }
        XSync(m_display, False);
        remapped.clear();
    }

    Display* m_display = nullptr;
    int64_t m_lastTry = 0;
    bool m_evdevKeycodes = true;
    int32_t m_wheelX = 0, m_wheelY = 0;
    bool m_sparesKnown = false;
    std::vector<KeyCode> m_spares;
};

}  // namespace

std::mutex& InputMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unique_ptr<InputBackend> CreateX11Input() { return std::make_unique<X11Input>(); }

int VkToEvdev(uint16_t vk) {
    if (vk >= 'A' && vk <= 'Z') return kLetters[vk - 'A'];
    if (vk >= '0' && vk <= '9') return kDigits[vk - '0'];
    if (vk >= 0x60 && vk <= 0x69) return kNumpad[vk - 0x60];
    if (vk >= 0x70 && vk <= 0x87) return kFunction[vk - 0x70];
    switch (vk) {
        case 0x08: return KEY_BACKSPACE;
        case 0x09: return KEY_TAB;
        case 0x0D: return KEY_ENTER;
        case 0x10: return KEY_LEFTSHIFT;   // VK_SHIFT
        case 0x11: return KEY_LEFTCTRL;    // VK_CONTROL
        case 0x12: return KEY_LEFTALT;     // VK_MENU
        case 0x13: return KEY_PAUSE;
        case 0x14: return KEY_CAPSLOCK;
        case 0x15: return KEY_HANGEUL;     // VK_KANA / VK_HANGUL
        case 0x19: return KEY_HANJA;       // VK_HANJA / VK_KANJI
        case 0x1B: return KEY_ESC;
        case 0x1C: return KEY_HENKAN;      // VK_CONVERT
        case 0x1D: return KEY_MUHENKAN;    // VK_NONCONVERT
        case 0x20: return KEY_SPACE;
        case 0x21: return KEY_PAGEUP;
        case 0x22: return KEY_PAGEDOWN;
        case 0x23: return KEY_END;
        case 0x24: return KEY_HOME;
        case 0x25: return KEY_LEFT;
        case 0x26: return KEY_UP;
        case 0x27: return KEY_RIGHT;
        case 0x28: return KEY_DOWN;
        case 0x2C: return KEY_SYSRQ;       // VK_SNAPSHOT (Print Screen)
        case 0x2D: return KEY_INSERT;
        case 0x2E: return KEY_DELETE;
        case 0x2F: return KEY_HELP;
        case 0x5B: return KEY_LEFTMETA;    // VK_LWIN
        case 0x5C: return KEY_RIGHTMETA;
        case 0x5D: return KEY_COMPOSE;     // VK_APPS: the menu key
        case 0x5F: return KEY_SLEEP;
        case 0x6A: return KEY_KPASTERISK;
        case 0x6B: return KEY_KPPLUS;
        case 0x6C: return KEY_KPCOMMA;     // VK_SEPARATOR
        case 0x6D: return KEY_KPMINUS;
        case 0x6E: return KEY_KPDOT;
        case 0x6F: return KEY_KPSLASH;
        case 0x90: return KEY_NUMLOCK;
        case 0x91: return KEY_SCROLLLOCK;
        case 0xA0: return KEY_LEFTSHIFT;
        case 0xA1: return KEY_RIGHTSHIFT;
        case 0xA2: return KEY_LEFTCTRL;
        case 0xA3: return KEY_RIGHTCTRL;
        case 0xA4: return KEY_LEFTALT;
        case 0xA5: return KEY_RIGHTALT;    // AltGr on layouts that have it
        case 0xA6: return KEY_BACK;        // browser keys
        case 0xA7: return KEY_FORWARD;
        case 0xA8: return KEY_REFRESH;
        case 0xA9: return KEY_STOP;
        case 0xAA: return KEY_SEARCH;
        case 0xAB: return KEY_BOOKMARKS;
        case 0xAC: return KEY_HOMEPAGE;
        case 0xAD: return KEY_MUTE;        // media keys
        case 0xAE: return KEY_VOLUMEDOWN;
        case 0xAF: return KEY_VOLUMEUP;
        case 0xB0: return KEY_NEXTSONG;
        case 0xB1: return KEY_PREVIOUSSONG;
        case 0xB2: return KEY_STOPCD;
        case 0xB3: return KEY_PLAYPAUSE;
        case 0xB4: return KEY_MAIL;
        case 0xB7: return KEY_CALC;
        // OEM keys: US-layout positions; the PC's layout decides what they type (on a German
        // layout VK_OEM_1's position is Ü, as it is on Windows).
        case 0xBA: return KEY_SEMICOLON;
        case 0xBB: return KEY_EQUAL;
        case 0xBC: return KEY_COMMA;
        case 0xBD: return KEY_MINUS;
        case 0xBE: return KEY_DOT;
        case 0xBF: return KEY_SLASH;
        case 0xC0: return KEY_GRAVE;
        case 0xDB: return KEY_LEFTBRACE;
        case 0xDC: return KEY_BACKSLASH;
        case 0xDD: return KEY_RIGHTBRACE;
        case 0xDE: return KEY_APOSTROPHE;
        case 0xE2: return KEY_102ND;       // the extra key of ISO keyboards
    }
    return 0;
}

bool UsKeyFor(char32_t c, int& evdevKey, bool& shift) {
    shift = false;
    if (c >= 'a' && c <= 'z') {
        evdevKey = kLetters[c - 'a'];
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        evdevKey = kLetters[c - 'A'];
        shift = true;
        return true;
    }
    if (c >= '0' && c <= '9') {
        evdevKey = kDigits[c - '0'];
        return true;
    }
    static constexpr struct {
        char c;
        int key;
        bool shift;
    } kKeys[] = {
        {' ', KEY_SPACE, false},      {'\n', KEY_ENTER, false},      {'\r', KEY_ENTER, false},
        {'\t', KEY_TAB, false},       {'\b', KEY_BACKSPACE, false},  {'!', KEY_1, true},
        {'@', KEY_2, true},           {'#', KEY_3, true},            {'$', KEY_4, true},
        {'%', KEY_5, true},           {'^', KEY_6, true},            {'&', KEY_7, true},
        {'*', KEY_8, true},           {'(', KEY_9, true},            {')', KEY_0, true},
        {'-', KEY_MINUS, false},      {'_', KEY_MINUS, true},        {'=', KEY_EQUAL, false},
        {'+', KEY_EQUAL, true},       {'[', KEY_LEFTBRACE, false},   {'{', KEY_LEFTBRACE, true},
        {']', KEY_RIGHTBRACE, false}, {'}', KEY_RIGHTBRACE, true},   {'\\', KEY_BACKSLASH, false},
        {'|', KEY_BACKSLASH, true},   {';', KEY_SEMICOLON, false},   {':', KEY_SEMICOLON, true},
        {'\'', KEY_APOSTROPHE, false}, {'"', KEY_APOSTROPHE, true},  {',', KEY_COMMA, false},
        {'<', KEY_COMMA, true},       {'.', KEY_DOT, false},         {'>', KEY_DOT, true},
        {'/', KEY_SLASH, false},      {'?', KEY_SLASH, true},        {'`', KEY_GRAVE, false},
        {'~', KEY_GRAVE, true},
    };
    for (const auto& k : kKeys)
        if (char32_t(static_cast<unsigned char>(k.c)) == c) {
            evdevKey = k.key;
            shift = k.shift;
            return true;
        }
    return false;
}

// u, v across the monitor's area: root window coordinates on X11, the compositor's logical
// space on Wayland.
void MoveMouse(const MonitorInfo& monitor, float u, float v) {
    std::lock_guard lock(InputMutex());
    Backend().Move(monitor, std::clamp(u, 0.0f, 1.0f), std::clamp(v, 0.0f, 1.0f));
}

void MouseButton(int button, bool down) {
    std::lock_guard lock(InputMutex());
    if (button < 0 || button >= 5) return;
    g_buttonsDown[button] = down;
    Backend().Button(kButtons[button], down);
}

// Notches as the headset sends them (1.0 = one wheel detent, often less from a thumbstick):
// fractions add up, so slow scrolling isn't lost.
void MouseWheel(float dx, float dy) {
    std::lock_guard lock(InputMutex());
    g_wheelX += dx * 120.f;
    g_wheelY += dy * 120.f;
    const int32_t x = int32_t(std::trunc(g_wheelX)), y = int32_t(std::trunc(g_wheelY));
    if (!x && !y) return;
    g_wheelX -= float(x);
    g_wheelY -= float(y);
    Backend().Wheel(x, y);
}

void Key(unsigned short vk, bool down) {
    std::lock_guard lock(InputMutex());
    const int code = VkToEvdev(vk);
    if (!code) return;
    if (down) g_vksDown.insert(vk);
    else g_vksDown.erase(vk);
    Backend().Key(code, vk, down);
}

void Text(const std::wstring& text) {
    std::lock_guard lock(InputMutex());
    // wchar_t is UTF-32 on Linux.
    std::u32string chars;
    for (wchar_t c : text) chars += char32_t(c);
    if (!chars.empty()) Backend().Text(chars);
}

void ReleaseAll() {
    std::lock_guard lock(InputMutex());
    for (uint16_t vk : g_vksDown) Backend().Key(VkToEvdev(vk), vk, false);
    g_vksDown.clear();
    for (int i = 0; i < 5; ++i)
        if (g_buttonsDown[i]) {
            g_buttonsDown[i] = false;
            Backend().Button(kButtons[i], false);
        }
    g_wheelX = g_wheelY = 0;
}

void WakeDisplays() {
    std::lock_guard lock(InputMutex());
    Backend().Wake();
}

void PrepareInput() {
    std::lock_guard lock(InputMutex());
    Backend().Prepare();
}

}  // namespace rcs::input
