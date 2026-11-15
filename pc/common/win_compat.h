// What the PC side's shared headers use from <windows.h>, for Linux and macOS: the plain value
// types and the atomics (no BOOL: Objective-C has its own). On Windows this is <windows.h> itself,
// so Windows builds see no difference.
#pragma once

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>

#include <cstdint>

using UINT = unsigned int;
using DWORD = uint32_t;
using LONG = int32_t;  // 32 bits, as on Windows (where long is 32 bits)
using LONG64 = int64_t;

struct RECT {
    LONG left, top, right, bottom;
};
struct POINT {
    LONG x, y;
};
struct LUID {
    DWORD LowPart;
    LONG HighPart;
};

inline LONG InterlockedIncrement(volatile LONG* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedIncrement64(volatile LONG64* p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
inline LONG InterlockedExchange(volatile LONG* p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedExchange64(volatile LONG64* p, LONG64 v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
// Windows' argument order: the new value, then the one expected. Returns what was there.
inline LONG InterlockedCompareExchange(volatile LONG* p, LONG exchange, LONG comparand) {
    __atomic_compare_exchange_n(p, &comparand, exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return comparand;
}
inline void MemoryBarrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
inline void YieldProcessor() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    sched_yield();
#endif
}
#endif
