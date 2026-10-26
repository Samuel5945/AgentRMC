@echo off
setlocal
cd /d "%~dp0.."

set PROXY=http://127.0.0.1:7897
set REPO=https://huggingface.co/Systran/faster-whisper-small/resolve/main
set DEST=models\faster-whisper-small

if not exist "%DEST%" mkdir "%DEST%"

echo Downloading faster-whisper-small via %PROXY%
echo Target: %DEST%
echo.

for %%F in (config.json tokenizer.json vocabulary.txt) do (
    echo   %%F
    curl -sL -x %PROXY% --max-time 120 -o "%DEST%\%%F" "%REPO%/%%F"
)

echo   model.bin  (about 480MB, please wait)
curl -L -x %PROXY% --max-time 1800 -o "%DEST%\model.bin" "%REPO%/model.bin"

echo.
dir "%DEST%"
echo.
echo Done. Now run tools\run.bat
pause
