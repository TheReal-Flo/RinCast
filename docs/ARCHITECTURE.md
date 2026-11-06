# Architecture

```
 Quest 3 (headset app)                         Windows PC
 ┌──────────────────────────┐   UDP 38711    ┌───────────────────────────────────────────┐
 │ OpenXR + GLES 3.2        │◄──────────────►│ RinCast Streamer                          │
 │  windows / sky / warp    │  encrypted     │  StreamServer ─ ClientSession             │
 │  MediaCodec → OES tex    │  control,video │   ├ DesktopStream × monitors              │
 │  AAudio  ↔ Opus          │  audio,track   │   │   DXGI duplication → NV12/P010 → enc  │
 │  quad/cylinder layers    │                │   ├ VrStream ◄── VR bridge (shared mem)   │
 │  projection layer (VR)   │   UDP 38710    │   ├ Audio: WASAPI loopback / mic render   │
 │  Discovery ──────────────┼── broadcast ──►│   ├ Input: SendInput                      │
 │  Cloud (device sign-in)  │                │   └ CloudClient (presence, long-poll)     │
 └───────────┬──────────────┘                └──────┬─────────────────────┬──────────────┘
             │ HTTPS + UDP 38720                    │ shared textures     │ shared textures
             ▼                                      ▼                     ▼
      RinCast server (Go)                     driver_rincast.dll    rincast-openxr.dll
      accounts · devices · rendezvous         (SteamVR)             + LibOVRRT64_1.dll (RinCast XR)
```

## PC side

### Streamer (`pc/streamer`)
- **UI.** WebView2 with a Mica backdrop and a tray icon, single instance. The page is in `ui/`. It talks to `App` through JSON `postMessage` (`getState`, `setSettings`, `approve`, `signIn`, …). Open `ui/index.html?demo=connected` in a browser to see a mock of the UI.
- **Capture** (`capture/`). Uses one `IDXGIOutputDuplication` per monitor and composites the cursor on the GPU. If a monitor is asleep, it wakes it (`input::WakeDisplays`) before duplicating.
- **Encode** (`encode/`). A shader converts BT.709 limited-range RGB to NV12 (8-bit) or P010 (10-bit). The encoder backends are:
  - **NVENC** (SDK 13.1, loaded at runtime) supports H.264, HEVC and AV1 on RTX 40 and newer.
  - **AMF** supports H.264, HEVC and AV1 on RX 7000 and newer.
  - **Media Foundation** is the fallback, used for Intel and anything else.

  All of them run in low-latency, infinite-GOP CBR mode with a VBV of one frame (two for desktop), so a frame never takes much longer than a frame interval to send. On NVENC:
  - **Split-frame encoding.** On GPUs with two or more NVENC engines, frames of 2560×1440 and larger (HEVC, AV1) are encoded by all engines together, in strips. On an RTX 5070 Ti, a 3664×1920 VR frame takes 2.1 ms instead of 3.5 ms.
  - **Loss recovery.** HEVC and AV1 keep 8 reference frames. When the headset reports a loss, the streamer invalidates every frame after the last one the headset decoded (`NvEncInvalidateRefFrames`). The next frame is then a normal P-frame flagged `kVideoRecovery`, not a keyframe. AV1 can bridge 3 lost frames this way and HEVC 7; past that, NVENC falls back to an intra frame. Other encoders and H.264 send a keyframe. Repeated requests are ignored while an answer is already on its way.
  - **Foveated encoding** (VR, on by default). A per-block QP offset, computed from each eye's field of view, raises the quantizer from 25° off the lens axis up to 50° and beyond. Rate control keeps the frame size, so those bits go to the centre.

  AMF never adds CBR filler data, enforces the VBV and moves it with bitrate changes, and waits on `QueryOutput` instead of polling.
