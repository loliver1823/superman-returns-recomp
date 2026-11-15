// --graphics-interrupt-registration-test [native]. Controlled VD/CP delivery
// lifetimes exercise the publication service, without guest execution or MMIO.
#include "kernel/graphics_interrupt_registration.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace {
namespace G = sr::kernel::graphics_interrupt;
using namespace std::chrono_literals;
constexpr G::Registration kA{0x820FDBA0u, 0x81234000u, true};
constexpr G::Registration kB{0x820FDBA0u, 0x85678000u, false};
constexpr G::Registration kC{0x82345678u, 0x89ABC000u, true};
int passed = 0, failed = 0;

void check(bool good, const char* name) {
  std::printf("[%s] %s\n", good ? "PASS" : "FAIL", name);
  good ? ++passed : ++failed;
}
bool same(const G::Registration& a, const G::Registration& b) {
  return a.callbackVA == b.callbackVA && a.contextVA == b.contextVA &&
         a.nativeSource0 == b.nativeSource0;
}

struct Signal {
  std::mutex mutex;
  std::condition_variable changed;
  bool set = false;
  void notify() noexcept {
    { std::lock_guard lock(mutex); set = true; }
    changed.notify_all();
  }
  bool wait(std::chrono::milliseconds timeout = 5s) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, timeout, [this] { return set; });
  }
};
void require(Signal& signal, const char* name) {
  if (signal.wait()) return;
  std::printf("[FAIL] timed out at %s\n", name);
  std::fflush(stdout);
  std::_Exit(2);  // Do not destroy a service beneath a still-blocked test thread.
}

struct Prepared {
  G::Registration value;
  std::atomic<unsigned> calls{0};
  Signal entered;
  Signal* phase = nullptr;
};
void prepare(const G::Registration& next, void* context) noexcept {
  auto& prepared = *static_cast<Prepared*>(context);
  prepared.value = next;
  prepared.calls.fetch_add(1, std::memory_order_relaxed);
  prepared.entered.notify();
  if (prepared.phase) prepared.phase->notify();
}

void basicPublication() {
  G::Service service;
  check(!service.begin(), "unregistered service admits no delivery");
  Prepared prepared;
  check(service.publish(kA, prepare, &prepared) == G::PublishResult::Published &&
        prepared.calls.load() == 1 && same(prepared.value, kA),
        "prepare receives the complete tuple before publication");
  auto first = service.begin();
  check(first && same(first.registration(), kA), "delivery snapshots callback, context and native mode");
  G::DeliveryLease moved(std::move(first));
  G::DeliveryLease assigned;
  assigned = std::move(moved);
  check(!first && !moved && assigned && same(assigned.registration(), kA),
        "move construction and assignment transfer one delivery lease");
  check(service.publish({}, prepare, &prepared) == G::PublishResult::ReentrantUnregister &&
        prepared.calls.load() == 1,
        "reentrant unregister rejects before prepare or publication");
  auto unchanged = service.begin();
  check(unchanged && same(unchanged.registration(), kA), "rejected unregister leaves admission unchanged");
  unchanged = {};
  assigned = {};
  check(service.publish({}) == G::PublishResult::Published && !service.begin(),
        "releasing moved leases restores external unregister semantics");
  check(&G::registrationService() == &G::registrationService(), "global service has stable process lifetime identity");
}

void vdAndCpRetirement() {
  G::Service service;
  service.publish(kA);
  auto vd = service.begin();
  Signal cpEntered, releaseCp, returned;
  std::atomic<bool> cpTupleGood{false};
  std::thread cp([&] {
    auto lease = service.begin();
    cpTupleGood.store(lease && same(lease.registration(), kA));
    cpEntered.notify();
    require(releaseCp, "CP lease release");
    // The lease spans a simulated complete CPU-mask loop, not just its call.
    cpTupleGood.store(cpTupleGood.load() && same(lease.registration(), kA));
  });
  require(cpEntered, "CP lease acquisition");
  Prepared replacement;
  std::atomic<G::PublishResult> result{G::PublishResult::RetirementInProgress};
  std::thread setter([&] {
    result.store(service.publish(kB, prepare, &replacement));
    returned.notify();
  });
  require(replacement.entered, "replacement closes admission");
  check(!service.begin() && !returned.wait(30ms),
        "external replacement closes admission and waits for VD and CP leases");
  check(same(vd.registration(), kA) && cpTupleGood.load() && same(replacement.value, kB),
        "old VD and CP snapshots remain coherent through prepared replacement");
  vd = {};
  check(!service.begin() && !returned.wait(30ms),
        "retirement continues after VD returns while CP still owns its lease");
  releaseCp.notify();
  cp.join();
  require(returned, "replacement after both deliveries finish");
  setter.join();
  auto next = service.begin();
  check(result.load() == G::PublishResult::Published && next && same(next.registration(), kB) &&
        cpTupleGood.load(), "replacement reopens admission only after both leases drain");
}

