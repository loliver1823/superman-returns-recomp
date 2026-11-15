// Native allocation provenance and alias mutations; Oracle-only H-WIN/H-UNIQ diagnostics.
#include "phys_window.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "guest_context.hpp"
#include "tex_census.hpp"   // iter-458 (Astra 068): grant lifetimes feed the texture census
#include "gpu_phys_route.hpp"   // iter-458 (Astra 072): HR1's capture snapshot at the close
#include "phys_alloc.hpp"
#include "../app/log.hpp"

#include "sr_oracle.hpp"

// The HLE trace recorder's published open state (gfx/hle_video.cpp), read by
// the PWS2 window capture to know when the SRHT window has closed.
#if SR_ORACLE
namespace sr { namespace gfx { namespace hle {
uint64_t txcFifoPublished();   // Astra 069: the census clock around grant mutations
bool traceRecorderOpen();
uint32_t traceRecorderCloseCount();   // Astra 062: the H-UNIQ seal trigger
void setTraceCloseHook(void (*hook)());   // Astra 063: synchronous seal at close
} } }
#endif

namespace sr {
namespace kernel {
namespace physwin {
namespace {

std::shared_mutex g_grantMu;
// iter-458 (Astra 054): each grant also carries its allocation SERIAL - one
// counter shared by both registries, so the relative age of the two owners of a
// double-booked physical range is recorded, not inferred.
struct GrantRec {
  uint32_t end;      // exclusive
  uint32_t serial;   // 1-based registration order across BOTH maps
  uint8_t state;     // Astra 058: 0 live, 1 released-but-retained (guest freed, pages kept)
};
std::atomic<uint32_t> g_grantSerial{0};
std::atomic<uint64_t> g_grantGen{0};   // every registry mutation, under the unique lock
// iter-465 native lane (CP costs): which physical range each generation touched, so a memoised
// window answer stays valid across mutations elsewhere (the guest allocates every frame; a
// global generation alone made gpuWindow's memo miss ~40% of the time). Written under the
// unique lock BEFORE the generation moves; entry (gen - 1) % kGrantLog belongs to generation gen.
struct GrantMut { uint32_t lo, hi; };
constexpr uint32_t kGrantLog = 4096;
GrantMut g_grantLog[kGrantLog];
// iter-466 cp round 2 (SR_CP_OWN_MEMO): one mutation counter per 64 KB PHYSICAL page, bumped
// by every registry mutation and every command-buffer note (and eviction) that touches the
// page. Every window answer for a physical range depends only on the registry records and
// command-buffer notes overlapping it, so the sum of its pages' counters is that answer's
// version: unchanged sum (the counters only grow) = no mutation touched the range. A VA range
// that crosses a 512 MB boundary wraps (0x9F000000.. covers the top and the bottom of phys).
constexpr uint32_t kMutShift = 16, kMutPages = 0x20000000u >> kMutShift;
std::atomic<uint32_t> g_pgMut[kMutPages];
inline void pgMutBump(uint32_t physLo, uint64_t bytes) {
  if (!bytes) return;
  const uint64_t p0 = (physLo & 0x1FFFFFFFu) >> kMutShift;
  const uint64_t n = bytes >= 0x20000000ull ? kMutPages
                                            : ((uint64_t(physLo & 0x1FFFFFFFu) + bytes - 1u) >> kMutShift) - p0 + 1u;
  for (uint64_t i = 0; i < n && i < kMutPages; i++)
    g_pgMut[(p0 + i) % kMutPages].fetch_add(1, std::memory_order_release);
}
inline void grantMutated(uint32_t va, uint64_t end) {
  pgMutBump(va, end > va ? end - va : 1u);
  const uint64_t g = g_grantGen.load(std::memory_order_relaxed);
  const uint32_t lo = va & 0x1FFFFFFFu;
  uint64_t hi = uint64_t(lo) + (end > va ? end - va : 1u);
  if (hi > 0x20000000ull) hi = 0x20000000ull;
  g_grantLog[g % kGrantLog] = GrantMut{lo, uint32_t(hi)};
  g_grantGen.fetch_add(1, std::memory_order_acq_rel);
}
std::map<uint32_t, GrantRec> g_grants;   // heap-routed: start -> {end, serial}
// iter-458 (Astra 043): genuine GPU physical grants (the precise owner model).
std::map<uint32_t, GrantRec> g_physGrants;   // start -> {end, serial}, under g_grantMu
std::map<uint32_t, GrantRec> g_stackGrants;  // H-UNIQ census only (kind 3), under g_grantMu
// iter-460: live grants per 64 KB physical page, per side (0xA = a physical
// grant at 0xA0000000|p; 8 = a heap-routed grant or a physical grant that
// spilled below 0xA0000000). Lock-free reads for foldPhysAlias (every guest
// load/store through the 0xC / 0xE windows); written under g_grantMu.
constexpr uint32_t kPageShift = 16, kPages = 0x20000000u >> kPageShift;
std::atomic<uint16_t> g_pgA[kPages];
std::atomic<uint16_t> g_pg8[kPages];
void pageAdjustLocked(int kind, uint32_t va, uint32_t end, int delta) {
  if ((kind != 1 && kind != 2) || end <= va) return;
  // A physical grant that straddles 0xA0000000 (the 224 MB world pool spills
  // below it) is two ranges: its VAs below 0xA0000000 are 0x8-window VAs
  // (phys = va & 0x1FFFFFFF, counted on the 8 side) and its VAs from
  // 0xA0000000 up are 0xA-window VAs (phys = va - 0xA0000000, the A side).
  // One side chosen from the start VA, with the phys span clamped at
  // 0x1FFFFFFF, dropped the pool's whole upper part: physPageOwner said "no
  // 0xA owner" where gpuWindow's range check said 0xA, so the game's
  // write-combined (0xC) stores of an index buffer landed in 0x8 while the
  // renderer read 0xA (the title sky strip: 591 zero indices).
  auto count = [&](uint32_t lo, uint32_t hi, int side) {
    if (hi <= lo) return;
    const uint64_t p0 = lo & 0x1FFFFFFFu;
    const uint64_t p1 = p0 + (hi - lo) - 1u;
    std::atomic<uint16_t>* t = side == 0xA ? g_pgA : g_pg8;
    for (uint64_t pg = p0 >> kPageShift; pg <= (p1 >> kPageShift) && pg < kPages; pg++)
      t[pg].fetch_add(uint16_t(delta), std::memory_order_relaxed);
  };
  if (kind == 1) {
    count(va, end, 8);
    return;
  }
  const uint64_t split = std::min<uint64_t>(std::max<uint64_t>(va, 0xA0000000ull), end);
  count(va, uint32_t(split), 8);
  count(uint32_t(split), end, 0xA);
}

#if SR_ORACLE
const char* const kSiteNames[kSites] = {"idx-ib", "idx-vb", "s4-vb"};
const char* const kOwnNames[3] = {"none", "physRegion", "heapGrant"};

// The byte comparison is bounded so the census stays cheap on large spans.
constexpr uint32_t kCompareCap = 4096u;

struct SiteCounts {
  std::atomic<uint64_t> calls{0}, ok{0}, wrongDiffer{0}, wrongSame{0},
      bothDiffer{0}, bothSame{0}, none{0}, unreadable{0};
  std::atomic<uint32_t> wrongShown{0}, bothShown{0};
};
SiteCounts g_sites[kSites];

// iter-458 (Astra 029 B) resolve counters.
const char* const kRsNames[kRsSites] = {"idx-ib", "idx-vb", "s4-vb", "tex", "refresh"};
struct RsCounts {
  std::atomic<uint64_t> a{0}, w8{0}, both{0}, unknown{0}, validOk{0}, validBad{0};
  std::atomic<uint64_t> agree{0}, disagree{0}, dSelOk{0}, dSelBad{0}, dLegOk{0}, dLegBad{0};
  std::atomic<uint64_t> scopeSkip{0};   // Astra 045: grant mode, out-of-scope site -> legacy
};
RsCounts g_rs[kRsSites];

void reportResolve(int site) {
  const RsCounts& c = g_rs[site];
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[PHYS_RESOLVE] site=%s resolvedA=%llu resolved8=%llu BOTH(legacy)=%llu "
      "unknown(legacy)=%llu | validation ok=%llu BAD=%llu | vs legacy: agree=%llu "
      "disagree=%llu [selected ok=%llu BAD=%llu | legacy ok=%llu BAD=%llu] scopeSkip=%llu",
      kRsNames[site], (unsigned long long)c.a.load(), (unsigned long long)c.w8.load(),
      (unsigned long long)c.both.load(), (unsigned long long)c.unknown.load(),
      (unsigned long long)c.validOk.load(), (unsigned long long)c.validBad.load(),
      (unsigned long long)c.agree.load(), (unsigned long long)c.disagree.load(),
      (unsigned long long)c.dSelOk.load(), (unsigned long long)c.dSelBad.load(),
      (unsigned long long)c.dLegOk.load(), (unsigned long long)c.dLegBad.load(),
      (unsigned long long)c.scopeSkip.load());
}
bool sampleDue(uint64_t n) { return n <= 65536u ? (n & (n - 1)) == 0 : (n % 65536u) == 0; }

#endif

bool inMap(const std::map<uint32_t, GrantRec>& m, uint32_t va, uint32_t size) {
  const uint64_t end = (uint64_t)va + size;
  std::shared_lock<std::shared_mutex> lk(g_grantMu);
  auto it = m.upper_bound(va);
  if (it == m.begin()) return false;
  --it;
  return it->first <= va && end <= it->second.end;
}
#if SR_ORACLE
// The grant record CONTAINING va (start address only), for the window capture.
bool grantAt(const std::map<uint32_t, GrantRec>& m, uint32_t va, uint32_t& start,
             uint32_t& end, uint32_t& serial) {
  std::shared_lock<std::shared_mutex> lk(g_grantMu);
  auto it = m.upper_bound(va);
  if (it == m.begin()) return false;
  --it;
  if (!(it->first <= va && va < it->second.end)) return false;
  start = it->first;
  end = it->second.end;
  serial = it->second.serial;
  return true;
}
#endif

bool inGrant(uint32_t va, uint32_t size) { return inMap(g_grants, va, size); }
bool inPhysGrant(uint32_t va, uint32_t size) { return inMap(g_physGrants, va, size); }
// iter-460: a grant the guest released (MmFreePhysicalMemory, state 1) owns
// nothing any more - the console gives its pages to the next allocation.
bool inMapLive(const std::map<uint32_t, GrantRec>& m, uint32_t va, uint32_t size) {
  const uint64_t end = (uint64_t)va + size;
  std::shared_lock<std::shared_mutex> lk(g_grantMu);
  auto it = m.upper_bound(va);
  if (it == m.begin()) return false;
  --it;
  return it->first <= va && end <= it->second.end && it->second.state == 0;
}
// iter-461: does ANY (live) grant of the map overlap [va, va+size)? The
// whole-range containment above is per RECORD, and the heap registers its
// allocations per chunk, so a range spanning several chunks is "not inside"
// even though every page of it is owned - the question a range-level owner
// answer needs is whether the OTHER window touches the range at all.
bool overlapsMap(const std::map<uint32_t, GrantRec>& m, uint32_t va, uint32_t size, bool liveOnly) {
  const uint64_t end = (uint64_t)va + size;
  std::shared_lock<std::shared_mutex> lk(g_grantMu);
  auto it = m.upper_bound(va);
  if (it != m.begin()) {
    auto prev = std::prev(it);
    if (prev->second.end > va && (!liveOnly || prev->second.state == 0)) return true;
  }
  for (; it != m.end() && it->first < end; ++it)
    if (!liveOnly || it->second.state == 0) return true;
  return false;
}
bool gpuLiveOnly() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_OWNER_LIVE");
    return !e || e[0] != '0';
  }();
  return v;
}
bool inGrantG(uint32_t va, uint32_t size) {
  return gpuLiveOnly() ? inMapLive(g_grants, va, size) : inMap(g_grants, va, size);
}
bool inPhysGrantG(uint32_t va, uint32_t size) {
  return gpuLiveOnly() ? inMapLive(g_physGrants, va, size) : inMap(g_physGrants, va, size);
}

// ── the grant-model counters and the sampler (Astra 043) ──
#if SR_ORACLE
struct GrantSiteCounts {
  std::atomic<uint64_t> cat[kGCats];
  std::atomic<uint64_t> seen[kGCats], sampled[kGCats];
};
GrantSiteCounts g_gsites[kSites];
std::atomic<uint64_t> g_drawSeq{0};
thread_local DrawCtx t_ctx;
thread_local uint64_t t_drawId = 0;
// Astra 044: the legacy choice of the resolver decision made at this read (set by
// resolveCompare, consumed by census at the same site). -1 = none pending.
thread_local int t_cmpSite = -1;
thread_local int t_cmpLegacyWin = 0;

std::mutex g_sampleMu;
FILE* g_sampleFile = nullptr;
bool g_sampleOpenTried = false;
std::string g_sampleTestPath;   // non-empty = test override
bool g_sampleTestSet = false;
uint64_t g_sampleWritten = 0;

constexpr uint64_t kHashCap = 16ull << 20;

uint64_t fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

// Fault-free candidate read: the first min(size,64) bytes and the full-span hash.
bool readCandidate(uint32_t va, uint32_t size, uint8_t (&out)[64], uint64_t& hash,
                   bool& truncated) {
  const uint8_t* p = guestVAToHost(va);
  const uint64_t n = size < kHashCap ? size : kHashCap;
  truncated = size > kHashCap;
  if (!p || !n || !hostRangeReadable(p, (size_t)n)) { hash = 0; return false; }
  const uint32_t k = size < 64u ? size : 64u;
  std::memcpy(out, p, k);
  hash = fnv1a(p, (size_t)n);
  return true;
}

bool sampleDueStratum(uint64_t n) { return n <= 2000u || (n % 1024u) == 0; }

FILE* sampleFileLocked() {
  if (g_sampleFile || g_sampleOpenTried) return g_sampleFile;
  g_sampleOpenTried = true;
  const char* path = nullptr;
  if (g_sampleTestSet) path = g_sampleTestPath.empty() ? nullptr : g_sampleTestPath.c_str();
  else path = std::getenv("SR_PHYSWIN_SAMPLE");
  if (!path || !*path) return nullptr;
  g_sampleFile = std::fopen(path, "wb");
  SR_LOG_BOOT(g_sampleFile ? sr::LogSeverity::Warn : sr::LogSeverity::Error,
      "[PHYSWIN_SAMPLE] %s '%s' (record %zu B, first 2000 per site x grant-category, then 1/1024)",
      g_sampleFile ? "writing" : "FAILED to open", path, sizeof(SampleRec));
  return g_sampleFile;
}

void writeSample(Site site, int catLegacy, int catGrant, bool chose8, uint32_t p,
                 uint32_t size, uint64_t seen, int legacyWin) {
  SampleRec r;
  std::memset(&r, 0, sizeof(r));
  std::memcpy(r.magic, "PWS1", 4);
  r.site = (uint8_t)site;
  r.catLegacy = (uint8_t)catLegacy;
  r.catGrant = (uint8_t)catGrant;
  r.phys = p;
  r.size = size;
  r.sampledLen = size < 64u ? size : 64u;
  r.drawId = t_drawId;
  r.presentId = t_ctx.presentId;
  r.seenInStratum = seen;
  bool tr8 = false, trA = false;
  const bool ok8 = readCandidate(0x80000000u | p, size, r.bytes8, r.hash8, tr8);
  const bool okA = readCandidate(0xA0000000u | p, size, r.bytesA, r.hashA, trA);
  r.flags = (uint8_t)((chose8 ? 1 : 0) | (ok8 ? 2 : 0) | (okA ? 4 : 0) | ((tr8 || trA) ? 8 : 0) |
                      (legacyWin ? 16 : 0) | (legacyWin == 8 ? 32 : 0));
  r.prim = t_ctx.prim;
  r.stride = t_ctx.stride;
  r.count = t_ctx.count;
  r.startIndex = t_ctx.startIndex;
  r.minIdx = t_ctx.minIdx;
  r.maxIdx = t_ctx.maxIdx;
  r.baseVertex = t_ctx.baseVertex;
  r.idx32 = t_ctx.idx32;
  r.idxEndian = t_ctx.idxEndian;
  r.vbEndian = t_ctx.vbEndian;
  std::lock_guard<std::mutex> lk(g_sampleMu);
  FILE* f = sampleFileLocked();
  if (!f) return;
  std::fwrite(&r, sizeof(r), 1, f);
  if ((++g_sampleWritten & 63u) == 0) std::fflush(f);
}

// ── iter-458 (Astra 054): the PWS2 per-draw window capture ──
constexpr uint32_t kWinHashCap = 512u;
std::mutex g_winMu;
std::vector<WinRec> g_winRing;          // sized to kWinRing on first append
uint64_t g_winHead = 0;                 // records ever appended
bool g_winSeenOpen = false;
std::atomic<bool> g_winFlushed{false};
uint64_t g_winFlushedCount = 0;
std::atomic<uint64_t> g_winSeq{0};
std::string g_winTestPath;
bool g_winTestSet = false;
bool (*g_winOpenProbe)() = nullptr;

const char* winPath() {
  if (g_winTestSet) return g_winTestPath.empty() ? nullptr : g_winTestPath.c_str();
  static const std::string s_path = [] {
    const char* e = std::getenv("SR_PHYSWIN_SAMPLE_WIN");
    return std::string(e ? e : "");
  }();
  return s_path.empty() ? nullptr : s_path.c_str();
}

// Fault-free: the first min(size,128) bytes and FNV over min(size, 512).
bool readCandidate128(uint32_t va, uint32_t size, uint8_t (&out)[128], uint64_t& hash) {
  const uint8_t* p = guestVAToHost(va);
  const uint32_t n = size < kWinHashCap ? size : kWinHashCap;
  if (!p || !n || !hostRangeReadable(p, (size_t)n)) { hash = 0; return false; }
  std::memcpy(out, p, size < 128u ? size : 128u);
  hash = fnv1a(p, n);
  return true;
}

void fillWinGrant(WinGrant& g, uint32_t va, uint32_t size) {
  std::memset(&g, 0, sizeof(g));
  uint32_t s = 0, e = 0, ser = 0;
  if (grantAt(g_physGrants, va, s, e, ser)) g.kind = 2;
  else if (grantAt(g_grants, va, s, e, ser)) g.kind = 1;
  else return;
  g.start = s;
  g.end = e;
  g.serial = ser;
  g.coversSpan = ((uint64_t)va + size <= (uint64_t)e) ? 1 : 0;
}

void winFlushLocked(const char* path) {
  const uint64_t n = g_winHead < kWinRing ? g_winHead : kWinRing;
  const uint64_t first = g_winHead - n;
  FILE* f = std::fopen(path, "wb");
  if (f) {
    for (uint64_t i = 0; i < n; i++)
      std::fwrite(&g_winRing[(size_t)((first + i) % kWinRing)], sizeof(WinRec), 1, f);
    std::fclose(f);
  }
  g_winFlushedCount = f ? n : 0;
  g_winFlushed.store(true, std::memory_order_release);
  SR_LOG_BOOT(f ? sr::LogSeverity::Warn : sr::LogSeverity::Error,
      "[PHYSWIN_WIN] %s %llu records (appended %llu; seq %llu..%llu) -> '%s'",
      f ? "flushed" : "FAILED to open for", (unsigned long long)n,
      (unsigned long long)g_winHead,
      (unsigned long long)(n ? g_winRing[(size_t)(first % kWinRing)].seq : 0),
      (unsigned long long)(n ? g_winRing[(size_t)((g_winHead - 1) % kWinRing)].seq : 0), path);
}

void winCapture(Site site, int catLegacy, int catGrant, bool chose8, uint32_t p,
                uint32_t size, int legacyWin) {
  const char* path = winPath();
  if (!path || g_winFlushed.load(std::memory_order_acquire)) return;
  const bool open = g_winOpenProbe ? g_winOpenProbe() : sr::gfx::hle::traceRecorderOpen();
  WinRec r;
  std::memset(&r, 0, sizeof(r));
  std::memcpy(r.magic, "PWS2", 4);
  r.site = (uint8_t)site;
  r.catLegacy = (uint8_t)catLegacy;
  r.catGrant = (uint8_t)catGrant;
  r.phys = p;
  r.size = size;
  r.seq = g_winSeq.fetch_add(1, std::memory_order_relaxed) + 1;
  r.drawId = t_drawId;
  r.presentIdConsumer = t_ctx.presentId;
  r.prim = t_ctx.prim;
  r.stride = t_ctx.stride;
  r.count = t_ctx.count;
  r.startIndex = t_ctx.startIndex;
  r.minIdx = t_ctx.minIdx;
  r.maxIdx = t_ctx.maxIdx;
  r.baseVertex = t_ctx.baseVertex;
  r.idx32 = t_ctx.idx32;
  r.idxEndian = t_ctx.idxEndian;
  r.vbEndian = t_ctx.vbEndian;
  const uint32_t va8 = 0x80000000u | p, vaA = 0xA0000000u | p;
  fillWinGrant(r.g8, va8, size);
  fillWinGrant(r.gA, vaA, size);
  const bool ok8 = readCandidate128(va8, size, r.bytes8, r.hash8);
  const bool okA = readCandidate128(vaA, size, r.bytesA, r.hashA);
  r.flags = (uint8_t)((chose8 ? 1 : 0) | (ok8 ? 2 : 0) | (okA ? 4 : 0) |
                      (size > kWinHashCap ? 8 : 0) | (legacyWin ? 16 : 0) |
                      (legacyWin == 8 ? 32 : 0));
  std::lock_guard<std::mutex> lk(g_winMu);
  if (g_winFlushed.load(std::memory_order_relaxed)) return;
  if (open) {
    g_winSeenOpen = true;
  } else if (g_winSeenOpen) {
    winFlushLocked(path);   // the window has closed: write it, once
    return;
  }
  if (g_winRing.empty()) g_winRing.resize(kWinRing);
  g_winRing[(size_t)(g_winHead % kWinRing)] = r;
  g_winHead++;
}

void reportGrant(const char* why) {
  char line[1200];
  size_t o = 0;
  for (int s = 0; s < kSites && o + 300 < sizeof(line); s++) {
    const GrantSiteCounts& g = g_gsites[s];
    o += (size_t)snprintf(line + o, sizeof(line) - o,
        " | %s none=%llu ok=%llu WRONG=%llu BOTH=%llu seen/sampled "
        "none=%llu/%llu ok=%llu/%llu wrong=%llu/%llu both=%llu/%llu",
        kSiteNames[s], (unsigned long long)g.cat[0].load(), (unsigned long long)g.cat[1].load(),
        (unsigned long long)g.cat[2].load(), (unsigned long long)g.cat[3].load(),
        (unsigned long long)g.seen[0].load(), (unsigned long long)g.sampled[0].load(),
        (unsigned long long)g.seen[1].load(), (unsigned long long)g.sampled[1].load(),
        (unsigned long long)g.seen[2].load(), (unsigned long long)g.sampled[2].load(),
        (unsigned long long)g.seen[3].load(), (unsigned long long)g.sampled[3].load());
  }
  size_t pg = 0;
  {
    std::shared_lock<std::shared_mutex> lk(g_grantMu);
    pg = g_physGrants.size();
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn, "[PHYSWIN_G] site=%s model=grants/1 physGrants=%zu%s",
              why, pg, o ? line : "");
}

// Fault-free (the harness VEH makes any host AV fatal): 1 = differ, 0 = same,
// -1 = a window is unreadable.
int bytesDiffer(uint32_t va8, uint32_t vaA, uint32_t size) {
  const uint32_t n = size < kCompareCap ? size : kCompareCap;
  const uint8_t* p8 = guestVAToHost(va8);
  const uint8_t* pA = guestVAToHost(vaA);
  if (!p8 || !pA || !hostRangeReadable(p8, n) || !hostRangeReadable(pA, n))
    return -1;
  return std::memcmp(p8, pA, n) != 0 ? 1 : 0;
}

void report(const char* why) {
  char line[900];
  size_t o = 0;
  for (int s = 0; s < kSites && o + 200 < sizeof(line); s++) {
    const SiteCounts& c = g_sites[s];
    o += (size_t)snprintf(line + o, sizeof(line) - o,
        " | %s calls=%llu ok=%llu WRONG(differ=%llu same=%llu) "
        "BOTH(differ=%llu same=%llu) none=%llu unreadable=%llu",
        kSiteNames[s], (unsigned long long)c.calls.load(),
        (unsigned long long)c.ok.load(), (unsigned long long)c.wrongDiffer.load(),
        (unsigned long long)c.wrongSame.load(),
        (unsigned long long)c.bothDiffer.load(),
        (unsigned long long)c.bothSame.load(), (unsigned long long)c.none.load(),
        (unsigned long long)c.unreadable.load());
  }
  uint32_t lo = 0, hi = 0;
  guestPhysRegion(lo, hi);
  size_t grants = 0;
  {
    std::shared_lock<std::shared_mutex> lk(g_grantMu);
    grants = g_grants.size();
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[PHYSWIN] site=%s physRegion=[%08X,%08X) heapGrants=%zu%s", why, lo, hi,
      grants, o ? line : "");
}

#endif

}  // namespace

