// iter-178: fix GPU ring buffer allocation failure that prevents VdInitializeRingBuffer
// from ever being called.
//
// Root cause (traced through ppc_recomp.1.cpp:23153-23221):
//   The GPU init function allocates two ring buffer memory blocks via sub_82167F90
//   (trampoline → sub_82467F40). Both calls have bit 31 of r4 set, which routes
//   sub_82467F40 to sub_82469B48 → sub_8246DF08 (a CRT-init stub that never
//   finishes wiring, cf. iter-32 crt_file_lock_init_fix.cpp) → sub_8246CA80 → 0.
//
//   With r27=0 or r30=0, the GPU init jumps to loc_820FE2EC, skipping
//   VdInitializeRingBuffer entirely. g_ringBufBase stays at the fake ring
//   (0xD0010000), and the PM4 scanner never sees real draw commands.
//
// Fix: strong-override sub_82467F40. When bit 31 of r4 is set (the broken path),
// use guestHeapAlloc(r3) directly. Zero-init the block, return the VA, and log.
// Also store the FIRST successful ring alloc to g_gpuRingAllocVA so that
// VdInitializeRingBuffer (which gets r3=0 from stub_generic_success MmGetPhysicalAddress)
// can find the ring VA via the global instead.
//
// The bit-31-clear path delegates to the original body, which routes through
// sub_82468008 → MmAllocatePhysicalMemoryEx → already works.

#include "guest/ppc/ppc_recomp_shared.h"
#include "gpu_phys_route.hpp"
#include "guest_context.hpp"
#include "sr_oracle.hpp"
#include "phys_window.hpp"
#include "phys_alloc.hpp"
#include "phys_lifo.hpp"   // iter-465 physmem: PhysKindScope
#include "guest_call.hpp"
#include "../app/log.hpp"
#include "../gfx/resource_census.hpp"  // mem.resources memres5: the grants' windows (census only; no-ops under SR_ORACLE=0)
#include "../gfx/resource_memory.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

extern "C" void __imp__sub_82467F40(PPCContext& __restrict ctx, uint8_t* base);
void sub_82467F40(PPCContext& __restrict ctx, uint8_t* base);  // fwd: the hooked (fixed) allocator
// iter-462 (guest_context.cpp; not in guest_context.hpp, which every guest TU includes).
namespace sr { namespace kernel { void guestPhysReserveAlias(uint32_t va, uint32_t size); } }

// iter-360: DETERMINISTIC isolation probe for the FMV allocator corruption.
// The FMV wall is: the ASF-parser (0x270 bytes, flags 0x248c8000) has its tail
// field parser+0x260 zeroed when the very next 0x20 allocation lands adjacent
// (caught live: [XMV_ALLOC] size=0x20 -> parser_end+0x10, clobbers +260). This
// probe reproduces JUST that alloc pattern in isolation — no FMV path, no
// roll-dependence — to decide: is the clobber intrinsic to sub_82467F40 at this
// size pattern (deterministic allocator bug), or context-dependent (an earlier
// heap corruption planted a phantom free chunk)? Gated SR_ALLOC_PROBE=1.
static bool allocProbeOn() {
  static const bool v = [] {
    // iter-463 charter: calls the guest allocator + writes markers = forcing.
    const char* e = std::getenv("SR_ALLOC_PROBE");
    return sr::kernel::forcingAllowed() && e && e[0] == '1';
  }();
  return v;
}

static void runAllocIsolationProbe() {
  // iter-377: self-gate (same rule as runAllocWatchProbe below) — a probe that
  // calls the guest allocator and writes marker patterns into guest memory must
  // be visibly gated at its own entry, not only at the call site.
  if (!allocProbeOn()) return;
  using namespace sr::kernel;
  GuestCaller gc("ALLOC_PROBE");
  if (!gc.ok()) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[ALLOC_PROBE] GuestCaller setup failed");
    return;
  }
  // Call the HOOKED allocator so the probe reflects the SR_ALLOC_FIX state
  // (raw __imp__ always clobbers 16-aligned; hooked is clean when the fix is on).
  GuestFn* alloc = reinterpret_cast<GuestFn*>(&sub_82467F40);
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[ALLOC_PROBE] characterizing (via HOOKED allocator): "
              "for each request R, alloc R then 0x20; is R's last 16 bytes (R-0x10) clobbered?");
  // Probe a spread of sizes: 16-aligned (0x100/0x270/0x500), non-aligned
  // (0x268/0x271), and small (0x30). If ONLY 16-aligned sizes clobber, the
  // rounding leaves zero slack and the next chunk's tag lands in usable space.
  const uint32_t sizes[] = {0x30, 0x100, 0x268, 0x270, 0x271, 0x280, 0x500};
  for (uint32_t R : sizes) {
    GuestArgs a1{}; a1.r3 = R; a1.r4 = 0x248c8000; uint32_t exc = 0;
    if (!gc.call(alloc, a1, &exc)) { SR_LOG_BOOT(sr::LogSeverity::Warn, "[ALLOC_PROBE] R=0x%X FAULT", R); continue; }
    const uint32_t P1 = gc.ctx().r3.u32;
    if (P1 < 0x10000u || P1 >= 0xC0000000u) continue;
    // Fill the whole requested region with a per-word marker; the tail word we
    // watch is at R-0x10 (where the FMV clobber landed for R=0x270).
    const uint32_t watch = (R >= 0x10u) ? (R - 0x10u) : 0u;
    for (uint32_t o = 0; o + 4 <= R; o += 4) writeGuestBE32(P1 + o, 0xAB000000u | o);
    const uint32_t pre = readGuestU32BE(P1 + watch);
    GuestArgs a2{}; a2.r3 = 0x20; a2.r4 = 0x248c8000;
    if (!gc.call(alloc, a2, &exc)) { SR_LOG_BOOT(sr::LogSeverity::Warn, "[ALLOC_PROBE] R=0x%X 0x20 FAULT", R); continue; }
    const uint32_t P2 = gc.ctx().r3.u32;
    const uint32_t post = readGuestU32BE(P1 + watch);
    // Also scan how many of R's own words got corrupted (the damage extent).
    uint32_t firstBad = 0xFFFFFFFFu, nBad = 0;
    for (uint32_t o = 0; o + 4 <= R; o += 4) {
      if (readGuestU32BE(P1 + o) != (0xAB000000u | o)) { if (firstBad == 0xFFFFFFFFu) firstBad = o; nBad++; }
    }
    const bool clob = post != pre;
    SR_LOG_BOOT(clob ? sr::LogSeverity::Error : sr::LogSeverity::Info,
        "[ALLOC_PROBE] R=0x%-4X P1=%08X P2=%08X gap=0x%-3X aligned16=%d | tail[R-0x10] %08X->%08X %s | corrupted %u words from +0x%X",
        R, P1, P2, (unsigned)(P2 - P1), (R & 0xf) == 0, pre, post,
        clob ? "CLOBBERED" : "clean", nBad, firstBad == 0xFFFFFFFFu ? 0 : firstBad);
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[ALLOC_PROBE] done — see which sizes clobber");
}

// iter-357l: published by cs_timeout_underflow_fix.cpp's ASF-walk probes.
#if SR_ORACLE
extern std::atomic<uint32_t> g_xmvParser;
#endif

// Declared in stubs.cpp inside namespace sr::kernel.
namespace sr { namespace kernel {
#if SR_ORACLE
extern std::atomic<uint32_t> g_gpuRingAllocVA;
void trySetRingBufBaseFromAlloc(uint32_t allocVA);
#endif
// iter-467 p2 fix (4), U1: SR_VD_RING_ARG (stubs.cpp) - the ring comes from VdInitializeRingBuffer's
// physical-address argument, so no physical request needs the pre-claim heap route.
bool vdRingFromArgOn();
// iter-360f: VEH write-watch (image_write_watchdog.cpp) to name the miscompiled
// store that clobbers a chunk's tail.
bool armRuntimeWriteWatch(uint32_t guestVA, uint32_t len);
void disarmRuntimeWriteWatch();
} }

// iter-360f: WRITE-WATCH probe — name the exact store that clobbers the tail.
// The dlmalloc-recon workflow proved the core arithmetic is faithful and the
// clobber is a port-only miscompile in a store landing at chunk_end-0x10 (the
// alloc(0x20) carve/split path). Reproduce the RAW clobber (call __imp__ to
// bypass SR_ALLOC_FIX), arm the VEH on the tail across the triggering alloc, and
// let [IMAGE_WRITE_VEH] print the writer's guest PC + host RIP + regs. Gated
// SR_ALLOC_WATCH=1; fires once. NON-destructive (read-only diagnostic).
static bool allocWatchOn() {
  static const bool v = [] {
    // iter-463 charter: calls the guest allocator + writes markers = forcing.
    const char* e = std::getenv("SR_ALLOC_WATCH");
    return sr::kernel::forcingAllowed() && e && e[0] == '1';
  }();
  return v;
}

