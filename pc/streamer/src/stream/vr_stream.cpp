#include "vr_stream.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "foveator.h"
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

// For what can go wrong on every frame: each reason at most once every 5 seconds, with how
// often it happened since it was last logged.
void WarnEvery5s(const char* reason) {
    static std::mutex mutex;
    static std::map<const char*, std::pair<int64_t, uint32_t>> seen;  // last logged, count since
    std::lock_guard lock(mutex);
    auto& [last, count] = seen[reason];
    count++;
    const int64_t now = rc::NowNs();
    if (last && now - last < 5'000'000'000LL) return;
    RC_LOGW("%s (%u times)", reason, count);
    last = now;
    count = 0;
}

}  // namespace

// The full-density square follows the eyes, but only moves once they look clearly elsewhere:
// while it stays put, the encoder can keep referring to the frames before. Without a recent
// gaze, it sits where each eye's lens is sharpest.
void VrStream::PlaceFovea(const float fovTan[2][4], float start[2][2]) {
    rc::GazePayload g;
    int64_t at;
    {
        std::lock_guard lock(m_gazeMutex);
        g = m_gaze;
        at = m_gazeNs;
    }
    const bool fresh = at && rc::NowNs() - at < 250'000'000LL;
    const float F = Foveator::kFoveaSize;
    for (int eye = 0; eye < 2; eye++) {
        const float* t = fovTan[eye];  // left, right, up, down
        // The eye's own gaze, or the other one's while it blinks.
        const int from = (g.flags & (1 << eye)) ? eye : (g.flags & (1 << (1 - eye))) ? 1 - eye : -1;
        float u = t[0] / (t[0] + t[1]), v = t[2] / (t[2] + t[3]);  // straight ahead
        if (fresh && from >= 0 && g.gaze[from][2] < -0.2f) {
            const float* d = g.gaze[from];
            u = (d[0] / -d[2] + t[0]) / (t[0] + t[1]);
            v = (t[2] - d[1] / -d[2]) / (t[2] + t[3]);
        }
        const float x = std::clamp(u - F / 2, 0.f, 1.f - F), y = std::clamp(v - F / 2, 0.f, 1.f - F);
        float* s = m_foveaStart[eye];
        if (!m_foveaPlaced || std::fabs(x - s[0]) > 0.04f || std::fabs(y - s[1]) > 0.04f) {
            s[0] = x;
            s[1] = y;
        }
        start[eye][0] = s[0];
        start[eye][1] = s[1];
    }
    m_foveaPlaced = true;
}

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
    Foveator foveator;
    const bool foveated = m_config.foveatedEyeWidth && m_config.foveatedEyeHeight;
    std::unique_ptr<VideoEncoder> encoder;
    uint32_t srcWidth = 0, srcHeight = 0;  // the frame from the game
    uint32_t encWidth = 0, encHeight = 0;  // what's encoded: smaller when foveated
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
    bool firstFrameLogged = false;
    int64_t lastWaitReportNs = rc::NowNs();
    while (m_running) {
        WaitForSingleObject(m_bridge->FrameEvent(), 50);
        const int64_t now = rc::NowNs();
        // No picture: say every 10 s what the bridge looks like, so a log shows whether the
        // driver never sent a frame or frames arrive and get dropped here.
        if (!streaming && now - lastWaitReportNs > 10'000'000'000LL) {
            lastWaitReportNs = now;
            RC_LOGW("VR: no frame sent for 10 s. Producer %s (pid %ld), latest slot %lld, slots %ld/%ld/%ld, "
                    "heartbeat %.1f s ago, encoder %s",
                    ProducerName(m_bridge->ActiveProducer()), long(shared->producerPid), (long long)shared->latestReadySlot,
                    long(shared->slots[0].state), long(shared->slots[1].state), long(shared->slots[2].state),
                    shared->producerHeartbeatNs ? double(rcvr::QpcNowNs() - shared->producerHeartbeatNs) / 1e9 : -1.0,
                    encoder ? "ready" : "not created");
        }

        const rcvr::ProducerKind producer = m_bridge->ActiveProducer();
        const std::string app = m_bridge->ApplicationName();
        const bool fresh = now - lastFrameNs < 1'000'000'000LL;
        if (streaming && !fresh) {
            streaming = false;
            m_streaming = false;
            lastWaitReportNs = now;
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
            WarnEvery5s("VR frame dropped: the frame's texture isn't available");
            InterlockedExchange(&fs.state, rcvr::kSlotFree);
            continue;
        }
        D3D11_TEXTURE2D_DESC desc;
        tex->GetDesc(&desc);
        if (!firstFrameLogged) {
            firstFrameLogged = true;
            RC_LOGI("VR: first frame from %s (%ux%u, format %u)", ProducerName(producer), desc.Width, desc.Height,
                    unsigned(desc.Format));
        }
        if (!encoder || srcWidth != (desc.Width & ~1u) || srcHeight != (desc.Height & ~1u)) {
            srcWidth = desc.Width & ~1u;
            srcHeight = desc.Height & ~1u;
            encWidth = foveated ? m_config.foveatedEyeWidth * 2 : srcWidth;
            encHeight = foveated ? m_config.foveatedEyeHeight : srcHeight;
            if (foveated && !foveator.Init(device, srcWidth / 2, srcHeight, encWidth / 2, encHeight)) {
                WarnEvery5s("VR frame dropped: foveated encoding didn't start");
                encoder.reset();
                InterlockedExchange(&fs.state, rcvr::kSlotFree);
                continue;
            }
            if (!converter.Init(device, encWidth, encHeight, m_config.tenBit)) {
                WarnEvery5s("VR frame dropped: the colour converter didn't start");
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
            // The lens-based bit budget assumes a picture that isn't squeezed.
            ec.foveated = m_config.foveated && !foveated;
            ec.adaptiveQuant = m_config.adaptiveQuant;
            ec.twoPass = m_config.twoPass;
            motionViews.clear();
            motionReady = m_config.motionField && motion.Init(device, srcWidth, srcHeight);
            if (m_config.motionField && !motionReady) RC_LOGW("Frame generation: the motion estimator didn't start");
            encoder = CreateEncoder(device, m_bridge->VendorId(), ec);
            appliedKbps = ec.bitrateKbps;
            m_sender.RequestKeyframe();
            if (!encoder) {
                WarnEvery5s("VR frame dropped: the encoder didn't start");
                InterlockedExchange(&fs.state, rcvr::kSlotFree);
                rc::SleepMs(500);
                continue;
            }
        }
        ComPtr<IDXGIKeyedMutex> mutex;
        tex->QueryInterface(IID_PPV_ARGS(&mutex));
        if (mutex && !rcvr::AcquireKeyed(mutex.Get(), 100)) {
            WarnEvery5s("VR frame dropped: the driver kept the frame locked for over 100 ms");
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
        float foveaStart[2][2] = {};
        if (foveated) PlaceFovea(fs.fovTan, foveaStart);
        const bool converted = foveated ? foveator.Squeeze(ctx.Get(), tex, foveaStart) && converter.Convert(ctx.Get(), foveator.Output())
                                        : converter.Convert(ctx.Get(), tex);
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
        if (!converted) {
            WarnEvery5s("VR frame dropped: colour conversion failed");
            continue;
        }

        // The encoder budgets bitrate / fps per frame, so half the frames need twice the rate.
        const uint32_t encoderKbps = halfRate ? target * 2 : target;
        if (encoderKbps != appliedKbps) {
            encoder->SetBitrate(encoderKbps);
            appliedKbps = encoderKbps;
        }
        if (m_config.foveated && !foveated) encoder->SetFoveation(slotCopy.fovTan);
        const bool keyframe = m_sender.BeginFrame(*encoder);
        const int64_t encStart = rc::NowNs();
        if (!encoder->Encode(converter.Output(), keyframe, out)) {
            WarnEvery5s("VR frame dropped: encoding failed; restarting the encoder");
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
        meta.eyeWidth = srcWidth / 2;
        meta.eyeHeight = srcHeight;
        meta.pcSubmitNs = slotCopy.submitPcNs;
        meta.pcEncodeStartNs = encStart;
        meta.pcEncodeEndNs = encEnd;
        frameMeta.assign(reinterpret_cast<const uint8_t*>(&meta), reinterpret_cast<const uint8_t*>(&meta) + sizeof(meta));
        if (foveated) {
            rc::VrFoveationMetadata fm{};
            std::memcpy(fm.foveaStart, foveaStart, sizeof(fm.foveaStart));
            frameMeta.insert(frameMeta.end(), reinterpret_cast<const uint8_t*>(&fm), reinterpret_cast<const uint8_t*>(&fm) + sizeof(fm));
        }
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
            RC_LOGI("VR: frames are reaching the headset");
        }
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace rcs
