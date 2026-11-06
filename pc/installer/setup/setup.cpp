// RinCast's setup and uninstaller: one small program.
//
//   RinCast-<version>-Setup.exe            installs, with a window (or --quiet without one)
//   Uninstall RinCast.exe [--quiet]        removes it again (Apps > Installed apps runs this)
//   RinCast Setup.exe --pack <dir> <out>   build step: this program plus the files of <dir>
//
// The files travel appended to the program. The steps themselves are install.ps1 and
// uninstall.ps1 from the package, run hidden: the same ones a manual install uses.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "../../streamer/src/version.h"

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[8] = {'R', 'C', 'S', 'E', 'T', 'U', 'P', '1'};
constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RinCast";
constexpr wchar_t kDownloadUrl[] = L"https://rincast.justfeli.dev/#download";

// ---- small helpers --------------------------------------------------------------------

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
    return s;
}

std::wstring SelfPath() {
    wchar_t p[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, p, MAX_PATH * 4);
    return p;
}

bool IsElevated() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                    GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size);
    if (token) CloseHandle(token);
    return ok && e.TokenIsElevated;
}

std::wstring RegString(const wchar_t* name) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kUninstallKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

bool ProcessRunning(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL more = Process32FirstW(snap, &pe); more && !found; more = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, exe) == 0;
    CloseHandle(snap);
    return found;
}

// Through Explorer: runs as the person at the desktop, not elevated like this program.
void OpenAsUser(const std::wstring& target) {
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + target + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

// ---- the payload: files appended to this program ----------------------------------------
// [file]... then [u64 start][u32 count][magic]. Each file: u32 path length, UTF-8 path with
// '/', u64 size, bytes.

bool Pack(const fs::path& dir, const fs::path& out) {
    std::ifstream self(SelfPath(), std::ios::binary);
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    if (!self || !o) return false;
    o << self.rdbuf();
    const uint64_t start = uint64_t(o.tellp());
    uint32_t count = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        std::string rel = Narrow(fs::relative(e.path(), dir).wstring());
        std::replace(rel.begin(), rel.end(), '\\', '/');
        std::ifstream in(e.path(), std::ios::binary);
        const uint64_t size = uint64_t(e.file_size());
        const uint32_t len = uint32_t(rel.size());
        o.write(reinterpret_cast<const char*>(&len), 4);
        o.write(rel.data(), len);
        o.write(reinterpret_cast<const char*>(&size), 8);
        o << in.rdbuf();
        count++;
    }
    o.write(reinterpret_cast<const char*>(&start), 8);
    o.write(reinterpret_cast<const char*>(&count), 4);
    o.write(kMagic, 8);
    return bool(o);
}

bool HasPayload() {
    std::ifstream f(SelfPath(), std::ios::binary);
    char magic[8] = {};
    f.seekg(-8, std::ios::end);
    f.read(magic, 8);
    return f && std::equal(magic, magic + 8, kMagic);
}

bool Extract(const fs::path& to) {
    std::ifstream f(SelfPath(), std::ios::binary);
    uint64_t start = 0;
    uint32_t count = 0;
    f.seekg(-20, std::ios::end);
    f.read(reinterpret_cast<char*>(&start), 8);
    f.read(reinterpret_cast<char*>(&count), 4);
    if (!f) return false;
    f.seekg(std::streamoff(start));
    std::vector<char> buf(1 << 20);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t len = 0;
        uint64_t size = 0;
        f.read(reinterpret_cast<char*>(&len), 4);
        if (!f || len > 4096) return false;
        std::string rel(len, '\0');
        f.read(rel.data(), len);
        f.read(reinterpret_cast<char*>(&size), 8);
        const fs::path path = to / fs::path(Widen(rel));
        // Nothing outside the folder it's unpacked to.
        if (rel.find("..") != std::string::npos || !f) return false;
        fs::create_directories(path.parent_path());
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        for (uint64_t left = size; left > 0;) {
            const size_t n = size_t(std::min<uint64_t>(left, buf.size()));
            f.read(buf.data(), std::streamsize(n));
            o.write(buf.data(), std::streamsize(n));
            if (!f || !o) return false;
            left -= n;
        }
    }
    return true;
}

// ---- running the steps ------------------------------------------------------------------