// iter-364: defined further down (with the memset hook); declared here for the probe.
namespace sr { namespace kernel { void setAllocMemsetWatch(uint32_t va, uint32_t len); } }
// iter-364: hardware (Dr0) write-watch — kernel/hw_watchpoint.cpp.
namespace sr { namespace kernel { bool armHwWriteWatchHost(void* hostAddr); void disarmHwWriteWatch(); } }

static void runAllocWatchProbe() {
  // iter-377: self-gate. The only caller already checks allocWatchOn(), but the
  // audit classifies per-function and a probe that CALLS a guest allocator and
  // writes a test pattern into guest memory must be visibly gated at its own
  // entry, not by convention at the call site.
  if (!allocWatchOn()) return;
  using namespace sr::kernel;
  GuestCaller gc("ALLOC_WATCH");
  if (!gc.ok()) { SR_LOG_BOOT(sr::LogSeverity::Warn, "[ALLOC_WATCH] GuestCaller failed"); return; }
  // RAW allocator (bypass the +0x10 bump) so the clobber reproduces.
  GuestFn* rawAlloc = reinterpret_cast<GuestFn*>(&__imp__sub_82467F40);
  const uint32_t R = 0x270;
  GuestArgs a1{}; a1.r3 = R; a1.r4 = 0x248c8000; uint32_t exc = 0;
  if (!gc.call(rawAlloc, a1, &exc)) { SR_LOG_BOOT(sr::LogSeverity::Warn, "[ALLOC_WATCH] alloc R FAULT"); return; }
  const uint32_t P1 = gc.ctx().r3.u32;
  if (P1 < 0x10000u || P1 >= 0xC0000000u) { SR_LOG_BOOT(sr::LogSeverity::Warn, "[ALLOC_WATCH] bad P1=%08X", P1); return; }
  for (uint32_t o = 0; o + 4 <= R; o += 4) writeGuestBE32(P1 + o, 0xAB000000u | o);
  const uint32_t tail = P1 + (R - 0x10u);  // = P1+0x260, the field the FMV parser loses
  const uint32_t pre = readGuestU32BE(tail);
  // iter-360h: dump the chunk header geometry. dlmalloc stores a ushort size (in
  // 0x10 units) at chunk+0; user ptr should be chunk+0x10, so the header is at
  // P1-0x10. Dump P1-0x20..P1 to SEE where the size (0x28 for a 0x270 req) lands:
  // header at P1-0x10 => usable=chunksize-0x10=R (correct); at P1-0x20 => the
  // return offset is +0x20 not +0x10 => usable=R-0x10 (the overlap defect).
  // iter-360i: compare our live heap to the GOLDEN format. Golden (working):
  // mstate at DAT_829ac16c has magic 0xeeffeeff at mstate+0x10; every chunk has a
  // 0x10 header at user-0x10 = [size:u16][prevfoot:u16][?:1][flags:1(CINUSE)]...;
  // golden reads user-0x10 -> a valid size (0x28 units for a 0x270 alloc). Our
  // probe read P1-0x10 = 0. Dump our mstate + P1's golden-format header to see if
  // (a) our mstate is valid, (b) P1 is inside its managed range, (c) the size
  // header is actually written at P1-0x10.
  const uint32_t mstate = readGuestU32BE(0x829ac16cu);
  const uint32_t magic  = mstate ? readGuestU32BE(mstate + 0x10u) : 0;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
    "[ALLOC_WATCH] P1=%08X | mstate=%08X magic=%08X(want EEFFEEFF) | hdr@P1-0x10: "
    "size=%04X prevfoot=%04X flags=%02X | P1-0x10..P1=%08X %08X %08X %08X | tail=%08X pre=%08X",
    P1, mstate, magic,
    (uint16_t)(readGuestU32BE(P1 - 0x10) >> 16), (uint16_t)(readGuestU32BE(P1 - 0xe) >> 16),
    (uint8_t)(readGuestU32BE(P1 - 0xc) >> 16),
    readGuestU32BE(P1 - 0x10), readGuestU32BE(P1 - 0xc), readGuestU32BE(P1 - 8),
    readGuestU32BE(P1 - 4), tail, pre);
  const bool armed = armRuntimeWriteWatch(tail, 16);
  sr::kernel::setAllocMemsetWatch(tail, 16);   // iter-364: race-free memset naming
  // iter-364: Dr0 hardware write-watch on the tail — race-free, address-exact.
  // The alloc runs synchronously on THIS thread, so the store traps here.
  uint8_t* gbase = nullptr; size_t gsz = 0;
  getGlobalGuestMemory(&gbase, &gsz);
  bool hwArmed = gbase && armHwWriteWatchHost(gbase + tail);
  GuestArgs a2{}; a2.r3 = 0x20; a2.r4 = 0x248c8000;
  gc.call(rawAlloc, a2, &exc);   // the clobbering store fires here -> [HWWATCH]/[MEMSET_HIT]
  const uint32_t P2 = gc.ctx().r3.u32;
  if (hwArmed) disarmHwWriteWatch();
  sr::kernel::setAllocMemsetWatch(0, 0);
  if (armed) disarmRuntimeWriteWatch();
  const uint32_t post = readGuestU32BE(tail);
  SR_LOG_BOOT(post != pre ? sr::LogSeverity::Error : sr::LogSeverity::Info,
    "[ALLOC_WATCH] P2=%08X tail %08X->%08X %s -- see [IMAGE_WRITE_VEH] # WRITE for the store",
    P2, pre, post, post != pre ? "CLOBBERED" : "clean");
}

// iter-364: NAME THE CLOBBERING WRITE without the VEH (the page-guard is racy here:
// the heap page carries concurrent traffic from other threads, and any thread's fault
// globally unprotects the page, so the probe's clobbering store slips through the
// window — that's why [ALLOC_WATCH] reproduces the clobber but logs no hit).
// The damage is exactly 16 ZERO bytes = a memset signature, and sub_8246CA80 calls
// memset (sub_8279F290) internally. Range-filter memset instead: zero cost when
// disarmed (one relaxed load), exact caller via ctx.lr, no faults, no races.
namespace {
std::atomic<uint32_t> g_memsetWatchVA{0};
std::atomic<uint32_t> g_memsetWatchLen{0};
}  // namespace
namespace sr { namespace kernel {
void setAllocMemsetWatch(uint32_t va, uint32_t len) {
  g_memsetWatchLen.store(len, std::memory_order_relaxed);
  g_memsetWatchVA.store(va, std::memory_order_relaxed);
}
} }
extern "C" void __imp__sub_8279F290(PPCContext& __restrict ctx, uint8_t* base);
void sub_8279F290(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t wva = g_memsetWatchVA.load(std::memory_order_relaxed);
  if (wva) {
    const uint32_t dst = ctx.r3.u32, val = ctx.r4.u32, len = ctx.r5.u32;
    const uint32_t wlen = g_memsetWatchLen.load(std::memory_order_relaxed);
    // overlap test against the watched window
    if (len && dst < wva + wlen && dst + len > wva) {
      static std::atomic<uint32_t> s_n{0};
      uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 8)
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[MEMSET_HIT] #%u memset(dst=%08X val=%X len=0x%X) OVERLAPS watch [%08X,+0x%X) lr=%08X",
          n, dst, val, len, wva, wlen, (uint32_t)ctx.lr);
    }
  }
  __imp__sub_8279F290(ctx, base);
}

