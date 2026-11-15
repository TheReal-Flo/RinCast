// Desktop capture on X11: the monitor's area of the root window through MIT-SHM (one copy, into
// memory shared with the X server), only when XDamage says something there changed or the cursor
// moved. The cursor is a hardware sprite on X, not part of the root window's pixels, so its image
// (XFixes) is drawn into the picture, as Desktop Duplication does on Windows.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <sys/ipc.h>
#include <sys/select.h>
#include <sys/shm.h>

#include <algorithm>
#include <cstring>

#include "platform/linux/bgra_scale.h"
#include "platform/linux/linux_capture.h"
#include "platform/linux/x11_util.h"
#include "rc/platform.h"

namespace rcs {

namespace {

bool Intersects(const XRectangle& r, const RECT& m) {
    return r.x < m.right && r.x + r.width > m.left && r.y < m.bottom && r.y + r.height > m.top;
}

class X11Capture : public LinuxCapture {
   public:
    ~X11Capture() override { Stop(); }

    bool Start(const MonitorInfo& monitor, uint32_t width, uint32_t height, float fps,
               const std::atomic<bool>& running) override {
        Stop();
        m_display = x11::Open();
        if (!m_display) {
            RC_LOGW("X11 capture: can't open the display");
            return false;
        }
        m_root = DefaultRootWindow(m_display);
        m_outputId = monitor.id;
        if (!FindRect(m_rect)) {
            RC_LOGW("X11 capture: monitor %s isn't there", monitor.id.c_str());
            Stop();
            return false;
        }
        m_width = width;
        m_height = height;
        m_interval = int64_t(1e9 / std::max(fps, 1.f));
        if (!CreateImage()) {
            Stop();
            return false;
        }

        int errorBase = 0;
        if (XDamageQueryExtension(m_display, &m_damageEvent, &errorBase))
            m_damage = XDamageCreate(m_display, m_root, XDamageReportNonEmpty);
        else RC_LOGW("X11 capture: no XDamage; capturing every frame");
        int major = 0, minor = 0;
        if (XFixesQueryExtension(m_display, &m_fixesEvent, &errorBase) && XFixesQueryVersion(m_display, &major, &minor) &&
            major >= 2) {
            m_haveFixes = true;
            XFixesSelectCursorInput(m_display, m_root, XFixesDisplayCursorNotifyMask);
        } else {
            RC_LOGW("X11 capture: no XFixes 2; the cursor isn't drawn");
        }
        if (XRRQueryExtension(m_display, &m_randrEvent, &errorBase))
            XRRSelectInput(m_display, m_root, RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
        XSync(m_display, False);
        if (x11::TakeError(m_display)) {
            RC_LOGW("X11 capture: setting up failed");
            Stop();
            return false;
        }
        m_dirty = true;
        m_lost = false;
        m_cursorShapeChanged = true;
        RC_LOGI("X11 capture: %s at %d,%d %dx%d%s", m_outputId.c_str(), m_rect.left, m_rect.top,
                m_rect.right - m_rect.left, m_rect.bottom - m_rect.top, m_shmImage ? "" : " (no MIT-SHM, slower)");
        return true;
    }

    int Wait(int64_t timeoutNs, CapturedPicture& out) override {
        if (!m_display) return -1;
        const int64_t deadline = rc::NowNs() + timeoutNs;
        for (;;) {
            ProcessEvents();
            PollCursor();
            if (m_lost) return -1;
            const int64_t now = rc::NowNs();
            const bool due = now - m_lastCapture >= m_interval;
            if ((m_dirty || !m_damage) && due) {
                if (!Capture()) return -1;
                Fill(out);
                return 1;
            }
            if (now >= deadline) return 0;
            // Until the next capture may happen, or the next look at the cursor (which moves
            // without damaging anything), whichever comes first.
            int64_t next = m_dirty ? m_lastCapture + m_interval : now + m_interval;
            next = std::min(next, deadline);
            WaitForEvents(std::max<int64_t>(next - now, 500'000));
        }
    }

    void Stop() override {
        if (!m_display) return;
        if (m_damage) XDamageDestroy(m_display, m_damage);
        m_damage = 0;
        DestroyImage();
        x11::Close(m_display);
        m_display = nullptr;
        m_haveFixes = false;
        m_cursorPixels.clear();
    }

    void Cursor(int& x, int& y) override {
        x = m_cursorOutX;
        y = m_cursorOutY;
    }

   private:
    // The monitor's current area on the root window.
    bool FindRect(RECT& rect) {
        for (const MonitorInfo& m : x11::Monitors(m_display))
            if (m.id == m_outputId) {
                rect = m.desktopRect;
                return true;
            }
        return false;
    }

    int RectWidth() const { return m_rect.right - m_rect.left; }
    int RectHeight() const { return m_rect.bottom - m_rect.top; }

    bool CreateImage() {
        const int screen = DefaultScreen(m_display);
        Visual* visual = DefaultVisual(m_display, screen);
        const int depth = DefaultDepth(m_display, screen);
        if (depth < 24) {
            RC_LOGW("X11 capture: the screen's depth is %d bits; only 24 and 32 are supported", depth);
            return false;
        }
        if (XShmQueryExtension(m_display)) {
            m_shmImage = XShmCreateImage(m_display, visual, depth, ZPixmap, nullptr, &m_shm, RectWidth(), RectHeight());
            if (m_shmImage) {
                m_shm.shmid = shmget(IPC_PRIVATE, size_t(m_shmImage->bytes_per_line) * m_shmImage->height, IPC_CREAT | 0600);
                if (m_shm.shmid >= 0) {
                    void* address = shmat(m_shm.shmid, nullptr, 0);
                    if (address != reinterpret_cast<void*>(-1)) m_shm.shmaddr = m_shmImage->data = static_cast<char*>(address);
                    m_shm.readOnly = False;
                    if (m_shm.shmaddr && XShmAttach(m_display, &m_shm)) {
                        m_attached = true;
                        XSync(m_display, False);
                        if (!x11::TakeError(m_display) && m_shmImage->bits_per_pixel == 32) {
                            // Marked for removal now that the server has it: freed once both sides
                            // let go, even if we crash.
                            shmctl(m_shm.shmid, IPC_RMID, nullptr);
                            m_shm.shmid = -1;
                            return true;
                        }
                    }
                }
            }
            // A remote X server, or one that won't share memory with us: plain XGetImage.
            DestroyImage();
        }
        return true;
    }

    void DestroyImage() {
        if (m_attached) XShmDetach(m_display, &m_shm);
        m_attached = false;
        if (m_shmImage) {
            m_shmImage->data = nullptr;  // ours (shmat), not Xlib's to free
            XDestroyImage(m_shmImage);
            m_shmImage = nullptr;
        }
        if (m_shm.shmaddr) shmdt(m_shm.shmaddr);
        if (m_shm.shmid >= 0) shmctl(m_shm.shmid, IPC_RMID, nullptr);
        m_shm = {};
        m_shm.shmid = -1;
    }

    void WaitForEvents(int64_t ns) {
        if (XPending(m_display)) return;
        const int fd = ConnectionNumber(m_display);
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeval tv{time_t(ns / 1'000'000'000), suseconds_t((ns % 1'000'000'000) / 1000)};
        select(fd + 1, &set, nullptr, nullptr, &tv);
    }

    void ProcessEvents() {
        while (XPending(m_display)) {
            XEvent event;
            XNextEvent(m_display, &event);
            if (m_damage && event.type == m_damageEvent + XDamageNotify) {
                // Take the damage so far, and look whether any of it is on this monitor.
                XserverRegion region = XFixesCreateRegion(m_display, nullptr, 0);
                XDamageSubtract(m_display, m_damage, None, region);
                int count = 0;
                if (XRectangle* rects = XFixesFetchRegion(m_display, region, &count)) {
                    for (int i = 0; i < count && !m_dirty; i++)
                        if (Intersects(rects[i], m_rect)) m_dirty = true;
                    XFree(rects);
                }
                XFixesDestroyRegion(m_display, region);
            } else if (m_haveFixes && event.type == m_fixesEvent + XFixesCursorNotify) {
                m_cursorShapeChanged = true;
                if (m_cursorOutX >= 0) m_dirty = true;
            } else if (event.type == m_randrEvent + RRScreenChangeNotify || event.type == m_randrEvent + RRNotify) {
                XRRUpdateConfiguration(&event);
                // A mode change, a rotation, a monitor moved or gone: start over with its new area.
                RECT rect{};
                if (!FindRect(rect) || std::memcmp(&rect, &m_rect, sizeof(rect)) != 0) {
                    RC_LOGI("X11 capture: %s changed; restarting", m_outputId.c_str());
                    m_lost = true;
                }
            }
        }
    }

    // The cursor moves without damage: a move on (or off) this monitor needs a new picture.
    void PollCursor() {
        if (!m_haveFixes) return;
        Window rootReturn, child;
        int rootX = 0, rootY = 0, winX = 0, winY = 0;
        unsigned mask = 0;
        if (!XQueryPointer(m_display, m_root, &rootReturn, &child, &rootX, &rootY, &winX, &winY, &mask)) {
            rootX = rootY = -1000000;  // on another screen
        }
        if (rootX == m_cursorRootX && rootY == m_cursorRootY) return;
        const bool wasHere = m_cursorOutX >= 0;
        m_cursorRootX = rootX;
        m_cursorRootY = rootY;
        const bool here = rootX >= m_rect.left && rootX < m_rect.right && rootY >= m_rect.top && rootY < m_rect.bottom;
        if (here) {
            m_cursorOutX = int(int64_t(rootX - m_rect.left) * m_width / RectWidth());
            m_cursorOutY = int(int64_t(rootY - m_rect.top) * m_height / RectHeight());
        } else {
            m_cursorOutX = m_cursorOutY = -1;
        }
        // Near the edge the cursor image can overlap this monitor while its hotspot is elsewhere:
        // that small miss isn't worth a capture per move.
        if (here || wasHere) m_dirty = true;
    }

    bool Capture() {
        uint8_t* pixels = nullptr;
        int stride = 0;
        if (m_shmImage) {
            if (!XShmGetImage(m_display, m_root, m_shmImage, m_rect.left, m_rect.top, AllPlanes) ||
                x11::TakeError(m_display)) {
                RC_LOGW("X11 capture: reading %s failed", m_outputId.c_str());
                m_lost = true;
                return false;
            }
            pixels = reinterpret_cast<uint8_t*>(m_shmImage->data);
            stride = m_shmImage->bytes_per_line;
        } else {
            XImage* image = XGetImage(m_display, m_root, m_rect.left, m_rect.top, RectWidth(), RectHeight(), AllPlanes, ZPixmap);
            if (!image || x11::TakeError(m_display) || image->bits_per_pixel != 32) {
                if (image) XDestroyImage(image);
                RC_LOGW("X11 capture: reading %s failed", m_outputId.c_str());
                m_lost = true;
                return false;
            }
            stride = RectWidth() * 4;
            m_copy.resize(size_t(stride) * RectHeight());
            for (int y = 0; y < RectHeight(); y++)
                std::memcpy(m_copy.data() + size_t(y) * stride, image->data + size_t(y) * image->bytes_per_line, stride);
            XDestroyImage(image);
            pixels = m_copy.data();
        }
        m_lastCapture = rc::NowNs();
        m_dirty = false;

        if (m_haveFixes && m_cursorOutX >= 0) {
            if (m_cursorShapeChanged) FetchCursor();
            if (!m_cursorPixels.empty())
                DrawCursor(pixels, RectWidth(), RectHeight(), stride, m_cursorPixels.data(), m_cursorWidth, m_cursorHeight,
                           m_cursorWidth, m_cursorRootX - m_cursorHotX - m_rect.left,
                           m_cursorRootY - m_cursorHotY - m_rect.top);
        }

        if (uint32_t(RectWidth()) == m_width && uint32_t(RectHeight()) == m_height) {
            m_pixels = pixels;
            m_stride = stride;
        } else {
            ScaleBgra(pixels, RectWidth(), RectHeight(), stride, m_scaled, int(m_width), int(m_height));
            m_pixels = m_scaled.data();
            m_stride = int(m_width) * 4;
        }
        return true;
    }

    void FetchCursor() {
        m_cursorShapeChanged = false;
        XFixesCursorImage* image = XFixesGetCursorImage(m_display);
        if (!image) return;
        m_cursorWidth = image->width;
        m_cursorHeight = image->height;
        m_cursorHotX = image->xhot;
        m_cursorHotY = image->yhot;
        // `pixels` are unsigned longs (64 bits on 64-bit systems), one ARGB value in each.
        m_cursorPixels.resize(size_t(image->width) * image->height);
        for (size_t i = 0; i < m_cursorPixels.size(); i++) m_cursorPixels[i] = uint32_t(image->pixels[i]);
        XFree(image);
    }

    void Fill(CapturedPicture& out) {
        out = {};
        out.width = m_width;
        out.height = m_height;
        out.pixels = m_pixels;
        out.stride = m_stride;
        out.captureNs = m_lastCapture;
    }

    Display* m_display = nullptr;
    Window m_root = 0;
    std::string m_outputId;
    RECT m_rect{};
    uint32_t m_width = 0, m_height = 0;
    int64_t m_interval = 16'666'666;

    XShmSegmentInfo m_shm{0, -1, nullptr, False};
    XImage* m_shmImage = nullptr;
    bool m_attached = false;
    std::vector<uint8_t> m_copy;    // without MIT-SHM
    std::vector<uint8_t> m_scaled;  // when the stream's size differs
    const uint8_t* m_pixels = nullptr;
    int m_stride = 0;

    Damage m_damage = 0;
    int m_damageEvent = 0, m_fixesEvent = 0, m_randrEvent = 0;
    bool m_haveFixes = false;
    bool m_dirty = true;
    bool m_lost = false;
    int64_t m_lastCapture = 0;

    int m_cursorRootX = -1000000, m_cursorRootY = -1000000;
    int m_cursorOutX = -1, m_cursorOutY = -1;
    bool m_cursorShapeChanged = true;
    std::vector<uint32_t> m_cursorPixels;
    int m_cursorWidth = 0, m_cursorHeight = 0, m_cursorHotX = 0, m_cursorHotY = 0;
};

}  // namespace

std::unique_ptr<LinuxCapture> CreateX11Capture() { return std::make_unique<X11Capture>(); }

}  // namespace rcs
