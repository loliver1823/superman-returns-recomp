// NATIVE ROADMAP N0.2 / N0.5: the true per-packet PM4 counter and the bracket
// coverage map. Contract and gates: pm4_coverage.hpp.

#include <intrin.h>
#include <windows.h>
#include "pm4_coverage.hpp"

#include "../app/log.hpp"
#include "../kernel/guest_context.hpp"
#include "../kernel/native_brackets.hpp"
#include "native_device.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace sr::gfx::pm4cov {

namespace {
bool envOn(const char* n) {
  const char* e = std::getenv(n);
  return e && e[0] == '1';
}
}  // namespace

bool g_on = envOn("SR_PM4_COVERAGE") || envOn("SR_NATIVE_BRACKETS") || envOn("SR_NATIVE_RECORD") ||
           envOn("SR_NATIVE_DRAW");

namespace {

constexpr int kOpKeys = 256 + 256 + kOtherKeys;  // type-3 op | 256 + type-0 page | 512 + other (+ the witnessed pad)
constexpr int kMaxDepth = 6;

inline uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

struct Counters {
  uint64_t t0Pk[256], t0Dw[256];
  uint64_t t3Pk[256], t3Dw[256], t3Pred[256];
  uint64_t other[kOtherKinds];
  uint64_t setConst[8], loadConst[8];
  uint64_t ringIb;              // ring-level IB descriptors (also in t3Pk[0x3F])
  uint64_t depth0, depthN;      // packets read from the ring / from an IB
  uint64_t unstamped, accounted;
  uint64_t chain;               // of the unstamped: read from a chained close's unowned list node (kStampChain)
  void clear() { std::memset(this, 0, sizeof(*this)); }
  uint64_t packets() const {
    uint64_t n = other[kType1] + other[kType2] + other[kType1Skip] + other[kChainTail];
    for (int i = 0; i < 256; i++) n += t0Pk[i] + t3Pk[i] + t3Pred[i];
    return n;
  }
  uint64_t t0() const { uint64_t n = 0; for (int i = 0; i < 256; i++) n += t0Pk[i]; return n; }
  uint64_t t0dw() const { uint64_t n = 0; for (int i = 0; i < 256; i++) n += t0Dw[i]; return n; }
  uint64_t t3() const { uint64_t n = 0; for (int i = 0; i < 256; i++) n += t3Pk[i]; return n; }
  uint64_t t3dw() const { uint64_t n = 0; for (int i = 0; i < 256; i++) n += t3Dw[i]; return n; }
  uint64_t pred() const { uint64_t n = 0; for (int i = 0; i < 256; i++) n += t3Pred[i]; return n; }
  void add(const Counters& o) {
    const uint64_t* s = reinterpret_cast<const uint64_t*>(&o);
    uint64_t* d = reinterpret_cast<uint64_t*>(this);
    for (size_t i = 0; i < sizeof(Counters) / 8; i++) d[i] += s[i];
  }
};

Counters g_cur, g_cum;  // CP thread (or replay thread) only
uint64_t g_frames = 0, g_pkMax = 0, g_pkMin = ~0ull;
std::chrono::steady_clock::time_point g_lastReport;
// How frames ended (presented(); the [PM4_COV] ENDS line): at the XE_SWAP packet hook, or at a present the CP applied
// with no 0x64 packet before it; the presents the CP applied while counting. g_swapPacketEnded: packet3 ended the frame
// at a 0x64 packet just now and the CP is about to apply that packet's present (cleared by the next counted packet).
uint64_t g_endsXeSwap = 0, g_endsPresent = 0, g_presents = 0;
bool g_swapPacketEnded = false;
bool g_armed = false;

// The counter's positive witness, once, at its first hook (CP thread): the gates it read and how its frames end.
void armed() {
  if (g_armed) return;
  g_armed = true;
  auto gate = [](const char* n) {
    const char* e = std::getenv(n);
    return e ? e : "unset";
  };
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PM4_COV] armed brackets=%d SR_PM4_COVERAGE=%s SR_NATIVE_BRACKETS=%s SR_NATIVE_RECORD=%s "
              "SR_NATIVE_DRAW=%s | frames end at an XE_SWAP packet or at a present the CP applies without one; one "
              "F line per frame (0 packets included), a CUM + ENDS report every 256 frames / 20 s",
              sr::native::g_bracketsOn ? 1 : 0, gate("SR_PM4_COVERAGE"), gate("SR_NATIVE_BRACKETS"),
              gate("SR_NATIVE_RECORD"), gate("SR_NATIVE_DRAW"));
}

