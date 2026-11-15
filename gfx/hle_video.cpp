// iter-441/442 MIGRATION PHASE 1-2 — see gfx/hle_video.hpp for the model.
//
// This translation unit owns the command queue, the render thread and the
// counters. It owns NO D3D12 objects yet: S3 moves device ownership here and
// replaces RenderCommandType::Present with the reference's
// BeginCommandList/ExecuteCommandList rendezvous.
//
// HARD RULE inherited from the reference (video.cpp): the render thread must
// never read guest memory. Anything the guest owns is snapshotted on the
// PRODUCER side at enqueue time — otherwise the guest overwrites a resource
// (movie planes are rewritten every frame into the SAME allocation, ENGINE_MAP
// §6g.2) before the render thread gets to it, and we render last frame's bytes.

#include "hle_video.hpp"
#include "backend_publication.hpp"
#include "cube_layout.hpp"          // iter-457 H1a: the ONE cube layout body
#include "vb_publish.hpp"   // iter-456p Step 3A: the publication decode
#include "tex_ownership.hpp"
#include "../kernel/sr_oracle.hpp"
#if SR_ORACLE
#include "hle_prov_ring.hpp"
#endif
#include "xenos_state.hpp"     // iter-450UE FIX B: texswz:: swizzle decode
#include "../kernel/tex_census.hpp"   // iter-458 (Astra 068): texture-path census taps

#include <atomic>
#include <chrono>             // iter-450f: within-roll whiten toggle phase
#include <cmath>              // iter-450N: palette-selector decode
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>          // iter-449s: replay autopsy sort
#include <map>                // iter-449s: replay autopsy aggregation
#include <set>                // iter-450O: PALSEL (vs,idx) dedup
#include <memory>             // iter-449L: VB cache stable-address entries
#include <mutex>
#include <tuple>              // iter-449s: replay autopsy key
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>          // GetCurrentThreadId (iter-445 sequence probe)

#include "../app/log.hpp"
#include "d3d12_backend.hpp"
#include "shader_cache.hpp"   // S5: payload hashing for the trace round-trip

namespace sr {
namespace gfx {
namespace hle {

namespace {

// Double-buffered producer vector: the drain SWAPS buffers, so producers are
// never blocked for longer than a swap and the buffers keep their capacity
// (no per-command allocation at 4,250 cmds/frame).
struct Queue {
  std::mutex m;
  std::condition_variable cv;
  std::vector<RenderCommand> produce;
  bool started;
  Queue() : started(false) { produce.reserve(8192); }
};

Queue& queue() {
  static Queue s_q;
  return s_q;
}

std::atomic<uint64_t> s_enqueued{0};
std::atomic<uint64_t> s_drained{0};
std::atomic<uint64_t> s_present{0};
std::atomic<uint64_t> s_submitted{0};
std::atomic<uint64_t> s_perType[static_cast<size_t>(RenderCommandType::Count)];
std::atomic<uint32_t> s_depth{0};
std::atomic<uint32_t> s_maxDepth{0};

// -- iter-458 (Astra 017): STAGING BACKPRESSURE, SR_HLE_ARENA_WAIT (default OFF).
// Every staging pool is fixed-capacity and is freed ONLY when the render thread
// executes the published command that carries the slot. Under render-thread lag
// the producers found the pools full and REJECTED the snapshot, so the draw or
// upload was dropped (idle1: 144,470 UP drops). A console's command ring never
// drops work - a full ring stalls the CPU - so with the gate on the producer
// WAITS for a release instead, with three hard exits:
//   * the render thread never waits (it is the only consumer);
//   * nothing pending in the queue for ~50 ms means no release can come (the
//     busy slots are held by unpublished work or leaked) -> fail at once;
//   * a hard timeout.
// A failed wait INVALIDATES THE RUN (logged, then the process terminates) - a
// capacity drop under this gate is never silent. Exhaustion and invalid input
// are counted SEPARATELY per pool whether or not the gate is on.
enum AwPool : int { kAwBlock = 0, kAwGeo, kAwArena, kAwTex, kAwClassA, kAwPools };
const char* const kAwPoolNames[kAwPools] = {"block", "geo", "arena", "tex", "classA"};
std::atomic<uint64_t> g_awExhausted[kAwPools];
std::atomic<uint64_t> g_awInvalid[kAwPools];
std::atomic<uint64_t> g_awWaits{0}, g_awWaitsOk{0}, g_awWaitUsTotal{0},
    g_awWaitUsMax{0}, g_awTimeouts{0}, g_awNoProgress{0},
    g_awRenderRefusals{0}, g_awFails{0};
std::mutex g_awMu;
std::condition_variable g_awCv;
std::atomic<int> g_awWaiters{0};
std::atomic<int> g_awEnabledOverride{-1};     // test seam; -1 = env
// iter-458 (Astra 019): COMPLETE capacity accounting in one line. The
// power-of-two [ARENA_WAIT] line is a sampled snapshot; this one carries every
// cumulative counter, so the last one before a window closes (or the process
// is stopped) is the terminal total for everything up to that point.
void logArenaWaitSum(const char* site) {
  const ArenaWaitStats s = arenaWaitStats();
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[ARENA_WAIT_SUM] site=%s gate=%d | waits=%llu ok=%llu timeouts=%llu "
      "noProgress=%llu fails=%llu renderRefusals=%llu waitUsTotal=%llu "
      "waitUsMax=%llu | exhausted block/geo/arena/tex/classA="
      "%llu/%llu/%llu/%llu/%llu invalid=%llu/%llu/%llu/%llu/%llu",
      site, arenaWaitEnabled() ? 1 : 0,
      (unsigned long long)s.waits, (unsigned long long)s.waitsOk,
      (unsigned long long)s.timeouts, (unsigned long long)s.noProgress,
      (unsigned long long)s.fails, (unsigned long long)s.renderRefusals,
      (unsigned long long)s.waitUsTotal, (unsigned long long)s.waitUsMax,
      (unsigned long long)s.exhausted[0], (unsigned long long)s.exhausted[1],
      (unsigned long long)s.exhausted[2], (unsigned long long)s.exhausted[3],
      (unsigned long long)s.exhausted[4], (unsigned long long)s.invalid[0],
      (unsigned long long)s.invalid[1], (unsigned long long)s.invalid[2],
      (unsigned long long)s.invalid[3], (unsigned long long)s.invalid[4]);
}
std::atomic<uint32_t> g_awTimeoutMs{5000};
bool (*g_awPendingProbe)() = nullptr;          // test seam; null = queue depth
void (*g_awFailHook)(const char*, const char*, uint32_t, uint64_t) = nullptr;
thread_local bool t_awFull = false;   // last *Once failure: capacity vs invalid
thread_local int t_awPool = kAwArena; // which pool that failure came from
thread_local bool t_awRenderThread = false;

// Called on EVERY *Once failure: classify, count, and log the first few and
// every power of two per pool (the counters are cumulative from boot).
void awNote(int pool, bool full) {
  t_awFull = full;
  t_awPool = pool;
  std::atomic<uint64_t>* ctr = full ? g_awExhausted : g_awInvalid;
  const uint64_t n = ctr[pool].fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 4 || (n & (n - 1)) == 0)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[ARENA_STATS] pool=%s %s #%llu (cumulative from boot; exhausted "
        "block/geo/arena/tex/classA=%llu/%llu/%llu/%llu/%llu invalid=%llu/%llu/"
        "%llu/%llu/%llu)", kAwPoolNames[pool], full ? "EXHAUSTED" : "INVALID",
        (unsigned long long)n,
        (unsigned long long)g_awExhausted[0].load(), (unsigned long long)g_awExhausted[1].load(),
        (unsigned long long)g_awExhausted[2].load(), (unsigned long long)g_awExhausted[3].load(),
        (unsigned long long)g_awExhausted[4].load(),
        (unsigned long long)g_awInvalid[0].load(), (unsigned long long)g_awInvalid[1].load(),
        (unsigned long long)g_awInvalid[2].load(), (unsigned long long)g_awInvalid[3].load(),
        (unsigned long long)g_awInvalid[4].load());
}

// Wakes waiting producers after a release. Called with the pool's lock held;
// a waiter never holds g_awMu while taking a pool lock, so there is no cycle.
void awNotify() {
  if (g_awWaiters.load(std::memory_order_acquire) > 0) {
    std::lock_guard<std::mutex> lk(g_awMu);
    g_awCv.notify_all();
  }
}

// S3: the device, owned by the render thread once main() publishes it.
// Worker loops outlive a retired backend. Keep the publication alive until
// process exit so later empty acquisitions never touch a destroyed mutex.
auto& s_backend = *new sr::gfx::BackendPublication<sr::gfx::D3D12Backend>();
std::mutex s_presentMu;
std::condition_variable s_presentCv;

// SR_HLE_OWN_PRESENT=0 restores the pre-S3 arrangement (main loop presents by
// scraping the guest framebuffer). Kept as the escape hatch + A/B knob: this
// slice moves the present off the main thread entirely, and a regression must
// be one env var away, not one revert away.

// ── S4.2: the producer-side pixel arena (contract in hle_video.hpp) ─────────
struct Arena {
  std::mutex m;
  std::vector<uint8_t> mem;
  std::vector<uint8_t> busy;   // per-slot in-flight flag
  uint32_t next;               // round-robin cursor
  uint32_t inFlight;
  uint32_t peak;
  uint64_t writes;
  uint64_t bytes;
  uint64_t rejects;
  Arena() : next(0), inFlight(0), peak(0), writes(0), bytes(0), rejects(0) {}
};

Arena& arena() {
  static Arena s_a;
  return s_a;
}

// ── iter-444 S6e: the small-block pool (contract in hle_video.hpp) ──────────
// Deliberately the SAME structure as the arena rather than something cleverer:
// the pixel arena's slot discipline is the part that has been proven correct
// across three iterations, and the only thing wrong with putting a 36-byte
// decl through it was the slot SIZE.
struct Blocks {
  std::mutex m;
  std::vector<uint8_t> mem;
  std::vector<uint8_t> busy;
  uint32_t next;
  uint32_t inFlight;
  uint32_t peak;
  uint64_t writes;
  uint64_t bytes;
  uint64_t rejects;
  Blocks() : next(0), inFlight(0), peak(0), writes(0), bytes(0), rejects(0) {}
};

Blocks& blocks() {
  static Blocks s_b;
  return s_b;
}

// ── iter-444 S6g: the texture pool (contract in hle_video.hpp) ──────────────
// Third size class, same proven slot discipline. Lazily sized: a roll that
// never binds a general texture should not commit 64 MB.
struct TexPool {
  std::mutex m;
  std::vector<uint8_t> mem;
  std::vector<uint8_t> busy;
  uint32_t next, inFlight, peak;
  uint64_t writes, bytes, rejects;
  TexPool() : next(0), inFlight(0), peak(0), writes(0), bytes(0), rejects(0) {}
};

TexPool& texPool() {
  static TexPool s_t;
  return s_t;
}

// ── iter-442 S4.4-S4.6: the movie-frame render state ────────────────────────
// All of it lives on the render thread and is touched only from execute().
// SR_HLE_DRAW is the master gate for the whole first-light path: default OFF
// until it is verified on the replay fixture AND eyeballed, per the charter's
// force -> diagnose -> confirm -> bake cycle.
// BAKED DEFAULT-ON (iter-442; SR_HLE_DRAW=0 keeps the A/B). Verified visually
// on BOTH loops before baking: the replay fixture reproduces
// data/gpu_traces/fmv_eahd_golden.png (EA|HD logo, mean RGB [36.5,36.4,36.8],
// neutral), and a live natural K=1 roll rendered the animated EA logo and the
// full-colour WBIE logo into the backbuffer (GPU readback, max=255, rich
// per-channel means) with walksRet 4, crash 0, GATE=MOVIE-ACTIVE.
bool hleDrawEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_HLE_DRAW");
    return !e || e[0] != '0';
  }();
  return v;
}

constexpr uint32_t kMaxSamplers = 16;
std::unordered_map<uint32_t, uint32_t> s_texSlot;    // guest tex handle -> SRV slot
// iter-450c [LUM_BIND] probe: current RT-0 key as the executor sees it.
uint32_t s_probeCurRt = 0;
// iter-449x: guest tex handle -> Texture3D RANGE index (space1, 1..7).
// Disjoint from s_texSlot by construction: a key is a volume or a 2D texture,
// never both (the fetch constant's dimension field says which).
std::unordered_map<uint32_t, uint32_t> s_texSlot3D;
// iter-457 H1(b): key -> TextureCube descriptor index (1..7). Populated only
// by a SUCCESSFUL hleUploadCube; a refused cube leaves the key absent, so the
// slot resolves to 0 and reads the white dummy - today's picture.
std::unordered_map<uint32_t, uint32_t> s_texSlotCube;
// iter-457 H1(b)-P1: the cube backend stand-in (contract in hle_video.hpp).
// Null in production; the suite installs it because no D3D12Backend exists
// there. It replaces the backend CALL only - see the cube arm in execute().
std::atomic<CubeUploadStandInFn> s_cubeUploadStandIn{nullptr};
// r5: WHICH path owns the [S6_CUBE_SUM] line. Live execution emits it as
// "periodic" from execute(); a replay emits it exactly ONCE as
// "replay-final" from replayTrace() and suppresses the periodic path for
// its duration, so a capture never sees the r4 duplicate.
std::atomic<bool> s_replayActive{false};
// iter-457: guest key -> its STACKED depth, latched from the texture-fetch
// entry at bind time. OBSERVE-ONLY on its own: nothing reads it unless the
// stacked-resolve gate is on, so this map alone changes no behaviour.
//
// ⚠ A CONFLICT IS A REFUSAL, NOT A LAST-WRITE-WINS. If the same key ever
// reports two different depths, we do not know which destination we are
// about to build, and building the wrong one samples the wrong layer --
// silently. The key is poisoned (depth 0) and stays refused.
std::unordered_map<uint32_t, uint32_t> s_stackDepth;
std::atomic<uint64_t> s_stackedResolves{0};
std::atomic<uint64_t> s_stackedRefused{0};
std::atomic<uint64_t> s_stackDepthConflicts{0};

static void noteStackedTexture(uint32_t key, uint32_t depth) {
  auto it = s_stackDepth.find(key);
  if (it == s_stackDepth.end()) { s_stackDepth[key] = depth; return; }
  if (it->second == depth || it->second == 0u) return;
  s_stackDepthConflicts.fetch_add(1, std::memory_order_relaxed);
  it->second = 0u;                       // poisoned: refuse, never guess
}

// 0 = unknown or poisoned. Callers MUST treat 0 as "do not build".
uint32_t stackedDepthOf(uint32_t key) {
  auto it = s_stackDepth.find(key & 0x1FFFFFFFu);
  return it == s_stackDepth.end() ? 0u : it->second;
}
// iter-444 S6g: keys whose pixels the UNLOCK path owns (the movie planes).
// The movie planes are ALSO bound through SetTexture, so bind-time capture
// would happily re-upload them from a stale mid-frame snapshot and fight the
// Unlock path that is authoritative for them (they are rewritten every frame).
// Unlock wins for anything it has ever produced; bind-capture never overwrites.
// iter-456n, sixth adjudication: the ownership decision moved OUT of this file
// into gfx/tex_ownership.hpp, because the predicate that used to live here was
// REVERSED against its own documented contract (it returned "owned" for a
// DIFFERENT descriptor signature, so the upload arm admitted the same-epoch
// stale capture Class A exists to block and refused the recreation it must
// admit). The vestigial `s_unlockOwned` set is DELETED with it: it was written
// at two sites and read at none, so it recorded ownership nothing consulted.
//
// The state machine is a single production instance the tests drive directly.
// See tex_ownership.hpp for the contract and for what it deliberately does not
// decide (when a generation changes).
uint64_t s_texUploads = 0, s_texUploadFail = 0, s_texSkipOwned = 0;
// iter-457 H1a: the executor's fail-closed cube refusals, readable by the test.
std::atomic<uint64_t> s_cubeExecRefused{0};
// iter-457 H1a-r2: entries into the NON-CUBE upload path, counted before any
// backend test so a fall-through is visible without a live backend.
std::atomic<uint64_t> s_nonCubeUploadEntries{0};
// iter-457 H1a-r2: the REPLAY-side cube gate. The reason pointer is a string
// LITERAL owned by the validator, so storing it raw is safe and needs no
// allocation on the replay path.
std::atomic<uint64_t> s_cubeReplayRefused{0};
std::atomic<uint64_t> s_cubeReplayAccepted{0};
// iter-457 H1(b): how many cubes the BACKEND actually took, and how many
// it refused by name. Separate from s_cubeExecRefused, which also counts
// the gate-off case - conflating "off" with "tried and refused" is how a
// disabled feature reads as a working one.
std::atomic<uint64_t> s_cubeExecUploaded{0};
std::atomic<uint64_t> s_cubeExecUploadRefused{0};
std::atomic<const char*> s_cubeReplayReason{nullptr};
// iter-445 S1: does a BOUND texture resolve to an uploaded SRV slot? Counted at
// the bind itself, over every sampler, so the front-end era (which stages no
// draws at all today) still reports.
uint64_t s_bindHit = 0, s_bindMiss = 0;
// iter-445: the guest's own clears, counted. Execution is gated because the
// targets they name are not bound yet.
uint64_t s_clearSeen = 0, s_clearFSeen = 0;
// iter-446 R6: ClearF calls carrying a sub-rect, which v1 widens to the whole
// surface. Counted so the approximation is visible instead of assumed rare.
uint64_t s_clearRectApprox = 0;
// Astra 085: the ClearF colour contract, counted BY NAME on both sides.
// Executor: a colour that is missing (colorBytes 0, or its block unreadable) or
// malformed (any size other than kClearFColorBytes) is never read and never
// substituted; the command's TARGET bits are dropped instead.
uint64_t s_clearFColourMissing = 0, s_clearFColourMalformed = 0;
uint64_t s_clearFTargetsMasked = 0;
uint32_t s_clearFLastFlags = 0;
bool s_clearFLastColourOk = false;
float s_clearFLastRgba[4] = {0, 0, 0, 0};
// (Replayer-side colour accounting is PER CALL, in TraceStats - Astra 086: a
// process-cumulative counter printed beside a per-call record count could not
// conserve on the second replay in one process.)
// Astra 086: THE SUBMISSION BOUNDARY - see submitGuestClear().
sr::gfx::hle::ClearSubmitTapFn s_clearSubmitTap = nullptr;
// iter-446 R3/R4. The viewport and scissor are LATCHED here and not yet applied
// to draws: the draw path still renders through the backbuffer-sized geo target,
// so setting a 80x16 viewport on it would clip the movie to a corner. They are
// applied in the same slice that moves draws onto the bound render target.
uint64_t s_setRtSeen = 0, s_setDsSeen = 0, s_setVpSeen = 0, s_setScSeen = 0;
uint64_t s_setRtBound = 0, s_setDsBound = 0;   // binds that resolved to a resource
struct LatchedViewport { uint32_t x, y, w, h, minZ, maxZ; bool valid; };
LatchedViewport s_viewport{0, 0, 0, 0, 0, 0x3F800000u, false};
struct LatchedScissor { int32_t l, t, r, b; bool valid; };
LatchedScissor s_scissor{0, 0, 0, 0, false};
// iter-446 R5. `destKnown` and `srcPooled` are the two numbers that decide
// whether the copy in R5b can reach anything; everything else is shape.
uint64_t s_resolveSeen = 0, s_resolveDepth = 0, s_resolveClearRT = 0;
uint64_t s_resolveClearZ = 0, s_resolveDestKnown = 0, s_resolveSrcPooled = 0;
std::unordered_set<uint32_t> s_resolveDests, s_resolveSrcs;
// iter-448: per-destination skip census (key normalised). Answers "how were
// THIS key's resolves handled" — the anonymous global counters could not.
struct DestSkips { uint64_t exec = 0, rect = 0, sliced = 0; };
std::unordered_map<uint32_t, DestSkips> s_destSkips;
// ⚠ EVERY key the guest has ever BOUND through SetTexture, normalised the same
// way the sampler lookup normalises. This exists to answer ONE question that
// must not be assumed: when `destKnown` reads 0, is it because the destinations
// genuinely have no host resource yet, or because the resolve key and the bind
// key are derived DIFFERENTLY — iter-445's texture-identity bug, which hid for
// a whole iteration behind exactly this shape of zero. `s_texSlot` cannot
// settle it: it only ever contained the movie planes.
std::unordered_set<uint32_t> s_boundTexKeys;
// iter-446 R5b. `s_resolveOwned` is the unlock-ownership map's sibling: keys whose pixels
// the RESOLVE path owns, so bind-capture can never overwrite a render product
// with the stale guest memory that happens to sit at its address.
std::unordered_set<uint32_t> s_resolveOwned;
uint64_t s_resolveExec = 0, s_resolveSkipRect = 0, s_resolveSkipSliced = 0;
uint64_t s_resolveSkipOther = 0, s_resolveBindHit = 0;

// ── slice 6 / Class B v2 (SR_HLE_RESOLVE_OWNERSHIP_V2, DEFAULT OFF) ────────
// v1 kept `s_resolveOwned`, a PERMANENT set of physical keys with no erase
// path — the same defect class as the deleted `s_unlockOwned`: a destroyed or
// reused address inherited a dead render product's exemption forever. v2 routes
// resolve authority through the ONE ownership state machine, so it is scoped to
// a descriptor epoch and a generation and is superseded by a later Unlock, a
// recreation, a destroy, or physical-key reuse without any special case.
//
// v1 remains the behaviour when the gate is off, byte-for-byte.

// Resolve-authority accounting. `successNoEpoch` is the honest one: a
// backend-confirmed resolve whose destination descriptor could not be
// established (a legacy trace record with no descriptor, and no CURRENT claim
// to adopt). Such a resolve still executes — refusing to draw would be a
// regression — but it takes NO authority, because inventing an epoch is exactly
// the guess this slice exists to remove.
// The LAST APPLIED upload descriptor per normalized key — recorded executor
// state, written only where an upload actually reached the host texture. It
// exists so a LEGACY Resolve record (one carrying no descriptor, from a trace
// banked before the v2 producer packing) can adopt an epoch that was genuinely
// ESTABLISHED for that key by a preceding TextureUpload, instead of inventing
// one or inferring one from a later capture. Adoption still requires the
// generation to be current and the dimensions to match.
struct AppliedDesc { uint64_t sig = 0; uint64_t generation = 0; uint32_t dims = 0; };
std::unordered_map<uint32_t, AppliedDesc> s_lastAppliedDesc;

// ── THE CLASS-B ORDERED JOIN (SR_HLE_RESOLVE_V2_WATCH=<hex>[,<hex>...]) ────
// The director's H3 killer needs upload, backend-success, ownership transition
// and capture decision for named keys IN ORDER, so the presence or absence of a
// successful transition is readable rather than inferred from totals. A
// diagnostic: default empty, and it only prints.
bool rv2Watched(uint32_t key) {
  static const std::vector<uint32_t> s_keys = [] {
    std::vector<uint32_t> v;
    const char* e = std::getenv("SR_HLE_RESOLVE_V2_WATCH");
    if (e) for (const char* p = e; *p;) {
      char* end = nullptr;
      const unsigned long k = std::strtoul(p, &end, 16);
      if (end == p) break;
      v.push_back((uint32_t)(k & 0x1FFFFFFFu));
      p = (*end == ',') ? end + 1 : end;
    }
    return v;
  }();
  if (s_keys.empty()) return false;
  const uint32_t k = key & 0x1FFFFFFFu;
  for (uint32_t w : s_keys) if (w == k) return true;
  return false;
}
uint64_t s_rv2WatchSeq = 0;
void rv2Watch(uint32_t key, const char* what, uint64_t sig, const char* extra) {
  if (!rv2Watched(key)) return;
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[S6_RV2_WATCH] #%llu key=%08X %s sig=%016llX gen=%llu auth=%s %s",
      (unsigned long long)++s_rv2WatchSeq, key & 0x1FFFFFFFu, what,
      (unsigned long long)sig,
      (unsigned long long)unlockOwnership().generationOf(key),
      authorityName(unlockOwnership().authorityFor(key)), extra ? extra : "");
}

uint64_t s_rv2Claims = 0, s_rv2SuccessNoEpoch = 0, s_rv2LegacyAdopted = 0;
uint64_t s_rv2RefusedNoClaim = 0;

// iter-457: STACKED (2D-array) resolve destinations. DEFAULT OFF.
//
// The gap it closes, measured on the title fixture: RT 0x83a6d800 takes
// 4,684 of 6,049 admitted draws, its destination 0b5be000 is a four-slice
// 640x640 stacked texture, the guest resolves slices 0/1/2 full-surface,
// and we (a) refuse slices 1 and 2 outright and (b) register slice 0 as a
// 2D descriptor while all 672 of its consumers read the 3D band. So the
// whole pass reaches nothing.
//
// ⚠ OFF by default and NOT baked: this changes which descriptor a real
// draw samples, so it is a rendering change that needs a verdict, not a
// diagnostic. With the gate off the branch below is never taken and the
// refusal chain is byte-for-byte the old one.
bool stackedResolveEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_HLE_RESOLVE_STACKED");
    return e && e[0] == '1';
  }();
  return v;
}

bool resolveExecEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_RESOLVE");
  // iter-449o WAVE-1 BAKE: default-ON, proven across the iter-447..449
  // armed-roll record (20 rolls, 0 crashes, movie gate exact). `=0` keeps
  // the A/B; the [GATES] line names the live set on every roll.
    return !e || e[0] != '0';
  }();
  return s_on;
}

const char* resolveSkipName(sr::gfx::D3D12Backend::ResolveSkip s) {
  using S = sr::gfx::D3D12Backend::ResolveSkip;
  switch (s) {
    case S::None: return "none";
    case S::Disabled: return "disabled";
    case S::NoList: return "no-open-list";
    case S::NoSource: return "source-not-pooled";
    case S::DepthSource: return "DEPTH-SOURCE (named gap)";
    case S::SubRect: return "sub-rect";
    case S::Sliced: return "sliced";
    case S::CreateFailed: return "create-failed";
    case S::NoSlot: return "srv-slot-exhausted";
    case S::BadDestSize: return "BAD-DEST-EXTENT (§6g.12)";
    // ⚠ Added with the enum value, not after it: the first run of the interlock
    // printed "?" because the name was missing — a refusal that cannot say why
    // it refused is the same unusable-instrument class as one that never fires.
    case S::NoFrameList:
      return "NO-FRAME-LIST (main thread owns the list; SR_HLE_OWN_PRESENT=0)";
  }
  return "?";
}
// Astra 086: -1 = the env gate (production); 0/1 = the suite's override, so
// the submission boundary can be exercised in-process without an env latch.
std::atomic<int> g_guestClearOverride{-1};
bool guestClearEnabled() {
  const int o = g_guestClearOverride.load(std::memory_order_relaxed);
  if (o >= 0) return o != 0;
  static const bool v = [] {
    const char* e = std::getenv("SR_HLE_GUEST_CLEAR");
    return e && e[0] == '1';
  }();
  return v;
}
uint32_t s_samplerTex[kMaxSamplers] = {};            // sampler -> guest tex handle
uint64_t s_curVs = 0, s_curPs = 0;
uint64_t s_quadSeen = 0, s_quadSubmitted = 0;
// iter-444 S6i: staged draws that are NOT the movie quad — the arming
// signal for a trace that must actually CONTAIN general geometry.
uint64_t s_generalStaged = 0;

// ── iter-443 S6a: THE COVERAGE INSTRUMENT ───────────────────────────────────
// THE one true number for the general-geometry phase, and the reason it exists:
// "does it look right" is not a progress metric — it needs an eyeball, it is
// roll-dependent, and it cannot distinguish 3 draws from 300. Coverage is
// monotonic, cheap, and answers the only question that matters while S6 is
// being built: OF THE DRAWS THE GUEST ISSUES, HOW MANY DO WE ACTUALLY RENDER?
//
// Counted at the executor, NOT at the hook, so it measures what reached the
// render thread rather than what the guest asked for. A gap between
// [API_OBS] draw counts and `issued` here is itself a finding (dropped
// commands); a gap between `issued` and `rendered` is the S6 work remaining.
//
// Reasons are counted, not just the total: a bare "we render 0.4%" says
// nothing about what to fix next, whereas "97% skipped for no-vertex-data"
// names the next slice by itself.
struct Coverage {
  uint64_t issued;      // draw commands the executor saw
  // ⚠ iter-444 S6f: this counts draws STAGED into the per-frame list, which is
  // NOT the same as recorded. The north-star `recorded` comes from the backend,
  // i.e. from the code that actually issues DrawInstanced — because a present
  // that early-returns retires its staged batches without recording them, and a
  // staged-counts-as-rendered instrument would report those as pixels.
  uint64_t staged;
  uint64_t noVerts;     // no vertex snapshot (over the size cap, or unhooked path)
  uint64_t notQuad;     // has vertices but not the movie-quad shape we can draw
  uint64_t noPlanes;    // quad shape but a texture slot did not resolve
  uint64_t noBackend;   // backend absent or gate off
  uint64_t noExpand;    // S6e: shape ok but the declaration/VS pair did not map
};
Coverage s_cov{};

// ── iter-444 S6f: THE SHAPE GATE ────────────────────────────────────────────
// DEFAULT OFF, deliberately, and this is not timidity: a widened default would
// paint the 1x1 WHITE fallback over the verified movie picture for any draw
// whose texture has not resolved, i.e. trade a correct render for a higher
// number. SR_S6_WIDE=1 measures coverage; the default exe keeps the picture.
//
// ⛔ TWO CLAIMS THAT USED TO SIT HERE WERE STALE — corrected iter-447 after an
// audit found this comment still steering the next slice:
//   1. "s_texSlot is populated ONLY from TextureUnlock, i.e. only for the
//      movie's three planes" — FALSE since iter-444 S6g. There are THREE
//      populate sites: general BIND capture (~line 700), the upload path
//      (~763), and resolve destinations (~983). S6g moved capture to BIND
//      precisely BECAUSE Unlock does not see general textures: 883 distinct
//      textures are BOUND per run against texCreate=104 and 133 non-movie
//      Unlocks (hle_video.hpp §setTexture), so ~85% never pass through D3D9's
//      lock path at all.
//   2. "the s_texSlot gap IS the next slice" — this directive CONTRADICTED
//      PROGRESS's own conclusion ("do NOT do textures first") for a whole
//      iteration. Do not restate a next-slice decision here; it rots out of
//      sync with PROGRESS and docs/MIGRATION_API_HLE.md, which own it.
//      Current ordering: docs/MIGRATION_API_HLE.md §"THE NEXT TRUNK — as
//      AUDITED".
// SR_S6_STATE, DEFAULT OFF. The guest's real depth/blend/mask/viewport/c18
// CHANGES THE TRANSFORM OF A VERIFIED RENDER, so it may not ride in by default
// on the strength of "it should be more correct". First capture read
// vte=00000000 — neither of the two values PA_CL_VTE_CNTL is documented to take
// (0x43F on / 0x400 off), i.e. it looks UNWRITTEN for the movie draw, and
// honouring it would send a clip-space quad through the pixel-space branch.
bool s6StateEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_S6_STATE");
  // iter-449o WAVE-1 BAKE: default-ON, proven across the iter-447..449
  // armed-roll record (20 rolls, 0 crashes, movie gate exact). `=0` keeps
  // the A/B; the [GATES] line names the live set on every roll.
    return !e || e[0] != '0';
  }();
  return v;
}

// iter-448: blit the guest-named front-buffer texture to the backbuffer at
// Present. BAKED DEFAULT-ON (iter-449o wave-2; =0 keeps the A/B): it REPLACES
// the backbuffer content whenever the key hits — eyeballed in before baking.
bool presentFrontEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_HLE_PRESENT_FRONT");
  // iter-449o WAVE-2 BAKE: default-ON after wave 1 + two clean soak rolls
  // (movie gate exact, rejects=0 on all three). `=0` keeps the A/B.
    return !e || e[0] != '0';
  }();
  return v;
}

bool s6WideEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_S6_WIDE");
  // iter-449o WAVE-1 BAKE: default-ON, proven across the iter-447..449
  // armed-roll record (20 rolls, 0 crashes, movie gate exact). `=0` keeps
  // the A/B; the [GATES] line names the live set on every roll.
    return !e || e[0] != '0';
  }();
  return v;
}

// Skip-reason tally. Slots 0..8 mirror the backend's own HleReject verdict;
// slot 9 is OUR side's only refusal (no usable snapshot). Every increment of
// noExpand must bump exactly one of these — an unattributed refusal is how
// 39,983 skipped draws hid behind an empty [S6_REJECT] line.
constexpr uint32_t kBackendRejectKinds = 11;   // iter-445: +MultiStream
constexpr uint32_t kRejectKinds = kBackendRejectKinds + 1;
constexpr uint32_t kRejectNoSnapshot = kBackendRejectKinds;
uint64_t s_reject[kRejectKinds] = {};

// ── The prim census ─────────────────────────────────────────────────────────
// notQuad was a single number hiding the ONE fact that decides how much the
// widening is worth: which topologies the guest actually draws, and how big
// those draws are. Counted per prim type with a vertex-count histogram so the
// next slice is chosen from data rather than from what is easy to implement.
constexpr uint32_t kPrimKinds = 32;
uint64_t s_primN[kPrimKinds] = {};
uint64_t s_primVerts[kPrimKinds] = {};
uint64_t s_primMaxV[kPrimKinds] = {};
// iter-445 S1: SPLIT. The type histogram is valid for ALL THREE draw entry
// points (r4 is primType in every one of them), but the VERTEX columns are not:
// the shared macro publishes count=r6, which is startIndex for
// DrawIndexedVertices and strideBytes for BeginVertices. Counting types for all
// three and verts for DrawVertices only is the difference between a census that
// answers "does the guest ever draw a triangle FAN" and one that is dead on a
// wide roll (it was only ever called from the notQuad branch, which wide=1
// never reaches).
void primCensusType(uint32_t prim) {
  s_primN[prim < kPrimKinds ? prim : kPrimKinds - 1]++;
}
void primCensusVerts(uint32_t prim, uint32_t count) {
  const uint32_t p = prim < kPrimKinds ? prim : kPrimKinds - 1;
  s_primVerts[p] += count;
  if (count > s_primMaxV[p]) s_primMaxV[p] = count;
}

// ── iter-444 S6e: the guest's DERIVED STATE, owned by the render thread ─────
// All of it arrives as its own command immediately before the draw it belongs
// to, so "whatever is current when a draw executes" is exactly the state that
// draw was issued with. Held as HOST bytes: the render thread never reads
// guest memory (the file's hard rule), and the small-block pool that carried
// them here is released the moment the bytes are copied out.
constexpr uint32_t kMaxDeclElems = 32;
constexpr uint32_t kDeclElemBytes = 12;
uint8_t  s_declRaw[kMaxDeclElems * kDeclElemBytes];
uint32_t s_declCount = 0;

constexpr uint32_t kVsConstBytes = 256u * 16u;   // c0..c255
// iter-449v: 256 float4 (was 224 — 33/208 cached PS read up to c231, whose
// loads silently landed in the adjacent shared block). Producer may append
// an 8-byte bool tail (VS/PS bool dword 0); psBytes >= kPsConstBytes+8 is
// the witness. Bools are already host-endian in the payload.
constexpr uint32_t kPsConstBytes = 256u * 16u;   // c0..c255
// ── iter-444 S6h: the guest's decoded render state ──────────────────────────
// Big-endian in the shadow, like the ALU constants. Register index within the
// captured block = (reg - 0x2100), because the 0x2100 and 0x2200 runs are
// contiguous in the device shadow.
struct HleState {
  bool valid;
  uint32_t depthControl, blendControl, colorMask, vteCntl;
  float vpScale[3], vpOffset[3];
  // iter-450T: per-slot sampler-table index decoded from the appended
  // texture-fetch file (see the SetRenderState consumer). sampValid=false
  // (old traces / capture miss) keeps the fixed clamp+linear behavior.
  bool sampValid;
  uint8_t sampSel[16];
  // iter-450UE FIX B: per-slot GUEST TEXTURE-FETCH SWIZZLE (12 bits) from the
  // same appended fetch file, plus that entry's OWN decoded base. The base is
  // carried so the consumer can prove the entry it is about to trust really
  // describes the texture bound to that sampler — a swizzle applied to the
  // wrong texture is a silent wrong picture, which is worse than no fix.
  // 0xFFFF = no data (not a texture-type entry, or a pre-450T trace).
  uint16_t texSwiz[16];
  uint32_t texBase[16];
  // iter-457: the bound texture's own DIMENSIONALITY and DEPTH, from the
  // same fetch entry. dword2 bits 26+ = (depth-1) for 2D/stacked and cube;
  // dword5 bits 9-10 = 0 1D / 1 2D-or-STACKED / 2 3D / 3 CUBE.
  //
  // WHY: a resolve record carries destSize but NO depth, so the executor
  // cannot tell a plain 2D destination from a stacked one at the moment it
  // creates it. This is the only place the guest states it. Measured on the
  // title fixture: 0b5be000 reports (depth 4, dim 1) at all 1,935 of its
  // binds -- one value, zero variance -- and 39 of its 40 resolves happen
  // AFTER the first such bind.
  uint8_t texDim[16];      // 0xFF = no data
  uint16_t texDepth[16];   // 0 = no data
};
HleState s_rs{};
// iter-458 (Astra 068): texture-census-only decode of each fetch slot's format
// and dimensions (the same fields the producer decodes), written only while
// SR_TEX_OWN_CENSUS is on.
uint8_t s_txcFetchFmt[16] = {};
uint32_t s_txcFetchDims[16] = {};
// The census's FIFO clock: commands published (advanced inside the queue lock)
// and commands executed (render thread). Every published command is executed
// exactly once in publish order, so one index names one command on both sides.
std::atomic<uint64_t> s_txcPub{0}, s_txcExec{0};
// Astra 069: the renderer's OWN recorder-window counters (render thread only),
// reset at the window's open and carried by the census's close edge, so the
// analyzer can conserve the recorded bindings against them.
sr::kernel::txc::WindowCounters s_txcWin{};
// Astra 070: the census's draw commands are EXACTLY the executor's draw arm
// (DrawVerticesUP included - isDrawType() omits it).
bool txcIsDrawCmd(RenderCommandType t) {
  return t == RenderCommandType::DrawVertices || t == RenderCommandType::DrawIndexedVertices ||
         t == RenderCommandType::DrawVerticesUP || t == RenderCommandType::BeginVertices;
}
// The census's part of every command's execution: its FIFO index and, inside
// the recorder window, the draw-command count. execute() and the draw-tap test
// seam share it, so neither can drift from the other.
void txcExecPrologue(const RenderCommand& c) {
  if (!sr::kernel::txc::enabled()) return;
  s_txcExec.fetch_add(1, std::memory_order_relaxed);
  if (txcIsDrawCmd(c.type) && traceRecorderOpen()) s_txcWin.drawCmds++;
}
// Astra 069: THE ONLY writer of the three slot bands (2D, 3D, cube), so every
// write is a census Resident. tools/test_txc_taps.py fails on a direct write.
void slotWrite(uint32_t band, uint32_t key, uint32_t slot, uint32_t source) {
  key &= 0x1FFFFFFFu;
  auto& m = band == sr::kernel::txc::kBand3D     ? s_texSlot3D
            : band == sr::kernel::txc::kBandCube ? s_texSlotCube
                                                 : s_texSlot;
  m[key] = slot;
  if (sr::kernel::txc::enabled())
    sr::kernel::txc::onResident(key, band, source, s_txcExec.load(std::memory_order_relaxed));
}
// ⚠ THE TWO RUNS ARE CONTIGUOUS IN MEMORY, NOT IN REGISTER INDEX. The 0x2100
// run is 26 dwords, so reg 0x2200 sits at block index 26 — NOT at 0x2200-0x2100
// (=256, i.e. byte 1024 of a 236-byte block, an out-of-bounds read into the
// next pool slot). MEASURED SYMPTOM that named it exactly: colorMask (0x2104)
// and the whole PA_CL_VPORT_* set (0x210F-0x2114) decoded perfectly while
// depth/blend/vte ALL read 0 — every correct field was in run 1, every zero
// field in run 2.
constexpr uint32_t kRsRun2Base = 26u;               // dwords into the block
constexpr uint32_t kRsBlockDwords = 236u / 4u;
inline uint32_t rsIdx1(uint32_t reg) { return reg - 0x2100u; }
inline uint32_t rsIdx2(uint32_t reg) { return kRsRun2Base + (reg - 0x2200u); }
inline uint32_t rsBE32(const uint8_t* p, uint32_t regIdx) {
  if (regIdx >= kRsBlockDwords) return 0;   // never read past the block
  const uint8_t* q = p + regIdx * 4u;
  return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) |
         (uint32_t(q[2]) << 8) | uint32_t(q[3]);
}
inline float rsBEf(const uint8_t* p, uint32_t regIdx) {
  const uint32_t v = rsBE32(p, regIdx);
  float f; std::memcpy(&f, &v, 4); return f;
}

