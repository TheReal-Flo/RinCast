#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#import <CoreMedia/CoreMedia.h>

#include "rc/audio_codec.h"
#include "rc/connection.h"
#include "rc/protocol.h"

namespace rcm {

class MacAudioStreamer {
   public:
    explicit MacAudioStreamer(rc::Connection* connection);
    ~MacAudioStreamer();

    bool Start();
    void Stop();

    // Ingests audio samples from ScreenCaptureKit (SCStreamOutputTypeAudio)
    void HandleAudioSampleBuffer(CMSampleBufferRef sampleBuffer);

   private:
    void ProcessPcm(const int16_t* samples, size_t count);

    rc::Connection* m_connection = nullptr;
    rc::AudioEncoder m_encoder;
    std::atomic<bool> m_running{false};
    uint32_t m_sequence = 0;

    std::mutex m_pcmMutex;
    std::vector<int16_t> m_pcmBuffer;
};

}  // namespace rcm
