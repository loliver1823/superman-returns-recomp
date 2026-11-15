#include "guest_context.hpp"
#include "sr_oracle.hpp"
#include "native_hardware_memory.hpp"
#include "guest_protect.hpp"  // 2026-10-14: releaseGuestArena retires arithmetic admission first
#include "phys_alloc.hpp"
#include "phys_window.hpp"   // iter-458 H-LIFO: registry lifetimes under the allocator lock
#include "phys_lifo.hpp"     // iter-463: LIFO reuse + floor
#include "gpu_hang_wd.hpp"   // iter-463: the (f) allocator provider for GPU_HANG_WD
#include "sr_timebase.hpp"   // iter-463: 50 MHz guest clock domain (SR_TIMEBASE_50MHZ)
#include "../app/log.hpp"
#include "../gfx/d3d12_backend.hpp"
#include "../gfx/null_renderer.hpp"
#include "../gfx/xenos_state.hpp"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <string>
#include <thread>
#include <unordered_set>
#if defined(_WIN32)
#include <windows.h>
#endif

// Process-wide guest base for external debugger tools (Frida, WinDbg, TTD).
// Set once by the harness thread on first setGuestMemory call.
// NOT thread-local — intentionally process-global so debuggers can find it by
// symbol name: `g_debugGuestBase` → add guest VA offset → host address.
extern "C" __declspec(dllexport) uint8_t* g_debugGuestBase = nullptr;
// iter-188: dispatch-table debug exports so Frida can inverse-map a host
// instruction pointer to the containing recomp function's guest VA:
// slot i holds the host fn for guest VA (g_debugDispatchCodeBase + i*4).
extern "C" __declspec(dllexport) void** g_debugDispatchTable = nullptr;
extern "C" __declspec(dllexport) uint64_t g_debugDispatchTableEntries = 0;
extern "C" __declspec(dllexport) uint32_t g_debugDispatchCodeBase = 0;

// iter-364: live effect-compiler +104 region (defined at GLOBAL scope in
// cs_timeout_underflow_fix.cpp); guestHeapAlloc reports grants that overlap it so the
// clobbering caller of the allocator-overlap effect-compile failure is named.
#include <atomic>
#if SR_ORACLE
extern std::atomic<uint32_t> g_fxRegionAddr;
#endif

namespace sr {
namespace kernel {

namespace {
  // 2026-04-24 wave 9 — proper fix for the multi-thread allocator race.
  // Audit (3 read-only agents) found: setGuestMemory is called from the
  // harness AND from runGuestThreadEntry on every worker/render-thread spawn,
  // and resets the bump pointers each time. With thread_local storage, every
  // thread got its own zero-initialized counters → workers handed out the same
  // VAs as the main thread (silent corruption). With naive global storage
  // (wave-8 first attempt), each spawn re-zeroed the globals, breaking main
  // thread allocations.
  //
  // Fix: demote shared allocator state to process-global. Make setGuestMemory
  // idempotent via a CAS guard so subsequent calls only set the per-thread
  // (g_guestBase, g_guestSize) pair without touching the global bumps.
  //
  // Per-thread (worker/render thread re-runs setGuestMemory for these via
  // runGuestThreadEntry to make PPC_LOAD/STORE work — also mirrored to
  // g_exCreateThreadBase/Size via registerExCreateThreadRunner for non-guest
  // host threads).
  thread_local uint8_t* g_guestBase = nullptr;
  thread_local size_t g_guestSize = 0;
  thread_local uint32_t g_currentGuestLR = 0;

  // Process-global allocator state (was thread_local). Initialized once on
  // the harness thread by setGuestMemory; read-only thereafter (with atomic
  // bump-counter writes for actual allocations).
  std::atomic<size_t> g_heapNext{0};
  size_t g_heapStart = 0;
  size_t g_heapSize = 0;
  // Fallback heap so malloc stub never returns 0 (avoids bugcheck when main heap exhausted or cap hit).
  constexpr size_t kFallbackHeapSize = 16 * 1024 * 1024;  // 16 MB
  uint32_t g_fallbackHeapVA = 0;
  std::atomic<size_t> g_fallbackHeapNext{0};
  // Phys region grows DOWN from kPhysTop (=0xBF000000) toward kPhysBase (=0x84000000).
  // Primary heap grows UP from g_heapStart (=0x82AE0000) with an upper bound at
  // (kPhysTop - g_physNext). Both allocators fail when they would meet.
  constexpr uint32_t kPhysBase = 0x84000000u;
  // iter-173: raised from 768MB to 944MB (kPhysTop=0xBF000000) so the game's 10 × 128MB
  // physical allocations before VdInitializeRingBuffer all succeed.  Must stay < 0xC0000000
  // so phys VAs are not remapped by aliasGuestVA (which aliases 0xC0-0xDF → 0x80-0x9F).
  constexpr size_t kPhysSizeMax = 944 * 1024 * 1024;  // 944 MB → kPhysTop = 0xBF000000
  constexpr uint32_t kPhysTop = kPhysBase + static_cast<uint32_t>(kPhysSizeMax);  // 0xBF000000
  std::atomic<size_t> g_physNext{0};  // bytes consumed from the top
  size_t g_physSize = 0;
  constexpr size_t kGfxBufferSize = 65536;
  uint32_t g_gfxBufferVA = 0;

  // Idempotency guard for setGuestMemory.
  std::atomic<bool> g_globalAllocatorInitialized{false};

  // iter-364 PROPER ALLOCATOR COORDINATION: registry of VA ranges placed OUTSIDE guestHeapAlloc's
  // bump (NtAVM commit-in-place, DOGMA pool, guest dlmalloc arenas). guestHeapAlloc skips these so
  // its forward bump never overlaps another allocator's live memory. Deterministic (no content
  // heuristic). Append-only, low-cardinality (big reservations only); linear scan is cheap.
  struct ResvRange { std::atomic<uint32_t> start{0}, end{0}; };
  constexpr int kResvMax = 1024;
  ResvRange g_resv[kResvMax];
  std::atomic<int> g_resvCount{0};
  // If [va, va+size) overlaps any reserved range, return the MAX end of the overlapping ranges
  // (the VA to skip the bump past); else 0. Iterated to a fixpoint by the caller.
  uint32_t reservedOverlapEnd(uint32_t va, uint32_t size) {
    const uint32_t vaEnd = va + size;
    uint32_t skipTo = 0;
    int n = g_resvCount.load(std::memory_order_acquire);
    if (n > kResvMax) n = kResvMax;
    for (int i = 0; i < n; ++i) {
      const uint32_t s = g_resv[i].start.load(std::memory_order_relaxed);
      if (!s) continue;
      const uint32_t e = g_resv[i].end.load(std::memory_order_relaxed);
      if (va < e && vaEnd > s && e > skipTo) skipTo = e;  // overlap; skip past this range
    }
    return skipTo;
  }

  // Xbox 360 kernel/image data region constants
  // The game accesses data starting at image base (0x82000000).
  // The first few KB of this region contain kernel-like structures.
  constexpr uint32_t kKernelDataBase = 0x82000000u;  // Same as PPC_IMAGE_BASE
  
  // Key offsets in kernel data that games access:
  // 0x768 (1896): Pointer to current XEX module info (XEX_EXECUTION_ID / module list)
  // 0x900 (2304): Pointer to current process/title info (KPROCESS / notification list)
  constexpr uint32_t kKernelModuleInfoPtrOffset = 0x768;
  constexpr uint32_t kKernelProcessInfoPtrOffset = 0x900;
  
  // Helper to write big-endian 32-bit value to guest memory
  inline void writeGuestBE32(uint8_t* host, uint32_t value) {
    host[0] = (value >> 24) & 0xFF;
    host[1] = (value >> 16) & 0xFF;
    host[2] = (value >> 8) & 0xFF;
    host[3] = value & 0xFF;
  }
}

namespace hrr {
std::atomic<uintptr_t> g_resBase{0}, g_resEnd{0};
}

// iter-461 perf: the guest macros' inline view of the per-thread base, and the
// flags that gate the store watchpoints / the indirect-call tracer.
thread_local constinit uint8_t* g_guestBaseTls = nullptr;  // iter-465 perf2: see the header

// iter-465 perf2: aliasGuestVA's cold half lives in phys_window.cpp next to
// foldPhysAlias (same TU, so the fold inlines into it). The 0xA identity flag:
bool g_physAliasAIdentity = [] {
  const char* e = std::getenv("SR_PHYS_ALIAS_A");
  return !(e && e[0] == '1');
}();
bool g_storeWatchOn = false;
bool g_traceCallsOn = false;

bool logVerbose() {
  static const bool v = [] {
    const char* e = std::getenv("SR_LOG_VERBOSE");
    return e && e[0] == '1';
  }();
  return v;
}

void setGuestMemory(uint8_t* base, size_t totalSize) {
  // Always set the per-thread base/size — workers and render-thread call this
  // on entry so PPC_LOAD/STORE macros resolve via TLS.
  g_guestBase = base;
  g_guestBaseTls = base;
  g_guestSize = totalSize;
  // iter-458 H-VQ: the process-global reservation identity for the
  // hostRangeReadable cache (the TLS pair above is per thread). A different
  // reservation invalidates every cached interval.
  if (base && totalSize) {
    const uintptr_t b = reinterpret_cast<uintptr_t>(base);
    const uintptr_t en = b + totalSize;
    if (hrr::g_resBase.load(std::memory_order_acquire) != b ||
        hrr::g_resEnd.load(std::memory_order_acquire) != en) {
      hrr::g_resBase.store(b, std::memory_order_release);
      hrr::g_resEnd.store(en, std::memory_order_release);
      hostRangeReadableInvalidate();
    }
  }
  // Idempotent global init: only the first caller (the harness thread) sets up
  // the process-global heap/phys layout. Subsequent worker/render-thread spawns
  // skip this so they don't zero the bump pointers out from under existing
  // allocations.
  if (g_globalAllocatorInitialized.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
#if SR_ORACLE
  {
    const char* sw = std::getenv("SR_STORE_WATCH");
    if (sw && sw[0] == '1') g_storeWatchOn = true;
    const char* tc = std::getenv("SR_TRACE_CALLS");
    if (tc && tc[0] == '1') g_traceCallsOn = true;
  }
#endif
#if !SR_ORACLE
  uint32_t hardwareError = 0;
  // The NOACCESS makes the guest's hardware range unreadable: the readable-interval cache's removal protocol (step 6
  // G3, the coordinator 10-24: it was a plain generation bump AFTER the protect, which a lookup between the two could
  // still answer from). tools/test_hrr_invalidation_surface.py checks this caller-side wrap.
  hostRangeReadableBeginRemoval();
  const bool hardwareProtected = protectNativeHardwareMemory(base, totalSize, &hardwareError);
  hostRangeReadableEndRemoval();
  if (!hardwareProtected) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_MMIO_BAD] cannot protect hardware reservation: error=%u", hardwareError);
    std::_Exit(3);
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_MMIO] no-access MMIO reservation 7FC80000..7FFFFFFF");
#endif
  g_debugGuestBase = base;  // expose to Frida/WinDbg/TTD by symbol name
  g_heapNext.store(0);
  g_fallbackHeapNext.store(0);
  // 2026-04-24: dispatch table moved to a host-only allocation (see guest_harness.cpp
  // sr::kernel::g_dispatchTable). The former carve-out at 0x829E0000..0x83C00000 is
  // gone; the range is now plain zeroed guest RAM. Heaps pack immediately after the
  // guest image.
  //
  // VA OVERLAP FIX (2026-04-24 wave 6, applied):
  // Previously the primary heap [0x82AE0000, totalSize) and the phys region
  // [0x84000000, 0xB4000000) overlapped VA-wise; within 6 ms of boot the primary heap
  // bump pointer crossed 0x84000000 and every MmAllocatePhysicalMemoryEx silently
  // stomped primary-heap-tracked state (including DOGMA's pool backing), producing the
  // "60th CTOR perturbs CTOR #30" non-determinism. Now fixed by unifying both bump
  // allocators into a shared window [0x82AE0000, 0xBF000000): primary heap grows UP
  // from g_heapStart, phys grows DOWN from kPhysTop (0xBF000000). See guestHeapAlloc
  // and guestPhysAlloc below — both check that heap-high and phys-low don't cross.
  // Phys VAs remain in [0x84000000, 0xBF000000) (just starting near the top) so guest
  // code's range expectations are preserved.
  constexpr size_t kImageBase = 0x82000000ull;
  constexpr size_t kImageSize = 0x9E0000ull;
  constexpr size_t kFallbackHeapStart = kImageBase + kImageSize;

  g_fallbackHeapVA = (totalSize >= kFallbackHeapStart + kFallbackHeapSize)
                   ? static_cast<uint32_t>(kFallbackHeapStart) : 0;
  // Primary heap starts right after the fallback heap and, in cooperation with the
  // downward-growing phys region, is bounded above by kPhysTop (0xB4000000). This is
  // the single dividing window both allocators share: heap grows up from g_heapStart,
  // phys grows down from kPhysTop, and alloc fails when they would meet.
  g_heapStart = kFallbackHeapStart + kFallbackHeapSize;
  size_t maxHeapCap = (kPhysTop > g_heapStart) ? (static_cast<size_t>(kPhysTop) - g_heapStart) : 0;
  size_t totalAvail = (totalSize > g_heapStart) ? (totalSize - g_heapStart) : 0;
  g_heapSize = (totalAvail < maxHeapCap) ? totalAvail : maxHeapCap;
  // Reserve GPU buffer at start of heap so it has a stable VA.
  g_gfxBufferVA = (g_heapSize >= kGfxBufferSize) ? static_cast<uint32_t>(g_heapStart) : 0;
  if (g_gfxBufferVA) g_heapNext.store(kGfxBufferSize, std::memory_order_relaxed);
  // Physical region: contiguous range for MmAllocatePhysicalMemoryEx (DOGMA pool etc.).
  // Grows DOWN from kPhysTop, so first alloc of size S returns VA = kPhysTop - S.
  g_physNext.store(0, std::memory_order_relaxed);
  size_t physEnd = static_cast<size_t>(kPhysBase) + kPhysSizeMax;
  g_physSize = (totalSize >= physEnd) ? kPhysSizeMax : (totalSize > kPhysBase ? totalSize - kPhysBase : 0);
}

// iter-450S: host window focus (see guest_context.hpp). Plain atomic — the
// app's SDL pump writes, guest-thread input stubs read; no ordering needed
// beyond visibility.
static std::atomic<bool> g_hostWindowFocused{false};
void setHostWindowFocused(bool focused) {
  g_hostWindowFocused.store(focused, std::memory_order_relaxed);
}
bool hostWindowFocused() {
  return g_hostWindowFocused.load(std::memory_order_relaxed);
}

uint8_t* getGuestBase() {
  return g_guestBase;
}

size_t getGuestSize() {
  return g_guestSize;
}

// ── iter-460 S1: CPU-write detection (Windows write watch on the arena) ──────
namespace {
std::atomic<bool> g_writeWatchActive{false};
}
bool guestWriteWatchWanted() {
  static const bool v = [] {
    const char* e = std::getenv("SR_WRITE_WATCH");
    // Opt-in only: an arena under MEM_WRITE_WATCH cost the live intro ~47 ms
    // of guest time per frame (p2_ww1 vs p2_ww0: 7.4 -> 12.0 swaps/s) - every
    // per-present reset re-arms the ~2000 pages the guest writes each frame.
    return e && e[0] == '1';
  }();
  return v;
}
void setGuestWriteWatchActive(bool active) { g_writeWatchActive.store(active, std::memory_order_release); }
bool guestWriteWatchActive() { return g_writeWatchActive.load(std::memory_order_acquire); }
size_t guestCollectWrittenPages(uint32_t va, uint32_t size, std::vector<uint32_t>& outVa) {
#if defined(_WIN32)
  if (!guestWriteWatchActive() || !g_guestBase || !size) return 0;
  if (uint64_t(va) + size > g_guestSize) return 0;
  thread_local std::vector<PVOID> addrs;
  const size_t maxPages = (size_t(size) + 4095u) / 4096u;
  if (addrs.size() < maxPages) addrs.resize(maxPages);
  ULONG_PTR count = ULONG_PTR(maxPages);
  DWORD gran = 0;
  if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_guestBase + va, size, addrs.data(), &count, &gran) != 0)
    return 0;
  for (ULONG_PTR i = 0; i < count; i++)
    outVa.push_back(uint32_t(static_cast<uint8_t*>(addrs[i]) - g_guestBase));
  return size_t(count);
#else
  (void)va; (void)size; (void)outVa;
  return 0;
#endif
}

// Effective (base, size) for the current thread: the TLS pair when set
// (guest threads call setGuestMemory on entry), else the process-global pair
// registered via registerExCreateThreadRunner for non-guest host threads.
// FIX: readGuestU32BE/writeGuestBE32/isGuestVAInRange bounds-checked against
// the raw TLS g_guestSize, which is 0 on host threads that never called
// setGuestMemory — so every cross-thread guest read returned 0 and every
// write silently no-opped (e.g. FM_WORLD_PATCHER never actually patched).
static void getEffectiveGuestMemory(uint8_t*& base, size_t& size) {
  base = g_guestBase;
  size = g_guestSize;
  if (!base) {
    size_t exSize = 0;
    (void)getExCreateThreadRunner(&base, &exSize);
    size = exSize;
  }
}

uint8_t* guestVAToHost(uint32_t guestVA) {
  // Apply VA aliasing so uncached/WC reads hit the same memory as cached stores.
  guestVA = aliasGuestVA(guestVA);
  uint8_t* base = nullptr;
  size_t size = 0;
  getEffectiveGuestMemory(base, size);
  if (!base || guestVA >= size) return nullptr;
  return base + guestVA;
}

bool isGuestVAInRange(uint32_t guestVA) {
  guestVA = aliasGuestVA(guestVA);
  uint8_t* base = nullptr;
  size_t size = 0;
  getEffectiveGuestMemory(base, size);
  if (static_cast<uint64_t>(guestVA) + 4 > size) return false;
  return true;
}

static uint32_t readBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

static void writeBE32(uint8_t* p, uint32_t value) {
  p[0] = (value >> 24) & 0xFF;
  p[1] = (value >> 16) & 0xFF;
  p[2] = (value >> 8) & 0xFF;
  p[3] = value & 0xFF;
}

uint32_t readGuestU32BE(uint32_t guestVA) {
  uint32_t aliased = aliasGuestVA(guestVA);
  uint8_t* base = nullptr;
  size_t size = 0;
  getEffectiveGuestMemory(base, size);
  if (!base || static_cast<uint64_t>(aliased) + 4 > size) return 0;
  return readBE32(base + aliased);
}

void writeGuestBE32(uint32_t guestVA, uint32_t value) {
  uint32_t aliased = aliasGuestVA(guestVA);
  uint8_t* base = nullptr;
  size_t size = 0;
  getEffectiveGuestMemory(base, size);
  if (base && static_cast<uint64_t>(aliased) + 4 <= size)
    writeBE32(base + aliased, value);
}

// iter-357l: published by the XMV ASF-walk probe; when nonzero,
// guestHeapAlloc reports any grant overlapping this live guest object.
std::atomic<uint32_t> g_xmvParserWatch{0};

// iter-364: register a VA range placed outside guestHeapAlloc's bump (NtAVM commit-in-place,
// DOGMA pool, guest dlmalloc arenas) so the bump skips it. Append-only; low cardinality.
void registerReservedRange(uint32_t start, uint32_t end) {
  if (end <= start) return;
  // Only ranges that could collide with guestHeapAlloc's bump window are worth tracking.
  if (end <= static_cast<uint32_t>(g_heapStart) || start >= kPhysTop) return;
  const int i = g_resvCount.fetch_add(1, std::memory_order_relaxed);
  if (i >= kResvMax) return;  // registry full — extremely unlikely for big reservations
  g_resv[i].end.store(end, std::memory_order_relaxed);
  g_resv[i].start.store(start, std::memory_order_release);  // publish start last (reader gate)
  static std::atomic<uint32_t> s_rr{0};
  uint32_t rn = s_rr.fetch_add(1, std::memory_order_relaxed) + 1;
  if (rn <= 24 || (rn & (rn - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[HEAP_RESV_REG] #%u reserved [0x%08X,0x%08X) size=0x%X (slot %d)", rn, start, end, end - start, i);
}

// iter-364 CENTRAL HEAP COORDINATION (the deterministic root-fix that retires
// SR_HEAP_SKIP_OCCUPIED). The guest's OWN dlmalloc (sub_8246CA80, bit31-clear via
// sub_82467F40) carves chunks from a wilderness that sits INSIDE guestHeapAlloc's forward-bump
// window [g_heapStart, kPhysTop) but is NOT tracked by g_heapNext (it never routes through
// guestHeapAlloc — proven by the [NTAVM]/[MSTATE3] arena trace: fake mstate 0x82110000, no
// NtAVM, no segment-install). So guestHeapAlloc's bump climbs INTO the live dlmalloc region and
// its zero-fill clobbers a live chunk (the D3DX effect +104 region, the VB/WMA/video walls —
// one class). Coordinate the two allocators by a single invariant: guestHeapAlloc's frontier
// (g_heapNext) MUST stay at or above the guest dlmalloc's highest live chunk. Each time the
// dlmalloc hands out a chunk in our window, raise g_heapNext past it (atomic max). Then
// guestHeapAlloc only ever grants ABOVE every dlmalloc chunk — disjoint by construction, no
// content heuristic, no per-chunk registry. Same-thread ordering (the effect compiler does the
// dlmalloc alloc, THEN the colliding guestHeapAlloc grant) makes this race-free.
void guestHeapReserveThrough(uint32_t vaEnd) {
  if (!g_guestBase) return;
  const uint32_t hs = static_cast<uint32_t>(g_heapStart);
  if (vaEnd <= hs || vaEnd > kPhysTop) return;  // outside the bump window
  size_t want = (static_cast<size_t>(vaEnd) - hs + 15) & ~size_t(15);
  if (want > g_heapSize) return;                // never advance past the cap
  size_t cur = g_heapNext.load(std::memory_order_relaxed);
  while (want > cur &&
         !g_heapNext.compare_exchange_weak(cur, want, std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
    // cur reloaded by CAS on failure; loop until want<=cur or the max is committed.
  }
}

// iter-437f: large grants are PAGE-ALIGNED, matching the console heap's
// big-block contract (RtlAllocateHeap_sub_8246CA80: requests over the
// heap's VirtualMemoryThreshold go straight to NtAllocateVirtualMemory =
// page-aligned base). Our 16B-aligned bump handed the movie decoder plane
// buffers at +0x280/+0xA80 offsets; the Xenos texture fetch encodes base
// bits [31:12] ONLY, silently flooring to the page — the V plane sampled
// 3.5 chroma rows early and (V-flipped) painted the movie bottom-strip
// chroma garbage the user reported. Alignment forward-only on a monotonic
// bump: no overlap risk, only slack. SR_HEAP_ALIGN4K=0 restores for A/B.
static bool heapAlign4KEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_HEAP_ALIGN4K");
    return !e || e[0] != '0';
  }();
  return v;
}

static bool heapCursorAlignEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_HEAP_CURSOR_ALIGN");
    return !e || e[0] != '0';
  }();
  return v;
}

