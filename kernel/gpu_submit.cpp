// Native submission service: queued records, callbacks, fences and presents.
// The Oracle build also calls the separately linked packet decoder.

#include "sr_oracle.hpp"
#include "gpu_submit.hpp"
#include "host_work.hpp"   // step 4 2k(4): the CP is a presenting thread (SR_GPU_HANG_HOST_WORK)
#include "../gfx/resource_census.hpp"  // Oracle census; no-op in release
#if SR_ORACLE
#include "gpu_command_processor.hpp"
#endif
#include "graphics_interrupt_registration.hpp"

#include "gpu_hang_wd.hpp"
#include "phys_lifo.hpp"

#include "guest_call.hpp"
#include "guest_context.hpp"
#include "../app/crash_handler.hpp"
#include "../app/log.hpp"
#include "../gfx/null_renderer.hpp"
#include "sr_oracle.hpp"
#if SR_ORACLE  // C1 stage 1: the development CP's engine is the PM4 walker (ring packets, plain IBs, the oracles)
#include "../gfx/pm4_engine.hpp"
#else          // the release configuration: records only (gfx/record_engine.hpp), no walker in the link
#include "../gfx/record_engine.hpp"
#endif
#include "../gfx/pm4_coverage.hpp"   // NATIVE N0.2 (SR_PM4_COVERAGE)
#include "../gfx/shader_cache.hpp"   // render/boot-attach: WP1 (the CP waits for the installed shader cache)
#include "native_meter.hpp"   // iter-467 native meter
#include "p2_trace.hpp"       // iter-467 native-a P2 trace (SR_P2_TRACE, observe)
#include "gpu_ringless.hpp"   // iter-467 native-a P2-A (SR_NATIVE_RINGLESS)
#include "native_flush.hpp"   // iter-467 native-a P2-B (cpSegment)
#include "native_transport.hpp"
#include "native_vblank.hpp"  // display.registers: the [NATIVE_VBLANK] census line (SR_NATIVE_VBLANK only)
#include "native_devcreate.hpp"  // boot.device_create: finalLine
#include "../gfx/native_zpd.hpp"  // p2.zpd: the [NATIVE_ZPD] census line
#include "quit.hpp"           // tools/graceful-quit: the ring cut + the CP drain point

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>  // render/boot-attach: the player's message box when the backend never attaches
#endif

#if SR_ORACLE
namespace sr::gfx {
void executeGuestIndirectBuffer(uint32_t ibPhys, uint32_t ibSizeDwords);  // null_renderer.cpp (its oracle half)
}
#endif

#if SR_ORACLE
#define SR_SUBMIT_TAG "[GPU_CP]"
#define SR_SUBMIT_TIME_TAG "[GPU_CP_TIME]"
#define SR_SUBMIT_STALL_TAG "[GPU_CP_STALL]"
#else
#define SR_SUBMIT_TAG "[NATIVE_SUBMIT]"
#define SR_SUBMIT_TIME_TAG "[NATIVE_SUBMIT_TIME]"
#define SR_SUBMIT_STALL_TAG "[NATIVE_SUBMIT_STALL]"
#endif