uint8_t s_vsConst[kVsConstBytes];
uint8_t s_psConst[kPsConstBytes];
// iter-449v: packed (vsBool0 & 0xFF) | ((psBool0 & 0xFF) << 16) — the
// reference's SharedConstants.booleans formula (video.cpp:4299-4307).
uint32_t s_boolBits = 0;
bool s_constValid = false;

// Guest floats are BIG-ENDIAN; the translated shaders read the constant files
// as host-order float4s (the xlat path memcpys already-host-order register
// values into the very same offsets). Swap on the way in, on our own copy.
void swapConstsInto(uint8_t* dst, const uint8_t* src, uint32_t bytes) {
  for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
    dst[i + 0] = src[i + 3];
    dst[i + 1] = src[i + 2];
    dst[i + 2] = src[i + 1];
    dst[i + 3] = src[i + 0];
  }
}

// The guest's movie quad is a QUADLIST of 4 (prim=13). D3D12 has no quadlist,
// so expand to two triangles, v0,v1,v2 + v0,v2,v3 — the same winding Xenia's
// primitive processor and our own xlat path use.
constexpr uint32_t kQuadIdx[6] = {0, 1, 2, 0, 2, 3};

// Describe the draw for the backend, which does the EXPANSION (iter-444 S6e:
// the packed guest vertex must be mapped onto the translated VS's float4-per-
// attribute inputs, and only the backend has the VS layout + the Xenos format
// decoder). Everything handed over is HOST memory.
//
// ⚠ iter-444 S6f: this validates ONLY that the snapshot exists and is
// non-degenerate. It used to also require count==4 && stride>=20 — the movie
// quad's own shape — which silently refused every widened triangle-list draw
// BEFORE the backend saw it, and did so through the one path that records no
// reject reason ([S6_REJECT] read empty while noExpand climbed to 39,983).
// Shape validation belongs to hleSubmitDraw, which names its verdict.
bool buildDraw(const RenderCommand& c, sr::gfx::D3D12Backend::HleDraw& q) {
  const uint8_t* v;
  if ((c.draw.vertArena & kPoolResidentFlag) == kPoolResidentFlag) {
    // iter-449L resident draw: the tag names the WHOLE buffer; this draw
    // reads [start*stride, (start+count)*stride) of it. Bounds are checked
    // against the ENTRY, not trusted from the command — an out-of-range
    // resident draw is refused exactly like a missing snapshot.
    uint32_t entBytes = 0;
    const uint8_t* base = vbCacheData(c.draw.vertArena, &entBytes);
    if (!base || !c.draw.count || !c.draw.stride) return false;
    const uint64_t lo = (uint64_t)c.draw.start * c.draw.stride;
    const uint64_t hi = lo + (uint64_t)c.draw.count * c.draw.stride;
    if (hi > entBytes) return false;
    v = base + lo;
  } else {
    v = poolRead(c.draw.vertArena);
  }
  if (!v || !c.draw.count || !c.draw.stride) return false;
  q.vsHash = s_curVs;
  q.psHash = s_curPs;
  // iter-449c: producer attribution for the landing census (backend counts
  // pool-vs-backbuffer landings per source; the UI overlay question).
  q.srcType = (c.type == RenderCommandType::DrawVerticesUP)        ? 2u
              : (c.type == RenderCommandType::DrawIndexedVertices) ? 1u
                                                                   : 0u;
  // iter-450O: bit 8 is the strip-conversion marker, not a prim type —
  // strip it here so every downstream switch sees the real prim.
  q.primType = c.draw.primType & ~kPrimStripConvertedBit;
  q.fromStrip = (c.draw.primType & kPrimStripConvertedBit) != 0;
  q.srcCount = c.draw.count;
  q.srcStride = c.draw.stride;
  q.srcVerts = v;
  q.srcBytes = c.draw.vertBytes;
  q.declRaw = s_declCount ? s_declRaw : nullptr;
  q.declCount = s_declCount;
  q.vsConst = s_constValid ? s_vsConst : nullptr;
  q.psConst = s_constValid ? s_psConst : nullptr;
  q.boolBits = s_boolBits;   // iter-449v: shared[64] (g_Booleans)
  // ── iter-448: THE vte==0 GUARD, measured before it was written ────────────
  // SR_S6_STATE=1 alone broke the MOVIE: the dot tunnel rendered (early, before
  // any state block was published) but the title-card era collapsed to
  // distinct<=2 frames with noExpand=0 — nothing refused, everything
  // mis-transformed. The parked observation was exact: the movie presenter's
  // register block reads PA_CL_VTE_CNTL = 0x00000000, which is NEITHER
  // documented value (0x43F clip-space, 0x400 pixel-space) — the register is
  // UNWRITTEN on that path, and honouring it routes a clip-space quad through
  // the pixel-space c18 remap. Both real values share bit 0x400 (VTX_W0_FMT),
  // so that bit is the "this block was actually written" witness: without it,
  // fall back to the legacy transform for the draw. vteHist below measures the
  // classes live so this guard is a counted decision, not a guess.
  q.stateValid = s_rs.valid && s6StateEnabled() && (s_rs.vteCntl & 0x400u);
  {
    static uint64_t s_vteHist[4] = {};   // 0: ==0, 1: 0x400-class, 2: 0x43F-class, 3: other
    static uint64_t s_vteN = 0;
    const uint32_t v = s_rs.vteCntl;
    s_vteHist[v == 0 ? 0 : (v & 1u) ? 2 : (v & 0x400u) ? 1 : 3]++;
    if (++s_vteN % 200000 == 0)
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_VTE] draws by vte class: unwritten=%llu pixel(0x400)=%llu "
          "clip(0x43F)=%llu other=%llu",
          (unsigned long long)s_vteHist[0], (unsigned long long)s_vteHist[1],
          (unsigned long long)s_vteHist[2], (unsigned long long)s_vteHist[3]);
  }
  q.depthControl = s_rs.depthControl;
  q.blendControl = s_rs.blendControl;
  q.colorMask = s_rs.colorMask;
  q.vteCntl = s_rs.vteCntl;
  for (int i = 0; i < 3; i++) { q.vpScale[i] = s_rs.vpScale[i]; q.vpOffset[i] = s_rs.vpOffset[i]; }
  // iter-450T: 0xFF = no sampler data for this draw -> backend keeps the
  // fixed clamp+linear (index 7) it has always used.
  // iter-450UE FIX B: the swizzle rides the SAME validity bit — both come from
  // the one appended fetch file, so if that is absent neither is trustworthy.
  for (int i = 0; i < 16; i++) {
    q.texSwiz[i] = s_rs.sampValid ? s_rs.texSwiz[i] : uint16_t(0xFFFFu);
    q.texSwizBase[i] = s_rs.sampValid ? s_rs.texBase[i] : 0u;
  }
  for (int i = 0; i < 16; i++)
    q.sampSel[i] = s_rs.sampValid ? s_rs.sampSel[i] : uint8_t(0xFF);
  // iter-449u: ALL 16 fetch slots travel with the draw (was 3 — every
  // truncated slot resolved to the 1x1 white fallback; the dominant terrain
  // PS provably samples slots 0-7, 14, 15).
  // iter-450e DIAGNOSTIC (SR_TEXSLOT_WHITE=<hexkey>): substitute the 1x1
  // WHITE dummy for one texture key across all slots — the KILLS-IT for
  // "which sampled texture zeroes every world draw" (suspect: the never-
  // painted shadow map). Diagnostic only; never bake.
  static const uint32_t s_whiteKey = [] {
    const char* e = std::getenv("SR_TEXSLOT_WHITE");
    return e && e[0] ? (uint32_t)strtoul(e, nullptr, 16) & 0x1FFFFFFFu : 0u;
  }();
  // SR_TEXSLOT_WHITE=zero — substitute white for EVERY key whose upload
  // snapshot was all-zero bytes (the stale-capture class, e.g. lightmaps
  // snapshotted before the CPU filled them).
  static const bool s_whiteZero = [] {
    const char* e = std::getenv("SR_TEXSLOT_WHITE");
    return e && (e[0] == 'z' || e[0] == 'Z');
  }();
  // SR_TEXSLOT_WHITE=all — white-substitute EVERY texture (replicates the
  // replay's dummy-texture condition in live; the texture-theory kill shot).
  // ⚠ CONFOUNDED for scene verdicts: it also whitens RESOLVE textures, so the
  // composite quad samples white and the screen goes bright regardless of the
  // world pass. Use =u for the clean experiment.
  static const bool s_whiteAll = [] {
    const char* e = std::getenv("SR_TEXSLOT_WHITE");
    return e && (e[0] == 'a' || e[0] == 'A');
  }();
  // SR_TEXSLOT_WHITE=u — whiten UPLOADED textures only; resolve textures
  // (HDR scene, shadow map, depth) stay real. This is the replay's exact
  // texture condition (the replay replays resolves but has no uploads).
  static const bool s_whiteUploads = [] {
    const char* e = std::getenv("SR_TEXSLOT_WHITE");
    return e && (e[0] == 'u' || e[0] == 'U');
  }();
  // SR_TEXSLOT_WHITE=t — WITHIN-ROLL A/B: uploads-white toggling on a 5 s
  // wall-clock cadence. iter-450f: an observer-only roll reproduced the
  // "whiten flip" (the FE-era HDR goes non-zero at probe ~#1920 NATURALLY,
  // on the attract rotation's own schedule), so cross-roll whiten verdicts
  // are unattributable under DOGMA variance. Only a phase-correlated
  // LUM_PROBE delta inside ONE roll is a clean readout.
  static const bool s_whiteToggle = [] {
    const char* e = std::getenv("SR_TEXSLOT_WHITE");
    return e && (e[0] == 't' || e[0] == 'T');
  }();
  bool togglePhase = false;
  if (s_whiteToggle) {
    const long long sec =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    togglePhase = ((sec / 5) & 1) != 0;
    static std::atomic<int> s_lastPhase{-1};
    const int ph = togglePhase ? 1 : 0;
    if (s_lastPhase.exchange(ph, std::memory_order_relaxed) != ph)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[WHITETOGGLE] phase=%d (%s)", ph,
          ph ? "uploads WHITE" : "uploads REAL");
  }
  if (s_whiteAll) {
    for (uint32_t s = 0; s < 16; s++) {
      q.texSlot[s] = 0u; q.texSlot3D[s] = 0u; q.texSlotCube[s] = 0u;
    }
    return true;
  }
  auto beW =
      (s_whiteZero || s_whiteUploads || s_whiteToggle)
          ? s_backend.acquire() : sr::gfx::BackendPublication<sr::gfx::D3D12Backend>::Lease{};
  // Astra 069: the window's independent binding counters (the census tap
  // below is conserved against these).
  const bool txcWin = sr::kernel::txc::enabled() && traceRecorderOpen();
  if (txcWin) s_txcWin.drawsAtTap++;
  for (uint32_t s = 0; s < 16; s++) {
    // Both sides are normalised to the PHYSICAL address: the fetch constant
    // reports 0x0F807000 while Lock reports the 0x8-window VA 0x8F807000 for
    // the very same plane. Comparing raw values silently misses every time.
    const uint32_t tk = s_samplerTex[s] & 0x1FFFFFFFu;
    // iter-450UA: carry the KEY beside the resolved descriptor so the [DRAWID]
    // record can state "slot s: key K -> descriptor D" for every sampler,
    // including the ones that resolve to 0. Set before any of the diagnostic
    // white-out branches below, so the key is reported truthfully even when a
    // diagnostic has forced the descriptor to the white dummy.
    q.texKey[s] = tk;
    if (txcWin && tk) s_txcWin.bindingsSeen++;
    if (s_whiteKey && tk == s_whiteKey) { q.texSlot[s] = 0u; q.texSlot3D[s] = 0u; if (txcWin && tk) s_txcWin.whiteoutSkips++; continue; }
    if (s_whiteZero && beW && beW->hleTexWasZeroUpload(tk)) { q.texSlot[s] = 0u; q.texSlot3D[s] = 0u; if (txcWin && tk) s_txcWin.whiteoutSkips++; continue; }
    if ((s_whiteUploads || (s_whiteToggle && togglePhase)) && beW &&
        beW->hleTexWasUpload(tk)) { q.texSlot[s] = 0u; q.texSlot3D[s] = 0u; if (txcWin && tk) s_txcWin.whiteoutSkips++; continue; }
    auto it = s_texSlot.find(tk);
    q.texSlot[s] = (it == s_texSlot.end()) ? 0u : it->second;
    // iter-449x: a slot bound to a VOLUME key resolves through the 3D table
    // instead (shader tfetch dimension picks the range it actually reads).
    auto it3 = s_texSlot3D.find(s_samplerTex[s] & 0x1FFFFFFFu);
    q.texSlot3D[s] = (it3 == s_texSlot3D.end()) ? 0u : (uint8_t)it3->second;
    // iter-457 H1(b): the cube range (space2). Absent key -> 0 -> white dummy.
    auto itC = s_texSlotCube.find(tk);
    q.texSlotCube[s] = (itC == s_texSlotCube.end()) ? 0u : (uint8_t)itC->second;
    // iter-458 (Astra 068): the census's per-draw witness - the SetTexture key,
    // the fetch-file base/fmt/dims at this draw, and (inside the census) the
    // version the key's slot holds. A bind, not a proven executed fetch.
    if (tk && sr::kernel::txc::enabled())
      sr::kernel::txc::onDrawTex((uint32_t)s_cov.issued, s, tk, s_rs.texBase[s], s_txcFetchFmt[s],
                                 s_rs.sampValid && s_rs.texDim[s] <= 3u, s_rs.texDim[s] & 3u,
                                 s_rs.texDepth[s] > 1u, s_txcFetchDims[s], q.psHash,
                                 s_txcExec.load(std::memory_order_relaxed));
  }
  // iter-450f [KEYBIND]: live (ps, slot) truth for a queried key. The trace
  // census has a stale-binding blind spot (a slot set BEFORE the trace window
  // never re-appears as an in-window SetTexture), so only a live probe can
  // say which slots hold the key at draw time. Capped; era-agnostic (fires
  // only when the key is actually bound).
  static const uint32_t s_keyQ = [] {
    const char* e = std::getenv("SR_AUTOPSY_KEY");
    return e && e[0] ? (uint32_t)strtoul(e, nullptr, 16) & 0x1FFFFFFFu : 0u;
  }();
  if (s_keyQ) {
    uint32_t hitMask = 0;
    for (uint32_t s = 0; s < 16; s++)
      if ((s_samplerTex[s] & 0x1FFFFFFFu) == s_keyQ) hitMask |= 1u << s;
    if (hitMask) {
      static std::atomic<uint32_t> s_kbN{0};
      const uint32_t n = s_kbN.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 24 || (n & (n - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[KEYBIND] #%u key=%08X slotMask=%04X ps=%016llX slot0: tex2D=%u "
            "tex3D=%u keys={%08X,%08X,"
            "%08X,%08X,%08X,%08X,%08X,%08X|%08X,%08X,%08X,%08X,%08X,%08X,"
            "%08X,%08X}",
            n, s_keyQ, hitMask, (unsigned long long)s_curPs,
            q.texSlot[0], q.texSlot3D[0],
            s_samplerTex[0] & 0x1FFFFFFFu, s_samplerTex[1] & 0x1FFFFFFFu,
            s_samplerTex[2] & 0x1FFFFFFFu, s_samplerTex[3] & 0x1FFFFFFFu,
            s_samplerTex[4] & 0x1FFFFFFFu, s_samplerTex[5] & 0x1FFFFFFFu,
            s_samplerTex[6] & 0x1FFFFFFFu, s_samplerTex[7] & 0x1FFFFFFFu,
            s_samplerTex[8] & 0x1FFFFFFFu, s_samplerTex[9] & 0x1FFFFFFFu,
            s_samplerTex[10] & 0x1FFFFFFFu, s_samplerTex[11] & 0x1FFFFFFFu,
            s_samplerTex[12] & 0x1FFFFFFFu, s_samplerTex[13] & 0x1FFFFFFFu,
            s_samplerTex[14] & 0x1FFFFFFFu, s_samplerTex[15] & 0x1FFFFFFFu);
    }
  }
  return true;
}


// |resolve destinations that the guest has ALSO bound as a texture|. If this
// equals the destination count, the two keys agree and `destKnown=0` simply
// means the destinations have no host resource YET. If it is 0 while the guest
// demonstrably samples them (ENGINE_MAP 6g.4), the two derivations disagree and
// R5b would be copying into a key nothing looks up.
size_t resolveDestEverBound() {
  size_t n = 0;
  for (uint32_t d : s_resolveDests)
    if (s_boundTexKeys.count(d & 0x1FFFFFFFu)) n++;
  return n;
}

void dropCensus();

// iter-445 C1: SetRenderTarget commands since the previous Present = the frame's
// PASS COUNT. Free to collect and it is the number the render-target slice is
// sized from; the RT census measured ~33-35 binds/frame across 16 surfaces.
uint64_t s_passes = 0, s_passesWin = 0;

// The command types with NO case arm in execute(). Printed by name, because
// "N types dropped" and "SetRenderTarget dropped 308K times" order completely
// different work.
//
// iter-446 shortened this list from nine to three: Clear/ClearF gained decoding
// arms in iter-445, and SetRenderTarget/SetDepthStencilSurface/SetViewport/
// SetScissorRect gained arms in R3/R4. ⚠ "Has an arm" is NOT "is executed" —
// the RT arms create and bind host resources but no draw targets them yet, and
// the viewport/scissor arms only latch. Removing a type from this list means
// the type is DECODED, and nothing more.
//
// ⛔ The trailing clause "and Resolve is worse: it has no command type" was
// CARRIED FORWARD INTO A BUILD WHERE IT IS FALSE — 3bfe1dd gave Resolve a type,
// a producer and a name arm, and 3f7a5da gave it an executing arm. It printed
// on every zero-env run, on the line the file itself calls "the progress
// number", i.e. the line a future session reads to pick the next slice. Deleted
// by the pre-bake audit, and the count is now derived from the array rather
// than written as a literal so it cannot go stale the same way again.
void dropCensus() {
  static const RenderCommandType kDropped[] = {
      RenderCommandType::SetStreamSource, RenderCommandType::SetIndices,
      RenderCommandType::EndVertices};
  char l[420];
  int o = 0;
  uint64_t total = 0;
  for (RenderCommandType t : kDropped) {
    const uint64_t n = s_perType[(size_t)t].load(std::memory_order_relaxed);
    total += n;
    if (o < 360)
      o += snprintf(l + o, sizeof(l) - o, " %s=%llu", renderCommandName(t),
                    (unsigned long long)n);
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[S6_DROP] undecoded=%llu across %zu types (was 9 before iter-446 R3/R4; "
      "these still have NO arm at all) | passes=%llu (+%llu this window) |%s",
      (unsigned long long)total, std::size(kDropped),
      (unsigned long long)s_passes,
      (unsigned long long)s_passesWin, o ? l : " -");
  s_passesWin = 0;
}

// ── iter-456n (seventh adjudication): THE PREAMBLE IS APPLIED STATE ────────
// The trace preamble used to be built from commands as they were RECORDED,
// i.e. before execute() ran — so it captured what was SUBMITTED, including
// uploads the executor then refused on ownership. A replay starting from that
// preamble begins in a state the live renderer never actually held, which is
// exactly why "the replay shows facades" could not be read as "live should
// show facades".
//
// These maps are now written from the EXECUTOR, at the point an upload has
// genuinely succeeded (s_texSlot assigned), so the preamble is applied host
// texture state by construction. Both the executor and traceRecord run on the
// render thread, so no lock is needed and none is implied.
struct ShadowPayload {
  RenderCommand cmd{};
  std::vector<uint8_t> bytes;
  uint64_t seq = 0;          // apply order, monotonic across BOTH classes
};
// ⚠ ONE map, not two (eighth adjudication). Splitting retention into
// s_shadowUp and s_shadowUnlock and then emitting every upload followed by
// every unlock groups the preamble by COMMAND CLASS instead of by when the
// write actually happened. For a key applied through both classes — after
// physical reuse, or a descriptor/class transition — an OLDER Unlock then
// overwrote a NEWER Upload on replay, so the preamble was applied-state
// FILTERED but not applied-state EXACT.
//
// One record per normalized key, holding whichever command class wrote it
// LAST, plus a monotonic sequence so the preamble is emitted in apply order.
// The sequence also makes the output byte-deterministic without relying on
// map iteration order carrying any meaning it does not have.
std::map<uint32_t, ShadowPayload> s_shadowApplied;
uint64_t s_shadowSeq = 0;
RenderCommand s_shadowBind[20];
bool s_shadowBindValid[20] = {};
uint64_t s_shadowBytes = 0;
bool s_shadowOpen = true;   // false once the window opens: the trace is live

// ── THE TRACE ARMS, counted on APPLIED pixels ──────────────────────────────
// These moved here with the shadow. The contract they have always advertised is
// "armed means the bytes are genuinely RETAINED and will travel in the
// preamble" — which is only true now that the shadow is written from the
// executor's success path. Counting them at submit time meant an arm could fire
// on an upload the executor went on to refuse.
const uint64_t g_unlockArmWant = [] {
  const char* e = std::getenv("SR_HLE_TRACE_ARM_UNLOCKN");
  const long long v = e ? std::atoll(e) : 0;
  return (uint64_t)(v > 0 ? v : 0);
}();
uint64_t g_unlockHits = 0;
// slice 6: the CLASS-B arm. ARM_UNLOCKN counts bit-26 unlock-origin publishes,
// which cannot fire at all when Class A is off - and the director requires
// Class A OFF on the Class-B roll, so that arm is structurally unusable here.
// This arms on the fact THIS class is about: the Nth carried-descriptor resolve
// success. Default OFF.
const uint64_t g_resolveArmWant = [] {
  const char* e = std::getenv("SR_HLE_TRACE_ARM_RESOLVEN");
  const long long v = e ? std::atoll(e) : 0;
  return (uint64_t)(v > 0 ? v : 0);
}();
uint64_t g_resolveHits = 0;

// ── slice 6: THE PRE-TREATMENT ARM (SR_HLE_TRACE_ARM_PRERESOLVE=1) ─────────
// A control fixture has to start BEFORE the mechanism under test has done
// anything, or its preamble encodes the treatment and both arms inherit it —
// which is exactly why bv3 cannot discriminate. ARM_RESOLVEN opens AFTER a
// resolve has already succeeded and claimed.
//
// This arms on the command immediately BEFORE the first descriptor-valid
// Resolve EXECUTES. traceRecord() runs before execute(), so the checkpoint can
// precede the claim; the triggering Resolve is then retained as the first
// treatment event rather than being lost to the preamble.
//
// FAILS CLOSED. It refuses to open unless the state is genuinely pre-treatment:
// no Authority::Resolve claim exists anywhere, and the legacy s_resolveOwned set
// is empty. A fixture that cannot satisfy that is not a control, and banking one
// that merely looks like one is the failure this whole slice keeps finding.
// Read per call, not latched. A latched value cannot be driven by a test, and
// three rounds of "the mutation still passes" came from exactly that: the tests
// reached helpers while production's decisions sat behind statics nothing could
// reach. Read only at trace open, so re-reading costs nothing.
bool g_preTreatArm_() { return envArmEnabled(std::getenv("SR_HLE_TRACE_ARM_PRERESOLVE")); }
#define g_preTreatArm (g_preTreatArm_())
bool g_preTreatFired = false;
uint64_t g_preTreatRefused = 0;

// Exactly one arm may be active. Silently preferring one over another produced
// a fixture armed by a rule the operator did not choose, which is unfalsifiable
// from the artifact alone — so a conflict REFUSES to record instead.
const char* traceEnvGet(const char* n) { return std::getenv(n); }
// Recomputed per call rather than latched in a static initializer: a latched
// value could not be observed by any test, which is why restoring
// presence-counting here passed the whole suite. The exported
// productionArmCount() below is what binds it.
// ⚠ iter-458 (H-REC): this is SIX getenv calls (~2.4 us each against a
// 106-variable environment). traceRecord() used to call it for EVERY command
// while the recorder was enabled but not yet open - 17.5 us/command, which
// capped the render thread at ~58k commands/s against the ~208k/s gameplay
// needs (idle1 drained 56.6k-59.3k/s, presents fell to ~8/s). The recorder now
// evaluates it once per recorder lifecycle (Recorder::armConflict).
int g_armCountNow() { return countEnabledArms(&traceEnvGet); }
bool g_armConflict() { return traceArmsConflict(g_armCountNow()); }

bool preTreatStateIsClean() {
  if (!s_resolveOwned.empty()) return false;
  bool anyResolve = false;
  unlockOwnership().forEachClaim(
      [&](uint32_t, uint64_t, uint64_t, uint64_t, Authority a) {
        if (a == Authority::Resolve) anyResolve = true;
      });
  return !anyResolve;
}
const uint32_t g_texKeyArm = [] {
  const char* e = std::getenv("SR_HLE_TRACE_ARM_TEXKEY");
  return (e && e[0]) ? (uint32_t)(std::strtoul(e, nullptr, 16) & 0x1FFFFFFFu)
                     : 0u;
}();
const uint64_t g_texKeyWant = [] {
  const char* e = std::getenv("SR_HLE_TRACE_ARM_TEXKEY");
  const char* c = e ? std::strchr(e, ':') : nullptr;
  const long long v = c ? std::atoll(c + 1) : 0;
  return (uint64_t)(v > 0 ? v : 1);
}();
uint64_t g_texKeyHits = 0;
uint32_t g_texKeyBytes = 0;

// Called by execute() ONLY after the pixels reached the host texture.
void shadowNoteApplied(const RenderCommand& c, const uint8_t* px,
                       uint32_t bytes, uint32_t key) {
  if (!s_shadowOpen || !px || !bytes) return;
  ShadowPayload& e = s_shadowApplied[key & 0x1FFFFFFFu];
  s_shadowBytes -= e.bytes.size();
  e.cmd = c;
  e.bytes.assign(px, px + bytes);
  e.seq = ++s_shadowSeq;      // LAST writer of this key wins, whatever class
  s_shadowBytes += e.bytes.size();

  if (c.type == RenderCommandType::TextureUpload) {
    if (g_unlockArmWant && (c.textureUpload.fmtBits & kUploadUnlockOriginBit)) {
      g_unlockHits++;
      if (g_unlockHits <= g_unlockArmWant)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[TRACE_UNLOCKN] HIT #%llu/%llu key=%08X fmtBits=%08X bytes=%u "
            "digest=%016llX APPLIED and retained in the recorder shadow",
            (unsigned long long)g_unlockHits,
            (unsigned long long)g_unlockArmWant, c.textureUpload.key,
            c.textureUpload.fmtBits, bytes,
            (unsigned long long)sr::gfx::ShaderCache::hashUcode(
                e.bytes.data(), e.bytes.size()));
    }
    if (g_texKeyArm && key == g_texKeyArm) {
      g_texKeyHits++;
      g_texKeyBytes = bytes;
      if (g_texKeyHits <= g_texKeyWant ||
          (g_texKeyHits & (g_texKeyHits - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[TRACE_TEXKEY] HIT #%llu/%llu key=%08X bytes=%u digest=%016llX "
            "APPLIED and retained in the recorder shadow",
            (unsigned long long)g_texKeyHits, (unsigned long long)g_texKeyWant,
            g_texKeyArm, g_texKeyBytes,
            (unsigned long long)sr::gfx::ShaderCache::hashUcode(
                e.bytes.data(), e.bytes.size()));
    }
  }

  static bool s_capWarned = false;
  if (!s_capWarned && s_shadowBytes > (uint64_t)512 * 1024 * 1024) {
    s_capWarned = true;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[HLE_TRACE] state shadow exceeds 512MB (%llu keys) - preamble will "
        "be large",
        (unsigned long long)s_shadowApplied.size());
  }
}

// ── THE FINAL CAPTURE DISPOSITION (slice 6, director's design item 3) ──────
// `captureVerdict()` is only the OWNERSHIP opinion. It is not what happened:
// with Class B OFF, `captureOk` can be true and the legacy permanent
// `s_resolveOwned` check still blocks the upload further down. A witness that
// records the verdict therefore cannot attribute a refusal to its SOURCE, which
// is the whole point of an OFF/ON control — in both arms the pixels can be
// equal while the mechanism that produced them differs.
//
// So the disposition records what the executor actually DID, and names which
// rule did it.

const char* captureDispositionName(CaptureDisposition d) {
  switch (d) {
    case CaptureDisposition::Applied:                 return "Applied";
    case CaptureDisposition::RefusedLegacyResolveSet:  return "RefusedLegacyResolveSet";
    case CaptureDisposition::RefusedResolveEpoch:      return "RefusedResolveEpoch";
    case CaptureDisposition::RefusedUnlockEpoch:       return "RefusedUnlockEpoch";
    case CaptureDisposition::RefusedNoOwnerOther:      return "RefusedNoOwnerOther";
    case CaptureDisposition::UploadFailed:             return "UploadFailed";
  }
  return "?";
}

// Default OFF and additive: with it off nothing is hashed and no digest moves,
// so every existing byte-identity bar is untouched by its presence.
bool captureDispositionEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_CAPTURE_DISPOSITION");
    return e && e[0] == '1';
  }();
  return s_on;
}

uint64_t s_dispDigest = 0, s_dispN = 0;
uint64_t s_dispByKind[6] = {};

// The director's tuple: ordinal, key, signature, generation, priorAuthority,
// disposition, refusalSource, payloadHash, resultingSlot. `refusalSource` is
// folded in as the disposition itself — it is the field that names the rule.
void noteDisposition(uint32_t key, uint64_t sig, uint64_t gen,
                     Authority priorAuth, CaptureDisposition disp,
                     uint64_t payloadHash, uint32_t slot) {
  if (!captureDispositionEnabled()) return;
  const uint64_t parts[8] = {++s_dispN, (uint64_t)(key & 0x1FFFFFFFu), sig, gen,
                             (uint64_t)priorAuth, (uint64_t)disp, payloadHash,
                             (uint64_t)slot};
  uint64_t h = s_dispDigest;
  for (uint64_t x : parts) {
    h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
  }
  s_dispDigest = h;
  s_dispByKind[(uint32_t)disp]++;
  if (s_dispN <= 64)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S6_DISPOSITION] #%llu key=%08X sig=%016llX gen=%llu prior=%s -> %s "
        "payload=%016llX slot=%u",
        (unsigned long long)s_dispN, key & 0x1FFFFFFFu,
        (unsigned long long)sig, (unsigned long long)gen,
        authorityName(priorAuth), captureDispositionName(disp),
        (unsigned long long)payloadHash, slot);
}


// ── iter-456p Step 3A: THE PERSISTENT VERTEX-BUFFER RESOURCE STORE ─────────
// Render-thread side. Keyed by the guest VB OBJECT VA, because that is what a
// SetStreamSource binding names; the physical base, size, endian, alias,
// generation and digest all travel inside the entry so a consumer can tell two
// generations of the same address apart.
//
// ⛔ NOTHING READS THIS YET. Step 3A is capture substrate only — no stream is
// bound and no draw changes shape. It exists so a replay can rebuild the
// resource before any binding could consume it, which is what makes the later
// gate OFF/ON comparison a within-fixture A/B rather than a re-record.
struct VbResource {
  uint32_t key = 0, sizeBytes = 0, endian = 0, alias = 0, generation = 0;
  uint64_t digest = 0;
  std::vector<uint8_t> bytes;   // ORIGINAL guest byte order
};
std::mutex s_vbResMu;
std::unordered_map<uint32_t, VbResource> s_vbResources;
std::atomic<uint64_t> s_vbResBuilt{0}, s_vbResBytes{0};
std::atomic<uint64_t> s_vbResBadDigest{0}, s_vbResNoPayload{0};


// ── iter-457 gates 3+4: THE EXPLICIT ADMISSION MAP ─────────────────────────
// The director rejected both shapes I proposed (plumbing trace metadata through
// HleDraw, and a positional join) and specified this instead:
//
//   "Build an explicit replay-only admission map joining each trace record to
//    the ordinal assigned at the actual hleSubmitDraw call ... This is an
//    observed join at the admission event - not a sequence assumption."
//
// A positional join would have been unsafe anyway: 6,904 trace draw records
// produce 6,073 admitted ordinals, so 831 do not correspond and any
// position-based mapping would silently misattribute them.
//
// REPLAY-ONLY BY CONSTRUCTION. The live path never sets a trace-record ordinal,
// so it stays kAdmitRecUnknown and no production behaviour reads it. The map is
// written only when SR_DRAW_ADMIT_MAP names a file.
static constexpr uint64_t kAdmitRecUnknown = ~0ull;
static std::atomic<uint64_t> s_admitTraceRec{kAdmitRecUnknown};

void setTraceRecordOrdinal(uint64_t rec) {
  s_admitTraceRec.store(rec, std::memory_order_relaxed);
}

static std::FILE* admitMapFile() {
  static std::FILE* f = [] () -> std::FILE* {
    const char* p = std::getenv("SR_DRAW_ADMIT_MAP");
    if (!p || !p[0]) return nullptr;
    std::FILE* h = std::fopen(p, "wb");
    if (h) std::fprintf(h, "# rec ord outcome\n");
    return h;
  }();
  return f;
}

// One row per ISSUED draw. `ord` is the admitted hleSubmitDraw ordinal, or "-"
// when the draw was excluded - and every exclusion carries a NAME, so
// conservation can be proved rather than assumed.
static void admitMapRow(uint64_t rec, uint64_t ord, const char* outcome) {
  std::FILE* f = admitMapFile();
  if (!f) return;
  if (rec == kAdmitRecUnknown) std::fprintf(f, "- ");
  else                          std::fprintf(f, "%llu ", (unsigned long long)rec);
  if (ord == kAdmitRecUnknown) std::fprintf(f, "- %s\n", outcome);
  else std::fprintf(f, "%llu %s\n", (unsigned long long)ord, outcome);
}

void admitMapFlush() {
  if (std::FILE* f = admitMapFile()) std::fflush(f);
}

// ── iter-457: THE RESOLVE-EXECUTION SIDECAR ───────────────────────────────
// Answers one question the trace cannot: did this resolve actually produce a
// product? The offline scanout router needs it because structural
// eligibility is NOT execution - the backend still refuses for a missing
// source, a depth gate, bad dimensions, a closed list, an allocation failure
// or slot exhaustion, and a product exists only when hleResolveToTexture
// returns a nonzero slot. Without this the router assumes success, and an
// assumed product can nominate a frame the draw never reached.
//
// ONE ROW PER RESOLVE COMMAND, whatever happens to it, each with a NAMED
// outcome - so conservation is provable rather than assumed (the same
// discipline the admission map above is built on). Observe-only,
// replay-only, and written ONLY when SR_RESOLVE_EXEC_MAP names a file.
static std::FILE* resolveExecMapFile() {
  static std::FILE* f = [] () -> std::FILE* {
    const char* p = std::getenv("SR_RESOLVE_EXEC_MAP");
    if (!p || !p[0]) return nullptr;
    std::FILE* h = std::fopen(p, "wb");
    if (h) std::fprintf(h, "# rec executed outcome\n");
    return h;
  }();
  return f;
}

static void resolveExecMapRow(uint64_t rec, bool executed,
                              const char* outcome, uint32_t why) {
  std::FILE* f = resolveExecMapFile();
  if (!f) return;
  char buf[64];
  if (why) std::snprintf(buf, sizeof buf, "%s:%u", outcome, why);
  else     std::snprintf(buf, sizeof buf, "%s", outcome);
  if (rec == kAdmitRecUnknown)
    std::fprintf(f, "- %d %s\n", executed ? 1 : 0, buf);
  else
    std::fprintf(f, "%llu %d %s\n", (unsigned long long)rec,
                 executed ? 1 : 0, buf);
}

