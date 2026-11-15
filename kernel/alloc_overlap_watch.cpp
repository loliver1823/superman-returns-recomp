// iter-188: allocation overlap catcher.
//
// Frida hardware watchpoints proved the FM module structs (modD..modA at
// 0x8A24F038..0x8A24F2F8, allocated during sub_8218CFE8's writer chain) get
// overwritten by a growing 8-byte-pair array (insert via sub_82762A08, chain
// sub_8276A178 -> sub_8276A0B0 -> sub_82762E78 -> sub_82762B10) whose backing
// buffer the game's pool allocator placed ON TOP of the module structs.
// All host-side granters (guestHeapAlloc / guestPhysAlloc / NtAlloc stubs)
// are disjoint by construction, so the double-grant happens inside the
// game's own pool (sub_82709470) or the heap glue above it (sub_82164818).
//
// These strong overrides log every allocation whose [result, result+size)
// intersects the module-struct zone, with the pool pointer and caller LR,
// so we can see BOTH grants and adjudicate which allocator mis-accounts.
//
// Observations plus the native present gate below (SR_NATIVE_GATE, default ON since the transport bake).

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "sr_oracle.hpp"   // C1: the present-gate CHECK below compiles out under SR_ORACLE=0
#include "../app/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include "native_brackets.hpp"
#include "native_flush.hpp"
#include "r1_hooks.hpp"  // R1 (docs/SPLICE_CONVERSION.md): ports of the old splice bodies

extern "C" unsigned long __stdcall GetCurrentThreadId(void);

namespace sr::native::flush {
// Keep the zero-argument gate next to its override for registry discovery. SR_NATIVE_GATE (the vblank gate 82111EC8 as
// kind 17): default ON since the transport bake (native/transport-bake, 2026-10-15); =0 keeps the guest body and the A/B.
bool presentGateOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_GATE");  // `e`: scripts/audit_forcing.py reads `!e ||` as default ON
    return !e || e[0] != '0';
  }();
  return on;
}
}

#if SR_ORACLE
// Oracle-only diagnostic observers and forcing experiments. The native release
// keeps only the present gate and host longjmp behavior below.
namespace {

constexpr uint32_t kZoneLo = 0x8A24E000u;
constexpr uint32_t kZoneHi = 0x8A250000u;

// iter-293: the Apt ActionScript-VM global zone (loaded XEX .data). Golden-diff
// (WinDbg data/xenia_canary.DMP vs live Frida) proved these are CREATED then
// CLOBBERED on our boot: resolver pools DAT_829a0aa8/0ad8, resolver root scope
// DAT_829a09e0/0ab8, and the init guards DAT_829a1928/199c all live here. The
// growing 8-byte-pair array insert (sub_82762A08) writes over this zone because
// its backing buffer was mis-placed into the image .data — killing the Apt VM
// (no heap/root => no movie resolves, no _global.FlowMgr, no menu).
constexpr uint32_t kAptLo = 0x829A0000u;
constexpr uint32_t kAptHi = 0x829A2000u;
// The loaded XEX image .data span — a legitimate growing-array backing buffer
// is ALWAYS a heap block (>= image end); a non-null buffer inside the image
// means the container is corrupt.
constexpr uint32_t kImageLo = 0x82000000u;
constexpr uint32_t kImageHi = 0x82B00000u;

inline void checkOverlap(const char* tag, uint32_t result, uint32_t size,
                         uint32_t poolOrCtx, uint32_t callerLR) {
  if (result == 0) return;
  // Treat absurd sizes as 4 bytes for the range test so garbage doesn't
  // false-positive the whole address space.
  uint64_t sz = (size > 0 && size <= 0x10000000u) ? size : 4u;
  if (result < kZoneHi && result + sz > kZoneLo) {
    static unsigned s_n = 0;
    if (++s_n <= 40) {
      fprintf(stderr,
        "[ALLOC_OVERLAP] %s -> [0x%08X,+0x%llX) covers module-struct zone "
        "(pool/ctx=0x%08X callerLR=0x%08X hit#%u)\n",
        tag, result, (unsigned long long)sz, poolOrCtx, callerLR, s_n);
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[ALLOC_OVERLAP] %s -> [0x%08X,+0x%X) covers module-struct zone "
        "(pool/ctx=0x%08X callerLR=0x%08X hit#%u)",
        tag, result, (uint32_t)sz, poolOrCtx, callerLR, s_n);
    }
  }
}

}  // namespace

