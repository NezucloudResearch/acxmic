@echo off
rem Makes the file you send to Microsoft for attestation signing: out\submission\padmic.cab with padmic.sys, padmic.inf
rem and padmic.pdb in one folder. Run build.cmd first. Then sign the .cab with your EV code-signing certificate and submit
rem it in Partner Center; docs\SIGNING.md has the steps. Microsoft replaces the catalog, so none is included.
setlocal
set OUT=%~dp0out
if not exist "%OUT%\padmic.sys" (echo Run build.cmd first & exit /b 1)
if exist "%OUT%\submission" rmdir /s /q "%OUT%\submission"

set DDF=%OUT%\padmic.ddf
(
echo .OPTION EXPLICIT
echo .Set CabinetFileCountThreshold=0
echo .Set FolderFileCountThreshold=0
echo .Set FolderSizeThreshold=0
echo .Set MaxCabinetSize=0
echo .Set MaxDiskFileCount=0
echo .Set MaxDiskSize=0
echo .Set CompressionType=MSZIP
echo .Set Cabinet=on
echo .Set Compress=on
echo .Set CabinetNameTemplate=padmic.cab
echo .Set DiskDirectoryTemplate=%OUT%\submission
echo .Set DestinationDir=padmic
echo %OUT%\padmic.sys
echo %OUT%\padmic.inf
echo %OUT%\padmic.pdb
) > "%DDF%"
makecab /f "%DDF%" || exit /b 1
echo Made %OUT%\submission\padmic.cab - now sign it with your EV certificate (docs\SIGNING.md).
