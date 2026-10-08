// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Parts derived from Microsoft's ACX audio samples (Microsoft Public License); see THIRD_PARTY_NOTICES.md.
//
// Driver entry, the audio device, and the control device PadDisplay.exe writes the tablet's microphone to.
#include "padmic.h"

PadMicRing g_Ring;
PadMicStats g_Stats;
static WDFDEVICE g_ControlDevice = nullptr; // created with the audio device and deleted with it, so removing the device unloads the driver

// ---- operator new/delete (the driver is C++; the kernel has none) --------------------------------------------------
PVOID operator new(size_t size, POOL_FLAGS flags, ULONG tag) { return ExAllocatePool2(flags, size, tag); }
void __cdecl operator delete(PVOID p) { if (p) ExFreePool(p); }
void __cdecl operator delete(PVOID p, size_t) { if (p) ExFreePool(p); }

// ---- the control device -----------------------------------------------------------------------------------------------
// A control device (not a PnP interface of the audio device) keeps the audio stack's own file handling out of the way.
// Exclusive: one writer at a time, so a second PadDisplay cannot mix its audio into the first one's.
// Access: SYSTEM and administrators everything, interactive users read and write (the host runs as the logged-on user,
// elevated or not); no other account can open it.

static EVT_WDF_IO_QUEUE_IO_WRITE EvtControlWrite;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtControlIoctl;
static EVT_WDF_DEVICE_FILE_CREATE EvtControlCreate;
static EVT_WDF_FILE_CLEANUP EvtControlCleanup;

NTSTATUS PadMic_CreateControlDevice(_In_ WDFDRIVER Driver) {
    DECLARE_CONST_UNICODE_STRING(sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)");
    DECLARE_CONST_UNICODE_STRING(deviceName, L"\\Device\\PadDisplayMic");
    DECLARE_CONST_UNICODE_STRING(linkName, L"\\DosDevices\\Global\\PadDisplayMic");

    PWDFDEVICE_INIT init = WdfControlDeviceInitAllocate(Driver, &sddl);
    RETURN_NTSTATUS_IF_TRUE(init == nullptr, STATUS_INSUFFICIENT_RESOURCES);

    NTSTATUS status = WdfDeviceInitAssignName(init, &deviceName);
    if (NT_SUCCESS(status)) {
        WdfDeviceInitSetExclusive(init, TRUE);
        WDF_FILEOBJECT_CONFIG fileCfg;
        WDF_FILEOBJECT_CONFIG_INIT(&fileCfg, EvtControlCreate, WDF_NO_EVENT_CALLBACK, EvtControlCleanup);
        WdfDeviceInitSetFileObjectConfig(init, &fileCfg, WDF_NO_OBJECT_ATTRIBUTES);
    }

    WDFDEVICE device = nullptr;
    if (NT_SUCCESS(status)) status = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status)) {
        if (init) WdfDeviceInitFree(init);
        PADMIC_LOG("control device: 0x%08X", status);
        return status;
    }

    RETURN_NTSTATUS_IF_FAILED(WdfDeviceCreateSymbolicLink(device, &linkName));

    WDF_IO_QUEUE_CONFIG queueCfg;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueCfg, WdfIoQueueDispatchParallel);
    queueCfg.EvtIoWrite = EvtControlWrite;
    queueCfg.EvtIoDeviceControl = EvtControlIoctl;
    WDFQUEUE queue;
    RETURN_NTSTATUS_IF_FAILED(WdfIoQueueCreate(device, &queueCfg, WDF_NO_OBJECT_ATTRIBUTES, &queue));

    WdfControlFinishInitializing(device);
    g_ControlDevice = device;
    return STATUS_SUCCESS;
}

// The audio device is going away (uninstall, disable): take the control device with it. A control device would otherwise
// keep the driver loaded after the audio device is gone, and \\.\PadDisplayMic would stay openable until a restart.
static VOID EvtAudioDeviceCleanup(_In_ WDFOBJECT) {
    if (g_ControlDevice) {
        WDFDEVICE control = g_ControlDevice;
        g_ControlDevice = nullptr;
        WdfObjectDelete(control);
    }
    g_Ring.Reset();
}