extern "C" void __imp__sub_82709470(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82164818(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82762A08(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8276A178(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8278D258(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82182700(PPCContext& __restrict ctx, uint8_t* base);  // iter-343d: 2nd allocator front door
extern "C" void __imp__sub_82762E78(PPCContext& __restrict ctx, uint8_t* base);  // iter-343e: AS-VM execute-action entry
extern "C" void __imp__sub_82762DA8(PPCContext& __restrict ctx, uint8_t* base);  // iter-343f: AS-VM result-store (the actual writer)
extern "C" void __imp__sub_825F6980(PPCContext& __restrict ctx, uint8_t* base);  // iter-343g: CRT memcpy (RIP-inversion-proven writer)
extern "C" void __imp__sub_825F4640(PPCContext& __restrict ctx, uint8_t* base);  // iter-343g: CRT memset-family (2nd proven writer)
extern "C" void __imp__sub_82187548(PPCContext& __restrict ctx, uint8_t* base);  // iter-350g: gameconfig getString
extern "C" void __imp__sub_8229F460(PPCContext& __restrict ctx, uint8_t* base);  // iter-344b: video-out subsystem init (creates the D3D allocator DAT_8297AF08)
extern "C" void __imp__sub_825FF9F8(PPCContext& __restrict ctx, uint8_t* base);  // iter-344c: tri-buffer resize (the buffer VAs' provenance point)
extern "C" void __imp__sub_822AB748(PPCContext& __restrict ctx, uint8_t* base);  // iter-345: PSM dispatcher (sole caller of the splash advance)
extern "C" void __imp__sub_8271F848(PPCContext& __restrict ctx, uint8_t* base);  // iter-345b: per-frame master dispatch (posted job — the callback-chain walker)
extern "C" void __imp__sub_82110840(PPCContext& __restrict ctx, uint8_t* base);  // iter-346: pump bottom-half (suspected cmd-0x2C poster)
extern "C" void __imp__sub_827206B8(PPCContext& __restrict ctx, uint8_t* base);  // iter-362n: GameKernelLoop per-manager time gate
extern "C" void __imp__sub_82720C60(PPCContext& __restrict ctx, uint8_t* base);  // iter-346b: GameKernelLoop_TickOnce (vt[9], time-gated)
extern "C" void __imp__sub_820F0000(PPCContext& __restrict ctx, uint8_t* base);  // iter-347b: reverse array-walk invoker (harness boot trampoline — benign)
extern "C" void __imp__sub_82604190(PPCContext& __restrict ctx, uint8_t* base);  // iter-347d: tri-buffer copy-assign (the VEH-proven wipe writer)
extern "C" void __imp__sub_821938E0(PPCContext& __restrict ctx, uint8_t* base);  // iter-348: REAL allocator-create (vt[1] of 0x8209A448) — the descriptor-wipe zeroer
extern "C" void __imp__sub_8263CCD0(PPCContext& __restrict ctx, uint8_t* base);  // iter-348e: producer-side tri-buffer writer (resize caller)
extern "C" void __imp__sub_8263CF70(PPCContext& __restrict ctx, uint8_t* base);  // iter-348e: producer-side tri-buffer writer (resize caller)
extern "C" void __imp__sub_8263C750(PPCContext& __restrict ctx, uint8_t* base);  // iter-348e: producer-side tri-buffer writer (resize caller)
extern "C" void __imp__sub_8271ED68(PPCContext& __restrict ctx, uint8_t* base);  // iter-346b: ServiceTick_RunMain (per-system dispatch)
extern "C" void __imp__sub_82112050(PPCContext& __restrict ctx, uint8_t* base);  // iter-346: swap processor
// (drain sub_828EEBDC is inlined by the recomp — no symbol to hook)

namespace {

// iter-188 finding (INSERT_WATCH): the error-report chain
// sub_8276A178 -> ... -> sub_82762A08 runs with `this` = NULL or garbage
// (0x00000000, 0x12C00545, ...) because the error-subsystem object is never
// constructed in our environment. The insert then scribbles relative to the
// bogus container, which is what was destroying the FM module structs at
// 0x8A24F038..0x8A24F2F8 (caught by Frida hardware watchpoints).
inline bool plausibleGuestObj(uint32_t va) {
  return va >= 0x82000000u && va < 0xBF000000u && (va & 1u) == 0u &&
         (va >> 24) != 0xFFu;
}

// Scratch sink returned when we suppress a garbage-`this` insert: callers
// immediately read/write a few fields of the returned element, so give them
// a dedicated 64-byte guest scratch block instead of address 0.
uint32_t errorSinkVA() {
  static uint32_t s_va = 0;
  if (s_va == 0) {
    s_va = sr::kernel::guestHeapAlloc(64);
  }
  return s_va;
}

}  // namespace

// fix/postgame-apt (lane K): SR_DAS_WITNESS=1 reports NULL pool grants and failed Lua reallocs
// (kernel/das_tree_witness.cpp; observe-only, called after the guest body).
namespace sr::daswitness {
void poolAllocNull(const uint8_t* base, uint32_t pool, uint32_t size, uint32_t lr);
void luaReallocNull(const uint8_t* base, uint32_t block, uint32_t oldSize, uint32_t newSize, uint32_t lr,
                    unsigned long long liveBytes, unsigned liveBlocks);
}

// Pool allocator: r3 = pool, r4 = size, returns r3 = block.
PPC_FUNC(sub_82709470) {
  uint32_t pool = ctx.r3.u32;
  uint32_t size = ctx.r4.u32;
  uint32_t callerLR = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82709470(ctx, base);
  if (ctx.r3.u32 == 0 && size != 0) sr::daswitness::poolAllocNull(base, pool, size, callerLR);
  // iter-195b: extend SR_DOGMA_HOST_ALLOC to the pool entry too — residual
  // corruption rolls (globals churn) come from double-grants on paths that
  // bypass sub_8278D258. Same policy: serve every grant from the host bump.
  {
    static const bool s_hostAll = [] {
      const char* e = std::getenv("SR_DOGMA_HOST_ALLOC");
      return e && e[0] == '1';
    }();
    if (s_hostAll && size > 0 && size <= 0x1000000u) {
      uint32_t repl = sr::kernel::guestHeapAlloc(size);
      if (!repl) repl = sr::kernel::guestHeapAllocFallback(size);
      if (repl) ctx.r3.u32 = repl;
    }
  }
  checkOverlap("sub_82709470(pool)", ctx.r3.u32, size, pool, callerLR);
}

// iter-357i: live-block tracker for the Lua/DOGMA heap pair
// (sub_82164818 realloc / sub_82164898 sized-free). Once script compiles
// started succeeding, DogmaHeapFree began spinning forever; repeated
// same-ptr frees appeared but could be legit block reuse. This tracker
// decides: a free of a tombstoned (already-freed, never-realloced) block is
// a REAL double free — logged loudly with the guest back-chain.
namespace sr_alloc_track {
std::mutex g_m;
std::unordered_map<uint32_t, uint32_t> g_live;   // ptr -> size
std::unordered_map<uint32_t, uint32_t> g_dead;   // ptr -> tombstone
void onAlloc(uint32_t oldPtr, uint32_t res, uint32_t sz) {
  std::lock_guard<std::mutex> l(g_m);
  if (res == 0) return;  // failed realloc: old block still live
  if (oldPtr && oldPtr != res) { g_live.erase(oldPtr); g_dead[oldPtr] = 1; }
  g_live[res] = sz;
  g_dead.erase(res);
}
// Live Lua-heap totals (lane K: the LUA_REALLOC_NULL report; only called on a failure).
void liveTotals(unsigned long long& bytes, unsigned& blocks) {
  std::lock_guard<std::mutex> l(g_m);
  bytes = 0;
  for (const auto& kv : g_live) bytes += kv.second;
  blocks = static_cast<unsigned>(g_live.size());
}
// 0 = was live (ok), 1 = tombstoned = DOUBLE FREE, 2 = unknown block
int onFree(uint32_t ptr) {
  std::lock_guard<std::mutex> l(g_m);
  if (g_live.erase(ptr)) { g_dead[ptr] = 1; return 0; }
  if (g_dead.count(ptr)) return 1;
  return 2;
}
}  // namespace sr_alloc_track

// Heap glue (realloc path): r3 = old, r4 = oldSize?, r5 = newSize, returns r3.
PPC_FUNC(sub_82164818) {
  uint32_t oldPtr = ctx.r3.u32;
  uint32_t oldSize = ctx.r4.u32;
  uint32_t newSize = ctx.r5.u32;
  uint32_t callerLR = static_cast<uint32_t>(ctx.lr);
  // Phase 4.3 batch Lua (docs/FORCING_RETIREMENT.md): the SR_REALLOC_GUARD entry refusal is
  // retired; the generated body runs for every call, as on the console.
  __imp__sub_82164818(ctx, base);
  if (ctx.r3.u32 == 0 && newSize != 0) {
    unsigned long long liveBytes = 0;
    unsigned liveBlocks = 0;
    sr_alloc_track::liveTotals(liveBytes, liveBlocks);
    sr::daswitness::luaReallocNull(base, oldPtr, oldSize, newSize, callerLR, liveBytes, liveBlocks);
  }
  sr_alloc_track::onAlloc(oldPtr, ctx.r3.u32, newSize);
  checkOverlap("sub_82164818(heap)", ctx.r3.u32, newSize, oldPtr, callerLR);
}

// Growing-array insert (the Frida-identified module-struct writer).
// r3 = container, r4 = sort key/position. Container fields (BE):
// +8 tail, +24 bufEnd, +28 bufStart, +32 count. Frida showed its element
// shift/store writes covering modX+4 — log the container state to learn
// whether bufStart is a mis-granted block or a corrupted pointer.
PPC_FUNC(sub_82762A08) {
  uint32_t c = ctx.r3.u32;
  uint32_t key = ctx.r4.u32;
  uint32_t callerLR = static_cast<uint32_t>(ctx.lr);
  // Guard: garbage container -> skip the insert entirely (it's error-record
  // bookkeeping; losing it is harmless, scribbling over live heap is not).
  // iter-463 charter (burndown U1): master-gated. "[INSERT_GUARD]
  // sub_82762A08" and "[APTZONE_GUARD]" are in 0/150 run logs (81 before the
  // heap-corruption fix 60a90bf, 69 after). Default exe: log, then run the
  // guest's own insert (the watch below reads only a plausible container).
  if (!plausibleGuestObj(c)) {
    static unsigned s_rej = 0;
    const bool skip = sr::kernel::forcingAllowed();
    if (++s_rej <= 12 || (s_rej & (s_rej - 1)) == 0) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[INSERT_GUARD] sub_82762A08 %s garbage container 0x%08X "
        "(lr=0x%08X rej#%u)", skip ? "rejected" : "OBSERVED (not rejected: SR_ALLOW_FORCING off)",
        c, callerLR, s_rej);
    }
    if (!skip) {
      __imp__sub_82762A08(ctx, base);
      return;
    }
    ctx.r3.u32 = errorSinkVA();
    return;
  }
  auto rd = [&](uint32_t va) -> uint32_t {
    uint32_t a = va;
    const uint8_t* p = base + a;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  };
  uint32_t tail = rd(c + 8), bufEnd = rd(c + 24), bufStart = rd(c + 28),
           count = rd(c + 32);
  // iter-293: THE Apt-VM clobber fix (golden-diff proven, unifies with iter-292e).
  // If the insert's backing buffer (or its write cursor `tail`) lands in the
  // loaded XEX .data — in particular the Apt-VM zone [kAptLo,kAptHi) holding the
  // resolver pools/roots — the container is corrupt and this insert would
  // SCRIBBLE the Apt ActionScript VM (the created-then-clobbered pools/roots the
  // golden diff pinned). A real growing-array buffer is a heap block; a non-null
  // buffer inside the image .data can never be legitimate. Skip the insert (the
  // record is harmless error bookkeeping) to preserve the Apt globals.
  {
    auto inImage = [](uint32_t va) {
      return va >= kImageLo && va < kImageHi;
    };
    bool bufOverApt = (bufStart != 0 && bufStart < kAptHi && bufEnd > kAptLo);
    bool bufInImage = inImage(bufStart);       // non-null image-.data buffer
    bool tailInImage = inImage(tail);          // write cursor in image .data
    if ((bufOverApt || bufInImage || tailInImage) && !sr::kernel::forcingAllowed()) {
      static unsigned s_aptObs = 0;
      if (++s_aptObs <= 24 || (s_aptObs & (s_aptObs - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[APTZONE_GUARD] sub_82762A08 insert into Apt-VM/.data zone OBSERVED (not "
          "skipped: SR_ALLOW_FORCING off) c=0x%08X buf=[0x%08X,0x%08X) tail=0x%08X lr=0x%08X #%u",
          c, bufStart, bufEnd, tail, callerLR, s_aptObs);
    } else if ((bufOverApt || bufInImage || tailInImage) && sr::kernel::forcingAllowed()) {
      static unsigned s_aptRej = 0;
      if (++s_aptRej <= 24 || (s_aptRej & (s_aptRej - 1)) == 0) {
        fprintf(stderr,
          "[APTZONE_GUARD] sub_82762A08 skipped insert into Apt-VM/.data zone "
          "c=0x%08X buf=[0x%08X,0x%08X) tail=0x%08X lr=0x%08X rej#%u\n",
          c, bufStart, bufEnd, tail, callerLR, s_aptRej);
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[APTZONE_GUARD] sub_82762A08 skipped insert into Apt-VM/.data zone "
          "c=0x%08X buf=[0x%08X,0x%08X) tail=0x%08X lr=0x%08X rej#%u",
          c, bufStart, bufEnd, tail, callerLR, s_aptRej);
      }
      ctx.r3.u32 = errorSinkVA();
      return;
    }
  }
  bool inZone = (bufStart < kZoneHi && bufEnd > kZoneLo) ||
                (tail >= kZoneLo && tail < kZoneHi) ||
                (c >= kZoneLo && c < kZoneHi);
  static unsigned s_calls = 0;
  ++s_calls;
  if (inZone || s_calls <= 12) {
    fprintf(stderr,
      "[INSERT_WATCH] sub_82762A08 #%u c=0x%08X key=0x%08X tail=0x%08X "
      "buf=[0x%08X,0x%08X) count=%u lr=0x%08X%s\n",
      s_calls, c, key, tail, bufStart, bufEnd, count, callerLR,
      inZone ? "  <<IN-ZONE>>" : "");
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[INSERT_WATCH] sub_82762A08 #%u c=0x%08X key=0x%08X tail=0x%08X "
      "buf=[0x%08X,0x%08X) count=%u lr=0x%08X%s",
      s_calls, c, key, tail, bufStart, bufEnd, count, callerLR,
      inZone ? "  <<IN-ZONE>>" : "");
  }
  __imp__sub_82762A08(ctx, base);
}

// DOGMA allocator: r3 = size (per FM_CTOR PRE/POST_8278D258 logging), returns
// r3 = block. The module structs themselves came from this allocator; FMUI's
// ctor allocates ~91k objects through it, and the second worldPtr clobber
// correlates with FMUI alloc activity — watch for grants in the zone.
// iter-343d (charter DIAG): the SECOND allocator front door. The thread
// sampler proved the game loop allocates via sub_82182700 -> sub_8278DCB8
// DIRECTLY, bypassing the sub_8278D258 hook below entirely — so an in-image
// grant on this path was invisible (and unsanitized). The .data zeroer at
// 0x8291363C is an INFLATE writing through a bogus output buffer; this hook
// catches the bogus grant AT ITS SOURCE with the requesting lr. Log-only.
// iter-347d (charter DIAG): tri-buffer copy-assign sub_82604190(dest, src) —
// VEH-proven writer of the t≈118s descriptor wipe (memcpys into
// *(dest+0x20/24/28)).
// iter-348 REFRAME: the 16-roll entry-check hunt was structurally blind — on
// the per-frame path (sub_8229AD10) dest is a ctor-ZEROED STACK TEMP, so its
// buffers are always {0,0,0} at entry; the poison happens INSIDE the call:
// resize sub_825FF9F8 gets buffers from allocator vt[1] via an sret OUT-PARAM
// on uninitialized stack. Our vt[1] CREATE (iter-344) only writes the
// out-param for desc {type==4, 0<bytes<=16MB}; any other shape (corrupt-huge
// src sizes; (size<<2) truncating to 0) falls to legacy `return 1` WITHOUT
// writing → buffer = stack garbage → resize memsets it (zeros morphology) and
// the copy memcpys it (content/shift morphology). So: log src SIZES before,
// re-read dest buffers AFTER the call, flag in-image/huge loudly.
PPC_FUNC(sub_82604190) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
  auto rd = [&](uint32_t va) -> uint32_t {
    if (va < 0x10000u) return 0;
    const uint8_t* p = base + va;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  };
  auto inImg = [](uint32_t v) { return v >= 0x82000000u && v < 0x82B00000u; };
  const uint32_t sz1 = rd(src + 0x14), sz2 = rd(src + 0x18), sz3 = rd(src + 0x1C);
  const uint32_t s1 = rd(src + 0x20), s2 = rd(src + 0x24), s3 = rd(src + 0x28);
  const uint32_t d1 = rd(dst + 0x20), d2 = rd(dst + 0x24), d3 = rd(dst + 0x28);
  // Sizes the resize will request: (sz1>>3)+1 bytes, sz2 bytes, sz3*4 bytes.
  // Anything that would fail the vt[1] CREATE 16MB guard (or truncate to 0)
  // is the poison precursor.
  const bool hugesz = ((sz1 >> 3) + 1) > 0x1000000u || sz2 > 0x1000000u ||
                      sz3 > 0x400000u;
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool preBad =
      inImg(d1) || inImg(d2) || inImg(d3) || inImg(s1) || inImg(s2) || inImg(s3);
  const uint32_t lr = (uint32_t)ctx.lr;
  // iter-349h: PRE line printed BEFORE the impl (wild calls never return in
  // time for the post log — that was the sampling mystery). Wild-size srcs
  // force an Error naming the SRC OBJECT identity at the moment of the call.
  {
    const uint32_t mode = rd(src + 8);
    const bool wildPre = hugesz || sz2 == 0xFFFFFFF8u || sz1 > 0x1000000u;
    if (wildPre || n <= 4)
      SR_LOG_BOOT(wildPre ? sr::LogSeverity::Error : sr::LogSeverity::Info,
        "[TRICOPY_PRE] #%u dst=%08X src=%08X mode=%08X sz={%X,%X,%X} "
        "bufs={%08X,%08X,%08X} lr=%08X%s",
        n, dst, src, mode, sz1, sz2, sz3, s1, s2, s3, lr,
        wildPre ? "  <<WILD SRC (the flatten's source object)" : "");
  }
  __imp__sub_82604190(ctx, base);
  const uint32_t p1 = rd(dst + 0x20), p2 = rd(dst + 0x24), p3 = rd(dst + 0x28);
  // iter-348e: re-read the SRC sizes after the call. Entry reads were sane in
  // every wipe roll, yet resize received size2=0x843A6028 — if the post reads
  // differ from entry, the object mutated DURING the copy (the deadline-CS in
  // sub_8263CC68 failing open = producer/consumer race, the platform gap).
  const uint32_t q1 = rd(src + 0x14), q2 = rd(src + 0x18), q3 = rd(src + 0x1C);
  const bool postHuge = ((q1 >> 3) + 1) > 0x1000000u || q2 > 0x1000000u ||
                        q3 > 0x400000u || q1 != sz1 || q2 != sz2 || q3 != sz3;
  const bool postBad = inImg(p1) || inImg(p2) || inImg(p3);
  // iter-349d: call volume is tiny (~35/run) and the wipe call keeps landing
  // in the sampling gaps with self-contradictory forensics — log EVERY call.
  // iter-461: it is not tiny in the city (~50/s): the bad ones, the first 200
  // and a 1-in-64 sample (SR_LOG_VERBOSE=1: every call).
  if (preBad || postBad || hugesz || postHuge || n <= 200 || (n % 64) == 0 ||
      sr::kernel::logVerbose()) {
    SR_LOG_BOOT((preBad || postBad || hugesz || postHuge)
                    ? sr::LogSeverity::Error : sr::LogSeverity::Info,
      "[TRICOPY] #%u dst=%08X pre{%08X,%08X,%08X} post{%08X,%08X,%08X} "
      "src=%08X{%08X,%08X,%08X} sz={%X,%X,%X} szPost={%X,%X,%X} lr=%08X%s%s%s%s",
      n, dst, d1, d2, d3, p1, p2, p3, src, s1, s2, s3, sz1, sz2, sz3,
      q1, q2, q3, lr,
      preBad ? "  <<PRE-IN-IMAGE" : "",
      postBad ? "  <<POST-IN-IMAGE (poisoned buffer assigned INSIDE this call)" : "",
      hugesz ? "  <<HUGE-SZ (vt[1] guard-fail precursor)" : "",
      postHuge ? "  <<SZ-MUTATED-DURING-COPY (deadline-CS race caught)" : "");
  }
  // iter-348d: the zeros cascade = the persistent SRC object stomped between
  // frames (size2 became a physical-range pointer 0x843A6028 → resize
  // memsets 2.2GB over .data). This hook sees src every frame (~30ms):
  // keep a last-good snapshot; on any huge/garbage size, dump the object
  // AND its neighborhood ±0x40 plus the last-good copy — brackets the stomp
  // to one frame and shows its shape (shift vs neighbor overflow).
  if (src >= 0x84000000u && src < 0x9F000000u) {
    struct Snap { uint32_t src; uint8_t good[0xC0]; };
    static Snap s_snaps[8] = {};
    static std::atomic<unsigned> s_stompLogs{0};
    const uint8_t* sp = base + src - 0x40;
    int slot = -1, freeSlot = -1;
    for (int i = 0; i < 8; ++i) {
      if (s_snaps[i].src == src) { slot = i; break; }
      if (s_snaps[i].src == 0 && freeSlot < 0) freeSlot = i;
    }
    if (hugesz) {
      if (s_stompLogs.fetch_add(1, std::memory_order_relaxed) < 6) {
        char hx[3 * 0xC0 + 1]; int o = 0;
        for (int i = 0; i < 0xC0; ++i)
          o += snprintf(hx + o, sizeof(hx) - o, "%02X ", sp[i]);
        hx[o] = '\0';
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[TRISTOMP] src=%08X NOW  (src-0x40..src+0x80): %s", src, hx);
        if (slot >= 0) {
          o = 0;
          for (int i = 0; i < 0xC0; ++i)
            o += snprintf(hx + o, sizeof(hx) - o, "%02X ", s_snaps[slot].good[i]);
          hx[o] = '\0';
          SR_LOG_BOOT(sr::LogSeverity::Error,
            "[TRISTOMP] src=%08X GOOD (last healthy frame):  %s", src, hx);
        }
      }
    } else {
      if (slot < 0) slot = (freeSlot >= 0) ? freeSlot : 7;
      s_snaps[slot].src = src;
      memcpy(s_snaps[slot].good, sp, sizeof(s_snaps[slot].good));
    }
  }
}

// iter-348 (charter DIAG): THE WIPE ZEROER CAUGHT — sub_821938E0 = the REAL
// allocator-create behind vt[1] of 0x8209A448 (NOT our host d3d stub; iter-344
// fixed the wrong surface). Its first act is zeroing its FIVE sret out-slots
// (param_1[0..4]). VEH hits #784/#785 (run0 12:50:43) show those zeroing
// stores landing at 0x8295C630/0x8295C638 — i.e. it was CALLED WITH
// out = 0x8295C62C, the service-descriptor table. lr=0x825FFB78 = the bctrl
// RA inside tri-buffer resize, whose out is a STACK LOCAL — meaning that
// call's r1 was in .data (~0x8295C5E0; matches the back-chain walk). This
// hook fires on the dispatch-table path (indirect call — cannot be blind):
// log out/alloc/desc/lr/r1, flag out-in-image or r1-outside-heap loudly.
PPC_FUNC(sub_821938E0) {
  const uint32_t out = ctx.r3.u32, alo = ctx.r4.u32, dsc = ctx.r5.u32;
  const uint32_t r1 = ctx.r1.u32, lr = (uint32_t)ctx.lr;
  auto rd = [&](uint32_t va) -> uint32_t {
    if (va < 0x10000u) return 0;
    const uint8_t* p = base + va;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  };
  auto inImg = [](uint32_t v) { return v >= 0x82000000u && v < 0x82B00000u; };
  // Guest stacks are heap-allocated well above the image; an r1 inside the
  // image means this call frame lives ON TOP OF .data (the wipe mechanism).
  const bool outBad = inImg(out), r1Bad = inImg(r1);
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (outBad || r1Bad || n <= 8 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT((outBad || r1Bad) ? sr::LogSeverity::Error : sr::LogSeverity::Info,
      "[CREATE5] #%u out=%08X alo=%08X desc=%08X{%X,%X|%X,%X|%X,%X|%X,%X|%X,%X} "
      "lr=%08X r1=%08X%s%s",
      n, out, alo, dsc,
      rd(dsc), rd(dsc + 4), rd(dsc + 8), rd(dsc + 12), rd(dsc + 16),
      rd(dsc + 20), rd(dsc + 24), rd(dsc + 28), rd(dsc + 32), rd(dsc + 36),
      lr, r1,
      outBad ? "  <<OUT-IN-IMAGE (descriptor wipe imminent)" : "",
      r1Bad ? "  <<R1-IN-IMAGE (guest stack on .data!)" : "");
  }
  __imp__sub_821938E0(ctx, base);
}

// iter-348e (charter DIAG): producer-side tri-buffer writers — the resize
// callers in the video TU (sub_8263CCD0 / sub_8263CF70 / sub_8263C750). If
// one of these passes size2=0x843A6028 (pointer-as-size) into resize, the
// garbage originates at ITS caller — log entry args, flag huge sizes.
static void triProducerProbe(const char* tag, PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32,
                 a6 = ctx.r6.u32, a7 = ctx.r7.u32;
  // iter-348e postmortem: these fns take POINTER args (CCD0's a4/a5 are
  // buffers) — the naive any-arg-huge check flooded 90K Error lines per run
  // once the accept loop went hot. Sample-only now; the real garbage-size
  // detector lives at the resize consumer (TRICOPY szPost).
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 6 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[TRIPROD:%s] #%u obj=%08X a4=%08X a5=%08X a6=%08X a7=%08X lr=%08X",
      tag, n, obj, a4, a5, a6, a7, (uint32_t)ctx.lr);
}
PPC_FUNC(sub_8263CCD0) { triProducerProbe("CCD0", ctx, base); __imp__sub_8263CCD0(ctx, base); }
PPC_FUNC(sub_8263CF70) { triProducerProbe("CF70", ctx, base); __imp__sub_8263CF70(ctx, base); }
PPC_FUNC(sub_8263C750) { triProducerProbe("C750", ctx, base); __imp__sub_8263C750(ctx, base); }

// iter-347b (charter DIAG): the reverse array-walk invoker sub_820F0000(base,
// stride, count, fn) — VEH-proven trampoline of the MID-BOOT SECOND CRT
// STARTUP (fn lands in 0x82468xxx: CRT startup → static ctors → guest main →
// Init2 re-run = the ".data wipe"). Log every call whose fn is OUTSIDE the
// known-benign dtor callback; flag CRT-range fn loudly with the invoker lr.
PPC_FUNC(sub_820F0000) {
  const uint32_t arr = ctx.r3.u32, stride = ctx.r4.u32,
                 cnt = ctx.r5.u32, fn = ctx.r6.u32;
  const uint32_t callerLR = (uint32_t)ctx.lr;
  const bool crt = fn >= 0x82468000u && fn < 0x8246B000u;
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (crt || n <= 8 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(crt ? sr::LogSeverity::Error : sr::LogSeverity::Info,
      "[REVWALK] #%u arr=%08X stride=%u cnt=%u fn=%08X lr=%08X%s",
      n, arr, stride, cnt, fn, callerLR,
      crt ? "  <<CRT-STARTUP CALLBACK (second-boot trampoline!)" : "");
  }
  __imp__sub_820F0000(ctx, base);
}

// iter-346b (charter DIAG): runner tick probes. FeMasterTick = FSGameSimulationMain
// vt[5] (vtable 0x82001888), ticked by GameKernelLoop vt[9] TickOnce →
// ServiceTick_RunMain. KLOOP ENTER spam = the run fn re-entered constantly
// (healthy loops enter ONCE) — these two counts localize the break.
PPC_FUNC(sub_827206B8) {
  const uint32_t mgr = ctx.r3.u32;
  const uint32_t renderMgr = sr::kernel::readGuestU32BE(0x8296D8DCu);
  static const bool s_trace = [] {
    const char* e = std::getenv("SR_PASS_TRACE");
    return e && e[0] == '1';
  }();
  auto rd64 = [](uint32_t va) -> uint64_t {
    return (uint64_t(sr::kernel::readGuestU32BE(va)) << 32) |
           sr::kernel::readGuestU32BE(va + 4);
  };
  const uint8_t* flagHost = sr::kernel::guestVAToHost(mgr + 65);
  const uint32_t flag65 = flagHost ? *flagHost : 0xFFu;
  const uint64_t threshold = rd64(mgr + 72);
  const uint64_t prevClock = rd64(mgr + 96);
  const uint64_t accum = rd64(mgr + 104);
  const uint64_t offset = rd64(mgr + 112);
  __imp__sub_827206B8(ctx, base);
  // iter-365 FE-FREEZE MECHANISM PROBE 2 (log-only, ungated): [SVCTICK_STUCK] refuted the
  // pending-transition mode — dispatcher+loops run (SVCTICK #8192+, TICKONCE #131072) while
  // FeMasterTick alone stops (#~21). Remaining TickOnce gate = THIS TimeGate. If a manager's
  // clock stalls (accum stops advancing), the gate returns 0 forever and its systems silently
  // never tick — exactly the freeze signature. Detect: same mgr gated-off (rc=0) for 8192
  // consecutive calls with a NON-advancing accum -> one [TIMEGATE_STUCK] line with the fields.
  {
    static std::atomic<uint32_t> s_gm[4]{};
    static std::atomic<uint32_t> s_gk[4]{};
    static std::atomic<uint64_t> s_ga[4]{};
    static std::atomic<uint32_t> s_gs[4]{};
    if (mgr >= 0x10000u && mgr < 0xC0000000u) {
      for (int i = 0; i < 4; ++i) {
        uint32_t cur = s_gm[i].load(std::memory_order_relaxed);
        if (cur != mgr && cur != 0) continue;
        if (cur == 0) s_gm[i].store(mgr, std::memory_order_relaxed);
        if (ctx.r3.u32 == 0) {
          uint32_t k = s_gk[i].fetch_add(1, std::memory_order_relaxed) + 1;
          if (k == 1u) s_ga[i].store(accum, std::memory_order_relaxed);
          if (k == 8192u && s_gs[i].exchange(1, std::memory_order_relaxed) == 0) {
            const uint64_t accum0 = s_ga[i].load(std::memory_order_relaxed);
            SR_LOG_BOOT(sr::LogSeverity::Error,
              "[TIMEGATE_STUCK] mgr=%08X rc=0 for 8192 consecutive: flag65=%u "
              "threshold=%016llX prev=%016llX accum(now)=%016llX accum(streak-start)=%016llX "
              "advanced=%s — systems on this mgr never tick",
              mgr, flag65,
              (unsigned long long)threshold, (unsigned long long)prevClock,
              (unsigned long long)accum, (unsigned long long)accum0,
              accum != accum0 ? "YES(throttle-only?)" : "NO(CLOCK STALLED)");
          }
        } else {
          // iter-461: a true one-shot per manager (s_gs stays set). The gate
          // closes and reopens ~60 times a second (the runners spin ~500K
          // iterations/s between 60 Hz ticks: 8192 gated-off calls = 16 ms),
          // so the reset re-fired this ERROR line ~6 times a second per runner.
          s_gk[i].store(0, std::memory_order_relaxed);
        }
        break;
      }
    }
  }
  // iter-390: [TGCENSUS] — per-mgr 2s pass-rate census that KEEPS reporting
  // after a freeze ([TIMEGATE_STUCK] is a one-shot that goes silent exactly
  // when the post-freeze state matters). The mode-B catch showed the 30Hz
  // gates passing until 18:11:30 then never again, and the 60Hz gates
  // stuttering from birth — but no probe could say whether the CLOCK READ
  // (cell 0x820007D0 -> bundle+0x10) still advances post-freeze, or what
  // total(+112) holds. This logs calls/passes per window + a live clock read
  // done exactly the way sub_824680C0's original body does it (the GuestClockMs
  // splice was deleted in 156b6f47, docs/SPLICE_CONVERSION.md).
  {
    static std::atomic<uint32_t> s_tm[4]{};
    static std::atomic<uint32_t> s_tc[4]{}, s_tp[4]{};
    static std::atomic<uint64_t> s_tn[4]{};
    if (mgr >= 0x10000u && mgr < 0xC0000000u) {
      for (int i = 0; i < 4; ++i) {
        uint32_t cur = s_tm[i].load(std::memory_order_relaxed);
        if (cur != mgr && cur != 0) continue;
        if (cur == 0) s_tm[i].store(mgr, std::memory_order_relaxed);
        const uint32_t c = s_tc[i].fetch_add(1, std::memory_order_relaxed) + 1;
        if (ctx.r3.u32 != 0) s_tp[i].fetch_add(1, std::memory_order_relaxed);
        if ((c & 0x3FFu) == 0) {   // time-check every 1024 calls (keeps it cold)
          const uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch()).count();
          uint64_t nxt = s_tn[i].load(std::memory_order_relaxed);
          if (nowMs >= nxt &&
              s_tn[i].compare_exchange_strong(nxt, nowMs + 2000,
                                              std::memory_order_relaxed)) {
            const uint32_t cell = sr::kernel::readGuestU32BE(0x820007D0u);
            const uint32_t clkMs = (cell >= 0x10000u)
                ? sr::kernel::readGuestU32BE(cell + 0x10u) : 0xDEADBEEFu;
            SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[TGCENSUS] mgr=%08X calls=%u passes=%u clkMs=%u cell=%08X "
              "thr=%llu accum=%llu total=%lld prev=%llu",
              mgr, s_tc[i].exchange(0, std::memory_order_relaxed),
              s_tp[i].exchange(0, std::memory_order_relaxed),
              clkMs, cell,
              (unsigned long long)threshold, (unsigned long long)accum,
              (long long)rd64(mgr + 112), (unsigned long long)prevClock);
          }
        }
        break;
      }
    }
  }
  if (s_trace && mgr == renderMgr) {
    static std::atomic<unsigned> s_n{0};
    const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[RENDER_TIME_GATE] #%u mgr=%08X flag65=%u threshold=%016llX "
          "prev=%016llX accum=%016llX offset=%016llX -> %u "
          "postPrev=%016llX postAccum=%016llX",
          n, mgr, flag65,
          static_cast<unsigned long long>(threshold),
          static_cast<unsigned long long>(prevClock),
          static_cast<unsigned long long>(accum),
          static_cast<unsigned long long>(offset), ctx.r3.u32,
          static_cast<unsigned long long>(rd64(mgr + 96)),
          static_cast<unsigned long long>(rd64(mgr + 104)));
  }
}
PPC_FUNC(sub_82720C60) {
  // iter-384: per-runner tick census for the shallow-2 repro
  // (SR_MAIN_BOOTSTRAPS=0, 2-in-3). TickOnce = TimeGate -> vt[+32] ready? ->
  // vt[+4] payload. Logs state changes + capped census: WHICH runner idles
  // and whether it is paused (+0x41) or ready-refused.
  const uint32_t self = ctx.r3.u32;
  uint8_t paused = 0, running = 0;
  uint32_t vptr = 0;
  if (self >= 0x10000u) {
    vptr = sr::kernel::readGuestU32BE(self);
    if (const uint8_t* p = sr::kernel::guestVAToHost(self + 0x41)) paused = *p;
    if (const uint8_t* p = sr::kernel::guestVAToHost(self + 0x42)) running = *p;
  }
  __imp__sub_82720C60(ctx, base);
  const uint32_t ticked = ctx.r3.u32 & 0xFF;
  struct Slot { uint32_t self; uint32_t lastState; uint64_t calls, ticks; };
  static Slot s_slots[8] = {};
  for (auto& sl : s_slots) {
    if (sl.self != self) {
      if (sl.self != 0) continue;
      sl.self = self;
    }
    sl.calls++;
    if (ticked) sl.ticks++;
    const uint32_t state = (paused << 8) | ticked;
    if (sl.calls <= 2 || state != sl.lastState || (sl.calls % 2000000) == 0) {
      sl.lastState = state;
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[KLOOP_TICK] runner=%08X vt=%08X paused=%u running=%u ticked=%u "
          "calls=%llu ticks=%llu tid=%u",
          self, vptr, paused, running, ticked,
          (unsigned long long)sl.calls, (unsigned long long)sl.ticks,
          sr::kernel::getCurrentThreadHandle());
    }
    break;
  }
}
PPC_FUNC(sub_8271ED68) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 12 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[SVCTICK] #%u desc=%08X r4=%08X lr=%08X tid=%lu",
                n, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr,
                (unsigned long)GetCurrentThreadId());
  // iter-398h: PER-CALL pend state at ENTRY (the census reads pend LATER and
  // can miss an oscillating set->apply->re-arm transition). If the render
  // desc's wedge-era entries all arrive pend-SET, ApplyTransition starves
  // the tick forever = a transition that never completes, retried per pump.
  {
    const uint32_t desc = ctx.r3.u32;
    if (desc == 0x8295C690u) {
      const uint32_t p7 = sr::kernel::readGuestU32BE(desc + 0x1Cu);
      const uint32_t p8 = sr::kernel::readGuestU32BE(desc + 0x20u);
      static std::atomic<unsigned> s_690n{0}, s_690set{0};
      unsigned m = s_690n.fetch_add(1, std::memory_order_relaxed) + 1;
      if (p7 && p8) s_690set.fetch_add(1, std::memory_order_relaxed);
      if (m <= 12 || (m & (m - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[SVC690] #%u pend=%08X/%08X setSoFar=%u tid=%lu", m, p7, p8,
            s_690set.load(std::memory_order_relaxed),
            (unsigned long)GetCurrentThreadId());
    }
  }
  // iter-365 FE-FREEZE PROBE 3 (log-only): probes 1+2 refuted stuck-pending and TimeGate-stall.
  // Remaining mode: the FE runner is SKIPPED by the master dispatch (deactivated byte / curSys
  // swap) — i.e. PumpOrTick simply stops being called for it. Census: every 8192 calls dump each
  // known runner's calls-in-window + live fields. The frozen roll shows WHICH runner went quiet
  // and what its state says (act=0? curSys=NULL/changed?).
  {
    static std::atomic<uint32_t> s_cm[6]{};
    static std::atomic<uint32_t> s_cw[6]{};
    static std::atomic<uint32_t> s_total{0};
    const uint32_t rr = ctx.r3.u32;
    if (rr >= 0x10000u && rr < 0xC0000000u) {
      for (int i = 0; i < 6; ++i) {
        uint32_t cur = s_cm[i].load(std::memory_order_relaxed);
        if (cur == rr) { s_cw[i].fetch_add(1, std::memory_order_relaxed); break; }
        if (cur == 0) {
          if (s_cm[i].compare_exchange_strong(cur, rr, std::memory_order_relaxed)) {
            s_cw[i].fetch_add(1, std::memory_order_relaxed);
            break;
          }
        }
      }
      uint32_t t = s_total.fetch_add(1, std::memory_order_relaxed) + 1;
      if ((t & 0x1FFFu) == 0) {  // census every 8192 calls
        for (int i = 0; i < 6; ++i) {
          uint32_t d = s_cm[i].load(std::memory_order_relaxed);
          if (!d) break;
          const uint8_t* actH = sr::kernel::guestVAToHost(d + 5u);
          const uint32_t curSys = sr::kernel::readGuestU32BE(d + 8u);
          const uint32_t csVt = (curSys >= 0x10000u && curSys < 0xC0000000u)
                                    ? sr::kernel::readGuestU32BE(curSys) : 0;
          SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[SVCTICK_CENSUS] t=%u slot%d desc=%08X win=%u act=%u curSys=%08X csVt=%08X "
            "pend=%08X/%08X",
            t, i, d, s_cw[i].exchange(0, std::memory_order_relaxed),
            actH ? *actH : 0xFFu, curSys, csVt,
            sr::kernel::readGuestU32BE(d + 0x1Cu), sr::kernel::readGuestU32BE(d + 0x20u));
        }
      }
    }
  }
  {
    const uint32_t r = ctx.r3.u32;
    if (r >= 0x10000u && r < 0xC0000000u) {
      const uint32_t pend1 = sr::kernel::readGuestU32BE(r + 0x1Cu);
      const uint32_t pend2 = sr::kernel::readGuestU32BE(r + 0x20u);
      // per-runner consecutive-pending tracker (4 runners; small open table)
      static std::atomic<uint32_t> s_runner[4]{}, s_streak[4]{}, s_said[4]{};
      for (int i = 0; i < 4; ++i) {
        uint32_t cur = s_runner[i].load(std::memory_order_relaxed);
        if (cur != r && cur != 0) continue;
        if (cur == 0) s_runner[i].store(r, std::memory_order_relaxed);
        if (pend1 && pend2) {
          uint32_t k = s_streak[i].fetch_add(1, std::memory_order_relaxed) + 1;
          if (k == 64u && s_said[i].exchange(1, std::memory_order_relaxed) == 0) {
            const uint32_t curSys = sr::kernel::readGuestU32BE(r + 8u);
            const uint32_t p1vt = (pend1 >= 0x10000u && pend1 < 0xC0000000u)
                                      ? sr::kernel::readGuestU32BE(pend1) : 0;
            SR_LOG_BOOT(sr::LogSeverity::Error,
                "[SVCTICK_STUCK] runner=%08X pending 64 consecutive calls: pend1=%08X(vt %08X) "
                "pend2=%08X curSys=%08X active=%u — transition never completes (FE tick starved)",
                r, pend1, p1vt, pend2, curSys,
                (sr::kernel::readGuestU32BE(r + 4u) >> 16) & 0xFFu);
          }
        } else {
          s_streak[i].store(0, std::memory_order_relaxed);
          s_said[i].store(0, std::memory_order_relaxed);
        }
        break;
      }
    }
  }
  __imp__sub_8271ED68(ctx, base);
}