// iter-360: compensate for the recompiled allocator's chunk under-alloc.
// PROVEN (SR_ALLOC_PROBE, size sweep): our sub_8246CA80 rounds a request R to a
// chunk (R+0x1f)&~0xf, which reserves the 0x10 FRONT header but NOT the 0x10
// BACK boundary tag the NEXT chunk writes — so any request that reaches into
// its chunk's last 0x10 has that tail clobbered (16-aligned R loses 0x10; any
// R loses up to R&0xf). The FMV needs BOTH the 0x270 video parser (16-aligned)
// AND the 0x7f8 audio decoder object (not 16-aligned, loses its last 8 bytes).
// The 0x1f rounding is the ORIGINAL game constant and the game works on
// hardware -> the console does not write that tag into in-use neighbors = a
// recompiler lowering bug in the tag-write. Until the regen root-fix, bump
// bit31-clear requests by 0x10 so chunk(R+0x10) >= R+0x20 (front+back room).
// iter-432 RETIREMENT (charter: force->diagnose->root-fix->confirm->DELETE):
// the regen root-fix LANDED with the iter-357k transplant — SR_ALLOC_WATCH's
// raw-__imp__ probe now shows a textbook header (mstate magic EEFFEEFF,
// size=0x28 @P1-0x10) and a CLEAN tail. The bump had become pure harm: it
// distorted heap layout vs console and correlated with the roll instability
// (WITH bump: 1/3 clean rolls, ASF-reader AV deaths; WITHOUT: 6/6 clean,
// all movies + FSM walk on every roll). DEFAULT OFF; SR_ALLOC_FIX=1
// temporarily re-enables for one deprecation cycle, then delete the hook.
static bool allocFixOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_ALLOC_FIX");
    return e && e[0] == '1';
  }();
  return v;
}

// iter-458 HR1 (Astra 071): SR_GPU_PHYS_ROUTE. A PLATFORM FIX, not forcing:
// the override stops substituting an allocator for the guest's.
// iter-462: DEFAULT ON (=0 keeps the heap route for A/B). The heap route
// double-booked physical pages with the 224 MB world pool once LIFO placed it
// at 0xB0008000: ~20 title / 14-19 gameplay textures read a heap object's
// bytes (the title highway, rooftops, meteor-site confetti) and the soft-particle
// depth texture header read zero. With the route + the ring's alias hole
// (guestPhysReserveAlias): window conflicts 514025 -> 36, clean title and
// street in archive/v1_hr1c, v1_hr1d (ENGINE_MAP "LIFO moved the double-booking").
//   The guest's own sub_82467F40 (IDA 0x82467F54) sends a bit-31-SET request to
//   sub_82468008 -> MmAllocatePhysicalMemoryEx(0, size, protect, 0, -1,
//   1 << attrs-nibble), zero-fills it when attrs bit 30 is set, and on failure
//   records SetLastError(8) (sub_82469C10) and returns null. The intercept
//   below serves such requests from the 0x8-window heap when they are larger
//   than 256 KB, and heap VAs alias real physical grants (hlifo3 pair 12:
//   89/112 live overlapping pairs).
// Contract with the gate on:
//   * Boundary: the RING CLAIM - the first successful intercepted grant, which
//     publishes g_gpuRingAllocVA (the ring-VA detection reads it). That is NOT
//     proof that ring initialisation finished. Before it, the intercept is
//     unchanged, so the claim keeps its path.
//   * After it, EVERY bit-31 request (any size, including 0 and > 64 MB) runs
//     the guest's own path. No heap fallback, no direct guestPhysAlloc from
//     here; the kernel import registers the grant, the override registers
//     nothing.
//   * Reclamation, bit-31-clear requests and everything before the claim are
//     unchanged. Gate off = today's ROUTING (the counters below add work).
// Evidence (Astra 072/073):
//   * ONE boundary observation per call: `claimed` is read once at admission
//     and classifies the call for both routing (hr1) and the witness. A request
//     admitted before the claim whose heap route is taken after it (the routing
//     re-reads ringUp) is counted apart as SPANNING, never as after-claim.
//   * ONE accounting lock (g_hr1Mu): every bit-31 request is ADMITTED under it
//     (its claim observation, and `started` when delegated) before anything
//     else happens, and SETTLED under it on every exit path (Hr1Call's
//     destructor): completed/null/below-A for a delegation, after-claim or
//     spanning for an intercepted heap route.
//   * ONE capture endpoint: the recorder's close takes the registry lock, then
//     this lock, seals H-UNIQ, and snapshots - no registration, admission or
//     settlement falls between the seal and the snapshot, and the snapshot
//     names the seal (nonce, seq). An admitted-but-unsettled request is
//     OUTSTANDING there; outstanding, a missing snapshot or an unsealed one
//     leaves the run INCONCLUSIVE.
static std::atomic<int> g_hr1Mode{-1};   // -1 env, 0 off, 1 on (test seam)
static std::mutex g_hr1Mu;               // THE accounting lock
struct Hr1Counters {
  uint64_t admitted = 0, settled = 0, started = 0, completed = 0, null = 0, belowA = 0,
           heapAfterClaim = 0, heapSpanning = 0;
};
static Hr1Counters g_hr1;                // guarded by g_hr1Mu
static std::atomic<void (*)(int)> g_hr1PhaseHook{nullptr};   // test seam (phases 1-3)
static bool gpuPhysRouteOn() {
  const int m = g_hr1Mode.load(std::memory_order_relaxed);
  if (m >= 0) return m == 1;
  static const bool v = [] {
    const char* e = std::getenv("SR_GPU_PHYS_ROUTE");
    const bool on = !e || e[0] != '0';
    // The capture snapshot rides the recorder's close hook; arm it for the
    // gate even when no census is on (it is idempotent).
    if (on) sr::kernel::physwin::armRecorderCloseHook();
    return on;
  }();
  return v;
}
static void hr1Phase(int phase) {
  if (void (*h)(int) = g_hr1PhaseHook.load(std::memory_order_relaxed)) h(phase);
}
// One bit-31 request's accounting: ADMITTED at construction (its single claim
// observation, under the lock), SETTLED at destruction (under the lock) on
// every exit path of the override.
struct Hr1Call {
  bool bit31 = false, claimed = false, hr1 = false;
  bool heapRouted = false, ringUpAtRoute = false;   // an intercepted heap route
  bool delegatedDone = false;                       // the guest's own path returned
  uint32_t result = 0, size = 0, flags = 0, lr = 0;
  explicit Hr1Call(uint32_t f) : flags(f) {
    bit31 = (f & 0x80000000u) != 0;
    if (!bit31) return;
    std::lock_guard<std::mutex> lk(g_hr1Mu);
    // iter-467 p2 fix (4), U1 (docs/NATIVE_ROADMAP.md P2.9): under SR_VD_RING_ARG the ring is learned from
    // VdInitializeRingBuffer's argument (a real MmGetPhysicalAddress, SR_MM_PHYSADDR), so there is no claim
    // to make: every physical request is past it from boot and, with SR_GPU_PHYS_ROUTE, takes the guest's
    // own sub_82468008 -> MmAllocatePhysicalMemoryEx path (Xenia). The heap route below is never reached.
#if SR_ORACLE
    claimed = sr::kernel::vdRingFromArgOn() ||
              sr::kernel::g_gpuRingAllocVA.load(std::memory_order_acquire) != 0;
#else
    claimed = true;  // native allocation never waits for an emulated ring claim
#endif
    hr1 = gpuPhysRouteOn() && claimed;
    g_hr1.admitted++;
    if (hr1) g_hr1.started++;
  }
  Hr1Call(const Hr1Call&) = delete;
  Hr1Call& operator=(const Hr1Call&) = delete;
  ~Hr1Call() {
    if (!bit31) return;
    if (hr1) hr1Phase(3);
    std::lock_guard<std::mutex> lk(g_hr1Mu);
    g_hr1.settled++;
    if (hr1 && delegatedDone) {
      const bool null = result == 0;
      if (null) g_hr1.null++;
      else if (result < 0xA0000000u) g_hr1.belowA++;
      const uint64_t n = ++g_hr1.completed;
      if (n <= 8 || (n & (n - 1)) == 0 || (null && g_hr1.null <= 16))
        SR_LOG_BOOT(null ? sr::LogSeverity::Warn : sr::LogSeverity::Info,
            "[GPU_PHYS_ROUTE] #%llu size=0x%X flags=0x%08X lr=%08X -> guest path -> 0x%08X "
            "(sampled line; the capture snapshot carries the exact counts)",
            (unsigned long long)n, size, flags, lr, result);
    } else if (!hr1 && heapRouted) {
      if (claimed) g_hr1.heapAfterClaim++;
      else if (ringUpAtRoute) g_hr1.heapSpanning++;
    }
  }
};
namespace sr { namespace kernel {
static GpuPhysRouteSnapshot g_hr1Snap;   // guarded by g_hr1Mu
static GpuPhysRouteSnapshot hr1NowLocked() {
  GpuPhysRouteSnapshot s;
  s.gate = gpuPhysRouteOn() ? 1 : 0;
  s.admitted = g_hr1.admitted;
  s.settled = g_hr1.settled;
  s.started = g_hr1.started;
  s.completed = g_hr1.completed;
  s.null = g_hr1.null;
  s.belowA = g_hr1.belowA;
  s.heapAfterClaim = g_hr1.heapAfterClaim;
  s.heapSpanning = g_hr1.heapSpanning;
  // The import's counters change outside this lock: a request in flight may
  // already be counted (conservative - it can only make BUDGET inadequate).
  uint64_t mm[2];
  mmAllocSizeAnomalies(mm);
  s.mmTruncated = mm[0];
  s.mmWrapped = mm[1];
  return s;
}
std::mutex& gpuPhysRouteAccountingMutex() { return g_hr1Mu; }
// physwin::uniqOnRecorderClose calls this holding the registry lock AND
// g_hr1Mu, right after the seal. The FIRST close is the capture boundary.
void gpuPhysRouteCaptureSnapshotLocked(const char* why, bool atSeal, uint32_t nonce,
                                       uint64_t sealSeq) {
  if (g_hr1Snap.taken) return;
  GpuPhysRouteSnapshot s = hr1NowLocked();
  s.taken = true;
  s.atSeal = atSeal ? 1 : 0;
  s.nonce = nonce;
  s.sealSeq = sealSeq;
  g_hr1Snap = s;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[GPU_PHYS_ROUTE] CAPTURE SNAPSHOT why=%s atSeal=%d nonce=%08X sealSeq=%llu gate=%d "
      "admitted=%llu settled=%llu outstanding=%llu started=%llu completed=%llu null=%llu "
      "delegatedBelowA=%llu heapAfterClaim=%llu heapSpanning=%llu mmTruncated=%llu mmWrapped=%llu",
      why, s.atSeal, s.nonce, (unsigned long long)s.sealSeq, s.gate,
      (unsigned long long)s.admitted, (unsigned long long)s.settled,
      (unsigned long long)(s.admitted - s.settled), (unsigned long long)s.started,
      (unsigned long long)s.completed, (unsigned long long)s.null, (unsigned long long)s.belowA,
      (unsigned long long)s.heapAfterClaim, (unsigned long long)s.heapSpanning,
      (unsigned long long)s.mmTruncated, (unsigned long long)s.mmWrapped);
}
GpuPhysRouteSnapshot gpuPhysRouteSnapshotForTest() {
  std::lock_guard<std::mutex> lk(g_hr1Mu);
  return g_hr1Snap;
}
GpuPhysRouteSnapshot gpuPhysRouteNowForTest() {
  std::lock_guard<std::mutex> lk(g_hr1Mu);
  return hr1NowLocked();
}
void gpuPhysRouteResetForTest() {
  std::lock_guard<std::mutex> lk(g_hr1Mu);
  g_hr1Snap = GpuPhysRouteSnapshot{};
  g_hr1 = Hr1Counters{};
}
void setGpuPhysRouteForTest(int mode) { g_hr1Mode.store(mode, std::memory_order_relaxed); }
void setGpuPhysRoutePhaseHookForTest(void (*fn)(int)) {
  g_hr1PhaseHook.store(fn, std::memory_order_relaxed);
}
} }

