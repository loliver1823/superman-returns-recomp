#include "kernel/resource_reader_observe.hpp"
#include "resource_memory.hpp"
#include "../kernel/sr_oracle.hpp"

#include "cube_layout.hpp"
#include "volume_gates.hpp"
#include "volume_layout.hpp"
#include "../kernel/tex_layout.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sr::gfx::memres {
namespace {
const auto resourceStarted = std::chrono::steady_clock::now();

// step 6 G8: SR_PATCH_LEAN, read once (default on; "0" = every note takes the full path, the A/B's off arm).
bool patchLeanDefault() noexcept {
  static const bool on = [] {
    const char* e = std::getenv("SR_PATCH_LEAN");
    const bool v = !e || e[0] != '0';
    std::fprintf(stderr, "[MEM_RES_NATIVE] SR_PATCH_LEAN=%d\n", v ? 1 : 0);
    return v;
  }();
  return on;
}
// The calling thread's counter shard, kept for its life.
uint32_t patchShard() noexcept {
  static std::atomic<uint32_t> next{0};
  thread_local const uint32_t shard = next.fetch_add(1, std::memory_order_relaxed);
  return shard;
}
}

bool patchLeanOn() noexcept { return patchLeanDefault(); }

ResourceMemory::ResourceMemory()
    : grantWindow_(new std::atomic<uint8_t>[kPages]), cpuWindow_(new uint8_t[kPages]),
      patchBlocks_(new std::atomic<PatchBlock*>[kPages]), patchLean_(patchLeanDefault()) {
  for (uint32_t p = 0; p < kPages; ++p) {
    grantWindow_[p].store(0, std::memory_order_relaxed);
    cpuWindow_[p] = 0;
    patchBlocks_[p].store(nullptr, std::memory_order_relaxed);
  }
  for (auto& word : dirty_) word.store(0, std::memory_order_relaxed);
  for (auto& word : movie_) word.store(0, std::memory_order_relaxed);
  for (auto& word : patchPending_) word.store(0, std::memory_order_relaxed);
}

