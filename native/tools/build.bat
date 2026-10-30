@echo off
setlocal
set HERE=%~dp0
set ROOT=%HERE%..\..
set VS=C:\Program Files\Microsoft Visual Studio\18\Community
set CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set QT=%ROOT%\native\.qt\6.8.3\msvc2022_64

if not exist "%QT%\bin\windeployqt.exe" (
    echo [!] Qt 6.8.3 not found at %QT%
    echo     run: .venv\Scripts\python.exe -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O native\.qt
    exit /b 1
)

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

"%CMAKE%" -S "%ROOT%\native" -B "%ROOT%\native\build" ^
    -G "Visual Studio 18 2026" -A x64 ^
    -DCMAKE_PREFIX_PATH="%QT%"
if errorlevel 1 exit /b 1

"%CMAKE%" --build "%ROOT%\native\build" --config Release
if errorlevel 1 exit /b 1

"%QT%\bin\windeployqt.exe" --release --no-translations --no-opengl-sw ^
    "%ROOT%\native\build\Release\AgentRMCNative.exe" >nul
if errorlevel 1 exit /b 1

echo Done -^> native\build\Release\AgentRMCNative.exe
