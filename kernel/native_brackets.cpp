// NATIVE ROADMAP N0.0 / N0.3 runtime (see native_brackets.hpp for the contract).
//
// Observe-only. Every read below is of guest memory the XDK itself maintains
// (VdGlobalDevice cell -> device, the command cursor dev+0x28/0x2C, the ring
// write index dev+10820, ring mask dev+13480); nothing is written to the guest.

#include "native_brackets.hpp"

#include "guest_context.hpp"
#include "../app/log.hpp"

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "guest/ppc/ppc_recomp_shared.h"  // PPCContext: the GpuCmdBufAllocate hooks' r3 / r4 (the stamp lifetime)

extern "C" sr::kernel::GuestThreadSlot g_debugThreadRegistry[64];

namespace sr::native {

namespace {
bool envOn(const char* n) {
  const char* e = std::getenv(n);
  return e && e[0] == '1';
}
}  // namespace

bool g_frameSplitOn = envOn("SR_NATIVE_FRAMESPLIT");
// SR_NATIVE_RECORD (gfx/native_device, N1 mode A) joins draws through the brackets.
bool g_bracketsOn = envOn("SR_NATIVE_BRACKETS") || envOn("SR_NATIVE_RECORD") || envOn("SR_NATIVE_DRAW");
bool g_anyOn = g_frameSplitOn || g_bracketsOn;

int entryIndex(uint32_t va) {
  int lo = 0, hi = kEntryCount - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    if (kEntries[mid].va == va) return mid;
    if (kEntries[mid].va < va) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
}

int entryIndexSym(const char* sym) {
  if (!sym || std::strncmp(sym, "sub_", 4) != 0) return -1;
  return entryIndex(uint32_t(std::strtoul(sym + 4, nullptr, 16)));
}

// iter-467 native-a P2 trace: per-entry observe hooks (kernel/p2_trace.cpp registers them at load,
// before the first guest call; the table is never changed afterwards).
bool g_hooksOn = false;
namespace {
constexpr int kHookCap = 1024;
ScopeHook g_hookBefore[kHookCap];
ScopeHook g_hookAfter[kHookCap];
}  // namespace

bool setScopeHook(uint32_t va, ScopeHook before, ScopeHook after) {
  const int i = entryIndex(va);
  if (i < 0 || i >= kHookCap) return false;
  g_hookBefore[i] = before;
  g_hookAfter[i] = after;
  g_hooksOn = true;
  return true;
}

void hookEnter(int idx, void* ctx) {
  if (idx < kHookCap && g_hookBefore[idx]) g_hookBefore[idx](ctx);
}

void hookLeave(int idx, void* ctx) {
  if (idx < kHookCap && g_hookAfter[idx]) g_hookAfter[idx](ctx);
}

namespace {

constexpr uint32_t kXdkLo = 0x820F0048u, kXdkHi = 0x821162E8u;
constexpr uint32_t kVdGlobalDeviceCell = 0x82000948u;  // ENGINE_MAP: dev = *(*cell)
constexpr uint32_t kDevCursor = 40, kDevLimit = 44;    // dev+0x28 / +0x2C
constexpr uint32_t kDevRingWptr = 10820, kDevRingMask = 13480;  // gpu_ring_doorbell.cpp
constexpr uint32_t kLimitSlack = 0x400;                // tail reserve past dev+0x2C
constexpr uint32_t kMaxSub = 0x400000;                 // 4 MB sanity bound per piece
constexpr size_t kRingCap = 0x10000;                   // dwords (mask <= 0xFFFF)

// ---------------------------------------------------------------- clocks
struct Clock {
  uint64_t tsc0 = 0;
  std::chrono::steady_clock::time_point t0;
  Clock() : tsc0(__rdtsc()), t0(std::chrono::steady_clock::now()) {}
  double ticksPerMs() const {
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    const double ticks = double(__rdtsc() - tsc0);
    return ms > 1.0 ? ticks / ms : 3.0e6;  // fallback ~3 GHz until calibrated
  }
};
Clock& clock() {
  static Clock c;
  return c;
}

// ---------------------------------------------------------------- per thread
struct FrameRec {
  float frameMs, xdkMs, waitMs, impInMs, impOutMs;
  uint32_t outerCalls, calls;
};

struct ThreadStats;  // fwd

struct Tls {
  ThreadStats* st;          // registered on first use
  uint32_t depth;           // XDK scope depth
  uint32_t waitDepth;
  // Frame-split accumulators (ticks) since this thread's last present.
  uint64_t xdk, wait, impIn, impOut;
  uint32_t outerCalls, calls;
  uint64_t lastPresentTsc;
  // Bracket state.
  bool open;
  int16_t entry;
  uint8_t flags;
  uint32_t lr;
  uint32_t callId;
  uint32_t dev;
  uint32_t subStart, subLimit;  // host offsets (guest-base relative)
  uint8_t kindStack[16];
  int kindTop;
  // The innermost table scope inside the open bracket (Stamp::inner): [0] the bracket's own entry, then every nested
  // table scope (an entry, a setter, the flush, the switch, a wait, the ring writer) pushes itself; a piece is cut at
  // each nested enter and leave so its bytes carry the scope that wrote them.
  int16_t innerStack[32];
  int innerTop;
  // Device ownership (the SDK's own owner test, 82468EF8: dev+0x2A08 against KTHREAD+332). myId = this thread's id
  // (KTHREAD (KPCR+0x100) +332, read at the bracket's open; 0 = unknown); owned = the device was positively this
  // thread's at the last cut. A piece is stamped only when it was at BOTH of its cuts: a thread that does not own the
  // device (a loader's lock merges a range, the pump writes the ring only), an unreadable identity and the unowned
  // device (dev+0x2A08 == 0, D3DDevice_ReleaseThreadOwnership) stamp nothing, and a cut that finds another owner
  // (an Acquire on another thread) drops the piece, so one bracket never spans another owner's writes. epoch = the
  // ownership epoch (g_ownerEpoch) read at the last cut: a piece is stamped only if it is unchanged at the next one
  // (an ownership change and change-back between two cuts - A -> B -> A - reads A at both).
  uint32_t myId;
  bool owned;
  uint64_t epoch;
};
thread_local Tls t_tls;  // zero-initialised POD: no TLS guard on access

struct ThreadStats {
  uint32_t tid = 0;
  char name[32] = {};
  // Whole-run totals (ticks), updated by the owning thread only.
  std::atomic<uint64_t> xdk{0}, wait{0}, impIn{0}, impOut{0}, calls{0}, outer{0};
  // Presenting threads only.
  std::mutex fmu;
  std::vector<FrameRec> frames;
  size_t winStart = 0;
  uint64_t winStartTsc = 0;
  uint64_t lastCumTsc = 0;
};

std::mutex g_thrMu;
std::vector<ThreadStats*> g_threads;

// Per-thread totals have ONE writer (the owning thread): a relaxed load+store
// pair is a plain add, not a locked RMW, on the hot path of every XDK call.
inline void bump(std::atomic<uint64_t>& a, uint64_t d) {
  a.store(a.load(std::memory_order_relaxed) + d, std::memory_order_relaxed);
}

void fillName(ThreadStats* s) {
  for (int i = 0; i < 64; i++) {
    if (g_debugThreadRegistry[i].tid == s->tid && g_debugThreadRegistry[i].name[0]) {
      std::memcpy(s->name, g_debugThreadRegistry[i].name, sizeof(s->name) - 1);
      return;
    }
  }
}

Tls& tls() {
  Tls& t = t_tls;
  if (!t.st) {
    auto* s = new ThreadStats();
#ifdef _WIN32
    s->tid = GetCurrentThreadId();
#endif
    fillName(s);
    {
      std::lock_guard<std::mutex> lk(g_thrMu);
      g_threads.push_back(s);
    }
    t.st = s;
    t.entry = -1;
  }
  return t;
}

// ---------------------------------------------------------------- guest reads
uint32_t rd32(uint32_t va) { return va ? sr::kernel::readGuestU32BE(va) : 0u; }

uint32_t deviceVA() { return rd32(rd32(kVdGlobalDeviceCell)); }

// The stamp key of a guest VA: the PHYSICAL address of the byte the host reads,
// i.e. the alias-resolved host offset masked to the 512 MB physical space. The
// XDK writes command memory through one window (dev+0x28 is a VA) and the CP may
// read the same physical page through another (physwin::resolveIbWindow); keying
// by physical address makes both sides agree whatever window either one used.
uint32_t hostOff(uint32_t va) {
  uint8_t* h = sr::kernel::guestVAToHost(va);
  uint8_t* b = sr::kernel::getGuestBase();
  if (!h || !b || h < b) return 0;
  const uint64_t d = uint64_t(h - b);
  return d <= 0xFFFFFFFFull ? uint32_t(d) & 0x1FFFFFFFu : 0u;
}

// ---------------------------------------------------------------- stamps
std::mutex g_cmdMu;
std::map<uint32_t, Interval> g_cmd;  // key = lo (host offset)

std::mutex g_ringMu;
Stamp g_ring[kRingCap];
uint32_t g_ringMask = 0;  // last ring index mask seen (guarded by g_ringMu)

struct Census {
  std::atomic<uint64_t> brackets{0}, internalOrigin{0}, noDevice{0}, pieces{0},
      emptyPieces{0}, cursorBack{0}, cursorJump{0}, ringStamps{0}, ringDwords{0},
      depthAnomaly{0}, mapPeak{0};
  // the stamp lifetime (native_brackets.hpp): intervals moved with a native close's copy, and the hand-outs of the two
  // allocators that erased something (a previous lap's stamps)
  std::atomic<uint64_t> moved{0}, segErase{0}, allocErase{0};
  // blocks stamped at their 820FCDF0 hand-out, chained closes' list nodes marked; ownership: brackets opened without
  // the device positively their thread's (another owner, released, an unknown identity), cuts that found it lost,
  // pieces owned at both cuts dropped for an ownership change between them (the epoch moved), pieces refused for
  // overlapping another thread's stamp
  std::atomic<uint64_t> allocStamp{0}, chainMarks{0}, foreign{0}, ownerLost{0}, ownerShift{0}, conflict{0};
} g_census;

// The OWNERSHIP EPOCH: bumped at the enter and the leave of every scope that writes dev+0x2A08 - the only guest
// writers are D3DDevice_AcquireThreadOwnership 820F3618, ReleaseThreadOwnership 820F3658 and D3DDeviceCreate 820F34C0
// (its 82106010 / 82106280; no host code writes the field). A cut reads it before the owner and the cursor: an
// ownership change between two cuts (A -> B -> A included) leaves it different, so the piece is dropped. Enter: after
// the scope's own cut; leave: before it (the scope's own writes fall inside, the pieces around it are untouched).
std::atomic<uint64_t> g_ownerEpoch{0};
const int g_acquireRow = entryIndex(0x820F3618u), g_releaseRow = entryIndex(0x820F3658u),
          g_createRow = entryIndex(0x820F34C0u);
inline bool writesOwner(int idx) {
  return idx >= 0 && (idx == g_acquireRow || idx == g_releaseRow || idx == g_createRow);
}
#if defined(SR_DIFFTEST)
void (*g_cutGapHook)() = nullptr;  // a cut's read gap (between the owner read and the cursor read): the census suite
#endif
}  // namespace
#if defined(SR_DIFFTEST)
void setCutGapHook(void (*hook)()) { g_cutGapHook = hook; }
#endif
namespace {

// A cut's piece into the map. Never over another thread's stamp: a piece overlapping one (a device owner change no
// scope saw, e.g. a raw dev+0x2A08 write) is refused whole - false = refused, nothing changed.
bool insertInterval(uint32_t lo, uint32_t hi, const Stamp& s) {
  std::lock_guard<std::mutex> lk(g_cmdMu);
  {
    auto it = g_cmd.lower_bound(lo);
    if (it != g_cmd.begin() && std::prev(it)->second.hi > lo) it = std::prev(it);
    for (; it != g_cmd.end() && it->first < hi; ++it)
      if (it->second.s.tid != s.tid) return false;
  }
  auto it = g_cmd.lower_bound(lo);
  if (it != g_cmd.begin()) {
    auto prev = std::prev(it);
    if (prev->second.hi > lo) {
      if (prev->second.hi > hi) {  // split: keep the tail
        Interval tail = prev->second;
        tail.lo = hi;
        g_cmd[hi] = tail;
      }
      prev->second.hi = lo;
      if (prev->second.hi <= prev->second.lo) g_cmd.erase(prev);
    }
  }
  it = g_cmd.lower_bound(lo);
  while (it != g_cmd.end() && it->first < hi) {
    if (it->second.hi > hi) {
      Interval tail = it->second;
      tail.lo = hi;
      it = g_cmd.erase(it);
      g_cmd[hi] = tail;
      break;
    }
    it = g_cmd.erase(it);
  }
  g_cmd[lo] = Interval{lo, hi, s};
  const uint64_t n = g_cmd.size();
  if (n > g_census.mapPeak.load(std::memory_order_relaxed))
    g_census.mapPeak.store(n, std::memory_order_relaxed);
  return true;
}

// Remove (clip) every interval overlapping [lo, hi); the overlapping parts go to `taken` when given. Under g_cmdMu.
// true = something was removed.
bool eraseLocked(uint32_t lo, uint32_t hi, std::vector<Interval>* taken) {
  if (hi <= lo) return false;
  bool any = false;
  auto it = g_cmd.lower_bound(lo);
  if (it != g_cmd.begin()) {
    auto prev = std::prev(it);
    if (prev->second.hi > lo) {
      Interval iv = prev->second;
      if (taken) taken->push_back(Interval{lo, (std::min)(iv.hi, hi), iv.s});
      if (iv.hi > hi) g_cmd[hi] = Interval{hi, iv.hi, iv.s};  // the tail beyond the range stays
      prev->second.hi = lo;
      if (prev->second.hi <= prev->second.lo) g_cmd.erase(prev);
      any = true;
    }
  }
  it = g_cmd.lower_bound(lo);
  while (it != g_cmd.end() && it->first < hi) {
    Interval iv = it->second;
    it = g_cmd.erase(it);
    any = true;
    if (taken) taken->push_back(Interval{iv.lo, (std::min)(iv.hi, hi), iv.s});
    if (iv.hi > hi) {
      g_cmd[hi] = Interval{hi, iv.hi, iv.s};
      break;
    }
  }
  return any;
}

// The stamp lifetime's END (native_brackets.hpp), allocator 1: GpuCmdBufBeginNewSegment 820FCF90 handed out a new
// segment (dev+0x34C4 = base, the cursor at base - 4, dev+0x2C = base + size - 60): [base, dev+0x2C + 60) is reused
// memory - whatever is stamped there is a previous lap's. A device state that does not read as a fresh segment (the
// cursor elsewhere, a range beyond 4 MB) is left alone.
void eraseNewSegment(Tls& t) {
  const uint32_t dev = t.dev ? t.dev : deviceVA();
  if (!dev) return;
  const uint32_t base = rd32(dev + 0x34C4u), cursor = rd32(dev + kDevCursor), limit = rd32(dev + kDevLimit);
  if (!base || cursor + 4u != base || limit < base || limit - base > kMaxSub) return;
  const uint32_t lo = hostOff(base), hi = hostOff(limit + 60u);
  if (!lo || hi <= lo) return;
  std::lock_guard<std::mutex> lk(g_cmdMu);
  if (eraseLocked(lo, hi, nullptr)) g_census.segErase.fetch_add(1, std::memory_order_relaxed);
}

// The END, allocator 2: GpuCmdBufAllocate 820FCDF0(dev, dwords r4, align r5) returned the block r3 of 4 * dwords bytes
// (the tail carve above the cursor, or the allocator callback). Its N0 scope hooks (registered below while the
// brackets are on) carry the request across the call. And the block's BIRTH: handed out inside a bracket whose thread
// POSITIVELY owns the device at the hand-out (ownsDevice: never an unknown identity or the released device), it is
// stamped with the allocating scope - the innermost scope below 820FCDF0's own (the hook runs before the scope leaves) -
// since that scope writes it (a Z-pass kick block, a tail block); handed out outside any bracket (820FCDF0 the
// outermost call) or on a thread the device is not positively its, it stays unstamped (the erase above still ends the
// previous lap's stamps).
Stamp currentStamp(const Tls& t, uint8_t kind);
bool ownsDevice(const Tls& t);
const int g_allocRow = entryIndex(0x820FCDF0u);
thread_local uint32_t t_allocBytes = 0;
void allocBefore(void* p) { t_allocBytes = static_cast<PPCContext*>(p)->r4.u32 * 4u; }
void allocAfter(void* p) {
  const uint32_t va = static_cast<PPCContext*>(p)->r3.u32, bytes = t_allocBytes;
  t_allocBytes = 0;
  if (!va || !bytes || bytes > kMaxSub) return;
  const uint32_t lo = hostOff(va);
  if (!lo) return;
  const Tls& t = t_tls;
  int top = t.open && ownsDevice(t) ? t.innerTop : -1;  // the device positively this thread's at the hand-out
  if (top >= 0 && t.innerStack[top] == g_allocRow) top--;  // 820FCDF0's own scope (pushed when nested)
  Stamp s;
  if (top >= 0) {
    s = currentStamp(t, kStampBlock);
    s.inner = t.innerStack[top];
  }
  std::lock_guard<std::mutex> lk(g_cmdMu);
  if (eraseLocked(lo, lo + bytes, nullptr)) g_census.allocErase.fetch_add(1, std::memory_order_relaxed);
  if (top >= 0) {
    g_cmd[lo] = Interval{lo, lo + bytes, s};
    g_census.allocStamp.fetch_add(1, std::memory_order_relaxed);
  }
}

Stamp currentStamp(const Tls& t, uint8_t kind) {
  Stamp s;
  s.entry = t.entry;
  s.inner = t.open && t.innerTop >= 0 ? t.innerStack[t.innerTop] : t.entry;
  s.kind = kind;
  s.flags = t.flags;
  s.lr = t.lr;
  s.tid = t.st ? t.st->tid : 0;
  s.callId = t.callId;
  return s;
}

// A guest word read fault-free (the harness VEH makes a host AV fatal): false = unmapped / unreadable.
bool readable4(uint32_t va) {
  const uint8_t* h = va ? sr::kernel::guestVAToHost(va) : nullptr;
  return h && sr::kernel::hostRangeReadableCached(h, 4u);
}

// This thread's id as the SDK's owner test reads it (82468EF8): KTHREAD (KPCR+0x100) +332; 0 = unknown.
uint32_t threadIdOf(uint32_t r13) {
  if (!readable4(r13 + 0x100u)) return 0;
  const uint32_t kt = rd32(r13 + 0x100u);
  return readable4(kt + 332u) ? rd32(kt + 332u) : 0u;
}

// The device is POSITIVELY this thread's now: a known id equal to dev+0x2A08 (never 0 = released, never unreadable).
bool ownsDevice(const Tls& t) {
  if (!t.dev || !t.myId || !readable4(t.dev + 0x2A08u)) return false;
  const uint32_t owner = rd32(t.dev + 0x2A08u);
  return owner == t.myId;
}

// A cut's reads: the epoch, the owner, the cursor, then the epoch and the owner again. The device reads as this
// thread's only when it is at both owner reads with no ownership change in between: a takeover between the owner read
// and the cursor read (the cursor may already hold the new owner's bytes) reads as not owned. *epoch = the epoch
// before the reads (a change during or after them compares unequal at the next cut). Returns the cursor VA.
uint32_t readCut(const Tls& t, bool gapHook, bool* owned, uint64_t* epoch) {
  const uint64_t e0 = g_ownerEpoch.load();
  const bool own0 = ownsDevice(t);
#if defined(SR_DIFFTEST)
  if (gapHook && g_cutGapHook) g_cutGapHook();
#else
  (void)gapHook;
#endif
  const uint32_t curVA = rd32(t.dev + kDevCursor);
  const uint64_t e1 = g_ownerEpoch.load();
  *owned = own0 && e1 == e0 && ownsDevice(t);
  *epoch = e0;
  return curVA;
}

// Close the current piece at the device cursor, then restart it there. Stamped only when the device was this
// thread's at both of the piece's cuts (Tls::owned) AND no ownership change happened between them (Tls::epoch):
// otherwise the cursor movement may be another owner's.
void cutPiece(Tls& t, uint8_t kindOfClosed) {
  if (!t.dev) return;
  bool owned = false;
  uint64_t epoch = 0;
  const uint32_t curVA = readCut(t, true, &owned, &epoch);
  const bool both = owned && t.owned;
  const bool insert = both && epoch == t.epoch;
  if (t.owned && !owned) g_census.ownerLost.fetch_add(1, std::memory_order_relaxed);
  else if (both && !insert) g_census.ownerShift.fetch_add(1, std::memory_order_relaxed);
  t.owned = owned;
  t.epoch = epoch;
  const uint32_t cur = hostOff(curVA);
  if (insert && t.subStart && cur) {
    if (cur == t.subStart) {
      g_census.emptyPieces.fetch_add(1, std::memory_order_relaxed);
    } else if (cur < t.subStart) {
      g_census.cursorBack.fetch_add(1, std::memory_order_relaxed);
    } else if (cur - t.subStart > kMaxSub || (t.subLimit && cur > t.subLimit + kLimitSlack)) {
      g_census.cursorJump.fetch_add(1, std::memory_order_relaxed);
    } else {
      // The XDK cursor points at the LAST dword written (stwu idiom: the present
      // block writes its first dword at cursor+4, GpuCmdBufBeginNewSegment_820FCF90
      // sets the cursor to segment start - 4), so the dwords a piece wrote are
      // (subStart, cur], i.e. [subStart + 4, cur + 4).
      if (insertInterval(t.subStart + 4u, cur + 4u, currentStamp(t, kindOfClosed)))
        g_census.pieces.fetch_add(1, std::memory_order_relaxed);
      else
        g_census.conflict.fetch_add(1, std::memory_order_relaxed);
    }
  }
  t.subStart = cur;
  const uint32_t lim = rd32(t.dev + kDevLimit);
  t.subLimit = lim ? hostOff(lim) : 0;
}

std::atomic<uint32_t> g_nextCallId{1};

void openBracket(Tls& t, int idx, uint32_t lr, uint32_t r13) {
  t.open = true;
  t.owned = false;
  t.myId = threadIdOf(r13);
  t.callId = g_nextCallId.fetch_add(1, std::memory_order_relaxed);
  if (!t.callId) t.callId = g_nextCallId.fetch_add(1, std::memory_order_relaxed);
  t.entry = int16_t(idx);
  t.lr = lr;
  t.flags = (lr >= kXdkLo && lr < kXdkHi) ? kStampInternalOrigin : 0;
  t.kindTop = 0;
  t.kindStack[0] = kEntry;
  t.innerTop = 0;
  t.innerStack[0] = int16_t(idx);
  t.subStart = 0;
  t.subLimit = 0;
  t.dev = deviceVA();
  g_census.brackets.fetch_add(1, std::memory_order_relaxed);
  if (t.flags) g_census.internalOrigin.fetch_add(1, std::memory_order_relaxed);
  if (!t.dev) {
    g_census.noDevice.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  bool owned = false;
  const uint32_t cur = hostOff(readCut(t, false, &owned, &t.epoch));
  t.owned = owned;
  if (!t.owned) g_census.foreign.fetch_add(1, std::memory_order_relaxed);
  t.subStart = cur;
  const uint32_t lim = rd32(t.dev + kDevLimit);
  t.subLimit = lim ? hostOff(lim) : 0;
}

void closeBracket(Tls& t) {
  if (!t.dev) t.dev = deviceVA();  // CreateDevice: the device appears during the call
  cutPiece(t, t.kindStack[t.kindTop]);
  t.open = false;
  t.entry = -1;
  t.callId = 0;
  t.dev = 0;
}

// ---------------------------------------------------------------- frame split
// Cost of one scope on the calling thread, measured once with the same
// operations (TLS access, two rdtsc, counter updates) - an ESTIMATE of the
// instrumentation overhead quoted next to every number.
double g_scopeCostNs = -1.0;
void calibrate() {
  if (g_scopeCostNs >= 0.0) return;
  Tls& t = tls();
  const uint64_t a = __rdtsc();
  volatile uint64_t sink = 0;
  for (int i = 0; i < 200000; i++) {
    const uint32_t d = t.depth++;
    const uint64_t s0 = d == 0 ? __rdtsc() : 0;
    t.calls++;
    if (--t.depth == 0) sink = sink + (__rdtsc() - s0);
  }
  const uint64_t b = __rdtsc();
  t.calls -= 200000;
  g_scopeCostNs = double(b - a) / 200000.0 / clock().ticksPerMs() * 1e6;
  (void)sink;
}

double pct(double part, double whole) { return whole > 0 ? 100.0 * part / whole : 0.0; }

void reportWindow(ThreadStats* s, bool cumulative) {
  // Caller holds s->fmu.
  const size_t from = cumulative ? 0 : s->winStart;
  const size_t n = s->frames.size() - from;
  if (!n) return;
  std::vector<float> fm;
  fm.reserve(n);
  double frame = 0, xdk = 0, wait = 0, impIn = 0, impOut = 0, outer = 0, calls = 0;
  for (size_t i = from; i < s->frames.size(); i++) {
    const FrameRec& r = s->frames[i];
    fm.push_back(r.frameMs);
    frame += r.frameMs; xdk += r.xdkMs; wait += r.waitMs;
    impIn += r.impInMs; impOut += r.impOutMs;
    outer += r.outerCalls; calls += r.calls;
  }
  std::sort(fm.begin(), fm.end());
  const double p50 = fm[n / 2], p90 = fm[std::min(n - 1, n * 9 / 10)];
  const double rest = frame - xdk - impOut;
  const double ovhMs = g_scopeCostNs > 0 ? outer * g_scopeCostNs * 1e-6 : -1.0;
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_FRAMESPLIT] %s thr=%u(%s) frames=%zu frame_ms mean=%.2f p50=%.2f p90=%.2f "
              "| xdk=%.2fms %.1f%% (wait=%.2fms %.1f%% imp=%.2fms %.1f%%) | imp_out=%.2fms %.1f%% "
              "| rest=%.2fms %.1f%% | calls/f=%.0f outer/f=%.0f ovh_est=%.2fms/f (%.1fns/scope)",
              cumulative ? "CUM" : "WIN", s->tid, s->name[0] ? s->name : "?", n, frame / n, p50,
              p90, xdk / n, pct(xdk, frame), wait / n, pct(wait, frame), impIn / n,
              pct(impIn, frame), impOut / n, pct(impOut, frame), rest / n, pct(rest, frame),
              calls / n, outer / n, ovhMs >= 0 ? ovhMs / n : -1.0, g_scopeCostNs);
}

// Outermost-scope time per entry (all threads): what each XDK entry costs the guest - sizes
// the native draw entries (N2 mode B) against the rest of the XDK.
std::atomic<uint64_t> g_entryTicks[1024];
std::atomic<uint64_t> g_entryCalls[1024];
void reportEntries() {
  const double tpm = clock().ticksPerMs();
  uint64_t tot = 0;
  for (int i = 0; i < kEntryCount && i < 1024; i++) tot += g_entryTicks[i].load(std::memory_order_relaxed);
  if (!tot) return;
  std::string top;
  bool used[1024] = {};
  for (int k = 0; k < 12; k++) {
    int best = -1;
    for (int i = 0; i < kEntryCount && i < 1024; i++)
      if (!used[i] && g_entryTicks[i].load(std::memory_order_relaxed) &&
          (best < 0 || g_entryTicks[i].load(std::memory_order_relaxed) > g_entryTicks[best].load(std::memory_order_relaxed)))
        best = i;
    if (best < 0) break;
    used[best] = true;
    const uint64_t t = g_entryTicks[best].load(std::memory_order_relaxed);
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s%s %.0fms %.1f%% (%llu calls)", top.empty() ? "" : ", ", kEntries[best].name,
                  double(t) / tpm, 100.0 * double(t) / double(tot),
                  (unsigned long long)g_entryCalls[best].load(std::memory_order_relaxed));
    top += buf;
  }
  SR_LOG_BOOT(sr::LogSeverity::Info, "[NATIVE_FRAMESPLIT] ENTRIES xdk outer total=%.0fms: %s", double(tot) / tpm,
              top.c_str());
}

