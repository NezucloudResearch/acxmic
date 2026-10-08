// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Parts derived from Microsoft's ACX audio samples (Microsoft Public License); see THIRD_PARTY_NOTICES.md.
//
// The capture stream: there is no hardware, so a high-resolution timer plays the part of the DMA engine. Once per
// packet (the audio engine picks the size, normally 10 ms) it fills the packet from the ring and tells ACX the packet
// is complete. The timer only exists while an application records: an idle microphone costs nothing.
#include "padmic.h"

// ---- the ACX callbacks: look up the engine and forward ------------------------------------------------------------------

static CPadMicStream* EngineOf(ACXSTREAM stream) { return GetPadMicStreamContext(stream)->Engine; }

VOID PadMic_EvtStreamDestroy(_In_ WDFOBJECT Object) {
    PPADMIC_STREAM_CONTEXT ctx = GetPadMicStreamContext(static_cast<ACXSTREAM>(Object));
    CPadMicStream* engine = ctx->Engine;
    ctx->Engine = nullptr;
    delete engine;
}

NTSTATUS PadMic_EvtStreamGetHwLatency(_In_ ACXSTREAM Stream, _Out_ ULONG* FifoSize, _Out_ ULONG* Delay) {
    UNREFERENCED_PARAMETER(Stream);
    *FifoSize = 0; // no hardware FIFO: the ring is ours and bounded by kMaxFill, which the host side accounts for
    *Delay = 0;
    return STATUS_SUCCESS;
}

NTSTATUS PadMic_EvtStreamAllocateRtPackets(_In_ ACXSTREAM Stream, _In_ ULONG PacketCount, _In_ ULONG PacketSize,
                                           _Out_ PACX_RTPACKET* Packets) {
    return EngineOf(Stream)->AllocateRtPackets(PacketCount, PacketSize, Packets);
}

VOID PadMic_EvtStreamFreeRtPackets(_In_ ACXSTREAM Stream, _In_ PACX_RTPACKET Packets, _In_ ULONG PacketCount) {
    EngineOf(Stream)->FreeRtPackets(Packets, PacketCount);
}

NTSTATUS PadMic_EvtStreamPrepareHardware(_In_ ACXSTREAM Stream) { return EngineOf(Stream)->PrepareHardware(); }
NTSTATUS PadMic_EvtStreamReleaseHardware(_In_ ACXSTREAM Stream) { return EngineOf(Stream)->ReleaseHardware(); }
NTSTATUS PadMic_EvtStreamRun(_In_ ACXSTREAM Stream) { return EngineOf(Stream)->Run(); }
NTSTATUS PadMic_EvtStreamPause(_In_ ACXSTREAM Stream) { return EngineOf(Stream)->Pause(); }

NTSTATUS PadMic_EvtStreamGetCurrentPacket(_In_ ACXSTREAM Stream, _Out_ PULONG CurrentPacket) {
    return EngineOf(Stream)->GetCurrentPacket(CurrentPacket);
}

NTSTATUS PadMic_EvtStreamGetPresentationPosition(_In_ ACXSTREAM Stream, _Out_ PULONGLONG PositionInBlocks,
                                                 _Out_ PULONGLONG QPCPosition) {
    return EngineOf(Stream)->GetPresentationPosition(PositionInBlocks, QPCPosition);
}

NTSTATUS PadMic_EvtStreamGetCapturePacket(_In_ ACXSTREAM Stream, _Out_ ULONG* LastCapturePacket,
                                          _Out_ ULONGLONG* QPCPacketStart, _Out_ BOOLEAN* MoreData) {
    return EngineOf(Stream)->GetCapturePacket(LastCapturePacket, QPCPacketStart, MoreData);
}

// ---- the engine ---------------------------------------------------------------------------------------------------------

CPadMicStream::CPadMicStream(_In_ ACXSTREAM Stream, _In_ ACXDATAFORMAT Format)
    : m_PacketsCount(0),
      m_PacketSize(0),
      m_FirstPacketOffset(0),
      m_Timer(nullptr),
      m_State(AcxStreamStateStop),
      m_CurrentPacket(0),
      m_Position(0),
      m_Stream(Stream),
      m_Format(Format),
      m_StartTime(0),
      m_StartPosition(0),
      m_GlitchAdjust(0),
      m_DueQpc(0) {
    InterlockedIncrement(&g_Stats.Created);
    KeQueryPerformanceCounter(&m_QpcFrequency);
    m_CurrentPacketStart.QuadPart = 0;
    m_LastPacketStart.QuadPart = 0;
    RtlZeroMemory(m_Packets, sizeof(m_Packets));
}

