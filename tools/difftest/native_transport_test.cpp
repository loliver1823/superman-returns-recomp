// Real SDK submission/park differential. Host fixture disables the ring drain;
// direct-route descriptors are compared without starting a GPU worker. Actual
// Kf imports execute; event/wait observers record their ordering and state.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "ppc/ppc_recomp_shared.h"
#include "guest/guest_harness.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/kpcr.hpp"
#include "kernel/native_transport.hpp"
#include "kernel/ab_toggle.hpp"
#include "kernel/native_memory.hpp"
#include "kernel/gpu_write_watch.hpp"
#include "gfx/resource_memory.hpp"  // memres::resourceMode (the native resource route, default ON since the bake)
#include "gfx/resource_ab.hpp"      // memres::resourceAbDual (the frozen-pass A/B keeps the tracker)
#include "kernel/guest_protect.hpp"
#include "kernel/audio_cmdq_obs.hpp"
#include "kernel/stubs.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>

extern "C" void __imp__sub_820FD800(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82110768(PPCContext& __restrict, uint8_t*);
namespace sr::kernel {  // kernel/image_write_watchdog.cpp (declared where used, as guest_harness.cpp does)
void installImageWriteWatchdog();
bool armRuntimeWriteWatch(uint32_t guestVA, uint32_t len);
void disarmRuntimeWriteWatch();
}
namespace K = sr::kernel;
namespace N = sr::native::transport;
namespace W = sr::kernel::gww;
namespace GP = sr::kernel::guest_protect;
namespace {
constexpr uint32_t dev = 0x70600000u, state = dev + 0x2A3Cu, list = dev + 0x32C8u;
constexpr uint32_t buffer = 0x90640000u, ring = 0x70660000u, fb = 0x70661000u;
constexpr uint32_t cell = 0x70662000u, stack = 0x70580000u, oldTail = buffer + 0xF00u;
int passed = 0, failed = 0;
using Event = std::array<uint32_t,7>;
std::vector<Event> events;
std::mutex eventsMutex;
K::ExportFn originalAcquire, originalRelease, originalSet, originalReset, originalWait;
bool parkBeforeAcquire = false, linkBeforeAcquire = false, realWait = false;
std::atomic<bool> waitEntered{false};
thread_local unsigned raceRole = 0;
unsigned pauseRole = 0, pauseBoundMs = 1000;
std::atomic<unsigned> acquisitionAttempts{0};
std::atomic<bool> acquisitionPaused{false}, acquisitionContinue{false}, scheduleValid{true};
uint32_t rd(uint32_t va) { return K::readGuestU32BE(va); }
void wr(uint32_t va,uint32_t value) { K::writeGuestBE32(va,value); }
void check(bool ok,const char* name) { std::printf("%s %s\n",ok ? "  ok " : "FAIL:",name); ok ? ++passed : ++failed; }
uint32_t irql() { return K::guestVAToHost(K::kpcrCurrent() + K::kKpcrOffIrql)[0]; }
void note(uint32_t kind,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0) {
  std::lock_guard<std::mutex> guard(eventsMutex);
  events.push_back({kind,a,b,c,d,rd(dev + 0x2A78u),irql()});
}
uint32_t acquire(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  if (raceRole) acquisitionAttempts.fetch_add(1,std::memory_order_release);
  if (parkBeforeAcquire) wr(dev + 0x2A74u,0u);
  if (linkBeforeAcquire) wr(oldTail,0xD0641000u);
  const uint32_t result = originalAcquire(a,b,c,d,e,f);
  note(1,a,result,rd(dev + 0x2A74u),rd(oldTail));
  if (raceRole && raceRole==pauseRole) {
    acquisitionPaused.store(true,std::memory_order_release);
    for (unsigned i=0; i<pauseBoundMs && !acquisitionContinue.load(std::memory_order_acquire); ++i) Sleep(1);
    if (!acquisitionContinue.load()) scheduleValid.store(false);
  }
  return result;
}
uint32_t release(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  note(2,a,b,rd(dev + 0x2A74u),rd(oldTail));
  return originalRelease(a,b,c,d,e,f);
}
uint32_t setEvent(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  note(3,a,b,c,rd(state + 0x30u)); return originalSet(a,b,c,d,e,f);
}
uint32_t resetEvent(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  note(4,a,rd(state + 0x30u)); return originalReset(a,b,c,d,e,f);
}
uint32_t waitEvent(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  note(5,a,b,c,d);
  note(6,e,rd(state),rd(state + 0x24u),rd(state + 0x34u));
  waitEntered.store(true,std::memory_order_release);
  return realWait ? originalWait(a,b,c,d,e,f) : 0u;
}
void fixture(uint32_t count) {
  std::memset(K::guestVAToHost(dev),0,0x5000u);
  std::memset(K::guestVAToHost(buffer),0xEE,0x2000u);
  std::memset(K::guestVAToHost(ring),0xCC,0x100u);
  std::memset(K::guestVAToHost(fb),0,0x100u);
  wr(0x82000948u,cell); wr(cell,dev);
  wr(dev + 0x2A74u,count); wr(dev + 0x2A10u,fb);
  wr(dev + 0x34A4u,ring); wr(dev + 0x34A8u,63u);
  wr(list,0x12345678u); wr(list + 4u,oldTail - 12u); wr(list + 8u,oldTail);
  wr(list + 12u,buffer + 0x1FFFu); wr(oldTail,0xC0000000u);
  K::guestVAToHost(K::kpcrCurrent() + K::kKpcrOffIrql)[0] = 0u;
  events.clear(); waitEntered.store(false,std::memory_order_relaxed);
}
PPCContext context() {
  PPCContext c{}; c.r1.u64 = stack; c.r13.u64 = K::kpcrCurrent();
  c.r14.u64 = 0xAABBCCDDEEFF0011ull; c.r31.u64 = 0x1122334455667788ull;
  return c;
}
struct Result {
  uint64_t returned, stack, r13, r14, r31;
  std::vector<uint8_t> device, bytes, ringBytes;
  std::vector<Event> calls;
  bool operator==(const Result&) const = default;
};
Result capture(const PPCContext& c) {
  return {c.r3.u64,c.r1.u64,c.r13.u64,c.r14.u64,c.r31.u64,
      {K::guestVAToHost(dev),K::guestVAToHost(dev)+0x5000u},
      {K::guestVAToHost(buffer),K::guestVAToHost(buffer)+0x2000u},
      {K::guestVAToHost(ring),K::guestVAToHost(ring)+0x100u},events};
}
Result append(bool oracle,uint32_t count,uint32_t increment,uint64_t at) {
  fixture(count); PPCContext c = context();
  c.r3.u64 = dev; c.r4.u64 = at; c.r5.u64 = 0x10641234u;
  c.r6.u64 = 37u; c.r7.u64 = increment; c.r8.u64 = list;
  if (oracle) __imp__sub_820FD800(c,K::getGuestBase());
  else PPC_LOOKUP_FUNC(K::getGuestBase(),0x820FD800u)(c,K::getGuestBase());
  return capture(c);
}
bool protectionIs(uint32_t va,DWORD protection) {
  MEMORY_BASIC_INFORMATION region{};
  return VirtualQuery(K::guestVAToHost(va),&region,sizeof(region)) &&
         region.State==MEM_COMMIT && region.Protect==protection;
}
// mem.resources bake (native/mem-native a1fda568, 2026-10-17): the development build's default is the native resource route,
// where the GPU write tracker is off (gww::enabled() is false) and no page is ever tracker-owned - only the page model
// (SR_MEM_RESOURCES=0, the oracle arm) calls protectForValidation. The tracker cases below test the page model; the
// runners pin SR_MEM_RESOURCES=0 for them. On the native route the native statement replaces them
// (docs/RELEASE_COMPOSITION.md 9.6, "the native producers' admission": without the tracker every guest page is writable
// - writableRange decides): the admission is writableRange || cpuWritableRange, and the tracker's half answers no.
bool nativeResourceRoute() { return sr::gfx::memres::resourceMode() && !sr::gfx::memres::resourceAbDual(); }
void nativeRouteAdmissionChecks(uint32_t va,uint32_t page) {
  check(!W::enabled(),"native route: the GPU write tracker is off (gww::enabled() false)");
  wr(va+0xFFCu,0x12345678u);
  check(sr::native::memory::writableRange(K::guestVAToHost(va+0xFFCu),8u) &&
        !W::cpuWritableRange(K::guestVAToHost(va+0xFFCu),8u),
        "native route: an ordinary writable range is admitted by writableRange; the tracker's half answers no");
  // A page protected through the tracker's own API (unreachable in production on this route) stays inadmissible: the
  // tracker-owned admission path does not exist here.
  W::protectForValidation(&page,1u); W::markValidated(page,8u);
  const bool owned=W::protGen(page)!=0u && protectionIs(va,PAGE_READONLY);
  check(owned && !W::cpuWritableRange(K::guestVAToHost(va+0xFFCu),8u) &&
        !sr::native::memory::writableRange(K::guestVAToHost(va+0xFFCu),8u),
        "native route: a page protected through the tracker's API is not admitted (writableRange decides)");
  wr(va+0xFFCu,0x12345678u);  // a store faults into the tracker's handler, which releases the page
  std::vector<uint32_t> released; W::takeReleasedBlocks(released);
  check(protectionIs(va,PAGE_READWRITE) && sr::native::memory::writableRange(K::guestVAToHost(va),16u),
        "native route: a store releases that page to ordinary writable");
  DWORD previous=0,ignored=0;
  const bool ro=VirtualProtect(K::guestVAToHost(va+0x1000u),4096u,PAGE_READONLY,&previous)!=0;
  check(ro && !sr::native::memory::writableRange(K::guestVAToHost(va+0xFFCu),8u) &&
        !W::cpuWritableRange(K::guestVAToHost(va+0xFFCu),8u),
        "native route: a range over an unowned read-only page declines (writableRange decides)");
  if (ro) VirtualProtect(K::guestVAToHost(va+0x1000u),4096u,previous,&ignored);
}
void trackedRangeChecks() {
  const uint32_t va=buffer+0x40000u, mirror=0xA0000000u|(va&0x1FFFFFFFu), page=(va&0x1FFFFFFFu)>>12;
  if (nativeResourceRoute()) { nativeRouteAdmissionChecks(va,page); return; }
  check(W::enabled() && !W::cpuWritableRange(K::guestVAToHost(va),16u),
        "CPU-write query does not install the absent tracker");
  wr(va+0xFFCu,0x12345678u);
  W::protectForValidation(&page,1u); W::markValidated(page,8u);
  check(W::protGen(page) && W::clean(page,8u) && protectionIs(va,PAGE_READONLY) &&
        protectionIs(mirror,PAGE_READONLY),"real tracker protects and validates both physical copies");
  const uint32_t gen=W::protGen(page);
  const uint64_t clock=W::releaseClock(), faults=W::stats().faults;
  check(W::cpuWritableRange(K::guestVAToHost(va+0xFFCu),8u) &&
        W::cpuWritableRange(K::guestVAToHost(mirror+0xFFCu),8u) &&
        !sr::native::memory::writableRange(K::guestVAToHost(va+0xFFCu),8u),
        "tracker-owned read-only and ordinary writable mixed ranges admit in both copies");
  check(W::protGen(page)==gen && W::clean(page,8u) && W::releaseClock()==clock &&
        W::stats().faults==faults && rd(va+0xFFCu)==0x12345678u,
        "CPU-write admission queries preserve protection generation and dirty state");
  check(!W::cpuWritableRange(nullptr,4u) && !W::cpuWritableRange(K::guestVAToHost(va),0u) &&
        !W::cpuWritableRange(reinterpret_cast<void*>(UINTPTR_MAX-1u),4u) &&
        !W::cpuWritableRange(K::guestVAToHost(dev),4u) &&
        !W::cpuWritableRange(K::getGuestBase()+0xBFFFFFFEu,4u),
        "CPU-write query refuses empty overflow and ranges outside tracked windows");
  DWORD previous=0,ignored=0;
  const bool ro=VirtualProtect(K::guestVAToHost(va+0x1000u),4096u,PAGE_READONLY,&previous)!=0;
  check(ro,"unowned read-only mixed-range fixture protection succeeds");
  if (!ro) return;
  check(!W::protGen(page+1u) && !W::cpuWritableRange(K::guestVAToHost(va+0xFFCu),8u) &&
        W::protGen(page)==gen && protectionIs(va+0x1000u,PAGE_READONLY),
        "mixed tracked and unowned read-only range declines without releasing either page");
  check(VirtualProtect(K::guestVAToHost(va+0x1000u),4096u,previous,&ignored)!=0,
        "unowned read-only mixed-range fixture protection restores");
  for (DWORD protection : {DWORD(PAGE_READONLY|PAGE_GUARD),DWORD(PAGE_NOACCESS)}) {
    const bool denied=VirtualProtect(K::guestVAToHost(va),4096u,protection,&previous)!=0;
    check(denied,"owned denied-page fixture protection succeeds");
    if (!denied) return;
    check(!W::cpuWritableRange(K::guestVAToHost(va),16u) && W::protGen(page)==gen &&
          protectionIs(va,protection),"tracker ownership cannot admit guard or inaccessible pages");
    check(VirtualProtect(K::guestVAToHost(va),4096u,previous,&ignored)!=0,
          "owned denied-page fixture protection restores");
  }
  const bool decommitted=VirtualFree(K::guestVAToHost(va+0x1000u),4096u,MEM_DECOMMIT)!=0;
  check(decommitted,"mixed uncommitted-page fixture decommits successfully");
  if (!decommitted) return;
  check(!W::cpuWritableRange(K::guestVAToHost(va+0xFFCu),8u) && W::protGen(page)==gen,
        "mixed tracked and uncommitted range declines without release");
  check(VirtualAlloc(K::guestVAToHost(va+0x1000u),4096u,MEM_COMMIT,PAGE_READWRITE)!=nullptr,
        "mixed uncommitted-page fixture recommits successfully");
  wr(va+0xFFCu,0x87654321u);
  std::vector<uint32_t> released; W::takeReleasedBlocks(released);
  bool notified=false; for (uint32_t block : released) notified |= block==(page>>W::kBlockLog2);
  check(!W::protGen(page) && !W::clean(page,8u) && W::stats().faults>faults &&
        W::releaseClock()>clock && notified && protectionIs(va,PAGE_READWRITE) &&
        protectionIs(mirror,PAGE_READWRITE) && rd(va+0xFFCu)==0x87654321u,
        "ordinary CPU store releases real tracking and publishes dirty-block invalidation");
}
// ---- The tracker path (2026-10-14, option A). The arithmetic admission lets the pair store into pages the GPU write
// tracker made read-only; such a store must take exactly the path a guest store takes: it faults into the tracker's
// handler, which releases the block (both physical copies) and publishes the dirty-block invalidation. Each fixture
// protects and validates pages the way the CP's shared-memory validation does and runs the actual SDK body, the
// production dispatch and the native entry directly; the tracker's view of the call must be the SDK's.
enum class Call { Sdk, Dispatch, Direct };
struct TrackerView {
  uint64_t faults, faultPages, hostWrites;
  std::vector<uint32_t> released;  // blocks published as released by the call
  std::vector<uint32_t> pages;     // per page: 1 owned (protGen), 2 validated (clean), 4 0x8 copy RO, 8 0xA copy RO
  bool operator==(const TrackerView&) const = default;
};
uint32_t pageBits(uint32_t page) {
  return (W::protGen(page) ? 1u : 0u) | (W::clean(page,8u) ? 2u : 0u) |
         (protectionIs(0x80000000u|(page<<12),PAGE_READONLY) ? 4u : 0u) |
         (protectionIs(0xA0000000u|(page<<12),PAGE_READONLY) ? 8u : 0u);
}
// Protect and validate pages[0..n) as the CP's validation does; true when the fixture owns them read-only.
bool trackPages(const uint32_t* pages,uint32_t n) {
  for (unsigned i=0;i<64u;++i) W::beginFrame();  // past the cooldown of every block that faulted before
  W::protectForValidation(pages,n);
  bool owned=true;
  for (uint32_t i=0;i<n;++i) { W::markValidated(pages[i],8u); owned &= pageBits(pages[i])==15u; }
  std::vector<uint32_t> pending; W::takeReleasedBlocks(pending);  // the call starts with no release pending
  return owned;
}
TrackerView trackerView(const W::Stats& before,const uint32_t* pages,uint32_t n) {
  const W::Stats now=W::stats();
  TrackerView v{now.faults-before.faults,now.faultPages-before.faultPages,now.hostWrites-before.hostWrites,{},{}};
  W::takeReleasedBlocks(v.released);
  for (uint32_t i=0;i<n;++i) v.pages.push_back(pageBits(pages[i]));
  return v;
}
// Every protected page released by the handler (not by a host-write scope) and its block published.
bool releasedByHandler(const TrackerView& v) {
  bool all=v.faults>=1u && v.hostWrites==0u && !v.released.empty();
  for (uint32_t bits : v.pages) all &= bits==0u;
  return all;
}
struct Tracked { Result result; TrackerView view; bool admitted; };
// kind 0: the new node's page is tracked; 1: the parked tail's page; 2: a node crossing two tracked pages.
Tracked trackedAppend(Call how,unsigned kind) {
  fixture(1u);
  const uint32_t previousTail=buffer+0x1F00u;
  wr(list+8u,previousTail); wr(previousTail,0xC0000000u);
  PPCContext c=context(); c.r3.u64=dev; c.r4.u64=buffer+(kind==2u ? 0xFF8u : 0x100u);
  c.r5.u64=0x10641234u; c.r6.u64=37u; c.r7.u64=1u; c.r8.u64=list;
  const uint32_t firstPage=(buffer&0x1FFFFFFFu)>>12;
  const uint32_t pages[]={firstPage+(kind==1u ? 1u : 0u),firstPage+1u};
  const uint32_t count=kind==2u ? 2u : 1u;
  check(trackPages(pages,count),"tracked append fixture owns actual read-only physical pages");
  const W::Stats before=W::stats();
  bool admitted=true;
  if (how==Call::Sdk) __imp__sub_820FD800(c,K::getGuestBase());
  else if (how==Call::Direct) {
    admitted=N::appendJumpSegment(c,K::getGuestBase());
    check(admitted==N::gateOn(),"tracked append obeys native admission gate before SDK fallback");
    if (!admitted) __imp__sub_820FD800(c,K::getGuestBase());
  } else PPC_LOOKUP_FUNC(K::getGuestBase(),0x820FD800u)(c,K::getGuestBase());
  Tracked t{capture(c),trackerView(before,pages,count),admitted};
  check(releasedByHandler(t.view),"SDK and native append stores release tracked outputs and invalidate validated contents");
  return t;
}
// EndSync with its state block on a tracked page: state = buffer + 0x1FC4 puts every store of the park (state +0,
// +0x24, +0x30, +0x34, +0x38) on the tracked page and the lock word (+0x3C) and event (+0x40) on the next, untracked
// page, so only the entry's own stores can release it (the imports' stores cannot). The marker's page is tracked too:
// a follow only reads it. terminal: park (stores) or follow a link (no store).
Tracked trackedFinish(Call how,bool terminal) {
  fixture(5u);
  const uint32_t ps=buffer+0x1FC4u;
  std::memset(K::guestVAToHost(ps),0,0x60u);
  wr(oldTail,terminal ? 0xC0000000u : 0xD0641000u);
  wr(ps,1u); wr(ps+0x30u,1u); wr(ps+0x34u,1u); wr(ps+0x38u,5u);
  PPCContext c=context(); c.r3.u64=ps; c.r4.u64=oldTail;
  const uint32_t firstPage=(buffer&0x1FFFFFFFu)>>12;
  const uint32_t pages[]={firstPage,firstPage+1u};
  check(trackPages(pages,2u),"tracked EndSync fixture owns actual read-only physical pages");
  const W::Stats before=W::stats();
  bool admitted=true;
  if (how==Call::Sdk) __imp__sub_82110768(c,K::getGuestBase());
  else if (how==Call::Direct) {
    admitted=N::endSync(c,K::getGuestBase());
    if (!admitted) __imp__sub_82110768(c,K::getGuestBase());
  } else PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110768u)(c,K::getGuestBase());
  Result r=capture(c);
  // the state block itself (the capture's buffer window ends at buffer + 0x2000)
  r.bytes.insert(r.bytes.end(),K::guestVAToHost(ps),K::guestVAToHost(ps)+0x60u);
  Tracked t{r,trackerView(before,pages,2u),admitted};
  if (terminal)
    check(releasedByHandler(t.view),"SDK and native EndSync park stores release the tracked state and invalidate it");
  else
    check(t.view.faults==0u && t.view.released.empty() && t.view.pages==std::vector<uint32_t>{15u,15u},
          "SDK and native EndSync follow reads tracked pages without releasing them");
  return t;
}
// A native entry that must decline before any import, read-modify-write or store: the caller context, device, buffer
// and ring bytes unchanged and no import observed (in gate-off every entry declines at its gate).
bool declinesClean(PPCContext c,bool end) {
  const Result before=capture(c);
  const bool admitted=end ? N::endSync(c,K::getGuestBase()) : N::appendJumpSegment(c,K::getGuestBase());
  return !admitted && capture(c)==before && events.empty();
}
PPCContext appendContext(uint32_t at) {
  PPCContext c=context(); c.r3.u64=dev; c.r4.u64=at; c.r5.u64=0x10641234u; c.r6.u64=37u; c.r7.u64=0u; c.r8.u64=list;
  return c;
}
PPCContext endContext(uint32_t s,uint32_t marker) { PPCContext c=context(); c.r3.u64=s; c.r4.u64=marker; return c; }
void endFixture(uint32_t count) { fixture(count); wr(state,1u); wr(state+0x30u,1u); wr(state+0x34u,1u); }
// The arithmetic admission's ranges (2026-10-14, option A; these replace the read-only / guard / no-access decline
// fixtures of the queried admission): a range must map contiguously - a range crossing the 0xC0000000 alias fold
// declines - and lie inside the registered guest arena (re-registered smaller, higher or absent here; restored after).
void rangeChecks(bool nativeEnabled) {
  // Both sides of the fold are zeroed first (and the crossing device's count word set), so a defect that admits a
  // crossing range takes a benign route - a free lock word, a chained list - and fails by name instead of hanging.
  auto fold=[](uint32_t count) {
    endFixture(1u);
    std::memset(K::guestVAToHost(0x80000000u),0,0x1000u); std::memset(K::guestVAToHost(0xBFFFF000u),0,0x1000u);
    wr(0xBFFFFFFCu,count);
  };
  fold(0u);
  check(declinesClean(appendContext(0xBFFFFFF8u),false),
        "append node crossing the 0xC0000000 alias fold declines before imports and writes");
  fold(0u);
  { PPCContext c=appendContext(buffer+0x100u); c.r8.u64=0xBFFFFFF8u;
    check(declinesClean(c,false),"append list crossing the 0xC0000000 alias fold declines before imports and writes"); }
  fold(1u);
  { PPCContext c=appendContext(buffer+0x100u); c.r3.u64=0xBFFFD588u;  // dev+0x2A74 = 0xBFFFFFFC, 8 bytes
    check(declinesClean(c,false),"append device word crossing the alias fold declines before imports and writes"); }
  fold(0u);
  { PPCContext c=appendContext(buffer+0x100u); c.r1.u64=0xC0000800u;  // stack window 0xBFFFF800..0xC00007FF
    check(declinesClean(c,false),"append stack window crossing the alias fold declines before imports and writes"); }
  fold(0u);
  check(declinesClean(endContext(0xBFFFFFE0u,oldTail),true),
        "EndSync state block crossing the alias fold declines before imports and writes");
  uint8_t* const base=K::getGuestBase();
  const size_t size=K::getGuestSize();
  const N::Counters before=N::counters();
  K::registerExCreateThreadRunner(nullptr,base,0x70000000u);  // device, list and state above the registered arena
  endFixture(1u);
  const bool above=declinesClean(appendContext(buffer+0x100u),false) && declinesClean(endContext(state,oldTail),true);
  K::registerExCreateThreadRunner(nullptr,base+0x90000000ull,0x10000000u);  // all of them below it
  endFixture(1u);
  const bool below=declinesClean(appendContext(buffer+0x100u),false) && declinesClean(endContext(state,oldTail),true);
  K::registerExCreateThreadRunner(nullptr,nullptr,0u);  // no arena registered
  endFixture(1u);
  const bool none=declinesClean(appendContext(buffer+0x100u),false) && declinesClean(endContext(state,oldTail),true);
  K::registerExCreateThreadRunner(nullptr,base,size);
  const N::Counters after=N::counters();
  check(above && below && none && after.append==before.append &&
        (!nativeEnabled || (after.appendInput==before.appendInput+3u && after.endState==before.endState+3u)),
        "ranges above, below or without the registered guest arena decline before imports and writes");
  // The registered arena's last byte is admitted (the bound is exact); a chain tail one page past it declines under
  // the lock, publishing nothing, with the lock and IRQL restored.
  K::registerExCreateThreadRunner(nullptr,base,0x90641000u);
  fixture(0u);
  PPCContext edge=appendContext(0x90640FF0u);
  const N::Counters edge0=N::counters();
  const bool edgeAdmitted=N::appendJumpSegment(edge,K::getGuestBase());
  const N::Counters edge1=N::counters();
  fixture(1u); wr(list+8u,buffer+0x1F00u); wr(buffer+0x1F00u,0xC0000000u);
  PPCContext tail=appendContext(buffer+0x100u);
  const Result tailBefore=capture(tail);
  const bool tailAdmitted=N::appendJumpSegment(tail,K::getGuestBase());
  Result tailAfter=capture(tail); tailAfter.calls.clear();
  K::registerExCreateThreadRunner(nullptr,base,size);
  check(edgeAdmitted==nativeEnabled && edge1.direct==edge0.direct+(nativeEnabled ? 1u : 0u),
        "a node ending at the registered arena's last byte is admitted");
  check(!tailAdmitted && tailAfter==tailBefore && events.size()==(nativeEnabled ? 2u : 0u) &&
        rd(dev+0x2A78u)==0u && irql()==0u && rd(buffer+0x1F00u)==0xC0000000u,
        "chain tail outside the registered arena declines under the lock with lock and IRQL restored");
}
Result finish(bool oracle,uint32_t marker,uint32_t nesting,bool paused,
              bool readOnly=false,bool directAdmission=false) {
  fixture(5u); wr(oldTail,marker); wr(state,1u);
  wr(state + 0x30u,nesting); wr(state + 0x34u,1u);
  wr(state + 0x50u,paused ? buffer + 0x100u : 0u);
  PPCContext c = context(); c.r3.u64 = state; c.r4.u64 = oldTail;
  DWORD previous=0;
  if (readOnly) {
    const bool protectedOk=VirtualProtect(K::guestVAToHost(oldTail),4u,PAGE_READONLY,&previous)!=0;
    check(protectedOk,"readable-only marker fixture protection operation succeeds");
    if (!protectedOk) return capture(c);
  }
  if (oracle) __imp__sub_82110768(c,K::getGuestBase());
  else if (directAdmission) {
    const bool admitted=N::endSync(c,K::getGuestBase());
    check(admitted==N::gateOn(),"readable-only EndSync marker obeys native admission gate before SDK fallback");
    if (!admitted) __imp__sub_82110768(c,K::getGuestBase());
  }
  else PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110768u)(c,K::getGuestBase());
  if (readOnly) {
    DWORD ignored=0;
    check(VirtualProtect(K::guestVAToHost(oldTail),4u,previous,&ignored)!=0,
          "readable-only marker fixture protection is restored successfully");
  }
  return capture(c);
}
bool race(bool oracle,bool producerFirst) {
  fixture(1u); wr(state,1u); wr(state+0x30u,1u); wr(state+0x34u,1u);
  uint8_t* const guestBase = K::getGuestBase();
  acquisitionAttempts.store(0); acquisitionPaused.store(false); acquisitionContinue.store(false);
  scheduleValid.store(true); pauseRole = producerFirst ? 1u : 2u;
  uint64_t producerReturn=0, consumerReturn=0;
  auto producer = [&] {
    raceRole=1;
    K::setGuestMemory(guestBase,0x100000000ull);
    K::kpcrBindCurrent(stack-0x20000u,stack-0x30000u,1);
    PPCContext c=context(); c.r1.u64=stack-0x20000u;
    c.r3.u64=dev; c.r4.u64=buffer+0x100u; c.r5.u64=0x10641234u;
    c.r6.u64=37u; c.r7.u64=0u; c.r8.u64=list;
    if (oracle) __imp__sub_820FD800(c,K::getGuestBase());
    else PPC_LOOKUP_FUNC(K::getGuestBase(),0x820FD800u)(c,K::getGuestBase());
    producerReturn=c.r3.u64;
  };
  auto consumer = [&] {
    raceRole=2;
    K::setGuestMemory(guestBase,0x100000000ull);
    K::kpcrBindCurrent(stack-0x40000u,stack-0x50000u,2);
    PPCContext c=context(); c.r1.u64=stack-0x40000u; c.r3.u64=state; c.r4.u64=oldTail;
    if (oracle) __imp__sub_82110768(c,K::getGuestBase());
    else PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110768u)(c,K::getGuestBase());
    consumerReturn=c.r3.u64;
  };
  std::thread first([&] { if (producerFirst) producer(); else consumer(); });
  for (unsigned i=0; i<1000 && !acquisitionPaused.load(std::memory_order_acquire); ++i) Sleep(1);
  if (!acquisitionPaused.load()) scheduleValid.store(false);
  std::thread second([&] { if (producerFirst) consumer(); else producer(); });
  for (unsigned i=0; i<1000 && acquisitionAttempts.load(std::memory_order_acquire)<2u; ++i) Sleep(1);
  if (acquisitionAttempts.load()<2u) scheduleValid.store(false);
  acquisitionContinue.store(true,std::memory_order_release);
  first.join(); second.join(); pauseRole=0;
  const bool unlocked = rd(dev+0x2A78u)==0u && irql()==0u;
  if (producerFirst)
    return scheduleValid && unlocked && producerReturn==buffer+0x110u && consumerReturn==0xD0640104u &&
           rd(oldTail)==0xD0640100u && rd(state+0x30u)==1u && rd(state+0x38u)==1u;
  return scheduleValid && unlocked && producerReturn==buffer+0x100u && consumerReturn==0u &&
         rd(oldTail)==0xC0000000u && rd(state+0x30u)==0u && rd(state+0x38u)==0u &&
         rd(ring)==0xC0013F00u && rd(ring+4u)==0x10641234u && rd(ring+8u)==37u;
}

