# Changelog

Notable changes to RinCast. Versions follow [Semantic Versioning](https://semver.org).

## 1.1.0 - 2026-11-07

New in the headset app and the PC streamer: install both. A 1.1.0 headset still works with an
older PC (it falls back to the resolution presets), but the PC's SteamVR driver and streamer
must match, which the installer takes care of.

Not tested on a headset yet: foveated encoding, matching SteamVR's resolution, the menus as
compositor layers, and the Galaxy XR, Quest 1, older-PICO and Steam Frame support.

### Added

- A language for the Streamer UI, under General > Language. It follows Windows' language out of
  the box, and English, 简体中文, 繁體中文, Deutsch, Español, Français and 日本語 can be picked directly.
  The choice is remembered across restarts. New languages only need a dictionary in
  `pc/streamer/ui/i18n.js`.
- A language for the headset's menus, under General > Interface > Language, with the same seven
  choices. It follows the headset's system language out of the box. The text atlas now also
  carries the Chinese and Japanese characters the menus use, baked from Noto Sans SC
  (`tools/assetbaker` reads `assets/ui/charset.txt`), and wrapped text breaks within Chinese and
  Japanese runs.
- The VR resolution is a slider now, from 50% to 130% of the headset's own in 5% steps, with
  the pixels per eye next to it. It replaces the five presets; older PCs get the nearest preset.
- "Match SteamVR's resolution" (VR quality): the stream takes the size SteamVR renders at, set
  in SteamVR under Video, up to 130% of the headset's own. When SteamVR's setting changes, or
  a game uses its own, the stream follows within a second or two. Turning it on or off asks
  for a SteamVR restart.
- Foveated encoding that follows the eyes (VR quality, on headsets that track them: Quest Pro,
  and others through OpenXR's eye gaze, such as the Galaxy XR). The PC keeps every pixel of a
  square around where you look, 45% of each eye's width and height, and half of them in each
  direction elsewhere; the headset stretches the frame back. That's about 53% of the pixels to
  encode, send and decode, on any GPU. The square only moves when the eyes look clearly
  elsewhere, and sits in each lens's centre while the eyes aren't tracked. NVENC's fixed
  foveation (fewer bits towards the lens edges) is off in that mode.
- Microphone volume on the headset (General), from 0 to 200%.
- A fixed play space, on by default (General > Tracking > "Fixed play space"): SteamVR's space
  sits on the headset's play area (the stage) instead of the floor where you last recentered,
  so recentering, by hand or by the headset, no longer moves it. Lighthouse trackers calibrated
  against the headset with Space Calibrator stay aligned. PICO headsets always worked this way.
  Turn it off to have SteamVR follow the headset's recentering again. The SteamVR driver also logs
  (in vrserver.txt) when the headset's position jumps, and the headset when its space recenters.
- The small in-game menu (double-press the left menu button, or hold A and B) also opens on the
  desktop while SteamVR runs, with "Back to VR" where it has "Back to desktop" in VR: switching
  either way is the same gesture and the same button.
- Support for the Samsung Galaxy XR, the original Quest (its own APK, `-quest1`), and PICO
  headsets on older firmware (PICO's own OpenXR loader).
- An experimental native app for the Steam Frame (see `headset/linux`).

### Fixed

- Windows' volume did nothing for the headset: the sound is recorded before Windows applies
  the speaker's volume, and the microphones' recording level wasn't applied either. RinCast now
  applies both itself, for RinCast's and Steam's virtual devices, so the volume slider, the
  volume keys and mute change the headset's sound, and turning the microphone down in Windows'
  sound settings works.
- Windows' default speaker and microphone could stay on Steam's streaming devices after RinCast.
  The streamer switches them while a headset is connected and only remembered the previous
  ones in memory, so when it was killed (by the installer during an update, a crash, Task
  Manager) they were never put back, and apps such as VRChat kept recording a microphone
  nothing was playing into. It now keeps them in a file and puts them back on the next start
  and when RinCast is uninstalled, and only where its device is still the default.
- SteamVR (and tools such as Space Calibrator) saw every headset as a Quest 3. It now sees the
  Quest, Quest 2, Quest Pro, Quest 3 and 3S by the names Quest Link gives them, and other
  headsets by their own; Quest 1 and 2 controllers get their own models. Takes effect after
  restarting SteamVR.
- Menus looked blurry next to the monitors: they were drawn into the eye images and so went
  through two resamplings. With "Sharp text" on (Desktop), they're compositor layers like the
  monitors.
- After closing the in-game menu with Resume, the game got no controller input until the menu
  was opened and closed again.
- The Steam Frame app didn't build since the language setting; it now follows the system's
  language (LANG) too.
- On the keyboard, pressing a key with one controller while the other pointed at another key
  didn't register.
- AV1 isn't offered on headsets that only have a software AV1 decoder (Quest 1 and 2), and the
  Quest 1 reports its decoder's real size limit.
- The Meta Store's requirement for the focus-aware manifest entry.

## 1.0.3 - 2026-11-07

### Fixed

- The USB cable connection: the PC turned the headset away with "Connections from outside your
  local network require signing in to a RinCast account", even when signed in. Over the cable
  the headset only has an IPv6 link-local address, and the PC only recognized local IPv4
  addresses. IPv6 link-local and private (unique local) addresses now count as local too.
  Public IPv6 addresses still count as the internet, and a new headset still has to be allowed
  on the PC.
- A second video stream on a Quest 2: a second monitor, or the game when VR starts. The headset
  asked its video decoders to run as fast as possible, and the Quest 2 then booked a single
  1080p stream at 960 frames per second, its entire decoding capacity, and refused every other
  stream ("MBPS limit exceeded"). Each decoder now asks for twice its stream's frame rate. Not
  yet tested on a Quest 2.

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