void reportOtherThreads() {
  reportEntries();
  const double tpm = clock().ticksPerMs();
  const double wallMs = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - clock().t0).count();
  std::lock_guard<std::mutex> lk(g_thrMu);
  for (ThreadStats* s : g_threads) {
    const uint64_t calls = s->calls.load(std::memory_order_relaxed);
    if (!calls) continue;
    if (!s->name[0]) fillName(s);
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[NATIVE_FRAMESPLIT] THREAD thr=%u(%s) wall=%.0fms xdk=%.0fms (%.1f%%) wait=%.0fms "
                "imp_in=%.0fms imp_out=%.0fms (%.1f%%) calls=%llu outer=%llu",
                s->tid, s->name[0] ? s->name : "?", wallMs,
                double(s->xdk.load()) / tpm, pct(double(s->xdk.load()) / tpm, wallMs),
                double(s->wait.load()) / tpm, double(s->impIn.load()) / tpm,
                double(s->impOut.load()) / tpm, pct(double(s->impOut.load()) / tpm, wallMs),
                (unsigned long long)calls, (unsigned long long)s->outer.load());
  }
}

void presentBoundary(Tls& t, uint64_t now) {
  ThreadStats* s = t.st;
  if (t.lastPresentTsc) {
    const double tpm = clock().ticksPerMs();
    FrameRec r{float(double(now - t.lastPresentTsc) / tpm), float(double(t.xdk) / tpm),
               float(double(t.wait) / tpm), float(double(t.impIn) / tpm),
               float(double(t.impOut) / tpm), t.outerCalls, t.calls};
    std::lock_guard<std::mutex> lk(s->fmu);
    if (s->frames.empty()) {
      s->winStartTsc = now;
      s->lastCumTsc = now;
      if (!s->name[0]) fillName(s);
    }
    s->frames.push_back(r);
    if (double(now - s->winStartTsc) / tpm >= 2000.0) {
      calibrate();
      reportWindow(s, false);
      s->winStart = s->frames.size();
      s->winStartTsc = now;
      if (double(now - s->lastCumTsc) / tpm >= 30000.0) {
        s->lastCumTsc = now;
        reportWindow(s, true);
        reportOtherThreads();
      }
    }
  }
  t.xdk = t.wait = t.impIn = t.impOut = 0;
  t.outerCalls = t.calls = 0;
  t.lastPresentTsc = now;
}

