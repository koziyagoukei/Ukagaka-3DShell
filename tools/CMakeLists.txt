# Synthetic test-asset generator (outputs are generated, never third-party data).
add_executable(make_test_assets make_test_assets.cpp)
target_link_libraries(make_test_assets PRIVATE mmd::core uka3d_stb)
target_compile_definitions(make_test_assets PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(make_test_assets PRIVATE /utf-8 /W3)

set(UKA3D_TEST_ASSET_DIR "${CMAKE_BINARY_DIR}/test-assets" CACHE INTERNAL "")
add_custom_target(test_assets
    COMMAND make_test_assets "${UKA3D_TEST_ASSET_DIR}"
    DEPENDS make_test_assets
    COMMENT "Generating synthetic PMX/VMD test assets into ${UKA3D_TEST_ASSET_DIR}"
    VERBATIM
)

# Physics-oriented PMX inspector (development aid; prints only).
add_executable(pmx_info pmx_info.cpp)
target_link_libraries(pmx_info PRIVATE mmd::core)
target_compile_definitions(pmx_info PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(pmx_info PRIVATE /utf-8 /W3)

# Headless per-tick CPU profile (animation / physics / skinning breakdown).
add_executable(uka3d_bench bench_eval.cpp)
target_link_libraries(uka3d_bench PRIVATE uka3d_core)
# Warning flags (/W4, /utf-8) come from uka3d_core.

# Motion generator: idle / walk / sit / grab VMDs fitted to a PMX (original data).
add_executable(uka3d_motiongen motion_gen.cpp)
target_link_libraries(uka3d_motiongen PRIVATE uka3d_core)
# Warning flags (/W4, /utf-8) come from uka3d_core.

# Texture downsizer for the distributed sample model (stb_image_resize2 / stb_image_write).
add_executable(uka3d_texresize texture_resize.cpp)
target_include_directories(uka3d_texresize PRIVATE "${stb_SOURCE_DIR}/deprecated")
target_link_libraries(uka3d_texresize PRIVATE uka3d_stb)
target_compile_definitions(uka3d_texresize PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(uka3d_texresize PRIVATE /utf-8 /W3)
