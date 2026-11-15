// iter-467 native-a P2 trace (see p2_trace.hpp): observe-only, gate SR_P2_TRACE.
//
// Every hook reads guest memory the XDK maintains (the device, its fence block, the ring) and
// writes a 40-byte host record; nothing is written to the guest and the two overrides below
// delegate to __imp__ exactly once. Records are appended under a mutex and written to the trace
// file by one host thread every 200 ms, with a [P2_TRACE] census line every 10 s.

#include "p2_trace.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "native_brackets.hpp"
#include "../app/log.hpp"

extern "C" void __imp__sub_820FD580(PPCContext& __restrict ctx, uint8_t* base);

namespace sr::native::p2trace {

namespace {

constexpr uint32_t kVdGlobalDeviceCell = 0x82000948u;  // ENGINE_MAP: dev = *(*cell)
constexpr uint32_t kDevCursor = 0x28u, kDevFenceBlock = 0x2A10u, kDevScratchBlock = 0x2A14u;
constexpr uint32_t kDevOpenId = 0x2A1Cu, kDevCloseCursor = 0x3298u, kDevSwapThrottle = 0x34D0u;
constexpr uint32_t kDevRingWptr = 10820u, kDevRingBase = 13476u, kDevRingMask = 13480u;
// the pacing words (NATIVE_ROADMAP P2.2 "Pacing"): the vblank counter, the gate count, the throttle wait snapshot
constexpr uint32_t kDevVblankCount = 0x3B14u, kDevGateCount = 0x3B20u, kDevThrottleWait = 0x4E4Cu;
constexpr uint32_t kIbHeader = 0xC0013F00u;
constexpr int kKinds = 32;  // every Kind below 32 is counted (kinds 24-27 were added after 24)

uint32_t rd(uint32_t va) { return va ? sr::kernel::readGuestU32BE(va) : 0u; }
uint32_t deviceVA() { return rd(rd(kVdGlobalDeviceCell)); }
// The XDK's inline CPU -> GPU address (native_flush.cpp phys()).
uint32_t phys(uint32_t va) { return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu); }

uint64_t qpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return uint64_t(t.QuadPart);
}

std::mutex g_mu;
std::vector<Rec> g_buf;
FILE* g_file = nullptr;
std::string g_path;
std::once_flag g_threadOnce;
std::atomic<uint64_t> g_count[kKinds];
std::atomic<uint64_t> g_waitNs{0}, g_waitNsByReason[16];
std::atomic<uint64_t> g_pendingTrue{0};
std::atomic<uint32_t> g_lastS{0}, g_lastC{0};
std::atomic<uint32_t> g_fbPhys{0}, g_pcPhys{0};
std::atomic<uint64_t> g_dropped{0};
uint64_t g_qpcFreq = 1;
uint64_t g_t0 = 0;

// Per-thread before -> after state (one slot per hooked entry; none of them re-enters itself).
struct Tls {
  uint32_t waitF = 0, waitReason = 0;
  uint64_t waitQpc = 0;
  uint32_t pendF = 0;
  uint32_t queryQ = 0, queryF = 0, querySize = 0;
  uint32_t ringW0 = 0;
  bool ringlessTaken = false;  // P2-A: the override recorded the item itself; the ring is stale
  uint32_t bsW0 = 0;
  uint32_t closeS0 = 0;
  uint32_t spaceF = 0, spaceN = 0;
  uint32_t wrapF = 0, wrapN = 0;
};
thread_local Tls t_;

void latchPhys() {
  const uint32_t dev = deviceVA();
  if (!dev) return;
  const uint32_t fb = rd(dev + kDevFenceBlock), pc = rd(dev + kDevScratchBlock);
  if (fb) g_fbPhys.store(phys(fb), std::memory_order_relaxed);
  if (pc) g_pcPhys.store(phys(pc), std::memory_order_relaxed);
}

void writerMain() {
  uint64_t lastLog = qpcNow();
  std::vector<Rec> out;
  for (;;) {
    Sleep(200);
    {
      std::lock_guard<std::mutex> lk(g_mu);
      out.swap(g_buf);
    }
    if (!out.empty() && g_file) {
      std::fwrite(out.data(), sizeof(Rec), out.size(), g_file);
      std::fflush(g_file);
      out.clear();
    }
    const uint64_t now = qpcNow();
    if (now - lastLog >= g_qpcFreq * 10u) {
      lastLog = now;
      finalLine("tick");
    }
  }
}

void push(Rec& r) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_buf.size() >= (1u << 20)) {  // 40 MB unflushed: the writer is wedged, drop rather than grow
    g_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_buf.push_back(r);
}

