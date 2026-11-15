// iter-463 D-AUDIO-CMDBUF: offline checks for kernel/audio_cmdq_obs (the [EA_RDR] anomaly
// predicate, the [RWAC_Q_STALL] detector, the S1 protect_zero mechanics).
//
// Run: build/difftest.exe --cmdq-test   (exit code = failed checks; prints "N passed, M failed")
//
//   C1  guestObjectPlausible / readerAnomalous on the values the rolls produced: a zeroed
//       null-page reader (owner 0, cs=4), roll1's owner 0xFFFFFFFF (cs=3), a healthy heap
//       reader + owner, FOURCC-class garbage.
//   C2  StallDetector: live drains never fire; ticks frozen under live callbacks fire START at
//       the 250 ms threshold (not before) and END on the next tick; frozen ticks with frozen
//       callbacks (audio paused) never fire.
//   C3  protectRange: [base, base+64K) becomes unreadable to hostRangeReadable (the removal
//       protocol was honoured: the cached predicate does not serve a stale "readable"), the
//       next page stays readable.
//   C4  SR_NTAVM_XENIA_HEAP allocator (kernel/ntavm_xenia_heap): Xenia placement - v00000000
//       bottom-up from 0x10000 (protect_zero), 64 KB size rounding, v40000000 for large pages,
//       top-down, exact-base release + reuse, owns(), exhaustion returns 0.
//   C5  the root cause on the REAL recompiled EA parser ParseTagStreamAndAlignData_sub_82330CA8,
//       fed the sound block bytes our roll A1 had at 0x88276120 (rbas + 5 records + 4 pad +
//       SNR): at a NEGATIVE base the header stays at base+0x6C (signed `% 16` skips the
//       alignment -> misparsed SNR = D-AUDIO-CMDBUF); at a positive (Xenia v00000000) base it
//       is aligned to base+0x70 = the real SNR header (golden: 036764A0 for base 03676340).
#include "kernel/audio_cmdq_obs.hpp"
#include "kernel/ntavm_xenia_heap.hpp"
#include "guest/ppc/ppc_recomp_shared.h"
#include "kernel/guest_context.hpp"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Q = sr::kernel::cmdq;
namespace X = sr::kernel::xheap;
PPC_EXTERN_FUNC(sub_82330CA8);   // ParseTagStreamAndAlignData
namespace K = sr::kernel;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) {
    ++g_pass;
  } else {
    ++g_fail;
    std::printf("FAIL %s\n", name);
  }
}

void c1() {
  check(!Q::guestObjectPlausible(0), "C1 0 is not an object");
  check(!Q::guestObjectPlausible(0xFFFFFFFFu), "C1 0xFFFFFFFF is not an object");
  check(!Q::guestObjectPlausible(0x464D5265u), "C1 FOURCC 'FMRe' is not an object");
  check(Q::guestObjectPlausible(0x8722DBD8u), "C1 heap 8722DBD8 (the RWAC buffer) is an object");
  check(Q::guestObjectPlausible(0x8299FDC8u), "C1 static 8299FDC8 is an object");
  check(Q::guestObjectPlausible(0xBE0FF000u), "C1 main stack is an object range");
  check(Q::readerAnomalous(0, 0, true), "C1 NULL reader (owner 0 -> cs=4) is anomalous");
  check(Q::readerAnomalous(0, 0, false), "C1 NULL reader unread (S1 armed) is anomalous");
  check(Q::readerAnomalous(0x20, 0xFFFFFFFFu, true), "C1 null-page reader with owner FFFFFFFF (roll1 cs=3)");
  check(Q::readerAnomalous(0x87400000u, 0, true), "C1 heap reader with owner 0 (freed, zeroed)");
  check(Q::readerAnomalous(0x87400000u, 0xFFFFFFFFu, true), "C1 heap reader with owner FFFFFFFF");
  check(Q::readerAnomalous(0xFFFFFFF0u, 0, false), "C1 garbage reader FFFFFFF0");
  check(!Q::readerAnomalous(0x87400000u, 0x87300000u, true), "C1 healthy reader + owner is NOT anomalous");
  check(!Q::readerAnomalous(0x87400000u, 0, false), "C1 healthy reader, owner unread is NOT anomalous");
}

