// iter-463 audio NATIVE: port of Xenia's XmaContextNew. Contract and deviations:
// kernel/apu/xma_context_new.hpp.
//
// Ported from Xenia Canary (BSD-3-Clause, see docs/THIRD_PARTY.md), oracle build
// canary_experimental@0bd090dbe, src/xenia/apu/xma_context_new.cc (Copyright 2024 Xenia Canary)
// and src/xenia/apu/xma_context.cc (Copyright 2021 Ben Vanik). Released under the BSD license.
// Xenia's file credits koolkdev/libertyv xma2dec.c "for most of this code". The credit is
// historical (Xenia's 2015 decoder); no xma2dec.c code is in this file (docs/THIRD_PARTY.md).
//
// Line references (xnc = xma_context_new.cc @0bd090dbe) are kept next to each function.
#include "kernel/apu/xma_context_new.hpp"

#include "app/log.hpp"

#include <algorithm>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace sr::kernel::apu {

std::atomic<int> g_xmaMutation{0};

namespace {
inline int mut() { return g_xmaMutation.load(std::memory_order_relaxed); }
std::atomic<uint32_t> g_missLogs{0}, g_warnLogs{0};
bool logN(std::atomic<uint32_t>& c, uint32_t first) {
  const uint32_t n = c.fetch_add(1, std::memory_order_relaxed) + 1;
  return n <= first || (n & (n - 1)) == 0;
}
}  // namespace

// xnc:47-82
int XmaContextNew::Setup(uint32_t id, IXmaMemory* memory, uint32_t guest_ptr,
                         std::unique_ptr<IXmaFrameDecoder> decoder) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;
  decoder_ = std::move(decoder);
  return 0;
}

uint8_t* XmaContextNew::TranslatePhysical(uint32_t pa, uint32_t bytes) {
  uint8_t* p = memory_ ? memory_->TranslatePhysical(id_, pa, bytes) : nullptr;
  if (!p) {
    phys_misses_.fetch_add(1, std::memory_order_relaxed);
    if (logN(g_missLogs, 16))
      SR_LOG_AUDIO("[XMA_PHYS_MISS] ctx=%u pa=%08X bytes=%u (buffer treated as unavailable)", id_,
                   pa, bytes);
  }
  return p;
}

// xnc:84-111
bool XmaContextNew::PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data, RingBuffer* out) {
  const uint32_t output_capacity = data->output_buffer_block_count * kOutputBytesPerBlock;
  const uint32_t output_read_offset = data->output_buffer_read_offset * kOutputBytesPerBlock;
  const uint32_t output_write_offset = data->output_buffer_write_offset * kOutputBytesPerBlock;

  if (output_capacity > kOutputMaxSizeBytes && logN(g_warnLogs, 8)) {
    SR_LOG_AUDIO("[XMA] XmaContext %u: Output buffer uses more space than expected! "
                 "(Actual: %u Max: %u)", id(), output_capacity, kOutputMaxSizeBytes);
  }
  uint8_t* output_buffer = output_capacity ? TranslatePhysical(data->output_buffer_ptr,
                                                               output_capacity)
                                           : nullptr;
  // Deviation: Xenia would fault (or divide by a zero capacity); we report "no output buffer".
  if (!output_buffer) return false;

  // Output buffers are in raw PCM samples, 256 bytes per block. Output buffer is a ring buffer.
  // We need to write from the write offset to the read offset.
  *out = RingBuffer(output_buffer, output_capacity);
  out->set_read_offset(output_read_offset);
  out->set_write_offset(output_write_offset);
  remaining_subframe_blocks_in_output_buffer_ =
      (int32_t)out->write_count() / kOutputBytesPerBlock;
  return true;
}

