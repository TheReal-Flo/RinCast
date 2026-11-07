# Building RinCast

`scripts\build-all.ps1` builds everything. The per-component steps are below.

## Requirements

| Component | Needs |
|---|---|
| PC (streamer, SteamVR driver, tools) | Visual Studio 2022 with the C++ desktop workload, CMake ≥ 3.22 |
| Audio driver | the above plus the WDK. `scripts\fetch-deps.ps1` fetches it as a NuGet package, so no WDK install is required. |
| Headset app | Android Studio (bundled JBR 17+), Android SDK 34, NDK 27.3.13750724, CMake 3.22.1 |
| Server | Go ≥ 1.26 (see `server/go.mod`) |
| Asset tools (optional) | Node.js ≥ 18 |

Everything else is vendored in `third_party/`.

## PC

```powershell
cmake -S pc -B build\pc -G "Visual Studio 17 2022" -A x64
cmake --build build\pc --config Release
```
The output goes to `build\pc\bin\Release`:
- `RinCast Streamer.exe`
- `driver\rincast\…`: the SteamVR driver layout
- `rincast-audio-setup.exe`
- the developer tools

You can run the streamer straight from that folder. It finds the driver
relative to its own path.

The version comes from the `VERSION` file in the repository root. Configuring
stamps it into `pc/streamer/src/version.h`, and `install.ps1` and Gradle read it.

### RinCast XR (not in this release)
RinCast XR, the OpenXR runtime in `pc/runtime-openxr`, and its test app
`tools/xr-smoke` aren't in the public repository or in release builds, because
they depend on Meta LibOVR and Microsoft GameInput, which can't be redistributed
here. If you have a local copy, build it with `-DRINCAST_BUILD_OPENXR=ON`
(`build-all.ps1 -OpenXR`). The output goes to `build\pc\bin\Release\openxr`.

### Developer tools
- **headset-sim.** Connects to a running streamer like a headset would: it handshakes, asks for desktop or VR, and writes decoded frame statistics and dumps. Use it to test the PC side without a Quest.
  `--drop-every N` throws away every Nth frame, as if lost on the network. The tool then recovers the way the headset does, counts how each loss was resolved (recovery frame or keyframe), and writes exactly what a decoder would have been given, so you can check the output with `ffmpeg -v error -i stream1.h265 -f null -`.
  - `--vr` also asks the streamer to launch SteamVR, and at the end prints the PC side of the VR latency: tracking sample → game frame, encode, and send.
  - `--limit-mbps N` sends the stream through a simulated link of N Mbit/s with a large buffer, like a slow upload. The progress lines show the bitrate that arrives and the queueing delay, so you can watch the bitrate control.
  - `--move` moves the right controller on a known path for `pose-probe`, and `--hz` sets the refresh rate.
  - `--press-system S` presses the system button S seconds after VR started, to close the dashboard SteamVR opens by itself (while it is open, SteamVR shows its own scene instead of the game).
- **pose-probe.** Checks the controller timing in SteamVR. Run it next to `headset-sim --vr --move`.
  - `pose-probe [seconds]` prints how far ahead of real time SteamVR's controller pose is.
  - `pose-probe --scene [seconds] [log] [busy ms]` runs as a VR app (optionally spending `busy ms` per frame like a heavy game) and logs which head pose and controller pose each frame got (and whether the dashboard was open), to compare with `headset-sim`'s `samples.txt` and `vr-frames.txt` (the pose each streamed frame was tagged with).
- **capture-test.** Captures each monitor, runs the colour conversion and saves PNGs.
- **encode-bench.** Measures the streamer's encoder on this GPU: encode time and frame sizes, with a panning picture taken from the first monitor. It can also simulate a loss and the recovery from it.
  ```powershell
  encode-bench --codec hevc|av1|h264 [--size 3664x1920] [--fps 90] [--mbps 150] [--frames 300]
               [--desktop] [--no-split] [--foveated] [--ten-bit] [--no-aq] [--two-pass] [--one-pass]
               [--loss <frame> [--lost <count>]]
               [--out <bitstream>] [--yuv <nv12 input>]
  ```
  Decode `--out` with ffmpeg (`-f obu` for AV1) to check it. `--yuv` writes the encoder input, for `psnr` comparisons.
