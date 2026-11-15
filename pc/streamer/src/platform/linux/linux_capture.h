// Desktop capture on Linux, behind one interface for desktop_stream_linux.cpp: X11 (XShm with
// XDamage for changes and XFixes for the cursor) or Wayland (the xdg-desktop-portal ScreenCast
// session and its PipeWire stream). Which one runs is the session's: WAYLAND_DISPLAY set means
// Wayland, where X11 would only see XWayland's windows.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "capture/monitors.h"
#include "platform/linux/ffmpeg_encoder.h"

namespace rcs {

// One captured picture: BGRA in memory, or a DMA-BUF image. It stays valid until Wait returns a
// newer one (or the capture stops), so the stream can encode it again when nothing changed.
struct CapturedPicture {
    uint32_t width = 0;
    uint32_t height = 0;
    const uint8_t* pixels = nullptr;  // BGRX / BGRA, or null when `dmabuf` is set
    int stride = 0;
    bool isDmaBuf = false;
    DmaBufFrame dmabuf;
    int64_t captureNs = 0;  // rc::NowNs when it arrived
};

class LinuxCapture {
   public:
    virtual ~LinuxCapture() = default;
    // Starts capturing `monitor` (with the cursor drawn in, as on Windows) at width x height.
    // May wait for the user (the portal's dialog on Wayland); gives up when `running` turns false.
    virtual bool Start(const MonitorInfo& monitor, uint32_t width, uint32_t height, float fps,
                       const std::atomic<bool>& running) = 0;
    // Waits up to timeoutNs for a picture newer than the last one. Returns:
    //   1: `out` holds a new picture;  0: nothing changed (`out` untouched);  -1: the capture ended (restart it).
    virtual int Wait(int64_t timeoutNs, CapturedPicture& out) = 0;
    virtual void Stop() = 0;
    // Where the cursor is on the monitor in the stream's pixels; -1, -1 when it's elsewhere or unknown.
    virtual void Cursor(int& x, int& y) = 0;
    // The encoder couldn't take a DMA-BUF picture: deliver pictures in memory from now on.
    virtual void PreferMemory() {}
};

// The capture this session uses (X11 or the Wayland portal).
std::unique_ptr<LinuxCapture> CreateCapture();

// True on a Wayland session (the portals do capture and input there). RINCAST_CAPTURE=x11 or
// =wayland overrides what the environment says.
bool IsWaylandSession();

std::unique_ptr<LinuxCapture> CreateX11Capture();
std::unique_ptr<LinuxCapture> CreateWaylandCapture();

}  // namespace rcs

namespace rcs::input {
// On Wayland, asks for the RemoteDesktop portal's input session now (a desktop stream starts),
// so its dialog comes up next to the screen-cast one while the user is at the PC, not on the
// first mouse move from the headset. Does nothing on X11.
void PrepareInput();
}  // namespace rcs::input
