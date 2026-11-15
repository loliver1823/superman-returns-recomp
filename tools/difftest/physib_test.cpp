// iter-458 (Astra 029 A): indirect buffers are read through the window that OWNS
// them. Driven through the REAL entry, sr::gfx::executeGuestIndirectBuffer, with
// DIFFERENT packets in the two windows, so the engine's own side effects name the
// window it consumed:
//   * the EVENT_WRITE_SHD fence value (the engine writes it through its own
//     physToHost, the 0xA window - SR_PM4_PHYS_A default), and
//   * the PM4_INTERRUPT count (only the 0xA copy carries a 0x54 packet).
//
// The gate is latched once per process, so run BOTH:
//   SR_PHYS_FREE=1 build/difftest.exe --physib-test   ownership decides
//   SR_PHYS_FREE=0 build/difftest.exe --physib-test   legacy 0x8 read everywhere
//   SR_PHYS_RESOLVE=1 additionally (B, Astra 029): the 2x2 of the two gates.
// (exit code = failed checks)
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/guest_context.hpp"
#include "kernel/phys_alloc.hpp"
#include "kernel/phys_window.hpp"
#include "gfx/null_renderer.hpp"
#include "gfx/pm4_engine.hpp"
#include "kernel/gpu_submit.hpp"
#include "kernel/gpu_command_processor.hpp"  // Oracle-only ring adapter
#include "kernel/gpu_hang_wd.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>

namespace sr {
namespace gfx {
void executeGuestIndirectBuffer(uint32_t ibPhys, uint32_t ibSizeDwords);
}
namespace kernel {
int pickAliasWindowForTest(uint32_t physBase, uint64_t span, int resolveSite);   // hle_api_observe.cpp
}
}  // namespace sr

namespace A = sr::kernel::physalloc;
namespace W = sr::kernel::physwin;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

constexpr uint32_t kMask = 0x1FFFFFFFu;
constexpr uint32_t kValA = 0x000A000Au;   // fence value carried by the 0xA copy
constexpr uint32_t kVal8 = 0x00080008u;   // fence value carried by the 0x8 copy

