#include "client_session.h"
#include "../vr/face_share.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../app.h"
#include "../audio/audio.h"
#include "../encode/encoder.h"
#include "../input/input_injector.h"
#include "../settings.h"
#include "../stream/desktop_stream.h"
#include "../stream/vr_stream.h"
#include "../vr/steamvr.h"
#include "../vr/vr_bridge_host.h"
#include "rc/platform.h"

namespace rcs {

namespace {

float QualityScale(const std::string& q) {
    if (q == "low") return 0.6f;
    if (q == "medium") return 0.75f;
    if (q == "ultra") return 1.0f;
    if (q == "godlike") return 1.2f;
    return 0.9f;  // high
}

uint32_t Align(uint32_t v, uint32_t a) { return std::max(a, (v + a / 2) / a * a); }

}  // namespace

ClientSession::ClientSession(std::unique_ptr<rc::Connection> connection, const HelloInfo& hello,
                             std::string clientPkHex, std::vector<uint8_t> welcome, bool remote, bool vpn)
    : m_connection(std::move(connection)),
      m_hello(hello),
      m_clientPk(std::move(clientPkHex)),
      m_welcome(std::move(welcome)),
      m_remote(remote),
      m_vpn(vpn),
      m_slowPath(remote || vpn) {
    m_connectedAtNs = rc::NowNs();
    m_connection->SetControlHandler([this](const std::string& text) { OnControl(text); });
    m_connection->SetPacketHandler(
        [this](rc::PacketType type, const uint8_t* p, size_t n) { OnPacket(type, p, n); });
    if (hello.raw.contains("display") || hello.raw.contains("decoders")) ApplyClientInfo(hello.raw);
}

ClientSession::~ClientSession() { Close("destroyed"); }

void ClientSession::Start() {
    m_worker = std::thread([this] { WorkerLoop(); });
}

void ClientSession::Close(const std::string& reason) {
    if (m_closed.exchange(true)) return;
    RC_LOGI("Session with %s closed (%s)", m_hello.deviceName.c_str(), reason.c_str());
    m_connection->SendDisconnect();
    {
        std::lock_guard lock(m_queueMutex);
        m_queue.clear();
    }
    m_queueCv.notify_all();
    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) m_worker.join();

    std::map<uint8_t, std::unique_ptr<DesktopStream>> desktop;
    std::unique_ptr<VrStream> vr;
    std::unique_ptr<AudioStreamer> audioOut;
    std::unique_ptr<MicPlayer> mic;
    {
        std::lock_guard lock(m_mutex);
        desktop.swap(m_desktop);
        vr.swap(m_vr);
        audioOut.swap(m_audio);
        mic.swap(m_mic);
    }
    desktop.clear();
    vr.reset();
    audioOut.reset();
    mic.reset();
    App::Get().Bridge().SetHeadsetConnected(false);
    audio::RestoreDefaults();
    input::ReleaseAll();
}

void ClientSession::HandleDatagram(const uint8_t* data, size_t size, const rc::NetAddress& from) {
    if (m_closed) return;
    m_connection->HandleDatagram(data, size, from);
}

void ClientSession::Tick(int64_t nowNs) {
    if (m_closed) return;
    m_connection->Tick(nowNs);
    static thread_local int64_t lastStats = 0;
    if (nowNs - lastStats > 1'000'000'000LL) {
        lastStats = nowNs;
        // Periodic work runs on the worker thread.
        std::lock_guard lock(m_queueMutex);
        m_queue.push_back(json{{"type", "_tick"}});
        m_queueCv.notify_one();
    }
}

void ClientSession::SendControl(const json& message) {
    if (m_closed) return;
    m_connection->SendControl(message.dump());
}

ClientCaps ClientSession::Caps() {
    std::lock_guard lock(m_mutex);
    return m_caps;
}

ClientStreamSettings ClientSession::StreamSettings() {
    std::lock_guard lock(m_mutex);
    return m_settings;
}

// ---------------------------------------------------------------------------

void ClientSession::OnControl(const std::string& text) {
    json m;
    try {
        m = json::parse(text);
    } catch (...) {
        RC_LOGW("Malformed control message");
        return;
    }
    if (!m.is_object() || !m.contains("type")) return;
    // Input events are latency sensitive and cheap: handle them inline.
    const std::string type = m["type"].get<std::string>();
    if (type == rc::msg::kMouseButton) {
        input::MouseButton(m.value("button", 0), m.value("down", false));
        return;
    }
    if (type == rc::msg::kMouseWheel) {
        input::MouseWheel(m.value("dx", 0.f), m.value("dy", 0.f));
        return;
    }
    if (type == rc::msg::kKey) {
        input::Key(uint16_t(m.value("vk", 0)), m.value("down", false));
        return;
    }
    if (type == rc::msg::kText) {
        input::Text(Widen(m.value("text", "")));
        return;
    }
    std::lock_guard lock(m_queueMutex);
    m_queue.push_back(std::move(m));
    m_queueCv.notify_one();
}

