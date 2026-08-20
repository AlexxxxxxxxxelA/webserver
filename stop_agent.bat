@echo off
setlocal EnableExtensions

set "PROJECT_ROOT=%~dp0"
if "%PROJECT_ROOT:~-1%"=="\" set "PROJECT_ROOT=%PROJECT_ROOT:~0,-1%"

where wsl.exe >nul 2>nul
if errorlevel 1 (
    echo [ERROR] WSL was not found.
    pause
    exit /b 1
)

for /f "usebackq delims=" %%I in (`wsl.exe wslpath -a "%PROJECT_ROOT%" 2^>nul`) do set "WSL_PROJECT=%%I"
if not defined WSL_PROJECT (
    echo [ERROR] Failed to convert the project path to a WSL path.
    pause
    exit /b 1
)

if /I "%~1"=="--check" (
    echo [OK] Project: %PROJECT_ROOT%
    echo [OK] WSL path: %WSL_PROJECT%
    echo [OK] Stop script prerequisites are available.
    exit /b 0
)

wsl.exe bash "%WSL_PROJECT%/tools/stop_server.sh" "%WSL_PROJECT%"
set "STOP_EXIT=%ERRORLEVEL%"
echo.
if "%STOP_EXIT%"=="0" (
    echo [INFO] Stop request completed.
) else (
    echo [ERROR] Failed to stop the project server.
)
pause
exit /b %STOP_EXIT%