// ── iter-458 H-UNIQ census (Astra 058/059) ───────────────────────────────────
namespace {
#if SR_ORACLE
std::atomic<int> g_uniqForce{-1};
std::atomic<uint64_t> g_uniqEvents{0};
std::atomic<uint64_t> g_uniqLifeEvents{0};
UniqSticky g_sticky;   // under g_grantMu (unique for writes)
bool g_regOverlapped[4] = {};   // [kind]: the registry has ever held VA-overlapping entries
#endif
// iter-463 F2: the same fact, maintained at EVERY kind-1/2 insert (g_regOverlapped
// is only maintained while the census or txc counts), for the double-book scan.
bool g_dbOverlapped[4] = {};
// Astra 065: the harness main stack is a grant route too (host-owned, inside
// the physical bump region) - registered once as a kind-3 grant so any grant
// overlapping it is a live-live 23 pair. Env-enabled census only (tests opt in).
#if SR_ORACLE
std::atomic<bool> g_noteMainStack{false};
bool g_mainStackNoted = false;

// Astra 060: the durable sticky record (SR_PHYS_UNIQ_STICKY_FILE=<path>). Runs
// end in TerminateProcess, so the terminal state is PUBLISHED at every update:
// two slots, each [seq u64][record][seq u64], written alternately (seq & 1) and
// flushed to the OS - data handed to the OS survives the process being killed.
// A slot is complete iff both seq words match; the reader takes the complete
// slot with the highest seq = the state after the last COMPLETED update.
// Astra 061: every fseek/fwrite/fflush result is checked and publication is
// FAIL-CLOSED - the first failure latches, stops all later publication (a later
// success must never re-validate a record that skipped an update), poisons the
// header magic ('UQSX'; itself checked), and is reported as an Error at once and
// on every ticker line. Acceptance (tools/uniq_sticky_read.py --accept) refuses
// a poisoned record, any PUBLICATION FAILED line, and a record older than the
// last seq a ticker line reported.
constexpr uint32_t kStickyMagic = 0x31535155u;    // 'UQS1'
constexpr uint32_t kStickyPoison = 0x58535155u;   // 'UQSX'
constexpr size_t kStickyHdr = 16;
constexpr size_t kStickySlot = 8 + kUniqStickyRecBytes + 8;
std::FILE* g_stickyFile = nullptr;
uint64_t g_stickySeq = 0;           // the last seq PUBLISHED successfully
bool g_stickyFailed = false;
uint64_t g_stickyFailSeq = 0;
int g_stickyFailInject = 0;         // test: 1 fail the next slot write, 2 also the poison write
std::string g_stickyPath;           // the record path ("" = no record)
uint32_t g_stickyNonce = 0;         // Astra 062: this acquisition's identity
uint32_t g_stickyNonceForTest = 0;  // Astra 063: test override (0 = env / random)
bool g_stickySealed = false;        // Astra 062: publication frozen by the seal
uint8_t g_stickyLastRec[kUniqStickyRecBytes] = {};   // the record at g_stickySeq
// Astra 073 test seam: runs inside the capture endpoint, between the seal and
// HR1's snapshot, with both locks held.
std::atomic<void (*)()> g_closeMidHook{nullptr};

void putU64(uint8_t*& o, uint64_t v) { std::memcpy(o, &v, 8); o += 8; }
void putU32(uint8_t*& o, uint32_t v) { std::memcpy(o, &v, 4); o += 4; }
uint64_t getU64(const uint8_t*& i) { uint64_t v; std::memcpy(&v, i, 8); i += 8; return v; }
uint32_t getU32(const uint8_t*& i) { uint32_t v; std::memcpy(&v, i, 4); i += 4; return v; }

bool g_stickyTried = false;

// One checked positioned write + flush.
bool stickyWriteAt(size_t off, const uint8_t* p, size_t n) {
  if (!g_stickyFile) return false;
  if (std::fseek(g_stickyFile, (long)off, SEEK_SET) != 0) return false;
  if (std::fwrite(p, 1, n, g_stickyFile) != n) return false;
  return std::fflush(g_stickyFile) == 0;
}

void stickyFailLocked(uint64_t seq, const char* what) {
  g_stickyFailed = true;
  g_stickyFailSeq = seq;
  uint8_t hdr[4];
  uint8_t* o = hdr;
  putU32(o, kStickyPoison);
  const bool poisoned = g_stickyFailInject < 2 && stickyWriteAt(0, hdr, sizeof(hdr));
  SR_LOG_BOOT(sr::LogSeverity::Error,
              "[PHYS_UNIQ_STICKY] PUBLICATION FAILED at seq=%llu (%s; header poison %s) - the durable "
              "record is INVALID for this acquisition",
              (unsigned long long)seq, what, poisoned ? "written" : "ALSO FAILED");
}

// Header + two empty slots, written and checked once at open.
void stickyInitLocked() {
  uint8_t init[kStickyHdr + 2 * kStickySlot] = {};
  uint8_t* o = init;
  putU32(o, kStickyMagic);
  putU32(o, 1u);
  putU32(o, (uint32_t)kUniqStickyRecBytes);
  // Astra 063: the launcher SUPPLIES the nonce (SR_PHYS_UNIQ_NONCE=<hex>, fresh
  // per run) so acceptance can require the expected acquisition - a stale pair
  // left at a reused path carries another nonce. Absent: random (unacceptable
  // without an expected nonce anyway).
  uint32_t n = g_stickyNonceForTest;
  if (!n) {
    const char* e = std::getenv("SR_PHYS_UNIQ_NONCE");
    if (e && *e) n = (uint32_t)std::strtoul(e, nullptr, 16);
  }
  g_stickyNonce = n ? n : (std::random_device{}() | 1u);
  putU32(o, g_stickyNonce);
  std::memset(g_stickyLastRec, 0, sizeof(g_stickyLastRec));
  if (!stickyWriteAt(0, init, sizeof(init))) stickyFailLocked(0, "header write");
}

void stickyOpenLocked() {
  if (g_stickyTried) return;
  g_stickyTried = true;
  const char* path = std::getenv("SR_PHYS_UNIQ_STICKY_FILE");
  if (!path || !*path) return;
  g_stickyFile = std::fopen(path, "w+b");
  g_stickyPath = path;
  if (g_stickyFile) stickyInitLocked();
  SR_LOG_BOOT(g_stickyFile ? sr::LogSeverity::Warn : sr::LogSeverity::Error,
              "[PHYS_UNIQ_STICKY] durable record %s '%s' (%zu-byte slots x2) nonce=%08X",
              g_stickyFile ? "->" : "FAILED to open", path, kStickySlot, g_stickyNonce);
  if (!g_stickyFile) stickyFailLocked(0, "open");
}

void stickyPublishLocked() {
  stickyOpenLocked();
  if (!g_stickyFile || g_stickyFailed || g_stickySealed) return;
  const uint64_t seq = g_stickySeq + 1;
  uint8_t buf[kStickySlot];
  uint8_t* o = buf;
  putU64(o, seq);
  serializeUniqSticky(g_sticky, o);
  o += kUniqStickyRecBytes;
  putU64(o, seq);
  const bool ok = g_stickyFailInject == 0 &&
                  stickyWriteAt(kStickyHdr + (seq & 1u) * kStickySlot, buf, sizeof(buf));
  if (!ok) {
    stickyFailLocked(seq, g_stickyFailInject ? "injected slot write failure" : "slot write");
    return;
  }
  g_stickySeq = seq;
  std::memcpy(g_stickyLastRec, buf + 8, kUniqStickyRecBytes);
}

// Astra 062: POSITIVE, acquisition-bound terminal evidence. The acquisition
// ends when the live SRHT recorder closes (the census ticker polls the sticky
// close count); at that moment, under the registry lock, the seal is written to
// "<record>.seal" as its own file and publication FREEZES (so the sealed state
// is terminal by definition). The seal carries the acquisition nonce (also in
// the record header and the open log line), the failure latch, the last
// published seq and that seq's record bytes, with an FNV-1a-64 over it all.
// Acceptance requires a present, checksummed, nonce-matching, failure-free seal
// whose seq and record equal the record file's: anything missing REJECTS.
constexpr uint32_t kSealMagic = 0x45535155u;   // 'UQSE'
constexpr size_t kSealBytes = 40 + kUniqStickyRecBytes + 8;

uint64_t fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

void uniqSealLocked(const char* why) {
  if (g_stickySealed || g_stickyPath.empty()) return;
  g_stickySealed = true;   // freeze publication whether or not the seal lands
  uint8_t buf[kSealBytes] = {};
  uint8_t* o = buf;
  putU32(o, kSealMagic);
  putU32(o, 1u);
  putU32(o, g_stickyNonce);
  putU32(o, g_stickyFailed ? 1u : 0u);
  putU64(o, g_stickySeq);
  putU64(o, g_stickyFailSeq);
  putU32(o, (uint32_t)kUniqStickyRecBytes);
  putU32(o, 0u);
  std::memcpy(o, g_stickyLastRec, kUniqStickyRecBytes);
  o += kUniqStickyRecBytes;
  putU64(o, fnv1a64(buf, (size_t)(o - buf)));
  const std::string sp = g_stickyPath + ".seal";
  std::FILE* f = std::fopen(sp.c_str(), "wb");
  bool ok = f != nullptr;
  if (ok) ok = std::fwrite(buf, 1, sizeof(buf), f) == sizeof(buf);
  if (ok) ok = std::fflush(f) == 0;
  if (f && std::fclose(f) != 0) ok = false;
  SR_LOG_BOOT(ok && !g_stickyFailed ? sr::LogSeverity::Warn : sr::LogSeverity::Error,
              "[PHYS_UNIQ_STICKY] SEAL %s (%s) nonce=%08X seq=%llu publicationFailed=%d -> '%s'",
              ok ? "written" : "FAILED", why, g_stickyNonce, (unsigned long long)g_stickySeq,
              g_stickyFailed ? 1 : 0, sp.c_str());
}

#endif

struct PIv {
  uint32_t lo, hi;        // physical [lo, hi) - hi may be 0x20000000
  uint8_t kind, state;    // kind 1 heap-routed, 2 GPU physical, 3 thread stack
  uint32_t start, size, serial;
};

// Map a grant's VA range to its physical interval(s): p = va & 0x1FFFFFFF, so
// the range is split wherever it crosses a 512 MB window boundary.
void addIntervals(std::vector<PIv>& v, uint32_t start, uint32_t end, uint8_t kind,
                  uint8_t state, uint32_t serial) {
  uint64_t s = start;
  const uint64_t e = end;
  while (s < e) {
    const uint64_t winEnd = (s & ~0x1FFFFFFFull) + 0x20000000ull;
    const uint64_t segEnd = e < winEnd ? e : winEnd;
    const uint32_t plo = (uint32_t)(s & 0x1FFFFFFFull);
    v.push_back(PIv{plo, (uint32_t)(plo + (segEnd - s)), kind, state, start, end - start, serial});
    s = segEnd;
  }
}

#if SR_ORACLE
const char* const kPairNames[6] = {"11", "12", "13", "22", "23", "33"};

#endif

std::map<uint32_t, GrantRec>& registryOf(int kind) {
  return kind == 1 ? g_grants : kind == 2 ? g_physGrants : g_stackGrants;
}

#if SR_ORACLE
// Called with g_grantMu held UNIQUE, BEFORE the new grant is inserted: every
// live grant (any registry) whose physical interval overlaps the new one forms
// a live-live pair NOW. Each physical segment is looked up at all eight 512 MB
// VA aliases of every registry.
void stickyOnRegisterLocked(int kind, uint32_t va, uint32_t size, uint32_t serial) {
  UniqSticky& s = g_sticky;
  s.registrations++;
  std::vector<PIv> segs;
  addIntervals(segs, va, va + size, (uint8_t)kind, 0, serial);
  for (const PIv& sg : segs) {
    for (int k = 1; k <= 3; k++) {
      const std::map<uint32_t, GrantRec>& m = registryOf(k);
      for (uint64_t w = 0; w < 8; w++) {
        const uint64_t vlo = (w << 29) + sg.lo, vhi = (w << 29) + sg.hi;
        // Astra 060: while a registry is VA-DISJOINT (checked at every insert,
        // registryDisjointCheckLocked) the entry with the greatest start <= vlo is
        // the only one starting below vlo that can reach it - any earlier entry
        // ends before that one starts. Once disjointness has ever failed, scan
        // every entry starting below vhi (exhaustive, slower).
        auto it = m.begin();
        if (!g_regOverlapped[k]) {
          it = m.upper_bound((uint32_t)vlo);
          if (it != m.begin()) {
            auto pv = std::prev(it);
            if (pv->second.end > vlo) it = pv;
          }
        }
        for (; it != m.end() && it->first < vhi; ++it) {
          if (it->second.end <= vlo) continue;
          if (it->second.state) continue;                 // released: not live-live
          if (k == kind && it->first == va) continue;     // the entry this registration replaces
          const uint64_t lo = it->first > vlo ? it->first : vlo;
          const uint64_t hi = it->second.end < vhi ? it->second.end : vhi;
          if (lo >= hi) continue;
          const int pi = uniqPairIndex(kind, k);
          if (!s.llPairs[pi]) {
            s.exNew[pi] = UniqEx{va, size, serial};
            s.exOld[pi] = UniqEx{it->first, it->second.end - it->first, it->second.serial};
          }
          s.llPairs[pi]++;
          s.llBytes[pi] += hi - lo;
        }
      }
    }
  }
  if (kind == 1) return;
  if (va < 0xA0000000u) {
    (kind == 2 ? s.physBelowA : s.stackBelowA)++;
    if (!s.firstBelowAVA) s.firstBelowAVA = va;
  }
  uint32_t rlo = 0, rhi = 0;
  sr::kernel::guestPhysRegion(rlo, rhi);
  if (rlo < s.minPhysRegionLo) s.minPhysRegionLo = rlo;
  uint32_t hlo = 0, hhi = 0;
  sr::kernel::guestHeapRegion(hlo, hhi);
  if (hhi > hlo) {
    std::vector<PIv> hv;
    addIntervals(hv, hlo, hhi, 4, 0, 0);
    for (const PIv& sg : segs)
      for (const PIv& h : hv) {
        const uint32_t lo = sg.lo > h.lo ? sg.lo : h.lo;
        const uint32_t hi = sg.hi < h.hi ? sg.hi : h.hi;
        if (lo < hi) { s.heapSpanPairs++; s.heapSpanBytes += hi - lo; }
      }
  }
}

#endif

#if SR_ORACLE
// After an insert (g_grantMu unique): does the new entry overlap its neighbours
// in its OWN registry? Erases never create overlap, so checking every insert
// keeps g_regOverlapped[kind] exact; once set it stays set (released entries
// remain in the map).
void registryDisjointCheckLocked(int kind, uint32_t va) {
  std::map<uint32_t, GrantRec>& m = registryOf(kind);
  auto it = m.find(va);
  if (it == m.end()) return;
  bool ov = false;
  if (it != m.begin() && std::prev(it)->second.end > va) ov = true;
  auto nx = std::next(it);
  if (nx != m.end() && nx->first < it->second.end) ov = true;
  if (ov) {
    g_regOverlapped[kind] = true;
    g_sticky.registryOverlaps[kind - 1]++;
  }
}
#endif

// ── iter-463 F2: the physical double-book invariant (contract in the header) ──
std::atomic<uint64_t> g_dbCount{0}, g_dbPhysPhys{0}, g_dbEvents{0};
PhysDoubleBook g_dbLast;   // under g_grantMu (unique for writes)
std::atomic<void (*)(uint32_t, uint32_t)> g_dbHook{nullptr};

// Called with g_grantMu held UNIQUE, BEFORE the new record is inserted. Returns
// the number of distinct LIVE kind-1/kind-2 records whose physical interval
// overlaps the new grant's; *first = the first of them.
uint32_t doubleBookCheckLocked(int kind, uint32_t va, uint32_t size, uint32_t serial,
                               PhysDoubleBook* first) {
  if ((kind != 1 && kind != 2) || !size) return 0;
  std::vector<PIv> segs;
  addIntervals(segs, va, va + size, (uint8_t)kind, 0, serial);
  std::vector<std::pair<int, uint32_t>> seen;   // (kind, start) already counted
  const uint32_t lr = sr::kernel::getCurrentGuestLR();
  for (const PIv& sg : segs) {
    for (int k = 1; k <= 2; k++) {
      const std::map<uint32_t, GrantRec>& m = registryOf(k);
      for (uint64_t w = 0; w < 8; w++) {
        const uint64_t vlo = (w << 29) + sg.lo, vhi = (w << 29) + sg.hi;
        // As stickyOnRegisterLocked: the predecessor suffices while the
        // registry is VA-disjoint; once it has ever overlapped, scan from the start.
        auto it = m.begin();
        if (!g_dbOverlapped[k]) {
          it = m.upper_bound((uint32_t)vlo);
          if (it != m.begin()) {
            auto pv = std::prev(it);
            if (pv->second.end > vlo) it = pv;
          }
        }
        for (; it != m.end() && it->first < vhi; ++it) {
          if (it->second.end <= vlo || it->second.state) continue;   // released: owns nothing
          // A LIVE record at the new grant's own key is NOT skipped (review): a
          // released one was skipped just above and every allocation registers
          // once, so a live same-key record is the same VA granted twice while
          // the first grant lives - the plainest double-book there is.
          const std::pair<int, uint32_t> key{k, it->first};
          if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
          seen.push_back(key);
          if (seen.size() == 1 && first) {
            *first = PhysDoubleBook{va, size, serial, lr, it->first, it->second.end - it->first,
                                    it->second.serial, (uint8_t)kind, (uint8_t)k,
                                    (uint8_t)(k == kind && it->first == va)};
          }
        }
      }
    }
  }
  const uint32_t n = (uint32_t)seen.size();
  if (n) {
    g_dbCount.fetch_add(n, std::memory_order_relaxed);
    if (kind == 2) {
      uint64_t pp = 0;
      for (const auto& s : seen) pp += s.first == 2 ? 1u : 0u;
      g_dbPhysPhys.fetch_add(pp, std::memory_order_relaxed);
    }
    if (first) g_dbLast = *first;
  }
  return n;
}

void reportDoubleBook(uint32_t n, const PhysDoubleBook& d) {
  const uint64_t k = g_dbEvents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (k <= 16 || (k & (k - 1)) == 0) {
    const uint32_t p0 = d.newVa & 0x1FFFFFFFu, q0 = d.oldVa & 0x1FFFFFFFu;
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[PHYS_DOUBLE_BOOK] #%llu NEW kind%u va=%08X size=0x%X serial=%u lr=%08X phys=%08X "
                "overlaps LIVE kind%u va=%08X size=0x%X serial=%u phys=%08X (age %u registrations)"
                "%s%s | pairs total=%llu phys-phys=%llu",
                (unsigned long long)k, d.newKind, d.newVa, d.newSize, d.newSerial, d.newLr, p0,
                d.oldKind, d.oldVa, d.oldSize, d.oldSerial, q0, d.newSerial - d.oldSerial,
                d.sameVa ? " SAME-VA (granted twice)" : "", n > 1 ? " (+more)" : "", (unsigned long long)g_dbCount.load(),
                (unsigned long long)g_dbPhysPhys.load());
  }
  if (auto h = g_dbHook.load(std::memory_order_acquire)) h(d.newVa, d.oldVa);
}

