// Hardware video encoding on macOS with VideoToolbox (H.264, HEVC; 10-bit HEVC where the Mac
// has it). The shared VideoEncoder interface, so the sender's bitrate and loss handling are the
// same as on Windows; frames come in as ScreenCaptureKit's CVPixelBuffers.
#pragma once

#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include "encode/encoder.h"

namespace rcs {

class VtEncoder : public VideoEncoder {
   public:
    ~VtEncoder() override;

    bool Init(const EncoderConfig& config);
    // Synchronous: returns once the frame's bitstream (Annex B, parameter sets on keyframes)
    // is in `out`.
    bool Encode(CVPixelBufferRef input, bool forceKeyframe, EncodedFrame& out);

    void SetBitrate(uint32_t kbps) override;
    const char* Name() const override { return "VideoToolbox"; }

   private:
    void Shutdown();
    void OnOutput(OSStatus status, CMSampleBufferRef sample);
    static void Output(void* self, void* frameRef, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample);

    VTCompressionSessionRef m_session = nullptr;
    int64_t m_frame = 0;
    EncodedFrame m_out;
    bool m_haveOutput = false;
};

// What VideoToolbox can encode on this Mac.
EncoderCaps ProbeVtCaps();

}  // namespace rcs
