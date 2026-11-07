# RinCast for the Steam Frame (native)

The headset app as a native SteamOS program for the Steam Frame (Linux, ARM64). It's the same code as
the Android app for Quest, PICO and Galaxy XR; what differs is in `platform/linux`,
`media/video_decoder_v4l2.cpp` and behind `#ifdef __ANDROID__`.

**Status: experimental, not tested on a Frame yet.** It draws with OpenGL ES, handed to SteamVR
through `XR_MNDX_egl_enable`. Whether SteamVR on the Frame accepts that is the first thing a test
shows.

## Building

On any PC with Docker (an x86 PC emulates ARM64; register that once with
`docker run --privileged --rm tonistiigi/binfmt --install arm64`), from the repository root:

```sh
docker run --rm --platform linux/arm64 -v "$PWD:/src" -w /src \
  registry.gitlab.steamos.cloud/steamrt/sniper/sdk/arm64:latest sh headset/linux/build.sh
```

The program is `build/frame/rincast-frame`, with its `assets/` folder next to it.

## Putting it on the Frame

1. On the Frame, turn on developer mode.
2. On the PC, install the **SteamOS Devkit Client** (in Steam, under Tools) and pair it with the Frame.
3. In the Devkit Client, use **Title Upload**:
   - Name: RinCast
   - Folder: the folder with `rincast-frame` and `assets/`
   - Start command: `rincast-frame`
   - Runtime: **Steam Linux Runtime 3.0 ARM64 (Sniper)**
4. On the Frame, start RinCast from **Library > Non-Steam**. It has to be started from Steam: that's
   what makes SteamVR's OpenXR runtime available to it.

## What's different from the Android app

- Video: H.264 and 8-bit HEVC only (the Frame's decoder driver has no AV1 or 10-bit yet). The PC
  picks accordingly.
- Controllers: the Frame's controllers appear as Touch controllers.
- No UI sounds or reading aloud yet; no USB cable connection.

## Logs

RinCast writes `~/.local/share/rincast/rincast.log` (the run before is `rincast.previous.log`).
Copy it off with the Devkit Client or `scp steamos@<frame address>:.local/share/rincast/rincast.log .`

The first lines say what matters most:

- `OpenXR loader: ...`: which OpenXR loader it found (SteamVR's).
- `Runtime extensions: ...`: everything SteamVR offers.
- `Graphics: OpenGL ES context through XR_MNDX_egl_enable, as ...`: which way the picture goes.
- `V4L2 decoder: H.264 ..., HEVC ...`: what the video decoder can do.
- `Decoder ... first frame decoded`: video works.

`RINCAST_OPENXR_LOADER=/path/to/libopenxr_loader.so` in the launch options picks another loader.
