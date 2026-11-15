#pragma once
// Foreign guest-page protection (2026-10-14, the transport pair's arithmetic admission; NATIVE_ROADMAP 'TRANSPORT PAIR
// ADMISSION A'). The guest arena is one committed read-write allocation in both memory models
// (guest/guest_harness.cpp allocGuestMemory / allocGuestMemoryUnified); NtFreeVirtualMemory zeroes or recycles inside
// it and never decommits. The GPU write tracker (kernel/gpu_write_watch.cpp) makes pages of the 0x8 / 0xA windows
// PAGE_READONLY, and its handler releases them on the first CPU store, a guest's or a host's. Any other host code that
// makes guest-arena pages non-writable must declare the host range here BEFORE its protection change:
//   - kernel/image_write_watchdog.cpp wdProtect: the always-on boot page (Scope::Range), and its opt-in modes
//     (SR_WRITE_WATCH_VA, SR_IMAGE_RO, and the runtime arm behind SR_XMV_WATCH / SR_XMV_EXTRA_WATCH / SR_ALLOC_WATCH)
//     as Scope::All;
//   - kernel/audio_cmdq_obs.cpp protectRange (SR_PROTECT_ZERO): Scope::All.
//   - kernel/native_hardware_memory.cpp protectHardwareReservation: the fixed
//     hardware reservation, Scope::Range, before release guest execution.
// Declarations are permanent: a range stays declared after its pages are restored. A consumer that admits guest
// stores by arithmetic (kernel/native_transport.cpp) refuses every store once diagnosticRequested() or excludesAll(),
// and any store that touches a declared range. tools/difftest/guest_protect_census.py fails when code in the exe's
// build inputs changes, allocates, maps or frees memory, or arms a watch, at a site it has not classified.
//
// THE RULE FOR A RUNTIME PROTECTOR (Astra round A). A declaration publishes; it does not quiesce. A call can pass
// excluded() / intersects() and only then reach its store, while another thread declares and protects in between.
// Every protector listed above is safe because its opt-in gate is read by diagnosticRequested() at the consumer's
// first call (before any of them can run), or because it protects before guest code runs (the watchdog's boot page).
// A future protector that can start while the pair runs needs the same: pre-exclusion through a gate listed in
// kDiagnosticGates, or a quiescence protocol (an ArenaLease-style drain of the calls in flight) before it protects.
// Lock-free and allocation-free: declare() may run inside a vectored exception handler.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <thread>

