// p2.transport COMPOSED present (transport-composed lane, 2026-10-14; NATIVE_ROADMAP "p2.transport GAP MAP"): one
// present exactly as the game's present callers make it (D3DPresentSwapOnly 82611FB8: InsertFence 820FDD30, the vblank
// gate 82111EC8, then D3DDeviceSwap 82112050, whose 820F0048 re-emit opens the next segment), through production
// dispatch with the process env's gates (GATE, SWAP_BLOCK + SWAP, DEVCREATE, CALLBACK and the rest of the composed set),
// against the all-SDK oracle (the scoped SDK oracles PresentGateOracle + ShaderConstRunsOracle: every leaf declines).
//
//   --present-composed-test <route>      route = direct (the strong symbols) | indirect (the production dispatch table)
//
// Per case: the device, command-buffer list, ABI and return equal the oracle's (the swap's QPC bytes apart); the
// closed recording-pin segments, consumed in order by one in-order emulated CP (native records through cpSegment, or
// the oracle's PM4 bytes), give the same register file, scratch write-backs, interrupt, waits, flush / present order and
// fence stores - applied twice (a pin is replayed twice); and each producer call was converted EXACTLY ONCE: the gate by
// GATE (kind 17) or, when GATE is off / declines, its callback block by CALLBACK (kind 13) - never both; VdSwap's block
// by SWAP_BLOCK; the SDK body by SWAP; 820F0048 by DEVCREATE. Mock CP completion: not live pacing or vblank acceptance.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "ppc/ppc_recomp_shared.h"
#include "guest/guest_harness.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/native_flush.hpp"
#include "kernel/stubs.hpp"
#include "gfx/pm4_engine.hpp"
#include "gfx/xenos_state.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include <windows.h>

