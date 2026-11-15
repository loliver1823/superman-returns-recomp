#pragma once

// Stable interface consumed by generated PowerPC memory/call macros.
// Keep host-only diagnostics and lifecycle APIs in guest_context.hpp: adding them
// here invalidates every generated guest translation unit.
#include <atomic>
#include <cstdint>

namespace sr {
namespace kernel {

// iter-461 perf: the same per-thread base, read INLINE by the guest memory macros
// (PPC_GUEST_BASE) - kept equal to getGuestBase() by setGuestMemory.
// iter-465 perf2: `constinit` (here AND on the definition). Without it clang (MSVC ABI)
// cannot see that the definition is constant-initialized and emits the on-demand TLS
// init guard - TLS index, gs:[0x58], a guard-byte compare and a call to
// __dyn_tls_on_demand_init that clobbers every volatile register - at EVERY use: every
// guest load, store and SIMD access of the build.
#if __cplusplus >= 202002L
extern thread_local constinit uint8_t* g_guestBaseTls;
#else
extern thread_local uint8_t* g_guestBaseTls;
#endif
// iter-461 perf: the PPC_STORE watchpoints (module globals, vtable area, the
// dynamic render-object / FMUI-queue / Apt-env / world-pointer watches) run only
// when this is set: SR_STORE_WATCH=1, or a kernel-side arm site sets it.
extern bool g_storeWatchOn;
// iter-461 perf: PPC_CALL_INDIRECT_FUNC calls traceIndirectCall only when set (SR_TRACE_CALLS=1).
extern bool g_traceCallsOn;

// Full VA aliasing for PPC memory macros. Maps uncached (0xC0-0xDF) and write-combined
// (0xE0-0xFF) VAs to their cached equivalents, EXCEPT for our fake D3D infrastructure
// region at 0xD0000000-0xD00FFFFF which must remain unaliased.
// Called on every PPC_LOAD/STORE and SIMD access, so keep the fast path (< 0xC0) branchless.
// iter-465 perf2: aliasGuestVA's cold half (VA >= 0xA0000000 not covered inline).
// preserve_most: the call site spills nothing on the hot path's behalf.
// iter-466 tick: `pure` (reads memory, writes none - the page-owner tables and, for a
// contested page, guest bytes; phys_window.cpp). Without it the cold call at EVERY guest
// access could, as far as the compiler knows, write the caller's PPCContext (ctx escapes
// to callees), so every live ctx field was reloaded after every guest load/store.
#if defined(__clang__)
__attribute__((preserve_most, noinline, pure))
#endif
uint32_t aliasGuestVASlow(uint32_t va);
// true while VA 0xA0000000-0xBFFFFFFF is identity (SR_PHYS_ALIAS_A unset).
extern bool g_physAliasAIdentity;

inline uint32_t aliasGuestVA(uint32_t va) {
  // iter-373: 0xA0000000-0xBFFFFFFF was MISSING from this enumeration. 0xC and
  // 0xE were folded, 0xA never was — yet on console all THREE are aliases of the
  // same physical RAM (ENGINE_MAP §6e; Xenia folds any of them by va & 0x1FFFFFFF).
  // Folding 0xA by 0x20000000 lands it on 0x80000000-0x9FFFFFFF, exactly where
  // the 0xC (-0x40000000) and 0xE (-0x60000000) folds already point, so all three
  // physical windows finally agree.
  //
  // WHY IT MATTERS, measured: the guest puts its GPU fence/sync words in the
  // physical range ([GPU_RING_ALLOC] #5 -> 0xAEFEECE0, #6 -> 0xAEFEECC0) and polls
  // them there, while the PM4 engine's EVENT_WRITE_SHD handler signals through
  // physToHost = 0x80000000 + (phys & 0x1FFFFFFF) = 0x8EFEECE0. The guest waited on
  // one address and we signalled another 0x20000000 away, so every GPU fence was
  // invisible to it — the suspected reason submission stops dead after the opening
  // command buffer (24 draws, then 2559 IBs of pure bookkeeping).
  //
  // ⛔ DEFAULT-OFF — CONSOLE-CORRECT BUT TRUNK-REGRESSING (iter-373 A/B).
  // SR_PHYS_ALIAS_A=1 opts in.
  //
  // Measured, K=4 200s matched A/B, ring-exec on both sides:
  //   OFF: feTick 2870 / 17 / 17 / 9  — including a DEEP roll
  //        (defExec=666 clipCtorReal=451 movInst=451)
  //   ON : max feTick 9, 0/4 deep
  // It also did NOT do what it was meant to: submission volume was IDENTICAL
  // either way (2560 descriptors, ~714 KB), so the fence-starvation hypothesis
  // (workflow H7) is REFUTED — the guest was never waiting on those fences.
  //
  // WHY IT REGRESSES, and why this matters more than the fix: folding 0xA onto
  // 0x80000000 lands physical-range data on the VIRTUAL heap, where the bump
  // allocator may already have handed those addresses out. So the fold trades a
  // stale-fence bug for a memory-collision bug. That is deviation §2.2 in
  // docs/MEMORY_MODEL_PLAN.md, and this A/B is the empirical proof that folding
  // physical onto virtual is the wrong shape: the correct fix is SEPARATE
  // physical backing (§3 view aliasing), not a better subtraction.
  // Precedent: iter-365 SR_MALLOC0_FIX — console-correct, trunk-regressing,
  // opt-in until the underlying layout issue is root-fixed.
  // ⚠️ THE EARLY RETURN MUST COME FIRST. A function-local `static const` compiles
  // to an MSVC thread-safe-static init guard (a TLS load + branch); placing it
  // above this line put that guard in front of the fast path at ~600,000 call
  // sites — every guest load, store and SIMD access in the build. That was my own
  // iter-373 regression, caught by the iter-376 triage. Hoisting the early return
  // above it keeps the common case (virtual heaps: the XEX at 0x82, the heap at
  // 0x8A) exactly as branchless as it was.
  if (__builtin_expect(va < 0xA0000000u, 1)) return va;   // virtual heaps
  // iter-465 perf2: the 0xA window is identity unless SR_PHYS_ALIAS_A=1 or the
  // unified model is active (g_physAliasAIdentity, set once in setGuestMemory);
  // everything else (0xC / 0xE folds, the 0xD carveout, the A/B gates) is ONE
  // out-of-line function instead of a copy at every access site. Same result
  // for every VA as the iter-460 inline body (aliasGuestVASlow in guest_context.cpp).
  if (va < 0xC0000000u && g_physAliasAIdentity) return va;
  return aliasGuestVASlow(va);
}

// iter-214 (L4 observability): env-gated indirect/vtable/computed-call tracer. Build ONCE;
// configure at runtime. SR_TRACE_CALLS=1 enables; SR_TRACE_LO/SR_TRACE_HI (hex) bound the
// target VA range (default = code section). Emits [CALLTRACE] target+callerLR for each call
// into the range (flood-capped). Near-zero cost when off (one cached-bool check). Lets us see
// dynamic control flow through dispatch tables/vtables WITHOUT a rebuild per investigation.
void traceIndirectCall(uint32_t target, uint32_t callerLR);
// L9-B (iter-216): FULL per-function-entry guest-execution trace (SR_GUEST_TRACE=1). Called from
// PPC_FUNC_PROLOGUE at every guest function entry; parses the guest VA from the recomp fn name
// ("__imp__sub_XXXXXXXX") and writes one canonical XENTRC01 record {va, lr, r3, r1, tid}. g_guestTraceOn
// is the cheap hot-path gate (~free when off). Feeds the L9-C divergence bisector at Xenia granularity.
extern bool g_guestTraceOn;
void emitGuestTraceEntry(const char* func, uint32_t lr, uint32_t r3, uint32_t r1);

void vtableWatchpoint(uint32_t guestAddr, uint32_t value, const char* file, int line);

// --- DOGMA guard1 write watchpoint (temporary debug instrumentation) ---
// Called from PPC_STORE_U32 macro when store target == 0x829AAE78.
// Logs file:line (recomp source), value written, and whether it differs from expected (1).
// Disable by removing the check in PPC_STORE_U32 once writer is found.
void dogmaGuardWatchpoint(uint32_t guestAddr, uint32_t value, const char* file, int line);

// --- Module-global write watchpoint (temporary debug instrumentation) ---
// Called from PPC_STORE_U32 when store target is one of:
// 0x8296D8D8/DC/E0/E4 or 0x829AD878/87C/880/884 (sub_82177E60 store at r30-10024, r30=0x829B0000).
// Used to trace who initializes the main module pointers for sub_821863A0.
void moduleGlobalWatchpoint(uint32_t guestAddr, uint32_t value, const char* file, int line);

// --- Render-object vtable write watchpoint ---
// Fires when any store hits the game render object (at the VA stored in g_renderObjVA).
// g_renderObjVA is set lazily by the RSTATE diagnostic in sub_8210B288 once
// the global chain 0x8200094C → renderObjVA is first observed to be non-null.
// FUN_8210be70 destructs this object (zeroes vtable[0]) ~frame 1073, causing
// sub_8210B1A8 to skip RSO allocation → cmd43 DRAW stops firing.
extern std::atomic<uint32_t> g_renderObjVA;
void renderObjDestructWatchpoint(uint32_t guestAddr, uint32_t value, const char* file, int line);

// --- FMUI pool queue-field write watchpoint (iter-191) ---
// The FMUI pool worker entry wrapper (sub_826F92E0 override) publishes
// workerVA+180 here. 191a proved the queue {begin,end} at that address is valid
// at worker entry and gets overwritten with .data addresses within ~3 s, turning
// the pop-compact (sub_826F9CD0) into a 19 KB misaligned .data self-copy — the
// root mechanism of the modA-D/PSM/Apt-singleton byte-stripping family.
extern std::atomic<uint32_t> g_fmuiPoolQueueVA;
void fmuiQueueWatchpoint(uint32_t guestAddr, uint32_t value, const char* file, int line);

// --- Apt env-object write watchpoint (iter-192) ---
// Published from the env-store site (sub_826D1298) and the flow factory.
// 192a: the env object's vtable word (env+0) was zeroed on heap (suspect:
// DOGMA double-grant overlap — new owner zero-fills) → factory alloc skipped
// → g3 never set. Watch [env, env+16) to attribute the zeroer.
extern std::atomic<uint32_t> g_aptEnvVA;

// --- FM hash-set base ptr (module+4) corruption watch (iter-292e) ---
// Pins the adjacent-overflow writer that clobbers the embedded DOGMA hash-set
// base pointer at module+4 with a garbage offset (the main-thread-stall root:
// corrupted base -> sub_825FDA60 infinite rehash -> menu blocked). Armed from
// moduleGlobalWatchpoint the moment a module ptr installs into 0x8296D8D8+,
// env-gated by SR_WATCH_WORLDPTR=1. Any U32/U64 store landing on one of the 4
// fields logs the writer's liveLR (va_to_source -> the corrupting instruction).
extern std::atomic<uint32_t> g_worldPtrFieldVA[4];
extern std::atomic<bool> g_worldPtrWatchArmed;
void worldPtrFieldWatchpoint(uint32_t guestAddr, uint32_t value, const char* file, int line);

}  // namespace kernel
}  // namespace sr
