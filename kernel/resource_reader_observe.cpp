#include "resource_reader_observe.hpp"
#if SR_ORACLE
#include "guest/ppc/ppc_recomp_shared.h"
#else
// Empty reference-only ABI needs no generated headers or their library code.
union PPCRegister;
#endif

// The shared generated guest ABI calls these symbols in both compositions.
// Release definitions below are empty; no observer data or dependencies survive.
#if SR_ORACLE
#include "../gfx/resource_reader_probe.hpp"
#include "../gfx/resource_memory.hpp"
#include "../app/log.hpp"
#include "guest_context.hpp"
#include "guest_protect.hpp"
#include "phys_window.hpp"
#include "quit.hpp"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sr::kernel::reader_observe {
namespace {
namespace rp = sr::gfx::reader_probe;
namespace mr = sr::gfx::memres;
std::atomic<uint64_t> failures{0};
uint64_t now() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count());
}
rp::State* probe() noexcept {
  static rp::State* const instance=[]() noexcept -> rp::State* {
    const char* e=std::getenv("SR_GUEST_READER_PROBE");
    if(!e || std::strcmp(e,"1")) return nullptr;
    try {
      // Register before allocation: allocation failure still has a terminal witness.
      quit::addFamily("guest_reader_probe",report);
      auto* state=new rp::State;
      SR_LOG_BOOT(sr::LogSeverity::Info,
        "[MEM_RES_CHECK] reader_enabled=1 storage_bound=%llu recordings_cap=%u pages=%u pending_cap=%u read_pages_cap=0 detail_pages=%u event_cap=%u exhaustive_readers=0 atomic_bytes=0",
        (unsigned long long)rp::State::storageBound(),rp::kRecordings,rp::kPages,rp::kCopies,rp::kDetailPages,rp::kEvents);
      return state;
    } catch(...) { failures.fetch_add(1); return nullptr; }
  }();
  return instance;
}
rp::Witness witness(uint32_t page) {
  rp::Witness w;
  if(page>=rp::kPages || !mr::resourceMode()) return w;
  const uint32_t phys=page*4096;
  const auto a=physwin::ownerVersion(phys,4096);
  const uint64_t generation=physwin::registryGeneration();
  const auto owners=physwin::ownersOfSpan(phys,4096);
  auto& memory=mr::resourceMemory();
  const uint32_t cpu=memory.cpuGeneration(page),gpu=memory.gpuGeneration(page);
  const int liveWindow=physwin::gpuWindowRange(phys,4096);
  const uint32_t window=memory.sourceWindow(page,liveWindow);
  const auto b=physwin::ownerVersion(phys,4096);
  w={a.sum,owners.setDigest,generation,a.cbSeq,owners.nLive,owners.serial[0],owners.serial[1],cpu,gpu,window,
     a.ok && b.ok && a.sum==b.sum && a.cbSeq==b.cbSeq && generation==owners.gen &&
     generation==physwin::registryGeneration() && owners.nLive && !owners.aliasedPairs &&
     (liveWindow==8 || liveWindow==0xA) &&
     cpu==memory.cpuGeneration(page) && gpu==memory.gpuGeneration(page) &&
     window==memory.sourceWindow(page,physwin::gpuWindowRange(phys,4096))};
  return w;
}
bool readPair(uint64_t va, uint32_t& pitch, uint32_t& bits) {
  pitch=bits=0;
  if(va<0x10000 || va+8>0x100000000ull) return false;
  guest_protect::ArenaLease lease;
  auto* base=getGuestBase();
  if(!lease || !base) return false;
  const uint32_t first=aliasGuestVA(uint32_t(va)),last=aliasGuestVA(uint32_t(va+7));
  if(uint64_t(first)+8>getGuestSize() || last<first || last-first!=7) return false;
  uint32_t a[2]{},b[2]{}; SIZE_T count=0;
  if(!ReadProcessMemory(GetCurrentProcess(),base+first,a,sizeof(a),&count) || count!=sizeof(a)) return false;
  if(!ReadProcessMemory(GetCurrentProcess(),base+first,b,sizeof(b),&count) || count!=sizeof(b) || std::memcmp(a,b,sizeof(a))) return false;
  pitch=__builtin_bswap32(a[0]); bits=__builtin_bswap32(a[1]);
  return true;
}
// One log line of the probe: false once the event cap is reached, and the first refused line logs (once, past the cap)
// that the census of this process is incomplete - never a silent truncation.
bool event(rp::State& p) {
  if(p.event()) return true;
  if(p.takeCapNotice()) SR_LOG_BOOT(sr::LogSeverity::Info,
    "[MEM_RES_CHECK] reader_events_capped event_cap=%u the census of this process is INCOMPLETE",rp::kEvents);
  return false;
}
void range(rp::State& p,uint64_t eventId,const char* phase,uint32_t phys,uint32_t bytes,
           uint32_t detailPages=rp::kDetailPages) {
  if(!bytes || uint64_t(phys)+bytes>0x20000000ull) {
    p.refuseRange();
    // No truncation into an apparently complete smaller read.
    if(event(p)) SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_CHECK] reader_range id=%llu phase=%s phys=%08X bytes=%u status=unknown_range",
      (unsigned long long)eventId,phase,phys,bytes);
    return;
  }
  // Every page is graded (no page cap); a range over detailPages lists only its GPU-overlap pages (walkRange).
  const auto tally=rp::walkRange(p,phys,bytes,
    [](uint32_t page) {
      auto a=witness(page),b=witness(page);
      if(!rp::sameObservation(a,b)) a.known=false;
      return a;
    },
    [&](uint32_t at,uint32_t count,const rp::Overlap& result,const rp::Witness& a) {
      if(event(p)) SR_LOG_BOOT(sr::LogSeverity::Info,
        "[MEM_RES_CHECK] reader_range id=%llu phase=%s phys=%08X bytes=%u status=%s recording=%llu stream=%u ticket=%llu completed=%llu sampled_ns=%llu observed_ns=%llu write_begin=%08X write_end=%08X write_gen=%u owner_sum=%llu grants=%u grant_digest=%llu serial0=%u serial1=%u grant_gen=%llu cpu=%u gpu=%u window=%X write_cpu=%u write_gpu=%u write_window=%X",
        (unsigned long long)eventId,phase,at,count,rp::name(result.status),
        (unsigned long long)result.recording,result.stream,(unsigned long long)result.ticket,
        (unsigned long long)result.completed,(unsigned long long)result.sampledNs,(unsigned long long)now(),
        result.begin,result.end,result.generation,(unsigned long long)a.ownerSum,a.grants,
        (unsigned long long)a.grantDigest,a.serial0,a.serial1,(unsigned long long)a.grantGeneration,
        a.cpu,a.gpu,a.window,result.written.cpu,result.written.gpu,result.written.window);
    },detailPages);
  if(tally.listed==tally.pages) return;
  // The unlisted pages of a long range, by status (u.<status>=pages; the listed ones have their own lines).
  char counts[640]; size_t used=0; counts[0]=0;
  for(uint32_t s=0;s<rp::kStatusCount;++s) {
    if(!tally.unlisted[s] || used>=sizeof(counts)) continue;
    const int n=std::snprintf(counts+used,sizeof(counts)-used," u.%s=%llu",rp::name(rp::Status(s)),
                              (unsigned long long)tally.unlisted[s]);
    if(n>0) used+=size_t(n);
  }
  if(event(p)) SR_LOG_BOOT(sr::LogSeverity::Info,
    "[MEM_RES_CHECK] reader_range_bulk id=%llu phase=%s phys=%08X bytes=%u pages=%llu listed=%llu%s",
    (unsigned long long)eventId,phase,phys,bytes,(unsigned long long)tally.pages,(unsigned long long)tally.listed,counts);
}
void sourceRange(rp::State& p,uint64_t id,const char* phase,uint32_t source,uint32_t bytes) {
  if(source<0x80000000u || !bytes || uint64_t(source)+bytes>0x100000000ull) {
    p.refuseRange();
    if(event(p)) SR_LOG_BOOT(sr::LogSeverity::Info,"[MEM_RES_CHECK] reader_range id=%llu phase=%s status=unknown_va_mapping",
      (unsigned long long)id,phase);
    return;
  }
  const uint32_t first=mr::resourcePhysOfVa(source),last=mr::resourcePhysOfVa(source+bytes-1);
  if(last<first || uint64_t(last)-first+1!=bytes) {
    p.refuseRange();
    if(event(p)) SR_LOG_BOOT(sr::LogSeverity::Info,"[MEM_RES_CHECK] reader_range id=%llu phase=%s status=noncontiguous_physical_mapping",
      (unsigned long long)id,phase);
    return;
  }
  range(p,id,phase,first,bytes);
}
}
bool enabled() noexcept { return probe()!=nullptr; }
uint32_t stream() noexcept { try { if(auto* p=probe()) return p->stream(); } catch(...) { failures.fetch_add(1); } return 0; }
uint64_t recording(uint32_t streamId,std::weak_ptr<const void> identity,uint32_t ownerAllocationBytes) noexcept {
  try { if(auto* p=probe()) return p->recording(streamId,std::move(identity),ownerAllocationBytes); } catch(...) { failures.fetch_add(1); } return 0;
}
void submitted(uint64_t id,const std::weak_ptr<const void>& identity,uint64_t ticket) noexcept {
  try { if(auto* p=probe()) p->submitted(id,identity,ticket); } catch(...) { failures.fetch_add(1); }
}
void completion(uint32_t id,uint64_t completed,bool valid) noexcept {
  try { if(auto* p=probe()) p->completion(id,completed,now(),valid); } catch(...) { failures.fetch_add(1); }
}
void gpuWrite(uint64_t id,uint32_t phys,uint32_t bytes,uint32_t generation) noexcept {
  try { if(auto* p=probe()) {
    p->noteGpuWriteSpan();
    if(!bytes || uint64_t(phys)+bytes>0x20000000ull) { p->write(id,phys,bytes,generation,{}); return; }
    for(uint64_t at=phys,end=uint64_t(phys)+bytes;at<end;) {
      uint32_t count=uint32_t(std::min(end,(at|4095ull)+1)-at);
      auto a=witness(uint32_t(at>>12)),b=witness(uint32_t(at>>12));
      if(!rp::sameObservation(a,b)) a.known=false;
      p->write(id,uint32_t(at),count,generation,a); at+=count;
    }
  }} catch(...) { failures.fetch_add(1); }
}
void nativeReadExecuted(uint32_t source,uint32_t destination,uint32_t bytes) noexcept {
  try { if(auto* p=probe()) p->nativeRead(GetCurrentThreadId(),source,destination,bytes); } catch(...) { failures.fetch_add(1); }
}
void textureLock(uint32_t texture,uint32_t level,uint32_t output,uint32_t flags) noexcept {
  try { if(auto* p=probe()) {
    uint32_t pitch=0,bits=0; bool known=readPair(output,pitch,bits);
    // General lock is potential access only, and has no exact reader cohort.
    const uint64_t id=p->lock(GetCurrentThreadId(),0,texture,known);
    if(event(*p)) SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_CHECK] reader_lock id=%llu kind=texture resource=%08X level=%u flags=%08X pitch=%u bits=%08X output=%08X known=%u actual_read=0",
      (unsigned long long)id,texture,level,flags,pitch,bits,output,unsigned(known));
  }} catch(...) { failures.fetch_add(1); }
}
void lockIntent(uint32_t resource,uint32_t reason,uint32_t address,uint32_t bytes,uint32_t flags) noexcept {
  try { if(auto* p=probe()) {
    p->noteLockIntent();
    uint32_t fence8=0,fence12=0; const bool fenceKnown=readPair(uint64_t(resource)+8,fence8,fence12);
    if(event(*p)) SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_CHECK] reader_lock_intent resource=%08X reason=%u address=%08X bytes=%u flags=%08X fence8=%08X fence12=%08X fence_known=%u actual_read=0",
      resource,reason,address,bytes,flags,fence8,fence12,unsigned(fenceKnown));
  }} catch(...) { failures.fetch_add(1); }
}
// The lock core 820F3988 after its body (resproof): the guest holds the CPU pointer now and the resource's fence wait
// is behind it, so every D3D lock - vertex, index and texture buffers, READ-ONLY locks included (flags 0x10 / 0x02 emit no
// coherency packet at all: no 0x0?000000 site can see them) - is graded over [address, +bytes) against the recorded GPU
// writes (phase after_lock_core, detail 0). A virtual address (below 0x80000000) is no GPU memory: counted, not graded.
void lockGraded(uint32_t resource,uint32_t reason,uint32_t address,uint32_t bytes,uint32_t flags) noexcept {
  (void)resource; (void)reason; (void)flags;
  try { if(auto* p=probe()) {
    const uint64_t id=p->noteLockGraded(address>=0x80000000u);
    if(address<0x80000000u || !bytes) return;
    if(uint64_t(address)+bytes>0x100000000ull) {
      p->refuseRange();
      if(event(*p)) SR_LOG_BOOT(sr::LogSeverity::Info,
        "[MEM_RES_CHECK] reader_range id=%llu phase=after_lock_core phys=%08X bytes=%u status=unknown_range",
        (unsigned long long)id,address,bytes);
      return;
    }
    // the physical span page by page (a lock never crosses a window; resourcePhysOfVa is per byte, so split at pages)
    for(uint64_t at=address,end=uint64_t(address)+bytes;at<end;) {
      const uint64_t next=std::min(end,(at|0xFFFFFull)+1);   // within one 1 MB section the mapping is linear
      range(*p,id,"after_lock_core",mr::resourcePhysOfVa(uint32_t(at)),uint32_t(next-at),0);
      at=next;
    }
  }} catch(...) { failures.fetch_add(1); }
}
void preCoherency(uint32_t status,uint32_t phys,uint32_t bytes) noexcept {
  // 0x02000000: a texture lock's coherency (Codex's site: a line per lock, a line per page up to rp::kDetailPages).
  // Whole-run census (resproof): every other coherency the CP notes as a CPU write is graded too - 0x03000000 is
  // D3DVertexBuffer_Lock's status and GpuInvalidateMemoryRange's (a lock or a CPU write announced), anything else
  // "other" - except the command buffer's 0x01000000 (CPU-written PM4, never resolve output). Those log no
  // reader_coherency line and fold their CPU-owned pages into one reader_range_bulk line (detail 0): only a page of
  // GPU output (or a gap) gets a line of its own.
  const bool texlock=status==0x02000000u;
  const char* phase=texlock?"before_texlock_cpu_note":status==0x03000000u?"before_inval_cpu_note":
                    (status==0x00010000u || status==0x03000100u || status==0x01000000u)?nullptr:"before_other_cpu_note";
  if(!phase) return;
  try { if(auto* p=probe()) {
    if(!texlock) { range(*p,p->noteOtherCoherency(),phase,phys,bytes,0); return; }
    const auto id=p->noteCoherency();
    if(event(*p)) SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_CHECK] reader_coherency id=%llu thread=%u status=%08X phys=%08X bytes=%u before_cpu_note=1 observed_ns=%llu",
      (unsigned long long)id,GetCurrentThreadId(),status,phys,bytes,(unsigned long long)now());
    range(*p,id,phase,phys,bytes);
  }} catch(...) { failures.fetch_add(1); }
}
void sourceLock(uint32_t frame,uint32_t resource) noexcept {
  try { if(auto* p=probe()) {
    uint32_t pitch=0,bits=0; bool known=readPair(uint64_t(frame)+0x50,pitch,bits);
    const uint64_t id=p->lock(GetCurrentThreadId(),frame,resource,known);
    if(event(*p)) SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_CHECK] reader_lock id=%llu kind=surface pc=823A1120 thread=%u frame=%08X resource=%08X flags=0 pitch=%u bits=%08X known=%u actual_read=0",
      (unsigned long long)id,GetCurrentThreadId(),frame,resource,pitch,bits,unsigned(known));
  }} catch(...) { failures.fetch_add(1); }
}
void copyBegin(uint32_t frame,uint32_t destination,uint32_t source,uint32_t bytes,uint32_t resource,uint32_t row) noexcept {
  try { if(auto* p=probe()) {
    const auto c=p->attempt(GetCurrentThreadId(),frame,resource,row,source,destination,bytes);
    if(event(*p)) SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_CHECK] reader_copy_attempt id=%llu lock=%llu pc=823A1194 thread=%u frame=%08X resource=%08X row=%u src=%08X dst=%08X bytes=%u range_valid=%u actual_read=0",
      (unsigned long long)c.id,(unsigned long long)c.lockId,c.thread,frame,resource,row,source,destination,bytes,unsigned(c.valid));
    if(c.id && c.valid) sourceRange(*p,c.id,"before_copy",source,bytes);
  }} catch(...) { failures.fetch_add(1); }
}
void copyEnd(uint32_t frame) noexcept {
  try { if(auto* p=probe()) {
    const auto c=p->returned(GetCurrentThreadId(),frame);
    if(event(*p)) SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_CHECK] reader_copy_return id=%llu pc=823A1198 thread=%u frame=%08X native_read=%u actual_read=%u delegated_or_unknown=%u",
      (unsigned long long)c.id,GetCurrentThreadId(),frame,unsigned(c.nativeRead),unsigned(c.id&&c.nativeRead&&c.valid),unsigned(!c.id||!c.nativeRead||!c.valid));
    if(c.id && c.valid) sourceRange(*p,c.id,"after_copy",c.source,c.bytes);
  }} catch(...) { failures.fetch_add(1); }
}
void report(const char* why) noexcept {
  try {
    auto* p=probe(); const auto c=p?p->counts():rp::Counts{};
    SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_CHECK] reader_summary phase=%s enabled=%u locks=%llu lock_unknown=%llu attempts=%llu returned=%llu native_read_returned=%llu copy_unknown=%llu pending=%llu abandoned=%llu refused=%llu unmatched_returns=%llu range_refused=%llu recordings=%llu recording_reused=%llu weak_bytes=%llu weak_peak_bytes=%llu submitted=%llu write_page_calls=%llu write_pages=%llu overlaps=%llu completed_overlaps=%llu overlap_unknown=%llu coherency=%llu events=%llu events_capped=%llu capacity=%llu invalid=%llu stream_errors=%llu counter_overflow=%llu failures=%llu lock_intents=%llu gpu_write_spans=%llu other_coherency=%llu lock_graded=%llu lock_virtual=%llu exhaustive_readers=0 atomic_bytes=0",
      why,unsigned(p!=nullptr),(unsigned long long)c.locks,(unsigned long long)c.lockUnknown,
      (unsigned long long)c.copyAttempted,(unsigned long long)c.copyReturned,(unsigned long long)c.nativeReadReturned,
      (unsigned long long)c.copyUnknown,(unsigned long long)c.pending,(unsigned long long)c.copyAbandoned,
      (unsigned long long)c.copyRefused,(unsigned long long)c.unmatchedReturns,(unsigned long long)c.rangeRefused,
      (unsigned long long)c.recordings,
      (unsigned long long)c.recordingReused,(unsigned long long)c.weakBytes,(unsigned long long)c.weakPeakBytes,
      (unsigned long long)c.submitted,(unsigned long long)c.writes,(unsigned long long)c.writePages,
      (unsigned long long)c.overlaps,(unsigned long long)c.completedOverlaps,(unsigned long long)c.overlapUnknown,
      (unsigned long long)c.coherency,(unsigned long long)c.events,(unsigned long long)c.eventsCapped,
      (unsigned long long)c.capacity,(unsigned long long)c.invalid,(unsigned long long)c.streamErrors,
      (unsigned long long)c.counterOverflow,(unsigned long long)failures.load(),
      (unsigned long long)c.lockIntents,(unsigned long long)c.gpuWriteSpans,
      (unsigned long long)c.otherCoherency,(unsigned long long)c.lockGraded,(unsigned long long)c.lockVirtual);
  } catch(...) { failures.fetch_add(1); }
}
#if defined(SR_DIFFTEST)
rp::Counts snapshotForTest() { auto* p=probe(); return p?p->counts():rp::Counts{}; }
#endif
} // namespace sr::kernel::reader_observe
#endif

void SrReaderLock_823A1120(PPCRegister& r1,PPCRegister& r27) {
#if SR_ORACLE
  sr::kernel::reader_observe::sourceLock(r1.u32,r27.u32);
#endif
}
void SrReaderCopy_823A1194(PPCRegister& r1,PPCRegister& r3,PPCRegister& r4,PPCRegister& r5,PPCRegister& r27,PPCRegister& r31) {
#if SR_ORACLE
  sr::kernel::reader_observe::copyBegin(r1.u32,r3.u32,r4.u32,r5.u32,r27.u32,r31.u32);
#endif
}
void SrReaderReturn_823A1198(PPCRegister& r1) {
#if SR_ORACLE
  sr::kernel::reader_observe::copyEnd(r1.u32);
#endif
}
