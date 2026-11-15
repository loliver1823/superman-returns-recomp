// Included by kernel/native_flush.cpp only in difftest (SR_DIFFTEST): p2.transport BRACKET (SR_NATIVE_BRACKET).
//
// The actual SDK Z-pass bracket path against the native BRACKET on identical fixtures, both driven through production
// dispatch: BeginZPass 8210DF38, the bracket's closes 820FD8C0, EndZPass 8210E1A0 -> InvokeRenderPass 8210E458 ->
// 82110C78 (or 82110C78 called directly), the per-present A/B toggle (SR_AB_TOGGLE=SR_NATIVE_BRACKET, SR_AB_PRESENT=1)
// choosing the arm. OFF = the actual SDK kick, then the actual SDK marker interpreter 82110460 and EndSync 82110768
// (the pump's work after the kick's interrupt); ON = BRACKET. One in-order consumer emulates the CP for both arms: ring
// IB descriptors resolved as the ringless override resolves them (the close's hand-off id, else the deferred lookup,
// else the literal guest PM4), BS items through applyBinReg, the kick block's INTERRUPT answered as the guest ISR
// answers it (the CPU bit cleared in pc+0). No GPU worker, no game. Run with SR_PM4_RING=0 SR_PM4_CP=0.
#include <cwchar>
#include "kernel/export_registry.hpp"
#include "gfx/resource_memory.hpp"  // memres::resourceMode (the native resource route, default ON since the bake)
#include "gfx/resource_ab.hpp"      // memres::resourceAbDual (the frozen-pass A/B keeps the tracker)
#include "kernel/kpcr.hpp"
#include "guest/guest_harness.hpp"

