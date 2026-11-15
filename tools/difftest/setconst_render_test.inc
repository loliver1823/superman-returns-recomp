// Included by kernel/native_flush.cpp only in difftest (SR_DIFFTEST): P2-B item 5's record (kRkSetConstMem) against
// the SDK, and the CP's render-back of it under the CPPM4 split (Astra round 1 on native/setconst-bake).
//
// D3DDeviceEmitPm4Packet 82113010 through production dispatch on identical fixtures, the per-present A/B
// (SR_AB_TOGGLE=SR_NATIVE_SETCONSTMEM, SR_AB_PRESENT=1) choosing the arm: OFF = the actual SDK body - the oracle, the
// XDK's bytes in the command buffer - and ON = the native Entry. SR_NATIVE_SETCONSTMEM itself is unset: the gate's
// default (ON since the bake) is what runs. After each call the "caller" fills the body as Lion 827333E0 does (all
// 16*count bytes here, so the bytes compare whole). Cases vary the cursor (pad 0 / 4 / 8 / 12 bytes), the count, the
// shader type and the predicate word dev+12440. Modes (one process each; the gates latch at static initialisation):
//   records - segmentless record mode: the record's payload {header, offset, body} against the SDK's packet; the CP
//             applies it through nativeRecord (setConstMemApply): the constants equal the body, and the render-back
//             counter cpPm4 does not move (the negative control of the live witness).
//   cppm4   - SR_NATIVE_FLUSH_CPPM4=1: the CP's render-back (cpRecordPm4) takes the record as PM4 - not the opaque
//             fallback - and equals the SDK's bytes without the pad NOPs; cpApplyRecord executes that PM4: the
//             constants equal the body and cpPm4 counts the record.
//   pm4     - SR_NATIVE_FLUSH_PM4=1: the producer-side split writes the span literal: the command buffer equals the
//             SDK's bytes, pad NOPs included.
// Each case prints its bytes ([SETCONST_RENDER] <case> sdk= / cp= / producer=, the pad NOPs stripped from producer=)
// so tools/difftest/setconst_render_run.py requires the CP's PM4 (cppm4) == the producer's (pm4) per case, across the
// two processes. Run with SR_PM4_RING=0 SR_PM4_RING_EXEC=0 SR_PM4_CP=0 (the runner's configuration; a bare run re-runs
// itself with it, tools/difftest/suite_bare.inc).
#include "kernel/kpcr.hpp"
#include "guest/guest_harness.hpp"
#include "gfx/xenos_state.hpp"