uint32_t guestHeapAlloc(size_t size) {
  if (!g_guestBase || g_heapSize == 0) return 0;
  size = (size + 15) & ~15ull;
  // iter-357l PLATFORM FIX (SR_HEAP_GAP=0 disables): a real RtlAllocateHeap
  // never places the next user block immediately at prevBlock+size — every
  // block carries the heap's own metadata (header/boundary tag), so adjacent
  // user regions are separated. Our bump packed them with ZERO gap, so any
  // guest that touches slightly past its request corrupts the NEXT LIVE
  // OBJECT instead of recoverable metadata.
  // Measured victim (movie blocker): the XMV ASF parser is handed a 0x280
  // block; its payload sits at block+0x20 and runs to block+0x290, so the
  // following grant landed on the live parser's reader back-pointer
  // (parser+0x260) and guestHeapAlloc's own zeroing memset killed it ->
  // header read fails -> XMV create E_FAIL -> no movie decodes.
  // Model the console's per-block metadata with a separation gap.
  // iter-357l VERDICT: REFUTED as a fix — default 0 (OFF). A 32-byte gap
  // removes the parser overlap but REGRESSES the boot hard (K=2: init walks
  // 4/4 -> 0/2, wedged at FMGameRtam 0x8295F83C, zero scripts, zero movies),
  // so some earlier consumer depends on our current packing/addresses. The
  // overlap itself is real and still the movie blocker — but the fix is NOT
  // a blanket gap. Kept as a one-line A/B knob (SR_HEAP_GAP=32) for the
  // next investigation; do not re-chase it as a default.
  static const size_t s_gap = [] {
    const char* e = std::getenv("SR_HEAP_GAP");
    return e ? (size_t)strtoul(e, nullptr, 0) : (size_t)0;
  }();
  // iter-364 PROPER ALLOCATOR COORDINATION. guestHeapAlloc's forward bump
  // [g_heapStart=0x839E0000, kPhysTop) shares VA space with the guest dlmalloc heap and the
  // NtAVM commit-in-place ranges (DOGMA pool, dlmalloc arenas), which are placed at guest-chosen
  // bases WITHOUT reserving from g_heapNext. So a plain bump can re-grant memory another allocator
  // already owns and this function's zero-fill clobbers it (proven: the D3DX effect-compiler +104
  // region, [FXCLOB3] — the whole 0/66 effect-compile failure).
  //  * SR_HEAP_RESERVE (default-ON): the DETERMINISTIC layer — consult the reserved-range registry
  //    (populated by registerReservedRange from the placements we CAN observe: NtAVM commit-in-
  //    place) and skip past any reserved range the candidate hits. Exact, no false-free/race.
  //  * SR_DLMALLOC_FENCE (default-ON, in gpu_ring_alloc_fix.cpp sub_82467F40) is the PRIMARY,
  //    ADDRESS-BASED root fix for the bit31-clear overlap: it raises g_heapNext past every guest
  //    dlmalloc chunk so this bump can never re-grant one. Proven (iter-364, K=4 det): fence ON +
  //    both layers below OFF => 33/33 effects compile, no boot regress. ROOT (arena trace [NTAVM]/
  //    [MSTATE3]): the region is a guest DLMALLOC chunk (sub_8246CA80 on the fake mstate 0x82110000)
  //    whose wilderness sits in this bump window but never routes through guestHeapAlloc, so
  //    g_heapNext didn't stay above it. (Earlier note blaming the "game pool sub_82468008" was the
  //    backwards-IDA-decompile error; bit31-CLEAR is the dlmalloc path, bit31-SET is the pool.)
  //  * SR_HEAP_SKIP_OCCUPIED (default-OFF, RETIRED): the old content heuristic — skip a candidate
  //    whose leading window is non-zero (a live neighbor). Superseded by SR_DLMALLOC_FENCE (exact,
  //    address-based, no false-free on all-zero live objects). Kept as an A/B knob only.
  static const bool s_reserveFix = [] {
    const char* e = std::getenv("SR_HEAP_RESERVE");
    return !e || e[0] != '0';
  }();
  static const bool s_skipOcc = [] {
    const char* e = std::getenv("SR_HEAP_SKIP_OCCUPIED");
    return e && e[0] == '1';  // RETIRED: default-OFF, superseded by SR_DLMALLOC_FENCE
  }();
  // iter-437f: large blocks (>=64KB — the console heap's big-block class)
  // start on a 4KB page, because texture fetch bases can only encode
  // page-aligned addresses (see heapAlign4KEnabled above). Implemented as a
  // request-size pad + base round-up inside the claimed span, so the CAS
  // accounting below stays exact.
  const bool alignLarge = heapAlign4KEnabled() && size >= 0x10000;
  // iter-462: the pad is 0x1000, not 0xFFF. An odd pad advanced the bump
  // cursor to 15 mod 16, and every later grant (RtlAllocateHeap, NtAVM, the
  // DOGMA sanitizer's replacements) came back byte-misaligned - the console
  // heap never returns less than 16-byte alignment. In 11 of 13 runs with a
  // misaligned cursor the guest's small-bin pool (MemPoolAlloc sub_82182700)
  // later threaded its free list through XEX .rdata vtables and killed the
  // D-pad power select (0x8208D29C, EventButtonMapped); 0 of 3 aligned runs
  // did. From a 16-aligned base the page round-up needs at most 0xFF0.
  // SR_HEAP_CURSOR_ALIGN=0 restores the odd pad for A/B.
  if (alignLarge) size += heapCursorAlignEnabled() ? 0x1000 : 0xFFF;
  size_t prev;
  uint32_t va;
  for (int tries = 0; ; ++tries) {
    prev = g_heapNext.load(std::memory_order_relaxed);
    if (prev + size > g_heapSize) return 0;
    // Coordinate with phys region: primary heap high-water must not cross phys low-water.
    // phys low-water VA = kPhysTop - g_physNext; heap high-water VA = g_heapStart + prev + size.
    size_t physUsed = g_physNext.load(std::memory_order_relaxed);
    size_t physLowVA = static_cast<size_t>(kPhysTop) - physUsed;
    // iter-458 H-FREE: with the reusable allocator the physical arena is the
    // FIXED [0xA0000000, 0xBF000000); the heap may grow up to its floor.
    if (physalloc::physFreeEnabled()) physLowVA = physalloc::kLo;
    if (g_heapStart + prev + size > physLowVA) {
      static std::atomic<int> warned{0};
      if (warned.fetch_add(1, std::memory_order_relaxed) < 4) {
        fprintf(stderr, "[HEAP_CAP] guestHeapAlloc(%zu) would cross phys low-water: heapHigh=0x%zX physLow=0x%zX — refusing\n",
                size, g_heapStart + prev + size, physLowVA);
      }
      return 0;
    }
    va = static_cast<uint32_t>(g_heapStart + prev);
    if (tries >= 4096) break;
    // (1) DETERMINISTIC: skip past any registered reserved range the candidate overlaps.
    if (s_reserveFix) {
      const uint32_t skipTo = reservedOverlapEnd(va, static_cast<uint32_t>(size));
      if (skipTo) {  // overlap (skipTo = end of the overlapping reserved range)
        const size_t newNext = ((static_cast<size_t>(skipTo) + 15) & ~size_t(15)) - g_heapStart;
        if (newNext > prev) {
          g_heapNext.store(newNext, std::memory_order_relaxed);
          static std::atomic<uint32_t> s_rv{0};
          uint32_t rn = s_rv.fetch_add(1, std::memory_order_relaxed) + 1;
          if (rn <= 16 || (rn & (rn - 1)) == 0)
            SR_LOG_BOOT(sr::LogSeverity::Info,
              "[HEAP_RESV] #%u guestHeapAlloc(0x%zX) VA 0x%08X hits reserved range -> skip to 0x%08X", rn, size, va, skipTo);
          continue;
        }
      }
    }
    // (2) heuristic backstop (content scan) — only if explicitly enabled.
    if (s_skipOcc) {
      uint8_t* probe = guestVAToHost(va);
      size_t scan = size < 0x100 ? size : 0x100;
      bool occupied = false;
      if (probe) for (size_t i = 0; i < scan; ++i) if (probe[i]) { occupied = true; break; }
      if (occupied) {
        g_heapNext.store(prev + size + s_gap, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_skip{0};
        uint32_t sk = s_skip.fetch_add(1, std::memory_order_relaxed) + 1;
        if (sk <= 16 || (sk & (sk - 1)) == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info,
            "[HEAP_SKIP] #%u guestHeapAlloc(0x%zX) skipped OCCUPIED VA 0x%08X (live neighbor) -> retry", sk, size, va);
        continue;
      }
    }
    // iter-364 RACE FIX: atomically CLAIM [prev, prev+size). The bump was load-then-store
    // (non-atomic), so two concurrent guestHeapAlloc callers (e.g. the effect thread's dlmalloc
    // morecore via NtAVM->guestHeapAlloc, and a GPU/other thread's grant) could load the SAME
    // g_heapNext and both get the same VA -> double-grant -> one zero-fills the other's live
    // object (the +104 region clobber). CAS makes each grant unique. If another thread advanced
    // g_heapNext since our load, CAS fails and we retry with the fresh value.
    {
      size_t want = prev + size + s_gap;
      if (g_heapNext.compare_exchange_strong(prev, want, std::memory_order_acq_rel,
                                             std::memory_order_relaxed))
        break;  // claimed [prev, prev+size) exclusively
      // CAS failed: prev now holds the current g_heapNext — retry the loop.
      continue;
    }
  }
  // iter-437f: round the returned base up to the next 4KB page inside the
  // claimed (padded) span. The claim covers [va, va+size) with size+0xFFF
  // slack, so the aligned block still fits.
  if (alignLarge) va = (va + 0xFFFu) & ~0xFFFu;
  // iter-364 ARENA TRACE: log grants in the effect-compiler dlmalloc band so the
  // collision grant (and its prev/g_heapNext) is visible next to the [NTAVM] reserves.
  // Settles rewind-vs-bypass: if a grant returns a VA already owned by a prior [NTAVM]
  // HEAPALLOC reserve, g_heapNext failed to stay above it (rewind); if the VA was never
  // a HEAPALLOC return, the arena bypassed guestHeapAlloc entirely. Gated SR_ARENA_TRACE.
  {
    static const bool s_atrace = [] {
      const char* e = std::getenv("SR_ARENA_TRACE");
      return e && e[0] == '1';
    }();
    if (s_atrace && va >= 0x8A000000u && va < 0x8C000000u) {
      static std::atomic<uint32_t> s_ht{0};
      uint32_t h = s_ht.fetch_add(1, std::memory_order_relaxed) + 1;
      if (h <= 64 || (h & 63) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[HTRACE] #%u guestHeapAlloc -> va=%08X size=%zX prev=%zX heapNext=%zX guestLR=%08X",
          h, va, size, prev, prev + size, getCurrentGuestLR());
    }
  }
  // iter-397n [BIGALLOC]: capped always-on log of large grants (>=256KB) with
  // the guest LR — the movie pipeline's frame buffers / codec ref planes /
  // ring item arrays are all bump grants in one band; the 0x10101010 item-
  // array clobber is an overrun from one of them. base..end + LR names the
  // owner of every big region so the clobbered VA can be matched to its
  // containing/preceding allocation.
  {
    const bool big = size >= 0x40000;
    // any-size grants in the movie ring-item band (item arrays land ~0x925xxxxx)
    const bool band = va >= 0x92400000u && va < 0x92700000u;
    if (big || band) {
      static std::atomic<uint32_t> s_big{0};
      uint32_t b = s_big.fetch_add(1, std::memory_order_relaxed) + 1;
      if (b <= 2000)
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[BIGALLOC] #%u va=%08X..%08X size=%zX guestLR=%08X tid=%u%s",
          b, va, va + static_cast<uint32_t>(size), size, getCurrentGuestLR(),
          getCurrentThreadHandle(), band && !big ? " (band)" : "");
    }
  }
  // iter-361 ALLOC-COORD DIAG: the flush-grind VB data collides with the __imp__
  // pool header at ~0x8FFBExxx. Does the guestHeapAlloc<->guestPhysAlloc
  // cross-check see the pool at collision time? Log the full allocator state for
  // grants in the collision zone: if physLowVA is far ABOVE va, the pool is NOT
  // tracked by g_physNext (a separate uncoordinated allocator = the root); if
  // physLowVA ~= va, the check is racing/off-by. Gated SR_ALLOC_COORD_DIAG.
  {
    static const bool s_coord = [] {
      const char* e = std::getenv("SR_ALLOC_COORD_DIAG");
      return e && e[0] == '1';
    }();
    if (s_coord && va >= 0x8FF00000u && va < 0x90000000u) {
      const size_t physUsed = g_physNext.load(std::memory_order_relaxed);
      const size_t physLowVA = static_cast<size_t>(kPhysTop) - physUsed;
      static std::atomic<int> s_cz{0};
      if (s_cz.fetch_add(1, std::memory_order_relaxed) < 12)
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[ALLOC_COORD] guestHeapAlloc -> 0x%08X size=0x%zX | heapStart=0x%zX "
          "heapHigh=0x%zX g_physNext=0x%zX physLowVA=0x%zX (pool-tracked=%s)",
          va, size, g_heapStart, (size_t)(g_heapStart + prev + size), physUsed,
          physLowVA, (physLowVA <= va + 0x1000) ? "YES" : "NO(separate-pool)");
    }
  }
  // iter-357l: THE MOVIE BLOCKER. The host page-guard caught THIS function's
  // memset zeroing the live XMV ASF parser's reader back-pointer
  // (parser+0x260) — i.e. we handed out a block that overlaps memory the game
  // already owns, then zeroed it. Report any grant overlapping the live
  // parser, with the guest caller, so the owning region can be identified.
  {
    // Ring of recent grants: when an overlap fires, the PRECEDING grant is the
    // one that under-reserved (its payload extends past what we handed out).
    struct Grant { uint32_t lr, sz, va; };
    static Grant s_ring[6] = {};
    static std::atomic<unsigned> s_ringPos{0};
    const uint32_t glr = getCurrentGuestLR();
    unsigned slot = s_ringPos.fetch_add(1, std::memory_order_relaxed) % 6;
    s_ring[slot] = {glr, (uint32_t)size, va};

    extern std::atomic<uint32_t> g_xmvParserWatch;
    uint32_t p = g_xmvParserWatch.load(std::memory_order_relaxed);
    if (p && va < p + 0x270u && va + (uint32_t)size > p) {
      static std::atomic<unsigned> s_n{0};
      if (s_n.fetch_add(1, std::memory_order_relaxed) < 4) {
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[HEAP_DOUBLE_GRANT] guestHeapAlloc(0x%zX) -> 0x%08X OVERLAPS live "
          "guest object [0x%08X,0x%08X) — guestLR=0x%08X heapStart=0x%zX "
          "heapNext=0x%zX",
          size, va, p, p + 0x270u, glr, g_heapStart, prev + size);
        for (int i = 1; i <= 5; ++i) {
          const Grant& g = s_ring[(slot + 6 - i) % 6];
          if (!g.va) continue;
          SR_LOG_BOOT(sr::LogSeverity::Error,
            "[HEAP_DOUBLE_GRANT]   prev-%d: guestHeapAlloc(0x%X) -> 0x%08X "
            "end=0x%08X lr=0x%08X%s",
            i, g.sz, g.va, g.va + g.sz, g.lr,
            (g.va <= p && p < g.va + g.sz + 0x40u) ? "   <<<< the object's own block" : "");
        }
      }
    }
  }
#if SR_ORACLE
  // iter-364: catch the effect-compiler +104 region clobber. g_fxRegionAddr is published
  // (during an armed SR_PASS_TRACE compile) with the live preprocessor region; if THIS grant
  // overlaps it, the memset below zeroes the region's counter -> premature EOF -> all D3DX
  // effect compiles fail. Name the caller so the overlapping guestHeapAlloc consumer is known.
  {
    uint32_t reg = ::g_fxRegionAddr.load(std::memory_order_relaxed);
    if (reg && va <= reg && va + (uint32_t)size > reg) {
      static std::atomic<unsigned> s_n{0};
      if (s_n.fetch_add(1, std::memory_order_relaxed) < 8)
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[FXCLOB3] guestHeapAlloc(0x%zX) -> 0x%08X OVERLAPS effect region 0x%08X — guestLR=0x%08X heapNext=0x%zX",
          size, va, reg, getCurrentGuestLR(), prev + size);
    }
  }
#endif
  // iter-388 fork-(b) probe: this memset zero-fills every grant; if the bump
  // window ever hands out VAs inside the DOGMA pool arena (0x84xxxxxx — the
  // 3rd allocator the resv registry doesn't cover), the zeroing IS the
  // shallow-death scrub (Havok header all-zeros at parse, RAG #189). Log any
  // grant overlapping the arena, loudly.
  if (va < 0x86000000u && va + (uint32_t)size > 0x84000000u) {
    static std::atomic<unsigned> s_px{0};
    unsigned px = s_px.fetch_add(1, std::memory_order_relaxed) + 1;
    if (px <= 64 || (px & (px - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[HEAP_POOLX] #%u guestHeapAlloc(0x%zX) -> 0x%08X..0x%08X INSIDE pool arena "
        "(zero-fill scrub candidate) guestLR=%08X",
        px, size, va, va + (uint32_t)size, getCurrentGuestLR());
  }
  // Platform contract (iter-463 charter, burndown item 7): a fresh grant from
  // the kernel pool reads as ZERO - Xenia Memory::SystemHeapAlloc zeroes every
  // allocation (memory.cc:715-724), as do XamAllocImpl (xam_info.cc:536-552)
  // and a first NtAllocateVirtualMemory commit (xboxkrnl_memory.cc:186-199).
  // The bump never reuses a VA, so on a correct grant this fill is a no-op.
  // (The old rationale here - "bound runaway format/memcpy scans" - was a
  // band-aid reading of the same line; the contract is the reason it stays.)
  uint8_t* host = guestVAToHost(va);
  if (host) std::memset(host, 0, size);
  return va;
}

uint32_t guestHeapAllocFallback(size_t size) {
  if (!g_guestBase || size == 0 || g_fallbackHeapVA == 0) return 0;
  size = (size + 15) & ~15ull;
  if (static_cast<size_t>(g_fallbackHeapVA) + size > g_guestSize) return 0;
  // iter-378 PLATFORM BUG FIX: this bump was a non-atomic load-then-store — two
  // concurrent callers could read the same `prev` and be handed the SAME VA, the
  // exact double-grant class iter-364 spent a session killing on the primary
  // heap (which already uses a CAS loop). Verified by the iter-376 triage.
  size_t prev = g_fallbackHeapNext.load(std::memory_order_relaxed);
  do {
    if (prev + size > kFallbackHeapSize) return 0;
  } while (!g_fallbackHeapNext.compare_exchange_weak(
      prev, prev + size, std::memory_order_relaxed, std::memory_order_relaxed));
  uint32_t va = static_cast<uint32_t>(g_fallbackHeapVA + prev);
  uint8_t* host = guestVAToHost(va);
  if (host) std::memset(host, 0, size);
  return va;
}

void guestPhysRegion(uint32_t& lo, uint32_t& hi) {
  if (physalloc::physFreeEnabled()) {   // iter-458 H-FREE: the allocator's own extent
    lo = physalloc::stats().lowWater;
    hi = physalloc::kHi;
    return;
  }
  const size_t used = g_physNext.load(std::memory_order_relaxed);
  hi = kPhysTop;
  lo = used >= kPhysTop ? 0u : kPhysTop - static_cast<uint32_t>(used);
}

void guestHeapRegion(uint32_t& lo, uint32_t& hi) {
  lo = static_cast<uint32_t>(g_heapStart);
  hi = static_cast<uint32_t>(g_heapStart + g_heapNext.load(std::memory_order_relaxed));
}

// iter-458 H-LIFO: defined below guestPhysAlloc (Astra 058).
std::mutex& physLifoMutex();
void physLifoRecord(uint32_t va, size_t prev, size_t reserve, uint32_t size);
// iter-465 physmem: [PHYS_PINS] / [PHYS_TRACE] (phys_lifo.hpp), defined below.
void physLifoLogPinsLocked(size_t size, uint32_t align, const char* why);
void physTraceGrant(uint32_t va, size_t size, uint32_t align, bool reuse);

// iter-462 PHYSICAL ALIAS HOLES (SR_PHYS_ALIAS_HOLES, default ON; registered
// by the sub_82467F40 override under SR_GPU_PHYS_ROUTE). On the console one
// physical page backs exactly one allocation, whichever window names it. Our
// 0x8 heap and this allocator are separate host memory sharing ONE physical
// address space, so a grant here could land on the physical pages of a
// GPU-visible heap grant (the ring, served from the heap before the ring
// claim) and that GPU address would name two objects: the renderer read the
// ring's bytes as the title dialog's art (iter-462, archive/v1_hr1a). A hole
// is the heap grant's 0xA alias [0xA0000000 | (va & 0x1FFFFFFF), +size); the
// LIFO bump steps below it exactly like the main-stack skip.
namespace {
constexpr int kPhysHoleMax = 32;
std::atomic<uint32_t> g_physHoleLo[kPhysHoleMax];
std::atomic<uint32_t> g_physHoleHi[kPhysHoleMax];
std::atomic<int> g_physHoleN{0};
std::atomic<uint64_t> g_physHoleDrops{0};   // iter-463: holes lost to a full table
bool physAliasHolesEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PHYS_ALIAS_HOLES");
    return !e || e[0] != '0';
  }();
  return v;
}
}  // namespace

void guestPhysReserveAlias(uint32_t va, uint32_t size) {
  if (!physAliasHolesEnabled() || size == 0) return;
  const uint64_t lo = 0xA0000000ull | (va & 0x1FFFFFFFu);
  const uint64_t hi = std::min<uint64_t>(lo + size, kPhysTop);
  if (lo >= hi) return;
  std::lock_guard<std::mutex> lk(physLifoMutex());
  const int i = g_physHoleN.load(std::memory_order_relaxed);
  if (i >= kPhysHoleMax) {
    // iter-463 F1 (c): a dropped hole is a physical range the LIFO bump may
    // double-book - never silent. Error at the first drop (the table-full
    // transition), then at powers of two.
    const uint64_t d = g_physHoleDrops.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((d & (d - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[PHYS_HOLE] TABLE FULL (%d holes): heap grant va=%08X size=0x%X is NOT "
                  "reserved - the physical allocator may double-book [%08X,%08X) (drop #%llu)",
                  kPhysHoleMax, va, size, static_cast<uint32_t>(lo), static_cast<uint32_t>(hi),
                  static_cast<unsigned long long>(d));
    return;
  }
  g_physHoleLo[i].store(static_cast<uint32_t>(lo), std::memory_order_relaxed);
  g_physHoleHi[i].store(static_cast<uint32_t>(hi), std::memory_order_relaxed);
  g_physHoleN.store(i + 1, std::memory_order_release);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PHYS_HOLE] #%d heap grant va=%08X size=0x%X -> physical allocator skips [%08X,%08X)",
              i + 1, va, size, static_cast<uint32_t>(lo), static_cast<uint32_t>(hi));
}

// Called with physLifoMutex() held. Offsets count down from kPhysTop: the grant
// needs `need` bytes starting at offset `start`; when [start, start+need)
// overlaps a hole, the grant starts below it (start = the hole's far edge).
// Returns true when it moved `start`. The caller keeps the skipped span inside
// the recorded reservation, so a reclaim still rolls back exactly.
static bool physSkipHoleLocked(size_t& start, size_t need) {
  const int n = g_physHoleN.load(std::memory_order_acquire);
  for (int i = 0; i < n; i++) {
    const size_t oLo = static_cast<size_t>(kPhysTop) - g_physHoleHi[i].load(std::memory_order_relaxed);
    const size_t oHi = static_cast<size_t>(kPhysTop) - g_physHoleLo[i].load(std::memory_order_relaxed);
    if (start < oHi && start + need > oLo) {
      static std::atomic<uint32_t> s_skips{0};
      const uint32_t k = s_skips.fetch_add(1, std::memory_order_relaxed) + 1;
      if (k <= 8 || (k & (k - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[PHYS_HOLE] skip #%u: grant 0x%zX at offset 0x%zX steps below hole %d (offset 0x%zX)",
                    k, need, start, i + 1, oHi);
      start = oHi;
      return true;
    }
  }
  return false;
}

// ── iter-463 F1: LIFO reuse + floor (contract in phys_lifo.hpp) ──────────────
// Offsets count down from kPhysTop (VA = kPhysTop - offset), like g_physNext.
// A free extent [lo, hi) is reservation space a freed grant gave back; a grant
// carved from one records its EXACT span as its reservation, so the is-top test
// and the rollback of every later free stay exact.
namespace {
constexpr uint32_t kPhysFloorVA = 0xA0000000u;
constexpr size_t kBigGrant = 0x1000000u;   // iter-465 SR_PHYS_BIG_GAP: 16 MB and up
constexpr size_t kBigGap = 0x2000000u;     //   leave 32 MB free above it
std::map<size_t, size_t> g_lifoFree;      // lo -> hi, under physLifoMutex(); coalesced
std::atomic<size_t> g_lifoFreeBytes{0};   // sum of the extents (exact, unsampled)
std::atomic<uint32_t> g_lifoFreeCount{0};
std::atomic<uint64_t> g_lifoReuses{0}, g_lifoCascades{0}, g_physFloorRefusals{0};
size_t g_lifoOrigin = 0;                  // the cursor the records start from (0 in the game)
std::atomic<int> g_reuseForce{-1}, g_floorForce{-1}, g_xeniaPlaceForce{-1};

// The extent map and its two counters change together, so the counters stay
// exact without re-walking the map under the lock (physLifoCheckForTest
// recounts from scratch).
void lifoFreePutLocked(size_t lo, size_t hi) {
  g_lifoFree[lo] = hi;
  g_lifoFreeBytes.fetch_add(hi - lo, std::memory_order_relaxed);
  g_lifoFreeCount.fetch_add(1, std::memory_order_relaxed);
}
std::map<size_t, size_t>::iterator lifoFreeDropLocked(std::map<size_t, size_t>::iterator it) {
  g_lifoFreeBytes.fetch_sub(it->second - it->first, std::memory_order_relaxed);
  g_lifoFreeCount.fetch_sub(1, std::memory_order_relaxed);
  return g_lifoFree.erase(it);
}
void lifoFreeClearLocked() {
  g_lifoFree.clear();
  g_lifoFreeBytes.store(0, std::memory_order_relaxed);
  g_lifoFreeCount.store(0, std::memory_order_relaxed);
}

// Adds [lo, hi) to the free extents, merged with the extents it touches. Both
// neighbours are checked for overlap BEFORE anything is erased, so the error
// path leaves the map (and its counters) exactly as it was.
void lifoFreeInsertLocked(size_t lo, size_t hi) {
  if (hi <= lo) return;
  auto nx = g_lifoFree.lower_bound(lo);
  auto pv = nx == g_lifoFree.begin() ? g_lifoFree.end() : std::prev(nx);
  const bool pvOverlap = pv != g_lifoFree.end() && pv->second > lo;
  const bool nxOverlap = nx != g_lifoFree.end() && nx->first < hi;
  if (pvOverlap || nxOverlap) {   // the bookkeeping is broken - never double-list pages
    const auto& o = pvOverlap ? *pv : *nx;
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[PHYS_LIFO] free extent [0x%zX,0x%zX) overlaps [0x%zX,0x%zX) - not listed",
                lo, hi, o.first, o.second);
    return;
  }
  if (pv != g_lifoFree.end() && pv->second == lo) { lo = pv->first; lifoFreeDropLocked(pv); }
  if (nx != g_lifoFree.end() && nx->first == hi) { hi = nx->second; lifoFreeDropLocked(nx); }
  lifoFreePutLocked(lo, hi);
}

// After a top free: the cursor keeps rolling back through the extents that end
// exactly at it (the freed grants below the one just reclaimed).
void lifoFreeCascadeLocked() {
  for (;;) {
    const size_t next = g_physNext.load(std::memory_order_relaxed);
    auto it = g_lifoFree.lower_bound(next);
    if (it == g_lifoFree.begin()) break;
    --it;
    if (it->second != next) break;
    g_physNext.store(it->first, std::memory_order_relaxed);
    lifoFreeDropLocked(it);
    g_lifoCascades.fetch_add(1, std::memory_order_relaxed);
  }
}

// Does VA [va, end) overlap the main stack or an alias hole? lowEdge = the
// lowest VA among the blockers it overlaps (the grant must end at or below it).
bool physBlockedLocked(uint64_t va, uint64_t end, uint64_t& lowEdge) {
  bool hit = false;
  lowEdge = ~0ull;
  auto take = [&](uint64_t lo, uint64_t hi) {
    if (va < hi && end > lo) { hit = true; if (lo < lowEdge) lowEdge = lo; }
  };
  take(kMainStackBase, static_cast<uint64_t>(kMainStackBase) + kMainStackSize);
  const int n = g_physHoleN.load(std::memory_order_acquire);
  for (int i = 0; i < n; i++)
    take(g_physHoleLo[i].load(std::memory_order_relaxed), g_physHoleHi[i].load(std::memory_order_relaxed));
  return hit;
}

// First-fit top-down over the free extents (highest VA first, like the bump),
// each placed as high as alignment, the main stack, the alias holes and the
// floor allow. Returns the VA (0 = no extent fits); the grant's exact span
// leaves the extent (split into what remains on either side).
uint32_t lifoReuseLocked(size_t size, uint32_t align, size_t& rPrev) {
  const bool floor = physFloorEnabled();
  for (auto it = g_lifoFree.begin(); it != g_lifoFree.end(); ++it) {
    const uint64_t vlo = static_cast<uint64_t>(kPhysTop) - it->second;
    uint64_t top = static_cast<uint64_t>(kPhysTop) - it->first;
    uint64_t va = 0;
    bool found = false;
    while (top >= vlo + size) {
      va = (top - size) & ~static_cast<uint64_t>(align - 1u);
      if (va < vlo || (floor && va < kPhysFloorVA)) break;
      uint64_t edge = 0;
      if (!physBlockedLocked(va, va + size, edge)) { found = true; break; }
      if (edge >= top) break;   // cannot happen (the blocker overlaps [va, va+size) below top)
      top = edge;
    }
    if (!found) continue;
    const size_t lo = it->first, hi = it->second;
    const size_t rLo = static_cast<size_t>(kPhysTop) - static_cast<size_t>(va + size);
    const size_t rHi = static_cast<size_t>(kPhysTop) - static_cast<size_t>(va);
    lifoFreeDropLocked(it);
    if (lo < rLo) lifoFreePutLocked(lo, rLo);
    if (rHi < hi) lifoFreePutLocked(rHi, hi);
    rPrev = rLo;
    return static_cast<uint32_t>(va);
  }
  return 0;
}

// E3 (DIAGNOSTIC ONLY): see phys_lifo.hpp. Returns the dummy's VA or 0.
uint32_t physPreadvanceLocked(size_t target, const char* why) {
  const size_t prev = g_physNext.load(std::memory_order_relaxed);
  if (target <= prev || target > g_physSize || target > kPhysTop) return 0;
  const uint32_t va = static_cast<uint32_t>(static_cast<size_t>(kPhysTop) - target);
  g_physNext.store(target, std::memory_order_relaxed);
  physLifoRecord(va, prev, target - prev, static_cast<uint32_t>(target - prev));
  SR_LOG_BOOT(sr::LogSeverity::Error,
              "[PHYS_PREADVANCE] DIAGNOSTIC (%s, never a default): cursor pre-consumed 0x%zX -> "
              "0x%zX as ONE dummy reservation nobody frees (va=%08X, low-water now %08X)",
              why, prev, target, va, va);
  return va;
}
// The trigger (review: at the FIRST grant the dummy filled the top of the
// region and the D3D ring / command buffer / fence block were placed BELOW it,
// out of reach of any later alias - a false negative). It is armed only once
// the fence block exists (gpuCpSetRptrWriteback -> physLifoPreadvanceArm, right
// after PHYS_ALLOC #5/#6 in every archived boot), and then applies at the first
// grant of exactly SR_PHYS_LIFO_PREADVANCE_AT bytes (hex; unset = the first
// grant after arming). E3 names the arena's own grant (2B30000), so the boot's
// 224 MB world pool keeps its normal place.
struct Preadvance {
  bool done = false;
  bool loaded = false;
  size_t target = 0, atSize = 0;
};
Preadvance g_preadv;                       // under physLifoMutex()
std::atomic<bool> g_preadvArmed{false};
void physPreadvanceMaybeLocked(size_t size) {
  Preadvance& p = g_preadv;
  if (p.done) return;
  if (!p.loaded) {
    p.loaded = true;
    const char* e = std::getenv("SR_PHYS_LIFO_PREADVANCE");
    const char* at = std::getenv("SR_PHYS_LIFO_PREADVANCE_AT");
    p.target = e && *e ? static_cast<size_t>(std::strtoull(e, nullptr, 16)) : 0;
    p.atSize = at && *at ? static_cast<size_t>(std::strtoull(at, nullptr, 16)) : 0;
  }
  if (!p.target) { p.done = true; return; }
  if (!g_preadvArmed.load(std::memory_order_acquire)) return;
  if (p.atSize && size != p.atSize) return;
  p.done = true;
  if (!physPreadvanceLocked(p.target, "SR_PHYS_LIFO_PREADVANCE"))
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[PHYS_PREADVANCE] SR_PHYS_LIFO_PREADVANCE=0x%zX ignored (cursor 0x%zX, region 0x%zX)",
                p.target, g_physNext.load(std::memory_order_relaxed), g_physSize);
}
}  // namespace

