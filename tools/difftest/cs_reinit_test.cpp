// Step 6 I8 (joint plan I8(a), Fable ruling 21 (3)): RtlInitializeCriticalSection's re-init, driven through the REAL
// exports (RtlInitializeCriticalSection / RtlEnterCriticalSection / RtlLeaveCriticalSection /
// RtlTryEnterCriticalSection) with the counts (kernel/cs_reinit.hpp) and the host object's identity (SR_ORACLE seam).
//
// Run: build/difftest.exe --cs-reinit-test     (exit = failed checks; ~1 s)
// Mutation / attribution: SR_CS_REINIT_REUSE=0 restores the old path (every re-init a new object, the old one to the
// graveyard): R2 R3 R4 R4b R5 R6 FAIL (6 checks: their re-inits are "replaced", not reused or held; the identity changes;
// the graveyard grows), R1 R7 pass - measured on 98c1d2ea's dev build (run_i8b, 2026-10-21).
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/export_registry.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/stubs.hpp"
#include "kernel/cs_reinit.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <thread>

namespace K = sr::kernel;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

constexpr size_t kGuestSize = 0x100000000ull;
uint8_t* g_base = nullptr;
// guest CS objects (RTL_CRITICAL_SECTION is 0x1C bytes) in difftest's flat scratch memory
constexpr uint32_t kCsA = 0x90700000u, kCsB = 0x90700040u, kCsC = 0x90700080u, kCsD = 0x907000C0u;

struct Ctx {
  PPCContext c{};
  explicit Ctx(uint32_t stack) {
    K::setGuestMemory(g_base, kGuestSize);
    c.r1.u32 = stack;
    c.r14.u32 = 0x829C0000u;
    K::setCurrentGuestContext(&c, offsetof(PPCContext, lr), offsetof(PPCContext, ctr),
                              offsetof(PPCContext, r3), offsetof(PPCContext, r1),
                              offsetof(PPCContext, r31));
  }
  ~Ctx() { K::setCurrentGuestContext(nullptr, 0); }
};

uint32_t call(const char* name, uint32_t a3) {
  K::ExportRegistry::setCurrentExtraArgs(0, 0);
  return K::ExportRegistry::instance().getExport(name)(a3, 0, 0, 0, 0, 0);
}
void init(uint32_t cs) { call("RtlInitializeCriticalSection", cs); }
void enter(uint32_t cs) { call("RtlEnterCriticalSection", cs); }
void leave(uint32_t cs) { call("RtlLeaveCriticalSection", cs); }
bool tryEnter(uint32_t cs) { return call("RtlTryEnterCriticalSection", cs) != 0; }
bool sums(const K::CsInitCounts& c) { return c.first + c.reused + c.held + c.replaced == c.all; }
}  // namespace

