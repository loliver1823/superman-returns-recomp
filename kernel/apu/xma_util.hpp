// iter-463 audio NATIVE: XMA packet helpers, bit stream and ring buffer.
//
// Ported from Xenia Canary (BSD-3-Clause, see docs/THIRD_PARTY.md), oracle build
// canary_experimental@0bd090dbe:
//   src/xenia/apu/xma_helpers.h        (Copyright 2023 Ben Vanik) - verbatim
//   src/xenia/base/bit_stream.{h,cc}   (Copyright 2015/2021 Ben Vanik) - Peek/Read/Copy/SetOffset
//   src/xenia/base/ring_buffer.{h,cc}  (Copyright 2015 Ben Vanik) - the members XMA uses
// Released under the BSD license.
//
// Only deviation: unaligned 64-bit loads go through memcpy (identical values; defined behaviour).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sr::kernel::apu {

namespace xma {
static constexpr uint32_t kMaxFrameLength = 0x7FFF;
// Number of frames that /begin/ in this packet (valid only for XMA2 packets).
inline uint8_t GetPacketFrameCount(const uint8_t* packet) { return packet[0] >> 2; }
inline uint8_t GetPacketMetadata(const uint8_t* packet) { return packet[2] & 0x7; }
inline bool IsPacketXma2Type(const uint8_t* packet) { return GetPacketMetadata(packet) == 1; }
inline uint8_t GetPacketSkipCount(const uint8_t* packet) { return packet[3]; }
// The first frame offset in bits.
inline uint32_t GetPacketFrameOffset(const uint8_t* packet) {
  uint32_t val =
      (uint16_t)(((packet[0] & 0x3) << 13) | (packet[1] << 5) | (packet[2] >> 3));
  return val + 32;
}
}  // namespace xma

// xenia/base/bit_stream.{h,cc}
class BitStream {
 public:
  BitStream(uint8_t* buffer, size_t size_in_bits) : buffer_(buffer), size_bits_(size_in_bits) {}
  size_t offset_bits() const { return offset_bits_; }
  size_t size_bits() const { return size_bits_; }
  void SetOffset(size_t offset_bits) { offset_bits_ = std::min(offset_bits, size_bits_); }
  void Advance(size_t num_bits) { SetOffset(offset_bits_ + num_bits); }
  size_t BitsRemaining() const { return size_bits_ - offset_bits_; }

  // num_bits MUST be in the range 0-57 (inclusive).
  uint64_t Peek(size_t num_bits) const {
    size_t offset_bytes = std::min(offset_bits_ >> 3, (size_bits_ - 64) >> 3);
    size_t rel_offset_bits = offset_bits_ - (offset_bytes << 3);
    uint64_t bits;
    std::memcpy(&bits, buffer_ + offset_bytes, 8);
    bits = __builtin_bswap64(bits);
    bits >>= 64 - (rel_offset_bits + num_bits);
    bits &= (1ULL << num_bits) - 1;
    return bits;
  }
  uint64_t Read(size_t num_bits) {
    uint64_t val = Peek(num_bits);
    Advance(num_bits);
    return val;
  }
  size_t Copy(uint8_t* dest_buffer, size_t num_bits) {
    size_t offset_bytes = offset_bits_ >> 3;
    size_t rel_offset_bits = offset_bits_ - (offset_bytes << 3);
    size_t bits_left = num_bits;
    size_t out_offset_bytes = 0;
    // First: Copy the first few bits up to a byte boundary.
    if (rel_offset_bits) {
      uint64_t bits = Peek(8 - rel_offset_bits);
      uint8_t clear_mask = ~((uint8_t(1) << rel_offset_bits) - 1);
      dest_buffer[out_offset_bytes] &= clear_mask;
      dest_buffer[out_offset_bytes] |= (uint8_t)bits;
      bits_left -= 8 - rel_offset_bits;
      Advance(8 - rel_offset_bits);
      out_offset_bytes++;
    }
    // Second: memcpy for the bytes left.
    if (bits_left >= 8) {
      std::memcpy(dest_buffer + out_offset_bytes, buffer_ + offset_bytes + out_offset_bytes,
                  bits_left / 8);
      out_offset_bytes += (bits_left / 8);
      Advance((bits_left / 8) * 8);
      bits_left -= (bits_left / 8) * 8;
    }
    // Third: Copy the last few bits.
    if (bits_left) {
      uint64_t bits = Peek(bits_left);
      bits <<= 8 - bits_left;
      uint8_t clear_mask = ((uint8_t(1) << bits_left) - 1);
      dest_buffer[out_offset_bytes] &= clear_mask;
      dest_buffer[out_offset_bytes] |= (uint8_t)bits;
      Advance(bits_left);
    }
    // Return the bit offset to the copied bits.
    return rel_offset_bits;
  }

 private:
  uint8_t* buffer_ = nullptr;
  size_t offset_bits_ = 0;
  size_t size_bits_ = 0;
};

// xenia/base/ring_buffer.{h,cc} - the subset XmaContextNew uses.
class RingBuffer {
 public:
  RingBuffer(uint8_t* buffer, size_t capacity)
      : buffer_(buffer), capacity_(static_cast<uint32_t>(capacity)) {}
  uint32_t capacity() const { return capacity_; }
  bool empty() const { return read_offset_ == write_offset_; }
  uint32_t read_offset() const { return read_offset_; }
  uint32_t write_offset() const { return write_offset_; }
  void set_read_offset(size_t offset) { read_offset_ = capacity_ ? offset % capacity_ : 0; }
  void set_write_offset(size_t offset) {
    write_offset_ = capacity_ ? static_cast<uint32_t>(offset) % capacity_ : 0;
  }
  uint32_t write_count() const {
    if (read_offset_ == write_offset_) {
      return capacity_;
    } else if (write_offset_ < read_offset_) {
      return read_offset_ - write_offset_;
    } else {
      return (capacity_ - write_offset_) + read_offset_;
    }
  }
  size_t Write(const uint8_t* buffer, size_t _count) {
    uint32_t count = static_cast<uint32_t>(_count);
    count = std::min(count, capacity_);
    if (!count) return 0;
    if (write_offset_ + count < capacity_) {
      std::memcpy(buffer_ + write_offset_, buffer, count);
      write_offset_ += count;
    } else {
      uint32_t left_half = capacity_ - write_offset_;
      uint32_t right_half = count - left_half;
      std::memcpy(buffer_ + write_offset_, buffer, left_half);
      std::memcpy(buffer_, buffer + left_half, right_half);
      write_offset_ = right_half;
    }
    return count;
  }

 private:
  uint8_t* buffer_ = nullptr;
  uint32_t capacity_ = 0;
  uint32_t read_offset_ = 0;
  uint32_t write_offset_ = 0;
};

}  // namespace sr::kernel::apu