// xnc:113-244
bool XmaContextNew::Work() {
  if (!is_enabled() || !is_allocated()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(false);
  // Kick handshake: the kick this Work consumes (see header).
  last_work_seq_ = kick_seq_.load(std::memory_order_acquire);

  uint8_t* context_ptr = memory_ ? memory_->TranslateVirtual(guest_ptr()) : nullptr;
  if (!context_ptr) return true;
  XMA_CONTEXT_DATA data(context_ptr);
  const XMA_CONTEXT_DATA initial_data = data;

  if (!data.output_buffer_valid) {
    return true;
  }

  RingBuffer output_rb(nullptr, 0);
  if (!PrepareOutputRingBuffer(&data, &output_rb)) {
    return true;
  }

  if (data.IsConsumeOnlyContext()) {
    // Nothing to drain - don't touch the context or we'll reset the game's output buffer
    // offsets, causing stale PCM to be re-read.
    if (current_frame_remaining_subframes_ == 0) {
      return true;
    }
    Consume(&output_rb, &data);
    data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;
    if (output_rb.empty()) {
      data.output_buffer_valid = false;
    }
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  // Minimum free blocks needed before attempting a decode: the subframes Consume() writes per
  // iteration (subframe_decode_count, clamped to 1) plus output_buffer_padding headroom.
  const uint32_t effective_sdc = std::max(static_cast<uint32_t>(1), data.subframe_decode_count);
  const int32_t minimum_subframe_decode_count =
      static_cast<int32_t>(effective_sdc) + data.output_buffer_padding;

  // We don't have enough space to even make one pass. Waiting for decoder to return more space.
  if (minimum_subframe_decode_count > remaining_subframe_blocks_in_output_buffer_) {
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  while (remaining_subframe_blocks_in_output_buffer_ >= minimum_subframe_decode_count) {
    const uint32_t pre_decode_offset = data.input_buffer_read_offset;
    const uint8_t pre_remaining_subframes = current_frame_remaining_subframes_;

    Decode(&data);
    Consume(&output_rb, &data);

    // 6bef1fb: don't abandon a partially consumed frame. Consume() hands over at most
    // subframe_decode_count blocks per pass so the pass that exhausts the input typically leaves
    // the rest of the frame undelivered. Work() has already cleared is_enabled_ and only
    // XMAEnableContext sets it again, so keep draining until the frame is out.
    const bool frame_done = (mut() == 1) ? true : (current_frame_remaining_subframes_ == 0);
    if ((!data.IsAnyInputBufferValid() || data.error_status == 4) && frame_done) {
      break;
    }

    // If Decode didn't advance the read offset and produced no new frame, we can't make
    // progress. Only check when there were no pending subframes.
    if (pre_remaining_subframes == 0 && data.input_buffer_read_offset == pre_decode_offset &&
        current_frame_remaining_subframes_ == 0) {
      break;
    }
  }

  if (initial_data.IsAnyInputBufferValid()) {
    data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;
  } else {
    // NFS: Carbon and NFS: MW use write_offset as a stall detector: make sure that they are
    // equal if we're starved on input.
    if (data.output_buffer_write_offset != data.output_buffer_read_offset) {
      data.output_buffer_write_offset = data.output_buffer_read_offset;
      data.output_buffer_valid = 0;
    }
  }

  // Signal the game that the output buffer is full.
  if (remaining_subframe_blocks_in_output_buffer_ == 0 && output_rb.empty()) {
    data.output_buffer_valid = 0;
  }

  StoreContextMerged(data, initial_data, context_ptr);
  return true;
}

// xma_context.h:206-215
bool XmaContextNew::Block(bool poll) {
  std::unique_lock<std::mutex> lock(lock_, std::try_to_lock);
  if (!lock.owns_lock()) {
    if (poll) {
      return false;
    }
    lock.lock();
  }
  return true;
}

// xnc:248-255
void XmaContextNew::Clear() {
  std::lock_guard<std::mutex> lock(lock_);
  uint8_t* context_ptr = memory_ ? memory_->TranslateVirtual(guest_ptr()) : nullptr;
  if (!context_ptr) return;
  XMA_CONTEXT_DATA data(context_ptr);
  ClearLocked(&data);
  data.Store(context_ptr);
}

// xnc:257-271
void XmaContextNew::ClearLocked(XMA_CONTEXT_DATA* data) {
  data->input_buffer_0_valid = 0;
  data->input_buffer_1_valid = 0;
  data->output_buffer_valid = 0;

  data->input_buffer_read_offset = kBitsPerPacketHeader;
  data->output_buffer_read_offset = 0;
  data->output_buffer_write_offset = 0;

  current_frame_remaining_subframes_ = 0;
  loop_frame_output_limit_ = 0;
  loop_start_skip_pending_ = false;
}

// xnc:275-283
void XmaContextNew::Release() {
  // Lock it in case the decoder thread is working on it now.
  std::lock_guard<std::mutex> lock(lock_);
  set_is_allocated(false);
  if (uint8_t* context_ptr = memory_ ? memory_->TranslateVirtual(guest_ptr()) : nullptr)
    std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));  // Zero it.
}

// xnc:285-287
int XmaContextNew::GetSampleRate(int id) { return kIdToSampleRate[std::min(id, 3)]; }

// xnc:289-300
void XmaContextNew::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  // No more frames.
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = kBitsPerPacketHeader;
}

