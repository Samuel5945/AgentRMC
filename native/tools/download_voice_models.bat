@echo off
setlocal EnableExtensions
set "ROOT=%~dp0.."
set "MODELS=%ROOT%\models"
set "THIRD=%ROOT%\third_party\sherpa-onnx"
set "TARGET=%~1"

if /I "%TARGET%"=="" goto usage
if /I "%TARGET%"=="runtime" goto runtime
if /I "%TARGET%"=="sensevoice" goto sensevoice
if /I "%TARGET%"=="qwen3" goto qwen3
goto usage

:usage
echo Usage: download_voice_models.bat [runtime^|sensevoice^|qwen3]
echo   runtime     sherpa-onnx Windows x64 prebuilt (about 7 MB)
echo   sensevoice  SenseVoiceSmall int8 model (about 158 MB download)
echo   qwen3       Qwen3-ASR 0.6B int8 model (about 838 MB download)
exit /b 1

:runtime
if exist "%THIRD%\lib\sherpa-onnx-c-api.dll" (
    echo [=] sherpa-onnx runtime already exists
    exit /b 0
)
if not exist "%THIRD%\lib" mkdir "%THIRD%\lib"
if not exist "%THIRD%\include\sherpa-onnx\c-api" mkdir "%THIRD%\include\sherpa-onnx\c-api"
curl.exe -L --fail -C - -o "%TEMP%\sherpa-onnx-win-x64.tar.bz2" ^
  "https://github.com/k2-fsa/sherpa-onnx/releases/download/v2.0.1/sherpa-onnx-v2.0.1-win-x64-shared-MD-Release-no-tts-lib.tar.bz2"
if errorlevel 1 goto fail
tar.exe -xjf "%TEMP%\sherpa-onnx-win-x64.tar.bz2" -C "%TEMP%"
if errorlevel 1 goto fail
copy /y "%TEMP%\sherpa-onnx-v2.0.1-win-x64-shared-MD-Release-no-tts-lib\lib\*" "%THIRD%\lib\" >nul
if errorlevel 1 goto fail
curl.exe -L --fail -o "%THIRD%\include\sherpa-onnx\c-api\c-api.h" ^
  "https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/v2.0.1/sherpa-onnx/c-api/c-api.h"
if errorlevel 1 goto fail
echo [OK] sherpa-onnx runtime -^> native\third_party\sherpa-onnx
exit /b 0

:sensevoice
if exist "%MODELS%\sensevoice\tokens.txt" (
    echo [=] SenseVoice model already exists
    exit /b 0
)
if not exist "%MODELS%" mkdir "%MODELS%"
curl.exe -L --fail -C - -o "%MODELS%\sensevoice.tar.bz2" ^
  "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2025-09-09.tar.bz2"
if errorlevel 1 goto fail
tar.exe -xjf "%MODELS%\sensevoice.tar.bz2" -C "%MODELS%"
if errorlevel 1 goto fail
if exist "%MODELS%\sensevoice" (
    if exist "%MODELS%\sensevoice.partial" rmdir /s /q "%MODELS%\sensevoice.partial"
    move "%MODELS%\sensevoice" "%MODELS%\sensevoice.partial" >nul
    if errorlevel 1 goto fail
)
move "%MODELS%\sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2025-09-09" "%MODELS%\sensevoice" >nul
if errorlevel 1 goto fail
echo [OK] SenseVoice model -^> native\models\sensevoice
exit /b 0

:qwen3
if exist "%MODELS%\qwen3-asr\tokenizer" (
    echo [=] Qwen3-ASR model already exists
    exit /b 0
)
if not exist "%MODELS%" mkdir "%MODELS%"
curl.exe -L --fail -C - -o "%MODELS%\qwen3-asr.tar.bz2" ^
  "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-qwen3-asr-0.6B-int8-2026-03-25.tar.bz2"
if errorlevel 1 goto fail
tar.exe -xjf "%MODELS%\qwen3-asr.tar.bz2" -C "%MODELS%"
if errorlevel 1 goto fail
if exist "%MODELS%\qwen3-asr" (
    if exist "%MODELS%\qwen3-asr.partial" rmdir /s /q "%MODELS%\qwen3-asr.partial"
    move "%MODELS%\qwen3-asr" "%MODELS%\qwen3-asr.partial" >nul
    if errorlevel 1 goto fail
)
move "%MODELS%\sherpa-onnx-qwen3-asr-0.6B-int8-2026-03-25" "%MODELS%\qwen3-asr" >nul
if errorlevel 1 goto fail
echo [OK] Qwen3-ASR model -^> native\models\qwen3-asr
exit /b 0

:fail
echo [!] download or extraction failed
exit /b 1
