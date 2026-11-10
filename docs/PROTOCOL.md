# RinCast wire protocol (version 3)

The authoritative definitions are in `shared/include/rc/protocol.h`. All
integers are little-endian and all structs are packed.

## Ports

| Port | Use |
|---|---|
| UDP 38710 | LAN discovery |
| UDP 38711 | streaming (one socket per streamer) |
| UDP 38720 | rendezvous on the RinCast server (introduction only, never relays) |
| HTTPS | account API (`server/README.md`) |

## Discovery
The headset broadcasts a `Probe` on every interface:
`DiscoveryHeader{magic 'RCDS', version, type}` followed by JSON
(`deviceId`, `name`). Streamers reply with a unicast `Announce` containing
`serverId`, `name`, `gpu`, `version`, `busy`, `accountName` and
`streamPort`, and also broadcast an announce every 3 s.

## Packet header
```
PacketHeader (16 B): version u8 | type u8 | flags u16 | sessionId u32 | sequence u64
```
Encrypted packets look like this:
```
header ‖ XChaCha20-Poly1305(payload) ‖ tag(16)
```
- The header is the associated data.
- The nonce is derived from `sequence` and the direction.
- Each direction has its own key.

The receiver keeps a 2048-entry replay window per direction.

## Handshake
```
Hello   [header][client X25519 pk][JSON]   client → server
Welcome [header][server X25519 pk][JSON]   server → client (assigns sessionId)
Reject  [header][JSON {reason, message}]
```
Hello and Welcome both carry a random nonce. Session keys are derived as
`BLAKE2b(X25519 shared ‖ client pk ‖ server pk ‖ context)`, where context
= `SessionContext(clientNonce, serverNonce, connectSecret)`.

- **Local sessions.** These use trust on first use. An unknown device gets `Reject{reason:"pending_approval"}` and the PC asks the user. The headset keeps retrying for 2 minutes. Only private IPv4 addresses are accepted.
- **Cloud sessions.** Hello carries `HelloProof = MAC(connectSecret, client pk)` and Welcome carries `WelcomeProof`. The `connectSecret` from the server is also mixed into the KDF.

## Channels (encrypted packet types)

| Type | Dir | Reliability | Content |
|---|---|---|---|
| Control (10) | both | reliable, ordered | JSON messages, split into chunks with `ControlChunkHeader` |
| Ack (11) | both | – | cumulative + 32-bit selective mask |
| Video (12) | PC→HS | FEC | `VideoFragmentHeader` + fragment; frame starts with `VrFrameMetadata` or `DesktopFrameMetadata` |
| VideoParity (13) | PC→HS | – | XOR parity over a group of fragments |
| Audio (14) | both | none | `AudioPacketHeader` + Opus (48 kHz, 10 ms) |
| Tracking (15) | HS→PC | none, latest wins | `TrackingPayload`: HMD + 2 × `ControllerState`, sample/target times |
| Pointer (16) | HS→PC | none, idempotent | absolute mouse position on a monitor stream |
| Ping/Pong (17/18) | both | – | RTT and clock offset |
| VideoFeedback (19) | HS→PC | none | receive/decode/display timing, loss and queueing delay, every 100 ms and immediately after a loss; `requestKeyframe` with `lastGoodFrame` |
| Haptics (20) | PC→HS | none | vibration requests |
| Disconnect (21) | both | – | – |
| Video metadata | PC→HS | with frame generation on | `MotionHeader` and run-length motion vectors after `VrFrameMetadata`; see `shared/include/rc/motion_field.h` |
| BodyTracking (22) | HS→PC | none | body joints for the SteamVR trackers, while body tracking is on |
| HandSkeleton (23) | HS→PC | none | SteamVR hand bones of one hand, while a tracked hand plays a game or "Real fingers" is on |

Stream id 0 carries VR. Ids 1–15 are desktop monitors.

### Controller identity and aiming

`Hello` and `client.info` may include `controllerProfiles`, an array of two OpenXR
interaction profile paths (left, right). An empty string means that no active profile
is available. Updated headsets refresh this while tracking with reliable control:

