// iter-463 PLATFORM FIX: per-thread guest KPCR (real r13) + Ke/Kf spinlocks.
// Contract, gates and references: kernel/kpcr.hpp.
#include "kpcr.hpp"

#include "export_registry.hpp"
#include "guest_context.hpp"
#include "kthread_objects.hpp"
#include "phys_lifo.hpp"          // iter-465 physmem: PhysKindScope
#include "thread_mem_xenia.hpp"   // iter-465 physmem: SR_THREAD_MEM_XENIA overflow blocks
#include "../app/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace sr {
namespace kernel {

namespace {
std::atomic<int> g_kpcrTestMode{-1};
std::atomic<int> g_spinTestMode{-1};

std::mutex g_mu;                                   // arena + kt -> pcr map
uint32_t g_arenaCursor = kKpcrArenaLo;
bool g_arenaExhaustLogged = false;
std::unordered_map<uint32_t, uint32_t> g_ktToPcr;  // KTHREAD VA -> KPCR VA

thread_local uint32_t t_pcr = 0;                   // this host thread's KPCR
std::atomic<uint32_t> g_nextCpu{0};                // Xenia's static next_cpu

std::atomic<uint64_t> g_acquires{0}, g_contended{0}, g_longSpins{0},
    g_recursions{0}, g_foreignReleases{0}, g_noPcr{0};
std::atomic<uint32_t> g_createLogs{0}, g_badLockLogs{0}, g_affLogs{0};

bool shouldLogN(uint64_t n, uint64_t firstN) { return n <= firstN || (n & (n - 1)) == 0; }

bool envOn(const char* name) {
  const char* e = std::getenv(name);
  return !e || e[0] != '0';
}

uint32_t bswap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}

uint8_t readU8(uint32_t va) {
  const uint8_t* p = guestVAToHost(va);
  return p ? *p : 0u;
}
void writeU8(uint32_t va, uint8_t v) {
  if (uint8_t* p = guestVAToHost(va)) *p = v;
}

// The census line the REG_GENERIC_STUB body printed (gate-off path).
bool stubCensusOnK() {
  static const bool v = envOn("SR_STUB_CENSUS");
  return v;
}
void genericCensus(const char* nm, std::atomic<uint32_t>& ctr, uint32_t r3, uint32_t r4,
                   uint32_t r5) {
  const uint32_t n = ctr.fetch_add(1, std::memory_order_relaxed) + 1;
  if (stubCensusOnK() && (n <= 3 || (n & (n - 1)) == 0))
    SR_LOG_BOOT(sr::LogSeverity::Info, "[STUB_CENSUS] %s #%u lr=%08X r3=%08X r4=%08X r5=%08X",
                nm, n, getCurrentGuestLR(), r3, r4, r5);
}

uint32_t allocPcrVA() {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_arenaCursor + kKpcrStride <= kKpcrArenaHi) {
    const uint32_t va = g_arenaCursor;
    g_arenaCursor += kKpcrStride;
    return va;
  }
  // iter-465 physmem (SR_THREAD_MEM_XENIA): Xenia's PCR is a system-heap block
  // (XThread::Create: SystemHeapAlloc(0x2D8)), never physical memory.
  const bool xen = threadmem::enabled();
  if (!g_arenaExhaustLogged) {
    g_arenaExhaustLogged = true;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[KPCR] arena [%08X,%08X) exhausted; falling back to %s",
                kKpcrArenaLo, kKpcrArenaHi, xen ? "the Xenia system heap" : "guestPhysAlloc");
  }
  if (xen) {
    if (const uint32_t va = threadmem::allocSystem(kKpcrSize)) return va;
  }
  PhysKindScope pk(PhysKind::kKpcr);
  return guestPhysAlloc(0x1000u, 0x1000u);
}

