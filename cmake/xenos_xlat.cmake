# iter-463 runtime shader translation (gfx/shader_runtime_xlat.cpp, SR_SHADER_RUNTIME_XLAT).
#
# Builds XenosRecomp's recompiler INTO the game as the static library `xenos_xlat`. Its sources are upstream
# hedge-dev/XenosRecomp at the commit the third_party/XenosRecomp submodule pins (990d03b) plus the
# tools/xenosrecomp/*.patch series, which tools/xenosrecomp/prepare.py applies at configure time to an export
# of the pinned commit in <build>/xenosrecomp (the submodule itself stays pristine). The result is byte for
# byte the clone the offline shader cache was built with. The headers it includes (fmt, smol-v, zstd, xxHash)
# come from the submodule's own pinned submodules; DXC is the official release (cmake/dxc_release.cmake).
# The recompiler is compiled UNCHANGED, with its own pch.h force-included, C++17 and the offline tool's
# definitions, so the in-game translation is the offline one (tools/difftest/shader_xlat_test_run.py proves
# the outputs byte-identical).
#
#   include(cmake/xenos_xlat.cmake)       # after the SupermanReturns target exists
#   sr_link_xenos_xlat(SupermanReturns)   # links the library, defines SR_HAVE_XENOS_XLAT=1,
#                                         # copies dxcompiler.dll + dxil.dll + licences next to it
#
# SR_XENOSRECOMP_DIR overrides the sources with a ready patched clone (tools/xenosrecomp/README.md; layout
# <dir>/XenosRecomp, <dir>/thirdparty). SR_XENOSRECOMP_SRC names another checkout of the pinned upstream to
# prepare from (like SR_XENONRECOMP_SRC for the guest code). Otherwise a lane worktree without initialized
# submodules falls back to the main checkout's submodule, then to its clone (SR_MAIN_ROOT, else the main
# checkout git knows: cmake/sr_main_root.cmake). Without sources the library is not built and
# gfx/shader_runtime_xlat.cpp compiles to a stub that logs "[SHADER_XLAT] ... NOT BUILT" (only with
# SR_ALLOW_NO_XLAT=ON).
#
# DXC is loaded at RUNTIME (LoadLibraryW + GetProcAddress("DxcCreateInstance")), not through
# the import library: a missing DLL disables runtime translation instead of stopping the exe
# from starting (0xC0000135).
get_filename_component(_sr_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)  # the repo root
option(SR_ALLOW_NO_XLAT "Build without runtime shader translation (a broken game; tests only)" OFF)
set(SR_XENOSRECOMP_DIR "" CACHE PATH
    "A patched XenosRecomp clone to build the runtime shader translator from (empty: the third_party/XenosRecomp submodule + tools/xenosrecomp patches)")
set(SR_XENOSRECOMP_SRC "" CACHE PATH
    "A XenosRecomp checkout with the pinned commit and its thirdparty submodules, in place of third_party/XenosRecomp (a worktree without initialized submodules)")
include("${CMAKE_CURRENT_LIST_DIR}/sr_main_root.cmake")
sr_main_root(_xr_main)

set(_xr "")    # the directory holding XenosRecomp/ (and LICENSE.md)
set(_xrt "")   # the directory holding thirdparty/{fmt,smol-v,zstd,xxHash}
set(_xr_sub "${_sr_root}/third_party/XenosRecomp")
set(_xr_main_sub "${_xr_main}/third_party/XenosRecomp")
if(SR_XENOSRECOMP_DIR)
  set(_xr "${SR_XENOSRECOMP_DIR}")
  set(_xrt "${SR_XENOSRECOMP_DIR}/thirdparty")
  set(_xr_how "the clone SR_XENOSRECOMP_DIR")
