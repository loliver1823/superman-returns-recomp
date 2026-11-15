// mem.resources step 2a: the resource registry in shadow beside the page model (gfx/resource_census.hpp;
// docs/RELEASE_COMPOSITION.md section 9.5 item 1). Class oracle: compiled out of the release configuration.
#include "resource_census.hpp"
#include "resource_memory_check.hpp"

#include "cube_layout.hpp"   // computeGuestLayout / levelSliceStride: a texture's real extent (the Lock / Unlock spans)
#include "shader_cache.hpp"  // ShaderCache::hashUcode (XXH3): the IB arm's own content check
#include "shader_mkey.hpp"   // be32 / unpackCfPair / isExecOpcode: memres5's vertex shader fetch analysis
#include "../app/log.hpp"
#include "../app/crash_handler.hpp"  // sr::kVehFirst (step4/relcrash: VEH order in ASan builds)
#include "../kernel/gpu_write_watch.hpp"  // gww::stats(): the model's own checker's counts beside the census
#include "../kernel/guest_context.hpp"    // guestVAToHost: the two windows' bytes (the source-window comparison)
#include "../kernel/phys_window.hpp"      // ownerVersion: a movie mark's allocation
#include "../kernel/quit.hpp"
#include "../kernel/tex_layout.hpp"

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX  // (the targets define it; a standalone parse of this unit too)
#endif
#include <windows.h>
#include <tlhelp32.h>  // the data watch: every thread of the process
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>  // _BitScanForward64 (fetchedClass)
#endif

namespace sr::gfx::memres {

namespace {
constexpr uint64_t kPayloadOverhead = 65536; // control blocks, frozen payload and serialization scratch
struct PayloadEntry { std::array<uint8_t, 4096> bytes; FetchPageVersion version; bool stable; };
struct PayloadSlot { uint32_t offset = 0; bool seen = false, capped = false; };
constexpr uint64_t kPayloadIndex =
    (uint64_t(kPages) * sizeof(PayloadSlot) + alignof(PayloadEntry) - 1) & ~(uint64_t(alignof(PayloadEntry)) - 1);
bool samePayloadVersion(const FetchPageVersion& a, const FetchPageVersion& b) {
  // Epoch advances with frames. Global registry generation can change for an
  // unrelated grant; the full-page mutation version is the relevant identity.
  return a.known && b.known && a.window == b.window && a.cpu == b.cpu && a.gpu == b.gpu &&
      a.uploaded == b.uploaded && a.ownerSum == b.ownerSum && a.ownerSeq == b.ownerSeq;
}
}
struct FetchPayloadStore::Impl {
  mutable std::mutex mutex;
  std::unique_ptr<uint8_t[]> arena;
  uint64_t next = kPayloadIndex, capacity = 0;
  FetchPayloadStats counts;
  explicit Impl(uint64_t budget) {
    if (budget < kPayloadIndex + kPayloadOverhead || budget > (256ull << 20))
      throw std::invalid_argument("fetch payload budget cannot hold its fixed index or exceeds 256 MiB");
    capacity = budget - kPayloadOverhead;
    arena.reset(new uint8_t[size_t(capacity)]);
    // One bounded allocation, no per-page allocator overhead or eviction. The
    // whole arena is charged up front, including presently unused capacity.
    for (uint32_t page = 0; page < kPages; ++page)
      new (arena.get() + size_t(page) * sizeof(PayloadSlot)) PayloadSlot{};
    counts.budget = counts.charged = counts.peak = budget;
  }
  PayloadSlot& slot(uint32_t p) { return reinterpret_cast<PayloadSlot*>(arena.get())[p]; }
  PayloadEntry& entry(uint32_t offset) { return *reinterpret_cast<PayloadEntry*>(arena.get() + offset); }
};
uint64_t FetchPayloadStore::indexBytes() noexcept { return kPayloadIndex + kPayloadOverhead; }
uint64_t FetchPayloadStore::entryBytes() noexcept { return sizeof(PayloadEntry); }
FetchPayloadStore::FetchPayloadStore(uint64_t budget) : impl_(new Impl(budget)) {}
FetchPayloadStore::~FetchPayloadStore() = default;
void FetchPayloadStore::record(uint32_t page, const uint8_t* bytes, const FetchPageVersion& version, bool stable) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if ((page & 4095u) || page > 0x1FFFF000u) { ++impl_->counts.invalid; return; }
  auto& slot = impl_->slot(page >> 12);
  if (!slot.seen) { slot.seen = true; ++impl_->counts.seen; }
  if (!slot.offset && !slot.capped) {
    if (impl_->next + sizeof(PayloadEntry) > impl_->capacity) {
      slot.capped = true; ++impl_->counts.capped;
    } else {
      slot.offset = uint32_t(impl_->next); impl_->next += sizeof(PayloadEntry);
      new (impl_->arena.get() + slot.offset) PayloadEntry{};
      ++impl_->counts.retained;
    }
  }
  if (!slot.offset) return;
  auto& entry = impl_->entry(slot.offset);
  if (bytes) std::memcpy(entry.bytes.data(), bytes, 4096);
  entry.version = version; entry.stable = bytes && stable && version.known;
  if (!entry.stable) ++impl_->counts.invalid;
}
std::shared_ptr<const FetchPagePayload> FetchPayloadStore::capture(uint32_t page, const uint8_t* bytes,
    const FetchPageVersion& version, bool stable, const std::array<uint64_t, 16>& fetched,
    const std::array<uint64_t, 8>& ring, uint64_t last, uint32_t head) {
  auto result = std::make_shared<FetchPagePayload>();
  result->page = page; result->current = version; result->fetched = fetched;
  result->requestRing = ring; result->requestLast = last; result->requestHead = head;
  if (bytes) std::memcpy(result->after.data(), bytes, 4096);
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if ((page & 4095u) || page > 0x1FFFF000u) { result->reason = "invalid_page"; return result; }
  const auto& slot = impl_->slot(page >> 12);
  if (!slot.offset) { result->reason = slot.capped ? "baseline_capped" : "baseline_missing"; return result; }
  const auto& entry = impl_->entry(slot.offset);
  result->before = entry.bytes; result->baseline = entry.version;
  for (uint32_t d = 0; d < 1024; ++d)
    if (std::memcmp(result->before.data() + d * 4, result->after.data() + d * 4, 4))
      result->changed[d >> 6] |= uint64_t(1) << (d & 63);
  result->known = bytes && stable && entry.stable && samePayloadVersion(entry.version, version);
  result->reason = result->known ? "observations_agree_epoch_coverage_unknown" : "baseline_or_current_changed_or_unknown";
  return result;
}
FetchPayloadStats FetchPayloadStore::stats() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->counts;
}
FetchPayloadIntersection intersectFetchPayload(const FetchPagePayload& payload, const FetchAnalysis& analysis) {
  FetchPayloadIntersection out;
  if (!payload.known || !analysis.complete || (payload.page & 4095u) || payload.page > 0x1FFFF000u) {
    out.reason = !payload.known ? payload.reason : !analysis.complete ? analysis.reason : "invalid_page";
    return out;
  }
  if (analysis.uses.size() > 65536) { out.reason = "uses_budget"; return out; }
  // Build a union once. A byte overlap includes its containing dword; this is
  // conservative candidate membership, not proof the entire dword was fetched.
  for (const auto& use : analysis.uses) {
    const uint64_t end = uint64_t(use.phys) + use.len;
    if (!use.len || end > 0x20000000ull) { out = {}; out.reason = "invalid_use_extent"; return out; }
    const uint64_t lo = std::max<uint64_t>(payload.page, use.phys);
    const uint64_t hi = std::min<uint64_t>(uint64_t(payload.page) + 4096, end);
    if (hi <= lo) continue;
    const uint32_t a = uint32_t(lo - payload.page) / 4, b = uint32_t(hi - 1 - payload.page) / 4;
    for (uint32_t word = a >> 6; word <= (b >> 6); ++word) {
      const uint32_t first = word == (a >> 6) ? (a & 63) : 0;
      const uint32_t last = word == (b >> 6) ? (b & 63) : 63;
      out.candidateMask[word] |= (~uint64_t(0) << first) & (~uint64_t(0) >> (63 - last));
    }
  }
  auto be = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
  for (uint32_t d = 0; d < 1024; ++d) {
    const uint64_t bit = uint64_t(1) << (d & 63);
    // Derive from raw bytes too: a missing/tampered bitmap cannot claim absence.
    const bool changed = std::memcmp(payload.before.data() + d * 4, payload.after.data() + d * 4, 4) != 0;
    if (changed != bool(payload.changed[d >> 6] & bit)) {
      out = {}; out.reason = "changed_bitmap_mismatch"; return out;
    }
    if (!changed) continue;
    const bool candidate = (out.candidateMask[d >> 6] & bit) != 0;
    out.changedWords.push_back({payload.page + d * 4, be(payload.before.data() + d * 4),
        be(payload.after.data() + d * 4), bool(payload.fetched[d >> 6] & bit), candidate});
    out.candidateWords += candidate ? 1u : 0u;
  }
  for (uint32_t i = 0; i < 16; ++i) out.candidateMask[i] &= payload.changed[i];
  out.complete = true; out.reason = "sampled_candidate_only_epoch_coverage_unknown";
  return out;
}

namespace {

using A = std::atomic<uint64_t>;

// Every counter in one flat table (reset and snapshot walk it whole). The page decisions are cells.
constexpr uint32_t kCells = kOutcomes * kKinds * kProvs * kSrcs * kSites * kWhys * 2u;
inline uint32_t cellIx(uint32_t o, uint32_t k, uint32_t p, uint32_t s, uint32_t si, uint32_t w, uint32_t m) noexcept {
  return (((((o * kKinds + k) * kProvs + p) * kSrcs + s) * kSites + si) * kWhys + w) * 2u + m;
}
// The missed class's attribute cells (kind x source x cb x seg x rng).
constexpr uint32_t kAttrCells = kKinds * kSrcs * kInCbs * kSegs * kRngs;
inline uint32_t attrIx(uint32_t k, uint32_t s, uint32_t c, uint32_t g, uint32_t r) noexcept {
  return (((k * kSrcs + s) * kInCbs + c) * kSegs + g) * kRngs + r;
}
// memres3: the fetched bytes at 16-byte chunks (256 per page): the fetched mask (4 x 64 bits; memres5: diagnostics only,
// the samples' at= / in_fetch).
constexpr uint32_t kChunkLog2 = 4, kChunks = 4096u >> kChunkLog2, kMaskWords = kChunks / 64u;
// memres4: the fetched dwords (1024 per page: 16 x 64 bits) and per page a ring of the last kRing request ranges that grew
// them (phys << 32 | len << 4 | kind). memres5: the dword mask is the gate's fetched set (a fence word beside fetched
// bytes is not fetched), compared with the registry's per-dword copy.
constexpr uint32_t kDwWords = 1024u / 64u, kRing = 8u;
constexpr uint32_t kGrantMin = 1u << 20;
constexpr uint8_t kTrapArmed = 1, kTrapFired = 2;
constexpr uint8_t kOnMissed = 0, kOnPageOnly = 1, kOnAlias = 2;  // what armed a trap
enum : uint32_t {
  kOffFirst = kCells,
  kOffFirstBy,
  kOffWinHeur = kOffFirstBy + kKinds * kProvs,
  kOffWinAliasSame,
  kOffWinAliasDiff,
  kOffWinUnmapped,
  kOffClobberBoth,
  kOffReuseNoted,
  kOffCov,
  kOffCovReq = kOffCov + kCovs,
  kOffCovValCands,
  kOffCovUnaccounted,
  kOffKeep,
  kOffIbReads = kOffKeep + kFeeds * kChks,
  kOffIbAgreeUp,
  kOffIbAgreeKeep,
  kOffIbMissed,
  kOffIbOver,
  kOffIbFirst,
  kOffIbEvict,
  kOffIbMissedBy,
  kOffNoteCalls = kOffIbMissedBy + kProvs * kSrcs,
  kOffNotePages = kOffNoteCalls + kSrcs,
  kOffNoteWhole = kOffNotePages + kSrcs,
  kOffNoteRefused,
  kOffNoteLowVa,
  kOffNoteUnlockNoFetch,
  kOffNoteLock,
  kOffNoteLockNoFetch,
  kOffNoteUnlockUnmatched,
  kOffMovieMarks,
  kOffLockOpenPages,  // a gauge (added and removed)
  kOffNoteResolve,
  kOffNoteResolvePages,
  kOffNoteShSync,
  kOffCoherRequests,
  kOffCoherOverflow,
  kOffCoherCalls,
  kOffCoherPages = kOffCoherCalls + kCoherSlots,
  kOffMissedAttr = kOffCoherPages + kCoherSlots,
  kOffMissedGrant = kOffMissedAttr + kAttrCells,
  kOffGrantOverflow = kOffMissedGrant + kMaxGrants + 1u,
  kOffUpAllocs,
  kOffUpAllocsZpass,
  kOffTrapArmed,
  kOffTrapFired,
  kOffTrapHost,
  kOffTrapArmFail,
  kOffTrapDeclined,
  kOffFetchBounded,  // memres5: vertex fetch requests marked index-bounded / fallen back to the declared range
  kOffFetchFbShader,
  kOffFetchFbDraw,
  kOffWinLive,       // memres5: the registry's window at an upload, by the rule that named it
  kOffWinGrant,
  kOffWinDefault,
  kOffFetchBoundedComp,  // memres6: requests bounded through a computed index (the world family's two levels)
  kOffBoundSampled,      // memres6: the soundness witness - computed-bound draws evaluated exactly per vertex
  kOffBoundViolations,   //          ... with an exact index outside the bound
  kOffFbReason,          // memres6: the fallbacks by reason (kFbReasons)
  kCounters = kOffFbReason + 16u
};
// memres6: why a vertex fetch request fell back to its declared range (each counted; census_vsfb).
enum : uint8_t {
  kFbSlot = 0,  // the slot not provable by the direct walk (memres5's reasons), or a fetch the walk did not classify
  kFbOp,        // an instruction outside the interpreter's set in the prologue (or a fetch before the index fetch)
  kFbRel,       // relative addressing in the prologue or the index fetch
  kFbCtl,       // predication, a conditional exec or a jump before the index fetch
  kFbFmt,       // the index stream's format not a 32-bit integer one (an L2 slot)
  kFbRead,      // the index stream unreadable in both windows
  kFbMany,      // more than kFamVertsMax vertices in the draw's range
  kFbRange,     // a non-finite value, an index beyond 2^24, an index-stream index outside its declared range
  kFbState,     // the draw's constants / fetch constants not handed to the census
  kFbClob,      // a data fetch's index register clobbered after the index fetch (or a constant index)
  kFbLong,      // a prologue longer than kProlMax instructions
  kFbVs,        // the VS never analysed (no IM_LOAD seen for its hash)
  kFbLit,       // the prologue reads a literal constant the shader corpus defines two ways (which one the host
                // translation baked in is not known)
  kFbReasons
};
static_assert(kFbReasons <= 16, "kOffFbReason's span");
const char* const kFbName[kFbReasons] = {"slot",  "op",    "rel",   "ctl", "fmt",  "read", "many",
                                         "range", "state", "clob",  "long", "vs",  "lit"};
constexpr uint32_t kProlMax = 48;          // prologue instructions interpreted
constexpr uint32_t kFamVertsMax = 65536;   // vertices per draw evaluated
constexpr uint32_t kFamIdxSpan = 64;       // indices one vertex's interval may span
constexpr uint32_t kWitnessFirst = 256;    // the first computed-bound draws all checked exactly, then 1 in kWitnessK
constexpr uint32_t kWitnessK = 64;

struct Span {
  uint32_t phys = 0, len = 0;
};

// memres5: one vertex fetch slot of a vertex shader (noteVsUcode): `safe` = every vfetch of it indexes r0.x directly;
// its stride and the extent of its elements inside a vertex record, in dwords. memres6: or through a computed index -
// its classes (D: the vertex index itself; L1: a register the prologue computed - the world family's vertex index
// modulo the instance count; L2: the element the index-stream fetch F1 read at the L1 index), the L1 register
// component (reg << 2 | comp), the F1 element component of an L2 index, the index conversions among its full fetches
// (bit 0 floor, bit 1 round: is_index_rounded) and, when not safe, the fallback reason.
constexpr uint8_t kClsD = 1, kClsL1 = 2, kClsL2 = 4;
struct VsSlot {
  uint32_t slot = 0, stride = 0;
  int32_t minOff = 0, maxEnd = 0;
  bool safe = false;
  uint8_t cls = kClsD, src = 0, l2c = 0, rnd = 1, fbr = kFbSlot;
};
// Separate from the conservative slot union: retain each instruction's own
// attribute interval and index class for the sampled consumed-fetch diagnostic.
struct VsFetch {
  uint32_t instruction = 0, slot = 0, stride = 0;
  int32_t off = 0;
  uint32_t width = 0;
  uint8_t cls = kClsD, src = 0, l2c = 0, rnd = 1;
  bool safe = false;
};
constexpr uint32_t kVsFetches = 128;
constexpr uint32_t kVsSlots = 16;
constexpr uint32_t kProgSrcs = 8;
constexpr uint32_t kProgLits = 8;
// memres6: the computed-index walk's program (the world family): the ALU instructions before the first fetch (F1),
// F1's slot / stride / offset / index source / conversion, its element format (a 32-bit integer one, or not), and the
// L1 sources the slots read.
struct VsProg {
  bool on = false;      // a prologue and an index fetch: the computed classes apply
  uint8_t reason = 0;   // the walk's fallback reason when it could not (kFb*), 0 = none
  uint32_t n = 0;
  uint32_t ins[kProlMax][3] = {};
  uint32_t f1Slot = 0, f1Stride = 0;
  int32_t f1Off = 0;
  uint8_t f1Src = 0;
  bool f1Rnd = false, f1FmtOk = false, f1Signed = false;
  uint32_t f1Comps = 1;
  uint8_t srcs[kProgSrcs] = {};
  uint32_t nSrcs = 0;
  uint8_t wr[64] = {};  // the register components the prologue writes (an evaluation resets them per vertex)
  // the literal constants the prologue reads (the host translation's values, not the constant file's)
  uint32_t nLit = 0;
  uint32_t litReg[kProgLits] = {};
  uint32_t litV[kProgLits][4] = {};
};
struct VsInfo {
  uint32_t n = 0;
  bool ok = false;  // the ucode walked (else every slot falls back)
  VsSlot s[kVsSlots];
  VsProg prog;
  uint32_t nFetch = 0;
  bool fetchComplete = true;
  VsFetch fetch[kVsFetches];
};

// The native consumer checker must not instantiate the legacy census's page
// tables just to recognize a shader. Fixed slots bound this independent cache;
// a missing/full entry makes the actual consumer analysis UNKNOWN.
struct ConsumerShaderEntry { uint64_t hash = 0; VsInfo info{}; };
struct ConsumerShaders {
  std::mutex mutex;
  std::array<ConsumerShaderEntry, 1024> entries{};
  uint32_t count = 0;
  uint64_t capped = 0;
};
static_assert(sizeof(ConsumerShaders) <= (8u << 20));
ConsumerShaders& consumerShaders() { static ConsumerShaders value; return value; }

// The IB arm's conversion-cache shadow: one entry per IB range (phys, bytes), the bytes' hash and the newest cpuGen
// over the range when they were last converted. Direct-mapped; a collision replaces the entry (counted: ib.evict - the
// next read of the evicted range is a first conversion again, never a miss).
struct IbEnt {
  uint32_t phys, bytes, gen;
  uint64_t hash;
  bool valid;
};
constexpr uint32_t kIbLog2 = 14;

template <class T>
std::unique_ptr<std::atomic<T>[]> table() {
  return std::unique_ptr<std::atomic<T>[]>(new std::atomic<T>[kPages]);
}

struct State {
  Registry reg;
  std::unique_ptr<A[]> n{new A[kCounters]};
  // Per page: the kind and site of the request that last decided it or made it a candidate; the model's epoch stamps
  // the census recorded (the drain's, the render's) and whether that stamp's validation failed; the cpu generation of
  // its last decision; the open guest Lock windows over it; a movie plane's mark (the owner version + 1 at its Unlock).
  std::unique_ptr<std::atomic<uint8_t>[]> kind = table<uint8_t>(), site = table<uint8_t>(),
                                          stampFailed = table<uint8_t>();
  std::unique_ptr<std::atomic<uint16_t>[]> lockOpen = table<uint16_t>();
  // (stampMemo: the drain epoch of the range cache's keep - the model stamps nothing there; only the range memo's reuse
  // relies on it, so it lives apart and never overwrites the stamp a walk set)
  std::unique_ptr<std::atomic<uint32_t>[]> stampColl = table<uint32_t>(), stampRend = table<uint32_t>(),
                                           stampMemo = table<uint32_t>(), lastDecCpu = table<uint32_t>(),
                                           movie = table<uint32_t>();
  std::mutex lockMu;
  std::unordered_map<uint32_t, std::vector<Span>> locks;  // texture header -> the spans its open Lock covers
  std::mutex ibMu;
  std::unique_ptr<IbEnt[]> ib{new IbEnt[1u << kIbLog2]};
  std::atomic<uint32_t> logMissed{0}, logClobber{0}, logAlias{0}, logUnseen{0}, logUnacc{0}, logRace{0}, logIb{0},
      logCoher{0}, logPageOnly{0}, logUnclassified{0};
  // memres2 / memres3. Per page: the last inline-vertex allocation over it (Seg), its trap state, whether the registry's
  // copy is recorded (rhOk) and whether it has diverged from RAM in bytes no request fetched (a page-only decision since
  // the registry's last upload); the fetched chunk mask (kMaskWords per page).
  std::unique_ptr<std::atomic<uint8_t>[]> upTag = table<uint8_t>(), trapSt = table<uint8_t>(), rhOk = table<uint8_t>(),
                                          diverged = table<uint8_t>();
  std::unique_ptr<std::atomic<uint64_t>[]> fmask{new std::atomic<uint64_t>[size_t(kPages) * kMaskWords]};
  // memres5: the registry's copy, a 16-bit hash per dword, in a block allocated at the page's first copy (pages the
  // registry never copies cost nothing); the page's last-grant window (0 none, 8, 0xA).
  std::unique_ptr<std::atomic<std::atomic<uint16_t>*>[]> rhBlk{new std::atomic<std::atomic<uint16_t>*>[kPages]()};
  std::unique_ptr<std::atomic<uint8_t>[]> gWin = table<uint8_t>();
  // memres5: the vertex shaders' fetch analyses (noteVsUcode), by ucode hash.
  std::mutex vsMu;
  std::unordered_map<uint64_t, VsInfo> vs;
  std::atomic<uint32_t> logVsb{0};
  // memres6: the computed-index bounds' memo (the L1 part: per (VS, range, the prologue's constants, F1's constant) -
  // each L1 source's value range and the set of F1 indices; the L2 values are read again every draw), the fallbacks
  // per VS (census_vsfb), the witness's draw count; per page the window of the model's last upload (8 / 0xA, 0 none);
  // the dwords a fallback / a bound that reached the declared end marked (the via= of a sample).
  struct FamMemo {
    double vLo[kProgSrcs], vHi[kProgSrcs];
    std::vector<uint32_t> f1Idx;
    uint8_t reason, l2reason;
  };
  std::mutex famMu;
  std::unordered_map<uint64_t, FamMemo> famMemo;
  std::mutex fbMu;
  std::unordered_map<uint64_t, std::pair<uint64_t, uint8_t>> fbByVs;  // vs -> (requests, the last reason)
  std::atomic<uint64_t> famDraws{0};
  std::atomic<uint32_t> logBoundV{0};
  std::unique_ptr<std::atomic<uint8_t>[]> mWin = table<uint8_t>();
  std::unique_ptr<std::atomic<uint64_t>[]> fmaskFb{new std::atomic<uint64_t>[size_t(kPages) * kDwWords]};
  std::unique_ptr<std::atomic<uint64_t>[]> fmaskCl{new std::atomic<uint64_t>[size_t(kPages) * kDwWords]};
  // memres4's diagnostics: the fetched dwords, the ring of the request ranges that grew them, the newest request.
  std::unique_ptr<std::atomic<uint64_t>[]> fmaskDw{new std::atomic<uint64_t>[size_t(kPages) * kDwWords]};
  std::unique_ptr<std::atomic<uint64_t>[]> reqRing{new std::atomic<uint64_t>[size_t(kPages) * kRing]};
  std::unique_ptr<std::atomic<uint64_t>[]> reqLast{new std::atomic<uint64_t>[kPages]};
  std::unique_ptr<std::atomic<uint8_t>[]> reqHead = table<uint8_t>();
  // mem-native: the owner version the page's fetched marks were made under (0 = none yet), the clears, and the
  // stripes serialising a clear against marking
  std::unique_ptr<std::atomic<uint32_t>[]> fOwner = table<uint32_t>();
  std::atomic<uint64_t> fOwnerClears{0}, missedPrior{0}, vtxStale{0}, missedCpuOnly{0}, cpuOnlyNotes{0};
  // the dwords a named CPU-only writer wrote (noteCpuOnly), under the owner version they were named under
  std::unique_ptr<std::atomic<uint64_t>[]> cpuOnlyDw{new std::atomic<uint64_t>[size_t(kPages) * kDwWords]};
  std::unique_ptr<std::atomic<uint32_t>[]> cpuOnlyOwner = table<uint32_t>();  // the census grant + 1 named under
  std::unique_ptr<std::atomic<uint32_t>[]> cpuOnlyMs = table<uint32_t>();     // the page's last naming (ms)
  // the fetched dwords marked under the page's current owner version (classification only: missed_prior)
  std::unique_ptr<std::atomic<uint64_t>[]> fmaskDwCur{new std::atomic<uint64_t>[size_t(kPages) * kDwWords]};
  std::mutex fOwnerMu[64];
  // mem-native: the exact CPU-write dwords (notePatch) the registry copies alone at the page's next decision - per
  // page a block (16 x 64 dword bits, then the writer's window) allocated at its first patch.
  std::unique_ptr<std::atomic<std::atomic<uint64_t>*>[]> patchBlk{new std::atomic<std::atomic<uint64_t>*>[kPages]()};
  std::atomic<uint64_t> patchNotes{0}, patchDwords{0}, patchRefused{0}, patchApplied{0}, patchAppliedDw{0},
      patchUpload{0}, patchGpu{0}, patchNoCopy{0}, patchRace{0}, vtxExact{0}, vtxRuns{0};
  std::atomic<uint32_t> aliasSeen{0};
  std::atomic<uint32_t> cbLo{0}, cbHi{0};
  std::atomic<uint64_t> coherKey[kCoherSlots];  // status | 1 << 32 (0 = a free slot)
  std::unique_ptr<std::atomic<uint32_t>[]> attrSeen{new std::atomic<uint32_t>[kAttrCells]};  // the per-cell samples
  // The grants (append-only: a slot is published by `ready` after its fields).
  std::atomic<uint32_t> nGrants{0};
  std::atomic<uint32_t> gVa[kMaxGrants], gBytes[kMaxGrants], gLr[kMaxGrants], gPhys[kMaxGrants];
  std::atomic<bool> gReady[kMaxGrants];
  // The traps: a slot per armed page (claimed by nTraps), its writer filled by the fire, printed once.
  std::atomic<uint32_t> nTraps{0}, nonCbMissed{0}, logWriter{0};
  struct Trap {
    std::atomic<uint32_t> page, kind, src, armClock, va, lr, ctr, r1, tid;
    std::atomic<uint64_t> rip, req;
    std::atomic<bool> ready, fired, host, printed, inFetch, dwIn;
    std::atomic<uint8_t> on;
    std::atomic<uint32_t> hdr[8];  // the 32 bytes at va - 8, in memory order
  } trap[kMaxTraps];
  State() { clear(); }
  void clear() {
    for (uint32_t i = 0; i < kCounters; i++) n[i].store(0, std::memory_order_relaxed);
    for (uint32_t p = 0; p < kPages; p++) {
      kind[p].store(0, std::memory_order_relaxed);
      site[p].store(0, std::memory_order_relaxed);
      stampFailed[p].store(0, std::memory_order_relaxed);
      lockOpen[p].store(0, std::memory_order_relaxed);
      stampColl[p].store(0, std::memory_order_relaxed);
      stampRend[p].store(0, std::memory_order_relaxed);
      stampMemo[p].store(0, std::memory_order_relaxed);
      lastDecCpu[p].store(0, std::memory_order_relaxed);
      movie[p].store(0, std::memory_order_relaxed);
      upTag[p].store(0, std::memory_order_relaxed);
      trapSt[p].store(0, std::memory_order_relaxed);
      rhOk[p].store(0, std::memory_order_relaxed);
      diverged[p].store(0, std::memory_order_relaxed);
      gWin[p].store(0, std::memory_order_relaxed);
      delete[] rhBlk[p].exchange(nullptr, std::memory_order_acq_rel);  // (a test's reset; nullptr on construction)
      delete[] patchBlk[p].exchange(nullptr, std::memory_order_acq_rel);
    }
    for (size_t i = 0; i < size_t(kPages) * kMaskWords; i++) fmask[i].store(0, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lk(vsMu);
      vs.clear();
    }
    logVsb.store(0, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lk(famMu);
      famMemo.clear();
    }
    {
      std::lock_guard<std::mutex> lk(fbMu);
      fbByVs.clear();
    }
    famDraws.store(0, std::memory_order_relaxed);
    logBoundV.store(0, std::memory_order_relaxed);
    for (uint32_t p = 0; p < kPages; p++) mWin[p].store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < size_t(kPages) * kDwWords; i++) {
      fmaskDw[i].store(0, std::memory_order_relaxed);
      fmaskDwCur[i].store(0, std::memory_order_relaxed);
      cpuOnlyDw[i].store(0, std::memory_order_relaxed);
      fmaskFb[i].store(0, std::memory_order_relaxed);
      fmaskCl[i].store(0, std::memory_order_relaxed);
    }
    for (size_t i = 0; i < size_t(kPages) * kRing; i++) reqRing[i].store(0, std::memory_order_relaxed);
    for (uint32_t p = 0; p < kPages; p++) {
      reqLast[p].store(0, std::memory_order_relaxed);
      reqHead[p].store(0, std::memory_order_relaxed);
      fOwner[p].store(0, std::memory_order_relaxed);
      cpuOnlyOwner[p].store(0, std::memory_order_relaxed);
      cpuOnlyMs[p].store(0, std::memory_order_relaxed);
    }
    for (auto* c : {&patchNotes, &patchDwords, &patchRefused, &patchApplied, &patchAppliedDw, &patchUpload, &patchGpu,
                     &patchNoCopy, &patchRace, &vtxExact, &vtxRuns, &fOwnerClears, &missedPrior, &vtxStale,
                     &missedCpuOnly, &cpuOnlyNotes})
      c->store(0, std::memory_order_relaxed);
    aliasSeen.store(0, std::memory_order_relaxed);
    cbLo.store(0, std::memory_order_relaxed);
    cbHi.store(0, std::memory_order_relaxed);
    for (auto& k : coherKey) k.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kAttrCells; i++) attrSeen[i].store(0, std::memory_order_relaxed);
    nGrants.store(0, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kMaxGrants; i++) gReady[i].store(false, std::memory_order_relaxed);
    nTraps.store(0, std::memory_order_relaxed);
    nonCbMissed.store(0, std::memory_order_relaxed);
    for (Trap& t : trap) {
      t.ready.store(false, std::memory_order_relaxed);
      t.fired.store(false, std::memory_order_relaxed);
      t.printed.store(false, std::memory_order_relaxed);
    }
    {
      std::lock_guard<std::mutex> lk(lockMu);
      locks.clear();
    }
    for (uint32_t i = 0; i < (1u << kIbLog2); i++) ib[i] = IbEnt{};
    for (auto* c : {&logMissed, &logClobber, &logAlias, &logUnseen, &logUnacc, &logRace, &logIb, &logCoher, &logWriter,
                     &logPageOnly, &logUnclassified})
      c->store(0, std::memory_order_relaxed);
  }
};

// -1 = follow the environment; 0 / 1 = a test's override.
std::atomic<int> g_testOverride{-1};
State* g_state = nullptr;
std::once_flag g_stateOnce;

const uint8_t* hostDefault(uint32_t va) {
  return sr::kernel::isGuestVAInRange(va + 4095u) ? sr::kernel::guestVAToHost(va) : nullptr;
}
uint32_t ownerVerDefault(uint32_t phys) {
  const sr::kernel::physwin::OwnerVer v = sr::kernel::physwin::ownerVersion(phys, 4096u);
  return v.ok ? uint32_t(v.sum) + 1u : 0u;  // 0 = unknown (a note half-written): no mark / an expired one
}
// (named pointer types, '=' initialized: scripts/audit_forcing.py's host-function parser read the brace-initialized
// `std::atomic<uint32_t (*)(uint32_t)> g_ownerVer{...}` as a host function named uint32_t)
std::atomic<TestHooks::HostFn> g_host = hostDefault;
std::atomic<TestHooks::OwnerVerFn> g_ownerVer = ownerVerDefault;
std::atomic<TestHooks::AfterTakeFn> g_afterTake = nullptr;
std::atomic<TestHooks::NowMsFn> g_nowMs = nullptr;
// mem-native: the census's millisecond clock (the cpu_only names' freshness)
uint32_t censusNowMs() noexcept {
  if (const auto f = g_nowMs.load(std::memory_order_relaxed)) return f();
  static const auto t0 = std::chrono::steady_clock::now();
  return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()) +
         1u;
}
constexpr uint32_t kCpuOnlyFreshMs = 2000u;  // a page whose records the walk stopped renaming loses its names
uint32_t ownerVerOf(uint32_t page) noexcept { return g_ownerVer.load(std::memory_order_relaxed)(page << kPageLog2); }

