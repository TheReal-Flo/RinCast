// Application root: owns the long-lived services and mediates between them
// and the UI.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "net/firewall.h"
#include "cloud/cloud_client.h"
#include "encode/encoder.h"
#include "games/game_library.h"
#include "gpu.h"
#include "net/stream_server.h"
#include "util.h"
#include "vr/vr_bridge_host.h"

namespace rcs {

class ClientSession;

class App {
   public:
    static App& Get();

    bool Init();
    void Shutdown();

    // Services
    StreamServer& Server() { return m_server; }
    VrBridgeHost& Bridge() { return m_bridge; }
    CloudClient& Cloud() { return m_cloud; }
    GameLibrary& Games() { return m_games; }
    const GpuInfo& Gpu() const { return m_gpu; }
    const EncoderCaps& Caps() const { return m_caps; }
    std::string Version() const;

    // Events from services (any thread)
    void OnSessionStarted(ClientSession* session);
    void OnSessionEnded(ClientSession* session, const std::string& reason);
    void OnVrStateChanged(const std::string& state, const std::string& producer, const std::string& app);

    // UI bridge. The sink posts JSON to the web UI (marshalled to the UI thread).
    using UiSink = std::function<void(const std::string& json)>;
    void SetUiSink(UiSink sink);
    void HandleUiMessage(const std::string& text);  // from the web UI (UI thread)
    void PushUi(const json& message);
    json StateJson();
    void Tick();  // ~1 Hz from the UI thread

    // Requests the window to show itself (approval prompts).
    std::function<void()> ShowWindowRequest;

   private:
    std::string ApplySettingSideEffects(const json& changed);  // what went wrong, if anything
    void RunAsync(std::function<void()> fn);

    StreamServer m_server;
    VrBridgeHost m_bridge;
    CloudClient m_cloud;
    std::mutex m_firewallMutex;
    firewall::Status m_firewall;
    json FirewallJson();
    std::atomic<bool> m_firewallChecking{false};
    int m_firewallTicks = 0;
    void CheckFirewall();
    GameLibrary m_games;
    GpuInfo m_gpu;
    EncoderCaps m_caps;

    std::mutex m_uiMutex;
    UiSink m_uiSink;
    json m_pendingApproval;
    std::atomic<bool> m_streaming{false};
    std::mutex m_eventMutex;
    std::vector<json> m_recentEvents;
    std::set<std::string> m_noRemember;  // approved once without "remember"
};

}  // namespace rcs
