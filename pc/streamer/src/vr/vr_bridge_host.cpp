#include "vr_bridge_host.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "../settings.h"
#include "rc/platform.h"
#include "rc/protocol.h"

namespace rcs {

bool VrBridgeHost::Start() {
    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(rcvr::Shared),
                                   rcvr::kSharedMemoryName);
    if (!m_mapping) {
        RC_LOGE("VR bridge: CreateFileMapping failed (%lu)", GetLastError());
        return false;
    }
    m_shared = static_cast<rcvr::Shared*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(rcvr::Shared)));
    if (!m_shared) return false;
    std::memset(m_shared, 0, sizeof(rcvr::Shared));
    m_shared->magic = rcvr::kMagic;
    m_shared->version = rcvr::kVersion;
    m_shared->latestReadySlot = -1;
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, rcvr::kFrameReadyEventName);
    m_trackingEvent = CreateEventW(nullptr, FALSE, FALSE, rcvr::kTrackingEventName);

    auto adapter = PreferredAdapter();
    m_gpu = DescribeAdapter(adapter.Get());
    m_device = CreateDevice(adapter.Get());
    if (!m_device) return false;
    rcvr::DisplayConfig initialConfig{};
    initialConfig.adapterLuid = m_gpu.luid;
    rcvr::WriteDisplayConfig(m_shared, initialConfig);

    // Restore the last headset configuration so SteamVR can start before a
    // headset connects (it then shows as "standby").
    std::vector<uint8_t> bytes;
    if (ReadFileBytes(DataDir() / L"display.json", bytes)) {
        try {
            const json j = json::parse(bytes.begin(), bytes.end());
            rcvr::DisplayConfig c{};
            c.eyeWidth = j.value("eyeWidth", 0u);
            c.eyeHeight = j.value("eyeHeight", 0u);
            c.refreshRate = j.value("refreshRate", 90.f);
            c.ipd = j.value("ipd", 0.063f);
            const auto fov = j.value("fov", json::array());
            for (int e = 0; e < 2 && e < int(fov.size()); e++)
                for (int k = 0; k < 4; k++) c.fovTan[e][k] = fov[e][k].get<float>();
            strncpy_s(c.headsetModel, j.value("model", "Quest").c_str(), _TRUNCATE);
            c.renderEyeWidth = j.value("renderEyeWidth", 0u);
            c.renderEyeHeight = j.value("renderEyeHeight", 0u);
            // Only the display is restored. Controller identities require this session's headset data.
            if (c.eyeWidth && c.eyeHeight) Configure(c);
        } catch (...) {
        }
    }

    m_running = true;
    m_thread = std::thread([this] { HeartbeatLoop(); });
    RC_LOGI("VR bridge ready on %s", m_gpu.name.c_str());
    return true;
}

void VrBridgeHost::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    for (auto& h : m_textureHandles)
        if (h) CloseHandle(h), h = nullptr;
    for (auto& t : m_textures) t.Reset();
    if (m_shared) UnmapViewOfFile(m_shared);
    if (m_mapping) CloseHandle(m_mapping);
    if (m_frameEvent) CloseHandle(m_frameEvent);
    if (m_trackingEvent) CloseHandle(m_trackingEvent);
    m_shared = nullptr;
    m_mapping = m_frameEvent = m_trackingEvent = nullptr;
}