namespace sr::kernel {
namespace {

// C1 stage 1 (docs/RELEASE_COMPOSITION.md 9.9): the engine this thread applies items on - the live PM4 walker in the
// development configuration, the live record engine in the release one (kernel/native_flush.hpp CpEngine). There a
// kind-1 item (a plain indirect buffer) and a ring packet are fatal: nothing could interpret them.
#if SR_ORACLE
sr::gfx::CpEngine& cpEngine() { return sr::gfx::getLivePm4(); }
#else
sr::gfx::CpEngine& cpEngine() { return sr::gfx::getLiveRecordEngine(); }
[[noreturn]] void cpNoWalkerFatal(const char* what, uint32_t a, uint32_t b) {
  SR_LOG_BOOT(sr::LogSeverity::Error,
              "[NATIVE_FLUSH_BAD] %s (%08X, %u): the release configuration has no PM4 walker (C1 stage 1) - a "
              "producer was not converted; the command is not acknowledged",
              what, a, b);
  sr::Log::shutdown();
  std::_Exit(3);
}
#endif

// iter-467 native-a P2-A: a host item in ring index space (gpuCpPushItem).
struct RlItem {
  uint32_t kind, index, n, a, b;
  uint64_t id;  // kind 3 (P2-B SEGMENT): the registered part list's id (native_flush cpSegment)
};

struct Cp {
  std::mutex mu;
  std::condition_variable cv;
  // P2-A item queue (SR_NATIVE_RINGLESS), consumed in index order by the CP loop.
  std::mutex rlMu;
  std::deque<RlItem> rl;
  std::atomic<uint64_t> rlIb{0}, rlBs{0}, rlDesync{0}, rlSeg{0};
  uint32_t ringBase = 0, mask = 0, wptr = 0;  // guarded by mu
  bool published = false;                    // guarded by mu
  // iter-463: lock-free mirrors of the above for the hang watchdog.
  std::atomic<uint32_t> ringBaseMirror{0}, wptrMirror{0};
  std::atomic<bool> publishedMirror{false};
  std::atomic<bool> running{true};
  std::atomic<bool> started{false};
  std::atomic<uint32_t> rptrWbPhys{0};
  std::atomic<uint32_t> counter{0};
  std::atomic<uint32_t> rptrShared{0};       // for the census line
  // CP-thread only.
  uint32_t rptr = 0;
  GuestCaller* gc = nullptr;
  uint32_t pcrVA = 0;
  // Stats.
  std::atomic<uint64_t> doorbells{0}, packets{0}, isrCalls{0}, isrFaults{0}, swaps{0};
  // Where the CP thread's wall time goes (ns, cumulative): the census prints
  // the deltas so a slow era names its own cost centre.
  std::atomic<uint64_t> nsIdle{0}, nsIb{0}, nsSwap{0}, nsIsr{0};
  // Stall watchdog (gpuCpVblank, the VD_ISR thread): what the CP is doing and
  // since when. 0 idle, 1 ring packet, 2 indirect buffer, 3 swap, 4 ISR,
  // 5 host wait (kCpPhaseHostWait: blocked on the live backend's attach or the boot shader cache, not the GPU).
  std::atomic<uint32_t> phase{0};
  std::atomic<uint32_t> hostWait{0};  // kCpHostWait*: what a phase-5 CP waits for (the watchdog's escalation)
  std::atomic<uint64_t> phaseSinceNs{0};
};

uint64_t nowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// Sets the CP phase; returns the previous one (for nested scopes).
uint32_t setPhase(Cp& c, uint32_t p) {
  c.phaseSinceNs.store(nowNs(), std::memory_order_relaxed);
  return c.phase.exchange(p, std::memory_order_relaxed);
}

Cp& cp() {
  static Cp c;
  return c;
}

// render/boot-attach (docs/NATIVE_ROADMAP.md "Boot attach"): a blocking shader-cache wait on the CP
// thread (WP1 at its start, WP2 on an installed miss while the translator stage runs) is a host wait,
// not a GPU hang: phase 5 for its duration (gpuHangEvaluate sets aside the CP's own stall; a long wait
// escalates as [HOST_WAIT_STALL]). Other threads' waits leave the phase alone.
thread_local bool t_isCpThread = false;
thread_local uint32_t t_phaseBeforeHostLoad = 0;
void cpHostLoadBegin(int stage) {
  if (!t_isCpThread) return;
  // ShaderCache's stages: 2 installed, 3 all (the translator stage); kWaitTranslation (4): an IM_LOAD waiting for
  // its runtime translation (render/shader-coverage, the miss policy).
  cp().hostWait.store(stage == sr::gfx::ShaderCache::kWaitTranslation ? kCpHostWaitTranslation
                      : stage >= 3                                    ? kCpHostWaitTranslator
                                                                      : kCpHostWaitInstalled,
                      std::memory_order_relaxed);
  t_phaseBeforeHostLoad = setPhase(cp(), kCpPhaseHostWait);
}
void cpHostLoadEnd(int) {
  if (!t_isCpThread) return;
  setPhase(cp(), t_phaseBeforeHostLoad);
  cp().hostWait.store(kCpHostWaitNone, std::memory_order_relaxed);
}
// render/shader-coverage: the graceful quit's CP drain (kernel/quit.cpp) bounds a translation wait in progress.
bool cpHostLoadDraining() { return sr::kernel::quit::cpDrainRequested(); }
const sr::gfx::ShaderCache::WaitObserver kCpHostLoadObserver{cpHostLoadBegin, cpHostLoadEnd, cpHostLoadDraining};

// render/boot-attach: main() reported that the live backend is not coming (its D3D12 init failed). Running the
// ring without a backend is the black-world path (ENGINE_MAP 6g.8d): stop, loudly. A backend that is merely late
// is waited for (gpuCpWaitForBackend), never given up on.
[[noreturn]] void cpBackendFatal(const char* reason) {
  SR_LOG_BOOT(sr::LogSeverity::Error,
              "[FATAL] the live D3D12 backend is not there: %s. The CP does not run the GPU command stream "
              "without it (that draws the black world, ENGINE_MAP 6g.8d) - exiting (3). SR_PM4_WAIT_BACKEND=0 "
              "runs on without it (diagnostic only)",
              reason);
#ifdef _WIN32
  const char* inst = std::getenv("SR_INSTANCE_ID");
  if (!(inst && *inst))  // a player; never an automated roll (no window may take the foreground there)
    MessageBoxW(nullptr,
                L"Superman Returns could not start its Direct3D 12 renderer, so it cannot draw the game.\n\n"
                L"Update the graphics driver and check that the graphics card supports Direct3D 12 "
                L"(feature level 11_0). The log file names the step that failed.",
                L"Superman Returns", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
  sr::Log::shutdown();
  std::_Exit(3);
}

void storeBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

#if SR_ORACLE
// step 6 G6a DIAG [DIAG_SWAP] (dev): a swap's latency from the guest's VdSwap call to the CP's XE_SWAP arrival (onSwap),
// and from that arrival to the next guest ISR call (onInterrupt), per 120 swaps: mean / p95 / max ms. The guest
// stamps a ring by its own swap count, and the CP pairs its count with it; the line shows both counts, so an offset
// between them is visible.
constexpr uint32_t kDiagSwapRing = 64;
std::atomic<uint64_t> g_diagGuestSwapNs[kDiagSwapRing];
std::atomic<uint64_t> g_diagGuestSwaps{0};
std::atomic<void (*)(uint64_t)> g_diagSwapCensusHook{nullptr};
thread_local uint32_t t_diagCpCallback = 0;   // 3 the swap's present, 4 a guest ISR ([DIAG_WAIT]'s signaller class)
struct SwapDiag {
  uint64_t arrivalNs = 0;   // the last XE_SWAP's arrival, until the first ISR after it
  std::vector<float> vdToCp, cpToIsr;
  uint64_t unpaired = 0;
};
SwapDiag& swapDiag() {   // the CP thread only
  static SwapDiag d;
  return d;
}
void diagSwapStats(std::vector<float>& v, double& mean, double& p95, double& mx) {
  mean = p95 = mx = 0;
  if (v.empty()) return;
  std::sort(v.begin(), v.end());
  double sum = 0;
  for (float x : v) sum += x;
  mean = sum / double(v.size());
  p95 = v[(std::min)(v.size() - 1, size_t(double(v.size()) * 0.95))];
  mx = v.back();
}
void diagSwapArrival(uint64_t cpSwap, uint64_t now) {
  SwapDiag& d = swapDiag();
  const uint64_t guest = g_diagGuestSwaps.load(std::memory_order_acquire);
  const uint64_t stamp = cpSwap && cpSwap <= guest && guest - cpSwap < kDiagSwapRing
                             ? g_diagGuestSwapNs[(cpSwap - 1u) % kDiagSwapRing].load(std::memory_order_relaxed)
                             : 0u;
  if (stamp && now >= stamp) d.vdToCp.push_back(float(double(now - stamp) * 1e-6));
  else ++d.unpaired;
  d.arrivalNs = now;
  if (cpSwap % 120u) return;
  double m1, p1, x1, m2, p2, x2;
  diagSwapStats(d.vdToCp, m1, p1, x1);
  diagSwapStats(d.cpToIsr, m2, p2, x2);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[DIAG_SWAP] cp_swaps=%llu guest_swaps=%llu vdswap->cp mean=%.2f p95=%.2f max=%.2f ms (n=%zu) "
              "cp->isr mean=%.2f p95=%.2f max=%.2f ms (n=%zu) unpaired=%llu",
              (unsigned long long)cpSwap, (unsigned long long)guest, m1, p1, x1, d.vdToCp.size(), m2, p2, x2,
              d.cpToIsr.size(), (unsigned long long)d.unpaired);
  d.vdToCp.clear();
  d.cpToIsr.clear();
  d.unpaired = 0;
  if (auto* hook = g_diagSwapCensusHook.load(std::memory_order_acquire)) hook(cpSwap);
}
void diagSwapIsr(uint64_t now) {
  SwapDiag& d = swapDiag();
  if (!d.arrivalNs) return;
  d.cpToIsr.push_back(float(double(now - d.arrivalNs) * 1e-6));
  d.arrivalNs = 0;
}
#endif

void onInterrupt(void* u, uint32_t cpuMask) {
  Cp& c = *static_cast<Cp*>(u);
  // Held through every per-CPU delivery and its device reads (see stubs.cpp,
  // VdSetGraphicsInterruptCallback): a setter returns only after it is released.
  const auto lease = graphics_interrupt::registrationService().begin();
  const uint32_t cb = lease ? lease.registration().callbackVA : 0u;
  const uint32_t ctx = lease ? lease.registration().contextVA : 0u;
  if (!cb || !c.gc) {
    static std::atomic<uint32_t> s_noIsr{0};
    if (s_noIsr.fetch_add(1, std::memory_order_relaxed) < 4)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  SR_SUBMIT_TAG " INTERRUPT mask=%X with no registered ISR - dropped", cpuMask);
    return;
  }
  PPCFunc* fn = PPC_LOOKUP_FUNC(getGuestBase(), cb);
  if (!fn) return;
  if (sr::native::p2trace::g_on) sr::native::p2trace::onIsr(cpuMask, ctx);  // iter-467 native-a P2 trace
  for (uint32_t n = 0; n < 6; n++) {
    if (!(cpuMask & (1u << n))) continue;
    // DPC impersonation on CPU n: the ISR and its continuations read the
    // processor number from KPCR+268 (`lbz r10,268(r13)` in sub_82110B00,
    // `1 << *(r13+268)` in GpuInterruptCallback_sub_820FDBA0).
    // iter-463 SR_KPCR: Xenia EmulateCPInterruptDPC - SetActiveCpu(n) (KPCR
    // and KTHREAD) and Begin/EndDPCImpersonation around the ISR call.
    const bool realPcr = kpcrEnabled() && kpcrForKThread(readGuestU32BE(c.pcrVA + 0x100u)) == c.pcrVA;
    if (realPcr) kpcrSetActiveCpu(c.pcrVA, uint8_t(n));
    else if (uint8_t* p = guestVAToHost(c.pcrVA + 268u)) *p = uint8_t(n);
    KpcrDpcScope dpc(realPcr ? c.pcrVA : 0u);
    const uint32_t cont = readGuestU32BE(readGuestU32BE(ctx + 10772u) + 16u);
    GuestArgs a{};
    a.r3 = 1;  // source 1: CP interrupt
    a.r4 = ctx;
    uint32_t exc = 0;
    const uint64_t t0 = nowNs();
    const uint32_t prevPhase = setPhase(c, 4);
#if SR_ORACLE
    diagSwapIsr(t0);   // step 6 G6a DIAG: the first guest ISR call after a swap's arrival (a dropped interrupt is none)
    const uint32_t prevCallback = t_diagCpCallback;
    t_diagCpCallback = 4;
#endif
    const bool ok = c.gc->call(fn, a, &exc);
#if SR_ORACLE
    t_diagCpCallback = prevCallback;
#endif
    setPhase(c, prevPhase);
    c.nsIsr.fetch_add(nowNs() - t0, std::memory_order_relaxed);
    const uint64_t k = c.isrCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!ok) c.isrFaults.fetch_add(1, std::memory_order_relaxed);
    if (k <= 8 || (k & (k - 1)) == 0 || !ok)
      SR_LOG_BOOT(ok ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
                  SR_SUBMIT_TAG " ISR #%llu cpu=%u cont=%08X %s exc=%08X",
                  (unsigned long long)k, n, cont, ok ? "ok" : "FAULT", exc);
  }
}

