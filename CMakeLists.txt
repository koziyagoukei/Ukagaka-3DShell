cmake_minimum_required(VERSION 3.25)

project(ukagaka3d VERSION 0.1.1 LANGUAGES CXX RC)

# Version shown by the runtime (--version) and the SAORI bridge (GET Version).
set(UKA3D_VERSION_LABEL "${PROJECT_VERSION}-beta")
add_compile_definitions(UKA3D_VERSION="${UKA3D_VERSION_LABEL}")

if(NOT WIN32)
    message(FATAL_ERROR "ukagaka3d targets Windows 10/11 only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(UKA3D_ARCH x64)
else()
    set(UKA3D_ARCH x86)
endif()

# The 3D runtime is x64 only. SSP (and therefore every SAORI it loads) is a
# 32-bit process, so the SAORI bridge is built for x86 (and for x64 for tests).
if(UKA3D_ARCH STREQUAL "x64")
    option(UKA3D_BUILD_RUNTIME "Build the x64 3D runtime (ukagaka3d.exe)" ON)
else()
    set(UKA3D_BUILD_RUNTIME OFF CACHE BOOL "Build the x64 3D runtime (ukagaka3d.exe)" FORCE)
endif()
option(UKA3D_BUILD_SAORI "Build the SAORI bridge DLL (ukagaka3d_saori.dll)" ON)
option(UKA3D_BUILD_TESTS "Build ukagaka3d tests and test-asset generator" ON)

# Link the C/C++ runtime statically everywhere (dependencies included), so the
# runtime and the SAORI start on PCs without the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(cmake/Dependencies.cmake)

set(UKA3D_WARNINGS /utf-8 /W4 /permissive- /Zc:__cplusplus)
set(UKA3D_DEFINES UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# ---------------------------------------------------------------------------
# IPC library (named pipe + JSON protocol), shared by the runtime, the SAORI
# bridge and the tests. Builds for x86 and x64.
# ---------------------------------------------------------------------------
add_library(uka3d_ipc STATIC
    src/ipc/PathPolicy.cpp
    src/ipc/PipeClient.cpp
    src/ipc/PipeServer.cpp
    src/ipc/Protocol.cpp
    src/platform/Utf.cpp
)
target_include_directories(uka3d_ipc PUBLIC src)
target_compile_definitions(uka3d_ipc PUBLIC ${UKA3D_DEFINES})
target_compile_options(uka3d_ipc PUBLIC ${UKA3D_WARNINGS})
target_link_libraries(uka3d_ipc PUBLIC nlohmann_json::nlohmann_json advapi32)

if(UKA3D_BUILD_RUNTIME)
    include(cmake/Shaders.cmake)

    # Core library: everything except the entry point, so tests can link it.
    add_library(uka3d_core STATIC
        src/app/App.cpp
        src/app/CameraTrack.cpp
        src/app/Face.cpp
        src/app/CommandLine.cpp
        src/app/DesktopBehavior.cpp
        src/app/Interaction.cpp
        src/app/Playback.cpp
        src/app/SeatAnchor.cpp
        src/mmd/MmdModel.cpp
        src/mmd/Skinning.cpp
        src/motion/MotionGenerator.cpp
        src/platform/FramePacer.cpp
        src/platform/Log.cpp
        src/platform/SstpEvents.cpp
        src/platform/TransparentWindow.cpp
        src/renderer/Camera.cpp
        src/renderer/D3DContext.cpp
        src/renderer/ModelRenderer.cpp
        src/renderer/TextureLoader.cpp
    )
    target_include_directories(uka3d_core PUBLIC src ${UKA3D_SHADER_INCLUDE_DIR})
    target_compile_definitions(uka3d_core PUBLIC ${UKA3D_DEFINES})
    target_compile_options(uka3d_core PUBLIC ${UKA3D_WARNINGS})
    target_link_libraries(uka3d_core
        PUBLIC mmd::core mmd::animation mmd::physics uka3d_ipc
        PRIVATE uka3d_stb d3d11 dxgi dcomp windowscodecs dwmapi winmm
    )
    add_dependencies(uka3d_core uka3d_shaders)

    add_executable(ukagaka3d src/main.cpp src/ukagaka3d.rc)
    target_link_libraries(ukagaka3d PRIVATE uka3d_core)
    set_target_properties(ukagaka3d PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    # The manifest is embedded via the .rc file; disable the linker default one.
    target_link_options(ukagaka3d PRIVATE /MANIFEST:NO)
endif()

if(UKA3D_BUILD_SAORI)
    add_subdirectory(src/saori)
endif()

if(UKA3D_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    if(UKA3D_BUILD_RUNTIME)
        add_subdirectory(tools)
    endif()
endif()
