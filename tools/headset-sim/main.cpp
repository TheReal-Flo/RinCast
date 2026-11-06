// Headset simulator: speaks the RinCast protocol like the Quest app does, so
// the PC side can be tested end to end without a headset.
//
//   headset-sim [--pc <ip>] [--seconds N] [--desktop] [--vr] [--still] [--codec hevc|h264|av1] [--out <dir>]
//               [--drop-every N] [--move] [--hz 72|80|90|120]
//               [--limit-mbps N] [--press-system S] [--body] [--hands] [--fingers]
//
// --drop-every N throws away every Nth video frame as if it was lost on the network and
// handles it like the headset app: frames after a gap are skipped until a keyframe or a
// recovery frame (kVideoRecovery) arrives, and the loss is reported right away. The
// output file then holds exactly what the decoder would have been given.
//
// --limit-mbps N passes the PC's packets through a queue drained at N Mbit/s (dropping
// beyond 1 s of data), like a slow upload with a large router buffer, to test the
// streamer's bitrate control. Progress lines show the bitrate and the queueing delay.
//
// --move moves the right controller on the path tools/pose-probe measures.
//
// Received video streams are written as raw Annex-B / OBU files
// (stream<N>.h264/.h265/.obu) so they can be checked with ffprobe/ffplay.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <climits>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "nlohmann/json.hpp"
#include "rc/connection.h"
#include "rc/discovery.h"
#include "rc/handshake.h"
#include "rc/platform.h"
#include "rc/protocol.h"
#include "rc/video_transport.h"

using namespace rc;
using json = nlohmann::json;