// memres2's traps: a page's 0x8 and 0xA copies write-protected (the arena does not fold the windows - the write watch's
// model, kernel/gpu_write_watch.hpp), the vectored handler installed with the first trap.
std::atomic<uint8_t*> g_guestBase{nullptr};
std::atomic<uint64_t> g_exeBase{0};
std::atomic<int> g_trapLimit{-1};  // a test's limit (-1 = SR_MEM_RES_TRAPS)
int trapLimit() {
  const int t = g_trapLimit.load(std::memory_order_relaxed);
  if (t >= 0) return t;
  static const int v = [] {
    const char* e = std::getenv("SR_MEM_RES_TRAPS");
    const int x = e && *e ? std::atoi(e) : 32;
    return x < 0 ? 0 : x > int(kMaxTraps) ? int(kMaxTraps) : x;
  }();
  return v;
}
#if defined(_WIN32)
LONG WINAPI trapVeh(EXCEPTION_POINTERS* info);
bool protectDefault(uint32_t page, bool readOnly) {
  static std::once_flag once;
  std::call_once(once, [] {
    uint8_t* b = nullptr;
    size_t sz = 0;
    sr::kernel::getGlobalGuestMemory(&b, &sz);
    g_exeBase.store(uint64_t(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))), std::memory_order_relaxed);
    if (b && sz >= 0xC0000000ull && AddVectoredExceptionHandler(sr::kVehFirst, trapVeh)) g_guestBase.store(b);
  });
  uint8_t* b = g_guestBase.load();
  if (!b || page >= kPages) return false;
  // A release of a page the write watch protects is the write watch's (its handler and HostWriteScope release whole
  // blocks and keep their own state): ours would leave it writable while the write watch believes it protected.
  if (!readOnly && sr::kernel::gww::protGen(page) != 0) return true;
  DWORD old = 0;
  const DWORD prot = readOnly ? PAGE_READONLY : PAGE_READWRITE;
  const bool a = VirtualProtect(b + 0x80000000ull + (uint64_t(page) << kPageLog2), 4096, prot, &old) != 0;
  const bool c = VirtualProtect(b + 0xA0000000ull + (uint64_t(page) << kPageLog2), 4096, prot, &old) != 0;
  return a && c;
}
#else
bool protectDefault(uint32_t, bool) { return false; }
#endif
std::atomic<TestHooks::ProtectFn> g_protect = protectDefault;

// memres6: the witness's 1 in K (a test sets 1: every computed-bound draw); a test's narrowing of every computed bound's
// end (the witness must see it: 0 in the census).
std::atomic<uint32_t> g_witnessK{kWitnessK};
std::atomic<uint32_t> g_boundShrink{0};
// memres4: the diagnostic tails of the last MISSED / PAGE_ONLY / ALIAS samples, kept for the tests only (test hooks set).
std::atomic<bool> g_keepSamples{false};
std::mutex g_sampleMu;
std::string g_lastSample[3];
void keepSample(int which, const char* tail) {
  if (!g_keepSamples.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(g_sampleMu);
  g_lastSample[which] = tail;
}

bool envOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_MEM_RESOURCES");
    const bool on = e && std::strcmp(e, "census") == 0;
    if (e && *e)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[MEM_RES] gate SR_MEM_RESOURCES=%s: %s", e,
                  on ? "census - the resource registry in shadow beside the page model (step 2a, observe-only)"
                     : "not 'census': no shadow census ([MEM_RES_NATIVE] route names the path)");
    return on;
  }();
  return v;
}

State& st() {
  std::call_once(g_stateOnce, [] { g_state = new State(); });
  return *g_state;
}

struct PayloadRuntime {
  bool enabled = false;
  const char* reason = "disabled";
  std::unique_ptr<FetchPayloadStore> store;
  std::filesystem::path directory;
  std::atomic<uint32_t> artifact{0}; // 0 absent, 1 complete, 2 failed
  // One copy/read buffer pair, only under g_payloadCopy. No per-caller 8 KiB
  // arrays are reserved on concurrent census threads' stacks.
  std::array<uint8_t, 4096> before{}, after{};
  std::array<uint64_t, 16> fetched{};
  std::array<uint64_t, 8> ring{};
  std::array<uint8_t, 16384> serialized{}; // only the single origin finalizer
};
static_assert(sizeof(PayloadRuntime) + 3 * sizeof(FetchPagePayload) + 4096 < kPayloadOverhead,
              "payload control/snapshot reserve must cover the fixed diagnostic storage");
void payloadReport(const char* phase);
PayloadRuntime& payloadRuntime() {
  // Process lifetime: graceful quit prints after backend teardown and prints
  // twice to detect changing counters. No device/backend pointer is retained.
  static PayloadRuntime runtime;
  static const bool initialized = [&] {
    auto* r = &runtime;
    const char* on = std::getenv("SR_MEM_RES_FETCH_PAYLOAD");
    if (!on || std::strcmp(on, "1")) return true;
    r->enabled = true; r->reason = "invalid_config";
    try {
      sr::kernel::quit::addFamily("resource-fetch-payload", payloadReport);
      uint64_t mb = 128;
      if (const char* text = std::getenv("SR_MEM_RES_FETCH_PAYLOAD_MB")) {
        mb = 0;
        if (!*text) throw std::invalid_argument("empty payload cap");
        for (const char* p = text; *p; ++p) {
          if (*p < '0' || *p > '9' || mb > 256) throw std::invalid_argument("malformed payload cap");
          mb = mb * 10 + uint64_t(*p - '0');
        }
      }
      if (!mb || mb > 256 || !fetchAutoTargetMode()) throw std::invalid_argument("payload requires bounded auto target");
      const char* dir = std::getenv("SR_MEM_RES_FETCH_PAYLOAD_DIR");
      if (!dir || !*dir || std::strlen(dir) > 2048) throw std::invalid_argument("payload directory required");
      r->directory = std::filesystem::u8path(dir);
      if (!r->directory.is_absolute() || !std::filesystem::is_directory(r->directory) ||
          !std::filesystem::is_empty(r->directory)) throw std::invalid_argument("payload directory must be fresh and absolute");
      for (auto path = r->directory; !path.empty(); path = path.parent_path()) {
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
          throw std::invalid_argument("payload directory link");
#if defined(_WIN32)
        const DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT))
          throw std::invalid_argument("payload directory reparse point");
#endif
        if (path == path.parent_path()) break;
      }
      r->store = std::make_unique<FetchPayloadStore>(mb << 20);
      r->reason = "bounded_observation";
    } catch (...) { r->artifact.store(2); }
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[MEM_RES_FETCH] payload_config known=%u reason=%s epoch_coverage=unknown",
        r->store ? 1u : 0u, r->reason);
    return true;
  }();
  (void)initialized;
  return runtime;
}
void payloadReport(const char* phase) {
  auto& r = payloadRuntime();
  if (!r.enabled) return;
  const auto s = r.store ? r.store->stats() : FetchPayloadStats{};
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_FETCH] payload_summary=%s known=%u reason=%s budget=%llu charged=%llu peak=%llu "
      "seen=%llu retained=%llu capped=%llu invalid=%llu artifact=%u epoch_coverage=unknown",
      phase, r.store ? 1u : 0u, r.reason, (unsigned long long)s.budget,
      (unsigned long long)s.charged, (unsigned long long)s.peak, (unsigned long long)s.seen,
      (unsigned long long)s.retained, (unsigned long long)s.capped,
      (unsigned long long)s.invalid, r.artifact.load());
}
std::mutex g_payloadSelection;

// SRFPAY1 wire format v1: all integers little endian, raw guest page bytes are
// unchanged. Header: magic[8], version/u32, length/u32, sequence/u64, target/u32,
// missed/u32, ordinal/u32, page/u32, known/u32, reason_length/u32, reason bytes;
// two versions (baseline/current): ownerSum/u64, grantGeneration/u64, then
// window,cpu,gpu,uploaded,epoch,ownerSeq,known/u32; grantDigest/u64, count/u32,
// serial[2]/u32; last/u64, head/u32, ring[8]/u64, changed[16]/u64, fetched[16]/u64;
// before[4096], after[4096]. A partial file or length mismatch is UNKNOWN.
bool publishPayload(const FetchTargetOrigin& origin) {
  auto& r = payloadRuntime();
  if (!r.store || !origin.payload) return false;
  const auto& p = *origin.payload;
  auto& bytes = r.serialized; size_t used = 0;
  auto raw = [&](const void* data, size_t size) {
    if (used + size > bytes.size()) throw std::length_error("payload wire budget");
    std::memcpy(bytes.data() + used, data, size); used += size;
  };
  auto u32 = [&](uint32_t value) { uint8_t b[4]; for (int i = 0; i < 4; ++i) b[i] = uint8_t(value >> (i * 8)); raw(b, 4); };
  auto u64 = [&](uint64_t value) { uint8_t b[8]; for (int i = 0; i < 8; ++i) b[i] = uint8_t(value >> (i * 8)); raw(b, 8); };
  raw("SRFPAY1\0", 8); u32(1); u32(0); u64(origin.sequence); u32(origin.target);
  u32(origin.missed); u32(origin.ordinal); u32(p.page); u32(p.known ? 1 : 0);
  u32(uint32_t(p.reason.size())); raw(p.reason.data(), p.reason.size());
  for (const auto* v : {&p.baseline, &p.current}) {
    u64(v->ownerSum); u64(v->grantGeneration); u32(v->window); u32(v->cpu); u32(v->gpu);
    u32(v->uploaded); u32(v->epoch); u32(v->ownerSeq); u32(v->known ? 1 : 0);
  }
  u64(p.grantDigest); u32(p.grantCount); for (auto x : p.grantSerial) u32(x);
  u64(p.requestLast); u32(p.requestHead); for (auto x : p.requestRing) u64(x);
  for (auto x : p.changed) u64(x); for (auto x : p.fetched) u64(x);
  raw(p.before.data(), p.before.size()); raw(p.after.data(), p.after.size());
  for (int i = 0; i < 4; ++i) bytes[12 + i] = uint8_t(used >> (i * 8));
  char name[100]; std::snprintf(name, sizeof name, "origin_%llu_%08X.srfp", (unsigned long long)origin.sequence, p.page);
  const auto path = r.directory / name;
  bool good = false;
#if defined(_WIN32)
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    good = WriteFile(file, bytes.data(), DWORD(used), &written, nullptr) && written == used && FlushFileBuffers(file);
    good = CloseHandle(file) && good;
  }
#endif
  r.artifact.store(good ? 1u : 2u);
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[MEM_RES_FETCH] payload_artifact=%s bytes=%llu complete=%u known=%u reason=%s",
      name, (unsigned long long)used, good ? 1u : 0u, p.known ? 1u : 0u, p.reason.c_str());
  return good;
}

thread_local uint8_t t_kind = uint8_t(Kind::kOther);
FetchTargetLatch g_fetchTarget;
std::atomic<uint64_t> g_fetchSequence{0};
thread_local uint64_t t_fetchSequence = 0;
thread_local uint32_t t_fetchOrdinal = 0;
// memres5: the draw whose vertex fetches this thread requests (DrawScope) and the slot of the next request (SlotScope:
// slot + 1, 0 = none).
// memres6: the draw's computed-index bounds, evaluated at its first request that needs them (famCompute): each L1
// source's value range, each F1 element component's stream value range, or the fallback reason.
struct FamDraw {
  bool done = false, ok = false, l2ok = false;
  uint8_t reason = 0, l2reason = 0;
  double vLo[kProgSrcs] = {}, vHi[kProgSrcs] = {};
  double l2Lo[4] = {}, l2Hi[4] = {};
};
struct DrawState {
  bool on = false, range = false, any = false, vsKnown = false;
  uint32_t vMin = 0, vMax = 0;
  // mem-native: an indexed draw's used vertices, bit (v - vMin) (exact = the bitmap holds this draw's)
  bool exact = false;
  std::vector<uint64_t> used;
  uint64_t vsHash = 0;
  const VsInfo* vs = nullptr;  // (the analyses' map nodes never move)
  const uint32_t* consts = nullptr;  // memres6: the draw's VS float constants (c0-c255, 4 dwords each) as uploaded
  const uint32_t* fetch = nullptr;   // memres6: the draw's vertex fetch constants (96 slots, 2 dwords each)
  FamDraw fam;
};
thread_local DrawState t_draw;
thread_local uint32_t t_slot = 0;
thread_local WalkScope* t_scope = nullptr;
thread_local uint32_t t_scopeSeq = 0;
// The generations a validation read before hashing its candidates (snapPage), tagged with the scope's sequence.
struct Snap {
  std::unique_ptr<uint32_t[]> cpu, gpu, seq;
};
thread_local Snap t_snap;
// The model's checks: the last shmWtCheck result (the write watch's accept consumes it; the range cache's loop moves
// it into the range's tally), and the range's tally until its keep.
struct WtChk {
  uint32_t page;
  bool stale, valid;
};
thread_local WtChk t_wt{0, false, false};
struct RcTally {
  uint32_t checked, stale, mismatch;
};
thread_local RcTally t_rc{0, 0, 0};

inline void add(State& s, uint32_t i, uint64_t v = 1) noexcept { s.n[i].fetch_add(v, std::memory_order_relaxed); }
inline uint64_t ld(const State& s, uint32_t i) noexcept { return s.n[i].load(std::memory_order_relaxed); }
inline bool sample(std::atomic<uint32_t>& c, uint32_t& n) noexcept {
  n = c.fetch_add(1, std::memory_order_relaxed) + 1u;
  return n <= 64u || (n & (n - 1u)) == 0u;
}

void tally(State& s, Cov c, uint32_t n = 1) noexcept {
  add(s, kOffCov + uint32_t(c), n);
  if (t_scope) t_scope->count(n);
}

uint32_t physOfVa(uint32_t va) noexcept {  // the allocators' physical address of a 0x8 / 0xA / 0xC / 0xE window VA
  return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu);
}

void noteCpu(State& s, uint32_t phys, uint32_t len, Src src) noexcept {
  const uint32_t n = s.reg.noteCpuWrite(phys, len, src);
  add(s, kOffNoteCalls + uint32_t(src));
  if (n) add(s, kOffNotePages + uint32_t(src), n);
  else if (len) add(s, kOffNoteRefused);
}

// ---- memres2: the missed class's attributes ----
InCb inCb(const State& s, uint32_t page) noexcept {
  const uint32_t lo = s.cbLo.load(std::memory_order_relaxed), hi = s.cbHi.load(std::memory_order_relaxed);
  if (hi <= lo) return InCb::kUnknown;
  const uint64_t a = uint64_t(page) << kPageLog2;
  return (a >= lo && a < hi) ? InCb::kIn : InCb::kOut;
}

void learnCb(State& s, uint32_t lo, uint32_t hi) noexcept {
  if (hi <= lo) return;
  s.cbLo.store(lo & 0x1FFFFFFFu, std::memory_order_relaxed);
  s.cbHi.store(hi > 0x20000000u ? 0x20000000u : hi, std::memory_order_relaxed);
}

// The newest remembered grant holding the page (kMaxGrants = none).
uint32_t grantOf(const State& s, uint32_t page) noexcept {
  const uint32_t n = std::min(s.nGrants.load(std::memory_order_acquire), kMaxGrants);
  const uint64_t a = uint64_t(page) << kPageLog2;
  for (uint32_t i = n; i-- > 0;) {
    if (!s.gReady[i].load(std::memory_order_acquire)) continue;
    const uint64_t lo = s.gPhys[i].load(std::memory_order_relaxed);
    if (a >= lo && a < lo + s.gBytes[i].load(std::memory_order_relaxed)) return i;
  }
  return kMaxGrants;
}

// memres5: THE REGISTRY'S WINDOW for a page - its live owner's (the model's provenance from the allocators' live
// records: 8 / A), else the window of the last grant that covered it (noteGrantWindow; kept after the grant's release),
// else 0x8. *rule: 0 live, 1 grant, 2 default.
bool regIsA(const State& s, uint32_t page, Prov prov, uint32_t* rule) noexcept {
  if (prov == Prov::kA || prov == Prov::k8) {
    if (rule) *rule = 0;
    return prov == Prov::kA;
  }
  const uint8_t g = page < kPages ? s.gWin[page].load(std::memory_order_relaxed) : 0u;
  if (g) {
    if (rule) *rule = 1;
    return g == 0xAu;
  }
  if (rule) *rule = 2;
  return false;
}
// The page's bytes in the window the REGISTRY serves it from.
const uint8_t* regBytes(const State& s, uint32_t page, Prov prov) noexcept {
  return g_host.load(std::memory_order_relaxed)((regIsA(s, page, prov, nullptr) ? 0xA0000000u : 0x80000000u) +
                                                (page << kPageLog2));
}

// OR the bits [b0, b1] into m (64 a word): true when any of them was clear.
bool orBits(std::atomic<uint64_t>* m, uint32_t b0, uint32_t b1) noexcept {
  bool grew = false;
  for (uint32_t w = b0 >> 6; w <= (b1 >> 6); w++) {
    const uint32_t x0 = w == (b0 >> 6) ? (b0 & 63u) : 0u, x1 = w == (b1 >> 6) ? (b1 & 63u) : 63u;
    const uint64_t bits = (x1 == 63u ? ~0ull : ((1ull << (x1 + 1u)) - 1u)) & ~((1ull << x0) - 1u);
    if ((m[w].load(std::memory_order_relaxed) & bits) != bits) {
      m[w].fetch_or(bits, std::memory_order_relaxed);
      grew = true;
    }
  }
  return grew;
}

// memres6: a request's class in the ring (bits 62-63 of an entry) and in the via= of a sample: another kind's request
// (its declared range by nature), a vertex fetch's bound, a bound that reached the declared end, a fallback.
constexpr uint8_t kReqOther = 0, kReqBound = 1, kReqClamp = 2, kReqFall = 3;
inline uint32_t reqPhys(uint64_t e) noexcept { return uint32_t(e >> 32) & 0x1FFFFFFFu; }
inline uint32_t reqLen(uint64_t e) noexcept { return uint32_t((e >> 4) & 0x0FFFFFFFu); }
inline uint8_t reqCls(uint64_t e) noexcept { return uint8_t(e >> 62); }

// memres3: every chunk of [phys, +len) the request fetches joins its page's fetched mask (a union over every request).
// memres4 (diagnostics): its dwords join the dword mask, and a request that grew it is kept in the page's ring.
// memres6: a fallback's dwords and a clamped bound's join their own masks too (via=), the ring entry carries the class.
// mem-native: the owner version (physwin::ownerVersion: the 64 KB pages' mutation counters - grants, command-buffer
// notes, evictions) the page's CURRENT-version marks were made under. A request or a decision that sees another
// version clears only those (fmaskDwCur); the fetched marks themselves are never cleared (Astra round 4: an
// outstanding request's marks must survive to its decision). Classification only: missed_prior.
uint32_t ownerVerOf(uint32_t page) noexcept;
void ownerEpoch(State& s, uint32_t page) noexcept {
  if (page >= kPages) return;
  const uint32_t ov = ownerVerOf(page);
  if (!ov || s.fOwner[page].load(std::memory_order_acquire) == ov) return;
  std::lock_guard<std::mutex> lk(s.fOwnerMu[page & 63u]);
  const uint32_t old = s.fOwner[page].load(std::memory_order_relaxed);
  if (old == ov) return;
  if (old) {
    for (uint32_t w = 0; w < kDwWords; w++) s.fmaskDwCur[size_t(page) * kDwWords + w].store(0, std::memory_order_relaxed);
    s.fOwnerClears.fetch_add(1, std::memory_order_relaxed);
  }
  s.fOwner[page].store(ov, std::memory_order_release);
}
// mem-native diagnostic (MARKF, 24 lines): a mark covering one of SR_MEM_RES_DWATCH's listed dwords, from the first
// request on - its kind, class, range and the draw (no arming needed: the grant is not known yet).
struct MarkTargets {
  uint32_t phys[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  uint32_t n = 0;
};
const MarkTargets& markTargets() {
  static const MarkTargets v = [] {
    MarkTargets t;
    const char* e = std::getenv("SR_MEM_RES_DWATCH");
    if (!e || std::strlen(e) < 8) return t;
    for (const char* p = e; *p && t.n < 4u;) {
      char* end = nullptr;
      const unsigned long x = std::strtoul(p, &end, 16);
      if (end == p) break;
      t.phys[t.n++] = uint32_t(x) & ~3u;
      if (*end != ',' && *end != ';') break;
      p = end + 1;
    }
    return t;
  }();
  return v;
}
std::atomic<uint32_t> g_markF{0};
void markTargetsLog(uint32_t phys, uint32_t len, uint8_t cls) noexcept {
  const MarkTargets& t = markTargets();
  for (uint32_t i = 0; i < t.n; i++) {
    if (t.phys[i] < phys || uint64_t(t.phys[i]) >= uint64_t(phys) + len) continue;
    const uint32_t k = g_markF.fetch_add(1, std::memory_order_relaxed) + 1u;
    if (k > 24u) return;
    const DrawState& d = t_draw;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[MEM_RES] MARKF #%u target=%08X mark=%08X+%X cls=%u kind=%u slot=%d draw=%d vs=%016llX vrange=[%u,%u] "
                "exact=%d known=%d",
                k, t.phys[i], phys, len, unsigned(cls), unsigned(t_kind), int(t_slot) - 1, d.on ? 1 : 0,
                (unsigned long long)d.vsHash, d.vMin, d.vMax, d.exact ? 1 : 0, d.vsKnown ? 1 : 0);
  }
}
void markFetched(State& s, uint32_t phys, uint32_t len, uint8_t cls = kReqOther) noexcept {
  uint32_t p0, p1;
  if (!len || !Registry::pageRange(phys, len, p0, p1)) return;
  if (markTargets().n && g_markF.load(std::memory_order_relaxed) < 24u) markTargetsLog(phys, len, cls);
  const uint64_t end = std::min<uint64_t>(uint64_t(phys) + len, kPhysBytes);
  const uint64_t e = (uint64_t(cls & 3u) << 62) | (uint64_t(phys) << 32) |
                     (uint64_t(std::min<uint32_t>(len, 0x0FFFFFFFu)) << 4) | (t_kind & 0xFu);
  for (uint32_t p = p0; p <= p1; p++) {
    const uint64_t a = uint64_t(p) << kPageLog2;
    const uint32_t lo = uint32_t(std::max<uint64_t>(phys, a) - a), hi = uint32_t(std::min<uint64_t>(end, a + 4096u) - a);
    orBits(s.fmask.get() + size_t(p) * kMaskWords, lo >> kChunkLog2, (hi - 1u) >> kChunkLog2);
    if (cls == kReqFall) orBits(s.fmaskFb.get() + size_t(p) * kDwWords, lo >> 2, (hi - 1u) >> 2);
    if (cls == kReqClamp) orBits(s.fmaskCl.get() + size_t(p) * kDwWords, lo >> 2, (hi - 1u) >> 2);
    orBits(s.fmaskDwCur.get() + size_t(p) * kDwWords, lo >> 2, (hi - 1u) >> 2);  // (classification: missed_prior)
    if (orBits(s.fmaskDw.get() + size_t(p) * kDwWords, lo >> 2, (hi - 1u) >> 2))
      s.reqRing[size_t(p) * kRing + (s.reqHead[p].fetch_add(1, std::memory_order_relaxed) & (kRing - 1u))].store(
          e, std::memory_order_relaxed);
    if (s.reqLast[p].load(std::memory_order_relaxed) != e) s.reqLast[p].store(e, std::memory_order_relaxed);
  }
}
// memres6: the strongest class that marked a dword - 'f' a fallback's declared range, 'c' a bound that reached the
// declared end, 'b' a bound or another kind's request only, '-' none.
char viaOf(const State& s, uint32_t page, uint32_t dw) noexcept {
  if (page >= kPages || dw >= 1024u) return '-';
  const size_t w = size_t(page) * kDwWords + (dw >> 6);
  const uint64_t bit = 1ull << (dw & 63u);
  if (s.fmaskFb[w].load(std::memory_order_relaxed) & bit) return 'f';
  if (s.fmaskCl[w].load(std::memory_order_relaxed) & bit) return 'c';
  if (s.fmaskDw[w].load(std::memory_order_relaxed) & bit) return 'b';
  return '-';
}

bool chunkFetched(const State& s, uint32_t page, uint32_t chunk) noexcept {
  return page < kPages && chunk < kChunks &&
         ((s.fmask[size_t(page) * kMaskWords + (chunk >> 6)].load(std::memory_order_relaxed) >> (chunk & 63u)) & 1u);
}

// ---- memres4's diagnostics (samples and WRITER lines only; no counter reads them) ----
bool dwFetched(const State& s, uint32_t page, uint32_t dw) noexcept {
  return page < kPages && dw < 1024u &&
         ((s.fmaskDw[size_t(page) * kDwWords + (dw >> 6)].load(std::memory_order_relaxed) >> (dw & 63u)) & 1u);
}
// The page's recorded request ranges (its newest, then the ring newest first) overlapping [off, off + n) of the page:
// up to `max`, distinct.
uint32_t coveringReqs(const State& s, uint32_t page, uint32_t off, uint32_t n, uint64_t* out, uint32_t max) noexcept {
  if (page >= kPages) return 0;
  const uint64_t a = (uint64_t(page) << kPageLog2) + off, b = a + n;
  uint32_t k = 0;
  auto take = [&](uint64_t e) {
    if (!e || k >= max) return;
    const uint64_t lo = reqPhys(e), hi = lo + reqLen(e);
    if (lo >= b || hi <= a) return;
    for (uint32_t j = 0; j < k; j++)
      if (out[j] == e) return;
    out[k++] = e;
  };
  take(s.reqLast[page].load(std::memory_order_relaxed));
  const uint32_t h = s.reqHead[page].load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < kRing; i++)
    take(s.reqRing[size_t(page) * kRing + ((h - 1u - i) & (kRing - 1u))].load(std::memory_order_relaxed));
  return k;
}
void fmtReq(uint64_t e, char* b, size_t cap) noexcept {
  if (!e) {
    std::snprintf(b, cap, "-");
    return;
  }
  static const char* const kCls[4] = {"", "/b", "/c", "/f"};
  std::snprintf(b, cap, "%08X+%X:%s%s", reqPhys(e), reqLen(e), kindName(Kind((e & 0xFu) < kKinds ? (e & 0xFu) : 0u)),
                kCls[reqCls(e)]);
}
void fmtReqs(const uint64_t* r, uint32_t k, char* b, size_t cap) noexcept {
  if (!k) {
    std::snprintf(b, cap, "-");
    return;
  }
  size_t o = 0;
  for (uint32_t i = 0; i < k && o + 1 < cap; i++) {
    char one[48];
    fmtReq(r[i], one, sizeof one);
    const int w = std::snprintf(b + o, cap - o, "%s%s", i ? "," : "", one);
    if (w < 0) break;
    o += size_t(w);
  }
}
void hexBytes(const uint8_t* p, uint32_t n, char* b, size_t cap) noexcept {
  static const char kHex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (uint32_t i = 0; i < n && o + 2 < cap; i++) {
    b[o++] = kHex[p[i] >> 4];
    b[o++] = kHex[p[i] & 15u];
  }
  b[o] = 0;
}
// The 32 guest bytes at [va, va + 32) through the census's view of each page (a page the view cannot read: zeros).
void read32(uint32_t va, uint8_t* out) noexcept {
  auto host = g_host.load(std::memory_order_relaxed);
  const uint8_t* pg = nullptr;
  uint32_t cur = 1u;  // (not a page base)
  for (uint32_t i = 0; i < 32u; i++) {
    const uint32_t x = va + i, base = x & ~0xFFFu;
    if (base != cur) {
      pg = host(base);
      cur = base;
    }
    out[i] = pg ? pg[x & 0xFFFu] : 0u;
  }
}
// memres5: the registry's copy is a 16-bit hash per dword (a dword-exact comparison: a fence word beside fetched bytes
// in one 16-byte chunk is not fetched).
inline uint16_t dwHash(const uint8_t* p) noexcept {
  uint32_t v;
  std::memcpy(&v, p, 4);
  v *= 0x9E3779B1u;
  v ^= v >> 15;
  v *= 0x85EBCA77u;
  return uint16_t(v >> 16);
}
inline const std::atomic<uint16_t>* copyOf(const State& s, uint32_t page) noexcept {
  return s.rhOk[page].load(std::memory_order_acquire) ? s.rhBlk[page].load(std::memory_order_acquire) : nullptr;
}
// The first dword of the page that differs from the registry's copy (its byte offset; 0xFFFFFFFF: none, or no copy).
uint32_t firstChanged(const State& s, uint32_t page, const uint8_t* h) noexcept {
  const std::atomic<uint16_t>* c = copyOf(s, page);
  if (!h || !c) return 0xFFFFFFFFu;
  for (uint32_t d = 0; d < 1024u; d++)
    if (dwHash(h + (d << 2)) != c[d].load(std::memory_order_relaxed)) return d << 2;
  return 0xFFFFFFFFu;
}
// A MISSED / PAGE_ONLY sample's tail: the dword the decision turns on (the moved fetched dword, else the first dword
// that differs from the registry's copy), which dwords of its 16-byte chunk any request fetched, the chunk's bytes now,
// the recorded request ranges covering the chunk (newest 4) and the page's newest request.
void sampleTail(const State& s, uint32_t page, const uint8_t* h, uint32_t movedAt, char* b, size_t cap) noexcept {
  char last[48];
  fmtReq(s.reqLast[page].load(std::memory_order_relaxed), last, sizeof last);
  const uint32_t at = movedAt != 0xFFFFFFFFu ? movedAt : firstChanged(s, page, h);
  if (at == 0xFFFFFFFFu || !h) {
    std::snprintf(b, cap, " at=- last=%s", last);
    return;
  }
  const uint32_t ch = at & ~0xFu;  // its 16-byte chunk
  uint32_t mdw = 0;
  for (uint32_t i = 0; i < 4u; i++)
    if (dwFetched(s, page, (ch >> 2) + i)) mdw |= 1u << i;
  char hex[72], reqs[200];
  hexBytes(h + ch, std::min<uint32_t>(32u, 4096u - ch), hex, sizeof hex);
  uint64_t r[4];
  fmtReqs(r, coveringReqs(s, page, ch, 1u << kChunkLog2, r, 4), reqs, sizeof reqs);
  std::snprintf(b, cap, " at=%03X mdw=%X bytes=%s reqs=%s last=%s via=%c", at, mdw, hex, reqs, last,
                viaOf(s, page, at >> 2));
}

std::mutex g_payloadCopy;
FetchPageVersion payloadVersion(const State& s, uint32_t page, Prov prov) {
  FetchPageVersion v;
  v.grantGeneration = sr::kernel::physwin::registryGeneration();
  const auto owner = sr::kernel::physwin::ownerVersion(page << 12, 4096);
  v.ownerSum = owner.sum; v.ownerSeq = owner.cbSeq;
  v.window = regIsA(s, page, prov, nullptr) ? 0xAu : 8u;
  v.cpu = s.reg.cpuGen(page); v.gpu = s.reg.gpuGen(page); v.uploaded = s.reg.uploadedGen(page);
  v.epoch = s.stampColl[page].load(std::memory_order_relaxed);
  v.known = owner.ok && v.grantGeneration == sr::kernel::physwin::registryGeneration();
  return v;
}
bool samePayloadObservation(const FetchPageVersion& a, const FetchPageVersion& b) {
  return samePayloadVersion(a, b) && a.grantGeneration == b.grantGeneration && a.epoch == b.epoch;
}

// Original census hash publication, unchanged when payload observation is off.
void storeCopyHashes(State& s, uint32_t page, const uint8_t* h) noexcept {
  s.diverged[page].store(0, std::memory_order_relaxed);
  if (!h) {
    s.rhOk[page].store(0, std::memory_order_relaxed);
    return;
  }
  std::atomic<uint16_t>* c = s.rhBlk[page].load(std::memory_order_acquire);
  if (!c) {  // the page's first copy: its block
    std::atomic<uint16_t>* fresh = new (std::nothrow) std::atomic<uint16_t>[1024]();
    if (!fresh) {
      s.rhOk[page].store(0, std::memory_order_relaxed);
      return;
    }
    if (s.rhBlk[page].compare_exchange_strong(c, fresh, std::memory_order_acq_rel)) c = fresh;
    else delete[] fresh;  // another thread's block won (c holds it)
  }
  for (uint32_t d = 0; d < 1024u; d++) c[d].store(dwHash(h + (d << 2)), std::memory_order_relaxed);
  s.rhOk[page].store(1, std::memory_order_release);
}
// The original hash loop remains the decision input. The opt-in payload only
// observes that loop, serialized against other payload/hash publications. Two
// byte reads + metadata/hash agreement detect observed races, not hidden ABA.
void storeCopy(State& s, uint32_t page, const uint8_t* h, Prov prov, uint32_t cpu, uint32_t gpu,
               uint32_t previousUploaded) noexcept {
  auto* payload = payloadRuntime().store.get();
  if (!payload) { storeCopyHashes(s, page, h); return; }
  std::lock_guard<std::mutex> observation(g_payloadCopy);
  auto& before = payloadRuntime().before;
  auto& after = payloadRuntime().after;
  before.fill(0); after.fill(0);
  const auto version = payloadVersion(s, page, prov);
  if (h) std::memcpy(before.data(), h, before.size());
  storeCopyHashes(s, page, h);
  if (h) std::memcpy(after.data(), h, after.size());
  const auto end = payloadVersion(s, page, prov);
  const auto* c = copyOf(s, page);
  bool stable = h && c && samePayloadObservation(version, end) && before == after &&
      version.cpu == cpu && version.gpu == gpu &&
      version.uploaded == std::max({previousUploaded, cpu, kFirstUseGen});
  for (uint32_t d = 0; d < 1024 && stable; ++d)
    stable = c[d].load(std::memory_order_relaxed) == dwHash(before.data() + d * 4);
  payload->record(page << 12, h ? before.data() : nullptr, version, stable);
}

std::shared_ptr<const FetchPagePayload> capturePagePayload(State& s, uint32_t page, Prov prov,
                                                         uint32_t movedAt) {
  auto* store = payloadRuntime().store.get();
  if (!store) return {};
  std::lock_guard<std::mutex> observation(g_payloadCopy);
  auto& bytes = payloadRuntime().before; auto& again = payloadRuntime().after;
  auto& fetched = payloadRuntime().fetched; auto& ring = payloadRuntime().ring;
  bytes.fill(0); again.fill(0);
  const auto version = payloadVersion(s, page, prov);
  const uint8_t* h = regBytes(s, page, prov);
  if (h) std::memcpy(bytes.data(), h, bytes.size());
  for (size_t i = 0; i < fetched.size(); ++i) fetched[i] = s.fmaskDw[size_t(page) * kDwWords + i].load();
  for (size_t i = 0; i < ring.size(); ++i) ring[i] = s.reqRing[size_t(page) * kRing + i].load();
  const uint64_t last = s.reqLast[page].load(); const uint32_t head = s.reqHead[page].load();
  if (h) std::memcpy(again.data(), h, again.size());
  const auto end = payloadVersion(s, page, prov);
  bool stable = h && bytes == again && samePayloadObservation(version, end) &&
      regBytes(s, page, prov) == h && s.reqLast[page].load() == last && s.reqHead[page].load() == head;
  for (size_t i = 0; i < fetched.size(); ++i) stable &= fetched[i] == s.fmaskDw[size_t(page) * kDwWords + i].load();
  for (size_t i = 0; i < ring.size(); ++i) stable &= ring[i] == s.reqRing[size_t(page) * kRing + i].load();
  const auto captured = store->capture(page << 12, h ? bytes.data() : nullptr, version, stable, fetched, ring, last, head);
  // Cross-check the immutable bytes against the exact existing hash baseline.
  auto result = std::make_shared<FetchPagePayload>(*captured);
  const auto* hashes = copyOf(s, page);
  bool agrees = hashes != nullptr;
  for (uint32_t d = 0; d < 1024 && agrees; ++d)
    agrees = hashes[d].load() == dwHash(result->before.data() + d * 4);
  const uint32_t d = movedAt / 4;
  agrees = agrees && movedAt < 4096 && (result->changed[d >> 6] & (uint64_t(1) << (d & 63)));
  if (!agrees && result->known) {
    result->known = false; result->reason = "census_baseline_or_selected_change_mismatch";
  }
  return result;
}

