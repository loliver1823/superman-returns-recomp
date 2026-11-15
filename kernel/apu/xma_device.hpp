// iter-463 audio NATIVE: the XMA decoder device - a port of Xenia's XmaDecoder front end, and the
// 16 XMA* kernel exports the game imports, implemented natively on the host.
//
// Ported from Xenia Canary (BSD-3-Clause, see docs/THIRD_PARTY.md), oracle build
// canary_experimental@0bd090dbe (xma_decoder = "new", use_dedicated_xma_thread = true):
//   src/xenia/apu/xma_decoder.cc:130-404   Setup, WorkerThreadMain, GetContextId,
//                                          Allocate/ReleaseContext, BlockOnContext, WriteRegister
//   src/xenia/kernel/xboxkrnl/xboxkrnl_audio_xma.cc  the XMA* exports
// Copyright 2022 Ben Vanik / Xenia Canary. Released under the BSD license.
//
// The application exports call native kick / lock / clear operations directly. Register
// dispatch is retained in xma_register_oracle.cpp, compiled only into difftest. The linked
// XDK XAudio XMA HAL still needs whole-game absence witnesses (docs/NATIVE_ROADMAP.md);
// this build separation does not establish whole-game or release composition acceptance.
//
// Gates (default ON since the iter-463 rolls; =0 keeps the A/B):
//   SR_XMA_NEW=1               the 16 exports below replace the legacy fakes (stubs.cpp)
//   SR_XMA_DEDICATED_THREAD    (default ON = the oracle's use_dedicated_xma_thread=true; only
//                              meaningful with SR_XMA_NEW=1). =0 runs Work() inline on the
//                              kicking guest thread (Xenia's other mode).
//
// Memory: the 320 x 64-byte context array is ONE guestPhysAlloc(0x5000, 0x1000) grant made on
// the first XMACreateContext (Xenia: SystemHeapAlloc(physical) at startup), zeroed. Context
// fields hold PHYSICAL addresses (xeGetPhysicalAddress, kernel/mm_phys.hpp). The decoder maps
// them back through a per-context side table of the exact VA each export converted (our flat
// arena does not alias the physical windows, so a PA alone is ambiguous), then a page memo of
// every conversion the exports made; a miss is logged [XMA_PHYS_MISS] and treated as "buffer
// unavailable", never dereferenced.
//
// Kick handshake (critique B1): each kick takes a sequence number; the worker publishes the
// sequence each Work() consumed; the kicker waits for done >= its own number (so a late signal
// cannot satisfy the next kick). A kick for an unallocated context does not wait (Xenia would
// hang). A wait longer than 250 ms logs [XMA_KICK_TIMEOUT] and returns - Xenia would hang there,
// so any timeout is a bug to root-cause.
#pragma once

#include "kernel/apu/xma_api.hpp"
#include "kernel/apu/xma_context_new.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace sr::kernel {
class ExportRegistry;
}

namespace sr::kernel::apu {

bool xmaDedicatedThreadEnabled();  // SR_XMA_DEDICATED_THREAD (default ON)
#if defined(SR_DIFFTEST)
// Register dispatch and its A/B gate are linked only into the test oracle.
bool xmaNativeFrontEnabled();
void xmaNativeFrontSetForTest(int mode);   // -1 = env
#endif
void xmaSetEnabledForTest(int newMode, int dedicatedMode);   // -1 = env

constexpr uint32_t kXmaContextCount = 320;
constexpr uint32_t kXmaContextArrayBytes = kXmaContextCount * 64;
constexpr uint32_t kStatusNoMemory = 0xC0000017u;    // X_STATUS_NO_MEMORY
constexpr uint32_t kXEFalse = 0x80000000u;           // X_E_FALSE (xbox.h)

#if defined(SR_DIFFTEST)
// Xenia register indices (xma_register_table.inc): kick 0x650-0x659 (0x1940), lock 0x690-0x699
// (0x1A40), clear 0x6A0-0x6A9 (0x1A80). Bit n of base+k addresses context 32k+n.
constexpr uint32_t kRegContext0Kick = 0x650;
constexpr uint32_t kRegContext0Lock = 0x690;
constexpr uint32_t kRegContext0Clear = 0x6A0;

#endif

struct XmaStats {
  uint64_t creates, createFails, releases, inits, kicks, works, locks, clears, timeouts,
      unallocKicks, frames, decodeFails, physMisses, physAliases, badCtx;
};

class XmaDevice final : public IXmaMemory {
 public:
  static XmaDevice& instance();

  // Lazily allocates the context array and starts the worker (first XMACreateContext).
  bool ensureInit();
  // Test seam: run on caller-provided memory (no guestPhysAlloc, no guest arena).
  void initForTest(IXmaMemory* mem, uint32_t arrayVA, uint32_t arrayPA,
                   XmaFrameDecoderFactory factory, bool dedicated);
  void shutdownForTest();

