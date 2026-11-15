// DOGMA hash-table workarounds for FMHud / FMVfxs / family.
//
// The Xenon build of this XEX appears to be missing the "grow" code path for
// the Robin-Hood hash family at sub_825FD958 (linear-probe lookup),
// sub_825FDA60 (insert), and sub_825FDBB0 (init). Those paths exist on the
// reference platform but were stripped/inlined-away here, so a freshly-init'd
// table has {head=0, capacity=1} and stays that way; consumers loop forever
// in the probe expecting an empty-slot sentinel that never arrives.
//
// Three strong overrides land here. Linker picks them over the recomp's weak
// aliases (same pattern as kernel/splash_hooks.cpp).
//
//   sub_825FDBB0 — init: delegate to original, then back-fill 16-entry
//                  bucket arrays with -1 sentinels at the 10 known table
//                  offsets. Cheap (1280 bytes per init).
//
//   sub_825FD958 — lookup: short-circuit when the table is degenerate
//                  (capacity==0 OR head==0). Without this, divwu r10,r5,r11
//                  with r11=0 traps; SEH skips with quotient=0; the scan
//                  loop retries forever (24M+ traps observed at iter-17).
//                  When head=0 with cap>0, the probe reads zeros from low
//                  guest VAs (treated as "occupied not match"), spins.
//                  We write {bucket_idx=0, capacity=0} to the output struct
//                  at r3 so the caller's empty-slot detection (via LZCOUNT
//                  trick) takes the "table is empty, just insert" path.
//
//   sub_825FDA60 — insert: same degenerate-hash check; no output struct.
//
// Layout of the struct sub_825FDBB0 inits (verified via Ghidra decompile of
// FUN_825fdbb0 + recomp ppc_recomp.73.cpp:14654-14743):
//
//   offset    field        original         after fix
//   +0/+4     table[0]     head=0,cap=1     head=ALLOC,cap=16
//   +8/+12    table[1]     head=0,cap=1     head=ALLOC,cap=16
//   +16/+20   table[2]     head=0,cap=1     head=ALLOC,cap=16
//   +24/+28   table[3]     head=0,cap=1     head=ALLOC,cap=16
//   +32/+36   table[4]     head=0,cap=1     head=ALLOC,cap=16
//   +76/+80   table[5]     head=0,cap=1     head=ALLOC,cap=16
//   +84/+88   table[6]     head=0,cap=1     head=ALLOC,cap=16
//   +92/+96   table[7]     head=0,cap=1     head=ALLOC,cap=16
//   +100/+104 table[8]     head=0,cap=1     head=ALLOC,cap=16
//   +108/+112 table[9]     head=0,cap=1     head=ALLOC,cap=16
//
// Each bucket is 8 bytes (per scan's `*param_1 * 8 + iVar3`).
//
// Calling-convention notes (verified by reading the recomp, NOT Ghidra's
// decompile — Ghidra mis-attributes these functions because their prologue
// `bl __savegprlr_NN` confuses auto-analysis):
//   sub_825FD958: hash struct in r4 (per `lwz r10,4(r4)` at recomp:14347).
//   sub_825FDA60: hash struct in r3 (per `lwz r30,0(r3)` at recomp:14525).
// iter-20 used r4 for sub_825FDA60 — that hook silently never fired. Fixed
// in iter-21. Always cross-check the recomp source for register usage.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

