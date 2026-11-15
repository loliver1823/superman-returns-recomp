// fix/postgame-apt (lane K, 2026-10-17) SR_DAS_WITNESS=1 - OBSERVE-ONLY diagnostic, default off.
//
// The defect: on the R3 route's post-game idle the game-kernel runner thread died of a host stack
// overflow in AptHashTreeCountMatching_sub_826CDB08 recursing on ONE attribute container forever
// (meter R3_a2 dump: every frame a2 = container 0x0081CC20, iterator 0x05BCE048; the node there is a
// group (flags 0x040E0005) whose child IS that container - a self-cycle), under
// EntitySpawner_SpawnGroupFromXml -> ... -> DasDocHandle_Ctor -> DatAssetBinding_Load_sub_8223BE90
// -> AptHashTreeCollectByMask. The dump has no guest memory, so this witness records who builds and
// frees the DAS ("Data Attribute System") trees while the route runs.
//
// The DAS tree (IDA, verified 2026-10-17):
//   container (20 B, "FixedAlloc DAS" = ThreadSafeFixedAllocatorMgr *(*(0x829A23A8)+4), locked):
//     +0 begin, +4 end, +8 capacity (eastl vector of node pointers, sorted by atom)
//     alloc FixedPoolBlk20AllocLocked_sub_82158EF8(mgr r3) -> r3; free sub_82158D30(mgr r3, ptr r4)
//   node (12 B, plus 8/12/16-B value blocks) from the DAS node wrapper *(0x829A23B0):
//     +0 name atom, +4 value / child container, +8 flags: byte 0 = type mask, (flags >> 17) & 0x7F =
//     type (7 = group: +4 is a container), bit 16 = shared, low 16 bits (+10) = refcount
//     alloc sub_82152498(wrapper r3, size r4) -> DOGMA PoolManager::Alloc_sub_82709470(wrapper+8)
//     free  sub_821524A0(wrapper r3, ptr r4, size r5) -> DogmaPoolManager_Free_sub_82709080(wrapper+8)
//     The DOGMA PoolManager has NO lock of its own (sub_82709470 / sub_82709080 decompiles).
//   AttribTree_Clone_sub_822366C8(r3, src container r4) -> new container; AptHashTreeDestroy_sub_826CDBC8
//   (container r3); AptValueAppendPtr_sub_826CE440(container r3, atom r4, type byte r5, child r6);
//   share sub_826CE400(container r3, node r4) (refcount + 1); DatAssetBinding_Load_sub_8223BE90
//   (binding r3, asset name r4, raw r5).
//
// Logged ([DAS_WITNESS], first 40 of each kind in full, then powers of two):
//   NODE_RACE        two host threads inside the DAS node pool (alloc/free) at once - the pool is unlocked
//   NODE_DOUBLE_GRANT / NODE_DOUBLE_FREE / BOX_DOUBLE_GRANT / BOX_DOUBLE_FREE
//                    the pool handed out a block that is live / a block was freed twice (with its last
//                    alloc + last two frees: seq, host tid, host call chain as exe offsets)
//   TREE_CONCURRENT  two host threads inside Clone / Destroy / DatAssetBinding_Load at once
//   SELF_LINK        a group node is appended or shared into the container it points at
//   DANGLING         a group node's child container is not live (freed) when appended / shared
//   SHARE_FREED      a node shared into a container is not live
//   REFCOUNT_ZERO    Destroy finds a node with refcount 0 in a live container
//   CYCLE            CountMatching recursion depth reached 64 on one thread (REPEAT = a container repeats on
//                    the active path, else DEPTH64 = a deep tree): the path, the containers,
//                    the nodes and every involved block's history (once per thread, max 4)
//   NODE_ALLOC_NULL / BOX_ALLOC_NULL   the DAS node pool / container FixedAlloc returned NULL
//   POOL_ALLOC_NULL  any DOGMA PoolManager::Alloc_sub_82709470 returned NULL (called from
//                    kernel/alloc_overlap_watch.cpp's observer): the pool (which one: the Lua script pool
//                    *(*(0x8296D8CC)+4), the DAS node pool *(*(0x829A23B0)+8), or other), the request, the pool's
//                    fields (+16 overflow-pool size, +20 largest pooled size, +24 flags, +32 backing allocator,
//                    +36 alloc count) and whether the request went to the backing allocator directly (> +20)
//   LUA_REALLOC_NULL Lua's l_realloc_sub_82164818 returned NULL for a non-zero size (-> LUA_ERRMEM): block, old
//                    and new size, the Lua heap's live bytes / blocks (alloc_overlap_watch's tracker) and the
//                    raw words at G+32 / G+36 of the script lua_State's global_State (Lua 5.0: GCthreshold /
//                    nblocks), so a failed GROW (which memcpys to guest 0) and a huge request are told apart
// Host call chains are exe offsets (resolve: cdb `ln SupermanReturns+0x<ofs>` on the same exe's PDB).
// Never writes guest memory, never changes a register, a return value or a call.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#endif

