// PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
// SPDX-License-Identifier: MIT
// Parts derived from Microsoft's ACX audio samples (Microsoft Public License); see THIRD_PARTY_NOTICES.md.
//
// The capture circuit: a host pin that applications record from, and a bridge pin that is the microphone endpoint.
//
//     Host pin ----------------> Bridge pin (microphone endpoint)
//
// One format only (48 kHz, 16-bit, mono): what the tablet sends, so there is nothing to convert in the driver; the
// audio engine converts for applications that want something else.
#include "padmic.h"

#define NOBITMAP
#include <mmreg.h>

static KSDATAFORMAT_WAVEFORMATEXTENSIBLE g_Pcm48000Mono = {
    {
        sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX),
    },
    {
        {
            WAVE_FORMAT_EXTENSIBLE,
            1,
            PADMIC_SAMPLE_RATE,
            PADMIC_SAMPLE_RATE * 2,
            2,
            16,
            sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX),
        },
        16,
        KSAUDIO_SPEAKER_MONO,
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
    },
};

static EVT_ACX_CIRCUIT_POWER_UP EvtCircuitPowerUp;
static EVT_ACX_CIRCUIT_POWER_DOWN EvtCircuitPowerDown;

_Use_decl_annotations_
static NTSTATUS EvtCircuitPowerUp(WDFDEVICE, ACXCIRCUIT, WDF_POWER_DEVICE_STATE) { return STATUS_SUCCESS; }

_Use_decl_annotations_
static NTSTATUS EvtCircuitPowerDown(WDFDEVICE, ACXCIRCUIT, WDF_POWER_DEVICE_STATE) { return STATUS_SUCCESS; }

// The format is fixed, so asking to change it is refused.
static NTSTATUS EvtPinSetDataFormat(_In_ ACXPIN, _In_ ACXDATAFORMAT) { return STATUS_NOT_SUPPORTED; }