void registerGrant(int kind, uint32_t va, uint32_t size) {
#if SR_ORACLE
  const bool census = uniqCensusEnabled();
#endif
  std::unique_lock<std::shared_mutex> lk(g_grantMu);
  // Astra 069: the census clock BEFORE the map write (the census reads the
  // upper bound after it), so the mutation lies inside [P0, P1].
#if SR_ORACLE
  const uint64_t txcP0 = txc::enabled() ? sr::gfx::hle::txcFifoPublished() : 0;
#endif
  const uint32_t serial = g_grantSerial.fetch_add(1, std::memory_order_relaxed) + 1;
#if SR_ORACLE
  // Astra 063: the acquisition ends at the recorder's close. The close seals
  // synchronously (uniqOnRecorderClose); should that hook be absent, the first
  // registration that observes the close seals BEFORE it is counted. After the
  // seal the census freezes: nothing later reaches the sticky state.
  if (census && !g_stickySealed && !g_stickyPath.empty() &&
      sr::gfx::hle::traceRecorderCloseCount() > 0)
    uniqSealLocked("close-observed-at-registration");
  const bool counting = census && !g_stickySealed;
  if (counting && !g_mainStackNoted && g_noteMainStack.load(std::memory_order_relaxed)) {
    g_mainStackNoted = true;
    const uint32_t ms = g_grantSerial.fetch_add(1, std::memory_order_relaxed) + 1;
    stickyOnRegisterLocked(3, sr::kernel::kMainStackBase, sr::kernel::kMainStackSize, ms);
    g_stackGrants[sr::kernel::kMainStackBase] =
        GrantRec{sr::kernel::kMainStackBase + sr::kernel::kMainStackSize, ms, 0};
    grantMutated(sr::kernel::kMainStackBase, uint64_t(sr::kernel::kMainStackBase) + sr::kernel::kMainStackSize);
    registryDisjointCheckLocked(3, sr::kernel::kMainStackBase);
    if (txc::enabled())
      txc::onGrantReg(3, sr::kernel::kMainStackBase, sr::kernel::kMainStackSize, ms, txcP0);
  }
  if (counting) stickyOnRegisterLocked(kind, va, size, serial);
#endif

  // iter-463 F2 (observe-only): before the insert, so the new record never
  // matches itself; the report and the hook run after the lock is released.
  PhysDoubleBook db;
  const uint32_t dbN = doubleBookCheckLocked(kind, va, size, serial, &db);
  uint64_t mutEnd = uint64_t(va) + size;
  {
    auto& reg = registryOf(kind);
    auto old = reg.find(va);
    if (old != reg.end() && old->second.state == 0) pageAdjustLocked(kind, va, old->second.end, -1);
    if (old != reg.end() && old->second.end > mutEnd) mutEnd = old->second.end;
  }
  registryOf(kind)[va] = GrantRec{va + size, serial, 0};
  pageAdjustLocked(kind, va, va + size, +1);
  grantMutated(va, mutEnd);
#if SR_ORACLE
  const bool txcOn = txc::enabled();
  if (counting || txcOn) registryDisjointCheckLocked(kind, va);   // exhaustive lookups
#endif
  if ((kind == 1 || kind == 2) && !g_dbOverlapped[kind]) {   // iter-463 F2: O(log n), always
    auto& reg = registryOf(kind);
    auto it = reg.find(va);
    if ((it != reg.begin() && std::prev(it)->second.end > va) ||
        (std::next(it) != reg.end() && std::next(it)->first < it->second.end))
      g_dbOverlapped[kind] = true;
  }
#if SR_ORACLE
  if (counting) stickyPublishLocked();
  if (txcOn) txc::onGrantReg((uint32_t)kind, va, size, serial, txcP0);
#endif
  lk.unlock();
  if (dbN) reportDoubleBook(dbN, db);
}