bool init() {
  const char* e = std::getenv("SR_P2_TRACE");
  if (!e || !e[0] || e[0] == '0') return false;
  g_path = (e[0] == '1' && !e[1]) ? "archive/p2_trace_" + std::to_string(GetCurrentProcessId()) + ".bin"
                                  : std::string(e);
  g_file = std::fopen(g_path.c_str(), "wb");
  if (!g_file) return false;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpcFreq = uint64_t(f.QuadPart);
  g_t0 = qpcNow();
  // Header: magic, version, record size, QPC frequency, t0.
  const uint32_t magic = 0x52543250u;  // 'P2TR' little-endian
  const uint16_t ver = 1, recSize = uint16_t(sizeof(Rec));
  std::fwrite(&magic, 4, 1, g_file);
  std::fwrite(&ver, 2, 1, g_file);
  std::fwrite(&recSize, 2, 1, g_file);
  std::fwrite(&g_qpcFreq, 8, 1, g_file);
  std::fwrite(&g_t0, 8, 1, g_file);
  std::fflush(g_file);
  g_buf.reserve(1u << 16);
  return true;
}

// ---- table hooks (kernel/native_brackets.hpp XdkScope) ----------------------------------------

PPCContext& C(void* p) { return *static_cast<PPCContext*>(p); }

// GpuRingUpdateWritePointer_820FD218: the descriptors published in [wptr before, wptr after).
void ringBefore(void* p) {
  (void)p;
  const uint32_t dev = deviceVA();
  t_.ringW0 = rd(dev + kDevRingWptr);
}
void ringAfter(void* p) {
  (void)p;
  if (t_.ringlessTaken) {
    t_.ringlessTaken = false;
    return;
  }
  const uint32_t dev = deviceVA();
  const uint32_t base = rd(dev + kDevRingBase), mask = rd(dev + kDevRingMask);
  const uint32_t w1 = rd(dev + kDevRingWptr);
  if (!base || !mask || (mask & (mask + 1u)) != 0u || mask > 0xFFFFFu) return;
  uint32_t r = t_.ringW0 & mask, n = 0, i = 0;
  while (r != (w1 & mask) && n <= mask) {
    const uint32_t hdr = rd(base + r * 4u);
    if (hdr == kIbHeader) {
      const uint32_t ibPhys = rd(base + ((r + 1u) & mask) * 4u);
      const uint32_t ibDwords = rd(base + ((r + 2u) & mask) * 4u);
      event(kIb, uint16_t(i++), t_.ringW0, ibPhys, ibDwords);
      r = (r + 3u) & mask;
      n += 3;
      continue;
    }
    r = (r + 1u) & mask;
    n++;
  }
}

// GpuBlockOnFence_820FD9A8(dev r3, F r4, reason r5).
void waitBefore(void* p) {
  PPCContext& ctx = C(p);
  t_.waitF = ctx.r4.u32;
  t_.waitReason = ctx.r5.u32;
  t_.waitQpc = qpcNow();
  event(kWaitBegin, uint16_t(t_.waitReason & 0xFFFFu), t_.waitF, uint32_t(ctx.lr), 0u);
}
void waitAfter(void* p) {
  (void)p;
  const uint64_t dt = qpcNow() - t_.waitQpc;
  const uint64_t ns = dt * 1000000000ull / g_qpcFreq;
  g_waitNs.fetch_add(ns, std::memory_order_relaxed);
  g_waitNsByReason[t_.waitReason & 15u].fetch_add(ns, std::memory_order_relaxed);
  event(kWaitEnd, uint16_t(t_.waitReason & 0xFFFFu), t_.waitF, uint32_t(dt), uint32_t(dt >> 32));
}

// D3DDeviceIsFencePending_820FDD90(F r3) -> BOOL r3.
void pendBefore(void* p) { t_.pendF = C(p).r3.u32; }
void pendAfter(void* p) {
  const uint32_t r = C(p).r3.u32;
  if (r) g_pendingTrue.fetch_add(1, std::memory_order_relaxed);
  event(kPending, uint16_t(r & 0xFFFFu), t_.pendF, 0u, 0u);
}

// D3DQueryGetData_82102DC8(q r3, out r4, size r5) -> HRESULT r3 (S_OK 0 / S_FALSE 1).
void queryBefore(void* p) {
  PPCContext& ctx = C(p);
  t_.queryQ = ctx.r3.u32;
  t_.queryF = rd(t_.queryQ + 0x14u);
  t_.querySize = ctx.r5.u32;
}
void queryAfter(void* p) {
  const uint32_t r = C(p).r3.u32;
  event(kQuery, uint16_t(r >= 0x80000000u ? 0xFFFFu : (r & 0xFFFFu)), t_.queryF, t_.queryQ, t_.querySize);
}

