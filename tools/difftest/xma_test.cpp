// iter-463 audio NATIVE: difftests for the XMA port (kernel/apu) and MmGetPhysicalAddress.
//
// Run: build/difftest.exe --xma-test        (exit code = failed checks; prints "N passed, M failed")
//
//   T1  XMA_CONTEXT_DATA layout vs raw big-endian words (critique A4: error_status = 26:5)
//   T2  xeGetPhysicalAddress / the MmGetPhysicalAddress export through the REAL import glue,
//       gate on and off, against Xenia memory.cc (golden: VA FFCA9000 -> PA 1FCAA000)
//   T3  XmaContextNew state machine with a MOCK frame decoder over synthetic XMA packets whose
//       frames carry an id (frame walking, splits and headers are real; audio is not):
//       frame order, 1-packet-per-buffer (EA geometry) == N-packet buffers, split frame header
//       (aee0871), sub-stream skip chain (7015473), drain before exit (6bef1fb), loops, output
//       ring wrap / full, starvation, consume-only, skip 0xFF, offset clamp, Clear, merged store
//   T4  ConvertFrame rounding / saturation / byte order (critique B4)
//   T5  the device + XMA* exports through the REAL import glue (__imp__XMA*), real FFmpeg, real
//       game packets (archive/v1_audio/xma_offline/disc; explicit SKIP when absent):
//       320 creates + the 321st, hw index arithmetic, synchronous kick on the dedicated worker,
//       dedicated == inline PCM, device == bare-context PCM, PA aliasing between two contexts
//   M   mutation validation: g_xmaMutation 1..5 must each make a T3/T4 check fail.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/apu/xma_context_new.hpp"
#include "kernel/apu/xma_device.hpp"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/mm_phys.hpp"
#include "kernel/stubs.hpp"

PPC_EXTERN_FUNC(__imp__MmGetPhysicalAddress);
PPC_EXTERN_FUNC(__imp__XMACreateContext);
PPC_EXTERN_FUNC(__imp__XMAReleaseContext);
PPC_EXTERN_FUNC(__imp__XMAInitializeContext);
PPC_EXTERN_FUNC(__imp__XMAEnableContext);
PPC_EXTERN_FUNC(__imp__XMADisableContext);
PPC_EXTERN_FUNC(__imp__XMASetInputBuffer0);
PPC_EXTERN_FUNC(__imp__XMASetInputBuffer1);
PPC_EXTERN_FUNC(__imp__XMASetInputBuffer0Valid);
PPC_EXTERN_FUNC(__imp__XMASetInputBuffer1Valid);
PPC_EXTERN_FUNC(__imp__XMAIsInputBuffer0Valid);
PPC_EXTERN_FUNC(__imp__XMAIsInputBuffer1Valid);
PPC_EXTERN_FUNC(__imp__XMASetOutputBufferValid);
PPC_EXTERN_FUNC(__imp__XMAIsOutputBufferValid);
PPC_EXTERN_FUNC(__imp__XMAGetOutputBufferReadOffset);
PPC_EXTERN_FUNC(__imp__XMAGetOutputBufferWriteOffset);
PPC_EXTERN_FUNC(__imp__XMASetOutputBufferReadOffset);

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace K = sr::kernel;
namespace A = sr::kernel::apu;

namespace {

int g_pass = 0, g_fail = 0;
bool g_quiet = false;   // mutation runs: count only
void check(bool ok, const char* name) {
  if (ok) {
    g_pass++;
    if (!g_quiet) std::printf("  ok  %s\n", name);
  } else {
    g_fail++;
    if (!g_quiet) std::printf("FAIL: %s\n", name);
  }
}

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// ── synthetic XMA packets ────────────────────────────────────────────────────────────────────
struct Frame { uint32_t bits; uint16_t id; };

// Frames laid back to back across packets, continuous bitstream (split frames and split headers
// happen where they fall). Frame: 15-bit length, 16-bit id, zero filler, trailing "more frames"
// bit (1 except the last frame). Packet header: XMA1 layout, metadata 0 (like the game's packets).
std::vector<uint8_t> buildPackets(const std::vector<Frame>& frames, uint32_t startPad = 0,
                                  uint8_t skip = 0) {
  const uint32_t kPay = 2044 * 8;
  uint64_t total = startPad;
  for (auto& f : frames) total += f.bits;
  const uint32_t npk = (uint32_t)((total + kPay - 1) / kPay);
  std::vector<uint8_t> out(npk * 2048, 0);
  auto setBit = [&](uint64_t g, int v) {
    const uint32_t pk = (uint32_t)(g / kPay), bit = (uint32_t)(g % kPay) + 32;
    if (v) out[pk * 2048 + bit / 8] |= uint8_t(0x80 >> (bit % 8));
  };
  auto putBits = [&](uint64_t g, uint32_t v, int n) {
    for (int i = 0; i < n; ++i) setBit(g + i, (v >> (n - 1 - i)) & 1);
  };
  std::vector<uint32_t> firstFrame(npk, 0x7FFF);
  uint64_t g = startPad;
  for (size_t i = 0; i < frames.size(); ++i) {
    const uint32_t pk = (uint32_t)(g / kPay);
    if (firstFrame[pk] == 0x7FFF) firstFrame[pk] = (uint32_t)(g % kPay);   // (+32 - 32)
    putBits(g, frames[i].bits, 15);
    putBits(g + 15, frames[i].id, 16);
    setBit(g + frames[i].bits - 1, i + 1 < frames.size() ? 1 : 0);
    g += frames[i].bits;
  }
  for (uint32_t k = 0; k < npk; ++k) {
    const uint32_t v = firstFrame[k];
    uint8_t* h = &out[k * 2048];
    h[0] = uint8_t(((k & 0x3F) << 2) | ((v >> 13) & 3));
    h[1] = uint8_t((v >> 5) & 0xFF);
    h[2] = uint8_t(((v & 0x1F) << 3) | 0);
    h[3] = skip;
  }
  return out;
}

// Mock decoder: reads the frame id; outputs channel c = (id * 4 + c) / 32767 for all 512 samples,
// so the PCM names the frame. Optional hook runs inside Work (the context lock is held).
struct MockDecoder final : A::IXmaFrameDecoder {
  std::vector<float> pl[2] = {std::vector<float>(512), std::vector<float>(512)};
  std::function<void()> hook;
  int Prepare(int, int) override { return 0; }
  bool Decode(const uint8_t* data, int size, const float* planes[2]) override {
    const int pad = data[0] >> 5;
    A::BitStream bs(const_cast<uint8_t*>(data + 1), (size_t)(size - 1) * 8 + 64);
    bs.SetOffset(pad);
    bs.Read(15);
    const uint32_t id = (uint32_t)bs.Read(16);
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < 512; ++i) pl[c][i] = float(id * 4 + c) / 32767.0f;
    planes[0] = pl[0].data();
    planes[1] = pl[1].data();
    if (hook) hook();
    return true;
  }
};

struct FlatMem final : A::IXmaMemory {
  std::vector<uint8_t> m = std::vector<uint8_t>(4u << 20);
  uint8_t* TranslateVirtual(uint32_t va) override { return va + 64 <= m.size() ? &m[va] : nullptr; }
  uint8_t* TranslatePhysical(uint32_t, uint32_t pa, uint32_t bytes) override {
    return (uint64_t)pa + bytes <= m.size() ? &m[pa] : nullptr;
  }
};

