// Shared-memory bridge between the RinCast Streamer and the VR producers that
// run inside other processes: the SteamVR driver (vrserver.exe) and the
// RinCast OpenXR runtime (inside the game).
//
//  Streamer  --(config, tracking, haptics ack)-->  shared memory  <--(frames, haptics)--  driver/runtime
//
// Frames travel through D3D11 shared textures (NT handles, keyed mutex) that
// the streamer creates and names; producers open them by name, render both
// eyes side by side, and mark the slot ready. Small scalar state uses a
// seqlock so neither side ever blocks the other.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace rcvr {

inline constexpr wchar_t kSharedMemoryName[] = L"Local\\RinCastVRBridge.v1";
inline constexpr wchar_t kFrameReadyEventName[] = L"Local\\RinCastVRFrameReady.v1";
inline constexpr wchar_t kTrackingEventName[] = L"Local\\RinCastVRTracking.v1";
inline constexpr uint32_t kMagic = 0x42564352;  // 'RCVB'
inline constexpr uint32_t kVersion = 4;
inline constexpr int kFrameSlots = 3;
inline constexpr int kHapticsRing = 32;
inline constexpr int kBodyTrackers = 10;  // rc::kBodyTrackerCount
inline constexpr int kHandBones = 31;     // rc::kHandBoneCount

enum class ProducerKind : uint32_t { None = 0, SteamVR = 1, OpenXR = 2 };

enum SlotState : LONG { kSlotFree = 0, kSlotWriting = 1, kSlotReady = 2, kSlotReading = 3 };

#pragma pack(push, 8)

struct BridgePose {
    float orientation[4];  // x y z w
    float position[3];
    float linearVelocity[3];
    float angularVelocity[3];
};

struct BridgeController {
    uint32_t flags;     // rc::ControllerFlags
    uint32_t buttons;   // rc::ControllerButtons
    float trigger;
    float grip;
    float thumbstick[2];
    BridgePose pose;    // grip pose
};

struct TrackingSnapshot {
    uint64_t sequence;               // increments per update
    int64_t receivedPcNs;            // PC QPC ns when the sample arrived
    int64_t targetDisplayPcNs;       // predicted display time, PC clock
    uint64_t headsetTargetNs;        // headset clock; echoed back with the frame
    uint32_t flags;                  // rc::TrackingFlags
    uint32_t reserved;
    BridgePose hmd;
    BridgeController controllers[2];
};

struct BodySnapshot {
    int64_t receivedPcNs;            // PC QPC ns when the sample arrived
    uint32_t validMask;              // bit per rc::BodyTracker
    uint32_t reserved;
    BridgePose poses[kBodyTrackers];
};

struct HandSnapshot {
    int64_t receivedPcNs;            // PC QPC ns when the sample arrived
    BridgePose bones[kHandBones];
};

struct DisplayConfig {
    uint32_t eyeWidth;
    uint32_t eyeHeight;
    float refreshRate;
    float ipd;                       // meters
    float fovTan[2][4];              // per eye: left, right, up, down (positive tangents)
    LUID adapterLuid;
    char headsetModel[64];
    char headsetSerial[64];
};

struct FrameSlot {
    volatile LONG state;             // SlotState
    uint32_t reserved;
    uint64_t frameIndex;
    uint64_t trackingSequence;       // tracking sample the frame was rendered with
    uint64_t headsetTargetNs;
    int64_t submitPcNs;
    BridgePose renderPose;
    float fovTan[2][4];
};

struct HapticEvent {
    uint32_t sequence;
    uint32_t hand;                   // 0 left, 1 right
    float durationS;
    float frequency;
    float amplitude;
};

struct Shared {
    uint32_t magic;
    uint32_t version;

    // ---- written by the streamer ----
    volatile LONG64 streamerHeartbeatNs;   // QPC ns, updated ~10 Hz
    volatile LONG headsetConnected;        // a headset is connected and in (or entering) VR mode
    volatile LONG configGeneration;        // bumped whenever config / textures change
    DisplayConfig config;
    wchar_t textureNames[kFrameSlots][64];
    uint32_t textureWidth;                 // = 2 * eyeWidth
    uint32_t textureHeight;
    uint32_t textureFormat;                // DXGI_FORMAT
    volatile LONG64 vsyncEpochNs;          // the driver's vsyncs: this + k * period, steered to the headset's display
    volatile LONG recenterCount;