// Coverage map (brackets on): owner x opKey.
int g_owners = 0;              // kEntryCount + 2
std::vector<uint64_t> g_cov;   // g_owners * kOpKeys
std::vector<uint64_t> g_covDw;  // g_owners: packet dwords (header included) - P0 producer census
std::vector<uint64_t> g_covKind;  // 8 kinds * kOpKeys
std::vector<uint64_t> g_covInternal;  // kOpKeys (bracket opened from inside the XDK)
inline int ownerUnstamped() { return sr::native::kEntryCount; }
inline int ownerRingUnstamped() { return sr::native::kEntryCount + 1; }
uint64_t g_unstampedLogged = 0, g_snapshotOverflow = 0;

struct Win {
  std::vector<sr::native::Interval> iv;
  size_t n = 0, cur = 0;
  bool valid = false;
  const uint8_t* host = nullptr;  // the window's first byte as the engine reads it
  uint32_t phys = 0;              // its stamp key: guest memory read in place, or the copy's origin
};
Win g_win[kMaxDepth];
bool g_ringPacket = false;
// The next depth-0 window is a host COPY of guest command bytes (a segmentless segment's gap part, native_flush
// cpSegment): its stamp key is the guest bytes' (`g_copyPhys`), its owners the stamps taken with the copy.
bool g_copyPending = false;
const uint8_t* g_copyHost = nullptr;  // the window it was set for: any other depth-0 window drops it unused
uint32_t g_copyPhys = 0;
const sr::native::Interval* g_copyIv = nullptr;
size_t g_copyN = 0;
// By the innermost scope (Stamp::inner): owner x opKey, and dwords.
std::vector<uint64_t> g_covInner, g_covInnerDw;
uint32_t g_ringIndex = 0, g_ringDwords = 0;

void ensureCov() {
  if (!g_owners) {
    g_owners = sr::native::kEntryCount + 2;
    g_cov.assign(size_t(g_owners) * kOpKeys, 0);
    g_covKind.assign(size_t(8) * kOpKeys, 0);
    g_covInternal.assign(kOpKeys, 0);
    g_covDw.assign(size_t(g_owners), 0);
    g_covInner.assign(size_t(g_owners) * kOpKeys, 0);
    g_covInnerDw.assign(size_t(g_owners), 0);
  }
}

// Physical address of a host pointer into guest memory (the stamp key of
// kernel/native_brackets.cpp hostOff: alias-resolved offset & 0x1FFFFFFF).
uint32_t hostOffOf(const uint8_t* p) {
  const uint8_t* b = sr::kernel::getGuestBase();
  if (!b || p < b) return 0;
  const uint64_t d = uint64_t(p - b);
  return d <= 0xFFFFFFFFull ? uint32_t(d) & 0x1FFFFFFFu : 0u;
}

const char* ownerName(int o, uint32_t* va) {
  if (o >= 0 && o < sr::native::kEntryCount) {
    *va = sr::native::kEntries[o].va;
    return sr::native::kEntries[o].name;
  }
  *va = 0;
  return o == ownerRingUnstamped() ? "RING_UNSTAMPED" : "UNSTAMPED";
}

// `key` = the packet's stamp key (a copy's: its guest origin), 0 = none (the host address is used).
void logUnstamped(const uint8_t* at, int depth, int opKey, uint32_t hdr, bool ring, uint32_t key) {
  const uint64_t n = ++g_unstampedLogged;
  if (n > 48 && (n & (n - 1)) != 0) return;
  if (ring) {
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[PM4_COV_UNSTAMPED] #%llu frame=%llu RING idx=%u hdr=%08X key=%d",
                (unsigned long long)n, (unsigned long long)g_frames, g_ringIndex, hdr, opKey);
    return;
  }
  const uint32_t off = key ? key : hostOffOf(at);
  sr::native::Interval prev{}, next{};
  sr::native::neighbours(off, &prev, &next);
  uint32_t pva = 0, nva = 0;
  const char* pn = prev.s.entry >= 0 ? ownerName(prev.s.entry, &pva) : "-";
  const char* nn = next.s.entry >= 0 ? ownerName(next.s.entry, &nva) : "-";
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PM4_COV_UNSTAMPED] #%llu frame=%llu depth=%d off=%08X hdr=%08X key=%d | prev=[%08X,%08X) "
              "%s kind=%u lr=%08X gap=%d | next=[%08X,..) %s lr=%08X",
              (unsigned long long)n, (unsigned long long)g_frames, depth, off, hdr, opKey, prev.lo,
              prev.hi, pn, prev.s.kind, prev.s.lr, int(off - prev.hi), next.lo, nn, next.s.lr);
}

