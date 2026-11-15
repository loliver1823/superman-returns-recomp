// Actual XDK bytes and native record execution for the complete present gate.
// Mock completion proves ordering; it is not live vblank or frame acceptance.
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
#include <cstring>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>
#include <windows.h>

extern "C" void __imp__sub_82111EC8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820FD8C0(PPCContext& __restrict, uint8_t*);
namespace K = sr::kernel;
namespace N = sr::native::flush;
namespace {
constexpr uint32_t kDev = 0x70600000u, kBuf = 0x70640000u;
constexpr uint32_t kStack = 0x70580000u, kPc = 0x7FE14000u, kPhys = 0x1FE14000u;
constexpr uint32_t kDevBytes = 0x5000u;
int passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "  ok " : "FAIL:", name);
  ok ? ++passed : ++failed;
}
uint32_t word(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void put(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value >> 24); p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8); p[3] = uint8_t(value);
}
void device(uint32_t interval, uint32_t previous, uint32_t pc) {
  std::memset(K::guestVAToHost(kDev), 0, kDevBytes);
  std::memset(K::guestVAToHost(kBuf), 0xEE, 512);
  K::writeGuestBE32(kDev + 40u, kBuf);
  K::writeGuestBE32(kDev + 44u, kBuf + 508u);
  K::writeGuestBE32(kDev + 48u, kBuf + 256u);
  K::writeGuestBE32(kDev + 0x2A14u, pc);
  K::writeGuestBE32(kDev + 0x2D3Cu, 0x7Fu << 23);
  K::writeGuestBE32(kDev + 0x33A4u, interval);
  K::writeGuestBE32(kDev + 0x3490u, previous);
  K::writeGuestBE32(kDev + 13508u, kBuf);
  K::guestVAToHost(kDev)[0x2A39u] = 0x85;
}
struct Run {
  std::array<uint8_t, 128> memory{};
  std::vector<std::pair<uint32_t,uint32_t>> writes;
  sr::gfx::Pm4Engine* engine = nullptr;
  unsigned polls = 0;
  unsigned timestamps = 0;
  bool bad = false;
  static uint8_t* map(void* context, uint32_t phys) {
    auto& run = *static_cast<Run*>(context);
    if (phys < kPhys || phys - kPhys > run.memory.size() - 4u) {
      run.bad = true;
      return nullptr;
    }
    return run.memory.data() + (phys - kPhys);
  }
  static void trace(void* context, uint32_t what, uint32_t addr, uint32_t value) {
    auto& run = *static_cast<Run*>(context);
    if (what > 1u) run.bad = true;
    run.writes.emplace_back(addr, value);
  }
  static uint32_t counter(void* context) {
    return 0xC10C0000u + ++static_cast<Run*>(context)->timestamps;
  }
  static bool running(void* context) {
    auto& run = *static_cast<Run*>(context);
    if (++run.polls > 1u) { run.bad = true; return false; }
    // Predication suppressed the callback: only an external pacing source can
    // clear the gate. Assert that its unconditional suffix wait really blocked.
    if (run.engine->cpWaits != 2u || word(run.memory.data()+4u) != 1u ||
        word(run.memory.data()) != 4u || word(run.memory.data()+16u) != 0x0BADF00Du)
      run.bad = true;
    put(run.memory.data()+4u, 0u);
    return true;
  }
  static void interrupt(void* context, uint32_t mask) {
    auto& run = *static_cast<Run*>(context);
    if (mask != 4u || run.engine->cpWaits != 2u || word(run.memory.data()+4u) != 1u)
      run.bad = true;
    put(run.memory.data(), 0u);
    put(run.memory.data()+4u, 0u);
  }
};
struct Result {
  Run state;
  uint64_t waits, blocked, interrupts, skipped;
};
Result execute(bool native, bool predicate, const uint32_t* payload) {
  Run state;
  auto engine = std::make_unique<sr::gfx::Pm4Engine>();
  state.engine = engine.get();
  engine->submitDraws = false;
  engine->cpMode = true;
  engine->physToHost = Run::map;
  engine->user = &state;
  engine->cp.user = &state;
  engine->cp.running = Run::running;
  engine->cp.interrupt = Run::interrupt;
  engine->binSelect = predicate ? 1u : 0u;
  engine->binMask = 1u;
  engine->writeReg(0x1DCu, 0x33u);  // scratch 0/1/4/5 write back
  engine->writeReg(0x1DDu, kPhys);
  engine->cp.trace = Run::trace;  // configuration writes have their own trace event kind
  if (native) {
    check(N::presentGateApply(N::kRkPresentGate, payload, 3u, *engine), "native handler accepts three-word payload");
  } else {
    uint32_t values[39];
    std::array<uint8_t,156> bytes;
    N::presentGateDwords(payload, values);
    for (unsigned i = 0; i < 39; ++i) put(bytes.data()+4u*i,values[i]);
    engine->execute(bytes.data(),bytes.size());
    if (engine->lastConsumed != bytes.size()) state.bad = true;
  }
  state.engine = nullptr;
  return {std::move(state), engine->cpWaits, engine->cpWaitsBlocked, engine->cpInterrupts, engine->predSkipped};
}
void leafCase(const char* name, bool handled, uint32_t emitted, void (*mutate)(uint8_t*) = nullptr, uint64_t upper = 0) {
  device(1u,0x12345000u,kPc);
  uint8_t* d = K::guestVAToHost(kDev);
  if (mutate) mutate(d);
  std::vector<uint8_t> expected(kDevBytes);
  std::memcpy(expected.data(),d,expected.size());
  PPCContext ctx{};
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = upper | kDev;
  std::array<uint8_t,sizeof(PPCContext)> saved;
  std::memcpy(saved.data(),&ctx,saved.size());
  const bool result = N::presentGateLeaf(ctx,K::getGuestBase());
  if (handled) {
    put(expected.data()+40u,word(expected.data()+40u)+emitted);
    expected[0x2A39u] |= 0x20u;
  }
  bool same = result == handled && !std::memcmp(expected.data(),d,expected.size()) &&
              !std::memcmp(saved.data(),&ctx,saved.size());
  // Segmentless publication advances the cursor; it writes neither PM4 nor a
  // marker over the command-buffer allocation. Declines write neither either.
  for (unsigned i = 0; i < 512u; ++i) same &= K::guestVAToHost(kBuf)[i] == 0xEE;
  check(same,name);
}
}

