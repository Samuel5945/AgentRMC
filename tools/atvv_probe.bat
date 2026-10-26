@echo off
setlocal
cd /d "%~dp0.."
title AgentRMC ATVV probe
echo ============================================================
echo   ATVV voice-channel probe
echo   1) make sure the remote is paired and awake (press any key)
echo   2) when the countdown starts, HOLD the voice key and speak
echo ============================================================
echo.
".venv\Scripts\python.exe" -u rc_probe\atvv_probe.py --seconds 30 %*