// The row is emitted by a DESTRUCTOR, not by a call at the end of the case.
// The Resolve handler is long and has several exits; a trailing call would
// silently drop whichever path someone adds next, and a resolve missing from
// the map reads to the router as "never executed" - a wrong answer that
// looks like a conservative one.
struct ResolveExecRow {
  uint64_t rec;
  bool executed = false;
  const char* outcome = "noBackend";
  uint32_t why = 0;
  ~ResolveExecRow() { resolveExecMapRow(rec, executed, outcome, why); }
};

void resolveExecMapFlush() {
  if (std::FILE* f = resolveExecMapFile()) std::fflush(f);
}

// ── Astra 086: THE GUEST-CLEAR SUBMISSION BOUNDARY ─────────────────────────
// The ClearF arm's ONLY exit to the backend. With the suite's tap installed
// the tap receives exactly the arguments hleClearBound would have received,
// so "the executor submits the effective (masked) flags" is asserted at the
// call itself rather than inferred from telemetry recorded beside it.
void submitGuestClear(uint32_t flags, const float rgba[4], float z,
                      uint8_t stencil, bool subRect) {
  if (s_clearSubmitTap) {
    s_clearSubmitTap(flags, rgba, z, stencil);
    return;
  }
  if (auto be = s_backend.acquire()) {
    if (subRect) s_clearRectApprox++;
    be->hleClearBound(flags, rgba, z, stencil);
  }
}

void execute(const RenderCommand& c) {
  // ── iter-445 C4: THE LAZY FRAME OPEN ──────────────────────────────────────
  // This is the change the whole sequence exists for: with SR_HLE_FRAME_LIST=1
  // the frame's D3D12 command list is OPEN while these commands run, so a
  // handler can actually record. No-op with the gate off, and idempotent.
  // noteWindowCommand is the positive evidence the window widened — it reads
  // exactly 0 with the gate off.
  if (auto beW = s_backend.acquire()) {
    beW->ensureFrameOpen();
    beW->noteWindowCommand();
  }
  const uint32_t ti = static_cast<uint32_t>(c.type);
  if (ti < static_cast<uint32_t>(RenderCommandType::Count))
    s_perType[ti].fetch_add(1, std::memory_order_relaxed);
  if (c.type == RenderCommandType::SetRenderTarget) { s_passes++; s_passesWin++; }
  txcExecPrologue(c);

  switch (c.type) {
    case RenderCommandType::Present: {
      // S3: THE frame boundary is the guest's own Swap, and this thread owns
      // the device. Present here, then publish the count so the guest thread
      // waiting in the Swap hook is released (one frame in flight).
      auto be = s_backend.acquire();
      // ── iter-448: OFFSCREEN→SCREEN ROUTING, the guest's own way ──────────
      // The Swap hook decoded the front-buffer object's embedded fetch
      // constant: frontKey names the pixel memory THIS present scans out. If
      // that key is in our texture registry (a resolve destination we
      // populated), we hold the guest's frame — the census counts it, and
      // SR_HLE_PRESENT_FRONT (BAKED DEFAULT-ON since iter-449o; =0 keeps the
      // A/B) blits it to the backbuffer.
      // Refusals are counted BY NAME, never guessed around.
      {
        static uint64_t s_fkNone = 0, s_fkMiss = 0, s_fkHit = 0, s_fkShown = 0;
        static uint64_t s_fkN = 0;
        uint32_t slot = 0;
        if (!c.present.frontKey) s_fkNone++;
        else {
          auto it = s_texSlot.find(c.present.frontKey);
          if (it == s_texSlot.end()) s_fkMiss++;
          else { s_fkHit++; slot = it->second; }
        }
        // ⚠ hlePresentFront takes the KEY (m_hleTextures is keyed by normalized
        // guest address); the slot is only the census' hit-witness. The first
        // build passed `slot` here and refused every blit with
        // "key=00000013 not in m_hleTextures" — the named refusal caught it in
        // one roll.
        bool provShown = false;
        if (slot && be && be->isInitialized() && presentFrontEnabled()) {
          if (be->hlePresentFront(c.present.frontKey, c.present.frontDims)) {
            s_fkShown++;
            provShown = true;
          }
        }
        // Provenance ring (observe-only, director-authorized): finalize this frame's
        // record at the Present boundary. Reasons: 0 shown, 1 no front key, 2 key not
        // in the registry, 3 blit refused inside hlePresentFront, 4 gate off.
#if SR_ORACLE
        if (sr::gfx::prov::enabled()) {
          uint32_t rw = 0, rh = 0, rfmt = 0, rslot = 0;
          const void* rres = be ? be->hleTexHostPtr(c.present.frontKey, &rfmt, &rw,
                                                    &rh, &rslot) : nullptr;
          const uint32_t reason = provShown ? 0u
                                : !c.present.frontKey ? 1u
                                : !slot ? 2u
                                : presentFrontEnabled() ? 3u : 4u;
          sr::gfx::prov::ring().onPresent(s_fkN + 1, c.present.frontKey, rw, rh,
                                          slot ? slot : rslot, rres, rfmt,
                                          provShown, reason);
        }
#endif
        if (++s_fkN % 600 == 0)
          SR_LOG_BOOT(sr::LogSeverity::Info,
              "[S6_FRONT] presents=%llu key none=%llu miss=%llu HIT=%llu "
              "shown=%llu (gate=%d) key=%08X dims=%08X",
              (unsigned long long)s_fkN, (unsigned long long)s_fkNone,
              (unsigned long long)s_fkMiss, (unsigned long long)s_fkHit,
              (unsigned long long)s_fkShown, presentFrontEnabled() ? 1 : 0,
              c.present.frontKey, c.present.frontDims);
      }
      if (be && ownPresentEnabled() && be->isInitialized()) {
        be->presentPlaceholder(s_submitted.load(std::memory_order_relaxed));
        s_submitted.fetch_add(1, std::memory_order_relaxed);
      }
      const uint64_t p = s_present.fetch_add(1, std::memory_order_release) + 1;
      {
        // Release any guest thread blocked in waitForPresent. The lock is taken
        // only to pair with the waiter's predicate check — the counter itself
        // is atomic.
        std::lock_guard<std::mutex> lk(s_presentMu);
      }
      s_presentCv.notify_all();
      // ── iter-445 S1: CADENCE + WINDOWED DELTAS ──────────────────────────
      // 600 presents fires at 1200 and 1800; the front-end era is presents
      // ~1260-1440, so the era whose numbers decide the whole plan produced NOT
      // ONE report line. Run totals also hide era structure by construction —
      // a cumulative counter cannot say "the FE window drew 25 UP draws/frame
      // and staged none". 60 presents = one line per second of guest time.
      // iter-445 C1: SR_FRAME_LIST_EVERY overrides the window so the fixture
      // (60 presents) can report every present without a 200 s boot.
      static const uint64_t s_every = [] {
        const char* e = std::getenv("SR_FRAME_LIST_EVERY");
        const long v = e ? std::atol(e) : 0;
        return (v > 0) ? (uint64_t)v : 60ull;
      }();
      if (p <= 4 || p % s_every == 0) {
        // S6a: THE COVERAGE LINE. pct is the phase's progress number; the
        // reason breakdown names what to build next without any eyeballing.
        auto beC = s_backend.acquire();
        const uint64_t rec = beC ? beC->hleDrawsRecorded() : 0;
        const uint64_t drp = beC ? beC->hleDrawsDropped() : 0;
        const uint64_t hld = beC ? beC->hleDrawsHeld() : 0;
        const double pct = s_cov.issued
            ? 100.0 * double(rec) / double(s_cov.issued) : 0.0;
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_COVER] recorded=%llu/%llu (%.3f%%) staged=%llu dropped=%llu "
            "held=%llu | skipped: noVerts=%llu "
            "notQuad=%llu noPlanes=%llu noBackend=%llu noExpand=%llu | wide=%d",
            (unsigned long long)rec, (unsigned long long)s_cov.issued,
            pct, (unsigned long long)s_cov.staged, (unsigned long long)drp,
            (unsigned long long)hld, (unsigned long long)s_cov.noVerts,
            (unsigned long long)s_cov.notQuad,
            (unsigned long long)s_cov.noPlanes,
            (unsigned long long)s_cov.noBackend,
            (unsigned long long)s_cov.noExpand, s6WideEnabled() ? 1 : 0);
        // iter-445 S1: THE SAME NUMBERS AS DELTAS OVER THIS WINDOW ONLY, plus
        // the pool state that decides whether the next slice can afford to
        // publish 4x more draws (every draw-with-vertices costs FIVE blocks:
        // decl + vsConst + psConst + registers + verts). A cumulative peak
        // cannot say when the pressure happened; this can.
        {
          static Coverage s_prev{};
          static uint64_t s_prevRec = 0, s_prevBindH = 0, s_prevBindM = 0;
          static uint64_t s_prevPresent = 0;
          const uint64_t dPresent = p - s_prevPresent;
          const ArenaStats bs = blockStats();
          const ArenaStats as = arenaStats();
          const uint64_t dIssued = s_cov.issued - s_prev.issued;
          const uint64_t dHit = s_bindHit - s_prevBindH;
          const uint64_t dMiss = s_bindMiss - s_prevBindM;
          SR_LOG_BOOT(sr::LogSeverity::Info,
              "[S6_WIN] p=%llu(+%llu) issued=+%llu rec=+%llu noVerts=+%llu "
              "notQuad=+%llu | bind hit=+%llu miss=+%llu (%.1f%% miss) | "
              "blocks inFlight=%u peak=%u rej=%llu | arena inFlight=%u peak=%u "
              "rej=%llu",
              (unsigned long long)p, (unsigned long long)dPresent,
              (unsigned long long)dIssued,
              (unsigned long long)(rec - s_prevRec),
              (unsigned long long)(s_cov.noVerts - s_prev.noVerts),
              (unsigned long long)(s_cov.notQuad - s_prev.notQuad),
              (unsigned long long)dHit, (unsigned long long)dMiss,
              (dHit + dMiss) ? 100.0 * double(dMiss) / double(dHit + dMiss) : 0.0,
              bs.inFlight, bs.peak, (unsigned long long)bs.rejects,
              as.inFlight, as.peak, (unsigned long long)as.rejects);
          s_prev = s_cov;
          s_prevRec = rec;
          s_prevBindH = s_bindHit;
          s_prevBindM = s_bindMiss;
          s_prevPresent = p;
        }
        // WHY a staged draw was refused. Eight named causes beat one bucket:
        // "vbFull" and "noVs" name completely different next slices.
        if (s_cov.noExpand) {
          char rl[256]; int ro = 0;
          for (uint32_t i = 0; i < kRejectKinds && ro < 200; i++)
            if (s_reject[i])
              ro += snprintf(rl + ro, sizeof(rl) - ro, " %s=%llu",
                  i == kRejectNoSnapshot
                      ? "noSnapshot"
                      : sr::gfx::D3D12Backend::hleRejectName(
                            (sr::gfx::D3D12Backend::HleReject)i),
                  (unsigned long long)s_reject[i]);
          SR_LOG_BOOT(sr::LogSeverity::Info, "[S6_REJECT]%s", ro ? rl : " -");
        }
        // THE CENSUS that chooses the next slice: which topologies does the
        // guest actually draw, and how large are those draws?
        {
          char pl[400]; int po = 0;
          for (uint32_t i = 0; i < kPrimKinds && po < 340; i++)
            if (s_primN[i])
              po += snprintf(pl + po, sizeof(pl) - po,
                  " p%u:n=%llu avgV=%llu maxV=%llu", i,
                  (unsigned long long)s_primN[i],
                  (unsigned long long)(s_primVerts[i] / s_primN[i]),
                  (unsigned long long)s_primMaxV[i]);
          if (po) SR_LOG_BOOT(sr::LogSeverity::Info, "[S6_PRIM]%s", pl);
        }
        // ── iter-445 C1: THE DROP CENSUS IS FREE ────────────────────────
        // s_perType[] already counts EVERY command at the head of execute(),
        // and these nine types have no case arm at all — they fall entirely to
        // `default:`. So for them dropped == typeCount() BY CONSTRUCTION; there
        // is nothing to add but the report. This is the list the open-list
        // refactor exists to shorten, so it is the progress number for it.
        dropCensus();
        if (auto beL = s_backend.acquire()) {
          beL->frameListReport(p);
          beL->hleSurfaceReport(p);
          // r5: the LIVE cube summary site. Suppressed while a replay owns
          // the line (replayTrace emits the one replay-final summary).
          if (!s_replayActive.load(std::memory_order_acquire))
            beL->hleCubeSumReport("periodic");
        }
        // iter-446 R3/R4. `bound` vs `seen` is the honest pair: a bind that did
        // not resolve to a host resource is counted as seen and NOT as bound,
        // so a refusal can never read as a success. The viewport/scissor lines
        // report the LATCHED state, which is not yet applied to any draw.
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_RTBIND] setRT=%llu bound=%llu | setDS=%llu bound=%llu | "
            "setVP=%llu latched=%dx%d@%d,%d valid=%d | setSC=%llu "
            "latched=(%d,%d)-(%d,%d) valid=%d | clearF=%llu subRectWidened=%llu "
            "colourMissing=%llu colourMalformed=%llu targetsMasked=%llu",
            (unsigned long long)s_setRtSeen, (unsigned long long)s_setRtBound,
            (unsigned long long)s_setDsSeen, (unsigned long long)s_setDsBound,
            (unsigned long long)s_setVpSeen, (int)s_viewport.w, (int)s_viewport.h,
            (int)s_viewport.x, (int)s_viewport.y, s_viewport.valid ? 1 : 0,
            (unsigned long long)s_setScSeen, s_scissor.l, s_scissor.t,
            s_scissor.r, s_scissor.b, s_scissor.valid ? 1 : 0,
            (unsigned long long)s_clearFSeen,
            (unsigned long long)s_clearRectApprox,
            (unsigned long long)s_clearFColourMissing,
            (unsigned long long)s_clearFColourMalformed,
            (unsigned long long)s_clearFTargetsMasked);
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_RESOLVE] n=%llu | distinct src=%zu dest=%zu | srcPooled=%llu destKnown=%llu destEverBound=%zu/%zu boundKeys=%zu | EXEC copies=%llu owned=%zu sampled=%llu skip(rect=%llu sliced=%llu other=%llu) | depthSrc=%llu clearRT=%llu clearZ=%llu",
            (unsigned long long)s_resolveSeen, s_resolveSrcs.size(),
            s_resolveDests.size(), (unsigned long long)s_resolveSrcPooled,
            (unsigned long long)s_resolveDestKnown, resolveDestEverBound(),
s_resolveDests.size(), s_boundTexKeys.size(),
            (unsigned long long)s_resolveExec, s_resolveOwned.size(),
