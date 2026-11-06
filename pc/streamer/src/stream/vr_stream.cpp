#include "vr_stream.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "motion_estimator.h"
#include "rc/motion_field.h"

#include <avrt.h>

#include "../vr/vr_bridge_host.h"
#include "rc/platform.h"

namespace rcs {

namespace {

// Rotates a direction seen from `now` into what it was seen as from `before`.
void HeadTurn(const rcvr::BridgePose& before, const rcvr::BridgePose& now, float m[9]) {
    const float* a = before.orientation;  // x, y, z, w; conjugated below
    const float* b = now.orientation;
    const float ax = -a[0], ay = -a[1], az = -a[2], aw = a[3];
    const float x = aw * b[0] + ax * b[3] + ay * b[2] - az * b[1];
    const float y = aw * b[1] - ax * b[2] + ay * b[3] + az * b[0];
    const float z = aw * b[2] + ax * b[1] - ay * b[0] + az * b[3];
    const float w = aw * b[3] - ax * b[0] - ay * b[1] - az * b[2];
    m[0] = 1 - 2 * (y * y + z * z);
    m[1] = 2 * (x * y - z * w);
    m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);
    m[4] = 1 - 2 * (x * x + z * z);
    m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);
    m[7] = 2 * (y * z + x * w);
    m[8] = 1 - 2 * (x * x + y * y);
}

void AppendMotion(const MotionEstimator& motion, const std::vector<float>& field, uint64_t previousTimestampNs,
                  std::vector<int8_t>& grid, std::vector<uint8_t>& out) {
    grid.resize(field.size());
    for (size_t i = 0; i < field.size(); i++)
        grid[i] = int8_t(std::clamp<long>(std::lround(field[i] / rc::kMotionUnitPixels), -127, 127));
    rc::MotionHeader h{};
    h.gridWidth = uint16_t(motion.GridWidth());
    h.gridHeight = uint16_t(motion.GridHeight());
    h.blockSize = uint16_t(MotionEstimator::kBlockSize);
    h.previousTimestampNs = previousTimestampNs;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&h);
    out.insert(out.end(), bytes, bytes + sizeof(h));
    rc::EncodeMotionRuns(grid.data(), field.size() / 2, out);
}

}  // namespace

VrStream::VrStream(rc::Connection* connection, VrBridgeHost* bridge, const VrStreamConfig& config,
                   StateHandler onState)
    : m_connection(connection),
      m_bridge(bridge),
      m_config(config),
      m_sender(connection, rc::kVrStreamId, config.codec, config.bitrateKbps, config.fecPercent, config.adaptive),
      m_onState(std::move(onState)) {
    if (config.startBitrateKbps) m_sender.StartAt(config.startBitrateKbps);
}

VrStream::~VrStream() { Stop(); }

bool VrStream::Start() {
    m_running = true;
    m_thread = std::thread([this] { Run(); });
    return true;
}

void VrStream::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

static const char* ProducerName(rcvr::ProducerKind k) {
    switch (k) {
        case rcvr::ProducerKind::SteamVR: return "steamvr";
        case rcvr::ProducerKind::OpenXR: return "openxr";
        default: return "";
    }
}