- **assetbaker.** Bakes the SDF text and icon atlases used by the headset UI (`headset/app/src/main/assets/ui`).
  An optional fifth argument is a charset file: every character in it that Inter doesn't cover is baked
  from Noto Sans SC in the same fonts dir. The charset (`assets/ui/charset.txt`) holds the non-ASCII
  characters the UI translations use, so the atlas carries exactly the Chinese and Japanese glyphs the
  menus need. Regenerate it after adding translated strings to `app/strings.cpp`:
  ```powershell
  assetbaker third_party\fonts third_party\fonts\MaterialSymbolsRounded.ttf `
             third_party\fonts\MaterialSymbolsRounded.codepoints `
             headset\app\src\main\assets\ui headset\app\src\main\assets\ui\charset.txt
  ```
- **headset-sim --interfaces** lists which network adapters and IPv6 addresses the PC offers for remote connections.
- **portmap-test [port]** asks the router for a UDP port mapping the way the streamer does (PCP, then NAT-PMP, then UPnP), prints the result or the reason it isn't possible, and removes the mapping again.
- **remote-e2e** tests a complete remote (account) connection on one machine: a local server, the real streamer signed in to a throwaway account, and the headset app's real `StreamClient` compiled for Windows. Close RinCast Streamer first (the tool restores your settings afterwards), start a local server, then run:
  ```powershell
  rincast-server -http 127.0.0.1:8099 -udp :38721 -data .\e2e-data -public-url http://127.0.0.1:8099 -rendezvous-host localhost
  remote-e2e http://127.0.0.1:8099 "C:\path\to\RinCast Streamer.exe" [--only-ipv6 | --rendezvous-only]
  ```
  `--only-ipv6` only allows the PC's global IPv6 addresses; `--rendezvous-only` only uses the addresses learned through the rendezvous.
- **bridge-grab** saves the newest VR frame from the bridge as a PNG, with its pose and field of view.
- **audio-check** lists the virtual audio devices.
  - `--mic-test steam|rincast` sends a tone through the streamer's microphone path (Opus packets into the same player the headset feeds) and measures the pitch, the delay and any gaps on the side apps record from.
  - `--formats steam|rincast` lists the formats both sides of the virtual microphone accept.
  - `--set-format steam|rincast RATE CH` and `--restore-format steam|rincast` change both sides' default format and put the old ones back.

## Audio driver

```powershell
scripts\fetch-deps.ps1
cmake -S pc\audio-driver -B build\audio -A x64 -DWDK_ROOT=<deps>\packages\Microsoft.Windows.WDK.x64.<ver>\c
cmake --build build\audio --config Release
```
`build-all.ps1` also creates the driver package in `build\audio\package` with Inf2Cat.

Windows only loads kernel drivers with a valid signature, so `build-all.ps1`
leaves an unsigned driver out of the PC package:
- **Development machines.** Run `build-all.ps1 -TestSign` (or `scripts\audio\test-sign.ps1`). Then run `bcdedit /set testsigning on` (elevated, Secure Boot off) and reboot. `install.ps1` trusts the test certificate, and `uninstall.ps1` removes it again.
- **Distribution.** Submit `build\audio\package` for attestation signing in the Microsoft Partner Center, then run `build-all.ps1 -SignedAudio <folder with the signed files>`. It checks for Microsoft's signature.

`build-all.ps1 -Package` refuses test-signed files and any test certificate.

To install, remove or check the driver:
`rincast-audio-setup.exe install|remove|status [RinCastAudio.inf]`. It must run
elevated. `install.ps1` calls it when the package has a driver. If the install
fails, the tool removes the device node it created.

### Audio modes
The streamer's **Audio devices** setting has these options; **Automatic** uses
RinCast's devices if they're installed, otherwise Steam's.

| Mode | Availability | Microphone |
|---|---|---|
| Steam Streaming Speakers & Microphone | Default for releases; needs Steam's devices | Yes |
| RinCast Speaker & Microphone | Only with a signed driver package (or test signing) | Yes |
| Default devices | Always | No |

