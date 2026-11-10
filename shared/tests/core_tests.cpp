// Loopback tests for the transport: handshake key agreement, reliable control
// messages under loss, video FEC recovery and discovery parsing.
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <random>
#include <thread>
#include <limits>

#include "rc/audio_codec.h"
#include "rc/connection.h"
#include "rc/controller_tracking.h"
#include "rc/discovery.h"
#include "rc/hand_skeleton.h"
#include "rc/handshake.h"
#include "rc/motion_field.h"
#include "rc/music.h"
#include "rc/platform.h"
#include "rc/video_transport.h"

using namespace rc;

static int g_failures = 0;
#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                              \
        }                                                              \
    } while (0)

struct Peer {
    UdpSocket socket;
    std::unique_ptr<Connection> conn;
    std::thread thread;
    std::atomic<bool> stop{false};
    double dropRate = 0;
    std::mt19937 rng{1234};

    void Run() {
        thread = std::thread([this] {
            uint8_t buf[2048];
            NetAddress from;
            std::uniform_real_distribution<double> dist(0, 1);
            while (!stop) {
                const int n = socket.RecvFrom(buf, sizeof(buf), from, 5);
                if (n > 0) {
                    if (dist(rng) < dropRate) continue;
                    conn->HandleDatagram(buf, size_t(n), from);
                }
                conn->Tick(NowNs());
            }
        });
    }
    void Stop() {
        stop = true;
        if (thread.joinable()) thread.join();
    }
};

