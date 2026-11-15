// Included by kernel/native_flush.cpp only in difftest (SR_DIFFTEST): p2.transport RESIDUE (SR_NATIVE_IDLE_WAIT,
// SR_NATIVE_INVAL_RANGE, SR_NATIVE_LOCK_COHER; docs/NATIVE_ROADMAP.md transport plan item 5, its build note).
//
// The actual SDK bodies against the native ports on identical fixtures, both reached through production dispatch:
// BlockUntilIdle 820FDCB8, GpuInvalidateMemoryRange 820F4280, the lock core 820F3988 (through VertexBuffer_Lock
// 820F3FA0, IndexBuffer_Lock 820F40F0, and directly with the texture lock's COHER status), InsertFence 820FDD30. The
// per-present A/B (SR_AB_TOGGLE = the three gates, SR_AB_PRESENT=1) chooses the arm: OFF = the SDK body, ON = native;
// both arms run on the segmentless transport (its close is native in both). One in-order emulated CP serves both arms:
// after every GpuRingUpdateWritePointer 820FD218 (its N0 scope hook - where the ringless override takes the close's
// hand-off) the producer side takes what was published and queues it; the consumer applies the queue synchronously
// (P2.7's sync mode: a close's fence completes inside the call) or late from another thread (the fence-wait exit).
// Compared per fixture: device, command buffer (outside the native span; the span: the SDK's bytes against untouched
// memory, the record standing for them), ring, stack window, every GPR and lr, the resource object, the consumer's
// register effects (0x5C8, 0xA2F..0xA31), waits and stores, fb+0, dev+0x2A74; plus the expected PM4 / register values
// computed here from the fixture's inputs. dev+0x2D18 starts at the SDK's empty range 0x00000000FFFFFFFF (end 0, base
// all ones; native/coher-sentinel), so a non-owner merge's both endpoints are visible and checked.
// Bracket cycles (Astra review of this lane): BeginZPass 8210DF38 -> residue producers inside the bracket -> EndZPass
// 8210E1A0, the CP lagging the producer (a pump never runs inside the producer's kick). Under SR_NATIVE_BRACKET=1 the
// bracket is BRACKET's in both arms (its gate is not in the A/B list): the residue spans ride the pinned segments and
// BRACKET_END replays them per bin pass; without it (or when BRACKET declines) the SDK kick's interrupt queues the
// actual pump - the marker interpreter 82110460 and EndSync 82110768 - which publishes the list's records the CP then
// resolves by the deferred lookup. One fixture ends a bracket while an SDK bracket's pump work is outstanding
// (dev+0x2A74 != 0): BRACKET's end declines (kBrPending) and the SDK kick chains behind that pump.
#include "kernel/kpcr.hpp"
#include "guest/guest_harness.hpp"
#include "gfx/xenos_state.hpp"
#include "kernel/export_registry.hpp"

extern "C" void __imp__sub_82110460(PPCContext& __restrict, uint8_t*);  // the marker interpreter (the pump)
extern "C" void __imp__sub_82110768(PPCContext& __restrict, uint8_t*);  // EndSync

namespace sr::native::flush {
namespace rt {
namespace K = sr::kernel;
constexpr uint32_t kDev = 0x70800000u, kCell = 0x70806000u, kObject = 0x70806800u, kStack = 0x70780000u;
constexpr uint32_t kKt = 0x70807000u, kRes = 0x70807400u;                         // a fake KTHREAD, a lock resource
constexpr uint32_t kFb = 0x90807000u, kPc = kFb + 0x80u, kSlots = kFb + 0x100u;  // physical-window VAs
constexpr uint32_t kRing = 0x90808000u, kRingMask = 4095u;                        // 16 KB ring
constexpr uint32_t kBuf = 0x90A00000u, kBufSize = 0x80000u;                       // segments up, allocations down
constexpr uint32_t kAllocCb = 0x82110800u;
constexpr uint32_t kHsio = 0x829AC0ECu;
constexpr uint32_t kThreadId = 0x00ABCD01u;  // KTHREAD+332 of the calling thread (the 82468EF8 read)
constexpr uint8_t kFill = 0xCDu;             // the command buffer's prefill: "untouched" is visible
constexpr uint32_t kS0 = 0x101u;             // the open submission id; fb+0 starts at kS0 - 2

int passed = 0, failed = 0;
void check(bool ok, const std::string& name) {
  std::printf("%s%s\n", ok ? "  ok  " : "FAIL: ", name.c_str());
  ok ? ++passed : ++failed;
}
// A defect that leaves a wait unsatisfiable (a fence never applied) would spin forever: the watchdog fails the fixture
// by name and ends the run (exit 1) instead of hanging.
std::atomic<int64_t> g_watchDeadline{INT64_MAX};
const char* volatile g_watchArm = "";
void watchdog() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (std::chrono::steady_clock::now().time_since_epoch().count() > g_watchDeadline.load()) {
      std::printf("FAIL: %s: both arms completed (no hang)\n", g_watchArm);
      std::printf("[NATIVE_RESIDUE_TEST] %d passed, %d failed\n", passed, failed + 1);
      std::fflush(stdout);
      std::_Exit(1);
    }
  }
}
uint32_t rd(uint32_t va) { return K::readGuestU32BE(va); }
void wr(uint32_t va, uint32_t v) { K::writeGuestBE32(va, v); }
uint8_t* host(uint32_t va) { return K::guestVAToHost(va); }
uint32_t physOf(uint32_t va) { return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu); }  // written here, apart
uint64_t ctr(const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); }

// The allocator callback (object dev+0x3354 +172): bump allocations down from the buffer top; optionally a helper that
// sets no-GPU mode on one request (a guest mode flipped inside the SDK's kick).
uint32_t g_top = 0, g_lateNoGpuBytes = 0;
PPC_FUNC(residueTestAllocate) {
  const uint32_t bytes = rd(ctx.r5.u32), align = ctx.r6.u32 ? ctx.r6.u32 : 4u;
  if (g_lateNoGpuBytes && bytes == g_lateNoGpuBytes) host(kDev + 0x2A39u)[0] |= 2u;
  g_top = (g_top - bytes) & ~(align - 1u);
  ctx.r3.u64 = g_top;
}

// ---- the emulated CP ----
struct QItem {
  uint32_t kind;  // 0 IB (id != 0: a native segment by its hand-off id; 0: literal guest PM4), 1 BS
  uint64_t id;
  uint32_t a, b;
};
std::mutex g_qMu, g_applyMu;
std::vector<QItem> g_q;
uint32_t g_seen = 0;               // ring dwords taken by the producer side
std::atomic<bool> g_sync{true};    // apply at every take (else a helper thread applies)
std::atomic<bool> g_bad{false};
std::atomic<uint64_t> g_gapExec{0};  // literal (non-native) IB bytes executed
sr::gfx::Pm4Engine* g_eng = nullptr;
struct Effects {
  std::vector<std::array<uint32_t, 3>> log;  // what (the CP trace's), addr, value
  bool blocked = false;
};
Effects g_fx;
uint8_t* fxMap(void*, uint32_t p) { return p < 0x20000000u ? host(0x80000000u | p) : nullptr; }
void fxTrace(void*, uint32_t what, uint32_t addr, uint32_t value) { g_fx.log.push_back({what, addr, value}); }
// The guest ISR's answer to an INTERRUPT: its CPU bit cleared in pc+0; a Z-pass kick's callback 82110B00 (SCRATCH_REG4
// at pc+0x10) queues its ctx (REG5 at pc+0x14) for the pump thread, as the ISR's DPC does.
constexpr uint32_t kS = kDev + 0x2A3Cu;  // the replay state (the marker interpreter's / EndSync's)
std::vector<uint32_t> g_pumpQ;
uint32_t g_pumps = 0, g_interrupts = 0;
void fxInterrupt(void*, uint32_t mask) {
  g_fx.log.push_back({9u, mask, 0u});
  g_interrupts++;
  if (rd(kPc + 0x10u) == 0x82110B00u) g_pumpQ.push_back(rd(kPc + 0x14u));
  wr(kPc, rd(kPc) & ~mask);
}
bool fxRunning(void*) {
  g_fx.blocked = true;  // a register / memory wait that would block: the emulated CP never waits on the producer
  return false;
}

PPCContext context();
// The pump thread's work for one kick: the actual marker interpreter 82110460 over the list from the kick's ctx, then
// EndSync 82110768 (82110840's participation: the driver lock held, itself counted), walking on through chained blocks.
bool runPump(uint32_t ctx) {
  wr(kS + 0x58u, ctx);
  uint32_t at = ctx + 4u;
  for (int guard = 0; guard < 64; guard++) {
    PPCContext c = context();
    c.r3.u64 = at;
    c.r4.u64 = kS;
    __imp__sub_82110460(c, K::getGuestBase());
    at = c.r3.u32;
    wr(kS, 1u);
    wr(kS + 0x30u, 1u);
    wr(kS + 0x34u, 1u);
    PPCContext e = context();
    e.r3.u64 = kS;
    e.r4.u64 = at;
    __imp__sub_82110768(e, K::getGuestBase());
    if (!e.r3.u32) {
      g_pumps++;
      return true;
    }
    at = e.r3.u32;  // a chained continuation: walk on
  }
  return false;
}

