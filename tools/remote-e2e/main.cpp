// remote-e2e: end-to-end test of a remote (account) connection on one machine.
//
// Needs a RinCast server running locally (see BUILDING.md) and RinCast Streamer
// NOT running. The tool
//   1. registers a test account on the server (as the PC),
//   2. writes the streamer settings so it signs in to that account, starts it,
//   3. signs in as a headset and asks the server to connect to the PC,
//   4. connects with the headset app's real StreamClient (rendezvous, candidate
//      exchange, hole punching, handshake) and checks that desktop video flows,
//   5. stops the streamer and restores the previous settings file.
//
//   remote-e2e <server url> <path to RinCast Streamer.exe> [--only-ipv6 | --rendezvous-only | --unknown-headset]
//
// --unknown-headset keeps "Ask before a new headset connects" on: the right account alone must
// not get in, so the test passes only if the PC holds the headset back for approval.
//
// --only-ipv6 keeps just the PC's global IPv6 addresses and skips the rendezvous, so the
// session can only come up over IPv6.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "net/http.h"
#include "net/stream_client.h"
#include "rc/platform.h"
#include "util.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

static int Fail(const char* what) {
    std::printf("FAIL: %s\n", what);
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: remote-e2e <server url> <RinCast Streamer.exe>\n");
        return 2;
    }
    const std::string server = argv[1];
    const std::string exe = argv[2];
    const bool onlyIpv6 = argc > 3 && std::string(argv[3]) == "--only-ipv6";
    const bool unknownHeadset = argc > 3 && std::string(argv[3]) == "--unknown-headset";
    const std::string api = server + "/api/v1";
    const std::string email = "e2e-" + std::to_string(rc::NowNs() % 1000000000) + "@rincast.test";
    const std::string password = "e2e-password-123";
    const std::string pcId = "e2e-pc-" + std::to_string(rc::NowNs() % 100000);

    // 1. Account + PC tokens.
    auto reg = rcs::HttpRequest("POST", api + "/auth/register",
                                json{{"email", email}, {"password", password}, {"name", "E2E"}, {"deviceKind", "pc"},
                                     {"deviceId", pcId}, {"deviceName", "E2E PC"}}
                                    .dump(),
                                "", 10000);
    if (reg.status != 201) return Fail(("register: HTTP " + std::to_string(reg.status) + " " + reg.body).c_str());
    const std::string pcRefresh = reg.Json().value("refreshToken", "");

    // 2. Streamer settings (previous file restored at the end).
    wchar_t local[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const fs::path settingsPath = fs::path(local) / L"RinCast" / L"settings.json";
    std::string previous;
    const bool hadSettings = fs::exists(settingsPath);
    if (hadSettings) {
        std::ifstream f(settingsPath, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        previous = ss.str();
    }
    {
        std::ofstream f(settingsPath, std::ios::binary | std::ios::trunc);
        f << json{{"serverId", pcId},
                  {"pcName", "E2E PC"},
                  {"requireApproval", unknownHeadset},  // otherwise nobody is there to allow the test headset
                  {"cloudUrl", server},
                  {"cloudRefreshTokenProtected", rcs::ProtectString(pcRefresh)},
                  {"cloudUserName", "E2E"},
                  {"cloudEmail", email},
                  {"allowRemote", true},
                  {"startWithWindows", false},
                  {"startMinimized", true},
                  {"switchDefaultAudio", false},
                  {"streamAudio", false},
                  {"autoRegisterSteamVrDriver", false}}
                 .dump(2);
    }
    auto restore = [&] {
        if (hadSettings) {
            std::ofstream f(settingsPath, std::ios::binary | std::ios::trunc);
            f << previous;
        } else {
            fs::remove(settingsPath);
        }
    };
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + rcs::Widen(exe) + L"\" --minimized";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        restore();
        return Fail("could not start the streamer");
    }
    int result = 1;
    auto finish = [&](int code) {
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        restore();
        return code;
    };

    // 3. Headset sign-in, wait for the PC to come online, connect request.
    auto login = rcs::HttpRequest("POST", api + "/auth/login",
                                  json{{"email", email}, {"password", password}, {"deviceKind", "headset"},
                                       {"deviceId", "e2e-headset"}, {"deviceName", "E2E Headset"}}
                                      .dump(),
                                  "", 10000);
    if (login.status != 200) return finish(Fail("headset login"));
    const std::string hsToken = login.Json().value("accessToken", "");
    bool online = false;
    for (int i = 0; i < 60 && !online; i++) {
        auto pcs = rcs::HttpRequest("GET", api + "/pcs", "", hsToken, 5000);
        for (const auto& p : pcs.Json().value("pcs", json::array()))
            if (p.value("id", "") == pcId && p.value("online", false)) {
                online = true;
                std::printf("PC online, advertised addresses: %s\n", p.value("localAddrs", json::array()).dump().c_str());
            }
        if (!online) rc::SleepMs(500);
    }
    if (!online) return finish(Fail("the streamer never came online"));
    auto conn = rcs::HttpRequest("POST", api + "/connect", json{{"pcId", pcId}}.dump(), hsToken, 10000);
    if (conn.status != 200) return finish(Fail(("connect: " + conn.body).c_str()));
    const json r = conn.Json();

    // Same as the headset app's Cloud::Connect.
    rv::ConnectTarget t;
    t.name = r.value("pcName", "PC");
    t.cloudSession = r.value("sessionId", "");
    t.connectSecret = r.value("connectSecret", "");
    t.sessionToken = r.value("relayToken", "");
    const json rv = r.value("rendezvous", json::object());
    t.rendezvous = rc::NetAddress::ResolveAll(rv.value("host", ""), uint16_t(rv.value("port", 0)));
    for (const auto& a : r.value("pcLocalAddrs", json::array())) {
        rc::NetAddress addr;
        if (a.is_string() && rc::NetAddress::ParseEndpoint(a.get<std::string>(), addr)) t.pcLocalAddrs.push_back(addr);
    }
    std::printf("PC addresses from the server:");
    for (const auto& a : t.pcLocalAddrs) std::printf(" %s", a.ToString().c_str());
    std::printf("\n");
    if (onlyIpv6) {
        std::vector<rc::NetAddress> v6;
        for (const auto& a : t.pcLocalAddrs)
            if (a.IsGlobalV6()) v6.push_back(a);
        if (v6.empty()) return finish(Fail("the PC advertised no global IPv6 address"));
        t.pcLocalAddrs = v6;
        t.rendezvous.clear();
        std::printf("IPv6-only test: %zu address(es), rendezvous skipped\n", v6.size());
    }
    if (argc > 3 && std::string(argv[3]) == "--rendezvous-only") {
        // Forget what /connect said: everything must come from the rendezvous exchange.
        t.pcLocalAddrs.clear();
        std::printf("rendezvous-only test: PC addresses must come from the candidate exchange\n");
    }
    std::printf("rendezvous:");
    for (const auto& a : t.rendezvous) std::printf(" %s", a.ToString().c_str());
    std::printf("\n");

    // 4. The headset app's real connection code.
    std::atomic<int> frames{0};
    rv::StreamClient client;
    rv::StreamClient::Callbacks cb;
    cb.onControl = [](const json&) {};
    cb.onVideoFrame = [&](rc::ReceivedFrame&&) { frames++; };
    client.SetCallbacks(cb);
    const json hello{{"deviceId", "e2e-headset"},
                     {"deviceName", "E2E Headset"},
                     {"model", "Simulator"},
                     {"appVersion", "1.0.0"},
                     {"display",
                      {{"eyeWidth", 1680},
                       {"eyeHeight", 1760},
                       {"refreshRates", {72, 90}},
                       {"ipd", 0.064},
                       {"fov", {{1.376, 0.839, 0.966, 1.428}, {0.839, 1.376, 0.966, 1.428}}}}},
                     {"decoders", {{"h264", true}, {"hevc", true}, {"av1", true}, {"maxWidth", 8192}, {"maxHeight", 4320}}}};
    const int64_t start = rc::NowNs();
    client.Connect(t, hello);
    while (client.GetState() != rv::StreamClient::State::Connected && rc::NowNs() - start < 35'000'000'000LL) {
        if (unknownHeadset && client.GetState() == rv::StreamClient::State::WaitingApproval) break;
        if (client.GetState() == rv::StreamClient::State::Failed) break;
        rc::SleepMs(50);
    }
    if (unknownHeadset) {
        const auto state = client.GetState();
        std::printf("unknown headset: %s\n", client.StatusText().c_str());
        client.Disconnect();
        const int outcome = state == rv::StreamClient::State::WaitingApproval ? 0
                            : Fail(state == rv::StreamClient::State::Connected ? "an unknown headset got in with only the account"
                                                                               : "expected the PC to ask for approval");
        if (outcome == 0) std::printf("PASS: held back for approval\n");
        return finish(outcome);
    }
    if (client.GetState() != rv::StreamClient::State::Connected)
        return finish(Fail(("no connection: " + client.StatusText()).c_str()));
    std::printf("connected in %.0f ms (%s)\n", double(rc::NowNs() - start) / 1e6, client.StatusText().c_str());
    client.SendControl({{"type", "client.info"}, {"display", hello["display"]}, {"decoders", hello["decoders"]},
                        {"settings", {{"codec", "hevc"}, {"refreshRate", 72}, {"streamAudio", false}}}});
    client.SendControl({{"type", "desktop.start"}});
    for (int i = 0; i < 60 && frames < 5; i++) rc::SleepMs(100);
    std::printf("desktop frames received: %d\n", frames.load());
    client.Disconnect();
    result = frames >= 1 ? 0 : Fail("no video over the remote session");
    if (result == 0) std::printf("PASS\n");
    return finish(result);
}