// iter-346 (charter DIAG): pump ACTION probes — do the swap processors and
// the bottom-half ever run now that vsync is live? Log-only.
#define SR_PUMP_PROBE(NAME) \
  PPC_FUNC(NAME) { \
    SR_NATIVE_SCOPE_SYM(NAME); /* NATIVE N0: scripts/gen_native_brackets.py */ \
    static std::atomic<unsigned> s_n{0}; \
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1; \
    if (n <= 6 || (n & (n - 1)) == 0) \
      SR_LOG_BOOT(sr::LogSeverity::Info, "[PUMP] " #NAME " #%u r3=%08X r4=%08X lr=%08X", \
                  n, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr); \
    __imp__##NAME(ctx, base); \
  }
// 82110840 is supplied by native_pump.cpp; its default-off driver gate retains
// the exact SDK body. The old observe-only macro must not duplicate that symbol.
// sub_82112050 (D3DDeviceSwap) probe moved to kernel/hle_api_observe.cpp
// iter-440: the API-boundary observe hook logs it as [API_OBS] Swap with
// full args + per-frame counter summary (this [PUMP] line was redundant).
#endif  // SR_ORACLE diagnostics

extern "C" void __imp__sub_82111EC8(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_82111EC8) {
  SR_NATIVE_SCOPE(0x82111EC8);
#if SR_ORACLE
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 6 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PUMP] sub_82111EC8 #%u r3=%08X r4=%08X lr=%08X",
                n, ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