#if SR_ORACLE
// The recorder's close hook (gfx/hle_video.cpp noteRecorderClosed): the seal,
// taken under the registry lock - registrations serialized after it are out.
void uniqOnRecorderClose() {
  bool sealedNow = false;
  {
    // Astra 073: ONE capture endpoint. Under the registry lock AND HR1's
    // accounting lock (that order; nothing holding the latter enters the
    // registry), the H-UNIQ seal and HR1's snapshot observe the same instant:
    // no registration, admission or settlement falls between them, and the
    // snapshot names the seal it was taken with.
    std::unique_lock<std::shared_mutex> lk(g_grantMu);
    std::unique_lock<std::mutex> hl(sr::kernel::gpuPhysRouteAccountingMutex());
    if (!g_stickySealed && !g_stickyPath.empty()) {
      uniqSealLocked("recorder-closed");
      sealedNow = true;
    }
    if (void (*h)() = g_closeMidHook.load(std::memory_order_relaxed)) h();   // test seam
    sr::kernel::gpuPhysRouteCaptureSnapshotLocked("recorder-closed", sealedNow,
                                                  sealedNow ? g_stickyNonce : 0u,
                                                  sealedNow ? g_stickySeq : 0u);
  }
  if (sealedNow) uniqStickyReport("sealed");
  // Astra 068: the texture-path census ends its acquisition at the same edge.
  if (txc::enabled()) txc::flush("recorder-closed");
}

void uniqReport(const char* why) {
  const UniqSnapshot u = uniqSnapshot();
  char kinds[256];
  int n = 0;
  for (int i = 0; i < 6; i++)
    n += std::snprintf(kinds + n, sizeof(kinds) - (size_t)n, "%s%s=%llu/0x%llX", i ? " " : "",
                       kPairNames[i], (unsigned long long)u.pairsLL[i], (unsigned long long)u.bytesLL[i]);
  SR_LOG_BOOT(u.pairsLiveLive ? sr::LogSeverity::Warn : sr::LogSeverity::Info,
      "[PHYS_UNIQ] why=%s live=%llu released=%llu | overlapping interval pairs: "
      "live-live=%llu (0x%llX bytes) [by kind %s] live-released=%llu released-released=%llu | "
      "physGrantsBelowA=%llu lowestPhysGrantVA=%08X physRegionLo=%08X | heap %08X-%08X vs live phys/stack: "
      "pairs=%llu (0x%llX bytes) | first live-live: kind%u %08X+0x%X s%u "
      "<-> kind%u %08X+0x%X s%u",
      why, (unsigned long long)u.grantsLive, (unsigned long long)u.grantsReleased,
      (unsigned long long)u.pairsLiveLive, (unsigned long long)u.bytesLiveLive, kinds,
      (unsigned long long)u.pairsLiveReleased, (unsigned long long)u.pairsReleasedReleased,
      (unsigned long long)u.physGrantsBelowA, u.lowestPhysGrantVA, u.physRegionLo, u.heapLo, u.heapHi,
      (unsigned long long)u.heapBulkPairs, (unsigned long long)u.heapBulkBytes, u.exKindA,
      u.exA, u.exSizeA, u.exSerA, u.exKindB, u.exB, u.exSizeB, u.exSerB);
}

// Registrations are reported at powers of two, every 512, and always for a
// grant >= 16 MB (the overcommit pair); lifetime changes (reclaim / release)
// for the first 64, then at powers of two and every 256. Sampled - the STICKY
// counters are what cover the whole run.
void uniqMaybeReport(const char* why, bool lifetime, uint32_t size = 0) {
  if (!uniqCensusEnabled()) return;
  if (lifetime) {
    const uint64_t k = g_uniqLifeEvents.fetch_add(1, std::memory_order_relaxed) + 1;
    if (k <= 64 || (k & (k - 1)) == 0 || (k % 256u) == 0) uniqReport(why);
    return;
  }
  const uint64_t n = g_uniqEvents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (size >= 0x1000000u || (n & (n - 1)) == 0 || (n % 512u) == 0) uniqReport(why);
}

// The census ticker: a host wall-clock thread (never touches guest memory).
// Every 2 s, and at the SRHT recorder's open->closed edge, one sticky line.
void uniqTicker() {
  bool wasOpen = false;
  int ticks = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const bool open = sr::gfx::hle::traceRecorderOpen();
    if (wasOpen && !open) uniqStickyReport("recorder-closed");
    wasOpen = open;
    // Astra 062: the acquisition ends at the recorder's first close (sticky
    // count - a window shorter than this poll is not missed).
    if (sr::gfx::hle::traceRecorderCloseCount() > 0) {
      bool sealedNow = false;
      {
        std::unique_lock<std::shared_mutex> lk(g_grantMu);
        if (!g_stickySealed && !g_stickyPath.empty()) {
          uniqSealLocked("recorder-closed");
          sealedNow = true;
        }
      }
      if (sealedNow) uniqStickyReport("sealed");
    }
    if (++ticks % 8 == 0) {
      const UniqSnapshot u = uniqSnapshot();
      {
        std::unique_lock<std::shared_mutex> lk(g_grantMu);
        if (!g_stickySealed && u.heapBulkBytes > g_sticky.heapSpanBytesMaxTick) {
          g_sticky.heapSpanBytesMaxTick = u.heapBulkBytes;
          stickyPublishLocked();
        }
      }
      uniqStickyReport("tick");
    }
  }
}

void startTickerOnce() {
  static std::once_flag s_once;
  std::call_once(s_once, [] { std::thread(uniqTicker).detach(); });
}
#endif

}  // namespace

#if SR_ORACLE
int uniqPairIndex(int a, int b) {
  if (a > b) std::swap(a, b);
  if (a == 1) return b == 1 ? 0 : b == 2 ? 1 : 2;
  if (a == 2) return b == 2 ? 3 : 4;
  return 5;
}

void armRecorderCloseHook() {
  static std::once_flag s_hook;
  std::call_once(s_hook, [] { sr::gfx::hle::setTraceCloseHook(&uniqOnRecorderClose); });
}

bool uniqCensusEnabled() {
  const int f = g_uniqForce.load(std::memory_order_relaxed);
  if (f >= 0) return f == 1;
  static const bool s_on = [] {
    const char* e = std::getenv("SR_PHYS_UNIQ_CENSUS");
    return e && e[0] == '1';
  }();
  if (s_on) {   // env-enabled runs only; tests force the gate without a ticker
    g_noteMainStack.store(true, std::memory_order_relaxed);
    armRecorderCloseHook();
    startTickerOnce();
  }
  return s_on;
}

void setUniqCensusForTest(int mode) { g_uniqForce.store(mode, std::memory_order_relaxed); }

void serializeUniqSticky(const UniqSticky& s, uint8_t* out) {
  uint8_t* o = out;
  putU64(o, s.registrations);
  for (int i = 0; i < 6; i++) putU64(o, s.llPairs[i]);
  for (int i = 0; i < 6; i++) putU64(o, s.llBytes[i]);
  for (int i = 0; i < 6; i++) { putU32(o, s.exNew[i].va); putU32(o, s.exNew[i].size); putU32(o, s.exNew[i].serial); }
  for (int i = 0; i < 6; i++) { putU32(o, s.exOld[i].va); putU32(o, s.exOld[i].size); putU32(o, s.exOld[i].serial); }
  putU64(o, s.physBelowA);
  putU64(o, s.stackBelowA);
  putU32(o, s.firstBelowAVA);
  putU32(o, s.minPhysRegionLo);
  putU64(o, s.heapSpanPairs);
  putU64(o, s.heapSpanBytes);
  putU64(o, s.heapSpanBytesMaxTick);
  for (int i = 0; i < 3; i++) putU64(o, s.registryOverlaps[i]);
}

UniqSticky deserializeUniqSticky(const uint8_t* in) {
  UniqSticky s;
  const uint8_t* i8 = in;
  s.registrations = getU64(i8);
  for (int i = 0; i < 6; i++) s.llPairs[i] = getU64(i8);
  for (int i = 0; i < 6; i++) s.llBytes[i] = getU64(i8);
  for (int i = 0; i < 6; i++) { s.exNew[i].va = getU32(i8); s.exNew[i].size = getU32(i8); s.exNew[i].serial = getU32(i8); }
  for (int i = 0; i < 6; i++) { s.exOld[i].va = getU32(i8); s.exOld[i].size = getU32(i8); s.exOld[i].serial = getU32(i8); }
  s.physBelowA = getU64(i8);
  s.stackBelowA = getU64(i8);
  s.firstBelowAVA = getU32(i8);
  s.minPhysRegionLo = getU32(i8);
  s.heapSpanPairs = getU64(i8);
  s.heapSpanBytes = getU64(i8);
  s.heapSpanBytesMaxTick = getU64(i8);
  for (int i = 0; i < 3; i++) s.registryOverlaps[i] = getU64(i8);
  return s;
}

bool readUniqStickyFile(const char* path, UniqSticky* out, uint64_t* seq, int* completeSlots,
                        bool* poisoned) {
  if (poisoned) *poisoned = false;
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  uint8_t buf[kStickyHdr + 2 * kStickySlot] = {};
  const size_t n = std::fread(buf, 1, sizeof(buf), f);
  std::fclose(f);
  if (n < kStickyHdr) return false;
  const uint8_t* h = buf;
  const uint32_t magic = getU32(h);
  if (magic == kStickyPoison) {   // a publication failed: nothing in this file is terminal
    if (poisoned) *poisoned = true;
    return false;
  }
  if (magic != kStickyMagic || getU32(h) != 1u || getU32(h) != kUniqStickyRecBytes) return false;
  int complete = 0;
  uint64_t best = 0;
  const uint8_t* bestRec = nullptr;
  for (int k = 0; k < 2; k++) {
    const size_t off = kStickyHdr + (size_t)k * kStickySlot;
    if (n < off + kStickySlot) continue;
    const uint8_t* p = buf + off;
    const uint64_t a = getU64(p);
    const uint8_t* rec = p;
    p += kUniqStickyRecBytes;
    const uint64_t b = getU64(p);
    if (a == 0 || a != b) continue;
    complete++;
    if (a > best) { best = a; bestRec = rec; }
  }
  if (completeSlots) *completeSlots = complete;
  if (!bestRec) return false;
  if (out) *out = deserializeUniqSticky(bestRec);
  if (seq) *seq = best;
  return true;
}

void setUniqStickyFileForTest(const char* path) {
  std::unique_lock<std::shared_mutex> lk(g_grantMu);
  if (g_stickyFile) std::fclose(g_stickyFile);
  g_stickyFile = path ? std::fopen(path, "w+b") : nullptr;
  g_stickyTried = true;
  g_stickySeq = 0;
  g_stickyFailed = false;
  g_stickyFailSeq = 0;
  g_stickyFailInject = 0;
  g_stickySealed = false;
  g_stickyPath = path ? path : "";
  g_stickyNonce = 0;
  if (g_stickyFile) stickyInitLocked();
}

void reopenUniqStickyReadOnlyForTest(const char* path) {
  std::unique_lock<std::shared_mutex> lk(g_grantMu);
  if (g_stickyFile) std::fclose(g_stickyFile);
  g_stickyFile = std::fopen(path, "rb");   // every later write fails for real
  g_stickyTried = true;
}

void setUniqStickyFailInjectForTest(int mode) {
  std::unique_lock<std::shared_mutex> lk(g_grantMu);
  g_stickyFailInject = mode;
}

bool uniqStickyPublicationFailed(uint64_t* failSeq, uint64_t* publishedSeq) {
  std::shared_lock<std::shared_mutex> lk(g_grantMu);
  if (failSeq) *failSeq = g_stickyFailSeq;
  if (publishedSeq) *publishedSeq = g_stickySeq;
  return g_stickyFailed;
}

bool readUniqSeal(const char* sealPath, UniqSeal* out) {
  UniqSeal s;
  std::FILE* f = std::fopen(sealPath, "rb");
  if (!f) { if (out) *out = s; return false; }
  uint8_t buf[kSealBytes + 1] = {};
  const size_t n = std::fread(buf, 1, sizeof(buf), f);
  std::fclose(f);
  s.present = true;
  if (n != kSealBytes) { if (out) *out = s; return false; }
  const uint8_t* i = buf;
  const uint32_t magic = getU32(i), ver = getU32(i);
  s.nonce = getU32(i);
  s.failed = getU32(i);
  s.seq = getU64(i);
  s.failSeq = getU64(i);
  const uint32_t rec = getU32(i);
  getU32(i);
  std::memcpy(s.rec, i, kUniqStickyRecBytes);
  i += kUniqStickyRecBytes;
  const uint64_t sum = getU64(i);
  s.checksumOk = magic == kSealMagic && ver == 1u && rec == kUniqStickyRecBytes &&
                 sum == fnv1a64(buf, kSealBytes - 8);
  if (out) *out = s;
  return s.checksumOk;
}

