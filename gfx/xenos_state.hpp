#pragma once

#include "record_engine.hpp"   // XenosRegisterFile, getXenosRegisters() (the device-state shadow; C1 stage 2a)
#include "render_draw_command.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace sr {
namespace gfx {

// ── iter-450UE: TEXTURE-FETCH SWIZZLE → HOST COMPONENT MAPPING ──────────────
// The guest names, per texture binding, which stored channel feeds each
// sampled component. We never decoded it (only VERTEX fetch swizzle), so every
// sampled component came from the host format's default. For an alpha-only
// k_8 font atlas that means sampled .a reads the R8_UNORM format constant 1.0
// instead of the stored coverage — opaque boxes where glyphs belong.
//
// Structure is the reference's, not invented: xenia-canary
// TextureCache::GuestToHostSwizzle (texture_cache.cc:300) composed with the
// per-format host swizzle column (d3d12_texture_cache.h:196), fed to
// Shader4ComponentMapping (d3d12_texture_cache.cc:1219-1221).
namespace texswz {

// Xenos selector alphabet (xenos.h:1059-1075). ⚠ LOAD-BEARING COINCIDENCE:
// D3D12_SHADER_COMPONENT_MAPPING uses THE SAME alphabet — 0-3 select a source
// component, 4 forces 0.0, 5 forces 1.0 — which is why the composed value goes
// straight into Shader4ComponentMapping with only the always-set bit ORed in.
// Do NOT insert a translation step; there isn't one in the reference either.
enum : uint32_t {
  kSelR = 0, kSelG = 1, kSelB = 2, kSelA = 3, kSelZero = 4, kSelOne = 5,
};

// Four 3-bit selectors, X in the low bits (XE_GPU_MAKE_TEXTURE_SWIZZLE).
constexpr uint32_t makeSwizzle(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
  return (x & 7u) | ((y & 7u) << 3) | ((z & 7u) << 6) | ((w & 7u) << 9);
}

constexpr uint32_t kSwizzleRGBA = makeSwizzle(kSelR, kSelG, kSelB, kSelA);
constexpr uint32_t kSwizzleRRRR = makeSwizzle(kSelR, kSelR, kSelR, kSelR);
constexpr uint32_t kSwizzleRGGG = makeSwizzle(kSelR, kSelG, kSelG, kSelG);

// The 12-bit swizzle field of a TEXTURE fetch constant: dword_3 bits 1-12
// (xenos.h:1299 — num_format at bit 0, swizzle immediately above it).
constexpr uint32_t guestSwizzleFromFetchDword3(uint32_t d3) {
  return (d3 >> 1) & 0xFFFu;
}

// EXACTLY xenia-canary TextureCache::GuestToHostSwizzle (texture_cache.cc:300).
// For each destination component: a CONSTANT selector passes through, and a
// channel selector INDEXES the host format swizzle — i.e. the host table
// answers "which host component actually stores guest channel g".
constexpr uint32_t guestToHostSwizzle(uint32_t guestSwizzle,
                                      uint32_t hostFormatSwizzle) {
  uint32_t out = 0;
  for (uint32_t i = 0; i < 4u; i++) {
    const uint32_t g = (guestSwizzle >> (3u * i)) & 7u;
    // 6 and 7 are undefined on hardware. The reference normalises them to
    // 4/5 with `& 0b101` rather than handing a host GPU an invalid mapping.
    const uint32_t h = (g >= kSelZero) ? (g & 5u)
                                       : ((hostFormatSwizzle >> (3u * g)) & 7u);
    out |= h << (3u * i);
  }
  return out;
}

// D3D12 requires bit 12 set on every Shader4ComponentMapping
// (D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES).
// Spelled numerically so this header stays free of d3d12.h and stays testable.
constexpr uint32_t kD3D12AlwaysSetBit = 1u << 12;

constexpr uint32_t toD3D12ComponentMapping(uint32_t hostSwizzle) {
  return (hostSwizzle & 0xFFFu) | kD3D12AlwaysSetBit;
}

// The identity mapping D3D12 uses by default, for "is this a no-op" tests.
constexpr uint32_t kD3D12DefaultMapping = toD3D12ComponentMapping(kSwizzleRGBA);

// ⚠ THE PER-FORMAT HOST SWIZZLE TABLE IS NOT HERE — it is keyed on DXGI_FORMAT
// in d3d12_backend.cpp, deliberately. The reference keys its table on the GUEST
// format, but our tree has TWO guest→DXGI switches that disagree (Xenos fmt 29
// is R16G16B16A16_FLOAT at d3d12_backend.cpp:2956 and _SNORM on the xlat path),
// so a guest-keyed table would describe a format we did not bind for whichever
// path disagrees. Keying on the DXGI format we ACTUALLY created always
// describes the resource in hand. The values are still lifted from the
// reference's column, not invented.

// ── THE ARITHMETIC TESTS ────────────────────────────────────────────────────
// Compile-time, so they cannot be skipped, cannot rot, and cost nothing at
// runtime: if the composition ever changes behaviour, the BUILD fails. A
// runtime PASS/FAIL echo of the same cases lives behind SR_HLE_TEX_SWIZZLE_TEST
// (d3d12_backend.cpp) so a verifier has a re-runnable command too.
static_assert(kSwizzleRGBA == 0x688u, "RGBA packs X in the low bits");
static_assert(kSwizzleRRRR == 0x000u, "RRRR is all-zero selectors");
static_assert(kSwizzleRGGG == 0x248u, "RGGG");

// 1. PRESS START: guest 000R over a single-channel host format. The atlas is
//    alpha-only, so sampled .a must come from the stored R8 channel.
static_assert(guestSwizzleFromFetchDword3(0x01280248u) == 0x124u,
              "the measured PRESS START fetch decodes to 000R");
static_assert(guestToHostSwizzle(0x124u, kSwizzleRRRR) == 0x124u,
              "000R over RRRR stays (ZERO,ZERO,ZERO,R)");
static_assert(toD3D12ComponentMapping(guestToHostSwizzle(0x124u, kSwizzleRRRR))
                  == (0x124u | 0x1000u),
              "and reaches D3D12 as (4,4,4,0) with the always-set bit");

// 2. Identity in, identity out.
static_assert(guestToHostSwizzle(kSwizzleRGBA, kSwizzleRGBA) == kSwizzleRGBA,
              "guest RGBA over an identity host format is a no-op");

// 3. ZERO/ONE selectors survive as constants, and the undefined 6/7 normalise.
static_assert(guestToHostSwizzle(makeSwizzle(kSelZero, kSelOne, kSelZero, kSelOne),
                                 kSwizzleRGBA)
                  == makeSwizzle(kSelZero, kSelOne, kSelZero, kSelOne),
              "constant selectors ignore the host table");
static_assert(guestToHostSwizzle(makeSwizzle(6u, 7u, 6u, 7u), kSwizzleRGBA)
                  == makeSwizzle(kSelZero, kSelOne, kSelZero, kSelOne),
              "6/7 normalise to 4/5 rather than reaching the host GPU");

// 4. BGRA — the majority non-identity case. Over an identity host format it
//    passes through unchanged; it is NOT special-cased anywhere.
static_assert(guestToHostSwizzle(makeSwizzle(kSelB, kSelG, kSelR, kSelA),
                                 kSwizzleRGBA)
                  == makeSwizzle(kSelB, kSelG, kSelR, kSelA),
              "guest BGRA over RGBA host composes to BGRA");

// 5/6. The host table doing real work: a guest RGBA read of a single-channel
//      and of a two-channel format collapses onto the channels that exist.
static_assert(guestToHostSwizzle(kSwizzleRGBA, kSwizzleRRRR) == kSwizzleRRRR,
              "every guest channel of a k_8 resolves to host R");
static_assert(guestToHostSwizzle(kSwizzleRGBA, kSwizzleRGGG) == kSwizzleRGGG,
              "guest B/A of a two-channel format resolve to host G");

}  // namespace texswz

// The Xenos register shadow (XenosRegisterFile) and getXenosRegisters() moved to gfx/record_engine.hpp with the record
// engine that writes it (C1 stage 2a); included above so every includer of this header still sees them.

// Captured draw command from cmd[43] dispatch.


// Lock-free SPSC ring buffer for draw commands.
// Producer: guest render thread (via the PM4 ring parser).
// Consumer: D3D12 present thread.
class DrawCommandQueue {
public:
  // iter-358k: a real menu frame carries 3,501 draws (golden trace) — 512
  // slots silently dropped 85% of them.
  // iter-435: with the GENERAL D3D9-HLE synthesis (every UI/world draw now
  // pushed at the API boundary) 4096 overflowed ~3962 times per 300s roll in
  // the world era — doubled. ~9.5KB/entry → ~78MB ring; revisit with a
  // slimmer command struct if memory matters later.
  static constexpr uint32_t kCapacity = 8192;