// xnc:302-373
void XmaContextNew::Consume(RingBuffer* output_rb, const XMA_CONTEXT_DATA* data) {
  if (!current_frame_remaining_subframes_) {
    return;
  }

  // Check if the loop end truncation limit has been reached.
  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    if (consumed >= loop_frame_output_limit_) {
      // Charge headroom as if the frame completed normally so the Work() loop doesn't
      // overestimate available output space.
      remaining_subframe_blocks_in_output_buffer_ -= data->output_buffer_padding;
      current_frame_remaining_subframes_ = 0;
      loop_frame_output_limit_ = 0;
      return;
    }
  }

  // Guard against subframe_decode_count == 0 (zero progress): treat 0 as 1.
  const uint8_t effective_sdc =
      (uint8_t)std::max(static_cast<uint32_t>(1), data->subframe_decode_count);
  int8_t subframes_to_write =
      std::min((int8_t)current_frame_remaining_subframes_, (int8_t)effective_sdc);

  // Clamp to loop end limit if active.
  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    const int8_t remaining_until_limit = (int8_t)(loop_frame_output_limit_ - consumed);
    if (subframes_to_write > remaining_until_limit) {
      subframes_to_write = remaining_until_limit;
    }
  }

  const int8_t raw_frame_read_offset =
      ((kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo) -
      current_frame_remaining_subframes_;

  output_rb->Write(raw_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset),
                   subframes_to_write * kOutputBytesPerBlock);

  // Reserve extra blocks as headroom; only when the frame is fully consumed.
  const int8_t headroom =
      (current_frame_remaining_subframes_ - subframes_to_write == 0) ? data->output_buffer_padding
                                                                     : 0;

  remaining_subframe_blocks_in_output_buffer_ -= subframes_to_write + headroom;
  current_frame_remaining_subframes_ -= subframes_to_write;
}

