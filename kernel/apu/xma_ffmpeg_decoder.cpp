// iter-463 audio NATIVE: the FFmpeg XMAFRAMES decoder behind IXmaFrameDecoder.
//
// Ported from Xenia Canary (BSD-3-Clause, see docs/THIRD_PARTY.md), oracle build
// canary_experimental@0bd090dbe:
//   src/xenia/apu/xma_context_new.cc:47-82   (Setup: packet / codec / context / frame)
//   src/xenia/apu/xma_context_new.cc:915-941 (PrepareDecoder)
//   src/xenia/apu/xma_context_new.cc:955-978 (DecodePacket)
//   src/xenia/apu/xma_decoder.cc:84-128      (av_log_callback: warnings and errors only)
// Copyright 2024 Xenia Canary. Released under the BSD license.
// Xenia also credits libertyv xma2dec.c: historical, none of its code is here (docs/THIRD_PARTY.md).
//
// This is the ONLY translation unit that calls FFmpeg for XMA. ffmpeg_xenia.dll is delay-loaded;
// makeFfmpegXmaFrameDecoder() checks sr::ffx::ffmpegXeniaAvailable() before the first av* call
// (a failed delay-load is fatal under the harness VEH).
#include "kernel/apu/xma_frame_decoder.hpp"

#include "kernel/ffmpeg_xenia.hpp"
#include "app/log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

#if defined(SR_HAVE_FFMPEG_XENIA)
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
}
#endif

namespace sr::kernel::apu {

#if defined(SR_HAVE_FFMPEG_XENIA)
namespace {

std::atomic<uint32_t> g_avLogLines{0};

// xma_decoder.cc av_log_callback: only warnings and errors (ffmpeg_verbose defaults false).
void avLogCallback(void*, int level, const char* fmt, va_list va) {
  if (level > AV_LOG_WARNING) return;
  const uint32_t n = g_avLogLines.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > 64 && (n & (n - 1)) != 0) return;   // first 64, then powers of two
  char buf[512];
  vsnprintf(buf, sizeof(buf), fmt, va);
  SR_LOG_AUDIO("[XMA_FFMPEG] #%u %s: %s", n, level <= AV_LOG_ERROR ? "error" : "warning", buf);
}

class FfmpegXmaFrameDecoder final : public IXmaFrameDecoder {
 public:
  bool Setup() {
    av_packet_ = av_packet_alloc();
    if (!av_packet_) return false;
    av_packet_->buf = av_buffer_alloc(128 * 1024);
    av_codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
    if (!av_codec_) {
      SR_LOG_AUDIO("[XMA] XmaContext: Codec not found");
      return false;
    }
    av_context_ = avcodec_alloc_context3(av_codec_);
    if (!av_context_) return false;
    // Initialize these to 0. They'll actually be set later.
    av_context_->ch_layout = AVChannelLayout{};
    av_context_->sample_rate = 0;
    av_frame_ = av_frame_alloc();
    return av_frame_ != nullptr;
  }
  ~FfmpegXmaFrameDecoder() override {
    if (av_context_) avcodec_free_context(&av_context_);
    if (av_frame_) av_frame_free(&av_frame_);
    if (av_packet_) av_packet_free(&av_packet_);
  }

  int Prepare(int sample_rate, int channels) override {
    if (av_context_->sample_rate != sample_rate ||
        av_context_->ch_layout.nb_channels != channels) {
      // We have to recreate the codec context so it'll realloc whatever data it needs.
      avcodec_free_context(&av_context_);
      av_context_ = avcodec_alloc_context3(av_codec_);
      av_context_->sample_rate = sample_rate;
      av_channel_layout_default(&av_context_->ch_layout, channels);
      av_context_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
      if (avcodec_open2(av_context_, av_codec_, nullptr) < 0) {
        SR_LOG_AUDIO("[XMA] XmaContext: Failed to reopen FFmpeg context");
        return -1;
      }
      return 1;
    }
    return 0;
  }

  bool Decode(const uint8_t* data, int size, const float* planes[2]) override {
    av_packet_->data = const_cast<uint8_t*>(data);
    av_packet_->size = size;
    int ret = avcodec_send_packet(av_context_, av_packet_);
    if (ret < 0) {
      logErr("Error sending packet for decoding", ret);
      return false;
    }
    ret = avcodec_receive_frame(av_context_, av_frame_);
    if (ret == AVERROR(EAGAIN)) return false;   // needs more input (first frame warm-up)
    if (ret < 0) {
      logErr("Error during decoding", ret);
      return false;
    }
    planes[0] = reinterpret_cast<const float*>(av_frame_->data[0]);
    planes[1] = reinterpret_cast<const float*>(av_frame_->data[1]);
    return true;
  }

 private:
  void logErr(const char* what, int ret) {
    const uint32_t n = errLogs_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 16 && (n & (n - 1)) != 0) return;
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(ret, errbuf, sizeof(errbuf));
    SR_LOG_AUDIO("[XMA] XmaContext: %s: %s (%d) #%u", what, errbuf, ret, n);
  }

  AVPacket* av_packet_ = nullptr;
  const AVCodec* av_codec_ = nullptr;
  AVCodecContext* av_context_ = nullptr;
  AVFrame* av_frame_ = nullptr;
  static inline std::atomic<uint32_t> errLogs_{0};
};

}  // namespace

std::unique_ptr<IXmaFrameDecoder> makeFfmpegXmaFrameDecoder() {
  std::string why;
  if (!sr::ffx::ffmpegXeniaAvailable(&why)) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      SR_LOG_AUDIO("[XMA] ffmpeg_xenia.dll unavailable (%s): XMA contexts will produce no PCM",
                   why.c_str());
    return nullptr;
  }
  static std::once_flag once;
  std::call_once(once, [] { av_log_set_callback(avLogCallback); });
  auto d = std::make_unique<FfmpegXmaFrameDecoder>();
  if (!d->Setup()) return nullptr;
  return d;
}

#else   // !SR_HAVE_FFMPEG_XENIA

std::unique_ptr<IXmaFrameDecoder> makeFfmpegXmaFrameDecoder() { return nullptr; }

#endif

}  // namespace sr::kernel::apu
