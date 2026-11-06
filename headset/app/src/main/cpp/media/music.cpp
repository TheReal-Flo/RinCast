#include "music.h"

#include <chrono>
#include <string>

#include "rc/platform.h"

namespace rv {

namespace {

// The tracks are matched to -16 LUFS; this puts the music at about -23 LUFS, well under
// a game or a video, so it stays in the background.
constexpr float kGain = 0.45f;

}  // namespace

MusicPlayer::~MusicPlayer() {
    if (m_loader.joinable()) m_loader.join();
    Close();
}

void MusicPlayer::Load(const Assets& assets) {
    if (m_loader.joinable()) return;
    m_mixer.SetGain(kGain);
    m_loader = std::thread([this, &assets] {
        std::vector<rc::TrackData> tracks;
        for (const MusicTrack& t : kMusicTracks) {
            std::vector<uint8_t> data;
            if (assets.Read((std::string("music/") + t.file).c_str(), data))
                tracks.push_back(std::make_shared<const std::vector<uint8_t>>(std::move(data)));
            else
                RC_LOGW("Music: %s is missing", t.file);
        }
        const auto seed = uint32_t(std::chrono::steady_clock::now().time_since_epoch().count());
        m_mixer.SetTracks(std::move(tracks), seed);
        m_loaded = true;
    });
}

void MusicPlayer::Update(bool wanted) {
    if (!m_loaded) return;
    m_mixer.SetActive(wanted);
    if (m_restart.exchange(false)) Close();  // output device changed: reopen below
    if (wanted && !m_stream) {
        // Without an output now, try again in a while rather than every frame.
        if (m_openDelay > 0) m_openDelay--;
        else if (!Open()) m_openDelay = 600;
    } else if (!wanted && m_stream && m_mixer.Idle()) {
        Close();
    }
}

bool MusicPlayer::Open() {
    AAudioStreamBuilder* b = nullptr;
    AAudio_createStreamBuilder(&b);
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    // Latency doesn't matter for music; battery does.
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_POWER_SAVING);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(b, Callback, this);
    AAudioStreamBuilder_setErrorCallback(b, ErrorCallback, this);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &m_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        RC_LOGW("Music: no audio output: %s", AAudio_convertResultToText(r));
        m_stream = nullptr;
        return false;
    }
    AAudioStream_requestStart(m_stream);
    return true;
}

void MusicPlayer::Close() {
    if (!m_stream) return;
    AAudioStream_requestStop(m_stream);
    AAudioStream_close(m_stream);
    m_stream = nullptr;
}

aaudio_data_callback_result_t MusicPlayer::Callback(AAudioStream*, void* user, void* audio, int32_t frames) {
    static_cast<MusicPlayer*>(user)->m_mixer.Render(static_cast<int16_t*>(audio), frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void MusicPlayer::ErrorCallback(AAudioStream*, void* user, aaudio_result_t error) {
    if (error == AAUDIO_ERROR_DISCONNECTED) static_cast<MusicPlayer*>(user)->m_restart = true;
}

}  // namespace rv
