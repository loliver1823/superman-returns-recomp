// Auto-generated import glue - routes __imp__* to ExportRegistry
// Run: python tools/gen_import_glue.py

#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/apu/xma_api.hpp"   // Macro-independent native import API
#include "app/log.hpp"
#include "kernel/native_brackets.hpp"   // NATIVE N0.0: SR_NATIVE_FRAMESPLIT import timing
#include "kernel/quit.hpp"   // tools/graceful-quit: the import gate (entry / exit) and the guest-lock count
#include "kernel/gpu_submit.hpp"   // step 6 G6a: [APC_STUCK]'s swap cadence (diagSetSwapCensusHook)
#if SR_AUDIT_IMPORTS
#include "kernel/import_audit.hpp"   // step4/audit: SR_AUDIT_IMPORTS (audit builds only) - the per-import census
#endif
#include <memory>
#include <atomic>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <cstdlib>
#include <cstring>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

// When game passes null to Rtl*CriticalSection, substitute this VA (same as fallback in guest_context).
constexpr uint32_t kNullCritSectSubstituteVA = 0x00002000u;

// Forward declaration (defined below callKernel).
void processGuestAPCs(PPCContext& ctx, uint8_t* base, uint32_t threadHandle);

// Resolve an export ONCE, then return the cached pointer on subsequent calls.
// Each call site uses its own static fn pointer, so no mutex after first call.
static sr::kernel::ExportFn resolveExport(const char* name, sr::kernel::ExportFn& cached) {
  if (cached) return cached;
  cached = sr::kernel::ExportRegistry::instance().getExport(name);
  return cached;
}

// iter-402: the [GUEST #N] import trace is opt-in (SR_IMPORT_TRACE=1).
static bool importTraceEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_IMPORT_TRACE");
    return e && e[0] == '1';
  }();
  return s_on;
}

// Only consulted when the trace is ON. Compares the literal POINTER first —
// call sites pass string literals with static storage, so the common case is a
// pointer hit and never a strcmp.
static bool importTraceSuppressed(const char* name) {
  static const char* const kHot[] = {
      "RtlEnterCriticalSection", "RtlLeaveCriticalSection", "NtYieldExecution",
      "KeTlsGetValue",           "KeTlsSetValue",           "KeQueryPerformanceCounter",
      "KeEnterCriticalRegion",   "KeLeaveCriticalRegion",   "RtlTryEnterCriticalSection",
  };
  for (const char* h : kHot)
    if (name == h || std::strcmp(name, h) == 0) return true;
  return false;
}