    volatile LONG64 trackingSeqlock;       // odd while writing
    TrackingSnapshot tracking;

    // ---- written by the producer ----
    volatile LONG producerKind;            // ProducerKind
    volatile LONG producerPid;
    volatile LONG64 producerHeartbeatNs;
    volatile LONG64 latestReadySlot;       // index | (frameIndex << 8), -1 = none
    FrameSlot slots[kFrameSlots];
    char applicationName[128];             // running VR app, for the UI

    volatile LONG hapticsWrite;            // producer increments after writing
    HapticEvent haptics[kHapticsRing];

    // ---- written by the streamer ----
    volatile LONG64 bodySeqlock;           // odd while writing
    BodySnapshot body;
    volatile LONG64 handSeqlock[2];        // odd while writing
    HandSnapshot hands[2];
};

#pragma pack(pop)

// ---------------------------------------------------------------------------

inline void WriteTracking(Shared* s, const TrackingSnapshot& snap) {
    InterlockedIncrement64(&s->trackingSeqlock);  // -> odd
    MemoryBarrier();
    std::memcpy(&s->tracking, &snap, sizeof(snap));
    MemoryBarrier();
    InterlockedIncrement64(&s->trackingSeqlock);  // -> even
}

inline bool ReadTracking(const Shared* s, TrackingSnapshot& out) {
    for (int attempt = 0; attempt < 64; attempt++) {
        const LONG64 before = s->trackingSeqlock;
        if (before & 1) {
            YieldProcessor();
            continue;
        }
        MemoryBarrier();
        std::memcpy(&out, const_cast<const TrackingSnapshot*>(&s->tracking), sizeof(out));
        MemoryBarrier();
        if (s->trackingSeqlock == before) return before != 0;
    }
    return false;
}

inline void WriteBody(Shared* s, const BodySnapshot& snap) {
    InterlockedIncrement64(&s->bodySeqlock);  // -> odd
    MemoryBarrier();
    std::memcpy(&s->body, &snap, sizeof(snap));
    MemoryBarrier();
    InterlockedIncrement64(&s->bodySeqlock);  // -> even
}

inline bool ReadBody(const Shared* s, BodySnapshot& out) {
    for (int attempt = 0; attempt < 64; attempt++) {
        const LONG64 before = s->bodySeqlock;
        if (before & 1) {
            YieldProcessor();
            continue;
        }
        MemoryBarrier();
        std::memcpy(&out, const_cast<const BodySnapshot*>(&s->body), sizeof(out));
        MemoryBarrier();
        if (s->bodySeqlock == before) return before != 0;
    }
    return false;
}

inline void WriteHand(Shared* s, int hand, const HandSnapshot& snap) {
    InterlockedIncrement64(&s->handSeqlock[hand]);  // -> odd
    MemoryBarrier();
    std::memcpy(&s->hands[hand], &snap, sizeof(snap));
    MemoryBarrier();
    InterlockedIncrement64(&s->handSeqlock[hand]);  // -> even
}

inline bool ReadHand(const Shared* s, int hand, HandSnapshot& out) {
    for (int attempt = 0; attempt < 64; attempt++) {
        const LONG64 before = s->handSeqlock[hand];
        if (before & 1) {
            YieldProcessor();
            continue;
        }
        MemoryBarrier();
        std::memcpy(&out, const_cast<const HandSnapshot*>(&s->hands[hand]), sizeof(out));
        MemoryBarrier();
        if (s->handSeqlock[hand] == before) return before != 0;
    }
    return false;
}

