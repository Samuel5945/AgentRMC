@echo off
setlocal
cd /d "%~dp0.."
title AgentRMC
".venv\Scripts\python.exe" -u agentrc.py %*
echo.
pause
