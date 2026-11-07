# RinCast

RinCast is a free, open-source way to use your Windows PC from a Meta Quest 3 or a PICO 4.
See and control all of your monitors in an immersive space, and play PC VR games
through SteamVR. There's no subscription and no store purchase.

- **Desktop:** every monitor as its own floating, curved or flat screen, controlled with the controllers and a virtual keyboard.
- **PC VR:** SteamVR sees a headset and Touch controllers, and you can launch your Steam VR games from the headset. OpenXR games run through SteamVR; a separate OpenXR runtime is not in this release.
- **Video:** hardware-encoded H.264, HEVC or AV1 with NVENC, AMF or Media Foundation.
- **Private:** every packet is encrypted, and stream traffic always goes directly between your headset and your PC.
- **No account needed at home:** PCs on the same network appear automatically. An optional account lets you connect from other networks.

## Requirements

- Windows 10 or 11, 64-bit, with an NVIDIA, AMD or Intel GPU that can encode video.
- [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) (already included with Windows 11 and most Windows 10 PCs).
- Meta Quest 3, Quest 3S, Quest Pro or Quest 2, with [developer mode](https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/) on to install the app.
  PICO 4 and PICO 4 Ultra work with the same app (not tested on a PICO yet): turn on USB debugging
  under **Settings > General > About** (tap **Software Version** seven times), then
  **Developer > USB Debug**.
  The Quest 1 needs its own build, `RinCast-<version>-quest1.apk` (not tested on a Quest 1 yet).
  The Samsung Galaxy XR runs the standard build (not tested on one yet): turn on developer options
  and USB debugging, and allow hand tracking when RinCast asks.
  The Steam Frame has an experimental native app, built from source for now: see
  [headset/linux](headset/linux/README.md).
- For VR games: Steam and SteamVR.
- A good network: the PC on Ethernet and the headset on 5 GHz or 6 GHz Wi-Fi works best.

## Quick start

1. Download `RinCast-<version>-Setup.exe` and `RinCast-<version>.apk` from the [releases page](../../releases).
2. Run `RinCast-<version>-Setup.exe`. Windows asks for administrator rights. (Without it: unpack
   `RinCast-<version>-windows-x64.zip`, right-click `install.ps1` and choose **Run with PowerShell**.)
3. Install the app on the headset: `adb install -r RinCast-<version>.apk`, or use a sideloading tool such as SideQuest.
4. On the headset, open **RinCast** from your apps (on the Quest under **Unknown Sources**).
5. Choose your PC from the list, and select **Allow** on the PC the first time.

The installer opens the firewall for Private and Domain networks. If Windows has your
network set as Public, RinCast Streamer's Home page says so: either set the network to
Private in Windows, or turn on **Allow headsets on Public networks** under General. On a
Public network anyone nearby can then see this PC in RinCast and ask to connect; a new
headset still has to be allowed on the PC. To remove RinCast, use **Settings > Apps >
Installed apps** in Windows.

The release isn't code-signed yet, so Windows SmartScreen may warn you about the
files.

## Audio

RinCast streams PC audio to the headset, and can send the headset's microphone to
the PC. It needs virtual audio devices for the full experience. The **Audio
devices** setting in the streamer has three modes:

| Mode | When | What you get |
|---|---|---|
| **Steam's streaming devices** | Default for releases | PC audio and the microphone, through "Steam Streaming Speakers" and "Steam Streaming Microphone". |
| **RinCast's own driver** | Only with a signed driver package | The same, through "RinCast Speaker" and "RinCast Microphone". |
| **Default devices** | No virtual devices needed | PC audio from your normal playback device, without a microphone. |