// xnc:375-676
void XmaContextNew::Decode(XMA_CONTEXT_DATA* data) {
  // No available data.
  if (!data->IsAnyInputBufferValid()) {
    return;
  }

  if (current_frame_remaining_subframes_ > 0) {
    return;
  }

  if (!data->IsCurrentInputBufferValid()) {
    SwapInputBuffer(data);
    if (!data->IsCurrentInputBufferValid()) {
      return;
    }
  }

  // A valid buffer with 0 packets has no host span; Xenia translates it anyway and then rejects it
  // at GetPacketNumber (-1 -> return, nothing swapped). Keep that: only a buffer that HAS packets
  // but cannot be reached takes the deviation below.
  const bool has_packets = GetCurrentInputBufferSize(data) != 0;
  uint8_t* current_input_buffer = has_packets ? GetCurrentInputBuffer(data) : nullptr;
  if (has_packets && !current_input_buffer) {
    // Deviation: Xenia would fault. Consume the unreachable buffer and move on.
    SwapInputBuffer(data);
    return;
  }

  input_buffer_.fill(0);

  // Detect if we're about to decode the loop end frame (before UpdateLoopStatus may reset the
  // offset).
  bool is_loop_end_frame = false;
  if (data->loop_count > 0) {
    const uint32_t loop_end = std::max(kBitsPerPacketHeader, (uint32_t)data->loop_end);
    is_loop_end_frame = (data->input_buffer_read_offset == loop_end);
  }

  UpdateLoopStatus(data);

  if (!data->output_buffer_block_count) {
    if (logN(g_warnLogs, 8))
      SR_LOG_AUDIO("[XMA] XmaContext %u: Error - Received 0 for output_buffer_block_count!",
                   id());
    return;
  }

  // Games like Dirt 2 can kick the decoder with read offset 0 (pointing into the packet header)
  // before filling in a valid offset. Clamp to the first valid data position.
  if (data->input_buffer_read_offset < kBitsPerPacketHeader) {
    data->input_buffer_read_offset = kBitsPerPacketHeader;
  }

  const uint32_t current_input_size = GetCurrentInputBufferSize(data);
  const uint32_t current_input_packet_count = current_input_size / kBytesPerPacket;

  const int16_t packet_index = GetPacketNumber(current_input_size, data->input_buffer_read_offset);

  if (packet_index == -1) {
    if (logN(g_warnLogs, 8))
      SR_LOG_AUDIO("[XMA] XmaContext %u: Invalid packet index. Input read offset: %u", id(),
                   (uint32_t)data->input_buffer_read_offset);
    return;
  }

  uint8_t* packet = current_input_buffer + (packet_index * kBytesPerPacket);
  const uint32_t packet_first_frame_offset = xma::GetPacketFrameOffset(packet);
  uint32_t relative_offset = data->input_buffer_read_offset % kBitsPerPacket;

  // If the read offset is before the first frame in this packet we're in the tail of a split
  // frame from the previous packet. Skip ahead to the first complete frame.
  if (relative_offset < packet_first_frame_offset) {
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + packet_first_frame_offset;
    relative_offset = packet_first_frame_offset;
  }
  const uint8_t skip_count = xma::GetPacketSkipCount(packet);

  // Full packet skip - no new frames begin in this packet. Advance to the next sequential packet.
  if (skip_count == 0xFF) {
    const uint32_t next_packet_index_skip = packet_index + 1;
    uint32_t next_input_offset =
        GetNextPacketReadOffset(data, next_packet_index_skip, current_input_packet_count);
    if (next_packet_index_skip >= current_input_packet_count ||
        next_input_offset == kBitsPerPacketHeader) {
      SwapInputBuffer(data);
    }
    data->input_buffer_read_offset = next_input_offset;
    return;
  }

  kPacketInfo packet_info = GetPacketInfo(packet, relative_offset);
  const uint32_t packet_to_skip = skip_count + 1;
  const uint32_t next_packet_index = packet_index + packet_to_skip;

  // Frame header split across packet boundary - combine packets to read the full 15-bit header
  // and resolve the real frame size.
  if (packet_info.current_frame_size_ == 0) {
    const uint8_t* next_packet = GetNextPacket(data, next_packet_index, current_input_packet_count);
    if (!next_packet) {
      // Next buffer not available yet: consume (swap) the current buffer and move on.
      SwapInputBuffer(data);
      return;
    }
    std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);
    std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                kBytesPerPacketData);

    BitStream combined(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
    combined.SetOffset(relative_offset - kBitsPerPacketHeader);

    uint64_t frame_size = combined.Peek(kBitsPerFrameHeader);
    if (frame_size == xma::kMaxFrameLength) {
      // Matching split-body error handling below; correct error code unknown.
      data->error_status = 4;
      return;
    }
    packet_info.current_frame_size_ = (uint32_t)frame_size;
  }

  BitStream stream = BitStream(current_input_buffer, (packet_index + 1) * kBitsPerPacket);
  stream.SetOffset(data->input_buffer_read_offset);

  const uint64_t bits_to_copy =
      GetAmountOfBitsToRead((uint32_t)stream.BitsRemaining(), packet_info.current_frame_size_);

  if (bits_to_copy == 0) {
    if (logN(g_warnLogs, 8))
      SR_LOG_AUDIO("[XMA] XmaContext %u: There is no bits to copy!", id());
    SwapInputBuffer(data);
    return;
  }

  if (packet_info.isLastFrameInPacket()) {
    // Frame is a split frame.
    if (stream.BitsRemaining() < packet_info.current_frame_size_) {
      const uint8_t* next_packet =
          GetNextPacket(data, next_packet_index, current_input_packet_count);
      if (!next_packet) {
        // Error path. Decoder probably should return error here; code unknown.
        data->error_status = 4;
        return;
      }
      // Copy next packet to buffer.
      std::memcpy(input_buffer_.data() + kBytesPerPacketData,
                  next_packet + kBytesPerPacketHeader, kBytesPerPacketData);
    }
  }

  // Copy current packet to buffer.
  std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);

  stream = BitStream(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
  stream.SetOffset(relative_offset - kBitsPerPacketHeader);

  xma_frame_.fill(0);

  const uint32_t padding_start =
      static_cast<uint8_t>(stream.Copy(xma_frame_.data() + 1, packet_info.current_frame_size_));

  raw_frame_.fill(0);

  const bool stereo = bool(data->is_stereo);
  const int prep = PrepareDecoder(data->sample_rate, stereo);
  PreparePacket(packet_info.current_frame_size_, padding_start);
  const float* planes[2] = {nullptr, nullptr};
  const bool decoded =
      decoder_ && prep >= 0 &&
      decoder_->Decode(xma_frame_.data(), packet_size_, planes);
  if (decoded) {
    frames_decoded_.fetch_add(1, std::memory_order_relaxed);
    ConvertFrame(planes, stereo, raw_frame_.data());
    current_frame_remaining_subframes_ = 4 << data->is_stereo;

    // Loop end: limit output to subframes 0..loop_subframe_end.
    if (is_loop_end_frame) {
      loop_frame_output_limit_ = (data->loop_subframe_end + 1) << data->is_stereo;
    } else {
      loop_frame_output_limit_ = 0;
    }

    // Loop start: skip leading subframes per loop_subframe_skip.
    if (loop_start_skip_pending_) {
      const uint8_t skip = data->loop_subframe_skip << data->is_stereo;
      if (skip < current_frame_remaining_subframes_) {
        current_frame_remaining_subframes_ -= skip;
      }
      loop_start_skip_pending_ = false;
    }
  } else {
    decode_failures_.fetch_add(1, std::memory_order_relaxed);
  }

  // Compute where to go next.
  if (!packet_info.isLastFrameInPacket()) {
    const uint32_t next_frame_offset =
        (data->input_buffer_read_offset + bits_to_copy) % kBitsPerPacket;
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + next_frame_offset;
    return;
  }

  // (Xenia logs here when the next packet is unavailable; GetNextPacketReadOffset below makes
  // the same availability decision.)
  uint32_t next_input_offset =
      GetNextPacketReadOffset(data, next_packet_index, current_input_packet_count);

  if (next_packet_index >= current_input_packet_count ||
      next_input_offset == kBitsPerPacketHeader) {
    SwapInputBuffer(data);
  }

  if (next_input_offset == kBitsPerPacketHeader) {
    // We're at start of next buffer. If it has any frame in this packet the decoder should go to
    // the first frame in the packet; if it doesn't, it should immediately go to the next packet.
    if (data->IsAnyInputBufferValid()) {
      const uint8_t* next_buf = TranslatePhysical(
          data->GetCurrentInputBufferAddress(),
          std::max<uint32_t>(1, data->GetCurrentInputBufferPacketCount()) * kBytesPerPacket);
      if (!next_buf) {
        SwapInputBuffer(data);
        return;
      }
      next_input_offset = xma::GetPacketFrameOffset(next_buf);
      if (next_input_offset > kMaxFrameSizeinBits) {
        SwapInputBuffer(data);
        return;
      }
    }
  }
  data->input_buffer_read_offset = next_input_offset;
}

