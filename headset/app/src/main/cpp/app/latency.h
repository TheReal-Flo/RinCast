// Where a frame's latency goes, stage by stage, in the order a frame passes through them.
// PC times are moved onto the headset's clock with the offset the pings measure.
#pragma once

namespace rv {

enum LatencyStage {
    kStageUplink,       // VR: head pose to the PC (half the round trip)
    kStageGame,         // VR: the game renders it and hands it to SteamVR
    kStageEncoderWait,  // VR: waiting for the encoder
    kStageEncode,       // VR: encoding
    kStageCapture,      // desktop: capture and encoding
    kStageDownlink,     // the frame to the headset
    kStageDecode,       // decoding
    kStageNextFrame,    // decoded, waiting for the headset's next frame to pick it up
    kStageCompositor,   // picked up: the headset's compositor until it's lit
    kStageCount
};

struct LatencyStageInfo {
    const char* key;    // in the stats sent to the PC
    const char* label;  // in the headset's performance overlay
};

inline constexpr LatencyStageInfo kLatencyStages[kStageCount] = {
    {"uplink", "Head movement to the PC"},
    {"game", "Game and SteamVR"},
    {"encoderWait", "Waiting for the encoder"},
    {"encode", "Encoding"},
    {"capture", "Capture and encoding"},
    {"downlink", "Video to the headset"},
    {"decode", "Decoding"},
    {"nextFrame", "Waiting for the next frame"},
    {"compositor", "Compositor and display"},
};

}  // namespace rv
