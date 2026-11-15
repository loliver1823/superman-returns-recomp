// p2.zpd: the logical side of the native occlusion queries. Contract: native_zpd.hpp, docs/NATIVE_ROADMAP.md P2.12.
#include "native_zpd.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "../app/log.hpp"
#include "../kernel/highfps.hpp"  // frameStampOn: the sim-freeze diagnostic's configuration
#include "res_scale.hpp"

namespace sr::gfx::zpd {

namespace {

std::atomic<int> g_modeOverride{-1};
std::atomic<uint64_t> g_port{0}, g_portDecl{0};  // the producer port (notePort)
// The frame stamp (native_zpd.hpp FrameStamp): this frame's counts so far (the CP thread's), the stamped frames, the
// test override and the presenting thread's stamp.
std::atomic<uint32_t> g_frameBegins{0}, g_frameRec{0}, g_framePm4{0}, g_frameNo{0};
std::atomic<int> g_stampOverride{-1};
thread_local FrameStamp t_presenting;

// The latch (mode()): SR_NATIVE_ZPD read once a process, its form logged once (native_zpd.hpp mode()).
Request latchRequest() {
  const char* e = std::getenv("SR_NATIVE_ZPD");
  const Request r = requestOf(e);
  if (r.mode == kOff)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_ZPD] SR_NATIVE_ZPD off (=%.16s): the guest path - EVENT_WRITE_ZPD stays guest PM4, no host "
                "query answers it", e ? e : "");
  else if (!r.explicitForm)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_ZPD] SR_NATIVE_ZPD on (the baked default; %s): host D3D12 occlusion queries answer "
                "EVENT_WRITE_ZPD, 82101E08's packets as records (=0 keeps the guest path)", modeName(r.mode));
  else
    SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_ZPD] SR_NATIVE_ZPD on (=%.16s; %s): host D3D12 occlusion queries answer "
                "EVENT_WRITE_ZPD", e, modeName(r.mode));
  return r;
}
const Request& latched() {
  static const Request r = latchRequest();
  return r;
}

struct Seg {
  uint32_t idx = kNoSeg;
  uint32_t spp = 1;  // the draw target's samples per guest pixel
  uint64_t fence = 0;
  bool closed = false, stamped = false;
  Probe probe = kAnswer;  // kRef / kRaster: a probe draw's segment (segmentOpenProbe), never part of the answer
};

struct Query {
  uint32_t id = 0;  // 0 = free
  uint32_t slot = 0, slotSeq = 0;
  uint8_t* host = nullptr;
  uint64_t endEpoch = 0;
  std::chrono::steady_clock::time_point endAt{};  // the CP parsed its END (the answer lag's start)
  bool ended = false, endDrained = false, endRecorded = false, covered = false, dead = false, overflow = false;
  bool answered = false;  // strict fallback written before its fence: the real count is dropped (superseded)
  uint32_t nseg = 0, nAnswer = 0;  // segments in segs / of them the answer's (at most kMaxSegs)
  uint8_t probed = 0;               // probe kinds taken (bit kRef / kRaster): one each, on the first probed draw
  uint32_t trace = 0;               // the live trace's swap when its BEGIN came inside a traced frame (onEvent)
  Seg segs[kMaxSegs + 2];
};

constexpr uint32_t kQueries = 256;
constexpr uint64_t kLostAfterStamps = 64;  // an ended query the render side never recorded: given up after this
constexpr uint32_t kQueryLines = 64;
constexpr uint32_t kTraceLines = 4096;  // [NATIVE_ZPD_T]: every query of the traced frames (a few hundred each)

std::mutex g_mu;
Query g_q[kQueries];
uint32_t g_nextId = 0;
uint32_t g_active = 0;  // the open (begun, not ended) query, 0 = none
std::unordered_map<uint32_t, uint32_t> g_slotSeq;
std::unordered_map<uint32_t, uint32_t> g_slotLast;  // the last count written per slot (the strict fallback's answer)
uint8_t g_segBusy[kHeapCapacity] = {};
uint32_t g_segNext = 0;
uint64_t g_stampEpoch = 0;
std::atomic<uint32_t> g_drainedUnwritten{0};
Stats g_st;
uint32_t g_qLines = 0;
uint32_t g_tLines = 0;