extern "C" void __imp__sub_82152498(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821524A0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82158EF8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82158D30(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_822366C8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826CDBC8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8223BE90(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826CE440(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826CE400(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826CDB08(PPCContext& __restrict ctx, uint8_t* base);

namespace {

constexpr uint32_t kNodeWrapperGlobal = 0x829A23B0u;   // DAS node allocator wrapper
constexpr uint32_t kBoxWrapperGlobal = 0x829A23A8u;    // DAS container allocator wrapper

bool witnessOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_DAS_WITNESS");
    return e && e[0] == '1';
  }();
  return v;
}

// Fault-free reads (Astra review 1, P1): the witness follows pointers the guest may never dereference (a corrupted
// tree), and the harness VEH makes a host AV fatal. The XEX image is always mapped; anything else goes through the
// cached VirtualQuery predicate. An unreadable word reads as 0 (every walk stops on 0).
inline bool readable(const uint8_t* base, uint32_t va, uint32_t n) {
  if (va < 0x10000u || va > 0xFFFFFFF0u - n) return false;
  if (va >= 0x82000000u && va + n <= 0x82A00000u) return true;
  return sr::kernel::hostRangeReadableCached(base + va, n);
}

inline uint32_t rd32(const uint8_t* base, uint32_t va) {
  if (!readable(base, va, 4)) return 0;
  const uint8_t* p = base + va;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint32_t hostTid() {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return 0;
#endif
}

uintptr_t exeBase() {
#if defined(_WIN32)
  static const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  return b;
#else
  return 0;
#endif
}

constexpr int kFrames = 10;
std::atomic<uint64_t> g_seq{0};

struct Rec {
  uint64_t seq = 0;
  uint32_t tid = 0;
  uint32_t ofs[kFrames] = {};   // host return addresses as exe offsets (0 = none / not in the exe)
};

__attribute__((noinline)) Rec capture() {
  Rec r;
  r.seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
  r.tid = hostTid();
#if defined(_WIN32)
  void* frames[kFrames + 2] = {};
  const USHORT n = RtlCaptureStackBackTrace(2, kFrames, frames, nullptr);
  const uintptr_t eb = exeBase();
  for (USHORT i = 0; i < n && i < kFrames; ++i) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
    r.ofs[i] = (a > eb && a - eb < 0x40000000u) ? static_cast<uint32_t>(a - eb) : 0xFFFFFFFFu;
  }
#endif
  return r;
}

int fmtRec(char* out, size_t n, const char* tag, const Rec& r) {
  if (!r.seq) return snprintf(out, n, " | %s: none", tag);
  int o = snprintf(out, n, " | %s: seq=%llu tid=%u at", tag, (unsigned long long)r.seq, r.tid);
  for (int i = 0; i < kFrames && r.ofs[i] && o < (int)n - 12; ++i)
    o += snprintf(out + o, n - o, " %X", r.ofs[i]);
  return o;
}

struct Hist {
  uint8_t state = 0;   // 0 unknown, 1 live, 2 free
  uint32_t size = 0;
  Rec alloc;
  Rec freed[2];        // [0] latest
};

enum Kind {
  kNodeRace, kNodeDoubleGrant, kNodeDoubleFree, kBoxDoubleGrant, kBoxDoubleFree, kTreeConcurrent,
  kSelfLink, kDangling, kShareFreed, kRefcountZero, kCycle, kNodeAllocNull, kBoxAllocNull, kPoolAllocNull,
  kLuaReallocNull, kKinds
};
const char* const kKindName[kKinds] = {
    "NODE_RACE", "NODE_DOUBLE_GRANT", "NODE_DOUBLE_FREE", "BOX_DOUBLE_GRANT", "BOX_DOUBLE_FREE",
    "TREE_CONCURRENT", "SELF_LINK", "DANGLING", "SHARE_FREED", "REFCOUNT_ZERO", "CYCLE",
    "NODE_ALLOC_NULL", "BOX_ALLOC_NULL", "POOL_ALLOC_NULL", "LUA_REALLOC_NULL"};
std::atomic<unsigned> g_kindCount[kKinds];

bool shouldLog(Kind k) {
  const unsigned n = g_kindCount[k].fetch_add(1, std::memory_order_relaxed) + 1;
  return n <= 40 || (n & (n - 1)) == 0;
}

std::mutex g_mu;                                // guards both maps (never held across a guest call)
std::unordered_map<uint32_t, Hist> g_nodes;     // DAS node pool blocks (all sizes)
std::unordered_map<uint32_t, Hist> g_boxes;     // DAS containers

void report(Kind k, const char* what, const Rec& now, const Hist* h, const char* h2tag = nullptr,
            const Hist* h2 = nullptr) {
  char buf[2048];
  int o = snprintf(buf, sizeof(buf), "%s #%u %s", kKindName[k],
                   g_kindCount[k].load(std::memory_order_relaxed), what);
  o += fmtRec(buf + o, sizeof(buf) - o, "now", now);
  if (h && o < (int)sizeof(buf) - 400) {
    o += fmtRec(buf + o, sizeof(buf) - o, "lastAlloc", h->alloc);
    o += fmtRec(buf + o, sizeof(buf) - o, "lastFree", h->freed[0]);
    o += fmtRec(buf + o, sizeof(buf) - o, "prevFree", h->freed[1]);
  }
  if (h2 && o < (int)sizeof(buf) - 400) {
    o += snprintf(buf + o, sizeof(buf) - o, " || %s state=%u", h2tag ? h2tag : "other", h2->state);
    o += fmtRec(buf + o, sizeof(buf) - o, "lastAlloc", h2->alloc);
    o += fmtRec(buf + o, sizeof(buf) - o, "lastFree", h2->freed[0]);
  }
  SR_LOG_BOOT(sr::LogSeverity::Error, "[DAS_WITNESS] %s", buf);
}

// ---- "inside" owners: lock-free, so the witness does not serialize what it watches ----
struct Inside {
  std::atomic<uint32_t> tid{0};
  std::atomic<uint32_t> depth{0};
  std::atomic<uint32_t> what{0};   // which entry the owner is in (for the report)
  std::mutex recMu;                // guards ownerRec (Astra review 1, P2): host-only, never held across a guest call
  Rec ownerRec;
};
Inside g_nodePool, g_treeOps;

// Returns true if this call took ownership (and must release it).
bool enterInside(Inside& in, uint32_t what, Kind raceKind, const char* where, const Rec& now) {
  uint32_t expect = 0;
  if (in.tid.compare_exchange_strong(expect, now.tid, std::memory_order_acq_rel)) {
    in.depth.store(1, std::memory_order_relaxed);
    in.what.store(what, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lk(in.recMu);
      in.ownerRec = now;
    }
    return true;
  }
  if (expect == now.tid) {
    in.depth.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (shouldLog(raceKind)) {
    char w[160];
    snprintf(w, sizeof(w), "%s by tid=%u while tid=%u is inside (entry %08X)", where, now.tid, expect,
             in.what.load(std::memory_order_relaxed));
    Hist owner;
    owner.state = 1;
    {
      std::lock_guard<std::mutex> lk(in.recMu);
      owner.alloc = in.ownerRec;   // the latest owner's entry (it may have left since: the report says "owner")
    }
    report(raceKind, w, now, nullptr, "owner", &owner);
  }
  return false;
}
void leaveInside(Inside& in, bool owned, uint32_t tid) {
  if (owned) {
    in.depth.store(0, std::memory_order_relaxed);
    in.tid.store(0, std::memory_order_release);
  } else if (in.tid.load(std::memory_order_relaxed) == tid) {
    in.depth.fetch_sub(1, std::memory_order_relaxed);
  }
}

uint32_t nodeWrapper(const uint8_t* base) { return rd32(base, kNodeWrapperGlobal); }
uint32_t boxMgr(const uint8_t* base) {
  const uint32_t w = rd32(base, kBoxWrapperGlobal);
  return w ? rd32(base, w + 4) : 0;
}

uint8_t boxState(uint32_t va) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_boxes.find(va);
  return it == g_boxes.end() ? 0 : it->second.state;
}
uint8_t nodeState(uint32_t va) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_nodes.find(va);
  return it == g_nodes.end() ? 0 : it->second.state;
}

bool isGroup(uint32_t flags) { return (flags & 0xFE0000u) == 0xE0000u; }

// ---- per-thread context for the cycle report ----
struct LoadCtx {
  uint32_t binding = 0, name = 0, raw = 0, payload = 0, tree = 0;
};
thread_local LoadCtx t_load;
thread_local uint32_t t_depth = 0;
thread_local uint32_t t_path[64] = {};
thread_local bool t_cycleReported = false;
std::atomic<int> g_cycleReports{0};

void dumpBox(const uint8_t* base, const char* tag, uint32_t box) {
  const uint32_t b = rd32(base, box), e = rd32(base, box + 4), cap = rd32(base, box + 8);
  Hist h;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_boxes.find(box);
    if (it != g_boxes.end()) h = it->second;
  }
  char buf[2048];
  int o = snprintf(buf, sizeof(buf), "CYCLE %s box=%08X begin=%08X end=%08X cap=%08X +12=%08X +16=%08X state=%u n=%d:",
                   tag, box, b, e, cap, rd32(base, box + 12), rd32(base, box + 16), h.state,
                   (e >= b && e - b < 0x10000u) ? int((e - b) / 4) : -1);
  if (e >= b && e - b < 0x10000u) {
    for (uint32_t p = b; p < e && o < (int)sizeof(buf) - 80; p += 4) {
      const uint32_t node = rd32(base, p);
      const uint32_t fl = rd32(base, node + 8);
      o += snprintf(buf + o, sizeof(buf) - o, " [%08X: n=%08X atom=%08X v=%08X fl=%08X s=%u]", p, node,
                    rd32(base, node), rd32(base, node + 4), fl, nodeState(node));
    }
  }
  SR_LOG_BOOT(sr::LogSeverity::Error, "[DAS_WITNESS] %s", buf);
  char w[96];
  snprintf(w, sizeof(w), "%s box %08X history", tag, box);
  Rec none;
  report(kCycle, w, none, &h);
}

void dumpNodeHist(const uint8_t* base, const char* tag, uint32_t node) {
  Hist h;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_nodes.find(node);
    if (it != g_nodes.end()) h = it->second;
  }
  char w[160];
  snprintf(w, sizeof(w), "%s node %08X atom=%08X v=%08X fl=%08X state=%u history", tag, node,
           rd32(base, node), rd32(base, node + 4), rd32(base, node + 8), h.state);
  Rec none;
  report(kCycle, w, none, &h);
}

void reportCycle(const uint8_t* base, uint32_t collector, uint32_t box) {
  if (t_cycleReported) return;
  t_cycleReported = true;
  if (g_cycleReports.fetch_add(1, std::memory_order_relaxed) >= 4) return;
  const Rec now = capture();
  if (t_load.binding) {
    const uint32_t handle = rd32(base, t_load.binding);
    t_load.payload = handle ? rd32(base, handle + 12) : 0;
    t_load.tree = rd32(base, t_load.binding + 4);
  }
  char w[320];
  snprintf(w, sizeof(w),
           "depth=%u collector=%08X box=%08X | load binding=%08X name=%08X raw=%u payload=%08X tree=%08X | path:",
           t_depth, collector, box, t_load.binding, t_load.name, t_load.raw, t_load.payload, t_load.tree);
  std::string path(w);
  for (uint32_t d = 0; d < 12 && d < t_depth; ++d) {
    char s[16];
    snprintf(s, sizeof(s), " %08X", t_path[d]);
    path += s;
  }
  // Astra review 1 (P3): depth alone is not a cycle - say CYCLE only when a container repeats on the active path
  // (the first repeat is named), else "DEPTH64 (no repeat)".
  int rep = -1, repOf = -1;
  for (uint32_t i = 0; i < 64 && i < t_depth && rep < 0; ++i)
    for (uint32_t j = 0; j < i; ++j)
      if (t_path[i] == t_path[j]) {
        rep = static_cast<int>(i);
        repOf = static_cast<int>(j);
        break;
      }
  char rs[96];
  if (rep >= 0)
    snprintf(rs, sizeof(rs), " | REPEAT path[%d]=path[%d]=%08X", rep, repOf, t_path[rep]);
  else
    snprintf(rs, sizeof(rs), " | DEPTH64 (no container repeats on the path: a deep tree, not a cycle)");
  path += rs;
  g_kindCount[kCycle].fetch_add(1, std::memory_order_relaxed);
  report(kCycle, path.c_str(), now, nullptr);
  // The first boxes of the path (the root and its first descendants) and the repeating box.
  uint32_t seen[8] = {};
  int ns = 0;
  for (uint32_t d = 0; d < 6 && d < t_depth; ++d) {
    const uint32_t bx = t_path[d];
    bool dup = false;
    for (int i = 0; i < ns; ++i) dup |= seen[i] == bx;
    if (dup || !bx) continue;
    seen[ns++] = bx;
    char tag[24];
    snprintf(tag, sizeof(tag), "path[%u]", d);
    dumpBox(base, tag, bx);
  }
  // Every group node in the repeating box whose child is the box itself, with its history.
  const uint32_t b = rd32(base, box), e = rd32(base, box + 4);
  if (e >= b && e - b < 0x10000u)
    for (uint32_t p = b; p < e; p += 4) {
      const uint32_t node = rd32(base, p);
      const uint32_t fl = rd32(base, node + 8);
      if (isGroup(fl) && rd32(base, node + 4) == box) dumpNodeHist(base, "self", node);
    }
  // The root's group nodes (the entry into the cycle).
  if (t_depth > 0) {
    const uint32_t root = t_path[0];
    const uint32_t rb = rd32(base, root), re = rd32(base, root + 4);
    if (re >= rb && re - rb < 0x1000u)
      for (uint32_t p = rb; p < re; p += 4) {
        const uint32_t node = rd32(base, p);
        if (isGroup(rd32(base, node + 8))) dumpNodeHist(base, "root", node);
      }
  }
}

}  // namespace

