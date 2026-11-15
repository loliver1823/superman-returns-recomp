#pragma once
// iter-463 GPU_HANG_WD: a wall-clock watchdog for GPU-progress hangs.
//
// WHY. The Kryptonian-arena freeze (~30 min in) was a GPU hang that no
// existing gauge named: a physical grant spilled below 0xA0000000, its 0x8
// alias double-booked the D3D command buffer's physical pages, the IB read
// took the contested 0x8 bytes, the fence EVENT_WRITE_SHD never ran and
// D3DDeviceSwap spun forever in GpuBlockOnFence_sub_820FD9A8. The CP-phase
// stall check could not see it: the CP was IDLE with nothing queued, which it
// treated as "guest-side quiet".
//
// WHAT. A dedicated host thread (not the vblank ISR, the CP or the pump),
// started at the first doorbell, ticks every 250 ms and evaluates four
// triggers over a snapshot (gpuHangEvaluate, a pure function):
//   H0  a BlockOnFence waiter for >= 1 s while the CP is idle, rptr == wptr and
//       the waited fence is still ahead of the completed one - on a correct
//       platform BlockOnFence kicks its own segment, so this cannot last;
//   H1  the completed fence flat for >= 3 s while a later one is issued;
//   H2  presents flat for >= 10 s (after the first present) while a guest
//       thread has sat in a GPU wait for >= 1 s of consecutive ticks;
//   H3  the old CP-phase stall (moved here from gpuCpVblank): a non-idle phase
//       for > 5 s, or idle with queued ring data or a waiter slot >= 1 s old.
// render/boot-attach: with the CP blocked on the host (phase 5) only H3 is set
// aside (H0-H2 fire, annotated with the wait), and a host wait that lasts
// 60 s, then every 5 min, gets the same block as a [HOST_WAIT_STALL] (what
// the CP waits for, the backend init's step and thread, the shader loader's
// progress and thread) - never a [GPU_HANG] trigger, never a minidump. Both
// blocks keep literal tags (milestones: uncapped in a player log).
// On fire: one [GPU_HANG] block (timing, the waits, both windows of the fence
// block, device fields, CP state + the 64-entry IB ring, allocator, threads,
// CRITSEC holders), once per episode (H0/H1 keyed by the completed fence
// value, H2/H3 alone by the present count) and again at +30 s and +120 s; at
// most 2 dumps per process - the first H0/H1/H2 (never H3 alone) and the
// first GPU-physical double-book (I1): a .bin sidecar of the guest ranges
// (archive/gpu_hang_<pid>_<n>.bin, copied at the trigger) and, only with
// SR_GPU_HANG_DUMP=1, a minidump from a PssCaptureSnapshot clone (.dmp; default
// OFF since app/log-async: the clone paused the whole process ~4.2 s and failed in
// tcmp1 census0 - never on a timing or proof roll). Both files are written by a
// one-shot helper thread, never the watchdog's own (docs/TOOLKIT.md 3.2.5).
//
// Gate SR_GPU_HANG_WD (default ON; =0 restores the vblank-thread stall check).
// OBSERVE-ONLY: the watchdog never writes guest memory, never calls guest
// code, reads guest memory only through hostRangeReadable-checked pointers and
// takes no lock it could wait on (atomics and try_lock; lock-taking queries run
// on a helper thread it abandons after a bounded wait).
#include <cstddef>
#include <cstdint>

