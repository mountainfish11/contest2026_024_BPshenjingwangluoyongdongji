@echo off
setlocal
chcp 65001 >nul
set "PYTHONUTF8=1"
cd /d "%~dp0"

if not "%~1"=="" set "BOARD_IP=%~1"
if "%BOARD_IP%"=="" goto usage

set "LOG=%USERPROFILE%\Desktop\blindbadge_stream_test.txt"
set "JPEG=%USERPROFILE%\Desktop\blindbadge_capture.jpg"

echo [BlindBadge] Real-board vision link test > "%LOG%"
echo Timestamp: %date% %time% >> "%LOG%"
echo. >> "%LOG%"
echo === Windows network === >> "%LOG%"
ipconfig >> "%LOG%" 2>&1

echo [1/3] Checking board HTTP health...
echo. >> "%LOG%"
echo === Board health === >> "%LOG%"
curl.exe --noproxy "*" -fsS --max-time 5 http://%BOARD_IP%/health >> "%LOG%" 2>&1
if errorlevel 1 goto board_error
echo. >> "%LOG%"

echo [2/3] Downloading one JPEG frame...
echo. >> "%LOG%"
echo === JPEG capture === >> "%LOG%"
curl.exe --noproxy "*" -fsS --max-time 15 http://%BOARD_IP%/capture -o "%JPEG%" >> "%LOG%" 2>&1
if errorlevel 1 goto board_error
for %%F in ("%JPEG%") do echo JPEG bytes: %%~zF >> "%LOG%"

echo [3/3] Probing the real MJPEG stream...
echo. >> "%LOG%"
echo === MJPEG stream === >> "%LOG%"
python -X utf8 -m tools.probe_stream mjpeg:http://%BOARD_IP%/stream >> "%LOG%" 2>&1
if errorlevel 1 goto stream_error

echo. >> "%LOG%"
echo RESULT: PASS >> "%LOG%"
echo.
echo [BlindBadge] PASS: health, JPEG and MJPEG stream are working.
echo [BlindBadge] JPEG: %JPEG%
echo [BlindBadge] Log:  %LOG%
goto end

:usage
echo.
echo Usage: verify_board_windows.bat ^<BOARD_IP^>
echo Example: verify_board_windows.bat 10.242.126.64
goto end

:board_error
echo. >> "%LOG%"
echo RESULT: FAIL - board HTTP or JPEG capture >> "%LOG%"
echo.
echo [BlindBadge] Board HTTP/JPEG test failed.
echo [BlindBadge] Confirm all devices use the same Wi-Fi and review: %LOG%
goto end

:stream_error
echo. >> "%LOG%"
echo RESULT: FAIL - MJPEG stream >> "%LOG%"
echo.
echo [BlindBadge] JPEG worked, but MJPEG stream validation failed.
echo [BlindBadge] Review: %LOG%

:end
pause
endlocal
