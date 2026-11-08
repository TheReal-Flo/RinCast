// RinCast headset application.
#pragma once

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../gfx/scene.h"
#include "../gfx/sky.h"
#include "../gfx/ui_renderer.h"
#include "../media/audio.h"
#include "../media/music.h"
#include "../media/video_decoder.h"
#include "../net/cloud.h"
#include "../net/stream_client.h"
#include "../platform/assets.h"
#include "../platform/jni_bridge.h"
#include "../ui/window.h"
#include "../headsets/headset.h"
#include "../keyboard/keyboard.h"
#include "../xr/xr_context.h"
#include "../xr/xr_input.h"
#include "rc/motion_field.h"
#include "latency.h"
#include "settings.h"

struct android_app;

namespace rv {

enum class Mode { Home, Connecting, Desktop, LaunchingVr, Vr };

struct MonitorInfo {
    std::string id;
    std::string name;
    int index = 0;
    int width = 0, height = 0;
    bool primary = false;
};

struct DesktopStreamInfo {
    uint8_t streamId = 0;
    std::string monitorId;
    std::string name;
    int width = 0, height = 0;
    rc::VideoCodec codec = rc::VideoCodec::HEVC;
    float fps = 60;
};

struct GameInfo {
    std::string id;
    std::string name;
    bool hasImage = false;
    Texture image;
    bool imageRequested = false;
};

struct Notice {
    std::string text;
    std::string level;    // info | warning | error
    std::string action;   // optional control message to send when tapped
    float age = 0;
    float life = 6;       // seconds on screen
};

class App {
   public:
    explicit App(android_app* androidApp);
    ~App();
    void Run();

    // ---- used by the UI builders (screens.cpp) ----
    HeadsetSettings& Settings() { return m_settings; }
    kb::Keyboard& Kb() { return m_keyboard; }
    void SaveSettings();
    void ApplyStreamSettings();      // pushes streaming settings to the PC
    Mode CurrentMode() const { return m_mode; }
    Discovery& Lan() { return m_discovery; }
    Cloud& Account() { return m_cloud; }
    StreamClient& Client() { return m_client; }
    XrContext& Xr() { return m_xr; }
    Headset& Device() { return *m_headset; }
    XrInput& Input() { return m_input; }
    // Foveated encoding can follow the eyes.
    bool GazeSupported() { return m_headset->GazeSupported(m_xr) || m_input.GazeSupported(); }

    void ConnectLan(const DiscoveredPc& pc);
    void ConnectCloud(const CloudPc& pc);
    void Disconnect();  // requests a disconnect; handled at the start of the next frame
    void LaunchSteamVr();
    void LaunchGame(const std::string& id);
    void ExitVr(bool byUser = false);
    void Recenter();
    // Applies the curve and distance settings to the screens where they are now.
    void UpdateScreenShape(float previousDistance);
    // Why the last connection failed, until retried or dismissed. While the connection is
    // being retried by itself, Reconnecting() is true.
    const std::string& ConnectionProblem() const { return m_problem; }
    bool Reconnecting() const { return m_retryUntil > m_time; }
    int ReconnectAttempt() const { return m_retryAttempt; }
    float GamesRequestedAgo() const { return m_time - m_gamesRequestedAt; }
    bool GamesAnswered() const { return m_gamesAnswered; }
    float Time() const { return m_time; }
    bool CanRetry() const { return m_haveLast; }
    void RetryConnection();
    void DismissProblem() { m_problem.clear(); m_retryUntil = 0; }
    void SetManualPc(const std::string& address);
    float TimeOnHomeWithoutPcs(bool noPcs);
    void ApplyAccessibility();
    void ToggleWindow(const std::string& id, bool show);
    void ToggleWindow(const std::string& id) { ToggleWindow(id, !IsVisible(id)); }
    bool IsVisible(const std::string& id);
    void ShowMonitors(bool show);
    bool MonitorsVisible() const { return m_monitorsVisible; }
    void SendKey(uint16_t vk, bool down);
    void SendText(const std::string& text);
    void PostNotice(const std::string& text, const std::string& level = "info", const std::string& action = {});
    void DismissNotice();
    void RunAction(const std::string& action);

    const json& ServerInfo() const { return m_serverInfo; }
    const json& ServerStats() const { return m_serverStats; }
    std::vector<GameInfo>& Games() { return m_games; }
    void RequestGames();
    void RequestGameImage(GameInfo& g);
    const std::vector<MonitorInfo>& Monitors() const { return m_monitors; }
    std::string VrStateText() const;
    bool VrRunning() const { return m_vrState == "running"; }
    const std::deque<Notice>& Notices() const { return m_notices; }
    struct Stats {
        double fps = 0, latencyMs = 0, decodeMs = 0, mbps = 0, rttMs = 0;
        std::string codec;
        int width = 0, height = 0;
        double stageMs[kStageCount] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};  // -1: not measured
    };
    Stats CurrentStats() const { return m_stats; }
    // A USB cable to the PC: the setting, and what the headset says about it.
    const Platform::UsbNetwork& UsbNetwork() const { return m_usbNetwork; }
    void SetUsbCable(bool on);
    float WarpProgress() const { return m_warp; }
    float ModeTime() const { return m_modeTime; }
    // A picture of a sky, or an empty texture for those without one.
    const Texture& EnvironmentThumbnail(const std::string& key);