extern "C" void __imp__sub_825FDBB0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_825FD958(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_825FDA60(PPCContext& __restrict ctx, uint8_t* base);

namespace {
constexpr uint32_t kBucketCount = 16;        // initial capacity per table
constexpr uint32_t kBucketBytes = 8;         // sub_825FD958 strides by 8
constexpr uint32_t kArrayBytes = kBucketCount * kBucketBytes;  // 128

constexpr uint32_t kTableOffsets[10] = {
  0, 8, 16, 24, 32, 76, 84, 92, 100, 108
};

uint32_t allocBucketArray() {
  uint32_t va = sr::kernel::guestHeapAlloc(kArrayBytes);
  if (va == 0) va = sr::kernel::guestHeapAllocFallback(kArrayBytes);
  if (va == 0) return 0;
  if (uint8_t* host = sr::kernel::guestVAToHost(va))
    std::memset(host, 0xFF, kArrayBytes);  // -1 sentinel
  return va;
}

bool isHashStructDegenerate(uint32_t hashTableVA) {
  const uint32_t head     = sr::kernel::readGuestU32BE(hashTableVA + 0);
  const uint32_t capacity = sr::kernel::readGuestU32BE(hashTableVA + 4);
  return (capacity == 0u) || (head == 0u);
}

// iter-292b: A hash struct is unusable if its OWN pointer is not a sane guest VA,
// or if its bucket-array pointer (+0) / capacity (+4) are out of range. Unlike
// isHashStructDegenerate (which reads *through* the pointer), this validates the
// pointer ITSELF first — catching the NULL / wild-r3 case that defeats the
// through-pointer test. Root: the game forms uncached pointers by setting bit 25
// (+0x02000000); our flat model doesn't fold that alias, so an aliased worldPtr
// reads back 0/garbage and flows into sub_825FDA60 as r3=0. The old guard then
// read guest[0]/[4] (XEX-header bytes, often nonzero) so cap==0/head==0 was false
// -> it fell through to the original, which walked header bytes as a bucket array
// and spun the rehash do-while forever, freezing the main thread at PC 0x825FDA68.
// Range/alignment idiom mirrors the recomp probe-guard at ppc_recomp.73.cpp:14487.
bool isHashStructUnusable(uint32_t structVA) {
  if (structVA < 0x82000000u || structVA >= 0xBF000000u || (structVA & 3u) != 0u)
    return true;  // NULL / wild struct ptr (reading through it would walk garbage)
  const uint32_t bucketsVA = sr::kernel::readGuestU32BE(structVA + 0);
  const uint32_t capacity  = sr::kernel::readGuestU32BE(structVA + 4);
  if (capacity == 0u || capacity > 0x400000u) return true;  // cap=0 (div) / absurd
  const uint64_t end = static_cast<uint64_t>(bucketsVA) + static_cast<uint64_t>(capacity) * 8u;
  if (bucketsVA < 0x82000000u || end > 0xBF000000u || (bucketsVA & 7u) != 0u)
    return true;  // bucket array not a sane 8-aligned in-range VA
  return false;
}

// Throttled log: prints first 8 hits then powers-of-two thereafter. Caller
// supplies the per-function counter so each strong-override gets independent
// rate-limiting.
void logDegenerate(const char* fnName, uint32_t reg, uint32_t hashTableVA,
                   std::atomic<unsigned>& counter) {
  unsigned n = counter.fetch_add(1, std::memory_order_relaxed);
  if (n < 8 || (n & (n - 1)) == 0) {
    const uint32_t head     = sr::kernel::readGuestU32BE(hashTableVA + 0);
    const uint32_t capacity = sr::kernel::readGuestU32BE(hashTableVA + 4);
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[HASH_FIX] %s degenerate-hash short-circuit @r%u=0x%08X head=0x%08X cap=%u (#%u)",
      fnName, reg, hashTableVA, head, capacity, n + 1);
  }
}
// iter-398e: SR_DOGMA_HASH_FAB=0 knocks out all three master-gated forcing
// paths in this TU (bucket back-fill + degenerate/spin short-circuits) —
// one of three master-alone-active sites bisected for the deep-load rescue.
static bool dogmaHashFabOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_DOGMA_HASH_FAB");
    return !e || e[0] != '0';
  }();
  return v;
}
}  // namespace

void sub_825FDBB0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t structVA = ctx.r3.u32;
  __imp__sub_825FDBB0(ctx, base);

  // FORCING (back-fills bucket arrays the game's own init left degenerate — a
  // band-aid over a missing grow path / unfolded pointer alias). Master-gated:
  // the natural default runs only __imp__ above, exposing the real gap.
  if (!sr::kernel::forcingAllowed() || !dogmaHashFabOn()) return;

  // Patch each {head, capacity} pair to point at a -1-filled bucket array.
  // On allocation failure leave that table at its original {head=0,cap=1} —
  // degrades to pre-fix behavior for that table only.
  unsigned patched = 0;
  for (uint32_t off : kTableOffsets) {
    uint32_t bucketsVA = allocBucketArray();
    if (bucketsVA == 0) continue;
    PPC_STORE_U32(structVA + off,     bucketsVA);
    PPC_STORE_U32(structVA + off + 4, kBucketCount);
    ++patched;
  }

  static std::atomic<unsigned> s_count{0};
  unsigned n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 5 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[HASH_FIX] sub_825FDBB0 #%u struct=0x%08X patched %u/10 tables (cap=%u each)",
      n, structVA, patched, kBucketCount);
  }
}

// iter-DOGMA-RE-12: spin detector for sub_825FD958. The lifted body's loop
// at loc_825FD9FC exits when it finds the search key OR a -1 sentinel slot.
// If the table is full of valid keys with no sentinels and the search key
// isn't there, the loop spins forever. Observed in FMUI's CTOR chain
// (200K calls/sec from caller at 0x82602CC8 inside FMUI(0x821516E0)).
//
// Detection: per-hashTableVA call-count tracker with a 1s window. If a single
// table sees >50000 lookups in 1 second, treat it as the spin signature and
// short-circuit with degenerate output (caller takes "empty slot, insert
// here" path), breaking the loop.
static std::atomic<unsigned> g_spinShortCircuits{0};

