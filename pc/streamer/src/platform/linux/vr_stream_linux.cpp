// PC VR on Linux: RinCast's Monado service renders each frame into one of the bridge's frame
// images (vr_images_linux.h); this takes the newest, encodes it with FFmpeg (a DMA-BUF straight
// into VAAPI, or read back for NVENC and software) and sends it with the pose it was rendered
// for, as on Windows. Not yet here: foveated encoding and the motion field for frame
// generation (the session doesn't offer them on Linux).
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "platform/linux/ffmpeg_encoder.h"
#include "platform/linux/vr_images_linux.h"
#include "rc/platform.h"
#include "stream/vr_stream.h"
#include "vr/vr_bridge_host.h"

namespace rcs {

namespace {

// For what can go wrong on every frame: each reason at most once every 5 seconds.
void WarnEvery5s(const char* reason) {
    static std::mutex mutex;
    static std::map<const char*, std::pair<int64_t, uint32_t>> seen;
    std::lock_guard lock(mutex);
    auto& [last, count] = seen[reason];
    count++;
    const int64_t now = rc::NowNs();
    if (last && now - last < 5'000'000'000LL) return;
    RC_LOGW("%s (%u times)", reason, count);
    last = now;
    count = 0;
}

const char* ProducerName(rcvr::ProducerKind k) {
    switch (k) {
        case rcvr::ProducerKind::SteamVR: return "steamvr";
        case rcvr::ProducerKind::OpenXR: return "openxr";
        case rcvr::ProducerKind::Monado: return "monado";
        default: return "";
    }
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
    m_sendThread = std::thread([this] { RunSender(); });
    m_thread = std::thread([this] { Run(); });
    return true;
}

void VrStream::Stop() {
    m_running = false;
    { std::lock_guard lock(m_sendMutex); }
    m_sendCv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    if (m_sendThread.joinable()) m_sendThread.join();
}

void VrStream::Run() {
    rcvr::Shared* shared = m_bridge->Shared();
    VrImages& images = VrImages::Get();
    FfmpegEncoder encoder;
    bool encoderReady = false;
    uint32_t encWidth = 0, encHeight = 0, appliedKbps = 0;
    constexpr double kHalfRateBelowBpp = 0.03, kFullRateAboveBpp = 0.05;
    bool halfRate = false, skipNext = false;
    m_sender.SetPacing(m_config.pace);
    uint64_t lastFrameIndex = 0;
    LONG lastProducerPid = 0;
    int64_t lastFrameNs = 0;
    bool streaming = false, firstFrameLogged = false;
    rcvr::ProducerKind lastProducer = rcvr::ProducerKind::None;
    std::string lastApp;
    LONG signalSeen = shared->frameSignal;
    int64_t lastWaitReportNs = rc::NowNs();

    m_onState("waiting", "", "");
    while (m_running) {
        signalSeen = rcvr::WaitSignal(&shared->frameSignal, signalSeen, 50);
        const int64_t now = rc::NowNs();
        if (!streaming && now - lastWaitReportNs > 10'000'000'000LL) {
            lastWaitReportNs = now;
            RC_LOGW("VR: no frame sent for 10 s. Producer %s (pid %ld), latest slot %lld, heartbeat %.1f s ago",
                    ProducerName(m_bridge->ActiveProducer()), long(shared->producerPid), (long long)shared->latestReadySlot,
                    shared->producerHeartbeatNs ? double(rcvr::QpcNowNs() - shared->producerHeartbeatNs) / 1e9 : -1.0);
        }

        const rcvr::ProducerKind producer = m_bridge->ActiveProducer();
        const LONG producerPid = shared->producerPid;
        if (producerPid != lastProducerPid) {
            // Each producer counts its frames from zero.
            lastProducerPid = producerPid;
            lastFrameIndex = 0;
            m_sender.RequestKeyframe();
        }
        const std::string app = m_bridge->ApplicationName();
        if (streaming && now - lastFrameNs > 1'000'000'000LL) {
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
        const rcvr::FrameSlot slotCopy = fs;
        lastFrameIndex = slotCopy.frameIndex;
        lastFrameNs = now;
        auto release = [&] { InterlockedExchange(&fs.state, rcvr::kSlotFree); };
        if (!streaming) {
            streaming = true;
            m_streaming = true;
            m_onState("running", ProducerName(producer), app);
        }
        if (m_paused) {
            release();
            continue;
        }

        const uint32_t width = images.Width() & ~1u, height = images.Height() & ~1u;
        if (!firstFrameLogged) {
            firstFrameLogged = true;
            RC_LOGI("VR: first frame from %s (%ux%u)", ProducerName(producer), width, height);
        }
        // On a narrow path a clean image at half the frames beats a smeared one at all of them.
        const uint32_t target = m_sender.TargetBitrateKbps();
        const double bitsPerPixel = target * 1000.0 / (double(width) * height * m_config.fps);
        if (bitsPerPixel < kHalfRateBelowBpp) halfRate = true;
        else if (bitsPerPixel > kFullRateAboveBpp) halfRate = false;
        skipNext = halfRate && !skipNext;
        if (skipNext) {
            release();
            continue;
        }

        if (!encoderReady || encWidth != width || encHeight != height) {
            EncoderConfig ec;
            ec.codec = m_config.codec;
            ec.width = encWidth = width;
            ec.height = encHeight = height;
            ec.fps = m_config.fps;
            ec.bitrateKbps = appliedKbps = target;
            ec.tenBit = m_config.tenBit;
            ec.adaptiveQuant = m_config.adaptiveQuant;
            ec.twoPass = m_config.twoPass;
            encoderReady = encoder.Init(ec);
            m_sender.RequestKeyframe();
            if (!encoderReady) {
                WarnEvery5s("VR frame dropped: the encoder didn't start");
                release();
                rc::SleepMs(500);
                continue;
            }
            RC_LOGI("VR: encoding %ux%u with %s", width, height, encoder.Name());
        }
        const uint32_t encoderKbps = halfRate ? target * 2 : target;
        if (encoderKbps != appliedKbps) {
            encoder.SetBitrate(encoderKbps);
            appliedKbps = encoderKbps;
        }

        const bool keyframe = m_sender.BeginFrame(encoder);
        const int64_t encStart = rc::NowNs();
        EncodedFrame out;
        bool ok = false;
        DmaBufFrame dmabuf;
        if (encoder.TakesDmaBuf() && images.DmaBuf(slot, dmabuf)) {
            ok = encoder.EncodeDmaBuf(dmabuf, keyframe, out);
        } else {
            int stride = 0;
            if (const uint8_t* pixels = images.ReadBack(slot, stride)) ok = encoder.EncodeRgba(pixels, stride, keyframe, out);
        }
        release();
        const int64_t encEnd = rc::NowNs();
        if (!ok) {
            WarnEvery5s("VR frame dropped: encoding failed; restarting the encoder");
            encoderReady = false;
            continue;
        }
        const bool recovery = m_sender.TakeRecovery();

        rc::VrFrameMetadata meta{};
        meta.trackingTimestampNs = slotCopy.headsetTargetNs;
        std::memcpy(meta.renderPose.orientation, slotCopy.renderPose.orientation, sizeof(meta.renderPose.orientation));
        std::memcpy(meta.renderPose.position, slotCopy.renderPose.position, sizeof(meta.renderPose.position));
        std::memcpy(meta.fovTan, slotCopy.fovTan, sizeof(meta.fovTan));
        meta.ipd = shared->config.ipd;
        meta.eyeWidth = width / 2;
        meta.eyeHeight = height;
        meta.pcSubmitNs = slotCopy.submitPcNs;
        meta.pcEncodeStartNs = encStart;
        meta.pcEncodeEndNs = encEnd;
        SendJob send;
        send.frame = std::move(out);
        send.meta.assign(reinterpret_cast<const uint8_t*>(&meta), reinterpret_cast<const uint8_t*>(&meta) + sizeof(meta));
        send.flags = rc::kVideoHasPose | (m_config.tenBit ? rc::kVideoHdr10 : 0);
        send.encodeMs = rc::NsToMs(encEnd - encStart);
        send.recovery = recovery;
        {
            // The one before still going out (a paced send): wait, a dropped frame would break the
            // ones referencing it.
            std::unique_lock lock(m_sendMutex);
            m_sendCv.wait(lock, [this] { return !m_running || !m_send; });
            if (!m_running) break;
            m_send = std::move(send);
        }
        m_sendCv.notify_all();
    }
}

void VrStream::RunSender() {
    bool firstSent = false;
    while (m_running) {
        SendJob send;
        {
            std::unique_lock lock(m_sendMutex);
            m_sendCv.wait(lock, [this] { return !m_running || m_send.has_value(); });
            if (!m_running) break;
            send = std::move(*m_send);
            m_send.reset();
        }
        m_sendCv.notify_all();
        m_sender.Send(send.frame, send.meta.data(), uint16_t(send.meta.size()), send.flags, send.encodeMs, send.recovery);
        if (!firstSent) {
            firstSent = true;
            RC_LOGI("VR: frames are reaching the headset");
        }
    }
}

}  // namespace rcs
