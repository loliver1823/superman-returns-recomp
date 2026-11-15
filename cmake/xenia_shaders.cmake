# Xenia's Direct3D 12 shaders (DXBC, Shader Model 5.1), compiled at build time.
#
# gfx/d3d12_pm4_shmem.cpp (EDRAM resolves, texture loads) and gfx/d3d12_present_chain.cpp (FXAA; the
# guest-output bilinear, AMD FidelityFX CAS and FSR 1 passes) embed Xenia's shaders as the byte-array
# headers Xenia's build generates:
#     #include "xenia/<gpu|ui>/shaders/bytecode/d3d12_5_1/<name>_<stage>.h"
# Xenia makes them with the Windows SDK's fxc.exe (xb buildshaders -> tools/build/compile_shader_dxbc.py);
# this module runs the same command, with the same arguments, on the sources vendored in
# third_party/xenia-shaders (tools/vendor_xenia_shaders.py; the list is third_party/xenia-shaders/shaders.txt),
# into <build>/generated/xenia-shaders. A build therefore needs no Xenia checkout. Checked 2026-10-13: all
# 39 headers byte-identical to those of a Xenia build of the same sources (xenia-canary 0b4e6c44, fxc
# 10.0.26100.7175 and .7705), from LF and from CRLF copies of the sources alike.
#
#   include(cmake/xenia_shaders.cmake)
#   sr_use_xenia_shaders(<target>)      # include root + generation before the target compiles
#
# SR_FXC overrides the compiler (default: the FXC_PATH environment variable, as Xenia's script, else
# the x64 fxc.exe of the newest installed Windows 10/11 SDK).
get_filename_component(_xs_repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_xs_src "${_xs_repo}/third_party/xenia-shaders")
set(SR_XENIA_SHADERS_INCLUDE "${CMAKE_BINARY_DIR}/generated/xenia-shaders")

set(SR_FXC "" CACHE FILEPATH "fxc.exe for Xenia's shaders (empty: FXC_PATH, else the newest Windows SDK's)")
set(_xs_fxc "${SR_FXC}")
if(NOT _xs_fxc AND DEFINED ENV{FXC_PATH})
  set(_xs_fxc "$ENV{FXC_PATH}")
endif()
if(NOT _xs_fxc)
  set(_xs_kits "")
  get_filename_component(_xs_reg
    "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
  if(_xs_reg AND NOT _xs_reg STREQUAL "/registry" AND EXISTS "${_xs_reg}/bin")
    list(APPEND _xs_kits "${_xs_reg}")
  endif()
  set(_xs_pf86 "$ENV{ProgramFiles\(x86\)}")
  if(_xs_pf86)
    file(TO_CMAKE_PATH "${_xs_pf86}/Windows Kits/10" _xs_def)
    list(APPEND _xs_kits "${_xs_def}")
  endif()
  set(_xs_cands "")
  foreach(_k IN LISTS _xs_kits)
    file(GLOB _c "${_k}/bin/10.*/x64/fxc.exe")
    list(APPEND _xs_cands ${_c})
  endforeach()
  if(_xs_cands)
    list(REMOVE_DUPLICATES _xs_cands)
    list(SORT _xs_cands COMPARE NATURAL ORDER DESCENDING)  # the newest SDK first
    list(GET _xs_cands 0 _xs_fxc)
  endif()
endif()
if(NOT _xs_fxc OR NOT EXISTS "${_xs_fxc}")
  message(FATAL_ERROR "fxc.exe not found (Xenia's shaders are compiled at build time). Install the Windows SDK "
                      "(Visual Studio's 'Desktop development with C++'), or pass -DSR_FXC=<path to fxc.exe>.")
endif()
message(STATUS "Xenia shaders: ${_xs_fxc}")

file(STRINGS "${_xs_src}/shaders.txt" _xs_list REGEX "^src/")
# The list decides which custom commands exist: an edit to it re-runs configure (as does a re-vendoring,
# which rewrites SOURCES.txt).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${_xs_src}/shaders.txt" "${_xs_src}/SOURCES.txt")
# Every vendored file is a dependency of every shader: a superset of each include closure (an #include can
# only reach a vendored file), and exact. CONFIGURE_DEPENDS re-checks the glob on every build, so a file
# added to the vendored tree (a new include) joins the dependencies without a manual reconfigure.
file(GLOB_RECURSE _xs_deps CONFIGURE_DEPENDS "${_xs_src}/src/*" "${_xs_src}/third_party/*")
set(SR_XENIA_SHADER_HEADERS "")
foreach(_s IN LISTS _xs_list)
  # src/xenia/gpu/shaders/resolve_full_8bpp.cs.xesl -> gpu, resolve_full_8bpp_cs, cs (Xenia's naming)
  string(REGEX MATCH "^src/xenia/([a-z]+)/shaders/([^/]+)\\.(xesl|hlsl)$" _m "${_s}")
  if(NOT _m)
    message(FATAL_ERROR "third_party/xenia-shaders/shaders.txt: unexpected entry ${_s}")
  endif()
  set(_area "${CMAKE_MATCH_1}")
  string(REPLACE "." "_" _id "${CMAKE_MATCH_2}")
  string(LENGTH "${_id}" _len)
  math(EXPR _at "${_len} - 2")
  string(SUBSTRING "${_id}" ${_at} 2 _stage)
  set(_in "${_xs_src}/${_s}")
  get_filename_component(_indir "${_in}" DIRECTORY)
  set(_outdir "${SR_XENIA_SHADERS_INCLUDE}/xenia/${_area}/shaders/bytecode/d3d12_5_1")
  set(_out "${_outdir}/${_id}.h")
  file(MAKE_DIRECTORY "${_outdir}")
  # tools/build/compile_shader_dxbc.py (Xenia), the fxc branch, argument for argument.
  add_custom_command(OUTPUT "${_out}"
    COMMAND "${_xs_fxc}" /D SHADING_LANGUAGE_HLSL_XE=1 /I "${_indir}" /Fh "${_out}" /T "${_stage}_5_1"
            /Vn "${_id}" /O3 /Qstrip_reflect /Qstrip_debug /Qstrip_priv /Gfp /nologo "${_in}"
    DEPENDS ${_xs_deps}
    COMMENT "fxc ${_id}"
    VERBATIM)
  list(APPEND SR_XENIA_SHADER_HEADERS "${_out}")
endforeach()
add_custom_target(sr_xenia_shaders DEPENDS ${SR_XENIA_SHADER_HEADERS})
# The two files that embed them: rebuild when a header is regenerated, and never compile before.
set_source_files_properties("${_xs_repo}/gfx/d3d12_pm4_shmem.cpp" "${_xs_repo}/gfx/d3d12_present_chain.cpp"
  PROPERTIES OBJECT_DEPENDS "${SR_XENIA_SHADER_HEADERS}")

function(sr_use_xenia_shaders tgt)
  add_dependencies(${tgt} sr_xenia_shaders)
  target_include_directories(${tgt} PRIVATE "${SR_XENIA_SHADERS_INCLUDE}")
endfunction()
