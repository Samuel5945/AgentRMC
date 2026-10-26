@echo off
setlocal
cd /d "%~dp0.."
title AgentRMC GUI
if not exist ".venv\Scripts\pythonw.exe" (
    echo venv missing, run tools\setup.bat first
    pause
    exit /b 1
)
start "" ".venv\Scripts\pythonw.exe" agentrc_gui.py