// iter-361: VB-alloc provenance probe. The flush-grind root is that
// D3DVertexBufferCreate_sub_820F3ED8's header(32B, bit31-clear) and data(80B,
// bit31-set) allocations OVERLAP by 0x10 (proven: header@0x8FFBE720,
// data@0x8FFBE730), so the vertex memcpy scribbles vb[5]. Log both allocs from
// that caller with the path taken + returned address to resolve which allocator
// under-reserves. Gated SR_VBALLOC_PROBE=1.
static bool vbAllocProbeOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_VBALLOC_PROBE");
    return e && e[0] == '1';
  }();
  return v;
}

// iter-362m: FMV WMV3 extradata clobber fix (SR_XMV_EXTRA_DISJOINT). The 4-byte
// STRUCT_C extradata (a pool-11 alloc via sub_82482F80, built by sub_82493060) is
// written 4FF10A01 CORRECTLY, then the PERSISTENT 0x20 parse struct that
// sub_82486820 allocs via sub_82167F90->sub_82467F40(tag 0x248C8000) is placed
// OVERLAPPING it by the broken heap (double-grant), and its zeroing memset wipes
// the extradata -> WMV3 decoder gets a blank seq header -> 0x80600006. Proven by
// [XMV_EXTRA_WATCH] (writer sub_82486820 lr=0x8248686C). SAME allocator-overlap
// class as Wall 1 / audio Wall 1b (fake mstate 0x82110000). Real root-fix = run
// the game's real heap init; INTERIM (gated): reserve the LIVE extradata region so
// this allocator won't hand it to the overlapping scratch — retry-discard (leak the
// overlapping block, pop the next), the heap_chunk_guard pattern. No free-pairing
// change; both allocators stay intact.
#if SR_ORACLE
namespace {
struct FmvReserve {
  std::atomic<uint32_t> start{0}, end{0};
  std::atomic<uint32_t> value{0};   // the game's own written extradata (<=4B snapshot)
  std::atomic<uint32_t> vlen{0};
};
constexpr int kFmvReserveN = 16;
FmvReserve g_fmvReserve[kFmvReserveN];
std::atomic<uint32_t> g_fmvReserveIdx{0};
bool xmvExtraDisjointOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_XMV_EXTRA_DISJOINT");
    return e && e[0] == '1';
  }();
  return v;
}
bool fmvReserveOverlaps(uint32_t a, uint32_t sz) {
  if (!a || !sz) return false;
  const uint32_t aEnd = a + sz;
  for (int i = 0; i < kFmvReserveN; ++i) {
    const uint32_t s = g_fmvReserve[i].start.load(std::memory_order_relaxed);
    const uint32_t e = g_fmvReserve[i].end.load(std::memory_order_relaxed);
    if (s && a < e && aEnd > s) return true;
  }
  return false;
}
}  // namespace
namespace sr { namespace kernel {
// Called from the video-extradata write site (sub_824925D8 probe) with the buffer
// the game just filled with the real STRUCT_C. Cheap; always safe to call.
void registerFmvExtradataReserve(uint32_t va, uint32_t len) {
  if (!va || !len) return;
  const uint32_t i = g_fmvReserveIdx.fetch_add(1, std::memory_order_relaxed) % kFmvReserveN;
  const uint32_t snap = len >= 4 ? sr::kernel::readGuestU32BE(va) : 0;  // BE snapshot
  g_fmvReserve[i].start.store(va, std::memory_order_relaxed);
  g_fmvReserve[i].end.store(va + len, std::memory_order_relaxed);
  g_fmvReserve[i].value.store(snap, std::memory_order_relaxed);
  g_fmvReserve[i].vlen.store(len, std::memory_order_relaxed);
  static std::atomic<uint32_t> s_rn{0};
  uint32_t rn = s_rn.fetch_add(1, std::memory_order_relaxed) + 1;
  if (rn <= 12)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMV_EXTRA_RESERVE] #%u registered [%08X,+%X) snap=%08X slot=%u",
      rn, va, len, snap, i);
}

// iter-362m DIAGNOSTIC (SR_XMV_EXTRA_RESTORE): the WMV3 extradata buffer is written
// correctly by the game then clobbered by an allocator/block-cache overlap before the
// decoder reads it. As a force→diagnose→confirm step, restore the game's OWN captured
// bytes into exPtr right before sub_824B2E78 reads them. If the decoder then accepts
// (rc != 0x80600006) it PROVES the corruption is the sole blocker → justifies the heap
// root-fix. Returns true if a restore was applied. Repairing our-heap-induced
// corruption with the guest's own value — NOT fabricated state.
bool restoreFmvExtradataIfCorrupt(uint32_t exPtr, uint32_t exSize) {
  // Charter: this writes guest state (a diagnostic that repairs our-heap-induced
  // corruption with the guest's OWN captured bytes). Fold onto the master gate so
  // the forcing-off default never writes; run with SR_ALLOW_FORCING=1 + SR_XMV_EXTRA_RESTORE=1.
  // iter-398e: the SR_XMV_EXTRA_RESTORE sub-gate the comment promised was never
  // implemented — this was the LAST master-alone-active site in the deep-load
  // rescue bisect. Now real (default ON under master, =0 knockout).
  static const bool s_on = [] {
    const char* e = std::getenv("SR_XMV_EXTRA_RESTORE");
    return !e || e[0] != '0';
  }();
  if (!sr::kernel::forcingAllowed() || !s_on) return false;
  if (!exPtr || exSize < 4) return false;
  for (int i = 0; i < kFmvReserveN; ++i) {
    const uint32_t s = g_fmvReserve[i].start.load(std::memory_order_relaxed);
    if (s != exPtr) continue;
    const uint32_t snap = g_fmvReserve[i].value.load(std::memory_order_relaxed);
    if (!snap) return false;
    const uint32_t cur = sr::kernel::readGuestU32BE(exPtr);
    if (cur == snap) return false;   // not corrupted
    sr::kernel::writeGuestBE32(exPtr, snap);
    static std::atomic<uint32_t> s_rn{0};
    uint32_t rn = s_rn.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rn <= 12)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[XMV_EXTRA_RESTORE] #%u exPtr=%08X was=%08X -> restored game value %08X",
        rn, exPtr, cur, snap);
    return true;
  }
  return false;
}
} }  // namespace sr::kernel

