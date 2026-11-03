@echo off
rem Builds LiberNTUpdate.exe and luupboot.exe (x64) with the newest Visual Studio that has the x64 C++ tools.
setlocal
cd /d "%~dp0"
if not exist out\native mkdir out\native
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > out\vsdir.txt
set /p VSDIR=<out\vsdir.txt
if not defined VSDIR (echo Visual Studio with C++ x64 tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

rem luupboot.exe: native subsystem, ntdll only, no C runtime.
cl /nologo /O1 /GS- /W3 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /Fo:out\native\ /c boot\luupboot.c boot\bootplan.c || exit /b 1
link /nologo /SUBSYSTEM:NATIVE /ENTRY:NtProcessStartup /NODEFAULTLIB /OUT:out\luupboot.exe ^
   out\native\luupboot.obj out\native\bootplan.obj ntdll.lib || exit /b 1

rem Optional default feed compiled in, e.g. set LNTU_FEED_URL=http://updates.example.org/ (config.json overrides it).
set FEEDDEF=
if defined LNTU_FEED_URL set FEEDDEF=/DLNTU_FEED_URL=L\"%LNTU_FEED_URL%\"
set VERDEF=
if defined LNTU_VERSION set VERDEF=/DLNTU_VERSION=\"%LNTU_VERSION%\"
rc /nologo /fo out\app.res res\app.rc || exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W3 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN %FEEDDEF% %VERDEF% /Fo:out\ ^
   src\main.cpp src\gui.cpp src\engine.cpp src\http.cpp src\util.cpp src\sha256.cpp boot\bootplan.c ^
   third_party\monocypher\monocypher.c third_party\monocypher\monocypher-ed25519.c ^
   /Fe:out\LiberNTUpdate.exe /link /SUBSYSTEM:WINDOWS out\app.res ^
   wininet.lib comctl32.lib shell32.lib advapi32.lib user32.lib gdi32.lib ntdll.lib
exit /b %errorlevel%