// ---- The per-present A/B (mode "ab": SR_NATIVE_TRANSPORT=1, SR_AB_TOGGLE=SR_NATIVE_TRANSPORT, SR_AB_PRESENT=1).
// The real kernel/ab_toggle flips the mask at each present (onGuestSwap, as VdSwap does): odd presents the SDK arm,
// even the native arm. Each entry reads the window once at its top. Both arms must equal the actual SDK, the census
// counters name the arm that ran, and a sequence whose calls fall in alternating presents equals the all-SDK one.
int abBit = -1;
bool selectArm(bool native) {
  for (int i=0; i<2 && sr::abt::on(abBit)!=native; ++i) sr::abt::onGuestSwap();
  return sr::abt::on(abBit)==native;
}
bool sameNativeCounts(const N::Counters& a,const N::Counters& b) {
  return a.append==b.append && a.chain==b.chain && a.direct==b.direct && a.park==b.park &&
         a.resume==b.resume && a.decline==b.decline;
}
void abEntries() {
  for (bool native : {false,true}) {
    check(selectArm(native),native ? "A/B: an even present selects the native arm" :
                                     "A/B: an odd present selects the SDK arm");
    for (uint32_t count : {0u,1u,3u}) for (uint32_t inc : {0u,1u}) {
      const Result expected=append(true,count,inc,buffer+0x100u);
      const N::Counters before=N::counters();
      const Result actual=append(false,count,inc,buffer+0x100u);
      const N::Counters after=N::counters();
      check(actual==expected,native ? "A/B ON present: append dispatch equals the actual SDK" :
                                      "A/B OFF present: append dispatch equals the actual SDK");
      if (native)
        check(after.append==before.append+1u && (count ? after.chain==before.chain+1u : after.direct==before.direct+1u) &&
              after.abOffAppend==before.abOffAppend && after.abOffEnd==before.abOffEnd && after.decline==before.decline,
              "A/B ON present: the native append ran (its route counted, no OFF call, no decline)");
      else
        check(sameNativeCounts(after,before) && after.abOffAppend==before.abOffAppend+1u && after.abOffEnd==before.abOffEnd,
              "A/B OFF present: append ran the actual SDK body whole (native counters unchanged, the OFF call counted)");
    }
    for (uint32_t marker : {0xD0641000u,0xC0000000u}) {
      const Result expected=finish(true,marker,1u,false);
      const N::Counters before=N::counters();
      const Result actual=finish(false,marker,1u,false);
      const N::Counters after=N::counters();
      check(actual==expected,native ? "A/B ON present: EndSync dispatch equals the actual SDK" :
                                      "A/B OFF present: EndSync dispatch equals the actual SDK");
      if (native)
        check((marker==0xC0000000u ? after.park==before.park+1u : after.resume==before.resume+1u) &&
              after.abOffEnd==before.abOffEnd && after.abOffAppend==before.abOffAppend && after.decline==before.decline,
              "A/B ON present: the native EndSync ran (park or follow counted, no OFF call, no decline)");
      else
        check(sameNativeCounts(after,before) && after.abOffEnd==before.abOffEnd+1u && after.abOffAppend==before.abOffAppend,
              "A/B OFF present: EndSync ran the actual SDK body whole (native counters unchanged, the OFF call counted)");
    }
    if (native) continue;
    // The entries themselves in an OFF window: nothing read, imported or written before the SDK fallback.
    fixture(1u); PPCContext c=context(); c.r3.u64=dev; c.r4.u64=buffer+0x100u;
    c.r5.u64=0x10641234u; c.r6.u64=37u; c.r7.u64=1u; c.r8.u64=list;
    Result before=capture(c);
    check(!N::appendJumpSegment(c,K::getGuestBase()) && capture(c)==before && events.empty(),
          "A/B OFF present: the native append entry falls back before any import or write");
    fixture(1u); wr(state,1u); wr(state+0x30u,1u); wr(state+0x34u,1u);
    c=context(); c.r3.u64=state; c.r4.u64=oldTail;
    before=capture(c);
    check(!N::endSync(c,K::getGuestBase()) && capture(c)==before && events.empty(),
          "A/B OFF present: the native EndSync entry falls back before any import or write");
  }
}
// One software list across presents: the kick's direct publication, closes chained while the pump is busy, the pump
// following links, parking at a terminal (nesting 2 -> 1), resuming at its park position after a later link, parking
// again with dev+0x2A74 drained (nesting 1 -> 0), then the next kick's direct publication and a chain onto it.
struct SeqStep { bool end; uint32_t at, increment; };
const SeqStep kSequence[] = {
    {false,buffer+0x100u,1u}, {false,buffer+0x200u,0u}, {true,oldTail,0u},
    {false,buffer+0x300u,0u}, {true,buffer+0x20Cu,0u}, {true,buffer+0x30Cu,0u},
    {false,buffer+0x400u,0u}, {true,buffer+0x30Cu,0u}, {true,buffer+0x40Cu,0u},
    {false,buffer+0x500u,1u}, {false,buffer+0x600u,0u}};