// zpd7 (the Xenia oracle): the outcome of a query of a live-traced frame, one line each - written (the count the guest
// reads), fallback (strict's stand-in), or dropped (superseded / stale / dead / lost: no count of its own reached the
// guest). tools/perf/zpd_xenia.py pairs swap + slot + order with Xenia's SR_ZPD_REPORT for the same END in that .xtr.
void traceLine(const Query& q, const char* how, uint32_t guest, uint64_t host) {
  if (!q.trace || g_tLines >= kTraceLines) return;
  g_tLines++;
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_ZPD_T] swap=%u id=%u slot=%08X %s guest=%u host=%llu", q.trace, q.id,
              q.slot, how, guest, (unsigned long long)host);
}

Query* byId(uint32_t id) {
  if (!id) return nullptr;
  Query& q = g_q[id % kQueries];
  return q.id == id ? &q : nullptr;
}

// A drained END nobody has answered for its fence yet (what fenceNeedsFlush counts).
bool unanswered(const Query& q) { return q.endDrained && !q.covered && !q.answered; }

void dropUnwritten() {
  const uint32_t v = g_drainedUnwritten.load(std::memory_order_relaxed);
  if (v) g_drainedUnwritten.store(v - 1u, std::memory_order_relaxed);
}

void freeSegs(Query& q) {
  for (uint32_t i = 0; i < q.nseg; i++)
    if (q.segs[i].idx < kHeapCapacity) g_segBusy[q.segs[i].idx] = 0;
  q.nseg = 0;
}

// The query leaves the table (written, stale, dead or lost).
void finish(Query& q) {
  if (unanswered(q)) dropUnwritten();
  freeSegs(q);
  if (g_active == q.id) g_active = 0;
  q = Query{};
}

bool current(const Query& q) {
  const auto it = g_slotSeq.find(q.slot);
  return q.host && it != g_slotSeq.end() && it->second == q.slotSeq;
}

void le32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }  // x64: the counts' little-endian layout
void sentinel(uint8_t* p) {  // 0xFFFFFEED as the guest's big-endian store left it
  p[0] = 0xFF;
  p[1] = 0xFF;
  p[2] = 0xFE;
  p[3] = 0xED;
}

uint32_t sppIndex(uint32_t spp) { return spp >= 4 ? 2u : spp >= 2 ? 1u : 0u; }

}  // namespace

Request requestOf(const char* e) {
  if (!e || !*e) return Request{kLatency, false};  // the BAKED default (native/zpd-bake): the latency model
  if (e[0] == '0') return Request{kOff, true};
  if (e[0] == '2' || std::strcmp(e, "latency") == 0) return Request{kLatency, true};
  return Request{kStrict, true};  // "1" / "strict" / any other value
}

Mode mode() {
  const int o = g_modeOverride.load(std::memory_order_relaxed);
  if (o >= 0) return Mode(o);
  return latched().mode;
}

bool explicitForm() {
  if (g_modeOverride.load(std::memory_order_relaxed) >= 0) return false;
  return latched().explicitForm;
}

void notePort(bool taken) { (taken ? g_port : g_portDecl).fetch_add(1, std::memory_order_relaxed); }

const char* modeName(Mode m) { return m == kStrict ? "strict" : m == kLatency ? "latency" : "off"; }

void setModeForTest(Mode m) { g_modeOverride.store(int(m), std::memory_order_relaxed); }

uint32_t normalize(uint64_t samples, uint32_t area) {
  if (!samples) return 0;
  uint64_t n = samples;
  if (area > 1) n = (samples + area / 2u) / area;
  if (n < 1) n = 1;
  return n > kMaxCount ? kMaxCount : uint32_t(n);
}

