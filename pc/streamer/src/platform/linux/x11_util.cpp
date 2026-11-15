// X11 helpers for the capture, the monitor list and the input (x11_util.h).
#include "platform/linux/x11_util.h"

#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include "rc/platform.h"

namespace rcs::x11 {

namespace {

std::mutex g_errorsMutex;
std::map<Display*, int> g_errors;  // our connections and their last error
XErrorHandler g_previousHandler = nullptr;

// Xlib's default handler ends the process on any error, and a capture asking for an area that
// just went away (a monitor unplugged mid-frame) is an error. Errors on our own connections are
// recorded for the caller to see; the others go where they went before. GTK installs its own
// handler when it starts, which ignores errors on displays it didn't open: ours are then silent,
// which is fine, since every caller checks what its requests returned anyway.
int OnXError(Display* display, XErrorEvent* error) {
    {
        std::lock_guard lock(g_errorsMutex);
        if (auto it = g_errors.find(display); it != g_errors.end()) {
            it->second = error->error_code;
            return 0;
        }
    }
    return g_previousHandler ? g_previousHandler(display, error) : 0;
}

// Xlib is called from several threads here (each capture and the input have connections of their
// own, but the library's global state is shared), and the GTK window uses X too: XInitThreads
// must run before the first Xlib call of anyone, so it runs before main. libX11 1.8 does this by
// itself; older ones (still in some LTS releases) don't. The error handler goes in at the same
// time, before GTK's, so GTK's chains nothing of ours and ours chains Xlib's default.
__attribute__((constructor)) void InitXlib() {
    XInitThreads();
    g_previousHandler = XSetErrorHandler(OnXError);
}

// The monitor's name from its EDID (the 0xFC display descriptor), or "".
std::string EdidName(Display* display, RROutput output) {
    static const Atom edidAtom = XInternAtom(display, RR_PROPERTY_RANDR_EDID, False);
    Atom type = None;
    int format = 0;
    unsigned long items = 0, after = 0;
    unsigned char* data = nullptr;
    std::string name;
    if (XRRGetOutputProperty(display, output, edidAtom, 0, 128, False, False, AnyPropertyType, &type, &format, &items,
                             &after, &data) == Success &&
        data && format == 8 && items >= 128) {
        for (int offset = 54; offset <= 108; offset += 18) {
            const unsigned char* d = data + offset;
            if (d[0] || d[1] || d[2] || d[3] != 0xFC) continue;
            for (int i = 5; i < 18 && d[i] != 0x0A && d[i]; i++) name += char(d[i]);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            break;
        }
    }
    if (data) XFree(data);
    return name;
}

double RefreshOf(const XRRScreenResources* resources, RRMode mode) {
    for (int i = 0; i < resources->nmode; i++) {
        const XRRModeInfo& m = resources->modes[i];
        if (m.id != mode) continue;
        double vTotal = m.vTotal;
        if (m.modeFlags & RR_DoubleScan) vTotal *= 2;
        if (m.modeFlags & RR_Interlace) vTotal /= 2;
        if (m.hTotal && vTotal > 0) return double(m.dotClock) / (double(m.hTotal) * vTotal);
    }
    return 60;
}

}  // namespace

Display* Open() {
    Display* display = XOpenDisplay(nullptr);
    if (!display) return nullptr;
    std::lock_guard lock(g_errorsMutex);
    g_errors[display] = 0;
    return display;
}

void Close(Display* display) {
    if (!display) return;
    XCloseDisplay(display);
    std::lock_guard lock(g_errorsMutex);
    g_errors.erase(display);
}

int TakeError(Display* display) {
    std::lock_guard lock(g_errorsMutex);
    auto it = g_errors.find(display);
    if (it == g_errors.end()) return 0;
    return std::exchange(it->second, 0);
}

std::vector<MonitorInfo> Monitors(Display* display) {
    std::vector<MonitorInfo> monitors;
    const Window root = DefaultRootWindow(display);
    int eventBase = 0, errorBase = 0;
    XRRScreenResources* resources = nullptr;
    if (XRRQueryExtension(display, &eventBase, &errorBase)) resources = XRRGetScreenResourcesCurrent(display, root);
    if (!resources) {
        // No RandR (a nested or very old server): the whole screen is the one monitor.
        MonitorInfo m;
        m.id = "screen";
        m.name = "Screen";
        m.width = DisplayWidth(display, DefaultScreen(display));
        m.height = DisplayHeight(display, DefaultScreen(display));
        m.desktopRect = {0, 0, m.width, m.height};
        m.primary = true;
        monitors.push_back(m);
        return monitors;
    }
    const RROutput primary = XRRGetOutputPrimary(display, root);
    std::set<RRCrtc> crtcsSeen;  // outputs sharing a CRTC show the same picture: list it once
    for (int i = 0; i < resources->noutput; i++) {
        XRROutputInfo* output = XRRGetOutputInfo(display, resources, resources->outputs[i]);
        if (!output) continue;
        if (output->connection == RR_Connected && output->crtc && crtcsSeen.insert(output->crtc).second) {
            if (XRRCrtcInfo* crtc = XRRGetCrtcInfo(display, resources, output->crtc)) {
                if (crtc->mode && crtc->width && crtc->height) {
                    MonitorInfo m;
                    m.id = std::string(output->name, output->nameLen);
                    m.name = EdidName(display, resources->outputs[i]);
                    if (m.name.empty()) m.name = m.id;
                    m.desktopRect = {crtc->x, crtc->y, LONG(crtc->x + crtc->width), LONG(crtc->y + crtc->height)};
                    // The size as shown, as on Windows and macOS: the CRTC's area already has the
                    // rotation (and any scaling transform) applied, and it is what the root window
                    // holds, so it is what gets captured.
                    m.width = int(crtc->width);
                    m.height = int(crtc->height);
                    m.primary = resources->outputs[i] == primary;
                    m.refreshHz = RefreshOf(resources, crtc->mode);
                    switch (crtc->rotation & 0xF) {
                        case RR_Rotate_90: m.rotation = 90; break;
                        case RR_Rotate_180: m.rotation = 180; break;
                        case RR_Rotate_270: m.rotation = 270; break;
                        default: m.rotation = 0; break;
                    }
                    monitors.push_back(m);
                }
                XRRFreeCrtcInfo(crtc);
            }
        }
        XRRFreeOutputInfo(output);
    }
    XRRFreeScreenResources(resources);
    std::stable_sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.primary != b.primary) return a.primary;
        return a.desktopRect.left < b.desktopRect.left;
    });
    // X doesn't always have a primary output (none was ever set): the leftmost one is then.
    if (!monitors.empty() && !monitors[0].primary) monitors[0].primary = true;
    for (size_t i = 0; i < monitors.size(); i++) monitors[i].index = int(i);
    return monitors;
}

}  // namespace rcs::x11