void c2() {
  // 187.5 Hz drains (one per ~5.333 ms), callbacks at the same rate: 2 s of healthy traffic.
  {
    Q::StallDetector d;
    int events = 0;
    uint32_t ticks = 0;
    uint64_t cb = 0;
    for (uint64_t t = 0; t <= 2000; t += 50) {
      ticks += 9; cb += 9;
      if (d.feed(t, ticks, cb) != Q::StallDetector::kNone) ++events;
    }
    check(events == 0, "C2 healthy drains: no stall event");
  }
  // Freeze ticks at t=1000 while callbacks keep coming.
  {
    Q::StallDetector d;
    uint32_t ticks = 0;
    uint64_t cb = 0;
    uint64_t startAt = 0, endAt = 0;
    for (uint64_t t = 0; t <= 3000; t += 50) {
      cb += 9;
      if (t <= 1000 || t >= 2000) ticks += 9;
      const auto ev = d.feed(t, ticks, cb);
      if (ev == Q::StallDetector::kStart && !startAt) startAt = t;
      if (ev == Q::StallDetector::kEnd && !endAt) endAt = t;
    }
    check(startAt == 1250, "C2 frozen ticks under live callbacks: START at 250 ms (t=1250)");
    check(endAt == 2000, "C2 START is followed by END on the next tick (t=2000)");
    check(d.stallStartMs == 1000, "C2 the stall is dated from the last tick change");
  }
  // Ticks AND callbacks frozen (audio paused / host stalled): never a DAC stall.
  {
    Q::StallDetector d;
    int events = 0;
    for (uint64_t t = 0; t <= 2000; t += 50)
      if (d.feed(t, 77, 500) != Q::StallDetector::kNone) ++events;
    check(events == 0, "C2 frozen ticks with frozen callbacks: no event");
  }
  // One stray callback is not enough (needs >= 2 during the freeze).
  {
    Q::StallDetector d;
    int events = 0;
    for (uint64_t t = 0; t <= 2000; t += 50)
      if (d.feed(t, 77, t >= 1000 ? 501 : 500) != Q::StallDetector::kNone) ++events;
    check(events == 0, "C2 a single callback during a freeze does not fire");
  }
}