// Owner lookup + coverage count for one packet (brackets on).
// ---- NATIVE step 3 sizing: CP time by packet class (CP thread; rdtsc between packets) ----
// 0 = packets inside a DRAW entry's bracket other than the DRAW itself (register / constant
//     writes the native projection already carries: what skipping PM4 parsing would save),
// 1 = the DRAW packets, 2 = packets of other brackets (Clear, Resolve, Swap, emitters),
// 3 = packets with no bracket. Idle gaps between ring windows are not counted.
uint64_t g_cpCyc[4] = {}, g_cpPk[4] = {};
uint64_t g_cpEntryCyc[1025] = {};  // class 2 cycles by bracket entry (1024 = out of range)
int g_cpLastEntry = 1024;
int g_cpLastOp = 0;
uint64_t g_cpOpCyc[768] = {};  // class 2 cycles by packet key (type-3 op, 256+type-0 page, 512+other)
uint32_t g_cpSlowLogged = 0;
double cpTicksPerMs() {
  static const double v = [] {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    const uint64_t t0 = __rdtsc();
    Sleep(20);
    QueryPerformanceCounter(&b);
    const uint64_t t1 = __rdtsc();
    return double(t1 - t0) / (double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart));
  }();
  return v;
}
uint64_t g_cpLastTsc = 0;
int g_cpLastCls = -1;
bool entryIsDraw(int entry) {
  static int8_t cache[1024];
  static bool init = false;
  if (!init) {
    std::memset(cache, -1, sizeof(cache));
    init = true;
  }
  if (entry < 0 || entry >= 1024 || entry >= sr::native::kEntryCount) return false;
  if (cache[entry] < 0) {
    const char* n = sr::native::kEntries[entry].name;
    cache[entry] = (n && (std::strstr(n, "Draw") || std::strstr(n, "Vertices"))) ? 1 : 0;
  }
  return cache[entry] == 1;
}
void cpTimeMark(const sr::native::Stamp& s, int opKey) {
  const uint64_t now = __rdtsc();
  if (g_cpLastCls >= 0) {
    g_cpCyc[g_cpLastCls] += now - g_cpLastTsc;
    if (g_cpLastCls == 2) {
      const uint64_t dt = now - g_cpLastTsc;
      g_cpEntryCyc[g_cpLastEntry] += dt;
      g_cpOpCyc[g_cpLastOp] += dt;
      // The slow ones: a packet of another bracket that held the CP > 4 ms (what it waited on).
      if (g_cpSlowLogged < 64 && dt > uint64_t(4.0 * cpTicksPerMs())) {
        g_cpSlowLogged++;
        SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_CPTIME] slow packet %.2f ms: entry %s key 0x%03X",
                    double(dt) / cpTicksPerMs(),
                    g_cpLastEntry < sr::native::kEntryCount ? sr::native::kEntries[g_cpLastEntry].name : "?",
                    g_cpLastOp);
      }
    }
  }
  g_cpLastOp = (opKey >= 0 && opKey < 768) ? opKey : 0;
  g_cpLastEntry = (s.entry >= 0 && s.entry < 1024) ? s.entry : 1024;
  int cls = 3;
  if (s.callId) {
    if (opKey == 0x22 || opKey == 0x36) {
      cls = 1;
    } else {
      cls = entryIsDraw(s.entry) ? 0 : 2;
    }
  }
  g_cpPk[cls]++;
  g_cpLastCls = cls;
  g_cpLastTsc = now;
}

sr::native::Stamp classifyImpl(const uint8_t* at, int depth, int opKey, uint32_t hdr) {
  ensureCov();
  sr::native::Stamp s;
  bool ring = false;
  uint32_t key = 0;
  if (depth == 0 && g_ringPacket) {
    s = sr::native::takeRingStamp(g_ringIndex);
    ring = true;
  } else if (depth < kMaxDepth && g_win[depth].valid) {
    Win& w = g_win[depth];
    // the packet's stamp key: the window's key plus its offset in the window (a copy's key is the guest bytes')
    const uint32_t off = w.phys + uint32_t(at - w.host);
    key = off;
    while (w.cur < w.n && w.iv[w.cur].hi <= off) w.cur++;
    if (w.cur < w.n && w.iv[w.cur].lo <= off) s = w.iv[w.cur].s;
  }
  int owner = s.entry;
  if (owner < 0 || owner >= sr::native::kEntryCount) {
    owner = ring ? ownerRingUnstamped() : ownerUnstamped();
    g_cur.unstamped++;
    if (s.flags & sr::native::kStampChain) g_cur.chain++;  // a chain node's bytes: unstamped, counted apart
    logUnstamped(at, depth, opKey, hdr, ring, key);
  } else {
    g_cur.accounted++;
    g_covKind[size_t(s.kind & 7) * kOpKeys + opKey]++;
    if (s.flags & sr::native::kStampInternalOrigin) g_covInternal[opKey]++;
  }
  g_cov[size_t(owner) * kOpKeys + opKey]++;
  // dwords of this packet (type-0 / type-3: header + count field + 1; type-2: 1; type-1: 3)
  const uint32_t ty = hdr >> 30;
  const uint32_t dw = ty == 2u ? 1u : ty == 1u ? 3u : 2u + ((hdr >> 16) & 0x3FFFu);
  if (size_t(owner) < g_covDw.size()) g_covDw[size_t(owner)] += dw;
  // the producer: the innermost scope that wrote the bytes (UNSTAMPED alike when no stamp)
  int inner = owner < sr::native::kEntryCount ? s.inner : owner;
  if (inner < 0 || inner >= sr::native::kEntryCount) inner = owner < sr::native::kEntryCount ? owner : inner;
  g_covInner[size_t(inner) * kOpKeys + opKey]++;
  g_covInnerDw[size_t(inner)] += dw;
  return s;
}

