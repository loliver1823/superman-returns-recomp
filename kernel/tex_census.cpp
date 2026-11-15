// iter-458 texture-path allocation/upload census - contract in tex_census.hpp.
#include "tex_census.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "guest_context.hpp"
#include "phys_window.hpp"
#include "../app/log.hpp"

namespace sr { namespace gfx { namespace hle {
bool traceRecorderOpen();
uint64_t txcFifoPublished();
} } }

namespace sr {
namespace kernel {
namespace txc {
namespace {

constexpr uint32_t kMagic = 0x31435854u;     // 'TXC1'
constexpr uint32_t kVersion = 2;             // Astra 069: 80-byte events
constexpr uint32_t kDefaultCap = 1u << 21;   // 2M events (160 MB) before overflow
constexpr uint32_t kDrawPerKeyOutside = 64;  // flagged-page draws recorded per key, outside the window
constexpr uint32_t kPages = 0x20000u;        // 512 MB / 4 KB physical pages
constexpr uint32_t kSampleBytes = 64;        // content probe: 3 x 64 bytes
constexpr uint32_t kMask = 0x1FFFFFFFu;

std::mutex g_mu;                 // the event buffer, seq, maps below
std::atomic<int> g_state{-1};    // -1 unresolved, 0 off, 1 on
std::string g_path;
uint32_t g_cap = kDefaultCap;
std::vector<Ev> g_ev;
uint64_t g_seq = 0, g_overflow = 0, g_byType[kTypes] = {};
bool g_flushed = false;
uint32_t g_pubOrd = 0, g_applyOrd = 0;

// Astra 069: the descriptor key is the exact triple, equality-checked (the
// XOR packing let fmt 6 256x256 and fmt 2 1280x256 collide at one base).
struct TupleKey {
  uint32_t base, fmt, dims;
  bool operator==(const TupleKey& o) const {
    return base == o.base && fmt == o.fmt && dims == o.dims;
  }
};
struct TupleKeyHash {
  size_t operator()(const TupleKey& k) const {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t v : {k.base, k.fmt, k.dims}) { h ^= v; h *= 1099511628211ull; }
    return (size_t)h;
  }
};
// Producer: per descriptor, the alias its upload was taken from and the last
// EMITTED state (the payload's hash first, then each observed change).
struct TupleState {
  uint64_t ownerDigest = 0, content = 0;
  uint32_t window = 0;   // 8 / 0xA; 0 = no ISSUED upload seen for this descriptor
};
std::unordered_map<TupleKey, TupleState, TupleKeyHash> g_tuples;
// Render thread: what each band's slot currently holds, per key.
struct Resident { uint32_t source = 0, ordinal = 0; };
std::unordered_map<uint32_t, Resident> g_resident[kBands];
std::unordered_map<uint32_t, uint32_t> g_drawBudget;
std::atomic<uint8_t> g_flag[kPages];
// The census's own cost (Mark 1).
std::atomic<uint64_t> g_probes{0}, g_probeNs{0}, g_probeMaxNs{0}, g_decodeNs{0}, g_drawNs{0};
std::atomic<void (*)()> g_decodeHook{nullptr};   // test seam
// Astra 070: one decode's observation (owners, hash) and its append are ONE
// step (append order == observation order); and bind observations in flight
// (BindToken alive) are counted so the flush can wait for them.
std::mutex g_obs;
std::atomic<int64_t> g_inflight{0};
constexpr int kFlushWaitMs = 250;
// The upload THIS thread published last (set in onTexPublish, which runs on
// the publishing thread inside the queue lock; reset by bindBegin). An ISSUED
// decode names exactly the upload its own bind enqueued - no other path's
// publish of the same descriptor can be mistaken for it.
thread_local uint64_t t_lastPubFifo = 0, t_lastPubHash = 0;

uint64_t nowNs() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool resolveOn() {
  int s = g_state.load(std::memory_order_acquire);
  if (s >= 0) return s == 1;
  std::lock_guard<std::mutex> lk(g_mu);
  s = g_state.load(std::memory_order_relaxed);
  if (s < 0) {
    const char* e = std::getenv("SR_TEX_OWN_CENSUS");
    g_path = (e && *e) ? e : "";
    s = g_path.empty() ? 0 : 1;
    if (s) {
      g_ev.reserve(1u << 16);
      sr::kernel::physwin::armRecorderCloseHook();   // the acquisition ends at the recorder's close
      SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[TXC] texture-path census -> '%s' (cap %u events, written at the "
                  "recorder's first close)",
                  g_path.c_str(), g_cap);
    }
    g_state.store(s, std::memory_order_release);
  }
  return s == 1;
}

// Called with g_mu held.
void appendLocked(const Ev& in) {
  const uint64_t seq = ++g_seq;          // a dropped event still consumes a seq
  if (g_flushed) return;                 // the acquisition has ended
  if (g_ev.size() >= g_cap) { g_overflow++; return; }
  Ev e = in;
  e.seq = seq;
  g_ev.push_back(e);
  if (e.type < kTypes) g_byType[e.type]++;
}

// The three probe offsets of a span (start, middle, end), shared by the host
// (payload) and guest (alias) hashes so equal bytes hash equal.
void probeOffsets(uint32_t span, uint32_t offs[3], uint32_t& n) {
  n = span < kSampleBytes ? span : kSampleBytes;
  offs[0] = 0;
  offs[1] = span > kSampleBytes ? (span / 2) & ~63u : 0u;
  offs[2] = span > kSampleBytes ? span - kSampleBytes : 0u;
}

// The guest alias `va` (0x8/0xA window | base): 0 when not readable
// (fault-free check through the interval cache - an uncached VirtualQuery on
// the guest reservation costs ~9.5 ms, H-VQ).
uint64_t sampleGuest(uint32_t va, uint32_t span) {
  if (!span) return 0;
  uint32_t offs[3], n;
  probeOffsets(span, offs, n);
  uint64_t h = 1469598103934665603ull;
  for (uint32_t k = 0; k < 3; k++) {
    const uint8_t* p = sr::kernel::guestVAToHost(va + offs[k]);
    if (!p || !sr::kernel::hostRangeReadableCached(p, n)) return 0;
    for (uint32_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
  }
  return h | 1;
}

uint32_t aliasBase(uint32_t window) {
  return window == 8 ? 0x80000000u : window == 0xA ? 0xA0000000u : 0u;
}

void flagSpan(uint32_t base, uint32_t span) {
  const uint32_t p0 = (base & kMask) >> 12;
  const uint32_t n = (span + 4095u) >> 12;
  for (uint32_t i = 0; i < n && p0 + i < kPages; i++)
    g_flag[p0 + i].store(1, std::memory_order_relaxed);
}

void putU32(std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
void putU64(std::string& s, uint64_t v) { s.append(reinterpret_cast<const char*>(&v), 8); }

struct DecodeCtx {
  uint64_t p0, gen0;     // the bind's clock lower bound and registry generation
  TupleKey key;
  uint32_t span;
  bool issued;
  uint32_t window;       // ISSUED: the alias the snapshot came from
  uint64_t content;      // ISSUED: the payload's hash (taken at publish)
  uint64_t uploadFifo;   // ISSUED: this thread's last publish
};

// Runs under the registry's shared lock (physwin::withOwnersOfSpan): every
// grant event is recorded under the unique lock, so none can fall between
// these owners and this event. Under g_obs the observation (a suppressed
// bind's hash, in the upload's alias) and its append are one step; P1 is read
// INSIDE g_mu immediately before the append (Astra 070), so [P0, P1] covers
// the observation and its append.
void decodeUnderRegistry(const sr::kernel::physwin::SpanOwners& o, void* v) {
  const DecodeCtx& c = *static_cast<const DecodeCtx*>(v);
  std::lock_guard<std::mutex> obs(g_obs);
  const uint32_t base = c.key.base, span = c.span;
  uint32_t window = c.window;
  uint64_t content = c.content;
  if (!c.issued) {
    {
      std::lock_guard<std::mutex> lk(g_mu);
      auto it = g_tuples.find(c.key);
      window = it != g_tuples.end() ? it->second.window : 0u;
    }
    // Only the alias the upload was taken from: the unchosen alias changing
    // says nothing about the resident payload.
    content = 0;
    if (window) {
      const uint64_t h0 = nowNs();
      content = sampleGuest(aliasBase(window) | base, span);
      const uint64_t ns = nowNs() - h0;
      g_probes.fetch_add(1, std::memory_order_relaxed);
      g_probeNs.fetch_add(ns, std::memory_order_relaxed);
      uint64_t mx = g_probeMaxNs.load(std::memory_order_relaxed);
      while (ns > mx && !g_probeMaxNs.compare_exchange_weak(mx, ns, std::memory_order_relaxed)) {}
    }
  }
  if (void (*hook)() = g_decodeHook.load(std::memory_order_relaxed)) hook();
  std::lock_guard<std::mutex> lk(g_mu);
  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();   // at the append
  uint32_t decision = 0, flags = 0;
  auto it = g_tuples.find(c.key);
  if (c.issued) {
    decision = kIssued;
    g_tuples[c.key] = TupleState{o.setDigest, content, window};
    if (o.gen != c.gen0) flags |= kDecOwnersUnstable;
  } else if (it == g_tuples.end()) {
    decision = kNoBaseline;   // uploaded before the census saw it: UNKNOWN baseline
    g_tuples[c.key] = TupleState{o.setDigest, 0, 0};
  } else if (it->second.ownerDigest != o.setDigest || it->second.content != content) {
    decision = kSuppressedChanged;   // same descriptor, new owners/content, no upload
    it->second.ownerDigest = o.setDigest;
    it->second.content = content;
  } else {
    return;   // unchanged suppressed bind: nothing new to record
  }
  if (o.aliasedPairs) flags |= kDecAliased;
  if (!content) flags |= kDecHashUnreadable;
  if (decision == kSuppressedChanged || o.nLive > 1 || (flags & kDecAliased))
    flagSpan(base, span);
  appendLocked(Ev{kTexDecode, base, span, c.key.fmt, c.key.dims,
                  decision | (std::min<uint32_t>(o.nLive, 255u) << 8) |
                      ((o.kindBits & 0xFFu) << 16) | ((window & 0xFu) << 24) | flags,
                  o.serial[0], o.serial[1], 0, content, c.issued ? c.uploadFifo : 0,
                  c.p0, p1, o.setDigest});
}

struct FlushCtx {
  const char* why;
  uint64_t waitedMs;
  bool timedOut;
  bool ok;
};
void flushUnderRegistry(void* v);

}  // namespace

bool enabled() { return resolveOn(); }

void setForTest(const char* path, uint32_t cap) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_path = path ? path : "";
  g_state.store(path ? 1 : 0, std::memory_order_release);
  g_cap = cap ? cap : kDefaultCap;
  g_ev.clear();
  g_seq = g_overflow = 0;
  for (uint64_t& b : g_byType) b = 0;
  g_flushed = false;
  g_pubOrd = g_applyOrd = 0;
  g_tuples.clear();
  g_probes = 0;
  g_probeNs = 0;
  g_probeMaxNs = 0;
  g_decodeNs = 0;
  g_drawNs = 0;
  for (auto& r : g_resident) r.clear();
  g_drawBudget.clear();
  for (auto& f : g_flag) f.store(0, std::memory_order_relaxed);
}

uint64_t probeHash(const uint8_t* p, uint32_t span) {
  if (!p || !span) return 0;
  uint32_t offs[3], n;
  probeOffsets(span, offs, n);
  uint64_t h = 1469598103934665603ull;
  for (uint32_t k = 0; k < 3; k++)
    for (uint32_t i = 0; i < n; i++) { h ^= p[offs[k] + i]; h *= 1099511628211ull; }
  return h | 1;
}

// Called under the registry's unique lock, after the map write; P1 is read
// inside g_mu at the append (Astra 070), like every observation.
void onGrantReg(uint32_t kind, uint32_t va, uint32_t size, uint32_t serial, uint64_t p0) {
  if (!resolveOn()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();
  appendLocked(Ev{kGrantReg, kind, va, size, serial, 0, 0, 0, 0, 0, 0, p0, p1, 0});
}

void onGrantRel(uint32_t kind, uint32_t va, uint32_t serial, uint32_t how, uint64_t p0) {
  if (!resolveOn()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();
  appendLocked(Ev{kGrantRel, kind, va, 0, serial, how, 0, 0, 0, 0, 0, p0, p1, 0});
}

BindToken::~BindToken() {
  if (on) g_inflight.fetch_sub(1, std::memory_order_acq_rel);
}

BindToken bindBegin() {
  BindToken t;
  if (!resolveOn()) return t;
  g_inflight.fetch_add(1, std::memory_order_acq_rel);
  t.on = true;
  t.p0 = sr::gfx::hle::txcFifoPublished();
  t.gen0 = sr::kernel::physwin::registryGeneration();
  t_lastPubFifo = 0;
  t_lastPubHash = 0;
  return t;
}

void onTexDecode(const BindToken& tok, uint32_t base, uint32_t span, uint32_t fmtBits,
                 uint32_t dims, bool issued, uint32_t window) {
  if (!resolveOn() || !tok.on) return;
  const uint64_t t0 = nowNs();
  // ISSUED: the baseline is the IMMUTABLE payload this bind published (hashed
  // at publish), never a re-read of guest memory after the snapshot. A
  // suppressed bind is observed inside decodeUnderRegistry.
  DecodeCtx c{tok.p0, tok.gen0, TupleKey{base & kMask, fmtBits & 0xFFFFu, dims}, span, issued,
              issued ? window : 0u, issued ? t_lastPubHash : 0u, issued ? t_lastPubFifo : 0u};
  sr::kernel::physwin::withOwnersOfSpan(c.key.base, span, &decodeUnderRegistry, &c);
  g_decodeNs.fetch_add(nowNs() - t0, std::memory_order_relaxed);
}

void onTexPublish(uint32_t key, uint32_t arenaOffset, uint32_t bytes, uint32_t dims,
                  uint32_t fmtBits, uint64_t fifoIdx, const uint8_t* payload) {
  if (!resolveOn()) return;
  const uint64_t ph = probeHash(payload, bytes);
  t_lastPubFifo = fifoIdx;
  t_lastPubHash = ph;
  std::lock_guard<std::mutex> lk(g_mu);
  appendLocked(Ev{kTexPublish, key & kMask, arenaOffset, bytes, dims, fmtBits, ++g_pubOrd, 0, 0,
                  ph, 0, fifoIdx, 0, 0});
}

void onTexApply(uint32_t key, uint32_t result, uint32_t band, uint32_t dims, uint32_t fmtBits,
                uint64_t execIdx) {
  if (!resolveOn()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  const uint32_t ord = ++g_applyOrd;
  appendLocked(Ev{kTexApply, key & kMask, result, band, dims, fmtBits, ord, 0, 0, 0, 0, execIdx,
                  0, 0});
}

void onResident(uint32_t key, uint32_t band, uint32_t source, uint64_t execIdx) {
  if (!resolveOn() || band >= kBands) return;
  key &= kMask;
  std::lock_guard<std::mutex> lk(g_mu);
  // An upload's slot write precedes its arm's apply witness: it names the
  // ordinal that witness will carry (the analyzer checks they agree).
  const uint32_t ord = source == kFromUpload ? g_applyOrd + 1 : 0u;
  g_resident[band][key] = Resident{source, ord};
  appendLocked(Ev{kResident, key, source, band, 0, 0, ord, 0, 0, 0, 0, execIdx, 0, 0});
}

void onDrawTex(uint32_t drawOrdinal, uint32_t sampler, uint32_t key, uint32_t fetchBase,
               uint32_t fetchFmt, bool fetchValid, uint32_t fetchDim, bool fetchStacked,
               uint32_t fetchDims, uint64_t psHash, uint64_t execIdx) {
  if (!resolveOn()) return;
  key &= kMask;
  if (!key) return;
  const uint64_t t0 = nowNs();
  const bool window = sr::gfx::hle::traceRecorderOpen();
  if (!window && !g_flag[key >> 12].load(std::memory_order_relaxed)) return;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!window && g_drawBudget[key]++ >= kDrawPerKeyOutside) return;
    Resident r[kBands];
    uint32_t populated = 0;
    for (uint32_t b = 0; b < kBands; b++) {
      auto it = g_resident[b].find(key);
      if (it != g_resident[b].end()) { r[b] = it->second; populated |= 1u << b; }
    }
    appendLocked(Ev{kDrawTex, drawOrdinal, sampler, key, fetchBase & kMask,
                    (fetchFmt & 0xFFu) | ((fetchValid ? 1u : 0u) << 8) | ((fetchDim & 3u) << 9) |
                        ((window ? 1u : 0u) << 11) | (populated << 12) |
                        ((fetchStacked ? 1u : 0u) << 15) |
                        ((r[kBand2D].source & 0xFu) << 16) | ((r[kBand3D].source & 0xFu) << 20) |
                        ((r[kBandCube].source & 0xFu) << 24),
                    fetchDims, r[kBand2D].ordinal, 0, psHash,
                    (uint64_t)r[kBand3D].ordinal | ((uint64_t)r[kBandCube].ordinal << 32),
                    execIdx, 0, 0});
  }
  g_drawNs.fetch_add(nowNs() - t0, std::memory_order_relaxed);
}

void onWindowEdge(bool open, uint64_t execIdx, const WindowCounters& n, uint64_t recordIndex) {
  if (!resolveOn()) return;
  std::lock_guard<std::mutex> lk(g_mu);
  appendLocked(Ev{kMark, open ? kMarkWindowOpen : kMarkWindowClose, (uint32_t)recordIndex,
                  (uint32_t)(recordIndex >> 32), 0, 0, 0, 0, 0, n.drawCmds, n.drawsAtTap, execIdx,
                  n.bindingsSeen, n.whiteoutSkips});
}

void setDecodeHookForTest(void (*fn)()) { g_decodeHook.store(fn, std::memory_order_relaxed); }

namespace {
void flushUnderRegistry(void* v) {
  FlushCtx& fc = *static_cast<FlushCtx*>(v);
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_flushed) return;
  // Mark 1: the census's own tap time (decode taps: owner sample, hash, locks,
  // append; draw taps; the hash-probe subset) and the observations still in
  // flight when written. It excludes publish-side hashing and this flush; the
  // census's effect on the run's timing is UNMEASURED (no A/B).
  const int64_t inflight = g_inflight.load(std::memory_order_acquire);
  appendLocked(Ev{kMark, kMarkCost,
                  (uint32_t)std::min<uint64_t>(g_probes.load(std::memory_order_relaxed), 0xFFFFFFFFull),
                  (uint32_t)(inflight > 0 ? inflight : 0), (uint32_t)fc.waitedMs,
                  fc.timedOut ? 1u : 0u, 0, 0, 0, g_probeNs.load(std::memory_order_relaxed),
                  g_probeMaxNs.load(std::memory_order_relaxed), 0,
                  g_decodeNs.load(std::memory_order_relaxed),
                  g_drawNs.load(std::memory_order_relaxed)});
  g_flushed = true;   // the acquisition ends here whether or not the write lands
  std::string hdr;
  putU32(hdr, kMagic);
  putU32(hdr, kVersion);
  putU32(hdr, (uint32_t)sizeof(Ev));
  putU32(hdr, 1u);                       // release routes hooked: bit0 MmFreePhysicalMemory
  putU64(hdr, (uint64_t)g_ev.size());
  putU64(hdr, g_overflow);
  putU64(hdr, g_seq);
  for (uint64_t b : g_byType) putU64(hdr, b);
  std::FILE* f = std::fopen(g_path.c_str(), "wb");
  bool ok = f != nullptr;
  if (ok) ok = std::fwrite(hdr.data(), 1, hdr.size(), f) == hdr.size();
  if (ok && !g_ev.empty())
    ok = std::fwrite(g_ev.data(), sizeof(Ev), g_ev.size(), f) == g_ev.size();
  if (ok) ok = std::fflush(f) == 0;
  if (f && std::fclose(f) != 0) ok = false;
  SR_LOG_BOOT(ok ? sr::LogSeverity::Warn : sr::LogSeverity::Error,
              "[TXC] %s (%s): %zu events, overflow=%llu, lastSeq=%llu, in flight=%lld "
              "(waited %llu ms%s) -> '%s'",
              ok ? "written" : "WRITE FAILED", fc.why, g_ev.size(),
              (unsigned long long)g_overflow, (unsigned long long)g_seq, (long long)inflight,
              (unsigned long long)fc.waitedMs, fc.timedOut ? ", TIMED OUT" : "", g_path.c_str());
  fc.ok = ok;
}
}  // namespace

bool flush(const char* why) {
  if (!resolveOn()) return false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_flushed) return false;
  }
  // Astra 070: an observation in flight now would be lost - wait (bounded)
  // for them, holding no census or registry lock; then write under the
  // registry's shared lock so no grant event straddles the flush.
  const auto t0 = std::chrono::steady_clock::now();
  bool timedOut = false;
  while (g_inflight.load(std::memory_order_acquire) > 0) {
    if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(kFlushWaitMs)) {
      timedOut = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  FlushCtx fc{why,
              (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count(),
              timedOut, false};
  sr::kernel::physwin::withRegistryShared(&flushUnderRegistry, &fc);
  return fc.ok;
}

Stats statsForTest() {
  std::lock_guard<std::mutex> lk(g_mu);
  Stats s;
  s.events = g_ev.size();
  s.overflow = g_overflow;
  for (uint32_t i = 0; i < kTypes; i++) s.byType[i] = g_byType[i];
  s.lastSeq = g_seq;
  s.flushed = g_flushed;
  return s;
}

uint64_t eventsForTest(Ev* out, uint64_t max) {
  std::lock_guard<std::mutex> lk(g_mu);
  const uint64_t n = g_ev.size() < max ? g_ev.size() : max;
  if (out && n) std::memcpy(out, g_ev.data(), n * sizeof(Ev));
  return n;
}

}  // namespace txc
}  // namespace kernel
}  // namespace sr
