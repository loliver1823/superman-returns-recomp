#pragma once
// iter-463 SR_TIMEBASE_50MHZ: ONE guest clock domain (Xenia's default, non-raw path).
//
// The console has a single 50 MHz timebase: mftb, KeQueryPerformanceCounter (the XDK
// inlines QueryPerformanceCounter as mftb, sub_824680D0), and the kernel's
// KeTimeStampBundle / KeQuerySystemTime all derive from it. Xenia models it as a guest
// tick count at set_guest_tick_frequency(50000000) derived from the host clock, with
// uptime ms = ticks/50000 and system time = base FILETIME + ticks/5 (xenia base/clock.cc).
//
// Before this, mftb was the raw host TSC (~3.7 GHz = 74x fast), KeQueryPerformanceCounter
// was steady_clock/100 (10 MHz against a 50 MHz frequency), and there were three separate
// millisecond epochs (bundle ticker, the sub_824680C0 splice, the sub_82564E80 override).
//
// Gate: SR_TIMEBASE_50MHZ (default ON since the iter-463 A/B rolls; =0 keeps legacy).
// NOT included by kernel/guest_context.hpp (that header rebuilds every guest TU).
#include <cstdint>

// The guest's mftb (force-included into the generated TUs via guest/sr_mftb.h).
// Gate on: 50 MHz guest ticks since process start. Gate off: raw __rdtsc() (legacy).
extern "C" uint64_t sr_guest_mftb() noexcept;

// True when the spliced sub_824680C0 should run its ORIGINAL body (read the live
// KeTimeStampBundle ms): gate on AND the bundle has been published at 0x820007D0.
extern "C" bool srTimebaseBundleLive() noexcept;

namespace sr::timebase {

constexpr uint64_t kGuestHz = 50000000ull;  // Xenon timebase (Xenia emulator.cc)
constexpr uint32_t kBundleVA = 0xD0012000u;  // our KeTimeStampBundle (iter-303, guest_context.cpp)

bool enabled() noexcept;

// 50 MHz guest ticks since the clock origin (process start), independent of the gate.
uint64_t guestTicks() noexcept;

// Host QPC -> guest ticks scale, reduced: ticks = d * num / den (num/den = 50e6/QPF).
uint64_t scaleNum() noexcept;
uint64_t scaleDen() noexcept;
int64_t qpcOrigin() noexcept;

// floor(d * num / den) without 128-bit division (clang+MSVC CRT has no __udivti3):
// (d/den)*num + ((d%den)*num)/den. Exact whenever (den-1)*num fits in 64 bits and the
// result fits in 64 bits; init() checks the first condition and falls back to mulDivExact.
uint64_t scaleSplit(uint64_t d, uint64_t num, uint64_t den) noexcept;
// Exact floor(d * num / den) for any inputs whose result fits in 64 bits (software
// 128/64 long division; the reference for the unit test and the slow-path fallback).
uint64_t mulDivExact(uint64_t d, uint64_t num, uint64_t den) noexcept;

// Guest system time (FILETIME, 100 ns since 1601) = FILETIME at the origin + ticks/5.
uint64_t systemTimeBase100ns() noexcept;
uint64_t systemTime100ns(uint64_t ticks) noexcept;

// KeTimeStampBundle fields from ONE guest tick read (all in the same domain):
//   +0x00 interrupt time (100 ns) = ticks/5
//   +0x08 system time             = base + ticks/5
//   +0x10 tick count (ms)         = min(ticks/50000, UINT32_MAX)   (xenia QueryGuestUptimeMillis)
// Writes big-endian into `bundle` (host pointer to guest memory). Returns the ticks used.
uint64_t writeBundle(uint8_t* bundle) noexcept;

// The bundle ticker's 1 ms period: a Windows high-resolution waitable timer
// (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION) - per-timer, so it does NOT change the process
// timer quantum the way timeBeginPeriod(1) would (SR_TIMER_RES_1MS stays opt-in).
// Falls back to sleep_for(1ms) where the flag is unavailable.
class PeriodicMs {
 public:
  PeriodicMs() noexcept;
  ~PeriodicMs();
  // Blocks until the next 1 ms boundary of a fixed schedule (drift-compensated: the
  // due time is target - now, so wake-up overhead does not stretch the period; a late
  // wake re-bases the schedule instead of bursting).
  void wait() noexcept;
  bool highResolution() const noexcept { return hr_; }
 private:
  void* h_ = nullptr;
  bool hr_ = false;
  int64_t next_ = 0;     // QPC ticks of the next boundary
  int64_t period_ = 0;   // QPC ticks per ms
};

// Set by the kernel once the bundle pointer is published at 0x820007D0.
void setBundlePublished(bool on) noexcept;

// Test-only: re-read the gate / re-seat the origin (the difftest drives both modes).
void reinitForTest(bool enable) noexcept;

}  // namespace sr::timebase