// The fetched dwords of a page (every dword any request fetched) against the registry's copy: kSame (none moved),
// kMoved (*moved = the first moved dword's byte offset), kUnknown (no copy, unreadable bytes, or nothing fetched -
// counted gated).
Rng fetchedClass(const State& s, uint32_t page, const uint8_t* h, uint32_t* moved) noexcept {
  const std::atomic<uint16_t>* c = copyOf(s, page);
  if (!h || !c) return Rng::kUnknown;
  bool any = false;
  for (uint32_t w = 0; w < kDwWords; w++) {
    uint64_t m = s.fmaskDw[size_t(page) * kDwWords + w].load(std::memory_order_relaxed);
    while (m) {
      unsigned long bit = 0;
#if defined(_MSC_VER) && !defined(__clang__)
      _BitScanForward64(&bit, m);
#else
      bit = static_cast<unsigned long>(__builtin_ctzll(m));
#endif
      m &= m - 1u;
      const uint32_t d = w * 64u + uint32_t(bit);
      any = true;
      if (dwHash(h + (d << 2)) != c[d].load(std::memory_order_relaxed)) {
        if (moved) *moved = d << 2;
        return Rng::kMoved;
      }
    }
  }
  return any ? Rng::kSame : Rng::kUnknown;
}

// mem-native: fetchedClass over the moved fetched dwords no exact-write note is pending for. The decision's take
// cleared every pending bit before this comparison, so a pending bit now is a note that landed after the take: the
// stamp raced the decision and the next decision's take applies it, as natively (`pended`: such a moved dword exists).
// Each pending bit is read after the dword's bytes: the stamp hooks note before the store (201b29d4), so a store this
// read sees has its note visible to the later load (x86 keeps loads in order and stores in order).
Rng fetchedClassUnpended(const State& s, uint32_t page, const uint8_t* h, uint32_t* moved, bool* pended) noexcept {
  *pended = false;
  const std::atomic<uint16_t>* c = copyOf(s, page);
  if (!h || !c) return Rng::kUnknown;
  const std::atomic<uint64_t>* pb = s.patchBlk[page].load(std::memory_order_acquire);
  bool any = false;
  for (uint32_t w = 0; w < kDwWords; w++) {
    uint64_t m = s.fmaskDw[size_t(page) * kDwWords + w].load(std::memory_order_relaxed);
    while (m) {
      unsigned long bit = 0;
#if defined(_MSC_VER) && !defined(__clang__)
      _BitScanForward64(&bit, m);
#else
      bit = static_cast<unsigned long>(__builtin_ctzll(m));
#endif
      m &= m - 1u;
      const uint32_t d = w * 64u + uint32_t(bit);
      any = true;
      if (dwHash(h + (d << 2)) == c[d].load(std::memory_order_relaxed)) continue;
      const uint64_t pend = pb ? (pb[w].load(std::memory_order_acquire) | pb[kDwWords + w].load(std::memory_order_acquire))
                               : 0u;
      if ((pend >> bit) & 1u) {
        *pended = true;
        continue;
      }
      if (moved) *moved = d << 2;
      return Rng::kMoved;
    }
  }
  return any ? Rng::kSame : Rng::kUnknown;
}

// mem-native (the orchestrator's 2026-10-17 decision): every moved fetched dword (no note pending) was written by the
// named CPU-only writer (noteCpuOnly) - missed_cpu_only. The names hold while the page's census grant (the newest
// >= 1 MiB grant over it: an append-only index, a re-grant is a new one) is the one they were named under and the page
// was named within kCpuOnlyFreshMs (the walk renames the records every draw pass); not through an owner-version bump
// alone (the 64 KB block's counter also moves for command-buffer notes, evictions and other grants). False when none
// moved, one was not so named, no grant holds the page, the grant table overflowed, or the names are stale.
uint32_t cpuOnlyKey(const State& s, uint32_t page) noexcept {
  if (s.nGrants.load(std::memory_order_acquire) >= kMaxGrants) return 0;  // (an overflow: no identity)
  const uint32_t gi = grantOf(s, page);
  return gi < kMaxGrants ? gi + 1u : 0u;
}
bool movedAllCpuOnly(const State& s, uint32_t page, const uint8_t* h) noexcept {
  const std::atomic<uint16_t>* c = copyOf(s, page);
  const uint32_t key = cpuOnlyKey(s, page);
  if (!h || !c || !key || s.cpuOnlyOwner[page].load(std::memory_order_acquire) != key) return false;
  if (censusNowMs() - s.cpuOnlyMs[page].load(std::memory_order_relaxed) > kCpuOnlyFreshMs) return false;
  const std::atomic<uint64_t>* pb = s.patchBlk[page].load(std::memory_order_acquire);
  bool any = false;
  for (uint32_t w = 0; w < kDwWords; w++) {
    uint64_t m = s.fmaskDw[size_t(page) * kDwWords + w].load(std::memory_order_relaxed);
    const uint64_t named = s.cpuOnlyDw[size_t(page) * kDwWords + w].load(std::memory_order_relaxed);
    while (m) {
      const uint32_t bit = uint32_t(__builtin_ctzll(m));
      m &= m - 1u;
      const uint32_t d = w * 64u + bit;
      if (dwHash(h + (d << 2)) == c[d].load(std::memory_order_relaxed)) continue;
      const uint64_t pend = pb ? (pb[w].load(std::memory_order_acquire) | pb[kDwWords + w].load(std::memory_order_acquire))
                               : 0u;
      if ((pend >> bit) & 1u) continue;
      if (!((named >> bit) & 1u)) return false;
      any = true;
    }
  }
  return any;
}
// mem-native: every moved fetched dword (no note pending) lacks a mark made under the page's current owner version -
// only an earlier tenancy's draws fetched what moved (missed_prior). False when none moved or one has a current mark.
bool movedAllPrior(const State& s, uint32_t page, const uint8_t* h) noexcept {
  const std::atomic<uint16_t>* c = copyOf(s, page);
  if (!h || !c || !s.fOwner[page].load(std::memory_order_acquire)) return false;
  const std::atomic<uint64_t>* pb = s.patchBlk[page].load(std::memory_order_acquire);
  bool any = false;
  for (uint32_t w = 0; w < kDwWords; w++) {
    uint64_t m = s.fmaskDw[size_t(page) * kDwWords + w].load(std::memory_order_relaxed);
    const uint64_t cur = s.fmaskDwCur[size_t(page) * kDwWords + w].load(std::memory_order_relaxed);
    while (m) {
      const uint32_t bit = uint32_t(__builtin_ctzll(m));
      m &= m - 1u;
      const uint32_t d = w * 64u + bit;
      if (dwHash(h + (d << 2)) == c[d].load(std::memory_order_relaxed)) continue;
      const uint64_t pend = pb ? (pb[w].load(std::memory_order_acquire) | pb[kDwWords + w].load(std::memory_order_acquire))
                               : 0u;
      if ((pend >> bit) & 1u) continue;
      if ((cur >> bit) & 1u) return false;
      any = true;
    }
  }
  return any;
}

// ---- memres5: index-bounded vertex fetches ----
// A vertex element's size in dwords by its ucode VertexFormat (0 = not one of the known formats: fail-closed).
uint32_t elemDwords(uint32_t fmt) noexcept {
  switch (fmt) {
    // 8_8_8_8, 2_10_10_10, 10_11_11, 11_11_10, 16_16, 16_16_FLOAT, 32, 32_FLOAT
    case 6: case 7: case 16: case 17: case 25: case 31: case 33: case 36: return 1u;
    case 26: case 32: case 34: case 37: return 2u;  // 16_16_16_16, 16_16_16_16_FLOAT, 32_32, 32_32_FLOAT
    case 57: return 3u;                             // 32_32_32_FLOAT
    case 35: case 38: return 4u;                    // 32_32_32_32, 32_32_32_32_FLOAT
    default: return 0u;
  }
}
// A vertex shader's fetch slots (the CF walk of shader_mkey.hpp's vfetchSlots, Xenia's AnalyzeUcode): per slot,
// SAFE when every vfetch of it takes its index from r0.x directly - the full fetch's src_reg 0, not relative, src_swiz
// x; a mini fetch inherits its full fetch's - and r0.x was not written by any instruction the walk met before it (an
// ALU's vector / scalar destination r0 or relative with x in its write mask, a fetch's destination r0 or relative with
// x not kept: the compiler packs other values into r0.yzw), the element's format known and the slot's stride the same
// and nonzero for all its fetches. The walk's order is the program's when control flow only runs forward: a forward
// cond_jmp and the conditional execs skip instructions (a subsequence: fewer r0 writes, never another order), so they
// are followed; a mini fetch whose full fetch may have been skipped - predicated, or in an earlier exec that was
// conditional or behind a jump - would read another full fetch's slot: unsafe. A loop / call / return, a backward jump
// (its target at or before it, or its direction bit set) anywhere, or malformed ucode: nothing is safe (ok = false).
void analyzeVs(const uint8_t* u, size_t bytes, VsInfo& out) noexcept {
  out = VsInfo{};
  const size_t n = bytes / 4, pairs = n / 3;
  if (!u || !pairs) return;
  auto dw = [&](size_t i) { return mkey::be32(u + i * 4); };
  size_t bound = pairs;
  for (size_t i = 0; i < bound; i++) {  // (the bound shrinks while scanning, as Xenia's)
    uint32_t cf[2][2];
    mkey::unpackCfPair(dw(3 * i), dw(3 * i + 1), dw(3 * i + 2), cf);
    for (int j = 0; j < 2; j++)
      if (mkey::isExecOpcode((cf[j][1] >> 12) & 0xFu) && (cf[j][0] & 0xFFFu) < bound) bound = cf[j][0] & 0xFFFu;
  }
  bool r0Written = false, fullSafe = false, sawExec = false, jumped = false, fullPred = false, fullCfSkippable = false;
  uint32_t lastSlot = 0, lastStride = 0, fullCf = 0;
  uint8_t lastRounded = 1;
  for (size_t i = 0; i < bound; i++) {
    uint32_t cf[2][2];
    mkey::unpackCfPair(dw(3 * i), dw(3 * i + 1), dw(3 * i + 2), cf);
    for (int j = 0; j < 2; j++) {
      const uint32_t op = (cf[j][1] >> 12) & 0xFu, cfIdx = uint32_t(2 * i) + uint32_t(j);
      if (op == 11u) {  // cond_jmp (Xenia ControlFlowCondJmpInstruction: address 13 bits, direction bit 33): forward only
        if (((cf[j][1] >> 1) & 1u) || (cf[j][0] & 0x1FFFu) <= cfIdx) return;
        jumped = true;
        continue;
      }
      if (op >= 7u && op <= 10u) return;  // loop_start / loop_end / cond_call / return: fail-closed
      if (!mkey::isExecOpcode(op)) continue;
      sawExec = true;
      const bool condExec = op != 1u && op != 2u;  // cond_exec / cond_exec_pred / cond_exec_pred_clean (+ _end)
      const uint32_t address = cf[j][0] & 0xFFFu, count = (cf[j][0] >> 12) & 0x7u;
      uint32_t seq = (cf[j][0] >> 16) & 0xFFFu;
      for (uint32_t k = 0; k < count; k++, seq >>= 2) {
        const size_t at = size_t(address) + k;
        if (at * 3 + 3 > n) return;  // malformed
        const uint32_t d0 = dw(at * 3), d1 = dw(at * 3 + 1), d2 = dw(at * 3 + 2);
        if (seq & 1u) {  // a fetch
          if ((d0 & 0x1Fu) == 0u) {  // vfetch
            const bool mini = (d1 >> 30) & 1u;
            if (!mini) {  // a full fetch: its index register, slot and stride (a mini inherits them)
              fullSafe = ((d0 >> 5) & 0x3Fu) == 0u && !((d0 >> 11) & 1u) && ((d0 >> 30) & 3u) == 0u;
              lastSlot = ((d0 >> 20) & 0x1Fu) * 3u + ((d0 >> 25) & 0x3u);
              lastStride = d2 & 0xFFu;
              lastRounded = ((d1 >> 15) & 1u) ? 2u : 1u;
              fullCf = cfIdx;
              fullPred = (d1 >> 31) & 1u;           // is_predicated: it may not run
              fullCfSkippable = condExec || jumped;  // its exec may not run (conditional, or a jump before it)
            }
            // a mini reads its full fetch's slot only when that full fetch surely ran: not predicated, and in the
            // mini's own exec or in an earlier exec nothing can skip
            const bool inherits = !mini || (!fullPred && (fullCf == cfIdx || !fullCfSkippable));
            int32_t off = int32_t((d2 >> 8) & 0x7FFFFFu);
            if (off & (1 << 22)) off -= (1 << 23);
            const uint32_t ed = elemDwords((d1 >> 16) & 0x3Fu);
            const bool safe = fullSafe && inherits && !r0Written && ed != 0u && lastStride != 0u;
            if (out.nFetch < kVsFetches)
              out.fetch[out.nFetch++] = VsFetch{uint32_t(at * 3u), lastSlot, lastStride, off, ed,
                                               kClsD, 0, 0, lastRounded, safe};
            else out.fetchComplete = false;
            uint32_t si = 0;
            while (si < out.n && out.s[si].slot != lastSlot) si++;
            if (si == out.n) {
              if (out.n == kVsSlots) return;  // more slots than the table: fail-closed
              out.s[out.n++] = VsSlot{lastSlot, lastStride, off, off + int32_t(ed), safe};
            } else {
              VsSlot& v = out.s[si];
              v.safe = v.safe && safe && v.stride == lastStride;
              v.minOff = std::min(v.minOff, off);
              v.maxEnd = std::max(v.maxEnd, off + int32_t(ed));
            }
          }
          // a fetch writing r0.x (its x destination swizzle not kKeep = 7) or relatively addressed
          const bool writesX = (d1 & 7u) != 7u;
          if (writesX && (((d0 >> 12) & 0x3Fu) == 0u || ((d0 >> 18) & 1u))) r0Written = true;
        } else {  // an ALU instruction (Xenia AluInstruction word 0): x = bit 0 of a write mask
          const bool vRel = (d0 >> 6) & 1u, sRel = (d0 >> 14) & 1u, exportData = (d0 >> 15) & 1u;
          const uint32_t vDst = d0 & 0x3Fu, sDst = (d0 >> 8) & 0x3Fu;
          const uint32_t vMask = (d0 >> 16) & 0xFu, sMask = (d0 >> 20) & 0xFu;
          if (!exportData && (vMask & 1u) && (vDst == 0u || vRel)) r0Written = true;
          if ((sMask & 1u) && (sDst == 0u || sRel)) r0Written = true;  // (an export's scalar too: fail-closed)
        }
      }
    }
  }
  out.ok = sawExec;
}

// ---- memres6: the computed-index bounds (the world family) ----
// The host translation's ALU semantics (XenosRecomp shader_recompiler.cpp recompile(AluInstruction), as
// tools/xenosrecomp patches it; the VS fetch variant's prologue): per instruction the vector result is written first,
// then the scalar `ps` is computed - reading the registers as the vector write left them - and written; r0 starts as
// (the vertex index, 0, 0, 0), every other register and ps as 0. Operands: a vector operand's component for result
// component i is ((swizzle >> 2i) + i) & 3; the scalar ops read src3's ((swizzle >> 6) + 3) & 3 (S0) and swizzle & 3
// (S1); the *sc ops a constant c[src3] (its S0 component) and the temp ((op & 1) | (src3_sel << 1) | (swizzle & 0x3C))
// (its S1 component); a temp operand's bit 7 is abs, a constant's is the instruction's abs_constants; negation after.
struct AluSrc {
  uint32_t reg = 0, swz = 0;
  bool sel = false, neg = false;
};
struct AluDec {
  uint32_t vop = 0, sop = 0, vDst = 0, sDst = 0, vMask = 0, sMask = 0;
  bool vRel = false, sRel = false, exp = false, vSat = false, sSat = false, absC = false, pred = false;
  bool c0Rel = false, c1Rel = false;
  AluSrc s1, s2, s3;
};
AluDec decodeAlu(uint32_t d0, uint32_t d1, uint32_t d2) noexcept {
  AluDec a;
  a.vDst = d0 & 0x3Fu;
  a.vRel = (d0 >> 6) & 1u;
  a.absC = (d0 >> 7) & 1u;
  a.sDst = (d0 >> 8) & 0x3Fu;
  a.sRel = (d0 >> 14) & 1u;
  a.exp = (d0 >> 15) & 1u;
  a.vMask = (d0 >> 16) & 0xFu;
  a.sMask = (d0 >> 20) & 0xFu;
  a.vSat = (d0 >> 24) & 1u;
  a.sSat = (d0 >> 25) & 1u;
  a.sop = (d0 >> 26) & 0x3Fu;
  a.s3.swz = d1 & 0xFFu;
  a.s2.swz = (d1 >> 8) & 0xFFu;
  a.s1.swz = (d1 >> 16) & 0xFFu;
  a.s3.neg = (d1 >> 24) & 1u;
  a.s2.neg = (d1 >> 25) & 1u;
  a.s1.neg = (d1 >> 26) & 1u;
  a.pred = (d1 >> 28) & 1u;
  a.c1Rel = (d1 >> 30) & 1u;
  a.c0Rel = (d1 >> 31) & 1u;
  a.s3.reg = d2 & 0xFFu;
  a.s2.reg = (d2 >> 8) & 0xFFu;
  a.s1.reg = (d2 >> 16) & 0xFFu;
  a.vop = (d2 >> 24) & 0x1Fu;
  a.s3.sel = (d2 >> 29) & 1u;
  a.s2.sel = (d2 >> 30) & 1u;
  a.s1.sel = (d2 >> 31) & 1u;
  return a;
}
// The interpreted set: vector add / mul / max / min / frc / trunc / floor / mad (any op with no effect when it writes
// nothing); scalar adds / adds_prev / muls / muls_prev(2) / maxs / mins / frcs / truncs / floors / rcp(c/f) / maxa(s/f)
// (a0: harmless without relative addressing) / subs / subs_prev / mul-add-sub sc / retain_prev.
bool aluSupported(const AluDec& a) noexcept {
  const bool vOk = a.vMask ? (a.vop <= 3u || (a.vop >= 8u && a.vop <= 11u)) : (a.vop <= 19u || a.vop == 28u);
  const uint32_t o = a.sop;
  const bool sOk = o <= 6u || (o >= 11u && o <= 13u) || (o >= 17u && o <= 19u) || (o >= 23u && o <= 26u) ||
                   (o >= 42u && o <= 47u) || o == 50u;
  return vOk && sOk;
}
inline uint32_t vComp(uint32_t swz, uint32_t i) noexcept { return ((swz >> (2u * i)) + i) & 3u; }
inline uint32_t sComp0(uint32_t swz) noexcept { return ((swz >> 6) + 3u) & 3u; }
inline uint32_t sComp1(uint32_t swz) noexcept { return swz & 3u; }
inline uint32_t scTemp(const AluDec& a) noexcept {
  return (a.sop & 1u) | (uint32_t(a.s3.sel) << 1) | (a.s3.swz & 0x3Cu);
}
inline float constF(const uint32_t* c, uint32_t reg, uint32_t comp) noexcept {
  float f;
  std::memcpy(&f, c + (reg & 0xFFu) * 4u + comp, 4);
  return f;
}
inline int32_t sext6(uint32_t v) noexcept { return (v & 0x20u) ? int32_t(v) - 64 : int32_t(v); }

// (1) EXACT, scalar float32 - the witness's evaluation, the host translation's arithmetic in order (an unfused mad;
// rcp as clamp(1 / x, FLT_MIN, FLT_MAX)). Separate code from the interval evaluation it checks.
struct XRegs {
  float r[64][4];
  float ps;
};
inline float xClamp01(float x) noexcept { return x > 1.0f ? 1.0f : x >= 0.0f ? x : 0.0f; }
float xSrcVal(const XRegs& R, const uint32_t* c, const AluSrc& s, uint32_t comp, bool absC) noexcept {
  float v;
  bool ab;
  if (s.sel) {
    v = R.r[s.reg & 0x3Fu][comp];
    ab = (s.reg & 0x80u) != 0;
  } else {
    v = constF(c, s.reg, comp);
    ab = absC;
  }
  if (ab) v = std::fabs(v);
  return s.neg ? -v : v;
}
float xRcp(float x) noexcept {
  const float r = 1.0f / x;
  if (r != r) return r;
  return r < FLT_MIN ? FLT_MIN : r > FLT_MAX ? FLT_MAX : r;
}
void xStep(XRegs& R, const uint32_t* c, const AluDec& a) noexcept {
  if (a.vMask && !a.exp) {
    float res[4] = {};
    for (uint32_t i = 0; i < 4; i++) {
      if (!((a.vMask >> i) & 1u)) continue;
      const float x = xSrcVal(R, c, a.s1, vComp(a.s1.swz, i), a.absC);
      const float y = xSrcVal(R, c, a.s2, vComp(a.s2.swz, i), a.absC);
      const float z = xSrcVal(R, c, a.s3, vComp(a.s3.swz, i), a.absC);
      float r = 0.0f;
      switch (a.vop) {
        case 0: r = x + y; break;
        case 1: r = x * y; break;
        case 2: r = std::fmax(x, y); break;
        case 3: r = std::fmin(x, y); break;
        case 8: r = x - std::floor(x); break;
        case 9: r = std::trunc(x); break;
        case 10: r = std::floor(x); break;
        case 11: {
          const float t = x * y;  // (two statements: never contracted into an fma)
          r = t + z;
          break;
        }
        default: break;
      }
      res[i] = a.vSat ? xClamp01(r) : r;
    }
    for (uint32_t i = 0; i < 4; i++)
      if ((a.vMask >> i) & 1u) R.r[a.vDst][i] = res[i];
  }
  if (a.sop != 50u) {
    const float s0 = xSrcVal(R, c, a.s3, sComp0(a.s3.swz), a.absC);
    const float s1 = xSrcVal(R, c, a.s3, sComp1(a.s3.swz), a.absC);
    float p = R.ps;
    switch (a.sop) {
      case 0: p = s0 + s1; break;
      case 1: p = s0 + R.ps; break;
      case 2: p = s0 * s1; break;
      case 3: case 4: p = s0 * R.ps; break;
      case 5: case 23: case 24: p = std::fmax(s0, s1); break;
      case 6: p = std::fmin(s0, s1); break;
      case 11: p = s0 - std::floor(s0); break;
      case 12: p = std::trunc(s0); break;
      case 13: p = std::floor(s0); break;
      case 17: case 18: case 19: p = xRcp(s0); break;
      case 25: p = s0 - s1; break;
      case 26: p = s0 - R.ps; break;
      case 42: case 43: case 44: case 45: case 46: case 47: {
        float k = constF(c, a.s3.reg, sComp0(a.s3.swz)), t = R.r[scTemp(a) & 0x3Fu][sComp1(a.s3.swz)];
        if (a.absC) {
          k = std::fabs(k);
          t = std::fabs(t);
        }
        if (a.s3.neg) {
          k = -k;
          t = -t;
        }
        p = a.sop <= 43u ? k * t : a.sop <= 45u ? k + t : k - t;
        break;
      }
      default: break;
    }
    R.ps = a.sSat ? xClamp01(p) : p;
  }
  if (a.sMask && !a.exp)
    for (uint32_t i = 0; i < 4; i++)
      if ((a.sMask >> i) & 1u) R.r[a.sDst][i] = R.ps;
}
// The host's index conversion (superman.patch XeVfAddr): int(rounded ? floor(index + 0.5) : floor(index)).
inline bool xIndex(float x, bool rnd, int64_t& i) noexcept {
  const float t = rnd ? x + 0.5f : x;
  if (!(std::fabs(t) <= 16777216.0f)) return false;
  i = int64_t(std::floor(t));
  return true;
}

// (2) INTERVALS - the bound. Every inexact step widens outward by 2^-20 relative (the GPU's rcp and fused-mad
// differences are a few ulps of 2^-24) plus FLT_MIN; a step on integer endpoints whose result stays an integer within
// 2^24 is exact. floor / trunc of an interval take both sides of an integer it straddles.
struct Iv {
  double lo, hi;
};
constexpr double kIvRel = 1.0 / 1048576.0, kIvAbs = 1.1754943508222875e-38, kIvInt = 16777216.0;
inline bool ivIsInt(const Iv& x) noexcept {
  return x.lo == std::floor(x.lo) && x.hi == std::floor(x.hi) && std::fabs(x.lo) <= kIvInt &&
         std::fabs(x.hi) <= kIvInt;
}
inline bool ivIsZero(const Iv& x) noexcept { return x.lo == 0.0 && x.hi == 0.0; }
inline Iv ivWiden(Iv x) noexcept {
  x.lo -= std::fabs(x.lo) * kIvRel + kIvAbs;
  x.hi += std::fabs(x.hi) * kIvRel + kIvAbs;
  return x;
}
inline bool ivFinite(const Iv& x) noexcept { return std::fabs(x.lo) <= double(FLT_MAX) && std::fabs(x.hi) <= double(FLT_MAX); }
Iv ivAdd(Iv a, Iv b) noexcept {
  if (ivIsZero(a)) return b;
  if (ivIsZero(b)) return a;
  const Iv r{a.lo + b.lo, a.hi + b.hi};
  return ivIsInt(a) && ivIsInt(b) && ivIsInt(r) ? r : ivWiden(r);
}
Iv ivMul(Iv a, Iv b) noexcept {
  if (ivIsZero(a) || ivIsZero(b)) return Iv{0.0, 0.0};
  if (a.lo == 1.0 && a.hi == 1.0) return b;
  if (b.lo == 1.0 && b.hi == 1.0) return a;
  const double p0 = a.lo * b.lo, p1 = a.lo * b.hi, p2 = a.hi * b.lo, p3 = a.hi * b.hi;
  const Iv r{std::min(std::min(p0, p1), std::min(p2, p3)), std::max(std::max(p0, p1), std::max(p2, p3))};
  return ivIsInt(a) && ivIsInt(b) && ivIsInt(r) ? r : ivWiden(r);
}
inline Iv ivMax(Iv a, Iv b) noexcept { return Iv{std::max(a.lo, b.lo), std::max(a.hi, b.hi)}; }
inline Iv ivMin(Iv a, Iv b) noexcept { return Iv{std::min(a.lo, b.lo), std::min(a.hi, b.hi)}; }
inline Iv ivFloor(Iv a) noexcept { return Iv{std::floor(a.lo), std::floor(a.hi)}; }
inline Iv ivTrunc(Iv a) noexcept { return Iv{std::trunc(a.lo), std::trunc(a.hi)}; }
Iv ivFrc(Iv a) noexcept {
  const double f = std::floor(a.lo);
  if (std::floor(a.hi) != f) return Iv{0.0, 1.0};
  Iv r{a.lo - f, a.hi - f};
  if (!ivIsInt(a)) r = ivWiden(r);
  return Iv{std::max(r.lo, 0.0), std::min(r.hi, 1.0)};
}
// clamp(rcp(x), FLT_MIN, FLT_MAX): a negative x (or -0) gives FLT_MIN, +0 FLT_MAX.
Iv ivRcp(Iv a) noexcept {
  const double fmin = double(FLT_MIN), fmax = double(FLT_MAX);
  if (a.lo > 0.0) {
    const Iv r = ivWiden(Iv{1.0 / a.hi, 1.0 / a.lo});
    return Iv{std::min(std::max(r.lo, fmin), fmax), std::min(std::max(r.hi, fmin), fmax)};
  }
  if (a.hi < 0.0) return Iv{fmin, fmin};
  return Iv{fmin, fmax};  // the interval holds a zero: +0, -0 or small numbers of either sign
}
inline Iv ivNeg(Iv a) noexcept { return Iv{-a.hi, -a.lo}; }
inline Iv ivAbs(Iv a) noexcept {
  if (a.lo >= 0.0) return a;
  if (a.hi <= 0.0) return Iv{-a.hi, -a.lo};
  return Iv{0.0, std::max(-a.lo, a.hi)};
}
inline Iv ivSat(Iv a) noexcept {
  return Iv{std::min(std::max(a.lo, 0.0), 1.0), std::min(std::max(a.hi, 0.0), 1.0)};
}
struct IRegs {
  Iv r[64][4];
  Iv ps;
};
Iv ivSrcVal(const IRegs& R, const uint32_t* c, const AluSrc& s, uint32_t comp, bool absC) noexcept {
  Iv v;
  bool ab;
  if (s.sel) {
    v = R.r[s.reg & 0x3Fu][comp];
    ab = (s.reg & 0x80u) != 0;
  } else {
    const double k = constF(c, s.reg, comp);
    v = Iv{k, k};
    ab = absC;
  }
  if (ab) v = ivAbs(v);
  return s.neg ? ivNeg(v) : v;
}
// One instruction in intervals; false = a non-finite value (or NaN) appeared.
bool ivStep(IRegs& R, const uint32_t* c, const AluDec& a) noexcept {
  if (a.vMask && !a.exp) {
    Iv res[4] = {};
    for (uint32_t i = 0; i < 4; i++) {
      if (!((a.vMask >> i) & 1u)) continue;
      const Iv x = ivSrcVal(R, c, a.s1, vComp(a.s1.swz, i), a.absC);
      const Iv y = ivSrcVal(R, c, a.s2, vComp(a.s2.swz, i), a.absC);
      const Iv z = ivSrcVal(R, c, a.s3, vComp(a.s3.swz, i), a.absC);
      Iv r{0.0, 0.0};
      switch (a.vop) {
        case 0: r = ivAdd(x, y); break;
        case 1: r = ivMul(x, y); break;
        case 2: r = ivMax(x, y); break;
        case 3: r = ivMin(x, y); break;
        case 8: r = ivFrc(x); break;
        case 9: r = ivTrunc(x); break;
        case 10: r = ivFloor(x); break;
        case 11: r = ivAdd(ivMul(x, y), z); break;  // two roundings widened: covers a fused mad too
        default: break;
      }
      if (a.vSat) r = ivSat(r);
      if (!ivFinite(r) || r.lo != r.lo || r.hi != r.hi) return false;
      res[i] = r;
    }
    for (uint32_t i = 0; i < 4; i++)
      if ((a.vMask >> i) & 1u) R.r[a.vDst][i] = res[i];
  }
  if (a.sop != 50u) {
    const Iv s0 = ivSrcVal(R, c, a.s3, sComp0(a.s3.swz), a.absC);
    const Iv s1 = ivSrcVal(R, c, a.s3, sComp1(a.s3.swz), a.absC);
    Iv p = R.ps;
    switch (a.sop) {
      case 0: p = ivAdd(s0, s1); break;
      case 1: p = ivAdd(s0, R.ps); break;
      case 2: p = ivMul(s0, s1); break;
      case 3: case 4: p = ivMul(s0, R.ps); break;
      case 5: case 23: case 24: p = ivMax(s0, s1); break;
      case 6: p = ivMin(s0, s1); break;
      case 11: p = ivFrc(s0); break;
      case 12: p = ivTrunc(s0); break;
      case 13: p = ivFloor(s0); break;
      case 17: case 18: case 19: p = ivRcp(s0); break;
      case 25: p = ivAdd(s0, ivNeg(s1)); break;
      case 26: p = ivAdd(s0, ivNeg(R.ps)); break;
      case 42: case 43: case 44: case 45: case 46: case 47: {
        const double kc = constF(c, a.s3.reg, sComp0(a.s3.swz));
        Iv k{kc, kc}, t = R.r[scTemp(a) & 0x3Fu][sComp1(a.s3.swz)];
        if (a.absC) {
          k = ivAbs(k);
          t = ivAbs(t);
        }
        if (a.s3.neg) {
          k = ivNeg(k);
          t = ivNeg(t);
        }
        p = a.sop <= 43u ? ivMul(k, t) : a.sop <= 45u ? ivAdd(k, t) : ivAdd(k, ivNeg(t));
        break;
      }
      default: break;
    }
    if (a.sSat) p = ivSat(p);
    if (!ivFinite(p) || p.lo != p.lo || p.hi != p.hi) return false;
    R.ps = p;
  }
  if (a.sMask && !a.exp)
    for (uint32_t i = 0; i < 4; i++)
      if ((a.sMask >> i) & 1u) R.r[a.sDst][i] = R.ps;
  return true;
}
// An interval value's integer index range under the host's conversion; false beyond 2^24.
bool ivIndex(Iv x, bool rnd, int64_t& lo, int64_t& hi) noexcept {
  if (rnd) x = ivAdd(x, Iv{0.5, 0.5});
  if (!(std::fabs(x.lo) <= kIvInt) || !(std::fabs(x.hi) <= kIvInt)) return false;
  lo = int64_t(std::floor(x.lo));
  hi = int64_t(std::floor(x.hi));
  return true;
}

