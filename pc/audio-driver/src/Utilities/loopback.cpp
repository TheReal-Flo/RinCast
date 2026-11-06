/*++

RinCast virtual audio: loopback ring buffer (see loopback.h).

Called from the WaveRT position-update paths, which run at DISPATCH_LEVEL
under the stream's own spin lock, so this uses a spin lock and non-paged
storage only.

--*/

#include "loopback.h"

namespace {

KSPIN_LOCK  g_Lock;
UCHAR        g_Buffer[RINCAST_LOOPBACK_BYTES];   // lives in the driver's non-paged data section
ULONG       g_Read = 0;
ULONG       g_Fill = 0;

} // namespace

#pragma code_seg()

VOID RinCastLoopbackInit()
{
    KeInitializeSpinLock(&g_Lock);
    g_Read = 0;
    g_Fill = 0;
}

VOID RinCastLoopbackReset()
{
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock, &irql);
    g_Read = 0;
    g_Fill = 0;
    KeReleaseSpinLock(&g_Lock, irql);
}

VOID RinCastLoopbackWrite(_In_reads_bytes_(Size) const UCHAR* Data, _In_ ULONG Size)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock, &irql);

    // Only the most recent audio matters for a live microphone.
    if (Size > RINCAST_LOOPBACK_BYTES)
    {
        Data += Size - RINCAST_LOOPBACK_BYTES;
        Size = RINCAST_LOOPBACK_BYTES;
    }

    ULONG write = (g_Read + g_Fill) % RINCAST_LOOPBACK_BYTES;
    ULONG remaining = Size;
    while (remaining > 0)
    {
        const ULONG run = min(remaining, RINCAST_LOOPBACK_BYTES - write);
        RtlCopyMemory(g_Buffer + write, Data, run);
        Data += run;
        remaining -= run;
        write = (write + run) % RINCAST_LOOPBACK_BYTES;
    }
    g_Fill += Size;
    if (g_Fill > RINCAST_LOOPBACK_BYTES)
    {
        g_Read = (g_Read + (g_Fill - RINCAST_LOOPBACK_BYTES)) % RINCAST_LOOPBACK_BYTES;
        g_Fill = RINCAST_LOOPBACK_BYTES;
    }

    // Bound latency (keep frame alignment: 4 bytes per stereo 16-bit frame).
    if (g_Fill > RINCAST_LOOPBACK_MAX_LAG)
    {
        ULONG drop = g_Fill - RINCAST_LOOPBACK_MAX_LAG / 2;
        drop -= drop % 4;
        g_Read = (g_Read + drop) % RINCAST_LOOPBACK_BYTES;
        g_Fill -= drop;
    }

    KeReleaseSpinLock(&g_Lock, irql);
}

VOID RinCastLoopbackRead(_Out_writes_bytes_(Size) UCHAR* Data, _In_ ULONG Size)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock, &irql);

    ULONG available = min(Size, g_Fill);
    available -= available % 4;
    ULONG remaining = available;
    UCHAR* out = Data;
    while (remaining > 0)
    {
        const ULONG run = min(remaining, RINCAST_LOOPBACK_BYTES - g_Read);
        RtlCopyMemory(out, g_Buffer + g_Read, run);
        out += run;
        remaining -= run;
        g_Read = (g_Read + run) % RINCAST_LOOPBACK_BYTES;
    }
    g_Fill -= available;

    KeReleaseSpinLock(&g_Lock, irql);

    // Underrun: silence.
    if (available < Size)
    {
        RtlZeroMemory(Data + available, Size - available);
    }
}
