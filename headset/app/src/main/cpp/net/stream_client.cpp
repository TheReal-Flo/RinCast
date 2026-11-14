#include "stream_client.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <iterator>
#if defined(__ANDROID__) || defined(__linux__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "rc/discovery.h"
#include "rc/handshake.h"
#include "rc/platform.h"
#include "rc/protocol.h"

namespace rv {

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

void Discovery::Start(const std::string& deviceId, const std::string& deviceName) {
    if (m_running) return;
    m_deviceId = deviceId;
    m_deviceName = deviceName;
    rc::NetInit();
    // Bind the discovery port to also hear periodic announcements; fall back
    // to an ephemeral port (replies to our probes are unicast anyway).
    if (!m_socket.Open(rc::kDiscoveryPort, true, true)) m_socket.Open(0, true);
    m_running = true;
    m_thread = std::thread([this] { Run(); });
}

void Discovery::SetUsbNetwork(uint64_t handle, uint32_t ifIndex) {
    std::lock_guard lock(m_usbMutex);
    if (handle == m_usbHandle && ifIndex == m_usbIndex) return;
    m_usbSocket.Close();
    m_usbHandle = handle;
    m_usbIndex = ifIndex;
    if (!handle || !ifIndex) return;
#ifdef __ANDROID__
    const bool bound = m_usbSocket.Open(0) && m_usbSocket.BindToNetwork(handle);
#else
    const bool bound = false;  // an Android network; elsewhere (test builds) there's none
#endif
    if (!bound) {
        RC_LOGW("USB network: no socket on it");
        m_usbSocket.Close();
        return;
    }
    RC_LOGI("USB network: looking for PCs on interface %u", ifIndex);
    m_probeNow = true;
}

void Discovery::Stop() {
    m_running = false;
    {
        std::lock_guard lock(m_usbMutex);
        m_usbSocket.Close();
    }
    m_socket.Close();
    if (m_thread.joinable()) m_thread.join();
}

std::vector<DiscoveredPc> Discovery::List() {
    std::lock_guard lock(m_mutex);
    std::vector<DiscoveredPc> out;
    const int64_t now = rc::NowNs();
    for (auto it = m_pcs.begin(); it != m_pcs.end();) {
        if (now - it->second.lastSeenNs > 12'000'000'000LL) {
            it = m_pcs.erase(it);
        } else {
            out.push_back(it->second);
            ++it;
        }
    }
    return out;
}

// The addresses to ask one by one, from every real network this headset is on.
static std::vector<uint32_t> SweepTargets() {
    constexpr size_t kMaxHosts = 1024;
    std::vector<uint32_t> out;
    auto usable = [](uint32_t ip) { return rc::IsPrivateLan(ip) || (ip >> 16) == ((169u << 8) | 254u); };
    for (const auto& itf : rc::ListInterfaces()) {
        if (itf.isVirtual || !usable(itf.ip)) continue;
        for (const uint32_t h : rc::SubnetHosts(itf.ip, itf.netmask, kMaxHosts)) out.push_back(h);
    }
    if (!out.empty()) return out;
#ifndef _WIN32
    // Newer Android versions can hide the interface list. Asking the system which address it
    // would send from works without sending anything; home networks are nearly always /24.
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return out;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    to.sin_addr.s_addr = htonl((1u << 24) | (1u << 16) | (1u << 8) | 1u);
    sockaddr_in self{};
    socklen_t size = sizeof(self);
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0 && getsockname(s, reinterpret_cast<sockaddr*>(&self), &size) == 0) {
        const uint32_t ip = ntohl(self.sin_addr.s_addr);
        if (usable(ip)) out = rc::SubnetHosts(ip, 0xFFFFFF00u, kMaxHosts);
    }
    close(s);
#endif
    return out;
}

