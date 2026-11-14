# Features

RinCast covers much of the same ground as Virtual Desktop; the tables below list what it does and doesn't do.

✅ implemented · ◐ partial · ✗ not implemented

## Desktop
| Feature | RinCast |
|---|---|
| See and control the PC desktop in VR | ✅ Each monitor is a floating window. The controller laser drives the mouse: trigger is left click, B/Y is right click, stick click is middle click, and the thumbstick scrolls smoothly. |
| Multiple monitors | ✅ One hardware-encoded stream per monitor. Show or hide them from the dock. |
| Curved screens, distance | ✅ Curvature and distance sliders. Screens are drawn as cylinder or quad compositor layers. |
| Sharp text | ✅ Screens and menus are compositor layers, with super-sampling (and sharpening for screens, `XR_FB_composition_layer_settings`). |
| Move and resize screens | ✅ Grab the handle bar to move, push and pull with the thumbstick, drag the corners to resize. This works for every window. |
| Virtual keyboard | ✅ QWERTY with symbols, shift, sticky Ctrl/Alt/Win, Esc, arrow keys, Tab and Enter. Shows what you typed, hidden by default. |
| Wake a sleeping PC display | ✅ |
| Hand tracking | ◐ On a screen: pinch to click, pinch and move to scroll, pinch and hold to right-click. Dragging with a hand isn't possible yet. |
| Desktop frame rate options | ✅ Auto, 30, 60 or 90 fps. |
| Window/app-only streaming, cursor-follow mode | ✗ |

