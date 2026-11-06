#include "cloud_client.h"

#include <winhttp.h>

#include <cstring>

#include "../app.h"
#include "../net/stream_server.h"
#include "../session/client_session.h"
#include "../settings.h"
#include "rc/platform.h"
#include "rc/protocol.h"

#pragma comment(lib, "winhttp.lib")

namespace rcs {

static std::string ErrorMessage(const HttpResponse& r, const char* fallback) {
    if (r.status == 0) return "Can't reach the RinCast server. Check your internet connection.";
    const json j = r.Json();
    return j.value("message", std::string(fallback));
}

// ---------------------------------------------------------------------------

std::string CloudClient::BaseUrl() const {
    std::string url = SettingsStore::Get().Snapshot().cloudUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url + "/api/v1";
}

bool CloudClient::Start(StreamServer* server) {
    m_server = server;
    m_server->SetRendezvousHandler(
        [this](const uint8_t* d, size_t n, const rc::NetAddress& from) { HandleRendezvous(d, n, from); });
    m_running = true;
    m_thread = std::thread([this] { Run(); });
    return true;
}

void CloudClient::Stop() {
    m_running = false;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    m_portMapper.Stop();
}

bool CloudClient::SignedIn() const { return !SettingsStore::Get().Snapshot().cloudRefreshTokenProtected.empty(); }

json CloudClient::StatusJson() const {
    const Settings s = SettingsStore::Get().Snapshot();
    std::lock_guard lock(m_mutex);
    return {{"signedIn", !s.cloudRefreshTokenProtected.empty()},
            {"userName", s.cloudUserName},
            {"email", s.cloudEmail},
            {"server", s.cloudUrl},
            {"allowRemote", s.allowRemote},
            {"status", m_status},
            {"error", m_lastError},
            {"portMapping", m_portMapper.Status()}};
}

void CloudClient::Wake() {
    {
        std::lock_guard lock(m_mutex);
        m_registered = false;
    }
    m_cv.notify_all();
}

void CloudClient::StoreTokens(const json& tokens) {
    const std::string refresh = tokens.value("refreshToken", "");
    const json user = tokens.value("user", json::object());
    SettingsStore::Get().Update([&](Settings& s) {
        if (!refresh.empty()) s.cloudRefreshTokenProtected = ProtectString(refresh);
        if (user.contains("name")) s.cloudUserName = user.value("name", "");
        if (user.contains("email")) s.cloudEmail = user.value("email", "");
    });
    std::lock_guard lock(m_mutex);
    m_accessToken = tokens.value("accessToken", "");
    m_accessExpiresNs = rc::NowNs() + int64_t(tokens.value("expiresIn", 3600) - 60) * 1'000'000'000LL;
}

std::string CloudClient::SignIn(const std::string& email, const std::string& password) {
    const Settings s = SettingsStore::Get().Snapshot();
    auto r = HttpRequest("POST", BaseUrl() + "/auth/login",
                         json{{"email", email}, {"password", password}, {"deviceKind", "pc"}, {"deviceId", s.serverId},
                              {"deviceName", s.pcName}}
                             .dump(),
                         "", 15000);
    if (r.status != 200) return ErrorMessage(r, "Sign-in failed");
    StoreTokens(r.Json());
    Wake();
    return {};
}

std::string CloudClient::Register(const std::string& name, const std::string& email, const std::string& password) {
    const Settings s = SettingsStore::Get().Snapshot();
    auto r = HttpRequest("POST", BaseUrl() + "/auth/register",
                         json{{"name", name}, {"email", email}, {"password", password}, {"deviceKind", "pc"},
                              {"deviceId", s.serverId}, {"deviceName", s.pcName}}
                             .dump(),
                         "", 15000);
    if (r.status != 201 && r.status != 200) return ErrorMessage(r, "Could not create the account");
    StoreTokens(r.Json());
    Wake();
    return {};
}

void CloudClient::SignOut() {
    const Settings s = SettingsStore::Get().Snapshot();
    const std::string refresh = UnprotectString(s.cloudRefreshTokenProtected);
    if (!refresh.empty())
        HttpRequest("POST", BaseUrl() + "/auth/logout", json{{"refreshToken", refresh}}.dump(), "", 5000);
    SettingsStore::Get().Update([](Settings& st) {
        st.cloudRefreshTokenProtected.clear();
        st.cloudUserName.clear();
        st.cloudEmail.clear();
    });
    std::lock_guard lock(m_mutex);
    m_accessToken.clear();
    m_status = "signed_out";
    m_registered = false;
}

std::string CloudClient::ApproveHeadsetCode(const std::string& code) {
    if (!EnsureAccessToken()) return "Sign in first.";
    auto r = Api("POST", "/device/approve", json{{"userCode", code}});
    if (r.status != 200) return ErrorMessage(r, "That code didn't work");
    return {};
}

bool CloudClient::RefreshTokens() {
    const std::string refresh = UnprotectString(SettingsStore::Get().Snapshot().cloudRefreshTokenProtected);
    if (refresh.empty()) return false;
    auto r = HttpRequest("POST", BaseUrl() + "/auth/refresh", json{{"refreshToken", refresh}}.dump(), "", 15000);
    if (r.status == 401) {
        // Revoked or expired: sign out locally.
        SettingsStore::Get().Update([](Settings& s) {
            s.cloudRefreshTokenProtected.clear();
            s.cloudUserName.clear();
        });
        std::lock_guard lock(m_mutex);
        m_status = "signed_out";
        m_lastError = "Your session expired. Please sign in again.";
        return false;
    }
    if (r.status != 200) {
        std::lock_guard lock(m_mutex);
        m_status = "error";
        m_lastError = ErrorMessage(r, "Server error");
        return false;
    }
    StoreTokens(r.Json());
    return true;
}

bool CloudClient::EnsureAccessToken() {
    {
        std::lock_guard lock(m_mutex);
        if (!m_accessToken.empty() && rc::NowNs() < m_accessExpiresNs) return true;
    }
    return RefreshTokens();
}

HttpResponse CloudClient::Api(const std::string& method, const std::string& path, const json& body, int timeoutMs) {
    std::string token;
    {
        std::lock_guard lock(m_mutex);
        token = m_accessToken;
    }
    auto r = HttpRequest(method, BaseUrl() + path, body.is_null() ? "" : body.dump(), token, timeoutMs);
    if (r.status == 401 && RefreshTokens()) {
        std::lock_guard lock(m_mutex);
        token = m_accessToken;
        r = HttpRequest(method, BaseUrl() + path, body.is_null() ? "" : body.dump(), token, timeoutMs);
    }
    return r;
}

std::vector<std::string> CloudClient::LanAddresses() const {
    std::vector<std::string> out;
    for (const auto& itf : rc::ListInterfaces())
        if (!itf.isVirtual && (itf.ip >> 16) != ((169u << 8) | 254u)) out.push_back(rc::NetAddress{itf.ip, 0}.IpString());
    return out;
}

std::vector<std::string> CloudClient::LocalAddresses() const {
    std::vector<std::string> out;
    const uint16_t port = m_server ? m_server->Port() : rc::kStreamPort;
    // Only real LAN adapters: a headset that can reach one of these is on the same network.
    // VPN / virtual adapters (Tailscale, NordLynx, WSL, ...) would route the stream through a tunnel.
    for (const auto& itf : rc::ListInterfaces())
        if (!itf.isVirtual) out.push_back(rc::NetAddress{itf.ip, port}.ToString());
    // Global IPv6 addresses: directly reachable once both firewalls are opened by punching.
    if (m_server && m_server->Socket().DualStack())
        for (const auto& a : rc::ListGlobalIpv6(port)) out.push_back(a.ToString());
    // Public endpoint the router forwards to us (PCP / NAT-PMP / UPnP).
    rc::NetAddress mapped;
    if (m_portMapper.Mapped(mapped)) out.push_back(mapped.ToString());
    return out;
}

void CloudClient::RegisterPc() {
    const Settings s = SettingsStore::Get().Snapshot();
    auto r = Api("POST", "/pcs",
                 json{{"id", s.serverId}, {"name", s.pcName}, {"gpu", App::Get().Gpu().name}, {"os", WindowsVersion()},
                      {"version", App::Get().Version()}, {"localAddrs", LocalAddresses()}});
    std::lock_guard lock(m_mutex);
    if (r.status == 200) {
        m_registered = true;
        m_registeredName = s.pcName;
        m_status = "online";
        m_lastError.clear();
    } else {
        m_status = "error";
        m_lastError = ErrorMessage(r, "Could not register this PC");
    }
}

void CloudClient::Heartbeat() {
    const Settings s = SettingsStore::Get().Snapshot();
    const bool busy = m_server && m_server->ActiveSession() != nullptr;
    Api("POST", "/pcs/" + s.serverId + "/heartbeat", json{{"busy", busy}, {"localAddrs", LocalAddresses()}});
}

void CloudClient::Run() {
    int64_t lastHeartbeat = 0;
    int backoffMs = 1000;
    while (m_running) {
        const Settings s = SettingsStore::Get().Snapshot();
        if (s.cloudRefreshTokenProtected.empty() || !s.allowRemote) {
            m_portMapper.Stop();  // no remote access: no open port on the router
            std::unique_lock lock(m_mutex);
            if (s.cloudRefreshTokenProtected.empty()) m_status = "signed_out";
            m_cv.wait_for(lock, std::chrono::seconds(5));
            continue;
        }
        if (!EnsureAccessToken()) {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(backoffMs));
            backoffMs = std::min(backoffMs * 2, 60000);
            continue;
        }
        bool registered;
        {
            std::lock_guard lock(m_mutex);
            registered = m_registered && m_registeredName == s.pcName;
        }
        if (!registered) {
            RegisterPc();
            {
                std::lock_guard lock(m_mutex);
                registered = m_registered;
            }
            if (!registered) {
                std::unique_lock lock(m_mutex);
                m_cv.wait_for(lock, std::chrono::milliseconds(backoffMs));
                backoffMs = std::min(backoffMs * 2, 60000);
                continue;
            }
        }
        backoffMs = 1000;
        if (m_server) m_portMapper.Start(m_server->Port());  // no-op while running
        if (rc::NowNs() - lastHeartbeat > 30'000'000'000LL) {
            Heartbeat();
            lastHeartbeat = rc::NowNs();
        }
        // Long-poll for connection requests.
        auto r = Api("GET", "/pcs/" + s.serverId + "/events?wait=25", json(), 35000);
        if (r.status == 200) {
            for (const auto& ev : r.Json().value("events", json::array())) HandleEvent(ev);
        } else if (r.status == 404) {
            std::lock_guard lock(m_mutex);
            m_registered = false;  // PC was removed from the account: re-register
        } else {
            std::unique_lock lock(m_mutex);
            m_status = r.status == 0 ? "connecting" : "error";
            m_cv.wait_for(lock, std::chrono::seconds(3));
        }
    }
}