_Use_decl_annotations_
VOID EvtControlCreate(WDFDEVICE, WDFREQUEST Request, WDFFILEOBJECT) {
    g_Ring.Reset(); // a new writer starts from nothing
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID EvtControlCleanup(WDFFILEOBJECT) {
    // The writer went away (closed, or its process ended): do not replay its last audio to whoever is recording.
    g_Ring.Reset();
}

_Use_decl_annotations_
VOID EvtControlWrite(WDFQUEUE, WDFREQUEST Request, size_t Length) {
    if (Length == 0 || (Length & 1) != 0 || Length > PADMIC_MAX_WRITE_BYTES) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    PVOID buffer = nullptr;
    NTSTATUS status = WdfRequestRetrieveInputBuffer(Request, Length, &buffer, nullptr);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }
    const LONGLONG t0 = KeQueryPerformanceCounter(nullptr).QuadPart;
    g_Ring.Write(static_cast<const SHORT*>(buffer), static_cast<ULONG>(Length / sizeof(SHORT)));
    const LONGLONG busy = KeQueryPerformanceCounter(nullptr).QuadPart - t0;
    InterlockedIncrement64(&g_Stats.Writes);
    InterlockedAdd64(&g_Stats.WriteBusyTotal, busy);
    PadMicStatMax(&g_Stats.WriteBusyMax, busy);
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Length);
}

_Use_decl_annotations_
VOID EvtControlIoctl(WDFQUEUE, WDFREQUEST Request, size_t OutputBufferLength, size_t, ULONG IoControlCode) {
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t info = 0;
    if (IoControlCode == IOCTL_PADMIC_GET_STATUS) {
        // Version 1 callers pass the smaller structure; they get the part they know.
        PADMIC_STATUS full{};
        PADMIC_STATUS* out = &full;
        PVOID callerBuffer = nullptr;
        size_t callerSize = 0;
        status = WdfRequestRetrieveOutputBuffer(Request, PADMIC_STATUS_V1_SIZE, &callerBuffer, &callerSize);
        if (NT_SUCCESS(status)) {
            ULONG fill, running, primed, underruns, drops;
            ULONGLONG written, read;
            g_Ring.Snapshot(&fill, &running, &primed, &underruns, &drops, &written, &read);
            out->Version = PADMIC_INTERFACE_VERSION;
            out->RingFillSamples = fill;
            out->StreamsRunning = running;
            out->Primed = primed;
            out->Underruns = underruns;
            out->Drops = drops;
            out->SamplesWritten = written;
            out->SamplesRead = read;
            out->StreamsCreated = static_cast<unsigned int>(g_Stats.Created);
            out->StreamsDestroyed = static_cast<unsigned int>(g_Stats.Destroyed);
            out->Runs = static_cast<unsigned int>(g_Stats.Runs);
            out->Pauses = static_cast<unsigned int>(g_Stats.Pauses);
            out->DestroyedWhileRunning = static_cast<unsigned int>(g_Stats.DestroyedWhileRunning);
            out->Ticks = static_cast<unsigned long long>(g_Stats.Ticks);
            out->QpcFrequency = static_cast<unsigned long long>(g_Stats.QpcFrequency);
            out->TickBusyTotal = static_cast<unsigned long long>(g_Stats.TickBusyTotal);
            out->TickBusyMax = static_cast<unsigned long long>(g_Stats.TickBusyMax);
            out->TickLateTotal = static_cast<unsigned long long>(g_Stats.TickLateTotal);
            out->TickLateMax = static_cast<unsigned long long>(g_Stats.TickLateMax);
            out->LateTicks = static_cast<unsigned long long>(g_Stats.LateTicks);
            out->Writes = static_cast<unsigned long long>(g_Stats.Writes);
            out->WriteBusyTotal = static_cast<unsigned long long>(g_Stats.WriteBusyTotal);
            out->WriteBusyMax = static_cast<unsigned long long>(g_Stats.WriteBusyMax);
            info = callerSize < sizeof(PADMIC_STATUS) ? callerSize : sizeof(PADMIC_STATUS);
            RtlCopyMemory(callerBuffer, out, info);
        }
    } else if (IoControlCode == IOCTL_PADMIC_RESET) {
        g_Ring.Reset();
        status = STATUS_SUCCESS;
    }
    UNREFERENCED_PARAMETER(OutputBufferLength);
    WdfRequestCompleteWithInformation(Request, status, info);
}

