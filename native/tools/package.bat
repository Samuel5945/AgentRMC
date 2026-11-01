@echo off
setlocal
set "HERE=%~dp0"
set "NSIS=C:\Program Files (x86)\NSIS\makensis.exe"

rem One-shot packaging: build + windeployqt + NSIS installer.
rem Output: dist\AgentRMC-Setup.exe
rem Optional arg "lean" skips local speech models (same as build.bat).

if not exist "%NSIS%" (
    echo [!] NSIS not found: "%NSIS%"
    echo     install from https://nsis.sourceforge.io or adjust the path here
    exit /b 1
)

call "%HERE%build.bat" %1
if errorlevel 1 exit /b 1

echo.
echo ============================================================
echo   Pack installer -^> dist\AgentRMC-Setup.exe
echo ============================================================
echo.

"%NSIS%" "%HERE%installer.nsi"
if errorlevel 1 exit /b 1

echo.
echo Done -^> dist\AgentRMC-Setup.exe
