// iter-240: Apt movie-def PARSE-CHAIN instrumentation.
//
// We identified the .apt movie-def parser (iter-239) and mapped the full reach
// path (iter-240). This file overrides the parse-chain functions to LOG when they
// run in OUR build, so we can localize exactly where the chain breaks (the menu
// never appears because the parse is never reached).
//
// Chain (working game): manager per-tick pump sub_826D2718(manager) ->
//   sub_826D23A0 (drain completed-load list at manager+0x7c) ->
//   sub_826AECC8 (parse entry, ctx-swap) -> sub_826AEA00 -> sub_826AE6D0 (parser).
// Async front half: content-fetch -> sub_826D5FE8 (onDataArrived) -> ... ->
//   sub_826D7850 (completion trigger) -> posts a record to manager+0x7c.
// Manager singleton = DAT_829a23e0; Apt ctx at manager+0xe0; completed list +0x7c.
//
// Pure diagnostic — calls straight through to the real impl. Enable always (cheap;
// rate-limited). Look for [PARSE_TRACE] lines in the log.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "sr_oracle.hpp"
#if SR_ORACLE
#include "wffsm_obs.hpp"
#endif
#include "natural_gauges.hpp"  // iter-358: uncapped natural counters
#include "sr_timebase.hpp"     // iter-463: SR_TIMEBASE_50MHZ retires the sub_82564E80 clock
#include "game_options.hpp"    // lane E L-b: the OPTIONS GAME tab's calls of the game's own setters
#include "mouse_ui.hpp"        // iter-464: the mouse cursor's Apt menu snapshot (observe)
#include "asset_receipt.hpp"   // lane E E3-E6 r3: the [ASSET] MAIN receipt
#include "../app/log.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(_WIN32)
#include <windows.h>  // __try/__except SEH + EXCEPTION_EXECUTE_HANDLER (blob scan)
#endif

#if SR_ORACLE
// Parsing, forcing and speculative pointer-walk observers belong to Oracle.
namespace {
inline uint32_t beRead32(uint8_t* base, uint32_t va) {
  if (!base) return 0;
  return (uint32_t(base[va]) << 24) | (uint32_t(base[va + 1]) << 16) |
         (uint32_t(base[va + 2]) << 8) | uint32_t(base[va + 3]);
}

#if defined(_WIN32)
// iter-245d: one-shot heap scan for the resident .apt blob's "Apt Data" magic.
// SR_DRIVE_APTDELIVER needs the real blob VA to fill req+8; SR_FORCE_LOOSE mounts
// ScrPressStart.AST somewhere in the heap. Scan in 1MB chunks each under its own
// __try so a single unmapped page only skips that chunk, not the whole scan.
void scanRange(uint8_t* base, uint32_t lo, uint32_t hi, uint32_t& hits) {
  for (uint32_t chunk = lo; chunk < hi && hits < 64; chunk += 0x100000u) {
    __try {
      uint32_t end = chunk + 0x100000u;
      for (uint32_t va = chunk; va < end && hits < 64; ++va) {
        if (base[va] == 'A' && base[va + 1] == 'p' && base[va + 2] == 't' &&
            base[va + 3] == ' ' && base[va + 4] == 'D' && base[va + 5] == 'a' &&
            base[va + 6] == 't' && base[va + 7] == 'a') {
          char tail[28];
          for (int k = 0; k < 27; ++k) {
            uint8_t b = base[va + k];
            tail[k] = (b >= 32 && b < 127) ? (char)b : '.';
          }
          tail[27] = 0;
          SR_LOG_BOOT(sr::LogSeverity::Info,
            "[FIND_APTBLOB] 'Apt Data' magic @0x%08X : %s", va, tail);
          ++hits;
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) { /* unmapped chunk - skip */ }
  }
}

void scanForAptBlob(uint8_t* base) {
  if (!base) return;
  uint32_t hits = 0;
  // iter-245k: BGFA mounts land at LOW VAs (~0x05xxxxxx, seen via SR_TRACE_BGFA),
  // NOT 0x82-0x8F (iter-245d scanned the wrong region). Scan the low file-mount
  // region first, then the guest heap.
  scanRange(base, 0x01000000u, 0x10000000u, hits);
  scanRange(base, 0x82000000u, 0x8F000000u, hits);
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[FIND_APTBLOB] scan complete: %u hit(s) in 0x01000000-0x10000000 + 0x82000000-0x8F000000", hits);
}
#endif
} // namespace

// Public one-shot entry for the blob scan. Gate (SR_FIND_APTBLOB) + one-shot guard
// live here so callers can fire it unconditionally every tick; it no-ops after the
// first real scan or when the gate is off. Decoupled from the (roll-dependent)
// parser so the scan runs reliably once SR_FORCE_LOOSE has mounted the bytes.
extern "C" void sr_find_apt_blob(uint8_t* base) {
#if defined(_WIN32)
  static const bool s_findBlob = [] {
    const char* e = std::getenv("SR_FIND_APTBLOB");
    return e && e[0] == '1';
  }();
  static std::atomic<bool> s_scanned{false};
  if (s_findBlob && base && !s_scanned.exchange(true)) scanForAptBlob(base);
#else
  (void)base;
#endif
}

#define PARSE_TRACE_FN(NAME, LIMIT)                                                  \
  extern "C" void __imp__##NAME(PPCContext& __restrict ctx, uint8_t* base);          \
  void NAME(PPCContext& __restrict ctx, uint8_t* base) {                             \
    static std::atomic<uint32_t> s_n{0};                                            \
    uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);                        \
    if (c < (LIMIT)) {                                                               \
      SR_LOG_BOOT(sr::LogSeverity::Info,                                             \
        "[PARSE_TRACE] " #NAME " #%u r3=%08X r4=%08X r5=%08X",                       \
        c, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);                                      \
    }                                                                               \
    __imp__##NAME(ctx, base);                                                        \
  }

// Manager per-tick UPDATE pump (r3 = manager). On first call, dump manager state.
// iter-291 TIMER ROOT FIX (collapses BANDAIDS rows 86 + 87 + timer fragility).
// sub_82564E80 is the engine's tick source: (*DAT_8297932c)(). sub_82566A50 always selects
// sub_825667C8 = rdtsc/(freq/1000) = elapsed MILLISECONDS (sub_82566818 = KeTimeStampBundle+0x10
// = TickCount ms is the other branch — same unit). But our harness's underlying timebase
// counter (FUN_824680d0/sub_828EE56C) is static/0, so sub_82564E80 never advances and every
// timer-poll-with-timeout (e.g. FMGameInput sub_8229F460: while tick < start+10000) never
// expires -> infinite spin (the reason the row-87 iteration-cap exists). Return real monotonic
// host-elapsed milliseconds — the value the kernel clock would have produced. Unit-correct
// (both dispatch branches yield ms), race-free, dispatch-agnostic; callers compare sub_82564E80
// only against itself.
// iter-463 RECLASSIFIED band-aid (docs/OVERRIDES.json; it was filed platform-fix): the
// real defect is the timebase - sub_825667C8 = QPC/(QPF/1000) where QPC is the inlined
// mftb, lowered to raw __rdtsc (~74x the 50 MHz timebase), so the guest's own clock ran
// 74x fast and this override hid it behind a THIRD millisecond epoch. With
// SR_TIMEBASE_50MHZ=1 mftb is the 50 MHz guest clock and the guest's own dispatch
// (*DAT_8297932c)() yields correct ms, so the override just delegates.
// RETIRE: delete this override when SR_TIMEBASE_50MHZ is baked default-ON.
extern "C" void __imp__sub_82564E80(PPCContext& __restrict ctx, uint8_t* base);
void sub_82564E80(PPCContext& __restrict ctx, uint8_t* base) {
  if (sr::timebase::enabled()) {
    __imp__sub_82564E80(ctx, base);
    return;
  }
  static const auto t0 = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0).count();
  ctx.r3.u32 = (uint32_t)ms;
}