**Enabling Steam's devices.** They come with Steam, not with RinCast:
1. Install Steam and sign in.
2. In Steam, open **Settings → Remote Play** and turn on Remote Play.
3. Stream a game once from this PC to another device with Steam Link or Remote Play. Steam adds the devices when it first needs them.
4. Check for **Steam Streaming Speakers** and **Steam Streaming Microphone** in Windows sound settings, including under **All sound devices**, and allow them if they're marked as not allowed. The streamer's **Audio** tab shows whether it found them.

Newer versions of Steam may not add the devices on every PC. If they don't
appear, use **Default devices**: you still get PC audio, but no microphone.

## Headset app

```powershell
$env:JAVA_HOME = "$env:ProgramFiles\Android\Android Studio\jbr"
cd headset
.\gradlew assembleDebug        # or assembleRelease
adb install -r app\build\outputs\apk\standard\debug\app-standard-debug.apk
```
There are two builds of the app. `standard` is the one for the store and every headset but the
Quest 1. `quest1` (`app\build\outputs\apk\quest1\...`) also carries Meta's own OpenXR loader, which the
Quest 1's last Horizon OS needs, from Meta's OpenXR Mobile SDK 64 (downloaded into `deps/meta`,
43 MB, on its first build), and lists the Quest 1 among its devices. Both have the same package
name and version, so either installs over the other.