// Xenia XThread::Create (xthread.cc:399-409): the fields a reader can observe.
bool initPcr(uint32_t pcr, uint32_t kt, uint8_t cpu, uint32_t stackBase, uint32_t stackLimit) {
  if (!pcr || !guestVAToHost(pcr) || !guestVAToHost(pcr + kKpcrSize - 1)) return false;
  std::memset(guestVAToHost(pcr), 0, kKpcrSize);
  writeGuestBE32(pcr + kKpcrOffTlsPtr, 0u);          // TLS goes through KeTls* here
  writeGuestBE32(pcr + kKpcrOffPcrPtr + 0, 0u);      // be64 pcr_ptr
  writeGuestBE32(pcr + kKpcrOffPcrPtr + 4, pcr);
  writeGuestBE32(pcr + kKpcrOffStackBase, stackBase);
  writeGuestBE32(pcr + kKpcrOffStackEnd, stackLimit);
  writeGuestBE32(pcr + kKpcrOffCurrentThread, kt);
  writeGuestBE32(pcr + kKpcrOffPrcbPtr, pcr + kKpcrOffPrcb);
  writeGuestBE32(pcr + kKpcrOffDpcActive, 0u);
  writeU8(pcr + kKpcrOffCurrentCpu, cpu);
  if (kt) writeU8(kt + kKtOffCurrentCpu, cpu);
  return true;
}

uint32_t createPcr(uint32_t kt, uint8_t cpu, uint32_t stackBase, uint32_t stackLimit) {
  const uint32_t pcr = allocPcrVA();
  if (!initPcr(pcr, kt, cpu, stackBase, stackLimit)) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[KPCR] create kt=%08X: no block (pcr=%08X)", kt, pcr);
    return 0;
  }
  if (kt) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_ktToPcr[kt] = pcr;
  }
  const uint32_t n = g_createLogs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (shouldLogN(n, 64))
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[KPCR] #%u pcr=%08X kt=%08X cpu=%u stack=[%08X,%08X) handle=%u",
                n, pcr, kt, cpu, stackLimit, stackBase, getCurrentThreadHandle());
  return pcr;
}

// The lock owner for this call: the caller's r13 (Xenia), else this thread's
// bound KPCR (a host context that never got r13 - logged, never 0).
uint32_t ownerPcr(uint32_t r13, const char* who) {
  if (r13) return r13;
  uint32_t pcr = t_pcr;
  if (!pcr) pcr = kpcrBindCurrent(0u, 0u, -1);
  const uint64_t n = g_noPcr.fetch_add(1, std::memory_order_relaxed) + 1;
  if (shouldLogN(n, 8))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[KE_SPIN_NOPCR] #%llu %s lr=%08X r13=0 -> owner pcr=%08X",
                (unsigned long long)n, who, getCurrentGuestLR(), pcr);
  return pcr;
}

uint32_t* lockWord(uint32_t lockVA, const char* who) {
  uint8_t* p = (lockVA & 3u) == 0 ? guestVAToHost(lockVA) : nullptr;
  if (!p) {
    const uint32_t n = g_badLockLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (shouldLogN(n, 8))
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[KE_SPIN_BADLOCK] #%u %s lock=%08X lr=%08X", n, who,
                  lockVA, getCurrentGuestLR());
    return nullptr;
  }
  return reinterpret_cast<uint32_t*>(p);
}

// ── contention census (observe-only) ────────────────────────────────────────
// Last successful acquirer's guest lr per lock (racy hash, diagnostics only).
struct HolderSlot { std::atomic<uint32_t> lock{0}, lr{0}; };
HolderSlot g_holder[256];
uint32_t holderIdx(uint32_t lockVA) { return ((lockVA >> 2) * 2654435761u) >> 24; }
void noteHolder(uint32_t lockVA, uint32_t lr) {
  HolderSlot& h = g_holder[holderIdx(lockVA)];
  h.lock.store(lockVA, std::memory_order_relaxed);
  h.lr.store(lr, std::memory_order_relaxed);
}
uint32_t holderLr(uint32_t lockVA) {
  HolderSlot& h = g_holder[holderIdx(lockVA)];
  return h.lock.load(std::memory_order_relaxed) == lockVA ? h.lr.load(std::memory_order_relaxed) : 0u;
}
struct SpinRec { uint32_t lock, holderLr, waiterLr; uint64_t n, us; };
std::mutex g_spinMu;
SpinRec g_spinRecs[64];
int g_spinRecN = 0;
std::atomic<uint64_t> g_spinUs{0};
std::atomic<uint64_t> g_nextSumMs{0};
uint64_t g_prevAcq = 0, g_prevCont = 0, g_prevUs = 0;   // under g_spinMu

