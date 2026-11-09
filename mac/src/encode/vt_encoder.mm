#include "vt_encoder.h"

#include <cmath>
#include <cstring>

#import <CoreFoundation/CoreFoundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include "rc/platform.h"

namespace rcm {

static const uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

VideoToolboxEncoder::VideoToolboxEncoder() = default;

VideoToolboxEncoder::~VideoToolboxEncoder() {
    Shutdown();
}

bool VideoToolboxEncoder::Init(rc::VideoCodec codec, uint32_t width, uint32_t height, float fps, uint32_t bitrateKbps) {
    Shutdown();

    m_codec = codec;
    m_width = width;
    m_height = height;
    m_fps = fps > 0 ? fps : 60.0f;
    m_bitrateKbps = bitrateKbps > 0 ? bitrateKbps : 20000;

    CMVideoCodecType vtCodec = kCMVideoCodecType_HEVC;
    if (codec == rc::VideoCodec::H264) {
        vtCodec = kCMVideoCodecType_H264;
    }

    NSDictionary* encoderSpec = @{
        (id)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES,
    };

    NSDictionary* sourceImageBufferAttributes = @{
        (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (id)kCVPixelBufferWidthKey: @(m_width),
        (id)kCVPixelBufferHeightKey: @(m_height),
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };

    OSStatus status = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        m_width,
        m_height,
        vtCodec,
        (__bridge CFDictionaryRef)encoderSpec,
        (__bridge CFDictionaryRef)sourceImageBufferAttributes,
        kCFAllocatorDefault,
        &VideoToolboxEncoder::CompressionOutputCallback,
        this,
        &m_session
    );

    if (status != noErr || !m_session) {
        RC_LOGE("VTCompressionSessionCreate failed: %d", (int)status);
        return false;
    }

    // Ultra-low latency real-time streaming configuration
    VTSessionSetProperty(m_session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(m_session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

    if (codec == rc::VideoCodec::H264) {
        VTSessionSetProperty(m_session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
    } else {
        VTSessionSetProperty(m_session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main_AutoLevel);
    }

    // Set bitrates
    SetBitrate(m_bitrateKbps);

    // Disable forced periodic keyframes (generate on demand or at long interval)
    int maxKeyFrameInterval = static_cast<int>(m_fps * 10);
    CFNumberRef maxKeyRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &maxKeyFrameInterval);
    VTSessionSetProperty(m_session, kVTCompressionPropertyKey_MaxKeyFrameInterval, maxKeyRef);
    CFRelease(maxKeyRef);

    double maxKeyIntervalDuration = 10.0;
    CFNumberRef maxKeyDurationRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &maxKeyIntervalDuration);
    VTSessionSetProperty(m_session, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, maxKeyDurationRef);
    CFRelease(maxKeyDurationRef);

    status = VTCompressionSessionPrepareToEncodeFrames(m_session);
    if (status != noErr) {
        RC_LOGE("VTCompressionSessionPrepareToEncodeFrames failed: %d", (int)status);
        Shutdown();
        return false;
    }

    RC_LOGI("VideoToolbox encoder initialized (%s, %ux%u @ %.1f fps, %u kbps)",
            codec == rc::VideoCodec::H264 ? "H.264" : "HEVC", m_width, m_height, m_fps, m_bitrateKbps);
    return true;
}

void VideoToolboxEncoder::Shutdown() {
    if (m_session) {
        VTCompressionSessionInvalidate(m_session);
        CFRelease(m_session);
        m_session = nullptr;
    }
}

void VideoToolboxEncoder::SetBitrate(uint32_t bitrateKbps) {
    m_bitrateKbps = bitrateKbps;
    if (!m_session) return;

    SInt32 bitRate = static_cast<SInt32>(bitrateKbps * 1000);
    CFNumberRef bitRateRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bitRate);
    VTSessionSetProperty(m_session, kVTCompressionPropertyKey_AverageBitRate, bitRateRef);
    CFRelease(bitRateRef);

    // Rate limits: allow up to 1.5x in bursts over 1 second window
    SInt32 dataLimitBytes = static_cast<SInt32>((bitrateKbps * 1000 * 1.5) / 8);
    CFNumberRef bytesLimitRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &dataLimitBytes);
    double duration = 1.0;
    CFNumberRef durationRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &duration);
    const void* limitsArray[2] = {bytesLimitRef, durationRef};
    CFArrayRef limits = CFArrayCreate(kCFAllocatorDefault, limitsArray, 2, &kCFTypeArrayCallBacks);
    VTSessionSetProperty(m_session, kVTCompressionPropertyKey_DataRateLimits, limits);
    CFRelease(limits);
    CFRelease(bytesLimitRef);
    CFRelease(durationRef);
}

