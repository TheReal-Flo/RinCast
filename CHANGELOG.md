# Changelog

Notable changes to RinCast. Versions follow [Semantic Versioning](https://semver.org).

## 1.0.0 - unreleased

First public release.

- Windows streamer with desktop streaming of every monitor and PC VR through SteamVR.
- Quest 3 app with procedural skies and passthrough.
- The same app on PICO 4 and PICO 4 Ultra, with PICO Motion Trackers as SteamVR trackers and
  PICO face tracking (not tested on a PICO yet).
- H.264, HEVC and AV1 with NVENC, AMF or Media Foundation, over an encrypted UDP transport.
- A performance overlay with latency split by stage, from the in-VR menu.
- A wired connection over a USB-C cable on Horizon OS 2.5 or later (not tested with a headset yet).
- Body tracking as SteamVR trackers, and face and eye tracking shared with VRCFaceTracking
  through RinCast's module (off by default, not tested on a headset yet).
- Background music on the headset while no PC is connected.
- LAN discovery and pairing without an account. Optional accounts for direct connections
  from other networks, with a self-hostable server.
- Audio through Steam's streaming devices by default. See "Audio" in the README.
- A PC only accepts headsets it has allowed, on every path, tied to a key the headset keeps.
- The headset reconnects by itself after a dropped connection, and explains failures until
  they're dealt with.
- Accessibility on the headset: interface size, high contrast, sounds, read aloud, a one-handed
  route to the menu. The streamer and website work with keyboard, screen readers and Windows
  high contrast.
