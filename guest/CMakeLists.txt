# Guest code - XenonRecomp or ReXGlue output for Superman Returns
# Requires Clang (__builtin_*, __attribute__((weak)))

cmake_minimum_required(VERSION 3.20)

set(GUEST_IMPORT_GLUE ppc_import_glue.cpp)
set(GUEST_HARNESS guest_harness.cpp)
set(GUEST_MATH math_helpers.cpp)

# C1 (kernel/sr_oracle.hpp, root CMakeLists.txt SR_RELEASE_COMPOSITION): the units of this library that read SR_ORACLE -
# the import glue (the NATIVE N0 frame-split gate, kernel/native_brackets.hpp; the SR_XMA_NEW selector,
# kernel/apu/xma_api.hpp), kernel/fm_module_walker.cpp (the SR_CTORWALK_ALWAYS double-run A/B), and guest_harness.cpp
# (the legacy guest-mirror workaround, absent in release). The recompiled game units
# never read it. In the release configuration these units leave this library and are compiled ONCE PER EXECUTABLE (the
# object libraries at the end of this file: sr_guest_glue_release with SR_ORACLE=0 into SupermanReturns, sr_guest_glue_dev
# with SR_ORACLE=1 into difftest). A library both executables link cannot carry one definition of a selector that must
# differ between them: compiled here with SR_ORACLE=0, difftest's import glue took the native XMA branch whatever
# xmaSetEnabledForTest said (Astra round 1 on release/emu-removal stage 0, xma_test T5d).
set(GUEST_LIB_IMPORT_GLUE ${GUEST_IMPORT_GLUE})
set(GUEST_LIB_MODULE_WALKER ${CMAKE_SOURCE_DIR}/kernel/fm_module_walker.cpp)
set(GUEST_LIB_HARNESS ${GUEST_HARNESS})
if(SR_RELEASE_COMPOSITION)
  set(GUEST_LIB_IMPORT_GLUE)
  set(GUEST_LIB_MODULE_WALKER)
  set(GUEST_LIB_HARNESS)
endif()