else()
  set(_xr_src "")
  set(_xr_cands "${_xr_sub}" "${_xr_main_sub}")
  if(SR_XENOSRECOMP_SRC)
    set(_xr_cands "${SR_XENOSRECOMP_SRC}")
  endif()
  foreach(_c ${_xr_cands})
    if(EXISTS "${_c}/thirdparty/fmt/include/fmt/core.h" AND EXISTS "${_c}/.git")
      set(_xr_src "${_c}")
      break()
    endif()
  endforeach()
  if(_xr_src)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_xr "${CMAKE_BINARY_DIR}/xenosrecomp")
    execute_process(
      COMMAND "${Python3_EXECUTABLE}" "${_sr_root}/tools/xenosrecomp/prepare.py" --src "${_xr_src}" --out "${_xr}"
      WORKING_DIRECTORY "${_sr_root}"
      RESULT_VARIABLE _xr_rc)
    if(NOT _xr_rc EQUAL 0)
      message(FATAL_ERROR "tools/xenosrecomp/prepare.py failed (rc=${_xr_rc}); see the output above.")
    endif()
    set(_xrt "${_xr_src}/thirdparty")
    set(_xr_how "${_xr_src} (pinned) + tools/xenosrecomp patches")
    file(GLOB _xr_patches "${_sr_root}/tools/xenosrecomp/*.patch")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_xr_patches}
                 "${_sr_root}/tools/xenosrecomp/prepare.py")
  elseif(EXISTS "${_xr_main}/tools/XenosRecomp-clone/XenosRecomp/shader_recompiler.cpp")
    # A lane worktree of a main checkout from before the submodule: its untracked clone.
    set(_xr "${_xr_main}/tools/XenosRecomp-clone")
    set(_xrt "${_xr}/thirdparty")
    set(_xr_how "the main checkout's clone")
  endif()
endif()
# What make_player_build.ps1 checks (the resolution patch) and the embed names.
set(SR_XENOSRECOMP_EFFECTIVE_DIR "${_xr}" CACHE INTERNAL "XenosRecomp sources the translator is built from")

