// The real VdSwap export's reserved block and native segment transport.
// Mock present callbacks prove ordering and wiring, not live graphics/pacing.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "ppc/ppc_recomp_shared.h"
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
#include <vector>
#include <windows.h>

extern "C" void __imp__sub_820FD8C0(PPCContext& __restrict, uint8_t*);
namespace K = sr::kernel;
namespace N = sr::native::flush;
namespace G = sr::gfx;
namespace {
constexpr uint32_t kDev = 0x70600000u, kCell = 0x70654000u;
constexpr uint32_t kBuf = 0x90640000u, kList = 0x70650000u;
constexpr uint32_t kFetch = 0x70651000u, kFb = 0x70652000u;
constexpr uint32_t kDevBytes = 0x5000u, kBufBytes = 0x1000u;
int passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "  ok " : "FAIL:", name);
  ok ? ++passed : ++failed;
}
uint32_t word(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void fixture(uint32_t base, uint32_t offset, bool fetch) {
  std::memset(K::guestVAToHost(kDev), 0, kDevBytes);
  std::memset(K::guestVAToHost(kBuf), 0xEE, kBufBytes);
  std::memset(K::guestVAToHost(kList), 0xEE, 0x800u);
  K::writeGuestBE32(0x82000948u, kCell);
  K::writeGuestBE32(kCell, kDev);
  K::writeGuestBE32(0x829AC0ECu, 0u);
  const uint32_t cursor = kBuf + offset;
  K::writeGuestBE32(kDev + 40u, cursor);
  K::writeGuestBE32(kDev + 44u, kBuf + kBufBytes - 4u);
  K::writeGuestBE32(kDev + 48u, cursor + 700u);
  K::writeGuestBE32(kDev + 13508u, kBuf);
  K::writeGuestBE32(kDev + 0x3290u, kBuf + kBufBytes - 4u);
  K::writeGuestBE32(kDev + 0x3294u, 1u);
  K::writeGuestBE32(kDev + 0x32D0u, kList);
  K::writeGuestBE32(kDev + 0x32D4u, kList + 0x800u);
  K::writeGuestBE32(kDev + 0x34ACu, kBuf);
  K::writeGuestBE32(kDev + 0x34B0u, kBuf + kBufBytes);
  K::writeGuestBE32(kDev + 0x34B4u, 512u);
  K::writeGuestBE32(kDev + 0x34C0u, kBuf + 512u);
  K::writeGuestBE32(kDev + 0x34C4u, cursor + 4u);
  K::writeGuestBE32(kDev + 0x4FC0u, 1u);
  const uint32_t values[6] = {0x80000002u | (40u << 22), base | 6u,
    (719u << 13) | 1279u, 0x1414u, 0xA5A50123u, 0x54321000u};
  for (uint32_t i = 0; i < 6u; ++i) K::writeGuestBE32(kFetch + 4u * i, values[i]);
  K::writeGuestBE32(kFb, fetch ? base : 0u);
}
struct Segment { uint32_t va, phys, dwords; };
std::vector<Segment> segments() {
  std::vector<Segment> result;
  const uint32_t end = K::readGuestU32BE(kDev + 0x32D0u);
  if (end < kList || end > kList + 0x800u || ((end - kList) & 7u)) return {};
  for (uint32_t p = kList; p < end; p += 8u) {
    const uint32_t h = K::readGuestU32BE(p), phys = K::readGuestU32BE(p + 4u);
    const uint32_t va = 0x80000000u | (phys & 0x1FFFFFFFu), n = h & 0x01FFFFFFu;
    if ((h & 0xFE000000u) != 0x82000000u || !n || va < kBuf ||
        uint64_t(va) + 4ull * n > kBuf + kBufBytes) return {};
    result.push_back({va, phys, n});
  }
  return result;
}
struct Result {
  std::vector<uint32_t> registers;
  std::vector<std::array<uint32_t,4>> order;
  uint64_t swaps = 0;
  bool bad = false;
  G::Pm4Engine* engine = nullptr;
  static void flush(void* opaque) {
    auto& r = *static_cast<Result*>(opaque);
    r.order.push_back({0u, 0u, 0u, 0u});
    if (r.engine->cpSwaps != 1u) r.bad = true;
  }
  static void swap(void* opaque, uint32_t fb, uint32_t w, uint32_t h) {
    auto& r = *static_cast<Result*>(opaque);
    if (r.order.size() != 1u || r.order[0][0] != 0u || r.engine->cpSwaps != 1u) r.bad = true;
    r.order.push_back({1u,fb,w,h});
  }
};
Result execute(const std::vector<Segment>& parts, const uint8_t* bytes, bool native) {
  Result r;
  auto eng = std::make_unique<G::Pm4Engine>();
  r.engine = eng.get(); eng->cpMode = true; eng->submitDraws = false;
  eng->cp.user = &r; eng->cp.flush = Result::flush; eng->cp.swap = Result::swap;
  auto& regs = G::getXenosRegisters();
  for (uint32_t i = 0; i < G::XenosRegisterFile::kNumRegisters; ++i) regs.write(i, 0xA5000000u | i);
  for (const auto& p : parts) {
    if (native) {
      const uint64_t id = N::segmentPublishId(p.phys,p.dwords);
      if (!id || !N::cpSegment(*eng,id,p.phys,p.dwords)) r.bad = true;
    } else {
      eng->execute(bytes + p.va - kBuf,4u*p.dwords);
      if (eng->lastConsumed != 4u*p.dwords) r.bad = true;
    }
  }
  r.registers.assign(regs.data(),regs.data()+G::XenosRegisterFile::kNumRegisters);
  r.swaps = eng->cpSwaps; r.engine = nullptr;
  if (r.swaps != 1u || r.order.size() != 2u) r.bad = true;
  return r;
}
void close(uint32_t cursor, bool oracle) {
  // The 82112050 caller advances the cursor after the import returns.
  K::writeGuestBE32(kDev + 40u,cursor + 256u);
  PPCContext context{}; context.r1.u64 = 0x70580000u; context.r3.u64 = kDev;
  if (oracle) { N::PresentGateOracle guard; __imp__sub_820FD8C0(context,K::getGuestBase()); }
  else sub_820FD8C0(context,K::getGuestBase());
}
}

