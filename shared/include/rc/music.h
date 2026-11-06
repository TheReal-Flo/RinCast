// Background music: an Ogg Opus reader that decodes as it plays (a whole track as PCM
// would be ~55 MB), and a mixer that fades tracks in, out and into each other.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

#include "rc/audio_codec.h"

namespace rc {

using TrackData = std::shared_ptr<const std::vector<uint8_t>>;

// One Ogg Opus file in memory, decoded to 48 kHz stereo int16. Single stream, no CRC check.
class OggOpusReader {
   public:
    bool Open(TrackData data);
    // Fills up to `frames` stereo frames; fewer only at the end of the track.
    int Read(int16_t* out, int frames);
    int64_t TotalFrames() const { return m_total; }
    int64_t RemainingFrames() const { return m_eof ? 0 : m_total - m_emitted; }

   private:
    bool NextPacket(std::vector<uint8_t>& packet);

    TrackData m_data;
    AudioDecoder m_decoder;
    size_t m_pageEnd = 0;     // where the next page starts
    size_t m_lacing = 0;      // the current page's segment table
    int m_segments = 0;
    int m_segment = 0;
    size_t m_body = 0;        // where the current segment's bytes start
    int64_t m_total = 0;      // frames, without the encoder's pre-skip
    int64_t m_emitted = 0;
    int m_skip = 0;           // pre-skip still to drop
    bool m_eof = false;
    std::vector<uint8_t> m_packet;
    int16_t m_pcm[5760 * 2];
    int m_pcmLen = 0, m_pcmPos = 0;
};

class MusicMixer {
   public:
    static constexpr float kFadeInSeconds = 4.f;     // when music starts
    static constexpr float kFadeOutSeconds = 2.f;    // when it should stop
    static constexpr float kCrossfadeSeconds = 8.f;  // from one track into the next

    void SetTracks(std::vector<TrackData> tracks, uint32_t seed);
    // Playing, or fading out to silence. Back on after that always starts another track.
    void SetActive(bool active) { m_active = active; }
    void SetGain(float gain) { m_gain = gain; }
    // Nothing playing and nothing wanted: the output can be closed.
    bool Idle() const { return m_idle; }
    int TrackCount();
    // Audio thread: 48 kHz interleaved stereo.
    void Render(int16_t* out, int frames);

   private:
    struct Voice {
        OggOpusReader reader;
        bool alive = false;
        float level = 0;  // 0..1 along an equal-power curve
        float rate = 0;   // level change per frame: + fading in, - fading out
    };
    Voice* Start(float fadeSeconds);
    int NextTrack();

    std::mutex m_mutex;
    std::vector<TrackData> m_tracks;
    std::minstd_rand m_random;
    int m_lastTrack = -1;
    Voice m_voices[3];
    Voice* m_current = nullptr;  // the one playing on; the others are fading out
    std::atomic<bool> m_active{false};
    std::atomic<float> m_gain{1.f};
    std::atomic<bool> m_idle{true};
    std::vector<int16_t> m_scratch;
    std::vector<float> m_mix;
};

}  // namespace rc