NTSTATUS PadMic_CreateCaptureCircuit(_In_ WDFDEVICE Device, _Out_ ACXCIRCUIT* Circuit) {
    *Circuit = nullptr;

    // The circuit itself.
    PACXCIRCUIT_INIT circuitInit = AcxCircuitInitAllocate(Device);
    RETURN_NTSTATUS_IF_TRUE(circuitInit == nullptr, STATUS_INSUFFICIENT_RESOURCES);

    AcxCircuitInitSetComponentId(circuitInit, &PADMIC_COMPONENT_GUID);
    (VOID)AcxCircuitInitAssignName(circuitInit, &PadMicCircuitName);
    AcxCircuitInitSetCircuitType(circuitInit, AcxCircuitTypeCapture);

    ACX_CIRCUIT_PNPPOWER_CALLBACKS power;
    ACX_CIRCUIT_PNPPOWER_CALLBACKS_INIT(&power);
    power.EvtAcxCircuitPowerUp = EvtCircuitPowerUp;
    power.EvtAcxCircuitPowerDown = EvtCircuitPowerDown;
    AcxCircuitInitSetAcxCircuitPnpPowerCallbacks(circuitInit, &power);

    NTSTATUS status = AcxCircuitInitAssignAcxCreateStreamCallback(circuitInit, PadMic_EvtCircuitCreateStream);
    if (!NT_SUCCESS(status)) {
        AcxCircuitInitFree(circuitInit);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    ACXCIRCUIT circuit = nullptr;
    status = AcxCircuitCreate(Device, &attributes, &circuitInit, &circuit); // frees circuitInit on success
    if (!NT_SUCCESS(status)) {
        if (circuitInit) AcxCircuitInitFree(circuitInit);
        PADMIC_LOG("AcxCircuitCreate: 0x%08X", status);
        return status;
    }

    // The pins.
    ACXPIN pins[2] = {};
    {
        ACX_PIN_CALLBACKS callbacks;
        ACX_PIN_CALLBACKS_INIT(&callbacks);
        callbacks.EvtAcxPinSetDataFormat = EvtPinSetDataFormat;

        ACX_PIN_CONFIG cfg;
        ACX_PIN_CONFIG_INIT(&cfg);
        cfg.Type = AcxPinTypeSource;
        cfg.Communication = AcxPinCommunicationSink;
        cfg.Category = &KSCATEGORY_AUDIO;
        cfg.PinCallbacks = &callbacks;

        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = circuit;
        RETURN_NTSTATUS_IF_FAILED(AcxPinCreate(circuit, &attributes, &cfg, &pins[0])); // host pin
    }
    {
        ACX_PIN_CALLBACKS callbacks;
        ACX_PIN_CALLBACKS_INIT(&callbacks);

        ACX_PIN_CONFIG cfg;
        ACX_PIN_CONFIG_INIT(&cfg);
        cfg.Type = AcxPinTypeSink;
        cfg.Communication = AcxPinCommunicationNone;
        cfg.Category = &KSNODETYPE_MICROPHONE;
        cfg.PinCallbacks = &callbacks;

        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = circuit;
        RETURN_NTSTATUS_IF_FAILED(AcxPinCreate(circuit, &attributes, &cfg, &pins[1])); // bridge pin = the endpoint
    }

    // The bridge pin's jack: what Windows shows as the microphone's connector.
    {
        ACX_JACK_CONFIG jackCfg;
        ACX_JACK_CONFIG_INIT(&jackCfg);
        jackCfg.Description.ChannelMapping = KSAUDIO_SPEAKER_MONO;
        jackCfg.Description.Color = 0;
        jackCfg.Description.ConnectionType = AcxConnTypeAtapiInternal;
        jackCfg.Description.GeoLocation = AcxGeoLocFront;
        jackCfg.Description.GenLocation = AcxGenLocPrimaryBox;
        jackCfg.Description.PortConnection = AcxPortConnIntegratedDevice;

        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = pins[1];
        ACXJACK jack = nullptr;
        RETURN_NTSTATUS_IF_FAILED(AcxJackCreate(pins[1], &attributes, &jackCfg, &jack));
        RETURN_NTSTATUS_IF_FAILED(AcxPinAddJacks(pins[1], &jack, 1));
    }

    // The format the host pin offers (raw mode: the audio engine does its own processing on top).
    {
        ACX_DATAFORMAT_CONFIG formatCfg;
        ACX_DATAFORMAT_CONFIG_INIT_KS(&formatCfg, &g_Pcm48000Mono);
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = circuit;
        ACXDATAFORMAT format = nullptr;
        RETURN_NTSTATUS_IF_FAILED(AcxDataFormatCreate(Device, &attributes, &formatCfg, &format));

        ACXDATAFORMATLIST list = AcxPinGetRawDataFormatList(pins[0]);
        RETURN_NTSTATUS_IF_TRUE(list == nullptr, STATUS_INSUFFICIENT_RESOURCES);
        RETURN_NTSTATUS_IF_FAILED(AcxDataFormatListAddDataFormat(list, format));
    }

    RETURN_NTSTATUS_IF_FAILED(AcxCircuitAddPins(circuit, pins, SIZEOF_ARRAY(pins)));

    *Circuit = circuit;
    return STATUS_SUCCESS;
}

// A recording application opened the microphone: create its stream.
_Use_decl_annotations_
NTSTATUS PadMic_EvtCircuitCreateStream(WDFDEVICE Device, ACXCIRCUIT Circuit, ACXPIN, PACXSTREAM_INIT StreamInit,
                                       ACXDATAFORMAT StreamFormat, const GUID*, ACXOBJECTBAG) {
    ACX_STREAM_CALLBACKS streamCallbacks;
    ACX_STREAM_CALLBACKS_INIT(&streamCallbacks);
    streamCallbacks.EvtAcxStreamPrepareHardware = PadMic_EvtStreamPrepareHardware;
    streamCallbacks.EvtAcxStreamReleaseHardware = PadMic_EvtStreamReleaseHardware;
    streamCallbacks.EvtAcxStreamRun = PadMic_EvtStreamRun;
    streamCallbacks.EvtAcxStreamPause = PadMic_EvtStreamPause;
    RETURN_NTSTATUS_IF_FAILED(AcxStreamInitAssignAcxStreamCallbacks(StreamInit, &streamCallbacks));

    ACX_RT_STREAM_CALLBACKS rtCallbacks;
    ACX_RT_STREAM_CALLBACKS_INIT(&rtCallbacks);
    rtCallbacks.EvtAcxStreamGetHwLatency = PadMic_EvtStreamGetHwLatency;
    rtCallbacks.EvtAcxStreamAllocateRtPackets = PadMic_EvtStreamAllocateRtPackets;
    rtCallbacks.EvtAcxStreamFreeRtPackets = PadMic_EvtStreamFreeRtPackets;
    rtCallbacks.EvtAcxStreamGetCapturePacket = PadMic_EvtStreamGetCapturePacket;
    rtCallbacks.EvtAcxStreamGetCurrentPacket = PadMic_EvtStreamGetCurrentPacket;
    rtCallbacks.EvtAcxStreamGetPresentationPosition = PadMic_EvtStreamGetPresentationPosition;
    RETURN_NTSTATUS_IF_FAILED(AcxStreamInitAssignAcxRtStreamCallbacks(StreamInit, &rtCallbacks));

    // The engine is told after each packet (event-driven recording), not polled: fewer wakeups, lower latency.
    AcxStreamInitSetAcxRtStreamSupportsNotifications(StreamInit);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, PADMIC_STREAM_CONTEXT);
    attributes.EvtDestroyCallback = PadMic_EvtStreamDestroy;
    ACXSTREAM stream = nullptr;
    RETURN_NTSTATUS_IF_FAILED(AcxRtStreamCreate(Device, Circuit, &attributes, &StreamInit, &stream));

    CPadMicStream* engine = new (POOL_FLAG_NON_PAGED, PADMIC_TAG) CPadMicStream(stream, StreamFormat);
    RETURN_NTSTATUS_IF_TRUE(engine == nullptr, STATUS_INSUFFICIENT_RESOURCES);
    GetPadMicStreamContext(stream)->Engine = engine;
    return STATUS_SUCCESS;
}