- **VideoSender.** Splits frames into fragments sized to the MTU and adds XOR parity packets (FEC). The bitrate follows the queueing delay the headset measures (how much longer frames take to arrive than the quickest ones recently): when the stream is faster than the path, it cuts the bitrate below the rate that actually arrives before the router queue grows into a lag, then probes upward again. See [Bitrate control](PROTOCOL.md#bitrate-control).
- **VR** (`vr/`, `stream/vr_stream`). The SteamVR driver or RinCast XR fills a ring of three shared D3D11 textures, protected by keyed mutexes, in the VR bridge (`pc/common/vr_bridge.h`). Each frame carries the pose it was rendered with, and that pose is sent to the headset with the frame. Tracking flows the other way through a seqlock in the same shared memory. The bridge survives a producer crash: abandoned mutexes are reclaimed and stuck slots are reset.
- **SteamVR control.** Registers the driver with `vrpathreg`, launches SteamVR, switches the active OpenXR runtime (SteamVR or RinCast XR), and lists installed VR games from Steam's `appmanifest`/`localconfig.vdf` files.
- **Audio.** The **Audio devices** setting picks a virtual device set: RinCast ("RinCast Speaker" and "RinCast Microphone"), Steam ("Steam Streaming Speakers" and "Steam Streaming Microphone"), Automatic (RinCast if installed, otherwise Steam) or the default devices. While a headset is connected, the streamer can make the chosen devices the Windows defaults and restores the previous ones on disconnect. It captures the speaker (or the default device) with WASAPI loopback and encodes it with Opus at 48 kHz in 10 ms frames. Microphone audio from the headset is decoded and rendered to the virtual microphone's render side ("RinCast Microphone Feed", or the render endpoint of "Steam Streaming Microphone"), and the driver loops it into the capture endpoint that apps record from. Virtual microphones copy raw audio from their render side to their capture side without converting, so the streamer gives both sides the same default format (48 kHz stereo) when they differ. Steam's ships as stereo 48 kHz / mono 44.1 kHz, which played the voice at less than half its pitch with 400+ ms of growing delay. `tools/audio-check` lists the devices and can verify that loopback.

### SteamVR driver (`pc/driver-openvr`)
Implements `IServerTrackedDeviceProvider` with a HMD (`IVRDisplayComponent` + `IVRDriverDirectModeComponent`) and two controllers on SteamVR's `oculus_touch` input profile.
- **Direct mode.** SteamVR renders each layer (the game's eyes, overlays such as the dashboard) into textures the driver creates, and submits them with the head pose each was rendered with. `Present` composites the layers side by side into a bridge slot and tags it with the tracking sample whose pose matches. The headset reprojects every frame from that pose, so it must be exact. The virtual display interface this replaced didn't say which pose a frame used, and the tag was one sample off on most frames: jitter while turning the head.
- **Pacing.** The tracking thread hands SteamVR the newest pose 1 ms before each (virtual) vsync and then sends `VsyncEvent`; `PostPresent` blocks until the next vsync. The vsyncs fall on a grid the streamer sets (`vsyncEpochNs`), which the headset steers with `vr.phase` so frames are decoded just before its late latch instead of at a random point of its frame.
- **Controller pose.** The headset sends the OpenXR grip pose. The driver maps it to the device pose SteamVR expects through the `openxr_grip` component of SteamVR's own render model for the controller (10.2 cm behind the device origin, pitched 20.6°), so SteamVR reports to games exactly the grip pose the headset measured.
- **Controllers** are labelled with the time SteamVR predicts for (two vsyncs after the one following the pose update, plus 2 ms, measured with `pose-probe`), so SteamVR's own extrapolation cancels out and games render the headset's prediction. Labelled with the real display time, SteamVR rewound them and games drew the hands up to the stream latency behind.
- **Haptics** are sent back through the bridge.

### RinCast XR (`pc/runtime-openxr`)
Not in this release: this runtime is excluded from the public repository and from release builds, and is built only with `-DRINCAST_BUILD_OPENXR=ON`.

A fork of VDXR. VDXR translates OpenXR to LibOVR. RinCast supplies its own `LibOVRRT64_1.dll` (`ovr-backend/`), which reads tracking from the bridge and composites projection and quad layers into bridge slots. So OpenXR games stream without SteamVR. The runtime manifest `rincast-openxr.json` is registered under `HKLM\SOFTWARE\Khronos\OpenXR\1\AvailableRuntimes`.

### Audio driver (`pc/audio-driver`)
A PortCls/WaveRT kernel driver derived from Microsoft's SimpleAudioSample. It is root-enumerated (`ROOT\RinCastAudio`) and exposes three endpoints:

| Endpoint | Direction | Purpose |
|---|---|---|
| RinCast Speaker | render | PC audio routed to the headset |
| RinCast Microphone | capture | what apps such as Discord and games record |
| RinCast Microphone Feed | render (internal) | the streamer writes the headset mic here; a kernel ring buffer loops it into RinCast Microphone |

## Headset app (`headset/app/src/main/cpp`)

- `xr/`: instance, session and swapchains. It uses these extensions:
  - `XR_FB_composition_layer_settings` for sharpening
  - `XR_KHR_composition_layer_cylinder`
  - `XR_FB_display_refresh_rate`
  - `XR_FB_passthrough`
  - `XR_KHR_convert_timespec_time`

  Controller input is bound to the profiles of the headset's family (below).
- `headsets/`: what differs between headsets. One APK runs on all of them; `Headset::Create`
  picks the family from the OpenXR runtime's name, and every family's extensions are turned on
  where the runtime has them. The rest of the app only sees the `Headset` interface.
  - `quest/`: Touch controllers, body tracking (`XR_FB_body_tracking`) and face and eye
    tracking (`XR_FB_face_tracking2`, `XR_FB_eye_tracking_social`).
  - `pico/`: PICO 4, 4 Ultra and Neo3 controllers (`XR_BD_controller_interaction`,
    `XR_BD_ultra_controller_interaction`); the Motion Trackers, each as it is through PICO's
    `XR_BD_motion_tracking`, or the whole body through `XR_BD_body_tracking`; and face tracking
    (`XR_BD_facial_simulation`), whose 52 expressions are sent as the Quest's 70. On PICO the app
    uses the stage space, where the trackers are, and works velocities out from poses.
  - Anything else gets plain OpenXR and Touch bindings. A new family is a `Headset` subclass and
    a line in `Headset::Create`.
- `media/`: `AMediaCodec` decodes into an `AImageReader` surface. The images are wrapped as `EGLImage` and sampled as external OES textures. AAudio handles playback and microphone capture.
- `net/`: UDP discovery, the stream client (handshake, connection and reassembly) and the cloud client (device-code sign-in and connect requests).
- `gfx/`: procedural skies and the hyperspace tunnel. Also the 2D UI renderer (SDF text and icons, rounded rects, shadows) and the 3D scene (panels, laser pointers).
- `ui/`: an immediate-mode widget toolkit and the 3D `WindowManager`. Windows can be moved by grabbing their handle bar, pushed and pulled with the thumbstick, and resized from the corners. They render into per-window render targets that are redrawn only when dirty.
- `app/`: the `App` state machine (`Home → Connecting → Desktop ⇄ LaunchingVr → Vr`) and the window contents (`screens.cpp`).

### Monitors
Each monitor is a separate video stream. It is shown as an `XrCompositionLayerQuad` or `XrCompositionLayerCylinderKHR`, so text stays sharp, and placed under the eye buffer with a hole punched through the eye buffer. With "Sharp text" off, monitors are drawn as textured meshes instead.

### Launching SteamVR
1. All windows fade out.
2. The hyperspace tunnel fades in and speeds up.
3. The PC launches SteamVR or the chosen game and reports `vr.state`.
4. Once the first VR frame decodes, the tunnel slows and the projection layer of the stream crossfades in.

The `vr.state` messages go `idle → launching → running` (or `error`).

In VR mode, frames are shown as a projection layer using the pose they were rendered with, so the headset's compositor does the time warp. Double-pressing the menu button opens the RinCast overlay (Desktop, Recenter, Stats, Disconnect).

## Account server (`server`)
Go with bbolt storage. Passwords are hashed with argon2id. Access tokens are HS256 JWTs with rotating refresh tokens. The headset signs in with a device code. PCs hold a long-poll connection for connect requests.

A remote connection works like this:
1. The headset asks the server to connect to a PC.
2. The server hands both sides a one-time `connectSecret` and a rendezvous token.
3. Both sides register with the UDP rendezvous on port 38720 together with every address they can be reached on: LAN IPv4, global IPv6 and, on the PC, a router port mapping (PCP, NAT-PMP or UPnP). The server adds the public endpoints it observed and gives each side the other's list.
4. Both sides send to all of those addresses at once (hole punching). The first path that answers carries the session, and the headset switches to a better path later if one starts answering.
5. The server never relays stream traffic. If no direct path exists, the connection fails with an explanation.
6. The secret is mixed into the handshake, but the server knows it too, so it only protects against someone who isn't the server. What keeps a PC safe from a stolen password or a hostile server is the next step.
7. The headset has a long-lived identity key (Ed25519, in its settings) and signs each connection's temporary key with it. A PC only accepts headsets it has allowed, on every path, and pins that key the first time. A headset the PC doesn't know gets a prompt on the PC, even if its account is right. "Ask before a new headset connects" turns this off.

Streaming sockets are dual-stack (IPv4 and IPv6). With DS-Lite or another carrier NAT at home, IPv6 is usually the direct path.
