// Owns the UDP stream socket and the discovery responder, performs the
// handshake, and routes datagrams to the active ClientSession.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "rc/crypto.h"
#include "rc/socket.h"
#include "../util.h"

namespace rcs {

class ClientSession;

struct HelloInfo {
    std::string deviceId;
    std::string deviceName;
    std::string model;
    std::string appVersion;
    std::string cloudSession;
    std::string deviceKey;  // hex, only if the Hello proved it
    rc::NetAddress address;
    json raw;
};

// Cloud-authorized session expected to arrive (from a connect event).
struct ExpectedCloudSession {
    std::string connectSecret;
    std::string headsetId;
    std::string headsetName;
    int64_t expiresNs = 0;
};

class StreamServer {
   public:
    using ApprovalCallback = std::function<void(const HelloInfo& hello)>;
    using RendezvousHandler = std::function<void(const uint8_t* data, size_t size, const rc::NetAddress& from)>;

    bool Start();
    void Stop();

    // Approval UI: the App calls Approve/Deny after asking the user.
    void SetApprovalCallback(ApprovalCallback cb) { m_onApproval = std::move(cb); }
    // A waiting headset keeps asking, several times a second; when that stops, it has given up.
    bool StillAsking(const std::string& deviceId, int64_t withinNs);
    void Approve(const std::string& deviceId, bool remember);
    void Deny(const std::string& deviceId);

    void ExpectCloudSession(const std::string& sessionId, const ExpectedCloudSession& s);
    void SetRendezvousHandler(RendezvousHandler h) { m_onRendezvous = std::move(h); }

    rc::UdpSocket& Socket() { return m_socket; }
    std::shared_ptr<ClientSession> ActiveSession();
    void DisconnectActive(const std::string& reason);
    uint16_t Port() const { return m_socket.LocalPort(); }

   private:
    void ReceiveLoop();
    void DiscoveryLoop();
    void DiscoveryV6Loop();
    std::vector<uint8_t> MakeAnnounce(uint32_t nonce);
    void HandleHello(const uint8_t* data, size_t size, const rc::NetAddress& from);
    void SendReject(const rc::NetAddress& to, const std::string& reason, const std::string& message);

    rc::UdpSocket m_socket;
    rc::UdpSocket m_discovery;
    rc::UdpSocket m_discoveryV6;  // link-local: a headset on a USB cable has only that
    std::thread m_receiveThread;
    std::thread m_discoveryThread;
    std::thread m_discoveryV6Thread;
    std::atomic<bool> m_running{false};

    std::mutex m_mutex;
    std::shared_ptr<ClientSession> m_session;
    // Handshake state per client public key (so retransmitted Hellos reuse keys).
    struct PendingHandshake {
        rc::crypto::KeyPair serverKeys;
        uint32_t sessionId = 0;
        std::string serverNonce;
        int64_t createdNs = 0;
    };
    std::map<std::string, PendingHandshake> m_handshakes;  // by client pk hex
    std::map<std::string, int64_t> m_approved;              // deviceId -> allowed until (one-shot approvals)
    std::map<std::string, int64_t> m_denied;                // deviceId -> denied until
    std::map<std::string, int64_t> m_promptedAt;            // deviceId -> when we last asked
    std::map<std::string, int64_t> m_askedAt;               // deviceId -> when it last asked us
    std::map<std::string, std::string> m_approvalKey;       // deviceId -> identity key that asked
    std::map<std::string, ExpectedCloudSession> m_cloudSessions;

    ApprovalCallback m_onApproval;
    RendezvousHandler m_onRendezvous;
};

}  // namespace rcs
