// iter-463 SR_TIMEBASE_50MHZ: one 50 MHz guest clock domain. See kernel/sr_timebase.hpp.
#include "kernel/sr_timebase.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif
#include <x86intrin.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {

// Plain globals, seated by the static initializer below BEFORE main() (so before the
// first guest instruction); no function-local static guard in the hot path.
bool g_tb50 = false;
bool g_slowScale = false;
uint64_t g_num = 5, g_den = 1;
int64_t g_qpc0 = 0;
uint64_t g_ftBase = 0;
std::atomic<bool> g_bundlePublished{false};

uint64_t gcd64(uint64_t a, uint64_t b) {
  while (b) {
    const uint64_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

bool readGate() {
  // iter-463: BAKED default ON (A/B rolls archive/v1_fix2/ab2 + ab3: no new crash or
  // freeze, sim dt and rate unchanged, load-screen sequencer + shader time at 1.000x
  // wall vs 74x legacy). =0 is the legacy A/B (raw TSC / steady / FILETIME epochs).
  const char* e = std::getenv("SR_TIMEBASE_50MHZ");
  return !e || e[0] != '0';
}

int64_t qpcNow() {
#if defined(_WIN32)
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return c.QuadPart;
#else
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

void seat(bool enable) {
  int64_t freq = 1000000000;
#if defined(_WIN32)
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  freq = f.QuadPart;
#endif
  uint64_t num = sr::timebase::kGuestHz, den = static_cast<uint64_t>(freq);
  const uint64_t g = gcd64(num, den);
  num /= g;
  den /= g;
  g_num = num;
  g_den = den;
  // (d % den) * num must not overflow for the split form to be exact.
  g_slowScale = den > 1 && (den - 1) > UINT64_MAX / num;
#if defined(_WIN32)
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  g_ftBase = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
#else
  g_ftBase = 116444736000000000ull;
#endif
  g_qpc0 = qpcNow();
  g_tb50 = enable;
}

struct Init {
  Init() { seat(readGate()); }
} g_init;

// Single aligned stores (never byte-by-byte): the guest reads these fields with one
// lwz / ld while the ticker thread rewrites them, so a torn value must be impossible.
inline void storeBE64(uint8_t* p, uint64_t v) {
  __atomic_store_n(reinterpret_cast<uint64_t*>(p), __builtin_bswap64(v), __ATOMIC_RELAXED);
}
inline void storeBE32(uint8_t* p, uint32_t v) {
  __atomic_store_n(reinterpret_cast<uint32_t*>(p), __builtin_bswap32(v), __ATOMIC_RELAXED);
}

}  // namespace

namespace sr::timebase {

bool enabled() noexcept { return g_tb50; }
uint64_t scaleNum() noexcept { return g_num; }
uint64_t scaleDen() noexcept { return g_den; }
int64_t qpcOrigin() noexcept { return g_qpc0; }
uint64_t systemTimeBase100ns() noexcept { return g_ftBase; }

uint64_t scaleSplit(uint64_t d, uint64_t num, uint64_t den) noexcept {
  return (d / den) * num + ((d % den) * num) / den;
}

uint64_t mulDivExact(uint64_t d, uint64_t num, uint64_t den) noexcept {
  const unsigned __int128 p = static_cast<unsigned __int128>(d) * num;  // 64x64 mul, no libcall
  const uint64_t hi = static_cast<uint64_t>(p >> 64);
  const uint64_t lo = static_cast<uint64_t>(p);
  if (den == 0 || hi >= den) return UINT64_MAX;  // quotient does not fit
  uint64_t rem = hi, q = 0;
  for (int i = 63; i >= 0; --i) {
    const uint64_t carry = rem >> 63;
    rem = (rem << 1) | ((lo >> i) & 1u);
    q <<= 1;
    if (carry || rem >= den) {
      rem -= den;
      q |= 1u;
    }
  }
  return q;
}

uint64_t guestTicks() noexcept {
  const uint64_t d = static_cast<uint64_t>(qpcNow() - g_qpc0);
  return g_slowScale ? mulDivExact(d, g_num, g_den) : scaleSplit(d, g_num, g_den);
}

uint64_t systemTime100ns(uint64_t ticks) noexcept { return g_ftBase + ticks / 5u; }

uint64_t writeBundle(uint8_t* bundle) noexcept {
  const uint64_t t = guestTicks();
  const uint64_t ms = t / 50000u;
  storeBE64(bundle + 0x00, t / 5u);
  storeBE64(bundle + 0x08, systemTime100ns(t));
  storeBE32(bundle + 0x10, ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ms));
  return t;
}

PeriodicMs::PeriodicMs() noexcept {
#if defined(_WIN32)
  h_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                              TIMER_ALL_ACCESS);
  hr_ = h_ != nullptr;
#endif
}

PeriodicMs::~PeriodicMs() {
#if defined(_WIN32)
  if (h_) CloseHandle(h_);
#endif
}

void PeriodicMs::wait() noexcept {
#if defined(_WIN32)
  if (h_) {
    if (period_ == 0) {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      period_ = f.QuadPart / 1000;
      next_ = qpcNow();
    }
    next_ += period_;
    const int64_t now = qpcNow();
    if (next_ <= now) {             // late (preempted): re-base, no catch-up burst
      next_ = now;
      return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -((next_ - now) * 10000 / period_);   // relative, 100 ns units
    if (due.QuadPart == 0) due.QuadPart = -1;
    if (SetWaitableTimer(h_, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(h_, INFINITE);
      return;
    }
  }
#endif
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void setBundlePublished(bool on) noexcept { g_bundlePublished.store(on, std::memory_order_release); }

void reinitForTest(bool enable) noexcept { seat(enable); }

}  // namespace sr::timebase

extern "C" uint64_t sr_guest_mftb() noexcept {
  if (!g_tb50) return __rdtsc();  // legacy A/B: raw host TSC
  return sr::timebase::guestTicks();
}

extern "C" bool srTimebaseBundleLive() noexcept {
  return g_tb50 && g_bundlePublished.load(std::memory_order_acquire);
}