// Fast kernel call: resolves export once (first call), then calls it directly.
// Avoids mutex+hashmap lookup on every subsequent call, preventing ntdll crash
// from repeated std::string allocation + SRWLOCK contention.
static uint32_t callKernelFast(sr::kernel::ExportFn& cachedFn, const char* name,
                               PPCContext& ctx, uint8_t*) {
  // tools/graceful-quit (kernel/quit.hpp): the import gate. Until a harness-requested quit it is a relaxed load of
  // the cut flag and this thread's "inside <import>" mark (one store each way); at a quit's thread cut the thread
  // parks HERE before the import runs, or at the exit below before any guest code - unless it holds a guest lock.
  const char* quitPrev = sr::kernel::quit::importEnter(name);
  sr::kernel::setCurrentGuestLR(static_cast<uint32_t>(ctx.lr));
  sr::kernel::ExportRegistry::setCurrentExtraArgs(ctx.r9.u32, ctx.r10.u32);
  sr::kernel::ExportRegistry::setCurrentR13(ctx.r13.u32);   // iter-463 SR_KPCR: caller's KPCR
  sr::kernel::incrementMainThreadCallCount(name);

  sr::kernel::ExportFn fn = resolveExport(name, cachedFn);
  uint32_t result = 0;
#if SR_AUDIT_IMPORTS
  // step4/audit census: the entry before the export runs, the return right after it (before the exit gate)
  const sr::audit::ImportAuditToken auditTok = sr::audit::importEnter(
      name, static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
#endif
  if (fn) {
    // NATIVE N0.0 (SR_NATIVE_FRAMESPLIT, default OFF): time spent in kernel imports.
    const uint64_t nativeT0 = sr::native::g_frameSplitOn ? sr::native::importEnter() : 0;
    result = fn(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
    if (nativeT0) sr::native::importLeave(nativeT0);
  }
#if SR_AUDIT_IMPORTS
  sr::audit::importLeave(auditTok, result, fn != nullptr);
#endif
  sr::kernel::quit::importLeave(name, quitPrev);  // tools/graceful-quit: the exit gate
  if (fn) {
    // iter-402: this trace was 95.4% of the log — 2,513,488 of 2,636,011 lines
    // in a 446 MB run, i.e. 1.3 GB per K=3 batch, and every analysis grep paid
    // for it. The 1-in-500 sampling is not enough when the guest makes ~1.25
    // BILLION import calls, and unlike ExportRegistry::callOrStub's copy of
    // this logging, the fast path had no high-frequency name filter, so three
    // names produced the entire bulk: RtlLeaveCriticalSection (859,733),
    // RtlEnterCriticalSection (859,194), NtYieldExecution (793,176).
    // Now opt-in via SR_IMPORT_TRACE=1 (the env read is a one-time static, so
    // the default path costs a single bool test on this very hot call).
    // When enabled, the hot names are still filtered so the trace is readable.
    if (importTraceEnabled() && !importTraceSuppressed(name)) {
      static std::atomic<uint32_t> s_callNo{0};
      uint32_t n = s_callNo.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 100 || (n % 500 == 0))
        SR_LOG_KERNEL_IMPORT("[GUEST #%u] %s (r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X) -> %08X",
                             n, name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                             ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, result);
    }
  }
  return result;
}

// Fast export cache: maps string literal pointers to resolved ExportFn.
// String literals have static storage duration so the pointer is stable.
// Lock-free: only the main thread + a few worker threads call kernel imports,
// and each slot is written once (null→fn) then only read.
static constexpr int kExportCacheSize = 256;
struct ExportCacheEntry {
  const char* name;
  sr::kernel::ExportFn fn;
};
static ExportCacheEntry g_exportCache[kExportCacheSize];
static std::atomic<int> g_exportCacheUsed{0};

// iter-464 perf lane: an open-addressed hash on the name POINTER in front of the linear
// list below (every kernel import - ~2M calls/s - scanned up to 256 entries). Same
// pointer-identity semantics. A slot's name is claimed first, then its fn published; a
// reader that finds the name before the fn takes the linear path for that one call.
struct ExportHashSlot {
  std::atomic<const char*> name{nullptr};
  std::atomic<sr::kernel::ExportFn> fn{nullptr};
};
static ExportHashSlot g_exportHash[1024];

static sr::kernel::ExportFn lookupCachedLinear(const char* name);

static sr::kernel::ExportFn lookupCached(const char* name) {
  const size_t h = size_t((uint64_t(uintptr_t(name)) * 0x9E3779B97F4A7C15ull) >> 54);  // 10 bits
  for (size_t i = 0; i < 16; ++i) {
    ExportHashSlot& s = g_exportHash[(h + i) & 1023];
    const char* k = s.name.load(std::memory_order_acquire);
    if (k == name) {
      if (sr::kernel::ExportFn fn = s.fn.load(std::memory_order_acquire)) return fn;
      return lookupCachedLinear(name);
    }
    if (!k) {
      sr::kernel::ExportFn fn = lookupCachedLinear(name);
      if (!fn) return fn;  // unresolved: never cache a null (the registry may fill later)
      const char* expected = nullptr;
      if (s.name.compare_exchange_strong(expected, name, std::memory_order_acq_rel)) {
        s.fn.store(fn, std::memory_order_release);
        return fn;
      }
      if (expected == name) return fn;
      // Lost this slot to another name: keep probing.
    }
  }
  return lookupCachedLinear(name);
}

static sr::kernel::ExportFn lookupCachedLinear(const char* name) {
  int used = g_exportCacheUsed.load(std::memory_order_acquire);
  if (used > kExportCacheSize) used = kExportCacheSize;
  for (int i = 0; i < used; ++i) {
    if (g_exportCache[i].name == name)  // pointer compare, not strcmp
      return g_exportCache[i].fn;
  }
  // Cache miss: resolve and add (only if room)
  sr::kernel::ExportFn fn = sr::kernel::ExportRegistry::instance().getExport(name);
  int slot = g_exportCacheUsed.load(std::memory_order_relaxed);
  if (slot >= kExportCacheSize) return fn;
  if (g_exportCacheUsed.compare_exchange_strong(slot, slot + 1, std::memory_order_acq_rel)) {
    g_exportCache[slot].fn = fn;
    g_exportCache[slot].name = name;
  }
  return fn;
}

// Track the last few kernel calls for crash diagnostics.
//
// fix/postgame-apt (lane K, 2026-10-17) PLATFORM FIX: the ring counters were `int` and the slot was
// `idx % kCallRingSize`. A busy guest thread makes ~2.5 M imports a second (the game-kernel runners'
// RtlEnter/LeaveCriticalSection + NtYieldExecution idle loop), so after 2^31 calls (~14 min) the
// counter went negative and the slot became -7..-1: every 8th..14th import wrote 16-byte entries up to
// 112 bytes BEFORE t_callRing, i.e. over the thread's other thread_locals (dev exe f1379269fc82:
// t_callRing at TLS+0x618F0 - the R3_a2 thread-fatal's `[THREAD_MEM] release of unknown stack 00007FF6`
// is a ring entry's name-pointer high dword in t_allocBase, TLS+0x61894; g_deliveringAPCs, quit's
// t_exempt, the audio sleep timer handle, ... sit in the same 112 bytes) or, when the ring is the TLS
// block's first object (release exe bd8765cb69fe: TLS+0x20), over the heap entry header in front of the
// TLS block - the R3 release crash 0xC0000374 in RtlpCoalesceFreeBlocks when the neighbouring thread
// exited (error address = that header). The MAIN guest thread also writes g_mainCallRing (a static) with
// its own `int` counter, so the same crossing on the main thread wrote 112 bytes in front of that array in
// the image's .data - dev exe f1379269fc82: two $TSS0 guards, fmt's format_facet<std::locale>::id, the whole
// APC queue map g_apcQueues (std::unordered_map: list head, size, bucket vector, mask, maxidx) and
// g_lastKernelCallName / g_inKernelCall (cdb on the exe's PDB + the APC code's RIP-relative refs, lane K
// NOTES 19:50). The R3_a2 thread that died was the main guest thread (runGuestHarness at its stack base).
// The counters are unsigned now and the slot is a mask: always in [0, kCallRingSize) whatever the count.
// (Old behaviour for the A/B, both rings: SR_DIAG_CALLRING_LEGACY=1.)
static constexpr uint32_t kCallRingSize = 8;   // a power of two: the slot is (count & kCallRingMask)
static constexpr uint32_t kCallRingMask = kCallRingSize - 1;
static_assert((kCallRingSize & kCallRingMask) == 0, "kCallRingSize must be a power of two");
struct CallRingEntry { const char* name; uint32_t lr; uint32_t r3; };
static thread_local CallRingEntry t_callRing[kCallRingSize];
static thread_local uint32_t t_callRingIdx = 0;
static thread_local bool t_callRingSeeded = false;

// Global crash ring for main thread — accessible from guest_harness crash handler.
static CallRingEntry g_mainCallRing[kCallRingSize];
static uint32_t g_mainCallRingIdx = 0;
static bool g_mainCallRingSeeded = false;   // SR_DIAG_CALLRING_START, main ring (written by the main thread only)

// SR_DIAG_CALLRING_START=<n> (DIAGNOSTIC, default off): every thread's ring counter (and the main thread's
// g_mainCallRing counter) starts at n (decimal or 0x hex) instead of 0, so the 2^31 crossing that took ~14
// minutes of a busy thread happens within seconds of boot. SR_DIAG_CALLRING_LEGACY=1 (DIAGNOSTIC, default off): the pre-fix slot arithmetic (signed
// remainder, slots -7..-1 write in front of the ring) - reproduces the corruption on purpose for the A/B.
// Neither touches guest memory or a guest-visible value; both exist only to prove the fix.
struct CallRingDiag {
  bool seeded = false;
  uint32_t start = 0;
  bool legacy = false;
};
static const CallRingDiag& callRingDiag() {
  static const CallRingDiag d = [] {
    CallRingDiag c;
    if (const char* e = std::getenv("SR_DIAG_CALLRING_START"); e && *e) {
      c.seeded = true;
      c.start = static_cast<uint32_t>(std::strtoull(e, nullptr, 0));
    }
    const char* l = std::getenv("SR_DIAG_CALLRING_LEGACY");
    c.legacy = l && l[0] == '1';
    return c;
  }();
  return d;
}

// The ring slot for call number n (the test seam: tools/difftest/callring_slot_test.cpp).
inline uint32_t callRingSlot(uint32_t n) { return n & kCallRingMask; }
// The pre-fix arithmetic, kept only for SR_DIAG_CALLRING_LEGACY (and the test's mutant check).
inline int callRingSlotLegacy(uint32_t n) { return static_cast<int>(n) % static_cast<int>(kCallRingSize); }

// One ring record into `ring` (kCallRingSize entries) at counter `idx`: callKernel's writer of t_callRing, and the
// one the test drives against its own guarded storage (tools/difftest/callring_test.cpp).
inline void recordCallRingTo(CallRingEntry* ring, uint32_t& idx, const char* name, uint32_t lr, uint32_t r3,
                             bool legacy) {
  const CallRingEntry e{name, lr, r3};
  if (__builtin_expect(legacy, 0))
    ring[callRingSlotLegacy(idx)] = e;   // SR_DIAG_CALLRING_LEGACY: the pre-fix bug, on purpose
  else
    ring[callRingSlot(idx)] = e;
  idx++;
}
inline void recordCallRing(const char* name, uint32_t lr, uint32_t r3, bool legacy) {
  recordCallRingTo(t_callRing, t_callRingIdx, name, lr, r3, legacy);
}
static const char* g_lastKernelCallName = nullptr;
static bool g_inKernelCall = false;

uint32_t callKernel(const char* name, PPCContext& ctx, uint8_t*) {
  // tools/graceful-quit (kernel/quit.hpp): the import gate's entry - see callKernelFast.
  const char* quitPrev = sr::kernel::quit::importEnter(name);
  sr::kernel::setCurrentGuestLR(static_cast<uint32_t>(ctx.lr));
  sr::kernel::ExportRegistry::setCurrentExtraArgs(ctx.r9.u32, ctx.r10.u32);
  sr::kernel::ExportRegistry::setCurrentR13(ctx.r13.u32);   // iter-463 SR_KPCR: caller's KPCR
  sr::kernel::incrementMainThreadCallCount(name);

  // Record in ring buffer for crash diagnostics
  const CallRingDiag& diag = callRingDiag();
  if (__builtin_expect(diag.seeded && !t_callRingSeeded, 0)) {
    t_callRingSeeded = true;
    t_callRingIdx = diag.start;
  }
  recordCallRing(name, static_cast<uint32_t>(ctx.lr), ctx.r3.u32, diag.legacy);
  // Main thread global ring — accessible from guest_harness crash handler
  if (sr::kernel::isMainGuestThread()) {
    if (__builtin_expect(diag.seeded && !g_mainCallRingSeeded, 0)) {
      g_mainCallRingSeeded = true;
      g_mainCallRingIdx = diag.start;
    }
    recordCallRingTo(g_mainCallRing, g_mainCallRingIdx, name, static_cast<uint32_t>(ctx.lr), ctx.r3.u32,
                     diag.legacy);
    g_lastKernelCallName = name;
    g_inKernelCall = true;
  }

  // Use cached lookup — no mutex or hash map after first call per export.
  sr::kernel::ExportFn fn = lookupCached(name);
  uint32_t result = 0;
  volatile bool quitLeft = false;  // tools/graceful-quit: the exit gate ran (else the __except arm runs it)
#if SR_AUDIT_IMPORTS
  // step4/audit census: the ENTRY is counted before the export runs, so a call that never returns (ExTerminateThread,
  // a wait blocked to the end, a faulting export - the __except arm below - or a thread parked at the exit gate)
  // still counts; the RETURN is counted right after the export, before the exit gate.
  const sr::audit::ImportAuditToken auditTok = sr::audit::importEnter(
      name, static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
#endif
  __try {
    if (fn) {
      // NATIVE N0.0 (SR_NATIVE_FRAMESPLIT, default OFF): time spent in kernel imports.
      const uint64_t nativeT0 = sr::native::g_frameSplitOn ? sr::native::importEnter() : 0;
      result = fn(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                  ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
      if (nativeT0) sr::native::importLeave(nativeT0);
    }
#if SR_AUDIT_IMPORTS
    sr::audit::importLeave(auditTok, result, fn != nullptr);
#endif
    // tools/graceful-quit: the exit gate, right after the export returns - before the trace and before any guest
    // APC runs below: a thread that waited in this import through a quit's thread cut parks here, holding no lock.
    quitLeft = true;
    sr::kernel::quit::importLeave(name, quitPrev);
    // Logging: first 100 + every 500th, opt-in (see importTraceEnabled).
    if (importTraceEnabled() && !importTraceSuppressed(name)) {
      static std::atomic<uint32_t> s_logCallNo{0};
      uint32_t n = s_logCallNo.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 100 || (n % 500 == 0))
        SR_LOG_KERNEL_IMPORT("[GUEST #%u] %s (r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X) -> %08X",
                             n, name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                             ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, result);
    }
    // Xbox 360 delivers pending user-mode APCs at every kernel→user transition.
    uint32_t myHandle = sr::kernel::getCurrentThreadHandle();
    if (myHandle != 0)
      processGuestAPCs(ctx, nullptr, myHandle);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    DWORD code = GetExceptionCode();
    if (code == 0xC00000FDu) _resetstkoflw();  // restore guard page after stack overflow
    fprintf(stderr, "[KERNEL_CRASH] name=%s lr=0x%08X r3=0x%08X exception=0x%08lX\n",
            name, (unsigned)ctx.lr, ctx.r3.u32, code);
    fprintf(stderr, "[KERNEL_CRASH] Recent calls (newest first):\n");
    for (uint32_t i = 0; i < kCallRingSize; ++i) {
      const uint32_t idx = callRingSlot(t_callRingIdx - 1u - i);
      if (t_callRing[idx].name)
        fprintf(stderr, "  [%u] %s lr=0x%08X r3=0x%08X\n",
                i, t_callRing[idx].name, t_callRing[idx].lr, t_callRing[idx].r3);
    }
    fflush(stderr);
    // Don't terminate — return 0 and let the caller continue
    if (!quitLeft) sr::kernel::quit::importLeave(name, quitPrev);  // tools/graceful-quit: never left "inside"
  }

  if (sr::kernel::isMainGuestThread()) g_inKernelCall = false;
  return result;
}

// --- Guest APC delivery ---
// Xbox 360 NtQueueApcThread queues a user-mode APC that the kernel delivers
// when the target thread enters an alertable wait. The APC routine runs on the
// target thread before the wait returns. Our host stub only fires a no-op APC
// to wake the thread; this layer actually calls the guest APC function.
struct GuestAPC {
  uint32_t routine;  // guest VA of APC routine (e.g. sub_82469E38)
  uint32_t arg1;     // NormalContext
  uint32_t arg2;     // SystemArgument1
  uint32_t arg3;     // SystemArgument2
};

// Use atomic spinlock instead of std::mutex to avoid ntdll SRWLOCK crashes.
// The SRWLOCK in ntdll can become corrupted if a thread crashes while holding it,
// causing subsequent lock attempts to read from address 0x0.
static std::atomic_flag g_apcLock = ATOMIC_FLAG_INIT;
static std::unordered_map<uint32_t, std::queue<GuestAPC>> g_apcQueues;

#if SR_ORACLE
static std::atomic<uint32_t> g_apcHolderTid{0};   // [APC_LOCK_SLOW]: the thread that last took g_apcLock
#endif
// step 6 G14b: the guard never proceeds without the lock. The old one broke out after 100000 spins and entered the
// critical section unlocked, and its destructor then cleared ANOTHER holder's lock (a data race on g_apcQueues). Past
// that budget it now yields and keeps trying; nothing that can fault runs under the lock (the guest APC runs outside
// it, SEH-protected), so no holder dies inside.
struct ApcSpinGuard {
  static constexpr uint32_t kSpinBudget = 100000;
  std::atomic_flag& flag;
  ApcSpinGuard(std::atomic_flag& f) : flag(f) {
    uint32_t spins = 0;
#if SR_ORACLE
    uint64_t slowStart = 0;
    uint32_t holder = 0;
#endif
    while (flag.test_and_set(std::memory_order_acquire)) {
      if (++spins < kSpinBudget) continue;
#if SR_ORACLE
      if (spins == kSpinBudget) {
        slowStart = GetTickCount64();
        holder = g_apcHolderTid.load(std::memory_order_relaxed);
      }
#endif
      SwitchToThread();
    }
#if SR_ORACLE
    g_apcHolderTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (spins >= kSpinBudget) {
      static std::atomic<uint32_t> s_slow{0};
      const uint32_t n = s_slow.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 16 || n % 1000 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn, "[APC_LOCK_SLOW] #%u waiter tid=%lu holder tid=%u waited %llu ms past %u spins",
                    n, GetCurrentThreadId(), holder, (unsigned long long)(GetTickCount64() - slowStart), kSpinBudget);
    }
#endif
  }
  ~ApcSpinGuard() { flag.clear(std::memory_order_release); }
};
static std::atomic<bool> g_anyAPCsPending{false};
static thread_local bool g_deliveringAPCs = false;
#if SR_ORACLE
// step 6 G6a [APC_STUCK] (dev): per handle, the APCs queued and delivered, kept beside g_apcQueues (its type stays what
// release builds have) and written under g_apcLock.
struct ApcStat {
  uint64_t enqueued = 0, delivered = 0, censusDelivered = 0;
  bool censusNonEmpty = false;
};
static std::unordered_map<uint32_t, ApcStat> g_apcStats;
#endif

void enqueueGuestAPC(uint32_t targetThreadHandle, uint32_t routine,
                     uint32_t arg1, uint32_t arg2, uint32_t arg3) {
  {
    ApcSpinGuard lock(g_apcLock);
    g_apcQueues[targetThreadHandle].push({routine, arg1, arg2, arg3});
#if SR_ORACLE
    ++g_apcStats[targetThreadHandle].enqueued;
#endif
  }
  g_anyAPCsPending.store(true, std::memory_order_release);
}

// iter-463 platform fix (restoreHostFpState): `ctx = saved` restores only the CACHED
// fpscr.csr. The host MXCSR is the live FP state, and enable/disableFlushMode() only call
// setcsr when the cached value differs, so an APC that switched FTZ/DAZ (every VMX op does
// printSetFlushMode(true)) or the rounding mode (mtfsf) left the interrupted guest code
// running under the APC's MXCSR indefinitely: scalar FP flushed to zero, or VMX without
// flush. Every restore below re-applies the restored csr to the host (setcsr ORs in the
// 0x1F80 exception masks). Test: build/difftest.exe --fp-platform-test (A1..A4).
// SEH-protected APC call — no C++ objects with destructors allowed in this function.
// Returns true if the call succeeded, false if it crashed.
#if defined(_WIN32)
static bool callGuestAPCSafe(PPCContext& ctx, uint32_t routine, uint32_t arg1,
                             uint32_t arg2, uint32_t arg3) {
  // PPC_CALL_INDIRECT_FUNC macro references 'base' in its expansion
  uint8_t* base = sr::kernel::getGuestBase();
  PPCContext saved = ctx;
  ctx.r3.u32 = arg1;
  ctx.r4.u32 = arg2;
  ctx.r5.u32 = arg3;

  __try {
    PPC_CALL_INDIRECT_FUNC(routine);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    DWORD code = GetExceptionCode();
    fprintf(stderr, "[APC_CRASH] routine=0x%08X exception=0x%08lX arg1=0x%08X "
            "arg2=0x%08X arg3=0x%08X lr=0x%08X r1=0x%08X\n",
            routine, code, arg1, arg2, arg3,
            (uint32_t)ctx.lr, ctx.r1.u32);
    ctx = saved;
    ctx.fpscr.setcsr(ctx.fpscr.csr);   // see restoreHostFpState below
    return false;
  }
  ctx = saved;
  ctx.fpscr.setcsr(ctx.fpscr.csr);   // see restoreHostFpState below
  return true;
}
#endif

void processGuestAPCs(PPCContext& ctx, uint8_t* /*base*/, uint32_t threadHandle) {
  if (!g_anyAPCsPending.load(std::memory_order_acquire)) return;
  if (g_deliveringAPCs) return;

  std::queue<GuestAPC> pending;
  {
    ApcSpinGuard lock(g_apcLock);
    auto it = g_apcQueues.find(threadHandle);
    if (it == g_apcQueues.end() || it->second.empty()) return;
    std::swap(pending, it->second);
#if SR_ORACLE
    g_apcStats[threadHandle].delivered += pending.size();
#endif
    bool anyLeft = false;
    for (auto& [k, q] : g_apcQueues)
      if (!q.empty()) { anyLeft = true; break; }
    if (!anyLeft) g_anyAPCsPending.store(false, std::memory_order_release);
  }

  g_deliveringAPCs = true;
  static std::atomic<int> s_apcDeliverLog{0};
  static std::atomic<int> s_apcCrashCount{0};

  while (!pending.empty()) {
    GuestAPC apc = pending.front();
    pending.pop();

    int n = s_apcDeliverLog.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 30 || (n % 500 == 0))
      SR_LOG_KERNEL_IMPORT("[APC_DELIVER] thread=%u routine=0x%08X arg1=0x%08X arg2=0x%08X arg3=0x%08X (call#%d)",
                           threadHandle, apc.routine, apc.arg1, apc.arg2, apc.arg3, n);

#if defined(_WIN32)
    if (!callGuestAPCSafe(ctx, apc.routine, apc.arg1, apc.arg2, apc.arg3)) {
      int crashes = s_apcCrashCount.fetch_add(1, std::memory_order_relaxed) + 1;
      fprintf(stderr, "[APC_CRASH] Survived APC crash #%d, continuing\n", crashes);
      continue;
    }
#else
    PPCContext saved = ctx;
    ctx.r3.u32 = apc.arg1;
    ctx.r4.u32 = apc.arg2;
    ctx.r5.u32 = apc.arg3;
    PPC_CALL_INDIRECT_FUNC(apc.routine);
    ctx = saved;
    ctx.fpscr.setcsr(ctx.fpscr.csr);
#endif
  }
  g_deliveringAPCs = false;
}

}  // anonymous namespace

