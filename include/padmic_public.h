// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// What the PadDisplay Microphone driver and PadDisplay.exe agree on: the control device the host writes the tablet's
// microphone audio to, and its status. Included by both the kernel driver and the user-mode host.
#pragma once

// The control device. One writer at a time (the driver opens it exclusive); WriteFile() takes 48 kHz, mono, 16-bit
// little-endian PCM, any number of samples per call (up to kPadMicMaxWriteBytes).
#define PADMIC_USER_PATH L"\\\\.\\PadDisplayMic"

#define PADMIC_SAMPLE_RATE 48000u
#define PADMIC_MAX_WRITE_BYTES 16384u

// Version of this interface; PADMIC_STATUS::Version reports the driver's.
#define PADMIC_INTERFACE_VERSION 2u

#define PADMIC_CTL(code, access) (((0x22u) << 16) | ((access) << 14) | ((code) << 2)) // FILE_DEVICE_UNKNOWN, METHOD_BUFFERED
#define IOCTL_PADMIC_GET_STATUS PADMIC_CTL(0x800u, 1u)  // FILE_READ_ACCESS: out = PADMIC_STATUS
#define IOCTL_PADMIC_RESET      PADMIC_CTL(0x801u, 2u)  // FILE_WRITE_ACCESS: empties the ring

struct PADMIC_STATUS {
    unsigned int Version;
    unsigned int RingFillSamples;   // audio written and not yet read by an application
    unsigned int StreamsRunning;    // applications recording from the microphone right now
    unsigned int Primed;            // 1 = the driver is handing out real audio, 0 = silence until enough has arrived
    unsigned int Underruns;         // times an application read more than was there (the tablet's audio came late)
    unsigned int Drops;             // times audio was discarded because the writer ran ahead (kept latency bounded)
    unsigned long long SamplesWritten;
    unsigned long long SamplesRead;
    // Bookkeeping of the recording streams, for finding bugs: opened and destroyed streams, Run and Pause calls, timer ticks,
    // and streams that were destroyed while still running (the ring is told they are gone).
    unsigned int StreamsCreated;
    unsigned int StreamsDestroyed;
    unsigned int Runs;
    unsigned int Pauses;
    unsigned int DestroyedWhileRunning;
    unsigned long long Ticks;
    // Version 2: what the driver costs, for finding bottlenecks. Times are QueryPerformanceCounter counts (QpcFrequency per second).
    // A tick is one pass of the stream timer (one packet, normally 10 ms): "busy" is the time the pass itself takes at
    // DISPATCH_LEVEL, "late" is how long after its due time the timer actually ran it. LateTicks counts ticks later than 2 ms.
    unsigned long long QpcFrequency;
    unsigned long long TickBusyTotal;
    unsigned long long TickBusyMax;
    unsigned long long TickLateTotal;
    unsigned long long TickLateMax;
    unsigned long long LateTicks;
    unsigned long long Writes;          // WriteFile calls to the control device
    unsigned long long WriteBusyTotal;  // time inside the write handler
    unsigned long long WriteBusyMax;
};
// Size of the version 1 structure: a program built against version 1 passes this much, and still gets a valid answer.
#define PADMIC_STATUS_V1_SIZE 72u