// iter-291 cont4 DIAGNOSTIC (SR_DIAG_STUCK_MODULES): E630's construct loop calls
// sub_8271F2C8(module, mgr) -> module->vt[0x1c]() until it returns done=1 (stored at
// module+0x44); a module that never returns done is re-ticked forever and the loop never
// transitions to the per-frame update (menu frozen). After construction settles (30s wall),
// log each still-not-done module's vtable ONCE -> maps to the stuck subsystem (the next waves).
extern "C" void __imp__sub_8271F2C8(PPCContext& __restrict ctx, uint8_t* base);
void sub_8271F2C8(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t module = ctx.r3.u32;
  // iter-385b [MODINIT] (ungated, capped): main's shallow-death = leaving
  // FmModuleInitGraphWalk ~40s before the world-flow preload completes; the
  // walk IGNORES a module Init's rc, so an early-bailing vt[0x1c] silently
  // abandons the paced preload (drain-runner then spins at 826F4734 and all
  // I/O dies). Log every FIRST-init module ENTER/EXIT with rc + duration —
  // the module whose rc/dt differs shallow-vs-deep names the killer.
  // Per-module transition table (iter-385c): the flat 300-cap exhausted in the
  // first 25s on name-string-arg calls (r3='FMRe'/'Game'... fragments) before
  // the 60-140s loading phase. Filter to real modules (vt in image .rdata),
  // then log: first tick, power-of-2 re-ticks, and EVERY rc/done transition.
  // The world-flow module's transition (rc 0->1) time is THE shallow-vs-deep
  // discriminator: early rc=1 = spurious completion; ticks just stopping =
  // the construct loop bailed.
  struct MiSlot { uint32_t mod, ticks, lastRc; };
  static MiSlot s_miTab[128];
  static std::atomic<uint32_t> s_miCnt{0};
  int miIdx = -1;
  bool miReal = false;
  if (base && module >= 0x10000u && module < 0xC0000000u) {
    uint32_t vt = beRead32(base, module);
    miReal = vt >= 0x82000000u && vt < 0x82400000u;
  }
  if (miReal && base[module + 0x44] == 0) {
    uint32_t cnt = s_miCnt.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < cnt && i < 128; ++i)
      if (s_miTab[i].mod == module) { miIdx = (int)i; break; }
    if (miIdx < 0) {
      uint32_t idx = s_miCnt.fetch_add(1, std::memory_order_acq_rel);
      if (idx < 128) {
        s_miTab[idx] = {module, 0, 0xFFu};
        miIdx = (int)idx;
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[MODINIT] FIRST mod=%08X vt=%08X init=%08X tid=%u lr=%08X",
          module, beRead32(base, module),
          beRead32(base, beRead32(base, module) + 0x1c),
          sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
      }
    }
  }
  struct MiExit {
    PPCContext* c; uint8_t* b; uint32_t mod; int idx; MiSlot* tab;
    ~MiExit() {
      if (idx < 0) return;
      MiSlot& s = tab[idx];
      s.ticks++;
      uint32_t rc = c->r3.u32 & 0xFFu;
      uint32_t done = (b[mod + 0x44] != 0) ? 1u : 0u;
      bool transition = rc != s.lastRc || done;
      bool pow2 = (s.ticks & (s.ticks - 1)) == 0;
      if (transition || pow2)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[MODINIT] mod=%08X tick#%u rc=%u done=%u tid=%u",
          mod, s.ticks, rc, done, sr::kernel::getCurrentThreadHandle());
      s.lastRc = rc;
    }
  } miExit{&ctx, base, module, miIdx, s_miTab};
  {
    // iter-291 cont7: log each tick ENTRY (module + vtable + vt[0x1c]) for the first N calls.
    // The LAST [TICK_ENTRY] before the log goes silent = the node whose construct (or whose
    // following sibling's vt[8]) blocks the dispatch walk -> names the first subsystem gate.
    static const char* enE = std::getenv("SR_DIAG_STUCK_MODULES");
    if (enE && enE[0] == '1' && base && module >= 0x1000u) {
      static std::atomic<uint32_t> s_e{0};
      uint32_t e = s_e.fetch_add(1, std::memory_order_relaxed);
      if (e < 400) {
        uint32_t vt = beRead32(base, module);
        SR_LOG_BOOT(sr::LogSeverity::Info, "[TICK_ENTRY] #%u module=%08X vt=%08X vt[0x1c]=%08X done=%u",
          e, module, vt, beRead32(base, vt + 0x1c), (base[module + 0x44] != 0) ? 1u : 0u);
      }
    }
  }
  static const char* en = std::getenv("SR_DIAG_STUCK_MODULES");
  // iter-291 cont5: time each tick; a node whose __imp__ call crawls/blocks (contended critsec
  // self-heal in Function_825FC108, or a blocking vt[0x1c]) is the one stalling E630's walk.
  if (en && en[0] == '1' && base && module >= 0x1000u) {
    auto ts = std::chrono::steady_clock::now();
    __imp__sub_8271F2C8(ctx, base);
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ts).count();
    if (dt >= 25) {
      uint32_t vt = beRead32(base, module);
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[SLOW_TICK] module=%08X vtable=%08X vt[0x1c]=%08X took %lldms (stalls E630 walk)",
        module, vt, beRead32(base, vt + 0x1c), (long long)dt);
    }
  } else {
    __imp__sub_8271F2C8(ctx, base);
  }
  if (!en || en[0] != '1' || !base || module < 0x1000u) return;
  static const auto t0 = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count() < 30)
    return;  // still constructing — only persistently-stuck modules matter
  if (base[module + 0x44] != 0) return;  // module reached done — fine
  uint32_t vt = beRead32(base, module);
  static std::atomic<uint32_t> s_seen[32]; static std::atomic<int> s_cnt{0};
  int cnt = s_cnt.load(std::memory_order_acquire);
  for (int i = 0; i < cnt; ++i) if (s_seen[i].load(std::memory_order_relaxed) == vt) return;
  int idx = s_cnt.fetch_add(1, std::memory_order_acq_rel);
  if (idx >= 32) return;
  s_seen[idx].store(vt, std::memory_order_relaxed);
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[STUCK_MODULE] module=%08X vtable=%08X vt[0x1c]=%08X never reaches done (E630 re-ticks forever)",
    module, vt, beRead32(base, vt + 0x1c));
}

#endif  // Oracle parsing diagnostics

extern "C" void __imp__sub_826D2718(PPCContext& __restrict ctx, uint8_t* base);
void sub_826D2718(PPCContext& __restrict ctx, uint8_t* base) {
  sr::kernel::gaugeBump(sr::kernel::Gauge::AptPump);  // iter-358: uncapped count
#if SR_ORACLE
  static std::atomic<uint32_t> s_n{0};
  uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
  // iter-356: the old `c < 4` cap made "+0x84/+0x7c == 0" read as a steady-state fact when
  // it was only a sample ~1.4 s into boot (the sampling trap, 5th occurrence). Golden AT THE
  // MENU has BOTH queues 0 as well (executor 0x050ABA30: +0x7c=0, +0x84=0) => they are
  // transient work queues, empty at rest, and NOT the gate.
  // Likewise DAT_829a17bc is NOT null here: the pump publishes it every tick via
  // sub_82686770(*(this+0xE0)) (ours 88A04954 / golden 05083C3C) — also not the gate.
  // The real golden divergence is one hop further in: the CATALOG at aptCtx+0x1c
  // (golden 0x050848DC, holding __Packages.IconMenu / ScrPressStart / SELECT / GlobalAd).
  // Nobody has ever measured OUR catalog. Log the first 4 ticks, then periodically.
  if (c < 4 || (c % 600) == 0) {
    uint32_t mgr = ctx.r3.u32;
    uint32_t completedHead = mgr ? beRead32(base, mgr + 0x7c) : 0;  // drain list head
    uint32_t aptCtx        = mgr ? beRead32(base, mgr + 0xe0) : 0;  // Apt context (-> 17bc)
    uint32_t pending2      = mgr ? beRead32(base, mgr + 0x84) : 0;  // 2nd queue
    auto ok = [](uint32_t va) { return va >= 0x1000u && va < 0xC0000000u; };
    uint32_t cat  = ok(aptCtx) ? beRead32(base, aptCtx + 0x1c) : 0;  // THE catalog
    uint32_t cat0 = ok(cat) ? beRead32(base, cat + 0x00) : 0;
    uint32_t cat4 = ok(cat) ? beRead32(base, cat + 0x04) : 0;
    uint32_t cat10 = ok(cat) ? beRead32(base, cat + 0x10) : 0;
    uint32_t live = beRead32(base, 0x829a17bc);  // published copy, for attribution
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[PARSE_TRACE] sub_826D2718(pump) #%u mgr=%08X +0x7c(completed)=%08X "
      "+0xe0(aptCtx)=%08X +0x84(pending)=%08X | 17bc=%08X cat(+0x1c)=%08X "
      "cat[0]=%08X cat[4]=%08X cat[10]=%08X",
      c, mgr, completedHead, aptCtx, pending2, live, cat, cat0, cat4, cat10);
  }
#endif
  __imp__sub_826D2718(ctx, base);
  // iter-464 (docs/MOUSE_CURSOR.md): the mouse cursor's snapshot of the Apt menus, on this guest
  // thread right after the pump (the AS interpreter is idle here); iter-466 controls2: then at most
  // one call of a menu's own function for the player's mouse (kernel/apt_call.hpp). Returns at
  // once while the mouse is untouched or the feature is off.
  sr::kernel::mouseUiAfterAptPump(ctx, base);
  // lane E L-b: the OPTIONS screen's GAME tab - at most one call of the opener's own SR_GameOptions (the game's
  // own setters through the custom front end's ApplyStore), FetchModOptions and SR_ShowPadLayout. Returns at once
  // when nothing is pending or port features are off.
  sr::kernel::gameopts::afterAptPump(ctx, base);
}

#if SR_ORACLE
// Drain of the completed-load list -> calls the parse entry per record.
// iter-295: pure trace (the register path needs a PARSED def structure, not raw .apt
// bytes — the decode note is in splash_hooks.cpp; the embedded blobs were deleted 2026-10-13. The proper wiring
// drives the .apt->def LOAD sub_826E83D0 on the game's own bytes, then the natural drain registers it).
PARSE_TRACE_FN(sub_826D23A0, 8)

// Parse ENTRY (ctx-swap) -> sub_826AEA00 -> the parser.
PARSE_TRACE_FN(sub_826AECC8, 16)