void take();
thread_local bool t_inApply = false;  // a pump's own ring writes (820FD218) only queue; this loop applies them
// The pump the CP's interrupts queue (null = runPump on the CP's thread). The census suite runs the production driver
// 82110840 on another guest thread instead (tools/difftest/native_census_test.inc).
bool (*g_pumpRunner)(uint32_t ctx) = nullptr;
void apply() {
  std::lock_guard<std::mutex> one(g_applyMu);
  t_inApply = true;
  for (;;) {
    QItem it;
    {
      std::lock_guard<std::mutex> lk(g_qMu);
      if (g_q.empty() && g_pumpQ.empty()) break;
      if (g_q.empty()) {
        it.kind = 2u;  // no ring item left: a queued pump
      } else {
        it = g_q.front();
        g_q.erase(g_q.begin());
      }
    }
    if (!g_eng) {
      g_bad = true;
      g_pumpQ.clear();
      continue;
    }
    if (it.kind == 0u) {
      if (it.id) {
        if (!cpSegment(*g_eng, it.id, it.a, it.b)) g_bad = true;
      } else {
        g_eng->execute(host(0x80000000u | it.a), size_t(it.b) * 4u, 1);
        g_gapExec.fetch_add(4ull * it.b, std::memory_order_relaxed);
      }
    } else if (it.kind == 1u) {
      g_eng->applyBinReg(2u, it.a);
    }
    if (g_eng->nativeRecordFailed) g_bad = true;
    // the pumps this item's interrupt queued run once the IB is done; what they publish joins the queue in order
    while (!g_pumpQ.empty()) {
      const uint32_t ctx = g_pumpQ.front();
      g_pumpQ.erase(g_pumpQ.begin());
      if (!(g_pumpRunner ? g_pumpRunner(ctx) : runPump(ctx))) g_bad = true;
      take();
    }
  }
  t_inApply = false;
}
// The producer side, after every 820FD218 on the publishing thread: the descriptors it wrote, each with the close's
// hand-off taken here (segmentPublishId) exactly where the ringless override takes it.
void take() {
  std::vector<QItem> add;
  const uint32_t w1 = rd(kDev + 0x2A44u);
  while (g_seen < w1) {
    const uint32_t hdr = rd(kRing + 4u * g_seen);
    if (hdr == 0xC0013F00u) {
      const uint32_t p = rd(kRing + 4u * (g_seen + 1u)), n = rd(kRing + 4u * (g_seen + 2u));
      add.push_back({0u, segmentPublishId(p, n), p, n});
      g_seen += 3u;
    } else if (hdr == 0xC0006200u) {
      add.push_back({1u, 0u, rd(kRing + 4u * (g_seen + 1u)), 0u});
      g_seen += 2u;
    } else {
      g_bad = true;
      g_seen = w1;
    }
  }
  {
    std::lock_guard<std::mutex> lk(g_qMu);
    g_q.insert(g_q.end(), add.begin(), add.end());
  }
  if (g_sync && !t_inApply) apply();
}
void ringAfter(void*) {
  if (g_eng) take();
}

// ---- fixtures ----
enum Prod { kIdle = 0, kInval = 1, kLock = 2, kFence = 3 };
struct Case {
  const char* name;
  int prod;
  bool gap = true;          // guest PM4 in the open segment before the call
  bool records = false;     // native records in the open segment before the call
  bool kick = false;        // cursor above the threshold: the SDK's kick check closes first
  bool owner = true;        // dev+0x2A08 = the calling thread
  uint8_t b39 = 0;          // device mode byte 0x2A39 (2 = no-GPU)
  bool tilePred = false;    // 10808 & 2: tile auto-predication
  uint32_t lateNoGpu = 0;   // the allocator flips no-GPU mode on this request (inside the kick)
  // IDLE
  uint32_t S = kS0;         // dev+0x2A1C at the call (0: no fence wait)
  bool pinned = false;      // a Z-pass bracket open (dev+0x3290): the fence wait returns at once
  bool spin = false;        // dev+0x2A74 = 1 at entry; another thread releases it late
  bool lateFence = false;   // the consumer applies late, from another thread
  // LOCK: route 0 VertexBuffer_Lock 820F3FA0, 1 the lock core 820F3988 directly, 2 IndexBuffer_Lock 820F40F0
  int route = 0;
  uint32_t status = 0x03000000u, flags = 0u, fence = 0u, resType = 1u, r5 = 0u, r7 = 0u, offset = 0x40u;
  // INVAL / LOCK range (the lock: res+24 = addr's page base, the call locks [addr, addr + size))
  uint32_t addr = 0xA0A00040u, size = 0x100u;
  // expectation for the native arm: 1 taken (a record span), 2 taken as the merge (no PM4), 3 late (the SDK's bytes),
  // 0 declined at the top for `reason`
  int expect = 1;
  int reason = -1;
};

struct Snap {
  std::vector<uint8_t> device, buffer, ring, fb, res;
  bool operator==(const Snap& o) const {
    return device == o.device && buffer == o.buffer && ring == o.ring && fb == o.fb && res == o.res;
  }
};
struct Outcome {
  Snap after;                      // after the call and the closing InsertFence's consumption
  std::vector<uint8_t> stack;      // the stack window right after the call
  std::array<uint64_t, 33> regs{};  // r0..r31, lr right after the call
  uint32_t r3 = 0, fb0AtReturn = 0, a74AtReturn = 0;
  bool releasedFirst = true, consumed = false, blocked = false;
  std::vector<std::array<uint32_t, 3>> log;
  uint32_t reg5C8 = 0, regA2F = 0, regA30 = 0, regA31 = 0;
  uint64_t waits = 0, gapBytes = 0;
  uint32_t spanLo = 0, spanHi = 0;
  uint64_t range = 0;  // dev+0x2D18 right after the call (before any close takes it): {end, base}
};

PPCRegister PPCContext::* const kGpr[32] = {
    &PPCContext::r0,  &PPCContext::r1,  &PPCContext::r2,  &PPCContext::r3,  &PPCContext::r4,  &PPCContext::r5,
    &PPCContext::r6,  &PPCContext::r7,  &PPCContext::r8,  &PPCContext::r9,  &PPCContext::r10, &PPCContext::r11,
    &PPCContext::r12, &PPCContext::r13, &PPCContext::r14, &PPCContext::r15, &PPCContext::r16, &PPCContext::r17,
    &PPCContext::r18, &PPCContext::r19, &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23,
    &PPCContext::r24, &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29,
    &PPCContext::r30, &PPCContext::r31};
thread_local uint32_t t_stackTop = kStack;  // the calling guest thread's stack (another thread: its own)
PPCContext context() {
  PPCContext c{};
  for (int i = 0; i < 32; i++) (c.*kGpr[i]).u64 = 0x1111111100000000ull | uint64_t(0x100u + i);
  c.r1.u64 = t_stackTop;
  c.r13.u64 = K::kpcrCurrent();
  c.lr = 0x82AB0000u;
  c.f14.u64 = 0x1234567812345678ull;
  return c;
}
std::array<uint64_t, 33> regsOf(const PPCContext& c) {
  std::array<uint64_t, 33> r{};
  for (int i = 0; i < 32; i++) r[size_t(i)] = (c.*kGpr[i]).u64;
  r[32] = c.lr;
  return r;
}
void call(uint32_t va, PPCContext& c) { PPC_LOOKUP_FUNC(K::getGuestBase(), va)(c, K::getGuestBase()); }
void closeNow() {
  PPCContext c = context();
  c.r3.u64 = kDev;
  call(0x820FD8C0u, c);
}
// Guest PM4 in the open segment (gap bytes): a register write and an EVENT_WRITE_SHD store.
void gapContent(uint32_t k) {
  const uint32_t at = rd(kDev + 0x28u) + 4u;
  const uint32_t w[] = {0x00002000u, 0xA0000000u | k, 0xC0025800u, 3u, physOf(kSlots + 4u * (k & 63u)) | 2u, 0xB0000000u | k};
  for (uint32_t i = 0; i < sizeof(w) / 4u; i++) wr(at + 4u * i, w[i]);
  wr(kDev + 0x28u, at + uint32_t(sizeof(w)) - 4u);
}
// A native record span (the producer API): an EVENT_WRITE_SHD store.
bool recordContent(uint32_t k) {
  PPCContext c = context();
  Entry e(c, K::getGuestBase(), kDev);
  if (!e.open()) return false;
  const uint32_t body[3] = {3u, physOf(kSlots + 0x400u + 4u * (k & 63u)) | 2u, 0xD0000000u | k};
  if (!e.eventShd(body[0], body[1], body[2])) {
    e.abandon();
    return false;
  }
  e.close();
  return true;
}