void onSwap(void* u, uint32_t fbPhys, uint32_t w, uint32_t h) {
  Cp& c = *static_cast<Cp*>(u);
  c.counter.fetch_add(1, std::memory_order_relaxed);
  const uint64_t s = c.swaps.fetch_add(1, std::memory_order_relaxed) + 1;
  if (s <= 4 || (s & (s - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, SR_SUBMIT_TAG " XE_SWAP #%llu fb=%08X %ux%u",
                (unsigned long long)s, fbPhys, w, h);
  const uint64_t t0 = nowNs();
#if SR_ORACLE
  diagSwapArrival(s, t0);   // step 6 G6a DIAG [DIAG_SWAP]
  const uint32_t prevCallback = t_diagCpCallback;
  t_diagCpCallback = 3;
#endif
  const uint32_t prevPhase = setPhase(c, 3);
  sr::gfx::cpPresentSwap(fbPhys, w, h);
  setPhase(c, prevPhase);
#if SR_ORACLE
  t_diagCpCallback = prevCallback;
#endif
  c.nsSwap.fetch_add(nowNs() - t0, std::memory_order_relaxed);
}

void onFlush(void*) { sr::gfx::cpFlushDraws(); }
void onZpdFence(void*) { sr::gfx::cpZpdFence(); }  // p2.zpd: the covering-fence check (SR_NATIVE_ZPD)
bool onRunning(void* u) { return static_cast<Cp*>(u)->running.load(std::memory_order_relaxed); }
uint32_t onCounter(void* u) { return static_cast<Cp*>(u)->counter.load(std::memory_order_relaxed); }
uint8_t* onVirtToHost(void*, uint32_t va) { return guestVAToHost(va); }

void writeBackRptr(sr::gfx::CpEngine& eng, uint32_t rptr) {
  const uint32_t phys = cp().rptrWbPhys.load(std::memory_order_acquire);
  if (!phys) return;
  if (uint8_t* p = eng.physToHost(eng.user, phys)) storeBE32(p, rptr);
}

void census(const char* why) {
  Cp& c = cp();
  auto& e = cpEngine();
  // The packet walker's own counts: ZPD / EXT query events and the IB / desync health. C1 stage 1: the release
  // configuration has no walker - each needs a packet, which is fatal there - so they read 0 (the line keeps its form).
#if SR_ORACLE
  const uint64_t wZpd = e.cpZpdEvents, wZpdFake = e.cpZpdFakeWrites, wExt = e.cpExtEvents,
                 wBadIb = e.badIndirectBuffers, wDesync = e.desyncStops;
#else
  const uint64_t wZpd = 0, wZpdFake = 0, wExt = 0, wBadIb = 0, wDesync = 0;
#endif
  uint32_t wptr;
  {
    std::lock_guard<std::mutex> lk(c.mu);
    wptr = c.wptr;
  }
  // Wall-time split since the previous census (CP thread only calls this).
  static uint64_t s_prevT = 0, s_prevIdle = 0, s_prevIb = 0, s_prevSwap = 0, s_prevIsr = 0,
                  s_prevSwaps = 0;
  const uint64_t t = nowNs(), idle = c.nsIdle.load(), ib = c.nsIb.load(),
                 sw = c.nsSwap.load(), isr = c.nsIsr.load(), swaps = c.swaps.load();
  if (s_prevT) {
    const double wall = double(t - s_prevT) * 1e-6;
    const double dIdle = double(idle - s_prevIdle) * 1e-6, dIb = double(ib - s_prevIb) * 1e-6,
                 dSw = double(sw - s_prevSwap) * 1e-6, dIsr = double(isr - s_prevIsr) * 1e-6;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                SR_SUBMIT_TIME_TAG " %s wall=%.0fms swaps=+%llu (%.1f/s) idle=%.0f ib=%.0f swap=%.0f "
                "isr=%.0f other=%.0f (ms)",
                why, wall, (unsigned long long)(swaps - s_prevSwaps),
                wall > 0 ? double(swaps - s_prevSwaps) * 1000.0 / wall : 0.0, dIdle, dIb, dSw,
                dIsr, wall - dIdle - dIb - dSw - dIsr);
  }
  s_prevT = t; s_prevIdle = idle; s_prevIb = ib; s_prevSwap = sw; s_prevIsr = isr;
  s_prevSwaps = swaps;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              SR_SUBMIT_TAG " %s rptr=%u wptr=%u doorbells=%llu packets=%llu draws=%llu "
              "isr=%llu (faults %llu) swaps=%llu waits=%llu (blocked %llu) scratch=%llu "
              "memWrites=%llu zpd=%llu (fake %llu) ext=%llu badIB=%llu desync=%llu | ringless ib=%llu bs=%llu "
              "desync=%llu seg=%llu",
              why, c.rptrShared.load(), wptr, (unsigned long long)c.doorbells.load(),
              (unsigned long long)c.packets.load(), (unsigned long long)e.draws,
              (unsigned long long)c.isrCalls.load(), (unsigned long long)c.isrFaults.load(),
              (unsigned long long)c.swaps.load(), (unsigned long long)e.cpWaits,
              (unsigned long long)e.cpWaitsBlocked, (unsigned long long)e.cpScratchWrites,
              (unsigned long long)e.cpMemWrites, (unsigned long long)wZpd,
              (unsigned long long)wZpdFake, (unsigned long long)wExt,
              (unsigned long long)wBadIb, (unsigned long long)wDesync,
              (unsigned long long)c.rlIb.load(), (unsigned long long)c.rlBs.load(),
              (unsigned long long)c.rlDesync.load(), (unsigned long long)c.rlSeg.load());
  // The coherency requests the CP applied (guest 11-dword blocks and kRkInvalidate records alike): top = ranges
  // reaching 4 GB, page = the bogus {0xFFFFF000, 0x1000} - the all-ones sentinel defect's witnesses (both 0 once fixed,
  // native/coher-sentinel). Its own tag: census_item selects the last SR_SUBMIT_TAG " census" line.
  SR_LOG_BOOT(sr::LogSeverity::Info, "[GPU_COHER] %s requests=%llu whole=%llu top=%llu page=%llu", why,
              (unsigned long long)e.cpCoher, (unsigned long long)e.cpCoherWhole, (unsigned long long)e.cpCoherTop,
              (unsigned long long)e.cpCoherPage);
  sr::gfx::memres::censusLine(why, swaps);
  sr::kernel::ringless::finalLine(why);  // iter-467 native-a P2-A: the producer-side counts (gate ON only)
  sr::native::transport::finalLine(why);
  sr::native::vblank::finalLine(why);     // display.registers: counts per vblank / pacing call, dispreg by source
  sr::native::devcreate::finalLine(why);  // boot.device_create: the device-create ports' counts (gate ON only)
  sr::gfx::zpd::logLine(why);             // p2.zpd: the native occlusion queries' counts (SR_NATIVE_ZPD only)
}

