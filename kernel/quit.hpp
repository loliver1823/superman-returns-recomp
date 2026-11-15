// tools/graceful-quit: THE GRACEFUL QUIT AND THE TERMINAL SNAPSHOT (docs/TOOLKIT.md 1.8m).
//
// Live rolls used to end with the harness's Stop-Process -Force: no exit code ran, so every periodic census
// ([PM4_COV] / [PM4_COVMAP], the [GPU_CP] census and the transport lines beside it, [NATIVE_FLUSH], the native
// meter, [AUDIO_NATIVE]) had a blind tail of up to one reporting period. This module lets the harness ASK the game
// to quit: the game cuts the ring, drains the command processor, stops every guest-running thread at a park point,
// prints every counter family once more with the why token "terminal", re-renders them to prove nothing moved,
// prints `[QUIT] terminal snapshot complete frames=N ...`, closes the log and exits 0.
//
// THE PROTOCOL (automated runs only - SR_INSTANCE_ID set; a player never has it; SR_QUIT_CHANNEL=0 turns it off):
//   request  Local\SR_QUIT_<id>_<pid>      manual-reset event the game creates; the harness sets it
//   ack      Local\SR_QUIT_ACK_<id>_<pid>  manual-reset event the game sets when it takes the request
//   <id> = SR_INSTANCE_ID sanitised to [A-Za-z0-9_.-] (at most 64, "default" when empty), <pid> = the game's PID.
//   Exit codes: 0 = terminal snapshot complete, 81 = the quiesce failed (a FAILED line), 82 = the hard deadline (the
//   sequence itself hung). The harness side: tools/sr_quit.ps1; the graders: tools/perf/terminal_snapshot.py.
//
// THE STAGES (run(), every wait bounded): claim -> ack -> RING CUT (written under the CP's publish mutex: no doorbell
// publishes after it) -> CP DRAIN (the CP consumes up to the final write index, flushes the native executor, parks)
// -> THREAD CUT (every thread parks at its next gate: a kernel import's entry or exit, a doorbell, a host loop's
// safe point or guest call - lock holders and exempt loops included) -> SETTLE (every thread ACKNOWLEDGED - parked,
// or blocked inside an import (its exit gate parks it), or a host loop between guest calls - and the producer digest
// unchanged, for settleMs) -> TERMINAL SNAPSHOT (the families' printers, captured) -> VERIFY (settleMs later: every
// thread still acknowledged and a capture-only re-render of the families equal on every integer field the snapshot
// printed) -> complete line -> exit.
//
// LOCKS. No thread parks holding a tracked guest lock WHILE THE CP DRAINS (the ISRs it runs may need one): the kernel
// spinlock and critical-section imports count the locks a thread holds (guest/ppc_import_glue.cpp), and a holder's
// doorbell between the ring cut and the thread cut is dropped (never published, never parked). After the drain, at
// the thread cut, holders park like everyone else: nothing the quit still needs can wait on what they hold.
//
// ⛔ NOT FORCING: nothing here writes guest memory or calls guest code. It ends a run the harness is ending anyway,
// the way a debugger freezes threads; the measured window closes at the ring cut.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace sr::kernel::quit {

// ── The gates the hot paths test: false for the whole run until a quit ──────────────────────────────────────────
inline std::atomic<bool> g_ringCut{false};    // set (under the CP publish mutex): gpuCpDoorbell publishes nothing
inline std::atomic<bool> g_threadCut{false};  // set: guest threads park at their next gate

// ── Names (pure; tools/sr_quit.ps1 mirrors them) ─────────────────────────────────────────────────────────────
std::string sanitizeInstance(const char* instanceId);
std::string requestEventName(const char* instanceId, uint32_t pid);
std::string ackEventName(const char* instanceId, uint32_t pid);

constexpr int kExitComplete = 0;
constexpr int kExitFailed = 81;
constexpr int kExitDeadline = 82;

// Once, from main after the log and the automation policy: in an automated run, creates the channel and starts the
// watcher thread. One [QUIT] channel line either way.
void init();

// SR_QUIT_*_MS: decimal digits only, within [lo, hi]; anything else (empty, a sign, "-1", a unit, overflow, out of
// range) is the default, with *bad set when the text was present but rejected.
uint32_t parseMs(const char* text, uint32_t def, uint32_t lo, uint32_t hi, bool* bad);