void ResourceMemory::notePatch(uint32_t va, uint32_t len) noexcept {
  // Virtual-heap addresses are not GPU-addressable through the arena.
  if (va < 0x80000000u || !len || uint64_t(va) + len > 0x100000000ull) {
    patchRefused_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint8_t window = va >= 0xA0000000u ? 0xAu : 8u;
  const uint32_t set = window == 0xAu ? 1u : 0u;
  PatchShard& shard = patchShards_[patchLean_ ? patchShard() % kPatchShards : 0u];  // off: the one shared line
  // step 6 G8 (SR_PATCH_LEAN): one whole dword - every D3D header fence stamp, twice a shader bind - whose bit and page
  // are both still pending needs no write: the pending take will copy it. The writer's guest store must be visible
  // before the two loads (x86 lets a load pass an earlier buffered store): without the fence a take could clear the
  // bits and copy the OLD dword while the store is still buffered, and the skipped note would leave nothing pending.
  // With it, reading the pending bit set orders this note before the take's clear (a seq_cst RMW, takePatch), and the
  // take copies after its clear. Any other state takes the full path below.
  if (patchLean_ && len == 4u && !(va & 3u)) {
    const uint32_t phys = resourcePhysOfVa(va);
    const uint32_t page = phys >> kPageLog2;
    if (page < kPages) {
      if (const PatchBlock* block = patchBlocks_[page].load(std::memory_order_acquire)) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const uint32_t d = (va & 0xFFFu) >> 2;
        if ((patchPending_[page >> 6].load(std::memory_order_acquire) >> (page & 63u) & 1u) &&
            (block->dwords[set][d >> 6].load(std::memory_order_acquire) >> (d & 63u) & 1u)) {
          // A note of one dword (patchStats counts it in both). step 6 G8c: a relaxed load + store, not fetch_add - no
          // locked RMW on the fast path. A shard two threads share (the 17th noting thread) can lose a count: a
          // statistic only, never patch state.
          shard.fast.store(shard.fast.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
          return;
        }
      }
    }
  }
  notePatchSlow(va, len, set, window, shard);
}

void ResourceMemory::notePatchSlow(uint32_t va, uint32_t len, uint32_t set, uint8_t window,
                                   PatchShard& shard) noexcept {
  // Whole dwords: the arena copy is dword-granular, the guest's stores are
  // dword stores at dword-aligned header fields.
  uint32_t vaFirst = va & ~3u;
  const uint64_t vaEnd = (uint64_t(va) + len + 3u) & ~uint64_t(3u);
  uint64_t dwords = 0;
  while (vaFirst < vaEnd) {
    const uint32_t phys = resourcePhysOfVa(vaFirst);
    const uint32_t page = phys >> kPageLog2;
    const uint32_t pageEndVa = (vaFirst | 0xFFFu) + 1u;  // the VA page and the physical page move together
    const uint32_t stop = uint32_t(std::min<uint64_t>(vaEnd, pageEndVa));
    if (page >= kPages) {
      patchRefused_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    PatchBlock* block = patchBlocks_[page].load(std::memory_order_acquire);
    if (!block) {
      PatchBlock* fresh = new (std::nothrow) PatchBlock;
      if (!fresh) {
        // Never silently lose a CPU write: fall back to a page note.
        patchRefused_.fetch_add(1, std::memory_order_relaxed);
        noteCpuWindow(phys & ~0xFFFu, 4096u, Src::kOther, window);
        vaFirst = stop;
        continue;
      }
      if (patchBlocks_[page].compare_exchange_strong(block, fresh, std::memory_order_acq_rel)) block = fresh;
      else delete fresh;  // another thread's block won (block holds it)
    }
    for (uint32_t a = vaFirst; a < stop; a += 4u) {
      const uint32_t d = (a & 0xFFFu) >> 2;
      block->dwords[set][d >> 6].fetch_or(uint64_t{1} << (d & 63u), std::memory_order_release);
      ++dwords;
    }
    // After the dwords: a taker that clears this bit first sees them or leaves
    // them pending for the next take (the bit is set again here).
    const uint64_t bit = uint64_t{1} << (page & 63u);
    if (!(patchPending_[page >> 6].fetch_or(bit, std::memory_order_acq_rel) & bit))
      patchArmed_.fetch_add(1, std::memory_order_acq_rel);
    vaFirst = stop;
  }
  shard.notes.fetch_add(1, std::memory_order_relaxed);
  shard.dwords.fetch_add(dwords, std::memory_order_relaxed);
}

bool ResourceMemory::patchPendingRange(uint32_t first, uint32_t last) const noexcept {
  if (first > last || first >= kPages) return false;
  last = std::min(last, kPages - 1u);
  for (uint32_t word = first >> 6; word <= last >> 6; ++word) {
    uint64_t bits = patchPending_[word].load(std::memory_order_acquire);
    if (!bits) continue;
    const uint32_t lo = word == first >> 6 ? first & 63u : 0u;
    const uint32_t hi = word == last >> 6 ? last & 63u : 63u;
    const uint64_t mask = (hi == 63u ? ~uint64_t{0} : (uint64_t{1} << (hi + 1u)) - 1u) & ~((uint64_t{1} << lo) - 1u);
    if (bits & mask) return true;
  }
  return false;
}

bool ResourceMemory::takePatch(uint32_t page, Patch& out) noexcept {
  if (!patchPending(page)) return false;
  // seq_cst: the clear pairs with notePatch's lean-path fence (the same locked instruction on x86); the copy of the
  // taken dwords follows it.
  patchPending_[page >> 6].fetch_and(~(uint64_t{1} << (page & 63u)), std::memory_order_seq_cst);
  PatchBlock* block = patchBlocks_[page].load(std::memory_order_acquire);
  if (!block) return false;
  for (uint32_t w = 0; w < 2u; ++w)
    for (uint32_t i = 0; i < 16u; ++i) out.dwords[w][i] = block->dwords[w][i].exchange(0, std::memory_order_seq_cst);
  out.page = page;
  const bool any = out.any();
  if (any) patchTaken_.fetch_add(1, std::memory_order_relaxed);
  return any;
}

ResourceMemory::PatchStats ResourceMemory::patchStats() const noexcept {
  PatchStats s;
  for (const PatchShard& shard : patchShards_) {
    const uint64_t fast = shard.fast.load(std::memory_order_relaxed);  // each a note of one dword
    s.notes += shard.notes.load(std::memory_order_relaxed) + fast;
    s.dwords += shard.dwords.load(std::memory_order_relaxed) + fast;
    s.fast += fast;
  }
  s.refused = patchRefused_.load(std::memory_order_relaxed);
  s.taken = patchTaken_.load(std::memory_order_relaxed);
  s.superseded = patchSuperseded_.load(std::memory_order_relaxed);
  return s;
}

uint32_t resourcePhysOfVa(uint32_t va) noexcept {
  return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu);
}

uint32_t ResourceMemory::noteCpu(uint32_t phys, uint32_t len, Src source) noexcept {
  return noteCpuWindow(phys, len, source, 0);
}

uint32_t ResourceMemory::noteCpuWindow(uint32_t phys, uint32_t len, Src source, uint8_t window) noexcept {
  std::lock_guard<std::recursive_mutex> lock(metadataMutex_);
  const uint32_t count = registry_.noteCpuWrite(phys, len, source);
  uint32_t first, last;
  if (!count || !Registry::pageRange(phys, len, first, last)) return count;
  // A whole-command-buffer coherency record retains known inline provenance.
  // Other physical-only writers require the current owner.
  if (window || source != Src::kCmdBuf)
    for (uint32_t page = first; page <= last; ++page) cpuWindow_[page] = window;
  for (uint32_t block = first >> kDirtyBlockLog2; block <= last >> kDirtyBlockLog2; ++block)
    dirty_[block >> 6].fetch_or(uint64_t{1} << (block & 63u), std::memory_order_release);
  return count;
}

void ResourceMemory::supersedePatches(uint32_t phys, uint32_t len) noexcept {
  uint32_t first, last;
  if (!len || !Registry::pageRange(phys, len, first, last)) return;
  const uint64_t end = uint64_t(phys) + len;
  uint64_t dropped = 0;
  for (uint32_t page = first; page <= last; ++page) {
    if (!patchPending(page)) continue;  // a block with no pending bit holds no dword (takePatch drains both)
    PatchBlock* block = patchBlocks_[page].load(std::memory_order_acquire);
    if (!block) continue;
    const uint64_t base = uint64_t(page) << kPageLog2;
    // the dwords the write touches at all: a partly written dword is the GPU's too (its other bytes were the GPU's
    // copy of the CPU's older bytes, or are rewritten by the next whole-page upload)
    const uint32_t d0 = uint32_t((std::max<uint64_t>(phys, base) - base) >> 2);
    const uint32_t d1 = uint32_t((std::min<uint64_t>(end, base + 4096u) - base + 3u) >> 2);  // exclusive
    for (uint32_t w = d0 >> 6; w <= (d1 - 1u) >> 6; ++w) {
      const uint32_t lo = w == d0 >> 6 ? d0 & 63u : 0u;
      const uint32_t hi = w == (d1 - 1u) >> 6 ? (d1 - 1u) & 63u : 63u;
      const uint64_t mask =
          (hi == 63u ? ~uint64_t{0} : (uint64_t{1} << (hi + 1u)) - 1u) & ~((uint64_t{1} << lo) - 1u);
      for (uint32_t set = 0; set < 2u; ++set)
        dropped += uint64_t(std::popcount(block->dwords[set][w].fetch_and(~mask, std::memory_order_acq_rel) & mask));
    }
  }
  if (dropped) patchSuperseded_.fetch_add(dropped, std::memory_order_relaxed);
}

uint32_t ResourceMemory::noteGpu(uint32_t phys, uint32_t len) noexcept {
  std::lock_guard<std::recursive_mutex> lock(metadataMutex_);
  supersedePatches(phys, len);
  return registry_.noteGpuWrite(phys, len);
}

uint32_t ResourceMemory::noteGpuAt(uint32_t phys, uint32_t len, uint32_t generation) noexcept {
  std::lock_guard<std::recursive_mutex> lock(metadataMutex_);
  supersedePatches(phys, len);
  return registry_.noteGpuWriteAt(phys, len, generation);
}

void ResourceMemory::noteCoherency(uint32_t status, uint32_t phys, uint32_t len) noexcept {
#if SR_ORACLE
  sr::kernel::reader_observe::preCoherency(status, phys, len);
#endif
  switch (status) {
    case 0x00010000u: // Shader-constant synchronization carries no resource range.
    case 0x03000100u: // Resolve announces its GPU write; it is never a CPU write.
      return;
    case 0x03000000u: noteCpu(phys, len, Src::kInval); return;
    case 0x02000000u: noteCpu(phys, len, Src::kTexLock); return;
    case 0x01000000u: noteCpu(phys, len, Src::kCmdBuf); return;
    default: noteCpu(phys, len, Src::kOther); return;
  }
}

void ResourceMemory::noteFileRead(uint32_t va, uint32_t len) noexcept {
  if (va >= 0x80000000u)
    noteCpuWindow(resourcePhysOfVa(va), len, Src::kReadFile, va >= 0xA0000000u ? 0xAu : 8u);
}

void ResourceMemory::noteGrant(uint32_t va, uint32_t len) noexcept {
  std::lock_guard<std::recursive_mutex> lock(metadataMutex_);
  uint32_t first, last;
  if (va < 0x80000000u || !Registry::pageRange(resourcePhysOfVa(va), len, first, last)) return;
  const uint8_t window = va >= 0xA0000000u ? 0xAu : 8u;
  for (uint32_t p = first; p <= last; ++p) {
    grantWindow_[p].store(window, std::memory_order_release);
    movie_[p >> 6].fetch_and(~(uint64_t{1} << (p & 63u)), std::memory_order_release);
  }
  // Reusing a physical range invalidates a mirror even when the new resource's
  // first write precedes its first coherency record.
  noteCpu(resourcePhysOfVa(va), len, Src::kOther);
}

void ResourceMemory::noteInlineWrite(uint32_t va, uint32_t len) noexcept {
  if (va >= 0x80000000u)
    noteCpuWindow(resourcePhysOfVa(va), len, Src::kCmdBuf, va >= 0xA0000000u ? 0xAu : 8u);
}

void ResourceMemory::noteTextureUnlock(const uint32_t* fetch, bool moviePlane) noexcept {
  ResourceSpan spans[2];
  const uint32_t count = resourceTextureSpans(fetch, spans);
  for (uint32_t i = 0; i < count; ++i) {
    noteCpu(spans[i].phys, spans[i].len, Src::kUnlock);
    uint32_t first, last;
    if (!Registry::pageRange(spans[i].phys, spans[i].len, first, last)) continue;
    for (uint32_t page = first; page <= last; ++page) {
      const uint64_t bit = uint64_t{1} << (page & 63u);
      if (moviePlane) movie_[page >> 6].fetch_or(bit, std::memory_order_release);
      else movie_[page >> 6].fetch_and(~bit, std::memory_order_release);
    }
  }
}

void ResourceMemory::beginFrame() noexcept {
  for (uint32_t i = 0; i < movie_.size(); ++i) {
    uint64_t bits = movie_[i].load(std::memory_order_acquire);
    while (bits) {
      const uint32_t first = uint32_t(std::countr_zero(bits));
      const uint32_t count = uint32_t(std::countr_one(bits >> first));
      noteCpu((i * 64u + first) << kPageLog2, count << kPageLog2, Src::kUnlock);
      const uint64_t mask = count == 64u ? ~uint64_t{0} : ((uint64_t{1} << count) - 1u) << first;
      bits &= ~mask;
    }
  }
}

uint32_t ResourceMemory::sourceWindow(uint32_t page, int owner) const noexcept {
  std::lock_guard<std::recursive_mutex> lock(metadataMutex_);
  const uint8_t writerWindow = page < kPages ? cpuWindow_[page] : 0;
  if (writerWindow) return writerWindow;
  if (owner == 8 || owner == 0xA) return uint32_t(owner);
  const uint8_t grant = page < kPages ? grantWindow_[page].load(std::memory_order_acquire) : 0;
  return grant ? grant : 8u;
}

ResourceMemory::UploadTicket ResourceMemory::prepare(uint32_t page, int liveOwner) noexcept {
  std::lock_guard<std::recursive_mutex> lock(metadataMutex_);
  if (page >= kPages) return {};
  const uint32_t cpu = registry_.cpuGen(page), gpu = registry_.gpuGen(page);
  return {page, cpu, gpu, Registry::decideGens(cpu, gpu, registry_.uploadedGen(page)), sourceWindow(page, liveOwner)};
}

bool ResourceMemory::commit(const UploadTicket& ticket) noexcept {
  std::lock_guard<std::recursive_mutex> lock(metadataMutex_);
  if (ticket.page >= kPages || !isUpload(ticket.decision)) return false;
  registry_.ensureAt(ticket.page, ticket.cpu, ticket.gpu);
  return true;
}

void ResourceMemory::forgetUpload(uint32_t page) noexcept {
  std::lock_guard<std::recursive_mutex> lock(metadataMutex_);
  registry_.forgetUpload(page);
}

uint32_t ResourceMemory::version(uint32_t phys, uint32_t len) const noexcept {
  return registry_.version(phys, len);
}

void ResourceMemory::takeDirtyBlocks(std::vector<uint32_t>& out) {
  out.clear();
  for (uint32_t i = 0; i < dirty_.size(); ++i) {
    uint64_t bits = dirty_[i].exchange(0, std::memory_order_acq_rel);
    while (bits) {
      out.push_back(i * 64u + uint32_t(std::countr_zero(bits)));
      bits &= bits - 1;
    }
  }
}

#if SR_ORACLE
namespace {
// SR_MEM_RESOURCES, read once: 0 unset / empty (the baked default: native), 1 another value (native, explicit),
// 2 "0" (the page model), 3 "census" (the page model + the shadow census).
int routeForm() noexcept {
  static const int form = [] {
    const char* value = std::getenv("SR_MEM_RESOURCES");
    if (!value || !*value) return 0;
    if (std::strcmp(value, "0") == 0) return 2;
    if (std::strcmp(value, "census") == 0) return 3;
    return 1;
  }();
  return form;
}
}  // namespace
#endif

bool resourceMode() noexcept {
#if !SR_ORACLE
  return true;  // shipping resources have no page-model alternative
#else
  static const bool on = [] {
    const bool enabled = routeForm() <= 1;
    std::fprintf(stderr, "[MEM_RES_NATIVE] route %s form=%s\n", enabled ? "native" : "page_model",
                 resourceRouteForm());
    return enabled;
  }();
  return on;
#endif
}

const char* resourceRouteForm() noexcept {
#if !SR_ORACLE
  return "release";
#else
  static const char* const kForm[] = {"default", "explicit", "off", "census"};
  return kForm[routeForm()];
#endif
}

ResourceMemory& resourceMemory() {
  static ResourceMemory memory;
  return memory;
}

uint64_t resourceElapsedMilliseconds() noexcept {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - resourceStarted).count());
}