(unsigned long long)s_resolveBindHit,
(unsigned long long)s_resolveSkipRect,
(unsigned long long)s_resolveSkipSliced,
(unsigned long long)s_resolveSkipOther,
(unsigned long long)s_resolveDepth,
            (unsigned long long)s_resolveClearRT,
            (unsigned long long)s_resolveClearZ);
        // iter-448: NAME the destination keys, once per report. The routing
        // design turns on ONE membership question — is the guest's Swap front
        // buffer (82998720/839F5CF0) a resolve DESTINATION? — and the counters
        // above cannot answer it. 16 keys fit in one line; print them.
        {
          char dl[900]; int dn = 0;
          for (uint32_t d : s_resolveDests) {
            const auto it2 = s_destSkips.find(d & 0x1FFFFFFFu);
            const DestSkips ds = (it2 == s_destSkips.end()) ? DestSkips{}
                                                            : it2->second;
            if (dn < 820)
              dn += snprintf(dl + dn, sizeof(dl) - dn, " %08X(e%llu/r%llu/s%llu)",
                             d, (unsigned long long)ds.exec,
                             (unsigned long long)ds.rect,
                             (unsigned long long)ds.sliced);
          }
          SR_LOG_BOOT(sr::LogSeverity::Info, "[S6_RESOLVE_DESTS]%s", dn ? dl : " -");
        }
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[HLE_VIDEO] Present #%llu dev=%08X | submitted=%llu enq=%llu "
            "drained=%llu maxDepth=%u",
            (unsigned long long)p, c.present.device,
            (unsigned long long)s_submitted.load(std::memory_order_relaxed),
            (unsigned long long)s_enqueued.load(std::memory_order_relaxed),
            (unsigned long long)s_drained.load(std::memory_order_relaxed),
            s_maxDepth.load(std::memory_order_relaxed));
      }
      if (p % 600 == 0) logArenaWaitSum("periodic");
      break;
    }
    case RenderCommandType::TextureUnlock: {
      // S4.2 snapshotted the pixels; S4.4 uploads them here — still without
      // touching guest memory, because arenaRead hands back HOST bytes.
      const auto& a = c.textureUnlock;
      if (a.bytes) {
        auto be = s_backend.acquire();
        if (be && be->isInitialized() && hleDrawEnabled()) {
          if (const uint8_t* px = arenaRead(a.arenaOffset)) {
            const uint32_t slot =
                be->hleUploadHostTexture(a.texture, px, a.pitch, a.width, a.height);
            if (slot) {
              const uint32_t k8key = a.texture & 0x1FFFFFFFu;
              slotWrite(sr::kernel::txc::kBand2D, k8key, slot, sr::kernel::txc::kFromUnlock);
              shadowNoteApplied(c, px, a.bytes, k8key);   // APPLIED, not submitted
              // Both Unlock routes share ONE ownership model: the legacy k_8
              // path claims through the same state machine as the widened one.
              unlockOwnership().claim(
                  k8key, UnlockOwnership::signature(
                             /*fmtBits=*/2u,
                             (a.width << 16) | (a.height & 0xFFFFu), a.pitch));
            }
          }
        }
        arenaRelease(a.arenaOffset);
      }
      break;
    }
    case RenderCommandType::SetRenderState: {
      const auto& a = c.setRenderState;
      if (a.bytes >= 236u) {
        if (const uint8_t* p = blockRead(a.blockOffset)) {
          s_rs.colorMask    = rsBE32(p, rsIdx1(0x2104));
          s_rs.vpScale[0]   = rsBEf (p, rsIdx1(0x210F));
          s_rs.vpOffset[0]  = rsBEf (p, rsIdx1(0x2110));
          s_rs.vpScale[1]   = rsBEf (p, rsIdx1(0x2111));
          s_rs.vpOffset[1]  = rsBEf (p, rsIdx1(0x2112));
          s_rs.vpScale[2]   = rsBEf (p, rsIdx1(0x2113));
          s_rs.vpOffset[2]  = rsBEf (p, rsIdx1(0x2114));
          s_rs.depthControl = rsBE32(p, rsIdx2(0x2200));
          s_rs.blendControl = rsBE32(p, rsIdx2(0x2201));
          s_rs.vteCntl      = rsBE32(p, rsIdx2(0x2206));
          s_rs.valid = true;
          // iter-450T: the appended 768-byte texture-fetch file (dev+1024,
          // 16 slots x 24B... actually 32 slots x 24B share the region; we
          // decode the first 16 the draw path binds). IDA-verified home of
          // LIVE sampler state: the ~20 SetSamplerState setters RMW these
          // dwords in place and SetTexture preserves the sampler-owned bits
          // (keep-masks dw0 0x3FFC00 / dw3 0x7FF80000). Decode = the xlat
          // path's exact rule (d3d12_backend.cpp:6670-6683): dword0 bits
          // 10-12/13-15 clamp_x/y (>=2 = clamp family), dword3 bits 19-20
          // mag filter (0=point). Old traces carry bytes=236 -> sampValid
          // stays false and the fixed clamp+linear behavior is unchanged.
          if (a.bytes >= 236u + 768u) {
            const uint8_t* tf = p + 236u;
            for (uint32_t t = 0; t < 16u; t++) {
              const uint8_t* f = tf + t * 24u;
              const uint32_t d0 = (uint32_t(f[0]) << 24) | (uint32_t(f[1]) << 16) |
                                  (uint32_t(f[2]) << 8) | f[3];
              const uint32_t d3 = (uint32_t(f[12]) << 24) | (uint32_t(f[13]) << 16) |
                                  (uint32_t(f[14]) << 8) | f[15];
              const uint32_t clampX = (d0 >> 10) & 7u;
              const uint32_t clampY = (d0 >> 13) & 7u;
              const uint32_t magF = (d3 >> 19) & 3u;
              s_rs.sampSel[t] = uint8_t(((clampX >= 2u) ? 4u : 0u) |
                                        ((clampY >= 2u) ? 2u : 0u) |
                                        ((magF != 0u) ? 1u : 0u));
              // iter-450UE FIX B: the 12-bit texture swizzle lives in the SAME
              // dword 3, bits 1-12 (xenos.h:1299 — num_format at bit 0). Only
              // a TEXTURE-type entry (dword0 bits 0-1 == 2) has one: the fetch
              // slots are a union, and a vertex entry's bits would decode to a
              // plausible-looking but meaningless swizzle.
              const uint32_t d1 = (uint32_t(f[4]) << 24) | (uint32_t(f[5]) << 16) |
                                  (uint32_t(f[6]) << 8) | f[7];
              if ((d0 & 3u) == 2u) {
                s_rs.texSwiz[t] =
                    uint16_t(sr::gfx::texswz::guestSwizzleFromFetchDword3(d3));
                s_rs.texBase[t] = ((d1 >> 12) & 0xFFFFFu) << 12;
                // iter-457: SAME entry, two more fields. Only a
                // TEXTURE-type entry has them, so they live inside this
                // branch for exactly the reason the swizzle does -- a
                // vertex entry would decode to a plausible-looking depth.
                const uint32_t d2 = (uint32_t(f[8]) << 24) |
                                    (uint32_t(f[9]) << 16) |
                                    (uint32_t(f[10]) << 8) | f[11];
                const uint32_t d5 = (uint32_t(f[20]) << 24) |
                                    (uint32_t(f[21]) << 16) |
                                    (uint32_t(f[22]) << 8) | f[23];
                s_rs.texDim[t] = uint8_t((d5 >> 9) & 3u);
                s_rs.texDepth[t] = uint16_t((d2 >> 26) + 1u);
                if (sr::kernel::txc::enabled()) {
                  s_txcFetchFmt[t] = uint8_t(d1 & 0x3Fu);
                  s_txcFetchDims[t] = (((d2 & 0x1FFFu) + 1u) << 16) | (((d2 >> 13) & 0x1FFFu) + 1u);
                }
                // A STACKED destination is what the resolve path cannot
                // otherwise recognise: dim 1 with depth > 1. Latch it by
                // the entry's OWN base, never by the sampler binding --
                // the same discipline texBase exists to enforce.
                if (s_rs.texDim[t] == 1u && s_rs.texDepth[t] > 1u &&
                    s_rs.texBase[t])
                  noteStackedTexture(s_rs.texBase[t] & 0x1FFFFFFFu,
                                     s_rs.texDepth[t]);
              } else {
                s_rs.texSwiz[t] = 0xFFFFu;
                s_rs.texBase[t] = 0u;
                s_rs.texDim[t] = 0xFFu;
                s_rs.texDepth[t] = 0u;
                s_txcFetchFmt[t] = 0u;    // not a texture entry: no census fetch fields
                s_txcFetchDims[t] = 0u;
              }
            }
            s_rs.sampValid = true;
          } else {
            s_rs.sampValid = false;
            for (uint32_t t = 0; t < 16u; t++) {
              s_rs.texSwiz[t] = 0xFFFFu;
              s_rs.texBase[t] = 0u;
              s_rs.texDim[t] = 0xFFu;
              s_rs.texDepth[t] = 0u;
            }
          }
          // Log a SAMPLE, not just the first: one draw's registers cannot tell
          // an unwritten register from a deliberate 0, but a spread across
          // draws shows whether the field ever varies the way it should.
          static uint64_t s_n = 0;
          if (++s_n <= 12 || s_n % 20000 == 0) {
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[S6_STATE] depth=%08X blend=%08X mask=%08X vte=%08X | "
                "vpScale=(%.1f,%.1f,%.3f) vpOffset=(%.1f,%.1f,%.3f)",
                s_rs.depthControl, s_rs.blendControl, s_rs.colorMask,
                s_rs.vteCntl, s_rs.vpScale[0], s_rs.vpScale[1], s_rs.vpScale[2],
                s_rs.vpOffset[0], s_rs.vpOffset[1], s_rs.vpOffset[2]);
          }
        }
        blockRelease(a.blockOffset);
      } else {
        // Capture failed: invalidate rather than let the previous draw's
        // transform silently stand in for this one.
        s_rs.valid = false;
        s_rs.sampValid = false;
        for (uint32_t t = 0; t < 16u; t++) {
          s_rs.texSwiz[t] = 0xFFFFu;   // FIX B: invalidate, never inherit
          s_rs.texBase[t] = 0u;
        }
      }
      break;
    }
    // ── iter-456p Step 3A: rebuild the persistent vertex-buffer resource ────
    // ⛔ CAPTURE SUBSTRATE ONLY. No stream is bound, no draw changes shape, no
    // instancing is lowered — Step 3A is explicitly not authorized to change
    // behavior, and 3B is conditional on H10 surviving. This arm exists so a
    // replay REBUILDS the resource before any binding could consume it, which
    // is what makes the later A/B a within-fixture comparison rather than a
    // re-record.
    case RenderCommandType::VertexBufferPublish: {
      const auto& a = c.vertexBufferPublish;
      if (a.sizeBytes) {
        if (const uint8_t* src = poolRead(a.arenaOffset)) {
          VbResource r;
          r.key = a.key;
          r.sizeBytes = a.sizeBytes;
          r.endian = vbUnpackEndian(a.endAliasGen);
          r.alias = (uint32_t)vbUnpackAlias(a.endAliasGen);
          r.generation = vbUnpackGeneration(a.endAliasGen);
          r.digest = ((uint64_t)a.digestHi << 32) | (uint64_t)a.digestLo;
          // ⚠ The bytes are copied in ORIGINAL GUEST ORDER and kept that way.
          // Swapping here would make the resource's digest disagree with the
          // record's, and the endian mode is carried precisely so the eventual
          // consumer swaps at use rather than the store guessing.
          r.bytes.assign(src, src + a.sizeBytes);
          const uint64_t vd = sr::gfx::vbDigest(r.bytes.data(), a.sizeBytes);
          if (vd != r.digest) {
            // A payload that disagrees with the digest that decided its own
            // publication is CORRUPT. Refused by name and counted — never
            // stored, because a wrong resource is worse than a missing one.
            s_vbResBadDigest.fetch_add(1, std::memory_order_relaxed);
          } else {
            std::lock_guard<std::mutex> lk(s_vbResMu);
            s_vbResources[a.objectVA] = std::move(r);
            s_vbResBytes.fetch_add(a.sizeBytes, std::memory_order_relaxed);
            s_vbResBuilt.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          s_vbResNoPayload.fetch_add(1, std::memory_order_relaxed);
        }
        // Released only AFTER the copy above — the resource now owns the bytes,
        // so this is the last consumer of the pool slot.
        poolRelease(a.arenaOffset);
      }
      break;
    }
    case RenderCommandType::TextureUpload: {
      // S6g: HOST bytes in, decoded + uploaded here. The render thread still
      // touches no guest memory — the producer snapshotted at bind.
      const auto& a = c.textureUpload;
      // iter-458 (Astra 068): the census apply witness runs on EVERY exit of this
      // arm (the cube paths break early), keeping the apply ordinal in lockstep
      // with the publish ordinal.
      struct TxcApply {
        const RenderCommand& cmd;
        uint32_t result, band;
        ~TxcApply() {
          if (sr::kernel::txc::enabled())
            sr::kernel::txc::onTexApply(cmd.textureUpload.key, result, band,
                                        cmd.textureUpload.dims, cmd.textureUpload.fmtBits,
                                        s_txcExec.load(std::memory_order_relaxed));
        }
      } txcApply{c, sr::kernel::txc::kOther, sr::kernel::txc::kBand2D};
      if (a.bytes) {
        auto be = s_backend.acquire();
        const uint32_t key = a.key & 0x1FFFFFFFu;
        // ⚠ s_resolveOwned joins unlock ownership here for the same reason: this
        // path snapshots pixels from GUEST memory at bind time, and a render
        // product's guest memory was never written by the CPU. Capturing it
        // would replace a resolved image with garbage on the next bind.
        // iter-456n Class A: fmtBits bit 26 marks an UNLOCK-ORIGIN upload — the
        // guest itself just wrote these bytes and unlocked. It is newer than any
        // earlier capture or resolve, so it bypasses the ownership SKIP and then
        // TAKES ownership below. Without this, a later stale bind capture
        // overwrote fresh unlock data with zeros (measured, live1: key 0BC22000
        // fresh at upload #6, clobbered all-zero at #15).
        const bool unlockOrigin = (a.fmtBits & kUploadUnlockOriginBit) != 0;
        // iter-456n: ownership is EPOCH- AND GENERATION-SCOPED. This comment
        // stated the rule BACKWARDS twice before — the same contract drift that
        // hid the predicate inversion for a whole slice — so it now names every
        // verdict the state machine actually returns (gfx/tex_ownership.hpp):
        //   * SAME live epoch (same descriptor, same generation) -> REFUSED.
        //     That is the stale capture clobbering fresh authored pixels.
        //   * INCOMPATIBLE descriptor -> ADMITTED as a recreation, and its
        //     first successful publication replaces the old claim.
        //   * different generation -> ADMITTED: the object was destroyed or the
        //     address reused, so the old claim is dead.
        //   * no claim -> ADMITTED.
        // Slice 6 adds no new rule here: a backend-confirmed resolve claims
        // through this same machine, so resolve authority is refused, admitted
        // and superseded by exactly these four cases.
        const uint64_t sig = UnlockOwnership::signature(a.fmtBits, a.dims, a.pitch);
        const CaptureVerdict verdict = unlockOwnership().captureVerdict(key, sig);
        const bool captureOk = captureAdmitted(verdict);
        // Witnessed at the DECISION, so live and replay are comparable in order
        // and not merely in totals (ninth adjudication).
        unlockOwnership().noteVerdict(key, sig,
                                      unlockOwnership().generationOf(key),
                                      verdict);
        rv2Watch(key, "CAPTURE_DECISION", sig, captureVerdictName(verdict));
        // Class B v2 dominance, enforced HERE — the existing executor-side
        // capture decision, in executor order. With the gate on, the permanent
        // physical-key exemption is not consulted at all: `captureOk` already
        // carries resolve authority, because a confirmed resolve claims through
        // the same machine. So an initial upload stays authoritative until a
        // LATER successful resolve supersedes it, and a capture under an
        // incompatible descriptor is admitted as a recreation rather than being
        // refused forever.
        const bool v2 = resolveOwnershipV2Enabled();
        // Sampled BEFORE anything can change it, so a refusal can be attributed
        // to the rule that was in force when the decision was taken.
        const Authority priorAuth = unlockOwnership().authorityFor(key);
        const uint64_t priorGen = unlockOwnership().generationOf(key);
        const bool legacyBlocked = !v2 && s_resolveOwned.count(key) != 0;
        bool dispRecorded = false;
        // ── iter-457 H1a: THE FAIL-CLOSED CUBE ARM, BEFORE the backend guard ─
        // ⛔ It was first written INSIDE the guard below, and the C6 fixture
        // caught that: with no initialized backend the cube upload was simply
        // dropped, silently and unnamed, instead of refused. The refusal is a
        // property of the RECORD, not of the backend - no cube path exists in
        // any backend state - so it belongs here, where it always runs and
        // always says why.
        //
        // Before this arm existed at all, texDim == 3 fell into the 2D path
        // below: six faces' worth of bytes uploaded as ONE image, published
        // into the 2D descriptor band, and sampled by whatever bound that key.
        // H1(a) does not implement the backend cube upload - that is H1(b) -
        // and until it does, refusing costs nothing (the cube slots keep the
        // existing white 6-face dummy, i.e. today's picture) while guessing
        // would cost correctness.
        if (uploadIsCube(a.fmtBits)) {
          // ── iter-457 H1(b): THE REAL CUBE UPLOAD (SR_HLE_CUBE_UPLOAD) ────
          // Default OFF. With the gate on, the six faces go to the backend's
          // TEXTURECUBE path; a REFUSAL there is named and the key stays out
          // of s_texSlotCube, so the slot resolves to 0 and reads the white
          // dummy - exactly the picture this arm produced before H1(b).
          //
          // ⛔ The refusal path below is UNCHANGED and still runs whenever the
          // gate is off or the upload refuses. It was written outside the
          // backend guard on purpose (the C6 fixture caught it being inside):
          // the refusal is a property of the RECORD, so it must always run and
          // always say why, including when no backend exists at all.
          static const bool s_cubeUpload = [] {
            const char* e = std::getenv("SR_HLE_CUBE_UPLOAD");
            return e && e[0] == '1';
          }();
          bool cubeUploaded = false;
          // ── iter-457 H1(b)-P1: the backend CALL may be stood in for ──────
          // The self-test process has no D3D12Backend, so the SUCCESS arm
          // below - the one that must retain the applied cube for the recorder
          // preamble - was unreachable in-suite, and the K=1 acquisition
          // c57b7ed found it unwired. The stand-in replaces ONLY the backend
          // call: the gate, the pool reads, the success and refusal arms, the
          // shadow retention and the releases stay production. Null in
          // production (setCubeUploadStandInForTest).
          const CubeUploadStandInFn cubeStandIn =
              s_cubeUploadStandIn.load(std::memory_order_relaxed);
          if (s_cubeUpload && hleDrawEnabled() &&
              (cubeStandIn || (be && be->isInitialized()))) {
            const uint8_t* bytes = unlockOrigin
                ? classARead(a.arenaOffset)
                : texRead(a.arenaOffset);
            if (bytes) {
              const uint32_t fmt = a.fmtBits & 0xFFu;
              const bool tiled = (a.fmtBits >> 8) & 1u;
              const uint32_t endian = (a.fmtBits >> 12) & 3u;
              // The mip region follows the base region in ONE contiguous
              // allocation; the recorder serialises exactly that concatenation.
              const uint8_t* mipSrc = a.mipBytes ? bytes + a.bytes : nullptr;
              sr::gfx::cubebind::CubeRefuse why =
                  sr::gfx::cubebind::CubeRefuse::None;
              uint32_t idx = 0;
              if (cubeStandIn) {
                CubeUploadRequest rq{};
                rq.key = key; rq.base = bytes; rq.baseBytes = a.bytes;
                rq.mip = mipSrc; rq.mipBytes = a.mipBytes; rq.fmt = fmt;
                rq.width = a.dims >> 16; rq.height = a.dims & 0xFFFFu;
                rq.pitch = a.pitch; rq.tiled = tiled; rq.endian = endian;
                rq.mipLevels = uploadMipLevels(a.fmtBits);
                rq.packed = uploadPackedMips(a.fmtBits);
                uint32_t code = 0;
                idx = cubeStandIn(rq, code);
                why = static_cast<sr::gfx::cubebind::CubeRefuse>(code);
              } else {
                idx = be->hleUploadCube(
                    key, bytes, a.bytes, mipSrc, a.mipBytes,
                    fmt, a.dims >> 16, a.dims & 0xFFFFu, a.pitch, tiled, endian,
                    uploadMipLevels(a.fmtBits), uploadPackedMips(a.fmtBits),
                    why);
              }
              if (idx) {
                slotWrite(sr::kernel::txc::kBandCube, key, idx, sr::kernel::txc::kFromUpload);
                cubeUploaded = true;
                // ── iter-457 H1(b)-P1: RETAIN THE APPLIED CUBE FOR THE PREAMBLE
                // Through the SAME applied-state shadow the 2D arms feed
                // (shadowNoteApplied: one normalized key, last successful
                // writer, monotonic apply order - no cube-only shadow, writer
                // or preamble order), with the FULL base+mip region, after
                // backend success and before either pool releases the bytes.
                // The total is CHECKED by the one cube validator, so the shadow
                // can never hold a cube whose two regions disagree with its
                // payload. Without this the preamble carried only 2D state and
                // a title trace armed after cube creation replayed with
                // attempted=0 (K=1 c57b7ed).
                const uint64_t cubeTotal =
                    (uint64_t)a.bytes + (uint64_t)a.mipBytes;
                const CubeCheck cubeCk = checkCubeUpload(c, cubeTotal);
                if (cubeCk.ok && cubeTotal <= 0xFFFFFFFFull) {
                  shadowNoteApplied(c, bytes, (uint32_t)cubeTotal, key);
                } else {
                  SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[S6_CUBE] applied cube key=%08X NOT retained for the "
                      "preamble: %s (base=%u mip=%u)",
                      key, cubeCk.why, a.bytes, a.mipBytes);
                }
                s_cubeExecUploaded.fetch_add(1, std::memory_order_relaxed);
                if (s_cubeExecUploaded.load(std::memory_order_relaxed) <= 8)
                  SR_LOG_BOOT(sr::LogSeverity::Info,
                      "[S6_CUBE] uploaded key=%08X %ux%u mips=%u -> cube idx=%u",
                      key, a.dims >> 16, a.dims & 0xFFFFu,
                      uploadMipLevels(a.fmtBits), idx);
              } else {
                s_cubeExecUploadRefused.fetch_add(1, std::memory_order_relaxed);
                if (s_cubeExecUploadRefused.load(std::memory_order_relaxed) <= 8)
                  SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[S6_CUBE] backend REFUSED key=%08X %ux%u mips=%u "
                      "reason=%s - the slot keeps the white dummy",
                      key, a.dims >> 16, a.dims & 0xFFFFu,
                      uploadMipLevels(a.fmtBits),
                      sr::gfx::cubebind::refuseName(why));
              }
            }
          }
          if (cubeUploaded) {
            txcApply.result = sr::kernel::txc::kApplied;
            txcApply.band = sr::kernel::txc::kBandCube;
            if (unlockOrigin) classARelease(a.arenaOffset, a.bytes);
            else texRelease(a.arenaOffset);
            break;
          }
          s_texUploadFail++;
          s_cubeExecRefused.fetch_add(1, std::memory_order_relaxed);
          if (s_cubeExecRefused.load(std::memory_order_relaxed) <= 8)
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[S6_CUBE] upload REFUSED key=%08X %ux%u mips=%u packed=%u "
                "base=%u mip=%u - cube upload is OFF or refused; "
                "NOT uploaded as 2D",
                key, a.dims >> 16, a.dims & 0xFFFFu,
                uploadMipLevels(a.fmtBits),
                uploadPackedMips(a.fmtBits) ? 1u : 0u, a.bytes, a.mipBytes);
          // ⛔ texRelease, NOT poolRelease. The director caught this: a cube
          // payload is allocated by `texWrite`, and poolRelease is the
          // SIZE-ROUTED releaser - on a texture-pool offset it would leak the
          // texture slot AND free an unrelated arena slot. This mirrors the
          // normal TextureUpload exit below exactly. Neither the suite nor the
          // mutation harness could see it, because nothing asserts pool
          // accounting across a refusal; that is an r2 fixture, not a comment.
          if (unlockOrigin) classARelease(a.arenaOffset, a.bytes);
          else texRelease(a.arenaOffset);
          break;
        }
        // ⛔ THE BACKEND-INDEPENDENT WITNESS (iter-457 H1a-r2). This is the
        // first statement of the NON-CUBE path and it runs before any backend
        // availability test, which is the whole point: the fall-through mutant
        // (delete the `break` above) survived precisely because everything
        // downstream is gated on `be && be->isInitialized()`, so with no
        // backend a fallen-through cube was indistinguishable from a refused
        // one. A "no 2D slot was registered" assertion could never have caught
        // it either, for the same reason. Counting ENTRY, not effect, is what
        // makes the terminator observable.
        s_nonCubeUploadEntries.fetch_add(1, std::memory_order_relaxed);
        if (be && be->isInitialized() && hleDrawEnabled() &&
            (unlockOrigin || (captureOk && !legacyBlocked))) {
          // Unlock-origin payloads live in the isolated Class-A pool; bind
          // captures live in the texture pool. The handle tag decides, so the
          // two allocators can never be crossed.
          if (const uint8_t* px = unlockOrigin ? classARead(a.arenaOffset)
                                               : texRead(a.arenaOffset)) {
            // iter-449x: dimension rides fmtBits bits 14-15 (0 = legacy/2D —
            // every pre-slice trace reads 0 here), depth-1 rides bits 16-25.
            const uint32_t texDim = (a.fmtBits >> 14) & 3u;
            if (texDim == 2u) {
              const uint32_t vDepth = ((a.fmtBits >> 16) & 0x3FFu) + 1u;
              const uint32_t idx3 = be->hleUploadHostVolume(
                  key, px, a.bytes, a.fmtBits & 0xFFu, a.dims >> 16,
                  a.dims & 0xFFFFu, vDepth, a.pitch,
                  ((a.fmtBits >> 8) & 1u) != 0, (a.fmtBits >> 12) & 3u);
              if (idx3) {
                slotWrite(sr::kernel::txc::kBand3D, key, idx3, sr::kernel::txc::kFromUpload);
                s_texUploads++;
                txcApply.result = sr::kernel::txc::kApplied;
                txcApply.band = sr::kernel::txc::kBand3D;
              }
              else s_texUploadFail++;
            } else {
              const uint32_t slot = be->hleUploadHostTextureFmt(
                  key, px, a.bytes, a.fmtBits & 0xFFu, a.dims >> 16,
                  a.dims & 0xFFFFu, a.pitch, ((a.fmtBits >> 8) & 1u) != 0,
                  (a.fmtBits >> 12) & 3u);
              if (slot) {
                slotWrite(sr::kernel::txc::kBand2D, key, slot, sr::kernel::txc::kFromUpload);
                s_texUploads++;
                txcApply.result = sr::kernel::txc::kApplied;
                shadowNoteApplied(c, px, a.bytes, key);   // APPLIED, not submitted
                rv2Watch(key, "UPLOAD_APPLIED", sig,
                         unlockOrigin ? "unlock-origin" : "bind-capture");
                {   // the epoch this key is genuinely established under
                  AppliedDesc& ad = s_lastAppliedDesc[key];
                  ad.sig = sig;
                  ad.generation = unlockOwnership().generationOf(key);
                  ad.dims = a.dims;
                }
                // Take ownership for unlock-origin data so later bind-time
                // captures of the same key cannot clobber it — scoped to THIS
                // descriptor epoch, and REPLACING any previous epoch at the
                // same address (recreation / physical-key reuse).
                if (unlockOrigin) unlockOwnership().claim(key, sig);
                noteDisposition(key, sig, priorGen, priorAuth,
                                CaptureDisposition::Applied,
                                captureDispositionEnabled()
                                    ? sr::gfx::ShaderCache::hashUcode(px, a.bytes)
                                    : 0ull,
                                slot);
                dispRecorded = true;
              }
              else {
                s_texUploadFail++;
                noteDisposition(key, sig, priorGen, priorAuth,
                                CaptureDisposition::UploadFailed, 0ull, 0u);
                dispRecorded = true;
              }
            }
          }
        } else if (be && (!captureOk || legacyBlocked)) {
          s_texSkipOwned++;
          txcApply.result = sr::kernel::txc::kRefused;
          if (!captureOk) unlockOwnership().noteRefusal();
          if (v2 && priorAuth == Authority::Resolve)
            s_rv2RefusedNoClaim++;   // a capture the resolve product refused
          // WHICH RULE refused it. This is the attributable difference between
          // the two arms: OFF blocks through the permanent legacy set, ON blocks
          // through epoch-scoped resolve authority — same pixels, different
          // source — and after a generation advance ON admits where OFF still
          // refuses forever.
          const CaptureDisposition d =
              classifyDisposition(captureOk, legacyBlocked, priorAuth);
          noteDisposition(key, sig, priorGen, priorAuth, d, 0ull, 0u);
          dispRecorded = true;
        }
        (void)dispRecorded;
        if (unlockOrigin) classARelease(a.arenaOffset, a.bytes);
        else texRelease(a.arenaOffset);
      }
      break;
    }
    case RenderCommandType::OwnershipRestore: {
      // Preamble-only. Puts back exactly what the live run held, so replay's
      // first accept/refuse decision matches live's.
      const auto& o = c.ownershipRestore;
      const uint64_t sig64 = ((uint64_t)o.sigHi << 32) | (uint64_t)o.sigLo;
      // key == 0 is the reserved WINDOW-START marker, not a claim: pixel
      // address 0 is never a real texture, so it cannot collide with one.
      // It is emitted LAST in the preamble, so every preamble decision made
      // while re-establishing state is discarded and only in-window decisions
      // are witnessed - which is what makes live and replay comparable.
      if (o.key == 0u) beginWitnessWindow();
      else unlockOwnership().restoreClaim(
               o.key, sig64, (uint64_t)o.generation, (uint64_t)o.publishes,
               (Authority)o.reserved);
      break;
    }
    case RenderCommandType::TextureObjectEvent:
      // Executed HERE, on the render thread, at its exact position in the
      // stream: everything queued before it belongs to the old object and
      // has already run; everything after belongs to the new one.
      unlockOwnership().noteObjectEvent(c.textureObjectEvent.key,
                                        c.textureObjectEvent.reason);
      break;
    case RenderCommandType::SetTexture:
      // The guest's OWN bind order is the plane->sampler authority. Never infer
      // from dimensions: Cb and Cr are identical shapes, so a dimension-based
      // guess silently swaps the chroma channels and tints the whole picture.
      if (c.setTexture.sampler < kMaxSamplers)
        s_samplerTex[c.setTexture.sampler] = c.setTexture.texture;
      // ── iter-445 S1: THE BIND HIT/MISS COUNTER ───────────────────────────
      // Sited HERE and not in buildDraw, deliberately. buildDraw runs only for
      // draws that already carry vertices AND pass the shape gate, over
      // samplers 0..2 — which is ZERO samples in the front-end era, exactly
      // where the question matters. This sees every bind on every sampler.
      // It measures the ONE thing the texture-key fix must move: does a bound
      // texture resolve to an uploaded SRV slot at all? Ordering is safe:
      // TextureUpload is enqueued before SetTexture from the SAME hook on the
      // SAME thread into the SAME queue.
      if (c.setTexture.texture) {
        const uint32_t k = c.setTexture.texture & 0x1FFFFFFFu;
        s_boundTexKeys.insert(k);   // iter-446 R5: the resolve-key cross-check
        // iter-446 R5b: how often the guest SAMPLES A RENDER PRODUCT. This is
        // the slice's own metric and it must not be confused with s_bindHit —
        // registering the destinations raises bindHit BY CONSTRUCTION, so that
        // number cannot grade this work. This one counts the actual event.
        if (s_resolveOwned.count(k)) s_resolveBindHit++;
        if (s_texSlot.count(k)) s_bindHit++;
        else {
          s_bindMiss++;
          if (s_bindMiss <= 8)
            SR_LOG_BOOT(sr::LogSeverity::Info,
                "[S6_BIND] miss #%llu sampler=%u key=%08X (no uploaded slot) — "
                "this draw samples the 1x1 WHITE fallback",
                (unsigned long long)s_bindMiss, c.setTexture.sampler, k);
        }
      }
      break;
    // ── iter-445: THE GUEST'S OWN CLEAR ─────────────────────────────────────
    // The producer now publishes decoded scalars instead of a raw guest VA and
    // a literal zero, and C4 made the list open so these CAN be recorded. They
    // are still counted-and-not-executed by default, and the reason is
    // ordering, not laziness: the guest's clears target the SIXTEEN render
    // targets it binds (ENGINE_MAP §6g.4/§6g.7), and we do not bind those yet.
    // Executing one against the backbuffer would paint an offscreen pass's
    // clear onto the screen and destroy the one verified picture we have.
    // SR_HLE_GUEST_CLEAR=1 turns them on for the render-target slice to A/B.
    // ⚠ THIS ARM MUST NEVER EXECUTE A CLEAR. D3DDeviceClear is a WRAPPER that
    // tail-calls D3DDeviceClearF once per rect, and our hooks delegate — so the
    // work already arrives as ClearF commands. Implementing both would
    // double-clear. This arm exists to COUNT and to prove the pairing.
    case RenderCommandType::Clear: {
      const auto& a = c.clear;
      float z;
      std::memcpy(&z, &a.z, 4);
      s_clearSeen++;
      if (s_clearSeen <= 6)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_CLEAR] Clear(wrapper) #%llu flags=%08X argb=%08X z=%g "
            "rectCount=%u — forwards to ClearF, never executed here",
            (unsigned long long)s_clearSeen, a.flags, a.color, z, a.count);
      break;
    }
    case RenderCommandType::ClearF: {
      const auto& a = c.clearF;
      float rgba[4] = {0, 0, 0, 0}, z;
      std::memcpy(&z, &a.z, 4);
      // ── Astra 085: THE COLOUR IS READ ONLY WHEN IT IS WELL-FORMED ──────────
      // A well-formed command carries EXACTLY one float4 (kClearFColorBytes) in
      // a block this command owns: the producer's, or the replayer's restaged
      // copy. Anything else is refused BY NAME and never read - hr1b replayed
      // 12 ClearF records whose colorBlock was a LIVE session offset with no
      // payload, and this arm read (and released) whatever the fresh pool held
      // there. A missing colour is never replaced by an invented one either:
      // the old default (0,0,0,1) logged as if it were the guest's value.
      bool colourOk = false;
      if (a.colorBytes == kClearFColorBytes) {
        if (const uint8_t* p = blockRead(a.colorBlock)) {
          std::memcpy(rgba, p, kClearFColorBytes);
          colourOk = true;
        }
      } else if (a.colorBytes) {
        s_clearFColourMalformed++;
      }
      if (!colourOk) s_clearFColourMissing++;
      // Without a colour the TARGET bits (0-3, ENGINE_MAP §6g.8) cannot be
      // honoured; depth (0x10) and stencil (0x20) carry their values in the
      // command itself and still can. Refused by name, never substituted.
      const uint32_t effFlags = colourOk ? a.flags : (a.flags & ~0xFu);
      if (effFlags != a.flags) s_clearFTargetsMasked++;
      s_clearFLastFlags = effFlags;
      s_clearFLastColourOk = colourOk;
      std::memcpy(s_clearFLastRgba, rgba, sizeof(rgba));
      s_clearFSeen++;
      if (s_clearFSeen <= 6) {
        if (colourOk)
          SR_LOG_BOOT(sr::LogSeverity::Info,
              "[S6_CLEAR] ClearF #%llu flags=%08X rgba=(%g,%g,%g,%g) z=%g "
              "stencil=%u%s", (unsigned long long)s_clearFSeen, a.flags,
              rgba[0], rgba[1], rgba[2], rgba[3], z, a.stencil,
              guestClearEnabled() ? "" : " (not executed)");
        else
          SR_LOG_BOOT(sr::LogSeverity::Info,
              "[S6_CLEAR] ClearF #%llu flags=%08X colour=MISSING (colorBytes=%u) "
              "-> target bits refused, effective flags=%08X z=%g stencil=%u%s",
              (unsigned long long)s_clearFSeen, a.flags, a.colorBytes, effFlags,
              z, a.stencil, guestClearEnabled() ? "" : " (not executed)");
      }
      // ── iter-446 R6: EXECUTE THE GUEST'S OWN CLEAR ───────────────────────
      // ⚠ HERE AND ONLY HERE. D3DDeviceClear is a WRAPPER that tail-calls this
      // function once per rect, and our hooks delegate — so arming the Clear
      // arm too would double-clear every wrapper call (ENGINE_MAP §6g.8).
      // The targets are the surfaces the GUEST bound, which is why this could
      // not be switched on before the pool existed: without it, "clear the
      // guest's render target" had nowhere to land but the backbuffer, which
      // would paint an offscreen pass's clear over the picture.
      // ⚠ a.rect != 0 means a partial clear; v1 clears the FULL surface and
      // says so rather than silently treating a sub-rect as the whole thing.
      if (guestClearEnabled() && effFlags)
        submitGuestClear(effFlags, rgba, z, (uint8_t)(a.stencil & 0xFFu),
                         a.rect != 0);
      // ⚠ RELEASE ON EVERY WELL-FORMED PATH, including execution-gated-off.
      // This arm is the ONLY thing that frees the block; iter-443 published
      // three such commands with no release and pinned the pool at 48/48 with
      // 2,320,314 rejects. Only a well-formed command OWNS a block (the
      // producer publishes 0 or kClearFColorBytes; the replayer normalises
      // everything else to 0/kBlockFull), so a malformed size never releases
      // an offset it cannot prove it allocated.
      if (a.colorBytes == kClearFColorBytes) blockRelease(a.colorBlock);
      break;
    }
    // ── iter-446 R3: RENDER TARGET / DEPTH-STENCIL BINDING ───────────────────
    // D3D9 binding is STATE, so these only resolve the surface to a host
    // resource and update the mirror. Nothing is bound to the OM here: D3D12
    // wants the binding at the draw, and the clear path takes an RTV handle
    // directly. A NULL surface is a real, frequent event — MRT teardown of
    // slots 1/2/3 was measured 65K times per run — and must clear the mirror
    // slot rather than be ignored.
    case RenderCommandType::SetRenderTarget: {
      const auto& a = c.setRenderTarget;
      s_setRtSeen++;
      // iter-450c: latch the current RT key for the [LUM_BIND] probe.
      if (a.index == 0) s_probeCurRt = a.surface & 0x1FFFFFFFu;
      if (auto be = s_backend.acquire()) {
        sr::gfx::D3D12Backend::HleSurface* s = nullptr;
        if (a.surface >= 0x1000u)
          s = be->hleSurfaceGet(a.surface, a.surfInfo, a.colorInfo, a.byteSize,
                                a.sizeWord, /*isDepth=*/false);
        if (s) s_setRtBound++;
        be->hleBindRenderTarget(a.index, s);
      }
      break;
    }
    case RenderCommandType::SetDepthStencilSurface: {
      const auto& a = c.setDepthStencilSurface;
      s_setDsSeen++;
      if (auto be = s_backend.acquire()) {
        sr::gfx::D3D12Backend::HleSurface* s = nullptr;
        if (a.surface >= 0x1000u)
          s = be->hleSurfaceGet(a.surface, a.surfInfo, a.colorInfo, a.byteSize,
                                a.sizeWord, /*isDepth=*/true);
        if (s) s_setDsBound++;
        be->hleBindDepthStencil(s);
      }
      break;
    }
    // ── iter-446 R4: VIEWPORT / SCISSOR ──────────────────────────────────────
    // LATCHED ONLY — see the note at the declarations. Both were publishing raw
    // guest VAs until R0b; they now carry real scalars, so latching them is
    // finally meaningful rather than storing a pointer nobody may dereference.
    case RenderCommandType::SetViewport: {
      const auto& a = c.setViewport;
      s_setVpSeen++;
      // width==height==0 is the producer's "unreadable or NULL pViewport"; the
      // default viewport in .rdata legitimately reads 0x7FFFFFFF, which means
      // "unbounded", not a 2-billion-pixel surface. Neither is a viewport we
      // could hand to D3D12, so neither is latched as valid.
      const bool sane = a.width && a.height && a.width <= 8192u && a.height <= 8192u;
      s_viewport = {a.x, a.y, a.width, a.height, a.minZ, a.maxZ, sane};
      break;
    }
    case RenderCommandType::SetScissorRect: {
      const auto& a = c.setScissorRect;
      s_setScSeen++;
      const int32_t l = (int32_t)a.left, t = (int32_t)a.top;
      const int32_t r = (int32_t)a.right, b = (int32_t)a.bottom;
      s_scissor = {l, t, r, b, r > l && b > t && r <= 8192 && b <= 8192};
      break;
    }
    // ── iter-446 R5: RESOLVE ─────────────────────────────────────────────────
    // DECODES AND CENSUSES; the copy itself is R5b. The order is deliberate:
    // the copy's destination is a texture the guest SAMPLES, and iter-445 lost
    // an iteration to a texture stored under a key no bind could find. So the
    // first thing this arm answers is whether BOTH ends actually resolve —
    // source in the surface pool, destination among the keys SetTexture binds
    // by — because a copy between two resources we cannot name is worse than no
    // copy at all: it would look like progress and change nothing on screen.
    case RenderCommandType::Resolve: {
      const auto& a = c.resolve;
      // iter-457: armed here, emitted by ~ResolveExecRow on EVERY exit.
      ResolveExecRow rex{s_admitTraceRec.load(std::memory_order_relaxed)};
      s_resolveSeen++;
      // Unpacked from levelSliceFlags — the `flags` dword became `destSize`
      // so the destination's TRUE extent could be carried (§6g.12).
      const uint32_t srcIdx = (a.levelSliceFlags >> 12) & 7u;
      if (srcIdx == 4u) s_resolveDepth++;
      if (a.levelSliceFlags & (1u << 15)) s_resolveClearRT++;
      if (a.levelSliceFlags & (1u << 16)) s_resolveClearZ++;
      if (a.destKey) s_resolveDests.insert(a.destKey);
      if (a.srcSurface) s_resolveSrcs.insert(a.srcSurface);
      // The SAME normalisation the sampler lookup uses (physical address): the
      // fetch constant reports 0x0F807000 where Lock reports 0x8F807000 for the
      // very same pixels, and comparing raw values misses every time.
      if (s_texSlot.count(a.destKey & 0x1FFFFFFFu)) s_resolveDestKnown++;
      if (auto be = s_backend.acquire()) {
        sr::gfx::D3D12Backend::HleSurface* src = be->hleSurfaceFind(a.srcSurface);
        if (src) s_resolveSrcPooled++;
        // Distinguish the two ways the execute block is skipped: a missing
        // source surface and the gate being off are NOT the same finding.
        rex.outcome = !src ? "noSource"
                           : (resolveExecEnabled() ? "unreached" : "gateOff");
        // ── R5b: EXECUTE IT ────────────────────────────────────────────────
        // v1 copies the WHOLE surface into slice 0 at level 0. Everything else
        // is refused BY NAME rather than approximated: a sub-rect copied as a
        // full surface, or slice 2 written over slice 0, produces a texture
        // that samples plausibly and is wrong — the single worst outcome
        // available here, because it looks like progress.
        if (resolveExecEnabled() && src) {
          const bool fullSurface = (a.rectLT == 0 && a.rectRB == 0);
          const bool atOrigin = (a.destPoint == 0);
          const uint32_t level = a.levelSliceFlags & 0xFu;
          const uint32_t slice = (a.levelSliceFlags >> 4) & 0xFFu;
          // ⛔ §6g.12: the DESTINATION's true extent, from its own fetch
          // constant. Sizing it from the source instead makes every 360-tall
          // target 368 tall — 8 garbage rows and a 2.2% vertical stretch on
          // everything that samples it. 0 means the producer could not read the
          // dest header. ⚠ destSize == 0 is a LEGAL 1x1 texture (the final
          // luminance step), so it is decoded like any other value rather than
          // treated as an error — reading 0 as "unreadable" refused the real
          // 1x1 target ~6,355 times per run.
          const uint32_t dstW = (a.destSize & 0x1FFFu) + 1u;
          const uint32_t dstH = ((a.destSize >> 13) & 0x1FFFu) + 1u;
          // iter-448: PER-KEY skip census. The global rect/sliced counters
          // could not answer the routing question ("why does the FRONT
          // BUFFER's dest never register?") because they are anonymous. One
          // small map names, for every destination, how its resolves were
          // handled — printed beside [S6_RESOLVE_DESTS].
          auto& pk = s_destSkips[a.destKey & 0x1FFFFFFFu];
          // iter-448: A FULL-COVERAGE RECT IS THE FULL SURFACE. The per-key
          // census caught the FRONT BUFFER as the one dest that was 100%
          // rect-refused (8A94E000: e0/r2406/s0): the guest's final
          // primary→front resolve writes its rect EXPLICITLY as
          // (0,0)-(dstW,dstH) instead of as zeros, and v1 read any non-zero
          // rect as "sub-rect". Accept exactly the explicit-full case; a
          // genuinely partial rect still refuses by name as before.
          const bool explicitFull =
              a.rectLT == 0 && a.destPoint == 0 &&
              (a.rectRB & 0xFFFFu) == dstW && (a.rectRB >> 16) == dstH;
          if ((!fullSurface && !explicitFull) || !atOrigin) {
            s_resolveSkipRect++; pk.rect++; rex.outcome = "rect";
#if SR_ORACLE
            if (sr::gfx::prov::enabled())
              sr::gfx::prov::ring().onResolve(a.destKey, a.destKey & 0x1FFFFFFFu,
                  src, src ? (const void*)src->res.Get() : nullptr, a.srcSurface,
                  dstW, dstH, src ? (uint32_t)src->dxgi : 0u, a.levelSliceFlags,
                  /*recorded=*/false, /*reason=*/10u, be->drawOrdinalNow());
#endif
          }
          // ── iter-457 STACKED ARM ─────────────────────────────────────
          // Placed BEFORE the sliced refusal and guarded by the gate, so
          // with the gate OFF the chain below is exactly what it was.
          // `stackedDepthOf` returns 0 for unknown AND for a key whose
          // depth was seen to conflict, so both refuse here rather than
          // building a destination we cannot justify.
          else if (stackedResolveEnabled() && !level &&
                   stackedDepthOf(a.destKey) &&
                   slice < stackedDepthOf(a.destKey)) {
            const uint32_t sdepth = stackedDepthOf(a.destKey);
            sr::gfx::D3D12Backend::ResolveSkip swhy{};
            const uint32_t idx3 = be->hleResolveToVolume(
                src, a.destKey, dstW, dstH, sdepth, slice, swhy);
            rex.executed = (idx3 != 0);
            rex.outcome = idx3 ? "ok3d" : "refused3d";
            rex.why = idx3 ? 0u : (uint32_t)swhy;
            if (idx3) {
              // ⚠ THE 3D TABLE, not s_texSlot. Registering a stacked
              // product as 2D is the whole defect: every consumer looks
              // it up in the 3D band and finds the white dummy.
              slotWrite(sr::kernel::txc::kBand3D, a.destKey, idx3, sr::kernel::txc::kFromResolve);
              s_resolveOwned.insert(a.destKey & 0x1FFFFFFFu);
              s_resolveExec++;
              pk.exec++;
              s_stackedResolves++;
            } else {
              s_stackedRefused++;
            }
          }
          else if (level || slice) {
            s_resolveSkipSliced++; pk.sliced++; rex.outcome = "sliced";
#if SR_ORACLE
            if (sr::gfx::prov::enabled())
              sr::gfx::prov::ring().onResolve(a.destKey, a.destKey & 0x1FFFFFFFu,
                  src, src ? (const void*)src->res.Get() : nullptr, a.srcSurface,
                  dstW, dstH, src ? (uint32_t)src->dxgi : 0u, a.levelSliceFlags,
                  /*recorded=*/false, /*reason=*/11u, be->drawOrdinalNow());
#endif
          }
          else {
            pk.exec++;
            sr::gfx::D3D12Backend::ResolveSkip why{};
            const uint32_t slot =
                    be->hleResolveToTexture(src, a.destKey, dstW, dstH, why);
            // THE ONE LINE THE SIDECAR EXISTS FOR: a product exists iff the
            // backend returned a slot. A refusal keeps the backend's own
            // ResolveSkip code so the reason survives to the analysis.
            rex.executed = (slot != 0);
            rex.outcome = slot ? "ok" : "refused";
            rex.why = slot ? 0u : (uint32_t)why;
            // Provenance ring (observe-only): the resolve is the middle link of the
            // director-ruled chain producer->pool surface->frontKey->backbuffer.
#if SR_ORACLE
            if (sr::gfx::prov::enabled())
              sr::gfx::prov::ring().onResolve(a.destKey, a.destKey & 0x1FFFFFFFu,
                  src, src ? (const void*)src->res.Get() : nullptr, a.srcSurface,
                  dstW, dstH, src ? (uint32_t)src->dxgi : 0u, a.levelSliceFlags,
                  /*recorded=*/slot != 0, /*reason=*/(uint32_t)why,
                  be->drawOrdinalNow());
#endif
            if (slot) {
              // ⚠ REGISTER IT UNDER THE BIND KEY, normalised exactly as the
              // sampler lookup normalises. This line is the whole point of the
              // slice: without it the copy happens and nothing can ever read it.
              slotWrite(sr::kernel::txc::kBand2D, a.destKey, slot, sr::kernel::txc::kFromResolve);
              // ⚠ AND CLAIM OWNERSHIP. The bind-capture path (S6g) snapshots a
              // texture's pixels from GUEST memory when it is bound — and a
              // render product's guest memory holds stale garbage, because the
              // CPU never wrote it. Without this the very next bind would
              // overwrite the resolved image with rubbish. Same hazard and same
              // remedy as unlock ownership for the movie planes.
              s_resolveOwned.insert(a.destKey & 0x1FFFFFFFu);
              s_resolveExec++;
              // ── Class B v2: TAKE AUTHORITY, but only on CONFIRMED success.
              // This branch is reached only when the backend returned a slot,
              // so an attempted or refused resolve provably changes nothing.
              if (resolveOwnershipV2Enabled()) {
                const uint32_t rkey = a.destKey & 0x1FFFFFFFu;
                uint64_t rsig = 0;
                if (a.levelSliceFlags & kResolveDescValidBit) {
                  // The producer carried the destination's REAL descriptor.
                  const uint32_t rfmt =
                      (a.destSize >> kResolveFmtShift) & kResolveFmtMask;
                  const uint32_t rw = (a.destSize & 0x1FFFu) + 1u;
                  const uint32_t rh = ((a.destSize >> 13) & 0x1FFFu) + 1u;
                  const uint32_t rpitch =
                      ((a.levelSliceFlags >> kResolvePitchShift) &
                       kResolvePitchMask) << 5;
                  const uint32_t rtiled =
                      (a.levelSliceFlags & kResolveTiledBit) ? 1u : 0u;
                  const uint32_t rendian =
                      (a.levelSliceFlags >> kResolveEndianShift) &
                      kResolveEndianMask;
                  const uint32_t rfmtBits =
                      (rfmt & 0xFFu) | (rtiled << 8) | (rendian << 12);
                  rsig = UnlockOwnership::signature(
                      rfmtBits, (rw << 16) | (rh & 0xFFFFu), rpitch);
                  // ── CARRIED-DESCRIPTOR ROUND-TRIP WITNESS ──────────────
                  // The director's correction: the unit tests construct
                  // signatures directly and therefore do NOT cover producer
                  // decode -> bit packing -> trace serialization -> executor
                  // unpacking. This prints the unpacked components and compares
                  // the resulting signature against the epoch the CAPTURE side
                  // established for the same key, so exact equality across the
                  // whole chain is measured rather than assumed.
                  {
                    static uint64_t s_cdN = 0;
                    const auto itC = s_lastAppliedDesc.find(rkey);
                    const uint64_t capSig =
                        (itC == s_lastAppliedDesc.end()) ? 0ull : itC->second.sig;
                    if (g_resolveArmWant) {
                      g_resolveHits++;
                      if (g_resolveHits <= g_resolveArmWant)
                        SR_LOG_BOOT(sr::LogSeverity::Warn,
                            "[TRACE_RESOLVEN] HIT #%llu/%llu key=%08X "
                            "sig=%016llX carried descriptor applied",
                            (unsigned long long)g_resolveHits,
                            (unsigned long long)g_resolveArmWant, rkey,
                            (unsigned long long)rsig);
                    }
                    if (++s_cdN <= 24)
                      SR_LOG_BOOT(sr::LogSeverity::Warn,
                          "[S6_RV2_CARRIED] #%llu key=%08X fmt=%u %ux%u "
                          "pitch=%u tiled=%u endian=%u | resolveSig=%016llX "
                          "captureSig=%016llX EQUAL=%d (-1 = no capture epoch yet, "
                          "not a mismatch)",
                          (unsigned long long)s_cdN, rkey, rfmt, rw, rh, rpitch,
                          rtiled, rendian, (unsigned long long)rsig,
                          (unsigned long long)capSig,
                          (capSig == 0) ? -1 : (capSig == rsig ? 1 : 0));
                  }
                } else {
                  // LEGACY RECORD (no descriptor). It may adopt an epoch that
                  // is ALREADY ESTABLISHED for this key — recorded executor
                  // state, never capture inference — and only while that claim
                  // is still current, i.e. its generation matches. Otherwise it
                  // takes nothing.
                  const auto itAD = s_lastAppliedDesc.find(rkey);
                  const uint32_t rw = (a.destSize & 0x1FFFu) + 1u;
                  const uint32_t rh = ((a.destSize >> 13) & 0x1FFFu) + 1u;
                  const uint32_t wantDims = (rw << 16) | (rh & 0xFFFFu);
                  if (itAD != s_lastAppliedDesc.end() && itAD->second.sig != 0 &&
                      itAD->second.generation ==
                          unlockOwnership().generationOf(rkey) &&
                      itAD->second.dims == wantDims) {
                    rsig = itAD->second.sig;
                    s_rv2LegacyAdopted++;
                  }
                }
                rv2Watch(rkey, "RESOLVE_BACKEND_SUCCESS", rsig,
                         (a.levelSliceFlags & kResolveDescValidBit)
                             ? "descriptor=carried" : "descriptor=legacy");
                if (rsig) {
                  unlockOwnership().claimResolve(rkey, rsig);
                  s_rv2Claims++;
                  rv2Watch(rkey, "OWNERSHIP_TRANSITION", rsig, "-> resolve");
                } else {
                  // NAMED, not just counted: the director fails the packet if a
                  // specific target key lands here, so the keys must be legible
                  // rather than inferred from a total.
                  s_rv2SuccessNoEpoch++;
                  if (s_rv2SuccessNoEpoch <= 32)
                    SR_LOG_BOOT(sr::LogSeverity::Warn,
                        "[S6_RESOLVE_V2] successNoEpoch #%llu key=%08X "
                        "destSize=%08X lsf=%08X",
                        (unsigned long long)s_rv2SuccessNoEpoch, rkey,
                        a.destSize, a.levelSliceFlags);
                }
              }
            } else {
              s_resolveSkipOther++;
              if (s_resolveSkipOther <= 8)
                SR_LOG_BOOT(sr::LogSeverity::Info,
                    "[S6_RESOLVE] refused key=%08X: %s", a.destKey,
                    resolveSkipName(why));
            }
          }
        }
      }
      if (s_resolveSeen <= 8)
        SR_LOG_BOOT(sr::LogSeverity::Info,
            "[S6_RESOLVE] #%llu lsf=%08X src=%u(surf=%08X) destKey=%08X "
            "destSize=%08X (%ux%u) "
            "rect=(%u,%u)-(%u,%u) point=(%u,%u) level=%u slice=%u",
            (unsigned long long)s_resolveSeen, a.levelSliceFlags, srcIdx,
            a.srcSurface, a.destKey, a.destSize,
            (a.destSize & 0x1FFFu) + 1u, ((a.destSize >> 13) & 0x1FFFu) + 1u,
            a.rectLT & 0xFFFFu, a.rectLT >> 16, a.rectRB & 0xFFFFu,
            a.rectRB >> 16, a.destPoint & 0xFFFFu, a.destPoint >> 16,
            a.levelSliceFlags & 0xFu, (a.levelSliceFlags >> 4) & 0xFFu);
      break;
    }
    case RenderCommandType::SetShaders:
      s_curVs = ((uint64_t)c.setShaders.vsHi << 32) | c.setShaders.vsLo;
      s_curPs = ((uint64_t)c.setShaders.psHi << 32) | c.setShaders.psLo;
      break;
    case RenderCommandType::SetVertexDecl: {
      // S6c captured it; S6e consumes it. ⚠ THE BLOCK MUST BE RELEASED ON
      // EVERY PATH — iter-443 published these commands with no arm here at
      // all, so each one leaked its slot and the pool saturated within ~16
      // draws (measured: rejects=2,320,314, GATE=MOVIE-BUT-ARENA-LAPPED).
      const auto& a = c.setVertexDecl;
      if (a.bytes) {
        if (const uint8_t* d = blockRead(a.arenaOffset)) {
          const uint32_t n = a.elemCount <= kMaxDeclElems ? a.elemCount : 0u;
          const uint32_t b = n * kDeclElemBytes;
          if (n && b <= a.bytes) {
            std::memcpy(s_declRaw, d, b);
            s_declCount = n;
          }
        }
        blockRelease(a.arenaOffset);
      } else {
        // bytes==0 means the producer could not capture this draw's
        // declaration. Holding the PREVIOUS one would expand this draw's
        // vertices with the wrong attribute map and still count as rendered,
        // so drop it and let the draw be refused with a named reason.
        s_declCount = 0;
      }
      break;
    }
    case RenderCommandType::SetConstants: {
      const auto& a = c.setConstants;
      // ENDIAN PROBE, once, BEFORE the release. The swap direction is derived,
      // not measured, and a wrong one is silent: it yields finite-looking
      // garbage floats rather than an error. Print BOTH readings of c0 so the
      // log settles it — the same discipline that caught the 12-vs-8-byte
      // declaration element.
      static bool s_probed = false;
      float rawC0[4] = {0, 0, 0, 0};
      bool probeNow = false;
      if (!s_probed && a.vsBytes >= 16) {
        if (const uint8_t* p = blockRead(a.vsArena)) {
          std::memcpy(rawC0, p, sizeof(rawC0));
          probeNow = true;
        }
      }
      if (a.vsBytes) {
        if (const uint8_t* p = blockRead(a.vsArena)) {
          swapConstsInto(s_vsConst, p,
                         a.vsBytes < kVsConstBytes ? a.vsBytes : kVsConstBytes);
          s_constValid = true;
        }
        blockRelease(a.vsArena);
      }
      if (a.psBytes) {
        if (const uint8_t* p = blockRead(a.psArena)) {
          swapConstsInto(s_psConst, p,
                         a.psBytes < kPsConstBytes ? a.psBytes : kPsConstBytes);
          // iter-449v: the bool tail (producer wrote host-endian dwords).
          if (a.psBytes >= kPsConstBytes + 8u) {
            uint32_t vsB = 0, psB = 0;
            std::memcpy(&vsB, p + kPsConstBytes, 4);
            std::memcpy(&psB, p + kPsConstBytes + 4, 4);
            s_boolBits = (vsB & 0xFFu) | ((psB & 0xFFu) << 16);
          }
          s_constValid = true;
        }
        blockRelease(a.psArena);
      }
      // Neither file captured => the guest's constants for this draw are
      // unknown. Same rule as the declaration: invalidate rather than let the
      // previous draw's transform silently stand in for this one.
      if (!a.vsBytes && !a.psBytes) s_constValid = false;
      if (probeNow && s_constValid) {
        s_probed = true;
        const float* sw = reinterpret_cast<const float*>(s_vsConst);
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[S6_CONST] vsBytes=%u psBytes=%u | swapped c0=(%g,%g,%g,%g) c1.x=%g"
            " | raw c0=(%g,%g,%g,%g)",
            a.vsBytes, a.psBytes, sw[0], sw[1], sw[2], sw[3], sw[4],
            rawC0[0], rawC0[1], rawC0[2], rawC0[3]);
      }
      break;
    }
    case RenderCommandType::DrawVertices:
    case RenderCommandType::DrawIndexedVertices:
    case RenderCommandType::DrawVerticesUP:      // iter-447 S6-UP
    case RenderCommandType::BeginVertices: {
      // iter-445 S1: the TYPE histogram counts every draw of every kind, before
      // any gate — it is the answer to "does the guest draw primitives
      // hleSubmitDraw refuses outright (fans, lines, points)". The VERTEX
      // columns stay DrawVertices-only, because count means something different
      // in the other two commands (see primCensusType/primCensusVerts).
      primCensusType(c.draw.primType & ~kPrimStripConvertedBit);
      // iter-447 S6-UP: the UP command carries a genuine vertex count in
      // `count`, so its verts are valid census input too. ⚠ With SR_S6_UP on,
      // [S6_PRIM]'s vertex columns therefore include UP draws — do not compare
      // them against a gate-off roll's numbers.
      if (c.type == RenderCommandType::DrawVertices ||
          c.type == RenderCommandType::DrawVerticesUP)
        primCensusVerts(c.draw.primType & ~kPrimStripConvertedBit,
                        c.draw.count);
      // S4.3: same discipline as the plane slots — the snapshot is released as
      // soon as the executor is done with it. S4.5 consumes it before this.
      const auto& d = c.draw;
      s_cov.issued++;
      // iter-457: the admission join. One row per ISSUED draw, emitted at the
      // arm's single exit, so no path can leave a draw unaccounted for.
      const uint64_t admRec = s_admitTraceRec.load(std::memory_order_relaxed);
      const char* admOutcome = "unclassified";
      uint64_t admOrd = kAdmitRecUnknown;
      auto beCov = s_backend.acquire();
      // ⚠⚠ THE FIELD-TRUST GUARD (iter-444 S6f). Only producers that fill
      // draw.start/count with what their NAMES say are trusted:
      //  * DrawVertices — sub_820FBBF8's own snapshot.
      //  * DrawVerticesUP (iter-447) — published at the DRAIN with start=0,
      //    count=vertex count, stride=the guest's own stride.
      //  * DrawIndexedVertices WITH vertBytes>0 (iter-449) — the SR_S6_IDX
      //    producer GATHERS vertex (baseVertex + idx) per index at the hook,
      //    so start=0 and count is a genuine vertex count. vertBytes>0 is the
      //    witness that THAT producer wrote the command: the gate-off census
      //    mirror still publishes sub_820FC000's raw r5/r6 (baseVertexIndex/
      //    startIndex — NOT what the field names say, and the real index
      //    count r7 absent) with vertBytes=0, which this guard refuses by
      //    construction, exactly as it has since iter-444. Reading one of
      //    those as start/count would render garbage that counts as
      //    `recorded` — the lying-metric failure this guard exists to stop.
      const bool fieldsTrusted =
          (c.type == RenderCommandType::DrawVertices) ||
          (c.type == RenderCommandType::DrawVerticesUP) ||
          (c.type == RenderCommandType::DrawIndexedVertices &&
           d.vertBytes != 0);
      if (!fieldsTrusted && d.vertBytes) {
        static uint64_t s_trap = 0;
        if (++s_trap <= 4)
          SR_LOG_BOOT(sr::LogSeverity::Error,
              "[S6_FIELDS] %s arrived WITH a vertex snapshot (%u B) but its "
              "start/count are baseVertex/startIndex, not start/count — "
              "REFUSED. Give it its own command struct before enabling it.",
              renderCommandName(c.type), d.vertBytes);
      }
      // ⛔ DELIBERATELY `== DrawVertices`, NOT `fieldsTrusted`. The UP path
      // emits prim=13 count=4 constantly, so widening this to the trusted set
      // would classify front-end quads as MOVIE quads: it would bind the three
      // YUV planes to UI geometry AND inflate mQuads, breaking the
      // `snaps == 3 x mQuads` invariant that is the only thing telling us a
      // roll had a movie frame at all. The movie quad has ONE producer.
      const bool isMovieQuad = c.type == RenderCommandType::DrawVertices &&
                               d.primType == 13u && d.count == 4u;
      // S6f: the shape gate. The movie quad is the BAKED path; everything else
      // is behind SR_S6_WIDE and OFF BY DEFAULT — general geometry has no
      // texture slots yet (s_texSlot is populated only at TextureUnlock, i.e.
      // only for movie planes), so a widened default would paint flat-white
      // geometry OVER the working movie. Coverage is measured with the gate on;
      // the default exe keeps the verified picture.
      const bool wide = s6WideEnabled();
      // fieldsTrusted gates the SUBMIT too, not just the movie-quad test —
      // otherwise a wide roll would happily stage an indexed draw whose
      // start/count mean something else entirely.
      const bool shapeOk = fieldsTrusted && (isMovieQuad || wide);
      if (!beCov || !beCov->isInitialized() || !hleDrawEnabled()) {
        s_cov.noBackend++; admOutcome = "excluded:noBackend";
      } else if (!d.vertBytes) {
        s_cov.noVerts++; admOutcome = "excluded:noVerts";
      } else if (!shapeOk) {
        s_cov.notQuad++; admOutcome = "excluded:notQuad";
      }
      if (d.vertBytes && shapeOk) {
        if (isMovieQuad) s_quadSeen++;
        sr::gfx::D3D12Backend* be = beCov.get();
        if (be && be->isInitialized() && hleDrawEnabled()) {
          sr::gfx::D3D12Backend::HleDraw q{};
          if (!buildDraw(c, q)) {
            s_cov.noExpand++;
            admOutcome = "excluded:noExpand:noSnapshot";
            s_reject[kRejectNoSnapshot]++;   // never leave a refusal unattributed
          } else {
            // iter-450Q: the movie quad's vertices are host-synthesized to the
            // positional layout — the sidecar describes the guest's own
            // fetches, so offset pairing must not rewire this draw.
            q.hostVerts = isMovieQuad;
            // The movie quad REQUIRES all three planes: a slot of 0 is the 1x1
            // WHITE fallback, and it would paint a flat rectangle that is
            // indistinguishable from a correct render at a glance. General
            // geometry has no plane contract, so this check is quad-only.
            // iter-450c [LUM_BIND] (SR_LUM_PROBE=1): the luminance downsample
            // draw's actual texture bindings — was its scene-capture input
            // found in s_texSlot, or did it fall to the slot-0 white dummy?
            {
              static const bool s_lumBind = [] {
                const char* e = std::getenv("SR_LUM_PROBE");
                return e && e[0] == '1';
              }();
              if (s_lumBind &&
                  (s_probeCurRt == 0x03A83040u || s_probeCurRt == 0x03A83080u)) {
                static uint64_t s_lb = 0;
                if (++s_lb <= 24 || s_lb % 256 == 0)
                  SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[LUM_BIND] #%llu rt=%08X texKeys={%08X,%08X,%08X,%08X} "
                      "slots={%u,%u,%u,%u} prim=%u count=%u",
                      (unsigned long long)s_lb, s_probeCurRt,
                      s_samplerTex[0], s_samplerTex[1], s_samplerTex[2],
                      s_samplerTex[3], q.texSlot[0], q.texSlot[1],
                      q.texSlot[2], q.texSlot[3], d.primType, d.count);
              }
              // iter-450c: the WORLD pass draws paint bit-zero color while
              // writing correct depth ([LUM_PROBE] res1=0.0 with a full depth
              // resolve) — log their bindings: which sampled texture (or a
              // white-dummy miss on a MULTIPLIED slot) could zero every PS.
              if (s_lumBind && s_probeCurRt == 0x02986F60u) {
                static uint64_t s_wb2 = 0;
                if (++s_wb2 <= 40 || s_wb2 % 4096 == 0)
                  SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[WORLD_BIND] #%llu texKeys={%08X,%08X,%08X,%08X,"
                      "%08X,%08X,%08X,%08X} slots={%u,%u,%u,%u,%u,%u,%u,%u} "
                      "s14=%08X/%u s15=%08X/%u prim=%u count=%u",
                      (unsigned long long)s_wb2, s_samplerTex[0],
                      s_samplerTex[1], s_samplerTex[2], s_samplerTex[3],
                      s_samplerTex[4], s_samplerTex[5], s_samplerTex[6],
                      s_samplerTex[7], q.texSlot[0], q.texSlot[1],
                      q.texSlot[2], q.texSlot[3], q.texSlot[4], q.texSlot[5],
                      q.texSlot[6], q.texSlot[7], s_samplerTex[14],
                      q.texSlot[14], s_samplerTex[15], q.texSlot[15],
                      d.primType, d.count);
              }
            }
            const bool haveAll = q.texSlot[0] && q.texSlot[1] && q.texSlot[2];
            if (isMovieQuad && !haveAll) {
              s_cov.noPlanes++;
              admOutcome = "excluded:noPlanes";
              static uint64_t s_miss = 0;
              if (++s_miss <= 8)
                SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[S4_BIND] MISSING plane slots {%u,%u,%u} tex {%08X,%08X,%08X}"
                    " — draw would paint flat white",
                    q.texSlot[0], q.texSlot[1], q.texSlot[2],
                    s_samplerTex[0], s_samplerTex[1], s_samplerTex[2]);
            } else if (true) {
              // THE ADMISSION EVENT. Read the backend's draw ordinal
              // immediately before the call and immediately after, and assert it
              // advanced by exactly one. That makes the join an OBSERVATION at
              // the admission point rather than an assumption about ordering.
              const uint64_t ordBefore = be->drawOrdinalNow();
              const bool submitted = be->hleSubmitDraw(q);
              const uint64_t ordAfter = be->drawOrdinalNow();
              // ⚠ THE DISCONTINUITY IS LATCHED, NOT JUST ASSIGNED. The first
              // version set admOutcome here and then let BOTH later branches
              // overwrite it, so the instrument could never report the one
              // condition it exists to catch (director, iter-457). The latch
              // takes precedence over every other outcome below.
              const bool ordDiscontinuity = (ordAfter != ordBefore + 1);
              if (ordDiscontinuity) {
                static uint64_t s_ordWarn = 0;
                if (++s_ordWarn <= 8)
                  SR_LOG_BOOT(sr::LogSeverity::Error,
                      "[ADMITMAP] ordinal did not advance by one across "
                      "hleSubmitDraw: before=%llu after=%llu rec=%llu",
                      (unsigned long long)ordBefore,
                      (unsigned long long)ordAfter,
                      (unsigned long long)admRec);
              } else {
                admOrd = ordBefore;
              }
              if (!submitted) {
                // A backend reject is still an ASSIGNED ordinal, and the
                // director requires that outcome preserved rather than folded
                // into the discontinuity case.
                admOutcome = ordDiscontinuity
                    ? "excluded:ordinalDiscontinuity+backendReject"
                    : "excluded:noExpand:backendReject";
                goto admitSubmitFailed;
              }
              admOutcome = ordDiscontinuity ? "excluded:ordinalDiscontinuity"
                                            : "admitted";
              if (isMovieQuad) s_quadSubmitted++; else s_generalStaged++;
              s_cov.staged++;
            } else if (false) {
            admitSubmitFailed:
              // S6e/S6f: the shape passed but staging refused it. Counted
              // separately, and BY REASON, so "we skipped it" never hides
              // inside "we rendered it" — the backend is the only thing that
              // knows which of eight reasons applied.
              s_cov.noExpand++;
              const uint32_t rk = (uint32_t)be->hleLastReject();
              s_reject[rk < kBackendRejectKinds ? rk : 0]++;
            }
          }
        }
      }
      admitMapRow(admRec, admOrd, admOutcome);
      if (d.vertBytes) poolRelease(d.vertArena);
      break;
    }
    // S2 is the executor SHAPE only — every other command is counted and
    // dropped. S3/S4 fill these in against the D3D12 device.
    default:
      break;
  }
}

