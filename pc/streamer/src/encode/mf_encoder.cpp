// Media Foundation hardware encoder MFT. Covers Intel Quick Sync and acts as
// a generic fallback for any GPU whose vendor ships H.264/HEVC/AV1 MFTs.
#include <strmif.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include "encoder.h"
#include "rc/platform.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

#ifndef MFVideoFormat_AV1
// {31305641-0000-0010-8000-00AA00389B71}
static const GUID MFVideoFormat_AV1_RC = {0x31305641, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
#define RC_MF_AV1 MFVideoFormat_AV1_RC
#else
#define RC_MF_AV1 MFVideoFormat_AV1
#endif

namespace rcs {

namespace {

void EnsureMfStarted() {
    static bool started = [] { return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL)); }();
    (void)started;
}

GUID Subtype(rc::VideoCodec codec) {
    switch (codec) {
        case rc::VideoCodec::H264: return MFVideoFormat_H264;
        case rc::VideoCodec::HEVC: return MFVideoFormat_HEVC;
        case rc::VideoCodec::AV1: return RC_MF_AV1;
    }
    return MFVideoFormat_HEVC;
}

LUID DeviceLuid(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)))
        adapter->GetDesc(&desc);
    return desc.AdapterLuid;
}

// Finds a hardware encoder MFT for the codec on the device's adapter.
ComPtr<IMFTransform> FindEncoder(ID3D11Device* device, rc::VideoCodec codec, bool tenBit) {
    EnsureMfStarted();
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, tenBit ? MFVideoFormat_P010 : MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out{MFMediaType_Video, Subtype(codec)};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out,
                         &activates, &count)))
        return nullptr;
    const LUID want = DeviceLuid(device);
    ComPtr<IMFTransform> result;
    for (UINT32 i = 0; i < count && !result; i++) {
        UINT64 luid = 0;
        if (SUCCEEDED(activates[i]->GetUINT64(MFT_ENUM_ADAPTER_LUID, &luid))) {
            LUID l;
            l.LowPart = DWORD(luid & 0xFFFFFFFF);
            l.HighPart = LONG(luid >> 32);
            if (l.LowPart != want.LowPart || l.HighPart != want.HighPart) continue;
        }
        activates[i]->ActivateObject(IID_PPV_ARGS(&result));
    }
    for (UINT32 i = 0; i < count; i++) activates[i]->Release();
    CoTaskMemFree(activates);
    return result;
}

class MfEncoder final : public VideoEncoder {
   public:
    ~MfEncoder() override {
        if (m_transform) {
            m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            m_transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        }
    }

    const char* Name() const override { return "MediaFoundation"; }

    bool Init(ID3D11Device* device, const EncoderConfig& config) override {
        m_config = config;
        m_transform = FindEncoder(device, config.codec, config.tenBit);
        if (!m_transform) {
            RC_LOGE("MF: no hardware %s encoder", CodecName(config.codec));
            return false;
        }
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(m_transform->GetAttributes(&attrs))) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        m_transform.As(&m_events);
        m_transform.As(&m_codecApi);