void prepare(const Case& cs) {
  std::memset(host(kDev), 0, 0x5000u);
  std::memset(host(kCell), 0, 0x800u);
  std::memset(host(kObject), 0, 0x800u);
  std::memset(host(kKt), 0, 0x400u);
  std::memset(host(kRes), 0, 0x100u);
  std::memset(host(kFb), 0, 0x1000u);
  std::memset(host(kRing), 0, (kRingMask + 1u) * 4u);
  std::memset(host(kBuf), kFill, kBufSize);
  std::memset(host(kStack - 0x2000u), 0xA5, 0x2000u);
  wr(0x82000948u, kCell);
  wr(kCell, kDev);
  wr(kDev + 0x2A10u, kFb);
  wr(kDev + 0x2A14u, kPc);
  wr(kDev + 0x34A4u, kRing);
  wr(kDev + 0x34A8u, kRingMask);
  wr(kDev + 0x34ACu, kBuf);
  wr(kDev + 0x34B0u, kBuf + kBufSize);
  wr(kDev + 0x34B4u, 0x10000u);  // the segment size BeginNewSegment requests from the allocator
  const uint32_t seg = kBuf + 0x100u;
  wr(kDev + 0x34C4u, seg);
  wr(kDev + 0x28u, seg - 4u);
  wr(kDev + 0x2Cu, kBuf + 0x60000u);
  wr(kDev + 0x30u, kBuf + 0x5F000u);
  wr(kDev + 0x2A1Cu, cs.S);
  wr(kFb, kS0 - 2u);  // the last completed submission
  wr(kDev + 0x3298u, seg - 4u);
  wr(kDev + 0x329Cu, 1u);
  wr(kDev + 0x34BCu, 1u);
  wr(kDev + 0x3354u, kObject);
  wr(kObject + 164u, 0x11223344u);
  wr(kObject + 172u, kAllocCb);
  wr(kDev + 0x34CCu, 0x7FFFFFFFu);
  wr(kDev + 0x2D18u, 0u);           // the SDK's empty range 0x00000000FFFFFFFF: end 0, base all ones (D3DDeviceResetState)
  wr(kDev + 0x2D1Cu, 0xFFFFFFFFu);
  // the Z-pass bracket's device fields (as the bracket suite's fixture): the pump CPU, the window scissor, EndZPass's
  // SetScissorRect source, the replay state's cache word
  wr(kDev + 0x2BB8u, 2u);
  wr(kDev + 0x2844u, 0u);
  wr(kDev + 0x2848u, (480u << 15) | 640u);
  wr(kDev + 0x3098u, 320u);
  wr(kDev + 0x309Cu, 240u);
  wr(kS + 0x16Cu, 0x7FFFFFFFu);
  wr(kDev + 0x2A08u, cs.owner ? kThreadId : kThreadId + 1u);
  wr(kKt + 332u, kThreadId);
  host(kDev + 0x2A39u)[0] = cs.b39;
  if (cs.tilePred) host(kDev + 10808u)[0] |= 2u;
  if (cs.pinned) {
    wr(kDev + 0x3290u, seg - 4u);
    wr(kDev + 0x3294u, 2u);
  }
  if (cs.spin) wr(kDev + 0x2A74u, 1u);
  wr(kHsio, 0u);
  // the lock resource: type word, fence, base address (page) and size
  wr(kRes + 0u, cs.resType);
  wr(kRes + 8u, cs.fence);
  wr(kRes + 12u, cs.fence);
  wr(kRes + 24u, cs.addr & ~0xFFFu);
  wr(kRes + 28u, 0x2000u);
  {  // each arm starts from an empty transport: the fixtures reuse the same (phys, dwords) positions in every case
    std::lock_guard<std::mutex> lk(g_segMu);
    g_segFeet.clear();
    g_segById.clear();
    g_segDeferred.clear();
    auditClearLocked();  // step 6 I12: the audit, both arms (the ring / the map)
    g_brOpen.reset();
    g_segBracket = 0;
  }
  t_segPublishing.reset();
  g_segWasPinned = false;
  g_head = g_reclaim = g_tailSeen = 0u;
  g_cpTail.store(0u);
  g_top = kBuf + kBufSize;
  g_lateNoGpuBytes = 0;
  g_seen = 0;
  {
    std::lock_guard<std::mutex> lk(g_qMu);
    g_q.clear();
  }
  g_bad = false;
  g_fx = Effects{};
  g_rsTestSpanLo = g_rsTestSpanHi = 0;
  g_rsTestSpans.clear();
  g_pumpQ.clear();
  g_pumps = g_interrupts = 0;
}

Snap snapNow() {
  Snap s;
  s.device.assign(host(kDev), host(kDev) + 0x5000u);
  s.buffer.assign(host(kBuf), host(kBuf) + kBufSize);
  s.ring.assign(host(kRing), host(kRing) + (kRingMask + 1u) * 4u);
  s.fb.assign(host(kFb), host(kFb) + 0x1000u);
  s.res.assign(host(kRes), host(kRes) + 0x100u);
  return s;
}

void callProducer(const Case& cs, PPCContext& c) {
  c.r3.u64 = kDev;
  switch (cs.prod) {
    case kIdle:
      call(0x820FDCB8u, c);
      break;
    case kInval:
      c.r3.u64 = 0x5555u;  // unused by the body (the device comes from VdGlobalDevice)
      c.r4.u64 = cs.addr;
      c.r5.u64 = cs.size;
      call(0x820F4280u, c);
      break;
    case kLock:
      if (cs.route == 1) {  // the lock core directly, the texture lock's register convention
        c.r1.u64 = kStack - 0x100u;  // a caller frame inside the compared window, holding the stack argument
        c.r3.u64 = kRes;
        c.r4.u64 = 14u;
        c.r5.u64 = cs.r5;
        c.r6.u64 = cs.addr & ~0xFFFu;
        c.r7.u64 = cs.r7;
        c.r8.u64 = cs.addr;
        c.r9.u64 = cs.size;
        c.r10.u64 = cs.flags;
        wr(c.r1.u32 + 84u, cs.status);  // the caller's stack argument (sp+0x54)
        call(0x820F3988u, c);
      } else {
        c.r3.u64 = kRes;
        c.r4.u64 = cs.offset;
        c.r5.u64 = cs.size;
        c.r6.u64 = cs.flags;
        call(cs.route == 2 ? 0x820F40F0u : 0x820F3FA0u, c);
      }
      break;
    default:
      call(0x820FDD30u, c);  // InsertFence
      break;
  }
}