struct SeqResult { Result snapshot; std::vector<uint64_t> returns; };
// ab: false = the all-SDK reference (the actual bodies called directly); true = production dispatch with step k in
// the present of arm ((k + phase) & 1) (1 = native).
SeqResult sequence(bool ab,unsigned phase) {
  fixture(0u); wr(state,1u); wr(state+0x30u,2u); wr(state+0x34u,1u);
  SeqResult r;
  for (size_t k=0; k<std::size(kSequence); ++k) {
    const SeqStep& s=kSequence[k];
    if (ab) selectArm(((k+phase)&1u)!=0u);
    PPCContext c=context();
    if (s.end) { c.r3.u64=state; c.r4.u64=s.at; }
    else {
      c.r3.u64=dev; c.r4.u64=s.at; c.r5.u64=0x10641234u+uint32_t(k)*0x40u;
      c.r6.u64=37u+uint32_t(k); c.r7.u64=s.increment; c.r8.u64=list;
    }
    const uint32_t entry=s.end ? 0x82110768u : 0x820FD800u;
    if (!ab) (s.end ? __imp__sub_82110768 : __imp__sub_820FD800)(c,K::getGuestBase());
    else PPC_LOOKUP_FUNC(K::getGuestBase(),entry)(c,K::getGuestBase());
    r.returns.push_back(c.r3.u64);
  }
  r.snapshot=capture(context());
  r.snapshot.returned=0u;
  return r;
}
void abSequence() {
  const SeqResult reference=sequence(false,0u);
  // The reference must actually take every route: directs and chains, follows and both parks.
  check(reference.returns.size()==std::size(kSequence) && reference.returns[0]==buffer+0x100u &&
        reference.returns[1]==buffer+0x210u && reference.returns[5]==0u && reference.returns[8]==0u &&
        reference.returns[9]==buffer+0x500u && reference.returns[10]==buffer+0x610u && rd(dev+0x2A74u)==1u &&
        rd(state+0x30u)==0u,"A/B mixed presents: the all-SDK reference takes the direct, chain, follow and park routes");
  for (unsigned phase : {0u,1u}) {
    unsigned onAppend=0, onEnd=0, offAppend=0, offEnd=0;
    for (size_t k=0; k<std::size(kSequence); ++k) {
      const bool native=((k+phase)&1u)!=0u;
      (kSequence[k].end ? (native ? onEnd : offEnd) : (native ? onAppend : offAppend))++;
    }
    const N::Counters before=N::counters();
    const SeqResult mixed=sequence(true,phase);
    const N::Counters after=N::counters();
    check(mixed.snapshot==reference.snapshot,
          "A/B mixed presents: device, software list, ring and import sequence equal the all-SDK reference");
    check(mixed.returns==reference.returns,"A/B mixed presents: every call's full-width return equals the all-SDK reference");
    check(after.append-before.append==onAppend && after.park+after.resume-before.park-before.resume==onEnd &&
          after.abOffAppend-before.abOffAppend==offAppend && after.abOffEnd-before.abOffEnd==offEnd &&
          after.decline==before.decline,"A/B mixed presents: each call ran in its present's arm (native and OFF counts)");
  }
}

