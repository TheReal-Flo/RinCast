#include "input_injector.h"

#include <windows.h>

#include <cmath>
#include <mutex>
#include <set>

namespace rcs::input {

namespace {

std::mutex g_mutex;
std::set<int> g_buttonsDown;
std::set<unsigned short> g_keysDown;
float g_wheelX = 0, g_wheelY = 0;

bool IsExtended(unsigned short vk) {
    switch (vk) {
        case VK_RMENU: case VK_RCONTROL: case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_NUMLOCK: case VK_SNAPSHOT: case VK_DIVIDE: case VK_LWIN: case VK_RWIN: case VK_APPS:
            return true;
    }
    return false;
}

void Send(INPUT* inputs, UINT count) { SendInput(count, inputs, sizeof(INPUT)); }

}  // namespace

void MoveMouse(const MonitorInfo& monitor, float u, float v) {
    u = std::fmin(std::fmax(u, 0.f), 1.f);
    v = std::fmin(std::fmax(v, 0.f), 1.f);
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const float px = monitor.desktopRect.left + u * float(monitor.desktopRect.right - monitor.desktopRect.left - 1);
    const float py = monitor.desktopRect.top + v * float(monitor.desktopRect.bottom - monitor.desktopRect.top - 1);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = LONG(std::lround((px - vx) * 65535.0 / std::max(vw - 1, 1)));
    in.mi.dy = LONG(std::lround((py - vy) * 65535.0 / std::max(vh - 1, 1)));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    Send(&in, 1);
}

void MouseButton(int button, bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    switch (button) {
        case 0: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case 1: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        case 2: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case 3:
        case 4:
            in.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            in.mi.mouseData = button == 3 ? XBUTTON1 : XBUTTON2;
            break;
        default: return;
    }
    {
        std::lock_guard lock(g_mutex);
        if (down)
            g_buttonsDown.insert(button);
        else
            g_buttonsDown.erase(button);
    }
    Send(&in, 1);
}

void MouseWheel(float dx, float dy) {
    std::lock_guard lock(g_mutex);
    g_wheelX += dx * WHEEL_DELTA;
    g_wheelY += dy * WHEEL_DELTA;
    INPUT ins[2]{};
    UINT count = 0;
    // High-resolution wheel: Windows accepts deltas below WHEEL_DELTA.
    if (std::fabs(g_wheelY) >= 10) {
        ins[count].type = INPUT_MOUSE;
        ins[count].mi.dwFlags = MOUSEEVENTF_WHEEL;
        ins[count].mi.mouseData = DWORD(LONG(g_wheelY));
        g_wheelY -= float(LONG(g_wheelY));
        count++;
    }
    if (std::fabs(g_wheelX) >= 10) {
        ins[count].type = INPUT_MOUSE;
        ins[count].mi.dwFlags = MOUSEEVENTF_HWHEEL;
        ins[count].mi.mouseData = DWORD(LONG(g_wheelX));
        g_wheelX -= float(LONG(g_wheelX));
        count++;
    }
    if (count) Send(ins, count);
}

void Key(unsigned short vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = WORD(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX) & 0xFF);
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (IsExtended(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    {
        std::lock_guard lock(g_mutex);
        if (down)
            g_keysDown.insert(vk);
        else
            g_keysDown.erase(vk);
    }
    Send(&in, 1);
}

void Text(const std::wstring& text) {
    std::vector<INPUT> ins;
    ins.reserve(text.size() * 2);
    for (wchar_t c : text) {
        if (c == L'\n' || c == L'\r') {
            Key(VK_RETURN, true);
            Key(VK_RETURN, false);
            continue;
        }
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = c;
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        INPUT up = down;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        ins.push_back(down);
        ins.push_back(up);
    }
    if (!ins.empty()) Send(ins.data(), UINT(ins.size()));
}

void ReleaseAll() {
    std::set<int> buttons;
    std::set<unsigned short> keys;
    {
        std::lock_guard lock(g_mutex);
        buttons = g_buttonsDown;
        keys = g_keysDown;
    }
    for (int b : buttons) MouseButton(b, false);
    for (unsigned short k : keys) Key(k, false);
}

}  // namespace rcs::input

namespace rcs::input {

void WakeDisplays() {
    SetThreadExecutionState(ES_DISPLAY_REQUIRED);
    INPUT ins[2]{};
    ins[0].type = ins[1].type = INPUT_MOUSE;
    ins[0].mi.dx = 1;
    ins[0].mi.dwFlags = MOUSEEVENTF_MOVE;
    ins[1].mi.dx = -1;
    ins[1].mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(2, ins, sizeof(INPUT));
}

}  // namespace rcs::input