void ClientSession::OnPacket(rc::PacketType type, const uint8_t* payload, size_t size) {
    switch (type) {
        case rc::PacketType::Tracking: {
            if (size < sizeof(rc::TrackingPayload)) return;
            rc::TrackingPayload t;
            std::memcpy(&t, payload, sizeof(t));
            rcvr::TrackingSnapshot s{};
            static std::atomic<uint64_t> seq{0};
            s.sequence = ++seq;
            s.receivedPcNs = rc::NowNs();
            s.headsetTargetNs = t.targetDisplayTimeNs;
            s.targetDisplayPcNs = m_connection->HasClockOffset()
                                      ? m_connection->RemoteToLocalNs(int64_t(t.targetDisplayTimeNs))
                                      : s.receivedPcNs + 40'000'000;
            s.flags = t.flags;
            std::memcpy(s.hmd.orientation, t.hmd.orientation, sizeof(float) * 4);
            std::memcpy(s.hmd.position, t.hmd.position, sizeof(float) * 3);
            std::memcpy(s.hmd.linearVelocity, t.hmdLinearVelocity, sizeof(float) * 3);
            std::memcpy(s.hmd.angularVelocity, t.hmdAngularVelocity, sizeof(float) * 3);
            for (int h = 0; h < 2; h++) {
                const auto& c = t.controllers[h];
                auto& d = s.controllers[h];
                d.flags = c.flags;
                d.buttons = c.buttons;
                d.trigger = c.trigger;
                d.grip = c.grip;
                d.thumbstick[0] = c.thumbstick[0];
                d.thumbstick[1] = c.thumbstick[1];
                std::memcpy(d.pose.orientation, c.pose.orientation, sizeof(float) * 4);
                std::memcpy(d.pose.position, c.pose.position, sizeof(float) * 3);
                std::memcpy(d.pose.linearVelocity, c.linearVelocity, sizeof(float) * 3);
                std::memcpy(d.pose.angularVelocity, c.angularVelocity, sizeof(float) * 3);
            }
            App::Get().Bridge().PublishTracking(s);
            break;
        }
        case rc::PacketType::BodyTracking: {
            if (size < sizeof(rc::BodyTrackingPayload)) return;
            static_assert(rc::kBodyTrackerCount == rcvr::kBodyTrackers);
            rc::BodyTrackingPayload b;
            std::memcpy(&b, payload, sizeof(b));
            rcvr::BodySnapshot s{};
            s.receivedPcNs = rc::NowNs();
            s.validMask = b.validMask;
            for (int i = 0; i < rc::kBodyTrackerCount; i++) {
                std::memcpy(s.poses[i].orientation, b.poses[i].orientation, sizeof(float) * 4);
                std::memcpy(s.poses[i].position, b.poses[i].position, sizeof(float) * 3);
            }
            App::Get().Bridge().PublishBody(s);
            break;
        }
        case rc::PacketType::FaceTracking: {
            if (size < sizeof(rc::FaceTrackingPayload)) return;
            rc::FaceTrackingPayload f;
            std::memcpy(&f, payload, sizeof(f));
            FaceShare::Get().Publish(f);
            break;
        }
        case rc::PacketType::HandSkeleton: {
            if (size < sizeof(rc::HandSkeletonPayload)) return;
            static_assert(rc::kHandBoneCount == rcvr::kHandBones);
            rc::HandSkeletonPayload h;
            std::memcpy(&h, payload, sizeof(h));
            if (h.hand > 1) return;
            rcvr::HandSnapshot s{};
            s.receivedPcNs = rc::NowNs();
            for (int i = 0; i < rc::kHandBoneCount; i++) {
                std::memcpy(s.bones[i].orientation, h.bones[i].orientation, sizeof(float) * 4);
                std::memcpy(s.bones[i].position, h.bones[i].position, sizeof(float) * 3);
            }
            App::Get().Bridge().PublishHand(h.hand, s);
            break;
        }
        case rc::PacketType::Pointer: {
            if (size < sizeof(rc::PointerPayload)) return;
            rc::PointerPayload p;
            std::memcpy(&p, payload, sizeof(p));
            if (!p.visible) return;
            std::lock_guard lock(m_mutex);
            auto it = m_desktop.find(p.monitorStreamId);
            if (it != m_desktop.end()) {
                input::MoveMouse(it->second->Monitor(), p.u, p.v);
                if (m_lastMouseMonitor != p.monitorStreamId) {
                    m_lastMouseMonitor = p.monitorStreamId;
                    for (auto& [id, s] : m_desktop) s->SetActive(true);
                }
            }
            break;
        }
        case rc::PacketType::Audio: {
            if (size < sizeof(rc::AudioPacketHeader)) return;
            if (payload[0] != uint8_t(rc::AudioStream::MicToPc)) return;
            std::lock_guard lock(m_mutex);
            if (m_mic) m_mic->PushPacket(payload, size);
            break;
        }
        case rc::PacketType::VideoFeedback: {
            if (size < rc::kVideoFeedbackMinSize) return;
            rc::VideoFeedbackPayload fb{};
            fb.queueDelayMs = -1;  // stays so when an older headset doesn't send it
            std::memcpy(&fb, payload, std::min(size, sizeof(fb)));
            std::lock_guard lock(m_mutex);
            if (fb.streamId == rc::kVrStreamId) {
                if (m_vr) m_vr->OnFeedback(fb);
            } else if (auto it = m_desktop.find(fb.streamId); it != m_desktop.end()) {
                it->second->OnFeedback(fb);
            }
            break;
        }
        default:
            break;
    }
}