// xnc:680-702
void XmaContextNew::UpdateLoopStatus(XMA_CONTEXT_DATA* data) {
  if (data->loop_count == 0) {
    return;
  }
  const uint32_t loop_start = std::max(kBitsPerPacketHeader, (uint32_t)data->loop_start);
  const uint32_t loop_end = std::max(kBitsPerPacketHeader, (uint32_t)data->loop_end);
  if (data->input_buffer_read_offset != loop_end) {
    return;
  }
  data->input_buffer_read_offset = loop_start;
  loop_start_skip_pending_ = true;
  if (data->loop_count != 255) {
    data->loop_count--;
  }
}

// xnc:704-743
kPacketHandle XmaContextNew::GetPacketHandle(XMA_CONTEXT_DATA* data, uint32_t buffer_index,
                                             uint32_t packet_index,
                                             uint32_t current_input_packet_count) {
  kPacketHandle result{};
  bool is_packet_in_next_buffer = packet_index >= current_input_packet_count;
  if (is_packet_in_next_buffer) {
    buffer_index = buffer_index ^ 1;
    packet_index = packet_index - current_input_packet_count;
  }
  if (!data->IsInputBufferValid((uint8_t)buffer_index)) {
    return result;
  }
  const uint32_t next_buffer_address = data->GetInputBufferAddress((uint8_t)buffer_index);
  if (!next_buffer_address) {
    // This should never occur but there is always a chance.
    return result;
  }
  const uint32_t next_buffer_packet_count = data->GetInputBufferPacketCount((uint8_t)buffer_index);
  if (packet_index >= next_buffer_packet_count) {
    return result;
  }
  result.buffer_index_ = buffer_index;
  result.packet_index_ = packet_index;
  result.is_valid_ = true;
  return result;
}