CPadMicStream::~CPadMicStream() {
    InterlockedIncrement(&g_Stats.Destroyed);
    // A stream torn down while it was still running (no Pause came): the ring must not go on counting a reader that is gone,
    // or it would believe someone is recording and keep a long backlog instead of the short idle one.
    if (m_State == AcxStreamStateRun) {
        InterlockedIncrement(&g_Stats.DestroyedWhileRunning);
        g_Ring.StreamStopped();
    }
}

NTSTATUS CPadMicStream::AllocateRtPackets(_In_ ULONG PacketCount, _In_ ULONG PacketSize, _Out_ PACX_RTPACKET* Packets) {
    if (PacketCount == 0 || PacketCount > kMaxPackets) return STATUS_INVALID_PARAMETER;

    size_t packetsSize = 0;
    NTSTATUS status = RtlSizeTMult(PacketCount, sizeof(ACX_RTPACKET), &packetsSize);
    if (!NT_SUCCESS(status)) return status;
    PACX_RTPACKET packets = static_cast<PACX_RTPACKET>(ExAllocatePool2(POOL_FLAG_NON_PAGED, packetsSize, PADMIC_TAG));
    if (!packets) return STATUS_NO_MEMORY;

    // Page-aligned buffers, so no other kernel memory is mapped into the application with ours. Packet 0 is shifted so
    // that it ends on a page boundary and packet 1 starts on one.
    ULONG allocBytes = 0;
    status = RtlULongAdd(PacketSize, PAGE_SIZE - 1, &allocBytes);
    if (!NT_SUCCESS(status)) {
        ExFreePool(packets);
        return status;
    }
    allocBytes = (allocBytes / PAGE_SIZE) * PAGE_SIZE;
    const ULONG firstOffset = allocBytes - PacketSize;

    ULONG made = 0;
    for (; made < PacketCount; ++made) {
        ACX_RTPACKET_INIT(&packets[made]);
        PVOID buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, allocBytes, PADMIC_TAG);
        if (!buffer) { status = STATUS_NO_MEMORY; break; }
        PMDL mdl = IoAllocateMdl(buffer, allocBytes, FALSE, TRUE, nullptr);
        if (!mdl) { ExFreePool(buffer); status = STATUS_NO_MEMORY; break; }
        MmBuildMdlForNonPagedPool(mdl);
        WDF_MEMORY_DESCRIPTOR_INIT_MDL(&packets[made].RtPacketBuffer, mdl, allocBytes);
        packets[made].RtPacketSize = PacketSize;
        packets[made].RtPacketOffset = made == 0 ? firstOffset : 0;
        m_Packets[made] = buffer;
    }
    if (!NT_SUCCESS(status)) {
        FreeRtPackets(packets, made);
        return status;
    }

    *Packets = packets;
    m_PacketsCount = PacketCount;
    m_PacketSize = PacketSize;
    m_FirstPacketOffset = firstOffset;
    return STATUS_SUCCESS;
}

VOID CPadMicStream::FreeRtPackets(_Frees_ptr_ PACX_RTPACKET Packets, _In_ ULONG PacketCount) {
    for (ULONG i = 0; i < PacketCount; ++i) {
        PMDL mdl = Packets[i].RtPacketBuffer.u.MdlType.Mdl;
        if (mdl) {
            PVOID buffer = MmGetMdlVirtualAddress(mdl);
            IoFreeMdl(mdl);
            ExFreePool(buffer);
        }
    }
    ExFreePool(Packets);
    RtlZeroMemory(m_Packets, sizeof(m_Packets));
}

