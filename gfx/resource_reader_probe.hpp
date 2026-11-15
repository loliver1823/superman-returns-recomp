#pragma once
// Oracle observation only. A completed GPU overlap is not proof that the CPU
// bytes contain that GPU result, nor proof that this is an exhaustive reader set.
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>

namespace sr::gfx::reader_probe {
constexpr uint32_t kPages = 0x20000, kPageBytes = 4096;
constexpr uint32_t kRecordings = 8192, kStreams = 4, kCopies = 64;
// Whole-run census (lane D resproof, 2026-10-17). A range of any size is graded page by page (no page cap: the movie
// planes' texture locks are 100-300 pages); a range up to kDetailPages logs one line per page, a longer one logs a line
// per GPU-overlap page and one count line for the rest (walkRange). kEvents bounds the log lines of one process, sized
// for whole runs (the longest roll before this logged ~72k; the cap is 58x that). Reaching it is never silent: the
// first capped event logs once (takeCapNotice) and the census grades the run INCOMPLETE.
constexpr uint32_t kDetailPages = 64, kEvents = 1u << 22;
constexpr uint32_t kOwnerAllocationBytes = 1024;

struct Witness {
  uint64_t ownerSum = 0, grantDigest = 0, grantGeneration = 0;
  uint32_t cbSequence = 0, grants = 0, serial0 = 0, serial1 = 0;
  uint32_t cpu = 0, gpu = 0, window = 0;
  bool known = false;
};
bool sameLocalOwner(const Witness&, const Witness&) noexcept;
bool sameObservation(const Witness&, const Witness&) noexcept;

enum class Status : uint8_t {
  CompletedOverlap, CompletionNotObserved, MissingWrite, PartialWrite,
  UnknownWitness, ChangedOwner, Unsubmitted, ExpiredUnsubmitted,
  WrongRecording, InvalidRange, Capacity, UnknownCompletion
};
constexpr uint32_t kStatusCount = uint32_t(Status::UnknownCompletion) + 1;
const char* name(Status) noexcept;
struct Overlap {
  Status status = Status::MissingWrite;
  uint64_t recording = 0, ticket = 0, completed = 0, sampledNs = 0;
  uint32_t stream = 0, generation = 0, begin = 0, end = 0;
  Witness written{};
};
struct Copy {
  uint64_t id = 0, lockId = 0;
  uint32_t thread = 0, frame = 0, resource = 0, row = 0;
  uint32_t source = 0, destination = 0, bytes = 0;
  bool nativeRead = false, valid = false;
};
struct Counts {
  uint64_t locks = 0, lockUnknown = 0, copyAttempted = 0, copyReturned = 0;
  uint64_t nativeReadReturned = 0, copyUnknown = 0, pending = 0;
  uint64_t copyAbandoned = 0, copyRefused = 0, unmatchedReturns = 0, rangeRefused = 0;
  uint64_t recordings = 0, submitted = 0, writes = 0, writePages = 0;
  uint64_t overlaps = 0, completedOverlaps = 0, overlapUnknown = 0;
  uint64_t coherency = 0, events = 0, eventsCapped = 0, capacity = 0, invalid = 0;
  uint64_t streamErrors = 0, counterOverflow = 0;
  uint64_t recordingReused = 0, weakBytes = 0, weakPeakBytes = 0;
  uint64_t lockIntents = 0, gpuWriteSpans = 0;
  uint64_t otherCoherency = 0, lockGraded = 0, lockVirtual = 0;
};

// Fixed process bounds, no eviction and no strong GPU/resource ownership.
// Capacity failure poisons subsequent overlap answers; it cannot masquerade
// as an absence of writes. The caller serializes no GPU operations with this.
class State {
 public:
  State();
  uint32_t stream();
  uint64_t recording(uint32_t stream, std::weak_ptr<const void> identity, uint32_t ownerAllocationBytes);
  void submitted(uint64_t recording, const std::weak_ptr<const void>& identity, uint64_t ticket);
  void completion(uint32_t stream, uint64_t completed, uint64_t sampledNs, bool valid);
  void write(uint64_t recording, uint32_t begin, uint32_t bytes, uint32_t generation,
             const Witness& fullPageWitness);
  Overlap overlap(uint32_t begin, uint32_t bytes, const Witness& current);
  uint64_t lock(uint32_t thread, uint32_t frame, uint32_t resource, bool known);
  Copy attempt(uint32_t thread, uint32_t frame, uint32_t resource, uint32_t row,
               uint32_t source, uint32_t destination, uint32_t bytes);
  void nativeRead(uint32_t thread, uint32_t source, uint32_t destination, uint32_t bytes);
  Copy returned(uint32_t thread, uint32_t frame);
  uint64_t noteCoherency();
  // A graded CPU-note coherency other than a texture lock's (a vertex-buffer lock, an invalidate, ...): its id.
  uint64_t noteOtherCoherency();
  // A lock-core lock graded after its body (physical = its address is GPU memory, else counted only): its id.
  uint64_t noteLockGraded(bool physical);
  void noteLockIntent();
  void noteGpuWriteSpan();
  void refuseRange();
  bool event();
  // True exactly once, after the first event the cap refused (the caller logs that the census is incomplete).
  bool takeCapNotice();
  Counts counts() const;
  static constexpr uint64_t storageBound();
 private:
  struct Recording {
    std::weak_ptr<const void> identity;
    uint64_t ticket = 0;
    uint32_t stream = 0, generation = 0, references = 0, ownerBytes = 0;
    bool bad = false;
  };
  struct Page { Witness witness; uint64_t recording = 0; uint32_t begin = 0, end = 0, generation = 0; };
  struct Stream { uint64_t completed = 0, sampledNs = 0; bool valid = false; };
  struct Lock { uint64_t id = 0; uint32_t thread = 0, frame = 0, resource = 0; };
  void bump(uint64_t&);
  Recording* findRecording(uint64_t id);
  mutable std::mutex mutex_;
  std::unique_ptr<std::array<Page, kPages>> pages_;
  std::array<Recording, kRecordings> recordings_{};
  std::array<Stream, kStreams> streams_{};
  std::array<Copy, kCopies> copies_{};
  std::array<Lock, kCopies> locks_{};
  uint32_t streamCount_ = 0, nextRecording_ = 0;
  uint64_t copySequence_ = 0, lockSequence_ = 0;
  bool incomplete_ = false, capNotice_ = false;
  Counts counts_{};
};
constexpr uint64_t State::storageBound() {
  return sizeof(State) + sizeof(std::array<Page, kPages>) + uint64_t(kRecordings)*kOwnerAllocationBytes;
}
static_assert(State::storageBound() <= 24u * 1024u * 1024u);

// One graded range [phys, phys + bytes) (the caller has refused an empty range or one past 512 MiB): every page's
// overlap against witness(page index) (the caller's bracketed observation). line(pagePhys, count, overlap, current) runs
// for the pages a caller lists - every page of a range up to kDetailPages, else only its CompletedOverlap pages (the
// finding) - and the rest are counted by status in the tally (the caller logs them as one line). No page is skipped.
struct RangeTally {
  uint64_t pages = 0, listed = 0;
  std::array<uint64_t, kStatusCount> unlisted{};
};
// detailPages: the longest range listed page by page (kDetailPages for a texture lock; 0 = only the GPU-overlap pages
// of any range, the coherency sites with many small CPU-owned ranges - vertex-buffer locks, invalidates).
template <class WitnessFn, class LineFn>
RangeTally walkRange(State& p, uint32_t phys, uint32_t bytes, WitnessFn&& witness, LineFn&& line,
                     uint32_t detailPages = kDetailPages) {
  RangeTally t;
  const uint64_t end = uint64_t(phys) + bytes;
  const bool detail = ((end - 1) >> 12) - (phys >> 12) + 1 <= detailPages;
  for (uint64_t at = phys; at < end;) {
    const uint64_t pageEnd = (at | 4095ull) + 1;
    const uint32_t count = uint32_t((end < pageEnd ? end : pageEnd) - at);
    const Witness current = witness(uint32_t(at >> 12));
    const Overlap result = p.overlap(uint32_t(at), count, current);
    ++t.pages;
    if (detail || result.status == Status::CompletedOverlap) {
      ++t.listed;
      line(uint32_t(at), count, result, current);
    } else {
      ++t.unlisted[uint32_t(result.status)];
    }
    at += count;
  }
  return t;
}
}  // namespace sr::gfx::reader_probe
