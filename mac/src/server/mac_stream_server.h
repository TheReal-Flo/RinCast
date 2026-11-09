#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../session/mac_session.h"
#include "rc/crypto.h"
#include "rc/discovery.h"
#include "rc/handshake.h"
#include "rc/socket.h"

namespace rcm {

class MacStreamServer {
   public:
    MacStreamServer();
    ~MacStreamServer();

    bool Start(const std::string& serverName = "Mac RinCast");
    void Stop();

    std::shared_ptr<MacClientSession> ActiveSession();
    void DisconnectActive(const std::string& reason);

   private:
    void ReceiveLoop();
    void DiscoveryLoop();

    void HandleHello(const uint8_t* data, size_t size, const rc::NetAddress& from);
    std::vector<uint8_t> MakeAnnounce(uint32_t nonce);

    std::string m_serverName = "Mac RinCast";
    std::string m_serverId;
    std::atomic<bool> m_running{false};

    rc::UdpSocket m_socket;
    rc::UdpSocket m_discovery;

    std::thread m_receiveThread;
    std::thread m_discoveryThread;

    std::mutex m_mutex;
    std::shared_ptr<MacClientSession> m_session;

    struct InFlightHandshake {
        rc::crypto::KeyPair serverKeys;
        uint32_t sessionId = 0;
        std::string serverNonce;
        int64_t createdNs = 0;
    };
    std::map<std::string, InFlightHandshake> m_handshakes;
};

}  // namespace rcm