void physLifoPreadvanceArm() { g_preadvArmed.store(true, std::memory_order_release); }
void setPhysPreadvanceForTest(size_t target, size_t atSize) {
  std::lock_guard<std::mutex> lk(physLifoMutex());
  g_preadv = Preadvance{};
  g_preadv.loaded = true;
  g_preadv.target = target;
  g_preadv.atSize = atSize;
  g_preadvArmed.store(false, std::memory_order_release);
}

std::atomic<int> g_bigGapForce{-1};
void setPhysBigGapForTest(int mode) { g_bigGapForce.store(mode, std::memory_order_relaxed); }
bool physBigGapEnabled() {
  const int f = g_bigGapForce.load(std::memory_order_relaxed);
  if (f >= 0) return f == 1;
  static const bool v = [] {
    const char* e = std::getenv("SR_PHYS_BIG_GAP");
    return !e || e[0] != '0';   // default ON since the iter-465 rolls; =0 keeps the A/B
  }();
  return v;
}

bool physXeniaPlaceEnabled() {
  const int f = g_xeniaPlaceForce.load(std::memory_order_relaxed);
  if (f >= 0) return f == 1;
  static const bool v = [] {
    const char* e = std::getenv("SR_PHYS_XENIA_PLACE");
    return !e || e[0] != '0';   // default ON since the iter-465 rolls; =0 keeps the A/B
  }();
  return v;
}
void setPhysXeniaPlaceForTest(int mode) { g_xeniaPlaceForce.store(mode, std::memory_order_relaxed); }

bool physLifoReuseEnabled() {
  const int f = g_reuseForce.load(std::memory_order_relaxed);
  if (f >= 0) return f == 1;
  static const bool v = [] {
    const char* e = std::getenv("SR_PHYS_LIFO_REUSE");
    return !e || e[0] != '0';
  }();
  return v;
}
bool physFloorEnabled() {
  const int f = g_floorForce.load(std::memory_order_relaxed);
  if (f >= 0) return f == 1;
  static const bool v = [] {
    const char* e = std::getenv("SR_PHYS_FLOOR");
    return !e || e[0] != '0';
  }();
  return v;
}
void setPhysLifoReuseForTest(int mode) { g_reuseForce.store(mode, std::memory_order_relaxed); }
void setPhysFloorForTest(int mode) { g_floorForce.store(mode, std::memory_order_relaxed); }

uint32_t physLifoPreadvanceNowForTest(size_t offset) {
  std::lock_guard<std::mutex> lk(physLifoMutex());
  return physPreadvanceLocked(offset, "test");
}

void resetPhysHolesForTest() {
  std::lock_guard<std::mutex> lk(physLifoMutex());
  g_physHoleN.store(0, std::memory_order_release);
  g_physHoleDrops.store(0, std::memory_order_relaxed);
}

uint32_t guestPhysAlloc(size_t size, uint32_t align) {
  if (!g_guestBase || g_physSize == 0) return 0;
  // iter-458 H-FREE (SR_PHYS_FREE=1): the reusable physical allocator
  // (page-granular, top-down, [0xA0000000, 0xBF000000)). Contract: phys_alloc.hpp.
  if (physalloc::physFreeEnabled())
    return size > 0xFFFFFFFFull ? 0u : physalloc::alloc(static_cast<uint32_t>(size), align);
  // iter-449B PLATFORM FIX: honor caller alignment. Console
  // MmAllocatePhysicalMemoryEx returns page-aligned (or r8-aligned) memory;
  // the D3DX effect compiler's pool depends on it — it finds each pool page's
  // header via *(obj & 0xFFFFF000) (4KB page-mask trick, sub_827D33B0).
  // Before this fix, small GPU VB allocs interleaved 16B-aligned sizes into
  // this same downward bump, so later Mm "pages" were unaligned → the
  // compiler read a garbage pool header and returned arena OFFSETS
  // (0x0/0x38/0x70...) → the new-backend mark pass wrote into the null page
  // and silently died → X4800 error cascade → infinite errno loop → every
  // runtime effect E_FAIL → debug-shader substitution → the black FE
  // (iter-449z/449B chain).
  if (align < 16u || (align & (align - 1u)) != 0u) align = 16u;
  size = (size + 15) & ~15ull;
  // Grow DOWN from kPhysTop. Over-reserve by `align` so an aligned VA fits
  // inside the reservation regardless of the bump's current phase.
  size_t reserve = size + align;
  // iter-458 H-LIFO (Astra 058): with the gate ON the reservation and its
  // record happen under ONE lock, so "the most recent grant" is exact even
  // with concurrent callers. The arithmetic is identical to the gate-OFF path.
  if (physLifoEnabled()) {
    std::lock_guard<std::mutex> lk(physLifoMutex());
    physPreadvanceMaybeLocked(size);   // iter-463 E3 diagnostic; inert unless the env names it
    // iter-463 F1 (b): space a freed grant gave back is reused before the
    // cursor moves (Xenia BaseHeap::AllocRange top-down over free pages).
    if (physLifoReuseEnabled()) {
      size_t rPrev = 0;
      if (const uint32_t rv = lifoReuseLocked(size, align, rPrev)) {
        physLifoRecord(rv, rPrev, size, static_cast<uint32_t>(size));
        physTraceGrant(rv, size, align, true);
        // The pages' old owners were released; the caller's notePhysGrant must
        // not find a stale record over them (Astra 059: under this lock).
        physwin::forgetReleasedPhysGrantsIn(rv, static_cast<uint32_t>(size));
        const uint64_t k = g_lifoReuses.fetch_add(1, std::memory_order_relaxed) + 1;
        if (k <= 32 || (k & (k - 1)) == 0 || size >= 0x1000000u)
          SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[PHYS_LIFO] reuse #%llu va=%08X size=0x%zX align=0x%X | retained now 0x%zX in %u "
                      "extents, physNext 0x%zX",
                      static_cast<unsigned long long>(k), rv, size, align,
                      g_lifoFreeBytes.load(std::memory_order_relaxed),
                      g_lifoFreeCount.load(std::memory_order_relaxed),
                      g_physNext.load(std::memory_order_relaxed));
        return rv;
      }
    }
    const size_t prev = g_physNext.load(std::memory_order_relaxed);
    // iter-458 H-STACK for H-LIFO (hlifo1, Astra 065): the harness main stack
    // [kMainStackBase, +kMainStackSize) lies inside this bump region. The
    // gate-OFF bump only ever overlapped it with the 0x12B00000 probe, which the
    // guest frees untouched; a reclaim hands that span out again, and hlifo1's
    // world pool landed on the live stack (FE stalled). A reservation that would
    // straddle the stack starts BELOW it; the skipped gap is part of the recorded
    // reservation, so a reclaim still rolls back exactly.
    const size_t stackHiOff = static_cast<size_t>(kPhysTop) - (kMainStackBase + kMainStackSize);
    const size_t stackLoOff = static_cast<size_t>(kPhysTop) - kMainStackBase;
    // iter-462: the grant's start moves below the main stack and every alias
    // hole it would overlap (a hole skip can land on the stack and vice versa;
    // `start` only grows, so this ends).
    const size_t need = reserve;
    size_t start = prev;
    // iter-465 physmem (SR_PHYS_BIG_GAP, needs SR_PHYS_XENIA_PLACE): a grant of 16 MB or
    // more placed at the cursor leaves 32 MB FREE above it (a free extent the next
    // smaller grants fill top-down). Xenia's own layout has such a hole above the world
    // pool, but only because its slower boot keeps the PHYSICS pool (0x690000) alive
    // while the intro movie allocates its frame buffers (archive/v1_physmem/xenia_234914);
    // native speed frees it first, the hole never forms, the persistent boot grants
    // land directly below the pool and the 224 MB pool no longer fits on the
    // Warworld -> Metropolis return. The gap makes that layout deterministic.
    if (physXeniaPlaceEnabled() && physBigGapEnabled() && size >= kBigGrant &&
        prev + kBigGap + reserve + 0x1000000u <= static_cast<size_t>(kPhysTop) - kPhysFloorVA)
      start = prev + kBigGap;
    for (bool moved = true; moved;) {
      moved = false;
      if (start < stackLoOff && start + need > stackHiOff) {
        start = stackLoOff;
        moved = true;
      }
      if (physSkipHoleLocked(start, need)) moved = true;
    }
    reserve = (start - prev) + need;
    if (prev + reserve > g_physSize) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[PHYS_CAP] guestPhysAlloc(0x%zX) exceeds the region (LIFO): used=0x%zX - refusing",
                  size, prev);
      physLifoLogPinsLocked(size, align, "region");
      return 0;
    }
    const size_t heapHigh = g_heapStart + g_heapNext.load(std::memory_order_relaxed);
    if (static_cast<size_t>(kPhysTop) < prev + reserve ||
        static_cast<size_t>(kPhysTop) - (prev + reserve) < heapHigh) {
      static std::atomic<int> warned{0};
      if (warned.fetch_add(1, std::memory_order_relaxed) < 4)
        fprintf(stderr, "[PHYS_CAP] guestPhysAlloc(%zu) would cross heap high-water (LIFO) - refusing\n",
                size);
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[PHYS_CAP] guestPhysAlloc(0x%zX) would cross heap high-water (LIFO): "
                  "physLowNew=0x%zX heapHigh=0x%zX - refusing",
                  size, static_cast<size_t>(kPhysTop) - (prev + reserve), heapHigh);
      return 0;
    }
    size_t low = static_cast<size_t>(kPhysTop) - (prev + reserve);
    // iter-463 F1 (a): physical memory ends at 0xA0000000. Below it a grant's
    // physical alias (va & 0x1FFFFFFF) is the top of the 0xA window - live GPU
    // objects (the v1_gpuhang freeze). The GRANT (its aligned VA) must lie at or
    // above the floor; only the alignment padding may not, and then the
    // reservation is clipped at the floor (the aligned VA is the floor itself -
    // it is a multiple of any alignment we honour). Otherwise the request fails:
    // MmAllocatePhysicalMemoryEx returns 0 to the guest as a console out of
    // physical memory does. (The sub_82467F40 override's routePhys arm may
    // still fall back to the 0x8 heap on a 0 - gpu_ring_alloc_fix.cpp - which
    // reaches this path after the ring claim only with SR_GPU_PHYS_ROUTE=0 or
    // SR_GPU_LARGE_PHYS=1; there [PHYS_CAP] is followed by its heap-route log.)
    if (physFloorEnabled() && low < kPhysFloorVA) {
      const size_t aligned = (low + align - 1) & ~static_cast<size_t>(align - 1);
      if (aligned < kPhysFloorVA || aligned + size > static_cast<size_t>(kPhysTop) - prev) {
        const uint64_t k = g_physFloorRefusals.fetch_add(1, std::memory_order_relaxed) + 1;
        if (k <= 16 || (k & (k - 1)) == 0)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[PHYS_CAP] guestPhysAlloc(0x%zX, align 0x%X) would reach below 0xA0000000 "
                      "(low=0x%zX, physNext 0x%zX, retained 0x%zX) - refusing (#%llu)",
                      size, align, low, prev, g_lifoFreeBytes.load(std::memory_order_relaxed),
                      static_cast<unsigned long long>(k));
        if (k <= 2 || physTraceEnabled()) physLifoLogPinsLocked(size, align, "floor");
        return 0;
      }
      reserve = static_cast<size_t>(kPhysTop) - kPhysFloorVA - prev;   // padding clipped at the floor
      low = kPhysFloorVA;
    }
    if (physXeniaPlaceEnabled() && physLifoReuseEnabled()) {
      // iter-465 physmem (SR_PHYS_XENIA_PLACE): Xenia BaseHeap::AllocRange top-down -
      // the grant takes the HIGHEST aligned position below the cursor and the pages
      // between it and the cursor (alignment gap, skipped stack/hole span) stay FREE
      // for later grants, instead of riding along as never-usable padding.
      const size_t topVA = static_cast<size_t>(kPhysTop) - start;
      const size_t xva = (topVA - size) & ~static_cast<size_t>(align - 1);
      const size_t rLo = static_cast<size_t>(kPhysTop) - (xva + size);
      const size_t rHi = static_cast<size_t>(kPhysTop) - xva;
      g_physNext.store(rHi, std::memory_order_relaxed);
      if (rLo > prev) lifoFreeInsertLocked(prev, rLo);
      physLifoRecord(static_cast<uint32_t>(xva), rLo, size, static_cast<uint32_t>(size));
      physTraceGrant(static_cast<uint32_t>(xva), size, align, false);
      physwin::forgetReleasedPhysGrantsIn(static_cast<uint32_t>(xva), static_cast<uint32_t>(size));
      return static_cast<uint32_t>(xva);
    }
    g_physNext.store(prev + reserve, std::memory_order_relaxed);
    const size_t va = (low + align - 1) & ~static_cast<size_t>(align - 1);
    physLifoRecord(static_cast<uint32_t>(va), prev, reserve, static_cast<uint32_t>(size));
    physTraceGrant(static_cast<uint32_t>(va), size, align, false);
    // iter-463: after a cascade the bump re-covers pages whose released
    // records are still registered - drop them before the caller registers.
    if (physLifoReuseEnabled())
      physwin::forgetReleasedPhysGrantsIn(static_cast<uint32_t>(va), static_cast<uint32_t>(size));
    return static_cast<uint32_t>(va);
  }
  size_t prev = g_physNext.fetch_add(reserve, std::memory_order_relaxed);
  if (prev + reserve > g_physSize) {
    // Roll back the reservation so subsequent callers see consistent state.
    g_physNext.fetch_sub(reserve, std::memory_order_relaxed);
    return 0;
  }
  // Check against heap high-water: new phys low-water = kPhysTop - (prev + reserve).
  size_t heapHigh = g_heapStart + g_heapNext.load(std::memory_order_relaxed);
  if (static_cast<size_t>(kPhysTop) < prev + reserve ||
      static_cast<size_t>(kPhysTop) - (prev + reserve) < heapHigh) {
    g_physNext.fetch_sub(reserve, std::memory_order_relaxed);
    static std::atomic<int> warned{0};
    if (warned.fetch_add(1, std::memory_order_relaxed) < 4) {
      fprintf(stderr, "[PHYS_CAP] guestPhysAlloc(%zu) would cross heap high-water: physLowNew=0x%zX heapHigh=0x%zX — refusing\n",
              size, static_cast<size_t>(kPhysTop) - (prev + reserve), heapHigh);
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[PHYS_CAP] guestPhysAlloc(0x%zX) would cross heap high-water: physLowNew=0x%zX "
                "heapHigh=0x%zX - refusing",
                size, static_cast<size_t>(kPhysTop) - (prev + reserve), heapHigh);
    return 0;
  }
  size_t low = static_cast<size_t>(kPhysTop) - (prev + reserve);
  // Round UP into the padded window; [va, va+size) stays inside the reservation.
  size_t va = (low + align - 1) & ~static_cast<size_t>(align - 1);
  return static_cast<uint32_t>(va);
}

// ── iter-458 H-LIFO (Astra 058) ──────────────────────────────────────────────
namespace {
struct LifoGrant {
  size_t prev;      // g_physNext BEFORE this reservation
  size_t reserve;   // size + alignment padding actually reserved
  uint32_t size;
  uint8_t state;    // 0 live, 1 released-but-retained (non-top free), 2 reclaimed
  uint32_t lr;      // iter-463: the guest caller (the GPU_HANG_WD (f) grant list)
  uint8_t kind;     // iter-465 physmem: PhysKind of the caller
};
std::map<uint32_t, LifoGrant> g_lifoGrants;   // va -> grant, under physLifoMutex()
thread_local PhysKind t_physKind = PhysKind::kGuest;   // iter-465 physmem (PhysKindScope)
std::atomic<int> g_lifoForce{-1};
}  // namespace

std::mutex& physLifoMutex() {
  static std::mutex m;
  return m;
}

bool physLifoEnabled() {
  const int f = g_lifoForce.load(std::memory_order_relaxed);
  if (f >= 0) return f == 1 && !physalloc::physFreeEnabled();
  // iter-460: default ON. Without the reclaim the 224 MB world pool is placed
  // after the never-freed 299 MB probe and straddles the end of physical
  // memory (phys 0x1D76xxxx + 224 MB wraps to 0x0000xxxx-0x0B75xxxx), where it
  // double-books the virtual heap's physical addresses: 73 index buffers per
  // title frame read the heap's bytes through the pool's GPU address
  // ([IB_RANGE]) and drew screen-sized slabs. With the probe reclaimed the pool
  // lands at 0xB0008000, no wrap, 0 hits (archive/p2_lifo5). =0 keeps the A/B.
  static const bool s_on = [] {
    const char* e = std::getenv("SR_PHYS_LIFO_FREE");
    return !e || e[0] != '0';
  }();
  return s_on && !physalloc::physFreeEnabled();
}

void setPhysLifoForTest(int mode) {
  std::lock_guard<std::mutex> lk(physLifoMutex());
  g_lifoForce.store(mode, std::memory_order_relaxed);
  g_lifoGrants.clear();
  // iter-463: the free extents belong to the records just dropped; the
  // bookkeeping restarts from the current cursor.
  lifoFreeClearLocked();
  g_lifoOrigin = g_physNext.load(std::memory_order_relaxed);
}

PhysLifoStats physLifoStats() {
  std::lock_guard<std::mutex> lk(physLifoMutex());
  PhysLifoStats s;
  s.physNext = g_physNext.load(std::memory_order_relaxed);
  s.retainedBytes = g_lifoFreeBytes.load(std::memory_order_relaxed);
  s.retainedExtents = g_lifoFreeCount.load(std::memory_order_relaxed);
  s.reuses = g_lifoReuses.load(std::memory_order_relaxed);
  s.cascades = g_lifoCascades.load(std::memory_order_relaxed);
  s.floorRefusals = g_physFloorRefusals.load(std::memory_order_relaxed);
  s.holes = static_cast<uint32_t>(g_physHoleN.load(std::memory_order_relaxed));
  s.holeDrops = g_physHoleDrops.load(std::memory_order_relaxed);
  for (const auto& g : g_lifoGrants)
    if (g.second.state == 0 && g.second.size) { s.lowestLiveVA = g.first; break; }   // map is VA-ordered
  return s;
}

size_t physLifoRetainedBytes() { return g_lifoFreeBytes.load(std::memory_order_relaxed); }
uint32_t physLifoRetainedExtents() { return g_lifoFreeCount.load(std::memory_order_relaxed); }

// iter-463 GPU_HANG_WD (f): the allocator's view for the [GPU_HANG] block and
// the dump. Contract (gpu_hang_wd.hpp): never blocks - the allocator lock is
// only TRIED - and reads host memory only.
bool physLifoHangAllocInfo(GpuHangAllocInfo* out) {
  std::unique_lock<std::mutex> lk(physLifoMutex(), std::try_to_lock);
  if (!lk.owns_lock()) return false;
  GpuHangAllocInfo a;
  a.physNext = g_physNext.load(std::memory_order_relaxed);
  for (const auto& g : g_lifoGrants) {   // VA-ordered: the first live one is the lowest
    if (g.second.state != 0 || !g.second.size) continue;
    if (!a.lowestPhysGrantVA) a.lowestPhysGrantVA = g.first;
    if (g.first >= kPhysFloorVA) break;
    a.belowACount++;
    a.belowABytes += g.second.size;
    if (a.belowAListed < 8) a.belowA[a.belowAListed++] = GpuHangAllocInfo::Grant{g.first, g.second.size, g.second.lr};
  }
  a.lifoRetainedCount = g_lifoFreeCount.load(std::memory_order_relaxed);   // the free extents
  a.lifoRetainedBytes = g_lifoFreeBytes.load(std::memory_order_relaxed);
  a.aliasHoles = g_physHoleN.load(std::memory_order_relaxed);
  a.aliasHoleMax = kPhysHoleMax;
  *out = a;
  return true;
}

bool physLifoGrantForTest(uint32_t va, size_t* prev, size_t* reserve, int* state) {
  std::lock_guard<std::mutex> lk(physLifoMutex());
  auto it = g_lifoGrants.find(va);
  if (it == g_lifoGrants.end()) return false;
  if (prev) *prev = it->second.prev;
  if (reserve) *reserve = it->second.reserve;
  if (state) *state = it->second.state;
  return true;
}

bool physLifoCheckForTest(std::string* why) {
  std::lock_guard<std::mutex> lk(physLifoMutex());
  char buf[192];
  auto fail = [&](const char* m, size_t a, size_t b) {
    std::snprintf(buf, sizeof(buf), "%s (0x%zX, 0x%zX)", m, a, b);
    if (why) *why = buf;
    return false;
  };
  // Intervals: the live reservations (kind 0) and the free extents (kind 1).
  std::vector<std::pair<std::pair<size_t, size_t>, int>> iv;
  for (const auto& g : g_lifoGrants)
    if (g.second.state == 0) iv.push_back({{g.second.prev, g.second.prev + g.second.reserve}, 0});
  size_t bytes = 0;
  for (const auto& e : g_lifoFree) {
    if (e.second <= e.first) return fail("empty free extent", e.first, e.second);
    iv.push_back({{e.first, e.second}, 1});
    bytes += e.second - e.first;
  }
  if (bytes != g_lifoFreeBytes.load(std::memory_order_relaxed))
    return fail("retained-bytes counter != sum of extents", bytes, g_lifoFreeBytes.load());
  if (g_lifoFree.size() != g_lifoFreeCount.load(std::memory_order_relaxed))
    return fail("extent counter != extents listed", g_lifoFree.size(), g_lifoFreeCount.load());
  std::sort(iv.begin(), iv.end());
  const size_t next = g_physNext.load(std::memory_order_relaxed);
  size_t at = g_lifoOrigin;
  int lastKind = -1;
  for (const auto& x : iv) {
    if (x.first.first != at) return fail("partition gap/overlap at", at, x.first.first);
    if (x.second == 1 && lastKind == 1) return fail("two free extents touch (not coalesced)", x.first.first, 0);
    at = x.first.second;
    lastKind = x.second;
  }
  if (at != next) return fail("partition end != physNext", at, next);
  if (lastKind == 1) return fail("a free extent ends at physNext (no cascade)", at, next);
  return true;
}

size_t guestPhysNextForTest() { return g_physNext.load(std::memory_order_relaxed); }

// Called with physLifoMutex() held.
void physLifoRecord(uint32_t va, size_t prev, size_t reserve, uint32_t size) {
  g_lifoGrants[va] = LifoGrant{prev, reserve, size, 0, getCurrentGuestLR(),
                               static_cast<uint8_t>(t_physKind)};
}

// ── iter-465 physmem: grant kinds, [PHYS_TRACE], [PHYS_PINS] (phys_lifo.hpp) ──
const char* physKindName(PhysKind k) {
  switch (k) {
    case PhysKind::kGuest:       return "guest";
    case PhysKind::kThreadStack: return "thread-stack";
    case PhysKind::kKThread:     return "kthread";
    case PhysKind::kKpcr:        return "kpcr";
    case PhysKind::kGpuRoute:    return "gpu-route";
    case PhysKind::kHost:        return "host";
  }
  return "?";
}
PhysKindScope::PhysKindScope(PhysKind k) : prev_(t_physKind) { t_physKind = k; }
PhysKindScope::~PhysKindScope() { t_physKind = prev_; }

