// Bounded native source-0 effects against the actual localized retail SDK ISR.
// --native-display-test [native | retired]. No clock-phase or live display acceptance.
// The SDK body pin is recorded in kernel/native_display.cpp; tests deliberately
// use scratch/device aliases to detect reordered reads and stale callback capture.
// Admission (native/dreg-hang, 2026-10-15) is arithmetic, kernel/guest_protect.hpp's contract: a range outside the
// registered arena, or touching a range a foreign protector DECLARED before protecting it, declines before any
// mutation; an all-excluding declaration declines everything; `retired` (its own process: retirement is permanent)
// checks the arena lease. No OS permission query on a null-callback completion.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "ppc/ppc_recomp_shared.h"
#include "guest/guest_harness.hpp"
#include "kernel/export_registry.hpp"
#include "kernel/guest_call.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/guest_protect.hpp"
#include "kernel/native_display.hpp"
#include "kernel/stubs.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <windows.h>

extern "C" void __imp__sub_820FDBA0(PPCContext& __restrict, uint8_t*);

namespace {
namespace K = sr::kernel;
namespace D = sr::native::display;
constexpr uint32_t kDev = 0x70690000u, kChild = 0x70695000u;
constexpr uint32_t kStack = 0x70578000u, kPcr = 0x706A0000u, kScratch = 0x706A1000u;
constexpr uint32_t kCallback = uint32_t(PPC_CODE_BASE) + 8u;
constexpr uint32_t kEnable = 0x7FC86544u, kFault = 0xE053D001u;
int passed = 0, failed = 0;
void check(bool value, const char* name) {
  std::printf("%s %s\n", value ? "  ok " : "FAIL:", name);
  value ? ++passed : ++failed;
}
uint32_t read(uint32_t va) { return K::readGuestU32BE(va); }
void write(uint32_t va, uint32_t value) { K::writeGuestBE32(va, value); }

enum class Action { Observe, Mutate, Reenter, Fault };
struct Witness {
  uint64_t stack, argument, pcr;
  std::array<uint32_t, 3> args;
  std::array<uint32_t, 5> state;
  uint32_t dpc;
  uint8_t cpu, irql;
  bool operator==(const Witness&) const = default;
};
thread_local std::vector<Witness> witnesses;
thread_local Action action = Action::Observe;
thread_local bool nativeCall = false;
thread_local bool nestedGood = true;

void observeCallback(PPCContext& ctx, uint8_t* base) {
  const uint32_t arg = ctx.r3.u32, dev = ctx.r4.u32;
  witnesses.push_back({ctx.r1.u64, ctx.r3.u64, ctx.r13.u64,
      {read(arg), read(arg + 4u), read(arg + 8u)},
      {read(dev + 0x3B10u), read(dev + 0x3B14u), read(dev + 0x3B18u),
       read(dev + 0x3B1Cu), read(dev + 0x3B20u)},
      read(ctx.r13.u32 + K::kKpcrOffDpcActive),
      *K::guestVAToHost(ctx.r13.u32 + K::kKpcrOffCurrentCpu),
      *K::guestVAToHost(ctx.r13.u32 + K::kKpcrOffIrql)});
  if (action == Action::Mutate) {
    write(dev + 0x3B10u, 0u);
    write(dev + 0x3B14u, 0x12345678u);
    write(dev + 0x3B20u, 0x87654321u);
  } else if (action == Action::Reenter) {
    ctx.r3.u64 = 0;
    ctx.r4.u64 = kChild;
    if (nativeCall) {
      uint32_t exception = UINT32_MAX;
      nestedGood &= D::trySource0(ctx, base, D::kD3DIsr, kChild, &exception) == D::Source0Result::Completed && !exception;
    } else __imp__sub_820FDBA0(ctx, base);
  } else if (action == Action::Fault) {
    RaiseException(kFault, 0u, 0u, nullptr);
  }
  ctx.r3.u64 = 0xFEEDBEEF12345678ull;
  ctx.r9.u64 = 0x1122334455667788ull;
  ctx.r10.u64 = 0x8877665544332211ull;
}

struct State {
  std::array<uint8_t, 0xA000u> devices;
  std::array<uint8_t, 0x200u> stack;
  std::array<uint8_t, 0x40u> scratch;
  std::array<uint8_t, 0x154u> pcr;
  bool operator==(const State&) const = default;
};
State capture() {
  State result;
  std::memcpy(result.devices.data(), K::guestVAToHost(kDev), result.devices.size());
  std::memcpy(result.stack.data(), K::guestVAToHost(kStack - result.stack.size()), result.stack.size());
  std::memcpy(result.scratch.data(), K::guestVAToHost(kScratch), result.scratch.size());
  std::memcpy(result.pcr.data(), K::guestVAToHost(kPcr), result.pcr.size());
  return result;
}
void seed(uint32_t count, uint32_t frameNumber, uint32_t cb, uint32_t gateTarget) {
  std::memset(K::guestVAToHost(kDev), 0xA5, 0xA000u);
  std::memset(K::guestVAToHost(kStack - 0x200u), 0x5A, 0x200u);
  std::memset(K::guestVAToHost(kScratch), 0xC3, 0x40u);
  std::memset(K::guestVAToHost(kPcr), 0, 0x154u);
  for (uint32_t dev : {kDev, kChild}) {
    write(dev + 0x2A14u, dev == kDev ? gateTarget - 4u : kScratch + 12u);
    write(dev + 0x3B10u, dev == kDev ? cb : 0u);
    write(dev + 0x3B14u, dev == kDev ? frameNumber : 42u);
    write(dev + 0x3B18u, 0x13579BDFu);
    write(dev + 0x3B1Cu, dev == kDev ? count : 1u);
    write(dev + 0x3B20u, 0x2468ACE0u);
  }
  *K::guestVAToHost(kPcr + K::kKpcrOffCurrentCpu) = 2u;
  write(kEnable, 1u);
  witnesses.clear();
  nestedGood = true;
}
PPCContext input() {
  PPCContext ctx{};
  ctx.r1.u64 = 0x1234000000000000ull | kStack;
  ctx.r3.u64 = 0xABCD000000000000ull;
  ctx.r4.u64 = 0x2345000000000000ull | kDev;
  ctx.r13.u64 = kPcr;
  ctx.r14.u64 = 0x829C0000u;
  ctx.lr = 0xABCDEFFFu;
  ctx.r9.u64 = 0x9999888877776666ull;
  ctx.r10.u64 = 0xAAAABBBBCCCCDDDDull;
  return ctx;
}

void differential(const char* name, uint32_t count, uint32_t frameNumber,
                  uint32_t cb, uint32_t gateTarget, Action requested = Action::Observe) {
  action = requested;
  seed(count, frameNumber, cb, gateTarget);
  PPCContext oracle = input();
  nativeCall = false;
  {
    K::KpcrDpcScope scope(kPcr);
    K::ExportRegistry::setGuestCallContext(true);
    __imp__sub_820FDBA0(oracle, K::getGuestBase());
    K::ExportRegistry::setGuestCallContext(false);
  }
  const State expected = capture();
  const auto expectedWitnesses = witnesses;
  const bool expectedNested = nestedGood;
  seed(count, frameNumber, cb, gateTarget);
  PPCContext actual = input();
  nativeCall = true;
  uint32_t exception = UINT32_MAX;
  D::Source0Result result;
  {
    K::KpcrDpcScope scope(kPcr);
    K::ExportRegistry::setGuestCallContext(true);
    result = D::trySource0(actual, K::getGuestBase(), D::kD3DIsr, kDev, &exception);
    K::ExportRegistry::setGuestCallContext(false);
  }
  const bool same = result == D::Source0Result::Completed && exception == 0u && expectedNested && nestedGood &&
    expected == capture() && expectedWitnesses == witnesses && !std::memcmp(&oracle, &actual, sizeof(actual));
  if (!same) std::printf("  result=%u exception=%08X ctx=%u memory=%u callbacks=%zu/%zu nested=%u\n",
      unsigned(result), exception, unsigned(!std::memcmp(&oracle, &actual, sizeof(actual))),
      unsigned(expected == capture()), witnesses.size(), expectedWitnesses.size(), unsigned(nestedGood));
  check(same, name);
  if (!witnesses.empty()) check(witnesses[0].stack == input().r1.u64 - 0x90u &&
      witnesses[0].argument == input().r1.u64 - 0x30u && witnesses[0].args[2] == 0u &&
      witnesses[0].cpu == 2u && witnesses[0].irql == 2u && witnesses[0].dpc == 1u,
      "callback receives exact ISR stack placement and CPU2/DPC context");
}

void rejection(const char* name, void (*change)(PPCContext&), uint32_t isr = D::kD3DIsr,
               uint32_t context = kDev, uint8_t* differentBase = nullptr) {
  seed(1u, 99u, kCallback, kScratch + 4u);
  PPCContext ctx = input();
  K::KpcrDpcScope scope(kPcr);
  if (change) change(ctx);
  const State before = capture();
  const PPCContext initial = ctx;
  uint32_t exception = 0xABABABABu;
  const auto result = D::trySource0(ctx, differentBase ? differentBase : K::getGuestBase(), isr, context, &exception);
  check(result == D::Source0Result::Declined && exception == 0xABABABABu && before == capture() &&
        !std::memcmp(&initial, &ctx, sizeof(ctx)) && witnesses.empty(), name);
}

// A foreign protector's page (kernel/guest_protect.hpp): DECLARED before its protection change, as every production
// protector must; the declaration is permanent, so these rows run after every row that admits the same pages.
void protectedRejection(const char* name, uint32_t va, DWORD protection, uint32_t count = 1u) {
  seed(count, 99u, kCallback, kScratch + 4u);
  PPCContext ctx = input();
  K::KpcrDpcScope scope(kPcr);
  const State before = capture();
  const PPCContext initial = ctx;
  uint8_t* page = K::guestVAToHost(va & ~0xFFFu);
  K::guest_protect::declare(page, 0x1000u, K::guest_protect::Scope::Range);
  DWORD original = 0;
  const bool changed = VirtualProtect(page, 0x1000u, protection, &original) != 0;
  uint32_t exception = 0xABABABABu;
  const auto result = changed ? D::trySource0(ctx, K::getGuestBase(), D::kD3DIsr, kDev, &exception) : D::Source0Result::Completed;
  DWORD temporary = 0;
  const bool restored = changed && VirtualProtect(page, 0x1000u, original, &temporary) != 0;
  if (!restored) { check(false, name); std::_Exit(2); }
  check(result == D::Source0Result::Declined && exception == 0xABABABABu && before == capture() &&
        !std::memcmp(&initial, &ctx, sizeof(ctx)) && witnesses.empty(), name);
}

// The registered arena (getGlobalGuestMemory) as `size` bytes from `base` for one call: a range outside it declines.
void arenaRejection(const char* name, uint8_t* base, size_t size) {
  uint8_t* savedBase = nullptr;
  size_t savedSize = 0;
  const K::ExCreateThreadRunnerFn savedRunner = K::getExCreateThreadRunner(&savedBase, &savedSize);
  K::registerExCreateThreadRunner(nullptr, base, size);
  rejection(name, nullptr);
  K::registerExCreateThreadRunner(savedRunner, savedBase, savedSize);
}

// `retired` / `excluded`: each its own process, since retirement and an all-excluding declaration are permanent. The
// same fixture first completes (the positive control), then declines untouched.
int runPermanent(bool retire) {
  differential(retire ? "retired control: the fixture completes before the arena is retired"
                      : "excluded control: the fixture completes before the all-excluding declaration",
               1u, 99u, 0u, kScratch + 4u);
  if (retire) {
    K::guest_protect::retireArena(0u);
    rejection("a retired guest arena (teardown) refuses the lease: declines before any access", nullptr);
  } else {
    K::guest_protect::declare(nullptr, 0u, K::guest_protect::Scope::All);  // a diagnostic's Scope::All
    rejection("an all-excluding declaration declines every call before any access", nullptr);
  }
  std::printf("[NATIVE_DISPLAY_TEST] %s: %d passed, %d failed\n", retire ? "retired" : "excluded", passed, failed);
  return failed ? 1 : 0;
}
}  // namespace

