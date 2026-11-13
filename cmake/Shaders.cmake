# Compile HLSL shaders at build time with fxc.exe into C++ headers.

set(_sdk_bin_hints "")
if(DEFINED ENV{WindowsSdkVerBinPath})
    list(APPEND _sdk_bin_hints "$ENV{WindowsSdkVerBinPath}/x64")
endif()
if(CMAKE_WINDOWS_KITS_10_DIR AND CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    list(APPEND _sdk_bin_hints
        "${CMAKE_WINDOWS_KITS_10_DIR}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
endif()
file(GLOB _sdk_bins "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
list(SORT _sdk_bins ORDER DESCENDING)
list(APPEND _sdk_bin_hints ${_sdk_bins})

find_program(UKA3D_FXC fxc HINTS ${_sdk_bin_hints} REQUIRED)
message(STATUS "ukagaka3d: fxc = ${UKA3D_FXC}")

set(UKA3D_SHADER_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")
set(_shader_out_dir "${UKA3D_SHADER_INCLUDE_DIR}/shaders")
file(MAKE_DIRECTORY "${_shader_out_dir}")

set(_shader_src "${CMAKE_SOURCE_DIR}/shaders/model.hlsl")
# entry;profile pairs
set(_shader_entries
    "VSMain\;vs_5_0"
    "PSMain\;ps_5_0"
    "VSEdge\;vs_5_0"
    "PSEdge\;ps_5_0"
)

set(_shader_outputs "")
foreach(_pair IN LISTS _shader_entries)
    list(GET _pair 0 _entry)
    list(GET _pair 1 _profile)
    set(_out "${_shader_out_dir}/model_${_entry}.h")
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND "${UKA3D_FXC}" /nologo /T ${_profile} /E ${_entry} /O3 /Zpr
                /Vn g_${_entry} /Fh "${_out}" "${_shader_src}"
        DEPENDS "${_shader_src}"
        COMMENT "fxc ${_entry} (${_profile})"
        VERBATIM
    )
    list(APPEND _shader_outputs "${_out}")
endforeach()

add_custom_target(uka3d_shaders DEPENDS ${_shader_outputs})