// ---- hooks. Each override calls its __imp__ body exactly once, unconditionally; the witness work is
// before / after it and only reads guest memory. ----

namespace {
struct Pre {
  bool on = false;
  bool owned = false;
  Rec now;
};

// ---- activation witness + census (so a run with no finding still proves the hooks fired) ----
std::atomic<uint64_t> g_cNodeAlloc{0}, g_cNodeFree{0}, g_cBoxAlloc{0}, g_cBoxFree{0}, g_cClone{0}, g_cDestroy{0},
    g_cLoad{0}, g_cCount{0};
std::atomic<uint32_t> g_maxDepth{0};
std::atomic<uint32_t> g_nodeTids[16];   // distinct host threads that used the DAS node pool (first 16)
std::atomic<int64_t> g_lastCensusMs{-1};

void noteNodeTid(uint32_t tid) {
  for (auto& t : g_nodeTids) {
    uint32_t cur = t.load(std::memory_order_relaxed);
    if (cur == tid) return;
    if (cur == 0 && t.compare_exchange_strong(cur, tid)) return;
    if (cur == tid) return;
  }
}

uint32_t g_nullWords[3] = {};   // guest VA 0..11 (where a NULL DAS node lands), sampled by census()

void census() {
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t last = g_lastCensusMs.load(std::memory_order_relaxed);
  if (last >= 0 && now - last < 30000) return;
  if (!g_lastCensusMs.compare_exchange_strong(last, now)) return;
  char tids[16 * 11 + 1];
  int o = 0;
  tids[0] = 0;
  for (auto& t : g_nodeTids)
    if (const uint32_t v = t.load(std::memory_order_relaxed)) o += snprintf(tids + o, sizeof(tids) - o, " %u", v);
  if (const uint8_t* gb = sr::kernel::getGuestBase())
    for (int i = 0; i < 3; ++i)
      g_nullWords[i] = sr::kernel::hostRangeReadableCached(gb, 12)
                           ? (uint32_t(gb[i * 4]) << 24) | (uint32_t(gb[i * 4 + 1]) << 16) |
                                 (uint32_t(gb[i * 4 + 2]) << 8) | uint32_t(gb[i * 4 + 3])
                           : 0xFFFFFFFFu;
  size_t liveNodes = 0, liveBoxes = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& kv : g_nodes) liveNodes += kv.second.state == 1;
    for (auto& kv : g_boxes) liveBoxes += kv.second.state == 1;
  }
  SR_LOG_BOOT(sr::LogSeverity::Error,
              "[DAS_WITNESS] census%s node alloc=%llu free=%llu live=%zu | box alloc=%llu free=%llu live=%zu | "
              "clone=%llu destroy=%llu load=%llu count=%llu maxDepth=%u | node-pool threads:%s | findings race=%u "
              "dgrant=%u dfree=%u bdgrant=%u bdfree=%u conc=%u self=%u dangling=%u sharefreed=%u rc0=%u cycle=%u "
              "nodeNull=%u boxNull=%u poolNull=%u luaNull=%u | guest[0..12)=%08X %08X %08X",
              last < 0 ? " (first)" : "", (unsigned long long)g_cNodeAlloc.load(), (unsigned long long)g_cNodeFree.load(),
              liveNodes, (unsigned long long)g_cBoxAlloc.load(), (unsigned long long)g_cBoxFree.load(), liveBoxes,
              (unsigned long long)g_cClone.load(), (unsigned long long)g_cDestroy.load(),
              (unsigned long long)g_cLoad.load(), (unsigned long long)g_cCount.load(), g_maxDepth.load(),
              tids[0] ? tids : " none", g_kindCount[kNodeRace].load(), g_kindCount[kNodeDoubleGrant].load(),
              g_kindCount[kNodeDoubleFree].load(), g_kindCount[kBoxDoubleGrant].load(),
              g_kindCount[kBoxDoubleFree].load(), g_kindCount[kTreeConcurrent].load(), g_kindCount[kSelfLink].load(),
              g_kindCount[kDangling].load(), g_kindCount[kShareFreed].load(), g_kindCount[kRefcountZero].load(),
              g_kindCount[kCycle].load(), g_kindCount[kNodeAllocNull].load(), g_kindCount[kBoxAllocNull].load(),
              g_kindCount[kPoolAllocNull].load(), g_kindCount[kLuaReallocNull].load(),
              g_nullWords[0], g_nullWords[1], g_nullWords[2]);
}

