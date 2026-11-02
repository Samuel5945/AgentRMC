@echo off
setlocal
set "ROOT=%~dp0..\.."
set "SETUP=%ROOT%\dist\AgentRMC-Setup.exe"

rem Canonical local silent install. Run from any shell:
rem   native\tools\install-silent.bat
rem Do NOT call "AgentRMC-Setup.exe /S" directly from Git Bash: MSYS may turn
rem /S into a path before the installer sees it, which opens the wizard UI.
if not exist "%SETUP%" (
    echo [!] installer not found: "%SETUP%"
    echo     run native\tools\package.bat first
    exit /b 1
)

"%SETUP%" /S
if errorlevel 1 exit /b 1
echo Installed silently: "%SETUP%" /S