static void TestHandshakeAndTransport() {
    auto client = crypto::KeyPair::Generate();
    auto server = crypto::KeyPair::Generate();
    const std::string ctx = SessionContext("cn", "sn", "secret");
    auto ck = crypto::DeriveSessionKeys(client.secretKey, server.publicKey, client.publicKey, server.publicKey, ctx);
    auto sk = crypto::DeriveSessionKeys(server.secretKey, client.publicKey, client.publicKey, server.publicKey, ctx);
    CHECK(ck.clientToServer == sk.clientToServer);
    CHECK(ck.serverToClient == sk.serverToClient);

    HandshakePacket hello;
    hello.type = PacketType::Hello;
    hello.publicKey = client.publicKey;
    hello.json = R"({"deviceId":"x"})";
    auto bytes = BuildHandshake(hello);
    HandshakePacket parsed;
    CHECK(IsHandshakeDatagram(bytes.data(), bytes.size()));
    CHECK(ParseHandshake(bytes.data(), bytes.size(), parsed));
    CHECK(parsed.publicKey == client.publicKey && parsed.json == hello.json);
    CHECK(HelloProof("s", client.publicKey) == HelloProof("s", client.publicKey));
    CHECK(HelloProof("s", client.publicKey) != HelloProof("t", client.publicKey));

    Peer a, b;
    CHECK(a.socket.Open(0));
    CHECK(b.socket.Open(0));
    NetAddress aAddr{0x7F000001, a.socket.LocalPort()};
    NetAddress bAddr{0x7F000001, b.socket.LocalPort()};
    a.conn = std::make_unique<Connection>(&a.socket, bAddr, 42, sk, true, kMaxDatagramLan);
    b.conn = std::make_unique<Connection>(&b.socket, aAddr, 42, ck, false, kMaxDatagramLan);
    a.dropRate = 0.2;
    b.dropRate = 0.2;

    std::atomic<int> received{0};
    std::string big(100000, 'x');
    for (size_t i = 0; i < big.size(); i++) big[i] = char('a' + i % 26);
    std::atomic<bool> bigOk{false};
    std::atomic<int> order{0};
    b.conn->SetControlHandler([&](const std::string& m) {
        if (m.size() == big.size()) {
            bigOk = (m == big);
        } else {
            const int idx = std::atoi(m.c_str());
            if (idx != order.load()) std::printf("out of order %d vs %d\n", idx, order.load());
            order = idx + 1;
        }
        received++;
    });

    // Video: frames with FEC through a lossy link.
    std::atomic<int> framesOk{0};
    VideoReassembler reasm([&](ReceivedFrame&& f) {
        bool ok = f.bitstream.size() == 50000 + f.frameIndex;
        for (size_t i = 0; ok && i < f.bitstream.size(); i++) ok = f.bitstream[i] == uint8_t(i * 7 + f.frameIndex);
        if (ok) framesOk++;
    });
    b.conn->SetPacketHandler([&](PacketType t, const uint8_t* p, size_t n) {
        if (t == PacketType::Video) reasm.HandleFragment(p, n);
        if (t == PacketType::VideoParity) reasm.HandleParity(p, n);
    });

    a.Run();
    b.Run();
    for (int i = 0; i < 200; i++) a.conn->SendControl(std::to_string(i));
    a.conn->SendControl(big);

    const int64_t start = NowNs();
    while (received < 201 && NowNs() - start < 10'000'000'000LL) SleepMs(10);
    CHECK(received == 201);
    CHECK(bigOk);
    CHECK(order == 200);

    // Low loss for video to make FEC meaningful (single loss per group recoverable).
    b.dropRate = 0.01;
    a.dropRate = 0;
    SleepMs(200);
    VideoPacketizer pkt(a.conn.get());
    pkt.SetFecPercent(10);
    for (uint32_t f = 1; f <= 60; f++) {
        std::vector<uint8_t> data(50000 + f);
        for (size_t i = 0; i < data.size(); i++) data[i] = uint8_t(i * 7 + f);
        DesktopFrameMetadata meta{};
        pkt.SendFrame(1, VideoCodec::HEVC, 0, f, &meta, sizeof(meta), data.data(), data.size());
        SleepMs(5);
    }
    SleepMs(300);
    auto stats = reasm.TakeStats();
    std::printf("video: %d/60 frames ok, %u recovered, %u lost frags, rtt %.2fms, offset %lldns\n", framesOk.load(),
                stats.fragmentsRecovered, stats.fragmentsLost, b.conn->Stats().rttMs,
                (long long)b.conn->ClockOffsetNs());
    CHECK(framesOk >= 50);
    CHECK(std::llabs(b.conn->ClockOffsetNs()) < 2'000'000);  // same machine => ~0

    a.Stop();
    b.Stop();
}

static void TestIdentity() {
    const auto seed = crypto::NewIdentitySeed();
    const auto id = crypto::IdentityPublicKey(seed);
    const auto eph = crypto::KeyPair::Generate();
    const auto sig = crypto::SignConnection(seed, eph.publicKey, "nonce");
    CHECK(crypto::VerifyConnection(id, eph.publicKey, "nonce", sig));
    CHECK(!crypto::VerifyConnection(id, eph.publicKey, "other", sig));
    CHECK(!crypto::VerifyConnection(id, crypto::KeyPair::Generate().publicKey, "nonce", sig));  // a replay can't reuse it
    CHECK(!crypto::VerifyConnection(crypto::IdentityPublicKey(crypto::NewIdentitySeed()), eph.publicKey, "nonce", sig));
}

static void TestDiscovery() {
    DiscoveryAnnounce a;
    a.serverId = "abc";
    a.name = "Gaming PC";
    a.accountLinked = true;
    a.nonce = 7;
    auto bytes = BuildAnnounce(a);
    DiscoveryAnnounce b;
    CHECK(ParseAnnounce(bytes.data(), bytes.size(), b));
    CHECK(b.serverId == "abc" && b.name == "Gaming PC" && b.accountLinked && b.nonce == 7);
    DiscoveryProbe p;
    CHECK(!ParseProbe(bytes.data(), bytes.size(), p));
}

static void TestAudio() {
    AudioEncoder enc;
    AudioDecoder dec;
    CHECK(enc.Init(48000, 2, 128000, true));
    CHECK(dec.Init(48000, 2));
    std::vector<int16_t> pcm(480 * 2);
    for (int i = 0; i < 480; i++) pcm[i * 2] = pcm[i * 2 + 1] = int16_t(8000 * std::sin(i * 0.05));
    uint8_t packet[1500];
    const int n = enc.Encode(pcm.data(), 480, packet, sizeof(packet));
    CHECK(n > 0);
    std::vector<int16_t> out(5760 * 2);
    CHECK(dec.Decode(packet, n, out.data(), 5760) == 480);

    JitterBuffer jb;
    jb.Configure(2, 48000, 20, 100);
    for (int i = 0; i < 3; i++) jb.Push(pcm.data(), 480);
    std::vector<int16_t> o(480 * 2);
    CHECK(jb.Pop(o.data(), 480) == 480);
    // Running dry after playback started means the network was late: hold 20 ms more.
    CHECK(jb.TargetMs() == 20);
    for (int i = 0; i < 3; i++) jb.Pop(o.data(), 480);
    CHECK(jb.TargetMs() == 40);
    CHECK(jb.Pop(o.data(), 480) == 0);  // priming again, to the new target
}

// Reordering: parity first must not lose the frame's flags; pacing spreads a frame out.
static void TestVideoReassemblyOrder() {
    std::vector<ReceivedFrame> done;
    VideoReassembler r([&](ReceivedFrame&& f) { done.push_back(std::move(f)); });
    const uint8_t payload[] = {0, 0, 'a', 'b', 'c', 'd'};  // no metadata
    VideoFragmentHeader h{};
    h.streamId = 1;
    h.codec = uint8_t(VideoCodec::HEVC);
    h.frameFlags = kVideoKeyframe;
    h.fecGroupSize = 2;
    h.frameIndex = 7;
    h.fragmentCount = 2;
    h.frameSize = sizeof(payload);
    VideoParityHeader ph{};
    ph.streamId = 1;
    ph.fecGroupSize = 2;
    ph.groupIndex = 0;
    ph.frameIndex = 7;
    ph.fragmentCount = 2;
    ph.lastFragmentSize = 3;
    ph.frameSize = sizeof(payload);
    std::vector<uint8_t> parity(sizeof(ph) + 3);
    std::memcpy(parity.data(), &ph, sizeof(ph));
    for (int i = 0; i < 3; i++) parity[sizeof(ph) + i] = uint8_t(payload[i] ^ payload[i + 3]);
    r.HandleParity(parity.data(), parity.size());
    std::vector<uint8_t> frag(sizeof(h) + 3);
    h.fragmentIndex = 0;
    std::memcpy(frag.data(), &h, sizeof(h));
    std::memcpy(frag.data() + sizeof(h), payload, 3);
    r.HandleFragment(frag.data(), frag.size());
    h.fragmentIndex = 1;
    std::memcpy(frag.data(), &h, sizeof(h));
    std::memcpy(frag.data() + sizeof(h), payload + 3, 3);
    r.HandleFragment(frag.data(), frag.size());
    CHECK(done.size() == 1);
    if (!done.empty()) {
        CHECK(done[0].codec == VideoCodec::HEVC);
        CHECK(done[0].flags == kVideoKeyframe);
        CHECK(done[0].firstFragmentNs <= done[0].completeNs);
    }
}

static void TestPacing() {
    UdpSocket a, b;
    CHECK(a.Open(0) && b.Open(0));
    auto keys = crypto::DeriveSessionKeys(crypto::KeyPair::Generate().secretKey, crypto::KeyPair::Generate().publicKey,
                                          crypto::KeyPair::Generate().publicKey, crypto::KeyPair::Generate().publicKey, "t");
    Connection conn(&a, NetAddress{0x7F000001, b.LocalPort()}, 1, keys, true, kMaxDatagramLan);
    VideoPacketizer p(&conn);
    p.SetFecPercent(0);
    std::vector<uint8_t> data(100000);
    DesktopFrameMetadata meta{};
    int64_t t = NowNs();
    p.SendFrame(1, VideoCodec::HEVC, 0, 1, &meta, sizeof(meta), data.data(), data.size());
    const double unpacedMs = NsToMs(NowNs() - t);
    p.SetPacingKbps(8000);  // 100 kB at 1 MB/s: about 100 ms
    t = NowNs();
    p.SendFrame(1, VideoCodec::HEVC, 0, 2, &meta, sizeof(meta), data.data(), data.size());
    const double pacedMs = NsToMs(NowNs() - t);
    std::printf("pacing: %.1f ms unpaced, %.1f ms at 8 Mbit/s\n", unpacedMs, pacedMs);
    CHECK(pacedMs > 80 && pacedMs < 130);
}

static void TestAddresses() {
    NetAddress a;
    CHECK(NetAddress::ParseEndpoint("192.168.1.20:38711", a) && !a.v6 && a.ip == 0xC0A80114 && a.port == 38711);
    CHECK(a.ToString() == "192.168.1.20:38711");
    CHECK(NetAddress::ParseEndpoint("[2001:db8::20]:38711", a) && a.v6 && a.port == 38711 && a.IsGlobalV6());
    CHECK(a.ToString() == "[2001:db8::20]:38711");
    NetAddress b;
    CHECK(NetAddress::ParseEndpoint(a.ToString(), b) && b == a);
    CHECK(!NetAddress::ParseEndpoint("2001:db8::20", a));  // IPv6 without brackets/port
    CHECK(!NetAddress::ParseEndpoint("1.2.3.4", a));
    CHECK(!NetAddress::ParseEndpoint("1.2.3.4:99999", a));
    CHECK(!NetAddress::Parse("", 1, a));  // empty host must not resolve to this machine
    CHECK(NetAddress::Parse("[fe80::1]", 5, a) && a.v6 && !a.IsGlobalV6());
    CHECK(IsPrivateLan(0x0A000001) && IsPrivateLan(0xAC100001) && IsPrivateLan(0xC0A80001) && !IsPrivateLan(0x08080808));
    CHECK(IsCgnat(0x64410001) && !IsCgnat(0x64000001));  // 100.65.0.1 yes, 100.0.0.1 no
    const NetAddress empty{}, one{1, 1};
    CHECK(!empty.valid() && one.valid());

    // One dual-stack socket talks to IPv4 and IPv6 peers; IPv4 senders show up as plain IPv4.
    UdpSocket s, v4peer, v6peer;
    CHECK(s.Open(0) && v4peer.Open(0) && v6peer.Open(0));
    if (!s.DualStack()) {
        std::printf("note: no IPv6 on this system, skipping dual-stack checks\n");
        return;
    }
    uint8_t buf[64];
    NetAddress from;
    const NetAddress sV4{0x7F000001, s.LocalPort()};
    CHECK(v4peer.SendTo(sV4, "four", 4));
    CHECK(s.RecvFrom(buf, sizeof(buf), from, 1000) == 4 && !from.v6 && from.ip == 0x7F000001 &&
          from.port == v4peer.LocalPort());
    NetAddress sV6;
    CHECK(NetAddress::Parse("::1", s.LocalPort(), sV6) && sV6.v6);
    CHECK(v6peer.SendTo(sV6, "six", 3));
    CHECK(s.RecvFrom(buf, sizeof(buf), from, 1000) == 3 && from.v6 && from.port == v6peer.LocalPort());
    CHECK(s.SendTo(from, "back", 4));  // reply over IPv6
    NetAddress back;
    CHECK(v6peer.RecvFrom(buf, sizeof(buf), back, 1000) == 4 && back.v6 && back.port == s.LocalPort());
}

// An open hand, palm down, fingers along -Z, with the wrist 5 cm behind the palm.
static void FlatHand(int hand, rc::Pose joints[rc::kHandJointCount]) {
    const float side = hand == 0 ? 1.f : -1.f;  // the thumb's side
    for (int i = 0; i < rc::kHandJointCount; i++) joints[i] = {{0, 0, 0, 1}, {0, 0, 0}};
    joints[1].position[2] = 0.05f;
    for (int finger = 1; finger <= 4; finger++) {
        const int base = 1 + finger * 5;
        const float x = side * (0.03f - 0.02f * float(finger - 1));
        for (int i = 0; i < 5; i++) {
            joints[base + i].position[0] = x;
            joints[base + i].position[2] = 0.035f - 0.04f * float(i);
        }
    }
    for (int i = 0; i < 4; i++) {
        joints[2 + i].position[0] = side * 0.04f;
        joints[2 + i].position[2] = 0.03f - 0.03f * float(i);
    }
}

static void TestHandSkeleton() {
    rc::Pose joints[rc::kHandJointCount], bones[rc::kHandBoneCount];
    for (int hand = 0; hand < 2; hand++) {
        FlatHand(hand, joints);
        rc::HandJointsToBones(hand, joints, bones);
        const float along = hand == 0 ? 1.f : -1.f;
        // Finger bones lie along +X on the left hand and -X on the right, as in SteamVR's own poses.
        for (int i : {7, 8, 9, 10, 12, 17, 22}) {
            CHECK(std::fabs(bones[i].position[0] - along * 0.04f) < 1e-4f);
            CHECK(std::fabs(bones[i].position[1]) < 1e-4f);
            CHECK(std::fabs(bones[i].position[2]) < 1e-4f);
            CHECK(std::fabs(bones[i].orientation[3] - 1) < 1e-4f);
        }
        // SteamVR's open hand has the index metacarpal 2.7 cm from the wrist's y, 1.5 cm in z.
        CHECK(std::fabs(bones[6].position[1] - 0.03f) < 0.012f);
        CHECK(std::fabs(bones[6].position[2] - 0.015f) < 0.012f);
        // SteamVR's held hand has the wrist 3.4 cm to the side, 3.7 cm up and 16.5 cm behind the controller.
        CHECK(std::fabs(bones[1].position[0] + along * 0.034f) < 0.03f);
        CHECK(std::fabs(bones[1].position[1] - 0.0365f) < 0.03f);
        CHECK(std::fabs(bones[1].position[2] - 0.1647f) < 0.03f);
        // The wrist's orientation is close to the one in SteamVR's held pose.
        const float sw = hand == 0 ? 1.f : -1.f;
        const float q[4] = {-0.078608f * 1, -0.920279f * sw, 0.379296f * sw, -0.055147f};
        const float dot = std::fabs(bones[1].orientation[0] * q[0] + bones[1].orientation[1] * q[1] +
                                    bones[1].orientation[2] * q[2] + bones[1].orientation[3] * q[3]);
        CHECK(dot > 0.8f);
        CHECK(rc::HandGrip(joints) == 0);
        // The controller's pose stays the same distance from the palm on both hands.
        rc::Pose palm{{0, 0, 0, 1}, {0.1f, 1.2f, -0.3f}};
        const rc::Pose device = rc::HandDevicePose(hand, palm);
        const float dx = device.position[0] - palm.position[0], dy = device.position[1] - palm.position[1],
                    dz = device.position[2] - palm.position[2];
        CHECK(std::fabs(std::sqrt(dx * dx + dy * dy + dz * dz) - std::sqrt(0.04f * 0.04f + 0.02f * 0.02f + 0.13f * 0.13f)) < 1e-3f);

        // Closing the lower three fingers grips; the index alone doesn't.
        for (int finger = 2; finger <= 4; finger++) {
            const int base = 1 + finger * 5;
            for (int i = 1; i < 4; i++) {
                const float half = 0.6f * float(i) / 2;
                joints[base + i].orientation[0] = std::sin(half);
                joints[base + i].orientation[3] = std::cos(half);
            }
        }
        CHECK(rc::HandFingerCurl(joints, 1) == 0);
        CHECK(rc::HandFingerCurl(joints, 2) > 0.5f);
        CHECK(rc::HandGrip(joints) > 0.1f);
    }
}

static void TestSubnetHosts() {
    const uint32_t ip = (192u << 24) | (168u << 16) | (178u << 8) | 110u;
    const auto all = rc::SubnetHosts(ip, 0xFFFFFF00u, 4096);
    CHECK(all.size() == 253);  // 256 less the network, the broadcast and ourselves
    CHECK(all[0] == ip - 1 && all[1] == ip + 1);
    bool bad = false;
    for (const uint32_t h : all) bad = bad || h == ip || (h & 255) == 0 || (h & 255) == 255 || (h >> 8) != (ip >> 8);
    CHECK(!bad);
    // A wide network: the nearest ones, and no more than asked.
    const auto wide = rc::SubnetHosts(ip, 0xFFFF0000u, 1024);
    CHECK(wide.size() == 1024);
    CHECK(wide.back() >> 16 == ip >> 16);
    // At the edge of the subnet it still finds them all.
    CHECK(rc::SubnetHosts((ip & 0xFFFFFF00u) | 1u, 0xFFFFFF00u, 4096).size() == 253);
    CHECK(rc::SubnetHosts((10u << 24) | 1u, 0xFFFFFFFCu, 10).size() == 1);  // a /30 has one other host
    CHECK(rc::SubnetHosts(ip, 0xFFFFFFFFu, 10).empty());
    CHECK(rc::SubnetHosts(ip, 0xFFFFFFFEu, 10).empty());
    // Tailscale's range is a tunnel wherever it's routed; loopback never is.
    CHECK(rc::RoutesThroughTunnel((100u << 24) | (64u << 16) | 1u));
    CHECK(!rc::RoutesThroughTunnel((127u << 24) | 1u));
    printf("subnet hosts OK\n");
}

static void TestMotionRuns() {
    std::vector<int8_t> grid(2 * 1000, 0);
    grid[2 * 3] = 5;
    grid[2 * 3 + 1] = -7;
    for (int i = 400; i < 800; i++) grid[2 * i] = int8_t(i % 100 + 1);
    std::vector<uint8_t> runs;
    rc::EncodeMotionRuns(grid.data(), 1000, runs);
    std::vector<int8_t> back;
    CHECK(rc::DecodeMotionRuns(runs.data(), runs.size(), 1000, back));
    CHECK(back == grid);
    // A still picture costs next to nothing; damaged data is refused.
    std::vector<int8_t> still(2 * 1000, 0);
    std::vector<uint8_t> stillRuns;
    rc::EncodeMotionRuns(still.data(), 1000, stillRuns);
    CHECK(stillRuns.size() < 16);
    CHECK(!rc::DecodeMotionRuns(runs.data(), runs.size() - 1, 1000, back));
    CHECK(!rc::DecodeMotionRuns(runs.data(), runs.size(), 999, back));
}

// An Ogg Opus file of a stereo sine, built the way an encoder lays it out (no CRCs: the
// reader doesn't check them).
static rc::TrackData MakeOggOpus(double hz, int packets, int trimEnd) {
    std::vector<uint8_t> file;
    int64_t pageNo = 0;
    auto page = [&](const std::vector<std::vector<uint8_t>>& ps, uint64_t granule, uint8_t flags) {
        std::vector<uint8_t> lacing;
        for (const auto& p : ps) {
            size_t n = p.size();
            for (; n >= 255; n -= 255) lacing.push_back(255);
            lacing.push_back(uint8_t(n));
        }
        const uint8_t head[] = {'O', 'g', 'g', 'S', 0, flags};
        file.insert(file.end(), head, head + 6);
        for (int i = 0; i < 8; i++) file.push_back(uint8_t(granule >> (8 * i)));
        for (int i = 0; i < 4; i++) file.push_back(uint8_t(i == 0 ? 1 : 0));  // serial
        for (int i = 0; i < 4; i++) file.push_back(uint8_t(pageNo >> (8 * i)));
        for (int i = 0; i < 4; i++) file.push_back(0);  // CRC
        file.push_back(uint8_t(lacing.size()));
        file.insert(file.end(), lacing.begin(), lacing.end());
        for (const auto& p : ps) file.insert(file.end(), p.begin(), p.end());
        pageNo++;
    };
    const int preSkip = 312;
    std::vector<uint8_t> opusHead = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, 2, uint8_t(preSkip & 255), uint8_t(preSkip >> 8),
                                     0x80, 0xBB, 0, 0, 0, 0, 0};
    page({opusHead}, 0, 2);
    std::vector<uint8_t> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 0, 0, 0, 0, 0, 0, 0, 0};
    page({tags}, 0, 0);
    AudioEncoder enc;
    enc.Init(48000, 2, 96000, false);
    std::vector<std::vector<uint8_t>> pending;
    int16_t pcm[960 * 2];
    uint8_t buf[4000];
    int64_t t = 0;
    for (int k = 0; k < packets; k++) {
        for (int i = 0; i < 960; i++, t++) pcm[2 * i] = pcm[2 * i + 1] = int16_t(8000 * std::sin(6.2831853 * hz * double(t) / 48000));
        const int n = enc.Encode(pcm, 960, buf, sizeof(buf));
        pending.emplace_back(buf, buf + std::max(n, 0));
        const bool last = k == packets - 1;
        if (pending.size() == 50 || last) {
            // The granule position counts decoded samples, the pre-skip among them.
            page(pending, uint64_t((k + 1) * 960 - (last ? trimEnd : 0)), last ? 4 : 0);
            pending.clear();
        }
    }
    return std::make_shared<const std::vector<uint8_t>>(std::move(file));
}

