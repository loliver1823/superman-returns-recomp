#pragma once

// The EDRAM WORD model (oracle; docs/RELEASE_COMPOSITION.md section 8, stage 3): the per-tile ownership map
// (edram::Ownership, Xenia render_target_cache's ownership ranges - the shipping path decides with the surface table of
// gfx/nb_surfaces.hpp and this map checks it under SR_NB_SURF_VERIFY), the sample addressing of the 10 MiB EDRAM
// buffer (sampleOffsetBytes: shaders/edram.xesli XeEdramOffsetBytes) and the CPU resolve from the mirror
// (resolveToGuest). Read by gfx/d3d12_edram_model.cpp, the tests and the replay; a shipping unit reads
// gfx/surface_key.hpp instead (the audit's content rule: this header is excluded content).
//
// Ported from Xenia (reference/xenia-canary/src/xenia/gpu/, BSD-3-Clause, Copyright Ben Vanik); the contract is
// docs/XENIA_EDRAM_CONTRACT.md.

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "surface_key.hpp"

namespace sr {
namespace gfx {
namespace edram {

constexpr uint32_t kTileDwords = kTileWidthSamples * kTileHeightSamples;  // 1280
constexpr uint32_t kEdramBytes = kTileDwords * 4u * kTileCount;           // 10 MiB

// ── Tile ownership (render_target_cache.h:607-717, .cc:1510-1667) ───────────
class Ownership {
 public:
  Ownership() { reset(); }
  void reset() {
    ranges_.clear();
    ranges_[0] = Range{kTileCount, RtKey()};
  }
  // Claim [base, base+length) (wrapping mod 2048) for `dest`. Every sub-range
  // whose previous owner is a different, non-empty key is appended to
  // `transfers` (the data has to move before `dest` is drawn to).
  void claim(RtKey dest, uint32_t base, uint32_t length, std::vector<Transfer>& transfers) {
    if (!length) return;
    length = std::min(length, kTileCount);
    base %= kTileCount;
    const uint32_t end = base + length;
    if (end <= kTileCount) {
      claimExtent(dest, base, end, transfers);
    } else {
      claimExtent(dest, base, kTileCount, transfers);
      claimExtent(dest, 0, end - kTileCount, transfers);
    }
  }
  // The owners of [start, end) in order (no wrap; caller splits).
  void owners(uint32_t start, uint32_t end, std::vector<Transfer>& out) const {
    if (start >= end) return;
    auto it = ranges_.upper_bound(start);
    --it;
    for (; it != ranges_.end() && it->first < end; ++it) {
      const uint32_t s = std::max(it->first, start);
      const uint32_t e = std::min(it->second.end, end);
      if (s < e) out.push_back(Transfer{s, e, it->second.owner});
    }
  }
  size_t rangeCount() const { return ranges_.size(); }
  // iter-467 native-b B-CPU: [start, end) (no wrap) lies inside ONE range owned by `k` - a claim of it moves
  // nothing and, the map being kept merged (no two neighbours with one owner), leaves the map as it is.
  bool ownedBy(RtKey k, uint32_t start, uint32_t end) const {
    if (start >= end) return true;
    auto it = ranges_.upper_bound(start);
    --it;
    return it->second.owner == k && it->second.end >= end;
  }

 private:
  struct Range { uint32_t end; RtKey owner; };
  std::map<uint32_t, Range> ranges_;

