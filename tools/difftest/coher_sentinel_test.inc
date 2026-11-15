// Included by kernel/native_flush.cpp only in difftest (SR_DIFFTEST): native/coher-sentinel.
//
// The deferred CPU-write range dev+0x2D18 (the big-endian doubleword {end, base}) and its EMPTY sentinel. The actual
// SDK close 820FD8C0 (its 820FD3B8) against the native segmentless close on identical fixtures, both through
// production dispatch, the segmentless per-present A/B choosing the arm (SR_AB_TOGGLE=SR_NATIVE_SEGMENTLESS,
// SR_AB_PRESENT=1: OFF = the guest body, ON = the native close). Between closes the fixtures run the actual SDK
// lock-core entry GpuInvalidateMemoryRange 820F4280 (owner: inline block; non-owner: the min / max merge into
// dev+0x2D18) and the actual SDK 820FD3B8 as the Z-pass kick 82110C78 calls it. Compared after every step: dev+0x2D18,
// the tail allocator, every device byte, the ring descriptors, the kick's block count; with SR_NATIVE_FLUSH_PM4=1 (the
// records written as PM4) also the whole command buffer and every coherency request an emulated CP applies (decoded
// and counted by Pm4Engine: top = ranges reaching 4 GB, page = the bogus {0xFFFFF000, 0x1000}). Absolute expectations
// hold on both arms: a close or kick with nothing merged requests nothing, a merged range is requested exactly once,
// dev+0x2D18 is the SDK's empty value after every take. Run with SR_PM4_RING=0 SR_PM4_RING_EXEC=0 SR_PM4_CP=0.
#include "kernel/kpcr.hpp"
#include "guest/guest_harness.hpp"

extern "C" void __imp__sub_820FD3B8(PPCContext& __restrict, uint8_t*);