void ClientSession::WorkerLoop() {
    ScopedCoInit com;
    // Greet: announce what this PC can do.
    SendServerInfo();
    SendMonitors();
    while (!m_closed) {
        json m;
        {
            std::unique_lock lock(m_queueMutex);
            m_queueCv.wait(lock, [&] { return m_closed || !m_queue.empty(); });
            if (m_closed) break;
            m = std::move(m_queue.front());
            m_queue.pop_front();
        }
        try {
            HandleMessage(m);
        } catch (const std::exception& e) {
            RC_LOGW("Error handling %s: %s", m.value("type", "?").c_str(), e.what());
        }
    }
}

void ClientSession::HandleMessage(const json& m) {
    const std::string type = m.value("type", "");
    if (type == "_tick") {
        // Steam VR launch timeout.
        if (m_steamVrLaunchNs && rc::NowNs() - m_steamVrLaunchNs > 90'000'000'000LL) {
            m_steamVrLaunchNs = 0;
            NotifyVrState("error", "steamvr", "", "SteamVR did not start. Check SteamVR on your PC.");
        }
        // Server statistics for the headset's performance overlay.
        json stats{{"type", rc::msg::kServerStats}};
        {
            std::lock_guard lock(m_mutex);
            if (m_vr) {
                auto s = m_vr->Stats();
                stats["vr"] = {{"fps", s.fps}, {"encodeMs", s.encodeMs}, {"mbps", s.mbps},
                               {"bitrateKbps", s.bitrateKbps}, {"framesLost", s.framesLost}};
            }
            json desk = json::array();
            for (auto& [id, d] : m_desktop) {
                auto s = d->Stats();
                desk.push_back({{"streamId", id}, {"fps", s.fps}, {"encodeMs", s.encodeMs}, {"mbps", s.mbps},
                                {"bitrateKbps", s.bitrateKbps}});
            }
            stats["desktop"] = desk;
        }
        stats["rttMs"] = m_connection->Stats().rttMs;
        SendControl(stats);
        return;
    }
    if (type == "_monitors") return SendMonitors();
    if (type == "_audio") return UpdateAudio();
    if (type == rc::msg::kClientInfo) {
        ApplyClientInfo(m);
        SendServerInfo();
        SendMonitors();
        StartVr();
        UpdateAudio();
        return;
    }
    if (type == rc::msg::kUpdateSettings) {
        ClientStreamSettings before = StreamSettings();
        ApplyClientInfo(json{{"settings", m.value("settings", json::object())}});
        ClientStreamSettings after = StreamSettings();
        const bool videoChanged = before.codec != after.codec || before.tenBit != after.tenBit ||
                                  before.vrQuality != after.vrQuality || before.frameGeneration != after.frameGeneration || before.refreshRate != after.refreshRate ||
                                  before.vrBitrateMbps != after.vrBitrateMbps ||
                                  before.adaptiveBitrate != after.adaptiveBitrate;
        if (videoChanged) {
            StopVr();
            StartVr();
        }
        if (before.codec != after.codec || before.tenBit != after.tenBit || before.desktopFps != after.desktopFps ||
            before.desktopBitrateMbps != after.desktopBitrateMbps) {
            json monitors = json::array();
            {
                std::lock_guard lock(m_mutex);
                for (auto& [id, d] : m_desktop) monitors.push_back(d->Monitor().id);
            }
            if (!monitors.empty()) {
                StopDesktop();
                StartDesktop(json{{"monitors", monitors}});
            }
        }
        if (before.streamAudio != after.streamAudio || before.micPassthrough != after.micPassthrough) UpdateAudio();
        return;
    }
    if (type == rc::msg::kStartDesktop) return StartDesktop(m);
    if (type == rc::msg::kStopDesktop) return StopDesktop();
    if (type == "desktop.active") {
        const std::string active = m.value("monitorId", "");
        std::lock_guard lock(m_mutex);
        for (auto& [id, d] : m_desktop) d->SetActive(active.empty() || d->Monitor().id == active);
        return;
    }
    if (type == "desktop.keyframe") {
        std::lock_guard lock(m_mutex);
        for (auto& [id, d] : m_desktop) d->RequestKeyframe();
        if (m_vr) m_vr->RequestKeyframe();
        return;
    }
    if (type == rc::msg::kLaunchSteamVr) return LaunchSteamVr();
    if (type == "vr.restartSteamVR") {
        steamvr::Restart();
        m_steamVrLaunchNs = rc::NowNs();
        NotifyVrState("launching", "steamvr", "");
        return;
    }
    if (type == rc::msg::kStartVr) return StartVr();
    if (type == rc::msg::kStopVr) {
        // The headset left the VR view. The game keeps running, so don't end the stream.
        if (m_vr) m_vr->SetPaused(true);
        return;
    }
    if (type == rc::msg::kRecenter) {
        App::Get().Bridge().Recenter();
        return;
    }
    if (type == rc::msg::kVrPhase) {
        App::Get().Bridge().ShiftVsync(m.value("shiftNs", int64_t(0)));
        return;
    }
    if (type == rc::msg::kListGames) {
        auto games = App::Get().Games().Scan();
        json list = json::array();
        for (const auto& g : games)
            list.push_back({{"id", g.id}, {"name", g.name}, {"source", g.source}, {"hasImage", !g.image.empty()},
                            {"lastPlayed", g.lastPlayed}});
        SendControl({{"type", rc::msg::kGames}, {"games", list}});
        return;
    }
    if (type == "games.image") {
        const std::string id = m.value("id", "");
        auto bytes = App::Get().Games().Image(id);
        if (bytes.size() > 400 * 1024) bytes.clear();  // keep the control channel snappy
        SendControl({{"type", "games.image"}, {"id", id}, {"data", Base64Encode(bytes.data(), bytes.size())}});
        return;
    }
    if (type == rc::msg::kLaunchGame) {
        const std::string id = m.value("id", "");
        const bool ok = App::Get().Games().Launch(id);
        SendControl({{"type", rc::msg::kNotice}, {"level", ok ? "info" : "error"},
                     {"text", ok ? "Launching game…" : "Could not launch the game"}});
        if (ok && !(m_vr && m_vr->Running())) NotifyVrState("launching", "steamvr", "");  // see LaunchSteamVr
        return;
    }
    if (type == rc::msg::kStats) {
        std::lock_guard lock(m_mutex);
        m_clientStats = m.value("stats", json::object());
        return;
    }
}

