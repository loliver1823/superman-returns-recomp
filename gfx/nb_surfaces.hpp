// iter-467 native-b, EDRAM zero step 2 (docs/NATIVE_ROADMAP.md "EDRAM zero: the plan"): the surface table.
//
// The EDRAM model decides which pixels move between host render targets with a per-TILE ownership map
// (edram::Ownership, a port of Xenia's render_target_cache ownership ranges): every tile range names the surface
// key that wrote it last. This table keeps the same knowledge per SURFACE - each surface key with the tile runs it
// currently owns - so a bind or a resolve is answered by asking which other surfaces hold part of the range it
// claims, and the answer is a list of surface-to-surface operations. It is exact by construction (the runs of all
// surfaces are disjoint and a claim moves the claimed range to the claiming surface, as the map does), and
// SR_NB_SURF_VERIFY=1 proves it: both decide every claim and the move lists must be identical.
//
// The tile coordinates stay: an Xbox 360 surface IS a base + pitch in EDRAM tiles (the XDK allocates surfaces by
// EDRAM offset, and the game aliases them on purpose - a 4x surface cleared then rendered at 1x, depth read as
// colour), so the overlap of two surfaces' tile ranges is the game's own statement of which surface's pixels the
// next one inherits.
#pragma once

#include "surface_key.hpp"  // C1 stage 3: the surface identity and the alias geometry (shipping; not the word model)

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sr {
namespace gfx {
namespace nbsurf {

struct Run {
  uint32_t s = 0, e = 0;  // absolute tiles, s < e <= edram::kTileCount (no wrap)
};

struct Surface {
  edram::RtKey key;
  std::vector<Run> own;  // disjoint, sorted, no two adjacent (merged)
};

class Table {
 public:
  void reset() { surf_.clear(); }
  size_t size() const { return surf_.size(); }

  // The surfaces owning [s, e) (no wrap) in tile order; a range nobody owns comes back with an empty key, as
  // edram::Ownership::owners reports it.
  void owners(uint32_t s, uint32_t e, std::vector<edram::Transfer>& out) const {
    if (s >= e) return;
    const size_t first = out.size();
    for (const Surface& f : surf_)
      for (const Run& r : f.own) {
        const uint32_t a = std::max(r.s, s), b = std::min(r.e, e);
        if (a < b) out.push_back(edram::Transfer{a, b, f.key});
      }
    std::sort(out.begin() + std::ptrdiff_t(first), out.end(),
              [](const edram::Transfer& x, const edram::Transfer& y) { return x.start < y.start; });
    // the gaps: nobody's
    std::vector<edram::Transfer> gaps;
    uint32_t at = s;
    for (size_t i = first; i < out.size(); i++) {
      if (out[i].start > at) gaps.push_back(edram::Transfer{at, out[i].start, edram::RtKey()});
      at = out[i].end;
    }
    if (at < e) gaps.push_back(edram::Transfer{at, e, edram::RtKey()});
    if (gaps.empty()) return;
    out.insert(out.end(), gaps.begin(), gaps.end());
    std::sort(out.begin() + std::ptrdiff_t(first), out.end(),
              [](const edram::Transfer& x, const edram::Transfer& y) { return x.start < y.start; });
  }

  // [s, e) (no wrap) lies inside one run of k: a claim of it moves nothing and changes nothing.
  bool ownedBy(edram::RtKey k, uint32_t s, uint32_t e) const {
    if (s >= e) return true;
    for (const Surface& f : surf_) {
      if (f.key != k) continue;
      for (const Run& r : f.own)
        if (r.s <= s && r.e >= e) return true;
      return false;
    }
    return false;
  }

  // The claim of [s, e) (no wrap) by k: every run another surface owns inside it is appended to `moves` (tile
  // order; the source surface's pixels have to reach k), and k owns the whole range afterwards. Unowned tiles are
  // not reported here (owners() lists them before the claim, as pm4Claim does with the map).
  void claimExtent(edram::RtKey k, uint32_t s, uint32_t e, std::vector<edram::Transfer>& moves) {
    if (s >= e) return;
    const size_t first = moves.size();
    for (size_t i = 0; i < surf_.size();) {
      Surface& f = surf_[i];
      bool hit = false;
      if (f.key != k)
        for (const Run& r : f.own) hit = hit || (r.s < e && s < r.e);
      if (!hit) {  // (most surfaces: nothing of theirs in the range)
        i++;
        continue;
      }
      thread_local std::vector<Run> keep;
      keep.clear();
      for (const Run& r : f.own) {
        const uint32_t a = std::max(r.s, s), b = std::min(r.e, e);
        if (a >= b) {
          keep.push_back(r);
          continue;
        }
        moves.push_back(edram::Transfer{a, b, f.key});
        if (r.s < a) keep.push_back(Run{r.s, a});
        if (b < r.e) keep.push_back(Run{b, r.e});
      }
      f.own.assign(keep.begin(), keep.end());
      if (f.own.empty()) {
        surf_.erase(surf_.begin() + std::ptrdiff_t(i));
        continue;
      }
      i++;
    }
    std::sort(moves.begin() + std::ptrdiff_t(first), moves.end(),
              [](const edram::Transfer& x, const edram::Transfer& y) { return x.start < y.start; });
    // k owns [s, e) now
    Surface* me = nullptr;
    for (Surface& f : surf_)
      if (f.key == k) me = &f;
    if (!me) {
      surf_.push_back(Surface{k, {}});
      me = &surf_.back();
    }
    me->own.push_back(Run{s, e});
    std::sort(me->own.begin(), me->own.end(), [](const Run& x, const Run& y) { return x.s < y.s; });
    size_t w = 0;  // merge in place (touching or overlapping runs)
    for (size_t r = 1; r < me->own.size(); r++) {
      if (me->own[r].s <= me->own[w].e) me->own[w].e = std::max(me->own[w].e, me->own[r].e);
      else me->own[++w] = me->own[r];
    }
    me->own.resize(w + 1);
  }

 private:
  std::vector<Surface> surf_;
};

}  // namespace nbsurf
}  // namespace gfx
}  // namespace sr
