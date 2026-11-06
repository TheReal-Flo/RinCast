// Hardware video encoder abstraction (NVENC, AMF, Media Foundation).
#pragma once

#include <d3d11_4.h>

#include <memory>
#include <string>
#include <vector>

#include "../util.h"
#include "rc/protocol.h"

namespace rcs {

struct EncoderConfig {
    rc::VideoCodec codec = rc::VideoCodec::HEVC;
    uint32_t width = 0;
    uint32_t height = 0;
    float fps = 90;
    uint32_t bitrateKbps = 100000;
    bool tenBit = false;
    // Desktop content: favour sharpness (more bits for I-frames, slower preset).
    bool desktop = false;
    DXGI_FORMAT inputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;  // or R8G8B8A8_UNORM
    // VR: spend fewer bits towards the lens edges, where the Quest's optics blur the
    // image anyway (needs SetFoveation; NVENC only).
    bool foveated = false;
    // Use every NVENC engine for one frame on GPUs that have more than one (HEVC/AV1).
    bool splitFrame = true;
    // More bits for flat areas (skies, walls), where blocking shows first; fewer for busy
    // detail that hides it. NVENC and AMF.
    bool adaptiveQuant = true;
    // A quick first pass over the frame before the real one, so rate control knows where
    // the detail is. Costs encode time. NVENC and AMF.
    bool twoPass = false;
};

struct EncodedFrame {
    std::vector<uint8_t> data;
    bool keyframe = false;
    uint64_t timestamp = 0;  // encoder-side id, for InvalidateAfter
};

class VideoEncoder {
   public:
    virtual ~VideoEncoder() = default;
    virtual bool Init(ID3D11Device* device, const EncoderConfig& config) = 0;
    // Encodes `input` (same size/format as configured). Synchronous: returns
    // once the bitstream is available.
    virtual bool Encode(ID3D11Texture2D* input, bool forceKeyframe, EncodedFrame& out) = 0;
    virtual void SetBitrate(uint32_t kbps) = 0;
    virtual const char* Name() const = 0;
    // Loss recovery without a keyframe: marks every frame encoded after `timestamp`
    // as unusable, so the next frame only references `timestamp` or older. Returns
    // false if that isn't possible (the caller then forces a keyframe).
    virtual bool InvalidateAfter(uint64_t /*timestamp*/) { return false; }
    // Side-by-side VR eyes: per-eye left, right, up, down tangents of the field of view.
    virtual void SetFoveation(const float /*fovTan*/[2][4]) {}
    const EncoderConfig& Config() const { return m_config; }

   protected:
    EncoderConfig m_config;
};

struct EncoderCaps {
    std::string backend;  // nvenc | amf | mf
    bool h264 = false;
    bool hevc = false;
    bool av1 = false;
    bool hevc10 = false;
    bool av110 = false;
    uint32_t maxWidth = 4096;
    uint32_t maxHeight = 4096;
};

// Probes what the adapter's hardware encoder supports.
EncoderCaps ProbeEncoderCaps(ID3D11Device* device, UINT vendorId);

// Creates the best encoder for the adapter: NVENC on NVIDIA, AMF on AMD,
// Media Foundation (Intel QSV and fallback) otherwise.
std::unique_ptr<VideoEncoder> CreateEncoder(ID3D11Device* device, UINT vendorId, const EncoderConfig& config);

std::unique_ptr<VideoEncoder> CreateNvencEncoder();
std::unique_ptr<VideoEncoder> CreateAmfEncoder();
std::unique_ptr<VideoEncoder> CreateMfEncoder();
bool NvencProbe(ID3D11Device* device, EncoderCaps& caps);
bool AmfProbe(ID3D11Device* device, EncoderCaps& caps);
bool MfProbe(ID3D11Device* device, EncoderCaps& caps);

const char* CodecName(rc::VideoCodec codec);

}  // namespace rcs