#endif
  if (sr::native::flush::presentGateOn() && sr::native::flush::presentGateLeaf(ctx, base)) return;
#if SR_ORACLE
  // The present-gate CHECK (SR_NATIVE_FLUSH_CHECK): a development oracle, compiled out under SR_ORACLE=0
  // (kernel/sr_oracle.hpp) - the guest body then runs as with the gate unset.
  if (sr::native::flush::checkOn()) {
    sr::native::flush::PresentGateCheck check;
    const bool admitted = sr::native::flush::presentGateCheckBefore(ctx, check);
    sr::native::flush::PresentGateOracle oracle;
    __imp__sub_82111EC8(ctx, base);
    if (admitted) sr::native::flush::presentGateCheckAfter(check, ctx);
    return;
  }
#endif
  __imp__sub_82111EC8(ctx, base);
}
#if SR_ORACLE
#undef SR_PUMP_PROBE

// iter-350g (charter DIAG): gameconfig getString probe. WorldFlowXML init
// derives the FE flow FSM asset name from sub_82187548(store DAT_8296D87C,
// "WorldFlowXML"). Roll A returned 'WorldFlow' (catalog hit rc=1); roll B
// returned a dangling heap buffer ('.*R.' junk) — the stored value string
// dies between gameconfig parse and flow init (transient-buffer class).
// Log every 'World*' key lookup (key + value ptr + value string) plus a
// sampled census. Cross-TU (caller TU6, callee TU11) so this fires.
PPC_FUNC(sub_82187548) {
  const uint32_t keyPtr = ctx.r4.u32, store = ctx.r3.u32;
  char key[48] = {0};
  for (unsigned i = 0; i < sizeof(key) - 1 && keyPtr; ++i) {
    char c = (char)base[keyPtr + i];
    if (!c) break;
    key[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
  }
  __imp__sub_82187548(ctx, base);
  const uint32_t val = ctx.r3.u32;
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool world = key[0] == 'W' && key[1] == 'o' && key[2] == 'r';
  if (world || n <= 24 || (n & (n - 1)) == 0) {
    char v[48] = {0};
    for (unsigned i = 0; i < sizeof(v) - 1 && val; ++i) {
      char c = (char)base[val + i];
      if (!c) break;
      v[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    SR_LOG_BOOT(world ? sr::LogSeverity::Warn : sr::LogSeverity::Info,
      "[CFGSTR] #%u store=%08X key='%s' -> val=%08X '%s' lr=%08X",
      n, store, key, val, v, (uint32_t)ctx.lr);
  }
}

// iter-350h (charter DIAG): resource-acquire callee trace. sub_826FAAC0's
// (TU95) decompiled control flow keeps contradicting the observed logs, so
// trace its branch-deciding DIRECT callees via cross-TU strong-defs:
//   sub_826E4DB0 (TU92)  record resolved-check
//   sub_826E4D80 (TU92)  record failed-check
//   sub_826E5168 (TU92)  record raw/fail mark (setter)
//   sub_826E70F8 (TU93)  catalog OPEN (catalog vt[+0x64])
//   sub_82703EA0 (TU96)  scheduler item RUN
// Logging gated on the WorldFlowXML-init window (g_srInFlowInit) + first
// few calls to prove hook liveness.
namespace sr { namespace kernel { extern std::atomic<int> g_srInFlowInit; } }
#define SR_ACQ_PROBE(NAME, TAG)                                              \
  extern "C" void __imp__##NAME(PPCContext& __restrict ctx, uint8_t* base);  \
  PPC_FUNC(NAME) {                                                           \
    static std::atomic<unsigned> s_n{0};                                     \
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;            \
    const bool inFlow =                                                      \
        sr::kernel::g_srInFlowInit.load(std::memory_order_relaxed) > 0;      \
    const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32,        \
                   a6 = ctx.r6.u32, alr = (uint32_t)ctx.lr;                  \
    if (inFlow || n <= 3)                                                    \
      SR_LOG_BOOT(sr::LogSeverity::Warn,                                     \
                  "[ACQ:" TAG "] #%u ENTER r3=%08X r4=%08X r5=%08X r6=%08X " \
                  "lr=%08X%s",                                               \
                  n, a3, a4, a5, a6, alr, inFlow ? " <<FLOW" : "");          \
    __imp__##NAME(ctx, base);                                                \
    if (inFlow || n <= 3)                                                    \
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[ACQ:" TAG "] #%u EXIT rc=%08X%s", \
                  n, ctx.r3.u32, inFlow ? " <<FLOW" : "");                   \
  }
// (sub_826E4DB0 and sub_82703EA0 already have strong-defs in
// rtam_service.cpp — their [RTAM_Q] tags cover those stages.)
// iter-350j: sub_826E4D80 gets its own richer probe (below), not the macro.
extern "C" void __imp__sub_826E4D80(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_826E4D80) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool inFlow =
      sr::kernel::g_srInFlowInit.load(std::memory_order_relaxed) > 0;
  const uint32_t handle = ctx.r3.u32;
  // rec = *(handle); state halfword = *(u16*)(rec+0x1C); the check returns
  // 2 if bits(0x18)==0x10 (failed), 1 if ==0x08, else 0. Dump rec + full
  // +1C word AT CHECK TIME — the post-hoc dumps read 0 (cleared later).
  uint32_t rec = 0, f1c = 0, nameVA = 0;
  char nm[40] = {0};
  if (handle >= 0x10000u && handle < 0xC0000000u) {
    rec = sr::kernel::readGuestU32BE(handle);
    if (rec >= 0x10000u && rec < 0xC0000000u) {
      f1c = sr::kernel::readGuestU32BE(rec + 0x1C);
      nameVA = sr::kernel::readGuestU32BE(rec);
      if (nameVA >= 0x10000u && nameVA < 0xC0000000u) {
        const uint8_t* p = base + nameVA;
        for (unsigned i = 0; i + 1 < sizeof(nm) && p[i]; ++i)
          nm[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? char(p[i]) : '.';
      }
    }
  }
  __imp__sub_826E4D80(ctx, base);
  static std::atomic<unsigned> s_nz{0};
  const bool logNz = ctx.r3.u32 != 0 &&
      s_nz.fetch_add(1, std::memory_order_relaxed) < 120;
  if (inFlow || n <= 3 || logNz)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[ACQ:failed] #%u handle=%08X rec=%08X name='%s' f1C=%08X -> rc=%08X lr=%08X%s",
      n, handle, rec, nm, f1c, ctx.r3.u32, (uint32_t)ctx.lr,
      inFlow ? " <<FLOW" : "");
}
SR_ACQ_PROBE(sub_826E5168, "markraw")
SR_ACQ_PROBE(sub_826E70F8, "catopen")
#undef SR_ACQ_PROBE

// iter-350i: record queued-bit (state 0x0008 at rec+0x1C) setter/clearer.
// WORLDFLOW's record carries a stale 0x0008 from the preload era, making
// every later acquire early-exit ("already in flight"). Log EVERY call:
// who queues which record (name at *(handle)+0) and who clears — the point
// where clears stop is the preload-chain stall. Low volume by design
// (schedule/complete events only).
extern "C" void __imp__sub_826E4FA8(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_826E4FA8) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t handle = ctx.r3.u32, val = ctx.r4.u32;
  uint32_t rec = 0, nameVA = 0;
  char nm[40] = {0};
  if (handle >= 0x10000u && handle < 0xC0000000u) {
    rec = sr::kernel::readGuestU32BE(handle);
    if (rec >= 0x10000u && rec < 0xC0000000u) {
      nameVA = sr::kernel::readGuestU32BE(rec);
      if (nameVA >= 0x10000u && nameVA < 0xC0000000u) {
        const uint8_t* p = base + nameVA;
        for (unsigned i = 0; i + 1 < sizeof(nm) && p[i]; ++i)
          nm[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? char(p[i]) : '.';
      }
    }
  }
  if (n <= 400)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[QBIT] #%u %s rec=%08X name='%s' flags=%08X lr=%08X",
      n, val ? "SET" : "CLR", rec, nm,
      rec >= 0x10000u ? sr::kernel::readGuestU32BE(rec + 0x1C) : 0,
      (uint32_t)ctx.lr);
  __imp__sub_826E4FA8(ctx, base);
}