// GpuRingWaitForSpace_820FCB30(dev r3, rptr threshold r4, token r5).
void spaceBefore(void* p) {
  PPCContext& ctx = C(p);
  t_.spaceF = ctx.r4.u32;
  t_.spaceN = ctx.r5.u32;
  t_.waitQpc = qpcNow();
  event(kRingSpaceBegin, 0, t_.spaceF, uint32_t(ctx.lr), t_.spaceN);
}
void spaceAfter(void* p) {
  (void)p;
  const uint64_t dt = qpcNow() - t_.waitQpc;
  event(kRingSpaceEnd, 0, t_.spaceF, uint32_t(dt), t_.spaceN);
}

// GpuRingWaitReadPtrWrap_820FD150(dev r3, wptr r4, count r5).
void wrapBefore(void* p) {
  PPCContext& ctx = C(p);
  t_.wrapF = ctx.r4.u32;
  t_.wrapN = ctx.r5.u32;
  t_.waitQpc = qpcNow();
  event(kRptrWrapBegin, 0, t_.wrapF, uint32_t(ctx.lr), t_.wrapN);
}
void wrapAfter(void* p) {
  (void)p;
  const uint64_t dt = qpcNow() - t_.waitQpc;
  event(kRptrWrapEnd, 0, t_.wrapF, uint32_t(dt), t_.wrapN);
}

// D3DDeviceInsertFence_820FDD30(dev r3) -> S r3.
void insertAfter(void* p) { event(kInsertFence, 0, 0u, C(p).r3.u32, 0u); }

// GpuCommandBufferFlush_820FD8C0(dev r3): the kick. Cursor before / after.
void flushBefore(void* p) {
  PPCContext& ctx = C(p);
  event(kFlushBegin, 0, 0u, rd(deviceVA() + kDevCursor), uint32_t(ctx.lr));
}
void flushAfter(void* p) {
  (void)p;
  event(kFlushEnd, 0, 0u, rd(deviceVA() + kDevCursor), 0u);
}

void registerHooks() {
  setScopeHook(0x820FD218u, ringBefore, ringAfter);
  setScopeHook(0x820FD9A8u, waitBefore, waitAfter);
  setScopeHook(0x820FDD90u, pendBefore, pendAfter);
  setScopeHook(0x82102DC8u, queryBefore, queryAfter);
  setScopeHook(0x820FCB30u, spaceBefore, spaceAfter);
  setScopeHook(0x820FD150u, wrapBefore, wrapAfter);
  setScopeHook(0x820FDD30u, nullptr, insertAfter);
  setScopeHook(0x820FD8C0u, flushBefore, flushAfter);
}

bool initAll() {
  if (!init()) return false;
  registerHooks();
  return true;
}

}  // namespace

bool g_on = initAll();

