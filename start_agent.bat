@echo off
setlocal EnableExtensions

set "PROJECT_ROOT=%~dp0"
if "%PROJECT_ROOT:~-1%"=="\" set "PROJECT_ROOT=%PROJECT_ROOT:~0,-1%"

where wsl.exe >nul 2>nul
if errorlevel 1 (
    echo [ERROR] WSL was not found. Install or enable WSL first.
    pause
    exit /b 1
)

where powershell.exe >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Windows PowerShell was not found.
    pause
    exit /b 1
)

set "PYTHON_CMD="
where py.exe >nul 2>nul
if not errorlevel 1 (
    py.exe -3 -c "import sys; assert sys.version_info.major == 3" >nul 2>nul
    if not errorlevel 1 set "PYTHON_CMD=py.exe -3"
)
if not defined PYTHON_CMD (
    where python.exe >nul 2>nul
    if not errorlevel 1 set "PYTHON_CMD=python.exe"
)
if not defined PYTHON_CMD (
    echo [ERROR] Windows Python 3 was not found.
    echo         You can still run the CLI with WSL Python:
    echo         wsl.exe python3 /mnt/f/webserver/webserver/tools/chat_client.py
    pause
    exit /b 1
)

for /f "usebackq delims=" %%I in (`wsl.exe wslpath -a "%PROJECT_ROOT%" 2^>nul`) do set "WSL_PROJECT=%%I"
if not defined WSL_PROJECT (
    echo [ERROR] Failed to convert the project path to a WSL path.
    pause
    exit /b 1
)

if not exist "%PROJECT_ROOT%\tools\chat_client.py" (
    echo [ERROR] tools\chat_client.py was not found under the project root.
    pause
    exit /b 1
)

for %%F in (tools\run_server.cmd tools\run_server.sh tools\stop_server.sh tools\check_environment.sh tools\wait_for_agent.ps1) do (
    if not exist "%PROJECT_ROOT%\%%F" (
        echo [ERROR] Required launcher file is missing: %%F
        pause
        exit /b 1
    )
)

wsl.exe bash "%WSL_PROJECT%/tools/check_environment.sh" "%WSL_PROJECT%" >nul
if errorlevel 1 (
    echo [ERROR] WSL build dependencies or the project path are not ready.
    echo         Run this command for details:
    echo         wsl.exe bash "%WSL_PROJECT%/tools/check_environment.sh" "%WSL_PROJECT%"
    pause
    exit /b 1
)

if /I "%~1"=="--check" (
    echo [OK] Project: %PROJECT_ROOT%
    echo [OK] WSL path: %WSL_PROJECT%
    echo [OK] Python: %PYTHON_CMD%
    echo [OK] Launcher prerequisites are available.
    exit /b 0
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PROJECT_ROOT%\tools\wait_for_agent.ps1" -TimeoutSeconds 0 >nul 2>nul
if not errorlevel 1 (
    echo [INFO] This project's Agent Server is already healthy on port 18081. Reusing it.
    goto :server_ready
)

call :start_and_wait
if errorlevel 2 (
    echo [ERROR] The Server window exited before becoming healthy.
    echo         Check the "C++ Agent Server" window for the build or config error.
    pause
    exit /b 1
)
if errorlevel 1 (
    echo [ERROR] Server did not become healthy within 120 seconds.
    echo         Check the "C++ Agent Server" window for build or config errors.
    pause
    exit /b 1
)

:server_ready

echo [INFO] Opening the HTTP/SSE chat client...
echo.
%PYTHON_CMD% "%PROJECT_ROOT%\tools\chat_client.py"
set "CLI_EXIT=%ERRORLEVEL%"

echo.
echo [INFO] Chat client exited with code %CLI_EXIT%.
echo [INFO] The C++ Agent Server is still running.
echo [INFO] Double-click stop_agent.bat when you want to stop it.
pause
exit /b %CLI_EXIT%

:start_and_wait
echo [INFO] Starting the C++ Agent Server in a separate window...
set "STATUS_FILE=%TEMP%\cpp_agent_server_%RANDOM%_%RANDOM%.status"
if exist "%STATUS_FILE%" del /q "%STATUS_FILE%" >nul 2>nul
start "C++ Agent Server" cmd.exe /k call "%PROJECT_ROOT%\tools\run_server.cmd" "%STATUS_FILE%"
echo [INFO] Waiting for the identified Agent service on http://127.0.0.1:18081/health ...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%PROJECT_ROOT%\tools\wait_for_agent.ps1" -TimeoutSeconds 120 -StatusFile "%STATUS_FILE%"
exit /b %ERRORLEVEL%
