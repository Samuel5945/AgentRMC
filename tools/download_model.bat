@echo off
setlocal
cd /d "%~dp0.."

rem Usage: download_model.bat [tiny / base / small / medium / large]
rem large maps to large-v3 (same as faster-whisper).
set SIZE=%1
if "%SIZE%"=="" set SIZE=small
if /I "%SIZE%"=="large" set SIZE=large-v3

set PROXY=http://127.0.0.1:7897
set REPO=https://huggingface.co/Systran/faster-whisper-%SIZE%/resolve/main
set DEST=models\faster-whisper-%SIZE%

if not exist "%DEST%" mkdir "%DEST%"

echo Downloading faster-whisper-%SIZE% via %PROXY%
echo Target: %DEST%
echo.

for %%F in (config.json tokenizer.json) do (
    echo   %%F
    curl -sfL -x %PROXY% --max-time 120 -o "%DEST%\%%F" "%REPO%/%%F"
)

rem large-v3 has no vocabulary.txt (tokenizer.json only)
if /I not "%SIZE%"=="large-v3" (
    echo   vocabulary.txt
    curl -sfL -x %PROXY% --max-time 120 -o "%DEST%\vocabulary.txt" "%REPO%/vocabulary.txt"
)

echo   model.bin  (large file, please wait)
curl -fL -x %PROXY% --max-time 3600 -o "%DEST%\model.bin" "%REPO%/model.bin"

echo.
dir "%DEST%"
echo.
echo Done. Pick the same size in the AgentRMC voice card, or restart tools\run.bat
pause
