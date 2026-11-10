// Streamer side of the VR bridge: owns the shared memory, the shared eye
// textures and the events that the SteamVR driver / OpenXR runtime use.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include "../gpu.h"
#include "vr_bridge.h"

namespace rcs {

class VrBridgeHost {
   public:
    using HapticHandler = std::function<void(const rcvr::HapticEvent&)>;

    bool Start();
    void Stop();

    // Applies a headset display configuration. Recreates textures when the
    // resolution changes. Returns true if SteamVR needs a restart to apply.
    bool Configure(const rcvr::DisplayConfig& config);
    // Takes effect when SteamVR next creates its controller devices.
    void SetTouchControllers(bool touch);
    void SetControllerProfiles(const std::string& left, const std::string& right);
    void SetHeadsetConnected(bool connected);
    void PublishTracking(const rcvr::TrackingSnapshot& snapshot);
    void PublishBody(const rcvr::BodySnapshot& snapshot);
    void PublishHand(int hand, const rcvr::HandSnapshot& snapshot);
    void Recenter();
    // Moves the driver's vsync by about half of shiftNs (+ = later), and learns the rate at
    // which the two clocks drift apart, from the headset's reports twice a second.
    void ShiftVsync(int64_t shiftNs);

    void SetHapticHandler(HapticHandler h);

    rcvr::Shared* Shared() const { return m_shared; }
    HANDLE FrameEvent() const { return m_frameEvent; }
    ID3D11Device* Device() const { return m_device.Get(); }
    UINT VendorId() const { return m_gpu.vendorId; }
    // Shared texture for a slot (same device as Device()).
    ComPtr<ID3D11Texture2D> SlotTexture(int slot) const {
        std::lock_guard lock(m_mutex);
        return m_textures[slot];
    }
    uint32_t TextureGeneration() const { return m_generation; }

    rcvr::ProducerKind ActiveProducer() const;
    std::string ApplicationName() const;
    bool HasConfig() const { return m_hasConfig; }
    // The size SteamVR renders each eye at, from its frames; false while it sends none.
    bool SteamVrEyeSize(uint32_t& width, uint32_t& height) const;
    rcvr::DisplayConfig LastConfig() const;

   private:
    void HeartbeatLoop();
    bool CreateTextures(uint32_t width, uint32_t height);
    void RecoverFromDeadProducer();

    HANDLE m_mapping = nullptr;
    HANDLE m_frameEvent = nullptr;
    HANDLE m_trackingEvent = nullptr;
    rcvr::Shared* m_shared = nullptr;

    ComPtr<ID3D11Device> m_device;
    GpuInfo m_gpu;
    ComPtr<ID3D11Texture2D> m_textures[rcvr::kFrameSlots];
    HANDLE m_textureHandles[rcvr::kFrameSlots] = {};
    uint32_t m_generation = 0;
    bool m_hasConfig = false;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    mutable std::mutex m_mutex;
    HapticHandler m_onHaptic;
    LONG m_hapticsRead = 0;
    int64_t m_epochBaseNs = 0;
    std::atomic<double> m_pendingShiftNs{0};  // for the heartbeat loop, which owns the phase
    std::atomic<double> m_driftNsPerS{0};
};

}  // namespace rcs