   private:
    void Init();
    void Shutdown();
    void HandleAndroidEvents();
    void Frame();
    void UpdateInput(const FrameState& frame, float dt);
    void UpdateTracking(const FrameState& frame);
    void UpdateBody(const FrameState& frame);
    void CreateVrSwapchain();
    void ReadMotion(const std::vector<uint8_t>& metadata);
    void GenerateFrame(const FrameState& frame);
    void ProcessNetwork();
    void HandleControl(const json& m);
    void OnVideoFrame(rc::ReceivedFrame&& frame);
    void ConfigureDesktopStreams(const json& streams);
    void ConfigureVrStream(const json& stream);
    void LayoutMonitors(bool force);
    void LayoutHome(bool force);
    void UpdateMode(float dt);
    void EnterMode(Mode m);
    void LatchVideo(const FrameState& frame);
    void RenderEyes(const FrameState& frame, float dt);
    void BuildLayers(const FrameState& frame);
    void SendHello();
    void CreateWindows();
    void DisconnectNow();
    void PollDebugProperties();
    bool VrFrameAvailable() const { return m_vrHasFrame; }

    android_app* m_android;
    Assets m_assets;
    HeadsetSettings m_settings;
    std::string m_settingsPath;

    XrContext m_xr;
    XrInput m_input;
    std::unique_ptr<Headset> m_headset;
    bool m_facePermissionAsked = false;
    bool m_gazePermissionAsked = false;
    void UpdateFace(const FrameState& frame);
    bool m_bodyPermissionAsked = false;
    SkyRenderer m_sky;
    UiRenderer m_ui;
    SceneRenderer m_scene;
    WindowManager m_windows;
    Discovery m_discovery;
    Cloud m_cloud;
    StreamClient m_client;
    AudioPlayer m_audio;
    MusicPlayer m_music;
    kb::Keyboard m_keyboard;
    MicCapture m_mic;
    bool m_micReleased = false;  // stopped in the background; taken again in front
    void ReleaseMicInBackground();

    bool m_initialized = false;
    bool m_exit = false;
    bool m_disconnectRequested = false;
    Mode m_mode = Mode::Home;
    float m_modeTime = 0;
    float m_time = 0;
    int64_t m_lastFrameNs = 0;
    Pose m_head;
    int m_spaceGeneration = 0;

    // Network -> main thread
    std::mutex m_netMutex;
    std::vector<json> m_controlQueue;

    json m_serverInfo = json::object();
    json m_serverStats = json::object();
    std::vector<MonitorInfo> m_monitors;
    std::vector<GameInfo> m_games;
    std::map<std::string, Texture> m_envThumbnails;
    std::deque<Notice> m_notices;
    std::string m_vrState = "idle";
    std::string m_vrProducer, m_vrApp;
    // Set when the user leaves VR while its app keeps running on the PC: the vrState
    // updates that keep arriving must not yank them back in. Cleared when the VR app
    // stops on the PC, so the next one it starts still pulls the headset in.
    bool m_vrLeftByUser = false;  // the user left VR: stay on the desktop until they return