if(BUILD_GUEST_REXGLUE)
  # ReXGlue-generated guest code (guest/ppc_rexglue/ must exist after running rexglue codegen)
  set(PPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ppc_rexglue)
  if(NOT EXISTS "${PPC_DIR}")
    message(FATAL_ERROR "BUILD_GUEST_REXGLUE=ON but guest/ppc_rexglue/ not found. Run ReXGlue codegen first. See reference/REXGLUE_BUILD.md")
  endif()
  file(GLOB PPC_SOURCES "${PPC_DIR}/*_recomp.[0-9]*.cpp")
  list(SORT PPC_SOURCES)
  if(NOT PPC_SOURCES)
    message(FATAL_ERROR "No *_recomp.*.cpp found in guest/ppc_rexglue/. Run: rexglue codegen guest/config/superman_rexglue.toml")
  endif()
  file(GLOB PPC_INIT_SOURCES "${PPC_DIR}/*_init.cpp")
  if(NOT PPC_INIT_SOURCES)
    message(FATAL_ERROR "No *_init.cpp found in guest/ppc_rexglue/. ReXGlue codegen produces {project}_init.cpp with PPCFuncMappings.")
  endif()
  add_library(SupermanReturnsGuest STATIC
    ${PPC_SOURCES}
    ${PPC_INIT_SOURCES}
    ppc_rexglue_entry.cpp
    ppc/ppc_missing_recomp_stubs.cpp
    ${GUEST_LIB_IMPORT_GLUE}
    ${GUEST_LIB_HARNESS}
    ${GUEST_MATH}
  )
  target_include_directories(SupermanReturnsGuest PUBLIC
    ${PPC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ppc
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tools/XenonRecomp/thirdparty/simde
  )
  message(STATUS "Building guest code from ReXGlue output (guest/ppc_rexglue/)")
else()
  # XenonRecomp-generated guest code (default).
  # R1 (docs/SPLICE_CONVERSION.md): guest/ppc is regenerated HERE, at configure time, from the
  # player's own default.xex whenever any input of tools/regen/regen.py changed (its stamp
  # guest/ppc/.regen_stamp): pinned upstream XenonRecomp + tools/regen/patches, built in the
  # build tree, then the post-regen pipeline. The repository carries no recompiled game code.
  option(SR_REGEN "Regenerate guest/ppc at configure time when its inputs change" ON)
  # The player's XEX: the extracted disc folder docs/BUILDING.md names ("Superman Returns (USA)/",
  # gitignored, which the game also reads at run time), else private/default.xex (private/README.md).
  set(_sr_xex_default "${CMAKE_SOURCE_DIR}/Superman Returns (USA)/default.xex")
  if(NOT EXISTS "${_sr_xex_default}" AND EXISTS "${CMAKE_SOURCE_DIR}/private/default.xex")
    set(_sr_xex_default "${CMAKE_SOURCE_DIR}/private/default.xex")
  endif()
  set(SR_XEX_PATH "${_sr_xex_default}" CACHE FILEPATH
      "The retail default.xex the guest code is recompiled from (never committed)")
  set(SR_XENONRECOMP_SRC "${CMAKE_SOURCE_DIR}/tools/XenonRecomp" CACHE PATH
      "XenonRecomp sources (a checkout that has the commit tools/XenonRecomp pins)")
  if(SR_REGEN)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(GLOB _sr_regen_inputs
      "${CMAKE_SOURCE_DIR}/tools/regen/*.py"
      "${CMAKE_SOURCE_DIR}/tools/regen/*.toml"
      "${CMAKE_SOURCE_DIR}/tools/regen/*.json"
      "${CMAKE_SOURCE_DIR}/tools/regen/*.txt"
      "${CMAKE_SOURCE_DIR}/tools/regen/patches/*"
      "${CMAKE_SOURCE_DIR}/tools/regen/hand/*")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_sr_regen_inputs})
    if(EXISTS "${SR_XEX_PATH}")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SR_XEX_PATH}")
    elseif(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ppc/.regen_stamp")
      message(FATAL_ERROR "No default.xex at ${SR_XEX_PATH}. The guest code is recompiled from your own "
                          "copy of the game: extract your USA disc to 'Superman Returns (USA)/' in the "
                          "repository root (docs/BUILDING.md, docs/DUMPING.md), or reconfigure with "
                          "-DSR_XEX_PATH=<path to default.xex>.")
    endif()
    execute_process(
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/regen/regen.py" --ensure
              --xex "${SR_XEX_PATH}" --xr-src "${SR_XENONRECOMP_SRC}"
              --work "${CMAKE_BINARY_DIR}/regen"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      RESULT_VARIABLE _sr_regen_rc)
    if(NOT _sr_regen_rc EQUAL 0)
      message(FATAL_ERROR "tools/regen/regen.py failed (rc=${_sr_regen_rc}); see the output above.")
    endif()
  endif()
  file(GLOB PPC_SOURCES "ppc/ppc_recomp.[0-9]*.cpp")
  list(SORT PPC_SOURCES)
  add_library(SupermanReturnsGuest STATIC
    ${PPC_SOURCES}
    ppc/ppc_func_mapping.cpp
    ppc/ppc_missing_recomp_stubs.cpp
    ${GUEST_LIB_IMPORT_GLUE}
    ${GUEST_LIB_HARNESS}
    ${GUEST_MATH}
    ${GUEST_LIB_MODULE_WALKER}
  )
  # SIMDe comes from the same XenonRecomp checkout the regen uses (tools/XenonRecomp in a full
  # clone; a lane worktree without initialized submodules points SR_XENONRECOMP_SRC elsewhere).
  target_include_directories(SupermanReturnsGuest PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ppc
    ${CMAKE_SOURCE_DIR}
    ${SR_XENONRECOMP_SRC}/thirdparty/simde
  )

  # iter-463 SR_TIMEBASE_50MHZ phase 1: XenonRecomp lowers mftb to `__rdtsc()` (raw host
  # TSC, ~74x the console's 50 MHz timebase). Force-include guest/sr_mftb.h - which
  # re-points __rdtsc() at sr_guest_mftb() (kernel/sr_timebase.cpp) - into exactly the
  # generated TUs that contain `__rdtsc`, detected at configure
  # time. Only these TUs rebuild when the header changes. After a regen, re-run
  # `cmake -B build` (the regen README already requires it for the glob).
  set(SR_MFTB_TUS "")
  foreach(_src ${PPC_SOURCES})
    file(STRINGS "${_src}" _sr_mftb_hit REGEX "__rdtsc" LIMIT_COUNT 1)
    if(_sr_mftb_hit)
      list(APPEND SR_MFTB_TUS "${_src}")
    endif()
  endforeach()
  list(LENGTH SR_MFTB_TUS _sr_mftb_n)
  if(_sr_mftb_n GREATER 0)
    set_source_files_properties(${SR_MFTB_TUS} PROPERTIES
      COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/sr_mftb.h")
  endif()
  set(_sr_mftb_names "")
  foreach(_src ${SR_MFTB_TUS})
    get_filename_component(_n "${_src}" NAME)
    list(APPEND _sr_mftb_names "${_n}")
  endforeach()
  message(STATUS "sr_mftb: force-included into ${_sr_mftb_n} guest TUs: ${_sr_mftb_names}")
endif()

target_compile_features(SupermanReturnsGuest PRIVATE cxx_std_20)

# iter-466 tick2: the deployed guest tree must match the committed transform inputs. guest/ppc is
# untracked: a commit that changes the register localization (tools/regen/flag_transform.py, its
# bridge spec, the [localize] policy) reaches a checkout only through
# `python tools/regen/flag_transform.py --deployed guest/ppc`, which writes guest/ppc/.transform_stamp.
# Same hash as flag_transform.py stamp_hash(); configure re-runs when an input or the stamp changes.
option(SR_ALLOW_STALE_GUEST "Build even when guest/ppc/.transform_stamp does not match the transform inputs" OFF)
if(NOT BUILD_GUEST_REXGLUE)
  set(_ft_inputs
    "${CMAKE_SOURCE_DIR}/tools/regen/flag_transform.py"
    "${CMAKE_SOURCE_DIR}/tools/regen/abi_bridge.json"
    "${CMAKE_SOURCE_DIR}/tools/regen/superman_regen.toml")
  set(_ft_stamp "${CMAKE_CURRENT_SOURCE_DIR}/ppc/.transform_stamp")
  set(_ft_cat "")
  foreach(_f IN LISTS _ft_inputs)
    file(SHA256 "${_f}" _h)
    string(APPEND _ft_cat "${_h}")
  endforeach()
  string(SHA256 _ft_want "${_ft_cat}")
  set(_ft_have "")
  if(EXISTS "${_ft_stamp}")
    file(STRINGS "${_ft_stamp}" _ft_line LIMIT_COUNT 1)
    string(REGEX REPLACE "^flag_transform v1 ([0-9a-f]+)$" "\\1" _ft_have "${_ft_line}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ft_inputs} "${_ft_stamp}")
  if(NOT _ft_have STREQUAL _ft_want)
    set(_ft_msg "guest/ppc is STALE: its register-localization stamp (${_ft_stamp}) does not match the "
                "committed transform inputs (tools/regen/flag_transform.py, abi_bridge.json, "
                "superman_regen.toml). From the checkout root run:\n"
                "    python tools/regen/flag_transform.py --deployed guest/ppc\n"
                "then build again (tools/regen/README.md, Register localization).")
    if(SR_ALLOW_STALE_GUEST)
      message(WARNING ${_ft_msg})
    else()
      message(FATAL_ERROR ${_ft_msg})
    endif()
  endif()
endif()

# iter-465 perf2: guest ISA level (root CMakeLists.txt SR_GUEST_ISA).
if(SR_GUEST_ISA STREQUAL "ssse3")
  target_compile_options(SupermanReturnsGuest PRIVATE -mssse3)
endif()
# iter-466 tick: ISO volatile (root CMakeLists.txt SR_GUEST_ISO_VOLATILE).
if(SR_GUEST_ISO_VOLATILE)
  target_compile_options(SupermanReturnsGuest PRIVATE -fno-ms-volatile)
endif()
# iter-467 guestcode: direct calls on hot guest edges (tools/regen/direct_calls.py) bypass the weak
# alias an override replaces; every build re-checks that none of the listed callees is overridden now.
find_program(SR_GUARD_PYTHON NAMES python REQUIRED)
add_custom_target(direct_calls_guard ALL
  COMMAND "${SR_GUARD_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/regen/direct_calls_guard.py" "${CMAKE_CURRENT_SOURCE_DIR}/ppc"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "direct_calls_guard: no overridden callee on a direct-call edge"
  VERBATIM)
add_dependencies(SupermanReturnsGuest direct_calls_guard)
# iter-467 guestcode EXPERIMENT: alias-free guest addressing (root CMakeLists.txt SR_GUEST_ALIAS_IDENTITY).
if(SR_GUEST_ALIAS_IDENTITY)
  target_compile_definitions(SupermanReturnsGuest PRIVATE
    "PPC_GUEST_OFFSET(x)=((uintptr_t)(uint32_t)(x))"
    "PPC_RAW_ADDR(x)=(PPC_GUEST_BASE() + (uintptr_t)(uint32_t)(x))")
endif()
# iter-466 tick: guest PGO (root CMakeLists.txt SR_GUEST_PGO).
if(SR_GUEST_PGO STREQUAL "gen")
  target_compile_options(SupermanReturnsGuest PRIVATE -fprofile-instr-generate)
  # The periodic profile dump lives in the guest lib (pgo_dump.cpp, kept by /INCLUDE), so a
  # gen / use / off switch rebuilds only the guest lib and relinks.
  target_sources(SupermanReturnsGuest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pgo_dump.cpp)
  target_link_options(SupermanReturnsGuest INTERFACE -fprofile-instr-generate
    "LINKER:/INCLUDE:sr_pgo_dump_anchor")
elseif(SR_GUEST_PGO)
  target_compile_options(SupermanReturnsGuest PRIVATE "-fprofile-instr-use=${SR_GUEST_PGO}"
    -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin)
endif()

# C1 (see GUEST_LIB_IMPORT_GLUE above): in the release configuration the library's SR_ORACLE readers are compiled once per
# executable - sr_guest_glue_release (SR_ORACLE=0, linked into SupermanReturns) and sr_guest_glue_dev (SR_ORACLE=1, linked
# into difftest) - with the library's include directories and the same compile options / definitions the blocks above
# gave it (copied here, after every one of them).
if(SR_RELEASE_COMPOSITION)
  set(_sr_oracle_glue ${GUEST_IMPORT_GLUE} ${GUEST_HARNESS})
  foreach(_cfg IN ITEMS release dev)
    add_library(sr_guest_glue_${_cfg} OBJECT ${_sr_oracle_glue})
    # The old forced CRT/heap walk has no generated caller. Keep it solely as
    # an Oracle diagnostic; the release follows the guest's natural _initterm.
    if(_cfg STREQUAL "dev" AND NOT BUILD_GUEST_REXGLUE)
      target_sources(sr_guest_glue_${_cfg} PRIVATE ${CMAKE_SOURCE_DIR}/kernel/fm_module_walker.cpp)
    endif()
    target_link_libraries(sr_guest_glue_${_cfg} PRIVATE SupermanReturnsGuest)
    target_compile_features(sr_guest_glue_${_cfg} PRIVATE cxx_std_20)
    foreach(_prop IN ITEMS COMPILE_OPTIONS COMPILE_DEFINITIONS)
      get_target_property(_v SupermanReturnsGuest ${_prop})
      if(_v)
        set_property(TARGET sr_guest_glue_${_cfg} APPEND PROPERTY ${_prop} ${_v})
      endif()
    endforeach()
  endforeach()
  target_compile_definitions(sr_guest_glue_release PRIVATE SR_ORACLE=0)
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(WARNING "Guest code requires Clang. Build with -DCMAKE_CXX_COMPILER=clang-cl")
endif()