constexpr uint32_t kCtx = 0x100, kIn0 = 0x10000, kIn1 = 0x110000, kOut = 0x210000;

struct Run {
  std::vector<int16_t> pcm;   // host-order int16 from the BE ring
  uint64_t kicks = 0;
  A::XMA_CONTEXT_DATA last;
};

// The game's side (EA): refill whichever buffer the decoder consumed, SetOutputBufferValid,
// kick, drain the ring, SetOutputBufferReadOffset(write).
Run drive(const std::vector<uint8_t>& pk, uint32_t perBuf, bool stereo, uint32_t blocks = 24,
          uint32_t sdc = 4, std::function<void(A::XMA_CONTEXT_DATA&)> tweak = nullptr,
          MockDecoder** mockOut = nullptr, int maxKicks = 100000) {
  Run r;
  FlatMem mem;
  A::XmaContextNew ctx;
  auto mock = std::make_unique<MockDecoder>();
  if (mockOut) *mockOut = mock.get();
  ctx.Setup(0, &mem, kCtx, std::move(mock));
  ctx.set_is_allocated(true);
  const uint32_t npk = (uint32_t)(pk.size() / 2048);
  uint32_t next = 0;
  uint8_t* c = mem.TranslateVirtual(kCtx);
  {
    A::XMA_CONTEXT_DATA d;
    d.output_buffer_ptr = kOut;
    d.output_buffer_block_count = blocks;
    d.subframe_decode_count = sdc;
    d.is_stereo = stereo;
    d.sample_rate = 2;
    d.input_buffer_read_offset = 32;
    if (tweak) tweak(d);
    d.Store(c);
  }
  auto feed = [&](int which) {
    if (next >= npk) return;
    const uint32_t n = std::min(perBuf, npk - next);
    const uint32_t addr = which ? kIn1 : kIn0;
    std::memcpy(&mem.m[addr], &pk[next * 2048], n * 2048);
    next += n;
    A::XMA_CONTEXT_DATA d(c);
    if (which) { d.input_buffer_1_ptr = addr; d.input_buffer_1_packet_count = n; d.input_buffer_1_valid = 1; }
    else { d.input_buffer_0_ptr = addr; d.input_buffer_0_packet_count = n; d.input_buffer_0_valid = 1; }
    d.Store(c);
  };
  for (int k = 0; k < maxKicks; ++k) {
    { A::XMA_CONTEXT_DATA d(c); if (!d.input_buffer_0_valid) feed(0); }
    { A::XMA_CONTEXT_DATA d(c); if (!d.input_buffer_1_valid) feed(1); }
    A::XMA_CONTEXT_DATA pre(c);
    if (!pre.IsAnyInputBufferValid() && ctx.current_frame_remaining_subframes() == 0) break;
    pre.output_buffer_valid = 1;
    pre.Store(c);
    ctx.BumpKickSeq();
    ctx.Enable();
    ctx.Work();
    r.kicks++;
    A::XMA_CONTEXT_DATA d(c);
    uint32_t avail = (d.output_buffer_write_offset + blocks - d.output_buffer_read_offset) % blocks;
    if (avail == 0 && !d.output_buffer_valid && pre.IsAnyInputBufferValid()) avail = blocks;
    for (uint32_t b = 0; b < avail; ++b) {
      const uint8_t* blk = &mem.m[kOut + ((d.output_buffer_read_offset + b) % blocks) * 256];
      for (int i = 0; i < 128; ++i) r.pcm.push_back((int16_t)((blk[2 * i] << 8) | blk[2 * i + 1]));
    }
    d.output_buffer_read_offset = d.output_buffer_write_offset;
    d.Store(c);
  }
  r.last = A::XMA_CONTEXT_DATA(c);
  return r;
}

// Frame ids in output order (stereo: L = id*4, R = id*4+1; mono: id*4), one per 512 samples of
// channel 0 when frames are whole.
std::vector<int> idsOf(const std::vector<int16_t>& pcm, bool stereo) {
  std::vector<int> ids;
  const size_t step = stereo ? 2 : 1;
  int cur = -1;
  size_t run = 0;
  for (size_t i = 0; i < pcm.size(); i += step) {
    const int id = pcm[i] / 4;
    if (id != cur || run == 512) {
      ids.push_back(id);
      cur = id;
      run = 0;
    }
    run++;
  }
  return ids;
}

std::vector<Frame> seqFrames(int n, uint32_t bits0, uint32_t stride, int id0 = 1) {
  std::vector<Frame> f;
  for (int i = 0; i < n; ++i) f.push_back({bits0 + (uint32_t)((i * stride) % 3001), (uint16_t)(id0 + i)});
  return f;
}

bool isSeq(const std::vector<int>& ids, int id0, int n) {
  if ((int)ids.size() != n) return false;
  for (int i = 0; i < n; ++i)
    if (ids[i] != id0 + i) return false;
  return true;
}

