// iter-463 collision watch: OBSERVE-ONLY evidence for the user-reported "some buildings didn't
// have collision" (build 60a90bf, Midtown, flying; never reproduced on HEAD). Built from
// archive/v1_invest/collision/prep/collision_route_watch.cpp plus the verifier's corrections
// (mover_syn "verify": SetTransform hook, world add/remove hooks, per-tick fast-flight path line,
// fault-free reads by construction, g_lastOut refreshed on inactive ticks).
//
// WHY IT EXISTS (the decision tree it feeds, tools/collision_route_analyze.py): a pass-through tick
// = Superman's capsule centre deep inside a building AABB with that body NOT in his contact
// manifold. The log then says which of these it was:
//   STREAMING   the body was added to the hkWorld after he got there / removed before;
//   WRITER      a direct position write (TELEPORT / SETPOS / SETXFORM, caller lr) or a
//               physrep change (SET_FILTERINFO layer 11, SET_PHYSREP_MODE) around the tick;
//   BROADPHASE  body in the world but not in his phantom's overlap list;
//   MOVER       body in the overlap list, integrate path taken, still no contact;
//   H-D         only after all of the above: the original data (check vs Xenia).
//
// Every hook is a strong def of a weak recomp alias that delegates to __imp__ and WRITES NOTHING
// guest-side (docs/OVERRIDES.json class "observe"):
//   sub_821C0C58 MoveTowardIdealPos(PRXY r3, entity r4, dt f1)     - per-tick mover evidence
//   sub_821AA218 actor vt+12 SetPosition(actor r3, vec4* r4)        - SETPOS (non-mover callers)
//   sub_821E3CF8 actor vt+20 SetTransform(actor r3, 2 x vec4* r4)   - SETXFORM (pos = r4+16;
//                writes actor+0x10/+0x20, never the proxy: a silent teleport)
//   sub_821AE1C8 PRXY vt+108 SetPhysRepMode(r3, mode r4)            - SET_PHYSREP_MODE
//   sub_821AE388 PRXY vt+168 Deactivate(r3)                         - DEACTIVATE
//   sub_821AE218 PRXY vt+136 SetFilterInfo(r3, layer r4, group r5)  - SET_FILTERINFO
//   sub_82596508 hkWorld::addEntity(world r3, entity r4, act r5)    - [COLL_WORLD] ADD
//                (queued as op 1 when world+0x8C lock != 0; re-run from the op queue at 0x825AD068)
//   sub_825967C0 hkWorld::removeEntity(result* r3, world r4, entity r5) - [COLL_WORLD] REMOVE
//                (every remover goes through it: PhysWorld_RemoveEntityIfInWorld_sub_822BE6B8,
//                the broadphase-border cull sub_822C6870, ragdolls, the op queue)
//
// GATE: SR_COLLISION_WATCH (default ON, `=0` disables; see "DEFAULT" below).
//   SR_COLLISION_MOVER_WATCH=1 (the prep's name) is a legacy alias: enables the watch AND the
//   fixed-route target lines, so the collision_route kit's commands still work.
//   Route target lines ([COLL_ROUTE], tools/collision_route_verdict.py) are OPT-IN:
//   SR_COLL_ROUTE=1 or SR_COLL_ROUTE_TARGET=x,y,z (+ SR_COLL_ROUTE_AABB, SR_COLL_ROUTE_RADIUS).
//   SR_COLL_PATH_PER_SEC=<n> raises the [COLL_PATH] per-second budget (default 40 > the 30 Hz
//   tick; only offline harness replays, which tick far faster, need it).
//
// LOG TAGS (every emitter is capped - counting lines measures the cap; the 1 s summary carries
// the uncapped counters; tools/audit_log_gating.py --tag COLL_PATH etc.):
//   [COLL_WATCH]  enable / refusal banner
//   [COLL_MOVER]  1 s summary (only while Superman ticks), TELEPORT, NO-INTEGRATE, SETPOS,
//                 SETXFORM, SET_PHYSREP_MODE, DEACTIVATE, SET_FILTERINFO, superman actor
//   [COLL_PATH]   one line per mover tick while speed > 20 m/s (plus the first slow tick after)
//   [COLL_BODY]   first sighting of a body in his manifold / overlap list: addr, translation,
//                 layer, shape vt (the analyzer maps addr -> snapshot AABB by translation)
//   [COLL_WORLD]  ADD / REMOVE of every layer-7 body, and of any body within 300 m of Superman
//   [COLL_ROUTE]  fixed-route target lines (opt-in, format of the prep patch)
//
// FAULT-FREE BY CONSTRUCTION (the harness VEH makes any host AV fatal): every guest read goes
// through Mem::u32/u8, which refuses (returns 0) unless [va, va+n) lies inside
// [0x10000, 0xA0000000). Below 0xA0000000 aliasGuestVA is the identity, and the enable check
// requires the FLAT guest model (allocGuestMemory: the whole reservation MEM_COMMIT|READWRITE for
// the harness's lifetime - guest/guest_harness.cpp) and verifies it ONCE with a VirtualQuery walk
// over [base, base+0xA0000000): committed, readable, no guard page. Under SR_PHYS_UNIFY (views and
// placeholders) the watch refuses to enable. Nothing in the flat model decommits or guards guest
// pages afterwards (image_write_watchdog only drops pages to PAGE_READONLY, still readable); the
// only release is freeGuestMemory at teardown, when the guest code itself can no longer run.
//
// DEFAULT: measured overhead (commit message + docs/TOOLKIT.md): per Superman tick a few hundred
// ns of host time (self-timed, reported as selfUs in the 1 s summary); per non-Superman mover
// call a gate check plus <= 6 guest loads. The user plays the game themselves and their own play
// is the best reproducer, so the evidence must already be in the log when they hit it.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"
#include "collision_watch.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#endif