// ── S5: the recorder (contract in hle_video.hpp) ────────────────────────────
// Touched ONLY by the render thread, from inside the drain loop, so it needs no
// lock of its own.
// ⚠ BUMP THIS whenever a RenderCommand field changes MEANING, not just when the
// struct changes SIZE. iter-446 widened setRenderTarget/setViewport/
// setScissorRect in place: the records stayed 32 bytes, so a v1 trace would
// still have loaded — and the dword that used to be a guest VA would have been
// silently reinterpreted as a viewport X. That is the "fixture replayed the
// wrong program" defect (iter-445) with no guard, because the version was
// written and printed but never CHECKED. It is checked now.
// v3 (iter-446): clearF.stencil changed SOURCE REGISTER (r7 -> r8) after the
// PPC prologue proved r7 is the slot the f1 `Z` argument consumes and is dead.
// The field's meaning did not change, so nothing would have caught this — a v2
// trace simply carries a WRONG stencil, and R6 is about to start executing
// stencil clears from it. A field whose VALUE becomes wrong needs the bump just
// as much as one whose meaning changes.
// v4 (iter-446 §6g.12): the `resolve` command's `flags` dword became
// `destSize` and `levelSlice` became `levelSliceFlags`. Same size, different
// MEANING — a v3 trace would replay a raw Flags word as a texture extent.
// v5 (iter-446): setRenderTarget/setDepthStencilSurface gained `sizeWord`
// (surface+36, THE TRUE EXTENT). A v4 trace has that dword as zero, which
// decodes as a 1x1 surface and refuses every bind — loud, but re-record.
// iter-449, still v5 — judged against this comment's own rule, deliberately:
// DrawIndexedVertices' start/count MEANING is now producer-dependent (census
// mirror: raw baseVertex/startIndex, vertBytes=0; SR_S6_IDX gather: 0/genuine
// vertex count, vertBytes>0). vertBytes>0 discriminates the two forms on both
// the write and read side, every banked trace carries ONLY the vertBytes=0
// form, and that form replays to the same refusal under old and new readers —
// so nothing a v5 trace holds is reinterpreted, which is the failure a bump
// exists to prevent.
// v6 (iter-456p Step 3A): the VertexBufferPublish command, and SetStreamSource
// becomes a CONSUMED binding rather than a dropped record. ⚠ THE BUMP IS NOT
// ABOUT THE APPENDED TYPE - appending is safe by construction. It is about
// SetStreamSource changing from a record nothing reads into one the executor
// acts on. A v5 trace's binds are real and correctly encoded, but the buffers
// they name were never published, so a v6 reader that executed them would bind
// streams whose contents it does not have.
//
// ⛔ THE V5 CONTRACT, MANDATED BY THE DIRECTOR: a v5 trace must continue to
// replay with streams UNAVAILABLE - never with a fabricated empty buffer. An
// empty bind is not a degraded version of the real thing; it is a different
// program that draws from zeros, and it would replay as a confident wrong
// answer. `traceStreamsAvailable()` below is that gate, and it is read-only
// state set once from the header.
// ⚠ THE CONSTANTS THEMSELVES LIVE IN THE HEADER (sr::gfx::hle), so the
// self-test asserts the SAME values the reader uses. A second definition here
// would let the two drift and the v5-compatibility test would pass against a
// copy rather than against the contract.
using sr::gfx::hle::kTraceVersion;
// The OLDEST version this build will replay. v5 is accepted deliberately: every
// banked fixture is v5, and refusing them would strand the whole regression
// corpus to gain nothing - a v5 trace decodes correctly under a v6 reader
// PROVIDED its stream binds stay unexecuted, which is exactly what the
// availability gate enforces.
using sr::gfx::hle::kTraceVersionMin;
// Set once from the replayed header, read by the executor. `true` only when the
// trace actually carries VertexBufferPublish records.
inline bool& traceStreamsAvailableRef() {
  static bool v = false;
  return v;
}
bool traceStreamsAvailable() { return traceStreamsAvailableRef(); }

struct TraceHeader {
  char     magic[4];      // "SRHT"
  uint32_t version;
  uint64_t records;
  uint64_t payloadBytes;
  uint32_t presents;
  uint32_t reserved;
};

// payloadBytes duplicates cmd.textureUnlock.bytes today, but keeping it in the
// envelope means S4.3's vertex payloads need no format change.
// -- iter-456n Q4: THE TRACE INSTRUMENT MUST SEE CLASS-A PIXELS -----------
// A TextureUpload's pixels live in the pool its HANDLE names: Class A
// (unlock-origin, kClassAHandleFlag) or the shared texture pool. The recorder
// read ONLY the shared pool, so a widened publish was recorded PAYLOADLESS and
// silently de-fanged - a fixture that carries the command but not its pixels
// "proves" a regression that exists only in the fixture, which is the exact
// failure the S6i payload arm was added to prevent. No banked trace contains
// such a record (censused: 0 in the v5 fixture), so nothing recorded so far is
// corrupt; the instrument was simply blind to the class under test.
//
// Routes on the HANDLE, not on bit 26: the handle is the ground truth of which
// pool owns the bytes, so this can never read the wrong arena. Bit 26 is what
// the EXECUTOR routes on, so a disagreement between them is a real defect and
// says so out loud rather than quietly returning nullptr.
const uint8_t* uploadPayloadRead(const RenderCommand& c) {
  const uint32_t h = c.textureUpload.arenaOffset;
  const bool classA = uploadIsClassA(h);
  const bool bit26 = (c.textureUpload.fmtBits & kUploadUnlockOriginBit) != 0;
  if (!uploadFlagAgrees(h, c.textureUpload.fmtBits)) {
    static uint32_t s_warned = 0;
    if (++s_warned <= 8)
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[HLE_TRACE] upload handle/flag DISAGREE: key=%08X handle=%08X "
          "classA=%d bit26=%d - reading from the handle's own pool",
          c.textureUpload.key, h, classA ? 1 : 0, bit26 ? 1 : 0);
  }
  return classA ? classARead(h) : texRead(h);
}

struct TraceRecord {
  RenderCommand cmd;
  uint32_t payloadBytes;
  uint32_t reserved;
  uint64_t payloadHash;
};

struct Recorder {
  FILE* f;
  bool  armed;      // waiting for movie activity
  bool  done;
  uint64_t records;
  uint64_t payloadBytes;
  uint32_t presents;
  uint32_t maxPresents;
  // iter-458 (H-REC): the arm-conflict verdict, evaluated ONCE per recorder
  // lifecycle (-1 = not yet). resetRecorderForTest() clears it, so a test that
  // changes the environment and resets still drives a fresh evaluation.
  int8_t armConflict;
  Recorder()
      : f(nullptr), armed(false), done(false), records(0), payloadBytes(0),
        presents(0), maxPresents(4), armConflict(-1) {}
};

Recorder& recorder() {
  static Recorder s_r;
  return s_r;
}

// iter-458 (Astra 014): the recorder's open state, PUBLISHED for other threads
// (the present-dump path reads it to capture dense GPU frames over exactly the
// trace window). Set after a successful open, cleared at every close site;
// R.f itself is never read cross-thread.
std::atomic<bool> g_traceOpenPub{false};
// iter-458 (Astra 062): how many times the live recorder has CLOSED - sticky, so
// the kernel's H-UNIQ seal can poll it without missing a short capture window.
std::atomic<uint32_t> g_traceCloseCount{0};
// Astra 063: called SYNCHRONOUSLY at the recorder's close (the kernel's H-UNIQ
// seal - the acquisition endpoint - registers it); then the count publishes.
std::atomic<void (*)()> g_traceCloseHook{nullptr};
void noteRecorderClosed() {
  if (auto h = g_traceCloseHook.load(std::memory_order_acquire)) h();
  g_traceCloseCount.fetch_add(1, std::memory_order_release);
}

void traceWriteHeader(Recorder& R) {
  TraceHeader h{};
  h.magic[0] = 'S'; h.magic[1] = 'R'; h.magic[2] = 'H'; h.magic[3] = 'T';
  h.version = kTraceVersion;
  h.records = R.records;
  h.payloadBytes = R.payloadBytes;
  h.presents = R.presents;
  std::fseek(R.f, 0, SEEK_SET);
  std::fwrite(&h, sizeof(h), 1, R.f);
}

const char* g_tracePathOverride = nullptr;   // test-only

const char* tracePath() {
  if (g_tracePathOverride) return g_tracePathOverride;
  const char* p = std::getenv("SR_HLE_TRACE_REC");
  return (p && *p) ? p : nullptr;
}

// Called on the render thread BEFORE execute() — execute(TextureUnlock)
// releases the arena slot, so recording afterwards would read freed pixels.
// The caller hoists the enabled check out of the per-command loop: this runs
// ~27M times a run, and a guarded function-local static on that path is pure
// waste when recording is off (which is always, outside the fixture).
// iter-450h STATE SHADOW: the live stream's long-lived texture state is
// established ONCE at load — a trace window armed later never sees those
// TextureUpload/SetTexture commands, so replay silently substitutes the
// white dummy for every pre-window texture and pre-window bindings are
// invisible to any in-window census (the iter-450f blind spot; it
// manufactured the "replay paints / live doesn't" texture theory). While
// recording is ENABLED but not yet armed, retain the LAST upload/unlock
// payload per key and the LAST SetTexture per sampler; at trace open these
// are written as a PREAMBLE (existing record types — replayer unchanged).

// ── iter-450UI: the operator arm edge (contract in hle_video.hpp) ───────────
namespace {
std::atomic<bool> s_opArm{false};    // latched on the first accepted F9 edge
std::atomic<bool> s_opSnap{false};   // one-shot dual-alias snapshot request
}  // namespace

// ── THE WINDOW BOUNDARY, AS ONE PRODUCTION OPERATION ──────────────────────
// Writes the WINDOW-START marker last in the preamble, then — for the
// pre-treatment arm only — the triggering command itself, which is the first
// treatment event. Both live recording and the bounded self-test call THIS, so
// a test cannot pass against an ordering production does not perform.
bool writeWindowStartAndTrigger(FILE* f, uint64_t& records,
                                const RenderCommand& trigger, bool preTreatArm) {
  TraceRecord marker{};
  marker.cmd.type = RenderCommandType::OwnershipRestore;
  marker.cmd.ownershipRestore.key = 0u;
  std::fwrite(&marker, sizeof(marker), 1, f);
  records++;
  // ⚠ THE WINDOW STARTS HERE, inside the one boundary operation. It used to be
  // a separate call beside this one, which meant deleting it from production
  // changed nothing any test could see.
  beginWitnessWindow();
  if (!openingCommandIsRecorded(trigger.type == RenderCommandType::Present,
                                preTreatArm))
    return false;
  TraceRecord trig{};
  trig.cmd = trigger;
  std::fwrite(&trig, sizeof(trig), 1, f);
  records++;
  return true;
}

void traceRecord(const RenderCommand& c) {
  // iter-458 (H-REC): the path is looked up only where the file is opened.
  // tracePath() is a getenv, and this function runs for every command.
  const char* s_path = nullptr;
  Recorder& R = recorder();
  if (R.done) return;


  // iter-450UF texture-key arm (default OFF). Physical-normalised like every
  // other key in this file, so the caller may pass either window's form.
  // iter-456n Q4: THE UNLOCK-ORIGIN ARM (default OFF).
  // The texkey arm needs a PHYSICAL ADDRESS known in advance, but the class
  // under test here is defined by a FLAG, not an address: "an upload whose
  // pixels came from a guest Unlock". Physical keys also move between rolls,
  // so arming on one is a guess about the era dressed up as a fact. This arms
  // on the fact itself - the Nth bit-26 upload has entered the shadow - so the
  // preamble written at trace open NECESSARILY contains the records the
  // derived keep/drop controls are about. Without it a fixture can only ever
  // be censused for the class after the fact, which is how w1 was banked with
  // zero of them.
  // iter-450UG: optional ":<n>" HIT TARGET — "<hex>[:<n>]". Arms on the Nth
  // payload for the key, i.e. capture version N-1 under the refresh gate, so a
  // fixture can be banked on a LATER atlas version rather than the first.
  // Default 1 = iter-450UF behaviour exactly.
  // ⚠ The shadow OVERWRITES per key (see below), so the preamble carries only
  // the LATEST payload — version N and N+1 cannot both be banked in one
  // preamble. Arm on the one you want.
  // iter-450UI: the OPERATOR arm outranks every programmatic arm. It is the
  // only trigger that means "the thing under test is on screen right now",
  // which is precisely what no guest-state or atlas-version signal can say for
  // this era (contract in hle_video.hpp). Function scope so the arm block and
  // the OPENED line agree about which arm fired.
  static const bool s_hotkey = [] {
    const char* e = std::getenv("SR_HLE_TRACE_HOTKEY");
    return e && e[0] == '1';   // DEFAULT OFF
  }();

  // The state shadow is NO LONGER written here. It is written by the executor
  // (shadowNoteApplied) at the point an upload has actually reached the host
  // texture, so the preamble is APPLIED state rather than SUBMITTED state.
  // Bindings still latch here: a SetTexture has no accept/refuse decision, so
  // recording it at submit time is already applied state.
  if (!R.f) {
    if (c.type == RenderCommandType::SetTexture &&
        c.setTexture.sampler < 20) {
      s_shadowBind[c.setTexture.sampler] = c;
      s_shadowBindValid[c.setTexture.sampler] = true;
    }
  }

  if (!R.f) {
    if (R.armConflict < 0) R.armConflict = g_armConflict() ? 1 : 0;
    if (R.armConflict) {
      static bool s_said = false;
      if (!s_said) {
        s_said = true;
        SR_LOG_BOOT(sr::LogSeverity::Error,
            "[HLE_TRACE] REFUSING to record: %d trace arms are enabled at once. "
            "Exactly one may be. Silently picking one would bank a fixture "
            "armed by a rule nobody chose, and the artifact could not show it.",
            g_armCountNow());
      }
      return;
    }
    // ARM ONLY ONCE THE MOVIE IS LIVE, and start on a Present so the trace
    // begins on a clean frame boundary. Recording from command 0 would capture
    // a boot with no plane data at all — a trace that replays to black and
    // proves nothing.
    if (!R.armed) {
      // ⚠ WHERE the window sits matters as much as how long it is. Arming on
      // the first movie activity puts the trace at the EA logo, seconds into
      // boot, where the guest has issued almost NO general geometry: a 60-
      // present capture there held 36 draws against ~90/present measured live,
      // and a wide-on/wide-off A/B across it showed ZERO pixel difference —
      // which says nothing about general geometry and everything about the
      // window. SR_HLE_TRACE_ARM_GEOM=<n> arms only once n general (non-movie)
      // draws have been staged, so the fixture contains what is being tested.
      static const uint64_t s_geomArm = [] {
        const char* e = std::getenv("SR_HLE_TRACE_ARM_GEOM");
        const long long v = e ? std::atoll(e) : 0;
        return (uint64_t)(v > 0 ? v : 0);
      }();
      if (s_hotkey) {
        if (!operatorArmRequested()) {
          // Unconditional WAITING, same contract as the texture-key arm: a
          // silent arm that never fires must never be mistakable for one that
          // fired and recorded nothing.
          if (c.type == RenderCommandType::Present) {
            static uint64_t s_hkWait = 0;
            if ((++s_hkWait % 240) == 1)
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[TRACE_HOTKEY] WAITING for F9 — not armed, trace NOT open "
                  "(presents waited=%llu)", (unsigned long long)s_hkWait);
          }
          return;
        }
        // armed by the operator: fall through to the Present check below
      } else if (g_texKeyArm) {
        if (g_texKeyHits < g_texKeyWant) {
          // ⚠ UNCONDITIONAL SIGNAL when enabled. A silent arm that never fires
          // is indistinguishable from one that fired and recorded nothing —
          // that is exactly how iter-450UE banked a fixture missing the very
          // texture under test. Throttled, but it always speaks.
          if (c.type == RenderCommandType::Present) {
            static uint64_t s_waitN = 0;
            if ((++s_waitN % 240) == 1)
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[TRACE_TEXKEY] WAITING key=%08X hits=%llu/%llu — not armed, "
                  "trace NOT open (presents waited=%llu)",
                  g_texKeyArm, (unsigned long long)g_texKeyHits,
                  (unsigned long long)g_texKeyWant,
                  (unsigned long long)s_waitN);
          }
          return;
        }
      } else if (g_preTreatArm) {
        // Open on the FIRST descriptor-valid Resolve, and only from a provably
        // pre-treatment state.
        const bool trigger =
            c.type == RenderCommandType::Resolve &&
            (c.resolve.levelSliceFlags & kResolveDescValidBit) != 0;
        if (!trigger) return;
        if (!preTreatStateIsClean()) {
          if (++g_preTreatRefused <= 8)
            SR_LOG_BOOT(sr::LogSeverity::Error,
                "[TRACE_PRERESOLVE] REFUSED to open at the first "
                "descriptor-valid Resolve: state is NOT pre-treatment "
                "(resolveOwned=%zu). A fixture opened here would bake the "
                "treatment into its own preamble.",
                s_resolveOwned.size());
          return;
        }
        g_preTreatFired = true;
      } else if (g_resolveArmWant) {
        if (g_resolveHits < g_resolveArmWant) {
          if (c.type == RenderCommandType::Present) {
            static uint64_t s_rWait = 0;
            if ((++s_rWait % 240) == 1)
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[TRACE_RESOLVEN] WAITING hits=%llu/%llu - not armed, trace "
                  "NOT open (presents waited=%llu)",
                  (unsigned long long)g_resolveHits,
                  (unsigned long long)g_resolveArmWant,
                  (unsigned long long)s_rWait);
          }
          return;
        }
      } else if (g_unlockArmWant) {
        if (g_unlockHits < g_unlockArmWant) {
          // UNCONDITIONAL SIGNAL, same contract as the texkey arm: a silent arm
          // that never fires is indistinguishable from one that fired and
          // recorded nothing.
          if (c.type == RenderCommandType::Present) {
            static uint64_t s_uWait = 0;
            if ((++s_uWait % 240) == 1)
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[TRACE_UNLOCKN] WAITING hits=%llu/%llu - not armed, trace "
                  "NOT open (presents waited=%llu)",
                  (unsigned long long)g_unlockHits,
                  (unsigned long long)g_unlockArmWant,
                  (unsigned long long)s_uWait);
          }
          return;
        }
      } else if (s_geomArm) {
        // Same WAITING contract as texkey/unlock/resolve: a silent geom arm
        // that never reaches N is indistinguishable from one that fired and
        // wrote nothing (iter-457 cube capture: ARM_GEOM=350000, no file, no
        // WAITING line — and 350000 is still the movie era on a 1.3M-draw roll).
        if (s_generalStaged < s_geomArm) {
          if (c.type == RenderCommandType::Present) {
            static uint64_t s_gWait = 0;
            if ((++s_gWait % 240) == 1)
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[TRACE_GEOM] WAITING generalStaged=%llu/%llu - not armed, "
                  "trace NOT open (presents waited=%llu)",
                  (unsigned long long)s_generalStaged,
                  (unsigned long long)s_geomArm,
                  (unsigned long long)s_gWait);
          }
          return;
        }
      } else if (arenaStats().writes == 0) {
        return;
      }
      R.armed = true;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[HLE_TRACE] ARMED (generalStaged=%llu, movieSnaps=%llu)",
          (unsigned long long)s_generalStaged,
          (unsigned long long)arenaStats().writes);
      if (s_hotkey)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[TRACE_HOTKEY] ARMED by operator — opening at the next Present");
      else if (g_preTreatArm)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[TRACE_PRERESOLVE] ARMED at the first descriptor-valid Resolve "
            "from a verified pre-treatment state (no resolve claim, empty "
            "legacy set) - the triggering Resolve is the first treatment event");
      else if (g_resolveArmWant)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[TRACE_RESOLVEN] ARMED hits=%llu/%llu - the preamble will carry "
            "the resolve-owned state established so far",
            (unsigned long long)g_resolveHits,
            (unsigned long long)g_resolveArmWant);
      else if (g_unlockArmWant)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[TRACE_UNLOCKN] ARMED hits=%llu/%llu - the preamble will carry "
            "every unlock-origin upload shadowed so far",
            (unsigned long long)g_unlockHits,
            (unsigned long long)g_unlockArmWant);
      else if (g_texKeyArm)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[TRACE_TEXKEY] ARMED key=%08X hits=%llu/%llu matchedBytes=%u",
            g_texKeyArm, (unsigned long long)g_texKeyHits,
            (unsigned long long)g_texKeyWant, g_texKeyBytes);
    }
    // The pre-treatment arm opens on its trigger command, NOT at the next
    // Present: waiting for a frame boundary would let the triggering Resolve
    // execute first, and the checkpoint would no longer precede the treatment.
    // A mid-frame boundary supports no first-frame pixel claim, which the
    // packet states rather than papers over.
    if (!g_preTreatFired && c.type != RenderCommandType::Present) return;
    s_path = tracePath();
    R.f = std::fopen(s_path, "wb");
    if (!R.f) { R.done = true;
      SR_LOG_BOOT(sr::LogSeverity::Error, "[HLE_TRACE] cannot open %s", s_path);
      return;
    }
    g_traceOpenPub.store(true, std::memory_order_release);
    if (const char* n = std::getenv("SR_HLE_TRACE_FRAMES")) {
      const int v = std::atoi(n);
      if (v > 0) R.maxPresents = (uint32_t)v;
    }
    // iter-450UF: the OPEN half of the unconditional signal, and the shadow
    // membership test that decides whether the preamble can carry the key.
    if (g_texKeyArm) {
      auto it = s_shadowApplied.find(g_texKeyArm);
      // The shadow OVERWRITES per key, so the banked payload must be IDENTIFIED
      // by digest, never assumed from the hit count: an N+1 upload can land
      // between the Nth hit and this Present.
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[TRACE_TEXKEY] OPENED %s | key=%08X hits=%llu/%llu matchedBytes=%u "
          "inShadow=%d shadowBytes=%zu shadowDigest=%016llX",
          s_path, g_texKeyArm, (unsigned long long)g_texKeyHits,
          (unsigned long long)g_texKeyWant, g_texKeyBytes,
          it != s_shadowApplied.end() ? 1 : 0,
          it != s_shadowApplied.end() ? it->second.bytes.size() : (size_t)0,
          it != s_shadowApplied.end()
              ? (unsigned long long)sr::gfx::ShaderCache::hashUcode(
                    it->second.bytes.data(), it->second.bytes.size())
              : 0ull);
    }
    TraceHeader blank{};
    std::fwrite(&blank, sizeof(blank), 1, R.f);   // patched on close
    // iter-450h: THE PREAMBLE — write the retained pre-window state first,
    // so the replay executor starts with the same long-lived textures and
    // bindings the live stream had.
    // ⚠ ORDER (iter-456n): OWNERSHIP ROWS ARE EMITTED FIRST, BEFORE ANY PIXELS.
    // They used to follow the uploads, and then a replayed preamble upload ran
    // against EMPTY ownership and claimed under generation 0 before the restore
    // landed (measured: live unattributedFirstClaims=0, its replay 31). Pixels
    // then follow in APPLY ORDER, not map order and not command class.
    uint64_t preN = 0, preB = 0;
    const auto writeShadow = [&](const ShadowPayload& e) {
      TraceRecord rec{};
      rec.cmd = e.cmd;
      rec.payloadBytes = (uint32_t)e.bytes.size();
      rec.payloadHash =
          sr::gfx::ShaderCache::hashUcode(e.bytes.data(), rec.payloadBytes);
      std::fwrite(&rec, sizeof(rec), 1, R.f);
      std::fwrite(e.bytes.data(), 1, rec.payloadBytes, R.f);
      R.records++;
      R.payloadBytes += rec.payloadBytes;
      preN++;
      preB += rec.payloadBytes;
    };
    // iter-456n: OWNERSHIP travels too, and it is written FIRST — before any
    // pixels. It used to follow the uploads, and a replayed preamble upload
    // then ran against EMPTY ownership and claimed under generation 0 before
    // the restore landed (measured: live unattributedFirstClaims=0, its replay
    // 31). The WINDOW-START marker is written LAST, after the pixels, so the
    // decisions a replay makes while re-establishing state are discarded.
    uint32_t preOwn = 0;
    // Collected then SORTED BY KEY: byte-deterministic trace output is a
    // standing promise, and an unordered_map's iteration order is not stable
    // across runs or across allocator states.
    struct OwnRow { uint32_t key; uint64_t sig, gen, pubs; uint32_t auth; };
    std::vector<OwnRow> ownRows;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[HLE_TRACE_OWN] window OPEN (counters zeroed; values below are the "
        "PRE-window totals being discarded): digest=%016llX verdicts=%llu "
        "(noOwner=%llu recreated=%llu staleGen=%llu refuse=%llu) keys=%llu",
        (unsigned long long)unlockOwnership().verdictDigest(),
        (unsigned long long)unlockOwnership().verdictCount(),
        (unsigned long long)unlockOwnership().verdictNoOwner(),
        (unsigned long long)unlockOwnership().verdictRecreated(),
        (unsigned long long)unlockOwnership().verdictStaleGen(),
        (unsigned long long)unlockOwnership().verdictRefuse(),
        (unsigned long long)unlockOwnership().trackedKeys());
    unlockOwnership().forEachClaim(
        [&](uint32_t key, uint64_t sig, uint64_t gen, uint64_t pubs,
            Authority auth) {
          ownRows.push_back(OwnRow{key, sig, gen, pubs, (uint32_t)auth});
        });
    std::sort(ownRows.begin(), ownRows.end(),
              [](const OwnRow& a, const OwnRow& b) { return a.key < b.key; });
    for (const OwnRow& r : ownRows) {
      const auto emitOwn = [&](uint32_t key, uint64_t sig, uint64_t gen,
                               uint64_t pubs) {
          TraceRecord rec{};
          rec.cmd.type = RenderCommandType::OwnershipRestore;
          rec.cmd.ownershipRestore.key = key;
          rec.cmd.ownershipRestore.generation = (uint32_t)gen;
          rec.cmd.ownershipRestore.sigLo = (uint32_t)(sig & 0xFFFFFFFFu);
          rec.cmd.ownershipRestore.sigHi = (uint32_t)(sig >> 32);
          rec.cmd.ownershipRestore.publishes = (uint32_t)pubs;
          rec.cmd.ownershipRestore.reserved = r.auth;   // Authority kind
          std::fwrite(&rec, sizeof(rec), 1, R.f);
          R.records++;
          preOwn++;
      };
      emitOwn(r.key, r.sig, r.gen, r.pubs);
    }
    uint32_t preOwnResolve = 0;
    for (const OwnRow& r : ownRows)
      if (r.auth == (uint32_t)Authority::Resolve) preOwnResolve++;
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[HLE_TRACE] preamble ownership: %u claims restored into the trace "
        "(%u of them Authority::Resolve)%s",
        preOwn, preOwnResolve,
        (g_preTreatArm && preOwnResolve)
            ? "  <== ⛔ NOT A NEUTRAL CONTROL" : "");

    // APPLY ORDER, not map order and not command class. Sorted by the
    // monotonic sequence stamped when each write actually reached the host, so
    // the preamble reproduces the exact order the live host texture state was
    // built in - and so the trace bytes stay deterministic.
    {
      std::vector<const ShadowPayload*> ordered;
      ordered.reserve(s_shadowApplied.size());
      for (const auto& kv : s_shadowApplied) ordered.push_back(&kv.second);
      std::sort(ordered.begin(), ordered.end(),
                [](const ShadowPayload* a, const ShadowPayload* b) {
                  return a->seq < b->seq;
                });
      for (const ShadowPayload* e : ordered) writeShadow(*e);
    }
    uint32_t preBind = 0;
    for (uint32_t s = 0; s < 20; s++) {
      if (!s_shadowBindValid[s]) continue;
      TraceRecord rec{};
      rec.cmd = s_shadowBind[s];
      std::fwrite(&rec, sizeof(rec), 1, R.f);
      R.records++;
      preBind++;
    }
    const bool wroteTrigger =
        writeWindowStartAndTrigger(R.f, R.records, c, g_preTreatArm);
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[HLE_TRACE] preamble: %llu uploads+unlocks (%llu payload bytes), "
        "%u bindings — pre-window state now travels with the trace",
        (unsigned long long)preN, (unsigned long long)preB, preBind);
    // The shadow's job is done — recording is live from here and in-window
    // commands land in the trace directly.
    s_shadowApplied.clear();
    s_shadowBytes = 0;
    s_shadowOpen = false;   // recording is live; in-window commands go direct
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[HLE_TRACE] recording -> %s (%u presents, armed on movie activity)",
        s_path, R.maxPresents);
    logArenaWaitSum("trace-open");
    if (s_hotkey)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[TRACE_HOTKEY] OPENED path=%s frames=%u shadowWritten=%d "
          "(preamble %llu records, %llu payload bytes, %u bindings)",
          s_path, R.maxPresents, (preN || preBind) ? 1 : 0,
          (unsigned long long)preN, (unsigned long long)preB, preBind);
    // ⚠ ALWAYS return. The boundary operation above has already serialized the
    // marker and, for the pre-treatment arm, the trigger. Falling through would
    // hand the SAME command to the generic recorder below and write it TWICE —
    // which is exactly what happened when this path merely stopped returning.
    if (wroteTrigger)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[TRACE_PRERESOLVE] the triggering %s is retained as the FIRST "
          "treatment event (written once, by the boundary operation)",
          renderCommandName(c.type));
    // Astra 069/070: the census's window opens where recording goes LIVE -
    // after the preamble, before the next command. Nothing executes in
    // between (this is still the opening command's traceRecord), so the FIFO
    // position is the same; the record count lets the analyzer map the
    // command with FIFO index E to trace record b + E - z - 2 exactly.
    if (sr::kernel::txc::enabled()) {
      s_txcWin = sr::kernel::txc::WindowCounters{};
      sr::kernel::txc::onWindowEdge(true, s_txcExec.load(std::memory_order_relaxed), s_txcWin,
                                    R.records);
    }
    return;
  }

  TraceRecord rec{};
  rec.cmd = c;
  const uint8_t* payload = nullptr;
  // iter-449s: DrawVerticesUP was MISSING here since the type appeared
  // (iter-447) — every UP draw recorded payload-LESS, so every banked trace
  // replays with the whole UI/composite pass silently dropped (land UP=0 in
  // replay vs ~100K live). The replayer's isDrawRec had the same gap.
  const bool isDraw = c.type == RenderCommandType::DrawVertices ||
                      c.type == RenderCommandType::DrawIndexedVertices ||
                      c.type == RenderCommandType::DrawVerticesUP ||
                      c.type == RenderCommandType::BeginVertices;
  if (isDraw && c.draw.vertBytes) {
    // S4.3: a draw carries its own vertex snapshot in the same envelope.
    payload = poolRead(c.draw.vertArena);
    if (payload) {
      rec.payloadBytes = c.draw.vertBytes;
      rec.payloadHash = sr::gfx::ShaderCache::hashUcode(payload, rec.payloadBytes);
    } else {
      rec.cmd.draw.vertBytes = 0;
      rec.cmd.draw.vertArena = kArenaFull;
    }
  } else if (c.type == RenderCommandType::TextureUnlock && c.textureUnlock.bytes) {
    payload = arenaRead(c.textureUnlock.arenaOffset);
    if (payload) {
      rec.payloadBytes = c.textureUnlock.bytes;
      rec.payloadHash = sr::gfx::ShaderCache::hashUcode(payload, rec.payloadBytes);
    } else {
      // Should be impossible (we run before execute frees the slot), but if the
      // pixels could not be captured the command must NOT keep claiming them:
      // replay would then arenaRelease() a recorded offset it never allocated,
      // freeing a slot another command is using.
      rec.cmd.textureUnlock.bytes = 0;
      rec.cmd.textureUnlock.arenaOffset = kArenaFull;
    }
  } else if (c.type == RenderCommandType::VertexBufferPublish &&
             c.vertexBufferPublish.sizeBytes) {
    // iter-456p Step 3A. The whole logical buffer, in ORIGINAL guest byte
    // order. Without this a v6 trace would carry publications naming buffers
    // whose bytes it does not hold — the same fabricated-empty-buffer failure
    // the v5 contract forbids, reintroduced one version later.
    payload = poolRead(c.vertexBufferPublish.arenaOffset);
    if (payload) {
      rec.payloadBytes = c.vertexBufferPublish.sizeBytes;
      rec.payloadHash = sr::gfx::ShaderCache::hashUcode(payload, rec.payloadBytes);
    } else {
      // The command must NOT keep claiming bytes it cannot produce: replay
      // would poolRelease an offset it never allocated, freeing a slot another
      // command is using. Zeroed so a reader sees a publication that carried
      // nothing rather than one that silently carried the wrong thing.
      rec.cmd.vertexBufferPublish.sizeBytes = 0;
      rec.cmd.vertexBufferPublish.arenaOffset = kArenaFull;
    }
  } else if (c.type == RenderCommandType::ClearF && c.clearF.colorBytes) {
    // Astra 085: THE CLEAR COLOUR TRAVELS WITH THE CLEAR. This branch was
    // missing, so every banked trace (hr1b: all 12 ClearF) carried
    // colorBytes=16 with NO payload and a colorBlock naming a LIVE session
    // offset - replay could not reproduce the guest's colour, and the executor
    // read and released whatever that offset held in the replay process.
    // Only the well-formed shape is serialized; anything else is recorded as
    // "no colour" rather than as a claim the payload cannot back.
    payload = (c.clearF.colorBytes == kClearFColorBytes)
                  ? blockRead(c.clearF.colorBlock) : nullptr;
    if (payload) {
      rec.payloadBytes = kClearFColorBytes;
      rec.payloadHash = sr::gfx::ShaderCache::hashUcode(payload, rec.payloadBytes);
    } else {
      rec.cmd.clearF.colorBytes = 0;
      rec.cmd.clearF.colorBlock = kBlockFull;
    }
  } else if (c.type == RenderCommandType::SetVertexDecl && c.setVertexDecl.bytes) {
    // iter-444 S6e: the declaration is now LOAD-BEARING for rendering — a
    // replay without it cannot expand a single vertex and would fixture to
    // black, i.e. report a renderer regression that does not exist.
    payload = blockRead(c.setVertexDecl.arenaOffset);
    if (payload) {
      rec.payloadBytes = c.setVertexDecl.bytes;
      rec.payloadHash = sr::gfx::ShaderCache::hashUcode(payload, rec.payloadBytes);
    } else {
      rec.cmd.setVertexDecl.bytes = 0;
      rec.cmd.setVertexDecl.arenaOffset = kBlockFull;
    }
  } else if (c.type == RenderCommandType::TextureUpload && c.textureUpload.bytes) {
    // iter-444 S6i: without this the fixture replays with NO general textures
    // and every widened draw samples the 1x1 white fallback — the fixture would
    // then "prove" a regression that exists only in the fixture.
    payload = uploadPayloadRead(c);
    if (payload) {
      // ⛔ iter-457 H1a: THE TOTAL, not `bytes`. A cube's pixels arrive as TWO
      // regions concatenated in the arena (base faces, then the mip chain), and
      // `bytes` is only the first. Serializing `bytes` here silently DROPPED
      // the mip region between the producer and the trace - the defect the
      // director caught in the design before it was ever written.
      // Non-cube uploads have mipBytes == 0, so this is byte-identical for them.
      rec.payloadBytes = (uint64_t)c.textureUpload.bytes +
                         (uint64_t)(uploadIsCube(c.textureUpload.fmtBits)
                                        ? c.textureUpload.mipBytes : 0u);
      rec.payloadHash = sr::gfx::ShaderCache::hashUcode(payload, rec.payloadBytes);
    } else {
      rec.cmd.textureUpload.bytes = 0;
      rec.cmd.textureUpload.arenaOffset = kArenaFull;
    }
  } else if (c.type == RenderCommandType::SetRenderState && c.setRenderState.bytes) {
    // Same reasoning: replaying without the register runs means every draw
    // falls back to the hardcoded transform, i.e. the fixture reproduces the
    // shard pile this iteration just removed.
    payload = blockRead(c.setRenderState.blockOffset);
    if (payload) {
      rec.payloadBytes = c.setRenderState.bytes;
      rec.payloadHash = sr::gfx::ShaderCache::hashUcode(payload, rec.payloadBytes);
    } else {
      rec.cmd.setRenderState.bytes = 0;
      rec.cmd.setRenderState.blockOffset = kBlockFull;
    }
  } else if (c.type == RenderCommandType::SetConstants &&
             (c.setConstants.vsBytes || c.setConstants.psBytes)) {
    // TWO blocks in ONE record: the envelope carries a single payload, so the
    // VS file is written first and the PS file immediately after, and replay
    // splits them back on the recorded byte counts. Concatenating is safe
    // because both sizes are in the command itself.
    static std::vector<uint8_t> s_cbuf;
    const uint8_t* vsp = c.setConstants.vsBytes
                             ? blockRead(c.setConstants.vsArena) : nullptr;
    const uint8_t* psp = c.setConstants.psBytes
                             ? blockRead(c.setConstants.psArena) : nullptr;
    const uint32_t vn = vsp ? c.setConstants.vsBytes : 0u;
    const uint32_t pn = psp ? c.setConstants.psBytes : 0u;
    if (vn || pn) {
      s_cbuf.resize(vn + pn);
      if (vn) std::memcpy(s_cbuf.data(), vsp, vn);
      if (pn) std::memcpy(s_cbuf.data() + vn, psp, pn);
      payload = s_cbuf.data();
      rec.payloadBytes = vn + pn;
      rec.payloadHash = sr::gfx::ShaderCache::hashUcode(payload, rec.payloadBytes);
    }
    // Never let the record claim bytes the payload does not carry.
    rec.cmd.setConstants.vsBytes = vn;
    rec.cmd.setConstants.psBytes = pn;
    if (!vn) rec.cmd.setConstants.vsArena = kBlockFull;
    if (!pn) rec.cmd.setConstants.psArena = kBlockFull;
  }
  std::fwrite(&rec, sizeof(rec), 1, R.f);
  if (rec.payloadBytes) {
    std::fwrite(payload, 1, rec.payloadBytes, R.f);
    R.payloadBytes += rec.payloadBytes;
  }
  R.records++;

  if (c.type == RenderCommandType::Present) {
    R.presents++;
    // Patch + FLUSH the header after EVERY present, not only at the end.
    // run_recomp_x.ps1 KILLS the process at its timeout, so a capture that has
    // not yet reached maxPresents would otherwise be left with a zeroed header
    // and replay as an EMPTY trace — which looks like a successful run that
    // proves nothing. A killed capture now yields a valid short trace.
    const long end = std::ftell(R.f);
    traceWriteHeader(R);
    std::fseek(R.f, end, SEEK_SET);
    std::fflush(R.f);
    if (R.presents >= R.maxPresents) {
      std::fclose(R.f);
      R.f = nullptr;
      g_traceOpenPub.store(false, std::memory_order_release);
      if (sr::kernel::txc::enabled())   // before the close hook flushes the census
        sr::kernel::txc::onWindowEdge(false, s_txcExec.load(std::memory_order_relaxed), s_txcWin,
                                      R.records);
      noteRecorderClosed();
      R.done = true;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[HLE_TRACE] wrote %llu records / %llu payload bytes / %u presents",
          (unsigned long long)R.records, (unsigned long long)R.payloadBytes,
          R.presents);
      logArenaWaitSum("trace-close");
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[HLE_TRACE_OWN] window CLOSE: digest=%016llX verdicts=%llu "
        "(noOwner=%llu recreated=%llu staleGen=%llu refuse=%llu) keys=%llu",
        (unsigned long long)unlockOwnership().verdictDigest(),
        (unsigned long long)unlockOwnership().verdictCount(),
        (unsigned long long)unlockOwnership().verdictNoOwner(),
        (unsigned long long)unlockOwnership().verdictRecreated(),
        (unsigned long long)unlockOwnership().verdictStaleGen(),
        (unsigned long long)unlockOwnership().verdictRefuse(),
        (unsigned long long)unlockOwnership().trackedKeys());
    }
  }
}

