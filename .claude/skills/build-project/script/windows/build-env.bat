@echo off
REM build-env.bat -- activate the MSVC + Ninja build environment.
REM
REM Usage (from a cmd shell, same session as the cmake commands):
REM   call .claude\skills\build-project\script\windows\build-env.bat
REM   cmake --preset x64-debug
REM   cmake --build --preset x64-debug
REM
REM Why a script (not inline): vcvars64 must run INSIDE the process that later
REM invokes cl.exe/ninja. Launching `cmd /c "vcvars && cmake"` from another shell
REM (bash/PowerShell) frequently breaks on the spaces in "Program Files", and
REM the environment does not survive across separate process spawns. Calling
REM this .bat with `call` keeps everything in one cmd session.
REM
REM Exit codes: 0 = environment ready (cl.exe on PATH); 1 = VS / vcvars not found.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" (
    echo [build-env] ERROR: vswhere.exe not found. Install Visual Studio with the
    echo            "Desktop development with C++" workload.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALL=%%i"
)

if not defined VSINSTALL (
    echo [build-env] ERROR: no Visual Studio with MSVC tools found via vswhere.
    exit /b 1
)

set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [build-env] ERROR: vcvars64.bat not found at: %VCVARS%
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo [build-env] ERROR: vcvars64.bat failed.
    exit /b 1
)

where ninja >nul 2>&1
if errorlevel 1 (
    echo [build-env] NOTE: ninja not on PATH. The Ninja that ships with VS CMake
    echo            is used automatically once CMake configures the preset.
)

echo [build-env] ready: cl.exe = %VCToolsInstallDir%bin\Hostx64\x64\cl.exe
set "VSWHERE="
set "VSINSTALL="
set "VCVARS="
exit /b 0
