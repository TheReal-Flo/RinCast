# RinCast Streamer for macOS

Streams your Mac's displays and sound to the RinCast headset app (Meta Quest, PICO), with the
headset's controllers as mouse and keyboard. Desktop only: there is no PC VR on macOS.

It is the same streamer as on Windows: the same pairing and approval of headsets, settings,
account, adaptive bitrate, loss recovery and frame pacing (`pc/streamer/src`). Only what touches
the system is macOS's own, in `pc/streamer/src/platform/mac`:

- **Capture:** ScreenCaptureKit, with the cursor drawn in, at each display's real pixels (a
  Retina display's backing resolution, so text stays sharp).
- **Encoding:** VideoToolbox in hardware, HEVC (10-bit where the Mac has it) or H.264, no
  B-frames, keyframes only when the headset needs one.
- **Sound:** ScreenCaptureKit's system audio (macOS 13 or later), as 48 kHz Opus like on
  Windows. The headset's microphone has no virtual device to go to on a Mac yet.
- **Mouse and keyboard:** CGEvents. The headset sends Windows key codes; they map to the Mac's key
  positions, so your own keyboard layout decides the characters.

`platform/posix` holds what macOS and Linux share (HTTP through libcurl, the terminal front end).

## Running it

Each release has `RinCast-<version>-macos-universal.zip`: one binary for Apple Silicon and Intel
Macs, this README and the licence texts.

```bash
unzip RinCast-<version>-macos-universal.zip
cd RinCast
./rincast-streamer
```

There's no window yet: the streamer runs in Terminal, and when a new headset wants to connect it
asks there:

```
Quest 3 (Meta Quest 3) at 192.168.1.42 wants to connect. Allow? [y = always, o = once, n = no]
```

Headsets you allow with `y` connect without asking next time. Settings and keys are in
`~/Library/Application Support/RinCast`.

The binary is **not code-signed or notarised** (that needs a paid Apple Developer ID). Browsers
mark downloads as quarantined, so macOS may refuse to start it until you clear that once:

```bash
xattr -d com.apple.quarantine rincast-streamer
```

### Permissions

On the first run macOS asks for two permissions, for Terminal (or whatever runs the streamer):

1. **Screen Recording** (System Settings > Privacy & Security > Screen Recording), to capture the
   displays. Start the streamer again after allowing it.
2. **Accessibility** (System Settings > Privacy & Security > Accessibility), for the headset's
   mouse and keyboard.

## Building

Requirements: macOS 12.3 or later (13 for sound), the Xcode Command Line Tools
(`xcode-select --install`) and CMake 3.22 or later.

```bash
scripts/build-mac.sh              # universal binary + dist/RinCast-<version>-macos-universal.zip
scripts/build-mac.sh --test       # also runs the shared core tests
```

or with CMake directly:

```bash
cmake -S pc/mac -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac --parallel
./build-mac/rincast-streamer
```

CI builds and packages it on a macOS runner for every push and pull request, without `--test`:
hosted runners drop IPv6 link-local multicast, which the core tests' discovery check needs, and
can't grant the Screen Recording and Accessibility consents. On a normal Mac `--test` passes.

## Connecting from the headset

1. The Mac and the headset on the same network (5 or 6 GHz Wi-Fi works best).
2. Open RinCast on the headset (on a Quest: under *Unknown Sources*). The Mac shows up in its list.
3. Select it, and allow the headset in the streamer's Terminal window.