std::string opsList(const Counters& c) {
  std::string s;
  char b[32];
  for (int i = 0; i < 256; i++) {
    const uint64_t n = c.t3Pk[i] + c.t3Pred[i];
    if (!n) continue;
    std::snprintf(b, sizeof(b), "%s%02X:%llu", s.empty() ? "" : ",", i, (unsigned long long)n);
    s += b;
  }
  return s;
}

std::string regsList(const Counters& c) {
  std::string s;
  char b[48];
  for (int i = 0; i < 256; i++) {
    if (!c.t0Pk[i]) continue;
    std::snprintf(b, sizeof(b), "%s%02X:%llu/%llu", s.empty() ? "" : ",", i,
                  (unsigned long long)c.t0Pk[i], (unsigned long long)c.t0Dw[i]);
    s += b;
  }
  return s;
}

// The F line of the frame just ended (or of the terminal snapshot's tail, terminal()): prints g_cur, folds it into
// g_cum and clears it.
void frameLine() {
  g_frames++;
  const uint64_t pk = g_cur.packets();
  g_pkMax = std::max(g_pkMax, pk);
  g_pkMin = std::min(g_pkMin, pk);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PM4_COV] F frame=%llu pk=%llu t0=%llu t0dw=%llu t3=%llu t3dw=%llu pred=%llu "
              "ringIb=%llu zero=%llu trunc=%llu d0=%llu dN=%llu acc=%llu unst=%llu ops=%s regs=%s",
              (unsigned long long)g_frames, (unsigned long long)pk,
              (unsigned long long)g_cur.t0(), (unsigned long long)g_cur.t0dw(),
              (unsigned long long)g_cur.t3(), (unsigned long long)g_cur.t3dw(),
              (unsigned long long)g_cur.pred(), (unsigned long long)g_cur.ringIb,
              (unsigned long long)g_cur.other[kZeroDword], (unsigned long long)g_cur.other[kTruncated],
              (unsigned long long)g_cur.depth0, (unsigned long long)g_cur.depthN,
              (unsigned long long)g_cur.accounted, (unsigned long long)g_cur.unstamped,
              opsList(g_cur).c_str(), regsList(g_cur).c_str());
  g_cum.add(g_cur);
  g_cur.clear();
}

void frameEnd() {
  frameLine();
  const auto now = std::chrono::steady_clock::now();
  if (g_frames % 256 == 0 || now - g_lastReport > std::chrono::seconds(20)) report("periodic");
}

bool anyCount(const Counters& c) {
  const uint64_t* w = reinterpret_cast<const uint64_t*>(&c);
  for (size_t i = 0; i < sizeof(Counters) / 8; i++)
    if (w[i]) return true;
  return false;
}

}  // namespace

sr::native::Stamp classify(const uint8_t* at, int depth, int opKey, uint32_t hdr) {
  const sr::native::Stamp s = classifyImpl(at, depth, opKey, hdr);
  cpTimeMark(s, opKey);
  return s;
}

void windowBegin(const uint8_t* host, size_t bytes, int depth) {
  armed();
  if (depth == 0) g_cpLastCls = -1;  // a new ring window: the wait before it is not CP work
  if (!sr::native::g_bracketsOn || depth < 0 || depth >= kMaxDepth) return;
  Win& w = g_win[depth];
  w.valid = false;
  w.n = w.cur = 0;
  w.host = host;
  const bool copy = depth == 0 && g_copyPending && host == g_copyHost;
  if (depth == 0) g_copyPending = false;  // set for one window: the next depth-0 one, if it is that copy
  if (copy) {  // a segment's gap copy: owners by the stamps taken with the copy
    const uint32_t lo = g_copyPhys, hi = uint32_t(std::min<uint64_t>(uint64_t(lo) + bytes, 0xFFFFFFFFull));
    w.phys = lo;
    if (!lo) return;
    if (w.iv.size() < 65536) w.iv.resize(65536);
    for (size_t i = 0; i < g_copyN && w.n < w.iv.size(); i++)
      if (g_copyIv[i].hi > lo && g_copyIv[i].lo < hi) w.iv[w.n++] = g_copyIv[i];
    w.valid = true;
    return;
  }
  if (depth == 0 && g_ringPacket) return;  // ring packets are looked up by ring index
  const uint32_t lo = hostOffOf(host);
  w.phys = lo;
  if (!lo) return;
  if (w.iv.size() < 65536) w.iv.resize(65536);
  w.n = sr::native::snapshotIntervals(lo, uint32_t(std::min<uint64_t>(uint64_t(lo) + bytes, 0xFFFFFFFFull)),
                                      w.iv.data(), w.iv.size());
  if (w.n == w.iv.size()) g_snapshotOverflow++;
  w.valid = true;
}