bool physTraceEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PHYS_TRACE");
    return e && e[0] == '1';
  }();
  return v;
}

void physTraceGrant(uint32_t va, size_t size, uint32_t align, bool reuse) {
  if (!physTraceEnabled()) return;
  // Grants of 1 MB and more also name their guest callers: code addresses found in
  // the first 0x400 bytes above r1 (the return addresses of the frames on the stack).
  char chain[160];
  chain[0] = 0;
  if (size >= 0x100000u) {
    const uint32_t r1 = getCurrentGuestR1();
    int cn = 0, words = 8;
    for (uint32_t o = 0; r1 && o < 0x400u && words > 0; o += 4) {
      const uint8_t* p = guestVAToHost(r1 + o);
      if (!p) break;
      const uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
      if (v < 0x82000000u || v >= 0x82990000u) continue;
      const int w = std::snprintf(chain + cn, sizeof(chain) - cn, " %08X", v);
      if (w <= 0 || cn + w >= static_cast<int>(sizeof(chain)) - 1) break;
      cn += w;
      --words;
    }
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PHYS_TRACE] grant va=%08X size=0x%zX align=0x%X kind=%s lr=%08X %s physNext=0x%zX "
              "retained=0x%zX%s%s",
              va, size, align, physKindName(t_physKind), getCurrentGuestLR(), reuse ? "reuse" : "bump",
              g_physNext.load(std::memory_order_relaxed),
              g_lifoFreeBytes.load(std::memory_order_relaxed), chain[0] ? " chain:" : "", chain);
}

void physLifoLogPinsLocked(size_t size, uint32_t align, const char* why) {
  struct Live { uint64_t lo, hi; uint32_t lr; uint8_t kind; };
  std::vector<Live> live;
  live.reserve(g_lifoGrants.size());
  uint64_t kindN[8] = {}, kindB[8] = {};
  for (const auto& g : g_lifoGrants) {
    if (g.second.state != 0 || !g.second.size) continue;
    live.push_back(Live{g.first, uint64_t(g.first) + g.second.size, g.second.lr, g.second.kind});
    const int k = g.second.kind < 8 ? g.second.kind : 7;
    kindN[k]++;
    kindB[k] += g.second.size;
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[PHYS_PINS] (%s) request 0x%zX align 0x%X | physNext 0x%zX retained 0x%zX in %u extents | "
              "live: guest %llu/0x%llX thread-stack %llu/0x%llX kthread %llu/0x%llX kpcr %llu/0x%llX "
              "gpu-route %llu/0x%llX host %llu/0x%llX",
              why, size, align, g_physNext.load(std::memory_order_relaxed),
              g_lifoFreeBytes.load(std::memory_order_relaxed),
              g_lifoFreeCount.load(std::memory_order_relaxed),
              (unsigned long long)kindN[0], (unsigned long long)kindB[0],
              (unsigned long long)kindN[1], (unsigned long long)kindB[1],
              (unsigned long long)kindN[2], (unsigned long long)kindB[2],
              (unsigned long long)kindN[3], (unsigned long long)kindB[3],
              (unsigned long long)kindN[4], (unsigned long long)kindB[4],
              (unsigned long long)kindN[5], (unsigned long long)kindB[5]);
  // The largest free extents (as VA ranges).
  std::vector<std::pair<size_t, size_t>> ext(g_lifoFree.begin(), g_lifoFree.end());
  std::sort(ext.begin(), ext.end(), [](const auto& a, const auto& b) {
    return (a.second - a.first) > (b.second - b.first);
  });
  for (size_t i = 0; i < ext.size() && i < 6; i++)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[PHYS_PINS] free extent #%zu [%08X,%08X) 0x%zX", i + 1,
                static_cast<uint32_t>(kPhysTop - ext[i].second),
                static_cast<uint32_t>(kPhysTop - ext[i].first), ext[i].second - ext[i].first);
  // The best window: [s, s+size) at or above the floor, s = the floor or a live grant's end
  // (aligned), with the fewest live bytes in it.
  const uint64_t floorVA = physFloorEnabled() ? kPhysFloorVA : (kPhysTop - g_physSize);
  std::vector<uint64_t> starts;
  starts.push_back(floorVA);
  for (const auto& l : live) starts.push_back(l.hi);
  std::sort(starts.begin(), starts.end());   // i0 below only moves forward
  uint64_t bestS = 0, bestBytes = ~0ull;
  size_t bestN = 0;
  size_t i0 = 0;   // first live grant ending after s
  for (uint64_t s0 : starts) {
    const uint64_t s = (s0 + align - 1) & ~uint64_t(align - 1);
    const uint64_t e = s + size;
    if (s < floorVA || e > kPhysTop) continue;
    while (i0 < live.size() && live[i0].hi <= s) i0++;
    uint64_t bytes = 0;
    size_t n = 0;
    for (size_t j = i0; j < live.size() && live[j].lo < e; j++) {
      bytes += std::min(live[j].hi, e) - std::max(live[j].lo, s);
      n++;
    }
    if (bytes < bestBytes) { bestBytes = bytes; bestS = s; bestN = n; }
    if (bytes == 0) break;
  }
  if (bestBytes == ~0ull) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[PHYS_PINS] no window of 0x%zX fits [%08llX,%08X)", size,
                (unsigned long long)floorVA, kPhysTop);
    return;
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[PHYS_PINS] best window [%08llX,%08llX): %zu live grant(s), 0x%llX bytes pin it",
              (unsigned long long)bestS, (unsigned long long)(bestS + size), bestN,
              (unsigned long long)bestBytes);
  size_t listed = 0;
  for (const auto& l : live) {
    if (l.hi <= bestS || l.lo >= bestS + size) continue;
    if (++listed > 48) break;
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[PHYS_PINS]   pin va=%08llX size=0x%llX kind=%s lr=%08X",
                (unsigned long long)l.lo, (unsigned long long)(l.hi - l.lo),
                physKindName(static_cast<PhysKind>(l.kind)), l.lr);
  }
}

void physLifoLogPins(size_t size, uint32_t align, const char* why) {
  std::lock_guard<std::mutex> lk(physLifoMutex());
  physLifoLogPinsLocked(size, align, why);
}

const char* physLifoFreeName(PhysLifoFree r) {
  switch (r) {
    case PhysLifoFree::kNotEnabled: return "not-enabled";
    case PhysLifoFree::kReclaimed:  return "RECLAIMED";
    case PhysLifoFree::kUnknown:    return "rejected-unknown";
    case PhysLifoFree::kDuplicate:  return "rejected-duplicate";
    case PhysLifoFree::kNotTop:     return "rejected-not-top (retained)";
  }
  return "?";
}

PhysLifoFree guestPhysLifoFree(uint32_t va) {
  if (!physLifoEnabled()) return PhysLifoFree::kNotEnabled;
  std::lock_guard<std::mutex> lk(physLifoMutex());
  auto it = g_lifoGrants.find(va);
  if (it == g_lifoGrants.end()) return PhysLifoFree::kUnknown;
  LifoGrant& g = it->second;
  if (physTraceEnabled())   // iter-465 physmem: the free half of the timeline
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PHYS_TRACE] free va=%08X size=0x%X kind=%s state=%u lr=%08X physNext=0x%zX",
                va, g.size, physKindName(static_cast<PhysKind>(g.kind)), g.state, getCurrentGuestLR(),
                g_physNext.load(std::memory_order_relaxed));
  if (g.state != 0) return PhysLifoFree::kDuplicate;
  // Astra 059: the ownership registry follows UNDER this lock - once the cursor
  // is rolled back another thread may re-grant (and register) the same VA, so
  // removing the old grant afterwards could erase the new owner.
  if (g_physNext.load(std::memory_order_relaxed) != g.prev + g.reserve) {
    g.state = 1;
    physwin::markPhysGrantReleased(va, /*report=*/false);
    // iter-463 F1 (b): the reservation is free space for the next grant.
    if (physLifoReuseEnabled()) lifoFreeInsertLocked(g.prev, g.prev + g.reserve);
    return PhysLifoFree::kNotTop;
  }
  g_physNext.store(g.prev, std::memory_order_relaxed);   // padding included
  g.state = 2;
  physwin::forgetPhysGrant(va, /*report=*/false);
  // iter-463 F1 (b): ...and through every freed grant directly below it.
  if (physLifoReuseEnabled()) lifoFreeCascadeLocked();
  return PhysLifoFree::kReclaimed;
}

namespace {
std::atomic<void (*)(uint32_t)> g_lifoPostFreeHook{nullptr};
}
void setPhysLifoPostFreeHookForTest(void (*hook)(uint32_t va)) { g_lifoPostFreeHook.store(hook); }
void physLifoPostFreeHook(uint32_t va) {
  if (auto h = g_lifoPostFreeHook.load(std::memory_order_relaxed)) h(va);
}

void dumpHeapState(const char* tag) {
  fprintf(stderr, "[HEAP_STATE] %s guestBase=%p heapStart=0x%zX heapSize=0x%zX heapNext=0x%zX fallbackNext=0x%zX\n",
    tag, (void*)g_guestBase, g_heapStart, g_heapSize,
    g_heapNext.load(std::memory_order_relaxed),
    g_fallbackHeapNext.load(std::memory_order_relaxed));
}

void setCurrentGuestLR(uint32_t lr) {
  g_currentGuestLR = lr;
}

uint32_t getCurrentGuestLR() {
  return g_currentGuestLR;
}

namespace {
  // iter-23 r1-corruption hunt: per-thread guest context registration so the
  // VEH watchdog can read the WRITER thread's r1 (not just the main thread's).
  // Each guest-running host thread must call setCurrentGuestContext() before
  // entering recomp code: main harness, runGuestThreadEntry (ExCreateThread
  // workers), and any other host thread that executes guest fns.
  thread_local void* t_currentGuestContext = nullptr;
  thread_local size_t t_currentGuestLROffset = 0;
  thread_local size_t t_currentGuestCtrOffset = 0;
  thread_local size_t t_currentGuestR3Offset = 0;
  thread_local size_t t_currentGuestR1Offset = 0;
  thread_local size_t t_currentGuestR31Offset = 0;
}

// iter-295g: the all-threads registry (see guest_context.hpp). Exported flat
// so tools/frida_mcp.py can enumerate every guest-running thread's live regs.
extern "C" __declspec(dllexport) GuestThreadSlot g_debugThreadRegistry[64] = {};
extern "C" __declspec(dllexport) uint32_t g_debugThreadRegistrySlots = 64;

namespace {
thread_local int t_registrySlot = -1;

void registryUpdate(void* ctx, size_t lrOff, size_t ctrOff, size_t r3Off, size_t r1Off) {
  const uint32_t tid = GetCurrentThreadId();
  if (ctx == nullptr) {                       // deregister
    if (t_registrySlot >= 0) {
      GuestThreadSlot& s = g_debugThreadRegistry[t_registrySlot];
      s.ctx = nullptr;
      s.tid = 0;
      t_registrySlot = -1;
    }
    return;
  }
  if (t_registrySlot < 0) {                   // claim a free slot
    for (int i = 0; i < 64; ++i) {
      uint32_t expected = 0;
      if (InterlockedCompareExchange(
              reinterpret_cast<volatile LONG*>(&g_debugThreadRegistry[i].tid),
              (LONG)tid, 0) == 0) {
        t_registrySlot = i;
        break;
      }
      (void)expected;
    }
    if (t_registrySlot < 0) return;           // registry full — diagnostics only
  }
  GuestThreadSlot& s = g_debugThreadRegistry[t_registrySlot];
  s.seq++;
  s.lrOff = (uint32_t)lrOff; s.r1Off = (uint32_t)r1Off;
  s.r3Off = (uint32_t)r3Off; s.ctrOff = (uint32_t)ctrOff;
  s.ctx = ctx;                                // publish ctx last
}
}  // namespace

void setGuestThreadName(const char* name) {
  if (t_registrySlot < 0 || !name) return;
  GuestThreadSlot& s = g_debugThreadRegistry[t_registrySlot];
  size_t i = 0;
  for (; i < sizeof(s.name) - 1 && name[i]; ++i) s.name[i] = name[i];
  s.name[i] = 0;
}

namespace {
// Fault-FREE slot read. A registered thread can exit between our tid check
// and the ctx reads, leaving ctx on a freed stack. We must not fault AT ALL
// (the harness VEH treats any host AV as a fatal crash before SEH filters
// run — 3/3 boots died at the first sampler tick with a __try guard), so
// validate the pages with VirtualQuery first.
// THE fault-free predicate for every host read a diagnostic makes. A range is
// safe ONLY if every page it spans is committed, readable, and NOT a guard page.
//
// iter-369 — why this exists as a shared helper. PAGE_GUARD (0x100) is a
// MODIFIER bit on top of the base protection, so a thread stack's guard page
// reports PAGE_READWRITE|PAGE_GUARD == 0x104. The old checks either masked with
// 0xFF (dropping the 0x100 outright) or ANDed against the readable set (0x104 &
// PAGE_READWRITE is still non-zero) — either way a guard page looked perfectly
// readable. Touching it raises STATUS_GUARD_PAGE_VIOLATION (0x80000001), which
// nothing in the harness handles, so the process DIES; it also consumes the
// guard page, breaking that thread's stack growth.
// Measured: this killed 2 of 3 natural boots at ~85s. The sampler reads a guest
// PPCContext that lives on ANOTHER thread's host stack, so as that thread's
// stack grows the context sits nearer the guard page until a sample walks in.
// Walks region by region, so a read straddling a page/region edge is covered.
}  // namespace

// ── iter-458 H-VQ: the readable-interval cache (contract in guest_context.hpp) ──
namespace hrr {
struct Cache {
  std::mutex mu;
  uint64_t gen = 0;
  uintptr_t resBase = 0, resEnd = 0;
  std::vector<std::pair<uintptr_t, uintptr_t>> iv;   // sorted, disjoint, coalesced
};
Cache& cache() {
  static Cache c;
  return c;
}
std::atomic<uint64_t> g_gen{1};
std::atomic<int> g_removers{0};   // active removals (Astra 041): no hits, no refills while > 0
std::atomic<int> g_testMode{-1};
std::atomic<void (*)()> g_beforeInsert{nullptr};
std::atomic<void (*)()> g_afterGenSample{nullptr};   // test seam (Astra 040 interleaving)
std::atomic<uint64_t> s_calls{0}, s_cached{0}, s_hits{0}, s_vq{0}, s_vqNs{0},
    s_maxVqNs{0}, s_inval{0}, s_stale{0};
std::atomic<int> s_lastMode{-1};
std::atomic<int64_t> s_lastSnapMs{0};
std::atomic<uint64_t> s_clockReads{0};   // I15 P1: the snapshot clock's reads under SR_GUESTMEM_LEAN (1 per 1024 calls)
const std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();
// Step 6 I15's combined switch (SR_GUESTMEM_LEAN, default on; =0 every piece off - today's code), read once.
bool guestMemLean() noexcept {
  static const bool on = [] {
    const char* e = std::getenv("SR_GUESTMEM_LEAN");
    return !(e && e[0] == '0');
  }();
  return on;
}

using Cfg = HrrConfig;
const Cfg& cfg() {
  static const Cfg c = [] {
    const char* e = std::getenv("SR_HRR_CACHE");
    const char* w = std::getenv("SR_HRR_CACHE_WINDOW");
    const Cfg r = hrrConfigFromEnv(e, w);
    SR_LOG_BOOT(r.badWindow ? sr::LogSeverity::Error : sr::LogSeverity::Info,
        "[HRR] config: SR_HRR_CACHE=%s window=%s%s%s -> enabled=%d windowed=%d on=%.1fs off=%.1fs%s",
        e ? e : "(unset)", w ? "'" : "", w ? w : "(unset)", w ? "'" : "", (int)r.enabled,
        (int)r.windowed, r.onAt, r.offAt,
        r.badWindow ? " | UNPARSABLE WINDOW - cache forced OFF" : "");
    return r;
  }();
  return c;
}

void logStats(const char* why) {
  const HostRangeReadableStats s = hostRangeReadableStats();
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[HRR] %s mode=%s t=%.1fs | calls=%llu cached=%llu hits=%llu vq=%llu "
      "vqMs=%.1f maxVqMs=%.2f invalidations=%llu staleDrops=%llu lean=%s clockReads=%llu",
      why, s.cacheOn ? "ON" : "OFF",
      std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count(),
      (unsigned long long)s.calls, (unsigned long long)s.cachedCalls,
      (unsigned long long)s.hits, (unsigned long long)s.vqCalls, s.vqNs / 1e6,
      s.maxVqNs / 1e6, (unsigned long long)s.invalidations,
      (unsigned long long)s.staleDrops, guestMemLean() ? "on" : "off",
      (unsigned long long)s_clockReads.load(std::memory_order_relaxed));
  // (I15 P1: with lean=on a "snapshot" line may come up to 1023 hrrQuery calls after its 10 s mark - see hrrQuery.)
}

bool cacheOn() noexcept {
  const int t = g_testMode.load(std::memory_order_relaxed);
  bool on;
  if (t >= 0) {
    on = t == 1;
  } else {
    const Cfg& c = cfg();
    on = c.enabled &&
         (!c.windowed ||
          hrrWindowActive(std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - g_t0).count(),
                          c.onAt, c.offAt));
  }
  const int m = on ? 1 : 0;
  if (s_lastMode.exchange(m, std::memory_order_relaxed) != m) logStats("mode");
  return on;
}
}  // namespace hrr

bool hrrWindowActive(double t, double onAt, double offAt) noexcept {
  return t >= onAt && t < offAt;
}

bool hrrParseWindow(const char* s, double& onAt, double& offAt) noexcept {
  if (!s || !*s) return false;
  char* rest = nullptr;
  const double a = std::strtod(s, &rest);
  if (rest == s) return false;
  const char* q = rest;
  if (!*q || !std::strchr("-:;,", *q)) return false;
  ++q;
  char* tail = nullptr;
  const double b = std::strtod(q, &tail);
  if (tail == q || *tail || !std::isfinite(a) || !std::isfinite(b) || !(b > a) || a < 0)
    return false;
  onAt = a;
  offAt = b;
  return true;
}

HrrConfig hrrConfigFromEnv(const char* cacheEnv, const char* windowEnv) noexcept {
  HrrConfig r;
  // iter-464 (movie lane): DEFAULT ON, `=0` keeps the uncached A/B. Measured on the
  // natural boot (f_main_final2, cache off): during the 31 s title movie the render
  // thread made ~3 uncached VirtualQuery walks per frame (the HLE observe layer's
  // TexUnlock snapshot of the three movie planes, checkedGuestSpan) at ~9.6 ms each =
  // 647 ms of every second, so the guest render loop ran ~22 Hz instead of ~30 Hz and
  // the movie, which consumes one frame per loop and never drops, played 38% long.
  r.enabled = !cacheEnv || cacheEnv[0] != '0';
  // "<on>-<off>": sr_drive splits -Gates on commas, so a comma separator never
  // survives the driver (iter-458 hrr1 ran with the window lost). A window that
  // is SET - even to "" - is a request: unparsable means fail CLOSED, never
  // "always on" (Astra 037).
  if (windowEnv) {
    double a = 0, b = 0;
    if (hrrParseWindow(windowEnv, a, b)) {
      r.windowed = true;
      r.onAt = a;
      r.offAt = b;
    } else {
      r.badWindow = true;
      r.enabled = false;
    }
  }
  return r;
}

void hostRangeReadableInvalidate() noexcept {
  hrr::g_gen.fetch_add(1, std::memory_order_seq_cst);
  hrr::s_inval.fetch_add(1, std::memory_order_relaxed);
}

// Order matters (Astra 041): Begin counts the remover BEFORE bumping, End bumps
// BEFORE uncounting - so any insert that sees removers == 0 also sees a
// generation from before Begin (its interval is cleared by the next lookup) or
// from after End (a fresh walk after the removal). seq_cst throughout.
void hostRangeReadableBeginRemoval() noexcept {
  hrr::g_removers.fetch_add(1, std::memory_order_seq_cst);
  hrr::g_gen.fetch_add(1, std::memory_order_seq_cst);
  hrr::s_inval.fetch_add(1, std::memory_order_relaxed);
}

void hostRangeReadableEndRemoval() noexcept {
  hrr::g_gen.fetch_add(1, std::memory_order_seq_cst);
  hrr::g_removers.fetch_sub(1, std::memory_order_seq_cst);
  hrr::s_inval.fetch_add(1, std::memory_order_relaxed);
}

bool releaseGuestArena(uint8_t* base, size_t size) noexcept {
  (void)size;
  const bool drained = guest_protect::retireArena(kArenaRetireMs);
  if (!drained) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[NATIVE_TRANSPORT] guest arena release skipped: %u native call(s) still hold the arena after %u ms; "
                "it stays mapped", guest_protect::arenaLeases(), kArenaRetireMs);
    return false;
  }
#if defined(_WIN32)
  // iter-458 H-VQ: releasing the reservation removes readability while other
  // threads may still run (the known teardown race, hle_api_observe.cpp) - the
  // readable-interval cache must not keep answering "readable". Protocol: bump
  // before AND after (kernel/guest_context.hpp).
  hostRangeReadableBeginRemoval();
  const bool released = VirtualFree(base, 0, MEM_RELEASE) != 0;
  hostRangeReadableEndRemoval();
  return released;
#else
  (void)base;
  return false;
#endif
}

HostRangeReadableStats hostRangeReadableStats() noexcept {
  return HostRangeReadableStats{
      hrr::s_calls.load(), hrr::s_cached.load(), hrr::s_hits.load(),
      hrr::s_vq.load(),    hrr::s_vqNs.load(),   hrr::s_maxVqNs.load(),
      hrr::s_inval.load(), hrr::s_stale.load(),
      hrr::s_lastMode.load(std::memory_order_relaxed) == 1};
}

void hostRangeReadableSetCacheForTest(int mode) noexcept {
  hrr::g_testMode.store(mode, std::memory_order_relaxed);
}

void hostRangeReadableSetBeforeInsertHookForTest(void (*fn)()) noexcept {
  hrr::g_beforeInsert.store(fn, std::memory_order_relaxed);
}

void hostRangeReadableSetAfterGenSampleHookForTest(void (*fn)()) noexcept {
  hrr::g_afterGenSample.store(fn, std::memory_order_relaxed);
}

static bool hrrQuery(const void* p, size_t bytes, bool force) noexcept {
#if defined(_WIN32)
  if (!p || !bytes) return false;
  const uintptr_t s = reinterpret_cast<uintptr_t>(p);
  if (bytes > UINTPTR_MAX - s) return false;   // overflow-safe: no wrapped span
  const uintptr_t e = s + bytes;
  const uint64_t nCalls = hrr::s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((nCalls & 0xFFFFFu) == 0) hrr::logStats("periodic");
  // Astra 036: a snapshot at least every 10 s, so per-window deltas are exact
  // at every measurement boundary (the window edges are process age).
  // Step 6 I15 P1 (J:/sr_lanec_drawthread/FREEZE_I15_20261024.txt; SR_GUESTMEM_LEAN, default on, =0 the old every-call
  // read): the clock is read only on every 1024th call - steady_clock::now() on every call was 61 % of the draw
  // thread's QPC (~0.08 ms per present). The [HRR] snapshot can therefore be up to 1023 calls LATE: milliseconds at the
  // render loop's call rate, longer in an idle phase - a diagnostics cadence, not a correctness edge. The cache's
  // invalidation protocol (g_gen, removers) is untouched: this is only the stats clock.
  if (!hrr::guestMemLean() || (nCalls & 1023u) == 0) {
    if (hrr::guestMemLean()) hrr::s_clockReads.fetch_add(1, std::memory_order_relaxed);
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - hrr::g_t0).count();
    int64_t last = hrr::s_lastSnapMs.load(std::memory_order_relaxed);
    if (nowMs - last >= 10000 &&
        hrr::s_lastSnapMs.compare_exchange_strong(last, nowMs, std::memory_order_relaxed))
      hrr::logStats("snapshot");
  }
  const uintptr_t rb = hrr::g_resBase.load(std::memory_order_acquire);
  const uintptr_t re = hrr::g_resEnd.load(std::memory_order_acquire);
  const bool inRes = rb != 0 && s >= rb && e <= re;
  const bool useCache = inRes && (force || hrr::cacheOn());
  uint64_t gen = 0;
  if (useCache) {
    hrr::s_cached.fetch_add(1, std::memory_order_relaxed);
    gen = hrr::g_gen.load(std::memory_order_acquire);
    if (void (*hook)() = hrr::g_afterGenSample.load(std::memory_order_relaxed)) hook();
    hrr::Cache& c = hrr::cache();
    if (c.mu.try_lock()) {   // never blocks: callable from VEH diagnostics
      // Astra 040: the generation MUST be re-read under the lock. A sample taken
      // before it can be stale: invalidate + release + invalidate may all land
      // between the sample and the lock, and with no other reader in between
      // the cache still carries that stale generation - a stale HIT for a
      // released range. Read under the lock, every lookup that starts after a
      // remover's first bump misses; one that finished before it is equivalent
      // to an uncached VirtualQuery taken before the removal.
      gen = hrr::g_gen.load(std::memory_order_acquire);
      if (c.gen != gen || c.resBase != rb || c.resEnd != re) {
        c.iv.clear();
        c.gen = gen;
        c.resBase = rb;
        c.resEnd = re;
      }
      bool hit = false;
      auto it = std::upper_bound(
          c.iv.begin(), c.iv.end(), s,
          [](uintptr_t v, const std::pair<uintptr_t, uintptr_t>& iv) { return v < iv.first; });
      // No answers while a removal is active (Astra 041). Defence in depth: the
      // Begin bump already empties the cache and inserts are refused below.
      if (hrr::g_removers.load(std::memory_order_seq_cst) == 0 && it != c.iv.begin()) {
        --it;
        hit = it->first <= s && e <= it->second;
      }
      c.mu.unlock();
      if (hit) {
        hrr::s_hits.fetch_add(1, std::memory_order_relaxed);
        return true;
      }
    }
  }
  // The walk: one VirtualQuery per region; readable regions inside the
  // reservation are remembered for the cache (at most 8 per call).
  std::pair<uintptr_t, uintptr_t> found[8];
  int nFound = 0;
  bool overflowFound = false;
  uintptr_t cur = s;
  while (cur < e) {
    MEMORY_BASIC_INFORMATION mbi{};
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    const SIZE_T got = VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof(mbi));
    QueryPerformanceCounter(&q1);
    {
      static const double s_nsPerTick = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e9 / (double)f.QuadPart;
      }();
      const uint64_t ns = (uint64_t)((q1.QuadPart - q0.QuadPart) * s_nsPerTick);
      hrr::s_vq.fetch_add(1, std::memory_order_relaxed);
      hrr::s_vqNs.fetch_add(ns, std::memory_order_relaxed);
      uint64_t mx = hrr::s_maxVqNs.load(std::memory_order_relaxed);
      while (ns > mx && !hrr::s_maxVqNs.compare_exchange_weak(mx, ns, std::memory_order_relaxed)) {}
    }
    if (!got) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD base = mbi.Protect & 0xFF;
    if (base != PAGE_READONLY && base != PAGE_READWRITE &&
        base != PAGE_EXECUTE_READ && base != PAGE_EXECUTE_READWRITE &&
        base != PAGE_WRITECOPY && base != PAGE_EXECUTE_WRITECOPY) return false;
    const uintptr_t rBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t regionEnd =
        mbi.RegionSize > UINTPTR_MAX - rBase ? UINTPTR_MAX : rBase + mbi.RegionSize;
    if (regionEnd <= cur) return false;   // no forward progress — bail
    if (useCache) {
      const uintptr_t a = rBase > rb ? rBase : rb;
      const uintptr_t z = regionEnd < re ? regionEnd : re;
      if (a < z) {
        if (nFound < 8) found[nFound++] = {a, z};
        else overflowFound = true;
      }
    }
    cur = regionEnd;
  }
  if (useCache && nFound && !overflowFound) {
    if (void (*hook)() = hrr::g_beforeInsert.load(std::memory_order_relaxed)) hook();
    hrr::Cache& c = hrr::cache();
    if (c.mu.try_lock()) {
      // No refills while a removal is active (Astra 041): a walk that ran after
      // Begin may have seen the range readable just before it was removed.
      // Removers are read BEFORE the generation (the mirror of Begin's order).
      if (hrr::g_removers.load(std::memory_order_seq_cst) == 0 &&
          hrr::g_gen.load(std::memory_order_seq_cst) == gen && c.gen == gen &&
          c.resBase == rb && c.resEnd == re) {
        for (int i = 0; i < nFound; i++) {
          uintptr_t a = found[i].first, z = found[i].second;
          // Coalesce with every overlapping or touching interval.
          auto it = std::lower_bound(
              c.iv.begin(), c.iv.end(), a,
              [](const std::pair<uintptr_t, uintptr_t>& iv, uintptr_t v) { return iv.second < v; });
          while (it != c.iv.end() && it->first <= z) {
            if (it->first < a) a = it->first;
            if (it->second > z) z = it->second;
            it = c.iv.erase(it);
          }
          c.iv.insert(it, {a, z});
        }
      } else {
        hrr::s_stale.fetch_add(1, std::memory_order_relaxed);
      }
      c.mu.unlock();
    }
  }