void callbackReplacement() {
  G::Service service;
  service.publish(kA);
  auto old = service.begin();
  Prepared replacement;
  check(service.publish(kB, prepare, &replacement) == G::PublishResult::Published &&
        same(old.registration(), kA), "callback nonzero re-registration keeps the old delivery pinned");
  auto current = service.begin();
  check(current && same(current.registration(), kB), "new delivery sees the callback replacement tuple");
  Prepared unregister;
  Signal returned;
  std::atomic<G::PublishResult> result{G::PublishResult::RetirementInProgress};
  std::thread setter([&] {
    result.store(service.publish({}, prepare, &unregister));
    returned.notify();
  });
  require(unregister.entered, "unregister after callback replacement");
  check(!service.begin() && !returned.wait(30ms),
        "external unregister drains snapshots from both old and new registrations");
  current = {};
  check(!returned.wait(30ms) && same(old.registration(), kA),
        "old callback snapshot still prevents unregister after newer delivery ends");
  old = {};
  require(returned, "old callback lease retirement");
  setter.join();
  check(result.load() == G::PublishResult::Published && !service.begin(),
        "unregister completes after every retained snapshot releases");
}

void serializedSettersAndRejection() {
  G::Service service;
  service.publish(kA);
  auto callback = service.begin();
  Prepared firstPrepared, secondPrepared, rejectedPrepared;
  Signal firstReturned, secondReturned;
  std::thread first([&] {
    service.publish(kB, prepare, &firstPrepared);
    firstReturned.notify();
  });
  require(firstPrepared.entered, "first external setter retirement");
  std::thread second([&] {
    service.publish(kC, prepare, &secondPrepared);
    secondReturned.notify();
  });
  check(!secondPrepared.entered.wait(30ms) && !secondReturned.wait(30ms),
        "second external setter cannot prepare or return during active retirement");
  check(service.publish(kC, prepare, &rejectedPrepared) == G::PublishResult::RetirementInProgress &&
        rejectedPrepared.calls.load() == 0 && !service.begin() && same(callback.registration(), kA),
        "callback replacement during retirement rejects without preparing or reopening admission");
  check(service.publish({}, prepare, &rejectedPrepared) == G::PublishResult::ReentrantUnregister &&
        rejectedPrepared.calls.load() == 0,
        "callback unregister during retirement also rejects before any mutation");
  callback = {};
  require(firstReturned, "first external setter completion");
  require(secondReturned, "serialized external setter completion");
  first.join();
  second.join();
  auto final = service.begin();
  check(firstPrepared.calls.load() == 1 && secondPrepared.calls.load() == 1 &&
        same(firstPrepared.value, kB) && same(secondPrepared.value, kC) &&
        final && same(final.registration(), kC), "serialized setters preserve complete tuple order");
}

void separateServiceRetirement(bool unregister) {
  G::Service serviceA, serviceB;
  serviceA.publish(kA);
  serviceB.publish(kB);
  auto heldB = serviceB.begin();
  Signal phase, returned;
  Prepared prepared;
  prepared.phase = &phase;
  std::atomic<bool> aTupleGood{false};
  std::atomic<G::PublishResult> result{G::PublishResult::RetirementInProgress};
  std::thread publisher([&] {
    auto heldA = serviceA.begin();
    aTupleGood.store(heldA && same(heldA.registration(), kA));
    result.store(serviceB.publish(unregister ? G::Registration{} : kC, prepare, &prepared));
    aTupleGood.store(aTupleGood.load() && same(heldA.registration(), kA));
    returned.notify();
    phase.notify();  // Also wakes the control if a defect rejects before prepare.
  });
  require(phase, "cross-service publication begins or rejects");
  check(prepared.calls.load() == 1 && !returned.wait(30ms), unregister ?
        "holding A while unregistering B externally waits for B's other-thread lease" :
        "holding A while replacing B externally waits for B's other-thread lease");
  auto blockedB = serviceB.begin();
  check(!blockedB && same(heldB.registration(), kB), unregister ?
        "cross-service unregister closes B admission and preserves its leased tuple" :
        "cross-service replacement closes B admission and preserves its leased tuple");
  blockedB = {};
  heldB = {};
  require(returned, "cross-service retirement after B releases");
  publisher.join();
  auto finalB = serviceB.begin();
  check(result.load() == G::PublishResult::Published && aTupleGood.load() &&
        (unregister ? !finalB : finalB && same(finalB.registration(), kC)), unregister ?
        "cross-service unregister is external and completes after B drains" :
        "cross-service replacement completes after B drains without changing A");
}
}  // namespace

int runGraphicsInterruptRegistrationTest(const char* mode) {
  if (mode && std::strcmp(mode, "native") != 0) {
    std::fprintf(stderr, "graphics interrupt registration test: unsupported mode %s\n", mode);
    return 2;
  }
  passed = failed = 0;
  basicPublication();
  vdAndCpRetirement();
  callbackReplacement();
  serializedSettersAndRejection();
  separateServiceRetirement(false);
  separateServiceRetirement(true);
  std::printf("graphics interrupt registration: %d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
