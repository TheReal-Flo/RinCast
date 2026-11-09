#include "mac_audio_streamer.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
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

    const CMItemCount numFrames = CMSampleBufferGetNumSamples(sampleBuffer);
    if (numFrames <= 0) return;

    size_t bufferListSizeNeeded = 0;
    OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sampleBuffer,
        &bufferListSizeNeeded,
        NULL,
        0,
        NULL,
        NULL,
        kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
        NULL
    );

    if (status != noErr || bufferListSizeNeeded == 0) return;

    std::vector<uint8_t> ablStorage(bufferListSizeNeeded);
    AudioBufferList* abl = reinterpret_cast<AudioBufferList*>(ablStorage.data());
    CMBlockBufferRef blockBuffer = nullptr;

    status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sampleBuffer,
        NULL,
        abl,
        bufferListSizeNeeded,
        NULL,
        NULL,
        kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
        &blockBuffer
    );

    if (status != noErr) {
        if (blockBuffer) CFRelease(blockBuffer);
        return;
    }

    const double srcSampleRate = asbd->mSampleRate;
    const bool isFloat = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    const bool isSignedInt = (asbd->mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0;
    const uint32_t bytesPerSample = asbd->mBitsPerChannel / 8;

    std::vector<int16_t> pcmInterleaved;
    pcmInterleaved.reserve(numFrames * kChannels);

    if (abl->mNumberBuffers >= 2 && isFloat && bytesPerSample == 4) {
        // Non-interleaved stereo float (ScreenCaptureKit default: Buffer 0 = Left, Buffer 1 = Right)
        const float* leftChannel = reinterpret_cast<const float*>(abl->mBuffers[0].mData);
        const float* rightChannel = reinterpret_cast<const float*>(abl->mBuffers[1].mData);

        if (leftChannel && rightChannel) {
            for (CMItemCount i = 0; i < numFrames; ++i) {
                float l = std::clamp(leftChannel[i], -1.0f, 1.0f);
                float r = std::clamp(rightChannel[i], -1.0f, 1.0f);
                pcmInterleaved.push_back(static_cast<int16_t>(l * 32767.0f));
                pcmInterleaved.push_back(static_cast<int16_t>(r * 32767.0f));
            }
        }
    } else if (abl->mNumberBuffers == 1 && isFloat && bytesPerSample == 4) {
        // Interleaved stereo float (or mono)
        const float* floatData = reinterpret_cast<const float*>(abl->mBuffers[0].mData);
        const uint32_t ch = abl->mBuffers[0].mNumberChannels;
        if (floatData) {
            for (CMItemCount i = 0; i < numFrames; ++i) {
                float l = std::clamp(floatData[i * ch], -1.0f, 1.0f);
                float r = (ch > 1) ? std::clamp(floatData[i * ch + 1], -1.0f, 1.0f) : l;
                pcmInterleaved.push_back(static_cast<int16_t>(l * 32767.0f));
                pcmInterleaved.push_back(static_cast<int16_t>(r * 32767.0f));
            }
        }
    } else if (abl->mNumberBuffers == 1 && isSignedInt && bytesPerSample == 2) {
        // Interleaved 16-bit int
        const int16_t* intData = reinterpret_cast<const int16_t*>(abl->mBuffers[0].mData);
        const uint32_t ch = abl->mBuffers[0].mNumberChannels;
        if (intData) {
            for (CMItemCount i = 0; i < numFrames; ++i) {
                int16_t l = intData[i * ch];
                int16_t r = (ch > 1) ? intData[i * ch + 1] : l;
                pcmInterleaved.push_back(l);
                pcmInterleaved.push_back(r);
            }
        }
    }

    if (blockBuffer) {
        CFRelease(blockBuffer);
    }

    if (pcmInterleaved.empty()) return;

    // Resample if necessary (e.g. from 44.1 kHz to 48 kHz)
    if (std::abs(srcSampleRate - kTargetSampleRate) > 10.0 && srcSampleRate > 0) {
        const double step = srcSampleRate / kTargetSampleRate;
        const size_t outFrames = static_cast<size_t>(numFrames / step);
        std::vector<int16_t> resampled;
        resampled.reserve(outFrames * kChannels);

        for (size_t i = 0; i < outFrames; ++i) {
            double srcIdx = i * step;
            size_t idx0 = static_cast<size_t>(srcIdx);
            size_t idx1 = std::min(idx0 + 1, static_cast<size_t>(numFrames - 1));
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
