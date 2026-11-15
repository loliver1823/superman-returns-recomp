// iter-463 GPU_HANG_WD - see kernel/gpu_hang_wd.hpp for the why and the
// triggers. This file holds the latches, the waiter table and its observe
// strong-defs, the watchdog thread, the [GPU_HANG] report and the minidump.
//
// FAULT-FREEDOM. Every guest byte the watchdog (or a latch) reads goes through
// guestVAToHost + hostRangeReadable(Cached) first - the harness VEH makes any
// host AV fatal before SEH runs. Thread contexts are read the way
// dumpGuestThreadsNow reads them (hostRangeReadable on the context first).
//
// DEADLOCK-FREEDOM. The watchdog thread takes no lock it can wait on: the CP
// state comes from atomic mirrors (gpuCpSnapshot - never c.mu), nothing here
// touches s_liveMu, the IB ring / fence latch / waiter table are atomics, the
// CRITSEC table uses try_lock, and the allocator provider's contract is
// try_lock. The minidump is written from a PssCaptureSnapshot clone, so no
// live thread is suspended while dbghelp allocates; app/log-async: it is opt-in
// (SR_GPU_HANG_DUMP=1 - the clone paused the whole process ~4.2 s in tcmp1) and
// every dump file is written by a one-shot helper thread, never the watchdog's
// own (docs/TOOLKIT.md 3.2.5). The few queries that DO take a lock (phys_window's shared registry
// lock behind gpuWindow / gpuGrantFlags / uniqSnapshot, and the live engine's
// physToHost, which calls gpuWindow) run on a one-shot helper thread the
// watchdog waits on for at most 750 ms and then abandons (its result block is
// shared-owned, so an abandoned helper never writes freed memory).
// Logging only enqueues (app/log.cpp's writer thread does the file I/O).
//
// OBSERVE-ONLY. No guest memory write, no guest call, no return value changed.

#include "gpu_hang_wd.hpp"

#include "guest/ppc/ppc_recomp_shared.h"
#include "gpu_submit.hpp"
#include "guest_context.hpp"
#include "host_work.hpp"   // step 4 2k(4): host work on the presenting path (SR_GPU_HANG_HOST_WORK)
#include "phys_lifo.hpp"
#include "phys_window.hpp"
#include "quit.hpp"   // native/quit-drain: the XDK GPU waits mark the thread's quit gate slot
#include "../app/crash_handler.hpp"   // app/log-async: DbgHelpLock (DbgHelp is single-threaded)
#include "../app/log.hpp"
#include "../gfx/null_renderer.hpp"
#include "../gfx/record_engine.hpp"  // C1 stage 1: the CP engine as the record path sees it (no PM4 walker needed)
#include "../gfx/shader_cache.hpp"   // render/boot-attach: the loader's progress in a host-wait stall report

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <DbgHelp.h>
#include <ProcessSnapshot.h>
#include "native_brackets.hpp"
#endif