void VrStream::Run() {
    ScopedCoInit com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);  // highest MMCSS class
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);

    ID3D11Device* device = m_bridge->Device();
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    rcvr::Shared* shared = m_bridge->Shared();

    ColorConverter converter;
    std::unique_ptr<VideoEncoder> encoder;
    uint32_t encWidth = 0, encHeight = 0;
    uint32_t appliedKbps = 0;
    constexpr double kHalfRateBelowBpp = 0.03, kFullRateAboveBpp = 0.05;
    bool halfRate = false, skipNext = false;
    m_sender.SetPacing(m_config.pace);
    uint64_t lastFrameIndex = 0;
    int64_t lastFrameNs = 0;
    bool streaming = false;
    rcvr::ProducerKind lastProducer = rcvr::ProducerKind::None;
    std::string lastApp;
    EncodedFrame out;
    MotionEstimator motion;
    bool motionReady = false;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> motionViews;
    rcvr::BridgePose motionPose{};
    uint64_t motionPreviousTimestamp = 0;
    std::vector<float> motionField;
    std::vector<int8_t> motionGrid;
    std::vector<uint8_t> frameMeta;

    m_onState("waiting", "", "");
    while (m_running) {
        WaitForSingleObject(m_bridge->FrameEvent(), 50);
        const int64_t now = rc::NowNs();

        const rcvr::ProducerKind producer = m_bridge->ActiveProducer();
        const std::string app = m_bridge->ApplicationName();
        const bool fresh = now - lastFrameNs < 1'000'000'000LL;
        if (streaming && !fresh) {
            streaming = false;
            m_streaming = false;
            m_onState("waiting", ProducerName(producer), app);
        }
        if (producer != lastProducer || app != lastApp) {
            lastProducer = producer;
            lastApp = app;
            m_onState(streaming ? "running" : "waiting", ProducerName(producer), app);
        }

        const LONG64 latest = shared->latestReadySlot;
        if (latest < 0) continue;
        const int slot = int(latest & 0xFF);
        if (slot >= rcvr::kFrameSlots) continue;
        rcvr::FrameSlot& fs = shared->slots[slot];
        if (fs.frameIndex <= lastFrameIndex && lastFrameIndex != 0) continue;
        if (InterlockedCompareExchange(&fs.state, rcvr::kSlotReading, rcvr::kSlotReady) != rcvr::kSlotReady) continue;

        if (m_paused) {
            lastFrameIndex = fs.frameIndex;
            lastFrameNs = now;
            InterlockedExchange(&fs.state, rcvr::kSlotFree);
            motion.Reset();
            if (!streaming) {
                streaming = true;
                m_streaming = true;
                m_onState("running", ProducerName(producer), app);
            }
            continue;
        }

        ComPtr<ID3D11Texture2D> texRef = m_bridge->SlotTexture(slot);
        ID3D11Texture2D* tex = texRef.Get();
        if (!tex) {
            InterlockedExchange(&fs.state, rcvr::kSlotFree);
            continue;
        }
        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        if (!encoder || encWidth != desc.Width || encHeight != desc.Height) {
            encWidth = desc.Width & ~1u;
            encHeight = desc.Height & ~1u;
            if (!converter.Init(device, encWidth, encHeight, m_config.tenBit)) {
                InterlockedExchange(&fs.state, rcvr::kSlotFree);
                continue;
            }
            EncoderConfig ec;
            ec.codec = m_config.codec;
            ec.width = encWidth;
            ec.height = encHeight;
            ec.fps = m_config.fps;
            ec.bitrateKbps = m_sender.TargetBitrateKbps();
            ec.tenBit = m_config.tenBit;
            ec.inputFormat = converter.Format();
            ec.foveated = m_config.foveated;
            ec.adaptiveQuant = m_config.adaptiveQuant;
            ec.twoPass = m_config.twoPass;
            motionViews.clear();
            motionReady = m_config.motionField && motion.Init(device, encWidth, encHeight);
            if (m_config.motionField && !motionReady) RC_LOGW("Frame generation: the motion estimator didn't start");
            encoder = CreateEncoder(device, m_bridge->VendorId(), ec);
            appliedKbps = ec.bitrateKbps;
            m_sender.RequestKeyframe();
            if (!encoder) {
                InterlockedExchange(&fs.state, rcvr::kSlotFree);
                rc::SleepMs(500);
                continue;
            }
        }
        ComPtr<IDXGIKeyedMutex> mutex;
        tex->QueryInterface(IID_PPV_ARGS(&mutex));
        if (mutex && !rcvr::AcquireKeyed(mutex.Get(), 100)) {
            InterlockedExchange(&fs.state, rcvr::kSlotFree);
            continue;
        }
        // On a narrow path a clean image at half the frames beats a smeared one at all of
        // them: the headset keeps reprojecting the last frame to the current head pose.
        const uint32_t target = m_sender.TargetBitrateKbps();
        const double bitsPerPixel = target * 1000.0 / (double(encWidth) * encHeight * m_config.fps);
        if (bitsPerPixel < kHalfRateBelowBpp) halfRate = true;
        else if (bitsPerPixel > kFullRateAboveBpp) halfRate = false;
        skipNext = halfRate && !skipNext;
        if (skipNext) {
            if (mutex) mutex->ReleaseSync(0);
            lastFrameIndex = fs.frameIndex;
            InterlockedExchange(&fs.state, rcvr::kSlotFree);
            continue;
        }
        const bool converted = converter.Convert(ctx.Get(), tex);
        bool motionValid = false;
        if (converted && motionReady) {
            ComPtr<ID3D11ShaderResourceView>& view = motionViews[tex];
            if (!view) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS   ? DXGI_FORMAT_R8G8B8A8_UNORM
                            : desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM
                                                                           : desc.Format;
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(tex, &sd, &view);
            }
            float turn[9];
            HeadTurn(motionPose, fs.renderPose, turn);
            if (view) motionValid = motion.Process(ctx.Get(), view.Get(), turn, fs.fovTan);
            motionPose = fs.renderPose;
        }
        if (mutex) mutex->ReleaseSync(0);

        const rcvr::FrameSlot slotCopy = fs;
        InterlockedExchange(&fs.state, rcvr::kSlotFree);
        if (!converted) continue;

        // The encoder budgets bitrate / fps per frame, so half the frames need twice the rate.
        const uint32_t encoderKbps = halfRate ? target * 2 : target;
        if (encoderKbps != appliedKbps) {
            encoder->SetBitrate(encoderKbps);
            appliedKbps = encoderKbps;
        }
        if (m_config.foveated) encoder->SetFoveation(slotCopy.fovTan);
        const bool keyframe = m_sender.BeginFrame(*encoder);
        const int64_t encStart = rc::NowNs();
        if (!encoder->Encode(converter.Output(), keyframe, out)) {
            encoder.reset();
            continue;
        }
        const int64_t encEnd = rc::NowNs();

        rc::VrFrameMetadata meta{};
        meta.trackingTimestampNs = slotCopy.headsetTargetNs;
        std::memcpy(meta.renderPose.orientation, slotCopy.renderPose.orientation, sizeof(float) * 4);
        std::memcpy(meta.renderPose.position, slotCopy.renderPose.position, sizeof(float) * 3);
        std::memcpy(meta.fovTan, slotCopy.fovTan, sizeof(meta.fovTan));
        meta.ipd = shared->config.ipd;
        meta.eyeWidth = encWidth / 2;
        meta.eyeHeight = encHeight;
        meta.pcSubmitNs = slotCopy.submitPcNs;
        meta.pcEncodeStartNs = encStart;
        meta.pcEncodeEndNs = encEnd;
        frameMeta.assign(reinterpret_cast<const uint8_t*>(&meta), reinterpret_cast<const uint8_t*>(&meta) + sizeof(meta));
        // The encoder has waited for the GPU work before it by now, so this doesn't stall.
        if (motionValid && motion.Read(ctx.Get(), motionField)) AppendMotion(motion, motionField, motionPreviousTimestamp, motionGrid, frameMeta);
        motionPreviousTimestamp = slotCopy.headsetTargetNs;
        m_sender.Send(out, frameMeta.data(), uint16_t(frameMeta.size()), rc::kVideoHasPose | (m_config.tenBit ? rc::kVideoHdr10 : 0),
                      rc::NsToMs(encEnd - encStart));

        lastFrameIndex = slotCopy.frameIndex;
        lastFrameNs = rc::NowNs();
        if (!streaming) {
            streaming = true;
            m_streaming = true;
            m_onState("running", ProducerName(m_bridge->ActiveProducer()), m_bridge->ApplicationName());
        }
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace rcs