namespace sr::native::flush {
namespace ct {
namespace K = sr::kernel;
constexpr uint32_t kDev = 0x70800000u, kCell = 0x70806000u, kObject = 0x70806800u, kStack = 0x70780000u;
constexpr uint32_t kOut = 0x70806C00u;                                            // the kick's out-params
constexpr uint32_t kFb = 0x90807000u;                                             // physical-window VAs
constexpr uint32_t kRing = 0x90808000u, kRingMask = 4095u;                        // 16 KB ring
constexpr uint32_t kBuf = 0x90A00000u, kBufSize = 0x80000u;                       // segments up, allocations down
constexpr uint32_t kAllocCb = 0x82110800u;
constexpr uint32_t kHsio = 0x829AC0ECu;
constexpr uint64_t kEmpty = 0x00000000FFFFFFFFull;  // 820FD3B8's r26; D3DDeviceResetState's value

int passed = 0, failed = 0;
void check(bool ok, const std::string& name) {
  std::printf("%s%s\n", ok ? "  ok  " : "FAIL: ", name.c_str());
  ok ? ++passed : ++failed;
}
uint32_t rd(uint32_t va) { return K::readGuestU32BE(va); }
void wr(uint32_t va, uint32_t v) { K::writeGuestBE32(va, v); }
uint8_t* host(uint32_t va) { return K::guestVAToHost(va); }
uint32_t phys(uint32_t va) { return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu); }
uint64_t range() { return (uint64_t(rd(kDev + 0x2D18u)) << 32) | rd(kDev + 0x2D1Cu); }

// The allocator callback (object dev+0x3354 +172, a supported SDK mechanism): bump allocations down from the top.
uint32_t g_top = 0;
PPC_FUNC(coherTestAllocate) {
  const uint32_t bytes = rd(ctx.r5.u32), align = ctx.r6.u32 ? ctx.r6.u32 : 4u;
  g_top = (g_top - bytes) & ~(align - 1u);
  ctx.r3.u64 = g_top;
}

PPCContext context() {
  PPCContext c{};
  c.r1.u64 = kStack;
  c.r13.u64 = K::kpcrCurrent();
  return c;
}
void call(uint32_t va, PPCContext& c) { PPC_LOOKUP_FUNC(K::getGuestBase(), va)(c, K::getGuestBase()); }

// A coherency request as the CP sees it: COHER_STATUS_HOST, COHER_SIZE_HOST, COHER_BASE_HOST.
struct Req {
  uint32_t status, size, base;
  bool operator==(const Req& o) const { return status == o.status && size == o.size && base == o.base; }
};
struct Step {
  uint64_t range = 0;
  uint32_t tail2C = 0, tail30 = 0, kickCount = 0, ringW = 0;
  std::vector<Req> reqs;  // the requests this step's coherency blocks carry (PM4 decoded; kicks always, closes in pm4)
  bool operator==(const Step& o) const {
    return range == o.range && tail2C == o.tail2C && tail30 == o.tail30 && kickCount == o.kickCount &&
           ringW == o.ringW && reqs == o.reqs;
  }
};
struct Arm {
  std::vector<Step> steps;
  std::vector<uint8_t> device, buffer, ring;
  uint64_t coher = 0, whole = 0, top = 0, page = 0;  // the emulated CP's counters over the ring and the kicks
  uint64_t nativeRange = 0, nativeTop = 0;          // the native close's own counters (deltas)
  bool decoded = true;                               // every coherency IB decoded as the 11-dword block
};

uint32_t g_ownerTid = 0;
// A device after D3DDeviceResetState: the range empty (0x00000000FFFFFFFF), a fresh segment, the thread owner or not.
void prepare(bool whole) {
  std::memset(host(kDev), 0, 0x5000u);
  std::memset(host(kCell), 0, 0x1000u);
  std::memset(host(kFb), 0, 0x1000u);
  std::memset(host(kRing), 0, (kRingMask + 1u) * 4u);
  std::memset(host(kBuf), 0, kBufSize);
  std::memset(host(kStack - 0x2000u), 0xA5, 0x2000u);
  wr(0x82000948u, kCell);
  wr(kCell, kDev);
  wr(kDev + 0x2A10u, kFb);
  wr(kDev + 0x2A14u, kFb + 0x80u);
  wr(kDev + 0x34A4u, kRing);
  wr(kDev + 0x34A8u, kRingMask);
  wr(kDev + 0x34ACu, kBuf);
  wr(kDev + 0x34B0u, kBuf + kBufSize);
  const uint32_t seg = kBuf + 0x100u;
  wr(kDev + 0x34C4u, seg);
  wr(kDev + 0x28u, seg - 4u);
  wr(kDev + 0x2Cu, kBuf + 0x60000u);
  wr(kDev + 0x30u, kBuf + 0x5F000u);
  wr(kDev + 0x2A1Cu, 0x101u);
  wr(kDev + 0x3298u, seg - 4u);
  wr(kDev + 0x329Cu, 1u);
  wr(kDev + 0x34BCu, 1u);
  wr(kDev + 0x3354u, kObject);
  wr(kObject + 164u, 0x11223344u);
  wr(kObject + 172u, kAllocCb);
  wr(kDev + 0x34CCu, 0x7FFFFFFFu);
  wr(kDev + 0x2D18u, uint32_t(kEmpty >> 32));  // D3DDeviceResetState 8210E520 (devResetFields): the empty range
  wr(kDev + 0x2D1Cu, uint32_t(kEmpty));
  wr(kDev + 0x2A08u, g_ownerTid + 0x100u);     // the device owner: another thread (the lock core merges)
  if (whole) host(kDev + 0x2A3Au)[0] |= 4u;
  wr(kHsio, 0u);
  {  // each arm starts from an empty transport
    std::lock_guard<std::mutex> lk(g_segMu);
    g_segFeet.clear();
    g_segById.clear();
    g_segDeferred.clear();
    auditClearLocked();  // step 6 I12: the audit, both arms (the ring / the map)
  }
  t_segPublishing.reset();
  g_head = g_reclaim = g_tailSeen = 0u;
  g_cpTail.store(0u);
  g_top = kBuf + kBufSize;
}

// Guest PM4 in the open segment (gap bytes): a NOP with a marker.
void gap(uint32_t k) {
  const uint32_t at = rd(kDev + 0x28u) + 4u;
  wr(at, 0xC0001000u);
  wr(at + 4u, 0xC0DE0000u | k);
  wr(kDev + 0x28u, at + 4u);
}

// Decodes the 11 / 22-dword coherency block at `va` (the SDK's layout: A31 {status}, 0x10A2F {size, base}, WAIT).
bool decode(uint32_t va, uint32_t dwords, std::vector<Req>& out) {
  if (dwords == 0u || dwords % 11u) return false;
  for (uint32_t b = 0; b < dwords; b += 11u) {
    const uint32_t at = va + 4u * b;
    if (rd(at) != 0x00000A31u || rd(at + 8u) != 0x00010A2Fu || rd(at + 20u) != 0xC0043C00u) return false;
    out.push_back({rd(at + 4u), rd(at + 12u), rd(at + 16u)});
  }
  return true;
}

// One arm: the step list on a fresh device, then the emulated CP over the ring (and the kicks' blocks, as their
// append would bring them) to count what it applies.
Arm runArm(const std::vector<std::string>& ops, bool nativeArm, int abBit, bool whole, bool pm4) {
  for (int i = 0; i < 2 && sr::abt::on(abBit) != nativeArm; i++) sr::abt::onGuestSwap();
  prepare(whole);
  Arm a;
  const uint64_t r0 = g_coherRange.get(), t0 = g_coherTop.get();
  std::vector<std::pair<uint32_t, uint32_t>> kicks;  // the kicks' blocks {va, dwords}
  uint32_t k = 0;
  for (const std::string& op : ops) {
    Step s;
    const uint32_t w0 = rd(kDev + 0x2A44u);
    PPCContext c = context();
    if (op == "close") {
      gap(k++);
      c.r3.u64 = kDev;
      call(0x820FD8C0u, c);
    } else if (op == "kick") {  // GpuBuildFlushMemoryPackets(dev, &phys, &count) as 82110C78 calls it
      wr(kOut, 0xDEADBEEFu);
      wr(kOut + 4u, 0xDEADBEEFu);
      c.r3.u64 = kDev;
      c.r4.u64 = kOut + 4u;
      c.r5.u64 = kOut;
      __imp__sub_820FD3B8(c, K::getGuestBase());
      s.kickCount = rd(kOut);
      if (s.kickCount) {
        const uint32_t va = 0x80000000u | rd(kOut + 4u);
        if (!decode(va, s.kickCount, s.reqs)) a.decoded = false;
        kicks.push_back({va, s.kickCount});
      }
    } else {  // "merge:<va>:<bytes>" (another thread: the lock core's min / max) or "owner:<va>:<bytes>" (inline)
      const bool owner = op.rfind("owner:", 0) == 0;
      const uint32_t va = uint32_t(std::stoul(op.substr(6, 8), nullptr, 16));
      const uint32_t bytes = uint32_t(std::stoul(op.substr(15), nullptr, 16));
      wr(kDev + 0x2A08u, owner ? g_ownerTid : g_ownerTid + 0x100u);
      c.r4.u64 = va;
      c.r5.u64 = bytes;
      call(0x820F4280u, c);
    }
    s.range = range();
    s.tail2C = rd(kDev + 0x2Cu);
    s.tail30 = rd(kDev + 0x30u);
    s.ringW = rd(kDev + 0x2A44u);
    if (pm4 && op == "close")  // the close's coherency IB(s): 11 / 22 dwords at the tail (the segment IB is longer)
      for (uint32_t i = w0; i < s.ringW; i += 3u)
        if (rd(kRing + 4u * i) == 0xC0013F00u) {
          const uint32_t p = rd(kRing + 4u * (i + 1u)), n = rd(kRing + 4u * (i + 2u));
          if ((n == 11u || n == 22u) && rd(0x80000000u | p) == 0x00000A31u && !decode(0x80000000u | p, n, s.reqs))
            a.decoded = false;
        }
    a.steps.push_back(s);
  }
  a.nativeRange = g_coherRange.get() - r0;
  a.nativeTop = g_coherTop.get() - t0;
  a.device.assign(host(kDev), host(kDev) + 0x5000u);
  a.buffer.assign(host(kBuf), host(kBuf) + kBufSize);
  a.ring.assign(host(kRing), host(kRing) + (kRingMask + 1u) * 4u);
  if (pm4) {  // the emulated CP: every ring IB literally (the PM4 split wrote the records as PM4), then the kicks
    auto eng = std::make_unique<sr::gfx::Pm4Engine>();
    eng->submitDraws = false;
    eng->cpMode = true;
    eng->physToHost = [](void*, uint32_t p) -> uint8_t* { return p < 0x20000000u ? host(0x80000000u | p) : nullptr; };
    eng->binMask = ~0ull;
    eng->binSelect = ~0ull;
    const uint32_t w1 = rd(kDev + 0x2A44u);
    for (uint32_t i = 0; i < w1; i += 3u)
      if (rd(kRing + 4u * i) == 0xC0013F00u)
        eng->execute(host(0x80000000u | rd(kRing + 4u * (i + 1u))), size_t(rd(kRing + 4u * (i + 2u))) * 4u, 1);
    for (const auto& kb : kicks) eng->execute(host(kb.first), size_t(kb.second) * 4u, 1);
    a.coher = eng->cpCoher;
    a.whole = eng->cpCoherWhole;
    a.top = eng->cpCoherTop;
    a.page = eng->cpCoherPage;
  }
  return a;
}

struct Case {
  const char* name;
  std::vector<std::string> ops;
  bool whole = false;
  std::vector<std::vector<Req>> expect;  // per step: the requests it must carry (closes in pm4 mode, kicks always)
};

int g_diag = 0;
void diag(const std::string& n, const Arm& sdk, const Arm& nat) {
  if (g_diag++ >= 12) return;
  for (size_t i = 0; i < sdk.steps.size() && i < nat.steps.size(); i++) {
    const Step &a = sdk.steps[i], &b = nat.steps[i];
    if (a == b) continue;
    std::printf("[COHER_DIAG] %s step %zu: range %016llX/%016llX tail %08X,%08X/%08X,%08X kick %u/%u ring %u/%u "
                "reqs %zu/%zu\n", n.c_str(), i, (unsigned long long)a.range, (unsigned long long)b.range, a.tail2C,
                a.tail30, b.tail2C, b.tail30, a.kickCount, b.kickCount, a.ringW, b.ringW, a.reqs.size(), b.reqs.size());
    for (const Req& r : b.reqs)
      std::printf("[COHER_DIAG]   native req status %08X size %08X base %08X\n", r.status, r.size, r.base);
    return;
  }
  for (size_t i = 0; i < 0x5000u; i++)
    if (sdk.device[i] != nat.device[i]) {
      std::printf("[COHER_DIAG] %s device first diff at +%04zX\n", n.c_str(), i & ~size_t(3));
      return;
    }
}

uint64_t g_cpRequests = 0;  // over every arm: the emulated CP did apply coherency requests (pm4 mode)
void runCase(const Case& cs, bool nativeOn, int abBit, bool pm4) {
  const std::string n = cs.name;
  const Arm sdk = runArm(cs.ops, false, abBit, cs.whole, pm4);
  const Arm nat = runArm(cs.ops, true, abBit, cs.whole, pm4);
  if (!(sdk.steps == nat.steps) || sdk.device != nat.device) diag(n, sdk, nat);
  g_cpRequests += sdk.coher + nat.coher;
  // ---- the SDK arm meets the expectations (the fixture means what it says) ----
  bool sdkExpect = sdk.steps.size() == cs.expect.size() && sdk.decoded;
  for (size_t i = 0; sdkExpect && i < cs.expect.size(); i++) {
    const bool kick = cs.ops[i] == "kick";
    if (kick || (pm4 && cs.ops[i] == "close")) sdkExpect = sdk.steps[i].reqs == cs.expect[i];
    if (kick || cs.ops[i] == "close") sdkExpect = sdkExpect && sdk.steps[i].range == kEmpty;  // every take leaves it empty
  }
  check(sdkExpect, n + ": SDK arm requests exactly the merged ranges and leaves dev+0x2D18 empty after every take");
  // ---- the native arm against the SDK arm ----
  bool ranges = sdk.steps.size() == nat.steps.size();
  for (size_t i = 0; ranges && i < sdk.steps.size(); i++) ranges = sdk.steps[i].range == nat.steps[i].range;
  check(ranges, n + ": dev+0x2D18 equals the SDK's after every step");
  bool kicks = ranges;
  for (size_t i = 0; kicks && i < sdk.steps.size(); i++)
    kicks = sdk.steps[i].kickCount == nat.steps[i].kickCount && (cs.ops[i] != "kick" || sdk.steps[i].reqs == nat.steps[i].reqs);
  check(kicks, n + ": every kick's coherency block (count, requests) equals the SDK's");
  bool tails = ranges;
  for (size_t i = 0; tails && i < sdk.steps.size(); i++)
    tails = sdk.steps[i].tail2C == nat.steps[i].tail2C && sdk.steps[i].tail30 == nat.steps[i].tail30 &&
            sdk.steps[i].ringW == nat.steps[i].ringW;
  check(tails, n + ": tail allocations and ring write index equal the SDK's after every step");
  check(sdk.device == nat.device, n + ": device equals the SDK's");
  check(sdk.ring == nat.ring, n + ": ring descriptors equal the SDK's");
  uint32_t sdkRangeBlocks = 0;  // the SDK closes' range blocks (status 0x03000000), for the native counter
  for (size_t i = 0; i < cs.ops.size(); i++)
    if (cs.ops[i] == "close")
      for (const Req& r : cs.expect[i]) sdkRangeBlocks += r.status == 0x03000000u;
  check(nativeOn ? nat.nativeRange == sdkRangeBlocks && nat.nativeTop == 0u : nat.nativeRange == 0u,
        n + ": native close range blocks (coherRange) equal the SDK's, none reaching 4 GB (coherTop 0)");
  if (pm4) {
    check(sdk.steps == nat.steps && nat.decoded, n + ": every coherency request equals the SDK's, step by step");
    check(sdk.buffer == nat.buffer, n + ": command buffer equals the SDK's (records written as PM4)");
    check(sdk.coher == nat.coher && sdk.whole == nat.whole && sdk.top == 0u && nat.top == 0u && sdk.page == 0u &&
              nat.page == 0u,
          n + ": the emulated CP's coherency counters equal the SDK's; top 0, page 0");
  }
}
}  // namespace ct

