// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// The buffer between the host (writes the tablet's audio as it arrives over Wi-Fi) and an application that records
// (reads on the audio engine's clock). Latency is the product, so it stays short on purpose:
//   - an application starts reading real audio only once PRIME samples are waiting (a little cushion against Wi-Fi
//     jitter); until then it gets silence. After running dry it primes again.
//   - the writer can run ahead of the reader (the two clocks differ by a few parts per million); beyond MAX_FILL the
//     oldest audio is discarded, so the delay can never grow past MAX_FILL.
//   - while nobody records, only PRIME samples are kept, so a recording starts with fresh audio, not a stale backlog.
// The lock is only ever held for a copy of at most a few KB, at DISPATCH_LEVEL or below.
#pragma once

#include <wdm.h>

class PadMicRing {
public:
    static constexpr ULONG kCapacity = 8192; // samples (a power of two): about 170 ms
    static constexpr ULONG kPrime = 960;     // 20 ms
    static constexpr ULONG kMaxFill = 4800;  // 100 ms

    VOID Init() {
        KeInitializeSpinLock(&m_Lock);
        Reset();
    }

    VOID Reset() {
        KIRQL irql;
        KeAcquireSpinLock(&m_Lock, &irql);
        m_Head = m_Tail = 0;
        m_Primed = FALSE;
        KeReleaseSpinLock(&m_Lock, irql);
    }

    // The tablet's audio. Never blocks, never fails: too much is dropped from the old end.
    VOID Write(_In_reads_(count) const SHORT* samples, _In_ ULONG count) {
        KIRQL irql;
        KeAcquireSpinLock(&m_Lock, &irql);
        const ULONG limit = m_Running > 0 ? kMaxFill : kPrime;
        if (count > limit) { // more than we may keep: only its newest part counts
            samples += count - limit;
            count = limit;
        }
        const ULONG fill = Fill();
        if (fill + count > limit) {
            m_Tail += fill + count - limit;
            if (m_Running > 0) m_Drops++; // idle trimming (nobody records) is not a drop that matters
        }
        for (ULONG i = 0; i < count; ++i) m_Data[(m_Head + i) & (kCapacity - 1)] = samples[i];
        m_Head += count;
        m_Written += count;
        KeReleaseSpinLock(&m_Lock, irql);
    }

    // The application's side: always fills `count` samples (silence where nothing has arrived).
    VOID Read(_Out_writes_(count) SHORT* out, _In_ ULONG count) {
        KIRQL irql;
        KeAcquireSpinLock(&m_Lock, &irql);
        ULONG fill = Fill();
        if (!m_Primed && fill >= kPrime) m_Primed = TRUE;
        ULONG have = 0;
        if (m_Primed) {
            have = fill < count ? fill : count;
            for (ULONG i = 0; i < have; ++i) out[i] = m_Data[(m_Tail + i) & (kCapacity - 1)];
            m_Tail += have;
            m_Read += have;
            if (have < count) { // ran dry: wait for the cushion again rather than stutter
                m_Primed = FALSE;
                m_Underruns++;
            }
        }
        KeReleaseSpinLock(&m_Lock, irql);
        for (ULONG i = have; i < count; ++i) out[i] = 0;
    }

    // An application started or stopped recording.
    VOID StreamStarted() {
        KIRQL irql;
        KeAcquireSpinLock(&m_Lock, &irql);
        // Start from the newest audio: drop what piled up before anyone listened.
        const ULONG fill = Fill();
        if (fill > kPrime) m_Tail += fill - kPrime;
        m_Running++;
        KeReleaseSpinLock(&m_Lock, irql);
    }

    VOID StreamStopped() {
        KIRQL irql;
        KeAcquireSpinLock(&m_Lock, &irql);
        if (m_Running > 0) m_Running--;
        KeReleaseSpinLock(&m_Lock, irql);
    }

    VOID Snapshot(_Out_ ULONG* fill, _Out_ ULONG* running, _Out_ ULONG* primed, _Out_ ULONG* underruns, _Out_ ULONG* drops,
                  _Out_ ULONGLONG* written, _Out_ ULONGLONG* read) {
        KIRQL irql;
        KeAcquireSpinLock(&m_Lock, &irql);
        *fill = Fill();
        *running = m_Running;
        *primed = m_Primed ? 1 : 0;
        *underruns = m_Underruns;
        *drops = m_Drops;
        *written = m_Written;
        *read = m_Read;
        KeReleaseSpinLock(&m_Lock, irql);
    }

private:
    ULONG Fill() const { return m_Head - m_Tail; } // head and tail count up forever; the difference survives wrap-around

    KSPIN_LOCK m_Lock;
    SHORT m_Data[kCapacity];
    ULONG m_Head = 0, m_Tail = 0;
    ULONG m_Running = 0;
    BOOLEAN m_Primed = FALSE;
    ULONG m_Underruns = 0, m_Drops = 0;
    ULONGLONG m_Written = 0, m_Read = 0;
};
