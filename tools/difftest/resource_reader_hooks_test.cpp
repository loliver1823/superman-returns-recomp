// Real production native memcpy and exact hook symbols. Early CLI: owns only
// a sparse guest reservation; no renderer, queue, game or GPU readback.
#include "guest/ppc/ppc_recomp_shared.h"
#include "kernel/guest_context.hpp"
#include "kernel/resource_reader_observe.hpp"
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
extern void SrReaderCopy_823A1194(PPCRegister&,PPCRegister&,PPCRegister&,PPCRegister&,PPCRegister&,PPCRegister&);
extern void SrReaderReturn_823A1198(PPCRegister&);
extern void SrReaderLock_823A1120(PPCRegister&,PPCRegister&);
extern void sub_8279EC60(PPCContext&,uint8_t*);

int resource_reader_hooks_test_main(int argc,char** argv) {
  const bool expectNative=argc<3 || std::strcmp(argv[2],"delegated");
  auto require=[](bool ok,const char* why) { if(!ok) throw std::runtime_error(why); };
  constexpr size_t size=0x100010000ull;
  uint8_t* base=nullptr;
  try {
    require(_putenv_s("SR_GUEST_READER_PROBE","1")==0,"probe gate");
    require(_putenv_s("SR_NATIVE_MEMCPY",expectNative?"1":"0")==0,"memcpy gate");
    require(_putenv_s("SR_NATIVE_MEMCPY_CHECK","0")==0,"check gate");
    base=static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(0x2B00000000ull),size,MEM_RESERVE,PAGE_READWRITE));
    require(base!=nullptr,"reserve sparse guest arena");
    require(VirtualAlloc(base+0x10000,0x10000,MEM_COMMIT,PAGE_READWRITE)!=nullptr,"commit fixture bytes");
    sr::kernel::setGuestMemory(base,size);
    for(unsigned i=0;i<64;++i) base[0x10000+i]=uint8_t(i^0x5a);
    std::memset(base+0x12000,0xcc,64);
    PPCContext ctx{}; ctx.r1.u32=0x18000; ctx.r3.u32=0x12000; ctx.r4.u32=0x10000; ctx.r5.u32=64;
    ctx.r27.u32=0x19000;
    const uint32_t lockPair[2]={__builtin_bswap32(64u),__builtin_bswap32(0x10000u)};
    std::memcpy(base+ctx.r1.u32+0x50,lockPair,sizeof(lockPair));
    std::array<uint8_t,sizeof(PPCContext)> unchanged{};
    std::memcpy(unchanged.data(),&ctx,sizeof(ctx));
    SrReaderLock_823A1120(ctx.r1,ctx.r27);
    SrReaderCopy_823A1194(ctx.r1,ctx.r3,ctx.r4,ctx.r5,ctx.r27,ctx.r31);
    require(std::memcmp(&ctx,unchanged.data(),sizeof(ctx))==0,"lock and pre-copy hooks preserve all registers");
    sub_8279EC60(ctx,base); // actual existing production override; no fake memcpy
    std::memcpy(unchanged.data(),&ctx,sizeof(ctx));
    SrReaderReturn_823A1198(ctx.r1);
    require(std::memcmp(&ctx,unchanged.data(),sizeof(ctx))==0,"return hook preserves all registers");
    require(std::memcmp(base+0x12000,base+0x10000,64)==0,"production copied bytes");
    auto counts=sr::kernel::reader_observe::snapshotForTest();
    require(counts.nativeReadReturned==unsigned(expectNative),"only the executed native path claims read");
    require(counts.copyUnknown==unsigned(!expectNative),"delegated return stays unknown");
    // A real normal return hook with no intervening copy must remain UNKNOWN.
    std::memset(base+0x12000,0xcc,64);
    ctx.r3.u32=0x12000; ctx.r4.u32=0x10000; ctx.r5.u32=64;
    SrReaderCopy_823A1194(ctx.r1,ctx.r3,ctx.r4,ctx.r5,ctx.r27,ctx.r31);
    SrReaderReturn_823A1198(ctx.r1);
    require(std::memcmp(base+0x12000,base+0x10000,64)!=0,"omitted-copy control performed no read/copy");
    counts=sr::kernel::reader_observe::snapshotForTest();
    require(counts.nativeReadReturned==unsigned(expectNative) && counts.copyUnknown==unsigned(!expectNative)+1,
            "normal return cannot fabricate omitted consumption");
    // Execute a different tuple: even real native copy activity cannot rescue
    // the armed 64-byte row when only 32 bytes were actually copied.
    ctx.r3.u32=0x12000; ctx.r4.u32=0x10000; ctx.r5.u32=64;
    SrReaderCopy_823A1194(ctx.r1,ctx.r3,ctx.r4,ctx.r5,ctx.r27,ctx.r31);
    ctx.r5.u32=32; sub_8279EC60(ctx,base); SrReaderReturn_823A1198(ctx.r1);
    require(std::memcmp(base+0x12000,base+0x10000,32)==0 && base[0x12000+32]==0xcc,"actual different tuple copied only 32 bytes");
    counts=sr::kernel::reader_observe::snapshotForTest();
    require(counts.nativeReadReturned==unsigned(expectNative) && counts.copyUnknown==unsigned(!expectNative)+2,"wrong tuple stays unknown");
    require(counts.pending==0 && counts.copyAttempted==3 && counts.copyReturned==3,"hook conservation");
    // resproof: the lock core's post-body grading - a virtual address is counted only, a physical one graded per page
    const uint64_t overlaps0=counts.overlaps;
    sr::kernel::reader_observe::lockGraded(0x4009E530u,10u,0x40010000u,8192u,0u);
    counts=sr::kernel::reader_observe::snapshotForTest();
    require(counts.lockVirtual==1 && counts.lockGraded==0 && counts.overlaps==overlaps0,"a virtual lock is counted, not graded");
    sr::kernel::reader_observe::lockGraded(0x4009E530u,10u,0xA0010800u,8192u,0x10u);
    counts=sr::kernel::reader_observe::snapshotForTest();
    require(counts.lockGraded==1 && counts.overlaps==overlaps0+3,"a read-only physical lock is graded page by page (3 pages)");
    sr::kernel::reader_observe::report("fixture");
    std::printf("[RESOURCE_READER_HOOK_TEST] native=%u passed=1\n",unsigned(expectNative));
    // Fresh early-test process, no guest threads, no backend ownership.
    sr::kernel::setGuestMemory(nullptr,0); VirtualFree(base,0,MEM_RELEASE); return 0;
  } catch(const std::exception& e) {
    std::fprintf(stderr,"[RESOURCE_READER_HOOK_TEST] FAILED %s\n",e.what());
    if(base) { sr::kernel::setGuestMemory(nullptr,0); VirtualFree(base,0,MEM_RELEASE); }
    return 1;
  }
}
