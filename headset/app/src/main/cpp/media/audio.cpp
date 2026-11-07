#include "audio.h"

#include <algorithm>
#include <cstring>

#ifndef __ANDROID__
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

#include "rc/platform.h"
#include "rc/protocol.h"

namespace rv {

#ifdef __ANDROID__

bool AudioPlayer::Start() {
    Stop();
    if (!m_decoder.Init(rc::kAudioSampleRate, 2)) return false;
    m_jitter.Configure(2, rc::kAudioSampleRate, 30, 150);
    AAudioStreamBuilder* b = nullptr;
    AAudio_createStreamBuilder(&b);
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, rc::kAudioSampleRate);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MOVIE);
    AAudioStreamBuilder_setDataCallback(b, Callback, this);
    AAudioStreamBuilder_setErrorCallback(b, ErrorCallback, this);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &m_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        RC_LOGE("AAudio output open failed: %s", AAudio_convertResultToText(r));
        m_stream = nullptr;
        return false;
    }
    AAudioStream_setBufferSizeInFrames(m_stream, AAudioStream_getFramesPerBurst(m_stream) * 2);
    AAudioStream_requestStart(m_stream);
    return true;
}

void AudioPlayer::Stop() {
    if (m_stream) {
        AAudioStream_requestStop(m_stream);
        AAudioStream_close(m_stream);
        m_stream = nullptr;
    }
}

aaudio_data_callback_result_t AudioPlayer::Callback(AAudioStream*, void* user, void* audio, int32_t frames) {
    auto* self = static_cast<AudioPlayer*>(user);
    auto* out = static_cast<int16_t*>(audio);
    self->m_jitter.Pop(out, frames);
    const float v = self->m_volume;
    if (v < 0.999f)
        for (int i = 0; i < frames * 2; i++) out[i] = int16_t(out[i] * v);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AudioPlayer::ErrorCallback(AAudioStream*, void* user, aaudio_result_t error) {
    if (error == AAUDIO_ERROR_DISCONNECTED) static_cast<AudioPlayer*>(user)->m_restart = true;
}

#else  // PulseAudio (PipeWire on SteamOS): a thread writes 10 ms blocks; the server paces it.

namespace {

pa_simple* OpenPulse(pa_stream_direction_t direction, int channels, const char* stream, uint32_t latencyMs) {
    pa_sample_spec spec{};
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = rc::kAudioSampleRate;
    spec.channels = uint8_t(channels);
    pa_buffer_attr attr{};
    const uint32_t bytes = uint32_t(rc::kAudioSampleRate / 1000 * latencyMs * channels * 2);
    attr.maxlength = uint32_t(-1);
    attr.tlength = direction == PA_STREAM_PLAYBACK ? bytes : uint32_t(-1);
    attr.prebuf = uint32_t(-1);
    attr.minreq = uint32_t(-1);
    attr.fragsize = direction == PA_STREAM_RECORD ? bytes : uint32_t(-1);
    int error = 0;
    pa_simple* s = pa_simple_new(nullptr, "RinCast", direction, nullptr, stream, &spec, nullptr, &attr, &error);
    if (!s) RC_LOGW("PulseAudio: couldn't open \"%s\": %s", stream, pa_strerror(error));
    return s;
}

}  // namespace

bool AudioPlayer::Start() {
    Stop();
    if (!m_decoder.Init(rc::kAudioSampleRate, 2)) return false;
    m_jitter.Configure(2, rc::kAudioSampleRate, 30, 150);
    m_stream = OpenPulse(PA_STREAM_PLAYBACK, 2, "PC audio", 20);
    if (!m_stream) return false;
    m_running = true;
    m_thread = std::thread([this] { Run(); });
    return true;
}

void AudioPlayer::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    if (m_stream) {
        pa_simple_free(static_cast<pa_simple*>(m_stream));
        m_stream = nullptr;
    }
}

void AudioPlayer::Run() {
    constexpr int kFrames = 480;  // 10 ms
    int16_t out[kFrames * 2];
    while (m_running) {
        m_jitter.Pop(out, kFrames);
        const float v = m_volume;
        if (v < 0.999f)
            for (int i = 0; i < kFrames * 2; i++) out[i] = int16_t(out[i] * v);
        int error = 0;
        if (pa_simple_write(static_cast<pa_simple*>(m_stream), out, sizeof(out), &error) < 0) {
            RC_LOGW("PulseAudio: playback stopped: %s", pa_strerror(error));
            m_restart = true;  // reopened with the next packet
            break;
        }
    }
}

#endif

int AudioPlayer::BufferedMs() { return m_jitter.BufferedFrames() * 1000 / int(rc::kAudioSampleRate); }

void AudioPlayer::PushPacket(const uint8_t* data, size_t size) {
    if (m_restart.exchange(false)) Start();  // device change (e.g. headphones plugged in)
    if (size < sizeof(rc::AudioPacketHeader)) return;
    rc::AudioPacketHeader h;
    std::memcpy(&h, data, sizeof(h));
    int16_t pcm[5760 * 2];
    std::lock_guard lock(m_mutex);
    // Too late (a newer packet was already played): playing it now would scramble the
    // sound. A big jump back means the PC restarted its stream.
    const int32_t ahead = int32_t(h.sequence - m_lastSeq);
    if (m_lastSeq && ahead <= 0 && ahead > -500) return;
    if (m_lastSeq && h.sequence > m_lastSeq + 1 && h.sequence - m_lastSeq < 5) {
        // Packet loss concealment for short gaps.
        for (uint32_t s = m_lastSeq + 1; s < h.sequence; s++) {
            const int n = m_decoder.Decode(nullptr, 0, pcm, h.frameSamples);
            if (n > 0) m_jitter.Push(pcm, n);
        }
    }
    m_lastSeq = h.sequence;
    const int n = m_decoder.Decode(data + sizeof(h), int(size - sizeof(h)), pcm, 5760);
    if (n > 0) m_jitter.Push(pcm, n);
}

