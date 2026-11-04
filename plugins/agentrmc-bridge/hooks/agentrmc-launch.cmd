@echo off
rem Resolve a Node-capable runtime, then run a JS entry with it.
rem   agentrmc-launch.cmd <hook-verb> [...]   -> hooks\agentrmc-notify.js <verb> [...]
rem   agentrmc-launch.cmd <script.js> [...]   -> <script.js> [...]   (MCP / generic)
rem Resolution order: explicit override -> host-provided var -> runtime the desktop
rem app resolved -> node on PATH / common install dirs -> ask Windows who spawned us.
rem Hooks must never block a session: unresolved runtime exits 0 silently.
setlocal
set "HERE=%~dp0"
set "JS=%HERE%agentrmc-notify.js"
if "%~1"=="" goto collect
set "FIRST=%~1"
if /i not "%FIRST:~-3%"==".js" goto collect
set "JS=%FIRST%"
shift

set "ARGS="

:collect
rem %* is not affected by shift, so rebuild the tail explicitly.
if "%~1"=="" goto resolve
set "ARGS=%ARGS% "%~1""
shift
goto collect

:resolve
set "RUNTIME="
if defined AGENTRMC_NODE set "RUNTIME=%AGENTRMC_NODE%"
if defined RUNTIME goto run
if defined QODER_NODE_RUNTIME set "RUNTIME=%QODER_NODE_RUNTIME%"
if defined RUNTIME goto run

rem Runtime the desktop app resolved from the hosts it sees running.
set "APPHINT=%LOCALAPPDATA%\AgentRMC\runtime\node.txt"
if exist "%APPHINT%" for /f "usebackq delims=" %%R in ("%APPHINT%") do if not defined RUNTIME set "RUNTIME=%%R"
if defined RUNTIME goto run

rem Real node on PATH or in a well-known spot (hosts like WorkBuddy ship their own).
for %%N in (node.exe) do set "RUNTIME=%%~$PATH:N"
if defined RUNTIME goto run
for %%D in (
  "%ProgramFiles%\nodejs"
  "%ProgramFiles(x86)%\nodejs"
  "%LOCALAPPDATA%\Programs\nodejs"
  "%LOCALAPPDATA%\Volta\bin"
  "%USERPROFILE%\scoop\shims"
  "%APPDATA%\nvm"
) do if not defined RUNTIME if exist "%%~D\node.exe" set "RUNTIME=%%~D\node.exe"
if defined RUNTIME goto run

rem Last resort: the nearest non-shell ancestor is the host that spawned this hook.
set "PROBE=%HERE%agentrmc-runtime.ps1"
if not exist "%PROBE%" goto done
for /f "usebackq delims=" %%R in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%PROBE%" 2^>nul`) do if not defined RUNTIME set "RUNTIME=%%R"
if defined RUNTIME goto run
goto done

:run
for %%F in ("%RUNTIME%") do set "RUNTIME_NAME=%%~nxF"
if /i "%RUNTIME_NAME%"=="node.exe" goto run_plain
rem An Electron host doubles as node when this is set.
set "ELECTRON_RUN_AS_NODE=1"
"%RUNTIME%" "%JS%" %ARGS%
exit /b %ERRORLEVEL%

:run_plain
"%RUNTIME%" "%JS%" %ARGS%
exit /b %ERRORLEVEL%

:done
endlocal
exit /b 0
