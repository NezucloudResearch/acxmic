// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Parts derived from Microsoft's ACX audio samples (Windows-driver-samples, audio/Acx), Microsoft Public License;
// see THIRD_PARTY_NOTICES.md.
//
// PadDisplay Microphone: a virtual capture endpoint. The host writes the tablet's microphone audio to the control
// device (driver.cpp), into a ring (ring.h); when an application records, the stream (stream.cpp) hands it out.
#pragma once

// The order matters: ks.h and mmsystem.h are user-mode headers that need windef.h's types after wdm.h (as in the sample).
#include <wdm.h>
#include <windef.h>
#include <mmsystem.h>
#include <ks.h>
#include <ksmedia.h>

extern "C" {
#include <initguid.h>
#include <ntddk.h>
#include <ntstrsafe.h>
#include <ntintsafe.h>
#include <wdf.h>
#include <acx.h>
}

#include "padmic_public.h"
#include "ring.h"

#define PADMIC_TAG 'cMdP'

#ifndef SIZEOF_ARRAY
#define SIZEOF_ARRAY(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define RETURN_NTSTATUS_IF_FAILED(expr) \
    do { NTSTATUS _st = (expr); if (!NT_SUCCESS(_st)) { PADMIC_LOG("%s failed: 0x%08X", #expr, _st); return _st; } } while (0)

#define RETURN_NTSTATUS_IF_TRUE(cond, st) \
    do { if (cond) { PADMIC_LOG("%s", #cond); return (st); } } while (0)

#if DBG
#define PADMIC_LOG(fmt, ...) DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "PadMic: " fmt "\n", ##__VA_ARGS__)
#else
#define PADMIC_LOG(fmt, ...) ((void)0)
#endif

PVOID operator new(size_t size, POOL_FLAGS flags, ULONG tag);

// Stream bookkeeping, reported through IOCTL_PADMIC_GET_STATUS.
struct PadMicStats {
    volatile LONG Created, Destroyed, Runs, Pauses, DestroyedWhileRunning;
    volatile LONG64 Ticks;
};
extern PadMicStats g_Stats;

// The one ring every stream reads and the control device writes (there is one microphone).
extern PadMicRing g_Ring;

// The circuit's component id (identifies this circuit instance) and its name, which must match the INF's interface names.
// {df09ba0c-8d11-42e9-a7ae-d305032dad68}
DEFINE_GUID(PADMIC_COMPONENT_GUID, 0xdf09ba0c, 0x8d11, 0x42e9, 0xa7, 0xae, 0xd3, 0x05, 0x03, 0x2d, 0xad, 0x68);
DECLARE_CONST_UNICODE_STRING(PadMicCircuitName, L"Microphone0");

// driver.cpp
EVT_WDF_DRIVER_DEVICE_ADD PadMic_EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE PadMic_EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE PadMic_EvtDeviceReleaseHardware;
NTSTATUS PadMic_CreateControlDevice(_In_ WDFDRIVER Driver);

// circuit.cpp
typedef struct _PADMIC_DEVICE_CONTEXT {
    ACXCIRCUIT Capture;
} PADMIC_DEVICE_CONTEXT, *PPADMIC_DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PADMIC_DEVICE_CONTEXT, GetPadMicDeviceContext)

NTSTATUS PadMic_CreateCaptureCircuit(_In_ WDFDEVICE Device, _Out_ ACXCIRCUIT* Circuit);

// stream.cpp: the capture stream engine, one per recording application.
class CPadMicStream {
public:
    CPadMicStream(_In_ ACXSTREAM Stream, _In_ ACXDATAFORMAT Format);
    ~CPadMicStream();

    NTSTATUS AllocateRtPackets(_In_ ULONG PacketCount, _In_ ULONG PacketSize, _Out_ PACX_RTPACKET* Packets);
    VOID FreeRtPackets(_Frees_ptr_ PACX_RTPACKET Packets, _In_ ULONG PacketCount);
    NTSTATUS PrepareHardware();
    NTSTATUS ReleaseHardware();
    NTSTATUS Run();
    NTSTATUS Pause();
    NTSTATUS GetPresentationPosition(_Out_ PULONGLONG PositionInBlocks, _Out_ PULONGLONG QPCPosition);
    NTSTATUS GetCurrentPacket(_Out_ PULONG CurrentPacket);
    NTSTATUS GetCapturePacket(_Out_ ULONG* LastCapturePacket, _Out_ ULONGLONG* QPCPacketStart, _Out_ BOOLEAN* MoreData);

private:
    static constexpr ULONG kMaxPackets = 2;
    static constexpr LONGLONG kHnsPerSec = 10000000;

    static EVT_WDF_TIMER s_EvtTimer;
    VOID PassCallback();
    VOID ScheduleNextPass();
    VOID UpdatePosition();
    VOID ProcessPacket();

    PVOID m_Packets[kMaxPackets];
    ULONG m_PacketsCount;
    ULONG m_PacketSize;
    ULONG m_FirstPacketOffset;
    WDFTIMER m_Timer;
    ACX_STREAM_STATE m_State;
    ULONG m_CurrentPacket;
    ULONGLONG m_Position;
    ACXSTREAM m_Stream;
    ACXDATAFORMAT m_Format;
    ULONGLONG m_StartTime;
    ULONGLONG m_StartPosition;
    ULONGLONG m_GlitchAdjust;
    LARGE_INTEGER m_QpcFrequency;
    LARGE_INTEGER m_CurrentPacketStart;
    LARGE_INTEGER m_LastPacketStart;
};

typedef struct _PADMIC_STREAM_CONTEXT {
    CPadMicStream* Engine;
} PADMIC_STREAM_CONTEXT, *PPADMIC_STREAM_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PADMIC_STREAM_CONTEXT, GetPadMicStreamContext)

typedef struct _PADMIC_TIMER_CONTEXT {
    CPadMicStream* Engine;
} PADMIC_TIMER_CONTEXT, *PPADMIC_TIMER_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(PADMIC_TIMER_CONTEXT, GetPadMicTimerContext)

EVT_ACX_CIRCUIT_CREATE_STREAM PadMic_EvtCircuitCreateStream;
EVT_ACX_STREAM_GET_CAPTURE_PACKET PadMic_EvtStreamGetCapturePacket;
EVT_ACX_STREAM_GET_HW_LATENCY PadMic_EvtStreamGetHwLatency;
EVT_ACX_STREAM_ALLOCATE_RTPACKETS PadMic_EvtStreamAllocateRtPackets;
EVT_ACX_STREAM_FREE_RTPACKETS PadMic_EvtStreamFreeRtPackets;
EVT_ACX_STREAM_PREPARE_HARDWARE PadMic_EvtStreamPrepareHardware;
EVT_ACX_STREAM_RELEASE_HARDWARE PadMic_EvtStreamReleaseHardware;
EVT_ACX_STREAM_RUN PadMic_EvtStreamRun;
EVT_ACX_STREAM_PAUSE PadMic_EvtStreamPause;
EVT_ACX_STREAM_GET_CURRENT_PACKET PadMic_EvtStreamGetCurrentPacket;
EVT_ACX_STREAM_GET_PRESENTATION_POSITION PadMic_EvtStreamGetPresentationPosition;
EVT_WDF_OBJECT_CONTEXT_DESTROY PadMic_EvtStreamDestroy;