extern "C" void __imp__sub_820FD9A8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_820FCB30(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_820FD150(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_820F33E8(PPCContext& __restrict ctx, uint8_t* base);

// guest_context.cpp: the all-threads registry (64 slots, 64-byte stride).
extern "C" sr::kernel::GuestThreadSlot g_debugThreadRegistry[64];

namespace sr::kernel {

// ── The pure trigger logic ───────────────────────────────────────────────────

bool gpuHangLrInGpuWait(uint32_t lr) {
  return (lr >= 0x820F33E8u && lr < 0x820F3480u) || (lr >= 0x820FD9A8u && lr < 0x820FDA60u);
}

bool gpuHangPresentsFlat(const GpuHangSnap& s) {
  return s.nowNs >= s.presentsSinceNs && s.nowNs - s.presentsSinceNs >= kGhH2Ns;
}

uint32_t gpuHangEvaluate(const GpuHangSnap& s) {
  // native/quit-ringspace: the graceful quit's ring cut parks the CP on purpose - nothing after it is a hang.
  if (s.quitCut) return 0u;
  uint32_t m = 0;
  const auto age = [&](uint64_t since) { return s.nowNs >= since ? s.nowNs - since : 0ull; };
  // render/boot-attach (Astra round 3): attribution is never decided by timing. H0, H1 and H2 fire as usual in a
  // host wait (the report then carries the wait's context, gpuHangHostWaitNote); only H3, the CP's own phase
  // stall, is set aside while the CP is provably in its host wait (phase 5 - the waiting thread set it).
  const bool hostWait = s.cpPhase == kCpPhaseHostWait;
  const bool cpIdleDrained = s.cpStarted && s.cpPhase == 0 && (!s.published || s.rptr == s.wptr);
  // H0: BlockOnFence kicks its own segment first, so an idle, drained CP under
  // a waiter whose fence is still ahead means the fence write never ran.
  if (s.fenceWaiter && s.fenceValid && age(s.fenceWaiterEntryNs) >= kGhH0Ns && cpIdleDrained &&
      int32_t(s.fenceWaiterTarget - s.completed) > 0)
    m |= kGhH0;
  // H1: the completed fence frozen while a later one is issued.
  if (s.fenceValid && s.issuedValid && age(s.completedSinceNs) >= kGhH1Ns &&
      int32_t(s.lastIssued - s.completed) > 0)
    m |= kGhH1;
  // H2: no present for 10 s while a guest thread waits on the GPU. Armed only
  // after the first present: boot's first VdSwap comes 12-16 s after the first
  // doorbell in every archived natural run, with the runtime's first kicks in
  // between. threadInGpuWait must already have PERSISTED >= 1 s (the tick's
  // gpuHangGpuWaitPersists), never one sampled LR.
  if (s.presents > 0 && gpuHangPresentsFlat(s) && s.threadInGpuWait) m |= kGhH2;
  // H3: the CP-phase stall. Quiet only when idle, drained AND no waiter has
  // been waiting >= 1 s (a waiter that just entered has not kicked yet).
  // cpPhaseSinceNs == 0: the CP has not set its first phase - no age yet (the
  // old gpuCpVblank guard `if (!since) return;`). Set aside in a host wait: the
  // CP's own stall IS the wait, which escalates on its own schedule
  // (gpuHangHostWaitDue, [HOST_WAIT_STALL]).
  if (!hostWait && s.cpStarted && s.cpPhaseSinceNs && age(s.cpPhaseSinceNs) >= kGhH3Ns) {
    const bool waiterStuck = s.anyWaiter && age(s.anyWaiterEntryNs) >= kGhH0Ns;
    if (!(cpIdleDrained && !waiterStuck)) m |= kGhH3;
  }
  return m;
}

uint32_t gpuHangHostWorkExplains(uint32_t mask, const GpuHangSnap& s) {
  if (!s.hostWork || s.quitCut) return 0u;
  // Astra P2 4: causality, not coincidence - the host GPU queue must be idle or progressing (a real GPU hang under an
  // open scope is still a [GPU_HANG], with the scope as context), and one scope explains at most 10 s (H2's bound:
  // before the first present nothing else limits it, and a 10 s freeze is a defect whatever holds it).
  if (!s.gpuCaughtUp) return 0u;
  if (s.nowNs < s.hostWorkSinceNs || s.nowNs - s.hostWorkSinceNs >= kGhH2Ns) return 0u;
  uint32_t e = 0;
  // H1: the frame's fence is written after the work - the CP handed its frame on (phase 3, swap) to the render path
  // that is inside it, or the CP itself is inside it.
  if ((mask & kGhH1) && (s.cpPhase == 3u || s.hostWorkOnCp)) e |= kGhH1;
  // H3: the CP's own phase stall while the CP itself does the work.
  if ((mask & kGhH3) && s.hostWorkOnCp) e |= kGhH3;
  return e;
}

bool gpuHangHostWaitNote(uint32_t cpPhase, uint32_t hostWait, uint64_t phaseSinceNs, uint64_t nowNs, char* out,
                         size_t n) {
  if (cpPhase != kCpPhaseHostWait || !out || !n) return false;
  const double waited = phaseSinceNs && nowNs >= phaseSinceNs ? double(nowNs - phaseSinceNs) * 1e-9 : -1.0;
  snprintf(out, n,
           "during a CP host wait: the CP has waited %.1f s for %s (CP phase 5; its own stall trigger H3 is set "
           "aside for that wait, H0-H2 are not)",
           waited, gpuCpHostWaitName(hostWait));
  return true;
}

bool gpuHangGpuWaitPersists(uint64_t& sinceNs, bool raw, uint64_t nowNs) {
  if (!raw) { sinceNs = 0; return false; }
  if (!sinceNs) sinceNs = nowNs ? nowNs : 1;
  return nowNs >= sinceNs && nowNs - sinceNs >= kGhH0Ns;
}

namespace {
std::atomic<uint64_t> g_hostWaitFirstNs{0}, g_hostWaitEveryNs{0};  // test schedule (0 = the default)
std::atomic<uint64_t> g_hostWaitStalls{0};
std::atomic<uint64_t> g_quitStandDowns{0};  // native/quit-ringspace: watchdog states that stood down for the quit
std::atomic<uint64_t> g_hostWorkStalls{0};  // step 4 2k(4): [HOST_WORK_STALL] blocks logged
std::atomic<GpuQueueProbe> g_queueProbe{nullptr};   // Astra P2 4: the host GPU queue's progress
}  // namespace

bool gpuHangHostWaitDue(uint64_t waitAgeNs, uint32_t& fired) {
  const uint64_t tf = g_hostWaitFirstNs.load(std::memory_order_relaxed);
  const uint64_t te = g_hostWaitEveryNs.load(std::memory_order_relaxed);
  const uint64_t first = tf ? tf : kGhHostWaitFirstNs, every = te ? te : kGhHostWaitEveryNs;
  if (waitAgeNs < first + uint64_t(fired) * every) return false;
  fired = uint32_t((waitAgeNs - first) / every) + 1u;  // several overdue escalations count once
  return true;
}

void gpuHangWdHostWaitScheduleForTest(uint64_t firstNs, uint64_t everyNs) {
  g_hostWaitFirstNs.store(firstNs, std::memory_order_relaxed);
  g_hostWaitEveryNs.store(everyNs, std::memory_order_relaxed);
}

uint64_t gpuHangWdHostWaitStallsForTest() { return g_hostWaitStalls.load(std::memory_order_relaxed); }
uint64_t gpuHangWdQuitStandDownsForTest() { return g_quitStandDowns.load(std::memory_order_relaxed); }

bool gpuHangWantsDump(uint32_t mask) { return (mask & (kGhH0 | kGhH1 | kGhH2)) != 0; }

uint32_t gpuHangEpisodeKey(uint32_t mask, uint32_t completed, uint64_t presents) {
  return (mask & (kGhH0 | kGhH1)) ? completed : uint32_t(presents);
}

bool gpuHangEpisodeShouldLog(GpuHangEpisode& e, uint32_t key, uint64_t nowNs) {
  if (!e.active || e.key != key) {
    e.active = true;
    e.key = key;
    e.firstNs = nowNs;
    e.relogs = 0;
    return true;
  }
  const uint64_t age = nowNs >= e.firstNs ? nowNs - e.firstNs : 0;
  if (e.relogs == 0 && age >= 30000000000ull) { e.relogs = 1; return true; }
  if (e.relogs == 1 && age >= 120000000000ull) { e.relogs = 2; return true; }
  return false;
}

namespace {

uint64_t nowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

uint32_t be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Fault-free guest reads (the cached predicate: hot enough to matter).
bool safeRead32(uint32_t va, uint32_t* out) {
  const uint8_t* p = guestVAToHost(va);
  if (!p || !hostRangeReadableCached(p, 4)) return false;
  *out = be32(p);
  return true;
}
bool safeRead8(uint32_t va, uint8_t* out) {
  const uint8_t* p = guestVAToHost(va);
  if (!p || !hostRangeReadableCached(p, 1)) return false;
  *out = *p;
  return true;
}

// ── latches ──
std::atomic<uint32_t> g_dev{0};
std::atomic<uint32_t> g_fenceP{0};    // physical address of the fence block (0 = unknown)

struct FenceWrite {
  std::atomic<uint32_t> seq{0};       // odd while being written
  std::atomic<uint32_t> op{0}, addr{0}, value{0}, win{0};
  std::atomic<uint64_t> ns{0}, ibSeq{0}, count{0};
};
FenceWrite g_fw;

// The IB ring: slot claimed by fetch_add, `seq` published last (0 = being
// written); a reader keeps a record only if seq reads the same before and after.
constexpr uint32_t kIbRing = 64;
struct IbRec {
  std::atomic<uint64_t> seq{0}, ns{0};
  std::atomic<uint32_t> phys{0}, dwords{0}, win{0};
  std::atomic<int32_t> gpuWin{0}, grantFlags{0}, cbWin{0};
  std::atomic<uint32_t> used[4] = {}, other[4] = {};
  std::atomic<uint32_t> readable{0};   // bit0 used words, bit1 other words, bit2 walked
  std::atomic<uint32_t> n58{0}, fenceHit{0}, fenceVal{0};
};
IbRec g_ib[kIbRing];
std::atomic<uint64_t> g_ibSeq{0};

// ── the waiter table ──
constexpr int kSlots = 16;
std::atomic<uint64_t> g_waitEnters[4];              // per GpuWaitKind (test seam)
std::atomic<uint32_t> g_lastWaitDev{0}, g_lastWaitA{0}, g_lastWaitB{0}, g_lastWaitKind{0};
std::atomic<uint64_t> g_slotsReaped{0};
struct WaitSlot {
  std::atomic<uint32_t> hostTid{0};   // 0 = free
  std::atomic<uint64_t> entryNs{0};   // 0 = not yet complete
  std::atomic<uint32_t> kind{0}, dev{0}, a{0}, b{0}, lr{0}, r1{0}, completedAtEntry{0};
};
WaitSlot g_slots[kSlots];
std::atomic<uint64_t> g_slotOverflow{0};

// ── I1 ──
std::atomic<uint64_t> g_i1Count{0};
std::atomic<uint32_t> g_i1Va{0}, g_i1Other{0};

std::atomic<GpuHangAllocProvider> g_allocProvider{nullptr};

bool fenceValue(uint32_t* out) {
  const uint32_t p = g_fenceP.load(std::memory_order_acquire);
  return p && safeRead32(0xA0000000u | p, out);
}

const char* kindName(uint32_t k) {
  switch (k) {
    case kGwBlockOnFence: return "BlockOnFence";
    case kGwRingSpace: return "RingWaitForSpace";
    case kGwReadPtrWrap: return "RingWaitReadPtrWrap";
  }
  return "?";
}

const char* guestNameForHostTid(uint32_t tid) {
  for (int i = 0; i < 64; ++i)
    if (g_debugThreadRegistry[i].tid == tid)
      return g_debugThreadRegistry[i].name[0] ? g_debugThreadRegistry[i].name : "-";
  return "?";
}

// Walks one IB's packets for 0x58 fence writes (bounded; no reads past the
// readable span).
void walkIb(const uint8_t* host, uint32_t dwords, uint32_t fenceP, uint32_t* n58, uint32_t* hit,
            uint32_t* val) {
  uint32_t i = 0;
  for (uint32_t guard = 0; i < dwords && guard < 65536; ++guard) {
    const uint32_t hdr = be32(host + size_t(i) * 4u);
    uint32_t n = 1;
    switch (hdr >> 30) {
      case 0: n = 2u + ((hdr >> 16) & 0x3FFFu); break;
      case 1: n = 3; break;
      case 2: n = 1; break;
      case 3: {
        n = 2u + ((hdr >> 16) & 0x3FFFu);
        const uint32_t op = (hdr >> 8) & 0x7Fu;
        if (op == 0x58u && i + 3 < dwords) {
          ++*n58;
          const uint32_t addr = be32(host + size_t(i + 2) * 4u) & ~3u;
          if (fenceP && (addr & 0x1FFFFFFFu) == fenceP) {
            *hit = 1;
            *val = be32(host + size_t(i + 3) * 4u);
          }
        }
        break;
      }
    }
    i += n;
  }
}

}  // namespace

bool gpuHangWdEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_GPU_HANG_WD");
    return !e || e[0] != '0';
  }();
  return v;
}

void setGpuHangAllocProvider(GpuHangAllocProvider fn) { g_allocProvider.store(fn); }

void gpuHangWdNoteDevice(uint32_t dev) {
  if (dev) g_dev.store(dev, std::memory_order_relaxed);
}

void gpuHangWdNoteRptrWriteback(uint32_t phys) {
  const uint32_t p = phys & 0x1FFFFFFFu;
  g_fenceP.store(p >= 0x3Cu ? p - 0x3Cu : 0u, std::memory_order_release);
}

bool gpuHangWdIsFence(uint32_t addr) {
  const uint32_t p = g_fenceP.load(std::memory_order_relaxed);
  return p && ((addr & ~3u) & 0x1FFFFFFFu) == p;
}

void gpuHangWdNoteFenceWrite(uint32_t op, uint32_t addr, uint32_t value, const uint8_t* dst) {
  if (!gpuHangWdEnabled() || !gpuHangWdIsFence(addr)) return;
  const uint32_t p = (addr & ~3u) & 0x1FFFFFFFu;
  uint32_t win = 0;
  if (!dst) win = 0xFFu;   // dropped
  else if (dst == guestVAToHost(0xA0000000u | p)) win = 0xA;
  else if (dst == guestVAToHost(0x80000000u | p)) win = 8;
  const uint32_t s = g_fw.seq.load(std::memory_order_relaxed);
  g_fw.seq.store(s + 1, std::memory_order_release);   // odd: writing
  g_fw.op.store(op, std::memory_order_relaxed);
  g_fw.addr.store(addr, std::memory_order_relaxed);
  g_fw.value.store(value, std::memory_order_relaxed);
  g_fw.win.store(win, std::memory_order_relaxed);
  g_fw.ns.store(nowNs(), std::memory_order_relaxed);
  g_fw.ibSeq.store(g_ibSeq.load(std::memory_order_relaxed), std::memory_order_relaxed);
  g_fw.count.fetch_add(1, std::memory_order_relaxed);
  g_fw.seq.store(s + 2, std::memory_order_release);
}

void gpuHangWdNoteIb(uint32_t ibPhys, uint32_t dwords, uint32_t ibWin, const uint8_t* host) {
  if (!gpuHangWdEnabled()) return;
  const uint64_t seq = g_ibSeq.fetch_add(1, std::memory_order_relaxed) + 1;
  IbRec& r = g_ib[(seq - 1) % kIbRing];
  r.seq.store(0, std::memory_order_release);
  const uint32_t p = ibPhys & 0x1FFFFFFFu;
  const uint64_t bytes = uint64_t(dwords) * 4u;
  r.ns.store(nowNs(), std::memory_order_relaxed);
  r.phys.store(ibPhys, std::memory_order_relaxed);
  r.dwords.store(dwords, std::memory_order_relaxed);
  r.win.store(ibWin, std::memory_order_relaxed);
  // The registry lookups and the second walk of every IB cost the command processor ~8% (perf2
  // r_wc_prof: this function 3.5% + its physwin lookups); the ring keeps phys / size / window /
  // time per IB always, the deep fields only with SR_GPU_HANG_WD_DEEP=1 (hang investigations).
  static const bool s_deep = [] {
    const char* e = std::getenv("SR_GPU_HANG_WD_DEEP");
    return e && e[0] == '1';
  }();
  if (!s_deep) {
    r.gpuWin.store(0, std::memory_order_relaxed);
    r.grantFlags.store(0, std::memory_order_relaxed);
    r.cbWin.store(0, std::memory_order_relaxed);
    r.readable.store(0, std::memory_order_relaxed);
    r.n58.store(0, std::memory_order_relaxed);
    r.fenceHit.store(0, std::memory_order_relaxed);
    r.fenceVal.store(0, std::memory_order_relaxed);
    r.seq.store(seq, std::memory_order_release);
    return;
  }
  r.gpuWin.store(physwin::gpuWindow(p, bytes ? bytes : 4u), std::memory_order_relaxed);
  r.grantFlags.store(physwin::gpuGrantFlags(p, uint32_t(bytes ? bytes : 4u)),
                     std::memory_order_relaxed);
  r.cbWin.store(physwin::commandBufferWindow(p, uint32_t(bytes ? bytes : 4u)),
                std::memory_order_relaxed);
  const uint32_t otherWin = ibWin == 0xA0000000u ? 0x80000000u : 0xA0000000u;
  const uint8_t* other = guestVAToHost(otherWin | p);
  const uint32_t n4 = dwords < 4 ? dwords : 4;
  uint32_t readable = 0;
  if (host && hostRangeReadableCached(host, size_t(n4) * 4u)) {
    readable |= 1;
    for (uint32_t i = 0; i < 4; i++) r.used[i].store(i < n4 ? be32(host + i * 4u) : 0u);
  }
  if (other && hostRangeReadableCached(other, size_t(n4) * 4u)) {
    readable |= 2;
    for (uint32_t i = 0; i < 4; i++) r.other[i].store(i < n4 ? be32(other + i * 4u) : 0u);
  }
  uint32_t n58 = 0, hit = 0, val = 0;
  if (host && dwords && hostRangeReadableCached(host, size_t(bytes))) {
    readable |= 4;
    walkIb(host, dwords, g_fenceP.load(std::memory_order_relaxed), &n58, &hit, &val);
  }
  r.readable.store(readable, std::memory_order_relaxed);
  r.n58.store(n58, std::memory_order_relaxed);
  r.fenceHit.store(hit, std::memory_order_relaxed);
  r.fenceVal.store(val, std::memory_order_relaxed);
  r.seq.store(seq, std::memory_order_release);
}

void gpuHangWdNotePhysDoubleBook(uint32_t va, uint32_t otherVa) {
  g_i1Va.store(va, std::memory_order_relaxed);
  g_i1Other.store(otherVa, std::memory_order_relaxed);
  g_i1Count.fetch_add(1, std::memory_order_release);
}

int gpuHangWdWaitEnter(uint32_t kind, uint32_t dev, uint32_t a, uint32_t b, uint32_t callerLr,
                       uint32_t r1) {
  if (!gpuHangWdEnabled()) return -1;
#if defined(_WIN32)
  const uint32_t tid = uint32_t(GetCurrentThreadId());
#else
  const uint32_t tid = 1;
#endif
  if (kind < 4) g_waitEnters[kind].fetch_add(1, std::memory_order_relaxed);
  g_lastWaitKind.store(kind, std::memory_order_relaxed);
  g_lastWaitDev.store(dev, std::memory_order_relaxed);
  g_lastWaitA.store(a, std::memory_order_relaxed);
  g_lastWaitB.store(b, std::memory_order_relaxed);
  for (int i = 0; i < kSlots; ++i) {
    uint32_t expect = 0;
    if (!g_slots[i].hostTid.compare_exchange_strong(expect, tid, std::memory_order_acq_rel))
      continue;
    WaitSlot& s = g_slots[i];
    s.kind.store(kind, std::memory_order_relaxed);
    s.dev.store(dev, std::memory_order_relaxed);
    s.a.store(a, std::memory_order_relaxed);
    s.b.store(b, std::memory_order_relaxed);
    s.lr.store(callerLr, std::memory_order_relaxed);
    s.r1.store(r1, std::memory_order_relaxed);
    uint32_t c = 0;
    s.completedAtEntry.store(kind == kGwBlockOnFence && fenceValue(&c) ? c : 0u,
                             std::memory_order_relaxed);
    s.entryNs.store(nowNs(), std::memory_order_release);
    return i;
  }
  g_slotOverflow.fetch_add(1, std::memory_order_relaxed);
  return -1;
}

void gpuHangWdWaitLeave(int slot) {
  if (slot < 0 || slot >= kSlots) return;
  g_slots[slot].entryNs.store(0, std::memory_order_release);
  g_slots[slot].hostTid.store(0, std::memory_order_release);
}

namespace {

// ── the watchdog ──

struct SlotView {
  uint32_t tid, kind, dev, a, b, lr, r1, completedAtEntry;
  uint64_t entryNs;
};
// Is host thread `tid` gone? (A kernel query - no guest or host memory read.)
bool hostThreadExited(uint32_t tid) {
#if defined(_WIN32)
  HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
  if (!h) return GetLastError() == ERROR_INVALID_PARAMETER;   // no such thread id
  DWORD code = STILL_ACTIVE;
  const bool gone = GetExitCodeThread(h, &code) && code != STILL_ACTIVE;
  CloseHandle(h);
  return gone;
#else
  (void)tid;
  return false;
#endif
}
int readSlots(SlotView* out, uint64_t nowNs) {
  int n = 0;
  for (int i = 0; i < kSlots; ++i) {
    WaitSlot& s = g_slots[i];
    const uint64_t e = s.entryNs.load(std::memory_order_acquire);
    if (!e) continue;
    SlotView v{s.hostTid.load(), s.kind.load(), s.dev.load(), s.a.load(), s.b.load(),
               s.lr.load(), s.r1.load(), s.completedAtEntry.load(), e};
    if (s.entryNs.load(std::memory_order_acquire) != e || !v.tid) continue;   // left meanwhile
    // A slot whose thread died inside the wait (an SEH unwind of a dying
    // worker, a guest longjmp) never reaches Leave: reap it, or it would hold
    // H3 / H2 on for the rest of the run (review nit).
    if (nowNs >= e && nowNs - e >= kGhH0Ns && hostThreadExited(v.tid)) {
      uint64_t expect = e;
      if (s.entryNs.compare_exchange_strong(expect, 0, std::memory_order_acq_rel)) {
        uint32_t t = v.tid;
        s.hostTid.compare_exchange_strong(t, 0, std::memory_order_acq_rel);
        const uint64_t k = g_slotsReaped.fetch_add(1, std::memory_order_relaxed) + 1;
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[GPU_HANG_WD] waiter slot %d reaped: host thread %u exited inside %s "
                    "(entered %.1fs ago; reaped %llu)",
                    i, v.tid, kindName(v.kind), double(nowNs - e) * 1e-9, (unsigned long long)k);
      }
      continue;
    }
    out[n++] = v;
  }
  return n;
}

struct IbView {
  uint64_t seq, ns;
  uint32_t phys, dwords, win;
  int32_t gpuWin, grantFlags, cbWin;
  uint32_t used[4], other[4], readable, n58, fenceHit, fenceVal;
};
// Newest first.
int readIbRing(IbView* out, int max) {
  const uint64_t head = g_ibSeq.load(std::memory_order_acquire);
  int n = 0;
  for (uint64_t k = 0; k < kIbRing && k < head && n < max; ++k) {
    const uint64_t want = head - k;
    const IbRec& r = g_ib[(want - 1) % kIbRing];
    if (r.seq.load(std::memory_order_acquire) != want) continue;
    IbView v{};
    v.seq = want;
    v.ns = r.ns.load(); v.phys = r.phys.load(); v.dwords = r.dwords.load(); v.win = r.win.load();
    v.gpuWin = r.gpuWin.load(); v.grantFlags = r.grantFlags.load(); v.cbWin = r.cbWin.load();
    for (int i = 0; i < 4; i++) { v.used[i] = r.used[i].load(); v.other[i] = r.other[i].load(); }
    v.readable = r.readable.load(); v.n58 = r.n58.load();
    v.fenceHit = r.fenceHit.load(); v.fenceVal = r.fenceVal.load();
    if (r.seq.load(std::memory_order_acquire) != want) continue;   // overwritten meanwhile
    out[n++] = v;
  }
  return n;
}

// Any registered guest thread whose live LR is in a GPU wait (fault-free, as
// dumpGuestThreadsNow). Only called once presents have been flat for 10 s.
bool anyThreadInGpuWait() {
  for (int i = 0; i < 64; ++i) {
    const GuestThreadSlot& s = g_debugThreadRegistry[i];
    void* ctx = s.ctx;
    if (!s.tid || !ctx) continue;
    const char* c = static_cast<const char*>(ctx);
    if (!hostRangeReadable(c + s.lrOff, 4)) continue;
    uint32_t lr = 0;
    std::memcpy(&lr, c + s.lrOff, 4);   // host-endian PPCContext field (low 32 bits)
    if (gpuHangLrInGpuWait(lr)) return true;
  }
  return false;
}

// The lock-taking queries, on a helper thread the watchdog may abandon.
// render/boot-attach (Astra round 3): at most ONE helper is outstanding. A helper blocked on a held registry /
// allocator lock is abandoned after 750 ms but keeps running until the lock is free; while it has not returned,
// the next report skips the queries (`skipped`, said in the report) instead of stacking another blocked thread.
struct HelperOut {
  std::atomic<bool> done{false};
  bool skipped = false;   // the previous helper has not returned: no new one started
  int gpuWin = 0, grantFlags = 0, engWin = 0;
#if SR_ORACLE
  physwin::UniqSnapshot uniq;
#endif
  PhysLifoStats lifo;   // takes the allocator lock
};
std::atomic<bool> g_helperBusy{false};
std::atomic<uint64_t> g_helpersStarted{0}, g_helpersSkipped{0};
std::atomic<void (*)()> g_helperBodyForTest{nullptr};
std::shared_ptr<HelperOut> runHelper(uint32_t fenceP) {
  auto out = std::make_shared<HelperOut>();
  bool idle = false;
  if (!g_helperBusy.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
    out->skipped = true;
    g_helpersSkipped.fetch_add(1, std::memory_order_relaxed);
    return out;
  }
  g_helpersStarted.fetch_add(1, std::memory_order_relaxed);
  std::thread([out, fenceP] {
    if (void (*body)() = g_helperBodyForTest.load(std::memory_order_acquire)) {
      body();  // test: stands in for the queries (blocks while the test holds its lock)
    } else {
      if (fenceP) {
        out->gpuWin = physwin::gpuWindow(fenceP, 96);
        out->grantFlags = physwin::gpuGrantFlags(fenceP, 96);
        auto& eng = sr::gfx::getLiveRecordEngine();
        const uint8_t* h = eng.physToHost ? eng.physToHost(eng.user, fenceP) : nullptr;
        out->engWin = !h ? 0 : h == guestVAToHost(0xA0000000u | fenceP) ? 0xA
                            : h == guestVAToHost(0x80000000u | fenceP) ? 8 : -1;
      }
#if SR_ORACLE
      out->uniq = physwin::uniqSnapshot();
#endif
      out->lifo = physLifoStats();
    }
    out->done.store(true, std::memory_order_release);
    g_helperBusy.store(false, std::memory_order_release);
  }).detach();
  const uint64_t t0 = nowNs();
  while (!out->done.load(std::memory_order_acquire) && nowNs() - t0 < 750000000ull)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return out;
}
// The note a report gives when the helper's results are missing.
const char* helperNote(const HelperOut& h) {
  return h.skipped ? " (helper SKIPPED: the previous one has not returned - a registry/allocator lock is held)"
                   : h.done.load(std::memory_order_acquire) ? ""
                   : " (helper TIMEOUT: a registry/allocator lock is held)";
}

const char* phaseName(uint32_t ph) {
  static const char* k[] = {"idle", "ring-packet", "indirect-buffer", "swap", "isr", "host-wait"};
  return ph < 6 ? k[ph] : "?";
}

struct State {
  uint64_t startNs = 0;
  bool haveCompleted = false;
  uint32_t prevCompleted = 0;
  uint64_t completedSinceNs = 0;
  uint64_t prevPresents = ~0ull;
  uint64_t presentsSinceNs = 0;
  GpuHangEpisode ep;
  uint64_t i1Seen = 0;
  uint64_t gpuWaitSinceNs = 0;   // H2: when the GPU-wait condition started to hold
  uint64_t lastReportNs = 0;
  int dumps = 0;
  bool hDumped = false, i1Dumped = false, crossChecked = false;
  uint64_t hostWaitSinceNs = 0;   // render/boot-attach: the CP host wait being watched (its phase-5 start)
  uint32_t hostWaitFired = 0;     // its [HOST_WAIT_STALL] escalations so far
  bool quitStoodDown = false;     // native/quit-ringspace: the stand-down line logged
  bool haveGpuCompleted = false;   // Astra P2 4: the host queue's completed value at the previous tick
  uint64_t prevGpuCompleted = 0;
  GpuHangEpisode hwEp;            // step 4 2k(4): the [HOST_WORK_STALL] episodes (apart from the [GPU_HANG] ones, so a
                                  // stall that outlives its host work is still logged as a hang)
};

// ── the minidump + sidecar ──
struct Range {
  uint32_t va, win, size, kind;   // kind: 1 dev 2 fence 3 ring 4 ib 5 stack 6 grant
  const uint8_t* host;
};
void addRange(std::vector<Range>& v, uint32_t va, uint32_t size, uint32_t kind) {
  if (!size) return;
  const uint8_t* h = guestVAToHost(va);
  // The cached predicate (review nit): an uncached VirtualQuery over the flat
  // guest region costs ~10 ms (max 42-62 ms, [HRR]); guest memory is never
  // decommitted here, as everywhere else in this file.
  if (!h || !hostRangeReadableCached(h, size)) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[GPU_HANG] dump range va=%08X size=0x%X kind=%u unreadable - skipped",
                va, size, kind);
    return;
  }
  v.push_back(Range{va, va >> 28, size, kind, h});
}

