// Monitors on Linux. On X11 they are RandR's outputs (x11_util.cpp). On Wayland a client can't
// capture an output it picks: the screen-cast portal shows the user a dialog and shares what they
// choose there. The list still comes from the compositor's wl_output globals, one monitor per
// output, so the headset offers the same choice of monitors as on Windows and sizes the streams
// right; each stream then has a portal session of its own, whose restore token is kept per
// monitor (capture_wayland.cpp), so the user picks the output for each of them once and the
// portal restores that choice from then on. When the compositor can't be asked, a single
// "Desktop" monitor stands for whatever the portal shares.
#include "capture/monitors.h"

#include <wayland-client.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "platform/linux/linux_capture.h"
#include "platform/linux/x11_util.h"
#include "rc/platform.h"

namespace rcs {

namespace {

struct WaylandOutput {
    wl_output* output = nullptr;
    uint32_t globalName = 0;
    std::string name;         // "DP-1" (version 4)
    std::string description;  // "Dell Inc. DELL U2720Q (DP-1)" (version 4)
    std::string makeModel;
    int32_t x = 0, y = 0;
    int32_t transform = WL_OUTPUT_TRANSFORM_NORMAL;
    int32_t modeWidth = 0, modeHeight = 0;
    int32_t refreshMhz = 0;
    int32_t scale = 1;
};

struct WaylandOutputs {
    std::vector<std::unique_ptr<WaylandOutput>> outputs;
};

void OnGeometry(void* data, wl_output*, int32_t x, int32_t y, int32_t, int32_t, int32_t, const char* make,
                const char* model, int32_t transform) {
    auto* o = static_cast<WaylandOutput*>(data);
    o->x = x;
    o->y = y;
    o->transform = transform;
    o->makeModel = std::string(make ? make : "") + (make && model && *make && *model ? " " : "") + (model ? model : "");
}

void OnMode(void* data, wl_output*, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    auto* o = static_cast<WaylandOutput*>(data);
    if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
    o->modeWidth = width;
    o->modeHeight = height;
    o->refreshMhz = refresh;
}

void OnDone(void*, wl_output*) {}

void OnScale(void* data, wl_output*, int32_t factor) { static_cast<WaylandOutput*>(data)->scale = std::max(1, factor); }

void OnName(void* data, wl_output*, const char* name) { static_cast<WaylandOutput*>(data)->name = name ? name : ""; }

void OnDescription(void* data, wl_output*, const char* description) {
    static_cast<WaylandOutput*>(data)->description = description ? description : "";
}

const wl_output_listener kOutputListener = {
    .geometry = OnGeometry,
    .mode = OnMode,
    .done = OnDone,
    .scale = OnScale,
    .name = OnName,
    .description = OnDescription,
};

void OnGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    if (std::strcmp(interface, wl_output_interface.name) != 0) return;
    auto* list = static_cast<WaylandOutputs*>(data);
    auto o = std::make_unique<WaylandOutput>();
    o->globalName = name;
    o->output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, std::min<uint32_t>(version, 4)));
    wl_output_add_listener(o->output, &kOutputListener, o.get());
    list->outputs.push_back(std::move(o));
}

void OnGlobalRemove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistryListener = {
    .global = OnGlobal,
    .global_remove = OnGlobalRemove,
};

// The outputs of the compositor, from a short-lived connection of our own.
std::vector<MonitorInfo> WaylandMonitors() {
    std::vector<MonitorInfo> monitors;
    wl_display* display = wl_display_connect(nullptr);
    if (!display) return monitors;
    WaylandOutputs list;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &kRegistryListener, &list);
    wl_display_roundtrip(display);  // the globals
    wl_display_roundtrip(display);  // each output's description
    int unnamed = 0;
    for (const auto& o : list.outputs) {
        if (o->modeWidth <= 0 || o->modeHeight <= 0) continue;
        MonitorInfo m;
        m.id = !o->name.empty() ? o->name : "output-" + std::to_string(++unnamed);
        m.name = !o->makeModel.empty() ? o->makeModel : !o->description.empty() ? o->description : m.id;
        // The mode is the panel's, unrotated; the picture the portal shares is as shown.
        const bool sideways = (o->transform & 1) != 0;  // 90 and 270, flipped or not
        m.width = sideways ? o->modeHeight : o->modeWidth;
        m.height = sideways ? o->modeWidth : o->modeHeight;
        switch (o->transform & 3) {
            case WL_OUTPUT_TRANSFORM_90: m.rotation = 90; break;
            case WL_OUTPUT_TRANSFORM_180: m.rotation = 180; break;
            case WL_OUTPUT_TRANSFORM_270: m.rotation = 270; break;
            default: m.rotation = 0; break;
        }
        if (o->refreshMhz > 0) m.refreshHz = o->refreshMhz / 1000.0;
        // The compositor's logical space, which libei's regions and the pointer use. wl_output
        // gives the integer scale only; libei's regions are matched by their position, so a
        // fractional scale doesn't matter there (input_wayland.cpp).
        m.desktopRect = {o->x, o->y, o->x + m.width / o->scale, o->y + m.height / o->scale};
        // Wayland has no primary output; the one at the origin is the closest thing.
        m.primary = o->x == 0 && o->y == 0;
        monitors.push_back(m);
    }
    for (const auto& o : list.outputs) {
        if (wl_output_get_version(o->output) >= WL_OUTPUT_RELEASE_SINCE_VERSION) wl_output_release(o->output);
        else wl_output_destroy(o->output);
    }
    wl_registry_destroy(registry);
    wl_display_disconnect(display);

    std::stable_sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.primary != b.primary) return a.primary;
        return a.desktopRect.left < b.desktopRect.left;
    });
    if (!monitors.empty()) monitors[0].primary = true;
    return monitors;
}

}  // namespace

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    if (IsWaylandSession()) {
        monitors = WaylandMonitors();
        if (monitors.empty()) {
            MonitorInfo m;
            m.id = "desktop";
            m.name = "Desktop";
            m.width = 1920;  // what the portal shares is scaled to this
            m.height = 1080;
            m.desktopRect = {0, 0, 1920, 1080};
            m.primary = true;
            monitors.push_back(m);
        }
    } else if (Display* display = x11::Open()) {
        monitors = x11::Monitors(display);
        x11::Close(display);
    } else {
        static bool said = false;
        if (!said) RC_LOGW("Monitors: can't open the X display (DISPLAY=%s)", getenv("DISPLAY") ? getenv("DISPLAY") : "");
        said = true;
    }
    for (size_t i = 0; i < monitors.size(); i++) monitors[i].index = int(i);
    return monitors;
}

}  // namespace rcs
