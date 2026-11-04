@echo off
setlocal
chcp 65001 >nul
set "PYTHONUTF8=1"
cd /d "%~dp0"

if not "%~1"=="" set "BOARD_IP=%~1"
if "%BOARD_IP%"=="" goto usage

echo [BlindBadge] Checking Python and inference device...
python -X utf8 -m vision_assist.cli --check-gpu
if errorlevel 1 goto error

echo [BlindBadge] Checking board HTTP service...
curl.exe --noproxy "*" -fsS --max-time 5 http://%BOARD_IP%/health >nul
if errorlevel 1 goto board_error

echo.
echo [BlindBadge] Board stream: http://%BOARD_IP%/stream
echo [BlindBadge] Debug page:  http://127.0.0.1:8760/
echo.

python -X utf8 -m vision_assist.cli --serve --source mjpeg:http://%BOARD_IP%/stream --host 127.0.0.1 --port 8760 --device auto --target-fps 30 --height 1.2 --pitch 30
if errorlevel 1 goto error
goto end

:usage
echo.
echo Usage: start_windows.bat ^<BOARD_IP^>
echo Example: start_windows.bat 10.242.126.64
goto end

:board_error
echo.
echo [BlindBadge] Cannot reach http://%BOARD_IP%/health
echo [BlindBadge] Confirm Windows, main board and sub-board use the same Wi-Fi.
pause
goto end

:error
echo.
echo [BlindBadge] Startup failed. Review the message above.
pause

:end
endlocal