// ---- Foreign page protection (kernel/guest_protect.hpp; 2026-10-14, option A). Mode "exclusion" (SR_NATIVE_TRANSPORT=1,
// no diagnostic requested) runs in one process because declarations are permanent: a declared range first (the
// entries refuse every range touching it, admit the bytes next to it, and never write a read-only declared page),
// then the real image-write watchdog's always-on boot page (a declared range), then its runtime arm (a protection
// that excludes everything: every entry declines, counted, and production dispatch runs the SDK body), then the audio
// null-page protection's declaration. Mode "exclusion-env" runs with one page-protecting diagnostic requested in the
// environment (the runner sets each in turn): every entry declines from the first call, before any protector runs.
constexpr uint32_t foreign=0x906C0000u, watchPage=0x906D0000u, zeroPage=0x906E0000u;
constexpr uint32_t bootPage=0x8209A000u;  // the watchdog's default target 0x8209A458
std::vector<uint8_t> pageBytes(uint32_t va) { return {K::guestVAToHost(va),K::guestVAToHost(va)+0x1000u}; }
// The four entry routes - direct and chained append, parking and following EndSync - each called on the native entry
// directly: true when all decline before imports and writes, each counted as an exclusion decline.
bool allRoutesExcluded() {
  bool all=true;
  for (unsigned route=0;route<4u;++route) {
    const N::Counters before=N::counters();
    bool clean;
    if (route<2u) { endFixture(route); clean=declinesClean(appendContext(buffer+0x100u),false); }
    else { endFixture(1u); wr(oldTail,route==2u ? 0xC0000000u : 0xD0641000u);
           clean=declinesClean(endContext(state,oldTail),true); }
    const N::Counters after=N::counters();
    all &= clean && after.exclusion==before.exclusion+1u && after.decline==before.decline+1u &&
           after.append==before.append && after.park==before.park && after.resume==before.resume;
  }
  return all;
}
// Production dispatch while excluded: each route falls back to the actual SDK body whole.
bool dispatchEqualsSdk() {
  bool all=true;
  for (uint32_t count : {0u,1u}) all &= append(false,count,1u,buffer+0x100u)==append(true,count,1u,buffer+0x100u);
  for (uint32_t marker : {0xC0000000u,0xD0641000u}) all &= finish(false,marker,1u,false)==finish(true,marker,1u,false);
  return all;
}
void exclusionMode() {
  check(N::gateOn() && !GP::diagnosticRequested() && !GP::excludesAll() && GP::declarations()==0u,
        "exclusion: gate on, no diagnostic requested and nothing declared at start");
  {
    const N::Counters before=N::counters();
    const bool equal=append(false,1u,0u,buffer+0x100u)==append(true,1u,0u,buffer+0x100u) &&
                     finish(false,0xC0000000u,1u,false)==finish(true,0xC0000000u,1u,false);
    const N::Counters after=N::counters();
    check(equal && after.chain==before.chain+1u && after.park==before.park+1u && after.decline==before.decline,
          "exclusion: before any declaration the native append and EndSync run and equal the SDK");
  }
  GP::declare(K::guestVAToHost(foreign),0x1000u,GP::Scope::Range);
  check(!GP::excludesAll() && GP::declarations()==1u && GP::intersects(K::guestVAToHost(foreign+0xFFCu),4u) &&
        !GP::intersects(K::guestVAToHost(foreign-16u),16u) && !GP::intersects(K::guestVAToHost(foreign+0x1000u),4u),
        "declared range: a page declaration covers exactly that page");
  bool rangeHeld=true;
  {
    endFixture(0u);
    const N::Counters before=N::counters();
    const bool clean=declinesClean(appendContext(foreign+0x100u),false);
    const N::Counters after=N::counters();
    const bool ok=clean && after.foreignRange==before.foreignRange+1u && after.appendInput==before.appendInput+1u;
    check(ok,"declared range: an append whose node lies in it declines before imports and writes");
    rangeHeld &= ok;
  }
  {
    const N::Counters before=N::counters();
    const Result expected=append(true,1u,0u,foreign-16u);
    const std::vector<uint8_t> sdkNode(K::guestVAToHost(foreign-16u),K::guestVAToHost(foreign));
    std::memset(K::guestVAToHost(foreign-16u),0,16u);
    const Result actual=append(false,1u,0u,foreign-16u);
    const std::vector<uint8_t> nativeNode(K::guestVAToHost(foreign-16u),K::guestVAToHost(foreign));
    const N::Counters after=N::counters();
    check(actual==expected && nativeNode==sdkNode && after.chain==before.chain+1u && after.decline==before.decline,
          "declared range: a node ending at its first byte is admitted and equals the SDK");
  }
  {
    fixture(1u); wr(list+8u,foreign+0x20u); wr(foreign+0x20u,0xC0000000u);
    PPCContext t=appendContext(buffer+0x100u);
    const N::Counters before=N::counters();
    const Result tailBefore=capture(t);
    const bool admitted=N::appendJumpSegment(t,K::getGuestBase());
    Result tailAfter=capture(t); tailAfter.calls.clear();
    const N::Counters after=N::counters();
    const bool ok=!admitted && tailAfter==tailBefore && events.size()==2u && rd(dev+0x2A78u)==0u && irql()==0u &&
                  rd(foreign+0x20u)==0xC0000000u && after.tailInput==before.tailInput+1u &&
                  after.foreignRange==before.foreignRange+1u;
    check(ok,"declared range: a chain tail in it declines under the lock, publishing nothing, lock and IRQL restored");
    rangeHeld &= ok;
  }
  {
    endFixture(1u); wr(foreign+0x40u,0xC0000000u);
    N::Counters before=N::counters();
    const bool marker=declinesClean(endContext(state,foreign+0x40u),true);
    N::Counters after=N::counters();
    const bool markerOk=marker && after.endMarker==before.endMarker+1u && after.foreignRange==before.foreignRange+1u;
    check(markerOk,"declared range: an EndSync marker in it declines before imports and reads");
    endFixture(1u); before=N::counters();
    const bool stateDeclined=declinesClean(endContext(foreign+0x100u,oldTail),true);
    after=N::counters();
    const bool stateOk=stateDeclined && after.endState==before.endState+1u && after.foreignRange==before.foreignRange+1u;
    check(stateOk,"declared range: an EndSync state block in it declines before imports and writes");
    rangeHeld &= markerOk && stateOk;
  }
  {
    const N::Counters before=N::counters();
    const bool equal=append(false,1u,0u,foreign+0x100u)==append(true,1u,0u,foreign+0x100u);
    const N::Counters after=N::counters();
    check(equal && after.decline==before.decline+1u && after.append==before.append,
          "declared range: production dispatch falls back to the actual SDK body (equal state and imports)");
  }
  // A read-only declared page (a foreign protection, as the watchdog's): never written. Entered only when the
  // unprotected declared range held, so a defect that admits it fails here by name instead of faulting.
  bool protectedOk=false;
  if (rangeHeld) {
    endFixture(1u);
    const std::vector<uint8_t> bytes=pageBytes(foreign);
    DWORD old=0,ignored=0;
    if (VirtualProtect(K::guestVAToHost(foreign),0x1000u,PAGE_READONLY,&old)) {
      protectedOk=declinesClean(appendContext(foreign+0x100u),false) &&
                  declinesClean(endContext(foreign+0x100u,oldTail),true) && protectionIs(foreign,PAGE_READONLY);
      protectedOk &= VirtualProtect(K::guestVAToHost(foreign),0x1000u,old,&ignored)!=0 && pageBytes(foreign)==bytes;
    }
  }
  check(protectedOk,"declared range: a read-only declared page is never written (entries decline, bytes and protection kept)");
  // The real image-write watchdog: its always-on boot page is declared as a range before it is protected.
  const uint32_t declared=GP::declarations();
  K::installImageWriteWatchdog();
  check(GP::declarations()==declared+1u && !GP::excludesAll() && protectionIs(bootPage,PAGE_READONLY) &&
        GP::intersects(K::guestVAToHost(bootPage+0x458u),4u),
        "image watchdog: its always-on boot page is declared as a range before it becomes read-only");
  bool bootOk=false;
  if (rangeHeld) {
    endFixture(0u);
    const std::vector<uint8_t> bytes=pageBytes(bootPage);
    const N::Counters before=N::counters();
    bootOk=declinesClean(appendContext(bootPage+0x100u),false) && protectionIs(bootPage,PAGE_READONLY) &&
           pageBytes(bootPage)==bytes && N::counters().foreignRange==before.foreignRange+1u;
  }
  check(bootOk,"image watchdog: an append whose node lies on the boot page declines, the page read-only and unchanged");
  {
    const N::Counters before=N::counters();
    const bool equal=append(false,1u,0u,buffer+0x100u)==append(true,1u,0u,buffer+0x100u);
    const N::Counters after=N::counters();
    check(equal && after.chain==before.chain+1u && after.decline==before.decline && !GP::excludesAll(),
          "image watchdog: entries away from the boot page still run natively");
  }
  // Its runtime arm (behind SR_XMV_WATCH / SR_XMV_EXTRA_WATCH / SR_ALLOC_WATCH) excludes everything from then on: it
  // declares the boot page's read-write restore and the watched page's read-only protection, both excluding all.
  const bool armed=K::armRuntimeWriteWatch(watchPage+0x10u,4u);
  check(armed && GP::excludesAll() && GP::declarations()==declared+3u && protectionIs(watchPage,PAGE_READONLY),
        "runtime write watch: arming declares a protection that excludes every arithmetic admission");
  check(allRoutesExcluded(),
        "exclusion: direct and chained append, parking and following EndSync decline before imports and writes, counted");
  check(dispatchEqualsSdk(),"exclusion: production dispatch runs the actual SDK body (equal state and imports)");
  K::disarmRuntimeWriteWatch();
  check(protectionIs(watchPage,PAGE_READWRITE) && GP::excludesAll() && allRoutesExcluded(),
        "exclusion: permanent - restoring the protected page does not readmit the pair");
  // The audio null-page protection (SR_PROTECT_ZERO's funnel), on a page nothing else touches.
  const uint32_t beforeZero=GP::declarations();
  const bool zero=K::cmdq::protectRange(K::guestVAToHost(zeroPage),0x1000u);
  DWORD old=0;
  check(zero && GP::declarations()==beforeZero+1u && GP::excludesAll() && protectionIs(zeroPage,PAGE_NOACCESS) &&
        VirtualProtect(K::guestVAToHost(zeroPage),0x1000u,PAGE_READWRITE,&old)!=0,
        "audio null-page protection: declared, excluding all, before the page becomes inaccessible");
}
void exclusionEnvMode() {
  check(N::gateOn() && GP::diagnosticRequested() && !GP::excludesAll() && GP::declarations()==0u,
        "exclusion env: a page-protecting diagnostic is requested and nothing is declared");
  check(allRoutesExcluded(),
        "exclusion env: from the first call every route declines before imports and writes, counted");
  check(dispatchEqualsSdk(),"exclusion env: production dispatch runs the actual SDK body (equal state and imports)");
}