// iter-350l (charter DIAG): MOVIE QUEUE chain. The attract-mode FSM's
// PlayFMV state enters + exits in 22ms with FMV SM stuck at state 0 and
// no name queued — the queue call dies silently. sub_8235F2D0 = FMV SM
// queue-movie (TU47); sub_822B7F38 = PSM movie play (TU33). Both cross-TU
// from their callers — strong-defs fire. Log every call with the name arg.
// iter-428c: the FMV SM's CS (sm+560) gets orphaned — a pool worker exits
// "normally" with the guest CS object still showing recursion=1, wedging the
// main thread's SM tick forever. Latch the CS address here so the CS-pairing
// tracer (cs_timeout_underflow_fix.cpp) can log every enter/leave on it.
std::atomic<uint32_t> g_fmvSmCsVA{0};

extern "C" void __imp__sub_8235F2D0(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8235F2D0) {
  // iter-428: this is MoviePlayerOpen (IDA contract, verified from the
  // disassembly): r4 = a 552-byte descriptor COPIED to sm+8; the movie NAME
  // sits at descriptor+16 (lands at sm+24, where the extension strip runs).
  // The old probe read descriptor+0 -> always '' (probe artifact, iter-350l).
  // Real status is *(sm+4): 0=closed 1=open 3=FAILED — the r3 return is the
  // CS-leave value, never a status.
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t sm = ctx.r3.u32;
  char nm[48] = {0};
  const uint32_t p = ctx.r4.u32;
  if (p >= 0x10000u && p < 0xC0000000u)
    for (unsigned i = 0; i + 1 < sizeof(nm) && base[p + 16 + i]; ++i)
      nm[i] = (base[p + 16 + i] >= 0x20 && base[p + 16 + i] < 0x7F)
                  ? (char)base[p + 16 + i] : '.';
  const uint32_t stBefore = sr::kernel::readGuestU32BE(sm + 4u);
  if (sm >= 0x10000u) g_fmvSmCsVA.store(sm + 560u, std::memory_order_relaxed);
  if (n <= 40)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[MOVIE_Q] F2D0 #%u sm=%08X name='%s' r5=%08X state(before)=%u lr=%08X",
      n, sm, nm, ctx.r5.u32, stBefore, (uint32_t)ctx.lr);
  __imp__sub_8235F2D0(ctx, base);
  if (n <= 40)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[MOVIE_Q] F2D0 #%u EXIT state(after)=%u (1=open 3=FAILED)",
      n, sr::kernel::readGuestU32BE(sm + 4u));
}

