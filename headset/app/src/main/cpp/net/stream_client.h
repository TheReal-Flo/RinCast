// Headset networking: LAN discovery and the stream connection to a PC.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"
#include "rc/connection.h"
#include "rc/crypto.h"
#include "rc/socket.h"
#include "rc/video_transport.h"

namespace rv {

using json = nlohmann::json;

struct DiscoveredPc {
    bool viaVpn = false;  // it answered through a VPN, not on this network
    bool viaUsb = false;  // it answered over a USB cable
    std::string serverId;
    std::string name;
    std::string gpu;
    std::string version;
    std::string accountName;
    bool accountLinked = false;
    bool busy = false;
    rc::NetAddress address;
    int64_t lastSeenNs = 0;
};

class Discovery {
   public:
    ~Discovery() { Stop(); }
    void Start(const std::string& deviceId, const std::string& deviceName);
    void Stop();
    std::vector<DiscoveredPc> List();
    void ProbeNow() { m_probeNow = true; }
    // An address always asked directly: one typed in, or where a PC was last.
    void AddProbeTarget(const rc::NetAddress& a) {
        std::lock_guard lock(m_mutex);
        for (const auto& t : m_extraTargets)
            if (t == a) return;
        if (m_extraTargets.size() >= 4) m_extraTargets.erase(m_extraTargets.begin());
        m_extraTargets.push_back(a);
        m_probeNow = true;
    }
    // While someone's choosing a PC, also ask each address on this network directly: plenty of
    // routers and mesh systems drop broadcasts between devices. Not while streaming.
    void SetSearching(bool searching) { m_searching = searching; }
    // A USB cable to the PC, as an Android network and its interface; 0 when there's none.
    void SetUsbNetwork(uint64_t handle, uint32_t ifIndex);

   private:
    void Run();
    rc::UdpSocket m_socket;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_probeNow{true};
    std::atomic<bool> m_searching{false};
    std::string m_deviceId, m_deviceName;
    std::mutex m_mutex;
    std::map<std::string, DiscoveredPc> m_pcs;
    std::vector<rc::NetAddress> m_extraTargets;
    std::mutex m_usbMutex;
    rc::UdpSocket m_usbSocket;  // bound to the cable's network
    uint64_t m_usbHandle = 0;
    uint32_t m_usbIndex = 0;
};

// How to reach a PC.
struct ConnectTarget {
    std::string name;
    rc::NetAddress direct;                  // LAN address (discovery) if known
    bool viaVpn = false;                    // that address is reached through a VPN
    uint64_t usbNetwork = 0;                // that address is on the USB cable: its Android network
    // Remote (account) connection
    std::string cloudSession;
    std::string connectSecret;
    std::string sessionToken;                // authorizes us at the rendezvous ("relayToken" on the wire)
    std::vector<rc::NetAddress> rendezvous;  // the server's rendezvous, one per address family
    std::vector<rc::NetAddress> pcLocalAddrs;
};

class StreamClient {
   public:
    enum class State { Idle, Connecting, WaitingApproval, Connected, Failed };

    struct Callbacks {
        std::function<void(const json&)> onControl;                       // network thread
        std::function<void(rc::ReceivedFrame&&)> onVideoFrame;            // network thread
        std::function<void(const uint8_t*, size_t)> onAudio;              // network thread
        std::function<void(const rc::HapticsPayload&)> onHaptics;         // network thread
    };

    ~StreamClient() { Disconnect(); }

    void SetCallbacks(Callbacks cb) { m_cb = std::move(cb); }
    void Connect(const ConnectTarget& target, const json& hello);
    // The headset's identity, for proving it's the headset the PC trusts.
    void SetIdentity(const rc::crypto::IdentitySeed& seed) { m_identity = seed; m_hasIdentity = true; }
    void Disconnect();

    State GetState() const { return m_state.load(); }
    std::string StatusText();
    std::string ServerName();
    bool IsRemote() const { return m_remote; }
    bool IsVpn() const { return m_vpn; }
    bool IsUsb() const { return m_usb; }

    // Thread-safe senders (no-ops when not connected).
    void SendControl(const json& message);
    // With where the eyes look appended, for foveated encoding.
    void SendTracking(const rc::TrackingPayload& t, const rc::GazePayload* gaze = nullptr);
    void SendBody(const rc::BodyTrackingPayload& b);
    void SendFace(const rc::FaceTrackingPayload& f);
    void SendHandSkeleton(const rc::HandSkeletonPayload& h);
    void SendPointer(const rc::PointerPayload& p);
    void SendAudio(const uint8_t* data, size_t size);
    void SendFeedback();

    rc::ConnectionStats Stats();
    int64_t ClockOffsetNs();  // PC - headset
    bool HasClockOffset();
    void RequestKeyframe(uint8_t streamId);
    void ResetStream(uint8_t streamId);

   private:
    void Run(ConnectTarget target, json hello);
    void HandleRendezvous(const uint8_t* data, size_t size, const ConnectTarget& target);
    void OnPacket(rc::PacketType type, const uint8_t* payload, size_t size);

    Callbacks m_cb;
    std::atomic<State> m_state{State::Idle};
    std::atomic<bool> m_running{false};
    rc::crypto::IdentitySeed m_identity{};
    bool m_hasIdentity = false;
    std::thread m_thread;
    rc::UdpSocket m_socket;
    std::mutex m_mutex;
    std::unique_ptr<rc::Connection> m_connection;
    std::string m_status;
    std::string m_serverName;
    bool m_remote = false;
    bool m_vpn = false;
    bool m_usb = false;
    std::vector<rc::NetAddress> m_peers;  // PC addresses learned from the rendezvous
    bool m_rvRegistered = false;

    // Queueing delay: each frame's arrival minus the PC's send time. The two clocks
    // differ by an unknown constant, so only the excess over the lowest value of the
    // last 10 s means anything: that is how long the frame waited in queues on the way.
    struct DelayTracker {
        static constexpr int kBuckets = 10;  // of one second each
        int64_t bucketMin[kBuckets];
        int64_t bucketStartNs = 0;
        int bucket = -1;
        double sumNs = 0;  // relative to the baseline at the time
        uint32_t count = 0;
        void Add(int64_t delayNs, int64_t nowNs);
        float TakeQueueMs();  // average over the frames since the last call; -1 without any
    };
    struct StreamRx {
        std::unique_ptr<rc::VideoReassembler> reassembler;
        DelayTracker delay;
        bool keyframeRequested = false;
        double decodeMs = 0;
        double latencyMs = 0;
        uint32_t lastGoodFrame = 0;  // from the decoder, for loss recovery
    };
    std::mutex m_streamsMutex;
    std::map<uint8_t, StreamRx> m_streams;
    std::map<uint8_t, bool> m_pendingKeyframe;
    std::atomic<bool> m_feedbackNow{false};  // a loss: tell the PC without waiting for the next tick
    int64_t m_lastFeedbackNs = 0;  // start of the current feedback window

   public:
    // Reported by the app for the feedback packets.
    void ReportDecode(uint8_t streamId, double decodeMs, double latencyMs);
    void SetLastGoodFrame(uint8_t streamId, uint32_t frame);
};

}  // namespace rv