// ---- Mode "retire" (Astra round A, P1; SR_NATIVE_TRANSPORT=1): the guest arena's release against native calls in
// flight. Arithmetic admission trusts the registered arena to stay mapped; every native entry holds an ArenaLease
// across its guest accesses, and the real release (K::releaseGuestArena, which freeGuestMemory calls) retires the
// arena first. A chained append pauses inside its chain-lock acquire - lease held, its node, list, tail and count
// stores still ahead - while the arena is retired and released: retirement must not report the arena drained, new
// native calls must decline before any guest access, and the release must keep the arena mapped. Then the append
// completes into the mapped arena and the release frees it. The arena is gone afterwards, so the process ends there
// (TerminateProcess: no exit-time code runs); a defect that frees it early never lets the append resume.
constexpr uint32_t dev2=0x70630000u, list2=dev2+0x32C8u, ps2=0x70640000u;
[[noreturn]] void endProcess() {
  std::printf("[NATIVE_TRANSPORT_TEST] %d passed, %d failed\n",passed,failed);
  std::fflush(stdout);
  TerminateProcess(GetCurrentProcess(),failed ? 1u : 0u);
  for (;;) Sleep(1000);
}
size_t eventCount() { std::lock_guard<std::mutex> guard(eventsMutex); return events.size(); }
[[noreturn]] void retireMode() {
  check(N::gateOn() && GP::arenaLeases()==0u && !GP::arenaRetired(),
        "retire: gate on, no arena lease held and the arena not retired at start");
  {
    const N::Counters before=N::counters();
    const bool equal=append(false,1u,0u,buffer+0x100u)==append(true,1u,0u,buffer+0x100u);
    check(equal && N::counters().chain==before.chain+1u && GP::arenaLeases()==0u,
          "retire: a native call takes and returns its lease (none held afterwards)");
  }
  fixture(1u);
  uint8_t* const guestBase=K::getGuestBase();
  const size_t guestSize=K::getGuestSize();
  acquisitionAttempts.store(0); acquisitionPaused.store(false); acquisitionContinue.store(false);
  scheduleValid.store(true); pauseRole=1u; pauseBoundMs=20000u;
  uint64_t inFlightReturn=0;
  std::thread inFlight([&] {
    raceRole=1;
    K::setGuestMemory(guestBase,0x100000000ull);
    K::kpcrBindCurrent(stack-0x20000u,stack-0x30000u,1);
    PPCContext c=appendContext(buffer+0x100u); c.r1.u64=stack-0x20000u;
    PPC_LOOKUP_FUNC(K::getGuestBase(),0x820FD800u)(c,K::getGuestBase());
    inFlightReturn=c.r3.u64;
  });
  for (unsigned i=0; i<5000 && !acquisitionPaused.load(std::memory_order_acquire); ++i) Sleep(1);
  check(acquisitionPaused.load() && GP::arenaLeases()==1u,
        "retire: an in-flight native append holds its lease while it waits on the chain lock");
  const bool drainedEarly=GP::retireArena(200u);
  check(!drainedEarly && GP::arenaRetired() && GP::arenaLeases()==1u,
        "retire: retiring the arena does not report it drained while a native call is in flight");
  {
    // New calls on a second device and state block (their own locks): a defect that admits them completes them
    // instead of waiting on the in-flight append's lock.
    std::memset(K::guestVAToHost(dev2),0,0x4000u); std::memset(K::guestVAToHost(ps2),0,0x60u);
    wr(dev2+0x2A74u,1u); wr(list2+8u,buffer+0xE00u); wr(buffer+0xE00u,0xC0000000u);
    wr(ps2,1u); wr(ps2+0x30u,1u); wr(ps2+0x34u,1u); wr(buffer+0xE80u,0xD0641000u);
    PPCContext n1=appendContext(buffer+0x300u); n1.r3.u64=dev2; n1.r8.u64=list2;
    PPCContext n2=context(); n2.r3.u64=ps2; n2.r4.u64=buffer+0xE80u;
    const std::vector<uint8_t> dev2Before(K::guestVAToHost(dev2),K::guestVAToHost(dev2)+0x4000u);
    const uint64_t r1=n1.r3.u64, r2=n2.r3.u64;
    const size_t events0=eventCount();
    const N::Counters before=N::counters();
    const bool appended=N::appendJumpSegment(n1,guestBase);
    const bool ended=N::endSync(n2,guestBase);
    const N::Counters after=N::counters();
    check(!appended && !ended && n1.r3.u64==r1 && n2.r3.u64==r2 && eventCount()==events0 &&
          std::vector<uint8_t>(K::guestVAToHost(dev2),K::guestVAToHost(dev2)+0x4000u)==dev2Before &&
          rd(buffer+0xE00u)==0xC0000000u && after.retired==before.retired+2u && after.decline==before.decline+2u,
          "retire: once the arena is retired every new native call declines before any guest access, counted");
  }
  const bool releasedEarly=K::releaseGuestArena(guestBase,guestSize);
  MEMORY_BASIC_INFORMATION region{};
  const bool mapped=VirtualQuery(guestBase,&region,sizeof(region))!=0 && region.State==MEM_COMMIT;
  check(!releasedEarly && mapped,
        "retire: the arena release is skipped while a native call is in flight (the arena stays mapped)");
  if (!mapped) {  // the in-flight append would store into freed memory: it never resumes
    check(false,"retire: the in-flight append completes its stores into the mapped arena and returns its lease");
    check(false,"retire: with no call in flight the release frees the arena");
    endProcess();
  }
  acquisitionContinue.store(true,std::memory_order_release);
  inFlight.join();
  check(scheduleValid.load() && inFlightReturn==buffer+0x110u && rd(oldTail)==0xD0640100u && rd(dev+0x2A78u)==0u &&
        GP::arenaLeases()==0u,
        "retire: the in-flight append completes its stores into the mapped arena and returns its lease");
  const bool released=K::releaseGuestArena(guestBase,guestSize);
  region={};
  const bool freed=VirtualQuery(guestBase,&region,sizeof(region))!=0 && region.State==MEM_FREE;
  check(released && freed,"retire: with no call in flight the release frees the arena");
  endProcess();  // nothing may touch guest memory from here on
}

