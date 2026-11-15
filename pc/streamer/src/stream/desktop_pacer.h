// When a desktop stream sends a frame, and at what bitrate its encoder runs. The same on every
// OS: only how a picture is captured and encoded is the platform's (desktop_stream.cpp on
// Windows, platform/mac/desktop_stream_mac.mm on macOS).
//
//   DesktopPacer pacer(config.fps);
//   loop:
//     wait for a new picture, at most pacer.WaitNs(now, active)
//     if one came: pacer.OnNewPicture(now)
//     switch (pacer.Decide(now, active, sender.WantsFrame())):
//       Skip: continue;  Repeat: encode the last picture again;  Send: encode the new one
//     encoder.SetBitrate(pacer.EncoderKbps(sender.TargetBitrateKbps(), width, height))  (if changed)
//     encode, send, pacer.OnSent(now)
#pragma once

#include <algorithm>
#include <cstdint>

namespace rcs {

class DesktopPacer {
   public:
    enum class Decision { Skip, Send, Repeat };  // Repeat: send the last picture again

    explicit DesktopPacer(float fps) : m_fps(fps > 0 ? fps : 60.f), m_frameInterval(int64_t(1e9 / m_fps)) {}

    // Between two frames. Monitors not looked at run at a quarter of the rate, and a stream at
    // half rate (too few bits per pixel, see EncoderKbps) at half of that.
    int64_t Interval(bool active) const { return (active ? m_frameInterval : m_frameInterval * 4) * (m_halfRate ? 2 : 1); }

    // How long to wait for a new picture before deciding again.
    int64_t WaitNs(int64_t now, bool active) const { return std::max<int64_t>(1'000'000, m_lastSent + Interval(active) - now); }

    void OnNewPicture(int64_t now) {
        m_havePicture = true;
        m_lastChange = now;
    }

    // senderWants: the sender needs a frame (a keyframe or loss recovery) even if nothing moved.
    Decision Decide(int64_t now, bool active, bool senderWants) const {
        if (now - m_lastSent < Interval(active)) return Decision::Skip;  // pace to the frame rate
        // A heartbeat frame when nothing changes, so the headset knows the stream is alive.
        const bool stale = now - m_lastSent > kIdleNs;
        // After a change, a few more frames of the same picture spend the bitrate on sharpening
        // it: at low bitrates text would otherwise stay blurry after scrolling.
        const bool refining = now - m_lastChange < kRefineNs && now - m_lastSent >= std::max(Interval(active), kRefineIntervalNs);
        if (!m_havePicture && !senderWants && !stale && !refining) return Decision::Skip;
        return m_havePicture ? Decision::Send : Decision::Repeat;
    }

    // The picture Decide chose is being encoded: the next Decide waits for a newer one.
    void TakePicture() { m_havePicture = false; }

    // The encoder budgets bitrate / fps per frame, so at half the frames it needs twice the rate.
    // Half rate starts below 0.05 bits per pixel and ends above 0.08.
    uint32_t EncoderKbps(uint32_t targetKbps, uint32_t width, uint32_t height) {
        const double bitsPerPixel = targetKbps * 1000.0 / (double(width) * height * m_fps);
        if (bitsPerPixel < 0.05) m_halfRate = true;
        else if (bitsPerPixel > 0.08) m_halfRate = false;
        return m_halfRate ? targetKbps * 2 : targetKbps;
    }

    void OnSent(int64_t now) { m_lastSent = now; }

   private:
    static constexpr int64_t kIdleNs = 1'000'000'000LL;
    static constexpr int64_t kRefineNs = 2'000'000'000LL;
    static constexpr int64_t kRefineIntervalNs = 66'000'000LL;

    float m_fps;
    int64_t m_frameInterval;
    int64_t m_lastSent = 0;
    int64_t m_lastChange = 0;
    bool m_havePicture = false;
    bool m_halfRate = false;
};

}  // namespace rcs