void ClientSession::ApplyClientInfo(const json& m) {
    std::lock_guard lock(m_mutex);
    if (m.contains("display")) {
        const auto& d = m["display"];
        m_caps.recommendedEyeWidth = d.value("eyeWidth", m_caps.recommendedEyeWidth);
        m_caps.recommendedEyeHeight = d.value("eyeHeight", m_caps.recommendedEyeHeight);
        m_caps.ipd = d.value("ipd", m_caps.ipd);
        if (d.contains("refreshRates")) m_caps.refreshRates = d["refreshRates"].get<std::vector<float>>();
        if (d.contains("fov") && d["fov"].size() == 2)
            for (int e = 0; e < 2; e++)
                for (int k = 0; k < 4; k++) m_caps.fovTan[e][k] = d["fov"][e][k].get<float>();
    }
    if (m.contains("decoders")) {
        const auto& d = m["decoders"];
        m_caps.h264 = d.value("h264", true);
        m_caps.hevc = d.value("hevc", true);
        m_caps.hevc10 = d.value("hevc10", false);
        m_caps.av1 = d.value("av1", false);
        m_caps.av110 = d.value("av110", false);
        m_caps.maxDecodeWidth = d.value("maxWidth", 4096u);
        m_caps.maxDecodeHeight = d.value("maxHeight", 4096u);
    }
    if (m.contains("settings")) {
        const auto& s = m["settings"];
        m_settings.codec = s.value("codec", m_settings.codec);
        m_settings.tenBit = s.value("tenBit", m_settings.tenBit);
        m_settings.vrQuality = s.value("vrQuality", m_settings.vrQuality);
        m_settings.frameGeneration = s.value("frameGeneration", m_settings.frameGeneration);
        m_settings.refreshRate = s.value("refreshRate", m_settings.refreshRate);
        m_settings.vrBitrateMbps = s.value("vrBitrateMbps", m_settings.vrBitrateMbps);
        m_settings.desktopBitrateMbps = s.value("desktopBitrateMbps", m_settings.desktopBitrateMbps);
        m_settings.desktopFps = s.value("desktopFps", m_settings.desktopFps);
        m_settings.adaptiveBitrate = s.value("adaptiveBitrate", m_settings.adaptiveBitrate);
        m_settings.streamAudio = s.value("streamAudio", m_settings.streamAudio);
        m_settings.micPassthrough = s.value("micPassthrough", m_settings.micPassthrough);
    }
}