void event(uint16_t kind, uint16_t aux, uint32_t f, uint32_t a, uint32_t b) {
  if (!g_on) return;
  std::call_once(g_threadOnce, [] { std::thread(writerMain).detach(); });
  const uint32_t dev = deviceVA();
  Rec r;
  r.present = uint32_t(sr::kernel::getVdSwapCount());
  r.tid = GetCurrentThreadId();
  r.kind = kind;
  r.aux = aux;
  r.f = f;
  r.a = a;
  r.b = b;
  if (kind < kKinds) g_count[kind].fetch_add(1, std::memory_order_relaxed);
  // The time and the S / C snapshot are taken under the buffer lock, so the file order IS the read
  // order across threads (the baseline roll showed 224 "C backwards" artifacts of 24.4M records
  // from reads made before the lock).
  std::lock_guard<std::mutex> lk(g_mu);
  r.qpc = qpcNow();
  r.s = rd(dev + kDevOpenId);
  r.c = rd(rd(dev + kDevFenceBlock));
  g_lastS.store(r.s, std::memory_order_relaxed);
  g_lastC.store(r.c, std::memory_order_relaxed);
  if (g_buf.size() >= (1u << 20)) {  // 40 MB unflushed: the writer is wedged, drop rather than grow
    g_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_buf.push_back(r);
}

void cpEvent(void* user, uint32_t what, uint32_t addr, uint32_t value) {
  (void)user;
  if (!g_on) return;
  uint32_t fb = g_fbPhys.load(std::memory_order_relaxed), pc = g_pcPhys.load(std::memory_order_relaxed);
  if (!fb || !pc) {
    latchPhys();
    fb = g_fbPhys.load(std::memory_order_relaxed);
    pc = g_pcPhys.load(std::memory_order_relaxed);
  }
  if (what == 0) {
    if (fb && addr == fb) event(kComplete, 0, value, value, addr);
    else if (fb && addr == fb + 4u) event(kProgress, 0, 0u, value, addr);
  } else if (what == 1) {
    if (pc && addr >= pc && addr < pc + 32u) event(kScratch, uint16_t((addr - pc) >> 2), 0u, value, addr);
  } else if (what == 2) {  // a CP write of SCRATCH_UMSK (0x1DC) / SCRATCH_ADDR (0x1DD): addr = the register
    event(kRegw, uint16_t(addr), 0u, value, 0u);
  } else if (what == 3 || what == 4) {  // a scratch write-back not performed (unmapped) / deferred to the executor
    const uint16_t n = uint16_t(pc && addr >= pc && addr < pc + 32u ? (addr - pc) >> 2 : 0xFFu);
    event(what == 3 ? kScratchSkip : kScratchDefer, n, 0u, addr, value);
  }
}

void onIsr(uint32_t cpuMask, uint32_t ctxVA) {
  (void)ctxVA;
  if (!g_on) return;
  const uint32_t pc = rd(deviceVA() + kDevScratchBlock);
  event(kIsr, uint16_t(cpuMask & 0xFFFFu), 0u, rd(pc), rd(pc + 0x10u));
}

void onPresent() {
  if (!g_on) return;
  const uint32_t dev = deviceVA();
  const uint32_t fb = rd(dev + kDevFenceBlock);
  if (!g_fbPhys.load(std::memory_order_relaxed)) latchPhys();
  event(kPresent, 0, 0u, rd(dev + kDevSwapThrottle), rd(fb + 0x3Cu));
  event(kPacing, 0, rd(dev + kDevVblankCount), rd(dev + kDevGateCount), rd(dev + kDevThrottleWait));
}

void markRingless() { t_.ringlessTaken = true; }

void onPatch(uint32_t fence, uint32_t result) {
  if (!g_on) return;
  event(kPatch, uint16_t(result & 0xFFFFu), fence, 0u, 0u);
}

void finalLine(const char* why) {
  if (!g_on) return;
  const double t = double(qpcNow() - g_t0) / double(g_qpcFreq);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[P2_TRACE] %s t=%.1fs close=%llu ib=%llu bs=%llu flush=%llu wait=%llu (%.1f ms; swap %.1f idle %.1f "
              "notbusy %.1f) space=%llu wrap=%llu pend=%llu (TRUE %llu) query=%llu patch=%llu insert=%llu | "
              "complete=%llu progress=%llu scratch=%llu isr=%llu present=%llu | S=%08X C=%08X dropped=%llu file=%s",
              why, t, (unsigned long long)g_count[kClose].load(), (unsigned long long)g_count[kIb].load(),
              (unsigned long long)g_count[kBs].load(), (unsigned long long)g_count[kFlushBegin].load(),
              (unsigned long long)g_count[kWaitBegin].load(), double(g_waitNs.load()) * 1e-6,
              double(g_waitNsByReason[3].load()) * 1e-6, double(g_waitNsByReason[4].load()) * 1e-6,
              double(g_waitNsByReason[5].load()) * 1e-6, (unsigned long long)g_count[kRingSpaceBegin].load(),
              (unsigned long long)g_count[kRptrWrapBegin].load(), (unsigned long long)g_count[kPending].load(),
              (unsigned long long)g_pendingTrue.load(), (unsigned long long)g_count[kQuery].load(),
              (unsigned long long)g_count[kPatch].load(), (unsigned long long)g_count[kInsertFence].load(),
              (unsigned long long)g_count[kComplete].load(), (unsigned long long)g_count[kProgress].load(),
              (unsigned long long)g_count[kScratch].load(), (unsigned long long)g_count[kIsr].load(),
              (unsigned long long)g_count[kPresent].load(), g_lastS.load(), g_lastC.load(),
              (unsigned long long)g_dropped.load(), g_path.c_str());
}

}  // namespace sr::native::p2trace

// ---- observe overrides for the two entries the N0 table does not shim ---------------------------

// GpuBuildFenceWritePacket_sub_820FD580(dev r3, dst r4): writes the 10-dword fence tail at dst and
// advances the open id dev+0x2A1C by 2. Record {f = the S written, a = cursor, b = close cursor}.
PPC_FUNC(sub_820FD580) {
  using namespace sr::native::p2trace;
  const bool on = g_on;
  const uint32_t dev = ctx.r3.u32;
  const uint32_t s0 = on ? sr::kernel::readGuestU32BE(dev + 0x2A1Cu) : 0u;
  __imp__sub_820FD580(ctx, base);  // always, exactly once
  if (!on) return;
  event(kClose, 0, s0, sr::kernel::readGuestU32BE(dev + 0x28u), sr::kernel::readGuestU32BE(dev + 0x3298u));
  // The fence block is stable from the first close: latch it for the consumer-side compare.
  if (!g_fbPhys.load(std::memory_order_relaxed)) latchPhys();
}

// GpuRingEmitBinSelectLo_sub_82110140: its override lives in kernel/gpu_ringless.cpp (P2-A), which
// records the kBs event in both modes.
