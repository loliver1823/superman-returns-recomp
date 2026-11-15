// iter-463 audio NATIVE: the per-context XMA frame decoder seam.
//
// XmaContextNew (xma_context_new.cpp) drives the decoder exactly as Xenia's
// XmaContextNew::PrepareDecoder / DecodePacket do (xma_context_new.cc:915-978). The FFmpeg calls
// live in ONE translation unit (xma_ffmpeg_decoder.cpp); the state-machine tests inject a mock
// through this interface, so they run without FFmpeg or game data.
#pragma once

#include <cstdint>
#include <memory>

namespace sr::kernel::apu {

class IXmaFrameDecoder {
 public:
  virtual ~IXmaFrameDecoder() = default;
  // PrepareDecoder(sample_rate_hz, channels): 0 = unchanged, 1 = (re)opened, -1 = open failed.
  virtual int Prepare(int sample_rate_hz, int channels) = 0;
  // DecodePacket: one xmaframes packet (first byte = padding bits, then the frame bits).
  // true = a frame came out; planes[0..channels-1] then point at 512 floats each (planar),
  // valid until the next call.
  virtual bool Decode(const uint8_t* data, int size, const float* planes[2]) = 0;
};

using XmaFrameDecoderFactory = std::unique_ptr<IXmaFrameDecoder> (*)();

// The FFmpeg (AV_CODEC_ID_XMAFRAMES) decoder. Returns nullptr when ffmpeg_xenia.dll is not
// available (sr::ffx::ffmpegXeniaAvailable() false) - callers must treat that as "no decoder".
std::unique_ptr<IXmaFrameDecoder> makeFfmpegXmaFrameDecoder();

}  // namespace sr::kernel::apu