extern "C" void __imp__sub_821C0C58(PPCContext& __restrict ctx, uint8_t* base);  // MoveTowardIdealPos
extern "C" void __imp__sub_821AA218(PPCContext& __restrict ctx, uint8_t* base);  // actor vt+12 SetPosition
extern "C" void __imp__sub_821E3CF8(PPCContext& __restrict ctx, uint8_t* base);  // actor vt+20 SetTransform
extern "C" void __imp__sub_821AE1C8(PPCContext& __restrict ctx, uint8_t* base);  // PRXY vt+108 SetPhysRepMode
extern "C" void __imp__sub_821AE388(PPCContext& __restrict ctx, uint8_t* base);  // PRXY vt+168 Deactivate
extern "C" void __imp__sub_821AE218(PPCContext& __restrict ctx, uint8_t* base);  // PRXY vt+136 SetFilterInfo
extern "C" void __imp__sub_82596508(PPCContext& __restrict ctx, uint8_t* base);  // hkWorld::addEntity
extern "C" void __imp__sub_825967C0(PPCContext& __restrict ctx, uint8_t* base);  // hkWorld::removeEntity

namespace sr::kernel::collwatch {

// The one-time precondition: flat model, and [base, base+kTop) committed + readable, no guard.
bool flatModelReadable(uint8_t* base, char* why, size_t whyN) {
  if (g_physUnifyActive) { std::snprintf(why, whyN, "SR_PHYS_UNIFY model (views/placeholders)"); return false; }
  if (!base) { std::snprintf(why, whyN, "no guest base"); return false; }
#if defined(_WIN32)
  const uintptr_t s = reinterpret_cast<uintptr_t>(base), e = s + kTop;
  uintptr_t cur = s;
  int regions = 0;
  while (cur < e) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(cur), &mbi, sizeof mbi)) {
      std::snprintf(why, whyN, "VirtualQuery failed at +0x%llX", (unsigned long long)(cur - s));
      return false;
    }
    const DWORD p = mbi.Protect & 0xFF;
    const bool readable = p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_EXECUTE_READ ||
                          p == PAGE_EXECUTE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !readable) {
      std::snprintf(why, whyN, "guest +0x%llX state=0x%lX protect=0x%lX not committed-readable",
                    (unsigned long long)(cur - s), (unsigned long)mbi.State, (unsigned long)mbi.Protect);
      return false;
    }
    const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (next <= cur) { std::snprintf(why, whyN, "region walk made no progress"); return false; }
    cur = next;
    ++regions;
  }
  std::snprintf(why, whyN, "flat model, %d committed readable region(s) over [base, base+0x%X)", regions, kTop);
  return true;
#else
  std::snprintf(why, whyN, "non-Windows host");
  return false;
#endif
}