bool spinCensusOn() {
  static const bool v = envOn("SR_KE_SPIN_CENSUS");
  return v;
}

void recordSpin(uint32_t lockVA, uint32_t hLr, uint32_t wLr, uint64_t us) {
  g_spinUs.fetch_add(us, std::memory_order_relaxed);
  if (!spinCensusOn()) return;
  std::lock_guard<std::mutex> lk(g_spinMu);
  for (int i = 0; i < g_spinRecN; i++) {
    SpinRec& r = g_spinRecs[i];
    if (r.lock == lockVA && r.holderLr == hLr && r.waiterLr == wLr) { r.n++; r.us += us; return; }
  }
  if (g_spinRecN < 64) g_spinRecs[g_spinRecN++] = SpinRec{lockVA, hLr, wLr, 1u, us};
}

// [KE_SPIN_SUM]: a fixed-cadence summary (one line per 5 s window that saw a
// spinlock call), so its counts are safe to quote.
void maybeSummary() {
  if (!spinCensusOn()) return;
  const uint64_t now = GetTickCount64();
  uint64_t due = g_nextSumMs.load(std::memory_order_relaxed);
  if (due == 0) { g_nextSumMs.compare_exchange_strong(due, now + 5000); return; }
  if (now < due || !g_nextSumMs.compare_exchange_strong(due, now + 5000)) return;
  std::lock_guard<std::mutex> lk(g_spinMu);
  const uint64_t acq = g_acquires.load(), cont = g_contended.load(), us = g_spinUs.load();
  SpinRec top[4] = {};
  for (int i = 0; i < g_spinRecN; i++) {
    for (int k = 0; k < 4; k++) {
      if (g_spinRecs[i].us > top[k].us) {
        for (int m = 3; m > k; m--) top[m] = top[m - 1];
        top[k] = g_spinRecs[i];
        break;
      }
    }
  }
  char buf[512];
  int o = std::snprintf(buf, sizeof buf, "acq=+%llu contended=+%llu spin=+%.1fms |",
                        (unsigned long long)(acq - g_prevAcq), (unsigned long long)(cont - g_prevCont),
                        double(us - g_prevUs) / 1000.0);
  for (int k = 0; k < 4 && top[k].n && o > 0 && o < int(sizeof buf); k++)
    o += std::snprintf(buf + o, sizeof buf - o, " lock=%08X holder=%08X waiter=%08X n=%llu %.1fms;",
                       top[k].lock, top[k].holderLr, top[k].waiterLr,
                       (unsigned long long)top[k].n, double(top[k].us) / 1000.0);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[KE_SPIN_SUM] %s", buf);
  g_prevAcq = acq; g_prevCont = cont; g_prevUs = us;
  g_spinRecN = 0;
}

uint64_t qpcUs() {
  static const uint64_t f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return uint64_t(q.QuadPart); }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return uint64_t(c.QuadPart) * 1000000ull / f;
}

uint8_t raiseIrql(uint32_t pcr, uint8_t newIrql) {   // xeKfRaiseIrql
  if (!pcr) return 0u;   // no KPCR could be built: never write guest VA 0x18
  const uint8_t old = readU8(pcr + kKpcrOffIrql);
  writeU8(pcr + kKpcrOffIrql, newIrql);
  return old;
}
}  // namespace

bool kpcrEnabled() {
  const int m = g_kpcrTestMode.load(std::memory_order_relaxed);
  if (m >= 0) return m != 0 && obKThreadEnabled();
  static const bool v = envOn("SR_KPCR");
  return v && obKThreadEnabled();
}

bool keSpinlockEnabled() {
  if (!kpcrEnabled()) return false;
  const int m = g_spinTestMode.load(std::memory_order_relaxed);
  if (m >= 0) return m != 0;
  static const bool v = envOn("SR_KE_SPINLOCK");
  return v;
}

