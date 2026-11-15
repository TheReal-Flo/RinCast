// Desktop streaming on Linux: X11 (XShm) or Wayland (screen-cast portal + PipeWire) capture
// (linux_capture.h), encoded with FFmpeg, and the shared DesktopPacer and VideoSender decide when
// frames go and at what bitrate, as on every OS.
#include "stream/desktop_stream.h"

#include "input/input_injector.h"
#include "platform/linux/ffmpeg_encoder.h"
#include "platform/linux/linux_capture.h"
#include "rc/platform.h"
#include "stream/desktop_pacer.h"

namespace rcs {

void DesktopStream::Run() {
    m_sender.SetPacing(m_config.pace);
    // On Wayland the input portal's dialog comes up now, while the user is at the PC anyway.
    input::PrepareInput();
    std::unique_ptr<LinuxCapture> capture = CreateCapture();
    FfmpegEncoder encoder;
    auto initAll = [&]() -> bool {
        capture->Stop();
        return capture->Start(m_monitor, m_config.width, m_config.height, m_config.fps, m_running) &&
               encoder.Init(EncoderSettings());
    };
    // Capture can fail for a while (the portal's dialog not answered yet, the display asleep, a
    // mode change); keep trying, as on Windows.
    bool told = false;
    while (m_running && !initAll()) {
        if (!told) RC_LOGW("Desktop: can't capture %s yet; retrying", m_monitor.name.c_str());
        told = true;
        input::WakeDisplays();
        for (int i = 0; i < 10 && m_running; i++) rc::SleepMs(100);
    }
    if (!m_running) {
        capture->Stop();
        return;
    }
    RC_LOGI("Desktop: streaming %s at %ux%u (%s)", m_monitor.name.c_str(), m_config.width, m_config.height,
            encoder.Backend());

    DesktopPacer pacer(m_config.fps);
    uint32_t appliedKbps = m_sender.TargetBitrateKbps();
    // The picture last taken, repeated when needed. It stays valid until the capture returns a
    // newer one (linux_capture.h).
    CapturedPicture current;
    bool haveCurrent = false;
    EncodedFrame out;
    while (m_running) {
        CapturedPicture picture;
        const int got = capture->Wait(pacer.WaitNs(rc::NowNs(), m_active), picture);
        if (!m_running) break;
        if (got < 0) {
            m_sender.RequestKeyframe();
            haveCurrent = false;
            while (m_running && !initAll()) {
                for (int i = 0; i < 10 && m_running; i++) rc::SleepMs(100);
            }
            continue;
        }
        if (got > 0) {
            current = picture;
            haveCurrent = true;
            pacer.OnNewPicture(rc::NowNs());
        }
        if (!haveCurrent) continue;  // nothing captured yet
        const int64_t t = rc::NowNs();
        if (pacer.Decide(t, m_active, m_sender.WantsFrame()) == DesktopPacer::Decision::Skip) continue;
        pacer.TakePicture();  // Send and Repeat both encode `current`

        const uint32_t encoderKbps = pacer.EncoderKbps(m_sender.TargetBitrateKbps(), m_config.width, m_config.height);
        if (encoderKbps != appliedKbps) {
            encoder.SetBitrate(encoderKbps);
            appliedKbps = encoderKbps;
        }
        const bool keyframe = m_sender.BeginFrame(encoder);
        const int64_t encStart = rc::NowNs();
        const bool encoded = current.isDmaBuf ? encoder.EncodeDmaBuf(current.dmabuf, keyframe, out)
                                              : encoder.EncodeBgra(current.pixels, current.stride, keyframe, out);
        if (!encoded) {
            m_sender.RequestKeyframe();
            if (current.isDmaBuf) {
                // An image the encoder can't import (a layout it doesn't know): pictures in
                // memory from now on, which every encoder takes.
                RC_LOGW("Desktop: encoding a DMA-BUF picture failed; switching to pictures in memory");
                capture->PreferMemory();
                haveCurrent = false;
            } else {
                RC_LOGW("Desktop encode failed; re-initializing");
            }
            encoder.Init(EncoderSettings());
            continue;
        }
        const int64_t encEnd = rc::NowNs();
        rc::DesktopFrameMetadata meta{};
        meta.width = m_config.width;
        meta.height = m_config.height;
        meta.pcCaptureNs = t;
        meta.pcEncodeEndNs = encEnd;
        int cursorX = -1, cursorY = -1;
        capture->Cursor(cursorX, cursorY);
        meta.cursorX = cursorX;
        meta.cursorY = cursorY;
        m_sender.Send(out, &meta, sizeof(meta), 0, rc::NsToMs(encEnd - encStart));
        pacer.OnSent(t);
    }
    capture->Stop();
}

}  // namespace rcs