void putBE(uint32_t va, uint32_t v) {
  uint8_t* p = sr::kernel::guestVAToHost(va);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
uint32_t getBE(uint32_t va) {
  const uint8_t* p = sr::kernel::guestVAToHost(va);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Six dwords: EVENT_WRITE_SHD {initiator, fence phys, value}, then either a
// PM4_INTERRUPT or two zero dwords (skipped by the parser).
void writeIb(uint32_t va, uint32_t fencePhys, uint32_t value, bool interrupt) {
  putBE(va + 0, 0xC0025800u);
  putBE(va + 4, 0);
  putBE(va + 8, fencePhys);
  putBE(va + 12, value);
  putBE(va + 16, interrupt ? 0xC0005400u : 0u);
  putBE(va + 20, interrupt ? 0x80000000u : 0u);
}
constexpr uint32_t kIbDwords = 6;

struct Outcome {
  uint32_t fence;       // value at 0xA|fencePhys after the call
  uint32_t fence8;      // value at the fence's 0x8 alias (must never move)
  uint64_t interrupts;  // PM4_INTERRUPT delta
  sr::gfx::IbWindowCounts d;
};

Outcome run(uint32_t ibPhys, uint32_t dwords, uint32_t fencePhys) {
  putBE(0xA0000000u | fencePhys, 0x0BADF00Du);
  putBE(0x80000000u | fencePhys, 0x8BADF00Du);
  const sr::gfx::IbWindowCounts c0 = sr::gfx::getLiveIbWindowCounts();
  const uint64_t i0 = sr::gfx::getLiveCpInterruptCount();
  sr::gfx::executeGuestIndirectBuffer(ibPhys, dwords);
  const sr::gfx::IbWindowCounts c1 = sr::gfx::getLiveIbWindowCounts();
  return Outcome{getBE(0xA0000000u | fencePhys), getBE(0x80000000u | fencePhys),
                 sr::gfx::getLiveCpInterruptCount() - i0,
                 sr::gfx::IbWindowCounts{c1.a - c0.a, c1.w8 - c0.w8, c1.both - c0.both,
                                         c1.unknown - c0.unknown}};
}

// Distinguishable copies of one IB in both windows.
void seed(uint32_t ibPhys, uint32_t fencePhys) {
  writeIb(0xA0000000u | ibPhys, fencePhys, kValA, true);
  writeIb(0x80000000u | ibPhys, fencePhys, kVal8, false);
}

bool consumedA(const Outcome& o) { return o.fence == kValA && o.interrupts == 1 && o.fence8 == 0x8BADF00Du; }
bool consumed8(const Outcome& o) { return o.fence == kVal8 && o.interrupts == 0 && o.fence8 == 0x8BADF00Du; }
bool counted(const sr::gfx::IbWindowCounts& d, uint64_t a, uint64_t w8, uint64_t both, uint64_t unk) {
  return d.a == a && d.w8 == w8 && d.both == both && d.unknown == unk;
}
}  // namespace

int runPhysIbTest() {
  const bool gate = A::physFreeEnabled();
  std::printf("[PHYSIB_TEST] ownership-resolved IB reads, SR_PHYS_FREE=%d\n", gate ? 1 : 0);
  {
    // The extracted Oracle decoder keeps packet boundaries and wrapping
    // byte-for-byte; native submission never links this implementation.
    constexpr uint32_t ring = 0x20000u;
    sr::kernel::OracleRingPacket packet;
    for (const auto& test : {std::pair{0u, 1u}, std::pair{0x3FFF0000u, 16385u},
                             std::pair{0x40000000u, 3u}, std::pair{0x80000000u, 1u},
                             std::pair{0xC0001000u, 2u}, std::pair{0xC0013F00u, 3u}}) {
      putBE(ring, test.first);
      sr::kernel::gpuCpPeekOraclePacket(ring, 0, packet);
      check(packet.header == test.first && packet.dwords == test.second, "R1 Oracle packet header/length preserved");
    }
    check(sr::kernel::gpuCpOraclePacketIndirect(packet), "R2 unpredicated INDIRECT_BUFFER goes through the Oracle IB entry");
    packet.header |= 1u;
    check(!sr::kernel::gpuCpOraclePacketIndirect(packet), "R3 predicated INDIRECT_BUFFER stays in the packet engine");
    putBE(ring + 12u, 0xC0001000u);  // NOP header at the last ring dword
    putBE(ring, 0x12345678u);        // payload wraps to the first dword
    sr::kernel::gpuCpPeekOraclePacket(ring, 3, packet);
    sr::gfx::Pm4Engine engine;
    sr::kernel::gpuCpPrepareOraclePacket(packet, ring, 3, 3);
    sr::kernel::gpuCpExecuteOraclePacket(engine, packet, 3);
    const uint8_t expected[] = {0xC0, 0, 0x10, 0, 0x12, 0x34, 0x56, 0x78};
    check(packet.bytes.size() == sizeof(expected) && std::memcmp(packet.bytes.data(), expected, sizeof(expected)) == 0,
          "R4 Oracle packet copy wraps at the ring mask and preserves byte order");
  }
  A::resetForTest(A::kLo, A::kHi);
  W::resetForTest();
  // The I-cases below are the iter-458 LEGACY IB rule (BOTH / unknown -> 0x8);
  // iter-463 F3 (SR_IB_WINDOW_UNIFIED, default ON) is the U-cases at the end.
  W::setIbWindowUnifiedForTest(0);

  const uint32_t F = A::alloc(0x1000, 0x1000);
  check(F != 0, "S0 fence page allocated");
  const uint32_t fp = F & kMask;

  // ── ownerWindow units ───────────────────────────────────────────────────────
  const uint32_t L = A::alloc(0x1000, 0x1000);
  const uint32_t lp = L & kMask;
  const int ownLive = gate ? W::kWinA : W::kWinUnknown;
  check(W::ownerWindow(lp, 0x1000) == ownLive, "W1 a whole live block -> 0xA (gate off: unknown)");
  check(W::ownerWindow(L, 0x1000) == ownLive, "W2 window bits on the input are masked");
  check(W::ownerWindow(lp + 0xFFCu, 4) == ownLive, "W3 last dword of the block (exact end)");
  check(W::ownerWindow(lp + 0xFFCu, 8) == W::kWinUnknown, "W4 one dword past the block end -> unknown");
  check(W::ownerWindow(lp, 0) == W::kWinUnknown, "W5 zero bytes -> unknown");
  check(W::ownerWindow(lp, 0x20000001ull) == W::kWinUnknown, "W6 more than 512 MB -> unknown");
  check(W::ownerWindow(lp, 0x100000000ull + 0x10) == W::kWinUnknown,
        "W7 a 64-bit length that would truncate to 16 bytes -> unknown");
  // A span that wraps the top of physical memory is not a contiguous physical
  // range, even where the VA arithmetic would make it look like one.
  W::noteHeapRoutedGrant(0x9FFFF000u, 0x2000u);
  check(W::ownerWindow(0x1FFFF000u, 0x2000u) == W::kWinUnknown,
        "W8 a span past physical 512 MB -> unknown (no wrap into the next window)");
  check(W::ownerWindow(0x1FFFF000u, 0x1000u) == W::kWin8, "W9 the same grant, in range -> 0x8");

  // ── the real entry ──────────────────────────────────────────────────────────
  // I1: an arena-resident IB (what 027's routing produces).
  {
    const uint32_t ib = A::alloc(0x1000, 0x1000);
    const uint32_t p = ib & kMask;
    seed(p, fp);
    const Outcome o = run(p, kIbDwords, fp);
    if (gate) {
      check(consumedA(o), "I1 arena-owned IB: the 0xA copy executed (fence=A value, 1 interrupt)");
      check(counted(o.d, 1, 0, 0, 0), "I1c counted as A");
    } else {
      check(consumed8(o), "I1 gate off: legacy 0x8 read (fence=0x8 value, no interrupt)");
      check(counted(o.d, 0, 0, 0, 0), "I1c gate off: nothing counted");
    }
  }
  // I2: a heap-routed grant (0x8-owned) keeps the 0x8 read.
  {
    const uint32_t ib = A::alloc(0x1000, 0x1000);
    const uint32_t p = ib & kMask;
    A::free(ib);   // the 0xA side is no longer live
    W::noteHeapRoutedGrant(0x80000000u | p, 0x1000);
    seed(p, fp);
    const Outcome o = run(p, kIbDwords, fp);
    check(consumed8(o), "I2 heap-grant IB: the 0x8 copy executed");
    check(counted(o.d, 0, gate ? 1 : 0, 0, 0), "I2c counted as 0x8 (gate off: nothing)");
  }
  // I3: BOTH windows own the range: explicit ambiguity, legacy selection, counted.
  {
    const uint32_t ib = A::alloc(0x1000, 0x1000);
    const uint32_t p = ib & kMask;
    W::noteHeapRoutedGrant(0x80000000u | p, 0x1000);
    check(W::ownerWindow(p, 24) == (gate ? W::kWinBoth : W::kWin8), "I3w ownerWindow = BOTH");
    seed(p, fp);
    const Outcome o = run(p, kIbDwords, fp);
    check(consumed8(o), "I3 both owned: legacy 0x8 copy executed (0xA never silently preferred)");
    check(counted(o.d, 0, 0, gate ? 1 : 0, 0), "I3c counted as BOTH (gate off: nothing)");
  }
  // I4: a FREED arena block is unknown -> legacy.
  {
    const uint32_t ib = A::alloc(0x1000, 0x1000);
    const uint32_t p = ib & kMask;
    A::free(ib);
    seed(p, fp);
    const Outcome o = run(p, kIbDwords, fp);
    check(consumed8(o), "I4 freed block: legacy 0x8 copy executed");
    check(counted(o.d, 0, 0, 0, gate ? 1 : 0), "I4c counted as unknown (gate off: nothing)");
  }
  // I5: a span crossing from one live block into the ADJACENT live block.
  {
    const uint32_t b1 = A::alloc(0x1000, 0x1000);
    const uint32_t b2 = A::alloc(0x1000, 0x1000);
    check(b2 + 0x1000u == b1, "I5p precondition: top-down, the blocks are adjacent");
    const uint32_t p = (b1 & kMask) - 8u;   // 8 bytes in b2, 16 in b1
    seed(p, fp);
    const Outcome o = run(p, kIbDwords, fp);
    check(consumed8(o), "I5 span across two live blocks: unknown -> legacy 0x8");
    check(counted(o.d, 0, 0, 0, gate ? 1 : 0), "I5c counted as unknown");
  }
  // I6: a span running past a block into never-allocated space.
  {
    const uint32_t lowBlk = A::mmAllocate(0x1000, A::kXPageReadWrite, 0x00200000u, 0x00200FFFu, 0);
    check(lowBlk == 0xA0200000u, "I6p precondition: a low block with free space above");
    const uint32_t p = 0x00200FF8u;
    seed(p, fp);
    const Outcome o = run(p, kIbDwords, fp);
    check(consumed8(o), "I6 span past the block end: unknown -> legacy 0x8");
  }
  // I7: an IB ending EXACTLY at its block end is owned.
  {
    const uint32_t ib = A::alloc(0x1000, 0x1000);
    const uint32_t p = (ib & kMask) + 0x1000u - kIbDwords * 4u;
    seed(p, fp);
    const Outcome o = run(p, kIbDwords, fp);
    check(gate ? consumedA(o) : consumed8(o), "I7 exact-fit at the block end: owned (gate off: legacy)");
  }
  // ── U (iter-463 F3): SR_IB_WINDOW_UNIFIED - contested and unowned IBs take
  // physToHost's default (0xA), the window the IB's own fences resolve to.
  W::setIbWindowUnifiedForTest(1);
  // GPU_HANG_WD wiring (review: the latches were only tested in isolation): the
  // real rptr write-back entry feeds the fence latch, and the IB ring / fence
  // write latches are fed by the real executeGuestIndirectBuffer + engine.
  const bool wd = sr::kernel::gpuHangWdEnabled();
  if (wd) {
    sr::kernel::gpuCpSetRptrWriteback(fp + 0x3Cu, 7);   // VdEnableRingBufferRPtrWriteBack's entry
    check(sr::kernel::gpuHangWdIsFence(0xA0000000u | fp) && !sr::kernel::gpuHangWdIsFence(fp + 4u),
          "U0 gpuCpSetRptrWriteback feeds the watchdog's fence block (rptrWb - 0x3C)");
  }
  {
    const uint32_t ib = A::alloc(0x1000, 0x1000);
    const uint32_t p = ib & kMask;
    if (!gate) W::notePhysGrant(0xA0000000u | p, 0x1000);   // the grant model's 0xA owner
    W::noteHeapRoutedGrant(0x80000000u | p, 0x1000);
    seed(p, fp);
    const uint64_t k0 = W::ibContestedCount();
    const Outcome o = run(p, kIbDwords, fp);
    check(consumedA(o), "U1 unified: a contested IB executes the 0xA copy (fence=A value, 1 interrupt)");
    check(W::ibContestedCount() == k0 + 1, "U1c counted as contested ([PM4_IB_CONTESTED])");
    if (wd) {
      sr::kernel::GpuHangIbForTest r{};
      check(sr::kernel::gpuHangWdLastIbForTest(&r) && r.phys == p && r.dwords == kIbDwords &&
                r.win == W::resolveIbWindow(p, kIbDwords * 4u) && r.win == 0xA0000000u &&
                r.fenceHit == 1 && r.fenceVal == kValA,
            "U1w the IB ring records the window the CP parsed (0xA) and its fence write (A value)");
      // The fence-write latch sits in Pm4Engine::cpPacket - the async CP's
      // packet path (the watchdog only runs with the CP; the non-CP switch is
      // the replay's). The same IB again with the live engine in CP mode (no
      // CP callbacks here, so no guest ISR runs).
      sr::gfx::Pm4Engine& eng = sr::gfx::getLivePm4();
      sr::kernel::GpuHangFenceWriteForTest fw0{}, fw{};
      sr::kernel::gpuHangWdLastFenceWriteForTest(&fw0);
      const bool cpm = eng.cpMode;
      eng.cpMode = true;
      const Outcome oc = run(p, kIbDwords, fp);
      eng.cpMode = cpm;
      // (CP mode stores with the packet's endian swap - 0 here, so the 0xA
      // word reads byte-reversed; the latch records the packet's value.)
      (void)oc;
      check(sr::kernel::gpuHangWdLastFenceWriteForTest(&fw) &&
                fw.count == fw0.count + 1 && fw.op == 0x58u && fw.win == 0xAu && fw.value == kValA &&
                (fw.addr & kMask) == fp,
            "U1f the CP's EVENT_WRITE_SHD feeds the fence-write latch (op 58, value, window 0xA)");
    }
  }
  {
    const uint32_t ib = A::alloc(0x1000, 0x1000);
    const uint32_t p = ib & kMask;
    A::free(ib);
    seed(p, fp);
    const uint64_t u0 = W::ibUnownedCount();
    const Outcome o = run(p, kIbDwords, fp);
    check(consumedA(o), "U2 unified: an unowned IB executes the 0xA copy");
    check(W::ibUnownedCount() == u0 + 1, "U2c counted as unowned ([PM4_IB_UNOWNED])");
  }
  W::setIbWindowUnifiedForTest(-1);

  // ── B: SR_PHYS_RESOLVE at the texture selector (pickAliasWindow) ────────────
  // The resolve gate is latched too; run the 2x2 of SR_PHYS_FREE x SR_PHYS_RESOLVE.
  const char* re = std::getenv("SR_PHYS_RESOLVE");
  const bool asked = re && re[0] == '1';
  const bool res = gate && asked;
  std::printf("[PHYSIB_TEST] B: SR_PHYS_RESOLVE=%d -> active=%d\n", asked ? 1 : 0, res ? 1 : 0);
  check(W::physResolveEnabled() == res, "G1 resolve active only with BOTH gates (inert without SR_PHYS_FREE)");
  using RC = W::ResolveCounts;
  auto rc = [](W::ResolveSite s) { return W::resolveCountsForTest(s); };
  auto same = [](const RC& a, const RC& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; };
  auto fillW = [](uint32_t va, uint8_t v, uint32_t n) {
    std::memset(sr::kernel::guestVAToHost(va), v, n);
  };
  constexpr uint32_t kSpan = 0x1000u;
  {
    // T1: an arena-live texture; BOTH windows populated, different bytes.
    const uint32_t t = A::alloc(kSpan, 0x1000);
    const uint32_t p = t & kMask;
    fillW(0xA0000000u | p, 0x5A, kSpan);
    fillW(0x80000000u | p, 0x18, kSpan);
    const RC c0 = rc(W::kRsTex);
    const int w = sr::kernel::pickAliasWindowForTest(p, kSpan, -1);
    const RC c1 = rc(W::kRsTex);
    check(w == (res ? 0xA : 8), "T1 arena-owned texture: 0xA when resolving, else the legacy 0x8");
    check(res ? (c1.a == c0.a + 1 && c1.validOk == c0.validOk + 1 && c1.validBad == c0.validBad)
              : same(c0, c1),
          "T1c counted resolvedA + validation ok (inactive: nothing)");
    check(res ? (c1.disagree == c0.disagree + 1 && c1.dSelOk == c0.dSelOk + 1 &&
                 c1.dLegOk == c0.dLegOk + 1 && c1.agree == c0.agree)
              : true,
          "T1d vs legacy: DISAGREE (0xA vs 0x8), both sides plausible by content");
    // T5: the caller-resolved sentinel makes no second decision.
    const int w5 = sr::kernel::pickAliasWindowForTest(p, kSpan, W::kRsSites);
    check(w5 == 8 && same(c1, rc(W::kRsTex)), "T5 kRsSites: legacy decision, nothing counted");
  }
  {
    // T2: a heap grant whose 0x8 bytes are BLANK while the 0xA alias is
    // populated: ownership says 0x8, the content check must flag it.
    const uint32_t t = A::alloc(kSpan, 0x1000);
    const uint32_t p = t & kMask;
    A::free(t);
    W::noteHeapRoutedGrant(0x80000000u | p, kSpan);
    fillW(0x80000000u | p, 0x00, kSpan);
    fillW(0xA0000000u | p, 0x77, kSpan);
    const RC c0 = rc(W::kRsTex);
    const int w = sr::kernel::pickAliasWindowForTest(p, kSpan, -1);
    const RC c1 = rc(W::kRsTex);
    check(w == (res ? 8 : 0xA), "T2 heap-grant texture: 0x8 when resolving, else the legacy 0xA rescue");
    check(res ? (c1.w8 == c0.w8 + 1 && c1.validBad == c0.validBad + 1 && c1.validOk == c0.validOk)
              : same(c0, c1),
          "T2c counted resolved8 + validation BAD (blank selected, other populated)");
    check(res ? (c1.disagree == c0.disagree + 1 && c1.dSelBad == c0.dSelBad + 1 &&
                 c1.dLegOk == c0.dLegOk + 1)
              : true,
          "T2d vs legacy: DISAGREE, selected BAD / legacy ok (the same predicate on both)");
  }
  {
    // T3: both windows own the range -> legacy (0x8 populated -> 0x8), counted.
    const uint32_t t = A::alloc(kSpan, 0x1000);
    const uint32_t p = t & kMask;
    W::noteHeapRoutedGrant(0x80000000u | p, kSpan);
    fillW(0x80000000u | p, 0x21, kSpan);
    fillW(0xA0000000u | p, 0x43, kSpan);
    const RC c0 = rc(W::kRsTex);
    const int w = sr::kernel::pickAliasWindowForTest(p, kSpan, -1);
    const RC c1 = rc(W::kRsTex);
    check(w == 8, "T3 both owned: the legacy choice (0x8)");
    check(res ? (c1.both == c0.both + 1 && c1.validOk == c0.validOk && c1.validBad == c0.validBad)
              : same(c0, c1),
          "T3c counted BOTH, no validation (no ownership decision)");
    check(c1.agree == c0.agree && c1.disagree == c0.disagree, "T3d no decision -> no comparison");
  }
  {
    // T4: unknown (freed, no grant): legacy - 0x8 blank, 0xA populated -> 0xA.
    const uint32_t t = A::alloc(kSpan, 0x1000);
    const uint32_t p = t & kMask;
    A::free(t);
    fillW(0x80000000u | p, 0x00, kSpan);
    fillW(0xA0000000u | p, 0x65, kSpan);
    const RC c0 = rc(W::kRsTex);
    const int w = sr::kernel::pickAliasWindowForTest(p, kSpan, -1);
    const RC c1 = rc(W::kRsTex);
    check(w == 0xA, "T4 unknown: the legacy rescue (0xA)");
    check(res ? (c1.unknown == c0.unknown + 1) : same(c0, c1), "T4c counted unknown");
  }
  {
    // T6 (Astra 030): the OWNED window is unreadable (its page decommitted).
    // Resolving, that is a counted FAILED read - never the legacy guess.
    const uint32_t t = A::alloc(kSpan, 0x1000);
    const uint32_t p = t & kMask;
    fillW(0x80000000u | p, 0x3C, kSpan);
    uint8_t* hA = sr::kernel::guestVAToHost(0xA0000000u | p);
    sr::kernel::hostRangeReadableBeginRemoval();   // H-VQ protocol: before AND after
    const bool decommitted = VirtualFree(hA, kSpan, MEM_DECOMMIT) != 0;
    sr::kernel::hostRangeReadableEndRemoval();
    check(decommitted && !sr::kernel::hostRangeReadable(hA, kSpan),
          "T6p precondition: the 0xA page is unreadable");
    const RC c0 = rc(W::kRsTex);
    const int w = sr::kernel::pickAliasWindowForTest(p, kSpan, -1);
    const RC c1 = rc(W::kRsTex);
    check(w == (res ? 0 : 8), "T6 unreadable owned window: FAILED read when resolving (legacy: 0x8)");
    check(res ? (c1.a == c0.a + 1 && c1.validBad == c0.validBad + 1 &&
                 c1.disagree == c0.disagree + 1 && c1.dSelBad == c0.dSelBad + 1 &&
                 c1.dLegOk == c0.dLegOk + 1)
              : same(c0, c1),
          "T6c counted resolvedA + validation BAD + disagreement (legacy 0x8 plausible)");
    sr::kernel::hostRangeReadableBeginRemoval();
    VirtualAlloc(hA, kSpan, MEM_COMMIT, PAGE_READWRITE);
    sr::kernel::hostRangeReadableEndRemoval();
    check(sr::kernel::hostRangeReadable(hA, kSpan), "T6r page recommitted");
  }
  {
    // T7: resolve and legacy agree (0x8 blank, 0xA populated, arena-owned).
    const uint32_t t = A::alloc(kSpan, 0x1000);
    const uint32_t p = t & kMask;
    fillW(0x80000000u | p, 0x00, kSpan);
    fillW(0xA0000000u | p, 0x29, kSpan);
    const RC c0 = rc(W::kRsTex);
    const int w = sr::kernel::pickAliasWindowForTest(p, kSpan, -1);
    const RC c1 = rc(W::kRsTex);
    check(w == 0xA, "T7 arena texture written through 0xA: both rules pick 0xA");
    check(res ? (c1.agree == c0.agree + 1 && c1.disagree == c0.disagree) : same(c0, c1),
          "T7c counted AGREE");
  }
  {
    // R1-R3: the resolve API itself.
    const uint32_t t = A::alloc(kSpan, 0x1000);
    const RC c0 = rc(W::kRsIdxIb);
    check(W::resolveOwned(W::kRsIdxIb, t & kMask, kSpan) == (res ? 0xA : 0),
          "R1 resolveOwned: 0xA for a live block when active, else 0");
    const RC c1 = rc(W::kRsIdxIb);
    check(res ? c1.a == c0.a + 1 : same(c0, c1), "R1c counted per site");
    check(same(rc(W::kRsIdxVb), RC{}) && same(rc(W::kRsS4Vb), RC{}) && same(rc(W::kRsRefresh), RC{}),
          "R2 other sites untouched");
    check(W::resolveOwned(W::kRsSites, t & kMask, kSpan) == 0, "R3 out-of-range site: no decision");
  }

  std::printf("[PHYSIB_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