// app/log-async (docs/TOOLKIT.md 3.2.5): one dump's files, handed to the helper thread.
struct DumpJob {
  int n = 0;
  std::string why;
  std::vector<Range> rs;
  std::vector<uint8_t> bytes;   // every range's bytes, copied at the trigger, in rs order
};
void writeDumpFiles(const DumpJob& job);

// SR_GPU_HANG_DUMP=1: the PssCaptureSnapshot minidump (default OFF - the VA clone pauses the whole
// process for seconds; never set it on a timing or proof roll). The .bin sidecar is always written.
bool gpuHangDumpEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_GPU_HANG_DUMP");
    return e && e[0] == '1';
  }();
  return v;
}

#if defined(_WIN32)
struct CbCtx {
  const std::vector<Range>* r;
  size_t next;
};
BOOL CALLBACK dumpCallback(PVOID param, const PMINIDUMP_CALLBACK_INPUT in,
                           PMINIDUMP_CALLBACK_OUTPUT out) {
  CbCtx* c = static_cast<CbCtx*>(param);
  if (!in || !out) return FALSE;
  switch (in->CallbackType) {
    case IncludeModuleCallback:
    case ModuleCallback:
    case IncludeThreadCallback:
    case ThreadCallback:
    case ThreadExCallback:
      return TRUE;
    case MemoryCallback:
      if (c->next >= c->r->size()) return FALSE;
      out->MemoryBase = reinterpret_cast<ULONG64>((*c->r)[c->next].host);
      out->MemorySize = (*c->r)[c->next].size;
      c->next++;
      return TRUE;
    case CancelCallback:
      out->Cancel = FALSE;
      out->CheckCancel = FALSE;
      return TRUE;
    case ReadMemoryFailureCallback:
      out->Status = S_OK;   // keep going; every range was checked readable first
      return TRUE;
    case IsProcessSnapshotCallback:
      out->Status = S_FALSE;   // the handle IS a PssCaptureSnapshot snapshot
      return TRUE;
    case IoStartCallback:
      out->Status = S_FALSE;   // dbghelp does its own file I/O
      return TRUE;
    case VmStartCallback:
      out->Status = S_FALSE;   // no VM-query callbacks
      return TRUE;
    default:
      return FALSE;
  }
}
#endif