// ── T3 cases (also the mutation targets) ─────────────────────────────────────────────────────
void t3Cases() {
  // T3a/b: 40 frames of 2000..5000 bits over ~9 packets; stereo.
  const auto frames = seqFrames(40, 2000, 977);
  const auto pk = buildPackets(frames);
  Run big = drive(pk, 64, true);
  check(isSeq(idsOf(big.pcm, true), 1, 40), "T3a all 40 frames decode in order (one buffer, all packets)");
  check(big.pcm.size() == 40u * 512 * 2, "T3a' exactly 40 x 512 stereo samples");
  Run ea = drive(pk, 1, true);
  check(ea.pcm == big.pcm, "T3b EA geometry (1 packet per buffer, in0/in1 refilled) == one big buffer");
  Run two = drive(pk, 2, true);
  check(two.pcm == big.pcm, "T3b' 2 packets per buffer == one big buffer");

  // T3c aee0871: frame headers that straddle packet boundaries. Walk sizes until at least one
  // frame starts in the last 1..14 bits of a packet.
  {
    std::vector<Frame> fr;
    uint64_t g = 0;
    int splitHeaders = 0;
    for (int i = 0; i < 60; ++i) {
      uint32_t bits = 3000 + (uint32_t)((i * 613) % 2500);
      const uint64_t end = g + bits;
      const uint32_t rem = (uint32_t)(2044 * 8 - end % (2044 * 8));
      if (i % 7 == 3 && rem > 20) bits += rem - 7;   // next header starts 7 bits before the end
      fr.push_back({bits, (uint16_t)(100 + i)});
      g += bits;
      const uint32_t r2 = (uint32_t)(2044 * 8 - g % (2044 * 8));
      if (r2 > 0 && r2 < 15) splitHeaders++;
    }
    const auto p2 = buildPackets(fr);
    Run a = drive(p2, 64, true);
    Run b = drive(p2, 1, true);
    check(splitHeaders >= 3, "T3c fixture has >= 3 split frame headers");
    check(isSeq(idsOf(a.pcm, true), 100, 60), "T3c split headers: all 60 frames, in order (aee0871)");
    check(b.pcm == a.pcm, "T3c' same with 1 packet per buffer (split header across in0/in1)");
  }

  // T3d 7015473: two interleaved sub-streams (A,B,A,B..., skip=1). A has a frame longer than a
  // packet, so one A packet is frameless; the next A frame must come from A, not B.
  {
    // Frame 2 starts 352 bits before the end of A0 and runs through ALL of A1 into A2, so A1 is
    // frameless (first-frame offset 0x7FFF): the walk to frame 3 must follow A1's skip count.
    std::vector<Frame> fa = {{16000, 1}, {17500, 2}, {3000, 3}, {3000, 4}, {3000, 5}};
    std::vector<Frame> fb = seqFrames(12, 3000, 311, 500);
    auto pa = buildPackets(fa, 0, 1), pb = buildPackets(fb, 0, 1);
    std::vector<uint8_t> inter;
    const size_t n = std::max(pa.size(), pb.size()) / 2048;
    for (size_t k = 0; k < n; ++k) {
      if (k * 2048 < pa.size()) inter.insert(inter.end(), pa.begin() + k * 2048, pa.begin() + (k + 1) * 2048);
      else inter.insert(inter.end(), 2048, 0);
      if (k * 2048 < pb.size()) inter.insert(inter.end(), pb.begin() + k * 2048, pb.begin() + (k + 1) * 2048);
      else inter.insert(inter.end(), 2048, 0);
    }
    Run a = drive(inter, 64, true, 24, 4, nullptr, nullptr, 200);
    check(idsOf(a.pcm, true) == std::vector<int>({1, 2, 3, 4, 5}),
          "T3d sub-stream skip chain: a frameless packet does not merge stream B into A (7015473)");
  }

  // T3e 6bef1fb: one stereo frame (8 blocks), sdc 4, one kick: the kick must deliver all 8.
  {
    const auto p1 = buildPackets({{5000, 7}});
    FlatMem mem;
    A::XmaContextNew ctx;
    ctx.Setup(0, &mem, kCtx, std::make_unique<MockDecoder>());
    ctx.set_is_allocated(true);
    std::memcpy(&mem.m[kIn0], p1.data(), 2048);
    A::XMA_CONTEXT_DATA d;
    d.output_buffer_ptr = kOut; d.output_buffer_block_count = 24; d.subframe_decode_count = 4;
    d.is_stereo = 1; d.sample_rate = 2; d.input_buffer_read_offset = 32; d.output_buffer_valid = 1;
    d.input_buffer_0_ptr = kIn0; d.input_buffer_0_packet_count = 1; d.input_buffer_0_valid = 1;
    d.Store(mem.TranslateVirtual(kCtx));
    ctx.Enable();
    ctx.Work();
    A::XMA_CONTEXT_DATA e(mem.TranslateVirtual(kCtx));
    check(e.output_buffer_write_offset == 8 && ctx.current_frame_remaining_subframes() == 0,
          "T3e one kick drains the whole last frame (8 blocks) before Work exits (6bef1fb)");
    check(!e.input_buffer_0_valid && e.current_buffer == 1, "T3e' the consumed buffer is released (swap)");
  }

  // T3f loops: loop_count 2 over frames [2..3] of a 5-frame stream -> 1 2 3 2 3 2 3 4 5.
  {
    const auto fr = seqFrames(5, 3000, 0, 1);   // 5 x 3000 bits, all in packet 0
    const auto p = buildPackets(fr);
    const uint32_t f2 = 32 + 3000, f4 = 32 + 3 * 3000;   // frame 2 start, frame 4 start (loop_end)
    Run a = drive(p, 1, true, 24, 4, [&](A::XMA_CONTEXT_DATA& d) {
      d.loop_start = f2; d.loop_end = f4; d.loop_count = 2; d.loop_subframe_end = 7;
    });
    const auto ids = idsOf(a.pcm, true);
    const std::vector<int> want = {1, 2, 3, 2, 3, 2, 3, 4, 5};
    check(ids == want && a.last.loop_count == 0, "T3f loop_count 2: 1 2 3 2 3 2 3 4 5, loop_count -> 0");
    Run inf = drive(p, 1, true, 24, 4, [&](A::XMA_CONTEXT_DATA& d) {
      d.loop_start = f2; d.loop_end = f4; d.loop_count = 255; d.loop_subframe_end = 7;
    }, nullptr, 40);
    check(inf.last.loop_count == 255 && idsOf(inf.pcm, true).size() > 20,
          "T3f' loop_count 255 never decrements (infinite)");
    // loop_subframe_end = 1 on the loop-end frame (stereo: (1+1)<<1 = 4 of 8 blocks) and
    // loop_subframe_skip = 1 at the loop start (skip 2 blocks): frame 3's output is 4 blocks.
    Run tr = drive(p, 1, true, 24, 4, [&](A::XMA_CONTEXT_DATA& d) {
      d.loop_start = f2; d.loop_end = 32 + 2 * 3000 + 0; d.loop_count = 1;
      d.loop_subframe_end = 1; d.loop_subframe_skip = 1;
    });
    // xnc:404-412 + 594-618: at read offset == loop_end the offset jumps back to loop_start
    // BEFORE the packet is read, so the "loop end frame" decoded is frame 2 again, with the
    // loop-start skip (2 blocks) and the loop-end limit (4 blocks): 2 blocks of frame 2 come out.
    // Blocks: 8 (f1) + 8 (f2) + 2 (f2, blocks 2..3) + 8 (f3) + 8 (f4) + 8 (f5) = 42.
    std::vector<int> blk;
    for (size_t i = 0; i + 128 <= tr.pcm.size(); i += 128) blk.push_back(tr.pcm[i] / 4);
    std::vector<int> wantBlk;
    for (int id : {1, 2}) wantBlk.insert(wantBlk.end(), 8, id);
    wantBlk.insert(wantBlk.end(), 2, 2);
    for (int id : {3, 4, 5}) wantBlk.insert(wantBlk.end(), 8, id);
    check(blk == wantBlk && tr.last.loop_count == 0,
          "T3f'' loop_subframe_skip (2 blocks) + loop_subframe_end limit (4) per xma_context_new.cc");
  }

  // T3g output ring: 3 frames (24 blocks) into a 24-block ring: full -> valid 0, write == read.
  {
    const auto p = buildPackets(seqFrames(3, 3000, 0, 1));
    FlatMem mem;
    A::XmaContextNew ctx;
    ctx.Setup(0, &mem, kCtx, std::make_unique<MockDecoder>());
    ctx.set_is_allocated(true);
    std::memcpy(&mem.m[kIn0], p.data(), 2048);
    A::XMA_CONTEXT_DATA d;
    d.output_buffer_ptr = kOut; d.output_buffer_block_count = 24; d.subframe_decode_count = 4;
    d.is_stereo = 1; d.input_buffer_read_offset = 32; d.output_buffer_valid = 1;
    d.output_buffer_read_offset = 5; d.output_buffer_write_offset = 5;   // wraps past 23
    d.input_buffer_0_ptr = kIn0; d.input_buffer_0_packet_count = 1; d.input_buffer_0_valid = 1;
    d.Store(mem.TranslateVirtual(kCtx));
    ctx.Enable();
    ctx.Work();
    A::XMA_CONTEXT_DATA e(mem.TranslateVirtual(kCtx));
    check(e.output_buffer_write_offset == 5 && !e.output_buffer_valid,
          "T3g 24 blocks into a 24-block ring from offset 5: wraps, write == read, valid = 0 (full)");
    check(be32(&mem.m[kOut + 4 * 256]) >> 16 == 12u, "T3g' block 4 (after the wrap) holds frame 3");
  }

  // T3h starvation (NFS rule): no input valid, write != read -> write = read, valid = 0.
  {
    FlatMem mem;
    A::XmaContextNew ctx;
    ctx.Setup(0, &mem, kCtx, std::make_unique<MockDecoder>());
    ctx.set_is_allocated(true);
    A::XMA_CONTEXT_DATA d;
    d.output_buffer_ptr = kOut; d.output_buffer_block_count = 24; d.subframe_decode_count = 4;
    d.input_buffer_0_packet_count = 1; d.output_buffer_valid = 1;
    d.output_buffer_read_offset = 3; d.output_buffer_write_offset = 9;
    d.Store(mem.TranslateVirtual(kCtx));
    ctx.Enable();
    ctx.Work();
    A::XMA_CONTEXT_DATA e(mem.TranslateVirtual(kCtx));
    check(e.output_buffer_write_offset == 3 && !e.output_buffer_valid, "T3h starved: write := read, valid := 0");
    // !output_buffer_valid early return: nothing touched.
    d.output_buffer_valid = 0;
    d.Store(mem.TranslateVirtual(kCtx));
    ctx.Enable();
    ctx.Work();
    A::XMA_CONTEXT_DATA f(mem.TranslateVirtual(kCtx));
    check(f.output_buffer_write_offset == 9, "T3h' output_buffer_valid = 0: Work leaves the context alone");
  }

  // T3p D-AUDIO-CMDBUF open item ("the starved-context rule may make EA read a phantom 6144
  // bytes"): starved on input with write == read and valid = 1. xma_context_new.cc @0bd090dbe
  // (Work: no input valid -> the loop breaks at once; write == read so the NFS rule does
  // nothing; the ring is empty, not full, so valid is NOT cleared): the context is stored back
  // unchanged. A ring with write == read and valid = 1 reads as EMPTY to the game, and the
  // oracle hands EA exactly these bytes - so no phantom 24-block (6144-byte) read can come from
  // our port that would not also come from Xenia.
  {
    FlatMem mem;
    A::XmaContextNew ctx;
    ctx.Setup(0, &mem, kCtx, std::make_unique<MockDecoder>());
    ctx.set_is_allocated(true);
    A::XMA_CONTEXT_DATA d;
    d.output_buffer_ptr = kOut; d.output_buffer_block_count = 24; d.subframe_decode_count = 4;
    d.input_buffer_0_packet_count = 1; d.output_buffer_valid = 1;
    d.output_buffer_read_offset = 7; d.output_buffer_write_offset = 7;
    d.Store(mem.TranslateVirtual(kCtx));
    uint8_t before[64];
    std::memcpy(before, mem.TranslateVirtual(kCtx), 64);
    ctx.Enable();
    ctx.Work();
    A::XMA_CONTEXT_DATA e(mem.TranslateVirtual(kCtx));
    check(e.output_buffer_write_offset == 7 && e.output_buffer_read_offset == 7 && e.output_buffer_valid,
          "T3p starved, write == read, valid 1: write/read unchanged, valid stays 1 (empty ring, oracle)");
    check(std::memcmp(before, mem.TranslateVirtual(kCtx), 64) == 0,
          "T3p' starved empty ring: the 64-byte context is byte-identical after Work (no phantom data)");
    check(ctx.current_frame_remaining_subframes() == 0, "T3p'' no frame was started from no input");
  }

  // T3i skip 0xFF packet, T3j read offset < 32 clamp.
  {
    auto p = buildPackets(seqFrames(6, 3000, 0, 1));
    std::vector<uint8_t> withSkip(2048, 0);
    withSkip[0] = 0x03; withSkip[1] = 0xFF; withSkip[2] = 0xF8; withSkip[3] = 0xFF;   // frameless, skip 0xFF
    withSkip.insert(withSkip.end(), p.begin(), p.end());
    Run a = drive(withSkip, 64, true);
    check(isSeq(idsOf(a.pcm, true), 1, 6), "T3i a leading skip-0xFF packet is stepped over");
    Run b = drive(p, 64, true, 24, 4, [](A::XMA_CONTEXT_DATA& d) { d.input_buffer_read_offset = 0; });
    check(isSeq(idsOf(b.pcm, true), 1, 6), "T3j read offset 0 is clamped to 32 (Dirt 2 rule)");
  }

  // T3k Clear, T3l consume-only, T3m merged store.
  {
    FlatMem mem;
    A::XmaContextNew ctx;
    ctx.Setup(0, &mem, kCtx, std::make_unique<MockDecoder>());
    ctx.set_is_allocated(true);
    A::XMA_CONTEXT_DATA d;
    d.input_buffer_0_valid = 1; d.input_buffer_1_valid = 1; d.output_buffer_valid = 1;
    d.input_buffer_read_offset = 777; d.output_buffer_read_offset = 4; d.output_buffer_write_offset = 9;
    d.loop_count = 3; d.sample_rate = 3; d.input_buffer_0_ptr = 0x1234; d.output_buffer_block_count = 24;
    d.Store(mem.TranslateVirtual(kCtx));
    ctx.Clear();
    A::XMA_CONTEXT_DATA e(mem.TranslateVirtual(kCtx));
    check(!e.input_buffer_0_valid && !e.input_buffer_1_valid && !e.output_buffer_valid &&
              e.input_buffer_read_offset == 32 && e.output_buffer_read_offset == 0 &&
              e.output_buffer_write_offset == 0 && e.loop_count == 3 && e.sample_rate == 3 &&
              e.input_buffer_0_ptr == 0x1234 && e.output_buffer_block_count == 24,
          "T3k Clear: valid bits 0, read 32, out offsets 0; everything else kept");
  }
  {
    // Consume-only with nothing pending: the context must not be touched.
    FlatMem mem;
    A::XmaContextNew ctx;
    ctx.Setup(0, &mem, kCtx, std::make_unique<MockDecoder>());
    ctx.set_is_allocated(true);
    A::XMA_CONTEXT_DATA d;
    d.output_buffer_ptr = kOut; d.output_buffer_block_count = 24; d.output_buffer_valid = 1;
    d.output_buffer_read_offset = 2; d.output_buffer_write_offset = 6;
    d.Store(mem.TranslateVirtual(kCtx));
    ctx.Enable();
    ctx.Work();
    A::XMA_CONTEXT_DATA e(mem.TranslateVirtual(kCtx));
    check(e.output_buffer_write_offset == 6 && e.output_buffer_read_offset == 2 && e.output_buffer_valid,
          "T3l consume-only context with no pending subframes is left alone");
  }
  {
    // Merged store: the game writes in1 (ptr, count, valid) while Work runs; those survive.
    const auto p = buildPackets(seqFrames(2, 3000, 0, 1));
    FlatMem mem;
    A::XmaContextNew ctx;
    auto mock = std::make_unique<MockDecoder>();
    MockDecoder* mk = mock.get();
    ctx.Setup(0, &mem, kCtx, std::move(mock));
    ctx.set_is_allocated(true);
    std::memcpy(&mem.m[kIn0], p.data(), 2048);
    A::XMA_CONTEXT_DATA d;
    d.output_buffer_ptr = kOut; d.output_buffer_block_count = 24; d.subframe_decode_count = 4;
    d.is_stereo = 1; d.input_buffer_read_offset = 32; d.output_buffer_valid = 1;
    d.input_buffer_0_ptr = kIn0; d.input_buffer_0_packet_count = 1; d.input_buffer_0_valid = 1;
    d.Store(mem.TranslateVirtual(kCtx));
    mk->hook = [&] {
      A::XMA_CONTEXT_DATA g(mem.TranslateVirtual(kCtx));
      g.input_buffer_1_ptr = 0x00ABC000; g.input_buffer_1_packet_count = 3; g.input_buffer_1_valid = 1;
      g.Store(mem.TranslateVirtual(kCtx));
      mk->hook = nullptr;
    };
    ctx.Enable();
    ctx.Work();
    A::XMA_CONTEXT_DATA e(mem.TranslateVirtual(kCtx));
    check(e.input_buffer_1_valid && e.input_buffer_1_ptr == 0x00ABC000 && e.input_buffer_1_packet_count == 3,
          "T3m StoreContextMerged keeps the game's concurrent in1 write (ptr, count, valid)");
    check(!e.input_buffer_0_valid, "T3m' and still clears the in0 valid the decoder consumed");
  }
  {
    // A VALID current buffer with 0 packets: Xenia rejects it at GetPacketNumber (-1) and returns
    // without swapping; the buffer stays valid and in1 is not reached on this kick.
    const auto p = buildPackets(seqFrames(2, 3000, 0, 1));
    FlatMem mem;
    A::XmaContextNew ctx;
    ctx.Setup(0, &mem, kCtx, std::make_unique<MockDecoder>());
    ctx.set_is_allocated(true);
    std::memcpy(&mem.m[kIn1], p.data(), 2048);
    A::XMA_CONTEXT_DATA d;
    d.output_buffer_ptr = kOut; d.output_buffer_block_count = 24; d.subframe_decode_count = 4;
    d.is_stereo = 1; d.input_buffer_read_offset = 32; d.output_buffer_valid = 1;
    d.input_buffer_0_ptr = kIn0; d.input_buffer_0_packet_count = 0; d.input_buffer_0_valid = 1;
    d.input_buffer_1_ptr = kIn1; d.input_buffer_1_packet_count = 1; d.input_buffer_1_valid = 1;
    d.Store(mem.TranslateVirtual(kCtx));
    ctx.Enable();
    ctx.Work();
    A::XMA_CONTEXT_DATA e(mem.TranslateVirtual(kCtx));
    check(e.input_buffer_0_valid && e.input_buffer_1_valid && e.current_buffer == 0 &&
              ctx.frames_decoded() == 0,
          "T3n valid 0-packet current buffer: rejected like Xenia (no swap, nothing decoded)");
  }
}

