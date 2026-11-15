// Actual 82110840 SDK, production dispatch and admitted native driver.
// No game/GPU worker. Helpers retain their selected SDK/native CPU paths.
// This bank proves scheduling semantics, not generation sealing or native-zero.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "ppc/ppc_recomp_shared.h"
#include "guest/guest_harness.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/kpcr.hpp"
#include "kernel/native_pump.hpp"
#include "kernel/stubs.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

extern "C" void __imp__sub_82110840(PPCContext& __restrict, uint8_t*);
namespace K = sr::kernel;
namespace N = sr::native::pump;
namespace {
constexpr uint32_t dev=0x70600000u, state=dev+0x2A3Cu, records=0x90640000u;
constexpr uint32_t ring=0x70660000u, fb=0x70661000u, cell=0x70662000u;
constexpr uint32_t stack=0x70580000u, pumpContext=0x70663000u;
constexpr uint32_t callback=0x822949D8u, secondCallback=0x822949DCu;
int passed=0,failed=0;
bool nativeEnabled=false,concurrent=false;
unsigned releaseFirst=1;
thread_local unsigned role=0;
std::atomic<unsigned> entered{0};
std::atomic<bool> scheduled{true};
bool handlerSwitchRace=false;
unsigned switchIteration=0;
std::atomic<bool> raceDone{false},raceValid{true};
using Event=std::array<uint64_t,7>;
std::vector<Event> calls;
std::mutex callsMutex;
K::ExportFn originalSet,originalReset,originalWait;
uint32_t rd(uint32_t va) { return K::readGuestU32BE(va); }
void writeFixtureWord(uint32_t va,uint32_t value) { K::writeGuestBE32(va,value); }
void check(bool ok,const std::string& name) {
  std::printf("%s %s\n",ok ? "  ok " : "FAIL:",name.c_str()); ok ? ++passed : ++failed;
}
void note(Event e) { std::lock_guard<std::mutex> lock(callsMutex); calls.push_back(e); }
void regainDriverLock() {
  // SDK 82110840 releases S+0 before the callback and continues its repeat
  // loop without reacquiring it. A participating callback returns with S+0
  // acquired. Omitting this protocol makes the two-thread fixture invalid.
  auto* lock=reinterpret_cast<volatile LONG*>(K::guestVAToHost(state));
  while(InterlockedCompareExchange(lock,LONG(_byteswap_ulong(1u)),0)!=0)YieldProcessor();
}
void switchAfterUnlock(unsigned iteration) {
  if(!handlerSwitchRace || iteration!=switchIteration || raceDone.load())return;
  uint8_t* const base=K::getGuestBase();
  std::thread producer([&] {
    K::setGuestMemory(base,0x100000000ull);
    auto* lock=reinterpret_cast<volatile LONG*>(K::guestVAToHost(state));
    const ULONGLONG until=GetTickCount64()+2000u;
    while(InterlockedCompareExchange(lock,LONG(_byteswap_ulong(1u)),0)!=0) {
      if(GetTickCount64()>=until) {raceValid.store(false);return;}YieldProcessor();
    }
    writeFixtureWord(state+0x10u,secondCallback);writeFixtureWord(state+0x14u,0x66778899u);
    InterlockedExchange(lock,0);raceDone.store(true);
  });
  producer.join();
}
void afterDriverUnlock(uint32_t,unsigned iteration) { switchAfterUnlock(iteration); }
uint32_t setEvent(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  note({3u,a,b,c,rd(state+0x30u),rd(state+0x50u),0u}); return originalSet(a,b,c,d,e,f);
}
uint32_t resetEvent(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  note({4u,a,rd(state+0x30u),0u,0u,0u,0u}); return originalReset(a,b,c,d,e,f);
}
uint32_t waitEvent(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  if (!concurrent) note({5u,a,b,c,d,e,rd(state)});
  // Sequential fixtures observe a terminal's wait boundary without inventing
  // another participant. Concurrent fixtures execute the real event wait.
  return concurrent ? originalWait(a,b,c,d,e,f) : 0u;
}
void handler(PPCContext& c,uint8_t*) {
  switchAfterUnlock(c.r6.u32);
  note({1u,c.r3.u64,c.r4.u64,c.r5.u64,c.r6.u64,c.r1.u64,rd(state)});
  if (concurrent) {
    entered.fetch_add(1,std::memory_order_release);
    const ULONGLONG until=GetTickCount64()+2000u;
    if (role==releaseFirst) {
      while (entered.load(std::memory_order_acquire)<2u && GetTickCount64()<until) Sleep(1);
      if (entered.load()<2u) scheduled.store(false);
    } else {
      while (!rd(state+0x6Cu) && GetTickCount64()<until) Sleep(1);
      if (!rd(state+0x6Cu)) scheduled.store(false);
    }
  }
  regainDriverLock();
  c.r3.u64=0xBEEF1234u;
}
void handler2(PPCContext& c,uint8_t*) {
  switchAfterUnlock(c.r6.u32);
  note({2u,c.r3.u64,c.r4.u64,c.r5.u64,c.r6.u64,c.r1.u64,rd(state)});regainDriverLock();c.r3.u64=0u;
}
PPCContext context(uint64_t pointer=pumpContext,uint64_t sp=stack) {
  PPCContext c{}; c.r1.u64=sp;c.r3.u64=pointer;c.r13.u64=K::kpcrCurrent();
  c.r14.u64=0xAABBCCDDEEFF0011ull;c.r30.u64=0xFEED0123456789ABull;
  c.r31.u64=0x1122334455667788ull;c.f14.u64=0x1234567812345678ull;return c;
}
void segment(uint32_t at) { writeFixtureWord(at,0x82000007u);writeFixtureWord(at+4u,0x10641234u); }
void fixture() {
  std::memset(K::guestVAToHost(dev),0,0x5000u);
  std::memset(K::guestVAToHost(records),0,0x2000u);
  std::memset(K::guestVAToHost(ring),0xCC,0x800u);
  std::memset(K::guestVAToHost(fb),0,0x100u);
  std::memset(K::guestVAToHost(stack-0x10000u),0xDD,0x10000u);
  writeFixtureWord(pumpContext,state);writeFixtureWord(0x82000948u,cell);writeFixtureWord(cell,dev);
  writeFixtureWord(dev+0x2A10u,fb);writeFixtureWord(dev+0x34A4u,ring);writeFixtureWord(dev+0x34A8u,511u);
  writeFixtureWord(state+0x58u,records);writeFixtureWord(state+0x38u,2u);
  writeFixtureWord(state+0x60u,0xC0000000u);writeFixtureWord(state+0x16Cu,0x7FFFFFFFu);
  writeFixtureWord(state+0x10u,callback);writeFixtureWord(state+0x14u,0x11223344u);
  K::ExportRegistry::instance().getExport("KeInitializeEvent")(state+0x40u,0u,0u,0u,0u,0u);
  calls.clear();entered.store(0);scheduled.store(true);concurrent=false;
}
enum class Case { Empty,Terminal,DataZero,DataOne,DataRepeat,Switch,Link,Segments,DepthColour,
  PauseTaken,PauseUntaken,Resume,PauseResume,Join,HighPointer,HighStack,DataWide };
constexpr std::array<const char*,17> names={"empty driver","terminal driver","zero repeat driver","one repeat driver",
  "three repeats driver","handler switch driver","software link driver","segments driver","depth colour driver",
  "pause taken driver","pause untaken driver","resume driver","pause and kick resume driver",
  "joined participant driver","full context pointer driver","full stack pointer driver","wide repeat driver"};
void prepare(Case kind) {
  fixture();writeFixtureWord(records+4u,0xC0000000u);
  if (kind==Case::Empty) writeFixtureWord(state+0x58u,0u);
  else if (kind==Case::DataZero || kind==Case::DataOne || kind==Case::DataRepeat || kind==Case::HighPointer || kind==Case::HighStack || kind==Case::DataWide) {
    const uint32_t count=kind==Case::DataZero ? 0u : kind==Case::DataRepeat ? 3u : kind==Case::DataWide ? 0x101u : 1u;
    writeFixtureWord(records+4u,0x00080000u|count);writeFixtureWord(records+8u,0x55667788u);
    writeFixtureWord(records+12u,0x99AABBCCu);writeFixtureWord(records+16u,0xC0000000u);
  } else if (kind==Case::Switch) {
    writeFixtureWord(records+4u,0x81000000u);writeFixtureWord(records+8u,secondCallback);
    writeFixtureWord(records+12u,0x22334455u);writeFixtureWord(records+16u,0x00040002u);
    writeFixtureWord(records+20u,0x12345678u);writeFixtureWord(records+24u,0xC0000000u);
  } else if (kind==Case::Link) {
    writeFixtureWord(records+4u,records+0x100u);segment(records+0x104u);writeFixtureWord(records+0x10Cu,0xC0000000u);
  } else if (kind==Case::Segments) {
    segment(records+4u);segment(records+12u);writeFixtureWord(records+20u,0xC0000000u);
  } else if (kind==Case::DepthColour) {
    writeFixtureWord(records+4u,0x84000000u);segment(records+8u);writeFixtureWord(records+16u,0x85000000u);
    segment(records+20u);writeFixtureWord(records+28u,0x86000000u);segment(records+32u);writeFixtureWord(records+40u,0xC0000000u);
  } else if (kind==Case::PauseTaken || kind==Case::PauseUntaken || kind==Case::PauseResume) {
    writeFixtureWord(state+0x16Cu,kind==Case::PauseUntaken ? 0x10u : 0x20u);
    writeFixtureWord(records+4u,0x8C000000u);writeFixtureWord(records+8u,0x20u);
    segment(records+12u);writeFixtureWord(records+20u,0xC0000000u);
  } else if (kind==Case::Resume) {
    writeFixtureWord(state+0x50u,records+0x100u);writeFixtureWord(state+0x54u,records+0x1800u);
    segment(records+0x100u);writeFixtureWord(records+0x108u,0xC0000000u);
  } else if (kind==Case::Join) {
    writeFixtureWord(state+0x30u,1u);writeFixtureWord(state+0x1Cu,1u);
    writeFixtureWord(state+0x24u,records+4u);writeFixtureWord(state+0x54u,records);
  }
}
std::vector<uint8_t> bytes(uint32_t va,uint32_t size) { return {K::guestVAToHost(va),K::guestVAToHost(va)+size}; }
struct Result {
  std::array<uint64_t,7> regs;
  std::vector<uint8_t> device,list,ringBytes;
  std::vector<Event> events;
  bool operator==(const Result&) const=default;
};
Result capture(const PPCContext& c) {
  return {{c.r3.u64,c.r1.u64,c.r13.u64,c.r14.u64,c.r30.u64,c.r31.u64,c.f14.u64},
    bytes(dev,0x5000u),bytes(records,0x2000u),bytes(ring,0x800u),calls};
}
void invoke(PPCContext& c,unsigned route,const std::string& label) {
  if (!route) __imp__sub_82110840(c,K::getGuestBase());
  else if (route==1u) PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110840u)(c,K::getGuestBase());
  else {
    const bool admitted=N::replayDriver(c,K::getGuestBase());
    check(admitted==nativeEnabled,label+": direct admission");
    if (!admitted) __imp__sub_82110840(c,K::getGuestBase());
  }
}
Result run(Case kind,unsigned route) {
  prepare(kind);
  const uint64_t pointer=kind==Case::HighPointer ? uint64_t(pumpContext)|0x1234567800000000ull : pumpContext;
  const uint64_t sp=kind==Case::HighStack ? uint64_t(stack)|0x1234567800000000ull : stack;
  PPCContext c=context(pointer,sp);invoke(c,route,names[unsigned(kind)]);
  if (kind==Case::PauseResume) {
    check(rd(state+0x50u)==records+12u && rd(state+0x38u)==2u && rd(state+0x54u)==records,
          "pause route "+std::to_string(route)+" preserves traversal and chain before next kick");
    // A different, valid pending root arrives while S+50 still owns the
    // paused traversal. Its terminal keeps the wrong-root control fault-free.
    writeFixtureWord(records+0x1004u,0xC0000000u);
    writeFixtureWord(state+0x58u,records+0x1000u);c=context();invoke(c,route,"resumed kick driver");
  }
  return capture(c);
}
uint32_t word(const std::vector<uint8_t>& data,uint32_t offset) {
  return uint32_t(data[offset])<<24|uint32_t(data[offset+1u])<<16|uint32_t(data[offset+2u])<<8|data[offset+3u];
}
bool oracleValid(Case kind,const Result& r) {
  const bool empty=kind==Case::Empty,joined=kind==Case::Join;
  const bool paused=kind==Case::PauseTaken;
  unsigned handlers=kind==Case::DataRepeat ? 3u : kind==Case::DataWide ? 0x101u : kind==Case::Switch ? 2u :
    (kind==Case::DataZero || kind==Case::DataOne || kind==Case::HighPointer || kind==Case::HighStack) ? 1u : 0u;
  unsigned observed=0;for (const auto& e:r.events) if(e[0]<=2u) ++observed;
  const unsigned submissions=kind==Case::Segments ? 2u : kind==Case::DepthColour ? 4u :
    (kind==Case::Link || kind==Case::PauseUntaken || kind==Case::Resume || kind==Case::PauseResume) ? 1u : 0u;
  return r.regs[0]==(empty ? pumpContext : 0u) && observed==handlers &&
    word(r.device,0x2A44u)==submissions*3u+(kind==Case::DepthColour ? 8u : 0u) &&
    word(r.device,0x2A3Cu)==0u && word(r.device,0x2A6Cu)==(joined ? 1u : 0u) &&
    word(r.device,0x2A74u)==(empty || joined || paused ? 2u : 1u) &&
    word(r.device,0x2A8Cu)==(paused ? records+12u : 0u);
}
void denied() {
  for (unsigned bad=0;bad<5u;++bad) {
    fixture();PPCContext c=context();
    if (bad==0u)c.r3.u64=0u;else if(bad==1u)c.r3.u64=pumpContext+1u;
    else if(bad==2u)c.r1.u64=0x80u;else if(bad==3u)writeFixtureWord(pumpContext,state+1u);
    else writeFixtureWord(pumpContext,0u);
    const Result before=capture(c);const auto stackBefore=bytes(stack-0x10000u,0x10000u);
    check(!N::replayDriver(c,K::getGuestBase()) && capture(c)==before && bytes(stack-0x10000u,0x10000u)==stackBefore,
          "invalid driver input "+std::to_string(bad)+" declines before mutation");
  }
  for (DWORD protection:{PAGE_READONLY,PAGE_NOACCESS,PAGE_READWRITE|PAGE_GUARD}) {
    fixture();PPCContext c=context();const Result before=capture(c);const auto stackBefore=bytes(stack-0x10000u,0x10000u);
    DWORD old=0,ignored=0;const bool changed=VirtualProtect(K::guestVAToHost(dev+0x2000u),4096u,protection,&old)!=0;
    const bool declined=changed && !N::replayDriver(c,K::getGuestBase());
    const bool restored=changed && VirtualProtect(K::guestVAToHost(dev+0x2000u),4096u,old,&ignored)!=0;
    check(declined && restored && capture(c)==before && bytes(stack-0x10000u,0x10000u)==stackBefore,
          "protected driver state "+std::to_string(protection)+" declines before mutation");
  }
}
Result participants(unsigned route,unsigned first) {
  fixture();concurrent=true;releaseFirst=first;
  writeFixtureWord(records+4u,0x00040002u);writeFixtureWord(records+8u,0x55667788u);
  segment(records+12u);writeFixtureWord(records+20u,0xC0000000u);
  std::array<PPCContext,2> result;
  std::atomic<bool> registersPreserved{true};
  uint8_t* const base=K::getGuestBase();
  auto worker=[&](unsigned number) {
    K::setGuestMemory(base,0x100000000ull);
    const uint32_t sp=stack-number*0x4000u;
    K::kpcrBindCurrent(sp,sp-0x2000u,3);role=number;
    PPCContext c=context(pumpContext,sp);const PPCContext initial=c;
    // Direct admission is asserted by each sequential fixture; avoid writing
    // the test counters from these concurrent participants.
    if (!route)__imp__sub_82110840(c,base);
    else if(route==1u)PPC_LOOKUP_FUNC(base,0x82110840u)(c,base);
    else if(!N::replayDriver(c,base))__imp__sub_82110840(c,base);
    if(c.r1.u64!=initial.r1.u64 || c.r13.u64!=initial.r13.u64 || c.r14.u64!=initial.r14.u64 ||
       c.r30.u64!=initial.r30.u64 || c.r31.u64!=initial.r31.u64 || c.f14.u64!=initial.f14.u64) {
      registersPreserved.store(false);
      std::fprintf(stderr,"driver participant registers role=%u r1=%llX/%llX r13=%llX/%llX r14=%llX/%llX r30=%llX/%llX r31=%llX/%llX f14=%llX/%llX\n",number,
        c.r1.u64,initial.r1.u64,c.r13.u64,initial.r13.u64,c.r14.u64,initial.r14.u64,c.r30.u64,initial.r30.u64,c.r31.u64,initial.r31.u64,c.f14.u64,initial.f14.u64);
    }
    result[number-1u]=c;
  };
  std::thread outer(worker,1u);
  const ULONGLONG until=GetTickCount64()+2000u;
  while(!entered.load(std::memory_order_acquire) && GetTickCount64()<until)Sleep(1);
  if(!entered.load())scheduled.store(false);
  std::thread joined(worker,2u);outer.join();joined.join();
  if(!scheduled.load() || entered.load()!=3u || result[0].r3.u64 || result[1].r3.u64)
    std::fprintf(stderr,"driver participant scheduling order=%u route=%u scheduled=%u entered=%u returns=%llX/%llX\n",first,route,unsigned(scheduled.load()),entered.load(),result[0].r3.u64,result[1].r3.u64);
  // The first finisher resets S+18 before its marker rendezvous. The other
  // participant then repeats iteration one: SDK callback order is 0, 1, 1.
  // Requiring two callbacks would reject this actual retail interleaving.
  std::vector<uint64_t> iterations;for(const auto& e:calls)if(e[0]==1u)iterations.push_back(e[4]);
  check(scheduled.load() && entered.load()==3u && iterations==std::vector<uint64_t>{0u,1u,1u} &&
        registersPreserved.load() && !result[0].r3.u64 && !result[1].r3.u64,
        "participants order "+std::to_string(first)+" route "+std::to_string(route)+" use real event rendezvous");
  concurrent=false;
  PPCContext normalized=context();normalized.r3.u64=result[0].r3.u64;
  return capture(normalized);
}
void contendedLock(unsigned route) {
  prepare(Case::Empty);writeFixtureWord(state,1u);PPCContext c=context();
  std::atomic<bool> released{false};uint8_t* const base=K::getGuestBase();
  std::thread owner([&] {
    K::setGuestMemory(base,0x100000000ull);Sleep(20);released.store(true,std::memory_order_release);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(K::guestVAToHost(state)),0);
  });
  invoke(c,route,"contended driver");const bool waited=released.load(std::memory_order_acquire);owner.join();
  check(waited,"contended driver route "+std::to_string(route)+" waits for actual lock owner");
  check(c.r3.u64==pumpContext && rd(state)==0u && rd(state+0x30u)==0u,
        "contended driver route "+std::to_string(route)+" preserves empty return and lock state");
}
Result callbackRace(unsigned route,unsigned iteration) {
  prepare(iteration ? Case::DataRepeat : Case::DataOne);
  handlerSwitchRace=true;switchIteration=iteration;raceDone.store(false);raceValid.store(true);
  N::setDriverCallbackObserver(afterDriverUnlock);
  PPCContext c=context();invoke(c,route,"race driver iteration "+std::to_string(iteration));
  N::setDriverCallbackObserver(nullptr);handlerSwitchRace=false;
  check(raceDone.load() && raceValid.load(),"handler switch iteration "+std::to_string(iteration)+
        " route "+std::to_string(route)+" occurs under real driver lock");
  return capture(c);
}
}
int runNativePumpDriverTest(const char* mode) {
  std::setvbuf(stdout,nullptr,_IONBF,0);nativeEnabled=std::strcmp(mode,"gate-off")!=0;
  K::registerStubs();sr::guest::initializeDispatchTable();K::kpcrBindCurrent(stack,stack-0x10000u,3);
  check(N::driverGateOn()==nativeEnabled,"requested native driver gate");
  check(PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110840u)==sub_82110840,"production dispatch resolves driver override");
  auto& registry=K::ExportRegistry::instance();originalSet=registry.getExport("KeSetEvent");
  originalReset=registry.getExport("KeResetEvent");originalWait=registry.getExport("KeWaitForSingleObject");
  registry.registerExport("KeSetEvent",setEvent);registry.registerExport("KeResetEvent",resetEvent);registry.registerExport("KeWaitForSingleObject",waitEvent);
  const size_t index=(callback-PPC_CODE_BASE)>>2,index2=(secondCallback-PPC_CODE_BASE)>>2;
  PPCFunc* original=K::g_dispatchTable[index];PPCFunc* original2=K::g_dispatchTable[index2];
  K::g_dispatchTable[index]=handler;K::g_dispatchTable[index2]=handler2;
  for(unsigned i=0;i<names.size();++i) {
    const Case kind=Case(i);const Result oracle=run(kind,0u);
    check(oracleValid(kind,oracle),std::string(names[i])+": actual SDK fixture is exercised");
    check(run(kind,1u)==oracle,std::string(names[i])+": dispatch equals actual SDK");
    check(run(kind,2u)==oracle,std::string(names[i])+": direct equals actual SDK");
  }
  denied();for(unsigned route=0;route<3u;++route)contendedLock(route);
  for(unsigned first=1;first<=2u;++first) {
    const Result oracle=participants(0u,first);
    check(word(oracle.device,0x2A44u)==6u && word(oracle.device,0x2A6Cu)==0u && word(oracle.device,0x2A74u)==0u,
          "participants order "+std::to_string(first)+": SDK exercises both marker participants");
    check(participants(1u,first)==oracle,"participants order "+std::to_string(first)+": dispatch equals actual SDK");
    check(participants(2u,first)==oracle,"participants order "+std::to_string(first)+": direct equals actual SDK");
  }
  for(unsigned iteration=0;iteration<=1u;++iteration) {
    const Result oracle=callbackRace(0u,iteration);
    std::vector<Event> handlers;for(const auto& e:oracle.events)if(e[0]<=2u)handlers.push_back(e);
    bool valid=handlers.size()==(iteration ? 3u : 1u) && handlers[iteration][0]==1u && handlers[iteration][2]==0x11223344u;
    if(iteration)valid &= handlers[2][0]==2u && handlers[2][2]==0x66778899u;
    check(valid,"handler switch iteration "+std::to_string(iteration)+": SDK captures old callback and arguments");
    check(callbackRace(1u,iteration)==oracle,"handler switch iteration "+std::to_string(iteration)+": dispatch equals actual SDK");
    check(callbackRace(2u,iteration)==oracle,"handler switch iteration "+std::to_string(iteration)+": direct equals actual SDK");
  }
  K::g_dispatchTable[index]=original;K::g_dispatchTable[index2]=original2;
  registry.registerExport("KeSetEvent",originalSet);registry.registerExport("KeResetEvent",originalReset);registry.registerExport("KeWaitForSingleObject",originalWait);
  std::printf("[NATIVE_PUMP_DRIVER_TEST] %d passed, %d failed\n",passed,failed);return failed ? 1 : 0;
}