void setCopyWindow(const uint8_t* host, uint32_t phys, const sr::native::Interval* iv, size_t n) {
  g_copyPending = true;
  g_copyHost = host;
  g_copyPhys = phys;
  g_copyIv = iv;
  g_copyN = n;
}

uint64_t ownerPackets(int owner, int opKey, bool inner) {
  if (!g_owners || owner < 0 || owner >= g_owners || opKey < 0 || opKey >= kOpKeys) return 0;
  return (inner ? g_covInner : g_cov)[size_t(owner) * kOpKeys + size_t(opKey)];
}
uint64_t unstampedPackets() { return g_cum.unstamped + g_cur.unstamped; }
uint64_t chainPackets() { return g_cum.chain + g_cur.chain; }
uint64_t accountedPackets() { return g_cum.accounted + g_cur.accounted; }

bool packet0(const uint8_t* at, int depth, uint32_t startReg, uint32_t count, bool oneReg) {
  g_swapPacketEnded = false;
  const uint32_t pg0 = (startReg >> 8) & 0xFFu;
  g_cur.t0Pk[pg0]++;
  if (oneReg) {
    g_cur.t0Dw[pg0] += count;
  } else {
    uint32_t r = startReg & 0xFFFFu, left = count;
    while (left) {
      const uint32_t pg = (r >> 8) & 0xFFu;
      const uint32_t room = ((pg + 1u) << 8) - (r & 0xFFFFu);
      const uint32_t n = left < room ? left : room;
      g_cur.t0Dw[pg] += n;
      r += n;
      left -= n;
    }
  }
  (depth == 0 ? g_cur.depth0 : g_cur.depthN)++;
  if (sr::native::g_bracketsOn) {
    const sr::native::Stamp st = classify(at, depth, 256 + int(pg0), be32(at));
    if (sr::native::dev::on() && startReg + count > 0x4000u && startReg < 0x4800u)
      sr::native::dev::noteAluWrite(st.entry, startReg, oneReg ? 1u : count);
    if (sr::native::dev::on() && !oneReg)
      return sr::native::dev::skipState(st.callId, entryIsDraw(st.entry), startReg, count);
  }
  return false;
}

namespace {
sr::native::dev::CpShaders g_drawSh;  // CP thread
}  // namespace
void setDrawShaders(uint64_t vs, uint64_t ps, uint32_t vsUse, uint32_t psUse, uint32_t vsAddr,
                    uint32_t vsDwords) {
  g_drawSh = sr::native::dev::CpShaders{vs, ps, vsUse, psUse, vsAddr, vsDwords};
}

void afterDraw() {
  if (sr::native::dev::on()) sr::native::dev::afterDraw();
}

