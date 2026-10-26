@echo off
setlocal
cd /d "%~dp0.."
title AgentRMC raw HID report test
if "%~1"=="" (set SECS=30) else (set SECS=%~1)
echo ============================================================
echo   HID raw report sniff  (%SECS% seconds)
echo   When the countdown starts, HOLD the voice key and speak
echo   3 seconds, release, then press once more.
echo ============================================================
echo.
".venv\Scripts\python.exe" -u rc_probe\rawsize.py %SECS%
echo.
pause