#if SR_ORACLE
// step 6 G6a [APC_STUCK] (dev): every 120 swaps (kernel/gpu_submit.cpp onSwap). A queue that held an APC at the last
// census and has delivered none since is stuck: no thread with that handle returned through callKernel. Such an APC
// keeps the global flag set, so every import on every thread then takes g_apcLock. Logs after the unlock. Returns the
// number of stuck queues.
uint32_t apcStuckCensus(uint64_t swaps) {
  struct Stuck { uint32_t handle; size_t queued; uint64_t enqueued, delivered; };
  Stuck stuck[8];
  uint32_t nStuck = 0, nonEmpty = 0, queues = 0;
  {
    ApcSpinGuard lock(g_apcLock);
    for (auto& [handle, q] : g_apcQueues) {
      ++queues;
      ApcStat& s = g_apcStats[handle];
      const bool held = !q.empty();
      nonEmpty += held;
      if (held && s.censusNonEmpty && s.delivered == s.censusDelivered) {
        if (nStuck < 8) stuck[nStuck] = {handle, q.size(), s.enqueued, s.delivered};
        ++nStuck;
      }
      s.censusNonEmpty = held;
      s.censusDelivered = s.delivered;
    }
  }
  static uint32_t s_census = 0, s_lines = 0;   // the CP thread only
  ++s_census;
  if (s_census <= 4 || (nStuck && s_lines < 16) || s_census % 100 == 0)
    SR_LOG_BOOT(nStuck ? sr::LogSeverity::Warn : sr::LogSeverity::Info,
                "[APC_STUCK] census swap=%llu queues=%u non_empty=%u stuck=%u", (unsigned long long)swaps, queues,
                nonEmpty, nStuck);
  for (uint32_t i = 0; i < nStuck && i < 8 && s_lines < 16; ++i, ++s_lines)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[APC_STUCK] handle=%u queued=%zu enqueued=%llu delivered=%llu (no delivery in "
                "120 swaps)", stuck[i].handle, stuck[i].queued, (unsigned long long)stuck[i].enqueued,
                (unsigned long long)stuck[i].delivered);
  return nStuck;
}
static void apcStuckCensusHook(uint64_t swaps) { (void)apcStuckCensus(swaps); }
// The CP runs it every 120 swaps, on its own thread (kernel/gpu_submit.hpp diagSetSwapCensusHook).
static const bool s_apcStuckHook = (sr::kernel::diagSetSwapCensusHook(&apcStuckCensusHook), true);
// step 6 G6a test seam (tools/difftest/diag_g6a_test.cpp): one census, as the CP would run it.
uint32_t srTestApcStuckCensus(uint64_t swaps) { return apcStuckCensus(swaps); }
#endif

// Test seam (tools/difftest/fp_platform_test.cpp): the REAL queue + delivery path, so the
// MXCSR-restore test exercises callGuestAPCSafe itself, not a copy of it.
void srTestQueueGuestAPC(uint32_t threadHandle, uint32_t routine, uint32_t arg1,
                         uint32_t arg2, uint32_t arg3) {
  enqueueGuestAPC(threadHandle, routine, arg1, arg2, arg3);
}
void srTestDeliverGuestAPCs(PPCContext& ctx, uint32_t threadHandle) {
  processGuestAPCs(ctx, nullptr, threadHandle);
}
// step 6 G14b test seams (tools/difftest/apc_guard_test.cpp): hold / release g_apcLock as another holder would, whether
// it is held now, and a queue's length.
void srTestApcLock(bool take) {
  if (take) {
    while (g_apcLock.test_and_set(std::memory_order_acquire)) {
    }
  } else {
    g_apcLock.clear(std::memory_order_release);
  }
}
bool srTestApcLockHeld() {
  if (g_apcLock.test_and_set(std::memory_order_acquire)) return true;
  g_apcLock.clear(std::memory_order_release);
  return false;
}
uint32_t srTestApcQueued(uint32_t threadHandle) {
  ApcSpinGuard lock(g_apcLock);
  const auto it = g_apcQueues.find(threadHandle);
  return it == g_apcQueues.end() ? 0u : uint32_t(it->second.size());
}