bool packet3(const uint8_t* at, int depth, uint32_t op, uint32_t count, bool predSkipped) {
  g_swapPacketEnded = false;
  bool skip = false;
  op &= 0xFFu;
  if (predSkipped) {
    g_cur.t3Pred[op]++;
  } else {
    g_cur.t3Pk[op]++;
    g_cur.t3Dw[op] += count;
    if (op == 0x2Du && count >= 1) g_cur.setConst[(be32(at + 4) >> 16) & 7u]++;
    if (op == 0x2Fu && count >= 2) g_cur.loadConst[(be32(at + 8) >> 16) & 7u]++;
  }
  (depth == 0 ? g_cur.depth0 : g_cur.depthN)++;
  if (sr::native::g_bracketsOn) {
    const sr::native::Stamp s = classify(at, depth, int(op), be32(at));
    // NATIVE N1 mode A: the native projection recorded for this draw's bracket vs
    // the CP register file now (gfx/native_device, SR_NATIVE_RECORD).
    if (!predSkipped && (op == 0x22u || op == 0x36u) && sr::native::dev::on())
      sr::native::dev::checkDraw(s.callId, s.entry, op, g_drawSh);
    if (predSkipped && (op == 0x22u || op == 0x36u) && sr::native::dev::on())
      sr::native::dev::noteDrawSkipped(s.callId);
    if (!predSkipped && sr::native::dev::on()) {
      if (op == 0x2Du && count >= 2 && ((be32(at + 4) >> 16) & 0xFFu) == 0u)
        sr::native::dev::noteAluWrite(s.entry, 0x4000u + (be32(at + 4) & 0x7FFu), count - 1u);
      if (op == 0x2Fu && count >= 3 && ((be32(at + 8) >> 16) & 0xFFu) == 0u)
        sr::native::dev::noteAluWrite(s.entry, 0x4000u + (be32(at + 8) & 0x7FFu), be32(at + 12));
    }
    // NATIVE step 3: SET_CONSTANT / LOAD_ALU_CONSTANT into the carried files (ALU, fetch, bool).
    if (!predSkipped && sr::native::dev::on() && (op == 0x2Du || op == 0x2Fu)) {
      const uint32_t info = be32(at + (op == 0x2Du ? 4 : 8));
      const uint32_t type = (info >> 16) & 0xFFu, offs = info & 0x7FFu;
      const uint32_t n = op == 0x2Du ? (count >= 2 ? count - 1u : 0u) : (count >= 3 ? be32(at + 12) : 0u);
      const uint32_t baseReg = type == 0 ? 0x4000u : type == 1 ? 0x4800u : type == 2 ? 0x4900u : 0u;
      if (n && baseReg) skip = sr::native::dev::skipState(s.callId, entryIsDraw(s.entry), baseReg + offs, n);
    }
  }
  if (op == 0x64u && !predSkipped) {
    // XE_SWAP: the frame ends here, before the packet runs; its present (Pm4Engine::applySwap -> presented(), next,
    // with no packet between) does not end another.
    g_endsXeSwap++;
    g_swapPacketEnded = true;
    frameEnd();
  }
  return skip;
}

void presented() {
  armed();
  g_presents++;
  if (g_swapPacketEnded) {  // the XE_SWAP packet's own present: packet3 ended its frame
    g_swapPacketEnded = false;
    return;
  }
  // A present with no 0x64 packet before it (SR_NATIVE_SWAP_BLOCK's kRkSwap record): it ends the frame.
  g_endsPresent++;
  frameEnd();
}

void swapWithoutPresent() { g_swapPacketEnded = false; }

uint64_t framesEnded() { return g_frames; }
uint64_t presentsApplied() { return g_presents; }
uint64_t presentEnds() { return g_endsPresent; }

void packetOther(const uint8_t* at, int depth, OtherKind k, uint32_t next) {
  g_swapPacketEnded = false;
  if (k == kType2 && (be32(at) >> 30) == 1u) k = kType1Skip;
  g_cur.other[k]++;
  if (k == kType1 || k == kType2 || k == kType1Skip || k == kChainTail) {
    (depth == 0 ? g_cur.depth0 : g_cur.depthN)++;
    // 821075E0's alignment pad, witnessed by the run header right after it (pm4_coverage.hpp kConstRunPadKey)
    const int key = k == kType2 && constRunHeader(next) ? kConstRunPadKey : int(k);
    if (sr::native::g_bracketsOn) classify(at, depth, 512 + key, be32(at));
  }
}

void setRingPacket(uint32_t ringIndex, uint32_t dwords) {
  g_ringPacket = true;
  g_ringIndex = ringIndex;
  g_ringDwords = dwords;
}

void clearRingPacket() {
  if (g_ringPacket && sr::native::g_bracketsOn) sr::native::clearRingStamps(g_ringIndex, g_ringDwords);
  g_ringPacket = false;
}

void ringIndirect(uint32_t ringIndex, uint32_t dwords) {
  armed();
  g_swapPacketEnded = false;
  g_cur.t3Pk[0x3F]++;
  g_cur.t3Dw[0x3F] += dwords > 0 ? dwords - 1u : 0u;
  g_cur.ringIb++;
  g_cur.depth0++;
  if (sr::native::g_bracketsOn) {
    setRingPacket(ringIndex, dwords);
    classify(nullptr, 0, 0x3F, 0xC0013F00u);
    clearRingPacket();
  }
}

