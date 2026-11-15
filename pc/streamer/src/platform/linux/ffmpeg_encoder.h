// Video encoding on Linux with FFmpeg (libavcodec): VAAPI on AMD and Intel, NVENC on NVIDIA,
// libx264 / libx265 in software as the last resort. The shared VideoEncoder interface, so the
// sender's bitrate and loss handling are the same as on Windows. Pictures come in as BGRA in
// system memory (X11, PipeWire's shared memory, a read-back) or as DMA-BUF images (PipeWire,
// the VR bridge's frames), which VAAPI takes without a copy.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "encode/encoder.h"

namespace rcs {

// A GPU image shared as DMA-BUF. The fds stay the caller's.
struct DmaBufFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t drmFourcc = 0;  // DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_ABGR8888, ...
    uint64_t modifier = 0;   // DRM_FORMAT_MOD_LINEAR = 0
    int planeCount = 1;
    struct Plane {
        int fd = -1;
        uint32_t offset = 0;
        uint32_t pitch = 0;
    } planes[4];
};

class FfmpegEncoder : public VideoEncoder {
   public:
    FfmpegEncoder();
    ~FfmpegEncoder() override;

    // Picks the backend ProbeEncoderCaps found (VAAPI, NVENC, software) for config.codec.
    bool Init(const EncoderConfig& config);
    // 32-bit BGRX / BGRA pixels, `stride` bytes per row. Synchronous: returns once the frame's
    // bitstream (Annex B, parameter sets on keyframes) is in `out`.
    bool EncodeBgra(const uint8_t* pixels, int stride, bool forceKeyframe, EncodedFrame& out);
    // The same with the bytes in R, G, B, A order (Vulkan's R8G8B8A8_UNORM, the VR frames).
    bool EncodeRgba(const uint8_t* pixels, int stride, bool forceKeyframe, EncodedFrame& out);
    // A DMA-BUF image of the configured size (DRM_FORMAT_ARGB8888 / XRGB8888 / ABGR8888 /
    // XBGR8888): imported without a copy on VAAPI, mapped and read when linear otherwise.
    bool EncodeDmaBuf(const DmaBufFrame& frame, bool forceKeyframe, EncodedFrame& out);
    // True when EncodeDmaBuf imports the image without a copy (VAAPI): a caller holding a GPU
    // image needs no read-back of its own then.
    bool TakesDmaBuf() const;

    void SetBitrate(uint32_t kbps) override;
    const char* Name() const override;
    // "vaapi", "nvenc" or "software": what Init picked.
    const char* Backend() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// The DRM render node the encoder uses ("/dev/dri/renderD128"): the GPU DescribeGpu reports.
std::string RenderNode();

}  // namespace rcs