// state bits
constexpr uint32_t kStOuter = 1u << 31, kStWait = 1u << 30, kStSplit = 1u << 29,
                   kStRing = 1u << 28, kStInner = 1u << 27;

}  // namespace

// The GpuCmdBufAllocate end of the stamp lifetime rides 820FCDF0's N0 scope hooks (a table internal), registered once
// at load while the brackets are on (before the first guest call; the hook table never changes afterwards). false with
// the brackets off: no hook, the shim's scope stays inactive.
const bool g_allocHooked = g_bracketsOn && setScopeHook(0x820FCDF0u, allocBefore, allocAfter);

// ============================================================== scope hooks
extern thread_local bool t_zpass;
extern thread_local uint32_t t_kicks;

void scopeEnter(int idx, uint32_t callerLr, uint32_t r13, uint64_t* tscOut, uint32_t* stateOut) {
  const EntryInfo& e = kEntries[idx];
  if (g_bracketsOn) {
    if (e.va == 0x8210DF38u) t_zpass = true;        // D3DDeviceBeginZPass
    else if (e.va == 0x8210E1A0u) t_zpass = false;  // D3DDeviceEndZPass
  }
  Tls& t = tls();
  t.calls++;
  uint32_t st = 0;
  const uint32_t d = t.depth++;
  if (d > 256) {  // a scope that never left (fault unwind): resynchronise
    g_census.depthAnomaly.fetch_add(1, std::memory_order_relaxed);
    t.depth = 1;
  }
  uint64_t now = 0;
  const bool outer = t.depth == 1;
  const bool waitOuter = g_frameSplitOn && e.kind == kWait && t.waitDepth == 0;
  if (e.kind == kWait) t.waitDepth++;
  if (outer || waitOuter) now = __rdtsc();
  if (outer) {
    st |= kStOuter;
    t.outerCalls++;
    if (g_frameSplitOn && e.present) presentBoundary(t, now);
    if (g_bracketsOn) openBracket(t, idx, callerLr, r13);
  }
  if (waitOuter) st |= kStWait;
  if (e.kind == kFlush || e.kind == kSwitch) t_kicks++;  // mode B stage 0 ledger (threadKicks)
  if (g_bracketsOn && t.open) {
    // The flush / switch split (as before), and a nested table scope: the bytes so far are the enclosing scope's;
    // from here on this scope's (Stamp::inner).
    const bool split = (e.kind == kFlush || e.kind == kSwitch) && t.kindTop < 15;
    const bool inner = !outer && t.innerTop >= 0 && t.innerTop < 31;
    if (split || inner) cutPiece(t, t.kindStack[t.kindTop]);
    if (split) {
      t.kindStack[++t.kindTop] = e.kind;
      st |= kStSplit;
    }
    if (inner) {
      t.innerStack[++t.innerTop] = int16_t(idx);
      st |= kStInner;
    }
  }
  // an ownership writer: the epoch moves after this scope's own cut (and the outermost open's reads) - any piece that
  // spans the call (the writer's own) is dropped, the pieces before it are not
  if (g_bracketsOn && writesOwner(idx)) g_ownerEpoch.fetch_add(1);
  if (g_bracketsOn && e.kind == kRingWrite) {
    if (!t.dev) t.dev = deviceVA();
    if (t.dev) {
      st |= kStRing | (rd32(t.dev + kDevRingWptr) & 0xFFFFu);
    }
  }
  *tscOut = now;
  *stateOut = st;
}