// The computed-index walk (after analyzeVs, whose slot table it refines): when the program's first fetch is a full
// vfetch whose index is not the untouched r0.x, the ALU instructions before it are the PROLOGUE (each in the
// interpreted set, unpredicated, in an unconditional exec, no relative addressing, no jump before it) and that fetch is
// F1 - its slot L1 by its index register component. After F1 every full vfetch is classified by its index register
// component: untouched since the prologue -> L1 (that component); one F1 wrote from its element -> L2 (that element
// component; F1's format a 32-bit integer one); anything else (written after F1, a constant, relative) -> unsafe. A
// mini fetch inherits its full fetch's class (the memres5 rule for a full fetch that may not have run).
void analyzeProg(const uint8_t* u, size_t bytes, VsInfo& out) noexcept {
  VsProg& g = out.prog;
  g = VsProg{};
  if (!out.ok || !u) return;
  const size_t n = bytes / 4, pairs = n / 3;
  if (!pairs) return;
  auto dw = [&](size_t i) { return mkey::be32(u + i * 4); };
  size_t bound = pairs;
  for (size_t i = 0; i < bound; i++) {
    uint32_t cf[2][2];
    mkey::unpackCfPair(dw(3 * i), dw(3 * i + 1), dw(3 * i + 2), cf);
    for (int j = 0; j < 2; j++)
      if (mkey::isExecOpcode((cf[j][1] >> 12) & 0xFu) && (cf[j][0] & 0xFFFu) < bound) bound = cf[j][0] & 0xFFFu;
  }
  // per register component: 0 initial (r0.x the vertex index, else 0), 1 the prologue's, 0x10 | c F1's element
  // component c, 0x20 a constant F1 wrote (its dst swizzle 0 / 1), 0xFF unknown (written after F1)
  uint8_t pv[64][4] = {};
  struct PSlot {
    uint32_t slot, stride;
    uint8_t cls, src, l2c, rnd, fbr;
    bool bad;
  };
  PSlot ps[kVsSlots];
  uint32_t nps = 0;
  uint32_t fetchOrdinal = 0;
  auto addSrc = [&](uint8_t sc) -> bool {
    for (uint32_t k = 0; k < g.nSrcs; k++)
      if (g.srcs[k] == sc) return true;
    if (g.nSrcs == kProgSrcs) return false;
    g.srcs[g.nSrcs++] = sc;
    return true;
  };
  auto rec = [&](uint32_t instruction, uint32_t slot, uint32_t stride, uint8_t cls, uint8_t src, uint8_t l2c, uint8_t rnd, bool bad,
                  uint8_t fbr) {
    // A CF program may execute the same instruction address in distinct forward
    // blocks. Match the walk occurrence, not just its address or merged slot.
    if (fetchOrdinal < out.nFetch && out.fetch[fetchOrdinal].instruction == instruction) {
      auto& f = out.fetch[fetchOrdinal++];
      f.cls = cls; f.src = src; f.l2c = l2c; f.rnd = rnd; f.safe = !bad;
    } else out.fetchComplete = false;
    uint32_t k = 0;
    while (k < nps && ps[k].slot != slot) k++;
    if (k == nps) {
      if (nps == kVsSlots) return;
      ps[nps++] = PSlot{slot, stride, cls, src, l2c, rnd, fbr, bad};
      return;
    }
    PSlot& p = ps[k];
    const bool clash = p.stride != stride || ((p.cls & cls & kClsL1) && p.src != src) ||
                       ((p.cls & cls & kClsL2) && p.l2c != l2c);
    if (!p.bad && (bad || clash)) p.fbr = bad ? fbr : kFbSlot;
    p.bad = p.bad || bad || clash;
    p.cls |= cls;
    if (cls & kClsL1) p.src = src;
    if (cls & kClsL2) p.l2c = l2c;
    p.rnd |= rnd;
  };
  bool direct = false, f1 = false;
  const auto walk = [&]() {
    bool jumped = false, fullPred = false, fullSkippable = false;
    uint32_t fullCf = 0, lastSlot = 0, lastStride = 0;
    uint8_t lastCls = 0, lastSrc = 0, lastL2c = 0, lastRnd = 1, lastFbr = kFbSlot;
    bool lastBad = true;
    for (size_t i = 0; i < bound; i++) {
      uint32_t cf[2][2];
      mkey::unpackCfPair(dw(3 * i), dw(3 * i + 1), dw(3 * i + 2), cf);
      for (int j = 0; j < 2; j++) {
        const uint32_t op = (cf[j][1] >> 12) & 0xFu, cfIdx = uint32_t(2 * i) + uint32_t(j);
        if (op == 11u) {
          if (!f1 && !g.reason) g.reason = kFbCtl;
          if (!f1) return;
          jumped = true;
          continue;
        }
        if (op >= 7u && op <= 10u) return;  // (analyzeVs failed such a program already)
        if (!mkey::isExecOpcode(op)) continue;
        const bool condExec = op != 1u && op != 2u;
        const uint32_t address = cf[j][0] & 0xFFFu, count = (cf[j][0] >> 12) & 0x7u;
        uint32_t seq = (cf[j][0] >> 16) & 0xFFFu;
        for (uint32_t k = 0; k < count; k++, seq >>= 2) {
          const size_t at = size_t(address) + k;
          if (at * 3 + 3 > n) return;
          const uint32_t d0 = dw(at * 3), d1 = dw(at * 3 + 1), d2 = dw(at * 3 + 2);
          if (seq & 1u) {
            const bool vf = (d0 & 0x1Fu) == 0u, mini = vf && ((d1 >> 30) & 1u);
            const uint32_t dst = (d0 >> 12) & 0x3Fu, src = (d0 >> 5) & 0x3Fu, swz = (d0 >> 30) & 3u;
            const bool dstRel = (d0 >> 18) & 1u, srcRel = (d0 >> 11) & 1u;
            const uint32_t fmt = (d1 >> 16) & 0x3Fu;
            if (!f1) {
              if (vf && !mini && src == 0u && swz == 0u && !srcRel && pv[0][0] == 0u && !g.reason) {
                direct = true;  // the direct walk's program
                return;
              }
              if (g.reason) return;  // the prologue's own failure
              if (!vf || mini) {     // a fetch the walk cannot follow before the index fetch
                g.reason = kFbOp;
                return;
              }
              if (srcRel || dstRel) {
                g.reason = kFbRel;
                return;
              }
              if (condExec || ((d1 >> 31) & 1u) || jumped) {
                g.reason = kFbCtl;
                return;
              }
              g.f1Slot = ((d0 >> 20) & 0x1Fu) * 3u + ((d0 >> 25) & 0x3u);
              g.f1Stride = d2 & 0xFFu;
              int32_t off = int32_t((d2 >> 8) & 0x7FFFFFu);
              if (off & (1 << 22)) off -= (1 << 23);
              g.f1Off = off;
              g.f1Src = uint8_t((src << 2) | swz);
              g.f1Rnd = (d1 >> 15) & 1u;
              g.f1Signed = (d1 >> 12) & 1u;
              g.f1FmtOk = (fmt == 33u || fmt == 34u || fmt == 35u) && ((d1 >> 13) & 1u) &&
                          sext6((d1 >> 24) & 0x3Fu) == 0;
              g.f1Comps = fmt == 33u ? 1u : fmt == 34u ? 2u : 4u;
              addSrc(g.f1Src);
              lastSlot = g.f1Slot;
              lastStride = g.f1Stride;
              lastCls = kClsL1;
              lastSrc = g.f1Src;
              lastL2c = 0;
              lastRnd = g.f1Rnd ? 2u : 1u;
              lastBad = false;
              lastFbr = kFbSlot;
              fullPred = false;
              fullSkippable = false;
              fullCf = cfIdx;
              rec(uint32_t(at * 3u), lastSlot, lastStride, lastCls, lastSrc, 0, lastRnd, elemDwords(fmt) == 0u || lastStride == 0u,
                  kFbSlot);
              for (uint32_t c = 0; c < 4; c++) {
                const uint32_t sel = (d1 >> (3u * c)) & 7u;
                if (sel <= 3u) pv[dst][c] = uint8_t(0x10u | sel);
                else if (sel == 4u || sel == 5u) pv[dst][c] = 0x20u;
              }
              f1 = true;
              continue;
            }
            if (vf) {
              if (!mini) {
                lastSlot = ((d0 >> 20) & 0x1Fu) * 3u + ((d0 >> 25) & 0x3u);
                lastStride = d2 & 0xFFu;
                fullPred = (d1 >> 31) & 1u;
                fullCf = cfIdx;
                fullSkippable = condExec || jumped;
                lastRnd = ((d1 >> 15) & 1u) ? 2u : 1u;
                lastBad = false;
                lastFbr = kFbSlot;
                lastSrc = 0;
                lastL2c = 0;
                lastCls = 0;
                const uint8_t p = srcRel ? 0xFFu : pv[src][swz];
                if (srcRel) {
                  lastBad = true;
                  lastFbr = kFbRel;
                } else if (p == 0u || p == 1u) {
                  lastCls = kClsL1;
                  lastSrc = uint8_t((src << 2) | swz);
                  if (!addSrc(lastSrc)) {
                    lastBad = true;
                    lastFbr = kFbClob;
                  }
                } else if ((p & 0xF0u) == 0x10u) {
                  lastCls = kClsL2;
                  lastL2c = uint8_t(p & 3u);
                  if (!g.f1FmtOk) {
                    lastBad = true;
                    lastFbr = kFbFmt;
                  }
                } else {
                  lastBad = true;
                  lastFbr = kFbClob;
                }
              }
              const bool inherits = !mini || (!fullPred && (fullCf == cfIdx || !fullSkippable));
              const bool bad = lastBad || !inherits || elemDwords(fmt) == 0u || lastStride == 0u;
              rec(uint32_t(at * 3u), lastSlot, lastStride, lastCls, lastSrc, lastL2c, lastRnd, bad,
                  lastBad ? lastFbr : !inherits ? uint8_t(kFbCtl) : uint8_t(kFbSlot));
            }
            // the fetch's destination: unknown values from here on
            if (dstRel) {
              std::memset(pv, 0xFF, sizeof pv);
            } else {
              for (uint32_t c = 0; c < 4; c++)
                if (((d1 >> (3u * c)) & 7u) != 7u) pv[dst][c] = 0xFFu;
            }
          } else {
            const AluDec a = decodeAlu(d0, d1, d2);
            if (!f1) {
              if (!g.reason) {
                const bool rel = a.vRel || a.sRel || a.c0Rel || a.c1Rel || (a.s1.sel && (a.s1.reg & 0x40u)) ||
                                 (a.s2.sel && (a.s2.reg & 0x40u)) || (a.s3.sel && (a.s3.reg & 0x40u));
                if (!aluSupported(a)) g.reason = kFbOp;
                else if (rel) g.reason = kFbRel;
                else if (a.pred || condExec || jumped) g.reason = kFbCtl;
                else if (g.n >= kProlMax) g.reason = kFbLong;
                else {
                  g.ins[g.n][0] = d0;
                  g.ins[g.n][1] = d1;
                  g.ins[g.n][2] = d2;
                  g.n++;
                  if (!a.exp) {
                    g.wr[a.vDst] |= uint8_t(a.vMask);
                    g.wr[a.sDst] |= uint8_t(a.sMask);
                  }
                }
              }
              if (!a.exp)
                for (uint32_t c = 0; c < 4; c++) {
                  if ((a.vMask >> c) & 1u) pv[a.vDst][c] = 1u;
                  if ((a.sMask >> c) & 1u) pv[a.sDst][c] = 1u;
                }
            } else if (a.vRel || a.sRel) {
              std::memset(pv, 0xFF, sizeof pv);
            } else if (!a.exp) {
              for (uint32_t c = 0; c < 4; c++) {
                if ((a.vMask >> c) & 1u) pv[a.vDst][c] = 0xFFu;
                if ((a.sMask >> c) & 1u) pv[a.sDst][c] = 0xFFu;
              }
            }
          }
        }
      }
    }
  };
  walk();
  if (direct) {
    g = VsProg{};
    return;
  }
  if (!f1 || g.reason) {  // the computed walk refused: the unsafe slots fall back with its reason
    if (!g.reason) g.reason = kFbSlot;
    for (uint32_t k = 0; k < out.n; k++)
      if (!out.s[k].safe) out.s[k].fbr = g.reason;
    g.on = false;
    return;
  }
  g.on = true;
  if (fetchOrdinal != out.nFetch) out.fetchComplete = false;
  for (uint32_t m = 0; m < out.n; m++) {
    VsSlot& v = out.s[m];
    uint32_t k = 0;
    while (k < nps && ps[k].slot != v.slot) k++;
    if (k == nps) {  // (analyzeVs saw a vfetch the computed walk did not)
      v.safe = false;
      v.fbr = kFbSlot;
      continue;
    }
    v.cls = ps[k].cls;
    v.src = ps[k].src;
    v.l2c = ps[k].l2c;
    v.rnd = ps[k].rnd;
    v.safe = !ps[k].bad;
    v.fbr = ps[k].bad ? ps[k].fbr : uint8_t(kFbSlot);
  }
}

// The literal constants. XenosRecomp reads a float constant operand from the live constant file when the container's
// constant table names its register, else from the container's definition table when that defines it (a literal baked
// into the translation: `float4 cN = asfloat(...)`), else from the live file (superman.patch g_UnnamedConstants) - so a
// prologue's literal (the world family's c255 = 0.5) is the corpus's value, whatever the guest's constant file holds.
// The table: tools/perf/mem_res_literals.py over the containers the shader cache is built from (data/shaders_carved,
// data/shaders_menu), the registers each VS's prologue reads.
const LiteralRow kLits[] = {
#include "resource_census_literals.inc"  // (generated: tools/perf/mem_res_literals.py)
    {~0ull, 0u, 0u, {0u, 0u, 0u, 0u}}  // (the end: never matched)
};
constexpr size_t kNLits = sizeof kLits / sizeof kLits[0] - 1u;
std::atomic<const LiteralRow*> g_litRows{nullptr};  // a test's table (nullptr = kLits)
std::atomic<size_t> g_litN{0};
const LiteralRow* litFind(uint64_t hash, uint32_t reg) noexcept {
  const LiteralRow* rows = g_litRows.load(std::memory_order_acquire);
  const size_t n = rows ? g_litN.load(std::memory_order_relaxed) : kNLits;
  if (!rows) rows = kLits;
  const LiteralRow* e = rows + n;
  const LiteralRow* it = std::lower_bound(rows, e, hash, [reg](const LiteralRow& r, uint64_t h) {
    return r.hash < h || (r.hash == h && r.reg < reg);
  });
  return it != e && it->hash == hash && it->reg == reg ? it : nullptr;
}
// The constant registers an interpreted ALU instruction's operands name (a *sc op's src3 register is its constant).
template <class F>
void aluConstRegs(const AluDec& a, F&& f) {
  for (const AluSrc* s : {&a.s1, &a.s2, &a.s3})
    if (!s->sel || (s == &a.s3 && a.sop >= 42u && a.sop <= 47u)) f(s->reg & 0xFFu);
}
// A computed program's literals (noteVsUcode, after analyzeProg): each prologue constant the table defines. A prologue
// naming a constant of a VS the corpus has no container for (no kLitKnown row: its literals unknown), a literal
// defined two ways in the corpus, or more than kProgLits: the computed slots fall back (lit).
constexpr uint32_t kLitKnown = 0xFFFFFFFFu;  // the row of a VS whose container the generator read
void progLiterals(uint64_t hash, VsInfo& v) noexcept {
  VsProg& g = v.prog;
  if (!g.on) return;
  bool bad = false, names = false;
  for (uint32_t i = 0; i < g.n && !bad; i++)
    aluConstRegs(decodeAlu(g.ins[i][0], g.ins[i][1], g.ins[i][2]), [&](uint32_t reg) {
      names = true;
      for (uint32_t k = 0; k < g.nLit; k++)
        if (g.litReg[k] == reg) return;
      const LiteralRow* r = litFind(hash, reg);
      if (!r) return;
      if ((r->flags & 1u) || g.nLit == kProgLits) {
        bad = true;
        return;
      }
      g.litReg[g.nLit] = reg;
      std::memcpy(g.litV[g.nLit], r->v, sizeof g.litV[0]);
      g.nLit++;
    });
  if (names && !litFind(hash, kLitKnown)) bad = true;
  if (!bad) return;
  g.on = false;
  g.reason = kFbLit;
  for (uint32_t k = 0; k < v.n; k++)
    if (v.s[k].cls & (kClsL1 | kClsL2)) {
      v.s[k].safe = false;
      v.s[k].fbr = kFbLit;
    }
}

// The L1 sources' and the F1 elements' values over a draw (famCompute): the bound evaluated per vertex of [vMin, vMax]
// in intervals; F1's index set read back in both windows (the GPU's copy is one of them).
uint32_t srcIndexOf(const VsProg& g, uint8_t sc) noexcept {
  for (uint32_t k = 0; k < g.nSrcs; k++)
    if (g.srcs[k] == sc) return k;
  return 0;
}
// A 32-bit integer element dword (XeVfSwap, XeVfLoad's integer path: float(uint) or float(int)) from one window.
bool f1Value(uint32_t phys, uint32_t endian, bool isSigned, uint32_t window, float& v) noexcept {
  const uint8_t* pg = g_host.load(std::memory_order_relaxed)(window + (phys & ~0xFFFu));
  if (!pg) return false;
  uint32_t d;
  std::memcpy(&d, pg + (phys & 0xFFFu), 4);
  if (endian == 1u || endian == 2u) d = ((d & 0x00FF00FFu) << 8) | ((d >> 8) & 0x00FF00FFu);
  if (endian == 2u || endian == 3u) d = (d << 16) | (d >> 16);
  v = isSigned ? float(int32_t(d)) : float(d);
  return true;
}
uint64_t famKey(const DrawState& d, const VsProg& g) noexcept {
  uint64_t h = d.vsHash ^ (uint64_t(d.vMin) << 1) ^ (uint64_t(d.vMax) << 33);
  auto mix = [&](uint64_t x) {
    h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  };
  for (uint32_t i = 0; i < g.n; i++)  // every constant the prologue's three operands could name
    aluConstRegs(decodeAlu(g.ins[i][0], g.ins[i][1], g.ins[i][2]), [&](uint32_t reg) {
      for (uint32_t c = 0; c < 4; c++) mix(d.consts[reg * 4u + c]);
    });
  return h;
}
void famWitness(State& s, DrawState& d, const std::vector<uint32_t>& f1Idx) noexcept;
void famCompute(State& s, DrawState& d) noexcept {
  FamDraw& f = d.fam;
  f = FamDraw{};
  f.done = true;
  const VsProg& g = d.vs->prog;
  if (!d.consts || !d.fetch) {
    f.reason = kFbState;
    return;
  }
  if (g.nLit) {  // the constants as the translation reads them: its literals over the live file (the memo key too)
    static thread_local uint32_t eff[1024];
    std::memcpy(eff, d.consts, sizeof eff);
    for (uint32_t k = 0; k < g.nLit; k++) std::memcpy(eff + g.litReg[k] * 4u, g.litV[k], sizeof g.litV[0]);
    d.consts = eff;
  }
  const uint64_t nv = uint64_t(d.vMax) - d.vMin + 1u;
  if (nv > kFamVertsMax) {
    f.reason = kFbMany;
    return;
  }
  const uint32_t f1d0 = d.fetch[g.f1Slot * 2u], f1d1 = d.fetch[g.f1Slot * 2u + 1u];
  const uint32_t f1Base = f1d0 & 0x1FFFFFFCu, f1Size = ((f1d1 >> 2) & 0xFFFFFFu) * 4u, f1End = f1d1 & 3u;
  const uint64_t key = famKey(d, g) ^ (uint64_t(f1d0) * 0x100000001B3ull) ^ f1d1;
  std::vector<uint32_t> idx;
  bool hit = false;
  {
    std::lock_guard<std::mutex> lk(s.famMu);
    const auto it = s.famMemo.find(key);
    if (it != s.famMemo.end()) {
      hit = true;
      if (it->second.reason) {
        f.reason = it->second.reason;
        return;
      }
      std::memcpy(f.vLo, it->second.vLo, sizeof f.vLo);
      std::memcpy(f.vHi, it->second.vHi, sizeof f.vHi);
      idx = it->second.f1Idx;
      f.l2reason = it->second.l2reason;
    }
  }
  if (!hit) {
    AluDec ins[kProlMax];
    for (uint32_t i = 0; i < g.n; i++) ins[i] = decodeAlu(g.ins[i][0], g.ins[i][1], g.ins[i][2]);
    const uint8_t* wr = g.wr;  // the components the prologue writes (reset per vertex)
    static thread_local IRegs R;
    for (auto& reg : R.r)
      for (auto& c : reg) c = Iv{0.0, 0.0};
    for (uint32_t k = 0; k < kProgSrcs; k++) {
      f.vLo[k] = 1e300;
      f.vHi[k] = -1e300;
    }
    const uint32_t f1Count = g.f1Stride ? f1Size / (g.f1Stride * 4u) : 1u;
    size_t idxCap = size_t(1) << 18;
    uint8_t reason = 0;
    for (uint64_t v = d.vMin; v <= d.vMax && !reason; v++) {
      for (uint32_t r = 0; r < 64; r++)
        if (wr[r])
          for (uint32_t c = 0; c < 4; c++)
            if ((wr[r] >> c) & 1u) R.r[r][c] = Iv{0.0, 0.0};
      R.r[0][0] = Iv{double(v), double(v)};
      R.r[0][1] = R.r[0][2] = R.r[0][3] = Iv{0.0, 0.0};
      R.ps = Iv{0.0, 0.0};
      for (uint32_t i = 0; i < g.n; i++)
        if (!ivStep(R, d.consts, ins[i])) {
          reason = kFbRange;
          break;
        }
      if (reason) break;
      for (uint32_t k = 0; k < g.nSrcs; k++) {
        const Iv x = R.r[g.srcs[k] >> 2][g.srcs[k] & 3u];
        f.vLo[k] = std::min(f.vLo[k], x.lo);
        f.vHi[k] = std::max(f.vHi[k], x.hi);
      }
      int64_t a, b;  // F1's indices for this vertex (both sides of a rounding edge)
      if (!ivIndex(R.r[g.f1Src >> 2][g.f1Src & 3u], g.f1Rnd, a, b) || b - a + 1 > int64_t(kFamIdxSpan)) {
        reason = kFbRange;
        break;
      }
      for (int64_t i = a; i <= b; i++)
        if (i >= 0 && i < int64_t(f1Count)) idx.push_back(uint32_t(i));
        else if (g.f1FmtOk) f.l2reason = kFbRange;  // (outside F1's declared range: its value is not known)
      if (idx.size() >= idxCap) {  // (kept small: a draw's indices repeat)
        std::sort(idx.begin(), idx.end());
        idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
        idxCap = std::max(idxCap, idx.size() * 2u);
      }
    }
    std::sort(idx.begin(), idx.end());  // the set, ascending (the witness searches it)
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
    {
      std::lock_guard<std::mutex> lk(s.famMu);
      if (s.famMemo.size() >= 8192u) s.famMemo.clear();
      State::FamMemo& m = s.famMemo[key];
      std::memcpy(m.vLo, f.vLo, sizeof m.vLo);
      std::memcpy(m.vHi, f.vHi, sizeof m.vHi);
      m.f1Idx = idx;
      m.reason = reason;
      m.l2reason = f.l2reason;
    }
    if (reason) {
      f.reason = reason;
      return;
    }
  }
  f.ok = true;
  // L2: the stream values at F1's index set, in both windows
  if (!g.f1FmtOk) {
    f.l2reason = kFbFmt;
  } else if (!f.l2reason) {
    for (uint32_t c = 0; c < 4; c++) {
      f.l2Lo[c] = c < g.f1Comps ? 1e300 : 0.0;
      f.l2Hi[c] = c < g.f1Comps ? -1e300 : 0.0;
    }
    bool any = false;
    for (const uint32_t i : idx) {
      for (uint32_t c = 0; c < g.f1Comps && !f.l2reason; c++) {
        const int64_t off = (int64_t(i) * g.f1Stride + g.f1Off + c) * 4;
        if (off < 0 || off + 4 > int64_t(f1Size)) {
          f.l2reason = kFbRange;
          break;
        }
        const uint32_t phys = f1Base + uint32_t(off);
        float v8 = 0.0f, vA = 0.0f;
        const bool r8 = f1Value(phys, f1End, g.f1Signed, 0x80000000u, v8),
                   rA = f1Value(phys, f1End, g.f1Signed, 0xA0000000u, vA);
        if (!r8 && !rA) {
          f.l2reason = kFbRead;
          break;
        }
        for (const float v : {r8 ? v8 : vA, rA ? vA : v8}) {
          f.l2Lo[c] = std::min(f.l2Lo[c], double(v));
          f.l2Hi[c] = std::max(f.l2Hi[c], double(v));
        }
        any = true;
      }
      if (f.l2reason) break;
    }
    if (!f.l2reason && !any) f.l2reason = kFbRange;  // (no F1 index inside its range: nothing to read)
  }
  f.l2ok = !f.l2reason;
  // the soundness witness: the first draws, then 1 in kWitnessK, evaluated exactly per vertex
  const uint64_t nd = s.famDraws.fetch_add(1, std::memory_order_relaxed);
  const uint32_t k = g_witnessK.load(std::memory_order_relaxed);
  if (nd < kWitnessFirst || (k && nd % k == 0)) famWitness(s, d, idx);
}

// A slot's integer index range [lo, hi] over its classes (the bytes fetchBound marks), under each of its fetches'
// index conversions. The witness checks exactly this.
bool slotIdxRange(const DrawState& d, const VsSlot& v, int64_t& lo, int64_t& hi) noexcept {
  lo = INT64_MAX;
  hi = INT64_MIN;
  auto join = [&](int64_t a, int64_t b) {
    lo = std::min(lo, a);
    hi = std::max(hi, b);
  };
  if (v.cls & kClsD) join(d.vMin, d.vMax);
  for (uint32_t r = 0; r < 2; r++) {
    if (!((v.rnd >> r) & 1u)) continue;
    int64_t a, b;
    if (v.cls & kClsL1) {
      const uint32_t k = srcIndexOf(d.vs->prog, v.src);
      if (!ivIndex(Iv{d.fam.vLo[k], d.fam.vHi[k]}, r == 1u, a, b)) return false;
      join(a, b);
    }
    if (v.cls & kClsL2) {
      if (!ivIndex(Iv{d.fam.l2Lo[v.l2c & 3u], d.fam.l2Hi[v.l2c & 3u]}, r == 1u, a, b)) return false;
      join(a, b);
    }
  }
  if (v.cls & (kClsL1 | kClsL2)) hi -= int64_t(g_boundShrink.load(std::memory_order_relaxed));  // (a test's: 0)
  return lo <= hi;
}

// The witness: every vertex of the draw evaluated EXACTLY (xStep), each computed slot's exact index checked inside
// slotIdxRange - and F1's element read exactly at F1's exact index (both windows) for the L2 slots.
void famWitness(State& s, DrawState& d, const std::vector<uint32_t>& f1Idx) noexcept {
  const VsProg& g = d.vs->prog;
  if (!d.fam.ok || !d.any) return;
  add(s, kOffBoundSampled);
  struct Chk {
    const VsSlot* v;
    int64_t lo, hi;
  };
  Chk chk[kVsSlots];
  uint32_t nc = 0;
  for (uint32_t m = 0; m < d.vs->n; m++) {
    const VsSlot& v = d.vs->s[m];
    if (!v.safe || !(v.cls & (kClsL1 | kClsL2))) continue;
    if ((v.cls & kClsL2) && !d.fam.l2ok) continue;
    int64_t lo, hi;
    if (!slotIdxRange(d, v, lo, hi)) continue;
    chk[nc++] = Chk{&v, lo, hi};
  }
  // an L2 bound reads the stream at F1's index SET (each vertex's interval's indices): F1's exact index must be in it
  bool l2Any = false;
  for (uint32_t q = 0; q < nc; q++) l2Any = l2Any || (chk[q].v->cls & kClsL2) != 0;
  AluDec ins[kProlMax];
  for (uint32_t i = 0; i < g.n; i++) ins[i] = decodeAlu(g.ins[i][0], g.ins[i][1], g.ins[i][2]);
  const uint32_t f1d0 = d.fetch[g.f1Slot * 2u], f1d1 = d.fetch[g.f1Slot * 2u + 1u];
  const uint32_t f1Base = f1d0 & 0x1FFFFFFCu, f1Size = ((f1d1 >> 2) & 0xFFFFFFu) * 4u, f1End = f1d1 & 3u;
  static thread_local XRegs R;
  std::memset(&R, 0, sizeof R);
  bool viol = false;
  auto report = [&](uint64_t v, uint32_t slot, const char* cls, int64_t e, int64_t lo, int64_t hi) {
    viol = true;
    uint32_t nlog;
    if (sample(s.logBoundV, nlog))
      SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[MEM_RES] BOUNDV #%u vs=%016llX slot=%u cls=%s v=%llu exact=%lld bound=[%lld,%lld] vrange=[%u,%u]",
                  nlog, (unsigned long long)d.vsHash, slot, cls, (unsigned long long)v, (long long)e, (long long)lo,
                  (long long)hi, d.vMin, d.vMax);
  };
  for (uint64_t v = d.vMin; v <= d.vMax && !viol; v++) {
    for (uint32_t r = 0; r < 64; r++)
      if (g.wr[r])
        for (uint32_t c = 0; c < 4; c++)
          if ((g.wr[r] >> c) & 1u) R.r[r][c] = 0.0f;
    R.r[0][0] = float(v);
    R.r[0][1] = R.r[0][2] = R.r[0][3] = 0.0f;
    R.ps = 0.0f;
    for (uint32_t i = 0; i < g.n; i++) xStep(R, d.consts, ins[i]);
    float el[4] = {};  // F1's element at its exact index (one window's; both checked below)
    float elA[4] = {};
    bool elOk = false;
    int64_t i1 = INT64_MIN;
    const bool i1Ok = xIndex(R.r[g.f1Src >> 2][g.f1Src & 3u], g.f1Rnd, i1);
    if (l2Any && (!i1Ok || i1 < 0 || !std::binary_search(f1Idx.begin(), f1Idx.end(), uint32_t(i1)))) {
      report(v, g.f1Slot, "F1", i1, f1Idx.empty() ? -1 : int64_t(f1Idx.front()),
             f1Idx.empty() ? -1 : int64_t(f1Idx.back()));
      break;
    }
    if (i1Ok && g.f1FmtOk) {
      elOk = true;
      for (uint32_t c = 0; c < g.f1Comps && elOk; c++) {
        const int64_t off = (i1 * g.f1Stride + g.f1Off + c) * 4;
        if (off < 0 || off + 4 > int64_t(f1Size)) {
          elOk = false;
          break;
        }
        const bool r8 = f1Value(f1Base + uint32_t(off), f1End, g.f1Signed, 0x80000000u, el[c]),
                   rA = f1Value(f1Base + uint32_t(off), f1End, g.f1Signed, 0xA0000000u, elA[c]);
        if (!r8 && !rA) elOk = false;
        if (!r8) el[c] = elA[c];
        if (!rA) elA[c] = el[c];
      }
    }
    if (l2Any && !elOk) {  // (F1's index in the set the bound read: its element must be readable as it was there)
      report(v, g.f1Slot, "F1read", i1, f1Idx.front(), f1Idx.back());
      break;
    }
    for (uint32_t q = 0; q < nc && !viol; q++) {
      const VsSlot& vs = *chk[q].v;
      for (uint32_t r = 0; r < 2 && !viol; r++) {
        if (!((vs.rnd >> r) & 1u)) continue;
        int64_t e = INT64_MIN;
        const char* cls = nullptr;
        // an exact index outside the bound - or one the conversion cannot form (non-finite, beyond 2^24) - is a
        // violation: the bound held finite values
        auto out = [&](float x) { return !xIndex(x, r == 1u, e) || e < chk[q].lo || e > chk[q].hi; };
        if ((vs.cls & kClsL1) && out(R.r[vs.src >> 2][vs.src & 3u])) cls = "L1";
        for (const float* src : {el, elA})
          if (!cls && (vs.cls & kClsL2) && elOk && out(src[vs.l2c & 3u])) cls = "L2";
        if (cls) report(v, vs.slot, cls, e, chk[q].lo, chk[q].hi);
      }
    }
  }
  if (viol) add(s, kOffBoundViolations);
}

void noteFallback(State& s, const DrawState& d, uint8_t reason) noexcept {
  add(s, kOffFetchFbShader);
  add(s, kOffFbReason + (reason < kFbReasons ? reason : 0u));
  std::lock_guard<std::mutex> lk(s.fbMu);
  if (s.fbByVs.size() >= 4096u && !s.fbByVs.count(d.vsHash)) return;
  auto& e = s.fbByVs[d.vsHash];
  e.first++;
  e.second = reason;
}

// A vertex fetch request's fetched bytes [lo, lo + n): the vertices its draw's indices address (memres6: through a
// computed index too), clamped to the declared range (the fetch constant's size); fail-closed to the declared range
// [phys, +len), counted by reason. Returns the request's class (the ring / via=).
// mem-native: *exactSlot = the slot when its bytes are the draw's USED vertices (markUsedVertices): a direct slot
// (cls D alone) of an indexed draw whose used-vertex bitmap is known; else the span [lo, lo + n) is the bound.
uint8_t fetchBound(State& s, uint32_t phys, uint32_t len, uint32_t& lo, uint32_t& n,
                   const VsSlot** exactSlot = nullptr) noexcept {
  DrawState& d = t_draw;
  lo = phys;
  n = len;
  if (exactSlot) *exactSlot = nullptr;
  if (!d.on || !t_slot || !d.range) {
    add(s, kOffFetchFbDraw);
    return kReqFall;
  }
  if (!d.vsKnown || !d.vs) {
    noteFallback(s, d, kFbVs);
    return kReqFall;
  }
  const VsSlot* v = nullptr;
  if (d.vs->ok)
    for (uint32_t i = 0; i < d.vs->n; i++)
      if (d.vs->s[i].slot == t_slot - 1u) {
        v = &d.vs->s[i];
        break;
      }
  if (!v || !v->safe) {
    noteFallback(s, d, v ? v->fbr : uint8_t(kFbSlot));
    return kReqFall;
  }
  const bool computed = (v->cls & (kClsL1 | kClsL2)) != 0;
  if (computed && d.any) {
    if (!d.fam.done) famCompute(s, d);
    if (!d.fam.ok) {
      noteFallback(s, d, d.fam.reason);
      return kReqFall;
    }
    if ((v->cls & kClsL2) && !d.fam.l2ok) {
      noteFallback(s, d, d.fam.l2reason);
      return kReqFall;
    }
  }
  // mem-native (Astra round 4): a direct slot's span is the unconverted [vMin, vMax]; a rounded conversion of an index
  // at or above 2^23 is not the index (float32: 0x800001 + 0.5 -> 0x800002) - the declared range
  auto rounds = [&]() {  // some vfetch of the slot rounds its index (the slot's own rnd is the computed walk's)
    if (!d.vs->fetchComplete) return true;
    for (uint32_t f = 0; f < d.vs->nFetch; f++)
      if (d.vs->fetch[f].slot == v->slot && (d.vs->fetch[f].rnd & 2u)) return true;
    return false;
  };
  if (!computed && d.any && d.vMax >= (1u << 23) && rounds()) {
    noteFallback(s, d, kFbRange);
    return kReqFall;
  }
  int64_t iLo = 0, iHi = -1;
  const bool idxOk = d.any && slotIdxRange(d, *v, iLo, iHi);
  if (d.any && !idxOk && computed) {  // (an index beyond 2^24 under a conversion: not formed - fail-closed)
    noteFallback(s, d, kFbRange);
    return kReqFall;
  }
  add(s, kOffFetchBounded);
  if (computed) add(s, kOffFetchBoundedComp);
  if (!idxOk) {  // no vertex (or, direct, no index left): nothing fetched
    n = 0;
    return kReqBound;
  }
  const int64_t a = int64_t(phys) + (iLo * int64_t(v->stride) + v->minOff) * 4;
  const int64_t b = int64_t(phys) + (iHi * int64_t(v->stride) + v->maxEnd) * 4;
  const int64_t a2 = std::max<int64_t>(a, int64_t(phys)), b2 = std::min<int64_t>(b, int64_t(phys) + len);
  if (b2 <= a2) {
    n = 0;
    return kReqBound;
  }
  lo = uint32_t(a2);
  n = uint32_t(b2 - a2);
  if (exactSlot && d.exact && v->cls == kClsD) *exactSlot = v;
  return b >= int64_t(phys) + len ? kReqClamp : kReqBound;
}

