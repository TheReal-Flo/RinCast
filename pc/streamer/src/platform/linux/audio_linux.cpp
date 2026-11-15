// The PC's sound on Linux: the default output's monitor, recorded through the PulseAudio API
// (PipeWire answers it too) and sent through the shared PcAudioSender (48 kHz Opus, as on every
// OS). Like the Windows loopback, it follows the default output when the user switches it.
#include <cstring>
#include <string>

#include "audio/audio.h"
#include "audio/audio_sender.h"
#include "platform/linux/pulse_loop.h"
#include "rc/platform.h"

namespace rcs {

namespace {

// The server's default output, "" when unknown.
std::string DefaultSink(PulseLoop& pulse) {
    std::string name;
    pa_operation* op = pa_context_get_server_info(
        pulse.Context(),
        [](pa_context*, const pa_server_info* info, void* user) {
            if (info && info->default_sink_name) *static_cast<std::string*>(user) = info->default_sink_name;
        },
        &name);
    pulse.Wait(op);
    return name;
}

}  // namespace

bool AudioStreamer::Start(rc::Connection* connection, const wchar_t* speakerName, int bitrate) {
    Stop();
    m_connection = connection;
    m_speakerName = speakerName ? speakerName : L"";  // there's no speaker of our own on Linux
    m_bitrate = bitrate;
    m_running = true;
    m_thread = std::thread([this] { Run(); });
    return true;
}

void AudioStreamer::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void AudioStreamer::Run() {
    PcAudioSender sender;
    if (!sender.Init(m_connection, m_bitrate)) return;
    // The server resamples to what we ask for: 48 kHz stereo float, the sender's own rate.
    sender.SetInputRate(rc::kAudioSampleRate);
    const pa_sample_spec spec{PA_SAMPLE_FLOAT32LE, uint32_t(rc::kAudioSampleRate), 2};
    bool warned = false;

    while (m_running) {
        PulseLoop pulse;
        if (!pulse.Connect("RinCast Streamer")) {
            if (!warned) RC_LOGW("Audio: no sound server (PulseAudio or PipeWire) to record from");
            warned = true;
            for (int i = 0; i < 20 && m_running; ++i) rc::SleepMs(100);
            continue;
        }
        const std::string sink = DefaultSink(pulse);
        pa_stream* stream = pa_stream_new(pulse.Context(), "PC sound to the headset", &spec, nullptr);
        if (!stream) {
            rc::SleepMs(1000);
            continue;
        }
        // 10 ms fragments: the server hands over what it has that often, not in big blocks.
        pa_buffer_attr attr;
        std::memset(&attr, 0xff, sizeof(attr));  // (uint32_t)-1: the server's choice
        attr.fragsize = uint32_t(pa_usec_to_bytes(10'000, &spec));
        // The monitor of whatever the default output is; we follow changes ourselves below.
        if (pa_stream_connect_record(stream, "@DEFAULT_MONITOR@", &attr, PA_STREAM_ADJUST_LATENCY) < 0) {
            pa_stream_unref(stream);
            rc::SleepMs(1000);
            continue;
        }
        bool ready = false;
        int64_t lastCheck = rc::NowNs();
        while (m_running) {
            pulse.Iterate(20);
            const pa_stream_state_t state = pa_stream_get_state(stream);
            if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED || !pulse.Ok()) {
                RC_LOGW("Audio: recording the PC's sound stopped (%s)", pa_strerror(pa_context_errno(pulse.Context())));
                break;
            }
            if (state != PA_STREAM_READY) continue;
            if (!ready) {
                ready = true;
                warned = false;
                RC_LOGI("Streaming audio from %s", sink.empty() ? "the default output" : sink.c_str());
            }
            const void* data = nullptr;
            size_t bytes = 0;
            while (pa_stream_readable_size(stream) > 0 && pa_stream_peek(stream, &data, &bytes) == 0 && bytes > 0) {
                const auto* f = static_cast<const float*>(data);
                // No data with a size is a hole (dropped samples): skipped, as nothing played.
                if (f)
                    for (size_t i = 0; i + 1 < bytes / sizeof(float); i += 2) sender.Push(f[i], f[i + 1]);
                pa_stream_drop(stream);
            }
            sender.Flush();
            // Another default output: record its monitor instead.
            if (rc::NowNs() - lastCheck > 2'000'000'000LL) {
                lastCheck = rc::NowNs();
                const std::string now = DefaultSink(pulse);
                if (!now.empty() && now != sink) break;
            }
        }
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
    }
}

}  // namespace rcs
