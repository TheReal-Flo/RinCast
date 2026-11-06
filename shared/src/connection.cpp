#include "rc/connection.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rc/platform.h"

namespace rc {

namespace {
constexpr size_t kMaxInFlightChunks = 512;
constexpr size_t kMaxOutOfOrder = 2048;
constexpr int64_t kPingIntervalNs = 250'000'000;
constexpr uint8_t kDirClientToServer = 0x43;  // 'C'
constexpr uint8_t kDirServerToClient = 0x53;  // 'S'

inline bool SeqLess(uint32_t a, uint32_t b) { return int32_t(a - b) < 0; }
}  // namespace

Connection::Connection(UdpSocket* socket, const NetAddress& remote, uint32_t sessionId,
                       const crypto::SessionKeys& keys, bool isServer, size_t maxDatagram)
    : m_socket(socket),
      m_remote(remote),
      m_sessionId(sessionId),
      m_isServer(isServer),
      m_maxDatagram(std::min(maxDatagram, kMaxDatagram)) {
    m_sendKey = isServer ? keys.serverToClient : keys.clientToServer;
    m_recvKey = isServer ? keys.clientToServer : keys.serverToClient;
    m_sendDirection = isServer ? kDirServerToClient : kDirClientToServer;
    m_recvDirection = isServer ? kDirClientToServer : kDirServerToClient;
    m_lastReceiveNs = NowNs();
}

Connection::~Connection() {
    crypto::Wipe(m_sendKey.data(), m_sendKey.size());
    crypto::Wipe(m_recvKey.data(), m_recvKey.size());
}

NetAddress Connection::Remote() const {
    std::lock_guard lock(m_remoteMutex);
    return m_remote;
}

void Connection::SetRemote(const NetAddress& addr) {
    std::lock_guard lock(m_remoteMutex);
    m_remote = addr;
}

ConnectionStats Connection::Stats() const {
    std::lock_guard lock(m_statsMutex);
    return m_stats;
}

bool Connection::Send(PacketType type, const void* payload, size_t size) {
    return SendRaw(type, payload, size, nullptr, 0);
}

bool Connection::Send2(PacketType type, const void* a, size_t aSize, const void* b, size_t bSize) {
    return SendRaw(type, a, aSize, b, bSize);
}

bool Connection::SendRaw(PacketType type, const void* a, size_t aSize, const void* b, size_t bSize) {
    const size_t plainSize = aSize + bSize;
    if (sizeof(PacketHeader) + plainSize + kMacSize > m_maxDatagram) {
        RC_LOGE("Packet too large (%zu bytes, type %d)", plainSize, int(type));
        return false;
    }
    uint8_t plain[kMaxDatagram];
    uint8_t datagram[kMaxDatagram];
    if (aSize) std::memcpy(plain, a, aSize);
    if (bSize) std::memcpy(plain + aSize, b, bSize);

    PacketHeader header{};
    header.version = uint8_t(kProtocolVersion);
    header.type = uint8_t(type);
    header.flags = kFlagEncrypted;
    header.sessionId = m_sessionId;
    header.sequence = m_sendSeq.fetch_add(1);
    std::memcpy(datagram, &header, sizeof(header));
    crypto::Seal(m_sendKey, header.sequence, m_sendDirection, datagram, sizeof(header), plain, plainSize,
                 datagram + sizeof(header));
    const size_t total = sizeof(header) + plainSize + kMacSize;
    const bool ok = m_socket->SendTo(Remote(), datagram, total);
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.bytesSent += total;
        m_stats.packetsSent++;
    }
    return ok;
}

void Connection::SendControl(const std::string& json) {
    const size_t chunkData = MaxPayload() - sizeof(ControlChunkHeader);
    const size_t count = std::max<size_t>(1, (json.size() + chunkData - 1) / chunkData);
    if (count > 0xFFFF) {
        RC_LOGE("Control message too large: %zu bytes", json.size());
        return;
    }
    std::lock_guard lock(m_controlMutex);
    const uint32_t messageId = m_nextMessageId++;
    for (size_t i = 0; i < count; i++) {
        PendingChunk chunk;
        chunk.chunkSeq = m_nextChunkSeq++;
        ControlChunkHeader h{};
        h.chunkSeq = chunk.chunkSeq;
        h.messageId = messageId;
        h.chunkIndex = uint16_t(i);
        h.chunkCount = uint16_t(count);
        const size_t offset = i * chunkData;
        const size_t len = std::min(chunkData, json.size() - std::min(json.size(), offset));
        chunk.payload.resize(sizeof(h) + len);
        std::memcpy(chunk.payload.data(), &h, sizeof(h));
        if (len) std::memcpy(chunk.payload.data() + sizeof(h), json.data() + offset, len);
        m_sendQueue.push_back(std::move(chunk));
    }
    const int64_t now = NowNs();
    while (!m_sendQueue.empty() && m_unacked.size() < kMaxInFlightChunks) {
        auto chunk = std::move(m_sendQueue.front());
        m_sendQueue.pop_front();
        chunk.lastSendNs = now;
        SendRaw(PacketType::Control, chunk.payload.data(), chunk.payload.size(), nullptr, 0);
        m_unacked.push_back(std::move(chunk));
    }
}