void c3() {
#ifdef _WIN32
  uint8_t* p = static_cast<uint8_t*>(
      VirtualAlloc(nullptr, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  check(p != nullptr, "C3 test reservation");
  if (!p) return;
  std::memset(p, 0x5A, 0x20000);
  check(K::hostRangeReadable(p, 4) && K::hostRangeReadableCached(p, 4),
        "C3 readable before protect (also primes the cache)");
  check(Q::protectRange(p, 0x10000), "C3 protectRange succeeds");
  check(!K::hostRangeReadable(p, 4), "C3 [base, +4) unreadable after protect");
  check(!K::hostRangeReadable(p + 0xFFFC, 4), "C3 last word of the 64K range unreadable");
  check(!K::hostRangeReadableCached(p, 4), "C3 the cached predicate does not serve a stale readable");
  check(K::hostRangeReadable(p + 0x10000, 4), "C3 the page after the range stays readable");
  check(p[0x10000] == 0x5A, "C3 contents after the range untouched");
  check(!Q::protectRange(nullptr, 0x10000), "C3 null base refused");
  K::hostRangeReadableBeginRemoval();
  VirtualFree(p, 0, MEM_RELEASE);
  K::hostRangeReadableEndRemoval();
#endif
}

void c4() {
  X::resetForTest();
  uint32_t g = 0;
  const uint32_t a = X::alloc(0x1A00000u, false, false, &g);
  check(a == 0x10000u && g == 0x1A00000u, "C4 first v00000000 grant at 0x10000 (above protect_zero)");
  const uint32_t b = X::alloc(0x37D000u, false, false, &g);
  check(b == 0x1A10000u && g == 0x380000u, "C4 next grant bottom-up, size rounded to 64 KB");
  check(static_cast<int32_t>(a) > 0 && static_cast<int32_t>(b) > 0, "C4 grants are positive int32");
  const uint32_t l = X::alloc(0x20000u, true, false, &g);
  check(l == 0x40000000u, "C4 X_MEM_LARGE_PAGES -> v40000000");
  const uint32_t t = X::alloc(0x10000u, false, true, &g);
  check(t == 0x3FFF0000u, "C4 X_MEM_TOP_DOWN -> top of v00000000");
  check(X::owns(a) && X::owns(a + 0x19FFFFFu) && !X::owns(a + 0x1A00000u + 0x380000u),
        "C4 owns() covers live regions only");
  check(X::release(a) == 0x1A00000u, "C4 release(base) returns the region size");
  check(!X::owns(a) && !X::owns(a + 0x19FFFFFu) && X::owns(b),
        "C4 owns() (lock-free page bitmap) drops a released region, keeps its neighbour");
  check(!X::owns(0x7F000000u) && !X::owns(0xFFFFFFFFu), "C4 owns() is false above v40000000");
  check(X::release(a) == 0, "C4 double release is refused");
  check(X::release(b + 0x1000u) == 0, "C4 release of a non-base VA is refused");
  const uint32_t c = X::alloc(0x100000u, false, false, &g);
  check(c == 0x10000u, "C4 a released range is reused first-fit");
  check(X::alloc(0x40000000u, false, false, &g) == 0, "C4 an allocation larger than the heap fails");
  check(X::alloc(0, false, false, &g) == 0, "C4 size 0 fails");
  X::resetForTest();
  check(X::liveRegionsForTest() == 0, "C4 reset");
}

// EA tagged-param FourCC table at 0x82914480 (golden sound_gameplay; the image .rdata),
// copied in because difftest does not load the image; terminated after 0x180 bytes.
const char* kNumcTableHex =
    "6e756d630000000000000001766f6c6d00000001000000646870662000000001"
    "000000006c706620000000010000000066616c6c00000001000000327374726d"
    "00000001000000007370676400000002820228a4706e7370000000010000000a"
    "73676d7800000002820228ac6c67616e00000001000000016d67616e00000001"
    "000000016d66726500000001000000006867616e0000000100000001736d7374"
    "00000002820228d8706c796c00000002820228ec666c737a0000000000000000"
    "636d707200000002820229206267666c00000000000000007661627200000000"
    "0000003c706c626600000000000000007072696f00000000000000007261706f"
    "00000002820229207373697a0000000100000064616c626d00000002820220bf"
    "617274730000000282022116736f6e67000000028202211767656e7200000002"
    "8202212b6e6d746b0000000000000001746b6e6d00000002820228c474696e66"
    "00000003000000007374636800000000000000006e6d63680000000000000001";

// The block our roll A1 held at 0x88276120 (Frida read), 0x80 bytes.
const uint8_t kSndBlock[0x80] = {
  0x72,0x62,0x61,0x73,0x0a,0x00,0x02,0x00,0x63,0x6d,0x75,0x6e,0x00,0x00,0x00,0x00,
  0x04,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x05,0x00,0x00,0x00,0x6d,0x6c,0x6f,0x76,
  0x01,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x64,0x00,0x00,
  0x6f,0x69,0x72,0x70,0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
  0x5a,0x00,0x00,0x00,0x6c,0x6c,0x61,0x66,0x01,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
  0x01,0x00,0x00,0x00,0x00,0xe8,0x03,0x00,0x7a,0x69,0x73,0x73,0x01,0x00,0x00,0x00,
  0x04,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0xfa,0x00,0x00,0x00,0x00,0x00,0x00,
  0x03,0x04,0xac,0x44,0x20,0x02,0xb1,0x10,0x00,0x00,0x00,0x00,0x00,0x01,0x40,0x0c};

void put32(uint32_t va, uint32_t v) {
  uint8_t* p = K::guestVAToHost(va);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
uint32_t get32(uint32_t va) {
  const uint8_t* p = K::guestVAToHost(va);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Run the real parser on a holder whose data block sits at `base`; returns holder[3] (header).
uint32_t parseAt(uint32_t base) {
  constexpr uint32_t kHolder = 0x90D00000u, kTracks = 0x90D00100u;
  std::memset(K::guestVAToHost(kHolder), 0, 0x200);
  std::memcpy(K::guestVAToHost(base), kSndBlock, sizeof(kSndBlock));
  put32(kHolder + 4, base);          // [1] data base
  put32(kHolder + 8, 0x140A0u);      // [2] total size (32-aligned)
  put32(kHolder + 20, kTracks);      // [5] track list
  PPCContext c{};
  c.r1.u32 = 0x90E00000u;
  c.r13.u32 = 0;
  c.r14.u32 = 0x829C0000u;
  c.r3.u32 = kHolder;
  sub_82330CA8(c, K::getGuestBase());
  return get32(kHolder + 12);
}

void c5() {
  K::setGuestMemory(K::getGuestBase(), 0x100000000ull);
  {
    uint8_t* t = K::guestVAToHost(0x82914480u);
    for (size_t i = 0; i < 0x180; ++i) {
      unsigned v = 0;
      std::sscanf(kNumcTableHex + 2 * i, "%2x", &v);
      t[i] = uint8_t(v);
    }
    std::memset(t + 0x180, 0, 12);   // terminator
  }
  const uint32_t neg = 0x88276120u;   // our roll A1 (bump heap, negative int32)
  const uint32_t pos = 0x03676120u;   // Xenia v00000000 (positive), same low nibble layout
  const uint32_t hNeg = parseAt(neg);
  const uint32_t hPos = parseAt(pos);
  check(hNeg == neg + 0x6Cu, "C5 negative base: real EA parser leaves the SNR header UNALIGNED (base+0x6C)");
  check(hPos == pos + 0x70u, "C5 positive base: real EA parser aligns the SNR header (base+0x70)");
  check(get32(pos + 0x70u) == 0x0304AC44u, "C5 base+0x70 is the SNR header (codec 3, stereo, 44100)");
  check(get32(neg + 0x6Cu) == 0 && get32(neg + 0x70u) == 0x0304AC44u,
        "C5 base+0x6C is the 4-byte pad in front of it (what roll A1 parsed: rate 0, mode 0)");
}
}  // namespace

int runCmdqTest() {
  c1();
  c2();
  c3();
  c4();
  c5();
  std::printf("cmdq-test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
