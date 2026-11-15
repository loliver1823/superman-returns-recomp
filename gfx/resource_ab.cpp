// mem.resources frozen-pass A/B of the native resource route against the page model - see resource_ab.hpp.
// Class oracle (ORACLE_SOURCES): the release configuration's resourceDecides() is the header's constant true.
#include "resource_ab.hpp"

#include "resource_memory.hpp"
#include "../app/log.hpp"
#include "../kernel/ab_toggle.hpp"
#include "../kernel/highfps.hpp"
#include "../kernel/quit.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace sr::gfx::memres {
namespace {

enum Counter : uint32_t {
  kSetsNative = 0, kSetsModel, kPresentedNative, kPresentedModel, kRtEnter,
  kDecNative, kDecModel, kDecNativeInModel, kDecModelInNative, kUnlatched, kDecUnlatched,
  kDumpedNative, kDumpedModel, kAltBreak, kAltPairs, kEmptySets,
  kEventBase,  // AbEvent counters follow
  kCounters = kEventBase + uint32_t(AbEvent::kCount)
};
std::atomic<uint64_t> g_c[kCounters];

thread_local int8_t t_arm = -1;   // -1 none, 0 model, 1 native
thread_local bool t_setBegun = false;  // the CP: a set began since the last present
std::atomic<int8_t> g_nextArm{1};  // the next presenting set's arm (SR_AB_PRESENT=1)
std::atomic<int> g_bit{-1};
// The previous dumped present (the render thread only): consecutive dumps must alternate.
uint64_t g_lastDumpNo = ~0ull;
int8_t g_lastDumpArm = -1;
std::mutex g_dumpMu;

bool presentMode() {
  static const bool v = [] {
    const char* e = std::getenv("SR_AB_PRESENT");
    return e && e[0] == '1';
  }();
  return v;
}

uint64_t c(Counter k) { return g_c[k].load(std::memory_order_relaxed); }
uint64_t ev(AbEvent e) { return g_c[kEventBase + uint32_t(e)].load(std::memory_order_relaxed); }

void report(const char* why) { resourceAbReport(why); }

}  // namespace

bool resourceAbDual() noexcept {
  static const bool on = [] {
    if (!resourceMode()) return false;  // the toggle switches the native route OFF in its off arm; it never turns it on
    const int bit = sr::abt::reg("SR_MEM_RESOURCES");
    if (bit < 0) return false;
    g_bit.store(bit, std::memory_order_relaxed);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[MEM_RES_AB] dual=1 bit=%d present_mode=%d: the registry and the page model keep their state in every "
                "present; the native route decides in %s, the page model in the others (CP-latched per frame set)",
                bit, presentMode() ? 1 : 0,
                presentMode() ? "alternate presenting sets (strict)" : "the pace windows the A/B mask holds ON");
    return true;
  }();
  return on;
}

bool resourceDecides() noexcept {
  if (!resourceMode()) return false;
  if (!resourceAbDual()) return true;
#if defined(SR_MEM_RES_AB_MUTANT_NOOP)  // compiled control: the toggle ignored at the decision sites
  return true;
#elif defined(SR_MEM_RES_AB_MUTANT_MODEL)  // compiled control: the model decides in every arm
  return false;
#else
  const int8_t a = t_arm;
  if (a >= 0) return a != 0;
  const uint64_t n = g_c[kUnlatched].fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MEM_RES_AB] UNLATCHED #%llu: a resource decision on a thread with no frame-set "
                "arm (the next set's arm %d is used)", (unsigned long long)n, int(g_nextArm.load()));
  return presentMode() ? g_nextArm.load(std::memory_order_relaxed) != 0
                       : sr::abt::on(g_bit.load(std::memory_order_relaxed));
#endif
}

void resourceAbBeginSet() noexcept {
  if (!resourceAbDual()) return;
  static std::once_flag s_family;  // the terminal family, registered on the CP (a normal thread context)
  std::call_once(s_family, [] { sr::kernel::quit::addFamily("resource_ab", report); });
  const bool native = presentMode() ? g_nextArm.load(std::memory_order_relaxed) != 0
                                    : sr::abt::on(g_bit.load(std::memory_order_relaxed));
  t_arm = native ? 1 : 0;
  t_setBegun = true;
  g_c[native ? kSetsNative : kSetsModel].fetch_add(1, std::memory_order_relaxed);
}

int8_t resourceAbPresentingSet() noexcept {
  if (!resourceAbDual()) return t_arm;
  if (!t_setBegun) {  // no drain began a set since the last present: this present is a set of its own
    resourceAbBeginSet();
    g_c[kEmptySets].fetch_add(1, std::memory_order_relaxed);
  }
  return t_arm;
}

void resourceAbPresented() noexcept {
  if (!resourceAbDual()) return;
  t_setBegun = false;  // the next drain with a command begins the next presenting set
  const int8_t a = t_arm;
  const uint64_t n = g_c[a == 1 ? kPresentedNative : kPresentedModel].fetch_add(1, std::memory_order_relaxed) + 1 +
                     c(a == 1 ? kPresentedModel : kPresentedNative);
  if (presentMode() && a >= 0) g_nextArm.store(a == 1 ? 0 : 1, std::memory_order_relaxed);
  if (n % 600u == 0u) resourceAbReport("periodic");
}

int8_t resourceAbArm() noexcept { return t_arm; }

void resourceAbEnter(int8_t arm) noexcept {
  if (!resourceAbDual()) return;
  t_arm = arm;
  g_c[kRtEnter].fetch_add(1, std::memory_order_relaxed);
}