int runPresentGateTest() {
  std::printf("[PRESENT_GATE_TEST] scoped XDK oracle and bounded execution equivalence\n");
  K::registerStubs();
  struct Case {uint32_t interval, previous, pc; bool emits; uint64_t upper = 0;} cases[] = {
    {0u,0x12345000u,kPc,true}, {1u,0x12345000u,kPc,true},
    {2u,0x12345000u,kPc,true}, {4u,0x12345000u,kPc,true},
    {1u,0x12345000u,0x7FFFFFFCu,true},
    {0x80000000u,0x12345000u,kPc,false}, {1u,0u,kPc,false}, {1u,0xFFFu,kPc,false},
    {1u,0x12345000u,kPc,true,0xFFFFFFFF00000000ull},
    {0x80000000u,0x12345000u,kPc,false,0x1234567800000000ull},
  };
  for (const auto& c : cases) {
    device(c.interval,c.previous,c.pc);
    std::vector<uint8_t> expected(kDevBytes);
    std::memcpy(expected.data(),K::guestVAToHost(kDev),expected.size());
    PPCContext ctx{};
    ctx.r1.u64 = kStack;
    ctx.r3.u64 = c.upper | kDev;
    N::PresentGateCheck comparison;
    const bool captured = N::presentGateCheckBefore(ctx, comparison);
    {
      N::PresentGateOracle oracle;
      __imp__sub_82111EC8(ctx,K::getGuestBase());
    }
    bool same = ctx.r3.u64 == (c.upper | kDev);
    if (c.emits) {
      uint32_t payload[3], values[39];
      same &= N::presentGatePayload(c.pc,c.interval,0x7Fu<<23,payload);
      same &= N::presentGateDwords(payload,values) == 39u;
      for (unsigned i = 0; i < 39u; ++i)
        same &= word(K::guestVAToHost(kBuf+4u+4u*i)) == values[i];
      put(expected.data()+40u,kBuf+156u);
    }
    expected[0x2A39u] |= 0x20u;
    same &= !std::memcmp(expected.data(),K::guestVAToHost(kDev),expected.size());
    same &= word(K::guestVAToHost(kBuf)) == 0xEEEEEEEEu;
    same &= word(K::guestVAToHost(kBuf+(c.emits ? 160u : 4u))) == 0xEEEEEEEEu;
    char name[160];
    std::snprintf(name,sizeof(name),"guest interval=%08X previous=%08X pc=%08X: exact bytes, full device, r3, boundaries",c.interval,c.previous,c.pc);
    check(same,name);
    check(captured && N::presentGateCheckAfter(comparison,ctx), "dry CHECK matches actual scoped SDK output");
  }
  // Detect changes to independent SDK output rather than just re-rendering a
  // native payload and comparing it with itself.
  device(1u,0x12345000u,kPc);
  PPCContext oracleCtx{};
  oracleCtx.r1.u64 = kStack;
  oracleCtx.r3.u64 = kDev;
  N::PresentGateCheck comparison;
  const bool captured = N::presentGateCheckBefore(oracleCtx,comparison);
  { N::PresentGateOracle oracle; __imp__sub_82111EC8(oracleCtx,K::getGuestBase()); }
  uint8_t* bytes = K::guestVAToHost(kBuf + 4u);
  bytes[0] ^= 1u;
  check(captured && !N::presentGateCheckAfter(comparison,oracleCtx), "dry CHECK detects an SDK command byte changed");
  bytes[0] ^= 1u;
  oracleCtx.r3.u64 |= 1ull << 32;
  check(!N::presentGateCheckAfter(comparison,oracleCtx), "dry CHECK detects wrong 64-bit r3 return");
  oracleCtx.r3.u64 = kDev;
  K::writeGuestBE32(kDev + 40u,kBuf + 152u);
  check(!N::presentGateCheckAfter(comparison,oracleCtx), "dry CHECK detects wrong cursor");
  K::writeGuestBE32(kDev + 40u,kBuf + 156u);
  K::guestVAToHost(kDev)[0x2A39u] &= uint8_t(~0x20u);
  check(!N::presentGateCheckAfter(comparison,oracleCtx), "dry CHECK detects missing SDK flag bit");
  device(8u,0x12345000u,kPc);
  check(!N::presentGateCheckBefore(oracleCtx,comparison) && !comparison.ready, "dry CHECK excludes undefined interval before SDK call");
  device(1u,0x12345000u,kPc);
  K::writeGuestBE32(kDev + 48u,kBuf - 4u);
  check(!N::presentGateCheckBefore(oracleCtx,comparison), "dry CHECK excludes kick-required paths");
  device(1u,0x12345000u,kPc);
  K::writeGuestBE32(kDev + 0x4E70u,2u);
  check(!N::presentGateCheckBefore(oracleCtx,comparison), "dry CHECK excludes active performance capture");
  oracleCtx.r3.u64 = 0;
  check(!N::presentGateCheckBefore(oracleCtx,comparison), "dry CHECK excludes unreadable device without guest fault");
  // Warm the readable-range cache, then make the final snapshot page unreadable.
  // RPM must fail safely even if the cached range still looks readable. Restore
  // the private page before any SDK/native-leaf execution uses this fixture.
  device(1u,0x12345000u,kPc);
  oracleCtx.r3.u64 = kDev;
  const bool warmed = N::presentGateCheckBefore(oracleCtx,comparison);
  uint8_t* protectedPage = K::guestVAToHost(kDev + 0x3000u);
  DWORD oldProtection = 0, restoredProtection = 0;
  const bool protectedOk = VirtualProtect(protectedPage,0x1000u,PAGE_NOACCESS,&oldProtection) != 0;
  const bool rejectedRange = protectedOk && !N::presentGateCheckBefore(oracleCtx,comparison) && !comparison.ready;
  const bool restored = !protectedOk || VirtualProtect(protectedPage,0x1000u,oldProtection,&restoredProtection) != 0;
  check(warmed && rejectedRange && restored, "dry CHECK rejects a cached-readable range crossing PAGE_NOACCESS without fault");
  if (!restored) return 1;
  uint32_t rejected[3] = {0xDEADu,0xDEADu,0xDEADu};
  check(!N::presentGatePayload(kPc,8u,0u,rejected) && rejected[0] == 0xDEADu &&
        !N::presentGatePayload(UINT32_MAX,1u,0u,rejected), "undefined interval and address overflow decline before payload writes");
  uint32_t payload[3];
  N::presentGatePayload(kPc,1u,8u<<23,payload);
  const bool enabled = N::presentGateOn() && N::on() && N::segmentlessOn();
  leafCase("leaf/Entry: gate defaults and successful segmentless cursor/device publication",enabled,156u);
  leafCase("leaf: full 64-bit r3 preserved on emission",enabled,156u,nullptr,0xFFFFFFFF00000000ull);
  leafCase("leaf: full 64-bit r3 preserved on skip",enabled,0u,
           [](uint8_t* d) {put(d+0x33A4u,0x80000000u);},0x1234567800000000ull);
  leafCase("leaf: exact extent limit is admitted",enabled,156u,[](uint8_t* d) {put(d+44u,kBuf+156u);});
  leafCase("leaf: malformed kick allocation declines without changing state",false,0u,
           [](uint8_t* d) {put(d+48u,kBuf-4u);});
  leafCase("leaf: both perf timestamps preserve device and return contract",enabled,188u,
           [](uint8_t* d) {put(d+0x4E70u,2u);});
  leafCase("leaf: undefined interval declines before any side effect",false,0u,
           [](uint8_t* d) {put(d+0x33A4u,8u);});
  leafCase("leaf: short extent declines without changing state",false,0u,
           [](uint8_t* d) {put(d+44u,kBuf+155u);});
  leafCase("leaf: exclusive footprint end overflow declines",false,0u,[](uint8_t* d) {
    put(d+40u,0xFFFFFF60u); put(d+44u,UINT32_MAX); put(d+48u,UINT32_MAX);
  });
  leafCase("leaf: unaligned cursor declines",false,0u,[](uint8_t* d) {put(d+40u,kBuf+1u);});
  leafCase("leaf: command-buffer mode declines before any mutation",false,0u,
           [](uint8_t* d) {d[0x2A38u] |= 0x80u;});
  leafCase("leaf: no-GPU mode declines before any mutation",false,0u,
           [](uint8_t* d) {d[0x2A39u] |= 2u;});
  leafCase("leaf: scratch mode declines before any mutation",false,0u,
           [](uint8_t* d) {d[0x2A39u] |= 0x40u;});
  leafCase("leaf: PIX mode declines before any mutation",false,0u,
           [](uint8_t* d) {put(d+0x4DF4u,1u);});
  leafCase("leaf: unsupported close mode declines before any mutation",false,0u,
           [](uint8_t* d) {put(d+0x4E84u,1u);});
  leafCase("leaf: immediate skip preserves flag bits and cursor",enabled,0u,
           [](uint8_t* d) {put(d+0x33A4u,0x80000000u);});
  leafCase("leaf: no front buffer skip preserves flag bits and cursor",enabled,0u,
           [](uint8_t* d) {put(d+0x3490u,0u);});
  const char* stress = std::getenv("SR_NATIVE_SEG_STRESS");
  if (stress && std::strtoul(stress,nullptr,0) == 32u)
    leafCase("leaf: stress kick declines without any side effect",false,0u,
             [](uint8_t* d) {put(d+13508u,kBuf-64u);});
  for (bool predicate : {false,true}) {
    const Result native = execute(true,predicate,payload);
    const Result pm4 = execute(false,predicate,payload);
    const uint64_t waits = predicate ? 4u : 2u;
    const std::vector<std::pair<uint32_t,uint32_t>> expectedWrites = {
      {kPhys+4u,1u},{kPhys+16u,0x82111E38u},{kPhys+20u,payload[0]},
      {kPhys,4u},{kPhys+16u,0x0BADF00Du},
    };
    const bool same = !native.state.bad && !pm4.state.bad && native.state.memory == pm4.state.memory &&
      native.state.writes == expectedWrites && native.state.writes == pm4.state.writes &&
      native.waits == waits && pm4.waits == waits && native.blocked == (predicate ? 0u : 1u) &&
      native.blocked == pm4.blocked && native.interrupts == (predicate ? 1u : 0u) &&
      native.interrupts == pm4.interrupts && native.skipped == (predicate ? 0u : 3u) &&
      native.skipped == pm4.skipped && native.state.polls == (predicate ? 0u : 1u);
    check(same,predicate ? "predicated callback: exact scratch ordering and four waits" :
                          "suppressed callback: two unconditional waits, external gate release, identical scratch ordering");
  }
  std::printf("[PRESENT_GATE_TEST] %d passed, %d failed (no live pacing or kick acceptance)\n",passed,failed);
  return failed ? 1 : 0;
}