void kpcrSetEnabledForTest(int mode) {
  g_kpcrTestMode.store(mode < 0 ? -1 : (mode ? 1 : 0), std::memory_order_relaxed);
}
void keSpinlockSetEnabledForTest(int mode) {
  g_spinTestMode.store(mode < 0 ? -1 : (mode ? 1 : 0), std::memory_order_relaxed);
}

// ── iter-463 SR_PROCTYPE (kpcr.hpp) ─────────────────────────────────────────────────────────
namespace {
std::atomic<int> g_procTypeTestMode{-1};
std::atomic<uint32_t> g_procTypeNoPcrLogs{0};
std::atomic<uint64_t> g_procTypeCalls{0};
}  // namespace

bool procTypeEnabled() {
  if (!kpcrEnabled()) return false;
  const int m = g_procTypeTestMode.load(std::memory_order_relaxed);
  if (m >= 0) return m != 0;
  static const bool v = [] {   // iter-463 rolls: default ON (=0 keeps the A/B)
    const char* e = std::getenv("SR_PROCTYPE");
    return !e || e[0] != '0';
  }();
  return v;
}
void procTypeSetEnabledForTest(int mode) {
  g_procTypeTestMode.store(mode < 0 ? -1 : (mode ? 1 : 0), std::memory_order_relaxed);
}

void kthreadSetProcessType(uint32_t kt, uint8_t type) {
  if (!procTypeEnabled() || !kt) return;
  writeU8(kt + kKtOffProcessTypeDup, type);
  writeU8(kt + kKtOffProcessType, type);
}

uint32_t keGetCurrentProcessType(uint32_t r13) {
  if (!procTypeEnabled()) return 0;
  g_procTypeCalls.fetch_add(1, std::memory_order_relaxed);
  if (!r13 || !guestVAToHost(r13) || !guestVAToHost(r13 + kKpcrSize - 1)) {
    // Xenia would dereference a bad r13 and crash; name it and keep the old answer.
    if (shouldLogN(g_procTypeNoPcrLogs.fetch_add(1, std::memory_order_relaxed) + 1, 10))
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[PROCTYPE] KeGetCurrentProcessType with r13=%08X (no KPCR) lr=%08X -> 0",
                  r13, getCurrentGuestLR());
    return 0;
  }
  if (readGuestU32BE(r13 + kKpcrOffDpcActive) != 0) return readU8(r13 + kKpcrOffProcTypeInDpc);
  const uint32_t kt = readGuestU32BE(r13 + kKpcrOffCurrentThread);
  if (!kt || !guestVAToHost(kt + kKtOffProcessType)) return 0;
  return readU8(kt + kKtOffProcessType);
}

uint8_t kpcrFakeCpuNumber(uint8_t procMask) {
  if (!procMask) {
    // xthread.cc:159-171: stay on the parent's hardware thread; round-robin
    // only for a thread no guest thread created.
    if (t_pcr) return readU8(t_pcr + kKpcrOffCurrentCpu);
    uint32_t cur = g_nextCpu.load(std::memory_order_relaxed), nxt;
    do { nxt = (cur + 1u) % 6u; } while (!g_nextCpu.compare_exchange_weak(cur, nxt));
    return uint8_t(nxt);
  }
  // 7 - lzcnt8(mask): the highest set bit (Xenia asserts exactly one bit < 6).
  uint8_t cpu = 7;
  while (cpu && !(procMask & (1u << cpu))) --cpu;
  if (cpu >= 6) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[KPCR] processor mask %02X names cpu %u (>5)", procMask, cpu);
    cpu = uint8_t(cpu % 6u);
  }
  return cpu;
}

uint32_t kpcrCreateForKThread(uint32_t kt, uint8_t cpu, uint32_t stackBase,
                              uint32_t stackLimit) {
  if (!kpcrEnabled() || !kt) return 0;
  return createPcr(kt, cpu, stackBase, stackLimit);
}