        UINT token = 0;
        if (FAILED(MFCreateDXGIDeviceManager(&token, &m_deviceManager))) return false;
        m_deviceManager->ResetDevice(device, token);
        if (FAILED(m_transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, ULONG_PTR(m_deviceManager.Get())))) {
            RC_LOGE("MF: encoder rejected D3D manager");
            return false;
        }

        if (m_codecApi) {
            SetCodecValue(CODECAPI_AVLowLatencyMode, VARIANT_TRUE, VT_BOOL);
            SetCodecValue(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, VT_UI4);
            SetCodecValue(CODECAPI_AVEncCommonMeanBitRate, config.bitrateKbps * 1000, VT_UI4);
            // Even frame sizes for low latency, like the other encoders. Not all encoders take it.
            SetCodecValue(CODECAPI_AVEncCommonBufferSize, uint32_t(config.bitrateKbps * 1000.0 / config.fps * (config.desktop ? 2 : 1)), VT_UI4);
            SetCodecValue(CODECAPI_AVEncMPVDefaultBPictureCount, 0, VT_UI4);
            SetCodecValue(CODECAPI_AVEncMPVGOPSize, 0, VT_UI4);
            SetCodecValue(CODECAPI_AVEncCommonQualityVsSpeed, config.desktop ? 50 : 0, VT_UI4);
        }

        ComPtr<IMFMediaType> outType;
        MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, Subtype(config.codec));
        outType->SetUINT32(MF_MT_AVG_BITRATE, config.bitrateKbps * 1000);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
        MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, UINT32(config.fps * 1000), 1000);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        outType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        outType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        outType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        if (config.codec == rc::VideoCodec::H264) outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        if (config.codec == rc::VideoCodec::HEVC)
            outType->SetUINT32(MF_MT_MPEG2_PROFILE, config.tenBit ? eAVEncH265VProfile_Main_420_10 : eAVEncH265VProfile_Main_420_8);
        HRESULT hr = m_transform->SetOutputType(0, outType.Get(), 0);
        if (FAILED(hr)) {
            RC_LOGE("MF: SetOutputType failed %s", HResultString(hr).c_str());
            return false;
        }
        ComPtr<IMFMediaType> inType;
        MFCreateMediaType(&inType);
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, config.tenBit ? MFVideoFormat_P010 : MFVideoFormat_NV12);
        MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
        MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, UINT32(config.fps * 1000), 1000);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        hr = m_transform->SetInputType(0, inType.Get(), 0);
        if (FAILED(hr)) {
            RC_LOGE("MF: SetInputType failed %s", HResultString(hr).c_str());
            return false;
        }
        MFT_OUTPUT_STREAM_INFO info{};
        m_transform->GetOutputStreamInfo(0, &info);
        m_providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        m_outputBufferSize = std::max<DWORD>(info.cbSize, config.width * config.height);

        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        m_frameDuration = int64_t(10'000'000.0 / config.fps);
        RC_LOGI("MediaFoundation %s %ux%u @%.0f fps, %u kbps", CodecName(config.codec), config.width, config.height,
                config.fps, config.bitrateKbps);
        return true;
    }

    bool Encode(ID3D11Texture2D* input, bool forceKeyframe, EncodedFrame& out) override {
        if (forceKeyframe && m_codecApi) SetCodecValue(CODECAPI_AVEncVideoForceKeyFrame, 1, VT_UI4);

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), input, 0, FALSE, &buffer))) return false;
        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(m_timestamp);
        sample->SetSampleDuration(m_frameDuration);
        m_timestamp += m_frameDuration;

        // Wait until the MFT asks for input (draining any output meanwhile).
        while (m_needInput == 0) {
            if (!PumpEvent(out)) return false;
        }
        if (FAILED(m_transform->ProcessInput(0, sample.Get(), 0))) return false;
        m_needInput--;
        out.data.clear();
        m_gotOutput = false;
        // Low-latency encoders emit one output per input.
        for (int i = 0; i < 200 && !m_gotOutput; i++) {
            if (!PumpEvent(out)) return false;
        }
        return m_gotOutput;
    }

    void SetBitrate(uint32_t kbps) override {
        m_config.bitrateKbps = kbps;
        if (m_codecApi) SetCodecValue(CODECAPI_AVEncCommonMeanBitRate, kbps * 1000, VT_UI4);
    }

   private:
    void SetCodecValue(const GUID& api, UINT32 value, VARTYPE type) {
        VARIANT v;
        VariantInit(&v);
        v.vt = type;
        if (type == VT_BOOL)
            v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        else
            v.ulVal = value;
        m_codecApi->SetValue(&api, &v);
    }

    bool PumpEvent(EncodedFrame& out) {
        ComPtr<IMFMediaEvent> event;
        if (FAILED(m_events->GetEvent(0, &event))) return false;
        MediaEventType type;
        event->GetType(&type);
        if (type == METransformNeedInput) {
            m_needInput++;
        } else if (type == METransformHaveOutput) {
            return DrainOutput(out);
        }
        return true;
    }

    bool DrainOutput(EncodedFrame& out) {
        MFT_OUTPUT_DATA_BUFFER output{};
        ComPtr<IMFSample> provided;
        if (!m_providesSamples) {
            ComPtr<IMFMediaBuffer> buf;
            MFCreateMemoryBuffer(m_outputBufferSize, &buf);
            MFCreateSample(&provided);
            provided->AddBuffer(buf.Get());
            output.pSample = provided.Get();
        }
        DWORD status = 0;
        HRESULT hr = m_transform->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> type;
            if (SUCCEEDED(m_transform->GetOutputAvailableType(0, 0, &type))) m_transform->SetOutputType(0, type.Get(), 0);
            return true;
        }
        if (FAILED(hr)) return hr == MF_E_TRANSFORM_NEED_MORE_INPUT;
        IMFSample* s = output.pSample;
        ComPtr<IMFMediaBuffer> contiguous;
        if (s && SUCCEEDED(s->ConvertToContiguousBuffer(&contiguous))) {
            BYTE* data = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(contiguous->Lock(&data, nullptr, &len))) {
                out.data.insert(out.data.end(), data, data + len);
                contiguous->Unlock();
            }
            UINT32 clean = 0;
            s->GetUINT32(MFSampleExtension_CleanPoint, &clean);
            out.keyframe = clean != 0;
            m_gotOutput = true;
        }
        if (m_providesSamples && s) s->Release();
        return true;
    }

    ComPtr<IMFTransform> m_transform;
    ComPtr<IMFMediaEventGenerator> m_events;
    ComPtr<ICodecAPI> m_codecApi;
    ComPtr<IMFDXGIDeviceManager> m_deviceManager;
    bool m_providesSamples = true;
    DWORD m_outputBufferSize = 0;
    int m_needInput = 0;
    bool m_gotOutput = false;
    int64_t m_timestamp = 0;
    int64_t m_frameDuration = 111111;
};

}  // namespace

bool MfProbe(ID3D11Device* device, EncoderCaps& caps) {
    caps.backend = "mf";
    caps.h264 = FindEncoder(device, rc::VideoCodec::H264, false) != nullptr;
    caps.hevc = FindEncoder(device, rc::VideoCodec::HEVC, false) != nullptr;
    caps.av1 = FindEncoder(device, rc::VideoCodec::AV1, false) != nullptr;
    caps.hevc10 = caps.hevc && FindEncoder(device, rc::VideoCodec::HEVC, true) != nullptr;
    caps.av110 = caps.av1 && FindEncoder(device, rc::VideoCodec::AV1, true) != nullptr;
    caps.maxWidth = 4096;
    caps.maxHeight = 4096;
    return caps.h264 || caps.hevc || caps.av1;
}

std::unique_ptr<VideoEncoder> CreateMfEncoder() { return std::make_unique<MfEncoder>(); }

}  // namespace rcs