extern "C" void __imp__sub_820FDD30(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82111EC8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82112050(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820FD8C0(PPCContext& __restrict, uint8_t*);
namespace K = sr::kernel;
namespace N = sr::native::flush;
namespace {
constexpr uint32_t dev = 0x70600000u, buf = 0x90640000u, list = 0x70650000u;
constexpr uint32_t front = 0x70655000u, rect = 0x70655100u, fb = 0x70655200u;
constexpr uint32_t recorder = 0x70655300u, cell = 0x70655400u, stack = 0x70580000u;
constexpr uint32_t kPc = 0x7FE14000u, kPhys = 0x1FE14000u;  // the gate's scratch write-back block
constexpr uint32_t fbPhys = fb & 0x1FFFFFFFu;
constexpr uint32_t allocator = PPC_CODE_BASE + 4u, bytes = 0x10000u;
int passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "  ok " : "FAIL:", name);
  ok ? ++passed : ++failed;
}
bool envOn(const char* name) {
  const char* e = std::getenv(name);
  return e && e[0] == '1';
}
bool defaultOn(const char* name) {  // a baked gate (rows 5 / 3b): on unless =0
  const char* e = std::getenv(name);
  return !e || e[0] != '0';
}
uint32_t word(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void put(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
// The recording allocator (BeginNewSegment's callback), as swap_sdk_test: one 8 KiB-or-larger block per call.
void allocate(PPCContext& ctx, uint8_t*) {
  const uint32_t at = K::readGuestU32BE(dev + 0x34C0u);
  const uint32_t requested = K::readGuestU32BE(ctx.r5.u32);
  const uint64_t aligned = (uint64_t(requested) + 31u) & ~uint64_t(31u);
  const uint64_t size = aligned < 8192u ? 8192u : aligned;
  if (ctx.r3.u32 != dev || ctx.r4.u32 != 2u || ctx.r6.u32 != 32u || at < buf ||
      uint64_t(at) + size > uint64_t(buf) + bytes) {
    ++failed; ctx.r3.u64 = 0u; return;
  }
  K::writeGuestBE32(ctx.r5.u32, uint32_t(size));
  K::writeGuestBE32(dev + 0x34C0u, at + uint32_t(size));
  ctx.r3.u64 = at;
}
struct Case {
  const char* name;
  uint32_t interval, previousFront;   // the gate: 0x80000000 = immediate (skipped); previous front 0 = skipped
  int gateThreshold;                  // dev+48 relative to the cursor (negative: the gate kicks first)
  uint32_t format; bool changedFront, scaler, gamma, previousFence;  // the swap (swap_sdk_test's axes)
};
void fixture(const Case& c, uint32_t offset) {
  std::memset(K::guestVAToHost(dev), 0, 0x5000u);
  std::memset(K::guestVAToHost(buf), 0xEE, bytes);
  std::memset(K::guestVAToHost(list), 0xEE, 0x4000u);
  std::memset(K::guestVAToHost(front), 0, 0x800u);
  std::memset(K::guestVAToHost(stack - 0x2000u), 0xCD, 0x2000u);
  K::writeGuestBE32(0x82000948u, cell); K::writeGuestBE32(cell, dev);
  K::writeGuestBE32(0x829AC0ECu, 0u); K::writeGuestBE32(0x829AC100u, 0u);
  K::guestVAToHost(0x829ABD8Cu)[0] = 0u;
  const uint32_t cursor = buf + offset;
  // the commands already written (the cursor is the last written dword; the next write lands at cursor + 4): type-2
  // NOPs, so the first close's segment - [dev+0x34C8, cursor + 4) - is PM4 the CP consumes whole
  for (uint32_t a = buf; a <= cursor; a += 4u) K::writeGuestBE32(a, 0x80000000u);
  K::writeGuestBE32(dev + 40u, cursor);
  K::writeGuestBE32(dev + 44u, buf + 8192u - 4u);
  K::writeGuestBE32(dev + 48u, uint32_t(int64_t(cursor) + c.gateThreshold));
  // the recording pin: every close is a mode-B record in the list dev+0x32C8 (no ring, no pump)
  K::writeGuestBE32(dev + 0x3290u, buf + bytes - 4u);
  K::writeGuestBE32(dev + 0x3294u, 1u);
  K::writeGuestBE32(dev + 0x32D0u, list); K::writeGuestBE32(dev + 0x32D4u, list + 0x4000u);
  K::writeGuestBE32(dev + 0x34ACu, buf); K::writeGuestBE32(dev + 0x34B0u, buf + bytes);
  K::writeGuestBE32(dev + 0x34B4u, 8192u); K::writeGuestBE32(dev + 0x34C0u, buf + 8192u);
  K::writeGuestBE32(dev + 0x34C4u, cursor + 4u);
  K::writeGuestBE32(dev + 0x34C8u, buf);
  K::writeGuestBE32(dev + 0x4FC0u, 1u);
  K::writeGuestBE32(dev + 0x2A10u, fb); K::writeGuestBE32(dev + 0x2A1Cu, 0x101u);
  K::writeGuestBE32(fb, 0xFFu);
  K::writeGuestBE32(dev + 0x34D0u, c.previousFence ? 0xFFu : 0u);
  K::writeGuestBE32(dev + 0x3354u, recorder);
  std::memset(K::guestVAToHost(recorder), 0, 0x100u);
  K::writeGuestBE32(recorder + 0xA4u, dev); K::writeGuestBE32(recorder + 0xA8u, 8192u);
  K::writeGuestBE32(recorder + 0xACu, allocator);
  K::writeGuestBE32(dev + 0x33B0u, 0x00120000u);
  K::writeGuestBE32(dev + 0x33B4u, 0x89ABCDEFu);
  // the gate's inputs (present_gate_test's device): the scratch block, the pacing word, interval, previous front
  K::writeGuestBE32(dev + 0x2A14u, kPc);
  K::writeGuestBE32(dev + 0x2D3Cu, 0x7Fu << 23);
  K::writeGuestBE32(dev + 0x33A4u, c.interval);
  K::writeGuestBE32(dev + 0x3490u, c.previousFront);
  K::writeGuestBE32(dev + 13508u, buf);
  K::guestVAToHost(dev)[0x2A39u] = 0u;
  K::guestVAToHost(dev)[0x2A3Au] = c.scaler ? 0x10u : 0u;
  K::guestVAToHost(dev)[0x2A3Bu] = c.gamma ? 0x40u : 0u;
  K::writeGuestBE32(dev + 0x4E0Cu, c.changedFront ? 0u : 0xBF130000u);
  const uint32_t fetch[6] = {0x80000002u | (40u << 22), 0xBF130000u | c.format,
                             (719u << 13) | 1279u, 0xA5001414u, 0xABCDEF12u, 0x12345678u};
  for (uint32_t i = 0; i < 6u; ++i) K::writeGuestBE32(front + 28u + 4u * i, fetch[i]);
  for (uint32_t i = 0; i < 7u; ++i) K::writeGuestBE32(rect + 4u * i, i == 2u ? 1280u : i == 3u ? 720u : 0u);
  std::memcpy(K::guestVAToHost(dev + 0x33D0u), K::guestVAToHost(rect), 28u);
  K::ExportRegistry::instance().callOrStub("RtlInitializeCriticalSection", dev + 0x34F0u, 0, 0, 0, 0, 0);
}
// The in-order emulated CP: the scratch block and the fence words in their own memory; every other physical address
// unmapped (counted, equal in both arms); the gate's callback interrupt (mask 4) answered as the guest ISR's callback
// would release the gate (pc+0 = 0, pc+4 = 0); flush and present in order.
struct Run {
  std::array<uint8_t, 128> scratch{};
  std::array<uint8_t, 16> fence{};
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  std::vector<std::array<uint32_t, 4>> order;
  std::vector<uint32_t> registers;
  unsigned unmapped = 0, timestamps = 0, polls = 0;
  uint64_t waits = 0, blocked = 0, interrupts = 0, swaps = 0;
  bool bad = false;
  // why bad (bits): 1 a trace kind other than a store / write-back {what, addr, value}, 2 a second blocked wait,
  // 4 an interrupt other than the gate's, 8 a segment the CP could not take, 16 a part not consumed whole
  unsigned why = 0;
  uint32_t whyWhat = 0, whyAddr = 0, whyValue = 0;
  void fault(unsigned bit, uint32_t what = 0, uint32_t addr = 0, uint32_t value = 0) {
    if (!(why & bit)) { whyWhat = what; whyAddr = addr; whyValue = value; }
    why |= bit;
    bad = true;
  }
  static uint8_t* map(void* context, uint32_t phys) {
    auto& run = *static_cast<Run*>(context);
    if (phys >= kPhys && phys - kPhys <= run.scratch.size() - 4u) return run.scratch.data() + (phys - kPhys);
    if (phys >= fbPhys && phys - fbPhys <= run.fence.size() - 4u) return run.fence.data() + (phys - fbPhys);
    ++run.unmapped;
    return nullptr;
  }
  static void trace(void* context, uint32_t what, uint32_t addr, uint32_t value) {
    auto& run = *static_cast<Run*>(context);
    if (what > 1u) run.fault(1u, what, addr, value);
    run.writes.emplace_back(addr, value);
  }
  static uint32_t counter(void* context) { return 0xC10C0000u + ++static_cast<Run*>(context)->timestamps; }
  static bool running(void* context) {
    auto& run = *static_cast<Run*>(context);
    // A wait no callback releases (the gate's hold with the callback predicated off): release it once, as the vblank
    // countdown would; a second blocked wait is a wedge.
    if (++run.polls > 4u) { run.fault(2u, run.polls); return false; }
    put(run.scratch.data() + 4u, 0u);
    return true;
  }
  static void interrupt(void* context, uint32_t mask) {
    auto& run = *static_cast<Run*>(context);
    if (mask != 4u) run.fault(4u, mask);
    put(run.scratch.data(), 0u);
    put(run.scratch.data() + 4u, 0u);
  }
  static void flush(void* context) { static_cast<Run*>(context)->order.push_back({0u, 0u, 0u, 0u}); }
  static void swap(void* context, uint32_t address, uint32_t width, uint32_t height) {
    static_cast<Run*>(context)->order.push_back({1u, address, width, height});
  }
};
struct Segment { uint32_t va, phys, dwords; };
std::vector<Segment> closedSegments(bool& ok) {
  std::vector<Segment> result;
  ok = true;
  const uint32_t end = K::readGuestU32BE(dev + 0x32D0u);
  if (end < list || end > list + 0x4000u || ((end - list) & 7u)) { ok = false; return result; }
  for (uint32_t p = list; p < end; p += 8u) {
    const uint32_t header = K::readGuestU32BE(p), phys = K::readGuestU32BE(p + 4u), n = header & 0x01FFFFFFu;
    const uint32_t va = (buf & 0xE0000000u) | (phys & 0x1FFFFFFFu);
    if ((header & 0xFE000000u) != 0x82000000u || !n || va < buf || uint64_t(va) + 4ull * n > buf + bytes) {
      ok = false; break;
    }
    result.push_back({va, phys, n});
  }
  return result;
}
Run execute(const std::vector<Segment>& segments, const std::vector<uint8_t>& bufferBytes, bool native) {
  Run run;
  auto engine = std::make_unique<sr::gfx::Pm4Engine>();
  engine->cpMode = true; engine->submitDraws = false;
  engine->physToHost = Run::map; engine->user = &run;
  engine->cp.user = &run; engine->cp.flush = Run::flush; engine->cp.swap = Run::swap;
  engine->cp.interrupt = Run::interrupt; engine->cp.counter = Run::counter; engine->cp.running = Run::running;
  engine->binSelect = engine->binMask = 1u;  // the gate's callback block is predicated in: its interrupt releases
  auto& registers = sr::gfx::getXenosRegisters();
  for (uint32_t i = 0; i < sr::gfx::XenosRegisterFile::kNumRegisters; ++i) registers.write(i, 0xA5000000u | i);
  registers.write(0x1973u, 0u);
  engine->writeReg(0x1DCu, 0x33u);  // scratch 0 / 1 / 4 / 5 write back
  engine->writeReg(0x1DDu, kPhys);
  engine->cp.trace = Run::trace;
  put(run.fence.data(), 0xFFu);
  for (const Segment& s : segments) {
    if (native) {
      const uint64_t id = N::segmentPublishId(s.phys, s.dwords);
      if (!id || !N::cpSegment(*engine, id, s.phys, s.dwords)) run.fault(8u, uint32_t(id), s.phys, s.dwords);
    } else {
      engine->execute(bufferBytes.data() + (s.va - buf), 4u * s.dwords);
      if (engine->lastConsumed != 4u * s.dwords)
      {
        const uint8_t* w = bufferBytes.data() + (s.va - buf);  // the first dword the CP was given (big-endian)
        run.fault(16u, (uint32_t(w[0]) << 24) | (uint32_t(w[1]) << 16) | (uint32_t(w[2]) << 8) | w[3], s.phys,
                  (uint32_t(engine->lastConsumed) << 16) | s.dwords);
      }
    }
  }
  run.registers.assign(registers.data(), registers.data() + sr::gfx::XenosRegisterFile::kNumRegisters);
  run.waits = engine->cpWaits; run.blocked = engine->cpWaitsBlocked;
  run.interrupts = engine->cpInterrupts; run.swaps = engine->cpSwaps;
  return run;
}
bool sameRun(const Run& a, const Run& b) {
  return !a.bad && !b.bad && a.registers == b.registers && a.writes == b.writes && a.order == b.order &&
         a.scratch == b.scratch && a.fence == b.fence && a.waits == b.waits && a.blocked == b.blocked &&
         a.interrupts == b.interrupts && a.swaps == b.swaps && a.unmapped == b.unmapped && a.timestamps == b.timestamps;
}
void explain(const Run& a, const Run& b) {
  std::printf("  apply bad=%u/%u waits=%llu/%llu blocked=%llu/%llu irq=%llu/%llu swaps=%llu/%llu order=%zu/%zu writes=%zu/%zu "
              "unmapped=%u/%u\n", a.bad, b.bad, (unsigned long long)a.waits, (unsigned long long)b.waits,
              (unsigned long long)a.blocked, (unsigned long long)b.blocked, (unsigned long long)a.interrupts,
              (unsigned long long)b.interrupts, (unsigned long long)a.swaps, (unsigned long long)b.swaps, a.order.size(),
              b.order.size(), a.writes.size(), b.writes.size(), a.unmapped, b.unmapped);
  for (const Run* r : {&a, &b})
    if (r->why)
      std::printf("  %s bad why=%02X first {%08X %08X %08X}\n", r == &a ? "oracle" : "route", r->why, r->whyWhat,
                  r->whyAddr, r->whyValue);
  for (size_t i = 0; i < a.registers.size() && i < b.registers.size(); ++i)
    if (a.registers[i] != b.registers[i]) {
      std::printf("  first register mismatch reg=%04X expected=%08X actual=%08X\n", unsigned(i), a.registers[i],
                  b.registers[i]);
      break;
    }
}
bool sameDevice(const std::array<uint8_t, 0x5000>& expected) {
  bool same = true;
  for (uint32_t i = 0; i < expected.size(); ++i) {
    if (i >= 0x4E24u && i < 0x4E30u) continue;  // the swap's QPC-derived times
    if (expected[i] != K::guestVAToHost(dev)[i]) {
      if (same) std::printf("  device mismatch at +%04X expected=%02X actual=%02X\n", i, expected[i], K::guestVAToHost(dev)[i]);
      same = false;
    }
  }
  return same;
}
// One present: InsertFence, the gate, Swap, then the close that publishes the re-emit's segment.
void present(PPCContext& ctx, bool oracle, bool indirect) {
  uint8_t* base = K::getGuestBase();
  const uint64_t r1 = ctx.r1.u64, r13 = ctx.r13.u64;
  auto call = [&](uint32_t va, void (*imp)(PPCContext&, uint8_t*), void (*strong)(PPCContext&, uint8_t*)) {
    ctx.r1.u64 = r1; ctx.r13.u64 = r13; ctx.r3.u64 = dev;
    if (va == 0x82112050u) { ctx.r4.u64 = front; ctx.r5.u64 = rect; }
    if (oracle) {
      N::PresentGateOracle guard; N::ShaderConstRunsOracle shader;
      imp(ctx, base);
    } else if (indirect) {
      ctx.ctr.u32 = va; PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    } else {
      strong(ctx, base);
    }
  };
  call(0x820FDD30u, __imp__sub_820FDD30, sub_820FDD30);
  call(0x82111EC8u, __imp__sub_82111EC8, sub_82111EC8);
  call(0x82112050u, __imp__sub_82112050, sub_82112050);
}
}  // namespace

int runPresentComposedTest(const char* route) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  K::registerStubs(); sr::guest::initializeDispatchTable();
  const bool indirect = !std::strcmp(route, "indirect");
  if (!indirect && std::strcmp(route, "direct")) return 1;
  uint8_t* dispatchBase = K::getGuestBase();
  PPCFunc* saved = PPC_LOOKUP_FUNC(dispatchBase, allocator);
  PPC_LOOKUP_FUNC(dispatchBase, allocator) = allocate;
  // What the env asks for (each leaf still decides per call; the counters say which one took it). Every present-side
  // leaf requires the segmentless transport: with it off each declines and the SDK bodies run.
  const bool seg = N::on() && N::segmentlessOn();
  const bool gate = seg && N::presentGateOn(), swapBlock = seg && N::swapBlockOn(), swapSdk = seg && N::swapOn();
  // CALLBACK is default ON since the transport bake (native/transport-bake: unset = ON, =0 off, as kernel/native_flush
  // callbackOn reads it); DEVCREATE default ON since native/devcreate-fix (kernel/native_flush devCreateOn reads it the
  // same way)
  const bool devcreate = seg && defaultOn("SR_NATIVE_DEVCREATE"), callback = seg && defaultOn("SR_NATIVE_CALLBACK");
  const bool literal = envOn("SR_NATIVE_FLUSH_PM4");
  std::printf("[PRESENT_COMPOSED_TEST] route=%s gate=%u swapBlock=%u swap=%u devcreate=%u callback=%u literal=%u\n", route,
              gate, swapBlock, swapSdk, devcreate, callback, literal);
  if (indirect)
    check(PPC_LOOKUP_FUNC(dispatchBase, 0x82111EC8u) == &sub_82111EC8 &&
          PPC_LOOKUP_FUNC(dispatchBase, 0x82112050u) == &sub_82112050 &&
          PPC_LOOKUP_FUNC(dispatchBase, 0x820F0048u) == &sub_820F0048,
          "production dispatch resolves the strong gate, swap and re-emit replacements");
  const Case cases[] = {
      // name, interval, previous front, gate threshold, format, changedFront, scaler, gamma, previousFence
      {"plain", 1u, 0x12345000u, 4096, 6u, false, false, false, false},
      {"interval0", 0u, 0x12345000u, 4096, 6u, true, false, false, true},
      {"interval2-fmt7", 2u, 0x12345000u, 4096, 7u, true, false, false, false},
      {"interval4-fmt50", 4u, 0x12345000u, 4096, 50u, true, true, true, true},
      {"gate-kicks", 1u, 0x12345000u, -4, 6u, true, false, false, true},
      {"immediate", 0x80000000u, 0x12345000u, 4096, 6u, true, false, false, true},
      {"no-front", 1u, 0u, 4096, 6u, true, true, false, false},
      // (a GATE decline in a whole present is not constructible here: InsertFence kicks first and refreshes the
      // bounds a decline needs; the declined gate's composition - the SDK gate body, its 820FDA80 call to CALLBACK -
      // is the composed_no_gate mode, and the gate's own declines are present_gate_test's leaf cases)
      {"scaler-gamma", 1u, 0x12345000u, 4096, 6u, true, true, true, false},
  };
  for (uint32_t offset : {0u, 4u}) for (const Case& c : cases) {
    // ---- the oracle: every body the SDK's, its segments' PM4 consumed by the CP
    fixture(c, offset);
    PPCContext before{}; before.r1.u64 = stack; before.r3.u64 = dev; before.r13.u64 = stack - 0x1000u;
    before.lr = 0x12345678u;
    PPCContext oracle = before;
    const N::TransportCounters t0 = N::transportCounters();
    present(oracle, true, false);
    const N::TransportCounters t1 = N::transportCounters();
    // (the baked rows 5 / 3b too: Swap's re-emit reaches 820F0208, and inside the scopes it must stay the SDK's)
    const bool oracleUntouched = t1.gateCalls == t0.gateCalls && t1.gateSkip == t0.gateSkip &&
        t1.cbCalls == t0.cbCalls && t1.swapBlockCalls == t0.swapBlockCalls && t1.swapSdkCalls == t0.swapSdkCalls &&
        t1.segBeginCalls == t0.segBeginCalls && t1.tileRectCalls == t0.tileRectCalls &&
        t1.tilePredCalls == t0.tilePredCalls;
    char name[256];
    std::snprintf(name, sizeof(name), "%s offset=%u: the scoped SDK oracle runs every body the SDK's (no leaf took a call)",
                  c.name, offset);
    check(oracleUntouched, name);
    { PPCContext close = before; close.r3.u64 = dev; N::PresentGateOracle g; N::ShaderConstRunsOracle s;
      __imp__sub_820FD8C0(close, K::getGuestBase()); }
    std::array<uint8_t, 0x5000> expectedDevice;
    std::memcpy(expectedDevice.data(), K::guestVAToHost(dev), expectedDevice.size());
    std::vector<uint8_t> expectedBuffer(K::guestVAToHost(buf), K::guestVAToHost(buf) + bytes);
    std::vector<uint8_t> expectedList(K::guestVAToHost(list), K::guestVAToHost(list) + 0x4000u);
    bool listOk = false;
    const std::vector<Segment> expectedSegments = closedSegments(listOk);
    const Run expected = execute(expectedSegments, expectedBuffer, false);

    // ---- the production route with the env's gates
    fixture(c, offset);
    PPCContext actual = before;
    const N::TransportCounters a0 = N::transportCounters();
    present(actual, false, indirect);
    const N::TransportCounters a1 = N::transportCounters();
    { PPCContext close = before; close.r3.u64 = dev; sub_820FD8C0(close, K::getGuestBase()); }
    const uint64_t gateTook = (a1.gateCalls - a0.gateCalls) + (a1.gateSkip - a0.gateSkip);
    const uint64_t cbTook = a1.cbCalls - a0.cbCalls;
    const uint64_t blockTook = a1.swapBlockCalls - a0.swapBlockCalls, sdkTook = a1.swapSdkCalls - a0.swapSdkCalls;
    const uint64_t reemitTook = a1.segBeginCalls - a0.segBeginCalls;
    const uint64_t rowsTook = (a1.tileRectCalls - a0.tileRectCalls) + (a1.tilePredCalls - a0.tilePredCalls);
    std::printf("  route %s offset=%u: rows 5 / 3b took %llu / %llu calls\n", c.name, offset,
                (unsigned long long)(a1.tileRectCalls - a0.tileRectCalls),
                (unsigned long long)(a1.tilePredCalls - a0.tilePredCalls));
    // gate emission: everything but the immediate / no-front skips (handled natively by GATE as skips)
    const bool gateEmits = c.interval != 0x80000000u && c.previousFront != 0u;
    std::snprintf(name, sizeof(name), "%s offset=%u: each producer call converted exactly once (gate %llu callback %llu "
                  "block %llu swap %llu re-emit %llu)", c.name, offset, (unsigned long long)gateTook,
                  (unsigned long long)cbTook, (unsigned long long)blockTook, (unsigned long long)sdkTook,
                  (unsigned long long)reemitTook);
    const bool gateNative = gate;
    bool once = gateTook == (gateNative ? 1u : 0u) && cbTook == ((!gateNative && callback && gateEmits) ? 1u : 0u) &&
                !(gateTook && cbTook) && sdkTook == (swapSdk ? 1u : 0u) && blockTook == (swapBlock ? 1u : 0u) &&
                reemitTook == (devcreate ? 1u : 0u);
    check(once, name);
    bool same = sameDevice(expectedDevice) && actual.r3.u64 == oracle.r3.u64 && actual.r1.u64 == before.r1.u64 &&
                actual.r13.u64 == before.r13.u64 &&
                !std::memcmp(expectedList.data(), K::guestVAToHost(list), expectedList.size());
    const bool nothingNative = !gateTook && !cbTook && !blockTook && !sdkTook && !reemitTook && !rowsTook;
    // every command byte is the SDK's when nothing went native (rows 5 / 3b included) or under the PM4 output
    const bool bytesComparable = literal || nothingNative;
    if (bytesComparable) same &= !std::memcmp(expectedBuffer.data(), K::guestVAToHost(buf), bytes);
    std::snprintf(name, sizeof(name), "%s offset=%u: device, list, ABI and return equal the SDK's%s", c.name, offset,
                  bytesComparable ? " (and every command byte)" : "");
    check(same, name);
    bool ok = false;
    const std::vector<Segment> actualSegments = closedSegments(ok);
    const bool nativeApply = N::on() && N::segmentlessOn() && !nothingNative;
    const Run observed = execute(actualSegments, expectedBuffer, nativeApply);
    const Run replay = execute(actualSegments, expectedBuffer, nativeApply);
    const bool applied = listOk && ok && !expectedSegments.empty() && expectedSegments.size() == actualSegments.size() &&
                         sameRun(expected, observed) && sameRun(expected, replay) &&
                         expected.swaps == 1u && expected.interrupts == (gateEmits ? 1u : 0u);
    if (!applied) explain(expected, observed);
    std::snprintf(name, sizeof(name), "%s offset=%u: the CP's effects equal the SDK's, twice (registers, scratch, fence, "
                  "interrupt, waits, flush / present order)", c.name, offset);
    check(applied, name);
  }
  // The scoped SDK oracles' contract: inside PresentGateOracle every transport body is the SDK's; inside
  // ShaderConstRunsOracle an Entry owns the arena spinlock, so no leaf may open its own. Each present-side leaf must
  // decline in both (the call runs the SDK body): the gate 82111EC8, the callback 820FDA80 (the gate's own call) and
  // the re-emit 820F0048. The baked rows 5 / 3b (820F0208 the tile rects, 8210D400 SetPredication; the orchestrator's
  // decision of 2026-10-14) decline there too - and ONLY there: outside both scopes they take their calls as baked.
  auto rows = [](PPCContext& ctx) {
    ctx.r1.u64 = stack; ctx.r3.u64 = dev;
    ctx.r4.u64 = 0u; ctx.r5.u64 = 0u; ctx.r6.u64 = 1280u; ctx.r7.u64 = 720u;  // the tile rects (0, 0)-(1280, 720)
    sub_820F0208(ctx, K::getGuestBase());
    ctx.r1.u64 = stack; ctx.r3.u64 = dev; ctx.r4.u64 = 0u;                    // SetPredication(dev, 0)
    sub_8210D400(ctx, K::getGuestBase());
  };
  for (int scope = 0; scope < 3; ++scope) {
    const Case& c = cases[0];
    fixture(c, 0u);
    const N::TransportCounters s0 = N::transportCounters();
    {
      PPCContext ctx{}; ctx.r1.u64 = stack; ctx.r3.u64 = dev; ctx.r13.u64 = stack - 0x1000u;
      if (scope == 0) {
        N::PresentGateOracle guard;
        sub_82111EC8(ctx, K::getGuestBase());
        ctx.r1.u64 = stack; ctx.r3.u64 = dev; ctx.r4.u64 = 0u;
        sub_820F0048(ctx, K::getGuestBase());
        rows(ctx);
      } else if (scope == 1) {
        N::ShaderConstRunsOracle guard;
        sub_82111EC8(ctx, K::getGuestBase());
        ctx.r1.u64 = stack; ctx.r3.u64 = dev; ctx.r4.u64 = 0u;
        sub_820F0048(ctx, K::getGuestBase());
        rows(ctx);
      } else {
        rows(ctx);  // no scope: the default path
      }
    }
    const N::TransportCounters s1 = N::transportCounters();
    const char* where = scope == 0 ? "PresentGateOracle" : scope == 1 ? "ShaderConstRunsOracle" : "no scope";
    char name[200];
    const unsigned long long tile = s1.tileRectCalls - s0.tileRectCalls, pred = s1.tilePredCalls - s0.tilePredCalls;
    if (scope < 2) {
      std::snprintf(name, sizeof(name), "%s: the gate, its callback and the re-emit decline inside the scope (gate %llu "
                    "callback %llu re-emit %llu)", where,
                    (unsigned long long)(s1.gateCalls - s0.gateCalls + s1.gateSkip - s0.gateSkip),
                    (unsigned long long)(s1.cbCalls - s0.cbCalls),
                    (unsigned long long)(s1.segBeginCalls - s0.segBeginCalls));
      check(s1.gateCalls == s0.gateCalls && s1.gateSkip == s0.gateSkip && s1.cbCalls == s0.cbCalls &&
            s1.segBeginCalls == s0.segBeginCalls, name);
      std::snprintf(name, sizeof(name), "%s: rows 5 / 3b (820F0208, 8210D400) decline inside the scope (tile %llu "
                    "pred %llu)", where, tile, pred);
      check(tile == 0u && pred == 0u, name);
    } else {
      // the baked default: each leaf takes its call (the env may pin a row off: then it must not)
      const bool tileOn = N::on() && defaultOn("SR_NATIVE_TILERECTS"), predOn = N::on() && defaultOn("SR_NATIVE_TILEPRED");
      std::snprintf(name, sizeof(name), "no scope: the baked rows 5 / 3b take their calls as before (tile %llu of %u, "
                    "pred %llu of %u)", tile, tileOn ? 1u : 0u, pred, predOn ? 1u : 0u);
      check(tile == (tileOn ? 1u : 0u) && pred == (predOn ? 1u : 0u), name);
    }
  }
  PPC_LOOKUP_FUNC(dispatchBase, allocator) = saved;
  std::printf("[PRESENT_COMPOSED_TEST] %d passed, %d failed (offline pin contract; no live pacing or vblank acceptance)\n",
              passed, failed);
  return failed ? 1 : 0;
}
