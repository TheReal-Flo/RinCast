#include "stream_server.h"

#include <cstring>
#include <set>

#include "../app.h"
#include "../session/client_session.h"
#include "../settings.h"
#include "rc/discovery.h"
#include "rc/handshake.h"
#include "rc/platform.h"
#include "rc/protocol.h"

namespace rcs {

namespace {

// Local (non-cloud) sessions are only accepted from private address space.
bool IsPrivateAddress(uint32_t ip) {
    const uint8_t a = uint8_t(ip >> 24), b = uint8_t(ip >> 16);
    if (a == 10 || a == 127) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 169 && b == 254) return true;
    if (a == 100 && b >= 64 && b <= 127) return true;  // CGNAT / Tailscale
    return false;
}

std::string RandomNonce() {
    uint8_t n[16];
    rc::RandomBytes(n, sizeof(n));
    return rc::HexEncode(n, sizeof(n));
}

}  // namespace

bool StreamServer::Start() {
    rc::NetInit();
    if (!m_socket.Open(rc::kStreamPort)) {
        RC_LOGW("Stream port %u busy, using an ephemeral port", rc::kStreamPort);
        if (!m_socket.Open(0)) return false;
    }
    m_socket.SetBuffers(2 << 20, 8 << 20);  // a small send buffer: queued data is only lag
    m_socket.SetTos(0xB8);  // DSCP EF: hint Wi-Fi WMM to use the video/voice queue
    if (!m_discovery.Open(rc::kDiscoveryPort, true, true)) RC_LOGW("Discovery port %u busy", rc::kDiscoveryPort);
    if (!m_discoveryV6.OpenV6(rc::kDiscoveryPort, true)) RC_LOGW("IPv6 discovery port %u busy", rc::kDiscoveryPort);
    m_running = true;
    m_receiveThread = std::thread([this] { ReceiveLoop(); });
    m_discoveryThread = std::thread([this] { DiscoveryLoop(); });
    if (m_discoveryV6.IsOpen()) m_discoveryV6Thread = std::thread([this] { DiscoveryV6Loop(); });
    RC_LOGI("Stream server listening on UDP %u", m_socket.LocalPort());
    return true;
}

bool StreamServer::StillAsking(const std::string& deviceId, int64_t withinNs) {
    std::lock_guard lock(m_mutex);
    const auto it = m_askedAt.find(deviceId);
    return it != m_askedAt.end() && rc::NowNs() - it->second < withinNs;
}

void StreamServer::Stop() {
    m_running = false;
    DisconnectActive("server_shutdown");
    m_socket.Close();
    m_discovery.Close();
    m_discoveryV6.Close();
    if (m_receiveThread.joinable()) m_receiveThread.join();
    if (m_discoveryThread.joinable()) m_discoveryThread.join();
    if (m_discoveryV6Thread.joinable()) m_discoveryV6Thread.join();
}

std::shared_ptr<ClientSession> StreamServer::ActiveSession() {
    std::lock_guard lock(m_mutex);
    return m_session;
}

void StreamServer::DisconnectActive(const std::string& reason) {
    std::shared_ptr<ClientSession> s;
    {
        std::lock_guard lock(m_mutex);
        s = std::move(m_session);
    }
    if (s) {
        s->Close(reason);
        App::Get().OnSessionEnded(s.get(), reason);
    }
}

void StreamServer::Approve(const std::string& deviceId, bool remember) {
    std::lock_guard lock(m_mutex);
    m_approved[deviceId] = rc::NowNs() + 120'000'000'000LL;
    m_denied.erase(deviceId);
    m_promptedAt.erase(deviceId);
    (void)remember;  // persisted by the App when the session starts
}

void StreamServer::Deny(const std::string& deviceId) {
    std::lock_guard lock(m_mutex);
    m_denied[deviceId] = rc::NowNs() + 60'000'000'000LL;
    m_approved.erase(deviceId);
}

void StreamServer::ExpectCloudSession(const std::string& sessionId, const ExpectedCloudSession& s) {
    std::lock_guard lock(m_mutex);
    m_cloudSessions[sessionId] = s;
}

void StreamServer::SendReject(const rc::NetAddress& to, const std::string& reason, const std::string& message) {
    rc::HandshakePacket p;
    p.type = rc::PacketType::Reject;
    p.json = json{{"reason", reason}, {"message", message}}.dump();
    auto bytes = rc::BuildHandshake(p);
    m_socket.SendTo(to, bytes.data(), bytes.size());
}

