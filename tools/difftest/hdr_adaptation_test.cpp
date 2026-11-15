// Test the real SDK wrapper using a privately supplied 120-byte shader ucode
// file. No game code/assets are embedded in this source or the public package.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/guest_context.hpp"
#include "kernel/hdr_adaptation.hpp"
#include "kernel/highfps.hpp"
#include "gfx/shader_cache.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <windows.h>

PPC_EXTERN_FUNC(sub_820F4A30);
namespace K = sr::kernel;
namespace H = sr::kernel::hdr;
namespace {
constexpr uint32_t dev = 0x80600000u, input = 0x70610000u, object = 0x70620000u;
constexpr uint32_t physical = 0x70630000u, manager = 0x70640000u, stack = 0x70650000u;
constexpr uint32_t effect = 0x70660000u, block = 0x70661000u, records = 0x70662000u, shaderRef = 0x70663000u;
int passed = 0, failed = 0;
void check(bool good, const char* message) {
  std::printf("%s %s\n", good ? "  ok" : "FAIL", message);
  good ? ++passed : ++failed;
}
void word(uint32_t va, uint32_t value) { K::writeGuestBE32(va, value); }
void number(uint32_t va, float value) {
  uint32_t bits; std::memcpy(&bits, &value, 4); word(va, bits);
}
float number(uint32_t va) {
  const uint32_t bits = K::readGuestU32BE(va);
  float value; std::memcpy(&value, &bits, 4); return value;
}
void dt(float seconds) { number(manager + 0x84u, seconds); }
PPCContext sdkUpload(uint32_t source = input, uint32_t start = 0, uint32_t count = 1) {
  PPCContext ctx{};
  ctx.r1.u64 = stack; ctx.r3.u64 = dev; ctx.r4.u64 = start;
  ctx.r5.u64 = source; ctx.r6.u64 = count; ctx.r7.u64 = 0x8100000000000000ull;
  sub_820F4A30(ctx, K::getGuestBase());
  return ctx;
}
PPCContext upload(uint32_t source = input, uint32_t start = 0,
                  uint32_t count = 1, bool compiled = true) {
  H::CompiledEffectScope scope(K::getGuestBase(), compiled ? effect : 0, block);
  return sdkUpload(source, start, count);
}
}  // namespace

