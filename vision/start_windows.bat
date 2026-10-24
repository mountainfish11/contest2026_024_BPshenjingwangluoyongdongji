@echo off
setlocal
chcp 65001 >nul
set "PYTHONUTF8=1"
cd /d "%~dp0"

echo [BlindBadge] Checking Python and inference device...
python -X utf8 -m vision_assist.cli --check-gpu
if errorlevel 1 goto error

echo [BlindBadge] Checking board HTTP service...
curl.exe --noproxy "*" -fsS --max-time 5 http://192.168.4.1/health >nul
if errorlevel 1 goto board_error

echo.
echo [BlindBadge] Connect Windows Wi-Fi to BlindBadge-Vision.
echo [BlindBadge] Board stream: http://192.168.4.1/stream
echo [BlindBadge] Debug page:  http://127.0.0.1:8760/
echo.

python -X utf8 -m vision_assist.cli --serve --source mjpeg:http://192.168.4.1/stream --host 127.0.0.1 --port 8760 --device auto --target-fps 15 --height 1.2 --pitch 30 %*
if errorlevel 1 goto error
goto end

:board_error
echo.
echo [BlindBadge] Cannot reach http://192.168.4.1/health
echo [BlindBadge] Connect Wi-Fi to BlindBadge-Vision and choose to stay
echo [BlindBadge] connected even though Windows reports no Internet.
pause
goto end

:error
echo.
echo [BlindBadge] Startup failed. Review the message above.
pause

:end
endlocal