// onDataArrived (async ENQUEUE front half). iter-282: capture the EXACT struct/vtable
// shape from real BOOT-asset calls (param_1=reqCtx{+4,+8}, param_2=assetObj{vtable,
// vt[0x14],vt[0x1c]}) so we can mirror it to natively enqueue SCRPRESSSTART.
extern "C" void __imp__sub_826D5FE8(PPCContext& __restrict ctx, uint8_t* base);
namespace sr { namespace kernel { extern std::atomic<uint32_t> g_enqLoaderVA; extern std::atomic<uint32_t> g_enqAssetVA; extern std::atomic<uint32_t> g_menuCtx; } }
void sub_826D5FE8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<uint32_t> s_n{0};
  uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
  // iter-288: capture assetObj only. The LOADER is captured from the POSTER (sub_826D7850 r3) -- reqCtx
  // here is the request RECORD (embedded path string), NOT the loader, so do NOT write g_enqLoaderVA here
  // (it would pollute the DRAIN's loader when the poster doesn't fire).
  sr::kernel::g_enqAssetVA.store(ctx.r4.u32, std::memory_order_relaxed);
  if (c < 8) {
    uint32_t reqCtx = ctx.r3.u32, assetObj = ctx.r4.u32;
    uint32_t req4 = beRead32(base, reqCtx + 4), req8 = beRead32(base, reqCtx + 8);
    uint32_t vt = (assetObj >= 0x1000u && assetObj < 0xC0000000u) ? beRead32(base, assetObj) : 0;
    uint32_t vt14 = (vt >= 0x1000u && vt < 0xC0000000u) ? beRead32(base, vt + 0x14) : 0;
    uint32_t vt1c = (vt >= 0x1000u && vt < 0xC0000000u) ? beRead32(base, vt + 0x1c) : 0;
    // assetObj name often at assetObj+8 (BigFileAsset layout)
    char nm[40]; nm[0] = 0;
    if (assetObj >= 0x1000u && assetObj < 0xC0000000u) {
      for (int i = 0; i < 39; ++i) { char ch = (char)base[assetObj + 8 + i]; nm[i] = (ch >= 32 && ch < 127) ? ch : 0; if (!ch) break; nm[i+1]=0; }
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[ENQUEUE_STRUCT] sub_826D5FE8 #%u reqCtx=%08X(+4=%08X +8=%08X) assetObj=%08X vt=%08X vt[0x14]=%08X vt[0x1c]=%08X name='%s'",
      c, reqCtx, req4, req8, assetObj, vt, vt14, vt1c, nm);
  }
  __imp__sub_826D5FE8(ctx, base);
}

// Completion POSTER (loader r3, subAsset r4, status r5). iter-286: it FIRES NATURALLY now (twice, for the
// 2-part package) -> on refcount 0 posts a node to *(loader+0x10)+0x7c. Capture the REAL loader (r3) so the
// LOADPKG drive can drain the CORRECT post-target mgr (= *(loader+0x10)), not resMgr.
extern "C" void __imp__sub_826D7850(PPCContext& __restrict ctx, uint8_t* base);
void sub_826D7850(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<uint32_t> s_n{0};
  uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
  uint32_t loader = ctx.r3.u32;
  sr::kernel::g_enqLoaderVA.store(loader, std::memory_order_relaxed);
  if (c < 8) {
    uint32_t post = (loader >= 0x1000u && loader < 0xC0000000u) ? beRead32(base, loader + 0x10u) : 0;
    uint32_t rc   = (loader >= 0x1000u && loader < 0xC0000000u) ? beRead32(base, loader + 0x18u) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[POSTER_FIRE] sub_826D7850 #%u loader=%08X subAsset=%08X status=%d | loader+0x10(post)=%08X +0x18(rc)=%u",
      c, loader, ctx.r4.u32, (int)ctx.r5.u32, post, rc);
  }
  __imp__sub_826D7850(ctx, base);
}

// iter-245h: loadMovie FETCH-ENQUEUE chain (sub_826B7378's load branch calls these).
// If they fire, loadMovie reached the fetch path -> the wall is the async completion
// (sub_826D5FE8/D7850 never firing). If they do NOT fire, loadMovie bailed earlier
// (resolve miss or node already-loaded/wrong-type) -> the wall is the resolve. This
// bisects why loadMovie ret=1 but enqueues nothing.
PARSE_TRACE_FN(sub_826C5260, 8)
PARSE_TRACE_FN(sub_826C2E58, 8)
PARSE_TRACE_FN(sub_826C9270, 8)

// iter-245i: the Apt EXECUTOR sub_826BF630 walks _level0's pending work-lists and, for
// a pending movie-load, calls sub_826AF688 (movie-build) -> sub_826B0358 (install).
// We never drive the executor, so loadMovie's queued request on _level0 is never
// processed. Log the movie-build to confirm the executor reaches it when driven.
PARSE_TRACE_FN(sub_826AF688, 8)

// iter-283: native load-by-name chain (SR_DRIVE_LOADPKG). Pinpoint where sub_826D2690 bails
// before the enqueue (sub_826D5FE8). sub_826D91F0(r3=mgr, r4=name): confirm it fires + mgr/name.
// sub_826D8A10(r3=loader, r4=descriptor): r4==0 => package descriptor did NOT resolve (registry miss).
PARSE_TRACE_FN(sub_826D91F0, 4)
// iter-284: THE FMUserInterface module init (descriptor @0x82003400 = {sub_821516E0, sub_82150EE0,
// "FMUserInterface",...}). It builds the AptAux::RtamPackage mgr (sub_826D9C40 -> 0x8295F0E4), installs
// resMgr, creates the +0xd4 scope, builds the ctx graph (sub_826D3FD0). Xenia golden: called ONCE with
// r3=0x8295FC8C (the module's STATIC object). OUR build's dispatcher passes r3=0 (bug) -> it can't init
// -> mgr never registered -> the whole menu package-load chain bails. FIX (SR_FIX_FMUI_MODULE): when the
// natural call arrives with r3==0, substitute the static module object 0x8295FC8C so the init runs for real.
extern "C" void __imp__sub_821516E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_828DBC88(PPCContext& __restrict ctx, uint8_t* base);  // FMUserInterface static ctor: sets *(0x8295FC8C)=vt 0x820033E4 + registers into DAT_829a2bb0 (XEX .ctors never ran in our build)
void sub_821516E0(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_fix = [] { const char* e = std::getenv("SR_FIX_FMUI_MODULE"); return e && e[0] == '1'; }();
  uint32_t r3in = ctx.r3.u32;
  if (s_fix && r3in == 0) {
    // FIX 1: the dispatcher passes r3=0 AND 0x8295FC8C is zeroed BSS whose static ctor never ran.
    // Run sub_828DBC88 first so the module object gets its vtable (0x820033E4) + list registration --
    // otherwise sub_821516E0 runs against a vtable-less object (the racy-garbage roll). Then pass it.
    uint32_t vtBefore = beRead32(base, 0x8295FC8Cu);
    if (vtBefore == 0) {
      PPCContext cc{}; cc.r1.u32 = ctx.r1.u32 - 0x400u; cc.r13.u32 = ctx.r13.u32; cc.r14.u32 = 0x829C0000u; cc.lr = 0x821516E0u;
      __imp__sub_828DBC88(cc, base);
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FMUI_MODULE] ran static ctor sub_828DBC88; 0x8295FC8C vtable %08X -> %08X (exp 820033E4)",
        vtBefore, beRead32(base, 0x8295FC8Cu));
    }
    ctx.r3.u32 = 0x8295FC8Cu;
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[FMUI_MODULE] sub_821516E0 entry r3=%08X%s", r3in,
    (s_fix && r3in == 0) ? " -> SUBSTITUTED module obj 0x8295FC8C" : "");
  __imp__sub_821516E0(ctx, base);
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[FMUI_MODULE] sub_821516E0 returned; 0x8295F0E4(AptAuxMgr)=%08X resMgr(829A23E0)=%08X",
    beRead32(base, 0x8295F0E4u), beRead32(base, 0x829A23E0u));
}
PARSE_TRACE_FN(sub_826D9C40, 2)
// iter-284: does sub_821516E0 reach scope-create (resMgr->vt[0x10]=sub_826D4AB0)? If it fires, the scope
// is created -> the resMgr+0xd4 handle should resolve; if not, sub_821516E0 bails before it.
PARSE_TRACE_FN(sub_826D4AB0, 4)
// iter-284: sub_826D2188(r3=name, r4=holder, r5=ctx). mgr=*(*(ctx+4)+4); mgr->vt[0xc] should == sub_826D91F0.
// Log the whole chain to find which link breaks (ctx handle-resolve / scope *(ctx+4) / registered mgr / vt).
extern "C" void __imp__sub_826D2188(PPCContext& __restrict ctx, uint8_t* base);
void sub_826D2188(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<uint32_t> s_n{0};
  uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
  if (c < 6) {
    auto ok = [](uint32_t v){ return v >= 0x1000u && v < 0xC0000000u; };
    uint32_t cx = ctx.r5.u32;
    uint32_t cx4 = ok(cx) ? beRead32(base, cx + 4) : 0;
    uint32_t mgr = ok(cx4) ? beRead32(base, cx4 + 4) : 0;
    uint32_t vt  = ok(mgr) ? beRead32(base, mgr) : 0;
    uint32_t vtc = ok(vt) ? beRead32(base, vt + 0xc) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[D2188] #%u name=%08X ctx(r5)=%08X *(ctx+4)=%08X mgr=*(.+4)=%08X vt=%08X vt[0xc]=%08X (exp 826D9350)",
      c, ctx.r3.u32, cx, cx4, mgr, vt, vtc);
  }
  __imp__sub_826D2188(ctx, base);
}