namespace sr::kernel::guest_protect {
enum class Scope : uint8_t { Range, All };
constexpr unsigned kSlots = 32;
inline std::atomic<uintptr_t> g_lo[kSlots]{}, g_hi[kSlots]{};
inline std::atomic<unsigned> g_claimed{0};
inline std::atomic<bool> g_all{false};
inline std::atomic<uint32_t> g_declarations{0};

inline void declare(const void* host, size_t bytes, Scope scope) noexcept {
  g_declarations.fetch_add(1, std::memory_order_relaxed);
  const uintptr_t lo = reinterpret_cast<uintptr_t>(host);
  // Fail safe: a diagnostic, an unusable range or a full table excludes every store.
  if (scope == Scope::All || !lo || !bytes || bytes > UINTPTR_MAX - lo) {
    g_all.store(true, std::memory_order_seq_cst);
    return;
  }
  const uintptr_t hi = lo + bytes;
  // A range declared before (the periodic re-protection of the same page) takes no new slot.
  const unsigned n = std::min(g_claimed.load(std::memory_order_acquire), kSlots);
  for (unsigned i = 0; i < n; ++i)
    if (g_hi[i].load(std::memory_order_acquire) == hi && g_lo[i].load(std::memory_order_acquire) == lo) return;
  const unsigned slot = g_claimed.fetch_add(1, std::memory_order_acq_rel);
  if (slot >= kSlots) {
    g_all.store(true, std::memory_order_seq_cst);
    return;
  }
  g_lo[slot].store(lo, std::memory_order_release);
  g_hi[slot].store(hi, std::memory_order_seq_cst);  // publishes the slot: readers check hi first
}

// Any Scope::All declaration (or a fail-safe one) made so far.
inline bool excludesAll() noexcept { return g_all.load(std::memory_order_seq_cst); }

// [host, host + bytes) overlaps a declared range. A slot claimed but not yet written counts as overlapping.
inline bool intersects(const void* host, size_t bytes) noexcept {
  const uintptr_t lo = reinterpret_cast<uintptr_t>(host), hi = lo + bytes;
  const unsigned n = std::min(g_claimed.load(std::memory_order_seq_cst), kSlots);
  for (unsigned i = 0; i < n; ++i) {
    const uintptr_t h = g_hi[i].load(std::memory_order_acquire);
    if (!h) return true;
    if (lo < h && g_lo[i].load(std::memory_order_acquire) < hi) return true;
  }
  return false;
}

inline uint32_t declarations() noexcept { return g_declarations.load(std::memory_order_relaxed); }

// The arena's lifetime (Astra round A, P1). Arithmetic admission trusts the registered arena to stay mapped, so a
// consumer holds an ArenaLease across every guest access it makes - its own loads and stores and the imports it
// calls, the park's wait included - and declines when the lease is refused. retireArena() runs before the arena is
// released (kernel/guest_context.cpp releaseGuestArena): it refuses every later lease, then waits for the leases in
// flight. Dekker order, seq_cst on both sides: a lease counts itself before it reads the retired flag, the retirer sets
// the flag before it reads the count, so either the lease sees the flag and backs out or the retirer sees the lease and
// waits. Retirement is permanent.
inline std::atomic<uint32_t> g_arenaLeases{0};
inline std::atomic<bool> g_arenaRetired{false};
class ArenaLease {
 public:
  ArenaLease() noexcept {
    g_arenaLeases.fetch_add(1, std::memory_order_seq_cst);
    if (g_arenaRetired.load(std::memory_order_seq_cst)) release();
  }
  ~ArenaLease() { release(); }
  ArenaLease(const ArenaLease&) = delete;
  ArenaLease& operator=(const ArenaLease&) = delete;
  explicit operator bool() const noexcept { return held_; }
  void release() noexcept {
    if (held_) {
      held_ = false;
      g_arenaLeases.fetch_sub(1, std::memory_order_seq_cst);
    }
  }
 private:
  bool held_ = true;
};
inline uint32_t arenaLeases() noexcept { return g_arenaLeases.load(std::memory_order_seq_cst); }
inline bool arenaRetired() noexcept { return g_arenaRetired.load(std::memory_order_seq_cst); }
// Refuse every later lease, then wait up to timeoutMs for the leases in flight: true when none is left (the arena may
// be released), false when some call still holds it (the caller must keep the arena mapped).
inline bool retireArena(uint32_t timeoutMs) noexcept {
  g_arenaRetired.store(true, std::memory_order_seq_cst);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (g_arenaLeases.load(std::memory_order_seq_cst)) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

// The opt-in protectors' own gates, read once so a consumer refuses from its first call - before any of them runs, and
// for those that arm at run time (the XMV / alloc write watches) before the arm. Each name is a superset of its owner's
// predicate (any set value counts for SR_WRITE_WATCH_VA, SR_ALLOC_WATCH counts without SR_ALLOW_FORCING):
// SR_PROTECT_ZERO (audio_cmdq_obs.cpp protectZeroRequested), SR_IMAGE_RO (image_write_watchdog.cpp imageRoDiag),
// SR_WRITE_WATCH_VA (installImageWriteWatchdog), SR_XMV_WATCH / SR_XMV_EXTRA_WATCH (cs_timeout_underflow_fix.cpp),
// SR_ALLOC_WATCH (gpu_ring_alloc_fix.cpp allocWatchOn).
inline constexpr const char* kDiagnosticGates[] = {"SR_PROTECT_ZERO", "SR_IMAGE_RO", "SR_XMV_WATCH",
                                                   "SR_XMV_EXTRA_WATCH", "SR_ALLOC_WATCH"};
inline constexpr const char* kDiagnosticAddress = "SR_WRITE_WATCH_VA";
inline bool diagnosticRequested() noexcept {
  static const bool requested = [] {
    for (const char* name : kDiagnosticGates) {
      const char* value = std::getenv(name);
      if (value && value[0] == '1') return true;
    }
    const char* address = std::getenv(kDiagnosticAddress);
    return address && address[0] != '\0';
  }();
  return requested;
}
}  // namespace sr::kernel::guest_protect

namespace sr::kernel {
// The guest arena's release at teardown (guest/guest_harness.cpp freeGuestMemory; kernel/guest_context.cpp). It retires
// the arena first: later native calls decline, calls in flight drain within kArenaRetireMs. When they do not (a pump
// parked in its wait for good), the release is skipped and the arena stays mapped - a leak at exit, never a native
// entry's access to freed memory. Scope (Astra round B): a declined native entry falls back to the SDK body, and guest
// threads that survive the release still run guest code; that pre-existing teardown race is not addressed here.
// Returns whether the arena was released. The only release of a registered arena in the exe
// (tools/difftest/guest_protect_census.py classifies every other release site).
constexpr uint32_t kArenaRetireMs = 250;
bool releaseGuestArena(uint8_t* base, size_t size) noexcept;
}  // namespace sr::kernel
