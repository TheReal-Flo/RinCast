// Mouse and keyboard from the headset on Wayland, where a client can't inject input into other
// clients. The RemoteDesktop portal (GNOME 46+, KDE Plasma 6.1+) hands us an EIS connection that
// libei sends emulated input through; the user allows it once (the restore token keeps it).
// Where there is no such portal, or until the user answered it, /dev/uinput creates virtual
// devices the compositor reads like real ones, if the user may write to it (a udev rule).
#include <fcntl.h>
#include <libei.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "platform/linux/linux_input.h"
#include "platform/linux/portal_linux.h"
#include "rc/platform.h"

namespace rcs::input {

namespace {

constexpr const char* kRemoteDesktop = "org.freedesktop.portal.RemoteDesktop";

// --- /dev/uinput ------------------------------------------------------------------------------------

class Uinput {
   public:
    ~Uinput() {
        for (int fd : {m_keyboard, m_pointer})
            if (fd >= 0) {
                ioctl(fd, UI_DEV_DESTROY);
                close(fd);
            }
    }

    // The devices, made on first use.
    bool Ready() {
        if (m_tried) return m_keyboard >= 0 && m_pointer >= 0;
        m_tried = true;
        m_keyboard = Create(false);
        m_pointer = m_keyboard >= 0 ? Create(true) : -1;
        if (m_keyboard < 0 || m_pointer < 0) {
            if (errno == EACCES || errno == EPERM)
                RC_LOGW("Input: /dev/uinput isn't writable, so mouse and keyboard from the headset can't work without the "
                        "RemoteDesktop portal. To allow it: echo 'KERNEL==\"uinput\", TAG+=\"uaccess\"' | sudo tee "
                        "/etc/udev/rules.d/70-rincast-uinput.rules && sudo udevadm control --reload && sudo udevadm trigger");
            else RC_LOGW("Input: can't create virtual input devices: %s", std::strerror(errno));
            return false;
        }
        return true;
    }

    // The pointer is absolute across the bounding box of all monitors (the compositor maps an
    // absolute device's range onto its whole desktop).
    void Move(const MonitorInfo& monitor, float u, float v) {
        const RECT& r = monitor.desktopRect;
        const int x = r.left + int(std::lround(u * float(r.right - r.left - 1))) - m_bounds.left;
        const int y = r.top + int(std::lround(v * float(r.bottom - r.top - 1))) - m_bounds.top;
        Emit(m_pointer, EV_ABS, ABS_X, std::clamp(x, 0, int(m_bounds.right - m_bounds.left - 1)));
        Emit(m_pointer, EV_ABS, ABS_Y, std::clamp(y, 0, int(m_bounds.bottom - m_bounds.top - 1)));
        Sync(m_pointer);
    }

    void Button(int code, bool down) {
        Emit(m_pointer, EV_KEY, code, down ? 1 : 0);
        Sync(m_pointer);
    }

    // High-resolution wheel events (120 per notch) for smooth scrolling, and the classic ones
    // per whole notch for applications that only read those.
    void Wheel(int32_t x120, int32_t y120) {
        if (y120) Emit(m_pointer, EV_REL, REL_WHEEL_HI_RES, y120);
        if (x120) Emit(m_pointer, EV_REL, REL_HWHEEL_HI_RES, x120);
        m_wheelX += x120;
        m_wheelY += y120;
        if (const int notches = m_wheelY / 120) {
            Emit(m_pointer, EV_REL, REL_WHEEL, notches);
            m_wheelY -= notches * 120;
        }
        if (const int notches = m_wheelX / 120) {
            Emit(m_pointer, EV_REL, REL_HWHEEL, notches);
            m_wheelX -= notches * 120;
        }
        Sync(m_pointer);
    }

    void Key(int code, bool down) {
        Emit(m_keyboard, EV_KEY, code, down ? 1 : 0);
        Sync(m_keyboard);
    }

    void SetBounds(const RECT& bounds) { m_bounds = bounds; }
    bool IsOpen() const { return m_keyboard >= 0 && m_pointer >= 0; }