// ---- the audio device --------------------------------------------------------------------------------------------------

NTSTATUS PadMic_EvtDeviceAdd(_In_ WDFDRIVER, _Inout_ PWDFDEVICE_INIT DeviceInit) {
    ACX_DEVICEINIT_CONFIG initCfg;
    ACX_DEVICEINIT_CONFIG_INIT(&initCfg);
    RETURN_NTSTATUS_IF_FAILED(AcxDeviceInitInitialize(DeviceInit, &initCfg));

    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = PadMic_EvtDevicePrepareHardware;
    pnp.EvtDeviceReleaseHardware = PadMic_EvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PADMIC_DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = EvtAudioDeviceCleanup;
    WDFDEVICE device = nullptr;
    RETURN_NTSTATUS_IF_FAILED(WdfDeviceCreate(&DeviceInit, &attributes, &device));

    GetPadMicDeviceContext(device)->Capture = nullptr;
    if (!g_ControlDevice) RETURN_NTSTATUS_IF_FAILED(PadMic_CreateControlDevice(WdfGetDriver()));

    ACX_DEVICE_CONFIG devCfg;
    ACX_DEVICE_CONFIG_INIT(&devCfg);
    RETURN_NTSTATUS_IF_FAILED(AcxDeviceInitialize(device, &devCfg));

    WDF_DEVICE_PNP_CAPABILITIES caps;
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&caps);
    caps.SurpriseRemovalOK = WdfTrue;
    WdfDeviceSetPnpCapabilities(device, &caps);

    // The circuit becomes visible when the device reaches D0, after AcxDeviceAddCircuit in PrepareHardware.
    RETURN_NTSTATUS_IF_FAILED(PadMic_CreateCaptureCircuit(device, &GetPadMicDeviceContext(device)->Capture));
    return STATUS_SUCCESS;
}

NTSTATUS PadMic_EvtDevicePrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST, _In_ WDFCMRESLIST) {
    PPADMIC_DEVICE_CONTEXT ctx = GetPadMicDeviceContext(Device);
    RETURN_NTSTATUS_IF_TRUE(ctx->Capture == nullptr, STATUS_INVALID_DEVICE_STATE);
    return AcxDeviceAddCircuit(Device, ctx->Capture);
}

NTSTATUS PadMic_EvtDeviceReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST) {
    PPADMIC_DEVICE_CONTEXT ctx = GetPadMicDeviceContext(Device);
    if (ctx->Capture) return AcxDeviceRemoveCircuit(Device, ctx->Capture);
    return STATUS_SUCCESS;
}

// ---- driver entry --------------------------------------------------------------------------------------------------------

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath) {
    g_Ring.Init();
    LARGE_INTEGER frequency;
    KeQueryPerformanceCounter(&frequency);
    g_Stats.QpcFrequency = frequency.QuadPart;

    WDF_DRIVER_CONFIG wdfCfg;
    WDF_DRIVER_CONFIG_INIT(&wdfCfg, PadMic_EvtDeviceAdd);
    WDFDRIVER driver;
    RETURN_NTSTATUS_IF_FAILED(WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &wdfCfg, &driver));

    ACX_DRIVER_CONFIG acxCfg;
    ACX_DRIVER_CONFIG_INIT(&acxCfg);
    RETURN_NTSTATUS_IF_FAILED(AcxDriverInitialize(driver, &acxCfg));
    return STATUS_SUCCESS;
}
