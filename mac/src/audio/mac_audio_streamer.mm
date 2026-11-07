#include "mac_audio_streamer.h"

#import <AVFoundation/AVFoundation.h>
#include <algorithm>
#include <cmath>

#include "rc/platform.h"

namespace rcm {

namespace {

constexpr int kFrameSamples = 480;  // 10 ms at 48 kHz
constexpr int kChannels = 2;
constexpr int kTargetSampleRate = 48000;
constexpr int kAudioBitrate = 96000; // 96 kbps stereo Opus

}  // namespace

MacAudioStreamer::MacAudioStreamer(rc::Connection* connection)
    : m_connection(connection) {}

MacAudioStreamer::~MacAudioStreamer() {
    Stop();
}

bool MacAudioStreamer::Start() {
    if (m_running) return true;

    if (!m_encoder.Init(kTargetSampleRate, kChannels, kAudioBitrate, true)) {
        RC_LOGE("Failed to initialize Opus audio encoder");
        return false;
    }

    m_sequence = 0;
    m_pcmBuffer.clear();
    m_pcmBuffer.reserve(kFrameSamples * kChannels * 4);
    m_running = true;

    RC_LOGI("MacAudioStreamer started (Opus %d Hz stereo @ %d bps)", kTargetSampleRate, kAudioBitrate);
    return true;
}

void MacAudioStreamer::Stop() {
    m_running = false;
    std::lock_guard lock(m_pcmMutex);
    m_pcmBuffer.clear();
}

void MacAudioStreamer::HandleAudioSampleBuffer(CMSampleBufferRef sampleBuffer) {
    if (!m_running || !m_connection) return;
    if (!CMSampleBufferIsValid(sampleBuffer)) return;

    CMFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
    if (!formatDesc) return;

    const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(formatDesc);
    if (!asbd) return;

    CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!blockBuffer) return;

    size_t lengthAtOffset = 0;
    size_t totalLength = 0;
    char* dataPointer = nullptr;
    OSStatus status = CMBlockBufferGetDataPointer(blockBuffer, 0, &lengthAtOffset, &totalLength, &dataPointer);
    if (status != noErr || !dataPointer || totalLength == 0) return;

    const uint32_t srcChannels = asbd->mChannelsPerFrame;
    const double srcSampleRate = asbd->mSampleRate;
    const bool isFloat = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    const bool isSignedInt = (asbd->mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0;
    const uint32_t bytesPerSample = asbd->mBitsPerChannel / 8;

    if (srcChannels == 0 || bytesPerSample == 0) return;

    const size_t numFrames = totalLength / (srcChannels * bytesPerSample);
    if (numFrames == 0) return;

    std::vector<int16_t> pcmInterleaved;
    pcmInterleaved.reserve(numFrames * kChannels);

    if (isFloat && bytesPerSample == 4) {
        const float* floatData = reinterpret_cast<const float*>(dataPointer);
        for (size_t i = 0; i < numFrames; ++i) {
            float l = floatData[i * srcChannels];
            float r = (srcChannels > 1) ? floatData[i * srcChannels + 1] : l;

            l = std::clamp(l, -1.0f, 1.0f);
            r = std::clamp(r, -1.0f, 1.0f);

            pcmInterleaved.push_back(static_cast<int16_t>(l * 32767.0f));
            pcmInterleaved.push_back(static_cast<int16_t>(r * 32767.0f));
        }
    } else if (isSignedInt && bytesPerSample == 2) {
        const int16_t* intData = reinterpret_cast<const int16_t*>(dataPointer);
        for (size_t i = 0; i < numFrames; ++i) {
            int16_t l = intData[i * srcChannels];
            int16_t r = (srcChannels > 1) ? intData[i * srcChannels + 1] : l;
            pcmInterleaved.push_back(l);
            pcmInterleaved.push_back(r);
        }
    } else {
        return;
    }

    // Resample if necessary (e.g. from 44.1 kHz to 48 kHz)
    if (std::abs(srcSampleRate - kTargetSampleRate) > 10.0) {
        const double step = srcSampleRate / kTargetSampleRate;
        const size_t outFrames = static_cast<size_t>(numFrames / step);
        std::vector<int16_t> resampled;
        resampled.reserve(outFrames * kChannels);

        for (size_t i = 0; i < outFrames; ++i) {
            double srcIdx = i * step;
            size_t idx0 = static_cast<size_t>(srcIdx);
            size_t idx1 = std::min(idx0 + 1, numFrames - 1);
            float frac = static_cast<float>(srcIdx - idx0);

            int16_t l0 = pcmInterleaved[idx0 * 2];
            int16_t l1 = pcmInterleaved[idx1 * 2];
            int16_t r0 = pcmInterleaved[idx0 * 2 + 1];
            int16_t r1 = pcmInterleaved[idx1 * 2 + 1];

            resampled.push_back(static_cast<int16_t>(l0 + frac * (l1 - l0)));
            resampled.push_back(static_cast<int16_t>(r0 + frac * (r1 - r0)));
        }
        ProcessPcm(resampled.data(), resampled.size());
    } else {
        ProcessPcm(pcmInterleaved.data(), pcmInterleaved.size());
    }
}

void MacAudioStreamer::ProcessPcm(const int16_t* samples, size_t count) {
    std::lock_guard lock(m_pcmMutex);
    m_pcmBuffer.insert(m_pcmBuffer.end(), samples, samples + count);

    uint8_t packet[1500];
    const size_t chunkSamples = kFrameSamples * kChannels;

    while (m_pcmBuffer.size() >= chunkSamples) {
        rc::AudioPacketHeader h{};
        h.stream = static_cast<uint8_t>(rc::AudioStream::PcToHeadset);
        h.channels = kChannels;
        h.frameSamples = kFrameSamples;
        h.sequence = ++m_sequence;
        h.timestampNs = static_cast<uint64_t>(rc::NowNs());

        int n = m_encoder.Encode(m_pcmBuffer.data(), kFrameSamples, packet, sizeof(packet) - 64);
        if (n > 0 && m_connection) {
            m_connection->Send2(rc::PacketType::Audio, &h, sizeof(h), packet, static_cast<size_t>(n));
        }

        m_pcmBuffer.erase(m_pcmBuffer.begin(), m_pcmBuffer.begin() + chunkSamples);
    }
}

}  // namespace rcm
