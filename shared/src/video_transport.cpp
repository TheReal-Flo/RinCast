#include "rc/video_transport.h"

#include <algorithm>
#include <cstring>

#include "rc/platform.h"

namespace rc {

namespace {
inline bool FrameNewer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }
}  // namespace

size_t VideoPacketizer::SendFrame(uint8_t streamId, VideoCodec codec, uint8_t frameFlags, uint32_t frameIndex,
                                  const void* metadata, uint16_t metadataSize, const uint8_t* bitstream,
                                  size_t bitstreamSize) {
    m_frame.resize(2 + metadataSize + bitstreamSize);
    std::memcpy(m_frame.data(), &metadataSize, 2);
    if (metadataSize) std::memcpy(m_frame.data() + 2, metadata, metadataSize);
    if (bitstreamSize) std::memcpy(m_frame.data() + 2 + metadataSize, bitstream, bitstreamSize);

    const size_t fragPayload = m_connection->MaxPayload() - sizeof(VideoFragmentHeader);
    const size_t total = m_frame.size();
    const size_t count = (total + fragPayload - 1) / fragPayload;
    if (count > 0xFFFF) {
        RC_LOGE("Video frame too large (%zu bytes)", total);
        return 0;
    }

    uint8_t group = 0;
    if (m_fecPercent > 0) group = uint8_t(std::clamp(100 / m_fecPercent, 2, 64));

    VideoFragmentHeader h{};
    h.streamId = streamId;
    h.codec = uint8_t(codec);
    h.frameFlags = frameFlags;
    h.fecGroupSize = group;
    h.frameIndex = frameIndex;
    h.fragmentCount = uint16_t(count);
    h.frameSize = uint32_t(total);

    std::vector<uint8_t> parity;
    if (group) parity.assign(fragPayload, 0);
    size_t sent = 0;
    const size_t lastSize = total - (count - 1) * fragPayload;
    const int64_t paceStart = m_pacingKbps ? NowNs() : 0;
    const double nsPerByte = m_pacingKbps ? 8e6 / m_pacingKbps : 0;

    for (size_t i = 0; i < count; i++) {
        const size_t offset = i * fragPayload;
        const size_t len = std::min(fragPayload, total - offset);
        h.fragmentIndex = uint16_t(i);
        if (m_pacingKbps) {
            const int64_t due = paceStart + int64_t(double(offset) * nsPerByte);
            if (due - NowNs() > 1'500'000) SleepMs(1);  // small bursts are fine; timers are coarse
        }
        m_connection->Send2(PacketType::Video, &h, sizeof(h), m_frame.data() + offset, len);
        sent++;
        if (group) {
            for (size_t b = 0; b < len; b++) parity[b] ^= m_frame[offset + b];
            const bool groupEnd = ((i + 1) % group == 0) || (i + 1 == count);
            if (groupEnd) {
                VideoParityHeader ph{};
                ph.streamId = streamId;
                ph.fecGroupSize = group;
                ph.groupIndex = uint16_t(i / group);
                ph.frameIndex = frameIndex;
                ph.fragmentCount = uint16_t(count);
                ph.lastFragmentSize = uint16_t(lastSize);
                ph.frameSize = uint32_t(total);
                // Only send parity when the group has more than one member.
                const size_t groupMembers = i - size_t(ph.groupIndex) * group + 1;
                if (groupMembers > 1) {
                    m_connection->Send2(PacketType::VideoParity, &ph, sizeof(ph), parity.data(), fragPayload);
                    sent++;
                }
                std::fill(parity.begin(), parity.end(), uint8_t(0));
            }
        }
    }
    return sent;
}

void VideoReassembler::Reset() {
    std::lock_guard lock(m_mutex);
    m_pending.clear();
    m_hasComplete = false;
}

ReassemblyStats VideoReassembler::TakeStats() {
    std::lock_guard lock(m_mutex);
    ReassemblyStats s = m_stats;
    m_stats = {};
    return s;
}

void VideoReassembler::HandleFragment(const uint8_t* payload, size_t size) {
    if (size < sizeof(VideoFragmentHeader)) return;
    VideoFragmentHeader h;
    std::memcpy(&h, payload, sizeof(h));
    const uint8_t* data = payload + sizeof(h);
    const size_t len = size - sizeof(h);
    if (h.fragmentCount == 0 || h.fragmentIndex >= h.fragmentCount) return;

    std::unique_lock lock(m_mutex);
    if (m_hasComplete && !FrameNewer(h.frameIndex, m_lastComplete)) return;  // stale
    m_stats.fragmentsReceived++;
    m_stats.bytesReceived += size;

    auto& a = m_pending[h.frameIndex];
    if (a.fragmentCount == 0) {
        a.streamId = h.streamId;
        a.fragmentCount = h.fragmentCount;
        a.fecGroupSize = h.fecGroupSize;
        a.frameSize = h.frameSize;
        a.fragments.resize(h.fragmentCount);
        a.present.assign(h.fragmentCount, false);
        a.firstNs = NowNs();
    }
    // Not only for a new assembly: a parity packet may have created it (reordering).
    a.codec = VideoCodec(h.codec);
    a.flags = h.frameFlags;
    if (h.fragmentIndex == 0 || h.fragmentIndex + 1 < h.fragmentCount) a.fragmentPayload = std::max(a.fragmentPayload, len);
    if (a.present[h.fragmentIndex]) return;
    a.fragments[h.fragmentIndex].assign(data, data + len);
    a.present[h.fragmentIndex] = true;
    a.received++;
    if (a.fecGroupSize) TryRecover(h.frameIndex, a, uint16_t(h.fragmentIndex / a.fecGroupSize));
    TryComplete(h.frameIndex);
}

