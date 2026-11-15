// NATIVE strand A: stream-resident constant dedup. Contract: native_const_dedup.hpp.
// C1: the CHECK's side (SR_NATIVE_CONST_DEDUP_CHECK: checkOn, the CP's DDCK check cpCheck / cpCheckPackets) and the
// mutation test (SR_NATIVE_CONST_DEDUP_MUTATE) are development oracles, compiled out under SR_ORACLE=0
// (kernel/sr_oracle.hpp): both gates then read as unset. Release retains the core and stream
// epoch tracking; unknown cursor advances invalidate residency without reading packets.

#include <windows.h>
#include <intrin.h>
#include "native_const_dedup.hpp"

#include "guest_context.hpp"
#include "sr_oracle.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <emmintrin.h>

namespace sr::native::cdedup {

namespace {

#if SR_ORACLE
bool envOn(const char* n) {
  const char* e = std::getenv(n);
  return e && e[0] == '1';
}
#else
// SR_ORACLE=0: the CHECK's gate reads as unset, with no env read (g_check below is constant-initialized false).
constexpr bool envOn(const char*) { return false; }
#endif
bool envNotZero(const char* n) {
  const char* e = std::getenv(n);
  return !e || e[0] != '0';
}
// Default ON since iter-467 S2a (records carry only the groups the stream does not hold: s2ab1/2
// in-roll A/B pooled CP -0.32 +-0.16 ms/frame, fps +4.4 +-2.3; CHECK 1.36B groups 0 BAD; mutation
// caught). SR_NATIVE_CONST_DEDUP=0 keeps the A/B; SR_NATIVE_CONST_DEDUP_CHECK=1 decides without
// skipping (it turns the skips off by itself).
const bool g_check = envOn("SR_NATIVE_CONST_DEDUP_CHECK");
const bool g_on = envNotZero("SR_NATIVE_CONST_DEDUP") && !g_check;
#if SR_ORACLE
// Mutation test of the CHECK (diagnostic): the scan ignores other writers of the constant file, so
// the CHECK must report BAD groups.
const bool g_mutate = envOn("SR_NATIVE_CONST_DEDUP_MUTATE");
#endif
// Dedup also inside the XDK record modes (Z-pass / segment table / recording context). Their
// replays run whole segments, which the epoch already makes self-contained; the CHECK covers it.
const bool g_modes = envNotZero("SR_NATIVE_CONST_DEDUP_MODES");  // default ON with the dedup (=0 off)
uint64_t g_modeCalls = 0;

inline uint32_t be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return _byteswap_ulong(v);
}

// ---- core state (render thread) ----
alignas(64) uint8_t g_data[kGroups][64];
uint8_t g_valid[kGroups];

// ---- adapter state (render thread) ----
uint32_t g_dev = 0, g_seg = 0, g_lim = 0, g_gen = 0, g_lastEnd = 0;
bool g_haveEnd = false;
DWORD g_tid = 0;

// ---- counters ----
uint64_t g_why[kWhyN] = {};
uint64_t g_seen = 0, g_skipped = 0, g_scans = 0, g_scanBytes = 0, g_regInval = 0, g_nextReport = 1ull << 20;
std::atomic<uint64_t> g_cpMarked{0}, g_cpOk{0}, g_cpBad{0}, g_cpPkts{0};
uint64_t g_marks = 0;
uint64_t g_tidCached = 0;  // step 6 I15 P5: thread-switch tests answered from the thread_local id (render thread)
// Step 6 I15's combined switch (SR_GUESTMEM_LEAN, default on; =0 every piece off), read once (as guest_context.cpp's).
bool leanOn() {
  static const bool on = [] {
    const char* e = std::getenv("SR_GUESTMEM_LEAN");
    return !(e && e[0] == '0');
  }();
  return on;
}

#if SR_ORACLE
// ---- check mode: the CP side (one pending prediction per CP thread) ----
thread_local bool t_pending = false;
#endif
thread_local bool t_suspended = false;

void report(const char* why) {
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[CONST_DEDUP] %s mode=%s groups=%llu resident=%llu (%.1f%%) scans=%llu scanKB=%llu regInval=%llu "
              "inval epoch=%llu thread=%llu mode=%llu misframe=%llu op=%llu gap=%llu fallback=%llu device=%llu "
              "modeCalls=%llu | check marks=%llu cpPkts=%llu cpMarked=%llu ok=%llu BAD=%llu | lean=%s tidCached=%llu",
              why, g_on ? "on" : g_check ? "check" : "off", (unsigned long long)g_seen,
              (unsigned long long)g_skipped, g_seen ? 100.0 * double(g_skipped) / double(g_seen) : 0.0,
              (unsigned long long)g_scans, (unsigned long long)(g_scanBytes >> 10), (unsigned long long)g_regInval,
              (unsigned long long)g_why[kWhyEpoch], (unsigned long long)g_why[kWhyThread],
              (unsigned long long)g_why[kWhyMode], (unsigned long long)g_why[kWhyMisframe],
              (unsigned long long)g_why[kWhyOp], (unsigned long long)g_why[kWhyGap],
              (unsigned long long)g_why[kWhyFallback], (unsigned long long)g_why[kWhyDevice],
              (unsigned long long)g_modeCalls, (unsigned long long)g_marks, (unsigned long long)g_cpPkts.load(), (unsigned long long)g_cpMarked.load(),
              (unsigned long long)g_cpOk.load(), (unsigned long long)g_cpBad.load(), leanOn() ? "on" : "off",
              (unsigned long long)g_tidCached);
}