// DAS node pool alloc / free: the "inside" slot spans only the guest body.
Pre nodePre(const PPCContext& ctx, const uint8_t* base, bool isFree) {
  Pre p;
  const uint32_t wrapper = ctx.r3.u32;
  if (!witnessOn() || !wrapper || wrapper != nodeWrapper(base)) return p;
  p.on = true;
  p.now = capture();
  (isFree ? g_cNodeFree : g_cNodeAlloc).fetch_add(1, std::memory_order_relaxed);
  noteNodeTid(p.now.tid);
  if (isFree && ctx.r4.u32) {
    const uint32_t blk = ctx.r4.u32;
    std::lock_guard<std::mutex> lk(g_mu);
    Hist& h = g_nodes[blk];
    if (h.state == 2 && shouldLog(kNodeDoubleFree)) {
      char w[96];
      snprintf(w, sizeof(w), "block=%08X size=%u", blk, ctx.r5.u32);
      report(kNodeDoubleFree, w, p.now, &h);
    }
    h.state = 2;
    h.freed[1] = h.freed[0];
    h.freed[0] = p.now;
  }
  p.owned = enterInside(g_nodePool, isFree ? 0x821524A0u : 0x82152498u, kNodeRace,
                        isFree ? "node free" : "node alloc", p.now);
  return p;
}

