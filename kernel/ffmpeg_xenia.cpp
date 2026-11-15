// iter-463 audio: see ffmpeg_xenia.hpp. The decoder-open sequence mirrors
// reference/xenia-canary-oracle src/xenia/apu/xma_context_new.cc (Xenia Canary, BSD-3; see
// third_party/ffmpeg/xenia-config/LICENSE-xenia.txt): Setup() + PrepareDecoder().
#include "kernel/ffmpeg_xenia.hpp"

#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

#if defined(SR_HAVE_FFMPEG_XENIA)
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/error.h"
}
#endif

namespace sr::ffx {

namespace {
std::once_flag g_once;
bool g_ok = false;
std::string g_why;

void loadOnce() {
#if !defined(SR_HAVE_FFMPEG_XENIA)
  g_why = "not built (SR_FFMPEG_XENIA=OFF or no FFmpeg source at configure time)";
#elif defined(_WIN32)
  wchar_t path[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
  if (n == 0 || n >= MAX_PATH * 2) {
    g_why = "GetModuleFileNameW failed";
    return;
  }
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!slash) {
    g_why = "exe path has no directory";
    return;
  }
  wcscpy_s(slash + 1, (MAX_PATH * 2) - (slash + 1 - path), L"ffmpeg_xenia.dll");
  // Full path + LOAD_WITH_ALTERED_SEARCH_PATH: this exact module, then the delay-load helper
  // finds it already loaded by name and binds to it.
  HMODULE h = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!h) {
    g_why = "ffmpeg_xenia.dll not loadable next to the exe (error " +
            std::to_string(GetLastError()) + ")";
    return;
  }
  g_ok = true;
#else
  g_ok = true;
#endif
}
}  // namespace

bool ffmpegXeniaAvailable(std::string* why) {
  std::call_once(g_once, loadOnce);
  if (!g_ok && why) *why = g_why;
  return g_ok;
}

int probeXmaFramesDecoder(int sampleRate, int channels, std::string* why) {
  if (!ffmpegXeniaAvailable(why)) return -1;
#if defined(SR_HAVE_FFMPEG_XENIA)
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!codec) {
    if (why) *why = "avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES) returned null";
    return -2;
  }
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    if (why) *why = "avcodec_alloc_context3 failed";
    return -3;
  }
  ctx->sample_rate = sampleRate;
  av_channel_layout_default(&ctx->ch_layout, channels);
  ctx->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
  int ret = avcodec_open2(ctx, codec, nullptr);
  if (ret < 0) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(ret, buf, sizeof(buf));
    if (why) *why = std::string("avcodec_open2 failed: ") + buf;
    avcodec_free_context(&ctx);
    return -4;
  }
  if (why) {
    *why = std::string("opened '") + codec->name + "' rate=" + std::to_string(ctx->sample_rate) +
           " ch=" + std::to_string(ctx->ch_layout.nb_channels) +
           " fmt=" + std::to_string(static_cast<int>(ctx->sample_fmt));
  }
  avcodec_free_context(&ctx);
  return 0;
#else
  return -1;
#endif
}

}  // namespace sr::ffx