void Connection::SendDisconnect() {
    // Unreliable; send a few times for good measure.
    for (int i = 0; i < 3; i++) Send(PacketType::Disconnect, nullptr, 0);
}

bool Connection::CheckReplay(uint64_t seq) {
    const size_t window = m_window.size();
    if (seq > m_highestSeq) {
        const uint64_t shift = seq - m_highestSeq;
        if (shift >= window) {
            m_window.reset();
        } else {
            m_window <<= size_t(shift);
        }
        m_window.set(0);
        m_highestSeq = seq;
        return true;
    }
    const uint64_t back = m_highestSeq - seq;
    if (back >= window) return false;
    if (m_window.test(size_t(back))) return false;
    m_window.set(size_t(back));
    return true;
}

bool Connection::HandleDatagram(const uint8_t* data, size_t size, const NetAddress& from) {
    if (size < sizeof(PacketHeader) + kMacSize) return false;
    PacketHeader header;
    std::memcpy(&header, data, sizeof(header));
    if (header.sessionId != m_sessionId || !(header.flags & kFlagEncrypted)) return false;

    uint8_t plain[kMaxDatagram];
    const size_t cipherSize = size - sizeof(header);
    if (cipherSize - kMacSize > sizeof(plain)) return false;
    if (!crypto::Open(m_recvKey, header.sequence, m_recvDirection, data, sizeof(header), data + sizeof(header),
                      cipherSize, plain)) {
        std::lock_guard lock(m_statsMutex);
        m_stats.packetsRejected++;
        return false;
    }
    if (!CheckReplay(header.sequence)) return true;  // authentic duplicate; ignore silently

    const size_t plainSize = cipherSize - kMacSize;
    m_lastReceiveNs = NowNs();
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.bytesReceived += size;
        m_stats.packetsReceived++;
    }
    // The peer roamed (NAT rebinding or relay switch): follow it.
    if (from != Remote()) {
        RC_LOGI("Peer address changed %s -> %s", Remote().ToString().c_str(), from.ToString().c_str());
        SetRemote(from);
    }

    const auto type = PacketType(header.type);
    switch (type) {
        case PacketType::Control:
            HandleControlChunk(plain, plainSize);
            break;
        case PacketType::Ack:
            if (plainSize >= sizeof(AckPayload)) {
                AckPayload ack;
                std::memcpy(&ack, plain, sizeof(ack));
                HandleAck(ack);
            }
            break;
        case PacketType::Ping:
            if (plainSize >= sizeof(PingPayload)) {
                PingPayload ping;
                std::memcpy(&ping, plain, sizeof(ping));
                PongPayload pong{ping.senderTimeNs, uint64_t(NowNs()), ping.pingId};
                Send(PacketType::Pong, &pong, sizeof(pong));
            }
            break;
        case PacketType::Pong:
            if (plainSize >= sizeof(PongPayload)) {
                PongPayload pong;
                std::memcpy(&pong, plain, sizeof(pong));
                HandlePong(pong);
            }
            break;
        case PacketType::Disconnect:
            m_peerDisconnected = true;
            break;
        default:
            if (m_onPacket) m_onPacket(type, plain, plainSize);
            break;
    }
    return true;
}

void Connection::HandleControlChunk(const uint8_t* payload, size_t size) {
    if (size < sizeof(ControlChunkHeader)) return;
    ControlChunkHeader h;
    std::memcpy(&h, payload, sizeof(h));

    std::vector<std::string> delivered;
    {
        std::lock_guard lock(m_controlMutex);
        auto deliver = [&](const uint8_t* p, size_t n) {
            ControlChunkHeader ch;
            std::memcpy(&ch, p, sizeof(ch));
            if (ch.chunkIndex == 0) {
                m_assembly.clear();
                m_assemblyMessage = ch.messageId;
            }
            if (ch.messageId != m_assemblyMessage) return;  // should not happen with in-order delivery
            m_assembly.append(reinterpret_cast<const char*>(p + sizeof(ch)), n - sizeof(ch));
            if (ch.chunkIndex + 1 == ch.chunkCount) {
                delivered.push_back(std::move(m_assembly));
                m_assembly.clear();
                m_assemblyMessage = ~0u;
            }
        };

        if (h.chunkSeq == m_expectedChunk) {
            deliver(payload, size);
            m_expectedChunk++;
            for (auto it = m_outOfOrder.find(m_expectedChunk); it != m_outOfOrder.end();
                 it = m_outOfOrder.find(m_expectedChunk)) {
                deliver(it->second.data(), it->second.size());
                m_outOfOrder.erase(it);
                m_expectedChunk++;
            }
        } else if (SeqLess(m_expectedChunk, h.chunkSeq) && m_outOfOrder.size() < kMaxOutOfOrder) {
            m_outOfOrder.emplace(h.chunkSeq, std::vector<uint8_t>(payload, payload + size));
        }
        m_ackPending = true;
    }
    SendAck();
    for (auto& message : delivered) {
        if (m_onControl) m_onControl(message);
    }
}