static bool Discover(NetAddress& out, std::string& name) {
    UdpSocket s;
    if (!s.Open(0, true)) return false;
    DiscoveryProbe probe{"sim-device", "Headset Simulator", 42};
    auto bytes = BuildProbe(probe);
    for (const auto& itf : ListInterfaces())
        s.SendTo(NetAddress{itf.broadcast(), kDiscoveryPort}, bytes.data(), bytes.size());
    s.SendTo(NetAddress{0x7F000001, kDiscoveryPort}, bytes.data(), bytes.size());
    uint8_t buf[2048];
    NetAddress from;
    const int64_t start = NowNs();
    while (NowNs() - start < 3'000'000'000LL) {
        const int n = s.RecvFrom(buf, sizeof(buf), from, 200);
        DiscoveryAnnounce a;
        if (n > 0 && ParseAnnounce(buf, size_t(n), a)) {
            out = NetAddress{from.ip, a.streamPort};
            name = a.name + " (" + a.gpu + ")";
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    NetInit();
    std::string pcIp, outDir = ".", codec = "hevc";
    int seconds = 8;
    float hz = 90;
    double limitMbps = 0;
    double lossPercent = 0;  // random datagram loss, like Wi-Fi
    double pressSystemAt = 0;
    uint32_t dropEvery = 0;
    bool desktop = false, vr = false, still = false, move = false;
    bool body = false, hands = false, fingers = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--pc" && i + 1 < argc) pcIp = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (a == "--desktop") desktop = true;
        else if (a == "--vr") vr = true;
        else if (a == "--still") still = true;
        else if (a == "--move") move = true;
        else if (a == "--body") body = true;
        else if (a == "--hands") hands = true;
        else if (a == "--fingers") fingers = true;
        else if (a == "--hz" && i + 1 < argc) hz = float(std::atof(argv[++i]));
        else if (a == "--limit-mbps" && i + 1 < argc) limitMbps = std::atof(argv[++i]);
        else if (a == "--loss" && i + 1 < argc) lossPercent = std::atof(argv[++i]);
        else if (a == "--press-system" && i + 1 < argc) pressSystemAt = std::atof(argv[++i]);
        else if (a == "--drop-every" && i + 1 < argc) dropEvery = uint32_t(std::atoi(argv[++i]));
        else if (a == "--interfaces") {
            // Which addresses the PC offers as LAN routes for remote connections.
            for (const auto& itf : ListInterfaces())
                std::printf("%-40s %-16s %s\n", itf.name.c_str(), NetAddress{itf.ip, 0}.IpString().c_str(),
                            itf.isVpn ? "VPN (shown on Home, not offered to the account server)" : itf.isVirtual ? "virtual (not offered)" : "LAN");
            for (const auto& a : ListGlobalIpv6(kStreamPort)) std::printf("%-40s %s\n", "IPv6 (global)", a.ToString().c_str());
            return 0;
        }
        else if (a == "--route" && i + 1 < argc) {
            // Whether packets to this address would leave through a tunnel.
            NetAddress t;
            if (!NetAddress::Parse(argv[++i], 9, t)) return 2;
            std::printf("%s %s\n", t.IpString().c_str(), RoutesThroughTunnel(t.ip) ? "tunnel" : "direct");
            return 0;
        }
        else if (a == "--codec" && i + 1 < argc) codec = argv[++i];
        else if (a == "--out" && i + 1 < argc) outDir = argv[++i];
    }
    if (!desktop && !vr) desktop = true;

    NetAddress pc;
    std::string pcName;
    if (pcIp.empty()) {
        if (!Discover(pc, pcName)) {
            std::printf("No RinCast PC found on the network\n");
            return 1;
        }
        std::printf("Discovered %s at %s\n", pcName.c_str(), pc.ToString().c_str());
    } else if (!NetAddress::Parse(pcIp, kStreamPort, pc)) {
        std::printf("Bad address\n");
        return 1;
    }

    UdpSocket sock;
    sock.Open(0);
    sock.SetBuffers(4 << 20, 16 << 20);
    auto keys = crypto::KeyPair::Generate();
    const std::string nonce = "simnonce";
    json hello{{"deviceId", "sim-device"},
               {"deviceName", "Headset Simulator"},
               {"model", "Simulator"},
               {"appVersion", "1.0.0"},
               {"nonce", nonce},
               {"display",
                {{"eyeWidth", 1680},
                 {"eyeHeight", 1760},
                 {"refreshRates", {72, 90, 120}},
                 {"ipd", 0.064},
                 {"fov", {{1.376, 0.839, 0.966, 1.191}, {0.839, 1.376, 0.966, 1.191}}}}},
               {"decoders", {{"h264", true}, {"hevc", true}, {"hevc10", true}, {"av1", true}, {"maxWidth", 8192}, {"maxHeight", 4320}}}};
    HandshakePacket hp;
    hp.type = PacketType::Hello;
    hp.publicKey = keys.publicKey;
    hp.json = hello.dump();
    auto helloBytes = BuildHandshake(hp);

    std::unique_ptr<Connection> conn;
    uint8_t buf[2048];
    NetAddress from;
    const int64_t hsStart = NowNs();
    int64_t lastHello = 0;
    while (!conn && NowNs() - hsStart < 60'000'000'000LL) {
        if (NowNs() - lastHello > 1'000'000'000LL) {
            sock.SendTo(pc, helloBytes.data(), helloBytes.size());
            lastHello = NowNs();
        }
        const int n = sock.RecvFrom(buf, sizeof(buf), from, 100);
        HandshakePacket reply;
        if (n > 0 && ParseHandshake(buf, size_t(n), reply)) {
            if (reply.type == PacketType::Reject) {
                std::printf("Rejected: %s\n", reply.json.c_str());
                if (reply.json.find("pending_approval") == std::string::npos) return 2;
            } else if (reply.type == PacketType::Welcome) {
                const json w = json::parse(reply.json);
                auto sk = crypto::DeriveSessionKeys(keys.secretKey, reply.publicKey, keys.publicKey, reply.publicKey,
                                                    SessionContext(nonce, w.value("nonce", ""), ""));
                conn = std::make_unique<Connection>(&sock, from, reply.sessionId, sk, false, kMaxDatagramLan);
                std::printf("Connected to %s (session %08x)\n", w.value("name", "?").c_str(), reply.sessionId);
            }
        }
    }
    if (!conn) {
        std::printf("Handshake timed out\n");
        return 3;
    }

    struct StreamOut {
        std::unique_ptr<VideoReassembler> reasm;
        std::ofstream file;
        uint32_t frames = 0, keyframes = 0;
        uint64_t bytes = 0;
        double latencyMsSum = 0;
        // Decoder chain, as in the headset app's VideoDecoder.
        bool hadKey = false, chain = false, needRequest = false;
        uint32_t last = 0, drops = 0, viaRecovery = 0, viaKeyframe = 0, skipped = 0;
        // Queueing delay, measured like the headset app (StreamClient::DelayTracker).
        int64_t bucketMin[10];
        int64_t bucketStart = 0;
        int bucket = -1;
        double delaySum = 0, queueMsSum = 0;
        uint32_t delayCount = 0, queueReports = 0;
        int64_t feedbackStart = 0;
        float lastQueueMs = -1;
        uint64_t lastBytes = 0;
    };
    std::map<uint8_t, StreamOut> streams;
    std::mutex streamsMutex;
    std::atomic<int> vrRunning{0};

    // VR latency, from the tracking sample a frame was rendered for to the moment the
    // whole frame arrived here, split at the PC's timestamps (same clock on one machine).
    std::mutex trackMutex;
    std::map<uint64_t, int64_t> trackSent;  // targetDisplayTimeNs -> sampleTimeNs
    struct Stage {
        double sum = 0, min = 1e9, max = 0;
        uint32_t n = 0;
        void Add(double v) {
            sum += v;
            min = std::min(min, v);
            max = std::max(max, v);
            n++;
        }
    };
    Stage stRender, stQueue, stEncode, stSend, stTotal;
    std::ofstream frameLog;  // --move: "pcSubmitNs sample" per VR frame, for pose-probe --scene
    if (move) frameLog.open(outDir + "/vr-frames.txt");
    std::ofstream sampleLog;  // --move: "sample targetDisplayTimeNs" per tracking sample
    if (move) sampleLog.open(outDir + "/samples.txt");

    auto getStream = [&](uint8_t id) -> StreamOut& {
        auto& s = streams[id];
        if (!s.reasm) {
            s.reasm = std::make_unique<VideoReassembler>([&, id](ReceivedFrame&& f) {
                int64_t sentNs = 0;
                if (f.streamId == kVrStreamId && f.metadata.size() >= sizeof(VrFrameMetadata))
                    sentNs = reinterpret_cast<const VrFrameMetadata*>(f.metadata.data())->pcEncodeEndNs;
                else if (f.streamId != kVrStreamId && f.metadata.size() >= sizeof(DesktopFrameMetadata))
                    sentNs = reinterpret_cast<const DesktopFrameMetadata*>(f.metadata.data())->pcEncodeEndNs;
                if (sentNs) {
                    const int64_t d = f.firstFragmentNs - sentNs, now = f.completeNs;  // like the headset
                    if (s.bucket < 0 || now - s.bucketStart > 2'000'000'000LL) {
                        std::fill(std::begin(s.bucketMin), std::end(s.bucketMin), INT64_MAX);
                        s.bucket = 0;
                        s.bucketStart = now;
                    } else if (now - s.bucketStart >= 1'000'000'000LL) {
                        s.bucket = (s.bucket + 1) % 10;
                        s.bucketMin[s.bucket] = INT64_MAX;
                        s.bucketStart = now;
                    }
                    s.bucketMin[s.bucket] = std::min(s.bucketMin[s.bucket], d);
                    s.delaySum += double(d - *std::min_element(std::begin(s.bucketMin), std::end(s.bucketMin)));
                    s.delayCount++;
                }
                if (f.streamId == kVrStreamId && f.metadata.size() >= sizeof(VrFrameMetadata)) {
                    VrFrameMetadata m;
                    std::memcpy(&m, f.metadata.data(), sizeof(m));
                    std::lock_guard lock(trackMutex);
                    auto it = trackSent.find(m.trackingTimestampNs);
                    if (frameLog.is_open())
                        frameLog << m.pcSubmitNs << " " << std::lround((m.renderPose.position[1] - 1.0f) * 1000) << "\n";
                    if (it != trackSent.end() && m.pcSubmitNs > it->second) {
                        stRender.Add(NsToMs(m.pcSubmitNs - it->second));
                        stQueue.Add(NsToMs(m.pcEncodeStartNs - m.pcSubmitNs));
                        stEncode.Add(NsToMs(m.pcEncodeEndNs - m.pcEncodeStartNs));
                        stSend.Add(NsToMs(f.completeNs - m.pcEncodeEndNs));
                        stTotal.Add(NsToMs(f.completeNs - it->second));
                    }
                }
                const bool key = (f.flags & kVideoKeyframe) != 0, rec = (f.flags & kVideoRecovery) != 0;
                if (dropEvery && s.hadKey && f.frameIndex % dropEvery == 0) {
                    s.drops++;
                    return;
                }
                if (s.hadKey && !key && f.frameIndex <= s.last) return;
                if (key) {
                    if (s.hadKey && !s.chain) s.viaKeyframe++;
                    s.hadKey = s.chain = true;
                } else if (rec && s.hadKey) {
                    if (!s.chain) s.viaRecovery++;
                    s.chain = true;
                } else if (s.chain && f.frameIndex != s.last + 1) {
                    s.chain = false;
                }
                if (!s.chain) {
                    s.skipped++;
                    s.needRequest = true;
                    return;
                }
                s.last = f.frameIndex;
                if (!s.file.is_open()) {
                    const char* ext = f.codec == VideoCodec::H264 ? "h264" : f.codec == VideoCodec::HEVC ? "h265" : "obu";
                    (void)id;
                    s.file.open(outDir + "/stream" + std::to_string(f.streamId) + "." + ext, std::ios::binary);
                }
                s.file.write(reinterpret_cast<const char*>(f.bitstream.data()), std::streamsize(f.bitstream.size()));
                s.frames++;
                s.bytes += f.bitstream.size();
                if (f.flags & kVideoKeyframe) s.keyframes++;
            });
        }
        return s;
    };

    conn->SetControlHandler([&](const std::string& text) {
        const json m = json::parse(text);
        const std::string type = m.value("type", "");
        if (type == msg::kServerInfo) {
            std::printf("server.info: gpu=%s encoder=%s\n", m.value("gpu", "").c_str(), m["encoder"].dump().c_str());
        } else if (type == msg::kMonitors) {
            std::printf("monitors: %zu\n", m["monitors"].size());
        } else if (type == msg::kStreamConfig) {
            std::printf("stream.config (%s): %s\n", m.value("kind", "").c_str(), m["streams"].dump().c_str());
        } else if (type == msg::kVrState) {
            std::printf("vr.state: %s %s %s\n", m.value("state", "").c_str(), m.value("producer", "").c_str(), m.value("app", "").c_str());
            vrRunning = m.value("state", "") == "running";
        } else if (type == msg::kNotice) {
            std::printf("notice: %s\n", m.value("text", "").c_str());
        }
    });
    conn->SetPacketHandler([&](PacketType t, const uint8_t* p, size_t n) {
        if ((t == PacketType::Video || t == PacketType::VideoParity) && n > 0) {
            std::lock_guard lock(streamsMutex);
            auto& s = getStream(p[0]);
            if (t == PacketType::Video)
                s.reasm->HandleFragment(p, n);
            else
                s.reasm->HandleParity(p, n);
        }
    });

    std::atomic<bool> running{true};
    // --limit-mbps: the PC's packets first wait in a queue drained at that rate, like a
    // slow upload or Wi-Fi link with a large router buffer (dropped beyond 1 s of data).
    struct Queued {
        int64_t releaseNs;
        std::vector<uint8_t> data;
        NetAddress from;
    };
    std::deque<Queued> linkQueue;
    std::mutex linkMutex;
    int64_t linkFreeNs = 0;
    uint64_t linkDropped = 0;
    std::thread link([&] {
        while (running && limitMbps > 0) {
            Queued q;
            {
                std::lock_guard lock(linkMutex);
                if (linkQueue.empty() || linkQueue.front().releaseNs > NowNs()) {
                    q.releaseNs = 0;
                } else {
                    q = std::move(linkQueue.front());
                    linkQueue.pop_front();
                }
            }
            if (q.releaseNs)
                conn->HandleDatagram(q.data.data(), q.data.size(), q.from);
            else
                std::this_thread::yield();
        }
    });
    std::thread rx([&] {
        uint8_t b[2048];
        NetAddress f;
        while (running) {
            const int n = sock.RecvFrom(b, sizeof(b), f, 2);
            if (n > 0 && lossPercent > 0 && std::rand() % 100000 < int(lossPercent * 1000)) continue;
            if (n > 0 && limitMbps > 0) {
                const int64_t now = NowNs();
                std::lock_guard lock(linkMutex);
                const int64_t start = std::max(now, linkFreeNs);
                if (start - now > 1'000'000'000LL) {
                    linkDropped++;
                } else {
                    linkFreeNs = start + int64_t(double(n) * 8 / (limitMbps * 1e6) * 1e9);
                    linkQueue.push_back({linkFreeNs, std::vector<uint8_t>(b, b + n), f});
                }
            } else if (n > 0) {
                conn->HandleDatagram(b, size_t(n), f);
            }
            conn->Tick(NowNs());
        }
    });

    conn->SendControl(json{{"type", msg::kClientInfo},
                           {"display", hello["display"]},
                           {"decoders", hello["decoders"]},
                           {"settings", {{"codec", codec}, {"refreshRate", hz}, {"vrQuality", "medium"}, {"streamAudio", false}}}}
                          .dump());
    if (desktop) conn->SendControl(json{{"type", msg::kStartDesktop}}.dump());
    if (vr) conn->SendControl(json{{"type", msg::kLaunchSteamVr}}.dump());

    // Tracking at 90 Hz (needed for VR) and feedback at 10 Hz.
    const int64_t start = NowNs();
    int64_t lastFeedback = 0, lastProgress = 0;
    while (NowNs() - start < int64_t(seconds) * 1'000'000'000LL) {
        TrackingPayload t{};
        t.sampleTimeNs = uint64_t(NowNs());
        t.targetDisplayTimeNs = t.sampleTimeNs + 40'000'000;
        t.flags = kTrackHmdValid;
        const double a = (NowNs() - start) / 1e9;
        t.hmd.orientation[1] = still ? 0.f : float(std::sin(a * 0.5) * 0.2);
        t.hmd.orientation[3] = std::sqrt(1 - t.hmd.orientation[1] * t.hmd.orientation[1]);
        t.hmd.position[1] = 1.6f;
        // With --move, the head height carries the sample number (in mm steps), so the
        // pose a VR app rendered with can be matched to the pose a frame was tagged with.
        static uint32_t sampleNo = 0;
        if (move) t.hmd.position[1] = 1.0f + float(++sampleNo % 500) * 0.001f;
        if (move) sampleLog << sampleNo % 500 << " " << t.targetDisplayTimeNs << "\n";
        for (int h = 0; h < 2; h++) {
            t.controllers[h].flags = kCtrlConnected | kCtrlPoseValid;
            t.controllers[h].pose.orientation[3] = 1;
            t.controllers[h].pose.position[0] = h ? 0.2f : -0.2f;
            t.controllers[h].pose.position[1] = 1.2f;
            t.controllers[h].pose.position[2] = -0.3f;
            if (move && h == 1) {
                // The path pose-probe expects, predicted for the target time like the headset does.
                const double c = std::fmod(double(t.targetDisplayTimeNs) / 1e9, 8.0);
                t.controllers[h].pose.position[0] = c < 4 ? float(c * 0.5 - 1.0) : 0.f;
                t.controllers[h].linearVelocity[0] = c < 4 ? 0.5f : 0.f;
            }
        }
        {
            std::lock_guard lock(trackMutex);
            trackSent[t.targetDisplayTimeNs] = int64_t(t.sampleTimeNs);
            while (trackSent.size() > 512) trackSent.erase(trackSent.begin());
        }
        // --press-system S: press the left menu (SteamVR system) button S seconds after VR
        // started, to close the dashboard SteamVR opens on its own.
        static int64_t vrSince = 0;
        if (vrRunning && !vrSince) vrSince = NowNs();
        if (pressSystemAt > 0 && vrSince) {
            const double s = (NowNs() - vrSince) / 1e9;
            if (s >= pressSystemAt && s < pressSystemAt + 0.2) t.controllers[0].buttons |= kBtnMenu;
        }
        if (hands || fingers) {
            // Open hands; each bone's x says which bone it is, so a probe can check them.
            for (int h = 0; h < 2; h++) {
                if (hands) t.controllers[h].flags |= kCtrlHandTracked;
                HandSkeletonPayload p{};
                p.hand = uint8_t(h);
                for (int i = 0; i < kHandBoneCount; i++) {
                    p.bones[i].orientation[3] = 1;
                    p.bones[i].position[0] = 0.001f * float(i + 1);
                }
                conn->Send(PacketType::HandSkeleton, &p, sizeof(p));
            }
        }
        conn->Send(PacketType::Tracking, &t, sizeof(t));
        if (body) {
            // Each tracker at its own known height: y = 0.1 * (index + 1) metres.
            BodyTrackingPayload b{};
            b.validMask = (1u << kBodyTrackerCount) - 1;
            for (int i = 0; i < kBodyTrackerCount; i++) {
                b.poses[i].orientation[3] = 1;
                b.poses[i].position[1] = 0.1f * float(i + 1);
            }
            conn->Send(PacketType::BodyTracking, &b, sizeof(b));
        }
        if (NowNs() - lastProgress > 1'000'000'000LL) {
            lastProgress = NowNs();
            std::lock_guard lock(streamsMutex);
            for (auto& [id, s] : streams) {
                std::printf("progress unix_ms=%lld stream=%u frames=%u mbps=%.1f queue_ms=%.0f\n",
                            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count(),
                            unsigned(id), s.frames, (s.bytes - s.lastBytes) * 8 / 1e6, s.lastQueueMs);
                s.lastBytes = s.bytes;
            }
            std::fflush(stdout);
        }
        bool urgent = false;
        {
            std::lock_guard lock(streamsMutex);
            for (auto& [id, s] : streams) urgent |= s.needRequest;
        }
        if (urgent || NowNs() - lastFeedback > 100'000'000LL) {
            lastFeedback = NowNs();
            std::lock_guard lock(streamsMutex);
            for (auto& [id, s] : streams) {
                const bool lost = s.reasm->Expire(NowNs(), 300'000'000LL);
                const auto st = s.reasm->TakeStats();
                VideoFeedbackPayload fb{};
                fb.streamId = id;
                fb.requestKeyframe = (lost || s.needRequest) ? 1 : 0;
                s.needRequest = false;
                fb.lastCompleteFrame = s.reasm->LastCompleteFrame();
                fb.lastGoodFrame = s.hadKey ? s.last : 0;
                fb.framesLost = st.framesLost;
                fb.fragmentsReceived = st.fragmentsReceived;
                fb.fragmentsLost = st.fragmentsLost;
                fb.fragmentsRecovered = st.fragmentsRecovered;
                fb.queueDelayMs = s.delayCount ? float(s.delaySum / s.delayCount / 1e6) : -1.f;
                if (s.delayCount) s.queueMsSum += fb.queueDelayMs, s.queueReports++;
                if (s.delayCount) s.lastQueueMs = fb.queueDelayMs;
                s.delaySum = 0;
                s.delayCount = 0;
                const int64_t window = std::max<int64_t>(NowNs() - s.feedbackStart, 1'000'000);
                s.feedbackStart = NowNs();
                fb.receivedKbps = uint32_t(st.bytesReceived * 8 * 1000 / uint64_t(window / 1000));
                conn->Send(PacketType::VideoFeedback, &fb, sizeof(fb));
            }
        }
        SleepMs(int(1000 / hz) - 1);  // about one sample per headset frame
    }

    conn->SendControl(json{{"type", msg::kStopDesktop}}.dump());
    SleepMs(200);
    conn->SendDisconnect();
    running = false;
    rx.join();
    link.join();

    const auto cs = conn->Stats();
    std::printf("\nRTT %.2f ms, %llu packets received, clock offset %lld us\n", cs.rttMs,
                (unsigned long long)cs.packetsReceived, (long long)(conn->ClockOffsetNs() / 1000));
    int ok = 0;
    for (auto& [id, s] : streams) {
        std::printf("stream %u: %u frames (%u keyframes), %.1f Mbps avg\n", id, s.frames, s.keyframes,
                    s.bytes * 8.0 / 1e6 / seconds);
        if (s.queueReports)
            std::printf("stream %u: average queueing delay %.1f ms\n", id, s.queueMsSum / s.queueReports);
        if (dropEvery)
            std::printf("stream %u: %u frames dropped, resumed %u times with a recovery frame and %u times with a "
                        "keyframe, %u frames skipped while waiting\n",
                        id, s.drops, s.viaRecovery, s.viaKeyframe, s.skipped);
        if (s.frames > 0) ok++;
    }
    if (stTotal.n) {
        std::printf("\nVR latency over %u frames (avg / min / max ms):\n", stTotal.n);
        auto row = [](const char* name, const Stage& s) {
            std::printf("  %-38s %6.2f %6.2f %6.2f\n", name, s.sum / s.n, s.min, s.max);
        };
        row("tracking sample -> game frame submitted", stRender);
        row("submitted -> encode start", stQueue);
        row("encode", stEncode);
        row("encoded -> received here", stSend);
        row("total", stTotal);
    }
    return ok > 0 ? 0 : 4;
}
