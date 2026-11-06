// Stands in for app/app.h when screens.cpp is compiled for the desktop preview: the same calls,
// answered from a scene description instead of a headset and a PC.
#pragma once

#include <deque>
#include <string>
#include <vector>

#include "app/latency.h"
#include "app/settings.h"
#include "platform/jni_bridge.h"
#include "gfx/sky.h"
#include "gfx/ui_renderer.h"
#include "nlohmann/json.hpp"
#include "ui/window.h"

namespace rv {

using json = nlohmann::json;

enum class Mode { Home, Connecting, Desktop, LaunchingVr, Vr };

struct GameInfo {
    std::string id;
    std::string name;
    bool hasImage = false;
    Texture image;
    bool imageRequested = false;
};

struct Notice {
    std::string text;
    std::string level;
    std::string action;
    float age = 0;
    float life = 6;
};

struct DiscoveredPc {
    std::string serverId, name, gpu;
    bool busy = false;
    bool viaVpn = false;
    bool viaUsb = false;
};

struct CloudPc {
    std::string id, name, gpu;
    bool online = false, busy = false;
};

class Discovery {
   public:
    std::vector<DiscoveredPc> pcs;
    std::vector<DiscoveredPc> List() { return pcs; }
};

class Cloud {
   public:
    enum class SignInState { SignedOut, RequestingCode, WaitingForApproval, SignedIn, Error };
    SignInState state = SignInState::SignedOut;
    std::string user, code, url, error;
    std::vector<CloudPc> pcs;
    SignInState State() const { return state; }
    std::string UserCode() { return code; }
    std::string VerificationUrl() { return url; }
    std::string UserName() { return user; }
    std::string Error() { return error; }
    std::vector<CloudPc> Pcs() { return pcs; }
    void BeginSignIn() {}
    void CancelSignIn() {}
    void SignOut() {}
    void RefreshPcs() {}
};

class StreamClient {
   public:
    enum class State { Idle, Connecting, WaitingApproval, Connected, Failed };
    State state = State::Idle;
    std::string serverName, status;
    bool remote = false, vpn = false;
    State GetState() const { return state; }
    std::string StatusText() { return status; }
    std::string ServerName() { return serverName; }
    bool IsRemote() const { return remote; }
    bool IsVpn() const { return vpn; }
};

class XrContext {
   public:
    std::vector<float> rates = {72, 80, 90, 120};
    int RecommendedWidth() const { return 2064; }
    int RecommendedHeight() const { return 2208; }
    std::vector<float> RefreshRates() const { return rates; }
    bool PassthroughSupported() const { return true; }
};

// The PICO family where `pico` is set (scene streaming-pico).
class Headset {
   public:
    bool pico = false;
    const char* SettingsName() const { return pico ? "PICO's settings" : "Quest settings"; }
    bool BodySupported(const XrContext&) const { return true; }
    const char* BodyHint() const { return pico ? "With PICO Motion Trackers" : nullptr; }
    std::string BodyStatus() const { return pico ? "3 Motion Trackers. Turn it on standing up" : ""; }
    bool BodyExcludesFingers() const { return !pico; }
    const char* BodyAction() const { return pico ? "Calibrate" : nullptr; }
    void RunBodyAction(XrContext&) {}
    bool FaceSupported(const XrContext&) const { return true; }
    const char* FaceHint() const { return "For avatars, through VRCFaceTracking on the PC"; }
};

class XrInput {
   public:
    bool simultaneous = true;
    bool SimultaneousSupported() const { return simultaneous; }
};

class App {
   public:
    HeadsetSettings settings;
    Mode mode = Mode::Home;
    Discovery lan;
    Cloud account;
    StreamClient client;
    XrContext xr;
    Headset device;
    XrInput input;
    std::string problem;
    bool reconnecting = false, canRetry = true;
    float noPcsFor = 0, gamesAgo = 0, time = 100;
    bool gamesAnswered = true, monitorsVisible = true;
    json serverInfo = json::object();
    std::vector<GameInfo> games;
    std::deque<Notice> notices;
    std::string vrState = "Half-Life: Alyx";
    struct Stats {
        double fps = 0, latencyMs = 0, decodeMs = 0, mbps = 0, rttMs = 0;
        std::string codec;
        int width = 0, height = 0;
        double stageMs[kStageCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    } stats;
    std::vector<std::string> open;

    HeadsetSettings& Settings() { return settings; }
    void SaveSettings() {}
    void ApplyStreamSettings() {}
    Mode CurrentMode() const { return mode; }
    Discovery& Lan() { return lan; }
    Cloud& Account() { return account; }
    StreamClient& Client() { return client; }
    XrContext& Xr() { return xr; }
    Headset& Device() { return device; }
    XrInput& Input() { return input; }
    void ConnectLan(const DiscoveredPc&) {}
    void ConnectCloud(const CloudPc&) {}
    void Disconnect() {}
    void LaunchSteamVr() {}
    void LaunchGame(const std::string&) {}
    void ExitVr() {}
    void Recenter() {}
    void UpdateScreenShape(float) {}
    const std::string& ConnectionProblem() const { return problem; }
    bool Reconnecting() const { return reconnecting; }
    int ReconnectAttempt() const { return 2; }
    float GamesRequestedAgo() const { return gamesAgo; }
    bool GamesAnswered() const { return gamesAnswered; }
    float Time() const { return time; }
    bool CanRetry() const { return canRetry; }
    void RetryConnection() {}
    void DismissProblem() {}
    void SetManualPc(const std::string&) {}
    float TimeOnHomeWithoutPcs(bool) { return noPcsFor; }
    void ApplyAccessibility() {}
    void ToggleWindow(const std::string&, bool) {}
    void ToggleWindow(const std::string&) {}
    bool IsVisible(const std::string& id) {
        for (const auto& o : open)
            if (o == id) return true;
        return false;
    }
    void ShowMonitors(bool) {}
    bool MonitorsVisible() const { return monitorsVisible; }
    void SendKey(uint16_t, bool) {}
    void SendText(const std::string&) {}
    void RunAction(const std::string&) {}
    void DismissNotice() {}
    float ModeTime() const { return 12; }
    const Texture& EnvironmentThumbnail(const std::string& key);
    const json& ServerInfo() const { return serverInfo; }
    std::vector<GameInfo>& Games() { return games; }
    void RequestGames() {}
    void RequestGameImage(GameInfo&) {}
    std::string VrStateText() const { return vrState; }
    bool VrRunning() const { return false; }
    Platform::UsbNetwork usb;
    const Platform::UsbNetwork& UsbNetwork() const { return usb; }
    void SetUsbCable(bool on) { settings.usbCable = on; }
    const std::deque<Notice>& Notices() const { return notices; }
    Stats CurrentStats() const { return stats; }
};

}  // namespace rv
