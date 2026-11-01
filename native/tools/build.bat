@echo off
setlocal
set HERE=%~dp0
set ROOT=%HERE%..\..
set VS=C:\Program Files\Microsoft Visual Studio\18\Community
set CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set QT=%ROOT%\native\.qt\6.8.3\msvc2022_64
set SHERPA=ON
if /I "%~1"=="lean" set SHERPA=OFF

if not exist "%QT%\bin\windeployqt.exe" (
    echo [!] Qt 6.8.3 not found at %QT%
    echo     run: .venv\Scripts\python.exe -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -O native\.qt
    exit /b 1
)

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

"%CMAKE%" -S "%ROOT%\native" -B "%ROOT%\native\build" ^
    -G "Visual Studio 18 2026" -A x64 ^
    -DCMAKE_PREFIX_PATH="%QT%" ^
    -DAGENTRMC_ENABLE_SHERPA=%SHERPA%
if errorlevel 1 exit /b 1

"%CMAKE%" --build "%ROOT%\native\build" --config Release
if errorlevel 1 exit /b 1

"%QT%\bin\windeployqt.exe" --release --no-translations --no-opengl-sw ^
    --no-system-d3d-compiler --no-compiler-runtime ^
    "%ROOT%\native\build\Release\AgentRMCNative.exe" >nul
if errorlevel 1 exit /b 1

rem Widgets 版用不到 QRhi 的 D3D 着色器编译器和 VC 安装包，删掉省 ~34MB。
for %%f in (vc_redist.x64.exe dxcompiler.dll dxil.dll d3dcompiler_47.dll) do (
    if exist "%ROOT%\native\build\Release\%%f" del /q "%ROOT%\native\build\Release\%%f"
)

rem Keep the native data copy across rebuilds; seed it on first build.
if not exist "%ROOT%\native\build\Release\config.json" copy /y "%ROOT%\config.json" "%ROOT%\native\build\Release\" >nul
if not exist "%ROOT%\native\build\Release\profiles.json" copy /y "%ROOT%\profiles.json" "%ROOT%\native\build\Release\" >nul

rem Themes are loose folders next to the exe so users can drop in new ones.
if exist "%ROOT%\themes" robocopy "%ROOT%\themes" "%ROOT%\native\build\Release\themes" /E /NFL /NDL /NJH /NJS >nul

echo Done -^> native\build\Release\AgentRMCNative.exe
