@echo off
rem Signs out\padmic.sys and out\padmic.cat with a self-made TEST certificate, for a machine with test-signing on
rem (a Hyper-V VM with Secure Boot off; never your everyday Windows). Run build.cmd first.
rem Creates the certificate in the current user's store the first time, and exports out\PadDisplayTest.cer: import that
rem into the test machine's Trusted Root and Trusted Publishers stores (import-test-cert.cmd does it, inside the VM).
setlocal
set KIT=%ProgramFiles(x86)%\Windows Kits\10
set KVER=10.0.26100.0
set OUT=%~dp0out
if not exist "%OUT%\padmic.sys" (echo Run build.cmd first & exit /b 1)

powershell -NoProfile -Command ^
 "$c = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=PadDisplay Test Signing' -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1;" ^
 "if (-not $c) { $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=PadDisplay Test Signing' -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(3) };" ^
 "Export-Certificate -Cert $c -FilePath '%OUT%\PadDisplayTest.cer' | Out-Null; Set-Content -Path '%OUT%\thumbprint.txt' -Value $c.Thumbprint" || exit /b 1
set /p THUMB=<"%OUT%\thumbprint.txt"

"%KIT%\bin\%KVER%\x64\signtool.exe" sign /sha1 %THUMB% /fd sha256 "%OUT%\padmic.sys" || exit /b 1
"%KIT%\bin\%KVER%\x64\signtool.exe" sign /sha1 %THUMB% /fd sha256 "%OUT%\padmic.cat" || exit /b 1
echo Test-signed. Certificate: %OUT%\PadDisplayTest.cer