void nodeAllocPost(const Pre& p, uint32_t blk, uint32_t size) {
  if (!p.on) return;
  leaveInside(g_nodePool, p.owned, p.now.tid);
  if (!blk) {
    // The DAS node pool is out of memory: AptHashTreeNodeCreate_sub_826CD4E0 does not check, it then writes the
    // node at guest address 0 (the console faults there; our guest page 0 is committed read-write).
    if (shouldLog(kNodeAllocNull)) {
      char w[96];
      snprintf(w, sizeof(w), "size=%u: the DAS node pool returned NULL", size);
      report(kNodeAllocNull, w, p.now, nullptr);
    }
    return;
  }
  std::lock_guard<std::mutex> lk(g_mu);
  Hist& h = g_nodes[blk];
  if (h.state == 1 && shouldLog(kNodeDoubleGrant)) {
    char w[128];
    snprintf(w, sizeof(w), "block=%08X size=%u (live since seq=%llu, size %u)", blk, size,
             (unsigned long long)h.alloc.seq, h.size);
    report(kNodeDoubleGrant, w, p.now, &h);
  }
  h.state = 1;
  h.size = size;
  h.alloc = p.now;
}

void nodeAllocCensus(const Pre& p) {
  if (p.on) census();
}

constexpr uint32_t kScriptMgrGlobal = 0x8296D8CCu;   // Lua script manager: +4 its PoolManager, +12 the lua_State