   private:
    int Create(bool pointer) {
        const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return -1;
        uinput_setup setup{};
        setup.id.bustype = BUS_VIRTUAL;
        setup.id.vendor = 0x1209;  // pid.codes' open-source vendor id
        setup.id.product = pointer ? 0x0002 : 0x0001;
        setup.id.version = 1;
        std::snprintf(setup.name, sizeof(setup.name), "%s", pointer ? "RinCast pointer" : "RinCast keyboard");
        if (pointer) {
            ioctl(fd, UI_SET_EVBIT, EV_KEY);
            for (int b : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA}) ioctl(fd, UI_SET_KEYBIT, b);
            ioctl(fd, UI_SET_EVBIT, EV_REL);
            for (int r : {REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES}) ioctl(fd, UI_SET_RELBIT, r);
            ioctl(fd, UI_SET_EVBIT, EV_ABS);
            for (int axis : {ABS_X, ABS_Y}) {
                uinput_abs_setup abs{};
                abs.code = uint16_t(axis);
                abs.absinfo.minimum = 0;
                abs.absinfo.maximum = axis == ABS_X ? std::max(1, int(m_bounds.right - m_bounds.left - 1))
                                                    : std::max(1, int(m_bounds.bottom - m_bounds.top - 1));
                ioctl(fd, UI_ABS_SETUP, &abs);
            }
        } else {
            ioctl(fd, UI_SET_EVBIT, EV_KEY);
            ioctl(fd, UI_SET_EVBIT, EV_REP);
            for (int k = KEY_ESC; k <= KEY_MICMUTE; k++) ioctl(fd, UI_SET_KEYBIT, k);
        }
        if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
            const int error = errno;
            close(fd);
            errno = error;
            return -1;
        }
        return fd;
    }

    static void Emit(int fd, int type, int code, int value) {
        input_event event{};
        event.type = uint16_t(type);
        event.code = uint16_t(code);
        event.value = value;
        if (write(fd, &event, sizeof(event)) < 0) {
            // A full queue (nobody reading yet): the event is lost, as a real device's would be.
        }
    }
    static void Sync(int fd) { Emit(fd, EV_SYN, SYN_REPORT, 0); }

    bool m_tried = false;
    int m_keyboard = -1, m_pointer = -1;
    RECT m_bounds{0, 0, 1920, 1080};
    int32_t m_wheelX = 0, m_wheelY = 0;
};

// --- The RemoteDesktop portal and libei ------------------------------------------------------------

class WaylandInput : public InputBackend {
   public:
    // Everything here runs with InputMutex() held.
    void Prepare() override {
        if (m_state == State::Connecting || m_state == State::Ready) return;
        // After a failure (no portal, declined) it is asked again only when a stream starts.
        m_state = State::Connecting;
        std::thread([this] { Connect(); }).detach();
    }