void writeDump(State& st, const char* why, uint32_t dev, uint32_t fenceP, const SlotView* slots,
               int nSlots) {
  if (st.dumps >= 2) return;
  const int n = ++st.dumps;
  std::vector<Range> rs;
  if (dev) addRange(rs, dev, 0x3600, 1);
  if (fenceP) {
    addRange(rs, 0xA0000000u | fenceP, 96, 2);
    addRange(rs, 0x80000000u | fenceP, 96, 2);
  }
  const GpuCpSnapshot cp = gpuCpSnapshot();
  if (cp.ringBase) addRange(rs, cp.ringBase, 0x10000, 3);
  IbView ibs[8];
  const int nIb = readIbRing(ibs, 8);
  for (int i = 0; i < nIb; i++) {
    const uint32_t p = ibs[i].phys & 0x1FFFFFFFu;
    uint32_t sz = ibs[i].dwords * 4u;
    if (sz > 0x10000u) sz = 0x10000u;
    addRange(rs, 0xA0000000u | p, sz, 4);
    addRange(rs, 0x80000000u | p, sz, 4);
  }
  int stacks = 0;
  for (int i = 0; i < nSlots && stacks < 4; i++) {
    if (slots[i].r1 < 0x400u) continue;
    addRange(rs, slots[i].r1 - 0x400u, 0x1400u, 5);
    stacks++;
  }
  GpuHangAllocInfo ai;
  if (auto prov = g_allocProvider.load(); prov && prov(&ai)) {
    for (uint32_t i = 0; i < ai.belowAListed && i < 8; i++)
      addRange(rs, ai.belowA[i].va, ai.belowA[i].size < 0x10000u ? ai.belowA[i].size : 0x10000u, 6);
  } else {
#if SR_ORACLE
    // The provider (physLifoHangAllocInfo) only TRIES the allocator lock; when
    // it is busy (or the LIFO path is off) the lowest live grant from the
    // registry still goes in (uniqSnapshot, on the helper thread).
    auto h = runHelper(0);
    if (h->done.load(std::memory_order_acquire) && h->uniq.lowestPhysGrantVA &&
        h->uniq.lowestPhysGrantVA < 0xA0000000u)
      addRange(rs, h->uniq.lowestPhysGrantVA, 0x10000, 6);
#endif
  }

  // app/log-async (docs/TOOLKIT.md 3.2.5): every file of the dump is written by a one-shot helper
  // thread, never this watchdog thread - a write that waits on the disk would stop the watchdog's
  // clock (no tick while it runs: the flat-for-N-s windows, H2's consecutive ticks and the re-reports
  // would measure the dump). The guest ranges are copied HERE (memory reads only, as before), so the
  // sidecar holds the trigger-time bytes. The minidump itself is opt-in (SR_GPU_HANG_DUMP=1): its
  // PssCaptureSnapshot VA clone froze the whole ~8 GB process ~4.2 s and failed (tcmp1 census0).
  auto job = std::make_shared<DumpJob>();
  job->n = n;
  job->why = why;
  job->rs = rs;
  size_t total = 0;
  for (const Range& r : rs) total += r.size;
  job->bytes.reserve(total);
  for (const Range& r : rs) job->bytes.insert(job->bytes.end(), r.host, r.host + r.size);
  std::thread([job] { writeDumpFiles(*job); }).detach();
}

// The dump's files, on the helper thread writeDump starts (one per dump; at most 2 per process).
void writeDumpFiles(const DumpJob& job) {
#if defined(_WIN32)
  using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  if (HMODULE k = GetModuleHandleW(L"kernel32.dll"))
    if (auto fn = reinterpret_cast<SetDesc>(GetProcAddress(k, "SetThreadDescription")))
      fn(GetCurrentThread(), L"GPU_HANG_DUMP");
#endif
  const int n = job.n;
  const char* why = job.why.c_str();
  const std::vector<Range>& rs = job.rs;
  const uint64_t t0 = nowNs();
  std::error_code ec;
  std::filesystem::create_directories("archive", ec);
#if defined(_WIN32)
  const unsigned long pid = GetCurrentProcessId();
#else
  const unsigned long pid = 0;
#endif
  char dmpPath[128], binPath[128];
  snprintf(dmpPath, sizeof(dmpPath), "archive/gpu_hang_%lu_%d.dmp", pid, n);
  snprintf(binPath, sizeof(binPath), "archive/gpu_hang_%lu_%d.bin", pid, n);

  // Sidecar first (cheap, and survives a dump that never returns):
  // {'GHW1', u32 version 1, u32 count, u32 0}, then per range
  // {u32 va, u32 window (va >> 28), u32 size, u32 kind} + size bytes.
  uint64_t binBytes = 0;
  if (FILE* f = std::fopen(binPath, "wb")) {
    const uint32_t hdr[4] = {0x31574847u /* 'GHW1' LE */, 1u, uint32_t(rs.size()), 0u};
    std::fwrite(hdr, sizeof(hdr), 1, f);
    size_t at = 0;
    for (const Range& r : rs) {
      const uint32_t rh[4] = {r.va, r.win, r.size, r.kind};
      std::fwrite(rh, sizeof(rh), 1, f);
      std::fwrite(job.bytes.data() + at, 1, r.size, f);
      at += r.size;
      binBytes += r.size;
    }
    std::fclose(f);
  }
  if (!gpuHangDumpEnabled()) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[GPU_HANG] dump #%d (%s) -> no minidump (off; SR_GPU_HANG_DUMP=1 enables it - never on a timing "
                "or proof roll: its snapshot pauses the whole process) + %s (%zu ranges, %llu bytes) in %.0f ms "
                "on the dump helper thread tid=%lu",
                n, why, binPath, rs.size(), (unsigned long long)binBytes, double(nowNs() - t0) * 1e-6,
                (unsigned long)GetCurrentThreadId());
    return;
  }
  // The dump is taken from a PssCaptureSnapshot VA clone, never the live
  // process (review): MiniDumpWriteDump(GetCurrentProcess()) suspends every
  // other thread and then allocates - a suspended heap- or loader-lock holder
  // would deadlock the game it observes. The clone suspends nothing in user
  // mode; the callback answers IsProcessSnapshotCallback. No snapshot, no .dmp
  // (the sidecar above already holds the guest ranges).
  const char* how = "no snapshot";
  bool ok = false;
