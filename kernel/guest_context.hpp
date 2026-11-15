#pragma once

#include "guest_runtime.hpp"
#include "sr_oracle.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

// Process-wide guest base exported for Frida/WinDbg/TTD.
// Read as: host_addr = g_debugGuestBase + (guest_va & 0x3FFFFFFF)
extern "C" __declspec(dllexport) uint8_t* g_debugGuestBase;

namespace sr {
namespace kernel {

// Current guest memory base and size (set by harness before calling guest code).
// Kernel stubs use this to read/write guest memory.
void setGuestMemory(uint8_t* base, size_t totalSize);
uint8_t* getGuestBase();
// iter-461: SR_LOG_VERBOSE=1 restores the uncapped high-volume diagnostics
// (the city wrote ~1,100 lines/s on the guest's own threads without the caps).
bool logVerbose();
size_t getGuestSize();

// Guest virtual address base (from ppc_config: 0x82000000).
constexpr uint32_t kGuestVABase = 0x82000000u;

// Minimum guest allocation size (bytes) that covers all known used guest VAs and the main heap.
// Guest allocation must cover every VA the game uses; otherwise init skips writes and loads read garbage.
// Must also be >= heap start or g_heapSize is 0 and guestHeapAlloc always fails -> sub_82167F90
// returns 0 -> we never reach ExCreateThread.
// Layout (2026-04-24 wave 6, heap/phys VA overlap FIXED by bidirectional bump allocators):
//   image         [0x82000000..0x829E0000)  (0x9E0000)
//   fallback heap [0x829E0000..0x82AE0000)  (16 MB)
//   primary heap  [0x82AE0000..           )  grows UP
//   phys region   [0x84000000..0xB4000000)  grows DOWN from 0xB4000000
// Primary heap and phys region share a single dividing window [0x82AE0000, 0xB4000000):
// heap grows up from the low end, phys grows down from the high end, and both alloc
// paths fail (return 0) if the two bump pointers would cross. Phys VAs are still
// returned in [0x84000000, 0xBF000000) so any guest code expecting that range is fine.
// Must cover up to 0xBF000000 so phys VAs near the top are inside allocated guest RAM.
// iter-173: raised from 0xB4000000 to 0xBF000000 (kPhysSizeMax=944MB) to fit 10 × 128MB
// physical allocs the game makes before VdInitializeRingBuffer is called.
// kPhysTop must stay < 0xC0000000 — aliasGuestVA aliases 0xC0-0xBF to 0x80-0x9F,
// which would corrupt phys VA→host-pointer translation.
// See RESEARCH_REFERENCE §4b, docs/_attic/DOGMA_ENGINE.md § DOGMA Guard Globals.
constexpr size_t kMinGuestSizeToCoverAllUsedVAs = 0xBF000000ull;

// Convert guest VA to host pointer. Returns nullptr if base not set or VA out of range.
// iter-458 H-STACK: the harness's main-thread guest stack (guest_harness.cpp
// sets r1 from these). It lies INSIDE the physical allocator's arena
// [0xA0000000, 0xBF000000), so phys_alloc excludes it from the arena at init -
// the console never hands a thread's stack out as physical memory, and pf3a
// showed what happens when we do (the page pool landed on the live stack).
constexpr uint32_t kMainStackBase = 0xBE000000u;
constexpr uint32_t kMainStackSize = 0x100000u;   // 1 MB

uint8_t* guestVAToHost(uint32_t guestVA);

// True if a 4-byte access at guestVA is within allocated guest memory (avoids AV when r31 is bad).
// ⚠ THIS IS AN ADDRESS-SPACE BOUND ONLY — it does NOT prove the pages are
// COMMITTED. A VA inside the reservation but uncommitted passes this and then
// FAULTS, and the harness VEH makes any host AV fatal before SEH filters run.
// For any host read a DIAGNOSTIC makes, use hostRangeReadable() below.
// (iter-444: a texture census guarded only by this crashed the roll.)
bool isGuestVAInRange(uint32_t guestVA);

// THE fault-free predicate for every host read a diagnostic makes: true only if
// every page the range spans is committed, readable and NOT a guard page.
// PAGE_GUARD is a MODIFIER bit, so naive masking makes a guard page look
// readable — touching one raises STATUS_GUARD_PAGE_VIOLATION, which nothing in
// the harness handles, and it consumes the page (breaking that thread's stack).
// Walks region by region, so a read straddling a page/region edge is covered.
bool hostRangeReadable(const void* p, size_t bytes) noexcept;
// iter-458 (Astra 068): the same predicate through the interval cache below
// WHATEVER SR_HRR_CACHE says - for default-OFF diagnostics that probe on hot
// paths (the texture census probes every suppressed bind), so their own
// VirtualQuery cost cannot distort the run they observe. Same invalidation
// protocol and the same cache; every other caller is unaffected.
bool hostRangeReadableCached(const void* p, size_t bytes) noexcept;

// iter-458 H-VQ (Astra 035): VirtualQuery on the guest reservation costs 5-10 ms per
// call (it reports the whole multi-GB run of identical pages), and the HLE read
// sites call hostRangeReadable per texture read - measured to keep the FE render
// thread inside ZwQueryVirtualMemory for most of each title frame while it holds
// a guest critical section. SR_HRR_CACHE=1 answers spans INSIDE the guest
// reservation from a cache of readable intervals (host pointers always query;
// unreadable results are never cached). SR_HRR_CACHE_WINDOW=<on>-<off> (seconds
// since process start) limits it to one window for a within-roll A/B.
//
// THE REMOVAL PROTOCOL (Astra 041). Any code that makes guest memory UNREADABLE
// must bracket the change: hostRangeReadableBeginRemoval() before, and
// hostRangeReadableEndRemoval() after. Begin counts an active remover and bumps
// the generation; End bumps again and uncounts. While ANY removal is active the
// cache neither answers nor refills (queries walk VirtualQuery), so no reader can
// cache a still-readable range between the first bump and the removal and serve
// it after. Both are atomics only - VEH-safe - and removers may overlap.
// tools/test_hrr_invalidation_surface.py enforces this over the whole source.
void hostRangeReadableBeginRemoval() noexcept;
void hostRangeReadableEndRemoval() noexcept;
// A plain generation bump, for a reservation IDENTITY change (setGuestMemory).
// Not sufficient for a removal.
void hostRangeReadableInvalidate() noexcept;
struct HostRangeReadableStats {
  uint64_t calls, cachedCalls, hits, vqCalls, vqNs, maxVqNs, invalidations, staleDrops;
  bool cacheOn;
};
HostRangeReadableStats hostRangeReadableStats() noexcept;
// Test seams (difftest only): mode -1 = env/window, 0 = off, 1 = on; the hook runs
// between the VirtualQuery walk and the cache insert.
void hostRangeReadableSetCacheForTest(int mode) noexcept;
void hostRangeReadableSetBeforeInsertHookForTest(void (*fn)()) noexcept;
// Test seam (Astra 040): runs between the lock-free generation sample and the
// lookup's lock - the interleaving where a stale sample could serve a stale hit.
void hostRangeReadableSetAfterGenSampleHookForTest(void (*fn)()) noexcept;
// Pure: is the cache active at `t` seconds for a window [onAt, offAt)?
bool hrrWindowActive(double t, double onAt, double offAt) noexcept;
// Pure: parse "<on>-<off>" (also ':' ';' ',' separators); false unless both are
// finite, on >= 0 and off > on.
bool hrrParseWindow(const char* s, double& onAt, double& offAt) noexcept;
// Pure: SR_HRR_CACHE / SR_HRR_CACHE_WINDOW values (nullptr = unset) -> config.
// A window that is SET (even empty) but unparsable forces the cache OFF.
struct HrrConfig { bool enabled = false, windowed = false, badWindow = false; double onAt = 0, offAt = 0; };
HrrConfig hrrConfigFromEnv(const char* cacheEnv, const char* windowEnv) noexcept;

// Read/write guest memory as big-endian 32-bit (guest byte order).
uint32_t readGuestU32BE(uint32_t guestVA);
void writeGuestBE32(uint32_t guestVA, uint32_t value);

// iter-450S: host window focus, written by the app's SDL event pump (main
// thread), read by the XamInputGetState keyboard-as-gamepad merge (guest
// threads). The gate guarantees unattended/background rolls stay input-
// neutral: keystrokes typed elsewhere can never leak into a recorded roll
// (DOGMA baselines are input-sensitive — a stray Enter is a phantom Start).
void setHostWindowFocused(bool focused);
bool hostWindowFocused();

// Sub-allocate from guest heap (used by NtAllocateVirtualMemory). Returns guest VA or 0.
// Heap region is after image + fallback heap in the guest allocation (dispatch table
// is host-only, see guest_harness.cpp sr::kernel::g_dispatchTable).
uint32_t guestHeapAlloc(size_t size);
// Fallback heap (0x82200000, 16MB) so malloc stub can avoid returning 0 when main heap exhausted.
uint32_t guestHeapAllocFallback(size_t size);

// iter-364 PROPER ALLOCATOR COORDINATION: register a VA range that was placed OUTSIDE
// guestHeapAlloc's bump (NtAllocateVirtualMemory commit-in-place honoring a guest base, the
// DOGMA pool, guest dlmalloc arenas). guestHeapAlloc consults this registry and skips any
// reserved range so its forward bump never re-grants (and zero-fills) memory another allocator
// already owns — the deterministic replacement for the SR_HEAP_SKIP_OCCUPIED content heuristic.
void registerReservedRange(uint32_t start, uint32_t end);
// iter-364: raise guestHeapAlloc's frontier (g_heapNext) to at/above vaEnd so its forward bump
// never re-grants memory the guest dlmalloc already carved below vaEnd. Atomic max; bounded to
// the heap cap and window. The deterministic central-heap coordination (retires SR_HEAP_SKIP_OCCUPIED).
void guestHeapReserveThrough(uint32_t vaEnd);

void dumpHeapState(const char* tag);

// Returns the current GPU ring buffer base VA (set by VdInitializeRingBuffer / alloc fix).
// Returns 0xD0010000 (fake) until the real ring is initialized.
uint32_t getGpuRingBase();

// Sub-allocate from dedicated "physical" region (used by MmAllocatePhysicalMemoryEx).
// Returns guest VA in a contiguous range so pool managers (e.g. DOGMA) see consistent addresses.
// `align` (power of two, min 16) is honored — console Mm allocations are page-aligned and
// the D3DX compiler pool depends on 4KB alignment (page-mask header lookup, iter-449B).
uint32_t guestPhysAlloc(size_t size, uint32_t align = 16u);
// iter-458 H-LIFO (Astra 058): SR_PHYS_LIFO_FREE=1 (default OFF; inert under
// SR_PHYS_FREE). A guest MmFreePhysicalMemory of the MOST RECENT live
// guestPhysAlloc grant - nothing reserved after it - rolls the bump cursor back
// to that grant's recorded pre-reservation value (alignment padding included).
// Unknown, duplicate and non-top frees are REJECTED (a non-top free is recorded
// as released-but-retained). With the gate OFF the allocator is byte-for-byte
// the old bump and every free returns kNotEnabled. Oracle: on Xenia the game's
// 0x12B00000 request is released 45-55 ms after allocation and the next big
// pool reuses its physical pages (docs/astra/058).
enum class PhysLifoFree : int { kNotEnabled = 0, kReclaimed, kUnknown, kDuplicate, kNotTop };
PhysLifoFree guestPhysLifoFree(uint32_t va);
const char* physLifoFreeName(PhysLifoFree r);
bool physLifoEnabled();
void setPhysLifoForTest(int mode);   // -1 = env, 0 = off, 1 = on
size_t guestPhysNextForTest();       // the bump cursor (bytes consumed from the top)
// Test seam (Astra 059): MmFreePhysicalMemory calls physLifoPostFreeHook right
// after guestPhysLifoFree returns - the exact point where a re-grant used to race
// the registry update. Null (the default) = no-op.
void setPhysLifoPostFreeHookForTest(void (*hook)(uint32_t va));
void physLifoPostFreeHook(uint32_t va);
// iter-458 H-WIN: the guestPhysAlloc bump region allocated so far,
// [lo, hi) (lo == hi before the first grant). Never freed, except that with
// SR_PHYS_LIFO_FREE=1 a reclaimed top grant rolls the cursor (and lo) back.
void guestPhysRegion(uint32_t& lo, uint32_t& hi);
// iter-458 H-UNIQ: the primary heap bump's extent [heapStart, high-water).
void guestHeapRegion(uint32_t& lo, uint32_t& hi);

// Xbox 360 physical memory aliasing: uncached VAs (0xC0000000-0xDFFFFFFF) and
// write-combined VAs (0xE0000000-0xFFFFFFFF) alias to the same physical memory as
// cached VAs (0x80000000-0x9FFFFFFF). Our flat guest memory has no aliasing, so
// reads from uncached VAs return zeros unless explicitly resolved.
// Use this when reading data that was written through cached VAs but referenced
// by uncached VAs (e.g., GPU vertex buffers, textures).
inline uint32_t resolveUncachedVA(uint32_t va) {
  if (va >= 0xC0000000u && va < 0xE0000000u) return va - 0x40000000u;
  if (va >= 0xE0000000u) return va - 0x60000000u;
  return va;
}

// iter-427: true when the unified physical-memory model is active (SR_PHYS_UNIFY=1):
// the 0x8/0xA/0xC/0xE windows are real MapViewOfFile3 aliases of ONE 512 MB
// pagefile-backed section (MEMORY_MODEL_PLAN §3, amended — the 0x8 window is a
// view too, because the guest mixes 0x8-/0xA-range VAs for the same physical
// bytes: the movie vertex shader is WRITTEN via 0xAE-class VAs and read back via
// 0x8E-class VAs for the ring copy; under the flat model that read returned
// zeros, which is how the movie VS reached the ring as 27/30 dwords of 0x00
// ([PM4_REJECT]) and the movie quad lost its UV-carrying shader). With real view
// aliasing there is no arithmetic to do — every window IS the same memory.
extern bool g_physUnifyActive;

// iter-460 (kernel/phys_window.cpp, SR_ALIAS_OWNER): a 0xC / 0xE alias already
// folded onto 0x80000000|p goes to 0xA0000000|p when only the physical
// allocator owns that page.
uint32_t foldPhysAlias(uint32_t folded);


// GPU command buffer: fixed guest VA for VdGetSystemCommandBuffer etc. Call after setGuestMemory.
uint32_t getGfxCommandBufferVA();

#if SR_ORACLE
// Guest framebuffer for host display (set by VdSwap stub when game presents a frame).
void setGuestFramebufferVA(uint32_t va, int width, int height, int pitchBytes,
                           bool tiled = false);
bool getGuestFramebufferInfo(uint32_t* outVA, int* outWidth, int* outHeight, int* outPitchBytes,
                             bool* outTiled = nullptr);

// iter-436h: frame-boundary SNAPSHOT. The present path reading LIVE guest
// memory raced our own resolve write-backs and the guest's buffer recycling —
// user-visible as movies flickering between fresh/half-written/cleared-black
// frames. VdSwap = the guest's "frame complete" moment: snapshot the front
// buffer there; the present consumes only snapshots.
void snapshotGuestFramebuffer(uint32_t va, int width, int height, int pitchBytes);
// Returns the latest snapshot (host memory, linear) or nullptr. seqOut
// increments per snapshot so the caller can skip redundant re-presents.
const uint8_t* getGuestFramebufferSnapshot(int* outWidth, int* outHeight,
                                           int* outPitchBytes, uint64_t* seqOut);

// iter-358: dump the guest framebuffer NOW, tagged (frame_<inst>_<why>.bmp).
// Use when the frame you care about is not on the 120/240/360s timer — e.g. the
// game's geometry burst lands ~4s into boot and every timed shot missed it.
void dumpGuestFramebufferTagged(const char* why);

#endif

// iter-358f: resetD3DBumpAllocator() removed with the fake-D3D-vtable subsystem
// (0x8200094C was KeCertMonitorData, not a device). See docs/NATURAL_BASELINE.md 2026-07-17.

// VdSwap frame counter (incremented each time the guest calls VdSwap).
uint64_t getVdSwapCount();
void incrementVdSwapCount();

// Test-pattern front buffer VA. Its only producer (setupFakeGpuDevice) was removed in
// iter-358f, so this now always returns 0; callers already treat 0 as "no test buffer".
uint32_t getTestFrontBufferVA();

// Guest lr at kernel call (set by import glue before callOrStub; used by KeBugCheck to identify caller).
void setCurrentGuestLR(uint32_t lr);
uint32_t getCurrentGuestLR();

// Current guest context (set by harness while guest runs). Used to sample guest PC/ctr/r3/r1/r31 when idle (spin debug).
// ctx is opaque; offsets are offsetof(PPCContext, lr) etc. so kernel can read without guest headers. Optional offsets 0 = do not sample.
void setCurrentGuestContext(void* ctx, size_t lrOffsetBytes, size_t ctrOffsetBytes = 0, size_t r3OffsetBytes = 0, size_t r1OffsetBytes = 0, size_t r31OffsetBytes = 0);
// Returns guest lr from current context if set; 0 otherwise. May be slightly stale (guest thread may be running).
uint32_t getCurrentGuestPC();
uint32_t getCurrentGuestCtr();
uint32_t getCurrentGuestR3();
uint32_t getCurrentGuestR5();  // iter-344: r5 = r3 offset + 16 (contiguous PPCRegisters)
uint32_t getCurrentGuestR1();
void setCurrentGuestR1(uint32_t r1);
uint32_t getCurrentGuestR31();
// Read any PPCContext field by byte offset. Used by image-write watchdog
// to capture callee-saved regs (r28-r30, etc.) that hold preserved-across-call
// values like the original r3 passed in.
uint32_t getCurrentGuestRegAtOffset(size_t offsetBytes);

// iter-295g: process-global registry of ALL guest-running threads (main,
// FM workers, host driver threads). setCurrentGuestContext auto-registers/
// deregisters the calling thread; setGuestThreadName labels it. Exported as
// g_debugThreadRegistry (fixed 64-slot array, 64B stride) so the Frida layer
// can enumerate EVERY thread's live guest registers — closes the "PPC Frida
// only tracks the main thread" gap. The host-side sampler (SR_THREAD_SAMPLER,
// default ON) logs each registered thread's lr/r1/r3/ctr periodically, so any
// hang names its own location in the log without attaching anything.
struct GuestThreadSlot {
  uint32_t tid;        // host thread id; 0 = slot free
  uint32_t seq;        // bumped on (re)register — lets readers detect reuse
  void*    ctx;        // PPCContext* (thread-owned; reads are racy snapshots)
  uint32_t lrOff, r1Off, r3Off, ctrOff;
  char     name[32];   // optional label (setGuestThreadName)
};
static_assert(sizeof(GuestThreadSlot) == 64, "Frida reads this layout — keep 64B stride");
void setGuestThreadName(const char* name);   // call AFTER setCurrentGuestContext
void startGuestThreadSampler();              // idempotent; gated SR_THREAD_SAMPLER
// iter-461: the sampling profiler (SR_GUEST_PROFILE_MS=<interval>; kernel/guest_profile.cpp):
// self + inclusive histograms per guest thread -> archive/guest_profile/prof_<pid>.txt,
// symbolized by tools/guest_profile.py.
void startGuestProfiler();
void dumpGuestThreadsNow(const char* tag);   // iter-388e: on-demand snapshot

// Process-global snapshot of the MAIN guest thread's PPCContext pointer.
// Unlike setCurrentGuestContext (TLS), this is readable from any host thread
// (e.g. host pollers running on detached worker threads). Set once from the
// harness right after setIsMainGuestThread(true). Reads are racy (the main
// thread is mutating these fields constantly) but adequate for diagnostics.
void setMainGuestContext(void* ctx, size_t lrOffsetBytes, size_t ctrOffsetBytes,
                         size_t r3OffsetBytes, size_t r1OffsetBytes);
uint32_t getMainGuestPC();
uint32_t getMainGuestCtr();
uint32_t getMainGuestR3();
uint32_t getMainGuestR1();
uint32_t getMainGuestR13();  // iter-345d: Xenon TLS reg (VD ISR gate reads through it)

// One-time log for sub_8276A4B0 table-search spin: key we're looking for and range [start,end). Fix: ensure list at 20(r31) is populated with this key (find populator and ensure it runs).
void logSub8276A4B0SearchRange(uint32_t key, uint32_t start, uint32_t end);

// One-time log for sub_827607B0: r5 (length) and r10 (step) when entering hash loop loc_827607DC. If r5 is huge we spin there; fix caller/length (e.g. string terminator).
void logSub827607B0HashLoop(uint32_t r5, uint32_t r10);

// One-time log for sub_82763C50: bounds 8(r6),20(r6),28(r6),40(r6) that drive loops (idle at lr=0x827644E0 when loops are huge).
void logSub82763C50LoopBounds(uint32_t off8, uint32_t off20, uint32_t off28, uint32_t off40);

// One-time log when sub_8279EC60 (memcpy-style) is called with huge r5 (idle at lr=0x82164870). Proper fix: fix caller sub_82164818 so args r29/r30/r31 are valid; do not cap in sub_8279EC60.
void logSub8279EC60Size(uint32_t r3_dest, uint32_t r4_src, uint32_t r5_size);

// One-time log when sub_82709470 has ctr==0 and vtable patch cannot apply (vtable_base==0 or slot0==0); explains idle at 0x8276061C.
void logSub82709470CtrZero(uint32_t vtable_base, uint32_t slot0);

// One-time log when sub_82760928 is entered with bad r5/r6 (feeds sub_82164818 -> sub_8279EC60). lr = caller of sub_82760928; use to find and fix that call site.
void logSub82760928BadArgs(uint32_t r4, uint32_t r5, uint32_t r6, uint32_t lr);

// One-time log when we skip sub_82762540 or sub_827605F8 because count/size field > 64K (BANDAIDS); confirms guard is on the path.
void logGuardSkipSub82762540(uint32_t val32_r31);
void logGuardSkipSub827605F8_82764244(uint32_t val8_16_r31);
void logSub827607A0HugeR9(uint32_t r9_count);  // one-time: r9 > 64K at 0x827607A0 (no skip; causes spin at 0x8276061C)

// Worker thread trace: logs checkpoint label + two values. Used to instrument sub_827A7420.
void logWorkerTrace(const char* label, uint32_t val1, uint32_t val2);

// Short alertable yield: SleepEx(ms, TRUE) so queued APCs can fire.
// Use in GPU-wait bypasses where we skip the real KeWaitFor but still need APC delivery.
void alertableYield(uint32_t ms = 1);

// (iter-463: logOutOfRangeIndirectCall is gone - every unresolvable indirect
// call goes to sr::kernel::indirectCallUnresolved, kernel/indirect_call.cpp.)

// Diagnostic: log once for large cleanup loops in init (sub_822EC980 etc.)
void logDiagCleanupLoop(const char* label, uint32_t count, uint32_t ptr);

// One-time log when sub_827606E0 depth cap is hit (avoids stack overflow at lr=0x82766C30).
void logSub827606E0DepthCap(int depthMax);
// One-time log when stack overflow guard triggers in sub_827606E0 (host stack depth exceeded threshold).
void logSub827606E0StackGuardTriggered();
// One-time log when sub_827606E0 sees sub_82760928 return 0 (alloc failed); we return 0 so drain reports insert_failed.
void logSub827606E0AllocFailed();
// One-time log when sub_827607B0 returns fake entry (reason: "stack_guard" or "insert_failed").
void logSub827607B0ReturningFake(const char* reason);
// One-time log when sub_82164818 fallback (guestHeapAlloc) returns 0 — heap exhausted or failing.
void logSub82164818FallbackHeapFailed(uint32_t requestedSize);
// Rate-limited log when RtlUnwind is called (caller_lr = return address; r3/r4 = first args). Debug exception loop.
void logRtlUnwindCall(uint32_t caller_lr, uint32_t r3, uint32_t r4);

// Returns a guest VA pointing to a 256-byte zeroed block.
// Safe for use as a "dummy hash entry" — all reads yield 0.
// Allocated once via guestHeapAlloc; cached afterwards.
uint32_t getDummyHashEntry();

// Initialize Xbox 360 kernel data structures at 0x81800000+.
// Must be called after setGuestMemory and before running guest code.
// This sets up KPCR, module info, and other kernel state the game expects.
void initKernelDataStructures();

// ExCreateThread: register a runner so the stub can create host threads that run guest code.
// stackVA = guest VA of thread stack (allocated by stub via guestHeapAlloc).
using ExCreateThreadRunnerFn = void (*)(uint32_t startVA, uint32_t startParam, uint32_t stackVA, uint8_t* base, size_t size);
void registerExCreateThreadRunner(ExCreateThreadRunnerFn fn, uint8_t* base, size_t size);
ExCreateThreadRunnerFn getExCreateThreadRunner(uint8_t** outBase, size_t* outSize);

// iter-358f: startRenderThreadHost() removed — it drove a guest "render loop" that only
// existed to service the fake D3D device built on the misidentified KeCertMonitorData
// import slot (0x8200094C). See docs/NATURAL_BASELINE.md 2026-07-17.

// Returns true when registerExCreateThreadRunner has been called (guest memory is available).
bool isGuestMemoryRegistered();

// Get the global (non-TLS) guest memory base and size. Usable from any thread (e.g. GPU interrupt thread).
void getGlobalGuestMemory(uint8_t** outBase, size_t* outSize);

// iter-378: seedDogmaFallbackPool DELETED - it fabricated a pool + vtable whose
// slot 9 pointed at the (also deleted) sub_8290E040 shim. Natural boot never
// needed it (master-gated off, 0 [POOL_SEED] fabrications in any 2026-07-26 run).

// iter-358f: setupFakeGpuDevice() removed — fabricated a D3D device around
// 0x8200094C, which is the KeCertMonitorData import thunk (NULL on retail by
// design), not a render device. See docs/NATURAL_BASELINE.md 2026-07-17.

// Host stack overflow guard: set once at guest thread start (address of a local). When too deep, return true
// so recompiled code can break recursion/loops and avoid 0xC00000FD.
void setGuestThreadStackAnchorForOverflowCheck(void* anchor);
void* getGuestThreadStackAnchorForOverflowCheck();
/** Returns true when host stack depth exceeds limit (call from recomp code to break recursion). */
bool isGuestThreadStackOverflowGuardTriggered();
/** Returns true when host stack depth exceeds half the thread limit (early guard for hot recursive paths). */
bool isGuestThreadNearStackLimit();

// --- ExCreateThread path diagnostics (temporary) ---
void logSub82106010Entry(uint32_t r3, uint32_t r4);
void logSub82167F90Result(uint32_t result);
void logExThreadPathBranch(const char* tag, uint32_t val1, uint32_t val2);
void logSub82106010Path(const char* tag, uint32_t val1, uint32_t val2);
/** One-time log when main() returns from init2 (sub_8218CFE8). r3 = return value. */
void logInit2Returned(uint32_t r3);
/** Init2 progress: log once per step (step names "after_sub_827742F0" etc.) to see how far init2 gets. */
void logInit2Progress(const char* step);
/** General-purpose log for module system events. */
void logModuleEvent(const char* msg);

/** iter-301: SR_NATURAL_MODULE_INIT gate (default ON; =0 restores the
 *  E630-skip band-aid). Used by the sub_8271E928/sub_8271F2C8 splices to run
 *  the game's own dependency-ordered FM module init walk. */
bool naturalModuleInitOn();

/** 2026-07-13 PRIME DIRECTIVE master gate (docs/NATURAL_PATH_CHARTER.md).
 *  Default OFF: the default build forces NOTHING. Every forcing band-aid MUST
 *  gate on this; SR_ALLOW_FORCING=1 enables forcing as opt-in diagnostics only.
 *  Platform fixes (recompiler/CS/TLS/GPU/kernel imports) do NOT gate on this.
 *  iter-455l r4 NAMED ISOLATION: when SR_FORCING_ISOLATE is set, every ordinary
 *  forcingAllowed() caller is DENIED and only forcingAllowedFor(name) with the
 *  byte-exact isolation name is allowed (unknown/malformed values fail closed).
 *  With isolation unset, both behave as legacy. The pure isolationAllows*
 *  functions expose the semantics for the self-test matrix. */
bool forcingAllowed();
bool forcingAllowedFor(const char* gateName);
bool isolationAllowsOrdinary(bool master, const char* iso);
bool isolationAllowsNamed(bool master, const char* iso, const char* gateName);

/** iter-460: true when NO real command processor runs (the asynchronous CP,
 *  kernel/gpu_command_processor.cpp / SR_PM4_CP, is off). The spliced "fake
 *  GPU" guest edits (disabled GPU waits, faked retired fence, skipped replay
 *  driver - tools/regen/patch_fakegpu_gate.py, docs/BANDAIDS.md) apply only
 *  then; with the CP on, the guest runs its pristine code. */
bool gpuFakeCompletion();

/** iter-467 p2: the sub_8210C090 FLOAT16_2 pack (SurfaceClearColorPackByFormat,
 *  fmt 6 k_16_16_FLOAT: `vpkd3d128 v0,v1,3,1,3`) = Xenia's float_to_xenos_half
 *  (L6 difftest 44/44). R1: XenonRecomp lowers it itself (tools/regen/patches/0005),
 *  so the guest code has one path and this is always true (the SR_SPLICE_VPK_F16
 *  A/B knob went with the old hand body). Still the ONE predicate a native port
 *  of its callers (strand B's rs_colorPack) asks. ENGINE_MAP 6g.8a. */
bool spliceVpkF16();

/** iter-460 S1: CPU-write detection for GPU-visible memory (Windows write
 *  watch on the guest arena). Wanted when the PM4 path is on (SR_PM4_LIVE /
 *  SR_PM4_RTCACHE) or SR_WRITE_WATCH=1; SR_WRITE_WATCH=0 disables. The arena
 *  allocator asks guestWriteWatchWanted() and reports the outcome. */
bool guestWriteWatchWanted();
void setGuestWriteWatchActive(bool active);
bool guestWriteWatchActive();
/** Appends the guest VAs of the 4 KiB pages in [va, va + size) written since
 *  the previous call for that range, and resets them. Returns the count. */
size_t guestCollectWrittenPages(uint32_t va, uint32_t size, std::vector<uint32_t>& outVa);

/**
 * Read FM Module Manager singleton pointer at guest VA 0x829A2BF0.
 * Returns 0 if NULL (singleton not yet constructed).
 */
uint32_t getFmManagerSingleton();
/**
 * Diagnostic: log the current singleton value with a tag.
 */
void logFmManagerSingleton(const char* tag);
/**
 * Dedicated splash/PSM trace (separate budget from EXTHREAD_PATH).
 */
void logSplashTrace(const char* tag, uint32_t val1, uint32_t val2);

/**
 * Read FMGamePlayStateMgr instance pointer at guest VA 0x8296EDAC.
 * Returns 0 if NULL (instance not constructed).
 */
uint32_t getFmGamePlayStateMgrInstance();
/**
 * Read splash counter at FMGamePlayStateMgr instance + 0x230.
 * Returns 0xFFFFFFFF if instance is NULL.
 */
uint32_t getFmGamePlayStateMgrSplashCounter();
/**
 * Read splash done flag at FMGamePlayStateMgr instance + 0x234.
 * Returns 0 if instance is NULL or splash not yet done.
 */
uint8_t getFmGamePlayStateMgrSplashDone();

/**
 * FM module CTOR walker logging — emits one line per begin/end so we can
 * pinpoint which CTOR hangs (last "begin" with no matching "end").
 * Unbudgeted; safe to call freely.
 */
void logFmCtorEvent(const char* event, const char* moduleName, uint32_t ctorAddr);

/** 
 * Init3 ready-flag bridge: init3 (sub_82176300) polls byte at [gameState+24]
 * waiting for it to become non-zero. On real hardware, the render thread or
 * a GPU callback sets this. We capture the address when init3 starts and
 * signal it from the host once the render thread is producing frames.
 */
void setInit3GameStateVA(uint32_t gameStateGuestVA);
void signalInit3Ready();  // called from VdSwap after first frames
// iter-358f: patchD3DDeviceVtable / pumpRenderState / linkRenderContext /
// d3dVtableStubDispatch[Ex] removed. All hung off guest VA 0x8200094C, which is the
// KeCertMonitorData import thunk (PIX cert-monitor hook, NULL on retail by design) —
// never a D3D device. See docs/NATURAL_BASELINE.md 2026-07-17.

// Per-thread guest handle (set by harness/ExCreateThread runner; read by NtDuplicateObject(-2)).
void setCurrentThreadHandle(uint32_t handle);
uint32_t getCurrentThreadHandle();

// iter-30: walker-phase worker thread deferral. While the FM module CTOR
// walker is replaying init in dependency order on the main thread, the game
// also fires off ExCreateThread for `startVA=0x827A7420` (DOGMA worker
// dispatch routine). Those workers race the main thread inside sub_82182700
// at the pool's CS+1328 — wave-10 witness empirically captured 20+ contention
// events when both fire concurrently, and walker reproducibility drops to
// ~1/3. Original Xbox 360 launches workers AFTER the engine-level CTOR walk
// completes; we replay the walk on host fallback, so the timing is broken.
//
// Walker calls beginWalkerPhase()/endWalkerPhase() around walkFmModuleCtors_impl.
// During the phase, ExCreateThread for startVA=0x827A7420 queues the spawn
// args (with a real handle allocated and returned to guest) but doesn't call
// CreateThread. endWalkerPhase() drains the queue, starting all deferred
// workers. Net effect: main thread is the SOLE participant in DOGMA pools
// during the walker, eliminating the race precondition entirely.
void beginWalkerPhase();
void endWalkerPhase();
bool isWalkerPhaseActive();

// iter-DOGMA-RE-10: Walker watchdog panic-release. Pumps a single
// ReleaseSemaphore on every known semaphore handle. Use only from the
// walker watchdog thread when a CTOR has been stalled past its timeout.
unsigned releaseAllPendingSemaphoresPanic();

// Main-thread identification: set true on the guest harness thread so
// kernel call tracking can distinguish it from worker/render threads.
void setIsMainGuestThread(bool v);
bool isMainGuestThread();

// Main-thread kernel call histogram: tracks per-export call counts on the main thread only.
// dumpMainThreadCallHistogram logs and resets the counters.
void incrementMainThreadCallCount(const char* name);
void dumpMainThreadCallHistogram();
// Separate counter for main-thread-only kernel calls (compare with global totalCalls).
uint32_t getMainThreadCallCount();


// iter-197: the Apt level node loadMovie targets (published by the
// sub_826B3998 logger override) — fabricator dumps its state per tick.
extern std::atomic<uint32_t> g_aptLevelNodeVA;

// iter-198: package streamer instance (see guest_context.cpp).
extern std::atomic<uint32_t> g_streamerVA;

}  // namespace kernel
}  // namespace sr