// ── Per-thread gate state (one slot per thread that reaches a gate; read by the coordinator) ─────────────────────
struct GateSlot {
  std::atomic<uint32_t> tid{0};
  std::atomic<const char*> import{nullptr};  // the innermost kernel import this thread is inside (nullptr: none)
  std::atomic<int> locks{0};                 // tracked guest locks held (kernel spinlocks + critical sections)
  std::atomic<int> guestCalls{0};            // GuestCaller::call depth (a host loop inside guest code)
  std::atomic<const char*> role{nullptr};    // nullptr = guest thread; kHostLoop; else the exempt loop's name
  // native/quit-drain: the XDK GPU wait this thread is inside (GpuWaitKind below; 0 = none), its device and target,
  // the residence's sequence number (g_gpuWaitSeqGen: unique process-wide; an outer wait resumed after a nested one
  // gets a NEW one), its phase (GpuWaitPhase) and, for a wait in its spin loop, the spin's own sequence number;
  // native/quit-ringspace: the ring-space wait's generation (r5) and the wait's own record address (kWaitRecBelowSp:
  // what its spin loop passes GpuRingWaitProgress); gpuWaitVer = a seqlock over those eight (odd while the owner writes
  // them). gpuSpins = the spin loop's calls of GpuRingWaitProgress (the evidence the loop is still turning).
  // Owner-written loads and stores, no RMW.
  std::atomic<uint32_t> gpuWaitVer{0};
  std::atomic<uint32_t> gpuWait{0}, gpuWaitDev{0}, gpuWaitArg{0}, gpuWaitPhase{0}, gpuWaitGen{0}, gpuWaitRec{0};
  std::atomic<uint64_t> gpuWaitSeq{0}, gpuSpinSeq{0}, gpuSpins{0};
};
inline constexpr const char* kHostLoop = "host-loop";
GateSlot* claimGate();  // this thread's slot, claimed once (a full table answers a shared overflow slot)
inline thread_local GateSlot* t_gate = nullptr;
inline GateSlot& gate() {
  GateSlot* g = t_gate;
  return g ? *g : *claimGate();
}