// iter-428e: LONGJMP CENSUS. The iter-350 fix registers only Lua's
// rawrunprotected buffer; the other SEVEN setjmp callers (libjpeg/libpng
// error recovery, sub_823F20F8, sub_827C1170/1438, sub_82858C90/82859260 —
// the codec family on the movie path) still hit the LEGACY restore-and-return
// on longjmp: the C++ recomp stack unwinds with NO guest cleanup (no CS
// leaves, no dtors) and the thread "exits normally". That orphaned the FMV
// SM's critical section mid-movie-3 (worker enters at [CS_PAIR] 23.784,
// longjmps ~30ms later, EXITS at 23.818). Log every longjmp with buf/val/
// lr/tid so the movie-path caller names itself; the fix is the iter-350
// treatment applied to that caller's spliced body.
// iter-467: done - the seven are libjpeg / libpng image loading and the HLSL / microcode
// shader compilers (IDA), each now protected by kernel/r1_platform.cpp's host setjmp, so a
// longjmp to any of their buffers throws below and the legacy path is unreachable.
#endif  // SR_ORACLE diagnostics

extern "C" void __imp__sub_827A4270(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_827A4270) {
#if SR_ORACLE
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 64 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Error,
        "[LONGJMP] #%u buf=%08X val=%u caller_lr=%08X tid=%u host=%lu — if no "
        "matching [LONGJMP_THROWN] follows, this unwound the LEGACY path "
        "(guest cleanup skipped)",
        n, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr,
        sr::kernel::getCurrentThreadHandle(),
        (unsigned long)GetCurrentThreadId());
#endif
  // R1: a longjmp to a buffer luaD_rawrunprotected or a protected setjmp user registered
  // unwinds the host frames by throwing (kernel/r1_platform.cpp), after the console's
  // _NLG_Notify call (iter-467); the old splice threw as its first statement.
  sr::r1::crtLongjmpThrow(ctx, base);
  __imp__sub_827A4270(ctx, base);
}

#if SR_ORACLE
// iter-428: the movie open's FILE leaf — MoviePlayerOpen calls this with the
// extension-stripped name (CreateFile-shaped args) and stores the handle to
// dword_829761D8; state goes 1 only if it returns != -1. Movie 2 reached
// state 1 with NO NtCreateFile in the log — this hook decides whether the
// open really fired (and with what name/rc) or state 1 came from elsewhere.
extern "C" void __imp__sub_824687F8(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_824687F8) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  char nm[64] = {0};
  const uint32_t p = ctx.r3.u32;
  if (p >= 0x10000u && p < 0xC0000000u)
    for (unsigned i = 0; i + 1 < sizeof(nm) && base[p + i]; ++i)
      nm[i] = (base[p + i] >= 0x20 && base[p + i] < 0x7F) ? (char)base[p + i] : '.';
  const uint32_t access = ctx.r4.u32, disp = ctx.r6.u32;
  __imp__sub_824687F8(ctx, base);
  if (n <= 40 || (n % 500) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[MOVIE_FOPEN] #%u name='%s' access=%08X disp=%08X -> rc=%08X", n, nm,
      access, disp, ctx.r3.u32);
}
// (sub_822B7F38 already strong-defed in splash_hooks.cpp — its filename
// observer covers the PSM movie-play stage.)

// iter-350l: FE-flow ACTION BIND observer. The whole parse builds exactly
// ONE factory action (actIdx=1); sub_8278C3D8(ctx, idx, item) is the
// generic array-store the parser uses to bind action->state slot. Log
// only stores whose item has a vtable in the FE action-class region
// [0x82017300, 0x82017C00) — names WHICH state slot carries the action.
extern "C" void __imp__sub_8278C3D8(PPCContext& __restrict ctx, uint8_t* base);
PPC_FUNC(sub_8278C3D8) {
  const uint32_t item = ctx.r5.u32;
  if (item >= 0x10000u && item < 0xC0000000u && !(item & 3u)) {
    const uint32_t vt = sr::kernel::readGuestU32BE(item);
    if (vt >= 0x82017300u && vt < 0x82017C00u) {
      static std::atomic<unsigned> s_n{0};
      unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 60)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[ACT_BIND] #%u ctx=%08X slot=%u action=%08X vt=%08X lr=%08X",
          n, ctx.r3.u32, ctx.r4.u32, item, vt, (uint32_t)ctx.lr);
    }
  }
  __imp__sub_8278C3D8(ctx, base);
}

// iter-345b (charter DIAG): the PSM delegate is REGISTERED on every roll but
// never invoked — the missing link is the callback-chain WALKER: the
// per-frame master dispatch sub_8271F848 (a POSTED JOB, no static caller;
// burndown class-C's open question "does the natural boot post it?" — the
// baseline's fsmPass=0 suggests NO). Count both the dispatch and the drain.
PPC_FUNC(sub_8271F848) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[FRAME_DISPATCH] sub_8271F848 #%u r3=%08X lr=%08X", n, ctx.r3.u32, (uint32_t)ctx.lr);
  __imp__sub_8271F848(ctx, base);
}
// iter-345 (charter DIAG): SPLASH DETERMINISM. sub_822AB748(psm, mode) is the
// runtime-registered PSM dispatcher — the SOLE caller of the natural splash
// advance (Function_822A8C68, gated on counter@psm+0x230 >= 0; else needs the
// flow-client globals EB80/EDF8/EDFC to issue the start cmd). The sequencer
// fires on ~1/4 rolls; this log answers per-roll: ticked-but-gated vs never
// ticked, and which gate blocked. Log-only.
PPC_FUNC(sub_822AB748) {
  const uint32_t psm = ctx.r3.u32;
  const uint32_t mode = ctx.r4.u32;
  const uint32_t callerLR = (uint32_t)ctx.lr;
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 12 || (n & (n - 1)) == 0) {
    auto rd = [&](uint32_t va) -> uint32_t {
      if (va < 0x10000u) return 0;
      const uint8_t* p = base + va;
      return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
             (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    };
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[PSM_DISPATCH] #%u psm=%08X mode=%u counter=%08X EB80=%08X EDF8=%08X EDFC=%08X lr=%08X",
      n, psm, mode, psm >= 0x10000u ? rd(psm + 0x230) : 0,
      rd(0x8296EB80u), rd(0x8296EDF8u), rd(0x8296EDFCu), callerLR);
  }
  __imp__sub_822AB748(ctx, base);
}