struct Level {
    double rms = 0;
    double hz = 0;  // from zero crossings
};

static Level Measure(const int16_t* stereo, int frames) {
    Level l;
    double sum = 0;
    int crossings = 0;
    for (int i = 0; i < frames; i++) {
        sum += double(stereo[2 * i]) * stereo[2 * i];
        if (i && (stereo[2 * i] >= 0) != (stereo[2 * i - 2] >= 0)) crossings++;
    }
    l.rms = std::sqrt(sum / frames);
    l.hz = crossings * 48000.0 / (2.0 * frames);
    return l;
}

static void TestMusic() {
    // The reader: exact length (pre-skip and the end trim honoured), and the sound itself.
    {
        rc::OggOpusReader r;
        CHECK(r.Open(MakeOggOpus(440, 120, 300)));
        CHECK(r.TotalFrames() == 120 * 960 - 300 - 312);
        std::vector<int16_t> all, chunk(1000 * 2);
        for (int n; (n = r.Read(chunk.data(), 1000)) > 0;) all.insert(all.end(), chunk.begin(), chunk.begin() + n * 2);
        CHECK(int64_t(all.size() / 2) == 120 * 960 - 300 - 312);
        const Level mid = Measure(all.data() + 48000 * 2, 4800);
        CHECK(mid.rms > 4000 && mid.rms < 7000);
        CHECK(std::abs(mid.hz - 440) < 15);
        rc::OggOpusReader bad;
        CHECK(!bad.Open(std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{'O', 'g', 'g'})));
    }

    // The mixer, 100 ms at a time.
    rc::MusicMixer mix;
    mix.SetTracks({MakeOggOpus(440, 1000, 0), MakeOggOpus(660, 1000, 0)}, 7);  // two 20 s tracks
    std::vector<int16_t> block(4800 * 2);
    auto step = [&] {
        mix.Render(block.data(), 4800);
        return Measure(block.data(), 4800);
    };
    CHECK(mix.Idle());
    mix.SetActive(true);
    const Level first = step();
    std::vector<Level> levels;
    for (int i = 0; i < 59; i++) levels.push_back(step());
    CHECK(first.rms < levels[50].rms * 0.3);  // faded in, not started at full level
    const double played = levels.back().hz;
    CHECK(std::abs(played - 440) < 15 || std::abs(played - 660) < 15);

    // Connected: silent within 2 s, and done.
    mix.SetActive(false);
    Level l;
    for (int i = 0; i < 20; i++) l = step();
    CHECK(l.rms < levels.back().rms * 0.1);  // the last 100 ms of the fade
    l = step();
    CHECK(l.rms < 1);
    CHECK(mix.Idle());

    // Disconnected again: a new track (here, with two, the other one), not the old one resumed.
    mix.SetActive(true);
    for (int i = 0; i < 60; i++) l = step();
    CHECK(std::abs(l.hz - played) > 100);

    // Through the end of that track into the next: no gap, no dip, no jump in loudness.
    double low = 1e9, high = 0;
    for (int i = 0; i < 200; i++) {
        l = step();
        low = std::min(low, l.rms);
        high = std::max(high, l.rms);
    }
    CHECK(low > 4000);
    CHECK(high < 7500);
    CHECK(std::abs(l.hz - played) < 15);  // and it's the other track again
    std::printf("music OK\n");
}