void CloudClient::HandleEvent(const json& ev) {
    const std::string type = ev.value("type", "");
    const json data = ev.value("data", json::object());
    if (type == "unlinked") {
        RC_LOGI("PC was unlinked from the account");
        SettingsStore::Get().Update([](Settings& s) {
            s.cloudRefreshTokenProtected.clear();
            s.cloudUserName.clear();
        });
        return;
    }
    if (type != "connect") return;
    const std::string sessionId = data.value("sessionId", "");
    ExpectedCloudSession expected;
    expected.connectSecret = data.value("connectSecret", "");
    expected.headsetId = data.value("headsetId", "");
    expected.headsetName = data.value("headsetName", "");
    expected.expiresNs = rc::NowNs() + 120'000'000'000LL;
    if (sessionId.empty() || expected.connectSecret.empty()) return;
    RC_LOGI("Remote connection request from %s", expected.headsetName.c_str());
    m_server->ExpectCloudSession(sessionId, expected);
    const json rv = data.value("rendezvous", json::object());
    std::thread(&CloudClient::RendezvousLoop, this, sessionId, data.value("relayToken", ""), rv.value("host", ""),
                rv.value("port", 0))
        .detach();
}

// Introduces this PC to the headset for one connect session: registers with the
// rendezvous (over IPv4 and IPv6 where available) together with every address we
// can be reached on, learns the headset's addresses, and sends punch packets to
// all of them so our NAT and firewall let the headset's Hello in. The headset
// does the same in the other direction; whichever path answers first carries the
// session. Nothing is ever relayed through the server.
void CloudClient::RendezvousLoop(std::string sessionId, std::string token, std::string host, int port) {
    std::vector<rc::NetAddress> servers;
    if (!host.empty() && port > 0)
        for (const auto& a : rc::NetAddress::ResolveAll(host, uint16_t(port)))
            if (!a.v6 || m_server->Socket().DualStack()) servers.push_back(a);
    if (servers.empty()) {
        RC_LOGW("Invalid rendezvous address %s:%d", host.c_str(), port);
        return;
    }
    const std::vector<std::string> candidates = LocalAddresses();
    RC_LOGI("Rendezvous via %s, offering %s", servers[0].ToString().c_str(), json(candidates).dump().c_str());
    {
        std::lock_guard lock(m_rvMutex);
        m_rvSession = sessionId;
        m_rvServers = servers;
        m_rvPeers.clear();
        m_rvRegistered = false;
    }
    const std::string msg = "RCRV" + json{{"op", "register"}, {"session", sessionId}, {"role", 0}, {"token", token},
                                          {"candidates", candidates}}
                                         .dump();
    rc::PacketHeader punch{};
    punch.version = uint8_t(rc::kProtocolVersion);
    punch.type = uint8_t(rc::PacketType::Punch);

    const int64_t start = rc::NowNs();
    int64_t lastRegister = 0, lastPunch = 0;
    while (m_running && rc::NowNs() - start < 30'000'000'000LL) {
        const int64_t now = rc::NowNs();
        std::vector<rc::NetAddress> peers;
        bool registered;
        {
            std::lock_guard lock(m_rvMutex);
            if (m_rvSession != sessionId) return;  // superseded by a newer request
            peers = m_rvPeers;
            registered = m_rvRegistered;
        }
        // Done once the headset is connected; its traffic keeps the path open.
        if (auto s = m_server->ActiveSession(); s && s->IsRemote() && now - start > 2'000'000'000LL) break;
        if (now - lastRegister > (registered ? 5'000'000'000LL : 300'000'000LL)) {
            for (const auto& sv : servers) m_server->Socket().SendTo(sv, msg.data(), msg.size());
            lastRegister = now;
        }
        if (!peers.empty() && now - lastPunch >= 100'000'000LL) {
            lastPunch = now;
            const bool early = now - start < 10'000'000'000LL;
            for (const auto& p : peers) {
                m_server->Socket().SendTo(p, &punch, sizeof(punch));
                // Routers that hand out ports sequentially give the headset's next mapping
                // a neighbouring port; a few guesses cost nothing.
                if (early && !p.v6 && !rc::IsPrivateLan(p.ip))
                    for (int d : {1, 2, 3, 4, -1}) {
                        rc::NetAddress q = p;
                        q.port = uint16_t(p.port + d);
                        m_server->Socket().SendTo(q, &punch, sizeof(punch));
                    }
            }
        }
        rc::SleepMs(25);
    }
    if (!m_server->ActiveSession()) {
        std::lock_guard lock(m_rvMutex);
        RC_LOGW("Rendezvous: no headset connection within 30 s (registered: %d, headset addresses: %zu)",
                int(m_rvRegistered), m_rvPeers.size());
    }
}

void CloudClient::HandleRendezvous(const uint8_t* data, size_t size, const rc::NetAddress& from) {
    json j;
    try {
        j = json::parse(reinterpret_cast<const char*>(data) + 4, reinterpret_cast<const char*>(data) + size);
    } catch (...) {
        return;
    }
    std::lock_guard lock(m_rvMutex);
    bool fromServer = false;
    for (const auto& sv : m_rvServers) fromServer |= sv == from;
    if (!fromServer || j.value("session", "") != m_rvSession) return;
    if (j.value("op", "") != "registered") return;
    if (!m_rvRegistered) RC_LOGI("Rendezvous: registered, the server sees us as %s", j.value("self", "?").c_str());
    m_rvRegistered = true;
    std::vector<std::string> list;
    for (const auto& p : j.value("peers", json::array()))
        if (p.is_string()) list.push_back(p.get<std::string>());
    if (j.contains("peer") && j["peer"].is_string()) list.push_back(j["peer"].get<std::string>());
    for (const auto& s : list) {
        rc::NetAddress a;
        if (!rc::NetAddress::ParseEndpoint(s, a) || (a.v6 && !m_server->Socket().DualStack())) continue;
        bool known = false;
        for (const auto& p : m_rvPeers) known |= p == a;
        if (!known && m_rvPeers.size() < 32) {
            m_rvPeers.push_back(a);
            RC_LOGI("Rendezvous: headset address %s", a.ToString().c_str());
        }
    }
}

}  // namespace rcs
