# RinCast Streamer on Linux (plan)

Status: **in progress, not usable yet.** This is the design the Linux work follows: desktop
streaming on X11 and Wayland, and PC VR through a Monado-based runtime. The headset app needs
no changes: a Linux PC speaks the same protocol (docs/PROTOCOL.md).

## Shape

The Windows streamer stays as it is. The session layer (`session/client_session.cpp`,
`net/`, `settings.cpp`, `cloud/`, `stream/video_sender.cpp`) is shared; what sits under it gets
a Linux implementation behind the same interface:

| Part | Windows | Linux |
|---|---|---|
| Monitors | DXGI outputs | X11: RandR. Wayland: the screen-cast portal's streams |
| Desktop capture | DXGI Desktop Duplication | X11: XShm + XFixes (cursor), XDamage for changes. Wayland: xdg-desktop-portal ScreenCast + PipeWire, DMA-BUF when offered, cursor as metadata. The portal's restore token is kept so it asks once. |
| Encoder | NVENC / AMF / Media Foundation on D3D11 | FFmpeg (libavcodec): `*_vaapi` (AMD, Intel), `*_nvenc` (NVIDIA), software (libx264/libx265) as the last resort. Frames go in as DMA-BUF (VAAPI) or CUDA (NVENC) where possible, as system memory otherwise. |
| Mouse and keyboard | SendInput | X11: XTest. Wayland: the RemoteDesktop portal (libei; GNOME 46+, KDE 6.1+), with `/dev/uinput` as the fallback (needs a udev rule). Windows virtual-key codes from the headset map to evdev codes. |
| Audio | WASAPI loopback + virtual devices | PipeWire through the PulseAudio API: record the default sink's monitor; the headset microphone goes into a null sink whose monitor is offered as a source ("RinCast Microphone"). |
| Window | Win32, tray, WebView2 | GTK 3 + WebKitGTK 4.1 showing the same `ui/` (same message bridge), StatusNotifierItem tray via libayatana-appindicator. |
| Data folder | `%LOCALAPPDATA%\RinCast` | `$XDG_DATA_HOME/rincast` (`~/.local/share/rincast`) |
| Secrets at rest | DPAPI | libsecret when available, else a 0600 file |
| Firewall | Windows Firewall rules | none (a hint in the UI if `ufw`/`firewalld` is active) |
| HTTP, account WebSocket | WinHTTP | libcurl (WebSockets since 7.86) |
| Games list | Steam, Epic and others from the registry | Steam libraries from `~/.local/share/Steam`, launched with `steam steam://rungameid/…` |
| VR | SteamVR driver + RinCast OpenXR runtime, D3D11 shared textures | Monado-based OpenXR runtime (below) |

Platform files live in `pc/streamer/src/platform/linux/`; what Linux shares with macOS (libcurl
HTTP, the terminal front end, stubs for Windows-only services) is in `platform/posix/`, and macOS
has its own in `platform/mac/` (pc/mac/README.md). Headers used by the session layer carry no
Windows types (`win_compat.h` gives the few value types and atomics they use), and `util.h` keeps
the Windows-only helpers behind `_WIN32`.

What isn't about the system is written once for every OS: `DesktopPacer` (when a desktop frame
goes, and at what bitrate), `desktop_stream_common.cpp` (the stream's set-up and encoder settings)
and `PcAudioSender` (resampling to 48 kHz and Opus packets). A platform's desktop stream only
captures and encodes; its audio only captures.

## PC VR: Monado

SteamVR on Linux can't hand frames to a driver, so VR runs through Monado, the open OpenXR
runtime, the way WiVRn does: RinCast's Monado service (`pc/monado`, see its README) is Monado's
own service and OpenXR runtime, built from Monado's sources at a pinned version, with RinCast's
devices (the headset and Touch controllers, from the bridge's tracking) and a compositor target
of its own in place of Monado's hardware drivers and display output. The same service runs on
Windows as a choice next to SteamVR.

The streamer and the service share `pc/common/vr_bridge.h`'s layout in POSIX shared memory
(`/rincast-vr-bridge.v1`), with futex counters for the events. The frame images are Vulkan images
the streamer allocates on the GPU it encodes on (linear where that GPU can render to them) and
passes to the service over a Unix socket (`$XDG_RUNTIME_DIR/rincast-vr.sock`,
`pc/common/vr_bridge_images.h`). The service renders each frame into one, timed to the vsync the
streamer steers to the headset's display (the same phase steering as on Windows). The streamer
encodes it as a DMA-BUF with VAAPI, or reads it back for NVENC and software encoding (importing
it into CUDA instead is a later step). SteamVR games run through xrizer or OpenComposite on top.

The streamer starts the service when a headset launches VR and makes it the user's active OpenXR
runtime when asked (`~/.config/openxr/1/active_runtime.json`).

## Order

1. **Done.** Neutral session headers; the Linux build of the shared layer with the session,
   server, settings and cloud (`pc/linux/CMakeLists.txt`).
2. **Written, not yet run on a Linux desktop.** Desktop: X11 capture, FFmpeg encoder, XTest
   input, Pulse audio and the headset microphone, the GTK/WebKit window.
3. **Written, not yet run.** Desktop on Wayland: portal ScreenCast + PipeWire, RemoteDesktop/libei
   input (`/dev/uinput` as the fallback).
4. **Written, not yet run.** VR: the bridge on POSIX, RinCast's Monado service (`pc/monado`), the
   streamer's VR stream from the shared Vulkan images.
5. Packaging (a tarball with an install script, or an AppImage; Flatpak later, its sandbox needs
   portals for everything anyway).

## Building (WSL or a Linux PC, Ubuntu 24.04+ / Debian 13 package names)

```sh
sudo apt install build-essential cmake ninja-build pkg-config git \
  libavcodec-dev libavutil-dev libswscale-dev libva-dev libdrm-dev libgbm-dev libegl-dev \
  libpipewire-0.3-dev libglib2.0-dev libx11-dev libxext-dev libxfixes-dev libxtst-dev \
  libxrandr-dev libxdamage-dev libei-dev libwayland-dev wayland-protocols libpulse-dev \
  libgtk-3-dev libwebkit2gtk-4.1-dev libayatana-appindicator3-dev libsecret-1-dev \
  libvulkan-dev glslang-tools libeigen3-dev libusb-1.0-0-dev libudev-dev libhidapi-dev \
  libbsd-dev libcjson-dev libsystemd-dev libdbus-1-dev libx11-xcb-dev libxcb-randr0-dev \
  libxxf86vm-dev libcurl4-openssl-dev python3
cmake -S pc/linux -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux
```

Tested so far: builds under WSL only. Nothing has run on a real Linux desktop or GPU yet.
