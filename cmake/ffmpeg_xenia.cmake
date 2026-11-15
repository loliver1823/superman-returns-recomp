# iter-463 audio: Xenia's FFmpeg fork (avcodec + avutil) with the XMA frames decoder
# (AV_CODEC_ID_XMAFRAMES), built from source for the native XMA context port
# (kernel XMA* exports decoded on the host, as reference xenia apu/xma_context_new.cc does).
#
#   include(cmake/ffmpeg_xenia.cmake)       # after project(); enables the C language
#   sr_link_ffmpeg_xenia(SupermanReturns)   # links ffmpeg_xenia.dll (DELAY-LOADED), defines
#                                           # SR_HAVE_FFMPEG_XENIA=1, copies the DLL + licences
#
# WHY this method (CMake over the fork's sources, not the fork's ./configure + make):
#   * The source lists and the config overlay are Xenia's own (oracle build canary_experimental
#     @0bd090dbe, third_party/CMakeLists.txt "FFmpeg" block + third_party/ffmpeg-xenia/), so
#     what we compile is exactly what the oracle's decoder compiles - the same XMAFRAMES code,
#     the same CONFIG_* switches, the same codec list (mp3, wmapro, wmav2, xmaframes).
#   * No MSYS2/bash/nasm toolchain: the same clang that builds the game builds FFmpeg, inside
#     the same Ninja graph, with sccache. One `cmake --build build` stays the whole build.
#   * HAVE_X86ASM=0 in the overlay, and clang-for-msvc defines _MSC_VER, so no inline asm is
#     taken either: plain C, identical to Xenia's MSVC Windows build.
#
# Sources:
#   third_party/ffmpeg/FFmpeg          submodule https://github.com/has207/FFmpeg.git (branch
#                                      xmaframes) pinned at d980192 - the commit the oracle pins.
#   third_party/ffmpeg/xenia-config    Xenia's config overlay (config.h, codec/parser/bsf lists,
#                                      avconfig.h, ffversion.h), copied verbatim from the oracle.
#   SR_FFMPEG_XENIA_SRC overrides the source directory; when the submodule is not checked out it
#   falls back to reference/xenia-canary-oracle/third_party/FFmpeg, then reference/xenia-canary/.
#
# LGPL-2.1+: avcodec + avutil are linked into ONE shared library, ffmpeg_xenia.dll, that the game
# loads dynamically, so a user can rebuild/replace it (licence texts are copied next to the exe;
# docs/THIRD_PARTY.md). One DLL rather than avcodec + avutil DLLs: FFmpeg's split shared build
# needs its configure-generated export maps and per-library copies of avpriv_* helpers; the
# single DLL keeps Xenia's static source lists unchanged and still satisfies the relink clause.
# Two files are dropped from Xenia's lists, because a DLL links EVERY object while Xenia's static
# libs pull only referenced members, and these two are never pulled there:
#   libavcodec/file_open.c    `#include "libavutil/file_open.c"`, a second avpriv_open copy only a
#                             split shared build needs (duplicate symbol in one DLL).
#   libavutil/x86/tx_float_init.c  its codelet tables reference the x86 asm FFT kernels; tx.c
#                             only references the table under HAVE_X86ASM, which the overlay sets
#                             to 0 (undefined ff_tx_fft*_sse3/avx/fma3 in one DLL).
#
# The DLL is DELAY-LOADED: a missing DLL disables XMA decoding (sr::ffx::ffmpegXeniaAvailable()
# returns false) instead of stopping the exe from starting (0xC0000135). Callers MUST check
# availability before the first av* call - a failed delay-load raises an SEH exception, which the
# harness VEH treats as fatal.
get_filename_component(_sr_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
option(SR_FFMPEG_XENIA "Build Xenia's FFmpeg fork (XMA frames decoder) as ffmpeg_xenia.dll" ON)

# A lane worktree usually has no checked-out submodule: fall back to the main checkout's copies
# (SR_MAIN_ROOT, else the main checkout git knows - cmake/sr_main_root.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/sr_main_root.cmake")
sr_main_root(_ffx_main)
set(_ffx_candidates
  "${_sr_root}/third_party/ffmpeg/FFmpeg"
  "${_sr_root}/reference/xenia-canary-oracle/third_party/FFmpeg"
  "${_sr_root}/reference/xenia-canary/third_party/FFmpeg"
  "${_ffx_main}/third_party/ffmpeg/FFmpeg"
  "${_ffx_main}/reference/xenia-canary-oracle/third_party/FFmpeg")
if(NOT SR_FFMPEG_XENIA_SRC)
  foreach(_c ${_ffx_candidates})
    if(EXISTS "${_c}/libavcodec/wmaprodec.c")
      set(_ffx_found "${_c}")
      break()
    endif()
  endforeach()
  set(SR_FFMPEG_XENIA_SRC "${_ffx_found}" CACHE PATH
      "Xenia FFmpeg fork source (has207/FFmpeg xmaframes @d980192)")
endif()
set(_ffx "${SR_FFMPEG_XENIA_SRC}")
set(_ffx_cfg "${_sr_root}/third_party/ffmpeg/xenia-config")
set(SR_FFMPEG_XENIA_PIN "d980192e175e6ff95bcd287af77e16fcb6597974")

set(SR_HAVE_FFMPEG_XENIA OFF)
if(NOT SR_FFMPEG_XENIA)
  message(STATUS "FFmpeg (xenia fork): disabled (SR_FFMPEG_XENIA=OFF) - XMA decoding not built")
elseif(NOT WIN32)
  message(WARNING "FFmpeg (xenia fork): only the Windows x64 build is wired up")
elseif(NOT _ffx OR NOT EXISTS "${_ffx}/libavcodec/wmaprodec.c")
  # iter-466: without the XMA decoder the game dies at the title (ICALL_FAIL in an EAThread) - refuse.
  message(FATAL_ERROR "FFmpeg (xenia fork) source not found. Run: git submodule update --init "
                      "third_party/ffmpeg/FFmpeg, or set SR_FFMPEG_XENIA_SRC (a lane worktree: "
                      "<main checkout>/reference/xenia-canary-oracle/third_party/FFmpeg). "
                      "-DSR_FFMPEG_XENIA=OFF builds without XMA audio on purpose.")
else()
  # The fork's XMAFRAMES decoder lives in wmaprodec.c; refuse a tree without it.
  file(STRINGS "${_ffx}/libavcodec/wmaprodec.c" _ffx_xmaf REGEX "ff_xmaframes_decoder")
  if(NOT _ffx_xmaf)
    message(FATAL_ERROR "SR_FFMPEG_XENIA_SRC=${_ffx} has no ff_xmaframes_decoder: not Xenia's fork")
  endif()
  # iter-467: never skip the pin check silently. A tree whose git metadata is unreadable (the oracle worktree's
  # link broke in the 2026-10-09 re-clone, and the check went quiet) is reported as unverified.
  find_package(Git QUIET)
  set(_ffx_head "")
  if(GIT_FOUND AND EXISTS "${_ffx}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_ffx}" rev-parse HEAD
                    OUTPUT_VARIABLE _ffx_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  endif()
  if(NOT _ffx_head)
    message(WARNING "FFmpeg (xenia fork): cannot verify the pin ${SR_FFMPEG_XENIA_PIN} at ${_ffx} "
                    "(no readable git metadata). Use the submodule: git submodule update --init "
                    "third_party/ffmpeg/FFmpeg")
  elseif(NOT _ffx_head STREQUAL SR_FFMPEG_XENIA_PIN)
    message(WARNING "FFmpeg (xenia fork) at ${_ffx_head}, oracle pins ${SR_FFMPEG_XENIA_PIN}")
  else()
    message(STATUS "FFmpeg (xenia fork): ${_ffx} at the pinned ${SR_FFMPEG_XENIA_PIN}")
  endif()

  # C is needed only here. The preset pins clang++; use its clang sibling unless set.
  if(NOT CMAKE_C_COMPILER AND CMAKE_CXX_COMPILER MATCHES "[Cc]lang")
    get_filename_component(_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(EXISTS "${_llvm_bin}/clang.exe")
      set(CMAKE_C_COMPILER "${_llvm_bin}/clang.exe" CACHE FILEPATH "C compiler" FORCE)
    endif()
  endif()
  enable_language(C)

  set(_ffx_avutil_srcs
    adler32.c aes.c aes_ctr.c audio_fifo.c avstring.c avsscanf.c base64.c blowfish.c
    bprint.c buffer.c cast5.c camellia.c channel_layout.c csp.c cpu.c crc.c
    des.c dict.c display.c dovi_meta.c downmix_info.c encryption_info.c error.c eval.c
    fifo.c file.c file_open.c float_dsp.c fixed_dsp.c frame.c hash.c hdr_dynamic_metadata.c
    hmac.c hwcontext.c imgutils.c integer.c intmath.c lfg.c lls.c log.c
    log2_tab.c mathematics.c mastering_display_metadata.c md5.c mem.c murmur3.c opt.c parseutils.c
    pixdesc.c pixelutils.c random_seed.c rational.c reverse.c rc4.c ripemd.c samplefmt.c
    sha.c sha512.c slicethread.c spherical.c stereo3d.c threadmessage.c time.c timecode.c
    tree.c twofish.c utils.c xga_font_data.c xtea.c tea.c tx.c tx_float.c
    tx_double.c tx_int32.c video_enc_params.c film_grain_params.c side_data.c refstruct.c
    container_fifo.c float_scalarproduct.c timecode_internal.c iamf.c timestamp.c
    x86/cpu.c x86/fixed_dsp_init.c x86/float_dsp_init.c x86/imgutils_init.c x86/lls_init.c)
  set(_ffx_avcodec_srcs
    ac3_parser.c adts_parser.c allcodecs.c avcodec.c avdct.c packet.c bitstream.c
    bitstream_filters.c bsf.c codec_desc.c codec_par.c d3d11va.c decode.c dirac.c dv_profile.c
    encode.c imgconvert.c jni.c mathtables.c mediacodec.c mpeg12framerate.c options.c parser.c
    parsers.c profiles.c qsv_api.c raw.c utils.c vorbis_parser.c xiph.c dct32_fixed.c
    dct32_float.c faandct.c faanidct.c fdctdsp.c jfdctfst.c jfdctint.c idctdsp.c simple_idct.c
    jrevdct.c mpegaudio.c mpegaudiodec_common.c mpegaudiodsp.c mpegaudiodsp_data.c
    mpegaudiodsp_fixed.c mpegaudiodsp_float.c mpegaudiodecheader.c mpegaudiodata.c sinewin.c
    wma_freqs.c mpegaudiodec_fixed.c mpegaudiodec_float.c wmaprodec.c wma.c wma_common.c
    wmadec.c aactab.c mpegaudio_parser.c pthread.c pthread_slice.c pthread_frame.c get_buffer.c
    vlc.c threadprogress.c bsf/null.c exif.c mpegaudiotabs.c tiff_common.c
    x86/constants.c x86/fdctdsp_init.c x86/idctdsp_init.c x86/mpegaudiodsp.c x86/fdct.c)
  list(TRANSFORM _ffx_avutil_srcs PREPEND "${_ffx}/libavutil/")
  list(TRANSFORM _ffx_avcodec_srcs PREPEND "${_ffx}/libavcodec/")

  # Xenia's ffmpeg_common(): HAVE_AV_CONFIG_H, overlay first on the include path, win32
  # atomics, bcrypt (random_seed), all warnings off.
  function(_sr_ffmpeg_common tgt)
    target_compile_definitions(${tgt} PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES
                               _CRT_SECURE_NO_WARNINGS)
    target_include_directories(${tgt} PRIVATE "${_ffx_cfg}" "${_ffx}"
                               "${_ffx}/compat/atomics/win32")
    # -Wno-error=incompatible-pointer-types: the overlay's codec/parser/bsf lists declare
    # `const AVCodec *` arrays of FFCodec objects (FFCodec starts with its AVCodec, so the
    # pointers are identical). MSVC only warns; clang >= 16 makes it an error that -w leaves on.
    target_compile_options(${tgt} PRIVATE -w -Wno-error=incompatible-pointer-types)
    set_target_properties(${tgt} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endfunction()

  add_library(ffx_avutil OBJECT ${_ffx_avutil_srcs})
  _sr_ffmpeg_common(ffx_avutil)
  target_compile_definitions(ffx_avutil PRIVATE BUILDING_avutil)
  add_library(ffx_avcodec OBJECT ${_ffx_avcodec_srcs})
  _sr_ffmpeg_common(ffx_avcodec)
  target_compile_definitions(ffx_avcodec PRIVATE BUILDING_avcodec)
  target_include_directories(ffx_avcodec PRIVATE "${_ffx}/libavcodec")

  add_library(ffmpeg_xenia SHARED $<TARGET_OBJECTS:ffx_avutil> $<TARGET_OBJECTS:ffx_avcodec>)
  set_target_properties(ffmpeg_xenia PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
    LINKER_LANGUAGE C
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ffmpeg_xenia")
  target_link_libraries(ffmpeg_xenia PRIVATE bcrypt)
  # Consumers see the public headers (overlay first: avconfig.h / ffversion.h are generated).
  target_include_directories(ffmpeg_xenia INTERFACE "${_ffx_cfg}" "${_ffx}")

  set(SR_HAVE_FFMPEG_XENIA ON)
  message(STATUS "FFmpeg (xenia fork): ${_ffx} -> ffmpeg_xenia.dll (avcodec+avutil, XMAFRAMES)")
endif()

# Link a host target against ffmpeg_xenia.dll (delay-loaded) and ship the DLL + licences.
function(sr_link_ffmpeg_xenia tgt)
  if(NOT SR_HAVE_FFMPEG_XENIA)
    return()
  endif()
  target_link_libraries(${tgt} PRIVATE ffmpeg_xenia delayimp)
  target_link_options(${tgt} PRIVATE "LINKER:/DELAYLOAD:ffmpeg_xenia.dll")
  target_compile_definitions(${tgt} PRIVATE SR_HAVE_FFMPEG_XENIA=1)
  add_custom_command(TARGET ${tgt} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ffmpeg_xenia>"
      "$<TARGET_FILE_DIR:${tgt}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ffx}/COPYING.LGPLv2.1"
      "$<TARGET_FILE_DIR:${tgt}>/LICENSE-FFmpeg-LGPLv2.1.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ffx_cfg}/LICENSE-xenia.txt"
      "$<TARGET_FILE_DIR:${tgt}>/LICENSE-xenia.txt")
endfunction()