void scopeLeave(int idx, uint64_t tsc0, uint32_t st) {
  const EntryInfo& e = kEntries[idx];
  Tls& t = t_tls;
  if (g_bracketsOn && writesOwner(idx)) g_ownerEpoch.fetch_add(1);  // before this scope's own cut (or its close)
  if (st & (kStSplit | kStInner)) cutPiece(t, t.kindStack[t.kindTop]);  // the bytes this scope wrote
  if ((st & kStSplit) && t.kindTop > 0) t.kindTop--;
  if (g_bracketsOn && e.kind == kSwitch) eraseNewSegment(t);
  if (st & kStRing) {
    const uint32_t mask = rd32(t.dev + kDevRingMask);
    if (mask && mask < kRingCap && (mask & (mask + 1u)) == 0) {
      const uint32_t w0 = st & 0xFFFFu & mask;
      const uint32_t w1 = rd32(t.dev + kDevRingWptr) & mask;
      Stamp s = currentStamp(t, kRingWrite);
      if (s.entry < 0) s.entry = int16_t(idx);
      std::lock_guard<std::mutex> lk(g_ringMu);
      g_ringMask = mask;
      uint32_t n = 0;
      for (uint32_t i = w0; i != w1 && n <= mask; i = (i + 1u) & mask, n++) g_ring[i] = s;
      g_census.ringStamps.fetch_add(1, std::memory_order_relaxed);
      g_census.ringDwords.fetch_add(n, std::memory_order_relaxed);
    }
  }
  if ((st & kStInner) && t.innerTop > 0) t.innerTop--;
  if (e.kind == kWait && t.waitDepth) t.waitDepth--;
  const uint64_t now = (st & (kStOuter | kStWait)) ? __rdtsc() : 0;
  if (st & kStWait) {
    const uint64_t dt = now - tsc0;
    t.wait += dt;
    bump(t.st->wait, dt);
  }
  if (t.depth) t.depth--;
  if (st & kStOuter) {
    const uint64_t dt = now - tsc0;
    if (idx >= 0 && idx < 1024) {
      g_entryTicks[idx].fetch_add(dt, std::memory_order_relaxed);
      g_entryCalls[idx].fetch_add(1, std::memory_order_relaxed);
    }
    t.xdk += dt;
    bump(t.st->xdk, dt);
    bump(t.st->outer, 1);
    if (g_bracketsOn && t.open) closeBracket(t);
  }
  bump(t.st->calls, 1);
}