void resourceAbDecision(AbWho who, uint32_t pages) noexcept {
  if (!resourceAbDual() || !pages) return;
  const int8_t a = t_arm;
  if (a < 0) g_c[kDecUnlatched].fetch_add(pages, std::memory_order_relaxed);
  if (who == AbWho::kNative) {
    g_c[kDecNative].fetch_add(pages, std::memory_order_relaxed);
    if (a == 0) g_c[kDecNativeInModel].fetch_add(pages, std::memory_order_relaxed);
  } else {
    g_c[kDecModel].fetch_add(pages, std::memory_order_relaxed);
    if (a == 1) g_c[kDecModelInNative].fetch_add(pages, std::memory_order_relaxed);
  }
}

void resourceAbCount(AbEvent event, uint64_t n) noexcept {
  if (!resourceAbDual() || !n || event >= AbEvent::kCount) return;
  const uint64_t was = g_c[kEventBase + uint32_t(event)].fetch_add(n, std::memory_order_relaxed);
  if ((event == AbEvent::kCrossClobber || event == AbEvent::kCrossWindow || event == AbEvent::kModelReadbackPages) &&
      (was < 8 || ((was + 1) & was) == 0))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MEM_RES_AB] %s #%llu (arm %d)",
                event == AbEvent::kCrossClobber ? "CROSS_CLOBBER" :
                event == AbEvent::kCrossWindow ? "CROSS_WINDOW" : "MODEL_READBACK",
                (unsigned long long)(was + n), int(t_arm));
}

void resourceAbDumped(uint64_t presentNo) noexcept {
  if (!resourceAbDual()) return;
  const int8_t a = t_arm;
  {
    std::lock_guard<std::mutex> lk(g_dumpMu);
    if (g_lastDumpNo != ~0ull && presentNo == g_lastDumpNo + 1) {
      g_c[kAltPairs].fetch_add(1, std::memory_order_relaxed);
      if (a == g_lastDumpArm) g_c[kAltBreak].fetch_add(1, std::memory_order_relaxed);
    }
    g_lastDumpNo = presentNo;
    g_lastDumpArm = a;
  }
  const uint64_t n = g_c[a == 1 ? kDumpedNative : kDumpedModel].fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 16 || sr::kernel::highfps::freezeHeldMs() > 0.0)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MEM_RES_AB] present=%llu arm=%c", (unsigned long long)presentNo,
                a == 1 ? 'N' : a == 0 ? 'M' : '?');
}

AbTotals resourceAbTotals() noexcept {
  AbTotals t;
  t.setsNative = c(kSetsNative);
  t.setsModel = c(kSetsModel);
  t.decNative = c(kDecNative);
  t.decModel = c(kDecModel);
  t.nativeInModel = c(kDecNativeInModel);
  t.modelInNative = c(kDecModelInNative);
  t.unlatched = c(kUnlatched);
  t.decUnlatched = c(kDecUnlatched);
  t.altPairs = c(kAltPairs);
  t.altBreak = c(kAltBreak);
  t.emptySets = c(kEmptySets);
  for (uint32_t e = 0; e < uint32_t(AbEvent::kCount); e++) t.event[e] = ev(AbEvent(e));
  return t;
}

void resourceAbReport(const char* why) noexcept {
  if (!resourceAbDual()) return;
  if (why && std::strcmp(why, "terminal") == 0)  // the activation's terminal twin (tools/perf/native_proof.py)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MEM_RES_AB] terminal dual=1 bit=%d present_mode=%d",
                g_bit.load(std::memory_order_relaxed), presentMode() ? 1 : 0);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[MEM_RES_AB] %s sets native=%llu model=%llu empty=%llu presented native=%llu model=%llu rt_enter=%llu | "
              "decisions "
              "native=%llu model=%llu native_in_model=%llu model_in_native=%llu unlatched=%llu dec_unlatched=%llu | uploads "
              "native=%llu "
              "model=%llu | inv_m forgot=%llu adopted=%llu | inv_r cross_window=%llu cross_clobber=%llu "
              "cross_unannounced=%llu preserve=%llu reestablished=%llu | model_readback=%llu gpu_note_model_arm=%llu | "
              "dumped native=%llu model=%llu "
              "alt_pairs=%llu alt_break=%llu",
              why ? why : "?", (unsigned long long)c(kSetsNative), (unsigned long long)c(kSetsModel),
              (unsigned long long)c(kEmptySets), (unsigned long long)c(kPresentedNative), (unsigned long long)c(kPresentedModel),
              (unsigned long long)c(kRtEnter), (unsigned long long)c(kDecNative), (unsigned long long)c(kDecModel),
              (unsigned long long)c(kDecNativeInModel), (unsigned long long)c(kDecModelInNative),
              (unsigned long long)c(kUnlatched), (unsigned long long)c(kDecUnlatched),
              (unsigned long long)ev(AbEvent::kNativeUploadPages),
              (unsigned long long)ev(AbEvent::kModelUploadPages), (unsigned long long)ev(AbEvent::kModelForgot),
              (unsigned long long)ev(AbEvent::kModelAdopted), (unsigned long long)ev(AbEvent::kCrossWindow),
              (unsigned long long)ev(AbEvent::kCrossClobber), (unsigned long long)ev(AbEvent::kCrossUnannounced),
              (unsigned long long)ev(AbEvent::kModelPreserve), (unsigned long long)ev(AbEvent::kModelReestablished),
              (unsigned long long)ev(AbEvent::kModelReadbackPages), (unsigned long long)ev(AbEvent::kGpuNoteModelArm),
              (unsigned long long)c(kDumpedNative), (unsigned long long)c(kDumpedModel),
              (unsigned long long)c(kAltPairs), (unsigned long long)c(kAltBreak));
}

}  // namespace sr::gfx::memres