uint32_t readUniqStickyNonce(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return 0;
  uint8_t h[kStickyHdr] = {};
  const size_t n = std::fread(h, 1, sizeof(h), f);
  std::fclose(f);
  if (n != kStickyHdr) return 0;
  const uint8_t* i = h;
  if (getU32(i) != kStickyMagic) return 0;
  getU32(i);
  getU32(i);
  return getU32(i);
}

// Astra 062: terminal acceptance from POSITIVE evidence only. Every failed rule
// is appended to *why (";"-separated); returns true only if none failed.
bool acceptUniqSticky(const char* recordPath, uint32_t expectedNonce, std::string* why) {
  std::string bad;
  if (expectedNonce == 0) bad += "S-expected: no expected acquisition nonce supplied;";
  UniqSeal seal;
  const bool sealOk = readUniqSeal((std::string(recordPath) + ".seal").c_str(), &seal);
  if (!seal.present) bad += "S-present: no seal;";
  else if (!sealOk) bad += "S-present: seal truncated or checksum/magic mismatch;";
  if (sealOk && seal.failed) bad += "S-ok: the seal records a publication failure;";
  UniqSticky rec;
  uint64_t seq = 0;
  bool poisoned = false;
  const bool recOk = readUniqStickyFile(recordPath, &rec, &seq, nullptr, &poisoned);
  if (!recOk) bad += poisoned ? "R-decode: record poisoned;" : "R-decode: no complete slot;";
  const uint32_t nonce = readUniqStickyNonce(recordPath);
  if (sealOk && (nonce == 0 || seal.nonce != nonce)) bad += "S-nonce: seal from another acquisition;";
  if (sealOk && expectedNonce != 0 && seal.nonce != expectedNonce)
    bad += "S-expected: the seal is not the expected acquisition's;";
  if (sealOk && recOk) {
    uint8_t bytes[kUniqStickyRecBytes];
    serializeUniqSticky(rec, bytes);
    if (seq == 0 || seal.seq != seq || std::memcmp(bytes, seal.rec, kUniqStickyRecBytes) != 0)
      bad += "S-match: the record is not the sealed state;";
  }
  if (why) *why = bad;
  return bad.empty();
}

void setUniqNoteMainStackForTest(bool on) { g_noteMainStack.store(on, std::memory_order_relaxed); }

void setUniqCloseMidHookForTest(void (*fn)()) {
  g_closeMidHook.store(fn, std::memory_order_relaxed);
}

void setUniqStickyNonceForTest(uint32_t nonce) {
  std::unique_lock<std::shared_mutex> lk(g_grantMu);
  g_stickyNonceForTest = nonce;
}

void armUniqCloseHookForTest(bool on) {
  sr::gfx::hle::setTraceCloseHook(on ? &uniqOnRecorderClose : nullptr);
}

uint32_t uniqStickyNonceForTest() {
  std::shared_lock<std::shared_mutex> lk(g_grantMu);
  return g_stickyNonce;
}

void uniqSealForTest(const char* why) {
  std::unique_lock<std::shared_mutex> lk(g_grantMu);
  uniqSealLocked(why);
}

void tearUniqStickyForTest() {
  std::unique_lock<std::shared_mutex> lk(g_grantMu);
  if (!g_stickyFile) return;
  const uint64_t seq = g_stickySeq + 1;   // the NEXT slot, begun but never finished
  uint8_t buf[8 + kUniqStickyRecBytes] = {};
  uint8_t* o = buf;
  putU64(o, seq);
  std::fseek(g_stickyFile, (long)(kStickyHdr + (seq & 1u) * kStickySlot), SEEK_SET);
  std::fwrite(buf, 1, sizeof(buf), g_stickyFile);
  std::fflush(g_stickyFile);
}

UniqSticky uniqSticky() {
  std::shared_lock<std::shared_mutex> lk(g_grantMu);
  return g_sticky;
}

void uniqStickyReport(const char* why) {
  const UniqSticky s = uniqSticky();
  uint64_t pubSeq = 0;
  const bool pubFailed = uniqStickyPublicationFailed(nullptr, &pubSeq);
  char kinds[512];
  int n = 0;
  for (int i = 0; i < 6; i++) {
    n += std::snprintf(kinds + n, sizeof(kinds) - (size_t)n, "%s%s=%llu/0x%llX", i ? " " : "",
                       kPairNames[i], (unsigned long long)s.llPairs[i], (unsigned long long)s.llBytes[i]);
    if (s.llPairs[i])
      n += std::snprintf(kinds + n, sizeof(kinds) - (size_t)n, "(%08X+0x%X s%u<-%08X+0x%X s%u)",
                         s.exNew[i].va, s.exNew[i].size, s.exNew[i].serial, s.exOld[i].va,
                         s.exOld[i].size, s.exOld[i].serial);
  }
  uint64_t ll = 0;
  for (int i = 0; i < 6; i++) ll += s.llPairs[i];
  SR_LOG_BOOT((ll || s.physBelowA || s.stackBelowA) ? sr::LogSeverity::Warn : sr::LogSeverity::Info,
      "[PHYS_UNIQ_STICKY] why=%s registrations=%llu | live-live pairs formed EVER by kind: %s | "
      "belowA EVER phys=%llu stack=%llu first=%08X | minPhysRegionLo=%08X | heap span EVER at "
      "registration pairs=%llu (0x%llX bytes), max at ticks 0x%llX | same-registry overlaps heap=%llu "
      "phys=%llu stack=%llu | durable seq=%llu%s",
      why, (unsigned long long)s.registrations, kinds, (unsigned long long)s.physBelowA,
      (unsigned long long)s.stackBelowA, s.firstBelowAVA, s.minPhysRegionLo,
      (unsigned long long)s.heapSpanPairs, (unsigned long long)s.heapSpanBytes,
      (unsigned long long)s.heapSpanBytesMaxTick, (unsigned long long)s.registryOverlaps[0],
      (unsigned long long)s.registryOverlaps[1], (unsigned long long)s.registryOverlaps[2],
      (unsigned long long)pubSeq, pubFailed ? " PUBLICATION FAILED - durable record INVALID" : "");
}

UniqSnapshot uniqSnapshot() {
  UniqSnapshot u;
  std::vector<PIv> v;
  {
    std::shared_lock<std::shared_mutex> lk(g_grantMu);
    v.reserve(g_grants.size() + g_physGrants.size() + g_stackGrants.size() + 64);
    for (const auto& kv : g_grants) {
      addIntervals(v, kv.first, kv.second.end, 1, kv.second.state, kv.second.serial);
      (kv.second.state ? u.grantsReleased : u.grantsLive)++;
    }
    for (const auto& kv : g_physGrants) {
      addIntervals(v, kv.first, kv.second.end, 2, kv.second.state, kv.second.serial);
      (kv.second.state ? u.grantsReleased : u.grantsLive)++;
      if (!kv.second.state) {
        if (kv.first < 0xA0000000u) u.physGrantsBelowA++;
        if (!u.lowestPhysGrantVA || kv.first < u.lowestPhysGrantVA) u.lowestPhysGrantVA = kv.first;
      }
    }
    for (const auto& kv : g_stackGrants) {
      addIntervals(v, kv.first, kv.second.end, 3, kv.second.state, kv.second.serial);
      (kv.second.state ? u.grantsReleased : u.grantsLive)++;
    }
  }
  std::sort(v.begin(), v.end(), [](const PIv& a, const PIv& b) { return a.lo < b.lo; });
  std::vector<const PIv*> active;
  for (const PIv& iv : v) {
    active.erase(std::remove_if(active.begin(), active.end(),
                                [&](const PIv* a) { return a->hi <= iv.lo; }),
                 active.end());
    for (const PIv* a : active) {
      const uint32_t ov = (a->hi < iv.hi ? a->hi : iv.hi) - iv.lo;
      if (!ov) continue;
      if (!a->state && !iv.state) {
        if (!u.pairsLiveLive) {
          u.exA = a->start; u.exSizeA = a->size; u.exSerA = a->serial; u.exKindA = a->kind;
          u.exB = iv.start; u.exSizeB = iv.size; u.exSerB = iv.serial; u.exKindB = iv.kind;
        }
        u.pairsLiveLive++;
        u.bytesLiveLive += ov;
        const int pi = uniqPairIndex(a->kind, iv.kind);
        u.pairsLL[pi]++;
        u.bytesLL[pi] += ov;
      } else if (a->state && iv.state) {
        u.pairsReleasedReleased++;
      } else {
        u.pairsLiveReleased++;
      }
    }
    active.push_back(&iv);
  }
  // Live GPU-physical / stack intervals against the heap bump's physical span.
  uint32_t rlo = 0, rhi = 0;
  sr::kernel::guestPhysRegion(rlo, rhi);
  u.physRegionLo = rlo;
  uint32_t hlo = 0, hhi = 0;
  sr::kernel::guestHeapRegion(hlo, hhi);
  u.heapLo = hlo;
  u.heapHi = hhi;
  if (hhi > hlo) {
    std::vector<PIv> hv;
    addIntervals(hv, hlo, hhi, 4, 0, 0);
    for (const PIv& iv : v) {
      if (iv.state || iv.kind == 1) continue;
      for (const PIv& h : hv) {
        const uint32_t lo = iv.lo > h.lo ? iv.lo : h.lo;
        const uint32_t hi = iv.hi < h.hi ? iv.hi : h.hi;
        if (lo < hi) { u.heapBulkPairs++; u.heapBulkBytes += hi - lo; }
      }
    }
  }
  return u;
}

#endif

void noteHeapRoutedGrant(uint32_t va, uint32_t size) {
  if (!va || !size) return;
  registerGrant(1, va, size);
#if SR_ORACLE
  uniqMaybeReport("heap-grant", false, size);
#endif
}

void notePhysGrant(uint32_t va, uint32_t size) {
  if (!va || !size) return;
  registerGrant(2, va, size);
#if SR_ORACLE
  uniqMaybeReport("phys-grant", false, size);
#endif
}

#if SR_ORACLE
void noteStackGrant(uint32_t va, uint32_t size) {
  if (!va || !size) return;
  registerGrant(3, va, size);
  uniqMaybeReport("stack-grant", false, size);
}

void uniqLifetimeReport(const char* why) { uniqMaybeReport(why, true); }

#endif

void forgetPhysGrant(uint32_t va, bool report) {
  {
    std::unique_lock<std::shared_mutex> lk(g_grantMu);
    auto it = g_physGrants.find(va);
    if (it != g_physGrants.end()) {
#if SR_ORACLE
      const uint64_t txcP0 = txc::enabled() ? sr::gfx::hle::txcFifoPublished() : 0;
      const uint32_t serial = it->second.serial;
#endif
      if (it->second.state == 0) pageAdjustLocked(2, va, it->second.end, -1);
      const uint64_t mutEnd = it->second.end;
      g_physGrants.erase(it);
      grantMutated(va, mutEnd);
#if SR_ORACLE
      if (txc::enabled()) txc::onGrantRel(2, va, serial, 2, txcP0);   // reclaimed
#endif
    }
  }
#if SR_ORACLE
  if (report) uniqMaybeReport("reclaimed", true);
#endif
}

uint32_t forgetReleasedPhysGrantsIn(uint32_t va, uint32_t size) {
  if (!size) return 0;
  const uint64_t end = (uint64_t)va + size;
  auto candidates = [&](std::vector<uint32_t>& out) {
    auto it = g_physGrants.upper_bound(va);
    if (it != g_physGrants.begin()) {
      auto pv = std::prev(it);
      if (pv->second.end > va) it = pv;
    }
    for (; it != g_physGrants.end() && it->first < end; ++it)
      if (it->second.state == 1 && it->second.end > va) out.push_back(it->first);
  };
  std::vector<uint32_t> hit;
  {
    std::shared_lock<std::shared_mutex> lk(g_grantMu);   // the common case: nothing to drop
    candidates(hit);
  }
  if (hit.empty()) return 0;
  hit.clear();
  std::unique_lock<std::shared_mutex> lk(g_grantMu);
  candidates(hit);
  for (uint32_t s : hit) {
    auto it = g_physGrants.find(s);
#if SR_ORACLE
    const uint64_t txcP0 = txc::enabled() ? sr::gfx::hle::txcFifoPublished() : 0;
    const uint32_t serial = it->second.serial;
#endif
    const uint64_t mutEnd = it->second.end;
    g_physGrants.erase(it);   // released: its pages were already off the page counts
    grantMutated(s, mutEnd);
#if SR_ORACLE
    if (txc::enabled()) txc::onGrantRel(2, s, serial, 2, txcP0);   // its pages are re-granted
#endif
  }
  return (uint32_t)hit.size();
}

uint64_t physDoubleBookCount() { return g_dbCount.load(std::memory_order_relaxed); }
uint64_t physDoubleBookCountPhysPhys() { return g_dbPhysPhys.load(std::memory_order_relaxed); }
PhysDoubleBook physDoubleBookLast() {
  std::shared_lock<std::shared_mutex> lk(g_grantMu);
  return g_dbLast;
}
void setPhysDoubleBookHook(void (*hook)(uint32_t newVa, uint32_t oldVa)) {
  g_dbHook.store(hook, std::memory_order_release);
}

bool markPhysGrantReleased(uint32_t va, bool report) {
  bool found = false;
  {
    std::unique_lock<std::shared_mutex> lk(g_grantMu);
    auto it = g_physGrants.find(va);
#if SR_ORACLE
    const uint64_t txcP0 = txc::enabled() ? sr::gfx::hle::txcFifoPublished() : 0;
#endif
    if (it != g_physGrants.end()) {
      if (it->second.state == 0) pageAdjustLocked(2, va, it->second.end, -1);
      it->second.state = 1;
      grantMutated(va, it->second.end);
      found = true;
#if SR_ORACLE
      if (txc::enabled()) txc::onGrantRel(2, va, it->second.serial, 1, txcP0);   // released-retained
#endif
    } else {
      auto ht = g_grants.find(va);   // a heap-routed grant: the heap bump keeps it
      if (ht != g_grants.end()) {
        if (ht->second.state == 0) pageAdjustLocked(1, va, ht->second.end, -1);
        ht->second.state = 1;
        grantMutated(va, ht->second.end);
        found = true;
#if SR_ORACLE
        if (txc::enabled()) txc::onGrantRel(1, va, ht->second.serial, 1, txcP0);
#endif
      }
    }
  }
#if SR_ORACLE
  if (report) uniqMaybeReport(found ? "released-retained" : "release-unregistered", true);
#endif
  return found;
}

