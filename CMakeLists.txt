cmake_minimum_required(VERSION 3.20)
project(BedrockLowLatency VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

option(BLL_SANITIZE "Build the portable tests with ASan and UBSan" OFF)
option(BLL_WERROR "Treat warnings as errors" OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(BLL_WERROR)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    if(BLL_WERROR)
        add_compile_options(-Werror)
    endif()
    if(WIN32)
        # %zu / %llu in printf-style calls need the C99 stdio of MinGW on msvcrt.
        add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN __USE_MINGW_ANSI_STDIO=1)
    endif()
endif()

if(BLL_SANITIZE AND NOT WIN32)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# MinGW: link the runtime statically so that neither the DLL (loaded inside the
# game's AppContainer) nor the tools depend on MinGW runtime DLLs.
if(MINGW)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

find_package(Threads REQUIRED)

# Platform-independent logic: everything that can be unit-tested without Windows.
add_library(bll_core STATIC
    src/core/config.cpp
    src/core/log.cpp
    src/core/time.cpp
    src/core/version.cpp
    src/input/transition_latch.cpp
    src/metrics/metrics.cpp
    src/platform/pe_image.cpp
    src/timing/adaptive_scheduler.cpp
    src/timing/bottleneck.cpp
    src/timing/frame_governor.cpp
    src/timing/frame_pacer.cpp
    src/timing/precise_sleeper.cpp
    src/timing/refresh_estimator.cpp)
target_include_directories(bll_core PUBLIC src)
target_link_libraries(bll_core PUBLIC Threads::Threads)
set_target_properties(bll_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(bll_tests
    tests/main.cpp
    tests/core_tests.cpp
    tests/governor_tests.cpp
    tests/input_tests.cpp
    tests/pe_tests.cpp
    tests/timing_tests.cpp)
target_link_libraries(bll_tests PRIVATE bll_core)

add_executable(bll-probe tools/probe/main.cpp)
target_link_libraries(bll-probe PRIVATE bll_core)

enable_testing()
add_test(NAME unit COMMAND bll_tests)

if(WIN32)

    add_library(bll SHARED
        src/dllmain.cpp
        src/graphics/dxgi_hooks.cpp
        src/graphics/gpu_sync.cpp
        src/input/gamepad_hooks.cpp
        src/platform/hook.cpp
        src/platform/package.cpp
        src/platform/abi.c
        src/diagnostics/crash_log.cpp)
    target_link_libraries(bll PRIVATE bll_core dxgi d3d11 dxguid runtimeobject ole32 user32)
    set_target_properties(bll PROPERTIES PREFIX "" OUTPUT_NAME "bll")

    add_executable(bll-launcher tools/launcher/main.cpp src/platform/package.cpp)
    target_link_libraries(bll-launcher PRIVATE bll_core ole32 advapi32 shell32 userenv)
    target_compile_definitions(bll-launcher PRIVATE UNICODE _UNICODE)

    # Windowed Direct3D 11 test application used for integration testing of
    # the DLL without the game. It is built into its own directory together
    # with a scripted xinput1_4.dll so that the fake controller is only ever
    # picked up by the harness.
    add_executable(bll-harness tools/harness/main.cpp)
    target_link_libraries(bll-harness PRIVATE bll_core dxgi d3d11 dxguid xinput1_4 user32 shell32)
    add_library(fake_xinput SHARED tools/harness/fake_xinput.c)
    set_target_properties(fake_xinput PROPERTIES PREFIX "" OUTPUT_NAME "xinput1_4")
    set_target_properties(bll-harness fake_xinput PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/harness"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/harness"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/harness/lib")
endif()