void writeSlot(uint8_t* slotHost, uint32_t n, void (*step)(void* user, uint32_t store), void* user) {
  if (!slotHost) return;
  if (n > kMaxCount) n = kMaxCount;
  uint8_t* end = slotHost;
  uint8_t* begin = slotHost + kRecordBytes;
  uint32_t k = 0;
  auto stepped = [&] {
    if (step) step(user, k);
    k++;
  };
  // BEGIN {0, S, S, S, 0, 0, 0, 0}: its Total_B / ZFail words equal END's, so they cancel in GetData's differences.
  le32(begin + 0, 0u);
  stepped();
  for (uint32_t i = 1; i < 4; i++) {
    sentinel(begin + 4u * i);
    stepped();
  }
  for (uint32_t i = 4; i < 8; i++) {
    le32(begin + 4u * i, 0u);
    stepped();
  }
  // END {_, S, S, S, n, 0, 0, 0}: END+4/+8/+C stay the preset sentinel (never a "written" signal).
  for (uint32_t i = 1; i < 4; i++) {
    sentinel(end + 4u * i);
    stepped();
  }
  le32(end + 16, n);  // ZPass_A
  stepped();
  for (uint32_t i = 5; i < 8; i++) {
    le32(end + 4u * i, 0u);
    stepped();
  }
  std::atomic_thread_fence(std::memory_order_release);
  // The one "written" signal, last: a reader that sees it sees every word above (x64 keeps stores in order).
  le32(end + 0, n);  // Total_A
  stepped();
}

Event onEvent(uint32_t addr, uint8_t* slotHost, uint32_t trace) {
  std::lock_guard<std::mutex> lk(g_mu);
  addr &= 0x1FFFFFFFu & ~3u;
  const uint32_t rec = addr & ~(kRecordBytes - 1u);
  const uint32_t slot = addr & ~(kSlotBytes - 1u);
  if (!rec) {
    g_st.badAddr++;
    return {};
  }
  if (rec == slot + kRecordBytes) {  // BEGIN
    if (g_active) {
      // Xenia BeginZPDReport: one sample-count register, so a new BEGIN ends the prior report; that one is
      // abandoned (never ended by the guest).
      if (Query* old = byId(g_active)) {
        old->dead = true;
        g_st.abandoned++;
      }
      g_active = 0;
    }
    uint32_t id = ++g_nextId;
    if (!id) id = ++g_nextId;
    Query& q = g_q[id % kQueries];
    if (q.id) {
      g_st.evicted++;
      traceLine(q, "evicted", 0, 0);
      finish(q);
    }
    q.id = id;
    q.slot = slot;
    q.host = slotHost;
    q.slotSeq = ++g_slotSeq[slot];
    q.trace = trace;
    // Astra merge review P1 (2026-10-15): re-arm the slot's publication with the new generation, under g_mu. The guest's
    // Issue preset END+0..+C to the sentinel when it (re-)issued this query, but until this BEGIN the slot's previous
    // query was still current: its collector (latency mode, or any late count) may have written its count over that
    // preset in between - and once this query's fence passed, GetData would return the previous query's count as
    // this one's. Restoring the preset here (what the guest's Issue wrote) closes the window: every later write of the
    // previous query is stale (current() checks the generation advanced above, under the same lock).
    if (slotHost)
      for (uint32_t i = 0; i < 4; i++) sentinel(slotHost + 4u * i);
    g_active = id;
    g_st.issued++;
    if (frameStampOn()) g_frameBegins.fetch_add(1, std::memory_order_relaxed);
    return Event{kBegin, id};
  }
  if (rec == slot) {  // END
    Query* q = byId(g_active);
    g_active = 0;
    if (!q) {
      g_st.orphan++;
      return {};
    }
    if (q->slot != slot) {  // the guest ended another slot than it began: the packet's record is the END
      q->slot = slot;
      q->host = slotHost;
      q->slotSeq = g_slotSeq[slot];
    }
    q->ended = true;
    q->endEpoch = g_stampEpoch;
    q->endAt = std::chrono::steady_clock::now();
    g_st.ended++;
    return Event{kEnd, q->id};
  }
  g_st.badAddr++;
  return {};
}