// Test seams (tools/difftest/callring_test.cpp).
//
// srTestCallRingOwned: callKernel's ring writer (recordCallRingTo) against storage the TEST owns - a ring with
// guard bytes in front (where the pre-fix slots -7..-1 wrote) and behind - from counter `seed` for `calls` records.
// Returns the number of guard bytes changed; *entriesOk = the ring holds the last min(calls, 8) records, newest
// first, at the slots the counter names (so a writer that stores nothing, or stores elsewhere, fails).
namespace {
constexpr uint8_t kGuardByte = 0xA5;
struct TestRingBox {
  uint8_t front[8 * sizeof(CallRingEntry)];
  CallRingEntry ring[kCallRingSize];
  uint8_t back[4 * sizeof(CallRingEntry)];
};
const char kTestRingName[] = "SrTestCallRing";
const char kTestImportName[] = "SrTestNoSuchImport";   // no export has it: callKernel calls nothing
}  // namespace
uint32_t srTestCallRingOwned(uint32_t seed, uint32_t calls, bool legacy, uint32_t* endIdx, bool* entriesOk) {
  // On the heap, not a static: a static box sat in .data right in front of g_mainCallRing (k6), where the
  // pre-fix exe had g_apcQueues - SR_DIAG_CALLRING_LEGACY's main-ring A/B must hit what the pre-fix exe hit.
  // The mutant's out-of-range writes land in `front`, inside this object.
  std::unique_ptr<TestRingBox> owned(new TestRingBox);
  TestRingBox& box = *owned;
  std::memset(&box, kGuardByte, sizeof(box));
  uint32_t idx = seed;
  for (uint32_t i = 0; i < calls; ++i) recordCallRingTo(box.ring, idx, kTestRingName, 0x82000000u + i, i, legacy);
  uint32_t changed = 0;
  for (uint8_t b : box.front) changed += b != kGuardByte;
  for (uint8_t b : box.back) changed += b != kGuardByte;
  bool ok = idx == seed + calls;
  const uint32_t n = calls < kCallRingSize ? calls : kCallRingSize;
  for (uint32_t k = 0; k < n && ok; ++k) {
    const CallRingEntry& e = box.ring[callRingSlot(idx - 1u - k)];
    ok = e.name == kTestRingName && e.lr == 0x82000000u + (calls - 1u - k) && e.r3 == calls - 1u - k;
  }
  if (endIdx) *endIdx = idx;
  if (entriesOk) *entriesOk = ok;
  return changed;
}
#if SR_AUDIT_IMPORTS
// srTestAuditCallKernel (step4/audit, tools/difftest/import_audit_test.cpp A3 / A4): one REAL callKernel of `name`
// with the given lr and r3, so the test sees the census hook in the glue itself, not only the census unit.
uint32_t srTestAuditCallKernel(const char* name, uint32_t lr, uint32_t r3) {
  PPCContext ctx{};
  ctx.lr = lr;
  ctx.r3.u32 = r3;
  return callKernel(name, ctx, nullptr);
}
#endif
// srTestCallKernelRing: the WIRING - this thread's real counter seeded to `seed`, then `calls` real callKernel
// calls (an import no export has, so nothing runs); true when the real t_callRing holds them newest-first at the
// slots the counter names (read in bounds only). A callKernel that stopped recording fails it.
bool srTestCallKernelRing(uint32_t seed, uint32_t calls, uint32_t* endIdx) {
  const uint32_t saved = t_callRingIdx;
  t_callRingIdx = seed;
  PPCContext ctx{};
  for (uint32_t i = 0; i < calls; ++i) {
    ctx.lr = 0x82100000u + i;
    ctx.r3.u32 = i;
    (void)callKernel(kTestImportName, ctx, nullptr);
  }
  const uint32_t idx = t_callRingIdx;
  bool ok = idx == seed + calls;
  const uint32_t n = calls < kCallRingSize ? calls : kCallRingSize;
  for (uint32_t k = 0; k < n && ok; ++k) {
    const CallRingEntry& e = t_callRing[callRingSlot(idx - 1u - k)];
    ok = e.name == kTestImportName && e.lr == 0x82100000u + (calls - 1u - k) && e.r3 == calls - 1u - k;
  }
  t_callRingIdx = saved;
  if (endIdx) *endIdx = idx;
  return ok;
}
// srTestCallKernelMainRing: the same for the MAIN thread's static g_mainCallRing (this thread marked as the main
// guest thread for the duration, its counter seeded to `seed`); true when g_mainCallRing holds the calls newest-first.
bool srTestCallKernelMainRing(uint32_t seed, uint32_t calls, uint32_t* endIdx) {
  const bool wasMain = sr::kernel::isMainGuestThread();
  const uint32_t saved = g_mainCallRingIdx;
  sr::kernel::setIsMainGuestThread(true);
  g_mainCallRingIdx = seed;
  PPCContext ctx{};
  for (uint32_t i = 0; i < calls; ++i) {
    ctx.lr = 0x82200000u + i;
    ctx.r3.u32 = 0x100u + i;
    (void)callKernel(kTestImportName, ctx, nullptr);
  }
  const uint32_t idx = g_mainCallRingIdx;
  bool ok = idx == seed + calls;
  const uint32_t n = calls < kCallRingSize ? calls : kCallRingSize;
  for (uint32_t k = 0; k < n && ok; ++k) {
    const CallRingEntry& e = g_mainCallRing[callRingSlot(idx - 1u - k)];
    ok = e.name == kTestImportName && e.lr == 0x82200000u + (calls - 1u - k) && e.r3 == 0x100u + (calls - 1u - k);
  }
  g_mainCallRingIdx = saved;
  sr::kernel::setIsMainGuestThread(wasMain);
  if (endIdx) *endIdx = idx;
  return ok;
}
uint32_t srTestCallRingSlot(uint32_t n) { return callRingSlot(n); }
int srTestCallRingSlotLegacy(uint32_t n) { return callRingSlotLegacy(n); }

// Dump main thread call ring for crash diagnostics — called from guest_harness.cpp
void dumpMainThreadCallRing() {
  fprintf(stderr, "[CALL_RING] inKernelCall=%d lastCall=%s ringIdx=%u\n",
          g_inKernelCall ? 1 : 0,
          g_lastKernelCallName ? g_lastKernelCallName : "(none)",
          g_mainCallRingIdx);
  fprintf(stderr, "[CALL_RING] Recent main-thread kernel calls (newest first):\n");
  for (uint32_t i = 0; i < kCallRingSize; ++i) {
    const uint32_t idx = callRingSlot(g_mainCallRingIdx - 1u - i);
    if (g_mainCallRing[idx].name)
      fprintf(stderr, "  [%u] %s lr=0x%08X r3=0x%08X\n",
              i, g_mainCallRing[idx].name, g_mainCallRing[idx].lr, g_mainCallRing[idx].r3);
  }
  fflush(stderr);
}

PPC_FUNC(__imp__DbgBreakPoint) {
  ctx.r3.u32 = callKernel("DbgBreakPoint", ctx, base);
}

PPC_FUNC(__imp__DbgPrint) {
  ctx.r3.u32 = callKernel("DbgPrint", ctx, base);
}

PPC_FUNC(__imp__ExCreateThread) {
  ctx.r3.u32 = callKernel("ExCreateThread", ctx, base);
}

PPC_FUNC(__imp__ExGetXConfigSetting) {
  ctx.r3.u32 = callKernel("ExGetXConfigSetting", ctx, base);
}

PPC_FUNC(__imp__ExRegisterTitleTerminateNotification) {
  ctx.r3.u32 = callKernel("ExRegisterTitleTerminateNotification", ctx, base);
}

PPC_FUNC(__imp__ExTerminateThread) {
  ctx.r3.u32 = callKernel("ExTerminateThread", ctx, base);
}

PPC_FUNC(__imp__HalReturnToFirmware) {
  ctx.r3.u32 = callKernel("HalReturnToFirmware", ctx, base);
}

// tools/graceful-quit (kernel/quit.hpp): the seven lock thunks keep this thread's guest-lock count - an acquire counts
// BEFORE its import, a release uncounts BEFORE its import - so the import gate never parks a lock holder.
PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql) {
  sr::kernel::quit::lockAcquiring();
  ctx.r3.u32 = callKernel("KeAcquireSpinLockAtRaisedIrql", ctx, base);
}

PPC_FUNC(__imp__KeBugCheck) {
  ctx.r3.u32 = callKernel("KeBugCheck", ctx, base);
}

PPC_FUNC(__imp__KeBugCheckEx) {
  // F4 (FATAL_OBJECT_TERMINATION): the caller already sets ctx.lr to the
  // safe continuation label BEFORE calling __imp__KeBugCheckEx, so C++
  // execution continues at that label after we return.  No stack or lr
  // manipulation needed — the old "long-return" hack was corrupting ctx.lr
  // and ctx.r1, causing a 20K-message cascade when the enclosing function
  // eventually returned to the wrong address.
  ctx.r3.u32 = callKernel("KeBugCheckEx", ctx, base);
}

PPC_FUNC(__imp__KeDelayExecutionThread) {
  ctx.r3.u32 = callKernel("KeDelayExecutionThread", ctx, base);
}

PPC_FUNC(__imp__KeEnableFpuExceptions) {
  ctx.r3.u32 = callKernel("KeEnableFpuExceptions", ctx, base);
}

PPC_FUNC(__imp__KeEnterCriticalRegion) {
  ctx.r3.u32 = callKernel("KeEnterCriticalRegion", ctx, base);
}

PPC_FUNC(__imp__KeGetCurrentProcessType) {
  // Return 0 (UserProcess) via kernel stub.  The game compares our return
  // with byte at 379(r30) in sub_8246D360/D368; that byte is typically 0
  // (heap created as UserProcess).  Returning 1 caused a mismatch that
  // triggered KeBugCheckEx(0xF4) and subsequent heap corruption leading to
  // PC=0x04000000 (garbage).  If a *different* path needs 1, fix that
  // specific condition instead of changing this global return.
  ctx.r3.u32 = callKernel("KeGetCurrentProcessType", ctx, base);
}

PPC_FUNC(__imp__KeInitializeSemaphore) {
  ctx.r3.u32 = callKernel("KeInitializeSemaphore", ctx, base);
}

