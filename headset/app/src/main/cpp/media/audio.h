// Headset audio: PC audio playback and microphone capture (AAudio + Opus).
#pragma once

#include <aaudio/AAudio.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "rc/audio_codec.h"

namespace rv {

class AudioPlayer {
   public:
    ~AudioPlayer() { Stop(); }
    bool Start();
    void Stop();
    // AudioPacketHeader + Opus payload from the network thread.
    void PushPacket(const uint8_t* data, size_t size);
    void SetVolume(float v) { m_volume = v; }
    int BufferedMs();

   private:
    static aaudio_data_callback_result_t Callback(AAudioStream*, void* user, void* audio, int32_t frames);
    static void ErrorCallback(AAudioStream*, void* user, aaudio_result_t error);

    AAudioStream* m_stream = nullptr;
    rc::AudioDecoder m_decoder;
    rc::JitterBuffer m_jitter;
    std::atomic<float> m_volume{1.f};
    std::atomic<bool> m_restart{false};
    uint32_t m_lastSeq = 0;
    std::mutex m_mutex;
};

class MicCapture {
   public:
    using PacketSink = std::function<void(const uint8_t* data, size_t size)>;
    ~MicCapture() { Stop(); }
    bool Start(PacketSink sink);
    void Stop();
    bool Running() const { return m_stream != nullptr; }
    void SetMuted(bool m) { m_muted = m; }

   private:
    static aaudio_data_callback_result_t Callback(AAudioStream*, void* user, void* audio, int32_t frames);

    AAudioStream* m_stream = nullptr;
    rc::AudioEncoder m_encoder;
    PacketSink m_sink;
    std::vector<int16_t> m_pending;
    uint32_t m_seq = 0;
    // What the microphone actually delivers (Android may not grant the 48 kHz mono we
    // ask for); converted to 48 kHz mono before encoding.
    int m_rate = 48000;
    int m_channels = 1;
    bool m_float = false;
    double m_resamplePos = 0;
    int16_t m_prevSample = 0;
    std::atomic<bool> m_muted{false};
};

}  // namespace rv