namespace sr::native::flush {
namespace scr {
namespace K = sr::kernel;
constexpr uint32_t kDev = 0x70800000u, kCell = 0x70806000u, kStack = 0x70780000u;
constexpr uint32_t kFb = 0x90807000u;                       // physical-window VAs
constexpr uint32_t kRing = 0x90808000u, kRingMask = 4095u;  // 16 KB ring
constexpr uint32_t kBuf = 0x90A00000u, kBufSize = 0x80000u;
constexpr uint32_t kSeg = kBuf + 0x100u;                    // the open segment's start
constexpr uint32_t kHsio = 0x829AC0ECu;

int passed = 0, failed = 0;
void check(bool ok, const std::string& name) {
  std::printf("%s%s\n", ok ? "  ok  " : "FAIL: ", name.c_str());
  ok ? ++passed : ++failed;
}
uint32_t rd(uint32_t va) { return K::readGuestU32BE(va); }
void wr(uint32_t va, uint32_t v) { K::writeGuestBE32(va, v); }
uint8_t* host(uint32_t va) { return K::guestVAToHost(va); }

PPCContext context() {
  PPCContext c{};
  c.r1.u64 = kStack;
  c.r13.u64 = K::kpcrCurrent();
  return c;
}

std::string hex(const std::vector<uint8_t>& b) {
  static const char* d = "0123456789ABCDEF";
  std::string s;
  for (uint8_t v : b) {
    s += d[v >> 4];
    s += d[v & 15u];
  }
  return s;
}

// A device with a fresh, mostly empty segment (the layout native/coher-sentinel's fixture uses): `gap` dwords of guest
// type-2 NOPs already written in it - the cursor dev+0x28 sets the pad - and dev+12440 = the predicate word.
void prepare(uint32_t gap, uint32_t pred) {
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
  wr(kDev + 0x34C4u, kSeg);
  for (uint32_t k = 0; k < gap; k++) wr(kSeg + 4u * k, 0x80000000u);
  wr(kDev + 0x28u, kSeg - 4u + 4u * gap);
  wr(kDev + 0x2Cu, kBuf + 0x60000u);
  wr(kDev + 0x30u, kBuf + 0x5F000u);
  wr(kDev + 0x2A1Cu, 0x101u);
  wr(kDev + 0x3298u, kSeg - 4u);
  wr(kDev + 0x329Cu, 1u);
  wr(kDev + 0x34BCu, 1u);
  wr(kDev + 0x34CCu, 0x7FFFFFFFu);
  wr(kDev + 0x2D18u, 0u);  // the SDK's empty coherency range 0x00000000FFFFFFFF
  wr(kDev + 0x2D1Cu, 0xFFFFFFFFu);
  wr(kDev + 12440u, pred);
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
}

struct Case {
  const char* name;
  uint32_t type, index, count, pred, gap;
};
struct Arm {
  uint32_t c0 = 0, r3 = 0, cursor = 0, padB = 0;
  std::vector<uint8_t> bytes;    // the command buffer from c0 + 4: pad NOPs, header, offset, body
  std::vector<uint32_t> record;  // the native arm's record words after its header (record modes)
  uint64_t pos = 0;
  bool recordFound = false;
};
uint32_t value(uint32_t seed, uint32_t i) { return 0x3F800000u + (seed << 12) + i; }

Arm runArm(const Case& c, uint32_t seed, bool native, int abBit) {
  for (int i = 0; i < 2 && sr::abt::on(abBit) != native; i++) sr::abt::onGuestSwap();
  prepare(c.gap, c.pred);
  Arm a;
  a.c0 = rd(kDev + 0x28u);
  a.padB = (0u - (a.c0 + 12u)) & 15u;
  a.pos = g_head;
  PPCContext x = context();
  x.r3.u64 = kDev;
  x.r4.u64 = c.type;
  x.r5.u64 = c.index;
  x.r6.u64 = c.count;
  PPC_LOOKUP_FUNC(K::getGuestBase(), 0x82113010u)(x, K::getGuestBase());
  a.r3 = x.r3.u32;
  a.cursor = rd(kDev + 0x28u);
  if (a.r3)
    for (uint32_t i = 0; i < 4u * c.count; i++) wr(a.r3 + 4u * i, value(seed, i));  // the caller's fill
  const uint32_t total = a.padB + 8u + 16u * c.count;
  a.bytes.assign(host(a.c0 + 4u), host(a.c0 + 4u) + total);
  if (native && !g_pm4Out && g_arena) {
    RecHdr h;
    std::memcpy(&h, g_arena + (a.pos % kRecArena), sizeof(h));
    if (h.magic == kRecMagic && h.pos == a.pos && h.bytes >= sizeof(RecHdr) && h.bytes <= kMaxDrawRec) {
      a.recordFound = true;
      a.record.resize((h.bytes - sizeof(RecHdr)) / 4u);
      std::memcpy(a.record.data(), g_arena + (a.pos % kRecArena) + sizeof(RecHdr), 4u * a.record.size());
    }
  }
  return a;
}

void runCase(const Case& c, uint32_t seed, int abBit, bool cppm4, bool pm4) {
  const std::string n = c.name;
  const Arm sdk = runArm(c, seed, false, abBit);
  // The oracle itself, from the fixture's inputs: the pad NOPs, SET_CONSTANT with 4*count values and the predicate
  // word, the offset word, the caller's body; r3 = the body, dev+0x28 = its last dword.
  const uint32_t nv = 4u * c.count;
  const uint32_t hdr = 0xC0002D00u | (nv << 16) | c.pred, off = ((c.index - (c.type << 8)) << 2) & 0x7FCu;
  std::vector<uint8_t> want(sdk.padB + 8u + 4u * nv);
  for (uint32_t k = 0; k < sdk.padB / 4u; k++) put32(want.data() + 4u * k, 0x80000000u);
  put32(want.data() + sdk.padB, hdr);
  put32(want.data() + sdk.padB + 4u, off);
  for (uint32_t i = 0; i < nv; i++) put32(want.data() + sdk.padB + 8u + 4u * i, value(seed, i));
  check(sdk.bytes == want && sdk.r3 == sdk.c0 + 12u + sdk.padB && sdk.cursor == sdk.r3 + 4u * nv - 4u,
        n + ": SDK arm: the XDK's pad NOPs, SET_CONSTANT header and offset word, the caller's body; r3 and dev+0x28");
  std::printf("[SETCONST_RENDER] %s sdk=%s\n", c.name, hex(sdk.bytes).c_str());

  const uint64_t calls0 = g_setConstMemCalls.get();
  const Arm nat = runArm(c, seed, true, abBit);
  check(g_setConstMemCalls.get() == calls0 + 1u, n + ": the native arm took the call (an Entry, not the guest body)");
  check(nat.c0 == sdk.c0 && nat.r3 == sdk.r3 && nat.cursor == sdk.cursor, n + ": r3 (the body) and dev+0x28 equal the SDK's");
  const std::vector<uint8_t> sdkPacket(sdk.bytes.begin() + sdk.padB, sdk.bytes.end());
  if (pm4) {
    check(nat.bytes == sdk.bytes, n + ": the producer's literal span (SR_NATIVE_FLUSH_PM4) equals the SDK's bytes");
    std::printf("[SETCONST_RENDER] %s producer=%s\n", c.name,
                hex(std::vector<uint8_t>(nat.bytes.begin() + nat.padB, nat.bytes.end())).c_str());
    return;
  }
  const uint32_t pred = (hdr & 1u) ? 2u : 0u;
  check(nat.recordFound && nat.record.size() == 4u && nat.record[0] == recTag(kRkSetConstMem, 3u, pred) &&
            nat.record[1] == be32(sdk.bytes.data() + sdk.padB) && nat.record[2] == be32(sdk.bytes.data() + sdk.padB + 4u) &&
            nat.record[3] == sdk.r3,
        n + ": one kRkSetConstMem record {the SDK's header, its offset word, the body}");
  if (!nat.recordFound || nat.record.size() != 4u) return;
  if (cppm4) {
    std::vector<uint8_t> cp;
    sr::gfx::Pm4Engine::NativeRecordStats st;
    bool opaque = false;
    uint32_t rendered = 0;
    cpRecordPm4(nat.record.data(), nat.record.data() + nat.record.size(), cp, st, opaque, rendered);
    check(!st.bad && !opaque && rendered == 1u,
          n + ": the CP renders the record to PM4 itself (one setconst, no opaque fallback to nativeRecord)");
    check(cp == sdkPacket, n + ": the CP's PM4 equals the SDK's bytes without the pad NOPs");
    std::printf("[SETCONST_RENDER] %s cp=%s\n", c.name, hex(cp).c_str());
  }
  // The CP applies the arena record as the consumer does (cpApplyRecord: the CPPM4 execute, or nativeRecord).
  auto& regs = sr::gfx::getXenosRegisters();
  for (uint32_t i = 0; i < nv; i++) regs.write(0x4000u + off + i, 0u);
  sr::gfx::Pm4Engine eng;
  eng.cpMode = true;
  eng.submitDraws = false;
  const uint64_t cp0 = g_cpSetConstPm4.get();
  const bool applied = cpApplyRecord(eng, nat.pos);
  bool constants = applied && !eng.nativeRecordFailed;
  for (uint32_t i = 0; i < nv; i++) constants = constants && regs.read(0x4000u + off + i) == value(seed, i);
  check(constants, n + ": the CP applies the record: the ALU constants equal the caller's body");
  if (cppm4)
    check(g_cpSetConstPm4.get() == cp0 + 1u, n + ": cpPm4 counts the record the CP rendered back and executed");
  else
    check(g_cpSetConstPm4.get() == cp0, n + ": cpPm4 does not move without the CPPM4 split");
}
}  // namespace scr

int runSetConstRenderTest(const char* mode) {
  namespace K = sr::kernel;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  {  // a bare run re-runs itself with exactly the runner's configuration for the mode (tools/difftest/suite_bare.inc)
    bare::Conf conf = {{L"SR_PM4_RING", L"0"}, {L"SR_PM4_RING_EXEC", L"0"}, {L"SR_PM4_CP", L"0"},
                       {L"SR_ALLOW_FORCING", L"0"}, {L"SR_AB_TOGGLE", L"SR_NATIVE_SETCONSTMEM"}, {L"SR_AB_PRESENT", L"1"},
                       {L"SR_NATIVE_SEGMENTLESS", L"1"}, {L"SR_NATIVE_FLUSH", L"1"}};
    if (std::strcmp(mode, "cppm4") == 0) conf.push_back({L"SR_NATIVE_FLUSH_CPPM4", L"1"});
    if (std::strcmp(mode, "pm4") == 0) conf.push_back({L"SR_NATIVE_FLUSH_PM4", L"1"});
    int code = 0;
    if (bare::handled("SETCONST_RENDER_TEST", "--setconst-render-test", mode, L"SR_NATIVE_SETCONSTMEM", conf, code))
      return code;
  }
  const bool cppm4 = std::strcmp(mode, "cppm4") == 0, pm4 = std::strcmp(mode, "pm4") == 0;
  if (!cppm4 && !pm4 && std::strcmp(mode, "records") != 0) {
    std::printf("FAIL: unknown mode %s (records | cppm4 | pm4)\n[SETCONST_RENDER_TEST] 0 passed, 1 failed\n", mode);
    return 1;
  }
  K::registerStubs();
  sr::guest::initializeDispatchTable();
  K::kpcrBindCurrent(scr::kStack, scr::kStack - 0x10000u, 3);
  const int abBit = sr::abt::reg("SR_NATIVE_SETCONSTMEM");
  scr::check(g_on && g_segmentless && g_cpPm4 == cppm4 && g_pm4Out == pm4 && abBit >= 0 && sr::abt::active(),
             "requested mode: record flush, segmentless, the CPPM4 / PM4 split as named, the per-present A/B");
  scr::check(setConstMemOn() && !std::getenv("SR_NATIVE_SETCONSTMEM"),
             "SR_NATIVE_SETCONSTMEM unset: the gate is ON (the bake's default)");
  scr::check(PPC_LOOKUP_FUNC(K::getGuestBase(), 0x82113010u) == ::sub_82113010,
             "production dispatch resolves 82113010 to the native override");
  bool mapped = true;
  for (uint32_t va : {scr::kDev, scr::kDev + 0x4FFFu, scr::kCell, scr::kFb, scr::kRing, scr::kBuf,
                      scr::kBuf + scr::kBufSize - 1u, scr::kStack - 0x2000u})
    mapped = mapped && scr::host(va) != nullptr;
  scr::check(mapped, "fixture memory mapped");
  if (scr::failed) {
    std::printf("[SETCONST_RENDER_TEST] %d passed, %d failed\n", scr::passed, scr::failed);
    return 1;
  }
  if (!g_arena) g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRecArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  // pad bytes from the cursor: kSeg is 16-aligned, c0 = kSeg - 4 + 4 * gap, pad = -(c0 + 12) & 15
  const scr::Case cases[] = {
      {"vs-c100-x4-pad0", 0u, 100u, 4u, 0u, 2u},
      {"ps-c100-x4-pad4-pred", 1u, 612u, 4u, 1u, 1u},
      {"vs-c96-x1-pad8", 0u, 96u, 1u, 0u, 0u},
      {"ps-c101-x2-pad12-pred", 1u, 613u, 2u, 1u, 3u},
  };
  uint32_t seed = 1;
  for (const auto& c : cases) scr::runCase(c, seed++, abBit, cppm4, pm4);
  std::printf("[SETCONST_RENDER_TEST] %d passed, %d failed\n", scr::passed, scr::failed);
  return scr::failed ? 1 : 0;
}
}  // namespace sr::native::flush