// xnc:745-759
const uint8_t* XmaContextNew::GetNextPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                                            uint32_t current_input_packet_count) {
  kPacketHandle packet_handle =
      GetPacketHandle(data, data->current_buffer, next_packet_index, current_input_packet_count);
  if (!packet_handle.is_valid_) {
    return nullptr;
  }
  const uint32_t next_buffer_address =
      data->GetInputBufferAddress((uint8_t)packet_handle.buffer_index_);
  uint8_t* buf = TranslatePhysical(
      next_buffer_address,
      data->GetInputBufferPacketCount((uint8_t)packet_handle.buffer_index_) * kBytesPerPacket);
  if (!buf) return nullptr;
  return buf + packet_handle.packet_index_ * kBytesPerPacket;
}

// xnc:761-790 (7015473: follow the sub-stream skip chain)
uint32_t XmaContextNew::GetNextPacketReadOffset(uint8_t* buffer, uint32_t next_packet_index,
                                                uint32_t current_input_packet_count) {
  // Scan forward for the next packet that contains a new frame.
  while (next_packet_index < current_input_packet_count) {
    uint8_t* next_packet = buffer + (next_packet_index * kBytesPerPacket);
    const uint32_t packet_frame_offset = xma::GetPacketFrameOffset(next_packet);

    if (packet_frame_offset <= kMaxFrameSizeinBits) {
      const uint32_t new_input_buffer_offset =
          (next_packet_index * kBitsPerPacket) + packet_frame_offset;
      return new_input_buffer_offset;
    }

    if (mut() == 2) {   // pre-7015473 behaviour (mutation check only)
      next_packet_index++;
      continue;
    }
    // No frame *starts* in this packet: it is entirely the continuation of a frame split across
    // the packet boundary. Follow this packet's own skip count to the next packet of the same
    // sub-stream.
    const uint8_t next_skip = xma::GetPacketSkipCount(next_packet);
    if (next_skip == 0xFF) {
      break;
    }
    next_packet_index += next_skip + 1;
  }
  return kBitsPerPacketHeader;
}

// xnc:792-808
uint32_t XmaContextNew::GetNextPacketReadOffset(XMA_CONTEXT_DATA* data,
                                                uint32_t next_packet_index,
                                                uint32_t current_input_packet_count) {
  kPacketHandle packet_handle =
      GetPacketHandle(data, data->current_buffer, next_packet_index, current_input_packet_count);
  if (!packet_handle.is_valid_) {
    return kBitsPerPacketHeader;
  }
  const uint32_t next_buffer_address =
      data->GetInputBufferAddress((uint8_t)packet_handle.buffer_index_);
  const uint32_t count = data->GetInputBufferPacketCount((uint8_t)packet_handle.buffer_index_);
  uint8_t* buf = TranslatePhysical(next_buffer_address, count * kBytesPerPacket);
  if (!buf) return kBitsPerPacketHeader;
  return GetNextPacketReadOffset(buf, packet_handle.packet_index_, count);
}

// xnc:810-813
uint32_t XmaContextNew::GetAmountOfBitsToRead(uint32_t remaining_stream_bits,
                                              uint32_t frame_size) {
  return std::min(remaining_stream_bits, frame_size);
}

