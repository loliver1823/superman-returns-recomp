// iter-462 SR_POOL_WITNESS=1 (OBSERVE-ONLY diagnostic, default off): a
// witness on the guest's fixed-block small-object pool, the one whose free
// list ends up threading through XEX .rdata vtables (0x8208CE5C, then
// 0x8208D29C = EventButtonMapped -> the D-pad power select dies; the pool
// already returns garbage 0x3846B33D before the HUD loads, and 1,036
// in-image grants in archive/v1_imgro1). SR_IMAGE_RO=1 showed every .rdata
// write is a constructor on memory the pool handed out - the victims - so
// the writer we want is whoever put a LIVE element on a free list.
//
// Pool layout (IDA, data/ida_contracts.tsv):
//   pool  +4 u16 element size, +8 block list head, +0xC current block
//   block +0 byte size, +4 next block, +0xC owning pool, +0x10 free head,
//         +0x14 u16 capacity, +0x16 u16 free count; 24-byte header, the
//         elements are carved downward from block+size in element steps.
//   FixedBlockPool_FreeToBlock_sub_82174210(pool r3, block r4, elem r5):
//         *elem = head; head = elem; ++free count
//   MemPoolPopFreeBlock_sub_821804C0(pool r3) -> r3: elem = head of the
//         current block; head = *elem
//
// Logged ([POOL_WITNESS], first 40 of each kind in full):
//   DOUBLE_FREE   the element being freed is already on its block's list
//   FOREIGN_FREE  the element is not an element of the block it is freed to
//   LIST_CORRUPT  a node on the list (seen during the free walk) is not one
//   POP_FOREIGN   a pop returned something that is not an element of the pool
//   NEXT_CORRUPT  a pop left a head that is not an element (the popped
//                 element was written while on the list: use-after-free, or
//                 it was listed twice and its other copy got constructed)
//   RACE          two host threads inside push/pop of the same pool at once
// Each carries the guest call chain now plus the element's last two frees
// and last allocation (seq, host tid, call chain).
// Never writes guest memory, never changes a return value.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#endif

