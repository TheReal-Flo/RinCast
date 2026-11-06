#include "rc/platform.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <android/log.h>
#endif
#endif

namespace rc {

int64_t NowNs() {
#ifdef _WIN32
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return int64_t(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    // Split to avoid overflow for large counter values.
    const int64_t whole = c.QuadPart / freq;
    const int64_t rem = c.QuadPart % freq;
    return whole * 1000000000LL + rem * 1000000000LL / freq;
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
#endif
}

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void RandomBytes(void* out, size_t size) {
#ifdef _WIN32
    BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    static int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    auto* p = static_cast<uint8_t*>(out);
    while (size > 0) {
        const ssize_t n = read(fd, p, size);
        if (n <= 0) continue;
        p += n;
        size -= size_t(n);
    }
#endif
}

namespace {
std::atomic<LogSink> g_sink{nullptr};
std::mutex g_logMutex;

void DefaultSink(LogLevel level, const char* message) {
#if defined(__ANDROID__)
    int prio = ANDROID_LOG_INFO;
    switch (level) {
        case LogLevel::Debug: prio = ANDROID_LOG_DEBUG; break;
        case LogLevel::Info: prio = ANDROID_LOG_INFO; break;
        case LogLevel::Warn: prio = ANDROID_LOG_WARN; break;
        case LogLevel::Error: prio = ANDROID_LOG_ERROR; break;
    }
    __android_log_write(prio, "RinCast", message);
#else
    static const char* names[] = {"D", "I", "W", "E"};
    std::lock_guard lock(g_logMutex);
    std::fprintf(stderr, "[%s] %s\n", names[int(level)], message);
#ifdef _WIN32
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
#endif
#endif
}
}  // namespace

void SetLogSink(LogSink sink) { g_sink = sink; }

void Log(LogLevel level, const char* fmt, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (auto sink = g_sink.load()) {
        sink(level, buffer);
    } else {
        DefaultSink(level, buffer);
    }
}

std::string StrFormat(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    std::string out(n > 0 ? size_t(n) : 0, '\0');
    if (n > 0) std::vsnprintf(out.data(), out.size() + 1, fmt, args);
    va_end(args);
    return out;
}

std::string HexEncode(const void* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    const auto* p = static_cast<const uint8_t*>(data);
    std::string out(size * 2, '0');
    for (size_t i = 0; i < size; i++) {
        out[i * 2] = digits[p[i] >> 4];
        out[i * 2 + 1] = digits[p[i] & 15];
    }
    return out;
}

bool HexDecode(const std::string& hex, void* out, size_t size) {
    if (hex.size() != size * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    auto* p = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < size; i++) {
        const int hi = nibble(hex[i * 2]);
        const int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        p[i] = uint8_t(hi << 4 | lo);
    }
    return true;
}

}  // namespace rc