struct Progress {
    std::function<void(const std::wstring&)> step;     // "Copying files to ..."
    std::function<void(const std::wstring&)> warning;  // worth showing at the end
};

// Runs a script hidden and reports its "-> " steps. Returns its exit code; `error` gets the
// line that says why it failed, and `log` everything.
int RunScript(const fs::path& script, const std::wstring& args, const Progress& p, std::wstring& error, std::wstring& log) {
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive "
                       L"-ExecutionPolicy Bypass -File \"" + script.wstring() + L"\" " + args;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    CreatePipe(&readPipe, &writePipe, &sa, 0);
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        error = L"PowerShell didn't start.";
        return -1;
    }
    CloseHandle(writePipe);
    std::string pending;
    char chunk[4096];
    DWORD got = 0;
    auto line = [&](std::string l) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        const std::wstring w = Widen(l);
        log += w + L"\r\n";
        if (l.rfind("-> ", 0) == 0) p.step(w.substr(3));
        else if (l.rfind("!  ", 0) == 0) p.warning(w.substr(3));
        else if (l.find("RinCast was not installed: ") != std::string::npos) error = w.substr(w.find(L": ") + 2);
    };
    while (ReadFile(readPipe, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
        pending.append(chunk, got);
        for (size_t nl; (nl = pending.find('\n')) != std::string::npos; pending.erase(0, nl + 1)) line(pending.substr(0, nl));
    }
    if (!pending.empty()) line(pending);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readPipe);
    return int(code);
}

struct Options {
    std::wstring dir;
    bool allowPublic = false;
    bool keepSettings = false;
};

int Install(const Options& o, const Progress& p, std::wstring& error, std::wstring& log) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const fs::path work = fs::path(temp) / (L"RinCast-Setup-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(work, ec);
    p.step(L"Unpacking");
    if (!Extract(work)) {
        error = L"This setup file is damaged. Download it again.";
        return 1;
    }
    std::wstring args = L"-InstallDir \"" + o.dir + L"\" -NoStart";
    if (o.allowPublic) args += L" -AllowPublic";
    const int code = RunScript(work / L"install.ps1", args, p, error, log);
    fs::remove_all(work, ec);
    if (code != 0 && error.empty()) error = L"The installation stopped (" + std::to_wstring(code) + L").";
    return code;
}

int Uninstall(const Options& o, const Progress& p, std::wstring& error, std::wstring& log) {
    p.step(L"Removing RinCast");
    const fs::path script = fs::path(o.dir) / L"uninstall.ps1";
    if (!fs::exists(script)) {
        error = L"RinCast's uninstaller is missing from " + o.dir + L".";
        return 1;
    }
    return RunScript(script, o.keepSettings ? L"-KeepSettings" : L"", p, error, log);
}

// ---- the window ---------------------------------------------------------------------------

enum class Mode { Install, Uninstall };
enum class Page { Choose, Working, Done, Failed };

struct Rgb {
    BYTE r, g, b;
    COLORREF c() const { return RGB(r, g, b); }
};
const Rgb kBg{0, 0, 0}, kText{255, 255, 255}, kMuted{200, 200, 200}, kAccent{46, 196, 255}, kAccentHover{138, 223, 255},
    kControl{58, 58, 58}, kControlHover{77, 77, 77}, kEdge{140, 140, 140}, kHoverRow{38, 38, 38}, kWarn{255, 184, 77},
    kDanger{168, 32, 26}, kDangerHover{198, 43, 36}, kLine{74, 74, 74};

struct Item {
    enum Kind { Button, Check, Link } kind;
    RECT r;  // in device pixels
    std::wstring label;
    int style = 0;  // buttons: 0 secondary, 1 primary, 2 danger
    bool* value = nullptr;
    std::function<void()> run;
};

struct App {
    HWND hwnd = nullptr;
    Mode mode = Mode::Install;
    Page page = Page::Choose;
    Options options;
    bool openWhenDone = true;
    bool updating = false;
    std::wstring installedVersion, newVersion = Widen(RINCAST_VERSION_STRING);
    std::wstring step, error, log;
    std::vector<std::wstring> warnings;
    int steps = 0;
    std::vector<Item> items;
    int hot = -1, pressed = -1;
    UINT dpi = 96;
    HFONT title = nullptr, body = nullptr, note = nullptr, bold = nullptr;
    std::thread worker;