#if defined(_WIN32)
  // app/log-async (Astra round 1): the hang and the double-book triggers start independent helpers; one dump
  // at a time (and never beside another DbgHelp call: the crash filter, the stack probe, the VEH symbolizer).
  // The wait is on this helper thread, never the watchdog's.
  DbgHelpLock dbg(0xFFFFFFFFu);
  HPSS snap = nullptr;
  // iter-463 E3 (archive/iter463_e3_off.log): the first flag set failed with
  // 87 (ERROR_INVALID_PARAMETER) on this process, so fall back through
  // simpler sets and log each refusal.
  const PSS_CAPTURE_FLAGS kFlagSets[] = {
      PSS_CAPTURE_VA_CLONE | PSS_CAPTURE_THREADS | PSS_CAPTURE_THREAD_CONTEXT |
          PSS_CREATE_BREAKAWAY_OPTIONAL | PSS_CREATE_USE_VM_ALLOCATIONS,
      PSS_CAPTURE_VA_CLONE | PSS_CAPTURE_THREADS | PSS_CAPTURE_THREAD_CONTEXT,
      PSS_CAPTURE_VA_CLONE,
  };
#if defined(_M_X64) || defined(__x86_64__)
  const DWORD kCtx[] = {CONTEXT_ALL, CONTEXT_FULL, 0};
#else
  const DWORD kCtx[] = {CONTEXT_FULL, CONTEXT_FULL, 0};
#endif
  DWORD pr = ERROR_INVALID_FUNCTION;
  for (int k = 0; k < 3 && pr != ERROR_SUCCESS; k++) {
    snap = nullptr;
    pr = PssCaptureSnapshot(GetCurrentProcess(), kFlagSets[k], kCtx[k], &snap);
    if (pr != ERROR_SUCCESS)
      SR_LOG_BOOT(sr::LogSeverity::Error, "[GPU_HANG] dump #%d: PssCaptureSnapshot set %d failed (%lu)", n, k,
                  (unsigned long)pr);
  }
  if (pr != ERROR_SUCCESS) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[GPU_HANG] dump #%d: no snapshot - sidecar only", n);
  } else {
    how = "snapshot";
    HANDLE hf = CreateFileA(dmpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
      CbCtx cc{&rs, 0};
      MINIDUMP_CALLBACK_INFORMATION ci{};
      ci.CallbackRoutine = dumpCallback;
      ci.CallbackParam = &cc;
      ok = MiniDumpWriteDump(reinterpret_cast<HANDLE>(snap), GetCurrentProcessId(), hf,
                             (MINIDUMP_TYPE)(MiniDumpWithThreadInfo |
                                             MiniDumpWithIndirectlyReferencedMemory),
                             nullptr, nullptr, &ci) != FALSE;
      CloseHandle(hf);
    }
    PssFreeSnapshot(GetCurrentProcess(), snap);
  }
#endif
  SR_LOG_BOOT(sr::LogSeverity::Error,
              "[GPU_HANG] dump #%d (%s) -> %s (%s, %s) + %s (%zu ranges, %llu bytes) in %.0f ms on the dump helper "
              "thread tid=%lu", n, why, dmpPath, ok ? "written" : "FAILED", how, binPath, rs.size(),
              (unsigned long long)binBytes, double(nowNs() - t0) * 1e-6, (unsigned long)GetCurrentThreadId());
}

