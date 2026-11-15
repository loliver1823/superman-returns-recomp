#include "resource_reader_probe.hpp"
#include <algorithm>
#include <limits>

namespace sr::gfx::reader_probe {
namespace {
bool identityEqual(const std::weak_ptr<const void>& a, const std::weak_ptr<const void>& b) {
  return !a.owner_before(b) && !b.owner_before(a);
}
bool pageRange(uint32_t begin, uint32_t bytes) {
  return bytes && uint64_t(begin) + bytes <= uint64_t(kPages) * kPageBytes &&
         (begin >> 12) == ((uint64_t(begin) + bytes - 1) >> 12);
}
bool guestRange(uint32_t begin, uint32_t bytes) {
  return begin >= 0x10000 && bytes && uint64_t(begin) + bytes <= 0x100000000ull;
}
}
bool sameLocalOwner(const Witness& a, const Witness& b) noexcept {
  return a.known && b.known && a.ownerSum == b.ownerSum && a.grantDigest == b.grantDigest &&
         a.grants == b.grants && a.serial0 == b.serial0 && a.serial1 == b.serial1;
}
bool sameObservation(const Witness& a, const Witness& b) noexcept {
  return sameLocalOwner(a,b) && a.grantGeneration == b.grantGeneration &&
         a.cbSequence == b.cbSequence && a.cpu == b.cpu && a.gpu == b.gpu && a.window == b.window;
}
const char* name(Status s) noexcept {
  switch (s) {
    case Status::CompletedOverlap: return "completed_prior_gpu_overlap";
    case Status::CompletionNotObserved: return "completion_not_observed";
    case Status::MissingWrite: return "no_recorded_write_observed";
    case Status::PartialWrite: return "latest_partial_write_does_not_cover";
    case Status::UnknownWitness: return "unknown_witness";
    case Status::ChangedOwner: return "changed_owner";
    case Status::Unsubmitted: return "unsubmitted";
    case Status::ExpiredUnsubmitted: return "expired_unsubmitted";
    case Status::WrongRecording: return "wrong_recording";
    case Status::InvalidRange: return "invalid_range";
    case Status::Capacity: return "capacity";
    case Status::UnknownCompletion: return "unknown_completion";
  }
  return "invalid_status";
}
State::State() : pages_(std::make_unique<std::array<Page,kPages>>()) {}
void State::bump(uint64_t& n) {
  if (n == UINT64_MAX) { counts_.counterOverflow = 1; incomplete_ = true; }
  else ++n;
}
uint32_t State::stream() {
  std::lock_guard lock(mutex_);
  if (streamCount_ == kStreams) { bump(counts_.capacity); incomplete_ = true; return 0; }
  return ++streamCount_;
}
State::Recording* State::findRecording(uint64_t id) {
  const uint32_t index=uint32_t(id),generation=uint32_t(id>>32);
  if(!index || index>kRecordings || !generation) return nullptr;
  auto& r=recordings_[index-1];
  return r.generation==generation?&r:nullptr;
}
uint64_t State::recording(uint32_t streamId, std::weak_ptr<const void> identity, uint32_t ownerBytes) {
  std::lock_guard lock(mutex_);
  if (!streamId || streamId > streamCount_ || identity.expired() || !ownerBytes || ownerBytes>kOwnerAllocationBytes) {
    bump(counts_.invalid); incomplete_=true; return 0;
  }
  for(uint32_t n=0;n<kRecordings;++n) {
    const uint32_t index=(nextRecording_+n)%kRecordings;
    auto& r=recordings_[index];
    if(r.references || (r.generation && !r.identity.expired()) || r.generation==UINT32_MAX) continue;
    const uint32_t generation=r.generation+1;
    if(r.generation) bump(counts_.recordingReused);
    counts_.weakBytes-=r.ownerBytes;
    r={std::move(identity),0,streamId,generation,0,ownerBytes,false};
    counts_.weakBytes+=ownerBytes; counts_.weakPeakBytes=std::max(counts_.weakPeakBytes,counts_.weakBytes);
    bump(counts_.recordings); nextRecording_=(index+1)%kRecordings;
    return (uint64_t(generation)<<32)|(index+1);
  }
  bump(counts_.capacity); incomplete_=true; return 0;
}
void State::submitted(uint64_t id, const std::weak_ptr<const void>& identity, uint64_t ticket) {
  std::lock_guard lock(mutex_);
  auto* slot=findRecording(id);
  if (!slot) { bump(counts_.invalid); incomplete_ = true; return; }
  auto& r = *slot;
  if (!ticket || r.ticket || !identityEqual(r.identity,identity) || identity.expired()) {
    r.bad = true; bump(counts_.invalid); return;
  }
  r.ticket = ticket; bump(counts_.submitted);
}
void State::completion(uint32_t id, uint64_t completed, uint64_t sampledNs, bool valid) {
  std::lock_guard lock(mutex_);
  if (!id || id > streamCount_) { bump(counts_.streamErrors); return; }
  auto& s = streams_[id-1];
  if (!valid || completed == UINT64_MAX || !sampledNs || completed < s.completed || sampledNs < s.sampledNs) {
    s.valid = false; bump(counts_.streamErrors); return;
  }
  s = {completed,sampledNs,true};
}
void State::write(uint64_t id, uint32_t begin, uint32_t bytes, uint32_t generation, const Witness& w) {
  std::lock_guard lock(mutex_); bump(counts_.writes);
  if (!pageRange(begin,bytes)) { bump(counts_.invalid); incomplete_ = true; return; }
  // Even an unknown later write replaces the old answer. Never rescue it with
  // an older complete write or by interpreting an expired owner as completion.
  auto& p = (*pages_)[begin >> 12];
  if(auto* old=findRecording(p.recording)) {
    if(!old->references) { bump(counts_.invalid); incomplete_=true; }
    else --old->references;
  }
  p = {w,id,begin,begin+bytes,generation}; bump(counts_.writePages);
  auto* slot=findRecording(id);
  if (!slot || !generation) { p.recording = 0; bump(counts_.invalid); }
  else ++slot->references;
}
Overlap State::overlap(uint32_t begin, uint32_t bytes, const Witness& current) {
  std::lock_guard lock(mutex_); bump(counts_.overlaps);
  Overlap out;
  auto finish = [&](Status s) { out.status=s; if(s==Status::CompletedOverlap) bump(counts_.completedOverlaps);
    else bump(counts_.overlapUnknown); return out; };
  if (incomplete_) return finish(Status::Capacity);
  if (!pageRange(begin,bytes)) return finish(Status::InvalidRange);
  const auto& p=(*pages_)[begin >> 12];
  out.recording=p.recording; out.generation=p.generation; out.begin=p.begin; out.end=p.end; out.written=p.witness;
  if (!p.recording) return finish(Status::MissingWrite);
  if (!current.known || !p.witness.known) return finish(Status::UnknownWitness);
  if (!sameLocalOwner(current,p.witness)) return finish(Status::ChangedOwner);
  if (begin < p.begin || uint64_t(begin)+bytes > p.end) return finish(Status::PartialWrite);
  const auto* slot=findRecording(p.recording);
  if(!slot) return finish(Status::WrongRecording);
  const auto& r=*slot; out.ticket=r.ticket; out.stream=r.stream;
  if (r.bad) return finish(Status::WrongRecording);
  if (!r.ticket) return finish(r.identity.expired()?Status::ExpiredUnsubmitted:Status::Unsubmitted);
  const auto& s=streams_[r.stream-1]; out.completed=s.completed; out.sampledNs=s.sampledNs;
  if (!s.valid) return finish(Status::UnknownCompletion);
  return finish(s.completed >= r.ticket ? Status::CompletedOverlap : Status::CompletionNotObserved);
}
uint64_t State::lock(uint32_t thread, uint32_t frame, uint32_t resource, bool known) {
  std::lock_guard guard(mutex_); bump(counts_.locks);
  if (!known) bump(counts_.lockUnknown);
  for (auto& l:locks_) if (!l.id || (l.thread==thread && l.frame==frame)) {
    if(lockSequence_==UINT64_MAX) { incomplete_=true; counts_.counterOverflow=1; return 0; }
    l={++lockSequence_,thread,frame,resource}; return l.id;
  }
  bump(counts_.capacity); return 0;
}
Copy State::attempt(uint32_t thread, uint32_t frame, uint32_t resource, uint32_t row,
                    uint32_t source, uint32_t destination, uint32_t bytes) {
  std::lock_guard lock(mutex_); bump(counts_.copyAttempted);
  for (auto& c:copies_) if(c.id && c.thread==thread && c.frame==frame) {
    // An interrupted previous call is not a returned pixel read.
    bump(counts_.copyAbandoned); --counts_.pending; c={};
  }
  for (auto& c:copies_) if(!c.id) {
    if(copySequence_==UINT64_MAX) { incomplete_=true; counts_.counterOverflow=1; bump(counts_.copyRefused); return {}; }
    c={++copySequence_,0,thread,frame,resource,row,source,destination,bytes,false,
       guestRange(source,bytes)&&guestRange(destination,bytes)};
    for(const auto& l:locks_) if(l.id && l.thread==thread && l.frame==frame && l.resource==resource) c.lockId=l.id;
    bump(counts_.pending); return c;
  }
  bump(counts_.capacity); bump(counts_.copyRefused); return {};
}
void State::nativeRead(uint32_t thread, uint32_t source, uint32_t destination, uint32_t bytes) {
  std::lock_guard lock(mutex_);
  Copy* found=nullptr;
  for(auto& c:copies_) if(c.id && c.thread==thread && c.source==source && c.destination==destination && c.bytes==bytes) {
    if(found) { bump(counts_.invalid); return; } found=&c;
  }
  if(found && found->valid) found->nativeRead=true;
}
Copy State::returned(uint32_t thread, uint32_t frame) {
  std::lock_guard lock(mutex_);
  for(auto& c:copies_) if(c.id && c.thread==thread && c.frame==frame) {
    Copy out=c; c={}; --counts_.pending; bump(counts_.copyReturned);
    if(out.nativeRead && out.valid) bump(counts_.nativeReadReturned); else bump(counts_.copyUnknown);
    return out;
  }
  bump(counts_.unmatchedReturns); return {};
}
uint64_t State::noteCoherency() { std::lock_guard lock(mutex_); bump(counts_.coherency); return counts_.coherency; }
uint64_t State::noteOtherCoherency() {
  std::lock_guard lock(mutex_); bump(counts_.otherCoherency); return counts_.otherCoherency;
}
uint64_t State::noteLockGraded(bool physical) {
  std::lock_guard lock(mutex_);
  bump(physical ? counts_.lockGraded : counts_.lockVirtual);
  return counts_.lockGraded + counts_.lockVirtual;
}
void State::noteLockIntent() { std::lock_guard lock(mutex_); bump(counts_.lockIntents); }
void State::noteGpuWriteSpan() { std::lock_guard lock(mutex_); bump(counts_.gpuWriteSpans); }
void State::refuseRange() { std::lock_guard lock(mutex_); bump(counts_.rangeRefused); }
bool State::event() {
  std::lock_guard lock(mutex_);
  if(counts_.events >= kEvents) { bump(counts_.eventsCapped); return false; }
  bump(counts_.events); return true;
}
bool State::takeCapNotice() {
  std::lock_guard lock(mutex_);
  if(!counts_.eventsCapped || capNotice_) return false;
  capNotice_=true; return true;
}
Counts State::counts() const { std::lock_guard lock(mutex_); return counts_; }
}  // namespace sr::gfx::reader_probe