    // Video
    bool m_streamingPerformance = false;
    std::string m_problem;
    bool m_haveLast = false, m_lastIsCloud = false;
    DiscoveredPc m_lastLan;
    CloudPc m_lastCloud;
    float m_retryUntil = 0, m_lastRetry = 0;
    int m_retryAttempt = 0;
    float m_gamesRequestedAt = -100;
    bool m_gamesAnswered = false;
    float m_noPcsSince = -1;
    float m_abHeld = 0;
    std::string m_spokenProblem;
    Vec3 m_smoothDir[2];
    void ReconnectTick();
    std::mutex m_decoderMutex;
    std::map<uint8_t, std::shared_ptr<VideoDecoder>> m_decoders;
    std::map<uint8_t, DesktopStreamInfo> m_desktopStreams;
    std::map<uint8_t, XrSwapchainGl> m_monitorLayers;
    std::map<uint8_t, bool> m_monitorFresh;
    bool m_monitorsVisible = true;
    bool m_monitorsLaidOut = false;
    int m_vrEyeWidth = 0, m_vrEyeHeight = 0;
    rc::VideoCodec m_vrCodec = rc::VideoCodec::HEVC;
    // Foveated encoding: the stream's frames are squeezed (see rc::VrFoveationMetadata);
    // m_vrEyeWidth x m_vrEyeHeight is the size they're stretched back to.
    bool m_vrFoveated = false;
    VideoFoveation m_vrFoveation;
    // Where the eyes look: the family's own eye tracking, or XR_EXT_eye_gaze_interaction.
    bool ReadGaze(XrTime time, rc::GazePayload& out);
    XrSwapchainGl m_vrSwapchain;
    // The performance overlay in a game: a compositor layer of its own (see Frame).
    void UpdateStatsLayer(Window* stats, bool active, float dt);
    XrSwapchainGl m_statsLayer;
    bool m_statsLayerActive = false;
    // Menus and other UI windows as compositor layers, like the monitors: drawn into the eye
    // images they went through two resamplings and looked blurry next to them.
    void UpdateUiLayers();
    std::map<std::string, XrSwapchainGl> m_uiLayers;
    int64_t m_statsDrawnNs = 0;
    bool m_vrUpscaleSetting = false;  // the setting m_vrSwapchain was made for
    bool m_vrUpscaled = false;        // m_vrSwapchain is sized for the upscaler
    bool m_vrHasFrame = false;
    float m_vrNoPicture = 0;  // seconds the PC has said "running" without a picture we could show
    rc::VrFrameMetadata m_vrMeta{};
    std::vector<int8_t> m_motionGrid;
    int m_motionWidth = 0, m_motionHeight = 0, m_motionBlock = 0;
    int64_t m_motionFrameNs = 0;   // time between the two frames it compares
    bool m_motionMoves = false;    // something moves: generating a frame would change the picture
    bool m_vrLatched = false;      // this display got a new VR frame
    uint32_t m_generatedFrames = 0;
    int64_t m_generatedLogNs = 0;
    uint32_t m_vrFrames = 0;

    // Tracking / latency
    double m_pipelineLatencyMs = 45;
    // The USB cable to the PC, as Android reports it, checked once a second.
    void UpdateUsbNetwork();
    Platform::UsbNetwork m_usbNetwork;
    int64_t m_usbCheckedNs = 0;
    // Steering the PC's frame timing so frames are decoded just before the late latch.
    void SteerVrTiming(const FrameState& frame);
    int64_t m_lateLatchNs = 0;
    double m_latchLeadNs = 0;          // late latch -> display, averaged
    double m_phaseMarginNs = 3e6;      // how early a frame should be ready, from how much arrivals vary
    std::vector<double> m_phaseErrors;
    int64_t m_phaseSentNs = 0;
    std::map<uint64_t, int64_t> m_trackingSent;  // target time -> sample time

    // Input state
    struct HandState {
        bool trigger = false, secondary = false, stickClick = false;
        bool menu = false;
        float secondaryHeld = 0;  // B or Y, to tell a press from the A+B chord
        bool chord = false;
        // A pinch on a screen: release soon = click, move = scroll, hold still = right-click.
        enum class Pinch { Undecided, Scrolling, Done } pinch = Pinch::Undecided;
        float pinchTime = 0, pinchStartV = 0, pinchLastV = 0;
        float lastWheelNs = 0;
        int lastMenuPress = 0;
    } m_handState[2];
    int64_t m_lastMenuPressNs = 0;
    WindowHit m_hits[2];
    WindowManager::HandRay m_rays[2];
    uint32_t m_pointerSeq = 0;

    // Transitions
    float m_warp = 0;           // hyperspace visibility
    float m_warpPhase = 0;
    float m_warpSpeed = 0;
    float m_arrival = 0;        // 0..1 crossfade into the VR stream
    bool m_vrOverlay = false;
    Quat m_tunnelOrientation;

    Stats m_stats;
    float m_lastDebugPoll = 0;
    std::string m_debugUi, m_debugTab, m_debugPc;
    std::set<std::string> m_placed;  // windows that have been positioned at least once
    int64_t m_statsWindowStart = 0;
    uint32_t m_statsFrames = 0;
    double m_latencySum = 0;
    uint32_t m_latencyCount = 0;
    double m_stageSum[kStageCount] = {};
    uint32_t m_stageFrames[kStageCount] = {};
    void AddStages(const double (&ms)[kStageCount]);

    // Layers built each frame (storage must outlive EndFrame)
    std::vector<XrCompositionLayerQuad> m_quadLayers;
    std::vector<XrCompositionLayerCylinderKHR> m_cylinderLayers;
    std::vector<XrCompositionLayerSettingsFB> m_layerSettings;
    XrCompositionLayerProjection m_vrLayer{};
    XrCompositionLayerProjectionView m_vrViews[2]{};
    std::vector<const XrCompositionLayerBaseHeader*> m_underlays;
    std::vector<const XrCompositionLayerBaseHeader*> m_overlays;
};

}  // namespace rv
