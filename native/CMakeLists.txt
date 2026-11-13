cmake_minimum_required(VERSION 3.24)

project(AgentRMCNative VERSION 0.4.1 LANGUAGES CXX)

# 版本唯一入口就是上面的 PROJECT_VERSION：app.rc 与 NSIS 的版本号都由这里生成，
# 不再手写第二、第三处。改了版本只需重新 configure（build.bat 每次都会）。
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(APP_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico")
configure_file(resources/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc" @ONLY)
configure_file(resources/version.nsi.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.nsi" @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network Sql)
qt_standard_project_setup()

qt_add_executable(AgentRMCNative
    src/main.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc"
    src/MainWindow.cpp
    src/MainWindow.h
    src/core/AgentBridge.cpp
    src/core/AgentBridge.h
    src/core/LanWebServer.cpp
    src/core/LanWebServer.h
    src/core/ChatText.cpp
    src/core/ChatText.h
    src/core/WorkItems.cpp
    src/core/WorkItems.h
    src/core/SessionDispatch.cpp
    src/core/SessionDispatch.h
    src/core/SessionTranscript.cpp
    src/core/SessionTranscript.h
    src/core/UiDispatch.cpp
    src/core/UiDispatch.h
    src/core/ZcodeDispatch.cpp
    src/core/ZcodeDispatch.h
    src/core/AgentWindow.cpp
    src/core/AgentWindow.h
    src/core/HostNewTask.cpp
    src/core/HostNewTask.h
    src/core/HostRuntime.cpp
    src/core/HostRuntime.h
    src/core/Profiles.cpp
    src/core/Profiles.h
    src/core/Presets.cpp
    src/core/Presets.h
    src/core/AppLauncher.cpp
    src/core/PinyinInitials.cpp
    src/core/AppLauncher.h
    src/core/Injector.cpp
    src/core/Injector.h
    src/core/SystemCommand.cpp
    src/core/SystemCommand.h
    src/core/SystemConfirm.cpp
    src/core/SystemConfirm.h
    src/core/SystemControl.cpp
    src/core/SystemControl.h
    src/core/KeyRouter.cpp
    src/core/KeyRouter.h
    src/core/InputEngine.cpp
    src/core/InputEngine.h
    src/core/NativeHooks.cpp
    src/core/NativeHooks.h
    src/core/VoiceSettings.cpp
    src/core/VoiceSettings.h
    src/core/ModelImport.cpp
    src/core/ModelImport.h
    src/core/Updater.cpp
    src/core/Updater.h
    src/core/PluginUpgrade.cpp
    src/core/PluginUpgrade.h
    src/core/AudioRecorder.cpp
    src/core/AudioRecorder.h
    src/core/AudioOut.cpp
    src/core/AudioOut.h
    src/core/AsrEngine.cpp
    src/core/AsrEngine.h
    src/core/AsrClient.cpp
    src/core/AsrClient.h
    src/core/LlmClient.cpp
    src/core/LlmClient.h
    src/core/PinyinInitials.cpp
    src/core/PinyinInitials.h
    src/core/VoiceController.cpp
    src/core/VoiceController.h
    src/core/TtsClient.cpp
    src/core/TtsClient.h
    src/core/SpeechText.cpp
    src/core/SpeechText.h
    src/core/TaskCue.cpp
    src/core/TaskCue.h
    src/ui/WorkWebView.cpp
    src/ui/WorkWebView.h
    src/core/ThemeManager.cpp
    src/core/ThemeManager.h
    src/ui/AppPage.cpp
    src/ui/AppPage.h
    src/ui/AsrSettingsDialog.cpp
    src/ui/AsrSettingsDialog.h
    src/ui/LlmSettingsDialog.cpp
    src/ui/LlmSettingsDialog.h
    src/ui/MapWidgets.cpp
    src/ui/MapWidgets.h
    src/ui/KeymapPage.cpp
    src/ui/KeymapPage.h
    src/ui/ToggleSwitch.cpp
    src/ui/ToggleSwitch.h
    src/ui/RemotePanel.cpp
    src/ui/RemotePanel.h
    src/ui/KeyIcon.cpp
    src/ui/KeyIcon.h
    src/ui/Toast.cpp
    src/ui/Toast.h
    src/ui/TtsSettingsDialog.cpp
    src/ui/TtsSettingsDialog.h
    src/ui/ThemeDialog.cpp
    src/ui/ThemeDialog.h
    src/ui/ThemeSlider.cpp
    src/ui/ThemeSlider.h
    src/ui/PopupShape.cpp
    src/ui/PopupShape.h
    src/ui/GuideDialog.cpp
    src/ui/GuideDialog.h
    src/ui/AgentBridgeDialog.cpp
    src/ui/AgentBridgeDialog.h
    src/ui/VoiceSettingsDialog.cpp
    src/ui/VoiceSettingsDialog.h
    src/ui/VoicePet.cpp
    src/ui/VoicePet.h
    src/ui/PetSprite.cpp
    src/ui/PetSprite.h
    src/ui/VoiceWindow.cpp
    src/ui/VoiceWindow.h
    src/ui/WaveformButton.cpp
    src/ui/WaveformButton.h
    src/ui/DispatchPicker.cpp
    src/ui/DispatchPicker.h
    resources/app.qrc
)

set_target_properties(AgentRMCNative PROPERTIES WIN32_EXECUTABLE TRUE)
target_include_directories(AgentRMCNative PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# 原型用：WebView2 的 COM 接口头（从 nuget 解到本地 .webview2-sdk/，不进仓库）。
# loader 是运行时 LoadLibrary 取的，不链接 .lib，所以 Release 目录里不会多出 DLL。
target_include_directories(AgentRMCNative PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/.webview2-sdk/build/native/include)
# powrprof = 睡眠/休眠，wbemuuid + oleaut32 = 笔记本背光的 WMI 回退。
set(AGENTRMC_SYSTEM_LIBS powrprof wbemuuid oleaut32)
target_link_libraries(AgentRMCNative PRIVATE Qt6::Widgets Qt6::Network Qt6::Sql winmm
    shell32 ole32 UIAutomationCore.lib ${AGENTRMC_SYSTEM_LIBS})
target_compile_options(AgentRMCNative PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(AgentRMCNative PRIVATE AGENTRMC_VERSION="${PROJECT_VERSION}")

# 本地转写：sherpa-onnx 预编译库放在 third_party/sherpa-onnx；没下载时仍可构建，
# 语音页会提示“当前构建未包含运行库”。
set(AGENTRMC_SHERPA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sherpa-onnx"
    CACHE PATH "sherpa-onnx prebuilt root")
option(AGENTRMC_ENABLE_SHERPA "Build the local sherpa-onnx ASR backend" ON)
if(AGENTRMC_ENABLE_SHERPA
   AND EXISTS "${AGENTRMC_SHERPA_ROOT}/include/sherpa-onnx/c-api/c-api.h"
   AND EXISTS "${AGENTRMC_SHERPA_ROOT}/lib/sherpa-onnx-c-api.lib")
    set(AGENTRMC_WITH_SHERPA ON)
    target_include_directories(AgentRMCNative PRIVATE
        "${AGENTRMC_SHERPA_ROOT}/include")
    target_link_libraries(AgentRMCNative PRIVATE
        "${AGENTRMC_SHERPA_ROOT}/lib/sherpa-onnx-c-api.lib")
    target_compile_definitions(AgentRMCNative PRIVATE AGENTRMC_WITH_SHERPA=1)
    add_custom_command(TARGET AgentRMCNative POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${AGENTRMC_SHERPA_ROOT}/lib/sherpa-onnx-c-api.dll"
            "${AGENTRMC_SHERPA_ROOT}/lib/onnxruntime.dll"
            "${AGENTRMC_SHERPA_ROOT}/lib/onnxruntime_providers_shared.dll"
            "$<TARGET_FILE_DIR:AgentRMCNative>")
else()
    set(AGENTRMC_WITH_SHERPA OFF)
endif()

# WinInjector::brightness 转调 SystemControl，所以凡是编 Injector.cpp 的 target
# 都要带上这条链（SystemControl → Profiles → Presets → AppLauncher）。
set(AGENTRMC_SYSTEM_SOURCES
    src/core/SystemCommand.cpp
    src/core/SystemCommand.h
    src/core/SystemConfirm.cpp
    src/core/SystemConfirm.h
    src/core/SystemControl.cpp
    src/core/SystemControl.h
    src/core/Profiles.cpp
    src/core/Profiles.h
    src/core/Presets.cpp
    src/core/Presets.h
    src/core/AppLauncher.cpp
    src/core/AppLauncher.h
    src/core/PinyinInitials.cpp
    src/core/PinyinInitials.h
)

add_executable(AgentRMCStoreTest
    tests/m2_store_test.cpp
    src/core/Profiles.cpp
    src/core/Profiles.h
    src/core/AppLauncher.cpp
    src/core/PinyinInitials.cpp
    src/core/AppLauncher.h
    src/core/Presets.cpp
    src/core/Presets.h
)
target_include_directories(AgentRMCStoreTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCStoreTest PRIVATE Qt6::Core shell32)
target_compile_options(AgentRMCStoreTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCRouterTest
    tests/m3_router_test.cpp
    src/core/KeyRouter.cpp
    src/core/KeyRouter.h
    src/core/AppLauncher.cpp
    src/core/PinyinInitials.cpp
    src/core/AppLauncher.h
    src/core/Presets.cpp
    src/core/Presets.h
    src/core/Injector.cpp
    src/core/Injector.h
    ${AGENTRMC_SYSTEM_SOURCES}
)
target_include_directories(AgentRMCRouterTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCRouterTest PRIVATE Qt6::Core shell32 ole32
    ${AGENTRMC_SYSTEM_LIBS})
target_compile_options(AgentRMCRouterTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCTakeoverTest
    tests/m4_takeover_test.cpp
    src/core/KeyRouter.cpp
    src/core/KeyRouter.h
    src/core/AppLauncher.cpp
    src/core/PinyinInitials.cpp
    src/core/AppLauncher.h
    src/core/Presets.cpp
    src/core/Presets.h
    src/core/Injector.cpp
    src/core/Injector.h
    src/core/NativeHooks.cpp
    src/core/NativeHooks.h
    ${AGENTRMC_SYSTEM_SOURCES}
)
target_include_directories(AgentRMCTakeoverTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCTakeoverTest PRIVATE Qt6::Core shell32 ole32
    ${AGENTRMC_SYSTEM_LIBS})
target_compile_options(AgentRMCTakeoverTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCAppTest
    tests/app_launcher_test.cpp
    src/core/AppLauncher.cpp
    src/core/AppLauncher.h
    src/core/PinyinInitials.cpp
    src/core/PinyinInitials.h
    src/core/Presets.cpp
    src/core/Presets.h
    src/core/Profiles.cpp
    src/core/Profiles.h
)
target_include_directories(AgentRMCAppTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCAppTest PRIVATE Qt6::Core shell32)
target_compile_options(AgentRMCAppTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCVoiceTest
    tests/m5_voice_test.cpp
    src/core/VoiceSettings.cpp
    src/core/VoiceSettings.h
)
target_include_directories(AgentRMCVoiceTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCVoiceTest PRIVATE Qt6::Core)
target_compile_options(AgentRMCVoiceTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# 按名激活「新建任务」那条链：这里只测能离线判定的部分（哪条路归谁管、
# 失败分支有没有带判定输入）。真活体的 SetFocus+回车稳定性用 rc_probe 实测。
add_executable(AgentRMCHostNewTaskTest
    tests/host_new_task_test.cpp
    src/core/HostNewTask.cpp
    src/core/HostNewTask.h
    src/core/AgentWindow.cpp
    src/core/AgentWindow.h
)
target_include_directories(AgentRMCHostNewTaskTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCHostNewTaskTest PRIVATE Qt6::Core ole32 shell32
    UIAutomationCore.lib)
target_compile_options(AgentRMCHostNewTaskTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCThemeTest
    tests/theme_test.cpp
    src/core/ThemeManager.cpp
    src/core/ThemeManager.h
    src/core/Profiles.cpp
    src/core/Profiles.h
    src/core/Presets.cpp
    src/core/Presets.h
    src/core/AppLauncher.cpp
    src/core/AppLauncher.h
    src/core/PinyinInitials.cpp
    src/core/PinyinInitials.h
)
target_include_directories(AgentRMCThemeTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCThemeTest PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
target_compile_options(AgentRMCThemeTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCBridgeTest
    tests/bridge_test.cpp
    src/core/AgentBridge.cpp
    src/core/AgentBridge.h
)
target_include_directories(AgentRMCBridgeTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCBridgeTest PRIVATE Qt6::Core Qt6::Network)
target_compile_options(AgentRMCBridgeTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# 局域网手机遥控口（移动端 M1）：配对/鉴权/白名单/越界/吊销/防爆破，
# 外加「桥必须回环」的锁死断言。全走临时端口，不碰真实 45915/45916。
add_executable(AgentRMCLanWebTest
    tests/lanweb_test.cpp
    src/core/LanWebServer.cpp
    src/core/LanWebServer.h
    src/core/AgentBridge.cpp
    src/core/AgentBridge.h
)
target_include_directories(AgentRMCLanWebTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCLanWebTest PRIVATE Qt6::Core Qt6::Network)
target_compile_options(AgentRMCLanWebTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# 聊天页记录读取器：jsonl 两记法 / 分页游标 / SQLite 两表 / 缓存失效。
# 全部用临时库，不碰真实宿主会话库。
add_executable(AgentRMCTranscriptTest
    tests/transcript_test.cpp
    src/core/SessionTranscript.cpp
    src/core/SessionTranscript.h
    src/core/ZcodeDispatch.cpp
    src/core/ZcodeDispatch.h
)
target_include_directories(AgentRMCTranscriptTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCTranscriptTest PRIVATE Qt6::Core Qt6::Sql shell32 ole32
    user32 UIAutomationCore.lib)
target_compile_options(AgentRMCTranscriptTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCPluginTest
    tests/plugin_upgrade_test.cpp
    src/core/PluginUpgrade.cpp
    src/core/PluginUpgrade.h
    src/core/Updater.cpp
    src/core/Updater.h
)
target_include_directories(AgentRMCPluginTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCPluginTest PRIVATE Qt6::Core)
target_compile_options(AgentRMCPluginTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCWorkItemTest
    tests/workitem_test.cpp
    src/core/WorkItems.cpp
    src/core/WorkItems.h
)
target_include_directories(AgentRMCWorkItemTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCWorkItemTest PRIVATE Qt6::Core)
target_compile_options(AgentRMCWorkItemTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCDispatchTest
    tests/dispatch_test.cpp
    src/core/SessionDispatch.cpp
    src/core/SessionDispatch.h
    src/core/UiDispatch.cpp
    src/core/UiDispatch.h
    src/core/ZcodeDispatch.cpp
    src/core/ZcodeDispatch.h
    src/core/AgentWindow.cpp
    src/core/AgentWindow.h
)
target_include_directories(AgentRMCDispatchTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCDispatchTest PRIVATE Qt6::Core Qt6::Sql shell32 ole32
    user32 UIAutomationCore.lib)
target_compile_options(AgentRMCDispatchTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCChatTextTest
    tests/chat_text_test.cpp
    src/core/ChatText.cpp
    src/core/ChatText.h
)
target_include_directories(AgentRMCChatTextTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCChatTextTest PRIVATE Qt6::Core)
target_compile_options(AgentRMCChatTextTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCSpeechTest
    tests/speech_text_test.cpp
    src/core/SpeechText.cpp
    src/core/SpeechText.h
    src/core/ChatText.cpp
    src/core/ChatText.h
)
target_include_directories(AgentRMCSpeechTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCSpeechTest PRIVATE Qt6::Core)
target_compile_options(AgentRMCSpeechTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# 对话模式「这句该谁接」的判据：纯本地词表，离线逐句可测。
add_executable(AgentRMCTaskCueTest
    tests/task_cue_test.cpp
    src/core/TaskCue.cpp
    src/core/TaskCue.h
)
target_include_directories(AgentRMCTaskCueTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCTaskCueTest PRIVATE Qt6::Core)
target_compile_options(AgentRMCTaskCueTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCAudioTest
    tests/m5_audio_cli.cpp
    src/core/AudioRecorder.cpp
    src/core/AudioRecorder.h
    src/core/VoiceSettings.cpp
    src/core/VoiceSettings.h
)
target_include_directories(AgentRMCAudioTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCAudioTest PRIVATE Qt6::Core winmm ole32)
target_compile_options(AgentRMCAudioTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# 语音系统控制：解析词表、二次确认状态机、执行器闸门。全部离线判定，
# 不碰真实音量/亮度/电源——真机那部分用 rc_probe 手动验。
add_executable(AgentRMCSystemTest
    tests/system_command_test.cpp
    src/core/Injector.cpp
    src/core/Injector.h
    ${AGENTRMC_SYSTEM_SOURCES}
)
target_include_directories(AgentRMCSystemTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCSystemTest PRIVATE Qt6::Core shell32 ole32 user32
    ${AGENTRMC_SYSTEM_LIBS})
target_compile_options(AgentRMCSystemTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

if(AGENTRMC_WITH_SHERPA)
    add_executable(AgentRMCAsrTest
        tests/m5_asr_cli.cpp
        src/core/AsrEngine.cpp
        src/core/AsrEngine.h
        src/core/VoiceSettings.cpp
        src/core/VoiceSettings.h
    )
    target_include_directories(AgentRMCAsrTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        "${AGENTRMC_SHERPA_ROOT}/include")
    target_link_libraries(AgentRMCAsrTest PRIVATE Qt6::Core
        "${AGENTRMC_SHERPA_ROOT}/lib/sherpa-onnx-c-api.lib")
    target_compile_definitions(AgentRMCAsrTest PRIVATE AGENTRMC_WITH_SHERPA=1)
    target_compile_options(AgentRMCAsrTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    add_custom_command(TARGET AgentRMCAsrTest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${AGENTRMC_SHERPA_ROOT}/lib/sherpa-onnx-c-api.dll"
            "${AGENTRMC_SHERPA_ROOT}/lib/onnxruntime.dll"
            "${AGENTRMC_SHERPA_ROOT}/lib/onnxruntime_providers_shared.dll"
            "$<TARGET_FILE_DIR:AgentRMCAsrTest>")
endif()
