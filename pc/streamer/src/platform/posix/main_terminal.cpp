// RinCast Streamer on Linux and macOS, for now without a window (Linux gets a GTK + WebKitGTK one
// with its desktop step, docs/LINUX.md). It runs the same service as on Windows and asks in the
// terminal when a headset wants to connect:
//
//   ./rincast-streamer            then answer "y" (allow, remember), "o" (allow once) or "n"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#include "app.h"
#include "platform_main.h"
#include "rc/platform.h"
#include "util.h"

using namespace rcs;

namespace {

std::atomic<bool> g_quit{false};
void OnSignal(int) { g_quit = true; }

std::mutex g_pendingMutex;
std::string g_pendingDevice;  // the headset waiting for an answer

void OnUi(const std::string& text) {
    json m;
    try {
        m = json::parse(text);
    } catch (...) {
        return;
    }
    const std::string type = m.value("type", "");
    if (type == "approval") {
        std::lock_guard lock(g_pendingMutex);
        g_pendingDevice = m.value("deviceId", "");
        std::printf("\n%s (%s) at %s wants to connect. Allow? [y = always, o = once, n = no] ",
                    m.value("deviceName", "A headset").c_str(), m.value("model", "").c_str(),
                    m.value("address", "").c_str());
        std::fflush(stdout);
    } else if (type == "approvalEnded") {
        std::lock_guard lock(g_pendingMutex);
        g_pendingDevice.clear();
    }
}

void Answer(char c) {
    std::string device;
    {
        std::lock_guard lock(g_pendingMutex);
        device = std::exchange(g_pendingDevice, {});
    }
    if (device.empty()) return;
    const bool allow = c == 'y' || c == 'o';
    App::Get().HandleUiMessage(json{{"type", allow ? "approve" : "deny"}, {"deviceId", device}, {"remember", c == 'y'}}.dump());
    std::printf(allow ? "Allowed.\n" : "Denied.\n");
}

}  // namespace

int main(int argc, char** argv) {
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
    signal(SIGPIPE, SIG_IGN);
    if (!PreparePlatform()) return 1;
    if (!App::Get().Init()) {
        std::fprintf(stderr, "RinCast Streamer could not start its network service. Is another copy running?\n");
        return 1;
    }
    App::Get().SetUiSink(OnUi);
    std::printf("RinCast Streamer %s is running as \"%s\". Ctrl+C to stop.\n", App::Get().Version().c_str(),
                ComputerName().c_str());
    int64_t lastTick = 0;
    bool haveStdin = true;
    while (!g_quit) {
        pollfd p{haveStdin ? STDIN_FILENO : -1, POLLIN, 0};  // fd -1: poll just waits
        if (poll(&p, 1, 200) > 0 && (p.revents & (POLLIN | POLLHUP))) {
            char line[64] = {};
            if (!fgets(line, sizeof(line), stdin)) {
                // No terminal (a service): nobody to ask, so nothing gets approved here.
                haveStdin = false;
            } else {
                Answer(char(line[0] | 0x20));
            }
        }
        if (rc::NowNs() - lastTick > 1'000'000'000LL) {
            lastTick = rc::NowNs();
            App::Get().Tick();
        }
    }
    App::Get().SetUiSink(nullptr);
    App::Get().Shutdown();
    return 0;
}