extern "C" void __imp__sub_82110460(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82110768(PPCContext& __restrict, uint8_t*);

namespace sr::native::flush {
namespace bt {
namespace K = sr::kernel;
constexpr uint32_t kDev = 0x70800000u, kCell = 0x70806000u, kObject = 0x70806800u, kStack = 0x70780000u;
constexpr uint32_t kFb = 0x90807000u, kPc = kFb + 0x80u, kSlots = kFb + 0x100u;  // physical-window VAs
constexpr uint32_t kRing = 0x90808000u, kRingMask = 4095u;                        // 16 KB ring
constexpr uint32_t kBuf = 0x90A00000u, kBufSize = 0x80000u;                       // segments up, allocations down
constexpr uint32_t kAllocCb = 0x82110800u;
constexpr uint32_t kHsio = 0x829AC0ECu;  // the HSIO failure global (kept 0)
constexpr uint32_t kS = kDev + 0x2A3Cu;  // the replay state

int passed = 0, failed = 0;
void check(bool ok, const std::string& name) {
  std::printf("%s%s\n", ok ? "  ok  " : "FAIL: ", name.c_str());
  ok ? ++passed : ++failed;
}
uint32_t rd(uint32_t va) { return K::readGuestU32BE(va); }
void wr(uint32_t va, uint32_t v) { K::writeGuestBE32(va, v); }
uint8_t* host(uint32_t va) { return K::guestVAToHost(va); }
uint32_t phys(uint32_t va) { return brPhys(va); }

// Tracker-owned recording lists (the tcmp1 diagnosis, 2026-10-14): the GPU write tracker makes pages it validated
// PAGE_READONLY in both physical copies; the first CPU store faults into its handler, which releases them. The end's
// admission must take the lists' prefix / cursor words on such pages as the SDK body does (it simply stores), and its
// own stores must release them. Pages protected as the CP's shared-memory validation does (native_transport_test.cpp).
namespace W = sr::kernel::gww;
// mem.resources bake (native/mem-native a1fda568, 2026-10-17): on the native resource route (the dev default) the
// tracker is off and no page is ever tracker-owned - only the page model (SR_MEM_RESOURCES=0, the oracle arm; the
// runners pin =0 for tracked-lists) protects pages. There the admission is writableRange alone
// (docs/RELEASE_COMPOSITION.md 9.6): tracked-lists-native - lists on pages protected through the tracker's API decline
// once, at a list's site, before any effect, and the end is the SDK's in every byte.
bool nativeResourceRoute() { return sr::gfx::memres::resourceMode() && !sr::gfx::memres::resourceAbDual(); }
bool protectionIs(uint32_t va, DWORD protection) {
  MEMORY_BASIC_INFORMATION region{};
  return VirtualQuery(host(va), &region, sizeof(region)) && region.State == MEM_COMMIT && region.Protect == protection;
}
uint32_t trackBits(uint32_t page) {  // 1 owned, 2 validated, 4 the 0x8 copy RO, 8 the 0xA copy RO
  return (W::protGen(page) ? 1u : 0u) | (W::clean(page, 8u) ? 2u : 0u) |
         (protectionIs(0x80000000u | (page << 12), PAGE_READONLY) ? 4u : 0u) |
         (protectionIs(0xA0000000u | (page << 12), PAGE_READONLY) ? 8u : 0u);
}
// Protect the pages holding the three lists' prefix and cursor words; true when the tracker owns every one.
bool trackLists(std::vector<uint32_t>& pages) {
  pages.clear();
  for (uint32_t off : {0x32C8u, 0x32A0u, 0x32B4u})
    for (uint32_t va : {rd(kDev + off + 4u), rd(kDev + off + 8u)})
      if (va >= 0x80000000u) pages.push_back((va & 0x1FFFFFFFu) >> 12);
  std::sort(pages.begin(), pages.end());
  pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
  if (pages.empty()) return false;
  for (unsigned i = 0; i < 64u; ++i) W::beginFrame();  // past the cooldown of every block that faulted before
  W::protectForValidation(pages.data(), uint32_t(pages.size()));
  bool owned = true;
  for (uint32_t p : pages) {
    W::markValidated(p, 8u);
    owned &= trackBits(p) == 15u;
  }
  std::vector<uint32_t> pending;
  W::takeReleasedBlocks(pending);
  return owned;
}
bool trackReleased(const W::Stats& before, const std::vector<uint32_t>& pages) {
  const W::Stats now = W::stats();
  std::vector<uint32_t> released;
  W::takeReleasedBlocks(released);
  bool all = now.faults > before.faults && now.hostWrites == before.hostWrites && !released.empty();
  for (uint32_t p : pages) all &= trackBits(p) == 0u;
  return all;
}
// The real tracker takes the guest arena from the process-global registration (native_transport_test.cpp registers it
// the same way): held for a tracked case only, the prior registration restored after it.
struct ArenaRegistration {
  uint8_t* base = nullptr;
  size_t size = 0;
  K::ExCreateThreadRunnerFn runner = nullptr;
  ArenaRegistration() {
    runner = K::getExCreateThreadRunner(&base, &size);
    K::registerExCreateThreadRunner(runner, K::getGuestBase(), K::getGuestSize());
  }
  ~ArenaRegistration() { K::registerExCreateThreadRunner(runner, base, size); }
  ArenaRegistration(const ArenaRegistration&) = delete;
  ArenaRegistration& operator=(const ArenaRegistration&) = delete;
};
uint64_t memoryDeclines(bool listsOnly = false) {  // the site counters' sum (listsOnly: the lists' prefix / cursor)
  uint64_t t = 0;
  for (int s = 0; s < kBmN; s++)
    if (!listsOnly || (s >= kBmPrefix0 && s <= kBmCursor2)) t += bracketMemoryDecl(s);
  return t;
}

// The allocator callback (object dev+0x3354 +172, a supported SDK mechanism): bump allocations down from the buffer top.
uint32_t g_top = 0, g_failBytes = 0, g_lateNoGpuBytes = 0;
std::vector<uint32_t> g_requests;
PPC_FUNC(bracketTestAllocate) {
  const uint32_t bytes = rd(ctx.r5.u32), align = ctx.r6.u32 ? ctx.r6.u32 : 4u;
  g_requests.push_back(bytes);
  if (g_lateNoGpuBytes && bytes == g_lateNoGpuBytes) host(kDev + 0x2A39u)[0] |= 2u;  // a helper flips no-GPU mode
  if (g_failBytes && bytes == g_failBytes) {
    ctx.r3.u64 = 0u;
    return;
  }
  g_top = (g_top - bytes) & ~(align - 1u);
  ctx.r3.u64 = g_top;
}
// EndSync's event imports: recorded, never a real wait (the ordering is the SDK's own).
std::vector<std::array<uint32_t, 3>> g_events;
uint32_t setEvent(uint32_t a, uint32_t b, uint32_t, uint32_t, uint32_t, uint32_t) {
  g_events.push_back({1u, a, b});
  return 0u;
}
uint32_t resetEvent(uint32_t a, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
  g_events.push_back({2u, a, 0u});
  return 0u;
}
uint32_t waitEvent(uint32_t a, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
  g_events.push_back({3u, a, 0u});
  return 0u;
}

// Publications the native close / BRACKET hand over while no CP runs (the ringless override's hand-off).
struct Token {
  uint64_t id;
  uint32_t phys, dwords;
  bool chained;
};
std::vector<Token> g_tokens;
void observe(uint64_t id, uint32_t p, uint32_t n, bool chained) { g_tokens.push_back({id, p, n, chained}); }

struct Case {
  const char* name;
  uint32_t closes = 2;      // bracket closes with content before EndZPass's own flush
  bool records = false;     // native Entry records inside the bracket segments (record parts besides gaps)
  bool neutral = false;     // 0x2A3B & 0x20: EndZPass's SetScissorRect writes PM4 between 0x85 and 0x86
  bool direct = false;      // 82110C78 called directly (EndZPass without InvokeRenderPass, its pre-kick part by hand)
  bool postKick = false;    // an unpinned close after the kick (the SDK chains it behind the replay)
  uint32_t range = 0;       // 0 empty (0x00000000FFFFFFFF), 1 a range, 2 all ones (the native close's sentinel)
  bool whole = false;       // 0x2A3A & 4: the whole command buffer
  uint32_t failBytes = 0;   // the allocator fails this request (44 coherency, 400 kick, 128 hole)
  bool preFailed = false;   // 0x2A39 & 0x40 before the kick (direct route)
  uint32_t lateNoGpu = 0;   // a helper sets no-GPU mode on this request
  // declines (the native arm must equal the SDK arm whole)
  bool poison = false;      // one bracket close goes the guest way (HSIO failure global)
  bool doubleZ = false;     // a second 0x84 in the list: not the canonical grammar
  bool dupRecord = false;   // the 2nd list record overwritten with the 1st: binding by sequence fails
  bool published = false;   // a bound entry was already published (a pump took it)
  bool tracked = false;     // the lists' prefix / cursor pages tracker-owned read-only before the end (direct route)
  bool ringCut = false;     // the graceful quit's ring cut lands before the direct route's last close (native arm)
  // expectations
  int taken = 1;            // 1 BRACKET_END published, 2 failure branch, 3 SDK route after a late mode flip, 0 declined
  BrDecl reason = kBrNoGen;
  bool sdkKick = true;      // the SDK body appends its kick (false: its failure branch, or no-GPU without PIX)
};

struct Snap {
  std::vector<uint8_t> device, buffer, ring, fb, stack;
  std::array<uint64_t, 8> regs{};
  uint32_t a74 = 0;
  bool operator==(const Snap& o) const {
    return device == o.device && buffer == o.buffer && ring == o.ring && fb == o.fb && stack == o.stack && regs == o.regs;
  }
};
struct Item2 {
  uint32_t kind, a, b;  // 0 BS {value}, 1 segment {phys, dwords}
  bool operator==(const Item2& o) const { return kind == o.kind && a == o.a && b == o.b; }
};
struct Effects {
  std::vector<std::array<uint32_t, 4>> log;  // what (the CP trace's 0 store .. 4 deferred; 5 interrupt), addr, value, binSelect
  size_t kickLo = 0, kickHi = 0;
  bool kick = false, bad = false;
};
struct Outcome {
  Snap producer;
  uint32_t a74After = 0;
  std::vector<Item2> replay;   // SDK: the pump's ring items; native: the BRACKET_END steps
  Effects fx;
  bool consumed = false, pumped = false;
  uint64_t r3 = 0;
  std::vector<std::shared_ptr<SegParts>> bound;
  // tracked: the list pages the end found tracker-owned, and whether its stores released them
  std::vector<uint32_t> trackPages;
  bool trackOwned = false, trackReleased = false;
};

Effects* g_fx = nullptr;
sr::gfx::Pm4Engine* g_eng = nullptr;
uint8_t* fxMap(void*, uint32_t p) { return p < 0x20000000u ? host(0x80000000u | p) : nullptr; }
void fxTrace(void*, uint32_t what, uint32_t addr, uint32_t value) {
  g_fx->log.push_back({what, addr, value, uint32_t(g_eng->binSelect)});
}
// The mixed fixture's pump work: the kick block's callback 82110B00 (SCRATCH_REG4 at pc+0x10) queues its ctx (REG5 at
// pc+0x14) for the pump thread, as the guest ISR's DPC does.
bool g_mixed = false;
std::vector<uint32_t> g_pumpQueue;
void fxInterrupt(void*, uint32_t mask) {  // the guest ISR's answer: cb(ctx) ran, its CPU bit cleared in pc+0
  g_fx->log.push_back({5u, mask, rd(kPc), uint32_t(g_eng->binSelect)});  // 5: apart from the trace's kinds 0..4
  if (g_mixed && rd(kPc + 0x10u) == 0x82110B00u) g_pumpQueue.push_back(rd(kPc + 0x14u));
  wr(kPc, rd(kPc) & ~mask);
}
bool fxRunning(void*) {
  g_fx->bad = true;  // a wait that would block: the emulated CP never waits on the producer
  return false;
}

PPCContext context() {
  PPCContext c{};
  c.r1.u64 = kStack;
  c.r13.u64 = K::kpcrCurrent();
  c.r14.u64 = 0xAABBCCDDEEFF0011ull;
  c.r23.u64 = 23u;
  c.r28.u64 = 28u;
  c.r31.u64 = 0x1122334455667788ull;
  c.f14.u64 = 0x1234567812345678ull;
  return c;
}
void call(uint32_t va, PPCContext& c) { PPC_LOOKUP_FUNC(K::getGuestBase(), va)(c, K::getGuestBase()); }
void closeNow() {
  PPCContext c = context();
  c.r3.u64 = kDev;
  call(0x820FD8C0u, c);
}
void listAppend(uint32_t marker) {  // as BeginZPass / EndZPass append a marker to dev+0x32C8
  const uint32_t at = rd(kDev + 0x32D0u);
  wr(at, marker);
  wr(kDev + 0x32D0u, at + 4u);
}
// Guest PM4 in the open segment (gap bytes): per-pass predicated stores and an unpredicated counter.
uint32_t g_counter = 0;
void gapContent(uint32_t k) {
  const uint32_t at = rd(kDev + 0x28u) + 4u;
  const uint32_t w[] = {0xC0006000u, 0x15555555u, 0xC0025801u, 3u, phys(kSlots + 8u * (k & 63u)) | 2u, 0xA0000000u | k,
                        0xC0006000u, 0x2AAAAAAAu, 0xC0025801u, 3u, phys(kSlots + 8u * (k & 63u) + 4u) | 2u, 0xB0000000u | k,
                        0xC0006000u, 0xFFFFFFFFu, 0xC0025800u, 3u, phys(kSlots + 0x700u) | 2u, 0xC0000000u | ++g_counter};
  for (uint32_t i = 0; i < sizeof(w) / 4u; i++) wr(at + 4u * i, w[i]);
  wr(kDev + 0x28u, at + uint32_t(sizeof(w)) - 4u);
}
// A native record span (the producer API): a colour-only predicated store.
bool recordContent(uint32_t k) {
  PPCContext c = context();
  Entry e(c, K::getGuestBase(), kDev);
  if (!e.open()) return false;
  const uint32_t mask = 0x2AAAAAAAu, body[3] = {3u, phys(kSlots + 0x400u + 4u * (k & 63u)) | 2u, 0xD0000000u | k};
  if (!e.pkt(kRkBinReg, 0xC0006000u, &mask, 1u, 0u) || !e.pkt(kRkEventShd, 0xC0025801u, body, 3u)) {
    e.abandon();
    return false;
  }
  e.close();
  return true;
}

void prepare(const Case& cs) {
  std::memset(host(kDev), 0, 0x5000u);
  std::memset(host(kCell), 0, 0x1000u);
  std::memset(host(kFb), 0, 0x1000u);
  std::memset(host(kRing), 0, (kRingMask + 1u) * 4u);
  std::memset(host(kBuf), 0, kBufSize);
  std::memset(host(kStack - 0x2000u), 0xA5, 0x2000u);
  wr(0x82000948u, kCell);
  wr(kCell, kDev);
  wr(kDev + 0x2A10u, kFb);
  wr(kDev + 0x2A14u, kPc);
  wr(kDev + 0x34A4u, kRing);
  wr(kDev + 0x34A8u, kRingMask);
  wr(kDev + 0x34ACu, kBuf);
  wr(kDev + 0x34B0u, kBuf + kBufSize);
  const uint32_t seg = kBuf + 0x100u;
  wr(kDev + 0x34C4u, seg);
  wr(kDev + 0x28u, seg - 4u);
  wr(kDev + 0x2Cu, kBuf + 0x60000u);
  wr(kDev + 0x30u, kBuf + 0x5F000u);
  wr(kDev + 0x2A1Cu, 0x101u);
  wr(kDev + 0x3298u, seg - 4u);
  wr(kDev + 0x329Cu, 1u);
  wr(kDev + 0x34BCu, 1u);
  wr(kDev + 0x3354u, kObject);
  wr(kObject + 164u, 0x11223344u);
  wr(kObject + 172u, kAllocCb);
  wr(kDev + 0x34CCu, 0x7FFFFFFFu);
  wr(kDev + 0x2D18u, 0xFFFFFFFFu);  // the native close's empty range for the begin's own close
  wr(kDev + 0x2D1Cu, 0xFFFFFFFFu);
  wr(kDev + 0x2BB8u, 2u);           // the pump CPU: InvokeRenderPass passes 0x01000000 << 2
  wr(kDev + 0x2844u, 0u);           // the window scissor 0,0 .. 640,480
  wr(kDev + 0x2848u, (480u << 15) | 640u);
  wr(kDev + 0x3090u, 0u);           // EndZPass's SetScissorRect source (the neutral case)
  wr(kDev + 0x3094u, 0u);
  wr(kDev + 0x3098u, 320u);
  wr(kDev + 0x309Cu, 240u);
  wr(kS + 0x16Cu, 0x7FFFFFFFu);
  wr(kHsio, 0u);
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
  g_failBytes = g_lateNoGpuBytes = 0;
  g_counter = 0;
  g_requests.clear();
  g_tokens.clear();
  g_events.clear();
  (void)cs;
}

Snap snap(const PPCContext& c) {
  Snap s;
  s.device.assign(host(kDev), host(kDev) + 0x5000u);
  s.a74 = rd(kDev + 0x2A74u);
  std::memset(s.device.data() + 0x2A74u, 0, 4u);  // compared on its own (the build note's one difference)
  s.buffer.assign(host(kBuf), host(kBuf) + kBufSize);
  s.ring.assign(host(kRing), host(kRing) + (kRingMask + 1u) * 4u);
  s.fb.assign(host(kFb), host(kFb) + 0x1000u);
  s.stack.assign(host(kStack - 0x2000u), host(kStack - 0x2000u) + 0x2000u);
  s.regs = {c.r1.u64, c.r3.u64, c.r13.u64, c.r14.u64, c.r23.u64, c.r28.u64, c.r31.u64, c.f14.u64};
  return s;
}

// The pump's work after the kick's interrupt: the actual marker interpreter over the recorded list, then EndSync.
std::vector<Item2> sdkPump(Outcome& o) {
  std::vector<Item2> items;
  const uint32_t root = rd(kDev + 0x32C8u), view = phys(root) - 0x40000000u;
  const uint32_t w0 = rd(kDev + 0x2A44u);
  wr(kS + 0x58u, view);  // 82110B00's store of the kick's ctx
  PPCContext c = context();
  c.r3.u64 = uint64_t(view) + 4u;
  c.r4.u64 = kS;
  __imp__sub_82110460(c, K::getGuestBase());
  const uint32_t at = c.r3.u32;
  o.pumped = rd(at) == 0xC0000000u;
  const uint32_t w1 = rd(kDev + 0x2A44u);
  for (uint32_t i = w0; i < w1;) {
    const uint32_t hdr = rd(kRing + 4u * i);
    if (hdr == 0xC0006200u) {
      items.push_back({0u, rd(kRing + 4u * (i + 1u)), 0u});
      i += 2u;
    } else if (hdr == 0xC0013F00u) {
      items.push_back({1u, rd(kRing + 4u * (i + 1u)), rd(kRing + 4u * (i + 2u))});
      i += 3u;
    } else {
      o.pumped = false;
      break;
    }
  }
  if (o.pumped) {  // 82110840's participation: the driver lock held, itself counted
    wr(kS, 1u);
    wr(kS + 0x30u, 1u);
    wr(kS + 0x34u, 1u);
    PPCContext e = context();
    e.r3.u64 = kS;
    e.r4.u64 = at;
    __imp__sub_82110768(e, K::getGuestBase());
  }
  return items;
}

// The emulated CP over the whole ring, in order.
bool consume(Outcome& o, uint32_t kickPhys) {
  auto eng = std::make_unique<sr::gfx::Pm4Engine>();
  eng->submitDraws = false;
  eng->cpMode = true;
  eng->physToHost = fxMap;
  eng->cp.user = &o.fx;
  eng->cp.interrupt = fxInterrupt;
  eng->cp.running = fxRunning;
  eng->cp.trace = fxTrace;
  eng->binMask = ~0ull;
  eng->binSelect = ~0ull;
  g_fx = &o.fx;
  g_eng = eng.get();
  eng->writeReg(0x1DCu, 0x00020033u);  // SCRATCH_UMSK as the device-create block sets it
  eng->writeReg(0x1DDu, phys(kPc));    // SCRATCH_ADDR
  std::vector<Token> direct;
  for (const Token& t : g_tokens)
    if (!t.chained) direct.push_back(t);
  size_t t = 0;
  const uint32_t w1 = rd(kDev + 0x2A44u);
  bool ok = true;
  for (uint32_t i = 0; ok && i < w1;) {
    const uint32_t hdr = rd(kRing + 4u * i);
    if (hdr == 0xC0013F00u) {
      const uint32_t p = rd(kRing + 4u * (i + 1u)), n = rd(kRing + 4u * (i + 2u));
      uint64_t id = 0;
      if (t < direct.size() && direct[t].phys == p && direct[t].dwords == n) id = direct[t++].id;
      else id = segmentPublishId(p, n);  // a pump-published bracket record / chained segment, as the override resolves it
      if (id) {
        ok = cpSegment(*eng, id, p, n);
      } else {
        const size_t mark = o.fx.log.size();
        eng->execute(host(0x80000000u | p), size_t(n) * 4u, 1);
        if (p == kickPhys && n == 23u) {
          o.fx.kick = true;
          o.fx.kickLo = mark;
          o.fx.kickHi = o.fx.log.size();
        }
      }
      i += 3u;
    } else if (hdr == 0xC0006200u) {
      eng->applyBinReg(2u, rd(kRing + 4u * (i + 1u)));  // the P2-A BS item
      i += 2u;
    } else {
      ok = false;
    }
  }
  g_fx = nullptr;
  g_eng = nullptr;
  return ok && t == direct.size() && !o.fx.bad && !eng->nativeRecordFailed;
}

uint64_t ctr(const Sw& s) { return s.get(); }

int g_diag = 0;  // the first few mismatches print what differs ([BRACKET_DIAG] lines are not graded)
void diffBytes(const std::string& what, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t base) {
  if (g_diag >= 24) return;
  size_t i = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
  if (i == a.size() && a.size() == b.size()) return;
  g_diag++;
  char line[256];
  int at = std::snprintf(line, sizeof(line), "[BRACKET_DIAG] %s first diff at %08X:", what.c_str(), base + uint32_t(i));
  for (size_t k = i & ~size_t(15); k < (i & ~size_t(15)) + 16u && k < a.size() && k < b.size(); k += 4)
    at += std::snprintf(line + at, sizeof(line) - size_t(at), " %02X%02X%02X%02X/%02X%02X%02X%02X", a[k], a[k + 1], a[k + 2],
                        a[k + 3], b[k], b[k + 1], b[k + 2], b[k + 3]);
  std::printf("%s\n", line);
}
void diffLog(const std::string& what, const std::vector<std::array<uint32_t, 4>>& a,
             const std::vector<std::array<uint32_t, 4>>& b) {
  if (g_diag >= 24 || a == b) return;
  g_diag++;
  size_t i = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
  std::printf("[BRACKET_DIAG] %s: %zu vs %zu events, first diff at %zu\n", what.c_str(), a.size(), b.size(), i);
  for (size_t k = i > 2 ? i - 2 : 0; k < i + 4 && (k < a.size() || k < b.size()); k++)
    std::printf("[BRACKET_DIAG]   %zu: %u %08X %08X %08X | %u %08X %08X %08X\n", k, k < a.size() ? a[k][0] : 9u,
                k < a.size() ? a[k][1] : 0u, k < a.size() ? a[k][2] : 0u, k < a.size() ? a[k][3] : 0u,
                k < b.size() ? b[k][0] : 9u, k < b.size() ? b[k][1] : 0u, k < b.size() ? b[k][2] : 0u,
                k < b.size() ? b[k][3] : 0u);
}

Outcome runArm(const Case& cs, bool nativeArm, int abBit) {
  for (int i = 0; i < 2 && sr::abt::on(abBit) != nativeArm; i++) sr::abt::onGuestSwap();
  prepare(cs);
  Outcome o;
  PPCContext c = context();
  c.r3.u64 = kDev;
  c.r4.u64 = 0u;
  call(0x8210DF38u, c);  // BeginZPass(dev, 0): 82110B30, 0x84, the scissor tile rects
  for (uint32_t k = 0; k < cs.closes; k++) {
    gapContent(k);
    if (cs.records && !recordContent(k)) check(false, std::string(cs.name) + ": native record content admitted");
    if (cs.poison && k == 1u) {
      PresentGateOracle sdkClose;  // the scoped SDK oracle: this one close is the guest body's (pinned mode B)
      closeNow();
    } else {
      closeNow();
    }
  }
  gapContent(cs.closes);  // EndZPass's own flush records it
  if (cs.neutral) host(kDev + 0x2A3Bu)[0] |= 0x20u;
  if (cs.doubleZ) listAppend(0x84000000u);
  if (cs.dupRecord) {  // the list's 2nd record := the 1st (header 252 + 0x84 4: records from head+4+256)
    const uint32_t first = rd(kDev + 0x32C8u) + 4u + 256u;
    wr(first + 8u, rd(first));
    wr(first + 12u, rd(first + 4u));
  }
  if (cs.published) {
    const uint32_t first = rd(kDev + 0x32C8u) + 4u + 256u;
    segmentPublishId(rd(first + 4u), rd(first) & 0xFFFFFFu);
  }
  if (cs.range == 0u) { wr(kDev + 0x2D18u, 0u); wr(kDev + 0x2D1Cu, 0xFFFFFFFFu); }
  if (cs.range == 1u) { wr(kDev + 0x2D18u, 0x10901234u); wr(kDev + 0x2D1Cu, 0x10800010u); }
  if (cs.range == 2u) { wr(kDev + 0x2D18u, 0xFFFFFFFFu); wr(kDev + 0x2D1Cu, 0xFFFFFFFFu); }
  if (cs.whole) host(kDev + 0x2A3Au)[0] |= 4u;
  g_failBytes = cs.failBytes;
  g_lateNoGpuBytes = cs.lateNoGpu;
  g_brTestBound.clear();
  if (cs.direct) {
    host(kDev + 0x2A39u)[0] &= ~0x80u;  // EndZPass without InvokeRenderPass
    c = context();
    c.r3.u64 = kDev;
    call(0x8210E1A0u, c);
    host(kDev + 0x2A38u)[0] &= ~1u;     // InvokeRenderPass's pre-kick part
    // tools/graceful-quit's ring cut (kernel/quit.hpp) from here: every publish after it is dropped - the override takes
    // no hand-off (the difftest observers model it) - the END's coherency block and the END (this last close is bound
    // to the bracket, no publish of its own), then an ordinary close after the bracket
    if (cs.ringCut && nativeArm) sr::kernel::quit::g_ringCut.store(true, std::memory_order_seq_cst);
    closeNow();
    listAppend(0x86000000u);
    if (cs.preFailed) host(kDev + 0x2A39u)[0] |= 0x40u;
    if (cs.tracked) o.trackOwned = trackLists(o.trackPages);  // no store between this and the end's own
    const W::Stats trackStart = W::stats();
    c = context();
    c.r3.u64 = kDev;
    c.r4.u64 = 1u;
    c.r5.u64 = 0x04000000u;
    call(0x82110C78u, c);
    if (cs.ringCut && nativeArm) {  // an ordinary close after the bracket, still under the cut: its own publish dropped
      gapContent(98u);
      PPCContext keep = c;
      closeNow();
      c = keep;
    }
    if (cs.ringCut) sr::kernel::quit::g_ringCut.store(false, std::memory_order_seq_cst);
    if (cs.tracked) o.trackReleased = trackReleased(trackStart, o.trackPages);
  } else {
    c = context();
    c.r3.u64 = kDev;
    call(0x8210E1A0u, c);  // EndZPass: flush, 0x85, InvokeRenderPass: flush, 0x86, the kick
  }
  o.r3 = c.r3.u64;
  g_failBytes = g_lateNoGpuBytes = 0;
  o.producer = snap(c);  // the kick's own result (a post-kick close then differs by design: chained vs direct)
  o.bound = g_brTestBound;
  if (cs.postKick) {
    gapContent(99u);
    closeNow();
  }
  if (rd(kDev + 0x2A74u)) o.replay = sdkPump(o);  // a guest kick queued pump work
  o.a74After = rd(kDev + 0x2A74u);
  return o;
}

// The graceful quit's ring cut mid-bracket (tfx2, 2026-10-15: probe_graceful's quit fail-fasted 2 ms after the cut with
// "[NATIVE_FLUSH_BAD] bracket: coherency block published without its hand-off" - bracketFatal's std::abort, 0xC0000409).
// The cut lands after EndZPass, before the direct route's last close (bound to the bracket): the BRACKET_END's coherency
// block, the END itself and an ordinary close after the bracket publish into a cut ring. Each must be an orderly stop - dropped as the guest's own publishes are,
// counted apart (quitCut), the generation retired, the call returning as the SDK body does - never a mismatch, a
// literal route or a fatal. The native arm only: there is nothing to compare (the SDK's ring in difftest has no cut).
void runRingCut(int abBit) {
  Case cs;
  cs.name = "ring-cut";
  cs.direct = true;
  cs.range = 1u;
  cs.ringCut = true;
  const std::string n = cs.name;
  const QuitCutCounters q0 = quitCutCounters();
  const uint64_t end0 = ctr(g_brEnd), mismatch0 = ctr(g_segMismatch), literal0 = ctr(g_kickCbLiteral);
  const uint64_t retired0 = ctr(g_brRetired), bad0 = ctr(g_brEndFailed);
  g_brTestCutGen = 0;
  g_brTestBinding.clear();
  const Outcome nat = runArm(cs, true, abBit);
  const QuitCutCounters q = quitCutCounters();
  std::printf("  ring-cut: quitCut +segs=%llu +kicks=%llu +coherency=%llu +ends=%llu (end +%llu mismatch +%llu)\n",
              (unsigned long long)(q.segs - q0.segs), (unsigned long long)(q.kicks - q0.kicks),
              (unsigned long long)(q.coherency - q0.coherency), (unsigned long long)(q.ends - q0.ends),
              (unsigned long long)(ctr(g_brEnd) - end0), (unsigned long long)(ctr(g_segMismatch) - mismatch0));
  check(!sr::kernel::quit::g_ringCut.load() && nat.r3 == 0u && ctr(g_brEndFailed) == bad0,
        n + ": the end under the cut returns as the SDK body does (r3 0, no failure branch, no fatal)");
  check(q.segs == q0.segs + 1u && q.coherency == q0.coherency + 1u && q.ends == q0.ends + 1u && q.kicks == q0.kicks,
        n + ": the coherency block, the END and a later close dropped at the cut, each counted once (quitCut)");
  check(ctr(g_brEnd) == end0 && ctr(g_segMismatch) == mismatch0 && ctr(g_kickCbLiteral) == literal0,
        n + ": no END published, no mismatch, no literal route");
  check(ctr(g_brRetired) > retired0, n + ": the dropped bracket's bound closes are retired (no deferred entry left)");
  if (sr::native::p2trace::g_on)
    check(g_brTestCutGen != 0u && !g_brTestBinding.empty() && g_brTestBinding[0][2] == uint32_t(g_brTestCutGen),
          n + ": the trace's cut record (kind 24 aux 10) names the bound generation");
}

void runCase(const Case& cs, bool gateOn, int abBit) {
  const std::string n = cs.name;
  std::unique_ptr<ArenaRegistration> arena;
  if (cs.tracked) arena = std::make_unique<ArenaRegistration>();
  // SDK arm (the A/B OFF window: BRACKET's begin declines, so everything is the SDK's).
  const uint64_t hit0 = ctr(g_segReplayHit);
  Outcome sdk = runArm(cs, false, abBit);
  // The kick block's physical address: the SDK's 23-dword IB right after its coherency IB, or none.
  auto kickPhysOf = [&](const Outcome& o) {
    const auto& r = o.producer.ring;
    uint32_t last = 0;
    for (size_t i = 0; i + 12u <= r.size();) {
      const uint32_t hdr = uint32_t(r[i]) << 24 | uint32_t(r[i + 1]) << 16 | uint32_t(r[i + 2]) << 8 | r[i + 3];
      if (hdr == 0xC0013F00u) {
        const uint32_t p = uint32_t(r[i + 4]) << 24 | uint32_t(r[i + 5]) << 16 | uint32_t(r[i + 6]) << 8 | r[i + 7];
        const uint32_t d = uint32_t(r[i + 8]) << 24 | uint32_t(r[i + 9]) << 16 | uint32_t(r[i + 10]) << 8 | r[i + 11];
        if (d == 23u) last = p;
        i += 12u;
      } else if (hdr == 0xC0006200u) {
        i += 8u;
      } else {
        break;
      }
    }
    return last;
  };
  const uint32_t sdkKick = kickPhysOf(sdk);
  sdk.consumed = consume(sdk, sdkKick);
  const uint64_t hitSdk = ctr(g_segReplayHit) - hit0;

  // Native arm (the A/B ON window).
  const uint64_t end0 = ctr(g_brEnd), fail0 = ctr(g_brEndFailed), lit0 = ctr(g_brLiteral), cons0 = ctr(g_brConsumed);
  const uint64_t decl0 = ctr(g_brDecl[cs.reason]), hit1 = ctr(g_segReplayHit), miss1 = ctr(g_segReplayMiss);
  const uint64_t memory0 = memoryDeclines(), lists0 = memoryDeclines(true), memoryTotal0 = ctr(g_brDecl[kBrMemory]);
  Outcome nat = runArm(cs, true, abBit);
  const uint64_t memoryN = memoryDeclines() - memory0, listsN = memoryDeclines(true) - lists0;
  const uint64_t memoryTotalN = ctr(g_brDecl[kBrMemory]) - memoryTotal0;
  // The native replay steps, read from the BRACKET_END segment before the consumer retires it.
  std::shared_ptr<SegParts> endParts;
  for (const Token& t : g_tokens)
    if (t.dwords == 23u && !t.chained) {
      std::lock_guard<std::mutex> lk(g_segMu);
      auto it = g_segById.find(t.id);
      if (it != g_segById.end() && it->second->bracketGen) endParts = it->second;
    }
  std::vector<Item2> steps;
  if (endParts)
    for (const SegPart& p : endParts->parts) {
      if (p.kind == 2u) steps.push_back({0u, p.off, 0u});
      if (p.kind == 3u && p.nested) steps.push_back({1u, p.nested->phys, p.nested->dwords});
    }
  bool retired = !nat.bound.empty();
  {
    std::lock_guard<std::mutex> lk(g_segMu);
    for (const auto& sp : nat.bound)
      if (g_segById.count(sp->id) ||
          std::find(g_segDeferred.begin(), g_segDeferred.end(), sp) != g_segDeferred.end())
        retired = false;
  }
  // Hazard 9: the bracket segments' guest memory is overwritten before the consumer runs; the native replay reads
  // only what the closes owned.
  if (gateOn && cs.taken == 1)
    std::memset(host(kBuf + 0x100u), 0xEE, rd(kDev + 0x28u) + 4u - (kBuf + 0x100u));
  nat.consumed = consume(nat, kickPhysOf(nat));
  const uint64_t hitNat = ctr(g_segReplayHit) - hit1, missNat = ctr(g_segReplayMiss) - miss1;

  // ---- the SDK arm really took the SDK path ----
  const bool sdkKicked = cs.sdkKick;
  check(sdk.consumed, n + ": SDK arm consumed in order");
  if (sdkKicked) {
    check(sdk.producer.a74 == 1u && sdk.pumped && sdk.a74After == 0u && sdk.fx.kick && sdk.fx.kickHi > sdk.fx.kickLo,
          n + ": SDK arm kicked the pump (dev+0x2A74 1, interpreter to the terminator, EndSync drained it)");
  } else {
    check(sdk.producer.a74 == 0u && !sdk.fx.kick, n + ": SDK arm queued no pump work (no kick)");
  }
  if (cs.tracked) {
    check(sdk.trackOwned && nat.trackOwned, n + ": the tracker owns the lists' prefix / cursor pages before the end");
    check(sdk.trackReleased && nat.trackReleased,
          n + ": the end's stores released the tracker's protection in both arms (the handler, not a host scope)");
    if (nativeResourceRoute()) {
      check(!W::enabled(), n + ": native route: the GPU write tracker is off (gww::enabled() false)");
      if (gateOn) check(memoryTotalN == 1u, n + ": native route: the lists on tracker-API pages decline once (writableRange)");
    } else if (gateOn) {
      check(memoryTotalN == 0u, n + ": no memory decline on tracker-owned list pages");
    }
    // Every memory decline the end counts is counted once more at its site, here a list's prefix / cursor
    // (BracketMemorySite), as the live sample line names it: none here; one without the tracker admission
    // (tracker_admission_run.py's mutations, which hold this assertion passing, or failing for a misattributed site).
    check(memoryN == memoryTotalN && listsN == memoryTotalN,
          n + ": a memory decline is counted once, at a list's prefix / cursor site");
  }
  if (!gateOn) {  // gate-off control: the ON window is the SDK too
    check(nat.producer == sdk.producer && nat.producer.a74 == sdk.producer.a74 && nat.replay == sdk.replay &&
              nat.fx.log == sdk.fx.log && nat.consumed,
          n + ": gate off: the ON window equals the actual SDK whole");
    check(ctr(g_brEnd) == end0 && !endParts, n + ": gate off: no BRACKET_END");
    return;
  }
  // ---- the native arm ----
  const bool wholeSdk = cs.taken == 0 || cs.taken == 3;  // declined / the SDK's own route: equal in every byte
  check(nat.consumed, n + ": native arm consumed in order");
  if (cs.taken == 0) {
    check(ctr(g_brDecl[cs.reason]) == decl0 + 1u && ctr(g_brEnd) == end0,
          n + ": declined to the SDK body (reason " + std::to_string(int(cs.reason)) + ")");
  } else if (cs.taken == 1) {
    check(ctr(g_brEnd) == end0 + 1u && endParts != nullptr, n + ": BRACKET_END published");
    if (sr::native::p2trace::g_on) {  // SR_P2_TRACE: the binding record traces exactly the bound closes (asserted
      // whenever the case expects a published end, so a declined one counts as this failure too)
      std::vector<uint64_t> ids;
      for (const auto& sp : nat.bound)
        if (sp) ids.push_back(sp->id);
      std::sort(ids.begin(), ids.end());
      ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
      std::vector<std::array<uint32_t, 3>> want;
      for (size_t i = 0; i < ids.size();) {
        size_t j = i + 1u;
        while (j < ids.size() && ids[j] == ids[j - 1u] + 1u) j++;
        want.push_back({uint32_t(ids[i]), uint32_t(j - i), endParts ? uint32_t(endParts->bracketGen) : 0u});
        i = j;
      }
      check(endParts && !want.empty() && g_brTestBinding == want,
            n + ": the trace's binding record names exactly the bound closes (first id, count, generation)");
      // the CP's lifecycle of that BRACKET_END (Astra round 1): begun (aux 6) and completed (aux 7) once, its steps
      // planned and applied, its generation - the evidence a cut bracket's tail needs (no aux 7)
      std::vector<std::array<uint32_t, 4>> life;
      if (endParts)
        for (uint32_t aux : {6u, 7u})
          life.push_back({aux, uint32_t(endParts->id), endParts->bracketSteps, uint32_t(endParts->bracketGen)});
      check(endParts && nat.consumed && g_brTestLifecycle == life,
            n + ": the CP traced the BRACKET_END's begin and completion (id, steps, generation)");
      // its replay plan, traced at the publish: every step in order (aux 8 a bin select, aux 9 a bound application)
      std::vector<std::array<uint32_t, 4>> plan;
      if (endParts) {
        uint32_t s = 0;
        for (const SegPart& part : endParts->parts) {
          if (part.kind == 2u) plan.push_back({8u, s++, part.off, uint32_t(endParts->bracketGen)});
          else if (part.kind == 3u && part.nested)
            plan.push_back({9u, s++, uint32_t(part.nested->id), uint32_t(endParts->bracketGen)});
        }
      }
      check(endParts && plan.size() == endParts->bracketSteps && g_brTestPlan == plan,
            n + ": the trace's plan records are the BRACKET_END's replay steps in order (step, value, generation)");
    }
  } else if (cs.taken == 2) {
    check(ctr(g_brEndFailed) == fail0 + 1u && !endParts, n + ": failure branch taken natively");
  } else {
    check(ctr(g_brLiteral) == lit0 + 1u || (ctr(g_brEnd) == end0 && !endParts), n + ": the SDK's own route after a late mode flip");
  }
  diffBytes(n + " device", sdk.producer.device, nat.producer.device, kDev);
  diffBytes(n + " buffer", sdk.producer.buffer, nat.producer.buffer, kBuf);
  diffBytes(n + " ring", sdk.producer.ring, nat.producer.ring, kRing);
  diffBytes(n + " stack", sdk.producer.stack, nat.producer.stack, kStack - 0x2000u);
  if (wholeSdk) diffLog(n + " effects (sdk | native)", sdk.fx.log, nat.fx.log);
  check(nat.producer.device == sdk.producer.device && nat.producer.fb == sdk.producer.fb,
        n + ": device equals the actual SDK (dev+0x2A74 compared apart)");
  check(nat.producer.buffer == sdk.producer.buffer, n + ": command buffer, projections and lists equal the actual SDK");
  check(nat.producer.ring == sdk.producer.ring, n + ": ring descriptors equal the actual SDK");
  check(nat.producer.stack == sdk.producer.stack && nat.producer.regs == sdk.producer.regs && nat.r3 == sdk.r3,
        n + ": stack window, return and ABI registers equal the actual SDK");
  if (wholeSdk) {
    check(nat.producer.a74 == sdk.producer.a74 && nat.a74After == sdk.a74After && nat.replay == sdk.replay &&
              nat.fx.log == sdk.fx.log,
          n + ": the SDK path whole: dev+0x2A74, pump replay and consumer effects identical");
    return;
  }
  check(nat.producer.a74 == 0u && nat.a74After == 0u, n + ": dev+0x2A74 never moved (nothing to drain)");
  if (cs.taken == 2) {
    check(nat.fx.log == sdk.fx.log && retired, n + ": failed bracket dropped as the SDK drops it; its entries retired");
    return;
  }
  // The replay: the SDK pump's ring items (up to its last bin select) against BRACKET_END's steps.
  std::vector<Item2> sdkReplay = sdk.replay;
  while (!sdkReplay.empty() && sdkReplay.back().kind != 0u) sdkReplay.pop_back();
  check(!steps.empty() && steps == sdkReplay, n + ": replay sequence equals the actual SDK pump (BS values, segments)");
  uint32_t bsCount = 0;
  for (const Item2& s : steps) bsCount += s.kind == 0u;
  check(bsCount == 4u && steps.front() == Item2{0u, 0x15555555u, 0u} && steps.back() == Item2{0u, 0xFFFFFFFFu, 0u},
        n + ": four bin selects, depth first, neutral last");
  Effects trimmed = sdk.fx;
  if (trimmed.kick) trimmed.log.erase(trimmed.log.begin() + ptrdiff_t(trimmed.kickLo), trimmed.log.begin() + ptrdiff_t(trimmed.kickHi));
  diffLog(n + " effects without the kick (sdk | native)", trimmed.log, nat.fx.log);
  check(nat.fx.log == trimmed.log, n + ": consumer effects equal the actual SDK (its kick block apart)");
  // REG4 = cb, REG5 = ctx (the list head's view), REG0 = 4, INTERRUPT 4 (answered), REG4 = 0x0BADF00D.
  const uint32_t view = phys(rd(kDev + 0x32C8u)) - 0x40000000u;
  bool kickEffects = sdk.fx.kickHi - sdk.fx.kickLo == 5u;
  if (kickEffects) {
    const auto* e = &sdk.fx.log[sdk.fx.kickLo];
    kickEffects = e[0][0] == 1u && e[0][2] == 0x82110B00u && e[1][0] == 1u && e[1][2] == view && e[2][0] == 1u &&
                  e[2][2] == 4u && e[3][0] == 5u && e[3][1] == 4u && e[3][2] == 4u && e[4][0] == 1u &&
                  e[4][2] == 0x0BADF00Du;
  }
  check(kickEffects, n + ": the SDK kick's own effects: REG4/5/0 write-backs, INTERRUPT 4, the 0x0BADF00D tail");
  bool interrupts = true;
  for (const auto& e : nat.fx.log) interrupts &= e[0] != 5u;
  check(interrupts, n + ": no interrupt on the native path");
  uint64_t pumpSegments = 0;
  for (const Item2& s : sdk.replay) pumpSegments += s.kind == 1u;
  check(retired && hitNat == 0u && missNat == 0u && hitSdk == pumpSegments && pumpSegments > 0u,
        n + ": bound entries retired; the native arm never used the deferred lookup the SDK pump needed");
  check(ctr(g_brConsumed) == cons0 + 1u, n + ": BRACKET_END consumed once");
}

// ---- Mixed per-present traffic (SR_AB_TOGGLE=SR_NATIVE_BRACKET in a live roll) --------------------------------------
// Frames run back to back with the A/B window flipping at every present (and, in some plans, between a bracket's
// begin and its end), the emulated CP lagging the producer as a real CP does: an OFF bracket's kick is executed, its
// interrupt queues the pump (the ISR's 82110B00), and the pump - the actual marker interpreter 82110460 and EndSync
// 82110768, continuing through chained blocks - publishes the list's records and its chained segments to the ring,
// which the same CP then applies. Every close carries an unpredicated marker store; at the end every marker must have
// reached the consumer (no segment lost in a list no pump walks), dev+0x2A74 must be drained and fb+0 must hold the
// last close's S. The SDK-only reference runs the same plan with every window OFF.
constexpr uint32_t kMarks = kFb + 0x800u;  // 256 marker slots
uint32_t g_markN = 0;
void markContent() {
  const uint32_t id = g_markN++;
  const uint32_t at = rd(kDev + 0x28u) + 4u;
  const uint32_t w[] = {0xC0025800u, 3u, phys(kMarks + 4u * (id & 255u)) | 2u, 0x5A000000u | id};
  for (uint32_t i = 0; i < 4u; i++) wr(at + 4u * i, w[i]);
  wr(kDev + 0x28u, at + 12u);
}

struct MixCp {
  std::unique_ptr<sr::gfx::Pm4Engine> eng;
  Effects fx;
  uint32_t rp = 0;
  size_t tok = 0;
  bool ok = true;
  uint32_t pumps = 0;
};
MixCp* g_mcp = nullptr;

void mixPump(uint32_t ctx) {  // the pump thread's work for one kick: 82110840's walk, as sdkPump, through chained blocks
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
      g_mcp->pumps++;
      return;
    }
    at = e.r3.u32;  // a chained continuation: walk on
  }
  g_mcp->ok = false;
}

// The CP up to ring index `until`: IB items by the close's hand-off token, else the deferred lookup (a pump-published
// record or chained segment), else the literal PM4; BS items; a kick's interrupt runs its pump once the IB is done.
void mixConsume(uint32_t until) {
  MixCp& m = *g_mcp;
  g_fx = &m.fx;
  g_eng = m.eng.get();
  for (uint32_t guard = 0; m.ok && m.rp != until && guard < 4096u; guard++) {
    const uint32_t hdr = rd(kRing + 4u * m.rp);
    if (hdr == 0xC0013F00u) {
      const uint32_t p = rd(kRing + 4u * (m.rp + 1u)), n = rd(kRing + 4u * (m.rp + 2u));
      while (m.tok < g_tokens.size() && g_tokens[m.tok].chained) m.tok++;
      uint64_t id = 0;
      if (m.tok < g_tokens.size() && g_tokens[m.tok].phys == p && g_tokens[m.tok].dwords == n) id = g_tokens[m.tok++].id;
      else id = segmentPublishId(p, n);
      if (id) {
        if (!cpSegment(*m.eng, id, p, n)) m.ok = false;
      } else {
        m.eng->execute(host(0x80000000u | p), size_t(n) * 4u, 1);
      }
      m.rp += 3u;
    } else if (hdr == 0xC0006200u) {
      m.eng->applyBinReg(2u, rd(kRing + 4u * (m.rp + 1u)));
      m.rp += 2u;
    } else {
      m.ok = false;
    }
    while (m.ok && !g_pumpQueue.empty()) {
      const uint32_t ctx = g_pumpQueue.front();
      g_pumpQueue.erase(g_pumpQueue.begin());
      mixPump(ctx);
      g_fx = &m.fx;
      g_eng = m.eng.get();
    }
  }
  if (m.fx.bad || m.eng->nativeRecordFailed) m.ok = false;
}

struct MixPlan {
  const char* name;
  int frames = 8;
  bool alternate = true;  // the window flips at every present (false: every window OFF - the SDK reference)
  int lag = 1;            // the CP is this many frames behind the producer (0: it keeps up)
  int midToggle = 0;      // a present between BeginZPass and EndZPass: 0 never, 1 every frame, 2 every other frame
  bool expectPending = false, expectNative = false;
};
struct MixOutcome {
  uint32_t marks = 0, seen = 0, lastS = 0, lastFence = 0, a74 = 0;
  bool consumed = false;
  uint64_t ends = 0, pending = 0, noGen = 0, pumps = 0;
};

MixOutcome runMixed(const MixPlan& pl, int abBit) {
  for (int i = 0; i < 2 && sr::abt::on(abBit); i++) sr::abt::onGuestSwap();  // every plan starts in an OFF window
  Case none{};
  prepare(none);
  g_markN = 0;
  std::memset(host(kMarks), 0, 0x400u);
  MixCp m;
  m.eng = std::make_unique<sr::gfx::Pm4Engine>();
  m.eng->submitDraws = false;
  m.eng->cpMode = true;
  m.eng->physToHost = fxMap;
  m.eng->cp.user = &m.fx;
  m.eng->cp.interrupt = fxInterrupt;
  m.eng->cp.running = fxRunning;
  m.eng->cp.trace = fxTrace;
  m.eng->binMask = ~0ull;
  m.eng->binSelect = ~0ull;
  g_mcp = &m;
  g_fx = &m.fx;
  g_eng = m.eng.get();
  m.eng->writeReg(0x1DCu, 0x00020033u);
  m.eng->writeReg(0x1DDu, phys(kPc));
  g_mixed = true;
  g_pumpQueue.clear();
  const uint64_t end0 = ctr(g_brEnd), pend0 = ctr(g_brDecl[kBrPending]), nogen0 = ctr(g_brDecl[kBrNoGen]);
  auto present = [&] {
    sr::abt::onGuestSwap();
    if (!pl.alternate) sr::abt::onGuestSwap();  // the reference: back to OFF at once
  };
  std::vector<uint32_t> frameEnd;
  for (int f = 0; f < pl.frames; f++) {
    markContent(); closeNow();  // before the bracket
    markContent(); closeNow();
    PPCContext c = context();
    c.r3.u64 = kDev;
    c.r4.u64 = 0u;
    call(0x8210DF38u, c);       // BeginZPass
    markContent(); closeNow();  // pinned
    markContent(); closeNow();
    if (pl.midToggle == 1 || (pl.midToggle == 2 && (f & 1))) present();
    markContent();              // EndZPass's own flush
    c = context();
    c.r3.u64 = kDev;
    call(0x8210E1A0u, c);       // EndZPass -> InvokeRenderPass -> 82110C78
    markContent(); closeNow();  // after the kick
    markContent(); closeNow();
    present();
    frameEnd.push_back(rd(kDev + 0x2A44u));
    if (pl.lag == 0) mixConsume(rd(kDev + 0x2A44u));
    else if (f >= pl.lag) mixConsume(frameEnd[size_t(f - pl.lag)]);
  }
  for (int guard = 0; guard < 64 && m.ok; guard++) {  // drain: the CP and every queued pump to quiescence
    const uint32_t w = rd(kDev + 0x2A44u);
    if (m.rp == w && g_pumpQueue.empty()) break;
    mixConsume(w);
  }
  MixOutcome o;
  o.marks = g_markN;
  std::vector<bool> seen(g_markN, false);
  for (const auto& e : m.fx.log) {
    if (e[0] != 0u || e[1] < phys(kMarks) || e[1] >= phys(kMarks) + 4u * 256u) continue;
    const uint32_t id = e[2] & 0xFFFFFFu;
    if ((e[2] >> 24) == 0x5Au && id < g_markN && (e[1] - phys(kMarks)) / 4u == (id & 255u)) seen[id] = true;
  }
  for (bool b : seen) o.seen += b;
  for (const auto& e : m.fx.log)
    if (e[0] == 0u && e[1] == phys(kFb)) o.lastFence = e[2];
  o.lastS = rd(kDev + 0x2A1Cu) - 2u;
  o.a74 = rd(kDev + 0x2A74u);
  o.consumed = m.ok && m.rp == rd(kDev + 0x2A44u) && g_pumpQueue.empty();
  o.ends = ctr(g_brEnd) - end0;
  o.pending = ctr(g_brDecl[kBrPending]) - pend0;
  o.noGen = ctr(g_brDecl[kBrNoGen]) - nogen0;
  o.pumps = m.pumps;
  g_mixed = false;
  g_mcp = nullptr;
  g_fx = nullptr;
  g_eng = nullptr;
  return o;
}

void runMixedPlan(const MixPlan& pl, bool gateOn, int abBit) {
  const std::string n = std::string("mixed ") + pl.name;
  const MixOutcome o = runMixed(pl, abBit);
  if (o.seen != o.marks || !o.consumed || o.a74 != 0u || o.lastFence != o.lastS)
    std::printf("[BRACKET_DIAG] %s: marks %u/%u consumed %d dev+0x2A74 %u fence %08X/S %08X ends %llu pending %llu "
                "noGen %llu pumps %llu\n", n.c_str(), o.seen, o.marks, int(o.consumed), o.a74, o.lastFence, o.lastS,
                (unsigned long long)o.ends, (unsigned long long)o.pending, (unsigned long long)o.noGen,
                (unsigned long long)o.pumps);
  check(o.consumed, n + ": the CP and every pump ran to quiescence in order");
  check(o.marks > 0u && o.seen == o.marks, n + ": every close's marker reached the consumer (no segment lost)");
  check(o.a74 == 0u, n + ": dev+0x2A74 drained to 0");
  check(o.lastFence == o.lastS, n + ": fb+0 holds the last close's S");
  if (gateOn && pl.expectPending)
    check(o.pending > 0u, n + ": BRACKET_END declined while an OFF bracket's pump work was outstanding");
  if (gateOn && pl.expectNative) check(o.ends > 0u, n + ": brackets ended natively between the OFF ones");
}
}  // namespace bt

