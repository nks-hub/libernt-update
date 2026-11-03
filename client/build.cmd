@echo off
rem Builds LiberNTUpdate.exe (x64, static CRT) with the Visual Studio 2022 toolchain.
setlocal
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist out mkdir out
cl /nologo /std:c++17 /O2 /EHsc /W3 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Fo:out\ ^
   src\main.cpp src\gui.cpp src\engine.cpp src\http.cpp src\util.cpp src\sha256.cpp ^
   third_party\monocypher\monocypher.c third_party\monocypher\monocypher-ed25519.c ^
   /Fe:out\LiberNTUpdate.exe /link /SUBSYSTEM:WINDOWS ^
   wininet.lib comctl32.lib shell32.lib advapi32.lib user32.lib gdi32.lib
exit /b %errorlevel%
