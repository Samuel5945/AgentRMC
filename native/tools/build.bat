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

rem 每次强制用仓库干净模板覆盖 build 内的 config/profiles：installer 会把它们当
rem 全新安装的默认种子。若只在不存在时拷贝，开发期写进 build 的个人密钥/模型
rem 配置会残留并被分发出去（2026-11-04 实测泄露过一枚 API key）。
copy /y "%ROOT%\config.json" "%ROOT%\native\build\Release\" >nul
copy /y "%ROOT%\profiles.json" "%ROOT%\native\build\Release\" >nul

rem Themes are loose folders next to the exe so users can drop in new ones.
if exist "%ROOT%\themes" robocopy "%ROOT%\themes" "%ROOT%\native\build\Release\themes" /E /NFL /NDL /NJH /NJS >nul
rem ZCode 联动插件（agentrmc-bridge）随包分发，装完 AgentRMC 就地可用。
rem Version single source is plugins\agentrmc-bridge\VERSION; sync the five
rem manifests into it before the copy so the zip can't carry a stale version.
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%sync-plugin-version.ps1" >nul
if errorlevel 1 exit /b 1
if exist "%ROOT%\plugins" robocopy "%ROOT%\plugins" "%ROOT%\native\build\Release\plugins" /E /NFL /NDL /NJH /NJS >nul
rem robocopy exit codes 0-7 mean success (1 = files copied); 8+ are failures.
rem Normalize so chained scripts don't treat a good copy as an error.
if errorlevel 8 exit /b 1
ver >nul

rem Qoder Desktop only accepts a ZIP upload (no local directory source), so ship
rem one next to the plugin source. Entry names must use "/" - see zip-plugin.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%zip-plugin.ps1" ^
    -Src "%ROOT%\plugins\agentrmc-bridge" ^
    -Out "%ROOT%\native\build\Release\plugins\agentrmc-bridge.zip" >nul
if errorlevel 1 echo [!] plugin zip step failed (Qoder upload needs the zip; other hosts read the source dir)

rem Qoder names its local plugins "@local" and its update button does not pull a
rem new zip for those. Ship a clearly named copy so users can re-upload it.
copy /y "%ROOT%\native\build\Release\plugins\agentrmc-bridge.zip" "%ROOT%\native\build\Release\plugins\agentrmc-bridge-qoder.zip" >nul

rem Mirror the plugin zip into dist with the manifest version in the filename.
rem FR-07: "which zip is current" must be answerable from the name alone.
for /f "usebackq delims=" %%v in (`powershell -NoProfile -Command "(Get-Content -Raw -Encoding UTF8 '%ROOT%\plugins\agentrmc-bridge\VERSION').Trim()"`) do set "PLUGVER=%%v"
if defined PLUGVER (
    if not exist "%ROOT%\dist" mkdir "%ROOT%\dist"
    copy /y "%ROOT%\native\build\Release\plugins\agentrmc-bridge.zip" "%ROOT%\dist\agentrmc-bridge-%PLUGVER%.zip" >nul
    copy /y "%ROOT%\native\build\Release\plugins\agentrmc-bridge-qoder.zip" "%ROOT%\dist\agentrmc-bridge-qoder-%PLUGVER%.zip" >nul
    echo Plugin zip -^> dist\agentrmc-bridge-%PLUGVER%.zip
    echo Qoder zip  -^> dist\agentrmc-bridge-qoder-%PLUGVER%.zip
)

echo Done -^> native\build\Release\AgentRMCNative.exe