void report(const char* why) {
  {
    const uint64_t tot = g_cpCyc[0] + g_cpCyc[1] + g_cpCyc[2] + g_cpCyc[3];
    if (tot)
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[NATIVE_CPTIME] %s CP packet time by class: draw-bracket state %.1f%% (%llu pk), DRAW %.1f%% "
                  "(%llu pk), other brackets %.1f%% (%llu pk), no bracket %.1f%% (%llu pk); %.2f Gcycles",
                  why, 100.0 * g_cpCyc[0] / tot, (unsigned long long)g_cpPk[0], 100.0 * g_cpCyc[1] / tot,
                  (unsigned long long)g_cpPk[1], 100.0 * g_cpCyc[2] / tot, (unsigned long long)g_cpPk[2],
                  100.0 * g_cpCyc[3] / tot, (unsigned long long)g_cpPk[3], double(tot) / 1e9);
    std::string top;
    bool used[1025] = {};
    for (int k = 0; k < 6; k++) {
      int best = -1;
      for (int e = 0; e < 1025; e++)
        if (!used[e] && g_cpEntryCyc[e] && (best < 0 || g_cpEntryCyc[e] > g_cpEntryCyc[best])) best = e;
      if (best < 0) break;
      used[best] = true;
      char buf[160];
      std::snprintf(buf, sizeof(buf), "%s%s %.1f%%", top.empty() ? "" : ", ",
                    best < sr::native::kEntryCount ? sr::native::kEntries[best].name : "?",
                    tot ? 100.0 * g_cpEntryCyc[best] / tot : 0.0);
      top += buf;
    }
    if (tot) SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_CPTIME] %s other-bracket time by entry: %s", why, top.c_str());
    std::string ops;
    bool usedOp[768] = {};
    for (int k = 0; k < 8; k++) {
      int best = -1;
      for (int o = 0; o < 768; o++)
        if (!usedOp[o] && g_cpOpCyc[o] && (best < 0 || g_cpOpCyc[o] > g_cpOpCyc[best])) best = o;
      if (best < 0) break;
      usedOp[best] = true;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%s0x%03X %.1f%%", ops.empty() ? "" : ", ", best, tot ? 100.0 * g_cpOpCyc[best] / tot : 0.0);
      ops += buf;
    }
    if (tot) SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_CPTIME] %s other-bracket time by packet (type-3 op / 0x1xx type-0 page / 0x2xx other): %s", why, ops.c_str());
  }
  if (!g_on) return;
  g_lastReport = std::chrono::steady_clock::now();
  Counters all = g_cum;
  all.add(g_cur);
  const uint64_t pk = all.packets();
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[PM4_COV] CUM %s frames=%llu pk=%llu pk/f mean=%.1f min=%llu max=%llu t0=%llu t0dw=%llu "
              "t3=%llu t3dw=%llu pred=%llu ringIb=%llu zero=%llu trunc=%llu chainTail=%llu t1=%llu t2=%llu t1skip=%llu "
              "acc=%llu unst=%llu chain=%llu setConst=%llu/%llu/%llu/%llu/%llu loadConst=%llu/%llu/%llu/%llu "
              "snapOverflow=%llu ops=%s regs=%s",
              why, (unsigned long long)g_frames, (unsigned long long)pk,
              g_frames ? double(g_cum.packets()) / double(g_frames) : 0.0,
              (unsigned long long)(g_frames ? g_pkMin : 0), (unsigned long long)g_pkMax,
              (unsigned long long)all.t0(), (unsigned long long)all.t0dw(),
              (unsigned long long)all.t3(), (unsigned long long)all.t3dw(),
              (unsigned long long)all.pred(), (unsigned long long)all.ringIb,
              (unsigned long long)all.other[kZeroDword], (unsigned long long)all.other[kTruncated],
              (unsigned long long)all.other[kChainTail], (unsigned long long)all.other[kType1],
              (unsigned long long)all.other[kType2], (unsigned long long)all.other[kType1Skip],
              (unsigned long long)all.accounted,
              (unsigned long long)all.unstamped, (unsigned long long)all.chain, (unsigned long long)all.setConst[0],
              (unsigned long long)all.setConst[1], (unsigned long long)all.setConst[2],
              (unsigned long long)all.setConst[3], (unsigned long long)all.setConst[4],
              (unsigned long long)all.loadConst[0], (unsigned long long)all.loadConst[1],
              (unsigned long long)all.loadConst[2], (unsigned long long)all.loadConst[3],
              (unsigned long long)g_snapshotOverflow, opsList(all).c_str(), regsList(all).c_str());
  // How the frames above ended: xeSwap = at an XE_SWAP packet, present = at a present the CP applied with no packet
  // (a native swap record); presents = every present the CP applied while counting (live: frames = xeSwap + present,
  // presents = frames unless an XE_SWAP without its 'SWAP' signature applied none; replay: presents 0).
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_COV] ENDS %s frames=%llu xeSwap=%llu present=%llu presents=%llu", why,
              (unsigned long long)g_frames, (unsigned long long)g_endsXeSwap, (unsigned long long)g_endsPresent,
              (unsigned long long)g_presents);
  sr::native::dev::report(why);  // NATIVE N1 mode A (no-op unless SR_NATIVE_RECORD=1)
  if (!sr::native::g_bracketsOn) return;
  // The producer lines below, internalOrigin always among them, print with every report while the producer census is
  // on - also before (or without) a first classified packet: a census of zero packets, never a missing one.
  ensureCov();
  // Per owner: every non-zero opKey (t3 op as XX, type-0 page as rXX, other as oN).
  for (int o = 0; o < g_owners; o++) {
    std::string s;
    uint64_t tot = 0;
    char b[40];
    for (int k = 0; k < kOpKeys; k++) {
      const uint64_t n = g_cov[size_t(o) * kOpKeys + k];
      if (!n) continue;
      tot += n;
      if (k < 256) std::snprintf(b, sizeof(b), "%s%02X:%llu", s.empty() ? "" : ",", k, (unsigned long long)n);
      else if (k < 512) std::snprintf(b, sizeof(b), "%sr%02X:%llu", s.empty() ? "" : ",", k - 256, (unsigned long long)n);
      else std::snprintf(b, sizeof(b), "%so%d:%llu", s.empty() ? "" : ",", k - 512, (unsigned long long)n);
      s += b;
    }
    if (!tot) continue;
    uint32_t va = 0;
    const char* nm = ownerName(o, &va);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_COVMAP] %s owner=%08X name=%s pk=%llu dw=%llu keys=%s", why, va, nm,
                (unsigned long long)tot, (unsigned long long)(size_t(o) < g_covDw.size() ? g_covDw[size_t(o)] : 0ull),
                s.c_str());
  }
  // Per producer (the innermost scope that wrote the bytes, Stamp::inner): the same keys. The owner lines above
  // stay the outermost call's (a lock under a draw's bracket is the draw's there, the lock's here).
  for (int o = 0; o < g_owners && size_t(o) * kOpKeys < g_covInner.size(); o++) {
    std::string s;
    uint64_t tot = 0;
    char b[40];
    for (int k = 0; k < kOpKeys; k++) {
      const uint64_t n = g_covInner[size_t(o) * kOpKeys + k];
      if (!n) continue;
      tot += n;
      if (k < 256) std::snprintf(b, sizeof(b), "%s%02X:%llu", s.empty() ? "" : ",", k, (unsigned long long)n);
      else if (k < 512) std::snprintf(b, sizeof(b), "%sr%02X:%llu", s.empty() ? "" : ",", k - 256, (unsigned long long)n);
      else std::snprintf(b, sizeof(b), "%so%d:%llu", s.empty() ? "" : ",", k - 512, (unsigned long long)n);
      s += b;
    }
    if (!tot) continue;
    uint32_t va = 0;
    const char* nm = ownerName(o, &va);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_COVMAP] %s inner=%08X name=%s pk=%llu dw=%llu keys=%s", why, va, nm,
                (unsigned long long)tot,
                (unsigned long long)(size_t(o) < g_covInnerDw.size() ? g_covInnerDw[size_t(o)] : 0ull), s.c_str());
  }
  static const char* kKindNames[8] = {"entry", "table", "flush", "switch", "wait", "ringwrite", "internal", "block"};
  for (int kd = 0; kd < 8; kd++) {
    uint64_t tot = 0;
    for (int k = 0; k < kOpKeys; k++) tot += g_covKind[size_t(kd) * kOpKeys + k];
    if (tot)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_COVMAP] %s kind=%s pk=%llu", why, kKindNames[kd],
                  (unsigned long long)tot);
  }
  uint64_t io = 0;
  for (int k = 0; k < kOpKeys; k++) io += g_covInternal[k];
  SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_COVMAP] %s internalOrigin pk=%llu", why, (unsigned long long)io);
  sr::native::bracketsCensus(why);
}