void cpMain() {
  Cp& c = cp();
  GuestCaller gc("GPU_CP");
  c.gc = &gc;
  // tools/graceful-quit: the ISRs this thread runs never park at an import; the drain point below stops it.
  sr::kernel::quit::markExempt("GPU_CP");
  // The CP thread's own processor control region: DPC impersonation writes
  // the active CPU number at +268 before each ISR call.
  // iter-463 SR_KPCR: GuestCaller already built this thread's real KPCR
  // (kernel/kpcr.hpp: prcb.current_thread = its KTHREAD) - use it. Gate off =
  // the private zeroed page as before.
  if (kpcrEnabled() && gc.ctx().r13.u32) {
    c.pcrVA = gc.ctx().r13.u32;
  } else {
    const uint32_t pcrRaw = guestHeapAlloc(0x2000);
    c.pcrVA = pcrRaw ? (pcrRaw + 0xFFFu) & ~0xFFFu : 0u;
    if (uint8_t* p = c.pcrVA ? guestVAToHost(c.pcrVA) : nullptr) std::memset(p, 0, 0x1000);
  }
  gc.ctx().r13.u32 = c.pcrVA;

  auto& eng = cpEngine();
  eng.cp.user = &c;
  eng.cp.interrupt = onInterrupt;
  eng.cp.swap = onSwap;
  eng.cp.flush = onFlush;
  eng.cp.running = onRunning;
  eng.cp.counter = onCounter;
  eng.cp.virtToHost = onVirtToHost;
  if (sr::gfx::zpd::on()) eng.cp.zpdFence = onZpdFence;  // p2.zpd: native query covering-fence callback
  if (sr::native::p2trace::g_on) eng.cp.trace = sr::native::p2trace::cpEvent;  // iter-467 native-a P2 trace
  eng.cpMode = true;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              SR_SUBMIT_TAG " command processor thread up (pcr=%08X guestCaller=%s)", c.pcrVA,
              gc.ok() ? "ok" : "DEAD");
  // iter-467 native-b (SR_PM4_WAIT_BACKEND, baked ON; =0 off): the GPU exists before the first command it executes.
  // main() starts the guest thread BEFORE d3d12.init() (6-8 s) and attaches the live backend after it; a CP that starts
  // first parses the boot frames without performing a draw or resolve, and its draw-command queue (8192 entries)
  // fills. job65 (exe f795c4795efe, 6 interleaved default-env gameplay rolls): =0 attached after 19 / 55 / 55 guest
  // VdSwaps with 3,024 / 8,191 / 8,191 commands queued - the two saturated rolls drew the 3D world BLACK under the HUD
  // (the eye-adaptation history NaN for good, ENGINE_MAP 6g.8d), the other drew; =1 waited 7.9 / 6.4 / 7.4 s,
  // attached at 0 swaps / 0 queued, drew 3 of 3; boot-to-title 20.6-23.8 s either way (one =0 outlier 42.6 s); the EA
  // logo movie plays whole (78 frames at speed 0.81-0.95) where =0 cut it short and raced the next one at 5-13x. The
  // guest waits on its own fences meanwhile, as behind a busy GPU.
  // Platform fix: the console's GPU is ready at power-on.
  // render/boot-attach (docs/NATIVE_ROADMAP.md "Boot attach"): the shader cache loads on its own thread, so the
  // attach's remaining cost is the device, swap chain and PSOs (plus the probe, a directory listing). The wait has
  // no time limit (gpuCpWaitForBackend): a failed D3D12 init, which main() reports at once, is FATAL (loud, exit 3);
  // a late attach is waited for (a Warn every 10 s; phase 5, [HOST_WAIT_STALL] past 60 s) - never the ring without a
  // backend, the black-world path. Then WP1: the CP waits for the installed shader cache before its first command
  // (no limit either: the alternative is a draw without its shader), so the first IM_LOAD and every draw after it
  // see the whole cache.
  t_isCpThread = true;
  sr::kernel::hostWorkMarkPresentingThread(/*cp=*/true);   // step 4 2k(4): host work here can hold a frame
  sr::gfx::ShaderCache::setWaitObserver(&kCpHostLoadObserver);
  {
#if SR_ORACLE
    static const bool s_wait = [] {
      const char* e = std::getenv("SR_PM4_WAIT_BACKEND");
      return !e || e[0] != '0';
    }();
#else
    constexpr bool s_wait = true;
#endif
    if (s_wait && sr::gfx::pm4LiveEnabled() && !sr::gfx::pm4LiveBackendAttached())
      gpuCpWaitForBackend(BackendWaitProbe{});
    if (s_wait && sr::gfx::pm4LiveEnabled()) {
      auto& shaderCache = sr::gfx::getShaderCache();
      const uint64_t t1 = nowNs();
      shaderCache.ensureInstalledLoaded();
      // "ready after WP1": the CP may consume its first command later (the [GPU_CP] first command line).
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[SHADER_CACHE] CP start: waited %.1f ms for the installed shader cache (%s; translator stage %s), "
                  "ready after WP1 at process +%.1f ms",
                  double(nowNs() - t1) / 1e6, shaderCache.installedPresent() ? "loaded" : "none installed",
                  shaderCache.fullyLoaded() ? "done" : "still loading", sr::gfx::msSinceProcessStart());
    }
  }
  // Step 4 item 7a (SR_PM4_EARLY_INIT=1, default OFF): the frame renderer's one-time GPU setup here, on this thread,
  // before the first command - not inside the guest's first swap, where it held the boot -> intro hand-off 0.9 s
  // (gfx/d3d12_frame_render.cpp D3D12Backend::pm4EarlyInit). No-op with the gate off or no live backend.
  sr::gfx::cpEarlyInit();

