// AUTO-GENERATED. FM module CTOR walker.
//
// Every FM module has a CTOR wrapper referenced ONLY in its descriptor (the
// {0x8271F428, 0x8231C140, 0x82196960} shared-method triplet identifies a
// descriptor; the CTOR ptr lives at +12 from the triplet).
//
// On real Xbox 360 an engine-level walker invoked these in dependency order
// before main() entered the simulation loop. That walker is not present in
// the lifted code (the original mechanism likely lived in a section
// XenonRecomp did not classify as code, or used register-relative loads we
// did not detect).
//
// This file replays the equivalent. Each call is wrapped with BEGIN/END
// log lines so a hang shows up as a BEGIN with no matching END.
//
// iter-463 (burndown item 9): walkFmModuleCtors_impl itself is DELETED (it
// only logged and started the splash thread) and main's splice no longer
// calls it. iter-357d (2026-07-16): its dead hand-walk BODY
// was DELETED per the burndown tranche-1 contract (retired iter-302, commit
// d1b0792, behind sr::kernel::naturalModuleInitOn(); the contract's "delete
// dead code after 2 clean soaks" was long since satisfied). The 61-entry
// kFmCtorTable, kSkipList, the SR_ENABLE_RTAM/SR_ENABLE_SCRIPT un-skip gates,
// the embedded SR_DRIVE_MENULOAD consumer (0x8214BC88 special case), and the
// sub_82148F20 / sub_82566A50 bootstraps live in the git history of this
// file. The SR_NATURAL_MODULE_INIT=0 legacy A/B is no longer available: the
// only module-init path is the game's OWN E630 dependency walk
// (SR_NATURAL_MODULE_INIT era, docs/BANDAID_BURNDOWN.md).
// walkStaticCtors_impl below is KEEP (charter class: platform fix — the real
// dropped MSVC CRT _initterm fallback).

#include "../guest/ppc/ppc_recomp_shared.h"
#include "../app/log.hpp"
#include "guest_context.hpp"
#include "sr_oracle.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#endif

// Called from the recompiled main() between PSM bootstrap and the host
// fallback game loop. C linkage so the call site (inside extern "C" via
// PPC_FUNC_IMPL) can resolve it.
extern void startKernelSplashOverlayThread();

