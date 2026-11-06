/*++

RinCast virtual audio: loopback ring buffer.

Audio rendered to the "RinCast Microphone Feed" endpoint (by the RinCast
Streamer, carrying the headset microphone) is stored here and read back by
the "RinCast Microphone" capture endpoint, which applications record from.

Both endpoints use the same format (48 kHz, 16-bit, stereo), so the data is
copied byte for byte. The buffer keeps latency bounded: when it holds more
than kMaxLatencyBytes the oldest audio is dropped; when it runs dry the
capture side receives silence.

--*/

#pragma once

#include <wdm.h>

#define RINCAST_LOOPBACK_BYTES      (192000)        // 1 s at 48 kHz / 16-bit / stereo
#define RINCAST_LOOPBACK_MAX_LAG    (192000 / 10)   // keep at most 100 ms queued

VOID RinCastLoopbackInit();
VOID RinCastLoopbackReset();
VOID RinCastLoopbackWrite(_In_reads_bytes_(Size) const UCHAR* Data, _In_ ULONG Size);
VOID RinCastLoopbackRead(_Out_writes_bytes_(Size) UCHAR* Data, _In_ ULONG Size);