int runCsReinitTest() {
  std::printf("[CS_REINIT_TEST] RtlInitializeCriticalSection re-init (step 6 I8, SR_CS_REINIT_REUSE)\n");
  g_base = K::getGuestBase();
  K::registerStubs();
  Ctx mainCtx(0x90100000u);

  // R1: a first init makes the object
  auto c0 = K::csInitCounts();
  init(kCsA);
  auto c1 = K::csInitCounts();
  const uintptr_t a0 = K::csHostIdentityForTest(kCsA);
  check(a0 != 0 && c1.all == c0.all + 1 && c1.first == c0.first + 1 && sums(c1), "R1 first init creates the object");

  // R2: re-init of a CS nobody holds (it was entered and left) keeps the SAME object; nothing goes to the graveyard
  enter(kCsA);
  leave(kCsA);
  init(kCsA);
  auto c2 = K::csInitCounts();
  check(K::csHostIdentityForTest(kCsA) == a0 && c2.reused == c1.reused + 1 && c2.graveyard == c1.graveyard &&
            c2.held == c1.held && sums(c2),
        "R2 an unlocked re-init reuses the object");

  // R3: 1,000 re-inits of an unlocked CS: one object throughout, the graveyard does not grow (gate 3's +282 / reload)
  for (int i = 0; i < 1000; ++i) init(kCsA);
  auto c3 = K::csInitCounts();
  check(K::csHostIdentityForTest(kCsA) == a0 && c3.reused == c2.reused + 1000 && c3.graveyard == c2.graveyard &&
            sums(c3),
        "R3 repeated re-inits keep one object and no graveyard growth");

  // R4: re-init while ANOTHER thread owns the CS: logged and skipped - the old path (a new object, the old one kept)
  init(kCsB);
  const uintptr_t b0 = K::csHostIdentityForTest(kCsB);
  std::atomic<int> phase{0};
  std::thread owner([&] {
    Ctx ctx(0x90200000u);
    enter(kCsB);
    phase = 1;
    while (phase.load() != 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    leave(kCsB);   // on the NEW object: not its owner, a no-op (the old path's behaviour, unchanged)
    phase = 3;
  });
  while (phase.load() != 1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  auto c4a = K::csInitCounts();
  init(kCsB);
  auto c4 = K::csInitCounts();
  const uintptr_t b1 = K::csHostIdentityForTest(kCsB);
  phase = 2;
  owner.join();
  check(b1 != 0 && b1 != b0 && c4.held == c4a.held + 1 && c4.reused == c4a.reused &&
            c4.graveyard == c4a.graveyard + 1 && sums(c4),
        "R4 a re-init of an owned CS keeps the old path (held, logged)");
  // ... and the re-init by the OWNER itself is held too (its recursive try_lock would succeed: owner is checked first)
  init(kCsD);
  enter(kCsD);
  auto c4b = K::csInitCounts();
  init(kCsD);
  auto c4c = K::csInitCounts();
  check(c4c.held == c4b.held + 1 && c4c.reused == c4b.reused && sums(c4c),
        "R4b a re-init by the owning thread is held, not reused");
  // (kCsD's old object stays entered in the graveyard, as on the old path)

  // R5: a waiter that waited and finished leaves no trace: the next re-init reuses (the waiter count went back to 0)
  init(kCsC);
  const uintptr_t cId = K::csHostIdentityForTest(kCsC);
  std::atomic<int> step{0};
  std::atomic<bool> waiterGot{false};
  std::thread holder([&] {
    Ctx ctx(0x90300000u);
    enter(kCsC);
    step = 1;
    while (step.load() != 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    leave(kCsC);
  });
  while (step.load() != 1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::thread waiter([&] {
    Ctx ctx(0x90400000u);
    enter(kCsC);   // blocks in the timed wait (> 2 iterations of 50 ms) until the holder leaves
    waiterGot = true;
    leave(kCsC);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(160));
  const bool blocked = !waiterGot.load();
  step = 2;
  holder.join();
  waiter.join();
  auto c5a = K::csInitCounts();
  init(kCsC);
  auto c5 = K::csInitCounts();
  check(blocked && waiterGot && K::csHostIdentityForTest(kCsC) == cId && c5.reused == c5a.reused + 1 && sums(c5),
        "R5 after a finished wait the re-init reuses (the waiter count returns to 0)");

  // R6: the reused object still excludes: another thread's hold makes TryEnter fail, its release lets it succeed
  std::atomic<int> s6{0};
  std::thread t6([&] {
    Ctx ctx(0x90500000u);
    enter(kCsA);
    s6 = 1;
    while (s6.load() != 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    leave(kCsA);
    s6 = 3;
  });
  while (s6.load() != 1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const bool contended = !tryEnter(kCsA);
  s6 = 2;
  while (s6.load() != 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  t6.join();
  const bool freed = tryEnter(kCsA);
  if (freed) leave(kCsA);
  check(contended && freed && K::csHostIdentityForTest(kCsA) == a0, "R6 the reused CS still excludes");

  auto cEnd = K::csInitCounts();
  std::printf("[CS_REINIT_TEST] counts all=%llu first=%llu reused=%llu held=%llu replaced=%llu graveyard=%zu\n",
              (unsigned long long)cEnd.all, (unsigned long long)cEnd.first, (unsigned long long)cEnd.reused,
              (unsigned long long)cEnd.held, (unsigned long long)cEnd.replaced, cEnd.graveyard);
  check(sums(cEnd), "R7 first + reused + held + replaced = all");
  std::printf("[CS_REINIT_TEST] passed=%d failed=%d\n", g_pass, g_fail);
  return g_fail;
}