#else
  (void)p; (void)bytes; (void)force;
#endif
  return true;
}

bool hostRangeReadable(const void* p, size_t bytes) noexcept {
  return hrrQuery(p, bytes, false);
}

bool hostRangeReadableCached(const void* p, size_t bytes) noexcept {
  return hrrQuery(p, bytes, true);
}

namespace {
bool sampleSlotSafe(void* ctx, const uint32_t offs[4], uint32_t out[4]) noexcept {
  uint32_t maxOff = 0;
  for (int k = 0; k < 4; ++k) if (offs[k] > maxOff) maxOff = offs[k];
  if (!hostRangeReadable(ctx, size_t(maxOff) + 4)) return false;
  for (int k = 0; k < 4; ++k)
    out[k] = offs[k] ? *reinterpret_cast<volatile uint32_t*>(
                           static_cast<char*>(ctx) + offs[k]) : 0;
  return true;
}
}  // namespace

// iter-388e: on-demand all-guest-threads snapshot (same mechanics as the 20s
// sampler) — called by the image-corruption poll watcher the instant it sees
// the transition, so the mid-loop bulk writer's thread shows its PC/LR.
void dumpGuestThreadsNow(const char* tag) {
  for (int i = 0; i < 64; ++i) {
    const GuestThreadSlot& s = g_debugThreadRegistry[i];
    void* ctx = s.ctx;
    if (!s.tid || !ctx) continue;
    const uint32_t offs[4] = { s.lrOff, s.r1Off, s.r3Off, s.ctrOff };
    uint32_t v[4] = {};
    if (!sampleSlotSafe(ctx, offs, v)) continue;
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[%s] tid=%u name=%s lr=%08X r1=%08X r3=%08X ctr=%08X",
                tag, s.tid, s.name[0] ? s.name : "-", v[0], v[1], v[2], v[3]);
  }
}

#if SR_ORACLE
void startGuestThreadSampler() {
  static std::atomic<bool> s_started{false};
  if (s_started.exchange(true)) return;
  static const bool s_on = [] {
    const char* e = std::getenv("SR_THREAD_SAMPLER");
    return !e || e[0] != '0';   // default ON; =0 to disable
  }();
  if (!s_on) return;
  std::thread([]() {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(20));
      for (int i = 0; i < 64; ++i) {
        const GuestThreadSlot& s = g_debugThreadRegistry[i];
        void* ctx = s.ctx;
        if (!s.tid || !ctx) continue;
        const uint32_t offs[4] = { s.lrOff, s.r1Off, s.r3Off, s.ctrOff };
        uint32_t v[4] = {};
        if (!sampleSlotSafe(ctx, offs, v)) continue;
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[THREAD_SAMPLER] tid=%u name=%s lr=%08X r1=%08X r3=%08X ctr=%08X",
                    s.tid, s.name[0] ? s.name : "-", v[0], v[1], v[2], v[3]);
        // iter-307: FMPhysics factory-era wedge — thread parks with stale
        // lr=0x8256B410 (pass-1 bctrl) / ctr=0x8256CBC0 for minutes, meaning a
        // call-less compute loop somewhere under the factory (suspect: the
        // string-map probe loop sub_828A9440). Registers can't name the frame;
        // the STACK can: dump raw words + follow the PPC backchain ([r1] =
        // caller r1; saved LR sits in the caller frame at [chain+? — log raw
        // words and let the analyst spot 0x82xxxxxx return addrs]. Fault-free
        // reads only.
        // iter-310: generalized trigger — ANY side-stack (walker/EXTHREAD,
        // r1 in 0xBE000000..0xBF000000) thread whose lr repeats across
        // consecutive samples is a wedge candidate; dump its stack. (The old
        // fixed lr==0x8256B410 trigger missed the FMGameScript-era wedge.)
        static uint32_t s_lastWalkerLR = 0;
        static int s_walkerRepeat = 0;
        bool walkerStuck = false;
        if (v[1] >= 0xBE000000u && v[1] < 0xBF000000u) {
          if (v[0] == s_lastWalkerLR) {
            if (++s_walkerRepeat >= 3) walkerStuck = true;
          } else { s_lastWalkerLR = v[0]; s_walkerRepeat = 0; }
        }
        if ((walkerStuck || v[0] == 0x8256B410u) && g_debugGuestBase) {
          static std::atomic<int> s_bcDumps{0};
          if (s_bcDumps.fetch_add(1, std::memory_order_relaxed) < 4) {
            auto rdq = [](uint32_t va, uint32_t& out) -> bool {
              if (va == 0) return false;
              const uint8_t* p = g_debugGuestBase + va;
              if (!hostRangeReadable(p, 4)) return false;
              out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                    (uint32_t(p[2]) << 8) | uint32_t(p[3]);
              return true;
            };
            char line[512]; int o = 0;
            uint32_t sp = v[1];
            for (int f = 0; f < 10 && o < 440; ++f) {
              uint32_t chain = 0, savedLR = 0;
              if (!rdq(sp, chain)) break;
              rdq(sp + 4, savedLR);   // CR/LR save area — heuristic, log raw
              uint32_t lrAbove = 0;
              rdq(chain + 4, lrAbove);
              o += snprintf(line + o, sizeof(line) - o, " f%d[r1=%08X +4=%08X ^+4=%08X]",
                            f, sp, savedLR, lrAbove);
              if (chain <= sp || chain - sp > 0x10000) break;
              sp = chain;
            }
            SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[WEDGE_BACKCHAIN] tid=%u%s", s.tid, line);
            // raw top-of-stack words: return addrs (0x82xxxxxx) identify frames
            char raw[600]; int ro = 0;
            for (uint32_t w = 0; w < 40 && ro < 540; ++w) {
              uint32_t val = 0;
              if (!rdq(v[1] + w * 4, val)) break;
              if (val >= 0x82000000u && val < 0x82A00000u)
                ro += snprintf(raw + ro, sizeof(raw) - ro, " +%02X=%08X", w * 4, val);
            }
            SR_LOG_BOOT(sr::LogSeverity::Info,
                        "[WEDGE_STACKCODE] tid=%u r1=%08X%s", s.tid, v[1], raw);
          }
        }
        // iter-303: factory-wedge dump. sub_8256B3C0's retry pass re-calls
        // factories that returned 0 forever, in a hard spin (no wait). The
        // pending nodes were UNLINKED from the global list (0x82979558) into a
        // stack-local array {ptr @ r1+0x50, count @ r1+0x54} (node layout:
        // +0 factoryFn, +4 next, +8 outSlot) — so when a thread samples inside
        // the retry loop [0x8256B484,0x8256B4F0), walk its frame and name the
        // stuck factory fns. Bounds-checked reads only (fault-free rule).
        if (v[0] >= 0x8256B484u && v[0] < 0x8256B4F0u && g_debugGuestBase) {
          static std::atomic<int> s_wedgeDumps{0};
          if (s_wedgeDumps.fetch_add(1, std::memory_order_relaxed) < 6) {
            // NOTE: g_guestBase/Size are thread_local and unset on this host
            // thread — use the process-global g_debugGuestBase, and VirtualQuery
            // every read (harness VEH makes any host AV fatal; never fault).
            auto rd = [](uint32_t va, uint32_t& out) -> bool {
              if (va == 0) return false;
              const uint8_t* p = g_debugGuestBase + va;
              // hostRangeReadable walks every region the 4 bytes span, so the
              // straddle case is covered without a second hand-rolled query.
              if (!hostRangeReadable(p, 4)) return false;
              out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                    (uint32_t(p[2]) << 8) | uint32_t(p[3]);
              return true;
            };
            uint32_t arrPtr = 0, count = 0;
            if (rd(v[1] + 0x50, arrPtr) && rd(v[1] + 0x54, count) && count > 0 &&
                count <= 256) {
              SR_LOG_BOOT(sr::LogSeverity::Info,
                          "[FACTORY_WEDGE] tid=%u retry array=0x%08X count=%u — stuck factories:",
                          s.tid, arrPtr, count);
              for (uint32_t j = 0; j < count && j < 32; ++j) {
                uint32_t node = 0, fn = 0, outSlot = 0, outVal = 0;
                if (!rd(arrPtr + j * 4, node) || !rd(node, fn)) break;
                rd(node + 8, outSlot);
                if (outSlot) rd(outSlot, outVal);
                SR_LOG_BOOT(sr::LogSeverity::Info,
                            "[FACTORY_WEDGE]   [%u] node=0x%08X factoryFn=0x%08X outSlot=0x%08X *outSlot=0x%08X",
                            j, node, fn, outSlot, outVal);
              }
            } else {
              SR_LOG_BOOT(sr::LogSeverity::Info,
                          "[FACTORY_WEDGE] tid=%u lr=%08X in retry loop but frame read failed (r1=%08X arrPtr=0x%08X count=%u)",
                          s.tid, v[0], v[1], arrPtr, count);
            }
          }
        }
      }
    }
  }).detach();
}

#endif  // SR_ORACLE: retired speculative guest-stack observer

// iter-361: SCRIBBLE_WATCH — deterministic, roll-independent host-side catch of
// the bad-roll scribble that clobbers the FE-service globals (dword_8296D878 +
// dword_8296D8D8/DC/E0/E4) to 0x000005FF, after which GameShutdown hangs. A
// tight poller (fault-free, VirtualQuery-guarded, harness-VEH-safe) watches the
// app-obj stash; the instant a previously-plausible pointer becomes garbage it
// dumps EVERY guest thread's live lr/r1/r3/ctr — the culprit thread's lr names
// the writer. Opt-in SR_SCRIBBLE_WATCH=1 (default OFF: burns one core polling).
#if SR_ORACLE
void startScribbleWatch() {
  static std::atomic<bool> s_started{false};
  if (s_started.exchange(true)) return;
  const char* e = std::getenv("SR_SCRIBBLE_WATCH");
  if (!e || e[0] != '1') return;
  std::thread([]() {
    constexpr uint32_t kWatchVA = 0x8296D878u;   // init2's app-obj stash
    auto rd = [](uint32_t va, uint32_t& out) -> bool {
      if (va == 0 || !g_debugGuestBase) return false;
      const uint8_t* p = g_debugGuestBase + va;
      if (!hostRangeReadable(p, 4)) return false;
      out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
            (uint32_t(p[2]) << 8) | uint32_t(p[3]);
      return true;
    };
    uint32_t lastGood = 0;
    bool caught = false;
    for (;;) {
      uint32_t v = 0;
      if (rd(kWatchVA, v)) {
        const bool plausible = (v >= 0x10000u && v < 0xC0000000u);
        if (plausible) {
          lastGood = v;
        } else if (!caught && lastGood != 0 && v != lastGood) {
          caught = true;
          uint32_t s8 = 0, sC = 0, s0 = 0, s4 = 0;
          rd(0x8296D8D8u, s8); rd(0x8296D8DCu, sC);
          rd(0x8296D8E0u, s0); rd(0x8296D8E4u, s4);
          SR_LOG_BOOT(sr::LogSeverity::Error,
            "[SCRIBBLE_WATCH] dword_8296D878 CLOBBERED 0x%08X -> 0x%08X "
            "(services D8=%08X DC=%08X E0=%08X E4=%08X) — dumping all guest threads",
            lastGood, v, s8, sC, s0, s4);
          for (int i = 0; i < 64; ++i) {
            const GuestThreadSlot& s = g_debugThreadRegistry[i];
            void* ctx = s.ctx;
            if (!s.tid || !ctx) continue;
            const uint32_t offs[4] = { s.lrOff, s.r1Off, s.r3Off, s.ctrOff };
            uint32_t rv[4] = {};
            if (!sampleSlotSafe(ctx, offs, rv)) continue;
            SR_LOG_BOOT(sr::LogSeverity::Error,
              "[SCRIBBLE_WATCH] tid=%u name=%s lr=%08X r1=%08X r3=%08X ctr=%08X",
              s.tid, s.name[0] ? s.name : "-", rv[0], rv[1], rv[2], rv[3]);
          }
        }
      }
      // Busy-poll (no sleep) until caught to minimize the store->dump gap, then
      // idle. The scribble is a one-shot event; a brief hot core is acceptable.
      if (caught) std::this_thread::sleep_for(std::chrono::seconds(5));
    }
  }).detach();
}

// iter-309: PIXEL PROOF — with full init landed (iter-308) the game submits
// real cmd43 draws (1664/run) and presents 1000+ frames; dump what the swap
// chain actually shows. Wall-clock thread writes the current guest
// framebuffer as BMP at 120/240/360s (frame_dump_runN_T.bmp in cwd).
// SR_FRAME_DUMP=0 disables.
void startFrameDumpThread() {
  static std::atomic<bool> s_started{false};
  if (s_started.exchange(true)) return;
  static const bool s_on = [] {
    // iter-465 player: test infrastructure - automated rolls only (it writes BMPs into the
    // working folder); SR_FRAME_DUMP=1/0 decides when set.
    const char* e = std::getenv("SR_FRAME_DUMP");
    if (e && *e) return e[0] != '0';
    const char* inst = std::getenv("SR_INSTANCE_ID");
    return inst && *inst;
  }();
  if (!s_on) return;
  std::thread([]() {
    const char* inst = std::getenv("SR_INSTANCE_ID");
    std::string tag = inst ? inst : "solo";
    // iter-435: SR_FRAME_DUMP_SECS="20,35,50" overrides the 120/240/360
    // schedule (FMV-era evidence: the movie window ends before shot 1).
    int times[16] = {120, 240, 360};
    int nTimes = 3;
    if (const char* ts = std::getenv("SR_FRAME_DUMP_SECS")) {
      nTimes = 0;
      int v = 0; bool have = false;
      for (const char* p = ts;; ++p) {
        if (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); have = true; }
        else { if (have && nTimes < 16) times[nTimes++] = v; v = 0; have = false; if (!*p) break; }
      }
      if (!nTimes) { times[0] = 120; times[1] = 240; times[2] = 360; nTimes = 3; }
    }
    int prev = 0;
    for (int shot = 1; shot <= nTimes; ++shot) {
      std::this_thread::sleep_for(std::chrono::seconds(times[shot - 1] - prev));
      prev = times[shot - 1];
      uint32_t va = 0; int w = 0, h = 0, pitch = 0;
      if (!getGuestFramebufferInfo(&va, &w, &h, &pitch) || !va || w <= 0 ||
          h <= 0 || !g_debugGuestBase) {
        SR_LOG_BOOT(sr::LogSeverity::Info, "[FRAME_DUMP] shot %d: no framebuffer yet", shot);
        continue;
      }
      const uint8_t* src = g_debugGuestBase + va;
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(src, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[FRAME_DUMP] shot %d: fb VA 0x%08X not committed", shot, va);
        continue;
      }
      char name[128];
      snprintf(name, sizeof(name), "frame_dump_%s_%ds.bmp", tag.c_str(), times[shot - 1]);
      FILE* fp = fopen(name, "wb");
      if (!fp) continue;
      const uint32_t imgSize = uint32_t(w) * uint32_t(h) * 4u;
      uint8_t fh[14] = {'B','M'}; uint32_t fsz = 14 + 40 + imgSize;
      memcpy(fh + 2, &fsz, 4); uint32_t off = 54; memcpy(fh + 10, &off, 4);
      fwrite(fh, 1, 14, fp);
      struct { uint32_t sz; int32_t w, h; uint16_t planes, bpp; uint32_t comp, isz; int32_t xppm, yppm; uint32_t clr, imp; }
        ih{40, w, -h, 1, 32, 0, imgSize, 2835, 2835, 0, 0};
      fwrite(&ih, 1, 40, fp);
      for (int y = 0; y < h; ++y) fwrite(src + size_t(y) * pitch, 1, size_t(w) * 4, fp);
      fclose(fp);
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FRAME_DUMP] shot %d written: %s (fb=0x%08X %dx%d pitch=%d)", shot, name, va, w, h, pitch);
    }
  }).detach();
}

// iter-358: dump the guest framebuffer ON DEMAND, tagged.
//
// The 120/240/360s timer thread MISSES the interesting frames: the game submits
// its geometry ~4 s into boot (one [D3D12] "Rendering guest geometry" burst),
// long before shot 1. Every timed dump therefore caught an empty post-burst
// frame and read as "we render nothing" — a timing artifact, not a fact.
// Callable from the render path so the shot lands in the frame that matters.
// Read-only w.r.t. guest state; VirtualQuery-guarded (harness VEH makes any host
// AV fatal before SEH filters run — diagnostics must be fault-free).
void dumpGuestFramebufferTagged(const char* why) {
  uint32_t va = 0; int w = 0, h = 0, pitch = 0;
  if (!getGuestFramebufferInfo(&va, &w, &h, &pitch) || !va || w <= 0 || h <= 0 ||
      !g_debugGuestBase)
    return;
  const uint8_t* src = g_debugGuestBase + va;
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(src, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return;
  const char* inst = std::getenv("SR_INSTANCE_ID");
  char name[160];
  snprintf(name, sizeof(name), "frame_%s_%s.bmp", inst ? inst : "solo",
           why ? why : "now");
  FILE* fp = fopen(name, "wb");
  if (!fp) return;
  const uint32_t imgSize = uint32_t(w) * uint32_t(h) * 4u;
  uint8_t fh[14] = {'B','M'}; uint32_t fsz = 14 + 40 + imgSize;
  memcpy(fh + 2, &fsz, 4); uint32_t off = 54; memcpy(fh + 10, &off, 4);
  fwrite(fh, 1, 14, fp);
  struct { uint32_t sz; int32_t w, h; uint16_t planes, bpp; uint32_t comp, isz; int32_t xppm, yppm; uint32_t clr, imp; }
    ih{40, w, -h, 1, 32, 0, imgSize, 2835, 2835, 0, 0};
  fwrite(&ih, 1, 40, fp);
  for (int y = 0; y < h; ++y) fwrite(src + size_t(y) * pitch, 1, size_t(w) * 4, fp);
  fclose(fp);
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[FRAME_DUMP] on-demand '%s' written: %s (fb=0x%08X %dx%d pitch=%d)",
    why ? why : "now", name, va, w, h, pitch);
}

#endif  // SR_ORACLE: legacy guest-framebuffer dump path

void setCurrentGuestContext(void* ctx, size_t lrOffsetBytes, size_t ctrOffsetBytes, size_t r3OffsetBytes, size_t r1OffsetBytes, size_t r31OffsetBytes) {
  t_currentGuestContext = ctx;
  t_currentGuestLROffset = lrOffsetBytes;
  t_currentGuestCtrOffset = ctrOffsetBytes;
  t_currentGuestR3Offset = r3OffsetBytes;
  t_currentGuestR1Offset = r1OffsetBytes;
  t_currentGuestR31Offset = r31OffsetBytes;
  registryUpdate(ctx, lrOffsetBytes, ctrOffsetBytes, r3OffsetBytes, r1OffsetBytes);
#if SR_ORACLE
  startGuestThreadSampler();
#endif
  startGuestProfiler();    // iter-461: SR_GUEST_PROFILE_MS (off unless set)
#if SR_ORACLE
  startScribbleWatch();    // retired scribble observer
  startFrameDumpThread();  // legacy guest-memory dump, not the native presented image
#endif
}

uint32_t getCurrentGuestPC() {
  if (!t_currentGuestContext || t_currentGuestLROffset == 0) return 0;
  return *reinterpret_cast<uint32_t*>(static_cast<char*>(t_currentGuestContext) + t_currentGuestLROffset);
}

uint32_t getCurrentGuestCtr() {
  if (!t_currentGuestContext || t_currentGuestCtrOffset == 0) return 0;
  return *reinterpret_cast<uint32_t*>(static_cast<char*>(t_currentGuestContext) + t_currentGuestCtrOffset);
}

uint32_t getCurrentGuestR3() {
  if (!t_currentGuestContext || t_currentGuestR3Offset == 0) return 0;
  return *reinterpret_cast<uint32_t*>(static_cast<char*>(t_currentGuestContext) + t_currentGuestR3Offset);
}

uint32_t getCurrentGuestR5() {
  // iter-344: r3..r5 are contiguous 8-byte PPCRegister fields; r5 = r3 + 16.
  if (!t_currentGuestContext || t_currentGuestR3Offset == 0) return 0;
  return *reinterpret_cast<uint32_t*>(static_cast<char*>(t_currentGuestContext) + t_currentGuestR3Offset + 16);
}

uint32_t getCurrentGuestR1() {
  if (!t_currentGuestContext || t_currentGuestR1Offset == 0) return 0;
  return *reinterpret_cast<uint32_t*>(static_cast<char*>(t_currentGuestContext) + t_currentGuestR1Offset);
}

void setCurrentGuestR1(uint32_t r1) {
  if (t_currentGuestContext && t_currentGuestR1Offset != 0)
    *reinterpret_cast<uint32_t*>(static_cast<char*>(t_currentGuestContext) + t_currentGuestR1Offset) = r1;
}

uint32_t getCurrentGuestR31() {
  if (!t_currentGuestContext || t_currentGuestR31Offset == 0) return 0;
  return *reinterpret_cast<uint32_t*>(static_cast<char*>(t_currentGuestContext) + t_currentGuestR31Offset);
}

uint32_t getCurrentGuestRegAtOffset(size_t offsetBytes) {
  if (!t_currentGuestContext || offsetBytes == 0) return 0;
  return *reinterpret_cast<uint32_t*>(static_cast<char*>(t_currentGuestContext) + offsetBytes);
}

namespace {
  // iter-24w: Process-global pointer to MAIN guest thread's PPCContext.
  // Readable from host poller threads to sample what guest fn the main thread
  // is currently executing. Written once at boot (and cleared at shutdown);
  // reads are intentionally relaxed — sampling is best-effort diagnostic.
  std::atomic<void*> g_mainGuestContext{nullptr};
  std::atomic<size_t> g_mainGuestLROffset{0};
  std::atomic<size_t> g_mainGuestCtrOffset{0};
  std::atomic<size_t> g_mainGuestR3Offset{0};
  std::atomic<size_t> g_mainGuestR1Offset{0};
}

void setMainGuestContext(void* ctx, size_t lrOffsetBytes, size_t ctrOffsetBytes,
                         size_t r3OffsetBytes, size_t r1OffsetBytes) {
  g_mainGuestLROffset.store(lrOffsetBytes, std::memory_order_relaxed);
  g_mainGuestCtrOffset.store(ctrOffsetBytes, std::memory_order_relaxed);
  g_mainGuestR3Offset.store(r3OffsetBytes, std::memory_order_relaxed);
  g_mainGuestR1Offset.store(r1OffsetBytes, std::memory_order_relaxed);
  g_mainGuestContext.store(ctx, std::memory_order_release);
}

uint32_t getMainGuestPC() {
  void* ctx = g_mainGuestContext.load(std::memory_order_acquire);
  size_t off = g_mainGuestLROffset.load(std::memory_order_relaxed);
  if (!ctx || off == 0) return 0;
  return *reinterpret_cast<uint32_t*>(static_cast<char*>(ctx) + off);
}
uint32_t getMainGuestCtr() {
  void* ctx = g_mainGuestContext.load(std::memory_order_acquire);
  size_t off = g_mainGuestCtrOffset.load(std::memory_order_relaxed);
  if (!ctx || off == 0) return 0;
  return *reinterpret_cast<uint32_t*>(static_cast<char*>(ctx) + off);
}
uint32_t getMainGuestR3() {
  void* ctx = g_mainGuestContext.load(std::memory_order_acquire);
  size_t off = g_mainGuestR3Offset.load(std::memory_order_relaxed);
  if (!ctx || off == 0) return 0;
  return *reinterpret_cast<uint32_t*>(static_cast<char*>(ctx) + off);
}
uint32_t getMainGuestR1() {
  void* ctx = g_mainGuestContext.load(std::memory_order_acquire);
  size_t off = g_mainGuestR1Offset.load(std::memory_order_relaxed);
  if (!ctx || off == 0) return 0;
  return *reinterpret_cast<uint32_t*>(static_cast<char*>(ctx) + off);
}

uint32_t getMainGuestR13() {
  // iter-345d: r13 = Xenon TLS register — the VD ISR's vsync-enable gate reads
  // through it. r3..r13 are contiguous 8-byte PPCRegisters: r13 = r3 + 10*8.
  void* ctx = g_mainGuestContext.load(std::memory_order_acquire);
  size_t off = g_mainGuestR3Offset.load(std::memory_order_relaxed);
  if (!ctx || off == 0) return 0;
  return *reinterpret_cast<uint32_t*>(static_cast<char*>(ctx) + off + 10 * 8);
}

