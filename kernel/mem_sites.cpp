// Step 4 item 7d: [MEM_SITE] names for the big host allocations (see mem_sites.hpp).
#include "mem_sites.hpp"

#include "../app/log.hpp"

#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace sr::kernel {

namespace {
constexpr uint64_t kMinBytes = 16ull << 20;
struct Region {
  uint64_t committed = 0;
  DWORD type = 0;
};
using Snapshot = std::map<uintptr_t, Region>;   // allocation base -> its committed bytes

Snapshot walk() {
  Snapshot out;
#if defined(_WIN32)
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  uintptr_t p = uintptr_t(si.lpMinimumApplicationAddress);
  const uintptr_t end = uintptr_t(si.lpMaximumApplicationAddress);
  MEMORY_BASIC_INFORMATION mbi;
  while (p < end && VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) == sizeof(mbi)) {
    if (mbi.State == MEM_COMMIT && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED)) {
      Region& r = out[uintptr_t(mbi.AllocationBase)];
      r.committed += mbi.RegionSize;
      r.type = mbi.Type;
    }
    p = uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
  }
#endif
  return out;
}

std::mutex g_mu;
// Bases already logged, with the committed size then (an inner scope names first; the outer ones skip them). Astra
// (10:2x) P3: a base whose block is gone at a later walk is forgotten (address reuse is named again), and one that grew
// by >= 16 MB since is named again.
std::map<uintptr_t, uint64_t> g_named;
}  // namespace

bool memSitesOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_MEM_SITES");
    return e && e[0] == '1';
  }();
  return on;
}

MemSiteScope::MemSiteScope(const char* name) : m_name(name) {
  if (!memSitesOn()) return;
  m_before = new Snapshot(walk());
}

MemSiteScope::~MemSiteScope() {
  if (!m_before) return;
  auto* before = static_cast<Snapshot*>(m_before);
  const Snapshot after = walk();
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto it = g_named.begin(); it != g_named.end();)   // freed since: a later block at that base is a new one
    it = after.count(it->first) ? std::next(it) : g_named.erase(it);
  for (const auto& [base, r] : after) {
    if (r.committed < kMinBytes) continue;
    const auto named = g_named.find(base);
    if (named != g_named.end() && r.committed < named->second + kMinBytes) continue;
    const auto it = before->find(base);
    const uint64_t was = it == before->end() ? 0 : it->second.committed;
    if (r.committed < was + kMinBytes) continue;
    g_named[base] = r.committed;
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MEM_SITE] %s: +%.1f MB %s at %llx appeared during this scope (SR_MEM_SITES, "
                "step 4 7d: an interval observation - any thread's allocation in the interval counts; not ownership)",
                m_name, double(r.committed - was) / 1048576.0, r.type == MEM_MAPPED ? "mapped" : "private",
                (unsigned long long)base);
  }
  delete before;
}

}  // namespace sr::kernel