// ── T4 ConvertFrame ───────────────────────────────────────────────────────────────────────────
void t4Convert() {
  std::vector<float> l(512, 0.f), r(512, 0.f);
  const float vals[] = {0.5f / 32767, 1.5f / 32767, 2.5f / 32767, 0.75f / 32767, -0.75f / 32767,
                        1.095f, -1.2f, 1.0f, -1.0f, 0.25f, -0.3333f};
  const int nv = (int)(sizeof(vals) / sizeof(vals[0]));
  for (int i = 0; i < 512; ++i) { l[i] = vals[i % nv]; r[i] = -vals[(i + 3) % nv]; }
  const float* pl[2] = {l.data(), r.data()};
  std::vector<uint8_t> out(4096);
  A::XmaContextNew::ConvertFrame(pl, true, out.data());
  auto expect = [](float x) {
    const float p = x * 32767.0f;
    const double rr = std::nearbyint((double)p);   // default rounding: nearest-even
    return (int16_t)std::max(-32768.0, std::min(32767.0, rr));
  };
  bool ok = true;
  for (int i = 0; i < 512 && ok; ++i) {
    const int16_t gl = (int16_t)((out[4 * i] << 8) | out[4 * i + 1]);
    const int16_t gr = (int16_t)((out[4 * i + 2] << 8) | out[4 * i + 3]);
    ok = gl == expect(l[i]) && gr == expect(r[i]);
  }
  check(ok, "T4a stereo: round-to-nearest-even, saturating, BE, L/R interleaved (AMD64 path)");
  const int16_t sat = (int16_t)((out[4 * 6] << 8) | out[4 * 6 + 1]);   // -1.2 -> -32768
  check(sat == -32768, "T4b -1.2 saturates to -32768 (SIMD pack, not the scalar clamp's -32767)");
  A::XmaContextNew::ConvertFrame(pl, false, out.data());
  bool okm = true;
  for (int i = 0; i < 512 && okm; ++i)
    okm = (int16_t)((out[2 * i] << 8) | out[2 * i + 1]) == expect(l[i]);
  check(okm, "T4c mono path");
}