void markerDropped(const Event& e) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_st.dropped++;
  if (Query* q = byId(e.id)) q->dead = true;
  if (g_active == e.id) g_active = 0;
}

void markerDrained(Op op, uint32_t id) {
  if (op != kEnd) return;
  std::lock_guard<std::mutex> lk(g_mu);
  Query* q = byId(id);
  if (!q || !q->ended || q->endDrained || q->dead) return;
  q->endDrained = true;
  g_drainedUnwritten.fetch_add(1, std::memory_order_relaxed);
  g_st.drained++;
}

bool fenceNeedsFlush() { return g_drainedUnwritten.load(std::memory_order_relaxed) != 0; }

void fenceFallback() {
  std::lock_guard<std::mutex> lk(g_mu);
  for (Query& q : g_q) {
    if (!q.id || !unanswered(q)) continue;
    q.answered = true;
    if (!current(q)) continue;  // re-issued since: the newer query's own fence covers the slot
    const auto it = g_slotLast.find(q.slot);
    const uint32_t n = it != g_slotLast.end() && it->second ? it->second : 1u;
    writeSlot(q.host, n);
    g_slotLast[q.slot] = n;
    g_st.fallback++;
    traceLine(q, "fallback", n, 0);
    if (g_qLines < kQueryLines) {
      g_qLines++;
      SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_ZPD_Q] #%u id=%u slot=%08X FALLBACK guest=%u (strict: the flush did "
                  "not write it before its fence)", g_qLines, q.id, q.slot, n);
    }
  }
  g_drainedUnwritten.store(0, std::memory_order_relaxed);
}

void fenceCovered() {
  std::lock_guard<std::mutex> lk(g_mu);
  for (Query& q : g_q)
    if (q.id && unanswered(q)) {
      q.covered = true;
      g_st.covered++;
    }
  g_drainedUnwritten.store(0, std::memory_order_relaxed);
}

void noteFlush(uint64_t waitUs, bool ok) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_st.flushes++;
  if (!ok) g_st.flushFail++;
  g_st.waitUs += waitUs;
  g_st.maxWaitUs = std::max(g_st.maxWaitUs, waitUs);
}

namespace {
uint32_t openSeg(uint32_t id, uint32_t samplesPerPixel, Probe probe) {
  Query* q = byId(id);
  if (!q || q->dead) return kNoSeg;
  if (probe != kAnswer) {
    // A probe never costs the answer a segment (two extra slots, one per kind) and never marks it overflowed.
    if ((q->probed >> probe) & 1u) return kNoSeg;
  } else if (q->nAnswer >= kMaxSegs) {
    if (!q->overflow) g_st.overSegs++;
    q->overflow = true;
    return kNoSeg;
  }
  if (q->nseg >= sizeof(q->segs) / sizeof(q->segs[0])) {  // the array (unreachable while the two rules above hold)
    if (probe == kAnswer) q->overflow = true;
    return kNoSeg;
  }
  for (uint32_t k = 0; k < kHeapCapacity; k++) {
    const uint32_t idx = (g_segNext + k) % kHeapCapacity;
    if (g_segBusy[idx]) continue;
    g_segBusy[idx] = 1;
    g_segNext = (idx + 1u) % kHeapCapacity;
    const uint32_t spp = samplesPerPixel >= 4 ? 4u : samplesPerPixel >= 2 ? 2u : 1u;
    q->segs[q->nseg++] = Seg{idx, spp, 0, false, false, probe};
    if (probe != kAnswer) {
      q->probed |= uint8_t(1u << probe);
    } else {
      q->nAnswer++;
      g_st.segs++;
      g_st.segsBySpp[sppIndex(spp)]++;
    }
    return idx;
  }
  if (probe != kAnswer) return kNoSeg;
  q->overflow = true;
  g_st.poolFull++;
  return kNoSeg;
}
}  // namespace