    void Move(const MonitorInfo& monitor, float u, float v) override {
        Prepare();
        if (ei_device* device = Device(EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
            // libei's regions are the outputs in the compositor's logical space: the monitor's is
            // the one at its position (its size there may differ from ours under fractional scaling).
            const RECT& r = monitor.desktopRect;
            double x = r.left + u * double(r.right - r.left - 1), y = r.top + v * double(r.bottom - r.top - 1);
            ei_region* match = nullptr;
            for (size_t i = 0; ei_region* region = ei_device_get_region(device, i); i++) {
                if (int64_t(ei_region_get_x(region)) == r.left && int64_t(ei_region_get_y(region)) == r.top) match = region;
                if (!match && int(i) == monitor.index) match = region;
            }
            if (match) {
                x = ei_region_get_x(match) + u * double(ei_region_get_width(match) - 1);
                y = ei_region_get_y(match) + v * double(ei_region_get_height(match) - 1);
            }
            ei_device_pointer_motion_absolute(device, x, y);
            ei_device_frame(device, ei_now(m_ei));
            return;
        }
        if (UinputReady()) m_uinput.Move(monitor, u, v);
    }

    void Button(int evdevButton, bool down) override {
        Prepare();
        if (ei_device* device = Device(EI_DEVICE_CAP_BUTTON)) {
            ei_device_button_button(device, uint32_t(evdevButton), down);
            ei_device_frame(device, ei_now(m_ei));
        } else if (UinputReady()) {
            m_uinput.Button(evdevButton, down);
        }
    }

    void Wheel(int32_t x120, int32_t y120) override {
        Prepare();
        if (ei_device* device = Device(EI_DEVICE_CAP_SCROLL)) {
            // libei, like Wayland, scrolls down for positive y.
            ei_device_scroll_discrete(device, x120, -y120);
            ei_device_frame(device, ei_now(m_ei));
        } else if (UinputReady()) {
            m_uinput.Wheel(x120, y120);
        }
    }

    void Key(int evdevKey, uint16_t vk, bool down) override {
        Prepare();
        bool sent = false;
        if (ei_device* device = Device(EI_DEVICE_CAP_KEYBOARD)) {
            ei_device_keyboard_key(device, uint32_t(evdevKey), down);
            ei_device_frame(device, ei_now(m_ei));
            sent = true;
        }
        // A release also goes where the press may have gone (uinput while the portal was
        // still asking), so nothing stays held.
        if (!sent && UinputReady()) m_uinput.Key(evdevKey, down);
        else if (sent && !down && m_uinput.IsOpen()) m_uinput.Key(evdevKey, down);
    }

    // Typed through the keys of a US layout: the compositor's keymap (which libei hands over)
    // would need libxkbcommon to read. Characters a US keyboard lacks are skipped.
    void Text(const std::u32string& text) override {
        Prepare();
        bool skipped = false;
        for (char32_t c : text) {
            int key = 0;
            bool shift = false;
            if (!UsKeyFor(c, key, shift)) {
                skipped = true;
                continue;
            }
            if (shift) Key(KEY_LEFTSHIFT, 0, true);
            Key(key, 0, true);
            Key(key, 0, false);
            if (shift) Key(KEY_LEFTSHIFT, 0, false);
        }
        if (skipped && !m_saidText) {
            RC_LOGI("Input: on Wayland, text the US keyboard layout can't type is skipped");
            m_saidText = true;
        }
    }

    // Nothing to do: a Wayland compositor wakes its outputs on input, and a client can't ask it
    // to otherwise. The first mouse move from the headset does it.
    void Wake() override {}

   private:
    enum class State { Idle, Connecting, Ready, Failed };

    struct Device_ {
        ei_device* device;
        bool resumed;
    };

    bool UinputReady() {
        if (!m_boundsSet) {
            m_boundsSet = true;
            // The desktop's bounding box, for the absolute pointer's range.
            std::vector<MonitorInfo> monitors = EnumerateMonitors();
            if (!monitors.empty()) {
                RECT b = monitors[0].desktopRect;
                for (const MonitorInfo& m : monitors) {
                    b.left = std::min(b.left, m.desktopRect.left);
                    b.top = std::min(b.top, m.desktopRect.top);
                    b.right = std::max(b.right, m.desktopRect.right);
                    b.bottom = std::max(b.bottom, m.desktopRect.bottom);
                }
                m_uinput.SetBounds(b);
            }
        }
        return m_uinput.Ready();
    }

    // A device that emulates `capability` now, or null.
    ei_device* Device(ei_device_capability capability) {
        if (m_state != State::Ready) return nullptr;
        for (const Device_& d : m_devices)
            if (d.resumed && ei_device_has_capability(d.device, capability)) return d.device;
        return nullptr;
    }

    // On a thread of its own: the portal may show a dialog and wait for the user.
    void Connect() {
        const std::atomic<bool> running{true};
        const uint32_t version = portal::InterfaceVersion(kRemoteDesktop);
        if (version < 2) {
            // Version 2 brought ConnectToEIS.
            RC_LOGI("Input: no RemoteDesktop portal with libei (GNOME 46+, KDE Plasma 6.1+); using /dev/uinput");
            Finish(nullptr, State::Failed);
            return;
        }
        GVariant* results = nullptr;
        std::string token = portal::NewToken();
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        g_variant_builder_add(&b, "{sv}", "session_handle_token", g_variant_new_string(portal::NewToken().c_str()));
        std::string session;
        if (portal::Request(kRemoteDesktop, "CreateSession", g_variant_new("(a{sv})", &b), token, &results, running, 30000) == 0) {
            if (GVariant* v = g_variant_lookup_value(results, "session_handle", nullptr)) {
                if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH))
                    session = g_variant_get_string(v, nullptr);
                g_variant_unref(v);
            }
        }
        if (results) g_variant_unref(results);
        results = nullptr;
        if (session.empty()) {
            RC_LOGW("Input: the RemoteDesktop portal didn't create a session; using /dev/uinput");
            Finish(nullptr, State::Failed);
            return;
        }

