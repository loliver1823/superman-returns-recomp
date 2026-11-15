// Actual 82110C78 plus actual allocator/coherency/callback/append helpers.
// The allocator's supported user callback supplies bounded mapped allocations.
// No game, GPU worker or fabricated helper implementation runs in this bank.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "ppc/ppc_recomp_shared.h"
#include "guest/guest_harness.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/kpcr.hpp"
#include "kernel/native_recording.hpp"
#include "kernel/native_flush.hpp"
#include "kernel/stubs.hpp"
#include "gfx/pm4_engine.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

extern "C" void __imp__sub_82110C78(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820FDA80(PPCContext& __restrict, uint8_t*);
namespace K = sr::kernel;
namespace N = sr::native::recording;
namespace sr::native::flush { bool flushSegmentless(PPCContext&, uint8_t*); }
namespace {
constexpr uint32_t dev=0x706C0000u, object=0x706D0000u, buffer=0x906E0000u;
constexpr uint32_t ring=0x706F0000u, fb=0x706F1000u, cell=0x706F2000u, stack=0x70580000u;
constexpr uint32_t allocatorCallback=0x82110800u;
int passed=0, failed=0;
bool enabled=false;
bool tailMode=false;
struct TailToken { uint64_t id; uint32_t phys, dwords; bool chained; };
std::vector<TailToken> tailTokens;
void observeTail(uint64_t id,uint32_t phys,uint32_t dwords,bool chained) {
  tailTokens.push_back({id,phys,dwords,chained});
}
uint32_t coherent=0, failRequest=0;
bool fullPointers=false;
bool latePix=false;
std::vector<std::array<uint64_t,6>> allocations;
uint32_t rd(uint32_t address) { return K::readGuestU32BE(address); }
void wr(uint32_t address,uint32_t value) { K::writeGuestBE32(address,value); }
uint32_t physical(uint32_t address) { return (((address>>20)+0x200u)&0x1000u)+(address&0x1FFFFFFFu); }
void check(bool ok,const std::string& name) {
  std::printf("%s%s\n",ok ? "  ok  " : "FAIL: ",name.c_str()); ok ? ++passed : ++failed;
}
PPC_FUNC(allocate) {
  const uint32_t bytes=rd(ctx.r5.u32);
  allocations.push_back({bytes,ctx.r6.u64,ctx.r1.u64,ctx.r3.u64,ctx.r4.u64,ctx.r5.u64});
  uint32_t result=bytes==400u ? buffer+0x2000u : bytes==128u ? buffer+0x3000u : buffer+0x1000u;
  if (failRequest==bytes) result=0;
  if (latePix && bytes==128u) wr(dev+0x4DF4u,object);
  ctx.r3.u64=result ? uint64_t(result)+(fullPointers ? 0x1234000000000000ull : 0ull) : 0ull;
}
enum class Case { Plain, Deferred, Whole, Both, PreFailed, CoherentFailed, KickFailed, AppendFailed,
                  NoGpu, NoGpuDeferred, Pix, ZeroRoot, CpuMask, FullPointers, Direct, DirectDeferred, AllOnes };
const std::array<const char*,17> names={"plain","deferred","whole","both","pre-failed","coherent-failed",
    "kick-failed","append-failed","no-gpu","no-gpu-deferred","pix","zero-root","cpu-mask","full-pointers","direct","direct-deferred","all-ones-range"};
void prepare(Case kind) {
  std::memset(K::guestVAToHost(dev),0,0x6000u);
  std::memset(K::guestVAToHost(object),0,0x200u);
  std::memset(K::guestVAToHost(buffer),0xCC,0x6000u);
  std::memset(K::guestVAToHost(ring),0xDD,0x200u);
  std::memset(K::guestVAToHost(fb),0,0x200u);
  std::memset(K::guestVAToHost(stack-0x1000u),0xA5,0x1000u);
  wr(0x82000948u,cell); wr(cell,dev);
  wr(dev+0x28u,buffer+0x500u); wr(dev+0x2Cu,buffer+0x5F00u); wr(dev+0x30u,buffer+0x5E60u);
  wr(dev+0x3354u,object); wr(object+164u,0x11223344u); wr(object+172u,allocatorCallback);
  wr(dev+0x34CCu,0x7FFFFFFFu);
  wr(dev+0x2A74u,3u); wr(dev+0x2A10u,fb); wr(dev+0x2A14u,fb+0x80u);
  wr(dev+0x34A4u,ring); wr(dev+0x34A8u,63u);
  // Actual SDK compares the complete packed range with 00000000FFFFFFFF.
  // FFFFFFFFFFFFFFFF requests a range flush; it is not the empty sentinel.
  wr(dev+0x2D18u,0u); wr(dev+0x2D1Cu,0xFFFFFFFFu);
  wr(dev+0x3290u,buffer+0x100u); wr(dev+0x3294u,17u);
  for (unsigned i=0;i<4u;++i) {
    const uint32_t offset=i==0 ? 0x32C8u : i==1 ? 0x32A0u : i==2 ? 0x32B4u : 0x32DCu;
    const uint32_t node=i==3u ? buffer+0x4100u : buffer+0x100u+i*0x40u;
    wr(dev+offset,node); wr(dev+offset+4u,node); wr(dev+offset+8u,node+0x10u);
    wr(dev+offset+12u,buffer+0x5FFCu); wr(dev+offset+16u,dev); wr(node+0x10u,0xC0000000u);
  }
  const uint32_t restored[]={buffer+0x400u,buffer+0x4800u,0x71717171u,0x72727272u,buffer+0x4900u,0x74747474u};
  for (unsigned i=0;i<6u;++i) wr(dev+0x32F0u+i*4u,restored[i]);
  coherent=0;failRequest=0;fullPointers=kind==Case::FullPointers;latePix=kind==Case::Pix;
  if (kind==Case::Deferred || kind==Case::Both || kind==Case::CoherentFailed ||
      kind==Case::NoGpuDeferred || kind==Case::DirectDeferred) {
    wr(dev+0x2D18u,0x12345u); wr(dev+0x2D1Cu,0x15678u); coherent+=44u;
  }
  if (kind==Case::Whole || kind==Case::Both) {
    K::guestVAToHost(dev+0x2A3Au)[0]=4u;wr(dev+0x34ACu,buffer);wr(dev+0x34B0u,buffer+0x6000u);coherent+=44u;
  }
  if (kind==Case::AllOnes) { wr(dev+0x2D18u,0xFFFFFFFFu);coherent=44u; }
  if (kind==Case::PreFailed) K::guestVAToHost(dev+0x2A39u)[0]=0xC2u;
  if (kind==Case::CoherentFailed)failRequest=44u;
  if (kind==Case::KickFailed)failRequest=400u;
  if (kind==Case::AppendFailed)failRequest=128u;
  if (kind==Case::NoGpu || kind==Case::NoGpuDeferred || kind==Case::Pix) K::guestVAToHost(dev+0x2A39u)[0]=2u;
  if (kind==Case::ZeroRoot)wr(dev+0x32C8u,0u);
  if (kind==Case::Direct || kind==Case::DirectDeferred)wr(dev+0x2A74u,0u);
  allocations.clear();tailTokens.clear();
}
PPCContext context(Case kind) {
  PPCContext c{};c.r3.u64=dev;c.r4.u64=0x99887766u;c.r5.u64=kind==Case::CpuMask ? 0x23000000u : 4u;
  c.r1.u64=stack;c.r13.u64=K::kpcrCurrent();c.r23.u64=23u;c.r26.u64=26u;c.r28.u64=28u;c.r31.u64=31u;
  if(fullPointers) { c.r3.u64|=0xAA11000000000000ull;c.r1.u64|=0xAA22000000000000ull;c.r5.u64|=0xAA33000000000000ull; }
  return c;
}
struct Result {
  std::vector<uint8_t> device,bytes,ringBytes;
  std::vector<std::array<uint64_t,6>> events;
  std::array<uint64_t,8> regs{};
  std::array<uint32_t,3> frame{};
  bool operator==(const Result& r) const {
    return device==r.device && bytes==r.bytes && ringBytes==r.ringBytes && events==r.events && regs==r.regs && frame==r.frame;
  }
};
bool verifyTail(Case kind,unsigned route,const Result& result);
Result capture(const PPCContext& c) {
  Result r;
  r.device.assign(K::guestVAToHost(dev),K::guestVAToHost(dev)+0x4E00u);
  r.bytes.assign(K::guestVAToHost(buffer),K::guestVAToHost(buffer)+0x6000u);
  r.ringBytes.assign(K::guestVAToHost(ring),K::guestVAToHost(ring)+0x200u);
  // This compares eight selected registers and three stack words, not the full ABI.
  r.events=allocations;r.regs={c.r3.u64,c.r1.u64,c.r13.u64,c.r23.u64,c.r26.u64,c.r28.u64,c.r31.u64,c.f14.u64};
  r.frame={rd(stack-0xB0u),rd(stack-0x60u),rd(stack-0x5Cu)};
  return r;
}
Result run(Case kind,unsigned route) {
  prepare(kind);PPCContext c=context(kind);const PPCContext before=c;
  if(route==0u)__imp__sub_82110C78(c,K::getGuestBase());
  else if(route==1u)PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110C78u)(c,K::getGuestBase());
  else {
    const bool handled=N::endRecordingKick(c,K::getGuestBase());
    check(handled==enabled,std::string(names[size_t(kind)])+": direct admission follows gate");
    if(!handled) {check(std::memcmp(&c,&before,sizeof(c))==0,std::string(names[size_t(kind)])+": gate-off context unchanged");__imp__sub_82110C78(c,K::getGuestBase());}
  }
  Result result=capture(c);
  if(tailMode && route)verifyTail(kind,route,result);
  return result;
}
uint32_t word(const std::vector<uint8_t>& bytes,uint32_t at) {
  return uint32_t(bytes[at])<<24|uint32_t(bytes[at+1])<<16|uint32_t(bytes[at+2])<<8|bytes[at+3];
}
struct TailEffect {
  std::array<uint8_t,32> scratch{};
  std::vector<std::array<uint32_t,3>> events;
  bool bad=false;
  bool ownedRecord=false;
  static uint8_t* map(void* user,uint32_t address) {
    auto& effect=*static_cast<TailEffect*>(user);const uint32_t first=physical(fb+0x80u);
    if(address<first || address-first>effect.scratch.size()-4u){effect.bad=true;return nullptr;}
    return effect.scratch.data()+address-first;
  }
  static void interrupt(void* user,uint32_t mask) {
    auto& effect=*static_cast<TailEffect*>(user);effect.events.push_back({5u,0u,mask});
    const uint8_t* p=effect.scratch.data();
    if((uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3])!=mask)effect.bad=true;
    std::memset(effect.scratch.data(),0,4u);
  }
  static void trace(void* user,uint32_t what,uint32_t address,uint32_t value) {
    auto& effect=*static_cast<TailEffect*>(user);effect.events.push_back({what,address,value});
    if(what!=1u)effect.bad=true;
    char part[192];const uint8_t* gap=nullptr;uint32_t gapBytes=0;
    if(sr::native::flush::cpSegPartDescribe(part,sizeof(part),nullptr,&gap,&gapBytes) && !gap && !gapBytes)
      effect.ownedRecord=true;
  }
  static bool running(void* user) {static_cast<TailEffect*>(user)->bad=true;return false;}
};
struct TailEffectResult {
  TailEffect effect;
  std::array<uint64_t,5> counts{};
  bool complete=false;
};
TailEffectResult executeTail(const TailToken* token,const Result& result,bool predicate) {
  TailEffect effect;
  auto engine=std::make_unique<sr::gfx::Pm4Engine>();
  engine->submitDraws=false;engine->cpMode=true;engine->physToHost=TailEffect::map;engine->user=&effect;
  engine->cp.user=&effect;engine->cp.interrupt=TailEffect::interrupt;engine->cp.running=TailEffect::running;
  engine->binMask=1u;engine->binSelect=predicate ? 1u : 0u;
  engine->writeReg(0x1DCu,0x31u);engine->writeReg(0x1DDu,physical(fb+0x80u));engine->cp.trace=TailEffect::trace;
  bool complete;
  if(token)complete=sr::native::flush::cpSegment(*engine,token->id,token->phys,token->dwords);
  else {
    engine->execute(result.bytes.data()+0x2000u,92u);
    complete=engine->lastConsumed==92u && !engine->nativeRecordFailed;
  }
  return {std::move(effect),{engine->cpWaits,engine->cpWaitsBlocked,engine->cpInterrupts,
      engine->predSkipped,engine->cpScratchWrites},complete};
}
bool verifyTail(Case kind,unsigned route,const Result& result) {
  const std::string label=std::string(names[size_t(kind)])+(route==1u ? ": dispatch tail " : ": direct tail ");
  const bool failure=kind==Case::PreFailed || kind==Case::CoherentFailed || kind==Case::KickFailed || kind==Case::AppendFailed;
  const bool expected=!failure && kind!=Case::NoGpu && kind!=Case::NoGpuDeferred && kind!=Case::Pix;
  check(tailTokens.size()==size_t(expected),label+"publication count preserves success/failure/PIX branches");
  if(tailTokens.size()!=size_t(expected) || !expected)return tailTokens.size()==size_t(expected);
  const TailToken token=tailTokens.front();const bool direct=kind==Case::Direct || kind==Case::DirectDeferred;
  check(token.id && token.phys==physical(buffer+0x2000u) && token.dwords==23u && token.chained!=direct,
      label+"exact owned tail identity and CPU append route");
  if(token.chained)check(sr::native::flush::segmentPublishId(token.phys,token.dwords)==token.id,
      label+"deferred handoff resolves registered callback record");
  const bool predicate=route==1u;
  const TailEffectResult literal=executeTail(nullptr,result,predicate),native=executeTail(&token,result,predicate);
  check(literal.complete && native.complete && !literal.effect.bad && !native.effect.bad &&
      literal.effect.scratch==native.effect.scratch && literal.effect.events==native.effect.events &&
      literal.counts==native.counts,label+"owned consumer equals actual SDK tail effects");
  check(native.counts[2]==uint64_t(predicate) && native.counts[0]==(predicate ? 2u : 0u) &&
      native.counts[3]==(predicate ? 0u : 3u),label+"predication and interrupt/wait ordering exercised");
  return true;
}
bool tailAdmission() {
  for(unsigned region=0;region<2u;++region)for(DWORD protection:{PAGE_READONLY,PAGE_NOACCESS,PAGE_GUARD|PAGE_READWRITE}) {
    prepare(Case::Plain);
    PPCContext c=context(Case::Plain);c.r4.u64=buffer+0x2000u-4u;c.r5.u64=0x04000001u;
    c.r6.u64=0x82110B00u;c.r7.u64=0x12345678u;
    const PPCContext before=c;
    const uint32_t address=region ? dev+0x2000u : buffer+0x2000u;
    std::array<uint8_t,4096> bytes;std::memcpy(bytes.data(),K::guestVAToHost(address),bytes.size());
    DWORD old=0,restored=0;
    const bool protectedOk=VirtualProtect(K::guestVAToHost(address),bytes.size(),protection,&old)!=0;
    sr::native::flush::ReplayKickCallback tail;
    const bool handled=protectedOk && tail.emit(c);
    const bool restoredOk=protectedOk && VirtualProtect(K::guestVAToHost(address),bytes.size(),old,&restored)!=0;
    const bool expected=region && protection==PAGE_READONLY;
    const std::string label="tail admission region "+std::to_string(region)+" mode "+std::to_string(protection);
    check(protectedOk && restoredOk && handled==expected,label+": read-only device allowed, unwritable output declined");
    if(!restoredOk)return false;
    if(!expected)check(!std::memcmp(&c,&before,sizeof(c)) && !std::memcmp(bytes.data(),K::guestVAToHost(address),bytes.size()),
        label+": decline preserves context and mapped bytes");
  }
  prepare(Case::Plain);
  PPCContext c=context(Case::Plain);c.r4.u64=0x1234000000000000ull|uint64_t(buffer+0x2000u-4u);
  c.r5.u64=0x23000001u;c.r6.u64=0xFFFFFFFF82110B00ull;c.r7.u64=0xABCDEF0011223344ull;
  PPCContext oracle=c;__imp__sub_820FDA80(oracle,K::getGuestBase());
  std::array<uint8_t,96> expected;std::memcpy(expected.data(),K::guestVAToHost(buffer+0x2000u),expected.size());
  std::memset(K::guestVAToHost(buffer+0x2000u),0xCC,expected.size());
  sr::native::flush::ReplayKickCallback tail;
  check(tail.emit(c) && c.r3.u64==oracle.r3.u64 &&
      !std::memcmp(expected.data(),K::guestVAToHost(buffer+0x2000u),expected.size()),
      "tail emitter preserves full-width SDK return, CPU-mask bytes and final canary");
  for(bool alias:{false,true})for(uint32_t scratchIndex:{6u,12u}) {
    prepare(Case::Plain);
    const uint32_t aliasDev=alias ? 0x90880000u : dev;
    std::memset(K::guestVAToHost(aliasDev),0,0x5000u);wr(aliasDev+0x2A14u,fb+0x80u);
    const uint32_t output=(alias ? 0xD0880000u : dev)+0x2A14u-4u*scratchIndex;
    PPCContext candidate=context(Case::Plain);candidate.r3.u64=aliasDev;candidate.r4.u64=output-4u;
    candidate.r5.u64=0x04000001u;candidate.r6.u64=0x82110B00u;candidate.r7.u64=0x77889900u;
    const PPCContext before=candidate;
    std::array<uint8_t,96> untouched;std::memcpy(untouched.data(),K::guestVAToHost(output),untouched.size());
    sr::native::flush::ReplayKickCallback overlap;
    check(!overlap.emit(candidate) && !std::memcmp(&candidate,&before,sizeof(candidate)) &&
        !std::memcmp(untouched.data(),K::guestVAToHost(output),untouched.size()),
        "tail scratch overlap "+std::to_string(scratchIndex)+(alias ? " through alias" : " direct")+": decline before mutation");
    PPCContext sdk=before;__imp__sub_820FDA80(sdk,K::getGuestBase());
    const uint32_t originalPc=physical(fb+0x80u)|2u;
    const uint32_t alteredPc=scratchIndex==6u ? 0x13u : 6u;
    check(sdk.r3.u64==before.r4.u64+92u && rd(output+28u)==(scratchIndex==6u ? alteredPc : originalPc) &&
        rd(output+68u)==alteredPc,
        "actual SDK observes ordered scratch reload "+std::to_string(scratchIndex)+(alias ? " through alias" : " direct"));
  }
  prepare(Case::Plain);
  PPCContext wrapped=context(Case::Plain);wrapped.r3.u64=0xFFFFB17Cu;wrapped.r4.u64=buffer+0x2000u-4u;
  wrapped.r5.u64=0x04000001u;wrapped.r6.u64=0x82110B00u;
  const PPCContext wrapBefore=wrapped;
  std::array<uint8_t,96> wrapBytes;std::memcpy(wrapBytes.data(),K::guestVAToHost(buffer+0x2000u),wrapBytes.size());
  sr::native::flush::ReplayKickCallback overflow;
  check(!overflow.emit(wrapped) && !std::memcmp(&wrapped,&wrapBefore,sizeof(wrapped)) &&
      !std::memcmp(wrapBytes.data(),K::guestVAToHost(buffer+0x2000u),wrapBytes.size()),
      "tail admission bounds every later append field before u32 addition");
  return true;
}