uint32_t segmentOpen(uint32_t id, uint32_t samplesPerPixel) {
  std::lock_guard<std::mutex> lk(g_mu);
  return openSeg(id, samplesPerPixel, kAnswer);
}

uint32_t segmentOpenProbe(uint32_t id, Probe kind) {
  if (kind != kRef && kind != kRaster) return kNoSeg;
  std::lock_guard<std::mutex> lk(g_mu);
  return openSeg(id, 1, kind);
}

bool probeOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_NATIVE_ZPD_PROBE");
    return !e || e[0] != '0';
  }();
  return v;
}

void segmentMissed(uint32_t id) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_st.missed++;
  if (Query* q = byId(id)) q->overflow = true;
}

void segmentClosed(uint32_t id, uint32_t seg) {
  std::lock_guard<std::mutex> lk(g_mu);
  Query* q = byId(id);
  if (!q) {
    if (seg < kHeapCapacity) g_segBusy[seg] = 0;  // its resolve lands before any later user's (one queue)
    return;
  }
  for (uint32_t i = 0; i < q->nseg; i++)
    if (q->segs[i].idx == seg) q->segs[i].closed = true;
}

void endRecorded(uint32_t id, bool begun) {
  std::lock_guard<std::mutex> lk(g_mu);
  Query* q = byId(id);
  if (!q) return;
  if (!begun) q->overflow = true;  // its BEGIN never reached this list owner: no draw was measured ("visible")
  q->endRecorded = true;
}

void stamp(uint64_t fence) {
  std::lock_guard<std::mutex> lk(g_mu);
  g_stampEpoch++;
  for (Query& q : g_q) {
    if (!q.id) continue;
    for (uint32_t i = 0; i < q.nseg; i++)
      if (q.segs[i].closed && !q.segs[i].stamped) {
        q.segs[i].stamped = true;
        q.segs[i].fence = fence;
      }
  }
}