void Connection::SendAck() {
    AckPayload ack{};
    {
        std::lock_guard lock(m_controlMutex);
        ack.cumulative = m_expectedChunk;
        for (const auto& [seq, _] : m_outOfOrder) {
            const uint32_t delta = seq - m_expectedChunk - 1;
            if (delta < 32) ack.selectiveMask |= 1u << delta;
        }
        m_ackPending = false;
    }
    Send(PacketType::Ack, &ack, sizeof(ack));
}

void Connection::HandleAck(const AckPayload& ack) {
    std::lock_guard lock(m_controlMutex);
    m_unacked.erase(std::remove_if(m_unacked.begin(), m_unacked.end(),
                                   [&](const PendingChunk& c) {
                                       if (SeqLess(c.chunkSeq, ack.cumulative)) return true;
                                       const uint32_t delta = c.chunkSeq - ack.cumulative - 1;
                                       return delta < 32 && (ack.selectiveMask & (1u << delta));
                                   }),
                    m_unacked.end());
    const int64_t now = NowNs();
    while (!m_sendQueue.empty() && m_unacked.size() < kMaxInFlightChunks) {
        auto chunk = std::move(m_sendQueue.front());
        m_sendQueue.pop_front();
        chunk.lastSendNs = now;
        SendRaw(PacketType::Control, chunk.payload.data(), chunk.payload.size(), nullptr, 0);
        m_unacked.push_back(std::move(chunk));
    }
}

void Connection::HandlePong(const PongPayload& pong) {
    const int64_t now = NowNs();
    const double rttMs = NsToMs(now - int64_t(pong.originalSenderTimeNs));
    if (rttMs < 0 || rttMs > 5000) return;
    if (m_srttMs == 0) {
        m_srttMs = rttMs;
        m_rttVarMs = rttMs / 2;
    } else {
        m_rttVarMs = 0.75 * m_rttVarMs + 0.25 * std::fabs(m_srttMs - rttMs);
        m_srttMs = 0.875 * m_srttMs + 0.125 * rttMs;
    }
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.rttMs = m_srttMs;
        m_stats.rttVarMs = m_rttVarMs;
    }
    // Clock offset: trust samples with near-minimal RTT (least queuing asymmetry).
    // The minimum decays every 10 s so we follow route changes.
    if (now - m_minRttAtNs > 10'000'000'000LL) m_minRttMs = 1e9;
    if (rttMs <= m_minRttMs + 0.5) {
        if (rttMs < m_minRttMs) {
            m_minRttMs = rttMs;
            m_minRttAtNs = now;
        }
        const int64_t localMid = int64_t(pong.originalSenderTimeNs) + (now - int64_t(pong.originalSenderTimeNs)) / 2;
        const int64_t offset = int64_t(pong.responderTimeNs) - localMid;
        if (!m_hasClockOffset) {
            m_clockOffsetNs = offset;
            m_hasClockOffset = true;
        } else {
            m_clockOffsetNs = m_clockOffsetNs.load() + (offset - m_clockOffsetNs.load()) / 4;
        }
    }
}

void Connection::Tick(int64_t nowNs) {
    if (nowNs - m_lastPingNs >= kPingIntervalNs) {
        m_lastPingNs = nowNs;
        PingPayload ping{uint64_t(nowNs), m_pingId++};
        Send(PacketType::Ping, &ping, sizeof(ping));
    }
    double rto = m_srttMs > 0 ? m_srttMs + 4 * m_rttVarMs : 100.0;
    rto = std::clamp(rto, 15.0, 1000.0);
    const int64_t rtoNs = MsToNs(rto);

    std::vector<std::vector<uint8_t>> resend;
    {
        std::lock_guard lock(m_controlMutex);
        for (auto& chunk : m_unacked) {
            const int64_t backoff = rtoNs << std::min(chunk.retries, 4);
            if (nowNs - chunk.lastSendNs >= backoff) {
                chunk.lastSendNs = nowNs;
                chunk.retries++;
                resend.push_back(chunk.payload);
            }
        }
    }
    if (!resend.empty()) {
        std::lock_guard lock(m_statsMutex);
        m_stats.controlRetransmits += resend.size();
    }
    for (const auto& p : resend) SendRaw(PacketType::Control, p.data(), p.size(), nullptr, 0);
}

}  // namespace rc