// mem-native: a direct slot's fetched bytes vertex-exact - each run [v0, v1] of the draw's used vertices marks
// [phys + (v0 * stride + minOff) * 4, phys + (v1 * stride + maxEnd) * 4), clamped to the declared range (a run reaching
// its end: the clamped class). The bytes between runs (no index uses them) are not fetched.
// The watched dword (the data watch's, once armed) and the marks of it logged (MARKW, 16).
std::atomic<uint32_t> g_markTarget{0xFFFFFFFFu}, g_markLogged{0};
// The slot's element dwords within one vertex: bit e = dword e of [0, stride) some vfetch of the slot reads. 0 = not
// expressible (an element outside [0, stride), a stride over 64 dwords, an incomplete fetch list).
uint64_t slotElements(const DrawState& d, const VsSlot& v) noexcept {
  if (!d.vs || !d.vs->fetchComplete || v.stride == 0u || v.stride > 64u) return 0;
  uint64_t m = 0;
  for (uint32_t f = 0; f < d.vs->nFetch; f++) {
    const VsFetch& x = d.vs->fetch[f];
    if (x.slot != v.slot) continue;
    if (x.off < 0 || x.width == 0u || uint64_t(x.off) + x.width > v.stride) return 0;
    for (uint32_t e = 0; e < x.width; e++) m |= 1ull << (uint32_t(x.off) + e);
  }
  return m;
}
void markWatchLog(State& s, uint32_t phys, uint32_t len, const VsSlot& v, uint64_t elems, int64_t a, int64_t b,
                  uint32_t v0, uint32_t v1) noexcept {
  const uint32_t t = g_markTarget.load(std::memory_order_relaxed);
  if (t == 0xFFFFFFFFu || int64_t(t) < a || int64_t(t) >= b) return;
  if (g_markLogged.fetch_add(1, std::memory_order_relaxed) >= 16u) return;
  const DrawState& d = t_draw;
  const int64_t rel = (int64_t(t) - int64_t(phys)) / 4;
  const int64_t k = v.stride ? rel / int64_t(v.stride) : 0, e = v.stride ? rel % int64_t(v.stride) : 0;
  (void)s;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[MEM_RES] MARKW target=%08X vs=%016llX slot=%u stride=%u min=%d max=%d elems=%016llX req=%08X+%X "
              "vrange=[%u,%u] run=[%u,%u] mark=%08X+%X vertex=%lld elem=%lld",
              t, (unsigned long long)d.vsHash, v.slot, v.stride, v.minOff, v.maxEnd, (unsigned long long)elems, phys,
              len, d.vMin, d.vMax, v0, v1, uint32_t(a), uint32_t(b - a), (long long)k, (long long)e);
}
void markWatchSpan(uint32_t phys, uint32_t len, uint32_t lo, uint32_t n, uint8_t cls) noexcept {
  const uint32_t t = g_markTarget.load(std::memory_order_relaxed);
  if (t == 0xFFFFFFFFu || t < lo || uint64_t(t) >= uint64_t(lo) + n) return;
  if (g_markLogged.fetch_add(1, std::memory_order_relaxed) >= 16u) return;
  const DrawState& d = t_draw;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
              "[MEM_RES] MARKW target=%08X vs=%016llX slot=%d span req=%08X+%X mark=%08X+%X cls=%u draw=%d vrange=[%u,%u]",
              t, (unsigned long long)d.vsHash, int(t_slot) - 1, phys, len, lo, n, unsigned(cls), d.on ? 1 : 0, d.vMin,
              d.vMax);
}
void markUsedVertices(State& s, uint32_t phys, uint32_t len, const VsSlot& v) noexcept {
  const DrawState& d = t_draw;
  const uint32_t nv = d.vMax - d.vMin + 1u, words = (nv + 63u) / 64u;
  const int64_t end = int64_t(phys) + len;
  s.vtxExact.fetch_add(1, std::memory_order_relaxed);
  // mem-native: a used vertex fetches its slot's element dwords; a dense slot (its elements cover the stride, or its
  // [minOff, maxEnd) does) marks each run's span at once
  const uint64_t elems = slotElements(d, v);
  const uint64_t full = v.stride >= 64u ? ~0ull : (1ull << v.stride) - 1u;
  const bool dense = elems ? (elems & full) == full : v.maxEnd - v.minOff >= int32_t(v.stride);
  auto markOne = [&](int64_t a, int64_t b, uint32_t v0, uint32_t v1) {
    const int64_t a2 = std::max<int64_t>(a, int64_t(phys)), b2 = std::min<int64_t>(b, end);
    if (b2 <= a2) return;
    markWatchLog(s, phys, len, v, elems, a2, b2, v0, v1);
    markFetched(s, uint32_t(a2), uint32_t(b2 - a2), b >= end ? kReqClamp : kReqBound);
  };
  uint32_t i = 0;
  while (i < nv) {
    const uint64_t m = d.used[i >> 6] >> (i & 63u);
    if (!m) {  // no used vertex left in this word
      i = ((i >> 6) + 1u) << 6;
      continue;
    }
    i += uint32_t(__builtin_ctzll(m));  // the next used vertex
    if (i >= nv) break;
    uint32_t j = i;  // the run's end: the next unused vertex (the bits past nv are 0)
    for (;;) {
      if ((j >> 6) >= words) break;
      const uint64_t inv = ~d.used[j >> 6] >> (j & 63u);
      if (inv) {
        j += uint32_t(__builtin_ctzll(inv));
        break;
      }
      j = ((j >> 6) + 1u) << 6;
    }
    j = std::min(j, nv);
    s.vtxRuns.fetch_add(1, std::memory_order_relaxed);
    if (dense) {
      markOne(int64_t(phys) + ((int64_t(d.vMin) + i) * int64_t(v.stride) + v.minOff) * 4,
              int64_t(phys) + ((int64_t(d.vMin) + j - 1) * int64_t(v.stride) + v.maxEnd) * 4, i, j - 1u);
    } else {
      for (uint32_t k = i; k < j; k++) {
        const int64_t base = int64_t(phys) + (int64_t(d.vMin) + k) * int64_t(v.stride) * 4;
        if (!elems) {
          markOne(base + int64_t(v.minOff) * 4, base + int64_t(v.maxEnd) * 4, k, k);
          continue;
        }
        for (uint64_t m = elems; m;) {  // each contiguous segment of the element mask
          const uint32_t e0 = uint32_t(__builtin_ctzll(m));
          const uint64_t rest = ~(m >> e0);
          const uint32_t n = rest ? uint32_t(__builtin_ctzll(rest)) : 64u - e0;
          markOne(base + int64_t(e0) * 4, base + int64_t(e0 + n) * 4, k, k);
          m &= (e0 + n >= 64u) ? 0ull : ~((1ull << (e0 + n)) - 1u);
        }
      }
    }
    i = j;
  }
}

// ---- memres2: the traps (naming the writer of a missed page outside the command buffer) ----
// Fire an armed trap: the writer recorded (the faulting thread's guest context; host = a kernel-mode host write about
// to start), the page released. Only the thread that wins the armed -> fired exchange records.
bool trapFire(State& s, uint32_t page, uint32_t va, uint64_t rip, bool host) noexcept {
  uint8_t armed = kTrapArmed;
  if (page >= kPages || !s.trapSt[page].compare_exchange_strong(armed, kTrapFired, std::memory_order_acq_rel))
    return false;
  const uint32_t n = std::min(s.nTraps.load(std::memory_order_acquire), kMaxTraps);
  for (uint32_t i = 0; i < n; i++) {
    State::Trap& t = s.trap[i];
    if (!t.ready.load(std::memory_order_acquire) || t.page.load(std::memory_order_relaxed) != page) continue;
    const uint64_t eb = g_exeBase.load(std::memory_order_relaxed);
    t.va.store(va, std::memory_order_relaxed);
    // exe-relative inside the image's first GiB, else absolute (bit 63 set); 0 = none (a host write scope)
    t.rip.store(!rip ? 0u : (eb && rip >= eb && rip - eb < 0x40000000ull) ? rip - eb : (rip | (1ull << 63)),
                std::memory_order_relaxed);
    t.lr.store(sr::kernel::getCurrentGuestPC(), std::memory_order_relaxed);
    t.ctr.store(sr::kernel::getCurrentGuestCtr(), std::memory_order_relaxed);
    t.r1.store(sr::kernel::getCurrentGuestR1(), std::memory_order_relaxed);
#if defined(_WIN32)
    t.tid.store(uint32_t(GetCurrentThreadId()), std::memory_order_relaxed);
#endif
    t.host.store(host, std::memory_order_relaxed);
    t.inFetch.store(chunkFetched(s, page, (va & 0xFFFu) >> kChunkLog2), std::memory_order_relaxed);
    // memres4: the written dword itself (fetched? by which recorded range?) and the 32 bytes at va - 8 before the write
    t.dwIn.store(dwFetched(s, page, (va & 0xFFFu) >> 2), std::memory_order_relaxed);
    uint64_t rq = 0;
    t.req.store(coveringReqs(s, page, va & 0xFFCu, 4u, &rq, 1) ? rq : 0u, std::memory_order_relaxed);
    uint8_t hb[32];
    read32(va - 8u, hb);
    for (uint32_t k = 0; k < 8u; k++) {
      uint32_t v;
      std::memcpy(&v, hb + 4u * k, 4);
      t.hdr[k].store(v, std::memory_order_relaxed);
    }
    t.fired.store(true, std::memory_order_release);
    break;
  }
  add(s, kOffTrapFired);
  if (host) add(s, kOffTrapHost);
  g_protect.load(std::memory_order_relaxed)(page, false);
  return true;
}

// ---- mem-native: the data watch (SR_MEM_RES_DWATCH=1) ----
// The first MISSED decision whose fetched bytes moved arms a hardware write watch (DR0, 4 bytes) on exactly that dword
// of the registry's window, on every thread of the process (a helper thread suspends each in turn); each write then
// records the writer (rip, the guest context) and the new value, printed beside the census lines. The patches noted for
// the same physical dword are counted per window. Diagnostic only: nothing decides by it.
struct DwHit {
  std::atomic<uint64_t> rip{0};
  std::atomic<uint32_t> value{0}, lr{0}, ctr{0}, r1{0}, tid{0};
  std::atomic<bool> ready{false};
};
constexpr uint32_t kDwHits = 64;
DwHit g_dwHit[kDwHits];
std::atomic<uintptr_t> g_dwHost{0};
std::atomic<uint32_t> g_dwVa{0}, g_dwPhys{0xFFFFFFFFu}, g_dwPage{0}, g_dwAt{0}, g_dwHits{0}, g_dwPrinted{0},
    g_dwThreads{0}, g_dwArmFail{0};
std::atomic<uint64_t> g_dwNoted[2] = {};
std::atomic<bool> g_dwArmed{false};
// SR_MEM_RES_DWATCH=1: the first moved miss; =<hex physical address>[;<hex>...] (8 hex digits each): the first listed
// dword whose page the census decides first (in a grant of SR_MEM_RES_DWATCH_GRANT bytes when that is set).
struct DwTargets {
  uint32_t phys[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  uint32_t n = 0, grantBytes = 0;
};
const DwTargets& dwatchTargets() {
  static const DwTargets v = [] {
    DwTargets t;
    const char* e = std::getenv("SR_MEM_RES_DWATCH");
    if (!e || std::strlen(e) < 8) return t;
    for (const char* p = e; *p && t.n < 4u;) {
      char* end = nullptr;
      const unsigned long x = std::strtoul(p, &end, 16);
      if (end == p) break;
      t.phys[t.n++] = uint32_t(x) & ~3u;
      // ',' or ';' between targets (';' inside perf_run --gates, which splits on ',')
      if (*end != ',' && *end != ';') break;
      p = end + 1;
    }
    if (const char* g = std::getenv("SR_MEM_RES_DWATCH_GRANT")) t.grantBytes = uint32_t(std::strtoul(g, nullptr, 16));
    return t;
  }();
  return v;
}
uint32_t dwatchTarget() { return dwatchTargets().n ? dwatchTargets().phys[0] : 0xFFFFFFFFu; }
bool dwatchOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_MEM_RES_DWATCH");
    return e && *e == '1' && dwatchTarget() == 0xFFFFFFFFu;
  }();
  return v;
}
#if defined(_WIN32)
LONG WINAPI dwatchVeh(EXCEPTION_POINTERS* info) {
  if (!info || !info->ExceptionRecord || !info->ContextRecord ||
      info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(info->ContextRecord->Dr6 & 1u))
    return EXCEPTION_CONTINUE_SEARCH;
  info->ContextRecord->Dr6 = 0;
  const uint32_t n = g_dwHits.fetch_add(1, std::memory_order_acq_rel);
  if (n < kDwHits) {
    DwHit& h = g_dwHit[n];
    const uint64_t eb = g_exeBase.load(std::memory_order_relaxed), rip = info->ContextRecord->Rip;
    h.rip.store(eb && rip >= eb && rip - eb < 0x40000000ull ? rip - eb : (rip | (1ull << 63)), std::memory_order_relaxed);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(g_dwHost.load(std::memory_order_relaxed));
    h.value.store(p ? (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]) : 0u,
                  std::memory_order_relaxed);
    h.lr.store(sr::kernel::getCurrentGuestPC(), std::memory_order_relaxed);
    h.ctr.store(sr::kernel::getCurrentGuestCtr(), std::memory_order_relaxed);
    h.r1.store(sr::kernel::getCurrentGuestR1(), std::memory_order_relaxed);
    h.tid.store(uint32_t(GetCurrentThreadId()), std::memory_order_relaxed);
    h.ready.store(true, std::memory_order_release);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}
DWORD WINAPI dwatchArmThread(LPVOID) {
  const uintptr_t addr = g_dwHost.load(std::memory_order_acquire);
  const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    g_dwArmFail.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  THREADENTRY32 te{};
  te.dwSize = sizeof te;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
    if (!t) {
      g_dwArmFail.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (SuspendThread(t) != DWORD(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(t, &c)) {
        c.Dr0 = addr;
        c.Dr7 = (c.Dr7 & ~uint64_t(0xF0003)) | 1u | (1u << 16) | (3u << 18);  // L0, RW0 = write, LEN0 = 4 bytes
        if (SetThreadContext(t, &c)) g_dwThreads.fetch_add(1, std::memory_order_relaxed);
        else g_dwArmFail.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_dwArmFail.fetch_add(1, std::memory_order_relaxed);
      }
      ResumeThread(t);
    }
    CloseHandle(t);
  }
  CloseHandle(snap);
  return 0;
}
#endif
void dwatchArm(uint32_t page, uint32_t at, const uint8_t* h, bool a, bool targeted = false) noexcept {
  if ((!targeted && !dwatchOn()) || !h || g_dwArmed.exchange(true, std::memory_order_acq_rel)) return;
  g_dwPage.store(page, std::memory_order_relaxed);
  g_dwAt.store(at, std::memory_order_relaxed);
  g_dwVa.store((a ? 0xA0000000u : 0x80000000u) + (page << kPageLog2) + at, std::memory_order_relaxed);
  g_dwPhys.store((page << kPageLog2) + at, std::memory_order_release);
  g_markTarget.store((page << kPageLog2) + at, std::memory_order_relaxed);  // the marks of it are logged (MARKW)
  g_dwHost.store(reinterpret_cast<uintptr_t>(h + at), std::memory_order_release);
#if defined(_WIN32)
  if (!g_exeBase.load(std::memory_order_relaxed))
    g_exeBase.store(uint64_t(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))), std::memory_order_relaxed);
  if (!AddVectoredExceptionHandler(sr::kVehFirst, dwatchVeh)) {
    g_dwArmFail.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  HANDLE t = CreateThread(nullptr, 0, dwatchArmThread, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
  else g_dwArmFail.fetch_add(1, std::memory_order_relaxed);
#endif
}
void dwatchNoted(uint32_t phys, uint32_t set) noexcept {
  if (g_dwPhys.load(std::memory_order_relaxed) == (phys & ~3u)) g_dwNoted[set & 1u].fetch_add(1, std::memory_order_relaxed);
}
// The watch's lines: the arming once, each recorded write once.
void dwatchLines(std::vector<std::string>& out) {
  if (!g_dwArmed.load(std::memory_order_acquire)) return;
  char b[320];
  const uint32_t hits = g_dwHits.load(std::memory_order_acquire);
  std::snprintf(b, sizeof b, "[MEM_RES] DWATCH armed va=%08X phys=%08X threads=%u fail=%u hits=%u noted8=%llu notedA=%llu",
                g_dwVa.load(), g_dwPhys.load(), g_dwThreads.load(), g_dwArmFail.load(), hits,
                (unsigned long long)g_dwNoted[0].load(), (unsigned long long)g_dwNoted[1].load());
  out.push_back(b);
  for (uint32_t i = g_dwPrinted.load(std::memory_order_relaxed); i < std::min(hits, kDwHits); i++) {
    const DwHit& h = g_dwHit[i];
    if (!h.ready.load(std::memory_order_acquire)) break;
    const uint64_t rip = h.rip.load();
    std::snprintf(b, sizeof b, "[MEM_RES] DWATCH #%u value=%08X rip=%s%llX lr=%08X ctr=%08X r1=%08X tid=%u", i + 1,
                  h.value.load(), (rip >> 63) ? "abs+0x" : "exe+0x",
                  (unsigned long long)(rip & ~(1ull << 63)), h.lr.load(), h.ctr.load(), h.r1.load(), h.tid.load());
    out.push_back(b);
    g_dwPrinted.store(i + 1, std::memory_order_relaxed);
  }
}

// A miss outside the command buffer: the first 8, then the powers of two of that count arm a trap on the page (once
// per page; at most trapLimit() traps).
void armTrap(State& s, uint32_t page, uint32_t kind, uint32_t src, uint8_t on) noexcept;
void maybeArm(State& s, uint32_t page, InCb cb, uint32_t kind, uint32_t src, uint8_t on) noexcept {
  if (cb == InCb::kIn) return;
  const uint32_t n = s.nonCbMissed.fetch_add(1, std::memory_order_relaxed) + 1u;
  if (n > 8u && (n & (n - 1u)) != 0u) return;
  armTrap(s, page, kind, src, on);
}
// One trap on the page's two window copies (memres4: `on` = what armed it - a missed / page-only decision, an ALIAS).
void armTrap(State& s, uint32_t page, uint32_t kind, uint32_t src, uint8_t on) noexcept {
  if (s.trapSt[page].load(std::memory_order_relaxed) != 0) return;  // a page is trapped once
  const uint32_t lim = uint32_t(std::max(0, trapLimit()));
  if (s.nTraps.load(std::memory_order_relaxed) >= std::min(lim, kMaxTraps)) {
    add(s, kOffTrapDeclined);
    return;
  }
  uint8_t none = 0;
  if (!s.trapSt[page].compare_exchange_strong(none, kTrapArmed, std::memory_order_acq_rel)) return;
  const uint32_t i = s.nTraps.fetch_add(1, std::memory_order_acq_rel);
  if (i >= std::min(lim, kMaxTraps)) {  // another thread took the last slot
    s.trapSt[page].store(0, std::memory_order_release);
    add(s, kOffTrapDeclined);
    return;
  }
  State::Trap& t = s.trap[i];
  t.page.store(page, std::memory_order_relaxed);
  t.kind.store(kind, std::memory_order_relaxed);
  t.src.store(src, std::memory_order_relaxed);
  t.on.store(on, std::memory_order_relaxed);
  t.armClock.store(s.reg.clock(), std::memory_order_relaxed);
  t.fired.store(false, std::memory_order_relaxed);
  t.printed.store(false, std::memory_order_relaxed);
  t.ready.store(true, std::memory_order_release);
  if (g_protect.load(std::memory_order_relaxed)(page, true)) {
    add(s, kOffTrapArmed);
  } else {
    t.ready.store(false, std::memory_order_release);
    s.trapSt[page].store(0, std::memory_order_release);
    add(s, kOffTrapArmFail);
  }
}

#if defined(_WIN32)
// The census's vectored handler (installed with the first trap, ahead of the write watch's): a write fault on a trapped
// page records the writer and releases the page. A page the write watch also protects is left to its handler (it
// releases the whole block); a write to a page another thread's fire already released is retried.
LONG WINAPI trapVeh(EXCEPTION_POINTERS* info) {
  if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
  const EXCEPTION_RECORD* er = info->ExceptionRecord;
  if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] != 1)
    return EXCEPTION_CONTINUE_SEARCH;
  uint8_t* b = g_guestBase.load(std::memory_order_relaxed);
  State* s = g_state;
  const uintptr_t addr = er->ExceptionInformation[1];
  if (!b || !s || addr < uintptr_t(b) + 0x80000000ull || addr >= uintptr_t(b) + 0xC0000000ull)
    return EXCEPTION_CONTINUE_SEARCH;
  const uint32_t va = uint32_t(addr - uintptr_t(b));
  const uint32_t page = (va & 0x1FFFFFFFu) >> kPageLog2;
  if (s->trapSt[page].load(std::memory_order_acquire) == 0) return EXCEPTION_CONTINUE_SEARCH;
  trapFire(*s, page, va, info->ContextRecord ? uint64_t(info->ContextRecord->Rip) : 0u, false);
  if (sr::kernel::gww::protGen(page) != 0) return EXCEPTION_CONTINUE_SEARCH;  // the write watch's release
  auto writable = [addr] {
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
           (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD);
  };
  // A page only a trap ever protected: another thread's fire may still be on its way to the release (this write came
  // second) - release it here too (idempotent) rather than let the fault go unhandled.
  if (!writable()) g_protect.load(std::memory_order_relaxed)(page, false);
  return writable() ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
#endif

// The guest ranges a texture fetch constant's texture occupies: the base level times its layers (the stack depth field
// even when the stacked bit is clear - shmPrepRanges reads it so -, the six faces of a cube, the depth of a volume)
// and the mip chain at the mip address. The mip extent is the real layout's (cube_layout.hpp computeGuestLayout: each
// level at its own aligned stride, packed tails placed), bounded from above by the sum of the per-level slice strides
// (a refused layout still bounds it). Over-noting costs uploads (over); under-noting would be a miss.
uint32_t textureSpans(const uint32_t* f, Span out[2]) noexcept {
  namespace CL = sr::gfx::cube;
  namespace TL = sr::kernel::texlayout;
  const TL::FetchDesc fd = TL::decodeFetchConstant(f);
  // (beyond 8192 texels the layout refuses and the stride arithmetic would overflow: counted as unreadable - the page model
  // serves no texture that large either, texShmRanges refuses over 4096)
  if (!fd.valid || !fd.basePhys || fd.width > 8192u || fd.height > 8192u) return 0;
  const TL::BlockGeom geo = TL::blockGeomFor(fd.fmt);
  const uint32_t bw = geo.blockW ? geo.blockW : 1u, bb = geo.blockBytes ? geo.blockBytes : 4u;
  const uint32_t pitch = std::max(fd.pitchTexels, fd.width);
  uint32_t layers = std::max(fd.depth, 1u);
  if (fd.dim == TL::TexDim::k2DOrStacked) layers = std::max(layers, ((f[2] >> 26) & 0x3Fu) + 1u);
  const uint64_t stride0 =
      CL::detail::levelSliceStride(0, true, fd.width, fd.height, pitch, bw, bw, bb, fd.tiled);
  uint64_t baseLen = stride0 * layers;
  const uint32_t mipPage = (f[5] >> 12) & 0xFFFFFu;
  const bool packed = ((f[5] >> 11) & 1u) != 0;
  const uint32_t sizeMax = CL::detail::log2Floor(std::max(fd.width, fd.height));
  const uint32_t mipMax = mipPage ? std::min(fd.mipMax, sizeMax) : 0u;
  uint64_t mipLen = 0;
  if (mipMax > 0u || packed) {
    const CL::CubeLayout L = CL::computeGuestLayout(fd.fmt, fd.width, fd.height, pitch, fd.tiled, mipMax + 1u, packed,
                                                    bw, bw, bb, layers);
    if (L.ok) {
      baseLen = std::max<uint64_t>(baseLen, L.baseSpanBytes);
      if (mipMax > 0u) mipLen = L.mipsTotalExtentBytes;
    }
    if (mipMax > 0u) {
      // packed_level 0 stores the mip tail as level 0 of the mip region (cube_layout.hpp): its stride counts too
      const uint32_t from = (packed && CL::detail::packedMipLevel(fd.width, fd.height) == 0u) ? 0u : 1u;
      uint64_t sum = 0;
      for (uint32_t l = from; l <= mipMax; l++)
        sum += uint64_t(CL::detail::levelSliceStride(l, false, fd.width, fd.height, pitch, bw, bw, bb, fd.tiled)) *
               layers;
      mipLen = std::max(mipLen, sum);
    }
  }
  uint32_t n = 0;
  out[n++] = Span{fd.basePhys & 0x1FFFFFFFu, uint32_t(std::min<uint64_t>(baseLen, kPhysBytes))};
  if (mipLen) out[n++] = Span{(mipPage << 12) & 0x1FFFFFFFu, uint32_t(std::min<uint64_t>(mipLen, kPhysBytes))};
  return n;
}

// Open (+1) or close (-1) the guest write window over a texture's spans.
void lockWindow(State& s, const std::vector<Span>& v, int dir) noexcept {
  for (const Span& sp : v) {
    uint32_t p0, p1;
    if (!Registry::pageRange(sp.phys, sp.len, p0, p1)) continue;
    for (uint32_t p = p0; p <= p1; p++) {
      if (dir > 0) {
        s.lockOpen[p].fetch_add(1, std::memory_order_relaxed);
        add(s, kOffLockOpenPages);
      } else {
        uint16_t cur = s.lockOpen[p].load(std::memory_order_relaxed);
        while (cur && !s.lockOpen[p].compare_exchange_weak(cur, uint16_t(cur - 1u), std::memory_order_relaxed)) {
        }
        if (cur) s.n[kOffLockOpenPages].fetch_sub(1, std::memory_order_relaxed);
      }
    }
  }
}

bool movieMarked(State& s, uint32_t page) noexcept {
  const uint32_t m = s.movie[page].load(std::memory_order_relaxed);
  return m != 0u && m == g_ownerVer.load(std::memory_order_relaxed)(page << kPageLog2);
}

bool zeroPage(const uint8_t* h) noexcept {
  static const uint8_t z[4096] = {};
  return std::memcmp(h, z, 4096) == 0;
}

// THE SOURCE WINDOW of a registry upload: the registry's window (memres5's rule: the live owner, else the last grant,
// else 0x8 - the registry has no zero-page heuristic) against the window the model served / validated the page from.
// memres6: a model KEEP serves its resident copy - the window of its last upload of the page (mWin, recorded at every
// model upload), not the keep site's label (the legacy path labels every keep l8: memres5's Warworld ALIAS samples).
void windowCheck(State& s, uint32_t page, Prov prov, Kind kind, Site site, Outcome o, Src src, bool modelUp) noexcept {
  auto host = g_host.load(std::memory_order_relaxed);
  const uint8_t* h8 = host(0x80000000u + (page << kPageLog2));
  const uint8_t* hA = host(0xA0000000u + (page << kPageLog2));
  uint32_t rule = 2;
  const bool regA = regIsA(s, page, prov, &rule);
  add(s, kOffWinLive + rule);
  bool modelA;
  switch (prov) {
    case Prov::kA:
    case Prov::kLegacyA: modelA = true; break;
    case Prov::k8:
    case Prov::kLegacy8: modelA = false; break;
    default: modelA = h8 && hA && zeroPage(h8) && !zeroPage(hA); break;  // a keep under the legacy rule: its window
  }
  const uint8_t mLast = s.mWin[page].load(std::memory_order_relaxed);
  if (!modelUp && mLast) modelA = mLast == 0xAu;
  if (regA == modelA) return;
  if (!h8 || !hA) {
    add(s, kOffWinUnmapped);
    return;
  }
  if (std::memcmp(h8, hA, 4096) == 0) {
    add(s, kOffWinAliasSame);
    return;
  }
  add(s, kOffWinAliasDiff);
  const uint32_t sx = uint32_t(src) < kSrcs ? uint32_t(src) : 0u;
  uint32_t n;
  if (sample(s.logAlias, n)) {
    // memres4: who holds the page - the last note's source, the grant, the allocators' records (phys_window.hpp)
    const uint32_t phys = page << kPageLog2, gi = grantOf(s, page);
    char gr[48] = "none";
    if (gi < kMaxGrants)
      std::snprintf(gr, sizeof gr, "%08X+%X@%08X", s.gVa[gi].load(std::memory_order_relaxed),
                    s.gBytes[gi].load(std::memory_order_relaxed), s.gLr[gi].load(std::memory_order_relaxed));
    // memres6: each copy zero (Z) or a 16-bit hash of it, the model's last upload window (8 / A / -)
    char z8[8], zA[8];
    auto pageTag = [](const uint8_t* h, char* b) {
      if (zeroPage(h)) {
        std::snprintf(b, 8, "Z");
        return;
      }
      uint32_t x = 2166136261u;
      for (uint32_t i = 0; i < 4096u; i += 4) x = (x ^ (uint32_t(h[i]) | (uint32_t(h[i + 1]) << 8))) * 16777619u;
      std::snprintf(b, 8, "%04X", (x ^ (x >> 16)) & 0xFFFFu);
    };
    pageTag(h8, z8);
    pageTag(hA, zA);
    char tail[220];
    std::snprintf(tail, sizeof tail,
                  " src=%s grant=%s pw_flags=%d pw_owner=%X pw_contested=%d reg_rule=%s h8=%s hA=%s mlast=%s",
                  srcName(Src(sx)), gr, sr::kernel::physwin::gpuGrantFlags(phys, 4096u),
                  unsigned(sr::kernel::physwin::physPageOwner(phys)),
                  sr::kernel::physwin::physPageContested(phys) ? 1 : 0,
                  rule == 0 ? "live" : rule == 1 ? "grant" : "default", z8, zA,
                  mLast == 0xAu ? "A" : mLast == 8u ? "8" : "-");
    keepSample(2, tail);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[MEM_RES] ALIAS #%u page=%08X kind=%s prov=%s site=%s outcome=%s reg_win=%X model_win=%X%s", n,
                phys, kindName(kind), provName(prov), siteName(site), outcomeName(o), regA ? 0xAu : 8u,
                modelA ? 0xAu : 8u, tail);
  }
  // memres4: the first 4 ALIAS decisions, then the powers of two of their count, trap the page's two window copies -
  // the WRITER's va names the window the CPU writes
  const uint32_t an = s.aliasSeen.fetch_add(1, std::memory_order_relaxed) + 1u;
  if (an <= 4u || (an & (an - 1u)) == 0u) armTrap(s, page, uint32_t(kind) < kKinds ? uint32_t(kind) : 0u, sx, kOnAlias);
}

