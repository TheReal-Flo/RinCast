// RinCast account integration: sign-in, PC registration/presence, incoming
// remote connection requests and UDP rendezvous (candidate exchange + hole
// punching; the server never relays stream traffic).
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "../net/http.h"
#include "../net/port_mapper.h"
#include "../util.h"
#include "rc/socket.h"

namespace rcs {

class StreamServer;

class CloudClient {
   public:
    bool Start(StreamServer* server);
    void Stop();

    // Synchronous account actions (called from the UI thread pool).
    // Return an empty string on success or a user-facing error message.
    std::string SignIn(const std::string& email, const std::string& password);
    std::string Register(const std::string& name, const std::string& email, const std::string& password);
    void SignOut();
    std::string ApproveHeadsetCode(const std::string& code);

    bool SignedIn() const;
    json StatusJson() const;
    // This PC's addresses on its own network, to type into a headset if discovery fails.
    std::vector<std::string> LanAddresses() const;
    void Wake();  // settings changed (name, allowRemote)

    void HandleRendezvous(const uint8_t* data, size_t size, const rc::NetAddress& from);

   private:
    void Run();
    bool EnsureAccessToken();
    bool RefreshTokens();
    void StoreTokens(const json& tokens);
    HttpResponse Api(const std::string& method, const std::string& path, const json& body, int timeoutMs = 15000);
    void RegisterPc();
    void Heartbeat();
    void HandleEvent(const json& ev);
    void RendezvousLoop(std::string sessionId, std::string relayToken, std::string host, int port);
    std::string BaseUrl() const;
    std::vector<std::string> LocalAddresses() const;

    StreamServer* m_server = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::string m_accessToken;
    int64_t m_accessExpiresNs = 0;
    std::string m_status = "signed_out";  // signed_out | connecting | online | error
    std::string m_lastError;
    bool m_registered = false;
    std::string m_registeredName;

    // Rendezvous
    std::mutex m_rvMutex;
    std::string m_rvSession;
    std::vector<rc::NetAddress> m_rvServers;  // rendezvous, one per address family
    std::vector<rc::NetAddress> m_rvPeers;    // everything known about the headset
    bool m_rvRegistered = false;

    PortMapper m_portMapper;
};

}  // namespace rcs