const char* poolName(const uint8_t* base, uint32_t pool) {
  if (pool && pool == rd32(base, rd32(base, kScriptMgrGlobal) + 4u)) return "lua-script";
  if (pool && pool == rd32(base, nodeWrapper(base) + 8u)) return "das-node";
  return "other";
}

void boxAllocPost(bool das, uint32_t blk) {
  if (das && !blk && shouldLog(kBoxAllocNull)) {
    const Rec now = capture();
    report(kBoxAllocNull, "the DAS container FixedAlloc returned NULL", now, nullptr);
  }
  if (!das || !blk) return;
  g_cBoxAlloc.fetch_add(1, std::memory_order_relaxed);
  const Rec now = capture();
  std::lock_guard<std::mutex> lk(g_mu);
  Hist& h = g_boxes[blk];
  if (h.state == 1 && shouldLog(kBoxDoubleGrant)) {
    char w[96];
    snprintf(w, sizeof(w), "box=%08X (live since seq=%llu)", blk, (unsigned long long)h.alloc.seq);
    report(kBoxDoubleGrant, w, now, &h);
  }
  h.state = 1;
  h.size = 20;
  h.alloc = now;
}

void boxFreePre(const uint8_t* base, uint32_t mgr, uint32_t blk) {
  if (!witnessOn() || !mgr || !blk || mgr != boxMgr(base)) return;
  g_cBoxFree.fetch_add(1, std::memory_order_relaxed);
  const Rec now = capture();
  std::lock_guard<std::mutex> lk(g_mu);
  Hist& h = g_boxes[blk];
  if (h.state == 2 && shouldLog(kBoxDoubleFree)) {
    char w[64];
    snprintf(w, sizeof(w), "box=%08X", blk);
    report(kBoxDoubleFree, w, now, &h);
  }
  h.state = 2;
  h.freed[1] = h.freed[0];
  h.freed[0] = now;
}

