# Changelog

Notable changes to RinCast. Versions follow [Semantic Versioning](https://semver.org).

## 1.0.2 - 2026-11-07

### Fixed

- PC VR on a Quest 2: the PC sent the game, but the headset never showed it and gave up after
  100 seconds. While VR started, the desktop stream kept running, and its decoder stayed open
  even in VR. A Quest 2 doesn't have the video decoding hardware for both at once. Starting VR
  now stops the desktop stream and frees its decoders first; leaving VR starts the desktop again.
  This also gives the game the desktop's share of the bandwidth while it starts. Not yet tested
  on a Quest 2.
- When the PC sends the game but the headset can't show it, the headset now says so after
  15 seconds and suggests H.264 or a lower VR quality. Before, it waited 100 seconds and blamed
  SteamVR.
- The headset's log says why a picture is missing: whether the video decoder started, why it
  couldn't, when it decoded its first frame, and why it dropped frames.

## 1.0.1 - 2026-11-07

### New

- RinCast Streamer can stop headsets from using the mouse and keyboard: turn off **Let headsets
  use the mouse and keyboard** under **Headsets**. The headset still shows your screens, and
  it's told when the PC turns this off or on. VR games still get the controllers.

### Fixed

- When PC VR starts but no picture reaches the headset, the logs now say why. The SteamVR
  driver writes to SteamVR's `vrserver.txt` when SteamVR creates its frame textures, when the
  first frame goes to the streamer, and why any frame was dropped. Every 10 seconds without a
  picture, it also says how far SteamVR got. RinCast Streamer's log says when the first frame
  arrived, why it dropped any, and what the hand-over looked like while nothing came through.
- The headset app shows the right version number. It was always 1.0.0 before.

## 1.0.0 - 2026-11-06

The first public release of RinCast: a free, open-source way to use your Windows PC from a
Meta Quest 3 or a PICO 4, for your desktop and for PC VR games through SteamVR.

### Installing

1. On the PC, run `RinCast-1.0.0-Setup.exe`. It asks for administrator rights once. (Or unpack
   `RinCast-1.0.0-windows-x64.zip` and run `install.ps1`.)
2. On the headset, install `RinCast-1.0.0.apk` with `adb install -r RinCast-1.0.0.apk` or
   SideQuest. The Quest needs developer mode; a PICO needs USB debugging.
3. Open RinCast on the headset, choose your PC, and select **Allow** on the PC.

`SHA256SUMS.txt` lists the checksums of every file. The `rincast-server` files are only for
running your own account server; you don't need them to use RinCast.

### Desktop

- Every monitor as its own floating screen, curved or flat, that you can move and resize.
- The controllers work as a mouse, with a virtual keyboard, and hand tracking can click and
  scroll.
- Sharp text through the headset's compositor layers.

### PC VR

- SteamVR sees a RinCast headset and Touch controllers, with haptics. Steam VR games can be
  launched from the headset.
- H.264, HEVC and AV1, hardware-encoded with NVENC, AMF or Media Foundation, with adaptive
  bitrate and fast recovery from lost packets.
- Foveated encoding on NVIDIA GPUs: fewer bits at the lens edges, more in the middle.
- A performance overlay with the latency split by stage, from the in-VR menu.

### Audio

- PC audio on the headset, and the headset's microphone on the PC, through Steam's streaming
  devices by default. See "Audio" in the README.

### Connecting

- PCs on the same network appear by themselves; no account needed.
- An optional account connects you from other networks, always directly between headset and
  PC. You can run your own account server.
- VPNs such as Tailscale or ZeroTier work without an account.
- Every packet is encrypted, and a PC only accepts headsets it has allowed, on every path.
- The headset reconnects by itself after a dropped connection, and explains what went wrong.

### The headset app

- Procedural skies and passthrough, and background music while no PC is connected.
- Accessibility: interface size, high contrast, sounds, read aloud and a one-handed route to
  the menu. The streamer works with keyboard, screen readers and Windows high contrast.

### Built, but not tested on a headset yet

These are in this release, mostly off by default, and haven't been tried on real hardware.
Reports are welcome.

- Everything on PICO 4 and PICO 4 Ultra, including PICO Motion Trackers as SteamVR trackers and
  PICO face tracking.
- Frame generation, like Synchronous Spacewarp, and upscaling on the headset.
- Body tracking as SteamVR trackers, and hand and finger tracking in PC games.
- Face and eye tracking shared with VRCFaceTracking through RinCast's module.
- A wired connection over a USB-C cable (Horizon OS 2.5 or later).

### Known limitations

- Windows only for now. A Linux streamer is planned.
- The files aren't code-signed yet, so Windows SmartScreen may warn about them.
- RinCast's own virtual audio driver isn't included, because Windows needs it signed by
  Microsoft. Steam's streaming devices are used instead.
- OpenXR games run through SteamVR; RinCast's own OpenXR runtime isn't in this release.