// iter-245k: BGFA asset constructor. param_4 (r6) = the mounted-file data base (stored
// at obj+0x100); it parses a 0x40-byte entry header from there. Log the mount base +
// first 16 bytes so we learn WHERE ScrPressStart.AST lands and whether its bytes are a
// plain BGFA/Apt magic (uncompressed -> scannable) or packed (need decompress). Gate
// SR_TRACE_BGFA. This answers the recurring "where are the .apt bytes" question.
extern "C" void __imp__sub_82700F80(PPCContext& __restrict ctx, uint8_t* base);
void sub_82700F80(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_trace = [] {
    const char* e = std::getenv("SR_TRACE_BGFA");
    return e && e[0] == '1';
  }();
  uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32;
  // iter-255: r5 = the filename BFAL passes ("<name>.AST"); capture it before the call.
  char fname[80]; fname[0] = 0;
  if (s_trace && base && r5 >= 0x10000u && r5 < 0xF0000000u) {
    for (int i = 0; i < 79; ++i) { char ch = (char)base[r5 + i]; if (!ch) { fname[i] = 0; break; } fname[i] = (ch >= 32 && ch < 127) ? ch : '.'; fname[i + 1] = 0; }
  }
  __imp__sub_82700F80(ctx, base);
  if (s_trace) {
    static std::atomic<uint32_t> s_n{0};
    uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
    if (c < 24) {
      // The REAL success marker is parseCtx+0x149 (set to 1 only on a complete parse); the return
      // r3 is always param_1 (the ctx) so it's meaningless. r3 (entry) = param_1 = the parse ctx.
      uint8_t okByte = (base && r3 >= 0x10000u && r3 < 0xF0000000u) ? base[r3 + 0x149] : 0xFFu;
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[REALPARSE] sub_82700F80 #%u fn(r5)='%s' mode(r4)=%u parseCtx=%08X SUCCESS(+0x149)=%u (1=parsed OK)",
        c, fname, r4, r3, (unsigned)okByte);
      // iter-260: on a SUCCESSFUL parse, dump the def-table + the active ctx's export-dict slot, so
      // we can wire the parsed defs (parseCtx+0x124) into the resolver's slot *(DAT_829A17BC+0x1c).
      if (okByte == 1 && base && r3 >= 0x10000u && r3 < 0xF0000000u) {
        uint32_t aptCtx = beRead32(base, 0x829A17BCu);
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[REALPARSE_DEFS] parseCtx=%08X +0x14=%08X +0x124(defTable)=%08X | aptCtx(0x829A17BC)=%08X +0x1c(exportDict)=%08X +0x18=%08X",
          r3, beRead32(base, r3 + 0x14u), beRead32(base, r3 + 0x124u), aptCtx,
          (aptCtx >= 0x10000u && aptCtx < 0xF0000000u) ? beRead32(base, aptCtx + 0x1cu) : 0,
          (aptCtx >= 0x10000u && aptCtx < 0xF0000000u) ? beRead32(base, aptCtx + 0x18u) : 0);
        // iter-261: the raw-blob + len args sub_826AECC8 needs to re-parse+register under our ctx.
        // blob=*(parseCtx+0x100), end/size hints at +0x104/+0x108. Captured for the register drive.
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[REALPARSE_BLOB] fn='%s' parseCtx=%08X blob(+0x100)=%08X +0x104=%08X +0x108=%08X +0x118(type)=%08X",
          fname, r3, beRead32(base, r3 + 0x100u), beRead32(base, r3 + 0x104u),
          beRead32(base, r3 + 0x108u), beRead32(base, r3 + 0x118u));
        // iter-268 Phase-1: verify the parsed def format the resolver accepts. def=parseCtx (or its
        // def-table @+0x124); the resolver accepts a def whose tag *(def+4) rotates (ror7, arith>>0x19)
        // to 0xC (clip) or 0x25 (sprite). Check both candidates so Phase-2 register feeds the right one.
        {
          uint32_t defTbl = beRead32(base, r3 + 0x124u);
          auto rot = [](uint32_t t) -> uint32_t { uint32_t r = (t << 25) | (t >> 7); return ((int32_t)r >> 25) & 0x7Fu; };
          uint32_t tagC = beRead32(base, r3 + 4u);
          uint32_t tagD = (defTbl >= 0x1000u && defTbl < 0xC0000000u) ? beRead32(base, defTbl + 4u) : 0;
          SR_LOG_BOOT(sr::LogSeverity::Info,
            "[REALPARSE_FMT] parseCtx+4 tag=%08X rot=0x%X | defTbl=%08X defTbl+4 tag=%08X rot=0x%X (want 0xC/0x25)",
            tagC, rot(tagC), defTbl, tagD, rot(tagD));
        }
      }
      // Peek the first 16 bytes at the data base (r6) as ASCII to spot a magic.
      char head[17]; head[16] = 0;
      bool sane = (r6 >= 0x10000u && r6 < 0xC0000000u);
      if (sane) {
        for (int k = 0; k < 16; ++k) {
          uint8_t b = base[r6 + k];
          head[k] = (b >= 32 && b < 127) ? (char)b : '.';
        }
      } else { head[0] = 0; }
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[BGFA] #%u obj=%08X mode=%u r5=%08X dataBase(r6)=%08X r7=%08X head='%s'",
        c, r3, r4, r5, r6, r7, sane ? head : "(unmapped)");
    }
    // iter-245k: when a real low-VA mount data base appears, trigger the .apt magic
    // scan right then (robust -- tied to the mount event, not the fragile actCtx tick).
    if (r6 >= 0x01000000u && r6 < 0x10000000u) { sr_find_apt_blob(base); }
  }
}

// iter-244: ROOT-CAUSE FIX for the null Apt context. The Apt VM init sub_826B2DC0
// is reached in our build via sub_826D4930->sub_826B8798->sub_826B2DC0 but with
// param_2 (r4) = 0, and with that flag it NULLs the context globals
// (DAT_829a17b8/17bc/17c0 = 0). With param_2 != 0 it instead BUILDS + installs the
// default context (using the 0x40-byte descriptor already in r3). Force r4=1 so the
// context is established -> 0x829A17BC becomes valid -> sub_826B05F0(0) can create
// _level0 and the parser sub_826AE6D0 gets a valid *(0x829A17BC+0x1c). Gated on
// SR_FORCE_APTCTX. Logs every call's r3/r4/lr so we can see/tune which call we hit.
extern "C" void __imp__sub_826B2DC0(PPCContext& __restrict ctx, uint8_t* base);
void sub_826B2DC0(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_forceCtx = [] {
    // 2026-07-16 charter cleanup: SR_FORCE_APTCTX forces sub_826B2DC0 r4=0->1,
    // fabricating a HOLLOW Apt ctx the guest deliberately nulled — and iter-263
    // proved it MASKS the real FMUI-built ctx. That is FORCING (a guest decision
    // made for it), so it must sit behind the master gate. Default exe now forces
    // nothing here. Bisected safe: with this off the module walk + FSM still run.
    if (!sr::kernel::forcingAllowed()) return false;
    const char* e = std::getenv("SR_FORCE_APTCTX");
    return !e || e[0] != '0';  // diagnostic only (SR_ALLOW_FORCING=1); =0 A/Bs off
  }();
  static std::atomic<uint32_t> s_n{0};
  uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
  uint32_t origR4 = ctx.r4.u32;
  if (s_forceCtx && origR4 == 0) {
    ctx.r4.u64 = 1;
  }
  if (c < 16) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[APTCTX] sub_826B2DC0 #%u r3(desc)=%08X r4=%08X%s lr=%08X",
      c, ctx.r3.u32, origR4,
      (s_forceCtx && origR4 == 0) ? "->1(FORCED)" : "", ctx.lr);
  }
  __imp__sub_826B2DC0(ctx, base);
  {
    uint32_t ctxNow =
      (uint32_t(base[0x829A17BC]) << 24) | (uint32_t(base[0x829A17BD]) << 16) |
      (uint32_t(base[0x829A17BE]) << 8) | uint32_t(base[0x829A17BF]);
    // iter-295: latch the freshly-established ctx into the STABLE g_menuCtx.
    // DAT_829A17BC is transient (published per-call, 0 between calls), so 1Hz
    // pollers (the BLOB_INJECT drive in splash_hooks.cpp) can never catch it
    // live — this hook IS the publication site.
    if (ctxNow >= 0x82000000u && ctxNow < 0x8F000000u)
      sr::kernel::g_menuCtx.store(ctxNow, std::memory_order_relaxed);
    if (c < 16) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[APTCTX] after sub_826B2DC0 #%u: 0x829A17BC=%08X 0x829A17B8=%08X",
        c, ctxNow,
        (uint32_t(base[0x829A17B8]) << 24) | (uint32_t(base[0x829A17B9]) << 16) |
          (uint32_t(base[0x829A17BA]) << 8) | uint32_t(base[0x829A17BB]));
    }
  }
}

// iter-455j: the H7c repeat-record observer lives in kernel/apt_h7_entrywatch.cpp
// but has to be DRIVEN from here — sub_826AE6D0 already carries the strong
// definition below, and a second one would not link. Gated inside (SR_H7_ENTRYWATCH,
// default OFF); first sighting of a record is silent, repeats print first-vs-current.
#if SR_ORACLE
namespace sr { namespace kernel { namespace h7 {
void noteParseRecordFromParser(uint32_t blobVA, uint32_t headerVA, uint32_t nodeVA);
} } }
#endif