Pre treePre(uint32_t entry, const char* name) {
  Pre p;
  if (!witnessOn()) return p;
  p.on = true;
  (entry == 0x822366C8u ? g_cClone : entry == 0x826CDBC8u ? g_cDestroy : g_cLoad).fetch_add(1, std::memory_order_relaxed);
  census();
  p.now = capture();
  p.owned = enterInside(g_treeOps, entry, kTreeConcurrent, name, p.now);
  return p;
}
void treePost(const Pre& p) {
  if (p.on) leaveInside(g_treeOps, p.owned, p.now.tid);
}

void destroyScan(const uint8_t* base, uint32_t box) {
  const uint32_t b = rd32(base, box), e = rd32(base, box + 4);
  if (!box || e < b || e - b >= 0x10000u) return;
  for (uint32_t p = b; p < e; p += 4) {
    const uint32_t node = rd32(base, p);
    const uint32_t fl = rd32(base, node + 8);
    if (node && (fl & 0xFFFFu) == 0 && shouldLog(kRefcountZero)) {
      const Rec now = capture();
      char w[160];
      snprintf(w, sizeof(w), "box=%08X node=%08X atom=%08X v=%08X fl=%08X nodeState=%u boxState=%u", box, node,
               rd32(base, node), rd32(base, node + 4), fl, nodeState(node), boxState(box));
      Hist h;
      {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_nodes.find(node);
        if (it != g_nodes.end()) h = it->second;
      }
      report(kRefcountZero, w, now, &h);
    }
  }
}

void appendCheck(uint32_t box, uint32_t atom, uint32_t type, uint32_t child) {
  if (!child || (child != box && boxState(child) != 2)) return;
  const Kind k = child == box ? kSelfLink : kDangling;
  if (!shouldLog(k)) return;
  const Rec now = capture();
  char w[128];
  snprintf(w, sizeof(w), "append box=%08X atom=%08X type=%02X child=%08X childState=%u", box, atom, type & 0xFF,
           child, boxState(child));
  Hist h;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_boxes.find(child);
    if (it != g_boxes.end()) h = it->second;
  }
  report(k, w, now, &h);
}

void shareCheck(const uint8_t* base, uint32_t box, uint32_t node) {
  if (!node) return;
  const uint32_t fl = rd32(base, node + 8), v = rd32(base, node + 4);
  const uint8_t ns = nodeState(node);
  Kind k = kKinds;
  if (ns == 2) k = kShareFreed;
  else if (isGroup(fl) && v == box) k = kSelfLink;
  else if (isGroup(fl) && v && boxState(v) == 2) k = kDangling;
  if (k == kKinds || !shouldLog(k)) return;
  const Rec now = capture();
  char w[160];
  snprintf(w, sizeof(w), "share box=%08X node=%08X atom=%08X v=%08X fl=%08X nodeState=%u childState=%u", box, node,
           rd32(base, node), v, fl, ns, isGroup(fl) ? boxState(v) : 0);
  Hist h, h2;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_nodes.find(node);
    if (it != g_nodes.end()) h = it->second;
    auto it2 = g_boxes.find(v);
    if (it2 != g_boxes.end()) h2 = it2->second;
  }
  report(k, w, now, &h, "child", isGroup(fl) ? &h2 : nullptr);
}
}  // namespace

// sub_82152498(wrapper r3, size r4) -> r3 block: the DAS node pool's alloc (unlocked DOGMA PoolManager).
PPC_FUNC(sub_82152498) {
  const uint32_t size = ctx.r4.u32;
  const Pre p = nodePre(ctx, base, false);
  __imp__sub_82152498(ctx, base);
  nodeAllocPost(p, ctx.r3.u32, size);
  nodeAllocCensus(p);
}

// sub_821524A0(wrapper r3, block r4, size r5): the DAS node pool's free.
PPC_FUNC(sub_821524A0) {
  const Pre p = nodePre(ctx, base, true);
  __imp__sub_821524A0(ctx, base);
  if (p.on) leaveInside(g_nodePool, p.owned, p.now.tid);
}

// FixedPoolBlk20AllocLocked_sub_82158EF8(mgr r3) -> r3: a DAS container when mgr is the DAS FixedAlloc.
PPC_FUNC(sub_82158EF8) {
  const uint32_t mgr = ctx.r3.u32;
  const bool das = witnessOn() && mgr && mgr == boxMgr(base);
  __imp__sub_82158EF8(ctx, base);
  boxAllocPost(das, ctx.r3.u32);
}

