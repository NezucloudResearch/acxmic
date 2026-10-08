@echo off
rem Builds the acxmic virtual microphone driver (KMDF + ACX) into out\: padmic.sys, padmic.inf, padmic.cat (unsigned).
rem Needs the VS 2022 Build Tools (same as the host build) and the Windows Driver Kit 10.0.26100 (wdksetup). The driver is
rem built with cl/link directly because the WDK's Visual Studio toolset is not part of the Build Tools.
rem Signing is separate: sign-test.cmd (test certificate, for a VM with test-signing) or Microsoft attestation signing
rem (see docs\SIGNING.md) for a PC with Secure Boot.
setlocal
set KIT=%ProgramFiles(x86)%\Windows Kits\10
set KVER=10.0.26100.0
set KMDF=1.31
set ACX=1.1
if not exist "%KIT%\Include\%KVER%\km\wdm.h" (echo Windows Driver Kit %KVER% not found in "%KIT%" & exit /b 1)
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS=%%i
if not defined VS (echo Visual Studio Build Tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0driver"

set OUT=%~dp0out
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%" || exit /b 1

set INC=/I"%~dp0include" /I"%KIT%\Include\%KVER%\km" /I"%KIT%\Include\%KVER%\km\crt" /I"%KIT%\Include\%KVER%\um" /I"%KIT%\Include\%KVER%\shared" /I"%KIT%\Include\%KVER%\km\acx\km\%ACX%" /I"%KIT%\Include\wdf\kmdf\%KMDF%"
set DEFS=/D_AMD64_ /DAMD64 /D_WIN64 /DNTDDI_VERSION=0x0A000008 /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DWINNT=1 ^
 /DKMDF_VERSION_MAJOR=1 /DKMDF_VERSION_MINOR=31 /DACX_VERSION_MAJOR=1 /DACX_VERSION_MINOR=1 /DPOOL_NX_OPTIN_AUTO=1 /DDEPRECATE_DDK_FUNCTIONS=1 /DNDEBUG
rem /kernel: no exceptions or RTTI, kernel-safe code generation. /GS-: the stack cookie needs the CRT; the driver uses BufferOverflowFastFailK.lib instead.
set CFLAGS=/nologo /c /kernel /GS- /Gy /O2 /Oi /Zi /Zp8 /W4 /WX /wd4201 /wd4214 /wd4324 /Zc:wchar_t /Zc:forScope /Zc:inline /FC /Fo"%OUT%\\" /Fd"%OUT%\vc.pdb"

cl %CFLAGS% %INC% %DEFS% driver.cpp circuit.cpp stream.cpp || exit /b 1
rc /nologo /I"%KIT%\Include\%KVER%\um" /I"%KIT%\Include\%KVER%\shared" /fo"%OUT%\padmic.res" padmic.rc || exit /b 1

link /nologo /DRIVER /SUBSYSTEM:NATIVE,10.00 /ENTRY:FxDriverEntry /NODEFAULTLIB /MACHINE:X64 /OPT:REF /OPT:ICF /RELEASE ^
 /INTEGRITYCHECK /NXCOMPAT /DYNAMICBASE /DEBUG /PDB:"%OUT%\padmic.pdb" /OUT:"%OUT%\padmic.sys" ^
 /LIBPATH:"%KIT%\Lib\%KVER%\km\x64" /LIBPATH:"%KIT%\Lib\wdf\kmdf\x64\%KMDF%" /LIBPATH:"%KIT%\Lib\%KVER%\km\x64\acx\km\%ACX%" ^
 "%OUT%\driver.obj" "%OUT%\circuit.obj" "%OUT%\stream.obj" "%OUT%\padmic.res" ^
 ntoskrnl.lib hal.lib wmilib.lib BufferOverflowFastFailK.lib libcntpr.lib WdfDriverEntry.lib WdfLdr.lib acxstub.lib || exit /b 1

copy /y padmic.inx "%OUT%\padmic.inf" >nul
"%KIT%\bin\%KVER%\x86\stampinf.exe" -f "%OUT%\padmic.inf" -d * -a amd64 -k %KMDF% -v * || exit /b 1
rem 10_VB_X64: Windows 10 2004 and later, the oldest version ACX 1.1 runs on.
"%KIT%\bin\%KVER%\x86\Inf2Cat.exe" /driver:"%OUT%" /os:10_VB_X64 /uselocaltime || exit /b 1
"%KIT%\Tools\%KVER%\x64\infverif.exe" /v /w "%OUT%\padmic.inf"

echo Built %OUT%\padmic.sys (unsigned); padmic.cat is unsigned too.
