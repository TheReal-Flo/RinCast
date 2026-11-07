#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#import <CoreMedia/CoreMedia.h>
#import <VideoToolbox/VideoToolbox.h>

#include "rc/protocol.h"

namespace rcm {

struct EncodedFrame {
    std::vector<uint8_t> data;
    bool keyframe = false;
    uint64_t timestamp = 0;  // timestamp in nanoseconds
};

class VideoToolboxEncoder {
   public:
    VideoToolboxEncoder();
    ~VideoToolboxEncoder();

    bool Init(rc::VideoCodec codec, uint32_t width, uint32_t height, float fps, uint32_t bitrateKbps);
    void Shutdown();

    // Encodes a CVPixelBuffer synchronously/asynchronously into Annex-B NALUs
    bool Encode(CVPixelBufferRef pixelBuffer, uint64_t timestampNs, bool forceKeyframe, EncodedFrame& outFrame);

    void SetBitrate(uint32_t bitrateKbps);
    void RequestKeyframe() { m_forceKeyframe = true; }

    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    rc::VideoCodec Codec() const { return m_codec; }

   private:
    void HandleOutputSample(CMSampleBufferRef sampleBuffer, uint64_t timestampNs);

    static void CompressionOutputCallback(void* outputCallbackRefCon,
                                          void* sourceFrameRefCon,
                                          OSStatus status,
                                          VTEncodeInfoFlags infoFlags,
                                          CMSampleBufferRef sampleBuffer);

    VTCompressionSessionRef m_session = nullptr;
    rc::VideoCodec m_codec = rc::VideoCodec::HEVC;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    float m_fps = 60.0f;
    uint32_t m_bitrateKbps = 20000;
    bool m_forceKeyframe = false;

    EncodedFrame m_currentFrame;
    bool m_hasOutput = false;
};

}  // namespace rcm
