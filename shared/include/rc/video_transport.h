// Splits encoded video frames into datagrams (with optional XOR parity FEC)
// and reassembles them on the other side.
//
// A "frame payload" is [uint16 metadataSize][metadata bytes][bitstream]. The
// metadata is VrFrameMetadata for the VR stream and DesktopFrameMetadata for
// monitor streams.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "rc/connection.h"
#include "rc/protocol.h"

namespace rc {

class VideoPacketizer {
   public:
    explicit VideoPacketizer(Connection* connection) : m_connection(connection) {}

    // fecPercent: 0 disables FEC; otherwise one parity packet per
    // round(100 / fecPercent) data fragments (clamped to [2, 64]).
    void SetFecPercent(int fecPercent) { m_fecPercent = fecPercent; }
    // Spreads each frame's datagrams at this rate instead of sending them in one burst,
    // which shallow internet queues drop. Blocks the caller. 0 = no pacing (LAN).
    void SetPacingKbps(uint32_t kbps) { m_pacingKbps = kbps; }

    // Sends one frame. Returns number of datagrams sent.
    size_t SendFrame(uint8_t streamId, VideoCodec codec, uint8_t frameFlags, uint32_t frameIndex,
                     const void* metadata, uint16_t metadataSize, const uint8_t* bitstream, size_t bitstreamSize);

   private:
    Connection* m_connection;
    int m_fecPercent = 5;
    uint32_t m_pacingKbps = 0;
    std::vector<uint8_t> m_frame;
};

struct ReceivedFrame {
    uint8_t streamId;
    VideoCodec codec;
    uint8_t flags;
    uint32_t frameIndex;
    std::vector<uint8_t> metadata;
    std::vector<uint8_t> bitstream;
    int64_t firstFragmentNs;
    int64_t completeNs;
};

struct ReassemblyStats {
    uint32_t fragmentsReceived = 0;
    uint32_t fragmentsRecovered = 0;
    uint32_t fragmentsLost = 0;
    uint32_t framesComplete = 0;
    uint32_t framesLost = 0;
    uint64_t bytesReceived = 0;
    double spreadMsSum = 0;
};

class VideoReassembler {
   public:
    using FrameHandler = std::function<void(ReceivedFrame&& frame)>;

    explicit VideoReassembler(FrameHandler handler) : m_handler(std::move(handler)) {}

    void HandleFragment(const uint8_t* payload, size_t size);
    void HandleParity(const uint8_t* payload, size_t size);

    // Drops frames that have been pending for too long. Returns true if a frame
    // was lost since the last call (=> caller should request a keyframe).
    bool Expire(int64_t nowNs, int64_t timeoutNs);

    // Returns and resets the accumulated statistics.
    ReassemblyStats TakeStats();
    uint32_t LastCompleteFrame() const { return m_lastComplete; }
    void Reset();

   private:
    struct Assembly {
        uint8_t streamId = 0;
        VideoCodec codec = VideoCodec::HEVC;
        uint8_t flags = 0;
        uint16_t fragmentCount = 0;
        uint8_t fecGroupSize = 0;
        uint32_t frameSize = 0;
        size_t fragmentPayload = 0;  // bytes per full fragment
        std::vector<std::vector<uint8_t>> fragments;
        std::vector<bool> present;
        std::map<uint16_t, std::vector<uint8_t>> parity;  // groupIndex -> xor data
        uint16_t received = 0;
        int64_t firstNs = 0;
    };

    void TryRecover(uint32_t frameIndex, Assembly& a, uint16_t group);
    void TryComplete(uint32_t frameIndex);

    FrameHandler m_handler;
    std::map<uint32_t, Assembly> m_pending;
    uint32_t m_lastComplete = 0;
    bool m_hasComplete = false;
    bool m_lostSinceExpire = false;
    ReassemblyStats m_stats;
    std::mutex m_mutex;
};

}  // namespace rc