rc::VideoCodec ClientSession::ChooseCodec(bool forVr) const {
    const EncoderCaps& enc = App::Get().Caps();
    const ClientCaps& dec = m_caps;
    const bool tenBit = m_settings.tenBit;
    auto ok = [&](rc::VideoCodec c) {
        switch (c) {
            case rc::VideoCodec::H264: return enc.h264 && dec.h264 && !tenBit;
            case rc::VideoCodec::HEVC: return enc.hevc && dec.hevc && (!tenBit || (enc.hevc10 && dec.hevc10));
            case rc::VideoCodec::AV1: return enc.av1 && dec.av1 && (!tenBit || (enc.av110 && dec.av110));
        }
        return false;
    };
    std::string pref = m_settings.codec;
    if (pref == "auto") pref = SettingsStore::Get().Snapshot().codec;
    if (pref == "av1" && ok(rc::VideoCodec::AV1)) return rc::VideoCodec::AV1;
    if (pref == "h264" && (enc.h264 && dec.h264)) return rc::VideoCodec::H264;
    if (pref == "hevc" && ok(rc::VideoCodec::HEVC)) return rc::VideoCodec::HEVC;
    // Auto: HEVC is the best latency/quality trade-off on Quest; AV1 when HEVC is missing.
    if (ok(rc::VideoCodec::HEVC)) return rc::VideoCodec::HEVC;
    if (ok(rc::VideoCodec::AV1)) return rc::VideoCodec::AV1;
    (void)forVr;
    return rc::VideoCodec::H264;
}

// ---------------------------------------------------------------------------

void ClientSession::SendServerInfo() {
    const Settings s = SettingsStore::Get().Snapshot();
    const EncoderCaps& caps = App::Get().Caps();
    const auto route = audio::ResolveRoute(s.audioDevices);
    const auto svr = steamvr::SteamVrPath();
    SendControl({
        {"type", rc::msg::kServerInfo},
        {"pcName", s.pcName},
        {"serverId", s.serverId},
        {"version", App::Get().Version()},
        {"os", WindowsVersion()},
        {"gpu", App::Get().Gpu().name},
        {"vrBitrateMbps", s.vrBitrateMbps},
        {"encoder",
         {{"backend", caps.backend}, {"h264", caps.h264}, {"hevc", caps.hevc}, {"av1", caps.av1},
          {"hevc10", caps.hevc10}, {"av110", caps.av110}}},
        {"steamvr",
         {{"installed", svr.has_value()}, {"running", steamvr::IsRunning()},
          {"driverRegistered", steamvr::IsDriverRegistered()}}},
        {"openxr", {{"rincastActive", openxr::IsRinCastActive()}}},
        {"audioDriver", {{"speaker", route.speaker != nullptr}, {"microphone", route.micFeed != nullptr},
                         {"devices", route.micSet.empty() ? route.speakerSet : route.micSet}}},
        {"account", s.cloudUserName},
        {"remote", m_remote},
        {"vrState", m_vrState},
    });
}

void ClientSession::SendMonitors() {
    auto monitors = EnumerateMonitors();
    json list = json::array();
    for (const auto& mon : monitors)
        list.push_back({{"id", mon.id},
                        {"name", mon.name},
                        {"index", mon.index},
                        {"width", mon.width},
                        {"height", mon.height},
                        {"primary", mon.primary},
                        {"refreshHz", mon.refreshHz}});
    {
        std::lock_guard lock(m_mutex);
        m_monitors = monitors;
    }
    SendControl({{"type", rc::msg::kMonitors}, {"monitors", list}});
}

void ClientSession::OnMonitorsChanged() {
    std::lock_guard lock(m_queueMutex);
    m_queue.push_back(json{{"type", "_monitors"}});
    m_queueCv.notify_one();
}

void ClientSession::OnSettingsChanged() {
    std::lock_guard lock(m_queueMutex);
    m_queue.push_back(json{{"type", rc::msg::kUpdateSettings}, {"settings", json::object()}});
    m_queue.push_back(json{{"type", "_audio"}});
    m_queueCv.notify_one();
}