// Requires g_grantMu held (shared or unique).
static SpanOwners ownersOfSpanLocked(uint32_t p, uint32_t span) {
  SpanOwners o;
  if (!span) return o;
  const uint64_t lo = p & 0x1FFFFFFFu;
  const uint64_t hi = std::min<uint64_t>(lo + span, 0x20000000ull);
  // Live owners' physical intervals clipped to the span, for the pairwise
  // aliasing check (distinct serials only: one grant seen in two aliases is
  // not aliasing). Unbounded: the sweep below is exact for any count.
  struct Iv { uint64_t lo, hi; uint32_t serial; };
  std::vector<Iv> iv;
  o.gen = g_grantGen.load(std::memory_order_acquire);
  for (int k = 1; k <= 3; k++) {
    const std::map<uint32_t, GrantRec>& m = registryOf(k);
    for (uint64_t w = 0; w < 8; w++) {
      const uint64_t vlo = (w << 29) + lo, vhi = (w << 29) + hi;
      auto it = m.begin();
#if SR_ORACLE
      if (!g_regOverlapped[k]) {
#else
      // The native invariant already tracks every insertion, independently of censuses.
      if (!g_dbOverlapped[k]) {
#endif
        it = m.upper_bound((uint32_t)vlo);
        if (it != m.begin()) {
          auto pv = std::prev(it);
          if (pv->second.end > vlo) it = pv;
        }
      }
      for (; it != m.end() && it->first < vhi; ++it) {
        if (it->second.end <= vlo) continue;
        if (it->second.state) { o.nReleased++; continue; }
        o.nLive++;
        o.kindBits |= 1u << k;
        const uint32_t s = it->second.serial;
        if (!o.serial[0] || s < o.serial[0]) { o.serial[1] = o.serial[0]; o.serial[0] = s; }
        else if (!o.serial[1] || s < o.serial[1]) o.serial[1] = s;
        uint64_t z = s * 0x9E3779B97F4A7C15ull;   // order-independent: sum of mixes
        z ^= z >> 31;
        o.setDigest += z;
        iv.push_back(Iv{std::max<uint64_t>(it->first, vlo) - (w << 29),
                        std::min<uint64_t>(it->second.end, vhi) - (w << 29), s});
      }
    }
  }
  // Sort by start; keep the largest end seen (with its serial) and the largest
  // end among OTHER serials. An interval aliases iff it starts before the end
  // of some earlier interval with a different serial.
  std::sort(iv.begin(), iv.end(), [](const Iv& a, const Iv& b) {
    return a.lo != b.lo ? a.lo < b.lo : a.serial < b.serial;
  });
  uint64_t bestHi = 0, otherHi = 0;
  uint32_t bestSerial = 0;
  bool any = false;
  for (const Iv& v : iv) {
    const uint64_t reach = !any ? 0 : (v.serial != bestSerial ? bestHi : otherHi);
    if (any && v.lo < reach) o.aliasedPairs++;
    if (!any || v.hi > bestHi) {
      if (any && v.serial != bestSerial) otherHi = bestHi;
      bestHi = v.hi;
      bestSerial = v.serial;
      any = true;
    } else if (v.serial != bestSerial && v.hi > otherHi) {
      otherHi = v.hi;
    }
  }
  return o;
}

uint64_t registryGeneration() { return g_grantGen.load(std::memory_order_acquire); }

SpanOwners ownersOfSpan(uint32_t p, uint32_t span) {
  std::shared_lock<std::shared_mutex> lk(g_grantMu);
  return ownersOfSpanLocked(p, span);
}

void withOwnersOfSpan(uint32_t p, uint32_t span, void (*fn)(const SpanOwners&, void*),
                      void* ctx) {
  std::shared_lock<std::shared_mutex> lk(g_grantMu);
  fn(ownersOfSpanLocked(p, span), ctx);
}

void withRegistryShared(void (*fn)(void*), void* ctx) {
  std::shared_lock<std::shared_mutex> lk(g_grantMu);
  fn(ctx);
}

Owner ownerOfGrant(uint32_t va, uint32_t size) {
  if (inPhysGrant(va, size)) return kOwnPhysRegion;
  return inGrant(va, size) ? kOwnHeapGrant : kOwnNone;
}

#if SR_ORACLE
void setDrawContext(const DrawCtx& c, bool newDraw) {
  t_ctx = c;
  if (newDraw) t_drawId = g_drawSeq.fetch_add(1, std::memory_order_relaxed) + 1;
}

GrantCounts grantCountsForTest(Site site) {
  const GrantSiteCounts& g = g_gsites[site];
  return GrantCounts{g.cat[0].load(), g.cat[1].load(), g.cat[2].load(), g.cat[3].load()};
}

SampleStats sampleStatsForTest() {
  SampleStats s{};
  for (int i = 0; i < kSites; i++)
    for (int k = 0; k < kGCats; k++) {
      s.seen[i][k] = g_gsites[i].seen[k].load();
      s.sampled[i][k] = g_gsites[i].sampled[k].load();
    }
  return s;
}

void setSampleFileForTest(const char* path) {
  std::lock_guard<std::mutex> lk(g_sampleMu);
  if (g_sampleFile) { std::fclose(g_sampleFile); g_sampleFile = nullptr; }
  g_sampleOpenTried = false;
  g_sampleWritten = 0;
  g_sampleTestSet = path != nullptr;
  g_sampleTestPath = path ? path : "";
}

uint64_t fnv1a64ForTest(const uint8_t* p, size_t n) { return fnv1a(p, n); }

void setWinCaptureForTest(const char* path, bool (*traceOpen)()) {
  std::lock_guard<std::mutex> lk(g_winMu);
  g_winTestSet = path != nullptr;
  g_winTestPath = path ? path : "";
  g_winOpenProbe = traceOpen;
  g_winRing.clear();
  g_winRing.shrink_to_fit();
  g_winHead = 0;
  g_winSeenOpen = false;
  g_winFlushedCount = 0;
  g_winSeq = 0;
  g_winFlushed.store(false, std::memory_order_release);
}
uint64_t winFlushedForTest() {
  std::lock_guard<std::mutex> lk(g_winMu);
  return g_winFlushedCount;
}

Owner ownerOf(uint32_t va, uint32_t size) {
  // iter-458 (Astra 022): with the reusable allocator, ownership is a LIVE
  // allocation extent - freed memory is owned by nobody.
  if (physalloc::physFreeEnabled()) {
    if (physalloc::insideLive(va, size)) return kOwnPhysRegion;
    return inGrant(va, size) ? kOwnHeapGrant : kOwnNone;
  }
  uint32_t lo = 0, hi = 0;
  guestPhysRegion(lo, hi);
  if (lo < hi && va >= lo && (uint64_t)va + size <= hi) return kOwnPhysRegion;
  return inGrant(va, size) ? kOwnHeapGrant : kOwnNone;
}

#endif

static int gpuWindowUncached(uint32_t phys, uint64_t bytes) {
  const uint64_t p = phys & 0x1FFFFFFFu;
  if (bytes == 0 || bytes > 0x20000000ull || p + bytes > 0x20000000ull) return 0;
  const bool a = inPhysGrantG((uint32_t)(0xA0000000u | p), (uint32_t)bytes);
  const bool h = inGrantG((uint32_t)(0x80000000u | p), (uint32_t)bytes) ||
                 inPhysGrantG((uint32_t)(0x80000000u | p), (uint32_t)bytes);
  if (a == h) return 0;
  return a ? 0xA : 8;
}

// iter-461 (the FMV regression): a window answer for a whole RANGE, safe to
// apply to every page of it. gpuWindow's per-record containment is wrong for
// that: the heap registers allocations per chunk, so the movie planes (one
// 900 KB texture across several heap chunks) failed the 0x8 containment while
// the physical pool's single grant passed the 0xA one, and the range came
// back 0xA for pages whose own answer is 0 (contested - the legacy rule takes
// the 0x8 copy). The 0xA copy is another allocation's bytes: noise on screen.
// Here a window owns the range only if it contains it (some record) AND the
// other window has no grant overlapping any part of it; anything else is 0 =
// every page decides for itself.
static int gpuWindowRangeUncached(uint32_t phys, uint64_t bytes) {
  const uint64_t p = phys & 0x1FFFFFFFu;
  if (bytes == 0 || bytes > 0x20000000ull || p + bytes > 0x20000000ull) return 0;
  const bool live = gpuLiveOnly();
  const uint32_t vaA = (uint32_t)(0xA0000000u | p), va8 = (uint32_t)(0x80000000u | p);
  const uint32_t n = (uint32_t)bytes;
  const bool aIn = inPhysGrantG(vaA, n);
  const bool hIn = inGrantG(va8, n) || inPhysGrantG(va8, n);
  if (aIn == hIn) return 0;
  if (aIn) {
    const bool hAny = overlapsMap(g_grants, va8, n, live) || overlapsMap(g_physGrants, va8, n, live);
    return hAny ? 0 : 0xA;
  }
  const bool aAny = overlapsMap(g_physGrants, vaA, n, live);
  return aAny ? 0 : 8;
}

int gpuGrantFlags(uint32_t phys, uint32_t bytes) {
  const uint32_t p = phys & 0x1FFFFFFFu;
  return (inGrantG(0x80000000u | p, bytes) ? 1 : 0) | (inPhysGrantG(0x80000000u | p, bytes) ? 2 : 0) |
         (inPhysGrantG(0xA0000000u | p, bytes) ? 4 : 0);
}

// ── iter-463 F3: one window rule for indirect buffers ──────────────────────
#if SR_ORACLE
namespace {
std::atomic<int> g_ibUnifiedForce{-1};
std::atomic<uint64_t> g_ibContested{0}, g_ibUnowned{0};
// The same env gates gfx/null_renderer.cpp's Pm4Engine::physToHost reads.
bool pm4EngineOwnerGate() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_ENGINE_OWNER");
    return !e || e[0] != '0';
  }();
  return v;
}
bool pm4PhysAGate() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_PHYS_A");
    return !e || e[0] != '0';
  }();
  return v;
}
}  // namespace

bool ibWindowUnifiedEnabled() {
  const int f = g_ibUnifiedForce.load(std::memory_order_relaxed);
  if (f >= 0) return f == 1;
  static const bool v = [] {
    const char* e = std::getenv("SR_IB_WINDOW_UNIFIED");
    return !e || e[0] != '0';
  }();
  return v;
}
void setIbWindowUnifiedForTest(int mode) { g_ibUnifiedForce.store(mode, std::memory_order_relaxed); }

uint32_t resolveIbWindow(uint32_t ibPhys, uint32_t bytes, int* verdict) {
  int v = kIbNotConsulted;
  if (physalloc::physFreeEnabled()) {
    // iter-458 (Astra 029 A): the reusable allocator's live blocks.
    const int w = ownerWindow(ibPhys, bytes);
    v = w == kWinA ? 0xA : w == kWin8 ? 8 : w == kWinBoth ? (int)kWinBoth : 0;
  } else if (pm4EngineOwnerGate() && gpuOwnerWindowEnabled()) {
    // iter-460 (SR_PM4_ENGINE_OWNER): the allocators' live records.
    const int w = gpuWindow(ibPhys, bytes);
    if (w == 0xA || w == 8) {
      v = w;
    } else {
      const int f = gpuGrantFlags(ibPhys, bytes);
      v = ((f & 4) && (f & 3)) ? (int)kWinBoth : 0;
    }
  }
  if (verdict) *verdict = v;
  if (v == 0xA) return 0xA0000000u;
  if (v == 8) return 0x80000000u;
  // SR_PM4_ENGINE_OWNER=0 / SR_PM4_OWNER_WINDOW=0: the pre-iter-460 arm - the
  // 0x8 IB fetch it exists to reproduce (review: not "unowned").
  if (v == kIbNotConsulted) return 0x80000000u;
  // Contested or unowned. Legacy: a separate 0x8 default for the IB fetch while
  // every read the IB then makes (fences, constants) took physToHost's 0xA - so
  // on a double-booked page the CP parsed one object and wrote another
  // (v1_gpuhang: the arena's file bytes parsed as the D3D command buffer).
  if (!ibWindowUnifiedEnabled()) return 0x80000000u;
  return pm4PhysAGate() ? 0xA0000000u : 0x80000000u;
}

void noteIbWindow(int verdict, uint32_t ibPhys, uint32_t bytes, uint32_t base) {
  if (verdict == 0xA || verdict == 8 || verdict == kIbNotConsulted) return;
  const bool contested = verdict == (int)kWinBoth;
  const uint64_t k = (contested ? g_ibContested : g_ibUnowned).fetch_add(1, std::memory_order_relaxed) + 1;
  if (k > 8 && (k & (k - 1)) != 0) return;
  if (contested)
    SR_LOG_BOOT(sr::LogSeverity::Error,
                "[PM4_IB_CONTESTED] #%llu ib phys=%08X bytes=0x%X: BOTH windows hold a live grant "
                "(flags=%d) - parsed from %08X (unified=%d) | unowned=%llu",
                (unsigned long long)k, ibPhys, bytes, gpuGrantFlags(ibPhys, bytes), base | (ibPhys & 0x1FFFFFFFu),
                ibWindowUnifiedEnabled() ? 1 : 0, (unsigned long long)g_ibUnowned.load());
  else
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PM4_IB_UNOWNED] #%llu ib phys=%08X bytes=0x%X: no live grant holds it - parsed "
                "from %08X (unified=%d) | contested=%llu",
                (unsigned long long)k, ibPhys, bytes, base | (ibPhys & 0x1FFFFFFFu),
                ibWindowUnifiedEnabled() ? 1 : 0, (unsigned long long)g_ibContested.load());
}
uint64_t ibContestedCount() { return g_ibContested.load(std::memory_order_relaxed); }
uint64_t ibUnownedCount() { return g_ibUnowned.load(std::memory_order_relaxed); }

#endif

bool physPageContested(uint32_t phys) {
  const uint32_t pg = (phys & 0x1FFFFFFFu) >> kPageShift;
  return g_pgA[pg].load(std::memory_order_relaxed) != 0 &&
         g_pg8[pg].load(std::memory_order_relaxed) != 0;
}

#if SR_ORACLE
namespace {
constexpr uint32_t kCbHistory = 256;  // ~5 frames of title-city command buffers
struct CbRec {
  std::atomic<uint32_t> lo{0}, hi{0};
  std::atomic<int> win{0};
};
CbRec g_cb[kCbHistory];
std::atomic<uint32_t> g_cbNext{0};
std::atomic<uint32_t> g_cbDone{0};   // iter-465: notes whose record is completely written
}  // namespace

bool commandBufferWindowEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_CB_WINDOW");
    return !e || e[0] != '0';
  }();
  return v;
}

void noteCommandBuffer(uint32_t phys, uint32_t bytes, int window) {
  if (!bytes || (window != 8 && window != 0xA)) return;
  const uint32_t p = phys & 0x1FFFFFFFu;
  CbRec& r = g_cb[g_cbNext.fetch_add(1, std::memory_order_acq_rel) % kCbHistory];
  const uint32_t oldLo = r.lo.load(std::memory_order_relaxed), oldHi = r.hi.load(std::memory_order_relaxed);
  r.hi.store(0, std::memory_order_release);  // empty while it changes
  r.lo.store(p, std::memory_order_relaxed);
  r.win.store(window, std::memory_order_relaxed);
  r.hi.store(p + bytes, std::memory_order_release);
  // iter-466 cp round 2 (SR_CP_OWN_MEMO): the evicted record's pages and the new one's change
  // their answer; bumped before the note completes (g_cbDone), so a reader that sees the note
  // done sees the bumps.
  if (oldHi > oldLo) pgMutBump(oldLo, oldHi - oldLo);
  pgMutBump(p, bytes);
  g_cbDone.fetch_add(1, std::memory_order_acq_rel);
}