Outcome runArm(const Case& cs, bool nativeArm, int abBit) {
  for (int i = 0; i < 2 && sr::abt::on(abBit) != nativeArm; i++) sr::abt::onGuestSwap();
  prepare(cs);
  Outcome o;
  auto eng = std::make_unique<sr::gfx::Pm4Engine>();
  eng->submitDraws = false;
  eng->cpMode = true;
  eng->physToHost = fxMap;
  eng->cp.user = &g_fx;
  eng->cp.interrupt = fxInterrupt;
  eng->cp.running = fxRunning;
  eng->cp.trace = fxTrace;
  eng->binMask = ~0ull;
  eng->binSelect = ~0ull;
  eng->writeReg(0x1DCu, 0x00020033u);  // SCRATCH_UMSK as the device-create block sets it
  eng->writeReg(0x1DDu, physOf(kPc));  // SCRATCH_ADDR
  auto& xr = sr::gfx::getXenosRegisters();
  for (uint32_t r : {0x5C8u, 0xA2Fu, 0xA30u, 0xA31u}) xr.write(r, 0xDEAD0000u | r);
  g_eng = eng.get();
  g_sync = !cs.lateFence;
  const uint64_t gap0 = ctr(g_gapExec);
  const uint64_t gapBytes0 = g_segGapBytes.get();
  // the open segment's content
  if (cs.gap) gapContent(0u);
  if (cs.records && !recordContent(0u)) check(false, std::string(cs.name) + ": native record content admitted");
  if (cs.gap && cs.records) gapContent(1u);
  if (cs.kick) wr(kDev + 0x30u, rd(kDev + 0x28u) - 4u);  // cursor > threshold
  g_lateNoGpuBytes = cs.lateNoGpu;
  // the helper thread: releases dev+0x2A74 (spin) or applies the queue (lateFence: until the fence it waits on landed)
  // after the producer is in its wait
  std::atomic<bool> released{false};
  std::thread helper;
  uint8_t* const guestBase = K::getGuestBase();
  if (cs.spin || cs.lateFence)
    helper = std::thread([&, guestBase] {
      K::setGuestMemory(guestBase, 0x100000000ull);  // a side thread's guest-memory TLS (CLAUDE.md)
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      released = true;
      if (cs.spin) wr(kDev + 0x2A74u, 0u);
      if (cs.lateFence) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
          apply();
          if (rd(kFb) == cs.S) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < until);
      }
    });
  g_watchArm = cs.name;
  g_watchDeadline = std::chrono::steady_clock::now().time_since_epoch().count() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(20)).count();
  PPCContext c = context();
  callProducer(cs, c);
  o.range = (uint64_t(rd(kDev + 0x2D18u)) << 32) | rd(kDev + 0x2D1Cu);
  o.releasedFirst = !(cs.spin || cs.lateFence) || released.load();
  o.fb0AtReturn = rd(kFb);
  o.a74AtReturn = rd(kDev + 0x2A74u);
  o.regs = regsOf(c);
  o.r3 = c.r3.u32;
  o.stack.assign(host(kStack - 0x2000u), host(kStack - 0x2000u) + 0x2000u);
  if (helper.joinable()) helper.join();
  g_lateNoGpuBytes = 0;
  o.spanLo = g_rsTestSpanLo;
  o.spanHi = g_rsTestSpanHi;
  // close the open segment so the consumer applies everything (a pinned fixture leaves the bracket first)
  g_sync = true;
  if (cs.pinned) {
    wr(kDev + 0x3290u, 0u);
    wr(kDev + 0x3294u, 0u);
  }
  closeNow();
  apply();
  o.after = snapNow();
  o.consumed = !g_bad && !eng->nativeRecordFailed;
  o.blocked = g_fx.blocked;
  o.log = g_fx.log;
  o.reg5C8 = xr.read(0x5C8u);
  o.regA2F = xr.read(0xA2Fu);
  o.regA30 = xr.read(0xA30u);
  o.regA31 = xr.read(0xA31u);
  o.waits = eng->cpWaits;
  o.gapBytes = (g_segGapBytes.get() - gapBytes0) + (ctr(g_gapExec) - gap0);
  g_eng = nullptr;
  g_watchDeadline = INT64_MAX;
  return o;
}

// The expected PM4 of the converted block, from the fixture's inputs (independent of the ports).
std::vector<uint32_t> expectedDwords(const Case& cs) {
  if (cs.prod == kIdle) return {0x000005C8u, 0x00020000u};
  const uint32_t p = physOf(cs.prod == kLock && cs.route == 0 ? (cs.addr & ~0xFFFu) + cs.offset : cs.addr);
  const uint32_t lo = p & ~0xFFFu, hi = (p + cs.size + 0xFFFu) & ~0xFFFu;
  const std::vector<uint32_t> coher = {0x00000A31u, cs.prod == kInval ? 0x03000000u : cs.status, 0x00010A2Fu, hi - lo,
                                       lo, 0xC0043C00u, 3u, 0x0A31u, 0u, 0x80000000u, 8u};
  if (cs.prod == kLock) return coher;
  std::vector<uint32_t> v = {0x000005C8u, 0x00020000u};
  v.insert(v.end(), coher.begin(), coher.end());
  return v;
}

int g_diag = 0;
void diffBytes(const std::string& what, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t base) {
  if (g_diag >= 24) return;
  size_t i = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
  if (i == a.size() && a.size() == b.size()) return;
  g_diag++;
  char line[256];
  int at = std::snprintf(line, sizeof(line), "[RESIDUE_DIAG] %s first diff at %08X:", what.c_str(), base + uint32_t(i));
  for (size_t k = i & ~size_t(15); k < (i & ~size_t(15)) + 16u && k < a.size() && k < b.size(); k += 4)
    at += std::snprintf(line + at, sizeof(line) - size_t(at), " %02X%02X%02X%02X/%02X%02X%02X%02X", a[k], a[k + 1], a[k + 2],
                        a[k + 3], b[k], b[k + 1], b[k + 2], b[k + 3]);
  std::printf("%s\n", line);
}

struct Counters {
  uint64_t taken, late, merge, decl[kRsDeclN], pk, litPk, host, hostApplied;
};
Counters counters(int p) {
  Counters k{};
  k.taken = ctr(g_rsTaken[p]);
  k.late = ctr(g_rsLate[p]);
  k.merge = ctr(g_rsMerge[p]);
  for (int r = 0; r < kRsDeclN; r++) k.decl[r] = ctr(g_rsDecl[p][r]);
  k.pk = ctr(g_rsPk);
  k.litPk = ctr(g_rsLitPk);
  k.host = ctr(g_rsHost);
  k.hostApplied = ctr(g_rsHostApplied);
  return k;
}