extern "C" void __imp__sub_82174210(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821804C0(PPCContext& __restrict ctx, uint8_t* base);

namespace {

bool witnessOn() {
  static const bool v = [] {
    const char* e = std::getenv("SR_POOL_WITNESS");
    return e && e[0] == '1';
  }();
  return v;
}

inline uint32_t rd32(const uint8_t* base, uint32_t va) {
  if (va < 0x10000u || va > 0xFFFFFFF0u) return 0;
  const uint8_t* p = base + va;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint32_t rd16(const uint8_t* base, uint32_t va) {
  if (va < 0x10000u || va > 0xFFFFFFF0u) return 0;
  const uint8_t* p = base + va;
  return (uint32_t(p[0]) << 8) | uint32_t(p[1]);
}

uint32_t hostTid() {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return 0;
#endif
}

struct Rec {
  uint64_t seq = 0;
  uint32_t tid = 0;
  uint32_t lr[6] = {};
};

struct ElemHist {
  Rec freed[2];   // [0] = latest free, [1] = the one before
  Rec alloc;      // latest pop
};

struct PoolOwner {
  uint32_t tid = 0;
  uint32_t depth = 0;
};

std::mutex g_mu;
std::unordered_map<uint32_t, ElemHist> g_hist;
std::unordered_map<uint32_t, PoolOwner> g_owner;
std::atomic<uint64_t> g_seq{0};
std::atomic<unsigned> g_kindCount[6];
enum Kind { kDoubleFree, kForeignFree, kListCorrupt, kPopForeign, kNextCorrupt, kRace };
const char* const kKindName[6] = {"DOUBLE_FREE", "FOREIGN_FREE", "LIST_CORRUPT",
                                  "POP_FOREIGN", "NEXT_CORRUPT", "RACE"};

// The guest call chain: the caller's lr, then the PPC back-chain from r1
// (bc = *(r1), saved lr = *(bc - 8)), as the image watchdog walks it.
Rec capture(const PPCContext& ctx, const uint8_t* base) {
  Rec r;
  r.seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
  r.tid = hostTid();
  r.lr[0] = static_cast<uint32_t>(ctx.lr);
  uint32_t fp = ctx.r1.u32;
  for (int d = 1; d < 6 && fp; ++d) {
    const uint32_t bc = rd32(base, fp);
    if (bc <= fp || bc - fp > 0x100000u) break;
    r.lr[d] = rd32(base, bc - 8);
    fp = bc;
  }
  return r;
}

int fmtRec(char* out, size_t n, const char* tag, const Rec& r) {
  if (!r.seq) return snprintf(out, n, " | %s: none", tag);
  return snprintf(out, n, " | %s: seq=%llu tid=%u lr=%08X<%08X<%08X<%08X<%08X<%08X", tag,
                  (unsigned long long)r.seq, r.tid, r.lr[0], r.lr[1], r.lr[2], r.lr[3],
                  r.lr[4], r.lr[5]);
}

bool shouldLog(Kind k) {
  const unsigned n = g_kindCount[k].fetch_add(1, std::memory_order_relaxed) + 1;
  return n <= 40 || (n & (n - 1)) == 0;
}

void report(Kind k, const char* what, const Rec& now, const ElemHist* h) {
  char buf[1024];
  int o = snprintf(buf, sizeof(buf), "[POOL_WITNESS] %s #%u %s", kKindName[k],
                   g_kindCount[k].load(std::memory_order_relaxed), what);
  o += fmtRec(buf + o, sizeof(buf) - o, "now", now);
  if (h && o < (int)sizeof(buf) - 200) {
    o += fmtRec(buf + o, sizeof(buf) - o, "lastFree", h->freed[0]);
    o += fmtRec(buf + o, sizeof(buf) - o, "prevFree", h->freed[1]);
    fmtRec(buf + o, sizeof(buf) - o, "lastAlloc", h->alloc);
  }
  SR_LOG_BOOT(sr::LogSeverity::Error, "%s", buf);
}

// An element of block b: inside [b+24, b+size) at a whole element step from
// the block's end (elements are carved downward from there).
bool isElem(const uint8_t* base, uint32_t b, uint32_t e, uint32_t elemSize) {
  if (!b || !e || !elemSize) return false;
  const uint32_t size = rd32(base, b);
  if (size < 24 || size > 0x1000000u) return false;
  const uint32_t end = b + size;
  if (e < b + 24 || e >= end) return false;
  return ((end - e) % elemSize) == 0;
}

void enterPool(uint32_t pool, const Rec& now) {
  std::lock_guard<std::mutex> lk(g_mu);
  PoolOwner& o = g_owner[pool];
  if (o.depth && o.tid != now.tid) {
    if (shouldLog(kRace)) {
      char what[128];
      snprintf(what, sizeof(what), "pool=%08X owner tid=%u depth=%u, entered by tid=%u", pool,
               o.tid, o.depth, now.tid);
      report(kRace, what, now, nullptr);
    }
    return;  // leave the first owner recorded
  }
  o.tid = now.tid;
  ++o.depth;
}

void leavePool(uint32_t pool, uint32_t tid) {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_owner.find(pool);
  if (it == g_owner.end() || it->second.tid != tid || !it->second.depth) return;
  if (--it->second.depth == 0) it->second.tid = 0;
}

}  // namespace

// FixedBlockPool_FreeToBlock: (pool r3, block r4, elem r5).
PPC_FUNC(sub_82174210) {
  if (!witnessOn()) {
    __imp__sub_82174210(ctx, base);
    return;
  }
  const uint32_t pool = ctx.r3.u32, block = ctx.r4.u32, elem = ctx.r5.u32;
  const uint32_t elemSize = rd16(base, pool + 4);
  const Rec now = capture(ctx, base);
  enterPool(pool, now);
  {
    std::lock_guard<std::mutex> lk(g_mu);
    ElemHist& h = g_hist[elem];
    char what[192];
    if (!isElem(base, block, elem, elemSize) && shouldLog(kForeignFree)) {
      snprintf(what, sizeof(what), "pool=%08X block=%08X elem=%08X elemSize=%u blockSize=%u",
               pool, block, elem, elemSize, rd32(base, block));
      report(kForeignFree, what, now, &h);
    }
    // Recently freed elements sit at the head, so a short walk catches the
    // double free that matters (a free of something freed moments ago).
    uint32_t node = rd32(base, block + 0x10);
    for (int i = 0; i < 256 && node; ++i) {
      if (node == elem) {
        if (shouldLog(kDoubleFree)) {
          snprintf(what, sizeof(what), "pool=%08X block=%08X elem=%08X at list index %d freeCount=%u",
                   pool, block, elem, i, rd16(base, block + 0x16));
          report(kDoubleFree, what, now, &h);
        }
        break;
      }
      if (!isElem(base, block, node, elemSize)) {
        if (shouldLog(kListCorrupt)) {
          snprintf(what, sizeof(what), "pool=%08X block=%08X node %d = %08X is not an element (freeing %08X)",
                   pool, block, i, node, elem);
          report(kListCorrupt, what, now, &g_hist[node]);
        }
        break;
      }
      node = rd32(base, node);
    }
    h.freed[1] = h.freed[0];
    h.freed[0] = now;
  }
  __imp__sub_82174210(ctx, base);
  leavePool(pool, now.tid);
}

// MemPoolPopFreeBlock: (pool r3) -> r3 element.
PPC_FUNC(sub_821804C0) {
  if (!witnessOn()) {
    __imp__sub_821804C0(ctx, base);
    return;
  }
  const uint32_t pool = ctx.r3.u32;
  const Rec now = capture(ctx, base);
  enterPool(pool, now);
  __imp__sub_821804C0(ctx, base);
  leavePool(pool, now.tid);
  const uint32_t elem = ctx.r3.u32;
  if (!elem) return;
  const uint32_t elemSize = rd16(base, pool + 4);
  uint32_t blk = rd32(base, pool + 8);
  for (int i = 0; i < 4096 && blk; ++i) {
    const uint32_t size = rd32(base, blk);
    if (elem >= blk && size && elem < blk + size) break;
    blk = rd32(base, blk + 4);
  }
  std::lock_guard<std::mutex> lk(g_mu);
  ElemHist& h = g_hist[elem];
  char what[192];
  if (!blk || !isElem(base, blk, elem, elemSize)) {
    if (shouldLog(kPopForeign)) {
      snprintf(what, sizeof(what), "pool=%08X returned %08X (elemSize=%u) - not an element of any block",
               pool, elem, elemSize);
      report(kPopForeign, what, now, &h);
    }
  } else {
    const uint32_t head = rd32(base, blk + 0x10);
    if (head && !isElem(base, blk, head, elemSize) && shouldLog(kNextCorrupt)) {
      snprintf(what, sizeof(what), "pool=%08X block=%08X popped %08X whose next %08X is not an element",
               pool, blk, elem, head);
      report(kNextCorrupt, what, now, &h);
    }
  }
  h.alloc = now;
}
