# Security

acxmic is a **kernel driver**. A bug in any kernel driver can crash Windows or be abused, so the surface is kept small.

- **Interface:** one control device, `\.\PadDisplayMic`, with the security descriptor `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`: SYSTEM and administrators have full access, interactive users may read and write. It is opened exclusively: only one program at a time.
- **What a program that opens it can do:** write audio into the microphone (up to 16384 bytes per write), read the driver's counters, and empty the buffer. The driver reads nothing from the PC and has no other interface. It accepts only whole 16-bit samples and validates every length.
- **Loading:** the driver is a root-enumerated device. Disabling the device unloads the driver, so a program can keep it unloaded except while it is needed (PadDisplay does this).
- **Signing:** Windows with Secure Boot loads kernel drivers only when Microsoft has signed them. A test-signed build only loads in a VM with test-signing on. Never turn test-signing on or Secure Boot off on a PC you use every day. See [docs/SIGNING.md](docs/SIGNING.md).
- **Not audited:** nobody outside the author has reviewed the driver, and it has not been run under Driver Verifier or through the Windows Hardware Lab Kit tests.

Report a vulnerability privately to the maintainer through GitHub's *Security → Report a vulnerability* on this repository.