PPC_FUNC(__imp__KeLeaveCriticalRegion) {
  ctx.r3.u32 = callKernel("KeLeaveCriticalRegion", ctx, base);
}

PPC_FUNC(__imp__KeLockL2) {
  ctx.r3.u32 = callKernel("KeLockL2", ctx, base);
}

PPC_FUNC(__imp__KeQueryPerformanceFrequency) {
  // iter-463: the full 64-bit r3 - both callers consume it as a doubleword
  // (sub_82468F08 `std r3`, XmaHw_WaitIdleOrResetDecoder `rldicl r11,r3,61,3`);
  // a u32 store left r3's stale upper half in place.
  ctx.r3.u64 = callKernel("KeQueryPerformanceFrequency", ctx, base);
}

PPC_FUNC(__imp__KeQuerySystemTime) {
  ctx.r3.u32 = callKernel("KeQuerySystemTime", ctx, base);
}

PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel) {
  ctx.r3.u32 = callKernel("KeRaiseIrqlToDpcLevel", ctx, base);
}

PPC_FUNC(__imp__KeReleaseSemaphore) {
  ctx.r3.u32 = callKernel("KeReleaseSemaphore", ctx, base);
}

PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql) {
  sr::kernel::quit::lockReleasing();  // tools/graceful-quit
  ctx.r3.u32 = callKernel("KeReleaseSpinLockFromRaisedIrql", ctx, base);
}

PPC_FUNC(__imp__KeResetEvent) {
  ctx.r3.u32 = callKernel("KeResetEvent", ctx, base);
}

PPC_FUNC(__imp__KeResumeThread) {
  ctx.r3.u32 = callKernel("KeResumeThread", ctx, base);
}

PPC_FUNC(__imp__KeSetAffinityThread) {
  ctx.r3.u32 = callKernel("KeSetAffinityThread", ctx, base);
}

PPC_FUNC(__imp__KeSetBasePriorityThread) {
  ctx.r3.u32 = callKernel("KeSetBasePriorityThread", ctx, base);
}

PPC_FUNC(__imp__KeSetEvent) {
  ctx.r3.u32 = callKernel("KeSetEvent", ctx, base);
}

PPC_FUNC(__imp__KeTlsAlloc) {
  ctx.r3.u32 = callKernel("KeTlsAlloc", ctx, base);
}

PPC_FUNC(__imp__KeTlsFree) {
  ctx.r3.u32 = callKernel("KeTlsFree", ctx, base);
}

PPC_FUNC(__imp__KeTlsGetValue) {
  ctx.r3.u32 = callKernel("KeTlsGetValue", ctx, base);
}

PPC_FUNC(__imp__KeTlsSetValue) {
  ctx.r3.u32 = callKernel("KeTlsSetValue", ctx, base);
}

PPC_FUNC(__imp__KeUnlockL2) {
  ctx.r3.u32 = callKernel("KeUnlockL2", ctx, base);
}

PPC_FUNC(__imp__KeWaitForMultipleObjects) {
  ctx.r3.u32 = callKernel("KeWaitForMultipleObjects", ctx, base);
}

PPC_FUNC(__imp__KeWaitForSingleObject) {
  ctx.r3.u32 = callKernel("KeWaitForSingleObject", ctx, base);
}

PPC_FUNC(__imp__KfAcquireSpinLock) {
  sr::kernel::quit::lockAcquiring();  // tools/graceful-quit
  ctx.r3.u32 = callKernel("KfAcquireSpinLock", ctx, base);
}

PPC_FUNC(__imp__KfLowerIrql) {
  ctx.r3.u32 = callKernel("KfLowerIrql", ctx, base);
}

PPC_FUNC(__imp__KfReleaseSpinLock) {
  sr::kernel::quit::lockReleasing();  // tools/graceful-quit
  ctx.r3.u32 = callKernel("KfReleaseSpinLock", ctx, base);
}

PPC_FUNC(__imp__KiApcNormalRoutineNop) {
  ctx.r3.u32 = callKernel("KiApcNormalRoutineNop", ctx, base);
}

PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx) {
  // Log caller (lr) for DOGMA/pool debugging: which code path requested physical alloc
  static int physAllocCount = 0;
  if (++physAllocCount <= 5) SR_LOG_KERNEL_IMPORT("[PHYS_ALLOC] caller lr=0x%08X", ctx.lr);
  ctx.r3.u32 = callKernel("MmAllocatePhysicalMemoryEx", ctx, base);
}

PPC_FUNC(__imp__MmFreePhysicalMemory) {
  ctx.r3.u32 = callKernel("MmFreePhysicalMemory", ctx, base);
}

PPC_FUNC(__imp__MmGetPhysicalAddress) {
  ctx.r3.u32 = callKernel("MmGetPhysicalAddress", ctx, base);
}

PPC_FUNC(__imp__MmQueryAddressProtect) {
  ctx.r3.u32 = callKernel("MmQueryAddressProtect", ctx, base);
}

PPC_FUNC(__imp__MmQueryAllocationSize) {
  ctx.r3.u32 = callKernel("MmQueryAllocationSize", ctx, base);
}

PPC_FUNC(__imp__MmSetAddressProtect) {
  ctx.r3.u32 = callKernel("MmSetAddressProtect", ctx, base);
}

PPC_FUNC(__imp__NetDll_WSACleanup) {
  ctx.r3.u32 = callKernel("NetDll_WSACleanup", ctx, base);
}

PPC_FUNC(__imp__NetDll_WSACloseEvent) {
  ctx.r3.u32 = callKernel("NetDll_WSACloseEvent", ctx, base);
}

PPC_FUNC(__imp__NetDll_WSACreateEvent) {
  ctx.r3.u32 = callKernel("NetDll_WSACreateEvent", ctx, base);
}

PPC_FUNC(__imp__NetDll_WSAGetLastError) {
  ctx.r3.u32 = callKernel("NetDll_WSAGetLastError", ctx, base);
}

PPC_FUNC(__imp__NetDll_WSAGetOverlappedResult) {
  ctx.r3.u32 = callKernel("NetDll_WSAGetOverlappedResult", ctx, base);
}

PPC_FUNC(__imp__NetDll_WSARecv) {
  ctx.r3.u32 = callKernel("NetDll_WSARecv", ctx, base);
}

PPC_FUNC(__imp__NetDll_WSARecvFrom) {
  ctx.r3.u32 = callKernel("NetDll_WSARecvFrom", ctx, base);
}

PPC_FUNC(__imp__NetDll_WSAResetEvent) {
  ctx.r3.u32 = callKernel("NetDll_WSAResetEvent", ctx, base);
}

PPC_FUNC(__imp__NetDll_WSASetEvent) {
  ctx.r3.u32 = callKernel("NetDll_WSASetEvent", ctx, base);
}

PPC_FUNC(__imp__NetDll_WSAStartup) {
  ctx.r3.u32 = callKernel("NetDll_WSAStartup", ctx, base);
}

PPC_FUNC(__imp__NetDll_WSAWaitForMultipleEvents) {
  ctx.r3.u32 = callKernel("NetDll_WSAWaitForMultipleEvents", ctx, base);
}

PPC_FUNC(__imp__NetDll_XNetCleanup) {
  ctx.r3.u32 = callKernel("NetDll_XNetCleanup", ctx, base);
}

PPC_FUNC(__imp__NetDll_XNetConnect) {
  ctx.r3.u32 = callKernel("NetDll_XNetConnect", ctx, base);
}

PPC_FUNC(__imp__NetDll_XNetCreateKey) {
  ctx.r3.u32 = callKernel("NetDll_XNetCreateKey", ctx, base);
}

PPC_FUNC(__imp__NetDll_XNetDnsLookup) {
  ctx.r3.u32 = callKernel("NetDll_XNetDnsLookup", ctx, base);
}

PPC_FUNC(__imp__NetDll_XNetDnsRelease) {
  ctx.r3.u32 = callKernel("NetDll_XNetDnsRelease", ctx, base);
}

PPC_FUNC(__imp__NetDll_XNetGetConnectStatus) {
  ctx.r3.u32 = callKernel("NetDll_XNetGetConnectStatus", ctx, base);
}

PPC_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus) {
  ctx.r3.u32 = callKernel("NetDll_XNetGetEthernetLinkStatus", ctx, base);
}

PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr) {
  ctx.r3.u32 = callKernel("NetDll_XNetGetTitleXnAddr", ctx, base);
}

PPC_FUNC(__imp__NetDll_XNetQosRelease) {
  ctx.r3.u32 = callKernel("NetDll_XNetQosRelease", ctx, base);
}

PPC_FUNC(__imp__NetDll_XNetQosServiceLookup) {
  ctx.r3.u32 = callKernel("NetDll_XNetQosServiceLookup", ctx, base);
}

PPC_FUNC(__imp__NetDll_XNetRegisterKey) {
  ctx.r3.u32 = callKernel("NetDll_XNetRegisterKey", ctx, base);
}

PPC_FUNC(__imp__NetDll_XNetStartup) {
  ctx.r3.u32 = callKernel("NetDll_XNetStartup", ctx, base);
}

PPC_FUNC(__imp__NetDll_XNetUnregisterKey) {
  ctx.r3.u32 = callKernel("NetDll_XNetUnregisterKey", ctx, base);
}

PPC_FUNC(__imp__NetDll_accept) {
  ctx.r3.u32 = callKernel("NetDll_accept", ctx, base);
}

PPC_FUNC(__imp__NetDll_bind) {
  ctx.r3.u32 = callKernel("NetDll_bind", ctx, base);
}

PPC_FUNC(__imp__NetDll_closesocket) {
  ctx.r3.u32 = callKernel("NetDll_closesocket", ctx, base);
}

PPC_FUNC(__imp__NetDll_connect) {
  ctx.r3.u32 = callKernel("NetDll_connect", ctx, base);
}

PPC_FUNC(__imp__NetDll_getpeername) {
  ctx.r3.u32 = callKernel("NetDll_getpeername", ctx, base);
}

PPC_FUNC(__imp__NetDll_getsockname) {
  ctx.r3.u32 = callKernel("NetDll_getsockname", ctx, base);
}

PPC_FUNC(__imp__NetDll_getsockopt) {
  ctx.r3.u32 = callKernel("NetDll_getsockopt", ctx, base);
}