// THE parser/finalizer. Log ALL calls (should be rare) with extra detail:
//   r3 = aptCtx allocator table, r4 = &holder, r5 = blob, r6 = header, r7 = node.
//
// ⚠ iter-455j CORRECTION (director-found): this comment previously read
// "r5 = dataLen, r6 = blob". That is WRONG and it had been wrong for a long time --
// IDA at 0x826AE70C-0x826AE71C shows `v11 = *(a4+20); if (v11) v11 += a3;
// v12 = (int *)(v11 + 16);`, i.e. a3(=r5) is the BASE added to the header's +20
// offset and a4(=r6) is the header. The [BGFA]/[PARSE_TRACE] "blob=%08X" field below
// therefore prints the HEADER, not the blob. Left as-is so old traces stay
// comparable, but do not read that column as a blob base.
extern "C" void __imp__sub_826AE6D0(PPCContext& __restrict ctx, uint8_t* base);
void sub_826AE6D0(PPCContext& __restrict ctx, uint8_t* base) {
  // Pre-call, so record[0] is read BEFORE the parser touches it (director item 9).
#if SR_ORACLE
  sr::kernel::h7::noteParseRecordFromParser(ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
#endif
  static std::atomic<uint32_t> s_n{0};
  uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
  // iter-260: the registrar (sub_826ADD40, run inside here) writes parsed exports into the CURRENT
  // ctx's export dict at *(DAT_829A17BC+0x1c) -- the SAME slot the loadMovie resolver reads. If the
  // dict GROWS here, registration works (then it's a ctx-mismatch vs resolve time); if it stays
  // empty, the registrar isn't effectively running. Cheap decisive probe (observability only).
  uint32_t aptCtxVA = beRead32(base, 0x829A17BCu);
  uint32_t dictBefore = (aptCtxVA >= 0x10000u && aptCtxVA < 0xF0000000u) ? beRead32(base, aptCtxVA + 0x1cu) : 0;
  if (c < 64) {
    uint32_t blob = ctx.r6.u32;
    uint32_t defField = blob ? beRead32(base, blob + 0x14) : 0;  // def = *(blob+0x14)+blob
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[PARSE_TRACE] *** sub_826AE6D0(PARSER) #%u r3=%08X holder=%08X len=%08X "
      "blob=%08X blob+0x14=%08X ctx=%08X dict(+0x1c)=%08X lr=%08X",  // iter-315: lr names the loader
      c, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, blob, defField, aptCtxVA, dictBefore,
      (uint32_t)ctx.lr);
  }
  // iter-245d: one-shot heap scan for the resident .apt blob (also driven per-tick
  // from splash_hooks; whichever fires first wins via the internal guard).
  sr_find_apt_blob(base);
  // iter-295: the actual blob delivery is driven from the sub_826D23A0 drain hook below
  // (the reliably-firing level); this hook stays a pure trace so we can see the driven
  // parse land with real r5(len)/r6(blob) here.
  // lane E E3-E6 r3: the [ASSET] receipt - which MAIN the game registers (r5 = the raw .apt, r6 = the raw .const,
  // compared before the parser relocates anything; kernel/asset_receipt.hpp)
  sr::kernel::assetreceipt::onParseGuest(ctx.r5.u32, ctx.r6.u32);
  __imp__sub_826AE6D0(ctx, base);
  if (c < 64) {
    uint32_t ctxAfter = beRead32(base, 0x829A17BCu);
    uint32_t dictAfter = (ctxAfter >= 0x10000u && ctxAfter < 0xF0000000u) ? beRead32(base, ctxAfter + 0x1cu) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[PARSE_TRACE] sub_826AE6D0 #%u RET ctx=%08X exportDict(+0x1c) %08X->%08X %s",
      c, ctxAfter, dictBefore, dictAfter, (dictAfter != dictBefore ? "(GREW=registered)" : "(unchanged)"));
  }
}

// iter-246h: APT SUBSYSTEM LIFECYCLE TRACE (gate SR_TRACE_APT_LIFECYCLE=1).
// Live-Xenia + forcing-off recomp tests (iter-246a..g) showed our recomp CREATES the Apt
// resource subsystem (resIO 0x829A23E4 / resMgr 0x829A23E0, constructed correctly) then
// TEARS IT DOWN during boot, before any movie starts -> the current-ctx 0x829A17BC is
// never set. HW watchpoints can't catch the writer (the recomp's VEH eats the #DB), so
// host-override loggers + a PPC back-chain walk pin WHO creates and WHO destroys it.
static bool aptLifecycleOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_TRACE_APT_LIFECYCLE");
    return e && e[0] == '1';
  }();
  return on;
}

// PPC linkage back-chain walk at function ENTRY: ctx.lr = immediate caller; then the
// saved-LR at *(sp+4) climbing the *(sp) back-chain. Logs up to 6 return addresses ->
// va_to_source/Ghidra each to read the call path.
static void logAptBacktrace(const char* tag, uint32_t r3, PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t f[6] = {0, 0, 0, 0, 0, 0};
  int n = 0;
  f[n++] = static_cast<uint32_t>(ctx.lr);
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < 5 && sp >= 0x1000u && sp < 0xC0000000u; ++i) {
    f[n++] = beRead32(base, sp + 4);
    uint32_t next = beRead32(base, sp);
    if (next <= sp || next >= 0xC0000000u) break;
    sp = next;
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[APT_LIFECYCLE] %s r3=%08X lr-chain: %08X %08X %08X %08X %08X %08X",
    tag, r3, f[0], f[1], f[2], f[3], f[4], f[5]);
}

// resIO teardown: sub_826D12A8 sets DAT_829a23e4 (resIO)=0 and does nothing else, so its
// caller IS the teardown trigger. DEFINITIVELY catches the resIO demolition, any path.
extern "C" void __imp__sub_826D12A8(PPCContext& __restrict ctx, uint8_t* base);
void sub_826D12A8(PPCContext& __restrict ctx, uint8_t* base) {
  if (aptLifecycleOn()) {
    static std::atomic<uint32_t> s_n{0};
    if (s_n.fetch_add(1, std::memory_order_relaxed) < 6)
      logAptBacktrace("RESIO_TEARDOWN(sub_826D12A8)", ctx.r3.u32, ctx, base);
  }
  __imp__sub_826D12A8(ctx, base);
}

// Apt subsystem teardown parent (nulls resIO via sub_826D12A8 + runs destructors).
extern "C" void __imp__sub_82150EE0(PPCContext& __restrict ctx, uint8_t* base);
void sub_82150EE0(PPCContext& __restrict ctx, uint8_t* base) {
  if (aptLifecycleOn()) {
    static std::atomic<uint32_t> s_n{0};
    if (s_n.fetch_add(1, std::memory_order_relaxed) < 6)
      logAptBacktrace("APT_TEARDOWN(sub_82150EE0)", ctx.r3.u32, ctx, base);
  }
  // iter-296f: the "resMgr appears then VANISHES" roll — the FM framework
  // dispatches this module shutdown (vtable slot 0x82003404) mid-boot after
  // the DOGMA-contended FMUI ctor fails, destroying the Apt resMgr
  // (sub_826D2688 -> sub_826D2108 zeroes DAT_829A23E0) and the flow client.
  // While NO menu screen has ever existed (aptCount 0x829A19B0 == 0) a UI
  // teardown is never the desired outcome: refuse it, report success, and
  // let the watcher's ctor retries operate on the preserved state.
  // SR_FMUI_SHUTDOWN_GUARD=0 keeps the A/B.
  {
    static const bool guard = [] {
      // 2026-07-16 charter cleanup: blocking a shutdown the guest itself dispatched
      // is FORCING (overriding a guest decision). Behind the master gate now; the
      // default exe lets the guest tear down if it chooses. The real fix is making
      // the FMUI ctor not fail under DOGMA contention, not blocking the shutdown.
      if (!sr::kernel::forcingAllowed()) return false;
      const char* e = std::getenv("SR_FMUI_SHUTDOWN_GUARD");
      return !e || e[0] != '0';
    }();
    if (guard && sr::kernel::readGuestU32BE(0x829A19B0u) == 0) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[FMUI_SHUTDOWN_GUARD] BLOCKED boot-time FMUI shutdown "
          "(caller lr=%08X r3=%08X aptCount=0) — resMgr/flow client preserved",
          (uint32_t)ctx.lr, ctx.r3.u32);
      ctx.r3.u64 = 1;   // report success; framework proceeds
      return;
    }
  }
  __imp__sub_82150EE0(ctx, base);
}

// resMgr init: sub_826D4930 creates DAT_829a23e0 (resMgr) when null. The CREATION side.
// FMUserInterface_GlobalInit (0x82151DA0) isn't a recomp function symbol (reached by
// fall-through), so we can't override it directly. But r30 is callee-saved, so at THIS
// function's entry ctx.r30 still holds GlobalInit's r30 -> read its UI-load gate
// *(*(r30-0x278c)+0x90) here (==1 => GlobalInit loads the initial UI elements/movie).
extern "C" void __imp__sub_826D4930(PPCContext& __restrict ctx, uint8_t* base);
void sub_826D4930(PPCContext& __restrict ctx, uint8_t* base) {
  bool lg = aptLifecycleOn();
  static std::atomic<uint32_t> s_n{0};
  uint32_t c = lg ? s_n.fetch_add(1, std::memory_order_relaxed) : 99u;
  // Decisive per-call diagnosis: resIO ready at entry? resMgr created at exit?
  //   resIO==0 at entry      -> allocator not ready (ordering/race)
  //   resIO!=0, resMgr==0 out -> the 0xD8 alloc FAILED (pool not ready/exhausted)
  //   resMgr!=0 out then null later -> torn down (teardown loggers catch it)
  if (lg && c < 6) {
    uint32_t r30 = ctx.r30.u32;  // == FMUserInterface_GlobalInit's r30 (callee-saved)
    uint32_t obj = (r30 >= 0x3000u && r30 < 0xC0000000u) ? beRead32(base, r30 - 0x278cu) : 0;
    uint32_t gate = (obj >= 0x1000u && obj < 0xC0000000u) ? beRead32(base, obj + 0x90u) : 0xFFFFFFFFu;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[APT_LIFECYCLE] RESMGR_INIT enter #%u resIO@23E4=%08X resMgr@23E0=%08X lr=%08X "
      "FMgate r30=%08X obj=%08X gate(+0x90)=%08X(==1 loads UI)",
      c, beRead32(base, 0x829A23E4u), beRead32(base, 0x829A23E0u), (uint32_t)ctx.lr,
      r30, obj, gate);
  }
  __imp__sub_826D4930(ctx, base);
  if (lg && c < 6) {
    // iter-263: did the factory's first-init block create the Apt heap? heap@0AA8 set by sub_826885A0
    // via (*osAlloc@1A60)(0x20). heap==0 after => first-init skipped (DAT_829a23e0 pre-set) OR
    // osAlloc@1A60 null. This is the foundational gate behind the whole dormant Apt subsystem.
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[APT_LIFECYCLE] RESMGR_INIT exit  #%u resIO@23E4=%08X resMgr@23E0=%08X heap@0AA8=%08X osAlloc@1A60=%08X heap2@0AD8=%08X",
      c, beRead32(base, 0x829A23E4u), beRead32(base, 0x829A23E0u),
      beRead32(base, 0x829A0AA8u), beRead32(base, 0x829A1A60u), beRead32(base, 0x829A0AD8u));
  }
}