bool VrBridgeHost::CreateTextures(uint32_t width, uint32_t height) {
    ComPtr<ID3D11Device1> device1;
    m_device.As(&device1);
    m_generation++;
    for (int i = 0; i < rcvr::kFrameSlots; i++) {
        if (m_textureHandles[i]) CloseHandle(m_textureHandles[i]);
        m_textureHandles[i] = nullptr;
        m_textures[i].Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        // Typeless so producers can render through an sRGB or UNORM view to
        // match their source, while we always read gamma-encoded UNORM.
        td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_textures[i]);
        if (FAILED(hr)) {
            RC_LOGE("VR bridge: texture creation failed %s", HResultString(hr).c_str());
            return false;
        }
        wchar_t name[64];
        swprintf_s(name, L"Local\\RinCastVRTex.%u.%lu.%d", m_generation, GetCurrentProcessId(), i);
        ComPtr<IDXGIResource1> res;
        m_textures[i].As(&res);
        hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name,
                                     &m_textureHandles[i]);
        if (FAILED(hr)) {
            RC_LOGE("VR bridge: CreateSharedHandle failed %s", HResultString(hr).c_str());
            return false;
        }
        wcscpy_s(m_shared->textureNames[i], name);
        m_shared->slots[i].state = rcvr::kSlotFree;
    }
    m_shared->textureWidth = width;
    m_shared->textureHeight = height;
    m_shared->textureFormat = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    m_shared->latestReadySlot = -1;
    return true;
}

void VrBridgeHost::SetControllerProfiles(const std::string& left, const std::string& right) {
    std::lock_guard lock(m_mutex);
    if (!m_shared) return;
    const std::string profiles[] = {left, right};
    auto config = m_shared->config;
    bool changed = false;
    for (int hand = 0; hand < 2; ++hand)
        if (profiles[hand] != config.controllerProfiles[hand]) {
            strncpy_s(config.controllerProfiles[hand], profiles[hand].c_str(), _TRUNCATE);
            changed = true;
            RC_LOGI("Headset controller profile (%s): %s", hand == 0 ? "left" : "right", profiles[hand].c_str());
        }
    if (changed) rcvr::WriteDisplayConfig(m_shared, config);
}

void VrBridgeHost::SetTouchControllers(bool touch) {
    std::lock_guard lock(m_mutex);
    if (m_shared) {
        auto config = m_shared->config;
        config.touchControllers = touch;
        rcvr::WriteDisplayConfig(m_shared, config);
    }
}

bool VrBridgeHost::Configure(const rcvr::DisplayConfig& config) {
    std::lock_guard lock(m_mutex);
    m_driftNsPerS = 0;  // another session, maybe another headset: learn its drift afresh
    rcvr::DisplayConfig c = config;
    c.adapterLuid = m_gpu.luid;
    c.touchControllers = SettingsStore::Get().Snapshot().controllerMode == "touch";
    const auto& old = m_shared->config;
    const bool resolutionChanged = !m_hasConfig || old.eyeWidth != c.eyeWidth || old.eyeHeight != c.eyeHeight;
    // What SteamVR renders at, which it only takes on when it starts.
    const auto renderSize = [](const rcvr::DisplayConfig& d) {
        return std::make_pair(d.renderEyeWidth ? d.renderEyeWidth : d.eyeWidth, d.renderEyeHeight ? d.renderEyeHeight : d.eyeHeight);
    };
    const bool renderSizeChanged = !m_hasConfig || renderSize(old) != renderSize(c);
    const bool displayChanged = resolutionChanged || old.refreshRate != c.refreshRate ||
                                std::memcmp(old.fovTan, c.fovTan, sizeof(c.fovTan)) != 0;
    if (resolutionChanged && !CreateTextures(c.eyeWidth * 2, c.eyeHeight)) return false;
    rcvr::WriteDisplayConfig(m_shared, c);
    if (displayChanged) InterlockedIncrement(&m_shared->configGeneration);
    m_hasConfig = true;

    json fov = json::array();
    for (int e = 0; e < 2; e++) fov.push_back({c.fovTan[e][0], c.fovTan[e][1], c.fovTan[e][2], c.fovTan[e][3]});
    WriteFileAtomic(DataDir() / L"display.json",
                    json{{"eyeWidth", c.eyeWidth}, {"eyeHeight", c.eyeHeight}, {"refreshRate", c.refreshRate},
                         {"ipd", c.ipd}, {"fov", fov}, {"model", c.headsetModel},
                         {"renderEyeWidth", c.renderEyeWidth}, {"renderEyeHeight", c.renderEyeHeight}}
                        .dump());
    // A running SteamVR keeps the resolution it started with; refresh rate, field of view,
    // IPD and the stream's size are applied live by our driver (it watches configGeneration).
    return renderSizeChanged && m_shared->producerKind == LONG(rcvr::ProducerKind::SteamVR);
}