namespace sr::kernel {

bool gpuHangWdEnabled();
// Idempotent; gpuCpDoorbell calls it on the first kick.
void gpuHangWdStart();

// ── Latches at the publication sites ─────────────────────────────────────────
// (1) VdSwap: the D3D device (global 0x82000948 -> ptr -> dev).
void gpuHangWdNoteDevice(uint32_t dev);
// (2) VdEnableRingBufferRPtrWriteBack: rptrWb = fence block + 0x3C
// (D3DDeviceInitRingBuffer_sub_820FDE90), so fenceP = phys - 0x3C.
void gpuHangWdNoteRptrWriteback(uint32_t phys);
// (4) The CP's memory writes (0x58 EVENT_WRITE_SHD / 0x3D MEM_WRITE /
// 0x3E REG_TO_MEM): the cheap pre-check, then the latch for fence-word hits.
// `dst` is the host byte the CP actually wrote (nullptr = dropped).
bool gpuHangWdIsFence(uint32_t addr);
void gpuHangWdNoteFenceWrite(uint32_t op, uint32_t addr, uint32_t value, const uint8_t* dst);
// (5) One ring IB, BEFORE it executes: the window it is read from and the host
// pointer of its first byte through that window.
void gpuHangWdNoteIb(uint32_t ibPhys, uint32_t dwords, uint32_t ibWin, const uint8_t* host);
// I1 sink for [PHYS_DOUBLE_BOOK] (installed by gpuHangWdStart through
// physwin::setPhysDoubleBookHook; the tick also polls physDoubleBookCount):
// counts the event and asks the watchdog thread for the dump. Lock-free.
void gpuHangWdNotePhysDoubleBook(uint32_t va, uint32_t otherVa);

// ── (3) The guest waits, from the observe strong-defs ────────────────────────
enum GpuWaitKind : uint32_t {
  kGwBlockOnFence = 1,   // GpuBlockOnFence_sub_820FD9A8(dev, target, mode)
  kGwRingSpace = 2,      // GpuRingWaitForSpace_sub_820FCB30(dev, rptrThreshold, token)
  kGwReadPtrWrap = 3,    // GpuRingWaitReadPtrWrap_sub_820FD150(dev, wptr, count)
};
// Returns the slot (or -1: gate off / table full); pass it to Leave.
int gpuHangWdWaitEnter(uint32_t kind, uint32_t dev, uint32_t a, uint32_t b, uint32_t callerLr,
                       uint32_t r1);
void gpuHangWdWaitLeave(int slot);

// ── (f) The allocator's view, supplied by guest_context.cpp ──────────────────
// The LIFO grant table is private to guest_context.cpp; its owner registers a
// provider. CONTRACT: the provider must not block (try_lock the allocator lock
// and return false when it is busy) and must not fault.
struct GpuHangAllocInfo {
  uint64_t physNext = 0;                // bytes consumed from the top
  uint32_t lowestPhysGrantVA = 0;       // lowest LIVE phys grant (0 = none)
  uint32_t belowACount = 0;             // live phys grants with VA < 0xA0000000
  uint64_t belowABytes = 0;
  struct Grant { uint32_t va, size, lr; } belowA[8] = {};
  uint32_t belowAListed = 0;
  uint32_t lifoRetainedCount = 0;       // LIFO state 1: freed but not reclaimed
  uint64_t lifoRetainedBytes = 0;
  int aliasHoles = 0, aliasHoleMax = 0;
};
using GpuHangAllocProvider = bool (*)(GpuHangAllocInfo* out);
void setGpuHangAllocProvider(GpuHangAllocProvider fn);

// kernel/stubs.cpp: every guest critical section with a live owner (the s_csMap
// under try_lock, the lock-free out-of-range table always). false = the map
// lock was busy (the OOR table is still listed).
bool dumpCsHeldTable(const char* tag);

// ── The trigger logic (pure; tools/difftest/gpu_hang_wd_test.cpp) ────────────
struct GpuHangSnap {
  uint64_t nowNs = 0;
  bool cpStarted = false;
  uint32_t cpPhase = 0;            // 0 idle, 1 ring packet, 2 IB, 3 swap, 4 ISR, 5 host wait (no trigger)
  uint64_t cpPhaseSinceNs = 0;
  uint32_t rptr = 0, wptr = 0;
  bool published = false;
  bool fenceValid = false;         // completed read from 0xA0000000|fenceP
  uint32_t completed = 0;
  uint64_t completedSinceNs = 0;   // when `completed` last changed
  bool issuedValid = false;
  uint32_t lastIssued = 0;         // *(dev+0x2A1C) - 2
  bool fenceWaiter = false;        // the oldest occupied BlockOnFence slot
  uint64_t fenceWaiterEntryNs = 0;
  uint32_t fenceWaiterTarget = 0;
  bool anyWaiter = false;          // the oldest occupied slot of any kind
  uint64_t anyWaiterEntryNs = 0;
  uint64_t presents = 0;           // VdSwap count + CP XE_SWAP count
  uint64_t presentsSinceNs = 0;    // when `presents` last changed
  bool threadInGpuWait = false;    // a guest LR in gpuHangLrInGpuWait, or a slot
  // native/quit-ringspace: the graceful quit has cut the ring (kernel/quit.hpp g_ringCut). From the cut on the quit
  // parks the CP by design - the fence goes flat, a stranded waiter stays - so the watchdog STANDS DOWN: no trigger
  // (gpuHangEvaluate returns 0), no host-wait report, no dump. A hang before the request is judged as before.
  bool quitCut = false;
  // step 4 2k(4) (SR_GPU_HANG_HOST_WORK=1, kernel/host_work.hpp): the oldest open host-work scope on a presenting
  // thread (the CP or the PM4 render thread) - a BMP write, a host shader / pipeline compile, the first frame's
  // setup. hostWork false = none (always, with the gate off).
  bool hostWork = false;
  uint32_t hostWorkKind = 0;
  bool hostWorkOnCp = false;     // the scope is on the CP's own thread
  uint32_t hostWorkTid = 0;
  uint64_t hostWorkSinceNs = 0;
  // Astra P2 4 (causality): the host's own GPU queue has no work outstanding (its frame fence's completed value has
  // reached the last signaled one) or its completed value moved since the previous tick. false = unknown or stuck:
  // a host-work scope then explains nothing (the GPU may really be hung under it).
  bool gpuCaughtUp = false;
};
enum : uint32_t { kGhH0 = 1u, kGhH1 = 2u, kGhH2 = 4u, kGhH3 = 8u };
constexpr uint64_t kGhH0Ns = 1000000000ull, kGhH1Ns = 3000000000ull,
                   kGhH2Ns = 10000000000ull, kGhH3Ns = 5000000000ull;
// Bitmask of the triggers that hold.
// render/boot-attach (Astra round 3): attribution is never decided by timing. With the CP in a host wait
// (cpPhase 5, kCpPhaseHostWait: the live backend's attach, WP1, WP2) H0-H2 fire exactly as they would otherwise
// and their [GPU_HANG] report is annotated with the wait (gpuHangHostWaitNote): graders and readers see both
// facts. Only H3, the CP's own phase stall, is set aside while the CP is in that wait; the wait itself escalates
// on its own schedule (gpuHangHostWaitDue) as a [HOST_WAIT_STALL] report.
uint32_t gpuHangEvaluate(const GpuHangSnap& s);
// Step 4 item 2k(4): the triggers of `mask` that host work on the presenting path explains - classified by state,
// never by time (Astra round 3). H1 (the completed fence flat while a later one is issued) when a presenting
// thread is inside host work AND the CP is in phase swap (it handed its frame to the render path, whose fence is
// written after the work) or the work runs on the CP's own thread; H3 (the CP's own phase stall) when the work
// runs on the CP's own thread. Never H0 (a fence the CP never wrote while idle) or H2 (10 s without a present: a
// freeze is a defect whatever holds it). The watchdog logs the explained ones as a [HOST_WORK_STALL] block (no
// [GPU_HANG] trigger, no minidump) and judges the rest as before.
uint32_t gpuHangHostWorkExplains(uint32_t mask, const GpuHangSnap& s);
// The host GPU queue the causality check reads (gfx registers it with its frame fence): true and the two values when
// known. Lock-free (an ID3D12Fence::GetCompletedValue and an atomic).
using GpuQueueProbe = bool (*)(uint64_t* completed, uint64_t* signaled);
void gpuHangSetQueueProbe(GpuQueueProbe probe);
// The annotation a [GPU_HANG] report carries when the CP is in a host wait: which wait, for how long. false (no
// text) outside a host wait.
bool gpuHangHostWaitNote(uint32_t cpPhase, uint32_t hostWait, uint64_t phaseSinceNs, uint64_t nowNs, char* out,
                         size_t n);
// The host-wait escalation schedule: at 60 s of one host wait, then every 5 min while it lasts. `fired` counts
// this wait's escalations (0 at its start); true when the next one is due (several overdue ones count once).
constexpr uint64_t kGhHostWaitFirstNs = 60000000000ull, kGhHostWaitEveryNs = 300000000000ull;
bool gpuHangHostWaitDue(uint64_t waitAgeNs, uint32_t& fired);
// Test seams: a shorter schedule (0, 0 = the default above), and the [HOST_WAIT_STALL] reports made so far.
void gpuHangWdHostWaitScheduleForTest(uint64_t firstNs, uint64_t everyNs);
uint64_t gpuHangWdHostWaitStallsForTest();
// native/quit-ringspace test seams: one watchdog tick (tick(), on a state of its own) from the calling thread, and
// how many times a watchdog stood down for the graceful quit (its one "[QUIT] gpu-hang watchdog stands down" line).
void gpuHangWdTickForTest();
uint64_t gpuHangWdQuitStandDownsForTest();
// render/boot-attach (Astra round 3): a report's lock-taking queries run on ONE helper thread at a time; while the
// previous helper has not returned (a registry / allocator lock held), the next report skips them and says so.
// Test seams: a helper body that replaces the queries (e.g. blocks on a lock the test holds), the helpers started
// and skipped so far, and one report made now (a [GPU_HANG] block for H1, or a [HOST_WAIT_STALL] block).
void gpuHangWdHelperBodyForTest(void (*body)());
void gpuHangWdHelperStatsForTest(uint64_t* started, uint64_t* skipped);
void gpuHangWdReportForTest(bool hostWait);
// Step 4 2k(4) test seam: one [HOST_WORK_STALL] block for H1 (the header the watchdog writes), made now.
void gpuHangWdHostWorkReportForTest();
uint64_t gpuHangWdHostWorkStallsForTest();   // [HOST_WORK_STALL] blocks the watchdog's ticks logged
// app/log-async test seam: one dump (writeDump, as a trigger takes it) from the CALLING thread; returns the ms the
// call took. The files are written by the dump helper thread, whose [GPU_HANG] dump line names its tid.
double gpuHangWdDumpForTest();
// H2 needs to know whether it is worth scanning the threads at all.
bool gpuHangPresentsFlat(const GpuHangSnap& s);
// {0x820F33E8-0x820F3480, 0x820FD9A8-0x820FDA60}: GpuRingWaitProgress and
// GpuBlockOnFence bodies.
bool gpuHangLrInGpuWait(uint32_t lr);
// H2's wait condition must persist: true once `raw` has held continuously for
// >= 1 s (sinceNs = when it started; reset when it does not hold).
bool gpuHangGpuWaitPersists(uint64_t& sinceNs, bool raw, uint64_t nowNs);
// The hang dump is taken for H0/H1/H2 - never for H3 alone (a boot-time CP
// stall must not spend the process's hang dump).
bool gpuHangWantsDump(uint32_t mask);
// The episode key: the completed fence for H0/H1, the present count otherwise.
uint32_t gpuHangEpisodeKey(uint32_t mask, uint32_t completed, uint64_t presents);
// An episode is keyed (gpuHangEpisodeKey): log at its start, then at +30 s and
// +120 s, else stay quiet.
struct GpuHangEpisode {
  bool active = false;
  uint32_t key = 0;
  uint64_t firstNs = 0;
  int relogs = 0;
};
bool gpuHangEpisodeShouldLog(GpuHangEpisode& e, uint32_t key, uint64_t nowNs);

// Test seam: the newest IB-ring record (false = none yet).
struct GpuHangIbForTest {
  uint64_t seq;
  uint32_t phys, dwords, win, used0, other0, readable, n58, fenceHit, fenceVal;
};
bool gpuHangWdLastIbForTest(GpuHangIbForTest* out);
// Test seam: the fence-write latch (false = none yet). win: 0xA, 8, 0xFF dropped.
struct GpuHangFenceWriteForTest {
  uint64_t count;
  uint32_t op, addr, value, win;
};
bool gpuHangWdLastFenceWriteForTest(GpuHangFenceWriteForTest* out);
// Test seam: the waiter table - Enter calls of `kind`, the last Enter's
// arguments (any kind) and the slots occupied now.
struct GpuHangWaitForTest {
  uint64_t enters = 0;
  uint32_t lastKind = 0, dev = 0, a = 0, b = 0;
  int occupied = 0;
};
GpuHangWaitForTest gpuHangWdWaitForTest(uint32_t kind);

}  // namespace sr::kernel
