#include "rc/audio_codec.h"

#include <algorithm>
#include <cstring>

#include "opus.h"
#include "rc/platform.h"

namespace rc {

AudioEncoder::~AudioEncoder() {
    if (m_enc) opus_encoder_destroy(m_enc);
}

bool AudioEncoder::Init(int sampleRate, int channels, int bitrate, bool lowDelay) {
    int err = 0;
    m_channels = channels;
    m_enc = opus_encoder_create(sampleRate, channels, lowDelay ? OPUS_APPLICATION_RESTRICTED_LOWDELAY : OPUS_APPLICATION_AUDIO,
                                &err);
    if (err != OPUS_OK || !m_enc) {
        RC_LOGE("opus_encoder_create failed: %d", err);
        return false;
    }
    opus_encoder_ctl(m_enc, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(m_enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(m_enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(m_enc, OPUS_SET_PACKET_LOSS_PERC(5));
    return true;
}

int AudioEncoder::Encode(const int16_t* pcm, int frameSamples, uint8_t* out, int capacity) {
    if (!m_enc) return -1;
    return opus_encode(m_enc, pcm, frameSamples, out, capacity);
}

AudioDecoder::~AudioDecoder() {
    if (m_dec) opus_decoder_destroy(m_dec);
}

bool AudioDecoder::Init(int sampleRate, int channels) {
    int err = 0;
    // Again with the same format: start over without reallocating.
    opus_int32 rate = 0;
    if (m_dec && channels == m_channels && opus_decoder_ctl(m_dec, OPUS_GET_SAMPLE_RATE(&rate)) == OPUS_OK && rate == sampleRate)
        return opus_decoder_ctl(m_dec, OPUS_RESET_STATE) == OPUS_OK;
    if (m_dec) opus_decoder_destroy(m_dec);
    m_channels = channels;
    m_dec = opus_decoder_create(sampleRate, channels, &err);
    if (err != OPUS_OK || !m_dec) {
        RC_LOGE("opus_decoder_create failed: %d", err);
        return false;
    }
    return true;
}

int AudioDecoder::Decode(const uint8_t* data, int size, int16_t* pcm, int maxFrameSamples) {
    if (!m_dec) return -1;
    return opus_decode(m_dec, data, size, pcm, maxFrameSamples, 0);
}

void JitterBuffer::Configure(int channels, int sampleRate, int targetMs, int maxMs) {
    std::lock_guard lock(m_mutex);
    m_channels = channels;
    m_sampleRate = sampleRate;
    m_target = sampleRate * targetMs / 1000;
    m_max = sampleRate * maxMs / 1000;
    m_minTarget = m_target;
    m_started = false;
    m_calmSinceNs = NowNs();
    m_data.assign(size_t(m_max * 2) * channels, 0);
    m_read = 0;
    m_size = 0;
    m_priming = true;
}

void JitterBuffer::Clear() {
    std::lock_guard lock(m_mutex);
    m_read = 0;
    m_size = 0;
    m_priming = true;
    m_started = false;
}

int JitterBuffer::TargetMs() {
    std::lock_guard lock(m_mutex);
    return m_target * 1000 / m_sampleRate;
}

int JitterBuffer::BufferedFrames() {
    std::lock_guard lock(m_mutex);
    return int(m_size);
}

void JitterBuffer::Push(const int16_t* pcm, int frames) {
    std::lock_guard lock(m_mutex);
    if (m_data.empty()) return;
    const size_t capacity = m_data.size() / m_channels;
    // Overflow: drop the oldest audio to bound latency.
    if (m_size + frames > size_t(m_max)) {
        const size_t drop = m_size + frames - size_t(m_target);
        const size_t d = std::min(drop, m_size);
        m_read = (m_read + d) % capacity;
        m_size -= d;
    }
    size_t write = (m_read + m_size) % capacity;
    for (int i = 0; i < frames; i++) {
        std::memcpy(&m_data[write * m_channels], pcm + size_t(i) * m_channels, sizeof(int16_t) * m_channels);
        write = (write + 1) % capacity;
    }
    m_size += frames;
    if (m_priming && m_size >= size_t(m_target)) m_priming = false;
}

int JitterBuffer::Pop(int16_t* out, int frames) {
    std::lock_guard lock(m_mutex);
    if (m_target > m_minTarget && NowNs() - m_calmSinceNs > 15'000'000'000LL) {
        m_target = std::max(m_minTarget, m_target - m_sampleRate / 100);
        m_calmSinceNs = NowNs();
    }
    if (m_data.empty() || m_priming) {
        std::memset(out, 0, sizeof(int16_t) * frames * m_channels);
        return 0;
    }
    const size_t capacity = m_data.size() / m_channels;
    // Drift correction: when well above target, consume one extra frame per
    // ~100 output frames; when below, repeat one. Inaudible at 1%.
    int real = 0;
    for (int i = 0; i < frames; i++) {
        if (m_size == 0) {
            std::memset(out + size_t(i) * m_channels, 0, sizeof(int16_t) * (frames - i) * m_channels);
            m_priming = true;
            // Audio arrived too late for this buffer: hold more from now on.
            if (m_started) {
                m_target = std::min(m_target + m_sampleRate / 50, m_max / 2);
                m_calmSinceNs = NowNs();
            }
            break;
        }
        m_started = true;
        std::memcpy(out + size_t(i) * m_channels, &m_data[m_read * m_channels], sizeof(int16_t) * m_channels);
        real++;
        bool advance = true;
        if (i % 100 == 50) {
            if (m_size > size_t(m_target) * 3 / 2 && m_size > 1) {
                m_read = (m_read + 1) % capacity;  // skip one
                m_size--;
            } else if (m_size < size_t(m_target) / 2) {
                advance = false;  // hold one
            }
        }
        if (advance) {
            m_read = (m_read + 1) % capacity;
            m_size--;
        }
    }
    return real;
}

}  // namespace rc