bool VrBridgeHost::SteamVrEyeSize(uint32_t& width, uint32_t& height) const {
    if (ActiveProducer() != rcvr::ProducerKind::SteamVR) return false;
    width = uint32_t(m_shared->renderedEyeWidth);
    height = uint32_t(m_shared->renderedEyeHeight);
    return width >= 256 && height >= 256;
}

rcvr::DisplayConfig VrBridgeHost::LastConfig() const {
    std::lock_guard lock(m_mutex);
    return m_shared ? m_shared->config : rcvr::DisplayConfig{};
}

void VrBridgeHost::SetHeadsetConnected(bool connected) {
    std::lock_guard lock(m_mutex);
    if (!m_shared) return;
    InterlockedExchange(&m_shared->headsetConnected, connected ? 1 : 0);
    if (!connected) {
        auto config = m_shared->config;
        std::memset(config.controllerProfiles, 0, sizeof(config.controllerProfiles));
        rcvr::WriteDisplayConfig(m_shared, config);
        rcvr::WriteTracking(m_shared, rcvr::TrackingSnapshot{});
        for (int hand = 0; hand < 2; ++hand) rcvr::WriteHand(m_shared, hand, rcvr::HandSnapshot{});
        rcvr::WriteBody(m_shared, rcvr::BodySnapshot{});
    }
}

void VrBridgeHost::PublishTracking(const rcvr::TrackingSnapshot& snapshot) {
    std::lock_guard lock(m_mutex);
    if (!m_shared) return;
    rcvr::WriteTracking(m_shared, snapshot);
    SetEvent(m_trackingEvent);
}

void VrBridgeHost::PublishBody(const rcvr::BodySnapshot& snapshot) {
    std::lock_guard lock(m_mutex);
    if (m_shared) rcvr::WriteBody(m_shared, snapshot);
}

void VrBridgeHost::PublishHand(int hand, const rcvr::HandSnapshot& snapshot) {
    std::lock_guard lock(m_mutex);
    if (m_shared) rcvr::WriteHand(m_shared, hand, snapshot);
}

void VrBridgeHost::Recenter() {
    if (m_shared) InterlockedIncrement(&m_shared->recenterCount);
}

void VrBridgeHost::ShiftVsync(int64_t shiftNs) {
    const double s = std::clamp(double(shiftNs), -10e6, 10e6);
    // Proportional for the offset, integral for the drift; settles in a few seconds.
    m_pendingShiftNs.fetch_add(0.5 * s);
    m_driftNsPerS = std::clamp(m_driftNsPerS.load() + 0.2 * s, -2e6, 2e6);
}

void VrBridgeHost::SlipVsync(int64_t slipNs) {
    if (slipNs <= 0) return;
    // A few frames at most: more than that waiting is no longer a matter of timing.
    m_pendingSlipNs = std::min(m_pendingSlipNs.load() + double(slipNs), 50e6);
}

bool VrBridgeHost::PoseWaitNs(int64_t atNs, int64_t& waitNs, int64_t& periodNs) const {
    if (ActiveProducer() != rcvr::ProducerKind::SteamVR) return false;
    const float rate = m_shared->config.refreshRate;
    const int64_t epoch = m_shared->vsyncEpochNs;
    if (rate <= 1 || !epoch) return false;
    // As the driver's TrackingLoop counts: vsyncs at epoch + k * period, poses read 1 ms before.
    periodNs = int64_t(1e9 / rate);
    const int64_t pickup = epoch - 1'000'000;
    waitNs = ((pickup - atNs) % periodNs + periodNs) % periodNs;
    return true;
}