// One decision event: the model's decision against the registry's, on the generations read before the model's bytes
// (snapped) or now; the registry commits as it would deciding alone (Registry::ensureAt).
// mem-native: the page's pending exact-write dwords (notePatch), as the native request handles them
// (gfx/d3d12_pm4_shmem.cpp pm4ResourceRequest): taken BEFORE the registry's copy is stored (a stamp landing after the
// take stays pending for the next decision, as natively); a keep's copy takes the dwords' bytes now, each from the
// window it was written through (before any comparison); an upload's copy holds the dwords of its own window, and those
// of the other window land over it after the copy. The page's generations do not move. Block: [0, 16) the dwords
// written through 0x8, [16, 32) through 0xA.
struct CensusPatch {
  uint64_t m[2][kDwWords];
  bool any;
};
bool takePatch(State& s, uint32_t page, CensusPatch& out) noexcept {
  out.any = false;
  std::atomic<uint64_t>* b = s.patchBlk[page].load(std::memory_order_acquire);
  if (!b) return false;
  for (uint32_t set = 0; set < 2u; set++)
    for (uint32_t w = 0; w < kDwWords; w++) {
      out.m[set][w] = b[set * kDwWords + w].exchange(0, std::memory_order_acq_rel);
      out.any = out.any || out.m[set][w] != 0;
    }
  return out.any;
}
// skipSet: the window set an upload's copy already holds (-1: none - a keep).
void applyPatch(State& s, uint32_t page, Decision d, const CensusPatch& p, int skipSet) noexcept {
  if (d == Decision::kKeepGpu) {  // the arena's GPU data takes the dwords (no copy is compared while it is the GPU's)
    s.patchGpu.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (isUpload(d)) s.patchUpload.fetch_add(1, std::memory_order_relaxed);
  std::atomic<uint16_t>* c = s.rhBlk[page].load(std::memory_order_acquire);
  uint64_t n = 0;
  bool noCopy = false;
  for (uint32_t set = 0; set < 2u; set++) {
    if (int(set) == skipSet) continue;
    bool anySet = false;
    for (uint32_t w = 0; w < kDwWords; w++) anySet = anySet || p.m[set][w] != 0;
    if (!anySet) continue;
    const uint8_t* h = g_host.load(std::memory_order_relaxed)((set ? 0xA0000000u : 0x80000000u) + (page << kPageLog2));
    if (!c || !h || !s.rhOk[page].load(std::memory_order_acquire)) {
      noCopy = true;
      continue;
    }
    for (uint32_t w = 0; w < kDwWords; w++)
      for (uint64_t bits = p.m[set][w]; bits; bits &= bits - 1u) {
        const uint32_t dw = w * 64u + uint32_t(__builtin_ctzll(bits));
        c[dw].store(dwHash(h + (dw << 2)), std::memory_order_relaxed);
        n++;
      }
  }
  if (noCopy) s.patchNoCopy.fetch_add(1, std::memory_order_relaxed);
  if (n) {
    s.patchApplied.fetch_add(1, std::memory_order_relaxed);
    s.patchAppliedDw.fetch_add(n, std::memory_order_relaxed);
  }
}

void decide(State& s, uint32_t page, bool modelUp, Why why, Prov prov, Kind kind, Site site, bool snapped,
            uint32_t cS, uint32_t gS) noexcept {
  const uint32_t cNow = s.reg.cpuGen(page), gNow = s.reg.gpuGen(page);
  const uint32_t c = snapped ? cS : cNow, g = snapped ? gS : gNow;
  const uint32_t u = s.reg.uploadedGen(page);
  const Src src = s.reg.lastSrc(page);
  const Decision d = s.reg.ensureAt(page, c, g);
  const bool regUp = isUpload(d);
  const bool gpuHeld = g != 0u && g > u;  // the arena holds the GPU's data: a GPU write since the last upload
  if (dwatchTargets().n && !g_dwArmed.load(std::memory_order_relaxed)) {
    const DwTargets& t = dwatchTargets();
    for (uint32_t i = 0; i < t.n; i++) {
      if (page != (t.phys[i] >> kPageLog2)) continue;
      if (t.grantBytes) {
        const uint32_t gi = grantOf(s, page);
        if (gi >= kMaxGrants || s.gBytes[gi].load(std::memory_order_relaxed) != t.grantBytes) continue;
      }
      dwatchArm(page, t.phys[i] & 0xFFFu, regBytes(s, page, prov), regIsA(s, page, prov, nullptr), true);
      break;
    }
  }
  ownerEpoch(s, page);  // mem-native: the current-version marks (missed_prior) are this tenancy's
  CensusPatch patch;
  const bool patched = takePatch(s, page, patch);  // before the copy below, as natively
  if (const auto hook = g_afterTake.load(std::memory_order_relaxed)) hook(page);  // tests: a stamp racing the decision
  if (patched && !regUp) applyPatch(s, page, d, patch, -1);  // a keep's copy takes the dwords before any comparison
  Outcome o;
  Why w = why == Why::kWindow ? Why::kWindow : Why::kContent;
  if (modelUp == regUp) {
    o = modelUp ? Outcome::kAgreeUp : Outcome::kAgreeKeep;
  } else if (regUp) {
    o = gpuHeld ? Outcome::kClobber : Outcome::kOver;
  } else {
    if (d == Decision::kKeepGpu) w = Why::kGpu;
    if (cNow > c) o = Outcome::kRaceNote;  // a note landed after the generations were read: it stays pending
    else if (s.lockOpen[page].load(std::memory_order_relaxed) != 0) o = Outcome::kRaceLock;
    else o = Outcome::kMissed;
  }
  // memres3: the fetched bytes against the registry's copy. A registry keep of RAM bytes (not the GPU's) that the model
  // re-uploaded is PAGE_ONLY when no dword any request ever fetched (memres5: per dword, a vertex fetch index-bounded)
  // moved since the registry's copy (then the page is diverged: the copy differs from RAM in unfetched bytes), MISSED
  // otherwise; while a page stays diverged, every keep re-checks the fetched dwords (the model's keeps too: a dword
  // fetched for the first time since may hold the change).
  Rng rg = Rng::kUnknown;
  uint32_t movedAt = 0xFFFFFFFFu;
  // mem-native: a moved fetched dword whose exact-write note is pending was noted after this decision's take - the
  // stamp raced the decision (race.note, census_patch race=); a moved dword with no pending note is still MISSED.
  if (o == Outcome::kMissed && d == Decision::kKeep) {
    bool pended = false;
    rg = fetchedClassUnpended(s, page, regBytes(s, page, prov), &movedAt, &pended);
    if (rg == Rng::kMoved) {
      // (the data watch arms below, on an unclassified miss only)
    } else if (pended) {
      o = Outcome::kRaceNote;
      rg = Rng::kUnknown;
      s.patchRace.fetch_add(1, std::memory_order_relaxed);
      s.diverged[page].store(1, std::memory_order_relaxed);  // later keeps re-check the fetched dwords
    } else if (rg == Rng::kSame) {
      o = Outcome::kPageOnly;
      s.diverged[page].store(1, std::memory_order_relaxed);
    }
  } else if (o == Outcome::kAgreeKeep && d == Decision::kKeep && s.diverged[page].load(std::memory_order_relaxed)) {
    bool pended = false;
    if (fetchedClassUnpended(s, page, regBytes(s, page, prov), &movedAt, &pended) == Rng::kMoved) {
      o = Outcome::kMissed;
      w = Why::kStale;
      rg = Rng::kMoved;
    } else if (pended) {
      o = Outcome::kRaceNote;
      s.patchRace.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (regUp) storeCopy(s, page, regBytes(s, page, prov), prov, c, g, u);  // original registry copy
  if (patched && regUp) applyPatch(s, page, d, patch, regIsA(s, page, prov, nullptr) ? 1 : 0);  // the other window's
  const uint32_t k = uint32_t(kind) < kKinds ? uint32_t(kind) : 0u;
  const uint32_t pv = uint32_t(prov) < kProvs ? uint32_t(prov) : uint32_t(Prov::kUndecided);
  const uint32_t si = uint32_t(site) < kSites ? uint32_t(site) : 0u;
  const uint32_t sx = uint32_t(src) < kSrcs ? uint32_t(src) : 0u;
  const bool movie = o == Outcome::kMissed && movieMarked(s, page);
  const bool cpuOnly = o == Outcome::kMissed && !movie && movedAllCpuOnly(s, page, regBytes(s, page, prov));
  if (cpuOnly) s.missedCpuOnly.fetch_add(1, std::memory_order_relaxed);
  const bool prior = o == Outcome::kMissed && !movie && !cpuOnly && movedAllPrior(s, page, regBytes(s, page, prov));
  if (prior) s.missedPrior.fetch_add(1, std::memory_order_relaxed);
  // mem-native: an unclassified miss (not movie / cpu_only / prior) - the data watch's first target, every sample logged
  const bool unclassified = o == Outcome::kMissed && !movie && !cpuOnly && !prior;
  if (unclassified && rg == Rng::kMoved && movedAt != 0xFFFFFFFFu)
    dwatchArm(page, movedAt, regBytes(s, page, prov), regIsA(s, page, prov, nullptr));
  add(s, cellIx(uint32_t(o), k, pv, sx, si, uint32_t(w), movie ? 1u : 0u));
  s.lastDecCpu[page].store(c, std::memory_order_relaxed);
  if (d == Decision::kUploadFirst) {
    add(s, kOffFirst);
    add(s, kOffFirstBy + k * kProvs + pv);
  }
  if (regUp && modelUp && gpuHeld) add(s, kOffClobberBoth);
  if (prov == Prov::kLegacyA) add(s, kOffWinHeur);
  if (regUp) windowCheck(s, page, prov, kind, site, o, src, modelUp);
  // memres6: the window of the model's upload (its resident copy until the next one)
  if (modelUp) {
    const uint8_t w8 = prov == Prov::kA || prov == Prov::kLegacyA ? 0xAu
                       : prov == Prov::k8 || prov == Prov::kLegacy8 ? 8u : 0u;
    s.mWin[page].store(w8, std::memory_order_relaxed);
  }
  // memres2: the attributes and the grant of a missed or page-only decision (rng same = exactly the page-only ones).
  InCb cb = InCb::kUnknown;
  Seg sg = Seg::kNone;
  uint32_t gi = kMaxGrants, ax = 0;
  const bool attr = o == Outcome::kMissed || o == Outcome::kPageOnly;
  if (attr) {
    cb = inCb(s, page);
    const uint8_t ut = s.upTag[page].load(std::memory_order_relaxed);
    sg = ut < kSegs ? Seg(ut) : Seg::kNone;
    gi = grantOf(s, page);
    ax = attrIx(k, sx, uint32_t(cb), uint32_t(sg), uint32_t(rg));
    add(s, kOffMissedAttr + ax);
    add(s, kOffMissedGrant + gi);
  }
  uint32_t n;
  if (attr) {
    const bool po = o == Outcome::kPageOnly;
    n = (po ? s.logPageOnly : s.logMissed).fetch_add(1, std::memory_order_relaxed) + 1u;
    if (!po && unclassified && kind == Kind::kVb && site == Site::kDrain && rg == Rng::kMoved &&
        movedAt < (1u << kPageLog2) && (movedAt & 3u) == 0 && t_fetchSequence && fetchAutoTargetMode()) {
      std::lock_guard<std::mutex> payloadSelection(g_payloadSelection);
      if (g_fetchTarget.canSelect()) {
      FetchTargetOrigin origin;
      origin.sequence = t_fetchSequence; origin.ordinal = t_fetchOrdinal;
      origin.target = (page << kPageLog2) + movedAt; origin.missed = n;
      origin.cpu = c; origin.gpu = g; origin.uploaded = u; origin.clock = s.reg.clock();
      origin.epoch = s.stampColl[page].load(std::memory_order_relaxed);
      // Only atomic identity reads here. The live-grant lookup is deferred until
      // the decision scope exits, outside any census/request locks.
      origin.grantGeneration = sr::kernel::physwin::registryGeneration();
      const auto owner = sr::kernel::physwin::ownerVersion(origin.target, 4);
      origin.ownerSum = owner.sum; origin.ownerSeq = owner.cbSeq;
      origin.known = owner.ok && origin.grantGeneration == sr::kernel::physwin::registryGeneration();
      try { origin.payload = capturePagePayload(s, page, prov, movedAt); }
      catch (...) { payloadRuntime().artifact.store(2); }
      if (gi < kMaxGrants) {
        origin.censusGrantVa = s.gVa[gi].load(std::memory_order_relaxed);
        origin.censusGrantBytes = s.gBytes[gi].load(std::memory_order_relaxed);
        origin.censusGrantLr = s.gLr[gi].load(std::memory_order_relaxed);
      }
      if (g_fetchTarget.select(origin))
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[MEM_RES_FETCH] target_selected=%08X missed=%u origin_sequence=%llu ordinal=%u epoch=%u "
            "cpu=%u gpu=%u uploaded=%u clock=%u census_grant=%08X+%X@%08X owner=%llu/%u/%u "
            "generation=%llu origin_kind=%s scope=first_post_hud_moved_vb_miss causing_draw=excluded epoch_coverage=unknown",
            origin.target, n, (unsigned long long)origin.sequence, origin.ordinal, origin.epoch,
            c, g, u, origin.clock, origin.censusGrantVa, origin.censusGrantBytes, origin.censusGrantLr,
            (unsigned long long)owner.sum, owner.cbSeq, origin.known ? 1u : 0u,
            (unsigned long long)origin.grantGeneration, origin.ordinal ? "command" : "validation_batch");
      }
    }
    const uint32_t cn = s.attrSeen[ax].fetch_add(1, std::memory_order_relaxed) + 1u;  // the per-cell samples
    // the per-cell samples (the first 4, then the powers of two); mem-native: every unclassified miss too (64)
    if (cn <= 4u || (cn & (cn - 1u)) == 0u ||
        (unclassified && s.logUnclassified.fetch_add(1, std::memory_order_relaxed) < 64u)) {
      char gr[48] = "none";
      if (gi < kMaxGrants)
        std::snprintf(gr, sizeof gr, "%08X+%X@%08X", s.gVa[gi].load(std::memory_order_relaxed),
                      s.gBytes[gi].load(std::memory_order_relaxed), s.gLr[gi].load(std::memory_order_relaxed));
      char mv[24] = "-";
      if (movedAt != 0xFFFFFFFFu) std::snprintf(mv, sizeof mv, "%03X", movedAt);
      char tail[400];  // memres4: the chunk the decision turns on, its fetched dwords, its bytes, the ranges covering it
      sampleTail(s, page, regBytes(s, page, prov), movedAt, tail, sizeof tail);
      keepSample(po ? 1 : 0, tail);
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[MEM_RES] %s #%u page=%08X kind=%s prov=%s src=%s site=%s why=%s reg=%s cpu=%u gpu=%u up=%u "
                  "clock=%u movie=%d prior=%d cpu_only=%d cb=%s seg=%s rng=%s moved=%s grant=%s cell=%u%s",
                  po ? "PAGE_ONLY" : "MISSED", n, page << kPageLog2, kindName(kind), provName(prov), srcName(src),
                  siteName(site),
                  w == Why::kGpu ? "gpu" : w == Why::kWindow ? "window" : w == Why::kStale ? "stale" : "content",
                  d == Decision::kKeepGpu ? "keep_gpu" : "keep", c, g, u, s.reg.clock(), movie ? 1 : 0, prior ? 1 : 0,
                  cpuOnly ? 1 : 0, inCbName(cb),
                  segName(sg), rngName(rg), mv, gr, cn, tail);
    }
    maybeArm(s, page, cb, k, sx, po ? kOnPageOnly : kOnMissed);
  } else if (o == Outcome::kClobber && sample(s.logClobber, n))
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[MEM_RES] CLOBBER #%u page=%08X kind=%s prov=%s src=%s site=%s cpu=%u gpu=%u up=%u clock=%u", n,
                page << kPageLog2, kindName(kind), provName(prov), srcName(src), siteName(site), c, g, u,
                s.reg.clock());
  else if ((o == Outcome::kRaceNote || o == Outcome::kRaceLock) && s.logRace.load(std::memory_order_relaxed) < 16u &&
           sample(s.logRace, n))
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MEM_RES] RACE #%u page=%08X kind=%s site=%s %s cpu=%u now=%u lock=%u", n,
                page << kPageLog2, kindName(kind), siteName(site), outcomeName(o), c, cNow,
                unsigned(s.lockOpen[page].load(std::memory_order_relaxed)));
}

void tagStamp(State& s, uint32_t page, Site site, uint32_t stamp, bool cpStamp) noexcept {
  s.kind[page].store(t_kind, std::memory_order_relaxed);
  s.site[page].store(uint8_t(site), std::memory_order_relaxed);
  (cpStamp ? s.stampColl : s.stampRend)[page].store(stamp, std::memory_order_relaxed);
  s.stampFailed[page].store(0, std::memory_order_relaxed);
}

void appendKv(std::string& out, const char* key, uint64_t v) {
  char b[96];
  std::snprintf(b, sizeof b, " %s=%llu", key, (unsigned long long)v);
  out += b;
}
void appendKv(std::string& out, const std::string& key, uint64_t v) { appendKv(out, key.c_str(), v); }

}  // namespace

bool FetchTargetLatch::arm(const char* path) {
  if (!path || std::strcmp(path, "screens/hud/main_hud") != 0) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (armed_) return false;
  armed_ = true;
  return true;
}
bool FetchTargetLatch::select(const FetchTargetOrigin& origin) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!armed_ || selected_ || !origin.sequence || origin.target > 0x1FFFFFFCu || (origin.target & 3u)) return false;
  origin_ = origin; selected_ = true;
  return true;
}
bool FetchTargetLatch::canSelect() {
  std::lock_guard<std::mutex> lock(mutex_);
  return armed_ && !selected_;
}
bool FetchTargetLatch::pending(uint64_t sequence, FetchTargetOrigin& origin) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!selected_ || finalized_ || sequence != origin_.sequence) return false;
  origin = origin_;
  return true;
}
bool FetchTargetLatch::finalize(const FetchTargetOrigin& origin) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!selected_ || finalized_ || origin.sequence != origin_.sequence || origin.target != origin_.target) return false;
  origin_.known = origin_.known && origin.known; origin_.reason = origin.reason;
  origin_.grantCount = origin.grantCount; origin_.grantDigest = origin.grantDigest;
  std::copy_n(origin.serial, 2, origin_.serial);
  origin_.payload = origin.payload;
  finalized_ = true;
  return true;
}
bool FetchTargetLatch::later(uint64_t sequence, FetchTargetOrigin& origin) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!finalized_ || sequence <= origin_.sequence) return false;
  origin = origin_;
  return true;
}
bool fetchAutoTargetMode() noexcept {
  static const bool on = [] {
    const char* enable = std::getenv("SR_MEM_RES_FETCH_CAPTURE");
    const char* target = std::getenv("SR_MEM_RES_FETCH_TARGET");
    return enable && std::strcmp(enable, "1") == 0 && target && std::strcmp(target, "first_missed_after_hud") == 0;
  }();
  return on;
}
void noteFetchHudRequest(uint32_t pathVA, uint8_t* base) noexcept {
  if (!fetchAutoTargetMode() || !census() || !base) return;
  constexpr char expected[] = "screens/hud/main_hud";
  if (pathVA < 0x10000u || uint64_t(pathVA) + sizeof(expected) > sr::kernel::getGuestSize() ||
      !sr::kernel::hostRangeReadable(base + pathVA, sizeof(expected))) return;
  if (std::memcmp(base + pathVA, expected, sizeof(expected)) == 0 && g_fetchTarget.arm(expected))
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[MEM_RES_FETCH] armed=1 boundary=exact_main_hud next_moved_vb_miss=unselected epoch_coverage=unknown");
}
FetchAdmissionScope::FetchAdmissionScope(uint32_t ordinal)
    : previousSequence_(t_fetchSequence), previousOrdinal_(t_fetchOrdinal) {
  if (!fetchAutoTargetMode() || !census()) return;
  uint64_t old = g_fetchSequence.load(std::memory_order_relaxed);
  do {
    if (old == UINT64_MAX) throw std::runtime_error("consumed-fetch admission sequence exhausted");
  } while (!g_fetchSequence.compare_exchange_weak(old, old + 1u, std::memory_order_relaxed));
  t_fetchSequence = sequence_ = old + 1u;
  t_fetchOrdinal = ordinal;
}
FetchAdmissionScope::~FetchAdmissionScope() {
  FetchTargetOrigin origin;
  if (sequence_ && g_fetchTarget.pending(sequence_, origin)) {
    origin.reason = "origin_changed_or_unknown";
    try {
      const auto ownerBefore = sr::kernel::physwin::ownerVersion(origin.target, 4);
      const auto grants = sr::kernel::physwin::ownersOfSpan(origin.target, 4);
      const auto ownerAfter = sr::kernel::physwin::ownerVersion(origin.target, 4);
      origin.known = origin.known && ownerBefore.ok && ownerAfter.ok &&
          ownerBefore.sum == origin.ownerSum && ownerAfter.sum == origin.ownerSum &&
          ownerBefore.cbSeq == origin.ownerSeq && ownerAfter.cbSeq == origin.ownerSeq &&
          grants.nLive != 0 && grants.gen == origin.grantGeneration &&
          sr::kernel::physwin::registryGeneration() == origin.grantGeneration;
      origin.grantCount = grants.nLive; origin.grantDigest = grants.setDigest;
      std::copy_n(grants.serial, 2, origin.serial);
      if (origin.known) origin.reason = "origin_confirmed";
    } catch (...) { origin.known = false; origin.reason = "origin_lookup_failed"; }
    if (origin.payload) {
      try {
        auto payload = std::make_shared<FetchPagePayload>(*origin.payload);
        const auto before = sr::kernel::physwin::ownerVersion(payload->page, 4096);
        const auto grants = sr::kernel::physwin::ownersOfSpan(payload->page, 4096);
        const auto window = sr::kernel::physwin::gpuWindow(payload->page, 4096);
        const auto after = sr::kernel::physwin::ownerVersion(payload->page, 4096);
        const auto current = payloadVersion(st(), payload->page >> 12,
            payload->current.window == 0xAu ? Prov::kA : Prov::k8);
        const bool confirmed = before.ok && after.ok && before.sum == payload->current.ownerSum &&
            after.sum == before.sum && before.cbSeq == payload->current.ownerSeq && after.cbSeq == before.cbSeq &&
            grants.nLive == 1 && window == int(payload->current.window) &&
            grants.gen == payload->current.grantGeneration &&
            sr::kernel::physwin::registryGeneration() == grants.gen && samePayloadVersion(current, payload->current);
        payload->grantDigest = grants.setDigest; payload->grantCount = grants.nLive;
        std::copy_n(grants.serial, 2, payload->grantSerial);
        if (!confirmed && payload->known) {
          payload->known = false; payload->reason = "full_page_grant_changed_or_unsupported";
        }
        origin.payload = payload;
        if (!publishPayload(origin)) { payload->known = false; payload->reason = "payload_artifact_incomplete"; }
      } catch (...) {
        origin.payload.reset(); payloadRuntime().artifact.store(2);
      }
    }
    g_fetchTarget.finalize(origin);  // Unknown first selections are retained, never silently replaced.
    SR_LOG_BOOT(sr::LogSeverity::Info,
        "[MEM_RES_FETCH] origin_finalized=%llu target=%08X known=%u reason=%s grants=%u/%u/%u digest=%llu generation=%llu",
        (unsigned long long)origin.sequence, origin.target, origin.known ? 1u : 0u, origin.reason,
        origin.grantCount, origin.serial[0], origin.serial[1], (unsigned long long)origin.grantDigest,
        (unsigned long long)origin.grantGeneration);
  }
  t_fetchSequence = previousSequence_; t_fetchOrdinal = previousOrdinal_;
}
bool fetchTargetForCurrentDraw(FetchTargetOrigin& origin) {
  return t_fetchSequence && g_fetchTarget.later(t_fetchSequence, origin);
}

struct FetchCapture {
  uint64_t drawId = 0, vsHash = 0;
  std::string reason;
  VsInfo vs;
  std::vector<uint32_t> vertices;
  std::vector<uint32_t> submittedIndices;
  uint32_t constants[1024] = {}, effectiveConstants[1024] = {}, fetch[192] = {};
  uint32_t indexCount = 0, firstVertex = 0, constantBlocks = 0;
  bool indexed = false, stripCut = false;
  FetchSpan f1;
};

namespace {
constexpr uint32_t kCaptureVertices = 4096, kCaptureUses = 65536, kCaptureF1Bytes = 1u << 20;
// Consumer leases charge 64 KiB per immutable capture, separately from page
// baselines. Both index vectors reserve at most the explicit vertex bound.
static_assert(sizeof(FetchCapture) + 2u * kCaptureVertices * sizeof(uint32_t) + 1024u <= 65536u);

bool capturedRegs(const FetchCapture& c, uint32_t vertex, IRegs& r) noexcept {
  r = IRegs{};
  r.r[0][0] = Iv{double(vertex), double(vertex)};
  for (uint32_t i = 0; i < c.vs.prog.n; ++i) {
    const auto* d = c.vs.prog.ins[i];
    if (!ivStep(r, c.effectiveConstants, decodeAlu(d[0], d[1], d[2]))) return false;
  }
  return true;
}

bool capturedIndices(Iv value, bool rounded, int64_t& lo, int64_t& hi) noexcept {
  return ivIndex(value, rounded, lo, hi) && hi >= lo && hi - lo < int64_t(kFamIdxSpan);
}

bool capturedOffset(const FetchCapture& c, uint32_t slot, uint32_t stride, int32_t offset,
                    uint32_t width, int64_t index, uint32_t& phys) noexcept {
  if (slot >= 96 || !stride || !width || index < 0 || index > 0xFFFFFF) return false;
  const uint32_t d0 = c.fetch[slot * 2], d1 = c.fetch[slot * 2 + 1];
  if ((d0 & 3u) != 3u) return false;
  const uint32_t base = d0 & 0x1FFFFFFCu, size = ((d1 >> 2) & 0xFFFFFFu) * 4u;
  const int64_t start = (index * stride + offset) * 4;
  const uint64_t len = uint64_t(width) * 4u;
  if (start < 0 || uint64_t(start) + len > size || uint64_t(base) + start + len > 0x20000000ull)
    return false;
  phys = base + uint32_t(start);
  return true;
}
}  // namespace

std::shared_ptr<const FetchCapture> captureFetchDraw(const FetchCaptureInput& input) {
  auto c = std::make_shared<FetchCapture>();
  c->drawId = input.drawId;
  c->vsHash = input.vsHash;
  auto refuse = [&](const char* reason) -> std::shared_ptr<const FetchCapture> {
    c->reason = reason;
    return c;
  };
  if (!input.drawId || !input.vsHash) return refuse("capture-identity");
  if (input.constantBlocks & ~0xFFFFu) return refuse("constant-mask");
  if (!input.indexCount || input.indexCount > kCaptureVertices) return refuse("vertex-budget");
  if (!input.fetch || !input.constants || (input.indexed && !input.indices)) return refuse("capture-input");
  if (resourceConsumerEnabled()) {
    auto& shaders = consumerShaders();
    std::lock_guard<std::mutex> lock(shaders.mutex);
    const auto end = shaders.entries.begin() + shaders.count;
    const auto it = std::find_if(shaders.entries.begin(), end,
        [&](const ConsumerShaderEntry& entry) { return entry.hash == input.vsHash; });
    if (it == end) return refuse("consumer-shader-unavailable");
    c->vs = it->info;
  } else {
    State& s = st();
    std::lock_guard<std::mutex> lock(s.vsMu);
    const auto it = s.vs.find(input.vsHash);
    if (it == s.vs.end()) return refuse("shader-unavailable");
    c->vs = it->second;
  }
  if (!c->vs.ok || !c->vs.fetchComplete || !c->vs.nFetch) return refuse("shader-unsupported");
  for (uint32_t i = 0; i < c->vs.n; ++i)
    if (!c->vs.s[i].safe) return refuse("shader-unsupported");
  bool needsF1 = false;
  for (uint32_t i = 0; i < c->vs.nFetch; ++i) {
    const auto& f = c->vs.fetch[i];
    if (!f.safe || (f.cls != kClsD && f.cls != kClsL1 && f.cls != kClsL2))
      return refuse("shader-unsupported");
    if (f.cls != kClsD && !c->vs.prog.on) return refuse("shader-prologue");
    needsF1 |= f.cls == kClsL2;
  }
  std::memcpy(c->constants, input.constants, sizeof c->constants);
  std::memcpy(c->effectiveConstants, input.constants, sizeof c->effectiveConstants);
  std::memcpy(c->fetch, input.fetch, sizeof c->fetch);
  c->indexCount = input.indexCount;
  c->firstVertex = input.firstVertex;
  c->constantBlocks = input.constantBlocks;
  c->indexed = input.indexed;
  c->stripCut = input.stripCut;
  if (input.indexed) c->submittedIndices.assign(input.indices, input.indices + input.indexCount);
  const VsProg& g = c->vs.prog;
  bool validConstants = true;
  for (uint32_t i = 0; i < g.n; ++i)
    aluConstRegs(decodeAlu(g.ins[i][0], g.ins[i][1], g.ins[i][2]), [&](uint32_t reg) {
      bool literal = false;
      for (uint32_t k = 0; k < g.nLit; ++k) literal |= g.litReg[k] == reg;
      if (reg >= 256 || (!literal && !(input.constantBlocks & (1u << (reg / 16u))))) validConstants = false;
    });
  if (!validConstants) return refuse("constant-block");
  for (uint32_t i = 0; i < g.nLit; ++i)
    std::memcpy(c->effectiveConstants + g.litReg[i] * 4u, g.litV[i], sizeof g.litV[i]);
  c->vertices.reserve(input.indexCount);
  for (uint32_t i = 0; i < input.indexCount; ++i) {
    const uint64_t v = input.indexed ? input.indices[i] : uint64_t(input.firstVertex) + i;
    if (input.indexed && input.stripCut && v == UINT32_MAX) continue;
    if (v > 0xFFFFFFu) return refuse("index-not-normalized");
    c->vertices.push_back(uint32_t(v));
  }
  std::sort(c->vertices.begin(), c->vertices.end());
  c->vertices.erase(std::unique(c->vertices.begin(), c->vertices.end()), c->vertices.end());
  if (!needsF1 || c->vertices.empty()) return c;
  if (!g.f1FmtOk || g.f1Slot >= 96) return refuse("f1-format");
  uint32_t low = UINT32_MAX;
  uint64_t high = 0;
  for (uint32_t vertex : c->vertices) {
    IRegs r;
    if (!capturedRegs(*c, vertex, r)) return refuse("prologue-range");
    int64_t lo, hi;
    if (!capturedIndices(r.r[g.f1Src >> 2][g.f1Src & 3u], g.f1Rnd, lo, hi)) return refuse("f1-index-range");
    for (int64_t i = lo; i <= hi; ++i) {
      uint32_t phys;
      if (!capturedOffset(*c, g.f1Slot, g.f1Stride, g.f1Off, g.f1Comps, i, phys))
        return refuse("f1-extent");
      low = std::min(low, phys);
      high = std::max(high, uint64_t(phys) + 4u * g.f1Comps);
    }
  }
  if (!high || high - low > kCaptureF1Bytes) return refuse("f1-budget");
  c->f1 = FetchSpan{low, uint32_t(high - low)};
  return c;
}

bool confirmFetchDraw(const FetchCapture& c, const FetchCaptureInput& input) noexcept {
  if (!c.reason.empty() || !input.constants || !input.fetch || input.drawId != c.drawId ||
      input.vsHash != c.vsHash || input.indexed != c.indexed || input.indexCount != c.indexCount ||
      input.firstVertex != c.firstVertex || input.stripCut != c.stripCut ||
      input.constantBlocks != c.constantBlocks) return false;
  if (input.indexed && (!input.indices ||
      std::memcmp(input.indices, c.submittedIndices.data(), size_t(c.indexCount) * 4u))) return false;
  if (std::memcmp(input.fetch, c.fetch, sizeof c.fetch)) return false;
  for (uint32_t b = 0; b < 16; ++b)
    if ((c.constantBlocks & (1u << b)) && std::memcmp(input.constants + b * 64u, c.constants + b * 64u, 256))
      return false;
  return true;
}

FetchShaderStats consumerFetchShaderStats() noexcept {
  auto& shaders = consumerShaders();
  std::lock_guard<std::mutex> lock(shaders.mutex);
  return {sizeof(ConsumerShaders), shaders.entries.size(), shaders.count, shaders.capped};
}

FetchSpan requestedFetchF1(const FetchCapture& c) noexcept { return c.reason.empty() ? c.f1 : FetchSpan{}; }

FetchAnalysis analyzeFetchDraw(const FetchCapture& c, const FetchFrozenSource& source) {
  FetchAnalysis result;
  result.drawId = c.drawId;
  result.vsHash = c.vsHash;
  auto refuse = [&](const char* reason) {
    result.reason = reason;
    result.uses.clear();
    return result;
  };
  if (!c.reason.empty()) return refuse(c.reason.c_str());
  if (c.f1.len && (!source.complete || source.drawId != c.drawId || !source.recordedBefore ||
                   source.recordedBefore != source.recordedAfter)) return refuse("f1-version");
  if (c.f1.len && (!source.bytes || source.phys != c.f1.phys || source.byteCount != c.f1.len))
    return refuse("f1-source");
  const VsProg& g = c.vs.prog;
  // One transient renderer analysis, serialized with the private F1 snapshot.
  // Reserve the explicit limit so allocator growth cannot exceed this bound.
  result.uses.reserve(kCaptureUses);
  auto emit = [&](const VsFetch& f, Iv value) {
    for (uint32_t rnd = 0; rnd < 2; ++rnd) {
      if (!(f.rnd & (1u << rnd))) continue;
      int64_t lo, hi;
      if (!capturedIndices(value, rnd != 0, lo, hi)) return false;
      for (int64_t index = lo; index <= hi; ++index) {
        uint32_t phys;
        if (!capturedOffset(c, f.slot, f.stride, f.off, f.width, index, phys)) return false;
        if (result.uses.size() >= kCaptureUses) return false;
        result.uses.push_back(FetchUse{f.instruction, f.slot, f.cls, phys, f.width * 4u});
      }
    }
    return true;
  };
  for (uint32_t vertex : c.vertices) {
    IRegs r;
    if (!capturedRegs(c, vertex, r)) return refuse("prologue-range");
    for (uint32_t i = 0; i < c.vs.nFetch; ++i) {
      const auto& f = c.vs.fetch[i];
      if (f.cls == kClsD) {
        if (!emit(f, Iv{double(vertex), double(vertex)})) return refuse("fetch-range-or-budget");
      } else if (f.cls == kClsL1) {
        if (!emit(f, r.r[f.src >> 2][f.src & 3u])) return refuse("fetch-range-or-budget");
      } else {
        int64_t lo, hi;
        if (!capturedIndices(r.r[g.f1Src >> 2][g.f1Src & 3u], g.f1Rnd, lo, hi)) return refuse("f1-index-range");
        for (int64_t index = lo; index <= hi; ++index) {
          // Missing components of a 32-bit integer F1 element are zero, matching
          // the existing evaluator. Never substitute either current guest alias.
          float value = 0;
          if (f.l2c < g.f1Comps) {
            uint32_t phys;
            if (!capturedOffset(c, g.f1Slot, g.f1Stride, g.f1Off + f.l2c, 1, index, phys) ||
                phys < source.phys || uint64_t(phys - source.phys) + 4 > source.byteCount)
              return refuse("f1-extent");
            uint32_t word;
            std::memcpy(&word, source.bytes + phys - source.phys, 4);
            const uint32_t endian = c.fetch[g.f1Slot * 2u + 1u] & 3u;
            if (endian == 1u || endian == 2u)
              word = ((word & 0x00FF00FFu) << 8) | ((word >> 8) & 0x00FF00FFu);
            if (endian == 2u || endian == 3u) word = (word << 16) | (word >> 16);
            value = g.f1Signed ? float(int32_t(word)) : float(word);
          }
          if (!emit(f, Iv{double(value), double(value)})) return refuse("fetch-range-or-budget");
        }
      }
    }
  }
  result.complete = true;
  result.reason = "captured-draw-only";
  return result;
}

