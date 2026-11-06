#include "cloud.h"

#include "../platform/jni_bridge.h"
#include "rc/platform.h"

namespace rv {

void Cloud::Start(const std::string& serverUrl, const std::string& refreshToken, const std::string& userName,
                  std::function<void(const std::string&, const std::string&)> persist) {
    {
        std::lock_guard lock(m_mutex);
        m_server = serverUrl;
        m_refresh = refreshToken;
        m_userName = userName;
        m_persist = std::move(persist);
    }
    m_state = refreshToken.empty() ? SignInState::SignedOut : SignInState::SignedIn;
    m_running = true;
    m_thread = std::thread([this] { Run(); });
}

void Cloud::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void Cloud::SetServer(const std::string& url) {
    std::lock_guard lock(m_mutex);
    m_server = url;
}

std::string Cloud::UserCode() {
    std::lock_guard lock(m_mutex);
    return m_userCode;
}
std::string Cloud::VerificationUrl() {
    std::lock_guard lock(m_mutex);
    return m_verificationUrl;
}
std::string Cloud::UserName() {
    std::lock_guard lock(m_mutex);
    return m_userName;
}
std::string Cloud::Error() {
    std::lock_guard lock(m_mutex);
    return m_error;
}
std::vector<CloudPc> Cloud::Pcs() {
    std::lock_guard lock(m_mutex);
    return m_pcs;
}

void Cloud::BeginSignIn() {
    m_state = SignInState::RequestingCode;
    m_signInRequested = true;
}

void Cloud::CancelSignIn() {
    std::lock_guard lock(m_mutex);
    m_deviceCode.clear();
    m_userCode.clear();
    m_state = m_refresh.empty() ? SignInState::SignedOut : SignInState::SignedIn;
}

void Cloud::SignOut() {
    std::string refresh;
    {
        std::lock_guard lock(m_mutex);
        refresh = m_refresh;
        m_refresh.clear();
        m_access.clear();
        m_userName.clear();
        m_pcs.clear();
    }
    m_state = SignInState::SignedOut;
    if (m_persist) m_persist("", "");
    std::thread([this, refresh] { Api("POST", "/auth/logout", json{{"refreshToken", refresh}}); }).detach();
}

json Cloud::Api(const std::string& method, const std::string& path, const json& body, int* status, int timeoutMs) {
    std::string server, access;
    {
        std::lock_guard lock(m_mutex);
        server = m_server;
        access = m_access;
    }
    while (!server.empty() && server.back() == '/') server.pop_back();
    const auto r = Platform::Http(method, server + "/api/v1" + path, body.is_null() ? "" : body.dump(), access, timeoutMs);
    if (status) *status = r.status;
    try {
        return json::parse(r.body);
    } catch (...) {
        return json::object();
    }
}

bool Cloud::EnsureAccess() {
    std::string refresh;
    {
        std::lock_guard lock(m_mutex);
        if (!m_access.empty() && rc::NowNs() < m_accessExpiry) return true;
        refresh = m_refresh;
    }
    if (refresh.empty()) return false;
    int status = 0;
    const json r = Api("POST", "/auth/refresh", json{{"refreshToken", refresh}}, &status);
    if (status == 401) {
        SignOut();
        std::lock_guard lock(m_mutex);
        m_error = "Your sign-in expired. Sign in again to reach PCs on other networks.";
        return false;
    }
    if (status != 200) return false;
    std::string newRefresh, name;
    {
        std::lock_guard lock(m_mutex);
        m_access = r.value("accessToken", "");
        m_refresh = r.value("refreshToken", m_refresh);
        m_accessExpiry = rc::NowNs() + int64_t(r.value("expiresIn", 3600) - 60) * 1'000'000'000LL;
        if (r.contains("user")) m_userName = r["user"].value("name", m_userName);
        newRefresh = m_refresh;
        name = m_userName;
    }
    if (m_persist) m_persist(newRefresh, name);
    return true;
}

void Cloud::Run() {
    int64_t lastList = 0;
    int64_t lastPoll = 0;
    while (m_running) {
        rc::SleepMs(200);
        const int64_t now = rc::NowNs();

        if (m_signInRequested.exchange(false)) {
            int status = 0;
            const json r = Api("POST", "/device/code",
                               json{{"deviceId", Platform::DeviceId()}, {"deviceName", Platform::DeviceModel()},
                                    {"model", Platform::DeviceModel()}},
                               &status);
            std::lock_guard lock(m_mutex);
            if (status == 200) {
                m_deviceCode = r.value("deviceCode", "");
                m_userCode = r.value("userCode", "");
                m_verificationUrl = r.value("verificationUri", "");
                m_pollInterval = std::max(2, r.value("interval", 3));
                m_codeExpiry = now + int64_t(r.value("expiresIn", 600)) * 1'000'000'000LL;
                m_error.clear();
                m_state = SignInState::WaitingForApproval;
            } else {
                m_error = status == 0 ? "Can't reach the RinCast server. Check your internet connection."
                                      : r.value("message", "Sign-in is unavailable right now.");
                m_state = SignInState::Error;
            }
        }

        if (m_state == SignInState::WaitingForApproval && now - lastPoll > int64_t(m_pollInterval) * 1'000'000'000LL) {
            lastPoll = now;
            std::string deviceCode;
            {
                std::lock_guard lock(m_mutex);
                deviceCode = m_deviceCode;
                if (now > m_codeExpiry) {
                    m_state = SignInState::Error;
                    m_error = "The code expired. Try again.";
                    continue;
                }
            }
            if (deviceCode.empty()) continue;
            int status = 0;
            const json r = Api("POST", "/device/token", json{{"deviceCode", deviceCode}}, &status);
            if (status == 200) {
                std::string refresh, name;
                {
                    std::lock_guard lock(m_mutex);
                    m_access = r.value("accessToken", "");
                    m_refresh = r.value("refreshToken", "");
                    m_accessExpiry = now + int64_t(r.value("expiresIn", 3600) - 60) * 1'000'000'000LL;
                    m_userName = r.contains("user") ? r["user"].value("name", "") : "";
                    m_deviceCode.clear();
                    m_userCode.clear();
                    refresh = m_refresh;
                    name = m_userName;
                }
                m_state = SignInState::SignedIn;
                if (m_persist) m_persist(refresh, name);
                m_refreshNow = true;
            } else if (r.value("error", "") == "expired_token") {
                std::lock_guard lock(m_mutex);
                m_state = SignInState::Error;
                m_error = "The code expired. Try again.";
            } else if (r.value("error", "") == "slow_down") {
                m_pollInterval++;
            }
        }

        if (m_state == SignInState::SignedIn && (m_refreshNow.exchange(false) || now - lastList > 15'000'000'000LL)) {
            lastList = now;
            if (!EnsureAccess()) continue;
            int status = 0;
            const json r = Api("GET", "/pcs", json(), &status);
            if (status == 200) {
                std::vector<CloudPc> pcs;
                for (const auto& p : r.value("pcs", json::array()))
                    pcs.push_back({p.value("id", ""), p.value("name", ""), p.value("gpu", ""), p.value("online", false),
                                   p.value("busy", false)});
                std::lock_guard lock(m_mutex);
                m_pcs = std::move(pcs);
            }
        }
    }
}

void Cloud::Connect(const std::string& pcId, const std::string& pcName,
                    std::function<void(bool, ConnectTarget, std::string)> done) {
    std::thread([this, pcId, pcName, done] {
        if (!EnsureAccess()) {
            done(false, {}, "Sign in to connect to PCs on other networks.");
            return;
        }
        int status = 0;
        const json r = Api("POST", "/connect", json{{"pcId", pcId}}, &status, 20000);
        if (status != 200) {
            done(false, {}, status == 0 ? "Can't reach the RinCast server." : r.value("message", "Couldn't connect."));
            return;
        }
        ConnectTarget t;
        t.name = r.value("pcName", pcName);
        t.cloudSession = r.value("sessionId", "");
        t.connectSecret = r.value("connectSecret", "");
        t.sessionToken = r.value("relayToken", "");
        const json rv = r.value("rendezvous", json::object());
        // Every address the rendezvous host resolves to: registering over IPv4 and IPv6
        // lets the server tell the PC where we are reachable on both.
        t.rendezvous = rc::NetAddress::ResolveAll(rv.value("host", ""), uint16_t(rv.value("port", 0)));
        if (t.rendezvous.empty()) {
            done(false, {}, "Can't reach the RinCast server's connection service.");
            return;
        }
        for (const auto& a : r.value("pcLocalAddrs", json::array())) {
            rc::NetAddress addr;
            if (a.is_string() && rc::NetAddress::ParseEndpoint(a.get<std::string>(), addr)) t.pcLocalAddrs.push_back(addr);
        }
        done(true, t, "");
    }).detach();
}

}  // namespace rv