void ClientSession::StartDesktop(const json& m) {
    StopDesktop();
    const Settings pc = SettingsStore::Get().Snapshot();
    std::vector<MonitorInfo> monitors = EnumerateMonitors();
    std::vector<std::string> wanted;
    if (m.contains("monitors") && m["monitors"].is_array())
        for (const auto& id : m["monitors"]) wanted.push_back(id.get<std::string>());

    ClientCaps caps;
    ClientStreamSettings st;
    rc::VideoCodec codec;
    {
        std::lock_guard lock(m_mutex);
        m_monitors = monitors;
        caps = m_caps;
        st = m_settings;
        codec = ChooseCodec(false);
    }
    std::vector<const MonitorInfo*> selected;
    for (const auto& mon : monitors)
        if (wanted.empty() || std::find(wanted.begin(), wanted.end(), mon.id) != wanted.end()) selected.push_back(&mon);
    if (selected.empty()) return;

    float fps = st.desktopFps > 0 ? float(st.desktopFps) : (pc.desktopFps > 0 ? float(pc.desktopFps) : st.refreshRate);
    fps = std::clamp(fps, 30.f, 120.f);
    const int totalMbps = st.desktopBitrateMbps > 0 ? st.desktopBitrateMbps : pc.desktopBitrateMbps;
    double totalPixels = 0;
    for (auto* mon : selected) totalPixels += double(mon->width) * mon->height;

    const uint32_t maxW = std::min<uint32_t>(caps.maxDecodeWidth, codec == rc::VideoCodec::H264 ? 4096 : 8192);
    const uint32_t maxH = std::min<uint32_t>(caps.maxDecodeHeight, codec == rc::VideoCodec::H264 ? 4096 : 8192);

    json streams = json::array();
    std::map<uint8_t, std::unique_ptr<DesktopStream>> created;
    for (auto* mon : selected) {
        double w = mon->width, h = mon->height;
        double scale = 1.0;
        if (pc.desktopMaxHeight > 0 && h > pc.desktopMaxHeight) scale = pc.desktopMaxHeight / h;
        scale = std::min({scale, maxW / w, maxH / h});
        DesktopStreamConfig cfg;
        cfg.codec = codec;
        cfg.tenBit = st.tenBit && codec != rc::VideoCodec::H264;
        cfg.width = Align(uint32_t(w * scale), 2);
        cfg.height = Align(uint32_t(h * scale), 2);
        cfg.fps = fps;
        cfg.bitrateKbps = uint32_t(totalMbps * 1000.0 * (double(mon->width) * mon->height / totalPixels));
        cfg.bitrateKbps = std::max<uint32_t>(cfg.bitrateKbps, 5000);
        // Internet paths lose packets and have no retransmissions: recover more of them locally.
        cfg.fecPercent = m_slowPath ? std::max(pc.fecPercent, 12) : pc.fecPercent;
        cfg.pace = m_slowPath;
        cfg.adaptiveQuant = pc.adaptiveQuantization;
        // Over the internet a fixed bitrate only fills queues or loses frames: always adapt.
        cfg.adaptive = m_slowPath || (st.adaptiveBitrate && pc.adaptiveBitrate);
        // Over the internet the upload is often far below the LAN bitrate: start low and
        // let the bitrate control find what the path carries, instead of filling queues.
        if (m_slowPath) cfg.startBitrateKbps = std::min<uint32_t>(cfg.bitrateKbps, std::max<uint32_t>(3000, 8000 / uint32_t(selected.size())));
        const uint8_t streamId = uint8_t(rc::kFirstMonitorStreamId + mon->index);
        if (streamId >= rc::kMaxStreams) continue;
        auto ds = std::make_unique<DesktopStream>(m_connection.get(), streamId, *mon, cfg);
        streams.push_back(ds->Describe());
        created[streamId] = std::move(ds);
    }
    SendControl({{"type", rc::msg::kStreamConfig}, {"kind", "desktop"}, {"streams", streams}});
    for (auto& [id, ds] : created) ds->Start();
    std::lock_guard lock(m_mutex);
    m_desktop = std::move(created);
}

void ClientSession::StopDesktop() {
    std::map<uint8_t, std::unique_ptr<DesktopStream>> old;
    {
        std::lock_guard lock(m_mutex);
        old.swap(m_desktop);
    }
    old.clear();
}