void collect(uint64_t completedFence, const uint64_t* readback, uint32_t area, bool partial) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (Query& q : g_q) {
    if (!q.id || q.id == g_active) continue;
    const bool closedOut = q.endRecorded || q.dead;
    if (!closedOut) {
      // Ended (or never recorded) and the render side never saw its END: give it up after a while.
      if (q.ended && g_stampEpoch - q.endEpoch > kLostAfterStamps) {
        g_st.lost++;
        traceLine(q, "lost", 0, 0);
        finish(q);
      }
      continue;
    }
    bool done = true;
    for (uint32_t i = 0; i < q.nseg && done; i++)
      done = q.segs[i].closed && q.segs[i].stamped && q.segs[i].fence <= completedFence;
    if (!done) continue;
    if (q.dead || !q.ended) {
      traceLine(q, "dead", 0, 0);
      finish(q);
      continue;
    }
    uint64_t host = 0, samples = 0, probeHost[3] = {};
    uint32_t sppMask = 0, probes = 0, probeKinds = 0;
    for (uint32_t i = 0; i < q.nseg; i++) {
      const Probe kind = q.segs[i].probe;
      if (kind != kAnswer) {
        probes++;
        probeKinds |= 1u << kind;
      }
      if (readback && q.segs[i].idx < kHeapCapacity) {
        if (kind != kAnswer) {
          probeHost[kind] += readback[q.segs[i].idx];
          continue;
        }
        host += readback[q.segs[i].idx];
        samples += readback[q.segs[i].idx] * q.segs[i].spp;
        sppMask |= q.segs[i].spp;
      }
    }
    const bool hasRef = probeKinds & (1u << kRef), hasRaster = probeKinds & (1u << kRaster);
    if (probes && readback) {
      g_st.probes++;
      if (!host) {
        if (hasRef && probeHost[kRef]) g_st.depthKilled++;
        else if (hasRaster && probeHost[kRaster]) g_st.psKilled++;
        else g_st.noRaster++;
      }
    }
    // A guest draw no host segment covered (no heap, over kMaxSegs, the pool full, its BEGIN unseen) answers
    // "visible" when nothing else counted, as Xenia's exhausted pool in fast mode.
    uint32_t n = normalize(samples, area);
    if (q.overflow && !n) n = 1;
    g_st.resolved++;
    if (q.answered) {  // the strict fallback answered it before its fence: this count is dropped
      g_st.superseded++;
      traceLine(q, "superseded", n, host);
    } else if (!current(q)) {
      g_st.stale++;
      traceLine(q, "stale", n, host);
    } else {
      writeSlot(q.host, n);
      g_slotLast[q.slot] = n;
      g_st.written++;
      traceLine(q, "written", n, host);
      if (q.covered) g_st.late++;
      if (partial) g_st.viaPartial++;
      else g_st.viaPresent++;
      if (n) g_st.nonzero++;
      else g_st.zero++;
      g_st.lastHost = host;
      g_st.lastGuest = n;
      const uint64_t lagUs = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - q.endAt).count());
      g_st.lagSumUs += lagUs;
      g_st.lagMaxUs = std::max(g_st.lagMaxUs, lagUs);
      g_st.lagN++;
      if (g_qLines < kQueryLines) {
        g_qLines++;
        char ref[64] = "";
        int rp = 0;
        if (hasRef) rp += std::snprintf(ref + rp, sizeof ref - rp, " ref=%llu", (unsigned long long)probeHost[kRef]);
        if (hasRaster)
          std::snprintf(ref + rp, sizeof ref - rp, " raster=%llu", (unsigned long long)probeHost[kRaster]);
        SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[NATIVE_ZPD_Q] #%u id=%u slot=%08X host=%llu spp=%s guest=%u area=%u segs=%u via=%s lagMs=%.1f%s%s%s",
                    g_qLines, q.id, q.slot, (unsigned long long)host,
                    sppMask == 1 ? "1" : sppMask == 2 ? "2" : sppMask == 4 ? "4" : sppMask ? "mixed" : "-", n, area,
                    q.nseg - probes, partial ? "partial" : "present", double(lagUs) * 1e-3, ref,
                    q.covered ? " LATE" : "", q.overflow ? " overflow" : "");
      }
    }
    finish(q);
  }
}

Stats stats() {
  std::lock_guard<std::mutex> lk(g_mu);
  Stats s = g_st;
  uint32_t live = 0, inflight = 0;
  for (const Query& q : g_q) {
    if (q.id) live++;
    if (q.id && q.ended) inflight++;
  }
  s.live = live;
  s.inflight = inflight;
  s.port = g_port.load(std::memory_order_relaxed);
  s.portDecl = g_portDecl.load(std::memory_order_relaxed);
  return s;
}

void resetForTest() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_port.store(0, std::memory_order_relaxed);
  g_portDecl.store(0, std::memory_order_relaxed);
  for (Query& q : g_q) q = Query{};
  g_nextId = 0;
  g_active = 0;
  g_slotSeq.clear();
  g_slotLast.clear();
  std::memset(g_segBusy, 0, sizeof(g_segBusy));
  g_segNext = 0;
  g_stampEpoch = 0;
  g_drainedUnwritten.store(0, std::memory_order_relaxed);
  g_st = Stats{};
  g_qLines = 0;
  g_tLines = 0;
  g_frameBegins.store(0, std::memory_order_relaxed);
  g_frameRec.store(0, std::memory_order_relaxed);
  g_framePm4.store(0, std::memory_order_relaxed);
  g_frameNo.store(0, std::memory_order_relaxed);
  t_presenting = FrameStamp{};
}

bool frameStampOn() {
  const int o = g_stampOverride.load(std::memory_order_relaxed);
  if (o >= 0) return o != 0;
  static const bool s_on = [] {
    const char* e = std::getenv("SR_PRESENT_DUMP_FREEZE");
    return sr::kernel::highfps::simFreezeOn() && e && (e[0] == '1' || e[0] == '2');
  }();
  return s_on;
}

