# xenos_conformance - L0 conformance units (docs/NATIVE_ROADMAP.md section 6.2).
# Builds standalone (no game tree, no vcpkg):
#   cmake -S tests/xenos_conformance -B <dir> -G Ninja -DCMAKE_CXX_COMPILER=clang++
#   cmake --build <dir> -j 2
# or from the root CMakeLists.txt: add_subdirectory(tests/xenos_conformance)
# Usage and attribution: the comment block at the top of conformance_main.cpp.
cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(xenos_conformance LANGUAGES CXX)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
  endif()
endif()

find_package(Threads REQUIRED)

add_executable(xenos_conformance
  conformance_main.cpp
  harness.hpp
  xenia_ref_common.hpp
  xenia_ref_formats.hpp
  xenia_ref_resolve.hpp
  xenia_ref_texture_address.hpp
  xenia_ref_gamma_ramp.hpp
)
target_compile_features(xenos_conformance PRIVATE cxx_std_20)
set_target_properties(xenos_conformance PROPERTIES CXX_EXTENSIONS OFF)
# The code under test: header-only gfx/ sources (edram.hpp, xenos_tiling.hpp,
# gamma_ramp.hpp, par.hpp).
target_include_directories(xenos_conformance PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../gfx
)
get_filename_component(XC_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../data/conformance" ABSOLUTE)
target_compile_definitions(xenos_conformance PRIVATE
  XC_DEFAULT_DATA_DIR="${XC_DATA_DIR}"
  _CRT_SECURE_NO_WARNINGS
)
# The reference ports model unfused multiply-add (see xenia_ref_formats.hpp):
# never let the compiler contract a*b+c into an FMA.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  target_compile_options(xenos_conformance PRIVATE /clang:-ffp-contract=off)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(xenos_conformance PRIVATE -ffp-contract=off)
elseif(MSVC)
  target_compile_options(xenos_conformance PRIVATE /fp:precise)
endif()
target_link_libraries(xenos_conformance PRIVATE Threads::Threads)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
endif()
add_test(NAME xenos_conformance COMMAND xenos_conformance --verify)
