@echo off
cd /d D:\githubrepos\Agentrmc
set "no_proxy="
set "no_proxy="
set "http_proxy="
set "http_proxy="
set "https_proxy="
set "https_proxy="
set "all_proxy="
set "all_proxy="
set "TEMP=D:\githubrepos\Agentrmc\tmp_test"
set "TMP=D:\githubrepos\Agentrmc\tmp_test"
echo === build ===
call native\tools\build.bat
echo build_exit=%ERRORLEVEL%
cd /d D:\githubrepos\Agentrmc\native\build\Release
echo === BridgeTest ===
AgentRMCBridgeTest.exe
echo bridge_exit=%ERRORLEVEL%
echo === AppTest ===
AgentRMCAppTest.exe
echo app_exit=%ERRORLEVEL%