void ClientSession::StartVr() {
    if (m_vr) {
        m_vr->SetPaused(false);
        return;
    }
    const Settings pc = SettingsStore::Get().Snapshot();
    ClientCaps caps;
    ClientStreamSettings st;
    rc::VideoCodec codec;
    {
        std::lock_guard lock(m_mutex);
        caps = m_caps;
        st = m_settings;
        codec = ChooseCodec(true);
    }
    // Per-eye resolution from the quality preset, limited by codec/decoder.
    // Internet uploads rarely carry the higher presets: their pixels would only add blur.
    const float scale = m_slowPath ? std::min(QualityScale(st.vrQuality), QualityScale("medium")) : QualityScale(st.vrQuality);
    double eyeW = caps.recommendedEyeWidth * scale;
    double eyeH = caps.recommendedEyeHeight * scale;
    const double maxTotalW = std::min<double>(caps.maxDecodeWidth, codec == rc::VideoCodec::H264 ? 4096 : 8192);
    const double maxH = std::min<double>(caps.maxDecodeHeight, codec == rc::VideoCodec::H264 ? 4096 : 8192);
    const double limit = std::min(maxTotalW / (eyeW * 2), maxH / eyeH);
    if (limit < 1.0) {
        eyeW *= limit;
        eyeH *= limit;
    }
    rcvr::DisplayConfig dc{};
    dc.eyeWidth = Align(uint32_t(eyeW), 32);
    dc.eyeHeight = Align(uint32_t(eyeH), 32);
    if (dc.eyeWidth * 2 > maxTotalW) dc.eyeWidth -= 32;
    if (dc.eyeHeight > maxH) dc.eyeHeight -= 32;
    float rate = st.refreshRate;
    if (!caps.refreshRates.empty() &&
        std::find(caps.refreshRates.begin(), caps.refreshRates.end(), rate) == caps.refreshRates.end())
        rate = caps.refreshRates.back();
    dc.refreshRate = rate;
    dc.ipd = caps.ipd;
    std::memcpy(dc.fovTan, caps.fovTan, sizeof(dc.fovTan));
    strncpy_s(dc.headsetModel, m_hello.model.empty() ? "Quest" : m_hello.model.c_str(), _TRUNCATE);
    strncpy_s(dc.headsetSerial, m_hello.deviceId.c_str(), _TRUNCATE);

    auto& bridge = App::Get().Bridge();
    const bool needsRestart = bridge.Configure(dc);
    bridge.SetHeadsetConnected(true);

    VrStreamConfig cfg;
    cfg.codec = codec;
    cfg.tenBit = st.tenBit && codec != rc::VideoCodec::H264;
    cfg.eyeWidth = dc.eyeWidth;
    cfg.eyeHeight = dc.eyeHeight;
    cfg.fps = rate;
    cfg.bitrateKbps = uint32_t((st.vrBitrateMbps > 0 ? st.vrBitrateMbps : pc.vrBitrateMbps) * 1000);
    if (m_slowPath) cfg.bitrateKbps = std::min<uint32_t>(cfg.bitrateKbps, 60000);
    cfg.fecPercent = m_slowPath ? std::max(pc.fecPercent, 12) : pc.fecPercent;
    cfg.pace = m_slowPath;
    cfg.adaptive = m_slowPath || (st.adaptiveBitrate && pc.adaptiveBitrate);  // see above
    if (m_slowPath) cfg.startBitrateKbps = std::min<uint32_t>(cfg.bitrateKbps, 12000);
    cfg.foveated = pc.foveatedEncoding;
    cfg.adaptiveQuant = pc.adaptiveQuantization;
    cfg.twoPass = pc.twoPassEncoding;
    cfg.motionField = st.frameGeneration;
    RC_LOGI("VR stream: %s bitrate, up to %u kbps%s", cfg.adaptive ? "adaptive" : "fixed", cfg.bitrateKbps,
            cfg.startBitrateKbps ? (", starting at " + std::to_string(cfg.startBitrateKbps) + " kbps").c_str() : "");

    std::weak_ptr<ClientSession> weak = shared_from_this();
    auto vr = std::make_unique<VrStream>(m_connection.get(), &bridge, cfg,
                                         [weak](const std::string& state, const std::string& producer,
                                                const std::string& app) {
                                             if (auto self = weak.lock()) self->NotifyVrState(state, producer, app);
                                         });
    SendControl({{"type", rc::msg::kStreamConfig},
                 {"kind", "vr"},
                 {"streams",
                  json::array({{{"streamId", rc::kVrStreamId},
                                {"eyeWidth", cfg.eyeWidth},
                                {"eyeHeight", cfg.eyeHeight},
                                {"fps", cfg.fps},
                                {"codec", int(cfg.codec)},
                                {"tenBit", cfg.tenBit}}})}});
    vr->Start();
    {
        std::lock_guard lock(m_mutex);
        m_vr = std::move(vr);
    }
    if (needsRestart) {
        SendControl({{"type", rc::msg::kNotice},
                     {"level", "warning"},
                     {"action", "vr.restartSteamVR"},
                     {"text", "Restart SteamVR to use the new resolution."}});
    }
}

void ClientSession::StopVr() {
    std::unique_ptr<VrStream> old;
    {
        std::lock_guard lock(m_mutex);
        old.swap(m_vr);
    }
    old.reset();
}

void ClientSession::LaunchSteamVr() {
    if (!steamvr::SteamVrPath()) {
        NotifyVrState("error", "steamvr", "", "SteamVR is not installed on this PC.");
        return;
    }
    if (SettingsStore::Get().Snapshot().autoRegisterSteamVrDriver) steamvr::RegisterDriver();
    StartVr();
    // Already streaming, for example when coming back from the desktop: the stream reports
    // only changes, so nothing would ever take the state back from "launching".
    if (m_vr && m_vr->Running()) {
        m_vr->RequestKeyframe();
        return;
    }
    m_steamVrLaunchNs = rc::NowNs();
    NotifyVrState("launching", "steamvr", "");
    {
        std::lock_guard lock(m_mutex);
        if (m_vr) m_vr->RequestKeyframe();
    }
    if (!steamvr::IsRunning()) {
        steamvr::Launch();
    } else if (App::Get().Bridge().ActiveProducer() != rcvr::ProducerKind::SteamVR) {
        // SteamVR is running without our HMD (started before the headset connected
        // or with another driver): restart so it picks up RinCast.
        steamvr::Restart();
    }
}

