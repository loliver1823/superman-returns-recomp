// iter-463 audio NATIVE: the XMA hardware context (64 bytes, big-endian in guest memory).
//
// Ported from Xenia Canary (BSD-3-Clause, see docs/THIRD_PARTY.md), the ORACLE build the user
// runs, canary_experimental@0bd090dbe:
//   src/xenia/apu/xma_context.h:49-162   (XMA_CONTEXT_DATA, verbatim bitfield layout)
//   src/xenia/apu/xma_context.h:173-196  (XmaContext constants)
// Copyright 2021 Ben Vanik / Xenia Canary. Released under the BSD license.
//
// The struct is the SAME C bitfield layout Xenia uses (it builds with MSVC / clang-cl on the MS
// ABI, as we do): each 32-bit word is loaded with a byte swap, and the bitfields are allocated
// from bit 0 upward inside that host-order word. tools/difftest/xma_test.cpp (T1) round-trips
// every field through raw big-endian bytes, including error_status = 4 -> BE word 4 << 26.
//
// Header-only, no FFmpeg, no harness dependencies (the offline tool links it standalone).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sr::kernel::apu {

inline void xmaCopyAndSwap32(uint32_t* dst, const uint32_t* src, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint32_t v;
    std::memcpy(&v, src + i, 4);
    v = __builtin_bswap32(v);
    std::memcpy(dst + i, &v, 4);
  }
}

// xma_context.h:49-162 (verbatim field order and widths).
struct XMA_CONTEXT_DATA {
  // DWORD 0
  uint32_t input_buffer_0_packet_count : 12;  // XMASetInputBuffer0, number of 2KB packets
  uint32_t loop_count : 8;                    // +12bit, XMASetLoopData NumLoops
  uint32_t input_buffer_0_valid : 1;          // +20bit, XMAIsInputBuffer0Valid
  uint32_t input_buffer_1_valid : 1;          // +21bit, XMAIsInputBuffer1Valid
  uint32_t output_buffer_block_count : 5;     // +22bit SizeWrite 256byte blocks
  uint32_t output_buffer_write_offset : 5;    // +27bit XMAGetOutputBufferWriteOffset

  // DWORD 1
  uint32_t input_buffer_1_packet_count : 12;  // XMASetInputBuffer1
  uint32_t loop_subframe_start : 2;           // +12bit, XMASetLoopData
  uint32_t loop_subframe_end : 3;             // +14bit, XMASetLoopData
  uint32_t loop_subframe_skip : 3;            // +17bit, XMASetLoopData
  uint32_t subframe_decode_count : 4;         // +20bit
  uint32_t output_buffer_padding : 3;         // +24bit (has207's guess, used as headroom)
  uint32_t sample_rate : 2;                   // +27bit enum of sample rates
  uint32_t is_stereo : 1;                     // +29bit
  uint32_t unk_dword_1_c : 1;                 // +30bit
  uint32_t output_buffer_valid : 1;           // +31bit, XMAIsOutputBufferValid

  // DWORD 2
  uint32_t input_buffer_read_offset : 26;  // XMAGetInputBufferReadOffset
  uint32_t error_status : 5;               // ErrorStatus
  uint32_t error_set : 1;                  // ErrorSet

  // DWORD 3
  uint32_t loop_start : 26;          // XMASetLoopData LoopStartOffset (bits)
  uint32_t parser_error_status : 5;  // ParserErrorStatus
  uint32_t parser_error_set : 1;     // ParserErrorSet

  // DWORD 4
  uint32_t loop_end : 26;        // XMASetLoopData LoopEndOffset (bits)
  uint32_t packet_metadata : 5;  // XMAGetPacketMetadata
  uint32_t current_buffer : 1;   // ?

  uint32_t input_buffer_0_ptr;  // DWORD 5, physical address
  uint32_t input_buffer_1_ptr;  // DWORD 6, physical address
  uint32_t output_buffer_ptr;   // DWORD 7, physical address
  uint32_t work_buffer_ptr;     // DWORD 8, PtrOverlapAdd(?)