const char* kindName(Kind k) noexcept {
  switch (k) {
    case Kind::kOther: return "other";
    case Kind::kVb: return "vb";
    case Kind::kIb: return "ib";
    case Kind::kTex: return "tex";
    case Kind::kResolve: return "res";
    case Kind::kScanout: return "scan";
    default: return "?";
  }
}
const char* provName(Prov p) noexcept {
  switch (p) {
    case Prov::k8: return "8";
    case Prov::kA: return "A";
    case Prov::kLegacy8: return "l8";
    case Prov::kLegacyA: return "lA";
    case Prov::kUndecided: return "l";
    default: return "?";
  }
}
const char* siteName(Site s) noexcept {
  switch (s) {
    case Site::kDrain: return "drain";
    case Site::kPrepass: return "prepass";
    case Site::kRender: return "render";
    default: return "?";
  }
}
const char* outcomeName(Outcome o) noexcept {
  switch (o) {
    case Outcome::kAgreeUp: return "agree_up";
    case Outcome::kAgreeKeep: return "agree_keep";
    case Outcome::kOver: return "over";
    case Outcome::kMissed: return "missed";
    case Outcome::kClobber: return "clobber";
    case Outcome::kRaceNote: return "race.note";
    case Outcome::kRaceLock: return "race.lock";
    case Outcome::kPageOnly: return "page_only";
    default: return "?";
  }
}
const char* covName(Cov c) noexcept {
  switch (c) {
    case Cov::kDecided: return "decided";
    case Cov::kCand: return "cand";
    case Cov::kVal: return "val";
    case Cov::kReuse: return "reuse";
    case Cov::kReuseFailed: return "reuse_failed";
    case Cov::kUnseen: return "unseen";
    case Cov::kDeferred: return "deferred";
    case Cov::kFailed: return "failed";
    case Cov::kRebase: return "rebase";
    default: return "?";
  }
}
const char* chkName(Chk c) noexcept {
  switch (c) {
    case Chk::kChecked: return "checked";
    case Chk::kStale: return "stale";
    case Chk::kMismatch: return "mismatch";
    case Chk::kUnchecked: return "unchecked";
    default: return "?";
  }
}
const char* inCbName(InCb c) noexcept {
  switch (c) {
    case InCb::kUnknown: return "unk";
    case InCb::kIn: return "in";
    case InCb::kOut: return "out";
    default: return "?";
  }
}
const char* segName(Seg s) noexcept {
  switch (s) {
    case Seg::kNone: return "none";
    case Seg::kUp: return "up";
    case Seg::kZpass: return "zp";
    default: return "?";
  }
}
const char* rngName(Rng r) noexcept {
  switch (r) {
    case Rng::kUnknown: return "unk";
    case Rng::kSame: return "same";
    case Rng::kMoved: return "moved";
    default: return "?";
  }
}

bool census() noexcept {
  const int o = g_testOverride.load(std::memory_order_relaxed);
  return o >= 0 ? o != 0 : envOn();
}

Registry* registry() noexcept { return census() ? &st().reg : nullptr; }

void noteCoher(uint32_t status, uint32_t base, uint32_t size) noexcept {
  if (!census()) return;
  State& s = st();
  // Every request by its status value (census_coher), then its class.
  uint32_t p0 = 0, p1 = 0;
  const uint32_t pages = Registry::pageRange(base, size, p0, p1) ? p1 - p0 + 1u : 0u;
  add(s, kOffCoherRequests);
  const uint64_t key = uint64_t(status) | (1ull << 32);
  uint32_t slot = kCoherSlots;
  for (uint32_t i = 0; i < kCoherSlots; i++) {
    uint64_t cur = s.coherKey[i].load(std::memory_order_acquire);
    if (cur == 0 && s.coherKey[i].compare_exchange_strong(cur, key, std::memory_order_acq_rel)) cur = key;
    if (cur == key) {
      slot = i;
      break;
    }
  }
  if (slot < kCoherSlots) {
    add(s, kOffCoherCalls + slot);
    add(s, kOffCoherPages + slot, pages);
  } else {
    add(s, kOffCoherOverflow);
  }
  switch (status) {
    case kCoherShaderSync:  // the SDK's shader-constant sync (SIZE 0): a shader-cache action, no memory range
      add(s, kOffNoteShSync);
      return;
    case kCoherResolve:  // the Resolve's request after its copy: the GPU wrote the destination - never a CPU note
      add(s, kOffNoteResolve);
      add(s, kOffNoteResolvePages, pages);
      return;
    case kCoherCmdBuf:  // the close's vertex-cache invalidate of the whole command buffer: the inline vertices' boundary
      add(s, kOffNoteWhole);
      if (size) learnCb(s, base, uint32_t(std::min<uint64_t>(uint64_t(base) + size, 0xFFFFFFFFull)));
      noteCpu(s, base, size, Src::kCmdBuf);
      return;
    case kCoherInval: noteCpu(s, base, size, Src::kInval); return;
    case kCoherTexLock: noteCpu(s, base, size, Src::kTexLock); return;
    default: break;
  }
  noteCpu(s, base, size, Src::kOther);  // none expected: the conservative side, sampled
  uint32_t n;
  if (sample(s.logCoher, n))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MEM_RES] COHER #%u status=%08X base=%08X size=%X (an unclassified status: noted "
                "as a CPU write)", n, status, base, size);
}

void noteUpAlloc(uint32_t phys, uint32_t bytes, bool zpass, uint32_t cbLo, uint32_t cbHi) noexcept {
  if (!census()) return;
  State& s = st();
  learnCb(s, cbLo, cbHi);
  add(s, kOffUpAllocs);
  if (zpass) add(s, kOffUpAllocsZpass);
  uint32_t p0, p1;
  if (!Registry::pageRange(phys & 0x1FFFFFFFu, bytes, p0, p1)) return;
  for (uint32_t p = p0; p <= p1; p++)
    s.upTag[p].store(uint8_t(zpass ? Seg::kZpass : Seg::kUp), std::memory_order_relaxed);
}

void noteGrantWindow(uint32_t va, uint32_t bytes) noexcept {
  if (!census() || !bytes || va < 0x80000000u) return;  // (a virtual allocation is no GPU window)
  State& s = st();
  const uint8_t w = va >= 0xA0000000u ? 0xAu : 8u;
  uint32_t p0, p1;
  if (!Registry::pageRange(physOfVa(va), bytes, p0, p1)) return;
  for (uint32_t p = p0; p <= p1; p++) s.gWin[p].store(w, std::memory_order_relaxed);
}

void noteGrant(uint32_t va, uint32_t bytes, uint32_t lr) noexcept {
  noteGrantWindow(va, bytes);  // memres5: every grant names its pages' last-grant window
  if (!census() || !va || bytes < kGrantMin) return;
  State& s = st();
  const uint32_t i = s.nGrants.load(std::memory_order_relaxed) < kMaxGrants
                         ? s.nGrants.fetch_add(1, std::memory_order_acq_rel)
                         : kMaxGrants;
  if (i >= kMaxGrants) {
    add(s, kOffGrantOverflow);
    return;
  }
  s.gVa[i].store(va, std::memory_order_relaxed);
  s.gBytes[i].store(bytes, std::memory_order_relaxed);
  s.gLr[i].store(lr, std::memory_order_relaxed);
  s.gPhys[i].store(physOfVa(va), std::memory_order_relaxed);
  s.gReady[i].store(true, std::memory_order_release);
}

void hostWillWrite(uint32_t guestVA, uint32_t len) noexcept {
  if (!census() || !len || guestVA < 0x80000000u || guestVA >= 0xC0000000u) return;
  State& s = st();
  if (!s.nTraps.load(std::memory_order_acquire)) return;
  const uint64_t end = std::min<uint64_t>(uint64_t(guestVA) + len, 0xC0000000ull);
  for (uint64_t a = guestVA & ~0xFFFull; a < end; a += 0x1000u) {
    const uint32_t page = (uint32_t(a) & 0x1FFFFFFFu) >> kPageLog2;
    if (s.trapSt[page].load(std::memory_order_acquire) == kTrapArmed)
      trapFire(s, page, a < guestVA ? guestVA : uint32_t(a), 0u, true);
  }
}

bool censusTrapHitForTest(uint32_t guestVA, uint64_t rip) noexcept {
  if (!census()) return false;
  State& s = st();
  const uint32_t page = (guestVA & 0x1FFFFFFFu) >> kPageLog2;
  if (s.trapSt[page].load(std::memory_order_acquire) == 0) return false;
  return trapFire(s, page, guestVA, rip, false);
}

void noteReadFile(uint32_t guestVA, uint32_t len) noexcept {
  if (!census() || !len) return;
  State& s = st();
  if (guestVA < 0x80000000u) {  // a virtual-heap buffer: not at a GPU physical address in this arena
    add(s, kOffNoteLowVa);
    return;
  }
  noteCpu(s, physOfVa(guestVA), len, Src::kReadFile);
}

void noteLock(uint32_t tex, const uint32_t* f) noexcept {
  if (!census()) return;
  State& s = st();
  add(s, kOffNoteLock);
  Span sp[2];
  const uint32_t n = f ? textureSpans(f, sp) : 0u;
  if (!n) {
    add(s, kOffNoteLockNoFetch);
    return;
  }
  std::lock_guard<std::mutex> lk(s.lockMu);
  std::vector<Span>& v = s.locks[tex];
  if (!v.empty()) lockWindow(s, v, -1);  // locked again without an Unlock: the new window replaces the old
  v.assign(sp, sp + n);
  lockWindow(s, v, +1);
}

void noteUnlock(uint32_t tex, const uint32_t* f, bool moviePlane) noexcept {
  if (!census()) return;
  State& s = st();
  Span sp[2];
  const uint32_t n = f ? textureSpans(f, sp) : 0u;
  if (!n) add(s, kOffNoteUnlockNoFetch);
  for (uint32_t i = 0; i < n; i++) noteCpu(s, sp[i].phys, sp[i].len, Src::kUnlock);
  // A movie plane's base range is the decoder's (marked under the allocation's owner version); another texture
  // unlocked over a page ends a mark there (the allocation was reused).
  auto ov = g_ownerVer.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t p0, p1;
    if (!Registry::pageRange(sp[i].phys, sp[i].len, p0, p1)) continue;
    for (uint32_t p = p0; p <= p1; p++) s.movie[p].store(moviePlane && i == 0 ? ov(p << kPageLog2) : 0u,
                                                        std::memory_order_relaxed);
    if (moviePlane && i == 0) add(s, kOffMovieMarks);
  }
  // The note first, then the window closes: a decision between the two sees the note.
  std::lock_guard<std::mutex> lk(s.lockMu);
  const auto it = s.locks.find(tex);
  if (it == s.locks.end()) {
    add(s, kOffNoteUnlockUnmatched);
    return;
  }
  lockWindow(s, it->second, -1);
  s.locks.erase(it);
}

void noteCpuOnly(uint32_t guestVA, uint32_t len) noexcept {
  if (!census() || !len || guestVA < 0x80000000u || uint64_t(guestVA) + len > 0x100000000ull) return;
  State& s = st();
  s.cpuOnlyNotes.fetch_add(1, std::memory_order_relaxed);
  const uint64_t end = (uint64_t(guestVA) + len + 3u) & ~uint64_t(3u);
  for (uint64_t a = guestVA & ~3u; a < end; a += 4u) {
    const uint32_t phys = physOfVa(uint32_t(a));
    const uint32_t page = phys >> kPageLog2;
    if (page >= kPages) return;
    const uint32_t key = cpuOnlyKey(s, page);
    if (!key) continue;  // no census grant over the page (or an overflow): nothing named
    if (s.cpuOnlyOwner[page].load(std::memory_order_acquire) != key) {  // another tenancy: its names no longer hold
      std::lock_guard<std::mutex> lk(s.fOwnerMu[page & 63u]);
      if (s.cpuOnlyOwner[page].load(std::memory_order_relaxed) != key) {
        for (uint32_t w = 0; w < kDwWords; w++) s.cpuOnlyDw[size_t(page) * kDwWords + w].store(0, std::memory_order_relaxed);
        s.cpuOnlyOwner[page].store(key, std::memory_order_release);
      }
    }
    const uint32_t dw = (phys & 0xFFFu) >> 2;
    s.cpuOnlyDw[size_t(page) * kDwWords + (dw >> 6)].fetch_or(uint64_t{1} << (dw & 63u), std::memory_order_relaxed);
    s.cpuOnlyMs[page].store(censusNowMs(), std::memory_order_relaxed);
  }
}

void notePatch(uint32_t guestVA, uint32_t len) noexcept {
  if (!census() || !len) return;
  State& s = st();
  if (guestVA < 0x80000000u || uint64_t(guestVA) + len > 0x100000000ull) {
    s.patchRefused.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t set = guestVA >= 0xA0000000u ? 1u : 0u;
  uint64_t n = 0;
  const uint64_t end = (uint64_t(guestVA) + len + 3u) & ~uint64_t(3u);
  for (uint64_t a = guestVA & ~3u; a < end; a += 4u) {
    const uint32_t phys = physOfVa(uint32_t(a));
    const uint32_t page = phys >> kPageLog2;
    if (page >= kPages) {
      s.patchRefused.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    std::atomic<uint64_t>* b = s.patchBlk[page].load(std::memory_order_acquire);
    if (!b) {
      std::atomic<uint64_t>* fresh = new (std::nothrow) std::atomic<uint64_t>[2u * kDwWords]();
      if (!fresh) {  // never lose a write: the page note instead (the conservative side)
        s.patchRefused.fetch_add(1, std::memory_order_relaxed);
        noteCpu(s, phys & ~0xFFFu, 4096u, Src::kOther);
        continue;
      }
      if (s.patchBlk[page].compare_exchange_strong(b, fresh, std::memory_order_acq_rel)) b = fresh;
      else delete[] fresh;
    }
    const uint32_t dw = (phys & 0xFFFu) >> 2;
    b[set * kDwWords + (dw >> 6)].fetch_or(uint64_t{1} << (dw & 63u), std::memory_order_release);
    dwatchNoted(phys, set);
    n++;
  }
  s.patchNotes.fetch_add(1, std::memory_order_relaxed);
  s.patchDwords.fetch_add(n, std::memory_order_relaxed);
}

void noteGpuWrite(uint32_t phys, uint32_t len) noexcept {
  if (!census() || !len) return;
  State& s = st();
  const uint32_t n = s.reg.noteGpuWrite(phys & 0x1FFFFFFFu, len);
  add(s, kOffNoteCalls + uint32_t(Src::kGpu));
  if (n) add(s, kOffNotePages + uint32_t(Src::kGpu), n);
  else add(s, kOffNoteRefused);
}

// ---- memres5: the draw's vertex fetches, index-bounded ----
void noteVsUcode(uint64_t hash, const uint8_t* ucodeBE, size_t bytes) noexcept {
  if (!ucodeBE || !bytes) return;
  if (resourceConsumerEnabled()) {
    auto& shaders = consumerShaders();
    std::lock_guard<std::mutex> lock(shaders.mutex);
    const auto end = shaders.entries.begin() + shaders.count;
    const auto found = std::find_if(shaders.entries.begin(), end,
        [&](const ConsumerShaderEntry& entry) { return entry.hash == hash; });
    if (found == end) {
      if (shaders.count == shaders.entries.size()) {
        ++shaders.capped;
      } else {
        auto& entry = shaders.entries[shaders.count++];
        entry.hash = hash;
        analyzeVs(ucodeBE, bytes, entry.info);
        analyzeProg(ucodeBE, bytes, entry.info);
        progLiterals(hash, entry.info);
      }
    }
  }
  if (!census()) return;
  State& s = st();
  {
    std::lock_guard<std::mutex> lk(s.vsMu);
    if (s.vs.count(hash) || s.vs.size() >= 65536u) return;
  }
  VsInfo v;
  analyzeVs(ucodeBE, bytes, v);
  analyzeProg(ucodeBE, bytes, v);  // memres6: the computed-index classes
  progLiterals(hash, v);           // memres6: ... and the literals the translation baked in
  {
    std::lock_guard<std::mutex> lk(s.vsMu);
    s.vs.emplace(hash, v);
  }
  // the first analyses, for the roll's reader: which slots are index-bounded (memres6: '' direct, 'c' through the
  // prologue (L1), 's' through the index stream (L2), '!' a fallback with its reason)
  const uint32_t ln = s.logVsb.fetch_add(1, std::memory_order_relaxed);
  if (ln < 64u) {
    char sl[320] = "";
    size_t o = 0;
    for (uint32_t i = 0; i < v.n && o + 32 < sizeof sl; i++) {
      const VsSlot& x = v.s[i];
      char tag[16] = "";
      if (!x.safe) std::snprintf(tag, sizeof tag, "!%s", kFbName[x.fbr < kFbReasons ? x.fbr : 0]);
      else if (x.cls & (kClsL1 | kClsL2))
        std::snprintf(tag, sizeof tag, "%s%s", (x.cls & kClsL1) ? "c" : "", (x.cls & kClsL2) ? "s" : "");
      const int w = std::snprintf(sl + o, sizeof sl - o, "%s%u:%u/%d..%d%s", i ? "," : "", x.slot, x.stride,
                                  x.minOff, x.maxEnd, tag);
      if (w < 0) break;
      o += size_t(w);
    }
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MEM_RES] VSB #%u vs=%016llX ok=%d slots=%s prog=%s n=%u f1=%u lit=%u",
                ln + 1u, (unsigned long long)hash, v.ok ? 1 : 0, v.n ? sl : "-",
                v.prog.on ? "on" : v.prog.reason ? kFbName[v.prog.reason] : "-", v.prog.n, v.prog.f1Slot, v.prog.nLit);
  }
}

namespace {
void openDraw(uint64_t vsHash, bool rangeKnown, bool any, uint32_t vMin, uint32_t vMax, const uint32_t* consts,
              const uint32_t* fetch) noexcept {
  DrawState& d = t_draw;
  d.on = true;
  d.range = rangeKnown;
  d.any = any;
  d.vMin = vMin;
  d.vMax = vMax;
  d.vsHash = vsHash;
  d.vsKnown = false;
  d.exact = false;
  d.vs = nullptr;
  d.consts = consts;
  d.fetch = fetch;
  d.fam = FamDraw{};
  State& s = st();
  std::lock_guard<std::mutex> lk(s.vsMu);
  auto it = s.vs.find(vsHash);
  if (it != s.vs.end()) {
    d.vs = &it->second;
    d.vsKnown = true;
  }
}
}  // namespace
DrawScope::DrawScope(uint64_t vsHash, bool rangeKnown, bool any, uint32_t vMin, uint32_t vMax, const uint32_t* consts,
                     const uint32_t* fetch) noexcept
    : on_(census()) {
  if (on_) openDraw(vsHash, rangeKnown, any, vMin, vMax, consts, fetch);
}
DrawScope::DrawScope(uint64_t vsHash, bool indexed, const uint8_t* ib, uint32_t n, bool idx32, uint32_t endian,
                     uint32_t offset, bool clampOn, uint32_t minV, uint32_t maxV, bool stripCut,
                     uint32_t resetIndex, const uint32_t* consts, const uint32_t* fetch) noexcept
    : on_(census()) {
  if (!on_) return;
  uint32_t vMin = 0, vMax = 0;
  const bool known = !indexed || ib;
  // mem-native (Astra round 3): one snapshot of the index bytes serves the range and the used-vertex bitmap (the
  // guest may rewrite its index buffer between two reads)
  static thread_local std::vector<uint8_t> t_ib;
  const uint8_t* src = ib;
  bool snap = false;
  if (indexed && ib && n) {
    const size_t bytes = size_t(n) * (idx32 ? 4u : 2u);
    if (bytes <= (size_t(64) << 20)) {
      t_ib.assign(ib, ib + bytes);
      src = t_ib.data();
      snap = true;
    }
  }
  const bool any = known && vertexIndexRange(indexed ? src : nullptr, n, idx32, endian, offset, clampOn, minV, maxV,
                                             stripCut, resetIndex, vMin, vMax);
  openDraw(vsHash, known, any, vMin, vMax, consts, fetch);
  // mem-native: the used vertices of an indexed draw (vertexIndexRange's conversion), for markUsedVertices - from the
  // snapshot, while every index is below 2^23 (float32 holds it exactly; a direct fetch's truncating and rounding
  // conversions are then the identity - Astra round 3: 0x800001 rounds to 0x800002)
  if (any && indexed && snap && vMax - vMin < (1u << 20) && vMax < (1u << 23)) {
    DrawState& d = t_draw;
    d.used.assign((vMax - vMin) / 64u + 1u, 0ull);
    bool inRange = true;
    for (uint32_t i = 0; i < n && inRange; i++) {
      uint32_t r;
      if (idx32) {
        std::memcpy(&r, src + size_t(i) * 4u, 4);
        if (endian == 2) r = (r >> 24) | ((r >> 8) & 0xFF00u) | ((r << 8) & 0xFF0000u) | (r << 24);
        else if (endian == 3) r = (r >> 16) | (r << 16);
        else if (endian == 1) r = ((r & 0x00FF00FFu) << 8) | ((r >> 8) & 0x00FF00FFu);
      } else {
        uint16_t h;
        std::memcpy(&h, src + size_t(i) * 2u, 2);
        if (endian == 1 || endian == 2) h = uint16_t((h >> 8) | (h << 8));
        r = h;
      }
      if (stripCut && r == resetIndex) continue;
      uint32_t v = (r + offset) & 0xFFFFFFu;
      if (clampOn) v = std::min(std::max(v, minV), maxV);
      if (v < vMin || v > vMax) {  // (not from the range's own walk of the same bytes: no bitmap, the span)
        inRange = false;
        break;
      }
      const uint32_t k = v - vMin;
      d.used[k >> 6] |= 1ull << (k & 63u);
    }
    d.exact = inRange;
  }
}
DrawScope::~DrawScope() {
  if (on_) t_draw.on = false;
}
SlotScope::SlotScope(uint32_t slot) noexcept : on_(census()) {
  if (!on_) return;
  prev_ = t_slot;
  t_slot = slot + 1u;
}
SlotScope::~SlotScope() {
  if (on_) t_slot = prev_;
}

bool vertexIndexRange(const uint8_t* ib, uint32_t n, bool idx32, uint32_t endian, uint32_t offset, bool clampOn,
                      uint32_t minV, uint32_t maxV, bool stripCut, uint32_t resetIndex, uint32_t& vMin,
                      uint32_t& vMax) noexcept {
  vMin = 0xFFFFFFFFu;
  vMax = 0;
  auto remap = [&](uint32_t r) -> uint32_t {  // pm4StageVsf's remap: offset, 24-bit mask, clamp
    uint32_t v = (r + offset) & 0xFFFFFFu;
    if (clampOn) v = std::min(std::max(v, minV), maxV);
    return v;
  };
  if (!n) return false;
  if (!ib && uint64_t(offset & 0xFFFFFFu) + n <= 0x1000000ull) {  // auto-indexed, no wrap: monotone
    vMin = remap(0);
    vMax = remap(n - 1u);
    return true;
  }
  bool any = false;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t r = i;
    if (ib && idx32) {
      std::memcpy(&r, ib + size_t(i) * 4u, 4);
      if (endian == 2) r = (r >> 24) | ((r >> 8) & 0xFF00u) | ((r << 8) & 0xFF0000u) | (r << 24);
      else if (endian == 3) r = (r >> 16) | (r << 16);
      else if (endian == 1) r = ((r & 0x00FF00FFu) << 8) | ((r >> 8) & 0x00FF00FFu);
    } else if (ib) {
      uint16_t h;
      std::memcpy(&h, ib + size_t(i) * 2u, 2);
      if (endian == 1 || endian == 2) h = uint16_t((h >> 8) | (h << 8));
      r = h;
    }
    if (stripCut && r == resetIndex) continue;  // a strip's cut reads no vertex
    const uint32_t v = remap(r);
    vMin = std::min(vMin, v);
    vMax = std::max(vMax, v);
    any = true;
  }
  return any;
}

WalkScope::WalkScope(uint32_t pages, bool validation, uint32_t phys, uint32_t len) noexcept
    : on_(census()), val_(validation) {
  if (!on_) return;
  want_ = pages;
  phys_ = phys & 0x1FFFFFFFu;
  len_ = len;
  // memres3: the request's bytes join the fetched mask of their pages - every request, whatever disposes of its pages
  // (a decision, a reuse, a deferral): the union a page-only decision is checked against. memres5: a vertex fetch's
  // bytes are only the vertices its draw's indices address (fetchBound; fail-closed to the declared range, counted).
  // memres6: through a computed index too; the request's class goes with its bytes (the ring, via=).
  if (!validation && len) {
    {
      uint32_t q0, q1;
      if (Registry::pageRange(phys_, len, q0, q1))
        for (uint32_t q = q0; q <= q1; q++) ownerEpoch(st(), q);  // mem-native: a new tenancy's current marks
    }
    uint32_t blo = phys_, bn = len;
    uint8_t cls = kReqOther;
    const VsSlot* exact = nullptr;
    if (t_kind == uint8_t(Kind::kVb)) cls = fetchBound(st(), phys_, len, blo, bn, &exact);
    if (exact) {
      markUsedVertices(st(), phys_, len, *exact);  // mem-native: the used vertices' element dwords only
    } else {
      if (t_kind == uint8_t(Kind::kVb)) markWatchSpan(phys_, len, blo, bn, cls);
      markFetched(st(), blo, bn, cls);
    }
  }
  seq_ = ++t_scopeSeq ? t_scopeSeq : ++t_scopeSeq;  // never 0 (0 = no snapshot)
  prev_ = t_scope;
  t_scope = this;
  add(st(), validation ? kOffCovValCands : kOffCovReq, pages);
}

WalkScope::~WalkScope() {
  if (!on_) return;
  t_scope = prev_;
  if (got_ == want_) return;
  State& s = st();
  add(s, kOffCovUnaccounted, got_ > want_ ? got_ - want_ : want_ - got_);
  uint32_t n;
  if (sample(s.logUnacc, n))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MEM_RES] UNACCOUNTED #%u %s pages=%u disposed=%u", n,
                val_ ? "validation" : "request", want_, got_);
}

void acceptPage(uint32_t page, Site site, uint32_t stamp, bool cpStamp, Prov prov) noexcept {
  if (!census() || page >= kPages) return;
  State& s = st();
  tagStamp(s, page, site, stamp, cpStamp);
  tally(s, Cov::kDecided);
  const Chk r = (t_wt.valid && t_wt.page == page) ? (t_wt.stale ? Chk::kStale : Chk::kChecked) : Chk::kUnchecked;
  t_wt.valid = false;
  add(s, kOffKeep + uint32_t(Feed::kWt) * kChks + uint32_t(r));
  decide(s, page, false, Why::kContent, prov, Kind(t_kind), site, false, 0, 0);
}

void candPage(uint32_t page, Site site, uint32_t stamp, bool cpStamp) noexcept {
  if (!census() || page >= kPages) return;
  State& s = st();
  tagStamp(s, page, site, stamp, cpStamp);
  tally(s, Cov::kCand);
}

void skipReuse(uint32_t page, uint32_t stamp, bool collect) noexcept {
  if (!census() || page >= kPages) return;
  State& s = st();
  const bool seen = s.stampColl[page].load(std::memory_order_relaxed) == stamp ||
                    (!collect && s.stampRend[page].load(std::memory_order_relaxed) == stamp);
  if (!seen) {
    tally(s, Cov::kUnseen);
    uint32_t n;
    if (sample(s.logUnseen, n))
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[MEM_RES] UNSEEN #%u page=%08X stamp=%u seen=%u/%u mode=%s", n,
                  page << kPageLog2, stamp, s.stampColl[page].load(std::memory_order_relaxed),
                  s.stampRend[page].load(std::memory_order_relaxed), collect ? "drain" : "render");
  } else if (s.stampFailed[page].load(std::memory_order_relaxed)) {
    tally(s, Cov::kReuseFailed);
  } else {
    tally(s, Cov::kReuse);
    if (s.reg.cpuGen(page) > s.lastDecCpu[page].load(std::memory_order_relaxed)) add(s, kOffReuseNoted);
  }
}

void skipMemo(uint32_t page, uint32_t epoch) noexcept {
  if (!census() || page >= kPages) return;
  State& s = st();
  if (s.stampColl[page].load(std::memory_order_relaxed) == epoch ||
      s.stampMemo[page].load(std::memory_order_relaxed) == epoch) {
    if (s.stampFailed[page].load(std::memory_order_relaxed) &&
        s.stampColl[page].load(std::memory_order_relaxed) == epoch) {
      tally(s, Cov::kReuseFailed);
      return;
    }
    tally(s, Cov::kReuse);
    if (s.reg.cpuGen(page) > s.lastDecCpu[page].load(std::memory_order_relaxed)) add(s, kOffReuseNoted);
    return;
  }
  tally(s, Cov::kUnseen);
  uint32_t n;
  if (sample(s.logUnseen, n))
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MEM_RES] UNSEEN #%u page=%08X stamp=%u seen=%u/%u/%u mode=memo", n,
                page << kPageLog2, epoch, s.stampColl[page].load(std::memory_order_relaxed),
                s.stampRend[page].load(std::memory_order_relaxed), s.stampMemo[page].load(std::memory_order_relaxed));
}

void skipDeferred(uint32_t page) noexcept {
  if (!census() || page >= kPages) return;
  tally(st(), Cov::kDeferred);
}

void modelKeepRange(uint32_t p0, uint32_t p1, Site site, Prov prov, uint32_t epoch) noexcept {
  if (!census() || p1 < p0 || p0 >= kPages) return;
  State& s = st();
  const uint32_t last = std::min(p1, kPages - 1u);
  const uint32_t pages = last - p0 + 1u;
  // The range cache's check loop ran just before this keep (SR_NB_SHM_RANGE_CHECK): its tally classifies the pages.
  const uint32_t ck = std::min(t_rc.checked, pages), sl = std::min(t_rc.stale, pages - ck),
                 mm = std::min(t_rc.mismatch, pages - ck - sl);
  const uint32_t keepRc = kOffKeep + uint32_t(Feed::kRc) * kChks;
  add(s, keepRc + uint32_t(Chk::kChecked), ck);
  add(s, keepRc + uint32_t(Chk::kStale), sl);
  add(s, keepRc + uint32_t(Chk::kMismatch), mm);
  add(s, keepRc + uint32_t(Chk::kUnchecked), pages - ck - sl - mm);
  t_rc = RcTally{0, 0, 0};
  for (uint32_t p = p0; p <= last; p++) {
    s.kind[p].store(t_kind, std::memory_order_relaxed);
    s.site[p].store(uint8_t(site), std::memory_order_relaxed);
    s.stampMemo[p].store(epoch, std::memory_order_relaxed);  // the model stamps nothing: only the memo relies on it
    decide(s, p, false, Why::kContent, prov, Kind(t_kind), site, false, 0, 0);
  }
  tally(s, Cov::kDecided, pages);
}

void wtChecked(uint32_t page, bool stale) noexcept {
  if (!census()) return;
  t_wt = WtChk{page, stale, true};
}

void rcMismatch(uint32_t page) noexcept {
  if (!census()) return;
  (void)page;
  t_rc.mismatch++;
}

void rcChecked(uint32_t page) noexcept {
  if (!census()) return;
  if (t_wt.valid && t_wt.page == page) (t_wt.stale ? t_rc.stale : t_rc.checked)++;
  t_wt.valid = false;
}

void snapPage(uint32_t page) noexcept {
  if (!census() || page >= kPages || !t_scope || !t_scope->validation()) return;
  State& s = st();
  if (!t_snap.seq) {
    t_snap.cpu.reset(new uint32_t[kPages]);
    t_snap.gpu.reset(new uint32_t[kPages]);
    t_snap.seq.reset(new uint32_t[kPages]());
  }
  t_snap.cpu[page] = s.reg.cpuGen(page);
  t_snap.gpu[page] = s.reg.gpuGen(page);
  t_snap.seq[page] = t_scope->seq();
}

void failedPage(uint32_t page) noexcept {
  if (!census() || page >= kPages) return;
  State& s = st();
  s.stampFailed[page].store(1, std::memory_order_relaxed);
  tally(s, Cov::kFailed);
}

void modelPage(uint32_t page, bool upload, Why why, Prov prov) noexcept {
  if (!census() || page >= kPages) return;
  State& s = st();
  tally(s, Cov::kVal);
  const bool snapped = t_snap.seq && t_scope && t_scope->validation() && t_snap.seq[page] == t_scope->seq();
  decide(s, page, upload, why, prov, Kind(s.kind[page].load(std::memory_order_relaxed)),
         Site(s.site[page].load(std::memory_order_relaxed)), snapped, snapped ? t_snap.cpu[page] : 0u,
         snapped ? t_snap.gpu[page] : 0u);
}

void rebasePage(uint32_t page, uint32_t stamp, bool cpStamp) noexcept {
  if (!census() || page >= kPages) return;
  State& s = st();
  (cpStamp ? s.stampColl : s.stampRend)[page].store(stamp, std::memory_order_relaxed);
  s.stampFailed[page].store(0, std::memory_order_relaxed);
  add(s, kOffCov + uint32_t(Cov::kRebase));
}

void ibRead(uint32_t phys, uint32_t bytes, const uint8_t* host, Prov prov) noexcept {
  if (!census() || !bytes || !host) return;
  State& s = st();
  phys &= 0x1FFFFFFFu;
  uint32_t p0, p1;
  if (!Registry::pageRange(phys, bytes, p0, p1)) return;
  // The registry's key for the range: the newest CPU note over its pages (a GPU write does not reach the bytes the CPU
  // conversion reads - no write-back - so only cpuGen counts), and the page carrying it (its source).
  uint32_t gen = 0, genPage = p0;
  for (uint32_t p = p0; p <= p1; p++) {
    const uint32_t c = s.reg.cpuGen(p);
    if (c > gen) {
      gen = c;
      genPage = p;
    }
  }
  const uint64_t h = ShaderCache::hashUcode(host, bytes);
  bool modelUp, regUp, first = false, evict = false;
  {
    std::lock_guard<std::mutex> lk(s.ibMu);
    IbEnt& e = s.ib[((phys * 0x9E3779B1u) ^ (bytes * 0x85EBCA6Bu)) >> (32u - kIbLog2)];
    if (!e.valid || e.phys != phys || e.bytes != bytes) {
      modelUp = regUp = true;  // the range's first conversion (or its entry was evicted): both convert
      first = gen == 0u;
      evict = e.valid;
    } else {
      modelUp = h != e.hash;  // the bytes moved since the last conversion
      regUp = gen > e.gen;    // a note since the last conversion
    }
    e = IbEnt{phys, bytes, gen, h, true};
  }
  const uint32_t pv = uint32_t(prov) < kProvs ? uint32_t(prov) : uint32_t(Prov::kUndecided);
  const Src src = s.reg.lastSrc(genPage);
  add(s, kOffIbReads);
  if (first) add(s, kOffIbFirst);
  if (evict) add(s, kOffIbEvict);
  if (modelUp == regUp) {
    add(s, modelUp ? kOffIbAgreeUp : kOffIbAgreeKeep);
  } else if (regUp) {
    add(s, kOffIbOver);
  } else {
    add(s, kOffIbMissed);
    add(s, kOffIbMissedBy + pv * kSrcs + uint32_t(src));
    uint32_t n;
    if (sample(s.logIb, n))
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[MEM_RES] IB_MISSED #%u page=%08X prov=%s src=%s range=%08X+%u gen=%u clock=%u (the IB arm: outside "
                  "the page gate)",
                  n, genPage << kPageLog2, provName(prov), srcName(src), phys, bytes, gen, s.reg.clock());
  }
}

