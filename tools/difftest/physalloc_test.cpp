// iter-458 H-FREE: the reusable physical allocator (kernel/phys_alloc.{hpp,cpp})
// and its wiring - the REAL MmAllocatePhysicalMemoryEx / MmFreePhysicalMemory
// exports from registerStubs(), guestPhysAlloc, and the census ownership rule.
//
// Run: build/difftest.exe --physalloc-test   (exit code = failed checks)
// The gate SR_PHYS_FREE is latched on first use, so this runner sets it first.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/phys_alloc.hpp"
#include "kernel/phys_window.hpp"
#include "kernel/stubs.hpp"

PPC_EXTERN_FUNC(sub_82467F40);   // the XMemAlloc physical-path override (gpu_ring_alloc_fix.cpp)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace A = sr::kernel::physalloc;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}
}  // namespace

int runPhysAllocTest() {
  _putenv_s("SR_PHYS_FREE", "1");
  std::printf("[PHYSALLOC_TEST] reusable physical allocator\n");
  check(A::physFreeEnabled(), "G0 SR_PHYS_FREE latched on");
  // iter-458 H-LIFO (Astra 058) is inert under H-FREE even when requested.
  sr::kernel::setPhysLifoForTest(1);
  check(!sr::kernel::physLifoEnabled(), "G1 H-LIFO stays off under SR_PHYS_FREE");
  sr::kernel::setPhysLifoForTest(-1);

  // ── unit: a 16 MB arena ────────────────────────────────────────────────────
  const uint32_t lo = 0xA0000000u, hi = 0xA1000000u;
  A::resetForTest(lo, hi);
  const uint32_t a = A::alloc(0x3000, 0x1000);
  check(a == hi - 0x3000, "U1 top-down: the first block ends at the arena top");
  const uint32_t b = A::alloc(0x1000, 0x10000);
  check(b != 0 && (b & 0xFFFFu) == 0 && b + 0x1000 <= a, "U2 alignment honoured below the first block");
  const uint32_t c = A::alloc(0x100, 0);
  check(c != 0 && (c & 0xFFFu) == 0 && A::liveSize(c) == 0x1000, "U3 sub-page request = one page, page-aligned");

  check(!A::free(a + 0x1000), "U4 interior pointer refused");
  check(!A::free(0xA0800000u), "U5 unknown base refused");
  check(A::free(a), "U6 live block freed");
  check(!A::free(a), "U7 double free refused");
  check(A::stats().refusedFrees == 3, "U8 three refusals counted");

  const uint32_t a2 = A::alloc(0x3000, 0x1000);
  check(a2 == a, "U9 freed block reused (top-down lands in it again)");

  // Split + coalesce: free two adjacent blocks, then a block spanning both fits.
  A::resetForTest(lo, hi);
  const uint32_t x = A::alloc(0x2000, 0x1000);   // top
  const uint32_t y = A::alloc(0x2000, 0x1000);   // just below
  const uint32_t z = A::alloc(0x2000, 0x1000);   // below y (keeps y..x off the big range)
  check(x == hi - 0x2000 && y == x - 0x2000 && z == y - 0x2000, "U10 three adjacent top-down blocks");
  check(A::free(x) && A::free(y), "U11 two adjacent blocks freed");
  const uint32_t xy = A::alloc(0x4000, 0x1000);
  check(xy == y, "U12 coalesced range serves a block spanning both");
  (void)z;

  // Exhaustion and exact fit.
  A::resetForTest(lo, hi);
  check(A::alloc(hi - lo + 0x1000, 0) == 0, "U13 larger than the arena -> 0");
  const uint32_t whole = A::alloc(hi - lo, 0);
  check(whole == lo, "U14 exact whole-arena fit");
  check(A::alloc(0x1000, 0) == 0, "U15 full arena -> 0");
  check(A::free(whole) && A::alloc(0x1000, 0) == hi - 0x1000, "U16 usable again after free");
  check(A::stats().failedAllocs == 2, "U17 failures counted");

  // ── Xenia service semantics ────────────────────────────────────────────────
  A::resetForTest(A::kLo, A::kHi);
  check(A::mmAllocate(0x1000, 0, 0, 0xFFFFFFFFu, 0) == 0 && A::stats().badProtect == 1,
        "X1 protect without READONLY/READWRITE fails");
  const uint32_t lp = A::mmAllocate(0x1234, A::kXPageReadWrite | A::kXMemLargePages, 0, 0xFFFFFFFFu, 0);
  check(lp != 0 && (lp & 0xFFFFu) == 0 && A::liveSize(lp) == 0x10000,
        "X2 large pages: size and alignment round to 64 KB");
  const uint32_t hp = A::mmAllocate(0x1000, A::kXPageReadWrite | A::kXMem16MbPages, 0, 0xFFFFFFFFu, 0);
  check(hp != 0 && (hp & 0xFFFFFFu) == 0 && A::liveSize(hp) == 0x1000000,
        "X3 16 MB pages: size and alignment round to 16 MB");
  const uint32_t rg = A::mmAllocate(0x1000, A::kXPageReadWrite, 0x00100000u, 0x001FFFFFu, 0);
  check(rg >= A::kLo + 0x00100000u && rg + 0x1000 <= A::kLo + 0x00200000u,
        "X4 min/max are PHYSICAL bounds (block inside [1 MB, 2 MB))");
  check(A::mmAllocate(0x1000, A::kXPageReadWrite, 0, 0xFFFFFFFFu, 0x8000) % 0x8000 == 0,
        "X5 explicit alignment honoured");

  // ── the REAL exports: reserve -> free -> world pool reuse ──────────────────
  A::resetForTest(A::kLo, A::kHi);
  sr::kernel::registerStubs();
  auto& reg = sr::kernel::ExportRegistry::instance();
  auto mmAlloc = reg.getExport("MmAllocatePhysicalMemoryEx");
  auto mmFree = reg.getExport("MmFreePhysicalMemory");
  check(mmAlloc && mmFree, "R0 both exports registered");
  if (mmAlloc && mmFree) {
    // ReserveMemoryWithFallback_sub_82167FA8: 0x12B00000, protect 4.
    const uint32_t R = mmAlloc(0, 0x12B00000u, 4u, 0u, 0xFFFFFFFFu, 0u);
    check(R >= A::kLo && R + 0x12B00000u <= A::kHi, "R1 299 MB reservation inside the 0xA arena");
    mmFree(0, R, 0, 0, 0, 0);   // GpuCreateBufferResourcePool_sub_82183DC8
    check(A::liveSize(R) == 0, "R2 MmFreePhysicalMemory released it");
    // The world-render pool: 0xDFF0000, protect 0x20000404, align 0x8000.
    const uint32_t W = mmAlloc(0, 0x0DFF0000u, 0x20000404u, 0u, 0xFFFFFFFFu, 0x8000u);
    check(W >= R && W + 0x0DFF0000u <= R + 0x12B00000u,
          "R3 the 224 MB world pool reuses the freed reservation");
    check(W >= A::kLo, "R4 nothing lands below 0xA0000000");
    // The export applies Xenia's service semantics, not the legacy 4 KB path.
    const uint32_t lpE = mmAlloc(0, 0x1234u, 0x20000404u, 0u, 0xFFFFFFFFu, 0u);
    check(lpE != 0 && A::liveSize(lpE) == 0x10000u,
          "R10 export: X_MEM_LARGE_PAGES rounds the block to 64 KB");
    check(mmAlloc(0, 0x1000u, 0u, 0u, 0xFFFFFFFFu, 0u) == 0,
          "R11 export: protect without READONLY/READWRITE returns 0");
    mmFree(0, lpE, 0, 0, 0, 0);
    // The override's small GPU route and thread stacks go through guestPhysAlloc.
    const uint32_t g = sr::kernel::guestPhysAlloc(0x90, 16);
    check(g >= A::kLo && (g & 0xFFFu) == 0 && A::liveSize(g) == 0x1000,
          "R5 guestPhysAlloc is served by the allocator (page-granular)");
    mmFree(0, g, 0, 0, 0, 0);
    check(A::liveSize(g) == 0, "R6 a guestPhysAlloc block is freeable by the export");
    uint32_t plo = 0, phi = 0;
    sr::kernel::guestPhysRegion(plo, phi);
    check(plo >= A::kLo && phi == A::kHi, "R7 guestPhysRegion reports the allocator's extent");
    // Census ownership follows LIVE extents.
    namespace W8 = sr::kernel::physwin;
    check(W8::ownerOf(W, 0x1000) == W8::kOwnPhysRegion, "R8 live block owned");
    // The bottom of the freed reservation lies below the world pool and is
    // covered by no live block, yet it is inside the allocator's extent.
    check(!A::insideLive(R, 0x1000) && W > R, "R9a the reservation's bottom page is free");
    check(W8::ownerOf(R, 0x1000) == W8::kOwnNone,
          "R9 freed memory inside the extent is owned by nobody (live extents, not the span)");
  }

  // ── Q: cache-history quarantine (Astra 024) ────────────────────────────────
  A::resetForTest(A::kLo, A::kHi);
  {
    const uint32_t qa = A::alloc(0x4000, 0);
    A::pinHistory(qa & 0x1FFFFFFFu, 0x4000);          // a publisher pinned it
    check(A::free(qa) && A::stats().quarantined == 1, "Q1 pinned block is quarantined on free");
    const uint32_t qa2 = A::alloc(0x4000, 0);
    check(qa2 != 0 && (qa2 + 0x4000 <= qa || qa2 >= qa + 0x4000),
          "Q2 a quarantined extent is never handed out again");

    const uint32_t qm = A::alloc(0x10000, 0);
    A::pinHistory((qm + 0x8000) & 0x1FFFFFFFu, 0x100);   // mip-only, mid-block
    check(A::free(qm) && A::stats().quarantined == 2, "Q3 a mid-block (mip-only) pin quarantines the whole block");

    const uint32_t qx = A::alloc(0x2000, 0);
    A::pinHistory(0x80000000u | (qx & 0x1FFFFFFFu), 0x10);   // 0x8-window alias of the key
    check(A::free(qx) && A::stats().quarantined == 3, "Q4 an aliased key (window bits) is normalised");

    // ABA, order 1: pin (capture) before free -> quarantine -> no reuse.
    const uint32_t ab = A::alloc(0x3000, 0);
    A::pinHistory(ab & 0x1FFFFFFFu, 0x3000);
    A::free(ab);
    bool reused = false;
    for (int i = 0; i < 64; i++) {
      const uint32_t n = A::alloc(0x1000, 0);
      if (n >= ab && n < ab + 0x3000) reused = true;
    }
    check(!reused, "Q5 capture->free->realloc: the pinned extent is never reallocated (ABA closed)");

    // ABA, order 2: free first (no pin) -> reuse is legal -> pin the new owner.
    A::resetForTest(A::kLo, A::kHi);
    const uint32_t f1 = A::alloc(0x2000, 0);
    A::free(f1);
    const uint32_t f2 = A::alloc(0x2000, 0);
    check(f2 == f1 && A::stats().quarantined == 0, "Q6 no history -> the freed block is reused");
    A::pinHistory(f2 & 0x1FFFFFFFu, 0x2000);
    A::free(f2);
    check(A::stats().quarantined == 1, "Q7 the new owner's pin quarantines on its free");

    // ABA, order 3 (Astra 025): free FIRST (no pin), THEN a stale capture pins
    // A and snapshots it, THEN an allocation arrives. The pin must have carved
    // A's already-free space out of the free list.
    const uint32_t f3 = A::alloc(0x3000, 0);
    A::free(f3);                                   // freed, no history yet
    A::pinHistory(f3 & 0x1FFFFFFFu, 0x3000);        // stale capture pins A
    bool reused3 = false;
    for (int i = 0; i < 64; i++) {
      const uint32_t n = A::alloc(0x1000, 0);
      if (n >= f3 && n < f3 + 0x3000) reused3 = true;
    }
    check(!reused3 && A::stats().carves >= 1 && A::stats().carvedBytes >= 0x3000,
          "Q7b free->pin->snapshot->alloc: a pin carves already-free space (never reallocated)");

    // Boundary: a pin that only touches the block's end does not overlap it.
    const uint32_t bd = A::alloc(0x1000, 0);
    A::pinHistory((bd + 0x1000) & 0x1FFFFFFFu, 0x1000);
    const uint64_t qBefore = A::stats().quarantined;
    A::free(bd);
    check(A::stats().quarantined == qBefore && A::liveSize(bd) == 0,
          "Q8 an adjacent (non-overlapping) pin does not quarantine");

    // ...and a pin that ends exactly where the block STARTS does not either.
    // The pinned extent sits just below the new block, so pin it after the
    // block exists (bl2 - 0x1000 is some other address; only adjacency matters).
    const uint32_t bl2 = A::alloc(0x1000, 0);
    A::pinHistory((bl2 - 0x1000) & 0x1FFFFFFFu, 0x1000);
    const uint64_t qB2 = A::stats().quarantined;
    A::free(bl2);
    check(A::stats().quarantined == qB2 && A::liveSize(bl2) == 0,
          "Q8b a pin ending exactly at the block start does not quarantine");

    // A refused free changes nothing.
    const uint64_t q0 = A::stats().quarantined;
    check(!A::free(0xA1234000u) && A::stats().quarantined == q0, "Q9 a refused free quarantines nothing");
    // (the order-2 section reset the allocator: Q7, Q7b, Q8 and Q8b pinned since)
    // Q7b sits just below Q7 (they coalesce); its 64 live probe blocks push bd
    // lower, so Q8 and Q8b are separate extents: 4 pins make 3 extents.
    std::printf("      (Q10 observed: pins=%llu extents=%llu)\n",
                (unsigned long long)A::stats().historyPins,
                (unsigned long long)A::stats().historyExtents);
    check(A::stats().historyPins == 4 && A::stats().historyExtents == 3 &&
              A::stats().freeBytes > 0 && A::stats().largestFree > 0,
          "Q10 history, free-capacity and largest-span stats are reported");
  }

  // Concurrent pin/free vs allocation: nothing ever lands on a pinned+freed extent.
  A::resetForTest(A::kLo, A::kLo + 0x04000000u);
  {
    std::mutex qmu;
    std::vector<std::pair<uint32_t, uint32_t>> dead;   // pinned then freed
    std::atomic<int> violations{0};
    std::vector<std::thread> qt;
    for (int t = 0; t < 6; t++) {
      qt.emplace_back([t, &qmu, &dead, &violations] {
        std::mt19937 rng(99u + t);
        for (int i = 0; i < 800; i++) {
          const uint32_t sz = 0x1000u * (1u + rng() % 4u);
          const uint32_t va = A::alloc(sz, 0);
          if (!va) continue;
          {
            std::lock_guard<std::mutex> lk(qmu);
            for (const auto& d : dead)
              if (va < d.second && va + sz > d.first) violations++;
          }
          if (rng() % 3u == 0u) {
            A::pinHistory(va & 0x1FFFFFFFu, sz);
            A::free(va);
            std::lock_guard<std::mutex> lk(qmu);
            dead.push_back({va, va + sz});
          } else {
            A::free(va);
          }
        }
      });
    }
    for (auto& x3 : qt) x3.join();
    check(violations.load() == 0, "Q11 6 threads: no allocation ever overlaps a pinned-and-freed extent");
  }

  // ── provenance (Astra 027) ─────────────────────────────────────────────────
  A::resetForTest(A::kLo, A::kHi);
  {
    // Pin over free space that was NEVER allocated: nothing to protect.
    const uint64_t c0 = A::stats().carves;
    A::pinHistory(0x00100000u, 0x10000);            // arena VA 0xA0100000, never handed out
    check(A::stats().carves == c0 && A::stats().pinNoProvenanceBytes == 0x10000,
          "P1 a pin over never-allocated free space carves nothing (counted)");
    const uint32_t low = A::mmAllocate(0x10000, A::kXPageReadWrite, 0x00100000u, 0x0010FFFFu, 0);
    check(low == 0xA0100000u, "P2 that space stays allocatable");

    // pf1 reproduction at allocator level: a freed reservation, then pins on
    // keys that ALIAS it (heap-resident resources under the old routing),
    // fragment it until a world-pool-sized block no longer fits.
    const uint32_t R = A::mmAllocate(0x12B00000u, 4u, 0u, 0xFFFFFFFFu, 0u);
    A::free(R);
    for (uint32_t k = 0; k < 6; k++)   // six 0x398000 "resolve products", spread out
      A::pinHistory(((R & 0x1FFFFFFFu) + 0x1000000u + k * 0x2800000u), 0x398000u);
    check(A::mmAllocate(0x0DFF0000u, 0x20000404u, 0u, 0xFFFFFFFFu, 0x8000u) == 0,
          "P3 (reproduced) pins aliasing a freed reservation fragment it: the world pool FAILS");
  }

  // ── the override under the gate (Astra 027): heap route retired ─────────────
  A::resetForTest(A::kLo, A::kHi);
  {
    auto override = [](uint32_t size, uint32_t flags) -> uint32_t {
      PPCContext c{};
      c.r3.u64 = size;
      c.r4.u64 = flags;
      c.lr = 0;
      sub_82467F40(c, sr::kernel::getGuestBase());
      return c.r3.u32;
    };
    // (1) ring initialisation keeps its path: the FIRST large grant is the
    //     ring (heap window), exactly as before the gate.
    const uint32_t ring = override(0xC0000u, 0x8FC00000u);
    check(ring != 0 && ring < A::kLo, "O1 ring initialisation keeps the heap path (first grant)");
    // (2) after the ring: every eligible request is arena-backed.
    const uint32_t b256 = override(0x40000u, 0x8FC00000u);
    const uint32_t b257 = override(0x40001u, 0x8FC00000u);
    const uint32_t rp = override(0x398000u, 0x8CC00000u);
    const uint32_t b64 = override(0x4000000u, 0x8FC00000u);
    check(b256 >= A::kLo && A::liveSize(b256) == 0x40000u, "O2 256 KB boundary -> arena");
    check(b257 >= A::kLo && A::liveSize(b257) == 0x41000u, "O3 256 KB + 1 (formerly heap) -> arena");
    check(rp >= A::kLo && A::liveSize(rp) == 0x398000u, "O4 a resolve-sized request (formerly heap) -> arena");
    check(b64 >= A::kLo && A::liveSize(b64) == 0x4000000u, "O5 64 MB boundary -> arena");
    check((b257 & 0x7FFFu) == 0 && (rp & 0xFFFu) == 0,
          "O6 alignment from the XALLOC attributes honoured (32 KB class / page)");
    // Astra 028: a null mapping must FAIL this check, not pass it vacuously.
    const uint8_t* zp = sr::kernel::guestVAToHost(rp);
    bool zero = zp != nullptr;
    if (zp)
      for (uint32_t i = 0; i < 0x398000u; i += 4096) if (zp[i]) { zero = false; break; }
    check(zero, "O7 the grant is mapped AND zeroed, as the heap route did");
    // (3) the pf1 sequence after rerouting: the reservation is freed, the
    //     resources are LIVE arena blocks, their own pins carve nothing, and the
    //     world pool fits.
    const uint32_t R = A::mmAllocate(0x12B00000u, 4u, 0u, 0xFFFFFFFFu, 0u);
    A::free(R);
    const uint64_t carvesBefore = A::stats().carves;
    uint32_t res[6];
    for (int k = 0; k < 6; k++) {
      res[k] = override(0x398000u, 0x8CC00000u);
      A::pinHistory(res[k] & 0x1FFFFFFFu, 0x398000u);   // what the capture/resolve sites do
    }
    check(A::stats().carves == carvesBefore, "O8 pins on live rerouted resources carve nothing");
    const uint32_t W = A::mmAllocate(0x0DFF0000u, 0x20000404u, 0u, 0xFFFFFFFFu, 0x8000u);
    check(W != 0, "O9 after rerouting the world pool FITS (the pf1 failure is gone)");
  }

  // ── H-STACK (iter-458 pf3a): the harness's main stack is never handed out ──
  A::resetForTest(A::kLo, A::kHi);
  {
    const uint32_t sLo = sr::kernel::kMainStackBase;
    const uint32_t sHi = sLo + sr::kernel::kMainStackSize;
    auto hits = [&](uint32_t va, uint32_t sz) {
      return va != 0 && va < sHi && (uint64_t)va + sz > sLo;
    };
    check(A::stats().hostReservedBytes == sr::kernel::kMainStackSize,
          "H1 the main stack (1 MB) is excluded from the arena");
    check(A::stats().freeBytes == (uint64_t)(A::kHi - A::kLo) - sr::kernel::kMainStackSize,
          "H2 free bytes = arena - 1 MB");
    // Page by page from the top through the REAL guestPhysAlloc.
    bool overlap = false;
    uint32_t last = 0;
    uint32_t n = 0;
    for (;;) {
      const uint32_t va = sr::kernel::guestPhysAlloc(0x1000, 0x1000);
      if (!va) break;
      overlap |= hits(va, 0x1000);
      last = va;
      n++;
      if (va < sLo) break;
    }
    check(!overlap, "H3 page by page from the top: no page ever lands in the stack");
    check(last != 0 && last + 0x1000u == sLo, "H4 the first page below the stack ends at its base");
    check(n == (A::kHi - sHi) / 0x1000u + 1u, "H5 all 15 MB above the stack were used first");
    const uint32_t big = A::alloc(0x1000000u, 0x1000);
    check(big != 0 && !hits(big, 0x1000000u), "H6 a 16 MB block lands clear of the stack");
    const uint64_t r0 = A::stats().refusedFrees;
    check(!A::free(sLo) && !A::free(sLo + 0x10000u) && A::stats().refusedFrees == r0 + 2,
          "H7 frees inside the stack are refused and counted");
    const uint32_t R = A::mmAllocate(0x12B00000u, 4u, 0u, 0xFFFFFFFFu, 0u);
    check(R != 0 && !hits(R, 0x12B00000u), "H8 the 299 MB reservation still fits, clear of the stack");
    A::free(R);
    const uint32_t Wp = A::mmAllocate(0x0DFF0000u, 0x20000404u, 0u, 0xFFFFFFFFu, 0x8000u);
    check(Wp != 0 && !hits(Wp, 0x0DFF0000u), "H9 the world pool still fits, clear of the stack");
    check(A::stats().hostOverlapAllocs == 0, "H11 no allocation above touched the stack (counter)");
  }
  A::resetForTest(0xA0000000u, 0xA1000000u);
  check(A::stats().hostReservedBytes == 0 && A::stats().freeBytes == 0x1000000u,
        "H10 an arena without the stack is unchanged");

  // ── H-STACK boundaries (Astra 031): free / coalesce / reallocate at both edges ──
  A::resetForTest(A::kLo, A::kHi);
  {
    const uint32_t sLo = sr::kernel::kMainStackBase;
    const uint32_t sHi = sLo + sr::kernel::kMainStackSize;
    const uint32_t pLo = sLo & 0x1FFFFFFFu, pHi = sHi & 0x1FFFFFFFu;
    const uint32_t RW = A::kXPageReadWrite;
    const uint32_t above = A::mmAllocate(0x1000, RW, pHi, pHi + 0xFFFu, 0);
    const uint32_t below = A::mmAllocate(0x1000, RW, pLo - 0x1000u, pLo - 1u, 0);
    check(above == sHi && below == sLo - 0x1000u, "B1 the pages touching both edges are allocatable");
    check(A::mmAllocate(0x1000, RW, pLo, pHi - 1u, 0) == 0, "B2 a range entirely inside the stack FAILS");
    check(A::mmAllocate(0x1000, RW, pLo + 0x80000u, pLo + 0x80FFFu, 0) == 0, "B2b so does its middle page");
    A::free(above);
    A::free(below);
    check(A::mmAllocate(0x2000, RW, pLo - 0x1000u, pHi + 0xFFFu, 0) == 0,
          "B3 after both edges are freed, nothing coalesces ACROSS the stack (2 pages cannot straddle it)");
    check(A::stats().freeBytes == (uint64_t)(A::kHi - A::kLo) - sr::kernel::kMainStackSize,
          "B4 free bytes back to arena - 1 MB after the edge frees");
    check(A::mmAllocate(0x1000, RW, pHi, pHi + 0xFFFu, 0) == sHi &&
              A::mmAllocate(0x1000, RW, pLo - 0x1000u, pLo - 1u, 0) == sLo - 0x1000u,
          "B5 the edge pages are reallocated in place");
    check(A::stats().hostOverlapAllocs == 0, "B6 no boundary allocation touched the stack");
  }
  // Partial overlaps: the INTERSECTION is carved.
  A::resetForTest(0xBE080000u, 0xBE200000u);
  check(A::stats().hostReservedBytes == 0x80000u && A::stats().freeBytes == 0x100000u,
        "B7 arena overlapping the stack's top half: [BE080000,BE100000) carved");
  check(A::alloc(0x100000u, 0x1000) == 0xBE100000u && A::alloc(0x1000, 0x1000) == 0,
        "B8 only the space above is allocatable");
  A::resetForTest(0xBDF00000u, 0xBE080000u);
  check(A::stats().hostReservedBytes == 0x80000u && A::stats().freeBytes == 0x100000u &&
            A::alloc(0x1000, 0x1000) == 0xBDFFF000u,
        "B9 arena overlapping the stack's bottom half: allocation starts below it");
  // The counter itself (test seam: no carve).
  A::resetForTestNoHostCarve(0xBE000000u, 0xBE200000u);
  {
    const uint32_t x = A::alloc(0x100000u, 0x1000);
    const uint32_t y = A::alloc(0x1000u, 0x1000);
    check(x == 0xBE100000u && y == 0xBE0FF000u && A::stats().hostOverlapAllocs == 1,
          "B10 without the carve, the overlapping allocation IS counted (and only it)");
  }

  // ── concurrency ─────────────────────────────────────────────────────────────
  A::resetForTest(A::kLo, A::kLo + 0x04000000u);   // 64 MB
  std::atomic<int> corrupt{0}, allocFail{0};
  std::vector<std::thread> th;
  for (int t = 0; t < 8; t++) {
    th.emplace_back([t, &corrupt, &allocFail] {
      std::mt19937 rng(1234u + t);
      std::vector<std::pair<uint32_t, uint32_t>> mine;
      for (int i = 0; i < 1500; i++) {
        if (mine.empty() || (rng() & 1u)) {
          const uint32_t sz = 0x1000u * (1u + rng() % 16u);
          const uint32_t va = A::alloc(sz, 0x1000u << (rng() % 3u));
          if (!va) { allocFail++; continue; }
          if (uint8_t* p = sr::kernel::guestVAToHost(va)) std::memset(p, 0x10 + t, sz);
          mine.push_back({va, sz});
        } else {
          const size_t k = rng() % mine.size();
          const auto blk = mine[k];
          if (const uint8_t* p = sr::kernel::guestVAToHost(blk.first))
            for (uint32_t j = 0; j < blk.second; j += 512)
              if (p[j] != 0x10 + t) { corrupt++; break; }
          if (!A::free(blk.first)) corrupt++;
          mine.erase(mine.begin() + (long)k);
        }
      }
      for (const auto& blk : mine) A::free(blk.first);
    });
  }
  for (auto& x2 : th) x2.join();
  check(corrupt.load() == 0, "C1 8 threads x 1500 ops: no overlap corruption, every free accepted");
  check(A::stats().liveBytes == 0 && A::stats().liveBlocks == 0, "C2 all memory returned");
  const uint32_t full = A::alloc(0x04000000u, 0);
  check(full == A::kLo, "C3 fully coalesced after the storm (whole arena fits)");
  (void)allocFail;

  std::printf("[PHYSALLOC_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
