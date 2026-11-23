@echo off
setlocal
rem Usage: build.bat [test | asan | release]   (asan: the tests, with AddressSanitizer)
rem Needs the C/C++ tools set up in this window: run vsenv first (tools\vsenv.bat, copied to a folder
rem on PATH), once per window.

where cl >nul 2>nul
if errorlevel 1 goto no_cl

set COMMON=/nologo /std:c11 /utf-8 /D_CRT_SECURE_NO_WARNINGS /W4 /wd4100 /wd4201 /wd4204 /wd4505 /wd4127 /Zi /I..\src /I.
set MODE=/Od
if "%1"=="release" set MODE=/O2 /DNDEBUG
rem /PDBALTPATH: the exe names its .pdb without this folder (no build paths in what ships).
set LINK=/INCREMENTAL:NO /OPT:REF /PDBALTPATH:%%_PDB%%

if not exist build mkdir build
rem Embed the CurseForge API key from cf_key.txt (gitignored), if present.
powershell -NoProfile -ExecutionPolicy Bypass -File tools\gen_key.ps1 cf_key.txt build\cf_key.h || goto fail_nopop
rem The GUI's CurseForge download window uses WebView2: fetch its SDK header and static loader once.
powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch_webview2.ps1 build\webview2 || goto fail_nopop
pushd build

set TESTS=
set TESTFLAGS=
if "%1"=="test" set TESTS=1
if "%1"=="asan" set TESTS=1
if "%1"=="asan" set TESTFLAGS=/fsanitize=address
if defined TESTS (
    cl %COMMON% /Od %TESTFLAGS% ..\src\main_test.c /Fe:wamtest.exe /link %LINK% /SUBSYSTEM:CONSOLE winhttp.lib shell32.lib version.lib ole32.lib crypt32.lib advapi32.lib || goto fail
    .\wamtest.exe || goto fail
    popd
    exit /b 0
)

rem DirectWrite's headers are C++ only: that one file is C++, compiled on its own (no exceptions, no RTTI).
cl /nologo /W4 /Zi %MODE% /EHs-c- /GR- /c ..\src\font\font_dwrite.cpp /Fo:font_dwrite.obj || goto fail
rem The icon (res\wam.rc; tools\make_icon.c draws it) and the version (src\version.h).
rc /nologo /i ..\src /fo wam.res ..\res\wam.rc || goto fail
cl %COMMON% %MODE% ..\src\main_gui.c /Fe:wam.exe /Iwebview2 /link %LINK% /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib dwmapi.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib dwrite.lib winhttp.lib version.lib ole32.lib crypt32.lib advapi32.lib uuid.lib shlwapi.lib windowscodecs.lib webview2\WebView2LoaderStatic.lib font_dwrite.obj wam.res || goto fail

popd
exit /b 0

:fail
popd
:fail_nopop
exit /b 1

:no_cl
echo error: the C/C++ compiler isn't set up in this window: run vsenv first ^(tools\vsenv.bat^)
exit /b 1
