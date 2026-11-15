// Compile actual native_meter.cpp + audio_native_census.cpp with SR_ORACLE=0.
// The dedicated fixture include path supplies only guest/device dependencies.
// No generated guest, dispatcher, game, GPU, or audio worker is linked or run.
#include "kernel/native_meter.hpp"
#include "kernel/audio_native_census.hpp"
#include "app/log.hpp"
#include "guest/ppc/ppc_recomp_shared.h"
#include "kernel/apu/xma_device.hpp"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <windows.h>

static_assert(SR_ORACLE == 0, "this fixture must exercise the release APIs");
namespace M = sr::native::meter;
namespace A = sr::kernel::apu;
namespace {
int failures = 0;
int delegateCalls[7] = {};
bool throwDelegate = false;
uint32_t storedAddress = 0, storedValue = 0;
void check(bool value, const char* label) {
  std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
  failures += !value;
}
void delegated(int which) {
  ++delegateCalls[which];
  if (throwDelegate) throw std::runtime_error("fixture delegate failure");
}
}

void sr::Log::write(LogCategory, LogSeverity, const char* format, ...) {
  char text[4096];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  std::puts(text);
}
void sr::Log::shutdown() {
  std::printf("[NATIVE_METER_REFUSAL] boundary=%llu delegated=%d\n",
              (unsigned long long)sr::kernel::audioNativeCensus().reg600_lwbrx, delegateCalls[0]);
  std::fflush(stdout);
}
namespace sr::kernel {
uint32_t readGuestU32BE(uint32_t) { throw std::runtime_error("release observer read guest memory"); }
void writeGuestBE32(uint32_t address, uint32_t value) { storedAddress = address; storedValue = value; }
}
#define DELEGATE(address, slot) PPC_FUNC(__imp__sub_##address) { (void)ctx; (void)base; delegated(slot); }
DELEGATE(82116800, 0)
DELEGATE(82119980, 1)
DELEGATE(82119BA8, 2)
DELEGATE(82119C28, 3)
DELEGATE(82119CE8, 4)
DELEGATE(82119E68, 5)
PPC_FUNC(sub_82117018) { (void)ctx; (void)base; delegated(6); }
PPC_FUNC(sub_82116800);
PPC_FUNC(sub_82119980);
PPC_FUNC(sub_82119BA8);
PPC_FUNC(sub_82119C28);
PPC_FUNC(sub_82119CE8);
PPC_FUNC(sub_82119E68);

int main(int argc, char** argv) {
  const bool reset = argc == 2 && std::strcmp(argv[1], "--control=reset") == 0;
  const bool omit = argc == 2 && std::strcmp(argv[1], "--control=omit-terminal") == 0;
  const bool unwired = argc == 2 && std::strcmp(argv[1], "--control=unwired") == 0;
  const bool initFailure = argc == 2 && std::strcmp(argv[1], "--control=array-init-failure") == 0;
  if (argc > 2 || (argc == 2 && !reset && !omit && !unwired && !initFailure)) return 2;
  _putenv_s("SR_NATIVE_METER", "1");
  _putenv_s("SR_XMA_ARRAY_NATIVE", "0");  // Release must ignore the old A/B selector.
  PPCContext context{};
  A::fixtureEnabled = true;
  std::printf("[QUIT] channel Local\\SR_QUIT_witness_%lu ready\n", GetCurrentProcessId());
  if (initFailure) {
    A::fixtureInit = false;
    sub_82116800(context, nullptr);
    std::puts("FAIL release delegated instead of refusing unavailable native array");
    return 1;
  }
  using Entry = void (*)(PPCContext&, uint8_t*);
  const Entry entries[] = {sub_82116800, sub_82119980, sub_82119BA8, sub_82119C28, sub_82119CE8, sub_82119E68};
  for (int i = 1; i < 6; ++i) {
    const auto before = sr::kernel::audioNativeCensus();
    entries[i](context, nullptr);
    const auto after = sr::kernel::audioNativeCensus();
    check(after.guest_mmio_fns == before.guest_mmio_fns + 1, "actual release MMIO wrapper increments before delegation");
    check(delegateCalls[i] == 1, "actual wrapper delegates exactly once");
  }
  throwDelegate = true;
  for (int i = 1; i < 6; ++i) {
    const auto before = sr::kernel::audioNativeCensus();
    bool threw = false;
    try { entries[i](context, nullptr); } catch (const std::runtime_error&) { threw = true; }
    check(threw && sr::kernel::audioNativeCensus().guest_mmio_fns == before.guest_mmio_fns + 1,
          "counter survives failing delegate, never hidden by early fault");
    check(delegateCalls[i] == 2, "throwing wrapper delegates exactly once");
  }
  throwDelegate = false;
  A::fixtureEnabled = true;
  context.r5.u64 = 5;
  context.r6.u64 = 6;
  sub_82116800(context, nullptr);
  auto audio = sr::kernel::audioNativeCensus();
  check(audio.guest_mmio_fns == 10 && audio.reg600_native == 1 && delegateCalls[0] == 0,
        "release array selector is baked ON and never delegates MMIO");
  check(storedAddress == 0x829AC0E8 && storedValue == 0x1FCAA000 && context.r3.u64 == 5 &&
        context.r4.u64 == 6 && delegateCalls[6] == 1, "native publication and tail call preserved");
  A::fixtureRegisterWrites = 7;
  std::array<uint64_t, 8> before{}, after{};
  M::snapshotColumns(before.data(), int(before.size()));
  check(before.back() == 17, "xma column pulls real entry and device getter values");
  for (unsigned i = 0; i < M::kIds; ++i) M::add(M::Id(i), i + 1);
  M::snapshotColumns(after.data(), int(after.size()));
  for (unsigned i = 0; i < M::kIds; ++i)
    check(after[i] == before[i] + i + 1, "release pushed column preserves exact nonzero increment");
  check(!M::columnWired(M::kEdramLoad), "uninitialized EDRAM remains unmeasured");
  if (!unwired) {
    M::wire(M::kEdramResolve); M::wire(M::kEdramTransfer); M::wire(M::kEdramLoad);
  }
  M::onFrame();
  M::snapshotColumns(before.data(), int(before.size()));
  check(before == after, "first-frame initialization never resets pre-frame counters");
  M::onFrame();
  M::report("periodic");
  if (reset) M::g_count[M::kPm4].store(0);
  std::puts("[QUIT] request received: frames=1 swaps=1 vdswaps=2 -> quiesce (fixture)");
  if (!omit) { M::report("terminal"); M::report("terminal"); }
  std::puts("[QUIT] terminal snapshot complete frames=1 swaps=1 vdswaps=2 tail=0 rl_pending=0 threads=0 parked=0 waiting=0 host_idle=0 families=1 fields=8 elapsed_ms=0 stranded=0");
  std::printf("[NATIVE_METER_WITNESS_TEST] failures=%d\n", failures);
  return failures ? 1 : 0;
}
