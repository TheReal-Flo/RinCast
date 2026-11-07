// What both decoders share (MediaCodec on Android, V4L2 on Linux): the chain of frames handed
// to the hardware, the statistics and the log.
#include "video_decoder.h"

#include <map>
#include <mutex>
#include <string>

#include "rc/platform.h"

namespace rv {

const char* VideoDecoder::CodecName(rc::VideoCodec c) {
    switch (c) {
        case rc::VideoCodec::H264: return "H.264";
        case rc::VideoCodec::HEVC: return "HEVC";
        case rc::VideoCodec::AV1: return "AV1";
    }
    return "HEVC";
}

// What can go wrong on every frame is logged at most once every 5 seconds per decoder and
// reason, with how often it happened since, so a missing picture shows up in the log without
// flooding it.
void VideoDecoder::Warn(const char* what) {
    static std::mutex mutex;
    static std::map<std::string, std::pair<int64_t, uint32_t>> seen;  // last logged, count since
    const std::string key = std::to_string(uintptr_t(this)) + what;
    std::lock_guard lock(mutex);
    auto& [last, count] = seen[key];
    count++;
    const int64_t now = rc::NowNs();
    if (last && now - last < 5'000'000'000LL) return;
    RC_LOGW("Decoder %s %dx%d: %s (%u times)", CodecName(m_codec), m_width, m_height, what, count);
    last = now;
    count = 0;
}

void VideoDecoder::Submit(uint32_t frameIndex, bool keyframe, bool recovery, std::vector<uint8_t>&& bitstream,
                          std::vector<uint8_t>&& metadata, int64_t receivedNs) {
    std::lock_guard lock(m_mutex);
    if (m_hadKeyframe && !keyframe && frameIndex <= m_lastFrameIndex) return;  // stale duplicate / reordered
    // Frames are predicted from the ones before. If any frame went missing (lost on
    // the network, or arrived too late), everything after it would decode into smeared
    // garbage, so keep showing the last good picture and ask the PC right away for a
    // frame to resume from: a keyframe, or a recovery frame that only references
    // frames up to LastGoodFrame().
    if (keyframe) {
        m_hadKeyframe = true;
        m_seenKeyframe = true;
    } else if (recovery && m_hadKeyframe) {
        if (!m_seenKeyframe) RC_LOGI("Resuming at recovery frame %u", frameIndex);
        m_seenKeyframe = true;
    } else if (m_seenKeyframe && frameIndex != m_lastFrameIndex + 1) {
        RC_LOGI("Frame %u missing (got %u), waiting for a frame to resume from", m_lastFrameIndex + 1, frameIndex);
        m_seenKeyframe = false;
    }
    if (!m_seenKeyframe) {
        Warn("frame skipped while waiting for a keyframe");
        m_needKeyframe = true;
        return;
    }
    // Never build a backlog: if the decoder falls behind, drop what is queued (the
    // chain then ends at the last frame MediaCodec got; a keyframe starts a new one).
    if (m_queue.size() > 6) {
        m_queue.clear();
        if (!keyframe) {
            m_lastFrameIndex = m_lastQueued;
            m_seenKeyframe = false;
            m_needKeyframe = true;
            return;
        }
    }
    m_lastFrameIndex = frameIndex;
    DecodedFrameInfo info;
    info.frameIndex = frameIndex;
    info.metadata = std::move(metadata);
    info.receivedNs = receivedNs;
    m_infos[int64_t(frameIndex)] = std::move(info);
    while (m_infos.size() > 64) m_infos.erase(m_infos.begin());
    m_queue.push_back({frameIndex, keyframe, std::move(bitstream), receivedNs});
    m_cv.notify_one();
}

uint32_t VideoDecoder::LastGoodFrame() {
    std::lock_guard lock(m_mutex);
    return m_hadKeyframe ? m_lastFrameIndex : 0;
}

double VideoDecoder::AverageDecodeMs() {
    std::lock_guard lock(m_statsMutex);
    const double avg = m_decodeCount ? m_decodeMsSum / m_decodeCount : 0;
    m_decodeMsSum = 0;
    m_decodeCount = 0;
    return avg;
}

}  // namespace rv