// ── T1 layout ─────────────────────────────────────────────────────────────────────────────────
void t1Layout() {
  A::XMA_CONTEXT_DATA d;
  d.error_status = 4;
  uint8_t raw[64];
  d.Store(raw);
  check(be32(raw + 8) == (4u << 26), "T1a error_status = 4 -> DW2 == 4 << 26 (26:5, critique A4)");
  struct F { const char* n; int dw; int lo; int w; std::function<void(A::XMA_CONTEXT_DATA&, uint32_t)> set;
             std::function<uint32_t(const A::XMA_CONTEXT_DATA&)> get; };
#define FLD(dw, lo, w, name) {#name, dw, lo, w, [](A::XMA_CONTEXT_DATA& x, uint32_t v) { x.name = v; }, \
                               [](const A::XMA_CONTEXT_DATA& x) { return (uint32_t)x.name; }}
  const F fields[] = {
      FLD(0, 0, 12, input_buffer_0_packet_count), FLD(0, 12, 8, loop_count),
      FLD(0, 20, 1, input_buffer_0_valid), FLD(0, 21, 1, input_buffer_1_valid),
      FLD(0, 22, 5, output_buffer_block_count), FLD(0, 27, 5, output_buffer_write_offset),
      FLD(1, 0, 12, input_buffer_1_packet_count), FLD(1, 12, 2, loop_subframe_start),
      FLD(1, 14, 3, loop_subframe_end), FLD(1, 17, 3, loop_subframe_skip),
      FLD(1, 20, 4, subframe_decode_count), FLD(1, 24, 3, output_buffer_padding),
      FLD(1, 27, 2, sample_rate), FLD(1, 29, 1, is_stereo), FLD(1, 31, 1, output_buffer_valid),
      FLD(2, 0, 26, input_buffer_read_offset), FLD(2, 26, 5, error_status), FLD(2, 31, 1, error_set),
      FLD(3, 0, 26, loop_start), FLD(4, 0, 26, loop_end), FLD(4, 26, 5, packet_metadata),
      FLD(4, 31, 1, current_buffer), FLD(5, 0, 32, input_buffer_0_ptr),
      FLD(6, 0, 32, input_buffer_1_ptr), FLD(7, 0, 32, output_buffer_ptr),
      FLD(9, 0, 5, output_buffer_read_offset), FLD(9, 30, 1, stop_when_done),
      FLD(9, 31, 1, interrupt_when_done)};