    int S(int v) const { return MulDiv(v, int(dpi), 96); }
};

App g;
constexpr UINT WM_STEP = WM_APP + 1, WM_WARN = WM_APP + 2, WM_FINISHED = WM_APP + 3;

void MakeFonts() {
    for (HFONT f : {g.title, g.body, g.note, g.bold})
        if (f) DeleteObject(f);
    auto font = [](int px, int weight, const wchar_t* face) {
        return CreateFontW(-g.S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
    };
    g.title = font(28, FW_SEMIBOLD, L"Segoe UI Variable Display");
    g.body = font(15, FW_NORMAL, L"Segoe UI Variable Text");
    g.note = font(13, FW_NORMAL, L"Segoe UI Variable Text");
    g.bold = font(15, FW_SEMIBOLD, L"Segoe UI Variable Text");
}

void Fill(HDC dc, const RECT& r, const Rgb& c, int radius = 0) {
    HBRUSH b = CreateSolidBrush(c.c());
    HPEN p = CreatePen(PS_SOLID, 1, c.c());
    auto ob = SelectObject(dc, b);
    auto op = SelectObject(dc, p);
    if (radius) RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    else Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

void Outline(HDC dc, const RECT& r, const Rgb& c, int width, int radius) {
    HPEN p = CreatePen(PS_INSIDEFRAME, width, c.c());
    auto op = SelectObject(dc, p);
    auto ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
}

// Draws wrapped text at (x, y), at most `w` wide; returns its height.
int Text(HDC dc, const std::wstring& s, int x, int y, int w, HFONT font, const Rgb& color, UINT extra = 0) {
    SelectObject(dc, font);
    SetTextColor(dc, color.c());
    RECT r{x, y, x + w, y + 10000};
    DrawTextW(dc, s.c_str(), -1, &r, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT | extra);
    const int h = r.bottom - r.top;
    r.right = x + w;
    DrawTextW(dc, s.c_str(), -1, &r, DT_WORDBREAK | DT_NOPREFIX | extra);
    return h;
}

void AddButton(const std::wstring& label, int style, RECT r, std::function<void()> run) {
    g.items.push_back({Item::Button, r, label, style, nullptr, std::move(run)});
}

void StartWork();
void Close() { DestroyWindow(g.hwnd); }

void PickFolder() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&d)))) return;
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Install RinCast in");
    IShellItem* item = nullptr;
    if (SUCCEEDED(d->Show(g.hwnd)) && SUCCEEDED(d->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            std::wstring p = path;
            // A folder of its own, as uninstalling removes it.
            if (fs::path(p).filename() != L"RinCast") p = (fs::path(p) / L"RinCast").wstring();
            g.options.dir = p;
            CoTaskMemFree(path);
        }
        item->Release();
    }
    d->Release();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// Lays out and paints the page; the clickable parts are collected in g.items.