uint32_t kpcrBindCurrent(uint32_t stackBase, uint32_t stackLimit, int cpu) {
  if (!kpcrEnabled()) return 0;
  if (t_pcr) return t_pcr;
  const uint32_t kt = kthreadCurrent(true);
  uint32_t pcr = kt ? kpcrForKThread(kt) : 0u;
  if (pcr) {
    // Built by ExCreateThread; the runner knows the real stack now.
    if (stackBase) {
      writeGuestBE32(pcr + kKpcrOffStackBase, stackBase);
      writeGuestBE32(pcr + kKpcrOffStackEnd, stackLimit);
    }
    if (cpu >= 0) kpcrSetActiveCpu(pcr, uint8_t(cpu));
  } else {
    const uint8_t c = cpu >= 0 ? uint8_t(cpu) : kpcrFakeCpuNumber(0);
    pcr = createPcr(kt, c, stackBase, stackLimit);
  }
  t_pcr = pcr;
  return pcr;
}

uint32_t kpcrCurrent() { return t_pcr; }

uint32_t kpcrForKThread(uint32_t kt) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_ktToPcr.find(kt);
  return it != g_ktToPcr.end() ? it->second : 0u;
}

uint8_t kpcrCpu(uint32_t pcr) { return pcr ? readU8(pcr + kKpcrOffCurrentCpu) : 0u; }

void kpcrSetActiveCpu(uint32_t pcr, uint8_t cpu) {
  if (!pcr) return;
  writeU8(pcr + kKpcrOffCurrentCpu, cpu);
  const uint32_t kt = readGuestU32BE(pcr + kKpcrOffCurrentThread);
  if (kt) writeU8(kt + kKtOffCurrentCpu, cpu);
}

KpcrDpcScope::KpcrDpcScope(uint32_t pcr) : pcr_(pcr), prevIrql_(0) {
  if (!pcr_) return;
  prevIrql_ = readU8(pcr_ + kKpcrOffIrql);
  writeU8(pcr_ + kKpcrOffIrql, 2u);
  writeGuestBE32(pcr_ + kKpcrOffDpcActive, 1u);
  writeU8(pcr_ + kKpcrOffProcTypeInDpc, 1u);   // X_PROCTYPE_TITLE
}
KpcrDpcScope::~KpcrDpcScope() {
  if (!pcr_) return;
  writeU8(pcr_ + kKpcrOffProcTypeInDpc, 0u);   // X_PROCTYPE_IDLE (while still in the DPC)
  writeU8(pcr_ + kKpcrOffIrql, prevIrql_);
  writeGuestBE32(pcr_ + kKpcrOffDpcActive, 0u);
}

// xboxkrnl_threading.cc:1184-1222 (xeKeKfAcquireSpinLock).
uint32_t keAcquireSpinLock(uint32_t lockVA, uint32_t r13, bool raise) {
  const uint32_t pcr = ownerPcr(r13, raise ? "KfAcquireSpinLock" : "KeAcquireSpinLockAtRaisedIrql");
  const uint32_t oldIrql = raise ? raiseIrql(pcr, 2u) : 0u;
  uint32_t* w = lockWord(lockVA, "acquire");
  if (!w || !pcr) return oldIrql;
  g_acquires.fetch_add(1, std::memory_order_relaxed);
  const uint32_t mine = bswap32(pcr);
  volatile LONG* lw = reinterpret_cast<volatile LONG*>(w);
  if (uint32_t(*lw) == mine) {
    // Xenia asserts here and then spins forever; so do we - named.
    const uint64_t n = g_recursions.fetch_add(1, std::memory_order_relaxed) + 1;
    if (shouldLogN(n, 16))
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[KE_SPIN_RECURSE] #%llu lock=%08X pcr=%08X lr=%08X",
                  (unsigned long long)n, lockVA, pcr, getCurrentGuestLR());
  }
  const uint32_t myLr = getCurrentGuestLR();
  maybeSummary();
  if (uint32_t(InterlockedCompareExchange(lw, LONG(mine), 0)) == 0) {
    noteHolder(lockVA, myLr);
    return oldIrql;
  }
  g_contended.fetch_add(1, std::memory_order_relaxed);
  const uint32_t hLr = holderLr(lockVA);
  const uint64_t us0 = qpcUs();
  const uint8_t ourCpu = readU8(pcr + kKpcrOffCurrentCpu);
  const uint64_t t0 = GetTickCount64();
  uint64_t nextReportMs = 2000;
  for (;;) {
    const uint32_t ownerBe = uint32_t(*lw);
    if (ownerBe == 0) {
      if (uint32_t(InterlockedCompareExchange(lw, LONG(mine), 0)) == 0) {
        noteHolder(lockVA, myLr);
        recordSpin(lockVA, hLr, myLr, qpcUs() - us0);
        return oldIrql;
      }
      continue;
    }
    // Same guest CPU: the console scheduler would preempt the spinner so the
    // holder runs; force a host context switch. Else just yield.
    const uint32_t owner = bswap32(ownerBe);
    const uint8_t* op = guestVAToHost(owner + kKpcrOffCurrentCpu);
    if (op && *op == ourCpu) Sleep(0);
    else SwitchToThread();
    const uint64_t dt = GetTickCount64() - t0;
    if (dt >= nextReportMs) {
      nextReportMs *= 2;
      const uint64_t n = g_longSpins.fetch_add(1, std::memory_order_relaxed) + 1;
      const uint32_t ownerKt = guestVAToHost(owner) ? readGuestU32BE(owner + kKpcrOffCurrentThread) : 0u;
      if (shouldLogN(n, 32))
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[KE_SPIN_LONG] #%llu lock=%08X spun=%llums owner_pcr=%08X owner_kt=%08X "
                    "self_pcr=%08X lr=%08X",
                    (unsigned long long)n, lockVA, (unsigned long long)dt, owner, ownerKt, pcr,
                    getCurrentGuestLR());
    }
  }
}