#undef FLD
  bool all = true;
  for (const auto& f : fields) {
    A::XMA_CONTEXT_DATA x;
    const uint32_t mx = f.w == 32 ? 0xFFFFFFFFu : ((1u << f.w) - 1);
    f.set(x, mx);
    uint8_t b[64];
    x.Store(b);
    bool ok = true;
    for (int w = 0; w < 16; ++w) {
      const uint32_t want = (w == f.dw) ? (f.w == 32 ? mx : (mx << f.lo)) : 0u;
      ok = ok && be32(b + 4 * w) == want;
    }
    A::XMA_CONTEXT_DATA y(b);
    ok = ok && f.get(y) == mx;
    if (!ok) std::printf("      field %s misplaced\n", f.n);
    all = all && ok;
  }
  check(all, "T1b every DW0-DW9 field sits at xma_context.h's bit position and round-trips");
  // The Xenia oracle's own context bytes (golden sound_gameplay ctx001) decode to sane values.
}

// ── T2 MmGetPhysicalAddress ───────────────────────────────────────────────────────────────────
void t2Phys(uint8_t* base) {
  struct C { uint32_t va, pa; };
  const C cases[] = {{0x00001000, 0x00001000}, {0x1FFFFFFE, 0x1FFFFFFE}, {0x30000000, 0x30000000},
                     {0x40001000, 0x00001000}, {0x7F001000, 0x00001000}, {0x7FC80000, UINT32_MAX},
                     {0x7FEA1800, UINT32_MAX}, {0x82000000, 0x02000000}, {0x9A000000, 0x0A000000},
                     {0xA0010000, 0x00010000}, {0xBFFFFFFF, 0x1FFFFFFF}, {0xC1000000, 0x01000000},
                     {0xE0000000, 0x00001000}, {0xFFCA9000, 0x1FCAA000}, {0xFFCFFFFF, 0x1FD00FFF},
                     {0xFFD00000, UINT32_MAX}};
  bool ok = true;
  for (const auto& c : cases) {
    const uint32_t got = K::xeGetPhysicalAddress(c.va);
    if (got != c.pa) {
      std::printf("      va %08X -> %08X (want %08X)\n", c.va, got, c.pa);
      ok = false;
    }
  }
  check(ok, "T2a xeGetPhysicalAddress == Xenia memory.cc for every heap (+0x1000 for 0xE; none -> UINT32_MAX)");
  check(K::xeGetPhysicalAddress(0xFFCA9000u) == 0x1FCAA000u,
        "T2b oracle witness: golden context array VA FFCA9000 has PA 1FCAA000 (findings.json)");

  auto viaGlue = [&](uint32_t va) {
    PPCContext c{};
    c.r1.u32 = 0x705FFF00u;
    c.r3.u32 = va;
    __imp__MmGetPhysicalAddress(c, base);
    return c.r3.u32;
  };
  // The glue caches the resolved export per name on its FIRST call (lookupCached), exactly as in
  // the game where registration precedes every call: so the gate-off world is checked through the
  // registry, and the glue is first called after the gate-on registration.
  K::mmPhysAddrSetEnabledForTest(0);
  K::registerStubs();   // the generic stub (gate off)
  K::registerMmPhysExport(K::ExportRegistry::instance());   // no-op with the gate off
  auto viaReg = [](uint32_t va) {
    return K::ExportRegistry::instance().getExport("MmGetPhysicalAddress")(va, 0, 0, 0, 0, 0);
  };
  check(viaReg(0xA0010000u) == 0 && viaReg(0xE0000000u) == 0, "T2c SR_MM_PHYSADDR=0: the generic stub stays -> 0");
  K::mmPhysAddrSetEnabledForTest(1);
  K::registerMmPhysExport(K::ExportRegistry::instance());
  check(viaGlue(0xA0010000u) == 0x10000u && viaGlue(0xE0000000u) == 0x1000u && viaGlue(0x82000000u) == 0x02000000u,
        "T2d SR_MM_PHYSADDR=1: glue -> Xenia GetPhysicalAddress");
  check(viaGlue(0x7FC80000u) == 0u, "T2e no heap: export maps UINT32_MAX to 0 (xboxkrnl_memory.cc:650)");
  K::mmPhysAddrSetEnabledForTest(-1);
}

// ── T5 device + exports through the real glue ────────────────────────────────────────────────
struct GuestMem final : A::IXmaMemory {
  uint8_t* TranslateVirtual(uint32_t va) override { return K::guestVAToHost(va); }
  uint8_t* TranslatePhysical(uint32_t, uint32_t, uint32_t) override { return nullptr; }
};

uint32_t glue(void (*fn)(PPCContext&, uint8_t*), uint8_t* base, uint32_t r3, uint32_t r4 = 0,
              uint32_t r5 = 0) {
  PPCContext c{};
  c.r1.u32 = 0x705FFF00u;
  c.r3.u32 = r3; c.r4.u32 = r4; c.r5.u32 = r5;
  fn(c, base);
  return c.r3.u32;
}