#endif

// iter-364: effect-compile token-trace state (defined in cs_timeout_underflow_fix.cpp).
#if SR_ORACLE
extern std::atomic<int> g_fxLexArm;
#endif
#if SR_ORACLE
extern std::atomic<uint32_t> g_fxRegionAddr;
#endif

void sub_82467F40(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t size  = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t vbProbeLR = static_cast<uint32_t>(ctx.lr);

  // iter-364 MALLOC(0) SEMANTICS FIX (SR_MALLOC0_FIX, default ON). PROVEN root of the
  // effect-system failure: this allocator bottoms out in MmAllocatePhysicalMemoryEx
  // (sub_82468008), and MmAllocatePhysicalMemoryEx(size=0) returns NULL in our build.
  // The ID3DXEffectCompiler ctor (sub_8237D028 line 489) does
  //   v90 = malloc(a1[208]);  if (!v90) return E_OUTOFMEMORY;
  // with a1[208]==0 for these effects, and it treats the null as OOM -> every one of
  // the 66 boot effect-compiles fails (61x E_OUTOFMEMORY) -> no effect object is ever
  // built -> the FMV YUV blit (and the whole D3DX material path) can't FindTechnique.
  // C/console allocator semantics: a 0-byte request returns a VALID, unique, non-null
  // pointer (Xbox dlmalloc/Mm return a minimal chunk); code like this relies on it.
  // Platform fix = honor that: round a 0-byte bit31-clear request up to a minimal 0x10
  // block so the real allocator hands back a usable non-null pointer. Not forcing — it
  // implements the console's own allocator contract. SR_MALLOC0_FIX=0 restores the
  // broken (null) baseline for A/B.
  // ⚠️ DEFAULT-OFF (iter-365, 2026-07-23): PROVEN to REGRESS the deep-Apt menu TRUNK.
  // Bisect (5×K=6): malloc0 ON alone => FE tick DIES at 24 frames, DEF_EXEC 0/6; all four
  // iter-364 alloc fixes OFF => deep-Apt recovers (DEF_EXEC=666, movInst=451, 3178 FE frames,
  // matching the 64b86fe 2026-07-19 baseline). Root: rounding a 0-byte bit31-clear request up to
  // 0x10 makes it carve a REAL 0x10 chunk from the guest dlmalloc arena (falls through to
  // __imp__sub_82467F40 -> sub_8246CA80) instead of the console's minimal in-place return, so
  // every malloc(0) SHIFTS the dlmalloc layout and drags the deep-Apt allocations off the
  // addresses the menu's latent overlap depends on. The menu (north star) outranks the intro
  // effects, so this is OPT-IN: SR_MALLOC0_FIX=1 for effect/FMV work. NOTE the charter subtlety:
  // console dlmalloc DOES return non-null for malloc(0), so this fix is more console-correct — it
  // EXPOSES a separate latent menu bug (deep-Apt fragile to dlmalloc layout). The true both-work
  // fix = root-cause that menu overlap (todo: conflict-free malloc(0) via a disjoint tiny-stub
  // arena that never touches dlmalloc). Do NOT re-default-ON without a deep-Apt A/B.
  // ✅ iter-381 (2026-07-26): DEFAULT-ON — console semantics baked. The deep-Apt
  // A/B the note above demands was run on the post-iter-380c exe (pristine DOGMA
  // dispatch, REAL dlmalloc heap — the fragile fake-mstate arena the iter-365
  // regression depended on no longer exists): K=3 =1 → 3/3 DEEP (defExec=666,
  // movInst=451), [ALLOC_NULL] size=0 dropped 63/run → 0, and the effect
  // compiler advances past init (66x "error initializing" → distinct later-stage
  // errors; the remaining effects wall is a truncated-source class, separate
  // arc). =0 restores the broken null baseline for A/B.
  static const bool s_malloc0Fix = [] {
    const char* e = std::getenv("SR_MALLOC0_FIX");
    return !e || e[0] != '0';
  }();
  if (s_malloc0Fix && (flags & 0x80000000u) == 0 && size == 0u) {
    size = 0x10u;
    ctx.r3.u32 = 0x10u;
    static std::atomic<uint32_t> s_m0{0};
    uint32_t mn = s_m0.fetch_add(1, std::memory_order_relaxed) + 1;
    if (mn <= 6 || (mn & (mn - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[MALLOC0_FIX] #%u size=0 -> 0x10 (return valid non-null, console malloc(0) semantics) lr=%08X",
        mn, vbProbeLR);
  }

  const bool vbProbe = vbAllocProbeOn() && vbProbeLR >= 0x820F3E00u &&
                       vbProbeLR < 0x820F4010u;
  const uint32_t vbProbeSize = size, vbProbeFlags = flags;

  // iter-362 (FMV audio wall): the WMA block descriptor is a TINY (~0x14) bit31-clear
  // alloc whose last 0x10 (desc+4/+8, the block start/len get-block reads) is clobbered
  // by the next chunk's boundary tag — the Wall-1 bug, but it slips under the 0x100
  // floor. Bumping ALL tiny allocs exhausts the heap, so target by caller: only small
  // allocs from the WMA/XMV/ASF code region (0x82470000-0x824A0000). Log the callers
  // first (SR_ALLOC_WMA_PROBE) to pin the descriptor's alloc site before bumping.
  const bool wmaCaller = (vbProbeLR >= 0x82470000u && vbProbeLR < 0x824A0000u);
  {
    static const bool s_probe = [] { const char* e = std::getenv("SR_ALLOC_WMA_PROBE"); return e && e[0] == '1'; }();
    if (s_probe && (flags & 0x80000000u) == 0 && size < 0x100u && wmaCaller) {
      static std::atomic<uint32_t> s_sm{0};
      uint32_t sn = s_sm.fetch_add(1, std::memory_order_relaxed) + 1;
      if (sn <= 80)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[ALLOC_WMA] #%u size=0x%X caller lr=%08X", sn, size, vbProbeLR);
    }
  }

  // iter-362 (FMV audio wall fix, SR_WMA_ALLOC_FIX): also protect the SMALL WMA/XMV
  // allocs (< 0x100) that the 0x100 floor skips — the WMA block descriptor's last
  // 0x10 (get-block's start/len) is clobbered by the same tag bug, failing the audio
  // ASF parse at ~0xCA. The WMA code region is bounded, so bumping its small allocs
  // does NOT trigger the global tiny-alloc heap exhaustion the 0x100 floor guards.
  // Default-ON (A/B-proven, iter-362): SR_WMA_ALLOC_FIX=0 restores the broken
  // baseline. OFF => WMA block descriptor's desc+4/+8 (get-block start/len) clobbered
  // with arena ptr 0x82976AA8, audio ASF parse fails at pos 0xCA (found=0), 56 err/run.
  // ON => descriptor clean [s=0 len=0x200], parse passes through full header, 0 err.
  // ⚠️ DEFAULT-OFF (iter-365): also perturbs the shared dlmalloc bump (+0x10 on WMA/XMV allocs)
  // and contributes to the deep-Apt trunk regression (bisect: malloc0-off + FMV-three-ON never
  // reached the deep build; all-four-off did). Opt-in for FMV audio work: SR_WMA_ALLOC_FIX=1.
  static const bool s_wmaFix = [] { const char* e = std::getenv("SR_WMA_ALLOC_FIX"); return e && e[0] == '1'; }();
  // iter-362n (FMV VIDEO wall fix, SR_FMV_TINY_ALLOC_FIX): the WMV3 STRUCT_C extradata
  // is a TINY (4-byte) alloc via sub_82482F80 (LR ~0x82482Fa8, in the WMA/XMV region) —
  // TWICE: the ASF parse's copy (sub_82493060 line 230, ptr@fmt+52) AND the decoder's
  // own copy (sub_824B2E78 line 45, ptr@codec+84). BOTH get their 4 bytes zeroed by the
  // SAME dlmalloc boundary-tag overlap (writer lr=0x82469B74 -> sub_8246CA80), which is
  // why the seq header the SIMD decoder reads (=codec+84 on the WMV3 EOS path) is 0 ->
  // sub_824C9740 rc=6 -> 0x80600006 -> XMVCREATE 0x8000FFFF. PROVEN root: writing the
  // real 4FF10A01 at sub_824C4250 (after both clobbers) flips rc 6->0, XMVCREATE
  // succeeds, FMV smState 1->2->5 (iter-362n). These allocs are < 0x10 so the wmaSmall
  // floor skips them; there are only ~27 sub-0x10 allocs in the bounded WMA/XMV region,
  // so bumping them by +0x10 costs a few hundred bytes (no heap-exhaustion risk).
  // Default-ON (A/B-proven, iter-362n): OFF => WMV3 STRUCT_C (fmt+52 & codec+84) clobbered
  // to 0, sub_824B2E78 rc=0x80600006, XMVCREATE 0x8000FFFF (2/2 rolls). ON => both survive
  // (4FF10A01), rc=0, XMVCREATE rc=0 (2/2 rolls), no boot regress (present=3900). SR_FMV_TINY_ALLOC_FIX=0 restores broken baseline.
  // ⚠️ DEFAULT-OFF (iter-365): same shared-bump perturbation class as SR_WMA_ALLOC_FIX; contributes
  // to the deep-Apt trunk regression. Opt-in for FMV video work: SR_FMV_TINY_ALLOC_FIX=1.
  static const bool s_tinyFix = [] { const char* e = std::getenv("SR_FMV_TINY_ALLOC_FIX"); return e && e[0] == '1'; }();
  const bool wmaTiny = s_tinyFix && wmaCaller && size >= 4u && size < 0x10u;
  // iter-363: WMV's 0x30 bitreader is allocated through the tail trampoline with
  // lr=0x824BF3DC, just outside wmaCaller. Its callback pointer lives in the final
  // word (+0x2c): live Frida capture proved construction writes the real reader,
  // then the next chunk tag clears it before the first sub_824C4250. End-of-packet
  // refill consequently dispatches through null and turns stale stack locals into
  // cur=0x18/end=0xffffffff, making sub_824C4428 scan almost 4 GiB.
  // Default-ON after the gated K=4 run: XMVCREATE succeeded in every roll and
  // DECODE_DRAW returned repeatedly instead of parking in the first sweep.
  // SR_FMV_BITREADER_ALLOC_FIX=0 retains the captured broken baseline for A/B.
  // ⚠️ DEFAULT-OFF (iter-365): same shared-bump perturbation class; contributes to the deep-Apt
  // trunk regression. Opt-in for FMV video work: SR_FMV_BITREADER_ALLOC_FIX=1.
  static const bool s_bitReaderFix = [] {
    const char* e = std::getenv("SR_FMV_BITREADER_ALLOC_FIX");
    return e && e[0] == '1';  // DEFAULT-OFF: regresses the deep-Apt trunk
  }();
  const bool wmvBitReader =
      s_bitReaderFix && vbProbeLR == 0x824BF3DCu && size == 0x30u;
  const bool wmaSmall =
      (s_wmaFix && wmaCaller && size >= 0x10u && size < 0x100u) ||
      wmaTiny || wmvBitReader;
  if (allocFixOn() && (flags & 0x80000000u) == 0 && (size >= 0x100u || wmaSmall)) {
    ctx.r3.u32 = size + 0x10u;  // reserve the back boundary tag's 0x10
    static std::atomic<uint32_t> s_fixN{0};
    uint32_t fn = s_fixN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (fn <= 6 || (fn & (fn - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[ALLOC_FIX] #%u request 0x%X -> 0x%X (protect tail from next-chunk tag)%s",
        fn, size, size + 0x10u, wmaSmall ? " [WMA]" : "");
    size = ctx.r3.u32;
  }

  // iter-458 HR1 (Astra 071, SR_GPU_PHYS_ROUTE=1, default OFF): past the ring
  // claim, a physical (bit-31) request is NOT intercepted - the guest's own
  // path below runs (see gpuPhysRouteOn for the contract).
  // Astra 072/073: ADMISSION - one claim observation, counted under the
  // accounting lock before anything else; it classifies the call for routing
  // AND for the witness. hc settles it (under the lock) on every exit path.
  Hr1Call hc(flags);
  const bool hr1 = hc.hr1;
  if (hc.bit31) hr1Phase(1);
  // Bit 31 set → broken sub_82469B48 path; fix with guestHeapAlloc.
  // ⚠ CORRECTION (iter-458, IDA 0x82467F54/0x82467F90): the guest sends bit-31
  // SET to sub_82468008 -> MmAllocatePhysicalMemoryEx and bit-31 CLEAR to
  // sub_82469B48. This intercept serves physical requests the guest's own path
  // would take to the physical allocator.
  // iter-467 p2 fix (4), forcing-audit U1 RETIRED to a diagnostic: the ring now comes from
  // VdInitializeRingBuffer's argument (SR_VD_RING_ARG, default ON), so every bit-31 request takes the
  // guest's own path (hr1) and this heap route is never needed. It substitutes the guest's physical
  // allocator, so it is MASTER-gated: SR_VD_RING_ARG=0 SR_ALLOW_FORCING=1 restores the legacy pre-claim
  // route (and SR_GPU_PHYS_ROUTE=0 SR_ALLOW_FORCING=1 the pre-iter-458 route for every bit-31 request).
#if SR_ORACLE
  if ((flags & 0x80000000u) && !hr1 && sr::kernel::forcingAllowed()) {
    // Cap at 64 MB to guard against garbage size arguments.
    if (size > 0 && size <= 64u * 1024u * 1024u) {
      // iter-361 FLUSH-GRIND ROOT FIX (SR_GPU_VB_DISJOINT, default ON): SMALL
      // bit31-set GPU allocs (e.g. D3DVertexBufferCreate's 80-byte vertex data)
      // served from the primary guestHeapAlloc collide with the __imp__ pool that
      // holds the paired 32-byte VB HEADER (proven: data@0x8FFBE730 overlaps
      // header@0x8FFBE720 by 0x10, so the vertex memcpy scribbles vb[5]=0x3F800000
      // -> flush-range underflow -> ~88s dcbf grind). The two allocators are
      // uncoordinated (heap grows up into the pool's live region). Route SMALL
      // GPU allocs to guestPhysAlloc — the SAME physical arena the header pool is
      // carved from, but a FRESH page-aligned block (tracked by g_physNext, so
      // disjoint from every prior grant incl. the pool's block) and firmly OUTSIDE
      // the image (>= 0x84000000). Large allocs (ring buffer etc.) stay on
      // guestHeapAlloc (that path works + the ring-VA detection depends on it).
      static const bool s_vbDisjoint = [] {
        const char* e = std::getenv("SR_GPU_VB_DISJOINT");
        return !e || e[0] != '0';
      }();
      // iter-361 GENERAL COORD FIX: the __imp__ pool that holds resource HEADERS
      // is a third allocator UNTRACKED by the heap<->phys cross-check (proven via
      // [ALLOC_COORD]: physLowVA=0xAF000000 while headers live at 0x8FFBxxxx), so
      // any bit31-set resource DATA served from guestHeapAlloc can overlap a pool
      // header (VB grind; the residual TEXTURE grinds from sub_8260A590 are the
      // same class). guestPhysAlloc grants at 0xAExxxxxx — FAR from the collision
      // zone (~500MB headroom) — so route resource DATA there.
      //   * SR_GPU_VB_DISJOINT (default ON, PROVEN): small (<0x1000) allocs — the
      //     VB-data class; A/B-proven to kill the 0x8FFBE720 grind with 0 regress.
      //   * SR_GPU_RES_DISJOINT (default ON, PROVEN — FRONTIER ADVANCE): extend to
      //     <=256KB (covers textures) once the ring VA is up. The texture/resource
      //     overlap didn't just cause grinds — it CORRUPTED the render path and
      //     BLOCKED deep Apt instantiation. A/B (2×K=6 vs default): extension ON =>
      //     2-3/6 rolls reach DEEP Apt (DEF_EXEC=666+, REAL MOVIE INSTANCED,
      //     2700-3800 FE frames); default OFF => 0/6 deep. So fixing the overlap
      //     lets the game's OWN code reach deeper. KNOWN follow-up (non-fatal): ~1/6
      //     roll logs a recoverable guard-page 0x80000001 mid-render (process keeps
      //     running) + a residual grind at ctr=0x823ACE00; both non-blocking.
      static const bool s_resDisjoint = [] {
        const char* e = std::getenv("SR_GPU_RES_DISJOINT");
        return !e || e[0] != '0';
      }();
      uint32_t addr = 0;
      const bool ringUp = sr::kernel::g_gpuRingAllocVA.load(std::memory_order_relaxed) != 0;
      // iter-458 (Astra 027): under SR_PHYS_FREE, once the ring is up, EVERY
      // eligible physical request is served by the physical allocator - the
      // 0x8-window heap route aliased arena physical addresses (pf1: pins on
      // heap-resident resolve products carved and fragmented the arena until
      // the world pool could not fit). Ring initialisation keeps its path.
      // iter-460 (SR_GPU_LARGE_PHYS, default OFF): larger post-ring GPU requests
      // too. Served from the 0x8 heap, their physical address (va & 0x1FFFFFFF)
      // collides with the 224 MB world pool's, and the GPU cannot tell the two
      // objects apart ([GPU_READ_WIN] legacy(both): 17% of GPU reads).
      static const bool s_largePhys = [] {
        const char* e = std::getenv("SR_GPU_LARGE_PHYS");
        return e && e[0] == '1';
      }();
      const bool routePhys =
          (s_vbDisjoint && size < 0x1000u) ||
          (s_resDisjoint && size <= 0x40000u && ringUp) ||
          (s_largePhys && ringUp) ||
          (ringUp && sr::kernel::physalloc::physFreeEnabled());
      if (routePhys) {
        // iter-449B PLATFORM FIX: honor the XALLOC attrs' alignment nibble
        // (bits 24-27, guest formula 1 << (HIBYTE(attrs) & 0xF), default
        // class 0x0C => 4096) on the disjoint route too. The D3DX effect
        // compiler's 4KB pool pages arrive HERE (bit31-set, size 0x1000,
        // ring up) and find their pool header via *(obj & 0xFFFFF000) —
        // a 16B-aligned grant broke the whole new-backend HLSL compile
        // (offset-returning chunk allocator -> X4800 cascade -> black FE).
        uint32_t attrs = flags;
        if ((attrs & 0xF000000u) == 0u)
          attrs = (attrs & 0xF0FFFFFFu) | 0x0C000000u;
        uint32_t palign = 1u << ((attrs >> 24) & 0xFu);
        if (palign < 16u) palign = 16u;
        {
          sr::kernel::PhysKindScope pk(sr::kernel::PhysKind::kGpuRoute);   // iter-465 [PHYS_PINS]
          addr = sr::kernel::guestPhysAlloc(size, palign);
        }
        // iter-458 (Astra 043): a genuine GPU physical grant - the precise owner model.
        if (addr) sr::kernel::physwin::notePhysGrant(addr, (uint32_t)size);
#if SR_ORACLE
        // mem.resources memres5 (SR_MEM_RESOURCES=census): the grant's pages' last-grant window (the registry's window)
        if (addr && sr::gfx::memres::census()) sr::gfx::memres::noteGrantWindow(addr, (uint32_t)size);
#endif
        if (addr && sr::gfx::memres::resourceMode())
          sr::gfx::memres::resourceMemory().noteGrant(addr, (uint32_t)size);
        static std::atomic<uint32_t> s_dj{0};
        uint32_t dn = s_dj.fetch_add(1, std::memory_order_relaxed) + 1;
        if (addr && (dn <= 8 || (dn & (dn - 1)) == 0))
          SR_LOG_BOOT(sr::LogSeverity::Info,
            "[GPU_VB_DISJOINT] #%u GPU resource alloc size=0x%X align=0x%X -> phys 0x%08X (off the pool-colliding heap)",
            dn, size, palign, addr);
      }
      // iter-458 H-FREE: under SR_PHYS_FREE a physical-route request that the
      // physical allocator cannot satisfy must NOT leak into the 0x8-window heap
      // (it would re-create the masked-address ambiguity); it falls through to
      // the original, whose MmAllocatePhysicalMemoryEx fails as the console would.
      const bool noHeapFallback = routePhys && sr::kernel::physalloc::physFreeEnabled();
      if (addr == 0 && !noHeapFallback) {
        addr = sr::kernel::guestHeapAlloc(size);
        // iter-458 H-WIN: a PHYSICAL request satisfied from the main heap -
        // GPU-visible memory in the 0x8 window. Recorded, never altered.
        if (addr) sr::kernel::physwin::noteHeapRoutedGrant(addr, (uint32_t)size);
#if SR_ORACLE
        // mem.resources memres5 (SR_MEM_RESOURCES=census): a heap-routed grant - its pages' last-grant window is 0x8
        if (addr && sr::gfx::memres::census()) sr::gfx::memres::noteGrantWindow(addr, (uint32_t)size);
#endif
        if (addr && sr::gfx::memres::resourceMode())
          sr::gfx::memres::resourceMemory().noteGrant(addr, (uint32_t)size);
        // iter-462: under the physical route the heap-routed grants are the
        // pre-claim ones (the ring); keep the physical allocator off their pages.
        if (addr && gpuPhysRouteOn()) sr::kernel::guestPhysReserveAlias(addr, (uint32_t)size);
        // HR1's gate-independent witness (the pair-12 source), settled by hc
        // from the ADMISSION observation: admitted after the claim, or
        // SPANNING it (admitted before, routed after - routing re-reads ringUp).
        if (addr) {
          hc.heapRouted = true;
          hc.ringUpAtRoute = ringUp;
        }
      }
#if SR_ORACLE
      // iter-364: catch the effect-compiler region clobber HERE — a bit31-set grant that
      // overlaps the live +104 region, whose memset (below) zeroes the region's counter.
      if (g_fxLexArm.load(std::memory_order_relaxed) != 0) {
        uint32_t reg = g_fxRegionAddr.load(std::memory_order_relaxed);
        if (reg && addr && addr <= reg && addr + (uint32_t)size > reg)
          SR_LOG_BOOT(sr::LogSeverity::Error,
            "[FXCLOB2] bit31-set grant size=0x%X flags=0x%08X lr=0x%08X -> 0x%08X routePhys=%d ringUp=%d  <<<< HITS REGION %08X",
            (uint32_t)size, flags, vbProbeLR, addr, (int)routePhys, (int)ringUp, reg);
      }
#endif
      if (addr != 0) {
        if (uint8_t* h = sr::kernel::guestVAToHost(addr)) std::memset(h, 0, size);

        // Store first allocation as the ring buffer VA; also backfill g_ringBufBase which
        // VdInitializeRingBuffer set to fake (0xD0010000) because it fired before this alloc.
        uint32_t expected = 0;
        if (sr::kernel::g_gpuRingAllocVA.compare_exchange_strong(expected, addr, std::memory_order_release))
          sr::kernel::trySetRingBufBaseFromAlloc(addr);

        static std::atomic<uint32_t> s_count{0};
        uint32_t n = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n & (n - 1)) == 0) {
          SR_LOG_BOOT(sr::LogSeverity::Info,
            "[GPU_RING_ALLOC] sub_82467F40 override #%u: size=0x%X flags=0x%08X -> 0x%08X",
            n, size, flags, addr);
        }
        if (vbProbe)
          SR_LOG_BOOT(sr::LogSeverity::Error,
            "[VBALLOC] req=0x%X flags=0x%08X lr=0x%08X -> guestHeapAlloc -> 0x%08X",
            vbProbeSize, vbProbeFlags, vbProbeLR, addr);
        ctx.r3.u32 = addr;
        return;
      }
    }
    // Size out of range or heap exhausted — log and fall through to original.
    static std::atomic<uint32_t> s_fail{0};
    uint32_t nf = s_fail.fetch_add(1, std::memory_order_relaxed) + 1;
    if (nf <= 4) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[GPU_RING_ALLOC] sub_82467F40 override fallback: size=0x%X flags=0x%08X", size, flags);
    }
  }

#endif  // SR_ORACLE: retired pre-claim heap substitution

  // iter-357l: the ASF walk's reader back-pointer at parser+0x260 goes NULL
  // between sub_82486820's entry and its first read. The only steps in that
  // window are THIS allocator call and a 32-byte zero loop. Sample the field
  // across the allocator: valid->0 here means the allocator (or a callee)
  // scribbles it; unchanged means the zero loop / codegen is the writer.
  // NOTE: the parser is 0x270 bytes (Function_8247C590), so a block at
  // parser+0x280 is legally ADJACENT — an earlier version of this probe used a
  // guessed 0x2A8 range and cried "double grant" on a healthy allocation.
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
#if SR_ORACLE
  const uint32_t parser = g_xmvParser.load(std::memory_order_relaxed);
  const uint32_t pre =
      parser ? sr::kernel::readGuestU32BE(parser + 0x260u) : 0;
#endif
  // Capture the (possibly bumped) args so a reserved-overlap retry can re-issue.
  const uint32_t sr3 = ctx.r3.u32, sr4 = ctx.r4.u32, sr5 = ctx.r5.u32,
                 sr6 = ctx.r6.u32, sr7 = ctx.r7.u32;
  if (hr1) {
    // The delegation point. `started` was counted at ADMISSION, so a call
    // anywhere between admission and settlement (including inside allocation,
    // zero-fill or error handling) is outstanding at the capture, not missing.
    hc.size = sr3;
    hc.lr = lr;
    hr1Phase(2);
  }
  __imp__sub_82467F40(ctx, base);
  if (hr1) {
    // The guest's own outcome (it has already recorded its own error,
    // SetLastError(8), on a null); hc settles it under the accounting lock.
    hc.delegatedDone = true;
    hc.result = ctx.r3.u32;
  }
#if SR_ORACLE
  // iter-362m: FMV extradata overlap guard. If this bit31-clear alloc landed on a
  // LIVE WMV3 extradata buffer, discard (leak) it and retry so the overlapping
  // scratch gets a different address. Bounded 8 tries; only when SR_XMV_EXTRA_DISJOINT.
  if (xmvExtraDisjointOn() && (flags & 0x80000000u) == 0) {
    for (int tries = 0; tries < 8 && ctx.r3.u32 &&
                        fmvReserveOverlaps(ctx.r3.u32, size); ++tries) {
      static std::atomic<uint32_t> s_rej{0};
      const uint32_t rn = s_rej.fetch_add(1, std::memory_order_relaxed) + 1;
      if (rn <= 16)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[XMV_EXTRA_DISJOINT] alloc 0x%08X (size=0x%X lr=0x%08X) overlaps live FMV "
          "extradata — discarding, retry #%u", ctx.r3.u32, size, lr, rn);
      ctx.r3.u32 = sr3; ctx.r4.u32 = sr4; ctx.r5.u32 = sr5;
      ctx.r6.u32 = sr6; ctx.r7.u32 = sr7;
      __imp__sub_82467F40(ctx, base);
    }
  }

#endif
  // iter-364 CENTRAL HEAP COORDINATION (SR_DLMALLOC_FENCE). Fixes the D3DX effect-compile /
  // FMV overlap (0/66->33/33): the bit31-clear guest dlmalloc (sub_82469B48 -> sub_8246CA80)
  // carves chunks in guestHeapAlloc's forward-bump window UNTRACKED by g_heapNext, so the bump's
  // zero-fill clobbers a live chunk (the effect +104 region [FXCLOB3]). This raises g_heapNext
  // past every such chunk so grants land ABOVE the guest heap.
  //   ⚠️ DEFAULT-OFF (iter-364, 2026-07-23): PROVEN to REGRESS the deep-Apt menu TRUNK. K=6 A/B:
  //   fence ON => FE master tick FREEZES at 2-7 (6/6 rolls, defExec 0/18); fence OFF => FE tick
  //   recovers to 366-389 (2/6 deep-roll pattern). Advancing g_heapNext perturbs the allocator
  //   GEOMETRY the deep-Apt fix (SR_GPU_RES_DISJOINT) depends on, re-introducing the corruption
  //   that starves the ResourceScheduler/FE loop. The menu trunk is the north star and outranks the
  //   intro effects, so coordination is OPT-IN: SR_DLMALLOC_FENCE=1 for FMV/effect work. Root fix
  //   pending = a coordination that fixes effects WITHOUT moving g_heapNext (targeted to the effect
  //   region), + the ResourceScheduler CS livelock. Do NOT re-default-ON without a deep-Apt A/B.
  {
    static const bool s_fence = [] {
      const char* e = std::getenv("SR_DLMALLOC_FENCE");
      return e && e[0] == '1';  // DEFAULT-OFF: regresses the deep-Apt trunk (see above)
    }();
    const uint32_t res = ctx.r3.u32;
    if (s_fence && (flags & 0x80000000u) == 0 && res != 0)
      sr::kernel::guestHeapReserveThrough(res + size);
  }

  if (vbProbe)
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[VBALLOC] req=0x%X flags=0x%08X lr=0x%08X -> __imp__(pool) -> 0x%08X",
      vbProbeSize, vbProbeFlags, vbProbeLR, ctx.r3.u32);

