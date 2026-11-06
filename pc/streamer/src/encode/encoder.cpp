#include "encoder.h"

#include "rc/platform.h"

namespace rcs {

const char* CodecName(rc::VideoCodec codec) {
    switch (codec) {
        case rc::VideoCodec::H264: return "H.264";
        case rc::VideoCodec::HEVC: return "HEVC";
        case rc::VideoCodec::AV1: return "AV1";
    }
    return "?";
}

EncoderCaps ProbeEncoderCaps(ID3D11Device* device, UINT vendorId) {
    EncoderCaps caps;
    if (vendorId == 0x10DE && NvencProbe(device, caps)) return caps;
    caps = {};
    if (vendorId == 0x1002 && AmfProbe(device, caps)) return caps;
    caps = {};
    MfProbe(device, caps);
    return caps;
}

std::unique_ptr<VideoEncoder> CreateEncoder(ID3D11Device* device, UINT vendorId, const EncoderConfig& config) {
    std::unique_ptr<VideoEncoder> enc;
    if (vendorId == 0x10DE) {
        enc = CreateNvencEncoder();
        if (enc->Init(device, config)) return enc;
    }
    if (vendorId == 0x1002) {
        enc = CreateAmfEncoder();
        if (enc->Init(device, config)) return enc;
    }
    enc = CreateMfEncoder();
    if (enc->Init(device, config)) return enc;
    RC_LOGE("No hardware encoder could be created for %s %ux%u", CodecName(config.codec), config.width, config.height);
    return nullptr;
}

}  // namespace rcs
