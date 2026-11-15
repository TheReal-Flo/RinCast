// util.h on macOS: its folders, starting at login, keeping awake, and the permissions the
// streamer needs. What Linux and macOS share is in platform/posix/util_posix.cpp.
#include "util.h"

#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "input/input_injector.h"
#include "platform/posix/platform_main.h"

namespace rcs {

namespace {

std::filesystem::path Home() {
    const char* home = std::getenv("HOME");
    return home && *home ? std::filesystem::path(home) : std::filesystem::temp_directory_path();
}

}  // namespace

// ~/Library/Application Support/RinCast, the user's alone: keys and tokens live here.
std::filesystem::path DataDir() {
    static const std::filesystem::path dir = [] {
        const auto p = Home() / "Library/Application Support/RinCast";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        chmod(p.c_str(), 0700);
        return p;
    }();
    return dir;
}

std::filesystem::path ExePath() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::filesystem::current_path() / "rincast-streamer";
    std::error_code ec;
    const auto real = std::filesystem::canonical(buf.c_str(), ec);
    return ec ? std::filesystem::path(buf.c_str()) : real;
}

std::filesystem::path ExeDir() { return ExePath().parent_path(); }

std::filesystem::path PicturesDir() {
    NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(NSPicturesDirectory, NSUserDomainMask, YES);
    if (dirs.firstObject) return std::filesystem::path(dirs.firstObject.UTF8String);
    return Home() / "Pictures";
}

// A launch agent, which launchd starts when the user logs in.
bool SetStartAtLogin(bool enable) {
    const auto dir = Home() / "Library/LaunchAgents";
    const auto file = dir / "app.rincast.streamer.plist";
    std::error_code ec;
    if (!enable) {
        std::filesystem::remove(file, ec);  // already gone is fine
        return true;
    }
    std::filesystem::create_directories(dir, ec);
    NSDictionary* agent = @{
        @"Label": @"app.rincast.streamer",
        @"ProgramArguments": @[@(ExePath().c_str()), @"--minimized"],
        @"RunAtLoad": @YES,
        @"ProcessType": @"Interactive",
    };
    NSError* error = nil;
    NSData* data = [NSPropertyListSerialization dataWithPropertyList:agent format:NSPropertyListXMLFormat_v1_0 options:0 error:&error];
    if (!data) return false;
    return WriteFileAtomic(file, std::string(static_cast<const char*>(data.bytes), data.length));
}

// "macOS 15.3.1".
std::string WindowsVersion() {
    const NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
    char buf[64];
    if (v.patchVersion) std::snprintf(buf, sizeof(buf), "macOS %ld.%ld.%ld", long(v.majorVersion), long(v.minorVersion), long(v.patchVersion));
    else std::snprintf(buf, sizeof(buf), "macOS %ld.%ld", long(v.majorVersion), long(v.minorVersion));
    return buf;
}

// While a headset streams: the displays stay on (an asleep display delivers no pictures) and
// the Mac doesn't idle-sleep.
void KeepAwake(bool on) {
    static std::mutex mutex;
    static IOPMAssertionID assertion = kIOPMNullAssertionID;
    std::lock_guard lock(mutex);
    if (on && assertion == kIOPMNullAssertionID) {
        IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
                                    CFSTR("A RinCast headset is streaming"), &assertion);
    } else if (!on && assertion != kIOPMNullAssertionID) {
        IOPMAssertionRelease(assertion);
        assertion = kIOPMNullAssertionID;
    }
}

// Screen Recording (to capture) and Accessibility (to type and click for the headset) are the
// user's to grant in System Settings; macOS shows its prompts here. The streamer runs without
// them, and capture keeps retrying until Screen Recording is allowed.
bool PreparePlatform() {
    if (!CGPreflightScreenCaptureAccess()) {
        std::printf("Screen Recording isn't allowed yet: allow RinCast in System Settings > Privacy & Security >\n"
                    "Screen Recording, then start it again.\n");
        CGRequestScreenCaptureAccess();
    }
    NSDictionary* options = @{(__bridge id)kAXTrustedCheckOptionPrompt: @YES};
    if (!AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options))
        std::printf("Accessibility isn't allowed yet: allow RinCast in System Settings > Privacy & Security >\n"
                    "Accessibility for the headset's mouse and keyboard.\n");
    // A previous run may have been killed with a key still down: macOS keeps injected keys down
    // until their release comes.
    input::ReleaseAll();
    return true;
}

}  // namespace rcs