// ── The import gate (guest/ppc_import_glue.cpp callKernel / callKernelFast) ───────────────────────────────────
// The slow paths (the thread cut is set): park this thread forever - unless, at the entry, `importName` releases
// something: it parks at that import's exit instead, after the release.
void parkAtImport(const char* importName);
void parkAfterImport(const char* importName);
// Entry: the gate, then "inside importName" for the coordinator. Returns the previous import (nested imports).
inline const char* importEnter(const char* importName) {
  if (g_threadCut.load(std::memory_order_relaxed)) parkAtImport(importName);
  GateSlot& g = gate();
  const char* prev = g.import.load(std::memory_order_relaxed);
  g.import.store(importName, std::memory_order_release);
  return prev;
}
// Exit (right after the export returns, before any guest code or APC): "outside", then the gate - a thread that
// waited in the import through the cut parks HERE, before it runs a single guest instruction.
inline void importLeave(const char* importName, const char* prev) {
  gate().import.store(prev, std::memory_order_release);
  if (g_threadCut.load(std::memory_order_relaxed)) parkAfterImport(importName);
}
// The lock thunks: an acquire counts BEFORE its import (a thread about to hold passes both gates), a release
// uncounts BEFORE its import (its entry passes as a release, its exit parks the thread once it holds nothing).
// Written only by the owning thread: a load and a store, no RMW.
inline void lockAcquiring() {
  GateSlot& g = gate();
  g.locks.store(g.locks.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}
inline void lockReleasing() {
  GateSlot& g = gate();
  const int n = g.locks.load(std::memory_order_relaxed);
  if (n > 0) g.locks.store(n - 1, std::memory_order_release);
}
int heldLocks();  // the calling thread's

// true = this import releases a guest lock / lowers IRQL (passes the entry gate; its exit gate parks the thread).
bool releasesLock(const char* importName);
// true = KfAcquireSpinLock / KeAcquireSpinLockAtRaisedIrql / RtlEnterCriticalSection: a thread spinning in one for a
// lock a parked thread holds is acknowledged (it can never get the lock; if it did, the exit gate would park it).
bool isLockAcquire(const char* importName);

// A host loop that runs guest code on its own clock (VD_ISR, the audio worker), holding nothing, right before its
// guest call: parks forever once g_threadCut is set.
void safePoint(const char* who);
// A loop the CP drain may depend on (the CP, VD_ISR, the audio worker): its doorbell is never parked before the thread
// cut, and its own drain point / safe point is where it is meant to stop. (The import gates arm only at the thread
// cut, after the drain, and then park it like any thread.)
void markExempt(const char* who);
bool exempt();
// GuestCaller (kernel/guest_call.hpp): a host thread that calls guest functions from a host loop. Between calls it
// is acknowledged (host-idle); its next call parks at guestCallEnter once the thread cut is set.
void markHostLoop();
void parkAtGuestCall();
inline void guestCallEnter() {
  if (g_threadCut.load(std::memory_order_relaxed)) parkAtGuestCall();
  GateSlot& g = gate();
  g.guestCalls.store(g.guestCalls.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}
inline void guestCallLeave() {
  GateSlot& g = gate();
  const int n = g.guestCalls.load(std::memory_order_relaxed);
  if (n > 0) g.guestCalls.store(n - 1, std::memory_order_release);
}

// ── The XDK's GPU waits (native/quit-drain; kernel/gpu_hang_wd.cpp's strong-defs) ─────────────────────────────
// GpuBlockOnFence 820FD9A8, GpuRingWaitForSpace 820FCB30 and GpuRingWaitReadPtrWrap 820FD150 spin in guest code
// (GpuRingWaitProgress 820F33E8: no import, so no gate; its hang trap counts KTHREAD+0x58, which never advances here -
// NATIVE_ROADMAP P2.10) on state ONLY the CP writes - the fence block, the read-pointer write-back. A lock holder's
// publish the ring cut dropped (doorbellCut, before the thread cut) carries a fence the parked CP never writes: a later
// wait on it spins forever (memres1/mrc2, s3check2 rel_gate span / seed03 freeze: FAILED threads-not-quiescent, the
// render thread "running guest code" with 3 locks). The waits mark the thread's slot; the settle acknowledges a thread
// STRANDED in GpuBlockOnFence only with every condition of kernel/quit.cpp strandedWait (the fence one the cut DROPPED,
// the wait in its spin loop since after the cut, the loop still turning, its exits closed, the CP parked) - and,
// native/quit-ringspace, a thread STRANDED in GpuRingWaitForSpace on ring space only a dropped publish would have
// freed (the same conditions, with the wait's own test and the dropped progress words). GpuRingWaitReadPtrWrap is
// marked (an unacknowledged thread is named with its wait) but never stranded.
//
// GpuBlockOnFence's phases (IDA 820FD9A8): PRE - the fence test, the open-submission early return (F == next with
// dev+0x3290 set) and the preliminary flush (GpuCommandBufferFlush: a publish, maybe a nested ring-space wait); SPIN -
// `while (fence ahead && GpuRingWaitProgress(rec))`, the ONE call site of 820F33E8 in it (return address 0x820FDA44),
// promoted on its first call (gpuSpinTick) with a fresh spin sequence number; EXITED - GpuRingWaitProgress returned 0
// (the no-GPU mode, dev+0x2A39 & 2, or its hang trap): the loop ends without the fence, never stranded.
//
// GpuRingWaitForSpace's phases (IDA 820FCB30, dev r3, end r4, generation r5): PRE - its space test on the progress word
// P = *(fb+4) (ringSpaceFree; true: it returns at once, no call); SPIN - `do { if (!GpuRingWaitProgress(rec)) break; }
// while (!ringSpaceFree(P, end, gen))`, the ONE call site of 820F33E8 in it; EXITED - that call returned 0. P is
// written only by the CP executing the first EVENT_WRITE_SHD of a segment's fence tail (GpuBuildFenceWritePacket
// 820FD580: {3, mangled(fb+4)|2, (gen & 3) | cursor}; the native close's kRkFence record the same) - or, in the no-GPU
// mode, by the CPU (closed by the same dev+0x2A39 & 2 test).
//
// WHICH CALL IS THE SPIN (gpuSpinSite): both waits build a 0x90-byte frame and pass GpuRingWaitProgress the record at
// r1+0x50 - the caller's r1 - kWaitRecBelowSp (IDA 820FD9A8 / 820FCB30: stwu r1,-0x90; addi r3,r1,0x50), stored in
// the mark at entry. GpuBlockOnFence is generated ctx-style (tools/regen/superman_regen.toml keep_ctx_style), so its
// call also sets lr = kFenceSpinReturn: its spin call = that return address AND its own record (a stale lr left by
// localized code can never promote a PRE wait through a foreign record). GpuRingWaitForSpace is LOCALIZED (skip_lr: its
// call leaves ctx.lr stale - the hang dump's callerLR 8271EE0C for it in s3m_gate): its spin call = its own record
// alone. That is exact: inside its mark the thread runs only the generated 820FCB30 body, whose callees are 820F33E8
// (the one site, r3 = that record) and, after the loop, GpuTimingEventReport 820F3328 (no 820F33E8 call; its optional
// callback dev+13068 is never given the record).
enum GpuWaitKind : uint32_t { kGpuWaitNone = 0, kGpuWaitFence = 1, kGpuWaitRingSpace = 2, kGpuWaitReadPtrWrap = 3 };
enum GpuWaitPhase : uint32_t { kGpuPhasePre = 0, kGpuPhaseSpin = 1, kGpuPhaseExited = 2 };
inline constexpr uint32_t kFenceSpinReturn = 0x820FDA44u;  // GpuBlockOnFence's call of GpuRingWaitProgress returns here
inline constexpr uint32_t kWaitRecBelowSp = 0x40u;         // the waits' record: the caller's r1 - 0x90 + 0x50
inline std::atomic<uint64_t> g_gpuWaitSeqGen{0};           // every residence / spin start takes the next value
inline uint64_t nextGpuWaitSeq() { return g_gpuWaitSeqGen.fetch_add(1, std::memory_order_relaxed) + 1u; }
// GpuRingWaitForSpace's own test (IDA 820FCB30: subf + clrlwi 30, cmplw / ble): the progress word `progress` frees
// [.., end) of generation `gen` - the GPU is in `gen` already, or in the one before it and past `end`.
inline bool ringSpaceFree(uint32_t progress, uint32_t end, uint32_t gen) {
  const uint32_t d = (gen - progress) & 3u;
  return d == 0u || (d == 1u && end <= (progress & ~3u));
}
struct GpuWaitMark {  // the enclosing wait (gpuWaitLeave resumes its kind / dev / target as a new residence)
  uint32_t kind = 0, dev = 0, arg = 0, phase = 0, gen = 0, rec = 0;
  uint64_t seq = 0, spin = 0;
};
// The owner's seqlock write of the mark (kind, dev, arg, gen, rec, phase, seq, spin seq): odd version while it writes.
inline void gpuWaitWrite(GateSlot& g, uint32_t kind, uint32_t dev, uint32_t arg, uint32_t gen, uint32_t rec,
                         uint32_t phase, uint64_t seq, uint64_t spinSeq) {
  const uint32_t v = g.gpuWaitVer.load(std::memory_order_relaxed);
  g.gpuWaitVer.store(v + 1u, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  g.gpuWait.store(kind, std::memory_order_relaxed);
  g.gpuWaitDev.store(dev, std::memory_order_relaxed);
  g.gpuWaitArg.store(arg, std::memory_order_relaxed);
  g.gpuWaitGen.store(gen, std::memory_order_relaxed);
  g.gpuWaitRec.store(rec, std::memory_order_relaxed);
  g.gpuWaitPhase.store(phase, std::memory_order_relaxed);
  g.gpuWaitSeq.store(seq, std::memory_order_relaxed);
  g.gpuSpinSeq.store(spinSeq, std::memory_order_relaxed);
  g.gpuWaitVer.store(v + 2u, std::memory_order_release);
}
// `gen` = GpuRingWaitForSpace's generation (r5; 0 for the others), `rec` = the wait's record (caller's r1 -
// kWaitRecBelowSp; 0: none - such a wait's spin is never recognised).
inline GpuWaitMark gpuWaitEnter(uint32_t kind, uint32_t dev, uint32_t arg, uint32_t gen = 0u, uint32_t rec = 0u) {
  GateSlot& g = gate();
  GpuWaitMark prev;
  prev.kind = g.gpuWait.load(std::memory_order_relaxed);
  prev.dev = g.gpuWaitDev.load(std::memory_order_relaxed);
  prev.arg = g.gpuWaitArg.load(std::memory_order_relaxed);
  prev.gen = g.gpuWaitGen.load(std::memory_order_relaxed);
  prev.rec = g.gpuWaitRec.load(std::memory_order_relaxed);
  prev.phase = g.gpuWaitPhase.load(std::memory_order_relaxed);
  prev.seq = g.gpuWaitSeq.load(std::memory_order_relaxed);
  prev.spin = g.gpuSpinSeq.load(std::memory_order_relaxed);
  gpuWaitWrite(g, kind, dev, arg, gen, rec, kGpuPhasePre, nextGpuWaitSeq(), 0u);
  return prev;
}
// Leaving: the enclosing wait (a flush inside BlockOnFence waits for ring space) resumes as a NEW residence - a fresh
// sequence number, back in its PRE phase - so an unchanged number always means an uninterrupted residence.
inline void gpuWaitLeave(const GpuWaitMark& prev) {
  GateSlot& g = gate();
  gpuWaitWrite(g, prev.kind, prev.dev, prev.arg, prev.gen, prev.rec, kGpuPhasePre, prev.kind ? nextGpuWaitSeq() : 0u, 0u);
}
// kernel/gpu_hang_wd.cpp's GpuRingWaitProgress strong-def, before the body: is this call (return address `lr`, record
// `rec` = r3) the spin loop of the wait this thread is innermost in? The wait kind, or 0 (any other call: the PRE
// phase's nested callers, GpuWaitTilesComplete, a ReadPtrWrap wait - which has no stranded rule - or no wait).
inline uint32_t gpuSpinSite(uint32_t lr, uint32_t rec) {
  const GateSlot* g = t_gate;
  if (!g) return 0u;  // no slot yet: in no wait (gpuWaitEnter claims one)
  const uint32_t kind = g->gpuWait.load(std::memory_order_relaxed);
  if (kind == kGpuWaitFence && lr == kFenceSpinReturn && rec == g->gpuWaitRec.load(std::memory_order_relaxed)) return kind;
  if (kind == kGpuWaitRingSpace) return rec == g->gpuWaitRec.load(std::memory_order_relaxed) ? kind : 0u;
  return 0u;
}
// The spin call (gpuSpinSite non-zero): `dev` = the wait record's device (its first word). The first call of the wait
// this thread is in promotes it to SPIN (a fresh spin sequence number); every call counts.
inline void gpuSpinTick(uint32_t dev) {
  GateSlot& g = gate();
  const uint32_t kind = g.gpuWait.load(std::memory_order_relaxed);
  if ((kind != kGpuWaitFence && kind != kGpuWaitRingSpace) || g.gpuWaitDev.load(std::memory_order_relaxed) != dev)
    return;
  const uint32_t ph = g.gpuWaitPhase.load(std::memory_order_relaxed);
  if (ph == kGpuPhaseExited) return;
  if (ph == kGpuPhasePre)
    gpuWaitWrite(g, kind, dev, g.gpuWaitArg.load(std::memory_order_relaxed), g.gpuWaitGen.load(std::memory_order_relaxed),
                 g.gpuWaitRec.load(std::memory_order_relaxed), kGpuPhaseSpin,
                 g.gpuWaitSeq.load(std::memory_order_relaxed), nextGpuWaitSeq());
  g.gpuSpins.store(g.gpuSpins.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
}
// The same call returned 0: the spin loop ends without its condition (no-GPU mode or the hang trap) - EXITED for good.
inline void gpuSpinExit() {
  GateSlot& g = gate();
  const uint32_t kind = g.gpuWait.load(std::memory_order_relaxed);
  if ((kind != kGpuWaitFence && kind != kGpuWaitRingSpace) || g.gpuWaitPhase.load(std::memory_order_relaxed) != kGpuPhaseSpin)
    return;
  gpuWaitWrite(g, kind, g.gpuWaitDev.load(std::memory_order_relaxed), g.gpuWaitArg.load(std::memory_order_relaxed),
               g.gpuWaitGen.load(std::memory_order_relaxed), g.gpuWaitRec.load(std::memory_order_relaxed),
               kGpuPhaseExited, g.gpuWaitSeq.load(std::memory_order_relaxed),
               g.gpuSpinSeq.load(std::memory_order_relaxed));
}

// ── The ring cut (kernel/gpu_command_processor.cpp gpuCpDoorbell / gpuCpCutRing) ──────────────────────────────
// The cut is written under the publisher's own mutex and tested under it, so a doorbell either published before the
// cut or never publishes - no in-flight window, no RMW: the no-request path is ONE relaxed load inside a mutex the
// doorbell already takes.
void cutRingUnder(std::mutex& publishMutex);
// A doorbell that found the ring cut (after it released the publish mutex): parks forever, or - before the thread cut,
// an exempt thread or one holding a tracked guest lock - drops this publish and returns (counted). Also returns
// after resetForTest().
void doorbellCut();
template <class Publish>
bool publishUnlessCut(std::mutex& publishMutex, Publish&& publish) {
  {
    std::lock_guard<std::mutex> lk(publishMutex);
    if (!g_ringCut.load(std::memory_order_relaxed)) {
      publish();
      return true;
    }
  }
  doorbellCut();
  return false;  // nothing published: the caller returns
}
uint64_t doorbellDrops();  // doorbells dropped by lock holders / exempt threads before the thread cut

// ── What the ring cut dropped (native/quit-drain round 2) ──────────────────────────────────────────────────────────
// A segment close carries exactly ONE fence: GpuBuildFenceWritePacket 820FD580 writes F = dev+0x2A1C into the segment's
// tail (EVENT_WRITE_SHD {3, mangled(fb)|2, F}) and advances it by 2; the native segmentless close writes the same S as
// a kRkFence record. A dropped close records {dev, F} - recorded only once the ring is cut (the drop branches; nothing
// before the request), and only these fences can be the target of a STRANDED wait. Other publications carry no fence
// (bin select, kick callback, bracket parts, the coherency block, the flush-memory block); the legacy CP_RB_WPTR
// doorbell and chain mode record nothing - a wait on their fences stays FAILED (fail closed).
void recordDroppedFence(uint32_t dev, uint32_t fence);
// native/quit-ringspace: the same close's PROGRESS WORD (the tail's first EVENT_WRITE_SHD {3, mangled(fb+4)|2,
// (gen & 3) | cursor} - what the CP would have written to fb+4, freeing the ring up to that cursor); recorded on the same
// terms (after the cut only, per device, once per value, the same overflow). Only these can free the space of a
// STRANDED GpuRingWaitForSpace; a close whose progress packet does not read as one records none (fail closed).
void recordDroppedProgress(uint32_t dev, uint32_t progress);
// The fence a dropped PM4 segment IB {phys, dwords} carries: its last 4 dwords, the segment starting at dev+0x34C4
// (the close's own segment) - 0 when the IB is not that segment or its tail is not the fence packet (no fence).
// *progress (when given) = the progress word of the packet right before it, 0 when that is not the progress packet.
uint32_t ibTailFence(uint32_t dev, uint32_t phys, uint32_t dwords, uint32_t* progress = nullptr);
inline void recordDroppedIb(uint32_t dev, uint32_t phys, uint32_t dwords) {
  uint32_t progress = 0;
  if (const uint32_t f = ibTailFence(dev, phys, dwords, &progress)) {
    recordDroppedFence(dev, f);
    recordDroppedProgress(dev, progress);  // 0 (no progress packet before the fence): nothing
  }
}
bool droppedFence(uint32_t dev, uint32_t fence);  // was {dev, fence} in a publish the cut dropped
uint32_t droppedFenceCount();                     // distinct {dev, fence} recorded
uint32_t droppedFenceOverflow();                  // fences / progress words not recorded (a full table): FAILED waits
// Some progress word a publish the cut dropped carried frees [.., end) of generation gen (ringSpaceFree): had the CP
// executed that publish, the wait would have ended.
bool droppedProgressFrees(uint32_t dev, uint32_t end, uint32_t gen);
bool droppedProgress(uint32_t dev, uint32_t progress);  // was {dev, progress} recorded (the tests' exact read)
uint32_t droppedProgressCount();                        // distinct {dev, progress} recorded

// ── The CP's drain point (kernel/gpu_command_processor.cpp cpMain) ───────────────────────────────────────────
struct CpDrained {
  uint32_t rptr = 0, wptr = 0, rlPending = 0;
  uint64_t swaps = 0, packets = 0, doorbells = 0;
  uint32_t flushMs = 0;  // cpFlushDraws() at the drain point
};
bool cpDrainRequested();
// Publishes the drained state, then blocks the CP thread forever (returns only after resetForTest()).
void cpParkDrained(const CpDrained& d);

// ── The exit claim (app/main.cpp hostExitNow) ─────────────────────────────────────────────────────────────────
// false = a graceful quit owns the process exit: the caller must not exit (it waits; the quit ends the process).
bool claimExit();
// Step 4 (lane D, Astra 10:2x #6): the window-close exit's quiesce of the presenting path, before the host's final
// drains (app/main.cpp hostFinalDrains) - the ring cut (no doorbell publishes any more) and the CP's drain point (the
// CP consumes what was published, flushes the native executor and parks: no present, no dump, no pipeline create
// after it). The quit's own stages 1-2, without the thread cut / snapshot. Only after claimExit() returned true (a
// graceful quit can then never start). Waits at most `ms`; true = the CP parked drained (or no CP runs).
bool parkPresenterForExit(uint32_t ms);

// ── Counter families (the terminal snapshot) ──────────────────────────────────────────────────────────────────
using Printer = void (*)(const char* why);
// Registers a family printed in the terminal snapshot (in registration order, after the built-in ones).
void addFamily(const char* name, Printer p);
int familyCount();

// ── The coordinator (pure sequence; tools/difftest/quit_test.cpp drives it with fake hooks) ───────────────────
struct Digest {
  static constexpr int kMax = 32;
  int n = 0;
  const char* name[kMax] = {};
  uint64_t value[kMax] = {};
  void add(const char* nm, uint64_t v) {
    if (n < kMax) { name[n] = nm; value[n] = v; ++n; }
  }
};
// The names of the fields that differ (", "-joined into `out`), false when identical.
bool digestDiff(const Digest& a, const Digest& b, char* out, size_t n);

// The snapshot check: `snap` = the lines the terminal snapshot printed, `check` = a later capture-only re-render
// (one message per line). Every integer field `key=N` (N followed by a space, ',', ';', '|', ')' or the end; not a
// time: keys ending in us / Us / ms / Ms / ns / Ns, and *_TIME / *_TIMING lines, are skipped) of each line is
// compared with the same field of the line with the same shape (digits blanked) and occurrence in `check`. A line
// only `check` has is a difference (a family that started printing); a line only `snap` has is not (the one-shot
// tail frame). Returns the number of differing fields (described in `out`); *fields = the fields compared.
int snapshotDiff(const std::string& snap, const std::string& check, char* out, size_t n, int* fields);

enum class Drain { Draining = 0, Parked = 1, NoCp = 2 };
// One stranded thread's identity (the census record; verify compares these, never just the count). GpuBlockOnFence:
// target = its fence F, completed = *fb (the fence the parked CP last wrote), next = dev+0x2A1C. GpuRingWaitForSpace
// (native/quit-ringspace): target = the end of the space (r4), completed = the progress word *(fb+4) the parked CP
// last wrote, next = the generation it reserves in (r5).
struct StrandedId {
  uint32_t tid = 0, kind = 0, dev = 0, target = 0, completed = 0, next = 0;
  uint64_t seq = 0, spin = 0;  // the wait's residence and its spin loop's sequence numbers
  int locks = 0;
  bool operator==(const StrandedId& o) const {
    return tid == o.tid && kind == o.kind && dev == o.dev && target == o.target && completed == o.completed &&
           next == o.next && seq == o.seq && spin == o.spin && locks == o.locks;
  }
};
// "tid=.. wait=.. dev=.. target=.. completed=.. next=.. seq=.. spin=.. locks=.." (the [QUIT] stranded line's body)
void formatStranded(const StrandedId& s, char* out, size_t n);
struct ThreadSummary {
  int threads = 0;    // examined: registered guest threads + exempt loops + host loops (alive)
  int parked = 0;     // at a park point of this module
  int waiting = 0;    // blocked inside an import (idle over the window) or spinning in a lock acquire
  int hostIdle = 0;   // a host loop between guest calls
  int stranded = 0;   // in GpuBlockOnFence's spin loop on a fence the ring cut dropped (or GpuRingWaitForSpace's on
                      // space only a dropped publish frees), the same spin over the whole window since after the cut,
                      // its exits closed, the CP parked (native/quit-drain / quit-ringspace strandedWait)
  int unacked = 0;    // anything else (running guest code, busy inside an import, an exempt loop in host code not
                      // yet at its safe point, a thread that appeared during the window)
  int untracked = 0;  // gate slots of threads that are neither registered nor a loop (reported only)
  int holding = 0;    // threads (parked or not) holding a tracked guest lock (reported only)
  uint64_t drops = 0;
  uint32_t droppedFences = 0;    // {dev, fence} the dropped publishes carried (droppedFenceCount)
  uint32_t droppedProgress = 0;  // {dev, progress word} they carried (droppedProgressCount; native/quit-ringspace)
  char firstUnacked[300] = {};
  // EVERY stranded thread's identity, in census order; run() logs each as a "[QUIT] stranded ..." line. More than
  // kStrandedMax (never seen: one render thread) sets strandedOverflow and the settle FAILS - never a silent cap.
  static constexpr int kStrandedMax = 64;
  StrandedId strandedId[kStrandedMax] = {};
  bool strandedOverflow = false;
};
// Same stranded set (count, overflow, and every identity - as a set, by tid); `why` names the first difference.
bool sameStranded(const ThreadSummary& a, const ThreadSummary& b, char* why, size_t n);
struct FrameInfo {
  uint64_t frames = 0, swaps = 0, vdswaps = 0;
  int tail = 0;
};
struct Config {
  uint32_t drainMs = 10000, settleMs = 200, settleMaxMs = 3000, stepMs = 50;
  // native/quit-drain: a STRANDED thread (ThreadSummary::stranded) counts as acknowledged; false (SR_QUIT_STRANDED=0)
  // = it does not - the quit then FAILS threads-not-quiescent, as before the stranded state existed.
  bool allowStranded = true;
  // native/quit-ringspace: a DIAGNOSTIC (SR_QUIT_DIAG_CUT_DELAY_MS, default 0 = off, at most 5000): the thread cut held
  // back this long after the CP drain, so a guest-lock holder runs on dropping its publishes against the parked CP -
  // the stranded states (a fence wait on a dropped fence, a ring-space wait lapping the frozen progress word) provoked
  // on demand. Only the guest's own work after the ring cut grows; nothing is forced. Not for evidence runs: guest-side
  // counters (vdswaps) keep moving until the thread cut.
  uint32_t cutDelayMs = 0;
};
struct Hooks {
  void* user = nullptr;
  uint64_t (*nowMs)(void* u) = nullptr;
  void (*sleepMs)(void* u, uint32_t ms) = nullptr;
  void (*log)(void* u, bool error, const char* line) = nullptr;  // one [QUIT] line (already tagged)
  bool (*claim)(void* u) = nullptr;           // RUNNING -> QUITTING; false = the game is already exiting
  void (*ack)(void* u) = nullptr;             // set the ack event
  void (*cutRing)(void* u) = nullptr;         // returns once no doorbell can publish
  void (*requestDrain)(void* u) = nullptr;
  Drain (*drainState)(void* u, CpDrained* out) = nullptr;
  void (*cutThreads)(void* u) = nullptr;
  void (*digest)(void* u, Digest* out) = nullptr;
  void (*threadsBegin)(void* u) = nullptr;    // start the CPU-time window of the thread census
  ThreadSummary (*threadsCheck)(void* u, bool logRows) = nullptr;  // classify every thread (logRows: one line each)
  FrameInfo (*frames)(void* u) = nullptr;
  // Prints every family with `why`; each line is also appended to *capture (one message per line); suppress = the
  // lines are captured only, never logged. Returns the families printed.
  int (*printFamilies)(void* u, const char* why, std::string* capture, bool suppress) = nullptr;
  void (*exit)(void* u, int code) = nullptr;  // production: never returns
  Config cfg;
};
struct Result {
  int code = -1;          // the exit code passed to exit(); -1 = not claimed (the game was already exiting)
  const char* stage = ""; // where it ended ("complete" or the failing stage)
  const char* reason = "";
};
Result run(const Hooks& h);
Hooks productionHooks();

// ── Test seams (difftest only) ────────────────────────────────────────────────────────────────────────────────
// Releases every thread parked by this module and clears the cut flags (parking blocks on an event production
// never sets).
void resetForTest();
// Forgets every gate slot (call with no test thread alive): an exited thread's slot - an exempt loop's role - must not
// be met under a thread id the OS reused (native/quit-drain's tests run after countersTest's loops exited).
void resetGatesForTest();
uint32_t parkedCount();
bool isParked(uint32_t tid);
// Creates the channel for (instanceId, pid) without the automation policy and starts a watcher that calls
// `onRequest(user)` instead of run(productionHooks()); returns false when the events cannot be created.
bool openChannelForTest(const char* instanceId, uint32_t pid, void (*onRequest)(void*), void* user);
void closeChannelForTest();
// Sets the ack event of the open channel (what run()'s ack hook does in production).
void setAckForTest();
// Puts the process state in QUITTING without the deadline thread (claimExit's other arm).
void setQuittingForTest();

}  // namespace sr::kernel::quit