  void splitAt(uint32_t at) {
    if (at == 0 || at >= kTileCount) return;
    auto it = ranges_.upper_bound(at);
    --it;
    if (it->first == at) return;
    const Range r = it->second;
    it->second.end = at;
    ranges_[at] = Range{r.end, r.owner};
  }
  void claimExtent(RtKey dest, uint32_t s, uint32_t e, std::vector<Transfer>& transfers) {
    splitAt(s);
    splitAt(e);
    for (auto it = ranges_.find(s); it != ranges_.end() && it->first < e; ++it) {
      Range& r = it->second;
      if (r.owner == dest) continue;
      if (!r.owner.empty()) {
        if (!transfers.empty() && transfers.back().end == it->first &&
            transfers.back().source == r.owner)
          transfers.back().end = r.end;
        else
          transfers.push_back(Transfer{it->first, r.end, r.owner});
      }
      r.owner = dest;
    }
    // Merge neighbours with identical owners.
    for (auto it = ranges_.begin(); it != ranges_.end();) {
      auto nx = std::next(it);
      if (nx != ranges_.end() && nx->second.owner == it->second.owner) {
        it->second.end = nx->second.end;
        ranges_.erase(nx);
      } else {
        it = nx;
      }
    }
  }
};

// ── EDRAM sample addressing (shaders/edram.xesli XeEdramOffsetBytes) ───────
inline uint32_t sampleOffsetBytes(uint32_t px, uint32_t py, uint32_t baseTiles,
                                  uint32_t pitchTiles, uint32_t msaa, bool isDepth,
                                  uint32_t intsLog2, uint32_t sample) {
  uint32_t sx = (px << (msaa >= kMsaa4X ? 1u : 0u)) + ((sample >> 1) & 1u);
  uint32_t sy = (py << (msaa >= kMsaa2X ? 1u : 0u)) + (sample & 1u);
  const uint32_t tsx = kTileWidthSamples >> intsLog2, tsy = kTileHeightSamples;
  const uint32_t tx = sx / tsx, ty = sy / tsy;
  baseTiles += ty * pitchTiles + tx;
  sx -= tx * tsx;
  sy -= ty * tsy;
  if (isDepth) {
    const uint32_t half = tsx >> 1;
    sx = sx >= half ? sx - half : sx + half;
  }
  uint32_t addr = baseTiles * kTileDwords + ((sy * tsx + sx) << intsLog2);
  addr %= kTileDwords * kTileCount;
  return addr << 2;
}

// Execute a resolve copy from the EDRAM mirror into guest memory.
// destHost points at RB_COPY_DEST_BASE in host memory with destAvail writable
// bytes. Returns the number of destination texels written (0 = nothing).
inline uint32_t resolveToGuest(const uint8_t* edramMirror, const ResolveInfo& ri,
                               uint8_t* destHost, size_t destAvail) {
  if (!ri.copy || ri.destArray) return 0;
  const uint32_t bpt = destBytesPerTexel(ri.destFormat);
  if (!bpt) return 0;
  uint32_t bpbLog2 = 0;
  while ((1u << bpbLog2) < bpt) bpbLog2++;
  const uint32_t pitchTiles = ri.srcKey.pitchTiles();
  const uint32_t intsLog2 = ri.src64 ? 1u : 0u;
  auto rd32 = [&](uint32_t off) { uint32_t v; std::memcpy(&v, edramMirror + off, 4); return v; };
  auto sampleBits = [&](uint32_t x, uint32_t y, uint32_t s) -> uint64_t {
    const uint32_t off = sampleOffsetBytes(x, y, ri.srcBase, pitchTiles, ri.msaa, ri.isDepth, intsLog2, s);
    uint64_t v = rd32(off);
    if (intsLog2) v |= uint64_t(rd32((off + 4) % kEdramBytes)) << 32;
    return v;
  };
  // Samples averaged per sample_select: k0..k3 single, k01 {0,1}, k23 {2,3}, k0123 all.
  uint32_t samples[4], ns = 0;
  switch (ri.sampleSelect) {
    case 4: samples[0] = 0; samples[1] = 1; ns = 2; break;
    case 5: samples[0] = 2; samples[1] = 3; ns = 2; break;
    case 6: samples[0] = 0; samples[1] = 1; samples[2] = 2; samples[3] = 3; ns = 4; break;
    default: samples[0] = ri.sampleSelect; ns = 1; break;
  }
  // Scratch: collect LE texel bytes at their tiled offsets, then endian-swap
  // whole units into guest memory (only fully written units are committed).
  // Tiled offsets are not monotonic in (x, y); every texel of rows < y1 lies
  // inside pitch * align(y1, 32) * bpt (whole macro-tile rows).
  const uint32_t rowsAligned = (ri.y1 + 31u) & ~31u;
  const size_t spanEnd = size_t(ri.destPitch) * rowsAligned * bpt;
  const uint32_t minOff = 0;
  const uint32_t maxOff = uint32_t(std::min(spanEnd, destAvail));
  if (maxOff <= minOff) return 0;
  std::vector<uint8_t> scratch(maxOff - minOff);
  std::vector<uint8_t> written(maxOff - minOff, 0);
  std::atomic<uint32_t> countAll{0};
  // Rows in parallel: every (x, y) has its own tiled offset, so no two
  // iterations write the same scratch bytes.
  par::parallelFor(ri.y0, ri.y1, [&](uint32_t y) {
    uint32_t count = 0;
    uint8_t texel[16];
    for (uint32_t x = ri.x0; x < ri.x1; x++) {
      const int32_t off = xenos_tiling::tiled2D(int32_t(x), int32_t(y), ri.destPitch, bpbLog2);
      if (off < 0 || uint32_t(off) + bpt > maxOff) continue;
      uint32_t nb;
      if (ri.raw || ri.isDepth) {
        const uint64_t v = sampleBits(x, y, samples[0]);
        std::memcpy(texel, &v, 8);
        nb = bpt > 8 ? 8 : bpt;
      } else {
        float acc[4] = {0, 0, 0, 0};
        for (uint32_t i = 0; i < ns; i++) {
          float c[4];
          unpackColor(ri.srcFormat, sampleBits(x, y, samples[i]), c);
          for (int k = 0; k < 4; k++) acc[k] += c[k];
        }
        const float f = ri.expBiasFactor / float(ns);
        for (int k = 0; k < 4; k++) acc[k] *= f;
        if (ri.destSwap) std::swap(acc[0], acc[2]);
        nb = packDest(ri.destFormat, acc, texel);
      }
      std::memcpy(&scratch[uint32_t(off) - minOff], texel, std::min(nb, bpt));
      std::memset(&written[uint32_t(off) - minOff], 1, bpt);
      count++;
    }
    countAll.fetch_add(count, std::memory_order_relaxed);
  });
  const uint32_t count = countAll.load();
  const uint32_t unit = std::max(endianUnitBytes(ri.destEndian), 1u);
  const uint32_t units = uint32_t(scratch.size()) / unit;
  par::parallelFor(0u, (units + 4095u) / 4096u, [&](uint32_t blk) {
    const uint32_t uEnd = std::min(units, (blk + 1u) * 4096u);
    for (uint32_t u = blk * 4096u; u < uEnd; u++) {
      const uint32_t o = u * unit;
      bool full = true;
      for (uint32_t k = 0; k < unit; k++) full = full && written[o + k];
      if (!full) continue;  // never tear a swap unit
      endianSwapUnit(ri.destEndian, &scratch[o]);
      std::memcpy(destHost + minOff + o, &scratch[o], unit);
    }
  }, 1u);
  return count;
}

}  // namespace edram
}  // namespace gfx
}  // namespace sr
