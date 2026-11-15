# Step 4 item 7a (lane D, 2026-10-20): the host's own HLSL compiled ahead of time (gfx/host_hlsl.hpp).
#
# tools/regen/host_hlsl_aot.py extracts the raw-string HLSL literals the code compiles at runtime (the frame
# renderer's transfer passes, the EDRAM unit, the scan-out, the overlay, the present chain, the costume viewer) and
# compiles every variant with the same D3DCompile (d3dcompiler_47) into two units of byte arrays, one build edge each,
# each depending only on its own sources:
#   <build>/generated/host_hlsl_blobs.cpp         the shipping shaders (every configuration)
#   <build>/generated/host_hlsl_blobs_oracle.cpp  the development build's oracle-only shaders (an ORACLE_SOURCES entry: the
#                                                 release configuration leaves it out with the sources it is made from)
# The game uses them only with SR_HOST_HLSL_AOT=1; a source the tool does not cover still compiles at runtime.
#
#   include(cmake/host_hlsl.cmake)   # before HARNESS_SOURCES_NO_MAIN: appends both units to APP_SOURCES
find_program(SR_HOST_HLSL_PYTHON NAMES python python3 REQUIRED)
set(SR_HOST_HLSL_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/host_hlsl_blobs.cpp")
set(SR_HOST_HLSL_OUT_ORACLE "${CMAKE_CURRENT_BINARY_DIR}/generated/host_hlsl_blobs_oracle.cpp")
# Astra (2026-10-20): the blobs are pinned to one d3dcompiler (sha256 + size); another DLL fails the generator.
set(SR_HOST_HLSL_PIN "${CMAKE_SOURCE_DIR}/tools/regen/d3dcompiler_47.pin.json")
# The COMMAND names each source it reads (--file REL=ABS), never the source root: the root folder holds the build
# tree, and the release composition audit reads a COMMAND that names a folder as reading everything under it.
set(SR_HOST_HLSL_SHIP_SRC gfx/d3d12_frame_render.cpp gfx/d3d12_pm4_shmem.cpp gfx/d3d12_overlay.cpp
  gfx/d3d12_present_chain.cpp gfx/costume_viewer_draw.cpp)
set(SR_HOST_HLSL_ORACLE_SRC gfx/d3d12_frame_render.cpp gfx/d3d12_edram_model.cpp gfx/d3d12_backend.cpp)
set(SR_HOST_HLSL_SHIP_ARGS)
foreach(f IN LISTS SR_HOST_HLSL_SHIP_SRC)
  list(APPEND SR_HOST_HLSL_SHIP_ARGS --file "${f}=${CMAKE_SOURCE_DIR}/${f}")
endforeach()
set(SR_HOST_HLSL_ORACLE_ARGS)
foreach(f IN LISTS SR_HOST_HLSL_ORACLE_SRC)
  list(APPEND SR_HOST_HLSL_ORACLE_ARGS --file "${f}=${CMAKE_SOURCE_DIR}/${f}")
endforeach()
add_custom_command(OUTPUT "${SR_HOST_HLSL_OUT}"
  COMMAND "${SR_HOST_HLSL_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/regen/host_hlsl_aot.py" ${SR_HOST_HLSL_SHIP_ARGS}
    --pin "${SR_HOST_HLSL_PIN}" --only ship --out "${SR_HOST_HLSL_OUT}"
  DEPENDS tools/regen/host_hlsl_aot.py tools/regen/d3dcompiler_47.pin.json ${SR_HOST_HLSL_SHIP_SRC}
  COMMENT "host HLSL ahead of time: the shipping shaders (tools/regen/host_hlsl_aot.py)"
  VERBATIM)
add_custom_command(OUTPUT "${SR_HOST_HLSL_OUT_ORACLE}"
  COMMAND "${SR_HOST_HLSL_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/regen/host_hlsl_aot.py" ${SR_HOST_HLSL_ORACLE_ARGS}
    --pin "${SR_HOST_HLSL_PIN}" --only oracle --out-oracle "${SR_HOST_HLSL_OUT_ORACLE}"
  DEPENDS tools/regen/host_hlsl_aot.py tools/regen/d3dcompiler_47.pin.json ${SR_HOST_HLSL_ORACLE_SRC}
  COMMENT "host HLSL ahead of time: the oracle-only shaders (tools/regen/host_hlsl_aot.py)"
  VERBATIM)
list(APPEND APP_SOURCES "${SR_HOST_HLSL_OUT}" "${SR_HOST_HLSL_OUT_ORACLE}")
set_source_files_properties("${SR_HOST_HLSL_OUT}" "${SR_HOST_HLSL_OUT_ORACLE}" PROPERTIES
  INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}")