void renderThreadMain() {
  t_awRenderThread = true;   // iter-458: the only consumer never waits
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[HLE_VIDEO] render thread up (S2: typed commands, count-only "
              "executor; device ownership lands in S3)");
  auto& Q = queue();
  std::vector<RenderCommand> batch;
  batch.reserve(8192);
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(Q.m);
      Q.cv.wait(lk, [&] { return !Q.produce.empty(); });
      batch.swap(Q.produce);
      Q.produce.clear();
    }
    const bool rec = tracePath() != nullptr;   // hoisted: ~27M commands/run
    for (const RenderCommand& c : batch) {
      if (rec) traceRecord(c);
      execute(c);
    }
    s_drained.fetch_add(batch.size(), std::memory_order_relaxed);
    s_depth.fetch_sub((uint32_t)batch.size(), std::memory_order_relaxed);
    batch.clear();
  }
}

void ensureStartedLocked(Queue& Q) {
  if (Q.started) return;
  Q.started = true;
  std::thread(renderThreadMain).detach();
}

// ── iter-445: THE PRODUCER-SEQUENCE PROBE (contract in hle_video.hpp) ───────
bool seqProbeEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_SEQPROBE");
    return e && e[0] != '0';
  }();
  return s_on;
}

std::atomic<uint32_t> s_seqOwner{0};
std::atomic<uint64_t> s_seqOpens{0}, s_seqConcurrent{0}, s_seqSplits{0},
    s_seqPublishes{0};
std::mutex s_tidMu;
uint32_t s_tidId[16] = {};
uint64_t s_tidPub[16] = {}, s_tidDraw[16] = {};
uint32_t s_tidN = 0;

uint32_t cachedTid() {
  thread_local const uint32_t s_tid = (uint32_t)GetCurrentThreadId();
  return s_tid;
}

// Index of this thread in the census table, or -1 once the table is full.
// Cached per thread so the steady-state cost is one TLS read.
int tidIndex() {
  thread_local int s_idx = -2;
  if (s_idx != -2) return s_idx;
  const uint32_t tid = cachedTid();
  std::lock_guard<std::mutex> lk(s_tidMu);
  for (uint32_t i = 0; i < s_tidN; i++)
    if (s_tidId[i] == tid) { s_idx = (int)i; return s_idx; }
  if (s_tidN < 16) { s_tidId[s_tidN] = tid; s_idx = (int)s_tidN++; return s_idx; }
  s_idx = -1;
  return -1;
}

bool isDrawType(RenderCommandType t) {
  return t == RenderCommandType::DrawVertices ||
         t == RenderCommandType::DrawIndexedVertices ||
         t == RenderCommandType::BeginVertices;
}

// iter-457 H1(a)-r4: see the header. Null in production.
std::atomic<EnqueueTapFn> s_enqueueTap{nullptr};

void publish(const RenderCommand* cmds, uint32_t count) {
  if (!count) return;
  if (EnqueueTapFn tap = s_enqueueTap.load(std::memory_order_relaxed)) {
    for (uint32_t i = 0; i < count; i++) tap(cmds[i]);
  }
  if (seqProbeEnabled()) {
    s_seqPublishes.fetch_add(1, std::memory_order_relaxed);
    const int ti = tidIndex();
    if (ti >= 0) {
      // Only this thread writes these slots, so no lock is needed for the
      // increment — the reporter reads them under s_tidMu and may see a
      // slightly stale count, which cannot change any verdict here.
      s_tidPub[ti] += count;
      for (uint32_t i = 0; i < count; i++)
        if (isDrawType(cmds[i].type)) s_tidDraw[ti]++;
    }
    const uint32_t owner = s_seqOwner.load(std::memory_order_acquire);
    if (owner && owner != cachedTid()) {
      const uint64_t sp = s_seqSplits.fetch_add(1, std::memory_order_relaxed) + 1;
      if (sp <= 8 || sp % 100000 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[SEQ_SPLIT] #%llu tid=%lu published %s INSIDE tid=%lu's open draw "
            "sequence — that draw's state can be another thread's",
            (unsigned long long)sp, (unsigned long)cachedTid(),
            renderCommandName(cmds[0].type), (unsigned long)owner);
    }
  }
  auto& Q = queue();
  {
    std::lock_guard<std::mutex> lk(Q.m);
    ensureStartedLocked(Q);
    // iter-458 (Astra 068): the census publish ordinal is taken INSIDE the FIFO
    // lock, so ordinal order IS queue order (the render thread's apply ordinal
    // counts the same commands in the same order).
    if (sr::kernel::txc::enabled()) {
      const uint64_t at = s_txcPub.load(std::memory_order_relaxed);
      for (uint32_t i = 0; i < count; i++)
        if (cmds[i].type == RenderCommandType::TextureUpload) {
          const auto& u = cmds[i].textureUpload;
          // The IMMUTABLE snapshot this command carries: the census hashes the
          // uploaded bytes themselves (Astra 069), not a later guest re-read.
          const uint8_t* pay = (!u.bytes || u.arenaOffset == kArenaFull) ? nullptr
                               : uploadIsClassA(u.arenaOffset) ? classARead(u.arenaOffset)
                                                               : texRead(u.arenaOffset);
          sr::kernel::txc::onTexPublish(u.key, u.arenaOffset, u.bytes, u.dims, u.fmtBits,
                                        at + i + 1, pay);
        }
      s_txcPub.store(at + count, std::memory_order_release);
    }
    Q.produce.insert(Q.produce.end(), cmds, cmds + count);
  }
  s_enqueued.fetch_add(count, std::memory_order_relaxed);
  const uint32_t d = s_depth.fetch_add(count, std::memory_order_relaxed) + count;
  uint32_t prev = s_maxDepth.load(std::memory_order_relaxed);
  while (d > prev &&
         !s_maxDepth.compare_exchange_weak(prev, d, std::memory_order_relaxed)) {
  }
  Q.cv.notify_one();
}

}  // namespace

// ── iter-450UI: the operator arm edge (contract in hle_video.hpp) ───────────
// Defined OUTSIDE the anonymous namespace above: the SDL thread in
// app/platform_sdl.cpp calls operatorArmRequest(), so these need external
// linkage. The atomics themselves stay file-local.
void operatorArmRequest() {
  bool expected = false;
  // CAS, so repeats after the first accepted edge are ignored by construction
  // rather than by a debounce timer that could disagree with the log.
  if (!s_opArm.compare_exchange_strong(expected, true,
                                       std::memory_order_acq_rel))
    return;
  s_opSnap.store(true, std::memory_order_release);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[TRACE_HOTKEY] HOTKEY HIT (F9) — recorder arm requested and one-shot "
      "dual-alias snapshot requested; repeats from here are ignored");
}

bool operatorArmRequested() {
  return s_opArm.load(std::memory_order_acquire);
}

bool consumeOperatorSnapshot() {
  bool expected = true;
  return s_opSnap.compare_exchange_strong(expected, false,
                                          std::memory_order_acq_rel);
}

// iter-457 H1a. RECOMPUTES both segment lengths from the geometry the record
// itself carries and refuses BY NAME on any disagreement. It never repairs,
// never truncates and never pads: a payload that disagrees with the geometry
// that described it is corrupt, and the vertex-buffer path already sets this
// precedent (a digest mismatch is refused rather than stored, above).
CubeCheck checkCubeUpload(const RenderCommand& c, uint64_t payloadBytes) {
  CubeCheck r{};
  const auto& a = c.textureUpload;
  if (!uploadIsCube(a.fmtBits)) { r.why = "notCube"; return r; }
  const uint32_t w = a.dims >> 16, h = a.dims & 0xFFFFu;
  const uint32_t fmt = a.fmtBits & 0xFFu;
  const bool tiled = ((a.fmtBits >> 8) & 1u) != 0;
  const uint32_t mipLevels = uploadMipLevels(a.fmtBits);
  const bool packed = uploadPackedMips(a.fmtBits);
  // Depth-1 rides bits 16-25 and for a cube MUST say 5, i.e. six faces. The
  // count comes from the record, and the record has to agree with the Xenos
  // invariant rather than be trusted.
  const uint32_t faces = ((a.fmtBits >> 16) & 0x3FFu) + 1u;
  if (faces != sr::gfx::cube::kCubeFaces) { r.why = "faceCount"; return r; }

  uint32_t bw = 1, bh = 1, bpb = 4;
  if (!sr::gfx::cube::cubeBlockGeom(fmt, bw, bh, bpb)) {
    r.why = "format"; return r;
  }
  const auto L = sr::gfx::cube::computeCubeLayout(fmt, w, h, a.pitch, tiled,
                                                  mipLevels, packed, bw, bh, bpb);
  if (!L.ok) { r.why = sr::gfx::cube::cubeRefuseName(L.refuse); return r; }
  if (a.bytes != L.baseSpanBytes)           { r.why = "baseBytes"; return r; }
  if (a.mipBytes != L.mipsTotalExtentBytes) { r.why = "mipBytes"; return r; }
  if (payloadBytes != (uint64_t)a.bytes + (uint64_t)a.mipBytes) {
    r.why = "payloadTotal"; return r;
  }
  r.ok = true;
  r.why = "ok";
  r.baseBytes = L.baseSpanBytes;
  r.mipBytes = L.mipsTotalExtentBytes;
  return r;
}

uint64_t cubeExecRefusedForTest() {
  return s_cubeExecRefused.load(std::memory_order_relaxed);
}

uint32_t texPoolInFlightForTest() {
  TexPool& t = texPool();
  std::lock_guard<std::mutex> lk(t.m);
  return t.inFlight;
}

uint32_t arenaInFlightForTest() {
  Arena& a = arena();
  std::lock_guard<std::mutex> lk(a.m);
  return a.inFlight;
}

uint32_t blockInFlightForTest() {
  Blocks& b = blocks();
  std::lock_guard<std::mutex> lk(b.m);
  return b.inFlight;
}

ClearFStatsForTest clearFStatsForTest() {
  ClearFStatsForTest s{};
  s.seen = s_clearFSeen;
  s.colourMissing = s_clearFColourMissing;
  s.colourMalformed = s_clearFColourMalformed;
  s.targetsMasked = s_clearFTargetsMasked;
  s.lastFlags = s_clearFLastFlags;
  s.lastColourOk = s_clearFLastColourOk;
  std::memcpy(s.lastRgba, s_clearFLastRgba, sizeof(s.lastRgba));
  return s;
}

void resetClearFStatsForTest() {
  s_clearFSeen = 0;
  s_clearFColourMissing = s_clearFColourMalformed = s_clearFTargetsMasked = 0;
  s_clearFLastFlags = 0;
  s_clearFLastColourOk = false;
  for (float& v : s_clearFLastRgba) v = 0.f;
}

void setClearSubmitTapForTest(ClearSubmitTapFn fn) { s_clearSubmitTap = fn; }

void guestClearSetEnabledForTest(int v) {
  g_guestClearOverride.store(v, std::memory_order_relaxed);
}

uint64_t nonCubeUploadEntriesForTest() {
  return s_nonCubeUploadEntries.load(std::memory_order_relaxed);
}

uint64_t cubeReplayRefusedForTest() {
  return s_cubeReplayRefused.load(std::memory_order_relaxed);
}

const char* cubeReplayLastReasonForTest() {
  const char* p = s_cubeReplayReason.load(std::memory_order_relaxed);
  return p ? p : "";
}

uint64_t cubeReplayAcceptedForTest() {
  return s_cubeReplayAccepted.load(std::memory_order_relaxed);
}

void setEnqueueTapForTest(EnqueueTapFn fn) {
  s_enqueueTap.store(fn, std::memory_order_relaxed);
}

void setCubeUploadStandInForTest(CubeUploadStandInFn fn) {
  s_cubeUploadStandIn.store(fn, std::memory_order_relaxed);
}

void resetCubeCountersForTest() {
  s_cubeExecRefused.store(0, std::memory_order_relaxed);
  s_nonCubeUploadEntries.store(0, std::memory_order_relaxed);
  s_cubeReplayRefused.store(0, std::memory_order_relaxed);
  s_cubeReplayAccepted.store(0, std::memory_order_relaxed);
  s_cubeReplayReason.store(nullptr, std::memory_order_relaxed);
}

const char* renderCommandName(RenderCommandType t) {
  switch (t) {
    case RenderCommandType::Present: return "Present";
    case RenderCommandType::Clear: return "Clear";
    case RenderCommandType::ClearF: return "ClearF";
    case RenderCommandType::SetRenderTarget: return "SetRenderTarget";
    case RenderCommandType::SetDepthStencilSurface: return "SetDepthStencilSurface";
    case RenderCommandType::SetViewport: return "SetViewport";
    case RenderCommandType::SetScissorRect: return "SetScissorRect";
    case RenderCommandType::SetStreamSource: return "SetStreamSource";
    case RenderCommandType::SetIndices: return "SetIndices";
    case RenderCommandType::SetTexture: return "SetTexture";
    case RenderCommandType::TextureObjectEvent: return "TextureObjectEvent";
    case RenderCommandType::OwnershipRestore: return "OwnershipRestore";
    case RenderCommandType::VertexBufferPublish: return "VertexBufferPublish";
    case RenderCommandType::DrawVertices: return "DrawVertices";
    case RenderCommandType::DrawIndexedVertices: return "DrawIndexedVertices";
    case RenderCommandType::BeginVertices: return "BeginVertices";
    case RenderCommandType::EndVertices: return "EndVertices";
    case RenderCommandType::TextureUnlock: return "TextureUnlock";
    case RenderCommandType::SetShaders: return "SetShaders";
    case RenderCommandType::SetVertexDecl: return "SetVertexDecl";
    case RenderCommandType::SetConstants: return "SetConstants";
    case RenderCommandType::TextureUpload: return "TextureUpload";
    case RenderCommandType::SetRenderState: return "SetRenderState";
    case RenderCommandType::Resolve: return "Resolve";
    case RenderCommandType::DrawVerticesUP: return "DrawVerticesUP";
    case RenderCommandType::Count: break;
  }
  return "?";
}

// iter-445 C4: PUBLIC now — the backend's ensureFrameOpen must honour the same
// switch, and two copies of an env read is how an A/B knob silently diverges.
bool ownPresentEnabled() {
  static const bool v = [] {
    const char* e = std::getenv("SR_HLE_OWN_PRESENT");
    return !e || e[0] != '0';
  }();
  return v;
}

void enqueue(const RenderCommand& cmd) { publish(&cmd, 1); }
void enqueueBulk(const RenderCommand* cmds, uint32_t count) { publish(cmds, count); }

// iter-445: the producer-sequence probe. Zero cost when the gate is off.
void seqBegin() {
  if (!seqProbeEnabled()) return;
  const uint32_t me = cachedTid();
  uint32_t expect = 0;
  if (!s_seqOwner.compare_exchange_strong(expect, me, std::memory_order_acq_rel)) {
    // expect now holds the CURRENT owner. A different thread means two draw
    // sequences are genuinely open at once; the same thread means our own
    // nesting (a draw hook re-entered), which is not the race being measured.
    if (expect != me) {
      const uint64_t c = s_seqConcurrent.fetch_add(1, std::memory_order_relaxed) + 1;
      if (c <= 8 || c % 100000 == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[SEQ_CONCURRENT] #%llu tid=%lu opened a draw sequence while "
            "tid=%lu still had one open",
            (unsigned long long)c, (unsigned long)me, (unsigned long)expect);
    }
  }
  s_seqOpens.fetch_add(1, std::memory_order_relaxed);
}

void seqEnd() {
  if (!seqProbeEnabled()) return;
  uint32_t me = cachedTid();
  // Only the owner clears it — a loser of the begin CAS must not release the
  // winner's sequence, or every later split would go unseen.
  s_seqOwner.compare_exchange_strong(me, 0u, std::memory_order_acq_rel);
}

SeqStats seqStats() {
  SeqStats s{};
  s.opens = s_seqOpens.load(std::memory_order_relaxed);
  s.concurrent = s_seqConcurrent.load(std::memory_order_relaxed);
  s.splits = s_seqSplits.load(std::memory_order_relaxed);
  s.publishes = s_seqPublishes.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(s_tidMu);
  s.tidN = s_tidN;
  for (uint32_t i = 0; i < s_tidN && i < 16; i++) {
    s.tid[i] = s_tidId[i];
    s.tidPub[i] = s_tidPub[i];
    s.tidDraw[i] = s_tidDraw[i];
  }
  return s;
}

// ── S4.2 arena ──────────────────────────────────────────────────────────────
uint32_t arenaWriteOnce(const void* src, uint32_t bytes) {
  Arena& a = arena();
  uint32_t slot = kArenaSlots;
  {
    std::lock_guard<std::mutex> lk(a.m);
    if (!src || !bytes || bytes > kArenaSlotBytes) { a.rejects++; awNote(kAwArena, false); return kArenaFull; }
    // Lazily size the arena: 48 MB is not worth committing on a run that never
    // reaches a movie (the open no-movie roll class).
    if (a.mem.empty()) {
      a.mem.resize((size_t)kArenaSlots * kArenaSlotBytes);
      a.busy.assign(kArenaSlots, 0);
    }
    // Release order equals alloc order (the queue is FIFO), so the round-robin
    // cursor normally hits a free slot first try; the scan is the safety net.
    for (uint32_t i = 0; i < kArenaSlots; i++) {
      const uint32_t s = (a.next + i) % kArenaSlots;
      if (!a.busy[s]) { slot = s; break; }
    }
    if (slot == kArenaSlots) { a.rejects++; awNote(kAwArena, true); return kArenaFull; }
    a.busy[slot] = 1;
    a.next = (slot + 1) % kArenaSlots;
    a.inFlight++;
    if (a.inFlight > a.peak) a.peak = a.inFlight;
    a.writes++;
    a.bytes += bytes;
  }
  // The copy itself is outside the lock: it is up to 900 KB and the slot is
  // already privately owned, so holding the lock across it would serialise
  // every producer behind one memcpy.
  std::memcpy(a.mem.data() + (size_t)slot * kArenaSlotBytes, src, bytes);
  return slot * kArenaSlotBytes;
}

const uint8_t* arenaRead(uint32_t offset) {
  Arena& a = arena();
  std::lock_guard<std::mutex> lk(a.m);
  if (offset % kArenaSlotBytes) return nullptr;
  const uint32_t slot = offset / kArenaSlotBytes;
  if (slot >= kArenaSlots || a.mem.empty() || !a.busy[slot]) return nullptr;
  return a.mem.data() + (size_t)slot * kArenaSlotBytes;
}

void arenaRelease(uint32_t offset) {
  if (offset == kArenaFull || (offset % kArenaSlotBytes)) return;
  const uint32_t slot = offset / kArenaSlotBytes;
  if (slot >= kArenaSlots) return;
  Arena& a = arena();
  std::lock_guard<std::mutex> lk(a.m);
  if (a.busy.empty() || !a.busy[slot]) return;   // double-release is a no-op
  a.busy[slot] = 0;
  if (a.inFlight) a.inFlight--;
  awNotify();
}

uint32_t blockWriteOnce(const void* src, uint32_t bytes) {
  Blocks& b = blocks();
  uint32_t slot = kBlockCount;
  {
    std::lock_guard<std::mutex> lk(b.m);
    if (!src || !bytes || bytes > kBlockBytes) { b.rejects++; awNote(kAwBlock, false); return kBlockFull; }
    if (b.mem.empty()) {
      b.mem.resize((size_t)kBlockCount * kBlockBytes);
      b.busy.assign(kBlockCount, 0);
    }
    for (uint32_t i = 0; i < kBlockCount; i++) {
      const uint32_t s = (b.next + i) % kBlockCount;
      if (!b.busy[s]) { slot = s; break; }
    }
    if (slot == kBlockCount) { b.rejects++; awNote(kAwBlock, true); return kBlockFull; }
    b.busy[slot] = 1;
    b.next = (slot + 1) % kBlockCount;
    b.inFlight++;
    if (b.inFlight > b.peak) b.peak = b.inFlight;
    b.writes++;
    b.bytes += bytes;
  }
  std::memcpy(b.mem.data() + (size_t)slot * kBlockBytes, src, bytes);
  return slot * kBlockBytes;
}

const uint8_t* blockRead(uint32_t offset) {
  Blocks& b = blocks();
  std::lock_guard<std::mutex> lk(b.m);
  if (offset % kBlockBytes) return nullptr;
  const uint32_t slot = offset / kBlockBytes;
  if (slot >= kBlockCount || b.mem.empty() || !b.busy[slot]) return nullptr;
  return b.mem.data() + (size_t)slot * kBlockBytes;
}

void blockRelease(uint32_t offset) {
  if (offset == kBlockFull || (offset % kBlockBytes)) return;
  const uint32_t slot = offset / kBlockBytes;
  if (slot >= kBlockCount) return;
  Blocks& b = blocks();
  std::lock_guard<std::mutex> lk(b.m);
  if (b.busy.empty() || !b.busy[slot]) return;
  b.busy[slot] = 0;
  if (b.inFlight) b.inFlight--;
  awNotify();
}

uint32_t texWriteOnce(const void* src, uint32_t bytes) {
  TexPool& t = texPool();
  uint32_t slot = kTexSlotCount;
  {
    std::lock_guard<std::mutex> lk(t.m);
    if (!src || !bytes || bytes > kTexSlotBytes) { t.rejects++; awNote(kAwTex, false); return kArenaFull; }
    if (t.mem.empty()) {
      t.mem.resize((size_t)kTexSlotCount * kTexSlotBytes);
      t.busy.assign(kTexSlotCount, 0);
    }
    for (uint32_t i = 0; i < kTexSlotCount; i++) {
      const uint32_t s2 = (t.next + i) % kTexSlotCount;
      if (!t.busy[s2]) { slot = s2; break; }
    }
    if (slot == kTexSlotCount) { t.rejects++; awNote(kAwTex, true); return kArenaFull; }
    t.busy[slot] = 1;
    t.next = (slot + 1) % kTexSlotCount;
    t.inFlight++;
    if (t.inFlight > t.peak) t.peak = t.inFlight;
    t.writes++;
    t.bytes += bytes;
  }
  std::memcpy(t.mem.data() + (size_t)slot * kTexSlotBytes, src, bytes);
  return slot * kTexSlotBytes;
}

// ── iter-456n rework: THE CLASS-A SNAPSHOT POOL (bounded, variable-size) ─────
// Fifth adjudication, failed bar 4: widened Unlock snapshots shared the 8-slot
// fixed texture pool with bind capture, and a burst exhausted it — v1 dropped
// 14 supported guest publications as `arena-full`. The ruling: "Isolate widened
// Unlock snapshots from bind capture with a bounded variable-size pool, prove
// exact once-only release, and require zero Class-A capacity drops."
//
// Shape: VARIABLE-SIZE allocations (a 2 KB icon costs 2 KB, not an 8 MB slot)
// out of a bounded TOTAL byte budget, with a free-list of retired buffers for
// reuse. Handles are opaque ids, tagged so a Class-A handle can never be
// mistaken for a texture-pool offset (they index different memory). Release is
// exact and ONCE-ONLY: a double release is COUNTED, never silently tolerated.
struct ClassAPool {
  struct Buf { std::vector<uint8_t> mem; bool busy = false; };
  std::mutex m;
  std::vector<Buf> bufs;
  uint64_t liveBytes = 0, budget = kClassABudgetBytes;
  uint64_t writes = 0, bytes = 0, rejects = 0, doubleFree = 0, badHandle = 0;
  uint32_t inFlight = 0, peak = 0;
};
ClassAPool& classAPool() { static ClassAPool p; return p; }

uint32_t classAWriteOnce(const void* src, uint32_t nbytes) {
  ClassAPool& p = classAPool();
  std::lock_guard<std::mutex> lk(p.m);
  if (!src || !nbytes || nbytes > kClassAMaxBytes) { p.rejects++; awNote(kAwClassA, false); return kArenaFull; }
  if (p.liveBytes + nbytes > p.budget) { p.rejects++; awNote(kAwClassA, true); return kArenaFull; }
  uint32_t idx = (uint32_t)p.bufs.size();
  for (uint32_t i = 0; i < p.bufs.size(); i++)
    if (!p.bufs[i].busy && p.bufs[i].mem.size() >= nbytes) { idx = i; break; }
  if (idx == p.bufs.size()) {
    if (p.bufs.size() >= kClassAMaxBufs) { p.rejects++; awNote(kAwClassA, true); return kArenaFull; }
    p.bufs.emplace_back();
  }
  ClassAPool::Buf& b = p.bufs[idx];
  if (b.mem.size() < nbytes) b.mem.resize(nbytes);
  std::memcpy(b.mem.data(), src, nbytes);
  b.busy = true;
  p.liveBytes += nbytes;
  p.inFlight++;
  if (p.inFlight > p.peak) p.peak = p.inFlight;
  p.writes++; p.bytes += nbytes;
  return kClassAHandleFlag | idx;      // opaque, never a texture-pool offset
}

const uint8_t* classARead(uint32_t handle) {
  if ((handle & kClassAHandleFlag) != kClassAHandleFlag) return nullptr;
  const uint32_t idx = handle & ~kClassAHandleFlag;
  ClassAPool& p = classAPool();
  std::lock_guard<std::mutex> lk(p.m);
  if (idx >= p.bufs.size() || !p.bufs[idx].busy) { p.badHandle++; return nullptr; }
  return p.bufs[idx].mem.data();
}

void classARelease(uint32_t handle, uint32_t nbytes) {
  if ((handle & kClassAHandleFlag) != kClassAHandleFlag) return;
  const uint32_t idx = handle & ~kClassAHandleFlag;
  ClassAPool& p = classAPool();
  std::lock_guard<std::mutex> lk(p.m);
  if (idx >= p.bufs.size()) { p.badHandle++; return; }
  if (!p.bufs[idx].busy) { p.doubleFree++; return; }   // ONCE-ONLY, counted
  p.bufs[idx].busy = false;
  p.liveBytes = (p.liveBytes >= nbytes) ? (p.liveBytes - nbytes) : 0;
  if (p.inFlight) p.inFlight--;
  awNotify();
}

ClassAStats classAStats() {
  ClassAPool& p = classAPool();
  std::lock_guard<std::mutex> lk(p.m);
  return ClassAStats{p.writes, p.bytes, p.rejects, p.doubleFree, p.badHandle,
                     p.liveBytes, p.budget, (uint32_t)p.bufs.size(),
                     p.inFlight, p.peak};
}

void shadowNoteAppliedPublic(const RenderCommand& c, const uint8_t* px,
                             uint32_t bytes, uint32_t key) {
  shadowNoteApplied(c, px, bytes, key);
}

