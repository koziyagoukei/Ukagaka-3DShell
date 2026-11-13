if(NOT UKA3D_BUILD_RUNTIME)
    include(${CMAKE_CURRENT_SOURCE_DIR}/saori_x86.cmake)
    return()
endif()

set(_assets "${CMAKE_BINARY_DIR}/test-assets")
set(_out "${CMAKE_BINARY_DIR}/test-output")
file(MAKE_DIRECTORY "${_out}")

add_executable(uka3d_tests unit_tests.cpp)
target_link_libraries(uka3d_tests PRIVATE uka3d_core)
target_link_options(uka3d_tests PRIVATE /MANIFEST:NO)
target_sources(uka3d_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/ukagaka3d.rc)

# 1) Generate synthetic assets (fixture)
add_test(NAME generate_assets COMMAND make_test_assets "${_assets}")
set_tests_properties(generate_assets PROPERTIES FIXTURES_SETUP assets)

# 2) Unit tests (CLI parsing, UTF-8 paths, PMX/VMD loading incl. error paths, image decoding)
add_test(NAME unit_tests COMMAND uka3d_tests "${_assets}")
set_tests_properties(unit_tests PROPERTIES FIXTURES_REQUIRED assets)

# 3) Executable exit-code checks
function(uka3d_cli_test name expected)
    add_test(NAME ${name}
        COMMAND ${CMAKE_COMMAND}
            -DEXE=$<TARGET_FILE:ukagaka3d>
            -DEXPECTED=${expected}
            "-DARGS=${ARGN}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_expect.cmake)
    set_tests_properties(${name} PROPERTIES FIXTURES_REQUIRED assets)
endfunction()

set(_model "${_assets}/テスト用モデル/テストキャラ.pmx")
set(_motion "${_assets}/モーション/待機テスト.vmd")
uka3d_cli_test(cli_help 0 --help)
uka3d_cli_test(cli_version 0 --version)
uka3d_cli_test(cli_no_args 1)
uka3d_cli_test(cli_unknown_option 1 --model "${_model}" --bogus)
uka3d_cli_test(cli_bad_width 1 --model "${_model}" --width abc)
uka3d_cli_test(cli_missing_model 2 --model "${_assets}/存在しない.pmx" --log "${_out}/missing.log")
uka3d_cli_test(cli_broken_model 2 --model "${_assets}/broken/壊れたモデル.pmx" --log "${_out}/broken.log")
uka3d_cli_test(cli_fake_model 2 --model "${_assets}/broken/偽物.pmx" --log "${_out}/fake.log")
uka3d_cli_test(cli_broken_motion 2 --model "${_model}" --motion "${_assets}/broken/壊れたモーション.vmd" --log "${_out}/broken_motion.log")
uka3d_cli_test(cli_missing_motion 2 --model "${_model}" --motion "${_assets}/無い.vmd" --log "${_out}/missing_motion.log")

# 4) GUI smoke tests: open a real window, render for 3 seconds, save the last frame, exit normally.
uka3d_cli_test(gui_windowed_static 0 --model "${_model}" --windowed --exit-after 3 --capture "${_out}/windowed_static.png" --log "${_out}/windowed_static.log")
uka3d_cli_test(gui_windowed_motion 0 --model "${_model}" --motion "${_motion}" --windowed --exit-after 3 --capture "${_out}/windowed_motion.png" --log "${_out}/windowed_motion.log")
uka3d_cli_test(gui_transparent_motion 0 --model "${_model}" --motion "${_motion}" --transparent --exit-after 3 --capture "${_out}/transparent_motion.png" --log "${_out}/transparent_motion.log")
set_tests_properties(gui_windowed_static gui_windowed_motion gui_transparent_motion PROPERTIES LABELS gui RUN_SERIAL TRUE)

# Phase 0.5: FPS limiter and physics
set(_physics_model "${_assets}/テスト用モデル/物理テストキャラ.pmx")
uka3d_cli_test(cli_fps_invalid 1 --model "${_model}" --fps 5)
uka3d_cli_test(cli_fps_text 1 --model "${_model}" --fps fast)
uka3d_cli_test(cli_physics_value 1 --model "${_model}" --physics=on)
uka3d_cli_test(gui_physics_30fps 0 --model "${_physics_model}" --motion "${_motion}" --fps 30 --exit-after 3 --capture "${_out}/physics_30fps.png" --log "${_out}/physics_30fps.log")
uka3d_cli_test(gui_physics_unlimited 0 --model "${_physics_model}" --motion "${_motion}" --fps unlimited --windowed --exit-after 3 --log "${_out}/physics_unlimited.log")
uka3d_cli_test(gui_no_physics_120fps 0 --model "${_physics_model}" --motion "${_motion}" --fps 120 --no-physics --exit-after 3 --log "${_out}/no_physics_120fps.log")
uka3d_cli_test(gui_physics_no_rigid_bodies 0 --model "${_model}" --physics --windowed --exit-after 2 --log "${_out}/physics_no_bodies.log")
set_tests_properties(gui_physics_30fps gui_physics_unlimited gui_no_physics_120fps gui_physics_no_rigid_bodies
    PROPERTIES LABELS gui RUN_SERIAL TRUE)

# 5) Verify the captured frames (non-empty model pixels, transparent background in transparent mode).
add_test(NAME verify_captures COMMAND uka3d_tests --verify-captures "${_out}")
set_tests_properties(verify_captures PROPERTIES DEPENDS "gui_windowed_static;gui_windowed_motion;gui_transparent_motion" LABELS gui)

# Phase 1: Satori -> YAYA converter (needs Python 3)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME satori2yaya
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/satori2yaya_test.py
                ${CMAKE_SOURCE_DIR}/tools/satori2yaya.py)
else()
    message(STATUS "ukagaka3d: Python 3 not found; satori2yaya test disabled")
endif()

# Phase 1: IPC and SAORI bridge (x64 build of the bridge logic and DLL)
add_executable(uka3d_ipc_tests ipc_tests.cpp)
target_link_libraries(uka3d_ipc_tests PRIVATE uka3d_saori_logic)
target_compile_options(uka3d_ipc_tests PRIVATE ${UKA3D_WARNINGS})
add_test(NAME ipc_saori_tests
    COMMAND uka3d_ipc_tests $<TARGET_FILE:ukagaka3d> $<TARGET_FILE:ukagaka3d_saori> "${_assets}")
set_tests_properties(ipc_saori_tests PROPERTIES FIXTURES_REQUIRED assets LABELS "gui;ipc" RUN_SERIAL TRUE)

# Seating / performance work: skinning equivalence, pose blending, seat anchor.
add_executable(uka3d_quality_tests quality_tests.cpp)
target_link_libraries(uka3d_quality_tests PRIVATE uka3d_core)
add_test(NAME quality_tests COMMAND uka3d_quality_tests "${_assets}")
set_tests_properties(quality_tests PROPERTIES FIXTURES_REQUIRED assets)
