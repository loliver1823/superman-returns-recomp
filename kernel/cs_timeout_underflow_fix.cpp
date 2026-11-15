#include "kernel/resource_reader_observe.hpp"
// iter-DOGMA-RE-17: fix the sub_825FC108 / sub_825F6B90 counter-underflow loop.
//
// The recompiled sub_825FC108 (CS Enter with bandaid 3s wall-clock TryEnter
// timeout) returns -2 when the timeout fires. Critically, the function
// returns -2 WITHOUT incrementing the refcount at param_1+0x20.
//
// Callers (e.g. Function_826E55F8 in FMUI's CTOR chain) unconditionally call
// sub_825F6B90 (Leave wrapper) after, which DECREMENTS the refcount and
// calls RtlLeaveCriticalSection. The decrement is an underflow because the
// matched increment never happened. Caller then loops, calling Enter again
// (returns -2 again), Leave again (decrements again), repeat.
//
// FMUI sees this as a 1M+/sec tight loop on RtlLeaveCriticalSection of a
// real heap CS handle (iter-DOGMA-RE-16 evidence: r3=0x894B62F4, r7 = -2
// lingering from sub_825FC108's recent return).
//
// Fix: strong-override sub_825FC108 to delegate to the original lifted body,
// then if it returned -2, increment the refcount at param_1+0x20 ourselves
// and override the return value to +1. This makes the matched Leave a clean
// decrement back to its starting value (no underflow) and lets the caller's
// loop exit when the refcount goes positive.
//
// Risk: when -2 fires, the CS isn't actually held. Code "after" the Enter
// call may race with another CS holder. In practice during init (FMUI CTOR)
// there are few competing threads (workers deferred OR completed), so the
// race window is small. If subsequent issues arise, the override can be
// scoped to specific caller LRs.

#include "guest/ppc/ppc_recomp_shared.h"
#include "kernel/ntavm_xenia_heap.hpp"
#include "guest_context.hpp"
#include "sr_oracle.hpp"        // C1: the development oracles below compile out under SR_ORACLE=0
#include "r1_hooks.hpp"         // iter-467: sr::r1::setjmpProtected (NewBackendCompile)
#include "p2_trace.hpp"         // iter-467 native-a: BlockUntilIdle whole (kIdleBegin / kIdleEnd)
#include "native_flush.hpp"     // P2-B: segTileDrained (SR_NATIVE_SEG_TILE's drained reconciliation)
#include "native_resource_writes.hpp"
#include "native_resource_stamps.hpp"
#include "cheats_menu.hpp"     // iter-466: CHEATS rows call the game's own cheat functions
#include "natural_gauges.hpp"  // iter-358: uncapped natural counters
#include "../app/log.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <functional>
#include <string_view>
#include <mutex>
#include <vector>
#include <algorithm>
#include "native_brackets.hpp"
#ifdef _WIN32
#include <windows.h>  // iter-464 [MOVIE_PACE]: NtQueryTimerResolution lookup
#include "r1_hooks.hpp"  // R1 (docs/SPLICE_CONVERSION.md): ports of the old splice bodies
#endif

#if SR_ORACLE
// iter-403: declared HERE (top of file) rather than beside one probe family:
// the later declaration sat below several probe sites that wanted it, so
// adding a tid to any earlier probe failed to compile.
extern "C" unsigned long __stdcall GetCurrentThreadId(void);

namespace sr { namespace kernel {
extern std::atomic<uint64_t> g_sweepThreadToken;
} }

extern "C" void __imp__sub_825FC108(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_825F6B90(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826F92E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826E55F8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8258C278(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826F4230(PPCContext& __restrict ctx, uint8_t* base);

// iter-DOGMA-RE-19: kill the runaway DOGMA worker before it spins.
//
// sub_826F92E0 is a DOGMA worker thread main loop (identified in iter-18).
// Its outer loop runs while *r26 == 0 (shutdown flag); inner loop drains a
// collection at r31 = r26 + 180 ({begin, end} pointers) by popping elements
// and dispatching their vtables. The collection's begin/end pointers are
// garbage (start of run, before any work was actually enqueued), so the
// inner loop iterates millions of times reading random memory and calling
// sub_825F6B90 with garbage CS pointers (r29 = r26+0x5C; r26 NULL-ish →
// r29 small junk).
//
// Fix: BEFORE calling __imp__:
//   1. Sanitize the collection: set end = begin (empty), so inner loop
//      exits immediately if entered.
//   2. Set the shutdown flag *r26 = 1, so the outer loop's first check
//      (`bne cr6, loc_826F9440` at line 364) takes the early exit path.
// Then call __imp__sub_826F92E0 which immediately returns.
//
// Worker terminates cleanly. FMUI's CTOR (which spawned this worker via
// vtable dispatch) proceeds.
//
// Risk: if FMUI's tick later expects this worker to be alive and process
// real work, killing it may cascade to a different hang. We have prior
// art for "kill workers and accept the consequence" (iter-30 deferral
// experiment) — different scope but same risk class. We'll iterate if so.
// iter-191: healthy queue-buffer pointer captured at worker entry; used by the
// sub_826F9CD0 compact guard below to reset a scribbled queue to a sane state.
static std::atomic<uint32_t> s_fmuiQueueHealthyBegin{0};

void sub_826F92E0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t workerVA = ctx.r3.u32;
  static std::atomic<unsigned> s_callCount{0};
  unsigned n = s_callCount.fetch_add(1, std::memory_order_relaxed) + 1;

  // Sanitize the work-queue collection at workerVA + 180 ({begin, end}).
  // Read both pointers, log if they look bad, then force end = begin (empty).
  uint32_t collBegin = sr::kernel::readGuestU32BE(workerVA + 180 + 0);
  uint32_t collEnd   = sr::kernel::readGuestU32BE(workerVA + 180 + 4);

  // iter-389 RETIREMENT (registry band-aid #2): the sanitize predates the
  // real heap (iter-380) and iter-388's arena fix; the garbage begin/end it
  // guarded against came from the pre-real-heap era. Default OFF; alarm if
  // the queue ever looks non-empty at worker entry (the condition the
  // band-aid silently swallowed). SR_DOGMA_QUEUE_SANITIZE=1 restores.
  static const bool s_sanitizeOn = [] {
    const char* e = std::getenv("SR_DOGMA_QUEUE_SANITIZE");
    return e && e[0] == '1';
  }();
  if (collEnd != collBegin) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[DOGMA_WORKER_QUEUE_NONEMPTY] #%u worker=0x%08X {0x%08X..0x%08X} "
      "(count=%u) at entry — retired sanitize would have emptied it%s",
      n, workerVA, collBegin, collEnd,
      collEnd > collBegin ? (collEnd - collBegin) / 4 : 0,
      s_sanitizeOn ? " (SANITIZING, gate on)" : " (left intact)");
  }
  if (s_sanitizeOn) {
    if (uint8_t* host = sr::kernel::guestVAToHost(workerVA + 180 + 4)) {
      host[0] = (collBegin >> 24) & 0xFF;
      host[1] = (collBegin >> 16) & 0xFF;
      host[2] = (collBegin >>  8) & 0xFF;
      host[3] =  collBegin        & 0xFF;
    }
  }

  // FIX iter-65: do NOT set shutdown flag. The outer loop has a proper condvar
  // wait when the queue is empty; killing the worker prevents it from ever
  // processing real work items added by FM module init. Let it block instead.

  if (n <= 4) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[DOGMA_WORKER_SANITIZE] sub_826F92E0 #%u worker=0x%08X collection={0x%08X..0x%08X} "
      "(count=%u) — sanitized end=begin, worker kept alive",
      n, workerVA, collBegin, collEnd,
      collEnd > collBegin ? (collEnd - collBegin) / 4 : 0);
  }

  // iter-191: publish the queue-field address so the PPC_STORE macros watch it.
  // 191a: fields were valid at entry, then overwritten with .data addresses
  // (0x8296D8D5/0x829A23F1) within ~3 s → the pop-compact swept 19 KB of .data.
  sr::kernel::g_fmuiPoolQueueVA.store(workerVA + 180, std::memory_order_relaxed);
  sr::kernel::g_storeWatchOn = true;  // iter-461: the PPC_STORE checks run only when armed
  // Remember the healthy buffer pointer for the compact guard's queue reset.
  if (collBegin != 0 && (collBegin & 3u) == 0 &&
      !(collBegin >= 0x82000000u && collBegin < 0x82B00000u)) {
    s_fmuiQueueHealthyBegin.store(collBegin, std::memory_order_relaxed);
  }

  __imp__sub_826F92E0(ctx, base);
}

// iter-192: AS-invoke logger. sub_826B8DC0 is the EA Apt "invoke named
// ActionScript handler" worker (chain: dispatcher vt[7] → client sub_826D0CF0
// → context wrapper sub_826BB918 → sub_826B9048 (vararg marshal) → here).
// Log the first calls' raw args to learn which handler names the engine
// requests once the dispatcher pump runs with the real client.
extern "C" void __imp__sub_826B8DC0(PPCContext& __restrict ctx, uint8_t* base);

void sub_826B8DC0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 40 || (n & (n - 1)) == 0) {
    char s4[40] = {0}, s5[40] = {0};
    auto peek = [&](uint32_t va, char* out) {
      uint8_t* h = sr::kernel::guestVAToHost(va);
      if (!h) return;
      for (int i = 0; i < 39; ++i) {
        char c = (char)h[i];
        if (c == 0) break;
        out[i] = (c >= 32 && c < 127) ? c : '.';
      }
    };
    peek(ctx.r4.u32, s4);
    peek(ctx.r5.u32, s5);
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[AS_INVOKE] #%u r3=0x%08X r4=0x%08X(\"%s\") r5=0x%08X(\"%s\") r6=0x%08X r7=0x%08X lr=0x%08X",
      n, ctx.r3.u32, ctx.r4.u32, s4, ctx.r5.u32, s5, ctx.r6.u32, ctx.r7.u32,
      (uint32_t)ctx.lr);
  }
  __imp__sub_826B8DC0(ctx, base);
}

// iter-193: package-mount pump logger. sub_8236EBD0(streamer, &outState) is the
// inline pump-one-step of the package/mount state machine (caller sub_8236F0C0
// loops it until state 0xD). 193a: the movie-load drive blocked forever with no
// file opens — log pump invocations + state to see whether the pump runs at all
// during the block and where its state machine parks.
extern "C" void __imp__sub_8236EBD0(PPCContext& __restrict ctx, uint8_t* base);
extern std::atomic<int> g_fxLexArm;  // defined below (iter-364 effect-compile token trace)
extern std::atomic<uint32_t> g_fxRegionAddr;  // defined below (iter-364 live +104 region)

void sub_8236EBD0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t self = ctx.r3.u32;
  uint32_t outP = ctx.r4.u32;
  // iter-364 STEP3b: sub_8236EBD0 is ALSO the effect-compiler PREPROCESSOR token pump (this
  // iter-193 label was a misnomer). During the armed fallback compile it returns EOF (token
  // 13) on the 6th call WITHOUT lexing though 772 bytes remain. Capture entry state to pin
  // the branch: +72=stop flag, +104=region obj(+8=counter), +100=pushback, +64=at-line-start.
  bool fxOn = g_fxLexArm.load(std::memory_order_relaxed) != 0;
  auto R = [&](uint32_t va) -> uint32_t { return va >= 0x1000u ? sr::kernel::readGuestU32BE(va) : 0; };
  uint32_t f72 = 0, f100 = 0, f104 = 0, f64 = 0, f80 = 0, r104_4 = 0, r104_8 = 0;
  if (fxOn) {
    f72 = R(self + 72); f100 = R(self + 100); f104 = R(self + 104);
    f64 = R(self + 64); f80 = R(self + 80);
    r104_4 = f104 ? R(f104 + 4) : 0; r104_8 = f104 ? R(f104 + 8) : 0;
    if (f104) g_fxRegionAddr.store(f104, std::memory_order_relaxed);  // publish live region for the alloc trace
  }
  __imp__sub_8236EBD0(ctx, base);
  if (fxOn) {
    static std::atomic<unsigned> s_fx{0};
    unsigned fc = s_fx.fetch_add(1, std::memory_order_relaxed) + 1;
    uint32_t f104x = R(self + 104);
    uint32_t r104_8x = f104x ? R(f104x + 8) : 0;  // counter AFTER the call (bracket the 1->0 drop)
    if (fc <= 12)
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FXPP] #%u a1=%08X +72=%08X +100=%08X +104=%08X(+4=%08X +8:%08X->%08X) +64=%08X +80=%08X -> tok=%d rc=%08X",
        fc, self, f72, f100, f104, r104_4, r104_8, r104_8x, f64, f80,
        (int)(outP ? R(outP) : 0xFFFFFFFFu), ctx.r3.u32);
  }
  if (n <= 60 || (n & (n - 1)) == 0) {
    uint32_t state = outP ? sr::kernel::readGuestU32BE(outP) : 0xFFFFFFFF;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MOUNT_PUMP] #%u self=0x%08X ret=0x%08X state=0x%X curPkgState=0x%X",
      n, self, ctx.r3.u32, state,
      self ? sr::kernel::readGuestU32BE(self + 0x298u) : 0);
  }
}

// iter-193: deque-iterator copy guard (scribbler #2 contained at the source).
// sub_821A65D0(?, srcBeginIt, srcEndIt, dstIt) copies *src → *dst advancing
// deque iterators ({curPtr, …, pageEnd@+8, pageMap@+12}). Run on junk deques
// (DOGMA double-grant containers) its dst walks .data: 193c/d caught it
// zeroing EB80 and -4-shifting the Apt singleton/env pair (env value →
// singleton slot, garbage 2 → env) — which fed the flow factory a fake
// singleton. Block the copy when the dst cursor is null/misaligned/in-image.
extern "C" void __imp__sub_821A65D0(PPCContext& __restrict ctx, uint8_t* base);

void sub_821A65D0(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t dstIt = ctx.r6.u32;
  uint32_t dstPtr = dstIt ? sr::kernel::readGuestU32BE(dstIt) : 0;
  uint32_t srcIt = ctx.r4.u32;
  uint32_t srcPtr = srcIt ? sr::kernel::readGuestU32BE(srcIt) : 0;
  const bool dstBad = dstPtr == 0 || (dstPtr & 3u) != 0 ||
                      (dstPtr >= 0x82000000u && dstPtr < 0x82B00000u);
  const bool srcBad = srcPtr == 0 || (srcPtr & 3u) != 0;
  // iter-463 charter (burndown U1): master-gated ("[DEQUE_COPY_GUARD]" 0/150
  // run logs). Default exe: log the shape, run the guest's own copy.
  if (dstBad || srcBad) {
    static std::atomic<unsigned> s_blk{0};
    unsigned b = s_blk.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool block = sr::kernel::forcingAllowed();
    if (b <= 20 || (b & (b - 1)) == 0) {
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[DEQUE_COPY_GUARD] %s sub_821A65D0 srcIt=0x%08X(*=0x%08X) dstIt=0x%08X(*=0x%08X) lr=0x%08X (hit #%u)",
        block ? "BLOCKED" : "OBSERVED (not blocked: SR_ALLOW_FORCING off)",
        srcIt, srcPtr, dstIt, dstPtr, (uint32_t)ctx.lr, b);
    }
    if (block) {
      ctx.r3.u32 = dstIt;  // return the dst iterator unchanged
      return;
    }
  }
  __imp__sub_821A65D0(ctx, base);
}

// iter-193h: CRT memmove guard. sub_8279F130 is the generic (overlap-safe)
// memmove; the iter-189 "~300KB -4 shift over .data" runs through it and is
// STILL alive (193h: wrote 2 over g3/EB80, recomp.111.cpp:29536, called with
// no guest context). Legit guest memmoves into the XEX image are small struct
// writes — block only the corruption class: len >= 4KB with dst inside the
// image. Returns dst (the memmove contract) without copying.
extern "C" void __imp__sub_8279F130(PPCContext& __restrict ctx, uint8_t* base);

void sub_8279F130(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32;
  const uint32_t src = ctx.r4.u32;
  const uint32_t len = ctx.r5.u32;
  // iter-193j: the EB80 kill was a SMALL move (slipped the 4KB threshold) with
  // host-thread context (r1=0 in the watchpoint = our own driven calls reach
  // it). Any memmove targeting the hot .data globals band is corruption.
  if ((len >= 0x1000u && dst >= 0x82000000u && dst < 0x82B00000u) ||
      (len >= 4u && dst < 0x829B0000u && dst + len > 0x82960000u)) {
    static std::atomic<unsigned> s_blk{0};
    unsigned b = s_blk.fetch_add(1, std::memory_order_relaxed) + 1;
    if (b <= 20 || (b & (b - 1)) == 0) {
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[MEMMOVE_GUARD] BLOCKED sub_8279F130 dst=0x%08X src=0x%08X len=0x%X lr=0x%08X (hit #%u)",
        dst, src, len, (uint32_t)ctx.lr, b);
    }
    return;  // r3 already holds dst
  }
  __imp__sub_8279F130(ctx, base);
}

// iter-194: Apt loadMovie logger. Chain: client vt[+20](client,name) →
// vt[+24](client,name,"_level0"@0x82088490) → env wrapper sub_826BB6F0 →
// sub_826B8A90 = Apt loadMovie(name, level). 194b: ran for "main", returned,
// but no blob reads / no screens — log args+result to see the failure.
extern "C" void __imp__sub_826B8A90(PPCContext& __restrict ctx, uint8_t* base);

void sub_826B8A90(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32;
  char s3[40] = {0}, s4[40] = {0};
  auto peek = [&](uint32_t va, char* out) {
    uint8_t* h = sr::kernel::guestVAToHost(va);
    if (!h) return;
    for (int i = 0; i < 39; ++i) {
      char c = (char)h[i];
      if (c == 0) break;
      out[i] = (c >= 32 && c < 127) ? c : '.';
    }
  };
  peek(a3, s3); peek(a4, s4);
  __imp__sub_826B8A90(ctx, base);
  if (n <= 30 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[APT_LOADMOVIE] #%u r3=0x%08X(\"%s\") r4=0x%08X(\"%s\") r5=0x%08X -> ret=0x%08X lr=0x%08X",
      n, a3, s3, a4, s4, a5, ctx.r3.u32, (uint32_t)ctx.lr);
  }
}

// iter-197: level-node creation logger. sub_826B3998(envBlock, mgr, ?, level,
// 1, 1, 0) finds/creates the level entry loadMovie targets. Log args+result
// and publish the node VA so the fabricator can dump its state over time
// (the pending-load state machine lives in this node).
extern "C" void __imp__sub_826B3998(PPCContext& __restrict ctx, uint8_t* base);

void sub_826B3998(PPCContext& __restrict ctx, uint8_t* base) {
  // iter-198: null-ctx backstop. With r4 (resolution context) == 0 a plain
  // level/movie name null-resolves BY DESIGN and the caller builds the dead
  // placeholder node (vtable 0x82086F78). Our host-driven loadMovie paths
  // lose the context; substitute the manager root ([0x829A0020]).
  if (ctx.r4.u32 == 0) {
    uint32_t mgr = sr::kernel::readGuestU32BE(0x829A0020u);
    if (mgr >= 0x10000u) {
      ctx.r4.u32 = mgr;
      static std::atomic<unsigned> s_h{0};
      unsigned h = s_h.fetch_add(1, std::memory_order_relaxed) + 1;
      if (h <= 5 || (h & (h - 1)) == 0) {
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[APT_LEVEL] #%u null ctx — substituted manager root 0x%08X (lr=0x%08X)",
          h, mgr, (uint32_t)ctx.lr);
      }
    }
  }
  uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a6 = ctx.r6.u32;
  uint32_t alr = (uint32_t)ctx.lr;
  // iter-297j: the ChangeInterfaceState wrapper's two resolve sites — always
  // log, with the NAME STRING (holder -> strNode -> chars@+8). This is the
  // exact call whose failure keeps FlowMgr "unresolved".
  const bool fromInvokeByPath = (alr == 0x826B8E20u || alr == 0x826B8F4Cu);
  char pnm[32] = {};
  if (fromInvokeByPath) {
    uint32_t strNode = (a6 >= 0x10000u && a6 < 0xC0000000u)
        ? sr::kernel::readGuestU32BE(a6) : 0;
    if (strNode >= 0x10000u && strNode < 0xC0000000u) {
      uint8_t* h = sr::kernel::guestVAToHost(strNode + 8u);
      if (h) { for (int i = 0; i < 30 && h[i] >= 0x20 && h[i] < 0x7F; ++i) pnm[i] = (char)h[i]; }
    }
  }
  __imp__sub_826B3998(ctx, base);
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (ctx.r3.u32 >= 0x10000u) {
    sr::kernel::g_aptLevelNodeVA.store(ctx.r3.u32, std::memory_order_relaxed);
  }
  if (fromInvokeByPath) {
    uint32_t res = ctx.r3.u32;
    uint32_t resVt = (res >= 0x10000u && res < 0xC0000000u)
        ? sr::kernel::readGuestU32BE(res) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[PATH_RESOLVE] wrapper site lr=%08X root=%08X name='%s' -> %08X (vt=%08X, "
      "undefined-sentinel=%08X)",
      alr, a4, pnm, res, resVt,
      sr::kernel::readGuestU32BE(0x829A0AB8u));
  } else if (n <= 20 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[APT_LEVEL] #%u sub_826B3998(env=0x%08X mgr=0x%08X lvl=0x%08X) -> node=0x%08X [+4]=0x%08X lr=0x%08X",
      n, a3, a4, a6, ctx.r3.u32,
      ctx.r3.u32 >= 0x10000u ? sr::kernel::readGuestU32BE(ctx.r3.u32 + 4) : 0, alr);
  }
}

// iter-198: AS builtin loadMovie (entry in the AS-native dispatch table at
// 0x820DB8C8). r3 = invoking movieclip node ("this"), r4 = name object.
// With this==0 the resolver (sub_826B33C8) null-returns a plain name BY
// DESIGN and the level create falls to the empty-placeholder node (vtable
// 0x82086F78) — the iter-198 root cause of the dead frontend. Our host-driven
// loadMovie has no current movieclip, so substitute the manager root node
// ([0x829A0020], the _level0 system node) as the context.
extern "C" void __imp__sub_826CB500(PPCContext& __restrict ctx, uint8_t* base);

void sub_826CB500(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t self = ctx.r3.u32, nameObj = ctx.r4.u32, alr = (uint32_t)ctx.lr;
  if (self == 0) {
    uint32_t mgr = sr::kernel::readGuestU32BE(0x829A0020u);
    if (mgr >= 0x10000u) {
      ctx.r3.u32 = mgr;
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[LOADMOVIE] #%u null this — substituting manager root 0x%08X", n, mgr);
    }
  }
  char nm[40] = {0};
  uint32_t strPtr = nameObj >= 0x10000u ? sr::kernel::readGuestU32BE(nameObj) : 0;
  if (strPtr >= 0x10000u) {
    uint8_t* h = sr::kernel::guestVAToHost(strPtr + 8);
    if (h) {
      for (int i = 0; i < 39; ++i) {
        char c = (char)h[i];
        if (c == 0) break;
        nm[i] = (c >= 32 && c < 127) ? c : '.';
      }
    }
  }
  __imp__sub_826CB500(ctx, base);
  if (n <= 30 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[LOADMOVIE] #%u this=0x%08X name=\"%s\" -> ret=0x%08X lr=0x%08X",
      n, self, nm, ctx.r3.u32, alr);
  }
}

// iter-198: u32-vector insert (sub_822B9910(vec, pos, valPtr)). Caught with a
// garbage end pointer ([vec+4]=3) inserting into the AST package registry —
// the wild inner memmove was blocked by MEMMOVE_GUARD but the insert still
// corrupts state, so registrations never stick and every Apt lookup re-mounts
// packages with the CS held (slow ticks + retry hangs). Log + reject garbage.
extern "C" void __imp__sub_822B9910(PPCContext& __restrict ctx, uint8_t* base);

void sub_822B9910(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t vec = ctx.r3.u32, pos = ctx.r4.u32, val = ctx.r5.u32;
  uint32_t alr = (uint32_t)ctx.lr;
  uint32_t beg = vec >= 0x10000u ? sr::kernel::readGuestU32BE(vec) : 0;
  uint32_t end = vec >= 0x10000u ? sr::kernel::readGuestU32BE(vec + 4) : 0;
  uint32_t cap = vec >= 0x10000u ? sr::kernel::readGuestU32BE(vec + 8) : 0;
  bool sane = vec >= 0x10000u && vec < 0xC0000000u &&
              (end == 0 || (end >= 0x10000u && end < 0xC0000000u)) &&
              (beg == 0 || (beg >= 0x10000u && beg < 0xC0000000u));
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  // iter-463 charter (burndown U1): master-gated ("[VECINS_GUARD]" 0/150).
  if (!sane) {
    static std::atomic<unsigned> s_r{0};
    unsigned r = s_r.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool reject = sr::kernel::forcingAllowed();
    if (r <= 10 || (r & (r - 1)) == 0) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[VECINS_GUARD] %s #%u vec=0x%08X beg=0x%08X end=0x%08X cap=0x%08X pos=0x%08X lr=0x%08X",
        reject ? "reject" : "OBSERVED (not rejected: SR_ALLOW_FORCING off)",
        r, vec, beg, end, cap, pos, alr);
    }
    if (reject) {
      ctx.r3.u32 = pos;
      return;
    }
    __imp__sub_822B9910(ctx, base);
    return;
  }
  if (n <= 12 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[VECINS] #%u vec=0x%08X beg=0x%08X end=0x%08X cap=0x%08X pos=0x%08X val=0x%08X lr=0x%08X",
      n, vec, beg, end, cap, pos, val, alr);
  }
  __imp__sub_822B9910(ctx, base);
}

// iter-198: stride-8 array element-remove (sub_82357738(list, elem): shift
// 8-byte entries down over elem, [list+4] -= 8, return elem). Caught writing
// through a garbage end pointer (RENDER_OBJ hits at 0x66020100) right after
// the AST package walk — one of the .data scribblers that kill the walk.
// Reject implausible list headers; mimic the return without touching memory.
extern "C" void __imp__sub_82357738(PPCContext& __restrict ctx, uint8_t* base);

void sub_82357738(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t lst = ctx.r3.u32, elem = ctx.r4.u32;
  bool sane = lst >= 0x10000u && lst < 0xC0000000u &&
              elem >= 0x10000u && elem < 0xC0000000u;
  uint32_t end = sane ? sr::kernel::readGuestU32BE(lst + 4) : 0;
  // iter-463 charter (burndown U1): master-gated ("[STRIDE8_GUARD]" 0/150).
  if (!sane || end < 0x10000u || end >= 0xC0000000u ||
      end < elem || (end - elem) > 0x100000u) {
    static std::atomic<unsigned> s_r{0};
    unsigned r = s_r.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool reject = sr::kernel::forcingAllowed();
    if (r <= 5 || (r & (r - 1)) == 0) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[STRIDE8_GUARD] %s #%u list=0x%08X elem=0x%08X end=0x%08X lr=0x%08X",
        reject ? "reject" : "OBSERVED (not rejected: SR_ALLOW_FORCING off)",
        r, lst, elem, end, (uint32_t)ctx.lr);
    }
    if (reject) {
      ctx.r3.u32 = elem;
      return;
    }
  }
  __imp__sub_82357738(ctx, base);
}

// iter-198: FM device open (vtable slot 3 of the file-device class at
// 0x82074C10). The lazy AST package walk opens each package through here;
// the caller LR identifies the mount-job function and its stop condition.
extern "C" void __imp__sub_82601FD0(PPCContext& __restrict ctx, uint8_t* base);

void sub_82601FD0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, alr = (uint32_t)ctx.lr;
  char p[64] = {0};
  if (a4 >= 0x10000u) {
    uint8_t* h = sr::kernel::guestVAToHost(a4);
    if (h) {
      for (int i = 0; i < 63; ++i) {
        char c = (char)h[i];
        if (c == 0) break;
        p[i] = (c >= 32 && c < 127) ? c : '.';
      }
    }
  }
  __imp__sub_82601FD0(ctx, base);
  if (n <= 80 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[FM_OPEN] #%u this=0x%08X path=\"%s\" flags=0x%08X -> 0x%08X lr=0x%08X",
      n, a3, p, a5, ctx.r3.u32, alr);
  }
}

// iter-211: BigFileAsset::Load (the asset content-fetch). Does it EVER run,
// and for which assets? If it loads fonts/TEXT but never the UI movie, the
// REQUEST is missing; if it never runs, the async load is dead. r3 = the
// BigFileAsset object; peek a name-ish field + the caller lr.
extern "C" void __imp__sub_826E83D0(PPCContext& __restrict ctx, uint8_t* base);

void sub_826E83D0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t a3 = ctx.r3.u32, alr = (uint32_t)ctx.lr;
  // iter-224: the asset NAME is an INLINE C string at this+8 (sub_826E83D0 builds
  // "<name>.AST" from param_1+8). Prior pointer-chase logged name="". Read it inline
  // to see WHICH assets our host-driven load actually loads (is the frontend "main"
  // movie among them?). Also peek vtable for the asset class.
  char nm[64] = {0};
  if (a3 >= 0x10000u) {
    uint8_t* h = sr::kernel::guestVAToHost(a3 + 8);
    if (h) { for (int i = 0; i < 63; ++i) { char c = (char)h[i]; if (c == 0) break; if (c < 32 || c >= 127) { nm[i] = '?'; } else nm[i] = c; } }
  }
  if (n <= 80 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[ASSET_LOAD] #%u BigFileAsset::Load this=0x%08X name=\"%s\" vt=0x%08X lr=0x%08X",
      n, a3, nm, sr::kernel::readGuestU32BE(a3), alr);
  }
  // iter-229: force the LOOSE-FILE load path for screen movies (Scr*). The default
  // param_3!=0 path (sub_826E83D0) does a resMgr/package LOOKUP that resolves the name
  // in the ui dir but never FETCHES the content bytes (iter-228: ScrPressStart loaded
  // as a stub, 0 NtCreateFile, no .apt resident, movieDef=NULL). param_3==0 takes the
  // loose path (LAB_826e84a0: build "<name>.AST", open+read+parse) -- the path that
  // makes loose movies (BossHealth) actually parse. ScrPressStart.AST exists (445KB).
  // Gated by SR_FORCE_LOOSE; targets Scr* (screen) names only so package loads are intact.
  static const bool s_forceLoose = [] {
    const char* e = std::getenv("SR_FORCE_LOOSE");
    return e && e[0] == '1';
  }();
  if (s_forceLoose && ctx.r5.u32 != 0 && nm[0] == 'S' && nm[1] == 'c' && nm[2] == 'r') {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[FORCE_LOOSE] %s: param_3 0x%X -> 0 (forcing loose-file .AST load)", nm, ctx.r5.u32);
    ctx.r5.u64 = 0;
  }
  __imp__sub_826E83D0(ctx, base);
}

// iter-220: DIAGNOSE + UNBLOCK the eager texture-upload runaway (sub_827B9060)
// that our host-driven menu-load (SR_DRIVE_MENULOAD, fm_module_walker.cpp) triggers.
// sub_8261C5B0 = the texture-upload dispatcher; its tiled branch
// (FUN_8260a4e8(param_1) & 0x100) runs away (sub_8279EC60 x54M, garbage dims) on a
// malformed descriptor. The working Xenia boot does ZERO texture upload at boot
// (sub_8261C5B0 = 0 in 348M records). Under SR_DRIVE_MENULOAD only, log the texture
// descriptor (the GPU fetch-constant fields FUN_8260a4e8 reads) and SKIP the upload
// (return success) so the loader proceeds — letting us see whether the .apt movie
// def then gets created (clip_ctor sub_826B0458 / DEF_EXEC sub_826B2298). Default
// build calls sub_8261C5B0 zero times, so the gate makes this a no-op for normal boot.
extern "C" void __imp__sub_8261C5B0(PPCContext& __restrict ctx, uint8_t* base);
void sub_8261C5B0(PPCContext& __restrict ctx, uint8_t* base) {
  // iter-357b CHARTER FIX: the fake-success skip above was written when the
  // iter-220 comment's claim held ("default build calls sub_8261C5B0 zero
  // times"). Since full natural module init (iter-320), the NATURAL boot calls
  // this 6x/run (FMGameRtam's own package kick, lr=0x822F9AC0) — so the skip
  // had become ungated live forcing swallowing real texture uploads on every
  // "natural" run. The skip now requires the master gate (SR_ALLOW_FORCING=1
  // AND SR_DRIVE_MENULOAD as the named diagnostic); the default exe logs and
  // runs the game's real upload path.
  static const bool s_skip = [] {
    const char* e = std::getenv("SR_DRIVE_MENULOAD");
    return sr::kernel::forcingAllowed() && (!e || e[0] != '0');
  }();
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 32) {
    uint32_t a3 = ctx.r3.u32;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[TEXUP] #%u sub_8261C5B0 this=0x%08X +1c=%08X +20=%08X +24=%08X +28=%08X +30=%08X +34=%08X lr=0x%08X %s",
      n, a3,
      sr::kernel::readGuestU32BE(a3 + 0x1c), sr::kernel::readGuestU32BE(a3 + 0x20),
      sr::kernel::readGuestU32BE(a3 + 0x24), sr::kernel::readGuestU32BE(a3 + 0x28),
      sr::kernel::readGuestU32BE(a3 + 0x30), sr::kernel::readGuestU32BE(a3 + 0x34),
      (uint32_t)ctx.lr, s_skip ? "SKIPPED (forced diagnostic)" : "EXEC (real upload)");
  }
  if (s_skip) {
    ctx.r3.u64 = 1;  // mimic the success return (sub_8279E460(1)); skip the real upload
    return;
  }
  __imp__sub_8261C5B0(ctx, base);
}

// iter-357d PROBE (observability only): name the texture-tiler wedge. Logs (a) the
// FMPlatformInit retail flags (app+0x1a/+0x1b select the retail-vs-dev package table
// in sub_8218D128 — golden has 1,1) and (b) the tiler's real arguments/bounds, so one
// run distinguishes: dev-table divergence vs garbage bounds vs slow-but-finite copy.
extern "C" void __imp__sub_827B9060(PPCContext& __restrict ctx, uint8_t* base);
void sub_827B9060(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 12 || (n % 1000 == 0)) {
    uint32_t app = sr::kernel::readGuestU32BE(0x8296d878);
    uint32_t flags = app ? sr::kernel::readGuestU32BE(app + 0x18) : 0xDEADDEAD;
    uint32_t p7 = ctx.r9.u32;  // param_7: uint* {x0, y0, x1, y1}
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[TILER] #%u r3=%08X r4(w)=%u r5(h)=%u r6=%08X r7=%08X r8=%08X r9=%08X r10(bpp)=%u"
      " | p7={%u,%u,%u,%u} | app=%08X flags18=%08X lr=%08X",
      n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
      ctx.r9.u32, ctx.r10.u32,
      p7 ? sr::kernel::readGuestU32BE(p7) : 0, p7 ? sr::kernel::readGuestU32BE(p7 + 4) : 0,
      p7 ? sr::kernel::readGuestU32BE(p7 + 8) : 0, p7 ? sr::kernel::readGuestU32BE(p7 + 12) : 0,
      app, flags, (uint32_t)ctx.lr);
  }
  __imp__sub_827B9060(ctx, base);
  if (n <= 12) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[TILER] #%u RETURNED", n);
  }
}

// iter-214: CONTENT-FETCH TRACE (ROADMAP layer 2). sub_826D8A10 = RTAM
// register-package = the ENUMERATOR. It queries the package source object's
// asset count via source->vtable[0x2c], allocates a count-sized id table, and
// stores that table at [registrar+8] with the count at table[0]. If ui.AST is
// mounted header-only, that count is 0 (asset directory never parsed) → the
// per-asset load never fires for "main". This logger reads back the computed
// count post-call so we can compare ui.AST (suspected 0) vs the loose .AST
// packages (>0). r3 = registrar, r4 = package source object (peek a name).
extern "C" void __imp__sub_826D8A10(PPCContext& __restrict ctx, uint8_t* base);

static void srPeekName(uint32_t obj, char* out, int outsz) {
  out[0] = 0;
  if (obj < 0x10000u) return;
  for (int off = 4; off <= 64 && out[0] == 0; off += 4) {
    uint32_t p = sr::kernel::readGuestU32BE(obj + off);
    if (p < 0x10000u || p >= 0xC0000000u) continue;
    uint8_t* h = sr::kernel::guestVAToHost(p);
    if (!h || h[0] < 32 || h[0] >= 127) continue;
    int ok = 0;
    for (int i = 0; i < outsz - 1; ++i) {
      char c = (char)h[i];
      if (c == 0) break;
      if (c < 32 || c >= 127) { ok = 0; break; }
      out[i] = c; ok++;
    }
    if (ok < 2) out[0] = 0;
  }
}

void sub_826D8A10(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t reg = ctx.r3.u32, src = ctx.r4.u32, alr = (uint32_t)ctx.lr;
  char nm[48]; srPeekName(src, nm, sizeof(nm));
  __imp__sub_826D8A10(ctx, base);
  // [registrar+8] = id-table; table[0] = asset count (table = [reg+8]; count = [table-4]).
  uint32_t tbl = (reg >= 0x10000u) ? sr::kernel::readGuestU32BE(reg + 8) : 0;
  uint32_t count = (tbl >= 0x10000u) ? sr::kernel::readGuestU32BE(tbl - 4) : 0;
  SR_LOG_BOOT(sr::LogSeverity::Info,
    "[PKG_ENUM] #%u register-package reg=0x%08X src=0x%08X name=\"%s\" assetCount=%u table=0x%08X lr=0x%08X",
    n, reg, src, nm, count, tbl, alr);
}

// iter-214: the per-asset DRIVER. sub_826D8A10's loop calls this once per
// enumerated asset; it invokes asset->vtable[3] x2 to fetch/parse the asset.
// If [PKG_ENUM] shows a non-zero count for ui.AST but this never fires for it,
// the gap is between enumeration and drive; if it fires with "main" we've found
// the path. Log the asset object, a name, and the caller LR.
extern "C" void __imp__sub_826D8480(PPCContext& __restrict ctx, uint8_t* base);

void sub_826D8480(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t a3 = ctx.r3.u32, alr = (uint32_t)ctx.lr;
  char nm[48]; srPeekName(a3, nm, sizeof(nm));
  if (n <= 200 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[ASSET_DRIVE] #%u sub_826D8480 asset=0x%08X name=\"%s\" lr=0x%08X",
      n, a3, nm, alr);
  }
  __imp__sub_826D8480(ctx, base);
}

// iter-203: empirical test — does ANY Apt movie definition ever execute or
// get attached? sub_826B2298 = "begin movie-def processing" (pushes the def
// scope [0x829A1630]); sub_826B0458 = the type-12 clip constructor (movieDef
// arg in r5). If sub_826B2298 never fires and sub_826B0458's r5 is always 0,
// no movie is ever instanced/run — the whole frontend never starts.
extern "C" void __imp__sub_826B2298(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826B0458(PPCContext& __restrict ctx, uint8_t* base);

void sub_826B2298(PPCContext& __restrict ctx, uint8_t* base) {
  sr::kernel::gaugeBump(sr::kernel::Gauge::DefExec);  // iter-358: uncapped count
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 20 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[DEF_EXEC] #%u sub_826B2298 begin-def this=0x%08X save=0x%08X lr=0x%08X",
      n, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_826B2298(ctx, base);
}

namespace sr { namespace kernel { extern std::atomic<uint32_t> g_lastClipNode; } }
void sub_826B0458(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t node = ctx.r3.u32, movieDef = ctx.r5.u32, alr = (uint32_t)ctx.lr;
  // iter-295b: publish the newest clip node for the MAIN instantiation drive
  // (splash_hooks.cpp BLOB_INJECT watcher installs the built def into it).
  if (node >= 0x10000u && node < 0xC0000000u)
    sr::kernel::g_lastClipNode.store(node, std::memory_order_relaxed);
  // iter-358: uncapped counts. clipCtorAll vs clipCtorReal are SEPARATE gauges
  // on purpose — the old aggregate counted [CLIP_CTOR] log lines, conflating
  // NULL-movieDef bookkeeping ctors with real movie instances, so "clipCtor=453"
  // could be 453 empties and zero movies. clipCtorReal is the one that means
  // "a movie got instanced".
  sr::kernel::gaugeBump(sr::kernel::Gauge::ClipCtorAll);
  static std::atomic<unsigned> s_n{0}, s_nz{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  bool nonNull = movieDef >= 0x10000u;
  if (nonNull) {
    sr::kernel::gaugeBump(sr::kernel::Gauge::ClipCtorReal);
    unsigned z = s_nz.fetch_add(1, std::memory_order_relaxed) + 1;
    // iter-461: the gauge is the count; the line keeps the first 200 and a sample.
    if (z <= 200 || (z % 64) == 0 || sr::kernel::logVerbose())
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[CLIP_CTOR] #%u NON-NULL movieDef=0x%08X node=0x%08X lr=0x%08X (nonNull #%u)",
      n, movieDef, node, alr, z);
  } else if (n <= 10 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[CLIP_CTOR] #%u movieDef=NULL node=0x%08X lr=0x%08X", n, node, alr);
  }
  __imp__sub_826B0458(ctx, base);
}

// iter-198: search-path open (one level below the package-walk loop). The
// caller LR here identifies the walk function that decides "next package".
extern "C" void __imp__sub_82605538(PPCContext& __restrict ctx, uint8_t* base);

void sub_82605538(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t a4 = ctx.r4.u32, alr = (uint32_t)ctx.lr;
  char p[64] = {0};
  if (a4 >= 0x10000u) {
    uint8_t* h = sr::kernel::guestVAToHost(a4);
    if (h) {
      for (int i = 0; i < 63; ++i) {
        char c = (char)h[i];
        if (c == 0) break;
        p[i] = (c >= 32 && c < 127) ? c : '.';
      }
    }
  }
  __imp__sub_82605538(ctx, base);
  if (n <= 80 || (n & (n - 1)) == 0) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[FM_OPEN2] #%u path=\"%s\" -> 0x%08X lr=0x%08X", n, p, ctx.r3.u32, alr);
  }
}

// iter-198: streamer capture. sub_8236F0C0(streamer, &state) is the inline
// pump-until-state-13 used by the self-pumping whole-file load (runs at boot
// for TEXT.AST). Publish the streamer so the fabricator can drive the package
// mounter (sub_8236DD90) on it.
extern "C" void __imp__sub_8236F0C0(PPCContext& __restrict ctx, uint8_t* base);

void sub_8236F0C0(PPCContext& __restrict ctx, uint8_t* base) {
  if (ctx.r3.u32 >= 0x10000u) {
    uint32_t prev = sr::kernel::g_streamerVA.exchange(ctx.r3.u32, std::memory_order_relaxed);
    if (prev != ctx.r3.u32) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[STREAMER] captured streamer=0x%08X (pump caller lr=0x%08X)",
        ctx.r3.u32, (uint32_t)ctx.lr);
    }
  }
  __imp__sub_8236F0C0(ctx, base);
}

// iter-191: range guard on the pool queue's pop-compact (refcounted range copy).
// sub_826F9CD0(src_begin=r3, src_end=r4, dst=r5) is called only from the FMUI
// pool pump (sub_826F92E0) to compact the queue after a pop. When the queue
// {begin,end} fields are scribbled (191a), dst lands misaligned inside .data
// (0x8296D8D5) and the copy byte-strips ~19 KB of module globals, the PSM
// pointer, and the Apt flow singleton/env — the whole iter-24z corruption
// family. A legit dst is always a 4-aligned heap buffer outside the XEX image.
extern "C" void __imp__sub_826F9CD0(PPCContext& __restrict ctx, uint8_t* base);

void sub_826F9CD0(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t srcBegin = ctx.r3.u32;
  const uint32_t srcEnd   = ctx.r4.u32;
  const uint32_t dst      = ctx.r5.u32;
  const uint32_t count    = (srcEnd > srcBegin) ? (srcEnd - srcBegin) / 4 : 0;
  const bool misaligned   = ((dst | srcBegin) & 3u) != 0;
  const bool inImage      = (dst >= 0x82000000u && dst < 0x82B00000u) ||
                            (srcBegin >= 0x82000000u && srcBegin < 0x82B00000u);
  // iter-463 charter (burndown item 4): the guard is a registered band-aid and
  // ran with NO gate. "[FMUI_COMPACT_GUARD]" is in 0/150 run logs (81 before
  // 60a90bf, 69 after), and its scribbled-queue premise is the heap-corruption
  // family 60a90bf fixed. The default exe now only OBSERVES the shape and runs
  // the guest's own copy; the block + queue collapse is SR_ALLOW_FORCING only.
  if (misaligned || inImage || count > 65536u) {
    static std::atomic<unsigned> s_blocked{0};
    unsigned b = s_blocked.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool block = sr::kernel::forcingAllowed();
    if (b <= 20 || (b & (b - 1)) == 0) {
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[FMUI_COMPACT_GUARD] %s sub_826F9CD0 src={0x%08X..0x%08X} dst=0x%08X "
        "count=%u (misaligned=%d inImage=%d) (hit #%u)",
        block ? "BLOCKED" : "OBSERVED (not blocked: SR_ALLOW_FORCING off)",
        srcBegin, srcEnd, dst, count, (int)misaligned, (int)inImage, b);
    }
    if (!block) {
      __imp__sub_826F9CD0(ctx, base);
      return;
    }
    // Collapse the scribbled queue so the pump doesn't drain thousands of
    // garbage "jobs" (each a potential vcall on a byte-rotated pointer).
    // Restore {begin, end} to the healthy buffer captured at worker entry
    // (begin = healthy, end = healthy+4); the pump's unconditional post-compact
    // `end -= 4` then lands end == begin → size 0 → back to the condvar wait.
    // Fallback if no healthy pointer: end := srcBegin (= begin+4), same effect
    // but begin stays garbage (a later push would still need the watchpoint fix).
    uint32_t qva = sr::kernel::g_fmuiPoolQueueVA.load(std::memory_order_relaxed);
    if (qva != 0) {
      uint32_t healthy = s_fmuiQueueHealthyBegin.load(std::memory_order_relaxed);
      uint32_t newBegin = healthy ? healthy : 0;
      uint32_t newEnd   = healthy ? healthy + 4 : srcBegin;
      if (newBegin != 0) {
        if (uint8_t* hostB = sr::kernel::guestVAToHost(qva)) {
          hostB[0] = (newBegin >> 24) & 0xFF;
          hostB[1] = (newBegin >> 16) & 0xFF;
          hostB[2] = (newBegin >>  8) & 0xFF;
          hostB[3] =  newBegin        & 0xFF;
        }
      }
      if (uint8_t* hostE = sr::kernel::guestVAToHost(qva + 4)) {
        hostE[0] = (newEnd >> 24) & 0xFF;
        hostE[1] = (newEnd >> 16) & 0xFF;
        hostE[2] = (newEnd >>  8) & 0xFF;
        hostE[3] =  newEnd        & 0xFF;
      }
    }
    ctx.r3.u64 = dst;  // return value is the final dst cursor; caller ignores it
    return;
  }
  __imp__sub_826F9CD0(ctx, base);
}

// iter-DOGMA-RE-20/21: catch the runaway loop on Function_826E55F8 from
// caller_lr=0x826E5740 (post-iter-19 FMUI-loaded).
//
// Strategy: per-(caller_lr, param) call-count tracker. If the same (lr, param)
// pair is called >1000 times within a 100ms window, return early — caller is
// in a runaway loop. Each iteration's "work" is a CS Enter + vtable dispatch
// + CS Leave; skipping it for the runaway iterations prevents the spin.
//
// Diagnostic logging stays in place; first 5 calls + every 100K + every loop
// detection event.
void sub_826E55F8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<uint64_t> s_calls{0};
  static std::atomic<uint32_t> s_topLR{0};
  static std::atomic<uint64_t> s_topLRCount{0};
  static std::atomic<int64_t>  s_topLRWindowStartMs{0};
  static std::atomic<uint64_t> s_loopBreaks{0};

  uint64_t n = s_calls.fetch_add(1, std::memory_order_relaxed);
  uint32_t lr = static_cast<uint32_t>(ctx.lr);
  int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();

  // Per-caller-LR rate tracking.
  uint32_t topLR = s_topLR.load(std::memory_order_relaxed);
  uint64_t curCount = 0;
  if (topLR == lr) {
    curCount = s_topLRCount.fetch_add(1, std::memory_order_relaxed) + 1;
    int64_t winStart = s_topLRWindowStartMs.load(std::memory_order_relaxed);
    // Reset window if > 100ms elapsed since first call of this LR streak.
    if (nowMs - winStart > 100) {
      s_topLRWindowStartMs.store(nowMs, std::memory_order_relaxed);
      s_topLRCount.store(1, std::memory_order_relaxed);
      curCount = 1;
    }
  } else {
    // Switch top-LR if current isn't dominating.
    if (s_topLRCount.load(std::memory_order_relaxed) < 100) {
      s_topLR.store(lr, std::memory_order_relaxed);
      s_topLRCount.store(1, std::memory_order_relaxed);
      s_topLRWindowStartMs.store(nowMs, std::memory_order_relaxed);
      curCount = 1;
    }
  }

  // Loop detection: same LR called > 1000 times in <100ms.
  if (curCount > 1000) {
    uint64_t b = s_loopBreaks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (b == 1 || (b & (b - 1)) == 0) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[826E55F8_LOOP_BREAK] #%llu lr=0x%08X param=0x%08X count=%llu — returning early",
        (unsigned long long)b, lr, ctx.r3.u32, (unsigned long long)curCount);
    }
    return;  // caller's runaway loop will get nothing done — caller may detect or not
  }

  // Sample logging: first 5 + every 100K.
  bool log = (n < 5) || (n % 100000 == 0);
  if (log) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[826E55F8_LR] sub_826E55F8 #%llu param=0x%08X caller_lr=0x%08X (topLR=0x%08X count=%llu)",
      (unsigned long long)(n + 1), ctx.r3.u32, lr,
      s_topLR.load(std::memory_order_relaxed),
      (unsigned long long)s_topLRCount.load(std::memory_order_relaxed));
  }
  __imp__sub_826E55F8(ctx, base);
}

// iter-DOGMA-RE-18 diagnostic: capture caller LR at sub_825F6B90 (CS Leave
// wrapper) entry. From iter-17 we know FMUI hangs in a 798K calls/sec
// iter-365w CONTENDED-LOCK DIAGNOSTIC (SR_CS_WAIT_TRACE=1, opt-in).
// The dominant natural stall (2/6 rolls of the iter-365v batches, both at the
// same conveyor entry) is a thread blocked inside CsEnterWithDeadline for 15+
// seconds. A sampler snapshot names the WAITER but never the LOCK or its
// HOLDER, so record the last successful enterer per critical section and report
// any wait longer than kCsWaitWarnMs with both sides. Direct-mapped table, no
// allocation, plain relaxed atomics — cheap enough to leave in a hot path.
namespace sr { namespace kernel { void dumpGuestThreadNativeStacks(const char* tag, int maxFrames);
void dumpHostThreadNativeStack(const char* tag, uint32_t tid, int maxFrames); } }  // native_stack_probe.cpp
namespace {
struct CsOwnerSlot {
  std::atomic<uint32_t> cs{0};
  std::atomic<uint32_t> tid{0};
  std::atomic<uint32_t> lr{0};
  std::atomic<uint32_t> hostTid{0};  // iter-428: bridge to the thread registry
};
CsOwnerSlot g_csOwners[256];
inline CsOwnerSlot& csSlot(uint32_t cs) { return g_csOwners[(cs >> 4) & 255]; }
bool csWaitTrace() {
  // iter-436 STABILITY CAMPAIGN: BAKED DEFAULT-ON (=0 keeps the A/B). The
  // freeze family (~1/3 rolls; task-pipe park + CsEnterWithDeadline deadlock)
  // is intermittent — every unattributed occurrence is a wasted roll. With
  // the trace always armed, ANY freeze self-names {waiter, cs, holder,
  // holder_lr} in the log. Overhead: slot hash + timestamps per enter,
  // measured negligible (K=3 clean, deep signature intact).
  static const bool v = [] {
    const char* e = std::getenv("SR_CS_WAIT_TRACE");
    return !e || e[0] != '0';
  }();
  return v;
}
constexpr long long kCsWaitWarnMs = 250;

// iter-428: ENTRY-SIDE wedge watchdog. The [CS_WAIT] tracer measures on
// RETURN, so a thread that never acquires never logs — the permanent wedge is
// exactly the case the return-side instrument cannot see. Every armed enter
// registers itself here; a wall-clock thread sweeps for waits older than 8s
// and names the CS, the waiter, and the last-known holder while the wedge is
// still standing.
struct CsPendingSlot {
  std::atomic<uint32_t> cs{0}, tid{0}, lr{0};
  std::atomic<long long> t0{0};
};
CsPendingSlot g_csPending[64];
long long csNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
// iter-428c: FMV-SM CS pairing tracer. alloc_overlap_watch latches sm+560 at
// queue time; every enter/leave on that CS logs {op, rc, guest tid, host tid,
// lr, live recursion} — the orphaned-lock leak (worker exits "normally" with
// guest recursion still 1) names its own unpaired enter from this history.
bool csPairOn() { return csWaitTrace(); }
std::atomic<unsigned> g_csPairN{0};
void csPairLog(const char* op, uint32_t cs, uint32_t lr, int rc) {
  const unsigned n = g_csPairN.fetch_add(1, std::memory_order_relaxed);
  if (n > 4000) return;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[CS_PAIR] %s cs=%08X tid=%u host=%u lr=%08X rc=%d rec=%08X own=%08X",
      op, cs, sr::kernel::getCurrentThreadHandle(), GetCurrentThreadId(), lr,
      rc, sr::kernel::readGuestU32BE(cs + 0x20),
      sr::kernel::readGuestU32BE(cs + 0x14));
}

void csWedgeWatchdogStart() {
  static std::once_flag s_once;
  std::call_once(s_once, [] {
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const long long now = csNowMs();
        for (auto& p : g_csPending) {
          const uint32_t cs = p.cs.load(std::memory_order_relaxed);
          if (!cs) continue;
          const long long age = now - p.t0.load(std::memory_order_relaxed);
          if (age < 8000) continue;
          CsOwnerSlot& o = csSlot(cs);
          SR_LOG_BOOT(sr::LogSeverity::Error,
              "[CS_WEDGE] cs=%08X waiter_tid=%u waiter_lr=%08X age=%llds "
              "last_holder_tid=%u holder_host=%u last_holder_lr=%08X rec=%08X",
              cs, p.tid.load(std::memory_order_relaxed),
              p.lr.load(std::memory_order_relaxed), (long long)(age / 1000),
              o.tid.load(std::memory_order_relaxed),
              o.hostTid.load(std::memory_order_relaxed),
              o.lr.load(std::memory_order_relaxed),
              sr::kernel::readGuestU32BE(cs + 0x20));
          // Name the holder's LIVE position: dump every registered guest
          // thread (host tid + lr/r1/r3/ctr) alongside — the holder_host row
          // in this dump IS where the deadlock's root currently sits.
          static std::atomic<long long> s_lastDump{0};
          long long ld = s_lastDump.load(std::memory_order_relaxed);
          if (now - ld > 30000 &&
              s_lastDump.compare_exchange_strong(ld, now, std::memory_order_relaxed))
            sr::kernel::dumpGuestThreadsNow("CS_WEDGE");
        }
      }
    }).detach();
  });
}

// Step 4 item 7a (lane D, 2026-10-18; DIAGNOSTIC, default OFF, observe only): SR_CS_HOLDER_STACK=1.
// The boot movie-transition freezes are render-tick waits on cs 05AE9A98 whose [CS_WAIT] holder_lr (8271EE0C) is
// stale: the recomp updates ctx.lr only at indirect calls, and the owner slot names the LAST enterer, which need not
// be the thread inside. While such a wait is still standing (>= SR_CS_HOLDER_STACK_MS, default 150 ms) this dumps the
// HOST stacks of every registered guest thread (native_stack_probe.cpp: suspend -> unwind -> resume, then symbolize;
// the recompiled functions are host symbols sub_XXXXXXXX), once per pending enter, so the thread inside the lock
// names its own code. SR_CS_HOLDER_STACK_CS=<hex> limits it to one CS; SR_CS_HOLDER_STACK_MAX (12) caps the episodes.
// The guest's own CS words are logged beside it: +0x14 (the owner field [CS_PAIR] reads) and +0x20 (recursion).
bool csHolderStackOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_CS_HOLDER_STACK");
    return e && e[0] == '1';
  }();
  return v;
}

void csHolderStackStart() {
  static std::once_flag s_once;
  std::call_once(s_once, [] {
    auto envLong = [](const char* n, long def, long lo, long hi) {
      const char* e = std::getenv(n);
      const long v = (e && *e) ? std::strtol(e, nullptr, 10) : def;
      return v < lo ? lo : v > hi ? hi : v;
    };
    const long long thresholdMs = envLong("SR_CS_HOLDER_STACK_MS", 150, 20, 10000);
    const int maxEpisodes = int(envLong("SR_CS_HOLDER_STACK_MAX", 12, 1, 200));
    const char* only = std::getenv("SR_CS_HOLDER_STACK_CS");
    const uint32_t onlyCs = (only && *only) ? uint32_t(std::strtoul(only, nullptr, 16)) : 0u;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[CS_HOLDER_STACK] ARMED threshold=%lldms cs=%08X max=%d exe_base=%p (stacks: [CS_HOLDER_STACK_T])",
        thresholdMs, onlyCs, maxEpisodes, (void*)GetModuleHandleW(nullptr));
    std::thread([thresholdMs, maxEpisodes, onlyCs] {
      SetThreadDescription(GetCurrentThread(), L"SR CsHolderStack");
      struct Seen { uint32_t cs; long long t0; };
      Seen cand[64] = {}, done[64] = {};
      int episodes = 0;
      while (episodes < maxEpisodes) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const long long now = csNowMs();
        for (int i = 0; i < 64 && episodes < maxEpisodes; ++i) {
          CsPendingSlot& p = g_csPending[i];
          const uint32_t cs = p.cs.load(std::memory_order_relaxed);
          const long long t0 = p.t0.load(std::memory_order_relaxed);
          // a slot's cs is claimed before its t0 is written: count only a (cs, t0) pair seen on two polls in a row
          const bool stable = cs && cand[i].cs == cs && cand[i].t0 == t0;
          cand[i] = {cs, t0};
          if (!stable || (onlyCs && cs != onlyCs) || now - t0 < thresholdMs) continue;
          if (done[i].cs == cs && done[i].t0 == t0) continue;
          done[i] = {cs, t0};
          ++episodes;
          CsOwnerSlot& o = csSlot(cs);
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[CS_HOLDER_STACK] episode #%d cs=%08X age=%lldms waiter_tid=%u waiter_lr=%08X last_holder_tid=%u "
              "holder_host=%u last_holder_lr=%08X own=%08X rec=%08X",
              episodes, cs, now - t0, p.tid.load(std::memory_order_relaxed), p.lr.load(std::memory_order_relaxed),
              o.tid.load(std::memory_order_relaxed), o.hostTid.load(std::memory_order_relaxed),
              o.lr.load(std::memory_order_relaxed), sr::kernel::readGuestU32BE(cs + 0x14),
              sr::kernel::readGuestU32BE(cs + 0x20));
          sr::kernel::dumpGuestThreadNativeStacks("CS_HOLDER_STACK_T", 24);
          // the holder itself, by its host tid (it may be outside the guest thread registry)
          sr::kernel::dumpHostThreadNativeStack("CS_HOLDER_STACK_T", o.hostTid.load(std::memory_order_relaxed), 24);
          SR_LOG_BOOT(sr::LogSeverity::Warn, "[CS_HOLDER_STACK] episode #%d end: wait still pending=%d", episodes,
                      int(p.cs.load(std::memory_order_relaxed) == cs && p.t0.load(std::memory_order_relaxed) == t0));
        }
      }
      SR_LOG_BOOT(sr::LogSeverity::Info, "[CS_HOLDER_STACK] %d episodes dumped: off", maxEpisodes);
    }).detach();
  });
}
}  // namespace

// Leave-only loop; the LR captured INSIDE the kernel stub is sub_825F6B90's
// return point (uninformative). Capturing on entry tells us who calls
// sub_825F6B90 — that's the loop's body function we need to override.
void sub_825F6B90(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t paramVA = ctx.r3.u32;
  const uint32_t callerLR = ctx.lr;
  static std::atomic<uint64_t> s_calls{0};
  uint64_t n = s_calls.fetch_add(1, std::memory_order_relaxed);
  // iter-213: keep sampling THROUGHOUT the sustained storm (every 1M) so the
  // dominant spinner's caller_lr + CS object + recursion count are visible
  // while it's running — not just at fixed early points. Also log thread.
  if (n < 5 || n == 100 || n == 10000 || (n % 1000000 == 0)) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[LEAVE_WRAPPER_LR] sub_825F6B90 #%llu param=0x%08X caller_lr=0x%08X rec=0x%08X thread=%u",
      (unsigned long long)(n + 1), paramVA, callerLR,
      (paramVA >= 0x10000u ? sr::kernel::readGuestU32BE(paramVA + 0x20) : 0u),
      sr::kernel::getCurrentThreadHandle());
  }
  // Release ownership before the guest actually leaves: a slot that still names
  // us after this point would frame the wrong holder.
  if (csWaitTrace() && paramVA >= 0x10000u) {
    CsOwnerSlot& s = csSlot(paramVA);
    if (s.cs.load(std::memory_order_relaxed) == paramVA &&
        s.tid.load(std::memory_order_relaxed) == sr::kernel::getCurrentThreadHandle()) {
      s.cs.store(0, std::memory_order_relaxed);
      s.tid.store(0, std::memory_order_relaxed);
    }
#if SR_ORACLE
    extern std::atomic<uint32_t> g_fmvSmCsVA;
    if (paramVA == g_fmvSmCsVA.load(std::memory_order_relaxed) && paramVA)
      csPairLog("LEAVE", paramVA, callerLR, 0);
#endif
  }
  __imp__sub_825F6B90(ctx, base);
}

namespace sr { namespace kernel { extern std::atomic<uint32_t> g_kloopMgrs[4]; } }

// iter-398h: [CSRUN] — every enter on a RUNNER's loop CS (runner+8; runners
// registered by fe_flow_watch's shimKernelLoopRun). The movie-2 wedge = the
// render runner (R2) blocked forever in CsEnterWithDeadline on its OWN CS
// (sampler lr=825FC134); the foreign enter that never leaves is the holder.
static inline int runnerCsIndex(uint32_t csVA) {
  for (int i = 0; i < 4; ++i) {
    const uint32_t m = sr::kernel::g_kloopMgrs[i].load(std::memory_order_relaxed);
    if (m && csVA == m + 8u) return i;
  }
  return -1;
}

void sub_825FC108(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t paramVA = ctx.r3.u32;  // capture before __imp__ clobbers
  const uint32_t callerLR = ctx.lr;
  {
    const int ri = runnerCsIndex(paramVA);
    if (ri >= 0) {
      static std::atomic<unsigned> s_rn[4]{};
      static std::atomic<uint32_t> s_ownTid[4]{};
      const uint32_t tid = sr::kernel::getCurrentThreadHandle();
      unsigned rn = s_rn[ri].fetch_add(1, std::memory_order_relaxed) + 1;
      uint32_t own = s_ownTid[ri].load(std::memory_order_relaxed);
      if (own == 0) s_ownTid[ri].compare_exchange_strong(own, tid, std::memory_order_relaxed);
      // The runner's own loop enters ~30/s (sampled); a FOREIGN thread on a
      // runner CS is the wedge suspect — log those uncapped.
      const bool foreign = own != 0 && tid != own;
      if (foreign || rn <= 6 || (rn & (rn - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[CSRUN] R%d ENTER%s #%u cs=%08X lr=%08X tid=%u", ri,
            foreign ? "-FOREIGN" : "", rn, paramVA, callerLR, tid);
    }
  }
  // iter-386 [CS_GARBAGE]: console-impossible CS addresses reach this enter
  // (e.g. FOURCC 'FMRe'=0x464D5265 via FmModuleInitOnce lr=8271F2F0 — would
  // AV on hardware; our flat reservation makes them readable). The enter then
  // spins on whatever bytes sit there: usually "unlocked" (instant), but a
  // 12.7s main-thread stall on cs=33445849 was caught live (run0 10:00:46),
  // and a forever-looking-locked pattern would stall a thread PERMANENTLY —
  // a candidate roll-dependence generator for the conveyor death. Log every
  // enter whose CS is outside all plausible guest regions (capped).
  {
    const bool plausible =
        (paramVA >= 0x82000000u && paramVA < 0x82B00000u) ||   // statics
        (paramVA >= 0x83000000u && paramVA < 0x9F000000u) ||   // heaps/streams
        (paramVA >= 0xBE000000u && paramVA < 0xBF000000u) ||   // main stack
        (paramVA >= 0xA0000000u && paramVA < 0xB0000000u) ||   // big file cache
        (sr::kernel::xheap::enabled() && sr::kernel::xheap::inRange(paramVA));  // iter-463 Xenia heaps
    if (!plausible) {
      static std::atomic<unsigned> s_g{0};
      unsigned g = s_g.fetch_add(1, std::memory_order_relaxed) + 1;
      if (g <= 64 || (g & (g - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Error,
            "[CS_GARBAGE] #%u cs=%08X (console-impossible) caller_lr=%08X tid=%u",
            g, paramVA, callerLR,
            sr::kernel::getCurrentThreadHandle());
    }
  }
  static const bool s_sweepTrace = [] {
    const char* e = std::getenv("SR_PASS_TRACE");
    return e && e[0] == '1';
  }();
  const uint64_t threadToken =
      std::hash<std::thread::id>{}(std::this_thread::get_id());
  const bool onSweepThread = s_sweepTrace &&
      threadToken == sr::kernel::g_sweepThreadToken.load(std::memory_order_relaxed);
  unsigned sweepN = 0;
  if (onSweepThread) {
    static std::atomic<unsigned> s_sweepN{0};
    sweepN = s_sweepN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sweepN <= 2048)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[SWEEP_CS_ENTER] #%u param=%08X caller=%08X",
          sweepN, paramVA, callerLR);
  }

  const bool csTrace = csWaitTrace();
  // Announce once: silence from an unvalidated instrument is indistinguishable
  // from "no contention", and that ambiguity has cost this project real time.
  if (csTrace) {
    static std::atomic<bool> s_announced{false};
    bool expected = false;
    if (s_announced.compare_exchange_strong(expected, true))
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[CS_WAIT] ARMED (SR_CS_WAIT_TRACE=1, threshold %lldms) — absence of "
          "[CS_WAIT] lines below now means no wait exceeded it",
          (long long)kCsWaitWarnMs);
  }
  const auto csT0 = csTrace ? std::chrono::steady_clock::now()
                            : std::chrono::steady_clock::time_point{};

  // iter-428: entry-side wedge registry (see csWedgeWatchdogStart). Claim a
  // slot for the duration of the enter; the watchdog names us if we never
  // come back.
  CsPendingSlot* pend = nullptr;
  if (csTrace && paramVA >= 0x10000u) {
    csWedgeWatchdogStart();
    if (csHolderStackOn()) csHolderStackStart();   // 7a: SR_CS_HOLDER_STACK=1 (default OFF)
    for (auto& p : g_csPending) {
      uint32_t expect = 0;
      if (p.cs.compare_exchange_strong(expect, paramVA,
                                       std::memory_order_relaxed)) {
        p.tid.store(sr::kernel::getCurrentThreadHandle(),
                    std::memory_order_relaxed);
        p.lr.store(callerLR, std::memory_order_relaxed);
        p.t0.store(csNowMs(), std::memory_order_relaxed);
        pend = &p;
        break;
      }
    }
  }

  // R1 (docs/SPLICE_CONVERSION.md): the [FC108] call counter the old splice body logged at
  // its entry; run_recomp_x's spinStorm gauge reads the largest call#. One call of the body
  // per call of this override, so the count is the same.
  {
    static std::atomic<uint64_t> s_fc108_calls{0};
    const uint64_t n = s_fc108_calls.fetch_add(1, std::memory_order_relaxed);
    if (n < 5 || n % 2000000 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[FC108] call#%llu r3=0x%08X caller_lr=0x%08X",
                  (unsigned long long)(n + 1), ctx.r3.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_825FC108(ctx, base);

  if (pend) pend->cs.store(0, std::memory_order_relaxed);
  // iter-428c: pairing trace on the latched FMV-SM CS.
  if (csTrace) {
#if SR_ORACLE
    extern std::atomic<uint32_t> g_fmvSmCsVA;
    if (paramVA == g_fmvSmCsVA.load(std::memory_order_relaxed) && paramVA)
      csPairLog("ENTER", paramVA, callerLR, ctx.r3.s32);
#endif
  }

  if (csTrace && paramVA >= 0x10000u) {
    const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - csT0).count();
    CsOwnerSlot& s = csSlot(paramVA);
    if (waitedMs >= kCsWaitWarnMs) {
      static std::atomic<unsigned> s_warn{0};
      unsigned w = s_warn.fetch_add(1, std::memory_order_relaxed) + 1;
      if (w <= 200 || (w % 64) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[CS_WAIT] #%u cs=%08X waited=%lldms waiter_tid=%u waiter_lr=%08X "
            "holder_tid=%u holder_lr=%08X rec=%08X",
            w, paramVA, (long long)waitedMs,
            sr::kernel::getCurrentThreadHandle(), callerLR,
            s.tid.load(std::memory_order_relaxed),
            s.lr.load(std::memory_order_relaxed),
            sr::kernel::readGuestU32BE(paramVA + 0x20));
    }
    // Record ourselves as the current holder (the guest returns >0 on success).
    if (ctx.r3.s32 > 0) {
      s.cs.store(paramVA, std::memory_order_relaxed);
      s.tid.store(sr::kernel::getCurrentThreadHandle(), std::memory_order_relaxed);
      s.lr.store(callerLR, std::memory_order_relaxed);
      s.hostTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    }
  }

  if (onSweepThread && sweepN <= 2048)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[SWEEP_CS_RETURN] #%u param=%08X caller=%08X rc=%08X",
        sweepN, paramVA, callerLR, ctx.r3.u32);

  // Detect the -2 timeout return and convert it to a faked-success +1.
  // iter-389 RETIREMENT (registry band-aid #1): the conversion fired ZERO
  // times across every iter-385..388 batch — the timeout path is dormant on
  // the healthy boot. Default OFF (console-faithful: the caller receives the
  // -2 the guest code produced); if a -2 EVER occurs, alarm loudly so the
  // regression is seen instead of silently papered over. SR_CS_TIMEOUT_CONVERT=1
  // restores the old conversion for diagnosis.
  static const bool s_convertOn = [] {
    const char* e = std::getenv("SR_CS_TIMEOUT_CONVERT");
    return e && e[0] == '1';
  }();
  if (ctx.r3.s32 == -2 && !s_convertOn) {
    static std::atomic<unsigned> s_raw{0};
    unsigned rn = s_raw.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rn <= 16)
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[CS_TIMEOUT_RAW] #%u sub_825FC108 returned -2 UNCONVERTED (retired "
        "band-aid would have faked +1) param=0x%08X caller_lr=0x%08X — "
        "investigate the timeout, do not re-enable blindly",
        rn, paramVA, callerLR);
  }
  if (ctx.r3.s32 == -2 && s_convertOn) {
    static std::atomic<unsigned> s_fixCount{0};
    unsigned n = s_fixCount.fetch_add(1, std::memory_order_relaxed) + 1;

    // Manually do what the success path would have done: increment refcount
    // at param+0x20 (the field at offset 32, per the recomp's
    // `lwz r11, 32(r31); addi r3, r11, 1; stw r3, 32(r31)` sequence).
    if (uint8_t* host = sr::kernel::guestVAToHost(paramVA + 0x20)) {
      uint32_t cur = (uint32_t(host[0]) << 24) | (uint32_t(host[1]) << 16) |
                     (uint32_t(host[2]) <<  8) |  uint32_t(host[3]);
      uint32_t next = cur + 1;
      host[0] = (next >> 24) & 0xFF;
      host[1] = (next >> 16) & 0xFF;
      host[2] = (next >>  8) & 0xFF;
      host[3] =  next        & 0xFF;
      ctx.r3.s64 = static_cast<int64_t>(static_cast<int32_t>(next));
    } else {
      ctx.r3.s64 = 1;  // safe fallback
    }

    if (n <= 8 || (n & (n - 1)) == 0) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[CS_TIMEOUT_FIX] sub_825FC108 #%u param=0x%08X caller_lr=0x%08X — "
        "converted -2 timeout to +%lld (refcount preincr) so matched Leave "
        "doesn't underflow",
        n, paramVA, callerLR, (long long)ctx.r3.s64);
    }
  }
}

// iter-24: worker-kill for sub_8258C278 (FMGameScript hang family).
//
// Loop structure (ppc_recomp.67.cpp:9656-9930):
//   r31 = r3 (this), iteration count = *(r31 + 16), begin pointer = *(r31 + 12)
//   if count <= 0: skip loop entirely (line 9686-9687)
//   else: loop count times, advancing index by 12 bytes each iteration
//   Each iteration: dispatch via vtable, then write at *(begin + 12*idx + 8)
//
// When begin points into image .rdata (e.g. r28=0x820979C0 + offset = 0x8209AFFE
// per iter-24d capture), every iteration writes to image space. Multiple worker
// threads run this concurrently. The watchdog catches each write but the
// LOOPS themselves don't terminate, so workers spin and the modules waiting
// on them (FMGameScript, FMGameRtamIni, etc.) hang indefinitely.
//
// Same fix pattern as iter-19's sub_826F92E0: zero the count BEFORE delegating
// to original. The original sees count=0 at the prologue check and takes the
// early-exit path. Worker terminates cleanly. Modules waiting on the worker
// proceed.
//
// Risk: caller may expect the loop to have done useful work. In iter-24's
// soaks the caller's CTOR was hanging anyway, so converting "infinite spin"
// to "no-op" trades a hard hang for a possibly-broken initialization. Same
// risk class as iter-19, accepted.
void sub_8258C278(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t thisVA = ctx.r3.u32;
  static std::atomic<unsigned> s_callCount{0};
  unsigned n = s_callCount.fetch_add(1, std::memory_order_relaxed) + 1;

  // Read count and begin pointer for diagnostics.
  uint32_t count = sr::kernel::readGuestU32BE(thisVA + 16);
  uint32_t begin = sr::kernel::readGuestU32BE(thisVA + 12);

  // iter-24 diagnostic: log first 5 entries unconditionally to confirm
  // the strong override is engaged.
  if (n <= 5) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[DOGMA_WORKER_KILL] sub_8258C278 ENTRY #%u this=0x%08X count=0x%08X begin=0x%08X",
      n, thisVA, count, begin);
  }

  // If count is huge (image-corruption-derived) OR begin points into image
  // .rdata, zero the count to skip the loop. This converts a hang into a
  // no-op (matching iter-19's worker-kill philosophy).
  // iter-389 RETIREMENT (registry band-aid #3): the corrupt state this
  // guarded (garbage count/begin) came from the pre-real-heap scribble era;
  // zero kill-fires observed post-388. Default OFF; alarm if the condition
  // ever recurs. SR_WORKER_KILL_8258=1 restores the kill for diagnosis.
  static const bool s_killOn = [] {
    const char* e = std::getenv("SR_WORKER_KILL_8258");
    return e && e[0] == '1';
  }();
  bool kill = false;
  if (count > 0x10000) kill = true;  // sanity: real loops < 64K iters
  if (begin >= 0x82000000u && begin < 0x82A00000u) kill = true;  // begin in image
  if (kill && !s_killOn) {
    static std::atomic<unsigned> s_alarm{0};
    unsigned an = s_alarm.fetch_add(1, std::memory_order_relaxed) + 1;
    if (an <= 8)
      SR_LOG_BOOT(sr::LogSeverity::Error,
        "[DOGMA_WORKER_KILL_RAW] #%u corrupt loop state this=0x%08X count=0x%08X "
        "begin=0x%08X — retired kill would have zeroed it (left intact)",
        an, thisVA, count, begin);
    kill = false;
  }

  if (kill) {
    if (uint8_t* host = sr::kernel::guestVAToHost(thisVA + 16)) {
      host[0] = 0; host[1] = 0; host[2] = 0; host[3] = 0;
    }
    if (n <= 4 || (n & (n - 1)) == 0) {  // first 4 + powers of 2
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[DOGMA_WORKER_KILL] sub_8258C278 #%u this=0x%08X count=0x%08X begin=0x%08X "
        "— zeroed count to break runaway loop",
        n, thisVA, count, begin);
    }
  }

  __imp__sub_8258C278(ctx, base);
}

// iter-63: throttle the sub_826F4230 busy-spin on empty DOGMA dispatch queue.
//
// sub_826F4230 is a DOGMA dispatch function (caller_lr=0x826F4254):
//   r29 = r3+8  (CS handle)
//   r30 = r3+60 (work queue: begin=*(r30), end=*(r30+4))
//   If end==begin (empty): releases CS and returns 0.
//   Outer caller loops calling it again immediately → ~4M CS Enter/Leave/sec.
//
// Fix: when the work queue is empty, return 0 immediately (matching what
// the original does) and sleep 100µs after 100 consecutive empty calls.
// Non-empty queue: delegate to original for real dispatch.
// iter-365f: SR_SCHED_FAST_POLL (opt-in =1). Platform fix, not forcing: the
// guest expects its scheduler polls to cost ~nothing (console drains preload
// items back-to-back); our empty-poll sleeps rounded to the OS quantum and
// were the dominant per-item latency of the whole resource pipeline. Pair
// with SR_TIMER_RES_1MS=1. Proven: world-preload chain 3.5x faster and the
// FE-tick death becomes DETERMINISTIC at ~+50s (same self-deadlock every
// roll: preload executor's sync EFFECT request head-of-line-blocks the vxd
// load that would register the name). Default-OFF until that ordering wall
// is root-fixed, because firing the fatal request early on every roll closes
// the lucky-roll window today's deep-Apt menu builds escape through (iter-365
// malloc0 precedent). Use =1 to reproduce the deadlock deterministically.
static bool schedFastPollEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_SCHED_FAST_POLL");
    return e && e[0] == '1';
  }();
  return s_on;
}

void sub_826F4230(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<uint64_t> s_calls{0};
  static std::atomic<uint64_t> s_emptyRun{0};

  uint64_t n = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t thisVA = ctx.r3.u32;

  // Queue at r3+60 (begin) and r3+64 (end) — big-endian guest pointers.
  uint32_t qBegin = sr::kernel::readGuestU32BE(thisVA + 60);
  uint32_t qEnd   = sr::kernel::readGuestU32BE(thisVA + 64);
  bool empty = (qBegin == qEnd);

  if (empty) {
    uint64_t er = s_emptyRun.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 5 || (er & (er - 1)) == 0) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[826F4230_THROTTLE] #%llu this=0x%08X queue={0x%08X..0x%08X} emptyRun=%llu",
        (unsigned long long)n, thisVA, qBegin, qEnd, (unsigned long long)er);
    }
    // iter-213: sleep UNCONDITIONALLY on empty (the prior `er > 100` gate was
    // dead — s_emptyRun is shared across multiple scheduler objects so it reset
    // before 100, and the worker busy-spun at millions/sec). Empty queue ⇒ no
    // work ⇒ a 50µs sleep delays nothing real, and (since we short-circuit
    // BEFORE locking the CS) it frees the scheduler CS for the producer thread
    // to enqueue a load.
    //
    // iter-365f SR_SCHED_FAST_POLL (default ON, =0 reverts to the pre-365f
    // behavior): the unconditional 50µs sleep rounds up to the OS scheduler
    // quantum (15.6ms default), so during an active load every brief
    // queue-empty gap cost a full quantum → the world preload drained at
    // ~60 items/s and the FE drain-wait outlived any timeout. With
    // SR_TIMER_RES_1MS (main.cpp) the same sleep now costs ~1ms — the bulk of
    // the win. A short yield burst on top catches back-to-back completions.
    // The burst MUST stay small: an unbounded yield phase (first cut, 5000)
    // turned every poller into a core-burning spinner across K instances and
    // slowed the whole boot 10x (metropol open 30s→555s).
    // iter-463 charter (burndown U1): a TIMING band-aid. The r3=0 matches the
    // game's own empty-queue return (IDA 0x826F436C), but the shortcut skips
    // the guest's CsEnterWithDeadline/CsLeaveCounted pair and adds a host
    // sleep. It fired in 79/81 + 68/69 run logs, so this changes the default
    // exe's scheduling: NAMED master gate, rolled alone with
    //   SR_ALLOW_FORCING=1 SR_FORCING_ISOLATE=SR_SCHED_THROTTLE
    // Default exe: the guest's own ServiceNext (locks, finds nothing, returns 0).
    if (sr::kernel::forcingAllowedFor("SR_SCHED_THROTTLE")) {
      if (schedFastPollEnabled() && er < 64) {
        std::this_thread::yield();
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
      ctx.r3.s64 = 0;
      return;
    }
    __imp__sub_826F4230(ctx, base);
    return;
  }

  s_emptyRun.store(0, std::memory_order_relaxed);
  // iter-213: queue NON-empty but the worker re-dispatches at ~2M/s without
  // consuming → a STUCK load item. Log it (its first words: vtable→type,
  // and a peeked name) to identify which resource load never completes.
  {
    static std::atomic<uint64_t> s_busy{0};
    uint64_t b = s_busy.fetch_add(1, std::memory_order_relaxed) + 1;
    if (b <= 5 || (b % 500000 == 0)) {
      uint32_t w0 = sr::kernel::readGuestU32BE(qBegin);
      uint32_t w1 = sr::kernel::readGuestU32BE(qBegin + 4);
      uint32_t w2 = sr::kernel::readGuestU32BE(qBegin + 8);
      // peek a name string from the item or its first pointer
      char nm[40] = {0};
      for (uint32_t cand : {w0, w1, w2, qBegin}) {
        if (cand >= 0x10000u && cand < 0xC0000000u) {
          if (uint8_t* h = sr::kernel::guestVAToHost(cand)) {
            if (h[0] >= 32 && h[0] < 127) {
              int ok = 0; for (int i = 0; i < 39; ++i) { char c=(char)h[i]; if(!c) break; if(c<32||c>=127){ok=0;break;} nm[i]=c; ok++; }
              if (ok >= 3) break; else nm[0]=0;
            }
          }
        }
      }
      // iter-365e: the head item ptr is *qBegin (w0). Log its owner-gate fields + the
      // draining host thread so owner-token <-> drainer mismatches name themselves.
      const uint32_t headItem = w0;
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[RESSCHED_BUSY] #%llu this=0x%08X q={0x%08X..0x%08X} item=0x%08X w0=0x%08X w1=0x%08X w2=0x%08X name=\"%s\" "
        "headOwner44=%08X started4C=%u complete4F=%u drainThread=%u",
        (unsigned long long)b, thisVA, qBegin, qEnd, qBegin, w0, w1, w2, nm,
        (headItem >= 0x10000u) ? sr::kernel::readGuestU32BE(headItem + 0x44u) : 0,
        (headItem >= 0x10000u) ? ((sr::kernel::readGuestU32BE(headItem + 0x4Cu) >> 24) & 0xFFu) : 0,
        (headItem >= 0x10000u) ? (sr::kernel::readGuestU32BE(headItem + 0x4Cu) & 0xFFu) : 0,
        sr::kernel::getCurrentThreadHandle());
    }
  }
  __imp__sub_826F4230(ctx, base);
}

// iter-213: throttle sub_826F4378's empty-poll on queue B (this+72..76).
//
// The ResourceScheduler worker (DOGMA pool, thread startVA=0x827A7420) polls two
// dispatch queues: queue A via sub_826F4230 ([this+60..64], throttled by iter-63)
// and queue B via sub_826F4378 ([this+72..76], NEVER throttled). sub_826F4378
// LOCKS the scheduler CS (this+8) on every call, finds queue B empty, unlocks,
// returns 0; the worker re-polls at millions/sec, cycling the CS and STARVING
// the producer that needs the same CS to enqueue a load. This is the consistent
// half of the "FMUI hang" (iter-17) = RTAM asset-load stall livelock.
// (NB: sub_826F44C0 — the apparent hasWork() predicate — is NOT on the hot path;
//  a build-tested override of it was never invoked. The worker calls
//  sub_826F4230/4378 via an indirect path Ghidra's caller list missed.)
//
// Fix: peek queue B WITHOUT the lock; if empty, sleep 50µs and return 0,
// skipping the lock entirely so the producer can win the CS. Mirrors iter-63.
extern "C" void __imp__sub_826F4378(PPCContext& __restrict ctx, uint8_t* base);

void sub_826F4378(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<uint64_t> s_emptyRunB{0};
  const uint32_t thisVA = ctx.r3.u32;
  // iter-434: scheduler-state sampler (every 65536th call incl. empty polls).
  // In wedge rolls the FSM-driver thread spins the drain loop forever on an
  // in-flight item that never completes: sample slot+56 (in-flight item), the
  // two pending vectors, and the CS latches — a frozen slot names the item.
  {
    static std::atomic<uint32_t> s_smp{0};
    uint32_t n = s_smp.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & 0xFFFF) == 0 && thisVA >= 0x10000u) {
      uint32_t slot = sr::kernel::readGuestU32BE(thisVA + 56);
      uint32_t q1 = (sr::kernel::readGuestU32BE(thisVA + 76) -
                     sr::kernel::readGuestU32BE(thisVA + 72)) / 4;
      uint32_t q2 = (sr::kernel::readGuestU32BE(thisVA + 64) -
                     sr::kernel::readGuestU32BE(thisVA + 60)) / 4;
      uint32_t w0 = slot >= 0x10000u ? sr::kernel::readGuestU32BE(slot) : 0;
      uint32_t w4 = slot >= 0x10000u ? sr::kernel::readGuestU32BE(slot + 4) : 0;
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[RSCHED] svcB#%u sched=%08X slot56=%08X [w0=%08X w4=%08X] q1=%u q2=%u tid=%lu",
          n, thisVA, slot, w0, w4, q1, q2,
          (unsigned long)GetCurrentThreadId());
    }
  }
  if (thisVA >= 0x10000u) {
    uint32_t qB = sr::kernel::readGuestU32BE(thisVA + 72);  // queue B begin
    uint32_t qE = sr::kernel::readGuestU32BE(thisVA + 76);  // queue B end
    if (qB == qE) {  // empty — no work
      static std::atomic<uint64_t> s_n{0};
      uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 3 || (n % 500000 == 0)) {
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[826F4378_THROTTLE] #%llu this=0x%08X queueB={0x%08X..0x%08X} thread=%u",
          (unsigned long long)n, thisVA, qB, qE, sr::kernel::getCurrentThreadHandle());
      }
      // iter-365f SR_SCHED_FAST_POLL: same adaptive backoff as sub_826F4230
      // above (this call sits directly in the FE thread's drain-wait loop at
      // lr=826F473C, so an unconditional quantum-length sleep here throttled
      // FeMasterTick's entire synchronous preload drain to ~60 items/s).
      // Yield burst kept small — see the 4230 comment for the 5000-yield
      // core-burn lesson.
      uint64_t er = s_emptyRunB.fetch_add(1, std::memory_order_relaxed) + 1;
      // iter-463 charter (burndown U1): same TIMING band-aid as sub_826F4230
      // (guest returns 0 at IDA 0x826F43C8 after its own CS pair); named gate.
      if (sr::kernel::forcingAllowedFor("SR_SCHED_THROTTLE")) {
        if (schedFastPollEnabled() && er < 64) {
          std::this_thread::yield();
        } else {
          std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        ctx.r3.s64 = 0;
        return;
      }
    }
  }
  s_emptyRunB.store(0, std::memory_order_relaxed);
  __imp__sub_826F4378(ctx, base);
}

// iter-213: capture the stuck load item's isComplete() predicate.
//
// sub_826F4378's non-empty dispatch does: item = sub_826F4090(this); then
// ready = item->vtable[+120](item); if ready==0 the item is NOT consumed and is
// re-dispatched forever (the storm). So vtable[+120] = "isLoadComplete()" and it
// returns 0 eternally for the stuck item. This logs the item + its vtable slots
// so we can decode vtable[+120] and find what the load polls (suspected: an
// async NtReadFile completion Event/APC that never fires — see
// docs/OTHER_RECOMPS_LESSONS.md). sub_826F4090 returns the item in r3.
extern "C" void __imp__sub_826F4090(PPCContext& __restrict ctx, uint8_t* base);

void sub_826F4090(PPCContext& __restrict ctx, uint8_t* base) {
  // iter-365i [RESSCHED_REAP]: sub_826F4090's scan REMOVES items whose QI-state
  // is neither 0 (pending) nor 1 (ready) — a silent swap-with-last reap. Detect
  // by queue-size delta across the call and log what vanished (the reaped
  // item's identity is gone post-call, so log the pre-call head candidates).
  const uint32_t qDesc = ctx.r4.u32;
  uint32_t preB = 0, preE = 0, preN = 0;
  uint32_t preItems[6] = {0};
  if (qDesc >= 0x10000u) {
    preB = sr::kernel::readGuestU32BE(qDesc);
    preE = sr::kernel::readGuestU32BE(qDesc + 4);
    preN = (preE - preB) / 4u;
    for (uint32_t i = 0; i < preN && i < 6; ++i)
      preItems[i] = sr::kernel::readGuestU32BE(preB + 4u * i);
  }
  __imp__sub_826F4090(ctx, base);
  if (qDesc >= 0x10000u) {
    uint32_t postE = sr::kernel::readGuestU32BE(qDesc + 4);
    uint32_t postN = (postE - sr::kernel::readGuestU32BE(qDesc)) / 4u;
    if (postN < preN) {
      static std::atomic<unsigned> s_r{0};
      unsigned r = s_r.fetch_add(1, std::memory_order_relaxed) + 1;
      if (r <= 200 || (r % 64 == 0)) {
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[RESSCHED_REAP] #%u q=%08X %u->%u pre={%08X %08X %08X %08X %08X %08X} ret=%08X",
          r, qDesc, preN, postN, preItems[0], preItems[1], preItems[2],
          preItems[3], preItems[4], preItems[5], ctx.r3.u32);
      }
    }
  }
  const uint32_t item = ctx.r3.u32;
  if (item >= 0x10000u) {
    static std::atomic<uint64_t> s_n{0};
    uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 5 || (n % 500000 == 0)) {
      uint32_t vt = sr::kernel::readGuestU32BE(item);
      uint32_t s120 = (vt >= 0x10000u) ? sr::kernel::readGuestU32BE(vt + 120) : 0;
      uint32_t s124 = (vt >= 0x10000u) ? sr::kernel::readGuestU32BE(vt + 124) : 0;
      uint32_t s28  = (vt >= 0x10000u) ? sr::kernel::readGuestU32BE(vt + 28)  : 0;
      // peek a name from the item's fields
      char nm[40] = {0};
      for (int off = 4; off <= 48 && nm[0] == 0; off += 4) {
        uint32_t p = sr::kernel::readGuestU32BE(item + off);
        if (p >= 0x10000u && p < 0xC0000000u) {
          if (uint8_t* h = sr::kernel::guestVAToHost(p)) {
            if (h[0] >= 32 && h[0] < 127) {
              int ok = 0; for (int i = 0; i < 39; ++i){ char c=(char)h[i]; if(!c) break; if(c<32||c>=127){ok=0;break;} nm[i]=c; ok++; }
              if (ok < 3) nm[0] = 0;
            }
          }
        }
      }
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[RESSCHED_ITEM] #%llu item=0x%08X vt=0x%08X isComplete[+120]=0x%08X [+124]=0x%08X dtor[+28]=0x%08X name=\"%s\"",
        (unsigned long long)n, item, vt, s120, s124, s28, nm);
    }
  }
}

// iter-365i [RESTASK]: ResourceTask execute-dispatch = scheduler vt[+40] =
// sub_826F3BA8 (branches by item phase vt[+48] / mode vt[+20]). Task ctor
// sub_82704E88 is already instrumented below ([GAMECONFIG_FINAL]). Together
// with [RESSCHED_REAP] this times every stream-load task from birth to death —
// the open question is whether the death-burst stream tasks are created and
// then reaped-on-failure, or never created at all once the FE parks.
extern "C" void __imp__sub_826F3BA8(PPCContext& __restrict ctx, uint8_t* base);
void sub_826F3BA8(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t item = ctx.r4.u32;
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  // iter-365r: SR_RESTASK_ALL=1 logs EVERY exec with the item's NAME and its
  // before/after state. The default sampling hid the one exec that matters —
  // the item that ends up stuck in the scheduler's single in-flight slot
  // (+52) with complete[+0x4F]=0, which deadlocks every later dispatch.
  static const bool s_all = [] {
    const char* e = std::getenv("SR_RESTASK_ALL");
    return e && e[0] == '1';
  }();
  const bool logIt = s_all || (n <= 600 || (n % 64 == 0));
  // iter-365i item field map (decoded from the vt getter stubs @0x82704D28+):
  // +0x14 phase (vt+48), +0x48 mode (vt+20), +0x40 provider (vt+92 — its
  // vt[+24] predicate gates ProcessLoadName), +0x38 access (vt+84),
  // +0x0C inner (vt+32), +0x4C started, +0x4F complete, +0x44 owner.
  uint32_t phase = 0, mode = 0, prov = 0, provVt = 0, acc = 0;
  char name[40] = {0};
  if (logIt && item >= 0x10000u) {
    phase = sr::kernel::readGuestU32BE(item + 0x14);
    mode  = sr::kernel::readGuestU32BE(item + 0x48);
    prov  = sr::kernel::readGuestU32BE(item + 0x40);
    acc   = sr::kernel::readGuestU32BE(item + 0x38);
    if (prov >= 0x10000u) provVt = sr::kernel::readGuestU32BE(prov);
    // +0x18 = the request's NAME pointer (vt[56] getter = lwz r3,0x18(r3)).
    uint32_t np = sr::kernel::readGuestU32BE(item + 0x18);
    if (np >= 0x10000u) {
      for (int i = 0; i < 39; ++i) {
        uint8_t* c = sr::kernel::guestVAToHost(np + i);
        if (!c || !*c) break;
        name[i] = (char)*c;
      }
    }
  }
  // iter-385h: the task's RECORD (inner obj at item+0xC) completion word
  // (inner+12) pre-exec — the conveyor's ResMgrAcquire waits on exactly this
  // word (idiom cntlzw&0x20). Deep cycles must show it flip 0->nonzero
  // somewhere; the fatal shallow exec shows where the write went missing.
  uint32_t recInner = 0, cwPre = 0;
  if (item >= 0x10000u) {
    recInner = sr::kernel::readGuestU32BE(item + 0xC);
    if (recInner >= 0x10000u && recInner < 0xC0000000u)
      cwPre = sr::kernel::readGuestU32BE(recInner + 12);
  }
  __imp__sub_826F3BA8(ctx, base);
  if (logIt && item >= 0x10000u) {
    uint8_t complete = 0, started = 0, okFlag = 0;
    if (uint8_t* h = sr::kernel::guestVAToHost(item + 79)) complete = *h;
    if (uint8_t* h = sr::kernel::guestVAToHost(item + 0x4C)) started = *h;
    if (uint8_t* h = sr::kernel::guestVAToHost(item + 0x4D)) okFlag = *h;
    // The scheduler's single in-flight slot: if this stays non-zero with an
    // incomplete item, every later ServiceNext degenerates to a passive poll.
    uint32_t sched = sr::kernel::readGuestU32BE(0x829A2424);
    uint32_t slotA = (sched >= 0x10000u) ? sr::kernel::readGuestU32BE(sched + 52) : 0;
    uint32_t cwPost = 0;
    if (recInner >= 0x10000u && recInner < 0xC0000000u)
      cwPost = sr::kernel::readGuestU32BE(recInner + 12);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[RESTASK] exec #%u item=%08X '%s' phase=%u mode=%u prov=%08X provVt=%08X "
      "acc=%08X rc=%d cmpl=%u started=%u ok4D=%u slotA=%08X rec=%08X cw=%08X->%08X tid=%u",
      n, item, name, phase, mode, prov, provVt, acc, (int)ctx.r3.s32, complete,
      started, okFlag, slotA, recInner, cwPre, cwPost,
      sr::kernel::getCurrentThreadHandle());
  }
  // iter-365j SR_CONVEYOR_PACE=<µs> (opt-in): pace the cooperative preload
  // conveyor (one manifest entry per exec) without touching worker/I/O speed.
  // The race that kills the FE tick is conveyor-vs-stream-I/O: the manifest
  // walk's EFFECT entry sync-waits on names that in-flight stream loads
  // (requested by EARLIER entries of the same walk) would register on
  // completion. Console's conveyor is frame-paced while its I/O runs free;
  // our fast-poll build accelerates the conveyor MORE than the disk-bound
  // I/O and flips the race against us (why the slow baseline occasionally
  // deep-rolled and fast-poll never does). Pacing only the exec step gives
  // the streams back their head start.
  static const unsigned s_paceUs = [] {
    const char* e = std::getenv("SR_CONVEYOR_PACE");
    return e ? (unsigned)strtoul(e, nullptr, 10) : 0u;
  }();
  if (s_paceUs) std::this_thread::sleep_for(std::chrono::microseconds(s_paceUs));
}

// iter-213N: VALIDATED fix — force GAMECONFIG's isComplete (vtable[+120]=sub_82704DD8,
// = the byte [item+79]) to report complete. Under SR_ENABLE_RTAM the gameconfig
// ResourceScheduler item (vtable 0x8208B908, key 0x0EA70951) never sets [item+79]
// after its 1372-byte blob is read (the post-read finalize never re-runs — iter-213i),
// so FMGameConfig.init's request (resMgr vt[8]=sub_826FAA70 -> worker sub_826FAAC0)
// synchronously DRAINS the scheduler (queue-A drain sub_826F4230) waiting for the
// item to complete and spins ~47s until a host timeout, returning a GARBAGE handle
// (iter-213M: inner=0x8271F330). Forcing isComplete=1 makes the drain's consume
// condition (item->vtable[+120]()!=0) true -> it consumes the item via loc_826F4300
// and the request returns in ~46ms with a VALID "loading" handle (iter-213N run2:
// inner=0x89F62B20, flags=0x0000) — exactly the healthy DEFAULT-build behavior.
// This CORRECTS iter-213L's "force is ineffective" claim (that was iter-213j's
// queue-B Frida write on a frozen instance; the queue-A drain DOES consume on
// forced isComplete). SAFE: FMGameConfig.init (sub_8214AFB8) returns 1 unconditionally
// regardless of the load result, and the handle is the same loading-state the working
// default path hands back. Guarded to the GAMECONFIG vtable 0x8208B908 so other
// isComplete users are untouched.
// iter-357e DECONTAMINATION: this force previously ALSO auto-armed under
// SR_ENABLE_RTAM, silently coupling a forcing band-aid to what looked like a pure
// un-skip diagnostic (tainting e.g. iter-351's "SR_ENABLE_RTAM delivers 16/16"
// attribution). Now: SR_FORCE_GAMECONFIG=1 only, and only under the master
// forcing gate (charter: no forcing path may be ungated).
extern "C" void __imp__sub_82704DD8(PPCContext& __restrict ctx, uint8_t* base);
void sub_82704DD8(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_force = [] {
    const char* f = std::getenv("SR_FORCE_GAMECONFIG");
    return sr::kernel::forcingAllowed() && f && f[0] == '1';
  }();
  uint32_t item = ctx.r3.u32;
  // iter-365 FE-FREEZE PROBE 4 (log-only, ungated): the FE-tick thread freezes INSIDE
  // FeMasterTick's resMgr pump — [THREAD_SAMPLER] pins it at lr=826F4740 (sub_826F4700
  // cooperative drain) ctr=82704DD8 (THIS getter) for 80+s, every roll (deep rolls just
  // freeze later: census win=302 then 0). The iter-213 class on the natural path. Name the
  // item: one item polled 2M+ consecutive times -> dump its fields once + the polling tid.
  {
    // cumulative per-item polls (4-slot table): 200K polls (~20s of the observed ~10K/s
    // spin) => dump once per item. Consecutive-streak v1 never fired: polls interleave and
    // the whole-run total is only ~2M.
    static std::atomic<uint32_t> s_pi[4]{};
    static std::atomic<uint32_t> s_pc[4]{};
    for (int i = 0; i < 4; ++i) {
      uint32_t cur = s_pi[i].load(std::memory_order_relaxed);
      if (cur != item && cur != 0) continue;
      if (cur == 0 && !s_pi[i].compare_exchange_strong(cur, item, std::memory_order_relaxed)
          && cur != item)
        continue;
      uint32_t k = s_pc[i].fetch_add(1, std::memory_order_relaxed) + 1;
      if (k == 200000u) {
        char name[40] = {0};
        const uint32_t nameVA = sr::kernel::readGuestU32BE(item + 8u);
        if (nameVA >= 0x10000u && nameVA < 0xC0000000u) {
          const uint8_t* p = sr::kernel::guestVAToHost(nameVA);
          if (p) { for (size_t ci = 0; ci + 1 < sizeof(name) && p[ci]; ++ci)
                     name[ci] = (p[ci] >= 0x20 && p[ci] < 0x7F) ? char(p[ci]) : '.'; }
        }
        const uint8_t* b4c = sr::kernel::guestVAToHost(item + 0x4Cu);
        const uint8_t* b4f = sr::kernel::guestVAToHost(item + 0x4Fu);
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[ITEM_SPIN] item=%08X polled 200K+ times: name='%s' owner+44=%08X phase+14=%08X "
          "substate+48=%08X started+4C=%u isComplete+4F=%u vt=%08X",
          item, name,
          sr::kernel::readGuestU32BE(item + 0x44u),
          sr::kernel::readGuestU32BE(item + 0x14u),
          sr::kernel::readGuestU32BE(item + 0x48u),
          b4c ? *b4c : 0xFF, b4f ? *b4f : 0xFF,
          sr::kernel::readGuestU32BE(item));
      }
      break;
    }
  }
  __imp__sub_82704DD8(ctx, base);
  if (s_force && item >= 0x10000u && (ctx.r3.u32 & 0xFF) == 0 &&
      sr::kernel::readGuestU32BE(item) == 0x8208B908u) {
    static std::atomic<uint64_t> s{0};
    uint64_t n = s.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n % 1000000 == 0))
      SR_LOG_BOOT(sr::LogSeverity::Info, "[GAMECONFIG_FORCE] item=0x%08X forced isComplete=1 #%llu", item, (unsigned long long)n);
    ctx.r3.u64 = 1;
  }
}

// iter-357g PROBE (observability): the natural boot never reaches CreatePackage/
// OnPackageBound (zero [PKG] lines) even though FMGameRtam's kick sub_8218D128
// runs. Log the kick entry/exit and every package-loader creation
// (sub_82183C18(mem, ctx, namePtr, ...)) so one run shows how far the kick's
// chain gets and where it dies.
extern "C" void __imp__sub_8218D128(PPCContext& __restrict ctx, uint8_t* base);
void sub_8218D128(PPCContext& __restrict ctx, uint8_t* base) {
  SR_LOG_BOOT(sr::LogSeverity::Info, "[KICK] sub_8218D128 ENTER r3=%08X lr=%08X",
              ctx.r3.u32, (uint32_t)ctx.lr);
  __imp__sub_8218D128(ctx, base);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[KICK] sub_8218D128 EXIT");
}

extern "C" void __imp__sub_82183C18(PPCContext& __restrict ctx, uint8_t* base);
void sub_82183C18(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 40) {
    char nm[64] = {};
    for (uint32_t off = 0; off < 8; ++off) {
      uint32_t cand = (off == 0) ? ctx.r5.u32 : 0;
      (void)cand;
    }
    uint8_t* h = sr::kernel::guestVAToHost(ctx.r5.u32);
    if (h) {
      for (int i = 0; i < 63; ++i) {
        char c = (char)h[i];
        if (!c || c < 32 || c >= 127) break;
        nm[i] = c;
      }
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[KICK] loaderCreate #%u r3=%08X r4=%08X r5=%08X('%s') r6=%08X r7=%08X r8=%08X lr=%08X",
        n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, nm, ctx.r6.u32, ctx.r7.u32,
        ctx.r8.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_82183C18(ctx, base);
  if (n <= 40)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[KICK] loaderCreate #%u rc=%08X", n, ctx.r3.u32);
}

// iter-357g PROBE: the PSM (FMGamePlayStateMgr, *(0x8296EDAC)) is never
// constructed on the natural boot — its splash-name enqueue native
// (sub_822A8F88 -> FMV queue sub_8235F2D0) therefore can never run, so no
// intro movie is ever queued. The lazy-ctor Function_822AE160 SWALLOWS
// allocation failure (alloc rc 0 -> global stays 0, no log). Log every entry:
// allocator global, alloc result, final PSM value.
extern "C" void __imp__sub_822AE160(PPCContext& __restrict ctx, uint8_t* base);
void sub_822AE160(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t before = sr::kernel::readGuestU32BE(0x8296EDACu);
  uint32_t alloc8a4 = sr::kernel::readGuestU32BE(0x8296D8A4u);
  __imp__sub_822AE160(ctx, base);
  uint32_t after = sr::kernel::readGuestU32BE(0x8296EDACu);
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8 || before != after)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[PSM_CTOR] #%u Function_822AE160 psm before=%08X after=%08X allocator[8296D8A4]=%08X lr=%08X%s",
        n, before, after, alloc8a4, (uint32_t)ctx.lr,
        (after == 0) ? "  *** PSM STILL NULL (alloc failed or ctor bailed) ***" : "");
  // One dedicated wall-clock poller (fe_flow_watch's watcher never reaches its
  // read): golden-at-menu = counter -1, done byte 1 (set <30ms post-ctor).
  if (after && n == 1) {
    std::thread([] {
      for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        uint32_t psm = sr::kernel::readGuestU32BE(0x8296EDACu);
        if (!psm) continue;
        uint32_t counter = sr::kernel::readGuestU32BE(psm + 0x230u);
        uint32_t done = sr::kernel::readGuestU32BE(psm + 0x234u);
        uint32_t smPtr = sr::kernel::readGuestU32BE(0x829761E0u);
        uint32_t smState = smPtr ? sr::kernel::readGuestU32BE(smPtr + 4u) : 0xDEAD;
        static uint32_t s_last = 0xEE;
        uint32_t sig = counter ^ done ^ smState;
        if (sig != s_last || i % 10 == 0) {
          s_last = sig;
          SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PSM_POLL] t=%ds psm=%08X counter=%08X doneWord=%08X fmvState=%u",
              i * 3, psm, counter, done, smState);
        }
      }
    }).detach();
  }
}

// iter-357h PROBE: THE trunk (workflow-mapped): every FSM state's script is an
// 'FSMA' Lua asset whose chunk is compiled+ref'd ONLY by the FSMA type-processor
// sub_82191AE8 (dostring sub_82176EB8, counter DAT_8296dbf0); state-entry
// sub_82259630 pcalls the ref (pcall swallows errors; failed condition pcalls
// read TRUE -> our instant-pass walk + silent scripts, one cause). Log the
// processor, the dostring counter, and every state-entry script exec.
extern "C" void __imp__sub_82191AE8(PPCContext& __restrict ctx, uint8_t* base);
void sub_82191AE8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32;
  if (n <= 400)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FSMA] proc #%u ENTER r3=%08X r4=%08X lr=%08X dostrings=%u", n, r3, r4,
        (uint32_t)ctx.lr, sr::kernel::readGuestU32BE(0x8296DBF0u));
  __imp__sub_82191AE8(ctx, base);
  if (n <= 400)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FSMA] proc #%u EXIT rc=%08X dostrings=%u", n, ctx.r3.u32,
        sr::kernel::readGuestU32BE(0x8296DBF0u));
}

extern "C" void __imp__sub_82259630(PPCContext& __restrict ctx, uint8_t* base);
void sub_82259630(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t st = ctx.r3.u32;
  uint32_t scriptRef = st ? sr::kernel::readGuestU32BE(st + 0x10u) : 0;
  if (n <= 400)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FSMA] stateExec #%u state=%08X script(+10)=%08X r4=%08X lr=%08X tid=%lu",
        n, st, scriptRef, ctx.r4.u32, (uint32_t)ctx.lr,
        (unsigned long)GetCurrentThreadId());
  __imp__sub_82259630(ctx, base);
}

// iter-357i: dostring itself (sub_82176EB8: r3=script mgr, r4=source buffer,
// r5=length; loadbuffer + lua_pcall + store-ref, counter DAT_8296dbf0). Tag
// the chunks that carry the boot-critical calls (PlayFMVs / AreFMVsDone /
// LoadWorldFlow — the baseworl.AST @0x592f91 boot script) so we can see
// whether THE chunk that fires the movies is ever compiled on our walk.
extern "C" void __imp__sub_82176EB8(PPCContext& __restrict ctx, uint8_t* base);
void sub_82176EB8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t src = ctx.r4.u32, len = ctx.r5.u32;
  bool hasPlay = false, hasDone = false, hasLoadWF = false;
  if (src >= 0x1000u && src < 0xC0000000u && len > 0 && len < 0x100000u) {
    const char* p = reinterpret_cast<const char*>(base + src);
    std::string_view sv(p, len);
    hasPlay = sv.find("PlayFMVs") != std::string_view::npos;
    hasDone = sv.find("AreFMVsDone") != std::string_view::npos;
    hasLoadWF = sv.find("LoadWorldFlow") != std::string_view::npos;
  }
  bool interesting = hasPlay || hasDone || hasLoadWF;
  // iter-433: dump EVERY chunk's source (bounded 400 chunks, 220 chars) plus
  // the ref counter — the offline map from a pcall'd registry ref (the parked
  // FSM condition in [FEQ]) back to its Lua SOURCE text.
  if (n <= 400 || interesting) {
    char preview[221] = {0};
    if (src >= 0x1000u && src < 0xC0000000u && len > 0) {
      const char* p = reinterpret_cast<const char*>(base + src);
      uint32_t m = len < 220u ? len : 220u;
      for (uint32_t i = 0; i < m; ++i) {
        char c = p[i];
        preview[i] = (c >= 32 && c < 127) ? c : (c == '\n' || c == '\r') ? '|' : '.';
      }
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FSMA] dostring #%u len=%u lr=%08X cnt=%u play=%d done=%d loadWF=%d src=\"%s\"",
        n, len, (uint32_t)ctx.lr, sr::kernel::readGuestU32BE(0x8296DBF0u),
        (int)hasPlay, (int)hasDone, (int)hasLoadWF, preview);
  }
  __imp__sub_82176EB8(ctx, base);
  static std::atomic<unsigned> s_fail{0};
  bool failed = ctx.r3.u32 == 0;
  unsigned nf = failed ? s_fail.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
  if (interesting || n <= 40 || (failed && (nf <= 16 || (nf & (nf - 1)) == 0)))
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FSMA] dostring #%u EXIT rc=%08X%s fails=%u", n, ctx.r3.u32,
        interesting ? " (boot-critical chunk)" : "", nf ? nf : s_fail.load());
}

// iter-433: sub_82164D48(mod, namePtr, nameLen) = pushlstring(name) →
// gettable(LUA_GLOBALSINDEX) → luaL_ref(LUA_REGISTRYINDEX) → ref. This is the
// ONLY place FSMA scripts get their registry refs (called from the processor
// sub_82191AE8 right after dostring mangles "main" → 4-char name). Logging
// name+ref here, interleaved with the dostring source dump above, is the exact
// ref→source map for the parked flow-FSM condition ([FEQ] ref=0x12).
std::atomic<uint32_t> g_luaRegT{0};   // registry Table VA, latched at refName #1
std::atomic<uint32_t> g_slot18Clo{0}; // slot-18 closure VA, latched per regTrav
std::atomic<uint32_t> g_gcCollectN{0};   // collect sequence number
std::atomic<uint32_t> g_lastSweepList{0}; // list-head arg of the current sweeplist

extern "C" void __imp__sub_82164D48(PPCContext& __restrict ctx, uint8_t* base);
void sub_82164D48(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  char name[17] = {0};
  uint32_t np = ctx.r4.u32, nl = ctx.r5.u32;
  if (np >= 0x1000u && np < 0xC0000000u && nl > 0) {
    const char* p = reinterpret_cast<const char*>(base + np);
    uint32_t m = nl < 16u ? nl : 16u;
    for (uint32_t i = 0; i < m; ++i)
      name[i] = (p[i] >= 32 && p[i] < 127) ? p[i] : '.';
  }
  uint32_t lr = (uint32_t)ctx.lr;
  uint32_t mod = ctx.r3.u32;
  __imp__sub_82164D48(ctx, base);
  if (n == 1) {
    // one-shot: publish this module's registry Table VA (Lua 5.0: L=mod+12,
    // G=L+16, registry TObject value at G+48) so luaH_setnum logs can be
    // filtered to REGISTRY writes offline.
    uint32_t L = sr::kernel::readGuestU32BE(mod + 12);
    uint32_t G = L ? sr::kernel::readGuestU32BE(L + 16) : 0;
    uint32_t regT = G ? sr::kernel::readGuestU32BE(G + 48) : 0;
    g_luaRegT.store(regT, std::memory_order_relaxed);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FSMA] regTable mod=%08X L=%08X G=%08X regT=%08X", mod, L, G, regT);
  }
  if (n <= 4000 || ctx.r3.u32 == 0x12)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FSMA] refName #%u \"%s\" -> ref=%08X mod=%08X lr=%08X", n, name,
        ctx.r3.u32, mod, lr);
}

// iter-433b: luaL_unref (sub_827617D8: L, tIdx, ref — freelist head at t[1],
// t[ref]=oldHead, t[1]=ref). THE ASSASSIN TRAP. Oracle (golden dump): registry
// slot 18 still holds the 0-arg AreLoadUnitsBusy condition (aaap) AT THE MENU —
// on console this binding survives. Our run: slot 18 gets unref'd mid-boot and
// recycled to a 1-param SuperLaunch mode script, so the flow FSM's parked
// 1->13 condition pcalls the wrong function (mode=nil -> returns nil -> false
// forever) and the menu never comes. Log every unref's lr to name the caller.
extern "C" void __imp__sub_827617D8(PPCContext& __restrict ctx, uint8_t* base);
void sub_827617D8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t ref = ctx.r5.u32;
  if (n <= 4000 || ref == 0x12)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FSMA] unref #%u ref=%08X t=%d L=%08X lr=%08X%s", n, ref,
        (int32_t)ctx.r4.u32, ctx.r3.u32, (uint32_t)ctx.lr,
        ref == 0x12 ? " *** SLOT-18 (flow condition) ***" : "");
  __imp__sub_827617D8(ctx, base);
}

// iter-433c: lua_rawseti (sub_8275D158: L, tIdx, n — pops TOS into t[n]).
// The iter-433b trap proved slot 18 gets REBOUND with ZERO luaL_unref calls on
// it — the writer bypasses the ref API. rawseti is the next choke point down
// (luaL_ref itself stores through here; so does any direct registry[n]=v).
// Log every rawseti with n==18 plus its lr; also first 64 registry-band writes
// (tIdx==-10000) for context.
extern "C" void __imp__sub_8275D158(PPCContext& __restrict ctx, uint8_t* base);
void sub_8275D158(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_reg{0};
  uint32_t n = ctx.r5.u32;
  int32_t tIdx = (int32_t)ctx.r4.u32;
  bool isReg = tIdx == -10000;
  unsigned rn = isReg ? s_reg.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
  if (n == 18 || (isReg && rn <= 64))
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FSMA] rawseti n=%u t=%d L=%08X lr=%08X%s", n, tIdx, ctx.r3.u32,
        (uint32_t)ctx.lr, n == 18 ? " *** SLOT-18 WRITE ***" : "");
  __imp__sub_8275D158(ctx, base);
}

// iter-433d: LoadUnitsAnyBusy (sub_82199BF0: mgr -> busy byte, OR over every
// unit's 8 stage-deque counts). iter-433c proved slot 18 can stay HEALTHY all
// run (1 write, 0 unrefs) and the flow STILL parks — so either the queues are
// genuinely busy at poll time (Frida walks mistimed) or the boolean dies
// between pcall and predicate. Log the computed verdict at the source.
extern "C" void __imp__sub_82199BF0(PPCContext& __restrict ctx, uint8_t* base);
void sub_82199BF0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t mgr = ctx.r3.u32, lr = (uint32_t)ctx.lr;
  __imp__sub_82199BF0(ctx, base);
  if (n <= 32 || (n & 63) == 0 || (ctx.r3.u32 == 0 && n <= 4096))
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[LOADU] busy#%u mgr=%08X -> %u lr=%08X", n, mgr, ctx.r3.u32, lr);
}

// iter-433f: traversetable (sub_82763728: GCState*, Table*). The GC mark chain
// audits faithful in the recomp (root-marker, reallymarkobject, propagate,
// array loop all correct) yet registry-anchored closures get SWEPT. The weak
// decision writes mode bits into t->marked (weakkey=0x2, weakvalue=0x4, mask
// 0xF9 clear first). If the REGISTRY ever classifies weak-valued, its array is
// skipped at mark and every registry-only closure dies — exactly the observed
// slot-16..20 kill band. Log big-array tables' classification per traverse.
extern "C" void __imp__sub_82763728(PPCContext& __restrict ctx, uint8_t* base);
void sub_82763728(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t tbl = ctx.r4.u32;
  const bool isReg = tbl && tbl == g_luaRegT.load(std::memory_order_relaxed);
  uint32_t sizeArr = 0, clo = 0, tt = 0;
  uint8_t cloMarkPre = 0;
  if (isReg) {
    sizeArr = sr::kernel::readGuestU32BE(tbl + 28);
    uint32_t arr = sr::kernel::readGuestU32BE(tbl + 12);
    if (arr && sizeArr >= 18) {
      tt = sr::kernel::readGuestU32BE(arr + 8 * 17);
      clo = sr::kernel::readGuestU32BE(arr + 8 * 17 + 4);
      if (tt == 6 && clo >= 0x10000) {
        cloMarkPre = *reinterpret_cast<const uint8_t*>(base + clo + 5);
        g_slot18Clo.store(clo, std::memory_order_relaxed);
      }
    }
  }
  __imp__sub_82763728(ctx, base);
  if (isReg) {
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    uint8_t cloMarkPost = (tt == 6 && clo >= 0x10000)
        ? *reinterpret_cast<const uint8_t*>(base + clo + 5) : 0xFF;
    uint8_t tMark = *reinterpret_cast<const uint8_t*>(base + tbl + 5);
    // The interesting event: registry traversed but slot 18's closure came out
    // UNMARKED (bit0 clear) — the mark walk skipped it and the next sweep will
    // free it. Log those always; otherwise sample.
    bool unmarked = (tt == 6 && clo >= 0x10000) && (cloMarkPost & 0x1) == 0;
    if (n <= 40 || (n & 31) == 0 || unmarked)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[LGC] regTrav#%u collect#%u szA=%u tMark=%02X s18[tt=%u clo=%08X mark %02X->%02X]%s",
          n, g_gcCollectN.load(std::memory_order_relaxed), sizeArr, tMark, tt,
          clo, cloMarkPre, cloMarkPost,
          unmarked ? " *** SLOT-18 LEFT UNMARKED -> next sweep kills it ***" : "");
  }
}

// iter-433g: the kill-shot instruments. Run1 proved slot 18's closure is
// MARKED in every registry traverse all run yet still gets freed — the free
// comes from a path outside the logged collects. Trap the free itself
// (luaF_freeclosure sub_82767598: L, closure -> luaM_realloc(sz,0)) on the
// latched slot-18 closure, and any sweeplist (sub_82764080: L, listHead*,
// deadmask) with deadmask>0 (the lua_close-style kill-everything sweep).
extern "C" void __imp__sub_827645D8(PPCContext& __restrict ctx, uint8_t* base);
void sub_827645D8(PPCContext& __restrict ctx, uint8_t* base) {
  unsigned n = g_gcCollectN.fetch_add(1, std::memory_order_relaxed) + 1;
  // iter-433h: regTrav# lags collect# by 9 before the kill and 10 after — the
  // kill collect SKIPS the registry traverse. Root-marker skips any root whose
  // mark bit is already set; if the registry's bit survived the previous
  // sweep un-cleared, this collect never grays it and every registry-only
  // object dies. Log the registry table's mark byte at collect entry.
  uint32_t regT = g_luaRegT.load(std::memory_order_relaxed);
  if (regT) {
    uint8_t m = *reinterpret_cast<const uint8_t*>(base + regT + 5);
    if ((m & 0x11) != 0) {
      static std::atomic<unsigned> s_k{0};
      unsigned k = s_k.fetch_add(1, std::memory_order_relaxed) + 1;
      if (k <= 32)
        SR_LOG_BOOT(sr::LogSeverity::Error,
            "[LGC] *** collect#%u ENTERS with regT mark=%02X (STALE -> root "
            "skip, registry not traversed this cycle) ***", n, m);
    }
  }
  __imp__sub_827645D8(ctx, base);
  // iter-433h: the collect that EXITS with the registry's bit still set is
  // the one whose sweep failed to visit it (survivor-clear miss) — the cycle
  // BEFORE the kill. Name it.
  if (regT) {
    uint8_t m2 = *reinterpret_cast<const uint8_t*>(base + regT + 5);
    if ((m2 & 0x01) != 0) {
      static std::atomic<unsigned> s_x{0};
      unsigned x = s_x.fetch_add(1, std::memory_order_relaxed) + 1;
      if (x <= 32)
        SR_LOG_BOOT(sr::LogSeverity::Error,
            "[LGC] *** collect#%u EXITS with regT mark=%02X (sweep did not "
            "clear the registry table's bit) ***", n, m2);
    }
  }
}

// iter-433i: phase probes inside the collect. Order in luaC_collectgarbage
// (sub_827645D8): markall -> sweeplist(G+16) -> sub_82764188 -> sweeplist
// (G+12 rootgc) -> sub_827641F8 -> sub_82764348. Log the registry's mark bit
// at each phase boundary for collects 680-688 — pinpoints which phase fails
// to clear (or re-sets) it in the deterministic #684 anomaly.
static void logRegBitAtPhase(const char* phase, uint8_t* base) {
  unsigned n = g_gcCollectN.load(std::memory_order_relaxed);
  if (n < 680 || n > 688) return;
  uint32_t regT = g_luaRegT.load(std::memory_order_relaxed);
  if (!regT) return;
  uint8_t m = *reinterpret_cast<const uint8_t*>(base + regT + 5);
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[LGC] collect#%u phase %s: regT mark=%02X", n, phase, m);
}

extern "C" void __imp__sub_82764188(PPCContext& __restrict ctx, uint8_t* base);
void sub_82764188(PPCContext& __restrict ctx, uint8_t* base) {
  logRegBitAtPhase("post-sweep1(G+16) / enter-64188", base);
  __imp__sub_82764188(ctx, base);
}

extern "C" void __imp__sub_827641F8(PPCContext& __restrict ctx, uint8_t* base);
void sub_827641F8(PPCContext& __restrict ctx, uint8_t* base) {
  logRegBitAtPhase("post-sweep2(G+12 rootgc) / enter-641F8", base);
  __imp__sub_827641F8(ctx, base);
}

extern "C" void __imp__sub_82764348(PPCContext& __restrict ctx, uint8_t* base);
void sub_82764348(PPCContext& __restrict ctx, uint8_t* base) {
  logRegBitAtPhase("post-641F8 / enter-64348", base);
  __imp__sub_82764348(ctx, base);
}

extern "C" void __imp__sub_82767598(PPCContext& __restrict ctx, uint8_t* base);
void sub_82767598(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t clo = ctx.r4.u32;
  if (clo && clo == g_slot18Clo.load(std::memory_order_relaxed)) {
    uint8_t mark = *reinterpret_cast<const uint8_t*>(base + clo + 5);
    SR_LOG_BOOT(sr::LogSeverity::Error,
        "[LGC] *** SLOT-18 CLOSURE FREED *** clo=%08X marked=%02X lr=%08X "
        "collect#%u sweepList=%08X", clo, mark, (uint32_t)ctx.lr,
        g_gcCollectN.load(std::memory_order_relaxed),
        g_lastSweepList.load(std::memory_order_relaxed));
  }
  __imp__sub_82767598(ctx, base);
}

extern "C" void __imp__sub_82764080(PPCContext& __restrict ctx, uint8_t* base);
void sub_82764080(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t mask = ctx.r5.u32;
  g_lastSweepList.store(ctx.r4.u32, std::memory_order_relaxed);
  if ((int32_t)mask > 0) {
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 64)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[LGC] sweeplist DEADMASK=%d #%u list=%08X lr=%08X", (int32_t)mask,
          n, ctx.r4.u32, (uint32_t)ctx.lr);
  }
  // iter-433i: walk the chain OURSELVES before the sweep for collects 683-685
  // — count nodes, find the registry table and the slot-18 closure positions,
  // detect truncation. Names how #684's rootgc walk misses the registry.
  unsigned cn = g_gcCollectN.load(std::memory_order_relaxed);
  if (cn >= 683 && cn <= 685) {
    uint32_t regT = g_luaRegT.load(std::memory_order_relaxed);
    uint32_t clo18 = g_slot18Clo.load(std::memory_order_relaxed);
    uint32_t head = ctx.r4.u32;
    uint32_t node = (head >= 0x10000u) ? sr::kernel::readGuestU32BE(head) : 0;
    uint32_t count = 0; int regPos = -1, cloPos = -1;
    uint32_t lastNode = 0;
    while (node >= 0x10000u && node < 0xC0000000u && count < 400000u) {
      if (node == regT) regPos = (int)count;
      if (node == clo18) cloPos = (int)count;
      lastNode = node;
      node = sr::kernel::readGuestU32BE(node);
      ++count;
    }
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[LGC] collect#%u PRE-SWEEP walk list=%08X nodes=%u regPos=%d cloPos=%d "
        "endNode=%08X endNext=%08X", cn, head, count, regPos, cloPos, lastNode,
        node);
  }
  __imp__sub_82764080(ctx, base);
}

// iter-434: guest thread-create (sub_827A74B0: handleOut, attr, jobFn, jobArg,
// a5, a6 -> sub_828A7768 with start routine sub_827A7420 which runs jobFn(arg)
// ONCE and self-terminates). The conveyor/FE "wedge" = its JOB FN returning
// early. Map every created thread to its job fn so exits name their run-loop.
extern "C" void __imp__sub_827A74B0(PPCContext& __restrict ctx, uint8_t* base);
void sub_827A74B0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t fn = ctx.r5.u32, arg = ctx.r6.u32, lr = (uint32_t)ctx.lr;
  // iter-434b: jobFn=sub_825FF140 is a one-shot wrapper running the INNER fn
  // *(argBlk+16)(*(argBlk+20)) — the real run-loop. Log it so the conveyor's
  // loop names itself.
  uint32_t inner = 0, innerArg = 0;
  if (arg >= 0x10000u) {
    inner = sr::kernel::readGuestU32BE(arg + 16);
    innerArg = sr::kernel::readGuestU32BE(arg + 20);
  }
  __imp__sub_827A74B0(ctx, base);
  if (n <= 512)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[THCREATE] #%u jobFn=%08X arg=%08X inner=%08X innerArg=%08X rc=%u lr=%08X",
        n, fn, arg, inner, innerArg, ctx.r3.u32, lr);
}

// iter-434c: THE SCRIBBLER — sub_824E8978 = the WMV codec's draw_edges
// (border-extension) block func (installed at codecCtx[3948] by
// sub_8253CDE0). hw-watch proved its VMX stores hit the GameKernel loop
// manager's running byte (0x89668622), killing loop runner 50 => the wedge.
// Log the call args + computed write extents; flag any call whose extent
// touches the manager band 0x89668000..0x89669000.
extern "C" void __imp__sub_824E8978(PPCContext& __restrict ctx, uint8_t* base);
void sub_824E8978(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32;
  uint32_t r7 = ctx.r7.u32, r8 = ctx.r8.u32, r9 = ctx.r9.u32, r10 = ctx.r10.u32;
  // stack args a28 (pitch) ride the caller frame; log the low regs + r1.
  bool nearMgr = false;
  // crude extent guess: base r3 +/- 0x2000 window vs the manager band
  if ((r3 >= 0x89660000u && r3 <= 0x89672000u) ||
      (r3 + r4 >= 0x89660000u && r3 + r4 <= 0x89672000u))
    nearMgr = true;
  if (n <= 64 || nearMgr)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[DRAWEDGE] #%u r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X "
        "r10=%08X r1=%08X lr=%08X%s",
        n, r3, r4, r5, r6, r7, r8, r9, r10, ctx.r1.u32, (uint32_t)ctx.lr,
        nearMgr ? " *** NEAR MANAGER BAND ***" : "");
  __imp__sub_824E8978(ctx, base);
}

// iter-433c: luaH_setnum (sub_827685F8: L, Table*, n -> TObject* slot). The
// Table-level numeric setter — EVERY write to a numbered slot funnels through
// here (rawseti, settable, internal). Log all n==18 writes with the table ptr
// (filter offline against the [FSMA] regTable line) — the slot-18 clobberer
// cannot dodge this one.
extern "C" void __imp__sub_827685F8(PPCContext& __restrict ctx, uint8_t* base);
void sub_827685F8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n18{0};
  if (ctx.r5.u32 == 18) {
    unsigned k = s_n18.fetch_add(1, std::memory_order_relaxed) + 1;
    if (k <= 512)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[FSMA] H_setnum#%u t=%08X L=%08X lr=%08X", k, ctx.r4.u32,
          ctx.r3.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_827685F8(ctx, base);
}

// iter-357j: the movie-open frontier. FMV SM parks at state 1 (request queued,
// EA_HD_logo at psm+0x18) and nobody ever asks for the asset. FeMasterTick's
// UI-manager tick sub_82151298(dt, uiMgr) runs ONLY when DAT_8295c838 != 0
// (golden: 1) and the decoder pump Function_821F93F8 no-ops when mode
// *(DAT_8296df4c+8) >= 4. Log both.
// (dt rides in f1 for both — the object pointer is r3, NOT r4.)
#endif

extern "C" void __imp__sub_82151298(PPCContext& __restrict ctx, uint8_t* base);
void sub_82151298(PPCContext& __restrict ctx, uint8_t* base) {
#if SR_ORACLE
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 6 || n % 2000 == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[UITICK] #%u uiMgr=%08X lr=%08X", n,
                ctx.r3.u32, (uint32_t)ctx.lr);
#endif
  __imp__sub_82151298(ctx, base);
  // iter-466 charswitch (PORT FEATURE, charter 2a): a CHEATS row's cheat runs here, after the UI
  // tick where the game's own matcher runs, through the matcher's own condition and effect calls
  // (kernel/cheats_menu.hpp). Returns at once when port features are off.
  sr::kernel::cheatsMenuOnUiTick(ctx, base);
}
#if SR_ORACLE


extern "C" void __imp__sub_821F93F8(PPCContext& __restrict ctx, uint8_t* base);
void sub_821F93F8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  static uint32_t s_prevMode = 0xEE;
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t dec = ctx.r3.u32;
  uint32_t mode = dec ? sr::kernel::readGuestU32BE(dec + 8u) : 0xFF;
  uint32_t arr = dec ? sr::kernel::readGuestU32BE(dec + 4u) : 0;
  uint32_t handler =
      (mode < 4u && arr) ? sr::kernel::readGuestU32BE(arr + mode * 4u) : 0;
  if (n <= 6 || mode != s_prevMode || n % 2000 == 0) {
    s_prevMode = mode;
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FMVDEC] #%u dec=%08X mode=%u handler=%08X ui838=%02X", n,
        dec, mode, handler,
        (unsigned)*(sr::kernel::guestVAToHost(0x8295C838u)));
  }
  __imp__sub_821F93F8(ctx, base);
}

// iter-357j GATE-1 probes: the 'XenonRender' .rs render-script payload stays
// NULL (readiness sub_82776EB0 fails -> fallback pass walk -> no fmvmovie
// pass). sub_82780C50 = the .rs TEXT PARSER (silent-failure suspect),
// sub_826E4E90 = the payload REGISTRATION it should call on success.
extern "C" void __imp__sub_82780C50(PPCContext& __restrict ctx, uint8_t* base);
void sub_82780C50(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  __imp__sub_82780C50(ctx, base);
  if (n <= 16 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[RS_PARSE] #%u r3=%08X r4=%08X r5=%08X -> rc=%08X lr(caller)=n/a", n,
        r3, r4, r5, ctx.r3.u32);
}

// iter-387 upgrade: this is THE record-completion setter (Dr0-caught writer
// of rec inner+12 = the constructed-asset ptr the conveyor polls; guts
// sub_826E5D08 under the global record CS 829A246C). ~55 per-asset-type
// loader callers. Unconditional [RECSET] with record NAME + caller lr: on a
// shallow roll the fatal record's call is ABSENT and the healthy X2-serving
// lr names the ONE caller whose guard fails. (Old sampled [RS_REG] retired.)
extern "C" void __imp__sub_826E4E90(PPCContext& __restrict ctx, uint8_t* base);
void sub_826E4E90(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t handle = ctx.r3.u32, asset = ctx.r4.u32;
  uint32_t inner = 0;
  char nm[40] = {0};
  if (handle >= 0x10000u && handle < 0xC0000000u) {
    inner = sr::kernel::readGuestU32BE(handle);
    if (inner >= 0x10000u && inner < 0xC0000000u) {
      uint32_t np = sr::kernel::readGuestU32BE(inner);
      if (np >= 0x10000u && np < 0xC0000000u)
        for (int i = 0; i < 39; ++i) {
          uint8_t* c = sr::kernel::guestVAToHost(np + i);
          if (!c || !*c || *c < 32 || *c >= 127) break;
          nm[i] = (char)*c;
        }
    }
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[RECSET] #%u rec=%08X inner=%08X asset=%08X name='%s' tid=%u lr=%08X",
      n, handle, inner, asset, nm,
      sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
  __imp__sub_826E4E90(ctx, base);
}

// iter-357l: XMV create internals. sub_82470F28 (create-from-desc) runs case 2
// (file source): allocates source objects (FUN_82476d40 + Function_824780A8),
// opens each via vt[+0x60], then builds the player in sub_82470A58. Our create
// returns E_FAIL (0x80004005 — NOT the E_OUTOFMEMORY 0x8007000E path), so the
// failure is an open or the player build. Bracket them.
extern "C" void __imp__sub_82470A58(PPCContext& __restrict ctx, uint8_t* base);
void sub_82470A58(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t a = ctx.r3.u32, b = ctx.r4.u32, c = ctx.r5.u32, d = ctx.r6.u32;
  __imp__sub_82470A58(ctx, base);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMV_BUILD] sub_82470A58(dev=%08X desc=%08X srcA=%08X srcB=%08X) -> rc=%08X",
      a, b, c, d, ctx.r3.u32);
}

// iter-357l: the player build's sub-object ctors. Log each object's vtable and
// the slots the build then calls, so the E_FAIL step can be named without
// guessing (the build calls: video +0x2c/+0x44/+0x60, audio +0x2c/+0x44/+0x5c,
// linker +0x24/+0x38, player +0x24).
namespace {
void logXmvObj(const char* tag, unsigned n, PPCContext& ctx) {
  uint32_t o = ctx.r3.u32;
  if (!o) { SR_LOG_BOOT(sr::LogSeverity::Warn, "[XMV_OBJ] %s #%u -> NULL", tag, n); return; }
  uint32_t vt = sr::kernel::readGuestU32BE(o);
  auto slot = [&](uint32_t off) {
    return vt ? sr::kernel::readGuestU32BE(vt + off) : 0;
  };
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[XMV_OBJ] %s #%u obj=%08X vt=%08X | +24=%08X +2c=%08X +38=%08X "
      "+44=%08X +5c=%08X +60=%08X",
      tag, n, o, vt, slot(0x24), slot(0x2c), slot(0x38), slot(0x44),
      slot(0x5c), slot(0x60));
}
}  // namespace

#define SR_XMV_CTOR_PROBE(fn, tag)                                            \
  extern "C" void __imp__##fn(PPCContext& __restrict ctx, uint8_t* base);     \
  void fn(PPCContext& __restrict ctx, uint8_t* base) {                        \
    static std::atomic<unsigned> s_n{0};                                      \
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;             \
    __imp__##fn(ctx, base);                                                   \
    if (n <= 4) logXmvObj(tag, n, ctx);                                       \
  }

SR_XMV_CTOR_PROBE(sub_82476720, "player(0x128)")
SR_XMV_CTOR_PROBE(sub_824760F8, "linker(0x158)")
SR_XMV_CTOR_PROBE(sub_82475878, "video(0x2a8)")
SR_XMV_CTOR_PROBE(sub_82472640, "vdec(0x5d0)")
SR_XMV_CTOR_PROBE(sub_82474460, "audio(0x228)")
SR_XMV_CTOR_PROBE(sub_82472E70, "adec(0x178)")
#undef SR_XMV_CTOR_PROBE

// iter-357l round 2: every remaining step of the build, by real address
// (resolved from the round-1 vtable dump). Order in sub_82470A58:
//   video+0x2c=82474AC0, audio+0x2c=82473868, [824726B0 ok], [82472068 ok],
//   linker+0x24=824759F8, player+0x24=82480598, video+0x44=82474CB8,
//   audio+0x44=824743A0, video+0x60=8247BD50, audio+0x5c=82478550,
//   linker+0x38=82475D28. The first negative rc is the culprit.
#define SR_XMV_STEP_PROBE(fn, tag)                                            \
  extern "C" void __imp__##fn(PPCContext& __restrict ctx, uint8_t* base);     \
  void fn(PPCContext& __restrict ctx, uint8_t* base) {                        \
    static std::atomic<unsigned> s_n{0};                                      \
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;             \
    uint32_t o = ctx.r3.u32, p4 = ctx.r4.u32;                                 \
    __imp__##fn(ctx, base);                                                   \
    if (n <= 4)                                                               \
      SR_LOG_BOOT(sr::LogSeverity::Warn,                                      \
          "[XMV_STEP] %s(this=%08X r4=%08X) -> rc=%08X%s", tag, o, p4,        \
          ctx.r3.u32, ((int32_t)ctx.r3.u32 < 0) ? "  <<<< FAILS" : "");       \
  }

SR_XMV_STEP_PROBE(sub_82474AC0, "video+2c/init")
SR_XMV_STEP_PROBE(sub_82473868, "audio+2c/init")
SR_XMV_STEP_PROBE(sub_824759F8, "linker+24/attach")
SR_XMV_STEP_PROBE(sub_82480598, "player+24/init")
SR_XMV_STEP_PROBE(sub_82474CB8, "video+44/setsrc")
SR_XMV_STEP_PROBE(sub_824743A0, "audio+44/setsrc")
SR_XMV_STEP_PROBE(sub_8247BD50, "video+60/getfmt")
SR_XMV_STEP_PROBE(sub_82478550, "audio+5c/getfmt")
SR_XMV_STEP_PROBE(sub_82475D28, "linker+38/finalize")
#undef SR_XMV_STEP_PROBE

// iter-357l round 3: inside video+0x44/setsrc (sub_82474CB8) — the E_FAIL step.
// Its rc comes from one of: Function_8247C590(&vdecSlot) [create],
// FUN_8247c620(vdec,0) [reset], sub_8247C650(vdec, ..., streamMask) [open].
#define SR_XMV_STEP_PROBE(fn, tag)                                            \
  extern "C" void __imp__##fn(PPCContext& __restrict ctx, uint8_t* base);     \
  void fn(PPCContext& __restrict ctx, uint8_t* base) {                        \
    static std::atomic<unsigned> s_n{0};                                      \
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;             \
    uint32_t o = ctx.r3.u32, p4 = ctx.r4.u32, p5 = ctx.r5.u32;                \
    __imp__##fn(ctx, base);                                                   \
    if (n <= 4)                                                               \
      SR_LOG_BOOT(sr::LogSeverity::Warn,                                      \
          "[XMV_DEC] %s(r3=%08X r4=%08X r5=%08X) -> rc=%08X%s", tag, o, p4,   \
          p5, ctx.r3.u32, ((int32_t)ctx.r3.u32 < 0 || ctx.r3.u32) ? "  <<<<" : "");\
  }

SR_XMV_STEP_PROBE(sub_8247C590, "8247C590/create")
SR_XMV_STEP_PROBE(sub_8247C620, "8247c620/reset")
SR_XMV_STEP_PROBE(sub_8247C650, "8247C650/open")
#undef SR_XMV_STEP_PROBE

// iter-360d: audio+44/setsrc (sub_824743A0) fails 0x8000FFFF. PROVEN NOT the
// allocator (ALLOC_NULL: no audio-size NULL returns; heap not exhausted). The
// 0x8000FFFF is synthesized when Function_8247ACB8(param_1) returns 0/NULL. Its
// static trace says it should succeed (all pool allocs non-NULL, both
// sub_82483260 return 0) — so either a codegen divergence or a vtable side
// effect. Probe the actual returns of the whole create chain: Function_82482A40
// (0x7f8 obj, hr), sub_8247A460 (init, hr), Function_8247ACB8 (decoder ptr, 0=fail).
#define SR_XMV_ACREATE_PROBE(fn, tag, zerofail)                                \
  extern "C" void __imp__##fn(PPCContext& __restrict ctx, uint8_t* base);      \
  void fn(PPCContext& __restrict ctx, uint8_t* base) {                         \
    static std::atomic<unsigned> s_n{0};                                       \
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;              \
    uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32;               \
    __imp__##fn(ctx, base);                                                    \
    uint32_t rc = ctx.r3.u32;                                                  \
    bool bad = (zerofail) ? (rc == 0) : ((int32_t)rc < 0);                     \
    if (n <= 6)                                                                \
      SR_LOG_BOOT(sr::LogSeverity::Warn,                                       \
          "[XMV_ACREATE] %s(r3=%08X r4=%08X r5=%08X) -> %08X%s", tag, a3, a4,  \
          a5, rc, bad ? "  <<<< FAILS" : "");                                  \
  }
SR_XMV_ACREATE_PROBE(sub_82482A40, "82482A40/alloc0x7f8", 0)
SR_XMV_ACREATE_PROBE(sub_8247A460, "8247A460/init", 0)
SR_XMV_ACREATE_PROBE(sub_8247ACB8, "8247ACB8/create", 1)
SR_XMV_ACREATE_PROBE(sub_82483260, "82483260/regcb", 0)
#undef SR_XMV_ACREATE_PROBE

// iter-360d round 2: the 0x8000FFFF actually comes from sub_82473F60 (called by
// sub_824743A0 AFTER the decoder is created non-NULL). It has FOUR sequential
// gates, each -> 0x8000FFFF if its call returns non-zero (0=success):
//   sub_8247AD38(dec,9,bits,-1,0)  [configure]
//   sub_824798F8(dec,&out)          [query]
//   sub_8247AF80(dec,src,&out)      [bind source]
//   sub_8247B258(dec,&scratch0x3f8) [start]
// Probe all four: the first non-zero return is the real audio blocker.
#define SR_XMV_G4_PROBE(fn, tag)                                               \
  extern "C" void __imp__##fn(PPCContext& __restrict ctx, uint8_t* base);      \
  void fn(PPCContext& __restrict ctx, uint8_t* base) {                         \
    static std::atomic<unsigned> s_n{0};                                       \
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;              \
    uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32;\
    __imp__##fn(ctx, base);                                                    \
    uint32_t rc = ctx.r3.u32;                                                  \
    if (n <= 6)                                                                \
      SR_LOG_BOOT(sr::LogSeverity::Warn,                                       \
          "[XMV_G4] %s(r3=%08X r4=%08X r5=%08X r6=%08X) -> %08X%s", tag, a3,   \
          a4, a5, a6, rc, (rc != 0) ? "  <<<< NONZERO=FAIL" : "");             \
  }
SR_XMV_G4_PROBE(sub_8247AD38, "8247AD38/configure")
SR_XMV_G4_PROBE(sub_824798F8, "824798F8/query")
SR_XMV_G4_PROBE(sub_8247AF80, "8247AF80/bindsrc")
#undef SR_XMV_G4_PROBE

// iter-362j: dedicated hook for the channel-config "start" gate (was in G4). Dumps,
// per decoder-core, the CHANNEL COUNT the format claims vs how many slots are filled.
// core v6 = *(dstate=a1); chCount = *(int16)(*(v6+4)+36); slotArr = *(*(v6+4)+124).
// If chCount==2 but only slot[1] is filled => stream enumeration/populate delivered
// too few slots (the real bug). If chCount==1 but the loop queries idx=2 => a
// channel-map codegen bug after all. a2 = the u16-pairs the caller passed.
extern "C" void __imp__sub_8247B258(PPCContext& __restrict ctx, uint8_t* base);
void sub_8247B258(PPCContext& __restrict ctx, uint8_t* base) {
  // iter-403: entry breadcrumb. This is the rung immediately after
  // [XMV_G4] bindsrc, which is the last line the creating thread emits before
  // the terminal XMVCREATE goes silent - and like every probe in this family it
  // logged only on return, so a hang here is indistinguishable from never being
  // called. ENTER with no EXIT identifies it positively.
  {
    static std::atomic<unsigned> s_e{0};
    unsigned e = s_e.fetch_add(1, std::memory_order_relaxed) + 1;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[XMV_START] ENTER #%u dstate=%08X a2=%08X lr=%08X tid=%lu",
        e, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr, GetCurrentThreadId());
  }
  auto r32 = [](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };
  auto r16 = [](uint32_t a) { return a ? (uint16_t)(sr::kernel::readGuestU32BE(a) >> 16) : 0; };
  uint32_t dstate = ctx.r3.u32, a2 = ctx.r4.u32;
  uint32_t core = r32(dstate), cfg = core ? r32(core + 4) : 0;
  uint32_t chCount = cfg ? (int16_t)r16(cfg + 36) : 0;
  uint32_t slotArr = cfg ? r32(cfg + 124) : 0;
  __imp__sub_8247B258(ctx, base);
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMV_START] #%u sub_8247B258(dstate=%08X a2=%08X) -> rc=%08X%s | core=%08X cfg=%08X "
      "chCount=%d slotArr=%08X | a2pairs=%08X %08X %08X | slot[0..4]=%08X %08X %08X %08X %08X",
      n, dstate, a2, ctx.r3.u32, (ctx.r3.u32 != 0) ? "  <<<< FAIL" : "", core, cfg,
      (int)chCount, slotArr, r32(a2), r32(a2 + 4), r32(a2 + 8),
      r32(slotArr), r32(slotArr + 4), r32(slotArr + 8), r32(slotArr + 12), r32(slotArr + 16));
}

// iter-362j: the two channel-COUPLING calls inside sub_8247B258. Both take
// (core, chA, chB, flag) and read the channels' TYPE fields from the secondary slot
// array a1[142] (core+0x238) via sub_82483400. sub_8247A9F0 (merge) has many
// WMA validity returns (-2142240608 etc.) that map to 0x1D. Log core, the two
// channel indices, and the return; flag negatives. This names the exact failing
// coupling + the channel pair, so we can read their slot-type fields next.
#define SR_XMV_COUPLE_PROBE(fn, tag)                                           \
  extern "C" void __imp__##fn(PPCContext& __restrict ctx, uint8_t* base);      \
  void fn(PPCContext& __restrict ctx, uint8_t* base) {                         \
    uint32_t core = ctx.r3.u32, chA = ctx.r4.u32, chB = ctx.r5.u32, fl = ctx.r6.u32;\
    auto r32 = [](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };\
    uint32_t arr2 = r32(core + 0x238);                                         \
    __imp__##fn(ctx, base);                                                    \
    static std::atomic<unsigned> s_n{0};                                       \
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;              \
    bool bad = (int32_t)ctx.r3.u32 < 0;                                        \
    if (bad || n <= 6)                                                         \
      SR_LOG_BOOT(sr::LogSeverity::Warn,                                       \
        "[XMV_COUPLE] #%u %s(core=%08X chA=%u chB=%u fl=%u) -> %08X%s | arr2(core+238)=%08X "\
        "slotA=%08X slotB=%08X", n, tag, core, chA, chB, fl, ctx.r3.u32,       \
        bad ? "  <<<< NEG" : "", arr2,                                         \
        r32(arr2 + ((4 * chA) & 0x3FC)), r32(arr2 + ((4 * chB) & 0x3FC)));     \
  }
SR_XMV_COUPLE_PROBE(sub_8247A910, "8247A910/coupleAdd")
SR_XMV_COUPLE_PROBE(sub_8247A9F0, "8247A9F0/coupleMerge")
#undef SR_XMV_COUPLE_PROBE

// iter-362j: coupleAdd -> sub_8247A650 (couple-work) returns E_FAIL 0x80004005. Its
// a3=0 path calls ~7 sub-ops; batch-probe each to name the first that returns <0.
// Log-only, negatives + first few, capped. Shared fns -> filter on negative rc.
#define SR_XMV_WORK_PROBE(fn, tag)                                             \
  extern "C" void __imp__##fn(PPCContext& __restrict ctx, uint8_t* base);      \
  void fn(PPCContext& __restrict ctx, uint8_t* base) {                         \
    uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32;               \
    __imp__##fn(ctx, base);                                                    \
    static std::atomic<unsigned> s_n{0}, s_bad{0};                            \
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;             \
    bool bad = (int32_t)ctx.r3.u32 < 0;                                        \
    if ((bad && s_bad.fetch_add(1, std::memory_order_relaxed) < 10) || n <= 2)\
      SR_LOG_BOOT(sr::LogSeverity::Warn,                                       \
        "[XMV_WORK] #%u %s(r3=%08X r4=%08X r5=%08X) -> %08X%s", n, tag, a3, a4,\
        a5, ctx.r3.u32, bad ? "  <<<< NEG" : "");                             \
  }
SR_XMV_WORK_PROBE(sub_82483318, "82483318/slotAlloc")
SR_XMV_WORK_PROBE(sub_82484538, "82484538/alloc152")
SR_XMV_WORK_PROBE(sub_82484A10, "82484A10/dspSetup")
SR_XMV_WORK_PROBE(sub_82482AF0, "82482AF0/regCb")
SR_XMV_WORK_PROBE(sub_82479FD8, "82479FD8/prep")
SR_XMV_WORK_PROBE(sub_82482CB8, "82482CB8/regFmt")
// iter-362j: sub_82484A10 (dspSetup) internal calls — the E_FAIL leaf is here or in
// its two vtable calls. sub_8249EA48(streamSlot, ctx, codecStruct) = codec-init from
// the stream format; sub_8249E9C0 = a later init. If both return 0, the vtable call
// (*codecStruct[0])() is the origin (drill via *v19[0]).
SR_XMV_WORK_PROBE(sub_8249E9C0, "8249E9C0/init2")
#undef SR_XMV_WORK_PROBE

// iter-362j: codecInit sub_8249EA48(streamSlot, ctx, codecStruct) succeeds (a factory
// claims the format). The E_FAIL is the NEXT step in dspSetup: (*(*codecStruct))() —
// the codec's own vtable method-0. Capture that leaf's address = *(*a3) so we can
// decompile exactly where the WMA codec rejects the stream. a3 = r5 = codecStruct.
extern "C" void __imp__sub_8249EA48(PPCContext& __restrict ctx, uint8_t* base);
void sub_8249EA48(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t a1 = ctx.r3.u32, a2 = ctx.r4.u32, a3 = ctx.r5.u32;
  __imp__sub_8249EA48(ctx, base);
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 4) {
    auto r32 = [](uint32_t x) { return x ? sr::kernel::readGuestU32BE(x) : 0u; };
    uint32_t vt = r32(a3), method0 = r32(vt);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMV_CODEC] #%u sub_8249EA48(slot=%08X ctx=%08X struct=%08X) -> rc=%08X | "
      "struct[0]=vt=%08X method0=%08X | struct[1..3]=%08X %08X %08X",
      n, a1, a2, a3, ctx.r3.u32, vt, method0, r32(a3 + 4), r32(a3 + 8), r32(a3 + 12));
  }
}

// iter-360d round 3: sub_8247AD38 (WMFDecodeX86 registration) returns 1, which
// FUN_82479b00 maps ONLY from internal hr 0x8050000C ("decoder not found").
// The internal WMF-decoder-registry calls that can produce it:
//   sub_82485060(ctx, dec, ...)     [decoder factory]
//   sub_82479A30(9)                 [type-9 codec create; 0 -> 0x805000ba]
//   sub_82482E08(reg, 1, "WMFDecodeX86")  [register by NAME]
//   sub_82482AB0(reg, N, 82479408, dec)   [register format handler N=1/2/3/5]
//   sub_82478650(dec, ...)          [finalize]
// Probe all to find the 0x8050000C source (all return hr; <0 = fail).
#define SR_XMV_G5_PROBE(fn, tag)                                               \
  extern "C" void __imp__##fn(PPCContext& __restrict ctx, uint8_t* base);      \
  void fn(PPCContext& __restrict ctx, uint8_t* base) {                         \
    static std::atomic<unsigned> s_n{0};                                       \
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;              \
    uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32;               \
    __imp__##fn(ctx, base);                                                    \
    uint32_t rc = ctx.r3.u32;                                                  \
    if (n <= 10)                                                               \
      SR_LOG_BOOT(sr::LogSeverity::Warn,                                       \
          "[XMV_G5] %s(r3=%08X r4=%08X r5=%08X) -> %08X%s", tag, a3, a4, a5,   \
          rc, ((int32_t)rc < 0) ? "  <<<< HR<0" : "");                         \
  }
SR_XMV_G5_PROBE(sub_82485060, "82485060/factory")
SR_XMV_G5_PROBE(sub_82479A30, "82479A30/codec9")
SR_XMV_G5_PROBE(sub_82482E08, "82482E08/reg-name")
SR_XMV_G5_PROBE(sub_82482AB0, "82482AB0/reg-fmt")
SR_XMV_G5_PROBE(sub_82483180, "82483180/fmthandlers")
#undef SR_XMV_G5_PROBE

// iter-360e: the ACTUAL 0x8050000C source (per audio-decoder recon workflow) is
// sub_82498270, the ASF/WMA header parser. It rejects the audio header at
// LAB_82498494: fails if consumed != header_size, or fmt+0x2a != 1, or
// fmt+0x42 != 1, or fmt+0x2c != fmt+0x24 (fmt = piVar5[1], parse state
// piVar5 = *(obj+0x1c)); GUID check Function_82498018 is earlier (state 0/1).
// This probe dumps those fields on failure to pick: all-zero = H1 (header buffer
// corrupted upstream, same allocator/boundary-tag class as the video parser),
// garbage = H2 (wrong source ptr), valid-but-rejected = H3 (recomp field-read).
extern "C" void __imp__sub_82498270(PPCContext& __restrict ctx, uint8_t* base);
void sub_82498270(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t p1 = ctx.r3.u32;
  __imp__sub_82498270(ctx, base);
  uint32_t rc = ctx.r3.u32;
  if (n <= 6) {
    auto r32 = [](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };
    auto r16 = [](uint32_t a) { return a ? (uint16_t)(sr::kernel::readGuestU32BE(a) >> 16) : 0; };
    uint32_t st = r32(p1 + 0x1c);   // piVar5 (parse state)
    uint32_t fmt = r32(st + 4);     // piVar5[1] (format descriptor / header base)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[ASF_HDR] #%u sub_82498270(obj=%08X) -> rc=%08X | st=%08X fmt=%08X state=%d | "
      "consumed=%08X hdrsz=%08X | f2a=%04X f42=%04X f2c=%04X f24=%04X | "
      "hdr16=%08X %08X %08X %08X",
      n, p1, rc, st, fmt, (int)r32(st + 0x50), r32(st + 8), r32(fmt + 4),
      r16(fmt + 0x2a), r16(fmt + 0x42), r16(fmt + 0x2c), r16(fmt + 0x24),
      r32(fmt), r32(fmt + 4), r32(fmt + 8), r32(fmt + 0xc));
  }
}

// iter-360g: the audio wall reframed (recon+verify) — object #0 parses fine but
// sub_82498190 (sub-object #1) reads its SIZE field garbage (< 0x18) via the WMA
// reader, so the whole header is rejected. consumed=0 is WEAK (only advances on a
// refill branch), NOT proof of empty. Probe sub_82498190: dump the size it read
// (*param_3), rc, and the reader (*(obj+0x1c)) window words to see if the reader
// delivered valid bytes for object #1 (timing-lag/demux) or garbage (reader bug).
extern "C" void __imp__sub_82498190(PPCContext& __restrict ctx, uint8_t* base);
void sub_82498190(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t p1 = ctx.r3.u32, p3 = ctx.r5.u32;
  __imp__sub_82498190(ctx, base);
  uint32_t rc = ctx.r3.u32;
  if (n <= 10) {
    auto r32 = [](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };
    uint32_t pst = r32(p1 + 0x1c);      // parse-state
    uint32_t rdr = r32(pst);            // reader = *piVar1
    // *param_3 and consumed are u64 big-endian: low word is at +4.
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[ASF190] #%u sub_82498190(obj=%08X) -> rc=%08X | subobj_size(hi/lo)=%08X/%08X "
      "consumed(hi/lo)=%08X/%08X | pst=%08X reader=%08X | pst[+0x50 state]=%d fmt=%08X "
      "hdrsz=%08X | rdr[+8]=%08X rdr[+c]=%08X rdr[+10]=%08X rdr[+14]=%08X rdr[+18]=%08X",
      n, p1, rc, r32(p3), r32(p3 + 4), r32(pst + 8), r32(pst + 0xc), pst, rdr,
      (int)r32(pst + 0x50), r32(pst + 4), r32(r32(pst + 4) + 4),
      r32(rdr + 8), r32(rdr + 0xc), r32(rdr + 0x10), r32(rdr + 0x14), r32(rdr + 0x18));
  }
}

// iter-362 (FMV arc): the audio ASF header parse dies at consumed ~0xE2 reading
// subobj_size=0 — a SHORT delivery of a header the live-Xenia oracle proved is
// real+complete. sub_8249D498(a1, a2, a3=dataptr*, a4=avail*, a5=pos*) is the
// reader+0x10 block-cache refill (windowed pull). Capture what each pull delivers:
// requested count, returned avail, the delivered dataptr + its first bytes. The
// decisive read: at the failing pull, avail==0 => the block covering 0xE2+ is
// genuinely absent (async I/O / demux timing gap); avail>0 with zero bytes =>
// a read-zero/window bug. This picks the fix class without hand-deriving the cache.
extern "C" void __imp__sub_8249D498(PPCContext& __restrict ctx, uint8_t* base);
void sub_8249D498(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  uint32_t a1 = ctx.r3.u32, a2 = ctx.r4.u32;
  uint32_t pA3 = ctx.r5.u32, pA4 = ctx.r6.u32, pA5 = ctx.r7.u32;
  __imp__sub_8249D498(ctx, base);
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 40) {
    auto r32 = [](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };
    uint32_t dataptr = r32(pA3), avail = r32(pA4), posLo = r32(pA5 + 4);
    uint32_t d0 = dataptr ? r32(dataptr) : 0, d1 = dataptr ? r32(dataptr + 4) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[ASF_RDR] #%u refill(src=%08X req=%08X) rc=%08X | dataptr=%08X avail=%08X pos=%08X | data[0..7]=%08X %08X",
      n, a1, a2, ctx.r3.u32, dataptr, avail, posLo, d0, d1);
  }
}

// iter-362 (FMV arc): split corrupt-descriptor from bad-arithmetic. get-block
// sub_8249CD30(a1, a2=pos, a3=block*, a4=found*) returns the cache block covering
// pos. Log any call whose returned block's descriptor base (*(*block)+0) is an
// INVALID guest pointer (< 0x10000) — that is the garbage source the refill turns
// into dataptr=0x116. If the descriptor base is already garbage HERE => the block
// was never loaded / its descriptor is corrupt (block-load bug, upstream of the
// refill). If the base is valid here but the refill still yields 0x116 => the
// miscompile is in sub_8249D498's window math (then L6-difftest it).
extern "C" void __imp__sub_8249CD30(PPCContext& __restrict ctx, uint8_t* base);
void sub_8249CD30(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t a1 = ctx.r3.u32, pos = ctx.r4.u32, pA4 = ctx.r6.u32;
  __imp__sub_8249CD30(ctx, base);
  auto r32 = [](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };
  uint32_t found = r32(pA4);
  static std::atomic<unsigned> s_n{0}, s_fail{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  // On a NOT-FOUND in the header region (the audio failure at ~0xCA) enumerate the
  // whole block list: each block's START (desc+8 qword, both halves) + LEN (desc+4).
  // get-block covers [start, start+len); the qword START is the field I never logged.
  // If a block's start_hi != 0 -> a 64-bit zero-extend bug fed block_start; if a
  // clean block covers pos but found=0 -> get-block/list bug; if NO block covers pos
  // -> the block for 0xCA+ was never loaded (evict/load ordering). a1 isolates reader.
  bool logIt = (n <= 4);
  if (!found && pos < 0x400u && s_fail.fetch_add(1, std::memory_order_relaxed) < 16) logIt = true;
  if (logIt) {
    char blocks[280]; int bo = 0; blocks[0] = 0;
    uint32_t node = r32(r32(r32(a1 + 0x2c) + 0x10) + 8);  // list head
    for (int g = 0; g < 6 && node && bo < 240; g++) {
      uint32_t desc = r32(node);
      bo += snprintf(blocks + bo, sizeof(blocks) - bo, "[s=%08X:%08X len=%08X]",
                     r32(desc + 8), r32(desc + 12), r32(desc + 4));
      node = r32(node + 8);
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[ASF_BLK] #%u a1=%08X getblock(pos=%08X) found=%u | blocks: %s",
      n, a1, pos, found, blocks[0] ? blocks : "(none)");
  }
}

// iter-362c: identify the audio DEMUX SOURCE. ensure-N-bytes sub_8249D490(p1=
// reader-src, p2=N) loops loading from SRC=*(bc+0x34) (bc=*(p1+0x2c)) via the fn
// ptr at SRC+4, until readpos(bc+0x28)+N <= available. It short-delivers at ~0xCA.
// Capture SRC + its load fn (SRC+4) + the read-position/counters so we can name
// the source's stream and decompile its load to find why it stops. bc+0x74/+0x78
// = the per-load advance/cursor the loop bumps.
extern "C" void __imp__sub_8249D490(PPCContext& __restrict ctx, uint8_t* base);
void sub_8249D490(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t p1 = ctx.r3.u32, p2 = ctx.r4.u32;
  __imp__sub_8249D490(ctx, base);
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 24) {
    auto r32 = [](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };
    uint32_t bc = r32(p1 + 0x2c), src = r32(bc + 0x34);
    uint32_t srcvt = src ? r32(src) : 0, loadfn = src ? r32(src + 4) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[ASF_SRC] #%u ensure(rdr=%08X N=%08X) | bc=%08X src=%08X src[0]=%08X loadfn(src+4)=%08X | "
      "readpos=%08X bc74=%08X bc78=%08X",
      n, p1, p2, bc, src, srcvt, loadfn, r32(bc + 0x28 + 4), r32(bc + 0x74), r32(bc + 0x78));
  }
}

// iter-362d: IDA cleared sub_824741A8 (buffered reader, faithful, returns
// min(reqlen, buffered)). The short comes from the UNDERLYING stream read it
// calls: stream=*(a1+0x34), read method = *(*(stream)+0x3C), called with
// (stream, buf, 0x10000, &v18) returning v18 bytes. Hook sub_824741A8 to name
// that stream + its read fn, and log the buffered-valid (a1+0x218) + return so
// we see it's a genuine short read one level down (the demux/source), not this fn.
extern "C" void __imp__sub_824741A8(PPCContext& __restrict ctx, uint8_t* base);
void sub_824741A8(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t a1 = ctx.r3.u32, reqpos = ctx.r4.u32, reqlen = ctx.r5.u32;
  __imp__sub_824741A8(ctx, base);
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  // Catch the actual SHORT read (ret < reqlen) wherever it happens (the audio
  // reader's fill is later than the first-24 video/container reads), plus a few
  // samples for contrast.
  const bool shortRead = (ctx.r3.u32 < reqlen) && reqlen != 0;
  if (shortRead || n <= 6) {
    auto r32 = [](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };
    uint32_t strm = r32(a1 + 0x34), vt = strm ? r32(strm) : 0;
    uint32_t readfn = vt ? r32(vt + 0x3C) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[ASF_UND] #%u%s sub_824741A8(a1=%08X reqpos=%08X reqlen=%08X) ret=%08X | "
      "bufvalid(a1+218)=%08X bufpos(a1+210)=%08X | stream=%08X vt=%08X readfn(vt+3C)=%08X",
      n, shortRead ? " SHORT" : "", a1, reqpos, reqlen, ctx.r3.u32,
      r32(a1 + 0x218), r32(a1 + 0x210 + 4), strm, vt, readfn);
  }
}

// iter-362h: catch the descriptor FREE. The arena ptr (0x82976AA8) in the clobbered
// descriptor is a FREE-list link. sub_82482FE8(a1,a2,a3=&block) frees *a3 via
// sub_82167F98. Prime suspect: block-cache eviction sub_8249C940 frees the ACTIVE
// block mid-parse (UAF). Log each block free in the FMV heap range + the caller LR
// (names who frees it) — if the freed desc matches a live audio descriptor we saw
// in [ASF_BLK], that's the premature free to fix.
#if SR_ORACLE
namespace sr::kernel {
bool tryObserveAsfFree(uint32_t a3, uint32_t& block, uint32_t& desc, uint32_t& dbase) {
  auto read = [](uint32_t va, uint32_t& value) {
    if (!va || uint64_t(va) + 4u > getGuestSize()) return false;
    const uint8_t* source = guestVAToHost(va);
    if (!hostRangeReadable(source, 4)) return false;
    uint8_t bytes[4];
    SIZE_T copied = 0;
    // A diagnostic never owns the pointed-to object's lifetime. RPM also
    // fails safely if a free/protection change races the readability check.
    if (!ReadProcessMemory(GetCurrentProcess(), source, bytes, sizeof(bytes), &copied) || copied != sizeof(bytes))
      return false;
    value = uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
    return true;
  };
  // Filter before following the historical descriptor layout: other frees
  // need not have this layout at all (the stage-4 FMV teardown crash).
  return read(a3, block) && block >= 0x92000000u && block < 0x96000000u &&
         read(block, desc) && read(desc, dbase);
}
}
#endif
extern "C" void __imp__sub_82482FE8(PPCContext& __restrict ctx, uint8_t* base);
void sub_82482FE8(PPCContext& __restrict ctx, uint8_t* base) {
#if SR_ORACLE
  uint32_t block = 0, desc = 0, dbase = 0;
  const bool observed = sr::kernel::tryObserveAsfFree(ctx.r5.u32, block, desc, dbase);
  const uint32_t lr = ctx.lr;
#endif
  __imp__sub_82482FE8(ctx, base);
#if SR_ORACLE
  static std::atomic<unsigned> s_n{0};
  if (observed && s_n.fetch_add(1, std::memory_order_relaxed) < 60)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[ASF_FREE] free block=%08X desc=%08X base=%08X | caller lr=%08X", block, desc, dbase, lr);
#endif
}

// iter-362j: NEW FRONTIER after the descriptor-clobber fix. The ASF header now
// parses (ASF_HDR rc=0); XMVCREATE still fails at the 4th decoder gate
// sub_8247B258 (WMA multichannel channel-coupling "start"), which returns 0x1D =
// sub_82479B00's generic unknown-neg-hr code. The neg hr is -2142240746 from
// sub_82483400(arr,idx,&out) when the queried stream slot is ZERO: it reads
// *(arr + ((4*idx)&0x3FC)), arr = *(*(dec+4)+124). Probe both to decide REAL zero
// slot (genuine unpopulated channel) vs a codegen bug in sub_8247B258's __ROL4__
// bit-indexed v33/v32 channel map (which picks WHICH idx to query).
//   (sub_8247B258's return is already logged by the [XMV_G4] start probe.)
//   sub_82483400: on the failing (return<0) query, log the idx + computed offset +
//     slot value + a dump of the first 16 array slots so we can see whether the
//     stream-slot array was populated at all (all-zero = ch-map/populate upstream;
//     populated-but-this-idx-zero = sub_8247B258 picked a bad idx = codegen suspect).
extern "C" void __imp__sub_82483400(PPCContext& __restrict ctx, uint8_t* base);
void sub_82483400(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t arr = ctx.r3.u32, idx = ctx.r4.u32;
  __imp__sub_82483400(ctx, base);
  static std::atomic<unsigned> s_n{0}, s_fail{0};
  bool bad = (int32_t)ctx.r3.u32 < 0;
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((bad && s_fail.fetch_add(1, std::memory_order_relaxed) < 12) || n <= 4) {
    auto r32 = [](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };
    uint32_t off = (4 * idx) & 0x3FC;
    char slots[200]; int so = 0; slots[0] = 0;
    for (int i = 0; i < 16 && so < 170; i++)
      so += snprintf(slots + so, sizeof(slots) - so, "%08X ", r32(arr + 4 * i));
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMV_SLOT] #%u%s sub_82483400(arr=%08X idx=%u off=%03X) -> rc=%08X | slot=%08X | arr[0..15]=%s",
      n, bad ? " ZERO-SLOT" : "", arr, idx, off, ctx.r3.u32, r32(arr + off), slots);
  }
}

// iter-362k: the ACTUAL failing leaf's input. sub_824B2E78 (audio codec method-0)
// returns E_FAIL. It reads the stream FORMAT from *(a2+8) and checks the fourcc at
// *(*(a2+8)+20) against a WMA list. Log that fourcc + the WAVEFORMATEX head (wFormatTag
// at +0, channels +4, samplerate +8) + the return. WMA fourcc/tag => codec should work
// (E_FAIL = codegen bug); 'WMV3'/garbage => audio decoder fed the VIDEO format (mixup).
namespace sr { namespace kernel { bool restoreFmvExtradataIfCorrupt(uint32_t, uint32_t); } }
extern "C" void __imp__sub_824B2E78(PPCContext& __restrict ctx, uint8_t* base);
void sub_824B2E78(PPCContext& __restrict ctx, uint8_t* base) {
  auto r32 = [](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };
  auto r16 = [](uint32_t a) { return a ? (uint16_t)(sr::kernel::readGuestU32BE(a) >> 16) : 0; };
  uint32_t a1 = ctx.r3.u32, a2 = ctx.r4.u32;
  uint32_t fmt = r32(a2 + 8);
  uint32_t fourcc = r32(fmt + 20);
  uint16_t wTag = r16(fmt), wCh = r16(fmt + 2);
  uint32_t wRate = r32(fmt + 4);
  // iter-362n: the SR_XMV_EXTRA_RESTORE forcing (restoring fmt+52's STRUCT_C pre-init) is DELETED.
  // It proved the fmt+52 clobber is distinct from the codec+84 clobber (restoring fmt+52 alone left
  // codec+84 zeroed -> rc still 6); the natural fix (SR_FMV_TINY_ALLOC_FIX) protects BOTH 4-byte
  // allocs so preExData & cxData are 4FF10A01 naturally. [XMV_AFMT] logging kept as diagnostic.
  // iter-362n CLOBBER-LOCATOR: capture fmt+52's STRUCT_C bytes at init ENTRY (before the
  // decoder copies them into its own core at codec+84). Compare the ENTRY value against the
  // codec's copy AFTER init: entry!=0 && copy==0 => the copy (v23+84, a small dlmalloc alloc)
  // is clobbered DURING init (by sub_824BF348's adjacent allocs); entry==0 => fmt+0x60 was
  // already clobbered pre-init. Pins WHICH buffer to protect for the natural allocator fix.
  uint32_t preExPtr = fmt ? r32(fmt + 52) : 0;
  uint32_t preExData = preExPtr ? r32(preExPtr) : 0;
  __imp__sub_824B2E78(ctx, base);
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 6) {
    char fc[5] = {char(fourcc >> 24), char(fourcc >> 16), char(fourcc >> 8), char(fourcc), 0};
    for (int i = 0; i < 4; i++) if (fc[i] < 0x20 || fc[i] > 0x7E) fc[i] = '.';
    // WMV3 STRUCT_C extradata: size u16 @fmt+48, src ptr @fmt+52 (or inline). Dump both
    // interpretations so we can validate the sequence header the SIMD decoder parses.
    uint16_t exSize = r16(fmt + 48);
    uint32_t exPtr = r32(fmt + 52), exDeref0 = exPtr ? r32(exPtr) : 0;
    // The codec's OWN extradata copy: sub_824B2E78 writes the codec core to *(a1+32), then
    // memcpy's fmt+52 -> codec+84 (buffer ptr) with size codec+88 (u16). The readfn returns
    // codec+84 on the WMV3 EOS path = the seq header the SIMD decoder parses.
    uint32_t codec = r32(a1 + 32);
    uint32_t cxBufPtr = codec ? r32(codec + 84) : 0;
    uint16_t cxBufSz = codec ? r16(codec + 88) : 0;
    uint32_t cxBufData = cxBufPtr ? r32(cxBufPtr) : 0;
    // Raw dump of the whole format struct so we can read biSize + locate the extradata
    // and whether the BITMAPINFOHEADER copy was short (dropped the trailing STRUCT_C).
    char raw[220]; int ro = 0; raw[0] = 0;
    for (int i = 0; i < 0x68 && ro < 200; i += 4)
      ro += snprintf(raw + ro, sizeof(raw) - ro, "%08X ", r32(fmt + i));
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMV_AFMT] #%u sub_824B2E78(fmtObj=%08X) -> rc=%08X | fourcc='%s' ch=%u rate=%u | "
      "exSize=%u exPtr=%08X exData0=%08X preExData=%08X | codec=%08X cxBuf=%08X cxSz=%u cxData=%08X | fmt[0..67]=%s",
      n, a2, ctx.r3.u32, fc, wCh, wRate, exSize, exPtr, exDeref0, preExData,
      codec, cxBufPtr, cxBufSz, cxBufData, raw);
  }
}

// iter-362j: THE decisive input check. sub_824DF168(codecState, formatTag, ...) is the
// WMA decoder core init; it switches on the format-tag fourcc (a2=r4) and returns 6 on
// an unknown tag, or a deeper status. Its return feeds sub_824B2740 (default->E_FAIL).
// Log the format tag + the return: a VALID WMA fourcc ("WMA1/2/3"=0x574D41xx etc.) with
// a bad return => codegen/DSP bug (oracle/difftest); a GARBAGE tag => our ASF parse
// delivered a wrong format field (trace the parse, fixable our-side).
extern "C" void __imp__sub_824DF168(PPCContext& __restrict ctx, uint8_t* base);
void sub_824DF168(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t a1 = ctx.r3.u32, tag = ctx.r4.u32, a3 = ctx.r5.u32;
  // iter-403: this probe logs ONLY after the call returns, which is exactly why
  // the terminal XMVCREATE's hang looked like "nothing happened here" — the
  // last thing the creating thread emits is [XMV_G4] bindsrc, and if this call
  // never returns the post-log never prints. Log entry too, so an ENTER with no
  // matching line is a positive identification instead of an absence.
  {
    static std::atomic<unsigned> s_e{0};
    unsigned e = s_e.fetch_add(1, std::memory_order_relaxed) + 1;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMV_WMAINIT] ENTER #%u state=%08X tag=%08X a3=%08X lr=%08X tid=%lu",
      e, a1, tag, a3, (uint32_t)ctx.lr, GetCurrentThreadId());
  }
  __imp__sub_824DF168(ctx, base);
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMV_WMAINIT] EXIT  #%u rc=%08X tid=%lu", n, ctx.r3.u32, GetCurrentThreadId());
  }
  if (n <= 6) {
    char fourcc[5] = {char(tag >> 24), char(tag >> 16), char(tag >> 8), char(tag), 0};
    for (int i = 0; i < 4; i++) if (fourcc[i] < 0x20 || fourcc[i] > 0x7E) fourcc[i] = '.';
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMV_WMAINIT] #%u sub_824DF168(state=%08X tag=%08X '%s' a3=%08X) -> rc=%08X",
      n, a1, tag, fourcc, a3, ctx.r3.u32);
  }
}

// iter-362k: LOCATE the miscompile inside sub_824B2E78 (WMV3 video decoder init, confirmed
// our-bug by live Xenia). E_FAIL 0x80004005 = sub_824B2740's DEFAULT (input status NOT in
// [0,11]). The status feeding it comes from sub_824BF348 (DSP init, line 70) or
// sub_824BF5B8 (line 77). Capture: sub_824B2740's INPUT (the exact out-of-range status) +
// output, filtered to the E_FAIL mapping; and the two producers' returns.
extern "C" void __imp__sub_824B2740(PPCContext& __restrict ctx, uint8_t* base);
void sub_824B2740(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t in = ctx.r3.u32;
  __imp__sub_824B2740(ctx, base);
  static std::atomic<unsigned> s_bad{0};
  // Log the E_FAIL mapping (out-of-range input) + a few generic negatives.
  if (ctx.r3.u32 == 0x80004005u && s_bad.fetch_add(1, std::memory_order_relaxed) < 12)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[WMV_MAP] sub_824B2740(status=%d / 0x%08X) -> 0x%08X  <<<< E_FAIL (status out of [0,11])",
      (int)in, in, ctx.r3.u32);
}

#define SR_WMV_RET_PROBE(fn, tag)                                              \
  extern "C" void __imp__##fn(PPCContext& __restrict ctx, uint8_t* base);      \
  void fn(PPCContext& __restrict ctx, uint8_t* base) {                         \
    uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32;               \
    __imp__##fn(ctx, base);                                                    \
    static std::atomic<unsigned> s_n{0}, s_bad{0};                            \
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;             \
    int rc = (int)ctx.r3.u32;                                                  \
    bool bad = (rc < 0 || rc > 11);                                           \
    if ((bad && s_bad.fetch_add(1, std::memory_order_relaxed) < 12) || n <= 3)\
      SR_LOG_BOOT(sr::LogSeverity::Warn,                                       \
        "[WMV_RET] %s(r3=%08X r4=%08X r5=%08X) -> %d / 0x%08X%s", tag, a3, a4, \
        a5, rc, ctx.r3.u32, bad ? "  <<<< out-of-[0,11]" : "");               \
  }
SR_WMV_RET_PROBE(sub_824BF348, "824BF348/dspInit")
SR_WMV_RET_PROBE(sub_824BF5B8, "824BF5B8/producer2")
SR_WMV_RET_PROBE(sub_824BE438, "824BE438/tableInit")
SR_WMV_RET_PROBE(sub_824DCAB0, "824DCAB0/successRet")
// The 4 setup calls inside sub_824DF168's success block; sub_824DCAB0 was NOT reached,
// so one of these returns a pointer (should be 0) -> sub_824DF168 falls to `return result`.
SR_WMV_RET_PROBE(sub_82507380, "82507380/setupA")
SR_WMV_RET_PROBE(sub_824DE5A0, "824DE5A0/setupB")
SR_WMV_RET_PROBE(sub_824DE7F8, "824DE7F8/setupC")
SR_WMV_RET_PROBE(sub_824DF098, "824DF098/setupD")
// setupC=sub_824DE7F8 returns the pointer; its two early-return sub-calls:
SR_WMV_RET_PROBE(sub_82507520, "82507520/C_sub1")
SR_WMV_RET_PROBE(sub_824DA900, "824DA900/C_sub2")
#undef SR_WMV_RET_PROBE

// iter-362l: the ASF Type-Specific Data byte-array reader. sub_82493060 reads the WMV3
// STRUCT_C extradata via sub_824925D8(a1, dest, size, ...). The extradata comes out ZERO.
// Log small reads (size<=16) + the bytes actually delivered + return, to see if the
// extradata read short-delivers zeros vs is skipped. dest = the buffer being filled.
namespace sr { namespace kernel {
bool armRuntimeWriteWatch(uint32_t, uint32_t);
void registerFmvExtradataReserve(uint32_t, uint32_t);
bool restoreFmvExtradataIfCorrupt(uint32_t, uint32_t);
} }
extern "C" void __imp__sub_824925D8(PPCContext& __restrict ctx, uint8_t* base);
void sub_824925D8(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t a1 = ctx.r3.u32, dest = ctx.r4.u32, size = ctx.r5.u32;
  uint32_t lr = (uint32_t)ctx.lr;
  __imp__sub_824925D8(ctx, base);
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  // Video WMV3 extradata read = caller 0x82493988 (sub_82493060 video branch), size=4.
  // Always capture that site; capture other small reads for the first 400 calls.
  bool videoExtra = (lr >= 0x82493980 && lr <= 0x82493998);
  if ((size <= 16 && n <= 400) || videoExtra) {
    auto r32 = [](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[ASF_TSD] #%u%s sub_824925D8(ctx=%08X lr=%08X dest=%08X size=%u) -> rc=%08X | data=%08X %08X %08X %08X",
      n, videoExtra ? " VIDEO-EXTRADATA" : "", a1, lr, dest, size, ctx.r3.u32,
      r32(dest), r32(dest + 4), r32(dest + 8), r32(dest + 12));
  }
  // iter-362m: the video extradata buffer is written 4FF10A01 CORRECTLY here, then
  // ZEROED before sub_824B2E78 reads it (proven: [XMV_AFMT] exData0=0 same ptr).
  // SR_XMV_EXTRA_WATCH=1 arms the HOST page-guard on the just-written buffer and
  // LEAVES it armed (the clobber is ~330ms later, cross-call), so the VEH full-logs
  // the zeroing writer's guest fn + backtrace. Arm once (first FMV, deterministic ptr).
  if (videoExtra && dest) {
    // Register the just-filled extradata buffer as a reserved region so the
    // sub_82467F40 hook won't hand it to sub_82486820's overlapping scratch
    // (guarded by SR_XMV_EXTRA_DISJOINT on the alloc side). Cheap, always safe.
    if (size) sr::kernel::registerFmvExtradataReserve(dest, size);
    static const bool s_watch = [] {
      const char* e = std::getenv("SR_XMV_EXTRA_WATCH");
      return e && e[0] == '1';
    }();
    static std::atomic<unsigned> s_armed{0};
    if (s_watch && s_armed.fetch_add(1, std::memory_order_relaxed) == 0) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[XMV_EXTRA_WATCH] arming HOST page-guard on video extradata buffer %08X "
        "(=4FF10A01 now) to catch the zeroing writer", dest);
      sr::kernel::armRuntimeWriteWatch(dest, 4);
    }
  }
}

// iter-362l: sub_824C9740 (WMV3 decode/validate core, variant 6) returns 6 -> 0x80600006.
// The 3 return-6 sites gate on decoder-state flags. Dump them when it returns 6 to see
// which config the decoder rejects (and whether the value looks wrong/garbage vs a real
// unsupported feature). a1 = codec core (r3).
extern "C" void __imp__sub_824C9740(PPCContext& __restrict ctx, uint8_t* base);
void sub_824C9740(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t a1 = ctx.r3.u32;
  auto R = [&](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };
  auto F = [&](uint32_t idx) { return a1 ? sr::kernel::readGuestU32BE(a1 + idx * 4) : 0u; };
  // iter-362m: 0x80600006 discriminant is a1[967] (console>=2, ours=0), set from a seq-header
  // bitstream value. Dump the bit-buffer at core+80 (setup by sub_824C4250: +0 accumulator,
  // +8 bitpos, +12 byteidx, +16 end, +24/+28 byte source) + the key gate fields at ENTRY, and
  // a1[967] + rc after. Tells us if the seq header (STRUCT_C 4FF10A01) reached the bit reader.
  uint32_t bb = F(20);           // core+80 = bit-buffer ptr
  uint32_t accHi=R(bb), accLo=R(bb+4), bpos=R(bb+8), bidx=R(bb+12), bend=R(bb+16),
           bsrc24=R(bb+24), bsrc28=R(bb+28);
  uint32_t f3813=F(3813), f444=F(444), f3856=F(3856);
  // iter-362m: the seq-header READER at core+3308 delivers 0. Dump it to find its byte source.
  // reader is a callback obj: sub_827BAB80 calls (*(reader+4))(*(reader+0),...). readfn identifies
  // WHICH reader (mem-buffer over STRUCT_C vs frame-stream demux); ctx holds the byte buffer.
  uint32_t rdr = F(827);         // core+3308 = reader
  uint32_t rdCtx = R(rdr), rdFn = R(rdr + 4);
  // readfn sub_824B2818 delegates to the underlying SOURCE at codec+32 (=rdCtx+0x20):
  // v10=*(codec+32); read method *(v10); available *(v10+16); buffer *(v10+20); EOS *(v10+80).
  uint32_t src = R(rdCtx + 0x20);
  uint32_t srcAvail=R(src+16), srcBuf=R(src+20),
           src80=R(src+80), src84=R(src+84), src88=(src?sr::kernel::readGuestU32BE(src+88):0);
  uint32_t cachedLen = (uint16_t)(src88 >> 16);   // *(u16)(src+88)
  // cached buffer content (src+84 ptr) — console has a 256KB block here; is ours filled?
  uint32_t ca0=R(src84), ca4=R(src84+4);
  // iter-362n: the SR_WMV_SEQ_RESTORE forcing (writing 4FF10A01 into src+84) is DELETED — it was
  // the diagnostic that proved the STRUCT_C-at-seq-buffer root; the natural fix (SR_FMV_TINY_ALLOC_FIX,
  // gpu_ring_alloc_fix.cpp) now makes the 4-byte extradata allocs survive so ca0 is 4FF10A01
  // naturally. [WMV_CFG] logging kept as the residual diagnostic.
  __imp__sub_824C9740(ctx, base);
  uint32_t f967=F(967), f961=F(961), f445=F(445), f972=F(972), f4684=F(4684),
           f963=F(963), f970=F(970), f3693=F(3693), f984=F(984), f3681=F(3681), f3686=F(3686);
  {
    static std::atomic<unsigned> s_sn{0};
    static const bool s_seq = [] { const char* e = std::getenv("SR_WMV_SEQ_RESTORE"); return e && e[0] == '1'; }();
    if (s_seq && s_sn.fetch_add(1, std::memory_order_relaxed) < 8)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[WMV_SEQ_TEST] sub_824C9740(core=%08X) src84=%08X cachedLen=%u ca0(was)=%08X -> rc=%08X a1[967]=%X",
        a1, src84, cachedLen, ca0, ctx.r3.u32, f967);
  }
  static std::atomic<unsigned> s_n{0};
  if (ctx.r3.u32 == 6 && s_n.fetch_add(1, std::memory_order_relaxed) < 8)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[WMV_CFG] sub_824C9740(core=%08X)->6 | a1[967]=%X | SRC=%08X avail[+16]=%d buf[+20]=%08X "
      "EOS[+80]=%X cachedPtr[+84]=%08X cachedLen(u16@+88)=%u | cachedBytes=%08X %08X",
      a1, f967, src, (int)srcAvail, srcBuf, src80, src84, cachedLen, ca0, ca4);
}

// iter-362m: the VIDEO ELEMENTARY STREAM source read (empty/EOS at init). It looks up the stream
// slot via sub_82483400(slotArr=*(a1+72), idx=*(a1+96)) then returns -2142240757 if slot[6] (the
// packet queue head) is NULL. Dump: stream idx, the slot array, the resolved slot + its queue
// head (slot+24) + count, and the return. Tells us WRONG-SLOT (mis-wired idx) vs EMPTY-QUEUE
// (demuxer never delivered video packets). Throttled; SR_LOG_BOOT.
extern "C" void __imp__sub_82483CC8(PPCContext& __restrict ctx, uint8_t* base);
void sub_82483CC8(PPCContext& __restrict ctx, uint8_t* base) {
  auto R = [](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };
  uint32_t a1 = ctx.r3.u32;
  uint32_t idx = a1 ? (sr::kernel::readGuestU32BE(a1 + 96) >> 24) : 0;  // *(u8)(a1+96), BE top byte
  uint32_t slotArr = R(a1 + 72);
  uint32_t slot = slotArr ? R(slotArr + ((4 * idx) & 0x3FC)) : 0;
  uint32_t queue = R(slot + 24), qcount = R(slot + 20);
  __imp__sub_82483CC8(ctx, base);
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 10)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[VID_SRC] #%u sub_82483CC8(src=%08X idx=%u slotArr=%08X) | slot=%08X queue[+24]=%08X cnt[+20]=%X -> rc=%08X",
      n, a1, idx, slotArr, slot, queue, qcount, ctx.r3.u32);
}

// iter-362m: the VIDEO PACKET-RECEIVE callback. sub_82484A10 registers sub_824847B0 with the
// demux (sub_82482AF0); the demux calls it with a2=0xC0000 (786432) to DELIVER a packet, which
// sub_824845F0 enqueues into the stream slot. If this never fires (or never with 0xC0000), the
// demuxer delivers NO video packets -> the stream is empty+EOS -> blank seq header -> 0x80600006.
extern "C" void __imp__sub_824847B0(PPCContext& __restrict ctx, uint8_t* base);
void sub_824847B0(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t a1 = ctx.r3.u32, a2 = ctx.r4.u32;
  __imp__sub_824847B0(ctx, base);
  static std::atomic<unsigned> s_n{0}, s_pkt{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  bool pkt = (a2 == 786432u);
  if (n <= 20 || (pkt && s_pkt.fetch_add(1, std::memory_order_relaxed) < 12))
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[VID_PKT] #%u sub_824847B0(ctx=%08X cmd=%u%s) -> rc=%08X", n, a1, a2,
      pkt ? " DELIVER-PACKET" : "", ctx.r3.u32);
}

// iter-362m: the bit-buffer setup for the WMV3 seq-header parse. sub_824BF5B8 calls
// sub_824C4250(bitbuf=core[19], seqBytes) to load the seq header (STRUCT_C) before
// sub_824BBBC0 -> sub_824C9740 reads it. Dump the source byte pointer + the first 16 bytes:
// this is the EXACT seq header the bit reader will consume. 4FF10A01-ish => data OK (bug is the
// bit-reader codegen); 0/short => the seq header isn't being delivered (data bug, like STRUCT_C).
extern "C" void __imp__sub_824C4250(PPCContext& __restrict ctx, uint8_t* base);
void sub_824C4250(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t bitbuf = ctx.r3.u32, src = ctx.r4.u32, lr = (uint32_t)ctx.lr;
  bool wmvSeq = (lr >= 0x824BF700 && lr <= 0x824BF900);
  // iter-362n: the SR_WMV_SEQ_RESTORE forcing (writing 4FF10A01 into src) is DELETED — proven
  // diagnostic; the natural fix (SR_FMV_TINY_ALLOC_FIX) now delivers the real STRUCT_C here. The
  // [WMV_BITSRC] logging below stays as the residual diagnostic (seqbytes should read 4FF10A01).
  (void)wmvSeq;
  __imp__sub_824C4250(ctx, base);
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  // WMV3 seq-header setup is called from sub_824BF5B8 (~0x824BF808); log those + first few.
  bool wmv = (lr >= 0x824BF700 && lr <= 0x824BF900);
  if (wmv || n <= 8) {
    auto R = [&](uint32_t a) { return a ? sr::kernel::readGuestU32BE(a) : 0u; };
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[WMV_BITSRC]%s #%u sub_824C4250(bitbuf=%08X src=%08X lr=%08X) | seqbytes=%08X %08X %08X %08X",
      wmv ? " WMV3-SEQHDR" : "", n, bitbuf, src, lr, R(src), R(src+4), R(src+8), R(src+12));
  }
}

// iter-360d round 4: sub_82478650 returns 0x8050000C from one of FIVE calls:
//  (1) sub_82483180(dec0x98, p5, p6, *piVar3)   [format-handler table]
//  (2) (*(*piVar3))(p5,p6,p7,dec0x98)            [vtable A method 0]
//  (3) sub_82483170()                            [thunk -> PTR_8249ABB8]
//  (4) (*(*param_1))(p2,*piVar3,param_1[1],dec)  [vtable B method 0]
//  (5) (*(*param_1+8))()                         [vtable B method 2]
// Custom hook: dump the two vtable ptrs (*param_1, *piVar3) so we can name the
// indirect targets, and log which sub_82483180 return we get.
extern "C" void __imp__sub_82478650(PPCContext& __restrict ctx, uint8_t* base);
void sub_82478650(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t p1 = ctx.r3.u32;
  uint32_t vtB = sr::kernel::readGuestU32BE(p1);         // *param_1
  uint32_t vtA = sr::kernel::readGuestU32BE(p1 + 8);     // *piVar3 (param_1+2 words)
  uint32_t vtB0 = vtB ? sr::kernel::readGuestU32BE(vtB) : 0;
  uint32_t vtB2 = vtB ? sr::kernel::readGuestU32BE(vtB + 8) : 0;
  uint32_t vtA0 = vtA ? sr::kernel::readGuestU32BE(vtA) : 0;
  __imp__sub_82478650(ctx, base);
  if (n <= 4)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMV_FIN] sub_82478650(dec=%08X) -> %08X | vtB=%08X vtB[0]=%08X vtB[2]=%08X | vtA=%08X vtA[0]=%08X",
      p1, ctx.r3.u32, vtB, vtB0, vtB2, vtA, vtA0);
}

// iter-357l round 4: sub_8247C650 returns 3 from ONE gate —
//   rc = sub_82488070(this,1) != 0  ||  *(this+0x40)==0 || *(short*)(this+0x4c)==0
//        || *(this+0x48)==0 || *(this+0x44)==0
// i.e. the container header parse failed or left the format fields empty.
extern "C" void __imp__sub_82488070(PPCContext& __restrict ctx, uint8_t* base);
void sub_82488070(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t o = ctx.r3.u32;
  __imp__sub_82488070(ctx, base);
  if (n <= 4) {
    auto rd = [&](uint32_t off) { return sr::kernel::readGuestU32BE(o + off); };
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[XMV_HDR] sub_82488070(this=%08X) -> rc=%08X | +40=%08X +44=%08X "
        "+48=%08X +4c=%08X (chan=%u) | +10=%08X +14=%08X +e4=%04X",
        o, ctx.r3.u32, rd(0x40), rd(0x44), rd(0x48), rd(0x4c),
        (unsigned)(rd(0x4c) >> 16), rd(0x10), rd(0x14), rd(0xe4) >> 16);
  }
}

// iter-357l round 5 — THE GATE: sub_82485798 (read ASF/XMV header object) does
//   n = FUN_82475868(this, pos, 0x1E, &buf);  if (n != 0x1E) return 3;
// i.e. the SOURCE READ is short. Log every read: request vs actual, and the
// first bytes delivered (a real header starts with the 16-byte GUID at
// 0x82050908).
// ABI: FUN_82475868(this, longlong pos, uint32 size, void** outBuf) — the
// 64-bit pos occupies r4 alone, so size=r5 and outBuf=r6 (an earlier probe
// mis-mapped these and cried SHORT on healthy reads; corrected).
// iter-357l round 7: the wrapper forwards to sub_824754B8(*(this+0x260), ...).
// [XMV_CACHE] proved that reader pointer is VALID for the first two reads and
// NULL for the rest — so *(this+0x260) is being cleared (or a second parser
// instance never got it set). Log the field + the caller each time.
extern "C" void __imp__sub_82475868(PPCContext& __restrict ctx, uint8_t* base);
void sub_82475868(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t self = ctx.r3.u32;
  uint64_t pos = ctx.r4.u64;
  uint32_t want = ctx.r5.u32;
  uint32_t outp = ctx.r6.u32;
  uint32_t rdrPre = self ? sr::kernel::readGuestU32BE(self + 0x260u) : 0;
  uint32_t lr = (uint32_t)ctx.lr;
  __imp__sub_82475868(ctx, base);
  if (n <= 16) {
    uint32_t got = ctx.r3.u32;
    char head[49] = {0};
    uint32_t bufVA = (outp >= 0x1000u && outp < 0xC0000000u)
                         ? sr::kernel::readGuestU32BE(outp) : 0;
    if (bufVA >= 0x1000u && bufVA < 0xC0000000u) {
      const uint8_t* p = sr::kernel::guestVAToHost(bufVA);
      if (p) for (int i = 0; i < 16; ++i) snprintf(head + i * 3, 4, "%02X ", p[i]);
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[XMV_READ] #%u parser=%08X rdr(+260)=%08X lr=%08X pos=%llu want=%u "
        "-> got=%u buf=%08X [%s]%s",
        n, self, rdrPre, lr, (unsigned long long)pos, want, got, bufVA, head,
        (got != want) ? "  <<<< SHORT" : "");
  }
}

// iter-357l round 5b: the file source's own seek/read (vt 0x82050228:
// +0x34=sub_82477660 seek, +0x3C=sub_824776F0 read). The buffered reader
// sub_824754B8 fills a 64KB cache through these; if the fill comes up short
// the header walk dies with rc=3.
extern "C" void __imp__sub_82477660(PPCContext& __restrict ctx, uint8_t* base);
void sub_82477660(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t self = ctx.r3.u32;
  uint64_t pos = ctx.r4.u64;
  __imp__sub_82477660(ctx, base);
  if (n <= 12)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[XMV_SEEK] #%u src=%08X pos=%llu -> rc=%08X", n, self,
        (unsigned long long)pos, ctx.r3.u32);
}

extern "C" void __imp__sub_824776F0(PPCContext& __restrict ctx, uint8_t* base);
void sub_824776F0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t self = ctx.r3.u32, dst = ctx.r4.u32, len = ctx.r5.u32,
           outp = ctx.r6.u32;
  __imp__sub_824776F0(ctx, base);
  if (n <= 12) {
    uint32_t actual = (outp >= 0x1000u && outp < 0xC0000000u)
                          ? sr::kernel::readGuestU32BE(outp) : 0xFFFFFFFF;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[XMV_FREAD] #%u src=%08X dst=%08X len=%u -> rc=%08X actual=%u%s", n,
        self, dst, len, ctx.r3.u32, actual,
        (actual != len) ? "  <<<< SHORT FILL" : "");
  }
}

// iter-357l round 6 — THE CONTRADICTION: the 64KB cache fill succeeds
// (actual=65536 from pos 0, real ASF GUID in the buffer) and reads at pos
// 0/30 serve fine, yet a 10-byte read at pos 54 returns 0. Per the decompile
// that is impossible unless the cache bookkeeping (+0x298 start, +0x29c len,
// +0x2c buf) says otherwise. Dump it on every call.
extern "C" void __imp__sub_824754B8(PPCContext& __restrict ctx, uint8_t* base);
void sub_824754B8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t self = ctx.r3.u32;
  uint64_t pos = ctx.r4.u64;
  uint32_t want = ctx.r5.u32;
  auto rd = [&](uint32_t off) { return sr::kernel::readGuestU32BE(self + off); };
  uint32_t cs = rd(0x298), cl = rd(0x29c), cb = rd(0x2c), srcp = rd(0x34);
  __imp__sub_824754B8(ctx, base);
  if (n <= 12)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[XMV_CACHE] #%u rdr=%08X pos=%llu want=%u | pre{start=%u len=%u "
        "buf=%08X src=%08X} -> got=%u | post{start=%u len=%u}", n, self,
        (unsigned long long)pos, want, cs, cl, cb, srcp, ctx.r3.u32,
        rd(0x298), rd(0x29c));
}

// iter-357l round 8 — bracket the clear of parser+0x260 (the reader
// back-pointer set by setsrc). Valid at read #2 (lr=82485450, inside
// sub_82485408), NULL at read #3 (lr=8248692C, inside sub_82486820). Log the
// field at entry/exit of every handler in the walk; the one that enters with
// it set and exits with it zero is the culprit.
// Published for the XMV allocator overlap check in gpu_ring_alloc_fix.cpp.
std::atomic<uint32_t> g_xmvParser{0};
namespace {
void xmvField(const char* tag, const char* when, uint32_t parser) {
  static std::atomic<unsigned> s_n{0};
  if (s_n.fetch_add(1, std::memory_order_relaxed) >= 40) return;
  if (!parser) return;
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[XMV_260] %-12s %-5s parser=%08X +260=%08X",
              tag, when, parser, sr::kernel::readGuestU32BE(parser + 0x260u));
}
}  // namespace

#define SR_XMV_260_PROBE(fn, tag)                                             \
  extern "C" void __imp__##fn(PPCContext& __restrict ctx, uint8_t* base);     \
  void fn(PPCContext& __restrict ctx, uint8_t* base) {                        \
    uint32_t p = ctx.r3.u32;                                                  \
    g_xmvParser.store(p, std::memory_order_relaxed);                          \
    xmvField(tag, "in", p);                                                   \
    __imp__##fn(ctx, base);                                                   \
    xmvField(tag, "out", p);                                                  \
  }

SR_XMV_260_PROBE(sub_82485408, "85408/objhdr")
SR_XMV_260_PROBE(sub_82485990, "85990/A")
SR_XMV_260_PROBE(sub_82485C00, "85C00/B")
#undef SR_XMV_260_PROBE

// iter-357l round 10 — NAME THE STORING INSTRUCTION. The dlmalloc-family
// allocator (sub_82467F40 -> sub_82468008 -> sub_828EE050) writes 16 bytes at
// (returnedChunk - 0x20) = exactly parser+0x260, i.e. the previous block's
// TAIL. That is dlmalloc's backward-coalesce boundary tag, which it only
// writes when it believes the preceding chunk is FREE — but the parser is
// live. Arm the PPC_STORE watch (g_aptEnvVA watches [va, va+16)) on the victim
// field for the duration of the handler; the watchpoint logs the recomp
// file:line of the storing guest instruction.
// iter-357l round 11: the guest-store watch found NOTHING writing
// parser+0x260, so the writer is host-side (bypasses PPC_STORE). Arm the
// HOST page-guard (VirtualProtect + VEH, iter-323 re-arming mode) on the
// victim for the duration of the handler — the VEH logs the faulting host
// RIP + full context, which names the writer outright.
namespace sr { namespace kernel {
bool armRuntimeWriteWatch(uint32_t guestVA, uint32_t len);
void disarmRuntimeWriteWatch();
extern std::atomic<uint32_t> g_xmvParserWatch;  // guestHeapAlloc overlap report
} }

extern "C" void __imp__sub_82486820(PPCContext& __restrict ctx, uint8_t* base);
void sub_82486820(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t p = ctx.r3.u32;
  g_xmvParser.store(p, std::memory_order_relaxed);
  xmvField("86820/cdesc", "in", p);
  static std::atomic<unsigned> s_arm{0};
  // iter-462: the host page-guard is a DIAGNOSTIC, now opt-in (SR_XMV_WATCH=1).
  // Ungated it froze a boot (archive/v1_freeze1): its disarm lost to the VEH
  // re-arm, page 0x83A8E000 stayed read-only after "disarmed", the audio
  // callback's writes faulted and the XMV parser thread died in
  // AsfReadObjectHeader (workflow cs-livelock-rootcause, mechanism B).
  static const bool s_watch = [] {
    const char* e = std::getenv("SR_XMV_WATCH");
    return e && e[0] == '1';
  }();
  const bool arm = s_watch && p && s_arm.fetch_add(1, std::memory_order_relaxed) < 2;
  bool armed = false;
  if (arm) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[XMV_WATCH] arming HOST page-guard on parser+0x260 = %08X",
        p + 0x260u);
    armed = sr::kernel::armRuntimeWriteWatch(p + 0x260u, 16);
  }
  sr::kernel::g_xmvParserWatch.store(p, std::memory_order_relaxed);
  __imp__sub_82486820(ctx, base);
  sr::kernel::g_xmvParserWatch.store(0, std::memory_order_relaxed);
  if (armed) {
    sr::kernel::disarmRuntimeWriteWatch();
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[XMV_WATCH] disarmed; parser+0x260 now = %08X",
        sr::kernel::readGuestU32BE(p + 0x260u));
  }
  xmvField("86820/cdesc", "out", p);
}

extern "C" void __imp__sub_824726B0(PPCContext& __restrict ctx, uint8_t* base);
void sub_824726B0(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t o = ctx.r3.u32;
  __imp__sub_824726B0(ctx, base);
  static std::atomic<unsigned> s_n{0};
  if (s_n.fetch_add(1, std::memory_order_relaxed) < 4)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[XMV_STEP] sub_824726B0(vdec=%08X) -> rc=%08X",
                o, ctx.r3.u32);
}

extern "C" void __imp__sub_82472068(PPCContext& __restrict ctx, uint8_t* base);
void sub_82472068(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t o = ctx.r3.u32, p = ctx.r4.u32;
  __imp__sub_82472068(ctx, base);
  static std::atomic<unsigned> s_n{0};
  if (s_n.fetch_add(1, std::memory_order_relaxed) < 4)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[XMV_STEP] sub_82472068(adec=%08X dev=%08X) -> rc=%08X", o, p,
                ctx.r3.u32);
}

extern "C" void __imp__sub_82476D40(PPCContext& __restrict ctx, uint8_t* base);
void sub_82476D40(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t sz = ctx.r3.u32;
  __imp__sub_82476D40(ctx, base);
  if (n <= 8)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[XMV_SRCALLOC] #%u FUN_82476d40(size=%u) -> %08X", n, sz, ctx.r3.u32);
}

extern "C" void __imp__sub_824780A8(PPCContext& __restrict ctx, uint8_t* base);
void sub_824780A8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  __imp__sub_824780A8(ctx, base);
  if (n <= 8)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[XMV_SRCCTOR] #%u Function_824780A8 -> obj=%08X vt=%08X", n,
        ctx.r3.u32,
        ctx.r3.u32 ? sr::kernel::readGuestU32BE(ctx.r3.u32) : 0);
}

// iter-357j: XMV player create + the render-side FMV SM — the pump dispatched
// once but no player appeared and state stayed 1. Name what happens inside.
extern "C" unsigned long __stdcall GetCurrentThreadId(void);

// iter-400 breadcrumbs: EVERY run's terminal event is an XMVCREATE that ENTERs
// and never EXITs (5 ENTER/4 EXIT etc., treatment AND control), with no guest
// thread visibly parked inside it. Bisect which callee never returns.
// sub_824715F0 -> sub_82470F28 (case 2: source ctors + vt+96 opens) ->
// sub_82470A58 (the player/pipeline construct, which runs InitWorkerThreads).
// (sub_82470A58 / sub_82480598 / sub_82480888 are hooked further down this file
// and already log enter/exit; only the dispatcher itself was uninstrumented.
// Written out longhand rather than macro-generated so gen_override_manifest.py
// can see the symbol — the registry cannot classify what it cannot scan.)
extern "C" void __imp__sub_82470F28(PPCContext& __restrict ctx, uint8_t* base);
void sub_82470F28(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMVSTEP] F28/dispatch ENTER #%u dev=%08X desc=%08X lr=%08X tid=%lu",
      n, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr, GetCurrentThreadId());
  __imp__sub_82470F28(ctx, base);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMVSTEP] F28/dispatch EXIT  #%u rc=%08X tid=%lu",
      n, ctx.r3.u32, GetCurrentThreadId());
}

extern "C" void __imp__sub_824715F0(PPCContext& __restrict ctx, uint8_t* base);
void sub_824715F0(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t d3d = ctx.r3.u32, h = ctx.r4.u32;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMVCREATE] ENTER d3d=%08X handle=%08X r5=%08X r6=%08X r7=%08X lr=%08X",
      d3d, h, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, (uint32_t)ctx.lr);
  // iter-364 COUPLING DIAGNOSTIC (SR_FMV_SUPPRESS, default-OFF, NOT baked): the fence unblocked
  // the WMV3 decoder, which now SPINS a thread inside sub_82480D80 (5/6 rolls, stuck the whole run)
  // while deep-Apt (defExec) is 0/6. Hypothesis: the spinning decoder starves the menu trunk. To
  // A/B the coupling, fail XMVCREATE here (return negative) so NO player/worker/decode is created
  // (SM parks in state 3) => no spin. If deep-Apt fires with this ON but not OFF, the decoder spin
  // is the trunk drag. Named opt-in diagnostic (charter: force->diagnose->root-fix->delete).
  static const bool s_suppress = [] {
    const char* e = std::getenv("SR_FMV_SUPPRESS");
    return e && e[0] == '1';
  }();
  if (s_suppress) {
    ctx.r3.u32 = 0x8000FFFFu;  // negative -> FMV SM sub_8235F488 case1 goes to state 3 (no decode)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[XMVCREATE] SUPPRESSED (SR_FMV_SUPPRESS) -> rc=8000FFFF (no player/decode)");
    return;
  }
  __imp__sub_824715F0(ctx, base);
  const uint32_t playerVA = sr::kernel::readGuestU32BE(0x829761D4u);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMVCREATE] EXIT rc=%08X player=%08X", ctx.r3.u32, playerVA);
  // iter-397k DIAGNOSTIC (SR_FMV_MODE2, default-OFF, NOT baked): the frame-get
  // (sub_82480D80) treats an EMPTY frame-queue item as END-OF-MOVIE when
  // player+220 bit1 is clear, but as "re-arm and retry" when set. Our pipeline
  // produces one legit empty pull (decode #2 finishes before the WMV3 core
  // emits picture #2 — B-frame/latency warmup), so the movie dies at frame 1.
  // Console must run with bit1 set (or never produce empty pulls). Setting it
  // here proves/disproves the mechanism; the natural setter is then found via
  // the Xenia oracle (JIT-hook sub_8247FCB8) and this diagnostic deleted.
  static const bool s_mode2 = [] {
    const char* e = std::getenv("SR_FMV_MODE2");
    return e && e[0] == '1';
  }();
  if (s_mode2 && ctx.r3.u32 == 0 && playerVA >= 0x1000u) {
    const uint32_t m = sr::kernel::readGuestU32BE(playerVA + 220u);
    sr::kernel::writeGuestBE32(playerVA + 220u, m | 2u);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[XMVCREATE] SR_FMV_MODE2: player+220 %X -> %X", m, m | 2u);
  }
}

// iter-397k2 (same SR_FMV_MODE2 diagnostic, correct injection point): +220 is
// OVERWRITTEN each call by a2 = desc[1] (sub_8247E9D0 wiring), and the app
// (RenderYuvVideoFrameTec0) hard-codes desc[1]=0. Patch the desc's mode field
// at the thunk so an empty pull RETRIES instead of declaring end-of-movie.
extern "C" void __imp__sub_8247E9D0(PPCContext& __restrict ctx, uint8_t* base);
void sub_8247E9D0(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_mode2 = [] {
    const char* e = std::getenv("SR_FMV_MODE2");
    return e && e[0] == '1';
  }();
  if (s_mode2 && ctx.r4.u32 >= 0x1000u) {
    const uint32_t d1 = sr::kernel::readGuestU32BE(ctx.r4.u32 + 4u);
    if ((d1 & 2u) == 0)
      sr::kernel::writeGuestBE32(ctx.r4.u32 + 4u, d1 | 2u);
  }
  __imp__sub_8247E9D0(ctx, base);
}

void hlslNoteCoreEntry();   // below (SR_HLSL_COMPILE_WITNESS)

// iter-397q [HLSLC]: the runtime HLSL compiler core (CompileHlslShaderCore_
// sub_823B2D10; wrapper 823B3370 matched profile strings ps_3_0/vs_3_0). The
// FMV effect's pass shaders ("compile ps_2_0 PS()") never yield objects
// (holder+4 AND holder+8 both NULL) — is the core CALLED, and with what rc?
extern "C" void __imp__sub_823B2D10(PPCContext& __restrict ctx, uint8_t* base);
void sub_823B2D10(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<int> s_n{0};
  const int n = s_n.fetch_add(1, std::memory_order_relaxed);
  char prof[12] = {0};
  const uint32_t profPP = ctx.r5.u32;
  const uint32_t profP = profPP >= 0x1000u ? sr::kernel::readGuestU32BE(profPP) : 0;
  if (profP >= 0x1000u)
    for (int i = 0; i < 8; i++) {
      prof[i] = (char)base[profP + i];
      if (!prof[i]) break;
    }
  const uint32_t srcLen = ctx.r4.u32;
  const uint32_t outPP = ctx.r7.u32;
  const uint32_t callerLr = (uint32_t)ctx.lr;
  hlslNoteCoreEntry();   // step4/audit batch 3 SR_HLSL_COMPILE_WITNESS: cores= per compile
  __imp__sub_823B2D10(ctx, base);
  if (n < 90) {
    const uint32_t outP = outPP >= 0x1000u ? sr::kernel::readGuestU32BE(outPP) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[HLSLC] #%d prof=%s a2=%08X rc=%08X out=%08X lr=%08X",
        n + 1, prof[0] ? prof : "?", srcLen, ctx.r3.u32, outP, callerLr);
  }
}

// iter-398 [MPPRES]: the movie self-present loop (worker4). The whole render
// rides on v45 = renderer from *(player+44)->vt[+64] (NULL -> silent bail);
// success path draws via renderer vt[+80] (binds player[81/82] YUV shaders)
// and presents via D3DDeviceBlitAndSwap_sub_82112750. Does worker4 ENTER, and
// does the movie present ever fire?
extern "C" void __imp__sub_82481F40(PPCContext& __restrict ctx, uint8_t* base);
void sub_82481F40(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<int> s_n{0};
  const int n = s_n.fetch_add(1, std::memory_order_relaxed);
  if (n < 20)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[MPPRES] MoviePresentLoop ENTER #%d player=%08X lr=%08X",
        n + 1, ctx.r3.u32, (uint32_t)ctx.lr);
  __imp__sub_82481F40(ctx, base);
  if (n < 20)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MPPRES] MoviePresentLoop EXIT #%d",
                n + 1);
}

extern "C" void __imp__sub_82112750(PPCContext& __restrict ctx, uint8_t* base);
void sub_82112750(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x82112750);  // NATIVE N0 (scripts/gen_native_brackets.py)
  static std::atomic<int> s_n{0};
  const int n = s_n.fetch_add(1, std::memory_order_relaxed);
  if (n < 8 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[MPPRES] BlitAndSwap #%d dev=%08X lr=%08X",
        n + 1, ctx.r3.u32, (uint32_t)ctx.lr);
  __imp__sub_82112750(ctx, base);
}

// step4/audit batch 3 (2026-10-20) SR_HLSL_COMPILE_WITNESS=1 (OBSERVE-ONLY, default OFF; nothing is written to guest
// memory): per run-time HLSL compile, an [HLSL_COMPILE] line with the source hash, profile, flags, the #include
// files the effect preprocessor pulled in during it (name + a hash of the included text) and a hash of the output
// bytecode. SR_IMPORT_FIX_ATTRS's A/B compares them off vs on: EffectPreprocHandleInclude_sub_8236DD90 chooses WHERE
// an #include resolves with GetFileAttributesA (-1 or a folder -> retried under the includer's folder), the
// attribute query batch 3 changes, so the fix could change which file a compile includes. Layouts (IDA): the
// output ID3DXBuffer (CreateD3DXBufferReader_sub_82397998) = {vt 0x82036C80, refs, size +8, data +12}; the
// preprocessor's source stack top [pp+616] = FxPreprocSource {text begin +0, end +4, name +76, folder +80}.
namespace {
bool hlslWitnessOn() {
  static const bool v = [] { const char* e = std::getenv("SR_HLSL_COMPILE_WITNESS"); return e && e[0] == '1'; }();
  return v;
}
thread_local std::string t_hlslIncludes;   // this thread's compile in progress: "name#hash/len;" (deduplicated)
thread_local int t_hlslDepth = 0;
thread_local int t_hlslCores = 0;          // compiler-core runs on this thread (sub_823B2D10 entries)
bool hlslReadable(uint32_t va, uint32_t n) {
  if (va < 0x1000u || n == 0 || uint64_t(va) + n > 0x100000000ull) return false;
  if (!sr::kernel::isGuestVAInRange(va) || !sr::kernel::isGuestVAInRange(va + n - 1)) return false;
  const uint8_t* h = sr::kernel::guestVAToHost(va);
  return h && sr::kernel::hostRangeReadableCached(h, n);
}
uint32_t hlslRd32(uint32_t va) { return hlslReadable(va, 4) ? sr::kernel::readGuestU32BE(va) : 0; }
uint64_t hlslHash(uint32_t va, uint32_t n) {   // FNV-1a 64 of guest bytes; 0 = unreadable
  if (n == 0 || n > (8u << 20) || !hlslReadable(va, n)) return 0;
  const uint8_t* p = sr::kernel::guestVAToHost(va);
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint32_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}
std::string hlslCStr(uint32_t va, uint32_t cap = 160) {
  std::string s;
  for (uint32_t i = 0; i < cap && hlslReadable(va + i, 1); ++i) {
    const char c = char(*sr::kernel::guestVAToHost(va + i));
    if (!c) break;
    s += (c == ';' || c == '[' || c == ']' || uint8_t(c) < 0x20) ? '_' : c;
  }
  return s;
}
}  // namespace

void hlslNoteCoreEntry() { ++t_hlslCores; }

extern "C" void __imp__sub_8236DD90(PPCContext& __restrict ctx, uint8_t* base);
void sub_8236DD90(PPCContext& __restrict ctx, uint8_t* base) {   // EffectPreprocHandleInclude (#include directive)
  const uint32_t pp = ctx.r3.u32, lr = (uint32_t)ctx.lr;
  __imp__sub_8236DD90(ctx, base);
  if (!hlslWitnessOn()) return;
  const uint32_t rc = ctx.r3.u32;
  const uint32_t iface = hlslRd32(pp + 624), top = hlslRd32(pp + 616);
  const std::string tok = hlslCStr(hlslRd32(pp + 640));   // the #include's file-name token
  const uint32_t b = hlslRd32(top), e = hlslRd32(top + 4);
  const uint32_t len = e > b ? e - b : 0;
  const uint64_t h = rc < 0x80000000u ? hlslHash(b, len) : 0;
  const std::string name = hlslCStr(hlslRd32(top + 76)), folder = hlslCStr(hlslRd32(top + 80));
  char rec[64];
  std::snprintf(rec, sizeof rec, "#%016llX/%u;", (unsigned long long)h, len);
  const std::string key = tok + rec;
  if (t_hlslDepth > 0 && t_hlslIncludes.find(key) == std::string::npos) t_hlslIncludes += key;
  static std::atomic<uint32_t> s_n{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 512)
    SR_LOG_BOOT(sr::LogSeverity::Warn,   // Warn: the player log profile drops Info lines of unlisted tags
                "[HLSL_INCLUDE] #%u \"%s\" -> rc=%08X iface=%08X src=\"%s\" folder=\"%s\" text=%016llX/%u in_compile=%d "
                "lr=%08X", n, tok.c_str(), rc, iface, name.c_str(), folder.c_str(), (unsigned long long)h, len,
                t_hlslDepth, lr);
}

// iter-397q: the compare-backends wrapper (823B3370) — its lr names the
// shader-expression evaluator that successfully reaches the compiler for the
// fallback effect (and, by absence, where the 33 real effects diverge).
extern "C" void __imp__sub_823B3370(PPCContext& __restrict ctx, uint8_t* base);
void sub_823B3370(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<int> s_n{0};
  const int n = s_n.fetch_add(1, std::memory_order_relaxed);
  const uint32_t callerLr = (uint32_t)ctx.lr;
  // SR_HLSL_COMPILE_WITNESS: the inputs before the call (r3 / r4 source and length, r5 -> profile, r6 flags,
  // r7 -> the output buffer), the output after it.
  const bool w = hlslWitnessOn();
  const uint32_t srcVA = ctx.r3.u32, srcLen = ctx.r4.u32, profPP = ctx.r5.u32, flags = ctx.r6.u32, outPP = ctx.r7.u32;
  uint64_t srcHash = 0;
  std::string prof;
  std::string outerIncludes;
  const int coresBefore = t_hlslCores;
  const auto t0 = std::chrono::steady_clock::now();
  if (w) {
    srcHash = hlslHash(srcVA, srcLen);
    prof = hlslCStr(hlslRd32(profPP), 16);
    if (t_hlslDepth++ == 0) t_hlslIncludes.clear();
    else outerIncludes = t_hlslIncludes;
  }
  __imp__sub_823B3370(ctx, base);
  if (w) {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const uint32_t outP = hlslRd32(outPP);
    const uint32_t vt = hlslRd32(outP), size = hlslRd32(outP + 8), data = hlslRd32(outP + 12);
    const uint64_t outHash = (ctx.r3.u32 < 0x80000000u && vt == 0x82036C80u) ? hlslHash(data, size) : 0;
    static std::atomic<uint32_t> s_w{0};
    const uint32_t k = s_w.fetch_add(1, std::memory_order_relaxed) + 1;
    if (k <= 1024)
      SR_LOG_BOOT(sr::LogSeverity::Warn,   // Warn: the player log profile drops Info lines of unlisted tags
                  "[HLSL_COMPILE] #%u src=%016llX/%u prof=%s flags=%08X -> rc=%08X out=%016llX/%u vt=%08X "
                  "ms=%.2f cores=%d includes=[%s] lr=%08X", k, (unsigned long long)srcHash, srcLen, prof.c_str(), flags,
                  ctx.r3.u32, (unsigned long long)outHash, size, vt, ms, t_hlslCores - coresBefore,
                  t_hlslIncludes.c_str(), callerLr);
    if (--t_hlslDepth > 0) t_hlslIncludes = outerIncludes + t_hlslIncludes;
  }
  if (n < 90)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[HLSLW] #%d rc=%08X lr=%08X",
                n + 1, ctx.r3.u32, callerLr);
}

extern "C" void __imp__sub_823B38A8(PPCContext& __restrict ctx, uint8_t* base);
void sub_823B38A8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<int> s_n{0};
  const int n = s_n.fetch_add(1, std::memory_order_relaxed);
  const uint32_t callerLr = (uint32_t)ctx.lr;
  __imp__sub_823B38A8(ctx, base);
  if (n < 90)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[HLSLD] #%d rc=%08X lr=%08X",
                n + 1, ctx.r3.u32, callerLr);
}

// iter-397q [FXSH]: the two shader-bind record cases inside the effect pass
// applier sub_823EB3C0 (jump-table bodies). One is the VS bind, one the PS
// bind. The question the census left open: is the PS one EVER called during
// FMV, and with what? Entry-logged, capped.
// Decompiled (iter-397q): 820F5218 = D3DDevice_SetPixelShader(dev, psObj)
// (current-PS slot dev+12420, baked reg-patch table at obj+872/+892);
// 820F4E88 = D3DDevice_SetVertexShader(dev, vsObj) (slot dev+12416, table at
// obj+40/+60). A NULL obj clears the slot -> the draw flush emits NO IM_LOAD
// -> the GPU keeps executing resident ucode (the stale-2E37 symptom). Every
// sampled call so far had obj==NULL, so: log EVERY non-NULL bind uncapped
// (they should be rare) + sparse-sample the NULL ones.
extern "C" void __imp__sub_820F5218(PPCContext& __restrict ctx, uint8_t* base);
void sub_820F5218(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820F5218);  // NATIVE N0 (scripts/gen_native_brackets.py)
  static std::atomic<int> s_n{0}, s_nn{0};
  const uint32_t obj = ctx.r4.u32;
  if (obj != 0) {
    // iter-404: the 200-cap was hit inside a 0.9s window at boot, so the log
    // said nothing about whether the MOVIE's draw path binds a pixel shader at
    // all - the actual question behind "the YUV PS never reaches the GPU".
    // Keep the first 200 detailed, then sample forever so the movie window is
    // visible (probe-cap trap, third time this arc).
    const int m = s_nn.fetch_add(1, std::memory_order_relaxed);
    // iter-404c: SetPixelShader itself never reads ucode (it stores the object
    // into dev+12420 and replays the reg-patch block at obj+872/+892), so the
    // ucode pointer lives somewhere else in the object and its offset is the
    // unknown blocking the comparison against [SHADER_SEEN]. Dump a window of
    // the object once per distinct object (first 8 objects) and read the offset
    // off the dump rather than guessing it.
    if (obj >= 0x10000u) {
      static std::mutex s_dm;
      static std::vector<uint32_t> s_dumped;
      bool doDump = false;
      {
        std::lock_guard<std::mutex> lk(s_dm);
        if (s_dumped.size() < 64 &&
            std::find(s_dumped.begin(), s_dumped.end(), obj) == s_dumped.end()) {
          s_dumped.push_back(obj);
          doDump = true;
        }
      }
      if (doDump) {
        char line[512]; int off = 0;
        for (int i = 0; i < 24 && off < (int)sizeof(line) - 12; ++i)
          off += snprintf(line + off, sizeof(line) - off, "%08X ",
                          sr::kernel::readGuestU32BE(obj + (uint32_t)i * 4u));
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[FXSH_OBJ] obj=%08X lr=%08X w0..23: %s", obj, (uint32_t)ctx.lr, line);
        char l2[512]; int o2 = 0;
        for (int i = 216; i < 240 && o2 < (int)sizeof(l2) - 12; ++i)
          o2 += snprintf(l2 + o2, sizeof(l2) - o2, "%08X ",
                         sr::kernel::readGuestU32BE(obj + (uint32_t)i * 4u));
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[FXSH_OBJ] obj=%08X w216..239 (around the +872/+892 block): %s", obj, l2);
        // iter-404d: obj+68 looks like the ucode's GPU-physical address. Xenon
        // maps physical P to virtual 0x80000000|P, so read there and print the
        // first dwords: real Xenos ucode starts with a recognisable control
        // flow word, garbage means the offset guess is wrong. Printing the
        // ADDRESS is the point - the accepted-IM_LOAD side logs its address
        // too, so "was this program ever emitted" becomes a set comparison
        // with no hashing and no assumptions about the container format.
        // iter-404i: dump the CONTAINER, not just the ucode. ENGINE_MAP 6d:
        // the shader object carries a copy of the D3D9 compiled-shader
        // container (PS at +0x28, VS at +0x368) - 36B BE header, magic
        // 0x102A11xx at +0, virtualSize at +4, physicalSize at +8. XenosRecomp
        // consumes containers, we only had raw ucode, and none of the captured
        // movie shaders match the carved MENU corpus (0/10, iter-404h) - so
        // taking the container straight from guest memory beats synthesizing
        // one. Scan a bounded window for the magic rather than trusting a
        // single offset, because the PS/VS offsets differ and the object we
        // dumped through the SetPS hook carried its magic at +0x368.
        for (uint32_t off = 0; off <= 0x400u; off += 4u) {
          const uint32_t magic = sr::kernel::readGuestU32BE(obj + off);
          if ((magic & 0xFFFFFF00u) != 0x102A1100u) continue;
          const uint32_t cbase = obj + off;
          const uint32_t virtSz = sr::kernel::readGuestU32BE(cbase + 4u);
          const uint32_t physSz = sr::kernel::readGuestU32BE(cbase + 8u);
          const uint64_t total = (uint64_t)virtSz + (uint64_t)physSz;
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[FXSH_CONT] obj=%08X +0x%X magic=%08X virt=%u phys=%u total=%llu",
              obj, off, magic, virtSz, physSz, (unsigned long long)total);
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[FXSH_STAGE] via=820F5218 obj=%08X +0x%X magic=%08X bit0=%u",
              obj, off, magic, magic & 1u);
          if (total >= 36u && total <= (1u << 20)) {
            char cpath[192];
            std::snprintf(cpath, sizeof(cpath), "shader_containers/%08X_%08X.xpu",
                          obj, magic);
            if (FILE* cf = std::fopen(cpath, "wb")) {
              for (uint64_t i = 0; i < total; ++i) {
                const uint8_t* hb = sr::kernel::guestVAToHost(cbase + (uint32_t)i);
                const uint8_t byte = hb ? *hb : 0u;
                std::fwrite(&byte, 1, 1, cf);
              }
              std::fclose(cf);
            }
          }
          break;
        }
        const uint32_t phys = sr::kernel::readGuestU32BE(obj + 68u);
        const uint32_t uva  = 0x80000000u | (phys & 0x1FFFFFFFu);
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[FXSH_UCODE] obj=%08X phys=%08X va=%08X u[0..3]=%08X %08X %08X %08X",
            obj, phys, uva,
            sr::kernel::readGuestU32BE(uva), sr::kernel::readGuestU32BE(uva + 4),
            sr::kernel::readGuestU32BE(uva + 8), sr::kernel::readGuestU32BE(uva + 12));
      }
    }
    if (m < 200 || (m % 200) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[FXSH] SetPS #%d obj=%08X lr=%08X obj[0..3]=%08X %08X %08X %08X",
          m + 1, obj, (uint32_t)ctx.lr,
          sr::kernel::readGuestU32BE(obj), sr::kernel::readGuestU32BE(obj + 4),
          sr::kernel::readGuestU32BE(obj + 8),
          sr::kernel::readGuestU32BE(obj + 12));
  } else {
    const int n = s_n.fetch_add(1, std::memory_order_relaxed);
    if (n < 4 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[FXSH] SetPS(NULL) #%d lr=%08X",
                  n + 1, (uint32_t)ctx.lr);
  }
  // mem.resources: the outgoing vertex shader's header fence stamp (kernel/native_resource_stamps.hpp).
  const uint32_t resourceStamp = sr::kernel::resourceFenceStampTarget(ctx.r3.u32, sr::kernel::kDevVertexShaderSlot);
  __imp__sub_820F5218(ctx, base);
  sr::kernel::resourceFenceStampDone(resourceStamp);
}

extern "C" void __imp__sub_820F4E88(PPCContext& __restrict ctx, uint8_t* base);
void sub_820F4E88(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820F4E88);  // NATIVE N0 (scripts/gen_native_brackets.py)
  // iter-405: STAGE-IDENTITY TEST. Every container captured through
  // sub_820F5218 (named SetPixelShader by iter-397q) carried magic 102A1101 at
  // +0x368 - bit0=1 = VERTEX, at the VERTEX container offset. Either the two
  // bind functions are swapped in our notes or something stranger is going on,
  // and "~1800 PIXEL binds during the movie" rests on the answer. The magic's
  // bit0 is authoritative, so log it from THIS side too: whichever entry point
  // receives bit0=0 containers is the pixel one.
  {
    const uint32_t o = ctx.r4.u32;
    if (o >= 0x10000u) {
      static std::mutex s_im;
      static std::vector<uint32_t> s_ids;
      bool go = false;
      {
        std::lock_guard<std::mutex> lk(s_im);
        if (s_ids.size() < 64 &&
            std::find(s_ids.begin(), s_ids.end(), o) == s_ids.end()) {
          s_ids.push_back(o); go = true;
        }
      }
      if (go) {
        for (uint32_t off = 0; off <= 0x400u; off += 4u) {
          const uint32_t mg = sr::kernel::readGuestU32BE(o + off);
          if ((mg & 0xFFFFFF00u) != 0x102A1100u) continue;
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[FXSH_STAGE] via=820F4E88 obj=%08X +0x%X magic=%08X bit0=%u",
              o, off, mg, mg & 1u);
          // iter-405c: the earlier container dump ran only on the VERTEX hook,
          // so every .xpu captured so far is a VS container. The PIXEL
          // containers - which is where the YUV converter lives - are here, at
          // +0x28 on this entry point. Same header contract: virtualSize +4,
          // physicalSize +8.
          {
            const uint32_t cb = o + off;
            const uint32_t vsz = sr::kernel::readGuestU32BE(cb + 4u);
            const uint32_t psz = sr::kernel::readGuestU32BE(cb + 8u);
            const uint64_t tot = (uint64_t)vsz + (uint64_t)psz;
            if (tot >= 36u && tot <= (1u << 20)) {
              char cp[192];
              std::snprintf(cp, sizeof(cp), "shader_containers/ps_%08X_%08X.xpu",
                            o, mg);
              if (FILE* cf = std::fopen(cp, "wb")) {
                for (uint64_t i = 0; i < tot; ++i) {
                  const uint8_t* hb = sr::kernel::guestVAToHost(cb + (uint32_t)i);
                  const uint8_t by = hb ? *hb : 0u;
                  std::fwrite(&by, 1, 1, cf);
                }
                std::fclose(cf);
              }
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[FXSH_CONT] PS obj=%08X +0x%X virt=%u phys=%u total=%llu",
                  o, off, vsz, psz, (unsigned long long)tot);
              // iter-406 HYPOTHESIS: the container is SPLIT, which is what its
              // own virtualSize/physicalSize fields are telling us. The VIRTUAL
              // part (header + ctab) is copied inline at +0x28; the PHYSICAL
              // part (the ucode) lives separately, and ENGINE_MAP 6d says the
              // PS object keeps that pointer at +0x18. That explains iter-405e
              // exactly: reading virt+physOfs from the HEADER address walked
              // into adjacent object fields, because the payload was never
              // there to begin with.
              // ACCEPTANCE TEST: the bytes at obj+0x18 must byte-match an
              // IM_LOAD blob. Log the head so it can be checked against
              // shader_misses/ without trusting the reconstruction.
              const uint32_t ucVA = sr::kernel::readGuestU32BE(o + 0x18u);
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[FXSH_UC] PS obj=%08X ucodeVA=%08X phys=%u u[0..3]=%08X %08X %08X %08X",
                  o, ucVA, psz,
                  sr::kernel::readGuestU32BE(ucVA),
                  sr::kernel::readGuestU32BE(ucVA + 4u),
                  sr::kernel::readGuestU32BE(ucVA + 8u),
                  sr::kernel::readGuestU32BE(ucVA + 12u));
              // Rebuild a COMPLETE container: virtual part inline + physical
              // part from the pointer. If the hypothesis holds these translate
              // AND hash to what the runtime asks for, by construction.
              if (ucVA >= 0x10000u && psz && psz <= (1u << 20)) {
                char rp[192];
                std::snprintf(rp, sizeof(rp), "shader_rebuilt/ps_%08X.xpu", o);
                if (FILE* rf = std::fopen(rp, "wb")) {
                  for (uint32_t i = 0; i < vsz; ++i) {
                    const uint8_t* hb = sr::kernel::guestVAToHost(cb + i);
                    const uint8_t by = hb ? *hb : 0u;
                    std::fwrite(&by, 1, 1, rf);
                  }
                  for (uint32_t i = 0; i < psz; ++i) {
                    const uint8_t* hb = sr::kernel::guestVAToHost(ucVA + i);
                    const uint8_t by = hb ? *hb : 0u;
                    std::fwrite(&by, 1, 1, rf);
                  }
                  std::fclose(rf);
                }
              }
            }
          }
          break;
        }
      }
    }
  }
  static std::atomic<int> s_n{0}, s_nn{0};
  const uint32_t obj = ctx.r4.u32;
  if (obj != 0) {
    const int m = s_nn.fetch_add(1, std::memory_order_relaxed);
    if (m < 200)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[FXSH] SetVS #%d obj=%08X lr=%08X obj[0..3]=%08X %08X %08X %08X",
          m + 1, obj, (uint32_t)ctx.lr,
          sr::kernel::readGuestU32BE(obj), sr::kernel::readGuestU32BE(obj + 4),
          sr::kernel::readGuestU32BE(obj + 8),
          sr::kernel::readGuestU32BE(obj + 12));
  } else {
    const int n = s_n.fetch_add(1, std::memory_order_relaxed);
    if (n < 4 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[FXSH] SetVS(NULL) #%d lr=%08X",
                  n + 1, (uint32_t)ctx.lr);
  }
  // iter-405b: volume + timeline for the REAL pixel bind. iter-404 measured
  // this on the wrong entry point (820F5218 is SetVertexShader), so "does the
  // movie bind a pixel shader at all" is still unanswered. Same shape as the
  // other side: first 200 detailed, then sampled forever so the movie window is
  // never hidden behind a cap - that trap has now cost three separate readings
  // in this arc.
  {
    static std::atomic<int> s_pn{0}, s_pnull{0};
    const uint32_t po = ctx.r4.u32;
    if (po != 0) {
      const int pm = s_pn.fetch_add(1, std::memory_order_relaxed);
      if (pm < 200 || (pm % 200) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[FXPS] bind #%d obj=%08X lr=%08X", pm + 1, po, (uint32_t)ctx.lr);
    } else {
      const int pz = s_pnull.fetch_add(1, std::memory_order_relaxed);
      if (pz < 8 || (pz % 200) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[FXPS] bind(NULL) #%d lr=%08X", pz + 1, (uint32_t)ctx.lr);
    }
  }
  // mem.resources: the outgoing pixel shader's header fence stamp (kernel/native_resource_stamps.hpp).
  const uint32_t resourceStamp = sr::kernel::resourceFenceStampTarget(ctx.r3.u32, sr::kernel::kDevPixelShaderSlot);
  __imp__sub_820F4E88(ctx, base);
  sr::kernel::resourceFenceStampDone(resourceStamp);
}

extern "C" void __imp__sub_8235F488(PPCContext& __restrict ctx, uint8_t* base);
#endif

extern "C" void __imp__sub_820FDCB8(PPCContext& __restrict ctx, uint8_t* base);
// p2.transport RESIDUE IDLE (SR_NATIVE_IDLE_WAIT, default ON since the transport bake, 2026-10-15, =0 off; NATIVE_ROADMAP
// transport plan item 5 build note): BlockUntilIdle's WAIT_UNTIL as a record (sr::native::flush::idleWaitLeaf, a port of
// the generated body whose fence wait and dev+0x2A74 spin are the SDK's own). The same latched gate native_flush.cpp
// reads (the same expression), named here so the override registry attributes it to this override.
static bool residueIdleWaitGate() {
  static const bool on = [] {
    const char* e = std::getenv("SR_NATIVE_IDLE_WAIT");
    return !e || e[0] != '0';
  }();
  return on;
}
void sub_820FDCB8(PPCContext& __restrict ctx, uint8_t* base) {
  SR_NATIVE_SCOPE(0x820FDCB8);  // NATIVE N0 (scripts/gen_native_brackets.py)
#if SR_ORACLE
  static const bool s_trace = [] {
    const char* e = std::getenv("SR_PASS_TRACE");
    return e && e[0] == '1';
  }();
  if (s_trace)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FMV_POST_CREATE] GPU_WAIT enter dev=%08X lr=%08X",
        ctx.r3.u32, (uint32_t)ctx.lr);
  // NATIVE strand A (SR_NATIVE_IDLE_DIAG=1, diagnostic): how long each BlockUntilIdle holds the
  // calling thread, by guest caller - MoviePlayerRenderStateTick_sub_8235F488 calls it every tick
  // while an in-game movie plays (0.58 ms of the gameplay tick, PERFORMANCE 2.11).
  // C1: a development measurement, compiled out under SR_ORACLE=0 (kernel/sr_oracle.hpp; the gate then reads unset).
#if SR_ORACLE
  static const bool s_idleDiag = [] {
    const char* e = std::getenv("SR_NATIVE_IDLE_DIAG");
    return e && e[0] == '1';
  }();
  LARGE_INTEGER t0{};
#endif
  const uint32_t idleLr = (uint32_t)ctx.lr;
#if SR_ORACLE
  if (s_idleDiag) QueryPerformanceCounter(&t0);
#endif
  // iter-467 native-a P2 trace (observe-only): the whole call - its fence wait and the dev+0x2A74 drain spin at
  // 0x820FDD10-18 (un-hooked by release/forcing: the ISR / pump must drain it, or this call never returns).
  const bool p2tr = sr::native::p2trace::g_on;
  const uint32_t idleDev = ctx.r3.u32;
  LARGE_INTEGER q0{};
  if (p2tr) {
    QueryPerformanceCounter(&q0);
    sr::native::p2trace::event(sr::native::p2trace::kIdleBegin, 0,
                               idleDev ? sr::kernel::readGuestU32BE(idleDev + 0x2A74u) : 0u, idleLr, 0u);
  }
#endif
  if (!(residueIdleWaitGate() && sr::native::flush::idleWaitLeaf(ctx, base))) __imp__sub_820FDCB8(ctx, base);
  sr::native::flush::segTileDrained();  // SR_NATIVE_SEG_TILE's drained reconciliation (a no-op otherwise)
#if SR_ORACLE
  if (p2tr) {
    LARGE_INTEGER q1;
    QueryPerformanceCounter(&q1);
    const uint64_t dq = uint64_t(q1.QuadPart - q0.QuadPart);
    sr::native::p2trace::event(sr::native::p2trace::kIdleEnd, 0,
                               idleDev ? sr::kernel::readGuestU32BE(idleDev + 0x2A74u) : 0u, uint32_t(dq),
                               uint32_t(dq >> 32));
  }
#if SR_ORACLE
  if (s_idleDiag) {
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const double us = double(t1.QuadPart - t0.QuadPart) * 1e6 / double(f.QuadPart);
    struct Row { uint32_t lr; uint64_t n; double sum, max; };
    static std::mutex s_mu;
    static Row s_rows[16];
    static uint64_t s_total = 0;
    std::lock_guard<std::mutex> lk(s_mu);
    for (Row& r : s_rows) {
      if (r.n && r.lr != idleLr) continue;
      r.lr = idleLr;
      r.n++;
      r.sum += us;
      if (us > r.max) r.max = us;
      break;
    }
    if ((++s_total & 1023u) == 0) {
      std::string line;
      for (const Row& r : s_rows) {
        if (!r.n) continue;
        char b[96];
        std::snprintf(b, sizeof(b), " lr=%08X n=%llu mean=%.0fus max=%.0fus", r.lr, (unsigned long long)r.n,
                      r.sum / double(r.n), r.max);
        line += b;
      }
      SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_IDLE] BlockUntilIdle after %llu calls:%s",
                  (unsigned long long)s_total, line.c_str());
    }
  }
#endif
  if (s_trace)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FMV_POST_CREATE] GPU_WAIT return rc=%08X", ctx.r3.u32);
#endif
}
#if SR_ORACLE


extern "C" void __imp__sub_824709F0(PPCContext& __restrict ctx, uint8_t* base);
void sub_824709F0(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_trace = [] {
    const char* e = std::getenv("SR_PASS_TRACE");
    return e && e[0] == '1';
  }();
  if (s_trace)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FMV_POST_CREATE] GET_STATE enter player=%08X out=%08X lr=%08X",
        ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr);
  __imp__sub_824709F0(ctx, base);
  if (s_trace)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FMV_POST_CREATE] GET_STATE return rc=%08X", ctx.r3.u32);
}

extern "C" void __imp__sub_8235CA48(PPCContext& __restrict ctx, uint8_t* base);
void sub_8235CA48(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_trace = [] {
    const char* e = std::getenv("SR_PASS_TRACE");
    return e && e[0] == '1';
  }();
  if (s_trace)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FMV_POST_CREATE] DECODE_DRAW enter sm=%08X lr=%08X",
        ctx.r3.u32, (uint32_t)ctx.lr);
  __imp__sub_8235CA48(ctx, base);
  if (s_trace) {
    // iter-364: on E_FAIL (0x80004005), name WHICH of the two E_FAIL sites fired:
    //  line 18 = a null YUV texture (dword_829761E4/E8/EC); line 74 = the "tec0"
    //  technique lookup on the shader (dword_829761F8) returning null.
    auto R = [](uint32_t a){ return sr::kernel::readGuestU32BE(a); };
    uint32_t texY = R(0x829761E4), texU = R(0x829761E8), texV = R(0x829761EC);
    uint32_t shader = R(0x829761F8), player = R(0x829761D4);
    bool nullTex = (!texY || !texU || !texV);
    // iter-364: the effect at dword_829761F8. vt=*(effect); vt[52]=FindTechnique.
    // Dump the effect's vtable + FindTechnique addr + the first effect fields so we
    // can decompile the real lookup and see whether the technique table is empty.
    uint32_t vt = shader ? R(shader) : 0;
    // iter-397q: one more level — `vt` above is THIS (0x83A48E50-class); the
    // REAL vtable is *(this). Dump it + the slots RenderYuv actually calls
    // (+52 findTech, +240, +260 Begin, +264 BeginPass, +268 Commit).
    uint32_t vt2 = (vt >= 0x10000u) ? R(vt) : 0;
    if (vt2 >= 0x82000000u && vt2 < 0x82A00000u) {
      static std::atomic<int> s_vt2Log{0};
      if (s_vt2Log.fetch_add(1, std::memory_order_relaxed) < 2)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[FXVT2] this=%08X vtbl=%08X +52=%08X +240=%08X +260=%08X +264=%08X +268=%08X +272=%08X +276=%08X",
            vt, vt2, R(vt2 + 52), R(vt2 + 240), R(vt2 + 260), R(vt2 + 264),
            R(vt2 + 268), R(vt2 + 272), R(vt2 + 276));
      // iter-397q [FXBIND]: the shader-bind gates, live. sub_823EB240 binds
      // VS/PS ONLY when this+308 & 0x10000000; PS only when the pass program
      // record's u16@+34 != 0xFFFF; sub_823E44B8 further gates on the
      // technique record (196B stride @ this+196[this+94]) fields
      // +168/+176 (built) and max-counts +188/190/192.
      static std::atomic<int> s_fbLog{0};
      if (s_fbLog.fetch_add(1, std::memory_order_relaxed) < 4) {
        auto R16 = [&](uint32_t a) -> uint32_t {
          return (R(a & ~3u) >> ((2 - (a & 2)) * 8)) & 0xFFFFu;
        };
        const uint32_t flags308 = R(vt + 308);
        const uint32_t techIdx = R16(vt + 94);
        const uint32_t techBase = R(vt + 196);
        const uint32_t techRec = techBase + 196u * techIdx;
        const uint32_t passCount = techRec ? R(techRec + 56) : 0;
        const uint32_t passTable = techRec ? R(techRec + 60) : 0;
        const uint32_t pass0 = passTable;                 // 72B stride
        const uint32_t progIdx = pass0 ? R16(pass0 + 4) : 0;
        const uint32_t progBase = R(vt + 72);
        const uint32_t prog = progBase + 108u * progIdx;
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[FXBIND] flags308=%08X techIdx=%u techRec=%08X passes=%u "
            "pass0=%08X progIdx=%u prog=%08X vsIdx=%04X psIdx=%04X "
            "prog+12=%08X | tech+168=%08X +176=%08X",
            flags308, techIdx, techRec, passCount, pass0, progIdx, prog,
            prog ? R16(prog + 32) : 0xDEAD, prog ? R16(prog + 34) : 0xDEAD,
            prog ? R(prog + 12) : 0,
            techRec ? R(techRec + 168) : 0, techRec ? R(techRec + 176) : 0);
        // iter-397q [FXREC]: the pass's 32-byte STATE RECORDS (base=*(prog+8),
        // count=u16(prog+62)) — the shader-bind cases read holder=*(rec+26),
        // obj=*(holder+4). Every live SetVS/SetPS call had obj==NULL; this
        // names the record types + whether the holder or the object is the
        // missing link.
        const uint32_t recBase = prog ? R(prog + 8) : 0;
        const uint32_t recCount = prog ? R16(prog + 62) : 0;
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[FXREC] recBase=%08X count=%u",
                    recBase, recCount);
        for (uint32_t ri = 0; ri < recCount && ri < 64; ri++) {
          const uint32_t rec = recBase + 32u * ri;
          const uint32_t type = (R(rec + 8) >> 24) & 0xFFu;
          if (type < 2) continue;   // constant/state spam — shader records only
          const uint32_t holder = R(rec + 26);
          const uint32_t hObj = holder >= 0x10000u ? R(holder + 4) : 0;
          SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[FXREC] #%u type=%02X rec=%08X d=[%08X %08X %08X %08X %08X %08X"
              " %08X %08X] holder(+26)=%08X obj(h+4)=%08X",
              ri, type, rec, R(rec), R(rec + 4), R(rec + 8), R(rec + 12),
              R(rec + 16), R(rec + 20), R(rec + 24), R(rec + 28), holder, hObj);
          // The holder struct: +4 device shader obj (NULL live), +8 program
          // RESOURCE (mode-2 create input: res->vt[+0xC](res) realizes the
          // shader), +0x14 flags (bit30 gate). Dump it + the res vtable.
          if (holder >= 0x10000u) {
            const uint32_t res = R(holder + 8);
            const uint32_t resVt = res >= 0x10000u ? R(res) : 0;
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[FXREC]   holder[0..6]=%08X %08X %08X %08X %08X %08X %08X | "
                "res=%08X resVt=%08X realizeFn(vt+C)=%08X",
                R(holder), R(holder + 4), R(holder + 8), R(holder + 12),
                R(holder + 16), R(holder + 20), R(holder + 24),
                res, resVt,
                (resVt >= 0x82000000u && resVt < 0x82A00000u) ? R(resVt + 12)
                                                              : 0);
          }
        }
      }
    }
    uint32_t findTech = vt ? R(vt + 52) : 0;
    char ef[80]; int eo = 0; ef[0] = 0;
    for (int i = 0; i < 0x30 && eo < 72; i += 4) eo += snprintf(ef + eo, sizeof(ef)-eo, "%08X ", shader ? R(shader + i) : 0);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FMV_POST_CREATE] DECODE_DRAW return rc=%08X | texY=%08X texU=%08X texV=%08X shader=%08X player=%08X | site=%s | vt=%08X FindTechnique=%08X | eff[0..2f]=%s",
        ctx.r3.u32, texY, texU, texV, shader, player,
        ctx.r3.u32 == 0x80004005u ? (nullTex ? "NULL_TEXTURE(line18)" : "tec0_or_shader(line74)") : "-",
        vt, findTech, ef);
  }
}

// iter-364: THE effect-create probe (D3DXCreateEffect-equivalent, sub_823683F8).
// Oracle (golden dump): console builds the FMV effect object at holder[0]=0x4002FED0
// (vtable 0x82036CA0, ctor sub_82398470) in GPU-physical memory; ours leaves holder[0]=0
// so FindTechnique("tec0") dispatches through null -> E_FAIL. This hook answers
// reached-vs-failed: it logs EVERY effect-create with the blob magic/size, return code,
// and the output effect pointer (*a8). Args: a2=r4 (blob), a3=r5 (size), a8=r10 (out).
// If the FMV create never fires -> load-trigger gap; if it fires with rc<0 -> the parse
// (sub_82395DC8) / finalize (sub_82393C00) failed (they call into our GPU shader-create).
// iter-364 STEP2: token-trace arming. Set by the FXCREATE hook for the FIRST fallback
// compile (blob 82913F28 = static string aFloatGtime0Sta, whose exact HLSL we can read)
// so the yylex hook (sub_82387310) logs tokens for exactly that one parse. Only ever set
// under SR_PASS_TRACE, so the hot lexer path is free otherwise.
std::atomic<int>  g_fxLexArm{0};
std::atomic<bool> g_fxLexUsed{false};
std::atomic<uint32_t> g_fxRegionAddr{0};  // iter-364: live +104 preprocessor region (for alloc-overlap confirmation/fix)
extern "C" void __imp__sub_823683F8(PPCContext& __restrict ctx, uint8_t* base);
void sub_823683F8(PPCContext& __restrict ctx, uint8_t* base) {
  static const bool s_trace = [] {
    const char* e = std::getenv("SR_PASS_TRACE");
    return e && e[0] == '1';
  }();
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t blob = ctx.r4.u32, size = ctx.r5.u32, outp = ctx.r10.u32;
  uint32_t magic = blob ? sr::kernel::readGuestU32BE(blob) : 0;
  uint32_t lr = (uint32_t)ctx.lr;
  // iter-364: is the effect SOURCE truncated or fully present? Scan the blob for the
  // "technique" keyword + capture the last 24 printable bytes. If "technique" IS present
  // in the source but the compiler reports "no techniques" -> parser codegen bug; if the
  // blob is truncated (no "technique", tail mid-statement) -> upstream loading/alloc bug.
  int techCount = 0; uint32_t lastTechOff = 0; char tail[28]; tail[0] = 0;
  if (blob >= 0x1000u && blob < 0xC0000000u && size > 0 && size < 0x100000u) {
    const char* p = reinterpret_cast<const char*>(base + blob);
    for (uint32_t i = 0; i + 9 <= size; ++i)
      if (p[i]=='t'&&p[i+1]=='e'&&p[i+2]=='c'&&p[i+3]=='h'&&p[i+4]=='n'&&
          p[i+5]=='i'&&p[i+6]=='q'&&p[i+7]=='u'&&p[i+8]=='e') { ++techCount; lastTechOff = i; }
    uint32_t ts = size >= 24 ? size - 24 : 0;
    for (uint32_t i = 0; i < 24 && ts + i < size; ++i) {
      uint8_t ch = (uint8_t)p[ts + i];
      tail[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.'; tail[i+1] = 0;
    }
  }
  bool armedHere = false;
  if (s_trace && blob == 0x82913F28u && !g_fxLexUsed.exchange(true)) {
    g_fxLexArm.store(1, std::memory_order_relaxed); armedHere = true;
  }
  __imp__sub_823683F8(ctx, base);
  if (armedHere) g_fxLexArm.store(0, std::memory_order_relaxed);
  uint32_t rc = ctx.r3.u32;
  uint32_t eff = outp ? sr::kernel::readGuestU32BE(outp) : 0;
  uint32_t effVt = eff ? sr::kernel::readGuestU32BE(eff) : 0;
  if (s_trace || n <= 200)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FXCREATE] #%u lr=%08X blob=%08X size=%u magic=%08X -> rc=%08X eff=%08X | techInSrc=%d lastOff=%u tail=\"%s\"",
        n, lr, blob, size, magic, rc, eff, techCount, lastTechOff, tail);
  // iter-449z: a FAILED create's source is the root-cause artifact (the FE
  // post effects E_FAIL and fall back to the debug shader = the black screen).
  // Dump the exact HLSL so the failing construct can be read. First 8 only.
  if (rc != 0 && blob >= 0x1000u && size > 0 && size < 0x100000u) {
    static std::atomic<unsigned> s_dump{0};
    const unsigned dn = s_dump.fetch_add(1, std::memory_order_relaxed);
    if (dn < 8) {
      char nm[128];
      snprintf(nm, sizeof(nm), "archive/fx_fail_%u_%08X.hlsl", dn, blob);
      if (FILE* f = fopen(nm, "wb")) {
        fwrite(base + blob, 1, size, f);
        fclose(f);
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[FXCREATE] dumped %s", nm);
      }
    }
  }
}

// iter-364: localize the effect-compile E_FAIL. sub_82367DF8 (the HLSL source compiler)
// runs three stages: sub_8236DFC0 (parse) -> sub_8238EF00 (compile/build) ->
// sub_8237D028 (ID3DXEffectCompiler finalize, where malloc(0) was fixed). After the
// malloc(0) fix all 66 creates fail E_FAIL (0x80004005) one step later. These wrappers
// log each stage's return code so we see WHICH stage returns the E_FAIL. Gated
// SR_PASS_TRACE; log the first 200 of each.
#define SR_FXSTAGE_HOOK(FN, TAG)                                                        \
  extern "C" void __imp__##FN(PPCContext& __restrict ctx, uint8_t* base);               \
  void FN(PPCContext& __restrict ctx, uint8_t* base) {                                  \
    static std::atomic<unsigned> s_c{0};                                               \
    unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;                       \
    uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, lr = (uint32_t)ctx.lr;  \
    __imp__##FN(ctx, base);                                                             \
    uint32_t rc = ctx.r3.u32;                                                           \
    if (c <= 200)                                                                       \
      SR_LOG_BOOT(sr::LogSeverity::Info,                                                \
        "[FXSTAGE] " TAG " #%u in(r3=%08X r4=%08X r5=%08X) lr=%08X -> rc=%08X",         \
        c, a3, a4, a5, lr, rc);                                                         \
  }
SR_FXSTAGE_HOOK(sub_8236DFC0, "PARSE")
SR_FXSTAGE_HOOK(sub_8238EF00, "COMPILE")
// iter-398: FINALIZE gets a custom body — the driver calls the compiler's
// vt[+244] (CompileEffect) afterwards, and [FXSTAGE] proved sub_82374798 is
// NOT it (never fires while the produced blob feeds BUILDEFFECT). Name the
// REAL slot from the live vtable.
extern "C" void __imp__sub_8237D028(PPCContext& __restrict ctx, uint8_t* base);
void sub_8237D028(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32,
           lr = (uint32_t)ctx.lr;
  __imp__sub_8237D028(ctx, base);
  if (c <= 200) {
    const uint32_t vt = a3 >= 0x10000u ? sr::kernel::readGuestU32BE(a3) : 0;
    const uint32_t f244 =
        (vt >= 0x82000000u && vt < 0x82A00000u)
            ? sr::kernel::readGuestU32BE(vt + 244u) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[FXSTAGE] FINALIZE #%u in(r3=%08X r4=%08X r5=%08X) lr=%08X -> rc=%08X"
      " | vt=%08X vt+244=%08X",
      c, a3, a4, a5, lr, ctx.r3.u32, vt, f244);
  }
}
// iter-364: the 3 steps AFTER the compiler object finalizes (sub_823683F8):
//   vt+244 = sub_82374798  = ID3DXEffectCompiler::CompileEffect (source -> effect blob)
//   sub_82395DC8           = build ID3DXEffect from the compiled blob
//   sub_82393C00           = finalize the effect
// One of these holds the surviving E_FAIL (FXCREATE still 0/66 despite 37 finalizes).
SR_FXSTAGE_HOOK(sub_82374798, "COMPILEEFFECT")
SR_FXSTAGE_HOOK(sub_82395DC8, "BUILDEFFECT")
SR_FXSTAGE_HOOK(sub_82393C00, "FINEFFECT")
// iter-398: the REAL CompileEffect = vt+244 = sub_8237A318 (live vtable
// 0x8202B738; the sub_82374798 mapping was wrong). Its shader work: per-param
// CompileShaderParams (82378BF0) + the object-expression list at this+860 via
// CompileObjectExpr (823C9E08 — evaluates `compile ps_2_0 PS()`). If neither
// is ever called, the parser produced no shader objects (the iter-364
// token-pump EOF class) and CompileEffect succeeds vacuously.
SR_FXSTAGE_HOOK(sub_8237A318, "COMPILEFX")
SR_FXSTAGE_HOOK(sub_82378BF0, "SHPARAM")
#undef SR_FXSTAGE_HOOK

// iter-398b: CompileObjectExpr with OUTPUT inspection — args (ctx, this+16,
// scope, node, 0, flags|0x100, &outBuf, &outObj) = r3..r10; log *(&outBuf)
// and its size dword (+8) plus *(&outObj) after the call. All 122 calls
// return rc=0 yet every consumed object record has size 0.
extern "C" void __imp__sub_823C9E08(PPCContext& __restrict ctx, uint8_t* base);
void sub_823C9E08(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t outBufP = ctx.r9.u32, outObjP = ctx.r10.u32;
  const uint32_t flags = ctx.r8.u32;
  __imp__sub_823C9E08(ctx, base);
  if (c <= 140) {
    const uint32_t buf = outBufP >= 0x1000u ? sr::kernel::readGuestU32BE(outBufP) : 0;
    const uint32_t obj = outObjP >= 0x1000u ? sr::kernel::readGuestU32BE(outObjP) : 0;
    const uint32_t bufSz = buf >= 0x1000u ? sr::kernel::readGuestU32BE(buf + 8u) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[OBJEXPR2] #%u flags=%08X -> rc=%08X buf=%08X bufSz=%u obj=%08X",
        c, flags, ctx.r3.u32, buf, bufSz, obj);
  }
}

// iter-398b [SHPARM2]: EffectCreateShaderParam — the holder wiring. Reads the
// object SIZE from the blob object stream at cursor *a5; if 0, the whole
// container-copy + Realize{V,P}SResource is SILENTLY skipped (holder stays
// {obj=0,res=0}) and rc is still success. Log the size per call.
extern "C" void __imp__sub_82394110(PPCContext& __restrict ctx, uint8_t* base);
void sub_82394110(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t bufObj = ctx.r6.u32;   // a4 = object-stream buffer
  const uint32_t curP = ctx.r7.u32;     // a5 = &cursor (dwords)
  uint32_t cur = 0, sizeAtCur = 0, bufPtr = 0, bufSize = 0;
  if (curP >= 0x1000u) cur = sr::kernel::readGuestU32BE(curP);
  if (bufObj >= 0x1000u) {
    const uint32_t bvt = sr::kernel::readGuestU32BE(bufObj);
    if (bvt >= 0x82000000u && bvt < 0x82A00000u) {
      // ID3DXBuffer: vt+12 GetBufferPointer, vt+16 GetBufferSize — call-free
      // read: the buffer object layout is {vt, ptr(+4?), size(+8?)} — read
      // both candidates raw instead of calling guest code.
      bufPtr = sr::kernel::readGuestU32BE(bufObj + 4u);
      bufSize = sr::kernel::readGuestU32BE(bufObj + 8u);
      if (bufPtr >= 0x1000u)
        sizeAtCur = sr::kernel::readGuestU32BE(bufPtr + 4u * cur);
    }
  }
  __imp__sub_82394110(ctx, base);
  if (c <= 140)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[SHPARM2] #%u cur=%u objSize@cur=%u bufPtr=%08X bufSize=%u -> rc=%08X",
        c, cur, sizeAtCur, bufPtr, bufSize, ctx.r3.u32);
}

// iter-398c: level census inside the expr-compile driver. 823C8CC0 =
// HlslCompileShader (front-end + backend select); 827C1170 = the NEW compiler
// back end. Name which level runs for the 122 effect shader exprs and where
// the output dies.
extern "C" void __imp__sub_823C8CC0(PPCContext& __restrict ctx, uint8_t* base);
void sub_823C8CC0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t lr = (uint32_t)ctx.lr;
  // iter-398d: full-argument capture. Frame math from the 823C8CC0 asm
  // (r31 = entry_r1 - 0x250; stack args read at 0x2A4/0x2AC/0x2B4/0x2BC/
  // 0x2C4(r31)) puts the 64-bit-slot stack args at entry_r1 + 0x54 + 8*i:
  // [0]=profile(a9) [1]=flags(a10) [2]=outA/ppShader(a11) [3]=outB(a12)
  // [4]=x-options(a13).
  const uint32_t sp = ctx.r1.u32;
  const uint32_t a3 = ctx.r5.u32, a5 = ctx.r7.u32;
  const uint32_t a6 = ctx.r8.u32, a8 = ctx.r10.u32;
  // iter-449B: the iter-398d SR_HLSL_OLDBE band-aid is RETIRED (deleted per
  // its own retirement condition). The new backend's X4800 cascade + infinite
  // binder loop was never a game bug: our MmAllocatePhysicalMemoryEx stub and
  // the GPU disjoint alloc route dropped the XALLOC alignment attribute, so
  // the D3DX compiler's 4KB pool pages were 16B-aligned and its page-mask
  // header lookup (*(obj & 0xFFFFF000), sub_827D33B0) read garbage. With
  // guestPhysAlloc honoring alignment (SR_PHYS_ALIGN_FIX platform fix), all
  // runtime effects compile rc=0 on the NEW backend — the console's natural
  // path — including the inline-asm tfetch/vfetch and [unroll] sources the
  // old backend was masking. 33/33 FXCREATE rc=0 (2026-08-09 roll).
  // iter-398d A/B instrument (SR_HLSL_STUB=1, opt-in): replicate the deleted
  // ppc_recomp.51.cpp ANIM-INIT stub's net effect (rc=0, outputs untouched,
  // no compile) at the kernel-hook level — isolates whether the compile
  // storm itself is what perturbs the deep-load pipeline's boot race.
  static const bool s_stub = [] {
    const char* e = std::getenv("SR_HLSL_STUB");
    return e && e[0] == '1';
  }();
  if (s_stub) {
    ctx.r3.u64 = 0;
    return;
  }
  uint32_t sArg[5] = {0, 0, 0, 0, 0};
  for (int i = 0; i < 5; i++)
    sArg[i] = sr::kernel::readGuestU32BE(sp + 0x54u + 8u * (uint32_t)i);
  if (c <= 4)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[HLSLS] #%u ENTER a3=%08X a5=%08X a6=%08X a8=%08X flags=%08X",
                c, a3, a5, a6, a8, sArg[1]);
  __imp__sub_823C8CC0(ctx, base);
  uint32_t outAv = 0, outBv = 0;
  if (sArg[2] >= 0x1000u) outAv = sr::kernel::readGuestU32BE(sArg[2]);
  if (sArg[3] >= 0x1000u) outBv = sr::kernel::readGuestU32BE(sArg[3]);
  if (c <= 140)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[HLSLS] #%u lr=%08X a6=%08X a8=%08X prof=%08X flags=%08X "
                "outA=%08X->%08X outB=%08X->%08X xopt=%08X -> rc=%08X",
                c, lr, a6, a8, sArg[0], sArg[1], sArg[2], outAv, sArg[3],
                outBv, sArg[4], ctx.r3.u32);
}

// iter-398d: the two diagnostic sinks inside the effect/shader compiler.
// Every error/warning path in HlslCompileShader (823C8CC0) funnels through
// one of these with a unique caller lr + format string — the lr IS the
// control-flow trace the degraded decompile can't give us.
extern "C" void __imp__sub_823B5FC8(PPCContext& __restrict ctx, uint8_t* base);
void sub_823B5FC8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t lr = (uint32_t)ctx.lr;
  const uint32_t line = ctx.r5.u32, fmt = ctx.r6.u32;
  char txt[64] = {0};
  if (const uint8_t* p = sr::kernel::guestVAToHost(fmt)) {
    for (int i = 0; i < 60 && p[i]; i++)
      txt[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '.';
  }
  __imp__sub_823B5FC8(ctx, base);
  if (c <= 200)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[FXDIAG] warn #%u lr=%08X line=%u fmt=%08X \"%s\"",
                c, lr, line, fmt, txt);
}

extern "C" void __imp__sub_823B5F38(PPCContext& __restrict ctx, uint8_t* base);
void sub_823B5F38(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t lr = (uint32_t)ctx.lr;
  const uint32_t line = ctx.r5.u32, fmt = ctx.r6.u32;
  char txt[64] = {0};
  if (const uint8_t* p = sr::kernel::guestVAToHost(fmt)) {
    for (int i = 0; i < 60 && p[i]; i++)
      txt[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '.';
  }
  __imp__sub_823B5F38(ctx, base);
  if (c <= 200)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[FXDIAG] ERROR #%u lr=%08X line=%u fmt=%08X \"%s\"",
                c, lr, line, fmt, txt);
}

#endif

extern "C" void __imp__sub_827C1170(PPCContext& __restrict ctx, uint8_t* base);
void sub_827C1170(PPCContext& __restrict ctx, uint8_t* base) {
#if SR_ORACLE
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t lr = (uint32_t)ctx.lr;
#endif
  // iter-467: its setjmp(ctx+1040) error trap resumes on a guest longjmp (kernel/r1_platform.cpp)
  sr::r1::setjmpProtected(ctx, base, 0x827C1170u, __imp__sub_827C1170);
#if SR_ORACLE
  if (c <= 140)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[NEWBE] #%u lr=%08X -> rc=%08X",
                c, lr, ctx.r3.u32);
#endif
}
#if SR_ORACLE


// iter-398c2: inside 823C8CC0's old-backend flow — 823C6990 = the codegen
// (HlslCompileEntryFunction), 823CADC0 = AssembleShaderToBuffer (produces the
// final container into the out-arg). rc + counts localize where output dies.
extern "C" void __imp__sub_823C6990(PPCContext& __restrict ctx, uint8_t* base);
void sub_823C6990(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  __imp__sub_823C6990(ctx, base);
  if (c <= 100)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[CGEN] #%u -> rc=%08X", c, ctx.r3.u32);
}

extern "C" void __imp__sub_823CADC0(PPCContext& __restrict ctx, uint8_t* base);
void sub_823CADC0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  __imp__sub_823CADC0(ctx, base);
  if (c <= 100)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[ASMBUF] #%u -> rc=%08X", c, ctx.r3.u32);
}

// iter-398d: the NEW back end's real error sink sub_827C0420(ctx, code, va).
// The restored HlslCompileShader hangs inside the backend with error codes
// 3502/4502/3541 frozen in registers — log the error sequence to see what
// the backend objects to and whether the sink returns or loops.
extern "C" void __imp__sub_827C0420(PPCContext& __restrict ctx, uint8_t* base);
void sub_827C0420(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t lr = (uint32_t)ctx.lr;
  const uint32_t code = ctx.r4.u32;
  if (c <= 60 || (c % 4096) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[BEERR] #%u code=%u lr=%08X in",
                c, code, lr);
  __imp__sub_827C0420(ctx, base);
  if (c <= 60 || (c % 4096) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[BEERR] #%u code=%u out rc=%08X",
                c, code, ctx.r3.u32);
}

// iter-449A2: the varargs error logger sub_827C0680(ctx, code, ...) — its lr
// is the EXACT error site (the [BEERR] sink always sees lr=827C06B8, i.e.
// LogErrorVarargs itself). Also dump ctx[133] (interpolator-registry count,
// cap 16 -> X4800) and ctx[3] (function-row table) at error time. The FIRST
// error of the compile names the true defect; everything after is cascade.
extern "C" void __imp__sub_827C0680(PPCContext& __restrict ctx, uint8_t* base);
void sub_827C0680(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  if (c <= 60 || (c % 8192) == 0) {
    const uint32_t bectx = ctx.r3.u32, code = ctx.r4.u32,
                   lr = (uint32_t)ctx.lr;
    const uint32_t interpCnt =
        bectx >= 0x1000u ? sr::kernel::readGuestU32BE(bectx + 532u) : 0;
    const uint32_t rowTbl =
        bectx >= 0x1000u ? sr::kernel::readGuestU32BE(bectx + 12u) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[BESITE] #%u code=%u lr=%08X ctx=%08X interpCnt=%u rowTbl=%08X",
        c, code, lr, bectx, interpCnt, rowTbl);
  }
  __imp__sub_827C0680(ctx, base);
}

// iter-449A3: the FIRST X4800 fires inside sub_82836128 (site 82836184):
// `if (BYTE3(row.w3) && !(row.w1 & 2)) LogError(4800)` — our row 78 has
// w1=0x00010038 (bit1 CLEAR). Log every call's lr (names which pass calls
// it: 82836600 / 82837978 / 82838710x4) + rowIdx + row w0..w3 + a4 range.
extern "C" void __imp__sub_82836128(PPCContext& __restrict ctx, uint8_t* base);
void sub_82836128(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  if (c <= 40) {
    const uint32_t bectx = ctx.r3.u32, rowIdx = ctx.r5.u32,
                   lr = (uint32_t)ctx.lr;
    const uint32_t srcP = ctx.r6.u32, start = ctx.r7.u32, cnt = ctx.r8.u32;
    const uint32_t tbl =
        bectx >= 0x1000u ? sr::kernel::readGuestU32BE(bectx + 12u) : 0;
    uint32_t w[4] = {0, 0, 0, 0};
    if (tbl >= 0x1000u)
      for (uint32_t i = 0; i < 4u; i++)
        w[i] = sr::kernel::readGuestU32BE(tbl + 40u * rowIdx + 4u * i);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[NBEMIT] #%u lr=%08X row=%u src=%08X start=%u cnt=%u "
        "w={%08X %08X %08X %08X}",
        c, lr, rowIdx, srcP, start, cnt, w[0], w[1], w[2], w[3]);
    // iter-449A5: w3 -> the entity descriptor; dump it + its name string
    if (w[3] >= 0x1000u) {
      uint32_t d[8];
      for (uint32_t i = 0; i < 8u; i++)
        d[i] = sr::kernel::readGuestU32BE(w[3] + 4u * i);
      // iter-449A7: name chase — the row-name resolver reads a kind-3 decl
      // node's +24 string; probe d[5] and d[6] as candidate nodes.
      char nm[48] = {0};
      for (int k = 0; k < 2 && !nm[0]; k++) {
        const uint32_t node = d[5 + k];
        if (node < 0x1000u) continue;
        const uint32_t kind = sr::kernel::readGuestU32BE(node + 4u);
        const uint32_t nmP = sr::kernel::readGuestU32BE(node + 24u);
        if (kind == 3u && nmP >= 0x1000u) {
          if (const uint8_t* p = sr::kernel::guestVAToHost(nmP))
            for (int i = 0; i < 44 && p[i]; i++)
              nm[i] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '.';
        }
      }
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[NBEMIT] #%u   desc={%08X %08X %08X %08X %08X %08X %08X %08X} "
          "name=\"%s\"",
          c, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], nm);
    }
  }
  __imp__sub_82836128(ctx, base);
}

// iter-449A9: worklist forensics. The mark-pass BFS never pops the asm node
// though it hangs on exactly the field case-12 pushes; roots DID get marked
// (pre-pop-loop). Suspect: the chunked tagged-pointer worklist ring
// (sub_82830C00 grow / sub_827D30C8 release) or the pop math in 82840D00
// itself (codegen). Log grow/release with caller lr + chunk state.
extern "C" void __imp__sub_82830C00(PPCContext& __restrict ctx, uint8_t* base);
void sub_82830C00(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t ring = ctx.r3.u32, want = ctx.r4.u32,
                 lr = (uint32_t)ctx.lr;
  __imp__sub_82830C00(ctx, base);
  if (c <= 40 || (c % 4096) == 0) {
    const uint32_t chunk = ctx.r3.u32;
    const uint32_t used =
        chunk >= 0x1000u ? sr::kernel::readGuestU32BE(chunk + 8u) : 0;
    const uint32_t cap =
        chunk >= 0x1000u ? sr::kernel::readGuestU32BE(chunk + 12u) : 0;
    const uint32_t l0 = ring >= 0x1000u ? sr::kernel::readGuestU32BE(ring) : 0;
    const uint32_t l1 =
        ring >= 0x1000u ? sr::kernel::readGuestU32BE(ring + 4u) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[NBGROW] #%u lr=%08X ring=%08X want=%u -> chunk=%08X used=%u cap=%u "
        "ring0=%08X ring1=%08X",
        c, lr, ring, want, chunk, used, cap, l0, l1);
  }
}

// iter-449A6: the stale-flag discriminator. Row 88 = a Xenos asm-register
// pseudo-var (eA/eM0-4/a0/prev_scalar/lodgradientX-Y...) registered by
// sub_82840B60, which is GATED on ctx flag 0x40000 ("already registered") —
// on a ctx REUSED across compiles. Log setup entry state (tbl/count/flags)
// and every sub_82840B60 run; if the wedge compile enters with 0x40000 SET
// but a fresh table, the skip is the root cause.
extern "C" void __imp__sub_828433F8(PPCContext& __restrict ctx, uint8_t* base);
void sub_828433F8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t bectx = ctx.r3.u32;
  if (c <= 30 && bectx >= 0x1000u) {
    const uint32_t tbl = sr::kernel::readGuestU32BE(bectx + 12u);
    const uint32_t cnt = sr::kernel::readGuestU32BE(bectx + 16u);
    const uint32_t fl = sr::kernel::readGuestU32BE(bectx + 36u);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[NBSETUP] #%u ctx=%08X tbl=%08X cnt=%u flags=%08X bit40000=%d",
        c, bectx, tbl, cnt, fl, (fl & 0x40000u) ? 1 : 0);
  }
  __imp__sub_828433F8(ctx, base);
  if (c <= 30 && bectx >= 0x1000u) {
    const uint32_t tbl = sr::kernel::readGuestU32BE(bectx + 12u);
    const uint32_t cnt = sr::kernel::readGuestU32BE(bectx + 16u);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[NBSETUP] #%u post tbl=%08X cnt=%u", c, tbl, cnt);
  }
}

extern "C" void __imp__sub_82840B60(PPCContext& __restrict ctx, uint8_t* base);
void sub_82840B60(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t bectx = ctx.r3.u32;
  const uint32_t cntIn =
      bectx >= 0x1000u ? sr::kernel::readGuestU32BE(bectx + 16u) : 0;
  __imp__sub_82840B60(ctx, base);
  if (c <= 30) {
    const uint32_t tbl =
        bectx >= 0x1000u ? sr::kernel::readGuestU32BE(bectx + 12u) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[NBREGS] #%u ctx=%08X cntIn=%u newTbl=%08X (asm-reg rows %u..%u)",
        c, bectx, cntIn, tbl, cntIn + 1u, cntIn + 12u);
  }
}

// iter-449A4: sub_82840D00 = the mark pass that SETS row.w1 bit1 (the bit
// whose absence fires the first X4800): for each fn node i in ctx[137] with
// a2 != i (non-entry), every kind-6 input var's owning row gets w1|=2. Dump
// the fn list + each var's row idx pre-call, and the touched rows' w1 post.
extern "C" void __imp__sub_82840D00(PPCContext& __restrict ctx, uint8_t* base);
void sub_82840D00(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t bectx = ctx.r3.u32, a2 = ctx.r4.u32;
  uint32_t rowsSeen[12] = {0};
  uint32_t nRows = 0;
  if (c <= 16 && bectx >= 0x1000u) {
    const uint32_t tbl = sr::kernel::readGuestU32BE(bectx + 12u);
    uint32_t i = sr::kernel::readGuestU32BE(bectx + 137u * 4u);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[NBMARK] #%u ctx=%08X a2=%08X fnList=%08X tbl=%08X",
                c, bectx, a2, i, tbl);
    for (uint32_t k = 0; k < 8u && i >= 0x1000u; k++) {
      const uint32_t fn = sr::kernel::readGuestU32BE(i + 4u);
      uint32_t vl = fn >= 0x1000u ? sr::kernel::readGuestU32BE(fn + 36u) : 0;
      char rowsTxt[96];
      int off = 0;
      for (uint32_t m = 0; m < 8u && vl >= 0x1000u; m++) {
        const uint32_t inner = sr::kernel::readGuestU32BE(vl + 8u);
        if (inner >= 0x1000u &&
            sr::kernel::readGuestU32BE(inner + 4u) == 6u) {
          const uint32_t vdesc = sr::kernel::readGuestU32BE(inner + 24u);
          const uint32_t rowIdx =
              vdesc >= 0x1000u ? sr::kernel::readGuestU32BE(vdesc + 16u) : 0xFFFFu;
          if (nRows < 12u) rowsSeen[nRows++] = rowIdx;
          if (off < 80)
            off += snprintf(rowsTxt + off, sizeof(rowsTxt) - (size_t)off,
                            "%u ", rowIdx);
        }
        vl = sr::kernel::readGuestU32BE(vl + 12u);
      }
      rowsTxt[off] = 0;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NBMARK] #%u   fnNode=%08X fn=%08X isEntry=%d varRows=[%s]",
                  c, i, fn, (a2 == i) ? 1 : 0, rowsTxt);
      // iter-449A8: entry body statement chain — kinds + link candidates.
      // An unhandled kind in the mark-pass BFS switch silently drops its
      // children; the first kind outside {1,12,13,23,25} names the break.
      if (a2 == i) {
        uint32_t body = fn >= 0x1000u ? sr::kernel::readGuestU32BE(fn + 40u) : 0;
        for (uint32_t s = 0; s < 24u && body >= 0x1000u; s++) {
          const uint32_t k4 = sr::kernel::readGuestU32BE(body + 4u);
          const uint32_t l3 = sr::kernel::readGuestU32BE(body + 12u);
          const uint32_t child = sr::kernel::readGuestU32BE(body + 8u);
          const uint32_t ck =
              child >= 0x1000u ? sr::kernel::readGuestU32BE(child + 4u) : 0;
          const uint32_t c7 =
              child >= 0x1000u ? sr::kernel::readGuestU32BE(child + 28u) : 0;
          SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[NBBODY] #%u s%u node=%08X kind=%u child=%08X "
                      "childKind=%u child+28=%08X",
                      c, s, body, k4, child, ck, c7);
          body = l3;
        }
      }
      i = sr::kernel::readGuestU32BE(i + 8u);
    }
  }
  __imp__sub_82840D00(ctx, base);
  if (c <= 16 && bectx >= 0x1000u) {
    const uint32_t tbl = sr::kernel::readGuestU32BE(bectx + 12u);
    for (uint32_t k = 0; k < nRows; k++) {
      if (tbl < 0x1000u || rowsSeen[k] >= 0x4000u) continue;
      const uint32_t w1 =
          sr::kernel::readGuestU32BE(tbl + 40u * rowsSeen[k] + 4u);
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[NBMARK] #%u   post row%u w1=%08X bit1=%d",
                  c, rowsSeen[k], w1, (w1 & 2u) ? 1 : 0);
    }
  }
}

// iter-449A: new-backend hollow-row discriminator. Live Frida on the wedge
// proved the binder (828635F8) reads all-zero function-param rows (name ptr
// NULL -> _strnicmp invalid-parameter errno storm; flags missing 0x2000000 ->
// X4502; type count *(row[4]+44)==0 -> X3502). These observe-only hooks dump
// the row as first READ (8285C028), the type-object ctor args (827E7408 copies
// *(row[4]+44) verbatim into obj[2]), and the AST-side input-var descriptors
// the walker (82837348) lowers — discriminating "row never written" from
// "row written wrong" and "AST side empty too".
extern "C" void __imp__sub_8285C028(PPCContext& __restrict ctx, uint8_t* base);
void sub_8285C028(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  if (c <= 48) {
    const uint32_t bectx = ctx.r3.u32, rowIdx = ctx.r4.u32, pIdx = ctx.r5.u32;
    const uint32_t tbl = bectx >= 0x1000u ? sr::kernel::readGuestU32BE(bectx + 12u) : 0;
    const uint32_t row = tbl >= 0x1000u ? tbl + 40u * rowIdx : 0;
    uint32_t w0 = 0, w1 = 0, desc = 0, d44 = 0, typIdx = 0, cache = 0, flagsA = 0;
    uint32_t f[4] = {0, 0, 0, 0};
    if (row >= 0x1000u) {
      w0 = sr::kernel::readGuestU32BE(row);
      w1 = sr::kernel::readGuestU32BE(row + 4u);
      desc = sr::kernel::readGuestU32BE(row + 16u);
      typIdx = sr::kernel::readGuestU32BE(row + 20u);
      cache = sr::kernel::readGuestU32BE(row + 24u);
      flagsA = sr::kernel::readGuestU32BE(row + 28u);
      if (desc >= 0x1000u) d44 = sr::kernel::readGuestU32BE(desc + 44u);
      if (flagsA >= 0x1000u)
        for (uint32_t i = 0; i < 4u; i++)
          f[i] = sr::kernel::readGuestU32BE(flagsA + 4u * i);
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[NBROW] #%u row=%u p=%u w0=%08X(cnt=%u) w1=%08X desc=%08X d44=%u "
        "typ=%08X cache=%08X flagsA=%08X f={%08X %08X %08X %08X}",
        c, rowIdx, pIdx, w0, (w0 >> 3) & 0x3FFFu, w1, desc, d44,
        typIdx, cache, flagsA, f[0], f[1], f[2], f[3]);
  }
  __imp__sub_8285C028(ctx, base);
}

extern "C" void __imp__sub_827E7408(PPCContext& __restrict ctx, uint8_t* base);
void sub_827E7408(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t a2 = ctx.r4.u32, a3 = ctx.r5.u32, a4 = ctx.r6.u32;
  __imp__sub_827E7408(ctx, base);
  if (c <= 48)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[NBTYP] #%u typeIdx=%08X count=%u a4=%08X -> obj=%08X",
        c, a2, a3, a4, ctx.r3.u32);
}

extern "C" void __imp__sub_82837348(PPCContext& __restrict ctx, uint8_t* base);
void sub_82837348(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  if (c <= 24) {
    const uint32_t a2 = ctx.r4.u32;
    const uint32_t scope = a2 >= 0x1000u ? sr::kernel::readGuestU32BE(a2 + 4u) : 0;
    uint32_t node = scope >= 0x1000u ? sr::kernel::readGuestU32BE(scope + 36u) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[NBVAR] #%u scope=%08X head=%08X",
                c, scope, node);
    for (uint32_t n = 0; n < 12u && node >= 0x1000u; n++) {
      const uint32_t kind = sr::kernel::readGuestU32BE(node + 4u);
      const uint32_t inner = sr::kernel::readGuestU32BE(node + 8u);
      uint32_t ikind = 0, varDesc = 0, vIdx = 0, vCnt = 0, vFlags = 0;
      if (inner >= 0x1000u) {
        ikind = sr::kernel::readGuestU32BE(inner + 4u);
        varDesc = sr::kernel::readGuestU32BE(inner + 24u);
        if (varDesc >= 0x1000u) {
          vIdx = sr::kernel::readGuestU32BE(varDesc + 16u);
          vCnt = sr::kernel::readGuestU32BE(varDesc + 20u);
          vFlags = sr::kernel::readGuestU32BE(varDesc + 28u);
        }
      }
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[NBVAR] #%u   n%u node=%08X kind=%u ikind=%u vdesc=%08X "
          "vIdx=%u vCnt=%u vFlags=%08X",
          c, n, node, kind, ikind, varDesc, vIdx, vCnt, vFlags);
      node = sr::kernel::readGuestU32BE(node + 12u);
    }
  }
  __imp__sub_82837348(ctx, base);
}

// iter-398b: the two realize functions — if these never fire while SHPARM2
// rc=0, every object record's size was 0 (the silent-skip path). Also try
// reading the inline buffer data at bufObj+16 (layout {vt, refcnt=2, size,
// ?, data...}) for the size dword.
extern "C" void __imp__sub_8238F948(PPCContext& __restrict ctx, uint8_t* base);
void sub_8238F948(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<int> s_n{0};
  const int n = s_n.fetch_add(1, std::memory_order_relaxed);
  if (n < 80)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[SHPARM2] RealizeVS #%d holder=%08X",
                n + 1, ctx.r4.u32);
  __imp__sub_8238F948(ctx, base);
}

extern "C" void __imp__sub_8238F9E0(PPCContext& __restrict ctx, uint8_t* base);
void sub_8238F9E0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<int> s_n{0};
  const int n = s_n.fetch_add(1, std::memory_order_relaxed);
  if (n < 80)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[SHPARM2] RealizePS #%d holder=%08X",
                n + 1, ctx.r4.u32);
  __imp__sub_8238F9E0(ctx, base);
}

// iter-364: surface the ACTUAL effect-compiler error text. The FXLite compiler records
// every failure via sub_82371C10(errBuf, 0, 0, "ID3DXEffectCompiler: <reason>") — r6 is
// the message string. Logging it tells us WHY the compile fails (parse-tree error vs a
// real HLSL feature the compiler rejects vs a codegen bug) directly, instead of guessing
// from return codes. Gated SR_PASS_TRACE; first 120.
// iter-364: does the parser ever reach the technique-build action? sub_8237CC30 is
// called from the compiler ctor when a parse node is a technique (v19==3). If it NEVER
// fires during a compile -> the parser/lexer never produces a technique node (upstream
// recognition bug); if it fires but "no techniques" still errors -> the link to a1+812
// fails. sub_8237BB78 (v19==1) and sub_82379A30 (v19<3) are the other node handlers —
// hook all three to see which node types the parser actually emits.
#define SR_FXNODE_HOOK(FN, TAG)                                                         \
  extern "C" void __imp__##FN(PPCContext& __restrict ctx, uint8_t* base);               \
  void FN(PPCContext& __restrict ctx, uint8_t* base) {                                  \
    static std::atomic<unsigned> s_c{0};                                               \
    unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;                       \
    uint32_t a3 = ctx.r3.u32, lr = (uint32_t)ctx.lr;                                    \
    __imp__##FN(ctx, base);                                                             \
    if (c <= 200)                                                                       \
      SR_LOG_BOOT(sr::LogSeverity::Warn,                                                \
        "[FXNODE] " TAG " #%u a1=%08X lr=%08X -> rc=%08X", c, a3, lr, ctx.r3.u32);       \
  }
SR_FXNODE_HOOK(sub_8237CC30, "TECHNIQUE")
SR_FXNODE_HOOK(sub_8237BB78, "NODE1")
SR_FXNODE_HOOK(sub_82379A30, "NODE2")
#undef SR_FXNODE_HOOK

// iter-364 ARENA PROBE gate (used by [MSTATE3] in the sub_82469B48 hook, [NTAVM] in stubs.cpp,
// and [HTRACE] in guest_context.cpp). All gated SR_ARENA_TRACE (off by default = zero cost);
// reusable heap-overlap tracing. The one-shot [SEGADD]/[MSTATE]/[MSTATE2] investigation hooks
// were removed after the root was proven (guest dlmalloc chunk on the fake mstate 0x82110000,
// fenced by SR_DLMALLOC_FENCE). Keep this flag for future arena investigations.
static const bool s_arenaProbe = [] {
  const char* e = std::getenv("SR_ARENA_TRACE");
  return e && e[0] == '1';
}();

// iter-364 STEP2: yylex token trace. sub_82387310 is the scanner (called by yyparse
// sub_8238DC90 with scanner=*(parser+3032); returns the token id in r3, single-char
// tokens = their ASCII value, named tokens >= 256). Scoped to the ONE fallback compile
// (armed by the FXCREATE hook, blob 82913F28) whose exact HLSL source we know, so the
// first token that diverges from the known-correct stream fingers lexer-codegen (wrong
// token) vs parser-reduce (right tokens, wrong tree) AND names the exact construct.
namespace sr { namespace kernel { bool armHwWriteWatchHost(void* hostAddr); void disarmHwWriteWatch(); } }
extern "C" void __imp__sub_82387310(PPCContext& __restrict ctx, uint8_t* base);
void sub_82387310(PPCContext& __restrict ctx, uint8_t* base) {
  bool on = g_fxLexArm.load(std::memory_order_relaxed) != 0;
  uint32_t scan = ctx.r3.u32;
  // bracket the +104 region-counter across the yylex call. compiler = scan+4 (=8B3F0340);
  // region = *(compiler+0x68); counter = *(region+8). If it drops 1->0 within this yylex,
  // the decrementer is in the yylex/its callees; if it's already dropped between yylex
  // calls, it's in yyparse's reduce action.
  auto cntr = [&]() -> uint32_t {
    if (scan < 0x1000u) return 0xEEEE;
    uint32_t comp = sr::kernel::readGuestU32BE(scan + 4u);
    uint32_t reg = comp >= 0x1000u ? sr::kernel::readGuestU32BE(comp + 0x68u) : 0;
    return reg >= 0x1000u ? sr::kernel::readGuestU32BE(reg + 8u) : 0xDDDD;
  };
  auto cntrVA = [&]() -> uint32_t {
    if (scan < 0x1000u) return 0;
    uint32_t comp = sr::kernel::readGuestU32BE(scan + 4u);
    uint32_t reg = comp >= 0x1000u ? sr::kernel::readGuestU32BE(comp + 0x68u) : 0;
    return reg >= 0x1000u ? reg + 8u : 0;
  };
  uint32_t cbBefore = on ? cntr() : 0;
  __imp__sub_82387310(ctx, base);
  if (!on) return;
  uint32_t cbAfter = cntr();
  int tok = (int)ctx.r3.u32;
  // iter-364 STEP3: Dr0 HW write-watch on the region-counter (race-free, names the storing
  // guest PC). Arm once while the counter is still >0; the yyparse reduce action that drops
  // it 1->0 traps -> [HWWATCH] guestPC = the miscompiled/errant decrement site.
  {
    static std::atomic<bool> s_armed{false};
    uint32_t cva = cntrVA();
    if (!s_armed.load(std::memory_order_relaxed) && cbAfter != 0 && cva >= 0x1000u) {
      if (sr::kernel::armHwWriteWatchHost(base + cva)) {
        s_armed.store(true, std::memory_order_relaxed);
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[FXWATCH] armed Dr0 on counter VA=%08X", cva);
      }
    }
    if (tok < 0 && s_armed.load(std::memory_order_relaxed)) {
      sr::kernel::disarmHwWriteWatch();
      s_armed.store(false, std::memory_order_relaxed);
    }
  }
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  if (c > 900) return;
  char tc = (tok >= 32 && tok < 127) ? (char)tok : '.';
  // flex cursor yy_c_buf_p is commonly at scanner+0xC and points just PAST the matched
  // token; show 22 printable bytes ending at the cursor. The first-6 struct-head dump
  // validates the offset (if 0xC is wrong, the dump reveals which field advances).
  char lx[28]; lx[0] = 0;
  uint32_t cur = (scan >= 0x1000u) ? sr::kernel::readGuestU32BE(scan + 0x0Cu) : 0;
  if (cur >= 0x1000u + 22u && cur < 0xC0000000u) {
    const uint8_t* p = base + (cur - 22u);
    for (int i = 0; i < 22; ++i) { uint8_t ch = p[i]; lx[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.'; }
    lx[22] = 0;
  }
  // On the FIRST and the EOF call, dump a big slice of the scanner struct to locate the
  // input-END pointer (expect 82913F38 = src+16, the truncation) and any n_chars=16 field.
  if (c == 1 || tok < 0) {
    for (int row = 0; row < 4; ++row) {
      char sd[220]; int o = 0;
      for (int k = 0; k < 12 && o < 200; ++k)
        o += std::snprintf(sd + o, sizeof(sd) - o, "%08X ",
                           scan >= 0x1000u ? sr::kernel::readGuestU32BE(scan + 4u * (row * 12 + k)) : 0);
      SR_LOG_BOOT(sr::LogSeverity::Info, "[FXLEXHDR] #%u +%03X: %s", c, row * 48, sd);
    }
    // The scanner's actual INPUT object is at scan+0x10; the truncation-to-16 (input end
    // = src+16) lives here. Dump it + the reader object at scan+0x28.
    uint32_t inbuf = scan >= 0x1000u ? sr::kernel::readGuestU32BE(scan + 0x10u) : 0;
    uint32_t rdr   = scan >= 0x1000u ? sr::kernel::readGuestU32BE(scan + 0x28u) : 0;
    for (int t = 0; t < 2; ++t) {
      uint32_t ob = t ? rdr : inbuf; const char* tg = t ? "RDR28" : "IN10";
      char bd[220]; int bo = 0;
      for (int k = 0; k < 16 && bo < 200; ++k)
        bo += std::snprintf(bd + bo, sizeof(bd) - bo, "%08X ",
                            ob >= 0x1000u ? sr::kernel::readGuestU32BE(ob + 4u * k) : 0);
      SR_LOG_BOOT(sr::LogSeverity::Info, "[FXLEXIN] #%u %s=%08X: %s", c, tg, ob, bd);
    }
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[FXLEX] #%u tok=%d '%c' cur=%08X cntr:%X->%X <=\"%s\"", c, tok, tc, cur, cbBefore, cbAfter, lx);
}

// iter-397b: [MPSHUT] — MoviePipeline_ShutdownWorkerThreads_sub_82480888
// entry/exit attribution. The teardown-wedge sweeps sit INSIDE the first
// block's per-worker vt[+184]/vt[+188] calls with NO NtResumeThread spam
// (the join loops never run) — so either those vt calls wedge or shutdown
// is invoked repeatedly. Log every entry (caller lr, tid, worker handles,
// the +244/+248 state-check results) and every exit; the join fn
// sub_824692A8 gets a reach-counter. Observe-only.
extern "C" void __imp__sub_82480888(PPCContext& __restrict ctx, uint8_t* base);
void sub_82480888(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t a1 = ctx.r3.u32, lr = (uint32_t)ctx.lr;
  uint32_t h66 = 0, h67 = 0, h68 = 0, h69 = 0;
  if (a1 >= 0x1000u) {
    h66 = sr::kernel::readGuestU32BE(a1 + 66u * 4u);
    h67 = sr::kernel::readGuestU32BE(a1 + 67u * 4u);
    h68 = sr::kernel::readGuestU32BE(a1 + 68u * 4u);
    h69 = sr::kernel::readGuestU32BE(a1 + 69u * 4u);
  }
  if (n <= 16 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MPSHUT] ENTER #%u a1=%08X lr=%08X tid=%u workers=%u/%u/%u/%u",
      n, a1, lr, sr::kernel::getCurrentThreadHandle(), h66, h67, h68, h69);
  __imp__sub_82480888(ctx, base);
  if (n <= 16 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Error, "[MPSHUT] EXIT  #%u", n);
}
// iter-397d: [MPGETF] — GetDecodedFrame thunk sub_824707B0 (player vt[+72]).
// RenderYuvVideoFrameTec0 gets 0x16660026 (END-OF-MOVIE) from it on call #1
// — the whole instant-teardown trigger. Log the resolved target + rc; the
// target is the real frame-get to dissect (EOS-at-start class, iter-362m).
extern "C" void __imp__sub_824707B0(PPCContext& __restrict ctx, uint8_t* base);
void sub_824707B0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t a1 = ctx.r3.u32;
  const uint32_t vt = a1 >= 0x1000u ? sr::kernel::readGuestU32BE(a1) : 0;
  const uint32_t tgt = vt >= 0x1000u ? sr::kernel::readGuestU32BE(vt + 72u) : 0;
  // iter-397e: player status block = words +212(sub-state) +216(state; 7=done)
  // +220(data flags: 1=frame-avail 2=more-data) +224(worker flags) +228(stop/
  // error; &6 -> END-OF-MOVIE sentinel). Dump before/after to see which bit
  // flips between call #1 (real frame) and call #2 (sentinel).
  uint32_t b212 = 0, b216 = 0, b220 = 0, b224 = 0, b228 = 0;
  if (a1 >= 0x1000u) {
    b212 = sr::kernel::readGuestU32BE(a1 + 212u);
    b216 = sr::kernel::readGuestU32BE(a1 + 216u);
    b220 = sr::kernel::readGuestU32BE(a1 + 220u);
    b224 = sr::kernel::readGuestU32BE(a1 + 224u);
    b228 = sr::kernel::readGuestU32BE(a1 + 228u);
  }
  __imp__sub_824707B0(ctx, base);
  if (n <= 12 || (n & (n - 1)) == 0) {
    uint32_t a212 = 0, a216 = 0, a220 = 0, a224 = 0, a228 = 0;
    if (a1 >= 0x1000u) {
      a212 = sr::kernel::readGuestU32BE(a1 + 212u);
      a216 = sr::kernel::readGuestU32BE(a1 + 216u);
      a220 = sr::kernel::readGuestU32BE(a1 + 220u);
      a224 = sr::kernel::readGuestU32BE(a1 + 224u);
      a228 = sr::kernel::readGuestU32BE(a1 + 228u);
    }
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MPGETF] #%u player=%08X vt=%08X target=%08X -> rc=%08X "
      "st[212/216/220/224/228] pre=%X/%X/%X/%X/%X post=%X/%X/%X/%X/%X",
      n, a1, vt, tgt, ctx.r3.u32,
      b212, b216, b220, b224, b228, a212, a216, a220, a224, a228);
  }
}

// iter-397e: [MPFLAG] — the player's flag mutators. OR-into: sub_8247FCB8
// (+220 data), sub_8247FD20 (+224 worker), sub_8247FD88 (+228 stop/error).
// CLEAR: sub_8247FDF0 (+220), sub_8247FEC0 (+228). The lr of the call that
// clears +220 bit2 (more-data) or sets +228 bits 2/4 IS the EOS decider.
static void mpFlagLog(const char* op, PPCContext& ctx, unsigned n) {
  if (n > 48 && (n & (n - 1)) != 0) return;
  SR_LOG_BOOT(sr::LogSeverity::Error,
    "[MPFLAG] %s #%u obj=%08X bits=%X lr=%08X tid=%u",
    op, n, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr,
    sr::kernel::getCurrentThreadHandle());
}
#define SR_MPFLAG_HOOK(sym, opname)                                          \
  extern "C" void __imp__##sym(PPCContext& __restrict ctx, uint8_t* base);   \
  void sym(PPCContext& __restrict ctx, uint8_t* base) {                      \
    static std::atomic<unsigned> s_fn{0};                                    \
    mpFlagLog(opname, ctx, s_fn.fetch_add(1, std::memory_order_relaxed) + 1);\
    __imp__##sym(ctx, base);                                                 \
  }
SR_MPFLAG_HOOK(sub_8247FCB8, "OR220")
SR_MPFLAG_HOOK(sub_8247FD20, "OR224")
SR_MPFLAG_HOOK(sub_8247FD88, "OR228")
SR_MPFLAG_HOOK(sub_8247FDF0, "CLR220")
SR_MPFLAG_HOOK(sub_8247FEC0, "CLR228")
// iter-397f: [MPEVT] — the player's SEVEN embedded events (+100..+196) and
// their KeSetEvent / KeWaitForSingleObject wrappers (vt map at 0x82050038:
// waits vt[+172..+192] = FB00(+116) FB18(+132) FB30(+148) FB48(+164)
// FB60(+180); sets vt[+196..+224] = FBA0(+100) FBB0(+116) FBC0(+132)
// FBD0(+148) FBE0(+164) FBF0(+180) FC00(+196)). Worker3 (video stream pump,
// sub_824819E8) parks INFINITE in wait(+148) and no frame ever queues — this
// battery maps the live kick topology: who signals what, which kick is
// missing. Sub-state setters FC58(+212)/FC88(+216) logged with value+lr for
// the full state trail (missed in iter-397e).
SR_MPFLAG_HOOK(sub_8247FBA0, "SET+100")
SR_MPFLAG_HOOK(sub_8247FBB0, "SET+116")
SR_MPFLAG_HOOK(sub_8247FBC0, "SET+132")
SR_MPFLAG_HOOK(sub_8247FBD0, "SET+148")
SR_MPFLAG_HOOK(sub_8247FBE0, "SET+164")
SR_MPFLAG_HOOK(sub_8247FBF0, "SET+180")
SR_MPFLAG_HOOK(sub_8247FC00, "SET+196")
SR_MPFLAG_HOOK(sub_8247FB00, "WAIT+116")
SR_MPFLAG_HOOK(sub_8247FB18, "WAIT+132")
SR_MPFLAG_HOOK(sub_8247FB30, "WAIT+148")
SR_MPFLAG_HOOK(sub_8247FB48, "WAIT+164")
SR_MPFLAG_HOOK(sub_8247FB60, "WAIT+180")
SR_MPFLAG_HOOK(sub_8247FC58, "SUBST212")
SR_MPFLAG_HOOK(sub_8247FC88, "STATE216")

// iter-397g: [MPNODE] — the six movie-graph node "process one block" impls
// (vt[+120] of the node classes found by vtable-header sweep at 0x8204F830/
// 0x8204FDC0/0x8204FEF8/0x8204FFB8/0x82050628/0x82050758; file-source node
// 0x82050228 -> sub_82477328 already known). Logs entry size (r4)+tid, exit
// rc + wall-ms. Names: worker2's node, the 2.85s-blocked submit, and the
// empty-output emitter that the frame-get misreads as END-OF-MOVIE.
#define SR_MPNODE_HOOK(sym, opname)                                          \
  extern "C" void __imp__##sym(PPCContext& __restrict ctx, uint8_t* base);   \
  void sym(PPCContext& __restrict ctx, uint8_t* base) {                      \
    static std::atomic<unsigned> s_nn{0};                                    \
    const unsigned n = s_nn.fetch_add(1, std::memory_order_relaxed) + 1;     \
    const uint32_t a1 = ctx.r3.u32, sz = ctx.r4.u32;                         \
    const bool logit = n <= 24 || (n & (n - 1)) == 0;                        \
    const auto t0 = std::chrono::steady_clock::now();                        \
    __imp__##sym(ctx, base);                                                 \
    if (logit) {                                                             \
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>( \
          std::chrono::steady_clock::now() - t0).count();                    \
      SR_LOG_BOOT(sr::LogSeverity::Error,                                    \
        "[MPNODE] %s #%u obj=%08X sz=%u -> rc=%08X ms=%lld tid=%u",          \
        opname, n, a1, sz, ctx.r3.u32, (long long)ms,                        \
        sr::kernel::getCurrentThreadHandle());                               \
    }                                                                        \
  }
SR_MPNODE_HOOK(sub_82472428, "N-72428")
SR_MPNODE_HOOK(sub_82473BD0, "N-73BD0")
SR_MPNODE_HOOK(sub_82475688, "N-75688")
SR_MPNODE_HOOK(sub_82476638, "N-76638")
SR_MPNODE_HOOK(sub_82485208, "N-85208")
SR_MPNODE_HOOK(sub_824826A8, "N-826A8")
SR_MPNODE_HOOK(sub_82477328, "N-77328-file")

// iter-397i: [MPSEEK] — demux position jumps. AsfDemuxDataPayloads case-5 law:
// state 18 (EOS/0x50000B) fires when pos(v5[9]) >= dataEnd(v5[7]). Sequential
// packet consumption can't reach 23MB in 0.2-8s, so the position must JUMP:
// DemuxSetReadPositionResetStreams_sub_82491978(hi, lo) = absolute seek;
// DemuxStepIndexEntryOffset_sub_82491860 = index-entry lookup feeding it.
// Log every seek target + lr + tid.
extern "C" void __imp__sub_82491978(PPCContext& __restrict ctx, uint8_t* base);
void sub_82491978(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t hi = ctx.r3.u32, lo = ctx.r4.u32;
  const uint32_t lr = (uint32_t)ctx.lr;
  __imp__sub_82491978(ctx, base);
  if (n <= 64 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MPSEEK] SETPOS #%u ctx=%08X pos=%u -> rc=%08X lr=%08X tid=%u",
      n, hi, lo, ctx.r3.u32, lr, sr::kernel::getCurrentThreadHandle());
}
extern "C" void __imp__sub_82491860(PPCContext& __restrict ctx, uint8_t* base);
void sub_82491860(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t a1 = ctx.r3.u32, idx = ctx.r4.u32, outVA = ctx.r5.u32;
  const uint32_t lr = (uint32_t)ctx.lr;
  __imp__sub_82491860(ctx, base);
  uint32_t off = outVA >= 0x1000u ? sr::kernel::readGuestU32BE(outVA) : 0;
  uint32_t off2 = outVA >= 0x1000u ? sr::kernel::readGuestU32BE(outVA + 4) : 0;
  if (n <= 64 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MPSEEK] IDXSTEP #%u ctx=%08X idx=%u out=%08X:%08X -> rc=%08X lr=%08X tid=%u",
      n, a1, idx, off, off2, ctx.r3.u32, lr, sr::kernel::getCurrentThreadHandle());
}

// iter-397i2: [MPSM] — AsfDemuxerRunStateMachine entry: state(v3[20]),
// packet counter(v3[45]), position lo(v3[9]), dataEnd lo(v3[7]). The position
// TRAJECTORY names the mechanism (sequential-fast vs jump) since zero seeks
// fire. v3 = *(a1+28).
extern "C" void __imp__sub_8249A680(PPCContext& __restrict ctx, uint8_t* base);
void sub_8249A680(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t a1 = ctx.r3.u32;
  uint32_t st = 0, pkts = 0, pos = 0, dend = 0, v3 = 0;
  uint32_t nxt = 0, hdr = 0, psz = 0, sk46 = 0, sk47 = 0, rdlo = 0;
  if (a1 >= 0x1000u) {
    v3 = sr::kernel::readGuestU32BE(a1 + 28u);
    if (v3 >= 0x1000u) {
      st   = sr::kernel::readGuestU32BE(v3 + 20u * 4u);
      pkts = sr::kernel::readGuestU32BE(v3 + 45u * 4u);
      pos  = sr::kernel::readGuestU32BE(v3 + 9u * 4u);
      dend = sr::kernel::readGuestU32BE(v3 + 7u * 4u);
      nxt  = sr::kernel::readGuestU32BE(v3 + 11u * 4u);  // nextPacketPos lo
      rdlo = sr::kernel::readGuestU32BE(v3 + 3u * 4u);   // read-cursor lo
      hdr  = sr::kernel::readGuestU32BE(v3 + 1u * 4u);   // fileprops struct
      if (hdr >= 0x1000u) psz = sr::kernel::readGuestU32BE(hdr + 8u);
      sk46 = sr::kernel::readGuestU32BE(v3 + 46u * 4u);
      sk47 = sr::kernel::readGuestU32BE(v3 + 47u * 4u);
    }
  }
  __imp__sub_8249A680(ctx, base);
  if (n <= 48 || (n & (n - 1)) == 0 || ctx.r3.u32 == 5242891u) {
    uint32_t epos = 0, enxt = 0, erd = 0, est = 0;
    if (v3 >= 0x1000u) {
      est  = sr::kernel::readGuestU32BE(v3 + 20u * 4u);
      epos = sr::kernel::readGuestU32BE(v3 + 9u * 4u);
      enxt = sr::kernel::readGuestU32BE(v3 + 11u * 4u);
      erd  = sr::kernel::readGuestU32BE(v3 + 3u * 4u);
    }
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MPSM] #%u dmx=%08X st=%u pkts=%u pos=%u nxt=%u rd=%u dataEnd=%u psz=%u sk=%u/%u -> rc=%08X st'=%u pos'=%u nxt'=%u rd'=%u tid=%u",
      n, v3, st, pkts, pos, nxt, rdlo, dend, psz, sk46, sk47, ctx.r3.u32,
      est, epos, enxt, erd, sr::kernel::getCurrentThreadHandle());
  }
}

// iter-397j: [MPCASE] — bracket the 271-packet position rewrite INSIDE one
// SM pump: per-state helpers logged with the demux position triplet
// (pos lo +36 / next lo +44 / readcursor lo +12 of v3=*(a1+28)). The first
// helper whose ENTRY shows the jumped values brackets the writer.
static void mpCaseLog(const char* op, uint32_t a1, uint32_t rc, unsigned n) {
  if (n > 20 && (n & (n - 1)) != 0) return;
  uint32_t v3 = a1 >= 0x1000u ? sr::kernel::readGuestU32BE(a1 + 28u) : 0;
  uint32_t pos = 0, nxt = 0, rd = 0;
  if (v3 >= 0x1000u) {
    pos = sr::kernel::readGuestU32BE(v3 + 36u);
    nxt = sr::kernel::readGuestU32BE(v3 + 44u);
    rd  = sr::kernel::readGuestU32BE(v3 + 12u);
  }
  SR_LOG_BOOT(sr::LogSeverity::Error,
    "[MPCASE] %s #%u pos=%u nxt=%u rd=%u rc=%08X tid=%u",
    op, n, pos, nxt, rd, rc, sr::kernel::getCurrentThreadHandle());
}
#define SR_MPCASE_HOOK(sym, opname)                                          \
  extern "C" void __imp__##sym(PPCContext& __restrict ctx, uint8_t* base);   \
  void sym(PPCContext& __restrict ctx, uint8_t* base) {                      \
    static std::atomic<unsigned> s_cn{0};                                    \
    const unsigned n = s_cn.fetch_add(1, std::memory_order_relaxed) + 1;     \
    const uint32_t a1c = ctx.r3.u32;                                         \
    mpCaseLog(opname "-IN ", a1c, 0, n);                                     \
    __imp__##sym(ctx, base);                                                 \
    mpCaseLog(opname "-OUT", a1c, ctx.r3.u32, n);                            \
  }
SR_MPCASE_HOOK(sub_82495B78, "PKTFLAGS")
SR_MPCASE_HOOK(sub_82495F50, "PKTFIELDS")
SR_MPCASE_HOOK(sub_824963D0, "PAYLHDR")
SR_MPCASE_HOOK(sub_824990D8, "PKTHDR")
SR_MPCASE_HOOK(sub_824839A0, "ENQ")

// iter-397k: [MPFLUSH] — sub_8247B158 = codec flush/reset (fired from the
// video node when a1[500]==5 = discontinuity state). If it fires between
// decode pull #1 and pull #2 it DISCARDS the ~343 buffered samples of the
// fully-parsed 3.3MB submovie -> pull #2 sees empty+source-done -> EOS.
extern "C" void __imp__sub_8247B158(PPCContext& __restrict ctx, uint8_t* base);
void sub_8247B158(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t a1 = ctx.r3.u32, lr = (uint32_t)ctx.lr;
  __imp__sub_8247B158(ctx, base);
  if (n <= 32 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MPFLUSH] #%u codec=%08X lr=%08X -> rc=%08X tid=%u",
      n, a1, lr, ctx.r3.u32, sr::kernel::getCurrentThreadHandle());
}

// iter-449t: the two-ended movie-6 probe (SR_FMV_EOM_PROBE=1). Movie 6 (the
// long title-card/starfield FMV, PSM cursor 5) false-EOMs ~5 frames in on
// EVERY roll (user-observed freeze; xam1 quantified). The iter-440q design:
// watch BOTH ring ends + the source-read EOS edge, densely, ON THAT MOVIE.
static bool fmvEomProbeOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_FMV_EOM_PROBE");
    return e && e[0] == '1';
  }();
  return v;
}
// PSM splash cursor (+0x230): which intro movie is playing (0-5; -1 done).
static uint32_t fmvPsmCursor() {
  const uint32_t psm = sr::kernel::readGuestU32BE(0x8296EDACu);
  if (psm < 0x1000u) return 0xDEADu;
  return sr::kernel::readGuestU32BE(psm + 0x230u);
}
// Last ring the MPRING battery saw — lets the D80 EOM edge report ring state
// without knowing the queue pointer itself.
static std::atomic<uint32_t> s_lastRingQ{0};

// iter-397m: [MPRING] — the decoded-frame ring ops. Ring struct: +48 items
// (60B each), +52 capacity, +56 READ idx, +60 WRITE idx, +68 atomic fill.
// E178=peek@read, E1C8=slot@write, E260=read-adv(fill--), E2C8=write-adv
// (fill++). No guards in the ring itself; consumer gate = fill!=0
// (sub_822E1BA8). The false-EOS pops a ZERO-SIZE item ~1 frame in while the
// decoder emits full pictures — this battery gives the cursor+slot history.
static void mpRingLog(const char* op, uint32_t q, unsigned n, bool dumpItem,
                      uint32_t itemVA) {
  s_lastRingQ.store(q, std::memory_order_relaxed);
  const uint32_t rd = sr::kernel::readGuestU32BE(q + 56u);
  const uint32_t wr = sr::kernel::readGuestU32BE(q + 60u);
  const uint32_t fl = sr::kernel::readGuestU32BE(q + 68u);
  const uint32_t cap = sr::kernel::readGuestU32BE(q + 52u);
  if (dumpItem && itemVA >= 0x1000u) {
    uint32_t w[15];
    for (int i = 0; i < 15; ++i) w[i] = sr::kernel::readGuestU32BE(itemVA + 4u * i);
    // item fields: +44(w11)=buffer ptr, +48(w12)=capacity, +52(w13)=size
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MPRING] %s #%u q=%08X rd=%u wr=%u fill=%u cap=%u item=%08X "
      "[%08X %08X %08X %08X %08X %08X %08X %08X | %08X %08X %08X buf=%08X icap=%u isz=%u %08X] tid=%u",
      op, n, q, rd, wr, fl, cap, itemVA,
      w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
      w[8], w[9], w[10], w[11], w[12], w[13], w[14],
      sr::kernel::getCurrentThreadHandle());
  } else {
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MPRING] %s #%u q=%08X rd=%u wr=%u fill=%u cap=%u tid=%u",
      op, n, q, rd, wr, fl, cap, sr::kernel::getCurrentThreadHandle());
  }
}
// iter-449t: with SR_FMV_EOM_PROBE=1 the battery goes DENSE while the PSM
// cursor sits on the late movies (4-5) — the movie-6 death window — instead
// of the sparse power-of-2 cadence that misses it.
#define SR_MPRING_HOOK(sym, opname, dumps)                                   \
  extern "C" void __imp__##sym(PPCContext& __restrict ctx, uint8_t* base);   \
  void sym(PPCContext& __restrict ctx, uint8_t* base) {                      \
    static std::atomic<unsigned> s_rn{0};                                    \
    const unsigned n = s_rn.fetch_add(1, std::memory_order_relaxed) + 1;     \
    const uint32_t q = ctx.r3.u32;                                           \
    __imp__##sym(ctx, base);                                                 \
    const bool dense = fmvEomProbeOn() && fmvPsmCursor() >= 4u &&            \
                       fmvPsmCursor() <= 8u;                                 \
    if (n <= 64 || (n & (n - 1)) == 0 || dense)                              \
      mpRingLog(opname, q, n, dumps, dumps ? ctx.r3.u32 : 0);                \
  }
SR_MPRING_HOOK(sub_8247E178, "PEEK-RD", true)
// GET-WR gets a custom body: on the FIRST video-ring slot get (item capacity
// 1382408), arm the hardware write-watch on the SECOND item's vtable word —
// the 0x10101010 clobber victim. The [HWWATCH] hit names the writer's guest
// PC directly (suspect: WMV3 core black-plane init memset walking backwards
// over a missing padding border).
extern "C" void __imp__sub_8247E1C8(PPCContext& __restrict ctx, uint8_t* base);
void sub_8247E1C8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_rn{0};
  const unsigned n = s_rn.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t q = ctx.r3.u32;
  __imp__sub_8247E1C8(ctx, base);
  if (n <= 64 || (n & (n - 1)) == 0)
    mpRingLog("GET-WR ", q, n, true, ctx.r3.u32);
  // iter-428f: DIAGNOSTIC, opt-in only (SR_MPRING_HWWATCH=1). This armer ran
  // ungated in every natural build; combined with the hw-watch disarm leak it
  // randomly killed whichever movie thread wrote the watched dword after the
  // hit budget (caught: the presenter died holding the FMV SM CS -> the
  // movie-transition deadlock -> menu never requested). The iter-397m clobber
  // hunt it served is over; arm it only when hunting.
  static const bool s_hwArmEnabled = [] {
    const char* e = std::getenv("SR_MPRING_HWWATCH");
    return e && e[0] == '1';
  }();
  static std::atomic<bool> s_armed{false};
  const uint32_t item = ctx.r3.u32;
  if (s_hwArmEnabled && !s_armed.load(std::memory_order_relaxed) && item >= 0x1000u &&
      sr::kernel::readGuestU32BE(item + 48u) == 1382408u) {
    bool expected = false;
    if (s_armed.compare_exchange_strong(expected, true)) {
      const uint32_t victim = item + 60u;  // second item's vt word
      if (sr::kernel::armHwWriteWatchHost(base + victim))
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[MPRING] HWWATCH armed on item2 vt word VA=%08X", victim);
    }
  }
}
SR_MPRING_HOOK(sub_8247E260, "ADV-RD ", false)
SR_MPRING_HOOK(sub_8247E2C8, "ADV-WR ", false)

// iter-449t: [SRCREAD] — sub_824B2818 = the demux source read fn (observe).
// IDA (this session): source obj = *(a1+32); rc semantics:
//   rc==0  normal read (ptr/len via a3hi/a4)
//   rc==7  bad-args
//   rc==10 EOS/MARKER, three flavors:
//     (i)   source+80 NONZERO -> sticky marker: returns bytes at source+84,
//           len *(u16*)(source+88), *a5=0 — the iter-360 "sticky source+80"
//           lead: if +80 wedges nonzero, EVERY read returns the marker.
//     (ii)  provider read failed (vt[0] < 0): *a5=1, ptr=0.
//     (iii) natural exhaustion: source+112 done-flag set AND remaining
//           (source+16) drained: *a5=0, clears +112.
// Log every rc!=0 (plus the first 8 calls) with the flavor fields + PSM
// cursor — on the movie-6 death this names WHICH flavor ended the movie.
extern "C" void __imp__sub_824B2818(PPCContext& __restrict ctx, uint8_t* base);
void sub_824B2818(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t a1 = ctx.r3.u32, lr = (uint32_t)ctx.lr;
  const uint32_t src = a1 >= 0x1000u ? sr::kernel::readGuestU32BE(a1 + 32u) : 0;
  uint32_t pre80 = 0, pre112 = 0, preRem = 0;
  if (src >= 0x1000u) {
    pre80  = sr::kernel::readGuestU32BE(src + 80u);
    pre112 = sr::kernel::readGuestU32BE(src + 112u);
    preRem = sr::kernel::readGuestU32BE(src + 16u);
  }
  __imp__sub_824B2818(ctx, base);
  const uint32_t rc = ctx.r3.u32;
  static std::atomic<unsigned> s_eosN{0};
  if (rc != 0 || n <= 8) {
    const unsigned en = rc ? s_eosN.fetch_add(1, std::memory_order_relaxed) + 1
                           : 0;
    // Unconditional first 64 nonzero returns + powers of 2 after; nonzero
    // returns are rare (once per movie end naturally) so a storm IS the
    // finding.
    if (rc == 0 || en <= 64 || (en & (en - 1)) == 0) {
      const uint32_t post80 = src >= 0x1000u
          ? sr::kernel::readGuestU32BE(src + 80u) : 0;
      const uint32_t post112 = src >= 0x1000u
          ? sr::kernel::readGuestU32BE(src + 112u) : 0;
      const uint32_t mlen = src >= 0x1000u
          ? (sr::kernel::readGuestU32BE(src + 88u) >> 16) : 0;  // u16 at +88
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[SRCREAD] #%u rc=%u src=%08X pre(+80=%08X +112=%u rem=%u) "
          "post(+80=%08X +112=%u) mlen=%u cursor=%d lr=%08X tid=%u",
          n, rc, src, pre80, pre112, preRem, post80, post112, mlen,
          (int)fmvPsmCursor(), lr, sr::kernel::getCurrentThreadHandle());
    }
  }
}

// iter-397l: [MPNEXT] — sub_8247B748 = decode-next-picture wrapper. Logs rc,
// *a3 (pendingOut bytes written to node+540), node st500 (discontinuity) and
// eos544. Decode #2 exits empty in 32ms with all inner B30 calls rc=0 — this
// pins its exact iteration/return path.
extern "C" void __imp__sub_8247B748(PPCContext& __restrict ctx, uint8_t* base);
void sub_8247B748(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t a3 = ctx.r5.u32;
  __imp__sub_8247B748(ctx, base);
  if (n <= 40 || (n & (n - 1)) == 0 || (ctx.r3.u32 != 0 && ctx.r3.u32 != 35)) {
    const uint32_t pend = a3 >= 0x1000u ? sr::kernel::readGuestU32BE(a3) : 0;
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MPNEXT] #%u -> rc=%u pend=%u tid=%u",
      n, ctx.r3.u32, pend, sr::kernel::getCurrentThreadHandle());
  }
}

// iter-397h: [MPPUMP] — sub_82478B30 = the codec "decode next picture" step.
// EOS law (decompiled): pump vt[a1[0]+8]() rc==0x50000B ("source done") AND
// no picture in the filled slots -> return 0x500004 -> upstream 33 -> sticky
// EOS at node+544 -> zero-payload item -> FMVRSM end-of-movie. Log the pump
// object + resolved vt[+8] target + final rc; the target is the next
// decompile (what makes it claim source-done ~200ms in with 34MB unread).
extern "C" void __imp__sub_82478B30(PPCContext& __restrict ctx, uint8_t* base);
void sub_82478B30(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t a1 = ctx.r3.u32;
  uint32_t feeder = 0, pumpFn = 0, st133 = 0, f158 = 0;
  if (a1 >= 0x1000u) {
    feeder = sr::kernel::readGuestU32BE(a1);
    st133 = sr::kernel::readGuestU32BE(a1 + 133u * 4u);
    f158 = sr::kernel::readGuestU32BE(a1 + 158u * 4u);
    if (feeder >= 0x1000u) pumpFn = sr::kernel::readGuestU32BE(feeder + 8u);
  }
  __imp__sub_82478B30(ctx, base);
  const uint32_t prc = ctx.r3.u32;
  // rc!=0 (EOS 0x500004 / no-data 0x8051xxxx) always logs — the video codec's
  // terminal call is drowned by the audio codec's 30Hz rc=0 stream otherwise.
  if (prc != 0 || n <= 24 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MPPUMP] #%u codec=%08X feeder=%08X pumpFn=%08X st133=%u f158=%X -> rc=%08X tid=%u",
      n, a1, feeder, pumpFn, st133, f158, ctx.r3.u32,
      sr::kernel::getCurrentThreadHandle());
}

// iter-397c: [MPDTOR] — the pipeline's deleting-dtor sub_82476760 (virtual;
// no static callers). Its lr names WHO destroys the movie pipeline ~300ms
// after create (the instant-teardown trigger).
extern "C" void __imp__sub_82476760(PPCContext& __restrict ctx, uint8_t* base);
void sub_82476760(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 12)
    SR_LOG_BOOT(sr::LogSeverity::Error,
      "[MPDTOR] #%u obj=%08X flags=%X lr=%08X tid=%u",
      n, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr,
      sr::kernel::getCurrentThreadHandle());
  __imp__sub_82476760(ctx, base);
}
extern "C" void __imp__sub_824692A8(PPCContext& __restrict ctx, uint8_t* base);
void sub_824692A8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t h = ctx.r3.u32;
  __imp__sub_824692A8(ctx, base);
  if (n <= 8 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[MPJOIN] #%u handle=%u -> rc=%u", n, h, ctx.r3.u32);
}

// iter-395 NOTE: the planned [HEAPCORE] hook on sub_8246CA80 collided with the
// REAL owner of that symbol: guest/ppc_import_glue.cpp had substituted the
// game's entire heap allocator with the bump allocator (with a double `+= 16`
// out-of-bounds bug = the FXCLOB3 clobber). See the glue for the dissection
// and the SR_REAL_CRT_HEAP delegation. Registry blind spot: guest/*.cpp
// strong-defs are invisible to gen_override_manifest.py — extend the scan.

// iter-364 STEP3c: NAME the region clobberer. The Dr0 watch caught the +104 counter zeroed
// from guestPC 82469B74 = sub_82469B48 -> sub_8246CA80 (the bit31-set dlmalloc path), which
// re-grants the region's LOW address while sub_82467F40's bump is already past it. Hook this
// entry and log the alloc (args + result + caller LR) whenever its result lands on the live
// region -> identifies the exact clobbering alloc to fix. Scoped by g_fxLexArm.
extern "C" void __imp__sub_82469B48(PPCContext& __restrict ctx, uint8_t* base);
void sub_82469B48(PPCContext& __restrict ctx, uint8_t* base) {
  bool on = g_fxLexArm.load(std::memory_order_relaxed) != 0;
  uint32_t a1 = ctx.r3.u32, a2 = ctx.r4.u32, lr = (uint32_t)ctx.lr;
  __imp__sub_82469B48(ctx, base);
  // iter-364 arena probe: capture the LIVE default-heap mstate (handle @0x829AC16C) and
  // dump it on the first 0x8Bxxxxxx results — the real mstate the effect-compiler dlmalloc
  // uses (sub_8246DF08 loads this handle). Resolves which mstate + wilderness backs the region.
  if (s_arenaProbe) {
    uint32_t res = ctx.r3.u32;
    if (res >= 0x8B000000u && res < 0x8C000000u) {
      static std::atomic<int> s_h{0};
      if (s_h.fetch_add(1, std::memory_order_relaxed) < 2) {
        uint32_t hnd = sr::kernel::readGuestU32BE(0x829AC16Cu);
        char buf[720]; int off = 0;
        for (int w = 0; w < 44 && off < (int)sizeof(buf) - 24; ++w) {
          uint32_t v = (hnd >= 0x1000u) ? sr::kernel::readGuestU32BE(hnd + (uint32_t)(w * 4)) : 0;
          if (v) off += snprintf(buf + off, sizeof(buf) - off, " [%d]=%08X", w, v);
        }
        if (off == 0) { buf[0] = '('; buf[1] = '0'; buf[2] = ')'; buf[3] = 0; }
        SR_LOG_BOOT(sr::LogSeverity::Error,
          "[MSTATE3] res=%08X a1=%08X handle@829AC16C=%08X nz:%s", res, a1, hnd, buf);
      }
    }
  }
  if (!on) return;
  uint32_t res = ctx.r3.u32;
  uint32_t reg = g_fxRegionAddr.load(std::memory_order_relaxed);
  // log if the result lands on the region's page (catches the re-grant of 0x..08F0)
  bool nearReg = reg && res && res >= (reg & ~0xFFFu) && res < ((reg & ~0xFFFu) + 0x1000u);
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  if (nearReg || c <= 24) {
    const char* mark = (reg && res && res <= reg && res + 0x40u > reg) ? "  <<<< HITS REGION" : "";
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FXCLOB] #%u sub_82469B48(a1=%08X a2=%08X) lr=%08X -> %08X reg=%08X%s", c, a1, a2, lr, res, reg, mark);
  }
}

// iter-364 STEP3: the RAW char-lexer sub_82372E50(desc, a2, out). It returns token 13
// (end-of-buffer) exactly when desc[0](cursor) >= desc[1](end), and advances desc[0] by the
// token length after each token. The premature EOF at 16 means the desc it reads has
// end=start+16 (vs the correct 788-byte stream 8B3F0710). Log desc/cursor/end/token/advance
// per call to see whether end is set to +16 (buffer-setup bug) or the cursor jumps (advance
// miscompile). Scoped by g_fxLexArm.
extern "C" void __imp__sub_82372E50(PPCContext& __restrict ctx, uint8_t* base);
void sub_82372E50(PPCContext& __restrict ctx, uint8_t* base) {
  bool on = g_fxLexArm.load(std::memory_order_relaxed) != 0;
  uint32_t desc = ctx.r3.u32, outp = ctx.r5.u32;
  uint32_t curB = (desc >= 0x1000u) ? sr::kernel::readGuestU32BE(desc) : 0;
  uint32_t end  = (desc >= 0x1000u) ? sr::kernel::readGuestU32BE(desc + 4u) : 0;
  __imp__sub_82372E50(ctx, base);
  if (!on) return;
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  if (c > 60) return;
  uint32_t curA = (desc >= 0x1000u) ? sr::kernel::readGuestU32BE(desc) : 0;
  int tok = (outp >= 0x1000u) ? (int)sr::kernel::readGuestU32BE(outp) : -999;
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[FXRAW] #%u desc=%08X cur=%08X->%08X end=%08X (avail=%d) tok=%d adv=%d",
      c, desc, curB, curA, end, (int)(end - curB), tok, (int)(curA - curB));
}

// iter-364 STEP2b: the input-stream setup (sub_82371198(obj, src, len, ...)) sets
// obj[0]=src, obj[1]=src+len (end). Log the len it receives during the fallback compile
// to split "wrong length passed in" (codegen at the caller) vs "flex fill stops short"
// (bug is downstream of a correct 788 stream). Scoped by g_fxLexArm.
extern "C" void __imp__sub_82371198(PPCContext& __restrict ctx, uint8_t* base);
void sub_82371198(PPCContext& __restrict ctx, uint8_t* base) {
  bool on = g_fxLexArm.load(std::memory_order_relaxed) != 0;
  uint32_t obj = ctx.r3.u32, src = ctx.r4.u32, len = ctx.r5.u32;
  int a6 = (int)ctx.r6.u32, a7 = (int)ctx.r7.u32;
  __imp__sub_82371198(ctx, base);
  if (on) {
    static std::atomic<unsigned> s_c{0};
    unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
    if (c <= 12) {
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[FXSTREAM] #%u obj=%08X src=%08X len=%d(%08X) a6=%d a7=%d end=%08X", c, obj, src,
          (int)len, len, a6, a7, obj >= 0x1000u ? sr::kernel::readGuestU32BE(obj + 4u) : 0);
      char od[220]; int oo = 0;
      for (int k = 0; k < 16 && oo < 200; ++k)
        oo += std::snprintf(od + oo, sizeof(od) - oo, "%08X ",
                            obj >= 0x1000u ? sr::kernel::readGuestU32BE(obj + 4u * k) : 0);
      SR_LOG_BOOT(sr::LogSeverity::Info, "[FXSTREAMOBJ] #%u obj=%08X: %s", c, obj, od);
    }
  }
}

extern "C" void __imp__sub_82371C10(PPCContext& __restrict ctx, uint8_t* base);
void sub_82371C10(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_c{0};
  unsigned c = s_c.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t strVA = ctx.r6.u32, argVA = ctx.r7.u32, lr = (uint32_t)ctx.lr;
  auto rdStr = [&](uint32_t va, char* out, int cap) {
    out[0] = 0;
    if (va < 0x1000u || va >= 0xC0000000u) return;
    for (int i = 0; i < cap - 1; ++i) {
      uint8_t ch = *reinterpret_cast<const uint8_t*>(base + va + i);
      if (!ch) { out[i] = 0; break; }
      out[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
      out[i + 1] = 0;
    }
  };
  char buf[160], arg[96]; rdStr(strVA, buf, 160); rdStr(argVA, arg, 96);
  if (c <= 120)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[FXERR] #%u lr=%08X: \"%s\" | arg(r7)=\"%s\"", c, lr, buf, arg);
  __imp__sub_82371C10(ctx, base);
}

void sub_8235F488(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t sm = ctx.r3.u32;
  uint32_t stIn = sm ? sr::kernel::readGuestU32BE(sm + 4u) : 0xEE;
  // iter-428d ORPHANED-LOCK probe: [CS_PAIR] proved a pool worker ENTERS the
  // FMV SM CS inside this tick (lr=8235F4B0) and never leaves, then the
  // thread "EXITS normally" — yet every return path in the recompiled body
  // calls the leave. The only leave-skipping exit is an ABNORMAL unwind (a
  // C++ exception — the srGuestLongjmp machinery — thrown by a callee and
  // caught above in the pool wrapper). Catch it in the act.
  try {
    __imp__sub_8235F488(ctx, base);
  } catch (...) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
        "[MOVTICK] #%u EXCEPTION unwinding through RenderStateTick sm=%08X "
        "stateIn=%u stateNow=%u — the SM CS leave was SKIPPED; rethrowing",
        n, sm, stIn, sm ? sr::kernel::readGuestU32BE(sm + 4u) : 0xEE);
    throw;
  }
  uint32_t stOut = sm ? sr::kernel::readGuestU32BE(sm + 4u) : 0xEE;
  // iter-364: when case1 latches dword_829761F8 (state 1->2), dump the holder chain so
  // we see, at latch time, the resource handle (a1+624), the holder (*(R+12)), and
  // holder[0] (the effect ptr the draw will read). null here == effect never built.
  if (stIn == 1u && stOut != 1u) {
    uint32_t hnd = sm ? sr::kernel::readGuestU32BE(sm + 624u) : 0;   // a1+624 handle
    uint32_t R = hnd ? sr::kernel::readGuestU32BE(hnd) : 0;          // *(a1+624)
    uint32_t holder = sr::kernel::readGuestU32BE(0x829761F8u);       // = *(R+12)
    uint32_t eff = holder ? sr::kernel::readGuestU32BE(holder) : 0;  // holder[0]
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FMVRSM] LATCH hnd=%08X R=%08X holder=%08X holder[0]=%08X", hnd, R, holder, eff);
  }
  if (n <= 12 || stIn != stOut || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FMVRSM] #%u state %u->%u player=%08X hr=%08X", n, stIn, stOut,
        sr::kernel::readGuestU32BE(0x829761D4u),
        sr::kernel::readGuestU32BE(0x829760BCu));
}

// iter-357j: the FMV render pump Function_822E5100 (vt[0] of the render-layer
// object from ctor sub_827761F0, "main"/"ui" layers) drives sub_8235F488 —
// the state machine that CREATES the XMV player from the open file handle
// (case 1), decodes+draws frames (case 2), and fires movie-end (case 5->4).
// Zero reads on the movie handle = this pump likely never runs. Count both.
extern "C" void __imp__sub_822E5100(PPCContext& __restrict ctx, uint8_t* base);
void sub_822E5100(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 6 || n % 2000 == 0) {
    uint32_t sm = sr::kernel::readGuestU32BE(0x829761E0u);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[FMVPUMP] #%u smState=%u player=%08X handle=%08X lr=%08X", n,
        sm ? sr::kernel::readGuestU32BE(sm + 4u) : 0xEE,
        sr::kernel::readGuestU32BE(0x829761D4u),
        sr::kernel::readGuestU32BE(0x829761D8u), (uint32_t)ctx.lr);
  }
  __imp__sub_822E5100(ctx, base);
}

extern "C" void __imp__sub_827761F0(PPCContext& __restrict ctx, uint8_t* base);
void sub_827761F0(PPCContext& __restrict ctx, uint8_t* base) {
  SR_LOG_BOOT(sr::LogSeverity::Info, "[RLAYER_CTOR] sub_827761F0 obj=%08X lr=%08X",
              ctx.r3.u32, (uint32_t)ctx.lr);
  __imp__sub_827761F0(ctx, base);
}

// iter-357i: proto-strip walker sub_82164B60 (post-compile: frees the proto's
// debug arrays via luaM_realloc(...,0) then recurses into nested protos) and
// the sized-free wrapper sub_82164898 beneath it — the FIRST successful
// compile ever to reach this path hangs inside DogmaHeapFree_828CF538.
// Log the walker's proto + array fields and every free's ptr/size.
extern "C" void __imp__sub_82164B60(PPCContext& __restrict ctx, uint8_t* base);
void sub_82164B60(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t p = ctx.r4.u32;
  if (n <= 24 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[PROTO_STRIP] #%u mgr=%08X proto=%08X code=%08X/%u k=%08X/%u "
        "lines=%08X/%u subs=%08X/%u lr=%08X",
        n, ctx.r3.u32, p, sr::kernel::readGuestU32BE(p + 0x14),
        sr::kernel::readGuestU32BE(p + 0x30),
        sr::kernel::readGuestU32BE(p + 0x18),
        sr::kernel::readGuestU32BE(p + 0x38),
        sr::kernel::readGuestU32BE(p + 0x1c),
        sr::kernel::readGuestU32BE(p + 0x24),
        sr::kernel::readGuestU32BE(p + 0x10),
        sr::kernel::readGuestU32BE(p + 0x34), (uint32_t)ctx.lr);
  // Phase 4 row 9 (docs/FORCING_RETIREMENT.md): the generated recursive body runs, as on the
  // console; the R1 work-list rewrite (sr::r1::protoStripIterative) is retired.
  __imp__sub_82164B60(ctx, base);
  if (n <= 24)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PROTO_STRIP] #%u EXIT", n);
}

#if SR_ORACLE
namespace sr_alloc_track {  // defined in alloc_overlap_watch.cpp
void onAlloc(uint32_t oldPtr, uint32_t res, uint32_t sz);
int onFree(uint32_t ptr);
}

extern "C" void __imp__sub_82164898(PPCContext& __restrict ctx, uint8_t* base);
void sub_82164898(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  static std::atomic<unsigned> s_dbl{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  int verdict = sr_alloc_track::onFree(ctx.r3.u32);
  if (verdict == 1) {
    unsigned d = s_dbl.fetch_add(1, std::memory_order_relaxed) + 1;
    if (d <= 16 || (d & (d - 1)) == 0) {
      uint32_t bt[5] = {0, 0, 0, 0, 0};
      uint32_t fp = ctx.r1.u32;
      for (int i = 0; i < 5 && fp; ++i) {
        uint32_t bc = sr::kernel::readGuestU32BE(fp);
        if (bc <= fp || bc - fp > 0x100000u) break;
        bt[i] = sr::kernel::readGuestU32BE(bc - 8);
        fp = bc;
      }
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[DOUBLE_FREE] #%u ptr=%08X size=%08X lr=%08X chain=[%08X %08X "
          "%08X %08X %08X]",
          d, ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr, bt[0], bt[1], bt[2],
          bt[3], bt[4]);
    }
  }
  if (n <= 24 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[DOGMA_FREE] #%u ptr=%08X size=%08X lr=%08X v=%d", n, ctx.r3.u32,
        ctx.r4.u32, (uint32_t)ctx.lr, verdict);
  __imp__sub_82164898(ctx, base);
  if (n <= 24 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[DOGMA_FREE] #%u EXIT", n);
}
#endif  // Oracle heap observation; release uses the generated free body

// iter-357i: luaD_throw itself (Function_827623B8(L, status); named via
// luaG_errormsg sub_8276A0B0 tail). EVERY Lua error passes here including
// syntax (status 3) and OOM (status 4) that never touch luaG_runerror —
// this is how a silent dostring rc=0 tells us WHAT failed.
extern "C" void __imp__sub_827623B8(PPCContext& __restrict ctx, uint8_t* base);
void sub_827623B8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 96 || (n & (n - 1)) == 0) {
    // The error object sits at L->top-1 (top at L+8). TValue = {tt, value};
    // tt==4 -> TString, chars at +0x10. This names every swallowed failure
    // ("malformed number near ...", "attempt to call ... `Name'").
    char msg[161] = {0};
    uint32_t L = ctx.r3.u32;
    uint32_t top = L ? sr::kernel::readGuestU32BE(L + 8u) : 0;
    if (top >= 8u) {
      uint32_t tt = sr::kernel::readGuestU32BE(top - 8u);
      uint32_t val = sr::kernel::readGuestU32BE(top - 4u);
      if (tt == 4u && val >= 0x1000u && val < 0xC0000000u) {
        const char* p =
            reinterpret_cast<const char*>(sr::kernel::guestVAToHost(val + 0x10u));
        if (p) {
          for (int i = 0; i < 160; ++i) {
            char c = p[i];
            if (!c) break;
            msg[i] = (c >= 32 && c < 127) ? c : '.';
          }
        }
      }
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[LUA_THROW] #%u L=%08X status=%d lr=%08X msg=\"%s\"", n, ctx.r3.u32,
        (int)(int32_t)ctx.r4.u32, (uint32_t)ctx.lr, msg);
  }
  __imp__sub_827623B8(ctx, base);
}

// iter-357h: the Lua natives themselves. PlayFMVs sub_822A2640 gates on
// {uiMgr *(0x82911460)!=0, FUN_8214cea8(), [DAT_8296d874]+0x78 && +0x90} then
// trampolines to StartSplash; AreFMVsDone sub_822A26E8 polls psm+0x234.
// (PlayFMVs sub_822A2640 already logged as [SPLASH_TICK] in apt_parse_trace.cpp
//  — 0 hits on natural runs = the Lua boot chunks never call it.)
extern "C" void __imp__sub_822A26E8(PPCContext& __restrict ctx, uint8_t* base);
void sub_822A26E8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8 || n % 500 == 0) {
    uint32_t psm = sr::kernel::readGuestU32BE(0x8296EDACu);
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[LUA_NATIVE] AreFMVsDone #%u psm=%08X done=%08X lr=%08X", n, psm,
        psm ? sr::kernel::readGuestU32BE(psm + 0x234u) : 0, (uint32_t)ctx.lr);
  }
  __imp__sub_822A26E8(ctx, base);
}

// The splash-name -> FMV enqueue chain (golden: AS/runtime-dispatched native
// sub_822A8F88 copies splash item name into psm+0x18 and queues via
// sub_8235F2D0 when the FMV SM is in state 0). Log both ends.
extern "C" void __imp__sub_822A8F88(PPCContext& __restrict ctx, uint8_t* base);
void sub_822A8F88(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 12)
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[SPLASH_ENQ] sub_822A8F88 #%u r3=%08X r4=%08X lr=%08X", n, ctx.r3.u32,
        ctx.r4.u32, (uint32_t)ctx.lr);
  __imp__sub_822A8F88(ctx, base);
}

// (FMV queue sub_8235F2D0 already logged as [MOVIE_Q] in alloc_overlap_watch.cpp.)

// iter-213: split the GAMECONFIG stall into "driver never runs" vs "source not
// ready". sub_826FB5F0 = GAMECONFIG load driver; sub_82704E88 = finalizer that
// sets the done-flag [item+79] = source->vtable[+16](). The ResourceScheduler
// re-dispatches the GAMECONFIG item forever because [item+79] stays 0.
static int sr_rdU8(uint32_t va) {
  if (va < 0x10000u) return -1;
  uint8_t* h = sr::kernel::guestVAToHost(va);
  return h ? (int)h[0] : -1;
}

extern "C" void __imp__sub_826FB5F0(PPCContext& __restrict ctx, uint8_t* base);
void sub_826FB5F0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<uint64_t> s_n{0};
  uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t a3 = ctx.r3.u32, alr = (uint32_t)ctx.lr;
  if (n <= 10 || (n % 100000 == 0)) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[GAMECONFIG_DRIVER] sub_826FB5F0 #%llu this=0x%08X lr=0x%08X thread=%u",
      (unsigned long long)n, a3, alr, sr::kernel::getCurrentThreadHandle());
  }
  __imp__sub_826FB5F0(ctx, base);
}

extern "C" void __imp__sub_82704E88(PPCContext& __restrict ctx, uint8_t* base);
void sub_82704E88(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t item = ctx.r3.u32;
  // iter-366: a4 (r6) is the flag that decides the FATAL case — when the
  // delivery manager does not know the resource AND a4 == 0, the ctor flags the
  // request and deliberately SKIPS its second-chance provider assignment, so
  // +0x40 stays NULL and the item can never complete. Capture a4 before the
  // body runs and report every provider-less construction, unsampled: that is
  // the exact birth of the deadlock (iter-365s/z).
  const uint32_t ctorA4 = ctx.r6.u32;
  int before = sr_rdU8(item + 79);
  __imp__sub_82704E88(ctx, base);
  int after = sr_rdU8(item + 79);
  if (item >= 0x10000u) {
    const uint32_t prov = sr::kernel::readGuestU32BE(item + 0x40);
    if (prov == 0) {
      static std::atomic<unsigned> s_pl{0};
      unsigned p = s_pl.fetch_add(1, std::memory_order_relaxed) + 1;
      if (p <= 64) {
        char nm[40] = {0};
        uint32_t np = sr::kernel::readGuestU32BE(item + 0x18);
        if (np >= 0x10000u)
          for (int i = 0; i < 39; ++i) {
            uint8_t* c = sr::kernel::guestVAToHost(np + i);
            if (!c || !*c) break;
            nm[i] = (char)*c;
          }
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[REQ_PROVIDERLESS] #%u item=%08X name18=%08X '%s' a4=%08X "
            "field72=%08X key=%08X tid=%u — item can NEVER complete",
            p, item, np, nm, ctorA4,
            sr::kernel::readGuestU32BE(item + 72),
            sr::kernel::readGuestU32BE(sr::kernel::readGuestU32BE(item + 8) + 8),
            sr::kernel::getCurrentThreadHandle());
      }
    }
  }
  static std::atomic<uint64_t> s_n{0};
  uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  // iter-350g: XML-config-type items (hash 0x6A1B8478 at mgr[+8], incl. the
  // FE flow 'WorldFlow' asset) log unsampled — their instant-fail is the
  // menu blocker under investigation. Bounded (~100/run).
  auto rdPeek = [](uint32_t va) -> uint32_t { return va >= 0x10000u ? sr::kernel::readGuestU32BE(va) : 0u; };
  const bool xmlCfg = rdPeek(rdPeek(item + 8) + 8) == 0x6A1B8478u;
  if (n <= 12 || (n % 50000 == 0) || before != after || (xmlCfg && n < 10000)) {
    // Identify the SHARED source manager [item+8]=0x89F62AA0 by its static vtable,
    // and the per-entry sub-object [item+0x18]. Their vtables (at +0) are static
    // 0x82xxxxxx addresses we can decode to name the classes + find the stream.
    auto rd = [](uint32_t va) -> uint32_t { return va >= 0x10000u ? sr::kernel::readGuestU32BE(va) : 0u; };
    uint32_t mgr   = rd(item + 8);
    uint32_t mgrVt = rd(mgr);
    uint32_t sub18 = rd(item + 0x18);
    uint32_t s18Vt = rd(sub18);
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[GAMECONFIG_FINAL] sub_82704E88 #%llu item=0x%08X done %d->%d | mgr=0x%08X mgrVt=0x%08X mgr[+4]=0x%08X mgr[+8]=0x%08X mgr[+C]=0x%08X mgr[+10]=0x%08X mgr[+14]=0x%08X | sub18=0x%08X s18Vt=0x%08X s18[+4]=0x%08X s18[+8]=0x%08X thread=%u owner44=%08X",
      (unsigned long long)n, item, before, after,
      mgr, mgrVt, rd(mgr + 4), rd(mgr + 8), rd(mgr + 0xC), rd(mgr + 0x10), rd(mgr + 0x14),
      sub18, s18Vt, rd(sub18 + 4), rd(sub18 + 8), sr::kernel::getCurrentThreadHandle(),
      rd(item + 0x44));  // iter-365e: owner token — the dispatch owner-gate comparator
  }
}

// iter-213L: pin WHY "gameconfig" never reaches "ready" (the airtight gate:
// FM walker stalls at module 10 FMGameConfig, which requests asset key 0x0EA70951
// and blocks until ready). FMGameConfig (sub_8214AFB8) calls resMgr->vtable[8] =
// sub_826FAA70(r3=&handle, r4=resMgr, r5=key, r6=name, r7=0). Asset readiness =
// the 16-bit load-state flags at [[handle+0]+28] (sub_826E4DB0=bit5, _DC0=bit6,
// _DD0=state[8:9]==1). Log the gameconfig asset's live flags right after the
// request. If [GC_REQ_EXIT] never appears while [GC_REQ_ENTER] does, the wait is
// INSIDE sub_826FAA70 itself (next layer to instrument).
extern "C" void __imp__sub_826FAA70(PPCContext& __restrict ctx, uint8_t* base);
void sub_826FAA70(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t key       = ctx.r5.u32;
  const uint32_t handleOut = ctx.r3.u32;   // &handle (output param)
  const bool gc = (key == 0x0EA70951u);    // "gameconfig"
  static std::atomic<uint64_t> s_n{0};
  uint64_t n = gc ? (s_n.fetch_add(1, std::memory_order_relaxed) + 1) : 0;
  const bool logit = gc && (n <= 12 || (n % 200000 == 0));
  if (logit) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[GC_REQ_ENTER] #%llu handleOut=0x%08X resMgr=0x%08X thread=%u",
      (unsigned long long)n, handleOut, ctx.r4.u32, sr::kernel::getCurrentThreadHandle());
  }
  __imp__sub_826FAA70(ctx, base);
  if (logit) {
    auto rd = [](uint32_t va) -> uint32_t { return va >= 0x10000u ? sr::kernel::readGuestU32BE(va) : 0u; };
    uint32_t inner = rd(handleOut);            // [handle+0] = asset/state object
    uint32_t flags = rd(inner + 28) >> 16;     // 16-bit load-state halfword at +28
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[GC_REQ_EXIT] #%llu inner=0x%08X flags=0x%04X bit5=%u bit6=%u st89=%u | a[+0]=0x%08X a[+4]=0x%08X a[+0x18]=0x%08X a[+0x44]=0x%08X",
      (unsigned long long)n, inner, flags,
      (unsigned)((flags >> 5) & 1), (unsigned)((flags >> 6) & 1), (unsigned)((flags >> 8) & 3),
      rd(inner), rd(inner + 4), rd(inner + 0x18), rd(inner + 0x44));
  }
}

// iter-365n I/O-DEATH LAW attribution: ALL file I/O dies at the regime
// transition (4x GameKernelLoop spawn + intro-XMV create = the last NtReadFile
// ever, 5/5 runs), and the live walk showed every station empty-but-healthy:
// the FM job PRODUCERS stop. Hook the two push sites to name them:
//  - sub_82605330(worker, job)  = FM file-worker job push (list +8/+12, count
//    +16, condvar +64; pop side = FmFileWorkerLoop_sub_826045C0). Callers:
//    sub_826054C0/62C0/6328/6460/7470/75A8/77F0 (the read/write post APIs).
//  - sub_826F97A0(dlv, item)    = delivery-worker input push (CS +116, vector
//    +204, condvar +28; pop side = ResDeliveryWorkerLoop_sub_826F92E0).
// Pure logging, no behavior change. The last [FMPOST]/[DLVPOST] before the
// transition instant names the producer class that dies there.
extern "C" void __imp__sub_82605330(PPCContext& __restrict ctx, uint8_t* base);
void sub_82605330(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t worker = ctx.r3.u32, job = ctx.r4.u32, lr = (uint32_t)ctx.lr;
  if (n <= 500 || (n % 16) == 0) {
    auto rd = [](uint32_t va) -> uint32_t { return va >= 0x10000u ? sr::kernel::readGuestU32BE(va) : 0u; };
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[FMPOST] #%u worker=%08X job=%08X prio=%08X flag9=%u cnt=%u tid=%u lr=%08X",
        n, worker, job, rd(job + 12),
        (unsigned)(job >= 0x10000u ? *(uint8_t*)(base + job + 9) : 0),
        rd(worker + 16), sr::kernel::getCurrentThreadHandle(), lr);
  }
  __imp__sub_82605330(ctx, base);
}

extern "C" void __imp__sub_826F97A0(PPCContext& __restrict ctx, uint8_t* base);
void sub_826F97A0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 500 || (n % 64 == 0))
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[DLVPOST] #%u dlv=%08X item=%08X tid=%u lr=%08X",
        n, ctx.r3.u32, ctx.r4.u32,
        sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
  __imp__sub_826F97A0(ctx, base);
}

// ============================================================================
// iter-385: DELIVER-STEP X-RAY (the one-race killer experiment; RAG #185).
//
// iter-384b pinned the I/O-death instant to "first delivery consumed": the
// delivery worker pops the item (vector at dlv+0xB0 goes empty) yet catalog
// registration never happens and ALL file I/O stops. The consume path is
// sub_82700910 / sub_827009E8 (notify wrappers) -> sub_82700820 (deliver).
//
// IDA contract of sub_82700820(item) — three SILENT-SWALLOW exits:
//   sink = *(item+128)  (per-item delivery sink, ref-assigned)
//   if (!sink)                     -> swallow             [NOSINK]
//   else if (byte(item+145))       -> sink->vt[+20](payload @item+120)  [FORCED]
//   else poll op(item+8), status = *(item+12):
//        ==  1  -> sink->vt[+20](payload)                 [DELIVER]
//        == -2  -> sink->vt[+24]()                        [ERRCB]
//        else   -> NOTHING (pending!)                     [SWALLOW-PEND]
//   then ALWAYS sub_82703AC8(item): byte(item+142)=1 (consumed), sink
//   released, *(item+128)=0, item freed. A pending status is swallowed with
//   no re-post visible here — if that branch fires on the first delivery,
//   the continuation is lost exactly as observed.
//
// This hook logs pre/post item state + branch classification. Pure logging,
// capped. Decides: registration RAN (vt[+20] target named -> hook deeper)
// vs continuation LOST (NOSINK / SWALLOW-PEND -> our wait/event semantics).
// ============================================================================
extern "C" void __imp__sub_82700820(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82700910(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_827009E8(PPCContext& __restrict ctx, uint8_t* base);

static inline uint32_t dlvBE32(uint8_t* base, uint32_t va) {
  if (va < 0x10000u || va >= 0x9FFF0000u) return 0xDEADDEADu;
  return _byteswap_ulong(*reinterpret_cast<const uint32_t*>(base + va));
}
static inline uint8_t dlvB8(uint8_t* base, uint32_t va) {
  if (va < 0x10000u || va >= 0x9FFF0000u) return 0xEEu;
  return *reinterpret_cast<const uint8_t*>(base + va);
}

void sub_82700910(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 64 || (n % 64 == 0))
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[DLVRUN] A #%u item=%08X tid=%u lr=%08X",
        n, ctx.r3.u32, sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
  __imp__sub_82700910(ctx, base);
}

void sub_827009E8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 64 || (n % 64 == 0))
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[DLVRUN] B #%u item=%08X tid=%u lr=%08X",
        n, ctx.r3.u32, sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
  __imp__sub_827009E8(ctx, base);
}

// iter-385b: [MODINIT] FmModuleInitOnce X-ray lives in apt_parse_trace.cpp
// (merged into the existing iter-291 sub_8271F2C8 hook to avoid a dup symbol).

// iter-390: [RUNNER_STOP]/[RUNNER_START] — mode-B's divergence is a runner
// LIFECYCLE event: in deep rolls the loading runners' host threads (guest
// 49/50) never exit; in mode-B rolls something STOPS runners 894BF440/4F0
// at flow-completion (Run loops return, workers exit, replacements never
// re-host the loops) and the FE descriptor starves forever. These name the
// stopper (lr/tid) and whether a matching restart follows. Cold paths —
// log every call.
extern "C" void __imp__sub_82720650(PPCContext& __restrict ctx, uint8_t* base);
void sub_82720650(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  SR_LOG_BOOT(sr::LogSeverity::Error,
      "[RUNNER_STOP] #%u runner=%08X tid=%u lr=%08X",
      n, ctx.r3.u32, sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
  __imp__sub_82720650(ctx, base);
}

extern "C" void __imp__sub_82720590(PPCContext& __restrict ctx, uint8_t* base);
void sub_82720590(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[RUNNER_START] #%u runner=%08X tid=%u lr=%08X",
      n, ctx.r3.u32, sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
  __imp__sub_82720590(ctx, base);
}

// ============================================================================
// iter-385d: [SLOTPIN] — the in-flight-slot install X-ray (sub_826F4D98).
//
// iter-385's lockstep A/B (two instances, 42ms skew, both exec #426
// 'X2_DECO_02_J_PHYS'; deep continues at +8ms, shallow never execs again)
// reduced the shallow-death to: ServiceNext sub_826F4230 degenerates to
// PassivePoll forever = the queue-A in-flight slot (+52) is PINNED. The
// iter-365s plate comment names the mechanic: a sync acquire for a name in
// NO registered catalog builds a PROVIDER-LESS item (+0x40=0, cursor
// +0x38=FFFFFFFF), installs it into the slot, then blocks its own thread in
// the drain; mid-METROPOLIS-churn the roll-dependent collision freezes the
// preload at a variable index (233/304/426/446 observed).
//
// sub_826F4D98(slotAddr, item) is the single install point (slotAddr =
// sched+52 queue A / sched+56 queue B). Log every install (capped), and
// ALWAYS log provider-less installs at ERROR with the item's name — the
// wedge-instant install in a shallow roll names the pinned request and the
// requesting thread. Pure logging.
// ============================================================================
// iter-385e: [PASSIVE] — PassivePoll state X-ray (sub_826F37C0). The shallow
// wedge = ServiceNext degenerating to PassivePoll forever, but iter-385d
// killed the provider-less-pin variant (0 provider-less installs in 3
// shallow rolls) and CS_TIMEOUT_FIX=0 killed the fake-held-CS race. Remaining
// no-exec paths: slot +52/+56 occupied by a NORMAL item whose completion was
// lost, or head item owned by another thread (vt[+124] != current tid; main
// parks seconds before every observed wedge). Log the scheduler state on
// every CHANGE of (slotA, slotB, cntA, cntB) + heavy sampling; the steady
// post-wedge tuple names the wedge class, the slotted item names the loss.
extern "C" void __imp__sub_826F37C0(PPCContext& __restrict ctx, uint8_t* base);
void sub_826F37C0(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t sched = ctx.r3.u32;
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t slotA = dlvBE32(base, sched + 52), slotB = dlvBE32(base, sched + 56);
  uint32_t cntA = (dlvBE32(base, sched + 64) - dlvBE32(base, sched + 60)) >> 2;
  uint32_t cntB = (dlvBE32(base, sched + 76) - dlvBE32(base, sched + 72)) >> 2;
  static std::atomic<uint64_t> s_lastSig{0};
  uint64_t sig = (uint64_t(slotA ^ (slotB << 1)) << 32) |
                 ((cntA & 0xFFFFu) << 16) | (cntB & 0xFFFFu);
  bool changed = s_lastSig.exchange(sig, std::memory_order_relaxed) != sig;
  if (changed || n <= 16 || (n & (n - 1)) == 0) {
    char nmA[40] = {0};
    uint32_t ownA = 0;
    if (slotA >= 0x10000u && slotA != 0xDEADDEADu) {
      uint32_t np = dlvBE32(base, slotA + 0x18);
      if (np >= 0x10000u && np < 0xC0000000u)
        for (int i = 0; i < 39; ++i) {
          uint8_t* c = sr::kernel::guestVAToHost(np + i);
          if (!c || !*c || *c < 32 || *c >= 127) break;
          nmA[i] = (char)*c;
        }
      ownA = dlvBE32(base, slotA + 0x58);
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[PASSIVE] #%u sched=%08X slotA=%08X('%s' own58=%08X) slotB=%08X "
        "cntA=%u cntB=%u schedOwn88=%08X flag92=%u tid=%u lr=%08X",
        n, sched, slotA, nmA, ownA, slotB, cntA, cntB,
        dlvBE32(base, sched + 88), (unsigned)dlvB8(base, sched + 92),
        sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
  }
  __imp__sub_826F37C0(ctx, base);
}

// iter-385g: [TASKFLOW] — the task state-machine edge X-ray. iter-385f named
// the wedge: the conveyor's LAST enqueued task EXECS (enq/exec 1:1 to the
// end) yet its record completion never lands and no next acquire follows.
// The deliver handler sub_827046E8 has three exits: status 0 -> re-arm via
// inline kick sub_82703EA0 OR (vt[+120] defer) sub_827042E0 deferred re-post
// to the sub_82703E90 global queue; status 1 -> complete (record write);
// else -> drop. Log EVERY edge (task ptr + tid + lr, ~2k lines/run) — the
// final cycle's last edge names the loss (prime suspect: a deferred re-post
// into a queue whose pump isn't running in shallow rolls).
extern "C" void __imp__sub_827046E8(PPCContext& __restrict ctx, uint8_t* base);
void sub_827046E8(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t sinkCtx = ctx.r3.u32, payload = ctx.r4.u32;
  uint32_t task = dlvBE32(base, sinkCtx + 8);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[TASKFLOW] DELIVER #%u ctx=%08X task=%08X pay=%08X tid=%u lr=%08X",
      n, sinkCtx, task, payload,
      sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
  __imp__sub_827046E8(ctx, base);
}

// (KICK sub_82703EA0 logging merged into rtam_service.cpp's existing hook.)

// iter-387: [RECSET] lives at the iter-357j sub_826E4E90 hook above
// (upgraded in place; duplicate removed).

// iter-388 fork-(a) probe: [POOLFILL] — the guest memcpy (sub_8279EC60) when
// the DEST is a pool-arena record buffer (0x84xxxxxx, size >= 0xC00 = the
// X2 record class). The fatal record's fill presence + its src first-dword
// decides the fork: fill absent = LoadResourceAsset branch skipped it; fill
// present with src-dword == Havok magic then parse sees zeros = post-fill
// scrub ([HEAP_POOLX] names the scrubber); fill present with src-dword == 0
// = the SOURCE (package image) was scrubbed instead. Ultra-cheap range check
// on the hot memcpy; logs are rare.
#endif

extern "C" void __imp__sub_8279EC60(PPCContext& __restrict ctx, uint8_t* base);

// iter-466 tick (SR_NATIVE_MEMCPY, A/B gate; SR_NATIVE_MEMCPY_CHECK=1 verifies): the guest CRT
// memcpy as one host memcpy. Guest contract (the splice body = the recompiled 0x8279EC60):
// std r3,-8(r1); copy r5 bytes from r4 to r3 forward (byte head to 8-byte alignment, then
// 128-byte / 8-byte / byte loops); ld r3,-8(r1); blr - so r3 comes back unchanged and the
// stack word r1-8 holds it. Native does the same two observable writes (the copy and the
// stack word) when both ranges are contiguous after the alias fold and DISJOINT (overlapping
// copies, where the guest's chunk order decides the bytes, and the > 16 MB cap of the splice
// body keep the guest body). Volatile registers (r4-r12, ctr, cr, xer) are not reproduced:
// the guest ABI makes them dead after the call (tools/regen/abi_audit.py lists no OUT use).
static bool nativeMemcpy(PPCContext& ctx, uint8_t* base) {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_NATIVE_MEMCPY");
    return !e || e[0] != '0';  // iter-466 tick: baked default ON (=0 A/B)
  }();
  // C1: the CHECK (SR_NATIVE_MEMCPY_CHECK=1 re-runs the guest body and compares) is a development oracle, compiled
  // out under SR_ORACLE=0 (kernel/sr_oracle.hpp): the gate then reads unset, and checked / checkBad stay 0.
#if SR_ORACLE
  static const bool s_check = [] {
    const char* e = std::getenv("SR_NATIVE_MEMCPY_CHECK");
    return e && e[0] == '1';
  }();
#endif
  if (!s_on) return false;
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, sz = ctx.r5.u32;
  if (sz == 0u || sz > 0x1000000u) return false;
  const uint32_t d0 = sr::kernel::aliasGuestVA(dst), d1 = sr::kernel::aliasGuestVA(dst + sz - 1u);
  const uint32_t s0 = sr::kernel::aliasGuestVA(src), s1 = sr::kernel::aliasGuestVA(src + sz - 1u);
  const uint32_t k0 = sr::kernel::aliasGuestVA(ctx.r1.u32 - 8u), k1 = sr::kernel::aliasGuestVA(ctx.r1.u32 - 1u);
  if (d1 < d0 || d1 - d0 != sz - 1u || s1 < s0 || s1 - s0 != sz - 1u || k1 < k0 || k1 - k0 != 7u)
    return false;
  if (!(uint64_t(d0) + sz <= s0 || uint64_t(s0) + sz <= d0)) return false;  // overlap: guest body
  // The guest reservation spans every 32-bit VA + a guard (guest_harness kTotalSize); a
  // smaller fallback reservation keeps the guest body.
  if (uint64_t(sr::kernel::getGuestSize()) < 0x100000008ull) return false;
  const uint64_t r3be = __builtin_bswap64(ctx.r3.u64);
  std::memcpy(base + k0, &r3be, 8);
  std::memcpy(base + d0, base + s0, sz);
#if SR_ORACLE
  // Only this existing, actually executed byte copy can mark the armed reader.
  // A before-call hook or a delegated normal return cannot do so.
  sr::kernel::reader_observe::nativeReadExecuted(src, dst, sz);
#endif
  static std::atomic<uint64_t> s_native{0}, s_checked{0}, s_bad{0};
  const uint64_t n = s_native.fetch_add(1, std::memory_order_relaxed) + 1;
#if SR_ORACLE
  if (s_check && sz <= 65536u) {
    static thread_local uint8_t snap[65536];
    std::memcpy(snap, base + d0, sz);
    const uint64_t r3 = ctx.r3.u64;
    const PPCRegister r4 = ctx.r4, r5 = ctx.r5;
    __imp__sub_8279EC60(ctx, base);  // idempotent for a disjoint copy: same bytes again
    s_checked.fetch_add(1, std::memory_order_relaxed);
    const bool ok = std::memcmp(snap, base + d0, sz) == 0 && ctx.r3.u64 == r3;
    if (!ok) {
      const uint64_t bad = s_bad.fetch_add(1, std::memory_order_relaxed) + 1;
      if (bad <= 16)
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[NATIVE_MEMCPY] MISMATCH #%llu dst=%08X src=%08X sz=%u r3=%016llX/%016llX",
                    (unsigned long long)bad, dst, r4.u32, r5.u32, (unsigned long long)r3,
                    (unsigned long long)ctx.r3.u64);
    }
  }
#endif
  if ((n & 0xFFFFFu) == 1u)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_MEMCPY] native=%llu checked=%llu checkBad=%llu",
                (unsigned long long)n, (unsigned long long)s_checked.load(std::memory_order_relaxed),
                (unsigned long long)s_bad.load(std::memory_order_relaxed));
  return true;
}

void sub_8279EC60(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, sz = ctx.r5.u32;
#if SR_ORACLE
  const uint32_t src = ctx.r4.u32;
  // iter-388c [IMGWRITE]: any guest memcpy landing inside the (deterministic)
  // metropolis physics package image [0x91D8AC90, 0x9245F2C4) — the poll
  // watcher proved the image gets overwritten with 0x5FF tile data even in
  // deep rolls; this names the writer (lr + src).
  if (dst < 0x9245F2C4u && dst + sz > 0x91D8AC90u) {
    static std::atomic<unsigned> s_iw{0};
    unsigned iw = s_iw.fetch_add(1, std::memory_order_relaxed) + 1;
    if (iw <= 48 || (iw & (iw - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[IMGWRITE] #%u dst=%08X src=%08X sz=%X tid=%u lr=%08X  <<writes into pkg image",
          iw, dst, src, sz,
          sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
  }
  if (dst >= 0x84000000u && dst < 0x86000000u && sz >= 0xC00u) {
    static std::atomic<unsigned> s_n{0};
    unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    uint32_t s0 = (src >= 0x10000u && src < 0xC0000000u)
                      ? sr::kernel::readGuestU32BE(src) : 0xEEEEEEEEu;
    if (n <= 1200 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[POOLFILL] #%u dst=%08X src=%08X sz=%X src0=%08X tid=%u lr=%08X",
          n, dst, src, sz, s0,
          sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
  }
#endif
  if (!nativeMemcpy(ctx, base)) __imp__sub_8279EC60(ctx, base);
  // DrawVerticesUP's exact scoped copy is complete here, before its guest
  // caller publishes dev+0x28. No LR inference (skip_lr builds do not keep it).
  sr::kernel::resourceInlineMemcpyComplete(dst, sz);
}
#if SR_ORACLE


// iter-387b: [RIGBPARSE] — sub_822BE140, the RIGB physics-asset parse whose
// silent failure IS the shallow-death (fatal record: alloc OK, parse fails,
// setter(NULL), no FAILED-flag mark, conveyor waits forever; DEEP runs parse
// the SAME record fine -> state-driven). Suspect: the record's data ptr
// points into the streamed window cache; a delayed exec parses a RECYCLED
// window. Log inputs + rc + first data bytes: the fatal parse's bytes tell
// recycled-buffer (foreign bytes) vs deeper state issue.
extern "C" void __imp__sub_822BE140(PPCContext& __restrict ctx, uint8_t* base);
void sub_822BE140(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t wrap = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32;
  // iter-387c: rc=1 = sub_825E88C0 container-OPEN rejected the buffer header
  // (rc=2 section missing, rc=3 'Physics Data' chunk missing). Log the
  // buffer's FIRST 16 BYTES: fatal-vs-healthy content names recycled-buffer
  // vs deeper state.
  uint32_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
  if (a4 >= 0x10000u && a4 < 0xC0000000u) {
    b0 = sr::kernel::readGuestU32BE(a4);
    b1 = sr::kernel::readGuestU32BE(a4 + 4);
    b2 = sr::kernel::readGuestU32BE(a4 + 8);
    b3 = sr::kernel::readGuestU32BE(a4 + 12);
  }
  __imp__sub_822BE140(ctx, base);
  SR_LOG_BOOT(ctx.r3.u32 ? sr::LogSeverity::Error : sr::LogSeverity::Warn,
      "[RIGBPARSE] #%u wrap=%08X buf=%08X len=%X hdr=%08X %08X %08X %08X "
      "-> rc=%08X tid=%u%s",
      n, wrap, a4, a5, b0, b1, b2, b3, ctx.r3.u32,
      sr::kernel::getCurrentThreadHandle(),
      ctx.r3.u32 ? "  <<PARSE FAILED" : "");
}

extern "C" void __imp__sub_827042E0(PPCContext& __restrict ctx, uint8_t* base);
void sub_827042E0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  SR_LOG_BOOT(sr::LogSeverity::Error,
      "[TASKFLOW] DEFER #%u q=%08X task=%08X tid=%u lr=%08X  <<deferred re-post",
      n, ctx.r3.u32, ctx.r4.u32,
      sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
  __imp__sub_827042E0(ctx, base);
}

extern "C" void __imp__sub_826F4D98(PPCContext& __restrict ctx, uint8_t* base);
void sub_826F4D98(PPCContext& __restrict ctx, uint8_t* base) {
  uint32_t slotAddr = ctx.r3.u32, item = ctx.r4.u32;
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t prov = dlvBE32(base, item + 0x40);
  bool provLess = (prov == 0 && item >= 0x10000u);
  if (n <= 64 || (n % 64 == 0) || provLess) {
    uint32_t cursor = dlvBE32(base, item + 0x38);
    uint32_t np = dlvBE32(base, item + 0x18);
    char name[40] = {0};
    if (np >= 0x10000u && np < 0xC0000000u) {
      for (int i = 0; i < 39; ++i) {
        uint8_t* c = sr::kernel::guestVAToHost(np + i);
        if (!c || !*c || *c < 32 || *c >= 127) break;
        name[i] = (char)*c;
      }
    }
    SR_LOG_BOOT(provLess ? sr::LogSeverity::Error : sr::LogSeverity::Warn,
        "[SLOTPIN] #%u install slot=%08X item=%08X prov=%08X cursor=%08X "
        "name='%s' tid=%u lr=%08X%s",
        n, slotAddr, item, prov, cursor, name,
        sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr,
        provLess ? "  <<PROVIDER-LESS (slot-pin deadlock candidate)" : "");
  }
  __imp__sub_826F4D98(ctx, base);
}

void sub_82700820(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  bool logIt = (n <= 64) || (n % 64 == 0);
  uint32_t item = ctx.r3.u32;
  uint32_t sink = 0, sinkVt = 0, fnDeliver = 0, fnErr = 0;
  uint32_t st = 0, pay0 = 0, pay1 = 0, itemVt = 0;
  uint8_t f145 = 0, f142 = 0;
  if (logIt) {
    sink   = dlvBE32(base, item + 128);
    st     = dlvBE32(base, item + 12);
    pay0   = dlvBE32(base, item + 120);
    pay1   = dlvBE32(base, item + 124);
    itemVt = dlvBE32(base, item);
    f145   = dlvB8(base, item + 145);
    f142   = dlvB8(base, item + 142);
    if (sink && sink != 0xDEADDEADu) {
      sinkVt    = dlvBE32(base, sink);
      fnDeliver = dlvBE32(base, sinkVt + 20);
      fnErr     = dlvBE32(base, sinkVt + 24);
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[DLV] #%u pre item=%08X vt=%08X sink=%08X sinkVt=%08X "
        "vt20=%08X vt24=%08X st=%08X f145=%u f142=%u pay=%08X:%08X tid=%u lr=%08X",
        n, item, itemVt, sink, sinkVt, fnDeliver, fnErr, st,
        (unsigned)f145, (unsigned)f142, pay0, pay1,
        sr::kernel::getCurrentThreadHandle(), (uint32_t)ctx.lr);
  }
  __imp__sub_82700820(ctx, base);
  if (logIt) {
    uint32_t st2 = dlvBE32(base, item + 12);
    uint8_t f142b = dlvB8(base, item + 142);
    const char* branch;
    if (!sink || sink == 0xDEADDEADu)  branch = "NOSINK";
    else if (f145)                     branch = "FORCED-DELIVER";
    else if (st2 == 1u)                branch = "DELIVER";
    else if (st2 == 0xFFFFFFFEu)       branch = "ERRCB";
    else                               branch = "SWALLOW-PEND";
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[DLV] #%u post item=%08X st2=%08X f142=%u branch=%s vt20=%08X",
        n, item, st2, (unsigned)f142b, branch, fnDeliver);
  }
}

// ===========================================================================
// iter-401 [XMVOPEN] — bisect the terminal XMVCREATE hang (iter-400).
//
// Every run's last event is an XMVCREATE (sub_824715F0) that ENTERs and never
// EXITs; the previous pipeline's teardown completes cleanly and the create
// never reaches [XMV_BUILD] (player ctor sub_82470A58), so the blocker lives in
// sub_82470F28's case-2 prologue = source construction + open. Chain:
//
//   vt+96  sub_82476FC0  open dispatcher (descriptor type bits 1/2/4)
//     bit1 -> vt+108 sub_82476DA0  open by ANSI path (CreateFile + GetFileSize)
//     bit2 -> vt+104 sub_82476EB0  wide path -> narrows -> vt+108
//     bit4 -> vt+100 sub_82476F30  open from an EXISTING handle + base/length
//                                  (the XMVCREATE r5=base r6=len shape)
//   ...all three then call vt+116 sub_824770C0 = post-open init, which
//   allocates the block buffer, then per block creates a MediaStream
//   (sub_82482718 pool alloc + MediaStreamCtor_sub_824827C0) and calls its
//   vt+36 = the per-block READ SUBMISSION, finally vt+120 = start.
//
// A read submission that never completes is precisely the I/O-death signature
// this project keeps hitting, so log enter/exit around each rung: whichever
// rung shows ENTER with no EXIT is the blocking call. Observe-class, logging
// only, no guest state touched. Longhand (not macro-generated) so
// gen_override_manifest.py can classify each symbol.
// ===========================================================================

extern "C" void __imp__sub_82476FC0(PPCContext& __restrict ctx, uint8_t* base);
void sub_82476FC0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t self = ctx.r3.u32, desc = ctx.r4.u32;
  const uint32_t bits = (desc >= 0x1000u) ? sr::kernel::readGuestU32BE(desc) : 0u;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMVOPEN] FC0/dispatch ENTER #%u self=%08X desc=%08X bits=%08X lr=%08X tid=%lu",
      n, self, desc, bits, (uint32_t)ctx.lr, GetCurrentThreadId());
  __imp__sub_82476FC0(ctx, base);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMVOPEN] FC0/dispatch EXIT  #%u rc=%08X tid=%lu",
      n, ctx.r3.u32, GetCurrentThreadId());
}

extern "C" void __imp__sub_82476DA0(PPCContext& __restrict ctx, uint8_t* base);
void sub_82476DA0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMVOPEN] DA0/openpath ENTER #%u self=%08X path=%08X tid=%lu",
      n, ctx.r3.u32, ctx.r4.u32, GetCurrentThreadId());
  __imp__sub_82476DA0(ctx, base);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMVOPEN] DA0/openpath EXIT  #%u rc=%08X tid=%lu",
      n, ctx.r3.u32, GetCurrentThreadId());
}

extern "C" void __imp__sub_82476F30(PPCContext& __restrict ctx, uint8_t* base);
void sub_82476F30(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMVOPEN] F30/openhandle ENTER #%u self=%08X r4=%08X r5=%08X r6=%08X tid=%lu",
      n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, GetCurrentThreadId());
  __imp__sub_82476F30(ctx, base);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMVOPEN] F30/openhandle EXIT  #%u rc=%08X tid=%lu",
      n, ctx.r3.u32, GetCurrentThreadId());
}

extern "C" void __imp__sub_824770C0(PPCContext& __restrict ctx, uint8_t* base);
void sub_824770C0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMVOPEN] 0C0/postinit ENTER #%u self=%08X buf=%08X blockSz=%08X nBlocks=%08X a5=%08X tid=%lu",
      n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
      GetCurrentThreadId());
  __imp__sub_824770C0(ctx, base);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMVOPEN] 0C0/postinit EXIT  #%u rc=%08X tid=%lu",
      n, ctx.r3.u32, GetCurrentThreadId());
}

// Per-block progress inside the post-init loop: how many stream blocks were
// built before the stall (the loop is ctor -> vt+24 attach -> vt+36 submit).
extern "C" void __imp__sub_824827C0(PPCContext& __restrict ctx, uint8_t* base);
void sub_824827C0(PPCContext& __restrict ctx, uint8_t* base) {
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t raw = ctx.r3.u32;
  __imp__sub_824827C0(ctx, base);
  const uint32_t obj = ctx.r3.u32;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[XMVOPEN] streamctor #%u raw=%08X -> obj=%08X vt=%08X tid=%lu",
      n, raw, obj, obj >= 0x1000u ? sr::kernel::readGuestU32BE(obj) : 0u,
      GetCurrentThreadId());
}

// ===========================================================================
// iter-408 DIAGNOSTIC ([YUVCOPY]) — the decoded-frame -> texture copy.
//
// Standing measurement (iter-406d/407): the movie's video planes reach the GPU
// ALL-BLACK (fmt2 1280x720 Y + two 640x360 chroma, distinct=2, mean 63 = zero
// bytes + opaque alpha) while the WMV3 decoder demonstrably emits a full
// 1382400-byte YUV420 frame every ~35ms. The shader path is measured green
// (10/10 movie pixel shaders translated and cached, iter-406/406b) and the
// texture READ window was tested and exonerated (iter-407). So the break is
// the WRITE: the copy from decoded frame into the locked texture memory.
//
// MovieVideoDecodeYuvToRgbLoop_sub_82480D80(a1=player, a2, a3=outDelayMs,
// a4=destDesc) is that copy (IDA, this session). Its contract:
//   a4 == NULL          -> no copy here at all; the sample is handed to the
//                          media renderer instead (vt+64 sink, sub_82472DE0).
//   a4[0] == 1          -> PLANAR path: three memcpys, Y/U/V -> a4[3]/a4[4]/
//                          a4[5] with pitches a4[9]/a4[10]/a4[11]. This is the
//                          one that matches our three observed textures.
//   a4[0] == 255        -> VMX YUV->RGB convert into a4[3], pitch a4[5].
//   anything else       -> falls straight through, COPYING NOTHING.
// and the planar path is additionally guarded by (width & 1) == 0 &&
// (height & 1) == 0 — an odd dimension returns E_NOTIMPL (0x80004001) with no
// copy performed. width = player[58], height = player[59].
//
// So this probe is a decision tree, not a hunch: it prints which arm was taken,
// the destination VAs and pitches, and — the acceptance test — whether the
// destination actually holds non-zero bytes AFTER the call returns. If dest is
// non-zero here but the texture dump is still black, the break is downstream
// (lock pointer != texture memory); if dest is still zero, the break is here.
extern "C" void __imp__sub_82480D80(PPCContext& __restrict ctx, uint8_t* base);

static bool yuvCopyProbeEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_YUVCOPY_PROBE");
    return e && e[0] == '1';
  }();
  return v;
}

// Fault-free non-zero scan (HARD RULE: the harness VEH makes any host AV fatal,
// so this must never touch memory it has not range-checked). Returns the count
// of non-zero bytes in the first `n` bytes at `va`, or -1 if unmapped.
static int yuvNonZero(uint32_t va, uint32_t n) {
  if (va < 0x1000u) return -1;
  const uint8_t* p = sr::kernel::guestVAToHost(va);
  if (!p) return -1;
  if (!sr::kernel::isGuestVAInRange(va + n)) return -1;
  int nz = 0;
  for (uint32_t i = 0; i < n; ++i) if (p[i]) ++nz;
  return nz;
}

// iter-449t: the always-on EOM edge (cheap: one compare post-call). IDA this
// session pinned the two EOM paths inside this loop:
//   (a) 0x82481130 STATUS: vt+260(player) returns bits 1|2 -> state 7,
//       rc=0x16660026 — the queue is never consulted.
//   (b) 0x824810BC NULL-SAMPLE: ring item's vt+88() == 0 and the caller mode
//       word (player+220) bit1 CLEAR -> state 7, rc=0x16660026.
//   (c) 0x824818AC STOP: vt+252(player) bit0 set while ring empty ->
//       rc=0x166600E8.
// The edge log carries the mode word, the vtable slots (so the status/flags
// getters can be IDA'd from the log alone), the last-seen ring state, and
// the PSM cursor — movie 6's death prints exactly here.
static void fmvEomEdgeLog(uint32_t player, uint32_t rc, uint32_t lr) {
  if (rc != 0x16660026u && rc != 0x166600E8u) return;
  static std::atomic<unsigned> s_en{0};
  const unsigned en = s_en.fetch_add(1, std::memory_order_relaxed) + 1;
  if (en > 16) return;
  uint32_t mode = 0, vt = 0, f248 = 0, f252 = 0, f260 = 0, f268 = 0;
  if (player >= 0x1000u) {
    mode = sr::kernel::readGuestU32BE(player + 220u);
    vt = sr::kernel::readGuestU32BE(player);
    if (vt >= 0x1000u) {
      f248 = sr::kernel::readGuestU32BE(vt + 248u);
      f252 = sr::kernel::readGuestU32BE(vt + 252u);
      f260 = sr::kernel::readGuestU32BE(vt + 260u);
      f268 = sr::kernel::readGuestU32BE(vt + 268u);
    }
  }
  const uint32_t q = s_lastRingQ.load(std::memory_order_relaxed);
  uint32_t rd = 0, wr = 0, fl = 0;
  if (q >= 0x1000u) {
    rd = sr::kernel::readGuestU32BE(q + 56u);
    wr = sr::kernel::readGuestU32BE(q + 60u);
    fl = sr::kernel::readGuestU32BE(q + 68u);
  }
  SR_LOG_BOOT(sr::LogSeverity::Error,
      "[FMV_EOM] #%u rc=%08X player=%08X mode=%08X(bit1=%u) cursor=%d "
      "ring q=%08X rd=%u wr=%u fill=%u | vt=%08X st248=%08X fl252=%08X "
      "st260=%08X set268=%08X lr=%08X tid=%u",
      en, rc, player, mode, (mode >> 1) & 1u, (int)fmvPsmCursor(), q, rd, wr,
      fl, vt, f248, f252, f260, f268, lr,
      sr::kernel::getCurrentThreadHandle());
}

// iter-464 [MOVIE_PACE] (SR_MOVIE_PACE, default ON; OBSERVE-ONLY: it reads guest
// memory after the guest's own call has returned and writes nothing).
// Pacing contract (IDA, movie lane): RenderYuvVideoFrameTec0_sub_8235CA48 calls
// this frame-get ONCE per render-script fmvmovie pass. The body peeks the ring,
// copies the planes, then asks the CCalChannel (player[11] = player+0x2C) for
// drift = sampleMs(player+0x120) - clockMs(calch+0x120), where
// CalChannel_AdvanceClockGetDrift_sub_82476408 advances clock += (QPCms - last) *
// rate(calch+0x154) and stores drift at calch+0x138.
//   drift > 38 ms  -> HOLD: rd not advanced, the same frame is offered next call;
//   else           -> consume (AdvanceRead) and, if drift > 5, Sleep(drift - 5)
//                     on the calling (render) thread.
// There is NO catch-up drop: a late frame is still consumed and shown, so the
// video pace is min(movie fps, render-loop rate).
// Per-movie summary at EOM; optional per-call CSV (SR_MOVIE_PACE_CSV=<path>).
static bool moviePaceEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_MOVIE_PACE");
    return !e || e[0] != '0';
  }();
  return v;
}

namespace {
struct MoviePace {
  bool active = false;
  unsigned movie = 0;
  int64_t firstNs = 0, lastEntryNs = 0;
  unsigned calls = 0, frames = 0, holds = 0, late = 0, sleeps = 0;
  int32_t driftMin = 0, driftMax = 0;
  uint64_t tsFirst = 0, tsLast = 0;
  double sleepReqMs = 0, sleepCallMs = 0, inCallMs = 0, maxGapMs = 0;
  unsigned hist[7] = {};  // entry-to-entry: <25, <31, <36, <45, <60, <100, >=100 ms
  uint64_t swapAtStart = 0;
};
std::mutex g_mpMu;
MoviePace g_mp;
FILE* g_mpCsv = nullptr;
}  // namespace

static int64_t moviePaceNowNs() {
  using namespace std::chrono;
  static const auto origin = steady_clock::now();
  return duration_cast<nanoseconds>(steady_clock::now() - origin).count();
}

static double hostTimerResolutionMs() {
#ifdef _WIN32
  using QueryFn = LONG(WINAPI*)(PULONG, PULONG, PULONG);
  static QueryFn fn = reinterpret_cast<QueryFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution"));
  if (fn) {
    ULONG mn = 0, mx = 0, cur = 0;
    if (fn(&mn, &mx, &cur) == 0) return cur / 10000.0;
  }
#endif
  return -1.0;
}

static void moviePaceSummary(const MoviePace& m, const char* why, int64_t endNs) {
  const double wall = (endNs - m.firstNs) / 1e9;
  const double media = (m.tsLast - m.tsFirst) / 1000.0;
  const uint64_t swaps = sr::kernel::getVdSwapCount() - m.swapAtStart;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[MOVIE_PACE] movie#%u %s wall=%.2fs frames=%u calls=%u fps=%.2f media=%.2fs "
      "speed=%.3f holds=%u late=%u drift=[%d..%d]ms sleeps=%u req=%.0fms "
      "sleepCallMs=%.0f inCallMs=%.0f maxGap=%.1fms swaps=%llu swapHz=%.2f "
      "gaps<25/31/36/45/60/100/+=%u/%u/%u/%u/%u/%u/%u timerRes=%.3fms",
      m.movie, why, wall, m.frames, m.calls, wall > 0 ? m.frames / wall : 0.0, media,
      wall > 0 ? media / wall : 0.0, m.holds, m.late, m.driftMin, m.driftMax, m.sleeps,
      m.sleepReqMs, m.sleepCallMs, m.inCallMs, m.maxGapMs, (unsigned long long)swaps,
      wall > 0 ? swaps / wall : 0.0, m.hist[0], m.hist[1], m.hist[2], m.hist[3],
      m.hist[4], m.hist[5], m.hist[6], hostTimerResolutionMs());
}

static void moviePaceObserve(uint32_t player, uint32_t rc, int64_t t0, int64_t t1) {
  if (player < 0x1000u) return;
  const uint64_t ts = (uint64_t(sr::kernel::readGuestU32BE(player + 0x120u)) << 32) |
                      sr::kernel::readGuestU32BE(player + 0x124u);
  const uint32_t calch = sr::kernel::readGuestU32BE(player + 0x2Cu);
  int32_t drift = 0;
  uint64_t clockMs = 0;
  if (calch >= 0x1000u) {
    drift = (int32_t)sr::kernel::readGuestU32BE(calch + 0x138u);
    clockMs = (uint64_t(sr::kernel::readGuestU32BE(calch + 0x120u)) << 32) |
              sr::kernel::readGuestU32BE(calch + 0x124u);
  }
  const bool eom = rc == 0x16660026u || rc == 0x166600E8u;
  std::lock_guard<std::mutex> lk(g_mpMu);
  static bool s_csvInit = false;
  if (!s_csvInit) {
    s_csvInit = true;
    if (const char* p = std::getenv("SR_MOVIE_PACE_CSV"); p && *p) {
      g_mpCsv = std::fopen(p, "w");
      if (g_mpCsv)
        std::fprintf(g_mpCsv, "movie,call,t_entry_ms,in_call_ms,gap_ms,ts_ms,drift_ms,clock_ms,swaps,rc\n");
    }
  }
  MoviePace& m = g_mp;
  // A call more than 2 s after the previous one starts a new movie even without an EOM.
  if (m.active && !eom && (t0 - m.lastEntryNs) > 2000000000LL) {
    moviePaceSummary(m, "GAP", m.lastEntryNs);
    m.active = false;
  }
  if (!m.active) {
    const unsigned next = m.movie + 1;
    m = MoviePace{};
    m.movie = next;
    m.active = true;
    m.firstNs = t0;
    m.lastEntryNs = t0;
    m.swapAtStart = sr::kernel::getVdSwapCount();
    m.tsFirst = ts;
    m.tsLast = ts;
    m.driftMin = m.driftMax = drift;
  }
  const double gapMs = m.calls ? (t0 - m.lastEntryNs) / 1e6 : 0.0;
  const double inMs = (t1 - t0) / 1e6;
  if (m.calls) {
    const int b = gapMs < 25 ? 0 : gapMs < 31 ? 1 : gapMs < 36 ? 2 : gapMs < 45 ? 3
                : gapMs < 60 ? 4 : gapMs < 100 ? 5 : 6;
    m.hist[b]++;
    if (gapMs > m.maxGapMs) m.maxGapMs = gapMs;
  }
  m.calls++;
  m.lastEntryNs = t0;
  m.inCallMs += inMs;
  if (!eom) {
    if (m.calls == 1 || ts != m.tsLast) m.frames++;
    m.tsLast = ts;
    if (drift < m.driftMin) m.driftMin = drift;
    if (drift > m.driftMax) m.driftMax = drift;
    if (drift > 38) {
      m.holds++;
    } else {
      if (drift < 0) m.late++;
      if (drift > 5) {
        m.sleeps++;
        m.sleepReqMs += drift - 5;
        m.sleepCallMs += inMs;
      }
    }
  }
  if (g_mpCsv)
    std::fprintf(g_mpCsv, "%u,%u,%.3f,%.3f,%.3f,%llu,%d,%llu,%llu,%08X\n", m.movie, m.calls,
                 t0 / 1e6, inMs, gapMs, (unsigned long long)ts, drift,
                 (unsigned long long)clockMs,
                 (unsigned long long)sr::kernel::getVdSwapCount(), rc);
  if (eom) {
    moviePaceSummary(m, "EOM", t1);
    if (g_mpCsv) std::fflush(g_mpCsv);
    m.active = false;
  }
}

void sub_82480D80(PPCContext& __restrict ctx, uint8_t* base) {
  if (!yuvCopyProbeEnabled()) {
    const uint32_t player = ctx.r3.u32, lr = (uint32_t)ctx.lr;
    if (moviePaceEnabled()) {
      const int64_t t0 = moviePaceNowNs();
      __imp__sub_82480D80(ctx, base);
      moviePaceObserve(player, ctx.r3.u32, t0, moviePaceNowNs());
    } else {
      __imp__sub_82480D80(ctx, base);
    }
    fmvEomEdgeLog(player, ctx.r3.u32, lr);
    return;
  }

  const uint32_t player = ctx.r3.u32;
  const uint32_t desc   = ctx.r6.u32;
  // iter-440o ROOT-CAUSE PROBE: a2 (r4) is stored to player+220 on EVERY call
  // (a1[55]=a2) and IS the mode word the empty-pull path tests: bit1 (0x2)
  // clear -> END-OF-MOVIE 0x16660026; set -> soft-restart retry. So the
  // "retry bit" is CALLER-SUPPLIED, not hidden state. Log it + the caller LR
  // to find who passes what (and whether a second, correct caller exists).
  const uint32_t modeArg = ctx.r4.u32;
  const uint32_t callerLr = (uint32_t)ctx.lr;
  static std::atomic<unsigned> s_argN{0};
  {
    const unsigned an = s_argN.fetch_add(1, std::memory_order_relaxed) + 1;
    if (an <= 12 || an % 500 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[FRAMEGET_ARG] #%u player=%08X a2/mode=%08X (bit1=%u) lr=%08X",
          an, player, modeArg, (modeArg >> 1) & 1u, callerLr);
  }
  static std::atomic<unsigned> s_n{0};
  const unsigned n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool loud = (n <= 24) || (n % 250 == 0);

  uint32_t mode = 0, dY = 0, dU = 0, dV = 0, pY = 0, pU = 0, pV = 0;
  if (desc >= 0x1000u) {
    mode = sr::kernel::readGuestU32BE(desc + 0);
    dY   = sr::kernel::readGuestU32BE(desc + 12);
    dU   = sr::kernel::readGuestU32BE(desc + 16);
    dV   = sr::kernel::readGuestU32BE(desc + 20);
    pY   = sr::kernel::readGuestU32BE(desc + 36);
    pU   = sr::kernel::readGuestU32BE(desc + 40);
    pV   = sr::kernel::readGuestU32BE(desc + 44);
  }

  __imp__sub_82480D80(ctx, base);
  fmvEomEdgeLog(player, ctx.r3.u32, callerLr);

  const uint32_t rc = ctx.r3.u32;
  const uint32_t w  = player >= 0x1000u ? sr::kernel::readGuestU32BE(player + 232) : 0;
  const uint32_t h  = player >= 0x1000u ? sr::kernel::readGuestU32BE(player + 236) : 0;

  if (loud) {
    if (desc < 0x1000u) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[YUVCOPY] #%u NO-DESC (renderer-sink arm) player=%08X w=%u h=%u rc=%08X tid=%lu",
        n, player, w, h, rc, GetCurrentThreadId());
    } else {
      // Sample well inside the frame, not at row 0 — a letterboxed or
      // top-cropped frame can legitimately start with black rows.
      const uint32_t midY = (pY && h) ? dY + pY * (h / 2) : dY;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[YUVCOPY] #%u desc=%08X mode=%u w=%u h=%u even=%u "
        "Y=%08X/p%u U=%08X/p%u V=%08X/p%u nzY0=%d nzYmid=%d nzU=%d nzV=%d rc=%08X tid=%lu",
        n, desc, mode, w, h, (unsigned)(((w | h) & 1) == 0),
        dY, pY, dU, pU, dV, pV,
        yuvNonZero(dY, 256), yuvNonZero(midY, 256),
        yuvNonZero(dU, 256), yuvNonZero(dV, 256),
        rc, GetCurrentThreadId());
    }
  }
}

#endif

#if !SR_ORACLE
// Uncapped natural-run counters used by run_recomp_x. No pointer observation
// or forced clip publication is needed to count the real guest entries.
extern "C" void __imp__sub_826B2298(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826B0458(PPCContext& __restrict ctx, uint8_t* base);
void sub_826B2298(PPCContext& __restrict ctx, uint8_t* base) {
  sr::kernel::gaugeBump(sr::kernel::Gauge::DefExec);
  __imp__sub_826B2298(ctx, base);
}
void sub_826B0458(PPCContext& __restrict ctx, uint8_t* base) {
  sr::kernel::gaugeBump(sr::kernel::Gauge::ClipCtorAll);
  if (ctx.r5.u32 >= 0x10000u) sr::kernel::gaugeBump(sr::kernel::Gauge::ClipCtorReal);
  __imp__sub_826B0458(ctx, base);
}
#endif