// ---- Mode "bench" (a diagnostic, not graded by the runner): host time per call of the actual SDK body and the native
// pair on the fixture with the real imports (no observers): a chained append (no ring wait) and an EndSync that follows
// a link, then the admission queries alone - QueryWorkingSetEx on a writable page and the tracker query on a
// tracker-protected one. One uncontended host thread on whatever machine runs it: a per-call bound, not live cost.
double nsPer(LARGE_INTEGER a,LARGE_INTEGER b,LARGE_INTEGER f,unsigned n) {
  return double(b.QuadPart-a.QuadPart)*1e9/double(f.QuadPart)/n;
}
template <class F> double timeLoop(unsigned n,F&& body) {
  LARGE_INTEGER f,a,b; QueryPerformanceFrequency(&f);
  for (unsigned i=0; i<n/10u; ++i) body();  // warm up
  QueryPerformanceCounter(&a);
  for (unsigned i=0; i<n; ++i) body();
  QueryPerformanceCounter(&b);
  return nsPer(a,b,f,n);
}
void bench() {
  constexpr unsigned n=200000u;
  const N::Counters before=N::counters();
  fixture(1u);
  auto chainCall=[](bool sdk) {
    PPCContext c=context(); c.r3.u64=dev; c.r4.u64=buffer+0x100u; c.r5.u64=0x10641234u;
    c.r6.u64=37u; c.r7.u64=0u; c.r8.u64=list;
    if (sdk) __imp__sub_820FD800(c,K::getGuestBase()); else PPC_LOOKUP_FUNC(K::getGuestBase(),0x820FD800u)(c,K::getGuestBase());
  };
  const double sdkChain=timeLoop(n,[&] { chainCall(true); }), nativeChain=timeLoop(n,[&] { chainCall(false); });
  fixture(1u); wr(oldTail,0xD0641000u); wr(state,1u); wr(state+0x30u,1u); wr(state+0x34u,1u);
  auto followCall=[](bool sdk) {
    PPCContext c=context(); c.r3.u64=state; c.r4.u64=oldTail;
    if (sdk) __imp__sub_82110768(c,K::getGuestBase()); else PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110768u)(c,K::getGuestBase());
  };
  const double sdkFollow=timeLoop(n,[&] { followCall(true); }), nativeFollow=timeLoop(n,[&] { followCall(false); });
  const N::Counters after=N::counters();
  const uint8_t* rw=K::guestVAToHost(buffer+0x100u);
  bool ok=true;
  const double queryRw=timeLoop(n,[&] { ok&=sr::native::memory::writableRange(rw,16u); });
  const uint32_t va=buffer+0x40000u, page=(va&0x1FFFFFFFu)>>12;
  W::protectForValidation(&page,1u); W::markValidated(page,8u);
  const uint8_t* ro=K::guestVAToHost(va);
  const bool tracked=!sr::native::memory::writableRange(ro,16u) && W::cpuWritableRange(ro,16u);
  const double queryTracked=timeLoop(n,[&] { ok&=W::cpuWritableRange(ro,16u); });
  wr(va,0u);  // a real store releases the tracker's page
  check(N::gateOn() && after.chain-before.chain>=n && after.resume-before.resume>=n && after.decline==before.decline &&
        ok && tracked,"bench: the native arm ran every timed call (chain and follow counted) and both queries admitted");
  std::printf("[NATIVE_TRANSPORT_BENCH] n=%u ns/call: chain sdk=%.0f native=%.0f | follow sdk=%.0f native=%.0f | "
              "admission query writableRange=%.0f trackerQuery=%.0f\n",
              n,sdkChain,nativeChain,sdkFollow,nativeFollow,queryRw,queryTracked);
}