int runNativeDisplayTest(const char* mode) {
  if (mode && std::strcmp(mode, "native") && std::strcmp(mode, "retired") && std::strcmp(mode, "excluded")) return 2;
  passed = failed = 0;
  K::registerStubs();
  sr::guest::initializeDispatchTable();
  // Arithmetic admission reads the process's registered arena (the game registers it at guest init).
  K::registerExCreateThreadRunner(nullptr, K::getGuestBase(), K::getGuestSize());
  if (mode && (!std::strcmp(mode, "retired") || !std::strcmp(mode, "excluded"))) {
    auto*& permanentSlot = PPC_LOOKUP_FUNC(K::getGuestBase(), kCallback);
    PPCFunc* const saved = permanentSlot;
    permanentSlot = observeCallback;
    const int rc = runPermanent(!std::strcmp(mode, "retired"));
    permanentSlot = saved;
    return rc;
  }
  auto*& slot = PPC_LOOKUP_FUNC(K::getGuestBase(), kCallback);
  PPCFunc* original = slot;
  slot = observeCallback;
  const uint32_t originalEnable = read(kEnable);
  for (uint32_t count : {0u, 1u, 2u, 0x7FFFFFFFu, 0x80000000u, UINT32_MAX}) {
    char name[160];
    std::snprintf(name, sizeof(name), "actual SDK countdown=%08X, exact ctx/device/stack/callback effects", count);
    differential(name, count, 99u, kCallback, kScratch + 4u);
  }
  differential("frame wrap matches SDK including full-width r10/r3 state", 1u, UINT32_MAX, kCallback, kScratch + 4u);
  differential("null callback retains original r3 and no argument block", 1u, 99u, 0u, kScratch + 4u);
  differential("scratch alias clears frame before last-release and payload rereads", 1u, 99u, kCallback, kDev + 0x3B14u);
  differential("scratch alias clears callback before it can be invoked", 1u, 99u, kCallback, kDev + 0x3B10u);
  differential("scratch alias clears an initially invalid callback, requiring no lookup", 1u, 99u, 0xDEADBEEFu, kDev + 0x3B10u);
  differential("scratch alias clears submissions before callback payload read", 1u, 99u, kCallback, kDev + 0x3B20u);
  differential("scratch alias of countdown matches actual post-clear state", 1u, 99u, kCallback, kDev + 0x3B1Cu);
  differential("scratch alias of last-release keeps clear-before-frame store order", 1u, 99u, kCallback, kDev + 0x3B18u);
  differential("callback-owned device mutations remain visible after return", 1u, 99u, kCallback, kScratch + 4u, Action::Mutate);
  differential("nested source0 uses a distinct lower ISR frame and preserves outer scope", 1u, 99u, kCallback, kScratch + 4u, Action::Reenter);
  // Registration precedes ring initialization. Nonpositive countdown must not
  // dereference the still-null scratch pointer.
  differential("initial boot countdown ignores scratch pointer zero", 0u, 99u, 0u, 4u);
  rejection("unknown registered ISR declines before mutation", nullptr, 0x82111E38u);
  rejection("registered context mismatch declines before mutation", nullptr, D::kD3DIsr, kChild);
  rejection("different guest base declines before mutation", nullptr, D::kD3DIsr, kDev, K::getGuestBase() + 32u);
  rejection("source1 is outside this helper", [](PPCContext& ctx) {ctx.r3.u64 = 1u;});
  rejection("unaligned KPCR declines before volatile-u32 loads or any mutation", [](PPCContext& ctx) {ctx.r13.u64 = kPcr + 1u;});
  rejection("wrong CPU declines before mutation", [](PPCContext&) {*K::guestVAToHost(kPcr + K::kKpcrOffCurrentCpu) = 1u;});
  rejection("non-DPC state declines before mutation", [](PPCContext&) {write(kPcr + K::kKpcrOffDpcActive, 0u);});
  rejection("wrong IRQL declines before mutation", [](PPCContext&) {*K::guestVAToHost(kPcr + K::kKpcrOffIrql) = 0u;});
  rejection("unmapped callback declines before any state or stack store", [](PPCContext&) {write(kDev + 0x3B10u, uint32_t(PPC_CODE_BASE) + 3u);});
  rejection("null release scratch declines before mutation", [](PPCContext&) {write(kDev + 0x2A14u, 0u);});
  rejection("overflow release scratch declines before mutation", [](PPCContext&) {write(kDev + 0x2A14u, 0xFFFFFFFCu);});
  rejection("stack/device overlap declines before mutation", [](PPCContext& ctx) {ctx.r1.u64 = kDev + 0x3B20u;});
  rejection("scratch/stack overlap declines before mutation", [](PPCContext&) {write(kDev + 0x2A14u, kStack - 0x34u);});
  // The registered arena bounds every guest range (native/dreg-hang): the device, stack, KPCR and scratch all lie
  // above 0x70000000 here.
  arenaRejection("device / stack / KPCR above the registered arena decline before mutation", K::getGuestBase(),
                 0x70000000u);
  arenaRejection("device / stack / KPCR below the registered arena decline before mutation",
                 K::getGuestBase() + 0x90000000ull, 0x10000000u);
  arenaRejection("no registered arena declines before mutation", nullptr, 0u);
  // No OS permission query on a tick that completes with a null swap callback (dreg2: five VirtualQuery walks of the
  // resident arena per tick held the CPU-2 lock ~21 ms); a non-null callback's dispatch / host-code check makes some.
  {
    const uint64_t q0 = D::test::osQueries();
    differential("null callback, release tick: completes with no OS permission query", 1u, 99u, 0u, kScratch + 4u);
    differential("null callback, countdown 2: completes with no OS permission query", 2u, 99u, 0u, kScratch + 4u);
    const uint64_t q1 = D::test::osQueries();
    differential("non-null callback: its dispatch-slot / host-code check queries", 1u, 99u, kCallback, kScratch + 4u);
    const uint64_t q2 = D::test::osQueries();
    check(q1 == q0 && q2 > q1, "OS permission queries: 0 across null-callback completions, > 0 only for a callback");
  }

  seed(1u, 99u, kCallback, kScratch + 4u);
  action = Action::Observe;
  nativeCall = true;
  PPCContext independent = input();
  uint8_t* mmioPage = K::guestVAToHost(kEnable & ~0xFFFu);
  DWORD originalProtection = 0, temporaryProtection = 0;
  bool independentCompleted = false;
  const bool mmioProtected = VirtualProtect(mmioPage, 0x1000u, PAGE_NOACCESS, &originalProtection) != 0;
  if (mmioProtected) {
    uint32_t independentException = UINT32_MAX;
    K::KpcrDpcScope scope(kPcr);
    K::ExportRegistry::setGuestCallContext(true);
    independentCompleted = D::trySource0(independent, K::getGuestBase(), D::kD3DIsr, kDev, &independentException) ==
      D::Source0Result::Completed && independentException == 0u;
    K::ExportRegistry::setGuestCallContext(false);
  }
  const bool mmioRestored = mmioProtected &&
    VirtualProtect(mmioPage, 0x1000u, originalProtection, &temporaryProtection) != 0;
  check(mmioRestored && independentCompleted && witnesses.size() == 1u && read(kDev + 0x3B14u) == 100u,
        "native source0 completes with the MMIO enable page PAGE_NOACCESS");
  if (!mmioRestored) {
    slot = original;
    std::printf("[NATIVE_DISPLAY_TEST] protection restoration failed; stopping this fixture bank\n");
    return 1;
  }

  seed(1u, 99u, kCallback, kScratch + 4u);
  PPCContext ctx = input();
  action = Action::Fault;
  nativeCall = true;
  uint32_t exception = 0u;
  D::Source0Result faultResult;
  {
    K::KpcrDpcScope scope(kPcr);
    K::ExportRegistry::setGuestCallContext(true);
    faultResult = D::trySource0(ctx, K::getGuestBase(), D::kD3DIsr, kDev, &exception);
    K::ExportRegistry::setGuestCallContext(false);
  }
  check(faultResult == D::Source0Result::CallbackFault && exception == kFault && ctx.r1.u64 == input().r1.u64 &&
        read(kDev + 0x3B14u) == 100u && read(kDev + 0x3B18u) == 100u && read(kDev + 0x3B1Cu) == 0u &&
        read(kScratch + 4u) == 0u && witnesses.size() == 1u,
        "callback fault reports terminal committed operation without rollback or replay");

  // Foreign protectors, declared before protecting (permanent declarations: after every admitting row).
  action = Action::Observe;
  protectedRejection("declared PAGE_NOACCESS device rejection", kDev + 0x3B10u, PAGE_NOACCESS);
  protectedRejection("declared read-only device rejects before writes", kDev + 0x3B10u, PAGE_READONLY);
  protectedRejection("declared guarded stack rejects without consuming guard", kStack - 0x90u,
                     PAGE_READWRITE | PAGE_GUARD);
  protectedRejection("declared protected release scratch rejects before mutation", kScratch, PAGE_NOACCESS);
  protectedRejection("declared protected KPCR rejects without fault", kPcr, PAGE_NOACCESS);
  slot = original;
  write(kEnable, originalEnable);
  check(PPC_LOOKUP_FUNC(K::getGuestBase(), kCallback) == original, "dispatch callback fixture is restored");
  std::printf("[NATIVE_DISPLAY_TEST] %d passed, %d failed (bounded source0 SDK differential; no live display acceptance)\n", passed, failed);
  return failed ? 1 : 0;
}