```json
{"type":"controller.profiles","controllerProfiles":["/interaction_profiles/bytedance/pico4_controller","/interaction_profiles/bytedance/pico4_controller"]}
```

The PC uses these reported profiles for native controller identity; it does not infer
them from `model`. Unknown or missing profiles are not substituted. Explicit Touch
compatibility is a PC-side choice. Native mode requires an updated headset app.

Tracking datagrams retain the existing `TrackingPayload` prefix. They can append
`GazePayload` (28 bytes), followed by `ControllerAimPayload` (60 bytes):

```
validMask u32 | left aim Pose (28 B) | right aim Pose (28 B)
```

Bits 0 and 1 mark valid left/right aim poses, located in the same app space and at
the same predicted time as the grip poses in the prefix. Each pose contains quaternion
`x,y,z,w` followed by position `x,y,z` (all float32). The gaze block is always present
before aim; its flags are zero when gaze is unavailable. Older receivers can ignore
the extra suffix, and newer receivers accept old packets without aim. Truncated or
non-finite aim data is not used for calibration. The wire protocol remains version 3.

### Loss recovery
Frames are predicted from earlier frames, so the headset stops feeding the decoder at
the first missing frame. It keeps showing the last good picture and sends
`VideoFeedback{requestKeyframe=1, lastGoodFrame}` straight away, where `lastGoodFrame`
is the newest frame of the unbroken chain it decoded. The PC either:
- invalidates every frame after `lastGoodFrame` in the encoder and sends the next frame
  with the `kVideoRecovery` flag (it references only `lastGoodFrame` or older), or
- sends a keyframe, if the encoder can't do that or `lastGoodFrame` is too old.

The headset resumes at either one. While such a frame is on its way, requests are
ignored: that is, as long as the headset's `lastCompleteFrame` is older than the frame
and less than 300 ms have passed. `lastGoodFrame` was added after version 3 shipped:
the PC accepts shorter feedback packets and treats the field as 0, which means a
keyframe.

## Control messages (JSON `type`)

**Headset → PC:**
- `client.info`: device, display size and rate, decoders
- `settings.get` and `settings.set`
- `desktop.start` and `desktop.stop`
- `vr.start`, `vr.stop`, `vr.launchSteamVR` and `vr.recenter`
- `games.list`, `games.launch` and `games.image`
- `input.mouseButton`, `input.mouseWheel`, `input.key`, `input.text` and `input.gamepad`
- `vr.phase`: twice a second in VR, `shiftNs`, how far the PC should move its frame timing (+ = later) so frames are decoded just before the headset's late latch, with a margin sized from how much their arrival varies. The streamer moves the driver's vsync grid (`vsyncEpochNs` in the shared memory) by half of it and learns the drift between the two clocks from the rest.
- `stats.client`: once a second, fps, latency, decode time and `latencyStages`, the latency split into the stages a frame passes through, in ms (`uplink`, `game`, `encoderWait`, `encode` for VR, `capture` for desktop, then `downlink`, `decode`, `nextFrame` (decoded until the headset's late latch picks it up) and `compositor` (from there until it's lit)). PC timestamps from the frame metadata are moved onto the headset's clock with the offset measured by the pings; `uplink` is half the round trip.

**PC → headset:**
- `server.info`
- `settings`
- `desktop.monitors`
- `stream.config`: codec, resolution and stream id per stream
- `vr.state`: `idle`, `launching`, `running` or `error`
- `games` and `games.image`
- `stats.server`
- `notice`: text, level and an optional action

## USB cable connections

On Horizon OS 2.5+, the app asks Android for the headset's USB network (`TRANSPORT_USB`,
without the internet and trusted capabilities); the user allows it once. The link is IPv6
link-local only. The headset sends its discovery probe to `ff02::1` (every device on the link)
on that interface, from a socket bound to that network. The streamer joins `ff02::1` on every
adapter, again as adapters appear, and answers on an IPv6-only socket on the discovery port. The
stream then runs to that link-local address, with its scope, like any local connection.

## Face tracking