// ── gate ──────────────────────────────────────────────────────────────────────────────────────
struct Cfg {
  bool requested = false, route = false;
  float target[3] = {-2281.447f, 1176.876f, -365.4114f};                      // Havok_Box01
  float lo[3] = {-2301.17f, 1171.95f, -375.39f}, hi[3] = {-2262.25f, 1374.14f, -354.79f};
  float radius = 40.f;
  uint32_t pathPerSec = 40;   // SR_COLL_PATH_PER_SEC: [COLL_PATH] budget (the game ticks at 30 Hz;
                              // offline harness replays run far faster and may raise it)
};
const Cfg& cfg() {
  static const Cfg c = [] {
    Cfg x;
    const char* e = std::getenv("SR_COLLISION_WATCH");
    const char* legacy = std::getenv("SR_COLLISION_MOVER_WATCH");
    const bool legacyOn = legacy && legacy[0] == '1';
    x.requested = legacyOn || !e || e[0] != '0';   // default ON; =0 disables
    if (e && e[0] == '0' && !legacyOn) x.requested = false;
    const char* r = std::getenv("SR_COLL_ROUTE");
    const char* t = std::getenv("SR_COLL_ROUTE_TARGET");
    x.route = legacyOn || (r && r[0] == '1') || t != nullptr;
    if (t) std::sscanf(t, "%f,%f,%f", &x.target[0], &x.target[1], &x.target[2]);
    if (const char* a = std::getenv("SR_COLL_ROUTE_AABB"))
      std::sscanf(a, "%f,%f,%f,%f,%f,%f", &x.lo[0], &x.lo[1], &x.lo[2], &x.hi[0], &x.hi[1], &x.hi[2]);
    if (const char* a = std::getenv("SR_COLL_ROUTE_RADIUS")) x.radius = (float)std::atof(a);
    if (const char* a = std::getenv("SR_COLL_PATH_PER_SEC")) x.pathPerSec = (uint32_t)std::strtoul(a, nullptr, 10);
    return x;
  }();
  return c;
}

std::atomic<int> g_state{0};   // 0 undecided, 1 on, -1 off
std::mutex g_initMu;

bool enabledSlow(uint8_t* base) {
  std::lock_guard<std::mutex> lk(g_initMu);
  int s = g_state.load(std::memory_order_acquire);
  if (s) return s > 0;
  if (!cfg().requested) {
    g_state.store(-1, std::memory_order_release);
    return false;
  }
  char why[160];
  const bool ok = flatModelReadable(base, why, sizeof why);
  static std::atomic<uint32_t> s_banner{0};   // once per process (g_state latches); cap for the audit
  if (s_banner.fetch_add(1) < 4u)
    SR_LOG_BOOT(ok ? sr::LogSeverity::Info : sr::LogSeverity::Warn,
                "[COLL_WATCH] %s (SR_COLLISION_WATCH, default ON, =0 disables; route=%d): %s",
                ok ? "enabled" : "REFUSED", cfg().route ? 1 : 0, why);
  g_state.store(ok ? 1 : -1, std::memory_order_release);
  return ok;
}
inline bool enabled(uint8_t* base) {
  const int s = g_state.load(std::memory_order_relaxed);
  if (__builtin_expect(s != 0, 1)) return s > 0;
  return enabledSlow(base);
}

// ── shared state ──────────────────────────────────────────────────────────────────────────────
constexpr uint32_t kSupVt = 0x82009798u;
std::atomic<uint32_t> g_supActor{0}, g_supPrxy{0};
std::atomic<uint32_t> g_tick{0};
std::atomic<float> g_supPos[3] = {NAN, NAN, NAN};
std::atomic<float> g_supSpd{0.f};
const auto g_t0 = std::chrono::steady_clock::now();

inline uint32_t msNow() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - g_t0).count();
}

// Per-second line budget (lines beyond it are counted as dropped in the summary).
struct Budget {
  std::atomic<uint32_t> sec{0xFFFFFFFFu}, n{0};
};
std::atomic<uint32_t> g_dropped{0};
bool budget(Budget& b, uint32_t perSec) {
  const uint32_t s = msNow() / 1000u;
  if (b.sec.load(std::memory_order_relaxed) != s) {
    b.sec.store(s, std::memory_order_relaxed);
    b.n.store(0, std::memory_order_relaxed);
  }
  if (b.n.fetch_add(1, std::memory_order_relaxed) < perSec) return true;
  g_dropped.fetch_add(1, std::memory_order_relaxed);
  return false;
}
Budget g_bEvent, g_bPath, g_bBody, g_bWorld, g_bSup, g_bSum;
bool budgetForTest(uint32_t perSec, int calls) {
  Budget b;
  const uint32_t d0 = g_dropped.load();
  int passed = 0;
  for (int i = 0; i < calls; ++i) passed += budget(b, perSec) ? 1 : 0;
  // (a second boundary during the loop would reset the window; the test uses a tiny loop)
  return passed == (int)perSec && g_dropped.load() - d0 == (uint32_t)(calls - (int)perSec);
}
std::atomic<uint32_t> g_nEvent{0}, g_nSum{0}, g_nPath{0}, g_nBody{0}, g_nWorld{0}, g_nSup{0}, g_nRoute{0},
    g_nInside{0};

// Uncapped counters (reported in the 1 s summary, reset there).
std::atomic<uint32_t> g_adds{0}, g_addsQueued{0}, g_removes{0}, g_removesQueued{0};
std::atomic<uint64_t> g_selfNs{0};