#if SR_ORACLE
// Header-only walk of packets other code wrote ([p, p+bytes), big-endian). Any write that can
// reach the ALU constant file invalidates its groups; anything not understood drops them all.
void scanPackets(const uint8_t* p, uint32_t bytes) {
  if (g_mutate) return;
  g_scans++;
  g_scanBytes += bytes;
  uint32_t off = 0;
  while (off + 4u <= bytes) {
    const uint32_t hdr = be32(p + off);
    const uint32_t type = hdr >> 30;
    if (type == 0u) {
      const uint32_t n = ((hdr >> 16) & 0x3FFFu) + 1u;
      const uint32_t reg = hdr & 0x7FFFu;
      const bool one = (hdr >> 15) & 1u;
      if (reg < kAluReg + kAluRegs && reg + (one ? 1u : n) > kAluReg) invalidateRegs(reg, one ? 1u : n);
      off += 4u * (n + 1u);
    } else if (type == 1u) {
      off += 12u;  // two registers below 0x800
    } else if (type == 2u) {
      off += 4u;
    } else {
      const uint32_t n = ((hdr >> 16) & 0x3FFFu) + 1u;
      const uint32_t op = (hdr >> 8) & 0x7Fu;
      if (off + 4u * (n + 1u) > bytes) break;
      const uint8_t* pl = p + off + 4u;
      switch (op) {
        case 0x2Du: {  // SET_CONSTANT: offset_type, values
          const uint32_t ot = be32(pl);
          if (((ot >> 16) & 0xFFu) == 0u) invalidateRegs(kAluReg + (ot & 0x7FFu), n - 1u);
          break;
        }
        case 0x2Fu: {  // LOAD_ALU_CONSTANT: address, offset_type, size (dwords)
          if (n < 3u) { invalidateAll(kWhyOp); break; }
          const uint32_t ot = be32(pl + 4), sz = be32(pl + 8) & 0xFFFu;
          if (((ot >> 16) & 0xFFu) == 0u) invalidateRegs(kAluReg + (ot & 0x7FFu), sz);
          break;
        }
        case 0x10u:  // NOP - but a NATIVE 1a marker hides a bracket, and an S1 draw-flush marker
                     // ('NFLU', kernel/native_flush) a host record whose constant runs the scan cannot see
          if (n >= 2u && (be32(pl) == 0x4E445257u || be32(pl) == 0x4E464C55u)) invalidateAll(kWhyOp);
          break;
        // No ALU constant effect (Xenia command_processor): draws, shader loads, bins, events,
        // waits, memory writes, interrupts, swap, viz query, invalidate-state, reg r-m-w (< 0x2000).
        case 0x21u: case 0x22u: case 0x23u: case 0x27u: case 0x2Bu: case 0x36u: case 0x3Bu:
        case 0x3Cu: case 0x3Du: case 0x3Eu: case 0x45u: case 0x46u: case 0x54u: case 0x58u:
        case 0x59u: case 0x60u: case 0x61u: case 0x62u: case 0x63u: case 0x64u:
          break;
        default:
          invalidateAll(kWhyOp);
          break;
      }
      off += 4u * (n + 1u);
    }
  }
  if (off != bytes) invalidateAll(kWhyMisframe);
}

#endif  // Oracle packet scan

}  // namespace

bool on() { return g_on; }
bool checkOn() { return g_check; }

void invalidateAll(Why w) {
  std::memset(g_valid, 0, sizeof(g_valid));
  g_why[w]++;
}