uint64_t importEnter() { return __rdtsc(); }

void importLeave(uint64_t tsc0) {
  Tls& t = tls();
  const uint64_t dt = __rdtsc() - tsc0;
  if (t.depth) {
    t.impIn += dt;
    bump(t.st->impIn, dt);
  } else {
    t.impOut += dt;
    bump(t.st->impOut, dt);
  }
}

// ============================================================== lookups
uint32_t currentCallId() {
  const Tls& t = t_tls;
  return t.open ? t.callId : 0u;
}

thread_local bool t_zpass = false;
bool zPassActive() { return t_zpass; }
thread_local uint32_t t_kicks = 0;
uint32_t threadKicks() { return t_kicks; }

uint32_t stampKey(uint32_t va) { return hostOff(va); }

std::vector<Interval> takeIntervals(uint32_t lo, uint32_t hi) {
  std::vector<Interval> out;
  std::lock_guard<std::mutex> lk(g_cmdMu);
  eraseLocked(lo, hi, &out);
  g_census.moved.fetch_add(out.size(), std::memory_order_relaxed);
  return out;
}

void eraseIntervals(uint32_t lo, uint32_t hi) {
  std::lock_guard<std::mutex> lk(g_cmdMu);
  eraseLocked(lo, hi, nullptr);
}

