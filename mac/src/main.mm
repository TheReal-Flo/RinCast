#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include <csignal>
#include <iostream>

#include "capture/mac_monitors.h"
#include "input/mac_input.h"
#include "rc/platform.h"
#include "server/mac_stream_server.h"

static rcm::MacStreamServer* g_server = nullptr;

static void SignalHandler(int signum) {
    RC_LOGI("Received signal %d, stopping server...", signum);
    if (g_server) {
        g_server->Stop();
    }
    CFRunLoopStop(CFRunLoopGetMain());
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);

        std::cout << "========================================" << std::endl;
        std::cout << "       RinCast Streamer for macOS       " << std::endl;
        std::cout << "========================================" << std::endl;

        // Check Screen Recording permission (macOS 10.15+)
        if (@available(macOS 10.15, *)) {
            if (!CGPreflightScreenCaptureAccess()) {
                std::cout << "\n[!] Screen Recording permission is NOT granted." << std::endl;
                std::cout << "    Requesting access... Please allow RinCast in System Settings > Privacy & Security > Screen Recording." << std::endl;
                CGRequestScreenCaptureAccess();
            } else {
                std::cout << "[✓] Screen Recording permission granted." << std::endl;
            }
        }

        // Check Accessibility permission (for mouse/keyboard injection)
        NSDictionary* options = @{(__bridge id)kAXTrustedCheckOptionPrompt: @YES};
        if (!AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options)) {
            std::cout << "[!] Accessibility permission is NOT granted." << std::endl;
            std::cout << "    Please allow RinCast in System Settings > Privacy & Security > Accessibility to enable mouse and keyboard control." << std::endl;
        } else {
            std::cout << "[✓] Accessibility permission granted." << std::endl;
        }

        // Enumerate displays
        auto monitors = rcm::EnumerateMacMonitors();
        std::cout << "\nDetected " << monitors.size() << " display(s):" << std::endl;
        for (const auto& mon : monitors) {
            std::cout << "  - [" << mon.index << "] " << mon.name
                      << " (" << mon.width << "x" << mon.height << " @ " << mon.refreshHz << "Hz)"
                      << (mon.primary ? " [Primary]" : "") << std::endl;
        }

        // A previous run may have been killed while a modifier was still down. CGEvent
        // key-downs stay latched in macOS until a key-up is posted, so clear them before
        // the first headset connects.
        rcm::input::ReleaseAll();

        // Host name
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) != 0) {
            std::strncpy(hostname, "Mac", sizeof(hostname));
        }
        std::string serverName = std::string(hostname) + " (RinCast)";

        rcm::MacStreamServer server;
        g_server = &server;

        if (!server.Start(serverName)) {
            std::cerr << "Failed to start stream server!" << std::endl;
            return 1;
        }

        std::cout << "\nStreamer is broadcasting discovery packets." << std::endl;
        std::cout << "Put on your Meta Quest / PICO and select '" << serverName << "' to connect!" << std::endl;
        std::cout << "Press Ctrl+C to stop.\n" << std::endl;

        CFRunLoopRun();

        server.Stop();
        g_server = nullptr;
        std::cout << "RinCast Streamer exited cleanly." << std::endl;
    }
    return 0;
}