namespace {
constexpr uint32_t kList = 0x70650000u, kBytes = 0x1000u;
constexpr uint32_t kRecorder = 0x70653000u, kDeviceCell = 0x70654000u;
constexpr uint32_t kAllocator = PPC_CODE_BASE + 4u;
unsigned allocatorUpdates = 0;
enum class GateRoute { leaf, direct, indirect, oracle };
struct GateCase {
  uint32_t interval, perf, writer, reader;
  int thresholdOffset;
  uint64_t upper;
  bool mutateBaseOnAllocate = false;
};
// A caller-provided recording allocator is a real SDK callback. Its deliberate
// state change distinguishes the SDK's pre-kick slot from its post-kick base.
void recordingAllocator(PPCContext& ctx, uint8_t*) {
  ++allocatorUpdates;
  const uint32_t dev = ctx.r3.u32, cursor = K::readGuestU32BE(dev + 0x34C0u);
  if (ctx.r4.u32 != 2u || ctx.r6.u32 != 32u) ++failed;
  K::writeGuestBE32(ctx.r5.u32,512u);
  K::writeGuestBE32(dev + 0x34C0u,cursor + 512u);
  K::writeGuestBE32(dev + 0x4E6Cu,kPc + 64u);
  K::writeGuestBE32(dev + 0x4E78u,3u);
  K::writeGuestBE32(dev + 0x4E74u,1u);
  ctx.r3.u64 = cursor;
}
void fullFixture(const GateCase& c, uint32_t offset) {
  device(c.interval, 0x12345000u, kPc);
  std::memset(K::guestVAToHost(kDev + kDevBytes), 0, 0x5000u - kDevBytes);
  std::memset(K::guestVAToHost(kBuf), 0xEE, kBytes);
  std::memset(K::guestVAToHost(kList), 0xEE, 0x800u);
  const uint32_t cursor = kBuf + offset;
  K::guestVAToHost(kDev)[0x2A39u] = 0;  // a supported native recording pin, not no-GPU mode
  K::writeGuestBE32(0x829AC0ECu, 0u);
  K::writeGuestBE32(kDev + 40u, cursor);
  K::writeGuestBE32(kDev + 44u, kBuf + kBytes - 4u);
  K::writeGuestBE32(kDev + 48u, cursor + c.thresholdOffset);
  K::writeGuestBE32(kDev + 0x3290u, kBuf + kBytes - 4u);
  K::writeGuestBE32(kDev + 0x3294u, 1u);
  K::writeGuestBE32(kDev + 0x32D0u, kList);
  K::writeGuestBE32(kDev + 0x32D4u, kList + 0x800u);
  K::writeGuestBE32(kDev + 0x34ACu, kBuf);
  K::writeGuestBE32(kDev + 0x34B0u, kBuf + kBytes);
  K::writeGuestBE32(kDev + 0x34B4u, 512u);
  K::writeGuestBE32(kDev + 0x34C0u, kBuf + 512u);
  K::writeGuestBE32(kDev + 0x34C4u, cursor + 4u);
  K::writeGuestBE32(kDev + 0x4E6Cu, kPc + 32u);
  K::writeGuestBE32(kDev + 0x4E70u, c.perf);
  K::writeGuestBE32(kDev + 0x4E74u, c.reader);
  K::writeGuestBE32(kDev + 0x4E78u, c.writer);
  K::writeGuestBE32(kDev + 0x4FC0u, 1u);
  allocatorUpdates = 0;
  if (c.mutateBaseOnAllocate) {
    std::memset(K::guestVAToHost(kRecorder),0,0x100u);
    K::writeGuestBE32(kDev + 0x3354u,kRecorder);
    K::writeGuestBE32(kRecorder + 0xA4u,kDev);
    K::writeGuestBE32(kRecorder + 0xA8u,512u);
    K::writeGuestBE32(kRecorder + 0xACu,kAllocator);
    K::writeGuestBE32(0x82000948u,kDeviceCell);
    K::writeGuestBE32(kDeviceCell,kDev);
    K::ExportRegistry::instance().callOrStub("RtlInitializeCriticalSection",kDev+0x34F0u,0,0,0,0,0);
  }
}
struct GateSegment { uint32_t va, phys, dwords; };
std::vector<GateSegment> closedSegments() {
  std::vector<GateSegment> result;
  const uint32_t end = K::readGuestU32BE(kDev + 0x32D0u);
  if (end < kList || end > kList + 0x800u || ((end - kList) & 7u)) return {};
  for (uint32_t p = kList; p < end; p += 8u) {
    const uint32_t header = K::readGuestU32BE(p), phys = K::readGuestU32BE(p + 4u);
    const uint32_t va = (phys & 0x1FFFFFFFu) | (kBuf & 0xE0000000u), n = header & 0x01FFFFFFu;
    if ((header & 0xFE000000u) != 0x82000000u || !n || va < kBuf || uint64_t(va) + 4ull * n > kBuf + kBytes) return {};
    result.push_back({va, phys, n});
  }
  return result;
}
bool gateAbi(const PPCContext& before, const PPCContext& after) {
  bool same = before.r1.u64 == after.r1.u64 && before.r13.u64 == after.r13.u64;
#ifndef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL
  same &= before.r2.u64 == after.r2.u64;
#endif
#ifndef PPC_CONFIG_NON_VOLATILE_AS_LOCAL
  same &= !std::memcmp(&before.r14, &after.r14, offsetof(PPCContext, r31) + sizeof(before.r31) - offsetof(PPCContext, r14));
  same &= !std::memcmp(&before.f14, &after.f14, offsetof(PPCContext, f31) + sizeof(before.f31) - offsetof(PPCContext, f14));
#endif
#ifndef PPC_CONFIG_CR_AS_LOCAL
  same &= !std::memcmp(&before.cr2, &after.cr2, sizeof(before.cr2)) &&
          !std::memcmp(&before.cr3, &after.cr3, sizeof(before.cr3)) &&
          !std::memcmp(&before.cr4, &after.cr4, sizeof(before.cr4));
#endif
  return same;
}
Run runSegments(const std::vector<GateSegment>& segments, const uint8_t* oracleBytes, bool native) {
  Run run;
  auto engine = std::make_unique<sr::gfx::Pm4Engine>();
  run.engine = engine.get();
  engine->cpMode = true; engine->submitDraws = false;
  engine->physToHost = Run::map; engine->user = &run; engine->cp.user = &run;
  engine->cp.interrupt = Run::interrupt; engine->cp.counter = Run::counter;
  engine->binSelect = engine->binMask = 1u;
  engine->writeReg(0x1DCu, 0x33u); engine->writeReg(0x1DDu, kPhys);
  engine->cp.trace = Run::trace;
  for (const GateSegment& s : segments) {
    if (native) {
      const uint64_t id = N::segmentPublishId(s.phys, s.dwords);
      if (!id || !N::cpSegment(*engine, id, s.phys, s.dwords)) run.bad = true;
    } else {
      engine->execute(oracleBytes + s.va - kBuf, 4u * s.dwords);
      if (engine->lastConsumed != 4u * s.dwords) run.bad = true;
    }
  }
  if (engine->cpInterrupts != 1u || engine->cpWaits != 4u || engine->cpWaitsBlocked) run.bad = true;
  run.engine = nullptr;
  return run;
}
}

