// iter-463 audio: smoke test for ffmpeg_xenia.dll (cmake/ffmpeg_xenia.cmake).
//
//   build\ffmpeg_xenia_test.exe          -> "<N> passed, <M> failed", exit 0 only when M == 0
//
// Loads the DLL through the same delay-load guard the game uses (kernel/ffmpeg_xenia.cpp), finds
// AV_CODEC_ID_XMAFRAMES (present only in Xenia's fork) and opens it the way xenia
// apu/xma_context_new.cc PrepareDecoder() does, for every XMA sample rate (xma_context
// GetSampleRate: 24000, 32000, 44100, 48000) in mono and stereo. It also checks the decoder
// outputs planar float, the format xma_context_new's ConvertFrame consumes.
#include <cstdio>
#include <string>

#include "kernel/ffmpeg_xenia.hpp"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/samplefmt.h"
}

int main() {
  int pass = 0, fail = 0;
  auto check = [&](bool ok, const std::string& what, const std::string& detail) {
    std::printf("[%s] %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.empty() ? "" : " : ",
                detail.c_str());
    (ok ? pass : fail)++;
  };

  std::string why;
  bool avail = sr::ffx::ffmpegXeniaAvailable(&why);
  check(avail, "ffmpeg_xenia.dll loads next to the exe", avail ? "" : why);
  if (!avail) {
    std::printf("%d passed, %d failed\n", pass, fail);
    return 1;
  }

  // (avcodec_version()/avcodec_configuration() live in version.c, which Xenia's list omits.)
  std::printf("headers: avcodec %d.%d.%d (has207/FFmpeg xmaframes)\n", LIBAVCODEC_VERSION_MAJOR,
              LIBAVCODEC_VERSION_MINOR, LIBAVCODEC_VERSION_MICRO);
  const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  check(c != nullptr, "avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES)", c ? c->name : "null");
  if (c) {
    check(c->type == AVMEDIA_TYPE_AUDIO, "XMAFRAMES is an audio decoder", "");
    bool fltp = false;
    const void* fmts = nullptr;
    int nfmts = 0;
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &fmts, &nfmts) >= 0 && fmts) {
      const AVSampleFormat* f = static_cast<const AVSampleFormat*>(fmts);
      for (int i = 0; i < nfmts; ++i) fltp |= (f[i] == AV_SAMPLE_FMT_FLTP);
    }
    check(fltp, "XMAFRAMES advertises AV_SAMPLE_FMT_FLTP", "");
  }

  for (int rate : {24000, 32000, 44100, 48000}) {
    for (int ch : {1, 2}) {
      std::string d;
      int r = sr::ffx::probeXmaFramesDecoder(rate, ch, &d);
      bool ok = r == 0 && d.find("fmt=" + std::to_string(int(AV_SAMPLE_FMT_FLTP))) != std::string::npos;
      check(ok, "open XMAFRAMES rate=" + std::to_string(rate) + " ch=" + std::to_string(ch), d);
    }
  }

  std::printf("%d passed, %d failed\n", pass, fail);
  return fail == 0 ? 0 : 1;
}
