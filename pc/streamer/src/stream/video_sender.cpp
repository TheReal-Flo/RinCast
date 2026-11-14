#include "video_sender.h"

#include <algorithm>

#include "rc/platform.h"

namespace rcs {

VideoSender::VideoSender(rc::Connection* connection, uint8_t streamId, rc::VideoCodec codec, uint32_t maxBitrateKbps,
                         int fecPercent, bool adaptive)
    : m_connection(connection),
      m_packetizer(connection),
      m_streamId(streamId),
      m_codec(codec),
      m_targetKbps(maxBitrateKbps),
      m_maxKbps(maxBitrateKbps),
      m_adaptive(adaptive) {
    m_packetizer.SetFecPercent(fecPercent);
    m_windowStartNs = rc::NowNs();
}

void VideoSender::SetMaxBitrate(uint32_t kbps) {
    m_maxKbps = kbps;
    if (!m_adaptive || m_targetKbps > kbps) m_targetKbps = kbps;
}

void VideoSender::Send(const EncodedFrame& frame, const void* metadata, uint16_t metadataSize, uint8_t extraFlags,
                       double encodeMs, bool lossAnswer) {
    uint8_t flags = extraFlags;
    if (frame.keyframe) flags |= rc::kVideoKeyframe;
    const bool recovery = lossAnswer && !frame.keyframe;
    if (recovery) flags |= rc::kVideoRecovery;
    const uint32_t index = ++m_frameIndex;
    {
        std::lock_guard lock(m_lossMutex);
        m_sent[index % std::size(m_sent)] = {index, frame.timestamp};
        if (frame.keyframe) m_lastKeyframe = index;
        if (frame.keyframe || recovery) {
            m_lastRecovery = index;
            m_lastRecoveryNs = rc::NowNs();
        }
    }
    if (m_pace) m_packetizer.SetPacingKbps(uint32_t(m_targetKbps * 1.5));
    m_packetizer.SendFrame(m_streamId, m_codec, flags, index, metadata, metadataSize, frame.data.data(),
                           frame.data.size());

    std::lock_guard lock(m_statsMutex);
    m_windowFrames++;
    m_windowBytes += frame.data.size();
    m_windowEncodeMs += encodeMs;
    const int64_t now = rc::NowNs();
    const double elapsed = double(now - m_windowStartNs) / 1e9;
    if (elapsed >= 1.0) {
        m_stats.fps = m_windowFrames / elapsed;
        m_stats.mbps = double(m_windowBytes) * 8 / 1e6 / elapsed;
        m_stats.encodeMs = m_windowFrames ? m_windowEncodeMs / m_windowFrames : 0;
        m_stats.bitrateKbps = m_targetKbps;
        m_windowFrames = 0;
        m_windowBytes = 0;
        m_windowEncodeMs = 0;
        m_windowStartNs = now;
    }
}

bool VideoSender::WantsFrame() {
    if (m_keyframeRequested) return true;
    std::lock_guard lock(m_lossMutex);
    return m_lossPending;
}

bool VideoSender::BeginFrame(VideoEncoder& encoder) {
    m_nextIsRecovery = false;
    bool keyframe = m_keyframeRequested.exchange(false);
    uint32_t good;
    uint64_t timestamp = 0;
    bool known = false;
    {
        std::lock_guard lock(m_lossMutex);
        if (!m_lossPending) return keyframe;
        m_lossPending = false;
        good = m_lossGoodFrame;
        const SentFrame& s = m_sent[good % std::size(m_sent)];
        // Frames before the newest keyframe can't be referenced any more.
        known = good != 0 && s.index == good && good >= m_lastKeyframe;
        timestamp = s.timestamp;
    }
    if (keyframe) return true;
    if (known && encoder.InvalidateAfter(timestamp)) {
        m_nextIsRecovery = true;
        return false;
    }
    return true;
}

void VideoSender::OnFeedback(const rc::VideoFeedbackPayload& fb) {
    if (fb.requestKeyframe) {
        std::lock_guard lock(m_lossMutex);
        // The headset asks again until it gets something to resume from. If a keyframe
        // or recovery frame is already on its way (it hasn't completed anything that
        // new yet), this request predates it: answering again would only send another
        // one. If that frame got lost, the headset completes newer frames, or the
        // timeout passes, and the request counts again.
        const bool inFlight = m_lastRecovery != 0 && int32_t(fb.lastCompleteFrame - m_lastRecovery) < 0 &&
                              rc::NowNs() - m_lastRecoveryNs < 300'000'000LL;
        if (!inFlight) {
            m_lossPending = true;
            m_lossGoodFrame = fb.lastGoodFrame;
        }
    }
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.framesLost += fb.framesLost;
        m_stats.clientDecodeMs = fb.decodeMs;
        m_stats.clientLatencyMs = fb.totalLatencyMs;
    }
    if (!m_adaptive) return;