void runCase(const Case& cs, bool gateOn, bool segOff, int abBit) {
  const std::string n = cs.name;
  const int p = cs.prod == kFence ? kRsIdle : cs.prod;
  const uint32_t pk = cs.prod == kIdle ? 1u : cs.prod == kInval ? 4u : cs.prod == kLock ? 3u : 0u;
  // ---- the SDK arm (the A/B OFF window) ----
  const Counters s0 = counters(p);
  Outcome sdk = runArm(cs, false, abBit);
  const Counters s1 = counters(p);
  // ---- the native arm (the A/B ON window) ----
  Outcome nat = runArm(cs, true, abBit);
  const Counters n1 = counters(p);

  check(true, n + ": both arms completed (no hang)");
  check(sdk.consumed && nat.consumed && !sdk.blocked && !nat.blocked, n + ": both arms consumed in order, no blocked wait");
  // the SDK arm really ran the SDK body: no residue counter but its A/B-window decline moved
  if (cs.prod != kFence) {
    const bool sdkClean = s1.taken == s0.taken && s1.late == s0.late && s1.merge == s0.merge && s1.pk == s0.pk;
    if (!gateOn || segOff || cs.expect == 0) {
      check(sdkClean, n + ": SDK arm: no residue record");
    } else {
      const bool noPkDecl = cs.prod == kLock && (cs.flags & 0x12u);
      const int sdkReason = noPkDecl ? kRsNoPk : kRsAb;
      check(sdkClean && s1.decl[sdkReason] == s0.decl[sdkReason] + 1u,
            n + ": SDK arm: the guest body (declined in the A/B OFF window)");
    }
  }
  // ---- the native arm's route ----
  const bool expectNative = gateOn && !segOff && cs.prod != kFence;
  if (cs.prod != kFence) {
    if (!expectNative) {
      const bool clean = n1.taken == s1.taken && n1.late == s1.late && n1.merge == s1.merge && n1.pk == s1.pk;
      if (segOff)
        check(clean && (cs.prod == kLock && (cs.flags & 0x12u) ? n1.decl[kRsNoPk] == s1.decl[kRsNoPk] + 1u
                                                                 : n1.decl[kRsSeg] == s1.decl[kRsSeg] + 1u),
              n + ": segmentless off: declined at the top, the guest body whole");
      else
        check(clean, n + ": gate off: the native arm is the SDK body");
    } else if (cs.expect == 0) {
      check(n1.taken == s1.taken && n1.late == s1.late && n1.decl[cs.reason] == s1.decl[cs.reason] + 1u,
            n + ": declined at the top (reason " + std::to_string(cs.reason) + "), the guest body whole");
    } else if (cs.expect == 1) {
      check(n1.taken == s1.taken + 1u && n1.late == s1.late && n1.pk == s1.pk + pk && n1.litPk == s1.litPk,
            n + ": taken natively: one record span standing for the SDK's " + std::to_string(pk) + " packet(s)");
    } else if (cs.expect == 2) {
      check(n1.merge == s1.merge + 1u && n1.taken == s1.taken && n1.pk == s1.pk,
            n + ": another thread: the SDK's dev+0x2D18 merge ported, no record");
    } else {
      check(n1.late == s1.late + 1u && n1.litPk == s1.litPk + pk && n1.pk == s1.pk,
            n + ": refused late at the record point: the SDK's own dwords written");
    }
    if (expectNative && cs.expect == 1 && cs.prod == kLock && cs.status != 0x03000000u && cs.status != 0x01000000u)
      check(n1.host == s1.host + 1u && (g_cpPm4 || g_pm4Out || n1.hostApplied == s1.hostApplied + 1u),
            n + ": the status kRkInvalidate cannot carry travels as kRkCoherHost (kind 22), applied once");
  }
  // ---- byte / register / effect equality ----
  const bool span = expectNative && cs.expect == 1;
  std::vector<uint8_t> sdkBuf = sdk.after.buffer, natBuf = nat.after.buffer;
  if (span) {
    const std::vector<uint32_t> want = expectedDwords(cs);
    const uint32_t lo = nat.spanLo, hi = nat.spanHi;
    bool okSpan = lo >= kBuf && hi > lo && hi <= kBuf + kBufSize && hi - lo == 4u * want.size();
    for (uint32_t k = 0; okSpan && k < want.size(); k++) {
      const uint8_t* s = sdkBuf.data() + (lo - kBuf) + 4u * k;
      const uint32_t v = uint32_t(s[0]) << 24 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 8 | s[3];
      okSpan = v == want[k];
    }
    check(okSpan, n + ": the SDK wrote exactly the expected PM4 where the native span lies");
    bool untouched = okSpan;
    for (uint32_t a = lo; untouched && a < hi; a++) {
      const uint8_t b = natBuf[a - kBuf];
      untouched = g_pm4Out ? b == sdkBuf[a - kBuf] : b == kFill;
    }
    check(untouched, n + (g_pm4Out ? ": the native span is the SDK's bytes (PM4 output)"
                                   : ": the native span left memory untouched (the record stands for it)"));
    if (okSpan)
      for (uint32_t a = lo; a < hi; a++) natBuf[a - kBuf] = sdkBuf[a - kBuf];
  }
  const bool timing = cs.lateFence;  // the SDK's wait loop: rdtsc timing words in the device and the stack
  std::vector<uint8_t> sdkDev = sdk.after.device, natDev = nat.after.device;
  if (timing)
    for (uint32_t off = 0x4E38u; off < 0x4E48u; off++) sdkDev[off] = natDev[off] = 0;
  diffBytes(n + " device", sdkDev, natDev, kDev);
  diffBytes(n + " buffer", sdkBuf, natBuf, kBuf);
  diffBytes(n + " ring", sdk.after.ring, nat.after.ring, kRing);
  check(natDev == sdkDev && nat.after.fb == sdk.after.fb && nat.after.res == sdk.after.res,
        n + ": device, fence block and resource equal the actual SDK" +
            (timing ? std::string(" (the wait loop's timing words apart)") : std::string()));
  check(natBuf == sdkBuf, n + ": command buffer equals the actual SDK" + std::string(span ? " outside the span" : ""));
  check(nat.after.ring == sdk.after.ring, n + ": ring descriptors equal the actual SDK");
  if (!timing) {
    diffBytes(n + " stack", sdk.stack, nat.stack, kStack - 0x2000u);
    check(nat.stack == sdk.stack, n + ": stack window equals the actual SDK");
    bool regs = nat.regs == sdk.regs;
    if (!regs && g_diag < 24)
      for (size_t i = 0; i < 33; i++)
        if (nat.regs[i] != sdk.regs[i])
          std::printf("[RESIDUE_DIAG] %s reg %s%zu sdk %016llX native %016llX\n", n.c_str(), i == 32 ? "lr" : "r",
                      i == 32 ? 0 : i, (unsigned long long)sdk.regs[i], (unsigned long long)nat.regs[i]);
    check(regs, n + ": every GPR and lr equal the actual SDK");
  } else {
    bool regs = nat.r3 == sdk.r3 && nat.regs[32] == sdk.regs[32] && nat.regs[1] == sdk.regs[1];
    for (size_t i = 13; i < 32; i++) regs = regs && nat.regs[i] == sdk.regs[i];
    check(regs, n + ": r1, r3, r13..r31 and lr equal the actual SDK (volatile timing registers apart)");
  }
  check(nat.log == sdk.log, n + ": consumer stores equal the actual SDK's");
  check(nat.reg5C8 == sdk.reg5C8 && nat.regA2F == sdk.regA2F && nat.regA30 == sdk.regA30 && nat.regA31 == sdk.regA31 &&
            nat.waits == sdk.waits,
        n + ": consumer registers (WAIT_UNTIL, COHER) and waits equal the actual SDK's");
  // the consumer's registers against the values computed here (an oracle independent of both arms); a no-GPU close
  // (the guest's own, writing the fences itself) is not this consumer's to apply
  const bool gpu = !cs.b39 && !cs.lateNoGpu;
  if (gpu && (cs.prod == kIdle || (cs.prod == kInval && cs.owner)))
    check(nat.reg5C8 == 0x00020000u, n + ": WAIT_UNTIL applied: 0x5C8 = 0x00020000");
  if (gpu && (cs.prod == kInval || (cs.prod == kLock && !(cs.flags & 0x12u))) && cs.owner) {
    const std::vector<uint32_t> want = expectedDwords(cs);
    const size_t o = cs.prod == kInval ? 2u : 0u;
    check(nat.regA31 == want[o + 1] && nat.regA2F == want[o + 3] && nat.regA30 == want[o + 4] && nat.waits >= 1u,
          n + ": COHER applied: status, size, base as the SDK's packets set them, its wait run");
  }
  // dev+0x2D18 right after the call: another thread's INVAL / LOCK merges its range into the SDK's empty value (base =
  // min, end = max: both endpoints move); every other call leaves it empty. Then the next close's INVALIDATE.
  const bool merges = (cs.prod == kInval || (cs.prod == kLock && !(cs.flags & 0x12u))) && !cs.owner;
  if (merges) {
    const uint32_t p = physOf(cs.prod == kLock && cs.route == 0 ? (cs.addr & ~0xFFFu) + cs.offset : cs.addr);
    const uint64_t want = (uint64_t(p + cs.size) << 32) | p;
    check(sdk.range == want && nat.range == want,
          n + ": another thread: dev+0x2D18 = the merged range {end, base} (both endpoints, from the SDK's empty value)");
    if (gpu) {
      const uint32_t lo = p & ~0xFFFu, size = ((p + cs.size + 0xFFFu) & ~0xFFFu) - lo;
      check(sdk.regA30 == lo && sdk.regA2F == size && nat.regA30 == lo && nat.regA2F == size,
            n + ": the next close's INVALIDATE applied that range (page base, page-rounded size)");
    }
  } else if (cs.prod != kFence) {
    check(sdk.range == 0x00000000FFFFFFFFull && nat.range == 0x00000000FFFFFFFFull,
          n + ": dev+0x2D18 left at the SDK's empty value 0x00000000FFFFFFFF");
  }
  // the drain rule: fb+0 and dev+0x2A74 at the call's return; the spin's exits
  check(nat.fb0AtReturn == sdk.fb0AtReturn && nat.a74AtReturn == sdk.a74AtReturn,
        n + ": fb+0 and dev+0x2A74 at return equal the actual SDK's");
  if (gpu && cs.prod == kIdle && cs.S && !cs.pinned) {
    const uint32_t s = cs.S + (cs.kick ? 2u : 0u);  // the kick closed S first: the wait is on the next id
    check(nat.fb0AtReturn == s && sdk.fb0AtReturn == s, n + ": returned with fb+0 at S (the fence wait passed)");
  }
  if (cs.prod == kIdle && cs.pinned)
    check(nat.fb0AtReturn == kS0 - 2u && sdk.fb0AtReturn == kS0 - 2u,
          n + ": pinned: the fence wait returned at once, nothing closed inside the call");
  if (cs.spin || cs.lateFence)
    check(sdk.releasedFirst && nat.releasedFirst,
          n + (cs.spin ? ": returned only after dev+0x2A74 was released (the SDK's spin)"
                       : ": returned only after the consumer applied the fence (the SDK's wait)"));
  check(nat.a74AtReturn == 0u && rd(kDev + 0x2A74u) == 0u, n + ": dev+0x2A74 drained, never moved by the residue");
  if (cs.prod == kFence) {
    check(sdk.r3 == cs.S && nat.r3 == cs.S && rd(kDev + 0x2A2Cu) == cs.S && nat.fb0AtReturn == cs.S,
          n + ": InsertFence returned S, stored dev+0x2A2C = S, its close's FENCE applied");
    if (!g_pm4Out && !segOff && !cs.gap)
      check(nat.gapBytes == 0u && sdk.gapBytes == 0u, n + ": no guest PM4 reached the consumer (records only)");
  }
}