bool readFile(const std::string& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

// Decode a packet stream through the EXPORTS the way EA does; returns BE PCM bytes.
std::vector<uint8_t> exportDecode(uint8_t* base, const std::vector<uint8_t>& pk, uint32_t ctxVA,
                                  uint32_t in0, uint32_t in1, uint32_t out, uint32_t initVA,
                                  uint64_t* kicksOut = nullptr) {
  std::vector<uint8_t> pcm;
  const uint32_t npk = (uint32_t)(pk.size() / 2048);
  uint32_t next = 0;
  // XMAInitializeContext: in0 = first packet, in1 = 0, out = 24 blocks, sdc 4, 2 ch, 44.1k.
  auto w = [&](uint32_t off, uint32_t v) { K::writeGuestBE32(initVA + off, v); };
  std::memcpy(K::guestVAToHost(in0), &pk[0], 2048);
  next = 1;
  for (uint32_t o = 0; o < 56; o += 4) w(o, 0);
  w(0x00, in0); w(0x04, 1); w(0x10, 32); w(0x14, out); w(0x18, 24); w(0x20, 4); w(0x24, 1); w(0x28, 2);
  uint32_t rc = glue(__imp__XMAInitializeContext, base, ctxVA, initVA);
  if (rc != 0) return pcm;
  glue(__imp__XMASetInputBuffer0Valid, base, ctxVA);
  uint64_t kicks = 0;
  for (int guard = 0; guard < 100000; ++guard) {
    for (int b = 0; b < 2; ++b) {
      const bool valid = glue(b ? __imp__XMAIsInputBuffer1Valid : __imp__XMAIsInputBuffer0Valid, base, ctxVA);
      if (!valid && next < npk) {
        const uint32_t addr = b ? in1 : in0;
        std::memcpy(K::guestVAToHost(addr), &pk[next * 2048], 2048);
        next++;
        glue(b ? __imp__XMASetInputBuffer1 : __imp__XMASetInputBuffer0, base, ctxVA, addr, 1);
        glue(b ? __imp__XMASetInputBuffer1Valid : __imp__XMASetInputBuffer0Valid, base, ctxVA);
      }
    }
    const bool any = glue(__imp__XMAIsInputBuffer0Valid, base, ctxVA) || glue(__imp__XMAIsInputBuffer1Valid, base, ctxVA);
    A::XmaContextNew* cx = A::XmaDevice::instance().context((ctxVA - A::XmaDevice::instance().contextArrayVA()) / 64);
    if (!any && cx->current_frame_remaining_subframes() == 0) break;
    glue(__imp__XMASetOutputBufferValid, base, ctxVA);
    glue(__imp__XMAEnableContext, base, ctxVA);   // synchronous kick
    kicks++;
    const uint32_t wr = glue(__imp__XMAGetOutputBufferWriteOffset, base, ctxVA);
    const uint32_t rd = glue(__imp__XMAGetOutputBufferReadOffset, base, ctxVA);
    const bool valid = glue(__imp__XMAIsOutputBufferValid, base, ctxVA);
    uint32_t avail = (wr + 24 - rd) % 24;
    if (avail == 0 && !valid && any) avail = 24;
    for (uint32_t b = 0; b < avail; ++b) {
      const uint8_t* blk = K::guestVAToHost(out + ((rd + b) % 24) * 256);
      pcm.insert(pcm.end(), blk, blk + 256);
    }
    glue(__imp__XMASetOutputBufferReadOffset, base, ctxVA, wr);
  }
  if (kicksOut) *kicksOut = kicks;
  return pcm;
}

std::vector<uint8_t> bareDecode(const std::vector<uint8_t>& pk) {
  // The same EA-geometry drive over a bare XmaContextNew + real FFmpeg (as tools/xma_offline).
  FlatMem mem;
  mem.m.resize(8u << 20);
  A::XmaContextNew ctx;
  ctx.Setup(0, &mem, kCtx, A::makeFfmpegXmaFrameDecoder());
  ctx.set_is_allocated(true);
  uint8_t* c = mem.TranslateVirtual(kCtx);
  A::XMA_CONTEXT_DATA d;
  d.output_buffer_ptr = kOut; d.output_buffer_block_count = 24; d.subframe_decode_count = 4;
  d.is_stereo = 1; d.sample_rate = 2; d.input_buffer_read_offset = 32;
  d.input_buffer_0_ptr = kIn0; d.input_buffer_0_packet_count = 1; d.input_buffer_0_valid = 1;
  std::memcpy(&mem.m[kIn0], &pk[0], 2048);
  d.Store(c);
  const uint32_t npk = (uint32_t)(pk.size() / 2048);
  uint32_t next = 1;
  std::vector<uint8_t> pcm;
  for (int guard = 0; guard < 100000; ++guard) {
    for (int b = 0; b < 2; ++b) {
      A::XMA_CONTEXT_DATA e(c);
      if (!e.IsInputBufferValid((uint8_t)b) && next < npk) {
        const uint32_t addr = b ? kIn1 : kIn0;
        std::memcpy(&mem.m[addr], &pk[next * 2048], 2048);
        next++;
        if (b) { e.input_buffer_1_ptr = addr; e.input_buffer_1_packet_count = 1; e.input_buffer_1_valid = 1; }
        else { e.input_buffer_0_ptr = addr; e.input_buffer_0_packet_count = 1; e.input_buffer_0_valid = 1; }
        e.Store(c);
      }
    }
    A::XMA_CONTEXT_DATA pre(c);
    if (!pre.IsAnyInputBufferValid() && ctx.current_frame_remaining_subframes() == 0) break;
    pre.output_buffer_valid = 1;
    pre.Store(c);
    ctx.BumpKickSeq();
    ctx.Enable();
    ctx.Work();
    A::XMA_CONTEXT_DATA e(c);
    uint32_t avail = (e.output_buffer_write_offset + 24 - e.output_buffer_read_offset) % 24;
    if (avail == 0 && !e.output_buffer_valid && pre.IsAnyInputBufferValid()) avail = 24;
    for (uint32_t b = 0; b < avail; ++b) {
      const uint8_t* blk = &mem.m[kOut + ((e.output_buffer_read_offset + b) % 24) * 256];
      pcm.insert(pcm.end(), blk, blk + 256);
    }
    e.output_buffer_read_offset = e.output_buffer_write_offset;
    e.Store(c);
  }
  return pcm;
}

void t5Device(uint8_t* base) {
  auto& dev = A::XmaDevice::instance();
  A::xmaSetEnabledForTest(1, 1);
  K::registerStubs();
  A::registerXmaExports(K::ExportRegistry::instance());

  // Real init (guestPhysAlloc) through the first XMACreateContext.
  const uint32_t outCell = 0x70400000u;
  std::vector<uint32_t> ctxs;
  bool allOk = true;
  for (int i = 0; i < 320; ++i) {
    const uint32_t rc = glue(__imp__XMACreateContext, base, outCell);
    const uint32_t va = K::readGuestU32BE(outCell);
    allOk = allOk && rc == 0 && va != 0;
    ctxs.push_back(va);
  }
  check(allOk && dev.contextArrayVA() && ctxs[0] == dev.contextArrayVA() && ctxs[319] == dev.contextArrayVA() + 319 * 64,
        "T5a 320 XMACreateContext through the glue: the array's 64-byte slots, lowest index first");
  const uint32_t rc321 = glue(__imp__XMACreateContext, base, outCell);
  check(rc321 == A::kStatusNoMemory && K::readGuestU32BE(outCell) == 0,
        "T5b the 321st -> X_STATUS_NO_MEMORY (C0000017), *out = 0");
  bool hw = true;
  for (int i = 0; i < 320; ++i)
    hw = hw && (K::xeGetPhysicalAddress(ctxs[i]) - dev.contextArrayPA()) / 64 == (uint32_t)i;
  check(hw, "T5c hw index = (GetPhysicalAddress(ctx) - reg 0x600 PA) / 64 == slot, all 320");
  // Gate off: the glue keeps the old bypass (r3 = 0, no kernel call, the device never sees it).
  {
    const uint64_t rel0 = dev.stats().releases;
    A::xmaSetEnabledForTest(0, 1);
    const uint32_t r = glue(__imp__XMAReleaseContext, base, ctxs[5]);
    A::xmaSetEnabledForTest(1, 1);
    check(r == 0 && dev.stats().releases == rel0,
          "T5d SR_XMA_NEW=0: XMAReleaseContext glue keeps the bypass (r3=0, device untouched)");
  }
  glue(__imp__XMAReleaseContext, base, ctxs[5]);
  check(glue(__imp__XMACreateContext, base, outCell) == 0 && K::readGuestU32BE(outCell) == ctxs[5],
        "T5d' SR_XMA_NEW=1: XMAReleaseContext reaches the device; the slot is reused");
  for (int i = 8; i < 320; ++i) glue(__imp__XMAReleaseContext, base, ctxs[i]);

  // Real packets.
  std::vector<uint8_t> pk;
  const char* fxEnv = std::getenv("SR_XMA_FIXTURES");  // tools/ea_snr_xma.py --out (default below)
  const std::string fxDir = fxEnv ? fxEnv : "archive/v1_audio/xma_offline/disc";
  const std::string fx = fxDir + "/s0001_00009290.xma";
  if (!readFile(fx, pk) || pk.size() < 4096) {
    std::printf("  SKIP T5e-T5h: no fixture %s (run tools/ea_snr_xma.py)\n", fx.c_str());
    return;
  }
  const uint32_t in0 = K::guestPhysAlloc(2048, 0x1000), in1 = K::guestPhysAlloc(2048, 0x1000);
  const uint32_t out = K::guestPhysAlloc(24 * 256, 0x1000), init = 0x70401000u;
  uint64_t kicks = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const std::vector<uint8_t> viaDev = exportDecode(base, pk, ctxs[0], in0, in1, out, init, &kicks);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const std::vector<uint8_t> bare = bareDecode(pk);
  const A::XmaStats st = dev.stats();
  std::printf("      device decode: %zu bytes PCM, %llu kicks, %.1f ms, timeouts=%llu phys_miss=%llu\n",
              viaDev.size(), (unsigned long long)kicks, ms, (unsigned long long)st.timeouts,
              (unsigned long long)st.physMisses);
  check(viaDev.size() >= 220500u * 4 && st.timeouts == 0 && st.physMisses == 0,
        "T5e 5 s of real game XMA (s0001) through the exports on the dedicated worker: no timeout, no miss");
  check(viaDev == bare, "T5f device (exports + side table + worker kicks) PCM == bare XmaContextNew PCM, bit-exact");
  // iter-467 N6: the native front end (SR_XMA_NATIVE_FRONT) - the same decode, and no
  // register-file write at all; the register path (above) is the mutation control.
  {
    A::xmaNativeFrontSetForTest(0);   // the register path (oracle), pinned: the native front is the default
    const uint64_t regBefore = dev.regfileWrites();
    const std::vector<uint8_t> regAgain = exportDecode(base, pk, ctxs[1], in0, in1, out, init);   // fresh contexts: a reused one keeps decoder state
    const uint64_t regPath = dev.regfileWrites() - regBefore;
    A::xmaNativeFrontSetForTest(1);
    const uint64_t natBefore = dev.regfileWrites();
    uint64_t natKicks = 0;
    const std::vector<uint8_t> viaNative = exportDecode(base, pk, ctxs[2], in0, in1, out, init, &natKicks);
    const uint64_t natPath = dev.regfileWrites() - natBefore;
    A::xmaNativeFrontSetForTest(-1);
    std::printf("      native front: %zu bytes PCM, %llu kicks, register-file writes %llu (register path %llu)\n",
                viaNative.size(), (unsigned long long)natKicks, (unsigned long long)natPath,
                (unsigned long long)regPath);
    check(regAgain == bare && regPath > 0, "T5n' register path (oracle) writes the register file and decodes bit-exact");
    check(viaNative == bare && natPath == 0 && natKicks > 0,
          "T5n SR_XMA_NATIVE_FRONT: exports kick/lock/clear by context - 0 register-file writes, PCM bit-exact");
  }

  // Inline mode (use_dedicated_xma_thread = false): Work on the kicking thread.
  {
    GuestMem gm;
    const uint32_t arr = dev.contextArrayVA();
    A::xmaSetEnabledForTest(1, 0);
    dev.initForTest(&gm, arr, K::xeGetPhysicalAddress(arr), &A::makeFfmpegXmaFrameDecoder, false);
    for (int i = 0; i < 2; ++i) glue(__imp__XMACreateContext, base, outCell);
    const std::vector<uint8_t> inl = exportDecode(base, pk, arr, in0, in1, out, init);
    check(inl == bare, "T5g SR_XMA_DEDICATED_THREAD=0 (inline Work) PCM == dedicated-worker PCM");

    // PA aliasing: two contexts whose buffers share a Xenia PA (0x8A.. and 0xAA.. -> 0x0A..).
    dev.initForTest(&gm, arr, K::xeGetPhysicalAddress(arr), &A::makeFfmpegXmaFrameDecoder, true);
    glue(__imp__XMACreateContext, base, outCell);
    const uint32_t cA = K::readGuestU32BE(outCell);
    glue(__imp__XMACreateContext, base, outCell);
    const uint32_t cB = K::readGuestU32BE(outCell);
    std::vector<uint8_t> pkB;
    readFile(fxDir + "/s0002_00026320.xma", pkB);
    const std::vector<uint8_t> a = exportDecode(base, pk, cA, 0x8A100000u, 0x8A101000u, 0x8A102000u, init);
    const uint64_t aliasBefore = dev.stats().physAliases;
    const std::vector<uint8_t> b = pkB.size() >= 4096
        ? exportDecode(base, pkB, cB, 0xAA100000u, 0xAA101000u, 0xAA102000u, init) : std::vector<uint8_t>();
    const std::vector<uint8_t> bBare = pkB.size() >= 4096 ? bareDecode(pkB) : std::vector<uint8_t>();
    check(a == bare && !b.empty() && b == bBare && dev.stats().physAliases > aliasBefore,
          "T5h two contexts on aliasing PAs (8A.. vs AA..) each decode their OWN buffers; alias logged");
    // Side table beats the memo: B noted last (memo -> AA..), yet context A still maps its PA to 8A..
    const uint32_t idA = (cA - arr) / 64, idB = (cB - arr) / 64;
    const uint32_t pa = K::xeGetPhysicalAddress(0x8A102000u);
    check(dev.TranslatePhysical(idA, pa, 24 * 256) == K::guestVAToHost(0x8A102000u) &&
              dev.TranslatePhysical(idB, pa, 24 * 256) == K::guestVAToHost(0xAA102000u) &&
              dev.TranslatePhysical(7, pa, 24 * 256) == K::guestVAToHost(0xAA102000u),
          "T5i same PA -> each context's own VA (side table); an unrelated context gets the memo's latest");
    dev.shutdownForTest();
  }
  A::xmaSetEnabledForTest(-1, -1);
}

}  // namespace

