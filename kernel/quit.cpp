// tools/graceful-quit: the graceful quit and the terminal snapshot - contract in quit.hpp, design in
// docs/TOOLKIT.md 1.8m.
#include "quit.hpp"
#include "../gfx/present_dump_writer.hpp"  // step 4 item 4(c): the exit drain
#include "../gfx/pso_warm.hpp"             // step 4 2k: the pipeline warm-up / manifest at the quit
#include "../gfx/null_renderer.hpp"         // step 4 (Astra 10:5x #1): the render thread's exit quiescence

#include "host_automation.hpp"
#include "guest_context.hpp"
#include "gpu_submit.hpp"
#include "gpu_ringless.hpp"
#include "native_flush.hpp"
#include "native_exec.hpp"
#include "native_meter.hpp"
#include "natural_gauges.hpp"
#include "p2_trace.hpp"
#include "audio_native_census.hpp"
#include "ab_toggle.hpp"
#include "input_recorder.hpp"
#include "../app/log.hpp"
#include "../app/stdio_guard.hpp"
#include "../gfx/pm4_coverage.hpp"
#include "../gfx/resource_memory.hpp"
#if SR_ORACLE
#include "../gfx/resource_memory_check.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// The all-threads registry (kernel/guest_context.cpp, iter-295g): every guest-running host thread.
extern "C" sr::kernel::GuestThreadSlot g_debugThreadRegistry[64];

namespace sr::kernel::quit {

namespace {

// ── process state: who owns the exit ────────────────────────────────────────────────────────────────────────
enum State : int { kRunning = 0, kQuitting = 1, kExiting = 2 };
std::atomic<int> g_state{kRunning};

// ── parking ──────────────────────────────────────────────────────────────────────────────────────────────────
// Parked threads block on this manual-reset event. Production never sets it (the process exits around them);
// resetForTest() does.
HANDLE parkEvent() {
  static HANDLE h = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  return h;
}

struct ParkedSlot {
  uint32_t tid = 0;
  const char* where = nullptr;
};
constexpr int kParkedMax = 256;
std::mutex g_parkedMu;
ParkedSlot g_parked[kParkedMax];
std::atomic<uint32_t> g_parkedN{0};
thread_local const char* t_exempt = nullptr;

// Registers the thread as parked (its acknowledgement), then blocks. Holds g_parkedMu only for the bookkeeping.
void parkForever(const char* where) {
  {
    std::lock_guard<std::mutex> lk(g_parkedMu);
    const uint32_t tid = GetCurrentThreadId();
    for (ParkedSlot& s : g_parked) {
      if (!s.tid) {
        s.tid = tid;
        s.where = where;
        break;
      }
    }
  }
  g_parkedN.fetch_add(1, std::memory_order_acq_rel);
  WaitForSingleObject(parkEvent(), INFINITE);
  // Only resetForTest() gets here.
  g_parkedN.fetch_sub(1, std::memory_order_acq_rel);
  std::lock_guard<std::mutex> lk(g_parkedMu);
  const uint32_t tid = GetCurrentThreadId();
  for (ParkedSlot& s : g_parked)
    if (s.tid == tid) s = ParkedSlot{};
}

const char* parkedWhere(uint32_t tid) {
  std::lock_guard<std::mutex> lk(g_parkedMu);
  for (const ParkedSlot& s : g_parked)
    if (s.tid == tid) return s.where;
  return nullptr;
}

// ── gate slots ───────────────────────────────────────────────────────────────────────────────────────────────
constexpr int kGateMax = 256;
GateSlot g_gates[kGateMax];
GateSlot g_overflowGate;  // a full table: shared, never examined (counted)
std::atomic<uint32_t> g_gateOverflow{0};

GateSlot* findGate(uint32_t tid) {
  for (GateSlot& g : g_gates)
    if (g.tid.load(std::memory_order_acquire) == tid) return &g;
  return nullptr;
}

std::atomic<uint64_t> g_importPasses{0};  // release imports that passed the entry gate after the thread cut
std::atomic<uint64_t> g_drops{0};         // doorbells dropped between the ring cut and the thread cut

// ── the CP drain handshake ──────────────────────────────────────────────────────────────────────────────────
std::atomic<bool> g_drainReq{false};
std::atomic<bool> g_drained{false};
CpDrained g_drainInfo;  // written before g_drained (release)

// ── native/quit-drain: the stranded state's inputs, set by run() ───────────────────────────────────────────────
std::atomic<bool> g_cpStopped{false};      // the CP drain ended: the CP parked at its drain point, or there is none
std::atomic<bool> g_allowStranded{true};   // Config::allowStranded (SR_QUIT_STRANDED=0: false)
// round 2: g_gpuWaitSeqGen's value at the ring cut (taken under the publish mutex) - a spin that began at or before it
// began before the cut; kNoCut until a cut.
constexpr uint64_t kNoCut = ~uint64_t(0);
std::atomic<uint64_t> g_cutWaitSeq{kNoCut};
// round 2: the fences the cut's dropped publishes carried, per device (recordDroppedFence; after the cut only).
// native/quit-ringspace: and the progress words they carried (recordDroppedProgress), in the same device tables.
constexpr int kDropDevMax = 4, kDropFenceMax = 1024;
struct DropDev {
  uint32_t dev = 0;
  int n = 0, pn = 0;
  uint32_t fence[kDropFenceMax] = {};
  uint32_t prog[kDropFenceMax] = {};
};
std::mutex g_dropMu;
DropDev g_dropDev[kDropDevMax];
int g_dropDevN = 0;
std::atomic<uint32_t> g_dropFences{0}, g_dropProgress{0}, g_dropOverflow{0};

// ── families ─────────────────────────────────────────────────────────────────────────────────────────────────
struct Family {
  const char* name;
  Printer print;
};
constexpr int kFamilyMax = 32;
std::mutex g_famMu;
Family g_families[kFamilyMax];
int g_familyN = 0;

// ── the channel ──────────────────────────────────────────────────────────────────────────────────────────────
HANDLE g_req = nullptr, g_ack = nullptr;
std::string g_reqName, g_ackName;
void (*g_testRequest)(void*) = nullptr;
void* g_testUser = nullptr;

// The bounds (SR_QUIT_*_MS): name, default, accepted range.
struct Bound {
  const char* env;
  uint32_t def, lo, hi;
};
constexpr Bound kHard{"SR_QUIT_HARD_MS", 30000, 1000, 600000};
constexpr Bound kDrain{"SR_QUIT_DRAIN_MS", 10000, 100, 120000};
constexpr Bound kSettle{"SR_QUIT_SETTLE_MS", 200, 10, 60000};
constexpr Bound kSettleMax{"SR_QUIT_SETTLE_MAX_MS", 3000, 100, 120000};
constexpr Bound kCutDelay{"SR_QUIT_DIAG_CUT_DELAY_MS", 0, 0, 5000};  // native/quit-ringspace: a diagnostic (Config)

uint32_t envMs(const Bound& b, std::string* rejected = nullptr) {
  bool bad = false;
  const uint32_t v = parseMs(std::getenv(b.env), b.def, b.lo, b.hi, &bad);
  if (bad && rejected) *rejected += std::string(rejected->empty() ? "" : ",") + b.env;
  return v;
}

// SR_QUIT_STRANDED=0: a stranded GPU wait is not acknowledged (the quit fails as before; native/quit-drain).
bool strandedAllowedByEnv() {
  const char* e = std::getenv("SR_QUIT_STRANDED");
  return !(e && e[0] == '0');
}

}  // namespace

// ── bounds ─────────────────────────────────────────────────────────────────────────────────────────────────────
uint32_t parseMs(const char* text, uint32_t def, uint32_t lo, uint32_t hi, bool* bad) {
  if (bad) *bad = false;
  if (!text || !*text) return def;
  uint64_t v = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9' || p - text >= 10) {  // a sign, a space, a unit, or more than 10 digits
      if (bad) *bad = true;
      return def;
    }
    v = v * 10u + uint64_t(*p - '0');
  }
  if (v < lo || v > hi) {
    if (bad) *bad = true;
    return def;
  }
  return uint32_t(v);
}

// ── names ──────────────────────────────────────────────────────────────────────────────────────────────────────
std::string sanitizeInstance(const char* instanceId) {
  std::string id = (instanceId && *instanceId) ? instanceId : "default";
  for (char& c : id) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                    c == '.' || c == '-';
    if (!ok) c = '_';
  }
  if (id.size() > 64) id.resize(64);
  return id;
}

std::string requestEventName(const char* instanceId, uint32_t pid) {
  return "Local\\SR_QUIT_" + sanitizeInstance(instanceId) + "_" + std::to_string(pid);
}

std::string ackEventName(const char* instanceId, uint32_t pid) {
  return "Local\\SR_QUIT_ACK_" + sanitizeInstance(instanceId) + "_" + std::to_string(pid);
}