NTSTATUS CPadMicStream::PrepareHardware() {
    if (m_State == AcxStreamStatePause) return STATUS_SUCCESS;
    if (m_State != AcxStreamStateStop) return STATUS_INVALID_STATE_TRANSITION;

    WDF_TIMER_CONFIG timerCfg;
    WDF_TIMER_CONFIG_INIT(&timerCfg, CPadMicStream::s_EvtTimer);
    timerCfg.AutomaticSerialization = TRUE;
    // High resolution: the default timer ticks at 15.6 ms (or 1 ms at best), which cannot pace 10 ms packets.
    timerCfg.UseHighResolutionTimer = WdfTrue;
    timerCfg.Period = 0;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PADMIC_TIMER_CONTEXT);
    attributes.ParentObject = m_Stream;
    NTSTATUS status = WdfTimerCreate(&timerCfg, &attributes, &m_Timer);
    if (!NT_SUCCESS(status)) return status;
    GetPadMicTimerContext(m_Timer)->Engine = this;

    m_State = AcxStreamStatePause;
    return STATUS_SUCCESS;
}

NTSTATUS CPadMicStream::ReleaseHardware() {
    if (m_State == AcxStreamStateStop) return STATUS_SUCCESS;
    if (m_State == AcxStreamStateRun) { // ACX normally pauses first; do not leave the ring counting a reader that is gone
        WdfTimerStop(m_Timer, TRUE);
        g_Ring.StreamStopped();
    }
    if (m_Timer) {
        WdfTimerStop(m_Timer, TRUE);
        WdfObjectDelete(m_Timer);
        m_Timer = nullptr;
    }
    KeFlushQueuedDpcs();

    m_Position = 0;
    m_GlitchAdjust = 0;
    m_CurrentPacket = 0;
    m_State = AcxStreamStateStop;
    return STATUS_SUCCESS;
}

NTSTATUS CPadMicStream::Pause() {
    if (m_State == AcxStreamStatePause) return STATUS_SUCCESS;
    if (m_State != AcxStreamStateRun) return STATUS_INVALID_STATE_TRANSITION;

    WdfTimerStop(m_Timer, TRUE);
    UpdatePosition(); // remember where we stopped
    InterlockedIncrement(&g_Stats.Pauses);
    g_Ring.StreamStopped();
    m_State = AcxStreamStatePause;
    return STATUS_SUCCESS;
}

NTSTATUS CPadMicStream::Run() {
    if (m_State == AcxStreamStateRun) return STATUS_SUCCESS;
    if (m_State != AcxStreamStatePause) return STATUS_INVALID_STATE_TRANSITION;

    InterlockedIncrement(&g_Stats.Runs);
    g_Ring.StreamStarted();

    // After a pause the packets continue from the stored position, while the reported position stays absolute.
    m_StartTime = KSCONVERT_PERFORMANCE_TIME(m_QpcFrequency.QuadPart, KeQueryPerformanceCounter(nullptr));
    m_StartPosition = m_Position;
    m_GlitchAdjust = 0;

    m_State = AcxStreamStateRun; // before the first pass: UpdatePosition only counts while running
    ScheduleNextPass();
    return STATUS_SUCCESS;
}

NTSTATUS CPadMicStream::GetPresentationPosition(_Out_ PULONGLONG PositionInBlocks, _Out_ PULONGLONG QPCPosition) {
    const ULONG blockAlign = AcxDataFormatGetBlockAlign(m_Format);
    UpdatePosition();
    *PositionInBlocks = m_Position / blockAlign;
    *QPCPosition = static_cast<ULONGLONG>(KeQueryPerformanceCounter(nullptr).QuadPart);
    return STATUS_SUCCESS;
}

NTSTATUS CPadMicStream::GetCurrentPacket(_Out_ PULONG CurrentPacket) {
    *CurrentPacket = static_cast<ULONG>(InterlockedCompareExchange(reinterpret_cast<LONG*>(&m_CurrentPacket), -1, -1));
    return STATUS_SUCCESS;
}

NTSTATUS CPadMicStream::GetCapturePacket(_Out_ ULONG* LastCapturePacket, _Out_ ULONGLONG* QPCPacketStart, _Out_ BOOLEAN* MoreData) {
    const ULONG current = static_cast<ULONG>(InterlockedCompareExchange(reinterpret_cast<LONG*>(&m_CurrentPacket), -1, -1));
    const LONGLONG start = InterlockedCompareExchange64(&m_LastPacketStart.QuadPart, -1, -1);
    *LastCapturePacket = current - 1;
    *QPCPacketStart = static_cast<ULONGLONG>(start);
    *MoreData = FALSE;
    return STATUS_SUCCESS;
}

