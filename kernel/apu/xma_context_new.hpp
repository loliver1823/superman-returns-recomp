// iter-463 audio NATIVE: one XMA decoder context - a port of Xenia's "new" XMA decoder.
//
// Ported from Xenia Canary (BSD-3-Clause, see docs/THIRD_PARTY.md), the ORACLE build the user
// runs (xma_decoder = "new", use_dedicated_xma_thread = true), canary_experimental@0bd090dbe:
//   src/xenia/apu/xma_context_new.h / .cc  (Copyright 2024 Xenia Canary)
//   src/xenia/apu/xma_context.h / .cc      (Copyright 2021 Ben Vanik: Block, ConvertFrame)
// Released under the BSD license.
// Xenia also credits libertyv xma2dec.c: historical, none of its code is here (docs/THIRD_PARTY.md).
//
// Includes the oracle-only fixes that are NOT in our older reference/xenia-canary:
//   6bef1fb  drain the current frame before ending the Work loop
//   7015473  follow the sub-stream skip chain past frameless packets
//   aee0871  count the frame whose header crosses the packet boundary
//
// Deviations (each one is a harness-safety or test-seam change, never a behaviour change on
// valid input):
//   * memory goes through IXmaMemory, which returns nullptr instead of faulting (the harness VEH
//     makes any host AV fatal); every nullptr is logged ([XMA_PHYS_MISS]) and handled as "buffer
//     not available" where Xenia would have crashed;
//   * the FFmpeg calls sit behind IXmaFrameDecoder (tests inject a mock);
//   * the kick-completion handshake uses sequence numbers instead of an auto-reset event
//     (critique B1: a late signal must not satisfy the NEXT kick) - see XmaDevice;
//   * ConvertFrame pins MXCSR rounding to nearest (the dedicated worker's default; the inline
//     mode runs on a guest thread whose MXCSR the harness owns);
//   * mutation seams (g_xmaMutation) for the difftests; 0 in production.
#pragma once

#include "kernel/apu/xma_context_data.hpp"
#include "kernel/apu/xma_frame_decoder.hpp"
#include "kernel/apu/xma_util.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace sr::kernel::apu {

// Guest memory as the decoder sees it. Both return nullptr (never fault) when the address is
// not backed.
class IXmaMemory {
 public:
  virtual ~IXmaMemory() = default;
  // The 64-byte context at guest VA `va`.
  virtual uint8_t* TranslateVirtual(uint32_t va) = 0;
  // Physical address `pa` (as stored in the context by the XMA exports), `bytes` long, for the
  // context `ctx_id`.
  virtual uint8_t* TranslatePhysical(uint32_t ctx_id, uint32_t pa, uint32_t bytes) = 0;
};

// Test-only mutation seam (difftest mutation validation). 0 = the faithful port.
//   1 = drop 6bef1fb (Work loop exits with a partially consumed frame)
//   2 = drop 7015473 (GetNextPacketReadOffset scans sequentially, ignoring the skip chain)
//   3 = drop aee0871 (GetPacketInfo does not count a split-header frame)
//   4 = StoreContextMerged stores the whole decoder copy (clobbers concurrent guest writes)
//   5 = ConvertFrame truncates toward zero instead of rounding to nearest
extern std::atomic<int> g_xmaMutation;

struct kPacketInfo {
  uint8_t frame_count_;
  uint8_t current_frame_;
  uint32_t current_frame_size_;
  bool isLastFrameInPacket() const {
    return frame_count_ == 0 || current_frame_ == frame_count_ - 1;
  }
};

struct kPacketHandle {
  uint32_t buffer_index_ = 0;
  uint32_t packet_index_ = 0;
  bool is_valid_ = false;
};

static constexpr int kIdToSampleRate[4] = {24000, 32000, 44100, 48000};

class XmaContextNew : public XmaConst {
 public:
  static constexpr uint32_t kBitsPerPacketHeader = 32;
  static constexpr uint32_t kMaxFrameSizeinBits = 0x4000 - kBitsPerPacketHeader;

  XmaContextNew() = default;
  ~XmaContextNew() = default;

  // Setup(id, memory, guest_ptr) + the decoder (may be null: no FFmpeg -> frames never decode).
  int Setup(uint32_t id, IXmaMemory* memory, uint32_t guest_ptr,
            std::unique_ptr<IXmaFrameDecoder> decoder);
  bool Work();

  void Enable() { set_is_enabled(true); }
  bool Block(bool poll);   // xma_context.h:206-215
  void Clear();
  void Disable() { set_is_enabled(false); }
  void Release();

