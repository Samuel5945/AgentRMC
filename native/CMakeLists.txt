cmake_minimum_required(VERSION 3.24)

project(AgentRMCNative VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

qt_add_executable(AgentRMCNative
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/core/Profiles.cpp
    src/core/Profiles.h
    src/core/Presets.cpp
    src/core/Presets.h
    src/core/Injector.cpp
    src/core/Injector.h
    src/core/KeyRouter.cpp
    src/core/KeyRouter.h
    src/core/InputEngine.cpp
    src/core/InputEngine.h
    src/core/NativeHooks.cpp
    src/core/NativeHooks.h
    src/core/VoiceSettings.cpp
    src/core/VoiceSettings.h
    src/core/AudioRecorder.cpp
    src/core/AudioRecorder.h
    src/core/AsrEngine.cpp
    src/core/AsrEngine.h
    src/core/VoiceController.cpp
    src/core/VoiceController.h
    src/ui/MapWidgets.cpp
    src/ui/MapWidgets.h
    src/ui/KeymapPage.cpp
    src/ui/KeymapPage.h
    src/ui/ToggleSwitch.cpp
    src/ui/ToggleSwitch.h
    src/ui/VoiceSettingsDialog.cpp
    src/ui/VoiceSettingsDialog.h
    resources/app.qrc
)

target_include_directories(AgentRMCNative PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCNative PRIVATE Qt6::Widgets winmm)
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

add_executable(AgentRMCStoreTest
    tests/m2_store_test.cpp
    src/core/Profiles.cpp
    src/core/Profiles.h
    src/core/Presets.cpp
    src/core/Presets.h
)
target_include_directories(AgentRMCStoreTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCStoreTest PRIVATE Qt6::Core)
target_compile_options(AgentRMCStoreTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCRouterTest
    tests/m3_router_test.cpp
    src/core/KeyRouter.cpp
    src/core/KeyRouter.h
    src/core/Presets.cpp
    src/core/Presets.h
    src/core/Injector.cpp
    src/core/Injector.h
)
target_include_directories(AgentRMCRouterTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCRouterTest PRIVATE Qt6::Core)
target_compile_options(AgentRMCRouterTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCTakeoverTest
    tests/m4_takeover_test.cpp
    src/core/KeyRouter.cpp
    src/core/KeyRouter.h
    src/core/Presets.cpp
    src/core/Presets.h
    src/core/Injector.cpp
    src/core/Injector.h
    src/core/NativeHooks.cpp
    src/core/NativeHooks.h
)
target_include_directories(AgentRMCTakeoverTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCTakeoverTest PRIVATE Qt6::Core)
target_compile_options(AgentRMCTakeoverTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCVoiceTest
    tests/m5_voice_test.cpp
    src/core/VoiceSettings.cpp
    src/core/VoiceSettings.h
)
target_include_directories(AgentRMCVoiceTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCVoiceTest PRIVATE Qt6::Core)
target_compile_options(AgentRMCVoiceTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_executable(AgentRMCAudioTest
    tests/m5_audio_cli.cpp
    src/core/AudioRecorder.cpp
    src/core/AudioRecorder.h
    src/core/VoiceSettings.cpp
    src/core/VoiceSettings.h
)
target_include_directories(AgentRMCAudioTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCAudioTest PRIVATE Qt6::Core winmm)
target_compile_options(AgentRMCAudioTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

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