#if defined(_WIN32)
// A host thread's CPU time (kernel + user, s), or -1: whether a stuck thread spins or blocks.
double hostThreadCpuS(uint32_t tid) {
  if (!tid) return -1.0;
  HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
  if (!h) return -1.0;
  FILETIME c{}, e{}, k{}, u{};
  const BOOL ok = GetThreadTimes(h, &c, &e, &k, &u);
  CloseHandle(h);
  if (!ok) return -1.0;
  const auto ft = [](const FILETIME& f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
  return ft(k) + ft(u);
}
#else
double hostThreadCpuS(uint32_t) { return -1.0; }
#endif

// ── the [GPU_HANG] block; with hostHeader, the same block as a [HOST_WAIT_STALL] (render/boot-attach) ──
void report(State& st, uint32_t mask, const GpuHangSnap& s, const SlotView* slots, int nSlots,
            uint32_t dev, uint32_t fenceP, const char* hostHeader = nullptr) {
  const auto E = sr::LogSeverity::Error;
  const bool host = hostHeader != nullptr;
  // Astra round 3: every line keeps a LITERAL tag in its format string - app/log.cpp classifies the unformatted
  // string, and both tags are milestones (kept whole in a player log, never capped).
#define HLOG(fmt, ...)                                                          \
  do {                                                                          \
    if (host)                                                                   \
      SR_LOG_BOOT(E, "[HOST_WAIT_STALL] " fmt __VA_OPT__(,) __VA_ARGS__);       \
    else                                                                        \
      SR_LOG_BOOT(E, "[GPU_HANG] " fmt __VA_OPT__(,) __VA_ARGS__);              \
  } while (0)
  const GpuCpSnapshot cp = gpuCpSnapshot();
  const auto secs = [&](uint64_t t) { return t && s.nowNs >= t ? double(s.nowNs - t) * 1e-9 : -1.0; };
  char trig[32];
  snprintf(trig, sizeof(trig), "%s%s%s%s", (mask & kGhH0) ? "H0 " : "", (mask & kGhH1) ? "H1 " : "",
           (mask & kGhH2) ? "H2 " : "", (mask & kGhH3) ? "H3" : "");
  const uint64_t ibHead = g_ibSeq.load();
  IbView last{};
  const bool haveLast = readIbRing(&last, 1) == 1;
  if (hostHeader) {
    // (a) the host wait: what the CP waits for, the backend init's step and thread, the shader loader's progress
    HLOG("==== %s | now=%.3fs completed=%08X flat %.1fs | presents=%llu flat %.1fs", hostHeader,
         double(s.nowNs - st.startNs) * 1e-9, s.completed, secs(s.completedSinceNs),
         (unsigned long long)s.presents, secs(s.presentsSinceNs));
    const sr::gfx::BackendInitState bi = sr::gfx::backendInitState();
    char initState[256];
    if (!bi.step)
      snprintf(initState, sizeof(initState), "not started");
    else
      snprintf(initState, sizeof(initState), "%s '%s', host tid %u, started at process +%.0f ms, thread CPU %.2f s",
               bi.returned ? "returned ready after step" : "in step (or failed in it)", bi.step, bi.tid, bi.startMs,
               hostThreadCpuS(bi.tid));
    HLOG("(h) backend init: %s; live backend %s%s", initState,
         sr::gfx::pm4LiveBackendAttached() ? "attached" : sr::gfx::pm4LiveBackendFailed() ? "FAILED: " : "not attached",
         sr::gfx::pm4LiveBackendFailed() ? sr::gfx::pm4LiveBackendFailReason() : "");
    const sr::gfx::ShaderCache& sc = sr::gfx::getShaderCache();
    if (sc.loaderThreadId())
      HLOG("(h) shader cache: %s, %zu files read, loader host tid %u (thread CPU %.2f s)",
           sc.fullyLoaded() ? "both stages published" : sc.installedLoaded() ? "in its translator stage"
                                                                             : "in its installed stage",
           sc.filesRead(), sc.loaderThreadId(), hostThreadCpuS(sc.loaderThreadId()));
    else
      HLOG("(h) shader cache: no load started yet");
  } else {
  // (a) timing
  HLOG(
              "==== trigger %s| episode key=%08X relog=%d | now=%.3fs completed=%08X "
              "flat %.1fs | presents=%llu flat %.1fs (VdSwap #%llu XE_SWAP #%llu) | last IB "
              "seq=%llu %.3fs ago",
              trig, st.ep.key, st.ep.relogs, double(s.nowNs - st.startNs) * 1e-9, s.completed,
              secs(s.completedSinceNs), (unsigned long long)s.presents, secs(s.presentsSinceNs),
              (unsigned long long)getVdSwapCount(), (unsigned long long)cp.swaps,
              (unsigned long long)ibHead, haveLast ? secs(last.ns) : -1.0);
  // Astra round 3: a trigger that fires while the CP is in a host wait says so (both facts on record).
  char note[256];
  if (gpuHangHostWaitNote(cp.phase, cp.hostWait, cp.phaseSinceNs, s.nowNs, note, sizeof(note)))
    HLOG("(a) %s", note);
  // Astra P2 4: host work open but not taken as the cause - the context, on the record
  if (s.hostWork)
    HLOG("(a) host work %s on host tid %u (%s) open for %.2fs, NOT taken as the cause: %s", hostWorkName(s.hostWorkKind),
         s.hostWorkTid, s.hostWorkOnCp ? "the CP thread" : "the render thread", secs(s.hostWorkSinceNs),
         !s.gpuCaughtUp ? "the host GPU queue has work outstanding and is not progressing (or is unknown)"
                        : "it has been open 10 s or more, or the CP is neither in swap nor the worker");
  }
  // (b) the guest waits
  uint32_t waitVA = 0;
  const bool haveWaitVA = dev && safeRead32(dev + 0x2A10u, &waitVA);
  for (int i = 0; i < nSlots; i++) {
    const SlotView& w = slots[i];
    uint32_t target = w.a;
    if (w.kind == kGwReadPtrWrap) {
      uint32_t ringMask = 0;
      target = safeRead32(w.dev + 13480u, &ringMask) ? (w.a + w.b) & ringMask : 0xFFFFFFFFu;
    }
    HLOG(
                "(b) wait %s tid=%u name=%s dev=%08X target=%08X mode=%08X "
                "callerLR=%08X r1=%08X age=%.2fs completedAtEntry=%08X",
                kindName(w.kind), w.tid, guestNameForHostTid(w.tid), w.dev, target, w.b, w.lr,
                w.r1, secs(w.entryNs), w.completedAtEntry);
  }
  if (!nSlots) HLOG("(b) no waiter slot occupied (overflow=%llu)",
                           (unsigned long long)g_slotOverflow.load());
  HLOG("(b) wait VA *(dev+0x2A10)=%08X phys=%08X fenceP=%08X",
              haveWaitVA ? waitVA : 0u, haveWaitVA ? (waitVA & 0x1FFFFFFFu) : 0u, fenceP);
  // (c) both windows of the fence block
  auto helper = runHelper(fenceP);
  const bool hOk = helper->done.load(std::memory_order_acquire);
  if (fenceP) {
    static const uint32_t kOffs[4] = {0, 4, 8, 0x3C};
    char a[80] = "", b[80] = "";
    int na = 0, nb = 0;
    for (uint32_t o : kOffs) {
      uint32_t va = 0, vb = 0;
      const bool ra = safeRead32((0xA0000000u | fenceP) + o, &va);
      const bool rb = safeRead32((0x80000000u | fenceP) + o, &vb);
      na += snprintf(a + na, sizeof(a) - size_t(na), ra ? " %08X" : " ????????", va);
      nb += snprintf(b + nb, sizeof(b) - size_t(nb), rb ? " %08X" : " ????????", vb);
    }
    HLOG(
                "(c) fence block +0/+4/+8/+3C: 0xA:%s | 0x8:%s | physToHost picks %s | "
                "gpuWindow=%d gpuGrantFlags=%d commandBufferWindow=%d%s",
                a, b,
                !hOk ? "?" : helper->engWin == 0xA ? "0xA" : helper->engWin == 8 ? "0x8"
                           : helper->engWin == 0 ? "null" : "other",
                hOk ? helper->gpuWin : -99, hOk ? helper->grantFlags : -99,
                physwin::commandBufferWindow(fenceP, 96),
                helperNote(*helper));
  } else {
    HLOG("(c) fence block unknown (no rptr write-back registered)");
  }
  // (d) device fields
  if (dev) {
    uint32_t v[7] = {};
    const uint32_t offs[7] = {0x2A08u, 0x2A10u, 0x2A1Cu, 0x34D0u, 10808u, 13476u, 13508u};
    bool ok[7];
    for (int i = 0; i < 7; i++) ok[i] = safeRead32(dev + offs[i], &v[i]);
    uint8_t b39 = 0;
    const bool okb = safeRead8(dev + 0x2A39u, &b39);
    HLOG(
                "(d) dev=%08X +2A08 owner=%08X%s +2A10=%08X +2A1C next=%08X +34D0 "
                "prevFrame=%08X +2A39=%02X%s +10808 flags=%08X +13476 ringBase=%08X +13508 "
                "segStart=%08X",
                dev, v[0], ok[0] ? "" : "?", v[1], v[2], v[3], b39, okb ? "" : "?", v[4], v[5],
                v[6]);
  } else {
    HLOG("(d) device unknown (no VdSwap yet)");
  }
  // (e) CP
  auto& eng = sr::gfx::getLiveRecordEngine();
  const auto rd64 = [](const uint64_t& x) { return *reinterpret_cast<const volatile uint64_t*>(&x); };
  HLOG(
              "(e) CP phase=%s for %.2fs rptr=%u wptr=%u published=%d doorbells=%llu "
              "packets=%llu IBs=%llu swaps=%llu isr=%llu (faults %llu) cpMemWrites=%llu "
              "cpWaitsBlocked=%llu",
              phaseName(cp.phase), secs(cp.phaseSinceNs), cp.rptr, cp.wptr, cp.published ? 1 : 0,
              (unsigned long long)cp.doorbells, (unsigned long long)cp.packets,
              (unsigned long long)ibHead, (unsigned long long)cp.swaps,
              (unsigned long long)cp.isrCalls, (unsigned long long)cp.isrFaults,
              (unsigned long long)rd64(eng.cpMemWrites), (unsigned long long)rd64(eng.cpWaitsBlocked));
  {
    uint32_t s0 = 0, op = 0, addr = 0, val = 0, win = 0;
    uint64_t ns = 0, ibs = 0, cnt = 0;
    for (int tries = 0; tries < 4; ++tries) {
      s0 = g_fw.seq.load(std::memory_order_acquire);
      op = g_fw.op.load(); addr = g_fw.addr.load(); val = g_fw.value.load(); win = g_fw.win.load();
      ns = g_fw.ns.load(); ibs = g_fw.ibSeq.load(); cnt = g_fw.count.load();
      if (!(s0 & 1u) && g_fw.seq.load(std::memory_order_acquire) == s0) break;
    }
    HLOG(
                "(e) lastFenceWrite #%llu op=%02X addr=%08X value=%08X window=%s "
                "%.3fs ago ibSeq=%llu",
                (unsigned long long)cnt, op, addr, val,
                win == 0xA ? "0xA" : win == 8 ? "0x8" : win == 0xFF ? "DROPPED" : cnt ? "other" : "-",
                cnt ? secs(ns) : -1.0, (unsigned long long)ibs);
  }
  IbView ring[kIbRing];
  const int nIb = readIbRing(ring, int(kIbRing));
  for (int i = 0; i < nIb; i++) {
    const IbView& r = ring[i];
    HLOG(
                "(e) ib seq=%llu -%.3fs phys=%08X dw=%u win=%X gpuWin=%d grantFlags=%d "
                "cbWin=%d used=[%08X %08X %08X %08X]%s other=[%08X %08X %08X %08X]%s n58=%u "
                "fence=%s%08X",
                (unsigned long long)r.seq, secs(r.ns), r.phys, r.dwords, r.win >> 28, r.gpuWin,
                r.grantFlags, r.cbWin, r.used[0], r.used[1], r.used[2], r.used[3],
                (r.readable & 1) ? "" : "?", r.other[0], r.other[1], r.other[2], r.other[3],
                (r.readable & 2) ? "" : "?", r.n58, r.fenceHit ? "Y:" : "n:", r.fenceVal);
  }
  // (f) allocator
  GpuHangAllocInfo ai;
  auto prov = g_allocProvider.load();
  const bool aiOk = prov && prov(&ai);
  if (!aiOk) HLOG("(f) grant list unavailable (allocator lock busy or LIFO off)");
  if (aiOk) {
    HLOG(
                "(f) physNext=0x%llX lowestPhysGrant=%08X belowA=%u (%llu bytes) "
                "lifoRetained=%u (%llu bytes) aliasHoles=%d/%d%s",
                (unsigned long long)ai.physNext, ai.lowestPhysGrantVA, ai.belowACount,
                (unsigned long long)ai.belowABytes, ai.lifoRetainedCount,
                (unsigned long long)ai.lifoRetainedBytes, ai.aliasHoles, ai.aliasHoleMax,
                ai.aliasHoles >= ai.aliasHoleMax && ai.aliasHoleMax ? " HOLE TABLE FULL" : "");
    for (uint32_t i = 0; i < ai.belowAListed && i < 8; i++)
      HLOG("(f)   belowA va=%08X size=0x%X lr=%08X", ai.belowA[i].va,
                  ai.belowA[i].size, ai.belowA[i].lr);
  }
  // The LIFO allocator's own view (kernel/phys_lifo.hpp): the retained
  // (free-extent) totals are lock-free, the rest came from the helper. The
  // per-grant list above comes from the provider (physLifoHangAllocInfo,
  // try_lock); a busy allocator lock just leaves it out.
  HLOG(
#if SR_ORACLE
              "(f) physNext=0x%llX retained=%u extents / %llu bytes | uniq: "
              "physGrantsBelowA=%llu lowestPhysGrant=%08X physRegionLo=%08X | lifo: lowestLive=%08X "
              "reuses=%llu cascades=%llu floorRefusals=%llu aliasHoles=%u (dropped %llu%s)%s",
              (unsigned long long)guestPhysNextForTest(), physLifoRetainedExtents(),
              (unsigned long long)physLifoRetainedBytes(),
              hOk ? (unsigned long long)helper->uniq.physGrantsBelowA : 0ull,
              hOk ? helper->uniq.lowestPhysGrantVA : 0u, hOk ? helper->uniq.physRegionLo : 0u,
#else
              "(f) physNext=0x%llX retained=%u extents / %llu bytes | uniq: unavailable (Oracle-only) | "
              "lifo: lowestLive=%08X reuses=%llu cascades=%llu floorRefusals=%llu aliasHoles=%u "
              "(dropped %llu%s)%s",
              (unsigned long long)guestPhysNextForTest(), physLifoRetainedExtents(),
              (unsigned long long)physLifoRetainedBytes(),
#endif
              hOk ? helper->lifo.lowestLiveVA : 0u,
              hOk ? (unsigned long long)helper->lifo.reuses : 0ull,
              hOk ? (unsigned long long)helper->lifo.cascades : 0ull,
              hOk ? (unsigned long long)helper->lifo.floorRefusals : 0ull,
              hOk ? helper->lifo.holes : 0u, hOk ? (unsigned long long)helper->lifo.holeDrops : 0ull,
              hOk && helper->lifo.holeDrops ? " - HOLE TABLE FULL" : "",
              helperNote(*helper));
  HLOG(
              "(f) PHYS_DOUBLE_BOOK pairs=%llu (phys-phys %llu; hook events %llu, "
              "last va=%08X other=%08X)",
              (unsigned long long)physwin::physDoubleBookCount(),
              (unsigned long long)physwin::physDoubleBookCountPhysPhys(),
              (unsigned long long)g_i1Count.load(), g_i1Va.load(), g_i1Other.load());
  // (g) threads. The thread / CS dumps format their tag at run time ("[%s] ..."), so a player log caps those
  // lines per site, shared with every other caller (a genuine [GPU_HANG] included). A host-wait report, which
  // repeats every 5 min, takes them once per process; the hang report keeps them every time, as before.
  static std::atomic<bool> s_hostThreadsDumped{false};
  if (!host || !s_hostThreadsDumped.exchange(true)) {
    dumpGuestThreadsNow(host ? "HOST_WAIT_STALL_THREADS" : "GPU_HANG_THREADS");
    if (!dumpCsHeldTable(host ? "HOST_WAIT_STALL_CS" : "GPU_HANG_CS"))
      HLOG("(g) CS map lock busy - held table partial");
  } else {
    HLOG("(g) guest threads / CS holders: in this process's first host-wait block (dumped once: their lines share "
         "a capped log budget with the hang report's)");
  }
  HLOG("==== end");
#undef HLOG
}

// Step 4 2k(4): the compact block for triggers that host work on the presenting path explains
// (gpuHangHostWorkExplains). Never a [GPU_HANG] trigger, never a minidump: the CP waits for host work, the GPU did
// not stop. A literal tag in every format string (app/log.cpp classifies the unformatted string; a milestone).
void reportHostWork(State& st, uint32_t explained, const GpuHangSnap& s) {
  const auto E = sr::LogSeverity::Error;
  const GpuCpSnapshot cp = gpuCpSnapshot();
  const auto secs = [&](uint64_t t) { return t && s.nowNs >= t ? double(s.nowNs - t) * 1e-9 : -1.0; };
  SR_LOG_BOOT(E,
              "[HOST_WORK_STALL] ==== %s%s| episode key=%08X relog=%d | now=%.3fs completed=%08X flat %.1fs | "
              "presents=%llu flat %.1fs (VdSwap #%llu XE_SWAP #%llu)",
              (explained & kGhH1) ? "H1 " : "", (explained & kGhH3) ? "H3 " : "", st.hwEp.key, st.hwEp.relogs,
              double(s.nowNs - st.startNs) * 1e-9, s.completed, secs(s.completedSinceNs),
              (unsigned long long)s.presents, secs(s.presentsSinceNs), (unsigned long long)getVdSwapCount(),
              (unsigned long long)cp.swaps);
  SR_LOG_BOOT(E,
              "[HOST_WORK_STALL] (a) host work %s on host tid %u (%s) for %.2fs; CP phase=%s for %.2fs - the CP "
              "waits for host work on the presenting path, not a GPU hang (SR_GPU_HANG_HOST_WORK=1; H0 / H2 are "
              "judged as before)",
              hostWorkName(s.hostWorkKind), s.hostWorkTid, s.hostWorkOnCp ? "the CP thread" : "the render thread",
              secs(s.hostWorkSinceNs), phaseName(cp.phase), secs(cp.phaseSinceNs));
  SR_LOG_BOOT(E, "[HOST_WORK_STALL] ==== end");
}

void tick(State& st) {
  GpuHangSnap s;
  s.nowNs = nowNs();
  // native/quit-ringspace: from the graceful quit's ring cut on, the CP is parked by design (a flat fence, a stranded
  // waiter): the watchdog stands down - no trigger (gpuHangEvaluate), host-wait report or dump - and says so once.
  // A hang that began before the request was judged on every tick before the cut, exactly as before.
  s.quitCut = sr::kernel::quit::g_ringCut.load(std::memory_order_acquire);
  if (s.quitCut && !st.quitStoodDown) {
    st.quitStoodDown = true;
    g_quitStandDowns.fetch_add(1, std::memory_order_relaxed);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[QUIT] gpu-hang watchdog stands down: the ring is cut and the CP parks by design (a flat fence from "
                "here on is not a hang) - no [GPU_HANG] trigger, host-wait report or dump after this");
  }
  const GpuCpSnapshot cp = gpuCpSnapshot();
  s.cpStarted = cp.started;
  s.cpPhase = cp.phase;
  s.cpPhaseSinceNs = cp.phaseSinceNs;
  s.rptr = cp.rptr;
  s.wptr = cp.wptr;
  s.published = cp.published;
  {
    HostWorkView hw;
    if (hostWorkOldest(&hw)) {   // always false with SR_GPU_HANG_HOST_WORK unset
      s.hostWork = true;
      s.hostWorkKind = hw.kind;
      s.hostWorkOnCp = hw.onCp;
      s.hostWorkTid = hw.hostTid;
      s.hostWorkSinceNs = hw.sinceNs;
    }
    uint64_t done = 0, signaled = 0;
    const GpuQueueProbe probe = g_queueProbe.load(std::memory_order_acquire);
    if (probe && probe(&done, &signaled)) {
      s.gpuCaughtUp = done >= signaled || (st.haveGpuCompleted && done != st.prevGpuCompleted);
      st.prevGpuCompleted = done;
      st.haveGpuCompleted = true;
    }
  }

  const uint32_t fenceP = g_fenceP.load(std::memory_order_acquire);
  const uint32_t dev = g_dev.load(std::memory_order_relaxed);
  uint32_t completed = 0;
  s.fenceValid = fenceValue(&completed);
  if (s.fenceValid) {
    if (!st.haveCompleted || completed != st.prevCompleted) {
      st.haveCompleted = true;
      st.prevCompleted = completed;
      st.completedSinceNs = s.nowNs;
    }
    s.completed = completed;
    s.completedSinceNs = st.completedSinceNs;
  }
  uint32_t next = 0;
  if (dev && safeRead32(dev + 0x2A1Cu, &next)) {
    s.issuedValid = true;
    s.lastIssued = next - 2u;
  }
  // Cross-check the fence-block latch once against the device's own pointer.
  if (!st.crossChecked && dev && fenceP) {
    uint32_t waitVA = 0;
    if (safeRead32(dev + 0x2A10u, &waitVA) && waitVA) {
      st.crossChecked = true;
      const uint32_t wp = waitVA & 0x1FFFFFFFu;
      SR_LOG_BOOT(wp == fenceP ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
                  "[GPU_HANG_WD] fence block: rptrWb-0x3C=%08X, *(dev+0x2A10)=%08X (phys %08X) %s",
                  fenceP, waitVA, wp, wp == fenceP ? "match" : "MISMATCH");
    }
  }

  SlotView slots[kSlots];
  const int nSlots = readSlots(slots, s.nowNs);
  for (int i = 0; i < nSlots; i++) {
    const SlotView& w = slots[i];
    if (!s.anyWaiter || w.entryNs < s.anyWaiterEntryNs) {
      s.anyWaiter = true;
      s.anyWaiterEntryNs = w.entryNs;
    }
    if (w.kind == kGwBlockOnFence && (!s.fenceWaiter || w.entryNs < s.fenceWaiterEntryNs)) {
      s.fenceWaiter = true;
      s.fenceWaiterEntryNs = w.entryNs;
      s.fenceWaiterTarget = w.a;
    }
  }

  s.presents = getVdSwapCount() + cp.swaps;
  if (s.presents != st.prevPresents) {
    st.prevPresents = s.presents;
    st.presentsSinceNs = s.nowNs;
  }
  s.presentsSinceNs = st.presentsSinceNs;
  // H2's GPU-wait condition (review): the slot age is taken with the guarded
  // subtraction (a waiter that entered after nowNs was sampled is age 0, not
  // 2^64), and the condition must hold on consecutive ticks for >= 1 s.
  bool rawWait = false;
  if (s.presents > 0 && gpuHangPresentsFlat(s))
    rawWait = (s.anyWaiter && s.nowNs >= s.anyWaiterEntryNs &&
               s.nowNs - s.anyWaiterEntryNs >= kGhH0Ns) ||
              anyThreadInGpuWait();
  s.threadInGpuWait = gpuHangGpuWaitPersists(st.gpuWaitSinceNs, rawWait, s.nowNs);

  // render/boot-attach (Astra round 2): a host wait (CP phase 5) that lasts escalates on its own schedule - at
  // 60 s, then every 5 min - with the full block as a [HOST_WAIT_STALL]: never a [GPU_HANG] trigger, never a
  // minidump, never an exit (the CP never proceeds without what it waits for). gpuHangEvaluate below fires
  // H0-H2 as usual (annotated with the wait, Astra round 3); only H3 is set aside.
  if (!s.quitCut && s.cpPhase == kCpPhaseHostWait && s.cpPhaseSinceNs) {
    if (st.hostWaitSinceNs != s.cpPhaseSinceNs) {  // a new wait
      st.hostWaitSinceNs = s.cpPhaseSinceNs;
      st.hostWaitFired = 0;
    }
    const uint64_t waitAge = s.nowNs >= s.cpPhaseSinceNs ? s.nowNs - s.cpPhaseSinceNs : 0;
    if (gpuHangHostWaitDue(waitAge, st.hostWaitFired)) {
      g_hostWaitStalls.fetch_add(1, std::memory_order_relaxed);
      char head[256];
      snprintf(head, sizeof(head),
               "the CP has waited %.1f s for %s (escalation #%u; CP phase 5 - a host-wait stall, not a guest hang; "
               "the CP does not proceed without it)",
               double(waitAge) * 1e-9, gpuCpHostWaitName(cp.hostWait), st.hostWaitFired);
      report(st, 0, s, slots, nSlots, dev, fenceP, head);
    }
  } else {
    st.hostWaitSinceNs = 0;
  }

  uint32_t mask = gpuHangEvaluate(s);
  // step 4 2k(4): what host work on the presenting path explains is a [HOST_WORK_STALL], not a [GPU_HANG] trigger
  if (const uint32_t explained = gpuHangHostWorkExplains(mask, s)) {
    if (gpuHangEpisodeShouldLog(st.hwEp, s.fenceValid ? s.completed : 0u, s.nowNs)) {
      g_hostWorkStalls.fetch_add(1, std::memory_order_relaxed);
      reportHostWork(st, explained, s);
    }
    mask &= ~explained;
  }
  // Episodes: H0/H1 are keyed by the completed fence, H2/H3 alone by the
  // present count (a fence that keeps moving under a present stall is ONE
  // episode, not one per tick). An H2/H3-only report is also held to one per
  // 5 s across episodes; H0/H1 are never held back.
  const bool strong = (mask & (kGhH0 | kGhH1)) != 0;
  const bool held = !strong && st.lastReportNs && s.nowNs - st.lastReportNs < 5000000000ull;
  if (mask && !held &&
      gpuHangEpisodeShouldLog(st.ep, gpuHangEpisodeKey(mask, s.fenceValid ? s.completed : 0u, s.presents),
                              s.nowNs)) {
    st.lastReportNs = s.nowNs;
    report(st, mask, s, slots, nSlots, dev, fenceP);
    // The hang dump: H0/H1/H2 only (the spec). H3 alone is report-only, as the
    // old stall log was - a boot-time CP stall (v1_kthread1: 5.5 s behind the
    // shader-cache load) must not spend the process's hang dump.
    if (!st.hDumped && gpuHangWantsDump(mask)) {
      st.hDumped = true;
      writeDump(st, "hang trigger", dev, fenceP, slots, nSlots);
    }
  }

  // I1: [PHYS_DOUBLE_BOOK] is raised synchronously at registration (F2,
  // kernel/phys_window.cpp); the hook only notes it, the dump is taken here.
  // The dump keys on the GPU-physical vs GPU-physical class (the v1_gpuhang
  // collision) from the registry's own counter, which also covers
  // double-books before the hook was installed (the watchdog starts at the
  // first doorbell). Pairs with a heap-routed grant are logged by F2 itself
  // (pre-iter-462 runs had 175 of them) and do not spend a dump.
  const uint64_t i1 = physwin::physDoubleBookCountPhysPhys();
  if (!s.quitCut && i1 != st.i1Seen) {
    st.i1Seen = i1;
    if (!st.i1Dumped) {
      st.i1Dumped = true;
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[GPU_HANG] I1 PHYS_DOUBLE_BOOK phys-phys=%llu pairs=%llu (hook events %llu, "
                  "last va=%08X other=%08X) - dumping",
                  (unsigned long long)i1, (unsigned long long)physwin::physDoubleBookCount(),
                  (unsigned long long)g_i1Count.load(), g_i1Va.load(), g_i1Other.load());
      writeDump(st, "PHYS_DOUBLE_BOOK", dev, fenceP, slots, nSlots);
    }
  }
}

void wdMain() {
#if defined(_WIN32)
  using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
  if (HMODULE k = GetModuleHandleW(L"kernel32.dll"))
    if (auto fn = reinterpret_cast<SetDesc>(GetProcAddress(k, "SetThreadDescription")))
      fn(GetCurrentThread(), L"GPU_HANG_WD");
#endif
  State st;
  st.startNs = nowNs();
  st.presentsSinceNs = st.startNs;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[GPU_HANG_WD] watchdog up (250 ms; H0 1s / H1 3s / H2 10s / H3 5s; observe-only)");
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    tick(st);
  }
}

}  // namespace