#if SR_ORACLE
  // iter-364: confirm the effect-compiler +104 region-object clobber. During the armed
  // fallback compile (g_fxLexArm), log allocs whose result is on the region's page — this
  // catches (a) the region's OWN alloc (result == g_fxRegionAddr; names its size+caller LR
  // to scope a protect) and (b) the double-grant that re-issues an overlapping chunk. Gated
  // behind SR_PASS_TRACE via g_fxLexArm; zero cost otherwise.
  if (g_fxLexArm.load(std::memory_order_relaxed) != 0) {
    const uint32_t res = ctx.r3.u32;
    const uint32_t reg = g_fxRegionAddr.load(std::memory_order_relaxed);
    const bool nearReg = reg && res && res >= (reg & ~0xFFFu) && res < ((reg & ~0xFFFu) + 0x1000u);
    static std::atomic<uint32_t> s_fa{0};
    uint32_t fn = s_fa.fetch_add(1, std::memory_order_relaxed) + 1;
    if (nearReg || fn <= 60) {
      const char* mark = (reg && res && res <= reg && res + size > reg) ? "  <<<< OVERLAPS REGION" : "";
      SR_LOG_BOOT((mark[0] ? sr::LogSeverity::Error : sr::LogSeverity::Info),
        "[FXALLOC] #%u size=0x%X flags=0x%08X lr=0x%08X -> 0x%08X reg=%08X%s",
        fn, size, flags, (uint32_t)ctx.lr, res, reg, mark);
    }
  }
