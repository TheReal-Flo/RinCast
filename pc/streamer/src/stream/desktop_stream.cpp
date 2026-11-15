#include "desktop_stream.h"
#include "desktop_pacer.h"

#include <avrt.h>

#include "../gpu.h"
#include "../input/input_injector.h"
#include "rc/platform.h"

#pragma comment(lib, "avrt.lib")

namespace rcs {

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
        EncoderConfig ec = EncoderSettings();
        ec.inputFormat = converter.Format();
        encoder = CreateEncoder(device.Get(), gpu.vendorId, ec);
        return encoder != nullptr;
    };
    // Capture can fail transiently (secure desktop); keep retrying.
    while (m_running && !initAll()) rc::SleepMs(500);
    input::WakeDisplays();
    int64_t lastWake = rc::NowNs();
    if (!m_running) return;

    DesktopPacer pacer(m_config.fps);
    uint32_t appliedKbps = m_sender.TargetBitrateKbps();
    EncodedFrame out;

    while (m_running) {
        const int64_t wait = pacer.WaitNs(rc::NowNs(), m_active);
        const auto result = capture.Capture(UINT(wait / 1'000'000));
        if (result == DesktopCapture::Result::NewFrame) pacer.OnNewPicture(rc::NowNs());
        if (!capture.HasDesktop() && rc::NowNs() - lastWake > 3'000'000'000LL) {
            input::WakeDisplays();
            lastWake = rc::NowNs();
        }
        if (result == DesktopCapture::Result::Lost || result == DesktopCapture::Result::Error) {
            m_sender.RequestKeyframe();
            continue;
        }
        const int64_t t = rc::NowNs();
        const auto decision = pacer.Decide(t, m_active, m_sender.WantsFrame());
        if (decision == DesktopPacer::Decision::Skip) continue;
        if (decision == DesktopPacer::Decision::Repeat) capture.Recompose();  // keyframe/recovery or heartbeat
        pacer.TakePicture();

        const uint32_t encoderKbps = pacer.EncoderKbps(m_sender.TargetBitrateKbps(), m_config.width, m_config.height);
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
        pacer.OnSent(t);
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace rcs