void invalidateRegs(uint32_t reg, uint32_t count) {
  if (!count) return;
  const uint64_t lo = reg, hi = uint64_t(reg) + count;  // [lo, hi)
  if (hi <= kAluReg || lo >= kAluReg + kAluRegs) return;
  const uint32_t g0 = uint32_t((lo < kAluReg ? kAluReg : lo) - kAluReg) / kGroupRegs;
  const uint32_t g1 = uint32_t((hi > kAluReg + kAluRegs ? kAluReg + kAluRegs : hi) - kAluReg + kGroupRegs - 1u) / kGroupRegs;
  for (uint32_t g = g0; g < g1 && g < kGroups; g++) g_valid[g] = 0;
  g_regInval++;
}

uint32_t validGroups() {
  uint32_t n = 0;
  for (uint32_t g = 0; g < kGroups; g++) n += g_valid[g] ? 1u : 0u;
  return n;
}

bool resident(uint32_t group, const uint8_t* be64) {
  if (group >= kGroups || !g_valid[group]) return false;
  const __m128i* a = reinterpret_cast<const __m128i*>(g_data[group]);
  const __m128i e = _mm_and_si128(
      _mm_and_si128(_mm_cmpeq_epi8(_mm_load_si128(a + 0), _mm_loadu_si128(reinterpret_cast<const __m128i*>(be64 + 0))),
                    _mm_cmpeq_epi8(_mm_load_si128(a + 1), _mm_loadu_si128(reinterpret_cast<const __m128i*>(be64 + 16)))),
      _mm_and_si128(_mm_cmpeq_epi8(_mm_load_si128(a + 2), _mm_loadu_si128(reinterpret_cast<const __m128i*>(be64 + 32))),
                    _mm_cmpeq_epi8(_mm_load_si128(a + 3), _mm_loadu_si128(reinterpret_cast<const __m128i*>(be64 + 48)))));
  return _mm_movemask_epi8(e) == 0xFFFF;
}

void store(uint32_t group, const uint8_t* be64) {
  if (group >= kGroups) return;
  __m128i* a = reinterpret_cast<__m128i*>(g_data[group]);
  for (int i = 0; i < 4; i++) _mm_store_si128(a + i, _mm_loadu_si128(reinterpret_cast<const __m128i*>(be64 + 16 * i)));
  g_valid[group] = 1;
}

bool syncStream(uint8_t* base, uint32_t dev, const uint8_t* devH, uint32_t cur) {
  if (!active()) return false;
  // Step 6 I15 P5 (J:/sr_lanec_drawthread/FREEZE_I15_20261024.txt; SR_GUESTMEM_LEAN, default on, =0 the call per draw):
  // the thread-switch test's id from a thread_local initialised once per thread - the same value GetCurrentThreadId()
  // returns on that thread, without the call per draw (89 % of the draw thread's GetCurrentThreadId, ~0.05 ms/present).
  static thread_local const DWORD t_tid = GetCurrentThreadId();
  const DWORD tid = leanOn() ? (g_tidCached++, t_tid) : GetCurrentThreadId();
  if (dev != g_dev || tid != g_tid) {
    invalidateAll(dev != g_dev ? kWhyDevice : kWhyThread);
    g_dev = dev;
    g_tid = tid;
    g_haveEnd = false;
  }
  // XDK record modes (GpuCommandSegmentFlush 820FD660: byte dev+10808 bit 0x80 = segment table,
  // dword dev+12944 = recording context; 820FC000: byte dev+10808 bit 0x02 = Z-pass): ranges get
  // re-executed. No dedup inside them unless SR_NATIVE_CONST_DEDUP_MODES=1. BeginZPass sets 0x40.
  if ((devH[10808] & 0xC2u) || be32(devH + 12944)) {
    g_modeCalls++;
    if (!g_modes) {
      invalidateAll(kWhyMode);
      g_haveEnd = false;
      return false;
    }
  }
  // Segment epoch: start (dev+13508, moves on every kick), limit (dev+0x2C), ring wrap
  // generation (dev+13500). A change = a new IB range: nothing before it may be relied on.
  const uint32_t seg = be32(devH + 13508), lim = be32(devH + 44), gen = be32(devH + 13500);
  if (seg != g_seg || lim != g_lim || gen != g_gen) {
    invalidateAll(kWhyEpoch);
    g_seg = seg;
    g_lim = lim;
    g_gen = gen;
    g_haveEnd = false;
  }
  if (!g_haveEnd) return true;  // nothing valid; the emission re-anchors the position
  if (cur < g_lastEnd || cur - g_lastEnd > (256u << 10)) {
    invalidateAll(kWhyGap);
    g_haveEnd = false;
    return true;
  }
  if (cur > g_lastEnd) {
#if SR_ORACLE
    const uint32_t va = g_lastEnd + 4u, len = cur - g_lastEnd;
    const uint32_t a0 = sr::kernel::aliasGuestVA(va), a1 = sr::kernel::aliasGuestVA(va + len - 1u);
    if (a1 < a0 || a1 - a0 != len - 1u) {
      invalidateAll(kWhyGap);
    } else {
      scanPackets(base + a0, len);
    }
#else
    // Native records leave the guest command bytes untouched. A cursor advance
    // by another producer is an unknown constant write, never packet input.
    // Forget resident groups until the next native emission establishes them.
    invalidateAll(kWhyGap);
#endif
    g_lastEnd = cur;
  }
  return true;
}