int runHdrAdaptationTest(int argc, char** argv) {
  if (argc != 3) { std::fprintf(stderr, "Supply the private 120-byte adaptation ucode file\n"); return 2; }
  std::array<uint8_t, 120> code;
  std::ifstream file(argv[2], std::ios::binary);
  file.read(reinterpret_cast<char*>(code.data()), code.size());
  if (file.gcount() != code.size() || file.peek() != EOF ||
      sr::gfx::ShaderCache::hashUcode(code.data(), code.size()) != 0x040D6633B0AA5D6Dull) {
    std::fprintf(stderr, "Private shader identity/size mismatch\n"); return 2;
  }
  const char* gate = std::getenv("SR_HDR_ADAPT_DT");
  const bool enabled = !gate || gate[0] != '0';   // X2 bake (2026-10-18): default ON, =0 = the A/B arm
  // G3 (step 6): the metadata reads' copy - direct by default. The test's EXPECTATION comes from its own variable, so
  // a run with only SR_HDR_DIRECT_READ=0 (the old ReadProcessMemory body) FAILS the G3 checks (the failing-first run);
  // SR_HDR_TEST_EXPECT_OS=1 alongside it verifies the =0 arm.
  const char* expectOs = std::getenv("SR_HDR_TEST_EXPECT_OS");
  const bool expectDirect = !(expectOs && expectOs[0] == '1');
  _putenv_s("SR_PORT_FEATURES", "1");
  _putenv_s("SR_HIGHFPS", "120");
  check(K::highfps::liveConfig().mode != K::highfps::Mode::Off, "live high-frame-rate mode enabled");
  word(0x8296D8DCu, manager); word(manager + 0x80u, 17);
  word(dev + 0x3080u, object); word(object + 0x18u, physical);
  word(object + 0x28u, 0x102a1100u); word(object + 0x2cu, 0x144);
  word(object + 0x30u, 0xb8); word(object + 0x40u, 0x124);
  word(object + 0x28u + 0x124u, 0x40); word(object + 0x28u + 0x128u, 120);
  std::memcpy(K::guestVAToHost(physical + 0x40u), code.data(), code.size());
  word(effect + 8u, dev); word(block, input); word(block + 8u, records);
  word(block + 84u, 2); word(block + 88u, 0x00010000u);
  word(records + 26u, shaderRef); word(shaderRef + 4u, object); word(records + 32u, input);
  number(input + 4, 7); number(input + 8, 8); number(input + 12, 9);
  std::memset(K::guestVAToHost(input + 16), 0x55, 16);
  float numeric;
  check(H::scaledTimeScale(.25f, 1.0/120, numeric) && numeric == .0625f,
        "independent 120 Hz value is one quarter of the authored quarter");
  check(H::scaledTimeScale(.3f, 1.0/60, numeric) && numeric == .15f,
        "independent 60 Hz value is half the authored scale");
  constexpr double coefficient = .8743894696235657;
  const double endpoint = 2 + 30 * std::exp2(-coefficient * .25 * 30);
  for (int hz : {30,60,120,144}) {
    double history = 32;
    for (int frame = 0; frame < hz; ++frame) {
      H::scaledTimeScale(.25f, 1.0/hz, numeric);
      history += (2 - history) * -std::expm1(-std::log(2.0) * coefficient * numeric);
    }
    check(std::abs(history - endpoint) < 1e-6, "C++ policy reaches independent analytic one-second endpoint");
  }

  for (float authored : {0.24749997f, 0.25f, 0.30000001f}) {
    number(input, authored);
    for (float seconds : {1.f/30, 1.f/60, 1.f/120, 0.f, 1.f}) {
      dt(seconds);
      const auto reference = upload(); // Same SDK path, outside the HDR tick scope.
      H::RenderTickScope scope(K::getGuestBase());
      const auto actual = upload();
      float expected = authored;
      if (enabled) H::scaledTimeScale(authored, seconds, expected);
      check(number(dev + 0x1700) == expected && number(input) == authored &&
            number(dev + 0x1704) == 7 && number(dev + 0x1708) == 8 && number(dev + 0x170c) == 9,
            "rate correction preserves authored source and yzw");
      check(actual.r3.u64 == reference.r3.u64 && actual.r9.u64 == reference.r9.u64 &&
            actual.r10.u64 == reference.r10.u64 && actual.r11.u64 == reference.r11.u64 &&
            K::readGuestU32BE(dev + 8) == 0x81000000u,
            "SDK result registers and dirty mask preserved");
      upload();
      check(number(dev + 0x1700) == expected, "repeated upload scales freshly copied authored input once");
    }
  }
  number(input, 0.25f); dt(1.f/120);
  upload();
  check(number(dev + 0x1700) == 0.25f, "upload outside render scope unchanged");
  {
    H::RenderTickScope outer(K::getGuestBase());
    dt(1.f/60);
    { H::RenderTickScope inner(K::getGuestBase()); upload(); }
    upload();
    float want = .25f; if (enabled) H::scaledTimeScale(.25f, 1.f/120, want);
    check(number(dev + 0x1700) == want, "nested scope restores outer tick snapshot");
    upload(input, 1);
    check(number(dev + 0x1710) == .25f, "unrelated constant register unchanged");
    upload(input, 0, 1, false);
    check(number(dev + 0x1700) == .25f, "same shader outside compiled effect context unchanged");
    for (const auto& mismatch : std::array<std::array<uint32_t,3>,6>{{
             {block, input + 16u, input}, {effect + 8u, dev + 0x100u, dev},
             {shaderRef + 4u, object + 0x100u, object}, {block + 88u, 0x00020000u, 0x00010000u},
             {records + 32u, input + 16u, input}, {block + 12u, 0x20000000u, 0u}}}) {
      word(mismatch[0], mismatch[1]); upload();
      check(number(dev + 0x1700) == .25f, "mismatched compiled source/device/shader/count/record/stage excluded");
      word(mismatch[0], mismatch[2]);
    }
    {
      H::CompiledEffectScope outerEffect(K::getGuestBase(), effect, block);
      { H::CompiledEffectScope innerEffect(K::getGuestBase(), 0, block); sdkUpload(); }
      sdkUpload();
      check(number(dev + 0x1700) == want, "nested effect scope restores the matching compiled source");
    }
    K::guestVAToHost(physical + 0x40)[0] ^= 1;
    upload();
    check(number(dev + 0x1700) == .25f, "recycled shader object with changed ucode is excluded");
    K::guestVAToHost(physical + 0x40)[0] ^= 1;
    number(dev + 0x1700, .25f);
    check(K::aliasGuestVA(0xc0601700u) == K::aliasGuestVA(dev + 0x1700), "distinct source VA aliases destination");
    word(block, 0xc0601700u); word(records + 32u, 0xc0601700u);
    upload(0xc0601700u);
    check(number(dev + 0x1700) == .25f, "alias overlap does not repeatedly scale authored storage");
    word(block, input); word(records + 32u, input);
    DWORD oldProtection = 0, ignored = 0;
    // A removal of readability follows the readable-interval cache's removal protocol (kernel/guest_context.hpp), as
    // every product site must: without it the cache could still answer "readable" for this page.
    K::hostRangeReadableBeginRemoval();
    const bool protectedCode = VirtualProtect(K::guestVAToHost(physical), 0x1000, PAGE_NOACCESS, &oldProtection);
    K::hostRangeReadableEndRemoval();
    check(protectedCode, "private shader page made unreadable for fault-free probe case");
    if (protectedCode) {
      const H::ReadStats r0 = H::readStats();
      upload();
      const H::ReadStats r1 = H::readStats();
      const bool unchanged = number(dev + 0x1700) == .25f;
      const bool restored = VirtualProtect(K::guestVAToHost(physical), 0x1000, oldProtection, &ignored);
      check(unchanged && restored, "unreadable shader metadata retains authored value without a fault");
      check(!enabled ? (r1.unreadable == r0.unreadable && r1.os == r0.os && r1.direct == r0.direct)
                     : expectDirect ? (r1.unreadable > r0.unreadable && r1.os > r0.os) : (r1.os > r0.os),
            "G3: the unreadable range: the predicate refuses it and the OS copy decides (direct arm) / the OS copy (=0)");
    }
    // G3, Astra's P2: a protection the predicate refuses but the OS copy reads (PAGE_EXECUTE, bracketed so the cache
    // re-queries it) - both arms must give the SAME result (the corrected value: the shader is still identified).
    {
      float got[2] = {0, 0};
      for (int mode = 0; mode < 2; ++mode) {
        DWORD execOld = 0, execIgnored = 0;
        K::hostRangeReadableBeginRemoval();
        const bool execOk = VirtualProtect(K::guestVAToHost(physical), 0x1000, PAGE_EXECUTE, &execOld);
        K::hostRangeReadableEndRemoval();
        H::setDirectReadsForTest(mode);
        number(input, .25f);
        upload();
        H::setDirectReadsForTest(-1);
        got[mode] = number(dev + 0x1700);
        const bool execBack = execOk && VirtualProtect(K::guestVAToHost(physical), 0x1000, execOld, &execIgnored);
        check(execOk && execBack, "G3: shader page made execute-only and restored (the P2 case)");
      }
      check(std::memcmp(&got[0], &got[1], sizeof(float)) == 0,
            "G3 (Astra P2): an execute-only shader page gives the OS copy's result in the direct arm too");
      number(dev + 0x1700, .25f);
    }
    // G3 belt and braces: a removal that SKIPS the protocol. The page was read (and cached readable) just before, so the
    // predicate's stale answer lets the direct copy fault: it must be caught, counted once, and the read refused through
    // the OS copy - the authored value kept, no crash.
    upload();  // the shader page read: the readable cache holds it
    {
      const H::ReadStats f0 = H::readStats();
      DWORD rawOld = 0, rawIgnored = 0;
      const bool rawProtected = VirtualProtect(K::guestVAToHost(physical), 0x1000, PAGE_NOACCESS, &rawOld);  // no protocol
      check(rawProtected, "private shader page made unreadable WITHOUT the removal protocol (a stale-cache case)");
      if (rawProtected) {
        upload();
        const H::ReadStats f1 = H::readStats();
        const bool kept = number(dev + 0x1700) == .25f;
        const bool rawRestored = VirtualProtect(K::guestVAToHost(physical), 0x1000, rawOld, &rawIgnored);
        check(kept && rawRestored, "a stale-cache read keeps the authored value without a crash");
        check(!enabled ? (f1.faults == f0.faults)
                       : expectDirect ? (f1.faults == f0.faults + 1 && f1.os > f0.os) : (f1.faults == f0.faults),
              "G3: the faulting direct copy is caught, counted once (faults +1) and refused through the OS copy");
      }
    }
    word(block + 88u, 0x00020000u);
    upload(input, 0, 2);
    check(!std::memcmp(K::guestVAToHost(dev + 0x1710), K::guestVAToHost(input + 16), 16),
          "multi-register upload preserves subsequent vectors");
    word(block + 88u, 0x00010000u);
  }
  for (float invalid : {-1.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    dt(invalid); H::RenderTickScope scope(K::getGuestBase()); upload();
    check(number(dev + 0x1700) == .25f, "invalid render interval retains authored behavior");
  }
  dt(1.f/120);
  float liveWant = .25f; if (enabled) H::scaledTimeScale(.25f, 1.f/120, liveWant);
  {
    H::RenderTickScope scope(K::getGuestBase());
    check(K::highfps::applyLive("off", true), "live HFR setting switches off");
    upload();
    check(number(dev + 0x1700) == liveWant, "live setting change keeps current tick coherent");
  }
  {
    H::RenderTickScope scope(K::getGuestBase()); upload();
    check(number(dev + 0x1700) == .25f, "next tick with live HFR off retains authored scale");
    check(K::highfps::applyLive("120", true), "live HFR setting switches on");
    upload();
    check(number(dev + 0x1700) == .25f, "enabling HFR mid-tick waits for a new scope");
  }
  { H::RenderTickScope scope(K::getGuestBase()); upload();
    check(number(dev + 0x1700) == liveWant, "new tick uses the enabled live HFR policy"); }
  // G3: a qualifying upload's metadata reads take the expected copy, and both copies give the same results in every
  // case of a small matrix (authored x interval x one excluded case), the copy forced per upload through the seam.
  {
    number(input, 0.25f); dt(1.f/120);
    const H::ReadStats g0 = H::readStats();
    { H::RenderTickScope scope(K::getGuestBase()); upload(); }
    const H::ReadStats g1 = H::readStats();
    if (!enabled)   // SR_HDR_ADAPT_DT=0: the correction is off, so nothing is read at all
      check(g1.direct == g0.direct && g1.os == g0.os, "G3 (SR_HDR_ADAPT_DT=0): no metadata read at all");
    else
      check(expectDirect ? (g1.direct > g0.direct && g1.os == g0.os) : (g1.os > g0.os && g1.direct == g0.direct),
            expectDirect ? "G3: the metadata reads are direct copies - no OS copy (ReadProcessMemory) per upload"
                         : "G3 =0 arm: the metadata reads are OS copies");
    std::printf("[HDR_ADAPT_TEST] G3 one upload: direct=%llu os=%llu unreadable=%llu\n",
                (unsigned long long)(g1.direct - g0.direct), (unsigned long long)(g1.os - g0.os),
                (unsigned long long)(g1.unreadable - g0.unreadable));
    int same = 0, cases = 0;
    for (float authored : {0.24749997f, 0.25f, 0.30000001f}) {
      for (float seconds : {1.f/30, 1.f/120, 0.f}) {
        for (int excluded = 0; excluded < 2; ++excluded) {
          float got[2] = {0, 0};
          for (int mode = 0; mode < 2; ++mode) {
            H::setDirectReadsForTest(mode);
            number(input, authored); dt(seconds);
            if (excluded) word(shaderRef + 4u, object + 0x100u);   // a mismatched shader: the authored value stays
            { H::RenderTickScope scope(K::getGuestBase()); upload(); }
            if (excluded) word(shaderRef + 4u, object);
            got[mode] = number(dev + 0x1700);
          }
          ++cases;
          same += std::memcmp(&got[0], &got[1], sizeof(float)) == 0 ? 1 : 0;
        }
      }
    }
    H::setDirectReadsForTest(-1);
    check(same == cases, "G3: the direct copy and the OS copy give bit-identical results in every matrix case");
    std::printf("[HDR_ADAPT_TEST] G3 identity: %d of %d cases bit-identical\n", same, cases);
    number(input, 0.25f); dt(1.f/120);
  }
  float result = 123;
  check(!H::scaledTimeScale(std::numeric_limits<float>::max(), 2, result) && result == 123,
        "overflow retains authored behavior");
  check(!H::scaledTimeScale(-.25f, .01, result), "negative authored scale excluded");
  const H::ReadStats rs = H::readStats();
  std::printf("[HDR_ADAPT_TEST] reads direct=%llu os=%llu unreadable=%llu faults=%llu (expect %s)\n",
              (unsigned long long)rs.direct, (unsigned long long)rs.os, (unsigned long long)rs.unreadable,
              (unsigned long long)rs.faults, expectDirect ? "direct" : "os");
  std::printf("[HDR_ADAPT_TEST] gate=%d %d passed, %d failed\n", int(enabled), passed, failed);
  return failed ? 1 : 0;
}