if(WIN32 AND _xr AND EXISTS "${_xr}/XenosRecomp/shader_recompiler.cpp" AND EXISTS "${_xrt}/fmt/include/fmt/core.h")
  include("${CMAKE_CURRENT_LIST_DIR}/dxc_release.cmake")
  set(SR_HAVE_XENOS_XLAT ON)
  # shader_common.h bytes + translator source id, regenerated when a translator source changes.
  set(_xr_embed "${CMAKE_BINARY_DIR}/generated/xenos_embed.cpp")
  set(XR_DIR "${_xr}")
  set(OUT "${_xr_embed}")
  include("${CMAKE_CURRENT_LIST_DIR}/xenos_embed.cmake")
  unset(XR_DIR)
  unset(OUT)
  # Every file the translator id hashes (cmake/xenos_embed.cmake): a change re-runs configure,
  # which regenerates the embed, so the runtime cache directory follows the translator.
  foreach(_f constant_table.h pch.h shader.h shader_code.h shader_common.h shader_recompiler.cpp
             shader_recompiler.h dxc_compiler.cpp dxc_compiler.h main.cpp)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_xr}/XenosRecomp/${_f}")
  endforeach()
  foreach(_f gfx/shader_xlat_core.cpp gfx/shader_xlat_core.hpp gfx/xenos_bridge.cpp gfx/xenos_bridge.hpp
             gfx/shader_mkey.hpp)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sr_root}/${_f}")
  endforeach()

  add_library(xenos_xlat STATIC
    "${_xr}/XenosRecomp/shader_recompiler.cpp"
    "${_sr_root}/gfx/xenos_bridge.cpp"
    "${_sr_root}/gfx/shader_xlat_core.cpp"
    "${_xr_embed}")
  set_target_properties(xenos_xlat PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_include_directories(xenos_xlat PRIVATE
    "${_xr}/XenosRecomp"
    "${_xrt}/fmt/include"
    "${_xrt}/smol-v/source"
    "${_xrt}/zstd/lib"
    "${_xrt}/xxHash"
    "${SR_DXC_INC}"
    "${_sr_root}")
  # fmt header-only (the offline build links fmt.dll: same code, no DLL to ship). The offline
  # tool's own definitions (XenosRecomp/CMakeLists.txt): _CRT_SECURE_NO_WARNINGS, XENOS_RECOMP_DXIL.
  target_compile_definitions(xenos_xlat PRIVATE FMT_HEADER_ONLY _CRT_SECURE_NO_WARNINGS XENOS_RECOMP_DXIL)
  # The two XenosRecomp translation units see XenosRecomp's pch.h (full Windows.h, be<>, fmt),
  # exactly as the offline tool's precompiled header; the core stays a normal TU.
  set(_xr_tu_opts -include "${_xr}/XenosRecomp/pch.h" -fms-extensions -Wno-switch
      -Wno-unused-variable -Wno-null-arithmetic)
  set_source_files_properties("${_xr}/XenosRecomp/shader_recompiler.cpp" "${_sr_root}/gfx/xenos_bridge.cpp"
    PROPERTIES COMPILE_OPTIONS "${_xr_tu_opts}" COMPILE_DEFINITIONS "XXH_INLINE_ALL")
  set_source_files_properties("${_sr_root}/gfx/shader_xlat_core.cpp"
    PROPERTIES COMPILE_DEFINITIONS "NOMINMAX;WIN32_LEAN_AND_MEAN")
  message(STATUS "Runtime shader translation: XenosRecomp from ${_xr_how} (${_xr})")
else()
  set(SR_HAVE_XENOS_XLAT OFF)
  # iter-466: a build without the translator boots into a broken game (title freeze / ICALL_FAIL),
  # which cost lanes whole roll batches - refuse it unless explicitly asked for.
  set(_xr_msg "no XenosRecomp sources: run `git submodule update --init --recursive third_party/XenosRecomp`, "
              "or pass -DSR_XENOSRECOMP_DIR=<a patched clone> (tools/xenosrecomp/README.md)")
  if(SR_ALLOW_NO_XLAT)
    message(WARNING "Runtime shader translation NOT built (SR_ALLOW_NO_XLAT=ON): ${_xr_msg}. "
                    "A shader missing from the cache will drop its draws.")
  else()
    message(FATAL_ERROR "Runtime shader translation cannot be built: ${_xr_msg}. "
                        "-DSR_ALLOW_NO_XLAT=ON builds without it on purpose.")
  endif()
endif()

function(sr_link_xenos_xlat tgt)
  if(NOT SR_HAVE_XENOS_XLAT)
    return()
  endif()
  target_link_libraries(${tgt} PRIVATE xenos_xlat)
  target_compile_definitions(${tgt} PRIVATE SR_HAVE_XENOS_XLAT=1)
  # The exact DXC the offline cache was built with (identical DLL hashes are part of the
  # translator id), plus the licences their redistribution requires.
  add_custom_command(TARGET ${tgt} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SR_DXC_BIN}/dxcompiler.dll" "${SR_DXC_BIN}/dxil.dll"
      "$<TARGET_FILE_DIR:${tgt}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_xr}/LICENSE.md"
      "$<TARGET_FILE_DIR:${tgt}>/LICENSE-XenosRecomp.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_xrt}/fmt/LICENSE"
      "$<TARGET_FILE_DIR:${tgt}>/LICENSE-fmt.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sr_root}/tools/xenosrecomp/LICENSE-DXC.txt"
      "$<TARGET_FILE_DIR:${tgt}>/LICENSE-DXC.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sr_root}/tools/xenosrecomp/LICENSE-DXC-MS.txt"
      "$<TARGET_FILE_DIR:${tgt}>/LICENSE-DXC-MS.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_sr_root}/tools/xenosrecomp/LICENSE-DXC-MIT.txt"
      "$<TARGET_FILE_DIR:${tgt}>/LICENSE-DXC-MIT.txt")
endfunction()