void Paint(HDC dc, const RECT& client) {
    g.items.clear();
    Fill(dc, client, kBg);
    SetBkMode(dc, TRANSPARENT);
    const int pad = g.S(36), w = client.right - 2 * pad;
    int y = g.S(30);
    const bool install = g.mode == Mode::Install;
    auto heading = [&](const std::wstring& s) { y += Text(dc, s, pad, y, w, g.title, kText) + g.S(10); };
    auto para = [&](const std::wstring& s, const Rgb& c = kMuted) { y += Text(dc, s, pad, y, w, g.body, c) + g.S(14); };
    const int bw = g.S(150), bh = g.S(40), by = client.bottom - g.S(28) - bh;
    RECT right{client.right - pad - bw, by, client.right - pad, by + bh};
    RECT left{right.left - g.S(12) - bw, by, right.left - g.S(12), by + bh};

    switch (g.page) {
        case Page::Choose:
            if (install) {
                heading(g.updating ? (g.installedVersion == g.newVersion ? L"Reinstall RinCast" : L"Update RinCast") : L"Install RinCast");
                para(g.updating ? L"RinCast " + g.installedVersion + L" is installed. This puts " + g.newVersion +
                                      L" in its place and keeps your settings and paired headsets."
                                : L"RinCast Streamer and its SteamVR driver. Then open RinCast on your Quest to play this PC's games "
                                  L"and use its desktop.");
                y += g.S(4);
                Text(dc, L"Folder", pad, y, w, g.note, kMuted);
                y += g.S(20);
                const int changeW = g.S(110);
                Text(dc, g.options.dir, pad, y + g.S(9), w - changeW - g.S(16), g.body, g.updating ? kMuted : kText, DT_PATH_ELLIPSIS | DT_SINGLELINE);
                if (!g.updating) AddButton(L"Change", 0, RECT{pad + w - changeW, y, pad + w, y + g.S(38)}, PickFolder);
                y += g.S(56);
                g.items.push_back({Item::Check, RECT{pad - g.S(8), y, pad + w, y + g.S(58)},
                                   L"Allow headsets on Public networks\nOnly where you trust the network. You can change it in RinCast Streamer.",
                                   0, &g.options.allowPublic, nullptr});
                y += g.S(66);
                g.items.push_back({Item::Check, RECT{pad - g.S(8), y, pad + w, y + g.S(38)}, L"Open RinCast Streamer when it's done", 0,
                                   &g.openWhenDone, nullptr});
                y += g.S(50);
                if (ProcessRunning(L"vrserver.exe") || ProcessRunning(L"RinCast Streamer.exe"))
                    para(L"RinCast Streamer and SteamVR close while this runs.", kWarn);
                AddButton(L"Cancel", 0, left, Close);
                AddButton(g.updating ? L"Update" : L"Install", 1, right, StartWork);
            } else {
                heading(L"Remove RinCast");
                para(L"This removes RinCast Streamer, its SteamVR driver, its firewall rules and its shortcuts. RinCast on "
                     L"your Quest stays.");
                y += g.S(6);
                g.items.push_back({Item::Check, RECT{pad - g.S(8), y, pad + w, y + g.S(58)},
                                   L"Keep my settings\nPaired headsets and settings stay, for installing again later.", 0,
                                   &g.options.keepSettings, nullptr});
                AddButton(L"Cancel", 0, left, Close);
                AddButton(L"Remove", 2, right, StartWork);
            }
            break;
        case Page::Working: {
            heading(install ? (g.updating ? L"Updating RinCast" : L"Installing RinCast") : L"Removing RinCast");
            para(g.step.empty() ? L"Starting" : g.step);
            // Five steps or so; the bar fills as they pass.
            const RECT track{pad, y + g.S(6), pad + w, y + g.S(12)};
            Fill(dc, track, kControl, g.S(3));
            RECT done = track;
            done.right = track.left + (track.right - track.left) * std::min(g.steps, 6) / 6;
            if (done.right > done.left) Fill(dc, done, kAccent, g.S(3));
            break;
        }
        case Page::Done:
            if (install) {
                heading(g.updating ? L"RinCast is updated" : L"RinCast is installed");
                para(L"Open RinCast on your Quest and choose this PC. The first time, allow the headset here on the PC.");
                g.items.push_back({Item::Link, RECT{pad, y, pad + w, y + g.S(24)}, L"Get RinCast for the Quest", 0, nullptr,
                                   [] { OpenAsUser(kDownloadUrl); }});
                y += g.S(40);
            } else {
                heading(L"RinCast was removed");
                para(g.options.keepSettings ? L"Your settings stay, for installing again later." : L"Its settings were removed too.");
            }
            for (const auto& wtext : g.warnings) para(wtext, kWarn);
            if (install && !g.openWhenDone) {
                // Wider: the label is long.
                const RECT open{client.right - pad - g.S(210), by, client.right - pad, by + bh};
                const RECT close{open.left - g.S(12) - bw, by, open.left - g.S(12), by + bh};
                AddButton(L"Close", 0, close, Close);
                AddButton(L"Open RinCast Streamer", 1, open, [] {
                    OpenAsUser((fs::path(g.options.dir) / L"RinCast Streamer.exe").wstring());
                    Close();
                });
            } else {
                AddButton(L"Close", 1, right, Close);
            }
            break;
        case Page::Failed:
            heading(install ? L"RinCast wasn't installed" : L"RinCast wasn't removed");
            para(g.error.empty() ? L"Something went wrong." : g.error, kText);
            if (!g.log.empty())
                g.items.push_back({Item::Link, RECT{pad, y, pad + w, y + g.S(24)}, L"Show the details", 0, nullptr, [] {
                                       wchar_t temp[MAX_PATH];
                                       GetTempPathW(MAX_PATH, temp);
                                       const fs::path p = fs::path(temp) / L"RinCast-Setup.log";
                                       std::ofstream(p, std::ios::binary) << Narrow(g.log);
                                       ShellExecuteW(nullptr, L"open", L"notepad.exe", p.c_str(), nullptr, SW_SHOWNORMAL);
                                   }});
            AddButton(L"Close", 1, right, Close);
            break;
    }

    // Controls, in order; the one under the pointer gets a lighter background.
    for (size_t i = 0; i < g.items.size(); i++) {
        const Item& it = g.items[i];
        const bool hot = int(i) == g.hot, down = int(i) == g.pressed;
        switch (it.kind) {
            case Item::Button: {
                const Rgb bg = it.style == 1 ? (hot ? kAccentHover : kAccent) : it.style == 2 ? (hot ? kDangerHover : kDanger)
                                                                                              : (hot ? kControlHover : kControl);
                Fill(dc, it.r, down && it.style == 0 ? Rgb{90, 90, 90} : bg, g.S(4));
                SelectObject(dc, g.bold);
                SetTextColor(dc, (it.style == 1 ? Rgb{0, 0, 0} : kText).c());
                RECT r = it.r;
                DrawTextW(dc, it.label.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                break;
            }
            case Item::Check: {
                if (hot) Fill(dc, it.r, kHoverRow, g.S(4));
                const RECT box{it.r.left + g.S(8), it.r.top + g.S(9), it.r.left + g.S(30), it.r.top + g.S(31)};
                if (*it.value) {
                    Fill(dc, box, kAccent, g.S(3));
                    HPEN p = CreatePen(PS_SOLID, g.S(2), RGB(0, 0, 0));
                    auto op = SelectObject(dc, p);
                    MoveToEx(dc, box.left + g.S(5), box.top + g.S(11), nullptr);
                    LineTo(dc, box.left + g.S(9), box.top + g.S(15));
                    LineTo(dc, box.left + g.S(17), box.top + g.S(6));
                    SelectObject(dc, op);
                    DeleteObject(p);
                } else {
                    Outline(dc, box, kEdge, g.S(2), g.S(3));
                }
                const size_t nl = it.label.find(L'\n');
                const int tx = box.right + g.S(12), tw = it.r.right - tx - g.S(8);
                const int th = Text(dc, it.label.substr(0, nl), tx, it.r.top + g.S(9), tw, g.body, kText);
                if (nl != std::wstring::npos) Text(dc, it.label.substr(nl + 1), tx, it.r.top + g.S(11) + th, tw, g.note, kMuted);
                break;
            }
            case Item::Link: {
                HFONT f = g.body;
                LOGFONTW lf;
                GetObjectW(f, sizeof(lf), &lf);
                lf.lfUnderline = hot;
                HFONT u = CreateFontIndirectW(&lf);
                Text(dc, it.label, it.r.left, it.r.top, it.r.right - it.r.left, u, hot ? kAccentHover : kAccent);
                DeleteObject(u);
                break;
            }
        }
    }
}

void Repaint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(g.hwnd, &ps);
    RECT client;
    GetClientRect(g.hwnd, &client);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
    auto old = SelectObject(mem, bmp);
    Paint(mem, client);
    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(g.hwnd, &ps);
}