void ClientSession::NotifyVrState(const std::string& state, const std::string& producer, const std::string& app,
                                  const std::string& error) {
    // While SteamVR is starting, "waiting" means "still launching" to the headset.
    std::string effective = state;
    if (state == "running") m_steamVrLaunchNs = 0;
    if (state == "waiting" && m_steamVrLaunchNs) effective = "launching";
    {
        std::lock_guard lock(m_mutex);
        if (m_vrState == effective && m_vrProducer == producer && m_vrApp == app && error.empty()) return;
        m_vrState = effective;
        m_vrProducer = producer;
        m_vrApp = app;
    }
    json msg{{"type", rc::msg::kVrState}, {"state", effective}, {"producer", producer}, {"app", app}};
    if (!error.empty()) msg["error"] = error;
    SendControl(msg);
    App::Get().OnVrStateChanged(effective, producer, app);
}

void ClientSession::UpdateAudio() {
    const Settings pc = SettingsStore::Get().Snapshot();
    ClientStreamSettings st = StreamSettings();
    const auto route = audio::ResolveRoute(pc.audioDevices);
    const bool streamAudio = pc.streamAudio && st.streamAudio;
    const bool mic = pc.micPassthrough && st.micPassthrough && route.micFeed;

    std::unique_ptr<AudioStreamer> oldAudio;
    std::unique_ptr<MicPlayer> oldMic;
    {
        std::lock_guard lock(m_mutex);
        oldAudio.swap(m_audio);
        oldMic.swap(m_mic);
    }
    oldAudio.reset();
    oldMic.reset();
    audio::RestoreDefaults();

    // "Also play on this PC" keeps the user's speakers: capture the default device instead.
    const wchar_t* speaker = pc.playOnPcToo ? nullptr : route.speaker;
    RC_LOGI("Audio (%s): speaker %s, microphone %s", pc.audioDevices.c_str(),
            speaker ? Narrow(speaker).c_str() : "default device",
            mic ? Narrow(route.mic).c_str() : "off");
    if (pc.switchDefaultAudio) audio::SwitchDefaults(route, streamAudio && speaker, mic);

    std::unique_ptr<AudioStreamer> a;
    std::unique_ptr<MicPlayer> mp;
    if (streamAudio) {
        a = std::make_unique<AudioStreamer>();
        a->Start(m_connection.get(), speaker, m_slowPath ? 96000 : 192000);
    }
    if (mic) {
        mp = std::make_unique<MicPlayer>();
        mp->Start(route.micFeed, route.mic);
    }
    std::lock_guard lock(m_mutex);
    m_audio = std::move(a);
    m_mic = std::move(mp);
}

json ClientSession::StatusJson() {
    json j{{"deviceId", m_hello.deviceId},
           {"deviceName", m_hello.deviceName},
           {"model", m_hello.model},
           {"address", m_connection->Remote().IpString()},
           {"remote", m_remote},
           {"vpn", m_vpn},
           // A Quest on a USB cable reaches the PC by IPv6 link-local only.
           {"usb", m_connection->Remote().IsLinkLocalV6()},
           {"connectedSeconds", (rc::NowNs() - m_connectedAtNs) / 1'000'000'000LL},
           {"rttMs", m_connection->Stats().rttMs}};
    std::lock_guard lock(m_mutex);
    // What the headset chose itself, which goes before this PC's settings.
    j["headsetBitrateMbps"] = m_settings.vrBitrateMbps;
    j["headsetCodec"] = m_settings.codec;
    j["vrState"] = m_vrState;
    j["vrProducer"] = m_vrProducer;
    j["vrApp"] = m_vrApp;
    if (m_vr) {
        auto s = m_vr->Stats();
        const auto& c = m_vr->Config();
        j["vr"] = {{"fps", s.fps}, {"encodeMs", s.encodeMs}, {"mbps", s.mbps}, {"bitrateKbps", s.bitrateKbps},
                   {"codec", CodecName(c.codec)}, {"eyeWidth", c.eyeWidth}, {"eyeHeight", c.eyeHeight},
                   {"refreshRate", c.fps}, {"latencyMs", s.clientLatencyMs}, {"decodeMs", s.clientDecodeMs}};
    }
    json desk = json::array();
    for (auto& [id, d] : m_desktop) {
        auto s = d->Stats();
        desk.push_back({{"name", d->Monitor().name}, {"fps", s.fps}, {"mbps", s.mbps}});
    }
    j["desktop"] = desk;
    j["client"] = m_clientStats;
    return j;
}

}  // namespace rcs
