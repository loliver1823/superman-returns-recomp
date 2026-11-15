// Actual retail marker replay vs the native body and production dispatch.
// No GPU worker: submission/bin helpers use their SDK ring path in this bank.
// Cursor scope is tested here; generation binding and driver retirement are not.
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
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

extern "C" void __imp__sub_82110460(PPCContext& __restrict, uint8_t*);
namespace K = sr::kernel;
namespace N = sr::native::pump;
namespace {
constexpr uint32_t dev=0x70600000u, state=dev+0x2A3Cu, records=0x90640000u;
constexpr uint32_t ring=0x70660000u, fb=0x70661000u, cell=0x70662000u, stack=0x70580000u;
constexpr uint32_t scratch=0x7F100000u;
int passed=0, failed=0;
bool nativeEnabled=false, nesting=false, nestedValid=true;
using Event=std::array<uint32_t,6>;
std::vector<Event> imports;
std::vector<Event> pixCalls;
std::vector<uint32_t> observed;
K::ExportFn originalLock, originalUnlock;
uint32_t rd(uint32_t va) { return K::readGuestU32BE(va); }
void wr(uint32_t va,uint32_t value) { K::writeGuestBE32(va,value); }
void check(bool ok,const std::string& name) {
  std::printf("%s %s\n",ok ? "  ok " : "FAIL:",name.c_str()); ok ? ++passed : ++failed;
}
uint32_t lockL2(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  imports.push_back({1u,a,b,c,d,e}); return originalLock(a,b,c,d,e,f);
}
uint32_t unlockL2(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f) {
  // Observe clearing before the actual unlock, rather than only the final buffer.
  bool cleared=true;
  for (uint32_t i=0;i<0x20000u;i+=4u) cleared &= rd(scratch+i)==0u;
  imports.push_back({2u,a,uint32_t(cleared),0u,0u,0u}); return originalUnlock(a,b,c,d,e,f);
}
void pixCallback(PPCContext& c,uint8_t*) {
  const auto token=N::currentPublication();
  pixCalls.push_back({c.r3.u32,token.record,token.state,c.r4.u32,c.r5.u32,c.r6.u32});
  c.r3.u64=0u;
}
PPCContext context(uint64_t at=records,uint64_t s=state) {
  PPCContext c{}; c.r1.u64=stack; c.r3.u64=at; c.r4.u64=s; c.r13.u64=K::kpcrCurrent();
  c.r14.u64=0xAABBCCDDEEFF0011ull; c.r30.u64=0xFEED0123456789ABull;
  c.r31.u64=0x1122334455667788ull; c.f14.u64=0x1234567812345678ull;
  return c;
}
void observe(N::Publication token) {
  observed.push_back(token.record);
  nestedValid &= token.state==state && N::currentPublication().record==token.record;
  if (!nesting) return;
  nesting=false;
  PPCContext inner=context(records+0x1800u); inner.r1.u64=stack-0x4000u;
  const bool admitted=N::replayMarkers(inner,K::getGuestBase());
  nestedValid &= admitted && inner.r3.u64==records+0x1808u &&
                 N::currentPublication().record==token.record && N::currentPublication().state==token.state;
}
enum class Case { Data, Callback, Terminal, Header, Segments, Link, DepthColour,
  TileDepth, TileColour, PauseTaken, PauseUntaken, L2, CoverageInactive, CoverageActive,
  HighCursor, HighState, Wrap, ZeroCount };
constexpr std::array<const char*,18> names={"data stop","callback stop","terminal stop","header copy",
  "identical descriptors","software link","depth neutral colour post","tile depth replay","tile colour entry",
  "pause taken","pause untaken","L2 lock clear unlock","coverage inactive","coverage linked rectangles",
  "full cursor return","full state pause return","ring wrap","zero dword descriptor"};
struct Fixture { uint64_t at=records,s=state; std::vector<uint32_t> cursor; };
void segment(uint32_t at,uint32_t count=7u,uint32_t phys=0x10641234u) {
  wr(at,0x82000000u|count); wr(at+4u,phys);
}
Fixture fixture(Case kind) {
  std::memset(K::guestVAToHost(dev),0,0x5000u);
  std::memset(K::guestVAToHost(records),0,0x2000u);
  std::memset(K::guestVAToHost(ring),0xCC,0x800u);
  std::memset(K::guestVAToHost(fb),0,0x100u);
  std::memset(K::guestVAToHost(stack-0x8000u),0xDD,0x8000u);
  std::memset(K::guestVAToHost(scratch-4u),0xA5,0x20008u);
  wr(0x82000948u,cell); wr(cell,dev); wr(dev+0x2A10u,fb);
  wr(dev+0x34A4u,ring); wr(dev+0x34A8u,511u);
  wr(state+0x60u,0xC0000000u); wr(state+0x16Cu,0x7FFFFFFFu);
  imports.clear(); observed.clear(); pixCalls.clear(); nestedValid=true;
  Fixture f;
  switch (kind) {
    case Case::Data: wr(records,0x01230002u); break;
    case Case::Callback: wr(records,0x81ABCDEFu); break;
    case Case::Terminal: wr(records,0xC0000000u); break;
    case Case::Header:
      wr(records,0x80000000u);
      for (uint32_t i=0;i<62u;++i) wr(records+4u+i*4u,0x12340000u+i);
      wr(records+252u,0xC0000000u); wr(state+0x2Cu,11u); wr(state+0x16Cu,0u); break;
    case Case::Segments: case Case::Wrap: case Case::ZeroCount: {
      const uint32_t count=kind==Case::Segments ? 0x12ABCDu : kind==Case::Wrap ? 0xFFFFFFu : 0u;
      segment(records,count); segment(records+8u,count); wr(records+16u,0xC0000000u);
      f.cursor={records,records+8u};
      if (kind==Case::Wrap) { wr(dev+0x2A44u,510u); wr(fb+0x3Cu,256u); }
      break;
    }
    case Case::Link:
      wr(records,records+0x100u); segment(records+0x104u); wr(records+0x10Cu,0xC0000000u);
      f.cursor={records+0x104u}; break;
    case Case::DepthColour:
      wr(records,0x84000000u); segment(records+4u);
      wr(records+12u,0x85000000u); segment(records+16u);
      wr(records+24u,0x86000000u); segment(records+28u); wr(records+36u,0xC0000000u);
      f.cursor={records+4u,records+16u,records+4u,records+28u}; break;
    case Case::TileDepth: case Case::TileColour:
      wr(state+0x78u,2u); wr(state+0x74u,kind==Case::TileColour ? 2u : 0u);
      if (kind==Case::TileColour) { wr(state+0x2Cu,1u); wr(state+0x174u,records+28u); }
      wr(records,0x87000000u); wr(records+4u,0x84000000u); segment(records+8u);
      wr(records+16u,0x85000000u); segment(records+20u); wr(records+28u,0x86000000u);
      wr(records+32u,0x88000000u); segment(records+36u); wr(records+44u,0xC0000000u);
      f.cursor=kind==Case::TileColour ? std::vector<uint32_t>{records+8u,records+36u} :
        std::vector<uint32_t>{records+8u,records+20u,records+8u,records+8u,records+20u,records+8u,records+36u};
      break;
    case Case::PauseTaken: case Case::PauseUntaken: case Case::HighState:
      wr(state+0x16Cu,kind==Case::PauseUntaken ? 0x10u : 0x20u);
      wr(records,0x8C000000u); wr(records+4u,0x20u); segment(records+8u);
      wr(records+16u,0xC0000000u);
      if (kind==Case::PauseUntaken) f.cursor={records+8u};
      if (kind==Case::HighState) f.s|=0x1234567800000000ull;
      break;
    case Case::L2:
      wr(records,0x89000000u); wr(records+4u,0x8A000000u); wr(records+8u,0xC0000000u); break;
    case Case::CoverageInactive: case Case::CoverageActive: {
      wr(records,0x8B000000u); wr(records+4u,records+0x400u); wr(records+8u,records+0x600u);
      wr(records+12u,0xC0000000u);
      wr(state+0x16Cu,kind==Case::CoverageActive ? 0x80000000u : 0x7FFFFFFFu);
      wr(state+0x78u,1u); wr(state+0x7Cu,0u); wr(state+0x80u,0u);
      wr(state+0x84u,16u); wr(state+0x88u,16u);
      for (uint32_t i=0;i<3u;++i) {
        const uint32_t block=records+0x400u+i*0x40u;
        wr(block,block+20u); wr(block+4u,records+0x900u+i*16u);
        wr(block+8u,0u); wr(block+12u,0x00010001u); wr(block+20u,0xC0000000u);
      }
      wr(records+0x600u,records+0x608u); wr(records+0x604u,records+0x440u);
      wr(records+0x608u,records+0x620u); wr(records+0x620u,records+0x628u);
      wr(records+0x624u,records+0x480u); wr(records+0x628u,0xC0000000u);
      break;
    }
    case Case::HighCursor:
      f.at|=0x1234567800000000ull; segment(records); wr(records+8u,0xC0000000u);
      f.cursor={records}; break;
  }
  return f;
}
struct Result {
  std::array<uint64_t,7> regs;
  std::vector<uint8_t> device, list, ringBytes, scratchBytes;
  std::vector<Event> calls;
  bool operator==(const Result&) const=default;
};
std::vector<uint8_t> bytes(uint32_t va,uint32_t size) { return {K::guestVAToHost(va),K::guestVAToHost(va)+size}; }
Result capture(const PPCContext& c) {
  return {{c.r3.u64,c.r1.u64,c.r13.u64,c.r14.u64,c.r30.u64,c.r31.u64,c.f14.u64},
    bytes(dev,0x5000u),bytes(records,0x2000u),bytes(ring,0x800u),bytes(scratch-4u,0x20008u),imports};
}
uint32_t word(const std::vector<uint8_t>& data,uint32_t offset) {
  return uint32_t(data[offset])<<24 | uint32_t(data[offset+1u])<<16 |
         uint32_t(data[offset+2u])<<8 | data[offset+3u];
}
bool oracleFixtureValid(Case kind,const Result& result) {
  uint64_t returned=records; size_t submissions=0;
  switch (kind) {
    case Case::Data: case Case::Callback: case Case::Terminal: break;
    case Case::Header: returned+=252u; break;
    case Case::Segments: case Case::Wrap: case Case::ZeroCount: returned+=16u; submissions=2u; break;
    case Case::Link: returned+=0x10Cu; submissions=1u; break;
    case Case::DepthColour: returned+=36u; submissions=4u; break;
    case Case::TileDepth: returned+=44u; submissions=7u; break;
    case Case::TileColour: returned+=44u; submissions=2u; break;
    case Case::PauseTaken: returned=state+0x60u; break;
    case Case::PauseUntaken: returned+=16u; submissions=1u; break;
    case Case::HighState: returned=0x1234567800000000ull+state+0x60u; break;
    case Case::L2: returned+=8u; break;
    case Case::CoverageInactive: case Case::CoverageActive: returned+=12u; break;
    case Case::HighCursor: returned+=0x1234567800000000ull+8u; submissions=1u; break;
  }
  // Count the actual oracle ring output independently of native cursor scope.
  uint32_t at=kind==Case::Wrap ? 510u : 0u;
  const uint32_t end=word(result.device,0x2A44u);
  size_t count=0;
  for (unsigned guard=0;at!=end && guard<512u;++guard) {
    const uint32_t header=word(result.ringBytes,at*4u);
    if (header==0xC0013F00u) { ++count; at=(at+3u)&511u; }
    else if (header==0xC0006200u) at=(at+2u)&511u;
    else return false;
  }
  if (kind==Case::CoverageActive)
    for (uint32_t i=0;i<3u;++i) if (word(result.list,0x908u+i*16u)!=0x80000003u) return false;
  return result.regs[0]==returned && count==submissions && at==end;
}
Result run(Case kind,unsigned route) {
  const Fixture f=fixture(kind); PPCContext c=context(f.at,f.s);
  if (route==0u) __imp__sub_82110460(c,K::getGuestBase());
  else if (route==1u) PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110460u)(c,K::getGuestBase());
  else {
    const bool admitted=N::replayMarkers(c,K::getGuestBase());
    check(admitted==nativeEnabled,std::string(names[unsigned(kind)])+": explicit admission gate");
    if (!admitted) __imp__sub_82110460(c,K::getGuestBase());
  }
  if (route) {
    check(observed==(nativeEnabled ? f.cursor : std::vector<uint32_t>{}) && nestedValid &&
          !N::currentPublication().record && !N::currentPublication().state,
          std::string(names[unsigned(kind)])+(route==1u ? ": dispatch cursor scope" : ": direct cursor scope"));
  }
  return capture(c);
}
void deniedInputs() {
  for (unsigned invalid=0;invalid<5u;++invalid) {
    fixture(Case::Terminal); PPCContext c=context();
    if (invalid==0u) c.r3.u64=0u;
    if (invalid==1u) ++c.r3.u64;
    if (invalid==2u) ++c.r4.u64;
    if (invalid==3u) ++c.r1.u64;
    if (invalid==4u) c.r1.u64=0x80u;
    const Result before=capture(c); const auto stackBefore=bytes(stack-0x8000u,0x8000u);
    check(!N::replayMarkers(c,K::getGuestBase()) && capture(c)==before &&
          bytes(stack-0x8000u,0x8000u)==stackBefore && observed.empty(),
          "invalid initial input "+std::to_string(invalid)+" declines before mutation");
  }
  for (DWORD protection : {DWORD(PAGE_READONLY),DWORD(PAGE_NOACCESS),DWORD(PAGE_READWRITE|PAGE_GUARD)}) {
    fixture(Case::Terminal); PPCContext c=context(); DWORD old=0,ignored=0;
    const auto stackBefore=bytes(stack-0x8000u,0x8000u);
    const bool protectedOk=VirtualProtect(K::guestVAToHost(state),0x17Cu,protection,&old)!=0;
    const bool declined=protectedOk && !N::replayMarkers(c,K::getGuestBase());
    const bool restored=protectedOk && VirtualProtect(K::guestVAToHost(state),0x17Cu,old,&ignored);
    check(declined && restored && c.r3.u64==records && bytes(stack-0x8000u,0x8000u)==stackBefore &&
          observed.empty() && imports.empty(),"protected state "+std::to_string(protection)+" declines before mutation");
  }
  for (uint32_t marker : {0x80000000u,0x82000000u,0x8B000000u,0x8C000000u}) {
    fixture(Case::Terminal);
    const uint32_t at=records+(marker==0x80000000u ? 0xF80u : marker==0x8B000000u ? 0xFF8u : 0xFFCu);
    wr(at,marker); PPCContext c=context(at); const Result before=capture(c);
    const auto stackBefore=bytes(stack-0x8000u,0x8000u); DWORD old=0,ignored=0;
    const bool protectedOk=VirtualProtect(K::guestVAToHost(records+0x1000u),4096u,PAGE_NOACCESS,&old)!=0;
    const bool declined=protectedOk && !N::replayMarkers(c,K::getGuestBase());
    const bool restored=protectedOk && VirtualProtect(K::guestVAToHost(records+0x1000u),4096u,old,&ignored);
    check(declined && restored && capture(c)==before && bytes(stack-0x8000u,0x8000u)==stackBefore && observed.empty(),
          "cross-page initial marker "+std::to_string(marker>>24)+" declines before mutation");
  }
  for (DWORD protection : {DWORD(PAGE_NOACCESS),DWORD(PAGE_READONLY|PAGE_GUARD)}) {
    fixture(Case::Terminal); PPCContext c=context(); const Result before=capture(c);
    const auto stackBefore=bytes(stack-0x8000u,0x8000u); DWORD old=0,ignored=0;
    const bool protectedOk=VirtualProtect(K::guestVAToHost(records),4096u,protection,&old)!=0;
    const bool declined=protectedOk && !N::replayMarkers(c,K::getGuestBase());
    const bool restored=protectedOk && VirtualProtect(K::guestVAToHost(records),4096u,old,&ignored);
    check(declined && restored && capture(c)==before && bytes(stack-0x8000u,0x8000u)==stackBefore,
          "protected initial marker "+std::to_string(protection)+" declines before read");
  }
  fixture(Case::L2); PPCContext c=context(); const Result before=capture(c);
  const auto stackBefore=bytes(stack-0x8000u,0x8000u); DWORD old=0,ignored=0;
  const bool protectedOk=VirtualProtect(K::guestVAToHost(scratch),4096u,PAGE_READONLY,&old)!=0;
  const bool declined=protectedOk && !N::replayMarkers(c,K::getGuestBase());
  const bool restored=protectedOk && VirtualProtect(K::guestVAToHost(scratch),4096u,old,&ignored);
  check(declined && restored && capture(c)==before && bytes(stack-0x8000u,0x8000u)==stackBefore && imports.empty(),
        "protected initial L2 scratch declines before lock or mutation");
  fixture(Case::Terminal); c=context();
  const bool readOnly=VirtualProtect(K::guestVAToHost(records),4096u,PAGE_READONLY,&old)!=0;
  const bool admitted=readOnly && N::replayMarkers(c,K::getGuestBase());
  const bool restoredRead=readOnly && VirtualProtect(K::guestVAToHost(records),4096u,old,&ignored);
  check(readOnly && restoredRead && admitted==nativeEnabled && c.r3.u64==records,
        "read-only terminal admits without writing source");
}
void actualPublicationScope() {
  constexpr uint32_t callback=0x822949D8u;
  const size_t index=(callback-PPC_CODE_BASE)>>2;
  PPCFunc* const original=K::g_dispatchTable[index];
  K::g_dispatchTable[index]=pixCallback;
  for (unsigned route=0;route<3u;++route) {
    fixture(Case::Segments); wr(dev+0x4E84u,callback); PPCContext c=context();
    if (route==0u) __imp__sub_82110460(c,K::getGuestBase());
    else if (route==1u) PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110460u)(c,K::getGuestBase());
    else if (!N::replayMarkers(c,K::getGuestBase())) __imp__sub_82110460(c,K::getGuestBase());
    std::vector<Event> expected;
    for (uint32_t i=0;i<2u;++i) {
      const bool scoped=route && nativeEnabled;
      expected.push_back({1u,scoped ? records+i*8u : 0u,scoped ? state : 0u,0xD0641234u,0x12ABCDu,1u});
      expected.push_back({2u,scoped ? records+i*8u : 0u,scoped ? state : 0u,0u,(i+1u)*3u,0u});
    }
    check(pixCalls==expected && c.r3.u64==records+16u && !N::currentPublication().record,
          "actual publication route "+std::to_string(route)+" retains scope through SDK PIX callbacks");
  }
  K::g_dispatchTable[index]=original;
}
void spin(unsigned route) {
  fixture(Case::Terminal); wr(records,0x83000000u); PPCContext c=context();
  uint8_t* const base=K::getGuestBase();
  std::atomic<bool> producerReleased{false};
  std::thread producer([&] {
    K::setGuestMemory(base,0x100000000ull); Sleep(20);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(K::guestVAToHost(records)),LONG(_byteswap_ulong(0xC0000000u)));
    producerReleased.store(true,std::memory_order_release);
  });
  if (route==0u) __imp__sub_82110460(c,base);
  else if (route==1u) PPC_LOOKUP_FUNC(base,0x82110460u)(c,base);
  else { if (!N::replayMarkers(c,base)) __imp__sub_82110460(c,base); }
  // Check before join, so skipping the SDK spin cannot be concealed by waiting for the producer.
  const bool waited=producerReleased.load(std::memory_order_acquire) || rd(records)==0xC0000000u;
  producer.join();
  check(waited && c.r3.u64==records,"spin route "+std::to_string(route)+" waits for actual producer publication");
}
}
int runNativePumpTest(const char* mode) {
  std::setvbuf(stdout,nullptr,_IONBF,0);
  nativeEnabled=std::strcmp(mode,"gate-off")!=0;
  K::registerStubs(); sr::guest::initializeDispatchTable(); K::kpcrBindCurrent(stack,stack-0x10000u,3);
  check(N::markerGateOn()==nativeEnabled,"requested native marker gate");
  check(PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110460u)==sub_82110460,"production dispatch resolves marker override");
  auto& registry=K::ExportRegistry::instance();
  originalLock=registry.getExport("KeLockL2"); originalUnlock=registry.getExport("KeUnlockL2");
  registry.registerExport("KeLockL2",lockL2); registry.registerExport("KeUnlockL2",unlockL2);
  N::setPublicationObserver(observe);
  for (unsigned i=0;i<names.size();++i) {
    const Case kind=Case(i); const Result expected=run(kind,0u);
    check(oracleFixtureValid(kind,expected),std::string(names[i])+": oracle fixture exercises intended path");
    check(run(kind,1u)==expected,std::string(names[i])+": dispatch equals actual SDK");
    check(run(kind,2u)==expected,std::string(names[i])+": direct equals actual SDK");
  }
  deniedInputs(); for (unsigned route=0;route<3u;++route) spin(route);
  actualPublicationScope();
  fixture(Case::Segments); segment(records+0x1800u); wr(records+0x1808u,0xC0000000u);
  nesting=true; PPCContext c=context(); PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110460u)(c,K::getGuestBase());
  check(!nativeEnabled || (!nesting && nestedValid && observed==std::vector<uint32_t>{records,records+0x1800u,records+8u}),
        "nested publication restores exact outer cursor");
  check(!N::currentPublication().record && !N::currentPublication().state,"publication scope clears on return");
  N::setPublicationObserver(nullptr); registry.registerExport("KeLockL2",originalLock);
  registry.registerExport("KeUnlockL2",originalUnlock);
  std::printf("[NATIVE_PUMP_TEST] %d passed, %d failed\n",passed,failed);
  return failed ? 1 : 0;
}
