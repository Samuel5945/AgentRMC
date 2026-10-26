@echo off
setlocal
cd /d "%~dp0.."
title RC Key Learn
".venv\Scripts\python.exe" rc_probe\learn_keys.py
echo.
pause