  // DWORD 9
  uint32_t output_buffer_read_offset : 5;  // XMAGetOutputBufferReadOffset
  uint32_t : 25;
  uint32_t stop_when_done : 1;       // +30bit
  uint32_t interrupt_when_done : 1;  // +31bit

  // DWORD 10-15
  uint32_t unk_dwords_10_15[6];

  XMA_CONTEXT_DATA() { std::memset(this, 0, sizeof(*this)); }
  explicit XMA_CONTEXT_DATA(const void* ptr) {
    xmaCopyAndSwap32(reinterpret_cast<uint32_t*>(this), reinterpret_cast<const uint32_t*>(ptr),
                     sizeof(XMA_CONTEXT_DATA) / 4);
  }
  void Store(void* ptr) const {
    xmaCopyAndSwap32(reinterpret_cast<uint32_t*>(ptr), reinterpret_cast<const uint32_t*>(this),
                     sizeof(XMA_CONTEXT_DATA) / 4);
  }

  bool IsInputBufferValid(uint8_t buffer_index) const {
    return buffer_index == 0 ? input_buffer_0_valid : input_buffer_1_valid;
  }
  bool IsCurrentInputBufferValid() const { return IsInputBufferValid(current_buffer); }
  bool IsAnyInputBufferValid() const { return input_buffer_0_valid || input_buffer_1_valid; }
  uint32_t GetInputBufferAddress(uint8_t buffer_index) const {
    return buffer_index == 0 ? input_buffer_0_ptr : input_buffer_1_ptr;
  }
  uint32_t GetCurrentInputBufferAddress() const { return GetInputBufferAddress(current_buffer); }
  uint32_t GetInputBufferPacketCount(uint8_t buffer_index) const {
    return buffer_index == 0 ? input_buffer_0_packet_count : input_buffer_1_packet_count;
  }
  uint32_t GetCurrentInputBufferPacketCount() const {
    return GetInputBufferPacketCount(current_buffer);
  }
  bool IsStreamingContext() const {
    return (input_buffer_0_packet_count | input_buffer_1_packet_count) == 1;
  }
  bool IsConsumeOnlyContext() const {
    return (input_buffer_0_packet_count | input_buffer_1_packet_count) == 0;
  }
};
static_assert(sizeof(XMA_CONTEXT_DATA) == 64, "XMA_CONTEXT_DATA must be 64 bytes");

// xma_context.h:173-196 (XmaContext constants).
struct XmaConst {
  static constexpr uint32_t kBytesPerPacket = 2048;
  static constexpr uint32_t kBytesPerPacketHeader = 4;
  static constexpr uint32_t kBytesPerPacketData = kBytesPerPacket - kBytesPerPacketHeader;
  static constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
  static constexpr uint32_t kBitsPerHeader = 32;
  static constexpr uint32_t kBitsPerFrameHeader = 15;
  static constexpr uint32_t kBytesPerSample = 2;
  static constexpr uint32_t kSamplesPerFrame = 512;
  static constexpr uint32_t kSamplesPerSubframe = 128;
  static constexpr uint32_t kBytesPerFrameChannel = kSamplesPerFrame * kBytesPerSample;
  static constexpr uint32_t kBytesPerSubframeChannel = kSamplesPerSubframe * kBytesPerSample;
  static constexpr uint32_t kOutputBytesPerBlock = 256;
  static constexpr uint32_t kOutputMaxSizeBytes = 31 * kOutputBytesPerBlock;
  static constexpr uint32_t kLastFrameMarker = 0x7FFF;
};

// xboxkrnl_audio_xma.cc:92-115 (the guest-facing init structs, all big-endian).
struct XmaLoopDataBE {      // XMA_LOOP_DATA, 12 bytes
  uint8_t loop_start[4];
  uint8_t loop_end[4];
  uint8_t loop_count;
  uint8_t loop_subframe_end;
  uint8_t loop_subframe_skip;
  uint8_t pad;
};
static_assert(sizeof(XmaLoopDataBE) == 12, "XMA_LOOP_DATA is 12 bytes");
constexpr uint32_t kXmaContextInitSize = 56;  // XMA_CONTEXT_INIT: 11 be32 + XMA_LOOP_DATA

}  // namespace sr::kernel::apu