#if SR_ORACLE
  OracleRingPacket pkt;
#endif
  uint64_t lastCensusSwaps = 0;
  // render/boot-attach (Astra round 3): the first command is recorded where it is consumed - in the item and the
  // packet path, after their completeness checks (gpuCpTakeCommand) - and logged once.
  GpuCpFirstCommand first;
  bool firstLogged = false;
  const auto logFirst = [&](uint32_t rptr, uint32_t wptrNow) {
    firstLogged = true;
    SR_LOG_BOOT(sr::LogSeverity::Info,
                SR_SUBMIT_TAG " first command consumed at process +%.1f ms (%s at rptr %u, wptr %u; the live backend %s)",
                first.atMs, first.kind == 1 ? "a host item" : "a ring packet", rptr, wptrNow,
                sr::gfx::pm4LiveBackendAttached() ? "attached" : "NOT attached");
  };
  for (;;) {
    uint32_t base, mask, wptr;
    {
      std::unique_lock<std::mutex> lk(c.mu);
      const uint64_t tw = nowNs();
      setPhase(c, 0);
      c.cv.wait(lk, [&] {
        return !c.running || (c.published && c.wptr != c.rptr) || sr::kernel::quit::cpDrainRequested();
      });
      setPhase(c, 1);
      c.nsIdle.fetch_add(nowNs() - tw, std::memory_order_relaxed);
      if (!c.running) break;
      base = c.ringBase;
      mask = c.mask;
      wptr = c.wptr;
    }
    // ExecutePrimaryBuffer(read, write): packet by packet, wrap-aware.
    uint32_t budget = mask + 1u;
    while (c.rptr != wptr && budget) {
      // iter-467 native-a P2-A: a host item at this index stands where the XDK would have written the
      // ring packet (the ring memory there is stale). Items behind the read index were never
      // consumable (a producer / consumer index mismatch): dropped and counted.
      RlItem it{};
      bool haveItem = false;
      {
        std::lock_guard<std::mutex> lk(c.rlMu);
        while (!c.rl.empty() && ((c.rl.front().index - c.rptr) & mask) > (mask >> 1)) {
          c.rl.pop_front();
          c.rlDesync.fetch_add(1, std::memory_order_relaxed);
        }
        if (!c.rl.empty() && c.rl.front().index == c.rptr) {
          it = c.rl.front();
          c.rl.pop_front();
          haveItem = true;
        }
      }
      if (haveItem) {
        if (!gpuCpTakeCommand(it.n, (wptr - c.rptr) & mask, 1, sr::gfx::msSinceProcessStart, first)) {
          std::lock_guard<std::mutex> lk(c.rlMu);  // its index space is not published yet: wait for it
          c.rl.push_front(it);
          break;
        }
        if (!firstLogged) logFirst(c.rptr, wptr);
        if (it.kind == 1u || it.kind == 3u) {
          const uint64_t ti = nowNs();
          const uint64_t swapBefore = c.nsSwap.load(std::memory_order_relaxed),
                         isrBefore = c.nsIsr.load(std::memory_order_relaxed);
          setPhase(c, 2);
          // kind 3 (P2-B): the segment's part list - arena records + the gaps' PM4 - in order, found by
          // the item's id (never by its phys: p2c_abp). A native item failure is fatal; its
          // prefix may have executed, so interpreting the segment would replay side effects.
          if (sr::native::p2trace::g_on)  // the CP-side item sequence (the segmentless wedge's evidence)
            sr::native::p2trace::event(sr::native::p2trace::kCpItem, uint16_t(it.kind), uint32_t(it.id), it.a, it.b);
          if (it.kind == 1u)
#if SR_ORACLE
            sr::gfx::executeGuestIndirectBuffer(it.a, it.b);
#else
            cpNoWalkerFatal("a plain indirect buffer (kind-1 ring item, IB phys / dwords)", it.a, it.b);
#endif
          else if (sr::native::flush::cpSegment(eng, it.id, it.a, it.b))
            c.rlSeg.fetch_add(1, std::memory_order_relaxed);
          else
            eng.nativeRecordFailed = true;
          setPhase(c, 1);
          c.nsIb.fetch_add((nowNs() - ti) - (c.nsSwap.load(std::memory_order_relaxed) - swapBefore) -
                               (c.nsIsr.load(std::memory_order_relaxed) - isrBefore),
                           std::memory_order_relaxed);
          c.rlIb.fetch_add(1, std::memory_order_relaxed);
        } else {
#if !SR_ORACLE
          if (it.kind != 2u)
            cpNoWalkerFatal("an unknown native queue item (kind / index)", it.kind, it.index);
#endif
          eng.applyBinReg(2u, it.a);  // SET_BIN_SELECT_LO
          c.rlBs.fetch_add(1, std::memory_order_relaxed);
        }
        if (eng.nativeRecordFailed) {
          SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_FLUSH_BAD] native consumer stopped; ring item is not acknowledged");
          sr::Log::shutdown();
          std::_Exit(3);
        }
        c.rptr = (c.rptr + it.n) & mask;
        c.rptrShared.store(c.rptr, std::memory_order_relaxed);
        budget -= it.n < budget ? it.n : budget;
        writeBackRptr(eng, c.rptr);
        if (!c.running) break;
        continue;
      }
#if !SR_ORACLE
      // C1 stage 1 (docs/RELEASE_COMPOSITION.md 9.9): no host item at the read index means a ring packet - the release
      // configuration parses none (no PM4 walker; the ring memory is never read): fatal, never skipped.
      (void)base;
      cpNoWalkerFatal("a ring packet (no host item at the read index; rptr / wptr)", c.rptr, wptr);
#else
      gpuCpPeekOraclePacket(base, c.rptr, pkt);
      const uint32_t hdr = pkt.header, n = pkt.dwords;
      const uint32_t avail = (wptr - c.rptr) & mask;
      if (!gpuCpTakeCommand(n, avail, 2, sr::gfx::msSinceProcessStart, first)) {
        static std::atomic<uint32_t> s_ov{0};
        if (s_ov.fetch_add(1, std::memory_order_relaxed) < 8)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
                      SR_SUBMIT_TAG " packet hdr=%08X needs %u dwords, only %u published - "
                      "waiting for the rest", hdr, n, avail);
        break;
      }
      if (!firstLogged) logFirst(c.rptr, wptr);
      gpuCpPrepareOraclePacket(pkt, base, mask, c.rptr);
      if (gpuCpOraclePacketIndirect(pkt)) {
        const uint64_t ti = nowNs();
        const uint64_t swapBefore = c.nsSwap.load(std::memory_order_relaxed),
                       isrBefore = c.nsIsr.load(std::memory_order_relaxed);
        setPhase(c, 2);
        gpuCpExecuteOraclePacket(eng, pkt, c.rptr);
        setPhase(c, 1);
        c.nsIb.fetch_add((nowNs() - ti) - (c.nsSwap.load(std::memory_order_relaxed) - swapBefore) -
                             (c.nsIsr.load(std::memory_order_relaxed) - isrBefore),
                         std::memory_order_relaxed);
      } else {
        gpuCpExecuteOraclePacket(eng, pkt, c.rptr);
      }
      if (eng.nativeRecordFailed) {
        SR_LOG_BOOT(sr::LogSeverity::Error, "[NATIVE_FLUSH_BAD] native consumer stopped; ring packet is not acknowledged");
        sr::Log::shutdown();
        std::_Exit(3);
      }
      c.rptr = (c.rptr + n) & mask;
      c.rptrShared.store(c.rptr, std::memory_order_relaxed);
      budget -= n < budget ? n : budget;
      c.packets.fetch_add(1, std::memory_order_relaxed);
      writeBackRptr(eng, c.rptr);
      if (!c.running) break;