namespace {
struct SpinTracker {
  std::mutex mtx;
  std::unordered_map<uint32_t, std::pair<int64_t, unsigned>> counters;  // tableVA -> (windowStartMs, count)
};
SpinTracker& spinTracker() {
  static SpinTracker t;
  return t;
}
bool isSpinning(uint32_t hashTableVA) {
  constexpr int64_t kWindowMs = 1000;
  constexpr unsigned kThreshold = 50000;
  auto& tr = spinTracker();
  int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  std::lock_guard<std::mutex> lock(tr.mtx);
  auto& [windowStart, count] = tr.counters[hashTableVA];
  if (nowMs - windowStart > kWindowMs) {
    windowStart = nowMs;
    count = 0;
  }
  ++count;
  return count > kThreshold;
}
}  // namespace

void sub_825FD958(PPCContext& __restrict ctx, uint8_t* base) {
  // DIAGNOSTIC (SR_HASH_PROBE=1, default off): PURE OBSERVATION — no writes, no
  // forcing. Logs the table state the lookup sees on a NATURAL run, to
  // distinguish the missing bucket-allocate's cause: buckets==0 + sane ptr ->
  // recompiler-hole / never-reserved; bit25 set -> uncached-pointer alias our
  // flat model doesn't fold; buckets non-NULL but cap wrong -> gate mis-eval.
  static const bool s_hashProbe = []() {
    const char* e = std::getenv("SR_HASH_PROBE"); return e && e[0] == '1';
  }();
  if (s_hashProbe) {
    static std::atomic<unsigned> s_p{0};
    unsigned n = s_p.fetch_add(1, std::memory_order_relaxed);
    if (n < 48) {
      const uint32_t t = ctx.r4.u32;
      const bool sane = (t >= 0x82000000u && t < 0xC0000000u && (t & 3u) == 0u);
      const uint32_t buckets = sane ? sr::kernel::readGuestU32BE(t + 0) : 0xBADu;
      const uint32_t cap     = sane ? sr::kernel::readGuestU32BE(t + 4) : 0xBADu;
      const uint32_t cnt     = sane ? sr::kernel::readGuestU32BE(t + 8) : 0xBADu;
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[HASH_PROBE] lookup #%u table=0x%08X bit25=%u buckets=0x%08X cap=%u cnt=%u",
        n, t, (t >> 25) & 1u, buckets, cap, cnt);
    }
  }
  // FORCING: the degenerate/spin short-circuits below write the caller's output
  // struct for it. Master-gated; natural default runs the real lookup only.
  if (!sr::kernel::forcingAllowed() || !dogmaHashFabOn()) { __imp__sub_825FD958(ctx, base); return; }
  const uint32_t hashTableVA = ctx.r4.u32;
  if (isHashStructDegenerate(hashTableVA) || isHashStructUnusable(hashTableVA)) {
    static std::atomic<unsigned> s_hits{0};
    logDegenerate("sub_825FD958", 4, hashTableVA, s_hits);
    // Write {bucket_idx=0, capacity=0} to output struct at r3 so caller's
    // empty-slot detection takes the "table is empty, insert here" path.
    sr::kernel::writeGuestBE32(ctx.r3.u32,     0);
    sr::kernel::writeGuestBE32(ctx.r3.u32 + 4, 0);
    return;
  }
  if (isSpinning(hashTableVA)) {
    unsigned n = g_spinShortCircuits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n & (n - 1)) == 0) {
      const uint32_t head = sr::kernel::readGuestU32BE(hashTableVA + 0);
      const uint32_t cap  = sr::kernel::readGuestU32BE(hashTableVA + 4);
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[HASH_FIX] sub_825FD958 SPIN-SHORTCUT @r4=0x%08X head=0x%08X cap=%u (#%u) — forcing degenerate output to break tight loop",
        hashTableVA, head, cap, n);
    }
    sr::kernel::writeGuestBE32(ctx.r3.u32,     0);
    sr::kernel::writeGuestBE32(ctx.r3.u32 + 4, 0);
    return;
  }
  __imp__sub_825FD958(ctx, base);
}

void sub_825FDA60(PPCContext& __restrict ctx, uint8_t* base) {
  // FORCING: skips the rehash for degenerate/wild tables. Master-gated; the
  // natural default runs the real insert only.
  if (!sr::kernel::forcingAllowed() || !dogmaHashFabOn()) { __imp__sub_825FDA60(ctx, base); return; }
  const uint32_t hashTableVA = ctx.r3.u32;
  // iter-292b: also reject NULL/wild/cap-absurd tables (isHashStructUnusable) — the
  // through-pointer degenerate test alone missed r3=0 (read XEX-header bytes as a
  // table) and the rehash do-while spun forever, freezing the main thread.
  if (isHashStructDegenerate(hashTableVA) || isHashStructUnusable(hashTableVA)) {
    static std::atomic<unsigned> s_hits{0};
    logDegenerate("sub_825FDA60", 3, hashTableVA, s_hits);
    return;  // skip the rehash; erasing from a NULL/degenerate table is a no-op
  }
  __imp__sub_825FDA60(ctx, base);
}