// --- Main-thread identification and kernel call histogram ---
// g_isMainGuestThread is thread_local (only true on the harness thread).
// Histogram data is static (shared) since only the main guest thread writes
// (guarded by the TL flag) and the host main thread reads for dumps.
namespace {
  thread_local bool g_isMainGuestThread = false;

  struct CallHistEntry { const char* name; std::atomic<uint32_t> count; };
  static constexpr int kHistSize = 64;
  static CallHistEntry g_mainHist[kHistSize] = {};
  static std::atomic<int> g_mainHistUsed{0};
  static std::atomic<uint32_t> g_mainThreadCallCount{0};
}

void setIsMainGuestThread(bool v) { g_isMainGuestThread = v; }
bool isMainGuestThread() { return g_isMainGuestThread; }

void incrementMainThreadCallCount(const char* name) {
  if (!g_isMainGuestThread) return;
  g_mainThreadCallCount.fetch_add(1, std::memory_order_relaxed);
  int used = g_mainHistUsed.load(std::memory_order_acquire);
  if (used > kHistSize) used = kHistSize;  // cap to array bounds
  for (int i = 0; i < used; ++i) {
    const char* entryName = g_mainHist[i].name;
    // iter-464 perf: names are string literals - the pointer compare hits first.
    if (entryName == name || (entryName && std::strcmp(entryName, name) == 0)) {
      g_mainHist[i].count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  // Only claim a slot if there's room — don't inflate g_mainHistUsed past kHistSize
  int slot = g_mainHistUsed.load(std::memory_order_relaxed);
  if (slot >= kHistSize) return;  // histogram full, just count total
  if (g_mainHistUsed.compare_exchange_strong(slot, slot + 1, std::memory_order_acq_rel)) {
    g_mainHist[slot].name = name;
    g_mainHist[slot].count.store(1, std::memory_order_relaxed);
  }
}

void dumpMainThreadCallHistogram() {
  int used = g_mainHistUsed.load(std::memory_order_relaxed);
  uint32_t total = g_mainThreadCallCount.load(std::memory_order_relaxed);
  if (used == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MAIN_THREAD_HIST] (no main-thread kernel calls recorded)");
    return;
  }
  if (used > kHistSize) used = kHistSize;

  struct SortEntry { const char* name; uint32_t count; };
  SortEntry sorted[kHistSize];
  for (int i = 0; i < used; ++i) {
    sorted[i].name = g_mainHist[i].name;
    sorted[i].count = g_mainHist[i].count.load(std::memory_order_relaxed);
  }
  for (int i = 0; i < used - 1; ++i)
    for (int j = i + 1; j < used; ++j)
      if (sorted[j].count > sorted[i].count)
        std::swap(sorted[i], sorted[j]);

  SR_LOG_BOOT(sr::LogSeverity::Info, "[MAIN_THREAD_HIST] totalMainCalls=%u top callers:", total);
  int toShow = used < 15 ? used : 15;
  for (int i = 0; i < toShow; ++i) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MAIN_THREAD_HIST]   %s: %u", sorted[i].name, sorted[i].count);
  }
  // Reset counters for next interval
  for (int i = 0; i < used; ++i)
    g_mainHist[i].count.store(0, std::memory_order_relaxed);
  g_mainThreadCallCount.store(0, std::memory_order_relaxed);
}

uint32_t getMainThreadCallCount() {
  return g_mainThreadCallCount.load(std::memory_order_relaxed);
}

void logSub8276A4B0SearchRange(uint32_t key, uint32_t start, uint32_t end) {
  static unsigned n = 0;
  if (n++ < 2) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] sub_8276A4B0 table search: key=0x%08X range [0x%08X, 0x%08X) – list at 20(r31) must contain key; find populator (no cap)", key, start, end);
  }
}

void logSub827607B0HashLoop(uint32_t r5, uint32_t r10) {
  static unsigned n = 0;
  if (n++ < 2) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] sub_827607B0 hash loop: r5 (length)=0x%08X r10 (step)=0x%08X – if r5 huge we spin here; fix caller/string (no cap)", r5, r10);
  }
}

void logSub82763C50LoopBounds(uint32_t off8, uint32_t off20, uint32_t off28, uint32_t off40) {
  static unsigned n = 0;
  if (n++ < 1) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] sub_82763C50 loop bounds: 8(r6)=0x%08X 20(r6)=0x%08X 28(r6)=0x%08X 40(r6)=0x%08X – if ranges huge we spin (idle lr=0x827644E0)", off8, off20, off28, off40);
  }
}

void logSub8279EC60Size(uint32_t r3_dest, uint32_t r4_src, uint32_t r5_size) {
  static unsigned n = 0;
  if (n++ < 1) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] sub_8279EC60 (memcpy) size r5=0x%08X (>16M) dest=0x%08X src=0x%08X – idle lr=0x82164870; proper fix: fix caller (sub_82164818 args r29/r30/r31), do not cap", r5_size, r3_dest, r4_src);
  }
}

void logSub82709470CtrZero(uint32_t vtable_base, uint32_t slot0) {
  static unsigned n = 0;
  if (n++ < 1) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] sub_82709470 ctr=0 vtable_base=0x%08X slot0=0x%08X – patch not applied (idle 0x8276061C); fix object/vtable init", vtable_base, slot0);
  }
}

void logSub82760928BadArgs(uint32_t r4, uint32_t r5, uint32_t r6, uint32_t lr) {
  static unsigned n = 0;
  if (n++ < 1) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] sub_82760928 bad args: r4=0x%08X r5=0x%08X r6=0x%08X caller_lr=0x%08X – fix caller so (r5,r6) valid for sub_82164818", r4, r5, r6, lr);
  }
}

void logGuardSkipSub82762540(uint32_t val32_r31) {
  static unsigned n = 0;
  if (n++ < 1) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] guard: skip sub_82762540 (32(r31)=0x%08X > 64K) – would call sub_82760928 with huge size; fix struct init", val32_r31);
  }
}

void logGuardSkipSub827605F8_82764244(uint32_t val8_16_r31) {
  static unsigned n = 0;
  if (n++ < 1) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] guard: skip sub_827605F8 at 0x82764244 (8(16(r31))=0x%08X > 64K) – would pass huge count; fix struct init", val8_16_r31);
  }
}

void logSub827607A0HugeR9(uint32_t r9_count) {
  static unsigned n = 0;
  if (n++ < 1) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] sub_827605F8 from 0x827607A0 with huge r9=0x%08X (r4=r9*2) – causes spin at 0x8276061C; fix sub_827606E0 table init", r9_count);
  }
}

void logWorkerTrace(const char* label, uint32_t val1, uint32_t val2) {
  static thread_local unsigned n = 0;
  unsigned c = ++n;
  if (c <= 200 || (c % 10000 == 0)) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[WORKER_TRACE] %s val1=0x%08X val2=0x%08X (call#%u)", label, val1, val2, c);
  }
}

void alertableYield(uint32_t ms) {
#ifdef _WIN32
  SleepEx(ms, TRUE);
#else
  (void)ms;
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

// L9-B: env-gated machine-readable guest-execution trace (SR_GUEST_TRACE=1). Writes the SAME
// canonical BINARY format as the instrumented-Xenia golden oracle (tools/xenia-trace/xenia_trace.h,
// scripts/l9/trace_format.md): a 32-byte "XENTRC01" header + fixed 32-byte records
// {guest_pc u32, lr u32, r3 u64, r1 u64, thread_id u32, tag u32}. This is the "ours" input the
// L9-C divergence bisector diffs against the golden.
//
// iter-216 UPGRADE: FULL per-function-entry granularity. PPC_FUNC_PROLOGUE (ppc_context.h) calls
// emitGuestTraceEntry() at EVERY guest function entry -- matching Xenia's JIT-prolog hook -- so the
// bisector gets a precise frontier instead of an indirect-call-only subset. The hot-path guard is a
// plain bool (g_guestTraceOn), so it's ~free when off. The guest VA is parsed from the function name
// (every recomp fn is "__imp__sub_XXXXXXXX"), only when tracing is on. r3/r1/lr come from ctx at entry.
#if SR_ORACLE
bool g_guestTraceOn = [] {
  const char* e = std::getenv("SR_GUEST_TRACE");
  return e && e[0] && std::strcmp(e, "0") != 0;
}();

#else
// The generated guest archive shares this ABI with Oracle; tracing has no implementation in release.
bool g_guestTraceOn = false;
#endif

// iter-427: set by allocGuestMemory (guest_harness.cpp) when the unified
// physical-memory mapping (SR_PHYS_UNIFY) succeeds. Read by aliasGuestVA.
bool g_physUnifyActive = false;

#if SR_ORACLE
namespace {
struct TraceRec { uint32_t guest_pc; uint32_t lr; uint64_t r3; uint64_t r1; uint32_t thread_id; uint32_t tag; };
static_assert(sizeof(TraceRec) == 32, "L9 trace record must be 32 bytes");
std::mutex g_traceMu;
FILE* g_traceFp = nullptr;
bool g_traceOpened = false;
std::atomic<uint64_t> g_traceCount{0};
constexpr uint64_t kTraceCap = 60000000ull;   // runaway backstop (~1.9 GB); early boot needs far less

void writeTraceRecord(uint32_t pc, uint32_t lr, uint32_t r3, uint32_t r1) {
  if (g_traceCount.fetch_add(1, std::memory_order_relaxed) >= kTraceCap) return;
  std::lock_guard<std::mutex> lk(g_traceMu);
  if (!g_traceOpened) {
    g_traceOpened = true;
    const char* path = std::getenv("SR_GUEST_TRACE_PATH");
    g_traceFp = std::fopen(path ? path : "sr_boot.trace", "wb");
    if (g_traceFp) {
      uint8_t hdr[32]; std::memset(hdr, 0, sizeof(hdr));
      std::memcpy(hdr, "XENTRC01", 8);
      uint32_t rsz = 32, etag = 0x04030201u;
      std::memcpy(hdr + 8, &rsz, 4); std::memcpy(hdr + 12, &etag, 4);
      std::fwrite(hdr, sizeof(hdr), 1, g_traceFp);
    }
  }
  if (g_traceFp) {
    TraceRec rec{ pc, lr, (uint64_t)r3, (uint64_t)r1, (uint32_t)getCurrentThreadHandle(), 0 };
    std::fwrite(&rec, sizeof(rec), 1, g_traceFp);
  }
}

// "__imp__sub_828DA928" -> 0x828DA928 (0 if not a sub_ name).
uint32_t parseFuncVA(const char* fn) {
  if (!fn) return 0;
  const char* p = std::strstr(fn, "sub_");
  if (!p) return 0;
  p += 4;
  uint32_t va = 0; int n = 0;
  for (; n < 8 && p[n]; ++n) {
    char c = p[n], d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else break;
    va = (va << 4) | (uint32_t)d;
  }
  return n == 8 ? va : 0;
}
}  // namespace

// Called from PPC_FUNC_PROLOGUE on every guest function entry (only when g_guestTraceOn).
// L9-C start-gate: SR_GUEST_TRACE_START=hexVA defers recording until the first entry whose
// guest_pc == that VA (that entry is itself recorded). Keeps deep goldens/ours aligned to a
// common anchor (e.g. FMUserInterface_GlobalInit 0x82151DA0) without tens of GB of pre-menu
// noise. Branch-light: when armed (steady state, incl. the unset/0 default) the per-entry cost
// is one relaxed atomic load + a predicted-not-taken branch.
void emitGuestTraceEntry(const char* func, uint32_t lr, uint32_t r3, uint32_t r1) {
  if (!g_guestTraceOn) return;            // double-check (the macro also guards)
  static const uint32_t s_traceStartVA = [] {
    const char* e = std::getenv("SR_GUEST_TRACE_START");
    return e ? (uint32_t)std::strtoul(e, nullptr, 16) : 0u;
  }();
  static std::atomic<bool> g_traceArmed{ s_traceStartVA == 0 };
  uint32_t va = parseFuncVA(func);
  if (!g_traceArmed.load(std::memory_order_relaxed)) {
    if (va != s_traceStartVA) return;     // gate closed: skip until the start VA is seen
    g_traceArmed.store(true, std::memory_order_relaxed);  // arm; fall through to record it
  }
  if (va) writeTraceRecord(va, lr, r3, r1);
}

// iter-214 (L4 observability): env-gated indirect/vtable/computed-call tracer.
// (Its old SR_GUEST_TRACE emit is gone -- superseded by the full per-function-entry hook above,
//  which would otherwise double-trace indirect-call targets.)
void traceIndirectCall(uint32_t target, uint32_t callerLR) {
  static std::atomic<int> s_state{-1};   // -1=uninit, 0=off, 1=on
  static uint32_t s_lo = 0, s_hi = 0;
  static std::atomic<uint64_t> s_count{0};
  int st = s_state.load(std::memory_order_relaxed);
  if (st < 0) {  // first call: read env once (idempotent if raced)
    const char* en = std::getenv("SR_TRACE_CALLS");
    if (en && en[0] == '1') {
      const char* lo = std::getenv("SR_TRACE_LO");
      const char* hi = std::getenv("SR_TRACE_HI");
      s_lo = lo ? (uint32_t)std::strtoul(lo, nullptr, 16) : 0x82000000u;
      s_hi = hi ? (uint32_t)std::strtoul(hi, nullptr, 16) : 0x82FFFFFFu;
      st = 1;
    } else {
      st = 0;
    }
    s_state.store(st, std::memory_order_relaxed);
  }
  if (st != 1 || target < s_lo || target > s_hi) return;
  uint64_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > 50000) return;   // flood cap
  SR_LOG_BOOT(sr::LogSeverity::Info, "[CALLTRACE] #%llu target=0x%08X lr=0x%08X",
              (unsigned long long)n, target, callerLR);
}

#else
void emitGuestTraceEntry(const char*, uint32_t, uint32_t, uint32_t) {}
void traceIndirectCall(uint32_t, uint32_t) {}
#endif

void logDiagCleanupLoop(const char* label, uint32_t count, uint32_t ptr) {
  static unsigned n = 0;
  if (n++ < 10) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[DIAG] %s: count=0x%08X (%u) ptr=0x%08X", label, count, count, ptr);
  }
}

void logSub827606E0DepthCap(int depthMax) {
  static unsigned n = 0;
  if (n++ < 1) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] sub_827606E0 depth cap hit (%d) – avoids 0xC00000FD at lr=0x82766C30", depthMax);
  }
}

void logSub827606E0StackGuardTriggered() {
  static unsigned n = 0;
  if (n++ < 1) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] sub_827606E0 stack guard triggered (host stack depth) – return 0 to avoid 0xC00000FD");
  }
}

void logSub827606E0AllocFailed() {
  static unsigned n = 0;
  if (n++ < 1) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] sub_827606E0: sub_82760928 returned 0 (alloc failed) – return 0 so insert_failed; fix alloc/heap for init2");
  }
}

void logSub827607B0ReturningFake(const char* reason) {
  static unsigned n = 0;
  if (n++ < 3) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] sub_827607B0 returning fake entry: %s", reason ? reason : "?");
  }
}

void logSub82164818FallbackHeapFailed(uint32_t requestedSize) {
  static unsigned n = 0;
  if (n++ < 1) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SPIN_DEBUG] sub_82164818 fallback guestHeapAlloc(size=0x%X) returned 0 – heap exhausted or failing", requestedSize);
  }
}

void logRtlUnwindCall(uint32_t caller_lr, uint32_t r3, uint32_t r4) {
  static unsigned n = 0;
  unsigned c = n++;
  if (c < 5 || c == 100 || c == 500 || c == 1000 || c == 5000 || c == 10000) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[RtlUnwind] #%u caller_lr=0x%08X r3=0x%08X r4=0x%08X", c + 1, caller_lr, r3, r4);
  }
}

// --- Dummy hash entry (zeroed 256 bytes in guest heap) ---
uint32_t getDummyHashEntry() {
  static uint32_t s_va = 0;
  if (s_va) return s_va;
  s_va = guestHeapAlloc(256);
  if (!s_va) s_va = guestHeapAllocFallback(256);
  if (s_va) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[HASH] allocated zeroed dummy hash entry at 0x%08X", s_va);
  }
  return s_va;
}

// --- ExCreateThread path diagnostics ---
void logSub82106010Entry(uint32_t r3, uint32_t r4) {
  static unsigned s_count = 0;
  if (s_count++ < 10)
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[EXTHREAD_PATH] sub_82106010 entered: r3=0x%08X r4=0x%08X", r3, r4);
}
void logSub82167F90Result(uint32_t result) {
  static unsigned s_count = 0;
  if (s_count++ < 10)
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[EXTHREAD_PATH] sub_82167F90 returned: r3=0x%08X (%s)", result, result ? "SUCCESS" : "FAILED - path to ExCreateThread blocked");
}
void logExThreadPathBranch(const char* tag, uint32_t val1, uint32_t val2) {
  static unsigned s_count = 0;
  if (s_count++ < 5000)
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[EXTHREAD_PATH] %s: val1=0x%08X val2=0x%08X", tag, val1, val2);
}
void logSub82106010Path(const char* tag, uint32_t val1, uint32_t val2) {
  static unsigned s_count = 0;
  if (s_count++ < 2000)
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[SUB82106010] %s: val1=0x%08X val2=0x%08X", tag, val1, val2);
}

void logInit2Returned(uint32_t r3) {
  static unsigned s_once = 0;
  if (s_once++ == 0) {
    // iter-361 determinism: dword_8296D878 (app-obj stash init2 sets to r3) is
    // 0x000005FF in bad rolls (golden has a real ptr) -> a scribble clobbers it,
    // and GameShutdown then hangs deref'ing the garbage. Report whether it is
    // already clobbered AT init2 return (scribble is DURING init2) or still real
    // (scribble is AFTER, during GameLoop/GameShutdown). SR_INIT2_PAUSE_SEC holds
    // here so a Frida write-watchpoint can be armed on 0x8296D878 to name the writer.
    const uint32_t stash = readGuestU32BE(0x8296D878u);
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[EXTHREAD_PATH] init2 (sub_8218CFE8) RETURNED r3=0x%08X dword_8296D878=0x%08X %s",
      r3, stash, (stash == r3) ? "(intact)" : "(CLOBBERED-during-init2)");
    const char* p = std::getenv("SR_INIT2_PAUSE_SEC");
    const int secs = p ? atoi(p) : 0;
    if (secs > 0) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[INIT2_PAUSE] holding %d s for watchpoint arm; dword_8296D878=0x%08X", secs, stash);
      for (int i = 0; i < secs * 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const uint32_t v = readGuestU32BE(0x8296D878u);
        if (v != stash) {
          SR_LOG_BOOT(sr::LogSeverity::Info,
            "[INIT2_PAUSE] dword_8296D878 CHANGED during pause 0x%08X -> 0x%08X (i=%d)", stash, v, i);
          break;
        }
      }
      SR_LOG_BOOT(sr::LogSeverity::Info, "[INIT2_PAUSE] releasing");
    }
  }
}

void logInit2Progress(const char* step) {
  static std::unordered_set<std::string> s_logged;
  if (s_logged.insert(step).second)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[INIT2_PROGRESS] %s", step);
}

void logModuleEvent(const char* msg) {
  SR_LOG_BOOT(sr::LogSeverity::Info, "%s", msg);
}

bool naturalModuleInitOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NATURAL_MODULE_INIT");
    return !e || e[0] != '0';
  }();
  return v;
}

// 2026-07-13 PRIME DIRECTIVE master gate (docs/NATURAL_PATH_CHARTER.md).
// The single source of truth for whether ANY forcing band-aid may run. Default
// OFF: the default build forces NOTHING — the game runs on its own. Forcing is
// opt-in as a NAMED DIAGNOSTIC only (SR_ALLOW_FORCING=1), to reveal the platform
// gap it compensates for, then be root-fixed and deleted. Every forcing path
// (fabricator, FE/screen drives, SetInterfaceState hand-drive, movieGate drive,
// APT logic drive, SM rebuild, name substitution, metric bumps) MUST consult
// this — no ungated forcing (the old SR_T2_KEEP blind spot let the fabricator
// escape). Platform fixes (recompiler/CS/TLS/GPU/kernel-imports) are NOT forcing
// and never gate on this.
// iter-455l r4 (director): NAMED FORCING ISOLATION. SR_FORCING_ISOLATE names exactly
// ONE forcing gate that stays authorizable while every ordinary forcingAllowed()
// caller is denied — the mechanism that makes a treatment run "control + one named
// gate" without maintaining a brittle per-knob zero list (the T1 acquisition failure:
// SR_ALLOW_FORCING=1 alone activated the legacy default-ON forcing stack).
// Semantics (pure functions so the self-test can drive the whole matrix):
//   master off                -> everything off.
//   master on, isolate UNSET  -> legacy behavior (ordinary and named both allowed).
//   master on, isolate set    -> ordinary callers DENIED; only the EXACT-MATCH named
//                                caller allowed; unknown/malformed/empty isolation
//                                values match nothing and thus fail closed.
bool isolationAllowsOrdinary(bool master, const char* iso) {
  if (!master) return false;
  return iso == nullptr;                    // any isolation value denies ordinary callers
}
bool isolationAllowsNamed(bool master, const char* iso, const char* gateName) {
  if (!master) return false;
  if (iso == nullptr) return true;          // legacy: named caller behaves like ordinary
  if (!gateName || !gateName[0]) return false;
  return std::strcmp(iso, gateName) == 0;   // exact match only — no prefixes, no case folding
}

namespace {
// nullptr = unset (legacy). A set-but-empty value is returned as "" and matches no
// gate name — deliberately FAIL CLOSED, not legacy.
const char* forcingIsolateRaw() {
  static const char* v = [] () -> const char* {
    const char* e = std::getenv("SR_FORCING_ISOLATE");
    return e ? e : nullptr;
  }();
  return v;
}
bool forcingMaster() {
  static const bool v = [] {
    const char* e = std::getenv("SR_ALLOW_FORCING");
    return e && e[0] == '1';   // default OFF
  }();
  return v;
}
}  // namespace

bool forcingAllowed() {
  return isolationAllowsOrdinary(forcingMaster(), forcingIsolateRaw());
}

bool forcingAllowedFor(const char* gateName) {
  return isolationAllowsNamed(forcingMaster(), forcingIsolateRaw(), gateName);
}

// --- FM Module Manager Singleton ---
// The FM module priority list lives at *(0x829A2BF0). Constructed lazily by
// sub_82720360 (calls sub_8213BB80 -> sub_8278D258 DOGMA allocator).
//
// On real Xbox 360 the singleton is constructed early (before Init2's module
// activation calls hit sub_82720590 / sub_8267E928 path, which dereferences it).
// In our build the construction site is missing because XenonRecomp lifted
// sub_82720360 but no caller path runs that touches it before Init2.
//
// The fix: invoke sub_82720360 ourselves at the same boot point the original
// title would have. This mirrors what the missing init thunk would do, so the
// resulting state is identical to a real-hardware boot — not a workaround.

constexpr uint32_t kFmManagerSingletonVA = 0x829A2BF0u;

uint32_t getFmManagerSingleton() {
  uint8_t* host = guestVAToHost(kFmManagerSingletonVA);
  if (!host) return 0;
  // Guest memory is big-endian.
  return (uint32_t(host[0]) << 24) | (uint32_t(host[1]) << 16) |
         (uint32_t(host[2]) << 8)  |  uint32_t(host[3]);
}

void logFmManagerSingleton(const char* tag) {
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[FM_MGR] %s: *0x%08X = 0x%08X", tag, kFmManagerSingletonVA, getFmManagerSingleton());
}

void logSplashTrace(const char* tag, uint32_t val1, uint32_t val2) {
  static unsigned s_count = 0;
  if (s_count++ < 200)
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[SPLASH] %s: val1=0x%08X val2=0x%08X", tag, val1, val2);
}

constexpr uint32_t kFmGamePlayStateMgrInstanceVA = 0x8296EDACu;

uint32_t getFmGamePlayStateMgrInstance() {
  // /loop iter-12: latch the first valid PSM pointer we see at *(0x8296EDAC).
  // Diagnostic from iter-10 showed the slot gets corrupted to 0xFFFFFFFF after
  // construction (something in the walker overwrites it). Once construction
  // logs PSM=0x889F0F48, that value is the truth — cache and reuse it so the
  // host fallback loop can keep ticking the splash advance even after the
  // global slot is clobbered.
  static std::atomic<uint32_t> s_cachedPsm{0};
  uint32_t cached = s_cachedPsm.load(std::memory_order_relaxed);

  uint8_t* host = guestVAToHost(kFmGamePlayStateMgrInstanceVA);
  if (!host) return cached;  // memory not mapped — return whatever we latched
  uint32_t v = (uint32_t(host[0]) << 24) | (uint32_t(host[1]) << 16) |
               (uint32_t(host[2]) << 8)  |  uint32_t(host[3]);

  // Treat 0 (not yet constructed) and 0xFFFFFFFF (corrupted) as "ignore"; trust
  // the latched value if we have one.
  if (v == 0u || v == 0xFFFFFFFFu) return cached;

  // First valid read latches the pointer. (iter-463 charter, burndown item 2:
  // the splash-filename-table "backfill" that used to run here is DELETED. It
  // checked first and logged "already correct NATURALLY" in 147/150 run logs,
  // "NOT natural" in 0/150, and never wrote: the loader gap it covered is gone.)
  if (cached == 0u) {
    s_cachedPsm.store(v, std::memory_order_relaxed);
  }
  return v;
}

uint32_t getFmGamePlayStateMgrSplashCounter() {
  uint32_t inst = getFmGamePlayStateMgrInstance();
  if (inst == 0) return 0xFFFFFFFFu;
  uint8_t* host = guestVAToHost(inst + 0x230);
  if (!host) return 0xFFFFFFFFu;
  return (uint32_t(host[0]) << 24) | (uint32_t(host[1]) << 16) |
         (uint32_t(host[2]) << 8)  |  uint32_t(host[3]);
}

uint8_t getFmGamePlayStateMgrSplashDone() {
  uint32_t inst = getFmGamePlayStateMgrInstance();
  if (inst == 0) return 0;
  uint8_t* host = guestVAToHost(inst + 0x234);
  if (!host) return 0;
  return *host;
}

void logFmCtorEvent(const char* event, const char* moduleName, uint32_t ctorAddr) {
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[FM_CTOR] %s %s ctor=0x%08X", event, moduleName, ctorAddr);
}

