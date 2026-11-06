// Small cross-platform helpers: monotonic clock, logging, randomness.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rc {

// Monotonic nanoseconds. On Android this is CLOCK_MONOTONIC, which is also
// the time base OpenXR uses via XR_KHR_convert_timespec_time.
int64_t NowNs();

inline double NsToMs(int64_t ns) { return double(ns) / 1e6; }
inline int64_t MsToNs(double ms) { return int64_t(ms * 1e6); }

void SleepMs(int ms);

// Fills `out` with cryptographically secure random bytes.
void RandomBytes(void* out, size_t size);

enum class LogLevel { Debug, Info, Warn, Error };

using LogSink = void (*)(LogLevel level, const char* message);
void SetLogSink(LogSink sink);
void Log(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define RC_LOGD(...) ::rc::Log(::rc::LogLevel::Debug, __VA_ARGS__)
#define RC_LOGI(...) ::rc::Log(::rc::LogLevel::Info, __VA_ARGS__)
#define RC_LOGW(...) ::rc::Log(::rc::LogLevel::Warn, __VA_ARGS__)
#define RC_LOGE(...) ::rc::Log(::rc::LogLevel::Error, __VA_ARGS__)

std::string StrFormat(const char* fmt, ...);

std::string HexEncode(const void* data, size_t size);
bool HexDecode(const std::string& hex, void* out, size_t size);

}  // namespace rc
