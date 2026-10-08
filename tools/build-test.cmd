@echo off
rem Builds tools\padmic_test.exe, the test tool: writes a tone into the driver and records it back through WASAPI.
rem Usage after building: padmic_test status | open | hold | selftest | record | latency (see the source header).
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VS=%%i
if not defined VS (echo Visual Studio Build Tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%~dp0..\out" mkdir "%~dp0..\out"
cl /nologo /std:c++20 /EHsc /O2 /W4 /I"%~dp0..\include" /Fo"%~dp0..\out\\" /Fe"%~dp0..\out\padmic_test.exe" "%~dp0padmic_test.cpp" ole32.lib uuid.lib propsys.lib user32.lib avrt.lib || exit /b 1
echo Built %~dp0..\out\padmic_test.exe
