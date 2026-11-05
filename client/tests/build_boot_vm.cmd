@echo off
setlocal
cd /d "%~dp0.."
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > out\vsdir.txt
set /p VSDIR=<out\vsdir.txt
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rc /nologo /fo out\vm.res res\app.rc || exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W3 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DLUUP_REGISTRY_TESTING /Fo:out\ ^
   tests\boot_vm.cpp src\gui.cpp src\tray_settings.cpp src\feed.cpp src\runtime.cpp src\registry.cpp src\registry_snapshot.cpp src\http.cpp src\util.cpp src\sha256.cpp boot\bootplan.c boot\regplan.c ^
   third_party\monocypher\monocypher.c third_party\monocypher\monocypher-ed25519.c ^
   /Fe:out\boot_vm.exe /link out\vm.res wininet.lib ws2_32.lib comctl32.lib shell32.lib advapi32.lib user32.lib gdi32.lib ntdll.lib
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++17 /O2 /EHsc /W3 /MT /utf-8 /DUNICODE /D_UNICODE /Fo:out\ tests\service_vm.cpp /Fe:out\service_vm.exe /link advapi32.lib
if errorlevel 1 exit /b %errorlevel%
lib /nologo /def:tests\crash_driver.def /machine:x64 /out:out\crash_ntoskrnl.lib || exit /b 1
cl /nologo /O1 /GS- /W3 /Fo:out\crash_driver.obj /c tests\crash_driver.c || exit /b 1
link /nologo /NODEFAULTLIB /SUBSYSTEM:NATIVE /DRIVER /ENTRY:DriverEntry /MACHINE:X64 /OUT:out\lntu_recovery_crash.sys out\crash_driver.obj out\crash_ntoskrnl.lib
exit /b %errorlevel%