// --- Init3 game-state VA (observe only) ---
// sub_82176300 is the game's MAIN LOOP `while(*(gameState+0x18)==0) tick();` -
// +0x18 is its QUIT flag (iter-294). The iter-2x "init3 ready" bridge that
// wrote 1 there from VdSwap (signalInit3Ready, SR_RUN_REAL_GAMELOOP=0) was
// deleted in iter-463 (charter: it quit the game loop for the guest). The VA
// is still captured for the log; the declaration of signalInit3Ready in
// guest_context.hpp has no definition any more - do not call it.
static std::atomic<uint32_t> s_init3GameStateVA{0};

void setInit3GameStateVA(uint32_t gameStateGuestVA) {
  uint32_t expected = 0;
  if (s_init3GameStateVA.compare_exchange_strong(expected, gameStateGuestVA)) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[INIT3] game state VA=0x%08X, game-loop quit flag at VA=0x%08X (observe only)",
      gameStateGuestVA, gameStateGuestVA + 24);
  }
}


#if SR_ORACLE
// --- DOGMA guard1 write watchpoint ---
// Called from PPC_STORE_U32 macro when guest address == 0x829AAE78.
// Logs source file:line (identifies recomp function) and value written.
void dogmaGuardWatchpoint(uint32_t guestAddr, uint32_t value, const char* file, int line) {
  static std::atomic<unsigned> s_count{0};
  unsigned n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 10) {
    uint32_t lr = getCurrentGuestLR();
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[WATCHPOINT] DOGMA guard1 (0x%08X) written with value 0x%08X (%u) at %s:%d  guestLR=0x%08X  (hit #%u)",
      guestAddr, value, value, file, line, lr, n);
  }
}

// iter-292e: FM hash-set base ptr (module+4) corruption watch — see guest_context.hpp.
std::atomic<uint32_t> g_worldPtrFieldVA[4] = {};
std::atomic<bool> g_worldPtrWatchArmed{false};

void worldPtrFieldWatchpoint(uint32_t guestAddr, uint32_t value, const char* file, int line) {
  static std::atomic<unsigned> s_count{0};
  unsigned n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
  // A valid worldPtr / hash-base is a DOGMA-heap addr [0x88000000,0xBF000000);
  // anything else is the corrupting overflow store we're hunting.
  bool bad = (value < 0x88000000u || value >= 0xBF000000u);
  if (n <= 200 || bad) {
    uint32_t lr = getCurrentGuestLR();
    uint32_t liveLR = getCurrentGuestPC();
    uint32_t liveR1 = getCurrentGuestR1();
    uint32_t liveR31 = getCurrentGuestR31();
    SR_LOG_BOOT(bad ? sr::LogSeverity::Error : sr::LogSeverity::Info,
      "[WATCHPOINT] WORLDPTR write @0x%08X = 0x%08X%s at %s:%d guestLR=0x%08X liveLR=0x%08X r1=0x%08X r31=0x%08X (hit #%u)",
      guestAddr, value, bad ? " <<CORRUPT>>" : "", file, line, lr, liveLR, liveR1, liveR31, n);
  }
}

void moduleGlobalWatchpoint(uint32_t guestAddr, uint32_t value, const char* file, int line) {
  static std::atomic<unsigned> s_count{0};
  unsigned n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
  // iter-189: getCurrentGuestLR() is the last KERNEL-IMPORT lr (stale during pure
  // guest code); liveLR reads the writer thread's live PPCContext instead.
  if (n <= 200) {
    uint32_t lr = getCurrentGuestLR();
    uint32_t liveLR = getCurrentGuestPC();
    uint32_t liveR1 = getCurrentGuestR1();
    uint32_t liveR31 = getCurrentGuestR31();
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[WATCHPOINT] MODULE_GLOBAL write @0x%08X = 0x%08X (%u) at %s:%d guestLR=0x%08X liveLR=0x%08X r1=0x%08X r31=0x%08X (hit #%u)",
      guestAddr, value, value, file, line, lr, liveLR, liveR1, liveR31, n);
  }
  // iter-292e: arm the module+4 corruption-watch the moment a module ptr installs
  // into the globals array 0x8296D8D8/DC/E0/E4 (earliest possible — before the
  // corruption at ~16s). Env-gated so normal runs pay zero per-store cost.
  static const bool s_watchWP = [] {
    const char* e = std::getenv("SR_WATCH_WORLDPTR"); return e && e[0] == '1';
  }();
  if (s_watchWP) {
    for (int i = 0; i < 4; ++i) {
      if (guestAddr == 0x8296D8D8u + (uint32_t)i * 4u &&
          value >= 0x88000000u && value < 0xBF000000u) {
        g_worldPtrFieldVA[i].store(value + 4u, std::memory_order_relaxed);
        g_worldPtrWatchArmed.store(true, std::memory_order_relaxed);
        g_storeWatchOn = true;  // iter-461: the PPC_STORE checks run only when armed
      }
    }
  }
}

void vtableWatchpoint(uint32_t guestAddr, uint32_t value, const char* file, int line) {
  static std::atomic<unsigned> s_count{0};
  unsigned n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 30 || (n & (n - 1)) == 0) {
    uint32_t lr = getCurrentGuestLR();
    uint32_t liveLR = getCurrentGuestPC();
    uint32_t liveR1 = getCurrentGuestR1();
    uint32_t liveR31 = getCurrentGuestR31();
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[WATCHPOINT] VTABLE write @0x%08X = 0x%08X at %s:%d guestLR=0x%08X liveLR=0x%08X r1=0x%08X r31=0x%08X (hit #%u)",
      guestAddr, value, file, line, lr, liveLR, liveR1, liveR31, n);
  }
}

// Dynamic render-object watchpoint.
// g_renderObjVA is set lazily by the RSTATE diagnostic once the game render
// object address (0x8200094C → renderObjVA) is first observed.  Any store to
// that address is the destructor zeroing the vtable — the root cause of cmd43
// stopping at frame ~1073.
std::atomic<uint32_t> g_renderObjVA{0};

void renderObjDestructWatchpoint(uint32_t guestAddr, uint32_t value, const char* file, int line) {
  static std::atomic<unsigned> s_count{0};
  unsigned n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t lr = getCurrentGuestLR();
  SR_LOG_BOOT(sr::LogSeverity::Error,
    "[WATCHPOINT] RENDER_OBJ write @0x%08X = 0x%08X at %s:%d guestLR=0x%08X (hit #%u)",
    guestAddr, value, file, line, lr, n);
}

// iter-191: FMUI pool queue-field watchpoint (see guest_context.hpp).
std::atomic<uint32_t> g_fmuiPoolQueueVA{0};

// iter-192: Apt env-object watchpoint (see guest_context.hpp).
std::atomic<uint32_t> g_aptEnvVA{0};

// iter-197: Apt level node (loadMovie target) for state dumps.
std::atomic<uint32_t> g_aptLevelNodeVA{0};

// iter-198: package streamer instance (captured from the self-pumping
// whole-file load path at boot) — lets the fabricator drive the mounter.
std::atomic<uint32_t> g_streamerVA{0};

void fmuiQueueWatchpoint(uint32_t guestAddr, uint32_t value, const char* file, int line) {
  static std::atomic<unsigned> s_count{0};
  unsigned n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 100) {
    uint32_t lr = getCurrentGuestLR();
    uint32_t liveLR = getCurrentGuestPC();
    uint32_t liveR1 = getCurrentGuestR1();
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[WATCHPOINT] FMUI_QUEUE write @0x%08X = 0x%08X at %s:%d guestLR=0x%08X liveLR=0x%08X r1=0x%08X (hit #%u)",
      guestAddr, value, file, line, lr, liveLR, liveR1, n);
  }
}

#else
// Shared generated-guest ABI; all release watch flags stay disarmed.
std::atomic<uint32_t> g_worldPtrFieldVA[4] = {};
std::atomic<bool> g_worldPtrWatchArmed{false};
std::atomic<uint32_t> g_renderObjVA{0};
std::atomic<uint32_t> g_fmuiPoolQueueVA{0};
std::atomic<uint32_t> g_aptEnvVA{0};
void dogmaGuardWatchpoint(uint32_t, uint32_t, const char*, int) {}
void worldPtrFieldWatchpoint(uint32_t, uint32_t, const char*, int) {}
void moduleGlobalWatchpoint(uint32_t, uint32_t, const char*, int) {}
void vtableWatchpoint(uint32_t, uint32_t, const char*, int) {}
void renderObjDestructWatchpoint(uint32_t, uint32_t, const char*, int) {}
void fmuiQueueWatchpoint(uint32_t, uint32_t, const char*, int) {}
#endif

namespace {
  ExCreateThreadRunnerFn g_exCreateThreadRunner = nullptr;
  uint8_t* g_exCreateThreadBase = nullptr;
  size_t g_exCreateThreadSize = 0;
}

void registerExCreateThreadRunner(ExCreateThreadRunnerFn fn, uint8_t* base, size_t size) {
  g_exCreateThreadRunner = fn;
  g_exCreateThreadBase = base;
  g_exCreateThreadSize = size;
}

// Called by stub when ExCreateThread is invoked; returns runner/base/size if registered.
ExCreateThreadRunnerFn getExCreateThreadRunner(uint8_t** outBase, size_t* outSize) {
  *outBase = g_exCreateThreadBase;
  *outSize = g_exCreateThreadSize;
  return g_exCreateThreadRunner;
}

bool isGuestMemoryRegistered() {
  return g_exCreateThreadBase != nullptr && g_exCreateThreadSize != 0;
}

void getGlobalGuestMemory(uint8_t** outBase, size_t* outSize) {
  *outBase = g_exCreateThreadBase;
  *outSize = g_exCreateThreadSize;
}

namespace {
  thread_local void* g_guestThreadStackAnchor = nullptr;
  thread_local uint32_t g_currentThreadHandle = 0;
  thread_local uint32_t g_threadStartR4 = 0;   // step4/audit SR_XAPI_THREAD_STARTUP
}

void setCurrentThreadHandle(uint32_t handle) {
  g_currentThreadHandle = handle;
}

void setThreadStartR4(uint32_t r4) { g_threadStartR4 = r4; }
uint32_t takeThreadStartR4() {
  const uint32_t v = g_threadStartR4;
  g_threadStartR4 = 0;
  return v;
}

uint32_t getCurrentThreadHandle() {
  return g_currentThreadHandle;
}

void setGuestThreadStackAnchorForOverflowCheck(void* anchor) {
  g_guestThreadStackAnchor = anchor;
}

void* getGuestThreadStackAnchorForOverflowCheck() {
  return g_guestThreadStackAnchor;
}

bool isGuestThreadStackOverflowGuardTriggered() {
  if (!g_guestThreadStackAnchor) return false;
  volatile char here;
  // 256 MB – avoid main-thread 0xC00000FD after long run (~90s); ExCreateThread threads get 256 MB host stack so limit fits.
  constexpr size_t kMaxStackDepthBytes = 256u * 1024u * 1024u;
  return static_cast<size_t>(reinterpret_cast<uintptr_t>(g_guestThreadStackAnchor) - reinterpret_cast<uintptr_t>(&here)) > kMaxStackDepthBytes;
}

bool isGuestThreadNearStackLimit() {
  if (!g_guestThreadStackAnchor) return false;
  volatile char here;
  // 128 MB early guard – half the 256 MB thread budget; triggers before OS kills the thread.
  constexpr size_t kNearLimitBytes = 128u * 1024u * 1024u;
  return static_cast<size_t>(reinterpret_cast<uintptr_t>(g_guestThreadStackAnchor) - reinterpret_cast<uintptr_t>(&here)) > kNearLimitBytes;
}

uint32_t getGfxCommandBufferVA() {
  return g_gfxBufferVA;
}

#if SR_ORACLE
namespace {
  struct GuestFramebufferInfo {
    uint32_t va = 0;
    int width = 0;
    int height = 0;
    int pitchBytes = 0;
    bool tiled = false;  // iter-358j: Xenos 32x32-tiled surface (needs untile on CPU read)
  };
  GuestFramebufferInfo g_fbInfo;
  std::mutex g_fbMutex;
}

void setGuestFramebufferVA(uint32_t va, int width, int height, int pitchBytes, bool tiled) {
  std::lock_guard<std::mutex> lock(g_fbMutex);
  g_fbInfo.va = va;
  g_fbInfo.width = width;
  g_fbInfo.height = height;
  g_fbInfo.pitchBytes = pitchBytes;
  g_fbInfo.tiled = tiled;
}

bool getGuestFramebufferInfo(uint32_t* outVA, int* outWidth, int* outHeight, int* outPitchBytes,
                             bool* outTiled) {
  if (!outVA || !outWidth || !outHeight || !outPitchBytes) return false;
  std::lock_guard<std::mutex> lock(g_fbMutex);
  if (g_fbInfo.va == 0) return false;
  *outVA = g_fbInfo.va;
  *outWidth = g_fbInfo.width;
  *outHeight = g_fbInfo.height;
  *outPitchBytes = g_fbInfo.pitchBytes;
  if (outTiled) *outTiled = g_fbInfo.tiled;
  return true;
}

// iter-436h: frame-boundary snapshot (see hpp). Double-buffered so the
// present-side reader never sees a half-copied frame: writes go to the
// non-published slot, then the index flips atomically.
namespace {
  struct FbSnapshot {
    std::vector<uint8_t> buf;
    int width = 0, height = 0, pitchBytes = 0;
  };
  FbSnapshot g_fbSnap[2];
  std::atomic<uint32_t> g_fbSnapIdx{0};   // published slot
  std::atomic<uint64_t> g_fbSnapSeq{0};
  std::mutex g_fbSnapWriteMutex;          // one writer at a time (VdSwap thread)
}

void snapshotGuestFramebuffer(uint32_t va, int width, int height, int pitchBytes) {
  if (!va || width <= 0 || height <= 0 || pitchBytes < width * 4) return;
  uint8_t* gb = nullptr; size_t gsz = 0;
  getGlobalGuestMemory(&gb, &gsz);
  if (!gb || !gsz) return;
  const uint8_t* src = guestVAToHost(va);
  const size_t span = size_t(height - 1) * size_t(pitchBytes) + size_t(width) * 4u;
  if (!src || src < gb || src + span > gb + gsz) return;
  std::lock_guard<std::mutex> lk(g_fbSnapWriteMutex);
  FbSnapshot& s = g_fbSnap[g_fbSnapIdx.load(std::memory_order_relaxed) ^ 1u];
  s.buf.resize(span);
  std::memcpy(s.buf.data(), src, span);
  s.width = width;
  s.height = height;
  s.pitchBytes = pitchBytes;
  g_fbSnapIdx.fetch_xor(1u, std::memory_order_release);
  g_fbSnapSeq.fetch_add(1, std::memory_order_release);
}

const uint8_t* getGuestFramebufferSnapshot(int* outWidth, int* outHeight,
                                           int* outPitchBytes, uint64_t* seqOut) {
  const uint64_t seq = g_fbSnapSeq.load(std::memory_order_acquire);
  if (!seq) return nullptr;
  const FbSnapshot& s = g_fbSnap[g_fbSnapIdx.load(std::memory_order_acquire)];
  if (s.buf.empty()) return nullptr;
  if (outWidth) *outWidth = s.width;
  if (outHeight) *outHeight = s.height;
  if (outPitchBytes) *outPitchBytes = s.pitchBytes;
  if (seqOut) *seqOut = seq;
  return s.buf.data();
}

#endif  // SR_ORACLE: guest-memory framebuffer hand-off and snapshot

namespace {
  std::atomic<uint64_t> g_vdSwapCount{0};
  std::atomic<uint32_t> g_testFrontBufferVA{0};
}

uint64_t getVdSwapCount() {
  return g_vdSwapCount.load(std::memory_order_relaxed);
}

void incrementVdSwapCount() {
  g_vdSwapCount.fetch_add(1, std::memory_order_relaxed);
}

uint32_t getTestFrontBufferVA() {
  return g_testFrontBufferVA.load(std::memory_order_relaxed);
}