void markChain(uint32_t va, uint32_t bytes) {
  if (!g_bracketsOn || !bytes) return;
  const uint32_t lo = hostOff(va);
  if (!lo) return;
  Stamp s;  // unowned: entry / inner -1
  s.flags = kStampChain;
  s.tid = t_tls.st ? t_tls.st->tid : 0;
  std::lock_guard<std::mutex> lk(g_cmdMu);
  eraseLocked(lo, lo + bytes, nullptr);
  g_cmd[lo] = Interval{lo, lo + bytes, s};
  g_census.chainMarks.fetch_add(1, std::memory_order_relaxed);
}

void restartPiece(uint32_t cursorVA) {
  if (!g_bracketsOn) return;
  Tls& t = t_tls;
  if (!t.open || !t.dev) return;
  t.subStart = hostOff(cursorVA);
  const uint64_t e0 = g_ownerEpoch.load();  // as a cut: the epoch first, the owner, the epoch again
  t.owned = ownsDevice(t) && g_ownerEpoch.load() == e0;
  t.epoch = e0;
  const uint32_t lim = rd32(t.dev + kDevLimit);
  t.subLimit = lim ? hostOff(lim) : 0;
}

size_t snapshotIntervals(uint32_t lo, uint32_t hi, Interval* out, size_t cap) {
  std::lock_guard<std::mutex> lk(g_cmdMu);
  size_t n = 0;
  auto it = g_cmd.upper_bound(lo);
  if (it != g_cmd.begin()) {
    auto prev = std::prev(it);
    if (prev->second.hi > lo) it = prev;
  }
  for (; it != g_cmd.end() && it->first < hi && n < cap; ++it) out[n++] = it->second;
  return n;
}

