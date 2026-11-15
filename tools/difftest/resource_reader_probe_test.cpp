// Pure metadata controls. This is not the production-hook or live-reader proof.
#include "gfx/resource_reader_probe.hpp"
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace sr::gfx::reader_probe;
namespace {
unsigned checks=0;
void require(bool ok,const char* what) { if(!ok) throw std::runtime_error(what); ++checks; }
}
int main() {
  try {
    auto p=std::make_unique<State>();
    auto owner=std::make_shared<int>(1),other=std::make_shared<int>(2);
    const auto stream=p->stream();
    const auto recording=p->recording(stream,owner,64);
    Witness w{4,8,12,16,1,20,0,2,3,8,true};
    p->write(recording,0x4000,4096,3,w);
    require(p->overlap(0x4010,16,w).status==Status::Unsubmitted,"recorded is not submitted");
    p->submitted(recording,owner,9);
    require(p->overlap(0x4010,16,w).status==Status::UnknownCompletion,"submitted is not completed");
    p->completion(stream,8,100,true);
    require(p->overlap(0x4010,16,w).status==Status::CompletionNotObserved,"old completion sample is unknown");
    p->completion(stream,9,200,true);
    require(p->overlap(0x4010,16,w).status==Status::CompletedOverlap,"actual sampled completion joins");
    Witness global=w; global.grantGeneration++; global.cbSequence++; global.cpu++;
    require(p->overlap(0x4010,16,global).status==Status::CompletedOverlap,"unrelated global versions do not erase prior write");
    require(!sameObservation(w,global),"admission bracket still requires all versions");
    Witness reusedOwner=w; reusedOwner.grantDigest++;
    require(p->overlap(0x4010,16,reusedOwner).status==Status::ChangedOwner,"reused allocation is unknown");
    p->write(recording,0x4040,4,3,w);
    require(p->overlap(0x4010,16,w).status==Status::PartialWrite,"partial latest cannot rescue older full span");
    require(p->overlap(0x4040,4,w).status==Status::CompletedOverlap,"exact partial intersection");
    require(p->overlap(0x4fff,2,w).status==Status::InvalidRange,"cross-page input refused");
    const auto wrong=p->recording(stream,owner,64); p->write(wrong,0x5000,4096,3,w); p->submitted(wrong,other,10);
    require(p->overlap(0x5000,4,w).status==Status::WrongRecording,"reused list with different payload refused");
    auto transient=std::make_shared<int>(3); const auto expired=p->recording(stream,transient,64);
    p->write(expired,0x6000,4096,3,w); transient.reset();
    require(p->overlap(0x6000,4,w).status==Status::ExpiredUnsubmitted,"weak expiry is not completion");
    p->completion(stream,UINT64_MAX,300,true);
    require(p->overlap(0x4040,4,w).status==Status::UnknownCompletion,"device removed sentinel refused");

    const auto lock=p->lock(1,0x80080000,0x81000000,true);
    auto c=p->attempt(1,0x80080000,0x81000000,0,0x80004000,0x80008000,16);
    require(c.lockId==lock,"copy joins exact source lock cohort");
    c=p->returned(1,0x80080000);
    require(!c.nativeRead,"normal delegated return alone cannot claim consumption");
    p->attempt(1,0x80080000,0x81000000,1,0x80004010,0x80008010,16);
    p->nativeRead(1,0x80004010,0x80008010,15);
    require(!p->returned(1,0x80080000).nativeRead,"wrong byte count cannot mark consumption");
    p->attempt(1,0x80080000,0x81000000,2,0x80004020,0x80008020,16);
    p->nativeRead(1,0x80004020,0x80008020,16);
    require(p->returned(1,0x80080000).nativeRead,"exact executed native read joins return");
    p->attempt(1,0x80080000,0x81000000,3,0xfffffff0,0x80008020,32);
    p->nativeRead(1,0xfffffff0,0x80008020,32);
    require(!p->returned(1,0x80080000).nativeRead,"wrapped source cannot claim consumption");
    p->attempt(1,0x80080000,0x81000000,4,0x80004020,0x80008020,16);
    p->attempt(1,0x80080000,0x81000000,5,0x80004020,0x80008020,16);
    p->returned(1,0x80080000); p->returned(1,0x80080000);
    auto n=p->counts();
    require(n.copyAbandoned==1 && n.unmatchedReturns==1,"interrupted and duplicate calls explicit");
    require(n.copyAttempted==n.copyReturned+n.pending+n.copyAbandoned+n.copyRefused,"attempt conservation");
    require(n.copyReturned==n.nativeReadReturned+n.copyUnknown,"return conservation");
    for(uint32_t i=0;i<kCopies+1;++i) p->attempt(i+10,i+10,0,0,0x80004000,0x80008000,4);
    n=p->counts(); require(n.pending==kCopies && n.copyRefused==1,"pending bound retains unknown");
    // Whole runs (resproof): the cap is sized far past the longest roll's ~72k lines, and reaching it is never silent.
    require(kEvents>=50u*72000u,"event cap sized for whole runs");
    require(!p->takeCapNotice(),"no cap notice before the cap");
    for(uint32_t i=0;i<kEvents;++i) require(p->event(),"every event below the cap logs"),--checks;
    require(!p->takeCapNotice(),"no cap notice at the cap");
    require(!p->event(),"the event past the cap is refused");
    require(p->counts().eventsCapped==1,"event truncation visible");
    require(p->takeCapNotice(),"the first refused event gives the once-only notice");
    require(!p->event() && !p->takeCapNotice(),"the notice is given once");
    require(p->counts().eventsCapped==2,"every refused event counted");
    for(uint32_t i=3;i<kRecordings;++i) p->recording(stream,owner,64);
    require(p->recording(stream,owner,64)==0,"recording metadata cap");
    require(p->overlap(0x4040,4,w).status==Status::Capacity,"cap cannot become no-overlap evidence");
    p.reset();
    auto q=std::make_unique<State>(); const auto qs=q->stream();
    auto oldOwner=std::make_shared<int>(4);
    const auto old=q->recording(qs,oldOwner,64);
    q->write(old,0x7000,4096,3,w); q->submitted(old,oldOwner,1); q->completion(qs,1,10,true);
    oldOwner.reset();
    for(uint32_t i=0;i<kRecordings*2;++i) {
      auto ephemeral=std::make_shared<int>(5);
      require(q->recording(qs,ephemeral,64)!=0,"unreferenced expired recording can be reused");
    }
    require(q->overlap(0x7000,4,w).status==Status::CompletedOverlap,"old referenced write survives arbitrary metadata reuse");
    require(q->counts().recordingReused>0 && q->counts().capacity==0,"reuse does not consume cumulative capacity");
    auto replacement=std::make_shared<int>(6); const auto newer=q->recording(qs,replacement,64);
    q->write(newer,0x7000,4096,4,w); // releases the final old page reference
    uint64_t reused=0;
    for(uint32_t i=0;i<kRecordings;++i) {
      auto ephemeral=std::make_shared<int>(7); const auto id=q->recording(qs,ephemeral,64);
      if(uint32_t(id)==uint32_t(old)) { reused=id; break; }
    }
    require(reused && reused!=old,"reused slot has a distinct generation");
    q->write(old,0x8000,4096,3,w);
    require(q->overlap(0x8000,4,w).status==Status::MissingWrite,"stale slot generation cannot identify new recording");
    require(q->counts().weakPeakBytes<=uint64_t(kRecordings)*64,"retained weak allocation charges bounded");
    require(q->recording(qs,owner,0)==0,"unknown control-block allocation size refuses admission");

    // Whole ranges (resproof): walkRange grades every page of a range of any length (the movie planes' 100-300-page
    // locks were refused whole by the old 64-page cap); a long range lists only its completed GPU overlaps.
    auto g=std::make_unique<State>(); const auto gs=g->stream();
    auto gpuOwner=std::make_shared<int>(8); const auto rec=g->recording(gs,gpuOwner,64);
    for(uint32_t page=0x100;page<0x104;++page) g->write(rec,page*4096,4096,5,w);  // GPU output, pages 0x100..0x103
    g->write(rec,0x104*4096,4096,5,w);                                            // recorded; its reader sees unknown
    g->submitted(rec,gpuOwner,3); g->completion(gs,3,50,true);
    struct Line { uint32_t at, count; Status status; };
    std::vector<Line> lines;
    const auto seen=[&](uint32_t page) { Witness c=w; if(page==0x104) c.known=false; return c; };
    const auto keep=[&](uint32_t at,uint32_t count,const Overlap& o,const Witness&) { lines.push_back({at,count,o.status}); };
    auto t=walkRange(*g,0x100*4096,300*4096,seen,keep);
    require(t.pages==300,"a 300-page range is graded page by page, none refused");
    require(t.listed==4 && lines.size()==4,"a long range lists exactly its completed GPU overlaps");
    bool allGpu=true; for(const auto& l:lines) allGpu=allGpu && l.status==Status::CompletedOverlap && l.count==4096;
    require(allGpu && lines[0].at==0x100*4096 && lines[3].at==0x103*4096,"the listed pages are the GPU pages");
    require(t.unlisted[uint32_t(Status::UnknownWitness)]==1,"a long range's gap page is counted, not dropped");
    require(t.unlisted[uint32_t(Status::MissingWrite)]==295,"a long range's CPU-owned pages are counted");
    uint64_t sum=t.listed; for(auto n:t.unlisted) sum+=n;
    require(sum==t.pages,"every page of a long range is listed or counted");
    lines.clear(); t=walkRange(*g,0x100*4096,kDetailPages*4096,seen,keep);
    require(t.pages==kDetailPages && t.listed==kDetailPages && lines.size()==kDetailPages,
            "a range of exactly kDetailPages lists every page");
    require(lines[4].status==Status::UnknownWitness && lines[5].status==Status::MissingWrite,
            "a detailed range lists its gap and CPU pages with their status");
    lines.clear(); t=walkRange(*g,0x100*4096,(kDetailPages+1)*4096,seen,keep);
    require(t.pages==kDetailPages+1 && t.listed==4,"one page past kDetailPages lists only the GPU pages");
    lines.clear(); t=walkRange(*g,0x103*4096+0x800,0x1000,seen,keep);
    require(t.pages==2 && lines.size()==2 && lines[0].at==0x103*4096+0x800 && lines[0].count==0x800 &&
            lines[1].at==0x104*4096 && lines[1].count==0x800,"an unaligned range splits at the page boundary");
    require(lines[0].status==Status::CompletedOverlap && lines[1].status==Status::UnknownWitness,
            "the split pages keep their own status");
    // detail 0 (the vertex-buffer lock / invalidate coherencies): even a short range lists only its GPU-overlap pages
    lines.clear(); t=walkRange(*g,0x100*4096,10*4096,seen,keep,0);
    require(t.pages==10 && t.listed==4 && lines.size()==4,"detail 0 lists only the GPU pages of a short range");
    require(t.unlisted[uint32_t(Status::UnknownWitness)]==1 && t.unlisted[uint32_t(Status::MissingWrite)]==5,
            "detail 0 counts the short range's gap and CPU pages");
    std::printf("[RESOURCE_READER_TEST] checks=%u fail=0\n",checks); return 0;
  } catch(const std::exception& e) { std::fprintf(stderr,"[RESOURCE_READER_TEST] FAIL %s\n",e.what()); return 1; }
}
