// RinCast wire protocol definitions.
//
// Everything that crosses the network between the PC streamer and the headset
// is defined here. Both sides compile this header, so layouts are fixed-size,
// little-endian and packed. See docs/PROTOCOL.md for the narrative version.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rc {

constexpr uint16_t kProtocolVersion = 3;

// Ports. Discovery is UDP, everything else is multiplexed on a single UDP
// socket so that the same code path works locally, hole-punched and relayed.
constexpr uint16_t kDiscoveryPort = 38710;
constexpr uint16_t kStreamPort = 38711;

// Maximum datagram we ever send. 1400 fits Ethernet/Wi-Fi with room for
// IPv4/IPv6+UDP headers; remote sessions drop to 1200 to survive tunnels.
constexpr size_t kMaxDatagramLan = 1400;
constexpr size_t kMaxDatagramWan = 1200;
constexpr size_t kMaxDatagram = kMaxDatagramLan;

constexpr size_t kMacSize = 16;  // Poly1305 tag

// ---------------------------------------------------------------------------
// Discovery (plaintext, LAN only)
// ---------------------------------------------------------------------------

constexpr uint32_t kDiscoveryMagic = 0x52434453;  // 'RCDS'

enum class DiscoveryType : uint8_t {
    Probe = 1,     // headset -> broadcast
    Announce = 2,  // streamer -> unicast reply / periodic broadcast
};

