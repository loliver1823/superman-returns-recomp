// iter-463 audio: host access to Xenia's FFmpeg fork (ffmpeg_xenia.dll, cmake/ffmpeg_xenia.cmake).
//
// The DLL is delay-loaded. A failed delay-load raises an SEH exception that the harness VEH treats
// as fatal, so EVERY caller checks ffmpegXeniaAvailable() before its first av* call; the check
// loads the DLL from the exe's directory (never the search path) exactly once.
#pragma once

#include <string>

namespace sr::ffx {

// True when the build linked FFmpeg (SR_HAVE_FFMPEG_XENIA) AND ffmpeg_xenia.dll loaded from the
// exe directory. On false, *why (optional) says which.
bool ffmpegXeniaAvailable(std::string* why = nullptr);

// Opens the XMA frames decoder (AV_CODEC_ID_XMAFRAMES) the way xenia apu/xma_context_new.cc
// PrepareDecoder() does (sample rate, default channel layout, AV_CODEC_FLAG2_SKIP_MANUAL), then
// frees it. Returns 0 on success, a negative code on failure (*why says which step).
int probeXmaFramesDecoder(int sampleRate, int channels, std::string* why = nullptr);

}  // namespace sr::ffx