void noteEmitted(uint32_t end) {
  g_lastEnd = end;
  g_haveEnd = true;
}

void lostStream() {
  invalidateAll(kWhyFallback);
  g_haveEnd = false;
}

void advanceTo(uint32_t dev, uint32_t end) {
  if (!active() || !g_haveEnd) return;
  uint8_t* base = nullptr;
  size_t size = 0;
  sr::kernel::getGlobalGuestMemory(&base, &size);
  const uint8_t* devH = sr::kernel::guestVAToHost(dev);
  const uint8_t* devE = sr::kernel::guestVAToHost(dev + 13511u);
  if (!base || !devH || devE != devH + 13511) {
    lostStream();
    return;
  }
  if (syncStream(base, dev, devH, end)) noteEmitted(end);
}

#if SR_ORACLE
void notePredictedRun(uint32_t groups) { g_marks += groups; }

void cpNotePrediction() { t_pending = true; }
bool cpTakePrediction() {
  const bool p = t_pending;
  t_pending = false;
  return p;
}

void cpCheck(const uint8_t* data, uint32_t reg, uint32_t count, const uint32_t* regFile) {
  g_cpPkts.fetch_add(1, std::memory_order_relaxed);
  if (reg < kAluReg || ((reg - kAluReg) & 15u)) {
    g_cpBad.fetch_add(1, std::memory_order_relaxed);  // a predicted run must be a group-aligned ALU run
    return;
  }
  for (uint32_t j = 0; j + 16u <= count && reg + j + 16u <= kAluReg + kAluRegs; j += 16u) {
    g_cpMarked.fetch_add(1, std::memory_order_relaxed);
    uint32_t diff = 0;
    for (uint32_t i = 0; i < 16u; i++)
      if (regFile[reg + j + i] != be32(data + 4u * (j + i))) diff |= 1u << i;
    if (!diff) {
      g_cpOk.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const uint64_t bad = g_cpBad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad <= 24) {
      const uint32_t i = uint32_t(__builtin_ctz(diff));
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[CONST_DEDUP] CHECK BAD #%llu reg=%04X group=%u diffMask=%04X first@%u: CP %08X packet %08X",
                  (unsigned long long)bad, reg + j, (reg + j - kAluReg) / 16u, diff, i, regFile[reg + j + i],
                  be32(data + 4u * (j + i)));
    }
  }
}

void cpCheckPackets(const uint8_t* host, uint32_t bytes, const uint32_t* regFile) {
  uint32_t off = 0;
  bool pending = false;
  while (off + 4u <= bytes) {
    const uint32_t hdr = be32(host + off);
    const uint32_t type = hdr >> 30;
    if (type == 0u) {
      const uint32_t n = ((hdr >> 16) & 0x3FFFu) + 1u, reg = hdr & 0x7FFFu;
      if (pending && off + 4u * (n + 1u) <= bytes) cpCheck(host + off + 4u, reg, n, regFile);
      pending = false;
      off += 4u * (n + 1u);
    } else if (type == 1u) {
      off += 12u;
    } else if (type == 2u) {
      off += 4u;
    } else {
      const uint32_t n = ((hdr >> 16) & 0x3FFFu) + 1u;
      if (((hdr >> 8) & 0x7Fu) == 0x10u && off + 8u <= bytes && be32(host + off + 4u) == kCheckMagic) pending = true;
      off += 4u * (n + 1u);
    }
  }
  t_pending = false;  // the bracket's markers were consumed here, not by the type-0 path
}
#endif  // SR_ORACLE

void suspend(bool s) { t_suspended = s; }
bool suspended() { return t_suspended; }

void countGroups(uint32_t seen, uint32_t skipped) {
  g_seen += seen;
  g_skipped += skipped;
  if (g_seen >= g_nextReport) {
    report("periodic");
    g_nextReport = g_seen < (1ull << 26) ? g_seen * 2 : g_seen + (1ull << 26);
  }
}

}  // namespace sr::native::cdedup
