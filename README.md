# acxmic

A virtual microphone driver for Windows: a small KMDF + ACX (Audio Class Extensions) kernel driver that adds one recording device, **"Microphone (PadDisplay Virtual Microphone)"**, to Windows. Any program that is allowed to open the control device can play 48 kHz mono 16-bit audio into it, and every app that records from that microphone hears it.

It was written for [PadDisplay](https://github.com/NezucloudResearch/PadDisplay), which plays a tablet's microphone into the PC this way, and is kept in its own repository so that it can be built, signed and released on its own. It has no dependency on PadDisplay. PadDisplay uses it only on a PC where Windows will load it (a Microsoft-signed build, or test-signing on); everywhere else PadDisplay falls back to VB-CABLE, because this repository has no Microsoft signature yet.

- **Latency:** the driver adds about 20 ms (a 20 ms cushion that is refilled at once, never more than 100 ms; the oldest audio is dropped rather than letting a late source build a delay).
- **Zero cost when idle:** the stream timer runs only while an app records.
- **Exclusive writer:** one program at a time can open the control device.
- **Windows 10 2004 or later** (ACX 1.1), x64.

The names (`Root\PadDisplayMic`, `\.\PadDisplayMic`, the endpoint name) are PadDisplay's and are kept so that PadDisplay and existing installs keep working.

## Use it

The control device is `\.\PadDisplayMic`. Open it with `CreateFile` (overlapped is fine), then `WriteFile` s16 mono PCM at 48 kHz, up to 16384 bytes per write. `IOCTL_PADMIC_GET_STATUS` returns the counters (interface version 2 adds what the driver itself spent, see below; a caller built for version 1 passes `PADMIC_STATUS_V1_SIZE` and still gets a valid answer), `IOCTL_PADMIC_RESET` empties the buffer. Everything is declared in [`include/padmic_public.h`](include/padmic_public.h), the header consumers include.

Installing: create the root-enumerated device `Root\PadDisplayMic` (class MEDIA, GUID `{4d36e96c-e325-11ce-bfc1-08002be10318}`) and install `padmic.inf` for it. A device that is disabled is not loaded: PadDisplay enables the device only while the user has switched the microphone on, and disables it again afterwards.

### Switch it off only when nobody records

Disabling or removing the device while an application records from it makes Windows' audio engine (`audiodg.exe`) refuse the removal. Windows then keeps the device "pending restart": it can be neither enabled nor disabled until a restart, and after the restart it comes back disabled and "not connected" (`pnputil /enable-device` fails with 1167) until the driver is installed again. Reproduced in a VM. So ask the driver first: `PADMIC_STATUS::StreamsRunning` is the number of applications recording right now (open the control device, `IOCTL_PADMIC_GET_STATUS`, close it), and the device should be disabled or removed only when it is 0. PadDisplay waits for that, and gives up the wait when the user switches the microphone on again.

## Build and sign

See [docs/SIGNING.md](docs/SIGNING.md). Short version: `build.cmd` (VS 2022 Build Tools + WDK 10.0.26100), `sign-test.cmd` for a test VM, and Microsoft attestation signing for a release that loads with Secure Boot on. **A release needs a Microsoft signature; this repository does not contain one yet.**

## Layout

| Path | What |
| --- | --- |
| `driver/` | The driver: `driver.cpp` (DriverEntry, control device), `circuit.cpp` (ACX circuit), `stream.cpp` (timer-driven stream engine), `ring.h` (the buffer), `padmic.inx` (INF template) |
| `include/padmic_public.h` | Shared with user mode: device path, IOCTLs, `PADMIC_STATUS` |
| `tools/` | `padmic_test`: writes a tone, records it back through WASAPI, measures frequency, gaps and latency; `burstrec` and `cablewrite` time a writer in another process or a virtual audio cable (`PADMIC_ENDPOINT` picks the recording device) |
| `build.cmd`, `sign-test.cmd`, `import-test-cert.cmd`, `submission.cmd` | Build, test-sign, trust the test certificate (in a VM), make the Partner Center submission |

## Status

Built and tested in a Hyper-V VM (Windows 11 26100, 2 vCPUs, test-signing). All numbers are the driver's own measurements (`padmic_test`, `PADMIC_STATUS`), not a bare-metal PC:

| What | Result |
| --- | --- |
| Idle (device enabled, nobody records) | 0 timer ticks in 30 s: no timer, no thread, nothing scheduled |
| While an app records | one timer pass per packet, about 100 per second; a pass takes 6-25 us on average (max 0.3-0.7 ms), which is 0.06-0.25 % of one core; a write takes 0.4-3 us on average |
| Timer lateness | 0.3 ms on average; max 1.0-2.1 ms in most runs, one 10 ms outlier in about 12 runs of 30 s |
| Write-to-recording latency | median 18.8 ms (a 20 ms cushion that is refilled at once) |
| Quality | 999.97 Hz measured for a 1000 Hz tone over 30 s, 0 dropouts in six runs; an earlier 30 s run had two gaps while the VM was busy, cause not proven |
| Memory | about 8.4 KB (4 allocations) per open stream, 0 after Windows lets the stream go; 150 record sessions: runs equal pauses, no leak |
| Switching | disable 75 ms, enable 72 ms on average (40 cycles); 25 x record-then-disable and a disable after 100 sessions all worked |
| Driver Verifier (standard checks + the KMDF verifier) | 34 loads, 33 unloads, 222 allocations, none failed, 0 outstanding at the end, no bugcheck; 30 x record-disable-enable worked |

**Not tested:** loading on a PC with Secure Boot (needs the Microsoft signature), Windows 10, drift over more than 60 s, DPC time on real hardware, the Hardware Lab Kit tests. Driver Verifier's random allocation failures were switched on (10 % and 50 % of the driver's allocations) but injected nothing (0 deliberate failures in 30 allocations), so the allocation-failure paths were only read, not run. Under Driver Verifier alone, one early run had a disable refused by a Windows audio service (no app was recording); thirty-one later attempts did not repeat it and the cause is unknown. See [SECURITY.md](SECURITY.md).

## License

MIT, see [LICENSE](LICENSE). Parts derive from Microsoft's ACX samples (MS-PL), see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
