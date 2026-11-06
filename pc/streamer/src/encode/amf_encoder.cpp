// AMD AMF encoder (runtime amfrt64.dll ships with the Radeon driver).
#include <thread>

#include "amf/include/components/VideoEncoderAV1.h"
#include "amf/include/components/VideoEncoderHEVC.h"
#include "amf/include/components/VideoEncoderVCE.h"
#include "amf/include/core/Factory.h"
#include "encoder.h"
#include "rc/platform.h"

namespace rcs {

namespace {

struct AmfLib {
    HMODULE module = nullptr;
    amf::AMFFactory* factory = nullptr;

    static AmfLib& Get() {
        static AmfLib lib = [] {
            AmfLib l;
            l.module = LoadLibraryW(AMF_DLL_NAME);
            if (!l.module) return l;
            auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(l.module, AMF_INIT_FUNCTION_NAME));
            if (!init || init(AMF_FULL_VERSION, &l.factory) != AMF_OK) l.factory = nullptr;
            return l;
        }();
        return lib;
    }
};

const wchar_t* ComponentId(rc::VideoCodec codec) {
    switch (codec) {
        case rc::VideoCodec::H264: return AMFVideoEncoderVCE_AVC;
        case rc::VideoCodec::HEVC: return AMFVideoEncoder_HEVC;
        case rc::VideoCodec::AV1: return AMFVideoEncoder_AV1;
    }
    return AMFVideoEncoder_HEVC;
}

class AmfEncoder final : public VideoEncoder {
   public:
    ~AmfEncoder() override {
        if (m_encoder) m_encoder->Terminate();
        if (m_context) m_context->Terminate();
    }

    const char* Name() const override { return "AMF"; }