// Bodies already described by a [COLL_BODY] line, keyed by (addr, translation) so an address
// reused by another body is described again. Lock-free open addressing, insert-only.
constexpr uint32_t kSeenSlots = 8192;
std::atomic<uint64_t> g_seen[kSeenSlots];
bool firstSighting(uint32_t body, const float p[3]) {
  uint32_t bits[3];
  std::memcpy(bits, p, sizeof bits);
  uint64_t key = ((uint64_t)body << 32) | (bits[0] ^ (bits[1] * 0x9E3779B1u) ^ (bits[2] * 0x85EBCA6Bu));
  if (!key) key = 1;
  uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 51) & (kSeenSlots - 1);
  for (int i = 0; i < 32; ++i) {
    std::atomic<uint64_t>& s = g_seen[(h + i) & (kSeenSlots - 1)];
    uint64_t cur = s.load(std::memory_order_relaxed);
    if (cur == key) return false;
    if (cur == 0) {
      if (s.compare_exchange_strong(cur, key)) return true;
      if (cur == key) return false;
    }
  }
  return true;   // probe window full: describe it again (lines stay capped)
}

// Mover-thread-only state (Superman's MoveTowardIdealPos runs on one thread).
struct Sec {
  uint32_t calls = 0, inactive = 0, skipped = 0, keyframed = 0, teleports = 0, integrates = 0;
  uint32_t fast = 0, nearTicks = 0, targetContactTicks = 0, insideTicks = 0;
  float maxTeleport = 0.f, maxStep = 0.f, maxSpd = 0.f, maxDepth = -1e9f;
};
Sec g_sec;
float g_lastOut[3] = {NAN, NAN, NAN};
bool g_lastFast = false;
uint32_t g_targetBody = 0;
std::chrono::steady_clock::time_point g_secStart = std::chrono::steady_clock::now();

// ── guest structure readers (contracts: collision map iter-463, ENGINE_MAP "Superman mover") ──
struct BodyInfo {
  float pos[3];
  uint32_t layer, shapeVt;
};
BodyInfo bodyInfo(const Mem& m, uint32_t body) {
  BodyInfo b;
  m.vec3(m.u32(body + 0x1C) + 0x30, b.pos);   // collidable.m_motion -> hkTransform translation
  b.layer = m.u32(body + 0x30) & 0x1F;         // collidable filterInfo
  const uint32_t shape = m.u32(body + 0x14);
  b.shapeVt = shape ? m.u32(shape) : 0;
  return b;
}

bool isSuperman(const Mem& m, uint32_t ent) {
  const uint32_t cur = g_supActor.load(std::memory_order_relaxed);
  if (m.u32(ent) != kSupVt) {
    if (ent == cur && cur) g_supActor.store(0, std::memory_order_relaxed);   // freed / reused
    return false;
  }
  if (ent == cur) return true;
  const uint32_t s = m.u32(m.u32(ent + 0x84) + 0x24);
  static const char kName[] = "SUPERMAN";
  if (!inWindow(s, 8)) return false;
  for (int i = 0; i < 8; ++i)
    if (m.u8(s + i) != (uint8_t)kName[i]) return false;
  g_supActor.store(ent, std::memory_order_relaxed);
  g_supPrxy.store(m.u32(ent + 0x70), std::memory_order_relaxed);
  g_lastOut[0] = g_lastOut[1] = g_lastOut[2] = NAN;
  if (budget(g_bSup, 4) && g_nSup.fetch_add(1) < 64u)
    SR_LOG_BOOT(sr::LogSeverity::Info, "[COLL_MOVER] superman actor=0x%08X prxy=0x%08X t=%u", ent,
                g_supPrxy.load(), g_tick.load());
  return true;
}

inline uint32_t proxyOf(const Mem& m, uint32_t prxy) { return m.u32(prxy + 0x04); }
inline uint32_t phantomOf(const Mem& m, uint32_t proxy) { return m.u32(proxy + 0x30); }
inline uint32_t filterInfo(const Mem& m, uint32_t prxy) { return m.u32(phantomOf(m, proxyOf(m, prxy)) + 0x30); }

const char* locoName(uint32_t vt) {
  switch (vt) {
    case 0x8200EDF4u: return "LOCO";
    case 0x8200ED88u: return "FLY";
    case 0x8200ED28u: return "ANIM";
    case 0x8200EDBCu: return "GRND";
    case 0x8200ED58u: return "CPTR";
    case 0x8200F030u: return "WHLD";
    case 0x82008D50u: return "NONE";
    default: return "?";
  }
}
struct Loco {
  const char* name;
  uint32_t def, sub;
};
Loco locoOf(const Mem& m, uint32_t actor) {
  const uint32_t comp = m.u32(m.u32(actor + 0x60) + 4);
  return {locoName(m.u32(comp)), m.u32(comp + 0x60), m.u32(comp + 0x90)};
}