int runXmaTest() {
  std::printf("[XMA_TEST] native XMA port (xma_decoder=new, oracle 0bd090dbe) + MmGetPhysicalAddress\n");
  uint8_t* base = K::getGuestBase();
  t1Layout();
  t2Phys(base);
  t3Cases();
  t4Convert();
  t5Device(base);

  // Mutation validation: each seam must make at least one T3/T4 check fail.
  const int basePass = g_pass, baseFail = g_fail;
  std::printf("  mutation validation (each must FAIL >= 1 check):\n");
  const char* names[] = {"", "drop 6bef1fb drain", "drop 7015473 skip chain", "drop aee0871 split header",
                         "whole-struct store", "truncating ConvertFrame"};
  int mutOk = 0;
  for (int m = 1; m <= 5; ++m) {
    const int p0 = g_pass, f0 = g_fail;
    g_quiet = true;
    A::g_xmaMutation.store(m);
    t3Cases();
    t4Convert();
    A::g_xmaMutation.store(0);
    g_quiet = false;
    const int caught = g_fail - f0;
    std::printf("    M%d %-26s -> %d check(s) failed\n", m, names[m], caught);
    mutOk += caught > 0 ? 1 : 0;
    g_pass = p0;
    g_fail = f0;
  }
  g_pass = basePass;
  g_fail = baseFail;
  check(mutOk == 5, "M all 5 mutations are caught by the suite");
  std::printf("[XMA_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