int terminal(const char* why) {
  int tail = -1;
  if (g_on) {
    tail = 0;
    if (anyCount(g_cur)) {
      // The drain ended inside a frame (segments are kicked several times a frame): that partial frame is printed as
      // the next F line, so the F lines and the CUM below reconcile and none of its packets is dropped.
      SR_LOG_BOOT(sr::LogSeverity::Info,
                  "[PM4_COV] %s tail frame=%llu pk=%llu (the drained work after the last XE_SWAP; its F line "
                  "follows and the CUM frames= below counts it)",
                  why, (unsigned long long)(g_frames + 1), (unsigned long long)g_cur.packets());
      frameLine();
      tail = 1;
    }
    // The counter's witness again, in the snapshot (tools/perf/native_proof.py terminal_select: the armed item's
    // twin) - AFTER the tail fold: kernel/quit.cpp's verify re-renders every family and compares each integer field, and
    // the re-render finds no tail to fold, so a line printed before the fold read frames=N in the snapshot and N+1 in
    // the re-render (tbk2: both terminal snapshots FAILED "counters-moved-after-snapshot | [PM4_COV] frames(4648->4649)").
    // Every line here is printed from the folded state, so both renders print the same fields.
    SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_COV] armed %s brackets=%d frames=%llu presents=%llu", why,
                sr::native::g_bracketsOn ? 1 : 0, (unsigned long long)g_frames, (unsigned long long)g_presents);
  }
  report(why);
  return tail;
}

}  // namespace sr::gfx::pm4cov