// xboxkrnl_threading.cc:1231-1246 (xeKeKfReleaseSpinLock).
void keReleaseSpinLock(uint32_t lockVA, uint32_t r13, uint32_t oldIrql, bool changeIrql) {
  const uint32_t pcr = ownerPcr(r13, changeIrql ? "KfReleaseSpinLock" : "KeReleaseSpinLockFromRaisedIrql");
  if (uint32_t* w = lockWord(lockVA, "release")) {
    volatile LONG* lw = reinterpret_cast<volatile LONG*>(w);
    const uint32_t prev = uint32_t(InterlockedExchange(lw, 0));   // release (full barrier)
    if (prev != bswap32(pcr)) {
      const uint64_t n = g_foreignReleases.fetch_add(1, std::memory_order_relaxed) + 1;
      if (shouldLogN(n, 16))
        SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[KE_SPIN_FOREIGN_RELEASE] #%llu lock=%08X owner_pcr=%08X self_pcr=%08X lr=%08X",
                    (unsigned long long)n, lockVA, bswap32(prev), pcr, getCurrentGuestLR());
    }
  }
  if (changeIrql) {
    if (oldIrql >= 2u) return;
    kfLowerIrql(pcr, oldIrql);
  }
}

uint32_t keRaiseIrqlToDpcLevel(uint32_t r13) {
  const uint32_t pcr = ownerPcr(r13, "KeRaiseIrqlToDpcLevel");
  return pcr ? raiseIrql(pcr, 2u) : 0u;
}

void kfLowerIrql(uint32_t r13, uint32_t newIrql) {
  const uint32_t pcr = ownerPcr(r13, "KfLowerIrql");
  if (pcr) writeU8(pcr + kKpcrOffIrql, uint8_t(newIrql));
}

// xboxkrnl_threading.cc:360-385.
uint32_t keSetAffinityThread(uint32_t kt, uint32_t affinity, uint32_t prevOutVA) {
  if (!affinity) return kKeStatusInvalidParameter;
  const uint32_t pcr = kpcrForKThread(kt);
  uint32_t status = 0;
  if (!pcr) {
    status = kStatusInvalidHandleKe;
  } else {
    if (prevOutVA) writeGuestBE32(prevOutVA, 1u << kpcrCpu(pcr));
    kpcrSetActiveCpu(pcr, kpcrFakeCpuNumber(uint8_t(affinity)));
  }
  const uint32_t n = g_affLogs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (shouldLogN(n, 32))
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[KPCR] KeSetAffinityThread #%u kt=%08X mask=%08X -> pcr=%08X cpu=%u status=%08X lr=%08X",
                n, kt, affinity, pcr, pcr ? kpcrCpu(pcr) : 0u, status, getCurrentGuestLR());
  return status;
}

