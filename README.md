# acxmic

A virtual microphone driver for Windows: a small KMDF + ACX (Audio Class Extensions) kernel driver that adds one recording device, **"Microphone (PadDisplay Virtual Microphone)"**, to Windows. Any program that is allowed to open the control device can play 48 kHz mono 16-bit audio into it, and every app that records from that microphone hears it.

It was written for [PadDisplay](https://github.com/NezucloudResearch/PadDisplay), which plays a tablet's microphone into the PC this way, and is kept in its own repository so that it can be built, signed and released on its own. It has no dependency on PadDisplay.

- **Latency:** the driver adds about 20 ms (a 20 ms cushion that is refilled at once, never more than 100 ms; the oldest audio is dropped rather than letting a late source build a delay).
- **Zero cost when idle:** the stream timer runs only while an app records.
- **Exclusive writer:** one program at a time can open the control device.
- **Windows 10 2004 or later** (ACX 1.1), x64.

The names (`Root\PadDisplayMic`, `\.\PadDisplayMic`, the endpoint name) are PadDisplay's and are kept so that PadDisplay and existing installs keep working.

## Use it

The control device is `\.\PadDisplayMic`. Open it with `CreateFile` (overlapped is fine), then `WriteFile` s16 mono PCM at 48 kHz, up to 16384 bytes per write. `IOCTL_PADMIC_GET_STATUS` returns the counters, `IOCTL_PADMIC_RESET` empties the buffer. Everything is declared in [`include/padmic_public.h`](include/padmic_public.h), the header consumers include.

Installing: create the root-enumerated device `Root\PadDisplayMic` (class MEDIA, GUID `{4d36e96c-e325-11ce-bfc1-08002be10318}`) and install `padmic.inf` for it. A device that is disabled is not loaded: PadDisplay enables the device only while the user has switched the microphone on, and disables it again afterwards.

## Build and sign

See [docs/SIGNING.md](docs/SIGNING.md). Short version: `build.cmd` (VS 2022 Build Tools + WDK 10.0.26100), `sign-test.cmd` for a test VM, and Microsoft attestation signing for a release that loads with Secure Boot on. **A release needs a Microsoft signature; this repository does not contain one yet.**

## Layout

| Path | What |
| --- | --- |
| `driver/` | The driver: `driver.cpp` (DriverEntry, control device), `circuit.cpp` (ACX circuit), `stream.cpp` (timer-driven stream engine), `ring.h` (the buffer), `padmic.inx` (INF template) |
| `include/padmic_public.h` | Shared with user mode: device path, IOCTLs, `PADMIC_STATUS` |
| `tools/` | `padmic_test`: writes a tone, records it back through WASAPI, measures frequency, gaps and latency |
| `build.cmd`, `sign-test.cmd`, `import-test-cert.cmd`, `submission.cmd` | Build, test-sign, trust the test certificate (in a VM), make the Partner Center submission |

## Status

Built and tested in a Hyper-V VM with test-signing (installs, loads, records a clean 1 kHz tone, 16-20 ms write-to-capture latency, exclusive open, crash recovery, remove and reinstall). **Not tested:** loading on a PC with Secure Boot (needs the Microsoft signature), Windows 10, drift over more than 60 s, CPU and DPC cost. See [SECURITY.md](SECURITY.md).

## License

MIT, see [LICENSE](LICENSE). Parts derive from Microsoft's ACX samples (MS-PL), see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