        const std::string restore = portal::LoadToken("remote-desktop");
        token = portal::NewToken();
        g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(1 | 2));  // keyboard, pointer
        g_variant_builder_add(&b, "{sv}", "persist_mode", g_variant_new_uint32(2));
        if (!restore.empty()) g_variant_builder_add(&b, "{sv}", "restore_token", g_variant_new_string(restore.c_str()));
        int code = portal::Request(kRemoteDesktop, "SelectDevices", g_variant_new("(oa{sv})", session.c_str(), &b), token,
                                   &results, running, 30000);
        if (results) g_variant_unref(results);
        results = nullptr;
        if (code == 0) {
            if (restore.empty()) RC_LOGI("Input: allow remote interaction in the dialog on the PC");
            token = portal::NewToken();
            g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
            code = portal::Request(kRemoteDesktop, "Start", g_variant_new("(osa{sv})", session.c_str(), "", &b), token,
                                   &results, running, 5 * 60 * 1000);
            const char* newToken = nullptr;
            if (code == 0 && results && g_variant_lookup(results, "restore_token", "&s", &newToken) && newToken)
                portal::SaveToken("remote-desktop", newToken);
            else if (code == 2 && !restore.empty()) portal::SaveToken("remote-desktop", "");
            if (results) g_variant_unref(results);
        }
        if (code != 0) {
            RC_LOGW("Input: remote interaction wasn't allowed%s", code == 1 ? " (declined)" : "");
            portal::CloseSession(session);
            Finish(nullptr, State::Failed);
            return;
        }
        GVariantBuilder empty;
        g_variant_builder_init(&empty, G_VARIANT_TYPE_VARDICT);
        const int fd = portal::CallForFd(kRemoteDesktop, "ConnectToEIS", g_variant_new("(oa{sv})", session.c_str(), &empty));
        ei* context = fd >= 0 ? ei_new_sender(nullptr) : nullptr;
        if (context) {
            ei_configure_name(context, "RinCast");
            if (ei_setup_backend_fd(context, fd) != 0) context = ei_unref(context);  // the fd is libei's now either way
        } else if (fd >= 0) {
            close(fd);
        }
        if (!context) {
            RC_LOGW("Input: can't connect to the compositor's EIS; using /dev/uinput");
            portal::CloseSession(session);
            Finish(nullptr, State::Failed);
            return;
        }
        RC_LOGI("Input: through the RemoteDesktop portal (libei)");
        Finish(context, State::Ready);
        Run(context, session);
    }

    void Finish(ei* context, State state) {
        std::lock_guard lock(InputMutex());
        m_ei = context;
        m_state = state;
    }

    // Reads libei's events until the compositor ends the connection.
    void Run(ei* context, const std::string& session) {
        pollfd p{ei_get_fd(context), POLLIN, 0};
        bool connected = true;
        while (connected) {
            if (poll(&p, 1, 1000) < 0 && errno != EINTR) break;
            std::lock_guard lock(InputMutex());
            ei_dispatch(context);
            while (ei_event* event = ei_get_event(context)) {
                switch (ei_event_get_type(event)) {
                    case EI_EVENT_DISCONNECT: connected = false; break;
                    case EI_EVENT_SEAT_ADDED:
                        ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                                  EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, EI_DEVICE_CAP_KEYBOARD, nullptr);
                        break;
                    case EI_EVENT_DEVICE_ADDED: m_devices.push_back({ei_device_ref(ei_event_get_device(event)), false}); break;
                    case EI_EVENT_DEVICE_REMOVED: {
                        ei_device* device = ei_event_get_device(event);
                        for (auto it = m_devices.begin(); it != m_devices.end(); ++it)
                            if (it->device == device) {
                                ei_device_unref(it->device);
                                m_devices.erase(it);
                                break;
                            }
                        break;
                    }
                    case EI_EVENT_DEVICE_RESUMED:
                        for (Device_& d : m_devices)
                            if (d.device == ei_event_get_device(event)) {
                                d.resumed = true;
                                ei_device_start_emulating(d.device, ++m_sequence);
                            }
                        break;
                    case EI_EVENT_DEVICE_PAUSED:
                        for (Device_& d : m_devices)
                            if (d.device == ei_event_get_device(event)) d.resumed = false;
                        break;
                    default: break;
                }
                ei_event_unref(event);
            }
        }
        RC_LOGI("Input: the compositor ended remote interaction");
        std::lock_guard lock(InputMutex());
        for (const Device_& d : m_devices) ei_device_unref(d.device);
        m_devices.clear();
        m_ei = nullptr;
        ei_unref(context);
        m_state = State::Failed;
        portal::CloseSession(session);
    }

    State m_state = State::Idle;
    ei* m_ei = nullptr;
    std::vector<Device_> m_devices;
    uint32_t m_sequence = 0;
    Uinput m_uinput;
    bool m_boundsSet = false;
    bool m_saidText = false;
};

}  // namespace

std::unique_ptr<InputBackend> CreateWaylandInput() { return std::make_unique<WaylandInput>(); }

}  // namespace rcs::input