struct InterleaveState {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered=false,done=false,timedOut=false,producer=false,drained=false;
  TailToken second{};
  Result oracle;
};
InterleaveState* interleave=nullptr;
void pauseAfterDetachedClose() {
  auto& state=*interleave;
  std::unique_lock<std::mutex> lock(state.mutex);
  state.entered=true;state.changed.notify_all();
  if(!state.changed.wait_for(lock,std::chrono::seconds(5),[&]{return state.done;}))state.timedOut=true;
}
bool tailInterleaving() {
  // Hold producer A after close releases g_mu. Producer B must actually close
  // a shared-list footprint and drain it before A registers its detached tail.
  // The old global-back extraction aborts or steals B's record in this schedule.
  const Result firstOracle=run(Case::Plain,0u);
  InterleaveState state;interleave=&state;
  sr::native::flush::setReplayKickCloseObserver(pauseAfterDetachedClose);
  uint8_t* const guestBase=K::getGuestBase();const size_t guestSize=K::getGuestSize();
  std::thread second([&] {
    K::setGuestMemory(guestBase,guestSize);  // PPC memory macros require this thread's TLS binding.
    {
      std::unique_lock<std::mutex> lock(state.mutex);
      if(!state.changed.wait_for(lock,std::chrono::seconds(5),[&]{return state.entered;})) {
        state.timedOut=true;state.done=true;state.changed.notify_all();return;
      }
    }
    constexpr uint32_t otherDev=0x70700000u,otherBuffer=0x90720000u;
    std::memset(K::guestVAToHost(otherDev),0,0x5000u);
    std::memset(K::guestVAToHost(otherBuffer),0xCC,0x2000u);
    wr(otherDev+0x2A14u,fb+0x80u);wr(otherDev+0x28u,otherBuffer+88u);
    wr(otherDev+0x2Cu,otherBuffer+0x1F00u);wr(otherDev+0x30u,otherBuffer+0x1E00u);
    wr(otherDev+0x3290u,otherBuffer);wr(otherDev+0x34C4u,otherBuffer);
    wr(otherDev+0x32C8u,otherBuffer+0x1000u);wr(otherDev+0x32CCu,otherBuffer+0x1000u);
    wr(otherDev+0x32D0u,otherBuffer+0x1000u);wr(otherDev+0x32D4u,otherBuffer+0x1100u);
    PPCContext c{};c.r3.u64=otherDev;c.r4.u64=otherBuffer-4u;c.r5.u64=0x04000001u;
    c.r6.u64=0x82110B00u;c.r7.u64=0xAABBCCDDu;c.r1.u64=stack+0x30000u;
    __imp__sub_820FDA80(c,K::getGuestBase());
    state.oracle.bytes.resize(0x6000u);
    std::memcpy(state.oracle.bytes.data()+0x2000u,K::guestVAToHost(otherBuffer),92u);
    const uint32_t payload[5]={0x82110B00u,0xAABBCCDDu,4u,0x04000001u,physical(fb+0x80u)|2u};
    sr::native::flush::Entry entry(c,K::getGuestBase(),otherDev);
    state.producer=entry.openAt(otherBuffer-4u) && entry.opaque(sr::native::flush::kRkCallback,payload,5u,92u);
    if(state.producer) {
      entry.close();
      c.r3.u64=otherDev;
      state.drained=sr::native::flush::flushSegmentless(c,K::getGuestBase()) &&
          rd(otherBuffer+0x1000u)==0x82000017u && rd(otherBuffer+0x1004u)==physical(otherBuffer);
      state.second={sr::native::flush::segmentPublishId(physical(otherBuffer),23u),physical(otherBuffer),23u,true};
    } else entry.abandon();
    {
      std::lock_guard<std::mutex> lock(state.mutex);state.done=true;
    }
    state.changed.notify_all();
  });
  const Result firstNative=run(Case::Plain,1u);
  second.join();sr::native::flush::setReplayKickCloseObserver(nullptr);interleave=nullptr;
  const bool forced=state.entered && state.done && !state.timedOut && state.producer && state.drained;
  check(forced,"forced two-producer interleaving closes and drains B while A is paused after detach");
  check(firstNative==firstOracle,"forced interleaving preserves A original SDK fixture CPU result");
  if(!forced || !state.second.id) {
    check(false,"interleaved B has its own native identity and actual SDK effects");return false;
  }
  const TailEffectResult oracle=executeTail(nullptr,state.oracle,true),native=executeTail(&state.second,state.oracle,true);
  check(oracle.complete && native.complete && native.effect.ownedRecord && !oracle.effect.bad && !native.effect.bad &&
      oracle.effect.scratch==native.effect.scratch && oracle.effect.events==native.effect.events && oracle.counts==native.counts &&
      tailTokens.size()==1u && state.second.id!=tailTokens.front().id,
      "interleaved B has its own native identity and actual SDK effects");
  // The existing pinned-segment contract registers two passes; complete B's
  // second pass rather than leaving an unrelated unfinished fixture entry.
  const bool secondPublish=sr::native::flush::segmentPublishId(state.second.phys,23u)==state.second.id;
  const TailEffectResult repeated=executeTail(&state.second,state.oracle,true);
  check(secondPublish && repeated.complete && repeated.effect.ownedRecord && repeated.effect.scratch==oracle.effect.scratch &&
      repeated.effect.events==oracle.effect.events && repeated.counts==oracle.counts,
      "interleaved B completes both existing pinned-record passes");
  return true;
}
bool oracleValid(Case kind,const Result& r) {
  const bool failure=kind==Case::PreFailed || kind==Case::CoherentFailed || kind==Case::KickFailed || kind==Case::AppendFailed;
  if(r.regs[0]!=(failure ? 1u : 0u) || word(r.device,0x3290u) || word(r.device,0x3294u))return false;
  if(failure) {
    const uint32_t fields[]={0x28u,0x2Cu,0x34C4u,0x34C0u,0x34B8u,0x34BCu};
    for(unsigned i=0;i<6u;++i)if(word(r.device,fields[i])!=word(r.device,0x32F0u+i*4u))return false;
    return !(r.device[0x2A39u]&0x40u) && word(r.device,0x30u)==buffer+0x4800u-160u;
  }
  for(uint32_t off:{0x32C8u,0x32A0u,0x32B4u})if(word(r.device,off+12u))return false;
  for(unsigned i=0;i<3u;++i)if(word(r.bytes,0x110u+i*0x40u)!=0xC0000000u)return false;
  const bool direct=kind==Case::Direct || kind==Case::DirectDeferred;
  const bool skip=kind==Case::NoGpu || kind==Case::NoGpuDeferred;
  if(word(r.device,0x2A74u)!=(direct ? 1u : skip ? 3u : 4u) || r.frame[1]!=coherent/4u)return false;
  if(r.events.size()!=(coherent ? 3u : 2u) || r.events[r.events.size()-2u][0]!=400u || r.events.back()[0]!=128u)return false;
  const uint32_t root=kind==Case::ZeroRoot ? 0u : physical(buffer+0x100u)-0x40000000u;
  return word(r.bytes,0x2004u)==0x82110B00u && word(r.bytes,0x2008u)==root;
}
bool denied() {
  for(unsigned region=0;region<4u;++region)for(DWORD protection:{PAGE_READONLY,PAGE_NOACCESS,PAGE_GUARD|PAGE_READWRITE}) {
    prepare(Case::Plain);PPCContext c=context(Case::Plain);const auto before=c;
    const uint32_t address=region==0u ? dev : region==1u ? stack-0x1000u : region==2u ? buffer : buffer+0x4000u;
    std::vector<uint8_t> original(K::guestVAToHost(address),K::guestVAToHost(address)+4096u);
    DWORD old=0,restore=0;const bool protectedOk=VirtualProtect(K::guestVAToHost(address),4096u,protection,&old)!=0;
    const bool handled=protectedOk && N::endRecordingKick(c,K::getGuestBase());
    const bool restored=protectedOk && VirtualProtect(K::guestVAToHost(address),4096u,old,&restore)!=0;
    const std::string label="protected region "+std::to_string(region)+" mode "+std::to_string(protection);
    check(protectedOk && restored && !handled,label+": declined before mutation");
    // A failed restoration leaves this fixture unsafe to inspect or reuse.
    if (!restored) return false;
    check(std::memcmp(&c,&before,sizeof(c))==0 && allocations.empty(),label+": no context or helper effects");
    check(restored && std::memcmp(original.data(),K::guestVAToHost(address),4096u)==0,label+": bytes preserved");
  }
  return true;
}
}
int runNativeRecordingTest(const char* mode) {
  std::setvbuf(stdout,nullptr,_IONBF,0);enabled=std::strcmp(mode,"gate-off")!=0;tailMode=std::strcmp(mode,"tail")==0;
  K::registerStubs();sr::guest::initializeDispatchTable();K::kpcrBindCurrent(stack,stack-0x10000u,3);
  check(N::kickGateOn()==enabled,"requested recording kick gate");
  check(PPC_LOOKUP_FUNC(K::getGuestBase(),0x82110C78u)==sub_82110C78,"production dispatch resolves recording kick override");
  if(tailMode) {
    check(sr::native::flush::replayKickCallbackOn() && sr::native::flush::on() && sr::native::flush::segmentlessOn(),
        "requested native owned-tail gates");
    sr::native::flush::setReplayKickPublicationObserver(observeTail);
  }
  const size_t index=(allocatorCallback-PPC_CODE_BASE)>>2;PPCFunc* saved=K::g_dispatchTable[index];K::g_dispatchTable[index]=allocate;
  for(unsigned i=0;i<names.size();++i) {
    const Case kind=Case(i);const Result oracle=run(kind,0u);
    const bool exercised=oracleValid(kind,oracle);
    if(!exercised) {
      std::printf("[RECORDING_FIXTURE] %s return=%llu flags=%02X events=%zu fn=%08X arg=%08X count=%u limits=%X/%X/%X requests=",
          names[i],static_cast<unsigned long long>(oracle.regs[0]),oracle.device[0x2A39u],oracle.events.size(),
          word(oracle.bytes,0x2004u),word(oracle.bytes,0x2008u),oracle.frame[1],
          word(oracle.device,0x32D4u),word(oracle.device,0x32ACu),word(oracle.device,0x32C0u));
      for(const auto& e:oracle.events)std::printf("%llu,",static_cast<unsigned long long>(e[0]));
      std::printf("\n");
    }
    check(exercised,std::string(names[i])+": SDK exercises requested fixture");
    check(run(kind,1u)==oracle,std::string(names[i])+": dispatch equals actual SDK");
    check(run(kind,2u)==oracle,std::string(names[i])+": direct equals actual SDK");
  }
  bool safe=denied();
  if(tailMode)safe=tailAdmission() && tailInterleaving() && safe;
  K::g_dispatchTable[index]=saved;
  if(tailMode)sr::native::flush::setReplayKickPublicationObserver(nullptr);
  std::printf("[NATIVE_RECORDING_TEST] %d passed, %d failed\n",passed,failed);return safe && !failed ? 0 : 1;
}
