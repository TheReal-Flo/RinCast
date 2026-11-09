#include "app.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <android_native_app_glue.h>
#include <sys/system_properties.h>
#else
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../platform/jni_bridge.h"

#include <net/if.h>
#include "rc/hand_skeleton.h"
#include "rc/platform.h"
#include "rc/protocol.h"
#include "stb/stb_image.h"
#include "strings.h"

namespace rv {

namespace screens {
void Build(App& app, WindowManager& wm);
void SetPage(int page);
void ToggleMenu(App& app);
}

static const char* kVersion = RINCAST_VERSION_STRING;

App::~App() { Shutdown(); }

#ifdef __ANDROID__
App::App(android_app* androidApp) : m_android(androidApp), m_assets(androidApp->activity->assetManager) {}

static void OnAppCmd(android_app* app, int32_t cmd) {
    auto* self = static_cast<App*>(app->userData);
    (void)self;
    switch (cmd) {
        case APP_CMD_RESUME: RC_LOGI("onResume"); break;
        case APP_CMD_PAUSE: RC_LOGI("onPause"); break;
        case APP_CMD_DESTROY: RC_LOGI("onDestroy"); break;
        default: break;
    }
}

void App::HandleAndroidEvents() {
    int events;
    android_poll_source* source;
    const int timeout = (!m_xr.Running() && !m_exit) ? 10 : 0;
    while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
        if (source) source->process(m_android, source);
        if (m_android->destroyRequested) {
            m_exit = true;
            return;
        }
        if (timeout == 0) continue;
        break;
    }
}

void App::Run() {
    // Attached for good: PICO's runtime calls into Java from the app's thread.
    JNIEnv* env = nullptr;
    m_android->activity->vm->AttachCurrentThread(&env, nullptr);
    m_android->userData = this;
    m_android->onAppCmd = OnAppCmd;
    Init();
    while (!m_exit) {
        HandleAndroidEvents();
        if (m_exit) break;
        if (!m_xr.PollEvents()) {
            ANativeActivity_finish(m_android->activity);
            m_exit = true;
            break;
        }
        if (!m_xr.Running()) {
            ReleaseMicInBackground();
            m_music.Update(false);  // headset off or asleep: fade out
            continue;
        }
        Frame();
    }
    Shutdown();
    m_android->activity->vm->DetachCurrentThread();
}

#else
// Linux (the Steam Frame): a plain program that Steam starts. It ends when SteamVR ends the
// session or on SIGINT / SIGTERM.
namespace {
std::atomic<bool> g_quit{false};
void OnSignal(int) { g_quit = true; }
}  // namespace

App::App(android_app* androidApp) : m_android(androidApp) {}

void App::HandleAndroidEvents() {
    if (g_quit) m_exit = true;
}

void App::Run() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    Init();
    while (!m_exit) {
        HandleAndroidEvents();
        if (m_exit) break;
        if (!m_xr.PollEvents()) {
            m_exit = true;
            break;
        }
        if (!m_xr.Running()) {
            ReleaseMicInBackground();
            m_music.Update(false);
            usleep(10'000);
            continue;
        }
        Frame();
    }
    Shutdown();
}
#endif

void App::Init() {
    Platform::Init(m_android);
    m_settingsPath = Platform::FilesDir() + "/settings.json";
    m_settings = HeadsetSettings::Load(m_settingsPath);
    SetLanguage(m_settings.language, Platform::SystemLocale());
    {
        rc::crypto::IdentitySeed seed{};
        if (!rc::HexDecode(m_settings.deviceSecret, seed.data(), seed.size())) {
            seed = rc::crypto::NewIdentitySeed();
            m_settings.deviceSecret = rc::HexEncode(seed.data(), seed.size());
            m_settings.Save(m_settingsPath);
        }
        m_client.SetIdentity(seed);
    }
    if (!m_xr.Init(m_android, Headset::Extensions()) || !(m_headset = Headset::Create(m_xr)) ||
        !m_xr.StartSession(m_headset->PreferStageSpace() || m_settings.fixedPlaySpace, m_headset->VelocitiesFromPoses())) {
        RC_LOGE("OpenXR initialization failed");
        m_exit = true;
        return;
    }
    RC_LOGI("Headset family: %s", m_headset->Family());
    Platform::RequestPermissions(m_headset->StartupPermissions());
    m_input.Init(m_xr, m_headset->Controllers());
    m_sky.Init(m_assets);
    m_music.Load(m_assets);
    m_ui.Init(m_assets);
    m_scene.Init();
    m_windows.Init(&m_ui, &m_scene);
    m_xr.SetRefreshRate(m_settings.refreshRate);
    m_discovery.Start(Platform::DeviceId(), Platform::DeviceModel());
    m_cloud.Start(m_settings.cloudServer, m_settings.cloudRefreshToken, m_settings.cloudUserName,
                  [this](const std::string& refresh, const std::string& name) {
                      std::lock_guard lock(m_netMutex);
                      m_controlQueue.push_back(json{{"type", "_cloudTokens"}, {"refresh", refresh}, {"name", name}});
                  });

    StreamClient::Callbacks cb;
    cb.onControl = [this](const json& m) {
        std::lock_guard lock(m_netMutex);
        m_controlQueue.push_back(m);
    };
    cb.onVideoFrame = [this](rc::ReceivedFrame&& f) { OnVideoFrame(std::move(f)); };
    cb.onAudio = [this](const uint8_t* d, size_t n) {
        if (n > 0 && d[0] == uint8_t(rc::AudioStream::PcToHeadset)) m_audio.PushPacket(d, n);
    };
    cb.onHaptics = [this](const rc::HapticsPayload& h) {
        std::lock_guard lock(m_netMutex);
        m_controlQueue.push_back(json{{"type", "_haptic"}, {"hand", h.hand}, {"duration", h.durationS},
                                      {"frequency", h.frequency}, {"amplitude", h.amplitude}});
    };
    m_client.SetCallbacks(cb);

    CreateWindows();
    m_lastFrameNs = rc::NowNs();
    m_initialized = true;
    RC_LOGI("RinCast %s ready", kVersion);
}

void App::Shutdown() {
    if (!m_initialized) return;
    m_initialized = false;
    m_client.Disconnect();
    m_discovery.Stop();
    m_cloud.Stop();
    m_audio.Stop();
    m_mic.Stop();
    m_decoders.clear();
    for (auto& [id, sc] : m_monitorLayers) m_xr.DestroySwapchain(sc);
    for (auto& [id, sc] : m_uiLayers) m_xr.DestroySwapchain(sc);
    m_xr.DestroySwapchain(m_vrSwapchain);
    m_xr.DestroySwapchain(m_statsLayer);
    m_xr.Shutdown();
    Platform::SetStreamingLocks(false);
}

void App::SaveSettings() { m_settings.Save(m_settingsPath); }