#endif  // SR_ORACLE (the ring packet path)
    }
    // The ring is consumed up to wptr: translate what is queued before idling. Under the native
    // executor (SR_NATIVE_EXEC) that is a kick, not a wait - nothing the guest observes depends on it
    // (the primary ring's packets were consumed at parse; fences / interrupts are stream items).
    sr::gfx::cpIdleFlush();
    writeBackRptr(eng, c.rptr);
    const uint64_t sw = c.swaps.load(std::memory_order_relaxed);
    if (sw != lastCensusSwaps && (sw <= 4 || sw % 120 == 0)) {
      lastCensusSwaps = sw;
      census("census");
    }
    // tools/graceful-quit (kernel/quit.hpp, docs/TOOLKIT.md 1.8m): THE DRAIN POINT. The harness asked the game to
    // quit and the ring is cut (no doorbell publishes any more), so the write index is final. Once the read index
    // reaches it, everything the guest submitted has been consumed; cpFlushDraws() then has the native executor /
    // drain worker perform everything queued, and this thread parks for good, idle, holding no lock. The ring items
    // left in the queue were pushed for index space no doorbell published (built, never submitted).
    if (sr::kernel::quit::cpDrainRequested()) {
      uint32_t w;
      bool pub;
      {
        std::lock_guard<std::mutex> lk(c.mu);
        w = c.wptr;
        pub = c.published;
      }
      if (!pub || c.rptr == w) {
        const uint64_t tf = nowNs();
        sr::gfx::cpFlushDraws();
        writeBackRptr(eng, c.rptr);
        sr::kernel::quit::CpDrained d;
        d.rptr = c.rptr;
        d.wptr = w;
        d.swaps = c.swaps.load();
        d.packets = c.packets.load();
        d.doorbells = c.doorbells.load();
        {
          std::lock_guard<std::mutex> lk(c.rlMu);
          d.rlPending = uint32_t(c.rl.size());
        }
        d.flushMs = uint32_t((nowNs() - tf) / 1000000ull);
        setPhase(c, 0);
        sr::kernel::quit::cpParkDrained(d);
      }
    }
  }
  census("exit");
  c.gc = nullptr;
}

}  // namespace

bool gpuCpActive() { return sr::gfx::pm4CpAsyncEnabled(); }

bool gpuFakeCompletion() {
  static const bool v = !sr::gfx::pm4CpAsyncEnabled();
  return v;
}