## VR games
| Feature | RinCast |
|---|---|
| SteamVR support | ✅ SteamVR shows the headset by its model (Quest, Quest 2, Quest Pro, Quest 3, 3S, or the headset's own name) with matching Touch controllers and haptics. |
| Battery status in SteamVR | ◐ Quest and PICO headsets (charge and charging state); PICO controllers (20% steps). Quest controller batteries unavailable. |
| OpenXR runtime without SteamVR | ✗ Not in this release. OpenXR games run through SteamVR. |
| Launch SteamVR from the headset | ✅ The windows hide, the hyperspace tunnel plays, and it fades into the stream once the first frame arrives. |
| Game library with cover art | ✅ Lists installed Steam VR games with library art. Selecting one launches it. |
| Resolution | ✅ A slider from 50% to 130% of the headset's own, or "Match SteamVR's resolution": the stream takes the size SteamVR renders at, and follows it when it changes. |
| Refresh rate (72/80/90/120 Hz) | ✅ The options are the rates the headset reports. |
| H.264, HEVC, AV1, 10-bit | ✅ Uses NVENC, AMF or Media Foundation, with automatic codec choice. |
| Fast recovery from packet loss | ✅ NVENC (HEVC, AV1): a lost frame is answered with a normal frame built from the last one the headset decoded, not a keyframe. Other encoders send a keyframe. |
| Split-frame encoding | ✅ GPUs with two or more NVENC engines encode each large frame together. |
| Bitrate control, adaptive bitrate | ✅ |
| Performance overlay | ✅ Shows latency, fps, bitrate, decode time, network RTT and resolution, and the latency split by stage: network each way, game, encoder, decoder and display. The streamer's Home page shows the same split. |
| In-VR menu | ✅ Double-press the left menu button, or hold A and B, for Desktop, Recenter, Performance and Disconnect. |
| Recenter | ✅ |
| Frame generation, like Synchronous Spacewarp | ◐ Off by default ("Frame generation"). The PC finds motion on the GPU and sends it with each frame; when the next frame is late, the headset moves the last picture along it. Built, not tested on a headset yet. |
| Snapdragon Game Super Resolution | ◐ Off by default ("Upscaling"). Built, not tested on a headset yet. |
| Foveated encoding | ✅ NVENC only, on by default ("Sharper center in VR"): fewer bits at the lens edges, more in the middle, at the same frame size. |
| Foveated streaming (lower resolution at the edges) | ◐ Only following the eyes, off by default ("Foveated encoding", on headsets that track them): full resolution in a square around where you look, half elsewhere, about 53% of the pixels, on any GPU. Built, not tested on a headset yet. Fixed around the lens centre it was tried and left out: at most 1.4 dB better in the centre at very low bitrates, 2 to 7 dB worse towards the edges. |
| Body tracking as SteamVR trackers | ◐ Off by default ("Body tracking"). Waist, chest, feet, knees, elbows and shoulders as Vive trackers. Built, not tested on a headset yet. |
| PICO Motion Trackers | ◐ Through "Body tracking" on a PICO: up to three trackers as feet, knees, waist or chest, by where each is when first seen; or the whole body where the headset estimates it, with a button for PICO's calibration. Built, not tested on a PICO yet. |
| Hands and fingers in PC games | ◐ Fingers follow the controllers' touch sensors. "Hands in games" (off by default) plays with tracked hands: pinch for trigger, middle and ring pinches for A/X and B/Y, a fist for grip. There's no thumbstick without a controller. "Real fingers" (off by default) tracks your fingers while you hold the controllers, and can't run together with body tracking. Built, not tested on a headset yet. |
| Eye and face tracking forwarding | ◐ Off by default ("Face tracking"). Quest Pro: face and eyes; Quest 3 and 3S: mouth and jaw from the microphone. PICO 4 Pro and Enterprise: the face from the cameras; other PICOs: the mouth from the microphone, where the headset offers it. Shared on the PC for [VRCFaceTracking](../pc/vrcft-module/README.md), through RinCast's module. Built, not tested on a headset yet; the module isn't built against VRCFaceTracking yet. |
| Wired connection over USB | ◐ "USB cable" (Horizon OS 2.5+): the headset's USB networking, found by IPv6 link-local discovery. Built, not tested with a headset yet. |
| Xbox gamepad emulation | ✗ The protocol reserves `input.gamepad`, but nothing uses it yet. |

## Audio
| Feature | RinCast |
|---|---|
| Stream PC audio | ✅ Opus at 48 kHz. Volume is set on the headset, or with Windows' volume and volume keys. |
| Virtual speaker driver | ◐ "RinCast Speaker" kernel driver. It needs a signed driver package, which releases don't include yet. |
| Headset microphone on the PC | ✅ Through "Steam Streaming Microphone", or the "RinCast Microphone" kernel driver when a signed package is installed. Its volume is set on the headset or as the recording level in Windows. |
| Steam's virtual devices | ✅ The default for releases. The Audio devices setting can use Steam Streaming Speakers and Steam Streaming Microphone, either chosen directly or through Automatic when the RinCast driver isn't installed. |
| Switch Windows default devices while connected | ✅ This is optional, and the previous defaults are restored. |

## Connectivity and accounts
| Feature | RinCast |
|---|---|
| Automatic LAN discovery | ✅ |
| Pairing | ✅ A new headset has to be allowed on the PC, once or always, on every path including the internet. It's recognized by a key it keeps, not just its name. |
| Connect from other networks | ✅ Uses a RinCast account: device-code sign-in on the headset, then always a direct connection: IPv6, UDP hole punching with candidate exchange, or a router port mapping (PCP, NAT-PMP or UPnP). The server never relays stream traffic. |
| Encryption | ✅ X25519 key exchange with XChaCha20-Poly1305 on every packet. |
| Self-hostable server | ✅ `server/` (Go, Docker). |

## Environment
| Feature | RinCast |
|---|---|
| Environments | ✅ Procedural skies and passthrough. |
| Brightness | ✅ |
| 3D model environments, video player, recording | ✗ |

## PC streamer app
✅ Tray app with a WebView2 UI and Mica backdrop. It shows connection state and live statistics, and has settings for video, audio and VR. It also handles trusted devices, connection approval, account sign-in and approving headset codes, and start with Windows.