#pragma pack(push, 1)
struct DiscoveryHeader {
    uint32_t magic;
    uint16_t protocolVersion;
    uint8_t type;
    uint8_t reserved;
    // Followed by a UTF-8 JSON document (<= 1024 bytes).
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Stream transport
// ---------------------------------------------------------------------------

enum class PacketType : uint8_t {
    // Handshake (plaintext, payload is JSON + raw keys)
    Hello = 1,
    Welcome = 2,
    Reject = 3,
    // Encrypted from here on.
    Control = 10,        // reliable, ordered chunk of a JSON message
    Ack = 11,            // acknowledgement for Control
    Video = 12,          // fragment of an encoded video frame
    VideoParity = 13,    // XOR parity over a FEC group of Video fragments
    Audio = 14,          // Opus packet
    Tracking = 15,       // headset -> PC poses & buttons
    Pointer = 16,        // headset -> PC absolute mouse position (unreliable, idempotent)
    Ping = 17,
    Pong = 18,
    VideoFeedback = 19,  // headset -> PC per-frame timing & loss
    Haptics = 20,        // PC -> headset
    Disconnect = 21,
    BodyTracking = 22,   // headset -> PC body joints, while body tracking is on
    HandSkeleton = 23,   // headset -> PC SteamVR hand bones, while a tracked hand plays a game
    FaceTracking = 24,   // headset -> PC face expressions and eye gaze, while face tracking is on
    // Rendezvous helpers (server relay / hole punching), plaintext.
    Punch = 30,
};

enum PacketFlags : uint16_t {
    kFlagNone = 0,
    kFlagEncrypted = 1 << 0,
};

#pragma pack(push, 1)
// Common header. For encrypted packets the header is authenticated (AAD) and
// the 64-bit sequence is the AEAD nonce counter for the sending direction.
struct PacketHeader {
    uint8_t version;     // low byte of kProtocolVersion
    uint8_t type;        // PacketType
    uint16_t flags;      // PacketFlags
    uint32_t sessionId;  // chosen by the streamer in Welcome
    uint64_t sequence;   // per-direction monotonic counter
};
static_assert(sizeof(PacketHeader) == 16);

// --- Control channel -------------------------------------------------------
struct ControlChunkHeader {
    uint32_t chunkSeq;     // reliable sequence number of this chunk
    uint32_t messageId;    // message this chunk belongs to
    uint16_t chunkIndex;   // index inside the message
    uint16_t chunkCount;   // total chunks for the message
};

struct AckPayload {
    uint32_t cumulative;   // all chunks < cumulative were received
    uint32_t selectiveMask; // bit i => chunk (cumulative + 1 + i) received
};

// --- Video -----------------------------------------------------------------
enum class VideoCodec : uint8_t { H264 = 0, HEVC = 1, AV1 = 2 };

enum VideoFrameFlags : uint8_t {
    kVideoKeyframe = 1 << 0,
    kVideoHasPose = 1 << 1,   // VR frame: carries render pose
    kVideoHdr10 = 1 << 2,     // 10-bit stream
    kVideoCodecConfig = 1 << 3,
    // Loss recovery without a keyframe: this frame only references frames up to the
    // VideoFeedbackPayload::lastGoodFrame the headset reported, so a decoder that is
    // waiting after a loss can resume with it.
    kVideoRecovery = 1 << 4,
};

// Stream ids. VR always uses 0; desktop monitors use 1..15.
constexpr uint8_t kVrStreamId = 0;
constexpr uint8_t kFirstMonitorStreamId = 1;
constexpr uint8_t kMaxStreams = 16;

struct VideoFragmentHeader {
    uint8_t streamId;
    uint8_t codec;            // VideoCodec
    uint8_t frameFlags;       // VideoFrameFlags
    uint8_t fecGroupSize;     // data fragments per parity packet (0 = none)
    uint32_t frameIndex;
    uint16_t fragmentIndex;
    uint16_t fragmentCount;   // data fragments
    uint32_t frameSize;       // total encoded bytes
};

struct VideoParityHeader {
    uint8_t streamId;
    uint8_t fecGroupSize;
    uint16_t groupIndex;      // parity covers fragments [groupIndex*G, groupIndex*G+G)
    uint32_t frameIndex;
    uint16_t fragmentCount;
    uint16_t lastFragmentSize; // size of the final (possibly short) fragment
    uint32_t frameSize;
};

// Pose & timing metadata that precedes the bitstream of a VR frame.
struct Pose {
    float orientation[4];  // x, y, z, w
    float position[3];
};

struct VrFrameMetadata {
    uint64_t trackingTimestampNs;   // headset clock: the target display time the pose was predicted for
    Pose renderPose;                // HMD pose the PC rendered with
    float fovTan[2][4];             // per-eye left, right, up, down tangents
    float ipd;
    uint32_t eyeWidth;
    uint32_t eyeHeight;
    int64_t pcSubmitNs;             // PC clock, game submitted frame
    int64_t pcEncodeStartNs;
    int64_t pcEncodeEndNs;
};

// Foveated encoding: the PC keeps every pixel of a square around where the eyes look and half
// of them elsewhere, which the headset undoes when it draws the frame. A VR stream whose config
// has "foveation" carries this right after VrFrameMetadata (before any motion field).
//
// Along each axis of an eye, the picture's 0..1 is three parts: [0, a) at density s, [a, a + F)
// at full density, [a + F, 1] at s again. F is the config's "size"; s follows from the sizes,
// (encoded / eye - F) / (1 - F), separately for x and y.
struct VrFoveationMetadata {
    float foveaStart[2][2];  // a per eye (left, right): x, y (from the top), as a part of the eye's picture
};

struct DesktopFrameMetadata {
    uint32_t width;
    uint32_t height;
    int64_t pcCaptureNs;
    int64_t pcEncodeEndNs;
    int32_t cursorX;     // in pixels, -1 when hidden
    int32_t cursorY;
};

// --- Tracking ----------------------------------------------------------------
enum ControllerButtons : uint32_t {
    kBtnA = 1u << 0,
    kBtnB = 1u << 1,
    kBtnX = 1u << 2,
    kBtnY = 1u << 3,
    kBtnMenu = 1u << 4,
    kBtnSystem = 1u << 5,
    kBtnThumbstick = 1u << 6,
    kBtnTrigger = 1u << 7,       // digital (value > threshold)
    kBtnGrip = 1u << 8,
    kTouchA = 1u << 16,
    kTouchB = 1u << 17,
    kTouchX = 1u << 18,
    kTouchY = 1u << 19,
    kTouchThumbstick = 1u << 20,
    kTouchTrigger = 1u << 21,
    kTouchThumbrest = 1u << 22,
};

enum ControllerFlags : uint8_t {
    kCtrlConnected = 1 << 0,
    kCtrlPoseValid = 1 << 1,
    kCtrlHandTracked = 1 << 2,   // pose comes from hand tracking, buttons synthesized
};

struct ControllerState {
    uint8_t flags;
    uint8_t reserved[3];
    uint32_t buttons;
    float trigger;
    float grip;
    float thumbstick[2];
    Pose pose;              // grip pose
    float linearVelocity[3];
    float angularVelocity[3];
};

enum TrackingFlags : uint8_t {
    kTrackHmdValid = 1 << 0,
    kTrackRecentered = 1 << 1,
};

struct TrackingPayload {
    uint64_t sampleTimeNs;          // headset clock when sampled
    uint64_t targetDisplayTimeNs;   // headset clock the poses are predicted for
    uint8_t flags;
    uint8_t reserved[3];
    Pose hmd;
    float hmdLinearVelocity[3];
    float hmdAngularVelocity[3];
    ControllerState controllers[2];  // 0 = left, 1 = right
};

// --- Body tracking ----------------------------------------------------------
inline constexpr int kBodyTrackerCount = 10;
enum class BodyTracker : uint8_t {
    Waist, Chest, LeftFoot, RightFoot, LeftKnee, RightKnee, LeftElbow, RightElbow, LeftShoulder, RightShoulder,
};

struct BodyTrackingPayload {
    uint16_t validMask;  // bit per BodyTracker
    uint16_t reserved;
    Pose poses[kBodyTrackerCount];
};

// --- Face and eye tracking --------------------------------------------------
inline constexpr int kFaceExpressionCount = 70;  // XR_FB_face_tracking2's, in its order
enum FaceTrackingFlags : uint8_t {
    kFaceTrackedValid = 1 << 0,
    kFaceTrackedFromAudio = 1 << 1,  // estimated from the microphone (Quest 3 / 3S)
    kFaceLeftEyeValid = 1 << 2,
    kFaceRightEyeValid = 1 << 3,
};

struct FaceTrackingPayload {
    uint8_t flags;                           // FaceTrackingFlags
    uint8_t upperFaceConfidence;             // 0..255 = 0..1
    uint8_t lowerFaceConfidence;
    uint8_t expressions[kFaceExpressionCount];  // 0..255 = 0..1
    uint8_t gazeConfidence[2];               // left, right
    uint8_t reserved[5];   
    float gaze[2][3];                        // eye directions in the head's space: +x right, +y up, -z ahead
};
static_assert(sizeof(FaceTrackingPayload) == 104);

// Where the eyes look, for foveated encoding: appended to a TrackingPayload (older PCs ignore
// it) by headsets that track the eyes, while foveated encoding is on.
enum GazeFlags : uint8_t {
    kGazeLeftValid = 1 << 0,
    kGazeRightValid = 1 << 1,
};

struct GazePayload {
    uint8_t flags;  // GazeFlags
    uint8_t reserved[3];
    float gaze[2][3];  // eye directions in the head's space: +x right, +y up, -z ahead
};
static_assert(sizeof(GazePayload) == 28);

// Optional tracking extension, after TrackingPayload and GazePayload. Old peers
// can ignore it; old packets without it retain model-based controller aiming.
struct ControllerAimPayload {
    uint32_t validMask;  // bit per controller; poses share the tracking prediction time
    Pose poses[2];
};
static_assert(sizeof(ControllerAimPayload) == 60);

// --- Tracked hands in games -------------------------------------------------
inline constexpr int kHandJointCount = 26;  // XR_EXT_hand_tracking
inline constexpr int kHandBoneCount = 31;   // SteamVR's hand skeleton

struct HandSkeletonPayload {
    uint8_t hand;  // 0 left, 1 right
    uint8_t reserved[3];
    Pose bones[kHandBoneCount];
};

// --- Pointer (desktop mouse) ------------------------------------------------
struct PointerPayload {
    uint8_t monitorStreamId;
    uint8_t visible;
    uint16_t reserved;
    float u;    // 0..1 across the monitor
    float v;
    uint32_t inputSeq;
};

// --- Timing -----------------------------------------------------------------
struct PingPayload {
    uint64_t senderTimeNs;
    uint32_t pingId;
};

struct PongPayload {
    uint64_t originalSenderTimeNs;
    uint64_t responderTimeNs;
    uint32_t pingId;
};

struct VideoFeedbackPayload {
    uint8_t streamId;
    uint8_t requestKeyframe;
    uint16_t reserved;
    uint32_t lastCompleteFrame;
    uint32_t framesLost;          // since last feedback
    uint32_t fragmentsReceived;   // since last feedback
    uint32_t fragmentsRecovered;  // via FEC
    uint32_t fragmentsLost;
    float decodeMs;               // average
    float networkMs;              // average arrival spread
    float totalLatencyMs;         // tracking sample -> display (VR) or capture -> display
    uint32_t receivedKbps;
    // Newest frame of the unbroken chain the decoder was given (0 = unknown). With
    // requestKeyframe set, the PC may answer with a kVideoRecovery frame based on it
    // instead of a keyframe. Older headsets don't send this field.
    uint32_t lastGoodFrame;
    // How much longer frames currently take to arrive than the quickest ones of the
    // last 10 s: time spent waiting in queues (router, Wi-Fi) because the stream is
    // faster than the path. Below 0 = not measured. Older headsets don't send it.
    float queueDelayMs;
};
constexpr size_t kVideoFeedbackMinSize = offsetof(VideoFeedbackPayload, lastGoodFrame);

struct HapticsPayload {
    uint8_t hand;        // 0 left, 1 right
    uint8_t reserved[3];
    float durationS;
    float frequency;
    float amplitude;
};

// --- Audio --------------------------------------------------------------------
enum class AudioStream : uint8_t { PcToHeadset = 0, MicToPc = 1 };

struct AudioPacketHeader {
    uint8_t stream;       // AudioStream
    uint8_t channels;
    uint16_t frameSamples; // samples per channel in this packet (Opus frame)
    uint32_t sequence;
    uint64_t timestampNs;
};

constexpr uint32_t kAudioSampleRate = 48000;

#pragma pack(pop)

// ---------------------------------------------------------------------------
// Control message types (JSON "type" field). Kept as strings for
// forward-compatibility; listed here so both sides agree on spelling.
// ---------------------------------------------------------------------------
namespace msg {
// headset -> PC
inline constexpr const char* kClientInfo = "client.info";           // device caps, display, codecs
inline constexpr const char* kControllerProfiles = "controller.profiles";
inline constexpr const char* kRequestSettings = "settings.get";
inline constexpr const char* kUpdateSettings = "settings.set";
inline constexpr const char* kStartDesktop = "desktop.start";
inline constexpr const char* kStopDesktop = "desktop.stop";
inline constexpr const char* kStartVr = "vr.start";                 // start or resume VR streaming
inline constexpr const char* kStopVr = "vr.stop";                   // pause it; the game keeps running
inline constexpr const char* kLaunchSteamVr = "vr.launchSteamVR";
inline constexpr const char* kLaunchGame = "games.launch";
inline constexpr const char* kListGames = "games.list";
inline constexpr const char* kMouseButton = "input.mouseButton";
inline constexpr const char* kMouseWheel = "input.mouseWheel";
inline constexpr const char* kKey = "input.key";
inline constexpr const char* kText = "input.text";
inline constexpr const char* kGamepad = "input.gamepad";
inline constexpr const char* kRecenter = "vr.recenter";
inline constexpr const char* kStats = "stats.client";
inline constexpr const char* kVrPhase = "vr.phase";                 // "shiftNs": move the VR frame timing, + = later
// PC -> headset
inline constexpr const char* kServerInfo = "server.info";
inline constexpr const char* kSettings = "settings";
inline constexpr const char* kMonitors = "desktop.monitors";
inline constexpr const char* kGames = "games";
inline constexpr const char* kVrState = "vr.state";                 // idle / launching / running / error
inline constexpr const char* kStreamConfig = "stream.config";       // codec + resolution per stream
inline constexpr const char* kServerStats = "stats.server";
inline constexpr const char* kNotice = "notice";
}  // namespace msg

}  // namespace rc
