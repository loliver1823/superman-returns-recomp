// step4 stuck-load lane: the [GUEST_ALLOC_FAIL] breadcrumb. Contract in guest_alloc_fail.hpp.
#include "guest_alloc_fail.hpp"
#include "guest_context.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <windows.h>

extern "C" sr::kernel::GuestThreadSlot g_debugThreadRegistry[64];

namespace sr::kernel {

namespace {

std::atomic<uint32_t> g_fail[static_cast<uint32_t>(GuestAllocApi::kCount)];

// The calling thread's guest label (setGuestThreadName), copied out of the registry: "-" when unnamed, "?" when the
// thread is not a registered guest thread.
void currentGuestThreadName(uint32_t tid, char (&out)[32]) {
  std::snprintf(out, sizeof out, "%s", "?");
  for (int i = 0; i < 64; ++i) {
    if (g_debugThreadRegistry[i].tid != tid) continue;
    char tmp[32];
    std::memcpy(tmp, g_debugThreadRegistry[i].name, sizeof tmp);   // racy snapshot of a fixed-size buffer
    tmp[31] = '\0';
    std::snprintf(out, sizeof out, "%s", tmp[0] ? tmp : "-");
    return;
  }
}

}  // namespace

const char* guestAllocApiName(GuestAllocApi api) {
  switch (api) {
    case GuestAllocApi::XamAlloc: return "XamAlloc";
    case GuestAllocApi::NtAllocateVirtualMemory: return "NtAllocateVirtualMemory";
    case GuestAllocApi::MmAllocatePhysicalMemoryEx: return "MmAllocatePhysicalMemoryEx";
    case GuestAllocApi::RtlAllocateHeap: return "RtlAllocateHeap";
    case GuestAllocApi::kCount: break;
  }
  return "?";
}

bool guestAllocFailLogged(uint32_t n) { return n != 0 && (n <= 16 || (n & (n - 1)) == 0); }

bool noteGuestAllocFailure(GuestAllocApi api, uint32_t size, uint32_t flags, uint32_t prot, const char* why) {
  const uint32_t i = static_cast<uint32_t>(api);
  if (i >= static_cast<uint32_t>(GuestAllocApi::kCount)) return false;
  const uint32_t n = g_fail[i].fetch_add(1, std::memory_order_relaxed) + 1;
  if (!guestAllocFailLogged(n)) return false;
  const uint32_t tid = GetCurrentThreadId();
  char name[32];
  currentGuestThreadName(tid, name);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[GUEST_ALLOC_FAIL] #%u %s size=0x%X flags=0x%08X prot=0x%08X lr=%08X tid=%u thread=%s handle=%08X (%s)",
              n, guestAllocApiName(api), size, flags, prot, getCurrentGuestLR(), tid, name, getCurrentThreadHandle(),
              why ? why : "-");
  return true;
}

uint32_t guestAllocFailCount(GuestAllocApi api) {
  const uint32_t i = static_cast<uint32_t>(api);
  return i < static_cast<uint32_t>(GuestAllocApi::kCount) ? g_fail[i].load(std::memory_order_relaxed) : 0;
}

void resetGuestAllocFailForTest() {
  for (auto& c : g_fail) c.store(0, std::memory_order_relaxed);
}

}  // namespace sr::kernel