  bool push(const GuestDrawCommand& cmd) {
    uint32_t w = writeIdx_.load(std::memory_order_relaxed);
    uint32_t r = readIdx_.load(std::memory_order_acquire);
    if (((w + 1) % cap_) == r) return false;  // full
    ring_[w] = cmd;
    writeIdx_.store((w + 1) % cap_, std::memory_order_release);
    committed_.store(committed_.load(std::memory_order_relaxed) + 1, std::memory_order_release);  // one producer
    return true;
  }

  // iter-437e: CONSOLE-FAITHFUL BACKPRESSURE. On hardware a full command ring
  // BLOCKS the CPU until the GPU consumes — it never silently discards. Our
  // drop-on-full lost draws/resolves/markers in heavy eras (33K drops/run;
  // dropped movie quads = the single-present black flashes; dropped boundary
  // markers = the coherence feedback loop). Bounded wait: the drain runs on
  // the independent present thread, so progress is guaranteed; on timeout we
  // fall back to the old drop-and-log behavior.
  bool pushBlocking(const GuestDrawCommand& cmd, uint32_t maxWaitMs) {
    if (push(cmd)) return true;
    const uint32_t spins = maxWaitMs;  // 1ms per spin
    for (uint32_t i = 0; i < spins; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (push(cmd)) return true;
    }
    return false;
  }