bool spliceVpkF16() {
  // iter-467: the FLOAT16_2 pack (Xenia float_to_xenos_half) equals the old body bit for bit for every
  // production input (whole-function L6, and a full gameplay census with 0 differing words). The black
  // gameplay world once blamed on it was a startup race in every build (41572437, SR_PM4_WAIT_BACKEND;
  // ENGINE_MAP 6g.8d). R1 (docs/SPLICE_CONVERSION.md): XenonRecomp emits the pack itself
  // (tools/regen/patches/0005), so the guest code has no other path and the SR_SPLICE_VPK_F16 A/B knob
  // went with the hand body. Always true; kept so a native port of the callers keeps asking the one
  // predicate the guest code agrees with.
  return true;
}

void gpuCpDoorbell(uint32_t ringBaseVA, uint32_t indexMask, uint32_t writeIndex) {
  Cp& c = cp();
  if (!c.started.exchange(true)) {
    // First kick: the ring holds everything the runtime wrote since
    // VdInitializeRingBuffer (Xenia starts its read index at 0 there).
    char line[400];
    int p = 0;
#if SR_ORACLE  // C1 stage 1: the ring memory's first dwords (a diagnostic read); the release configuration reads no ring
    for (uint32_t i = 0; i < writeIndex && i < 32 && p < int(sizeof(line)) - 12; i++)
      p += snprintf(line + p, sizeof(line) - size_t(p), "%08X ",
                    readGuestU32BE(ringBaseVA + i * 4u));
#else
    line[0] = '\0';
    (void)p;
#endif
    SR_LOG_BOOT(sr::LogSeverity::Info,
                SR_SUBMIT_TAG " first doorbell: ring=%08X dwords=%u wptr=%u rptrWb=%08X | %s",
                ringBaseVA, indexMask + 1u, writeIndex, c.rptrWbPhys.load(), line);
    std::thread(cpMain).detach();
    gpuHangWdStart();   // iter-463 (SR_GPU_HANG_WD): the wall-clock hang watchdog
  }
  // tools/graceful-quit (kernel/quit.hpp publishUnlessCut): the ring cut is written under c.mu (gpuCpCutRing) and
  // tested here under c.mu - the no-request cost is ONE relaxed load inside the lock this doorbell takes anyway, no
  // RMW, no fence. A doorbell after the cut publishes nothing: its thread parks, or (holding a guest spinlock /
  // critical section, e.g. 820FD800 under dev+0x2A78) the publish is dropped and the thread runs on to its release.
  // The only publisher of CP work, so the published write index is final once the cut returns.
  uint64_t d = 0;
  if (!sr::kernel::quit::publishUnlessCut(c.mu, [&] {
        d = c.doorbells.fetch_add(1, std::memory_order_relaxed) + 1;
        c.ringBase = ringBaseVA;
        c.mask = indexMask;
        c.wptr = writeIndex & indexMask;
        c.published = true;
        c.ringBaseMirror.store(ringBaseVA, std::memory_order_relaxed);
        c.wptrMirror.store(c.wptr, std::memory_order_relaxed);
        c.publishedMirror.store(true, std::memory_order_release);
      }))
    return;
  c.cv.notify_one();
  if (d <= 8 || (d & (d - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Info, SR_SUBMIT_TAG " doorbell #%llu wptr=%u rptr=%u",
                (unsigned long long)d, writeIndex, c.rptrShared.load());
}

void gpuCpPushItem(uint32_t kind, uint32_t index, uint32_t a, uint32_t b) {
#if !SR_ORACLE
  if (kind != 2u)
    cpNoWalkerFatal("a non-bin-select queue publication (kind / index)", kind, index);
#endif
  Cp& c = cp();
  std::lock_guard<std::mutex> lk(c.rlMu);
  c.rl.push_back(RlItem{kind, index, kind == 2u ? 2u : 3u, a, b, 0ull});
}

void gpuCpPushSegment(uint32_t index, uint32_t phys, uint32_t dwords, uint64_t id) {
#if !SR_ORACLE
  if (!id)
    cpNoWalkerFatal("a segment without a native publication id (phys / dwords)", phys, dwords);
#endif
  Cp& c = cp();
  std::lock_guard<std::mutex> lk(c.rlMu);
  c.rl.push_back(RlItem{3u, index, 3u, phys, dwords, id});
}

void gpuCpSetRptrWriteback(uint32_t phys, uint32_t blockSizeLog2) {
  cp().rptrWbPhys.store(phys, std::memory_order_release);
  gpuHangWdNoteRptrWriteback(phys);   // iter-463: fence block = rptrWb - 0x3C
  physLifoPreadvanceArm();   // iter-463 E3 diagnostic trigger (inert unless SR_PHYS_LIFO_PREADVANCE)
  SR_LOG_BOOT(sr::LogSeverity::Info, SR_SUBMIT_TAG " rptr write-back -> phys %08X (block log2 %u)",
              phys, blockSizeLog2);
}

void gpuCpVblank() {
  Cp& c = cp();
  c.counter.fetch_add(1, std::memory_order_relaxed);
  // Stall watchdog: a phase frozen > 5 s (or idle with unread ring data) is a
  // hang - log it and snapshot every thread's stack once.
  if (!c.started.load(std::memory_order_relaxed)) return;
  // iter-463: with SR_GPU_HANG_WD (default ON) this check runs as H3 on the
  // watchdog's own thread, where it also sees a waiter under an idle CP.
  if (gpuHangWdEnabled()) return;
  const uint64_t since = c.phaseSinceNs.load(std::memory_order_relaxed);
  if (!since) return;
  const uint64_t age = nowNs() - since;
  if (age < 5000000000ull) return;
  static uint64_t s_reportedSince = 0;
  if (s_reportedSince == since) return;
  s_reportedSince = since;
  uint32_t wptr = 0;
  bool published = false;
  {
    std::lock_guard<std::mutex> lk(c.mu);
    wptr = c.wptr;
    published = c.published;
  }
  const uint32_t ph = c.phase.load(std::memory_order_relaxed);
  const uint32_t rptr = c.rptrShared.load(std::memory_order_relaxed);
  if (ph == 0 && (!published || wptr == rptr)) return;  // idle with nothing queued: guest-side quiet
  if (ph == kCpPhaseHostWait) return;  // render/boot-attach: the backend's attach or the boot shader cache
  static const char* kNames[] = {"idle", "ring-packet", "indirect-buffer", "swap", "isr"};
  SR_LOG_BOOT(sr::LogSeverity::Error,
              SR_SUBMIT_STALL_TAG " phase=%s for %.1fs rptr=%u wptr=%u published=%d swaps=%llu",
              ph < 5 ? kNames[ph] : "?", double(age) * 1e-9, rptr, wptr, published ? 1 : 0,
              (unsigned long long)c.swaps.load());
  static bool s_dumped = false;
  if (!s_dumped) {
    s_dumped = true;
    std::error_code ec;
    std::filesystem::create_directories("archive", ec);
    const bool ok = sr::writeDiagnosticMinidump("archive/gpu_cp_stall.dmp");
    SR_LOG_BOOT(sr::LogSeverity::Error, SR_SUBMIT_STALL_TAG " minidump -> archive/gpu_cp_stall.dmp (%s)",
                ok ? "written" : "FAILED");
  }
}

void gpuCpFinalLine() { census("final"); }

// tools/graceful-quit: the terminal census. Called by kernel/quit.cpp only after the CP thread has parked at its
// drain point, so the census's CP-thread state (the [GPU_CP_TIME] deltas) has no other writer.
void gpuCpCensusLine(const char* why) { census(why); }

void gpuCpQuitWake() {
  Cp& c = cp();
  { std::lock_guard<std::mutex> lk(c.mu); }  // a waiter between its predicate and its sleep cannot miss the notify
  c.cv.notify_one();
}

uint32_t gpuCpRlPending() {
  Cp& c = cp();
  std::lock_guard<std::mutex> lk(c.rlMu);
  return uint32_t(c.rl.size());
}

void gpuCpCutRing() { sr::kernel::quit::cutRingUnder(cp().mu); }

GpuCpSnapshot gpuCpSnapshot() {
  Cp& c = cp();
  GpuCpSnapshot s;
  s.started = c.started.load(std::memory_order_relaxed);
  s.phase = c.phase.load(std::memory_order_relaxed);
  s.hostWait = c.hostWait.load(std::memory_order_relaxed);
  s.phaseSinceNs = c.phaseSinceNs.load(std::memory_order_relaxed);
  s.ringBase = c.ringBaseMirror.load(std::memory_order_relaxed);
  s.published = c.publishedMirror.load(std::memory_order_acquire);
  s.wptr = c.wptrMirror.load(std::memory_order_relaxed);
  s.rptr = c.rptrShared.load(std::memory_order_relaxed);
  s.doorbells = c.doorbells.load(std::memory_order_relaxed);
  s.packets = c.packets.load(std::memory_order_relaxed);
  s.swaps = c.swaps.load(std::memory_order_relaxed);
  s.isrCalls = c.isrCalls.load(std::memory_order_relaxed);
  s.isrFaults = c.isrFaults.load(std::memory_order_relaxed);
  return s;
}

namespace {
std::atomic<uint64_t> g_backendWaitWarns{0};
void realSleepMs(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
}  // namespace

uint64_t gpuCpBackendWaitWarnsForTest() { return g_backendWaitWarns.load(std::memory_order_relaxed); }

bool gpuCpTakeCommand(uint32_t need, uint32_t avail, uint32_t kind, double (*nowMs)(), GpuCpFirstCommand& first) {
  if (need > avail) return false;  // the rest is not published yet: nothing is consumed
  if (!first.taken) first = GpuCpFirstCommand{true, nowMs ? nowMs() : -1.0, kind};
  return true;
}

const char* gpuCpHostWaitName(uint32_t what) {
  switch (what) {
    case kCpHostWaitBackend: return "the live backend's attach";
    case kCpHostWaitInstalled: return "the installed shader cache (WP1 / WP2)";
    case kCpHostWaitTranslator: return "the shader cache's translator stage (WP2)";
    case kCpHostWaitTranslation: return "a runtime shader translation (the miss policy)";
    default: return "the host";
  }
}

// render/boot-attach: the declaration (gpu_submit.hpp) states the policy. No time limit on purpose: the
// one way the backend never comes is a failed D3D12 init, and main() reports that at once (failPm4LiveBackend).
uint64_t gpuCpWaitForBackend(const BackendWaitProbe& p) {
  bool (*attached)() = p.attached ? p.attached : sr::gfx::pm4LiveBackendAttached;
  bool (*failed)() = p.failed ? p.failed : sr::gfx::pm4LiveBackendFailed;
  const char* (*reason)() = p.reason ? p.reason : sr::gfx::pm4LiveBackendFailReason;
  uint64_t (*now)() = p.nowNs ? p.nowNs : nowNs;
  void (*sleepMs)(uint32_t) = p.sleepMs ? p.sleepMs : realSleepMs;
  const uint64_t warnNs = uint64_t(p.warnEveryMs ? p.warnEveryMs : 10000u) * 1000000ull;
  g_backendWaitWarns.store(0, std::memory_order_relaxed);
  Cp& c = cp();
  c.hostWait.store(kCpHostWaitBackend, std::memory_order_relaxed);
  const uint32_t before = setPhase(c, kCpPhaseHostWait);
  const uint64_t t0 = now();
  uint64_t nextWarn = warnNs;
  bool ok = false;
  for (;;) {
    if (attached()) {
      ok = true;
      break;
    }
    if (failed()) break;
    const uint64_t waited = now() - t0;
    if (waited >= nextWarn) {
      g_backendWaitWarns.fetch_add(1, std::memory_order_relaxed);
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[DISPLAY] CP still waiting for the live backend after %.0f s (no time limit: the CP never runs "
                  "the GPU command stream without it; a failed D3D12 init ends the wait with exit 3)",
                  double(waited) / 1e9);
      nextWarn = (waited / warnNs + 1) * warnNs;
    }
    sleepMs(1);
  }
  const uint64_t waited = now() - t0;
  setPhase(c, before);
  c.hostWait.store(kCpHostWaitNone, std::memory_order_relaxed);
  if (!ok) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[DISPLAY] CP waited %.1f ms for the live backend (INIT FAILED)",
                double(waited) / 1e6);
    cpBackendFatal(reason());
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[DISPLAY] CP waited %.1f ms for the live backend (attached), %llu still-waiting warnings",
              double(waited) / 1e6, (unsigned long long)g_backendWaitWarns.load(std::memory_order_relaxed));
  return waited;
}

#if SR_ORACLE
// step 6 G6a DIAG (gpu_submit.hpp).
void diagSwapNoteGuest() noexcept {
  const uint64_t n = g_diagGuestSwaps.load(std::memory_order_relaxed) + 1u;   // the presenting guest thread only
  g_diagGuestSwapNs[(n - 1u) % kDiagSwapRing].store(nowNs(), std::memory_order_relaxed);
  g_diagGuestSwaps.store(n, std::memory_order_release);
}
uint32_t diagCpCallback() noexcept { return t_diagCpCallback; }
void diagSetSwapCensusHook(void (*hook)(uint64_t swaps)) noexcept {
  g_diagSwapCensusHook.store(hook, std::memory_order_release);
}
#endif

}  // namespace sr::kernel