// xnc:815-821
uint32_t XmaContextNew::GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data) {
  return data->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
}
uint8_t* XmaContextNew::GetCurrentInputBuffer(XMA_CONTEXT_DATA* data) {
  const uint32_t size = GetCurrentInputBufferSize(data);
  if (!size) return nullptr;   // a valid buffer with 0 packets: GetPacketNumber rejects it too
  return TranslatePhysical(data->GetCurrentInputBufferAddress(), size);
}

// xnc:823-896 (aee0871: count the split-header frame)
kPacketInfo XmaContextNew::GetPacketInfo(uint8_t* packet, uint32_t frame_offset) {
  kPacketInfo packet_info = {};

  const uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);

  // Handling of split frame.
  if (frame_offset < first_frame_offset) {
    packet_info.current_frame_ = 0;
    packet_info.current_frame_size_ = first_frame_offset - frame_offset;
  }

  while (true) {
    if (stream.BitsRemaining() < kBitsPerFrameHeader) {
      // This frame's 15-bit header runs into the next packet, so its size isn't readable yet.
      // Count it anyway, or the caller takes the previous frame for the packet's last and skips
      // straight past this one. Size 0 sends it to the split-header path.
      if (mut() != 3 && stream.BitsRemaining() > 0) {
        if (stream.offset_bits() == frame_offset) {
          packet_info.current_frame_ = packet_info.frame_count_;
          packet_info.current_frame_size_ = 0;
        }
        packet_info.frame_count_++;
      }
      break;
    }

    const uint64_t frame_size = stream.Peek(kBitsPerFrameHeader);
    if (frame_size == 0 || frame_size == xma::kMaxFrameLength) {
      break;
    }

    if (stream.offset_bits() == frame_offset) {
      packet_info.current_frame_ = packet_info.frame_count_;
      packet_info.current_frame_size_ = (uint32_t)frame_size;
    }

    packet_info.frame_count_++;

    if (frame_size > stream.BitsRemaining()) {
      // Last frame.
      break;
    }

    stream.Advance(frame_size - 1);

    // Read the trailing bit to see if frames follow.
    if (stream.Read(1) == 0) {
      break;
    }
  }

  if (xma::IsPacketXma2Type(packet)) {
    const uint8_t xma2_frame_count = xma::GetPacketFrameCount(packet);
    if (xma2_frame_count > packet_info.frame_count_) {
      // Frame header split across packet boundary - trust the XMA2 header count.
      if (packet_info.current_frame_size_ == 0) {
        packet_info.current_frame_ = packet_info.frame_count_;
      }
      packet_info.frame_count_ = xma2_frame_count;
    } else if (xma2_frame_count != packet_info.frame_count_ && logN(g_warnLogs, 8)) {
      SR_LOG_AUDIO("[XMA] XmaContext %u: XMA2 packet header defines different amount of frames "
                   "than internally found! (Header: %u Found: %u)",
                   id(), xma2_frame_count, packet_info.frame_count_);
    }
  }
  return packet_info;
}

// xnc:898-913
int16_t XmaContextNew::GetPacketNumber(size_t size, size_t bit_offset) {
  if (bit_offset < kBitsPerPacketHeader) {
    return -1;
  }
  if (bit_offset >= (size << 3)) {
    return -1;
  }
  size_t byte_offset = bit_offset >> 3;
  size_t packet_number = byte_offset / kBytesPerPacket;
  return (int16_t)packet_number;
}

// xnc:915-941 (the codec re-creation lives in the IXmaFrameDecoder implementation)
int XmaContextNew::PrepareDecoder(int sample_rate, bool is_two_channel) {
  if (!decoder_) return -1;
  sample_rate = GetSampleRate(sample_rate);
  const int channels = is_two_channel ? 2 : 1;
  return decoder_->Prepare(sample_rate, channels);
}

// xnc:943-953
void XmaContextNew::PreparePacket(uint32_t frame_size, uint32_t frame_padding) {
  packet_size_ = static_cast<int>(1 + ((frame_padding + frame_size) / 8) +
                                  (((frame_padding + frame_size) % 8) ? 1 : 0));
  auto padding_end = packet_size_ * 8 - (8 + frame_padding + frame_size);
  xma_frame_[0] = ((frame_padding & 7) << 5) | ((padding_end & 7) << 2);
}