void VrBridgeHost::SetHapticHandler(HapticHandler h) {
    std::lock_guard lock(m_mutex);
    m_onHaptic = std::move(h);
}

rcvr::ProducerKind VrBridgeHost::ActiveProducer() const {
    if (!m_shared) return rcvr::ProducerKind::None;
    if (rcvr::QpcNowNs() - m_shared->producerHeartbeatNs > 1'500'000'000LL) return rcvr::ProducerKind::None;
    return rcvr::ProducerKind(m_shared->producerKind);
}

std::string VrBridgeHost::ApplicationName() const {
    if (!m_shared) return {};
    char name[sizeof(m_shared->applicationName)];
    std::memcpy(name, m_shared->applicationName, sizeof(name));
    name[sizeof(name) - 1] = 0;
    return name;
}

// A producer (vrserver or a game) that dies mid-frame leaves a slot in the
// "writing" state and possibly a keyed mutex it owned. Reset everything and
// hand out fresh textures so the next producer starts clean.
void VrBridgeHost::RecoverFromDeadProducer() {
    std::lock_guard lock(m_mutex);
    RC_LOGW("VR producer (pid %ld) exited; resetting the VR bridge", m_shared->producerPid);
    const uint32_t w = m_shared->textureWidth, h = m_shared->textureHeight;
    m_shared->producerPid = 0;
    m_shared->producerKind = LONG(rcvr::ProducerKind::None);
    m_shared->renderedEyeWidth = m_shared->renderedEyeHeight = 0;
    if (w && h) CreateTextures(w, h);
    InterlockedIncrement(&m_shared->configGeneration);
}

void VrBridgeHost::HeartbeatLoop() {
    // Haptics are latency sensitive: poll at 1 kHz-ish, heartbeat at 10 Hz.
    int64_t lastBeat = 0;
    int64_t lastProducerCheck = 0;
    int64_t lastPhase = rcvr::QpcNowNs();
    double phaseNs = 0;
    if (!m_epochBaseNs) m_epochBaseNs = lastPhase;
    while (m_running) {
        const int64_t now = rcvr::QpcNowNs();
        // The driver's vsyncs fall on vsyncEpochNs + k * period.
        phaseNs += m_pendingShiftNs.exchange(0) + m_driftNsPerS.load() * double(now - lastPhase) / 1e9;
        // A slip in small steps, so no vsync comes much later than the one before: games see a
        // frame rate a few percent lower for a moment, not a frame missing.
        if (const double slip = m_pendingSlipNs.load(); slip > 0) {
            const double step = std::min(slip, rc::kVrPhaseSlipRate * double(now - lastPhase));
            phaseNs += step;
            m_pendingSlipNs.fetch_sub(step);
        }
        lastPhase = now;
        InterlockedExchange64(&m_shared->vsyncEpochNs, m_epochBaseNs + int64_t(phaseNs));
        if (now - lastBeat > 100'000'000LL) {
            InterlockedExchange64(&m_shared->streamerHeartbeatNs, now);
            lastBeat = now;
        }
        if (now - lastProducerCheck > 500'000'000LL) {
            lastProducerCheck = now;
            if (const DWORD pid = DWORD(m_shared->producerPid)) {
                HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
                const bool alive = process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
                if (process) CloseHandle(process);
                if (!alive) RecoverFromDeadProducer();
            }
        }
        const LONG write = m_shared->hapticsWrite;
        if (write - m_hapticsRead > rcvr::kHapticsRing) m_hapticsRead = write - rcvr::kHapticsRing;
        while (m_hapticsRead < write) {
            const rcvr::HapticEvent& e = m_shared->haptics[m_hapticsRead % rcvr::kHapticsRing];
            if (e.sequence == uint32_t(m_hapticsRead + 1)) {
                HapticHandler handler;
                {
                    std::lock_guard lock(m_mutex);
                    handler = m_onHaptic;
                }
                if (handler) handler(e);
            }
            m_hapticsRead++;
        }
        Sleep(1);
    }
}

}  // namespace rcs