inline float dist3(const float a[3], const float b[3]) {
  return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

void describeBody(const Mem& m, uint32_t body, const char* src) {
  if (!body) return;
  const BodyInfo b = bodyInfo(m, body);
  if (!firstSighting(body, b.pos)) return;
  if (budget(g_bBody, 64) && g_nBody.fetch_add(1) < 200000u)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[COLL_BODY] body=0x%08X src=%s pos=(%.3f %.3f %.3f) L=%u shapevt=0x%08X t=%u", body, src,
                b.pos[0], b.pos[1], b.pos[2], b.layer, b.shapeVt, g_tick.load(std::memory_order_relaxed));
}

// "[n]0xA,0xB,..." for a body list (first `show`), describing each body once.
void listBodies(const Mem& m, uint32_t arr, uint32_t n, uint32_t stride, uint32_t off, uint32_t show,
                const char* src, char* out, size_t outN) {
  size_t w = (size_t)std::snprintf(out, outN, "%u[", n);
  for (uint32_t i = 0; i < n && i < show; ++i) {
    const uint32_t coll = m.u32(arr + stride * i + off);
    const uint32_t body = coll ? coll - 0x14 : 0;
    describeBody(m, body, src);
    if (w < outN) w += (size_t)std::snprintf(out + w, outN - w, i ? ",%08X" : "%08X", body);
  }
  if (w < outN) std::snprintf(out + w, outN - w, n > show ? ",+%u]" : "]", n - show);
}

// ── route target (opt-in, prep format) ────────────────────────────────────────────────────────
bool isTargetBody(const Mem& m, uint32_t body) {
  if (!body) return false;
  if (body == g_targetBody) return true;
  float p[3];
  m.vec3(m.u32(body + 0x1C) + 0x30, p);
  for (int i = 0; i < 3; ++i)
    if (!(std::fabs(p[i] - cfg().target[i]) <= 0.5f)) return false;
  if (!g_targetBody) {
    g_targetBody = body;
    if (g_nRoute.fetch_add(1) < 4000u)
      SR_LOG_BOOT(sr::LogSeverity::Info, "[COLL_ROUTE] target body=0x%08X (translation match)", body);
  }
  return true;
}
float depthInAabb(const float p[3], const float lo[3], const float hi[3]) {   // >0 inside
  float d = 1e9f;
  for (int i = 0; i < 3; ++i) d = std::fmin(d, std::fmin(p[i] - lo[i], hi[i] - p[i]));
  return d;
}
float distToAabb(const float p[3], const float lo[3], const float hi[3]) {
  float s = 0.f;
  for (int i = 0; i < 3; ++i) {
    const float e = std::fmax(std::fmax(lo[i] - p[i], 0.f), p[i] - hi[i]);
    s += e * e;
  }
  return std::sqrt(s);
}

void routeTick(const Mem& m, uint32_t prxy, uint32_t ent, const float in[3], const float out[3],
               const char* path, uint8_t b0E, uint8_t b0F) {
  const Cfg& c = cfg();
  if (distToAabb(out, c.lo, c.hi) > c.radius && distToAabb(in, c.lo, c.hi) > c.radius) return;
  g_sec.nearTicks++;
  const uint32_t proxy = proxyOf(m, prxy), ph = phantomOf(m, proxy);
  uint32_t manN = m.u32(proxy + 0x78), manT = 0, ovN = m.u32(ph + 0x124), ovT = 0;
  const uint32_t marr = m.u32(proxy + 0x74), oarr = m.u32(ph + 0x120);
  if (manN > 64) manN = 0;
  if (ovN > 4096) ovN = 0;
  for (uint32_t i = 0; i < manN; ++i)
    if (isTargetBody(m, m.u32(marr + 48 * i + 40) - 0x14)) manT++;
  for (uint32_t i = 0; i < ovN; ++i)
    if (isTargetBody(m, m.u32(oarr + 4 * i) - 0x14)) ovT = 1;
  float v[3];
  m.vec3(proxy + 0x10, v);
  const float depth = depthInAabb(out, c.lo, c.hi);
  if (depth > g_sec.maxDepth) g_sec.maxDepth = depth;
  if (manT) g_sec.targetContactTicks++;
  if (depth > 1.5f) g_sec.insideTicks++;
  const Loco l = locoOf(m, ent);
  if (g_nRoute.fetch_add(1) < 4000u)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[COLL_ROUTE] t=%u loco=%s def=%u sub=%u b0E=%02X b0F=%02X path=%s in=(%.3f %.3f %.3f) "
                "out=(%.3f %.3f %.3f) v=(%.2f %.2f %.2f) man=%u manT=%u ovT=%u ovN=%u depth=%.3f ent=0x%08X",
                g_tick.load(), l.name, l.def, l.sub, b0E, b0F, path, in[0], in[1], in[2], out[0], out[1],
                out[2], v[0], v[1], v[2], manN, manT, ovT, ovN, depth, ent);
  if (depth > 1.5f && manT == 0 && g_nInside.fetch_add(1) < 200u)
    SR_LOG_BOOT(sr::LogSeverity::Warn,
                "[COLL_ROUTE] INSIDE t=%u depth=%.3f path=%s loco=%s ovT=%u pos=(%.3f %.3f %.3f)",
                g_tick.load(), depth, path, l.name, ovT, out[0], out[1], out[2]);
}

