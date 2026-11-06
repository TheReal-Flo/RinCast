// Opus encode/decode helpers and a PCM jitter buffer with gentle drift
// correction, shared by the streamer (PC audio out, mic in) and the headset.
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

struct OpusEncoder;
struct OpusDecoder;

namespace rc {

class AudioEncoder {
   public:
    ~AudioEncoder();
    bool Init(int sampleRate, int channels, int bitrate, bool lowDelay);
    // `pcm` holds frameSamples * channels interleaved int16. Returns encoded size or <0.
    int Encode(const int16_t* pcm, int frameSamples, uint8_t* out, int capacity);
    int Channels() const { return m_channels; }

   private:
    OpusEncoder* m_enc = nullptr;
    int m_channels = 2;
};

class AudioDecoder {
   public:
    ~AudioDecoder();
    bool Init(int sampleRate, int channels);
    // Returns decoded samples per channel, or <0 on error. Pass data=nullptr for PLC.
    int Decode(const uint8_t* data, int size, int16_t* pcm, int maxFrameSamples);
    int Channels() const { return m_channels; }

   private:
    OpusDecoder* m_dec = nullptr;
    int m_channels = 2;
};

// Interleaved int16 FIFO that keeps its fill level around a target latency. The target
// adapts: every underrun (the network delivered late) raises it by 20 ms, up to half of
// maxMs; after 15 s without one it comes down again by 10 ms, to targetMs at the least.
class JitterBuffer {
   public:
    void Configure(int channels, int sampleRate, int targetMs, int maxMs);
    int TargetMs();
    void Push(const int16_t* pcm, int frames);
    // Always fills `frames` frames (silence on underrun). Returns frames of real audio.
    int Pop(int16_t* out, int frames);
    int BufferedFrames();
    void Clear();

   private:
    std::mutex m_mutex;
    std::vector<int16_t> m_data;
    size_t m_read = 0;
    size_t m_size = 0;  // frames buffered
    int m_channels = 2;
    int m_sampleRate = 48000;
    int m_target = 2400;     // frames, current
    int m_minTarget = 2400;  // frames, as configured
    int m_max = 9600;
    bool m_priming = true;
    bool m_started = false;  // has played audio since Configure / Clear
    int64_t m_calmSinceNs = 0;
};

}  // namespace rc