    bool Init(ID3D11Device* device, const EncoderConfig& config) override {
        m_config = config;
        auto& lib = AmfLib::Get();
        if (!lib.factory) return false;
        if (lib.factory->CreateContext(&m_context) != AMF_OK) return false;
        if (m_context->InitDX11(device) != AMF_OK) {
            RC_LOGE("AMF: InitDX11 failed");
            return false;
        }
        if (lib.factory->CreateComponent(m_context, ComponentId(config.codec), &m_encoder) != AMF_OK) {
            RC_LOGE("AMF: codec %s not supported", CodecName(config.codec));
            return false;
        }
        const AMFSize size = AMFConstructSize(int32_t(config.width), int32_t(config.height));
        const AMFRate rate = AMFConstructRate(uint32_t(config.fps * 1000), 1000);
        const int64_t bps = int64_t(config.bitrateKbps) * 1000;
        const int64_t vbv = Vbv(bps);
        // Optional extras: an older driver without one still encodes, just without it.
        bool aq = config.adaptiveQuant, twoPass = config.twoPass;
        auto optional = [&](const wchar_t* name, auto value, bool& flag) {
            if (flag && m_encoder->SetProperty(name, value) != AMF_OK) {
                RC_LOGW("AMF: %ls not supported by this driver", name);
                flag = false;
            }
        };

        switch (config.codec) {
            case rc::VideoCodec::H264:
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, size);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, rate);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bps);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bps);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, vbv);
                // CBR must not pad frames with filler data (wasted bandwidth); HRD keeps every frame
                // within the VBV, so frame sizes (and so transfer times) stay bounded.
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_ENFORCE_HRD, true);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, int64_t(kQueryTimeoutMs));
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, 0);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET,
                                       config.desktop ? AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED
                                                      : AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_ENABLE_VBAQ, aq);
                optional(AMF_VIDEO_ENCODER_PREENCODE_ENABLE, int64_t(AMF_VIDEO_ENCODER_PREENCODE_ENABLED), twoPass);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_BT709);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC,
                                       AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709);
                break;
            case rc::VideoCodec::HEVC:
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, size);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, rate);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
                                       AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bps);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bps);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, vbv);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE, false);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_ENFORCE_HRD, true);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT, int64_t(kQueryTimeoutMs));
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, 0);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, true);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
                                       config.desktop ? AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED
                                                      : AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ, aq);
                optional(AMF_VIDEO_ENCODER_HEVC_PREENCODE_ENABLE, true, twoPass);
                if (config.tenBit) {
                    m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE, AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10);
                    m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_COLOR_BIT_DEPTH, AMF_COLOR_BIT_DEPTH_10);
                }
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PRIMARIES, AMF_COLOR_PRIMARIES_BT709);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_TRANSFER_CHARACTERISTIC,
                                       AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709);
                break;
            case rc::VideoCodec::AV1:
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_USAGE, AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMESIZE, size);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMERATE, rate);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE,
                                       AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
                                       AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, bps);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, bps);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, vbv);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_FILLER_DATA, false);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ENFORCE_HRD, true);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT, int64_t(kQueryTimeoutMs));
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, 0);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE,
                                       AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE_KEY_FRAME_ALIGNED);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE,
                                       AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_LOWEST_LATENCY);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET,
                                       config.desktop ? AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED
                                                      : AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED);
                optional(AMF_VIDEO_ENCODER_AV1_AQ_MODE,
                         int64_t(aq ? AMF_VIDEO_ENCODER_AV1_AQ_MODE_CAQ : AMF_VIDEO_ENCODER_AV1_AQ_MODE_NONE), aq);
                optional(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_PREENCODE, true, twoPass);
                if (config.tenBit) m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_COLOR_BIT_DEPTH, AMF_COLOR_BIT_DEPTH_10);
                break;
        }
        const amf::AMF_SURFACE_FORMAT fmt = config.tenBit ? amf::AMF_SURFACE_P010 : amf::AMF_SURFACE_NV12;
        const AMF_RESULT res = m_encoder->Init(fmt, int32_t(config.width), int32_t(config.height));
        if (res != AMF_OK) {
            RC_LOGE("AMF: encoder init failed (%d)", int(res));
            return false;
        }
        RC_LOGI("AMF %s %ux%u @%.0f fps, %u kbps%s%s", CodecName(config.codec), config.width, config.height, config.fps,
                config.bitrateKbps, aq ? ", AQ" : "", twoPass ? ", two-pass" : "");
        return true;
    }

    bool Encode(ID3D11Texture2D* input, bool forceKeyframe, EncodedFrame& out) override {
        amf::AMFSurfacePtr surface;
        if (m_context->CreateSurfaceFromDX11Native(input, &surface, nullptr) != AMF_OK) return false;
        if (forceKeyframe || m_frames == 0) {
            switch (m_config.codec) {
                case rc::VideoCodec::H264:
                    surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
                    surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
                    surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
                    break;
                case rc::VideoCodec::HEVC:
                    surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
                                         AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
                    surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
                    break;
                case rc::VideoCodec::AV1:
                    surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE,
                                         AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY);
                    surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER, true);
                    break;
            }
        }
        m_frames++;
        AMF_RESULT res = m_encoder->SubmitInput(surface);
        if (res != AMF_OK && res != AMF_NEED_MORE_INPUT) {
            RC_LOGW("AMF: SubmitInput failed (%d)", int(res));
            return false;
        }
        // Ultra-low-latency usage produces output for each input without lookahead.
        // QueryOutput blocks for up to kQueryTimeoutMs on drivers that support it, which
        // returns the moment the frame is done; older drivers return AMF_REPEAT at once,
        // so poll those against a deadline (a fixed spin count can run out before a
        // large frame is finished).
        const int64_t deadline = rc::NowNs() + int64_t(kQueryTimeoutMs) * 1'000'000;
        while (rc::NowNs() < deadline) {
            amf::AMFDataPtr data;
            res = m_encoder->QueryOutput(&data);
            if (res == AMF_OK && data) {
                amf::AMFBufferPtr buffer(data);
                const auto* p = static_cast<const uint8_t*>(buffer->GetNative());
                out.data.assign(p, p + buffer->GetSize());
                int64_t type = -1;
                switch (m_config.codec) {
                    case rc::VideoCodec::H264:
                        buffer->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type);
                        out.keyframe = type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR;
                        break;
                    case rc::VideoCodec::HEVC:
                        buffer->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &type);
                        out.keyframe = type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR;
                        break;
                    case rc::VideoCodec::AV1:
                        buffer->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &type);
                        out.keyframe = type == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY;
                        break;
                }
                return true;
            }
            if (res != AMF_REPEAT && res != AMF_OK) return false;
            std::this_thread::yield();
        }
        RC_LOGW("AMF: no output within %d ms", kQueryTimeoutMs);
        return false;
    }

    void SetBitrate(uint32_t kbps) override {
        if (!m_encoder) return;
        m_config.bitrateKbps = kbps;
        const int64_t bps = int64_t(kbps) * 1000;
        // The VBV follows the bitrate, or a lowered bitrate would still allow frames
        // sized for the old one.
        const int64_t vbv = Vbv(bps);
        switch (m_config.codec) {
            case rc::VideoCodec::H264:
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bps);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bps);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, vbv);
                break;
            case rc::VideoCodec::HEVC:
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bps);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bps);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, vbv);
                break;
            case rc::VideoCodec::AV1:
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, bps);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, bps);
                m_encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, vbv);
                break;
        }
    }

   private:
    static constexpr int kQueryTimeoutMs = 100;

    // One frame of VBV (two for desktop content, for sudden full-screen changes).
    int64_t Vbv(int64_t bps) const { return int64_t(double(bps) / m_config.fps) * (m_config.desktop ? 2 : 1); }

    amf::AMFContextPtr m_context;
    amf::AMFComponentPtr m_encoder;
    uint64_t m_frames = 0;
};

}  // namespace

bool AmfProbe(ID3D11Device* device, EncoderCaps& caps) {
    auto& lib = AmfLib::Get();
    if (!lib.factory) return false;
    amf::AMFContextPtr ctx;
    if (lib.factory->CreateContext(&ctx) != AMF_OK || ctx->InitDX11(device) != AMF_OK) return false;
    caps.backend = "amf";
    auto test = [&](const wchar_t* id) {
        amf::AMFComponentPtr c;
        return lib.factory->CreateComponent(ctx, id, &c) == AMF_OK;
    };
    caps.h264 = test(AMFVideoEncoderVCE_AVC);
    caps.hevc = test(AMFVideoEncoder_HEVC);
    caps.av1 = test(AMFVideoEncoder_AV1);
    caps.hevc10 = caps.hevc;
    caps.av110 = caps.av1;
    caps.maxWidth = 7680;
    caps.maxHeight = 4320;
    ctx->Terminate();
    return caps.h264 || caps.hevc;
}

std::unique_ptr<VideoEncoder> CreateAmfEncoder() { return std::make_unique<AmfEncoder>(); }

}  // namespace rcs