int runSwapBlockTest(const char* mode) {
  K::registerStubs();
  const auto swap = K::ExportRegistry::instance().getExport("VdSwap");
  check(swap != nullptr,"production registry resolves VdSwap");
  if (!swap) return 1;
  const bool native = N::swapBlockOn() && N::segmentlessOn();
  const char* literal = std::getenv("SR_NATIVE_FLUSH_PM4");
  const bool writesBytes = !native || (literal && literal[0] == '1');
  for (uint32_t offset : {0u,4u}) for (uint32_t base : {0x9F130000u,0xBF130000u,0xDF130000u,0u}) {
    const bool fetch = base != 0u;
    const uint32_t cursor = kBuf + offset, at = cursor + 4u;
    fixture(base,offset,fetch);
    const uint64_t before = K::getVdSwapCount();
    { N::PresentGateOracle guard; check(swap(at,fetch ? kFetch : 0u,0u,0u,0u,kFb) == 0u,
                                     "actual PM4 oracle export returns success"); }
    check(K::getVdSwapCount() == before+1u,"oracle export records exactly one guest swap");
    std::array<uint8_t,256> block;
    std::memcpy(block.data(),K::guestVAToHost(at),block.size());
    // Independent reference prefix, physical page, format bits and full NOP tail.
    bool prefix = word(block.data()) == (fetch ? 0x00054800u : 0xC0036400u);
    const uint32_t idx = fetch ? 7u : 0u;
    prefix &= word(block.data()+4u*idx) == 0xC0036400u &&
              word(block.data()+4u*(idx+1u)) == 0x53574150u &&
              word(block.data()+4u*(idx+2u)) == (base & 0x1FFFF000u) &&
              word(block.data()+4u*(idx+3u)) == 1280u && word(block.data()+4u*(idx+4u)) == 720u;
    if (fetch) prefix &= word(block.data()+8u) == ((base & 0x1FFFF000u)|6u);
    for (uint32_t i = idx+5u; i < 64u; ++i) prefix &= word(block.data()+4u*i) == 0x80000000u;
    check(prefix,"actual export has reference fetch fixup, SWAP dimensions and NOP tail");
    close(cursor,true);
    const auto expectedSegments = segments();
    std::array<uint8_t,kBufBytes> expectedBuffer;
    std::array<uint8_t,kDevBytes> expectedDevice;
    std::array<uint8_t,0x800> expectedList;
    std::memcpy(expectedBuffer.data(),K::guestVAToHost(kBuf),expectedBuffer.size());
    std::memcpy(expectedDevice.data(),K::guestVAToHost(kDev),expectedDevice.size());
    std::memcpy(expectedList.data(),K::guestVAToHost(kList),expectedList.size());
    const Result expected = execute(expectedSegments,expectedBuffer.data(),false);
    fixture(base,offset,fetch);
    const uint64_t beforeActual = K::getVdSwapCount();
    check(swap(at,fetch ? kFetch : 0u,0u,0u,0u,kFb) == 0u,"actual production export returns success");
    check(K::getVdSwapCount() == beforeActual+1u && K::readGuestU32BE(kDev+40u) == cursor,
          "production export records one swap and preserves the caller-owned cursor");
    // The stack/fetch input may be reused as soon as the import returns.
    std::memset(K::guestVAToHost(kFetch),0x77,24u);
    if (!std::strcmp(mode,"fault-bytes")) K::writeGuestBE32(at,0xDEADBEEFu);
    close(cursor,!N::segmentlessOn());
    const auto actualSegments = segments();
    bool same = !expectedSegments.empty() && actualSegments.size() == expectedSegments.size() &&
      !std::memcmp(expectedDevice.data(),K::guestVAToHost(kDev),expectedDevice.size()) &&
      !std::memcmp(expectedList.data(),K::guestVAToHost(kList),expectedList.size());
    if (writesBytes) same &= !std::memcmp(expectedBuffer.data(),K::guestVAToHost(kBuf),expectedBuffer.size());
    else for (uint32_t i = 0; i < kBufBytes; ++i) same &= K::guestVAToHost(kBuf)[i] == 0xEE;
    const Result actual = execute(actualSegments,writesBytes ? K::guestVAToHost(kBuf) : nullptr,native);
    const Result replay = execute(actualSegments,writesBytes ? K::guestVAToHost(kBuf) : nullptr,native);
    same &= !expected.bad && !actual.bad && !replay.bad && expected.registers == actual.registers &&
            expected.order == actual.order && expected.order == replay.order && expected.registers == replay.registers;
    char name[150];
    std::snprintf(name,sizeof(name),"production SWAP base=%08X offset=%u: full device, segments, owned fetch, full registers and two ordered replays",base,offset);
    if (!same) std::printf("  mismatch parts=%zu/%zu bad=%u/%u/%u swaps=%llu/%llu/%llu\n",actualSegments.size(),expectedSegments.size(),
      expected.bad,actual.bad,replay.bad,(unsigned long long)expected.swaps,(unsigned long long)actual.swaps,(unsigned long long)replay.swaps);
    check(same,name);
  }
  fixture(0x9F130000u,0u,true);
  const std::array<std::array<uint32_t,3>,7> declines = {{{0u,kBuf+4u,kFetch},
    {kDev,kBuf+8u,kFetch},{kDev,kBuf+5u,kFetch},{kDev,0xFFFFFF80u,kFetch},
    {kDev,kBuf-4u,kFetch},{kDev,kBuf+4u,0xFFFFFFF0u},{0xFFFFF000u,kBuf+4u,kFetch}}};
  for (const auto& input : declines) {
    std::array<uint8_t,kDevBytes> device; std::array<uint8_t,kBufBytes> buffer;
    std::memcpy(device.data(),K::guestVAToHost(kDev),device.size());
    std::memcpy(buffer.data(),K::guestVAToHost(kBuf),buffer.size());
    check(!N::swapBlockLeaf(input[0],input[1],input[2],1280u,720u) &&
      !std::memcmp(device.data(),K::guestVAToHost(kDev),device.size()) &&
      !std::memcmp(buffer.data(),K::guestVAToHost(kBuf),buffer.size()),"malformed native admission declines without mutation");
  }
  for (unsigned i = 0; i < 7u; ++i) {
    fixture(0x9F130000u,0u,true);
    if (i == 0u) K::guestVAToHost(kDev)[0x2A38u] = 0x80u;
    if (i == 1u) K::guestVAToHost(kDev)[0x2A39u] = 0x42u;
    if (i == 2u) K::writeGuestBE32(kDev+0x4DF4u,1u);
    if (i == 3u) K::writeGuestBE32(kDev+0x4E84u,1u);
    if (i == 4u) K::writeGuestBE32(0x829AC0ECu,1u);
    if (i == 5u) K::guestVAToHost(kDev)[0x2A38u] = 2u;
    if (i == 6u) K::writeGuestBE32(kDev+44u,kBuf+252u);
    std::array<uint8_t,kDevBytes> device; std::array<uint8_t,kBufBytes> buffer;
    std::memcpy(device.data(),K::guestVAToHost(kDev),device.size());
    std::memcpy(buffer.data(),K::guestVAToHost(kBuf),buffer.size());
    check(!N::swapBlockLeaf(kDev,kBuf+4u,kFetch,1280u,720u) &&
      !std::memcmp(device.data(),K::guestVAToHost(kDev),device.size()) &&
      !std::memcmp(buffer.data(),K::guestVAToHost(kBuf),buffer.size()),
      "unsupported segment mode or allocation declines before mutation");
  }
  // Exercise the actual export with non-null inaccessible metadata, including
  // its periodic census. The original block must still be written exactly once
  // with every native gate configuration, including gate OFF.
  for (uint32_t protectedVA : {0x82000000u,kCell}) {
    fixture(0x9F130000u,0u,true);
    std::array<uint8_t,256> expected;
    {
      N::PresentGateOracle guard;
      swap(kBuf+4u,kFetch,0u,0u,0u,kFb);
      std::memcpy(expected.data(),K::guestVAToHost(kBuf+4u),expected.size());
      while (K::getVdSwapCount() % 30u != 29u)
        swap(kBuf+4u,kFetch,0u,0u,0u,kFb);
    }
    fixture(0x9F130000u,0u,true);
    std::array<uint8_t,kDevBytes> device;
    std::memcpy(device.data(),K::guestVAToHost(kDev),device.size());
    const uint64_t before = K::getVdSwapCount();
    DWORD oldProtection = 0, ignored = 0;
    uint8_t* page = K::guestVAToHost(protectedVA);
    const bool protectedPage = VirtualProtect(page,0x1000u,PAGE_NOACCESS,&oldProtection) != 0;
    std::printf("[SWAP_BLOCK_CONTROL] export metadata VA=%08X swap=%llu protected=%u\n",
      protectedVA,(unsigned long long)(before+1u),protectedPage ? 1u : 0u);
    std::fflush(stdout);
    const bool returned = protectedPage && swap(kBuf+4u,kFetch,0u,0u,0u,kFb) == 0u;
    const bool restored = !protectedPage || VirtualProtect(page,0x1000u,oldProtection,&ignored) != 0;
    if (!restored) {
      check(false,"restore protected export metadata page");
      return 1;
    }
    bool same = returned && K::getVdSwapCount() == before+1u &&
      !std::memcmp(expected.data(),K::guestVAToHost(kBuf+4u),expected.size()) &&
      !std::memcmp(device.data(),K::guestVAToHost(kDev),device.size());
    for (uint32_t i = 0; i < kBufBytes; ++i)
      if (i < 4u || i >= 260u) same &= K::guestVAToHost(kBuf)[i] == 0xEE;
    check(same,protectedVA == kCell ? "actual export safely falls back with PAGE_NOACCESS device cell at census" :
                                    "actual export safely falls back with PAGE_NOACCESS device root at census");
  }
  // A cached-readable device becomes inaccessible after the cache is warmed.
  // Admission must fail before Entry opens or changes the cursor/transport.
  fixture(0x9F130000u,0u,true);
  std::array<uint8_t,kDevBytes> inaccessibleDevice;
  std::array<uint8_t,kBufBytes> inaccessibleBuffer;
  std::memcpy(inaccessibleDevice.data(),K::guestVAToHost(kDev),inaccessibleDevice.size());
  std::memcpy(inaccessibleBuffer.data(),K::guestVAToHost(kBuf),inaccessibleBuffer.size());
  const bool warmed = K::hostRangeReadableCached(K::guestVAToHost(kDev),kDevBytes);
  DWORD oldProtection = 0, ignored = 0;
  uint8_t* devicePage = K::guestVAToHost(kDev+0x4000u);
  const bool protectedDevice = VirtualProtect(devicePage,0x1000u,PAGE_NOACCESS,&oldProtection) != 0;
  std::printf("[SWAP_BLOCK_CONTROL] warmed-cache device protected=%u warmed=%u\n",
    protectedDevice ? 1u : 0u,warmed ? 1u : 0u);
  std::fflush(stdout);
  const bool declined = protectedDevice && !N::swapBlockLeaf(kDev,kBuf+4u,kFetch,1280u,720u);
  const bool restored = !protectedDevice || VirtualProtect(devicePage,0x1000u,oldProtection,&ignored) != 0;
  if (!restored) {
    check(false,"restore protected device admission page");
    return 1;
  }
  check(warmed && declined && !std::memcmp(inaccessibleDevice.data(),K::guestVAToHost(kDev),inaccessibleDevice.size()) &&
        !std::memcmp(inaccessibleBuffer.data(),K::guestVAToHost(kBuf),inaccessibleBuffer.size()),
        "warmed-cache PAGE_NOACCESS device snapshot declines without mutation");
  auto engine = std::make_unique<G::Pm4Engine>();
  uint32_t badPayload[10] = {2u};
  const auto untouched = G::getXenosRegisters().read(0x4800u);
  check(!N::swapBlockApply(N::kRkSwap,badPayload,10u,*engine) &&
    !N::swapBlockApply(N::kRkSwap,badPayload,9u,*engine) &&
    !N::swapBlockApply(N::kRkSwap,nullptr,10u,*engine) &&
    !N::swapBlockApply(N::kRkEvent,badPayload,10u,*engine) &&
    engine->cpSwaps == 0u && G::getXenosRegisters().read(0x4800u) == untouched,
    "malformed native payload rejects before register or present effects");
  std::printf("[SWAP_BLOCK_TEST] %d passed, %d failed (offline export and pin transport, no live acceptance)\n",passed,failed);
  return failed ? 1 : 0;
}
