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
    src/ui/MapWidgets.cpp
    src/ui/MapWidgets.h
    src/ui/KeymapPage.cpp
    src/ui/KeymapPage.h
    src/ui/ToggleSwitch.cpp
    src/ui/ToggleSwitch.h
    resources/app.qrc
)

target_include_directories(AgentRMCNative PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(AgentRMCNative PRIVATE Qt6::Widgets)
target_compile_options(AgentRMCNative PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_compile_definitions(AgentRMCNative PRIVATE AGENTRMC_VERSION="${PROJECT_VERSION}")

# 里程碑 5 再接本地转写；默认关闭，先保证壳和输入链可构建。
option(AGENTRMC_WITH_WHISPER "Build local whisper.cpp backend" OFF)
if(AGENTRMC_WITH_WHISPER)
    include(FetchContent)
    FetchContent_Declare(
        whisper_cpp
        GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
        GIT_TAG v1.7.4
        GIT_SHALLOW TRUE
    )
    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(whisper_cpp)
    target_link_libraries(AgentRMCNative PRIVATE whisper)
    target_compile_definitions(AgentRMCNative PRIVATE AGENTRMC_WITH_WHISPER=1)
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