// === iter-289 ARCHITECTURAL ROOT FIX: the real MSVC C++ static-initializer walk ===
// XenonRecomp dropped the XEX entrypoint -> mainCRTStartup (sub_82468D10) edge, so the
// C/C++ static-initializer pass (Function_8246A160 C-init + Function_8246A080 C++ ctors,
// _initterm over the tables in .data) NEVER RAN. Those static ctors are the engine's REAL
// FM-module registrators: each prepends its module object (with its real vtable) onto the
// FM priority list DAT_829a2bb0 (via Function_8271F130), in TRUE LINKER ORDER -> one
// coherent FM list + one Apt ctx by construction. fm_module_walker.cpp's flat r3=0 walk +
// kSkipList is a hand-rolled, mis-ordered stand-in for this. This function walks the REAL
// tables via the dispatch, logging each ctor BEGIN/END so a hang names the culprit.
// Tables (BE pointer arrays): C-init [0x82910000,0x8291000C); C++ ctors [0x82910010,0x82910E70).
extern "C" void walkStaticCtors_impl(PPCContext& ctx, uint8_t* base) {
  auto rd = [&](uint32_t va) -> uint32_t {
    return (uint32_t(base[va]) << 24) | (uint32_t(base[va + 1]) << 16) |
           (uint32_t(base[va + 2]) << 8) | uint32_t(base[va + 3]);
  };
  uint32_t savedR3 = ctx.r3.u32, savedLR = ctx.lr;

  // iter-360k ROUTE A (root-fix the heap trunk): run the game's OWN heap init.
  // XenonRecomp dropped the entrypoint->mainCRTStartup (sub_82468D10) edge, so
  // Function_8246A380/Function_8246A2A8 (mainCRTStartup's heap-init prologue,
  // BEFORE _initterm) never runs -> DAT_829ac16c stays 0 -> the kernel faked a
  // stub mstate (guest_context.cpp, SR_HEAP_STUB). The _initterm ctors DO run
  // naturally (via _xstart) so this walk self-skips below, but the heap init is
  // a SEPARATE dropped piece. When SR_HEAP_STUB=0, run the REAL heap init here
  // (platform fix: run the guest's own code, no fake) so malloc gets a real
  // dlmalloc mstate (magic 0xeeffeeff). Function_8246A2A8: if DAT_829ac16c==0,
  // DAT_829ac16c = sub_8246C4D0(...) -> NtAllocateVirtualMemory arena.
  {
    static const bool s_heapStub = [] {
      const char* e = std::getenv("SR_HEAP_STUB");
      return !e || e[0] != '0';
    }();
    if (!s_heapStub && rd(0x829AC16Cu) == 0) {
      auto wr = [&](uint32_t va, uint32_t v) {
        base[va] = uint8_t(v >> 24); base[va + 1] = uint8_t(v >> 16);
        base[va + 2] = uint8_t(v >> 8); base[va + 3] = uint8_t(v);
      };
      // Function_8246A2A8 skips creating the heap because the XEX heap-size field
      // (RtlImageXexHeaderField 0x20401) reads 0 in our stubbed XEX header. Call
      // the guest's heap CREATOR sub_8246C4D0 directly with an explicit growable
      // size (it grows via NtAllocateVirtualMemory when the wilderness is spent).
      // Params: r3=2(flags), r4=0, r5=initSize, r6=0x1000(granularity), r7=0,
      // r8=paramsVA (a 0x30-byte struct with [0]=0x30, rest 0 -> defaults filled).
      const uint32_t kParamsVA = 0x82110000u;  // ex-stub region, unused when stub off
      for (uint32_t o = 0; o < 0x30; o += 4) wr(kParamsVA + o, 0);
      wr(kParamsVA, 0x30u);
      PPCFunc* mkHeap = PPC_LOOKUP_FUNC_SAFE(base, 0x8246C4D0u);
      if (mkHeap) {
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[HEAPINIT] creating real dlmalloc heap via sub_8246C4D0");
        PPCContext hc = ctx;
        hc.r3.u64 = 2; hc.r4.u64 = 0; hc.r5.u64 = 0x00400000u; hc.r6.u64 = 0x1000;
        hc.r7.u64 = 0; hc.r8.u64 = kParamsVA; hc.lr = 0x82149910u;
        mkHeap(hc, base);
        uint32_t mstate = hc.r3.u32;
        uint32_t magic = mstate ? rd(mstate + 0x10u) : 0;
        if (mstate && magic == 0xEEFFEEFFu) {
          // Publish the REAL mstate to all 4 heap-handle globals (what the stub
          // faked, but with a genuine dlmalloc mstate this time).
          wr(0x829AC16Cu, mstate); wr(0x82A2C16Cu, mstate);
          wr(0x8296C16Cu, mstate); wr(0x829BE4CCu, mstate);
        }
        SR_LOG_BOOT(magic == 0xEEFFEEFFu ? sr::LogSeverity::Warn : sr::LogSeverity::Error,
          "[HEAPINIT] sub_8246C4D0 -> mstate=%08X magic=%08X (want EEFFEEFF) -> published=%d",
          mstate, magic, (mstate && magic == 0xEEFFEEFFu));
      } else {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[HEAPINIT] sub_8246C4D0 NO-DISPATCH");
      }
    }
  }

  // iter-308 (workflow wf_9179213e angle A): since iter-300 the NATURAL
  // _initterm (sub_8246A080 via _xstart) runs the whole static-ctor table in
  // correct pre-Init1 order — this harness walk is then a REDUNDANT SECOND
  // RUN. The iter-291 head-clearing below protects the two FM lists from
  // double-prepend, but every OTHER statically-registered prepend list is
  // corrupted into a cycle by the re-run: proven on the pending-factory list
  // DAT_82979558 (registrars sub_828E1E90/828EAE88/828DE478 do node.next=head;
  // head=node — run twice, a node points at itself), whose self-loop was the
  // deterministic FMPhysics-era init wedge (call-less pointer chase in
  // PendingFactoryRetryLoop pass 1). Structural check: FM list head populated
  // = _initterm already ran → skip the whole walk (keep the splash-thread
  // side-effect). SR_CTORWALK_ALWAYS=1 forces the old double-run for A/B.
  {
#if SR_ORACLE
    static const bool s_force = [] {
      const char* e = std::getenv("SR_CTORWALK_ALWAYS");
      return e && e[0] == '1';
    }();
#else
    // SR_ORACLE=0 (kernel/sr_oracle.hpp): the double-run A/B is compiled out - unset, the walk runs only when the
    // natural _initterm left the FM list head empty.
    constexpr bool s_force = false;
#endif
    uint32_t fmHead = (uint32_t(base[0x829A2BB0u]) << 24) |
                      (uint32_t(base[0x829A2BB1u]) << 16) |
                      (uint32_t(base[0x829A2BB2u]) << 8) |
                      uint32_t(base[0x829A2BB3u]);
    if (!s_force && fmHead != 0) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[CTORWALK] SKIPPED — natural _initterm already ran (FM head=0x%08X); "
        "harness re-run would cycle prepend lists (0x82979558 class)", fmHead);
      startKernelSplashOverlayThread();
      return;
    }
  }
  // iter-463 charter (burndown item 9): the two FM list-head CLEARS that used
  // to sit here (DAT_829a2bb0 / DAT_829a2b98 := 0 before a re-walk) are
  // DELETED. They never ran: the natural _initterm fills the FM head first,
  // so the walk returns above ("[CTORWALK] SKIPPED" 80/81 + 69/69 run logs,
  // "cleared FM list heads" 0/150). This whole function is now reached only
  // under SR_ALLOW_FORCING (main's splice sub_821498F8); SR_CTORWALK_ALWAYS=1
  // re-runs every static ctor on top of the natural run - a forcing
  // diagnostic that WILL cycle the prepend lists (the iter-291 hazard).
  std::atomic<uint32_t> curVA{0};
  std::atomic<bool> done{false};
  std::thread wd([&] {
    int ticks = 0;
    while (!done.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      uint32_t v = curVA.load(std::memory_order_acquire);
      if (v && ++ticks % 4 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[CTORWALK_WATCHDOG] stalled %ds in static ctor @0x%08X", ticks / 2, v);
    }
  });
  struct Range { uint32_t lo, hi; const char* tag; };
  const Range ranges[] = { {0x82910000u, 0x8291000Cu, "C-init"}, {0x82910010u, 0x82910E70u, "C++ctor"} };
  unsigned ran = 0, idx = 0;
  for (const auto& r : ranges) {
    for (uint32_t va = r.lo; va < r.hi; va += 4, ++idx) {
      uint32_t fnVA = rd(va);
      if (fnVA == 0 || fnVA == 0xFFFFFFFFu) continue;
      PPCFunc* fn = PPC_LOOKUP_FUNC_SAFE(base, fnVA);
      if (!fn) {
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[CTORWALK] #%u %s slot@%08X fn=%08X NO-DISPATCH (skip)", idx, r.tag, va, fnVA);
        continue;
      }
      uint32_t fmBefore = rd(0x829A2BB0u);
      SR_LOG_BOOT(sr::LogSeverity::Info, "[CTORWALK] #%u %s BEGIN ctor=0x%08X", idx, r.tag, fnVA);
      curVA.store(fnVA, std::memory_order_release);
      ctx.r3.u64 = 0; ctx.r4.u64 = 0; ctx.r5.u64 = 0; ctx.lr = 0x82149910u;
      fn(ctx, base);
      curVA.store(0, std::memory_order_release);
      uint32_t fmAfter = rd(0x829A2BB0u);
      SR_LOG_BOOT(sr::LogSeverity::Info, "[CTORWALK] #%u END ctor=0x%08X FMlist %08X->%08X%s",
                  idx, fnVA, fmBefore, fmAfter, (fmAfter != fmBefore) ? " *** registered ***" : "");
      ++ran;
    }
  }
  done.store(true, std::memory_order_release);
  wd.join();
  ctx.r3.u32 = savedR3; ctx.lr = savedLR;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[CTORWALK] DONE ran=%u ctors; DAT_829a2bb0(FM list head)=0x%08X", ran, rd(0x829A2BB0u));
  // The kernel splash thread's only remaining effect is the master-gated
  // colour overlay (splash_hooks.cpp); iter-463: this function is master-only.
  startKernelSplashOverlayThread();
}

