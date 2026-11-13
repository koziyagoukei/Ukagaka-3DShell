# SAORI bridge. The DLL is loaded into SSP (32-bit), so the shipping build is
# x86 and links the CRT statically (no VC++ redistributable needed in SSP).
set(_saori_sources
    SaoriBridge.cpp
    SaoriProtocol.cpp
    ${CMAKE_SOURCE_DIR}/src/ipc/PathPolicy.cpp
    ${CMAKE_SOURCE_DIR}/src/ipc/PipeClient.cpp
    ${CMAKE_SOURCE_DIR}/src/ipc/Protocol.cpp
    ${CMAKE_SOURCE_DIR}/src/platform/Utf.cpp
)

add_library(ukagaka3d_saori SHARED dllmain.cpp ${_saori_sources} ukagaka3d_saori.def)
target_include_directories(ukagaka3d_saori PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(ukagaka3d_saori PRIVATE ${UKA3D_DEFINES})
target_compile_options(ukagaka3d_saori PRIVATE ${UKA3D_WARNINGS})
target_link_libraries(ukagaka3d_saori PRIVATE nlohmann_json::nlohmann_json advapi32)
set_target_properties(ukagaka3d_saori PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# Same logic as a static library, for unit tests linked into test executables.
add_library(uka3d_saori_logic STATIC ${_saori_sources})
target_include_directories(uka3d_saori_logic PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(uka3d_saori_logic PUBLIC ${UKA3D_DEFINES})
target_compile_options(uka3d_saori_logic PRIVATE ${UKA3D_WARNINGS})
target_link_libraries(uka3d_saori_logic PUBLIC nlohmann_json::nlohmann_json advapi32)
set_target_properties(ukagaka3d_saori PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)
