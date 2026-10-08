# Building, testing and signing the driver

acxmic is a **kernel** audio driver, because Windows only lists microphones that a driver provides, and audio drivers on Windows run in the kernel (the Audio Class Extensions, ACX, are KMDF-only). Windows with **Secure Boot on** (the default on current PCs) loads kernel drivers only when Microsoft has signed them. A user-mode (UMDF) driver would only need an ordinary code-signing signature, but an audio driver cannot be one.

This page covers how the driver is built, tested, and signed. Nothing here turns off Secure Boot or enables test-signing on your everyday Windows, and nothing should ask you to.

## Build

Needs the Visual Studio 2022 Build Tools and the Windows Driver Kit 10.0.26100 (`wdksetup.exe` from [Download the WDK](https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk), "Other WDK downloads" for the 26100 build).

```
build.cmd
```

This writes `out\padmic.sys`, `padmic.inf` and an unsigned `padmic.cat`. The driver is compiled with `cl` and `link` directly, because the WDK's Visual Studio toolset is not part of the Build Tools; it uses KMDF 1.31 and ACX 1.1 (Windows 10 2004 or later). The build uses `/W4 /WX`: no warnings allowed.

`tools\build-test.cmd` builds `out\padmic_test.exe`, which writes a tone into the driver and records it back through the Windows endpoint (see the header of `tools\padmic_test.cpp`).

## Develop and test: a VM with test-signing

Do this in a virtual machine (for example Hyper-V), not on your own Windows:

1. In the VM, turn Secure Boot off (Hyper-V: VM settings → Security) and run `bcdedit /set testsigning on` as administrator, then restart.
2. On the build machine: `sign-test.cmd` makes a test certificate and signs `padmic.sys` and `padmic.cat` with it.
3. Copy the `out` folder into the VM. Run `import-test-cert.cmd` there as administrator: it imports `out\PadDisplayTest.cer` into the VM's **Trusted Root Certification Authorities** and **Trusted Publishers** stores.
4. Create the root device `Root\PadDisplayMic` (class MEDIA) and install `padmic.inf` for it (`UpdateDriverForPlugAndPlayDevices`, as PadDisplay's installer does). Copy all three files (`.sys`, `.inf`, `.cat`): the catalog has to match.

## Release: Microsoft signing

Two routes exist; Microsoft's own page ([Driver signing options](https://learn.microsoft.com/windows-hardware/drivers/dashboard/driver-signing-offerings)) describes both.

**Attestation signing** is the route for a small project. You need:

- A **Microsoft Partner Center** account enrolled in the **Windows Hardware Developer Program**, and an **EV (extended validation) code-signing certificate** to sign the submission (they are sold by a few certificate authorities, as a hardware token or cloud signing).
- The submission file: `submission.cmd` creates `out\submission\padmic.cab` (the driver, its INF and its symbols).
- Sign that `.cab` with the EV certificate: `signtool sign /s MY /n "Your company name" /fd sha256 /tr <your CA's timestamp server> /td sha256 /v out\submission\padmic.cab`.
- In Partner Center: **Hardware → Submit new hardware**, upload the signed `.cab`, give it a product name, leave both test-signing options off, request the signatures for Windows 10 and 11 x64, and submit. Download the result when it is ready (usually a day or two).
- The download holds `padmic.sys`, `padmic.inf` and a catalog `padmic.cat`, signed by Microsoft. Publish those three files as a release of this repository. Projects that use the driver ship that release (PadDisplay puts the three files into `host/driver/mic/`).

Microsoft's page calls attestation signing "for testing purposes only": attestation-signed drivers cannot be published through Windows Update, and are not "Windows Certified". Its compatibility table lists them as accepted on Windows 10 and 11 client editions, so they load with Secure Boot on. They are **not** accepted by Windows Server, and the Hardware Compatibility Program (route two) is the official way to a certified, Windows-Update-distributed driver.

**WHCP (HLK tested):** the Windows Hardware Compatibility Program requires running the Hardware Lab Kit tests for audio devices on a test machine and submitting the results. It is the long route and is only worth it if the driver is to be distributed through Windows Update.

Neither route has been tested: no Partner Center account or EV certificate was available when this was written. The steps above come from Microsoft's documentation; check them against the current pages before relying on them.

## What Windows does without a Microsoft signature

On a PC with Secure Boot, installing a driver that is only test-signed fails with an error about the signature, or installs and then shows the device with problem code 52 ("Windows cannot verify the digital signature").