PPC_FUNC(__imp__NetDll_ioctlsocket) {
  ctx.r3.u32 = callKernel("NetDll_ioctlsocket", ctx, base);
}

PPC_FUNC(__imp__NetDll_listen) {
  ctx.r3.u32 = callKernel("NetDll_listen", ctx, base);
}

PPC_FUNC(__imp__NetDll_recv) {
  ctx.r3.u32 = callKernel("NetDll_recv", ctx, base);
}

PPC_FUNC(__imp__NetDll_recvfrom) {
  ctx.r3.u32 = callKernel("NetDll_recvfrom", ctx, base);
}

PPC_FUNC(__imp__NetDll_select) {
  ctx.r3.u32 = callKernel("NetDll_select", ctx, base);
}

PPC_FUNC(__imp__NetDll_send) {
  ctx.r3.u32 = callKernel("NetDll_send", ctx, base);
}

PPC_FUNC(__imp__NetDll_sendto) {
  ctx.r3.u32 = callKernel("NetDll_sendto", ctx, base);
}

PPC_FUNC(__imp__NetDll_setsockopt) {
  ctx.r3.u32 = callKernel("NetDll_setsockopt", ctx, base);
}

PPC_FUNC(__imp__NetDll_shutdown) {
  ctx.r3.u32 = callKernel("NetDll_shutdown", ctx, base);
}

PPC_FUNC(__imp__NetDll_socket) {
  ctx.r3.u32 = callKernel("NetDll_socket", ctx, base);
}

PPC_FUNC(__imp__NtAllocateVirtualMemory) {
  ctx.r3.u32 = callKernel("NtAllocateVirtualMemory", ctx, base);
}

PPC_FUNC(__imp__NtClearEvent) {
  ctx.r3.u32 = callKernel("NtClearEvent", ctx, base);
}

PPC_FUNC(__imp__NtClose) {
  ctx.r3.u32 = callKernel("NtClose", ctx, base);
}

PPC_FUNC(__imp__NtCreateEvent) {
  ctx.r3.u32 = callKernel("NtCreateEvent", ctx, base);
}

PPC_FUNC(__imp__NtCreateFile) {
  ctx.r3.u32 = callKernel("NtCreateFile", ctx, base);
}

PPC_FUNC(__imp__NtCreateSemaphore) {
  ctx.r3.u32 = callKernel("NtCreateSemaphore", ctx, base);
}

PPC_FUNC(__imp__NtDuplicateObject) {
  ctx.r3.u32 = callKernel("NtDuplicateObject", ctx, base);
}

PPC_FUNC(__imp__NtFlushBuffersFile) {
  ctx.r3.u32 = callKernel("NtFlushBuffersFile", ctx, base);
}

PPC_FUNC(__imp__NtFreeVirtualMemory) {
  ctx.r3.u32 = callKernel("NtFreeVirtualMemory", ctx, base);
}

PPC_FUNC(__imp__NtOpenFile) {
  ctx.r3.u32 = callKernel("NtOpenFile", ctx, base);
}

PPC_FUNC(__imp__NtQueryDirectoryFile) {
  ctx.r3.u32 = callKernel("NtQueryDirectoryFile", ctx, base);
}

PPC_FUNC(__imp__NtQueryFullAttributesFile) {
  ctx.r3.u32 = callKernel("NtQueryFullAttributesFile", ctx, base);
}

PPC_FUNC(__imp__NtQueryInformationFile) {
  ctx.r3.u32 = callKernel("NtQueryInformationFile", ctx, base);
}

PPC_FUNC(__imp__NtQueryVirtualMemory) {
  ctx.r3.u32 = callKernel("NtQueryVirtualMemory", ctx, base);
}

PPC_FUNC(__imp__NtQueryVolumeInformationFile) {
  ctx.r3.u32 = callKernel("NtQueryVolumeInformationFile", ctx, base);
}

PPC_FUNC(__imp__NtQueueApcThread) {
  // Xbox 360: NtQueueApcThread(ThreadHandle, ApcRoutine, Arg1, Arg2, Arg3)
  // Capture guest APC params before kernel call (which may clobber state).
  uint32_t targetHandle = ctx.r3.u32;
  uint32_t apcRoutine   = ctx.r4.u32;
  uint32_t arg1         = ctx.r5.u32;
  uint32_t arg2         = ctx.r6.u32;
  uint32_t arg3         = ctx.r7.u32;

  // Enqueue BEFORE kernel call so the APC is ready when the target wakes.
  enqueueGuestAPC(targetHandle, apcRoutine, arg1, arg2, arg3);

  // Host stub: QueueUserAPC(noop) to interrupt the target's alertable wait.
  ctx.r3.u32 = callKernel("NtQueueApcThread", ctx, base);
}

PPC_FUNC(__imp__NtReadFile) {
  ctx.r3.u32 = callKernel("NtReadFile", ctx, base);
}

PPC_FUNC(__imp__NtReleaseSemaphore) {
  ctx.r3.u32 = callKernel("NtReleaseSemaphore", ctx, base);
}

PPC_FUNC(__imp__NtResumeThread) {
  ctx.r3.u32 = callKernel("NtResumeThread", ctx, base);
}

PPC_FUNC(__imp__NtSetInformationFile) {
  ctx.r3.u32 = callKernel("NtSetInformationFile", ctx, base);
}

PPC_FUNC(__imp__NtSuspendThread) {
  ctx.r3.u32 = callKernel("NtSuspendThread", ctx, base);
}

PPC_FUNC(__imp__NtWaitForSingleObjectEx) {
  ctx.r3.u32 = callKernel("NtWaitForSingleObjectEx", ctx, base);
}

PPC_FUNC(__imp__NtWriteFile) {
  ctx.r3.u32 = callKernel("NtWriteFile", ctx, base);
}

PPC_FUNC(__imp__NtYieldExecution) {
  ctx.r3.u32 = callKernel("NtYieldExecution", ctx, base);
}

PPC_FUNC(__imp__ObCreateSymbolicLink) {
  ctx.r3.u32 = callKernel("ObCreateSymbolicLink", ctx, base);
}

PPC_FUNC(__imp__ObDeleteSymbolicLink) {
  ctx.r3.u32 = callKernel("ObDeleteSymbolicLink", ctx, base);
}

PPC_FUNC(__imp__ObDereferenceObject) {
  ctx.r3.u32 = callKernel("ObDereferenceObject", ctx, base);
}

PPC_FUNC(__imp__ObReferenceObjectByHandle) {
  ctx.r3.u32 = callKernel("ObReferenceObjectByHandle", ctx, base);
}

PPC_FUNC(__imp__RtlCompareMemoryUlong) {
  ctx.r3.u32 = callKernel("RtlCompareMemoryUlong", ctx, base);
}

PPC_FUNC(__imp__RtlEnterCriticalSection) {
  if (ctx.r3.u32 == 0) ctx.r3.u32 = kNullCritSectSubstituteVA;
  sr::kernel::quit::lockAcquiring();  // tools/graceful-quit
  ctx.r3.u32 = callKernel("RtlEnterCriticalSection", ctx, base);
}

PPC_FUNC(__imp__RtlFillMemoryUlong) {
  ctx.r3.u32 = callKernel("RtlFillMemoryUlong", ctx, base);
}

PPC_FUNC(__imp__RtlImageXexHeaderField) {
  ctx.r3.u32 = callKernel("RtlImageXexHeaderField", ctx, base);
}

PPC_FUNC(__imp__RtlInitAnsiString) {
  ctx.r3.u32 = callKernel("RtlInitAnsiString", ctx, base);
}

PPC_FUNC(__imp__RtlInitializeCriticalSection) {
  ctx.r3.u32 = callKernel("RtlInitializeCriticalSection", ctx, base);
}

PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount) {
  ctx.r3.u32 = callKernel("RtlInitializeCriticalSectionAndSpinCount", ctx, base);
}

PPC_FUNC(__imp__RtlLeaveCriticalSection) {
  if (ctx.r3.u32 == 0) ctx.r3.u32 = kNullCritSectSubstituteVA;
  sr::kernel::quit::lockReleasing();  // tools/graceful-quit
  ctx.r3.u32 = callKernel("RtlLeaveCriticalSection", ctx, base);
}

PPC_FUNC(__imp__RtlMultiByteToUnicodeN) {
  ctx.r3.u32 = callKernel("RtlMultiByteToUnicodeN", ctx, base);
}

PPC_FUNC(__imp__RtlNtStatusToDosError) {
  ctx.r3.u32 = callKernel("RtlNtStatusToDosError", ctx, base);
}

PPC_FUNC(__imp__RtlRaiseException) {
  ctx.r3.u32 = callKernel("RtlRaiseException", ctx, base);
}

PPC_FUNC(__imp__RtlTimeFieldsToTime) {
  ctx.r3.u32 = callKernel("RtlTimeFieldsToTime", ctx, base);
}

PPC_FUNC(__imp__RtlTimeToTimeFields) {
  ctx.r3.u32 = callKernel("RtlTimeToTimeFields", ctx, base);
}

PPC_FUNC(__imp__RtlTryEnterCriticalSection) {
  if (ctx.r3.u32 == 0) ctx.r3.u32 = kNullCritSectSubstituteVA;
  sr::kernel::quit::lockAcquiring();  // tools/graceful-quit: counted as held unless the try failed
  ctx.r3.u32 = callKernel("RtlTryEnterCriticalSection", ctx, base);
  if (ctx.r3.u32 == 0) sr::kernel::quit::lockReleasing();
}

PPC_FUNC(__imp__RtlUnicodeToMultiByteN) {
  ctx.r3.u32 = callKernel("RtlUnicodeToMultiByteN", ctx, base);
}

PPC_FUNC(__imp__RtlUnwind) {
  sr::kernel::logRtlUnwindCall(static_cast<uint32_t>(ctx.lr), ctx.r3.u32, ctx.r4.u32);
  ctx.r3.u32 = callKernel("RtlUnwind", ctx, base);
}

PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines) {
  ctx.r3.u32 = callKernel("VdCallGraphicsNotificationRoutines", ctx, base);
}

PPC_FUNC(__imp__VdEnableDisableClockGating) {
  ctx.r3.u32 = callKernel("VdEnableDisableClockGating", ctx, base);
}

PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack) {
  ctx.r3.u32 = callKernel("VdEnableRingBufferRPtrWriteBack", ctx, base);
}