// iter-247 (agent team converged): the boot-movie ARMING. FUN_82682C08 (sub_82682C08) is the
// SOLE writer of the boot-asset bytestream globals DAT_829A0A90/_A94/_A98 -- it arms the
// fetch. It runs ONLY from the FMUI ctor Function_826D3FD0 (sub_826D3FD0) tail when the
// boot-asset descriptor *(param_2+0x20) != 0 AND descVt[0x8]()!=0 AND descVt[0xc]()==0. Our
// pipeline is dormant because this never fires. These two loggers resolve descriptor-null vs
// predicate-false (the precise root) in one run. Gate SR_TRACE_APT_LIFECYCLE.
extern "C" void __imp__sub_826D3FD0(PPCContext& __restrict ctx, uint8_t* base);
void sub_826D3FD0(PPCContext& __restrict ctx, uint8_t* base) {
  if (aptLifecycleOn()) {
    static std::atomic<uint32_t> s_n{0};
    if (s_n.fetch_add(1, std::memory_order_relaxed) < 8) {
      uint32_t p2 = ctx.r4.u32;
      uint32_t desc = (p2 >= 0x1000u && p2 < 0xC0000000u) ? beRead32(base, p2 + 0x20u) : 0;
      uint32_t dvt  = (desc >= 0x1000u && desc < 0xC0000000u) ? beRead32(base, desc) : 0;
      // iter-263: movieDef = puVar6[1] = *(r3+4). sub_826D3FD0 only builds the real ctx
      // (sub_826AF590->sub_826AF460) when movieDef is valid (reads dims movieDef+0x34..0x48).
      // movieDef==0 => no genuine ctx graph -> the dormant-Apt root.
      uint32_t r3 = ctx.r3.u32;
      uint32_t mdef = (r3 >= 0x1000u && r3 < 0xC0000000u) ? beRead32(base, r3 + 4u) : 0;
      uint32_t dim0 = (mdef >= 0x1000u && mdef < 0xC0000000u) ? beRead32(base, mdef + 0x34u) : 0;
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[APT_LIFECYCLE] FMUI_CTOR sub_826D3FD0 r3=%08X r4(src)=%08X desc(*r4+0x20)=%08X "
        "descVt=%08X DAT_829A0A90=%08X movieDef(r3+4)=%08X dim(+0x34)=%08X heap@0AA8=%08X",
        r3, p2, desc, dvt, beRead32(base, 0x829A0A90u), mdef, dim0, beRead32(base, 0x829A0AA8u));
    }
  }
  uint32_t feObjVA = ctx.r3.u32;
  __imp__sub_826D3FD0(ctx, base);
  // iter-263: after the FMUI ctor, did it build the ctx graph at FE-obj+0xe0 (working game: 0x5083c3c
  // with +0x18/+0x1c/+0x20 non-null)? This is THE measurement: feObj+0xe0==0 => sub_826AF590 never
  // built/stored the ctx; non-null-but-+0x1c==0 => degenerate (dims=0) graph.
  if (aptLifecycleOn()) {
    static std::atomic<uint32_t> s_m{0};
    if (s_m.fetch_add(1, std::memory_order_relaxed) < 8 && feObjVA >= 0x1000u && feObjVA < 0xC0000000u) {
      uint32_t ctxVA = beRead32(base, feObjVA + 0xe0u);
      uint32_t s18 = (ctxVA >= 0x1000u && ctxVA < 0xC0000000u) ? beRead32(base, ctxVA + 0x18u) : 0;
      uint32_t s1c = (ctxVA >= 0x1000u && ctxVA < 0xC0000000u) ? beRead32(base, ctxVA + 0x1cu) : 0;
      uint32_t s20 = (ctxVA >= 0x1000u && ctxVA < 0xC0000000u) ? beRead32(base, ctxVA + 0x20u) : 0;
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[APT_LIFECYCLE] FMUI_CTOR exit feObj=%08X +0xe0(ctx)=%08X ctx+0x18=%08X +0x1c(scope)=%08X +0x20(clipmgr)=%08X %s",
        feObjVA, ctxVA, s18, s1c, s20,
        (s1c != 0 && s20 != 0) ? "*** CTX GRAPH BUILT ***" : (ctxVA != 0 ? "(ctx exists, graph degenerate)" : "(no ctx built)"));
      // iter-263 FIX: the FE-obj ctor builds the REAL populated ctx at FE-obj+0xe0, but SR_FORCE_APTCTX
      // masks it by forcing a hollow synthetic ctx into DAT_829A17BC. Point the active ctx at the REAL
      // one so loadMovie's register+resolve hit the populated scope. Gate SR_USE_REAL_CTX (run WITHOUT
      // SR_FORCE_APTCTX). Latch once a real graph exists.
      static const bool s_useRealCtx = [] { const char* e = std::getenv("SR_USE_REAL_CTX"); return e && e[0] == '1'; }();
      static std::atomic<bool> s_pub{false};
      if (s_useRealCtx && s1c != 0 && s20 != 0 && ctxVA >= 0x1000u && ctxVA < 0xC0000000u && !s_pub.exchange(true)) {
        base[0x829A17BCu] = (uint8_t)(ctxVA >> 24); base[0x829A17BDu] = (uint8_t)(ctxVA >> 16);
        base[0x829A17BEu] = (uint8_t)(ctxVA >> 8);  base[0x829A17BFu] = (uint8_t)ctxVA;
        sr::kernel::g_menuCtx.store(ctxVA, std::memory_order_relaxed);  // iter-288: STABLE menu-ctx for the DRAIN UNIFY (DAT_829A17BC is transient/0 mid-tick)
        SR_LOG_BOOT(sr::LogSeverity::Info, "[APT_LIFECYCLE] *** SR_USE_REAL_CTX: DAT_829A17BC <- real ctx 0x%08X (scope 0x%08X) ***", ctxVA, s1c);
      }
    }
  }
}

extern "C" void __imp__sub_82682C08(PPCContext& __restrict ctx, uint8_t* base);
void sub_82682C08(PPCContext& __restrict ctx, uint8_t* base) {
  if (aptLifecycleOn()) {
    static std::atomic<uint32_t> s_n{0};
    if (s_n.fetch_add(1, std::memory_order_relaxed) < 8)
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[APT_LIFECYCLE] *** ARM sub_82682C08 stream=%08X len=%08X -- boot-asset bytestream ARMED",
        ctx.r3.u32, ctx.r4.u32);
  }
  __imp__sub_82682C08(ctx, base);
}

// iter-270: the per-frame Apt display-list EXECUTOR (pump sub_826C0888 -> this). r3 = clip mgr =
// *(DAT_829A17BC+0x20). It draws the root clip's display list. Probe: does it run in our build, and
// is the root clip (DAT_829A09E0) + its display list populated, or empty (=> nothing real to draw,
// backend shows placeholder)? This pins where our pixels stop, at the render layer. Gate SR_TRACE_APT_LIFECYCLE.
extern "C" void __imp__sub_826BF630(PPCContext& __restrict ctx, uint8_t* base);
void sub_826BF630(PPCContext& __restrict ctx, uint8_t* base) {
  if (aptLifecycleOn()) {
    static std::atomic<uint32_t> s_n{0};
    uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
    if (c < 6 || (c & (c - 1)) == 0) {
      uint32_t mgr = ctx.r3.u32;
      uint32_t rootClip = beRead32(base, 0x829A09E0u);          // the VM root/stage clip
      uint32_t mgrList  = (mgr >= 0x1000u && mgr < 0xC0000000u) ? beRead32(base, mgr) : 0;          // work/display-list head
      uint32_t mgrCnt   = (mgr >= 0x1000u && mgr < 0xC0000000u) ? beRead32(base, mgr + 0x1cu) : 0;  // count
      uint32_t rcDisp   = (rootClip >= 0x1000u && rootClip < 0xC0000000u) ? beRead32(base, rootClip + 0x20u) : 0; // root display list
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[APT_RENDER] executor sub_826BF630 #%u mgr=%08X mgr[0](list)=%08X mgr+0x1c(cnt)=%08X rootClip(09E0)=%08X rc+0x20(disp)=%08X",
        c, mgr, mgrList, mgrCnt, rootClip, rcDisp);
    }
  }
  __imp__sub_826BF630(ctx, base);
}