void registerKpcrExports(ExportRegistry& reg) {
  reg.registerExport("KeAcquireSpinLockAtRaisedIrql",
                     +[](uint32_t r3, uint32_t r4, uint32_t r5, uint32_t, uint32_t, uint32_t) -> uint32_t {
    static std::atomic<uint32_t> s_n{0};
    if (!keSpinlockEnabled()) { genericCensus("KeAcquireSpinLockAtRaisedIrql", s_n, r3, r4, r5); return 0; }
    keAcquireSpinLock(r3, ExportRegistry::getCurrentR13(), false);
    return 0;
  });
  reg.registerExport("KeReleaseSpinLockFromRaisedIrql",
                     +[](uint32_t r3, uint32_t r4, uint32_t r5, uint32_t, uint32_t, uint32_t) -> uint32_t {
    static std::atomic<uint32_t> s_n{0};
    if (!keSpinlockEnabled()) { genericCensus("KeReleaseSpinLockFromRaisedIrql", s_n, r3, r4, r5); return 0; }
    keReleaseSpinLock(r3, ExportRegistry::getCurrentR13(), 0u, false);
    return 0;
  });
  reg.registerExport("KfAcquireSpinLock",
                     +[](uint32_t r3, uint32_t r4, uint32_t r5, uint32_t, uint32_t, uint32_t) -> uint32_t {
    static std::atomic<uint32_t> s_n{0};
    if (!keSpinlockEnabled()) { genericCensus("KfAcquireSpinLock", s_n, r3, r4, r5); return 0; }
    return keAcquireSpinLock(r3, ExportRegistry::getCurrentR13(), true);
  });
  reg.registerExport("KfReleaseSpinLock",
                     +[](uint32_t r3, uint32_t r4, uint32_t r5, uint32_t, uint32_t, uint32_t) -> uint32_t {
    static std::atomic<uint32_t> s_n{0};
    if (!keSpinlockEnabled()) { genericCensus("KfReleaseSpinLock", s_n, r3, r4, r5); return 0; }
    keReleaseSpinLock(r3, ExportRegistry::getCurrentR13(), r4 & 0xFFu, true);
    return 0;
  });
  reg.registerExport("KeRaiseIrqlToDpcLevel",
                     +[](uint32_t r3, uint32_t r4, uint32_t r5, uint32_t, uint32_t, uint32_t) -> uint32_t {
    static std::atomic<uint32_t> s_n{0};
    if (!keSpinlockEnabled()) { genericCensus("KeRaiseIrqlToDpcLevel", s_n, r3, r4, r5); return 0; }
    return keRaiseIrqlToDpcLevel(ExportRegistry::getCurrentR13());
  });
  reg.registerExport("KfLowerIrql",
                     +[](uint32_t r3, uint32_t r4, uint32_t r5, uint32_t, uint32_t, uint32_t) -> uint32_t {
    static std::atomic<uint32_t> s_n{0};
    if (!keSpinlockEnabled()) { genericCensus("KfLowerIrql", s_n, r3, r4, r5); return 0; }
    kfLowerIrql(ExportRegistry::getCurrentR13(), r3 & 0xFFu);
    return 0;
  });
  reg.registerExport("KeSetAffinityThread",
                     +[](uint32_t r3, uint32_t r4, uint32_t r5, uint32_t, uint32_t, uint32_t) -> uint32_t {
    static std::atomic<uint32_t> s_n{0};
    if (!kpcrEnabled()) { genericCensus("KeSetAffinityThread", s_n, r3, r4, r5); return 0; }
    return keSetAffinityThread(r3, r4, r5);
  });
}

KeSpinStats keSpinStats() {
  return KeSpinStats{g_acquires.load(), g_contended.load(), g_longSpins.load(),
                     g_recursions.load(), g_foreignReleases.load(), g_noPcr.load()};
}

uint32_t kpcrArenaCursorForTest(uint32_t newCursor) {
  std::lock_guard<std::mutex> lk(g_mu);
  const uint32_t old = g_arenaCursor;
  g_arenaCursor = newCursor;
  return old;
}

}  // namespace kernel
}  // namespace sr