    // Congestion control. When the stream is faster than the path (an internet upload,
    // weak Wi-Fi), the excess doesn't get lost right away: it waits in some buffer on
    // the way (router, modem, access point) and every frame arrives later than the one
    // before, up to hundreds of ms. So the main signal is the queueing delay the headset
    // measures; loss and arrival spread (for headsets that don't measure it) back it up.
    const int64_t now = rc::NowNs();
    const uint32_t total = fb.fragmentsReceived + fb.fragmentsLost;
    const double lossRate = total ? double(fb.fragmentsLost) / total : 0;
    const bool measured = fb.queueDelayMs >= 0;
    if (measured) m_queueMs = m_queueMs < 0 ? fb.queueDelayMs : m_queueMs * 0.6 + fb.queueDelayMs * 0.4;
    if (fb.receivedKbps) m_receivedKbps = m_receivedKbps ? m_receivedKbps * 0.7 + fb.receivedKbps * 0.3 : fb.receivedKbps;
    if (m_baselineNetworkMs < 0 || fb.networkMs < m_baselineNetworkMs) m_baselineNetworkMs = fb.networkMs;
    m_baselineNetworkMs += 0.01;  // slowly forget
    {
        std::lock_guard lock(m_statsMutex);
        m_stats.queueMs = m_queueMs;
    }
    // Without frames in this report (a still desktop) there is nothing to judge by.
    const double q = measured ? m_queueMs : -1;

    const bool queueHigh = q > 40;  // well past what the path carries
    // Wi-Fi and mobile links lose the odd packet without being full: only sustained or
    // heavy loss counts. (Headsets that measure no delay have nothing better than lost frames.)
    m_lossAvg = m_lossAvg * 0.8 + lossRate * 0.2;
    const bool lossy = m_lossAvg > 0.02 || lossRate > 0.2;
    const bool congested = q > 10 || lossy || (!measured && (fb.framesLost > 0 || fb.networkMs > m_baselineNetworkMs + 8.0));
    const uint32_t minKbps = std::max<uint32_t>(m_maxKbps / 20, 2000);
    uint32_t target = m_targetKbps;
    // A queue takes a moment to drain after a cut: only cut again once that had time,
    // or if the queue is still growing.
    const bool mayDecrease = now - m_lastDecreaseNs > 500'000'000LL ||
                             (now - m_lastDecreaseNs > 150'000'000LL && q > m_queueAtDecreaseMs * 1.5);
    // Already sending less than arrives: the backlog is draining, so a still high queue
    // is no reason to cut further (only new loss is).
    const bool draining = m_receivedKbps > 0 && target <= m_receivedKbps * 0.85 && !lossy;
    if (congested && mayDecrease && !draining) {
        double next = target * 0.85;
        // What arrives is what the path carries right now: go below it so the queue empties
        // (but at most halve per step: the arrival rate lags behind and starts out low).
        if (queueHigh && m_receivedKbps > 0) next = std::max(target * 0.5, std::min(target * 0.7, m_receivedKbps * 0.8));
        target = std::max(minKbps, uint32_t(next));
        m_lastDecreaseNs = now;
        m_lastIncreaseNs = now;
        m_queueAtDecreaseMs = std::max(q, 1.0);
        RC_LOGI("Stream %u: %s, bitrate %u kbps (queue %.0f ms, loss %.1f %%, arriving %.0f kbps)", m_streamId,
                queueHigh ? "path overloaded" : "congestion", target, q, lossRate * 100, m_receivedKbps);
    } else if (!congested && q < 5 && now - m_lastIncreaseNs > 500'000'000LL &&
               now - m_lastDecreaseNs > 2'000'000'000LL) {
        target = std::min(m_maxKbps, uint32_t(target * 1.05) + 500);
        m_lastIncreaseNs = now;
    }
    m_targetKbps = target;
}

VideoSenderStats VideoSender::Stats() {
    std::lock_guard lock(m_statsMutex);
    VideoSenderStats s = m_stats;
    m_stats.framesLost = 0;
    return s;
}

}  // namespace rcs