int runCoherSentinelTest(const char* mode) {
  namespace K = sr::kernel;
  using ct::Req;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  {  // a bare run re-runs itself with exactly the runner's configuration for the mode (tools/difftest/suite_bare.inc)
    bare::Conf conf = {{L"SR_PM4_RING", L"0"}, {L"SR_PM4_RING_EXEC", L"0"}, {L"SR_PM4_CP", L"0"},
                       {L"SR_ALLOW_FORCING", L"0"}, {L"SR_AB_TOGGLE", L"SR_NATIVE_SEGMENTLESS"}, {L"SR_AB_PRESENT", L"1"},
                       {L"SR_NATIVE_SEGMENTLESS", std::strcmp(mode, "segmentless-off") == 0 ? L"0" : L"1"},
                       {L"SR_NATIVE_FLUSH", L"1"}};
    if (std::strcmp(mode, "pm4") == 0) conf.push_back({L"SR_NATIVE_FLUSH_PM4", L"1"});
    int code = 0;
    if (bare::handled("COHER_SENTINEL_TEST", "--coher-sentinel-test", mode, L"SR_NATIVE_SEGMENTLESS", conf, code))
      return code;
  }
  const bool pm4 = std::strcmp(mode, "pm4") == 0;
  const bool segOff = std::strcmp(mode, "segmentless-off") == 0;  // control: both arms the guest close
  K::registerStubs();
  sr::guest::initializeDispatchTable();
  K::kpcrBindCurrent(ct::kStack, ct::kStack - 0x10000u, 3);
  const int abBit = sr::abt::reg("SR_NATIVE_SEGMENTLESS");
  ct::check(g_on && g_segmentless == !segOff && g_pm4Out == pm4 && !g_cpPm4 && abBit >= 0 && sr::abt::active(),
            "requested mode: record flush, segmentless, PM4 split and the per-present A/B");
  ct::check(PPC_LOOKUP_FUNC(K::getGuestBase(), 0x820FD8C0u) == sub_820FD8C0, "production dispatch resolves the native close");
  if (ct::failed) {
    std::printf("[COHER_SENTINEL_TEST] %d passed, %d failed\n", ct::passed, ct::failed);
    return 1;
  }
  {
    PPCContext c = ct::context();
    ct::call(0x82468EF8u, c);  // the lock core's current-thread id (dev+0x2A08 == it: the owner)
    ct::g_ownerTid = c.r3.u32;
  }
  const size_t index = (ct::kAllocCb - PPC_CODE_BASE) >> 2;
  PPCFunc* const saved = K::g_dispatchTable[index];
  K::g_dispatchTable[index] = ct::coherTestAllocate;
  if (!g_arena) g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRecArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

  // Ranges as the lock core computes them: phys(va) .. + bytes, rounded to pages by 820FD3B8.
  auto rangeReq = [](uint32_t va, uint32_t bytes) {
    const uint32_t lo = ct::phys(va), hi = lo + bytes;
    return Req{0x03000000u, ((hi + 0xFFFu) & ~0xFFFu) - (lo & ~0xFFFu), lo & ~0xFFFu};
  };
  const uint32_t lo = ct::phys(ct::kBuf) & ~0xFFFu;
  const Req wholeReq{0x01000000u, ((ct::phys(ct::kBuf + ct::kBufSize) + 0xFFFu) & ~0xFFFu) - lo, lo};
  using V = std::vector<Req>;
  const std::string A = "merge:90B12345:00002100", B = "merge:90B40010:00000800";
  const Req rA = rangeReq(0x90B12345u, 0x2100u), rB = rangeReq(0x90B40010u, 0x800u);
  const Req rAB{0x03000000u, rB.base + rB.size - rA.base, rA.base};
  std::vector<ct::Case> cases = {
      {"fresh-reset", {"close", "close"}, false, {V{}, V{}}},
      {"kick-after-reset", {"close", "kick", "close", "kick"}, false, {V{}, V{}, V{}, V{}}},
      {"kick-first", {"kick", "close", "kick"}, false, {V{}, V{}, V{}}},
      {"non-owner-merge", {"close", A, "close", "kick", "close"}, false, {V{}, V{}, V{rA}, V{}, V{}}},
      {"merge-on-reset", {A, "close", "close"}, false, {V{}, V{rA}, V{}}},
      {"two-merges", {"close", A, B, "close", "kick"}, false, {V{}, V{}, V{}, V{rAB}, V{}}},
      {"merge-taken-by-kick", {"close", A, "kick", "close", "kick"}, false, {V{}, V{}, V{rA}, V{}, V{}}},
      {"owner-inline", {"close", "owner:90B12345:00002100", "close", "kick"}, false, {V{}, V{}, V{}, V{}}},
      {"whole", {"close", A, "close", "kick"}, true, {V{wholeReq}, V{}, V{rA, wholeReq}, V{wholeReq}}},
      {"cycle", {"close", "kick", A, "close", "kick", B, "kick", "close", "kick", "close"}, false,
       {V{}, V{}, V{}, V{rA}, V{}, V{}, V{rB}, V{}, V{}, V{}}},
  };
  for (const auto& cs : cases) ct::runCase(cs, !segOff, abBit, pm4);
  if (pm4) ct::check(ct::g_cpRequests > 0u, "the emulated CP applied coherency requests (the counters are not vacuous)");

  K::g_dispatchTable[index] = saved;
  std::printf("[COHER_SENTINEL_TEST] %d passed, %d failed\n", ct::passed, ct::failed);
  return ct::failed ? 1 : 0;
}
}  // namespace sr::native::flush
