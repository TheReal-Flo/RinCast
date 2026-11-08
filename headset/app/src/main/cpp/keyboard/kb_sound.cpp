#include "kb_sound.h"

#include <algorithm>

#if defined(__linux__) && !defined(__ANDROID__)
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

#include "platform/assets.h"
#include "rc/platform.h"

namespace rv::kb {

ClickPlayer::~ClickPlayer() { Close(); }

void ClickPlayer::Load(const Assets& assets) {
    const char* names[4] = {"click-deep-key", "click-deep-big", "click-light-key", "click-light-big"};
    for (int i = 0; i < 4; i++) {
        std::vector<uint8_t> raw;
        if (!assets.Read((std::string("keyboard/") + names[i] + ".pcm").c_str(), raw)) continue;
        m_clicks[i].resize(raw.size() / 2);
        std::copy_n(raw.data(), m_clicks[i].size() * 2, reinterpret_cast<uint8_t*>(m_clicks[i].data()));
    }
}

void ClickPlayer::SetOpen(bool open) {
    if (open && !m_stream) Open();
    else if (!open && m_stream) Close();
}

void ClickPlayer::Play(const std::string& sound, bool big) {
    const int i = (sound == "light" ? 2 : 0) + (big ? 1 : 0);
    if (m_clicks[i].empty()) return;
    if (!m_stream && !Open()) return;
    std::lock_guard<std::mutex> l(m_lock);
    m_voices.push_back({&m_clicks[i], 0});
}

void ClickPlayer::Render(int16_t* out, int frames) {
    std::lock_guard<std::mutex> l(m_lock);
    for (int f = 0; f < frames; f++) {
        int v = 0;
        for (Voice& voice : m_voices)
            if (voice.pos < voice.pcm->size()) v += (*voice.pcm)[voice.pos++];
        const int16_t s = int16_t(std::clamp(v, -32768, 32767));
        out[f * 2] = out[f * 2 + 1] = s;
    }
    m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(), [](const Voice& v) { return v.pos >= v.pcm->size(); }),
                   m_voices.end());
}

#ifdef __ANDROID__
bool ClickPlayer::Open() {
    AAudioStreamBuilder* b = nullptr;
    AAudio_createStreamBuilder(&b);
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);  // a click is late or it's wrong
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_ASSISTANCE_SONIFICATION);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SONIFICATION);
    AAudioStreamBuilder_setDataCallback(b, Callback, this);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &m_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        RC_LOGW("Keyboard: no audio output for clicks: %s", AAudio_convertResultToText(r));
        m_stream = nullptr;
        return false;
    }
    AAudioStream_requestStart(m_stream);
    return true;
}

void ClickPlayer::Close() {
    if (!m_stream) return;
    AAudioStream_requestStop(m_stream);
    AAudioStream_close(m_stream);
    m_stream = nullptr;
    std::lock_guard<std::mutex> l(m_lock);
    m_voices.clear();
}

aaudio_data_callback_result_t ClickPlayer::Callback(AAudioStream*, void* user, void* audio, int32_t frames) {
    static_cast<ClickPlayer*>(user)->Render(static_cast<int16_t*>(audio), frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

#elif !defined(__linux__)  // the UI preview on Windows: no sound

bool ClickPlayer::Open() { return false; }
void ClickPlayer::Close() {}

#else  // PulseAudio: 10 ms blocks

bool ClickPlayer::Open() {
    pa_sample_spec spec{};
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = 48000;
    spec.channels = 2;
    pa_buffer_attr attr{};
    attr.maxlength = uint32_t(-1);
    attr.tlength = 480 * 4 * 2;  // about 20 ms queued
    attr.prebuf = uint32_t(-1);
    attr.minreq = uint32_t(-1);
    attr.fragsize = uint32_t(-1);
    int error = 0;
    pa_simple* s = pa_simple_new(nullptr, "RinCast", PA_STREAM_PLAYBACK, nullptr, "Keyboard", &spec, nullptr, &attr, &error);
    if (!s) {
        RC_LOGW("Keyboard: no audio output for clicks: %s", pa_strerror(error));
        return false;
    }
    m_stream = s;
    m_running = true;
    m_output = std::thread([this] {
        int16_t block[480 * 2];
        while (m_running) {
            Render(block, 480);
            int err = 0;
            if (pa_simple_write(static_cast<pa_simple*>(m_stream), block, sizeof(block), &err) < 0) break;
        }
    });
    return true;
}

void ClickPlayer::Close() {
    if (!m_stream) return;
    m_running = false;
    if (m_output.joinable()) m_output.join();
    pa_simple_free(static_cast<pa_simple*>(m_stream));
    m_stream = nullptr;
}

#endif

}  // namespace rv::kb