bool shadowPeek(uint32_t key, uint32_t& type, uint64_t& digest,
                uint64_t& seq) {
  const auto it = s_shadowApplied.find(key & 0x1FFFFFFFu);
  if (it == s_shadowApplied.end()) return false;
  type = (uint32_t)it->second.cmd.type;
  digest = sr::gfx::ShaderCache::hashUcode(it->second.bytes.data(),
                                           it->second.bytes.size());
  seq = it->second.seq;
  return true;
}

bool shadowPeekFull(uint32_t key, RenderCommand& cmd, std::vector<uint8_t>& bytes,
                    uint64_t& seq) {
  const auto it = s_shadowApplied.find(key & 0x1FFFFFFFu);
  if (it == s_shadowApplied.end()) return false;
  cmd = it->second.cmd;
  bytes = it->second.bytes;
  seq = it->second.seq;
  return true;
}

uint64_t shadowKeyCount() { return (uint64_t)s_shadowApplied.size(); }

void shadowResetForTest() {
  s_shadowApplied.clear();
  s_shadowBytes = 0;
  s_shadowSeq = 0;
  s_shadowOpen = true;
}

bool resolveOwnershipV2Enabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_RESOLVE_OWNERSHIP_V2");
    return e && e[0] == '1';
  }();
  return s_on;
}

// ⚠ Defined HERE, outside this file's large anonymous namespace. Anything the
// header declares must be, or every call site sees two candidates and the build
// fails on ambiguity — which it has done three times in this file now.
// ⚠ The digest is ROLLING, so it must start where the window starts on BOTH
// sides. Without this, pre-window history contaminates the comparison and
// live/replay digest equality means nothing — the same defect the verdict
// witness had, in a second instrument.
// ── ONE window-start operation (eleventh adjudication) ────────────────────
// Both witnesses are rolling, and both must restart together at the window on
// both sides. They were reset by two separate calls at two sites, so dropping
// either one left a contaminated digest that still passed every test — the
// tests exercised the reset functions, not the wiring. There is now a single
// operation, and the production sites call only it.
void beginWitnessWindow();

void resetDispositionWindow() {
  s_dispDigest = 0;
  s_dispN = 0;
  for (uint64_t& k : s_dispByKind) k = 0;
}

// Bounded recorder self-test: drives the REAL boundary writer into a temp file
// and reports what it actually produced. Returns the two record types written,
// so the caller can assert marker -> exact trigger ordering.
bool recorderBoundarySelfTest(const char* path, bool preTreatArm,
                              RenderCommandType triggerType,
                              uint32_t triggerDestKey,
                              uint32_t out[2], uint32_t& outKey,
                              uint64_t& outRecords) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  RenderCommand trigger{};
  trigger.type = triggerType;
  trigger.resolve.destKey = triggerDestKey;
  outRecords = 0;
  writeWindowStartAndTrigger(f, outRecords, trigger, preTreatArm);
  std::fclose(f);
  admitMapFlush();
  f = std::fopen(path, "rb");
  if (!f) return false;
  out[0] = out[1] = 0xFFFFFFFFu; outKey = 0;
  TraceRecord r{};
  if (std::fread(&r, sizeof(r), 1, f) == 1) out[0] = (uint32_t)r.cmd.type;
  if (std::fread(&r, sizeof(r), 1, f) == 1) {
    out[1] = (uint32_t)r.cmd.type;
    outKey = r.cmd.resolve.destKey;
  }
  std::fclose(f);
  return true;
}

// Test-only: records a disposition WITHOUT the env gate, so a test can
// contaminate this witness and prove the window start clears it. Without this
// the disposition half of beginWitnessWindow() cannot be bound by any test —
// dropping it passed the whole suite.
void noteDispositionForTest(uint32_t key, uint64_t sig, CaptureDisposition d) {
  const uint64_t parts[8] = {++s_dispN, (uint64_t)(key & 0x1FFFFFFFu), sig, 0,
                             0, (uint64_t)d, 0, 0};
  uint64_t h = s_dispDigest;
  for (uint64_t x : parts) {
    h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
  }
  s_dispDigest = h;
  s_dispByKind[(uint32_t)d]++;
}

// THE production arm count, exported so a test can drive the real environment
// parsing through the real lookup.
int productionArmCount() { return g_armCountNow(); }

// ── DRIVERS FOR THE REAL DISPATCHERS (twelfth adjudication) ───────────────
// The director's four surviving mutations all lived in code no test reached:
// the live recorder's opening path, the replay marker dispatcher, and the
// conflict refusal. These call traceRecord() and execute() themselves, so a
// mutation anywhere in those paths shows up.
void resetRecorderForTest(const char* path) {
  Recorder& R = recorder();
  if (R.f) { std::fclose(R.f); R.f = nullptr; }
  g_traceOpenPub.store(false, std::memory_order_release);
  R.armed = false; R.done = false; R.records = 0; R.payloadBytes = 0;
  R.presents = 0; R.maxPresents = 4;
  R.armConflict = -1;
  g_tracePathOverride = path;
  g_preTreatFired = false;
  s_shadowApplied.clear();
  s_shadowBytes = 0;
  s_shadowOpen = true;
  for (bool& b : s_shadowBindValid) b = false;
}

void traceRecordForTest(const RenderCommand& c) { traceRecord(c); }

void closeRecorderForTest() {
  Recorder& R = recorder();
  if (R.f) { traceWriteHeader(R); std::fclose(R.f); R.f = nullptr; }
  g_traceOpenPub.store(false, std::memory_order_release);
  g_tracePathOverride = nullptr;
}

// Releases the path override ONLY. closeRecorderForTest() writes a header on
// the way out, which would mask a missing production header patch - the exact
// mutation that survived R14. A test that wants to prove PRODUCTION auto-close
// wrote the header must never touch the file itself.
void releaseTracePathForTest() { g_tracePathOverride = nullptr; }
void setTraceOpenPubForTest(bool open) { g_traceOpenPub.store(open, std::memory_order_release); }
uint64_t txcFifoPublished() { return s_txcPub.load(std::memory_order_acquire); }
void txcSetFifoForTest(uint64_t published, uint64_t executed) {
  s_txcPub.store(published, std::memory_order_release);
  s_txcExec.store(executed, std::memory_order_release);
}
uint64_t txcFifoExecutedForTest() { return s_txcExec.load(std::memory_order_acquire); }
bool executeDrawThroughTapForTest(const RenderCommand& c) {
  txcExecPrologue(c);
  sr::gfx::D3D12Backend::HleDraw q{};
  const bool ok = buildDraw(c, q);
  if (c.draw.vertBytes) poolRelease(c.draw.vertArena);
  return ok;
}
void txcWindowCountersForTest(uint64_t out[4]) {
  out[0] = s_txcWin.drawCmds;
  out[1] = s_txcWin.drawsAtTap;
  out[2] = s_txcWin.bindingsSeen;
  out[3] = s_txcWin.whiteoutSkips;
}

bool recorderOpenedForTest() { return recorder().f != nullptr; }
bool traceRecorderOpen() { return g_traceOpenPub.load(std::memory_order_acquire); }
uint32_t traceRecorderCloseCount() { return g_traceCloseCount.load(std::memory_order_acquire); }
void setTraceCloseHook(void (*hook)()) { g_traceCloseHook.store(hook, std::memory_order_release); }
void noteRecorderClosedForTest() { noteRecorderClosed(); }
void resetTraceCloseCountForTest() { g_traceCloseCount.store(0, std::memory_order_release); }
uint64_t recorderRecordsForTest() { return recorder().records; }

void executeForTest(const RenderCommand& c) { execute(c); }

// Seeds the LEGACY permanent resolve set, so the dirty-state refusal can be
// bound independently of an Authority::Resolve claim — they are two different
// ways for the state to be non-pre-treatment and each must refuse on its own.
void seedResolveOwnedForTest(uint32_t key) {
  s_resolveOwned.insert(key & 0x1FFFFFFFu);
}
void clearResolveOwnedForTest() { s_resolveOwned.clear(); }

void beginWitnessWindow() {
  unlockOwnership().beginVerdictWindow();
  resetDispositionWindow();
}

uint64_t dispositionDigest() { return s_dispDigest; }
uint64_t dispositionCount()  { return s_dispN; }
uint64_t dispositionOfKind(uint32_t k) { return k < 6 ? s_dispByKind[k] : 0; }

bool resolveDescCarryEnabled() {
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_RESOLVE_DESC_CARRY");
    return e && e[0] == '1';
  }();
  return s_on;
}

uint64_t resolveV2Claims()        { return s_rv2Claims; }
uint64_t resolveV2LegacyAdopted() { return s_rv2LegacyAdopted; }
uint64_t resolveV2SuccessNoEpoch(){ return s_rv2SuccessNoEpoch; }
uint64_t resolveV2CapturesRefused(){ return s_rv2RefusedNoClaim; }

UnlockOwnership& unlockOwnership() {
  static UnlockOwnership s_own;
  return s_own;
}

void noteTextureObjectEvent(uint32_t pixelKey, uint32_t reason) {
  // ⚠ ENQUEUED, never applied here. This runs on the guest/API thread; the
  // ownership maps belong to the render thread. Applying it directly was BOTH
  // an unsynchronized unordered_map mutation and an ordering bug — the render
  // thread lags the producer, so a create applied immediately can land BEFORE
  // the old object's still-queued uploads execute, and those uploads then run
  // under the new generation. As a command it arrives in exactly its stream
  // position, live and on replay. Seventh adjudication.
  if (!pixelKey) return;
  RenderCommand c{};
  c.type = RenderCommandType::TextureObjectEvent;
  c.textureObjectEvent.key = pixelKey;      // normalized owner-side
  c.textureObjectEvent.reason = reason;
  enqueue(c);
}

const uint8_t* texRead(uint32_t offset) {
  TexPool& t = texPool();
  std::lock_guard<std::mutex> lk(t.m);
  if (offset == kArenaFull || (offset % kTexSlotBytes)) return nullptr;
  const uint32_t slot = offset / kTexSlotBytes;
  if (slot >= kTexSlotCount || t.mem.empty() || !t.busy[slot]) return nullptr;
  return t.mem.data() + (size_t)slot * kTexSlotBytes;
}

void texRelease(uint32_t offset) {
  if (offset == kArenaFull || (offset % kTexSlotBytes)) return;
  const uint32_t slot = offset / kTexSlotBytes;
  if (slot >= kTexSlotCount) return;
  TexPool& t = texPool();
  std::lock_guard<std::mutex> lk(t.m);
  if (t.busy.empty() || !t.busy[slot]) return;
  t.busy[slot] = 0;
  if (t.inFlight) t.inFlight--;
  awNotify();
}

ArenaStats texStats() {
  TexPool& t = texPool();
  std::lock_guard<std::mutex> lk(t.m);
  ArenaStats s;
  s.writes = t.writes; s.bytes = t.bytes; s.rejects = t.rejects;
  s.inFlight = t.inFlight; s.peak = t.peak;
  return s;
}

// ── iter-449c: the geometry pool (contract in hle_video.hpp) ────────────────
// Same shape as the arena; its own storage so producer geometry can never
// evict a movie plane again (rejects 330→1,189 ARENA-LAPPED under the full
// armed stack, presenting as frame flicker).
Arena& geoPool() {
  static Arena s_g;
  return s_g;
}

uint32_t geoWriteOnce(const void* src, uint32_t bytes) {
  Arena& g = geoPool();
  uint32_t slot = kGeoSlots;
  {
    std::lock_guard<std::mutex> lk(g.m);
    if (!src || !bytes || bytes > kGeoSlotBytes) { g.rejects++; awNote(kAwGeo, false); return kArenaFull; }
    if (g.mem.empty()) {
      g.mem.resize((size_t)kGeoSlots * kGeoSlotBytes);
      g.busy.assign(kGeoSlots, 0);
    }
    for (uint32_t i = 0; i < kGeoSlots; i++) {
      const uint32_t s = (g.next + i) % kGeoSlots;
      if (!g.busy[s]) { slot = s; break; }
    }
    if (slot == kGeoSlots) { g.rejects++; awNote(kAwGeo, true); return kArenaFull; }
    g.busy[slot] = 1;
    g.next = (slot + 1) % kGeoSlots;
    g.inFlight++;
    if (g.inFlight > g.peak) g.peak = g.inFlight;
    g.writes++;
    g.bytes += bytes;
  }
  std::memcpy(g.mem.data() + (size_t)slot * kGeoSlotBytes, src, bytes);
  return slot * kGeoSlotBytes;
}

const uint8_t* geoRead(uint32_t offset) {
  Arena& g = geoPool();
  std::lock_guard<std::mutex> lk(g.m);
  if (offset % kGeoSlotBytes) return nullptr;
  const uint32_t slot = offset / kGeoSlotBytes;
  if (slot >= kGeoSlots || g.mem.empty() || !g.busy[slot]) return nullptr;
  return g.mem.data() + (size_t)slot * kGeoSlotBytes;
}

void geoRelease(uint32_t offset) {
  Arena& g = geoPool();
  std::lock_guard<std::mutex> lk(g.m);
  if (offset % kGeoSlotBytes) return;
  const uint32_t slot = offset / kGeoSlotBytes;
  if (slot >= kGeoSlots || g.busy.empty() || !g.busy[slot]) return;
  g.busy[slot] = 0;
  if (g.inFlight) g.inFlight--;
  awNotify();
}

ArenaStats geoStats() {
  Arena& g = geoPool();
  std::lock_guard<std::mutex> lk(g.m);
  return {g.writes, g.bytes, g.rejects, g.inFlight, g.peak};
}

// ── iter-449L: the VB residency cache (contract in hle_video.hpp) ───────────
namespace {
struct VbEntry {
  std::vector<uint8_t> bytes;   // endian-swapped (what the executor reads)
  // ⛔ vbc1 lesson (mutations==uploads, liveMB=0): the mutation probe MUST
  // compare raw-vs-raw. `bytes` is the SWAPPED copy; comparing it against
  // raw guest memory mismatches BY CONSTRUCTION for endian-2 buffers, which
  // orphaned every entry on first reuse and drove every key volatile. The
  // raw probe windows are captured at upload, before the swap.
  uint8_t rawSig[96];
  uint32_t rawSigLen;
  uint64_t key;
  bool dead;      // orphaned by a mutation re-upload; bytes stay valid
};
struct VbKeyState {
  uint32_t entryId;
  uint32_t reuploads;
  bool isVolatile;
  bool useAlt;    // vbc3 lesson: which physical window this buffer lives in,
                  // decided by whole-buffer scan at upload, probed thereafter
};

// Any nonzero byte? Word-strided with a byte tail; early-exit means real
// buffers answer in the first word.
bool vbAnyNonzero(const uint8_t* p, uint32_t n) {
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8)
    if (*reinterpret_cast<const uint64_t*>(p + i)) return true;
  for (; i < n; i++)
    if (p[i]) return true;
  return false;
}
struct VbCache {
  std::mutex m;
  // unique_ptr entries so addresses are STABLE across appends — the executor
  // reads bytes lock-free through the atomic count.
  std::unique_ptr<VbEntry> e[kVbCacheEntries];
  std::atomic<uint32_t> count{0};
  std::unordered_map<uint64_t, VbKeyState> byKey;
  uint64_t hits = 0, uploads = 0, mutations = 0, volatiles = 0, rejects = 0;
  uint64_t liveBytes = 0, deadBytes = 0;
};
VbCache& vbCache() {
  static VbCache s_c;
  return s_c;
}

// The sparse mutation probe: full compare under 96 B, else first/mid/last
// 32 B — RAW guest bytes against the RAW signature captured at upload
// (never against the swapped copy; see the rawSig note above). A probe that
// misses a mid-buffer edit is a counted risk class, not a silent one —
// `mutations` says how often buffers change at all.
void vbCaptureSig(VbEntry& e, const uint8_t* raw, uint32_t n) {
  if (n <= 96) {
    std::memcpy(e.rawSig, raw, n);
    e.rawSigLen = n;
    return;
  }
  std::memcpy(e.rawSig, raw, 32);
  std::memcpy(e.rawSig + 32, raw + n / 2, 32);
  std::memcpy(e.rawSig + 64, raw + n - 32, 32);
  e.rawSigLen = 96;
}
bool vbBytesMatch(const VbEntry& e, const uint8_t* now) {
  const size_t n = e.bytes.size();
  if (n <= 96) return std::memcmp(e.rawSig, now, n) == 0;
  return std::memcmp(e.rawSig, now, 32) == 0 &&
         std::memcmp(e.rawSig + 32, now + n / 2, 32) == 0 &&
         std::memcmp(e.rawSig + 64, now + n - 32, 32) == 0;
}

// Upload = copy + the same 8IN32 swap the pool path applies (host copy only,
// never guest memory). Other endian classes pass through unswapped, exactly
// like the pool path — identical behavior, different residence.
uint32_t vbUpload(VbCache& c, uint64_t key, const uint8_t* src,
                  uint32_t bytes, uint32_t endian) {
  const uint32_t n = c.count.load(std::memory_order_relaxed);
  if (n >= kVbCacheEntries || c.liveBytes + bytes > kVbCacheBudget) {
    c.rejects++;
    return kArenaFull;
  }
  auto ent = std::make_unique<VbEntry>();
  ent->key = key;
  ent->dead = false;
  ent->bytes.assign(src, src + bytes);
  vbCaptureSig(*ent, src, bytes);   // RAW windows, before the swap below
  if (endian == 2u) {
    uint8_t* d = ent->bytes.data();
    for (uint32_t i = 0; i + 4 <= bytes; i += 4) {
      std::swap(d[i + 0], d[i + 3]);
      std::swap(d[i + 1], d[i + 2]);
    }
  }
  c.e[n] = std::move(ent);
  c.count.store(n + 1, std::memory_order_release);
  c.liveBytes += bytes;
  c.uploads++;
  return kPoolResidentFlag | n;
}
}  // namespace

uint32_t vbCacheAcquire(uint64_t key, const uint8_t* src,
                        const uint8_t* srcAlt, uint32_t bytes,
                        uint32_t endian) {
  if (!src || !bytes || bytes > kVbCacheEntryCap) return kArenaFull;
  VbCache& c = vbCache();
  std::lock_guard<std::mutex> lk(c.m);
  // The window choice, made ONCE per upload by whole-buffer scan: prefer the
  // 0x8 window when it holds ANY data; fall to 0xA only when 0x8 is entirely
  // zero and 0xA is not (an all-zero buffer legitimately uploads zeros).
  auto chooseWin = [&](bool& useAlt) -> const uint8_t* {
    useAlt = false;
    if (vbAnyNonzero(src, bytes)) return src;
    if (srcAlt && vbAnyNonzero(srcAlt, bytes)) { useAlt = true; return srcAlt; }
    return src;
  };
  auto it = c.byKey.find(key);
  if (it == c.byKey.end()) {
    bool useAlt = false;
    const uint8_t* win = chooseWin(useAlt);
    const uint32_t tag = vbUpload(c, key, win, bytes, endian);
    if (tag != kArenaFull)
      c.byKey[key] = {tag & ~kPoolResidentFlag, 0, false, useAlt};
    return tag;
  }
  VbKeyState& ks = it->second;
  if (ks.isVolatile) { c.rejects++; return kArenaFull; }
  // Probe against the window the entry was UPLOADED from — probing the other
  // window would manufacture mutations out of the harness's unfolded views.
  const uint8_t* probeSrc = ks.useAlt ? srcAlt : src;
  VbEntry* ent = c.e[ks.entryId].get();
  if (ent && probeSrc && ent->bytes.size() == bytes &&
      vbBytesMatch(*ent, probeSrc)) {
    c.hits++;
    return kPoolResidentFlag | ks.entryId;
  }
  // Mutated (or resized, or the stored window went unreadable). Orphan the
  // old entry — commands in flight may still read it — and either re-upload
  // (re-choosing the window: a mutation may BE a window flip) or demote the
  // key to VOLATILE.
  c.mutations++;
  if (ent && !ent->dead) {
    ent->dead = true;
    c.deadBytes += ent->bytes.size();
    c.liveBytes -= ent->bytes.size();
  }
  if (++ks.reuploads > kVbCacheMaxReuploads) {
    ks.isVolatile = true;
    c.volatiles++;
    c.rejects++;
    return kArenaFull;
  }
  bool useAlt = false;
  const uint8_t* win = chooseWin(useAlt);
  const uint32_t tag = vbUpload(c, key, win, bytes, endian);
  if (tag != kArenaFull) {
    ks.entryId = tag & ~kPoolResidentFlag;
    ks.useAlt = useAlt;
  } else {
    ks.isVolatile = true;   // cache full: stop probing this key
  }
  return tag;
}

const uint8_t* vbCacheData(uint32_t tagged, uint32_t* sizeOut) {
  if (tagged == kArenaFull) return nullptr;
  const uint32_t id = tagged & ~kPoolResidentFlag;
  VbCache& c = vbCache();
  if (id >= c.count.load(std::memory_order_acquire)) return nullptr;
  const VbEntry* ent = c.e[id].get();
  if (!ent) return nullptr;
  if (sizeOut) *sizeOut = (uint32_t)ent->bytes.size();
  return ent->bytes.data();
}

VbCacheStats vbCacheStats() {
  VbCache& c = vbCache();
  std::lock_guard<std::mutex> lk(c.m);
  return {c.hits, c.uploads, c.mutations, c.volatiles, c.rejects,
          c.liveBytes, c.deadBytes, c.count.load(std::memory_order_relaxed)};
}

// Size-routed snapshot (contract in hle_video.hpp). kArenaFull is the single
// failure sentinel for ALL pools so call sites keep one check.
uint32_t poolWriteOnce(const void* src, uint32_t bytes) {
  if (bytes <= kBlockBytes) {
    const uint32_t o = blockWriteOnce(src, bytes);
    return o == kBlockFull ? kArenaFull : (o | kPoolBlockFlag);
  }
  // iter-449c: mid-size spans (UP vertices, indexed gathers) take the
  // geometry pool; only over-256 KB spans (movie planes, rare huge gathers)
  // reach the arena. A full geo pool falls back to the arena rather than
  // rejecting — the arena's own accounting then names the overflow.
  if (bytes <= kGeoSlotBytes) {
    const uint32_t o = geoWriteOnce(src, bytes);
    if (o != kArenaFull) return o | kPoolGeoFlag;
  }
  return arenaWriteOnce(src, bytes);
}

// -- iter-458 (Astra 017): the backpressure wrapper (contract at awNote) ----
bool arenaWaitEnabled() {
  const int o = g_awEnabledOverride.load(std::memory_order_relaxed);
  if (o >= 0) return o != 0;
  static const bool s_on = [] {
    const char* e = std::getenv("SR_HLE_ARENA_WAIT");
    return e && e[0] == '1';
  }();
  return s_on;
}

namespace {
void awFail(const char* reason, uint32_t bytes, uint64_t us) {
  g_awFails.fetch_add(1, std::memory_order_relaxed);
  const char* pool = kAwPoolNames[t_awPool];
  SR_LOG_BOOT(sr::LogSeverity::Error,
      "[ARENA_WAIT] FAIL reason=%s pool=%s bytes=%u waitedUs=%llu depth=%u "
      "drained=%llu - RUN INVALID: under SR_HLE_ARENA_WAIT a capacity drop is "
      "never silent", reason, pool, bytes, (unsigned long long)us,
      s_depth.load(std::memory_order_relaxed),
      (unsigned long long)s_drained.load(std::memory_order_relaxed));
  if (g_awFailHook) { g_awFailHook(reason, pool, bytes, us); return; }
  sr::Log::flush(3000);  // app/log-async: TerminateProcess runs no exit callback - drain the log first
  std::fflush(nullptr);
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(), 0xA7A1u);
#else
  std::abort();
#endif
}

template <class F>
uint32_t awRetry(uint32_t bytes, F attempt) {
  uint32_t r = attempt();
  if (r != kArenaFull || !t_awFull || !arenaWaitEnabled()) return r;
  if (t_awRenderThread) {
    // The consumer cannot wait for itself; keep the legacy reject, counted.
    const uint64_t n = g_awRenderRefusals.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4 || (n & (n - 1)) == 0)
      SR_LOG_BOOT(sr::LogSeverity::Error,
          "[ARENA_WAIT] render-thread capacity drop #%llu pool=%s bytes=%u "
          "(the consumer never waits)", (unsigned long long)n,
          kAwPoolNames[t_awPool], bytes);
    return r;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const uint64_t nWait = g_awWaits.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t idle = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(g_awMu);
      g_awWaiters.fetch_add(1, std::memory_order_acq_rel);
      g_awCv.wait_for(lk, std::chrono::milliseconds(2));
      g_awWaiters.fetch_sub(1, std::memory_order_acq_rel);
    }
    r = attempt();
    const uint64_t us = (uint64_t)std::chrono::duration_cast<
        std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if (r != kArenaFull) {
      g_awWaitsOk.fetch_add(1, std::memory_order_relaxed);
      g_awWaitUsTotal.fetch_add(us, std::memory_order_relaxed);
      uint64_t mx = g_awWaitUsMax.load(std::memory_order_relaxed);
      while (us > mx && !g_awWaitUsMax.compare_exchange_weak(mx, us)) {}
      if (nWait <= 8 || (nWait & (nWait - 1)) == 0)
        SR_LOG_BOOT(sr::LogSeverity::Warn,
            "[ARENA_WAIT] #%llu granted after %llu us (bytes=%u depth=%u) | "
            "waits=%llu ok=%llu maxUs=%llu timeouts=%llu noProgress=%llu",
            (unsigned long long)nWait, (unsigned long long)us, bytes,
            s_depth.load(std::memory_order_relaxed),
            (unsigned long long)g_awWaits.load(), (unsigned long long)g_awWaitsOk.load(),
            (unsigned long long)g_awWaitUsMax.load(), (unsigned long long)g_awTimeouts.load(),
            (unsigned long long)g_awNoProgress.load());
      return r;
    }
    if (!t_awFull) { awFail("became-invalid", bytes, us); return kArenaFull; }
    const bool pending = g_awPendingProbe
                             ? g_awPendingProbe()
                             : (s_depth.load(std::memory_order_relaxed) > 0);
    idle = pending ? 0u : idle + 1u;
    if (idle >= 25u) {   // 25 empty polls (~50-400 ms, timer-resolution bound) with nothing queued
      g_awNoProgress.fetch_add(1, std::memory_order_relaxed);
      awFail("no-progress", bytes, us);
      return kArenaFull;
    }
    if (us >= (uint64_t)g_awTimeoutMs.load(std::memory_order_relaxed) * 1000ull) {
      g_awTimeouts.fetch_add(1, std::memory_order_relaxed);
      awFail("timeout", bytes, us);
      return kArenaFull;
    }
  }
}
}  // namespace

uint32_t poolWrite(const void* src, uint32_t bytes) {
  return awRetry(bytes, [&] { return poolWriteOnce(src, bytes); });
}
uint32_t arenaWrite(const void* src, uint32_t bytes) {
  return awRetry(bytes, [&] { return arenaWriteOnce(src, bytes); });
}
uint32_t blockWrite(const void* src, uint32_t bytes) {
  return awRetry(bytes, [&] { return blockWriteOnce(src, bytes); });
}
uint32_t geoWrite(const void* src, uint32_t bytes) {
  return awRetry(bytes, [&] { return geoWriteOnce(src, bytes); });
}
uint32_t texWrite(const void* src, uint32_t bytes) {
  return awRetry(bytes, [&] { return texWriteOnce(src, bytes); });
}
uint32_t classAWrite(const void* src, uint32_t nbytes) {
  return awRetry(nbytes, [&] { return classAWriteOnce(src, nbytes); });
}

ArenaWaitStats arenaWaitStats() {
  ArenaWaitStats s{};
  for (int i = 0; i < kAwPools; i++) {
    s.exhausted[i] = g_awExhausted[i].load();
    s.invalid[i] = g_awInvalid[i].load();
  }
  s.waits = g_awWaits.load(); s.waitsOk = g_awWaitsOk.load();
  s.waitUsTotal = g_awWaitUsTotal.load(); s.waitUsMax = g_awWaitUsMax.load();
  s.timeouts = g_awTimeouts.load(); s.noProgress = g_awNoProgress.load();
  s.renderRefusals = g_awRenderRefusals.load(); s.fails = g_awFails.load();
  return s;
}
void arenaWaitResetForTest() {
  for (int i = 0; i < kAwPools; i++) { g_awExhausted[i] = 0; g_awInvalid[i] = 0; }
  g_awWaits = 0; g_awWaitsOk = 0; g_awWaitUsTotal = 0; g_awWaitUsMax = 0;
  g_awTimeouts = 0; g_awNoProgress = 0; g_awRenderRefusals = 0; g_awFails = 0;
}
void arenaWaitSetEnabledForTest(int v) { g_awEnabledOverride.store(v); }
void arenaWaitSetPendingProbeForTest(bool (*fn)()) { g_awPendingProbe = fn; }
void arenaWaitSetFailHookForTest(void (*fn)(const char*, const char*, uint32_t, uint64_t)) {
  g_awFailHook = fn;
}
void arenaWaitSetTimeoutMsForTest(uint32_t ms) { g_awTimeoutMs.store(ms); }
void arenaWaitMarkRenderThreadForTest(bool on) { t_awRenderThread = on; }

const uint8_t* poolRead(uint32_t tagged) {
  if (tagged == kArenaFull) return nullptr;       // MUST precede the flag tests
  // iter-449L: RESIDENT (both flag bits) before the single-bit tests — a
  // resident tag also has bit 31 set and would otherwise misroute to the
  // block pool with a garbage offset.
  if ((tagged & kPoolResidentFlag) == kPoolResidentFlag)
    return vbCacheData(tagged, nullptr);
  if (tagged & kPoolBlockFlag) return blockRead(tagged & ~kPoolBlockFlag);
  if (tagged & kPoolGeoFlag) return geoRead(tagged & ~kPoolGeoFlag);
  return arenaRead(tagged);
}

void poolRelease(uint32_t tagged) {
  if (tagged == kArenaFull) return;               // MUST precede the flag tests
  if ((tagged & kPoolResidentFlag) == kPoolResidentFlag) return;  // resident:
  // nothing to release — residency is the point (iter-449L).
  if (tagged & kPoolBlockFlag) blockRelease(tagged & ~kPoolBlockFlag);
  else if (tagged & kPoolGeoFlag) geoRelease(tagged & ~kPoolGeoFlag);
  else arenaRelease(tagged);
}

