@echo off
rem Builds LiberNTUpdate.exe (x64, static CRT) with the newest Visual Studio that has the x64 C++ tools.
setlocal
cd /d "%~dp0"
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (echo Visual Studio with C++ x64 tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist out mkdir out
rem Optional default feed compiled in, e.g. set LNTU_FEED_URL=http://updates.example.org/ (config.json overrides it).
set FEEDDEF=
if defined LNTU_FEED_URL set FEEDDEF=/DLNTU_FEED_URL=L\"%LNTU_FEED_URL%\"
cl /nologo /std:c++17 /O2 /EHsc /W3 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN %FEEDDEF% /Fo:out\ ^
   src\main.cpp src\gui.cpp src\engine.cpp src\http.cpp src\util.cpp src\sha256.cpp ^
   third_party\monocypher\monocypher.c third_party\monocypher\monocypher-ed25519.c ^
   /Fe:out\LiberNTUpdate.exe /link /SUBSYSTEM:WINDOWS ^
   wininet.lib comctl32.lib shell32.lib advapi32.lib user32.lib gdi32.lib
exit /b %errorlevel%
