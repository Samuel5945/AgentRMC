; AgentRMC 单文件安装包：装到 %LOCALAPPDATA%\AgentRMC，全程不需要管理员。
; 构建：makensis native\tools\installer.nsi  →  dist\AgentRMC-Setup.exe
Unicode true

!include "MUI2.nsh"
!include "LogicLib.nsh"

!define APPNAME "AgentRMC"
!define APPEXE "AgentRMCNative.exe"
; 版本号由 CMake 从 project(... VERSION x.y.z) 生成，不在安装脚本里手写第三处。
!include "${__FILEDIR__}\..\build\generated\version.nsi"
!define PUBLISHER "LIG"

!define SRC "${__FILEDIR__}\..\build\Release"
!define OUT "${__FILEDIR__}\..\..\dist\AgentRMC-Setup.exe"
!define UNKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"

Name "${APPNAME} ${APPVERSION}"
OutFile "${OUT}"
InstallDir "$LOCALAPPDATA\${APPNAME}"
InstallDirRegKey HKCU "Software\${APPNAME}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show

!define MUI_ABORTWARNING
!define MUI_ICON "${__FILEDIR__}\..\..\assets\icon.ico"
!define MUI_UNICON "${__FILEDIR__}\..\..\assets\icon.ico"
; Exec 会继承安装器当前目录，从网络/虚拟盘启动时会报「找不到网络路径」；
; 用 ExecShell 并先把工作目录设到安装目录。
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "立即启动 ${APPNAME}"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchApp

Icon "${__FILEDIR__}\..\..\assets\icon.ico"
UninstallIcon "${__FILEDIR__}\..\..\assets\icon.ico"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

Section "主程序（必需）" SEC_MAIN
  SectionIn RO
  ; 升级必须真覆盖：taskkill 返回时旧进程的 exe 句柄未必全释放（杀进程后
  ; 内核异步清理，输入法等注入 DLL 的第三方进程还可能短暂持有映像句柄），
  ; NSIS File 撞占用时静默模式吞错继续 → 装完还是旧程序（2026-11-04 两次
  ; 实测：theme 等文件更新了、exe hash 没变）。固定 Sleep 不够稳，改成
  ; 「删得掉旧 exe 才铺文件」的试探循环，最多等 10 秒。
  SetOverwrite on
  nsExec::Exec 'taskkill /IM ${APPEXE} /F'
  StrCpy $R0 0
  retry_delete:
    ClearErrors
    Delete "$INSTDIR\${APPEXE}"
    IfErrors 0 delete_done
    IntOp $R0 $R0 + 1
    IntCmp $R0 20 delete_done 0 0
    Sleep 500
    Goto retry_delete
  delete_done:
  SetOutPath "$INSTDIR"
  ; 排除调试符号、自测 exe，以及会覆盖用户设置的三个配置
  File /r /x "*.pdb" /x "AgentRMC*Test.exe" /x "config.json" \
       /x "profiles.json" /x "profiles.json.bak" "${SRC}\*.*"
  ; 配置只在没有时播种，升级安装不覆盖用户改过的东西
  ${IfNot} ${FileExists} "$INSTDIR\config.json"
    File /oname=config.json "${SRC}\config.json"
  ${EndIf}
  ${IfNot} ${FileExists} "$INSTDIR\profiles.json"
    File /oname=profiles.json "${SRC}\profiles.json"
  ${EndIf}

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  SetShellVarContext current
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\${APPEXE}"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk" "$INSTDIR\Uninstall.exe"

  WriteRegStr HKCU "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKCU "${UNKEY}" "DisplayVersion" "${APPVERSION}"
  WriteRegStr HKCU "${UNKEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKCU "${UNKEY}" "DisplayIcon" "$INSTDIR\${APPEXE}"
  WriteRegStr HKCU "${UNKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "${UNKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNKEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${UNKEY}" "EstimatedSize" 47000
SectionEnd

Section "桌面快捷方式" SEC_DESKTOP
  SetShellVarContext current
  CreateShortCut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${APPEXE}"
SectionEnd

LangString DESC_SEC_MAIN ${LANG_SIMPCHINESE} "程序本体、运行库和默认配置。"
LangString DESC_SEC_DESKTOP ${LANG_SIMPCHINESE} "在桌面放一个快捷方式。"
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_MAIN} $(DESC_SEC_MAIN)
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DESKTOP} $(DESC_SEC_DESKTOP)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function LaunchApp
  SetOutPath "$INSTDIR"
  ExecShell "open" "$INSTDIR\${APPEXE}"
FunctionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /IM ${APPEXE} /F'
  SetShellVarContext current
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\卸载 ${APPNAME}.lnk"
  RMDir "$SMPROGRAMS\${APPNAME}"
  Delete "$DESKTOP\${APPNAME}.lnk"
  DeleteRegKey HKCU "${UNKEY}"
  DeleteRegKey HKCU "Software\${APPNAME}"

  Delete "$INSTDIR\${APPEXE}"
  Delete "$INSTDIR\Uninstall.exe"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\qt.conf"
  Delete "$INSTDIR\commands.json"
  RMDir /r "$INSTDIR\platforms"
  RMDir /r "$INSTDIR\styles"
  RMDir /r "$INSTDIR\iconengines"
  RMDir /r "$INSTDIR\imageformats"
  RMDir /r "$INSTDIR\generic"
  RMDir /r "$INSTDIR\tls"
  RMDir /r "$INSTDIR\networkinformation"

  ; 程序与配置全删；模型单独问一句，静默卸载时默认保留
  MessageBox MB_YESNO|MB_ICONQUESTION \
    "是否同时删除已安装的本地语音模型（可能数百 MB）？" \
    /SD IDNO IDNO keepModels
  RMDir /r "$INSTDIR\models"
keepModels:
  RMDir /r "$INSTDIR\themes"
  RMDir /r "$INSTDIR\plugins"
  Delete "$INSTDIR\config.json"
  Delete "$INSTDIR\profiles.json"
  Delete "$INSTDIR\profiles.json.bak"
  RMDir "$INSTDIR"
SectionEnd