static int commandBufferWindowUncached(uint32_t phys, uint32_t bytes) {
  const uint32_t p = phys & 0x1FFFFFFFu;
  const uint32_t n = g_cbNext.load(std::memory_order_acquire);
  const uint32_t count = n < kCbHistory ? n : kCbHistory;
  for (uint32_t k = 1; k <= count; k++) {
    const CbRec& r = g_cb[(n - k) % kCbHistory];
    const uint32_t hi = r.hi.load(std::memory_order_acquire);
    const uint32_t lo = r.lo.load(std::memory_order_relaxed);
    if (p >= lo && uint64_t(p) + bytes <= hi) return r.win.load(std::memory_order_relaxed);
  }
  return 0;
}

#endif

// ── iter-465 perf lane: SR_PHYSWIN_MEMO (default ON; =0 keeps the uncached A/B) ──────────
// The PM4 renderer asks gpuWindow / gpuWindowRange / commandBufferWindow for every GPU read
// it resolves (physToHost for fences and constant loads, vertex / index / texture / shared-
// memory pages): three std::map lookups under the registry's shared lock, or a scan of the
// 256-record command-buffer history, per call - ~15% of the command processor's busy time in
// gameplay (host profile archive/v1_perf/g4_ffx_portdev). The answers only change when a
// registry mutates (g_grantGen, bumped under the unique lock) or a command buffer is noted
// (g_cbNext / g_cbDone), so a per-thread memo keyed by the EXACT (kind, phys, bytes) and
// tagged with that version is exact: a version read before the lookup and re-checked after it
// caches only an answer no mutation could have raced. SR_PHYSWIN_MEMO_CHECK=1 (diagnostic)
// also computes the uncached answer and counts any difference ([PHYSWIN_MEMO] lines).
namespace {
bool memoOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PHYSWIN_MEMO");
    return !e || e[0] != '0';
  }();
  return v;
}
#if SR_ORACLE
bool memoCheck() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PHYSWIN_MEMO_CHECK");
    return e && e[0] == '1';
  }();
  return v;
}
#endif

struct MemoEnt {
  uint64_t tok;     // version + 1 (0 = empty)
  uint32_t phys;
  uint32_t bytes;   // a 64-bit request above 4 GB is never memoised
  int32_t res;
  uint32_t kind;
};
constexpr uint32_t kMemoBits = 11, kMemo = 1u << kMemoBits;
thread_local MemoEnt t_memo[kMemo];
#if SR_ORACLE
std::atomic<uint64_t> g_memoHit{0}, g_memoMiss{0}, g_memoMismatch{0};
thread_local uint32_t t_memoHit = 0, t_memoMiss = 0;

#endif

inline MemoEnt& memoSlot(uint32_t kind, uint32_t phys, uint32_t bytes) {
  const uint32_t h = ((phys * 0x9E3779B1u) ^ (bytes * 0x85EBCA6Bu) ^ (kind * 0xC2B2AE35u)) >> (32 - kMemoBits);
  return t_memo[h];
}
#if SR_ORACLE
void memoReport(uint64_t n);
void memoCount(bool hit) {
  // Per-thread batches: the shared counters move once per 4096 lookups (no shared RMW per call).
  uint32_t& c = hit ? t_memoHit : t_memoMiss;
  if (++c < 4096) return;
  c = 0;
  (hit ? g_memoHit : g_memoMiss).fetch_add(4096, std::memory_order_relaxed);
  static std::atomic<uint64_t> s_batches{0};
  const uint64_t n = s_batches.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((n & (n - 1)) == 0 && n >= 64) memoReport(n * 4096);
}
void memoReport(uint64_t n) {
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PHYSWIN_MEMO] after %llu lookups: hit=%llu miss=%llu (per-thread batches of 4096)%s "
              "mismatch=%llu",
              (unsigned long long)n, (unsigned long long)g_memoHit.load(),
              (unsigned long long)g_memoMiss.load(), memoCheck() ? " CHECK on" : "",
              (unsigned long long)g_memoMismatch.load());
}
#endif

template <class F>
int memoized(uint32_t kind, uint32_t phys, uint64_t bytes64, uint64_t ver, F uncached,
             bool cacheable, uint64_t (*reread)()) {
  if (!memoOn() || bytes64 > 0xFFFFFFFFull || !cacheable) return uncached();
  const uint32_t bytes = uint32_t(bytes64);
  MemoEnt& e = memoSlot(kind, phys, bytes);
  if (e.tok == ver + 1 && e.phys == phys && e.bytes == bytes && e.kind == kind) {
#if SR_ORACLE
    memoCount(true);
    if (memoCheck()) {
      const int u = uncached();
      if (u != e.res && reread() == ver) {
        const uint64_t k = g_memoMismatch.fetch_add(1, std::memory_order_relaxed) + 1;
        if (k <= 16)
          SR_LOG_BOOT(sr::LogSeverity::Error,
                      "[PHYSWIN_MEMO] MISMATCH #%llu kind=%u phys=%08X bytes=%u memo=%d uncached=%d ver=%llu",
                      (unsigned long long)k, kind, phys, bytes, e.res, u, (unsigned long long)ver);
      }
    }
#endif

    return e.res;
  }
#if SR_ORACLE
  memoCount(false);
#endif
  const int r = uncached();
  if (reread() == ver) {   // no mutation raced the lookup: the answer belongs to `ver`
    e.tok = ver + 1;
    e.phys = phys;
    e.bytes = bytes;
    e.res = r;
    e.kind = kind;
  }
  return r;
}
uint64_t grantVer() { return g_grantGen.load(std::memory_order_acquire); }
#if SR_ORACLE
uint64_t cbVer() {
  // Cacheable only while no note is half-written: next == done.
  const uint32_t n = g_cbNext.load(std::memory_order_acquire);
  const uint32_t d = g_cbDone.load(std::memory_order_acquire);
  return n == d ? uint64_t(d) : ~0ull;
}
#endif

}  // namespace

namespace {
// A memoised window answer computed at generation gen stays exact at G when no mutation of
// generations (gen, G] touched [p, p + bytes) (g_grantLog). SR_PHYSWIN_GRANT_MEMO=0 = the
// previous whole-generation memo; SR_PHYSWIN_MEMO_CHECK=1 compares with the uncached answer.
struct GrantMemo {
  uint64_t gen = 0;
  uint32_t phys = 0, bytes = 0, kind = 0;
  int32_t res = 0;
  bool valid = false;
};
// iter-466 cp round 2 (SR_CP_PHYSWIN_MEMO_BIG): the direct-mapped memos held 2048 answers per
// thread, fewer than one gameplay frame's distinct (range, kind) requests on the CP - they
// thrashed into the std::map walks (_Tree ~4.5% of the CP). With the gate: 16384 per thread,
// heap-allocated on first use (the CP / render thread; other threads never pay for them).
uint32_t memoBitsPw() {
  static const uint32_t b = [] {
    const char* e = std::getenv("SR_CP_PHYSWIN_MEMO_BIG");
    return (e && e[0] == '1') ? 14u : 11u;
  }();
  return b;
}
GrantMemo* grantMemoTable() {
  thread_local std::unique_ptr<GrantMemo[]> t;
  if (!t) t.reset(new GrantMemo[size_t(1) << memoBitsPw()]);
  return t.get();
}
bool grantMemoOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PHYSWIN_GRANT_MEMO");
    return !e || e[0] != '0';
  }();
  return v && memoOn();
}
// No mutation of generations (from, to] overlaps [p, p + bytes)?
bool grantUntouched(uint64_t from, uint64_t to, uint32_t p, uint32_t bytes) {
  if (to - from > kGrantLog / 2) return false;
  const uint64_t end = uint64_t(p) + bytes;
  for (uint64_t g = from; g < to; g++) {
    const GrantMut& m = g_grantLog[g % kGrantLog];
    if (m.lo < end && p < m.hi) return false;
  }
  return g_grantGen.load(std::memory_order_acquire) - from <= kGrantLog / 2;  // not lapped meanwhile
}
template <class F>
int grantMemoized(uint32_t kind, uint32_t phys, uint64_t bytes64, F uncached) {
  if (!grantMemoOn() || bytes64 == 0 || bytes64 > 0x20000000ull) {
    const uint64_t v = grantVer();
    return memoized(kind, phys, bytes64, v, uncached, true, &grantVer);
  }
  const uint32_t p = phys & 0x1FFFFFFFu, bytes = uint32_t(bytes64);
  const uint32_t h = ((p * 0x9E3779B1u) ^ (bytes * 0x85EBCA6Bu) ^ (kind * 0xC2B2AE35u)) >> (32 - memoBitsPw());
  GrantMemo& e = grantMemoTable()[h];
  const uint64_t G = g_grantGen.load(std::memory_order_acquire);
  int res;
  if (e.valid && e.phys == p && e.bytes == bytes && e.kind == kind && e.gen <= G &&
      (e.gen == G || grantUntouched(e.gen, G, p, bytes))) {
    res = e.res;
    e.gen = G;
#if SR_ORACLE
    memoCount(true);
#endif
  } else {
    res = uncached();
#if SR_ORACLE
    memoCount(false);
#endif
    if (g_grantGen.load(std::memory_order_acquire) == G) {  // no mutation raced the lookup
      e.gen = G;
      e.phys = p;
      e.bytes = bytes;
      e.kind = kind;
      e.res = res;
      e.valid = true;
    }
  }
#if SR_ORACLE
  if (memoCheck()) {
    const int u = uncached();
    if (u != res && g_grantGen.load(std::memory_order_acquire) == G) {
      const uint64_t k = g_memoMismatch.fetch_add(1, std::memory_order_relaxed) + 1;
      if (k <= 16)
        SR_LOG_BOOT(sr::LogSeverity::Error, "[PHYSWIN_MEMO] GRANT MISMATCH #%llu kind=%u phys=%08X bytes=%u memo=%d uncached=%d",
                    (unsigned long long)k, kind, phys, bytes, res, u);
    }
  }
#endif
  return res;
}
}  // namespace

int gpuWindow(uint32_t phys, uint64_t bytes) {
  return grantMemoized(1u, phys, bytes, [&] { return gpuWindowUncached(phys, bytes); });
}
int gpuWindowRange(uint32_t phys, uint64_t bytes) {
  return grantMemoized(2u, phys, bytes, [&] { return gpuWindowRangeUncached(phys, bytes); });
}
OwnerVer ownerVersion(uint32_t phys, uint64_t bytes) {
  OwnerVer v{};
  if (!bytes) return v;
#if SR_ORACLE
  const uint32_t n = g_cbNext.load(std::memory_order_acquire);
  if (g_cbDone.load(std::memory_order_acquire) != n) return v;  // a note half-written
#else
  constexpr uint32_t n = 0;  // native records never publish packet-buffer ownership
#endif
  const uint32_t p = phys & 0x1FFFFFFFu;
  const uint64_t p0 = p >> kMutShift, p1 = (uint64_t(p) + bytes - 1u) >> kMutShift;
  if (p1 - p0 >= 64u || p1 >= kMutPages) return v;
  uint64_t s = 0;
  for (uint64_t pg = p0; pg <= p1; pg++) s += g_pgMut[pg].load(std::memory_order_acquire);
  v.sum = s;
  v.cbSeq = n;
  v.ok = true;
  return v;
}
#if SR_ORACLE
// iter-465 native lane (CP costs): every noted IB moved cbVer, so the generic memo above missed
// on nearly every call and each miss scanned the 256-record history. This memo is exact and
// incremental: an entry remembers the answer, the history length when it was computed and which
// record answered; a later call scans only the records noted since (the newest containing record
// wins, as in the full scan) and keeps the old answer while its record is still in the history.
// SR_PHYSWIN_CB_MEMO=0 keeps the previous path; SR_PHYSWIN_MEMO_CHECK=1 compares with the scan.
namespace {
struct CbMemo {
  uint32_t phys = 0, bytes = 0, seqAt = 0, src = 0;  // src = answering record index + 1 (0 = none)
  int32_t res = 0;
  bool valid = false;
};
CbMemo* cbMemoTable() {
  thread_local std::unique_ptr<CbMemo[]> t;
  if (!t) t.reset(new CbMemo[size_t(1) << memoBitsPw()]);
  return t.get();
}
bool cbMemoOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PHYSWIN_CB_MEMO");
    return !e || e[0] != '0';
  }();
  return v && memoOn();
}
// Newest record in [from, n) containing [p, p+bytes): its index + 1, or 0.
uint32_t cbScan(uint32_t p, uint32_t bytes, uint32_t from, uint32_t n) {
  for (uint32_t i = n; i > from; i--) {
    const CbRec& r = g_cb[(i - 1) % kCbHistory];
    const uint32_t hi = r.hi.load(std::memory_order_acquire);
    const uint32_t lo = r.lo.load(std::memory_order_relaxed);
    if (p >= lo && uint64_t(p) + bytes <= hi) return i;
  }
  return 0;
}
}  // namespace

int commandBufferWindow(uint32_t phys, uint32_t bytes) {
  if (!cbMemoOn()) {
    const uint64_t v = cbVer();
    return memoized(3u, phys, bytes, v, [&] { return commandBufferWindowUncached(phys, bytes); },
                    v != ~0ull, &cbVer);
  }
  const uint32_t n = g_cbNext.load(std::memory_order_acquire);
  if (g_cbDone.load(std::memory_order_acquire) != n) return commandBufferWindowUncached(phys, bytes);
  const uint32_t p = phys & 0x1FFFFFFFu;
  const uint32_t h = ((p * 0x9E3779B1u) ^ (bytes * 0x85EBCA6Bu)) >> (32 - memoBitsPw());
  CbMemo& e = cbMemoTable()[h];
  const uint32_t oldest = n > kCbHistory ? n - kCbHistory : 0u;  // records [oldest, n) are live
  int res;
  uint32_t src;
  if (e.valid && e.phys == p && e.bytes == bytes && e.seqAt <= n && n - e.seqAt <= kCbHistory &&
      (e.src == 0 || e.src - 1 >= oldest)) {
    src = cbScan(p, bytes, e.seqAt, n);  // records noted since: a newer containing one wins
    if (src) {
      res = g_cb[(src - 1) % kCbHistory].win.load(std::memory_order_relaxed);
    } else {
      src = e.src;
      res = e.res;
    }
    memoCount(true);
  } else {
    src = cbScan(p, bytes, oldest, n);
    res = src ? g_cb[(src - 1) % kCbHistory].win.load(std::memory_order_relaxed) : 0;
    memoCount(false);
  }
  if (g_cbNext.load(std::memory_order_acquire) == n && g_cbDone.load(std::memory_order_acquire) == n) {
    e.phys = p;
    e.bytes = bytes;
    e.seqAt = n;
    e.src = src;
    e.res = res;
    e.valid = true;
  }
  if (memoCheck()) {
    const int u = commandBufferWindowUncached(phys, bytes);
    if (u != res && g_cbNext.load(std::memory_order_acquire) == n) {
      const uint64_t k = g_memoMismatch.fetch_add(1, std::memory_order_relaxed) + 1;
      if (k <= 16)
        SR_LOG_BOOT(sr::LogSeverity::Error, "[PHYSWIN_MEMO] CB MISMATCH #%llu phys=%08X bytes=%u memo=%d uncached=%d",
                    (unsigned long long)k, phys, bytes, res, u);
    }
  }
  return res;
}

#endif