void VideoReassembler::HandleParity(const uint8_t* payload, size_t size) {
    if (size < sizeof(VideoParityHeader)) return;
    VideoParityHeader h;
    std::memcpy(&h, payload, sizeof(h));
    std::unique_lock lock(m_mutex);
    if (m_hasComplete && !FrameNewer(h.frameIndex, m_lastComplete)) return;
    auto& a = m_pending[h.frameIndex];
    if (a.fragmentCount == 0) {
        a.streamId = h.streamId;
        a.fragmentCount = h.fragmentCount;
        a.fecGroupSize = h.fecGroupSize;
        a.frameSize = h.frameSize;
        a.fragments.resize(h.fragmentCount);
        a.present.assign(h.fragmentCount, false);
        a.firstNs = NowNs();
    }
    a.fragmentPayload = std::max(a.fragmentPayload, size - sizeof(h));
    a.parity[h.groupIndex].assign(payload + sizeof(h), payload + size);
    TryRecover(h.frameIndex, a, h.groupIndex);
    TryComplete(h.frameIndex);
}

void VideoReassembler::TryRecover(uint32_t frameIndex, Assembly& a, uint16_t group) {
    auto it = a.parity.find(group);
    if (it == a.parity.end() || !a.fecGroupSize) return;
    const size_t first = size_t(group) * a.fecGroupSize;
    const size_t last = std::min<size_t>(first + a.fecGroupSize, a.fragmentCount);
    int missing = -1;
    for (size_t i = first; i < last; i++) {
        if (!a.present[i]) {
            if (missing >= 0) return;  // more than one missing: can't recover
            missing = int(i);
        }
    }
    if (missing < 0) return;
    std::vector<uint8_t> rec = it->second;
    for (size_t i = first; i < last; i++) {
        if (int(i) == missing) continue;
        const auto& f = a.fragments[i];
        for (size_t b = 0; b < f.size() && b < rec.size(); b++) rec[b] ^= f[b];
    }
    size_t len = a.fragmentPayload;
    if (size_t(missing) + 1 == a.fragmentCount) {
        len = a.frameSize - (size_t(a.fragmentCount) - 1) * a.fragmentPayload;
    }
    rec.resize(std::min(len, rec.size()));
    a.fragments[missing] = std::move(rec);
    a.present[missing] = true;
    a.received++;
    m_stats.fragmentsRecovered++;
    (void)frameIndex;
}

void VideoReassembler::TryComplete(uint32_t frameIndex) {
    auto it = m_pending.find(frameIndex);
    if (it == m_pending.end()) return;
    auto& a = it->second;
    if (a.received != a.fragmentCount || a.fragmentCount == 0) return;
    // Codec/flags are only known from data fragments; a frame fully rebuilt from parity
    // alone (single fragment groups never have parity) can't happen, so this is safe.

    std::vector<uint8_t> frame;
    frame.reserve(a.frameSize);
    for (auto& f : a.fragments) frame.insert(frame.end(), f.begin(), f.end());
    if (frame.size() < 2) {
        m_pending.erase(it);
        return;
    }
    uint16_t metaSize;
    std::memcpy(&metaSize, frame.data(), 2);
    if (2u + metaSize > frame.size()) {
        m_pending.erase(it);
        return;
    }

    ReceivedFrame out;
    out.streamId = a.streamId;
    out.codec = a.codec;
    out.flags = a.flags;
    out.frameIndex = frameIndex;
    out.metadata.assign(frame.begin() + 2, frame.begin() + 2 + metaSize);
    out.bitstream.assign(frame.begin() + 2 + metaSize, frame.end());
    out.firstFragmentNs = a.firstNs;
    out.completeNs = NowNs();
    m_stats.spreadMsSum += NsToMs(out.completeNs - out.firstFragmentNs);

    // Everything older than this frame is now useless; count unfinished ones as lost.
    for (auto p = m_pending.begin(); p != m_pending.end();) {
        if (!FrameNewer(p->first, frameIndex) && p->first != frameIndex) {
            m_stats.framesLost++;
            m_stats.fragmentsLost += p->second.fragmentCount - p->second.received;
            m_lostSinceExpire = true;
            p = m_pending.erase(p);
        } else {
            ++p;
        }
    }
    m_pending.erase(frameIndex);
    // Detect gaps in frame numbering (whole frames lost without a single fragment).
    if (m_hasComplete && frameIndex - m_lastComplete > 1) {
        m_stats.framesLost += frameIndex - m_lastComplete - 1;
        m_lostSinceExpire = true;
    }
    m_lastComplete = frameIndex;
    m_hasComplete = true;
    m_stats.framesComplete++;

    auto handler = m_handler;
    m_mutex.unlock();
    handler(std::move(out));
    m_mutex.lock();
}

bool VideoReassembler::Expire(int64_t nowNs, int64_t timeoutNs) {
    std::lock_guard lock(m_mutex);
    for (auto p = m_pending.begin(); p != m_pending.end();) {
        if (nowNs - p->second.firstNs > timeoutNs) {
            m_stats.framesLost++;
            m_stats.fragmentsLost += p->second.fragmentCount - p->second.received;
            m_lostSinceExpire = true;
            p = m_pending.erase(p);
        } else {
            ++p;
        }
    }
    const bool lost = m_lostSinceExpire;
    m_lostSinceExpire = false;
    return lost;
}

}  // namespace rc