  bool pop(GuestDrawCommand& out) {
    uint32_t r = readIdx_.load(std::memory_order_relaxed);
    uint32_t w = writeIdx_.load(std::memory_order_acquire);
    if (r == w) return false;  // empty
    out = ring_[r];
    readIdx_.store((r + 1) % cap_, std::memory_order_release);
    consumed_.store(consumed_.load(std::memory_order_relaxed) + 1, std::memory_order_release);  // one consumer
    return true;
  }
  // iter-461: in-place producer / consumer - a command is ~9.5 KB, and the
  // build-copy-push-pop-copy chain cost a city frame ~8 ms for ~3,500 draws.
  // reserve() hands the producer the next slot (nullptr = full); commit()
  // publishes it. front() is the oldest unconsumed command, valid until
  // popFront().
  GuestDrawCommand* reserve() {
    const uint32_t w = writeIdx_.load(std::memory_order_relaxed);
    const uint32_t r = readIdx_.load(std::memory_order_acquire);
    if (((w + 1) % cap_) == r) return nullptr;
    producer_.store(std::this_thread::get_id(), std::memory_order_relaxed);
    return &ring_[w];
  }
  // iter-466 cp lane (SR_CP_QUEUE_REWIND): an EMPTY queue restarts at slot 0 when the caller is
  // also the producer (the PM4 live path: the CP thread emits and drains). The slots of one
  // indirect buffer then stay cache-resident instead of walking the whole ~84 MB ring, where
  // every emitDraw wrote cold memory. Returns whether it rewound.
  bool rewindIfEmptyOnProducer() {
    if (producer_.load(std::memory_order_relaxed) != std::this_thread::get_id()) return false;
    const uint32_t r = readIdx_.load(std::memory_order_relaxed);
    const uint32_t w = writeIdx_.load(std::memory_order_relaxed);
    if (r != w || r == 0) return false;
    writeIdx_.store(0, std::memory_order_release);
    readIdx_.store(0, std::memory_order_release);
    return true;
  }
  void commit() {
    const uint32_t w = writeIdx_.load(std::memory_order_relaxed);
    writeIdx_.store((w + 1) % cap_, std::memory_order_release);
    committed_.store(committed_.load(std::memory_order_relaxed) + 1, std::memory_order_release);  // one producer
  }
  const GuestDrawCommand* front() {
    const uint32_t r = readIdx_.load(std::memory_order_relaxed);
    const uint32_t w = writeIdx_.load(std::memory_order_acquire);
    return r == w ? nullptr : &ring_[r];
  }
  void popFront() {
    const uint32_t r = readIdx_.load(std::memory_order_relaxed);
    readIdx_.store((r + 1) % cap_, std::memory_order_release);
    consumed_.store(consumed_.load(std::memory_order_relaxed) + 1, std::memory_order_release);  // one consumer
  }
  // NATIVE strand A executor: items ever published / consumed (the executor's progress), and a smaller
  // ring for a concurrent producer and consumer (no rewind there: the slots of a 1K ring stay in cache
  // instead of walking the whole ~84 MB ring). setCapacity only takes effect on an empty queue.
  uint64_t committed() const { return committed_.load(std::memory_order_acquire); }
  uint64_t consumed() const { return consumed_.load(std::memory_order_acquire); }
  bool setCapacity(uint32_t n) {
    const uint32_t r = readIdx_.load(std::memory_order_acquire);
    const uint32_t w = writeIdx_.load(std::memory_order_acquire);
    if (r != w || n < 16 || n > kCapacity) return false;
    writeIdx_.store(0, std::memory_order_release);
    readIdx_.store(0, std::memory_order_release);
    cap_ = n;
    return true;
  }