// ── gate slots ─────────────────────────────────────────────────────────────────────────────────────────────────
GateSlot* claimGate() {
  const uint32_t tid = GetCurrentThreadId();
  // A slot left by a dead thread whose id the OS reused for this one: retire it first.
  for (GateSlot& g : g_gates) {
    uint32_t t = tid;
    g.tid.compare_exchange_strong(t, 0u, std::memory_order_acq_rel);
  }
  for (GateSlot& g : g_gates) {
    uint32_t zero = 0;
    if (g.tid.load(std::memory_order_relaxed) == 0 &&
        g.tid.compare_exchange_strong(zero, tid, std::memory_order_acq_rel)) {
      g.import.store(nullptr, std::memory_order_relaxed);
      g.locks.store(0, std::memory_order_relaxed);
      g.guestCalls.store(0, std::memory_order_relaxed);
      g.gpuWait.store(0, std::memory_order_relaxed);
      g.gpuWaitDev.store(0, std::memory_order_relaxed);
      g.gpuWaitArg.store(0, std::memory_order_relaxed);
      g.gpuWaitGen.store(0, std::memory_order_relaxed);
      g.gpuWaitRec.store(0, std::memory_order_relaxed);
      g.gpuWaitSeq.store(0, std::memory_order_relaxed);
      g.role.store(nullptr, std::memory_order_release);
      t_gate = &g;
      return &g;
    }
  }
  g_gateOverflow.fetch_add(1, std::memory_order_relaxed);
  t_gate = &g_overflowGate;
  return &g_overflowGate;
}

int heldLocks() { return gate().locks.load(std::memory_order_relaxed); }

// ── park points ────────────────────────────────────────────────────────────────────────────────────────────────
bool releasesLock(const char* n) {
  static const char* const kRelease[] = {"RtlLeaveCriticalSection", "KeReleaseSpinLockFromRaisedIrql",
                                         "KfReleaseSpinLock",       "KfLowerIrql",
                                         "KeLeaveCriticalRegion",   "KeUnlockL2"};
  if (!n) return false;
  for (const char* r : kRelease)
    if (n == r || std::strcmp(n, r) == 0) return true;
  return false;
}

bool isLockAcquire(const char* n) {
  static const char* const kAcquire[] = {"KfAcquireSpinLock", "KeAcquireSpinLockAtRaisedIrql",
                                         "RtlEnterCriticalSection"};
  if (!n) return false;
  for (const char* a : kAcquire)
    if (n == a || std::strcmp(n, a) == 0) return true;
  return false;
}