// xnc:980-1012
void XmaContextNew::StoreContextMerged(const XMA_CONTEXT_DATA& data,
                                       const XMA_CONTEXT_DATA& initial_data,
                                       uint8_t* context_ptr) {
  if (mut() == 4) {   // whole-struct store (mutation check only)
    data.Store(context_ptr);
    return;
  }
  XMA_CONTEXT_DATA fresh(context_ptr);

  // DWORD 0: decoder owns loop_count, output_buffer_write_offset. Only clear valid flags the
  // decoder actually consumed (was 1, now 0).
  fresh.loop_count = data.loop_count;
  fresh.output_buffer_write_offset = data.output_buffer_write_offset;
  if (initial_data.input_buffer_0_valid && !data.input_buffer_0_valid) {
    fresh.input_buffer_0_valid = 0;
  }
  if (initial_data.input_buffer_1_valid && !data.input_buffer_1_valid) {
    fresh.input_buffer_1_valid = 0;
  }

  // DWORD 1: decoder conditionally clears output_buffer_valid.
  if (initial_data.output_buffer_valid && !data.output_buffer_valid) {
    fresh.output_buffer_valid = 0;
  }

  // DWORD 2: decoder owns input_buffer_read_offset, error_status.
  fresh.input_buffer_read_offset = data.input_buffer_read_offset;
  fresh.error_status = data.error_status;

  // DWORD 4: decoder owns current_buffer.
  fresh.current_buffer = data.current_buffer;

  // DWORD 9: decoder owns output_buffer_read_offset (reset by ClearLocked).
  fresh.output_buffer_read_offset = data.output_buffer_read_offset;

  fresh.Store(context_ptr);
}

// xma_context.cc:59-134, the XE_ARCH_AMD64 path: _mm_cvtps_epi32 (rounds per MXCSR), saturating
// pack, byte-swap shuffle. MXCSR rounding is pinned to nearest for the duration.
#if defined(_M_X64) || defined(__x86_64__)
__attribute__((target("ssse3")))
#endif
void XmaContextNew::ConvertFrame(const float* const* samples, bool is_two_channel,
                                 uint8_t* output_buffer) {
  constexpr float scale = (1 << 15) - 1;
  auto out = reinterpret_cast<int16_t*>(output_buffer);
#if defined(_M_X64) || defined(__x86_64__)
  if (g_xmaMutation.load(std::memory_order_relaxed) != 5) {
    const unsigned int saved_csr = _mm_getcsr();
    _mm_setcsr(saved_csr & ~0x6000u);   // RC = 00: round to nearest (even)
    const float* in_channel_0 = samples[0];
    const __m128 scale_mm = _mm_set1_ps(scale);
    if (is_two_channel && samples[1] != nullptr) {
      const float* in_channel_1 = samples[1];
      const __m128i shufmask =
          _mm_set_epi8(14, 15, 6, 7, 12, 13, 4, 5, 10, 11, 2, 3, 8, 9, 0, 1);
      for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
        __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
        __m128 in_mm1 = _mm_loadu_ps(&in_channel_1[i]);
        in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
        in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
        __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
        __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
        __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
        out_mm = _mm_shuffle_epi8(out_mm, shufmask);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i * 2]), out_mm);
      }
    } else {
      const __m128i shufmask =
          _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
      for (uint32_t i = 0; i < kSamplesPerFrame; i += 8) {
        __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
        __m128 in_mm1 = _mm_loadu_ps(&in_channel_0[i + 4]);
        in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
        in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
        __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
        __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
        __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
        out_mm = _mm_shuffle_epi8(out_mm, shufmask);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i]), out_mm);
      }
    }
    _mm_setcsr(saved_csr);
    return;
  }
#endif
  // xma_context.cc non-AMD64 path (also mutation 5): clamp, scale, truncate, byte swap.
  uint32_t o = 0;
  for (uint32_t i = 0; i < kSamplesPerFrame; i++) {
    for (uint32_t j = 0; j <= uint32_t(is_two_channel); j++) {
      const float* in = samples[j];
      float scaled_sample = std::min(std::max(in[i], -1.0f), 1.0f) * scale;
      auto sample = static_cast<int16_t>(scaled_sample);
      out[o++] = (int16_t)__builtin_bswap16((uint16_t)sample);
    }
  }
}

}  // namespace sr::kernel::apu