// ---- bracket cycles: BeginZPass -> residue producers inside the bracket -> EndZPass ----
struct BCase {
  const char* name;
  bool records = false;  // native record content in the bracket's segments besides the guest PM4
  bool pending = false;  // an SDK bracket's pump work outstanding at this bracket's end (dev+0x2A74 != 0)
};
struct BOutcome {
  Snap after;
  std::vector<std::array<uint32_t, 3>> log, spans;
  uint32_t reg5C8 = 0, regA2F = 0, regA30 = 0, regA31 = 0, a74AtEnd = 0, a74After = 0, pumps = 0, interrupts = 0;
  uint64_t waits = 0, coher = 0;
  bool consumed = false, blocked = false;
};
// The producers inside: the texture lock (status 0x02000000: kind 22), an owner GpuInvalidateMemoryRange, BlockUntilIdle.
Case bracketLock() {
  Case c{};
  c.name = "lock";
  c.prod = kLock;
  c.route = 1;
  c.status = 0x02000000u;
  c.addr = 0xA0D01234u;
  c.size = 0x3000u;
  return c;
}
Case bracketInval() {
  Case c{};
  c.name = "inval";
  c.prod = kInval;
  c.addr = 0xA0B00123u;
  c.size = 0x2345u;
  return c;
}
Case bracketIdle() {
  Case c{};
  c.name = "idle";
  c.prod = kIdle;
  return c;
}
void zBegin() {  // BeginZPass(dev, 0): 82110B30 (BRACKET's begin override), 0x84, the scissor tile rects
  PPCContext c = context();
  c.r3.u64 = kDev;
  c.r4.u64 = 0u;
  call(0x8210DF38u, c);
}
void zEnd() {  // EndZPass: flush, 0x85, InvokeRenderPass: flush, 0x86, 82110C78 (BRACKET's end override or the SDK kick)
  PPCContext c = context();
  c.r3.u64 = kDev;
  call(0x8210E1A0u, c);
}
void produce(const Case& cs) {
  PPCContext c = context();
  callProducer(cs, c);
}
// The CP and every pump its interrupts queue, to quiescence (the producer is not running).
void drain() {
  for (int guard = 0; guard < 64; guard++) {
    take();
    apply();
    std::lock_guard<std::mutex> lk(g_qMu);
    if (g_q.empty() && g_pumpQ.empty() && g_seen == rd(kDev + 0x2A44u)) return;
  }
  g_bad = true;
}
void observeNoop(uint64_t, uint32_t, uint32_t, bool) {}  // BRACKET's begin needs a consumer (the emulated CP is one)
uint32_t evNoop(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return 0u; }  // EndSync's event imports

BOutcome runBracketArm(const BCase& bc, bool nativeArm, int abBit) {
  for (int i = 0; i < 2 && sr::abt::on(abBit) != nativeArm; i++) sr::abt::onGuestSwap();
  Case base{};
  base.name = bc.name;
  base.prod = kIdle;
  base.gap = false;
  prepare(base);
  BOutcome o;
  auto eng = std::make_unique<sr::gfx::Pm4Engine>();
  eng->submitDraws = false;
  eng->cpMode = true;
  eng->physToHost = fxMap;
  eng->cp.user = &g_fx;
  eng->cp.interrupt = fxInterrupt;
  eng->cp.running = fxRunning;
  eng->cp.trace = fxTrace;
  eng->binMask = ~0ull;
  eng->binSelect = ~0ull;
  eng->writeReg(0x1DCu, 0x00020033u);
  eng->writeReg(0x1DDu, physOf(kPc));
  auto& xr = sr::gfx::getXenosRegisters();
  for (uint32_t r : {0x5C8u, 0xA2Fu, 0xA30u, 0xA31u}) xr.write(r, 0xDEAD0000u | r);
  g_eng = eng.get();
  g_sync = false;  // the CP lags the producer: it runs between the calls, never inside a kick (a pump could not)
  g_watchArm = bc.name;
  g_watchDeadline = std::chrono::steady_clock::now().time_since_epoch().count() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(20)).count();
  if (bc.pending) {  // an SDK bracket first: its begin declines under the scoped SDK oracle (as in an A/B OFF window)
    {
      PresentGateOracle sdkBegin;
      zBegin();
    }
    gapContent(10u);
    closeNow();
    gapContent(11u);
    zEnd();  // the SDK kick: dev+0x2A74 1, its pump not run yet (the CP has not reached the kick)
  }
  zBegin();
  gapContent(0u);
  if (bc.records && !recordContent(0u)) check(false, std::string(bc.name) + ": native record content admitted");
  produce(bracketLock());
  closeNow();  // a bracket close (pinned)
  gapContent(1u);
  produce(bracketInval());
  if (bc.records && !recordContent(1u)) check(false, std::string(bc.name) + ": native record content admitted");
  // pending: dev+0x2A74 stays 1 until the pump the lagging CP has not run - BlockUntilIdle would spin on this thread
  if (!bc.pending) produce(bracketIdle());
  gapContent(2u);  // EndZPass's own flush closes it
  o.a74AtEnd = rd(kDev + 0x2A74u);
  zEnd();
  drain();
  closeNow();
  drain();
  o.a74After = rd(kDev + 0x2A74u);
  o.after = snapNow();
  o.consumed = !g_bad && !eng->nativeRecordFailed;
  o.blocked = g_fx.blocked;
  o.log = g_fx.log;
  o.reg5C8 = xr.read(0x5C8u);
  o.regA2F = xr.read(0xA2Fu);
  o.regA30 = xr.read(0xA30u);
  o.regA31 = xr.read(0xA31u);
  o.waits = eng->cpWaits;
  o.coher = eng->cpCoher;
  o.spans = g_rsTestSpans;
  o.pumps = g_pumps;
  o.interrupts = g_interrupts;
  g_eng = nullptr;
  g_sync = true;
  g_watchDeadline = INT64_MAX;
  return o;
}

struct BrCounters {
  uint64_t taken[kRsN], late[kRsN], ab[kRsN], seg[kRsN], host, hostApplied, end, consumed, segs, pending, begin;
};
BrCounters brCounters() {
  BrCounters k{};
  for (int p = 0; p < kRsN; p++) {
    k.taken[p] = ctr(g_rsTaken[p]);
    k.late[p] = ctr(g_rsLate[p]);
    k.ab[p] = ctr(g_rsDecl[p][kRsAb]);
    k.seg[p] = ctr(g_rsDecl[p][kRsSeg]);
  }
  k.host = ctr(g_rsHost);
  k.hostApplied = ctr(g_rsHostApplied);
  k.end = g_brEnd.get();
  k.consumed = g_brConsumed.get();
  k.segs = g_brSegs.get();
  k.pending = g_brDecl[kBrPending].get();
  k.begin = g_brBegin.get();
  return k;
}

