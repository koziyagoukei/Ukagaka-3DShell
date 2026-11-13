# Third-party dependencies, pinned to exact commits.
include(FetchContent)

# ---------------------------------------------------------------------------
# nlohmann/json 3.11.3 (MIT) - IPC protocol messages (runtime, SAORI, tests).
# ---------------------------------------------------------------------------
FetchContent_Declare(
    nlohmann_json
    URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

if(NOT UKA3D_BUILD_RUNTIME)
    return() # the x86 SAORI build needs nothing else
endif()

# ---------------------------------------------------------------------------
# libmmd (MIT) - PMX/VMD parsing and animation evaluation.
#   https://github.com/haturatu/libmmd  tag v0.3.2
# ---------------------------------------------------------------------------
option(UKA3D_ENABLE_BULLET "Build MMD rigid-body physics with Bullet (OFF: physics module without backend)" ON)

# The libmmd physics module is always built so mmd::MmdPhysics exists; when
# Bullet is disabled MmdPhysics::available() returns false and the app falls
# back to animation without physics.
set(LIBMMD_ENABLE_PHYSICS ON CACHE BOOL "" FORCE)
set(LIBMMD_ENABLE_BULLET ${UKA3D_ENABLE_BULLET} CACHE BOOL "" FORCE)
set(LIBMMD_FETCH_DEPENDENCIES ${UKA3D_ENABLE_BULLET} CACHE BOOL "" FORCE)
set(LIBMMD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBMMD_INSTALL OFF CACHE BOOL "" FORCE)

if(UKA3D_ENABLE_BULLET)
    # ---------------------------------------------------------------------
    # Bullet Physics 3.25 (zlib license) - the exact release libmmd v0.3.2
    # fetches. Declared here first (FetchContent: first declaration wins) to
    # pin it with a SHA-256 hash and to build it against the static CRT (/MT),
    # matching the rest of the project.
    # ---------------------------------------------------------------------
    set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)
    set(USE_DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
    set(BUILD_BULLET2_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_BULLET3 OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
    set(BUILD_CPU_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_OPENGL3_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_ENET OFF CACHE BOOL "" FORCE)
    set(BUILD_CLSOCKET OFF CACHE BOOL "" FORCE)
    set(BUILD_PYBULLET OFF CACHE BOOL "" FORCE)
    set(USE_GRAPHICAL_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(INSTALL_LIBS OFF CACHE BOOL "" FORCE)
    # Bullet 3.25 declares cmake_minimum_required(2.4.3); allow CMake 4.x.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_Declare(
        bullet
        URL      https://github.com/bulletphysics/bullet3/archive/refs/tags/3.25.tar.gz
        URL_HASH SHA256=c45afb6399e3f68036ddb641c6bf6f552bf332d5ab6be62f7e6c54eda05ceb77
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
endif()

FetchContent_Declare(
    libmmd
    GIT_REPOSITORY https://github.com/haturatu/libmmd.git
    GIT_TAG        f478f985ca506fa9d265b6ba8f2359009b087548 # v0.3.2
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(libmmd)

# libmmd sources contain UTF-8 Japanese string literals (bone names) without a
# BOM. On Japanese Windows MSVC would otherwise read them as CP932.
foreach(_mmd_target mmd_core mmd_animation mmd_physics)
    if(TARGET ${_mmd_target})
        target_compile_options(${_mmd_target} PRIVATE /utf-8)
    endif()
endforeach()
if(UKA3D_ENABLE_BULLET AND NOT TARGET BulletDynamics)
    message(STATUS "ukagaka3d: using a system Bullet found by libmmd")
endif()
foreach(_bullet_target BulletDynamics BulletCollision LinearMath)
    if(TARGET ${_bullet_target})
        target_compile_options(${_bullet_target} PRIVATE /W0)
    endif()
endforeach()

# ---------------------------------------------------------------------------
# stb_image (public domain / MIT) - PNG/JPG/BMP/TGA/GIF/PSD decoding.
#   https://github.com/nothings/stb
# ---------------------------------------------------------------------------
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        4e9b368bfc071e97206ea5c6adba68dd0cbf8897
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(stb)

add_library(uka3d_stb STATIC ${CMAKE_CURRENT_LIST_DIR}/../src/renderer/StbImageImpl.cpp)
target_include_directories(uka3d_stb SYSTEM PUBLIC ${stb_SOURCE_DIR})
target_compile_options(uka3d_stb PRIVATE /W0)
