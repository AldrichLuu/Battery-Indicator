@echo off
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto msvc_ready

set "VCVARS="
set "VS_PATH="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto vswhere_done
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
:vswhere_done
if defined VS_PATH if exist "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS (
    echo Failed to locate MSVC x64 build environment.
    exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 (
    echo Failed to initialize MSVC x64 build environment.
    exit /b 1
)

:msvc_ready

if exist BatteryIndicator.old.exe del /F /Q BatteryIndicator.old.exe >nul 2>nul
if exist BatteryIndicator.exe move /Y BatteryIndicator.exe BatteryIndicator.old.exe >nul 2>nul

cl /nologo /W3 /O2 /EHsc /std:c++20 /DUNICODE /D_UNICODE /Fe:BatteryIndicator.exe src\main.cpp /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup user32.lib gdi32.lib shell32.lib setupapi.lib cfgmgr32.lib bthprops.lib hid.lib advapi32.lib
set ERR=%errorlevel%
if exist main.obj del main.obj
if %ERR% EQU 0 (
    if exist BatteryIndicator.old.exe del /F /Q BatteryIndicator.old.exe >nul 2>nul
) else (
    if not exist BatteryIndicator.exe if exist BatteryIndicator.old.exe move /Y BatteryIndicator.old.exe BatteryIndicator.exe >nul 2>nul
)
exit /b %ERR%