`FaceTracking` (24), headset to PC, unreliable, once per frame while face tracking is on:
`XR_FB_face_tracking2`'s 70 expression weights and 2 confidences as bytes (0..255), and eye gaze
directions in the head's space with their confidences. The streamer publishes them in shared
memory, `RinCastFaceTracking`, laid out in `pc/common/face_tracking.h`.

## VPN connections

A headset can reach a PC through any VPN by probing a typed address directly, with no account or rendezvous. Each side checks by itself whether its route to the other leaves through a tunnel (`rc::RoutesThroughTunnel`). If so, it uses the smaller datagram size, and the PC tunes the stream as it does for remote connections: pacing, more FEC, adaptive bitrate and a lower cap. When a PC answers on both the local network and a VPN, the headset keeps the local address for as long as it keeps answering.

## Remote connections (UDP 38720 rendezvous)
Remote sessions are always direct between headset and PC. The server only
introduces the two sides; it never forwards stream traffic.

Datagrams to the rendezvous start with `"RCRV"` followed by JSON:

```
{"op":"register","session":..,"role":0|1,"token":..,"candidates":["ip:port","[ipv6]:port",..]}
  -> {"op":"registered","self":"<observed ip:port>","peer":"<first>","peers":[..]}
```

- **Candidates** are the addresses a side can be reached on:
  - its LAN IPv4 addresses (VPN and virtual adapters such as Tailscale, NordLynx or WSL are left out),
  - its global IPv6 addresses,
  - on the PC, a public endpoint the router forwards to it (PCP, NAT-PMP or UPnP IGD, when the router allows it and has a public IPv4 address).
- **Registration:** each side registers over every address family the rendezvous host resolves to, so the server observes its public IPv4 endpoint and, if the server has an AAAA record, its IPv6 one.
- **The `peers` list:** the server answers with the other side's observed endpoints followed by its candidates. Whenever one side registers, the server also sends the other side an updated `registered` message.
- **Punching:** both sides then send to every peer address at the same time. The PC sends `Punch` packets every 100 ms for up to 30 s; the headset sends its `Hello` every 200 ms. For public IPv4 peers they also try a few neighbouring ports during the first 10 s, for routers that allocate ports sequentially. This opens both NATs and firewalls, and the first `Welcome` decides the path.
- **Path upgrade:** during the session the headset keeps offering the PC's better-ranked addresses (same LAN beats a direct internet path, which beats a VPN overlay). When the PC answers one with its `Welcome`, the headset switches to it, and the PC follows the authenticated traffic.
- **Failure:** if no path works within 30 s, the connection fails with an explanation. There is no relay fallback.

## Bitrate control
Each `VideoFeedback` carries `queueDelayMs`: how much later the first fragment of the
frames of the last report arrived after the PC sent them (`pcEncodeEndNs` in the frame
metadata) than for the quickest frame of the last 10 s. The first fragment, because a large
frame's own transfer time isn't a queue. The two clocks differ by an unknown constant, so only
this excess means anything. It is the time the frames waited in queues because the stream
was faster than the path. The PC adapts each stream's bitrate:
- **Cut by 15 %** when the smoothed queueing delay passes 10 ms, or when loss is sustained:
  more than 2 % of fragments on average, or 20 % in one report. Wi-Fi and mobile links lose
  the odd packet without being full, so a single lost frame is no reason to slow down.
- **Cut to 80 % of the rate that actually arrives** when the delay passes 40 ms, by at most
  half per step. While the bitrate is below what arrives, the queue is draining, so the PC
  waits instead of cutting further.
- **Raise by 5 % + 0.5 Mbit/s** every 500 ms while the delay stays under 5 ms, starting 2 s
  after the last cut.

Remote sessions start at 12 Mbit/s for VR and 8 Mbit/s shared between the monitors (at least
3 per monitor), then probe upward. Their frames are paced at 1.5 times the bitrate instead of
being sent in one burst, which shallow internet queues would drop.

Below about 0.03 bits per pixel (0.05 on the desktop) a stream sends every second frame with
twice the bits; it returns to the full rate above 0.05 (0.08). The headset keeps reprojecting
the newest frame to the current head pose in between.
Headsets that don't send `queueDelayMs` (shorter packets) fall back to loss and arrival
spread.