// ── the fast-flight path line ─────────────────────────────────────────────────────────────────
void pathTick(const Mem& m, uint32_t prxy, uint32_t ent, const float in[3], const float out[3],
              const float v[3], float spd, const char* path) {
  const uint32_t proxy = proxyOf(m, prxy), ph = phantomOf(m, proxy);
  uint32_t manN = m.u32(proxy + 0x78), ovN = m.u32(ph + 0x124);
  if (manN > 64) manN = 0;
  if (ovN > 4096) ovN = 0;
  char man[160], ov[200];
  listBodies(m, m.u32(proxy + 0x74), manN, 48, 40, 8, "man", man, sizeof man);
  listBodies(m, m.u32(ph + 0x120), ovN, 4, 0, 12, "ov", ov, sizeof ov);
  const Loco l = locoOf(m, ent);
  if (budget(g_bPath, cfg().pathPerSec) && g_nPath.fetch_add(1) < 2000000u)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[COLL_PATH] t=%u ms=%u loco=%s def=%u sub=%u path=%s L=%u in=(%.3f %.3f %.3f) "
                "out=(%.3f %.3f %.3f) v=(%.2f %.2f %.2f) spd=%.1f man=%s ov=%s",
                g_tick.load(), msNow(), l.name, l.def, l.sub, path, filterInfo(m, prxy) & 0x1F, in[0], in[1],
                in[2], out[0], out[1], out[2], v[0], v[1], v[2], spd, man, ov);
}

void maybeSummary(const Mem& m) {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_secStart < std::chrono::seconds(1)) return;
  g_secStart = now;
  const uint32_t p = g_supPrxy.load(), a = g_supActor.load();
  const Loco l = locoOf(m, a);
  if (budget(g_bSum, 4) && g_nSum.fetch_add(1) < 1000000u)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[COLL_MOVER] 1s t=%u calls=%u integrate=%u inactive=%u skip=%u keyframed=%u teleports=%u "
                "maxTeleport=%.2f maxStep=%.2f maxSpd=%.1f fast=%u b0E=%02X b0F=%02X sup=%u red=%u fi=%08X "
                "loco=%s movedef=%u sub=%u adds=%u(q%u) removes=%u(q%u) dropped=%u selfUs=%.1f "
                "near=%u targetContact=%u inside=%u maxDepth=%.2f",
                g_tick.load(), g_sec.calls, g_sec.integrates, g_sec.inactive, g_sec.skipped, g_sec.keyframed,
                g_sec.teleports, g_sec.maxTeleport, g_sec.maxStep, g_sec.maxSpd, g_sec.fast, m.u8(p + 0x0E),
                m.u8(p + 0x0F), m.u8(p + 0x7C), m.u32(p + 0x84), filterInfo(m, p), l.name, l.def, l.sub,
                g_adds.exchange(0), g_addsQueued.exchange(0), g_removes.exchange(0), g_removesQueued.exchange(0),
                g_dropped.exchange(0), g_selfNs.exchange(0) / 1000.0, g_sec.nearTicks, g_sec.targetContactTicks,
                g_sec.insideTicks, g_sec.nearTicks ? g_sec.maxDepth : -1.f);
  g_sec = Sec{};
}

inline bool fromMover(uint32_t lr) { return lr >= 0x821C0C58u && lr < 0x821C1718u; }

// World add/remove line: every layer-7 body, and any body within 300 m of Superman.
void worldEvent(const Mem& m, const char* what, const char* st, uint32_t world, uint32_t lock, uint32_t body,
                const BodyInfo& b, uint32_t lr) {
  float sup[3] = {g_supPos[0].load(std::memory_order_relaxed), g_supPos[1].load(std::memory_order_relaxed),
                  g_supPos[2].load(std::memory_order_relaxed)};
  const bool supKnown = g_supActor.load(std::memory_order_relaxed) != 0 && !std::isnan(sup[0]);
  const float d = supKnown ? dist3(sup, b.pos) : NAN;
  if (b.layer != 7 && !(supKnown && d <= 300.f)) return;
  if (budget(g_bWorld, 2000) && g_nWorld.fetch_add(1) < 400000u)
    SR_LOG_BOOT(sr::LogSeverity::Info,
                "[COLL_WORLD] %s st=%s body=0x%08X world=0x%08X lock=%u L=%u shapevt=0x%08X pos=(%.3f %.3f %.3f) "
                "sup=(%.2f %.2f %.2f) spd=%.1f dist=%.1f t=%u ms=%u lr=0x%08X",
                what, st, body, world, lock, b.layer, b.shapeVt, b.pos[0], b.pos[1], b.pos[2], sup[0], sup[1],
                sup[2], g_supSpd.load(std::memory_order_relaxed), d, g_tick.load(std::memory_order_relaxed),
                msNow(), lr);
  (void)m;
}

}  // namespace sr::kernel::collwatch

