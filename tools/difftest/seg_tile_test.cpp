// iter-467 native-a: the strict transport witness of segmentless (kernel/seg_tile.hpp, SR_NATIVE_SEG_TILE).
//
//   --seg-tile-test   a synthetic arena of 1024 bytes (the writer's no-straddle padding at its end):
//     T1  a clean stream - three segments, the end padding, an unsegmented (marker-path) record - counts nothing
//         (gap / overlap / order 0) while the padding and the unsegmented record are claimed
//     T2  the same stream with ONE gap (a record no segment claims), ONE overlap (a record claimed by two segments)
//         and ONE swapped pair (a segment listing two records out of emission order): gap, overlap and order each
//         exactly 1, the padding still no gap, and a second close over the same hole does not count it again
//     F1  the CP first-application check, clean: in-order, never-run parts count nothing
//     F2  ONE part that already ran (dup) and ONE pair out of order within a segment (reorder): each exactly 1; the
//         order is per segment (a new segment may start lower)
//     F3  across segments the order is the close order (registry ids): two segments first applied after a
//         later-closed one count idreorder exactly 2; a deferred (bracket) segment is counted apart, id 0 unordered
#include "kernel/seg_tile.hpp"

#include <cstdio>
#include <initializer_list>

namespace T = sr::native::segtile;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) {
    g_pass++;
    std::printf("  ok  %s\n", name);
  } else {
    g_fail++;
    std::printf("FAIL: %s\n", name);
  }
}

// The synthetic stream: records R0..R7 (pos, bytes); [300, 1024) is the padding before the arena boundary.
constexpr uint64_t P[] = {0, 100, 148, 1024, 1100, 1200, 1250, 1300};
constexpr uint32_t B[] = {100, 48, 152, 76, 100, 50, 50, 100};

void seg(T::Tile& t, std::initializer_list<int> recs) {
  uint64_t pos[8];
  uint32_t bytes[8];
  size_t n = 0;
  for (int r : recs) {
    pos[n] = P[r];
    bytes[n] = B[r];
    n++;
  }
  t.segment(pos, bytes, n);
}

void print(const char* tag, const T::TileCounters& c, uint64_t water) {
  std::printf("  %s: segs=%llu recs=%llu unsegmented=%llu pads=%llu gap=%llu overlap=%llu order=%llu water=%llu\n", tag,
              (unsigned long long)c.segs, (unsigned long long)c.recs, (unsigned long long)c.unsegmented,
              (unsigned long long)c.pads, (unsigned long long)c.gap, (unsigned long long)c.overlap,
              (unsigned long long)c.order, (unsigned long long)water);
}
}  // namespace

int runSegTileTest() {
  std::printf("[SEG_TILE_TEST] the producer tile check and the CP first-application check (kernel/seg_tile.hpp)\n");
  {  // T1: clean
    T::Tile t;
    seg(t, {0, 1});
    t.closeAt(148);
    seg(t, {2});
    t.pad(300, 1024);
    seg(t, {3, 4});
    t.closeAt(1200);
    t.unsegmented(P[5], B[5]);  // a marker-path record (segmentless inactive for its span)
    seg(t, {6, 7});
    t.closeAt(1400);
    const auto& c = t.counters();
    print("T1", c, t.water());
    check(c.gap == 0 && c.overlap == 0 && c.order == 0, "T1 clean stream: gap 0, overlap 0, order 0");
    check(c.pads == 1 && c.unsegmented == 1 && c.segs == 4 && c.recs == 7 && t.water() == 1400,
          "T1 the padding and the unsegmented record are claimed: pads 1, unsegmented 1, water at the head");
  }
  {  // T2: one gap, one overlap, one swapped pair
    T::Tile t;
    seg(t, {0, 1});
    t.closeAt(148);
    seg(t, {2});
    t.pad(300, 1024);
    seg(t, {4, 3});  // the swapped pair
    t.closeAt(1200);
    const uint64_t g0 = t.counters().gap;
    seg(t, {6});     // R5 is never claimed: the gap
    t.closeAt(1300);
    t.closeAt(1300);  // the same hole again: not recounted
    seg(t, {1});      // R1 claimed a second time: the overlap
    seg(t, {7});
    t.closeAt(1400);
    const auto& c = t.counters();
    print("T2", c, t.water());
    check(g0 == 0, "T2 the end padding is no gap (gap 0 after the close past it)");
    check(c.gap == 1, "T2 one unclaimed record: gap exactly 1 (a second close over the hole does not recount it)");
    check(c.overlap == 1, "T2 one record claimed by two segments: overlap exactly 1");
    check(c.order == 1, "T2 one segment with a swapped pair: order exactly 1");
    check(c.pads == 1 && c.unsegmented == 0 && t.water() == 1400, "T2 pads 1, unsegmented 0, water at the head");
  }
  {  // F1: clean first applications
    T::First f;
    f.begin();
    f.rec(10, false);
    f.rec(20, false);
    f.begin();
    f.rec(30, false);
    f.rec(40, false);
    const auto& c = f.counters();
    std::printf("  F1: segs=%llu recs=%llu dup=%llu reorder=%llu\n", (unsigned long long)c.segs,
                (unsigned long long)c.recs, (unsigned long long)c.dup, (unsigned long long)c.reorder);
    check(c.dup == 0 && c.reorder == 0 && c.segs == 2 && c.recs == 4, "F1 clean: dup 0, reorder 0");
  }
  {  // F2: one dup, one reorder
    T::First f;
    f.begin();
    f.rec(10, false);
    f.rec(20, false);
    f.begin();
    f.rec(50, false);
    f.rec(45, false);  // out of order within its segment
    f.begin();
    f.rec(60, true);   // already executed at its segment's first application
    f.begin();
    f.rec(5, false);   // a new segment may start lower: not a reorder
    const auto& c = f.counters();
    std::printf("  F2: segs=%llu recs=%llu dup=%llu reorder=%llu\n", (unsigned long long)c.segs,
                (unsigned long long)c.recs, (unsigned long long)c.dup, (unsigned long long)c.reorder);
    check(c.dup == 1, "F2 one part already executed: dup exactly 1");
    check(c.reorder == 1, "F2 one pair out of order within a segment: reorder exactly 1 (a new segment starting lower is not)");
  }
  {  // F3: the order across segments is the close order (registry ids); deferred (bracket) segments apart
    T::First f;
    f.begin(1);
    f.rec(100, false);
    f.begin(2);
    f.rec(50, false);   // lower in the arena, later in close order: fine
    f.begin(5, true);   // a deferred bracket segment applied by the replay: counted apart
    f.rec(10, false);
    f.begin(3);
    f.rec(60, false);
    f.begin(3);         // the same id first-applied again
    f.rec(70, false);
    f.begin(2);         // an earlier-closed segment first applied after a later one
    f.rec(80, false);
    f.begin(4);
    f.rec(90, false);
    f.begin(0);         // an unknown id: not ordered
    f.rec(95, false);
    const auto& c = f.counters();
    std::printf("  F3: segs=%llu idreorder=%llu deferred=%llu\n", (unsigned long long)c.segs,
                (unsigned long long)c.idreorder, (unsigned long long)c.deferred);
    check(c.idreorder == 2, "F3 two segments out of close order: idreorder exactly 2 (lower arena positions are not)");
    check(c.deferred == 1 && c.segs == 8, "F3 the deferred segment counted apart, every first application counted");
  }
  std::printf("[SEG_TILE_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