int physPageOwner(uint32_t phys) {
  const uint32_t pg = (phys & 0x1FFFFFFFu) >> kPageShift;
  const bool a = g_pgA[pg].load(std::memory_order_relaxed) != 0;
  const bool h = g_pg8[pg].load(std::memory_order_relaxed) != 0;
  return a == h ? 0 : a ? 0xA : 8;
}

bool gpuOwnerWindowEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_PM4_OWNER_WINDOW");
    return !e || e[0] != '0';
  }();
  return v;
}

#if SR_ORACLE
int ownerWindow(uint32_t phys, uint64_t bytes) {
  const uint64_t p = phys & 0x1FFFFFFFu;
  if (bytes == 0 || bytes > 0x20000000ull || p + bytes > 0x20000000ull) return kWinUnknown;
  // Live model under SR_PHYS_FREE; the registered-grant model in grant mode
  // (Astra 044); otherwise 0xA is never named (the pre-044 contract).
  const bool a = physalloc::physFreeEnabled()
                     ? physalloc::insideLive((uint32_t)(0xA0000000u | p), (uint32_t)bytes)
                     : physResolveGrantMode() &&
                           inPhysGrant((uint32_t)(0xA0000000u | p), (uint32_t)bytes);
  // Astra 045: in grant mode the 0x8 side is owned by a heap-routed grant OR a
  // GPU physical grant - the bump region spills below 0xA0000000, so the physical
  // allocator double-books masked addresses itself; those stay BOTH -> legacy.
  // The live model (SR_PHYS_FREE) keeps its pre-045 rule.
  const bool h = inGrant((uint32_t)(0x80000000u | p), (uint32_t)bytes) ||
                 (!physalloc::physFreeEnabled() && physResolveGrantMode() &&
                  inPhysGrant((uint32_t)(0x80000000u | p), (uint32_t)bytes));
  if (a && h) return kWinBoth;
  if (a) return kWinA;
  if (h) return kWin8;
  return kWinUnknown;
}

bool physResolveGrantMode() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_PHYS_RESOLVE_GRANTS");
    const bool asked = e && e[0] == '1';   // DEFAULT OFF
    const bool on = asked && !physalloc::physFreeEnabled();
    if (asked)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[PHYS_RESOLVE] grant mode %s (SR_PHYS_RESOLVE_GRANTS=1%s): 0xA owner = a registered "
          "GPU physical grant; BOTH/unknown keep the legacy rule",
          on ? "ACTIVE" : "INERT", on ? "" : " but SR_PHYS_FREE=1 - the live model takes precedence");
    return on;
  }();
  return s_on;
}

bool physResolveEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_PHYS_RESOLVE");
    const bool asked = e && e[0] == '1';   // DEFAULT OFF
    if (asked && !physalloc::physFreeEnabled())
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[PHYS_RESOLVE] SR_PHYS_RESOLVE=1 is INERT without SR_PHYS_FREE=1");
    return (asked && physalloc::physFreeEnabled()) || physResolveGrantMode();
  }();
  return s_on;
}

int resolveOwned(ResolveSite site, uint32_t phys, uint64_t bytes) {
  if (site < 0 || site >= kRsSites || !physResolveEnabled()) return 0;
  RsCounts& c = g_rs[site];
  // Astra 045 SCOPE: grant mode resolves only where the oracle validated it
  // (idx-ib, idx-vb, s4-vb). The texture and refresh sites keep the legacy rule:
  // hwin2's tex-site selections were implausible 197/327 and the FMV/emblem broke.
  // Counted, never silent. The live model (SR_PHYS_FREE) is unscoped as before.
  if (!physalloc::physFreeEnabled() && (site == kRsTex || site == kRsRefresh)) {
    c.scopeSkip.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const int w = ownerWindow(phys, bytes);
  std::atomic<uint64_t>& ctr = w == kWinA ? c.a : w == kWin8 ? c.w8
                             : w == kWinBoth ? c.both : c.unknown;
  ctr.fetch_add(1, std::memory_order_relaxed);
  const uint64_t n = c.a.load() + c.w8.load() + c.both.load() + c.unknown.load();
  if (sampleDue(n)) reportResolve(site);
  return (w == kWinA || w == kWin8) ? w : 0;
}

void resolveValidate(ResolveSite site, bool ok) {
  if (site < 0 || site >= kRsSites) return;
  RsCounts& c = g_rs[site];
  const uint64_t n = (ok ? c.validOk : c.validBad).fetch_add(1, std::memory_order_relaxed) + 1;
  if (!ok && sampleDue(n)) reportResolve(site);
}

void resolveCompare(ResolveSite site, int selectedWin, int legacyWin, bool selOk,
                    bool legacyOk) {
  if (site < 0 || site >= kRsSites) return;
  // Astra 044: hand this read's legacy choice to the census/sampler that follows
  // at the same site (same thread), so every sample carries selected AND legacy.
  t_cmpSite = site;
  t_cmpLegacyWin = legacyWin;
  RsCounts& c = g_rs[site];
  if (selectedWin == legacyWin) {
    c.agree.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint64_t n = c.disagree.fetch_add(1, std::memory_order_relaxed) + 1;
  (selOk ? c.dSelOk : c.dSelBad).fetch_add(1, std::memory_order_relaxed);
  (legacyOk ? c.dLegOk : c.dLegBad).fetch_add(1, std::memory_order_relaxed);
  if (sampleDue(n)) reportResolve(site);
}

ResolveCounts resolveCountsForTest(ResolveSite site) {
  const RsCounts& c = g_rs[site];
  return ResolveCounts{c.a.load(),       c.w8.load(),       c.both.load(),
                       c.unknown.load(), c.validOk.load(),  c.validBad.load(),
                       c.agree.load(),   c.disagree.load(), c.dSelOk.load(),
                       c.dSelBad.load(), c.dLegOk.load(),   c.dLegBad.load(),
                       c.scopeSkip.load()};
}

bool censusEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_PHYSWIN_CENSUS");
    return e && e[0] == '1';   // DEFAULT OFF
  }();
  return s_on;
}

void census(Site site, uint32_t phys, uint32_t size, bool chose8) {
  if (site < 0 || site >= kSites || !size) return;
  const uint32_t p = phys & 0x1FFFFFFFu;
  const uint32_t va8 = 0x80000000u | p, vaA = 0xA0000000u | p;
  const Owner o8 = ownerOf(va8, size), oA = ownerOf(vaA, size);
  const bool v8 = o8 != kOwnNone, vA = oA != kOwnNone;
  const bool chosenVis = chose8 ? v8 : vA;
  SiteCounts& c = g_sites[site];
  const uint64_t n = c.calls.fetch_add(1, std::memory_order_relaxed) + 1;
  // legacy categories for the sample record: 0 none, 1 ok, 2 wrongDiffer,
  // 3 wrongSame, 4 bothDiffer, 5 bothSame, 6 unreadable
  int catLegacy = 0;
  if (!v8 && !vA) {
    c.none.fetch_add(1, std::memory_order_relaxed);
  } else if (chosenVis && !(v8 && vA)) {
    c.ok.fetch_add(1, std::memory_order_relaxed);
    catLegacy = 1;
  } else {
    // wrong (chosen not owned, other owned) or both owned: do the bytes the
    // site read differ from the other candidate's?
    const int d = bytesDiffer(va8, vaA, size);
    const bool both = v8 && vA;
    if (d < 0) { c.unreadable.fetch_add(1, std::memory_order_relaxed); catLegacy = 6; }
    else if (both) { (d ? c.bothDiffer : c.bothSame).fetch_add(1, std::memory_order_relaxed); catLegacy = d ? 4 : 5; }
    else { (d ? c.wrongDiffer : c.wrongSame).fetch_add(1, std::memory_order_relaxed); catLegacy = d ? 2 : 3; }
    std::atomic<uint32_t>& shown = both ? c.bothShown : c.wrongShown;
    if (d > 0 && shown.fetch_add(1, std::memory_order_relaxed) < 8)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[PHYSWIN] %s %s phys=%08X size=%u chose=%s owner8=%s ownerA=%s "
          "bytesDiffer=1",
          both ? "BOTH" : "WRONG", kSiteNames[site], p, size,
          chose8 ? "0x8" : "0xA", kOwnNames[o8], kOwnNames[oA]);
  }
  // Astra 044: consume (always - never carry to a later read) the legacy choice
  // of a resolver decision made at this site for this read.
  const int legacyWin = (t_cmpSite == (int)site) ? t_cmpLegacyWin : 0;
  t_cmpSite = -1;
  // Astra 043: the precise (grant) model, counted side by side.
  const bool g8 = ownerOfGrant(va8, size) != kOwnNone, gA = ownerOfGrant(vaA, size) != kOwnNone;
  const bool gChosen = chose8 ? g8 : gA;
  const int catGrant = (!g8 && !gA) ? kGNone : (g8 && gA) ? kGBoth : gChosen ? kGOk : kGWrong;
  GrantSiteCounts& g = g_gsites[site];
  g.cat[catGrant].fetch_add(1, std::memory_order_relaxed);
  const uint64_t seen = g.seen[catGrant].fetch_add(1, std::memory_order_relaxed) + 1;
  // The env gate is read ONCE (H-REC: a getenv per call cost 17.5 us each).
  static const bool s_sampleEnv = [] {
    const char* e = std::getenv("SR_PHYSWIN_SAMPLE");
    return e && *e;
  }();
  if (sampleDueStratum(seen) && (g_sampleTestSet || s_sampleEnv)) {
    g.sampled[catGrant].fetch_add(1, std::memory_order_relaxed);
    writeSample(site, catLegacy, catGrant, chose8, p, size, seen, legacyWin);
  }
  // Astra 054: every read, no stratum limit, while SR_PHYSWIN_SAMPLE_WIN is set.
  winCapture(site, catLegacy, catGrant, chose8, p, size, legacyWin);
  if ((n & (n - 1)) == 0 || (n % 262144u) == 0) {
    report(kSiteNames[site]);
    reportGrant(kSiteNames[site]);
  }
}

Counts countsForTest(Site site) {
  const SiteCounts& c = g_sites[site];
  return Counts{c.calls.load(),     c.ok.load(),       c.wrongDiffer.load(),
                c.wrongSame.load(), c.bothDiffer.load(), c.bothSame.load(),
                c.none.load(),      c.unreadable.load()};
}

void resetForTest() {
  for (SiteCounts& c : g_sites) {
    c.calls = 0; c.ok = 0; c.wrongDiffer = 0; c.wrongSame = 0;
    c.bothDiffer = 0; c.bothSame = 0; c.none = 0; c.unreadable = 0;
    c.wrongShown = 0; c.bothShown = 0;
  }
  for (RsCounts& c : g_rs) {
    c.a = 0; c.w8 = 0; c.both = 0; c.unknown = 0; c.validOk = 0; c.validBad = 0;
    c.agree = 0; c.disagree = 0; c.dSelOk = 0; c.dSelBad = 0; c.dLegOk = 0; c.dLegBad = 0;
    c.scopeSkip = 0;
  }
  for (GrantSiteCounts& g : g_gsites)
    for (int k = 0; k < kGCats; k++) { g.cat[k] = 0; g.seen[k] = 0; g.sampled[k] = 0; }
  g_drawSeq = 0;
  std::unique_lock<std::shared_mutex> lk(g_grantMu);
  g_grants.clear();
  g_physGrants.clear();
  g_stackGrants.clear();
  for (uint32_t i = 0; i < kPages; i++) {
    g_pgA[i].store(0, std::memory_order_relaxed);
    g_pg8[i].store(0, std::memory_order_relaxed);
  }
  g_grantSerial = 0;
  g_uniqEvents = 0;
  g_uniqLifeEvents = 0;
  g_sticky = UniqSticky{};
  for (bool& b : g_regOverlapped) b = false;
  g_mainStackNoted = false;
  g_dbCount = 0;   // iter-463 F2
  g_dbPhysPhys = 0;
  g_dbEvents = 0;
  g_dbLast = PhysDoubleBook{};
  for (bool& b : g_dbOverlapped) b = false;
}

#endif

}  // namespace physwin
}  // namespace kernel
}  // namespace sr

// iter-460 (SR_ALIAS_OWNER, default ON; =0 A/B): the 0xC / 0xE windows are aliases of
// PHYSICAL memory. aliasGuestVA folds them onto 0x80000000|p (the harness's
// historical choice); for a page only the physical allocator owns, the bytes
// live at 0xA0000000|p. Measured consumer: GpuLoadShaderImmediate_sub_82108300
// copies a busy vertex shader's ucode from (phys - 0x40000000) = the 0xC alias
// into the ring; the 0x8 fold read other memory and ~66K immediate shader loads
// per boot carried all-zero ucode ([PM4_REJECT] addr=0), leaving the previous
// vertex shader bound for those draws.
namespace sr::kernel {
static const bool g_aliasOwner = [] {
  const char* e = std::getenv("SR_ALIAS_OWNER");
  return !e || e[0] != '0';
}();
// iter-465 perf2: the body, always inlined (foldPhysAlias and aliasGuestVASlow below).
static inline __attribute__((always_inline)) uint32_t foldPhysAliasImpl(uint32_t folded) {
  if (!g_aliasOwner) return folded;
  const uint32_t p = folded & 0x1FFFFFFFu;
  const int own = physwin::physPageOwner(p);
  if (own == 0xA) return 0xA0000000u | p;
  if (own == 0 && physwin::physPageContested(p)) {
    // Both a heap-routed and a physical grant claim the page (our windows
    // double-book physical memory; the console cannot). The GPU's reads of such
    // pages take "0x8 unless its word is zero, then 0xA"; the CPU's physical
    // aliases now see the same bytes. Measured: a busy VS's ucode at 0xABA19820
    // (physical grant) under a heap grant's all-zero page -> the immediate copy
    // read zeros.
    const uint8_t* h8 = sr::kernel::guestVAToHost(0x80000000u | (p & ~3u));
    const uint8_t* hA = sr::kernel::guestVAToHost(0xA0000000u | (p & ~3u));
    if (h8 && hA && !(h8[0] | h8[1] | h8[2] | h8[3]) && (hA[0] | hA[1] | hA[2] | hA[3]))
      return 0xA0000000u | p;
  }
  return folded;
}
uint32_t foldPhysAlias(uint32_t folded) { return foldPhysAliasImpl(folded); }

// iter-465 perf2: aliasGuestVA's cold half (the iter-460 inline body), defined here so
// foldPhysAlias / physPageOwner inline into it: one call per physical-window access
// instead of two. preserve_most: guest call sites spill nothing for it.
#if defined(__clang__)
__attribute__((preserve_most, noinline))
#endif
uint32_t aliasGuestVASlow(uint32_t va) {
  if (va < 0xA0000000u) return va;   // virtual heaps
  // iter-427: under the unified model the windows are real memory-mapped views
  // of one section - identity is CORRECT for every VA, including 0xD carveout.
  if (g_physUnifyActive) return va;
  if (va < 0xC0000000u) return g_physAliasAIdentity ? va : (va - 0x20000000u);
  if (va >= 0xD0000000u && va < 0xD0100000u) return va; // fake device/stubs/bump/framebuffer
  if (va >= 0xE0000000u) return foldPhysAliasImpl(va - 0x60000000u);
  return foldPhysAliasImpl(va - 0x40000000u);
}
}  // namespace sr::kernel