void VideoToolboxEncoder::CompressionOutputCallback(void* outputCallbackRefCon,
                                                    void* sourceFrameRefCon,
                                                    OSStatus status,
                                                    VTEncodeInfoFlags infoFlags,
                                                    CMSampleBufferRef sampleBuffer) {
    auto* self = static_cast<VideoToolboxEncoder*>(outputCallbackRefCon);
    if (!self || status != noErr || !sampleBuffer) {
        return;
    }
    uint64_t ts = reinterpret_cast<uint64_t>(sourceFrameRefCon);
    self->HandleOutputSample(sampleBuffer, ts);
}

void VideoToolboxEncoder::HandleOutputSample(CMSampleBufferRef sampleBuffer, uint64_t timestampNs) {
    m_currentFrame.data.clear();
    m_currentFrame.timestamp = timestampNs;

    // Check if this is a keyframe
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    bool isKeyframe = true;
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef attachment = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFBooleanRef notSync = (CFBooleanRef)CFDictionaryGetValue(attachment, kCMSampleAttachmentKey_NotSync);
        if (notSync && CFBooleanGetValue(notSync)) {
            isKeyframe = false;
        }
    }
    m_currentFrame.keyframe = isKeyframe;

    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);

    // If keyframe, prepend parameter sets (SPS/PPS/VPS) in Annex-B format
    if (isKeyframe && format) {
        if (m_codec == rc::VideoCodec::H264) {
            size_t spsCount = 0;
            const uint8_t* sps = nullptr;
            size_t spsSize = 0;
            if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &sps, &spsSize, &spsCount, nullptr) == noErr) {
                m_currentFrame.data.insert(m_currentFrame.data.end(), kStartCode, kStartCode + 4);
                m_currentFrame.data.insert(m_currentFrame.data.end(), sps, sps + spsSize);
            }
            const uint8_t* pps = nullptr;
            size_t ppsSize = 0;
            if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 1, &pps, &ppsSize, nullptr, nullptr) == noErr) {
                m_currentFrame.data.insert(m_currentFrame.data.end(), kStartCode, kStartCode + 4);
                m_currentFrame.data.insert(m_currentFrame.data.end(), pps, pps + ppsSize);
            }
        } else if (m_codec == rc::VideoCodec::HEVC) {
            size_t paramCount = 0;
            if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, nullptr, nullptr, &paramCount, nullptr) == noErr) {
                for (size_t i = 0; i < paramCount; ++i) {
                    const uint8_t* param = nullptr;
                    size_t paramSize = 0;
                    if (CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, i, &param, &paramSize, nullptr, nullptr) == noErr) {
                        m_currentFrame.data.insert(m_currentFrame.data.end(), kStartCode, kStartCode + 4);
                        m_currentFrame.data.insert(m_currentFrame.data.end(), param, param + paramSize);
                    }
                }
            }
        }
    }

    // Extract slice NALUs and convert 4-byte length headers to Annex-B start codes
    CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!blockBuffer) return;

    size_t totalLength = 0;
    char* dataPointer = nullptr;
    OSStatus status = CMBlockBufferGetDataPointer(blockBuffer, 0, nullptr, &totalLength, &dataPointer);
    if (status != noErr || !dataPointer) return;

    size_t offset = 0;
    while (offset + 4 <= totalLength) {
        uint32_t naluLength = CFSwapInt32BigToHost(*reinterpret_cast<uint32_t*>(dataPointer + offset));
        offset += 4;
        if (offset + naluLength > totalLength) break;

        m_currentFrame.data.insert(m_currentFrame.data.end(), kStartCode, kStartCode + 4);
        m_currentFrame.data.insert(m_currentFrame.data.end(), dataPointer + offset, dataPointer + offset + naluLength);
        offset += naluLength;
    }

    m_hasOutput = !m_currentFrame.data.empty();
}

bool VideoToolboxEncoder::Encode(CVPixelBufferRef pixelBuffer, uint64_t timestampNs, bool forceKeyframe, EncodedFrame& outFrame) {
    if (!m_session || !pixelBuffer) return false;

    m_hasOutput = false;
    CMTime pts = CMTimeMake(timestampNs, 1000000000);
    CMTime duration = CMTimeMake(1, static_cast<int32_t>(m_fps));

    NSDictionary* frameProperties = nil;
    if (forceKeyframe || m_forceKeyframe) {
        frameProperties = @{(id)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES};
        m_forceKeyframe = false;
    }

    VTEncodeInfoFlags flags = 0;
    OSStatus status = VTCompressionSessionEncodeFrame(
        m_session,
        pixelBuffer,
        pts,
        duration,
        (__bridge CFDictionaryRef)frameProperties,
        reinterpret_cast<void*>(timestampNs),
        &flags
    );

    if (status != noErr) {
        RC_LOGE("VTCompressionSessionEncodeFrame failed: %d", (int)status);
        return false;
    }

    // Wait for the synchronous output
    VTCompressionSessionCompleteFrames(m_session, pts);

    if (m_hasOutput) {
        outFrame = std::move(m_currentFrame);
        return true;
    }

    return false;
}

}  // namespace rcm