// The gates arm at the thread cut, which comes after the CP drain: from then on EVERY thread parks at its next gate -
// an exempt loop and a thread holding a guest lock included (the smoke roll gq/smoke1: a render thread holding a
// critical section passed the gates and drew a whole frame after the cut). Nothing the quit still needs can wait on
// what a parked thread holds: the CP has drained and parked; a thread that then wants that lock blocks inside its
// acquire import, can never get it, and would park at that import's exit if it did (TOOLKIT 1.8m).
void parkAtImport(const char* importName) {
  if (!g_threadCut.load(std::memory_order_acquire)) return;
  if (releasesLock(importName)) {  // release first (others may want it); the import's exit gate parks the thread
    g_importPasses.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  parkForever("import");
}

void parkAfterImport(const char*) {
  if (!g_threadCut.load(std::memory_order_acquire)) return;
  parkForever("import-exit");
}

void parkAtGuestCall() {
  if (!g_threadCut.load(std::memory_order_acquire)) return;
  parkForever("guest-call");
}

void safePoint(const char* who) {
  if (!g_threadCut.load(std::memory_order_acquire)) return;
  parkForever(who ? who : "safepoint");
}

void markExempt(const char* who) {
  t_exempt = who ? who : "exempt";
  gate().role.store(t_exempt, std::memory_order_release);
}
bool exempt() { return t_exempt != nullptr; }

void markHostLoop() {
  GateSlot& g = gate();
  const char* none = nullptr;
  g.role.compare_exchange_strong(none, kHostLoop, std::memory_order_acq_rel);
}

// ── the ring cut ───────────────────────────────────────────────────────────────────────────────────────────────
void cutRingUnder(std::mutex& publishMutex) {
  std::lock_guard<std::mutex> lk(publishMutex);
  // round 2: every wait residence / spin numbered after this began after the cut (a pre-request hang never qualifies)
  g_cutWaitSeq.store(g_gpuWaitSeqGen.load(std::memory_order_acquire), std::memory_order_release);
  g_ringCut.store(true, std::memory_order_relaxed);  // the mutex orders it against every later doorbell
}

// ── what the cut dropped (round 2) ─────────────────────────────────────────────────────────────────────────────────
// The device's table (g_dropMu held); nullptr = a full set of tables (counted as overflow).
DropDev* dropDevLocked(uint32_t dev) {
  for (int i = 0; i < g_dropDevN; ++i)
    if (g_dropDev[i].dev == dev) return &g_dropDev[i];
  if (g_dropDevN >= kDropDevMax) {
    g_dropOverflow.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  DropDev* d = &g_dropDev[g_dropDevN++];
  d->dev = dev;
  d->n = 0;
  d->pn = 0;
  return d;
}

void recordDroppedFence(uint32_t dev, uint32_t fence) {
  if (!dev || !fence || !g_ringCut.load(std::memory_order_acquire)) return;  // the cut's drops only
  std::lock_guard<std::mutex> lk(g_dropMu);
  DropDev* d = dropDevLocked(dev);
  if (!d) return;
  for (int i = 0; i < d->n; ++i)
    if (d->fence[i] == fence) return;
  if (d->n >= kDropFenceMax) {
    g_dropOverflow.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  d->fence[d->n++] = fence;
  g_dropFences.fetch_add(1, std::memory_order_relaxed);
}

void recordDroppedProgress(uint32_t dev, uint32_t progress) {
  if (!dev || !progress || !g_ringCut.load(std::memory_order_acquire)) return;  // the cut's drops only (progress)
  std::lock_guard<std::mutex> lk(g_dropMu);
  DropDev* d = dropDevLocked(dev);
  if (!d) return;
  for (int i = 0; i < d->pn; ++i)
    if (d->prog[i] == progress) return;
  if (d->pn >= kDropFenceMax) {
    g_dropOverflow.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  d->prog[d->pn++] = progress;
  g_dropProgress.fetch_add(1, std::memory_order_relaxed);
}

uint32_t ibTailFence(uint32_t dev, uint32_t phys, uint32_t dwords, uint32_t* progress) {
  if (progress) *progress = 0u;
  if (!dev || dwords < 10u) return 0;  // the fence tail is 10 dwords (INVALIDATE_STATE + two EVENT_WRITE_SHD)
  auto mangle = [](uint32_t va) { return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu); };
  const uint32_t seg = sr::kernel::readGuestU32BE(dev + 0x34C4u);  // the close's own segment start (820FD660)
  if (!seg || mangle(seg) != phys) return 0;
  const uint32_t fb = sr::kernel::readGuestU32BE(dev + 0x2A10u);
  const uint32_t t = seg + dwords * 4u - 16u;  // the last EVENT_WRITE_SHD: {header, 3, mangled(fb)|2, F}
  if (!fb || sr::kernel::readGuestU32BE(t) != 0xC0025800u || sr::kernel::readGuestU32BE(t + 4u) != 3u ||
      sr::kernel::readGuestU32BE(t + 8u) != (mangle(fb) | 2u))
    return 0;
  // native/quit-ringspace: the packet before it, {header, 3, mangled(fb+4)|2, (gen & 3) | cursor} - the progress word
  const uint32_t p = t - 16u;
  if (progress && sr::kernel::readGuestU32BE(p) == 0xC0025800u && sr::kernel::readGuestU32BE(p + 4u) == 3u &&
      sr::kernel::readGuestU32BE(p + 8u) == (mangle(fb + 4u) | 2u))
    *progress = sr::kernel::readGuestU32BE(p + 12u);
  return sr::kernel::readGuestU32BE(t + 12u);
}

bool droppedFence(uint32_t dev, uint32_t fence) {
  std::lock_guard<std::mutex> lk(g_dropMu);
  for (int i = 0; i < g_dropDevN; ++i)
    if (g_dropDev[i].dev == dev)
      for (int k = 0; k < g_dropDev[i].n; ++k)
        if (g_dropDev[i].fence[k] == fence) return true;
  return false;
}
uint32_t droppedFenceCount() { return g_dropFences.load(std::memory_order_relaxed); }
uint32_t droppedFenceOverflow() { return g_dropOverflow.load(std::memory_order_relaxed); }

bool droppedProgressFrees(uint32_t dev, uint32_t end, uint32_t gen) {
  std::lock_guard<std::mutex> lk(g_dropMu);
  for (int i = 0; i < g_dropDevN; ++i)
    if (g_dropDev[i].dev == dev)  // the waiting device's own words: another ring's progress frees nothing here
      for (int k = 0; k < g_dropDev[i].pn; ++k)
        if (ringSpaceFree(g_dropDev[i].prog[k], end, gen)) return true;
  return false;
}
bool droppedProgress(uint32_t dev, uint32_t progress) {
  std::lock_guard<std::mutex> lk(g_dropMu);
  for (int i = 0; i < g_dropDevN; ++i)
    if (g_dropDev[i].dev == dev)
      for (int k = 0; k < g_dropDev[i].pn; ++k)
        if (g_dropDev[i].prog[k] == progress) return true;
  return false;
}
uint32_t droppedProgressCount() { return g_dropProgress.load(std::memory_order_relaxed); }

void formatStranded(const StrandedId& s, char* out, size_t n) {
  const char* w = s.kind == kGpuWaitFence ? "GpuBlockOnFence" : s.kind == kGpuWaitRingSpace ? "GpuRingWaitForSpace" : "?";
  std::snprintf(out, n, "tid=%u wait=%s dev=%08X target=%08X completed=%08X next=%08X seq=%llu spin=%llu locks=%d",
                s.tid, w, s.dev, s.target, s.completed, s.next, (unsigned long long)s.seq,
                (unsigned long long)s.spin, s.locks);
}

bool sameStranded(const ThreadSummary& a, const ThreadSummary& b, char* why, size_t n) {
  if (a.stranded != b.stranded || a.strandedOverflow != b.strandedOverflow) {
    std::snprintf(why, n, "stranded %d%s -> %d%s", a.stranded, a.strandedOverflow ? " (overflow)" : "", b.stranded,
                  b.strandedOverflow ? " (overflow)" : "");
    return false;
  }
  const int k = a.stranded < ThreadSummary::kStrandedMax ? a.stranded : ThreadSummary::kStrandedMax;
  StrandedId x[ThreadSummary::kStrandedMax], y[ThreadSummary::kStrandedMax];
  std::copy(a.strandedId, a.strandedId + k, x);
  std::copy(b.strandedId, b.strandedId + k, y);
  auto byTid = [](const StrandedId& p, const StrandedId& q) { return p.tid < q.tid; };
  std::sort(x, x + k, byTid);
  std::sort(y, y + k, byTid);
  for (int i = 0; i < k; ++i)
    if (!(x[i] == y[i])) {
      char p[200], q[200];
      formatStranded(x[i], p, sizeof(p));
      formatStranded(y[i], q, sizeof(q));
      std::snprintf(why, n, "{%s} -> {%s}", p, q);
      return false;
    }
  return true;
}

void doorbellCut() {
  // Between the ring cut and the thread cut the CP is still draining, and the ISRs it runs may need a guest lock:
  // a lock holder (or an exempt loop) must not park then - its publish is dropped and it runs on to the thread cut.
  if (!g_threadCut.load(std::memory_order_acquire) &&
      (t_exempt || gate().locks.load(std::memory_order_relaxed) > 0)) {
    g_drops.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  parkForever("doorbell");
}

uint64_t doorbellDrops() { return g_drops.load(std::memory_order_relaxed); }

// ── the CP drain point ─────────────────────────────────────────────────────────────────────────────────────────
bool cpDrainRequested() { return g_drainReq.load(std::memory_order_acquire); }

void cpParkDrained(const CpDrained& d) {
  g_drainInfo = d;
  g_drained.store(true, std::memory_order_release);
  parkForever("cp-drained");
}

// ── exit claim ─────────────────────────────────────────────────────────────────────────────────────────────────
bool claimExit() {
  int s = kRunning;
  if (g_state.compare_exchange_strong(s, kExiting)) return true;
  return s != kQuitting;  // already exiting: go on; a quit in progress owns the exit
}

bool parkPresenterForExit(uint32_t ms) {
  if (g_state.load(std::memory_order_acquire) != kExiting) return false;   // only on the exit claimExit() granted
  if (!sr::kernel::gpuCpActive() || !sr::kernel::gpuCpSnapshot().started) return true;   // no CP: nothing presents
  sr::kernel::gpuCpCutRing();
  g_drainReq.store(true, std::memory_order_release);
  sr::kernel::gpuCpQuitWake();
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  for (;;) {
    if (g_drained.load(std::memory_order_acquire)) break;
    if (std::chrono::steady_clock::now() >= until) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  // Astra (10:5x) #1: the CP's drain point hands its last frames to the PM4 render thread - "parked" only once that
  // thread finished them too (no present, dump or pipeline create after it), within what is left of the deadline
  const auto left = until - std::chrono::steady_clock::now();
  const auto leftMs = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
  return sr::gfx::pm4RenderThreadIdleForExit(uint32_t(leftMs > 0 ? leftMs : 0));
}

// ── families ───────────────────────────────────────────────────────────────────────────────────────────────────
void addFamily(const char* name, Printer p) {
  std::lock_guard<std::mutex> lk(g_famMu);
  if (g_familyN < kFamilyMax && p) g_families[g_familyN++] = Family{name, p};
}

int familyCount() {
  std::lock_guard<std::mutex> lk(g_famMu);
  return g_familyN;
}

// ── the coordinator ────────────────────────────────────────────────────────────────────────────────────────────
bool digestDiff(const Digest& a, const Digest& b, char* out, size_t n) {
  bool diff = false;
  size_t at = 0;
  if (out && n) out[0] = 0;
  const int m = a.n > b.n ? a.n : b.n;
  for (int i = 0; i < m; ++i) {
    const bool same = i < a.n && i < b.n && a.value[i] == b.value[i] &&
                      (a.name[i] == b.name[i] || (a.name[i] && b.name[i] && std::strcmp(a.name[i], b.name[i]) == 0));
    if (same) continue;
    diff = true;
    if (out && at + 1 < n) {
      const char* nm = i < b.n ? b.name[i] : a.name[i];
      const int w = std::snprintf(out + at, n - at, "%s%s(%llu->%llu)", at ? "," : "", nm ? nm : "?",
                                  (unsigned long long)(i < a.n ? a.value[i] : 0),
                                  (unsigned long long)(i < b.n ? b.value[i] : 0));
      if (w > 0) at = at + size_t(w) < n ? at + size_t(w) : n - 1;
    }
  }
  return diff;
}

namespace {
// One captured line: its tag, its shape (digit runs blanked) and its integer fields in order.
struct SnapLine {
  std::string tag, shape;
  std::vector<std::pair<std::string, std::string>> fields;  // key, digits
};

bool isTimeKey(const std::string& k) {
  static const char* const kUnits[] = {"us", "Us", "ms", "Ms", "ns", "Ns"};
  for (const char* u : kUnits) {
    const size_t n = std::strlen(u);
    if (k.size() >= n && k.compare(k.size() - n, n, u) == 0) return true;
  }
  return false;
}

std::vector<SnapLine> parseSnap(const std::string& text) {
  std::vector<SnapLine> out;
  size_t at = 0;
  while (at < text.size()) {
    size_t eol = text.find('\n', at);
    if (eol == std::string::npos) eol = text.size();
    const std::string line = text.substr(at, eol - at);
    at = eol + 1;
    if (line.empty()) continue;
    SnapLine s;
    if (line[0] == '[') {
      const size_t close = line.find(']');
      if (close != std::string::npos) s.tag = line.substr(0, close + 1);
    }
    // A timing line (the CP's wall-time split): every number in it is a time.
    if (s.tag.size() > 6 && (s.tag.compare(s.tag.size() - 6, 6, "_TIME]") == 0 ||
                             (s.tag.size() > 8 && s.tag.compare(s.tag.size() - 8, 8, "_TIMING]") == 0)))
      continue;
    for (size_t i = 0; i < line.size(); ++i) {
      const bool digit = line[i] >= '0' && line[i] <= '9';
      if (digit && (s.shape.empty() || s.shape.back() != '#')) s.shape += '#';
      else if (!digit) s.shape += line[i];
    }
    for (size_t eq = line.find('='); eq != std::string::npos; eq = line.find('=', eq + 1)) {
      size_t k0 = eq;
      while (k0 > 0) {
        const char c = line[k0 - 1];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')
          --k0;
        else
          break;
      }
      if (k0 == eq) continue;
      size_t v0 = eq + 1;
      if (v0 < line.size() && line[v0] == '+') ++v0;
      size_t v1 = v0;
      while (v1 < line.size() && line[v1] >= '0' && line[v1] <= '9') ++v1;
      if (v1 == v0) continue;
      const bool delim = v1 == line.size() || line[v1] == ' ' || line[v1] == ',' || line[v1] == ';' ||
                         line[v1] == '|' || line[v1] == ')';
      if (!delim) continue;  // a float, a unit, a list entry: not an integer field
      const std::string key = line.substr(k0, eq - k0);
      if (isTimeKey(key)) continue;
      s.fields.emplace_back(key, line.substr(v0, v1 - v0));
    }
    out.push_back(std::move(s));
  }
  return out;
}

void appendf(char* out, size_t n, size_t& at, const char* fmt, ...) {
  if (!out || at + 1 >= n) return;
  va_list ap;
  va_start(ap, fmt);
  const int w = std::vsnprintf(out + at, n - at, fmt, ap);
  va_end(ap);
  if (w > 0) at = at + size_t(w) < n ? at + size_t(w) : n - 1;
}
}  // namespace

int snapshotDiff(const std::string& snap, const std::string& check, char* out, size_t n, int* fields) {
  if (out && n) out[0] = 0;
  size_t at = 0;
  int diffs = 0, compared = 0;
  const std::vector<SnapLine> a = parseSnap(snap), b = parseSnap(check);
  // (shape, occurrence) -> index into a
  std::map<std::pair<std::string, int>, size_t> idx;
  std::map<std::string, int> occA, occB;
  for (size_t i = 0; i < a.size(); ++i) idx[{a[i].shape, occA[a[i].shape]++}] = i;
  for (const SnapLine& l : b) {
    const auto it = idx.find({l.shape, occB[l.shape]++});
    if (it == idx.end()) {
      ++diffs;
      appendf(out, n, at, "%s%s line only in the re-render", diffs > 1 ? "; " : "", l.tag.c_str());
      continue;
    }
    const SnapLine& s = a[it->second];
    for (size_t f = 0; f < l.fields.size() && f < s.fields.size(); ++f) {
      ++compared;
      if (l.fields[f].second == s.fields[f].second) continue;
      ++diffs;
      appendf(out, n, at, "%s%s %s(%s->%s)", diffs > 1 ? "; " : "", l.tag.c_str(), l.fields[f].first.c_str(),
              s.fields[f].second.c_str(), l.fields[f].second.c_str());
    }
  }
  if (fields) *fields = compared;
  return diffs;
}

namespace {
void qlog(const Hooks& h, bool err, const char* fmt, ...) {
  char buf[1400];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  h.log(h.user, err, buf);
}
}  // namespace

Result run(const Hooks& h) {
  Result r;
  const Config& c = h.cfg;
  const uint64_t t0 = h.nowMs(h.user);
  auto since = [&](uint64_t t) { return h.nowMs(h.user) - t; };
  if (!h.claim(h.user)) {
    qlog(h, false, "request ignored: the game is already exiting (no ack)");
    r.stage = "claim";
    r.reason = "already-exiting";
    return r;
  }
  const FrameInfo f0 = h.frames(h.user);
  g_allowStranded.store(c.allowStranded, std::memory_order_release);
  g_cpStopped.store(false, std::memory_order_release);
  h.ack(h.user);
  qlog(h, false,
       "request received: frames=%llu swaps=%llu vdswaps=%llu -> quiesce (drain<=%u ms, settle %u ms within %u ms, "
       "verify after %u ms)",
       (unsigned long long)f0.frames, (unsigned long long)f0.swaps, (unsigned long long)f0.vdswaps, c.drainMs,
       c.settleMs, c.settleMaxMs, c.settleMs);
  auto fail = [&](const char* stage, const char* reason, const char* detail) {
    const FrameInfo f = h.frames(h.user);
    qlog(h, true, "terminal snapshot FAILED stage=%s reason=%s frames=%llu vdswaps=%llu elapsed_ms=%llu%s%s", stage,
         reason, (unsigned long long)f.frames, (unsigned long long)f.vdswaps, (unsigned long long)since(t0),
         detail && *detail ? " | " : "", detail ? detail : "");
    r.code = kExitFailed;
    r.stage = stage;
    r.reason = reason;
    h.exit(h.user, kExitFailed);
    return r;
  };

  // 1. RING CUT: written under the CP's publish mutex - when it returns no doorbell publishes again.
  h.cutRing(h.user);
  qlog(h, false, "stage ring-cut: CP doorbells closed under the publish lock (the published write index is final)");

  // 2. CP DRAIN: everything published is consumed and performed; the CP parks.
  h.requestDrain(h.user);
  uint64_t ts = h.nowMs(h.user);
  CpDrained d{};
  Drain st;
  while ((st = h.drainState(h.user, &d)) == Drain::Draining) {
    if (since(ts) >= c.drainMs) {
      char det[160];
      std::snprintf(det, sizeof(det), "rptr=%u wptr=%u swaps=%llu packets=%llu (the CP did not reach its idle point)",
                    d.rptr, d.wptr, (unsigned long long)d.swaps, (unsigned long long)d.packets);
      return fail("cp-drain", "drain-timeout", det);
    }
    h.sleepMs(h.user, 2);
  }
  if (st == Drain::Parked)
    qlog(h, false,
         "stage cp-drain: drained rptr=%u wptr=%u swaps=%llu packets=%llu doorbells=%llu rl_pending=%u (executor flush "
         "%u ms) in %llu ms",
         d.rptr, d.wptr, (unsigned long long)d.swaps, (unsigned long long)d.packets, (unsigned long long)d.doorbells,
         d.rlPending, d.flushMs, (unsigned long long)since(ts));
  else
    qlog(h, false, "stage cp-drain: no command processor thread (nothing to drain)");
  // From here nothing writes a fence or a read pointer again: a thread still inside an XDK GPU wait can be stranded.
  g_cpStopped.store(true, std::memory_order_release);
  // native/quit-ringspace: the DIAGNOSTIC thread-cut delay (Config::cutDelayMs; 0 in every evidence run).
  if (c.cutDelayMs) {
    qlog(h, false, "stage diag-cut-delay: the thread cut held back %u ms (SR_QUIT_DIAG_CUT_DELAY_MS, a diagnostic: lock "
                   "holders run on against the parked CP)", c.cutDelayMs);
    h.sleepMs(h.user, c.cutDelayMs);
  }

  // 3. THREAD CUT: guest threads park at their next import entry / exit, host loops at their safe point / guest call.
  h.cutThreads(h.user);
  qlog(h, false, "stage thread-cut: guest threads park at their next kernel import (entry or exit) unless they hold "
                 "a guest lock, host loops at their safe point or guest call");

  // 4. SETTLE: every thread ACKNOWLEDGED (parked, or blocked in a wait import holding no lock, or a host loop between
  // guest calls) and the producer digest unchanged, together, for settleMs.
  h.threadsBegin(h.user);
  Digest a;
  h.digest(h.user, &a);
  ts = h.nowMs(h.user);
  uint64_t stableSince = ts;
  char moved[512] = "", unack[200] = "";
  ThreadSummary th;
  // The settle deadline: the step's own check is the record (its rows logged), judged BEFORE the census window is reset
  // (native/quit-drain) - it covers that step's window, so a thread that left and re-entered a GPU wait during it is
  // unacknowledged, never "stranded" over a window just begun.
  auto settleFail = [&](const ThreadSummary& u) {
    char det[900];
    std::snprintf(det, sizeof(det), "%s%s%s%s | threads=%d parked=%d waiting=%d host_idle=%d stranded=%d unacked=%d",
                  u.unacked ? "not acknowledged: " : "", u.unacked ? u.firstUnacked : "",
                  u.unacked && moved[0] ? " | " : "", moved[0] ? (std::string("moved: ") + moved).c_str() : "",
                  u.threads, u.parked, u.waiting, u.hostIdle, u.stranded, u.unacked);
    return fail("settle", u.unacked ? "threads-not-quiescent" : "producers-not-quiesced", det);
  };
  for (;;) {
    h.sleepMs(h.user, c.stepMs);
    // native/quit-drain round 3 (Astra round 2 P2): every check consumes each waiter's progress baseline (strandedWait
    // condition 5), so no check runs straight after another: the check that ends a stable window IS the record (one
    // line per thread not parked), and so is the one at the deadline. A stranded waiter sharing one CPU with this
    // thread runs only while it sleeps; a second check right after the first would find it unmoved, restart the window,
    // and end a valid stranded wait FAILED.
    const bool deadline = since(ts) >= c.settleMaxMs;
    const bool ripe = since(stableSince) >= c.settleMs;
    const ThreadSummary t = h.threadsCheck(h.user, ripe || deadline);
    Digest b;
    h.digest(h.user, &b);
    char m[512];
    const bool mv = digestDiff(a, b, m, sizeof(m));
    if (mv) {
      std::snprintf(moved, sizeof(moved), "%s", m);
      a = b;
    }
    if (t.unacked) std::snprintf(unack, sizeof(unack), "%s", t.firstUnacked);
    if (mv || t.unacked) {
      if (deadline) return settleFail(t);
      stableSince = h.nowMs(h.user);
      h.threadsBegin(h.user);  // a waiter's CPU must be idle over the whole stable window
    } else if (ripe) {
      th = t;  // the record: this check, after a whole window of acknowledged checks
      break;
    }
    if (deadline) return settleFail(t);
  }
  // native/quit-drain round 2: every stranded thread is named, or the quit FAILS - never a silent cap.
  if (th.strandedOverflow) {
    char det[160];
    std::snprintf(det, sizeof(det), "stranded=%d: more stranded threads than the census names (%d)", th.stranded,
                  ThreadSummary::kStrandedMax);
    return fail("settle", "stranded-overflow", det);
  }
  qlog(h, false,
       "stage settle: every thread acknowledged and the producer counters (%d) unchanged for %llu ms, %llu ms after "
       "the thread cut; threads=%d parked=%d waiting=%d host_idle=%d stranded=%d untracked=%d holding_locks=%d "
       "doorbell_drops=%llu dropped_fences=%u dropped_progress=%u%s%s%s%s",
       a.n, (unsigned long long)since(stableSince), (unsigned long long)since(ts), th.threads, th.parked, th.waiting,
       th.hostIdle, th.stranded, th.untracked, th.holding, (unsigned long long)th.drops, th.droppedFences, th.droppedProgress,
       moved[0] ? " | settled after: " : "", moved, unack[0] ? " | acknowledged after: " : "", unack);
  // native/quit-drain: never silent - each stranded thread named (its wait, device, target, the fence the parked CP
  // last wrote, the residence and spin sequence numbers, the locks it holds); the count is on the complete line too.
  for (int i = 0; i < th.stranded; ++i) {
    char sl[200];
    formatStranded(th.strandedId[i], sl, sizeof(sl));
    qlog(h, false, "stranded %s (an XDK GPU wait the parked CP can never end: a publish the ring cut dropped)", sl);
  }

  // 5. TERMINAL SNAPSHOT: every family's own printer, why = "terminal", captured.
  std::string snap;
  const int fams = h.printFamilies(h.user, "terminal", &snap, false);

  // 6. VERIFY: settleMs later, still quiescent, and a capture-only re-render equal on every integer field printed.
  h.sleepMs(h.user, c.settleMs);
  const ThreadSummary tv = h.threadsCheck(h.user, false);
  if (tv.unacked) return fail("verify", "threads-not-quiescent", tv.firstUnacked);
  {  // the SAME stranded threads, identity by identity (round 2: a count match is not a set match)
    char why[460], det[520];
    if (!sameStranded(th, tv, why, sizeof(why))) {
      std::snprintf(det, sizeof(det), "the stranded threads changed after the snapshot: %s", why);
      return fail("verify", "threads-not-quiescent", det);
    }
  }
  std::string again;
  h.printFamilies(h.user, "terminal", &again, true);
  Digest after;
  h.digest(h.user, &after);
  char m2[900];
  int fields = 0;
  if (snapshotDiff(snap, again, m2, sizeof(m2), &fields)) return fail("verify", "counters-moved-after-snapshot", m2);
  if (digestDiff(a, after, m2, sizeof(m2))) return fail("verify", "counters-moved-after-snapshot", m2);
  if (fields == 0) return fail("verify", "snapshot-empty", "the families printed no integer field to verify");

  const FrameInfo f = h.frames(h.user);
  char tail[16];
  if (f.tail < 0) std::snprintf(tail, sizeof(tail), "n/a");
  else std::snprintf(tail, sizeof(tail), "%d", f.tail);
  // stranded= LAST: tools/perf/terminal_snapshot.py's pattern for the fields before it predates it.
  qlog(h, false,
       "terminal snapshot complete frames=%llu swaps=%llu vdswaps=%llu tail=%s rl_pending=%u threads=%d parked=%d "
       "waiting=%d host_idle=%d families=%d fields=%d elapsed_ms=%llu stranded=%d",
       (unsigned long long)f.frames, (unsigned long long)f.swaps, (unsigned long long)f.vdswaps, tail, d.rlPending,
       th.threads, th.parked, th.waiting, th.hostIdle, fams, fields, (unsigned long long)since(t0), th.stranded);
  r.code = kExitComplete;
  r.stage = "complete";
  h.exit(h.user, kExitComplete);
  return r;
}

// ── production hooks ───────────────────────────────────────────────────────────────────────────────────────────
namespace {

int g_pm4Tail = -1;       // pm4cov::terminal's answer at the snapshot (-1 = coverage off)
bool g_pm4TailTaken = false;

void registerBuiltins() {
  static std::once_flag once;
  std::call_once(once, [] {
    // Built-ins first, in a fixed order; addFamily() callers (other lanes) follow.
    std::lock_guard<std::mutex> lk(g_famMu);
    const Family builtins[] = {
        {"gpu_cp", [](const char* why) { sr::kernel::gpuCpCensusLine(why); }},
        {"pm4_cov",
         [](const char* why) {
           // The first call folds the drained partial frame in (its F line + tail); the verify's re-render finds
           // nothing left to fold and prints the same CUM.
           const int t = sr::gfx::pm4cov::terminal(why);
           if (!g_pm4TailTaken) {
             g_pm4Tail = t;
             g_pm4TailTaken = true;
           }
         }},
        {"native_flush", [](const char* why) { sr::native::flush::reportLine(why); }},
        {"native_exec", [](const char* why) { sr::native::exec::report(why); }},
        {"native_meter", [](const char* why) { sr::native::meter::report(why); }},
        {"resource_memory", [](const char*) {
           namespace R = sr::gfx::memres;
           if (!R::resourceMode()) return;
           // The verifier prints this family twice. Keep the time denominator
           // stable while rereading ownership counters to detect later notes.
           static const uint64_t elapsed = R::resourceElapsedMilliseconds();
           const uint32_t generation = R::resourceMemory().generation();
           const uint32_t reservations = generation - R::kFirstUseGen;
           SR_LOG_BOOT(sr::LogSeverity::Info,
                       "[MEM_RES_NATIVE] terminal=1 generation=%u reservations=%u remaining=%u elapsed_ms=%llu "
                       "generations_per_second=%.6f", generation, reservations, UINT32_MAX - generation,
                       static_cast<unsigned long long>(elapsed), elapsed ? 1000.0 * reservations / elapsed : 0.0);
           // The exact CPU-write patches (kernel/native_resource_stamps.hpp): no generation each.
           const auto patches = R::resourceMemory().patchStats();
           SR_LOG_BOOT(sr::LogSeverity::Info,
                       "[MEM_RES_NATIVE] patches terminal=1 notes=%llu dwords=%llu refused=%llu taken=%llu "
                       "superseded=%llu",
                       static_cast<unsigned long long>(patches.notes), static_cast<unsigned long long>(patches.dwords),
                       static_cast<unsigned long long>(patches.refused), static_cast<unsigned long long>(patches.taken),
                       static_cast<unsigned long long>(patches.superseded));
         }},
#if SR_ORACLE
        {"resource_checker", [](const char*) { sr::gfx::memres::resourceCheckTerminal(); }},
#endif
        {"audio_native",
         [](const char* why) {
           char line[400];
           if (sr::kernel::audioNativeCensusLine(line, sizeof(line)) > 0)
             SR_LOG_BOOT(sr::LogSeverity::Info, "[AUDIO_NATIVE] %s %s", why, line);
         }},
        {"p2_trace",
         [](const char* why) {
           if (sr::native::p2trace::g_on) sr::native::p2trace::finalLine(why);
         }},
        {"ab_drain", [](const char* why) { sr::abt::drainStatsLine(why); }},
        {"gauges", [](const char* why) { sr::kernel::gaugeCensus(why); }},
    };
    // Keep any family a lane registered before the first quit after the built-ins.
    Family later[kFamilyMax];
    const int laterN = g_familyN;
    for (int i = 0; i < laterN; ++i) later[i] = g_families[i];
    g_familyN = 0;
    for (const Family& f : builtins)
      if (g_familyN < kFamilyMax) g_families[g_familyN++] = f;
    for (int i = 0; i < laterN && g_familyN < kFamilyMax; ++i) g_families[g_familyN++] = later[i];
  });
}

uint64_t pNow(void*) { return GetTickCount64(); }
void pSleep(void*, uint32_t ms) { Sleep(ms); }
void pLog(void*, bool error, const char* body) {
  // The format string carries the tag, so a player-profile log keeps the line (app/log.cpp kMilestoneTags).
  SR_LOG_BOOT(error ? sr::LogSeverity::Error : sr::LogSeverity::Info, "[QUIT] %s", body);
}

void deadlineThread(uint32_t ms) {
  Sleep(ms);
  char line[200];
  std::snprintf(line, sizeof(line),
                "[QUIT] terminal snapshot FAILED stage=deadline reason=hard-deadline (%u ms, SR_QUIT_HARD_MS) - "
                "TerminateProcess(%d)",
                ms, kExitDeadline);
  sr::Log::tryWriteLine(sr::LogSeverity::Error, line);
  TerminateProcess(GetCurrentProcess(), UINT(kExitDeadline));
}

bool pClaim(void*) {
  int s = kRunning;
  if (!g_state.compare_exchange_strong(s, kQuitting)) return false;
  std::thread(deadlineThread, envMs(kHard)).detach();
  return true;
}
void pAck(void*) {
  if (g_ack) SetEvent(g_ack);
}
void pCutRing(void*) { sr::kernel::gpuCpCutRing(); }
void pRequestDrain(void*) {
  g_drainReq.store(true, std::memory_order_release);
  sr::kernel::gpuCpQuitWake();
}
Drain pDrainState(void*, CpDrained* out) {
  if (g_drained.load(std::memory_order_acquire)) {
    *out = g_drainInfo;
    return Drain::Parked;
  }
  const sr::kernel::GpuCpSnapshot s = sr::kernel::gpuCpSnapshot();
  if (!sr::kernel::gpuCpActive() || !s.started) return Drain::NoCp;
  out->rptr = s.rptr;
  out->wptr = s.wptr;
  out->swaps = s.swaps;
  out->packets = s.packets;
  out->doorbells = s.doorbells;
  return Drain::Draining;
}
void pCutThreads(void*) { g_threadCut.store(true, std::memory_order_seq_cst); }

void pDigest(void*, Digest* d) {
  const sr::kernel::GpuCpSnapshot s = sr::kernel::gpuCpSnapshot();
  d->add("cp.doorbells", s.doorbells);
  d->add("cp.packets", s.packets);
  d->add("cp.swaps", s.swaps);
  d->add("cp.isr", s.isrCalls);
  d->add("cp.rptr", s.rptr);
  d->add("cp.wptr", s.wptr);
  d->add("cp.rl_pending", sr::kernel::gpuCpRlPending());
  d->add("vdswaps", sr::kernel::getVdSwapCount());
  d->add("flush.activity", sr::native::flush::activityTotal());
  d->add("ringless.items", sr::kernel::ringless::itemsTotal());
#if SR_ORACLE
  uint64_t col[16] = {};
  const int n = sr::native::meter::columnCount();
  sr::native::meter::snapshotColumns(col, n < 16 ? n : 16);
  for (int i = 0; i < n && i < 16; ++i) d->add(sr::native::meter::columnName(i), col[i]);
#endif
}

// ── the thread census: every registered guest-running thread and every loop that reached a gate ────────────────
// A consistent read of a slot's wait mark (the owner's seqlock, quit.hpp gpuWaitWrite); false = the owner kept writing.
struct MarkSnap {
  uint32_t kind = 0, dev = 0, arg = 0, phase = 0, gen = 0, rec = 0;
  uint64_t seq = 0, spin = 0;
};
bool readMark(const GateSlot* g, MarkSnap* m) {
  if (!g) return false;
  for (int i = 0; i < 64; ++i) {
    const uint32_t v1 = g->gpuWaitVer.load(std::memory_order_acquire);
    if (v1 & 1u) {
      YieldProcessor();
      continue;
    }
    m->kind = g->gpuWait.load(std::memory_order_relaxed);
    m->dev = g->gpuWaitDev.load(std::memory_order_relaxed);
    m->arg = g->gpuWaitArg.load(std::memory_order_relaxed);
    m->gen = g->gpuWaitGen.load(std::memory_order_relaxed);
    m->rec = g->gpuWaitRec.load(std::memory_order_relaxed);
    m->phase = g->gpuWaitPhase.load(std::memory_order_relaxed);
    m->seq = g->gpuWaitSeq.load(std::memory_order_relaxed);
    m->spin = g->gpuSpinSeq.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g->gpuWaitVer.load(std::memory_order_relaxed) == v1) return true;
  }
  return false;
}

struct Row {
  uint32_t tid = 0;
  HANDLE h = nullptr;
  uint64_t cpu0 = 0;
  bool fresh = false;  // appeared after the window began
  char name[32] = {};
  // native/quit-drain: the XDK GPU wait it was inside when the window began, and its spin loop's call count then
  bool mark0 = false;
  MarkSnap m0;
  uint64_t spinsLast = 0;  // the spin loop's call count at the previous check (the window's start for the first)
};
constexpr int kRowMax = 256;
Row g_rows[kRowMax];
int g_rowN = 0;

uint64_t threadCpu100ns(HANDLE h) {
  FILETIME c, e, k, u;
  if (!h || !GetThreadTimes(h, &c, &e, &k, &u)) return 0;
  return (uint64_t(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (uint64_t(u.dwHighDateTime) << 32 | u.dwLowDateTime);
}

bool registered(uint32_t tid) {
  for (int i = 0; i < 64; ++i)
    if (*reinterpret_cast<const volatile uint32_t*>(&g_debugThreadRegistry[i].tid) == tid) return true;
  return false;
}

// The threads the census examines: the registry's (every guest-running thread) and every gate slot with a role (the
// exempt loops - the CP, VD_ISR, the audio worker, which need not be registered - and GuestCaller host loops).
void addRow(uint32_t tid, const char* name, bool fresh) {
  if (!tid) return;
  for (int i = 0; i < g_rowN; ++i)
    if (g_rows[i].tid == tid) return;
  if (g_rowN >= kRowMax) return;
  Row& r = g_rows[g_rowN++];
  r = Row{};
  r.tid = tid;
  if (name) std::snprintf(r.name, sizeof(r.name), "%s", name);
  r.h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, tid);
  r.cpu0 = threadCpu100ns(r.h);
  r.fresh = fresh;
  if (const GateSlot* g = findGate(tid)) {
    r.mark0 = readMark(g, &r.m0);
    r.spinsLast = g->gpuSpins.load(std::memory_order_acquire);
  }
}

const char* gpuWaitName(uint32_t kind) {
  return kind == kGpuWaitFence         ? "GpuBlockOnFence"
         : kind == kGpuWaitRingSpace   ? "GpuRingWaitForSpace"
         : kind == kGpuWaitReadPtrWrap ? "GpuRingWaitReadPtrWrap"
                                       : "?";
}

// native/quit-drain: STRANDED = every one of, at this check, against the window's start:
//  (1) the CP drain is over - the CP parked at its drain point, or there is none: nothing writes a fence again;
//  (2) the thread is in GpuBlockOnFence's SPIN loop (quit.hpp GpuWaitPhase) - not its PRE phase (the fence test, the
//      open-submission early return F == next with dev+0x3290 set, the preliminary flush), not EXITED (a
//      GpuRingWaitProgress that returned 0);
//  (3) the SAME residence and the SAME spin as when the window began (sequence numbers unique process-wide: a wait
//      left and re-entered, a nested wait in between, a new spin - all a thread that ran);
//  (4) that spin began AFTER the ring cut (its spin number above g_cutWaitSeq): a wait already spinning before the
//      request - a fence lost earlier - is a hang the quit did not cause and keeps FAILED;
//  (5) the spin loop is turning - its GpuRingWaitProgress calls advanced since the previous check (positive evidence
//      at every check that it loops, never a thread that stopped at an exit or was descheduled there);
//  (6) GpuBlockOnFence's own test still false - (next - F) < (next - completed), next = *(dev+0x2A1C), completed =
//      *(*(dev+0x2A10)) - and its other exit closed: byte dev+0x2A39 & 2 (the no-GPU mode, where GpuRingWaitProgress
//      returns 0) clear;
//  (7) {dev, F} was in a publish the ring cut DROPPED (recordDroppedFence): the parked CP will never write it.
// Such a thread spins forever on a value only the CP could write, holding its locks; no counter it could move moves.
// native/quit-ringspace: GpuRingWaitForSpace (dev, end r4, generation r5) has the same rule with (1)-(5) unchanged
// (its own spin loop: the call whose record is the wait's, quit.hpp gpuSpinSite) and, for (6) and (7):
//  (6') its own test still false on the progress word the parked CP last wrote - !ringSpaceFree(*(fb+4), end, gen) -
//      and the same no-GPU exit closed (the CPU writes fb+4 only in that mode);
//  (7') some progress word a publish the ring cut DROPPED carried (recordDroppedProgress) would pass that test: had the
//      CP executed the dropped publish, the wait would have ended. (The dropped closes follow the last executed one and
//      stay within one lap of it - the writer cannot reserve past the frozen progress word - so the mod-4 generation
//      test cannot alias.)
// GpuRingWaitReadPtrWrap keeps the old classification (no rule). Returns false with *whyNot = the first condition that
// fails (a fence or ring-space wait; nullptr for any other wait), true with *id filled.
bool strandedWait(const GateSlot* g, Row& r, int locks, StrandedId* id, const char** whyNot) {
  *whyNot = nullptr;
  if (!g) return false;  // no gate slot: in no wait
  MarkSnap m;
  if (!readMark(g, &m)) {
    *whyNot = "its wait mark could not be read consistently";
    return false;
  }
  const uint64_t spins = g->gpuSpins.load(std::memory_order_acquire);
  const bool turned = spins != r.spinsLast;  // since the previous check (every check moves the mark on)
  r.spinsLast = spins;
  if (m.kind != kGpuWaitFence && m.kind != kGpuWaitRingSpace) return false;  // ReadPtrWrap (or none): no rule
  const bool fence = m.kind == kGpuWaitFence;
  auto no = [&](const char* w) {
    *whyNot = w;
    return false;
  };
  if (!g_cpStopped.load(std::memory_order_acquire)) return no("the CP drain is not over");
  if (m.phase == kGpuPhasePre)
    return no(fence ? "not in its spin loop (the fence test, the early return or the flush)"
                    : "not in its spin loop (its space test)");
  if (m.phase != kGpuPhaseSpin) return no("its spin loop exited (GpuRingWaitProgress returned 0)");
  if (!r.mark0 || r.m0.kind != m.kind || r.m0.seq != m.seq || r.m0.phase != m.phase || r.m0.spin != m.spin ||
      r.m0.dev != m.dev || r.m0.arg != m.arg || r.m0.gen != m.gen || r.m0.rec != m.rec)
    return no("the wait or its spin changed during the window");
  const uint64_t cut = g_cutWaitSeq.load(std::memory_order_acquire);
  if (cut == kNoCut || m.spin <= cut) return no("its spin began before the ring cut");
  if (!turned) return no("its spin loop made no progress since the last check");
  const uint32_t dev = m.dev, target = m.arg;
  const uint32_t fb = dev ? sr::kernel::readGuestU32BE(dev + 0x2A10u) : 0u;
  if (!fb) return no("no fence block");
  if ((sr::kernel::readGuestU32BE(dev + 0x2A38u) >> 16) & 2u) return no("the no-GPU mode is set (dev+0x2A39 & 2)");
  uint32_t completed = 0, next = 0;
  if (fence) {
    next = sr::kernel::readGuestU32BE(dev + 0x2A1Cu);
    completed = sr::kernel::readGuestU32BE(fb);
    if (!(next - target < next - completed)) return no("the fence passed (its own test is true)");
    if (!droppedFence(dev, target)) return no("its fence was in no publish the ring cut dropped");
  } else {  // native/quit-ringspace: target = the space's end (r4), next = its generation (r5)
    next = m.gen;
    completed = sr::kernel::readGuestU32BE(fb + 4u);  // the progress word the parked CP last wrote
    if (ringSpaceFree(completed, target, next)) return no("the space is free (its own test is true)");
    if (!droppedProgressFrees(dev, target, next)) return no("no progress word the ring cut dropped frees its space");
  }
  *id = StrandedId{};
  id->tid = r.tid;
  id->kind = m.kind;
  id->dev = dev;
  id->target = target;
  id->completed = completed;
  id->next = next;
  id->seq = m.seq;
  id->spin = m.spin;
  id->locks = locks;
  return true;
}

void collectRows(bool fresh) {
  for (int i = 0; i < 64; ++i) {
    const sr::kernel::GuestThreadSlot& s = g_debugThreadRegistry[i];
    const uint32_t tid = *reinterpret_cast<const volatile uint32_t*>(&s.tid);
    if (!tid) continue;
    char nm[32];
    std::memcpy(nm, s.name, sizeof(nm));
    nm[sizeof(nm) - 1] = 0;
    addRow(tid, nm[0] ? nm : nullptr, fresh);
  }
  for (GateSlot& g : g_gates) {
    const uint32_t tid = g.tid.load(std::memory_order_acquire);
    const char* role = g.role.load(std::memory_order_acquire);
    if (tid && role) addRow(tid, role, fresh);
  }
}

void pThreadsBegin(void*) {
  for (int i = 0; i < g_rowN; ++i)
    if (g_rows[i].h) CloseHandle(g_rows[i].h);
  g_rowN = 0;
  collectRows(false);
}

ThreadSummary pThreadsCheck(void*, bool logRows) {
  collectRows(true);  // a thread that appeared since the window began is not acknowledged (it restarts the window)
  ThreadSummary t;
  t.drops = doorbellDrops();
  t.droppedFences = droppedFenceCount();
  t.droppedProgress = droppedProgressCount();
  for (int i = 0; i < g_rowN; ++i) {
    Row& r = g_rows[i];
    if (!r.h || WaitForSingleObject(r.h, 0) == WAIT_OBJECT_0) continue;  // exited (or never openable)
    t.threads++;
    const GateSlot* g = findGate(r.tid);
    const int locks = g ? g->locks.load(std::memory_order_acquire) : 0;
    if (locks > 0) t.holding++;
    const char* where = parkedWhere(r.tid);
    if (where) {
      t.parked++;
      continue;
    }
    const char* role = g ? g->role.load(std::memory_order_acquire) : nullptr;
    const char* imp = g ? g->import.load(std::memory_order_acquire) : nullptr;
    const int calls = g ? g->guestCalls.load(std::memory_order_acquire) : 0;
    const double cpuMs = double(threadCpu100ns(r.h) - r.cpu0) / 1e4;
    const bool hostLoop = role && std::strcmp(role, kHostLoop) == 0;
    const char* state = "unacked";
    char why[300] = "", strand[200] = "";
    StrandedId sid;
    const char* whyNot = nullptr;
    if (r.fresh) {
      std::snprintf(why, sizeof(why), "appeared during the settle");
    } else if (!imp && strandedWait(g, r, locks, &sid, &whyNot)) {
      // native/quit-drain: a spin in an XDK GPU wait the parked CP can never end (strandedWait's conditions)
      formatStranded(sid, strand, sizeof(strand));
      if (g_allowStranded.load(std::memory_order_acquire)) {
        state = "stranded";
        std::snprintf(why, sizeof(why), "%s", strand);
      } else {
        std::snprintf(why, sizeof(why), "stranded (SR_QUIT_STRANDED=0: not acknowledged) %s", strand);
      }
    } else if (imp && (cpuMs < 1.0 || isLockAcquire(imp))) {
      // Blocked inside an import (a wait, a contended lock, host I/O), or spinning for a guest lock: whatever it
      // holds, when the import returns its exit gate parks it before a single guest instruction - an exempt loop's
      // too, since the gates arm only after the CP drain. Any lock it holds stays held: whoever wants it blocks in
      // its own acquire import and is acknowledged the same way.
      state = "waiting";
    } else if (role && !hostLoop) {
      std::snprintf(why, sizeof(why), "exempt loop not at its safe point");
    } else if (hostLoop && calls == 0 && !imp) {
      state = "host-idle";
    } else if (imp) {
      std::snprintf(why, sizeof(why), "busy in %s (in flight: it parks at the exit)", imp);
    } else {
      const uint32_t w = g ? g->gpuWait.load(std::memory_order_acquire) : 0u;  // native/quit-drain: name the wait
      std::snprintf(why, sizeof(why), "running guest code (no gate reached)%s%s%s%s%s", w ? " in " : "",
                    w ? gpuWaitName(w) : "", whyNot ? " (not stranded: " : "", whyNot ? whyNot : "",
                    whyNot ? ")" : "");
    }
    if (std::strcmp(state, "waiting") == 0) t.waiting++;
    else if (std::strcmp(state, "host-idle") == 0) t.hostIdle++;
    else if (std::strcmp(state, "stranded") == 0) {
      if (t.stranded < ThreadSummary::kStrandedMax) t.strandedId[t.stranded] = sid;
      else t.strandedOverflow = true;  // counted, and run() FAILS: never a silent cap
      t.stranded++;
    } else {
      t.unacked++;
      if (!t.firstUnacked[0])
        std::snprintf(t.firstUnacked, sizeof(t.firstUnacked), "tid=%u name=%s %s", r.tid, r.name[0] ? r.name : "-",
                      why);
    }
    if (logRows)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[QUIT] thread tid=%u name=%s state=%s%s%s%s%s locks=%d cpu_ms=%.1f", r.tid,
                  r.name[0] ? r.name : "-", state, imp ? " import=" : "", imp ? imp : "", why[0] ? " - " : "", why,
                  locks, cpuMs);
  }
  // Gate slots of threads neither registered nor a loop (a host thread that called an import thunk): reported.
  for (GateSlot& g : g_gates) {
    const uint32_t tid = g.tid.load(std::memory_order_acquire);
    if (!tid || g.role.load(std::memory_order_acquire) || registered(tid)) continue;
    HANDLE h = OpenThread(SYNCHRONIZE, FALSE, tid);
    if (h) {
      if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) t.untracked++;
      CloseHandle(h);
    }
  }
  return t;
}

FrameInfo pFrames(void*) {
  FrameInfo f;
  const sr::kernel::GpuCpSnapshot s = sr::kernel::gpuCpSnapshot();
  f.swaps = s.swaps;
  f.vdswaps = sr::kernel::getVdSwapCount();
  f.frames = s.started ? s.swaps : f.vdswaps;
  f.tail = g_pm4Tail;
  return f;
}

void captureSink(void* user, const char* message) {
  std::string& s = *static_cast<std::string*>(user);
  s += message;
  s += '\n';
}

int pPrintFamilies(void*, const char* why, std::string* capture, bool suppress) {
  registerBuiltins();
  Family fs[kFamilyMax];
  int n = 0;
  {
    std::lock_guard<std::mutex> lk(g_famMu);
    n = g_familyN;
    for (int i = 0; i < n; ++i) fs[i] = g_families[i];
  }
  if (capture) sr::Log::setThreadCapture(captureSink, capture, suppress);
  for (int i = 0; i < n; ++i) fs[i].print(why);
  if (capture) sr::Log::setThreadCapture(nullptr, nullptr, false);
  return n;
}

void pExit(void*, int code) {
  // The window-close exit (app/main.cpp hostExitNow): host files first, then quick_exit - no static destructors,
  // no CRT teardown under the parked guest threads.
  input_recorder::shutdown();
  // step 4 (Astra 10:2x #5 / #6): the producers are already quiesced here (ring cut, CP drained, threads parked); the
  // dump queue stops admitting, then each drain is bounded. A drain that timed out leaves a writer that may hold a
  // C stream's lock: no global fflush then (stdio_guard's own flush is bounded).
  sr::gfx::dumpq::closeAdmission();
  // fix12 (Astra on fix11): the PM4 render thread may still finish the CP's last frames - the same bounded idle
  // check as parkPresenterForExit, so no present, dump or pipeline create runs past the global flush
  const bool idle = sr::gfx::pm4RenderThreadIdleForExit(1000);
  const bool dumps = sr::gfx::dumpq::drainAtExit(10000);  // step 4 item 4(c): queued present-dump BMPs
  const bool pso = sr::gfx::psoShutdown(2000);            // step 4 2k: the warm-up stopped, the last manifest write
  sr::Log::shutdown();
  if (idle && dumps && pso) std::fflush(nullptr);
  sr::stdio_guard::flush(500);
  std::quick_exit(code);
}

void watcherMain() {
  for (;;) {
    if (WaitForSingleObject(g_req, INFINITE) != WAIT_OBJECT_0) return;
    if (g_testRequest) {
      g_testRequest(g_testUser);
      ResetEvent(g_req);  // tests: one call per request
      continue;
    }
    run(productionHooks());
    return;  // a failed claim (the game is exiting): nothing more to do
  }
}

bool createChannel(const char* instanceId, uint32_t pid) {
  g_reqName = requestEventName(instanceId, pid);
  g_ackName = ackEventName(instanceId, pid);
  g_req = CreateEventA(nullptr, TRUE, FALSE, g_reqName.c_str());
  const bool reqExisted = g_req && GetLastError() == ERROR_ALREADY_EXISTS;
  g_ack = CreateEventA(nullptr, TRUE, FALSE, g_ackName.c_str());
  const bool ackExisted = g_ack && GetLastError() == ERROR_ALREADY_EXISTS;
  if (!g_req || !g_ack) {
    if (g_req) CloseHandle(g_req);
    if (g_ack) CloseHandle(g_ack);
    g_req = g_ack = nullptr;
    return false;
  }
  // A stale object under our own PID-scoped name cannot be a live harness's: start both unsignalled.
  if (reqExisted) ResetEvent(g_req);
  if (ackExisted) ResetEvent(g_ack);
  return true;
}

}  // namespace

Hooks productionHooks() {
  Hooks h;
  h.nowMs = pNow;
  h.sleepMs = pSleep;
  h.log = pLog;
  h.claim = pClaim;
  h.ack = pAck;
  h.cutRing = pCutRing;
  h.requestDrain = pRequestDrain;
  h.drainState = pDrainState;
  h.cutThreads = pCutThreads;
  h.digest = pDigest;
  h.threadsBegin = pThreadsBegin;
  h.threadsCheck = pThreadsCheck;
  h.frames = pFrames;
  h.printFamilies = pPrintFamilies;
  h.exit = pExit;
  h.cfg.drainMs = envMs(kDrain);
  h.cfg.settleMs = envMs(kSettle);
  h.cfg.settleMaxMs = envMs(kSettleMax);
  h.cfg.allowStranded = strandedAllowedByEnv();
  h.cfg.cutDelayMs = envMs(kCutDelay);
  return h;
}

void init() {
  static bool s_done = false;
  if (s_done) return;
  s_done = true;
  registerBuiltins();
  const char* inst = std::getenv("SR_INSTANCE_ID");
  const char* off = std::getenv("SR_QUIT_CHANNEL");
  if (!sr::kernel::automation::policy().automated || (off && off[0] == '0')) {
    SR_LOG_BOOT(sr::LogSeverity::Info, "[QUIT] channel off (%s)",
                sr::kernel::automation::policy().automated ? "SR_QUIT_CHANNEL=0" : "not an automated run");
    return;
  }
  if (!createChannel(inst, GetCurrentProcessId())) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[QUIT] channel %s could not be created (error %lu): no graceful quit",
                requestEventName(inst, GetCurrentProcessId()).c_str(), GetLastError());
    return;
  }
  std::thread(watcherMain).detach();
  std::string rejected;
  const uint32_t drain = envMs(kDrain, &rejected), settle = envMs(kSettle, &rejected),
                 settleMax = envMs(kSettleMax, &rejected), hard = envMs(kHard, &rejected),
                 cutDelay = envMs(kCutDelay, &rejected);
  char diag[96] = "";
  if (cutDelay)
    std::snprintf(diag, sizeof(diag), " - DIAGNOSTIC SR_QUIT_DIAG_CUT_DELAY_MS=%u: the thread cut held back", cutDelay);
  SR_LOG_BOOT(rejected.empty() ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
              "[QUIT] channel %s ready (ack %s; drain<=%u ms settle %u ms within %u ms, deadline %u ms; %d families)%s%s%s%s",
              g_reqName.c_str(), g_ackName.c_str(), drain, settle, settleMax, hard, familyCount(),
              rejected.empty() ? "" : " - out-of-range or malformed, default used: ", rejected.c_str(),
              strandedAllowedByEnv() ? "" : " - SR_QUIT_STRANDED=0: a stranded GPU wait fails the quit", diag);
}

// ── test seams ─────────────────────────────────────────────────────────────────────────────────────────────────
void resetForTest() {
  g_ringCut.store(false, std::memory_order_seq_cst);
  g_threadCut.store(false, std::memory_order_seq_cst);
  g_drainReq.store(false);
  g_drained.store(false);
  g_cpStopped.store(false);
  g_allowStranded.store(true);
  g_cutWaitSeq.store(kNoCut);
  {
    std::lock_guard<std::mutex> lk(g_dropMu);
    g_dropDevN = 0;
    g_dropFences.store(0);
    g_dropProgress.store(0);
    g_dropOverflow.store(0);
  }
  SetEvent(parkEvent());
  for (int i = 0; i < 500 && g_parkedN.load(std::memory_order_acquire) != 0; ++i) Sleep(2);
  ResetEvent(parkEvent());
  g_state.store(kRunning);
}

void resetGatesForTest() {
  // Every slot forgotten: a later test's census must not meet an earlier test's exited exempt loop under a thread id
  // the OS reused (production loops never exit).
  for (GateSlot& g : g_gates) {
    g.tid.store(0, std::memory_order_relaxed);
    g.role.store(nullptr, std::memory_order_relaxed);
    g.import.store(nullptr, std::memory_order_relaxed);
    g.locks.store(0, std::memory_order_relaxed);
    g.guestCalls.store(0, std::memory_order_relaxed);
    g.gpuWait.store(0, std::memory_order_relaxed);
    g.gpuWaitPhase.store(0, std::memory_order_relaxed);
    g.gpuSpinSeq.store(0, std::memory_order_relaxed);
    g.gpuSpins.store(0, std::memory_order_relaxed);
    g.gpuWaitSeq.store(0, std::memory_order_release);
  }
}

uint32_t parkedCount() { return g_parkedN.load(std::memory_order_acquire); }
bool isParked(uint32_t tid) { return parkedWhere(tid) != nullptr; }

bool openChannelForTest(const char* instanceId, uint32_t pid, void (*onRequest)(void*), void* user) {
  g_testRequest = onRequest;
  g_testUser = user;
  if (!createChannel(instanceId, pid)) return false;
  std::thread(watcherMain).detach();
  return true;
}

void closeChannelForTest() {
  // The watcher stays blocked on the (now orphaned) request handle; closing it would race the wait.
  g_testRequest = nullptr;
}

void setAckForTest() { pAck(nullptr); }

void setQuittingForTest() { g_state.store(kQuitting); }

}  // namespace sr::kernel::quit
