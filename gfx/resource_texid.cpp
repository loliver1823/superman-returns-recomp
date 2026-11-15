// mem.resources `textures_from_resolves`: the texture-identity census - see resource_texid.hpp. Class oracle.
#include "resource_texid.hpp"

#include "../app/log.hpp"
#include "../kernel/quit.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>

namespace sr::gfx::memres {
namespace {

constexpr uint32_t kClasses = uint32_t(TexIdClass::kCount);
constexpr uint32_t kKinds = uint32_t(TexIdKind::kCount);
std::atomic<uint64_t> g_bind[kKinds][kClasses];
std::atomic<uint64_t> g_gpuPages{0}, g_pages{0}, g_cpuAfterPages{0};
std::atomic<uint32_t> g_sampled[kClasses];
std::atomic<int64_t> g_lastReportMs{0};

const char* className(TexIdClass c) {
  switch (c) {
    case TexIdClass::kPlain: return "plain";
    case TexIdClass::kIdentity: return "identity";
    case TexIdClass::kArena: return "arena";
    case TexIdClass::kCpuSnapshot: return "cpu_snapshot";
    case TexIdClass::kGuestBytes: return "guest_bytes";
    case TexIdClass::kStale: return "stale";
    case TexIdClass::kUnknown: return "unknown";
    case TexIdClass::kCpuAfterGpu: return "cpu_after_gpu";
    default: return "?";
  }
}
const char* kindName(TexIdKind k) {
  return k == TexIdKind::k2D ? "2d" : k == TexIdKind::k2DClean ? "2d_clean" : "array";
}

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

void report(const char* why) { texIdReport(why); }

}  // namespace

bool texIdOn() noexcept {
  static const bool on = [] {
    const char* e = std::getenv("SR_MEM_RES_TEXID");
    const bool v = e && e[0] == '1';
    if (v) {
      sr::kernel::quit::addFamily("resource_texid", report);
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[MEM_RES_TEXID] enabled=1: every texture bind classified by resolve output on its pages and the "
                  "bound content's origin (identity / arena / cpu_snapshot / guest_bytes / stale / unknown / plain)");
    }
    return v;
  }();
  return on;
}

void texIdBind(TexIdKind kind, TexIdClass cls, uint32_t pages, uint32_t gpuPages, uint32_t base, uint32_t fmt,
               uint32_t cpuAfterPages, uint32_t src) noexcept {
  if (!texIdOn() || kind >= TexIdKind::kCount || cls >= TexIdClass::kCount) return;
  g_bind[uint32_t(kind)][uint32_t(cls)].fetch_add(1, std::memory_order_relaxed);
  g_pages.fetch_add(pages, std::memory_order_relaxed);
  g_gpuPages.fetch_add(gpuPages, std::memory_order_relaxed);
  g_cpuAfterPages.fetch_add(cpuAfterPages, std::memory_order_relaxed);
  if (cls != TexIdClass::kPlain) {
    const bool bad = cls == TexIdClass::kGuestBytes || cls == TexIdClass::kStale || cls == TexIdClass::kUnknown ||
                     cls == TexIdClass::kCpuAfterGpu;
    const uint32_t n = g_sampled[uint32_t(cls)].fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || (bad && n <= 64))
      SR_LOG_BOOT(bad ? sr::LogSeverity::Warn : sr::LogSeverity::Info,
                  "[MEM_RES_TEXID] SAMPLE %s #%u kind=%s base=%08X fmt=%u pages=%u gpu_pages=%u cpu_after_pages=%u "
                  "src=%u", className(cls), n, kindName(kind), base, fmt, pages, gpuPages, cpuAfterPages, src);
  }
  const int64_t t = nowMs(), last = g_lastReportMs.load(std::memory_order_relaxed);
  if (t - last >= 5000) {
    int64_t expect = last;
    if (g_lastReportMs.compare_exchange_strong(expect, t, std::memory_order_relaxed)) texIdReport("periodic");
  }
}

void texIdReport(const char* why) noexcept {
  if (!texIdOn()) return;
  uint64_t tot[kClasses] = {}, byKind[kKinds] = {};
  for (uint32_t k = 0; k < kKinds; k++)
    for (uint32_t c = 0; c < kClasses; c++) {
      const uint64_t v = g_bind[k][c].load(std::memory_order_relaxed);
      tot[c] += v;
      if (c != uint32_t(TexIdClass::kPlain)) byKind[k] += v;
    }
  uint64_t binds = 0, resolveBinds = 0;
  for (uint32_t c = 0; c < kClasses; c++) {
    binds += tot[c];
    if (c != uint32_t(TexIdClass::kPlain)) resolveBinds += tot[c];
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[MEM_RES_TEXID] %s binds=%llu resolve_binds=%llu plain=%llu identity=%llu arena=%llu cpu_snapshot=%llu "
              "guest_bytes=%llu stale=%llu unknown=%llu cpu_after_gpu=%llu | resolve_by_kind tex2d=%llu tex2d_clean=%llu "
              "array=%llu | pages=%llu gpu_pages=%llu cpu_after_pages=%llu",
              why ? why : "?", (unsigned long long)binds, (unsigned long long)resolveBinds,
              (unsigned long long)tot[uint32_t(TexIdClass::kPlain)],
              (unsigned long long)tot[uint32_t(TexIdClass::kIdentity)],
              (unsigned long long)tot[uint32_t(TexIdClass::kArena)],
              (unsigned long long)tot[uint32_t(TexIdClass::kCpuSnapshot)],
              (unsigned long long)tot[uint32_t(TexIdClass::kGuestBytes)],
              (unsigned long long)tot[uint32_t(TexIdClass::kStale)],
              (unsigned long long)tot[uint32_t(TexIdClass::kUnknown)],
              (unsigned long long)tot[uint32_t(TexIdClass::kCpuAfterGpu)], (unsigned long long)byKind[0],
              (unsigned long long)byKind[1], (unsigned long long)byKind[2],
              (unsigned long long)g_pages.load(std::memory_order_relaxed),
              (unsigned long long)g_gpuPages.load(std::memory_order_relaxed),
              (unsigned long long)g_cpuAfterPages.load(std::memory_order_relaxed));
}

}  // namespace sr::gfx::memres