bool neighbours(uint32_t off, Interval* prev, Interval* next) {
  std::lock_guard<std::mutex> lk(g_cmdMu);
  auto it = g_cmd.upper_bound(off);
  bool any = false;
  if (next) {
    if (it != g_cmd.end()) { *next = it->second; any = true; }
    else next->s.entry = -1, next->lo = next->hi = 0;
  }
  if (prev) {
    if (it != g_cmd.begin()) { *prev = std::prev(it)->second; any = true; }
    else prev->s.entry = -1, prev->lo = prev->hi = 0;
  }
  return any;
}

Stamp takeRingStamp(uint32_t idx) {
  std::lock_guard<std::mutex> lk(g_ringMu);
  if (idx >= kRingCap) return Stamp{};
  Stamp s = g_ring[idx];
  g_ring[idx] = Stamp{};
  return s;
}

void clearRingStamps(uint32_t idx, uint32_t n) {
  std::lock_guard<std::mutex> lk(g_ringMu);
  const uint32_t mask = g_ringMask ? g_ringMask : uint32_t(kRingCap - 1);
  for (uint32_t i = 0; i < n && i <= mask; i++) g_ring[(idx + i) & mask] = Stamp{};
}

void bracketsCensus(const char* why) {
  if (!g_bracketsOn) return;
  size_t mapN;
  {
    std::lock_guard<std::mutex> lk(g_cmdMu);
    mapN = g_cmd.size();
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[NATIVE_BRACKETS] %s brackets=%llu internalOrigin=%llu noDevice=%llu pieces=%llu "
              "empty=%llu cursorBack=%llu cursorJump=%llu ringStamps=%llu ringDwords=%llu "
              "depthAnomaly=%llu map=%zu (peak %llu) lifetime moved=%llu segErase=%llu allocErase=%llu allocStamp=%llu "
              "chainMarks=%llu foreign=%llu ownerLost=%llu ownerShift=%llu conflict=%llu allocHook=%d",
              why, (unsigned long long)g_census.brackets.load(),
              (unsigned long long)g_census.internalOrigin.load(),
              (unsigned long long)g_census.noDevice.load(),
              (unsigned long long)g_census.pieces.load(),
              (unsigned long long)g_census.emptyPieces.load(),
              (unsigned long long)g_census.cursorBack.load(),
              (unsigned long long)g_census.cursorJump.load(),
              (unsigned long long)g_census.ringStamps.load(),
              (unsigned long long)g_census.ringDwords.load(),
              (unsigned long long)g_census.depthAnomaly.load(), mapN,
              (unsigned long long)g_census.mapPeak.load(), (unsigned long long)g_census.moved.load(),
              (unsigned long long)g_census.segErase.load(), (unsigned long long)g_census.allocErase.load(),
              (unsigned long long)g_census.allocStamp.load(), (unsigned long long)g_census.chainMarks.load(),
              (unsigned long long)g_census.foreign.load(), (unsigned long long)g_census.ownerLost.load(),
              (unsigned long long)g_census.ownerShift.load(), (unsigned long long)g_census.conflict.load(),
              g_allocHooked ? 1 : 0);
}

// Whole-run frame-split summary (exit path; also safe to call any time).
void frameSplitFinal() {
  if (!g_frameSplitOn) return;
  calibrate();
  std::vector<ThreadStats*> ts;
  {
    std::lock_guard<std::mutex> lk(g_thrMu);
    ts = g_threads;
  }
  for (ThreadStats* s : ts) {
    std::lock_guard<std::mutex> lk(s->fmu);
    if (!s->frames.empty()) reportWindow(s, true);
  }
  reportOtherThreads();
}

}  // namespace sr::native
