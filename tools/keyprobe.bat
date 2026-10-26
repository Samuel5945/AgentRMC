@echo off
setlocal
cd /d "%~dp0.."
title AgentRMC key probe - press any key, it prints live
echo ============================================================
echo   This window stays open. Just press keys on the remote.
echo   Each press prints instantly. Close the window to stop.
echo ============================================================
echo.
".venv\Scripts\python.exe" -u rc_probe\keyprobe.py 0 %*
echo.
pause