// iter-351: BOOT-FLOW DRIVER probe. sub_822B5910 (DAT_8296eda0) is ticked every
// frame from FeMasterTick. It STARTS the boot flow (Function_822B4BC8 -> Press
// Start -> frontend Apt movie) only when XNotifyGetNext yields XN_SYS_UI (id=9,
// param=0) while the flow object is ready (+0x100==0), setting +0x100 0->2. The
// stubs.cpp XNotify fix now delivers (9,0); this probe confirms it actually kicks
// the flow. Pure trace (calls straight through). Look for [BOOTFLOW] in the log.
extern "C" void __imp__sub_822B5910(PPCContext& __restrict ctx, uint8_t* base);
void sub_822B5910(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t obj = ctx.r3.u32;
  bool ok = obj >= 0x1000u && obj < 0xC0000000u;
  uint32_t before = ok ? beRead32(base, obj + 0x100u) : 0xFFFFFFFFu;
  __imp__sub_822B5910(ctx, base);
  uint32_t after = ok ? beRead32(base, obj + 0x100u) : 0xFFFFFFFFu;
  static std::atomic<uint32_t> s_n{0};
  uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
  if (c < 40 || before != after) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[BOOTFLOW] sub_822B5910 #%u obj=%08X +0x100 %08X->%08X +0xfc=%08X %s",
      c, obj, before, after, ok ? beRead32(base, obj + 0xfcu) : 0u,
      (before == 0 && after == 2) ? "*** BOOT FLOW STARTED ***" : "");
  }
}

// iter-351: FSM WALK trace — the true menu trunk is the BaseWorldFlow FSM parking
// (~state 40) before it reaches the state that issues the UI load + show-screen.
// worker sub_8278C500 (r4=fsm; fsm+8=curState, +0x18=count[44=BaseWorldFlow],
// +4=entering, +0x1c=state array). transition sub_8278C1C0(r3=fsm, r4=destIdx).
// Log the walk so the LAST curState before it goes silent NAMES the parked state;
// dump the parked state's transition predicates so the wait condition is nameable.
//
// iter-450V: [FSM_WALK] is now OPT-IN (SR_FSM_WALK_TRACE=1, default OFF). Both
// emitters below were UNGATED on hot guest paths. A single 186 s drive run
// produced 205,620 FSM_WALK lines - 3,400 per second during the front-end load
// - each costing up to a dozen big-endian guest reads, a 160-200 char snprintf,
// and a trip through the GLOBAL log spinlock, all on the guest's own thread.
// The FSM work itself is untouched either way; only the observation is gated,
// so no guest behaviour changes with the flag.
bool fsmWalkTraceOn() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_FSM_WALK_TRACE");
    return e && e[0] == '1';
  }();
  return s_on;
}

extern "C" void __imp__sub_8278C500(PPCContext& __restrict ctx, uint8_t* base);
void sub_8278C500(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t fsm = ctx.r4.u32;
  bool ok = fsm >= 0x1000u && fsm < 0xC0000000u;
  uint32_t cur = ok ? beRead32(base, fsm + 8u) : 0xFFFFFFFFu;
  static std::atomic<uint32_t> s_last{0xFFFFFFFEu};
  static std::atomic<uint32_t> s_n{0};
  uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
  uint32_t prev = s_last.load(std::memory_order_relaxed);
  if (ok && fsmWalkTraceOn() && (cur != prev || c < 3)) {
    s_last.store(cur, std::memory_order_relaxed);
    uint32_t cnt = beRead32(base, fsm + 0x18u);
    uint32_t arr = beRead32(base, fsm + 0x1cu);
    // dump the current state's transition predicates (state+0xC array, count state+8)
    uint32_t st = (arr >= 0x1000u && arr < 0xC0000000u && cur < 0x100u)
                    ? beRead32(base, arr + cur * 4u) : 0;
    uint32_t txnN = (st >= 0x1000u && st < 0xC0000000u) ? beRead32(base, st + 8u) : 0;
    uint32_t txnA = (st >= 0x1000u && st < 0xC0000000u) ? beRead32(base, st + 0xCu) : 0;
    char preds[160]; int off = 0; preds[0] = 0;
    for (uint32_t i = 0; i < txnN && i < 6 && txnA >= 0x1000u && txnA < 0xC0000000u; ++i) {
      uint32_t txn = beRead32(base, txnA + i * 4u);
      uint32_t pvt = (txn >= 0x1000u && txn < 0xC0000000u) ? beRead32(base, txn) : 0;
      uint32_t pfn = (pvt >= 0x1000u && pvt < 0xC0000000u) ? beRead32(base, pvt + 4u) : 0;
      uint32_t dst = (txn >= 0x1000u && txn < 0xC0000000u) ? beRead32(base, txn + 4u) : 0;
      off += std::snprintf(preds + off, sizeof(preds) - off, "[pred=%08X dst=%u]", pfn, dst);
      if (off >= (int)sizeof(preds) - 24) break;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[FSM_WALK] fsm=%08X curState=%d count=%d stateObj=%08X txnN=%d %s",
      fsm, (int)cur, (int)cnt, st, (int)txnN, preds);
  }
  __imp__sub_8278C500(ctx, base);
}

extern "C" void __imp__sub_8278C1C0(PPCContext& __restrict ctx, uint8_t* base);
void sub_8278C1C0(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t fsm = ctx.r3.u32, dest = ctx.r4.u32;
  bool ok = fsm >= 0x1000u && fsm < 0xC0000000u;
  uint32_t cur = ok ? beRead32(base, fsm + 8u) : 0xFFFFFFFFu;
  __imp__sub_8278C1C0(ctx, base);   // perform the transition (sets fsm+8=dest)
  if (!fsmWalkTraceOn()) return;
  // Dump the DESTINATION state's wait: its transition predicates (vtable+4 fn VA
  // and dest idx), its Lua script handle (+0x10) and content/sub-FSM obj (+0x14).
  // The state we PARK on = the last dest reached; its predicates are the ones that
  // never fire -> names the state-40 wait condition (expect an FMV/IGC-complete poll).
  uint32_t arr = ok ? beRead32(base, fsm + 0x1cu) : 0;
  uint32_t st  = (arr >= 0x1000u && arr < 0xC0000000u && dest < 0x100u)
                   ? beRead32(base, arr + dest * 4u) : 0;
  bool sok = st >= 0x1000u && st < 0xC0000000u;
  uint32_t txnN = sok ? beRead32(base, st + 8u) : 0;
  uint32_t txnA = sok ? beRead32(base, st + 0xCu) : 0;
  char preds[200]; int off = 0; preds[0] = 0;
  for (uint32_t i = 0; i < txnN && i < 8 && txnA >= 0x1000u && txnA < 0xC0000000u; ++i) {
    uint32_t txn  = beRead32(base, txnA + i * 4u);
    uint32_t pvt  = (txn >= 0x1000u && txn < 0xC0000000u) ? beRead32(base, txn) : 0;
    uint32_t pfn  = (pvt >= 0x1000u && pvt < 0xC0000000u) ? beRead32(base, pvt + 4u) : 0;
    uint32_t didx = (txn >= 0x1000u && txn < 0xC0000000u) ? beRead32(base, txn + 4u) : 0;
    int w = std::snprintf(preds + off, sizeof(preds) - off, "[p=%08X->%u]", pfn, didx);
    if (w <= 0 || off + w >= (int)sizeof(preds) - 1) break;
    off += w;
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[FSM_WALK] fsm=%08X cur=%d -> state=%d obj=%08X script(+10)=%08X content(+14)=%08X txnN=%d %s",
    fsm, (int)cur, (int)dest, st,
    sok ? beRead32(base, st + 0x10u) : 0, sok ? beRead32(base, st + 0x14u) : 0,
    (int)txnN, preds);
}