void App::ApplyStreamSettings() {
    SaveSettings();
    m_xr.SetRefreshRate(m_settings.refreshRate);
    m_audio.SetVolume(m_settings.volume);
    m_mic.SetGain(m_settings.micVolume);
    if (m_client.GetState() == StreamClient::State::Connected)
        m_client.SendControl({{"type", rc::msg::kUpdateSettings}, {"settings", m_settings.StreamSettingsJson()}});
    if (m_settings.micPassthrough && m_client.GetState() == StreamClient::State::Connected && !m_mic.Running())
        m_mic.Start([this](const uint8_t* d, size_t n) { m_client.SendAudio(d, n); });
    if (!m_settings.micPassthrough) m_mic.Stop();
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

void App::SendHello() {
    // (The display description is also sent in client.info once connected.)
}

static json HelloJson(XrContext& xr, const HeadsetSettings& s, const FrameState* frame) {
    json fov = json::array();
    float ipd = 0.063f;
    if (frame) {
        for (int e = 0; e < 2; e++) {
            const auto& f = frame->eyes[e].fov;
            fov.push_back({std::tan(-f.angleLeft), std::tan(f.angleRight), std::tan(f.angleUp), std::tan(-f.angleDown)});
        }
        ipd = Length(frame->eyes[0].pose.position - frame->eyes[1].pose.position);
        if (ipd < 0.05f || ipd > 0.08f) ipd = 0.063f;
    } else {
        fov = {{1.376, 0.839, 0.966, 1.191}, {0.839, 1.376, 0.966, 1.191}};
    }
    static const DecoderCaps dec = VideoDecoder::Capabilities();
    json rates = json::array();
    for (float r : xr.RefreshRates()) rates.push_back(r);
    return {{"deviceId", Platform::DeviceId()},
            {"deviceName", Platform::DeviceModel()},
            {"model", Platform::DeviceModel()},
            {"appVersion", kVersion},
            {"display",
             {{"eyeWidth", xr.RecommendedWidth()},
              {"eyeHeight", xr.RecommendedHeight()},
              {"refreshRates", rates},
              {"ipd", ipd},
              {"fov", fov}}},
            {"decoders",
             {{"h264", dec.h264}, {"hevc", dec.hevc}, {"hevc10", dec.hevc10}, {"av1", dec.av1}, {"av110", dec.av110},
              {"maxWidth", dec.maxWidth}, {"maxHeight", dec.maxHeight}}},
            {"settings", s.StreamSettingsJson()}};
}

static FrameState g_lastFrame;
static bool g_haveFrame = false;

void App::ConnectLan(const DiscoveredPc& pc) {
    m_haveLast = true;
    m_lastIsCloud = false;
    m_lastLan = pc;
    ConnectTarget t;
    t.name = pc.name;
    t.direct = pc.address;
    t.viaVpn = pc.viaVpn;
    if (pc.viaUsb) t.usbNetwork = m_usbNetwork.handle;
    m_settings.lastPc = pc.serverId;
    // A link-local address means nothing on another day or cable.
    if (!pc.viaUsb) m_settings.lastPcAddress = pc.address.IpString();
    SaveSettings();
    m_client.Connect(t, HelloJson(m_xr, m_settings, g_haveFrame ? &g_lastFrame : nullptr));
    EnterMode(Mode::Connecting);
}

void App::SetUsbCable(bool on) {
    m_settings.usbCable = on;
    SaveSettings();
    Platform::SetUsbNetwork(on);
    m_usbCheckedNs = 0;
}

void App::UpdateUsbNetwork() {
    const int64_t now = rc::NowNs();
    if (now - m_usbCheckedNs < 1'000'000'000LL) return;
    if (!m_usbCheckedNs && m_settings.usbCable) Platform::SetUsbNetwork(true);  // asked for at start
    m_usbCheckedNs = now;
    const Platform::UsbNetwork u = Platform::GetUsbNetwork();
    const bool changed = u.state != m_usbNetwork.state || u.handle != m_usbNetwork.handle || u.interface != m_usbNetwork.interface;
    m_usbNetwork = u;
    if (!changed) return;
    const uint32_t index = u.state == Platform::UsbNetwork::Connected && !u.interface.empty() ? if_nametoindex(u.interface.c_str()) : 0;
    RC_LOGI("USB network: state %d, interface %s (%u), %d kbps", int(u.state), u.interface.c_str(), index, u.kbps);
    m_discovery.SetUsbNetwork(index ? u.handle : 0, index);
}

void App::ConnectCloud(const CloudPc& pc) {
    m_haveLast = true;
    m_lastIsCloud = true;
    m_lastCloud = pc;
    m_settings.lastPc = pc.id;
    SaveSettings();
    EnterMode(Mode::Connecting);
    // If the PC is also on this network, prefer the direct path.
    for (const auto& lan : m_discovery.List()) {
        if (lan.serverId == pc.id) {
            ConnectLan(lan);
            return;
        }
    }
    m_cloud.Connect(pc.id, pc.name, [this](bool ok, ConnectTarget target, std::string error) {
        std::lock_guard lock(m_netMutex);
        if (!ok) {
            m_controlQueue.push_back(json{{"type", "_connectFailed"}, {"error", error}});
            return;
        }
        // Hand the target to the main thread (it owns the client lifecycle).
        json t{{"type", "_cloudTarget"},
               {"name", target.name},
               {"session", target.cloudSession},
               {"secret", target.connectSecret},
               {"token", target.sessionToken}};
        json rendezvous = json::array(), locals = json::array();
        for (const auto& a : target.rendezvous) rendezvous.push_back(a.ToString());
        for (const auto& a : target.pcLocalAddrs) locals.push_back(a.ToString());
        t["rendezvous"] = rendezvous;
        t["locals"] = locals;
        m_controlQueue.push_back(t);
    });
}

// Called from window contents, i.e. while windows are being drawn: tearing the session
// down there would delete monitor windows mid-iteration. Do it at the start of the next frame.
void App::Disconnect() { m_disconnectRequested = true; }

void App::DisconnectNow() {
    m_disconnectRequested = false;
    m_client.Disconnect();
    m_audio.Stop();
    m_mic.Stop();
    Platform::SetStreamingLocks(false);
    {
        std::lock_guard lock(m_decoderMutex);
        m_decoders.clear();
    }
    for (auto& [id, ds] : m_desktopStreams) m_windows.Remove("monitor-" + std::to_string(id));
    m_desktopStreams.clear();
    for (auto& [id, sc] : m_monitorLayers) m_xr.DestroySwapchain(sc);
    m_monitorLayers.clear();
    m_xr.DestroySwapchain(m_vrSwapchain);
    m_vrHasFrame = false;
    m_vrState = "idle";
    m_serverInfo = json::object();
    m_games.clear();
    m_monitorsLaidOut = false;
    EnterMode(Mode::Home);
}

void App::LaunchSteamVr() {
    m_client.SendControl({{"type", rc::msg::kLaunchSteamVr}});
    EnterMode(Mode::LaunchingVr);
}

void App::LaunchGame(const std::string& id) {
    m_client.SendControl({{"type", rc::msg::kLaunchGame}, {"id", id}});
    EnterMode(Mode::LaunchingVr);
}

void App::ExitVr() {
    m_client.SendControl({{"type", rc::msg::kStopVr}});
    EnterMode(Mode::Desktop);
}

void App::Recenter() {
    LayoutMonitors(true);
    LayoutHome(true);
}

void App::UpdateScreenShape(float previousDistance) {
    const float distance = m_settings.screenDistance;
    for (auto& [id, d] : m_desktopStreams) {
        Window* w = m_windows.Find("monitor-" + std::to_string(id));
        if (!w) continue;
        w->curveRadius = m_settings.curvature > 0.02f ? distance / std::max(0.05f, m_settings.curvature) : 0.f;
        w->layerBacked = m_settings.sharpLayers && (w->curveRadius == 0 || m_xr.CylinderLayers());
        if (distance != previousDistance) {
            // Slide along the line from the head, so the arrangement keeps its shape.
            const Vec3 from = w->pose.position - m_head.position;
            w->pose.position = m_head.position + from * (distance / std::max(0.2f, previousDistance));
            w->widthM *= distance / std::max(0.2f, previousDistance);
        }
    }
}

void App::RetryConnection() {
    if (!m_haveLast) return;
    m_problem.clear();
    if (m_lastIsCloud) ConnectCloud(m_lastCloud);
    else ConnectLan(m_lastLan);
}

// A connection that dropped by itself gets a few chances before the user has to act.
void App::ReconnectTick() {
    if (!Reconnecting() || m_time - m_lastRetry < 4.f) return;
    m_lastRetry = m_time;
    m_retryAttempt++;
    RetryConnection();
}

void App::SetManualPc(const std::string& address) {
    m_settings.manualPc = address;
    SaveSettings();
    rc::NetAddress a;
    if (rc::NetAddress::Parse(address, rc::kDiscoveryPort, a)) m_discovery.AddProbeTarget(a);
}

float App::TimeOnHomeWithoutPcs(bool noPcs) {
    if (!noPcs) m_noPcsSince = -1;
    else if (m_noPcsSince < 0) m_noPcsSince = m_time;
    return noPcs ? m_time - m_noPcsSince : 0;
}

void App::ApplyAccessibility() {
    theme::SetHighContrast(m_settings.highContrast);
    for (const auto& w : m_windows.All()) {
        if (w->kind == WindowKind::Ui) {
            if (w->baseWidthM <= 0) w->baseWidthM = w->widthM;
            w->widthM = w->baseWidthM * std::min(m_settings.uiScale, 1.3f);
            w->needsRedraw = true;
        }
    }
}

void App::SendKey(uint16_t vk, bool down) { m_client.SendControl({{"type", rc::msg::kKey}, {"vk", vk}, {"down", down}}); }

void App::SendText(const std::string& text) { m_client.SendControl({{"type", rc::msg::kText}, {"text", text}}); }

void App::PostNotice(const std::string& text, const std::string& level, const std::string& action) {
    // An error, or one with something to do, stays until it's answered.
    m_notices.push_back({text, level, action, 0, !action.empty() || level == "error" ? 3600.f : level == "warning" ? 10.f : 6.f});
    if (m_settings.readAloud) Platform::Speak(text);
    while (m_notices.size() > 3) m_notices.pop_front();
    ToggleWindow("notices", true);
}

void App::DismissNotice() {
    if (!m_notices.empty()) m_notices.pop_back();
    if (m_notices.empty()) ToggleWindow("notices", false);
}

const Texture& App::EnvironmentThumbnail(const std::string& key) {
    auto it = m_envThumbnails.find(key);
    if (it != m_envThumbnails.end()) return it->second;
    Texture& t = m_envThumbnails[key];
    std::vector<uint8_t> bytes;
    if (m_assets.Read(("ui/env-" + key + ".png").c_str(), bytes)) LoadImageTexture(bytes, 4, true, true, t);
    return t;
}

void App::RunAction(const std::string& action) {
    if (action == "vr.retry") {
        LaunchSteamVr();
    } else if (action == "vr.restartSteamVR") {
        m_client.SendControl({{"type", action}});
        EnterMode(Mode::LaunchingVr);
    } else if (!action.empty()) {
        m_client.SendControl({{"type", action}});
    }
}

void App::RequestGames() {
    m_gamesRequestedAt = m_time;
    m_gamesAnswered = false;
    m_client.SendControl({{"type", rc::msg::kListGames}});
}

void App::RequestGameImage(GameInfo& g) {
    if (g.imageRequested || !g.hasImage) return;
    g.imageRequested = true;
    m_client.SendControl({{"type", "games.image"}, {"id", g.id}});
}

std::string App::VrStateText() const {
    if (m_vrState == "launching") return m_vrProducer == "openxr" ? Tr("vrState.startingGame") : Tr("vrState.startingSteamvr");
    if (m_vrState == "running") return m_vrApp.empty() ? Tr("vrState.running") : m_vrApp;
    if (m_vrState == "error") return Tr("vrState.error");
    return Tr("vrState.ready");
}

bool App::IsVisible(const std::string& id) {
    Window* w = m_windows.Find(id);
    return w && w->visible;
}

void App::ToggleWindow(const std::string& id, bool show) {
    Window* w = m_windows.Find(id);
    if (!w) return;
    if (show && !w->visible) {
        // Re-open in front of the user if it's far behind them.
        const Vec3 toWin = Normalize(w->pose.position - m_head.position);
        // Transient panels always open where the user is looking; the dock, stats and
        // overlays keep their own slot so they never stack on top of each other.
        const bool transient = id == "home" || id == "keyboard" || id == "vr-overlay" || id == "notices" || id == "launch";
        if (transient || !m_placed.count(id) || Dot(toWin, m_head.Forward()) < 0.3f) {
            if (id == "keyboard")
                w->pose = WindowManager::PlaceInFront(m_head, 0.75f, 0, -0.13f, 0);
            else if (id == "dock")
                w->pose = WindowManager::PlaceInFront(m_head, 0.9f, 0, -0.50f, 0);
            else if (id == "stats")
                w->pose = WindowManager::PlaceInFront(m_head, 1.1f, 38.f, 0.18f, 0);
            else if (id == "notices")
                w->pose = WindowManager::PlaceInFront(m_head, 1.0f, 0, 0.40f, 0);  // above the main panel, not over it
            else if (id == "launch")
                w->pose = WindowManager::PlaceInFront(m_head, 1.0f, 0, -0.18f, 0);
            else if (id == "vr-overlay")
                w->pose = WindowManager::PlaceInFront(m_head, 0.95f, 0, -0.12f, 0);
            else
                w->pose = WindowManager::PlaceInFront(m_head, 1.3f, 0, -0.05f, 0);
            m_placed.insert(id);
            w->appear = 0;
        }
    }
    w->visible = show;
    // In VR the controllers go to RinCast instead of the game while it's open, however it closes.
    if (id == "vr-overlay") m_vrOverlay = show && m_mode == Mode::Vr;
}

void App::ShowMonitors(bool show) {
    m_monitorsVisible = show;
    for (auto& [id, ds] : m_desktopStreams)
        if (Window* w = m_windows.Find("monitor-" + std::to_string(id))) w->visible = show && m_mode == Mode::Desktop;
}

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

void App::EnterMode(Mode m) {
    if (m == m_mode) return;
    RC_LOGI("Mode %d -> %d", int(m_mode), int(m));
    const Mode prev = m_mode;
    m_mode = m;
    m_modeTime = 0;
    switch (m) {
        case Mode::Home:
            for (auto& w : m_windows.All()) w->visible = false;
            ToggleWindow("home", true);
            m_warp = 0;
            m_arrival = 0;
            break;
        case Mode::Connecting:
            ToggleWindow("home", true);
            break;
        case Mode::Desktop:
            ToggleWindow("home", false);
            ToggleWindow("launch", false);
            ToggleWindow("vr-overlay", false);
            ToggleWindow("dock", true);
            if (m_settings.showStats) ToggleWindow("stats", true);
            m_arrival = 0;
            if (prev == Mode::Vr || prev == Mode::LaunchingVr) {
                m_client.SendControl({{"type", rc::msg::kStartDesktop}});
            }
            ShowMonitors(m_monitorsVisible);
            break;
        case Mode::LaunchingVr:
            // Everything floats away; hyperspace takes over.
            for (auto& w : m_windows.All())
                if (w->id != "notices") w->visible = false;
            m_tunnelOrientation = Quat::Yaw(std::atan2(-m_head.Forward().x, -m_head.Forward().z));
            m_warpSpeed = 0.15f;
            m_arrival = 0;
            // Wait for a new picture, not the one from when VR was left.
            m_vrHasFrame = false;
            m_motionMoves = false;
            m_vrNoPicture = 0;
            // The desktop isn't shown in VR: stop its streams and free their decoders before the
            // VR decoder starts. A Quest 2 doesn't have the decoding hardware for both at once, and
            // the bandwidth is the game's. Leaving VR starts the desktop again.
            m_client.SendControl({{"type", rc::msg::kStopDesktop}});
            {
                std::lock_guard lock(m_decoderMutex);
                for (const auto& [id, d] : m_desktopStreams) m_decoders.erase(id);
            }
            m_client.SendControl({{"type", rc::msg::kStartVr}});
            ToggleWindow("launch", true);
            break;
        case Mode::Vr:
            for (auto& w : m_windows.All()) w->visible = false;
            // Free bandwidth for the game.
            m_client.SendControl({{"type", rc::msg::kStopDesktop}});
            if (m_settings.vrMenuHints < 3) {
                m_settings.vrMenuHints++;
                SaveSettings();
                PostNotice(Tr("notice.menuHint"), "info");
            }
            break;
    }
}

// Developer hooks for testing without controllers (e.g. in an emulator):
//   adb shell setprop debug.rincast.ui home,keyboard       windows to show (others are hidden)
//   adb shell setprop debug.rincast.tab 2                  page of the main panel
//   adb shell setprop debug.rincast.pc 10.0.2.2            extra PC address to probe
// On Linux the same come from the environment: DEBUG_RINCAST_PC, DEBUG_RINCAST_TAB, DEBUG_RINCAST_UI.
static void DebugProperty(const char* name, char (&value)[92]) {
#ifdef __ANDROID__
    __system_property_get(name, value);
#else
    std::string env = name;
    for (char& c : env) c = c == '.' ? '_' : char(std::toupper(static_cast<unsigned char>(c)));
    const char* v = std::getenv(env.c_str());
    std::snprintf(value, sizeof(value), "%s", v ? v : "");
#endif
}

void App::PollDebugProperties() {
    if (m_time - m_lastDebugPoll < 1.f) return;
    m_lastDebugPoll = m_time;
    char value[92] = {};  // PROP_VALUE_MAX
    DebugProperty("debug.rincast.pc", value);
    if (value[0] && m_debugPc != value) {
        m_debugPc = value;
        rc::NetAddress a;
        if (rc::NetAddress::Parse(m_debugPc, rc::kDiscoveryPort, a)) m_discovery.AddProbeTarget(a);
    }
    value[0] = 0;
    DebugProperty("debug.rincast.tab", value);
    if (value[0] && m_debugTab != value) {
        m_debugTab = value;
        screens::SetPage(std::atoi(value));
    }
    value[0] = 0;
    DebugProperty("debug.rincast.ui", value);
    if (value[0] && m_debugUi != value) {
        m_debugUi = value;
        const std::string list = "," + m_debugUi + ",";
        for (const char* id : {"home", "dock", "keyboard", "stats", "vr-overlay"})
            ToggleWindow(id, list.find(std::string(",") + id + ",") != std::string::npos);
        RC_LOGI("debug.rincast.ui = %s", m_debugUi.c_str());
    }
}

void App::UpdateMode(float dt) {
    m_modeTime += dt;
    m_discovery.SetSearching(m_mode == Mode::Home);
    PollDebugProperties();
    UpdateUsbNetwork();
    // Until a PC is connected, and not while it's being reconnected to after a drop: the
    // music would only start to stop again.
    m_music.Update(m_settings.music && (m_mode == Mode::Home || m_mode == Mode::Connecting) && m_xr.Visible() &&
                   !Reconnecting());
    const auto clientState = m_client.GetState();
    switch (m_mode) {
        case Mode::Home:
            ReconnectTick();
            break;
        case Mode::Connecting:
            if (clientState == StreamClient::State::Connected) {
                // Introduce ourselves, then show the desktop.
                json info = HelloJson(m_xr, m_settings, g_haveFrame ? &g_lastFrame : nullptr);
                info["type"] = rc::msg::kClientInfo;
                m_client.SendControl(info);
                Platform::SetStreamingLocks(true);
                if (m_settings.streamAudio) m_audio.Start();
                m_audio.SetVolume(m_settings.volume);
                m_mic.SetGain(m_settings.micVolume);
                if (m_settings.micPassthrough)
                    m_mic.Start([this](const uint8_t* d, size_t n) { m_client.SendAudio(d, n); });
                m_client.SendControl({{"type", rc::msg::kStartDesktop}});
                m_problem.clear();
                m_retryUntil = 0;
                m_retryAttempt = 0;
                if (m_settings.uiSounds) Platform::PlaySound(Platform::Sound::Connected);
                EnterMode(Mode::Desktop);
            } else if (clientState == StreamClient::State::Failed) {
                m_problem = m_client.StatusText();
                if (m_settings.uiSounds && !Reconnecting()) Platform::PlaySound(Platform::Sound::Problem);
                m_client.Disconnect();
                EnterMode(Mode::Home);
            }
            break;
        case Mode::Desktop:
        case Mode::LaunchingVr:
        case Mode::Vr:
            if (clientState == StreamClient::State::Failed || clientState == StreamClient::State::Idle) {
                const std::string status = m_client.StatusText();
                DisconnectNow();
                // Anything but the PC ending it on purpose is worth another try.
                if (status != "The PC ended the session." && !status.empty() && m_haveLast) {
                    m_problem = status;
                    m_retryUntil = m_time + 20.f;
                    m_retryAttempt = 0;
                    m_lastRetry = m_time - 3.f;  // first try almost at once
                    if (m_settings.uiSounds) Platform::PlaySound(Platform::Sound::Problem);
                } else {
                    PostNotice(status.empty() ? Tr("notice.disconnected") : status, "warning");
                }
                return;
            }
            break;
    }

    // Hyperspace choreography.
    if (m_mode == Mode::LaunchingVr) {
        m_warp = Damp(m_warp, 1.f, 3.5f, dt);
        m_warpSpeed = std::min(1.f, m_warpSpeed + dt * 0.45f);  // accelerate into the jump
        if (m_vrState == "running" && m_vrHasFrame) EnterMode(Mode::Vr);
        // The PC sends the game, but no picture comes out of the decoder: say so, instead of
        // blaming SteamVR when the wait runs out.
        if (m_vrState == "running" && !m_vrHasFrame) m_vrNoPicture += dt;
        std::shared_ptr<VideoDecoder> vrDecoder;
        {
            std::lock_guard lock(m_decoderMutex);
            if (auto it = m_decoders.find(rc::kVrStreamId); it != m_decoders.end()) vrDecoder = it->second;
        }
        if (m_mode == Mode::LaunchingVr && m_vrNoPicture > 0 && (m_vrNoPicture > 15.f || (m_vrNoPicture > 3.f && vrDecoder && vrDecoder->Failed()))) {
            RC_LOGW("VR: the PC has been sending for %.0f s, but no picture could be shown. Decoder: %s, started %d, "
                    "failed %d, %u frames decoded, %dx%d",
                    m_vrNoPicture, vrDecoder ? "present" : "missing", vrDecoder && vrDecoder->Started(),
                    vrDecoder && vrDecoder->Failed(), vrDecoder ? vrDecoder->DecodedFrames() : 0u, m_vrEyeWidth * 2,
                    m_vrEyeHeight);
            PostNotice(Tr("notice.cantShow"), "error", "vr.retry");
            EnterMode(Mode::Desktop);
        } else if (m_mode == Mode::LaunchingVr && (m_vrState == "error" || m_modeTime > 100.f)) {
            PostNotice(m_vrState == "error" ? Tr("notice.vrError") : Tr("notice.vrTimeout"), "error", "vr.retry");
            EnterMode(Mode::Desktop);
        }
    } else if (m_mode == Mode::Vr) {
        // Arrival: a final burst of speed, then fade into the stream.
        m_arrival = std::min(1.f, m_arrival + dt / 0.9f);
        m_warpSpeed = m_arrival < 0.35f ? 1.f + m_arrival * 8.f : Damp(m_warpSpeed, 0.f, 4.f, dt);
        m_warp = 1.f - SmoothStep(0.25f, 1.f, m_arrival);
        // The game closed: back to the desktop after a short grace period.
        if (m_vrState != "running" && m_modeTime > 3.f) {
            static float idle = 0;
            idle += dt;
            if (idle > 2.f) {
                idle = 0;
                ExitVr();
            }
        }
    } else {
        m_warp = Damp(m_warp, 0.f, 6.f, dt);
        m_warpSpeed = Damp(m_warpSpeed, 0.f, 3.f, dt);
    }
    m_warpPhase += m_warpSpeed * dt;

    for (auto& n : m_notices) n.age += dt;
    while (!m_notices.empty() && m_notices.front().age > m_notices.front().life) m_notices.pop_front();
    if (m_notices.empty()) ToggleWindow("notices", false);
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

void App::ProcessNetwork() {
    std::vector<json> queue;
    {
        std::lock_guard lock(m_netMutex);
        queue.swap(m_controlQueue);
    }
    for (const auto& m : queue) HandleControl(m);
}

static bool DecodeImage(const std::string& b64, Texture& out) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> bytes;
    uint32_t buf = 0;
    int bits = 0;
    for (char c : b64) {
        const size_t p = chars.find(c);
        if (p == std::string::npos) continue;
        buf = (buf << 6) | uint32_t(p);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(uint8_t(buf >> bits));
        }
    }
    return !bytes.empty() && LoadImageTexture(bytes, 4, true, true, out);
}

void App::HandleControl(const json& m) {
    const std::string type = m.value("type", "");
    if (type == "_cloudTokens") {
        m_settings.cloudRefreshToken = m.value("refresh", "");
        m_settings.cloudUserName = m.value("name", "");
        SaveSettings();
    } else if (type == "_connectFailed") {
        m_problem = m.value("error", Tr("computers.couldntConnect"));
        EnterMode(Mode::Home);
    } else if (type == "_cloudTarget") {
        ConnectTarget t;
        t.name = m.value("name", "");
        t.cloudSession = m.value("session", "");
        t.connectSecret = m.value("secret", "");
        t.sessionToken = m.value("token", "");
        auto endpoints = [](const json& list, std::vector<rc::NetAddress>& out) {
            for (const auto& e : list) {
                rc::NetAddress a;
                if (e.is_string() && rc::NetAddress::ParseEndpoint(e.get<std::string>(), a)) out.push_back(a);
            }
        };
        endpoints(m.value("rendezvous", json::array()), t.rendezvous);
        endpoints(m.value("locals", json::array()), t.pcLocalAddrs);
        m_client.Connect(t, HelloJson(m_xr, m_settings, g_haveFrame ? &g_lastFrame : nullptr));
    } else if (type == "_haptic") {
        m_input.Vibrate(m.value("hand", 0), m.value("amplitude", 0.f), std::max(0.02f, m.value("duration", 0.05f)),
                        m.value("frequency", 0.f) > 0 ? m.value("frequency", 0.f) : XR_FREQUENCY_UNSPECIFIED);
    } else if (type == rc::msg::kServerInfo) {
        m_serverInfo = m;
    } else if (type == rc::msg::kMonitors) {
        m_monitors.clear();
        for (const auto& mon : m.value("monitors", json::array()))
            m_monitors.push_back({mon.value("id", ""), mon.value("name", ""), mon.value("index", 0),
                                  mon.value("width", 0), mon.value("height", 0), mon.value("primary", false)});
    } else if (type == rc::msg::kStreamConfig) {
        if (m.value("kind", "") == "desktop")
            ConfigureDesktopStreams(m.value("streams", json::array()));
        else if (!m.value("streams", json::array()).empty())
            ConfigureVrStream(m["streams"][0]);
    } else if (type == rc::msg::kVrState) {
        m_vrState = m.value("state", "idle");
        m_vrProducer = m.value("producer", "");
        m_vrApp = m.value("app", "");
        if (m.contains("error")) PostNotice(m.value("error", ""), "error");
        // A VR app started from the PC (or the Games list): jump in.
        if ((m_vrState == "launching" || m_vrState == "running") && m_mode == Mode::Desktop) EnterMode(Mode::LaunchingVr);
    } else if (type == rc::msg::kGames) {
        m_gamesAnswered = true;
        std::vector<GameInfo> games;
        for (const auto& g : m.value("games", json::array())) {
            GameInfo gi;
            gi.id = g.value("id", "");
            gi.name = g.value("name", "");
            gi.hasImage = g.value("hasImage", false);
            for (auto& old : m_games)
                if (old.id == gi.id && old.image.id) {
                    gi.image = old.image;
                    gi.imageRequested = true;
                    old.image = {};
                }
            games.push_back(std::move(gi));
        }
        for (auto& old : m_games) old.image.Destroy();
        m_games = std::move(games);
    } else if (type == "games.image") {
        for (auto& g : m_games)
            if (g.id == m.value("id", "") && !g.image.id) DecodeImage(m.value("data", ""), g.image);
    } else if (type == rc::msg::kServerStats) {
        m_serverStats = m;
    } else if (type == rc::msg::kNotice) {
        PostNotice(m.value("text", ""), m.value("level", "info"), m.value("action", ""));
    }
}

void App::OnVideoFrame(rc::ReceivedFrame&& f) {
    std::lock_guard lock(m_decoderMutex);
    auto it = m_decoders.find(f.streamId);
    if (it == m_decoders.end() || !it->second) return;
    if (it->second->Codec() != f.codec) return;
    it->second->Submit(f.frameIndex, (f.flags & rc::kVideoKeyframe) != 0, (f.flags & rc::kVideoRecovery) != 0,
                       std::move(f.bitstream), std::move(f.metadata), f.completeNs);
    m_client.SetLastGoodFrame(f.streamId, it->second->LastGoodFrame());
    if (it->second->NeedsKeyframe()) m_client.RequestKeyframe(f.streamId);
}

void App::ConfigureDesktopStreams(const json& streams) {
    std::map<uint8_t, DesktopStreamInfo> next;
    for (const auto& s : streams) {
        DesktopStreamInfo d;
        d.streamId = s.value("streamId", uint8_t(1));
        d.monitorId = s.value("monitorId", "");
        d.name = s.value("name", "");
        d.width = s.value("width", 1920);
        d.height = s.value("height", 1080);
        d.codec = rc::VideoCodec(s.value("codec", 1));
        d.fps = s.value("fps", 60.f);
        next[d.streamId] = d;
    }
    // A new stream counts its frames from the start and can't continue the old one's picture,
    // so even with the same size the decoder and frame tracking have to start over too.
    for (auto& [id, d] : next) {
        auto dec = std::make_unique<VideoDecoder>();
        dec->Init(d.codec, d.width, d.height, d.fps);
        {
            std::lock_guard lock(m_decoderMutex);
            m_decoders[id] = std::move(dec);
        }
        m_client.ResetStream(id);
        if (m_monitorLayers.count(id)) m_xr.DestroySwapchain(m_monitorLayers[id]);
        m_monitorLayers.erase(id);
        Window* w = m_windows.Create("monitor-" + std::to_string(id), WindowKind::Video);
        w->title = d.name;
        w->aspect = float(d.height) / float(d.width);
        w->minWidthM = 0.4f;
        w->maxWidthM = 6.f;
        w->videoReady = false;
    }
    for (auto& [id, d] : m_desktopStreams) {
        if (!next.count(id)) {
            m_windows.Remove("monitor-" + std::to_string(id));
            std::lock_guard lock(m_decoderMutex);
            m_decoders.erase(id);
            if (m_monitorLayers.count(id)) m_xr.DestroySwapchain(m_monitorLayers[id]);
            m_monitorLayers.erase(id);
        }
    }
    m_desktopStreams = next;
    LayoutMonitors(!m_monitorsLaidOut);
    m_monitorsLaidOut = true;
    ShowMonitors(m_monitorsVisible);
}

void App::ConfigureVrStream(const json& s) {
    const int eyeW = s.value("eyeWidth", 0), eyeH = s.value("eyeHeight", 0);
    const auto codec = rc::VideoCodec(s.value("codec", 1));
    if (eyeW <= 0 || eyeH <= 0) return;
    m_vrEyeWidth = eyeW;
    m_vrEyeHeight = eyeH;
    m_vrCodec = codec;
    // Foveated: the frames come smaller, and are stretched back to eyeWidth x eyeHeight.
    int codedW = eyeW, codedH = eyeH;
    const json f = s.value("foveation", json());
    m_vrFoveated = false;
    if (f.is_object()) {
        const int fw = f.value("eyeWidth", 0), fh = f.value("eyeHeight", 0);
        const float size = f.value("size", 0.f);
        if (fw > 0 && fh > 0 && fw <= eyeW && fh <= eyeH && size > 0.05f && size < 1.f) {
            codedW = fw;
            codedH = fh;
            m_vrFoveated = true;
            m_vrFoveation = {};
            m_vrFoveation.size = size;
            m_vrFoveation.density[0] = (float(fw) / eyeW - size) / (1 - size);
            m_vrFoveation.density[1] = (float(fh) / eyeH - size) / (1 - size);
            for (int eye = 0; eye < 2; eye++) m_vrFoveation.start[eye][0] = m_vrFoveation.start[eye][1] = (1 - size) / 2;
            RC_LOGI("VR stream: foveated, %dx%d per eye shown at %dx%d", fw, fh, eyeW, eyeH);
        }
    }
    auto dec = std::make_unique<VideoDecoder>();
    dec->Init(codec, codedW * 2, codedH, s.value("fps", 90.f));
    {
        std::lock_guard lock(m_decoderMutex);
        m_decoders[rc::kVrStreamId] = std::move(dec);
    }
    m_client.ResetStream(rc::kVrStreamId);
    CreateVrSwapchain();
    m_vrHasFrame = false;
    m_motionMoves = false;
}

void App::CreateVrSwapchain() {
    // Upscaling only has a job while the stream is below the headset's own resolution.
    const float scale = std::min(2.f, float(m_xr.RecommendedWidth()) / float(std::max(1, m_vrEyeWidth)));
    m_vrUpscaleSetting = m_settings.upscaling;
    m_vrUpscaled = m_settings.upscaling && scale > 1.05f && m_scene.UpscalerReady();
    const float s = m_vrUpscaled ? scale : 1.f;
    m_xr.DestroySwapchain(m_vrSwapchain);
    m_xr.CreateSwapchain(int(m_vrEyeWidth * s + 0.5f) * 2, int(m_vrEyeHeight * s + 0.5f), GL_SRGB8_ALPHA8, 1, false, m_vrSwapchain);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void App::LayoutHome(bool force) {
    Window* home = m_windows.Find("home");
    if (home && (force || home->appear < 0.01f)) home->pose = WindowManager::PlaceInFront(m_head, 1.3f, 0, -0.05f);
}

void App::LayoutMonitors(bool force) {
    if (!force || m_desktopStreams.empty()) return;
    // Arrange monitors on an arc around the user, primary in the middle.
    std::vector<uint8_t> order;
    for (auto& [id, d] : m_desktopStreams) order.push_back(id);
    const float distance = m_settings.screenDistance;
    const float width = 1.45f * distance / 1.35f;
    const float gapM = 0.06f;
    const float angleStep = (width + gapM) / distance;  // radians per monitor along the arc
    const float start = -angleStep * (float(order.size()) - 1) / 2;
    for (size_t i = 0; i < order.size(); i++) {
        Window* w = m_windows.Find("monitor-" + std::to_string(order[i]));
        if (!w) continue;
        w->widthM = width;
        w->curveRadius = m_settings.curvature > 0.02f ? distance / std::max(0.05f, m_settings.curvature) : 0.f;
        w->layerBacked = m_settings.sharpLayers && (w->curveRadius == 0 || m_xr.CylinderLayers());
        w->pose = WindowManager::PlaceInFront(m_head, distance, (start + angleStep * float(i)) * 180.f / kPi, 0.05f);
    }
    if (Window* dock = m_windows.Find("dock")) {
        dock->pose = WindowManager::PlaceInFront(m_head, 0.9f, 0, -0.50f, 0);
        m_placed.insert("dock");
    }
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

// The headset's microphone while RinCast is in the background (the Quest's home, another app,
// the headset asleep). The connection to the PC lives on its own thread, so before, a session
// kept recording there until it ended. Android lets one app at a time record well: a Quest Pro
// gave the next app that opened the microphone, Virtual Desktop say, a much quieter one until
// the headset restarted. Let go of it, and take it again when RinCast is back in front.
void App::ReleaseMicInBackground() {
    if (!m_mic.Running()) return;
    m_mic.Stop();
    m_micReleased = true;
    RC_LOGI("Microphone released while RinCast is in the background");
}

void App::Frame() {
    const int64_t now = rc::NowNs();
    if (m_micReleased) {
        m_micReleased = false;
        if (m_settings.micPassthrough && m_client.GetState() == StreamClient::State::Connected)
            m_mic.Start([this](const uint8_t* d, size_t n) { m_client.SendAudio(d, n); });
    }
    const float dt = std::clamp(float(now - m_lastFrameNs) / 1e9f, 0.001f, 0.1f);
    m_lastFrameNs = now;
    m_time += dt;

    FrameState frame;
    // Lighthouse trackers calibrated against the headset (Space Calibrator) need a space that
    // stays put; the local floor moves whenever the headset recenters.
    m_xr.SetPreferStage(m_headset->PreferStageSpace() || m_settings.fixedPlaySpace);
    if (!m_xr.BeginFrame(frame)) return;
    g_lastFrame = frame;
    g_haveFrame = true;
    const bool firstFrame = m_head.position.y == 0 && m_head.position.x == 0;
    m_head = frame.head;
    if (firstFrame) LayoutHome(true);
    if (m_xr.SpaceGeneration() != m_spaceGeneration) {
        m_spaceGeneration = m_xr.SpaceGeneration();
        Recenter();
    }

    m_input.Update(frame.predictedDisplayTime);
    ProcessNetwork();
    if (m_problem != m_spokenProblem) {
        m_spokenProblem = m_problem;
        if (m_settings.readAloud && !m_problem.empty()) Platform::Speak("Connection problem. " + m_problem);
    }
    const bool streaming = m_client.GetState() == StreamClient::State::Connected;
    if (streaming != m_streamingPerformance) {
        m_streamingPerformance = streaming;
        m_xr.SetStreamingPerformance(streaming);
    }
    if (m_disconnectRequested) DisconnectNow();
    UpdateMode(dt);
    UpdateInput(frame, dt);
    if (m_client.GetState() == StreamClient::State::Connected) {
        UpdateTracking(frame);
    } else {
        // Neither is any use without a PC, and hand tracking runs slower with simultaneous on.
        m_headset->StopBody();
        m_input.SetSimultaneous(false);
    }
    m_vrLatched = false;
    LatchVideo(frame);

    // Environment / passthrough.
    const Environment env = EnvironmentFromKey(m_settings.environment);
    m_xr.SetPassthrough(env == Environment::Passthrough && m_mode != Mode::Vr);

    // During a game with nothing on top, drawing an empty eye layer over the stream would
    // only cost GPU time and heat. The performance overlay alone doesn't need it either: it
    // goes to the compositor as a layer of its own, redrawn when its numbers change. Drawing
    // both eye images every frame for it took enough GPU time that the headset started its
    // frames earlier, which added to the very latency it shows.
    Window* stats = m_windows.Find("stats");
    bool anyWindow = false;
    for (const auto& w : m_windows.All())
        if (w.get() != stats) anyWindow = anyWindow || w->visible || w->appear > 0.01f;
    const bool vrClear = m_mode == Mode::Vr && m_vrHasFrame && !m_vrOverlay && m_warp <= 0.001f && !anyWindow;
    const bool statsShown = stats && (stats->visible || stats->appear > 0.01f);
    const bool statsLayer = vrClear && statsShown && stats->visible && stats->appear > 0.98f;
    UpdateStatsLayer(stats, statsLayer, dt);
    const bool uiLayer = !(vrClear && (!statsShown || statsLayer));
    m_windows.RenderCanvases(dt, m_time);
    UpdateUiLayers();
    if (frame.shouldRender && uiLayer) RenderEyes(frame, dt);
    // Late latch: frames decoded while we rendered still make it into this frame's
    // compositor layers (VR stream, monitor layers) instead of waiting a whole frame.
    m_lateLatchNs = rc::NowNs();
    LatchVideo(frame);
    {
        const double lead = double(m_xr.ToMonotonicNs(frame.predictedDisplayTime) - m_lateLatchNs);
        if (lead > 0 && lead < 1e8) m_latchLeadNs = m_latchLeadNs > 0 ? m_latchLeadNs * 0.95 + lead * 0.05 : lead;
    }
    SteerVrTiming(frame);
    if (m_mode == Mode::Vr && m_settings.frameGeneration && !m_vrLatched) GenerateFrame(frame);
    BuildLayers(frame);
    const bool alpha = m_xr.PassthroughActive() || !m_underlays.empty();
    m_xr.EndFrame(frame, m_underlays, alpha, m_overlays, uiLayer);
}

void App::UpdateInput(const FrameState& frame, float dt) {
    const bool uiActive = m_mode != Mode::Vr || m_vrOverlay;
    for (int h = 0; h < 2; h++) {
        const HandInput& in = m_input.Hand(h);
        auto& r = m_rays[h];
        const bool was = r.triggerDown;
        r.valid = uiActive && in.active && in.aimValid;
        r.origin = in.aim.position;
        // Steady the pointer when it's nearly still (more so for tracked hands), without
        // lag when it really moves.
        {
            const Vec3 target = in.aim.Forward();
            Vec3& smooth = m_smoothDir[h];
            const Vec3 delta = target - smooth;
            const float moved = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
            const float follow = std::clamp((in.isHand ? 0.12f : 0.3f) + moved * (in.isHand ? 18.f : 40.f), 0.f, 1.f);
            smooth = r.valid ? smooth + delta * follow : target;
            const float len = std::sqrt(smooth.x * smooth.x + smooth.y * smooth.y + smooth.z * smooth.z);
            r.dir = len > 0.001f ? smooth * (1.f / len) : target;
        }
        r.triggerDown = was ? in.trigger > 0.35f : in.trigger > 0.65f;
        r.triggerPressed = r.triggerDown && !was;
        r.triggerReleased = !r.triggerDown && was;
        r.scroll = std::fabs(in.thumbstick.y) > 0.15f ? in.thumbstick.y * m_settings.scrollSpeed : 0.f;
        r.push = in.thumbstick.y;

        // Left menu: toggle the dock (desktop) / double-press opens the VR overlay. On the
        // desktop while SteamVR runs, a double-press opens the same small menu, with "Back to
        // VR" where it has "Back to desktop" in VR: one gesture and one menu both ways.
        if (in.menu && !m_handState[h].menu) {
            int64_t now = rc::NowNs();
            const bool twice = now - m_lastMenuPressNs < 450'000'000LL;
            if (m_mode == Mode::Vr) {
                if (twice) ToggleWindow("vr-overlay", !m_vrOverlay);
            } else if (m_mode == Mode::Desktop) {
                if (twice && VrRunning()) {
                    ToggleWindow("dock");  // undo the first press
                    ToggleWindow("vr-overlay", !IsVisible("vr-overlay"));
                    now = 0;  // a third press starts over
                } else {
                    ToggleWindow("dock");
                }
            } else {
                screens::ToggleMenu(*this);
            }
            m_lastMenuPressNs = now;
        }
        m_handState[h].menu = in.menu;
    }
    // One-handed route to the same menu for the right hand: hold A and B.
    {
        const HandInput& right = m_input.Hand(1);
        const bool held = right.active && right.primary && right.secondary;
        const float before = m_abHeld;
        m_abHeld = held ? m_abHeld + dt : 0.f;
        if (before < 0.6f && m_abHeld >= 0.6f) {
            if (m_mode == Mode::Vr) {
                ToggleWindow("vr-overlay", !m_vrOverlay);
            } else if (m_mode == Mode::Desktop) {
                ToggleWindow("dock");
            } else {
                screens::ToggleMenu(*this);
            }
        }
    }

    m_windows.Update(m_rays, m_head, dt, m_time, m_hits);
    if (m_settings.haptics && m_windows.ConsumeHaptic()) m_input.Vibrate(m_windows.HapticHand(), 0.15f, 0.015f);
    if (m_windows.UiContext().ConsumeHoverChange() && m_settings.haptics) m_input.Vibrate(m_windows.HapticHand(), 0.06f, 0.008f);
    if (const std::string speech = m_windows.UiContext().ConsumeSpeech(); m_settings.readAloud && !speech.empty()) Platform::Speak(speech);
    const bool uiClicked = m_windows.UiContext().ConsumeClick();
    if (uiClicked && m_settings.uiSounds) Platform::PlaySound(Platform::Sound::Click);
    if (m_settings.haptics && uiClicked)
        m_input.Vibrate(m_windows.HapticHand(), 0.35f, 0.025f);

    // ---- PC mouse from laser over a monitor ----
    if (m_mode != Mode::Desktop || m_windows.Dragging()) return;
    int mouseHand = -1;
    for (int h = 1; h >= 0; h--) {
        const WindowHit& hit = m_hits[h];
        if (hit.window && hit.window->kind == WindowKind::Video && hit.zone == HitZone::Content) {
            if (mouseHand < 0 || m_rays[h].triggerDown) mouseHand = h;
        }
    }
    if (mouseHand < 0) {
        // Release anything held when the ray leaves the monitors.
        for (int h = 0; h < 2; h++) {
            if (m_handState[h].trigger) m_client.SendControl({{"type", rc::msg::kMouseButton}, {"button", 0}, {"down", false}});
            m_handState[h].trigger = false;
        }
        return;
    }
    const WindowHit& hit = m_hits[mouseHand];
    const int streamId = std::stoi(hit.window->id.substr(8));
    rc::PointerPayload p{};
    p.monitorStreamId = uint8_t(streamId);
    p.visible = 1;
    p.u = hit.uv.x;
    p.v = hit.uv.y;
    p.inputSeq = ++m_pointerSeq;
    m_client.SendPointer(p);

    const HandInput& in = m_input.Hand(mouseHand);
    auto& st = m_handState[mouseHand];
    auto button = [&](bool now, bool& prev, int index) {
        if (now != prev) {
            m_client.SendControl({{"type", rc::msg::kMouseButton}, {"button", index}, {"down", now}});
            if (now && m_settings.haptics) m_input.Vibrate(mouseHand, 0.25f, 0.02f);
            prev = now;
        }
    };
    if (in.isHand) {
        const bool down = m_rays[mouseHand].triggerDown;
        using Pinch = HandState::Pinch;
        if (down && !st.trigger) {
            st.pinch = Pinch::Undecided;
            st.pinchTime = 0;
            st.pinchStartV = st.pinchLastV = hit.uv.y;
        }
        const auto click = [&](int index) {
            m_client.SendControl({{"type", rc::msg::kMouseButton}, {"button", index}, {"down", true}});
            m_client.SendControl({{"type", rc::msg::kMouseButton}, {"button", index}, {"down", false}});
            if (m_settings.haptics) m_input.Vibrate(mouseHand, 0.25f, 0.02f);
        };
        if (down) {
            st.pinchTime += dt;
            if (st.pinch == Pinch::Undecided && std::fabs(hit.uv.y - st.pinchStartV) > 0.02f) st.pinch = Pinch::Scrolling;
            if (st.pinch == Pinch::Scrolling) {
                // The page follows the hand, as on a touch screen.
                m_client.SendControl({{"type", rc::msg::kMouseWheel}, {"dx", 0.f}, {"dy", (hit.uv.y - st.pinchLastV) * 24.f * m_settings.scrollSpeed}});
                st.pinchLastV = hit.uv.y;
            } else if (st.pinch == Pinch::Undecided && st.pinchTime > 0.6f) {
                click(1);
                st.pinch = Pinch::Done;
            }
        } else if (st.trigger && st.pinch == Pinch::Undecided) {
            click(0);
        }
        st.trigger = down;
        (void)frame;
        return;
    }
    button(m_rays[mouseHand].triggerDown, st.trigger, 0);
    // B or Y right-clicks, a moment later: holding A and B opens the menu instead.
    if (in.secondary && in.primary) st.chord = true;
    if (in.secondary) {
        st.secondaryHeld += dt;
    } else if (st.secondaryHeld > 0 && !st.secondary && !st.chord) {
        // A quick tap, over before the wait ended: still a click.
        button(true, st.secondary, 1);
    }
    button(in.secondary && !st.chord && (st.secondary || st.secondaryHeld > 0.1f), st.secondary, 1);
    if (!in.secondary) {
        st.secondaryHeld = 0;
        if (!in.primary) st.chord = false;
    }
    button(in.thumbstickClick, st.stickClick, 2);
    // Smooth scrolling: thumbstick -> wheel notches.
    const float sy = std::fabs(in.thumbstick.y) > 0.2f ? in.thumbstick.y : 0.f;
    const float sx = std::fabs(in.thumbstick.x) > 0.5f ? in.thumbstick.x : 0.f;
    if (sy != 0 || sx != 0)
        m_client.SendControl({{"type", rc::msg::kMouseWheel},
                              {"dx", sx * dt * 8.f * m_settings.scrollSpeed},
                              {"dy", sy * dt * 12.f * m_settings.scrollSpeed}});
    (void)frame;
}

void App::UpdateTracking(const FrameState& frame) {
    // Predict to when the PC's frame for this pose will reach the display.
    const XrTime target = frame.predictedDisplayTime + XrTime(m_pipelineLatencyMs * 1e6);
    rc::TrackingPayload t{};
    t.sampleTimeNs = uint64_t(rc::NowNs());
    t.targetDisplayTimeNs = uint64_t(m_xr.ToMonotonicNs(target));
    Pose head;
    Vec3 lin, ang;
    if (m_xr.LocateSpace(m_xr.ViewSpace(), target, head, &lin, &ang)) {
        t.flags |= rc::kTrackHmdValid;
        t.hmd.orientation[0] = head.orientation.x;
        t.hmd.orientation[1] = head.orientation.y;
        t.hmd.orientation[2] = head.orientation.z;
        t.hmd.orientation[3] = head.orientation.w;
        t.hmd.position[0] = head.position.x;
        t.hmd.position[1] = head.position.y;
        t.hmd.position[2] = head.position.z;
        std::memcpy(t.hmdLinearVelocity, &lin, sizeof(float) * 3);
        std::memcpy(t.hmdAngularVelocity, &ang, sizeof(float) * 3);
    }
    HandInput predicted[2];
    m_input.Locate(target, predicted);
    for (int h = 0; h < 2; h++) {
        const HandInput& in = m_input.Hand(h);
        auto& c = t.controllers[h];
        if (!in.active) continue;
        c.flags = rc::kCtrlConnected;
        rc::Pose joints[rc::kHandJointCount];
        const auto sendBones = [&] {
            rc::HandSkeletonPayload bones{};
            bones.hand = uint8_t(h);
            rc::HandJointsToBones(h, joints, bones.bones);
            m_client.SendHandSkeleton(bones);
        };
        const bool playingWithHand = in.isHand && m_settings.handsInGames && m_mode == Mode::Vr && m_input.LocateHandJoints(h, target, joints);
        // Only the fingers matter here: the bones are placed relative to the palm, so a slower
        // hand can't shake against the controller. The hand runs at about 30 Hz in this mode,
        // so don't ask it to predict as far ahead as the controller.
        if (in.held && m_input.LocateHandJoints(h, frame.predictedDisplayTime, joints)) sendBones();
        if (playingWithHand) {
            c.flags |= rc::kCtrlPoseValid;
            c.pose = rc::HandDevicePose(h, joints[0]);
            sendBones();
        } else if (predicted[h].gripValid) {
            c.flags |= rc::kCtrlPoseValid;
            const Pose& g = predicted[h].grip;
            c.pose.orientation[0] = g.orientation.x;
            c.pose.orientation[1] = g.orientation.y;
            c.pose.orientation[2] = g.orientation.z;
            c.pose.orientation[3] = g.orientation.w;
            c.pose.position[0] = g.position.x;
            c.pose.position[1] = g.position.y;
            c.pose.position[2] = g.position.z;
            std::memcpy(c.linearVelocity, &predicted[h].gripLinearVelocity, sizeof(float) * 3);
            std::memcpy(c.angularVelocity, &predicted[h].gripAngularVelocity, sizeof(float) * 3);
        }
        if (in.isHand) c.flags |= rc::kCtrlHandTracked;
        // Don't leak clicks on the desktop or the overlay into a game still running behind them.
        const bool gameInput = m_mode == Mode::Vr && !m_vrOverlay;
        if (gameInput) {
            uint32_t b = 0;
            if (h == 0) {
                if (in.primary) b |= rc::kBtnX;
                if (in.secondary) b |= rc::kBtnY;
                if (in.touchPrimary) b |= rc::kTouchX;
                if (in.touchSecondary) b |= rc::kTouchY;
                if (in.menu) b |= rc::kBtnMenu;
            } else {
                const bool menuChord = in.primary && in.secondary;  // hold both: RinCast's menu
                if (in.primary && !menuChord) b |= rc::kBtnA;
                if (in.secondary && !menuChord) b |= rc::kBtnB;
                if (in.touchPrimary) b |= rc::kTouchA;
                if (in.touchSecondary) b |= rc::kTouchB;
            }
            if (in.thumbstickClick) b |= rc::kBtnThumbstick;
            if (in.touchThumbstick) b |= rc::kTouchThumbstick;
            if (in.touchTrigger) b |= rc::kTouchTrigger;
            if (in.touchThumbrest) b |= rc::kTouchThumbrest;
            if (in.trigger > 0.85f) b |= rc::kBtnTrigger;
            if (in.squeeze > 0.85f) b |= rc::kBtnGrip;
            c.buttons = b;
            c.trigger = in.trigger;
            c.grip = in.squeeze;
            c.thumbstick[0] = in.thumbstick.x;
            c.thumbstick[1] = in.thumbstick.y;
            if (playingWithHand) {
                // A fist closes the fingers that would otherwise read as pinches.
                c.grip = rc::HandGrip(joints);
                if (c.grip > 0.85f) c.buttons |= rc::kBtnGrip;
                if (c.grip < 0.5f) {
                    if (in.pinchMiddle > 0.85f) c.buttons |= h == 0 ? rc::kBtnX : rc::kBtnA;
                    if (in.pinchRing > 0.85f) c.buttons |= h == 0 ? rc::kBtnY : rc::kBtnB;
                }
            }
        }
    }
    // Where the eyes look, for foveated encoding. Now rather than predicted: eyes jump, and the
    // PC keeps every pixel of a square around it.
    const bool foveate = m_settings.gazeFoveation && m_mode == Mode::Vr && GazeSupported();
    rc::GazePayload gaze{};
    if (!foveate) {
        m_headset->StopGaze();
    } else if (!m_gazePermissionAsked) {
        m_gazePermissionAsked = true;
        Platform::RequestPermissions(m_headset->GazePermissions());
    }
    m_client.SendTracking(t, foveate && ReadGaze(frame.predictedDisplayTime, gaze) ? &gaze : nullptr);
    m_trackingSent[t.targetDisplayTimeNs] = int64_t(t.sampleTimeNs);
    while (m_trackingSent.size() > 512) m_trackingSent.erase(m_trackingSent.begin());
    UpdateBody(frame);
    UpdateFace(frame);
}

bool App::ReadGaze(XrTime time, rc::GazePayload& out) {
    if (m_headset->GazeSupported(m_xr)) return m_headset->ReadGaze(m_xr, time, out);
    float d[3];
    if (!m_input.LocateGaze(time, d)) return false;
    // One gaze for both eyes: plenty to place the sharp square.
    out.flags = rc::kGazeLeftValid | rc::kGazeRightValid;
    for (int eye = 0; eye < 2; eye++) std::memcpy(out.gaze[eye], d, sizeof(d));
    return true;
}

void App::UpdateBody(const FrameState& frame) {
    const bool body = m_settings.bodyTracking && m_mode == Mode::Vr && m_headset->BodySupported(m_xr);
    // Stop body tracking before simultaneous tracking starts, and the other way round.
    if (!body) m_headset->StopBody();
    m_input.SetSimultaneous(m_settings.fingersWithControllers && m_mode == Mode::Vr &&
                            !(m_settings.bodyTracking && m_headset->BodyExcludesFingers()));
    if (!body) return;
    if (!m_bodyPermissionAsked) {
        m_bodyPermissionAsked = true;
        Platform::RequestPermissions(m_headset->BodyPermissions());
    }
    rc::BodyTrackingPayload b{};
    // For the current frame rather than the stream's prediction: body estimates are the
    // headset's own best guess already, and fail when asked far ahead.
    if (m_headset->LocateBody(m_xr, frame.predictedDisplayTime, b)) m_client.SendBody(b);
}

void App::UpdateFace(const FrameState& frame) {
    const bool face = m_settings.faceTracking && m_mode == Mode::Vr && m_headset->FaceSupported(m_xr);
    if (!face) {
        m_headset->StopFace();
        return;
    }
    if (!m_facePermissionAsked) {
        m_facePermissionAsked = true;
        Platform::RequestPermissions(m_headset->FacePermissions());
    }
    rc::FaceTrackingPayload f{};
    if (m_headset->ReadFace(m_xr, frame.predictedDisplayTime, f)) m_client.SendFace(f);
}

void App::ReadMotion(const std::vector<uint8_t>& metadata) {
    m_motionMoves = false;
    // After the frame's metadata, and its foveation's.
    const size_t at = sizeof(rc::VrFrameMetadata) + (m_vrFoveated ? sizeof(rc::VrFoveationMetadata) : 0);
    if (!m_settings.frameGeneration || metadata.size() < at + sizeof(rc::MotionHeader)) return;
    rc::MotionHeader h;
    std::memcpy(&h, metadata.data() + at, sizeof(h));
    const size_t head = at + sizeof(h);
    if (!h.gridWidth || !h.gridHeight || !h.blockSize || m_vrMeta.trackingTimestampNs <= h.previousTimestampNs) return;
    if (!rc::DecodeMotionRuns(metadata.data() + head, metadata.size() - head, size_t(h.gridWidth) * h.gridHeight, m_motionGrid)) return;
    m_motionWidth = h.gridWidth;
    m_motionHeight = h.gridHeight;
    m_motionBlock = h.blockSize;
    m_motionFrameNs = int64_t(m_vrMeta.trackingTimestampNs - h.previousTimestampNs);
    m_motionMoves = std::any_of(m_motionGrid.begin(), m_motionGrid.end(), [](int8_t v) { return v != 0; });
    if (m_motionMoves) m_scene.UploadMotion(m_motionGrid.data(), m_motionWidth, m_motionHeight);
}

// No new frame for this display: move the moving things on from the last picture. The layer
// keeps the last frame's pose, so the compositor still turns it with the head.
void App::GenerateFrame(const FrameState& frame) {
    if (!m_motionMoves || !m_vrHasFrame || !m_vrSwapchain.handle || m_motionFrameNs <= 0) return;
    const int64_t sinceFrame = m_xr.ToMonotonicNs(frame.predictedDisplayTime) - int64_t(m_vrMeta.trackingTimestampNs);
    const float amount = float(double(sinceFrame) / double(m_motionFrameNs));
    // Further out than this the guess gets worse than a held picture.
    if (amount <= 0.05f || amount > 1.25f) return;
    const GLuint fbo = m_xr.AcquireImage(m_vrSwapchain);
    const float perPixel = amount * 127.f * rc::kMotionUnitPixels;
    m_scene.DrawKeptMoved(fbo, m_vrSwapchain.width, m_vrSwapchain.height, perPixel / (m_vrEyeWidth * 2), perPixel / m_vrEyeHeight,
                          float(m_vrEyeWidth * 2) / float(m_motionWidth * m_motionBlock),
                          float(m_vrEyeHeight) / float(m_motionHeight * m_motionBlock));
    m_xr.ReleaseImage(m_vrSwapchain);
    m_generatedFrames++;
    const int64_t now = rc::NowNs();
    if (now - m_generatedLogNs > 5'000'000'000LL) {
        RC_LOGI("Frame generation: %u frames in the last %.0f s", m_generatedFrames, double(now - m_generatedLogNs) / 1e9);
        m_generatedFrames = 0;
        m_generatedLogNs = now;
    }
}

void App::LatchVideo(const FrameState& frame) {
    // Latching waits for swapchain images and blits: keep the network thread out of that.
    // (Only this thread removes decoders, so the last reference still drops on the GL thread.)
    std::map<uint8_t, std::shared_ptr<VideoDecoder>> decoders;
    {
        std::lock_guard lock(m_decoderMutex);
        decoders = m_decoders;
    }
    for (auto& [id, dec] : decoders) {
        if (!dec) continue;
        DecodedFrameInfo info;
        if (!dec->Latch(info)) continue;
        const int64_t latchNs = rc::NowNs();
        if (id == rc::kVrStreamId) {
            const bool haveMeta = info.metadata.size() >= sizeof(rc::VrFrameMetadata);
            if (haveMeta) std::memcpy(&m_vrMeta, info.metadata.data(), sizeof(m_vrMeta));
            if (m_vrFoveated && info.metadata.size() >= sizeof(rc::VrFrameMetadata) + sizeof(rc::VrFoveationMetadata)) {
                rc::VrFoveationMetadata fm;
                std::memcpy(&fm, info.metadata.data() + sizeof(rc::VrFrameMetadata), sizeof(fm));
                std::memcpy(m_vrFoveation.start, fm.foveaStart, sizeof(fm.foveaStart));
            }
            const VideoFoveation* foveation = m_vrFoveated ? &m_vrFoveation : nullptr;
            if (m_settings.upscaling != m_vrUpscaleSetting && m_vrSwapchain.handle) CreateVrSwapchain();
            if (m_vrSwapchain.handle) {
                const GLuint fbo = m_xr.AcquireImage(m_vrSwapchain);
                // Generating a frame needs the last picture afterwards, too.
                const bool keep = m_settings.frameGeneration && m_scene.UpscalerReady();
                if (m_vrUpscaled || keep) {
                    m_scene.KeepVideo(dec->Texture(), dec->Crop(), m_vrEyeWidth * 2, m_vrEyeHeight, foveation);
                    if (m_vrUpscaled) m_scene.DrawKeptUpscaled(fbo, m_vrSwapchain.width, m_vrSwapchain.height);
                    else m_scene.DrawKept(fbo, m_vrSwapchain.width, m_vrSwapchain.height);
                } else {
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    m_scene.BlitExternal(dec->Texture(), m_vrSwapchain.width, m_vrSwapchain.height, dec->Crop(), 0, 0, 1, 1,
                                         foveation);
                }
                m_xr.ReleaseImage(m_vrSwapchain);
            }
            m_vrHasFrame = true;
            m_vrLatched = true;
            ReadMotion(info.metadata);
            m_vrFrames++;
            // Latency feedback: the frame was rendered for `trackingTimestampNs` and will be
            // shown at this frame's display time; steer our prediction toward reality.
            const int64_t displayNs = m_xr.ToMonotonicNs(frame.predictedDisplayTime);
            const double errorMs = double(displayNs - int64_t(m_vrMeta.trackingTimestampNs)) / 1e6;
            if (std::fabs(errorMs) < 200) m_pipelineLatencyMs = std::clamp(m_pipelineLatencyMs + errorMs * 0.05, 15.0, 120.0);
            auto it = m_trackingSent.find(m_vrMeta.trackingTimestampNs);
            if (it != m_trackingSent.end()) {
                const double latency = double(displayNs - it->second) / 1e6;
                m_latencySum += latency;
                m_latencyCount++;
                if (haveMeta && info.receivedNs && info.decodedNs && m_client.HasClockOffset()) {
                    // PC times onto the headset's clock. How the pose's trip to the PC and the
                    // game share the time between them isn't measured: the trip counts as half
                    // the round trip.
                    const int64_t offset = m_client.ClockOffsetNs();
                    const double up = m_client.Stats().rttMs / 2;
                    double s[kStageCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
                    s[kStageUplink] = up;
                    s[kStageGame] = double(m_vrMeta.pcSubmitNs - offset - it->second) / 1e6 - up;
                    s[kStageEncoderWait] = double(m_vrMeta.pcEncodeStartNs - m_vrMeta.pcSubmitNs) / 1e6;
                    s[kStageEncode] = double(m_vrMeta.pcEncodeEndNs - m_vrMeta.pcEncodeStartNs) / 1e6;
                    s[kStageDownlink] = double(info.receivedNs - (m_vrMeta.pcEncodeEndNs - offset)) / 1e6;
                    s[kStageDecode] = double(info.decodedNs - info.receivedNs) / 1e6;
                    s[kStageNextFrame] = double(latchNs - info.decodedNs) / 1e6;
                    s[kStageCompositor] = double(displayNs - latchNs) / 1e6;
                    AddStages(s);
                }
            }
            // Where this frame landed against the moment it should have been ready: the late
            // latch, less a margin. Folded into one display period, since a frame that just
            // missed it is the same story as one a whole period too early.
            if (m_latchLeadNs > 0 && info.decodedNs && frame.period > 0) {
                const double period = double(frame.period);
                const double target = double(displayNs) - m_latchLeadNs - m_phaseMarginNs;
                double e = std::fmod(double(info.decodedNs) - target, period);
                if (e >= period / 2) e -= period;
                if (e < -period / 2) e += period;
                m_phaseErrors.push_back(e);
            }
        } else {
            Window* w = m_windows.Find("monitor-" + std::to_string(id));
            if (!w) continue;
            w->videoTexture = dec->Texture();
            std::memcpy(w->videoCrop, dec->Crop(), sizeof(w->videoCrop));
            w->videoReady = true;
            if (w->layerBacked) {
                auto& sc = m_monitorLayers[id];
                if (!sc.handle) m_xr.CreateSwapchain(dec->Width(), dec->Height(), GL_SRGB8_ALPHA8, 1, false, sc);
                if (sc.handle) {
                    const GLuint fbo = m_xr.AcquireImage(sc);
                    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                    m_scene.BlitExternal(dec->Texture(), sc.width, sc.height, dec->Crop());
                    m_xr.ReleaseImage(sc);
                    m_monitorFresh[id] = true;
                }
            }
            if (info.receivedNs) {
                const int64_t displayNs = m_xr.ToMonotonicNs(frame.predictedDisplayTime);
                rc::DesktopFrameMetadata dm{};
                if (info.metadata.size() >= sizeof(dm) && info.decodedNs && m_client.HasClockOffset()) {
                    // From the PC's capture to the headset's display, as the protocol defines it.
                    std::memcpy(&dm, info.metadata.data(), sizeof(dm));
                    const int64_t offset = m_client.ClockOffsetNs();
                    m_latencySum += double(displayNs - (dm.pcCaptureNs - offset)) / 1e6;
                    double s[kStageCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
                    s[kStageCapture] = double(dm.pcEncodeEndNs - dm.pcCaptureNs) / 1e6;
                    s[kStageDownlink] = double(info.receivedNs - (dm.pcEncodeEndNs - offset)) / 1e6;
                    s[kStageDecode] = double(info.decodedNs - info.receivedNs) / 1e6;
                    s[kStageNextFrame] = double(latchNs - info.decodedNs) / 1e6;
                    s[kStageCompositor] = double(displayNs - latchNs) / 1e6;
                    AddStages(s);
                } else {
                    // Before the clocks are matched: the headset's part only.
                    m_latencySum += double(displayNs - info.receivedNs) / 1e6;
                }
                m_latencyCount++;
            }
        }
        m_statsFrames += id == rc::kVrStreamId || m_mode != Mode::Vr ? 1 : 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Once per second: statistics for the overlay and the PC.
    const int64_t now = rc::NowNs();
    if (now - m_statsWindowStart > 1'000'000'000LL) {
        const double secs = double(now - m_statsWindowStart) / 1e9;
        m_statsWindowStart = now;
        const bool vr = m_mode == Mode::Vr || m_mode == Mode::LaunchingVr;
        m_stats.fps = m_statsFrames / secs / std::max<size_t>(1, vr ? 1 : m_desktopStreams.size());
        m_stats.latencyMs = m_latencyCount ? m_latencySum / m_latencyCount : 0;
        m_statsFrames = 0;
        m_latencySum = 0;
        m_latencyCount = 0;
        json stages = json::object();
        for (int i = 0; i < kStageCount; i++) {
            m_stats.stageMs[i] = m_stageFrames[i] ? m_stageSum[i] / m_stageFrames[i] : -1;
            if (m_stats.stageMs[i] >= 0) stages[kLatencyStages[i].key] = std::round(m_stats.stageMs[i] * 10) / 10;
            m_stageSum[i] = 0;
            m_stageFrames[i] = 0;
        }
        m_stats.rttMs = m_client.Stats().rttMs;
        double decode = 0;
        for (auto& [id, dec] : decoders) {
            if (!dec) continue;
            const double d = dec->AverageDecodeMs();
            m_client.ReportDecode(id, d, m_stats.latencyMs);
            if ((id == rc::kVrStreamId) == vr) decode = std::max(decode, d);
        }
        m_stats.decodeMs = decode;
        if (vr && m_serverStats.contains("vr")) {
            m_stats.mbps = m_serverStats["vr"].value("mbps", 0.0);
        } else if (m_serverStats.contains("desktop")) {
            double mbps = 0;
            for (const auto& d : m_serverStats["desktop"]) mbps += d.value("mbps", 0.0);
            m_stats.mbps = mbps;
        }
        const char* names[] = {"H.264", "HEVC", "AV1"};
        m_stats.codec = vr ? names[int(m_vrCodec) % 3]
                           : (m_desktopStreams.empty() ? "" : names[int(m_desktopStreams.begin()->second.codec) % 3]);
        m_stats.width = vr ? m_vrEyeWidth : (m_desktopStreams.empty() ? 0 : m_desktopStreams.begin()->second.width);
        m_stats.height = vr ? m_vrEyeHeight : (m_desktopStreams.empty() ? 0 : m_desktopStreams.begin()->second.height);
        m_client.SendControl({{"type", rc::msg::kStats},
                              {"stats", {{"fps", m_stats.fps}, {"latencyMs", m_stats.latencyMs}, {"decodeMs", m_stats.decodeMs}, {"latencyStages", stages}}}});
    }
}

// The PC paces SteamVR with a vsync of its own, at the headset's rate but not in step with
// its display: frames finished at any point of the headset's frame and waited up to a whole
// one to be shown. Twice a second, tell the PC how far to move its vsync so they finish just
// before the late latch, with a margin wide enough for how much they vary.
void App::SteerVrTiming(const FrameState& frame) {
    const int64_t now = rc::NowNs();
    if (m_mode != Mode::Vr || frame.period <= 0) {
        m_phaseErrors.clear();
        m_phaseSentNs = now;
        return;
    }
    if (now - m_phaseSentNs < 500'000'000LL) return;
    m_phaseSentNs = now;
    if (m_phaseErrors.size() < 10) {
        m_phaseErrors.clear();
        return;
    }
    // Averaged as angles on the period's circle, or early and late arrivals near the wrap
    // point would cancel out.
    const double period = double(frame.period), k = 6.283185307 / period;
    double sinSum = 0, cosSum = 0;
    for (double e : m_phaseErrors) {
        sinSum += std::sin(e * k);
        cosSum += std::cos(e * k);
    }
    const double mean = std::atan2(sinSum, cosSum) / k;
    double spread = 0;
    for (double e : m_phaseErrors) {
        double d = std::fmod(e - mean + 1.5 * period, period) - period / 2;
        spread += d * d;
    }
    spread = std::sqrt(spread / double(m_phaseErrors.size()));
    m_phaseErrors.clear();
    const double margin = std::clamp(2.5 * spread, 2e6, 0.4 * period);
    m_phaseMarginNs = m_phaseMarginNs * 0.7 + margin * 0.3;
    m_client.SendControl({{"type", rc::msg::kVrPhase}, {"shiftNs", int64_t(-mean)}});
}

void App::AddStages(const double (&ms)[kStageCount]) {
    for (int i = 0; i < kStageCount; i++) {
        if (ms[i] < 0 && ms[i] > -2) {
            // A slightly wrong clock offset can push a short stage just below zero.
            m_stageFrames[i]++;
        } else if (ms[i] >= 0 && ms[i] < 1000) {
            m_stageSum[i] += ms[i];
            m_stageFrames[i]++;
        }
    }
}

void App::RenderEyes(const FrameState& frame, float dt) {
    const Environment env = EnvironmentFromKey(m_settings.environment);
    const bool inVr = m_mode == Mode::Vr;
    m_sky.Prepare(inVr ? Environment::Passthrough : env, m_time);
    m_windows.SortForDrawing(frame.head.position);
    for (int eye = 0; eye < 2; eye++) {
        m_xr.BindEye(eye);
        glClearColor(0, 0, 0, 0);
        glClearDepthf(1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const EyeView& v = frame.eyes[eye];
        const Mat4 viewProj = v.projection * v.view;

        // Environment (or hyperspace). In VR the game stream is underneath; we only
        // draw the fading tunnel on top of it.
        const Environment drawEnv = inVr ? Environment::Passthrough : env;
        m_sky.Draw(v.projection, v.pose.orientation, drawEnv, m_time, m_settings.skyBrightness, m_warp, m_warpPhase,
                   m_tunnelOrientation);

        m_scene.SetViewProjection(viewProj, v.pose.position);
        m_windows.Draw(m_scene);

        // Lasers.
        for (int h = 0; h < 2; h++) {
            const auto& r = m_rays[h];
            if (!r.valid) continue;
            const WindowHit& hit = m_hits[h];
            const float len = hit.window ? hit.distance : 2.5f;
            const Vec3 end = r.origin + r.dir * len;
            const float press = r.triggerDown ? 1.f : 0.f;
            const Color start = Color(1.f, 1.f, 1.f, 0.0f);
            const Color tip = Mix(Color(1.f, 1.f, 1.f, hit.window ? 0.6f : 0.3f), Color(0.42f, 0.7f, 1.f, 0.9f), press);
            m_scene.Beam(r.origin + r.dir * 0.03f, end, 0.0035f, start, tip);
            m_scene.ControllerOrb(m_input.Hand(h).aim, Color(1, 1, 1, 0.65f));
            if (hit.window) {
                const bool onMonitor = hit.window->kind == WindowKind::Video && hit.zone == HitZone::Content;
                // The PC cursor is part of the video; keep our dot subtle there.
                m_scene.Dot(hit.point, hit.normal, onMonitor ? 0.004f : 0.007f,
                            Color(1, 1, 1, onMonitor ? 0.35f : 0.95f));
            }
        }
    }
    (void)dt;
}

void App::UpdateStatsLayer(Window* stats, bool active, float dt) {
    if (stats) stats->canvasLayer = active && m_statsLayer.handle;
    m_statsLayerActive = active && m_statsLayer.handle && m_statsDrawnNs;
    if (!active) {
        m_statsDrawnNs = 0;  // the next time, draw at once
        return;
    }
    const int w = int(stats->canvasW * stats->rtScale), h = int(stats->canvasH * stats->rtScale);
    if (m_statsLayer.handle && (m_statsLayer.width != w || m_statsLayer.height != h)) m_xr.DestroySwapchain(m_statsLayer);
    if (!m_statsLayer.handle && !m_xr.CreateSwapchain(w, h, GL_SRGB8_ALPHA8, 1, false, m_statsLayer)) return;
    stats->canvasLayer = true;
    // Its numbers change once a second.
    const int64_t now = rc::NowNs();
    if (m_statsDrawnNs && now - m_statsDrawnNs < 250'000'000LL) return;
    m_windows.RenderCanvasTo(*stats, m_xr.AcquireImage(m_statsLayer), w, h, dt, m_time);
    m_xr.ReleaseImage(m_statsLayer);
    m_statsDrawnNs = now;
    m_statsLayerActive = true;
}

void App::UpdateUiLayers() {
    // The headset composes up to 16 layers; the game, monitors, the overlay, passthrough and
    // the eye images come first.
    constexpr int kMaxUiLayers = 6;
    int used = 0;
    for (const auto& wp : m_windows.All()) {
        Window& w = *wp;
        if (w.kind != WindowKind::Ui) continue;
        // While it fades in or out, it's drawn into the eye images as before.
        // Layered from the first frame it is visible: drawn into the eye images while it faded,
        // a window was soft until it became a layer, then popped sharp. The fade is the layer's.
        const bool layer = m_settings.sharpLayers && !w.canvasLayer && !w.removed && w.visible && w.appear > 0.01f &&
                           w.curveRadius == 0 && w.rt.fbo && used < kMaxUiLayers;
        w.layerBacked = false;
        if (!layer) continue;
        XrSwapchainGl& sc = m_uiLayers[w.id];
        const int width = w.rt.color.width, height = w.rt.color.height;
        if (sc.handle && (sc.width != width || sc.height != height)) m_xr.DestroySwapchain(sc);
        if (!sc.handle && !m_xr.CreateSwapchain(width, height, GL_SRGB8_ALPHA8, 1, false, sc)) continue;
        // Pixel for pixel: the canvas was just drawn at this size. While it fades in or out, the
        // canvas goes in at that opacity instead, so the window is sharp from its first frame.
        const GLuint fbo = m_xr.AcquireImage(sc);
        if (w.appear < 0.995f) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            m_scene.Blit2DAlpha(w.rt.color.id, width, height, w.appear);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, w.rt.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
            glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        m_xr.ReleaseImage(sc);
        w.layerBacked = true;
        used++;
    }
    // Windows that are gone or hidden give their images back.
    for (auto it = m_uiLayers.begin(); it != m_uiLayers.end();) {
        const Window* w = m_windows.Find(it->first);
        if (!w || w->removed || (!w->visible && w->appear < 0.01f)) {
            m_xr.DestroySwapchain(it->second);
            it = m_uiLayers.erase(it);
        } else {
            ++it;
        }
    }
}

void App::BuildLayers(const FrameState& frame) {
    m_underlays.clear();
    m_overlays.clear();
    m_quadLayers.clear();
    m_cylinderLayers.clear();
    m_layerSettings.clear();
    // Room for every layer: m_underlays points into these.
    m_quadLayers.reserve(32);
    m_cylinderLayers.reserve(32);
    m_layerSettings.reserve(32);

    // VR stream: the PC frame with the exact pose it was rendered for; the Quest
    // compositor reprojects it to the current head pose.
    if (m_mode == Mode::Vr && m_vrHasFrame && m_vrSwapchain.handle) {
        const Pose head{{m_vrMeta.renderPose.orientation[0], m_vrMeta.renderPose.orientation[1],
                         m_vrMeta.renderPose.orientation[2], m_vrMeta.renderPose.orientation[3]},
                        {m_vrMeta.renderPose.position[0], m_vrMeta.renderPose.position[1], m_vrMeta.renderPose.position[2]}};
        const float ipd = m_vrMeta.ipd > 0.04f ? m_vrMeta.ipd : 0.063f;
        for (int eye = 0; eye < 2; eye++) {
            XrCompositionLayerProjectionView& v = m_vrViews[eye];
            v = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            const Pose eyePose = head * Pose{{}, {eye == 0 ? -ipd / 2 : ipd / 2, 0, 0}};
            v.pose = ToXr(eyePose);
            const float* t = m_vrMeta.fovTan[eye];
            v.fov = {-std::atan(t[0]), std::atan(t[1]), std::atan(t[2]), -std::atan(t[3])};
            v.subImage.swapchain = m_vrSwapchain.handle;
            v.subImage.imageRect.offset = {eye * m_vrSwapchain.width / 2, 0};
            v.subImage.imageRect.extent = {m_vrSwapchain.width / 2, m_vrSwapchain.height};
        }
        m_vrLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        m_vrLayer.space = m_xr.AppSpace();
        m_vrLayer.viewCount = 2;
        m_vrLayer.views = m_vrViews;
        // Sharpening shows up compression artifacts, so only with bits to spare.
        if (m_xr.LayerSettings() && m_stats.mbps >= 40) {
            XrCompositionLayerSettingsFB s{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
            s.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
            m_layerSettings.push_back(s);
            m_vrLayer.next = &m_layerSettings.back();
        }
        m_underlays.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_vrLayer));
    }

    if (m_statsLayerActive) {
        if (Window* w = m_windows.Find("stats")) {
            const void* next = nullptr;
            if (m_xr.LayerSettings()) {
                XrCompositionLayerSettingsFB s{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
                s.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB;
                m_layerSettings.push_back(s);
                next = &m_layerSettings.back();
            }
            XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.next = next;
            // The canvas is premultiplied, and its rounded corners are see-through.
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            q.space = m_xr.AppSpace();
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = m_statsLayer.handle;
            q.subImage.imageRect.extent = {m_statsLayer.width, m_statsLayer.height};
            q.pose = ToXr(w->pose);
            q.size = {w->widthM, w->HeightM()};
            m_quadLayers.push_back(q);
            m_overlays.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_quadLayers.back()));
        }
    }

    // Monitors and UI windows as compositor layers (sharp text): underneath the projection
    // layer, which punches matching holes. Back to front like the windows, so where two overlap
    // the nearer one is on top.
    for (Window* w : m_windows.DrawOrder()) {
        if (w->kind == WindowKind::Ui) {
            const auto it = m_uiLayers.find(w->id);
            if (!w->layerBacked || it == m_uiLayers.end() || !it->second.handle) continue;
            const void* next = nullptr;
            if (m_xr.LayerSettings()) {
                XrCompositionLayerSettingsFB s{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
                s.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB;
                m_layerSettings.push_back(s);
                next = &m_layerSettings.back();
            }
            XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.next = next;
            // Premultiplied, with see-through rounded corners (as the overlay above).
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            q.space = m_xr.AppSpace();
            q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain = it->second.handle;
            q.subImage.imageRect.extent = {it->second.width, it->second.height};
            q.pose = ToXr(w->pose);
            q.size = {w->widthM, w->HeightM()};
            m_quadLayers.push_back(q);
            m_underlays.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_quadLayers.back()));
            continue;
        }
        if (m_mode == Mode::Desktop) {
            if (w->id.rfind("monitor-", 0) != 0) continue;
            const auto it = m_monitorLayers.find(uint8_t(std::atoi(w->id.c_str() + 8)));
            if (it == m_monitorLayers.end()) continue;
            const uint8_t id = it->first;
            XrSwapchainGl& sc = it->second;
            if (!sc.handle || !w->layerBacked || !w->visible || w->appear < 0.98f || !m_monitorFresh[id]) continue;
            const void* next = nullptr;
            if (m_xr.LayerSettings()) {
                XrCompositionLayerSettingsFB s{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
                s.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB |
                               XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
                m_layerSettings.push_back(s);
                next = &m_layerSettings.back();
            }
            if (w->curveRadius > 0) {
                XrCompositionLayerCylinderKHR c{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
                c.next = next;
                c.space = m_xr.AppSpace();
                c.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                c.subImage.swapchain = sc.handle;
                c.subImage.imageRect.extent = {sc.width, sc.height};
                // Cylinder centre sits `radius` in front of the window surface.
                c.pose = ToXr({w->pose.orientation, w->pose.Transform({0, 0, w->curveRadius})});
                c.radius = w->curveRadius;
                c.centralAngle = w->widthM / w->curveRadius;
                c.aspectRatio = w->widthM / w->HeightM();
                m_cylinderLayers.push_back(c);
                m_underlays.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_cylinderLayers.back()));
            } else {
                XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.next = next;
                q.space = m_xr.AppSpace();
                q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                q.subImage.swapchain = sc.handle;
                q.subImage.imageRect.extent = {sc.width, sc.height};
                q.pose = ToXr(w->pose);
                q.size = {w->widthM, w->HeightM()};
                m_quadLayers.push_back(q);
                m_underlays.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_quadLayers.back()));
            }
        }
    }
    (void)frame;
}

void App::CreateWindows() {
    screens::Build(*this, m_windows);
    ApplyAccessibility();
    if (!m_settings.manualPc.empty()) SetManualPc(m_settings.manualPc);
    rc::NetAddress lastAddress;
    if (rc::NetAddress::Parse(m_settings.lastPcAddress, rc::kDiscoveryPort, lastAddress)) m_discovery.AddProbeTarget(lastAddress);
}

}  // namespace rv

// ---------------------------------------------------------------------------

#ifdef __ANDROID__
void android_main(android_app* app) {
    rv::App rincast(app);
    rincast.Run();
}
#else
int main() {
    rv::App rincast(nullptr);
    rincast.Run();
    return 0;
}
#endif