namespace {
uint32_t textureSpansUncached(const uint32_t* f, ResourceSpan out[2]) noexcept {
  namespace CL = sr::gfx::cube;
  namespace TL = sr::kernel::texlayout;
  const TL::FetchDesc fd = TL::decodeFetchConstant(f);
  if (!fd.valid || !fd.basePhys || fd.width > 8192u || fd.height > 8192u) return 0;
  const TL::BlockGeom geo = TL::blockGeomFor(fd.fmt);
  const uint32_t bw = geo.blockW ? geo.blockW : 1u;
  uint32_t bb = geo.blockBytes ? geo.blockBytes : 4u;
  // The renderer also accepts these expanded/float formats, beyond the HLE
  // capture helper's narrower format table.
  if (fd.fmt == 26u || fd.fmt == 32u) bb = 8u;
  if (fd.fmt == 27u || fd.fmt == 30u) bb = 2u;
  const uint32_t pitch = std::max(fd.pitchTexels, fd.width);
  uint32_t layers = std::max(fd.depth, 1u);
  if (fd.dim == TL::TexDim::k2DOrStacked) layers = std::max(layers, ((f[2] >> 26) & 0x3Fu) + 1u);
  if (fd.dim == TL::TexDim::k3D) layers = (layers + 3u) & ~3u;
  uint64_t baseLen = uint64_t(CL::detail::levelSliceStride(0, true, fd.width, fd.height, pitch,
                                                        bw, bw, bb, fd.tiled)) * layers;
  const uint32_t mipPage = (f[5] >> 12) & 0xFFFFFu;
  const bool packed = ((f[5] >> 11) & 1u) != 0;
  if (fd.dim == TL::TexDim::k3D && sr::gfx::volume::tex3dV2On()) {
    // SR_TEX3D_V2 (P2-1): what the volume loader reads (D3D12Backend::getOrCreateTexture3D, gfx/volume_layout.hpp):
    // the base to its data EXTENT (beyond the slice stride for a tiled 64^3 k_8_8_8_8) and the mip chain to Xenia's
    // mip_max_level - min(mip_max_level, log2 of the longest axis, depth included), not below mip_min_level
    // (texture_util.cc GetSubresourcesFromFetchConstant; kBaseMap ignored there too: a fetch instruction may
    // override it). The cube layout above clamps by width / height only: a 1x1x64 volume with mip_max 6 had no
    // mip span. Capture and texture-unlock invalidation both read these spans.
    namespace VL = sr::gfx::volume;
    const VL::Spans vs = VL::fetchSpans(fd.width, fd.height, fd.depth, pitch, fd.tiled, bw == 4u ? 2u : 0u, bb,
                                        mipPage != 0u, packed, fd.mipMin, fd.mipMax);
    if (vs.ok) {
      baseLen = std::max<uint64_t>(baseLen, vs.baseBytes);
      const uint64_t mipLen3 = vs.mipBytes;
      uint32_t count = 0;
      out[count++] = {fd.basePhys & 0x1FFFFFFFu, uint32_t(std::min<uint64_t>(baseLen, kPhysBytes))};
      if (mipLen3)
        out[count++] = {(mipPage << 12) & 0x1FFFFFFFu, uint32_t(std::min<uint64_t>(mipLen3, kPhysBytes))};
      return count;
    }
  }
  const uint32_t sizeMax = CL::detail::log2Floor(std::max(fd.width, fd.height));
  const uint32_t mipMax = mipPage ? std::min(fd.mipMax, sizeMax) : 0u;
  uint64_t mipLen = 0;
  if (mipMax > 0u || packed) {
    const CL::CubeLayout layout = CL::computeGuestLayout(fd.fmt, fd.width, fd.height, pitch, fd.tiled,
                                                        mipMax + 1u, packed, bw, bw, bb, layers);
    if (layout.ok) {
      baseLen = std::max<uint64_t>(baseLen, layout.baseSpanBytes);
      if (mipMax > 0u) mipLen = layout.mipsTotalExtentBytes;
    }
    if (mipMax > 0u) {
      const uint32_t from = packed && CL::detail::packedMipLevel(fd.width, fd.height) == 0u ? 0u : 1u;
      uint64_t sum = 0;
      for (uint32_t level = from; level <= mipMax; ++level)
        sum += uint64_t(CL::detail::levelSliceStride(level, false, fd.width, fd.height, pitch,
                                                    bw, bw, bb, fd.tiled)) * layers;
      mipLen = std::max(mipLen, sum);
    }
  }
  uint32_t count = 0;
  out[count++] = {fd.basePhys & 0x1FFFFFFFu, uint32_t(std::min<uint64_t>(baseLen, kPhysBytes))};
  if (mipLen) out[count++] = {(mipPage << 12) & 0x1FFFFFFFu, uint32_t(std::min<uint64_t>(mipLen, kPhysBytes))};
  return count;
}
struct SpanMemo {
  uint32_t key[6] = {};
  uint32_t count = 0;
  ResourceSpan spans[2];
  bool valid = false;
};
}  // namespace

// A pure function of the six fetch dwords, asked for 16 slots on every draw (the guest layout is ~12% of the CP
// recomputed each time: prof2_ng_native): memoised per thread, direct-mapped.
uint32_t resourceTextureSpans(const uint32_t* f, ResourceSpan out[2]) noexcept {
  if (!f) return 0;
  thread_local SpanMemo memo[1024];
  uint32_t h = 0x9E3779B9u;
  for (uint32_t i = 0; i < 6u; ++i) h = (h ^ f[i]) * 0x85EBCA6Bu;
  h ^= h >> 15;
  SpanMemo& e = memo[h & 1023u];
  if (!e.valid || std::memcmp(e.key, f, sizeof e.key) != 0) {
    e.count = textureSpansUncached(f, e.spans);
    std::memcpy(e.key, f, sizeof e.key);
    e.valid = true;
  }
  for (uint32_t i = 0; i < e.count; ++i) out[i] = e.spans[i];
  return e.count;
}

}  // namespace sr::gfx::memres
