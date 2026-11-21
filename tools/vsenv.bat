@echo off
rem vsenv - sets up the Visual Studio C/C++ tools (cl, link, rc; x64) for building, once per window.
rem
rem   In cmd:        sets up this window.
rem   In PowerShell: a .bat can't change PowerShell's environment, so it starts a set-up PowerShell in
rem                  this window (its prompt starts with [vs]); `exit` goes back to the one before.
rem   vsenv --here   only sets up the cmd it runs in, never starts a shell (for scripts: release.ps1).
rem
rem Install: copy it to a folder on PATH (e.g. %USERPROFILE%\.local\bin). It finds Visual Studio or its
rem Build Tools with vswhere, so it doesn't depend on where they're installed.

set "VSENV_HERE="
if /i "%~1"=="--here" set "VSENV_HERE=1"

where cl >nul 2>nul
if not errorlevel 1 (
    if not defined VSENV_HERE echo vsenv: already set up in this window
    exit /b 0
)

set "VSENV_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSENV_VSWHERE%" (
    echo vsenv: Visual Studio isn't installed ^(no vswhere.exe^) 1>&2
    exit /b 1
)
set "VSENV_DIR="
for /f "usebackq tokens=*" %%i in (`"%VSENV_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSENV_DIR=%%i"
if not defined VSENV_DIR (
    echo vsenv: no Visual Studio with the C++ build tools ^(x64^) found 1>&2
    exit /b 1
)
rem (VS's setup may change folders, e.g. to %USERPROFILE%\source: stay in this one.)
set "VSCMD_START_DIR=%CD%"
pushd "%CD%"
call "%VSENV_DIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
popd
where cl >nul 2>nul
if errorlevel 1 (
    echo vsenv: vcvars64.bat ran, but cl still isn't found 1>&2
    exit /b 1
)
if not defined VSENV_HERE echo vsenv: C/C++ tools ready ^(x64, %VSENV_DIR%^)

:shell
rem Started by PowerShell (cmd /c), not typed in cmd: what was set up ends with this cmd, so hand
rem it to a PowerShell started here (inside a vsenv shell already: nothing more to do).
if defined VSENV_HERE exit /b 0
if defined VSENV_SHELL exit /b 0
echo %CMDCMDLINE% | findstr /i /c:" /c " >nul
if errorlevel 1 exit /b 0
set "VSENV_SHELL=1"
echo vsenv: starting a set-up PowerShell here; `exit` goes back
powershell.exe -NoLogo -NoExit -Command "function global:prompt { '[vs] PS ' + $executionContext.SessionState.Path.CurrentLocation + '> ' }"
exit /b 0