The native code lives in `app/src/main/cpp` and pulls in `shared/`. The APK carries two
OpenXR loaders, and the app opens one when it starts (`app/src/main/cpp/xr/xr_loader.cpp`):
Khronos' standard loader, from Maven (`org.khronos.openxr:openxr_loader_for_android`), and
PICO's own, which PICO headsets from before PICO OS 5.9 need. The first build downloads PICO's
OpenXR SDK into `deps/pico` (7 MB, checked against a fixed SHA-256) for it. The same APK runs on
Quest and PICO: the manifest carries both's entries, and the code for each is in
`app/src/main/cpp/headsets` (see [ARCHITECTURE.md](ARCHITECTURE.md#headset-app-headsetappsrcmaincpp)).

### Release signing
Debug builds use the usual debug key. Release builds read their key from the
environment and stop with a clear error if any variable is missing:

| Variable | Value |
|---|---|
| `RINCAST_KEYSTORE` | Path to the keystore file |
| `RINCAST_KEYSTORE_PASSWORD` | Keystore password |
| `RINCAST_KEY_ALIAS` | Key alias |
| `RINCAST_KEY_PASSWORD` | Key password |

To create a key once (`keytool` is in the Android Studio JBR):
```powershell
keytool -genkeypair -v -keystore rincast-release.keystore -alias rincast -keyalg RSA -keysize 4096 -validity 10000
```
Keep the keystore and passwords outside the repository and back them up. Android
only accepts an update signed with the same key, so losing it means users must
uninstall the app to get new versions.

For the release workflow, add the repository secrets `RINCAST_KEYSTORE_BASE64`
(from `[Convert]::ToBase64String([IO.File]::ReadAllBytes('rincast-release.keystore'))`),
`RINCAST_KEYSTORE_PASSWORD`, `RINCAST_KEY_ALIAS` and `RINCAST_KEY_PASSWORD`.
Paste them on GitHub, or set them with `gh secret set` from Git Bash
(`base64 -w0 rincast-release.keystore | gh secret set RINCAST_KEYSTORE_BASE64`). Don't pipe
them to `gh` from Windows PowerShell: it re-encodes the text on the way, and the secret no
longer matches.

### Cleartext HTTP
A self-hosted server on a LAN is often plain `http://192.168.x.y`. Android's
network security config can only allow cleartext by exact host name or domain
suffix, not by IP range, so it can't say "private addresses only". Cleartext
therefore stays allowed in `res/xml/network_security_config.xml`. The default
server uses HTTPS.

### Visual testing without a Quest (AXRB)
This test needs a local RinCast XR build (see [RinCast XR](#rincast-xr-not-in-this-release)), which isn't in the public repository.

`tools/axrb/` runs the APK in [AXRB](https://github.com/Android-XR-Bridge/AXRB) (Android XR on Windows; the
emulator backend, since the app renders with GLES) and presents it through **RinCast XR** instead of
SteamVR, so no headset is required. The rendered eye buffers travel the normal VR path:

    app (emulator) → AXRB host → RinCast XR → VR bridge → RinCast Streamer (NVENC) → headset-sim → .h265

1. Start `RinCast Streamer.exe --minimized` with `sim-device` in its trusted devices.
2. `tools\axrb\ui-tour.ps1 [-Only home,settings-audio]` boots AXRB, starts `headset-sim --vr --still`,
   steps through the windows and records a timestamp per state.
3. `node tools\axrb\extract-shots.mjs` decodes one side-by-side PNG per state into `build\axrb-test\shots`.

The tour drives the UI through debug properties that the app polls once per second:
`debug.rincast.ui` (comma-separated windows to show), `debug.rincast.tab` (settings tab) and
`debug.rincast.pc` (an extra address to send discovery probes to; the emulator can't receive LAN
broadcasts, so the tour uses `10.0.2.2`, the emulator's host). The app only renders a few frames per
second in the emulator; that is the emulator's GLES readback, not the app.

### Regenerating assets
```powershell
build\pc\bin\Release\assetbaker.exe                          # → assets/ui/text|icons.png/json
node tools\gen-icons.mjs                                     # → assets/brand/* (logo, tray icon, app icon)
```

## Server

```sh
cd server
go test ./...
go run . -public-url http://<lan-ip>:8080 -rendezvous-host <lan-ip>
```
Point the streamer's **Account → Server** setting and the headset setting
(`cloudServer` in `settings.json`) at that URL. `server/Dockerfile` builds a
deployable image.

## Installing on a PC

`dist\RinCast-<version>-Setup.exe` is the installer most people use: the package folder packed
into RinCast's setup program (`pc/installer/setup`), with a window for the folder and the
options. It runs `install.ps1` hidden, and installs `Uninstall RinCast.exe` for **Installed apps**.
Both take `--quiet` for unattended use; setup also takes `--dir <folder>` and
`--allow-public`, the uninstaller `--keep-settings`. For working on it, `--render <page>
<file.bmp>` draws a page without running anything, and `--extract <folder>` unpacks the files.

Without the setup program, run `dist\RinCast\install.ps1`. It asks for administrator rights if it needs them, then:
1. Copies the files to `%ProgramFiles%\RinCast`, or to an empty folder given with `-InstallDir`.
2. Adds firewall rules for UDP 38710 and 38711 and for the streamer executable, on Private and Domain networks. Public networks open only when allowed in the streamer (General, Networks) or with `-AllowPublic`; a reinstall keeps that choice.
3. Registers the SteamVR driver.
4. Installs the audio driver, if the package has one.
5. Adds a Start menu entry and an entry in Apps & features.

`uninstall.ps1` reverses all of it. It only deletes a custom install folder if
the folder contains the `.rincast-install` marker the installer leaves there.

## Making a release

1. Set the new version in `VERSION`, and add a `## <version> - <date>` section to `CHANGELOG.md`.
   That section becomes the release notes. Push to `main`.
2. On GitHub, open **Actions > Release > Run workflow** on `main`. Only the repository's owner
   can run it. It builds everything, tags the commit `v<version>` and publishes the release.
   Tick **draft** to check it on GitHub before publishing.

The workflow stops before building if the tag already exists, if `CHANGELOG.md` has no section
for the version or still says "unreleased", or if the APK signing secrets are missing (unless
**Allow a debug-signed APK** is ticked). The Android version code comes from `VERSION` too.

`.github/workflows/ci.yml` builds and tests every commit on every branch and every pull request.

To build the same files locally, run `build-all.ps1 -Package`. It writes
`RinCast-<version>-windows-x64.zip`, `RinCast-<version>.apk`, the versioned server
binaries and `SHA256SUMS.txt` to `dist`. It needs the signing variables above, or
`-DebugApk` for a test build.
