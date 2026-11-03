@echo off
setlocal
set "HERE=%~dp0"
set "NSIS=C:\Program Files (x86)\NSIS\makensis.exe"

rem One-shot packaging: build + windeployqt + NSIS installer.
rem Output: dist\AgentRMC-Setup.exe
rem Optional arg "lean" DISABLES the sherpa-onnx local ASR backend at compile
rem time (build.bat sets AGENTRMC_ENABLE_SHERPA=OFF); models are never packaged
rem either way. Local speech needs a build WITHOUT lean (see AGENTS.md).

if not exist "%NSIS%" (
    echo [!] NSIS not found: "%NSIS%"
    echo     install from https://nsis.sourceforge.io or adjust the path here
    exit /b 1
)

call "%HERE%build.bat" %1
if errorlevel 1 exit /b 1

rem FR-07: CMake is the only version source. Fail before packaging if the
rem generated NSIS include or the built exe disagree with it.
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%check-version.ps1"
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
