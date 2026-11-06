#include "rc/music.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rc {

namespace {

constexpr int kRate = 48000;

uint64_t Le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

bool IsPage(const std::vector<uint8_t>& d, size_t at) { return at + 27 <= d.size() && std::memcmp(&d[at], "OggS", 4) == 0; }

}  // namespace

bool OggOpusReader::Open(TrackData data) {
    m_data = std::move(data);
    m_pageEnd = m_lacing = m_body = 0;
    m_segments = m_segment = 0;
    m_total = m_emitted = 0;
    m_pcmLen = m_pcmPos = 0;
    m_eof = true;
    if (!m_data) return false;
    // OpusHead: magic, version, channels, pre-skip (LE16), input rate, gain, mapping.
    if (!NextPacket(m_packet) || m_packet.size() < 19 || std::memcmp(m_packet.data(), "OpusHead", 8) != 0) return false;
    const int channels = m_packet[9];
    if (channels < 1 || channels > 2 || m_packet[18] != 0) return false;  // plain mono or stereo only
    m_skip = m_packet[10] | m_packet[11] << 8;
    if (!NextPacket(m_packet)) return false;  // OpusTags
    // Always stereo out: the decoder upmixes mono itself.
    if (!m_decoder.Init(kRate, 2)) return false;
    // The last page's granule position is the track's length, pre-skip included.
    const auto& d = *m_data;
    for (size_t at = d.size() >= 27 ? d.size() - 27 : 0;; at--) {
        if (IsPage(d, at)) {
            m_total = std::max<int64_t>(0, int64_t(Le64(&d[at + 6])) - m_skip);
            break;
        }
        if (at == 0) return false;
    }
    m_eof = false;
    return true;
}

bool OggOpusReader::NextPacket(std::vector<uint8_t>& packet) {
    const auto& d = *m_data;
    packet.clear();
    for (;;) {
        if (m_segment >= m_segments) {
            if (!IsPage(d, m_pageEnd)) return false;
            m_segments = d[m_pageEnd + 26];
            m_lacing = m_pageEnd + 27;
            m_body = m_lacing + m_segments;
            if (m_body > d.size()) return false;
            size_t size = 0;
            for (int i = 0; i < m_segments; i++) size += d[m_lacing + i];
            if (m_body + size > d.size()) return false;
            m_pageEnd = m_body + size;
            m_segment = 0;
            continue;
        }
        // A packet is a run of 255-byte segments ended by a shorter one, across pages if need be.
        const int length = d[m_lacing + m_segment++];
        packet.insert(packet.end(), d.begin() + m_body, d.begin() + m_body + length);
        m_body += length;
        if (length < 255) return true;
    }
}

int OggOpusReader::Read(int16_t* out, int frames) {
    int done = 0;
    while (done < frames && !m_eof && m_emitted < m_total) {
        if (m_pcmPos >= m_pcmLen) {
            if (!NextPacket(m_packet)) {
                m_eof = true;
                break;
            }
            const int n = m_decoder.Decode(m_packet.data(), int(m_packet.size()), m_pcm, 5760);
            if (n <= 0) continue;  // a damaged packet: skip it
            m_pcmLen = n;
            m_pcmPos = std::min(m_skip, n);
            m_skip -= m_pcmPos;
            continue;
        }
        const int take = int(std::min<int64_t>({frames - done, m_pcmLen - m_pcmPos, m_total - m_emitted}));
        std::memcpy(out + done * 2, m_pcm + m_pcmPos * 2, size_t(take) * 2 * sizeof(int16_t));
        m_pcmPos += take;
        m_emitted += take;
        done += take;
    }
    return done;
}

// ---------------------------------------------------------------------------

void MusicMixer::SetTracks(std::vector<TrackData> tracks, uint32_t seed) {
    std::lock_guard lock(m_mutex);
    m_tracks = std::move(tracks);
    m_random.seed(seed);
}

int MusicMixer::TrackCount() {
    std::lock_guard lock(m_mutex);
    return int(m_tracks.size());
}

int MusicMixer::NextTrack() {
    // Any but the one just played.
    const int n = int(m_tracks.size());
    if (n <= 1 || m_lastTrack < 0) return n ? int(m_random() % uint32_t(n)) : -1;
    const int pick = int(m_random() % uint32_t(n - 1));
    return pick >= m_lastTrack ? pick + 1 : pick;
}

MusicMixer::Voice* MusicMixer::Start(float fadeSeconds) {
    Voice* voice = nullptr;
    for (Voice& v : m_voices)
        if (!v.alive) voice = &v;
    if (!voice) {
        // All busy (stopped and started again mid-fade): drop the quietest.
        voice = &m_voices[0];
        for (Voice& v : m_voices)
            if (&v != m_current && v.level < voice->level) voice = &v;
    }
    // A track that doesn't open is skipped, not retried forever.
    for (size_t tries = 0; tries < m_tracks.size(); tries++) {
        const int track = NextTrack();
        m_lastTrack = track;
        if (voice->reader.Open(m_tracks[size_t(track)])) {
            voice->alive = true;
            voice->level = 0;
            voice->rate = 1.f / (fadeSeconds * kRate);
            return voice;
        }
    }
    voice->alive = false;
    return nullptr;
}

void MusicMixer::Render(int16_t* out, int frames) {
    std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t));
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;  // tracks are being swapped in: a moment of silence

    if (!m_active) {
        // Everything, a track still crossfading out too, is gone within kFadeOutSeconds.
        for (Voice& v : m_voices) v.rate = std::min(v.rate, -1.f / (kFadeOutSeconds * kRate));
        m_current = nullptr;
    } else if (!m_tracks.empty()) {
        if (!m_current) {
            m_current = Start(kFadeInSeconds);
        } else {
            // Into the next track while this one ends: equal-power, so no dip in the middle.
            const int64_t left = m_current->reader.RemainingFrames();
            if (left <= int64_t(kCrossfadeSeconds * kRate)) {
                Voice* ending = m_current;
                // Start may have to reuse a voice; it never takes the current one.
                m_current = Start(kCrossfadeSeconds);
                ending->rate = -std::max(ending->level, 0.01f) / float(std::max<int64_t>(left, 1));
            }
        }
    }

    m_mix.assign(size_t(frames) * 2, 0.f);
    m_scratch.resize(size_t(frames) * 2);
    bool any = false;
    for (Voice& v : m_voices) {
        if (!v.alive) continue;
        const int got = v.reader.Read(m_scratch.data(), frames);
        for (int i = 0; i < got; i++) {
            v.level = std::clamp(v.level + v.rate, 0.f, 1.f);
            const float g = std::sin(v.level * 1.5707963f);
            m_mix[size_t(i) * 2] += m_scratch[size_t(i) * 2] * g;
            m_mix[size_t(i) * 2 + 1] += m_scratch[size_t(i) * 2 + 1] * g;
        }
        // Faded out, or at the end of the track.
        if ((v.rate < 0 && v.level <= 0) || got < frames) {
            v.alive = false;
            if (m_current == &v) m_current = nullptr;
        } else {
            any = true;
        }
    }
    const float gain = m_gain;
    for (size_t i = 0; i < m_mix.size(); i++) out[i] = int16_t(std::clamp(m_mix[i] * gain, -32768.f, 32767.f));
    m_idle = !any && !m_active;
}

}  // namespace rc
