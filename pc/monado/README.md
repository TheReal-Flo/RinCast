# RinCast's Monado service

PC VR through [Monado](https://monado.freedesktop.org), the open OpenXR runtime, on Linux and
Windows. OpenXR games render through Monado's OpenXR runtime as usual, and its compositor hands
each finished frame to RinCast Streamer, which streams it to the headset like a SteamVR frame.

- **Linux:** this is how PC VR works. SteamVR on Linux can't hand frames to a driver.
- **Windows:** a choice next to SteamVR, under *OpenXR games run through* in the streamer's VR
  settings. SteamVR games that only speak OpenVR still need SteamVR.
- **macOS:** Monado doesn't run there, and macOS has no PC VR games.

## How it fits together

`rincast-monado` is Monado's own service (`ipc_server`, its main compositor, Monado's OpenXR
runtime library), built from Monado's sources at a pinned version (see `CMakeLists.txt`). Two
pieces of RinCast replace Monado's hardware drivers and display output:

| File | What it does |
|---|---|
| `src/devices.cpp` | The headset and two Oculus Touch controllers, from the streamer's tracking (`pc/common/vr_bridge.h`): poses, buttons, battery, haptics back to the headset |
| `src/target.cpp` | The compositor's output: each frame goes into one of the streamer's frame images, timed to the headset's display |
| `src/hub.cpp` | The link to the streamer: the VR bridge's shared memory, the producer heartbeat, the vsync the streamer steers |
| `src/instance.cpp`, `src/main.cpp` | Monado's instance with these devices and target, on the GPU the streamer encodes on |

The frame images are the streamer's. On Windows they're the D3D11 textures the SteamVR driver
also uses, opened by name from Vulkan, with their keyed mutex. On Linux they're Vulkan images the
streamer allocates and passes over a Unix socket (`pc/common/vr_bridge_images.h`). Linear images,
where the GPU can render to them, go to VAAPI without a copy.

The service, its runtime library and pid file have names of their own (`rincast-monado`,
`openxr_rincast_monado`, `rincast_monado_ipc`), so a Monado you installed yourself is left alone.

Not there yet: finger tracking and body trackers. The headset sends hands in SteamVR's skeleton,
and Monado wants OpenXR's joints.

## Building

Linux (Ubuntu 24.04 package names; the rest of docs/LINUX.md's list for the streamer):

```sh
sudo apt install build-essential cmake ninja-build git python3 libeigen3-dev libudev-dev glslang-tools libvulkan-dev
cmake -S pc/monado -B build/monado -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/monado --target rincast-monado
```

Windows, with vcpkg for Monado's dependencies (`scripts\build-all.ps1` does this when it finds
vcpkg):

```powershell
cmake -S pc\monado -B build\monado -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build\monado --config Release --target rincast-monado
```

Everything lands in `build/monado/monado`: the service, the runtime library and
`rincast-monado.json`, the runtime's OpenXR manifest. The streamer finds that folder next to
itself (`monado/`, in a package) or in the build tree.

## Running

The streamer starts the service when a headset launches VR with Monado chosen (always on Linux),
and stops it when it quits. On Windows Monado's runtime also starts it when a game opens it. Its
log is `monado.log` in the streamer's logs folder (Linux) or Monado's own output.

Choosing Monado in the streamer makes it the active OpenXR runtime: on Linux by linking
`~/.config/openxr/1/active_runtime.json` to `rincast-monado.json` (the file that was there is kept
as `active_runtime.json.before-rincast`), on Windows in the registry, as for SteamVR.

Steam games that use OpenVR run on OpenXR through [xrizer](https://github.com/Supreeeme/xrizer) or
[OpenComposite](https://gitlab.com/znixian/OpenOVR), as with other Monado setups.

## Licence

Monado is under the Boost Software License 1.0; packages carry it as `LICENSES/Monado.txt`.