// iter-352: PARKED-STATE predicate probe (menu-analysis pick). sub_8278C448(r3=fsm)
// is the per-frame predicate-eval loop: fsm+8=curIdx, +0x18=count, +0x1c=stateArr;
// stateObj=stateArr[idx]; stateObj+8=txnCount, +0xc=txnArray; each txnArray[i] is a
// CONDITION object whose +0=vtable (the class), vtable+4=predicate fn, +4=dest idx.
// If every predicate returns 0 the FSM PARKS (the state-40 symptom). The sub_8278C1C0
// probe above only fires ON a transition — so it never shows state-40's OWN exits
// being polled+failing during the park. This hook runs every frame WHILE parked at 40
// and NAMES each condition's class (vtable) + predicate fn + first data word (the
// polled global/handle), proving the tick is alive and the park is a predicate-false
// wait (not a dead tick) — the exact event to root-fix naturally. Read-only, fault-
// free, gated SR_LOG_FSM40; NO writes -> charter-clean (diagnostic, not forcing).
// NOTE: this also re-examines iter-351's "static sim clock = root" (91d082a): the
// synthesizer flagged sub_82241430 as reading the PARKED descriptor (symptom, not a
// frozen clock) — naming the real predicate here disambiguates that.
extern "C" void __imp__sub_8278C448(PPCContext& __restrict ctx, uint8_t* base);
void sub_8278C448(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_LOG_FSM40");
    return e && e[0] == '1';
  }();
  if (s_on) {
    uint32_t fsm = ctx.r3.u32;
    bool ok = fsm >= 0x1000u && fsm < 0xC0000000u;
    uint32_t idx = ok ? beRead32(base, fsm + 8u) : 0xFFFFFFFFu;
    if (ok && idx == 40u) {                      // parked BaseWorldFlow attract wait
      static std::atomic<uint32_t> s_n{0};
      uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
      if (c < 4 || (c % 120u) == 0) {            // first few + ~once/4s at 30Hz
        uint32_t cnt  = beRead32(base, fsm + 0x18u);
        uint32_t arr  = beRead32(base, fsm + 0x1cu);
        uint32_t st   = (arr >= 0x1000u && arr < 0xC0000000u) ? beRead32(base, arr + idx * 4u) : 0;
        bool sok = st >= 0x1000u && st < 0xC0000000u;
        uint32_t txnN = sok ? beRead32(base, st + 8u) : 0;
        uint32_t txnA = sok ? beRead32(base, st + 0xCu) : 0;
        char preds[256]; int off = 0; preds[0] = 0;
        for (uint32_t i = 0; i < txnN && i < 8 && txnA >= 0x1000u && txnA < 0xC0000000u; ++i) {
          uint32_t cond = beRead32(base, txnA + i * 4u);                                            // condition object
          uint32_t cvt  = (cond >= 0x1000u && cond < 0xC0000000u) ? beRead32(base, cond) : 0;       // vtable = class
          uint32_t pfn  = (cvt  >= 0x1000u && cvt  < 0xC0000000u) ? beRead32(base, cvt + 4u) : 0;    // predicate fn
          uint32_t c8   = (cond >= 0x1000u && cond < 0xC0000000u) ? beRead32(base, cond + 8u) : 0;   // first data word (polled global?)
          uint32_t dst  = (cond >= 0x1000u && cond < 0xC0000000u) ? beRead32(base, cond + 4u) : 0;   // dest state idx
          int w = std::snprintf(preds + off, sizeof(preds) - off,
                                "[cls=%08X pfn=%08X +8=%08X ->%u]", cvt, pfn, c8, dst);
          if (w <= 0 || off + w >= (int)sizeof(preds) - 1) break;
          off += w;
        }
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[FSM40] #%u fsm=%08X state=40 stCount=%u stateObj=%08X txnN=%u %s",
          c, fsm, cnt, st, txnN, preds);
      }
    }
  }
  // iter-455m [WFFSM] pre-arm dispatch (director rework): the wrap engages
  // ONLY when ACTIVE (gate on AND armed by the exact main_hud request); the
  // enabled-but-unarmed hot path delegates directly - no read, no TLS, no
  // record (the always-on wrap was elevated as a perturbation candidate by
  // the six-miss/gate-off-control experiment, ledger iter-455m-control1).
  // One delegation on every path.
  sr::wffsm::walkerDispatch(
      ctx, base,
      [](PPCContext& c, uint8_t* b) { __imp__sub_8278C448(c, b); },
      sr::wffsm::enabled(),
      []() -> sr::wffsm::Core& { return sr::wffsm::core(); });
}

// iter-353: FE SPLASH-STATE TICK probe. Agent-2 traced the intro/logo reel to
// sub_822A2640 (the FE-splash-STATE update slot) -> sub_8214CE08(splashObj
// 0x8295fc8c) -> FUN_822b7f38 -> sub_8235F2D0 -> SM+4 0->1. Every DATA gate is
// OPEN (splash obj non-null, session flags 2/1, SM idle) yet [MOVIE_Q] fires 0x.
// Hypothesis (MEDIUM): sub_822A2640 is NEVER ticked because the FE screen/flow
// system never installs the splash state (the aptCount=0 trunk). Confirm: log if
// it ever fires + the SM state. Read-only observer -> charter-clean.
extern "C" void __imp__sub_822A2640(PPCContext& __restrict ctx, uint8_t* base);
void sub_822A2640(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<uint32_t> s_n{0};
  uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
  if (c < 8 || (c % 300u) == 0) {
    uint32_t sm = beRead32(base, 0x829761E0u);
    int smState = (sm >= 0x1000u && sm < 0xC0000000u) ? (int)beRead32(base, sm + 4u) : -1;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[SPLASH_TICK] sub_822A2640 #%u obj=%08X SM=%08X smState=%d lr=%08X",
      c, ctx.r3.u32, sm, smState, (uint32_t)ctx.lr);
  }
  __imp__sub_822A2640(ctx, base);
}

// iter-351: STATE-40 predicate = a TIMER wait on the WorldFlow/Actor clock.
// sub_8224E1D0(param): +0xc=threshold(float, ==DAT_820013b8 sentinel means "poll
// boolean-done"), +0x10=timerId, +0x14=mode, returns nonzero when elapsed>threshold.
// The FSM parks at state 40 forever -> either (a) this is never CALLED (FSM tick
// stopped), or (b) it's called but the WorldFlow clock never reaches threshold
// (world sim / attract-demo time not advancing). This probe answers which:
// call-count (is the tick alive?) + threshold + result over time.
extern "C" void __imp__sub_8224E1D0(PPCContext& __restrict ctx, uint8_t* base);
void sub_8224E1D0(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t p = ctx.r3.u32;
  bool ok = p >= 0x1000u && p < 0xC0000000u;
  uint32_t thrRaw = ok ? beRead32(base, p + 0xcu) : 0;
  float thr; std::memcpy(&thr, &thrRaw, sizeof(thr));
  __imp__sub_8224E1D0(ctx, base);
  // iter-351 DIAG (SR_DIAG_ADVANCE40, behind SR_ALLOW_FORCING): force the state-40
  // timer-done predicate TRUE to advance the parked FSM past state 40 — a force->
  // diagnose to reveal the DOWNSTREAM chain (show-screen sub_82280760 -> UI load
  // sub_826D2690 -> catalog -> aptCount -> menu) and expose the next gap. FORCING
  // (makes the guest's decision) -> opt-in only; the real fix is advancing the sim clock.
  static const bool s_adv40 = [] {
    if (!sr::kernel::forcingAllowed()) return false;
    const char* e = std::getenv("SR_DIAG_ADVANCE40");
    return e && e[0] == '1';
  }();
  if (s_adv40) ctx.r3.u64 = 1;
  uint32_t res = ctx.r3.u32;
  static std::atomic<uint32_t> s_n{0};
  static std::atomic<uint32_t> s_hits{0};
  uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
  if (res) s_hits.fetch_add(1, std::memory_order_relaxed);
  if (c < 6 || (c % 300u) == 0) {
    // Also read the FMV SM globals (subagent af5a5752): is a movie actually PLAYING
    // while state 40 is parked? SM=*0x829761E0, SM.state@+4 (2=playing), decoder
    // DAT_829761d4, EOS flag DAT_829761dc (must reach 1), last-hr DAT_829760bc,
    // file handle DAT_829761d8. This resolves FMV-EOS-gap vs animation-clock wait.
    uint32_t sm = beRead32(base, 0x829761E0u);
    int smState = (sm >= 0x1000u && sm < 0xC0000000u) ? (int)beRead32(base, sm + 4u) : -1;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[STATE40] #%u thr=%.3f timerId=%08X result=%u hits=%u | FMV sm=%08X state=%d dec(d4)=%08X eos(dc)=%08X hr(bc)=%08X hnd(d8)=%08X",
      c, (double)thr, ok ? beRead32(base, p + 0x10u) : 0, res,
      s_hits.load(std::memory_order_relaxed), sm, smState,
      beRead32(base, 0x829761D4u), beRead32(base, 0x829761DCu),
      beRead32(base, 0x829760BCu), beRead32(base, 0x829761D8u));
  }
}

// iter-351: SIM-CLOCK probe. sub_82241430(out, x) writes out[0] = current time read
// from the 'WFLW'/'ACTR' context (via FUN_82259498()->vt[6](tag), *(ctx+0x3c)+0x4c/58).
// State-40's timer predicate reads THIS clock. If it never advances -> the animated
// event 0xC84B4B23 can never reach 'done' -> the FSM parks. Log the time over the run:
// advancing => the event-start is the gap; static => the world-flow sim clock is the root.
extern "C" void __imp__sub_82241430(PPCContext& __restrict ctx, uint8_t* base);
void sub_82241430(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t out = ctx.r3.u32;
  __imp__sub_82241430(ctx, base);
  static std::atomic<uint32_t> s_n{0};
  uint32_t c = s_n.fetch_add(1, std::memory_order_relaxed);
  if (c < 4 || (c % 2000u) == 0) {
    uint32_t t = (out >= 0x1000u && out < 0xC0000000u) ? beRead32(base, out) : 0;
    float tf; std::memcpy(&tf, &t, sizeof(tf));
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[SIMCLK] sub_82241430 #%u time=%.4f raw=%08X flag=%02X",
      c, (double)tf, t, (out >= 0x1000u && out < 0xC0000000u) ? base[out + 8u] : 0u);
  }
}

#endif  // Oracle parsing diagnostics

#if !SR_ORACLE
// Lane E (step 5 E3-E6 r4; Astra r3 P1): the RELEASE build (SR_ORACLE=0) keeps exactly one thing of the movie parser's
// oracle wrapper above - the [ASSET] MAIN receipt (kernel/asset_receipt.hpp), compared before the guest parser runs
// (r5 = the raw .apt, r6 = the raw .const); then the guest parser, unchanged.
extern "C" void __imp__sub_826AE6D0(PPCContext& __restrict ctx, uint8_t* base);
void sub_826AE6D0(PPCContext& __restrict ctx, uint8_t* base) {
  sr::kernel::assetreceipt::onParseGuest(ctx.r5.u32, ctx.r6.u32);
  __imp__sub_826AE6D0(ctx, base);
}
#endif