void StreamServer::HandleHello(const uint8_t* data, size_t size, const rc::NetAddress& from) {
    rc::HandshakePacket hello;
    if (!rc::ParseHandshake(data, size, hello) || hello.type != rc::PacketType::Hello) return;
    json j;
    try {
        j = json::parse(hello.json);
    } catch (...) {
        return;
    }
    HelloInfo info;
    info.deviceId = j.value("deviceId", "");
    info.deviceName = j.value("deviceName", "Headset");
    info.model = j.value("model", "");
    info.appVersion = j.value("appVersion", "");
    info.cloudSession = j.value("cloudSession", "");
    info.address = from;
    info.raw = j;
    if (info.deviceId.empty() || info.deviceId.size() > 64) return;
    const std::string clientNonce = j.value("nonce", "");
    const std::string pkHex = rc::HexEncode(hello.publicKey.data(), hello.publicKey.size());
    const Settings settings = SettingsStore::Get().Snapshot();
    const int64_t now = rc::NowNs();

    std::unique_lock lock(m_mutex);

    // Retransmitted Hello for an already-established session: resend Welcome.
    if (m_session && m_session->ClientPublicKeyHex() == pkHex) {
        auto welcome = m_session->WelcomePacket();
        lock.unlock();
        m_socket.SendTo(from, welcome.data(), welcome.size());
        return;
    }

    // Which headset is this, really? The ID is only a claim; the signature over this
    // connection's key is proof.
    {
        rc::crypto::Key deviceKey;
        rc::crypto::Signature sig;
        if (rc::HexDecode(j.value("deviceKey", ""), deviceKey.data(), deviceKey.size()) &&
            rc::HexDecode(j.value("deviceSig", ""), sig.data(), sig.size()) &&
            rc::crypto::VerifyConnection(deviceKey, hello.publicKey, clientNonce, sig))
            info.deviceKey = j.value("deviceKey", "");
    }

    // Authorization.
    std::string connectSecret;
    bool authorized = false;
    if (!info.cloudSession.empty()) {
        auto it = m_cloudSessions.find(info.cloudSession);
        if (it == m_cloudSessions.end() || it->second.expiresNs < now) {
            lock.unlock();
            RC_LOGW("Hello from %s for an unknown or expired remote session", from.ToString().c_str());
            SendReject(from, "invalid_session", "This remote session expired. Try connecting again.");
            return;
        }
        if (j.value("proof", "") != rc::HelloProof(it->second.connectSecret, hello.publicKey)) {
            lock.unlock();
            RC_LOGW("Hello from %s with a wrong remote-session proof", from.ToString().c_str());
            SendReject(from, "invalid_proof", "Authentication failed.");
            return;
        }
        connectSecret = it->second.connectSecret;
    } else {
        if (!IsPrivateAddress(from.ip)) {
            lock.unlock();
            SendReject(from, "remote_requires_account",
                       "Connections from outside your local network require signing in to a RinCast account.");
            return;
        }
    }
    // An account password or the account server alone must not unlock a PC: every path,
    // also over the internet, needs a headset this PC has allowed.
    if (auto d = m_denied.find(info.deviceId); d != m_denied.end() && d->second > now) {
        lock.unlock();
        SendReject(from, "denied", "The connection was declined on the PC.");
        return;
    }
    {
        const bool remote = !info.cloudSession.empty();
        bool trusted = !settings.requireApproval;
        for (const auto& t : settings.trustedDevices) {
            if (t.id != info.deviceId) continue;
            if (!t.publicKey.empty()) trusted = trusted || t.publicKey == info.deviceKey;
            // Entries from before identities: the first proof pins the key (see OnSessionStarted).
            // Over the internet there has to be a proof.
            else trusted = trusted || !remote || !info.deviceKey.empty();
        }
        // "Allow" applies to the headset that asked, not to anyone repeating its ID.
        if (auto a = m_approved.find(info.deviceId); a != m_approved.end() && a->second > now &&
            m_approvalKey[info.deviceId] == info.deviceKey)
            trusted = true;
        if (!trusted) {
            m_approvalKey[info.deviceId] = info.deviceKey;
            m_askedAt[info.deviceId] = now;
            auto& last = m_promptedAt[info.deviceId];
            const bool prompt = now - last > 15'000'000'000LL;
            if (prompt) last = now;
            lock.unlock();
            if (prompt && m_onApproval) m_onApproval(info);
            SendReject(from, "pending_approval", "Allow this headset in the RinCast Streamer on your PC.");
            return;
        }
        authorized = true;
    }

    // One headset at a time; the same device may reconnect and take over.
    if (m_session && m_session->DeviceId() != info.deviceId && now - m_session->LastReceiveNs() < 5'000'000'000LL) {
        const std::string busyWith = m_session->DeviceName();
        lock.unlock();
        SendReject(from, "busy", "This PC is streaming to " + busyWith + ".");
        return;
    }

    auto& hs = m_handshakes[pkHex];
    if (hs.sessionId == 0) {
        hs.serverKeys = rc::crypto::KeyPair::Generate();
        do {
            rc::RandomBytes(&hs.sessionId, sizeof(hs.sessionId));
        } while (hs.sessionId == 0);
        hs.serverNonce = RandomNonce();
        hs.createdNs = now;
    }
    // Garbage-collect stale handshakes.
    for (auto it = m_handshakes.begin(); it != m_handshakes.end();) {
        if (now - it->second.createdNs > 60'000'000'000LL && it->first != pkHex)
            it = m_handshakes.erase(it);
        else
            ++it;
    }

    const auto keys = rc::crypto::DeriveSessionKeys(hs.serverKeys.secretKey, hello.publicKey, hello.publicKey,
                                                    hs.serverKeys.publicKey,
                                                    rc::SessionContext(clientNonce, hs.serverNonce, connectSecret));

    json welcomeJson{
        {"serverId", settings.serverId},
        {"name", settings.pcName},
        {"version", App::Get().Version()},
        {"nonce", hs.serverNonce},
        {"accountName", settings.cloudUserName},
        {"remote", !connectSecret.empty()},
    };
    if (!connectSecret.empty()) welcomeJson["proof"] = rc::WelcomeProof(connectSecret, hs.serverKeys.publicKey, hello.publicKey);
    rc::HandshakePacket welcome;
    welcome.type = rc::PacketType::Welcome;
    welcome.sessionId = hs.sessionId;
    welcome.publicKey = hs.serverKeys.publicKey;
    welcome.json = welcomeJson.dump();
    auto welcomeBytes = rc::BuildHandshake(welcome);

    const bool remote = !connectSecret.empty();
    // A VPN needs no account, but its tunnel leaves less room per packet (Tailscale: 1280 bytes).
    const bool vpn = !remote && !from.v6 && rc::RoutesThroughTunnel(from.ip);
    const size_t mtu = remote || vpn ? rc::kMaxDatagramWan : rc::kMaxDatagramLan;
    auto connection = std::make_unique<rc::Connection>(&m_socket, from, hs.sessionId, keys, true, mtu);
    auto session = std::make_shared<ClientSession>(std::move(connection), info, pkHex, welcomeBytes, remote, vpn);
    m_handshakes.erase(pkHex);
    if (!info.cloudSession.empty()) m_cloudSessions.erase(info.cloudSession);
    m_approved.erase(info.deviceId);
    auto previous = std::move(m_session);
    m_session = session;
    lock.unlock();

    if (previous) {
        previous->Close("replaced");
        App::Get().OnSessionEnded(previous.get(), "replaced");
    }
    m_socket.SendTo(from, welcomeBytes.data(), welcomeBytes.size());
    RC_LOGI("Headset %s (%s) connected from %s%s", info.deviceName.c_str(), info.model.c_str(),
            from.ToString().c_str(), remote ? " [remote]" : "");
    (void)authorized;
    session->Start();
    App::Get().OnSessionStarted(session.get());
}

void StreamServer::ReceiveLoop() {
    std::vector<uint8_t> buffer(2048);
    rc::NetAddress from;
    int64_t lastTick = 0;
    while (m_running) {
        const int n = m_socket.RecvFrom(buffer.data(), buffer.size(), from, 2);
        if (n < 0) {
            if (!m_running) break;
            rc::SleepMs(5);
            continue;
        }
        if (n > 0) {
            const uint8_t* d = buffer.data();
            if (n >= 4 && std::memcmp(d, "RCRV", 4) == 0) {
                if (m_onRendezvous) m_onRendezvous(d, size_t(n), from);
            } else if (rc::IsHandshakeDatagram(d, size_t(n))) {
                rc::PacketHeader h;
                std::memcpy(&h, d, sizeof(h));
                if (h.type == uint8_t(rc::PacketType::Hello)) HandleHello(d, size_t(n), from);
            } else if (size_t(n) >= sizeof(rc::PacketHeader)) {
                std::shared_ptr<ClientSession> s = ActiveSession();
                if (s) s->HandleDatagram(d, size_t(n), from);
            }
        }
        const int64_t now = rc::NowNs();
        if (now - lastTick >= 2'000'000) {
            lastTick = now;
            std::shared_ptr<ClientSession> s = ActiveSession();
            if (s) {
                s->Tick(now);
                if (s->PeerDisconnected()) {
                    DisconnectActive("headset_disconnected");
                } else if (now - s->LastReceiveNs() > 8'000'000'000LL) {
                    DisconnectActive("timeout");
                }
            }
        }
    }
}

std::vector<uint8_t> StreamServer::MakeAnnounce(uint32_t nonce) {
    const Settings s = SettingsStore::Get().Snapshot();
    rc::DiscoveryAnnounce a;
    a.serverId = s.serverId;
    a.name = s.pcName;
    a.version = App::Get().Version();
    a.os = WindowsVersion();
    a.gpu = App::Get().Gpu().name;
    a.streamPort = m_socket.LocalPort();
    a.accountLinked = !s.cloudUserName.empty();
    a.accountName = s.cloudUserName;
    a.busy = ActiveSession() != nullptr;
    a.nonce = nonce;
    return rc::BuildAnnounce(a);
}

// Over a USB cable a Quest has only an IPv6 link-local address, and asks every device on the link
// (ff02::1). That group is joined on each adapter again as adapters come and go: plugging the
// cable in makes a new one.
void StreamServer::DiscoveryV6Loop() {
    std::vector<uint8_t> buffer(2048);
    rc::NetAddress from;
    std::set<uint32_t> joined;
    int64_t lastScan = 0;
    while (m_running) {
        const int64_t now = rc::NowNs();
        if (now - lastScan > 2'000'000'000LL) {
            lastScan = now;
            std::set<uint32_t> present;
            for (const auto& itf : rc::ListInterfacesV6()) {
                present.insert(itf.index);
                if (!joined.count(itf.index) && m_discoveryV6.JoinGroupV6(rc::NetAddress::AllNodesV6(0, itf.index), itf.index)) {
                    joined.insert(itf.index);
                    if (itf.usbNcm) RC_LOGI("USB network: listening on %s (%s)", itf.name.c_str(), itf.description.c_str());
                }
            }
            // Gone: joined anew if it comes back.
            for (auto it = joined.begin(); it != joined.end();) it = present.count(*it) ? std::next(it) : joined.erase(it);
        }
        const int n = m_discoveryV6.RecvFrom(buffer.data(), buffer.size(), from, 250);
        if (n < 0) {
            if (!m_running) break;
            rc::SleepMs(250);
            continue;
        }
        rc::DiscoveryProbe probe;
        if (n > 0 && from.IsLinkLocalV6() && rc::ParseProbe(buffer.data(), size_t(n), probe)) {
            auto reply = MakeAnnounce(probe.nonce);
            m_discoveryV6.SendTo(from, reply.data(), reply.size());
        }
    }
}

void StreamServer::DiscoveryLoop() {
    std::vector<uint8_t> buffer(2048);
    rc::NetAddress from;
    int64_t lastAnnounce = 0;
    auto makeAnnounce = [this](uint32_t nonce) { return MakeAnnounce(nonce); };
    while (m_running) {
        const int n = m_discovery.RecvFrom(buffer.data(), buffer.size(), from, 250);
        if (n < 0) {
            if (!m_running) break;
            rc::SleepMs(250);
            continue;
        }
        if (n > 0) {
            rc::DiscoveryProbe probe;
            if (rc::ParseProbe(buffer.data(), size_t(n), probe)) {
                auto reply = makeAnnounce(probe.nonce);
                m_discovery.SendTo(from, reply.data(), reply.size());
            }
        }
        const int64_t now = rc::NowNs();
        if (now - lastAnnounce > 3'000'000'000LL) {
            lastAnnounce = now;
            auto announce = makeAnnounce(0);
            // Directed broadcast on every interface plus the limited broadcast.
            for (const auto& itf : rc::ListInterfaces())
                m_discovery.SendTo(rc::NetAddress{itf.broadcast(), rc::kDiscoveryPort}, announce.data(), announce.size());
            m_discovery.SendTo(rc::NetAddress::Broadcast(rc::kDiscoveryPort), announce.data(), announce.size());
        }
    }
}

}  // namespace rcs