bool gpuHangWdLastFenceWriteForTest(GpuHangFenceWriteForTest* out) {
  for (int tries = 0; tries < 8; ++tries) {
    const uint32_t s0 = g_fw.seq.load(std::memory_order_acquire);
    GpuHangFenceWriteForTest v{g_fw.count.load(), g_fw.op.load(), g_fw.addr.load(), g_fw.value.load(),
                               g_fw.win.load()};
    if (!(s0 & 1u) && g_fw.seq.load(std::memory_order_acquire) == s0) {
      *out = v;
      return v.count != 0;
    }
  }
  return false;
}

void gpuHangWdTickForTest() {
  static State st;  // one watchdog state across the test's ticks
  if (!st.startNs) {
    st.startNs = nowNs();
    st.presentsSinceNs = st.startNs;
  }
  tick(st);
}

void gpuHangWdHelperBodyForTest(void (*body)()) { g_helperBodyForTest.store(body, std::memory_order_release); }

void gpuHangWdHelperStatsForTest(uint64_t* started, uint64_t* skipped) {
  if (started) *started = g_helpersStarted.load(std::memory_order_relaxed);
  if (skipped) *skipped = g_helpersSkipped.load(std::memory_order_relaxed);
}

void gpuHangWdReportForTest(bool hostWait) {
  static State st;  // one watchdog's state across the test's reports
  if (!st.startNs) st.startNs = nowNs();
  GpuHangSnap s;
  s.nowNs = nowNs();
  if (hostWait) {
    report(st, 0, s, nullptr, 0, 0, 0,
           "the CP has waited 60.0 s for the live backend's attach (escalation #1; test report - a host-wait stall, "
           "not a guest hang)");
  } else {
    report(st, kGhH1, s, nullptr, 0, 0, 0);
  }
}