  uint32_t allocateContext();          // VA, or 0 when all 320 are taken
  void releaseContext(uint32_t va);    // bounds-checked (Xenia indexes contexts_[-1])
  bool blockOnContext(uint32_t va, bool poll);
  int contextId(uint32_t va) const;    // -1 outside the array or misaligned
  uint32_t contextArrayVA() const { return arrayVA_; }
  uint32_t contextArrayPA() const { return arrayPA_; }
  XmaContextNew* context(uint32_t id) { return id < kXmaContextCount ? &contexts_[id] : nullptr; }

#if defined(SR_DIFFTEST)
  // Test-only register oracle; no declarations or implementations in the application.
  void storeIndexedRegister(uint32_t baseRegByteOffset, uint32_t ctxVA);
  void writeRegister(uint32_t reg, uint32_t value);
#endif
  // Native operations address the context named by the export.
  void kickContext(uint32_t ctxVA);
  void lockContext(uint32_t ctxVA);
  void clearContext(uint32_t ctxVA);
  uint64_t regfileWrites() const {
    return regfileWrites_.load(std::memory_order_relaxed);
  }

  // Side table (called by the exports right after they convert a VA): slot 0 = in0, 1 = in1,
  // 2 = out.
  void notePhys(uint32_t ctxVA, int slot, uint32_t pa, uint32_t va, uint32_t bytes);

  // IXmaMemory
  uint8_t* TranslateVirtual(uint32_t va) override;
  uint8_t* TranslatePhysical(uint32_t ctx_id, uint32_t pa, uint32_t bytes) override;

  XmaStats stats() const;

 private:
  XmaDevice();
  void startWorker();
  void workerMain();
  void kick(uint32_t ctxId);
  void kickGroup(uint32_t baseId, uint32_t mask);    // bit n = context baseId + n
  void lockGroup(uint32_t baseId, uint32_t mask);
  void clearGroup(uint32_t baseId, uint32_t mask);
  int nativeId(uint32_t ctxVA, const char* op);      // contextId + the bad-context accounting
  // Retained witness storage, not register dispatch. Game builds retain no writer.
  std::atomic<uint64_t> regfileWrites_{0};
  void maybeSummary();

  mutable std::mutex mu_;               // init + allocation bitmap
  bool inited_ = false;
  bool dedicated_ = true;
  IXmaMemory* testMem_ = nullptr;       // non-null in tests
  uint8_t* guestBase_ = nullptr;
  size_t guestSize_ = 0;
  uint32_t arrayVA_ = 0, arrayPA_ = 0;
  std::array<XmaContextNew, kXmaContextCount> contexts_;
  std::array<bool, kXmaContextCount> allocated_{};

  // Side table: [ctx][slot] = (pa << 32) | va ; bytes in sideBytes_.
  std::array<std::array<std::atomic<uint64_t>, 3>, kXmaContextCount> side_{};
  std::array<std::array<std::atomic<uint32_t>, 3>, kXmaContextCount> sideBytes_{};
  // Page memo: phys page (pa >> 12, pa < 1 GB) -> (va page + 1), 0 = none.
  std::unique_ptr<std::atomic<uint32_t>[]> memo_;

  // Worker.
  std::thread worker_;
  std::atomic<bool> running_{false};
  std::mutex workMu_;
  std::condition_variable workCv_;
  bool workPending_ = false;            // auto-reset work_event_
  std::mutex doneMu_;
  std::condition_variable doneCv_;

  std::atomic<uint64_t> creates_{0}, createFails_{0}, releases_{0}, inits_{0}, kicks_{0},
      works_{0}, locks_{0}, clears_{0}, timeouts_{0}, unallocKicks_{0}, physAliases_{0},
      badCtx_{0}, memoMisses_{0};
  std::atomic<uint64_t> lastSummaryMs_{0};
  friend uint32_t xmaExportInitialize(uint32_t, uint32_t);
};

// The exports (Xenia bodies). Exposed for the difftests.
uint32_t xmaExportCreate(uint32_t outPtrVA);
uint32_t xmaExportRelease(uint32_t ctxVA);
uint32_t xmaExportInitialize(uint32_t ctxVA, uint32_t initVA);
uint32_t xmaExportSetInputBuffer(int which, uint32_t ctxVA, uint32_t bufVA, uint32_t packetCount);
uint32_t xmaExportEnable(uint32_t ctxVA);
uint32_t xmaExportDisable(uint32_t ctxVA, uint32_t wait);

// Registers the 16 imported XMA* exports when xmaNewEnabled(); with the gate off the legacy
// fakes registered before it stay in place, byte for byte.
void registerXmaExports(ExportRegistry& reg);

}  // namespace sr::kernel::apu