// iter-358f: setupFakeGpuDevice() DELETED — it fabricated a D3D device + test
// front buffer around the misidentified KeCertMonitorData import slot (0x8200094C).
// See docs/NATURAL_BASELINE.md 2026-07-17.
void initKernelDataStructures() {
  if (!g_guestBase || g_guestSize == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "initKernelDataStructures: guest memory not set");
    return;
  }

  // iter-463 charter: the host no longer writes 1 to 0x829AAE78 / 0x829AAE74 here.
  // They are NOT "DOGMA guard globals": IDA shows sub_827A1698 is the CRT doexit()
  // and these are its _C_Termination_Done (0x829AAE78) / _C_Exit_Done (0x829AAE74)
  // flags - their only xrefs are in that function, and the golden dump reads 0.
  // The write was inert (the spliced goto loc_827A1770 in sub_827A1690/1698 never
  // reads them) and would have faked "termination already done" for the guest.

  // --- iter-390d PLATFORM FIX: VdGlobalDevice data import (SR_VD_GLOBAL_DEVICE,
  // default ON; =0 reverts) ---
  // Same class as KeTimeStampBundle below (iter-303): the XEX imports
  // VdGlobalDevice as a DATA import at cell 0x82000948 and we left the raw
  // token bytes there. On console the loader resolves the cell to a kernel
  // variable; the title STORES its D3D device pointer through it, and every
  // GPU fence check reads it back (D3DDeviceIsFencePending sub_820FDD90
  // (iter-440: returns TRUE = pending, not passed):
  // dev = *(*(0x82000948)), then +0x2A1C submit vs *(+0x2A10) completed).
  // With the token in place, *(cell) = 0xBE010100 = MAIN-THREAD STACK: the
  // game stored its device pointer into accidental stack memory. It reads
  // back fine until a deep post-flow call chain clobbers that slot — then
  // every fence wait spins forever. That is the mode-B FE freeze (iter-390
  // live catch: thread 49 wedged in fence reaper sub_82616208 inside FE tick
  // #121 while gate/clock stayed perfect; roll-dependence = whatever garbage
  // lands at 0xBE010100).
  {
    static const bool s_vdGlobalOn = [] {
      const char* e = std::getenv("SR_VD_GLOBAL_DEVICE");
      return !e || e[0] != '0';
    }();
    static bool s_vdGlobalDone = false;
    constexpr uint32_t kVdGlobalVarVA = 0xD0013000u;  // reserved fake-device region
    constexpr uint32_t kVdGlobalCell  = 0x82000948u;
    if (s_vdGlobalOn && !s_vdGlobalDone && kVdGlobalVarVA + 0x10 <= g_guestSize) {
      s_vdGlobalDone = true;
      std::memset(g_guestBase + kVdGlobalVarVA, 0, 0x10);
      writeGuestBE32(g_guestBase + kVdGlobalCell, kVdGlobalVarVA);
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "  [VD_GLOBAL_DEVICE] cell 0x%08X -> var 0x%08X (device ptr slot, zero-init)",
        kVdGlobalCell, kVdGlobalVarVA);
    } else if (!s_vdGlobalOn) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "  [VD_GLOBAL_DEVICE] OFF (raw import token stays; device ptr aliases stack)");
    }
  }

  // --- iter-391 PLATFORM FIX: resolve the REMAINING data imports
  // (SR_DATA_IMPORTS, default ON; =0 reverts) ---
  // The XEX imports exactly TEN xboxkrnl variables as DATA imports. Two are
  // resolved above/below (VdGlobalDevice iter-390, KeTimeStampBundle
  // iter-303) — both were multi-week walls caused by the guest reading or
  // writing THROUGH a raw token into pseudo-random VA space. This block
  // resolves seven more per the golden-dump contract (values read from the
  // live Xenia menu). The most dangerous pair: KeDebugMonitorData and
  // KeCertMonitorData are checked by D3DDevicePresent, GpuDriverIssueCommand
  // and the PIX capture machine — console value is NULL (no monitor); a
  // token that lands byte-swapped (the VdGlobalDevice failure shape) makes
  // the whole D3D layer think a debug/cert monitor is attached.
  // XexExecutableModuleHandle is resolved further down (iter-463: its own
  // block, variable 0xD001C000 -> hmodule 0xD001C100). Its history: the
  // honest fix (RtlImageXexHeaderField key 0x20401 must report
  // FIELD ABSENT so the CRT builds its default 1MB process heap; our stub's
  // pointer-to-zero makes the CRT SKIP the heap!) is iter-392, gated
  // separately because it changes boot allocator geometry.
  {
    static const bool s_dataImportsOn = [] {
      const char* e = std::getenv("SR_DATA_IMPORTS");
      return !e || e[0] != '0';
    }();
    static bool s_dataImportsDone = false;
    if (s_dataImportsOn && !s_dataImportsDone && 0xD001B010u <= g_guestSize) {
      s_dataImportsDone = true;
      struct Slot { uint32_t cell, var; const char* name; };
      static constexpr Slot kSlots[] = {
        { 0x82000770u, 0xD0015000u, "ExLoadedCommandLine" },
        { 0x82000780u, 0xD0016000u, "ExThreadObjectType" },
        { 0x82000814u, 0xD0017000u, "VdHSIOCalibrationLock" },
        { 0x82000844u, 0xD0018000u, "VdGpuClockInMHz" },
        { 0x8200084Cu, 0xD0019000u, "XboxKrnlVersion" },
        { 0x82000900u, 0xD001A000u, "KeDebugMonitorData" },
        { 0x8200094Cu, 0xD001B000u, "KeCertMonitorData" },
      };
      for (const Slot& s : kSlots) {
        std::memset(g_guestBase + s.var, 0, 0x10);
        writeGuestBE32(g_guestBase + s.cell, s.var);
      }
      // Non-zero contents (golden sound_menu, Xenia source cited per line):
      std::memcpy(g_guestBase + 0xD0015000u, "\"default.xex\"", 14);   // ExLoadedCommandLine
      // iter-463 charter (burndown item 5): VdGpuClockInMHz is 500 (Xenos
      // clock), stored big-endian: Xenia xboxkrnl_video.cc:577-582 and the
      // golden dump (0x82000844 -> 0x801D0E94 = 00 00 01 F4). The old bytes
      // 01 28 00 00 were VdHSIOCalibrationLock's critical-section header
      // (golden 0x801D1210), copied into the wrong variable.
      static const uint8_t kGpuClk[4] = { 0x00, 0x00, 0x01, 0xF4 };    // VdGpuClockInMHz = 500
      std::memcpy(g_guestBase + 0xD0018000u, kGpuClk, 4);
      // VdHSIOCalibrationLock is a critical section, initialized by the kernel:
      // Xenia xeRtlInitializeCriticalSectionAndSpinCount(lock, 10000)
      // (xboxkrnl_video.cc:585-591, xboxkrnl_rtl.cc:545-572) = type 1
      // (EventSynchronizationObject), spin count div 256 = 0x28, lock_count
      // -1, recursion 0, owner 0 - byte-identical to golden 0x801D1210. Its
      // reader is GpuRetrainEDRAMLoop_sub_82110058.
      static const uint8_t kHsioLock[28] = {
        0x01, 0x28, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0 };
      std::memcpy(g_guestBase + 0xD0017000u, kHsioLock, sizeof(kHsioLock));
      static const uint8_t kKrnlVer[8] = { 0x00, 0x02, 0x00, 0x00, 0x07, 0x60, 0x00, 0x00 };
      std::memcpy(g_guestBase + 0xD0019000u, kKrnlVer, 8);             // 2.0.1888.0
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "  [DATA_IMPORTS] 7 remaining data-import cells resolved (0xD0015000..0xD001B000); "
        "KeDebugMonitorData/KeCertMonitorData = NULL (golden contract)");
    } else if (!s_dataImportsOn) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "  [DATA_IMPORTS] OFF (7 cells keep raw import tokens)");
    }
  }

  // --- iter-303: real KeTimeStampBundle (gated SR_KE_TIMESTAMP) ---
  // The XEX imports KeTimeStampBundle as a DATA import at cell 0x820007D0; we
  // historically left the raw import token (0x000100AD) there, so the game's
  // universal ms clock FUN_824680c0 = *(*(0x820007D0)+0x10) read garbage at
  // guest VA 0x100BD. Consequence (root-caused iter-303): time never advances,
  // GameKernelLoop_Run_sub_82720B50's tick accumulator never reaches its period,
  // and all four FGGame* service loops yield-spin forever post-init (the
  // 100M+/run RtlTryEnterCriticalSection storm). Fix: point the cell at a real
  // bundle in our reserved guest region and keep its fields ticking (BE) from a
  // dedicated wall-clock thread. Layout (Xenia KeTimeStampBundle):
  //   +0x00 u64 interruptTime (100ns since boot)
  //   +0x08 u64 systemTime    (100ns since 1601, FILETIME)
  //   +0x10 u32 milliseconds  (uptime ms — THE game clock)
  // iter-463 SR_TIMEBASE_50MHZ (default ON): all three fields come from ONE 50 MHz
  // guest tick read (sr::timebase::writeBundle: interrupt = ticks/5, system = base +
  // ticks/5, ms = ticks/50000 - Xenia's QueryGuestUptimeMillis), refreshed on a 1 ms
  // high-resolution waitable timer (Xenia: 1 ms HighResolutionTimer). Legacy (=0):
  // QPC-double interrupt time from the ticker's own origin, host FILETIME, and
  // sleep_for(1ms), which rounds up to the process timer quantum ([KE_TS_PERIOD]
  // below measures it). NOTE: the GameKernelLoop pacing clock IS this field with either
  // gate - sub_824680C0 runs its original body (r3 = *(*(0x820007D0)+0x10)) since
  // 156b6f47 deleted the GuestClockMs splice (docs/SPLICE_CONVERSION.md: DEAD -> DELETE);
  // on a legacy run the legacy writer supplies the ms (difftest --timebase-test G3/G4).
  {
    // Proven 4/4 (iter-303 K-run): storm gone, CV waits really block, RTAM
    // scheduler works every roll. Baked default-ON; SR_KE_TIMESTAMP=0 = A/B.
    static const bool s_keTsOn = [] {
      const char* e = std::getenv("SR_KE_TIMESTAMP");
      return !e || e[0] != '0';
    }();
    static bool s_keTsDone = false;
    constexpr uint32_t kBundleVA  = 0xD0012000u;   // in the 0xD00xxxxx fake-device region
    constexpr uint32_t kBundleCell = 0x820007D0u;  // XEX import cell for KeTimeStampBundle
    if (s_keTsOn && !s_keTsDone && kBundleVA + 0x40 <= g_guestSize) {
      s_keTsDone = true;
      uint8_t* bundle = g_guestBase + kBundleVA;
      std::memset(bundle, 0, 0x40);
      auto writeBE64 = [](uint8_t* p, uint64_t v) {
        for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (56 - i * 8));
      };
      auto tick = [bundle, writeBE64] {
        if (sr::timebase::enabled()) {
          sr::timebase::writeBundle(bundle);
          return;
        }
        LARGE_INTEGER qpcFreq, qpc;
        QueryPerformanceFrequency(&qpcFreq);
        QueryPerformanceCounter(&qpc);
        static const int64_t s_qpc0 = qpc.QuadPart;
        uint64_t interrupt100ns = static_cast<uint64_t>(
            (static_cast<double>(qpc.QuadPart - s_qpc0) * 1e7) / static_cast<double>(qpcFreq.QuadPart));
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        uint64_t sys100ns = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        writeBE64(bundle + 0x00, interrupt100ns);
        writeBE64(bundle + 0x08, sys100ns);
        writeGuestBE32(bundle + 0x10, static_cast<uint32_t>(interrupt100ns / 10000u));
      };
      tick();  // fields valid before the pointer is published
      writeGuestBE32(g_guestBase + kBundleCell, kBundleVA);
      static_assert(kBundleVA == sr::timebase::kBundleVA, "one bundle VA");
      sr::timebase::setBundlePublished(true);
      std::thread([tick, bundle, gb = g_guestBase, gs = g_guestSize] {
        // iter-463: g_guestBase is THREAD-LOCAL. Without this the detector
        // reads below (readGuestU32BE) saw base == null -> 0 and logged a
        // false "[KE_TS_FOREIGN] cell 0x820007D0 REPOINTED to 00000000" and
        // "[VD_CELL_WATCH] ... read 00000000" 8x per run (seen in every
        // iter-463 smoke roll, gate on and off; the cells were not touched).
        // The bundle writes themselves use the host pointer and were right.
        setGuestMemory(gb, gs);
        // iter-390: two-writer detector for the mode-B FE freeze. The four
        // GameKernelLoop TimeGates share this one clock read (cell 0x820007D0
        // -> bundle+0x10); the frozen catch showed accum DECREASING between
        // samples, which needs negative now-prev deltas — i.e. a second
        // writer ping-ponging the ms field against ours. We are the only
        // intended writer, every 1ms: any read-back that differs from what WE
        // last wrote names a foreign writer (value + when). Also alarm if the
        // published cell itself gets repointed.
        uint32_t lastWroteMs = 0;
        auto rdBE32 = [](const uint8_t* p) -> uint32_t {
          return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                 (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        };
        bool first = true;
        static std::atomic<uint32_t> s_foreign{0};
        // iter-463: 1 ms period source. Gate on: high-resolution waitable timer
        // (per-timer; does not change the process quantum). Gate off: legacy sleep.
        const bool hrPeriod = sr::timebase::enabled();
        sr::timebase::PeriodicMs period;
        // [KE_TS_PERIOD] one-shot measurement of the ticker's real period over its
        // first 2000 iterations (observe-only; answers "is sleep_for(1ms) 1 ms here?").
        LARGE_INTEGER pf, pPrev;
        QueryPerformanceFrequency(&pf);
        QueryPerformanceCounter(&pPrev);
        const LARGE_INTEGER pStart = pPrev;
        double pMaxMs = 0.0;
        uint32_t pN = 0;
        bool pLogged = false;
        for (;;) {
          if (!first) {
            const uint32_t seen = rdBE32(bundle + 0x10);
            if (seen != lastWroteMs) {
              const uint32_t n = s_foreign.fetch_add(1, std::memory_order_relaxed) + 1;
              if (n <= 16 || (n & (n - 1)) == 0)
                SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[KE_TS_FOREIGN] #%u bundle+0x10 = %u but we last wrote %u "
                  "— SECOND WRITER on the game clock",
                  n, seen, lastWroteMs);
            }
            const uint32_t cell = readGuestU32BE(0x820007D0u);
            if (cell != 0xD0012000u) {
              static std::atomic<uint32_t> s_repoint{0};
              const uint32_t m = s_repoint.fetch_add(1, std::memory_order_relaxed) + 1;
              if (m <= 8)
                SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[KE_TS_FOREIGN] #%u cell 0x820007D0 REPOINTED to %08X", m, cell);
              // iter-390e: NO restore write from this thread (it raced the
              // watchdog's PAGE_READONLY window; VEH-fatal AV). iter-463: the
              // periodic 8MB image restore that used to re-write these cells
              // is master-gated off (forcing), so on a default run NOTHING
              // repairs a re-point - this line is pure detection. A repoint
              // is a guest write into the XEX import cells: find the writer
              // (SR_IMAGE_RO=1), do not restore over it.
            }
            // iter-390d/e: VdGlobalDevice cell watch (0x82000948 -> 0xD0013000),
            // log-only for the same reason. The VAR at 0xD0013000 is game-owned.
            {
              static const bool s_vdOn = [] {
                const char* e = std::getenv("SR_VD_GLOBAL_DEVICE");
                return !e || e[0] != '0';
              }();
              const uint32_t vdCell = s_vdOn ? readGuestU32BE(0x82000948u) : 0xD0013000u;
              if (vdCell != 0xD0013000u) {
                static std::atomic<uint32_t> s_vdRepoint{0};
                const uint32_t m2 = s_vdRepoint.fetch_add(1, std::memory_order_relaxed) + 1;
                if (m2 <= 8)
                  SR_LOG_BOOT(sr::LogSeverity::Error,
                    "[VD_CELL_WATCH] #%u cell 0x82000948 read %08X (no restorer on a default run)",
                    m2, vdCell);
              }
            }
          }
          tick();
          lastWroteMs = rdBE32(bundle + 0x10);
          first = false;
          if (hrPeriod)
            period.wait();
          else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          if (!pLogged) {
            LARGE_INTEGER pNow;
            QueryPerformanceCounter(&pNow);
            const double dt = double(pNow.QuadPart - pPrev.QuadPart) * 1000.0 / double(pf.QuadPart);
            if (dt > pMaxMs) pMaxMs = dt;
            pPrev = pNow;
            if (++pN == 2000) {
              pLogged = true;
              const double mean = double(pNow.QuadPart - pStart.QuadPart) * 1000.0 /
                                  double(pf.QuadPart) / double(pN);
              SR_LOG_BOOT(sr::LogSeverity::Info,
                "[KE_TS_PERIOD] bundle ticker: %u periods mean=%.3f ms max=%.3f ms source=%s tb50=%d",
                pN, mean, pMaxMs,
                hrPeriod ? (period.highResolution() ? "hr-waitable-timer" : "sleep_for(fallback)")
                         : "sleep_for(1ms)",
                sr::timebase::enabled() ? 1 : 0);
            }
          }
        }
      }).detach();
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "  [KE_TIMESTAMP] real KeTimeStampBundle live at 0x%08X (*0x%08X patched; ms=%u)",
        kBundleVA, kBundleCell, readGuestU32BE(kBundleVA + 0x10));
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "  [TIMEBASE] SR_TIMEBASE_50MHZ=%d: mftb/QPC/bundle %s (qpc->guest scale %llu/%llu)",
        sr::timebase::enabled() ? 1 : 0,
        sr::timebase::enabled() ? "= ONE 50 MHz guest clock" : "= legacy (raw TSC / steady / FILETIME)",
        (unsigned long long)sr::timebase::scaleNum(), (unsigned long long)sr::timebase::scaleDen());
    } else if (!s_keTsOn) {
      static bool s_offLogged = false;
      if (!s_offLogged) {
        s_offLogged = true;
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "  [KE_TIMESTAMP] OFF (import-token sentinel at 0x820007D0; game clock frozen) — SR_KE_TIMESTAMP=1 enables");
      }
    }
  }

  // iter-463 charter (burndown round 2, item 1): the "allocator vtable at
  // 0x8249AAC8" slot patch is DELETED. 0x8249AAC8 is CODE in all three golden
  // dumps (39 0B 00 10 89 2B 00 00 = addi r8,r11,16; lbz r9,0(r11)), so both
  // "slots" are always non-zero and the patch could never fire: "Allocator
  // vtable 0x" is in 0/150 run logs (81 before 60a90bf, 69 after). Its stub
  // (PPC_allocator_slot0_stub / kAllocatorSlot0StubVA 0x82100400, inside .text)
  // and the dispatch branch that hijacked any real call to 0x82100400 went with it.

  // The game accesses kernel-like data at the image base (0x82000000).
  // Specifically, offsets 0x768 and 0x900 contain pointers that are dereferenced.
  // After the XEX image is loaded, we check if these pointers need initialization.
  //
  // If the XEX image already has valid data at these offsets, we don't overwrite it.
  // If the data is zero (uninitialized), we set up minimal stub structures.
  
  if (kKernelDataBase >= g_guestSize) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "initKernelDataStructures: kernel data base 0x%08X outside allocation (size 0x%zX)",
                kKernelDataBase, g_guestSize);
    return;
  }
  
  uint8_t* kernelData = g_guestBase + kKernelDataBase;
  
  // Helper to read big-endian 32-bit value
  auto readGuestBE32 = [](const uint8_t* p) -> uint32_t {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
  };
  
  // Check current values at key offsets
  uint32_t currentModulePtr = readGuestBE32(kernelData + kKernelModuleInfoPtrOffset);
  uint32_t currentProcessPtr = readGuestBE32(kernelData + kKernelProcessInfoPtrOffset);
  
  SR_LOG_BOOT(sr::LogSeverity::Info, "initKernelDataStructures: checking kernel data at 0x%08X", kKernelDataBase);
  SR_LOG_BOOT(sr::LogSeverity::Info, "  Current module ptr at 0x%08X = 0x%08X", 
              kKernelDataBase + kKernelModuleInfoPtrOffset, currentModulePtr);
  SR_LOG_BOOT(sr::LogSeverity::Info, "  Current process ptr at 0x%08X = 0x%08X",
              kKernelDataBase + kKernelProcessInfoPtrOffset, currentProcessPtr);
  
  // iter-463 charter (burndown item 3): the "process info" block that used to
  // follow is DELETED. 0x82000900 is the KeDebugMonitorData DATA-import cell;
  // SR_DATA_IMPORTS (above) already resolves it to its kernel variable
  // 0xD001A000 (value NULL = no debug monitor, the golden contract: Xenia
  // 0x80207A64 -> 0). This block then read 0xD001A000 as "not a valid
  // pointer" (>= 0x90000000) and OVERWROTE the cell with 0x82100100, a zeroed
  // struct inside .text, on every boot ("Initialized process info ptr" in
  // 149/150 run logs). The early "already initialized" return it fed could
  // never fire (processNeedsInit was always true) and is gone with it.

  // --- XexExecutableModuleHandle (iter-463 charter, burndown item 6) ---
  // A DATA import like the others above: cell 0x82000768 (raw token 0x93010100
  // at load) -> a kernel VARIABLE that holds the executable's hmodule, a
  // X_LDR_DATA_TABLE_ENTRY. Xenia: xboxkrnl_module.cc:161-167 (variable
  // 0x802080D4) + kernel_state.cc:489-497 (*variable = hmodule_ptr);
  // xmodule.h:27-53 is the entry layout. The one reader is the CRT heap init
  // XapiInitProcessHeap_sub_8246A2A8: r11 = *(*(0x82000768)); if 0 skip;
  // r3 = *(r11 + 0x58) (xex_header_base) -> RtlImageXexHeaderField(r3, 0x20401).
  // This used to live at 0x82100000..0x821002FF, which is .text in all three
  // golden dumps (a zeroed struct written over code bytes); it now sits in the
  // 0xD00xxxxx kernel-variable region with the other data imports, and the
  // hmodule carries the fields Xenia's LDR entry has (golden sound_menu:
  // 0x82000768 -> 0x802080D4 -> hmodule 0x30013000: image_base 0x82000000,
  // full_image_size 0x009E0000, entry_point 0x82468D10, time_date_stamp
  // 0x452D391E, xex_header_base 0x30014000). The xex-header block is zeroed:
  // RtlImageXexHeaderField answers the one lookup (key 0x20401) itself
  // (SR_XEX_HEAP_FIELD); its legacy =0 arm still returns this block.
  {
    constexpr uint32_t kXexModuleCell   = kKernelDataBase + kKernelModuleInfoPtrOffset;  // 0x82000768
    constexpr uint32_t kXexModuleVarVA  = 0xD001C000u;  // the kernel variable
    constexpr uint32_t kXexHModuleVA    = 0xD001C100u;  // X_LDR_DATA_TABLE_ENTRY
    constexpr uint32_t kXexHeaderStubVA = 0xD001D000u;  // zeroed xex header (stubs.cpp RtlImageXexHeaderField legacy arm)
    if (kXexHeaderStubVA + 0x100 <= g_guestSize) {
      std::memset(g_guestBase + kXexModuleVarVA, 0, 0x200);
      std::memset(g_guestBase + kXexHeaderStubVA, 0, 0x100);
      uint8_t* hmod = g_guestBase + kXexHModuleVA;
      writeGuestBE32(hmod + 0x1C, 0x82000000u);           // image_base
      writeGuestBE32(hmod + 0x38, 0x009E0000u);           // full_image_size
      writeGuestBE32(hmod + 0x3C, 0x82468D10u);           // entry_point
      writeGuestBE32(hmod + 0x50, 0x452D391Eu);           // time_date_stamp
      writeGuestBE32(hmod + 0x58, kXexHeaderStubVA);      // xex_header_base
      writeGuestBE32(g_guestBase + kXexModuleVarVA, kXexHModuleVA);
      writeGuestBE32(g_guestBase + kXexModuleCell, kXexModuleVarVA);
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "  Initialized module info ptr -> 0x%08X (XexExecutableModuleHandle: cell 0x%08X -> "
        "var 0x%08X -> hmodule 0x%08X, xex_header_base 0x%08X; was token 0x%08X)",
        kXexModuleVarVA, kXexModuleCell, kXexModuleVarVA, kXexHModuleVA, kXexHeaderStubVA,
        currentModulePtr);
    }
  }

#if SR_ORACLE
  // --- Initialize default heap handle ---
  // The game's malloc looks up the heap handle from global variables.
  // sub_8246DF08 (ppc_recomp.57): lis r11,-32101 → r11=0x829B0000; lwz r3,-16020(r11) → load from 0x829AC16C.
  // Other paths: 0x82A2C16C, 0x8296C16C (alternates), 0x829BE4CC (sub_8246CA78 SDA2-6964).
  constexpr uint32_t kHeapHandleSub8246DF08VA = 0x829AC16Cu; // sub_8246DF08 actual load (fixes imageoperate bugcheck)
  constexpr uint32_t kHeapHandlePtrPath1VA = 0x82A2C16Cu;
  constexpr uint32_t kHeapHandlePtrVA = 0x8296C16Cu;
  constexpr uint32_t kHeapHandlePtr2VA = 0x829BE4CCu;
  constexpr uint32_t kHeapStateVA = 0x82110000u;          // Our stub heap state structure

  if (kHeapHandleSub8246DF08VA < g_guestSize && kHeapStateVA + 0x2000 < g_guestSize) {
    uint32_t currentSub = readGuestBE32(g_guestBase + kHeapHandleSub8246DF08VA);
    uint32_t current1 = (kHeapHandlePtrPath1VA < g_guestSize) ? readGuestBE32(g_guestBase + kHeapHandlePtrPath1VA) : 0xFFFFFFFFu;
    uint32_t current2 = (kHeapHandlePtrVA < g_guestSize) ? readGuestBE32(g_guestBase + kHeapHandlePtrVA) : 0xFFFFFFFFu;
    uint32_t current3 = (kHeapHandlePtr2VA < g_guestSize) ? readGuestBE32(g_guestBase + kHeapHandlePtr2VA) : 0xFFFFFFFFu;
    bool needsInit = (currentSub == 0 || current1 == 0 || current2 == 0 || current3 == 0);
    // iter-360j: this stub PREEMPTS the game's own dlmalloc init. The real init
    // (Function_8246A2A8, run via CRT _initterm) only builds a real mstate via
    // sub_8246C4D0 -> NtAllocateVirtualMemory `if (DAT_829ac16c == 0)`; by writing
    // kHeapStateVA into these handles FIRST we make it see a non-null handle and
    // skip the real heap entirely -> allocations fall to the host bump allocator
    // (guestHeapAlloc, zero-gap) whose overlap is the FMV parser+0x260 clobber
    // (iter-357l/360). This is UNGATED FORCING (fake mstate). GATE it so we can
    // A/B letting the guest's REAL dlmalloc run. SR_HEAP_STUB=0 disables the
    // preemption (default ON for now to preserve boot until the real heap is
    // proven; the intent is force->confirm->DELETE per charter).
    // ⚠️ iter-378: measured load-bearing THEN — K=4 300s: 0/4 FULL-INIT,
    // walksRet=0, lastInitObj=0x00080000 on the real dlmalloc path.
    // ✅ iter-380c (2026-07-26): RETIRED — DEFAULT FLIPPED OFF. The iter-378
    // measurement was taken on the pre-iter-380 exe, where the iter-66
    // suppression of the DOGMA explicit-pool fast path was still starving
    // early init; the "real dlmalloc death" was one more manufactured
    // failure. On the pristine-dispatch exe: K=3 SR_HEAP_STUB=0 -> 3/3 DEEP
    // (defExec=666, movInst=451, feFrames 3971-4077), all four handle
    // globals 0 at boot (the guest BUILT its real heap), CAD8 allocs served
    // from the real-heap region (0x898Axxxx vs the stub run's bump-window
    // 0x8A25xxxx), zero NULL enqueues. [ALLOC_NULL] size=0 lines = the known
    // malloc(0) semantics story (SR_MALLOC0_FIX arc), unchanged by this.
    // =1 re-arms the fake mstate as a DIAGNOSTIC only. Hazards of the
    // diagnostic, unchanged: kHeapStateVA=0x82110000 sits on REAL .text
    // (memset zeroes sub_82110A30's bytes) and the null-heap path fabricates
    // a chunk chain at VA 0/0x10000 (iter-377 H1). Once a few sessions stay
    // clean, DELETE this block + the iter-364 fence machinery built around
    // the fake mstate.
    static const bool s_heapStub = [] {
      const char* e = std::getenv("SR_HEAP_STUB");
      return e && e[0] == '1';
    }();
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "  Heap handle preempt %s -> 0x%08X (0x829AC16C=%u 0x82A2C16C=%u 0x8296C16C=%u 0x829BE4CC=%u)",
      s_heapStub ? "ON(stub)" : "OFF(real dlmalloc init)", kHeapStateVA,
      currentSub, current1, current2, current3);
    if (s_heapStub) {
      writeGuestBE32(g_guestBase + kHeapHandleSub8246DF08VA, kHeapStateVA);
      if (kHeapHandlePtrPath1VA < g_guestSize)
        writeGuestBE32(g_guestBase + kHeapHandlePtrPath1VA, kHeapStateVA);
      if (kHeapHandlePtrVA < g_guestSize)
        writeGuestBE32(g_guestBase + kHeapHandlePtrVA, kHeapStateVA);
      if (kHeapHandlePtr2VA < g_guestSize)
        writeGuestBE32(g_guestBase + kHeapHandlePtr2VA, kHeapStateVA);
    }
    if (s_heapStub && needsInit) {
      
      // ALSO initialize address 0 as a fallback heap state.
      // Some code paths pass r3=0 (null heap) to malloc, which then reads from
      // offsets like 0+1408. By initializing address 0 with valid heap state data,
      // we prevent crashes when the game passes null heap handles.
      SR_LOG_BOOT(sr::LogSeverity::Info, "  Also initializing fallback heap at VA 0x00000000");
      
      // Set up the fallback heap state at address 0.
      // dlmalloc-style allocator uses:
      // - offset 1408: critical section pointer
      // - offset 48-1400: bin arrays (each bin entry is 8 bytes: fwd, back ptrs)
      // - offset ~8-24: top chunk pointer (wilderness - fallback when bins empty)
      // For empty bins, fwd/back point to the bin itself (self-referential).
      constexpr uint32_t kFallbackCritSect = 0x00002000u;
      constexpr uint32_t kFallbackHeapBase = 0x00010000u;  // Start of fallback heap memory
      constexpr uint32_t kFallbackHeapSize = 0x00100000u;  // 1MB fallback heap
      
      writeGuestBE32(g_guestBase + 1408, kFallbackCritSect);
      // Offset 80: sub_827AC44C/sub_827AC468 use r3 = 80(r30); when r30=0 we load from VA 0x50.
      writeGuestBE32(g_guestBase + 80, kFallbackCritSect);
      std::memset(g_guestBase + kFallbackCritSect, 0, 64);
      
      // Initialize "top" chunk pointer at several common offsets (dlmalloc variants)
      // The top chunk is the "wilderness" - contiguous free memory for allocations
      // Different dlmalloc versions put this at different offsets (8, 16, 20, 24)
      writeGuestBE32(g_guestBase + 8, kFallbackHeapBase);   // Common offset for top
      writeGuestBE32(g_guestBase + 16, kFallbackHeapBase);  // Another common offset
      writeGuestBE32(g_guestBase + 20, kFallbackHeapBase);  // Yet another
      writeGuestBE32(g_guestBase + 24, kFallbackHeapSize);  // Top size
      
      // Initialize the top chunk itself (at kFallbackHeapBase)
      // Top chunk header: prev_size(4), size(4) with flags, then user data
      // Size has low bits: PINUSE (0x1) = previous in use, CINUSE (0x2) = current in use
      // For a free top chunk: PINUSE set, CINUSE clear = 0x1 | (size & ~0x7)
      uint32_t topSize = kFallbackHeapSize | 0x1;  // PINUSE set, marks free top
      writeGuestBE32(g_guestBase + kFallbackHeapBase + 0, 0);       // prev_size (unused for top)
      writeGuestBE32(g_guestBase + kFallbackHeapBase + 4, topSize); // size with PINUSE
      
      // Initialize bin arrays at address 0+offset with self-referential pointers.
      // Each bin at offset N has: [N+0]=N (fwd points to self), [N+4]=N (back points to self)
      // This makes malloc see all bins as empty.
      for (uint32_t binOffset = 48*8; binOffset < 1408; binOffset += 8) {
        writeGuestBE32(g_guestBase + binOffset, binOffset);      // fwd = self
        writeGuestBE32(g_guestBase + binOffset + 4, binOffset);  // back = self
      }
      
      SR_LOG_BOOT(sr::LogSeverity::Info, "  Fallback heap: top chunk at 0x%08X, size 0x%08X", 
                  kFallbackHeapBase, kFallbackHeapSize);
      
      // sub_827AC498 (RtlEnterCriticalSection caller): loads crit sect from table at 0x82E12AE0 + index*8.
      // When entry is 0 we get null r3. Fill first entries with fallback crit sect so small indices succeed.
      constexpr uint32_t kCritSectTableVA = 0x82E12AE0u;
      constexpr uint32_t kCritSectTableEntries = 256u;
      if (kCritSectTableVA + kCritSectTableEntries * 8 <= g_guestSize) {
        for (uint32_t i = 0; i < kCritSectTableEntries; ++i)
          writeGuestBE32(g_guestBase + kCritSectTableVA + i * 8, kFallbackCritSect);
        SR_LOG_BOOT(sr::LogSeverity::Info, "  Critical section table at 0x%08X: %u entries -> 0x%08X",
                    kCritSectTableVA, kCritSectTableEntries, kFallbackCritSect);
      }
      
      // Initialize minimal heap state structure (dlmalloc-like)
      // The malloc function accesses various offsets. Key ones:
      // - Offset 20: flags (checked for certain bits)
      // - Offset 24: more flags
      // - Offset 1408: critical section pointer (must be valid or NULL for our stub)
      uint8_t* heapState = g_guestBase + kHeapStateVA;
      std::memset(heapState, 0, 0x2000);  // Zero out 8KB for heap state
      
      // Critical section at offset 1408 - allocate a stub critical section
      constexpr uint32_t kCritSectOffset = 1408;
      constexpr uint32_t kCritSectVA = kHeapStateVA + 0x1000;  // Critical section at heap+4096
      writeGuestBE32(heapState + kCritSectOffset, kCritSectVA);
      
      // Initialize critical section structure (zero is fine for our stub)
      std::memset(heapState + 0x1000, 0, 64);
      
      // Verify the write
      uint32_t verifyCritSect = readGuestBE32(heapState + kCritSectOffset);
      uint32_t verifyHeapHandle = readGuestBE32(g_guestBase + kHeapHandleSub8246DF08VA);
      SR_LOG_BOOT(sr::LogSeverity::Info, "  Heap critical section at 0x%08X (wrote to VA 0x%08X+%u)", 
                  kCritSectVA, kHeapStateVA, kCritSectOffset);
      SR_LOG_BOOT(sr::LogSeverity::Info, "  Verify: heap handle @ 0x%08X = 0x%08X, crit sect @ 0x%08X = 0x%08X",
                  kHeapHandleSub8246DF08VA, verifyHeapHandle, kHeapStateVA + kCritSectOffset, verifyCritSect);
    } else {
      SR_LOG_BOOT(sr::LogSeverity::Info, "  Heap handles already set: 0x%08X, 0x%08X, 0x%08X",
                  current1, current2, current3);
    }
  }

#endif  // SR_ORACLE: retired fabricated CRT heap

  // (DOGMA guards written at start of init so they are set even when we return early above.)

#if SR_ORACLE
  // --- Initialize EDRAM / GPU memory region ---
  // Xbox 360 has 10MB of embedded DRAM (EDRAM) at physical address 0x7E800000.
  // The game's GPU initialization code expects to access this region.
  // We initialize it with valid data to prevent null pointer dereferences.
  constexpr uint32_t kEDRAMBase = 0x7E800000u;
  constexpr uint32_t kEDRAMSize = 0x01800000u;  // 24MB to cover EDRAM + surrounding GPU regs
  
  if (kEDRAMBase + kEDRAMSize <= g_guestSize) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "  Initializing EDRAM region at 0x%08X (size 0x%08X)", 
                kEDRAMBase, kEDRAMSize);
    
    // Zero out the EDRAM region - this prevents garbage data from being interpreted as pointers
    std::memset(g_guestBase + kEDRAMBase, 0, kEDRAMSize);
    
    // GPU command buffer / ring buffer setup - games often check these during init
    // Xbox 360 GPU uses a ring buffer at a specific location
    // We set up minimal structures to satisfy null checks
    constexpr uint32_t kGPURegBase = 0x7FC00000u;  // GPU register space typically here
    if (kGPURegBase + 0x10000 <= g_guestSize) {
      std::memset(g_guestBase + kGPURegBase, 0, 0x10000);
    }
    
    SR_LOG_BOOT(sr::LogSeverity::Info, "  EDRAM initialized");
  } else {
    SR_LOG_BOOT(sr::LogSeverity::Error, "  EDRAM region 0x%08X extends beyond allocation (0x%zX)", 
                kEDRAMBase + kEDRAMSize, g_guestSize);
  }
#endif
}

}  // namespace kernel
}  // namespace sr