PPC_FUNC(__imp__VdGetCurrentDisplayGamma) {
  ctx.r3.u32 = callKernel("VdGetCurrentDisplayGamma", ctx, base);
}

PPC_FUNC(__imp__VdGetCurrentDisplayInformation) {
  ctx.r3.u32 = callKernel("VdGetCurrentDisplayInformation", ctx, base);
}

PPC_FUNC(__imp__VdGetSystemCommandBuffer) {
  ctx.r3.u32 = callKernel("VdGetSystemCommandBuffer", ctx, base);
}

PPC_FUNC(__imp__VdInitializeEngines) {
  ctx.r3.u32 = callKernel("VdInitializeEngines", ctx, base);
}

PPC_FUNC(__imp__VdInitializeRingBuffer) {
  ctx.r3.u32 = callKernel("VdInitializeRingBuffer", ctx, base);
}

PPC_FUNC(__imp__VdInitializeScalerCommandBuffer) {
  ctx.r3.u32 = callKernel("VdInitializeScalerCommandBuffer", ctx, base);
}

PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded) {
  ctx.r3.u32 = callKernel("VdIsHSIOTrainingSucceeded", ctx, base);
}

PPC_FUNC(__imp__VdPersistDisplay) {
  ctx.r3.u32 = callKernel("VdPersistDisplay", ctx, base);
}

PPC_FUNC(__imp__VdQueryVideoFlags) {
  ctx.r3.u32 = callKernel("VdQueryVideoFlags", ctx, base);
}

PPC_FUNC(__imp__VdQueryVideoMode) {
  ctx.r3.u32 = callKernel("VdQueryVideoMode", ctx, base);
}

PPC_FUNC(__imp__VdRetrainEDRAM) {
  ctx.r3.u32 = callKernel("VdRetrainEDRAM", ctx, base);
}

PPC_FUNC(__imp__VdRetrainEDRAMWorker) {
  ctx.r3.u32 = callKernel("VdRetrainEDRAMWorker", ctx, base);
}

PPC_FUNC(__imp__VdSetDisplayMode) {
  ctx.r3.u32 = callKernel("VdSetDisplayMode", ctx, base);
}

PPC_FUNC(__imp__VdSetGraphicsInterruptCallback) {
  ctx.r3.u32 = callKernel("VdSetGraphicsInterruptCallback", ctx, base);
}

PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) {
  ctx.r3.u32 = callKernel("VdSetSystemCommandBufferGpuIdentifierAddress", ctx, base);
}

PPC_FUNC(__imp__VdShutdownEngines) {
  ctx.r3.u32 = callKernel("VdShutdownEngines", ctx, base);
}

PPC_FUNC(__imp__VdSwap) {
  ctx.r3.u32 = callKernel("VdSwap", ctx, base);
}

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume) {
  ctx.r3.u32 = callKernel("XAudioGetVoiceCategoryVolume", ctx, base);
}

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask) {
  ctx.r3.u32 = callKernel("XAudioGetVoiceCategoryVolumeChangeMask", ctx, base);
}

PPC_FUNC(__imp__XAudioRegisterRenderDriverClient) {
  ctx.r3.u32 = callKernel("XAudioRegisterRenderDriverClient", ctx, base);
}

PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame) {
  ctx.r3.u32 = callKernel("XAudioSubmitRenderDriverFrame", ctx, base);
}

PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient) {
  ctx.r3.u32 = callKernel("XAudioUnregisterRenderDriverClient", ctx, base);
}

PPC_FUNC(__imp__XGetAVPack) {
  ctx.r3.u32 = callKernel("XGetAVPack", ctx, base);
}

PPC_FUNC(__imp__XGetLanguage) {
  ctx.r3.u32 = callKernel("XGetLanguage", ctx, base);
}

PPC_FUNC(__imp__XGetVideoMode) {
  ctx.r3.u32 = callKernel("XGetVideoMode", ctx, base);
}

PPC_FUNC(__imp__XMACreateContext) {
  ctx.r3.u32 = callKernel("XMACreateContext", ctx, base);
}

PPC_FUNC(__imp__XMADisableContext) {
  ctx.r3.u32 = callKernel("XMADisableContext", ctx, base);
}

PPC_FUNC(__imp__XMAEnableContext) {
  ctx.r3.u32 = callKernel("XMAEnableContext", ctx, base);
}

PPC_FUNC(__imp__XMAGetOutputBufferReadOffset) {
  ctx.r3.u32 = callKernel("XMAGetOutputBufferReadOffset", ctx, base);
}

PPC_FUNC(__imp__XMAGetOutputBufferWriteOffset) {
  ctx.r3.u32 = callKernel("XMAGetOutputBufferWriteOffset", ctx, base);
}

PPC_FUNC(__imp__XMAInitializeContext) {
  ctx.r3.u32 = callKernel("XMAInitializeContext", ctx, base);
}

PPC_FUNC(__imp__XMAIsInputBuffer0Valid) {
  ctx.r3.u32 = callKernel("XMAIsInputBuffer0Valid", ctx, base);
}

PPC_FUNC(__imp__XMAIsInputBuffer1Valid) {
  ctx.r3.u32 = callKernel("XMAIsInputBuffer1Valid", ctx, base);
}

PPC_FUNC(__imp__XMAIsOutputBufferValid) {
  ctx.r3.u32 = callKernel("XMAIsOutputBufferValid", ctx, base);
}

PPC_FUNC(__imp__XMAReleaseContext) {
  // iter-463: with SR_XMA_NEW=1 the release goes through the registry like every other import, so
  // the native XMA device (kernel/apu/xma_device.cpp) returns the context to its pool. With the
  // gate off (default) the old bypass stays byte for byte: r3 = 0 without a kernel call (it
  // dates from a pre-iter-300 "~80K callKernel" crash; the legacy registration is a generic stub
  // that returns the same 0).
  if (sr::kernel::apu::xmaNewEnabled()) {
    ctx.r3.u32 = callKernel("XMAReleaseContext", ctx, base);
    return;
  }
  ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__XMASetInputBuffer0) {
  ctx.r3.u32 = callKernel("XMASetInputBuffer0", ctx, base);
}

PPC_FUNC(__imp__XMASetInputBuffer0Valid) {
  ctx.r3.u32 = callKernel("XMASetInputBuffer0Valid", ctx, base);
}

PPC_FUNC(__imp__XMASetInputBuffer1) {
  ctx.r3.u32 = callKernel("XMASetInputBuffer1", ctx, base);
}

PPC_FUNC(__imp__XMASetInputBuffer1Valid) {
  ctx.r3.u32 = callKernel("XMASetInputBuffer1Valid", ctx, base);
}

PPC_FUNC(__imp__XMASetOutputBufferReadOffset) {
  ctx.r3.u32 = callKernel("XMASetOutputBufferReadOffset", ctx, base);
}

PPC_FUNC(__imp__XMASetOutputBufferValid) {
  ctx.r3.u32 = callKernel("XMASetOutputBufferValid", ctx, base);
}

PPC_FUNC(__imp__XMsgCancelIORequest) {
  ctx.r3.u32 = callKernel("XMsgCancelIORequest", ctx, base);
}

PPC_FUNC(__imp__XMsgInProcessCall) {
  ctx.r3.u32 = callKernel("XMsgInProcessCall", ctx, base);
}

PPC_FUNC(__imp__XMsgStartIORequest) {
  ctx.r3.u32 = callKernel("XMsgStartIORequest", ctx, base);
}

PPC_FUNC(__imp__XMsgStartIORequestEx) {
  ctx.r3.u32 = callKernel("XMsgStartIORequestEx", ctx, base);
}

PPC_FUNC(__imp__XNetLogonGetMachineID) {
  ctx.r3.u32 = callKernel("XNetLogonGetMachineID", ctx, base);
}

PPC_FUNC(__imp__XNetLogonGetTitleID) {
  ctx.r3.u32 = callKernel("XNetLogonGetTitleID", ctx, base);
}

PPC_FUNC(__imp__XNotifyGetNext) {
  ctx.r3.u32 = callKernel("XNotifyGetNext", ctx, base);
}

PPC_FUNC(__imp__XamAlloc) {
  ctx.r3.u32 = callKernel("XamAlloc", ctx, base);
}

PPC_FUNC(__imp__XamContentClose) {
  ctx.r3.u32 = callKernel("XamContentClose", ctx, base);
}

PPC_FUNC(__imp__XamContentCreateEnumerator) {
  ctx.r3.u32 = callKernel("XamContentCreateEnumerator", ctx, base);
}

PPC_FUNC(__imp__XamContentCreateEx) {
  ctx.r3.u32 = callKernel("XamContentCreateEx", ctx, base);
}

PPC_FUNC(__imp__XamContentDelete) {
  ctx.r3.u32 = callKernel("XamContentDelete", ctx, base);
}

PPC_FUNC(__imp__XamContentGetCreator) {
  ctx.r3.u32 = callKernel("XamContentGetCreator", ctx, base);
}

PPC_FUNC(__imp__XamContentGetDeviceData) {
  ctx.r3.u32 = callKernel("XamContentGetDeviceData", ctx, base);
}

PPC_FUNC(__imp__XamContentGetDeviceName) {
  ctx.r3.u32 = callKernel("XamContentGetDeviceName", ctx, base);
}

PPC_FUNC(__imp__XamContentGetDeviceState) {
  ctx.r3.u32 = callKernel("XamContentGetDeviceState", ctx, base);
}

PPC_FUNC(__imp__XamContentSetThumbnail) {
  ctx.r3.u32 = callKernel("XamContentSetThumbnail", ctx, base);
}

PPC_FUNC(__imp__XamEnumerate) {
  ctx.r3.u32 = callKernel("XamEnumerate", ctx, base);
}

PPC_FUNC(__imp__XamFree) {
  ctx.r3.u32 = callKernel("XamFree", ctx, base);
}

PPC_FUNC(__imp__XamGetExecutionId) {
  ctx.r3.u32 = callKernel("XamGetExecutionId", ctx, base);
}

PPC_FUNC(__imp__XamGetSystemVersion) {
  ctx.r3.u32 = callKernel("XamGetSystemVersion", ctx, base);
}

PPC_FUNC(__imp__XamInputGetCapabilities) {
  ctx.r3.u32 = callKernel("XamInputGetCapabilities", ctx, base);
}