  uint32_t id() const { return id_; }
  uint32_t guest_ptr() const { return guest_ptr_; }
  bool is_allocated() const { return is_allocated_.load(std::memory_order_acquire); }
  bool is_enabled() const { return is_enabled_.load(std::memory_order_acquire); }
  void set_is_allocated(bool v) { is_allocated_.store(v, std::memory_order_release); }
  void set_is_enabled(bool v) { is_enabled_.store(v, std::memory_order_release); }

  // Kick handshake (replaces work_completion_event_): Enable() is preceded by kick_seq_++;
  // Work() records the kick sequence it consumed when it clears is_enabled.
  uint64_t BumpKickSeq() { return kick_seq_.fetch_add(1, std::memory_order_acq_rel) + 1; }
  // Observe-only read for the [RWAC_Q] per-context kick deltas (kernel/audio_cmdq_obs.cpp).
  uint64_t kick_seq() const { return kick_seq_.load(std::memory_order_relaxed); }
  uint64_t done_seq() const { return done_seq_.load(std::memory_order_acquire); }
  void PublishDone(uint64_t seq) {
    uint64_t cur = done_seq_.load(std::memory_order_acquire);
    while (cur < seq && !done_seq_.compare_exchange_weak(cur, seq, std::memory_order_acq_rel)) {}
  }
  uint64_t last_work_seq() const { return last_work_seq_; }

  // Diagnostics.
  uint64_t frames_decoded() const { return frames_decoded_.load(std::memory_order_relaxed); }
  uint64_t decode_failures() const { return decode_failures_.load(std::memory_order_relaxed); }
  uint64_t phys_misses() const { return phys_misses_.load(std::memory_order_relaxed); }
  uint8_t current_frame_remaining_subframes() const { return current_frame_remaining_subframes_; }

  static void ConvertFrame(const float* const* samples, bool is_two_channel,
                           uint8_t* output_buffer);
  static int GetSampleRate(int id);

 private:
  uint8_t* TranslatePhysical(uint32_t pa, uint32_t bytes);

  void ClearLocked(XMA_CONTEXT_DATA* data);
  static void SwapInputBuffer(XMA_CONTEXT_DATA* data);
  static int16_t GetPacketNumber(size_t size, size_t bit_offset);
  kPacketInfo GetPacketInfo(uint8_t* packet, uint32_t frame_offset);
  uint32_t GetAmountOfBitsToRead(uint32_t remaining_stream_bits, uint32_t frame_size);
  kPacketHandle GetPacketHandle(XMA_CONTEXT_DATA* data, uint32_t buffer_index,
                                uint32_t packet_index, uint32_t current_input_packet_count);
  const uint8_t* GetNextPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                               uint32_t current_input_packet_count);
  uint32_t GetNextPacketReadOffset(uint8_t* buffer, uint32_t next_packet_index,
                                   uint32_t current_input_packet_count);
  uint32_t GetNextPacketReadOffset(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                                   uint32_t current_input_packet_count);
  uint8_t* GetCurrentInputBuffer(XMA_CONTEXT_DATA* data);
  static uint32_t GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data);
  void Decode(XMA_CONTEXT_DATA* data);
  void Consume(RingBuffer* output_rb, const XMA_CONTEXT_DATA* data);
  void UpdateLoopStatus(XMA_CONTEXT_DATA* data);
  int PrepareDecoder(int sample_rate, bool is_two_channel);
  void PreparePacket(uint32_t frame_size, uint32_t frame_padding);
  bool PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data, RingBuffer* out);
  void StoreContextMerged(const XMA_CONTEXT_DATA& data, const XMA_CONTEXT_DATA& initial_data,
                          uint8_t* context_ptr);

  IXmaMemory* memory_ = nullptr;
  std::unique_ptr<IXmaFrameDecoder> decoder_;
  uint32_t id_ = 0;
  uint32_t guest_ptr_ = 0;
  std::mutex lock_;
  std::atomic<bool> is_allocated_{false};
  std::atomic<bool> is_enabled_{false};
  std::atomic<uint64_t> kick_seq_{0};
  std::atomic<uint64_t> done_seq_{0};
  uint64_t last_work_seq_ = 0;

  std::array<uint8_t, kBytesPerPacketData * 2> input_buffer_{};
  std::array<uint8_t, 1 + 4096> xma_frame_{};   // first byte = bit offset information
  std::array<uint8_t, kBytesPerFrameChannel * 2> raw_frame_{};
  int packet_size_ = 0;   // av_packet_->size (PreparePacket)
  int32_t remaining_subframe_blocks_in_output_buffer_ = 0;
  uint8_t current_frame_remaining_subframes_ = 0;
  uint8_t loop_frame_output_limit_ = 0;
  bool loop_start_skip_pending_ = false;

  std::atomic<uint64_t> frames_decoded_{0};
  std::atomic<uint64_t> decode_failures_{0};
  std::atomic<uint64_t> phys_misses_{0};
};

}  // namespace sr::kernel::apu
