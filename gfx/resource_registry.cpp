// mem.resources step 2: the resource registry (gfx/resource_registry.hpp; docs/RELEASE_COMPOSITION.md section 9.2).
#include "gfx/resource_registry.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace sr::gfx::memres {

namespace {
// Raise a generation to g unless a newer note already stands (two notes racing on one page keep the newer).
inline bool raise(std::atomic<uint32_t>& a, uint32_t g) noexcept {
  uint32_t cur = a.load(std::memory_order_relaxed);
  while (cur < g && !a.compare_exchange_weak(cur, g, std::memory_order_relaxed)) {
  }
  return cur < g;
}
}  // namespace

const char* srcName(Src s) noexcept {
  switch (s) {
    case Src::kNone: return "none";
    case Src::kInval: return "inval";
    case Src::kTexLock: return "texlock";
    case Src::kCmdBuf: return "cmdbuf";
    case Src::kOther: return "other";
    case Src::kReadFile: return "readfile";
    case Src::kUnlock: return "unlock";
    case Src::kGpu: return "gpu";
    default: return "?";
  }
}

Registry::Registry()
    : cpu_(new std::atomic<uint32_t>[kPages]),
      gpu_(new std::atomic<uint32_t>[kPages]),
      up_(new std::atomic<uint32_t>[kPages]),
      src_(new std::atomic<uint8_t>[kPages]),
      blockSeq_(new std::atomic<uint64_t>[kPages >> kSeqBlockLog2]()) {
  for (uint32_t b = 0; b < (kPages >> kSeqBlockLog2); ++b) blockSeq_[b].store(0, std::memory_order_relaxed);
  reset();
}

void Registry::reset(uint32_t initialGeneration) noexcept {
  for (uint32_t p = 0; p < kPages; p++) {
    cpu_[p].store(0, std::memory_order_relaxed);
    gpu_[p].store(0, std::memory_order_relaxed);
    up_[p].store(0, std::memory_order_relaxed);
    src_[p].store(uint8_t(Src::kNone), std::memory_order_relaxed);
  }
  clock_.store(std::max(kFirstUseGen, initialGeneration), std::memory_order_relaxed);
  refused_.store(0, std::memory_order_relaxed);
  markChanged(0, kPages - 1u);
}

void Registry::markChanged(uint32_t p0, uint32_t p1) noexcept {
  const uint64_t s = seq_.fetch_add(1, std::memory_order_acq_rel) + 1u;
  for (uint32_t b = p0 >> kSeqBlockLog2; b <= (p1 >> kSeqBlockLog2); ++b) {
    uint64_t cur = blockSeq_[b].load(std::memory_order_relaxed);
    while (cur < s && !blockSeq_[b].compare_exchange_weak(cur, s, std::memory_order_acq_rel)) {
    }
  }
}

bool Registry::pageRange(uint32_t phys, uint32_t len, uint32_t& p0, uint32_t& p1) noexcept {
  if (!len || phys >= kPhysBytes) return false;
  const uint64_t end = std::min<uint64_t>(uint64_t(phys) + len, kPhysBytes);  // exclusive
  p0 = phys >> kPageLog2;
  p1 = uint32_t((end - 1u) >> kPageLog2);
  return true;
}

uint32_t Registry::noteCpuWrite(uint32_t phys, uint32_t len, Src src) noexcept {
  uint32_t p0, p1;
  if (!pageRange(phys, len, p0, p1)) {
    if (len) refused_.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const uint32_t g = reserveGeneration();
  for (uint32_t p = p0; p <= p1; p++) {
    raise(cpu_[p], g);
    src_[p].store(uint8_t(src), std::memory_order_relaxed);
  }
  markChanged(p0, p1);  // after the raises: a reader of the old sequence re-walks
  return p1 - p0 + 1u;
}

uint32_t Registry::noteGpuWrite(uint32_t phys, uint32_t len) noexcept {
  uint32_t first, last;
  if (!pageRange(phys, len, first, last)) {
    if (len) refused_.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  return noteGpuWriteAt(phys, len, reserveGeneration());
}

uint32_t Registry::reserveGeneration() noexcept {
  uint32_t generation = 0;
  if (tryReserveGeneration(generation)) return generation;
  std::fprintf(stderr, "[MEM_RES_NATIVE] GENERATION_EXHAUSTED clock=%u; no ownership state changed\n", clock());
  std::fflush(stderr);
  std::_Exit(3);
}

bool Registry::tryReserveGeneration(uint32_t& generation) noexcept {
  uint32_t current = clock_.load(std::memory_order_relaxed);
  while (current != UINT32_MAX) {
    if (clock_.compare_exchange_weak(current, current + 1u, std::memory_order_relaxed)) {
      generation = current + 1u;
      return true;
    }
  }
  return false;
}

uint32_t Registry::noteGpuWriteAt(uint32_t phys, uint32_t len, uint32_t generation) noexcept {
  uint32_t p0, p1;
  if (!generation || generation > clock() || !pageRange(phys, len, p0, p1)) {
    if (len) refused_.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  for (uint32_t p = p0; p <= p1; p++) {
    // Destination preservation can have captured a CPU write produced after
    // this older resolve. The GPU operation overwrites that preservation: the
    // next CPU-owned draw must upload again. Re-publishing the same generation
    // at execution must not invalidate the copy already queued after it.
    if (raise(gpu_[p], generation) && cpu_[p].load(std::memory_order_relaxed) > generation)
      up_[p].store(0, std::memory_order_relaxed);
  }
  markChanged(p0, p1);
  return p1 - p0 + 1u;
}

Decision Registry::decideGens(uint32_t c, uint32_t g, uint32_t u) noexcept {
  if (g != 0u && g >= c) return Decision::kKeepGpu;
  if (u == 0u) return c == 0u ? Decision::kUploadFirst : Decision::kUploadNoted;  // first ensure: never noted / noted
  if (c > u) return Decision::kUploadNoted;
  return Decision::kKeep;
}

Decision Registry::decide(uint32_t page) const noexcept {
  if (page >= kPages) return Decision::kKeep;
  return decideGens(cpu_[page].load(std::memory_order_relaxed), gpu_[page].load(std::memory_order_relaxed),
                    up_[page].load(std::memory_order_relaxed));
}

Decision Registry::ensureAt(uint32_t page, uint32_t c, uint32_t g) noexcept {
  if (page >= kPages) return Decision::kKeep;
  const Decision d = decideGens(c, g, up_[page].load(std::memory_order_relaxed));
  if (isUpload(d)) raise(up_[page], c > kFirstUseGen ? c : kFirstUseGen);  // the generation the copied bytes hold
  return d;
}

Decision Registry::ensurePage(uint32_t page) noexcept {
  if (page >= kPages) return Decision::kKeep;
  return ensureAt(page, cpu_[page].load(std::memory_order_relaxed), gpu_[page].load(std::memory_order_relaxed));
}

uint32_t Registry::version(uint32_t phys, uint32_t len) const noexcept {
  uint32_t p0, p1;
  if (!pageRange(phys, len, p0, p1)) return 0;
  uint32_t v = 0;
  for (uint32_t p = p0; p <= p1; p++) {
    v = std::max(v, cpu_[p].load(std::memory_order_relaxed));
    v = std::max(v, gpu_[p].load(std::memory_order_relaxed));
  }
  return v;
}

}  // namespace sr::gfx::memres