// iter-344c (charter DIAG): the tri-buffer resize sub_825FF9F8(obj, fmt,
// bits, bytes2, count3). After the impl, the three buffer ptrs live at
// obj+0x20/0x24/0x28 — log them whenever any is in-image (the corrupt VA's
// provenance point: whatever the allocator's create returned). Log-only.
PPC_FUNC(sub_825FF9F8) {
  const uint32_t obj = ctx.r3.u32;
  const uint32_t callerLR = (uint32_t)ctx.lr;
  // iter-349h: ENTRY log BEFORE the impl — the wild call (pointer-as-size →
  // minutes-long memset) never returns in time for post-call logs; that was
  // the whole "sampling mystery". Wild args force an Error line with the
  // TRUE caller lr (register, not a stale stack walk).
  {
    const uint32_t a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32,
                   a7 = ctx.r7.u32;
    const bool wild = a5 > 0x1000000u || a6 > 0x1000000u || a7 > 0x400000u;
    static std::atomic<unsigned> s_pn{0};
    unsigned pn = s_pn.fetch_add(1, std::memory_order_relaxed) + 1;
    if (wild || pn <= 8 || (pn & (pn - 1)) == 0)
      SR_LOG_BOOT(wild ? sr::LogSeverity::Error : sr::LogSeverity::Info,
        "[TRIBUF_PRE] #%u obj=%08X mode=%08X sz={%X,%X,%X} lr=%08X%s",
        pn, obj, a4, a5, a6, a7, callerLR,
        wild ? "  <<WILD ARGS (the .data-flattening resize, caught at entry)"
             : "");
  }
  __imp__sub_825FF9F8(ctx, base);
  auto rd = [&](uint32_t va) -> uint32_t {
    if (va < 0x10000u) return 0;
    const uint8_t* p = base + va;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  };
  const uint32_t b1 = rd(obj + 0x20), b2 = rd(obj + 0x24), b3 = rd(obj + 0x28);
  auto inImg = [](uint32_t v) { return v >= 0x82000000u && v < 0x82B00000u; };
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool bad = inImg(b1) || inImg(b2) || inImg(b3);
  if (bad || n <= 6 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(bad ? sr::LogSeverity::Error : sr::LogSeverity::Info,
      "[TRIBUF] #%u obj=%08X bufs={%08X,%08X,%08X} sizes={%08X,%08X,%08X} lr=%08X%s",
      n, obj, b1, b2, b3, rd(obj + 0x14), rd(obj + 0x18), rd(obj + 0x1C),
      callerLR, bad ? "  <<IN-IMAGE BUFFER" : "");
  }
}

// iter-344b (charter DIAG): video-out init sub_8229F460 — builds the D3D
// allocator: obj = DOGMA sub_8278D258(0x110); allocator = obj+4; stored to
// DAT_8297AF08 via Function_82604250. EXPLICIT failure path: alloc fails ->
// allocator NULL -> every per-frame video-out buffer-create no-ops with the
// out-param unwritten -> stack-garbage memset/memcpy sprays .data (the
// VEH-proven zeroer). Log entry + results to confirm which leg fails per roll.
PPC_FUNC(sub_8229F460) {
  const uint32_t self = ctx.r3.u32;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[VIDOUT_INIT] sub_8229F460 ENTER obj=%08X lr=%08X",
              self, (uint32_t)ctx.lr);
  __imp__sub_8229F460(ctx, base);
  auto rd = [&](uint32_t va) -> uint32_t {
    if (va < 0x10000u) return 0;
    const uint8_t* p = base + va;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  };
  const uint32_t slot3 = rd(self + 12);           // param_1[3] = the 0x110 obj
  const uint32_t af08 = rd(0x8297AF08u);          // the allocator global
  const uint32_t aef4 = rd(0x8297AEF4u);          // the wrapper
  SR_LOG_BOOT(slot3 && af08 ? sr::LogSeverity::Info : sr::LogSeverity::Error,
    "[VIDOUT_INIT] sub_8229F460 DONE obj110=%08X DAT_8297AF08=%08X DAT_8297AEF4=%08X%s",
    slot3, af08, aef4,
    (slot3 && af08) ? "" : "  <<ALLOCATOR NULL — video-out will spray stack garbage (the zeroer)");
}