int runNativeBracketTest(const char* mode) {
  namespace K = sr::kernel;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  {  // a bare run re-runs itself with exactly the runner's configuration for the mode (tools/difftest/suite_bare.inc)
    const bool off = std::strcmp(mode, "gate-off") == 0, seg = std::strcmp(mode, "segmentless-off") == 0;
    const bare::Conf conf = {{L"SR_PM4_RING", L"0"}, {L"SR_PM4_RING_EXEC", L"0"}, {L"SR_PM4_CP", L"0"},
                             {L"SR_ALLOW_FORCING", L"0"}, {L"SR_AB_TOGGLE", L"SR_NATIVE_BRACKET"}, {L"SR_AB_PRESENT", L"1"},
                             {L"SR_NATIVE_SEGMENTLESS", seg ? L"0" : L"1"}, {L"SR_NATIVE_FLUSH", L"1"},
                             {L"SR_NATIVE_BRACKET", off ? L"0" : L"1"}, {L"SR_NATIVE_TRANSPORT", L"0"}};
    int code = 0;
    if (bare::handled("NATIVE_BRACKET_TEST", "--native-bracket-test", mode, L"SR_NATIVE_BRACKET", conf, code))
      return code;
  }
  // "gate-off": SR_NATIVE_BRACKET=0; "segmentless-off": the gate on without P2-B segmentless (every begin declines).
  const bool segOff = std::strcmp(mode, "segmentless-off") == 0;
  const bool gateOn = std::strcmp(mode, "gate-off") != 0 && !segOff;
  K::registerStubs();
  sr::guest::initializeDispatchTable();
  K::kpcrBindCurrent(bt::kStack, bt::kStack - 0x10000u, 3);
  const int abBit = sr::abt::reg("SR_NATIVE_BRACKET");
  bt::check(bracketGate() == (gateOn || segOff) && g_on && g_segmentless == !segOff && abBit >= 0 && sr::abt::active(),
            "requested gate, record mode, segmentless and the per-present A/B");
  bt::check(PPC_LOOKUP_FUNC(K::getGuestBase(), 0x82110B30u) == sub_82110B30 &&
                PPC_LOOKUP_FUNC(K::getGuestBase(), 0x82110C78u) == sub_82110C78,
            "production dispatch resolves the BRACKET begin / end overrides");
  if (bt::failed) {
    std::printf("[NATIVE_BRACKET_TEST] %d passed, %d failed\n", bt::passed, bt::failed);
    return 1;
  }
  auto& registry = K::ExportRegistry::instance();
  const K::ExportFn origSet = registry.getExport("KeSetEvent"), origReset = registry.getExport("KeResetEvent"),
                    origWait = registry.getExport("KeWaitForSingleObject");
  registry.registerExport("KeSetEvent", bt::setEvent);
  registry.registerExport("KeResetEvent", bt::resetEvent);
  registry.registerExport("KeWaitForSingleObject", bt::waitEvent);
  const size_t index = (bt::kAllocCb - PPC_CODE_BASE) >> 2;
  PPCFunc* const saved = K::g_dispatchTable[index];
  K::g_dispatchTable[index] = bt::bracketTestAllocate;
  setSegmentPublicationObserver(bt::observe);
  if (!g_arena) g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRecArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

  using C = bt::Case;
  std::vector<C> cases;
  auto add = [&](C c) { cases.push_back(c); };
  {C c; c.name = "plain"; add(c);}
  {C c; c.name = "records"; c.records = true; add(c);}
  {C c; c.name = "neutral"; c.neutral = true; add(c);}
  {C c; c.name = "range"; c.range = 1u; add(c);}
  {C c; c.name = "whole"; c.whole = true; add(c);}
  {C c; c.name = "range-whole"; c.range = 1u; c.whole = true; c.records = true; add(c);}
  {C c; c.name = "all-ones-range"; c.range = 2u; add(c);}
  {C c; c.name = "post-kick"; c.postKick = true; c.neutral = true; add(c);}
  {C c; c.name = "direct"; c.direct = true; c.range = 1u; add(c);}
  if (!bt::nativeResourceRoute()) {C c; c.name = "tracked-lists"; c.direct = true; c.range = 1u; c.tracked = true; add(c);}
  else {C c; c.name = "tracked-lists-native"; c.direct = true; c.range = 1u; c.tracked = true; c.taken = 0;
        c.reason = kBrMemory; add(c);}
  {C c; c.name = "single-record"; c.closes = 0u; add(c);}
  {C c; c.name = "list-overflow"; c.closes = 540u; add(c);}
  {C c; c.name = "kick-failed"; c.direct = true; c.failBytes = 400u; c.taken = 2; c.sdkKick = false; add(c);}
  {C c; c.name = "hole-failed"; c.direct = true; c.range = 1u; c.failBytes = 128u; c.taken = 2; c.sdkKick = false; add(c);}
  {C c; c.name = "coherency-failed"; c.direct = true; c.range = 1u; c.failBytes = 44u; c.taken = 2; c.sdkKick = false; add(c);}
  {C c; c.name = "pre-failed"; c.direct = true; c.preFailed = true; c.taken = 2; c.sdkKick = false; add(c);}
  {C c; c.name = "late-no-gpu"; c.direct = true; c.range = 1u; c.lateNoGpu = 128u; c.taken = 3; c.sdkKick = false; add(c);}
  {C c; c.name = "poisoned"; c.poison = true; c.closes = 3u; c.taken = 0; c.reason = kBrPoisoned; add(c);}
  {C c; c.name = "double-depth-marker"; c.doubleZ = true; c.taken = 0; c.reason = kBrWalk; add(c);}
  {C c; c.name = "duplicated-record"; c.closes = 3u; c.dupRecord = true; c.taken = 0; c.reason = kBrBind; add(c);}
  {C c; c.name = "already-published"; c.published = true; c.taken = 0; c.reason = kBrPublished; add(c);}
  const uint64_t begin0 = bt::ctr(g_brBegin), beginDecl0 = bt::ctr(g_brBeginDecl), abOff0 = bt::ctr(g_brBeginAbOff);
  for (const C& c : cases) bt::runCase(c, gateOn, abBit);
  if (segOff)  // the native arms' begins declined (BRACKET requires segmentless), the SDK arms' were A/B-off windows
    bt::check(bt::ctr(g_brBegin) == begin0 && bt::ctr(g_brBeginDecl) == beginDecl0 + cases.size() &&
                  bt::ctr(g_brBeginAbOff) == abOff0 + cases.size(),
              "segmentless off: every begin declined, no bracket opened");
  // Mixed per-present traffic: the reference (every window OFF, the SDK), then alternating windows with the CP keeping
  // up and lagging, and with a present between a bracket's begin and its end.
  {
    using P = bt::MixPlan;
    std::vector<P> plans;
    {P p; p.name = "sdk-reference-lag"; p.alternate = false; plans.push_back(p);}
    {P p; p.name = "alternate-prompt"; p.lag = 0; p.expectNative = true; plans.push_back(p);}
    {P p; p.name = "alternate-lag"; p.expectPending = true; plans.push_back(p);}
    {P p; p.name = "alternate-lag2"; p.lag = 2; p.expectPending = true; plans.push_back(p);}
    {P p; p.name = "toggle-inside-bracket"; p.midToggle = 1; p.lag = 0; plans.push_back(p);}
    {P p; p.name = "toggle-inside-bracket-lag"; p.midToggle = 2; plans.push_back(p);}
    for (const P& p : plans) bt::runMixedPlan(p, gateOn, abBit);
  }
  // Last: the graceful quit's ring cut mid-bracket (its dropped segments are never consumed).
  if (gateOn && !segOff) bt::runRingCut(abBit);

  setSegmentPublicationObserver(nullptr);
  K::g_dispatchTable[index] = saved;
  registry.registerExport("KeSetEvent", origSet);
  registry.registerExport("KeResetEvent", origReset);
  registry.registerExport("KeWaitForSingleObject", origWait);
  std::printf("[NATIVE_BRACKET_TEST] %d passed, %d failed\n", bt::passed, bt::failed);
  return bt::failed ? 1 : 0;
}
}  // namespace sr::native::flush
