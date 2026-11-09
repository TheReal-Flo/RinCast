#include "mac_stream_server.h"

#include <cstring>

#include "rc/platform.h"
#include "rc/protocol.h"

namespace rcm {

namespace {

std::string RandomNonce() {
    uint8_t n[16];
    rc::RandomBytes(n, sizeof(n));
    return rc::HexEncode(n, sizeof(n));
}

}  // namespace

MacStreamServer::MacStreamServer() {
    uint8_t idBytes[16];
    rc::RandomBytes(idBytes, sizeof(idBytes));
    m_serverId = rc::HexEncode(idBytes, sizeof(idBytes));
}

MacStreamServer::~MacStreamServer() {
    Stop();
}

bool MacStreamServer::Start(const std::string& serverName) {
    m_serverName = serverName;
    rc::NetInit();

    if (!m_socket.Open(rc::kStreamPort)) {
        RC_LOGW("Stream port %u busy, using ephemeral port", rc::kStreamPort);
        if (!m_socket.Open(0)) return false;
    }
    m_socket.SetBuffers(2 << 20, 8 << 20);

    if (!m_discovery.Open(rc::kDiscoveryPort, true, true)) {
        RC_LOGW("Discovery port %u busy", rc::kDiscoveryPort);
    }

    m_running = true;
    m_receiveThread = std::thread([this] { ReceiveLoop(); });
    m_discoveryThread = std::thread([this] { DiscoveryLoop(); });

    RC_LOGI("Mac RinCast stream server running on UDP %u (discovery on %u)",
            m_socket.LocalPort(), rc::kDiscoveryPort);
    return true;
}

void MacStreamServer::Stop() {
    if (!m_running) return;
    m_running = false;

    DisconnectActive("server_shutdown");
    m_socket.Close();
    m_discovery.Close();

    if (m_receiveThread.joinable()) m_receiveThread.join();
    if (m_discoveryThread.joinable()) m_discoveryThread.join();
}

std::shared_ptr<MacClientSession> MacStreamServer::ActiveSession() {
    std::lock_guard lock(m_mutex);
    return m_session;
}

void MacStreamServer::DisconnectActive(const std::string& reason) {
    std::shared_ptr<MacClientSession> s;
    {
        std::lock_guard lock(m_mutex);
        s = std::move(m_session);
    }
    if (s) {
        s->Close(reason);
    }
}

std::vector<uint8_t> MacStreamServer::MakeAnnounce(uint32_t nonce) {
    rc::DiscoveryAnnounce a;
    a.serverId = m_serverId;
    a.name = m_serverName;
    a.version = "1.1.0";
    a.os = "macOS";
    a.gpu = "Apple Silicon / Metal";
    a.streamPort = m_socket.LocalPort();
    a.accountLinked = false;
    a.accountName = "";
    a.busy = ActiveSession() != nullptr;
    a.nonce = nonce;
    return rc::BuildAnnounce(a);
}

void MacStreamServer::DiscoveryLoop() {
    std::vector<uint8_t> buffer(2048);
    rc::NetAddress from;
    int64_t lastAnnounce = 0;

    while (m_running) {
        const int n = m_discovery.RecvFrom(buffer.data(), buffer.size(), from, 250);
        if (n > 0) {
            rc::DiscoveryProbe probe;
            if (rc::ParseProbe(buffer.data(), size_t(n), probe)) {
                auto reply = MakeAnnounce(probe.nonce);
                m_discovery.SendTo(from, reply.data(), reply.size());
            }
        }

        const int64_t now = rc::NowNs();
        if (now - lastAnnounce > 3'000'000'000LL) {
            lastAnnounce = now;
            auto announce = MakeAnnounce(0);
            for (const auto& itf : rc::ListInterfaces()) {
                m_discovery.SendTo(rc::NetAddress{itf.broadcast(), rc::kDiscoveryPort}, announce.data(), announce.size());
            }
            m_discovery.SendTo(rc::NetAddress::Broadcast(rc::kDiscoveryPort), announce.data(), announce.size());
        }
    }
}

void MacStreamServer::HandleHello(const uint8_t* data, size_t size, const rc::NetAddress& from) {
    rc::HandshakePacket hello;
    if (!rc::ParseHandshake(data, size, hello) || hello.type != rc::PacketType::Hello) return;

    json j = json::parse(hello.json, nullptr, false);
    if (!j.is_object()) return;

    MacHelloInfo info;
    info.deviceId = j.value("deviceId", "");
    info.deviceName = j.value("deviceName", "Headset");
    info.model = j.value("model", "");
    info.appVersion = j.value("appVersion", "");
    info.address = from;
    info.raw = j;

    const std::string clientNonce = j.value("nonce", "");
    const std::string pkHex = rc::HexEncode(hello.publicKey.data(), hello.publicKey.size());
    const int64_t now = rc::NowNs();

    std::unique_lock lock(m_mutex);

    auto& hs = m_handshakes[pkHex];
    if (hs.sessionId == 0) {
        hs.serverKeys = rc::crypto::KeyPair::Generate();
        do {
            rc::RandomBytes(&hs.sessionId, sizeof(hs.sessionId));
        } while (hs.sessionId == 0);
        hs.serverNonce = RandomNonce();
        hs.createdNs = now;
    }

    const auto keys = rc::crypto::DeriveSessionKeys(
        hs.serverKeys.secretKey,
        hello.publicKey,
        hello.publicKey,
        hs.serverKeys.publicKey,
        rc::SessionContext(clientNonce, hs.serverNonce, "")
    );

    json welcomeJson{
        {"serverId", m_serverId},
        {"name", m_serverName},
        {"version", "1.1.0"},
        {"nonce", hs.serverNonce},
        {"remote", false}
    };

    rc::HandshakePacket welcome;
    welcome.type = rc::PacketType::Welcome;
    welcome.sessionId = hs.sessionId;
    welcome.publicKey = hs.serverKeys.publicKey;
    welcome.json = welcomeJson.dump();
    auto welcomeBytes = rc::BuildHandshake(welcome);

    const size_t mtu = rc::kMaxDatagramLan;
    auto connection = std::make_unique<rc::Connection>(&m_socket, from, hs.sessionId, keys, true, mtu);
    auto session = std::make_shared<MacClientSession>(std::move(connection), info);

    m_handshakes.erase(pkHex);
    auto previous = std::move(m_session);
    m_session = session;
    lock.unlock();

    if (previous) {
        previous->Close("replaced");
    }

    m_socket.SendTo(from, welcomeBytes.data(), welcomeBytes.size());
    RC_LOGI("Headset %s connected from %s", info.deviceName.c_str(), from.ToString().c_str());

    session->Start();
}

void MacStreamServer::ReceiveLoop() {
    std::vector<uint8_t> buffer(2048);
    rc::NetAddress from;
    int64_t lastTick = 0;

    while (m_running) {
        const int n = m_socket.RecvFrom(buffer.data(), buffer.size(), from, 2);
        if (n > 0) {
            const uint8_t* d = buffer.data();
            if (rc::IsHandshakeDatagram(d, size_t(n))) {
                rc::PacketHeader h;
                std::memcpy(&h, d, sizeof(h));
                if (h.type == uint8_t(rc::PacketType::Hello)) {
                    HandleHello(d, size_t(n), from);
                }
            } else if (size_t(n) >= sizeof(rc::PacketHeader)) {
                std::shared_ptr<MacClientSession> s = ActiveSession();
                if (s) {
                    s->HandleDatagram(d, size_t(n), from);
                }
            }
        }

        const int64_t now = rc::NowNs();
        if (now - lastTick >= 2'000'000) {
            lastTick = now;
            std::shared_ptr<MacClientSession> s = ActiveSession();
            if (s) {
                s->Tick(now);
            }
        }
    }
}

}  // namespace rcm