void runBracketCase(const BCase& bc, bool gateOn, bool segOff, int abBit) {
  const std::string n = std::string("bracket ") + bc.name;
  const bool brNative = bracketGate() && !segOff && !g_pm4Out;  // BRACKET's begin admits (its gate, segmentless, no PM4 output)
  const bool rsNative = gateOn && !segOff;
  const int producers[3] = {kRsLock, kRsInval, kRsIdle};
  const int nProd = bc.pending ? 2 : 3;
  const BrCounters c0 = brCounters();
  BOutcome sdk = runBracketArm(bc, false, abBit);
  const BrCounters c1 = brCounters();
  BOutcome nat = runBracketArm(bc, true, abBit);
  const BrCounters c2 = brCounters();
  check(true, n + ": both arms completed (no hang)");
  check(sdk.consumed && nat.consumed && !sdk.blocked && !nat.blocked && sdk.a74After == 0u && nat.a74After == 0u,
        n + ": both arms consumed in order to quiescence (the CP and every pump), no blocked wait, dev+0x2A74 drained");
  // ---- the residue's routes ----
  bool sdkClean = true, sdkAb = true, natRoute = true;
  for (int k = 0; k < nProd; k++) {
    const int p = producers[k];
    sdkClean = sdkClean && c1.taken[p] == c0.taken[p] && c1.late[p] == c0.late[p];
    sdkAb = sdkAb && c1.ab[p] == c0.ab[p] + 1u;
    if (rsNative)
      natRoute = natRoute && c2.taken[p] == c1.taken[p] + 1u && c2.late[p] == c1.late[p];
    else if (segOff)
      natRoute = natRoute && c2.taken[p] == c1.taken[p] && c2.seg[p] == c1.seg[p] + 1u;
    else
      natRoute = natRoute && c2.taken[p] == c1.taken[p] && c2.late[p] == c1.late[p];
  }
  if (!gateOn || segOff)
    check(sdkClean, n + ": SDK arm: no residue record");
  else
    check(sdkClean && sdkAb, n + ": SDK arm: every residue call the guest body (declined in the A/B OFF window)");
  check(natRoute, n + (rsNative ? ": taken natively inside the bracket: each residue call one record span"
                                : segOff ? ": segmentless off: every residue call declined at the top"
                                         : ": gate off: the native arm is the SDK body"));
  // ---- the spans: the SDK's bytes where each native span lies, the native memory untouched ----
  std::vector<uint8_t> sdkBuf = sdk.after.buffer, natBuf = nat.after.buffer;
  if (rsNative) {
    bool okSpans = nat.spans.size() == size_t(nProd), untouched = okSpans;
    for (size_t k = 0; okSpans && k < nat.spans.size(); k++) {
      const auto& s = nat.spans[k];
      const Case cs = s[0] == uint32_t(kRsLock) ? bracketLock() : s[0] == uint32_t(kRsInval) ? bracketInval() : bracketIdle();
      const std::vector<uint32_t> want = expectedDwords(cs);
      okSpans = s[0] == uint32_t(producers[k]) && s[1] >= kBuf && s[2] > s[1] && s[2] <= kBuf + kBufSize &&
                s[2] - s[1] == 4u * want.size();
      for (uint32_t i = 0; okSpans && i < want.size(); i++) {
        const uint8_t* b = sdkBuf.data() + (s[1] - kBuf) + 4u * i;
        okSpans = (uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]) == want[i];
      }
      for (uint32_t a = s[1]; okSpans && untouched && a < s[2]; a++)
        untouched = g_pm4Out ? natBuf[a - kBuf] == sdkBuf[a - kBuf] : natBuf[a - kBuf] == kFill;
      if (okSpans)
        for (uint32_t a = s[1]; a < s[2]; a++) natBuf[a - kBuf] = sdkBuf[a - kBuf];
    }
    check(okSpans, n + ": the SDK wrote exactly the expected PM4 where each native span lies (LOCK, INVAL" +
                       std::string(bc.pending ? ")" : ", IDLE)"));
    check(untouched && okSpans, n + (g_pm4Out ? ": the native spans are the SDK's bytes (PM4 output)"
                                              : ": the native spans left memory untouched (the records stand for them)"));
  } else {
    check(nat.spans.empty() && sdk.spans.empty(), n + ": no record span in either arm");
  }
  // ---- equality with the SDK arm ----
  diffBytes(n + " device", sdk.after.device, nat.after.device, kDev);
  diffBytes(n + " buffer", sdkBuf, natBuf, kBuf);
  diffBytes(n + " ring", sdk.after.ring, nat.after.ring, kRing);
  check(nat.after.device == sdk.after.device && nat.after.fb == sdk.after.fb && nat.after.res == sdk.after.res,
        n + ": device, fence block and resource equal the actual SDK");
  check(natBuf == sdkBuf, n + ": command buffer equals the actual SDK" + std::string(rsNative ? " outside the spans" : ""));
  check(nat.after.ring == sdk.after.ring, n + ": ring descriptors equal the actual SDK");
  check(nat.log == sdk.log, n + ": consumer stores and interrupts equal the actual SDK's");
  check(nat.reg5C8 == sdk.reg5C8 && nat.regA2F == sdk.regA2F && nat.regA30 == sdk.regA30 && nat.regA31 == sdk.regA31 &&
            nat.waits == sdk.waits && nat.coher == sdk.coher,
        n + ": consumer registers (WAIT_UNTIL, COHER), waits and coherency requests equal the actual SDK's");
  if (sdk.coher != 4u || nat.coher != 4u)
    std::printf("[RESIDUE_DIAG] %s coherency requests sdk %llu native %llu\n", n.c_str(), (unsigned long long)sdk.coher,
                (unsigned long long)nat.coher);
  check(sdk.coher == 4u && nat.coher == 4u,
        n + ": the bracket's two COHER blocks (LOCK, INVAL) applied in both bin passes: 4 coherency requests per arm");
  if (rsNative && !g_cpPm4 && !g_pm4Out)
    check(c2.host == c1.host + 1u && c2.hostApplied == c1.hostApplied + 2u,
          n + ": kind 22 (the texture lock's status) consumed in both BS passes (depth, colour): applied twice");
  // ---- the bracket's own route ----
  if (brNative && !bc.pending) {
    check(c1.end == c0.end + 1u && c2.end == c1.end + 1u && c1.consumed == c0.consumed + 1u &&
              c2.consumed == c1.consumed + 1u,
          n + ": BRACKET_END published and consumed once in each arm");
    check(c1.segs - c0.segs >= 2u && c2.segs - c1.segs == c1.segs - c0.segs,
          n + ": nested replay: BRACKET_END replayed the bracket's segments per pass (the same count in both arms)");
    check(sdk.pumps == 0u && nat.pumps == 0u && sdk.interrupts == 0u && nat.interrupts == 0u && sdk.a74AtEnd == 0u &&
              nat.a74AtEnd == 0u,
          n + ": no kick interrupt, no pump: dev+0x2A74 never moved");
  } else if (brNative) {
    check(c1.pending == c0.pending + 1u && c2.pending == c1.pending + 1u && c2.end == c0.end && sdk.a74AtEnd != 0u &&
              nat.a74AtEnd != 0u,
          n + ": BRACKET_END declined while the SDK bracket's pump work was outstanding (dev+0x2A74 != 0 at the end)");
    check(sdk.pumps >= 1u && nat.pumps >= 1u,
          n + ": the SDK kick chained behind that pump, which replayed both lists; EndSync drained dev+0x2A74");
  } else {
    check(c2.end == c0.end && sdk.pumps >= 1u && nat.pumps >= 1u,
          n + ": the SDK bracket: its kick's interrupt ran the actual pump, which replayed the list");
  }
}
}  // namespace rt