// ---- Mode "cost" (a diagnostic, not graded): where the admission's OS queries get expensive. (1) The touched span:
// the arena is one committed read-write allocation, so the marker's VirtualQuery region extends as far as nothing
// changes protection; touching (faulting in) a span around it gives that region present page-table entries, as the
// running game's memory has. (2) Churn: a second host thread changes protection elsewhere in the arena while the
// calls run - raw VirtualProtect toggles, or the write tracker's own protect / store-fault / release cycle (its VEH).
// Per configuration: mean and max ns per call of VirtualQuery on the marker, the SDK and native EndSync follow, the
// SDK and native chained append, and (2026-10-14, Astra round A) the queries the replaced admission made per call, on
// the same build and memory: a follow's QueryWorkingSetEx-or-tracker query of the state block plus its VirtualQuery
// walk of the marker, and a chained append's five QueryWorkingSetEx-or-tracker queries (device word, list, node,
// stack window, parked tail). The tracker churn rotates 32 blocks and advances 64 frames per cycle so every cycle
// protects and faults (each fault starts a cooldown of up to 64 frames on its block); its protect runs and faults
// are counted over the row.
struct Cost { double mean, max; };
// The replaced admission's queries (kernel/native_transport.cpp before option A), for the cost comparison only.
bool oldWritable(uint32_t va,uint32_t bytes) {
  const uint8_t* first=K::guestVAToHost(va);
  return sr::native::memory::writableRange(first,bytes) || W::cpuWritableRange(first,bytes);
}
bool oldReadable(uint32_t va) {
  const uintptr_t begin=reinterpret_cast<uintptr_t>(K::guestVAToHost(va));
  for (uintptr_t at=begin; at<begin+4u;) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void*>(at),&region,sizeof(region)) || region.State!=MEM_COMMIT) return false;
    at=reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize;
  }
  return true;
}
template <class F> Cost costLoop(unsigned n,F&& body) {
  LARGE_INTEGER f,a,b; QueryPerformanceFrequency(&f);
  for (unsigned i=0; i<8u; ++i) body();
  double sum=0, mx=0;
  for (unsigned i=0; i<n; ++i) {
    QueryPerformanceCounter(&a); body(); QueryPerformanceCounter(&b);
    const double ns=double(b.QuadPart-a.QuadPart)*1e9/double(f.QuadPart);
    sum+=ns; if (ns>mx) mx=ns;
  }
  return {sum/n,mx};
}
void costRow(const char* label) {
  constexpr unsigned n=4000u;
  fixture(1u); wr(oldTail,0xD0641000u); wr(state,1u); wr(state+0x30u,1u); wr(state+0x34u,1u);
  const uint8_t* marker=K::guestVAToHost(oldTail);
  MEMORY_BASIC_INFORMATION region{};
  VirtualQuery(marker,&region,sizeof(region));
  const Cost vq=costLoop(n,[&] { MEMORY_BASIC_INFORMATION r{}; VirtualQuery(marker,&r,sizeof(r)); });
  auto follow=[](bool sdk) {
    PPCContext c=context(); c.r3.u64=state; c.r4.u64=oldTail;
    if (sdk) __imp__sub_82110768(c,K::getGuestBase()); else PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110768u)(c,K::getGuestBase());
  };
  const Cost sdkFollow=costLoop(n,[&] { follow(true); }), nativeFollow=costLoop(n,[&] { follow(false); });
  wr(dev+0x2A74u,1u); wr(list+8u,oldTail);
  auto chain=[](bool sdk) {
    PPCContext c=context(); c.r3.u64=dev; c.r4.u64=buffer+0x100u; c.r5.u64=0x10641234u;
    c.r6.u64=37u; c.r7.u64=0u; c.r8.u64=list;
    if (sdk) __imp__sub_820FD800(c,K::getGuestBase()); else PPC_LOOKUP_FUNC(K::getGuestBase(),0x820FD800u)(c,K::getGuestBase());
  };
  const Cost sdkChain=costLoop(n,[&] { chain(true); }), nativeChain=costLoop(n,[&] { chain(false); });
  bool ok=true;
  const Cost oldFollow=costLoop(n,[&] { ok&=oldWritable(state,0x60u) && oldReadable(oldTail); });
  const Cost oldChain=costLoop(n,[&] {
    ok&=oldWritable(dev+0x2A74u,8u) && oldWritable(list,16u) && oldWritable(buffer+0x100u,16u) &&
        oldWritable(stack-0x1000u,0x1000u) && oldWritable(oldTail,4u);
  });
  std::printf("[NATIVE_TRANSPORT_COST] %-34s region=%6.1f MB | VirtualQuery mean=%8.0f max=%9.0f | follow sdk=%5.0f/%7.0f "
              "native=%8.0f/%9.0f old-queries=%8.0f/%9.0f | chain sdk=%5.0f/%7.0f native=%8.0f/%9.0f "
              "old-queries=%8.0f/%9.0f (ns mean/max)%s\n",
              label,double(region.RegionSize)/1048576.0,vq.mean,vq.max,sdkFollow.mean,sdkFollow.max,nativeFollow.mean,
              nativeFollow.max,oldFollow.mean,oldFollow.max,sdkChain.mean,sdkChain.max,nativeChain.mean,nativeChain.max,
              oldChain.mean,oldChain.max,ok ? "" : " OLD-QUERY-DECLINED");
}
void cost() {
  const N::Counters before=N::counters();
  // (1) the touched span around the marker (centred on the fixture buffer)
  uint32_t touched=0;
  for (uint32_t mb : {0u,16u,64u,256u}) {
    const uint32_t half=mb<<19, lo=buffer-half, hi=buffer+half;
    for (uint32_t va=lo; va<hi; va+=0x1000u) { volatile uint8_t* p=K::guestVAToHost(va); p[0]=p[0]; }
    touched=mb;
    char label[64]; std::snprintf(label,sizeof(label),"touched %u MB, quiet",touched);
    costRow(label);
  }
  // (2) churn on another host thread, 32 MB away from the fixture buffer
  uint8_t* const guestBase=K::getGuestBase();
  bool trackerChurned=false;
  for (int churn : {1,2}) {
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> cycles{0};
    const W::Stats stats0=W::stats();
    std::thread t([&,churn] {
      K::setGuestMemory(guestBase,0x100000000ull);
      const uint32_t va0=buffer+0x2000000u, page0=(va0&0x1FFFFFFFu)>>12;  // block-aligned (page 0x12640)
      while (!stop.load(std::memory_order_relaxed)) {
        const uint32_t block=uint32_t(cycles.load(std::memory_order_relaxed)%32u);
        const uint32_t page=page0+block*64u, va=va0+block*0x40000u;
        if (churn==1) {
          DWORD old=0;
          VirtualProtect(K::guestVAToHost(va),64u*4096u,PAGE_READONLY,&old);
          VirtualProtect(K::guestVAToHost(va),64u*4096u,PAGE_READWRITE,&old);
        } else {
          for (unsigned f=0; f<64u; ++f) W::beginFrame();  // past this block's fault cooldown (at most 64 frames)
          uint32_t pages[64]; for (uint32_t i=0; i<64u; ++i) pages[i]=page+i;
          W::protectForValidation(pages,64u); W::markValidated(page,8u);
          K::writeGuestBE32(va,uint32_t(cycles.load()));  // faults: the tracker's VEH releases the block
        }
        cycles.fetch_add(1,std::memory_order_relaxed);
      }
    });
    char label[64]; std::snprintf(label,sizeof(label),"touched %u MB, %s",touched,
                                  churn==1 ? "VirtualProtect churn" : "tracker fault churn");
    costRow(label);
    stop.store(true); t.join();
    const W::Stats stats1=W::stats();
    const uint64_t n=cycles.load(), faults=stats1.faults-stats0.faults, protects=stats1.protects-stats0.protects;
    std::printf("[NATIVE_TRANSPORT_COST]   churn cycles during the row: %llu (tracker protect runs %llu, faults %llu)\n",
                (unsigned long long)n,(unsigned long long)protects,(unsigned long long)faults);
    if (churn==2) trackerChurned=n>=100u && faults*10u>=n*9u && protects*10u>=n*9u;
  }
  const N::Counters after=N::counters();
  check(N::gateOn() && after.decline==before.decline && after.resume>before.resume && after.chain>before.chain,
        "cost: the native arm ran the timed calls with no decline");
  check(trackerChurned,"cost: the tracker churn protected and faulted on (nearly) every cycle of its row");
}
}