// The path a Quest on a USB cable takes: IPv6 link-local only, asking every device on the
// link. A socket that joined ff02::1 on an adapter hears what's sent there, from an address
// that carries the adapter as its scope, and can answer it directly.
static void TestLinkLocalDiscovery() {
    CHECK(rc::NetAddress::AllNodesV6(9, 3).ToString() == "[ff02::1%3]:9");
    rc::NetAddress ll;
    CHECK(rc::NetAddress::Parse("fe80::1", 5, ll) && ll.IsLinkLocalV6() && !ll.IsGlobalV6());
    rc::UdpSocket listener, sender;
    if (!listener.OpenV6(0) || !sender.Open(0)) {
        std::printf("link-local discovery skipped: no IPv6\n");
        return;
    }
    const uint16_t port = listener.LocalPort();
    int tried = 0, heard = 0;
    for (const auto& itf : rc::ListInterfacesV6()) {
        if (!listener.JoinGroupV6(rc::NetAddress::AllNodesV6(0, itf.index), itf.index)) continue;
        tried++;
        const char msg[] = "probe";
        sender.SendTo(rc::NetAddress::AllNodesV6(port, itf.index), msg, sizeof(msg));
        char buf[64];
        rc::NetAddress from;
        for (int i = 0; i < 10; i++) {
            const int n = listener.RecvFrom(buf, sizeof(buf), from, 50);
            if (n == int(sizeof(msg)) && from.IsLinkLocalV6()) {
                heard++;
                CHECK(from.scope == itf.index);
                // And the answer goes straight back, over the same adapter.
                CHECK(listener.SendTo(from, "announce", 8));
                char back[64];
                rc::NetAddress replyFrom;
                CHECK(sender.RecvFrom(back, sizeof(back), replyFrom, 200) == 8);
                break;
            }
        }
    }
    std::printf("link-local discovery: %d of %d adapters\n", heard, tried);
    CHECK(tried == 0 || heard > 0);
}