using namespace sr::kernel::collwatch;

// MoveTowardIdealPos: r3 = PRXY obj, r4 = entity, f1 = dt.
PPC_FUNC(sub_821C0C58) {
  const Mem m{base};
  const uint32_t prxy = ctx.r3.u32, ent = ctx.r4.u32;
  const bool sup = enabled(base) && isSuperman(m, ent);
  float in[3] = {0, 0, 0};
  uint8_t b0E = 0, b0F = 0;
  if (sup) {
    m.vec3(ent + 0x20, in);
    b0E = m.u8(prxy + 0x0E);
    b0F = m.u8(prxy + 0x0F);
  }
  const double dt = ctx.f1.f64;
  __imp__sub_821C0C58(ctx, base);   // the one, unconditional delegation
  if (!sup) return;
  const auto t0 = std::chrono::steady_clock::now();
  g_tick.fetch_add(1, std::memory_order_relaxed);
  g_sec.calls++;
  float out[3], v[3];
  m.vec3(ent + 0x20, out);
  m.vec3(proxyOf(m, prxy) + 0x10, v);
  const float step = dist3(in, out);
  float spd = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (dt > 1e-4 && step / (float)dt > spd) spd = step / (float)dt;
  if (!(spd == spd)) spd = 0.f;
  for (int i = 0; i < 3; ++i) g_supPos[i].store(out[i], std::memory_order_relaxed);
  g_supSpd.store(spd, std::memory_order_relaxed);
  const char* path;
  if (!(b0F & 4) || dt == 0.0) {
    g_sec.inactive++;
    path = "inactive";
  } else {
    const bool skipped = m.u32(prxy + 0x88) == 1;
    const bool keyed = (b0E >> 4) == 1;
    if (skipped) g_sec.skipped++;
    if (keyed) g_sec.keyframed++;
    if (!skipped && !keyed) g_sec.integrates++;
    path = skipped ? "off-camera-skip" : keyed ? "keyframed" : "integrate";
    if (!std::isnan(g_lastOut[0])) {
      const float d = dist3(in, g_lastOut);
      if (d > 0.01f) {
        g_sec.teleports++;
        if (d > g_sec.maxTeleport) g_sec.maxTeleport = d;
        if (budget(g_bEvent, 64) && g_nEvent.fetch_add(1) < 1000000u)
          SR_LOG_BOOT(sr::LogSeverity::Warn,
                      "[COLL_MOVER] TELEPORT %.3f m before mover: last-out (%.2f %.2f %.2f) -> in (%.2f %.2f %.2f) t=%u",
                      d, g_lastOut[0], g_lastOut[1], g_lastOut[2], in[0], in[1], in[2], g_tick.load());
      }
    }
    if (step > g_sec.maxStep) g_sec.maxStep = step;
    if ((skipped || keyed) && budget(g_bEvent, 64) && g_nEvent.fetch_add(1) < 1000000u)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[COLL_MOVER] NO-INTEGRATE %s step=%.3f pos (%.2f %.2f %.2f) t=%u",
                  path, step, out[0], out[1], out[2], g_tick.load());
  }
  if (spd > g_sec.maxSpd) g_sec.maxSpd = spd;
  const bool fast = spd > 20.f;
  if (fast) g_sec.fast++;
  if (fast || g_lastFast) pathTick(m, prxy, ent, in, out, v, spd, path);
  g_lastFast = fast;
  if (cfg().route) routeTick(m, prxy, ent, in, out, path, b0E, b0F);
  // Refreshed on EVERY tick, inactive ones included (verifier minor: a stale last-out gave one
  // false TELEPORT after reactivation).
  std::memcpy(g_lastOut, out, sizeof out);
  maybeSummary(m);
  g_selfNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - t0).count(),
                     std::memory_order_relaxed);
}

// Actor SetPosition (vt+12): r3 = actor, r4 = vec4*. The mover's own write-back (lr inside
// MoveTowardIdealPos) is not an event.
PPC_FUNC(sub_821AA218) {
  if (enabled(base) && ctx.r3.u32 == g_supActor.load() && ctx.r3.u32 && !fromMover((uint32_t)ctx.lr)) {
    const Mem m{base};
    float p[3];
    m.vec3(ctx.r4.u32, p);
    if (budget(g_bEvent, 64) && g_nEvent.fetch_add(1) < 1000000u)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[COLL_MOVER] SETPOS lr=0x%08X -> (%.2f %.2f %.2f) t=%u",
                  (uint32_t)ctx.lr, p[0], p[1], p[2], g_tick.load());
  }
  __imp__sub_821AA218(ctx, base);
}