void setFrameStampForTest(int on) { g_stampOverride.store(on, std::memory_order_relaxed); }

void noteSource(bool record) {
  if (!frameStampOn()) return;
  (record ? g_frameRec : g_framePm4).fetch_add(1, std::memory_order_relaxed);
}

FrameStamp takeFrameStamp() {
  if (!frameStampOn()) return FrameStamp{};
  FrameStamp s;
  s.begins = g_frameBegins.exchange(0, std::memory_order_relaxed);
  s.rec = g_frameRec.exchange(0, std::memory_order_relaxed);
  s.pm4 = g_framePm4.exchange(0, std::memory_order_relaxed);
  s.frame = g_frameNo.fetch_add(1, std::memory_order_relaxed) + 1u;
  if (!s.frame) s.frame = g_frameNo.fetch_add(1, std::memory_order_relaxed) + 1u;  // 0 means "no stamp"
  return s;
}

void setPresentingStamp(const FrameStamp& s) { t_presenting = s; }

FrameStamp presentingStamp() { return t_presenting; }

void logLine(const char* why) {
  const Mode m = mode();
  const Stats s = stats();
  if (m == kOff && !s.issued) return;
  const auto& sc = sr::gfx::resscale::get();
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_ZPD] %s mode=%s issued=%llu ended=%llu drained=%llu resolved=%llu written=%llu late=%llu "
              "stale=%llu orphan=%llu abandoned=%llu evicted=%llu dropped=%llu lost=%llu badAddr=%llu covered=%llu "
              "fallback=%llu superseded=%llu poolFull=%llu missed=%llu overSegs=%llu flushes=%llu flushFail=%llu "
              "waitMs=%.1f maxWaitMs=%.2f segs=%llu spp1=%llu spp2=%llu spp4=%llu zero=%llu nonzero=%llu "
              "lastHost=%llu lastGuest=%u live=%u inflight=%u scale=%ux%u viaPartial=%llu viaPresent=%llu probes=%llu "
              "depthKilled=%llu psKilled=%llu noRaster=%llu lagAvgMs=%.1f lagMaxMs=%.1f port=%llu portDecl=%llu",
              why ? why : "-", modeName(m), (unsigned long long)s.issued, (unsigned long long)s.ended,
              (unsigned long long)s.drained, (unsigned long long)s.resolved, (unsigned long long)s.written,
              (unsigned long long)s.late, (unsigned long long)s.stale, (unsigned long long)s.orphan,
              (unsigned long long)s.abandoned, (unsigned long long)s.evicted, (unsigned long long)s.dropped,
              (unsigned long long)s.lost, (unsigned long long)s.badAddr, (unsigned long long)s.covered,
              (unsigned long long)s.fallback, (unsigned long long)s.superseded, (unsigned long long)s.poolFull,
              (unsigned long long)s.missed, (unsigned long long)s.overSegs, (unsigned long long)s.flushes,
              (unsigned long long)s.flushFail, double(s.waitUs) * 1e-3, double(s.maxWaitUs) * 1e-3,
              (unsigned long long)s.segs, (unsigned long long)s.segsBySpp[0], (unsigned long long)s.segsBySpp[1],
              (unsigned long long)s.segsBySpp[2], (unsigned long long)s.zero, (unsigned long long)s.nonzero,
              (unsigned long long)s.lastHost, s.lastGuest, s.live, s.inflight, sc.x, sc.y,
              (unsigned long long)s.viaPartial, (unsigned long long)s.viaPresent, (unsigned long long)s.probes,
              (unsigned long long)s.depthKilled, (unsigned long long)s.psKilled, (unsigned long long)s.noRaster,
              s.lagN ? double(s.lagSumUs) * 1e-3 / double(s.lagN) : 0.0, double(s.lagMaxUs) * 1e-3,
              (unsigned long long)s.port, (unsigned long long)s.portDecl);
}

}  // namespace sr::gfx::zpd