static void TestControllerTrackingExtensions() {
    constexpr size_t offset = sizeof(TrackingPayload) + sizeof(GazePayload);
    std::vector<uint8_t> packet(offset + sizeof(ControllerAimPayload) + 1);
    ControllerAimPayload source{}, decoded{};
    source.validMask = 0xff;
    source.poses[0].orientation[3] = 1;
    source.poses[1].orientation[3] = 2;
    std::memcpy(packet.data() + offset, &source, sizeof(source));
    CHECK(!ReadControllerAimExtension(packet.data(), sizeof(TrackingPayload), decoded));
    CHECK(decoded.validMask == 0);
    CHECK(!ReadControllerAimExtension(packet.data(), offset, decoded));
    CHECK(!ReadControllerAimExtension(packet.data(), offset + sizeof(source) - 1, decoded));
    CHECK(ReadControllerAimExtension(packet.data(), packet.size(), decoded));
    CHECK(decoded.validMask == 3);
    CHECK(decoded.poses[1].orientation[3] == 1);
    source.poses[0].orientation[0] = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(packet.data() + offset, &source, sizeof(source));
    CHECK(ReadControllerAimExtension(packet.data(), packet.size(), decoded));
    CHECK(decoded.validMask == 2);
    source.poses[1].position[0] = std::numeric_limits<float>::infinity();
    std::memcpy(packet.data() + offset, &source, sizeof(source));
    CHECK(ReadControllerAimExtension(packet.data(), packet.size(), decoded));
    CHECK(decoded.validMask == 0);
    CHECK(!ReadControllerAimExtension(nullptr, packet.size(), decoded));
    CHECK(IsControllerProfilePath("/interaction_profiles/bytedance/pico4_controller"));
    CHECK(IsControllerProfilePath(""));
    CHECK(!IsControllerProfilePath("pico4"));
    std::string embeddedNull = "/interaction_profiles/x";
    embeddedNull.push_back('\0');
    embeddedNull += "junk";
    CHECK(!IsControllerProfilePath(embeddedNull));
    CHECK(!IsControllerProfilePath("/interaction_profiles/" + std::string(256, 'x')));
}

int main() {
    TestControllerTrackingExtensions();
    TestLinkLocalDiscovery();
    TestMusic();
    TestMotionRuns();
    TestHandSkeleton();
    NetInit();
    TestAddresses();
    TestDiscovery();
    TestSubnetHosts();
    TestAudio();
    TestIdentity();
    TestVideoReassemblyOrder();
    TestPacing();
    TestHandshakeAndTransport();
    std::printf(g_failures ? "%d FAILURES\n" : "ALL TESTS PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