PPC_FUNC(__imp__XamInputGetState) {
  ctx.r3.u32 = callKernel("XamInputGetState", ctx, base);
}

PPC_FUNC(__imp__XamInputSetState) {
  ctx.r3.u32 = callKernel("XamInputSetState", ctx, base);
}

PPC_FUNC(__imp__XamLoaderLaunchTitle) {
  ctx.r3.u32 = callKernel("XamLoaderLaunchTitle", ctx, base);
}

PPC_FUNC(__imp__XamLoaderTerminateTitle) {
  ctx.r3.u32 = callKernel("XamLoaderTerminateTitle", ctx, base);
}

PPC_FUNC(__imp__XamNotifyCreateListener) {
  ctx.r3.u32 = callKernel("XamNotifyCreateListener", ctx, base);
}

PPC_FUNC(__imp__XamShowDeviceSelectorUI) {
  ctx.r3.u32 = callKernel("XamShowDeviceSelectorUI", ctx, base);
}

PPC_FUNC(__imp__XamShowDirtyDiscErrorUI) {
  ctx.r3.u32 = callKernel("XamShowDirtyDiscErrorUI", ctx, base);
}

PPC_FUNC(__imp__XamShowMessageBoxUIEx) {
  ctx.r3.u32 = callKernel("XamShowMessageBoxUIEx", ctx, base);
}

PPC_FUNC(__imp__XamShowSigninUI) {
  ctx.r3.u32 = callKernel("XamShowSigninUI", ctx, base);
}

PPC_FUNC(__imp__XamUserGetName) {
  ctx.r3.u32 = callKernel("XamUserGetName", ctx, base);
}

PPC_FUNC(__imp__XamUserGetSigninState) {
  ctx.r3.u32 = callKernel("XamUserGetSigninState", ctx, base);
}

PPC_FUNC(__imp__XamUserGetXUID) {
  ctx.r3.u32 = callKernel("XamUserGetXUID", ctx, base);
}

PPC_FUNC(__imp__XamUserReadProfileSettings) {
  ctx.r3.u32 = callKernel("XamUserReadProfileSettings", ctx, base);
}

PPC_FUNC(__imp__XexCheckExecutablePrivilege) {
  ctx.r3.u32 = callKernel("XexCheckExecutablePrivilege", ctx, base);
}

PPC_FUNC(__imp__XexGetModuleHandle) {
  ctx.r3.u32 = callKernel("XexGetModuleHandle", ctx, base);
}

PPC_FUNC(__imp__XexGetProcedureAddress) {
  ctx.r3.u32 = callKernel("XexGetProcedureAddress", ctx, base);
}

PPC_FUNC(__imp____C_specific_handler) {
  ctx.r3.u32 = callKernel("__C_specific_handler", ctx, base);
}

PPC_FUNC(__imp___vsnprintf) {
  ctx.r3.u32 = callKernel("_vsnprintf", ctx, base);
}

PPC_FUNC(__imp__sprintf) {
  ctx.r3.u32 = callKernel("sprintf", ctx, base);
}

// NOTE: Missing recomp stubs moved to ppc/ppc_missing_recomp_stubs.cpp

// ========== CRT heap allocator (sub_8246CA80) ==========
// PRE-CHARTER SUBSTITUTION, dissected iter-395: this hand-glue replaced the
// game's ENTIRE heap allocator with our bump allocator — which is why the
// "CRT heap" appeared to hand out bump-cursor addresses and "grow without
// NtAVM" (there was no heap at all). It also carried a double-increment bug:
// `if (ptr) ptr += 16` ran BOTH after the primary alloc AND after the
// fallback check, returning base+32 of a size+16 grant — every allocation's
// tail 16 bytes lived INSIDE THE NEXT GRANT, whose zero-fill then clobbered
// it. That was the D3DX effect-region kill (FXCLOB3: the region's address
// exactly equals the next grant's base) = the whole 120-FXERR effects wall,
// and the "5th two-allocator overlap" was really ONE allocator plus this
// out-of-bounds pointer.
// iter-395 (SR_REAL_CRT_HEAP, default ON; =0 reverts to the fixed legacy
// substitution): delegate to the GAME'S OWN allocator — possible since
// iter-392 gave the CRT its real 1MB process heap (dword_829AC16C at
// 0x839F0000, built via the guest's own NtAVM reserve/commit).
// r3 = heap handle, r4 = flags, r5 = size; returns r3 = pointer.
extern "C" void __imp__sub_8246CA80(PPCContext& __restrict ctx, uint8_t* base);
static uint32_t legacyBumpAlloc(uint32_t inR3, uint32_t inR5);
PPC_FUNC(sub_8246CA80) {
  // ✅ DEFAULT-ON (iter-397o, was default-off since iter-395b): the "deep
  // build 0/4" that kept this off was the iter-396 weld illusion — the
  // historical deep menu was the FMV-FAILURE bail path; the corrected arms
  // never reached it because they play the REAL console boot. And the movie
  // needed the real heap for its implicit ALIGNMENT contract: large dlmalloc
  // allocs route through NtAVM (64KB-aligned); the YUV texture backing must
  // be 4KB-aligned because the GPU descriptor stores base>>12 and the locked
  // pointer is the ALIGNED-DOWN base — on the 16-byte-aligned bump the frame
  // copy wrote BELOW the backing and wiped the decoded-frame ring's item
  // array ([HWWATCH] guestPC=824813E4 = the frame-get's mode-1 Y-plane
  // memcpy; iter-397n). On the real heap the intro chain plays end-to-end
  // (K=3: legal pt1 8.5s -> natural EOS -> legal pt2 -> intro reel, all
  // 30fps rc=0). =0 reverts to the legacy bump substitution for A/B.
  static const bool s_real = [] {
    const char* e = std::getenv("SR_REAL_CRT_HEAP");
    return !e || e[0] != '0';
  }();
  const uint32_t inR3 = ctx.r3.u32, inR5 = ctx.r5.u32;
  if (s_real) {
    __imp__sub_8246CA80(ctx, base);
    if (ctx.r3.u32) {
      // iter-395b DIAGNOSTIC (SR_REAL_HEAP_ZERO): the legacy substitution
      // zero-filled every malloc; the real heap returns dirty blocks
      // (console-correct - console malloc does NOT zero, and Xenia runs the
      // guest's own dlmalloc). Real-heap arm: effects 33/33 but deep build
      // 0/4 deterministic - discriminate whether the deep build depends on
      // the accidental zeroing.
      // iter-463 charter (burndown item 4): it shipped DEFAULT-ON - a
      // registered band-aid zeroing every guest malloc. Now master-gated with
      // a NAMED gate so it can be rolled alone:
      //   SR_ALLOW_FORCING=1 SR_FORCING_ISOLATE=SR_REAL_HEAP_ZERO  (zeroing on)
      // The default exe returns the dirty block the guest's allocator gave.
      static const bool s_zero = [] {
        const char* e = std::getenv("SR_REAL_HEAP_ZERO");
        return !e || e[0] != '0';
      }();
      if (s_zero && sr::kernel::forcingAllowedFor("SR_REAL_HEAP_ZERO") && inR5 > 0 && inR5 <= 0x1000000u) {
        if (uint8_t* h = sr::kernel::guestVAToHost(ctx.r3.u32))
          std::memset(h, 0, inR5);
      }
      return;
    }
    // Real heap returned NULL (exhausted / not yet created). iter-463 charter
    // (burndown U1): the default exe hands the guest its OWN NULL (console
    // malloc fails the same way; "[CRT_HEAP] #" is in 0/150 run logs, so this
    // never happened in a recorded run). The legacy bump substitution below
    // is a fabricated block and runs only under SR_ALLOW_FORCING.
    static std::atomic<uint32_t> s_nullFalls{0};
    const uint32_t nf = s_nullFalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool bumpFallback = sr::kernel::forcingAllowed();
    if (nf <= 16 || (nf & (nf - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[CRT_HEAP] #%u real sub_8246CA80 returned NULL (h=%08X sz=0x%X lr=%08X) — %s",
        nf, inR3, inR5, (uint32_t)ctx.lr,
        bumpFallback ? "bump fallback (SR_ALLOW_FORCING)" : "NULL returned to the guest");
    if (sr::kernel::forcingAllowed()) ctx.r3.u32 = legacyBumpAlloc(inR3, inR5);
    return;
  }
  // SR_REAL_CRT_HEAP=0 (opt-in A/B): the legacy bump substitution.
  ctx.r3.u32 = legacyBumpAlloc(inR3, inR5);
}

// The legacy bump substitution for sub_8246CA80 (SR_REAL_CRT_HEAP=0, or a real
// NULL under SR_ALLOW_FORCING). Returns the fabricated block's VA.
static uint32_t legacyBumpAlloc(uint32_t inR3, uint32_t inR5) {
  uint32_t size = inR5;
  if (size == 0) size = inR3;
  if (size == 0) size = 256;

  // iter-395c: THE LAYOUT TRAP, recorded honestly. The historical code below
  // applies `+= 16` TWICE on the primary-alloc path (base+32 of a size+16
  // grant): every object's tail 16 bytes live INSIDE THE NEXT GRANT. That
  // overrun is a real bug (it was the D3DX effect-region clobber = the whole
  // effects wall), and fixing it to a single +16 DOES fix effects (33/33) —
  // but the DEEP MENU BUILD is 0/4 deterministic on EVERY corrected layout
  // (real heap, real heap+zerofill, single-offset bump): something in the
  // deep path reads through allocation boundaries and the mutual overlap is
  // load-bearing. Until that consumer is caught (next arc: first-divergence
  // diff historical-vs-fixed), the DEFAULT stays bug-compatible.
  // SR_GLUE_HEAP_FIXED=1 selects the corrected single-offset layout.
  static const bool s_fixedLayout = [] {
    const char* e = std::getenv("SR_GLUE_HEAP_FIXED");
    return e && e[0] == '1';
  }();
  uint32_t ptr = sr::kernel::guestHeapAlloc(size + 16);
  if (ptr && !s_fixedLayout) ptr += 16;          // historical 1st bump
  if (ptr == 0)
    ptr = sr::kernel::guestHeapAllocFallback(size + 16);
  if (ptr) ptr += 16;                            // historical 2nd bump / fixed single offset
  return ptr;
}
