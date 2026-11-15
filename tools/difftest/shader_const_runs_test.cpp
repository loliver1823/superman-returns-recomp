// GpuCmdEmitShaderConstantRuns_821075E0: actual SDK bytes and record execution.
// The boundary cases enter 82107058 without a flush. External segment stress
// does not prove this helper's internal flush loop or whole-game acceptance.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "ppc/ppc_recomp_shared.h"
#include "guest/guest_harness.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/native_flush.hpp"
#include "kernel/stubs.hpp"
#include "kernel/ab_toggle.hpp"
#include "gfx/pm4_engine.hpp"
#include "gfx/xenos_state.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

extern "C" void __imp__sub_821075E0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820FC478(PPCContext& __restrict, uint8_t*);
namespace K = sr::kernel;
namespace N = sr::native::flush;
namespace {
constexpr uint32_t kDev = 0x70600000u, kBuf = 0x70640000u, kStack = 0x70580000u;
constexpr uint32_t kList = 0x70650000u, kFence = 0x70651000u;
constexpr uint32_t kBytes = 0x1000u, kReg = 0x2388u;
int passed = 0, failed = 0;
enum class Route { entry, direct, indirect, oracle };
Route route = Route::entry;
bool enabled(const char* name) {
  const char* value = std::getenv(name);
  return value && value[0] == '1';
}
bool literalOutput() {
  // The gate itself (default ON since the construns bake; =0 keeps the SDK body), not the variable's text.
  if (route == Route::oracle || !N::on() || !N::shaderConstRunsGate()) return true;
  for (const char* name : {"SR_NATIVE_FLUSH", "SR_NATIVE_SHADER_CONST_RUNS"}) {
    const int bit = sr::abt::reg(name);
    if (bit >= 0 && !sr::abt::on(bit)) return true;
  }
  return enabled("SR_NATIVE_FLUSH_PM4");
}
PPCContext context(uint64_t mask) {
  PPCContext ctx{};
  ctx.r1.u64 = kStack; ctx.r3.u64 = kDev; ctx.r4.u64 = mask;
  ctx.r13.u64 = kStack - 0x1000u;
#ifndef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL
  ctx.r2.u64 = 0x1234567800000002ull;
#endif
#ifndef PPC_CONFIG_NON_VOLATILE_AS_LOCAL
  std::memset(&ctx.r14, 0xA5, offsetof(PPCContext, r31) + sizeof(ctx.r31) - offsetof(PPCContext, r14));
  std::memset(&ctx.f14, 0x5A, offsetof(PPCContext, f31) + sizeof(ctx.f31) - offsetof(PPCContext, f14));
#endif
#ifndef PPC_CONFIG_CR_AS_LOCAL
  ctx.cr2 = {1, 0, 1, 0}; ctx.cr3 = {0, 1, 0, 1}; ctx.cr4 = {1, 1, 0, 0};
#endif
  return ctx;
}
bool preserved(const PPCContext& before, const PPCContext& after) {
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
bool emit(PPCContext& ctx, uint64_t& result) {
  uint8_t* base = K::getGuestBase();
  if (route == Route::entry) {
    N::Entry entry(ctx, base, kDev);
    const bool ok = entry.open() && entry.shaderConstantRuns(ctx.r4.u64, result);
    if (ok) entry.close(); else entry.abandon();
    return ok;
  }
  if (route == Route::oracle) {
    N::ShaderConstRunsOracle guard;
    sub_821075E0(ctx, base);
  } else if (route == Route::indirect) {
    ctx.ctr.u32 = 0x821075E0u;
    ctx.lr = 0;  // isolated guest-style indirect call, not a captured game callsite
    PPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);
  } else sub_821075E0(ctx, base);
  result = ctx.r3.u64;
  return true;
}
void check(bool ok, const char* name) {
  std::printf("%s %s\n", ok ? "  ok " : "FAIL:", name);
  ok ? ++passed : ++failed;
}
void fixture(uint32_t cursor, uint32_t limit) {
  std::memset(K::guestVAToHost(kDev), 0, 0x3600u);
  std::memset(K::guestVAToHost(kBuf), 0xEE, kBytes);
  for (uint32_t i = 0; i < 256u; ++i)
    K::writeGuestBE32(kDev + 0x27A0u + 4u * i, 0xCAFE0000u + i);
  // Broad synthetic masks overlap later device fields. Keep the real entry's
  // unsupported tile-auto-predication mode clear in both SDK/native fixtures.
  K::guestVAToHost(kDev)[0x2A38u] = 0;
  K::writeGuestBE32(kDev + 40u, cursor);
  K::writeGuestBE32(kDev + 44u, limit);
  K::writeGuestBE32(kDev + 48u, kBuf + kBytes - 4u);
  K::writeGuestBE32(kDev + 13508u, kBuf);
}
void seedRegisters() {
  auto& regs = sr::gfx::getXenosRegisters();
  for (uint32_t i = 0; i < 256u; ++i) regs.write(kReg + i, 0xBAD00000u + i);
  for (uint32_t reg : {0x2007u, 0x0A2Fu, 0x0A30u, 0x0A31u}) regs.write(reg, 0xFFFFFFFFu);
}
std::array<uint32_t, 260> registerSnapshot() {
  std::array<uint32_t, 260> state;
  auto& regs = sr::gfx::getXenosRegisters();
  for (uint32_t i = 0; i < 256u; ++i) state[i] = regs.read(kReg + i);
  state[256] = regs.read(0x2007u); state[257] = regs.read(0x0A2Fu);
  state[258] = regs.read(0x0A30u); state[259] = regs.read(0x0A31u);
  return state;
}
void compare(uint64_t mask, uint32_t offset, bool boundary) {
  const uint32_t cursor = kBuf + offset;
  // Boundary masks have one contiguous run starting at the MSB. Equality in
  // the SDK's strict fit test selects its chunk helper, which does not align.
  const uint32_t groups = uint32_t(__builtin_popcountll(mask));
  const uint32_t limit = boundary ? cursor + 52u + 16u * groups + 4u : kBuf + kBytes - 4u;
  fixture(cursor, limit);
  PPCContext oracle = context(mask);
  const PPCContext before = oracle;
  __imp__sub_821075E0(oracle, K::getGuestBase());
  std::array<uint8_t, 0x3600> expectedDevice;
  std::array<uint8_t, kBytes> expectedBuffer;
  std::memcpy(expectedDevice.data(), K::guestVAToHost(kDev), expectedDevice.size());
  std::memcpy(expectedBuffer.data(), K::guestVAToHost(kBuf), expectedBuffer.size());
  const uint32_t end = K::readGuestU32BE(kDev + 40u), bytes = end - cursor;
  seedRegisters();
  auto sdkEngine = std::make_unique<sr::gfx::Pm4Engine>();
  sdkEngine->cpMode = true; sdkEngine->submitDraws = false;
  sdkEngine->execute(expectedBuffer.data() + offset + 4u, bytes);
  const auto expectedRegisters = registerSnapshot();
  bool same = sdkEngine->lastConsumed == bytes && sdkEngine->cpWaits == 1u &&
              sdkEngine->cpWaitsBlocked == 0u;
  fixture(cursor, limit);
  PPCContext native = before;
  uint64_t result = 0;
  const bool emitted = emit(native, result);
  same &= emitted && preserved(before, oracle) && preserved(before, native);
  same &= result == oracle.r3.u64 &&
          !std::memcmp(expectedDevice.data(), K::guestVAToHost(kDev), expectedDevice.size());
  const bool literal = literalOutput();
  const char* reconstructedEnv = std::getenv("SR_NATIVE_FLUSH_CPPM4");
  const bool reconstructed = reconstructedEnv && reconstructedEnv[0] == '1';
  if (literal) {
    same &= !std::memcmp(expectedBuffer.data(), K::guestVAToHost(kBuf), expectedBuffer.size());
  } else {
    same &= K::readGuestU32BE(cursor + 8u) == N::kMarkerMagic &&
            K::readGuestU32BE(cursor) == 0xEEEEEEEEu &&
            K::readGuestU32BE(end + 4u) == 0xEEEEEEEEu;
  }
  seedRegisters();
  auto nativeEngine = std::make_unique<sr::gfx::Pm4Engine>();
  nativeEngine->cpMode = true; nativeEngine->submitDraws = false;
  if (literal) nativeEngine->execute(K::guestVAToHost(cursor + 4u), bytes);
  else N::cpExecute(*nativeEngine, K::guestVAToHost(cursor + 8u), bytes / 4u - 1u);
  same &= registerSnapshot() == expectedRegisters && nativeEngine->cpWaitsBlocked == 0u;
  // Record execution eliminates the fixed, already-satisfied register poll.
  same &= nativeEngine->cpWaits == ((literal || reconstructed) ? 1u : 0u);
  char name[160];
  std::snprintf(name, sizeof(name), "SDK mask=%016llX cursor%%8=%u %s: bytes/record, device, return, all registers",
                (unsigned long long)mask, offset, boundary ? "chunked equality" : "aligned fast");
  if (!same && failed < 8)
    std::printf("  mismatch emitted=%u r3=%llX/%llX cursor=%08X/%08X waits=%llu/%llu\n",
                emitted, (unsigned long long)result, (unsigned long long)oracle.r3.u64,
                K::readGuestU32BE(kDev + 40u), end, (unsigned long long)nativeEngine->cpWaits,
                (unsigned long long)sdkEngine->cpWaits);
  check(same, name);
}

// A real SDK recording pin captures closed segments without publishing a GPU
// ring. Its allocator and flush remain the SDK functions. A 216-byte allocation
// leaves 160 bytes before the fence tail, forcing repeated internal flushes.
// entryKick (the construns bake): dev+48 below the cursor, so the entry check (cursor > dev+48) flushes first.
void flushFixture(uint32_t offset, uint32_t firstGroups, bool entryKick = false) {
  const uint32_t cursor = kBuf + offset;
  std::memset(K::guestVAToHost(kDev), 0, 0x5000u);
  fixture(cursor, cursor + 52u + 16u * firstGroups + 4u);
  std::memset(K::guestVAToHost(kList), 0xEE, 0x800u);
  std::memset(K::guestVAToHost(kFence), 0, 0x40u);
  K::writeGuestBE32(kDev + 48u, K::readGuestU32BE(kDev + 44u));
  K::writeGuestBE32(kDev + 0x2A10u, kFence);
  K::guestVAToHost(kDev)[0x2A39u] = 2u;  // already in the SDK's no-HSIO mode
  K::writeGuestBE32(kDev + 12944u, kBuf + kBytes - 4u);
  K::writeGuestBE32(kDev + 12948u, 1u);  // recording pin is one generation ahead
  K::writeGuestBE32(kDev + 13008u, kList);
  K::writeGuestBE32(kDev + 13012u, kList + 0x800u);
  K::writeGuestBE32(kDev + 13484u, kBuf);
  K::writeGuestBE32(kDev + 13488u, kBuf + kBytes);
  K::writeGuestBE32(kDev + 13492u, 216u);
  K::writeGuestBE32(kDev + 13504u, kBuf);
  K::writeGuestBE32(kDev + 13508u, cursor + 4u);
  K::writeGuestBE32(kDev + 20416u, 1u);  // no swap-fence throttle in this fixture
  if (entryKick) {
    // The segment holds earlier work (7 type-2 NOPs) so the entry check's flush closes a non-empty segment (the SDK
    // lists no empty one): the cursor (its last dword) 28 bytes past the segment start, dev+48 just below it.
    for (uint32_t i = 0; i < 7u; ++i) K::writeGuestBE32(cursor + 4u + 4u * i, 0x80000000u);
    K::writeGuestBE32(kDev + 40u, cursor + 28u);
    K::writeGuestBE32(kDev + 48u, cursor + 24u);
  }
}
struct Segment { uint32_t start, bytes; };
std::vector<Segment> segments(uint32_t offset) {
  std::vector<Segment> result;
  const uint32_t listEnd = K::readGuestU32BE(kDev + 13008u);
  if (listEnd < kList || listEnd > kList + 0x800u || (listEnd - kList) % 8u) return result;
  for (uint32_t p = kList; p < listEnd; p += 8u) {
    const uint32_t header = K::readGuestU32BE(p);
    const uint32_t start = (K::readGuestU32BE(p + 4u) & 0x1FFFFFFFu) | (kBuf & 0xE0000000u);
    const uint32_t bytes = 4u * (header & 0x01FFFFFFu);
    if ((header & 0xFE000000u) != 0x82000000u || start < kBuf + offset + 4u ||
        !bytes || uint64_t(start) + bytes > kBuf + kBytes) return {};
    result.push_back({start, bytes});
  }
  const uint32_t start = K::readGuestU32BE(kDev + 13508u);
  const uint32_t end = K::readGuestU32BE(kDev + 40u) + 4u;
  if (start < kBuf + offset + 4u || end <= start || end > kBuf + kBytes) return {};
  result.push_back({start, end - start});
  return result;
}
void compareFlushes(uint64_t mask, uint32_t offset, uint32_t firstGroups, uint32_t minimumFlushes,
                    bool entryKick = false) {
  flushFixture(offset, firstGroups, entryKick);
  PPCContext oracle = context(mask);
  const PPCContext before = oracle;
  __imp__sub_821075E0(oracle, K::getGuestBase());
  const auto expectedSegments = segments(offset);
  std::array<uint8_t, 0x5000u> expectedDevice;
  std::array<uint8_t, kBytes> expectedBuffer;
  std::array<uint8_t, 0x800u> expectedList;
  std::memcpy(expectedDevice.data(), K::guestVAToHost(kDev), expectedDevice.size());
  std::memcpy(expectedBuffer.data(), K::guestVAToHost(kBuf), expectedBuffer.size());
  std::memcpy(expectedList.data(), K::guestVAToHost(kList), expectedList.size());
  bool same = expectedSegments.size() > minimumFlushes;
  seedRegisters();
  auto sdkEngine = std::make_unique<sr::gfx::Pm4Engine>();
  sdkEngine->cpMode = true; sdkEngine->submitDraws = false;
  for (const Segment& s : expectedSegments) {
    sdkEngine->execute(expectedBuffer.data() + s.start - kBuf, s.bytes);
    same &= sdkEngine->lastConsumed == s.bytes;
  }
  const auto expectedRegisters = registerSnapshot();
  same &= sdkEngine->cpWaits == 1u && sdkEngine->cpWaitsBlocked == 0u;
  flushFixture(offset, firstGroups, entryKick);
  PPCContext native = before;
  uint64_t result = 0;
  const bool emitted = emit(native, result);
  same &= preserved(before, oracle) && preserved(before, native);
  const auto actualSegments = segments(offset);
  same &= emitted && result == oracle.r3.u64 && actualSegments.size() == expectedSegments.size() &&
          !std::memcmp(expectedDevice.data(), K::guestVAToHost(kDev), expectedDevice.size()) &&
          !std::memcmp(expectedList.data(), K::guestVAToHost(kList), expectedList.size());
  const bool literal = literalOutput();
  const char* reconstructedEnv = std::getenv("SR_NATIVE_FLUSH_CPPM4");
  const bool reconstructed = reconstructedEnv && reconstructedEnv[0] == '1';
  if (literal) same &= !std::memcmp(expectedBuffer.data(), K::guestVAToHost(kBuf), expectedBuffer.size());
  // Gaps between closed segments are outside the command stream and stay intact.
  for (uint32_t i = 0; i < kBytes; ++i) {
    bool inSegment = false;
    for (const Segment& s : expectedSegments)
      inSegment |= kBuf + i >= s.start && kBuf + i < s.start + s.bytes;
    if (!inSegment) same &= K::guestVAToHost(kBuf)[i] == expectedBuffer[i];
  }
  seedRegisters();
  auto nativeEngine = std::make_unique<sr::gfx::Pm4Engine>();
  nativeEngine->cpMode = true; nativeEngine->submitDraws = false;
  for (size_t i = 0; i < actualSegments.size(); ++i) {
    const Segment& s = actualSegments[i];
    // entryKick: the segment the entry check closed holds only the SDK flush's own bytes (no native record yet)
    if (literal || (entryKick && i == 0)) nativeEngine->execute(K::guestVAToHost(s.start), s.bytes);
    else {
      same &= s.bytes >= 16u && K::readGuestU32BE(s.start + 4u) == N::kMarkerMagic;
      if (same) N::cpExecute(*nativeEngine, K::guestVAToHost(s.start + 4u), s.bytes / 4u - 1u);
    }
  }
  same &= registerSnapshot() == expectedRegisters && nativeEngine->cpWaitsBlocked == 0u &&
          nativeEngine->cpWaits == ((literal || reconstructed) ? 1u : 0u);
  char name[160];
  std::snprintf(name, sizeof(name), "SDK %s flush mask=%016llX cursor%%8=%u firstGroups=%u flushes=%zu: segments, device, return, all registers",
                entryKick ? "entry-check" : "internal", (unsigned long long)mask, offset, firstGroups,
                expectedSegments.empty() ? 0u : expectedSegments.size() - 1u);
  if (!same) std::printf("  flush mismatch emitted=%u segments=%zu/%zu r3=%llX/%llX\n", emitted,
                         actualSegments.size(), expectedSegments.size(), (unsigned long long)result,
                         (unsigned long long)oracle.r3.u64);
  check(same, name);
}
void tessellatedFixture(uint32_t offset, uint32_t dirty) {
  std::memset(K::guestVAToHost(kDev), 0, 0x5000u);
  fixture(kBuf + offset, kBuf + kBytes - 4u);
  const uint64_t mask = uint64_t(dirty) << 49u;
  K::writeGuestBE32(kDev + 32u, uint32_t(mask >> 32));
  K::writeGuestBE32(kDev + 36u, uint32_t(mask));
  // The isolated SDK caller uses zeroed primitive tables and no renderer.
  // Its authored dirty-state logic and actual nested SDK calls still execute.
}
void seedAllRegisters() {
  auto& regs = sr::gfx::getXenosRegisters();
  for (uint32_t i = 0; i < sr::gfx::XenosRegisterFile::kNumRegisters; ++i) regs.write(i, 0u);
  seedRegisters();
}
std::vector<uint32_t> allRegisters() {
  auto& regs = sr::gfx::getXenosRegisters();
  std::vector<uint32_t> values(sr::gfx::XenosRegisterFile::kNumRegisters);
  for (uint32_t i = 0; i < values.size(); ++i) values[i] = regs.read(i);
  return values;
}
void compareTessellated(uint32_t dirty, uint32_t offset) {
  tessellatedFixture(offset, dirty);
  PPCContext oracle = context(4u);
  oracle.r5.u64 = 7u; oracle.r6.u64 = 9u;
  const PPCContext before = oracle;
  {
    N::ShaderConstRunsOracle guard;
    __imp__sub_820FC478(oracle, K::getGuestBase());
  }
  const uint32_t end = K::readGuestU32BE(kDev + 40u);
  const uint32_t bytes = end - kBuf - offset;
  std::array<uint8_t, 0x5000u> expectedDevice;
  std::array<uint8_t, kBytes> expectedBuffer;
  std::memcpy(expectedDevice.data(), K::guestVAToHost(kDev), expectedDevice.size());
  std::memcpy(expectedBuffer.data(), K::guestVAToHost(kBuf), expectedBuffer.size());
  bool same = bytes > 52u && end < kBuf + kBytes;
  for (uint32_t p = offset + 4u; p <= end - kBuf; p += 4u)
    same &= K::readGuestU32BE(kBuf + p) != N::kMarkerMagic;
  seedAllRegisters();
  auto sdkEngine = std::make_unique<sr::gfx::Pm4Engine>();
  sdkEngine->cpMode = true; sdkEngine->submitDraws = false;
  sdkEngine->execute(expectedBuffer.data() + offset + 4u, bytes);
  const auto expectedRegisters = allRegisters();
  same &= sdkEngine->lastConsumed == bytes && sdkEngine->cpWaits == 1u && sdkEngine->cpWaitsBlocked == 0u;
  tessellatedFixture(offset, dirty);
  PPCContext native = before;
  if (route == Route::oracle) {
    N::ShaderConstRunsOracle guard;
    sub_820FC478(native, K::getGuestBase());
  } else sub_820FC478(native, K::getGuestBase());
  same &= preserved(before, oracle) && preserved(before, native) && native.r3.u64 == oracle.r3.u64 &&
          !std::memcmp(expectedDevice.data(), K::guestVAToHost(kDev), expectedDevice.size());
  const bool literal = literalOutput();
  uint32_t markers = 0;
  for (uint32_t p = kBuf + offset + 4u; p <= end; p += 4u)
    markers += K::readGuestU32BE(p) == N::kMarkerMagic;
  same &= markers == (literal ? 0u : 1u);
  if (literal) same &= !std::memcmp(expectedBuffer.data(), K::guestVAToHost(kBuf), expectedBuffer.size());
  seedAllRegisters();
  auto nativeEngine = std::make_unique<sr::gfx::Pm4Engine>();
  nativeEngine->cpMode = true; nativeEngine->submitDraws = false;
  nativeEngine->execute(K::guestVAToHost(kBuf + offset + 4u), bytes);
  same &= nativeEngine->lastConsumed == bytes && allRegisters() == expectedRegisters &&
          nativeEngine->cpWaitsBlocked == 0u && nativeEngine->cpWaits == ((literal || enabled("SR_NATIVE_FLUSH_CPPM4")) ? 1u : 0u);
  char name[128];
  std::snprintf(name, sizeof(name), "actual tessellated SDK caller dirty=%02X cursor%%8=%u: scoped oracle, device, ABI, full register execution", dirty, offset);
  if (!same) std::printf("  tess mismatch markers=%u bytes=%u consumed=%zu waits=%llu/%llu\n", markers, bytes,
                         nativeEngine->lastConsumed, (unsigned long long)nativeEngine->cpWaits,
                         (unsigned long long)sdkEngine->cpWaits);
  check(same, name);
}

// ---- the construns bake (native/construns-bake, 2026-10-14): --shader-const-runs-test bake ----
// Every caller of 821075E0 in this binary (820FB6E8 / 820FBBF8 / 820FC000 / 820FC478; IDA: `bl 0x821075e0` after
// `rldicr r4,r27,9,5`, behind `and r11,r27,63<<49 ; beq`) passes the device's six dirty bits as the mask's top six
// bits: at most six groups, never zero. The SDK flushes at entry only when cursor > dev+48, and GpuCmdBufBeginNewSegment
// 820FCF90 sets dev+48 = dev+44 - 160 on both of its branches; so cursor == dev+48 is the worst entry a caller can make.
uint64_t callerMask(uint32_t dirty) { return uint64_t(dirty & 63u) << 58u; }
void worstCaseFixture(uint32_t offset, uint32_t margin) {
  const uint32_t cursor = kBuf + offset;
  fixture(cursor, cursor + margin);       // dev+44 = cursor + margin
  K::writeGuestBE32(kDev + 48u, cursor);  // dev+48 = cursor: the entry check passes without a flush
}
struct DomainResult { bool sdkChunked = false, nativeRan = false, nativeChunked = false, sameResult = false, margin = false; };
// The SDK body's r3 stays 3 unless its chunk helper 82107058 ran (whose return replaces it). Gate ON: the native leaf
// once more on the same fixture - taken (sdk=), chunked= moved or not, its r3, and marginBad= counting margin != 160.
DomainResult domainCase(uint32_t dirty, uint32_t offset, uint32_t margin) {
  DomainResult r;
  worstCaseFixture(offset, margin);
  PPCContext oracle = context(callerMask(dirty));
  __imp__sub_821075E0(oracle, K::getGuestBase());
  r.sdkChunked = oracle.r3.u64 != 3u;
  if (!N::shaderConstRunsGate()) return r;
  worstCaseFixture(offset, margin);
  PPCContext native = context(callerMask(dirty));
  const auto before = N::shaderConstRunsCounters();
  sub_821075E0(native, K::getGuestBase());
  const auto after = N::shaderConstRunsCounters();
  r.nativeRan = after.sdk == before.sdk + 1u;
  r.nativeChunked = after.chunked != before.chunked;
  r.sameResult = native.r3.u64 == oracle.r3.u64;
  r.margin = after.marginBad == before.marginBad + (margin != 160u ? 1u : 0u);
  return r;
}
void bakeDomain() {
  const bool gate = N::shaderConstRunsGate();
  uint32_t cases = 0, sdkChunks = 0, ran = 0, nativeChunks = 0, same = 0, margins = 0;
  for (uint32_t offset : {0u, 4u})
    for (uint32_t dirty = 1u; dirty < 64u; ++dirty) {
      const DomainResult r = domainCase(dirty, offset, 160u);
      ++cases; sdkChunks += r.sdkChunked; ran += r.nativeRan; nativeChunks += r.nativeChunked;
      same += r.sameResult; margins += r.margin;
    }
  std::printf("[SHADER_CONST_BAKE] domain cases=%u sdkChunked=%u nativeRan=%u nativeChunked=%u sameR3=%u marginOk=%u\n",
              cases, sdkChunks, ran, nativeChunks, same, margins);
  check(cases == 126u && sdkChunks == 0u,
        "caller domain: 63 six-bit masks x cursor%8 0/4 at cursor == dev+48 == dev+44 - 160, the SDK never chunks");
  if (gate)
    check(ran == cases && nativeChunks == 0u && same == cases && margins == cases,
          "caller domain: the native leaf takes all 126, never chunks, returns the SDK's r3, marginBad stays 0");
  // The control: 8 bytes less, the six-group mask meets the SDK's strict fit test with equality (its chunk helper).
  const DomainResult c0 = domainCase(63u, 0u, 152u), c4 = domainCase(63u, 4u, 152u);
  check(c0.sdkChunked && c4.sdkChunked, "caller domain control: at dev+44 - dev+48 = 152 the six-group mask chunks in the SDK");
  if (gate)
    check(c0.nativeRan && c4.nativeRan && c0.nativeChunked && c4.nativeChunked && c0.sameResult && c4.sameResult &&
          c0.margin && c4.margin,
          "caller domain control: the native leaf chunks with the SDK, returns its r3 and counts marginBad");
}
void bakeCounters() {
  const bool gate = N::shaderConstRunsGate();
  uint8_t* base = K::getGuestBase();
  // A direct caller-domain call: one native SDK-leaf call (gate ON) or the SDK body (gate OFF).
  fixture(kBuf, kBuf + kBytes - 4u);
  PPCContext direct = context(callerMask(63u));
  auto b = N::shaderConstRunsCounters();
  sub_821075E0(direct, base);
  auto a = N::shaderConstRunsCounters();
  check(gate ? (a.sdk == b.sdk + 1u && a.calls == b.calls + 1u && a.tessSdk == b.tessSdk)
             : (a.sdk == b.sdk && a.calls == b.calls && a.tessSdk == b.tessSdk),
        "counters: a direct call is one sdk= (gate ON) or none (=0: the SDK body), never tessSdk=");
  // The CP renders the sync item back to PM4 only under CPPM4 (cpPm4=, counted once the PM4 executed).
  if (gate && !literalOutput()) {
    const uint32_t end = K::readGuestU32BE(kDev + 40u);
    auto engine = std::make_unique<sr::gfx::Pm4Engine>();
    engine->cpMode = true; engine->submitDraws = false;
    seedRegisters();
    b = N::shaderConstRunsCounters();
    N::cpExecute(*engine, K::guestVAToHost(kBuf + 8u), (end - kBuf) / 4u - 1u);
    a = N::shaderConstRunsCounters();
    const bool cppm4 = enabled("SR_NATIVE_FLUSH_CPPM4");
    check(!engine->nativeRecordFailed && a.cpPm4 == b.cpPm4 + (cppm4 ? 1u : 0u),
          cppm4 ? "counters: the CP renders the sync record back to PM4 and counts it (cpPm4=)"
                : "counters: record execution is no cpPm4=");
  }
  // The leaf's declines by reason, each before guest state changes (the leaf alone: an empty mask must never reach the
  // SDK body, whose loop has no exit for it).
  if (gate) {
    fixture(kBuf, kBuf + kBytes - 4u);
    b = N::shaderConstRunsCounters();
    bool none = true;
    {
      N::ShaderConstRunsOracle guard;
      PPCContext c = context(callerMask(63u));
      none &= !N::shaderConstantRunsLeaf(c, base);
    }
    PPCContext m = context(0u);
    none &= !N::shaderConstantRunsLeaf(m, base);
    PPCContext s = context(callerMask(63u));
    s.r3.u64 = 0u;  // no device
    none &= !N::shaderConstantRunsLeaf(s, base);
    a = N::shaderConstRunsCounters();
    check(none && a.declOracle == b.declOracle + 1u && a.declMask == b.declMask + 1u && a.declSpan == b.declSpan + 1u &&
              a.sdk == b.sdk && K::readGuestU32BE(kDev + 40u) == kBuf && K::readGuestU32BE(kBuf + 4u) == 0xEEEEEEEEu,
          "counters: the leaf's declines count by reason (oracle, mask, span) before any guest write");
    if (sr::abt::active()) {  // SR_AB_TOGGLE=SR_NATIVE_SHADER_CONST_RUNS: the A/B's off window
      const uint32_t old = sr::abt::g_mask.load(std::memory_order_relaxed);
      sr::abt::g_mask.store(0, std::memory_order_relaxed);
      b = N::shaderConstRunsCounters();
      PPCContext o = context(callerMask(63u));
      const bool declined = !N::shaderConstantRunsLeaf(o, base);
      a = N::shaderConstRunsCounters();
      sr::abt::g_mask.store(old, std::memory_order_relaxed);
      check(declined && a.declOff == b.declOff + 1u && a.sdk == b.sdk,
            "counters: the A/B's off window declines as off");
    }
  }
  // A tessellated draw: 820FC478's observe override counts it, and the leaf counts the SDK call made inside it.
  tessellatedFixture(0u, 63u);
  PPCContext t = context(4u);
  t.r5.u64 = 7u; t.r6.u64 = 9u;
  b = N::shaderConstRunsCounters();
  sub_820FC478(t, base);
  a = N::shaderConstRunsCounters();
  check(a.tessDraws == b.tessDraws + 1u && a.tessSdk == b.tessSdk + (gate ? 1u : 0u) &&
            a.sdk == b.sdk + (gate ? 1u : 0u),
        "counters: a tessellated draw is one tessDraws= and its SDK call one tessSdk= (gate ON)");
  // The entry check's segment switch (cursor > dev+48): the SDK and the native leaf flush at entry the same way.
  for (uint32_t offset : {0u, 4u}) {
    b = N::shaderConstRunsCounters();
    compareFlushes(callerMask(63u), offset, 6u, 1u, true);
    a = N::shaderConstRunsCounters();
    if (gate)
      check(a.kick == b.kick + 1u && a.chunked == b.chunked && a.marginBad == b.marginBad && a.sdk == b.sdk + 1u,
            offset ? "counters: an entry-check flush is one kick= (cursor%8=4), no chunk, the new segment's margin 160"
                   : "counters: an entry-check flush is one kick= (cursor%8=0), no chunk, the new segment's margin 160");
  }
}
int runBake() {
  const char* env = std::getenv("SR_NATIVE_SHADER_CONST_RUNS");
  const bool gate = N::shaderConstRunsGate();
  std::printf("[SHADER_CONST_BAKE] env=%s gate=%d cppm4=%d ab=%d\n", env ? env : "<unset>", gate ? 1 : 0,
              enabled("SR_NATIVE_FLUSH_CPPM4") ? 1 : 0, sr::abt::active() ? 1 : 0);
  check(gate == !(env && env[0] == '0'), "the gate is ON unless SR_NATIVE_SHADER_CONST_RUNS=0 (the bake's default)");
  if (sr::abt::active()) {  // the A/B mode: every window ON (the seeded first window is not under test) but the one
    (void)sr::abt::reg("SR_NATIVE_SHADER_CONST_RUNS");  // check that closes it (bakeCounters); reg() seeds the mask once
    sr::abt::g_mask.store(UINT32_MAX, std::memory_order_relaxed);
  }
  bakeDomain();
  bakeCounters();
  std::printf("[SHADER_CONST_RUNS_TEST] %d passed, %d failed (bake route; whole-game acceptance remains separate)\n",
              passed, failed);
  return failed ? 1 : 0;
}
}