int runPresentGateSdkTest(const char* requested) {
  passed = failed = 0;
  K::registerStubs();
  GateRoute route;
  if (!std::strcmp(requested, "leaf")) route = GateRoute::leaf;
  else if (!std::strcmp(requested, "direct")) route = GateRoute::direct;
  else if (!std::strcmp(requested, "indirect")) route = GateRoute::indirect;
  else if (!std::strcmp(requested, "oracle")) route = GateRoute::oracle;
  else if (!std::strcmp(requested, "fault-return")) route = GateRoute::leaf;
  else return 1;
  if (!N::on() || !N::segmentlessOn() || !N::presentGateOn()) return 1;
  sr::guest::initializeDispatchTable();
  uint8_t* dispatchBase = K::getGuestBase();
  PPCFunc* savedAllocator = PPC_LOOKUP_FUNC(dispatchBase,kAllocator);
  PPC_LOOKUP_FUNC(dispatchBase,kAllocator) = &recordingAllocator;
  if (route == GateRoute::indirect) {
    check(PPC_LOOKUP_FUNC(K::getGuestBase(), 0x82111EC8u) == &sub_82111EC8 &&
          PPC_LOOKUP_FUNC(K::getGuestBase(), 0x82111EC8u) != &__imp__sub_82111EC8,
          "production dispatch mapping resolves the strong present-gate replacement");
  }
  const GateCase cases[] = {
    {0u,0u,0u,0u,256,0ull}, {1u,0u,0u,0u,-4,0xABCDEF1200000000ull},
    {2u,2u,0u,0u,256,0xABCDEF1200000000ull}, {4u,2u,3u,1u,256,0ull},
    {1u,2u,1u,2u,-4,0xABCDEF1200000000ull}, // full timestamp ring, gate still kicks
    {1u,2u,0u,0u,-4,0xABCDEF1200000000ull}, // begin timestamp kicks
    {1u,2u,0u,0u,12,0xABCDEF1200000000ull}, // gate kicks after begin timestamp
    {1u,2u,0u,0u,16,0xABCDEF1200000000ull}, // final timestamp kicks, changes r3
    {1u,2u,0u,0u,172,0xABCDEF1200000000ull}, // equality must not kick
    {1u,2u,UINT32_MAX,1u,256,0ull}, {1u,2u,0x7FFFFFFFu,0u,256,0ull},
    {1u,2u,4u,0u,256,0ull}, // SDK uses writer+1, not modulo four
    {1u,2u,0u,0u,-4,0xABCDEF1200000000ull,true},
    {1u,2u,0u,0u,16,0xABCDEF1200000000ull,true},
  };
  for (uint32_t offset : {0u,4u}) for (const GateCase& c : cases) {
    fullFixture(c, offset);
    PPCContext before{}; before.r1.u64 = kStack; before.r3.u64 = c.upper | kDev;
    before.r13.u64 = kStack - 0x1000u;
    PPCContext oracle = before;
    {
      N::PresentGateOracle guard;
      __imp__sub_82111EC8(oracle, K::getGuestBase());
    }
    const uint64_t expectedReturn = oracle.r3.u64;
    const unsigned expectedAllocatorUpdates = allocatorUpdates;
    const uint32_t expectedCursor = K::readGuestU32BE(kDev + 40u);
    oracle.r3.u64 = kDev;
    { N::PresentGateOracle guard; __imp__sub_820FD8C0(oracle, K::getGuestBase()); }
    const auto expectedSegments = closedSegments();
    std::array<uint8_t,0x5000u> expectedDevice;
    std::array<uint8_t,kBytes> expectedBuffer;
    std::array<uint8_t,0x800u> expectedList;
    std::memcpy(expectedDevice.data(),K::guestVAToHost(kDev),expectedDevice.size());
    std::memcpy(expectedBuffer.data(),K::guestVAToHost(kBuf),expectedBuffer.size());
    std::memcpy(expectedList.data(),K::guestVAToHost(kList),expectedList.size());
    const Run expected = runSegments(expectedSegments,expectedBuffer.data(),false);
    fullFixture(c, offset);
    PPCContext actual = before;
    bool emitted = true;
    if (route == GateRoute::leaf) emitted = N::presentGateLeaf(actual,K::getGuestBase());
    else if (route == GateRoute::indirect) {
      actual.ctr.u32 = 0x82111EC8u; actual.lr = 0;
      uint8_t* base = K::getGuestBase(); PPCContext& ctx = actual;
      PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
    } else if (route == GateRoute::oracle) {
      N::PresentGateOracle guard; sub_82111EC8(actual,K::getGuestBase());
    } else sub_82111EC8(actual,K::getGuestBase());
    // Model the old unconditional-r3 restore bug at the observable boundary.
    // The runner requires this control to fail specifically on final kicks.
    if (!std::strcmp(requested,"fault-return")) actual.r3.u64 = before.r3.u64;
    bool same = emitted && actual.r3.u64 == expectedReturn && gateAbi(before,actual) &&
                K::readGuestU32BE(kDev + 40u) == expectedCursor && allocatorUpdates == expectedAllocatorUpdates;
    if (c.mutateBaseOnAllocate) same &= expectedAllocatorUpdates == 1u;
    actual.r3.u64 = kDev;
    if (route == GateRoute::oracle) {
      N::PresentGateOracle guard; sub_820FD8C0(actual,K::getGuestBase());
    } else sub_820FD8C0(actual,K::getGuestBase());
    const auto actualSegments = closedSegments();
    same &= !expectedSegments.empty() && actualSegments.size() == expectedSegments.size() &&
            !std::memcmp(expectedDevice.data(),K::guestVAToHost(kDev),expectedDevice.size()) &&
            !std::memcmp(expectedList.data(),K::guestVAToHost(kList),expectedList.size());
    const Run observed = runSegments(actualSegments,expectedBuffer.data(),route != GateRoute::oracle);
    same &= !expected.bad && !observed.bad && expected.memory == observed.memory &&
            expected.writes == observed.writes && expected.timestamps == observed.timestamps;
    const char* pm4 = std::getenv("SR_NATIVE_FLUSH_PM4");
    if (route == GateRoute::oracle || (pm4 && pm4[0] == '1'))
      same &= !std::memcmp(expectedBuffer.data(),K::guestVAToHost(kBuf),expectedBuffer.size());
    else for (uint32_t i = 0; i < kBytes; ++i) same &= K::guestVAToHost(kBuf)[i] == 0xEE;
    // A recording pin is replayed twice. Resolve the real transport ID again;
    // an implementation that consumes the record on its first pass must fail.
    const Run replay = runSegments(actualSegments,expectedBuffer.data(),route != GateRoute::oracle);
    same &= !replay.bad && replay.memory == expected.memory && replay.writes == expected.writes &&
            replay.timestamps == expected.timestamps;
    char name[192];
    std::snprintf(name,sizeof(name),"SDK route=%s interval=%u perf=%u writer=%08X reader=%u threshold=%d offset=%u mutateBase=%u: full device, r3, pin segments and execution",
                  requested,c.interval,c.perf,c.writer,c.reader,c.thresholdOffset,offset,c.mutateBaseOnAllocate);
    if (!same) std::printf("  gate mismatch emitted=%u return=%llX/%llX cursor=%08X/%08X segments=%zu/%zu bad=%u/%u writes=%zu/%zu\n",
       emitted,(unsigned long long)actual.r3.u64,(unsigned long long)expectedReturn,K::readGuestU32BE(kDev+40u),expectedCursor,
       actualSegments.size(),expectedSegments.size(),observed.bad,expected.bad,observed.writes.size(),expected.writes.size());
    check(same,name);
  }
  PPC_LOOKUP_FUNC(dispatchBase,kAllocator) = savedAllocator;
  std::printf("[PRESENT_GATE_SDK_TEST] %d passed, %d failed (offline pin contract, no live pacing acceptance)\n",passed,failed);
  return failed ? 1 : 0;
}
