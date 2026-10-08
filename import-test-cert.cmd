@echo off
rem Run as administrator INSIDE the test VM: trusts the test certificate that sign-test.cmd made, so the test-signed
rem driver installs there. Never run this on a PC you use every day.
setlocal
set CER=%~dp0out\PadDisplayTest.cer
if not exist "%CER%" (echo %CER% not found & exit /b 1)
certutil -addstore -f Root "%CER%" || exit /b 1
certutil -addstore -f TrustedPublisher "%CER%" || exit /b 1
echo Trusted. Make sure test-signing is on in this VM: bcdedit /set testsigning on, then restart.