void gpuHangSetQueueProbe(GpuQueueProbe probe) { g_queueProbe.store(probe, std::memory_order_release); }

void gpuHangWdHostWorkReportForTest() {
  static State st;
  if (!st.startNs) st.startNs = nowNs();
  GpuHangSnap s;
  s.nowNs = nowNs();
  s.hostWork = true;
  s.hostWorkKind = kHostWorkDumpWrite;
  s.hostWorkSinceNs = s.nowNs - 3200000000ull;
  gpuHangEpisodeShouldLog(st.hwEp, 0x65FBu, s.nowNs);
  reportHostWork(st, kGhH1, s);
}

uint64_t gpuHangWdHostWorkStallsForTest() { return g_hostWorkStalls.load(std::memory_order_relaxed); }

double gpuHangWdDumpForTest() {
  static State st;  // its own dump budget (2)
  const uint64_t t0 = nowNs();
  writeDump(st, "test", 0, 0, nullptr, 0);
  return double(nowNs() - t0) * 1e-6;
}

GpuHangWaitForTest gpuHangWdWaitForTest(uint32_t kind) {
  GpuHangWaitForTest w;
  w.enters = kind < 4 ? g_waitEnters[kind].load() : 0;
  w.lastKind = g_lastWaitKind.load();
  w.dev = g_lastWaitDev.load();
  w.a = g_lastWaitA.load();
  w.b = g_lastWaitB.load();
  for (int i = 0; i < kSlots; ++i)
    if (g_slots[i].entryNs.load(std::memory_order_acquire)) w.occupied++;
  return w;
}

bool gpuHangWdLastIbForTest(GpuHangIbForTest* out) {
  IbView v;
  if (readIbRing(&v, 1) != 1) return false;
  *out = GpuHangIbForTest{v.seq, v.phys, v.dwords, v.win, v.used[0], v.other[0],
                          v.readable, v.n58, v.fenceHit, v.fenceVal};
  return true;
}

void gpuHangWdStart() {
  static std::atomic<bool> s_started{false};
  if (!gpuHangWdEnabled() || s_started.exchange(true)) return;
  physwin::setPhysDoubleBookHook(&gpuHangWdNotePhysDoubleBook);   // I1
  if (!g_allocProvider.load()) setGpuHangAllocProvider(&physLifoHangAllocInfo);   // (f)
  std::thread(wdMain).detach();
}

}  // namespace sr::kernel

// ── (3) observe strong-defs: record the wait, delegate unconditionally ──────
// native/quit-drain: each also marks the thread's quit gate slot while it is inside the wait (kernel/quit.hpp
// gpuWaitEnter / gpuWaitLeave - a few owner-only stores, no RMW, whatever SR_GPU_HANG_WD says), so the graceful quit's
// settle can tell a thread stranded in a wait the parked CP can never end from one running guest code.

// GpuBlockOnFence(dev r3, target fence r4, mode r5).
PPC_FUNC(sub_820FD9A8) {
  SR_NATIVE_SCOPE(0x820FD9A8);  // NATIVE N0 (scripts/gen_native_brackets.py)
  const int slot = sr::kernel::gpuHangWdWaitEnter(sr::kernel::kGwBlockOnFence, ctx.r3.u32,
                                                  ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr),
                                                  ctx.r1.u32);
  const sr::kernel::quit::GpuWaitMark qw =
      sr::kernel::quit::gpuWaitEnter(sr::kernel::quit::kGpuWaitFence, ctx.r3.u32, ctx.r4.u32, 0u,
                                     ctx.r1.u32 - sr::kernel::quit::kWaitRecBelowSp);  // the record its spin passes
  __imp__sub_820FD9A8(ctx, base);
  sr::kernel::quit::gpuWaitLeave(qw);
  sr::kernel::gpuHangWdWaitLeave(slot);
}

// GpuRingWaitForSpace(dev r3, end r4, generation r5): waits until the progress word *(fb+4) frees [.., end) of
// generation r5 (kernel/quit.hpp ringSpaceFree). native/quit-ringspace: the mark carries r5 and the wait's record (the
// caller's r1 - 0x40: what its one spin call passes GpuRingWaitProgress) - this body is localized (skip_lr), so that
// call leaves ctx.lr stale and the record alone names it (quit.hpp gpuSpinSite).
PPC_FUNC(sub_820FCB30) {
  SR_NATIVE_SCOPE(0x820FCB30);  // NATIVE N0 (scripts/gen_native_brackets.py)
  const int slot = sr::kernel::gpuHangWdWaitEnter(sr::kernel::kGwRingSpace, ctx.r3.u32,
                                                  ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr),
                                                  ctx.r1.u32);
  const sr::kernel::quit::GpuWaitMark qw =
      sr::kernel::quit::gpuWaitEnter(sr::kernel::quit::kGpuWaitRingSpace, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                                     ctx.r1.u32 - sr::kernel::quit::kWaitRecBelowSp);  // its one spin call's record
  __imp__sub_820FCB30(ctx, base);
  sr::kernel::quit::gpuWaitLeave(qw);
  sr::kernel::gpuHangWdWaitLeave(slot);
}

// GpuRingWaitReadPtrWrap(dev r3, wptr r4, count r5): target rptr =
// (r4 + r5) & *(dev+13480), resolved at report time.
PPC_FUNC(sub_820FD150) {
  SR_NATIVE_SCOPE(0x820FD150);  // NATIVE N0 (scripts/gen_native_brackets.py)
  const int slot = sr::kernel::gpuHangWdWaitEnter(sr::kernel::kGwReadPtrWrap, ctx.r3.u32,
                                                  ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr),
                                                  ctx.r1.u32);
  const sr::kernel::quit::GpuWaitMark qw =
      sr::kernel::quit::gpuWaitEnter(sr::kernel::quit::kGpuWaitReadPtrWrap, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_820FD150(ctx, base);
  sr::kernel::quit::gpuWaitLeave(qw);
  sr::kernel::gpuHangWdWaitLeave(slot);
}

// GpuRingWaitProgress(rec r3 = {dev, reason, last completed, tick seed}): the XDK wait primitive every GPU busy-wait
// turns (native/quit-drain round 2). A PASS-THROUGH diagnostic on the default path: the body runs unconditionally,
// exactly once, and its r3 is the return, untouched; no guest register or memory is written. Only a call from the spin
// loop of the wait this thread is innermost in (kernel/quit.hpp gpuSpinSite: GpuBlockOnFence's - return address
// 0x820FDA44 AND that wait's own record; GpuRingWaitForSpace's - its own record, native/quit-ringspace) touches this
// thread's quit gate slot - owner-only stores: the first marks the wait's SPIN phase, a 0 return (the no-GPU mode, or
// the body's own hang trap - KTHREAD+0x58, NATIVE_ROADMAP P2.10, unchanged here) marks it EXITED (gpuSpinTick /
// gpuSpinExit). The graceful quit's settle reads them; nothing reads them before a quit request. Every other call costs
// a thread-local load and a relaxed load (gpuSpinSite).
PPC_FUNC(sub_820F33E8) {
  const uint32_t spin = sr::kernel::quit::gpuSpinSite(uint32_t(ctx.lr), ctx.r3.u32);
  if (spin) sr::kernel::quit::gpuSpinTick(sr::kernel::readGuestU32BE(ctx.r3.u32));  // rec+0 = dev
  __imp__sub_820F33E8(ctx, base);
  if (spin && ctx.r3.u32 == 0u) sr::kernel::quit::gpuSpinExit();
}