// iter-343g (charter DIAG): the RIP-inversion named the table writers as CRT
// memcpy (sub_825F6980) and memset-family (sub_825F4648). Hook both and log
// the GUEST CALLER LR whenever the dest lands in the watched .data page —
// thread-correct caller identity, no sampling ambiguity. Log-only.
static inline bool srInWatchPage(uint32_t va) {
  return va >= 0x82913000u && va < 0x82914000u;
}
PPC_FUNC(sub_825F6980) {
  if (srInWatchPage(ctx.r3.u32)) {
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[CRT_WRITE] memcpy dest=%08X src=%08X len=%08X lr=%08X  <<WATCH-PAGE (#%u)",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr, n);
  }
  __imp__sub_825F6980(ctx, base);
}
PPC_FUNC(sub_825F4640) {
  if (srInWatchPage(ctx.r3.u32)) {
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[CRT_WRITE] memset dest=%08X val=%08X len=%08X lr=%08X  <<WATCH-PAGE (#%u)",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr, n);
  }
  __imp__sub_825F4640(ctx, base);
}

// iter-343f (charter DIAG): the result-store ITSELF — sub_82762DA8(vmCtx,
// dest, valuePtr) copies the VM value into dest. Definitive catch regardless
// of which entry path / thread: any in-image dest is logged with lr (the
// exact interpreter call site). Log-only.
PPC_FUNC(sub_82762DA8) {
  const uint32_t dest = ctx.r4.u32;
  const uint32_t callerLR = static_cast<uint32_t>(ctx.lr);
  if (dest >= 0x82000000u && dest < 0x82B00000u) {
    static std::atomic<unsigned> s_bad{0};
    unsigned b = s_bad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (b <= 16 || (b & (b - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[ASVM_STORE] BAD dest=%08X vmCtx=%08X val=%08X lr=%08X  <<IN-IMAGE DEST (#%u)",
        dest, ctx.r3.u32, ctx.r5.u32, callerLR, b);
  }
  __imp__sub_82762DA8(ctx, base);
}

// iter-343e (charter DIAG): AS-VM execute-action entry sub_82762E78(vmCtx,
// action, destSlot). The .data zeroer at 0x8291363C is the VM's result-store
// (Function_82762DA8 @ lr 0x82762EEC) writing into a caller-provided dest
// slot that points INTO THE IMAGE — a broken AS frame register file (the
// 'type-8 frame' mystery / SR_AS_AUTO_FRAME class). Log any in-image dest
// WITH the true caller lr so the broken-frame creator is named. Log-only.
// iter-348 IDENTIFICATION: this is LUA 5.1 luaD_call — the depth counter at
// L+0x2E is nCcalls with Lua's exact guards (==200 → "C stack overflow" at
// 0x82943c00; >224 → hard throw: errorJmp fiber-switch, else panic vt[+0x28]
// → exit). DOGMA's script VM is Lua; coroutines are fibers. Both descriptor-
// wipe morphologies fire inside Lua error/GC paths, so a runaway C-call
// recursion (which does NOT happen on Xenia) is the prime upstream suspect.
// Probe: once depth crosses 150, log EVERY call with the callee TValue dump —
// the repeating function identifies the runaway binding.
PPC_FUNC(sub_82762E78) {
  const uint32_t vmCtx = ctx.r3.u32;
  const uint32_t action = ctx.r4.u32;
  const uint32_t dest = ctx.r5.u32;
  const uint32_t callerLR = static_cast<uint32_t>(ctx.lr);
  if (vmCtx >= 0x10000u) {
    const uint8_t* Lp = base + vmCtx;
    const uint16_t depth = (uint16_t(Lp[0x2E]) << 8) | Lp[0x2F];
    // iter-348b: lua_State health check (layout from luaD_precall 82762B10:
    // L+0x08 top, L+0x0C base, L+0x14 ci, L+0x18 stack_last, L+0x1C stack).
    // The wipe = a result-move sweep with a garbage count (r31≈0x7EF3BF89
    // counting down) through the descriptor page — i.e. top/base/ci->top in
    // .data or top>stack_last. Flag ANY in-image stack field at call entry;
    // the last good→first bad pair brackets the corruptor.
    {
      auto rdL = [&](uint32_t off) -> uint32_t {
        const uint8_t* p = Lp + off;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | uint32_t(p[3]);
      };
      const uint32_t top = rdL(0x08), Lbase = rdL(0x0C), ci = rdL(0x14),
                     slast = rdL(0x18), stk = rdL(0x1C);
      auto inImg2 = [](uint32_t v) { return v >= 0x82000000u && v < 0x82B00000u; };
      const bool sick = inImg2(top) || inImg2(Lbase) || inImg2(stk) ||
                        inImg2(ci) || top > slast || Lbase > top;
      static std::atomic<unsigned> s_lc{0};
      unsigned lcn = s_lc.fetch_add(1, std::memory_order_relaxed) + 1;
      static std::atomic<unsigned> s_sick{0};
      if (sick ? (s_sick.fetch_add(1, std::memory_order_relaxed) < 40)
               : (lcn <= 8 || (lcn & (lcn - 1)) == 0)) {
        SR_LOG_BOOT(sick ? sr::LogSeverity::Error : sr::LogSeverity::Info,
          "[LUA_CALL] #%u L=%08X top=%08X base=%08X stack=%08X last=%08X ci=%08X "
          "nC=%u lr=%08X%s",
          lcn, vmCtx, top, Lbase, stk, slast, ci, depth, callerLR,
          sick ? "  <<SICK LUA STACK (wipe precursor)" : "");
      }
    }
    if (depth >= 150) {
      static std::atomic<unsigned> s_deep{0};
      unsigned dn = s_deep.fetch_add(1, std::memory_order_relaxed) + 1;
      if (dn <= 120) {
        uint32_t f[4] = {0, 0, 0, 0};
        if (action >= 0x10000u) {
          const uint8_t* a = base + action;
          for (int i = 0; i < 4; ++i)
            f[i] = (uint32_t(a[i * 4]) << 24) | (uint32_t(a[i * 4 + 1]) << 16) |
                   (uint32_t(a[i * 4 + 2]) << 8) | uint32_t(a[i * 4 + 3]);
        }
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[LUA_DEPTH] nCcalls=%u L=%08X func=%08X{%08X %08X %08X %08X} lr=%08X (#%u)",
          depth, vmCtx, action, f[0], f[1], f[2], f[3], callerLR, dn);
      }
    }
  }
  const bool badDest = dest >= 0x82000000u && dest < 0x82B00000u;
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (badDest || n <= 8 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(badDest ? sr::LogSeverity::Error : sr::LogSeverity::Info,
      "[ASVM_EXEC] #%u vmCtx=%08X action=%08X dest=%08X lr=%08X%s",
      n, vmCtx, action, dest, callerLR,
      badDest ? "  <<IN-IMAGE DEST (broken AS frame - the zeroer)" : "");
  }
  // Phase 4.3 batch Lua (docs/FORCING_RETIREMENT.md): the degenerate-lua_State entry guard is
  // retired; the generated body runs for every call, as on the console.
  __imp__sub_82762E78(ctx, base);
}

PPC_FUNC(sub_82182700) {
  const uint32_t sizeArg = ctx.r4.u32;
  const uint32_t callerLR = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82182700(ctx, base);
  const uint32_t a = ctx.r3.u32;
  const bool inImage = a >= 0x82000000u && a < 0x82B00000u;
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (inImage || n <= 8) {
    SR_LOG_BOOT(inImage ? sr::LogSeverity::Error : sr::LogSeverity::Info,
      "[ALLOC2_GRANT] #%u size=0x%X -> 0x%08X lr=0x%08X%s",
      n, sizeArg, a, callerLR, inImage ? "  <<IN-IMAGE GRANT (the zeroer's buffer source?)" : "");
  }
}

PPC_FUNC(sub_8278D258) {
  uint32_t size = ctx.r3.u32;
  uint32_t callerLR = static_cast<uint32_t>(ctx.lr);
  __imp__sub_8278D258(ctx, base);
  // iter-192: sanitize garbage grants (OTHER_RECOMPS §4, surgical cut).
  // 191b/c proved a grant of 0x7D8802A6 (code-byte value, below the XEX image)
  // for size=0x8 became a vector buffer whose grow-copy zeroed the FMUI pool
  // queue fields and the Apt flow singleton/env. Any grant that is misaligned
  // or below the image end (0x82B00000 — covers below-image garbage, in-image
  // pointers, and the fabricated 0x10000 fallback-heap walk results) cannot be
  // a real DOGMA pool block; serve it from the host guest-heap instead.
  // NOT covered here: in-heap double-grants from cursor REWINDs (needs full
  // allocator hooks with range tracking — §4 structural fix).
  {
    uint32_t a = ctx.r3.u32;
    // iter-194: SR_DOGMA_HOST_ALLOC=1 → serve EVERY grant from the host bump
    // allocator (OTHER_RECOMPS §4, full policy). The REWIND/double-grant class
    // mints overlapping containers whose ops byte-strip .data (uiMgr/cfg/g3
    // wiped between dispatcher ticks — Frida 194a snapshot); value-sanitizing
    // alone can't catch overlap. Bump-serving starves the whole family.
    static const bool s_hostAll = [] {
      const char* e = std::getenv("SR_DOGMA_HOST_ALLOC");
      return e && e[0] == '1';
    }();
    // iter-394: the VM region [0x40000000, 0x7E000000) is now where kernel-
    // chosen NtAVM grants live (SR_NTAVM_VMREGION, console-accurate — golden:
    // the console process heap itself is at 0x40000000). Pool backings there
    // are LEGITIMATE; the old blanket "< 0x82B00000 = bogus" check replaced
    // every one of them and killed the boot (0/4, [DOGMA_GRANT_SANITIZE]
    // firing on clean 0x40xxxxxx grants). Keep the garbage protection for
    // everything outside both legitimate ranges.
    const bool inVmRegion = a >= 0x40000000u && a < 0x7E000000u;
    bool bogus = a != 0 && size > 0 && size <= 0x1000000u &&
                 (s_hostAll || (a & 3u) != 0 || (a < 0x82B00000u && !inVmRegion));
    // iter-462: the substitution is FORCING (it replaces the guest allocator's
    // own result) and now sits behind the master gate. The bogus grants it
    // masked were the EA ThreadPool use-after-free corrupting the small-object
    // pool (fixed in 60a90bf: 26 -> 0 per roll). With forcing off a bogus
    // grant is logged and the guest keeps its own value, as on the console.
    if (bogus && !sr::kernel::forcingAllowed()) {
      static std::atomic<unsigned> s_kept{0};
      const unsigned kn = s_kept.fetch_add(1, std::memory_order_relaxed) + 1;
      if (kn <= 20 || (kn & (kn - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[DOGMA_GRANT_SANITIZE] #%u bogus grant 0x%08X (size=0x%X lr=0x%08X) KEPT - "
          "substitution is forcing (SR_ALLOW_FORCING=1 restores it)", kn, a, size, callerLR);
      bogus = false;
    }
    if (bogus) {
      uint32_t repl = sr::kernel::guestHeapAlloc(size);
      if (!repl) repl = sr::kernel::guestHeapAllocFallback(size);
      static std::atomic<unsigned> s_san{0};
      unsigned sn = s_san.fetch_add(1, std::memory_order_relaxed) + 1;
      if (sn <= 20 || (sn & (sn - 1)) == 0) {
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[DOGMA_GRANT_SANITIZE] #%u bogus grant 0x%08X (size=0x%X lr=0x%08X) -> %s0x%08X",
          sn, a, size, callerLR, repl ? "replaced " : "REPLACE FAILED, kept ", repl ? repl : a);
      }
      if (repl) ctx.r3.u32 = repl;
    }
  }
  // Full grant trace for the boot window: the pool cursor visibly REWINDS
  // (e.g. grants 0x8A24EFB0+0x39098 then 0x8A24F038) — log every early grant
  // and any non-monotonic step to find the reset moment.
  {
    static unsigned s_n = 0;
    static uint32_t s_prevEnd = 0;
    unsigned n = ++s_n;
    uint32_t a = ctx.r3.u32;
    bool rewind = (a != 0 && a < s_prevEnd);
    // iter-461: a rewind is the allocator's normal LIFO reuse in the city
    // (~180/s); the first 200 and a 1-in-64 sample (SR_LOG_VERBOSE=1: all).
    static unsigned s_rewinds = 0;
    if (n <= 60 || (rewind && (++s_rewinds <= 200 || (s_rewinds % 64) == 0 ||
                               sr::kernel::logVerbose()))) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[DOGMA_GRANT] #%u size=0x%X -> 0x%08X lr=0x%08X%s",
        n, size, a, callerLR, rewind ? "  <<REWIND" : "");
    }
    if (a != 0 && size <= 0x10000000u) s_prevEnd = a + size;
  }
  checkOverlap("sub_8278D258(dogma)", ctx.r3.u32, size, 0, callerLR);
}

// iter-357i CORRECTED IDENTITY: sub_8276A178 is luaG_runerror(L, fmt, ...) —
// the single chokepoint for every formatted Lua runtime error ("attempt to
// call a nil value" etc.; format strings at 0x82943f6c/0x82943f90, caller
// sub_8276A4B0 = luaG_typeerror). r3 = lua_State, r4 = fmt, args r5..r10.
// The iter-188 "error subsystem object" story was this same chain throwing
// with a garbage lua_State. FSM state pcalls swallow these errors, which is
// why boot scripts can die silently — so log the formatted message here.
static void logLuaRunError(PPCContext& ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > 48 && (n & (n - 1)) != 0) return;
  auto gstr = [&](uint32_t va) -> const char* {
    if (va < 0x1000u || va >= 0xC0000000u) return nullptr;
    const char* p = reinterpret_cast<const char*>(base + va);
    for (int i = 0; i < 96; ++i) {
      unsigned char c = (unsigned char)p[i];
      if (!c) return p;
      if (c < 9 || c > 126) return nullptr;
    }
    return p;
  };
  uint32_t fmtVa = ctx.r4.u32;
  const char* fmt = gstr(fmtVa);
  char out[320];
  size_t o = 0;
  const uint64_t argv[6] = {ctx.r5.u64, ctx.r6.u64,  ctx.r7.u64,
                            ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
  int ai = 0;
  if (fmt) {
    for (const char* f = fmt; *f && o < sizeof(out) - 48; ++f) {
      if (*f != '%') { out[o++] = *f; continue; }
      ++f;
      if (!*f) break;
      if (*f == '%') { out[o++] = '%'; continue; }
      uint64_t a = (ai < 6) ? argv[ai++] : 0;
      if (*f == 's') {
        const char* s = gstr((uint32_t)a);
        o += (size_t)snprintf(out + o, sizeof(out) - o, "%s", s ? s : "<bad>");
      } else if (*f == 'd' || *f == 'c') {
        o += (size_t)snprintf(out + o, sizeof(out) - o, "%d", (int)(int32_t)a);
      } else if (*f == 'f') {
        o += (size_t)snprintf(out + o, sizeof(out) - o, "%g", ctx.f1.f64);
      } else {
        o += (size_t)snprintf(out + o, sizeof(out) - o, "?%c", *f);
      }
    }
  }
  out[o < sizeof(out) ? o : sizeof(out) - 1] = 0;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[LUA_ERR] #%u L=%08X lr=%08X msg=\"%s\" (fmt=%08X r5=%08X r6=%08X "
      "r7=%08X r8=%08X)",
      n, ctx.r3.u32, (uint32_t)ctx.lr, out, fmtVa, ctx.r5.u32, ctx.r6.u32,
      ctx.r7.u32, ctx.r8.u32);
}

PPC_FUNC(sub_8276A178) {
  uint32_t self = ctx.r3.u32;
  logLuaRunError(ctx, base);
  // iter-463 charter (burndown U1): master-gated ("[INSERT_GUARD]
  // sub_8276A178" 0/150 run logs). Default exe: log, run luaG_runerror.
  if (!plausibleGuestObj(self)) {
    static unsigned s_rej = 0;
    const bool skip = sr::kernel::forcingAllowed();
    if (++s_rej <= 12 || (s_rej & (s_rej - 1)) == 0) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[INSERT_GUARD] sub_8276A178 %s garbage this=0x%08X "
        "(lr=0x%08X r4=0x%08X rej#%u)", skip ? "rejected" : "OBSERVED (not rejected: SR_ALLOW_FORCING off)",
        self, (uint32_t)ctx.lr, ctx.r4.u32, s_rej);
    }
    if (skip) {
      ctx.r3.u32 = 0;
      return;
    }
  }
  __imp__sub_8276A178(ctx, base);
}

#endif  // SR_ORACLE diagnostics