int runNativeResidueTest(const char* mode) {
  namespace K = sr::kernel;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  {  // a bare run re-runs itself with exactly the runner's configuration for the mode (tools/difftest/suite_bare.inc)
    const bool off = std::strcmp(mode, "gate-off") == 0, seg = std::strcmp(mode, "segmentless-off") == 0;
    const wchar_t* const gate = off ? L"0" : L"1";
    const bare::Conf conf = {{L"SR_PM4_RING", L"0"}, {L"SR_PM4_RING_EXEC", L"0"}, {L"SR_PM4_CP", L"0"},
                             {L"SR_ALLOW_FORCING", L"0"},
                             {L"SR_AB_TOGGLE", L"SR_NATIVE_IDLE_WAIT+SR_NATIVE_INVAL_RANGE+SR_NATIVE_LOCK_COHER"},
                             {L"SR_AB_PRESENT", L"1"}, {L"SR_NATIVE_SEGMENTLESS", seg ? L"0" : L"1"},
                             {L"SR_NATIVE_FLUSH", L"1"}, {L"SR_NATIVE_IDLE_WAIT", gate}, {L"SR_NATIVE_INVAL_RANGE", gate},
                             {L"SR_NATIVE_LOCK_COHER", gate}, {L"SR_NATIVE_BRACKET", L"0"}, {L"SR_NATIVE_TRANSPORT", L"0"}};
    int code = 0;
    if (bare::handled("NATIVE_RESIDUE_TEST", "--native-residue-test", mode,
                       L"SR_NATIVE_IDLE_WAIT+SR_NATIVE_INVAL_RANGE+SR_NATIVE_LOCK_COHER", conf, code))
      return code;
  }
  // "gate-off": the three gates 0; "segmentless-off": the gates on without P2-B segmentless (every call declines).
  const bool segOff = std::strcmp(mode, "segmentless-off") == 0;
  const bool gateOn = std::strcmp(mode, "gate-off") != 0;
  K::registerStubs();
  sr::guest::initializeDispatchTable();
  K::kpcrBindCurrent(rt::kStack, rt::kStack - 0x10000u, 3);
  const int abIdle = sr::abt::reg("SR_NATIVE_IDLE_WAIT"), abInval = sr::abt::reg("SR_NATIVE_INVAL_RANGE"),
            abLock = sr::abt::reg("SR_NATIVE_LOCK_COHER");
  rt::check(idleWaitOn() == gateOn && invalRangeOn() == gateOn && lockCoherOn() == gateOn && g_on &&
                g_segmentless == !segOff && abIdle >= 0 && abInval >= 0 && abLock >= 0 && sr::abt::active(),
            "requested gates, record mode, segmentless and the per-present A/B of all three");
  rt::check(PPC_LOOKUP_FUNC(K::getGuestBase(), 0x820FDCB8u) == sub_820FDCB8 &&
                PPC_LOOKUP_FUNC(K::getGuestBase(), 0x820F4280u) == sub_820F4280 &&
                PPC_LOOKUP_FUNC(K::getGuestBase(), 0x820F3988u) == sub_820F3988,
            "production dispatch resolves the three residue overrides");
  rt::check(PPC_LOOKUP_FUNC(K::getGuestBase(), 0x820FD8C0u) == sub_820FD8C0,
            "production dispatch resolves the close 820FD8C0 (InsertFence's and 820FC000's) to the segmentless override");
  bool mapped = true;
  for (uint32_t va : {rt::kDev, rt::kDev + 0x4FFFu, rt::kCell, rt::kObject, rt::kKt, rt::kKt + 0x3FFu, rt::kRes,
                      rt::kFb, rt::kRing, rt::kBuf, rt::kBuf + rt::kBufSize - 1u, rt::kStack - 0x2000u})
    mapped = mapped && rt::host(va) != nullptr;
  rt::check(mapped, "fixture memory mapped");
  if (rt::failed) {
    std::printf("[NATIVE_RESIDUE_TEST] %d passed, %d failed\n", rt::passed, rt::failed);
    return 1;
  }
  const size_t index = (rt::kAllocCb - PPC_CODE_BASE) >> 2;
  PPCFunc* const saved = K::g_dispatchTable[index];
  K::g_dispatchTable[index] = rt::residueTestAllocate;
  // KPCR+0x100 (the current KTHREAD) -> the fixture's, whose +332 is the thread id the owner tests read
  const uint32_t pcr = K::kpcrCurrent(), oldKt = rt::rd(pcr + 0x100u);
  rt::wr(pcr + 0x100u, rt::kKt);
  if (!g_arena) g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRecArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  const bool hooked = setScopeHook(0x820FD218u, nullptr, rt::ringAfter);
  rt::check(hooked, "the emulated CP hooks 820FD218's N0 scope (the ringless override's hand-off point)");
  std::thread(rt::watchdog).detach();

  using C = rt::Case;
  std::vector<C> cases;
  auto add = [&](C c) { cases.push_back(c); };
  // IDLE: BlockUntilIdle 820FDCB8
  {C c; c.name = "idle-plain"; c.prod = rt::kIdle; add(c);}
  {C c; c.name = "idle-records"; c.prod = rt::kIdle; c.records = true; add(c);}
  {C c; c.name = "idle-records-only"; c.prod = rt::kIdle; c.gap = false; c.records = true; add(c);}
  {C c; c.name = "idle-kick"; c.prod = rt::kIdle; c.kick = true; c.records = true; add(c);}
  {C c; c.name = "idle-no-fence"; c.prod = rt::kIdle; c.S = 0u; add(c);}
  {C c; c.name = "idle-pinned"; c.prod = rt::kIdle; c.pinned = true; add(c);}
  {C c; c.name = "idle-spin"; c.prod = rt::kIdle; c.S = 0u; c.spin = true; add(c);}
  {C c; c.name = "idle-late-fence"; c.prod = rt::kIdle; c.lateFence = true; c.records = true; add(c);}
  {C c; c.name = "idle-late-no-gpu"; c.prod = rt::kIdle; c.kick = true; c.lateNoGpu = 0x10000u; c.expect = 3; add(c);}
  {C c; c.name = "idle-other-thread"; c.prod = rt::kIdle; c.owner = false; c.expect = 0; c.reason = kRsOwner; add(c);}
  {C c; c.name = "idle-no-gpu"; c.prod = rt::kIdle; c.b39 = 2u; c.expect = 0; c.reason = kRsMode; add(c);}
  {C c; c.name = "idle-tile-pred"; c.prod = rt::kIdle; c.tilePred = true; c.expect = 0; c.reason = kRsEntry; add(c);}
  // INVAL: GpuInvalidateMemoryRange 820F4280
  {C c; c.name = "inval-owner"; c.prod = rt::kInval; c.addr = 0xA0B00123u; c.size = 0x2345u; add(c);}
  {C c; c.name = "inval-records"; c.prod = rt::kInval; c.records = true; c.addr = 0xE0B00FF0u; c.size = 0x20u; add(c);}
  {C c; c.name = "inval-kick"; c.prod = rt::kInval; c.kick = true; c.addr = 0xA0C00000u; c.size = 0x1000u; add(c);}
  {C c; c.name = "inval-late-no-gpu"; c.prod = rt::kInval; c.kick = true; c.lateNoGpu = 0x10000u; c.expect = 3; add(c);}
  {C c; c.name = "inval-other-thread"; c.prod = rt::kInval; c.owner = false; c.addr = 0xA0B00123u; c.size = 0x2345u;
   c.expect = 2; add(c);}
  {C c; c.name = "inval-no-gpu"; c.prod = rt::kInval; c.b39 = 2u; c.expect = 0; c.reason = kRsMode; add(c);}
  // LOCK: the lock core 820F3988
  {C c; c.name = "lock-vb"; c.prod = rt::kLock; add(c);}
  {C c; c.name = "lock-vb-records"; c.prod = rt::kLock; c.records = true; c.addr = 0xE0A00040u; add(c);}
  {C c; c.name = "lock-vb-dirty"; c.prod = rt::kLock; c.resType = 0x00200001u; add(c);}
  {C c; c.name = "lock-vb-fence-open"; c.prod = rt::kLock; c.fence = rt::kS0; add(c);}
  {C c; c.name = "lock-vb-fence-done"; c.prod = rt::kLock; c.fence = rt::kS0 - 2u; add(c);}
  {C c; c.name = "lock-vb-kick"; c.prod = rt::kLock; c.kick = true; add(c);}
  {C c; c.name = "lock-texture"; c.prod = rt::kLock; c.route = 1; c.status = 0x02000000u; c.addr = 0xA0D01234u;
   c.size = 0x3000u; add(c);}
  {C c; c.name = "lock-whole-status"; c.prod = rt::kLock; c.route = 1; c.status = 0x01000000u; add(c);}
  {C c; c.name = "lock-fence12-range24"; c.prod = rt::kLock; c.route = 1; c.flags = 0x1000u; c.fence = rt::kS0 - 2u;
   c.resType = 0x00200001u; c.r5 = 1u; c.r7 = 0xA0A00000u; add(c);}
  {C c; c.name = "lock-phys-view"; c.prod = rt::kLock; c.route = 1; c.flags = 0x10u; c.expect = 0; c.reason = kRsNoPk;
   add(c);}
  {C c; c.name = "lock-texture-late-no-gpu"; c.prod = rt::kLock; c.route = 1; c.status = 0x02000000u; c.kick = true;
   c.lateNoGpu = 0x10000u; c.expect = 3; add(c);}
  {C c; c.name = "lock-index-buffer"; c.prod = rt::kLock; c.route = 2; c.flags = 0u; c.expect = 0; c.reason = kRsNoPk;
   add(c);}
  {C c; c.name = "lock-other-thread"; c.prod = rt::kLock; c.owner = false; c.expect = 2; add(c);}
  {C c; c.name = "lock-no-overwrite"; c.prod = rt::kLock; c.flags = 2u; c.expect = 0; c.reason = kRsNoPk; add(c);}
  // InsertFence 820FDD30: already native under segmentless (nothing converted; its close is the FENCE record)
  {C c; c.name = "insert-fence"; c.prod = rt::kFence; c.gap = false; c.records = true; add(c);}
  {C c; c.name = "insert-fence-gap"; c.prod = rt::kFence; add(c);}
  for (C c : cases) {
    // IndexBuffer_Lock always passes flags | 2: its native arm declines as no-packet (the guest body writes no PM4)
    if (c.route == 2) c.flags |= 2u;
    // segmentless off: the ring path's closes are guest code; the fence waits complete through the same emulated CP
    const int bit = c.prod == rt::kInval ? abInval : c.prod == rt::kLock ? abLock : abIdle;
    rt::runCase(c, gateOn, segOff, bit);
  }

  // Bracket cycles: BRACKET's begin needs a consumer (the publication observer: the emulated CP takes every hand-off
  // itself at 820FD218, before the close would hand it to the observer); EndSync's event imports answer at once.
  {
    auto& registry = K::ExportRegistry::instance();
    const K::ExportFn origSet = registry.getExport("KeSetEvent"), origReset = registry.getExport("KeResetEvent"),
                      origWait = registry.getExport("KeWaitForSingleObject");
    registry.registerExport("KeSetEvent", rt::evNoop);
    registry.registerExport("KeResetEvent", rt::evNoop);
    registry.registerExport("KeWaitForSingleObject", rt::evNoop);
    setSegmentPublicationObserver(rt::observeNoop);
    using B = rt::BCase;
    std::vector<B> brackets;
    {B b; b.name = "residue"; brackets.push_back(b);}
    {B b; b.name = "residue-records"; b.records = true; brackets.push_back(b);}
    {B b; b.name = "pending"; b.pending = true; brackets.push_back(b);}
    for (const B& b : brackets) rt::runBracketCase(b, gateOn, segOff, abIdle);
    setSegmentPublicationObserver(nullptr);
    registry.registerExport("KeSetEvent", origSet);
    registry.registerExport("KeResetEvent", origReset);
    registry.registerExport("KeWaitForSingleObject", origWait);
  }

  setScopeHook(0x820FD218u, nullptr, nullptr);
  rt::wr(pcr + 0x100u, oldKt);
  K::g_dispatchTable[index] = saved;
  std::printf("[NATIVE_RESIDUE_TEST] %d passed, %d failed\n", rt::passed, rt::failed);
  return rt::failed ? 1 : 0;
}
}  // namespace sr::native::flush
