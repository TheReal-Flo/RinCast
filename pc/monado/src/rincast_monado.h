// RinCast's Monado service: Monado's OpenXR service with two parts of RinCast's own, the devices
// (the headset and its controllers, from the streamer's tracking) and the compositor target (the
// frames, into the streamer's frame images). Both talk to the streamer through the VR bridge
// (pc/common/vr_bridge.h), as the SteamVR driver does.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

#include "vr_bridge.h"

#include "xrt/xrt_defines.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_tracking.h"

struct comp_target_factory;
struct xrt_session_event_sink;
struct xrt_space_overseer;
struct xrt_system_devices;

namespace rcm {

// The service's link to the streamer: the bridge's shared memory, opened once at start (the
// streamer starts this service and it ends with it), and what the devices and the target share.
class Hub {
   public:
    static Hub& Get();

    // Waits up to `timeoutMs` for the streamer's bridge.
    bool Open(int timeoutMs);
    rcvr::Shared* Shared() const { return m_bridge.shared(); }
    rcvr::BridgeClient& Bridge() { return m_bridge; }

    // The display the service was started for: Monado can't change it while it runs.
    const rcvr::DisplayConfig& Display() const { return m_display; }
    float RefreshRate() const { return m_display.refreshRate > 1 ? m_display.refreshRate : 90.f; }
    int64_t PeriodNs() const { return int64_t(1e9 / RefreshRate()); }

    // The newest tracking from the headset; false while there is none.
    bool Tracking(rcvr::TrackingSnapshot& out) const;

    // The compositor's thread marks itself, so the pose it renders the final image with can be
    // told apart from the app's queries (that pose goes with the frame to the headset).
    void SetCompositorThread() { m_compositorThread = std::this_thread::get_id(); }
    bool OnCompositorThread() const { return std::this_thread::get_id() == m_compositorThread.load(); }
    void SetRenderedWith(const rcvr::TrackingSnapshot& t);
    rcvr::TrackingSnapshot RenderedWith() const;

    // Producer heartbeat for the streamer: it shows the runtime as running while this beats.
    void StartHeartbeat();
    void StopHeartbeat();

    // The vsyncs the streamer steers to the headset's display: epoch + k * period.
    int64_t NextVsync(int64_t now, int64_t last) const;

   private:
    rcvr::BridgeClient m_bridge;
    rcvr::DisplayConfig m_display{};
    std::atomic<std::thread::id> m_compositorThread{};
    mutable std::mutex m_renderedMutex;
    rcvr::TrackingSnapshot m_rendered{};
    std::thread m_heartbeat;
    std::atomic<bool> m_beating{false};
    int64_t m_ownEpochNs = 0;
};

// The devices for Monado: the headset, both controllers, and the spaces they're tracked in.
xrt_result_t CreateSystemDevices(struct xrt_session_event_sink* broadcast, struct xrt_system_devices** out_xsysd,
                                 struct xrt_space_overseer** out_xso);

// The compositor target that renders into the streamer's frame images.
const struct comp_target_factory* TargetFactory();

// Bridge pose (x, y, z, w; position) to Monado's.
inline xrt_pose ToXrt(const rcvr::BridgePose& p) {
    xrt_pose out;
    out.orientation = {p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]};
    out.position = {p.position[0], p.position[1], p.position[2]};
    return out;
}

}  // namespace rcm