// sub_82158D30(mgr r3, block r4): the ThreadSafeFixedAllocatorMgr free.
PPC_FUNC(sub_82158D30) {
  boxFreePre(base, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_82158D30(ctx, base);
}

// AttribTree_Clone_sub_822366C8(r3, src r4) -> r3 new container
PPC_FUNC(sub_822366C8) {
  const Pre p = treePre(0x822366C8u, "Clone");
  __imp__sub_822366C8(ctx, base);
  treePost(p);
}

// AptHashTreeDestroy_sub_826CDBC8(container r3)
PPC_FUNC(sub_826CDBC8) {
  if (witnessOn()) destroyScan(base, ctx.r3.u32);
  const Pre p = treePre(0x826CDBC8u, "Destroy");
  __imp__sub_826CDBC8(ctx, base);
  treePost(p);
}

// DatAssetBinding_Load_sub_8223BE90(binding r3, asset name r4, raw r5)
PPC_FUNC(sub_8223BE90) {
  const LoadCtx saved = t_load;
  t_load.binding = ctx.r3.u32;
  t_load.name = ctx.r4.u32;
  t_load.raw = ctx.r5.u32 & 0xFF;
  t_load.payload = 0;
  t_load.tree = 0;
  const Pre p = treePre(0x8223BE90u, "DatAssetBinding_Load");
  __imp__sub_8223BE90(ctx, base);
  treePost(p);
  t_load = saved;
}

// AptValueAppendPtr_sub_826CE440(container r3, atom r4, type byte r5, child r6)
PPC_FUNC(sub_826CE440) {
  if (witnessOn()) appendCheck(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  __imp__sub_826CE440(ctx, base);
}

// sub_826CE400(container r3, node r4): share a node into a container (refcount + 1)
PPC_FUNC(sub_826CE400) {
  if (witnessOn()) shareCheck(base, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_826CE400(ctx, base);
}

// AptHashTreeCountMatching_sub_826CDB08(collector r3, container r4, flag r5) -> r3 count
PPC_FUNC(sub_826CDB08) {
  const bool on = witnessOn();
  if (on) {
    if (t_depth == 0) g_cCount.fetch_add(1, std::memory_order_relaxed);
    if (t_depth < 64) t_path[t_depth] = ctx.r4.u32;
    if (++t_depth == 64) reportCycle(base, ctx.r3.u32, ctx.r4.u32);
    uint32_t m = g_maxDepth.load(std::memory_order_relaxed);
    while (t_depth > m && !g_maxDepth.compare_exchange_weak(m, t_depth)) {}
  }
  __imp__sub_826CDB08(ctx, base);
  if (on) --t_depth;
}

// ---- called by kernel/alloc_overlap_watch.cpp's existing observers (SR_ORACLE builds), after the guest body ----
namespace sr::daswitness {

void poolAllocNull(const uint8_t* base, uint32_t pool, uint32_t size, uint32_t lr) {
  if (!witnessOn() || !base || !shouldLog(kPoolAllocNull)) return;
  const uint32_t ovf = rd32(base, pool + 16u), maxPooled = rd32(base, pool + 20u), flags = rd32(base, pool + 24u);
  const uint32_t backing = rd32(base, pool + 32u);
  char w[384];
  snprintf(w, sizeof(w),
           "pool=%08X (%s) size=%u guest lr=%08X path=%s | pool +16 ovf=%u +20 maxPooled=%u +24 flags=%08X "
           "+32 backing=%08X (vt %08X) +36 allocs=%u +40=%u",
           pool, poolName(base, pool), size, lr, size > maxPooled ? "outside(direct backing alloc)" : "overflowPool",
           ovf, maxPooled, flags, backing, rd32(base, backing), rd32(base, pool + 36u), rd32(base, pool + 40u));
  report(kPoolAllocNull, w, capture(), nullptr);
}

void luaReallocNull(const uint8_t* base, uint32_t block, uint32_t oldSize, uint32_t newSize, uint32_t lr,
                    unsigned long long liveBytes, unsigned liveBlocks) {
  if (!witnessOn() || !base || !shouldLog(kLuaReallocNull)) return;
  const uint32_t L = rd32(base, rd32(base, kScriptMgrGlobal) + 12u);
  const uint32_t G = L ? rd32(base, L + 16u) : 0;
  char w[384];
  snprintf(w, sizeof(w),
           "block=%08X old=%u new=%u guest lr=%08X (%s) | lua heap live=%llu B in %u blocks | L=%08X G=%08X "
           "G+32=%u G+36=%u",
           block, oldSize, newSize, lr, block ? "GROW: l_realloc memcpys to guest 0" : "fresh alloc", liveBytes,
           liveBlocks, L, G, G ? rd32(base, G + 32u) : 0, G ? rd32(base, G + 36u) : 0);
  report(kLuaReallocNull, w, capture(), nullptr);
}

}  // namespace sr::daswitness