// ---------------------------------------------------------------------------

#ifdef __ANDROID__

bool MicCapture::Start(PacketSink sink) {
    Stop();
    m_sink = std::move(sink);
    if (!m_encoder.Init(rc::kAudioSampleRate, 1, 48000, true)) return false;
    AAudioStreamBuilder* b = nullptr;
    AAudio_createStreamBuilder(&b);
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setSampleRate(b, rc::kAudioSampleRate);
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    AAudioStreamBuilder_setDataCallback(b, Callback, this);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &m_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        RC_LOGW("Microphone unavailable: %s", AAudio_convertResultToText(r));
        m_stream = nullptr;
        return false;
    }
    m_rate = AAudioStream_getSampleRate(m_stream);
    m_channels = std::max(1, int(AAudioStream_getChannelCount(m_stream)));
    m_float = AAudioStream_getFormat(m_stream) == AAUDIO_FORMAT_PCM_FLOAT;
    m_resamplePos = 0;
    m_prevSample = 0;
    RC_LOGI("Microphone: %d Hz, %d channel(s), %s%s", m_rate, m_channels, m_float ? "float" : "16-bit",
            m_rate != int(rc::kAudioSampleRate) || m_channels != 1 || m_float ? ", converting to 48 kHz mono 16-bit" : "");
    AAudioStream_requestStart(m_stream);
    return true;
}

void MicCapture::Stop() {
    if (m_stream) {
        AAudioStream_requestStop(m_stream);
        AAudioStream_close(m_stream);
        m_stream = nullptr;
    }
    m_pending.clear();
}

aaudio_data_callback_result_t MicCapture::Callback(AAudioStream*, void* user, void* audio, int32_t frames) {
    static_cast<MicCapture*>(user)->OnSamples(audio, frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

#else  // PulseAudio: asked for 48 kHz mono 16-bit, so no conversion is needed

bool MicCapture::Start(PacketSink sink) {
    Stop();
    m_sink = std::move(sink);
    if (!m_encoder.Init(rc::kAudioSampleRate, 1, 48000, true)) return false;
    m_stream = OpenPulse(PA_STREAM_RECORD, 1, "Microphone", 10);
    if (!m_stream) return false;
    m_rate = int(rc::kAudioSampleRate);
    m_channels = 1;
    m_float = false;
    RC_LOGI("Microphone: 48 kHz mono through PulseAudio");
    m_running = true;
    m_thread = std::thread([this] { Run(); });
    return true;
}

void MicCapture::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    if (m_stream) {
        pa_simple_free(static_cast<pa_simple*>(m_stream));
        m_stream = nullptr;
    }
    m_pending.clear();
}

void MicCapture::Run() {
    constexpr int kFrames = 480;  // 10 ms
    int16_t in[kFrames];
    while (m_running) {
        int error = 0;
        if (pa_simple_read(static_cast<pa_simple*>(m_stream), in, sizeof(in), &error) < 0) {
            RC_LOGW("PulseAudio: microphone stopped: %s", pa_strerror(error));
            break;
        }
        OnSamples(in, kFrames);
    }
}

#endif

void MicCapture::OnSamples(const void* audio, int32_t frames) {
    const auto* in = static_cast<const int16_t*>(audio);
    if (m_muted) return;
    const int ch = m_channels;
    if (m_rate == int(rc::kAudioSampleRate) && ch == 1 && !m_float) {
        m_pending.insert(m_pending.end(), in, in + frames);
    } else {
        // Treated as 48 kHz, audio at another rate would play too fast or too slow and at
        // the wrong pitch (a "voice changer"). Mix down to mono and resample linearly.
        const double step = double(m_rate) / rc::kAudioSampleRate;  // input frames per output sample
        for (int32_t i = 0; i < frames; i++) {
            int sum = 0;
            for (int c = 0; c < ch; c++) {
                const size_t k = size_t(i) * ch + c;
                sum += m_float ? int(std::clamp(static_cast<const float*>(audio)[k], -1.f, 1.f) * 32767.f) : in[k];
            }
            const int16_t cur = int16_t(sum / ch);
            while (m_resamplePos < 1.0) {
                const double p = m_resamplePos;
                m_pending.push_back(int16_t(m_prevSample + (cur - m_prevSample) * p));
                m_resamplePos += step;
            }
            m_resamplePos -= 1.0;
            m_prevSample = cur;
        }
    }
    constexpr int kFrame = 480;  // 10 ms
    uint8_t packet[sizeof(rc::AudioPacketHeader) + 1000];
    while (m_pending.size() >= size_t(kFrame)) {
        if (const float gain = m_gain; gain != 1.f)
            for (int i = 0; i < kFrame; i++)
                m_pending[size_t(i)] = int16_t(std::clamp(int(m_pending[size_t(i)] * gain), -32768, 32767));
        rc::AudioPacketHeader h{};
        h.stream = uint8_t(rc::AudioStream::MicToPc);
        h.channels = 1;
        h.frameSamples = kFrame;
        h.sequence = ++m_seq;
        h.timestampNs = uint64_t(rc::NowNs());
        const int n = m_encoder.Encode(m_pending.data(), kFrame, packet + sizeof(h), 1000);
        if (n > 0 && m_sink) {
            std::memcpy(packet, &h, sizeof(h));
            m_sink(packet, sizeof(h) + size_t(n));
        }
        m_pending.erase(m_pending.begin(), m_pending.begin() + kFrame);
    }
}

}  // namespace rv