  uint32_t size() const {
    uint32_t w = writeIdx_.load(std::memory_order_acquire);
    uint32_t r = readIdx_.load(std::memory_order_acquire);
    return (w >= r) ? (w - r) : (cap_ - r + w);
  }

  void clear() {
    readIdx_.store(writeIdx_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    consumed_.store(committed_.load(std::memory_order_relaxed), std::memory_order_release);
  }

  // iter-437e: frame-boundary bookkeeping for frame-coherent drains. Counted
  // ONLY for markers that actually entered the ring (a dropped marker merges
  // its frame into the next — one frame of latency, never a stall).
  // iter-466 cp round 2 (SR_CP_CONST_TRACK): the consumer saw a tracked command out of
  // sequence; the producer's next tracked command re-sends every used block.
  void requestConstResync() { constResync_.store(true, std::memory_order_release); }
  bool takeConstResync() {
    return constResync_.load(std::memory_order_acquire) && constResync_.exchange(false, std::memory_order_acq_rel);
  }
  void notifyBoundaryPushed() {
    boundariesPushed_.fetch_add(1, std::memory_order_release);
  }
  uint64_t boundariesPushed() const {
    return boundariesPushed_.load(std::memory_order_acquire);
  }
  // iter-440c API-PRESENT-BLOCK: the drain publishes each consumed boundary;
  // the Swap hook blocks the guest until consumption catches its own marker —
  // the reference's g_executedCommandList.wait (video.cpp:2811) and the
  // console's own one-frame-in-flight throttle, expressed host-side.
  void notifyBoundaryConsumed() {
    boundariesConsumed_.fetch_add(1, std::memory_order_release);
  }
  uint64_t boundariesConsumed() const {
    return boundariesConsumed_.load(std::memory_order_acquire);
  }

private:
  GuestDrawCommand ring_[kCapacity] = {};
  std::atomic<uint32_t> writeIdx_{0};
  std::atomic<uint32_t> readIdx_{0};
  uint32_t cap_ = kCapacity;
  std::atomic<uint64_t> committed_{0}, consumed_{0};
  std::atomic<std::thread::id> producer_{};
  std::atomic<bool> constResync_{false};
  std::atomic<uint64_t> boundariesPushed_{0};
  std::atomic<uint64_t> boundariesConsumed_{0};
};

// Global access to the shared draw queue (the register shadow's accessor is record_engine.hpp's).
DrawCommandQueue& getDrawCommandQueue();
// iter-441 MIGRATION STAGE-A: g_hleSuppressPm4Draws + HleShaderFix deleted with
// kernel/hle_d3d9.cpp (the old draw-synth renderer). Draw submission is now a
// per-Pm4Engine-instance property (Pm4Engine::submitDraws): the replay fixture
// submits, the live instance is command-processor service only.

// iter-369g: monotonically bumped once per VdSwap, AFTER that swap's command
// window has been parsed. THE CONSOLE'S FRAME BOUNDARY IS VdSwap — this is the
// signal that one frame's worth of draws is now queued. The drain was previously
// run from main.cpp's free 60Hz loop, which is unrelated to the guest's frame
// rate, so batches from many swaps piled into one render (measured: 1888 batches
// / 1707 resolve batches in a single render, against 68 resolve events for the
// whole run).
uint64_t getSwapFrameSeq();
// iter-421: the trace REPLAY presents once per kSwap event record, but only the
// live VdSwap ring scan bumped the sequence - so in a multi-frame stream replay
// presentFramebuffer's drain gate fired exactly once and every later draw was
// DROPPED at the 4096-entry ring (measured: 39,000 drops, pushed frozen at
// 4,271). The replay must publish the same frame boundary the live path does.
void bumpSwapFrameSeq();

// iter-358f: g_alloc2FloatVA/g_alloc2FloatStride removed — a heap-scan heuristic that
// overrode the guest's own Xenos vertex-fetch constants, tied to the fake-D3D subsystem
// built on the misidentified KeCertMonitorData slot. See docs/NATURAL_BASELINE.md 2026-07-17.

}  // namespace gfx
}  // namespace sr
