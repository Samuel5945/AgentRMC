@echo off
setlocal
set ROOT=D:\githubrepos\Agentrmc
set LOG=%ROOT%\rc_probe\bat.log

echo [%date% %time%] bat start >> "%LOG%"
cd /d "%ROOT%"
echo [%date% %time%] cwd=%CD% >> "%LOG%"

net session >nul 2>&1
if errorlevel 1 (
    echo [%date% %time%] not admin, requesting elevation >> "%LOG%"
    echo Requesting administrator privileges...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo [%date% %time%] running as admin >> "%LOG%"

title AgentRMC raw HID probe
echo ============================================================
echo   Raw HID report capture  (ADMIN)
echo ------------------------------------------------------------
echo   When you see "opened device", do this:
echo     1. HOLD the voice key, speak 3-5 seconds, release
echo     2. Repeat 2-3 times
echo     3. Press UP / DOWN a few times as a control
echo.
echo   Recording for 90 seconds. Keep this window open.
echo ============================================================
echo.

"%ROOT%\.venv\Scripts\python.exe" -u "%ROOT%\rc_probe\rawreport.py" 90
echo [%date% %time%] python exited with %errorlevel% >> "%LOG%"

echo.
echo Finished.
pause