// Actor SetTransform (vt+20): r3 = actor, r4 = {vec4 rot, vec4 pos}; copies both to actor+0x10 /
// actor+0x20 and never touches the proxy (verifier correction 4).
PPC_FUNC(sub_821E3CF8) {
  if (enabled(base) && ctx.r3.u32 == g_supActor.load() && ctx.r3.u32) {
    const Mem m{base};
    float p[3];
    m.vec3(ctx.r4.u32 + 16, p);
    if (budget(g_bEvent, 64) && g_nEvent.fetch_add(1) < 1000000u)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[COLL_MOVER] SETXFORM lr=0x%08X -> (%.2f %.2f %.2f) t=%u",
                  (uint32_t)ctx.lr, p[0], p[1], p[2], g_tick.load());
  }
  __imp__sub_821E3CF8(ctx, base);
}

PPC_FUNC(sub_821AE1C8) {
  if (enabled(base) && ctx.r3.u32 == g_supPrxy.load() && ctx.r3.u32) {
    const Mem m{base};
    if (budget(g_bEvent, 64) && g_nEvent.fetch_add(1) < 1000000u)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[COLL_MOVER] SET_PHYSREP_MODE %u lr=0x%08X lock8C=%u t=%u",
                  ctx.r4.u32, (uint32_t)ctx.lr, m.u8(ctx.r3.u32 + 0x8C), g_tick.load());
  }
  __imp__sub_821AE1C8(ctx, base);
}

PPC_FUNC(sub_821AE388) {
  if (enabled(base) && ctx.r3.u32 == g_supPrxy.load() && ctx.r3.u32) {
    if (budget(g_bEvent, 64) && g_nEvent.fetch_add(1) < 1000000u)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[COLL_MOVER] DEACTIVATE lr=0x%08X t=%u", (uint32_t)ctx.lr,
                  g_tick.load());
  }
  __imp__sub_821AE388(ctx, base);
}

PPC_FUNC(sub_821AE218) {
  if (enabled(base) && ctx.r3.u32 == g_supPrxy.load() && ctx.r3.u32) {
    if (budget(g_bEvent, 64) && g_nEvent.fetch_add(1) < 1000000u)
      SR_LOG_BOOT(sr::LogSeverity::Warn, "[COLL_MOVER] SET_FILTERINFO layer=%u group=%u lr=0x%08X t=%u",
                  ctx.r4.u32 & 0xFFFF, ctx.r5.u32 & 0xFFFF, (uint32_t)ctx.lr, g_tick.load());
  }
  __imp__sub_821AE218(ctx, base);
}

// hkWorld::addEntity: r3 = world, r4 = entity, r5 = activation. Locked world (+0x8C != 0): the
// add is queued (returns 0) and re-run unlocked from the op queue, which logs "added" then.
PPC_FUNC(sub_82596508) {
  const bool on = enabled(base);
  const Mem m{base};
  const uint32_t world = ctx.r3.u32, body = ctx.r4.u32, lr = (uint32_t)ctx.lr;
  const uint32_t lock = on ? m.u32(world + 0x8C) : 0;
  __imp__sub_82596508(ctx, base);   // the one, unconditional delegation
  if (!on) return;
  const auto t0 = std::chrono::steady_clock::now();
  (lock ? g_addsQueued : g_adds).fetch_add(1, std::memory_order_relaxed);
  worldEvent(m, "ADD", lock ? "queued" : "added", world, lock, body, bodyInfo(m, body), lr);
  g_selfNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - t0).count(),
                     std::memory_order_relaxed);
}

// hkWorld::removeEntity: r3 = result*, r4 = world, r5 = entity. The body can be released by the
// remove (reference drop), so it is read BEFORE delegating.
PPC_FUNC(sub_825967C0) {
  if (enabled(base)) {
    const auto t0 = std::chrono::steady_clock::now();
    const Mem m{base};
    const uint32_t world = ctx.r4.u32, body = ctx.r5.u32, lr = (uint32_t)ctx.lr;
    const uint32_t lock = m.u32(world + 0x8C);
    const BodyInfo b = bodyInfo(m, body);
    (lock ? g_removesQueued : g_removes).fetch_add(1, std::memory_order_relaxed);
    worldEvent(m, "REMOVE", lock ? "queued" : "removed", world, lock, body, b, lr);
    g_selfNs.fetch_add((uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - t0).count(),
                       std::memory_order_relaxed);
  }
  __imp__sub_825967C0(ctx, base);   // the one, unconditional delegation
}