void Discovery::Run() {
    uint8_t buf[2048];
    rc::NetAddress from;
    int64_t lastProbe = 0;
    uint32_t nonce = 1;
    std::vector<uint32_t> sweep;
    size_t sweepAt = 0;
    int64_t lastSweep = 0, lastBatch = 0;
    while (m_running) {
        const int64_t now = rc::NowNs();
        if (m_probeNow.exchange(false) || now - lastProbe > 2'000'000'000LL) {
            lastProbe = now;
            auto probe = rc::BuildProbe({m_deviceId, m_deviceName, nonce++});
            for (const auto& itf : rc::ListInterfaces())
                m_socket.SendTo({itf.broadcast(), rc::kDiscoveryPort}, probe.data(), probe.size());
            m_socket.SendTo(rc::NetAddress::Broadcast(rc::kDiscoveryPort), probe.data(), probe.size());
            // Addresses we know get asked directly each time, so a PC found once stays listed
            // on a network that drops broadcasts.
            std::vector<rc::NetAddress> known;
            {
                std::lock_guard lock(m_mutex);
                known = m_extraTargets;
                for (const auto& [id, pc] : m_pcs) known.push_back({pc.address.ip, rc::kDiscoveryPort});
            }
            for (const auto& a : known)
                if (a.valid()) m_socket.SendTo(a, probe.data(), probe.size());
            // Over the cable: every device on that link, which is the PC.
            std::lock_guard lock(m_usbMutex);
            if (m_usbSocket.IsOpen())
                m_usbSocket.SendTo(rc::NetAddress::AllNodesV6(rc::kDiscoveryPort, m_usbIndex), probe.data(), probe.size());
        }
        {
            std::lock_guard lock(m_usbMutex);
            int n;
            while (m_usbSocket.IsOpen() && (n = m_usbSocket.RecvFrom(buf, sizeof(buf), from, 0)) > 0) {
                rc::DiscoveryAnnounce a;
                if (!rc::ParseAnnounce(buf, size_t(n), a) || !from.IsLinkLocalV6()) continue;
                DiscoveredPc pc;
                pc.serverId = a.serverId;
                pc.name = a.name;
                pc.gpu = a.gpu;
                pc.version = a.version;
                pc.accountName = a.accountName;
                pc.accountLinked = a.accountLinked;
                pc.busy = a.busy;
                pc.address = from;
                pc.address.port = a.streamPort;
                pc.lastSeenNs = rc::NowNs();
                pc.viaUsb = true;
                std::lock_guard pcs(m_mutex);
                m_pcs[pc.serverId] = pc;  // the cable wins over any network
            }
        }
        if (m_searching) {
            // Every 30 s while nothing's found, less often once something is. 12 packets per
            // 100 ms: a /24 takes about 2 s, gently enough for any Wi-Fi.
            bool none;
            {
                std::lock_guard lock(m_mutex);
                none = m_pcs.empty();
            }
            if (sweepAt >= sweep.size() && (lastSweep == 0 || now - lastSweep > (none ? 30'000'000'000LL : 120'000'000'000LL))) {
                sweep = SweepTargets();
                sweepAt = 0;
                lastSweep = now;
            }
            if (sweepAt < sweep.size() && now - lastBatch >= 100'000'000LL) {
                lastBatch = now;
                auto probe = rc::BuildProbe({m_deviceId, m_deviceName, nonce});
                for (int i = 0; i < 12 && sweepAt < sweep.size(); i++, sweepAt++)
                    m_socket.SendTo({sweep[sweepAt], rc::kDiscoveryPort}, probe.data(), probe.size());
            }
        } else {
            sweepAt = sweep.size();
        }
        const int n = m_socket.RecvFrom(buf, sizeof(buf), from, 100);
        if (n <= 0) continue;
        rc::DiscoveryAnnounce a;
        if (!rc::ParseAnnounce(buf, size_t(n), a)) continue;
        DiscoveredPc pc;
        pc.serverId = a.serverId;
        pc.name = a.name;
        pc.gpu = a.gpu;
        pc.version = a.version;
        pc.accountName = a.accountName;
        pc.accountLinked = a.accountLinked;
        pc.busy = a.busy;
        pc.address = {from.ip, a.streamPort};
        pc.lastSeenNs = rc::NowNs();
        pc.viaVpn = !from.v6 && rc::RoutesThroughTunnel(from.ip);
        std::lock_guard lock(m_mutex);
        // A PC can answer on both. The local network wins as long as it keeps answering.
        const auto known = m_pcs.find(pc.serverId);
        if (known != m_pcs.end() && pc.viaVpn && !known->second.viaVpn && pc.lastSeenNs - known->second.lastSeenNs < 6'000'000'000LL) continue;
        // And a USB cable wins over both.
        if (known != m_pcs.end() && known->second.viaUsb && pc.lastSeenNs - known->second.lastSeenNs < 6'000'000'000LL) continue;
        m_pcs[pc.serverId] = pc;
    }
}

// ---------------------------------------------------------------------------
// StreamClient
// ---------------------------------------------------------------------------

void StreamClient::Connect(const ConnectTarget& target, const json& hello) {
    Disconnect();
    m_running = true;
    m_state = State::Connecting;
    {
        std::lock_guard lock(m_mutex);
        m_status = "Connecting to " + target.name + "…";
        m_serverName = target.name;
    }
    m_remote = !target.cloudSession.empty();
    m_vpn = !m_remote && target.viaVpn;
    m_usb = !m_remote && target.usbNetwork != 0;
    m_thread = std::thread([this, target, hello] { Run(target, hello); });
}

void StreamClient::Disconnect() {
    if (m_running.exchange(false)) {
        {
            std::lock_guard lock(m_mutex);
            if (m_connection) m_connection->SendDisconnect();
        }
        m_socket.Close();
        if (m_thread.joinable()) m_thread.join();
    }
    std::lock_guard lock(m_mutex);
    m_connection.reset();
    std::lock_guard sl(m_streamsMutex);
    m_streams.clear();
    if (m_state != State::Failed) m_state = State::Idle;
}

std::string StreamClient::StatusText() {
    std::lock_guard lock(m_mutex);
    return m_status;
}

std::string StreamClient::ServerName() {
    std::lock_guard lock(m_mutex);
    return m_serverName;
}

void StreamClient::SendControl(const json& message) {
    std::lock_guard lock(m_mutex);
    if (m_connection) m_connection->SendControl(message.dump());
}

size_t StreamClient::SendQueueChunks() {
    std::lock_guard lock(m_mutex);
    return m_connection ? m_connection->SendQueueChunks() : 0;
}

void StreamClient::SendBody(const rc::BodyTrackingPayload& b) {
    std::lock_guard lock(m_mutex);
    if (m_connection) m_connection->Send(rc::PacketType::BodyTracking, &b, sizeof(b));
}

void StreamClient::SendFace(const rc::FaceTrackingPayload& f) {
    std::lock_guard lock(m_mutex);
    if (m_connection) m_connection->Send(rc::PacketType::FaceTracking, &f, sizeof(f));
}

void StreamClient::SendHandSkeleton(const rc::HandSkeletonPayload& h) {
    std::lock_guard lock(m_mutex);
    if (m_connection) m_connection->Send(rc::PacketType::HandSkeleton, &h, sizeof(h));
}

void StreamClient::SendTracking(const rc::TrackingPayload& t, const rc::GazePayload* gaze, const rc::BatteryPayload* battery) {
    std::lock_guard lock(m_mutex);
    if (!m_connection) return;
    if (battery) {
        uint8_t extension[sizeof(rc::GazePayload) + sizeof(rc::BatteryPayload)]{};
        if (gaze) std::memcpy(extension, gaze, sizeof(*gaze));
        std::memcpy(extension + sizeof(rc::GazePayload), battery, sizeof(*battery));
        m_connection->Send2(rc::PacketType::Tracking, &t, sizeof(t), extension, sizeof(extension));
    } else if (gaze) m_connection->Send2(rc::PacketType::Tracking, &t, sizeof(t), gaze, sizeof(*gaze));
    else m_connection->Send(rc::PacketType::Tracking, &t, sizeof(t));
}

void StreamClient::SendPointer(const rc::PointerPayload& p) {
    std::lock_guard lock(m_mutex);
    if (m_connection) m_connection->Send(rc::PacketType::Pointer, &p, sizeof(p));
}

void StreamClient::SendAudio(const uint8_t* data, size_t size) {
    std::lock_guard lock(m_mutex);
    if (m_connection) m_connection->Send(rc::PacketType::Audio, data, size);
}

rc::ConnectionStats StreamClient::Stats() {
    std::lock_guard lock(m_mutex);
    return m_connection ? m_connection->Stats() : rc::ConnectionStats{};
}

int64_t StreamClient::ClockOffsetNs() {
    std::lock_guard lock(m_mutex);
    return m_connection ? m_connection->ClockOffsetNs() : 0;
}

bool StreamClient::HasClockOffset() {
    std::lock_guard lock(m_mutex);
    return m_connection && m_connection->HasClockOffset();
}

void StreamClient::RequestKeyframe(uint8_t streamId) {
    std::lock_guard lock(m_streamsMutex);
    m_pendingKeyframe[streamId] = true;
    m_feedbackNow = true;
}

void StreamClient::DelayTracker::Add(int64_t delayNs, int64_t nowNs) {
    // A new one-second bucket. After a pause, start over: the path may have changed.
    if (bucket < 0 || nowNs - bucketStartNs > 2'000'000'000LL) {
        std::fill(std::begin(bucketMin), std::end(bucketMin), INT64_MAX);
        bucket = 0;
        bucketStartNs = nowNs;
    } else if (nowNs - bucketStartNs >= 1'000'000'000LL) {
        bucket = (bucket + 1) % kBuckets;
        bucketMin[bucket] = INT64_MAX;
        bucketStartNs = nowNs;
    }
    bucketMin[bucket] = std::min(bucketMin[bucket], delayNs);
    const int64_t base = *std::min_element(std::begin(bucketMin), std::end(bucketMin));
    sumNs += double(delayNs - base);
    count++;
}

float StreamClient::DelayTracker::TakeQueueMs() {
    if (!count) return -1.f;
    const float ms = float(sumNs / count / 1e6);
    sumNs = 0;
    count = 0;
    return ms;
}

void StreamClient::SetLastGoodFrame(uint8_t streamId, uint32_t frame) {
    std::lock_guard lock(m_streamsMutex);
    m_streams[streamId].lastGoodFrame = frame;
}

void StreamClient::ResetStream(uint8_t streamId) {
    std::lock_guard lock(m_streamsMutex);
    m_streams.erase(streamId);
    m_pendingKeyframe[streamId] = true;
}

void StreamClient::ReportDecode(uint8_t streamId, double decodeMs, double latencyMs) {
    std::lock_guard lock(m_streamsMutex);
    auto& s = m_streams[streamId];
    s.decodeMs = decodeMs;
    s.latencyMs = latencyMs;
}

void StreamClient::OnPacket(rc::PacketType type, const uint8_t* payload, size_t size) {
    switch (type) {
        case rc::PacketType::Video:
        case rc::PacketType::VideoParity: {
            if (size < 1) return;
            const uint8_t streamId = payload[0];
            rc::VideoReassembler* r;
            {
                std::lock_guard lock(m_streamsMutex);
                auto& s = m_streams[streamId];
                if (!s.reassembler)
                    s.reassembler = std::make_unique<rc::VideoReassembler>([this](rc::ReceivedFrame&& f) {
                        int64_t sentNs = 0;  // PC clock: when the PC finished encoding (and sent) the frame
                        if (f.streamId == rc::kVrStreamId && f.metadata.size() >= sizeof(rc::VrFrameMetadata)) {
                            rc::VrFrameMetadata m;
                            std::memcpy(&m, f.metadata.data(), sizeof(m));
                            sentNs = m.pcEncodeEndNs;
                        } else if (f.streamId != rc::kVrStreamId &&
                                   f.metadata.size() >= sizeof(rc::DesktopFrameMetadata)) {
                            rc::DesktopFrameMetadata m;
                            std::memcpy(&m, f.metadata.data(), sizeof(m));
                            sentNs = m.pcEncodeEndNs;
                        }
                        if (sentNs) {
                            std::lock_guard lock(m_streamsMutex);
                            // First fragment: how late the frame started arriving is the queue in front of
                            // it. Its own transfer time (large frames on a slow link) isn't a queue.
                            m_streams[f.streamId].delay.Add(f.firstFragmentNs - sentNs, f.completeNs);
                        }
                        if (m_cb.onVideoFrame) m_cb.onVideoFrame(std::move(f));
                    });
                r = s.reassembler.get();
            }
            if (type == rc::PacketType::Video)
                r->HandleFragment(payload, size);
            else
                r->HandleParity(payload, size);
            break;
        }
        case rc::PacketType::Audio:
            if (m_cb.onAudio) m_cb.onAudio(payload, size);
            break;
        case rc::PacketType::Haptics:
            if (size >= sizeof(rc::HapticsPayload) && m_cb.onHaptics) {
                rc::HapticsPayload h;
                std::memcpy(&h, payload, sizeof(h));
                m_cb.onHaptics(h);
            }
            break;
        default:
            break;
    }
}

void StreamClient::SendFeedback() {
    std::vector<rc::VideoFeedbackPayload> feedback;
    {
        std::lock_guard lock(m_streamsMutex);
        const int64_t now = rc::NowNs();
        const int64_t windowNs = std::max<int64_t>(now - m_lastFeedbackNs, 1'000'000);
        m_lastFeedbackNs = now;
        for (auto& [id, s] : m_streams) {
            if (!s.reassembler) continue;
            const bool lost = s.reassembler->Expire(now, 250'000'000LL);
            const auto st = s.reassembler->TakeStats();
            rc::VideoFeedbackPayload fb{};
            fb.streamId = id;
            fb.requestKeyframe = (lost || m_pendingKeyframe[id]) ? 1 : 0;
            m_pendingKeyframe[id] = false;
            fb.lastCompleteFrame = s.reassembler->LastCompleteFrame();
            fb.lastGoodFrame = s.lastGoodFrame;
            fb.queueDelayMs = s.delay.TakeQueueMs();
            fb.framesLost = st.framesLost;
            fb.fragmentsReceived = st.fragmentsReceived;
            fb.fragmentsRecovered = st.fragmentsRecovered;
            fb.fragmentsLost = st.fragmentsLost;
            fb.decodeMs = float(s.decodeMs);
            fb.networkMs = st.framesComplete ? float(st.spreadMsSum / st.framesComplete) : 0.f;
            fb.totalLatencyMs = float(s.latencyMs);
            fb.receivedKbps = uint32_t(st.bytesReceived * 8 * 1000 / uint64_t(windowNs / 1000));  // bits/ms = kbps
            feedback.push_back(fb);
        }
        for (auto& [id, pending] : m_pendingKeyframe) {
            if (pending && !m_streams.count(id)) {
                rc::VideoFeedbackPayload fb{};
                fb.streamId = id;
                fb.requestKeyframe = 1;
                fb.queueDelayMs = -1;
                feedback.push_back(fb);
                pending = false;
            }
        }
    }
    std::lock_guard lock(m_mutex);
    if (!m_connection) return;
    for (const auto& fb : feedback) m_connection->Send(rc::PacketType::VideoFeedback, &fb, sizeof(fb));
}

// "registered" messages from the rendezvous list every address the PC can be
// reached on: where the server saw it, plus its LAN / IPv6 / router-mapped addresses.
void StreamClient::HandleRendezvous(const uint8_t* data, size_t size, const ConnectTarget& target) {
    json j;
    try {
        j = json::parse(reinterpret_cast<const char*>(data) + 4, reinterpret_cast<const char*>(data) + size);
    } catch (...) {
        return;
    }
    if (j.value("op", "") != "registered" || j.value("session", "") != target.cloudSession) return;
    std::vector<std::string> list;
    for (const auto& p : j.value("peers", json::array()))
        if (p.is_string()) list.push_back(p.get<std::string>());
    if (j.contains("peer") && j["peer"].is_string()) list.push_back(j["peer"].get<std::string>());
    std::lock_guard lock(m_mutex);
    m_rvRegistered = true;
    for (const auto& s : list) {
        rc::NetAddress a;
        if (!rc::NetAddress::ParseEndpoint(s, a) || (a.v6 && !m_socket.DualStack())) continue;
        bool known = false;
        for (const auto& p : m_peers) known |= p == a;
        if (!known && m_peers.size() < 32) {
            m_peers.push_back(a);
            RC_LOGI("Rendezvous: PC address %s", a.ToString().c_str());
        }
    }
}

namespace {

// Path preference: same LAN beats a direct internet path (public IPv4 or global IPv6),
// which beats a VPN overlay such as Tailscale (which may itself relay).
int PathRank(const rc::NetAddress& a) {
    if (!a.v6 && rc::IsPrivateLan(a.ip)) return 3;
    if (!a.v6 && rc::IsCgnat(a.ip)) return 1;
    return 2;
}

// Where the PC can reach us: LAN adapters and global IPv6 addresses.
json OwnCandidates(uint16_t port, bool dualStack) {
    json out = json::array();
    for (const auto& itf : rc::ListInterfaces())
        if (!itf.isVirtual) out.push_back(rc::NetAddress{itf.ip, port}.ToString());
    if (dualStack)
        for (const auto& a : rc::ListGlobalIpv6(port)) out.push_back(a.ToString());
    return out;
}

// Neighbouring ports of a public IPv4 endpoint: routers that allocate ports
// sequentially give the PC-facing mapping a port close to the one the server saw.
void SendWithPortGuesses(rc::UdpSocket& s, const rc::NetAddress& a, const std::vector<uint8_t>& bytes, bool guess) {
    s.SendTo(a, bytes.data(), bytes.size());
    if (!guess || a.v6 || rc::IsPrivateLan(a.ip)) return;
    for (int d : {1, 2, 3, 4, -1}) {
        rc::NetAddress q = a;
        q.port = uint16_t(a.port + d);
        s.SendTo(q, bytes.data(), bytes.size());
    }
}

}  // namespace

void StreamClient::Run(ConnectTarget target, json hello) {
#ifdef __ANDROID__
    setpriority(PRIO_PROCESS, gettid(), -10);  // best effort
#endif
    rc::NetInit();
    if (!m_socket.Open(0)) {
        m_state = State::Failed;
        std::lock_guard lock(m_mutex);
        m_status = "Network unavailable";
        return;
    }
    m_socket.SetBuffers(4 << 20, 16 << 20);
    m_socket.SetTos(0xB8);
    // Over the cable, through its network: the default one (Wi-Fi) can't reach a link-local address.
#ifdef __ANDROID__
    if (m_usb && !m_socket.BindToNetwork(target.usbNetwork)) RC_LOGW("USB network: couldn't bind the stream to it");
#endif
    {
        std::lock_guard lock(m_mutex);
        m_peers.clear();
        m_rvRegistered = false;
    }

    const auto keys = rc::crypto::KeyPair::Generate();
    uint8_t nonceBytes[16];
    rc::RandomBytes(nonceBytes, sizeof(nonceBytes));
    const std::string nonce = rc::HexEncode(nonceBytes, sizeof(nonceBytes));
    hello["nonce"] = nonce;
    if (m_hasIdentity) {
        const auto id = rc::crypto::IdentityPublicKey(m_identity);
        const auto sig = rc::crypto::SignConnection(m_identity, keys.publicKey, nonce);
        hello["deviceKey"] = rc::HexEncode(id.data(), id.size());
        hello["deviceSig"] = rc::HexEncode(sig.data(), sig.size());
    }
    if (m_remote) {
        hello["cloudSession"] = target.cloudSession;
        hello["proof"] = rc::HelloProof(target.connectSecret, keys.publicKey);
    }
    rc::HandshakePacket hp;
    hp.type = rc::PacketType::Hello;
    hp.publicKey = keys.publicKey;
    hp.json = hello.dump();
    const auto helloBytes = rc::BuildHandshake(hp);
    // Remote sessions: tell the rendezvous where we can be reached; it passes that to the
    // PC (which punches toward us) and gives us the PC's addresses. Nothing is relayed.
    const std::string registerMsg =
        "RCRV" + json{{"op", "register"},
                      {"session", target.cloudSession},
                      {"role", 1},
                      {"token", target.sessionToken},
                      {"candidates", OwnCandidates(m_socket.LocalPort(), m_socket.DualStack())}}
                     .dump();
    if (m_remote) {
        RC_LOGI("Remote connect: own addresses %s", OwnCandidates(m_socket.LocalPort(), m_socket.DualStack()).dump().c_str());
        for (const auto& rv : target.rendezvous) RC_LOGI("Remote connect: rendezvous %s", rv.ToString().c_str());
        for (const auto& a : target.pcLocalAddrs) RC_LOGI("Remote connect: PC address from the server %s", a.ToString().c_str());
    }

    uint8_t buf[2048];
    rc::NetAddress from;
    const int64_t start = rc::NowNs();
    int64_t lastHello = 0, lastRegister = 0;
    int64_t deadline = start + 30'000'000'000LL;

    // ---- handshake ----
    while (m_running && !m_connection) {
        const int64_t now = rc::NowNs();
        if (now > deadline) {
            m_state = State::Failed;
            std::lock_guard lock(m_mutex);
            RC_LOGW("Connect timed out (registered with the rendezvous: %d, PC addresses: %zu)", int(m_rvRegistered),
                    m_peers.size());
            m_status = m_usb ? "The PC didn't answer over the USB cable. Check RinCast Streamer on the PC for a firewall message."
                       : m_vpn ? "The PC didn't answer. Check that the VPN is on, on both devices."
                       : !m_remote ? "The PC didn't answer. Check that both are on the same network."
                       : m_peers.empty()
                           ? "The PC didn't respond. Make sure the RinCast Streamer is running and online."
                           : "No direct connection was possible: both networks block incoming connections. Using the same "
                             "Wi-Fi, IPv6, or allowing automatic port sharing for the PC in its router helps.";
            return;
        }
        if (m_remote) {
            bool registered;
            {
                std::lock_guard lock(m_mutex);
                registered = m_rvRegistered;
            }
            if (now - lastRegister > (registered ? 3'000'000'000LL : 300'000'000LL)) {
                lastRegister = now;
                for (const auto& rv : target.rendezvous)
                    if (!rv.v6 || m_socket.DualStack()) m_socket.SendTo(rv, registerMsg.data(), registerMsg.size());
            }
        }
        if (now - lastHello > 200'000'000LL) {
            lastHello = now;
            const bool early = now - start < 10'000'000'000LL;
            if (target.direct.valid()) m_socket.SendTo(target.direct, helloBytes.data(), helloBytes.size());
            for (const auto& a : target.pcLocalAddrs)
                if (!a.v6 || m_socket.DualStack()) SendWithPortGuesses(m_socket, a, helloBytes, false);
            std::vector<rc::NetAddress> peers;
            {
                std::lock_guard lock(m_mutex);
                peers = m_peers;
            }
            for (const auto& a : peers) SendWithPortGuesses(m_socket, a, helloBytes, early);
        }
        const int n = m_socket.RecvFrom(buf, sizeof(buf), from, 20);
        if (n <= 0) continue;
        if (n >= 4 && std::memcmp(buf, "RCRV", 4) == 0) {
            HandleRendezvous(buf, size_t(n), target);
            continue;
        }
        rc::HandshakePacket reply;
        if (!rc::ParseHandshake(buf, size_t(n), reply)) continue;
        if (reply.type == rc::PacketType::Reject) {
            json r;
            try {
                r = json::parse(reply.json);
            } catch (...) {
            }
            const std::string reason = r.value("reason", "");
            std::lock_guard lock(m_mutex);
            m_status = r.value("message", "The PC refused the connection.");
            if (reason == "pending_approval") {
                m_state = State::WaitingApproval;
                deadline = std::max<int64_t>(deadline, now + 120'000'000'000LL);
            } else {
                m_state = State::Failed;
                return;
            }
            continue;
        }
        if (reply.type != rc::PacketType::Welcome) continue;
        json w;
        try {
            w = json::parse(reply.json);
        } catch (...) {
            continue;
        }
        if (m_remote && w.value("proof", "") != rc::WelcomeProof(target.connectSecret, reply.publicKey, keys.publicKey)) {
            RC_LOGW("Welcome proof mismatch from %s", from.ToString().c_str());
            continue;
        }
        const auto sk = rc::crypto::DeriveSessionKeys(
            keys.secretKey, reply.publicKey, keys.publicKey, reply.publicKey,
            rc::SessionContext(nonce, w.value("nonce", ""), m_remote ? target.connectSecret : ""));
        auto conn = std::make_unique<rc::Connection>(&m_socket, from, reply.sessionId, sk, false,
                                                     m_remote || m_vpn ? rc::kMaxDatagramWan : rc::kMaxDatagramLan);
        conn->SetControlHandler([this](const std::string& text) {
            try {
                const json m = json::parse(text);
                if (m_cb.onControl) m_cb.onControl(m);
            } catch (...) {
            }
        });
        conn->SetPacketHandler([this](rc::PacketType t, const uint8_t* p, size_t s) { OnPacket(t, p, s); });
        {
            std::lock_guard lock(m_mutex);
            m_connection = std::move(conn);
            m_serverName = w.value("name", target.name);
            m_status = "Connected";
        }
        m_state = State::Connected;
        RC_LOGI("Connected to %s via %s", m_serverName.c_str(), from.ToString().c_str());
    }


    // ---- session ----
    // A remote session starts on whichever path answered first. Keep offering the
    // better-ranked ones (same LAN, or a direct internet path when we started on a VPN
    // overlay); the PC answers a repeated Hello with its Welcome, and when one arrives
    // from a better path we switch to it. The PC follows automatically (it tracks the
    // source of authenticated packets).
    const int64_t sessionStart = rc::NowNs();
    int64_t lastProbe = 0;
    int64_t lastFeedback = 0;
    while (m_running) {
        const int n = m_socket.RecvFrom(buf, sizeof(buf), from, 2);
        if (n > 0) {
            if (n >= 4 && std::memcmp(buf, "RCRV", 4) == 0) {
                HandleRendezvous(buf, size_t(n), target);
                continue;
            }
            if (rc::IsHandshakeDatagram(buf, size_t(n))) {
                rc::HandshakePacket late;
                std::lock_guard lock(m_mutex);
                if (m_connection && rc::ParseHandshake(buf, size_t(n), late) && late.type == rc::PacketType::Welcome &&
                    late.sessionId == m_connection->SessionId() && PathRank(from) > PathRank(m_connection->Remote())) {
                    RC_LOGI("Switching to the better path %s (was %s)", from.ToString().c_str(),
                            m_connection->Remote().ToString().c_str());
                    m_connection->SetRemote(from);
                }
                continue;
            }
            rc::Connection* c;
            {
                std::lock_guard lock(m_mutex);
                c = m_connection.get();
            }
            if (c) c->HandleDatagram(buf, size_t(n), from);
        } else if (n < 0 && !m_running) {
            break;
        }
        const int64_t now = rc::NowNs();
        rc::Connection* c;
        std::vector<rc::NetAddress> peers;
        {
            std::lock_guard lock(m_mutex);
            c = m_connection.get();
            peers = m_peers;
        }
        if (!c) break;
        c->Tick(now);
        // Every 100 ms, and right away when a frame went missing: every millisecond until
        // the PC hears about it is a millisecond of frozen picture. (The PC ignores
        // repeats while its answer is on the way.)
        const bool urgent = m_feedbackNow && now - lastFeedback > 5'000'000LL;
        if (urgent || now - lastFeedback > 100'000'000LL) {
            m_feedbackNow = false;
            lastFeedback = now;
            SendFeedback();
        }
        // Probe better paths: often for the first 10 s, then every 5 s.
        if (m_remote && now - lastProbe > (now - sessionStart < 10'000'000'000LL ? 500'000'000LL : 5'000'000'000LL)) {
            lastProbe = now;
            const int current = PathRank(c->Remote());
            if (current < 3) {
                for (const auto& a : target.pcLocalAddrs)
                    if (PathRank(a) > current && (!a.v6 || m_socket.DualStack()))
                        m_socket.SendTo(a, helloBytes.data(), helloBytes.size());
                for (const auto& a : peers)
                    if (PathRank(a) > current) m_socket.SendTo(a, helloBytes.data(), helloBytes.size());
            }
        }
        if (c->PeerDisconnected() || now - c->LastReceiveNs() > 6'000'000'000LL) {
            std::lock_guard lock(m_mutex);
            m_status = c->PeerDisconnected() ? "The PC ended the session." : "Connection lost.";
            m_state = State::Failed;
            break;
        }
    }
}

}  // namespace rv
