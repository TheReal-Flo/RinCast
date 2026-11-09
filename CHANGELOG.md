# Changelog

Notable changes to RinCast. Versions follow [Semantic Versioning](https://semver.org).

## Unreleased

### Added

- Native macOS desktop streamer (`mac/`), capturing displays via ScreenCaptureKit,
  encoding video with VideoToolbox hardware acceleration (HEVC / H.264), streaming system
  audio via Opus, and supporting remote mouse and keyboard input.
- CI builds the macOS streamer and runs the core tests on every push and pull request.
  Release builds publish a universal (Apple Silicon + Intel)
  `RinCast-<version>-macos-universal.zip` alongside the Windows package, with its checksum
  in `SHA256SUMS.txt`.

### Fixed

- Caps Lock did nothing on the macOS streamer: the injected key-up cleared the AlphaShift
  flag, so macOS undid the toggle immediately. The streamer now keeps the latch, so Caps
  Lock stays on until it is pressed again.
- Punctuation and accent keys were dropped on the Mac, and a dropped Bluetooth key-up
  could leave Command latched and turn every letter into Cmd+letter. OEM virtual keys are
  now mapped, held keys are tracked and released, and injected events carry only the
  modifiers the streamer knows are down.
- The macOS release binary keeps its macOS 12.3 deployment target: ScreenCaptureKit's
  macOS 13 audio properties are now guarded, so video streaming still works on Monterey
  without the audio path calling an unavailable selector.

## 1.2.0 - 2026-11-09

Update both the headset app and the PC: the new resolution range and most fixes need both.

### Added

- Desktop: "Send mouse input" toggle under Desktop, allowing users to view PC monitors without passing laser pointer movements, clicks or scrolls to the computer.
- Holding both grips for half a second opens or closes the keyboard on the desktop, besides the
  dock's button.

### Changed

- The VR resolution counts from the headset's panel now: 100% is the panel's pixels per eye
  (1832x1920 on a Quest 2, 2064x2208 on a Quest 3), not the smaller size its runtime recommends.
  It goes up to 150%, Virtual Desktop's highest, from what was about the panel's own pixels;
  "Match SteamVR resolution" gives SteamVR the panel as its 100% and stops there too. A saved
  resolution keeps its pixels at the new percentage. The codec and the headset's decoder still
  bring it down where they can't take the size (H.264: 2048 wide per eye).
- A new keyboard: a port of [BetterFrame Keyboard](https://github.com/hardwaresteam/BetterFrameKeyboard),
  the Steam Frame keyboard, that looks and types the same.
  - Three layouts from the top bar: Minimal (big keys, like a phone), Standard (with F keys and
    arrows) and Full (with the editing keys and a number pad).
  - Undo, Cut, Copy, Paste and Select all in the top bar.
  - Swipe typing, word suggestions, and an emoji page with Recent, Favorites (hold an emoji) and
    skin tones.
  - 47 languages with their own keyboards, and QWERTY, QWERTZ, AZERTY, Dvorak, Colemak and
    Colemak-DH. Japanese types kana (by kana, romaji or flick), without kanji conversion; Korean
    and Vietnamese compose in the app being typed into; Chinese isn't there, as its input methods
    need BetterFrame's dictionaries.
  - Both hands at once: hold Shift with one and type with the other. Holding a key repeats it or
    types its capital; a double tap locks Shift, Ctrl, Alt and Fn.
  - Its own handles: the bar under it moves it, the corners resize and tilt it.
  - 10 colour themes, a see-through backdrop, a curved or flat shape, and two key sounds. They're
    under Desktop > Keyboard, with the language, key layout, top bar and bottom row.

  Not ported: voice typing, Steam's emoticons and Steam Controller typing. Not tested on a headset
  yet.

### Fixed

- Changing a video setting on the headset's desktop (every step of a slider) no longer pulls you
  into VR while SteamVR is running ([#10](https://github.com/TheReal-Flo/RinCast/issues/10)). The
  PC restarts the VR stream for such a change, and the headset took that for a newly started
  game. The restarted stream also stays paused while you're on the desktop.
- Leaving VR while its app keeps running no longer pulls you back in a moment later.
- Monitors turned to portrait now show on the headset's desktop. Their picture comes from
  Windows unrotated, and the PC turned every frame of one away as the wrong size. They are also
  turned the right way up now, and laid out as tall as the others are wide instead of 2.3 m tall.
- Games that change their render size as they go (dynamic resolution, or while loading) no
  longer freeze VR: the PC restarted the stream every couple of seconds to follow SteamVR's
  resolution, which also made the game's audio break up. It now follows a new size once it has
  held for 3 s, at most every 20 s.
- The headset also holds a high-performance Wi-Fi lock while streaming, for headsets where the
  low-latency one doesn't take effect.
- Frame generation no longer makes the picture wobble like jelly. Flat walls and repeated text
  were matched far away and taken for motion; now only motion the picture makes as a whole is
  used, a generated frame reaches as far as the stream's frame rate needs (1 to 2.5 frames),
  and it extrapolates a little less than asked.
- Menus are sharp from their first frame, instead of fading in soft and snapping sharp at the
  end.

## 1.1.2 - 2026-11-07

Only the Quest 1 APK (`-quest1`) changed; everything else is 1.1.1 with a new version number.

### Fixed

- The Quest 1 APK didn't start on a Quest 1. It listed the Quest 3 and 3S among its supported
  headsets, and the Quest 1's last Horizon OS, older than those, refuses an app with a headset
  name it doesn't know instead of ignoring it. It now lists only the Quest 1 and Quest 2. The
  other headsets use the standard APK.

## 1.1.1 - 2026-11-07

The 1.1.0 headset app doesn't start on any headset: update it. Install both, though; the other
fixes are in the headset app and in the PC's SteamVR driver and streamer. Apart from the first
fix, not tested on a headset yet.

### Fixed

- The 1.1.0 headset app didn't start at all: its OpenXR loader looked up the runtime's
  functions without the instance it had just created, so every lookup failed
  ([#5](https://github.com/TheReal-Flo/RinCast/pull/5)). Tested on a Quest 2 and a PICO 4.
- Chinese PICO 4s (model A8110, "Phoenix") got the standard OpenXR loader instead of PICO's own.
- VRChat took the controllers for Index controllers: fingers curled with the grip, a fist
  wasn't possible, and its Touch gestures didn't work ([#4](https://github.com/TheReal-Flo/RinCast/issues/4)).
  They said their fingers were partly measured; like Touch controllers, they now say the
  fingers are estimated from the buttons. With "Finger tracking with controllers", the real
  fingers still reach games that read SteamVR's skeleton, but not VRChat, which uses its own.
- Tracked hands are their own SteamVR devices ("RinCast Hand"), as VRChat recommends for
  streaming apps, instead of controllers with a hand's pose ([#2](https://github.com/TheReal-Flo/RinCast/issues/2)).
  Their fingers count as fully tracked, so VRChat shows them as they are and makes its gestures
  from them, fists included, and does its own pinches. RinCast's driver brings VRChat's bindings
  for them; other games get pinch as the trigger and a fist as the grip.
- Windows' default audio devices still weren't put back when Windows shut down or signed out
  while a headset was connected: Windows ends the streamer before it gets to. It now puts them
  back as Windows asks it to close.
- The headset kept recording its microphone while RinCast was in the background (the Quest's
  home, another app, the headset asleep) as long as the PC session lasted. A Quest Pro then
  gave the next app to use the microphone, such as Virtual Desktop, a much quieter one until
  the headset restarted. RinCast now lets go of it in the background and takes it again when
  it's back in front.
- The headset's video codec and frame-rate options didn't follow the language setting.

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
