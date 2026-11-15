#pragma once
// iter-467 native-a: the STRICT transport witness of segmentless (p2.segmentless v2, the orchestrator 2026-10-11),
// observe-only, under SR_NATIVE_SEG_TILE=1 (default OFF: the default path never calls it). Pure logic here (no
// globals, no guest memory) so tools/difftest/seg_tile_test.cpp can drive it with a synthetic arena; the call sites
// and the counters' log line are in kernel/native_flush.cpp.
//
//   Tile (the PRODUCER side): every arena byte the record writer passes must be claimed exactly once - by a record
//   part of a REGISTERED segment, by a record the marker path owns (unsegmented: segmentless was inactive for that
//   span), or as the writer's no-straddle padding before an arena boundary. At a close that took every footprint,
//   everything below the arena head must be claimed:
//     gap      a hole below the head at a close: a record no registered segment holds (orphaned - it would reach the
//              reclaim audit as NEVER-EXECUTED); each hole is counted once
//     overlap  a range claimed twice (a record in two segments, or twice in one)
//     order    a segment whose record parts are not in strictly increasing arena position (emission order)
//   First (the CP side): at a segment's FIRST application (a deferred entry's replays are the guest re-running its
//   own buffer, by design, and are not checked) every record part must not have run before and the parts must come
//   in strictly increasing arena position:
//     dup      a record part already executed when its segment is first applied
//     reorder  a record part at or below the previous part's position within the segment
//     idreorder  a non-deferred segment first applied after a segment that closed later (registry ids are assigned
//              at the close, in order; a segment may lie lower in the arena, so the order across segments is by id)
//   Drained (kernel/native_flush.cpp segTileDrained, at every return of the guest's BlockUntilIdle outside a Z-pass
//   bracket): the device thread waited for the GPU to finish everything it closed, so every registered segment and
//   record part has had its first application - registered == first-applied, or drainMiss.
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>

namespace sr::native::segtile {

struct TileCounters {
  uint64_t segs = 0, recs = 0, unsegmented = 0, pads = 0, gap = 0, overlap = 0, order = 0;
};

class Tile {
 public:
  explicit Tile(uint64_t start = 0) : water_(start) {}
  // [lo, hi): the writer's padding before an arena boundary (no straddle) - claimed, never a gap.
  void pad(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return;
    c_.pads++;
    claim(lo, hi);
  }
  // A record the marker path owns (the span ran with segmentless inactive): legitimate, counted apart.
  void unsegmented(uint64_t pos, uint32_t bytes) {
    c_.unsegmented++;
    claim(pos, pos + bytes);
  }
  // A registered segment's record parts, in part order.
  void segment(const uint64_t* pos, const uint32_t* bytes, size_t n) {
    c_.segs++;
    for (size_t i = 0; i < n; i++) {
      c_.recs++;
      if (i && pos[i] <= pos[i - 1]) c_.order++;
      claim(pos[i], pos[i] + bytes[i]);
    }
  }
  // A close that took every footprint: every byte below head must be claimed. Each hole counts once.
  void closeAt(uint64_t head) {
    advance();  // after it, the first pending range (if any) starts above the water line: [water_, it) is a hole
    while (water_ < head) {
      auto it = pend_.begin();
      const bool tail = it == pend_.end() || it->first >= head;
      c_.gap++;  // the hole [water_, next)
      water_ = tail ? head : it->first;
      advance();
      if (tail) break;
    }
  }
  const TileCounters& counters() const { return c_; }
  uint64_t water() const { return water_; }

 private:
  void claim(uint64_t lo, uint64_t hi) {
    if (lo < water_) {
      c_.overlap++;
      return;
    }
    auto nx = pend_.lower_bound(lo);
    if (nx != pend_.end() && nx->first < hi) {
      c_.overlap++;
      return;
    }
    if (nx != pend_.begin() && std::prev(nx)->second > lo) {
      c_.overlap++;
      return;
    }
    pend_.emplace(lo, hi);
    advance();
  }
  void advance() {
    for (auto it = pend_.begin(); it != pend_.end() && it->first == water_; it = pend_.begin()) {
      water_ = it->second;
      pend_.erase(it);
    }
  }
  uint64_t water_;
  std::map<uint64_t, uint64_t> pend_;  // claimed ranges above the water line: lo -> hi
  TileCounters c_;
};

struct FirstCounters {
  uint64_t segs = 0, recs = 0, dup = 0, reorder = 0, idreorder = 0, deferred = 0;
};

class First {
 public:
  // id: the segment's registry id (1-based, assigned at its close, never reused; 0 = unknown). Across segments the
  // first applications must follow the close order: a non-deferred segment whose id is not above every earlier
  // non-deferred first application counts idreorder. A deferred segment (a Z-pass bracket's, applied by the guest's own
  // bracket replay) is counted apart and does not move the order.
  void begin(uint64_t id = 0, bool deferred = false) {
    c_.segs++;
    have_ = false;
    if (deferred) {
      c_.deferred++;
      return;
    }
    if (id) {
      if (id <= lastId_) c_.idreorder++;
      else lastId_ = id;
    }
  }
  void rec(uint64_t pos, bool alreadyExecuted) {
    c_.recs++;
    if (alreadyExecuted) c_.dup++;
    if (have_ && pos <= last_) c_.reorder++;
    last_ = pos;
    have_ = true;
  }
  const FirstCounters& counters() const { return c_; }

 private:
  uint64_t last_ = 0, lastId_ = 0;
  bool have_ = false;
  FirstCounters c_;
};

}  // namespace sr::native::segtile