// Runs at DISPATCH_LEVEL on the stream timer.
VOID CPadMicStream::s_EvtTimer(_In_ WDFTIMER Timer) { GetPadMicTimerContext(Timer)->Engine->PassCallback(); }

VOID CPadMicStream::PassCallback() {
    const LONGLONG started = KeQueryPerformanceCounter(nullptr).QuadPart;
    InterlockedIncrement64(&g_Stats.Ticks);
    const LONGLONG late = started - m_DueQpc;
    if (late > 0) {
        InterlockedAdd64(&g_Stats.TickLateTotal, late);
        PadMicStatMax(&g_Stats.TickLateMax, late);
        if (late * 500 > g_Stats.QpcFrequency) InterlockedIncrement64(&g_Stats.LateTicks); // later than 2 ms
    }
    ProcessPacket();

    const ULONGLONG completed = static_cast<ULONG>(InterlockedIncrement(reinterpret_cast<LONG*>(&m_CurrentPacket))) - 1;
    const ULONGLONG qpc = static_cast<ULONGLONG>(KeQueryPerformanceCounter(nullptr).QuadPart);
    InterlockedExchange64(&m_LastPacketStart.QuadPart, m_CurrentPacketStart.QuadPart);
    InterlockedExchange64(&m_CurrentPacketStart.QuadPart, static_cast<LONGLONG>(qpc));

    (VOID)AcxRtStreamNotifyPacketComplete(m_Stream, completed, qpc);
    const LONGLONG busy = KeQueryPerformanceCounter(nullptr).QuadPart - started;
    InterlockedAdd64(&g_Stats.TickBusyTotal, busy);
    PadMicStatMax(&g_Stats.TickBusyMax, busy);
    ScheduleNextPass();
}

// The next packet is due at (start of this run) + (bytes so far / byte rate): computed from the start rather than added
// to the last time, so the timer's rounding never accumulates into drift against the audio engine's clock.
VOID CPadMicStream::ScheduleNextPass() {
    const ULONG bytesPerSecond = AcxDataFormatGetAverageBytesPerSec(m_Format);
    const ULONGLONG nextPacketStart = static_cast<ULONGLONG>(m_CurrentPacket + 1) * m_PacketSize;
    const ULONGLONG sinceResume = nextPacketStart - m_StartPosition;
    const ULONGLONG nextTime = m_StartTime + m_GlitchAdjust + sinceResume * kHnsPerSec / bytesPerSecond;
    const LARGE_INTEGER nowQpc = KeQueryPerformanceCounter(nullptr);
    const ULONGLONG now = KSCONVERT_PERFORMANCE_TIME(m_QpcFrequency.QuadPart, nowQpc);

    const LONGLONG delay = -static_cast<LONGLONG>(nextTime - now); // negative = relative
    if (delay >= 0) {
        // We are late (the system stalled): skip the lost time and deliver the packet right away.
        m_GlitchAdjust += delay;
        m_DueQpc = nowQpc.QuadPart; // not counted as a late tick: the pass runs at once
        PassCallback();
        return;
    }
    m_DueQpc = nowQpc.QuadPart + (-delay) * m_QpcFrequency.QuadPart / kHnsPerSec;
    WdfTimerStart(m_Timer, delay);
}

VOID CPadMicStream::UpdatePosition() {
    if (m_State != AcxStreamStateRun) return;
    const ULONG bytesPerSecond = AcxDataFormatGetAverageBytesPerSec(m_Format);
    const ULONGLONG now = KSCONVERT_PERFORMANCE_TIME(m_QpcFrequency.QuadPart, KeQueryPerformanceCounter(nullptr));
    m_Position = m_StartPosition - m_GlitchAdjust + (now - m_StartTime) * bytesPerSecond / kHnsPerSec;
}

VOID CPadMicStream::ProcessPacket() {
    const ULONG current = static_cast<ULONG>(InterlockedCompareExchange(reinterpret_cast<LONG*>(&m_CurrentPacket), -1, -1));
    const ULONG index = current % m_PacketsCount;
    PBYTE buffer = static_cast<PBYTE>(m_Packets[index]);
    if (index == 0) buffer += m_FirstPacketOffset; // packet 0 starts at an offset when the size is not a page multiple
    g_Ring.Read(reinterpret_cast<SHORT*>(buffer), m_PacketSize / sizeof(SHORT));
}