int runNativeTransportTest(const char* mode) {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  // The real write tracker obtains the arena through this process-global
  // registration, independently of the test thread's TLS guest-memory binding.
  K::registerExCreateThreadRunner(nullptr,K::getGuestBase(),K::getGuestSize());
  // Query-only defect controls must never enter an unsafe native store after
  // deliberately admitting an unowned read-only page.
  if (std::strcmp(mode,"permissions")==0) {
    trackedRangeChecks();
    std::printf("[NATIVE_TRANSPORT_TEST] %d passed, %d failed\n",passed,failed);
    return failed ? 1 : 0;
  }
  K::registerStubs(); sr::guest::initializeDispatchTable();
  K::kpcrBindCurrent(stack,stack - 0x10000u,3);
  const bool nativeEnabled = std::strcmp(mode,"gate-off")!=0;
  check(N::gateOn()==nativeEnabled && K::keSpinlockEnabled() && K::kpcrCurrent(),
        "requested gate and actual per-thread spinlock enabled");
  check(PPC_LOOKUP_FUNC(K::getGuestBase(),0x820FD800u)==sub_820FD800,"production dispatch resolves native append override");
  check(PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110768u)==sub_82110768,"production dispatch resolves native EndSync override");
  auto& registry = K::ExportRegistry::instance();
  originalAcquire = registry.getExport("KfAcquireSpinLock"); originalRelease = registry.getExport("KfReleaseSpinLock");
  originalSet = registry.getExport("KeSetEvent"); originalReset = registry.getExport("KeResetEvent");
  originalWait = registry.getExport("KeWaitForSingleObject");
  if (!originalAcquire || !originalRelease || !originalSet || !originalReset || !originalWait) return 1;
  if (std::strcmp(mode,"bench")==0 || std::strcmp(mode,"cost")==0) {  // the real imports, no observers
    if (mode[0]=='b') bench(); else cost();
    std::printf("[NATIVE_TRANSPORT_TEST] %d passed, %d failed\n",passed,failed);
    return failed ? 1 : 0;
  }
  registry.registerExport("KfAcquireSpinLock",acquire); registry.registerExport("KfReleaseSpinLock",release);
  registry.registerExport("KeSetEvent",setEvent); registry.registerExport("KeResetEvent",resetEvent);
  registry.registerExport("KeWaitForSingleObject",waitEvent);
  if (std::strcmp(mode,"retire")==0) retireMode();  // ends the process
  if (std::strcmp(mode,"exclusion")==0 || std::strcmp(mode,"exclusion-env")==0) {
    if (std::strcmp(mode,"exclusion")==0) exclusionMode(); else exclusionEnvMode();
    registry.registerExport("KfAcquireSpinLock",originalAcquire); registry.registerExport("KfReleaseSpinLock",originalRelease);
    registry.registerExport("KeSetEvent",originalSet); registry.registerExport("KeResetEvent",originalReset);
    registry.registerExport("KeWaitForSingleObject",originalWait);
    N::finalLine("test");
    std::printf("[NATIVE_TRANSPORT_TEST] %d passed, %d failed\n",passed,failed);
    return failed ? 1 : 0;
  }
  if (std::strcmp(mode,"ab")==0) {
    abBit=sr::abt::reg("SR_NATIVE_TRANSPORT");
    check(abBit>=0 && sr::abt::active(),"A/B mode: SR_NATIVE_TRANSPORT is toggled per present (SR_AB_TOGGLE, SR_AB_PRESENT)");
    if (abBit>=0) { abEntries(); abSequence(); }
    registry.registerExport("KfAcquireSpinLock",originalAcquire); registry.registerExport("KfReleaseSpinLock",originalRelease);
    registry.registerExport("KeSetEvent",originalSet); registry.registerExport("KeResetEvent",originalReset);
    registry.registerExport("KeWaitForSingleObject",originalWait);
    N::finalLine("test");
    std::printf("[NATIVE_TRANSPORT_TEST] %d passed, %d failed\n",passed,failed);
    return failed ? 1 : 0;
  }
  for (uint32_t count : {0u,1u,3u}) for (uint32_t inc : {0u,1u})
    for (uint64_t upper : {0ull,0x1234567800000000ull}) {
      const uint64_t at = upper | (buffer + 0x100u);
      const Result expected = append(true,count,inc,at), actual = append(false,count,inc,at);
      check(actual == expected,"actual SDK and native dispatch: complete append state and import sequence");
      check(actual.returned == at + (count ? 16u : 0u),"full-width direct/chained return preserved");
      if (count) check(actual.calls.size()==2u && actual.calls[1][5]==K::kpcrCurrent() &&
                       rd(oldTail)==0xD0640100u && rd(buffer+0x10Cu)==0xC0000000u,
                       "new initialized node linked under the shared actual lock");
      else check(actual.calls.empty() && rd(ring)==0xC0013F00u && rd(ring+4u)==0x10641234u && rd(ring+8u)==37u,
                 "direct route emits the real descriptor without taking chain lock");
    }
  parkBeforeAcquire = true;
  check(append(false,1u,1u,buffer+0x100u)==append(true,1u,1u,buffer+0x100u),
        "consumer parks before acquired recheck: producer takes direct route");
  parkBeforeAcquire = false;
  for (uint32_t marker : {0xD0641000u,0xC0000000u}) for (uint32_t nesting : {1u,2u}) for (bool paused : {false,true}) {
    const Result expected = finish(true,marker,nesting,paused), actual = finish(false,marker,nesting,paused);
    check(actual==expected,"actual SDK and native dispatch: complete EndSync state and import sequence");
    if (marker==0xC0000000u)
      check(actual.calls.size()==5u && actual.calls[2][0]==2u && actual.calls[3][5]==0u &&
            actual.calls[4][2]==0u && actual.returned==0u,
            "park records terminal and waits after unlocking and clearing active state");
    else check(actual.calls.size()==2u && actual.returned==uint64_t(marker)+4u && rd(state+0x30u)==nesting,
               "producer won: follow linked node without decrementing completion state");
  }
  for (uint32_t marker : {0xC0000000u,0xFFFFFFFCu}) {
    const Result expected=finish(true,marker,1u,false,true);
    const Result dispatched=finish(false,marker,1u,false,true);
    const Result admitted=finish(false,marker,1u,false,true,true);
    check(dispatched==expected,"readable-only marker production dispatch matches complete SDK state and imports");
    check(admitted==expected,"readable-only marker admitted call matches complete SDK state and imports");
  }
  linkBeforeAcquire = true;
  check(finish(false,0xC0000000u,1u,false)==finish(true,0xC0000000u,1u,false),
        "producer links before acquired recheck: consumer follows new link");
  linkBeforeAcquire = false;
  realWait = true;
  uint8_t* const guestBase = K::getGuestBase();
  for (bool oracle : {true,false}) {
    waitEntered.store(false,std::memory_order_relaxed);
    std::thread signal([guestBase] {
      K::setGuestMemory(guestBase,0x100000000ull);
      for (unsigned i=0; i<250 && !waitEntered.load(std::memory_order_acquire); ++i) Sleep(1);
      originalSet(state+0x40u,1u,0u,0u,0u,0u);
    });
    const Result result = finish(oracle,0xC0000000u,2u,false);
    signal.join();
    check(waitEntered.load() && result.returned==0u && rd(state+0x30u)==1u && rd(state+0x38u)==5u,
          "real nested event reset/wait wakes on another host thread");
  }
  realWait = false;
  for (bool producerFirst : {true,false}) for (bool oracle : {true,false})
    check(race(oracle,producerFirst),producerFirst ?
          "controlled real two-thread producer-first ordering follows initialized link" :
          "controlled real two-thread consumer-first ordering publishes directly after park");
  for (unsigned invalid=0; invalid<5; ++invalid) {
    fixture(1u); PPCContext c=context(); c.r3.u64=dev; c.r4.u64=buffer+0x100u; c.r8.u64=list;
    if (invalid==0) c.r3.u64=0u;
    if (invalid==1) ++c.r3.u64;
    if (invalid==2) ++c.r4.u64;
    if (invalid==3) ++c.r8.u64;
    if (invalid==4) ++c.r1.u64;
    const Result before=capture(c);
    check(!N::appendJumpSegment(c,K::getGuestBase()) && capture(c)==before && events.empty(),
          "null or unaligned append inputs decline before imports and shared-state writes");
  }
  for (unsigned invalid=0; invalid<3; ++invalid) {
    fixture(1u); PPCContext c=context(); c.r3.u64=state; c.r4.u64=oldTail;
    if (invalid==0) c.r3.u64=0u;
    if (invalid==1) ++c.r3.u64;
    if (invalid==2) ++c.r4.u64;
    const Result before=capture(c);
    check(!N::endSync(c,K::getGuestBase()) && capture(c)==before && events.empty(),
          "null or unaligned EndSync inputs decline before imports and shared-state writes");
  }
  fixture(1u); PPCContext owner=context(); owner.r13.u64=0u; owner.r3.u64=dev;
  owner.r4.u64=buffer+0x100u; owner.r8.u64=list; const Result ownerBefore=capture(owner);
  check(!N::appendJumpSegment(owner,K::getGuestBase()) && capture(owner)==ownerBefore && events.empty(),
        "missing actual guest lock owner declines before imports and mutation");
  fixture(1u); PPCContext tail=context(); tail.r3.u64=dev; tail.r4.u64=buffer+0x100u; tail.r8.u64=list;
  wr(list+8u,oldTail+1u); const Result tailBefore=capture(tail);
  const bool tailAdmitted=N::appendJumpSegment(tail,K::getGuestBase());
  Result tailAfter=capture(tail); tailAfter.calls.clear();
  check(!tailAdmitted && tailAfter==tailBefore && events.size()==(nativeEnabled ? 2u : 0u) &&
        rd(dev+0x2A78u)==0u && irql()==0u,"unaligned actual chain tail declines with lock and IRQL restored");
  fixture(1u); PPCContext c = context(); c.r3.u64 = dev; c.r4.u64 = buffer+0x100u;
  c.r5.u64=0x10641234u; c.r6.u64=37u; c.r8.u64=list;
  wr(list+8u,0u); const Result before = capture(c);
  const bool admitted = N::appendJumpSegment(c,K::getGuestBase());
  Result after = capture(c); after.calls.clear();
  check(!admitted && after==before && events.size()==(nativeEnabled ? 2u : 0u) && rd(dev+0x2A78u)==0u && irql()==0u,
        "invalid actual chain tail declines before publication with lock and IRQL restored");
  rangeChecks(nativeEnabled);
  trackedRangeChecks();
  for (unsigned kind=0;kind<3u;++kind) {
    const Tracked expected=trackedAppend(Call::Sdk,kind);
    const Tracked dispatched=trackedAppend(Call::Dispatch,kind);
    const Tracked admittedTracked=trackedAppend(Call::Direct,kind);
    check(dispatched.result==expected.result,"tracked output production dispatch matches complete SDK state and imports");
    check(admittedTracked.result==expected.result,"tracked output admitted call matches complete SDK state and imports");
    check(dispatched.view==expected.view && admittedTracked.view==expected.view,
          "native append stores into tracked pages take the guest store's path: the SDK's faults, releases and invalidations");
  }
  for (bool terminal : {true,false}) {
    const Tracked expected=trackedFinish(Call::Sdk,terminal);
    const Tracked dispatched=trackedFinish(Call::Dispatch,terminal);
    const Tracked admittedTracked=trackedFinish(Call::Direct,terminal);
    check(dispatched.result==expected.result && admittedTracked.result==expected.result &&
          admittedTracked.admitted==nativeEnabled,
          "tracked EndSync production dispatch and admitted call match complete SDK state and imports");
    check(dispatched.view==expected.view && admittedTracked.view==expected.view,
          "native EndSync stores into tracked pages take the guest store's path: the SDK's faults, releases and invalidations");
  }
  registry.registerExport("KfAcquireSpinLock",originalAcquire); registry.registerExport("KfReleaseSpinLock",originalRelease);
  registry.registerExport("KeSetEvent",originalSet); registry.registerExport("KeResetEvent",originalReset);
  registry.registerExport("KeWaitForSingleObject",originalWait);
  const N::Counters counts=N::counters();
  check(!sr::abt::active() && counts.abOffAppend==0u && counts.abOffEnd==0u,
        "without SR_AB_TOGGLE no call takes an A/B OFF window (abOffAppend 0, abOffEnd 0)");
  N::finalLine("test");
  std::printf("[NATIVE_TRANSPORT_TEST] %d passed, %d failed\n",passed,failed);
  return failed ? 1 : 0;
}
