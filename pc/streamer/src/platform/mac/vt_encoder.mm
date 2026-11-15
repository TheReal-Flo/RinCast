#include "vt_encoder.h"

#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>

#include <climits>
#include <cstring>

#include "rc/platform.h"

namespace rcs {

namespace {

constexpr uint8_t kStartCode[4] = {0, 0, 0, 1};

void SetInt(VTCompressionSessionRef s, CFStringRef key, int64_t value) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
    VTSessionSetProperty(s, key, n);
    CFRelease(n);
}

void SetDouble(VTCompressionSessionRef s, CFStringRef key, double value) {
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &value);
    VTSessionSetProperty(s, key, n);
    CFRelease(n);
}

CMVideoCodecType CodecType(rc::VideoCodec codec) {
    return codec == rc::VideoCodec::H264 ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC;
}

// A hardware session, low-latency rate control first (one frame in, one out, the bitrate held
// per frame), plain hardware where the Mac doesn't offer that for the codec.
VTCompressionSessionRef CreateSession(const EncoderConfig& c, VTCompressionOutputCallback cb, void* self) {
    NSDictionary* source = @{
        (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (id)kCVPixelBufferWidthKey: @(c.width),
        (id)kCVPixelBufferHeightKey: @(c.height),
    };
    for (int attempt = 0; attempt < 2; ++attempt) {
        NSMutableDictionary* spec = [@{(id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES} mutableCopy];
        if (attempt == 0) {
            if (@available(macOS 11.3, *)) spec[(id)kVTVideoEncoderSpecification_EnableLowLatencyRateControl] = @YES;
            else continue;
        }
        VTCompressionSessionRef session = nullptr;
        const OSStatus status = VTCompressionSessionCreate(kCFAllocatorDefault, int32_t(c.width), int32_t(c.height), CodecType(c.codec),
                                                           (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)source,
                                                           kCFAllocatorDefault, cb, self, &session);
        if (status == noErr && session) {
            RC_LOGI("VideoToolbox: %s session%s", c.codec == rc::VideoCodec::H264 ? "H.264" : "HEVC",
                    attempt == 0 ? " with low-latency rate control" : "");
            return session;
        }
        RC_LOGI("VideoToolbox: %s session %s failed (%d)", c.codec == rc::VideoCodec::H264 ? "H.264" : "HEVC",
                attempt == 0 ? "with low-latency rate control" : "in hardware", int(status));
    }
    return nullptr;
}

}  // namespace

VtEncoder::~VtEncoder() { Shutdown(); }

bool VtEncoder::Init(const EncoderConfig& config) {
    Shutdown();
    m_config = config;
    if (config.codec == rc::VideoCodec::AV1) return false;  // not offered (see ProbeVtCaps)
    m_session = CreateSession(config, &VtEncoder::Output, this);
    if (!m_session) return false;

    VTSessionSetProperty(m_session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(m_session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);  // no B-frames
    SetInt(m_session, kVTCompressionPropertyKey_MaxFrameDelayCount, 0);
    const bool tenBit = config.tenBit && config.codec == rc::VideoCodec::HEVC;
    VTSessionSetProperty(m_session, kVTCompressionPropertyKey_ProfileLevel,
                         config.codec == rc::VideoCodec::H264 ? kVTProfileLevel_H264_High_AutoLevel
                         : tenBit                             ? kVTProfileLevel_HEVC_Main10_AutoLevel
                                                              : kVTProfileLevel_HEVC_Main_AutoLevel);
    // BT.709, as the Windows encoders write it, so the headset decodes the colours the same way.
    VTSessionSetProperty(m_session, kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
    VTSessionSetProperty(m_session, kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
    VTSessionSetProperty(m_session, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    // Desktops: sharp text matters more than the last millisecond (the Windows encoders' two
    // passes and slower preset).
    if (@available(macOS 13.0, *)) {
        VTSessionSetProperty(m_session, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
                             config.desktop ? kCFBooleanFalse : kCFBooleanTrue);
    }
    SetDouble(m_session, kVTCompressionPropertyKey_ExpectedFrameRate, config.fps);
    // Keyframes only when the headset needs one (start, loss), as on Windows: a periodic one
    // would be a burst several times a frame's size.
    SetInt(m_session, kVTCompressionPropertyKey_MaxKeyFrameInterval, INT_MAX);
    SetDouble(m_session, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, 0);  // no limit
    SetBitrate(config.bitrateKbps);

    const OSStatus status = VTCompressionSessionPrepareToEncodeFrames(m_session);
    if (status != noErr) {
        RC_LOGE("VideoToolbox: preparing the session failed (%d)", int(status));
        Shutdown();
        return false;
    }
    RC_LOGI("VideoToolbox: %ux%u @ %.0f fps, %u kbps%s", config.width, config.height, config.fps, config.bitrateKbps,
            tenBit ? ", 10-bit" : "");
    return true;
}

void VtEncoder::Shutdown() {
    if (!m_session) return;
    VTCompressionSessionInvalidate(m_session);
    CFRelease(m_session);
    m_session = nullptr;
}

void VtEncoder::SetBitrate(uint32_t kbps) {
    m_config.bitrateKbps = kbps;
    if (!m_session) return;
    SetInt(m_session, kVTCompressionPropertyKey_AverageBitRate, int64_t(kbps) * 1000);
    // At most one and a half times the rate in any second: a scroll may take more for a moment,
    // but not so much that it floods the link.
    const int64_t bytes = int64_t(kbps) * 1000 * 3 / 2 / 8;
    const double seconds = 1.0;
    CFNumberRef b = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &bytes);
    CFNumberRef s = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &seconds);
    const void* values[2] = {b, s};
    CFArrayRef limits = CFArrayCreate(kCFAllocatorDefault, values, 2, &kCFTypeArrayCallBacks);
    VTSessionSetProperty(m_session, kVTCompressionPropertyKey_DataRateLimits, limits);
    CFRelease(limits);
    CFRelease(b);
    CFRelease(s);
}

bool VtEncoder::Encode(CVPixelBufferRef input, bool forceKeyframe, EncodedFrame& out) {
    if (!m_session || !input) return false;
    m_haveOutput = false;
    const CMTime pts = CMTimeMake(m_frame++, 1000000);
    NSDictionary* props = forceKeyframe ? @{(id)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES} : nil;
    const OSStatus status =
        VTCompressionSessionEncodeFrame(m_session, input, pts, kCMTimeInvalid, (__bridge CFDictionaryRef)props, nullptr, nullptr);
    if (status != noErr) {
        RC_LOGW("VideoToolbox: encoding failed (%d)", int(status));
        return false;
    }
    VTCompressionSessionCompleteFrames(m_session, pts);  // the output callback has run
    if (!m_haveOutput) return false;
    m_out.timestamp = uint64_t(m_frame);
    out = std::move(m_out);
    m_out = {};
    return true;
}

void VtEncoder::Output(void* self, void*, OSStatus status, VTEncodeInfoFlags, CMSampleBufferRef sample) {
    static_cast<VtEncoder*>(self)->OnOutput(status, sample);
}

// AVCC/HVCC (length-prefixed NAL units) to Annex B, with the parameter sets on keyframes.
void VtEncoder::OnOutput(OSStatus status, CMSampleBufferRef sample) {
    if (status != noErr || !sample) return;
    m_out.data.clear();
    bool keyframe = true;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
        auto notSync = static_cast<CFBooleanRef>(CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync));
        keyframe = !(notSync && CFBooleanGetValue(notSync));
    }
    m_out.keyframe = keyframe;

    if (keyframe) {
        if (CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample)) {
            size_t count = 0;
            const bool h264 = m_config.codec == rc::VideoCodec::H264;
            const OSStatus got = h264 ? CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, nullptr, nullptr, &count, nullptr)
                                      : CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, nullptr, nullptr, &count, nullptr);
            for (size_t i = 0; got == noErr && i < count; ++i) {
                const uint8_t* ps = nullptr;
                size_t size = 0;
                const OSStatus ok = h264 ? CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &ps, &size, nullptr, nullptr)
                                         : CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, i, &ps, &size, nullptr, nullptr);
                if (ok != noErr) continue;
                m_out.data.insert(m_out.data.end(), kStartCode, kStartCode + 4);
                m_out.data.insert(m_out.data.end(), ps, ps + size);
            }
        }
    }

    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!block) return;
    size_t total = 0;
    char* data = nullptr;
    if (CMBlockBufferGetDataPointer(block, 0, nullptr, &total, &data) != noErr || !data) return;
    for (size_t offset = 0; offset + 4 <= total;) {
        uint32_t length;
        std::memcpy(&length, data + offset, 4);
        length = CFSwapInt32BigToHost(length);
        offset += 4;
        if (offset + length > total) break;
        m_out.data.insert(m_out.data.end(), kStartCode, kStartCode + 4);
        m_out.data.insert(m_out.data.end(), data + offset, data + offset + length);
        offset += length;
    }
    m_haveOutput = !m_out.data.empty();
}

EncoderCaps ProbeVtCaps() {
    EncoderCaps caps;
    caps.backend = "videotoolbox";
    // A session that can be made is a codec the Mac encodes in hardware.
    auto can = [](rc::VideoCodec codec, bool tenBit) {
        EncoderConfig c;
        c.codec = codec;
        c.width = 1920;
        c.height = 1080;
        c.tenBit = tenBit;
        VtEncoder e;
        return e.Init(c);
    };
    caps.h264 = can(rc::VideoCodec::H264, false);
    caps.hevc = can(rc::VideoCodec::HEVC, false);
    caps.hevc10 = caps.hevc && can(rc::VideoCodec::HEVC, true);
    caps.maxWidth = caps.maxHeight = 4096;
    RC_LOGI("VideoToolbox: H.264 %s, HEVC %s%s", caps.h264 ? "yes" : "no", caps.hevc ? "yes" : "no", caps.hevc10 ? " (10-bit too)" : "");
    return caps;
}

}  // namespace rcs

namespace rcs {

EncoderCaps ProbeEncoderCaps(UINT) { return ProbeVtCaps(); }

}  // namespace rcs