// Keyed-mutex acquire that also accepts ownership inherited from a crashed
// process (WAIT_ABANDONED), which otherwise wedges the texture forever.
template <typename KeyedMutex>
inline bool AcquireKeyed(KeyedMutex* mutex, DWORD timeoutMs) {
    const HRESULT hr = mutex->AcquireSync(0, timeoutMs);
    return hr == S_OK || hr == static_cast<HRESULT>(WAIT_ABANDONED);
}

inline int64_t QpcNowNs() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return int64_t(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (c.QuadPart / freq) * 1000000000LL + (c.QuadPart % freq) * 1000000000LL / freq;
}

// Producer side helper: maps the bridge (returns nullptr if the streamer is
// not running).
class BridgeClient {
   public:
    ~BridgeClient() { Close(); }

    bool Open() {
        if (m_shared) return true;
        m_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kSharedMemoryName);
        if (!m_mapping) return false;
        m_shared = static_cast<Shared*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (!m_shared || m_shared->magic != kMagic || m_shared->version != kVersion) {
            Close();
            return false;
        }
        m_frameReady = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kFrameReadyEventName);
        m_trackingEvent = OpenEventW(SYNCHRONIZE, FALSE, kTrackingEventName);
        return true;
    }

    void Close() {
        if (m_shared) UnmapViewOfFile(m_shared);
        if (m_mapping) CloseHandle(m_mapping);
        if (m_frameReady) CloseHandle(m_frameReady);
        if (m_trackingEvent) CloseHandle(m_trackingEvent);
        m_shared = nullptr;
        m_mapping = m_frameReady = m_trackingEvent = nullptr;
    }

    Shared* shared() const { return m_shared; }

    bool StreamerAlive() const {
        return m_shared && QpcNowNs() - m_shared->streamerHeartbeatNs < 2'000'000'000LL;
    }

    void SignalFrame() {
        if (m_frameReady) SetEvent(m_frameReady);
    }

    // Waits for a new tracking sample (or timeout).
    void WaitTracking(DWORD timeoutMs) {
        if (m_trackingEvent) WaitForSingleObject(m_trackingEvent, timeoutMs);
    }

    // Claims a slot for writing: a free one, or the oldest ready one the
    // streamer hasn't picked up yet (newest frame wins). Returns -1 if all busy.
    int AcquireSlot() {
        if (!m_shared) return -1;
        for (int i = 0; i < kFrameSlots; i++) {
            if (InterlockedCompareExchange(&m_shared->slots[i].state, kSlotWriting, kSlotFree) == kSlotFree) return i;
        }
        int oldest = -1;
        uint64_t oldestIndex = ~0ull;
        for (int i = 0; i < kFrameSlots; i++) {
            if (m_shared->slots[i].state == kSlotReady && m_shared->slots[i].frameIndex < oldestIndex) {
                oldest = i;
                oldestIndex = m_shared->slots[i].frameIndex;
            }
        }
        if (oldest >= 0 &&
            InterlockedCompareExchange(&m_shared->slots[oldest].state, kSlotWriting, kSlotReady) == kSlotReady)
            return oldest;
        return -1;
    }

    void PublishSlot(int slot) {
        MemoryBarrier();
        InterlockedExchange(&m_shared->slots[slot].state, kSlotReady);
        InterlockedExchange64(&m_shared->latestReadySlot,
                              LONG64(slot) | LONG64(m_shared->slots[slot].frameIndex << 8));
        SignalFrame();
    }

    void AbandonSlot(int slot) { InterlockedExchange(&m_shared->slots[slot].state, kSlotFree); }

    void PushHaptic(uint32_t hand, float duration, float frequency, float amplitude) {
        if (!m_shared) return;
        const LONG index = InterlockedIncrement(&m_shared->hapticsWrite) - 1;
        HapticEvent& e = m_shared->haptics[index % kHapticsRing];
        e.hand = hand;
        e.durationS = duration;
        e.frequency = frequency;
        e.amplitude = amplitude;
        MemoryBarrier();
        e.sequence = uint32_t(index + 1);
    }

   private:
    HANDLE m_mapping = nullptr;
    HANDLE m_frameReady = nullptr;
    HANDLE m_trackingEvent = nullptr;
    Shared* m_shared = nullptr;
};

}  // namespace rcvr