Releases don't include the RinCast driver because Windows won't load a kernel
driver without Microsoft's signature. Steam's devices come with Steam; see
[how to enable them](docs/BUILDING.md#audio-modes).

## Connecting with a USB cable

On Horizon OS 2.5 or later, a USB-C cable can take the place of Wi-Fi: no wireless
interference, and a USB 3 cable carries more than any Wi-Fi.

1. On the Quest, turn on **General > USB cable**, and plug the headset into the PC.
2. The headset asks once whether RinCast may use the cable. Allow it.
3. The PC appears under **PCs** as "USB cable". Choose it.

Windows treats the cable as an unknown network, which it sets as Public. RinCast Streamer's
Home page then offers **Allow the USB cable**, which opens the firewall for that one cable and
nothing else. Windows 11 recognizes the headset by itself; Windows 10 may not.

## PICO Motion Trackers

On a PICO, **VR quality > Body tracking** sends PICO Motion Trackers to SteamVR as trackers, the
way Quest body tracking does. Pair the trackers in PICO's Motion Tracker app first. The trackers
don't tell where they're worn, so RinCast decides the first time it sees each one: on the floor a
foot, at knee height a knee, higher up the waist or the chest. Turn **Body tracking** on while
standing up straight; turn it off and on again after moving a tracker. Where the headset can
estimate the whole body from the trackers instead, the setting does that, and **Calibrate** opens
PICO's calibration.

## Connecting through a VPN, without an account

If both devices are on the same VPN, such as Tailscale, ZeroTier or WireGuard, you don't need
an account:

1. Turn the VPN on, on the PC and on the Quest.
2. RinCast Streamer's **Home** page lists the PC's VPN address.
3. On the Quest, choose **Enter PC address** and type it in. The PC then appears under **PCs**.

When the local network and the VPN both reach the PC, RinCast uses the local network. If
Windows has the VPN's network set as Public, turn on **Allow headsets on Public networks**
under **General**, or set it to Private.

## Connecting from other networks

Sign in to the same RinCast account in the streamer (**Account** tab) and on the
headset. The headset shows a short code that you confirm on your PC or phone.
The server only introduces your devices: the stream still goes directly between
them, over IPv6, a hole-punched connection or a router port mapping.

You can run your own server. See [server/README.md](server/README.md).

## Not in RinCast yet

Compared with [Virtual Desktop](https://www.vrdesktop.net/), these are still missing.
[docs/FEATURES.md](docs/FEATURES.md) lists what RinCast already does.

**Built, but not tested on a headset yet** (off by default where they're settings)

- Frame generation, like Synchronous Spacewarp (SSW)
- Upscaling on the headset, like Snapdragon Game Super Resolution
- Body tracking as SteamVR trackers, and PICO Motion Trackers
- Hand and finger tracking in PC games
- Face and eye tracking for VRCFaceTracking
- Foveated encoding that follows the eyes
- Matching SteamVR's resolution
- The USB cable connection
- Everything on PICO headsets, the Quest 1, the Galaxy XR and the Steam Frame

**Picture and smoothness**

- A sharpening slider (sharpening is automatic for now)
- Fixed foveated encoding on AMD and Intel GPUs (it's NVENC only for now)
- A very high bitrate H.264 mode, like H.264+
- Gamma, vibrance and video range controls

**Tracking**

- A stage tracking option for mixed setups with real trackers

**Games**

- An OpenXR runtime that doesn't need SteamVR
- Oculus Rift games
- Launching non-Steam games from the headset
- Xbox gamepad emulation

**Passthrough**

- Passthrough over a running VR game, with adjustable opacity
- Hands-only passthrough, a desk portal and tracked keyboards

**Video and desktop**

- 3D side-by-side video on the desktop screen
- Playing local video files on the headset
- Keyboard shortcuts on the PC
- Streaming a single window or app instead of a whole monitor

**Platforms**

- Headsets other than Meta Quest, PICO, the Samsung Galaxy XR and the Steam Frame
- A ready-made Steam Frame download (it's built from source for now)
- Streaming from Linux. Desktop streaming is planned first; PC VR on Linux would go through
  a Monado-based runtime, the way WiVRn does it, because SteamVR on Linux can't hand frames to
  a driver.
- Streaming from macOS

## Building from source

See [docs/BUILDING.md](docs/BUILDING.md). In short:

```powershell
scripts\build-all.ps1           # builds everything into dist\
```

## Documentation

- [docs/FEATURES.md](docs/FEATURES.md): what's implemented and what isn't.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how the pieces fit together.
- [docs/PROTOCOL.md](docs/PROTOCOL.md): wire protocol.
- [docs/BUILDING.md](docs/BUILDING.md): toolchains, building, testing, signing and releasing.
- [server/README.md](server/README.md): running the account server.
- [CONTRIBUTING.md](CONTRIBUTING.md) and [SECURITY.md](SECURITY.md).

| Component | Path | What it is |
|---|---|---|
| RinCast Streamer | `pc/streamer` | Windows tray app with a WebView2 UI: capture, encoding, input, audio, pairing and SteamVR control. |
| SteamVR driver | `pc/driver-openvr` | Registers a RinCast headset and controllers in SteamVR. |
| Audio driver | `pc/audio-driver` | Optional kernel driver for the RinCast virtual audio devices. |
| Headset app | `headset` | Native OpenXR app for Quest and PICO. What differs between them is in `headsets/`. |
| Account server | `server` | Go service for accounts, headset sign-in codes, PC presence and rendezvous. |
| rincast-core | `shared` | Protocol, crypto, transport, FEC, Opus and discovery code shared by PC and headset. |

## Licence

RinCast is under the [MIT licence](LICENSE), except `pc/audio-driver/src`, which
is derived from a Microsoft sample and stays under the Microsoft Public License.
Third-party components keep their own licences; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The music the headset app plays before a PC is connected is by Scott Buckley
(www.scottbuckley.com.au): 'Electric Dreams', 'Twilight Echo', 'Machina' and
'Tears In Rain', released under [CC-BY 4.0](https://creativecommons.org/licenses/by/4.0/).

## Disclaimer

RinCast is an independent project. It isn't affiliated with, endorsed by or
sponsored by Meta, Oculus, PICO, ByteDance, Virtual Desktop Inc., Valve or Microsoft. Their names
and product names are trademarks of their owners and are used here only to
describe compatibility.
