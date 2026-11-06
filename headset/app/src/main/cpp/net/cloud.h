// RinCast account on the headset: sign in with a short code (device flow),
// list the account's PCs and request remote connections.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "stream_client.h"

namespace rv {

struct CloudPc {
    std::string id;
    std::string name;
    std::string gpu;
    bool online = false;
    bool busy = false;
};

class Cloud {
   public:
    enum class SignInState { SignedOut, RequestingCode, WaitingForApproval, SignedIn, Error };

    ~Cloud() { Stop(); }
    void Start(const std::string& serverUrl, const std::string& refreshToken, const std::string& userName,
               std::function<void(const std::string& refreshToken, const std::string& userName)> persist);
    void Stop();
    void SetServer(const std::string& url);

    void BeginSignIn();
    void CancelSignIn();
    void SignOut();
    void RefreshPcs() { m_refreshNow = true; }

    // Asynchronously requests a session; `done` gets a target or an error.
    void Connect(const std::string& pcId, const std::string& pcName,
                 std::function<void(bool ok, ConnectTarget target, std::string error)> done);

    SignInState State() const { return m_state; }
    std::string UserCode();
    std::string VerificationUrl();
    std::string UserName();
    std::string Error();
    std::vector<CloudPc> Pcs();

   private:
    void Run();
    bool EnsureAccess();
    json Api(const std::string& method, const std::string& path, const json& body, int* status = nullptr,
             int timeoutMs = 15000);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_refreshNow{true};
    std::atomic<bool> m_signInRequested{false};
    std::atomic<SignInState> m_state{SignInState::SignedOut};

    std::mutex m_mutex;
    std::string m_server;
    std::string m_refresh;
    std::string m_access;
    int64_t m_accessExpiry = 0;
    std::string m_userName;
    std::string m_userCode, m_verificationUrl, m_deviceCode, m_error;
    int64_t m_codeExpiry = 0;
    int m_pollInterval = 3;
    std::vector<CloudPc> m_pcs;
    std::function<void(const std::string&, const std::string&)> m_persist;
};

}  // namespace rv
