#include "desktop_stream.h"

#include <avrt.h>

#include "../gpu.h"
#include "../input/input_injector.h"
#include "rc/platform.h"

#pragma comment(lib, "avrt.lib")

namespace rcs {

DesktopStream::DesktopStream(rc::Connection* connection, uint8_t streamId, const MonitorInfo& monitor,
                             const DesktopStreamConfig& config)
    : m_connection(connection),
      m_streamId(streamId),
      m_monitor(monitor),
      m_config(config),
      m_sender(connection, streamId, config.codec, config.bitrateKbps, config.fecPercent, config.adaptive) {
    if (config.startBitrateKbps) m_sender.StartAt(config.startBitrateKbps);
}

DesktopStream::~DesktopStream() { Stop(); }

json DesktopStream::Describe() const {
    return {{"streamId", m_streamId},
            {"monitorId", m_monitor.id},
            {"name", m_monitor.name},
            {"index", m_monitor.index},
            {"width", m_config.width},
            {"height", m_config.height},
            {"fps", m_config.fps},
            {"codec", int(m_config.codec)},
            {"primary", m_monitor.primary}};
}

bool DesktopStream::Start() {
    m_running = true;
    m_thread = std::thread([this] { Run(); });
    return true;
}

void DesktopStream::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void DesktopStream::Run() {
    ScopedCoInit com;
    m_sender.SetPacing(m_config.pace);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);

    auto adapter = AdapterByLuid(m_monitor.adapterLuid);
    const GpuInfo gpu = DescribeAdapter(adapter.Get());
    auto device = CreateDevice(adapter.Get());
    if (!device) {
        m_failed = true;
        return;
    }
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);

    DesktopCapture capture;
    ColorConverter converter;
    std::unique_ptr<VideoEncoder> encoder;
    auto initAll = [&]() -> bool {
        if (!capture.Init(device.Get(), m_monitor, m_config.width, m_config.height)) return false;
        if (!converter.Init(device.Get(), m_config.width, m_config.height, m_config.tenBit)) return false;
        EncoderConfig ec;
        ec.codec = m_config.codec;
        ec.width = m_config.width;
        ec.height = m_config.height;
        ec.fps = m_config.fps;
        ec.bitrateKbps = m_sender.TargetBitrateKbps();
        ec.tenBit = m_config.tenBit;
        ec.desktop = true;
        ec.adaptiveQuant = m_config.adaptiveQuant;
        // Latency matters less than sharp text here, so always.
        ec.twoPass = true;
        ec.inputFormat = converter.Format();
        encoder = CreateEncoder(device.Get(), gpu.vendorId, ec);
        return encoder != nullptr;
    };
    // Capture can fail transiently (secure desktop); keep retrying.
    while (m_running && !initAll()) rc::SleepMs(500);
    input::WakeDisplays();
    int64_t lastWake = rc::NowNs();
    if (!m_running) return;

    const int64_t frameInterval = int64_t(1e9 / m_config.fps);
    const int64_t idleInterval = 1'000'000'000LL;  // heartbeat frame when nothing changes
    // After a change, a few more frames of the same picture spend the bitrate on sharpening
    // it: at low bitrates text would otherwise stay blurry after scrolling.
    const int64_t refineNs = 2'000'000'000LL;
    const int64_t refineInterval = 66'000'000LL;
    int64_t lastChange = 0;
    bool halfRate = false;
    int64_t lastSent = 0;
    uint32_t appliedKbps = m_sender.TargetBitrateKbps();
    EncodedFrame out;
    bool haveFrame = false;

    while (m_running) {
        const int64_t now = rc::NowNs();
        const int64_t interval = (m_active ? frameInterval : frameInterval * 4) * (halfRate ? 2 : 1);
        const int64_t wait = std::max<int64_t>(1'000'000, lastSent + interval - now);
        const auto result = capture.Capture(UINT(wait / 1'000'000));
        if (result == DesktopCapture::Result::NewFrame) {
            haveFrame = true;
            lastChange = rc::NowNs();
        }
        if (!capture.HasDesktop() && rc::NowNs() - lastWake > 3'000'000'000LL) {
            input::WakeDisplays();
            lastWake = rc::NowNs();
        }
        if (result == DesktopCapture::Result::Lost || result == DesktopCapture::Result::Error) {
            m_sender.RequestKeyframe();
            continue;
        }
        const int64_t t = rc::NowNs();
        if (t - lastSent < interval) continue;  // pace to target fps
        const bool wanted = m_sender.WantsFrame();
        const bool stale = t - lastSent > idleInterval;
        const bool refining = t - lastChange < refineNs && t - lastSent >= std::max(interval, refineInterval);
        if (!haveFrame && !wanted && !stale && !refining) continue;
        if (!haveFrame) capture.Recompose();  // repeat last image (keyframe/recovery or heartbeat)
        haveFrame = false;

        const uint32_t target = m_sender.TargetBitrateKbps();
        const double bitsPerPixel = target * 1000.0 / (double(m_config.width) * m_config.height * m_config.fps);
        if (bitsPerPixel < 0.05) halfRate = true;
        else if (bitsPerPixel > 0.08) halfRate = false;
        // The encoder budgets bitrate / fps per frame, so half the frames need twice the rate.
        const uint32_t encoderKbps = halfRate ? target * 2 : target;
        if (encoderKbps != appliedKbps) {
            encoder->SetBitrate(encoderKbps);
            appliedKbps = encoderKbps;
        }
        const int64_t captureNs = t;
        if (!converter.Convert(ctx.Get(), capture.Output())) continue;
        const bool keyframe = m_sender.BeginFrame(*encoder);
        const int64_t encStart = rc::NowNs();
        if (!encoder->Encode(converter.Output(), keyframe, out)) {
            RC_LOGW("Desktop encode failed; re-initializing");
            m_sender.RequestKeyframe();
            initAll();
            continue;
        }
        const int64_t encEnd = rc::NowNs();
        rc::DesktopFrameMetadata meta{};
        meta.width = m_config.width;
        meta.height = m_config.height;
        meta.pcCaptureNs = captureNs;
        meta.pcEncodeEndNs = encEnd;
        const POINT cursor = capture.CursorPosition();
        meta.cursorX = cursor.x;
        meta.cursorY = cursor.y;
        m_sender.Send(out, &meta, sizeof(meta), 0, rc::NsToMs(encEnd - encStart));
        lastSent = t;
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace rcs
