@echo off
setlocal
cd /d "%~dp0.."

echo ============================================================
echo   AgentRMC setup
echo ============================================================
echo.

if not exist ".venv\Scripts\python.exe" (
    echo [1/3] creating venv ...
    py -3 -m venv .venv 2>nul || python -m venv .venv
) else (
    echo [1/3] venv exists
)

echo [2/3] installing dependencies ...
".venv\Scripts\python.exe" -m pip install --quiet --upgrade pip
".venv\Scripts\python.exe" -m pip install --quiet bleak hidapi sounddevice numpy faster-whisper PySide6

echo [3/3] checking ASR model ...
if exist "models\faster-whisper-small\model.bin" (
    echo     model present
) else (
    echo     model missing, run tools\download_model.bat
)

echo.
echo Done. Run tools\run.bat to start.
pause
