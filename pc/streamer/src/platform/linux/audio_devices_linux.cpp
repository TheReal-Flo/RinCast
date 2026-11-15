// The headset's microphone on Linux. There's no driver to install: while a headset streams its
// microphone, the sound server gets a null sink, "RinCast Microphone Feed", which we play the
// headset's voice into, and a source on its monitor, "RinCast Microphone", which apps record
// from (a monitor itself is hidden by most apps' device lists). Both are modules of the server
// (PulseAudio's, or pipewire-pulse's versions of them) and go away when the microphone stops,
// at exit, and at the next start when a killed run left them behind.
//
// The PC's sound needs nothing of ours: it's recorded from the default output's monitor
// (audio_linux.cpp). Steam's virtual devices only exist on Windows.
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "audio/audio.h"
#include "platform/linux/pulse_loop.h"
#include "rc/platform.h"
#include "util.h"

namespace rcs {

namespace audio {

namespace {

constexpr const char* kFeedSink = "rincast_mic_feed";
constexpr const char* kMicSource = "rincast_mic";

std::mutex g_mutex;  // the modules and the default source, between sessions and the UI
uint32_t g_feedModule = PA_INVALID_INDEX;
uint32_t g_micModule = PA_INVALID_INDEX;
std::string g_previousSource;  // the default source before we switched it; "" when we didn't
int g_players = 0;             // running MicPlayers

struct Module {
    uint32_t index;
    std::string name, argument;
};

std::vector<Module> ListModules(PulseLoop& pulse) {
    std::vector<Module> modules;
    pa_operation* op = pa_context_get_module_info_list(
        pulse.Context(),
        [](pa_context*, const pa_module_info* info, int eol, void* user) {
            if (eol || !info) return;
            static_cast<std::vector<Module>*>(user)->push_back(
                {info->index, info->name ? info->name : "", info->argument ? info->argument : ""});
        },
        &modules);
    pulse.Wait(op);
    return modules;
}

// "key=value" as a whole word of a module's arguments.
bool HasArgument(const std::string& args, const std::string& keyValue) {
    for (size_t at = args.find(keyValue); at != std::string::npos; at = args.find(keyValue, at + 1)) {
        const size_t end = at + keyValue.size();
        if ((at == 0 || args[at - 1] == ' ') && (end == args.size() || args[end] == ' ')) return true;
    }
    return false;
}

bool IsFeed(const Module& m) { return HasArgument(m.argument, std::string("sink_name=") + kFeedSink); }
bool IsMic(const Module& m) { return HasArgument(m.argument, std::string("source_name=") + kMicSource); }

uint32_t LoadModule(PulseLoop& pulse, const char* name, const std::string& args) {
    uint32_t index = PA_INVALID_INDEX;
    pa_operation* op = pa_context_load_module(
        pulse.Context(), name, args.c_str(),
        [](pa_context*, uint32_t idx, void* user) { *static_cast<uint32_t*>(user) = idx; }, &index);
    pulse.Wait(op);
    if (index == PA_INVALID_INDEX)
        RC_LOGW("Audio: loading %s failed (%s)", name, pa_strerror(pa_context_errno(pulse.Context())));
    return index;
}

void UnloadModule(PulseLoop& pulse, uint32_t index) {
    pulse.Wait(pa_context_unload_module(pulse.Context(), index, nullptr, nullptr));
}

std::string DefaultSource(PulseLoop& pulse) {
    std::string name;
    pa_operation* op = pa_context_get_server_info(
        pulse.Context(),
        [](pa_context*, const pa_server_info* info, void* user) {
            if (info && info->default_source_name) *static_cast<std::string*>(user) = info->default_source_name;
        },
        &name);
    pulse.Wait(op);
    return name;
}

bool SetDefaultSource(PulseLoop& pulse, const std::string& name) {
    int ok = 0;
    pa_operation* op = pa_context_set_default_source(
        pulse.Context(), name.c_str(), [](pa_context*, int success, void* user) { *static_cast<int*>(user) = success; },
        &ok);
    pulse.Wait(op);
    return ok != 0;
}

// The feed sink and the microphone on its monitor, loaded unless they are there already (ours,
// or from a run that is gone). Mono 48 kHz, what the headset sends: no resampling anywhere.
bool EnsureDevicesLocked(PulseLoop& pulse) {
    bool haveFeed = false, haveMic = false;
    for (const Module& m : ListModules(pulse)) {
        if (IsFeed(m)) {
            haveFeed = true;
            g_feedModule = m.index;
        } else if (IsMic(m)) {
            haveMic = true;
            g_micModule = m.index;
        }
    }
    if (!haveFeed) {
        g_feedModule = LoadModule(pulse, "module-null-sink",
                                  std::string("sink_name=") + kFeedSink +
                                      " sink_properties='device.description=\"RinCast Microphone Feed\"'"
                                      " rate=48000 channels=1 channel_map=mono format=s16le");
        if (g_feedModule == PA_INVALID_INDEX) return false;
    }
    if (!haveMic) {
        g_micModule = LoadModule(pulse, "module-remap-source",
                                 std::string("source_name=") + kMicSource + " master=" + kFeedSink +
                                     ".monitor source_properties='device.description=\"RinCast Microphone\"'"
                                     " channels=1 channel_map=mono");
        if (g_micModule == PA_INVALID_INDEX) return false;
    }
    if (!haveFeed || !haveMic) RC_LOGI("Audio: added \"RinCast Microphone\" for the headset's microphone");
    return true;
}

// Puts back the default source we replaced, and, when no microphone plays, removes our modules
// (also ones a killed run left behind: they're recognised by their names).
void ReleaseLocked(PulseLoop& pulse) {
    if (!g_previousSource.empty()) {
        if (DefaultSource(pulse) == kMicSource && SetDefaultSource(pulse, g_previousSource))
            RC_LOGI("Audio: the default microphone is %s again", g_previousSource.c_str());
        g_previousSource.clear();
    }
    if (g_players > 0) return;
    bool removed = false;
    // The microphone first: it records from the feed's monitor.
    for (const bool mic : {true, false})
        for (const Module& m : ListModules(pulse))
            if (mic ? IsMic(m) : IsFeed(m)) {
                UnloadModule(pulse, m.index);
                removed = true;
            }
    g_feedModule = g_micModule = PA_INVALID_INDEX;
    if (removed) RC_LOGI("Audio: removed \"RinCast Microphone\"");
}

// Whether a sound server answers, asked at most every 10 s (the UI's state asks every second).
bool ServerAvailable() {
    static std::mutex mutex;
    static int64_t checkedNs = 0;
    static bool available = false;
    std::lock_guard lock(mutex);
    if (checkedNs && rc::NowNs() - checkedNs < 10'000'000'000LL) return available;
    PulseLoop pulse;
    available = pulse.Connect("RinCast Streamer", 1000);
    checkedNs = rc::NowNs();
    return available;
}

}  // namespace

const DeviceNames& Names(DeviceSet) {
    static const DeviceNames names{L"RinCast Speaker", L"RinCast Microphone", L"RinCast Microphone Feed"};
    return names;
}

// The microphone counts as there when the sound server is: we make it when it's needed.
DriverStatus QueryDriver(DeviceSet set) {
    DriverStatus s;
    if (set == DeviceSet::RinCast && ServerAvailable()) s.microphone = s.microphoneFeed = true;
    return s;
}

// No speaker of our own (the default output's monitor is recorded); the microphone is ours
// unless the user chose the system's devices only, or Steam's (which Linux doesn't have).
Route ResolveRoute(const std::string& preference) {
    Route r;
    if (preference == "system" || preference == "steam") return r;
    if (!QueryDriver(DeviceSet::RinCast).microphone) return r;
    const DeviceNames& n = Names(DeviceSet::RinCast);
    r.mic = n.mic;
    r.micFeed = n.micFeed;
    r.micSet = "rincast";
    return r;
}

// Makes "RinCast Microphone" the default source, so apps that record from the default hear the
// headset. The speaker stays: we record whatever the default output is.
void SwitchDefaults(const Route& route, bool, bool microphone) {
    if (!microphone || !route.mic) return;
    std::lock_guard lock(g_mutex);
    PulseLoop pulse;
    if (!pulse.Connect("RinCast Streamer") || !EnsureDevicesLocked(pulse)) return;
    const std::string current = DefaultSource(pulse);
    if (current == kMicSource) return;
    if (g_previousSource.empty()) g_previousSource = current;
    if (SetDefaultSource(pulse, kMicSource)) RC_LOGI("Audio: \"RinCast Microphone\" is the default microphone");
}

void RestoreDefaults() {
    std::lock_guard lock(g_mutex);
    PulseLoop pulse;
    if (pulse.Connect("RinCast Streamer", 1000)) ReleaseLocked(pulse);
}

}  // namespace audio

// ---------------------------------------------------------------------------
// Headset mic -> RinCast Microphone
// ---------------------------------------------------------------------------

bool MicPlayer::Start(const wchar_t* feedName, const wchar_t* micName) {
    Stop();
    if (!feedName) return false;
    {
        std::lock_guard lock(audio::g_mutex);
        PulseLoop pulse;
        if (!pulse.Connect("RinCast Streamer") || !audio::EnsureDevicesLocked(pulse)) {
            RC_LOGW("Audio: no \"RinCast Microphone\": the headset's microphone stays off");
            return false;
        }
        ++audio::g_players;
    }
    m_feedName = feedName;
    m_micName = micName ? micName : L"";
    if (!m_decoder.Init(rc::kAudioSampleRate, 1)) {
        Stop();
        return false;
    }
    m_jitter.Configure(1, rc::kAudioSampleRate, 40, 400);  // grows up to 200 ms on a jittery link
    m_lastSeq = 0;
    m_running = true;
    m_thread = std::thread([this] { Run(); });
    return true;
}

void MicPlayer::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    if (m_feedName.empty()) return;  // didn't start
    m_feedName.clear();
    std::lock_guard lock(audio::g_mutex);
    if (--audio::g_players > 0) return;
    PulseLoop pulse;
    if (pulse.Connect("RinCast Streamer", 1000)) audio::ReleaseLocked(pulse);
}

void MicPlayer::PushPacket(const uint8_t* payload, size_t size) {
    if (size < sizeof(rc::AudioPacketHeader) || !m_running) return;
    rc::AudioPacketHeader h;
    std::memcpy(&h, payload, sizeof(h));
    // A packet that arrives after a newer one is too late: its slot was already
    // concealed or skipped, and playing it now would scramble the speech.
    // (A big jump back means the headset restarted its microphone: start over.)
    const int32_t ahead = int32_t(h.sequence - m_lastSeq);
    if (m_lastSeq && ahead <= 0 && ahead > -500) return;
    if (ahead <= 0) m_lastSeq = 0;
    int16_t pcm[5760];
    // Conceal up to 5 lost packets (50 ms) with Opus PLC; after longer gaps just carry on.
    const uint32_t missing = m_lastSeq ? h.sequence - m_lastSeq - 1 : 0;
    for (uint32_t i = 0; i < missing && i < 5; i++) {
        const int n = m_decoder.Decode(nullptr, 0, pcm, h.frameSamples);
        if (n > 0) m_jitter.Push(pcm, n);
    }
    m_lastSeq = h.sequence;
    const int n = m_decoder.Decode(payload + sizeof(h), int(size - sizeof(h)), pcm, 5760);
    if (n > 0) m_jitter.Push(pcm, n);
}

// Plays the jitter buffer into the feed sink at the sink's pace, in 10 ms blocks; the buffer
// fills in silence when the network is late.
void MicPlayer::Run() {
    const pa_sample_spec spec{PA_SAMPLE_S16LE, uint32_t(rc::kAudioSampleRate), 1};
    constexpr int kBlock = 480;  // 10 ms
    int16_t block[kBlock];
    while (m_running) {
        PulseLoop pulse;
        pa_stream* stream = nullptr;
        if (pulse.Connect("RinCast Streamer")) stream = pa_stream_new(pulse.Context(), "Headset microphone", &spec, nullptr);
        if (!stream) {
            m_available = false;
            for (int i = 0; i < 20 && m_running; ++i) rc::SleepMs(100);
            continue;
        }
        // About 30 ms queued in the server, topped up every 10 ms: low latency without gaps.
        pa_buffer_attr attr;
        std::memset(&attr, 0xff, sizeof(attr));  // (uint32_t)-1: the server's choice
        attr.tlength = uint32_t(pa_usec_to_bytes(30'000, &spec));
        attr.minreq = uint32_t(pa_usec_to_bytes(10'000, &spec));
        attr.prebuf = attr.minreq;
        if (pa_stream_connect_playback(stream, audio::kFeedSink, &attr, PA_STREAM_ADJUST_LATENCY, nullptr, nullptr) < 0) {
            pa_stream_unref(stream);
            for (int i = 0; i < 20 && m_running; ++i) rc::SleepMs(100);
            continue;
        }
        while (m_running) {
            pulse.Iterate(10);
            const pa_stream_state_t state = pa_stream_get_state(stream);
            if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED || !pulse.Ok()) {
                RC_LOGW("Audio: playing the headset's microphone stopped (%s)",
                        pa_strerror(pa_context_errno(pulse.Context())));
                break;
            }
            if (state != PA_STREAM_READY) continue;
            m_available = true;
            size_t writable = pa_stream_writable_size(stream);
            if (writable == size_t(-1)) writable = 0;  // an error: the state check above sees it next
            for (; writable >= sizeof(block); writable -= sizeof(block)) {
                m_jitter.Pop(block, kBlock);
                if (pa_stream_write(stream, block, sizeof(block), nullptr, 0, PA_SEEK_RELATIVE) < 0) break;
            }
        }
        m_available = false;
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
        for (int i = 0; i < 10 && m_running; ++i) rc::SleepMs(100);  // before trying again
    }
    m_available = false;
}

}  // namespace rcs
