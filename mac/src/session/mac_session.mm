#include "mac_session.h"

#include "../input/mac_input.h"
#include "rc/platform.h"

namespace rcm {

MacClientSession::MacClientSession(std::unique_ptr<rc::Connection> connection, const MacHelloInfo& hello)
    : m_connection(std::move(connection)),
      m_hello(hello) {
    m_connection->SetControlHandler([this](const std::string& text) {
        OnControl(text);
    });
    m_connection->SetPacketHandler([this](rc::PacketType type, const uint8_t* payload, size_t size) {
        OnPacket(type, payload, size);
    });
}

MacClientSession::~MacClientSession() {
    Close("destroyed");
}

void MacClientSession::Start() {
    RC_LOGI("Client session started for '%s' (%s)", m_hello.deviceName.c_str(), m_hello.model.c_str());
    SendState();
    SendMonitors();
}

void MacClientSession::Close(const std::string& reason) {
    if (m_closed.exchange(true)) return;
    RC_LOGI("Session with %s closed (%s)", m_hello.deviceName.c_str(), reason.c_str());

    StopDesktop();
    input::ReleaseAll();

    if (m_connection) {
        m_connection->SendDisconnect();
    }
}

void MacClientSession::HandleDatagram(const uint8_t* data, size_t size, const rc::NetAddress& from) {
    if (m_closed || !m_connection) return;
    m_connection->HandleDatagram(data, size, from);
}

void MacClientSession::Tick(int64_t nowNs) {
    if (m_closed || !m_connection) return;
    m_connection->Tick(nowNs);
}

void MacClientSession::SendControl(const json& message) {
    if (m_closed || !m_connection) return;
    m_connection->SendControl(message.dump());
}

void MacClientSession::SendState() {
    SendControl({
        {"type", rc::msg::kServerInfo},
        {"pcName", "Mac RinCast"},
        {"os", "macos"},
        {"version", "1.1.0"},
        {"desktopInput", true},
        {"remote", false},
        {"vrState", "idle"}
    });
}

void MacClientSession::SendMonitors() {
    auto monitors = EnumerateMacMonitors();
    json list = json::array();
    for (const auto& mon : monitors) {
        list.push_back({
            {"id", mon.id},
            {"name", mon.name},
            {"index", mon.index},
            {"width", mon.width},
            {"height", mon.height},
            {"primary", mon.primary},
            {"refreshHz", mon.refreshHz}
        });
    }

    {
        std::lock_guard lock(m_mutex);
        m_monitors = monitors;
    }

    SendControl({{"type", rc::msg::kMonitors}, {"monitors", list}});
}

void MacClientSession::StartDesktop(const json& m) {
    StopDesktop();

    std::vector<MacMonitorInfo> monitors = EnumerateMacMonitors();
    std::vector<std::string> wanted;
    if (m.contains("monitors") && m["monitors"].is_array()) {
        for (const auto& id : m["monitors"]) {
            wanted.push_back(id.get<std::string>());
        }
    }

    std::vector<const MacMonitorInfo*> selected;
    for (const auto& mon : monitors) {
        if (wanted.empty() || std::find(wanted.begin(), wanted.end(), mon.id) != wanted.end()) {
            selected.push_back(&mon);
        }
    }
    if (selected.empty() && !monitors.empty()) {
        selected.push_back(&monitors[0]);
    }
    if (selected.empty()) return;

    json streams = json::array();
    std::map<uint8_t, std::unique_ptr<MacDesktopStream>> created;

    for (const auto* mon : selected) {
        DesktopStreamConfig cfg;
        cfg.codec = m_codec;
        cfg.width = mon->width;
        cfg.height = mon->height;
        cfg.fps = mon->refreshHz > 0 ? static_cast<float>(mon->refreshHz) : 60.0f;
        cfg.bitrateKbps = 30000;
        cfg.fecPercent = 5;
        cfg.pace = false;

        const uint8_t streamId = static_cast<uint8_t>(rc::kFirstMonitorStreamId + mon->index);
        auto ds = std::make_unique<MacDesktopStream>(m_connection.get(), streamId, *mon, cfg);
        streams.push_back(ds->Describe());
        created[streamId] = std::move(ds);
    }

    SendControl({
        {"type", rc::msg::kStreamConfig},
        {"kind", "desktop"},
        {"streams", streams}
    });

    for (auto& [id, ds] : created) {
        ds->Start();
    }

    std::lock_guard lock(m_mutex);
    m_monitors = monitors;
    m_desktopStreams = std::move(created);
}

void MacClientSession::StopDesktop() {
    std::map<uint8_t, std::unique_ptr<MacDesktopStream>> old;
    {
        std::lock_guard lock(m_mutex);
        old.swap(m_desktopStreams);
    }
    for (auto& [id, ds] : old) {
        ds->Stop();
    }
}

void MacClientSession::OnControl(const std::string& text) {
    json m = json::parse(text, nullptr, false);
    if (!m.is_object()) return;

    std::string type = m.value("type", "");
    if (type == rc::msg::kClientInfo) {
        if (m.contains("hevc") && m["hevc"].get<bool>()) {
            m_codec = rc::VideoCodec::HEVC;
        } else {
            m_codec = rc::VideoCodec::H264;
        }
        SendMonitors();
    } else if (type == rc::msg::kStartDesktop) {
        StartDesktop(m);
    } else if (type == rc::msg::kStopDesktop) {
        StopDesktop();
    } else if (type == rc::msg::kMouseButton) {
        if (m_inputAllowed) {
            input::MouseButton(m.value("button", 0), m.value("down", false));
        }
    } else if (type == rc::msg::kMouseWheel) {
        if (m_inputAllowed) {
            input::MouseWheel(m.value("dx", 0.0f), m.value("dy", 0.0f));
        }
    } else if (type == rc::msg::kKey) {
        if (m_inputAllowed) {
            input::Key(static_cast<uint16_t>(m.value("vk", 0)), m.value("down", false));
        }
    } else if (type == rc::msg::kText) {
        if (m_inputAllowed) {
            input::Text(m.value("text", ""));
        }
    } else if (type == "ping") {
        SendControl({{"type", "pong"}, {"echo", m.value("echo", 0)}});
    }
}

void MacClientSession::OnPacket(rc::PacketType type, const uint8_t* payload, size_t size) {
    if (type == rc::PacketType::Pointer) {
        if (size < sizeof(rc::PointerPayload)) return;
        rc::PointerPayload p;
        std::memcpy(&p, payload, sizeof(p));
        if (!p.visible || !m_inputAllowed) return;

        std::lock_guard lock(m_mutex);
        auto it = m_desktopStreams.find(p.monitorStreamId);
        if (it != m_desktopStreams.end()) {
            input::MoveMouse(it->second->Monitor(), p.u, p.v);
        }
    } else if (type == rc::PacketType::VideoFeedback) {
        if (size < sizeof(rc::VideoFeedbackPayload)) return;
        rc::VideoFeedbackPayload fb;
        std::memcpy(&fb, payload, sizeof(fb));

        std::lock_guard lock(m_mutex);
        auto it = m_desktopStreams.find(fb.streamId);
        if (it != m_desktopStreams.end()) {
            it->second->OnFeedback(fb);
        }
    }
}

}  // namespace rcm
