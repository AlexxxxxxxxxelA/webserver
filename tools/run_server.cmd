@echo off
setlocal EnableExtensions

set "PROJECT_ROOT=%~dp0.."
set "STATUS_FILE=%~1"
for %%I in ("%PROJECT_ROOT%") do set "PROJECT_ROOT=%%~fI"

for /f "usebackq delims=" %%I in (`wsl.exe wslpath -a "%PROJECT_ROOT%" 2^>nul`) do set "WSL_PROJECT=%%I"
if not defined WSL_PROJECT (
    echo [ERROR] Failed to convert the project path to WSL.
    exit /b 1
)

title C++ Agent Server
wsl.exe bash "%WSL_PROJECT%/tools/run_server.sh" "%WSL_PROJECT%"
set "SERVER_EXIT=%ERRORLEVEL%"
if defined STATUS_FILE >"%STATUS_FILE%" echo %SERVER_EXIT%
echo.
echo [INFO] Server process exited with code %SERVER_EXIT%.
exit /b %SERVER_EXIT%
