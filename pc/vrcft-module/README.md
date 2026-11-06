# RinCast module for VRCFaceTracking

Brings the face and eye tracking a Quest sends through RinCast into
[VRCFaceTracking](https://github.com/benaclejames/VRCFaceTracking), which drives avatars in VRChat
and other games.

- **Quest Pro:** face and eyes, from its cameras.
- **Quest 3 and 3S:** mouth and jaw, estimated from the microphone.

## Use

1. On the headset: **VR quality**, **Face tracking** on. The headset asks for permission once.
2. Build the module (below), and copy `RinCastModule.dll` to
   `%AppData%\VRCFaceTracking\CustomLibs`.
3. Start VRCFaceTracking. It finds RinCast once a headset is in VR mode with face tracking on.

## Build

Needs the .NET SDK and `VRCFaceTracking.Core.dll` from the VRCFaceTracking version you use:

```powershell
dotnet build -c Release -p:VRCFTCore="C:\path\to\VRCFaceTracking.Core.dll"
```

## How it gets the data

RinCast Streamer writes the newest face and eyes into shared memory named `RinCastFaceTracking`.
The layout, field by field, is in [`pc/common/face_tracking.h`](../common/face_tracking.h). Other
programs can read it the same way.

## Status

This module is a starting point: it hasn't been built against VRCFaceTracking or tried with a
headset yet. Expressions are mapped one-to-one where Meta's and VRCFaceTracking's sets match. Eye
gaze is passed as the direction's left/right and up/down parts, and may need calibrating.