int runShaderConstRunsTest(const char* requested) {
  K::registerStubs();
  if (!std::strcmp(requested, "lock-negative")) {
    if (!N::on() || N::segmentlessOn() || !enabled("SR_NATIVE_SHADER_CONST_RUNS") || sr::abt::active()) return 1;
    fixture(kBuf, kBuf + kBytes - 4u);
    PPCContext nested = context(0xFC00000000000000ull);
    N::Entry entry(nested, K::getGuestBase(), kDev);
    if (!entry.open()) return 1;
    std::printf("[SHADER_CONST_LOCK_NEGATIVE] outer Entry acquired; entering unscoped native call\n");
    std::fflush(stdout);
    // Deliberately reproduce the rejected lock ordering in an isolated process.
    // The runner must terminate this process after its bounded timeout.
    sub_821075E0(nested, K::getGuestBase());
    std::printf("FAIL: unscoped nested call unexpectedly returned\n");
    return 1;
  }
  if (!std::strcmp(requested, "bake")) {  // the construns bake: default, caller domain, counters (direct route)
    route = Route::direct;
    if (!N::on() || N::segmentlessOn()) {
      std::printf("FAIL: the bake route needs FLUSH=1/CHECK=0 and SEGMENTLESS=0\n");
      return 1;
    }
    return runBake();
  }
  if (!std::strcmp(requested, "entry")) route = Route::entry;
  else if (!std::strcmp(requested, "direct")) route = Route::direct;
  else if (!std::strcmp(requested, "indirect")) route = Route::indirect;
  else if (!std::strcmp(requested, "indirect-off")) {
    route = Route::indirect;
    if (!sr::abt::active()) { std::printf("FAIL: indirect-off needs SR_AB_TOGGLE\n"); return 1; }
    sr::abt::g_mask.store(0, std::memory_order_relaxed);
  }
  else if (!std::strcmp(requested, "oracle")) route = Route::oracle;
  else { std::printf("FAIL: unknown shader route %s\n", requested); return 1; }
  std::printf("[SHADER_CONST_RUNS_TEST] route=%s actual SDK generation and native record execution\n", requested);
  if ((route == Route::entry && !N::on()) || N::segmentlessOn()) {
    std::printf("FAIL: Entry needs FLUSH=1/CHECK=0; all routes need SEGMENTLESS=0\n");
    return 1;
  }
  if (route == Route::indirect) {
    sr::guest::initializeDispatchTable();
    check(PPC_LOOKUP_FUNC(K::getGuestBase(), 0x821075E0u) == &sub_821075E0 &&
          PPC_LOOKUP_FUNC(K::getGuestBase(), 0x821075E0u) != &__imp__sub_821075E0,
          "production dispatch mapping resolves the strong SDK replacement");
    if (failed) return 1;
  }
  for (uint32_t offset : {0u, 4u}) {
    for (uint32_t bit = 0; bit < 64u; ++bit) compare(1ull << bit, offset, false);
    for (uint32_t bit = 0; bit < 63u; ++bit) compare(3ull << bit, offset, false);
    for (uint64_t mask : {0xFFFFFFFFFFFFFFFFull, 0xFC00000000000000ull,
                          0xAAAAAAAAAAAAAAAAull, 0x5555555555555555ull,
                          0x8000000000000001ull}) compare(mask, offset, false);
    for (uint32_t groups : {1u, 2u, 6u, 32u, 64u})
      compare(groups == 64u ? UINT64_MAX : UINT64_MAX << (64u - groups), offset, true);
    compareFlushes(0xFC00000000000000ull, offset, 0u, 1u);
    compareFlushes(0xFFFFFFFFFFFFFFFFull, offset, 1u, 2u);
    compareFlushes(0x800000000000FFFFull, offset, 1u, 1u);
    for (uint32_t dirty : {1u, 2u, 4u, 8u, 16u, 32u, 63u}) compareTessellated(dirty, offset);
  }
  if (N::on()) {
    fixture(kBuf, kBuf + kBytes - 4u);
    PPCContext expected = context(0xFC00000000000000ull);
    __imp__sub_821075E0(expected, K::getGuestBase());
    std::array<uint8_t, kBytes> expectedBuffer;
    std::memcpy(expectedBuffer.data(), K::guestVAToHost(kBuf), expectedBuffer.size());
    const uint32_t expectedCursor = K::readGuestU32BE(kDev + 40u);
    fixture(kBuf, kBuf + kBytes - 4u);
    PPCContext nested = context(0xFC00000000000000ull);
    const uint32_t oldMask = sr::abt::g_mask.load(std::memory_order_relaxed);
    bool completed = false;
    {
      N::Entry entry(nested, K::getGuestBase(), kDev);
      if (entry.open()) {
        // The native gate is now ON while the outer Entry holds non-recursive
        // g_mu. Removing the SDK guard makes this call deadlock, caught by the
        // runner's timeout; an SDK call must return without a nested Entry.
        sr::abt::g_mask.store(UINT32_MAX, std::memory_order_relaxed);
        completed = entry.guest(sub_821075E0, kDev, 0xFC00000000000000ull);
        entry.close();
      }
    }
    sr::abt::g_mask.store(oldMask, std::memory_order_relaxed);
    check(completed && nested.r3.u64 == expected.r3.u64 && K::readGuestU32BE(kDev + 40u) == expectedCursor &&
          !std::memcmp(expectedBuffer.data(), K::guestVAToHost(kBuf), expectedBuffer.size()),
          "held Entry with A/B gate ON completes exact SDK fallback without recursive lock");
  }
  fixture(kBuf, kBuf + kBytes - 4u);
  PPCContext context{}; context.r1.u64 = kStack; context.r3.u64 = kDev;
  uint64_t result = 0xDEADBEEF;
  const PPCContext emptyBefore = context;
  check(!N::shaderConstantRunsLeaf(context, K::getGuestBase()) &&
        !std::memcmp(&context, &emptyBefore, sizeof(context)) &&
        K::readGuestU32BE(kDev + 40u) == kBuf && K::readGuestU32BE(kBuf + 4u) == 0xEEEEEEEEu,
        "shared leaf declines an empty mask before guest state changes");
  if (N::on()) {
    N::Entry entry(context, K::getGuestBase(), kDev);
    check(entry.open() && !entry.shaderConstantRuns(0, result) && result == 0xDEADBEEF &&
          K::readGuestU32BE(kDev + 40u) == kBuf && K::readGuestU32BE(kBuf + 4u) == 0xEEEEEEEEu,
          "empty mask declines before cursor, buffer or result mutation");
    entry.abandon();
  }
  auto engine = std::make_unique<sr::gfx::Pm4Engine>();
  const auto handler = N::opaqueHandler(N::kRkShaderConstSync);
  const uint32_t payload = 0xABCDEF01;
  seedRegisters(); const auto before = registerSnapshot();
  if (handler)
    check(!handler(N::kRkPresentGate, &payload, 1, *engine) &&
          !handler(N::kRkShaderConstSync, nullptr, 1, *engine) &&
          !handler(N::kRkShaderConstSync, &payload, 0, *engine) && registerSnapshot() == before,
          "sync handler rejects malformed records before register mutation");
  else check(literalOutput(), "SDK-only route leaves native sync handler unregistered");
  std::printf("[SHADER_CONST_RUNS_TEST] %d passed, %d failed (whole-game acceptance remains separate)\n", passed, failed);
  return failed ? 1 : 0;
}
