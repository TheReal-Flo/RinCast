// The PC's sound to the headset, whatever captured it: stereo float samples at the capture's
// rate go in, 10 ms Opus packets at 48 kHz go out. The same on every OS; only the capture is the
// platform's (WASAPI loopback on Windows, ScreenCaptureKit on macOS).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "rc/audio_codec.h"
#include "rc/connection.h"
#include "rc/platform.h"
#include "rc/protocol.h"

namespace rcs {

class PcAudioSender {
   public:
    bool Init(rc::Connection* connection, int bitrate) {
        m_connection = connection;
        m_pcm.reserve(kFrame * 2 * 4);
        return m_encoder.Init(rc::kAudioSampleRate, 2, bitrate, true);
    }

    // A new capture (device, format): its sample rate, and the resampler starts over.
    void SetInputRate(double hz) {
        m_ratio = hz > 0 ? hz / rc::kAudioSampleRate : 1.0;
        m_resamplePos = 0;
        m_lastL = m_lastR = 0;
    }

    // One input frame, linearly resampled to 48 kHz.
    void Push(float l, float r) {
        while (m_resamplePos < 1.0) {
            m_pcm.push_back(ToS16(m_lastL + (l - m_lastL) * float(m_resamplePos)));
            m_pcm.push_back(ToS16(m_lastR + (r - m_lastR) * float(m_resamplePos)));
            m_resamplePos += m_ratio;
        }
        m_resamplePos -= 1.0;
        m_lastL = l;
        m_lastR = r;
    }

    // Encodes and sends every whole 10 ms frame collected so far.
    void Flush() {
        size_t used = 0;
        while (m_pcm.size() - used >= size_t(kFrame) * 2) {
            rc::AudioPacketHeader h{};
            h.stream = uint8_t(rc::AudioStream::PcToHeadset);
            h.channels = 2;
            h.frameSamples = kFrame;
            h.sequence = ++m_sequence;
            h.timestampNs = uint64_t(rc::NowNs());
            const int n = m_encoder.Encode(m_pcm.data() + used, kFrame, m_packet, sizeof(m_packet) - 64);
            if (n > 0 && m_connection) m_connection->Send2(rc::PacketType::Audio, &h, sizeof(h), m_packet, size_t(n));
            used += size_t(kFrame) * 2;
        }
        m_pcm.erase(m_pcm.begin(), m_pcm.begin() + std::ptrdiff_t(used));
    }

   private:
    static constexpr int kFrame = 480;  // 10 ms

    static int16_t ToS16(float v) { return int16_t(std::lround(std::fmax(-1.f, std::fmin(1.f, v)) * 32767.f)); }

    rc::Connection* m_connection = nullptr;
    rc::AudioEncoder m_encoder;
    std::vector<int16_t> m_pcm;
    uint8_t m_packet[1500];
    uint32_t m_sequence = 0;
    double m_ratio = 1.0;
    double m_resamplePos = 0;
    float m_lastL = 0, m_lastR = 0;
};

}  // namespace rcs
