// An established, encrypted RinCast session over UDP.
//
// A Connection does not own the socket: the owner runs the receive loop and
// forwards datagrams coming from the peer to HandleDatagram(). This lets the
// streamer multiplex a single port and lets either side switch the remote
// address (hole punching -> relay fallback) without rebuilding state.
#pragma once

#include <atomic>
#include <bitset>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "rc/crypto.h"
#include "rc/protocol.h"
#include "rc/socket.h"

namespace rc {

struct ConnectionStats {
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    uint64_t packetsSent = 0;
    uint64_t packetsReceived = 0;
    uint64_t packetsRejected = 0;
    uint64_t controlRetransmits = 0;
    double rttMs = 0;
    double rttVarMs = 0;
};

class Connection {
   public:
    using ControlHandler = std::function<void(const std::string& json)>;
    using PacketHandler = std::function<void(PacketType type, const uint8_t* payload, size_t size)>;

    Connection(UdpSocket* socket, const NetAddress& remote, uint32_t sessionId, const crypto::SessionKeys& keys,
               bool isServer, size_t maxDatagram);
    ~Connection();

    void SetControlHandler(ControlHandler h) { m_onControl = std::move(h); }
    void SetPacketHandler(PacketHandler h) { m_onPacket = std::move(h); }

    // Unreliable, encrypted datagram. Thread-safe.
    bool Send(PacketType type, const void* payload, size_t size);
    // Same, but concatenates two buffers (header + body) without a copy by the caller.
    bool Send2(PacketType type, const void* a, size_t aSize, const void* b, size_t bSize);

    // Reliable, ordered JSON message. Thread-safe.
    void SendControl(const std::string& json);
    // Chunks waiting for window space: a screenshot burst can back this up.
    size_t SendQueueChunks() const;

    // Feed a datagram received from the peer. Returns false if it was not
    // authentic (wrong session or MAC failure).
    bool HandleDatagram(const uint8_t* data, size_t size, const NetAddress& from);

    // Drives retransmissions and keepalive pings. Call every few milliseconds.
    void Tick(int64_t nowNs);

    void SendDisconnect();

    size_t MaxPayload() const { return m_maxDatagram - sizeof(PacketHeader) - kMacSize; }
    uint32_t SessionId() const { return m_sessionId; }
    NetAddress Remote() const;
    void SetRemote(const NetAddress& addr);

    int64_t LastReceiveNs() const { return m_lastReceiveNs.load(); }
    bool PeerDisconnected() const { return m_peerDisconnected.load(); }
    ConnectionStats Stats() const;

    // Estimated (remote clock - local clock) in ns, valid once HasClockOffset().
    int64_t ClockOffsetNs() const { return m_clockOffsetNs.load(); }
    bool HasClockOffset() const { return m_hasClockOffset.load(); }
    int64_t RemoteToLocalNs(int64_t remoteNs) const { return remoteNs - m_clockOffsetNs.load(); }
    int64_t LocalToRemoteNs(int64_t localNs) const { return localNs + m_clockOffsetNs.load(); }

   private:
    struct PendingChunk {
        uint32_t chunkSeq;
        std::vector<uint8_t> payload;  // ControlChunkHeader + data
        int64_t lastSendNs = 0;
        int retries = 0;
    };

    bool SendRaw(PacketType type, const void* a, size_t aSize, const void* b, size_t bSize);
    void HandleControlChunk(const uint8_t* payload, size_t size);
    void HandleAck(const AckPayload& ack);
    void SendAck();
    bool CheckReplay(uint64_t seq);
    void HandlePong(const PongPayload& pong);

    UdpSocket* m_socket;
    mutable std::mutex m_remoteMutex;
    NetAddress m_remote;
    const uint32_t m_sessionId;
    const bool m_isServer;
    const size_t m_maxDatagram;
    crypto::Key m_sendKey;
    crypto::Key m_recvKey;
    uint8_t m_sendDirection;
    uint8_t m_recvDirection;
    std::atomic<uint64_t> m_sendSeq{1};

    // Replay window
    uint64_t m_highestSeq = 0;
    std::bitset<2048> m_window;

    ControlHandler m_onControl;
    PacketHandler m_onPacket;

    // Reliable send state
    mutable std::mutex m_controlMutex;
    uint32_t m_nextChunkSeq = 0;
    uint32_t m_nextMessageId = 0;
    std::deque<PendingChunk> m_unacked;     // in flight, ordered by seq
    std::deque<PendingChunk> m_sendQueue;   // waiting for window space

    // Reliable receive state
    uint32_t m_expectedChunk = 0;
    std::map<uint32_t, std::vector<uint8_t>> m_outOfOrder;
    std::string m_assembly;
    uint32_t m_assemblyMessage = ~0u;
    bool m_ackPending = false;

    // Timing
    std::atomic<int64_t> m_lastReceiveNs{0};
    int64_t m_lastPingNs = 0;
    uint32_t m_pingId = 0;
    double m_srttMs = 0;
    double m_rttVarMs = 0;
    double m_minRttMs = 1e9;
    int64_t m_minRttAtNs = 0;
    std::atomic<int64_t> m_clockOffsetNs{0};
    std::atomic<bool> m_hasClockOffset{false};
    std::atomic<bool> m_peerDisconnected{false};

    mutable std::mutex m_statsMutex;
    ConnectionStats m_stats;
};

}  // namespace rc
