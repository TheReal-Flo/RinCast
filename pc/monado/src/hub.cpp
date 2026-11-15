// The service's link to the streamer (rincast_monado.h).
#include <cstdio>
#include <cstring>

#include "rincast_monado.h"

#ifdef _WIN32
#define RCM_PID() unsigned(GetCurrentProcessId())
#else
#include <unistd.h>
#define RCM_PID() unsigned(getpid())
#endif

#include "util/u_logging.h"

namespace rcm {

Hub& Hub::Get() {
    static Hub hub;
    return hub;
}

bool Hub::Open(int timeoutMs) {
    const int64_t end = rcvr::QpcNowNs() + int64_t(timeoutMs) * 1'000'000;
    while (!m_bridge.Open()) {
        if (rcvr::QpcNowNs() > end) return false;
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100'000);
#endif
    }
    if (!rcvr::ReadDisplayConfig(m_bridge.shared(), m_display) || !m_display.eyeWidth || !m_display.eyeHeight) {
        // No headset has connected to this streamer yet: a Quest 3's size until one does (the
        // streamer restarts the service when the real one differs).
        m_display = {};
        m_display.eyeWidth = 2064;
        m_display.eyeHeight = 2208;
        m_display.refreshRate = 90;
        m_display.ipd = 0.063f;
        const float t[4] = {1.0f, 0.84f, 0.95f, 1.1f};  // left, right, up, down
        std::memcpy(m_display.fovTan[0], t, sizeof(t));
        const float r[4] = {0.84f, 1.0f, 0.95f, 1.1f};
        std::memcpy(m_display.fovTan[1], r, sizeof(r));
        U_LOG_W("RinCast: no headset display known yet, starting with a default one");
    }
    m_ownEpochNs = rcvr::QpcNowNs();
    U_LOG_I("RinCast: connected to the streamer, %ux%u per eye at %.0f Hz", m_display.eyeWidth, m_display.eyeHeight,
            double(RefreshRate()));
    return true;
}

bool Hub::Tracking(rcvr::TrackingSnapshot& out) const {
    const rcvr::Shared* s = m_bridge.shared();
    return s && s->headsetConnected && rcvr::ReadTracking(s, out);
}

void Hub::SetRenderedWith(const rcvr::TrackingSnapshot& t) {
    std::lock_guard lock(m_renderedMutex);
    m_rendered = t;
}

rcvr::TrackingSnapshot Hub::RenderedWith() const {
    std::lock_guard lock(m_renderedMutex);
    return m_rendered;
}

void Hub::StartHeartbeat() {
    if (m_beating.exchange(true)) return;
    m_heartbeat = std::thread([this] {
        while (m_beating) {
            if (rcvr::Shared* s = m_bridge.shared()) {
                s->producerKind = LONG(rcvr::ProducerKind::Monado);
                s->producerPid = LONG(RCM_PID());
                InterlockedExchange64(&s->producerHeartbeatNs, rcvr::QpcNowNs());
                if (!s->applicationName[0]) std::snprintf(s->applicationName, sizeof(s->applicationName), "Monado");
            }
#ifdef _WIN32
            Sleep(100);
#else
            usleep(100'000);
#endif
        }
    });
}

void Hub::StopHeartbeat() {
    m_beating = false;
    if (m_heartbeat.joinable()) m_heartbeat.join();
    if (rcvr::Shared* s = m_bridge.shared()) {
        if (s->producerPid == LONG(RCM_PID())) {
            s->producerKind = LONG(rcvr::ProducerKind::None);
            s->producerPid = 0;
        }
    }
}

int64_t Hub::NextVsync(int64_t now, int64_t last) const {
    const int64_t period = PeriodNs();
    const rcvr::Shared* s = m_bridge.shared();
    // The streamer moves the vsync so frames reach the headset just before it shows them; without
    // a streamer the service keeps time on its own.
    const int64_t epoch = s && s->vsyncEpochNs && m_bridge.StreamerAlive() ? s->vsyncEpochNs : m_ownEpochNs;
    int64_t next = epoch + ((now - epoch) / period + 1) * period;
    // Moved later, the vsync just passed comes up again: at least half a period after the last.
    if (last && next - last < period / 2) next += period;
    return next;
}

}  // namespace rcm
