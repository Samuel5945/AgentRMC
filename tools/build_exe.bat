@echo off
setlocal
cd /d "%~dp0.."

echo ============================================================
echo   Build AgentRMC.exe
echo ============================================================
echo.

if not exist "assets\icon.ico" (
    echo [!] assets\icon.ico missing
    pause
    exit /b 1
)

".venv\Scripts\python.exe" -m PyInstaller ^
  --noconfirm --windowed ^
  --name AgentRMC --onedir ^
  --icon=assets\icon.ico ^
  --collect-data faster_whisper ^
  --distpath release --workpath build_release ^
  --exclude-module tkinter ^
  --exclude-module PySide6.QtWebEngineCore ^
  --exclude-module PySide6.QtWebEngineWidgets ^
  --exclude-module PySide6.Qt3DCore ^
  --exclude-module PySide6.QtMultimedia ^
  --exclude-module matplotlib ^
  agentrc_gui.py

if errorlevel 1 (
    echo.
    echo [!] build failed
    pause
    exit /b 1
)

echo.
echo Linking model directory...
if not exist "release\AgentRMC\models" (
    mklink /J "release\AgentRMC\models" "%CD%\models" >nul
)

echo Copying configs...
copy /y config.json release\AgentRMC\ >nul
copy /y commands.json release\AgentRMC\ >nul
copy /y profiles.json release\AgentRMC\ >nul

echo Copying assets...
if exist "release\AgentRMC\assets" rmdir /s /q "release\AgentRMC\assets"
xcopy /y /i /e /q assets release\AgentRMC\assets\ >nul

echo.
echo Done -^> release\AgentRMC\AgentRMC.exe
dir release\AgentRMC\AgentRMC.exe
pause
