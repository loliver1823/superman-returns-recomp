# The DirectX Shader Compiler the runtime shader translator loads (cmake/xenos_xlat.cmake).
#
# The official v1.8.2407 release (https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2407,
# dxc_2024_07_31.zip), downloaded at configure time and checked by SHA-256: the zip, then the two DLLs the game
# loads. They are the DLLs the offline shader cache (data/shader_cache) was built with: XenosRecomp's
# thirdparty/dxc-bin @737ac9f, which the build used before, holds byte-identical copies (and the same inc/
# headers). The DLL bytes are part of the runtime translator id, so a different DXC is a different translator.
#
#   include(cmake/dxc_release.cmake)   # sets SR_DXC_BIN (dxcompiler.dll, dxil.dll) and SR_DXC_INC (dxcapi.h)
#
# SR_DXC_DIR: an unpacked release (bin/x64/, inc/) to use instead of the download, for a machine without
# network access; its DLLs must hash the same. Downloads are kept in SR_DOWNLOAD_DIR (default
# <build>/_downloads; the environment variable SR_DOWNLOAD_CACHE sets the default for every build tree).
set(_dxc_ver "1.8.2407")
set(_dxc_url "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2407/dxc_2024_07_31.zip")
set(_dxc_zip_sha256 "e2627f004f0f9424d8c71ea1314d04f38c5a5096884ae9217f1f18bd320267b5")
set(_dxc_dll_sha256_dxcompiler "1220478c87af97551d2342906c5a08d888ee9fa26f6f99b6072d674801f492d1")
set(_dxc_dll_sha256_dxil "e038a9755387fc396a9a5f393d292b9318cf6ac7e520e9283bbc3088c5f8ac93")

if(DEFINED ENV{SR_DOWNLOAD_CACHE})
  set(_dl_default "$ENV{SR_DOWNLOAD_CACHE}")
else()
  set(_dl_default "${CMAKE_BINARY_DIR}/_downloads")
endif()
set(SR_DOWNLOAD_DIR "${_dl_default}" CACHE PATH "Where configure-time downloads (the DXC release) are kept")
set(SR_DXC_DIR "" CACHE PATH "An unpacked DXC ${_dxc_ver} release (bin/x64, inc) instead of the download")

if(SR_DXC_DIR)
  set(_dxc_root "${SR_DXC_DIR}")
else()
  set(_dxc_zip "${SR_DOWNLOAD_DIR}/dxc_2024_07_31.zip")
  set(_dxc_root "${SR_DOWNLOAD_DIR}/dxc-${_dxc_ver}")
  if(NOT EXISTS "${_dxc_root}/bin/x64/dxcompiler.dll" OR NOT EXISTS "${_dxc_root}/bin/x64/dxil.dll"
     OR NOT EXISTS "${_dxc_root}/inc/dxcapi.h")
    # file(DOWNLOAD) skips the transfer when the file is already there with this hash.
    file(DOWNLOAD "${_dxc_url}" "${_dxc_zip}" EXPECTED_HASH SHA256=${_dxc_zip_sha256} TLS_VERIFY ON
         STATUS _dxc_st)
    list(GET _dxc_st 0 _dxc_rc)
    if(NOT _dxc_rc EQUAL 0)
      list(GET _dxc_st 1 _dxc_msg)
      message(FATAL_ERROR "DXC ${_dxc_ver}: download failed (${_dxc_msg}): ${_dxc_url}\n"
                          "Download it by hand and pass -DSR_DXC_DIR=<the unpacked zip>.")
    endif()
    file(REMOVE_RECURSE "${_dxc_root}")
    file(ARCHIVE_EXTRACT INPUT "${_dxc_zip}" DESTINATION "${_dxc_root}"
         PATTERNS "bin/x64/dxcompiler.dll" "bin/x64/dxil.dll" "inc/*" "LICENSE-*.txt")
  endif()
endif()
foreach(_d dxcompiler dxil)
  set(_p "${_dxc_root}/bin/x64/${_d}.dll")
  if(NOT EXISTS "${_p}")
    message(FATAL_ERROR "DXC ${_dxc_ver}: ${_p} is missing")
  endif()
  file(SHA256 "${_p}" _h)
  if(NOT _h STREQUAL _dxc_dll_sha256_${_d})
    message(FATAL_ERROR "DXC: ${_p} is not the ${_dxc_ver} release (SHA-256 ${_h}, want ${_dxc_dll_sha256_${_d}}). "
                        "The runtime translator must load the DXC the shader cache was built with.")
  endif()
endforeach()
set(SR_DXC_BIN "${_dxc_root}/bin/x64")
set(SR_DXC_INC "${_dxc_root}/inc")
message(STATUS "DXC ${_dxc_ver}: ${SR_DXC_BIN} (SHA-256 checked)")