#endif
#if SR_ORACLE
  {
    const uint32_t res = ctx.r3.u32;
    // iter-360d: hunt the audio-decoder-create failure. The audio setsrc
    // (sub_824743A0) fails 0x8000FFFF only when Function_8247ACB8 returns NULL,
    // which reduces ENTIRELY to one of its pool allocs (0x7f8/0x290/0x20/0x284/
    // 0x30/0x210, all bit31-clear via Function_82482F80) returning NULL from
    // this allocator. So: log EVERY bit31-clear NULL return with size+lr. If we
    // see NULLs in the FMV window, the bounded DOGMA heap is exhausted (and the
    // ALLOC_FIX +0x10 bump is a contributor) -> the real fix is the sub_8246CA80
    // tag-write root-fix (no bump, no heap cost), not a bigger bump.
    if (res == 0 && (flags & 0x80000000u) == 0) {
      static std::atomic<uint32_t> s_null{0};
      uint32_t nn = s_null.fetch_add(1, std::memory_order_relaxed) + 1;
      if (nn <= 64)
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[ALLOC_NULL] #%u sub_82467F40 returned NULL: size=0x%X flags=0x%08X lr=0x%08X (heap exhausted?)",
          nn, size, flags, lr);
    }
    const uint32_t post =
        parser ? sr::kernel::readGuestU32BE(parser + 0x260u) : 0;
    const bool inParser = parser && res && res >= parser && res < parser + 0x270u;
    const bool cleared = parser && pre && !post;
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cleared || inParser || n <= 16) {
      SR_LOG_BOOT((cleared || inParser) ? sr::LogSeverity::Error
                                        : sr::LogSeverity::Info,
        "[XMV_ALLOC] #%u size=0x%X flags=0x%08X lr=0x%08X -> 0x%08X | "
        "parser=0x%08X end=0x%08X +260: %08X -> %08X%s%s",
        n, size, flags, lr, res, parser, parser ? parser + 0x270u : 0, pre,
        post, inParser ? "  <<<< BLOCK INSIDE LIVE PARSER" : "",
        cleared ? "  <<<< ALLOCATOR CLEARED +260" : "");
    }
    // iter-360: once the heap has served plenty of allocs (definitely live +
    // past early boot), fire the deterministic isolation/watch probe ONCE.
    // iter-463 charter: both probes are forcing; the launch is master-gated
    // (the SR_ALLOC_PROBE / SR_ALLOC_WATCH predicates also require it).
    if (n == 800 && sr::kernel::forcingAllowed()) {
      if (allocWatchOn() || allocProbeOn()) {
        static std::atomic<bool> s_launched{false};
        if (!s_launched.exchange(true))
          std::thread([] {
            if (allocWatchOn()) runAllocWatchProbe();
            else runAllocIsolationProbe();
          }).detach();
      }
    }
  }
#endif
}