int HitTest(int x, int y) {
    for (size_t i = 0; i < g.items.size(); i++)
        if (PtInRect(&g.items[i].r, POINT{x, y})) return int(i);
    return -1;
}

void Activate(int i) {
    if (i < 0 || i >= int(g.items.size())) return;
    Item it = g.items[i];
    if (it.kind == Item::Check) *it.value = !*it.value;
    else if (it.run) it.run();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void StartWork() {
    g.page = Page::Working;
    g.steps = 0;
    InvalidateRect(g.hwnd, nullptr, FALSE);
    HWND hwnd = g.hwnd;
    const Options o = g.options;
    const Mode mode = g.mode;
    g.worker = std::thread([hwnd, o, mode] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Progress p;
        p.step = [hwnd](const std::wstring& s) { PostMessageW(hwnd, WM_STEP, 0, LPARAM(new std::wstring(s))); };
        p.warning = [hwnd](const std::wstring& s) { PostMessageW(hwnd, WM_WARN, 0, LPARAM(new std::wstring(s))); };
        auto* error = new std::wstring;
        auto* log = new std::wstring;
        const int code = mode == Mode::Install ? Install(o, p, *error, *log) : Uninstall(o, p, *error, *log);
        PostMessageW(hwnd, WM_FINISHED, WPARAM(code), LPARAM(new std::pair<std::wstring, std::wstring>(*error, *log)));
        delete error;
        delete log;
    });
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: Repaint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: {
            const int hot = HitTest(short(LOWORD(lp)), short(HIWORD(lp)));
            if (hot != g.hot) {
                g.hot = hot;
                InvalidateRect(hwnd, nullptr, FALSE);
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g.hot = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, g.hot >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
            g.pressed = HitTest(short(LOWORD(lp)), short(HIWORD(lp)));
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const int at = HitTest(short(LOWORD(lp)), short(HIWORD(lp)));
            const int was = g.pressed;
            g.pressed = -1;
            if (at >= 0 && at == was) Activate(at);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            // Enter: the main button. Escape: cancel or close, unless it's busy.
            if (wp == VK_RETURN) {
                for (int i = int(g.items.size()) - 1; i >= 0; i--)
                    if (g.items[i].kind == Item::Button && g.items[i].style != 0) return Activate(i), 0;
            } else if (wp == VK_ESCAPE && g.page != Page::Working) {
                Close();
            }
            return 0;
        case WM_STEP: {
            auto* s = reinterpret_cast<std::wstring*>(lp);
            g.step = *s;
            g.steps++;
            delete s;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_WARN: {
            auto* s = reinterpret_cast<std::wstring*>(lp);
            g.warnings.push_back(*s);
            delete s;
            return 0;
        }
        case WM_FINISHED: {
            auto* r = reinterpret_cast<std::pair<std::wstring, std::wstring>*>(lp);
            g.error = r->first;
            g.log = r->second;
            delete r;
            if (g.worker.joinable()) g.worker.join();
            g.page = wp == 0 ? Page::Done : Page::Failed;
            if (g.page == Page::Done && g.mode == Mode::Install && g.openWhenDone)
                OpenAsUser((fs::path(g.options.dir) / L"RinCast Streamer.exe").wstring());
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CLOSE:
            if (g.page == Page::Working) return 0;  // half an install is worse than waiting
            break;
        case WM_DPICHANGED: {
            g.dpi = HIWORD(wp);
            MakeFonts();
            const RECT* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
            return 0;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int RunWindow(HINSTANCE inst) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RinCastSetup";
    RegisterClassExW(&wc);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, g.mode == Mode::Install ? L"RinCast Setup" : L"Uninstall RinCast", style,
                             CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, inst, nullptr);
    g.dpi = GetDpiForWindow(g.hwnd);
    MakeFonts();
    // A black title bar to go with the window.
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(g.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    const COLORREF black = RGB(0, 0, 0);
    DwmSetWindowAttribute(g.hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &black, sizeof(black));
    RECT r{0, 0, g.S(600), g.S(460)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, g.dpi);
    const int w = r.right - r.left, h = r.bottom - r.top;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    SetWindowPos(g.hwnd, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - h) / 2, w, h, SWP_NOZORDER);
    ShowWindow(g.hwnd, SW_SHOWNORMAL);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (g.worker.joinable()) g.worker.join();
    return g.page == Page::Failed ? 1 : 0;
}

// For developers: a page as a picture, without running anything. Pages: choose, update, working,
// done, failed, remove, removed.
bool Render(const std::wstring& page, const fs::path& out) {
    g.dpi = 144;
    MakeFonts();
    g.options.dir = L"C:\\Program Files\\RinCast";
    g.mode = page == L"remove" || page == L"removed" ? Mode::Uninstall : Mode::Install;
    g.updating = page == L"update";
    g.installedVersion = L"0.9.0";
    g.page = page == L"working" ? Page::Working : page == L"done" || page == L"removed" ? Page::Done
             : page == L"failed"                ? Page::Failed
                                                : Page::Choose;
    g.step = L"Adding firewall rules";
    g.steps = 3;
    g.error = L"C:\\Program Files\\RinCast already contains other files. Choose an empty folder.";
    g.log = L"log";
    const int w = g.S(600), h = g.S(460);
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto old = SelectObject(dc, bmp);
    Paint(dc, RECT{0, 0, w, h});
    GdiFlush();
    BITMAPFILEHEADER fh{0x4D42, DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + w * h * 4), 0, 0,
                        sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    f.write(reinterpret_cast<const char*>(&bi.bmiHeader), sizeof(bi.bmiHeader));
    f.write(static_cast<const char*>(bits), std::streamsize(w) * h * 4);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return bool(f);
}

// Runs this program again, elevated, with the same arguments.
int Elevate(const std::wstring& args) {
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    const std::wstring self = SelfPath();
    sei.lpFile = self.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return 1;  // declined
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return int(code);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);
    auto has = [&](const wchar_t* a) { return std::find(args.begin(), args.end(), a) != args.end(); };
    auto value = [&](const wchar_t* a) {
        auto it = std::find(args.begin(), args.end(), a);
        return it != args.end() && it + 1 != args.end() ? *(it + 1) : std::wstring();
    };

    if (args.size() == 3 && args[0] == L"--pack") return Pack(args[1], args[2]) ? 0 : 1;
    if (args.size() == 2 && args[0] == L"--extract") return HasPayload() && Extract(args[1]) ? 0 : 1;
    if (args.size() == 3 && args[0] == L"--render") return Render(args[1], args[2]) ? 0 : 1;

    const bool quiet = has(L"--quiet");
    const fs::path self = SelfPath();
    g.mode = has(L"--uninstall") || (!HasPayload() && self.filename().wstring().rfind(L"Uninstall", 0) == 0) ? Mode::Uninstall
                                                                                                            : Mode::Install;
    if (g.mode == Mode::Install && !HasPayload()) {
        MessageBoxW(nullptr, L"This is RinCast's setup program without RinCast in it. Download RinCast-Setup.exe again.",
                    L"RinCast Setup", MB_ICONERROR);
        return 1;
    }
    if (!IsElevated()) {
        std::wstring joined;
        for (const auto& a : args) joined += L"\"" + a + L"\" ";
        return Elevate(joined);
    }

    g.installedVersion = RegString(L"DisplayVersion");
    const std::wstring installedAt = RegString(L"InstallLocation");
    g.updating = g.mode == Mode::Install && !installedAt.empty();
    wchar_t pf[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, pf);
    g.options.dir = !installedAt.empty() ? installedAt : (fs::path(pf) / L"RinCast").wstring();
    if (!value(L"--dir").empty()) g.options.dir = value(L"--dir");
    g.options.allowPublic = has(L"--allow-public");
    g.options.keepSettings = has(L"--keep-settings");

    if (g.mode == Mode::Uninstall) {
        // The uninstaller lives in the folder it removes: run from a copy outside it.
        if (!has(L"--from-temp")) {
            wchar_t temp[MAX_PATH];
            GetTempPathW(MAX_PATH, temp);
            const fs::path copy = fs::path(temp) / L"Uninstall RinCast.exe";
            std::error_code ec;
            fs::copy_file(self, copy, fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                std::wstring cmd = L"\"" + copy.wstring() + L"\" --uninstall --from-temp --dir \"" + g.options.dir + L"\"";
                if (quiet) cmd += L" --quiet";
                if (g.options.keepSettings) cmd += L" --keep-settings";
                STARTUPINFOW si{sizeof(si)};
                PROCESS_INFORMATION pi{};
                if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                    WaitForSingleObject(pi.hProcess, quiet ? INFINITE : 0);
                    DWORD code = 0;
                    GetExitCodeProcess(pi.hProcess, &code);
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    return quiet ? int(code) : 0;
                }
            }
        } else {
            MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);  // the copy goes on the next restart
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (quiet) {
        Progress p{[](const std::wstring&) {}, [](const std::wstring&) {}};
        std::wstring error, log;
        return g.mode == Mode::Install ? Install(g.options, p, error, log) : Uninstall(g.options, p, error, log);
    }
    return RunWindow(inst);
}