KindScope::KindScope(Kind k) noexcept : prev_(0), on_(census()) {
  if (on_) {
    prev_ = t_kind;
    t_kind = uint8_t(k);
  }
}
KindScope::~KindScope() {
  if (on_) t_kind = prev_;
}

void snapshot(Totals& o) noexcept {
  std::memset(&o, 0, sizeof o);
  if (!g_state) return;
  const State& s = *g_state;
  for (uint32_t oc = 0; oc < kOutcomes; oc++)
    for (uint32_t k = 0; k < kKinds; k++)
      for (uint32_t p = 0; p < kProvs; p++)
        for (uint32_t r = 0; r < kSrcs; r++)
          for (uint32_t si = 0; si < kSites; si++)
            for (uint32_t w = 0; w < kWhys; w++)
              for (uint32_t m = 0; m < 2u; m++) {
                const uint64_t v = ld(s, cellIx(oc, k, p, r, si, w, m));
                if (!v) continue;
                o.cell[oc][k][p][r][si][w][m] = v;
                o.decisions += v;
                o.outcome[oc] += v;
                if (oc == uint32_t(Outcome::kMissed)) {
                  o.missedWhy[w] += v;
                  if (m) o.missedMovie += v;
                  o.missedBy[k][p][r][si] += v;
                } else if (oc == uint32_t(Outcome::kOver)) {
                  o.overBy[k][r] += v;
                }
              }
  o.first = ld(s, kOffFirst);
  for (uint32_t k = 0; k < kKinds; k++)
    for (uint32_t p = 0; p < kProvs; p++) o.firstBy[k][p] = ld(s, kOffFirstBy + k * kProvs + p);
  o.winHeur = ld(s, kOffWinHeur);
  o.winAliasSame = ld(s, kOffWinAliasSame);
  o.winAliasDiff = ld(s, kOffWinAliasDiff);
  o.winUnmapped = ld(s, kOffWinUnmapped);
  o.clobberBoth = ld(s, kOffClobberBoth);
  o.reuseNoted = ld(s, kOffReuseNoted);
  for (uint32_t c = 0; c < kCovs; c++) o.cov[c] = ld(s, kOffCov + c);
  o.covReq = ld(s, kOffCovReq);
  o.covValCands = ld(s, kOffCovValCands);
  o.covUnaccounted = ld(s, kOffCovUnaccounted);
  for (uint32_t f = 0; f < kFeeds; f++)
    for (uint32_t c = 0; c < kChks; c++) o.keep[f][c] = ld(s, kOffKeep + f * kChks + c);
  o.ibReads = ld(s, kOffIbReads);
  o.ibAgreeUp = ld(s, kOffIbAgreeUp);
  o.ibAgreeKeep = ld(s, kOffIbAgreeKeep);
  o.ibMissed = ld(s, kOffIbMissed);
  o.ibOver = ld(s, kOffIbOver);
  o.ibFirst = ld(s, kOffIbFirst);
  o.ibEvict = ld(s, kOffIbEvict);
  for (uint32_t p = 0; p < kProvs; p++)
    for (uint32_t r = 0; r < kSrcs; r++) o.ibMissedBy[p][r] = ld(s, kOffIbMissedBy + p * kSrcs + r);
  for (uint32_t r = 0; r < kSrcs; r++) {
    o.noteCalls[r] = ld(s, kOffNoteCalls + r);
    o.notePages[r] = ld(s, kOffNotePages + r);
  }
  o.noteWhole = ld(s, kOffNoteWhole);
  o.noteRefused = ld(s, kOffNoteRefused);
  o.noteLowVa = ld(s, kOffNoteLowVa);
  o.noteUnlockNoFetch = ld(s, kOffNoteUnlockNoFetch);
  o.noteLock = ld(s, kOffNoteLock);
  o.noteLockNoFetch = ld(s, kOffNoteLockNoFetch);
  o.noteUnlockUnmatched = ld(s, kOffNoteUnlockUnmatched);
  o.movieMarks = ld(s, kOffMovieMarks);
  o.lockOpenPages = ld(s, kOffLockOpenPages);
  // memres2
  o.noteResolve = ld(s, kOffNoteResolve);
  o.noteResolvePages = ld(s, kOffNoteResolvePages);
  o.noteShSync = ld(s, kOffNoteShSync);
  o.coherRequests = ld(s, kOffCoherRequests);
  o.coherOverflow = ld(s, kOffCoherOverflow);
  for (uint32_t i = 0; i < kCoherSlots; i++) {
    const uint64_t key = s.coherKey[i].load(std::memory_order_acquire);
    o.coherUsed[i] = key != 0;
    o.coherStatus[i] = uint32_t(key);
    o.coherCalls[i] = ld(s, kOffCoherCalls + i);
    o.coherPages[i] = ld(s, kOffCoherPages + i);
  }
  for (uint32_t k = 0; k < kKinds; k++)
    for (uint32_t r = 0; r < kSrcs; r++)
      for (uint32_t c = 0; c < kInCbs; c++)
        for (uint32_t g = 0; g < kSegs; g++)
          for (uint32_t q = 0; q < kRngs; q++) o.missedAttr[k][r][c][g][q] = ld(s, kOffMissedAttr + attrIx(k, r, c, g, q));
  o.grants = std::min(s.nGrants.load(std::memory_order_acquire), kMaxGrants);
  for (uint32_t i = 0; i < o.grants; i++) {
    const bool ok = s.gReady[i].load(std::memory_order_acquire);
    o.grantVa[i] = ok ? s.gVa[i].load(std::memory_order_relaxed) : 0u;
    o.grantBytes[i] = ok ? s.gBytes[i].load(std::memory_order_relaxed) : 0u;
    o.grantLr[i] = ok ? s.gLr[i].load(std::memory_order_relaxed) : 0u;
  }
  o.grantOverflow = ld(s, kOffGrantOverflow);
  for (uint32_t i = 0; i <= kMaxGrants; i++) o.missedGrant[i] = ld(s, kOffMissedGrant + i);
  o.upAllocs = ld(s, kOffUpAllocs);
  o.upAllocsZpass = ld(s, kOffUpAllocsZpass);
  o.cbLo = s.cbLo.load(std::memory_order_relaxed);
  o.cbHi = s.cbHi.load(std::memory_order_relaxed);
  o.trapArmed = ld(s, kOffTrapArmed);
  o.trapFired = ld(s, kOffTrapFired);
  o.trapHost = ld(s, kOffTrapHost);
  o.trapArmFail = ld(s, kOffTrapArmFail);
  o.trapDeclined = ld(s, kOffTrapDeclined);
  o.fetchBounded = ld(s, kOffFetchBounded);
  o.fetchFbShader = ld(s, kOffFetchFbShader);
  o.fetchFbDraw = ld(s, kOffFetchFbDraw);
  o.winLive = ld(s, kOffWinLive);
  o.winGrant = ld(s, kOffWinGrant);
  o.winDefault = ld(s, kOffWinDefault);
  o.fetchBoundedComp = ld(s, kOffFetchBoundedComp);
  o.boundSampled = ld(s, kOffBoundSampled);
  o.boundViolations = ld(s, kOffBoundViolations);
  for (uint32_t r = 0; r < kFbReasonsMax; r++) o.fbReason[r] = r < kFbReasons ? ld(s, kOffFbReason + r) : 0u;
  o.traps = std::min(s.nTraps.load(std::memory_order_acquire), kMaxTraps);
  for (uint32_t i = 0; i < o.traps; i++) {
    const State::Trap& t = s.trap[i];
    TrapRec& r = o.trap[i];
    if (!t.ready.load(std::memory_order_acquire)) continue;
    r.page = t.page.load(std::memory_order_relaxed);
    r.kind = t.kind.load(std::memory_order_relaxed);
    r.src = t.src.load(std::memory_order_relaxed);
    r.armClock = t.armClock.load(std::memory_order_relaxed);
    r.fired = t.fired.load(std::memory_order_acquire);
    if (!r.fired) continue;
    r.host = t.host.load(std::memory_order_relaxed);
    r.inFetch = t.inFetch.load(std::memory_order_relaxed);
    r.dwInFetch = t.dwIn.load(std::memory_order_relaxed);
    r.on = t.on.load(std::memory_order_relaxed);
    r.req = t.req.load(std::memory_order_relaxed);
    for (uint32_t k = 0; k < 8u; k++) {
      const uint32_t v = t.hdr[k].load(std::memory_order_relaxed);
      std::memcpy(r.hdr + 4u * k, &v, 4);
    }
    r.va = t.va.load(std::memory_order_relaxed);
    r.lr = t.lr.load(std::memory_order_relaxed);
    r.ctr = t.ctr.load(std::memory_order_relaxed);
    r.r1 = t.r1.load(std::memory_order_relaxed);
    r.tid = t.tid.load(std::memory_order_relaxed);
    r.ripRva = t.rip.load(std::memory_order_relaxed);
  }
}

namespace {
constexpr uint32_t kLines = 12;
// The census lines (census, census_missed, census_over, census_first, census_missed_cells, census_hazard, census_ib,
// census_coher, census_missed_attr, census_missed_grant, memres6's census_vsfb and census_vsfb_top) of one snapshot,
// and the WRITER samples of the traps that fired since the last print (each printed once).
void buildLines(const char* why, uint64_t swaps, std::string (&out)[kLines], std::vector<std::string>* writers) {
  static Totals t;  // the CP thread's census (and the terminal line after the CP parked): one writer at a time
  snapshot(t);
  const std::string w = why ? why : "census";
  std::string a = "[MEM_RES] census " + w;
  appendKv(a, "swaps", swaps);
  appendKv(a, "decisions", t.decisions);
  for (uint32_t o = 0; o < kOutcomes; o++) appendKv(a, outcomeName(Outcome(o)), t.outcome[o]);
  appendKv(a, "missed.content", t.missedWhy[uint32_t(Why::kContent)]);
  appendKv(a, "missed.window", t.missedWhy[uint32_t(Why::kWindow)]);
  appendKv(a, "missed.gpu", t.missedWhy[uint32_t(Why::kGpu)]);
  appendKv(a, "missed.stale", t.missedWhy[uint32_t(Why::kStale)]);
  appendKv(a, "missed.movie", t.missedMovie);
  appendKv(a, "first", t.first);
  appendKv(a, "win.heur", t.winHeur);
  appendKv(a, "win.alias_same", t.winAliasSame);
  appendKv(a, "win.alias_diff", t.winAliasDiff);
  appendKv(a, "win.unmapped", t.winUnmapped);
  appendKv(a, "clobber.both", t.clobberBoth);
  appendKv(a, "reuse.noted", t.reuseNoted);
  appendKv(a, "cov.req", t.covReq);
  appendKv(a, "cov.val_cands", t.covValCands);
  for (uint32_t c = 0; c < kCovs; c++) appendKv(a, std::string("cov.") + covName(Cov(c)), t.cov[c]);
  appendKv(a, "cov.unaccounted", t.covUnaccounted);
  for (uint32_t f = 0; f < kFeeds; f++)
    for (uint32_t c = 0; c < kChks; c++)
      appendKv(a, std::string("keep.") + (f == uint32_t(Feed::kWt) ? "wt." : "rc.") + chkName(Chk(c)), t.keep[f][c]);
  for (uint32_t r = 0; r < kSrcs; r++) {
    if (r == uint32_t(Src::kNone)) continue;
    const std::string k = std::string("note.") + srcName(Src(r));
    appendKv(a, k, t.noteCalls[r]);
    appendKv(a, k + "_pages", t.notePages[r]);
  }
  appendKv(a, "note.whole", t.noteWhole);
  appendKv(a, "note.refused", t.noteRefused);
  appendKv(a, "note.lowva", t.noteLowVa);
  appendKv(a, "note.unlock_nofetch", t.noteUnlockNoFetch);
  appendKv(a, "note.lock", t.noteLock);
  appendKv(a, "note.lock_nofetch", t.noteLockNoFetch);
  appendKv(a, "note.unlock_unmatched", t.noteUnlockUnmatched);
  appendKv(a, "movie.marks", t.movieMarks);
  appendKv(a, "lock.open", t.lockOpenPages);
  // memres2: the Resolve's requests (never a CPU note), the inline-vertex allocations, the command buffer's range, the
  // traps.
  appendKv(a, "note.resolve", t.noteResolve);
  appendKv(a, "note.resolve_pages", t.noteResolvePages);
  appendKv(a, "note.shsync", t.noteShSync);
  appendKv(a, "coher.requests", t.coherRequests);
  appendKv(a, "up.allocs", t.upAllocs);
  appendKv(a, "up.zpass", t.upAllocsZpass);
  appendKv(a, "cb.lo", t.cbLo);
  appendKv(a, "cb.hi", t.cbHi);
  appendKv(a, "trap.armed", t.trapArmed);
  appendKv(a, "trap.fired", t.trapFired);
  appendKv(a, "trap.host", t.trapHost);
  appendKv(a, "trap.arm_fail", t.trapArmFail);
  appendKv(a, "trap.declined", t.trapDeclined);
  appendKv(a, "fetch.bounded", t.fetchBounded);  // memres5
  appendKv(a, "fetch.fallback", t.fetchFbShader + t.fetchFbDraw);
  appendKv(a, "fetch.fallback.shader", t.fetchFbShader);
  appendKv(a, "fetch.fallback.draw", t.fetchFbDraw);
  appendKv(a, "win.live", t.winLive);
  appendKv(a, "win.grant", t.winGrant);
  appendKv(a, "win.default", t.winDefault);
  appendKv(a, "fetch.bounded.computed", t.fetchBoundedComp);  // memres6
  appendKv(a, "bound.check.sampled", t.boundSampled);
  appendKv(a, "bound.check.violations", t.boundViolations);
  appendKv(a, "clock", g_state ? g_state->reg.clock() : 0u);
  // The model's own checker (SR_SHM_WT_CHECK=1): every re-hash of a page the write watch kept (also the rebaseline's
  // keeps) and the stale ones - wt.stale > 0 means the MODEL kept a stale page somewhere (both blind there).
  const sr::kernel::gww::Stats wts = sr::kernel::gww::stats();
  appendKv(a, "wt.checks", wts.staleChecks);
  appendKv(a, "wt.stale", wts.stale);
  out[0] = a;

  // The attributions: every dimension's marginal (each sums to its total), then the nonzero cells.
  std::string m = "[MEM_RES] census_missed " + w, ov = "[MEM_RES] census_over " + w,
              f = "[MEM_RES] census_first " + w, cells = "[MEM_RES] census_missed_cells " + w,
              hz = "[MEM_RES] census_hazard " + w, ib = "[MEM_RES] census_ib " + w;
  uint32_t nCells = 0;
  for (uint32_t k = 0; k < kKinds; k++) {
    uint64_t mk = 0, ok = 0, fk = 0;
    for (uint32_t p = 0; p < kProvs; p++) {
      fk += t.firstBy[k][p];
      for (uint32_t r = 0; r < kSrcs; r++)
        for (uint32_t s = 0; s < kSites; s++) {
          const uint64_t v = t.missedBy[k][p][r][s];
          mk += v;
          if (v && nCells < 96u) {
            char key[96];
            std::snprintf(key, sizeof key, "%s/%s/%s/%s", kindName(Kind(k)), provName(Prov(p)), srcName(Src(r)),
                          siteName(Site(s)));
            appendKv(cells, key, v);
            nCells++;
          }
        }
    }
    for (uint32_t r = 0; r < kSrcs; r++) ok += t.overBy[k][r];
    appendKv(m, std::string("kind.") + kindName(Kind(k)), mk);
    appendKv(ov, std::string("kind.") + kindName(Kind(k)), ok);
    appendKv(f, std::string("kind.") + kindName(Kind(k)), fk);
  }
  for (uint32_t p = 0; p < kProvs; p++) {
    uint64_t mp = 0, fp = 0;
    for (uint32_t k = 0; k < kKinds; k++) {
      fp += t.firstBy[k][p];
      for (uint32_t r = 0; r < kSrcs; r++)
        for (uint32_t s = 0; s < kSites; s++) mp += t.missedBy[k][p][r][s];
    }
    appendKv(m, std::string("prov.") + provName(Prov(p)), mp);
    appendKv(f, std::string("prov.") + provName(Prov(p)), fp);
  }
  for (uint32_t r = 0; r < kSrcs; r++) {
    uint64_t mr = 0, orr = 0;
    for (uint32_t k = 0; k < kKinds; k++) {
      orr += t.overBy[k][r];
      for (uint32_t p = 0; p < kProvs; p++)
        for (uint32_t s = 0; s < kSites; s++) mr += t.missedBy[k][p][r][s];
    }
    appendKv(m, std::string("src.") + srcName(Src(r)), mr);
    appendKv(ov, std::string("src.") + srcName(Src(r)), orr);
  }
  for (uint32_t s = 0; s < kSites; s++) {
    uint64_t ms = 0;
    for (uint32_t k = 0; k < kKinds; k++)
      for (uint32_t p = 0; p < kProvs; p++)
        for (uint32_t r = 0; r < kSrcs; r++) ms += t.missedBy[k][p][r][s];
    appendKv(m, std::string("site.") + siteName(Site(s)), ms);
  }
  appendKv(cells, "cells", nCells);
  // The hazards beside the misses: the clobbers by kind / source / site, the races by kind / site.
  auto marg = [&](Outcome oc, const char* pre, int dim) {
    const uint32_t n = dim == 0 ? kKinds : dim == 1 ? kSrcs : kSites;
    for (uint32_t i = 0; i < n; i++) {
      uint64_t v = 0;
      for (uint32_t k = 0; k < kKinds; k++)
        for (uint32_t p = 0; p < kProvs; p++)
          for (uint32_t r = 0; r < kSrcs; r++)
            for (uint32_t s = 0; s < kSites; s++)
              for (uint32_t wy = 0; wy < kWhys; wy++)
                for (uint32_t mv = 0; mv < 2u; mv++)
                  if ((dim == 0 ? k : dim == 1 ? r : s) == i) v += t.cell[uint32_t(oc)][k][p][r][s][wy][mv];
      appendKv(hz, std::string(pre) + (dim == 0 ? kindName(Kind(i)) : dim == 1 ? srcName(Src(i)) : siteName(Site(i))),
               v);
    }
  };
  marg(Outcome::kClobber, "clobber.kind.", 0);
  marg(Outcome::kClobber, "clobber.src.", 1);
  marg(Outcome::kClobber, "clobber.site.", 2);
  marg(Outcome::kRaceNote, "race.note.kind.", 0);
  marg(Outcome::kRaceNote, "race.note.site.", 2);
  marg(Outcome::kRaceLock, "race.lock.kind.", 0);
  marg(Outcome::kRaceLock, "race.lock.site.", 2);
  // The IB arm (a hypothetical conversion cache keyed by the registry; outside the page gate).
  appendKv(ib, "reads", t.ibReads);
  appendKv(ib, "agree_up", t.ibAgreeUp);
  appendKv(ib, "agree_keep", t.ibAgreeKeep);
  appendKv(ib, "missed", t.ibMissed);
  appendKv(ib, "over", t.ibOver);
  appendKv(ib, "first", t.ibFirst);
  appendKv(ib, "evict", t.ibEvict);
  for (uint32_t p = 0; p < kProvs; p++) {
    uint64_t v = 0;
    for (uint32_t r = 0; r < kSrcs; r++) v += t.ibMissedBy[p][r];
    appendKv(ib, std::string("missed.prov.") + provName(Prov(p)), v);
  }
  for (uint32_t r = 0; r < kSrcs; r++) {
    uint64_t v = 0;
    for (uint32_t p = 0; p < kProvs; p++) v += t.ibMissedBy[p][r];
    appendKv(ib, std::string("missed.src.") + srcName(Src(r)), v);
  }
  // memres2: every coherency request by its status value; the missed and page-only decisions' attributes (each marginal
  // sums to missed + page_only, rng.same = page_only; the nonzero cells kind/src/cb/seg/rng); the same decisions by the
  // grant holding the page (sums to missed + page_only).
  std::string co = "[MEM_RES] census_coher " + w, at = "[MEM_RES] census_missed_attr " + w,
              gr = "[MEM_RES] census_missed_grant " + w;
  appendKv(co, "requests", t.coherRequests);
  appendKv(co, "overflow", t.coherOverflow);
  for (uint32_t i = 0; i < kCoherSlots; i++) {
    if (!t.coherUsed[i]) continue;
    char key[40];
    std::snprintf(key, sizeof key, "calls.%08X", t.coherStatus[i]);
    appendKv(co, key, t.coherCalls[i]);
    std::snprintf(key, sizeof key, "pages.%08X", t.coherStatus[i]);
    appendKv(co, key, t.coherPages[i]);
  }
  uint64_t mc[kInCbs] = {}, mg[kSegs] = {}, mr[kRngs] = {};
  uint32_t nAttr = 0;
  std::string attrCells;
  for (uint32_t k = 0; k < kKinds; k++)
    for (uint32_t r = 0; r < kSrcs; r++)
      for (uint32_t c = 0; c < kInCbs; c++)
        for (uint32_t g = 0; g < kSegs; g++)
          for (uint32_t q = 0; q < kRngs; q++) {
            const uint64_t v = t.missedAttr[k][r][c][g][q];
            if (!v) continue;
            mc[c] += v;
            mg[g] += v;
            mr[q] += v;
            if (nAttr < 96u) {
              char key[96];
              std::snprintf(key, sizeof key, "%s/%s/%s/%s/%s", kindName(Kind(k)), srcName(Src(r)), inCbName(InCb(c)),
                            segName(Seg(g)), rngName(Rng(q)));
              appendKv(attrCells, key, v);
              nAttr++;
            }
          }
  for (uint32_t c = 0; c < kInCbs; c++) appendKv(at, std::string("cb.") + inCbName(InCb(c)), mc[c]);
  for (uint32_t g = 0; g < kSegs; g++) appendKv(at, std::string("seg.") + segName(Seg(g)), mg[g]);
  for (uint32_t q = 0; q < kRngs; q++) appendKv(at, std::string("rng.") + rngName(Rng(q)), mr[q]);
  at += attrCells;
  appendKv(at, "cells", nAttr);
  appendKv(gr, "none", t.missedGrant[kMaxGrants]);
  appendKv(gr, "grants", t.grants);
  appendKv(gr, "overflow", t.grantOverflow);
  for (uint32_t i = 0; i < t.grants; i++) {
    if (!t.missedGrant[i]) continue;
    char key[64];
    std::snprintf(key, sizeof key, "g.%08X+%X@%08X", t.grantVa[i], t.grantBytes[i], t.grantLr[i]);
    appendKv(gr, key, t.missedGrant[i]);
  }
  out[1] = m;
  out[2] = ov;
  out[3] = f;
  out[4] = cells;
  out[5] = hz;
  out[6] = ib;
  out[7] = co;
  out[8] = at;
  out[9] = gr;
  // memres6: the shader fallbacks by reason (r.* sums to fetch.fallback.shader) and the VS with the most - the top
  // eight hash:requests:last reason on a line of their own (census_vsfb_top: no key=count, the grader reads past it)
  std::string fb = "[MEM_RES] census_vsfb " + w, top = "[MEM_RES] census_vsfb_top " + w + " top=";
  for (uint32_t r = 0; r < kFbReasons; r++) appendKv(fb, std::string("r.") + kFbName[r], t.fbReason[r]);
  std::vector<std::pair<uint64_t, std::pair<uint64_t, uint8_t>>> v;
  if (g_state) {
    std::lock_guard<std::mutex> lk(g_state->fbMu);
    v.assign(g_state->fbByVs.begin(), g_state->fbByVs.end());
  }
  std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) {
    return x.second.first != y.second.first ? x.second.first > y.second.first : x.first < y.first;
  });
  appendKv(fb, "vs", uint64_t(v.size()));
  for (size_t i = 0; i < v.size() && i < 8u; i++) {
    char e[64];
    std::snprintf(e, sizeof e, "%s%016llX:%llu:%s", i ? "," : "", (unsigned long long)v[i].first,
                  (unsigned long long)v[i].second.first,
                  kFbName[v[i].second.second < kFbReasons ? v[i].second.second : 0]);
    top += e;
  }
  if (v.empty()) top += "-";
  out[10] = fb;
  out[11] = top;
  // The traps that fired since the last print (once each).
  if (writers && g_state) {
    State& s = *g_state;
    for (uint32_t i = 0; i < t.traps; i++) {
      const TrapRec& r = t.trap[i];
      if (!r.fired || s.trap[i].printed.exchange(true, std::memory_order_acq_rel)) continue;
      const uint32_t gi = grantOf(s, r.page);
      char gname[48] = "none";
      if (gi < kMaxGrants)
        std::snprintf(gname, sizeof gname, "%08X+%X@%08X", s.gVa[gi].load(std::memory_order_relaxed),
                      s.gBytes[gi].load(std::memory_order_relaxed), s.gLr[gi].load(std::memory_order_relaxed));
      char rq[48], hx[72];  // memres4: the range covering the written dword, the 32 bytes at va - 8 before the write
      fmtReq(r.req, rq, sizeof rq);
      hexBytes(r.hdr, 32u, hx, sizeof hx);
      char b[640];
      std::snprintf(b, sizeof b,
                    "[MEM_RES] WRITER #%u page=%08X va=%08X writer=%s tid=%u rip=%s%llX lr=%08X ctr=%08X r1=%08X "
                    "kind=%s src=%s grant=%s armed_clock=%u clock=%u in_fetch=%d on=%s dw_in_fetch=%d req=%s hdr=%s "
                    "via=%c",
                    s.logWriter.fetch_add(1, std::memory_order_relaxed) + 1u, r.page << kPageLog2, r.va,
                    r.host ? "host" : "guest", r.tid, (r.ripRva >> 63) ? "0x" : "exe+0x",
                    (unsigned long long)(r.ripRva & ~(1ull << 63)), r.lr, r.ctr, r.r1,
                    kindName(Kind(r.kind < kKinds ? r.kind : 0)),
                    srcName(Src(r.src < kSrcs ? r.src : 0)), gname, r.armClock, s.reg.clock(), r.inFetch ? 1 : 0,
                    r.on == kOnAlias ? "alias" : r.on == kOnPageOnly ? "page_only" : "missed", r.dwInFetch ? 1 : 0, rq,
                    hx, viaOf(s, r.page, (r.va & 0xFFFu) >> 2));
      writers->push_back(b);
    }
  }
}
}  // namespace

namespace {
// mem-native: the exact CPU-write patches (notePatch), reported beside the census lines and never gated: the notes and
// dwords noted (refused: not GPU-addressable), and at the registry's decisions the patches applied to a kept page's copy
// (applied, applied_dw), those an upload's copy held anyway (upload), those the GPU's data took (gpu), those with no
// registry copy to apply to (nocopy), and the decisions called race.note because a moved fetched dword's note landed
// after their take (race).
std::string patchLine(const char* why) {
  State& s = st();
  std::string l = std::string("[MEM_RES] census_patch ") + (why ? why : "census");
  appendKv(l, "notes", s.patchNotes.load(std::memory_order_relaxed));
  appendKv(l, "dwords", s.patchDwords.load(std::memory_order_relaxed));
  appendKv(l, "refused", s.patchRefused.load(std::memory_order_relaxed));
  appendKv(l, "applied", s.patchApplied.load(std::memory_order_relaxed));
  appendKv(l, "applied_dw", s.patchAppliedDw.load(std::memory_order_relaxed));
  appendKv(l, "upload", s.patchUpload.load(std::memory_order_relaxed));
  appendKv(l, "gpu", s.patchGpu.load(std::memory_order_relaxed));
  appendKv(l, "nocopy", s.patchNoCopy.load(std::memory_order_relaxed));
  appendKv(l, "race", s.patchRace.load(std::memory_order_relaxed));  // race.note: a moved dword noted after the take
  // the vertex requests marked per used vertex (markUsedVertices) and the runs they marked
  appendKv(l, "vtx_exact", s.vtxExact.load(std::memory_order_relaxed));
  appendKv(l, "vtx_runs", s.vtxRuns.load(std::memory_order_relaxed));
  // the owner-version changes seen (the current-version marks restarted), the misses whose moved dwords only an
  // earlier owner version's draws fetched (they stay MISSED), the converted indices the used-vertex bitmap lacked
  appendKv(l, "owner_clears", s.fOwnerClears.load(std::memory_order_relaxed));
  appendKv(l, "missed_prior", s.missedPrior.load(std::memory_order_relaxed));
  // the orchestrator's 2026-10-17 decision: the misses of only a named CPU-only writer's dwords (RenderModel_
  // SwapMeshChunkBindings' draw records; still MISSED in the totals), and the ranges it named
  appendKv(l, "missed_cpu_only", s.missedCpuOnly.load(std::memory_order_relaxed));
  appendKv(l, "cpu_only_notes", s.cpuOnlyNotes.load(std::memory_order_relaxed));
  appendKv(l, "vtx_stale", s.vtxStale.load(std::memory_order_relaxed));
  return l;
}
}  // namespace

void censusLine(const char* why, uint64_t swaps) noexcept {
  if (!census()) return;
  st();
  std::string lines[kLines];
  std::vector<std::string> writers;
  buildLines(why, swaps, lines, &writers);
  // The format names the tag (app/log.cpp keeps a player-profile line by its format's leading "[TAG"; MEM_RES is a
  // milestone tag there), the body follows it.
  static const size_t kTag = std::strlen("[MEM_RES] ");
  for (const std::string& l : lines)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[MEM_RES] %s", l.size() > kTag ? l.c_str() + kTag : "");
  const std::string patch = patchLine(why);
  SR_LOG_BOOT(sr::LogSeverity::Info, "[MEM_RES] %s", patch.c_str() + kTag);
  std::vector<std::string> dw;
  dwatchLines(dw);
  for (const std::string& l : dw) SR_LOG_BOOT(sr::LogSeverity::Warn, "[MEM_RES] %s", l.c_str() + kTag);
  for (const std::string& l : writers)
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[MEM_RES] %s", l.size() > kTag ? l.c_str() + kTag : "");
}

std::string censusText(const char* why, uint64_t swaps) {
  st();
  std::string lines[kLines];
  std::vector<std::string> writers;
  buildLines(why, swaps, lines, &writers);
  std::string all;
  for (const std::string& l : lines) {
    all += l;
    all += '\n';
  }
  all += patchLine(why);
  all += '\n';
  for (const std::string& l : writers) {
    all += l;
    all += '\n';
  }
  return all;
}

void censusSetWitnessKForTest(uint32_t k) noexcept { g_witnessK.store(k, std::memory_order_relaxed); }

void censusSetBoundShrinkForTest(uint32_t k) noexcept { g_boundShrink.store(k, std::memory_order_relaxed); }

void censusSetLiteralsForTest(const LiteralRow* rows, size_t n) noexcept {
  g_litN.store(rows ? n : 0u, std::memory_order_relaxed);
  g_litRows.store(rows, std::memory_order_release);
}
const LiteralRow* censusLiterals(size_t& n) noexcept {
  n = kNLits;
  return kLits;
}

bool censusDrawExactForTest() noexcept { return t_draw.on && t_draw.exact; }
// mem-native (Astra round 4): the draw's converted indices (pm4StageVsf's list, a strip cut 0xFFFFFFFF) against the
// used-vertex bitmap the census marked from - an index it lacks counts vtx_stale (a coverage gap: the guest rewrote its
// index buffer between the census's read and the conversion).
void censusCheckUsed(const uint32_t* idx, uint32_t n) noexcept {
  if (!census() || !idx) return;
  const DrawState& d = t_draw;
  if (!d.on || !d.exact) return;
  uint64_t bad = 0;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t v = idx[i];
    if (v == 0xFFFFFFFFu) continue;
    if (v < d.vMin || v > d.vMax) {
      bad++;
      continue;
    }
    const uint32_t k = v - d.vMin;
    if (!((d.used[k >> 6] >> (k & 63u)) & 1u)) bad++;
  }
  if (bad) st().vtxStale.fetch_add(bad, std::memory_order_relaxed);
}
bool censusSlotBoundForTest(uint32_t slot, int64_t& lo, int64_t& hi, uint32_t* reason) noexcept {
  DrawState& d = t_draw;
  if (reason) *reason = kFbSlot;
  if (!d.on || !d.vs) {
    if (reason) *reason = kFbVs;
    return false;
  }
  const VsSlot* v = nullptr;
  for (uint32_t i = 0; i < d.vs->n; i++)
    if (d.vs->s[i].slot == slot) v = &d.vs->s[i];
  if (!v || !v->safe || !(v->cls & (kClsL1 | kClsL2))) {
    if (reason && v) *reason = v->fbr;
    return false;
  }
  if (!d.fam.done && d.any) famCompute(st(), d);
  if (!d.fam.ok || ((v->cls & kClsL2) && !d.fam.l2ok)) {
    if (reason) *reason = !d.fam.ok ? d.fam.reason : d.fam.l2reason;
    return false;
  }
  if (!slotIdxRange(d, *v, lo, hi)) {
    if (reason) *reason = kFbRange;
    return false;
  }
  return true;
}

void censusResetForTest(bool on) noexcept {
  g_testOverride.store(on ? 1 : 0, std::memory_order_relaxed);
  st().clear();
  st().reg.reset();
  t_kind = uint8_t(Kind::kOther);
  t_wt = WtChk{0, false, false};
  t_rc = RcTally{0, 0, 0};
}

void censusSetTestHooks(const TestHooks* h) noexcept {
  g_host.store(h && h->host ? h->host : hostDefault, std::memory_order_relaxed);
  g_ownerVer.store(h && h->ownerVer ? h->ownerVer : ownerVerDefault, std::memory_order_relaxed);
  g_protect.store(h && h->protect ? h->protect : protectDefault, std::memory_order_relaxed);
  g_trapLimit.store(h ? h->trapLimit : -1, std::memory_order_relaxed);
  g_afterTake.store(h ? h->afterTake : nullptr, std::memory_order_relaxed);
  g_nowMs.store(h ? h->nowMs : nullptr, std::memory_order_relaxed);
  g_keepSamples.store(h != nullptr, std::memory_order_relaxed);
}

std::string censusLastSampleForTest(int which) {
  if (which < 0 || which > 2) return std::string();
  std::lock_guard<std::mutex> lk(g_sampleMu);
  return g_lastSample[which];
}

}  // namespace sr::gfx::memres
