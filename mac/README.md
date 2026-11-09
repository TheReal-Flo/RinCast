# RinCast Streamer for macOS

Native macOS streamer implementation for RinCast, enabling multi-monitor desktop streaming and remote control to VR headsets (such as Meta Quest 3, Quest 3S, Quest Pro, Quest 2, and PICO 4).

## Features

- **ScreenCaptureKit:** High-performance screen capture with zero-copy GPU pipeline and native cursor rendering.
- **System Audio Streaming:** Real-time system audio capture via ScreenCaptureKit, encoded to stereo **Opus** at 48 kHz (low-delay, 10 ms frames) transmitted directly to the headset.
- **VideoToolbox Hardware Encoding:** Real-time hardware-accelerated encoding in **HEVC (H.265)** and **H.264** with minimal latency (no B-frames / reordering).
- **CoreGraphics Input Injection:** Full remote mouse control (cursor motion, left/right/middle buttons, smooth scroll) and keyboard text input from the headset to the Mac.
- **RinCast Protocol:** End-to-end encryption (Monocypher), forward error correction (XOR FEC), adaptive bitrate, and automatic LAN discovery.

---

## Release download

Each release ships `RinCast-<version>-macos-universal.zip`. It holds one binary with
both `arm64` and `x86_64` slices, so the same download runs on Apple Silicon and Intel
Macs, plus this README and the licence texts:

```bash
unzip RinCast-<version>-macos-universal.zip
cd RinCast
./rincast-streamer-mac
```

The binary is built with a macOS 12.3 deployment target but is **not code-signed or
notarised**: that would need a paid Apple Developer ID certificate, the Hardened
Runtime and `notarytool` credentials. Browsers mark downloaded files as quarantined,
so macOS may refuse to start the streamer. Clear the attribute once after unzipping:

```bash
xattr -d com.apple.quarantine rincast-streamer-mac
```

No certificate is needed for Screen Recording or Accessibility: those are runtime
consents the user grants to Terminal (see [Required macOS Permissions](#required-macos-permissions)),
not signing entitlements.

---

## Building from Source

Requirements:
- macOS 12.3+ (Apple Silicon or Intel). Desktop capture works from 12.3; **system
  audio capture needs macOS 13 (Ventura) or newer**, where ScreenCaptureKit gained
  audio output. On Monterey the streamer runs with video only.
- Xcode Command Line Tools (`xcode-select --install`)
- CMake 3.22+

To build:

```bash
mkdir -p build-mac
cd build-mac
cmake -S ../mac -B .
cmake --build .
```

The resulting executable will be located at:
`build-mac/rincast-streamer-mac`

### Packaged build and CI

`scripts/build-mac.sh` builds the universal binary with the same 12.3 deployment
target, runs the shared core tests with `--test`, and writes the release zip to `dist/`:

```bash
scripts/build-mac.sh --test
```

CI (`.github/workflows/ci.yml`) and the release workflow run this script on a macOS
runner. No Apple Developer certificate or secret is involved: the ScreenCaptureKit
headers come with the macOS SDK. A headless runner can't grant the TCC consents, so
CI covers build/link and the core tests, not capture or input injection.

---

## Usage

Run the streamer binary from your terminal:

```bash
./build-mac/rincast-streamer-mac
```

### Required macOS Permissions

On first run, macOS requires two permissions:

1. **Screen Recording:**
   * Path: **System Settings > Privacy & Security > Screen Recording**.
   * Toggle the switch to allow Terminal (or your app runner) to capture display frames.
2. **Accessibility:**
   * Path: **System Settings > Privacy & Security > Accessibility**.
   * Toggle the switch to allow remote mouse and keyboard events from the headset.

---

## Connecting from the Headset

1. Ensure your Mac and headset are connected to the same local Wi-Fi network (5 GHz or 6 GHz recommended).
2. Open the **RinCast** app on your headset (on Meta Quest, located under *Unknown Sources*).
3. Your Mac will appear automatically in the list of available PCs.
4. Select your Mac to connect: your displays will appear as floating screens in your immersive VR space!