// ── S5: replay ──────────────────────────────────────────────────────────────
int replayTrace(const char* path, int loops, TraceStats* out) {
  if (out) *out = TraceStats{};
  // r5: for the life of this call the replay owns [S6_CUBE_SUM]. Every
  // return path below clears it.
  struct ReplayActiveGuard {
    ReplayActiveGuard() { s_replayActive.store(true, std::memory_order_release); }
    ~ReplayActiveGuard() { s_replayActive.store(false, std::memory_order_release); }
  } replayActiveGuard;
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[HLE_REPLAY] cannot open %s", path);
    return 1;
  }
  TraceHeader h{};
  if (std::fread(&h, sizeof(h), 1, f) != 1 || h.magic[0] != 'S' ||
      h.magic[1] != 'R' || h.magic[2] != 'H' || h.magic[3] != 'T') {
    SR_LOG_BOOT(sr::LogSeverity::Error, "[HLE_REPLAY] %s is not an SRHT trace", path);
    std::fclose(f);
    return 1;
  }
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[HLE_REPLAY] %s: v%u, %llu records, %llu payload bytes, %u presents",
      path, h.version, (unsigned long long)h.records,
      (unsigned long long)h.payloadBytes, h.presents);
  if (h.version > kTraceVersion || h.version < kTraceVersionMin) {
    SR_LOG_BOOT(sr::LogSeverity::Error,
        "[HLE_REPLAY] %s is trace v%u but this build speaks v%u..v%u — "
        "REFUSING. A stale trace replays a DIFFERENT PROGRAM than the one that "
        "ships; re-record with SR_HLE_TRACE_REC.", path, h.version,
        kTraceVersionMin, kTraceVersion);
    std::fclose(f);
    return 1;
  }
  // ⛔ THE V5 STREAM CONTRACT. A v5 trace carries SetStreamSource records but no
  // VertexBufferPublish, so the buffers those binds name were never captured.
  // Executing them would bind streams from nothing — a fabricated empty buffer
  // that draws zeros and reports success. Streams stay UNAVAILABLE for the
  // whole replay, and the executor refuses stream work by name rather than
  // silently substituting.
  traceStreamsAvailableRef() = (h.version >= 6);
  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[HLE_REPLAY] stream bindings %s for this trace (v%u)",
      traceStreamsAvailableRef() ? "AVAILABLE" : "UNAVAILABLE (v5: no "
      "VertexBufferPublish records — binds will be refused, never faked)",
      h.version);

  const long body = std::ftell(f);
  std::vector<uint8_t> buf;
  TraceStats st{};
  uint64_t perType[static_cast<size_t>(RenderCommandType::Count)] = {};

  // ── iter-449s AUTOPSY (SR_HLE_AUTOPSY=1, replay-only diagnostic) ─────────
  // Per-draw census aggregated by (vs, ps, tex0, rt, prim): draw count, vert
  // totals, and the union AABB of the first two floats of every vertex
  // (position-x/y by the offset-0 convention; BE in the snapshot). Separates
  // landed-but-invisible classes (degenerate/offscreen/untextured) from
  // absent ones without a per-draw readback. SR_HLE_AUTOPSY_FRAME=<n> adds a
  // full per-draw dump of present-frame n.
  const bool autopsyOn = [] {
    const char* e = std::getenv("SR_HLE_AUTOPSY");
    return e && e[0] == '1';
  }();
  const long autopsyFrame = [] {
    const char* e = std::getenv("SR_HLE_AUTOPSY_FRAME");
    return e ? std::atol(e) : -1;
  }();
  uint8_t aC0[32] = {};
  struct AutopsyRow {
    uint64_t draws = 0, verts = 0, snapBytes = 0, noSnap = 0;
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
  };
  std::map<std::tuple<uint64_t, uint64_t, uint32_t, uint32_t, uint32_t>,
           AutopsyRow>
      autopsy;
  uint64_t aVs = 0, aPs = 0;
  uint32_t aTex0 = 0, aRt = 0, aFrame = 0;
  // iter-450 SEGMENT CENSUS: per-frame, in draw ORDER, one segment per
  // (rt, depthControl, colorMask, blendControl) run — names the frame's pass
  // structure, the depth/mask/blend state every draw ran under, and the LAST
  // WRITER of each render target. Decodes the SAME registers the executor
  // reads (rsBE32/rsIdx: 0x2104 mask, 0x2200 depth, 0x2201 blend).
  struct ASeg {
    uint32_t rt, ds, draws, verts;
    uint32_t depth, mask, blend;
    uint64_t ps;
    uint32_t noV;   // draws in this segment with NO vertex snapshot =
                    // refused by the executor (the noVerts class)
  };
  std::vector<ASeg> aSegs;
  uint32_t aDs = 0, aMask = 0xFFFFFFFFu, aDepth = 0, aBlend = 0;
  bool aRsValid = false;
  // iter-450f: SR_AUTOPSY_KEY=<hex> — census every draw that has the queried
  // texture key bound at ANY sampler slot: (ps, slot, rt) → draws/noVerts.
  // Names WHICH shader actually consumes a key the live whiten-A/B convicted
  // (the bind slot alone doesn't prove the shader reads that slot).
  uint32_t aTexS[20] = {};
  const uint32_t aKeyQ = [] {
    const char* e = std::getenv("SR_AUTOPSY_KEY");
    return e && e[0] ? (uint32_t)strtoul(e, nullptr, 16) & 0x1FFFFFFFu : 0u;
  }();
  struct AKeyRow { uint32_t draws = 0, noV = 0; };
  std::map<std::tuple<uint64_t, uint32_t, uint32_t>, AKeyRow>
      aKeyRows;  // (ps, slot, rt) -> {draws, noVerts}
  // iter-450d FOG-CONSTANT decode: the world PS (63FE9C49...) computes
  // fogFactor = saturate((c32.y - distInterp) / (c32.y - c32.x)) and emits
  // lerp(fogColor(c33), scene, fogFactor). Observed live: factor==0 on every
  // fragment. Dump PS c30..c34 from the trace's own SetConstants (guest-BE).
  std::vector<uint8_t> aConsts;
  uint32_t aCvB = 0, aCpB = 0;
  uint32_t aFogLogged = 0;
  const auto beF32 = [](const uint8_t* p) {
    uint32_t u = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    float fv;
    std::memcpy(&fv, &u, 4);
    return fv;
  };

  for (int pass = 0; loops < 0 || pass < loops; pass++) {
    std::fseek(f, body, SEEK_SET);
    for (uint64_t i = 0; i < h.records; i++) {
      TraceRecord rec{};
      if (std::fread(&rec, sizeof(rec), 1, f) != 1) break;
      if (rec.payloadBytes) {
        buf.resize(rec.payloadBytes);
        if (std::fread(buf.data(), 1, rec.payloadBytes, f) != rec.payloadBytes) break;
        // Round-trip proof: the pixels must be byte-identical to capture.
        if (sr::gfx::ShaderCache::hashUcode(buf.data(), rec.payloadBytes) !=
            rec.payloadHash)
          st.hashMismatches++;
        // Re-stage into the arena so the executor sees exactly the shape it
        // sees live: an arena offset it may read and must release.
        //
        // ⚠ TYPE-CHECKED ON PURPOSE. RenderCommand is a UNION, so writing
        // textureUnlock.arenaOffset on a command of another type silently
        // corrupts a DIFFERENT field (it aliases draw.primType). Today only
        // TextureUnlock carries a payload; when S4.3 adds vertex payloads it
        // must add its own arm here rather than fall through this one.
        const bool isDrawRec =
            rec.cmd.type == RenderCommandType::DrawVertices ||
            rec.cmd.type == RenderCommandType::DrawIndexedVertices ||
            rec.cmd.type == RenderCommandType::DrawVerticesUP ||  // iter-449s
            rec.cmd.type == RenderCommandType::BeginVertices;
        // iter-444 S6e: SMALL-BLOCK payloads go back to the small-block pool.
        // Re-staging a 36-byte decl into a 1 MB arena slot would reproduce the
        // exact exhaustion this iteration removed, inside the fixture.
        if (rec.cmd.type == RenderCommandType::TextureUpload &&
            uploadIsCube(rec.cmd.textureUpload.fmtBits)) {
          // ── iter-457 H1a: THE CUBE GATE ────────────────────────────────────
          // Two independent refusals, both BY NAME, neither repairing anything.
          //
          // (1) VERSION. A v5/v6 record cannot carry a cube: `mipBytes` lives
          //     in a union dword those versions never wrote, and the producer
          //     builds its command as `RenderCommand tc;` (not `tc{}`), so that
          //     dword held indeterminate STACK BYTES that were serialized to
          //     disk. Interpreting it would be reading garbage. A pre-v7 record
          //     claiming a cube is malformed by construction - refuse, never
          //     guess, and never fall through to the 2D path.
          // (2) CONSERVATION. In v7, recompute BOTH segment lengths from the
          //     record's own geometry and refuse unless
          //     TraceRecord.payloadBytes == baseBytes + mipBytes.
          const char* why = nullptr;
          if (h.version < 7u) {
            why = "preV7CubeRecord";
          } else {
            const CubeCheck cc = checkCubeUpload(rec.cmd, rec.payloadBytes);
            if (!cc.ok) why = cc.why;
          }
          if (why) {
            s_cubeReplayRefused.fetch_add(1, std::memory_order_relaxed);
            s_cubeReplayReason.store(why, std::memory_order_relaxed);
            static uint64_t s_cubeRefused = 0;
            if (++s_cubeRefused <= 8)
              SR_LOG_BOOT(sr::LogSeverity::Error,
                  "[HLE_REPLAY] record %llu: CUBE upload REFUSED (%s) key=%08X "
                  "%ux%u pitch=%u base=%u mip=%u payload=%llu v%u - the record "
                  "is dropped, never reinterpreted as 2D",
                  (unsigned long long)i, why, rec.cmd.textureUpload.key,
                  rec.cmd.textureUpload.dims >> 16,
                  rec.cmd.textureUpload.dims & 0xFFFFu,
                  rec.cmd.textureUpload.pitch, rec.cmd.textureUpload.bytes,
                  rec.cmd.textureUpload.mipBytes,
                  (unsigned long long)rec.payloadBytes, h.version);
            rec.cmd.textureUpload.bytes = 0;
            rec.cmd.textureUpload.mipBytes = 0;
            rec.cmd.textureUpload.arenaOffset = kArenaFull;
          } else {
            s_cubeReplayAccepted.fetch_add(1, std::memory_order_relaxed);
            const uint32_t to = texWrite(buf.data(), rec.payloadBytes);
            rec.cmd.textureUpload.arenaOffset = to;
            if (to == kArenaFull) {
              rec.cmd.textureUpload.bytes = 0;
              rec.cmd.textureUpload.mipBytes = 0;
            }
          }
        } else if (rec.cmd.type == RenderCommandType::TextureUpload) {
          // iter-456n Q4: a bit-26 (unlock-origin) upload must be re-staged
          // into the pool the EXECUTOR reads it from. The executor routes on
          // bit 26 to classARead/classARelease, so putting these bytes in the
          // shared texture pool would drop the upload, count a badHandle, and
          // leak the shared slot - a replay that silently loses exactly the
          // records under test.
          const bool unlockOrigin =
              (rec.cmd.textureUpload.fmtBits & kUploadUnlockOriginBit) != 0;
          const uint32_t to = unlockOrigin
                                  ? classAWrite(buf.data(), rec.payloadBytes)
                                  : texWrite(buf.data(), rec.payloadBytes);
          rec.cmd.textureUpload.arenaOffset = to;
          if (to == kArenaFull) rec.cmd.textureUpload.bytes = 0;
        } else if (rec.cmd.type == RenderCommandType::SetRenderState) {
          const uint32_t bo = blockWrite(buf.data(), rec.payloadBytes);
          rec.cmd.setRenderState.blockOffset = bo;
          if (bo == kBlockFull) rec.cmd.setRenderState.bytes = 0;
        } else if (rec.cmd.type == RenderCommandType::SetVertexDecl) {
          const uint32_t bo = blockWrite(buf.data(), rec.payloadBytes);
          rec.cmd.setVertexDecl.arenaOffset = bo;
          if (bo == kBlockFull) rec.cmd.setVertexDecl.bytes = 0;
        } else if (rec.cmd.type == RenderCommandType::SetConstants) {
          const uint32_t vn = rec.cmd.setConstants.vsBytes;
          const uint32_t pn = rec.cmd.setConstants.psBytes;
          if (vn && vn <= rec.payloadBytes) {
            const uint32_t bo = blockWrite(buf.data(), vn);
            rec.cmd.setConstants.vsArena = bo;
            if (bo == kBlockFull) rec.cmd.setConstants.vsBytes = 0;
          }
          if (pn && vn + pn <= rec.payloadBytes) {
            const uint32_t bo = blockWrite(buf.data() + vn, pn);
            rec.cmd.setConstants.psArena = bo;
            if (bo == kBlockFull) rec.cmd.setConstants.psBytes = 0;
          }
        } else if (rec.cmd.type == RenderCommandType::ClearF) {
          // Astra 085: restage the colour into a block THIS replay owns, or
          // refuse it by name. Both sizes must be exactly one float4: a record
          // whose command and payload disagree is malformed, and neither of
          // its claims is trusted. On any refusal the recorded (live-session)
          // colorBlock is overwritten so the executor can never read or
          // release it.
          if (rec.cmd.clearF.colorBytes != kClearFColorBytes ||
              rec.payloadBytes != kClearFColorBytes) {
            st.clearFMalformed++;
            rec.cmd.clearF.colorBytes = 0;
            rec.cmd.clearF.colorBlock = kBlockFull;
          } else {
            const uint32_t bo = blockWrite(buf.data(), kClearFColorBytes);
            rec.cmd.clearF.colorBlock = bo;
            if (bo == kBlockFull) {
              st.clearFAllocFail++;
              rec.cmd.clearF.colorBytes = 0;
            } else {
              st.clearFRestaged++;
            }
          }
        } else if (rec.cmd.type == RenderCommandType::TextureUnlock) {
          const uint32_t ofs = arenaWrite(buf.data(), rec.payloadBytes);
          rec.cmd.textureUnlock.arenaOffset = ofs;
          if (ofs == kArenaFull) rec.cmd.textureUnlock.bytes = 0;
        } else if (isDrawRec) {
          // Size-routed, exactly like the live producer — otherwise replay
          // re-stages 4 KB vertex snapshots into 1 MB arena slots and
          // reproduces the exhaustion this routing removed.
          const uint32_t ofs = poolWrite(buf.data(), rec.payloadBytes);
          rec.cmd.draw.vertArena = ofs;
          if (ofs == kArenaFull) rec.cmd.draw.vertBytes = 0;
        } else {
          SR_LOG_BOOT(sr::LogSeverity::Error,
              "[HLE_REPLAY] record %llu has a payload but type=%s carries no "
              "arena field — payload DROPPED (add its arm to replayTrace)",
              (unsigned long long)i, renderCommandName(rec.cmd.type));
        }
        st.payloadBytes += rec.payloadBytes;
      } else if ((rec.cmd.type == RenderCommandType::DrawVertices ||
                  rec.cmd.type == RenderCommandType::DrawIndexedVertices ||
                  rec.cmd.type == RenderCommandType::DrawVerticesUP ||
                  rec.cmd.type == RenderCommandType::BeginVertices) &&
                 rec.cmd.draw.vertBytes) {
        // iter-449s: pre-fix traces carry UP draws whose vertArena is a LIVE
        // session offset with no serialized payload — in this process that
        // offset aliases whatever the fresh pool put there. Zero it so the
        // executor refuses instead of rendering someone else's bytes.
        rec.cmd.draw.vertBytes = 0;
        rec.cmd.draw.vertArena = kArenaFull;
      } else if (rec.cmd.type == RenderCommandType::ClearF) {
        // Astra 085: a payload-LESS ClearF. Every trace recorded before the
        // recorder carried the colour (hr1b included) has this shape with
        // colorBytes=16 and a colorBlock naming a LIVE session offset, which
        // in this process aliases whatever the fresh pool put there. The
        // colour is unavailable: say so, and make the handle unreadable and
        // unreleasable. No colour is substituted.
        if (rec.cmd.clearF.colorBytes) st.clearFMissing++;
        else st.clearFNoColour++;   // the record never claimed one
        rec.cmd.clearF.colorBytes = 0;
        rec.cmd.clearF.colorBlock = kBlockFull;
      }
      const uint32_t ti = static_cast<uint32_t>(rec.cmd.type);
      if (ti < static_cast<uint32_t>(RenderCommandType::Count)) perType[ti]++;
      if (rec.cmd.type == RenderCommandType::Present) st.presents++;
      if (rec.cmd.type == RenderCommandType::ClearF) st.clearFRecords++;
      if (autopsyOn && pass == 0) {
        switch (rec.cmd.type) {
          case RenderCommandType::SetShaders:
            aVs = ((uint64_t)rec.cmd.setShaders.vsHi << 32) |
                  rec.cmd.setShaders.vsLo;
            aPs = ((uint64_t)rec.cmd.setShaders.psHi << 32) |
                  rec.cmd.setShaders.psLo;
            break;
          case RenderCommandType::SetTexture:
            if (rec.cmd.setTexture.sampler == 0)
              aTex0 = rec.cmd.setTexture.texture;
            if (rec.cmd.setTexture.sampler < 20)
              aTexS[rec.cmd.setTexture.sampler] = rec.cmd.setTexture.texture;
            break;
          case RenderCommandType::SetConstants:
            // iter-449z: latch VS c0/c1 raw dwords — the exposure seed VS
            // (a41e583b) computes COLOR0 from c0; this decides guest-wrote-
            // zero vs executor-lost-it.
            if (rec.payloadBytes >= 32)
              std::memcpy(aC0, buf.data(), 32);
            // iter-450d: keep the full block for the fog decode.
            aConsts.assign(buf.begin(), buf.begin() + rec.payloadBytes);
            aCvB = rec.cmd.setConstants.vsBytes;
            aCpB = rec.cmd.setConstants.psBytes;
            break;
          case RenderCommandType::SetRenderTarget:
            if (rec.cmd.setRenderTarget.index == 0)
              aRt = rec.cmd.setRenderTarget.surface;
            break;
          case RenderCommandType::SetDepthStencilSurface:
            aDs = rec.cmd.setDepthStencilSurface.surface;
            break;
          case RenderCommandType::SetRenderState:
            if (rec.payloadBytes >= 236u) {
              const uint8_t* p = buf.data();
              aMask  = rsBE32(p, rsIdx1(0x2104));
              aDepth = rsBE32(p, rsIdx2(0x2200));
              aBlend = rsBE32(p, rsIdx2(0x2201));
              aRsValid = true;
            } else {
              aRsValid = false;
            }
            break;
          case RenderCommandType::Present: {
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[AUTOPSY_SEG] frame %u: %zu segments%s", aFrame,
                aSegs.size(), aRsValid ? "" : " (state INVALID at end)");
            size_t logN = aSegs.size() < 64 ? aSegs.size() : 64;
            for (size_t s = 0; s < logN; s++) {
              const ASeg& g = aSegs[s];
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[AUTOPSY_SEG] f%u s%02zu rt=%08X ds=%08X draws=%u "
                  "noVerts=%u verts=%u depth=%08X mask=%08X blend=%08X "
                  "ps=%016llX",
                  aFrame, s, g.rt, g.ds, g.draws, g.noV, g.verts, g.depth,
                  g.mask, g.blend, (unsigned long long)g.ps);
            }
            if (aSegs.size() > logN)
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[AUTOPSY_SEG] f%u ... %zu more segments elided", aFrame,
                  aSegs.size() - logN);
            aSegs.clear();
            aFrame++;
            break;
          }
          case RenderCommandType::Resolve: {
            // iter-450: RESOLVES IN FRAME ORDER — the ordering question is
            // whether the scene resolve fires BEFORE or AFTER the world pass.
            const auto& rv = rec.cmd.resolve;
            SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[AUTOPSY_RES] f%u afterSeg=%zu(draws=%u) src=%08X "
                "dest=%08X flags=%X%s",
                aFrame, aSegs.size(),
                aSegs.empty() ? 0u : aSegs.back().draws, rv.srcSurface,
                rv.destKey, (rv.levelSliceFlags >> 12) & 7u,
                (rv.levelSliceFlags & 0x8000u) ? " +clearRT" : "");
            break;
          }
          case RenderCommandType::Clear:
          case RenderCommandType::ClearF: {
            // iter-450: clears are frame-structure events — a full-screen
            // clear between passes is a prime flat-color suspect. Log the
            // first few with color when available.
            static uint64_t s_clrN = 0;
            if (++s_clrN <= 40) {
              float cr = 0, cg = 0, cb = 0, ca = 0;
              if (rec.cmd.type == RenderCommandType::ClearF &&
                  rec.payloadBytes >= 16u) {
                const auto rdBEf = [&](size_t o) {
                  uint32_t u = ((uint32_t)buf[o] << 24) |
                               ((uint32_t)buf[o + 1] << 16) |
                               ((uint32_t)buf[o + 2] << 8) | buf[o + 3];
                  float fv; std::memcpy(&fv, &u, 4); return fv;
                };
                cr = rdBEf(0); cg = rdBEf(4); cb = rdBEf(8); ca = rdBEf(12);
              }
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[AUTOPSY_CLR] f%u %s rt=%08X ds=%08X flags=%08X "
                  "color=(%.3f,%.3f,%.3f,%.3f) rawColor=%08X",
                  aFrame,
                  rec.cmd.type == RenderCommandType::ClearF ? "ClearF"
                                                            : "Clear",
                  aRt, aDs,
                  rec.cmd.type == RenderCommandType::ClearF
                      ? rec.cmd.clearF.flags : rec.cmd.clear.flags,
                  cr, cg, cb, ca,
                  rec.cmd.type == RenderCommandType::Clear
                      ? rec.cmd.clear.color : 0u);
            }
            break;
          }
          case RenderCommandType::DrawVertices:
          case RenderCommandType::DrawIndexedVertices:
          case RenderCommandType::DrawVerticesUP: {
            if (aSegs.empty() || aSegs.back().rt != aRt ||
                aSegs.back().ds != aDs || aSegs.back().depth != aDepth ||
                aSegs.back().mask != aMask || aSegs.back().blend != aBlend) {
              ASeg g{};
              g.rt = aRt; g.ds = aDs; g.depth = aDepth; g.mask = aMask;
              g.blend = aBlend; g.ps = aPs;
              aSegs.push_back(g);
            }
            aSegs.back().draws++;
            aSegs.back().verts += rec.cmd.draw.count;
            aSegs.back().ps = aPs;
            if (!rec.cmd.draw.vertBytes) aSegs.back().noV++;
            // iter-450d: fog constants of the main world PS, from the trace.
            if (aPs == 0x63FE9C492BBE69EAull && aFogLogged < 6 &&
                aCvB + aCpB <= aConsts.size() && aCpB >= 35u * 16u) {
              aFogLogged++;
              const auto beF = [&](uint32_t reg, uint32_t c) {
                const size_t o = size_t(aCvB) + size_t(reg) * 16u + c * 4u;
                uint32_t u = ((uint32_t)aConsts[o] << 24) |
                             ((uint32_t)aConsts[o + 1] << 16) |
                             ((uint32_t)aConsts[o + 2] << 8) | aConsts[o + 3];
                float fv;
                std::memcpy(&fv, &u, 4);
                return fv;
              };
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[AUTOPSY_FOG] #%u worldPS c30=(%g,%g,%g,%g) "
                  "c31=(%g,%g,%g,%g) c32=(%g,%g,%g,%g) c33=(%g,%g,%g,%g) "
                  "c34=(%g,%g,%g,%g)",
                  aFogLogged, beF(30, 0), beF(30, 1), beF(30, 2), beF(30, 3),
                  beF(31, 0), beF(31, 1), beF(31, 2), beF(31, 3), beF(32, 0),
                  beF(32, 1), beF(32, 2), beF(32, 3), beF(33, 0), beF(33, 1),
                  beF(33, 2), beF(33, 3), beF(34, 0), beF(34, 1), beF(34, 2),
                  beF(34, 3));
              // iter-450d: the VS transform palette c100+ — the fog distance
              // the PS receives is palette[100+nodeIdx].w (VS block at
              // offset 0 of the payload).
              const auto beV = [&](uint32_t reg, uint32_t c) {
                const size_t o = size_t(reg) * 16u + c * 4u;
                uint32_t u = ((uint32_t)aConsts[o] << 24) |
                             ((uint32_t)aConsts[o + 1] << 16) |
                             ((uint32_t)aConsts[o + 2] << 8) | aConsts[o + 3];
                float fv;
                std::memcpy(&fv, &u, 4);
                return fv;
              };
              if (aCvB >= 106u * 16u)
                SR_LOG_BOOT(sr::LogSeverity::Warn,
                    "[AUTOPSY_PAL] #%u vsB=%u c100=(%g,%g,%g,%g) "
                    "c101=(%g,%g,%g,%g) c102=(%g,%g,%g,%g) c103=(%g,%g,%g,%g)"
                    " c104.w=%g c105.w=%g",
                    aFogLogged, aCvB, beV(100, 0), beV(100, 1), beV(100, 2),
                    beV(100, 3), beV(101, 0), beV(101, 1), beV(101, 2),
                    beV(101, 3), beV(102, 0), beV(102, 1), beV(102, 2),
                    beV(102, 3), beV(103, 0), beV(103, 1), beV(103, 2),
                    beV(103, 3), beV(104, 3), beV(105, 3));
            }
            if (aKeyQ) {
              for (uint32_t sl = 0; sl < 20; sl++) {
                if ((aTexS[sl] & 0x1FFFFFFFu) == aKeyQ) {
                  auto& kr = aKeyRows[{aPs, sl, aRt}];
                  kr.draws++;
                  if (!rec.cmd.draw.vertBytes) kr.noV++;
                }
              }
            }
            // iter-450N [AUTOPSY_PALSEL]: per-draw palette-selector census
            // for the WORLD shaders. The VS derives its transform-palette
            // index from c4.x (idx ~ even-floor of 1/c4.x, DXIL-decoded) and
            // reads matrix rows at c[100+idx..]. A zero/garbage selected
            // entry names the transform as the fracture; healthy entries
            // acquit it and move suspicion to the strip conversion.
            {
              static const bool s_palSel = [] {
                const char* e = std::getenv("SR_AUTOPSY_PALSEL");
                return e && e[0] == '1';
              }();
              // iter-450O: the PS filter is gone — each fixture window has its
              // own era shaders, and the iter-450N list matched none of the
              // iter450o_strip draws (zero output = zero information). Instead:
              // every draw with constants, deduped by (vs, idx), capped. The
              // DXIL-proven fact this census now serves: c[100+idx] and
              // c[100+idx+1] are the per-NODE PLACEMENT TRANSFORM (2x2 XZ
              // rotation via dot2 + translation, applied BEFORE viewproj), so
              // "selector beyond the populated table" IS the slab-fracture
              // mechanism, not a cosmetic fog miss.
              static std::set<std::pair<uint64_t, int>> s_palSeen;
              static uint32_t s_psN = 0;
              if (s_palSel && rec.cmd.draw.vertBytes &&
                  aCvB >= 106u * 16u && aConsts.size() >= aCvB &&
                  s_psN < 60) {
                const auto beV2 = [&](uint32_t reg, uint32_t c) {
                  const size_t o = size_t(reg) * 16u + c * 4u;
                  if (o + 4 > aConsts.size()) return 0.0f;
                  uint32_t u = ((uint32_t)aConsts[o] << 24) |
                               ((uint32_t)aConsts[o + 1] << 16) |
                               ((uint32_t)aConsts[o + 2] << 8) | aConsts[o + 3];
                  float fv;
                  std::memcpy(&fv, &u, 4);
                  return fv;
                };
                const float c4x = beV2(4, 0);
                const float inv = (c4x != 0.0f) ? 1.0f / c4x : 0.0f;
                // DXIL: idx = min(155, int(floor(2*trunc(0.5*inv) + 0.5)))
                const float t = 2.0f * std::trunc(0.5f * inv) + 0.5f;
                int idx = (int)std::floor(t);
                if (idx < -256) idx = -256;
                if (idx > 255) idx = 255;
                if (idx > 155) idx = 155;
                const uint32_t pr = (uint32_t)(100 + (idx < 0 ? 0 : idx));
                const uint32_t maxReg = aCvB / 16u;
                if (s_palSeen.insert({aVs, idx}).second) {
                  // The populated-table census: which of c100..c255 hold ANY
                  // nonzero dword at THIS draw. The verdict column — a
                  // populated table with idx beyond it names a selector/
                  // staleness bug; an empty table names a write/capture gap.
                  uint32_t rows = 0, hiRow = 0;
                  for (uint32_t r = 100; r < 256 && r < maxReg; r++) {
                    bool nz = false;
                    for (uint32_t c = 0; c < 4 && !nz; c++)
                      nz = beV2(r, c) != 0.0f;
                    if (nz) { rows++; hiRow = r; }
                  }
                  s_psN++;
                  SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[AUTOPSY_PALSEL] #%u vs=%016llX ps=%08X c4.x=%g inv=%g "
                      "idx=%d palReg=c%u | table: %u rows populated, hi=c%u%s | "
                      "rowA=(%g,%g,%g,%g) rowB=(%g,%g,%g,%g) c255=(%g,%g,%g,%g)",
                      s_psN, (unsigned long long)aVs, (uint32_t)(aPs >> 32),
                      c4x, inv, idx, pr, rows, hiRow,
                      (pr + 1 > hiRow) ? " ⚠ SELECTOR BEYOND TABLE" : "",
                      beV2(pr, 0), beV2(pr, 1), beV2(pr, 2), beV2(pr, 3),
                      beV2(pr + 1, 0), beV2(pr + 1, 1), beV2(pr + 1, 2),
                      beV2(pr + 1, 3),
                      beV2(255, 0), beV2(255, 1), beV2(255, 2), beV2(255, 3));
                  // iter-450O H-A: the viewproj rows the world VS dot4s the
                  // placed position against (wvs592 %72/%78/... use c0..c3).
                  // BE-decoded here — [AUTOPSY_C0]'s raw memcpy prints LE
                  // garbage of valid data (the capture-format lesson).
                  SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[PALSEL_VP] vs=%016llX c0=(%g,%g,%g,%g) c1=(%g,%g,%g,%g) "
                      "c2=(%g,%g,%g,%g) c3=(%g,%g,%g,%g) c5=(%g,%g,%g,%g)",
                      (unsigned long long)aVs,
                      beV2(0, 0), beV2(0, 1), beV2(0, 2), beV2(0, 3),
                      beV2(1, 0), beV2(1, 1), beV2(1, 2), beV2(1, 3),
                      beV2(2, 0), beV2(2, 1), beV2(2, 2), beV2(2, 3),
                      beV2(3, 0), beV2(3, 1), beV2(3, 2), beV2(3, 3),
                      beV2(5, 0), beV2(5, 1), beV2(5, 2), beV2(5, 3));
                }
              }
            }
            auto& row = autopsy[{aVs, aPs, aTex0, aRt,
                                 rec.cmd.draw.primType}];
            row.draws++;
            row.verts += rec.cmd.draw.count;
            const uint32_t stride = rec.cmd.draw.stride;
            const uint8_t* vp = rec.payloadBytes ? buf.data() : nullptr;
            if (!vp || !rec.cmd.draw.vertBytes) row.noSnap++;
            row.snapBytes += rec.payloadBytes;
            float dMinX = 1e30f, dMinY = 1e30f, dMaxX = -1e30f,
                  dMaxY = -1e30f;
            if (vp && stride >= 8) {
              for (uint32_t off = 0; off + 8 <= rec.payloadBytes;
                   off += stride) {
                const float x = beF32(vp + off), y = beF32(vp + off + 4);
                // Sanity-filter: skip non-finite / absurd values so packed
                // non-float layouts cannot poison the union AABB.
                if (x > -1e6f && x < 1e6f && y > -1e6f && y < 1e6f) {
                  dMinX = (std::min)(dMinX, x); dMaxX = (std::max)(dMaxX, x);
                  dMinY = (std::min)(dMinY, y); dMaxY = (std::max)(dMaxY, y);
                }
              }
              row.minX = (std::min)(row.minX, dMinX);
              row.maxX = (std::max)(row.maxX, dMaxX);
              row.minY = (std::min)(row.minY, dMinY);
              row.maxY = (std::max)(row.maxY, dMaxY);
            }
            if ((long)aFrame == autopsyFrame) {
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[AUTOPSY_DRAW] f=%u %s prim=%u count=%u stride=%u "
                  "bytes=%u vs=%08X%08X ps=%08X%08X tex0=%08X rt=%08X "
                  "aabb=(%.1f,%.1f)-(%.1f,%.1f)",
                  aFrame, renderCommandName(rec.cmd.type),
                  rec.cmd.draw.primType, rec.cmd.draw.count, stride,
                  rec.payloadBytes, (uint32_t)(aVs >> 32), (uint32_t)aVs,
                  (uint32_t)(aPs >> 32), (uint32_t)aPs, aTex0, aRt,
                  dMinX, dMinY, dMaxX, dMaxY);
              float cf[8];
              std::memcpy(cf, aC0, 32);
              SR_LOG_BOOT(sr::LogSeverity::Warn,
                  "[AUTOPSY_C0] c0=(%g,%g,%g,%g) c1=(%g,%g,%g,%g)",
                  cf[0], cf[1], cf[2], cf[3], cf[4], cf[5], cf[6], cf[7]);
            }
            break;
          }
          default:
            break;
        }
      }
      // iter-457: the zero-based TRACE-RECORD ordinal, for the admission map.
      // Replay-only; the live path never sets it.
      setTraceRecordOrdinal(st.records);
      execute(rec.cmd);      // THE SAME executor the render thread runs
      setTraceRecordOrdinal(kAdmitRecUnknown);
      st.records++;
    }
  }
  std::fclose(f);
  // iter-457: both offline sidecars are written row-by-row during the replay;
  // flush them here so a caller that inspects the files before process exit
  // sees a complete map rather than a partial one.
  admitMapFlush();
  resolveExecMapFlush();
  // iter-457: one line, only when something was latched. A registry I cannot
  // observe is one I cannot verify, and this is the whole evidence for the
  // depth-source decision. Observe-only: nothing reads s_stackDepth yet.
  if (!s_stackDepth.empty()) {
    std::string sd;
    for (const auto& kv : s_stackDepth) {
      char b[48];
      std::snprintf(b, sizeof b, " %08X:d%u", kv.first, kv.second);
      sd += b;
    }
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[STACKTEX] gate=%d resolved=%llu refused=%llu | %zu stacked "
        "key(s) latched, %llu depth conflict(s):%s",
        stackedResolveEnabled() ? 1 : 0,
        (unsigned long long)s_stackedResolves.load(std::memory_order_relaxed),
        (unsigned long long)s_stackedRefused.load(std::memory_order_relaxed),
        s_stackDepth.size(),
        (unsigned long long)s_stackDepthConflicts.load(std::memory_order_relaxed),
        sd.c_str());
  }

  if (autopsyOn && !autopsy.empty()) {
    // Sorted by draw count, whole-trace aggregation. `frames` below is the
    // present count of pass 0 so per-frame rates are readable at a glance.
    std::vector<std::pair<std::tuple<uint64_t, uint64_t, uint32_t, uint32_t,
                                     uint32_t>,
                          AutopsyRow>>
        rows(autopsy.begin(), autopsy.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
      return a.second.draws > b.second.draws;
    });
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[AUTOPSY] %zu distinct (vs,ps,tex0,rt,prim) tuples across %u frames "
        "— top rows:", rows.size(), aFrame);
    size_t shown = 0;
    for (const auto& [k, r] : rows) {
      if (++shown > 48) break;
      const bool noAabb = r.minX > r.maxX;
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[AUTOPSY] draws=%-7llu verts=%-9llu noSnap=%-6llu vs=%08X%08X "
          "ps=%08X%08X tex0=%08X rt=%08X prim=%-2u aabb=%s(%.1f,%.1f)-(%.1f,%.1f)",
          (unsigned long long)r.draws, (unsigned long long)r.verts,
          (unsigned long long)r.noSnap,
          (uint32_t)(std::get<0>(k) >> 32), (uint32_t)std::get<0>(k),
          (uint32_t)(std::get<1>(k) >> 32), (uint32_t)std::get<1>(k),
          std::get<2>(k), std::get<3>(k), std::get<4>(k),
          noAabb ? "NONE " : "", noAabb ? 0.f : r.minX,
          noAabb ? 0.f : r.minY, noAabb ? 0.f : r.maxX,
          noAabb ? 0.f : r.maxY);
    }
  }
  if (aKeyQ) {
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[AUTOPSY_KEY] key=%08X bound on draws in %zu (ps,slot,rt) rows:",
        aKeyQ, aKeyRows.size());
    for (const auto& [k, v] : aKeyRows)
      SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[AUTOPSY_KEY] ps=%016llX slot=%-2u rt=%08X draws=%u noVerts=%u",
          (unsigned long long)std::get<0>(k), std::get<1>(k), std::get<2>(k),
          v.draws, v.noV);
  }

  SR_LOG_BOOT(sr::LogSeverity::Warn,
      "[HLE_REPLAY] executed %llu records / %llu payload bytes / %u presents | "
      "hashMismatches=%llu | texUnlock=%llu draws=%llu setTex=%llu",
      (unsigned long long)st.records, (unsigned long long)st.payloadBytes,
      st.presents, (unsigned long long)st.hashMismatches,
      (unsigned long long)perType[(size_t)RenderCommandType::TextureUnlock],
      (unsigned long long)(perType[(size_t)RenderCommandType::DrawVertices] +
                           perType[(size_t)RenderCommandType::DrawIndexedVertices]),
      (unsigned long long)perType[(size_t)RenderCommandType::SetTexture]);
  // Astra 085: how every ClearF colour was handled - a trace recorded before
  // the recorder carried it reports missing=N, and its clears must not be read
  // as the guest's colours.
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[HLE_REPLAY_CLEARF] records=%llu = restaged %llu + MISSING %llu + noColour %llu "
      "+ malformed %llu + allocFail %llu (this call)%s",
      (unsigned long long)st.clearFRecords,
      (unsigned long long)st.clearFRestaged,
      (unsigned long long)st.clearFMissing,
      (unsigned long long)st.clearFNoColour,
      (unsigned long long)st.clearFMalformed,
      (unsigned long long)st.clearFAllocFail,
      (st.clearFRestaged + st.clearFMissing + st.clearFNoColour +
       st.clearFMalformed + st.clearFAllocFail == st.clearFRecords)
          ? "" : "  <== ⛔ DOES NOT CONSERVE");
  {
    // iter-456n (eighth adjudication): the replay's own ownership state, so
    // "identical first accept/refuse verdicts live vs replay" is checkable
    // rather than asserted. A replay that restored the preamble's claims ends
    // with those keys owned at the recorded generations; one that ignored them
    // would report keys=0.
    auto& own = unlockOwnership();
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S6_RESOLVE_V2] gate=%d claims=%llu legacyAdopted=%llu "
        "successNoEpoch=%llu capturesRefusedByResolve=%llu resolveClaims=%llu",
        resolveOwnershipV2Enabled() ? 1 : 0,
        (unsigned long long)s_rv2Claims,
        (unsigned long long)s_rv2LegacyAdopted,
        (unsigned long long)s_rv2SuccessNoEpoch,
        (unsigned long long)s_rv2RefusedNoClaim,
        (unsigned long long)unlockOwnership().resolveClaims());
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[HLE_REPLAY_OWN] keys=%llu claims: new=%llu sameEpoch=%llu "
        "replaced=%llu | refusals=%llu | events=%llu (create=%llu "
        "destroy=%llu) | unattributedFirstClaims=%llu | verdicts=%llu "
        "digest=%016llX (noOwner=%llu recreated=%llu staleGen=%llu "
        "refuse=%llu)",
        (unsigned long long)own.trackedKeys(),
        (unsigned long long)own.claimsNew(),
        (unsigned long long)own.claimsSame(),
        (unsigned long long)own.claimsReplaced(),
        (unsigned long long)own.refusals(),
        (unsigned long long)own.objectEvents(),
        (unsigned long long)own.createEvents(),
        (unsigned long long)own.destroyEvents(),
        (unsigned long long)own.unattributedFirstClaims(),
        (unsigned long long)own.verdictCount(),
        (unsigned long long)own.verdictDigest(),
        (unsigned long long)own.verdictNoOwner(),
        (unsigned long long)own.verdictRecreated(),
        (unsigned long long)own.verdictStaleGen(),
        (unsigned long long)own.verdictRefuse());
    SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[S6_DISPOSITION_SUM] n=%llu digest=%016llX | applied=%llu "
        "refusedLegacySet=%llu refusedResolveEpoch=%llu "
        "refusedUnlockEpoch=%llu refusedOther=%llu uploadFailed=%llu",
        (unsigned long long)dispositionCount(),
        (unsigned long long)dispositionDigest(),
        (unsigned long long)dispositionOfKind(0),
        (unsigned long long)dispositionOfKind(1),
        (unsigned long long)dispositionOfKind(2),
        (unsigned long long)dispositionOfKind(3),
        (unsigned long long)dispositionOfKind(4),
        (unsigned long long)dispositionOfKind(5));
  }
  // iter-445 C1: THE FINAL LEDGER PAIR, emitted HERE and not from shutdown() —
  // D3D12Backend::shutdown() has ZERO call sites anywhere in the repo and there
  // is no destructor, so a teardown-time report is unreachable code. This path
  // runs exactly once and covers the whole trace.
  dropCensus();
  if (auto beF = s_backend.acquire()) {
    beF->frameListReport(st.presents);
    beF->hleSurfaceReport(st.presents);
    // r5: THE ONE replay-final cube summary. hleSurfaceReport no longer
    // emits it and the periodic site is suppressed while s_replayActive.
    beF->hleCubeSumReport("replay-final");
    // iter-449s AUTOPSY: per-surface pixel truth at end of replay.
    if (const char* sd = std::getenv("SR_HLE_SURF_DUMP"); sd && *sd)
      beF->hleDumpSurfaces(sd);
  }
  SR_LOG_BOOT(sr::LogSeverity::Info,
      "[S6_RTBIND] setRT=%llu bound=%llu | setDS=%llu bound=%llu | setVP=%llu "
      "latched=%dx%d valid=%d | setSC=%llu latched=(%d,%d)-(%d,%d) valid=%d | clearF=%llu subRectWidened=%llu "
      "colourMissing=%llu colourMalformed=%llu targetsMasked=%llu",
      (unsigned long long)s_setRtSeen, (unsigned long long)s_setRtBound,
      (unsigned long long)s_setDsSeen, (unsigned long long)s_setDsBound,
      (unsigned long long)s_setVpSeen, (int)s_viewport.w, (int)s_viewport.h,
      s_viewport.valid ? 1 : 0, (unsigned long long)s_setScSeen, s_scissor.l,
      s_scissor.t, s_scissor.r, s_scissor.b, s_scissor.valid ? 1 : 0,
      (unsigned long long)s_clearFSeen,
      (unsigned long long)s_clearRectApprox,
      (unsigned long long)s_clearFColourMissing,
      (unsigned long long)s_clearFColourMalformed,
      (unsigned long long)s_clearFTargetsMasked);
      SR_LOG_BOOT(sr::LogSeverity::Info,
          "[S6_RESOLVE] n=%llu | distinct src=%zu dest=%zu | srcPooled=%llu destKnown=%llu destEverBound=%zu/%zu boundKeys=%zu | EXEC copies=%llu owned=%zu sampled=%llu skip(rect=%llu sliced=%llu other=%llu) | depthSrc=%llu clearRT=%llu clearZ=%llu",
          (unsigned long long)s_resolveSeen, s_resolveSrcs.size(),
          s_resolveDests.size(), (unsigned long long)s_resolveSrcPooled,
          (unsigned long long)s_resolveDestKnown, resolveDestEverBound(),
s_resolveDests.size(), s_boundTexKeys.size(),
          (unsigned long long)s_resolveExec, s_resolveOwned.size(),
(unsigned long long)s_resolveBindHit,
(unsigned long long)s_resolveSkipRect,
(unsigned long long)s_resolveSkipSliced,
(unsigned long long)s_resolveSkipOther,
(unsigned long long)s_resolveDepth,
          (unsigned long long)s_resolveClearRT,
          (unsigned long long)s_resolveClearZ);
  if (out) *out = st;
  return st.hashMismatches ? 2 : 0;
}

ArenaStats arenaStats() {
  Arena& a = arena();
  std::lock_guard<std::mutex> lk(a.m);
  ArenaStats s;
  s.writes = a.writes;
  s.bytes = a.bytes;
  s.rejects = a.rejects;
  s.inFlight = a.inFlight;
  s.peak = a.peak;
  return s;
}

ArenaStats blockStats() {
  Blocks& b = blocks();
  std::lock_guard<std::mutex> lk(b.m);
  ArenaStats s;
  s.writes = b.writes;
  s.bytes = b.bytes;
  s.rejects = b.rejects;
  s.inFlight = b.inFlight;
  s.peak = b.peak;
  return s;
}

void setBackend(sr::gfx::D3D12Backend* backend) {
  s_backend.publish(backend);
  SR_LOG_BOOT(sr::LogSeverity::Info,
              "[HLE_VIDEO] device published to the render thread (%s) — the "
              "main loop is now an event pump",
              backend ? "ready" : "null");
}

void retireBackend(sr::gfx::D3D12Backend* expected) { s_backend.retire(expected); }

bool waitForPresent(uint64_t target, uint32_t timeoutMs) {
  if (s_present.load(std::memory_order_acquire) >= target) return true;
  std::unique_lock<std::mutex> lk(s_presentMu);
  return s_presentCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
    return s_present.load(std::memory_order_acquire) >= target;
  });
}

uint64_t enqueuedTotal() { return s_enqueued.load(std::memory_order_relaxed); }
uint64_t drainedTotal() { return s_drained.load(std::memory_order_relaxed); }
uint64_t presentCount() { return s_present.load(std::memory_order_relaxed); }
uint64_t presentsSubmitted() { return s_submitted.load(std::memory_order_relaxed); }
uint32_t queueDepth() { return s_depth.load(std::memory_order_relaxed); }
uint64_t typeCount(RenderCommandType t) {
  const uint32_t i = static_cast<uint32_t>(t);
  return i < static_cast<uint32_t>(RenderCommandType::Count)
             ? s_perType[i].load(std::memory_order_relaxed)
             : 0;
}

}  // namespace hle
}  // namespace gfx
}  // namespace sr
