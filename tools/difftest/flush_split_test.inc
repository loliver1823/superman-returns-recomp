// Included by kernel/native_flush.cpp only in difftest (SR_DIFFTEST): p1.flush_residue (SR_NATIVE_FLUSH_SPLIT;
// docs/NATIVE_ROADMAP.md "p1.flush_residue BUILD NOTE").
//
// The actual SDK bodies against the native paths on identical fixtures that force each fallback the gate converts:
//   - S1 sections crossing the segment limit (821078C0 constants, 82107520 register runs, 82107760 fetch constants,
//     the fetch trailer both ways, a run needing several kicks, cursor % 16 variants) - the SDK chunks them through
//     GpuCmdEmitRegisterRunChunked 82107058 and GpuCommandBufferFlush 820FD8C0;
//   - draws over 0xFFFF indices (820FC000 indexed with 16- / 32-bit indices, 820FBBF8 auto-indexed; triangle list /
//     strip, line strip, quad list, rect list; a part crossing the kick threshold) - the SDK splits them by its
//     indices-per-prim table 820B3668 (written into the fixture from the retail .rdata, IDA), and a prim whose entry is 0
//     (declined before anything is written);
//   - row 9 entries (SetShaderGPRAllocation 820F5570 + ClearFullSurface, Resolve 8210C5F8, ClearF 82101A58) whose kick
//     sites kick - the threshold swept across every site - a register run crossing the limit inside them, and two
//     decline witnesses: a tiled scissor restore reached only after the post-kick allocations, and the resolve's
//     S-dependent list path behind an even S.
// Also (Astra round 1): line lists and triangle fans over 0xFFFF, both sides of the 0xFFFF / 0x10000 transition, the
// per-part index offsets under a split (a negative baseVertex with the index DMA address wrapping 2^32 BETWEEN the parts
// - asserted per part, 0xFFFFFFE0 -> 0x0001FFD8 - and an auto-indexed strip whose per-part startVertex wraps), a
// visibility query active (the viz dword of every part, byte-compared under SR_NATIVE_FLUSH_PM4), tile
// auto-predication (declined in both arms), every kick a pinned close (a Z-pass recording pin: mode-B list records,
// nothing published), the generation ring wrapping at the kick, and a tail allocation failing after a transparent row-9
// kick. And (Astra round 2) the game's instanced path itself: the GENERATED body of RwDrawInstancedConstBatched 8261B050
// called in every arm (the SDK arm with its draw flushes and SetPredication declined to the XDK bodies on the thread):
// stream 0 the mesh, stream 1 the instance-index stream (SetStreamSource 820F2A50, the stream-2 unbind), the sampler
// words and c4 it writes into the device, per batch the instance constants at c100 (SetVertexShaderConstantF 820F4920:
// the production leaf in every arm) and DrawVertices 820FBBF8 of instances x vertices-per-instance over 0xFFFF; one
// tile, two tiles (SetPredication 3 << 2t, the c100 run chunked across kicks) and two tiles with a visibility region
// each (Begin / EndRegion 82102448 / 82102608: guest PM4 of their own, counted apart). Not built: its VS / PS binds
// (8260B800 / 8260B898: shader state, no draw) and the TilePush / Pop form of the regions (a30 == 1).
// And (native/flush-residue-2, frs1's stress_snap / rec_chk) the GPU a generation behind: each row-9 entry's tail
// allocation waits in 820FCB30 (the SDK body, the OFF arm's guest body, the ON arm's live pass inside its Entry) while a
// helper thread plays the CP; the record pre-pass models the wait under the gate ('W'), and a transparent kick followed
// by the wait (the live kick's next segment waits inside the Entry).
// Allocators: the device callback (dev+0x3354's object) and the game's generation ring (820FCBE8: tail carves past the
// segment, the next segment after them). The base cases (52) run on both; the later-kick sweep (22), the ring-wrap
// cases (7) and the GPU-wait cases (4) on the ring only (on the callback an allocation before the kick declines the
// pre-pass - "alloc via the device callback", a fixture path the game never takes - and the callback never waits), the
// allocation-failure cases (3) on the callback only: 140 in all, 55 callback + 85 ring. The .rdata the bodies read (the split table, ClearF / GPR ucode, the pixel-centre constants,
// 8261B050's 0.0f) is written into the fixture from the retail values (IDA).
// Three arms per fixture, each on a freshly prepared device: SDK = the guest body called directly (__imp__, the constant
// dedup suspended as the dry CHECK does: the plain XDK leaf); ON = production dispatch with SR_NATIVE_FLUSH_SPLIT's
// per-present A/B window on; OFF = the same with the window off (today's path: the guest leaf / body). The closes are
// the native segmentless close in every arm. After the arm the open segment is closed and every SEGMENT the close
// published (the publication observer: the hand-off the ringless override takes) is
//   - rendered to PM4 part by part - gap parts as the guest bytes, record parts through cpRecordPm4 (the CP's own CPPM4
//     render-back) and, for row 9's opaque kinds, their registered renderer - and parsed into a canonical event list:
//     register writes in order, draws {predicate, initiator, DMA base, size}, every other type-3 packet with its body
//     (type-2 and type-3 NOPs dropped: no effect);
//   - applied through cpSegment by an in-order Pm4Engine (the consumer; CPPM4 mode renders records back there).
// Compared, ON and OFF each against SDK: the published segments (phys, dwords) in order, the device (dev+0..0x5000),
// the ring, the allocator, the canonical list (exactly, or - where the constant dedup leaves resident groups out - the
// register state at every draw), the consumer's register file and draw count, the tail allocations' contents (the
// vertex data); under SR_NATIVE_FLUSH_PM4 the command buffer byte for byte. (In records mode today's guest constant
// leaf writes the deduped PM4 - its cursor is not the SDK's - so arms on today's path are held to the per-draw state.) Wiring: the ON arm's segments hold no guest PM4 packet (gap packets 0; the offline form of "the
// S1 flush's fallback packets 0") and its split counters moved while sectionFallbacks / count>0xFFFF did not; the OFF
// arm shows the fallback (the fixture forces what it says). Modes (one process each; the gates latch):
//   records      the default path (segmentless, constant dedup ON): the draw-state comparison
//   records-raw  SR_NATIVE_CONST_DEDUP=0: the canonical lists equal exactly
//   cppm4        SR_NATIVE_FLUSH_CPPM4=1 + SR_NATIVE_CONST_DEDUP_CHECK=1: the consumer renders every record back to PM4
//                (an OFF arm whose guest constant leaf predicts a resident group writes the CHECK's 'DDCK' marks:
//                Fx::ddHit, asserted, that arm held to the per-draw state)
//   pm4          SR_NATIVE_FLUSH_PM4=1 + SR_NATIVE_CONST_DEDUP=0: literal spans, the command buffer equals the SDK's
//   gate-off     SR_NATIVE_FLUSH_SPLIT=0: both native arms are today's path (the control: nothing converted)
//   zpass-leaf   records-raw + SR_DIAG_ZPASS_LEAF=1 (C1 stage 1, L3b's lever): on the pinned fixtures (a Z-pass
//                recording) the ON arm's S1 sections at the segment limit go to the guest leaf - their PM4 a gap of the
//                pinned segments, the layout the SDK's - while every unpinned fixture still chunks natively and the
//                draws over 0xFFFF / row 9 stay converted inside the recording too; the lever's counter moves on exactly
//                the pinned section fixtures. In every other mode it must never move (the gate unset: inert).
// SR_NATIVE_TILERECTS=0 in every mode keeps the SDK arm's nested 820F0208 (row 9's scissor restore) the guest's PM4 (the
// native row-9 port has its own scissor items; the S1 flush never calls it). Run with SR_PM4_RING=0 SR_PM4_RING_EXEC=0
// SR_PM4_CP=0 (the runner's configuration; a bare run re-runs itself with it, tools/difftest/suite_bare.inc).
#include <chrono>  // Fx::gpuWait's helper thread (std::thread: the TU's own <thread>)
#include "kernel/kpcr.hpp"
#include "guest/guest_harness.hpp"
#include "gfx/xenos_state.hpp"

extern "C" void __imp__sub_820FC000(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820FBBF8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_820F5570(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_8210C5F8(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_82101A58(PPCContext& __restrict, uint8_t*);
namespace sr::native::recb {
uint32_t renderBytes(const uint32_t* w, uint32_t nWords);  // kernel/native_rec_b.cpp (the op list's PM4 length)
}  // recCounters / kRecCounters: kernel/native_rec_b.hpp

namespace sr::native::flush {
namespace fsp {
namespace K = sr::kernel;
constexpr uint32_t kDev = 0x70800000u, kCell = 0x70806000u, kObject = 0x70806800u, kStack = 0x70780000u;
constexpr uint32_t kIbObj = 0x70807000u, kTex = 0x70807100u, kSurf = 0x70807200u;
// The pinned closes' scratch list (dev+0x32C8: +8 cursor, +0xC limit) and the tile auto-predication list (dev+12960:
// +8 cursor, +0xC limit), each with room for every fixture's entries; compared between the arms.
constexpr uint32_t kZList = 0x70806C00u, kTileList = 0x70807800u, kListBytes = 0x400u;
// ClearF's IM_LOAD_IMMEDIATE ucode (.rdata 820BA0D0 VS 27 dwords, 820B59DC PS 9 dwords): the fixture writes a pattern
// (the difftest image holds no .rdata; every arm copies the same bytes).
constexpr uint32_t kVsUcode = 0x820BA0D0u, kPsUcode = 0x820B59DCu;
constexpr uint32_t kFb = 0x90807000u;                       // physical-window VAs
constexpr uint32_t kRing = 0x90808000u, kRingMask = 4095u;  // 16 KB ring
constexpr uint32_t kBuf = 0x90A00000u, kBufSize = 0x80000u;  // the first segment at the bottom, allocations from the top
constexpr uint32_t kSeg0 = kBuf + 0x100u;
constexpr uint32_t kIdx = 0x90900000u;  // the index buffer's data (only its address reaches the stream)
// RwDrawInstancedConstBatched 8261B050's inputs (Op 'I'): the render object, its two vertex buffers, the per-tile
// instance counts / constant pointers / region lists, then the instance constants (kInstConst..)
constexpr uint32_t kInst = 0x70808000u, kInstConst = kInst + 0x1000u, kInstBytes = 0x5000u;
// Fx::gpuWait: how long the helper thread playing the CP holds the GPU a generation behind (every arm's first tail
// allocation reaches 820FCB30 within microseconds; the accumulators prove each one waited)
constexpr uint32_t kGpuWaitMs = 30;
constexpr uint32_t kRwDevice = 0x8299ADF4u, kRwStreams = 0x8299ABF8u, kRwVbCache = 0x8299AC00u, kFlt0 = 0x820013B8u;
constexpr uint32_t kAllocCb = 0x82110800u;
constexpr uint32_t kHsio = 0x829AC0ECu;
// The retail .rdata table at 820B3668 (IDA read_memory_bytes): {indices per primitive, overlap} for prims 0..15. The
// difftest image holds no .rdata, so the fixture writes it (both arms read it from guest memory).
constexpr uint32_t kTable[16][2] = {{0, 0}, {1, 0}, {2, 0}, {1, 1}, {3, 0}, {1, 2}, {1, 2}, {0, 0},
                                    {3, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {4, 0}, {0, 0}, {0, 0}};

int passed = 0, failed = 0;
void check(bool ok, const std::string& name) {
  std::printf("%s%s\n", ok ? "  ok  " : "FAIL: ", name.c_str());
  ok ? ++passed : ++failed;
}
uint32_t rd(uint32_t va) { return K::readGuestU32BE(va); }
void wr(uint32_t va, uint32_t v) { K::writeGuestBE32(va, v); }
void wr64(uint32_t va, uint64_t v) {
  wr(va, uint32_t(v >> 32));
  wr(va + 4u, uint32_t(v));
}
uint8_t* host(uint32_t va) { return K::guestVAToHost(va); }

// The allocator callback (object dev+0x3354 +172, a supported SDK mechanism; both the tail allocations of 820FCDF0 and
// the new segments of 820FCF90): bump allocations down from the top of the buffer.
uint32_t g_top = 0;
// The tail allocations (mode 1: 820FCDF0's vertex data) the callback handed out, {va, bytes} in order: their contents
// are compared between the arms after the consumer ran.
std::vector<std::pair<uint32_t, uint32_t>> g_allocs;
bool g_tailFails = false;  // Fx::tailFailsAfterKick: refuse mode 1 once a segment (mode 2) was handed out
uint32_t g_segAllocs = 0, g_tailRefused = 0;
PPC_FUNC(flushSplitAllocate) {
  if (ctx.r4.u32 == 2u) g_segAllocs++;
  if (ctx.r4.u32 == 1u && g_tailFails && g_segAllocs) {
    g_tailRefused++;
    ctx.r3.u64 = 0;
    return;
  }
  const uint32_t bytes = rd(ctx.r5.u32), align = ctx.r6.u32 ? ctx.r6.u32 : 4u;
  g_top = (g_top - bytes) & ~(align - 1u);
  if (ctx.r4.u32 == 1u) g_allocs.push_back({g_top, bytes});
  ctx.r3.u64 = g_top;
}

struct Pub {
  uint64_t id;
  uint32_t phys, dwords;
  bool operator==(const Pub& o) const { return phys == o.phys && dwords == o.dwords; }  // ids differ per arm
};
std::vector<Pub> g_pub;
uint32_t g_chained = 0;
void observe(uint64_t id, uint32_t phys, uint32_t dwords, bool chained) {
  g_pub.push_back(Pub{id, phys, dwords});
  if (chained) g_chained++;
}

PPCContext context() {
  PPCContext c{};
  c.r1.u64 = kStack;
  c.r13.u64 = K::kpcrCurrent();
  return c;
}

// ---- the fixtures ----
struct Op {
  char t = 'D';        // D a draw (820FC000 / 820FBBF8), G SetShaderGPRAllocation 820F5570, R Resolve 8210C5F8,
                       // C ClearF 82101A58 (RT0 colour, the whole surface, the local zero colour), I the game's
                       // RwDrawInstancedConstBatched 8261B050 (its generated body; the fields below)
  int kind = 0;        // D: 0 indexed (820FC000), 1 auto-indexed (820FBBF8)
  uint32_t prim = 4, a = 0, b = 0, count = 3;  // D: kind 0 {baseVertex, startIndex}, kind 1 {startVertex}
  uint64_t q[5] = {};  // the dirty qwords dev+0x00..0x20 written before the call (D: the flush's sections; G / R: q[2..4]
                       // are dev+0x10 / 0x18 / 0x20, the row-9 ports' phaseC masks)
  // I: vertices per instance (render object +0xC), constant registers per instance (r7: 128 / regs instances a
  // batch), the instances of each tile (r4's array; r6 = the tile count, > 1: SetPredication 3 << 2t per tile) and
  // one visibility region per tile (r10 = 1, the stack's a28 lists: Begin / EndRegion around the tile's batches)
  uint32_t vpi = 0, regs = 1;
  std::vector<uint32_t> inst;
  bool regions = false;
};
// What a fixture must show (the arms' wiring; 'c' 821078C0 chunked, 'r' 82107520 chunked, 'f' 82107760 chunked, 't' the
// slow fetch trailer, 'b' a split draw, 'k' a split draw's part kicked, 'g' a row-9 entry taken through a transparent kick,
// 'd' a row-9 entry declined by its pre-pass (the ON arm runs the guest body too), 'n' a draw of at most 0xFFFF indices
// taken natively by both arms (no split, no decline), 'p' a draw declined by tile auto-predication in both arms (the
// guest body's predicated per-tile form), 'w' the SDK arm wrapped the generation ring, 'x' no gate effect expected,
// 'C' as 'c' for draws over 0xFFFF only - today's path declines them whole, before any section, so no abort is claimed,
// 'W' a row-9 entry whose tail allocation waits for the GPU: taken through the record pre-pass's modelled wait under
// the gate (waitT 1), declined by today's stop otherwise).
struct Fx {
  std::string name;
  uint32_t used = 0;        // bytes of guest type-2 NOPs already in the open segment (the cursor % 16 variants)
  uint32_t room = 0x4000;   // limit - cursor at the start
  uint32_t thrGap = 160;    // limit - threshold (the SDK's 160)
  uint32_t segSize = 0x2000;  // dev+13492: the size of every later segment (820FCF90 through the callback)
  uint32_t S = 0x101;       // the open submission id dev+0x2A1C (odd, as the SDK keeps it)
  uint8_t b38 = 0;          // byte dev+0x2A38 (0x10: predicated tiling - the tiled restore)
  bool ib32 = false;        // 32-bit indices
  bool listFull = false;    // R: tex+0 = 1, dev+0x2A20 = 1, dev+0x335C == dev+0x3360 (the S == 0 path chains a list)
  bool rt0 = false;         // C: RT0 bound (a 64x32 surface), the viewport 64x32
  // The allocator: false = the device callback (dev+0x3354 object +172: bump allocations down from the buffer's top;
  // the row-9 pre-pass declines an allocation made before any kick there, "alloc via the device callback"); true = the
  // game's generation ring (dev+0x3354 = 0: 820FCBE8 mode 1 carves past the segment, mode 2 places the next segment
  // after it; the fence progress word's generation equals dev+13500's, so 820FCB30 never waits).
  bool ring = false;
  // Ring wrap (ring only): the buffer ends 64 bytes past the open segment's limit, so the first new segment wraps to
  // the buffer's start (820FCBE8 mode 2: generation + 1, byte dev+0x2A3A |= 4 - the next close invalidates the whole
  // buffer); the progress word's cursor bits all ones, so 820FCB30 waits for neither generation.
  bool wrap = false;
  // A Z-pass recording pin (dev+0x3290 / 0x3294, 820FCBE8's pin one generation ahead): every close is a PINNED close
  // (mode B: a {0x82000000 | n, phys} record in the scratch list dev+0x32C8, the parts registered deferred, nothing
  // published); the arms' list records and their segments are compared instead of the publications.
  bool pinned = false;
  // The device callback refuses every tail allocation (mode 1) once it has handed out a segment (mode 2): a row-9
  // entry's allocation after its kick fails in the live pass (the XDK's failure branch, the sticky byte dev+0x2A39
  // 0x40, the scratch segment dev+15152 after it). Callback allocator only.
  bool tailFailsAfterKick = false;
  // The GPU a generation behind (ring only; frs1's stress_snap / rec_chk decline "alloc would wait for the GPU"): the
  // progress word reads generation 0, so the first tail allocation's 820FCB30 waits; a helper thread playing the CP
  // writes generation 1 after kGpuWaitMs. Every arm waits (the SDK body, the OFF arm's guest body, the ON arm's live
  // pass inside its Entry); the wait-time accumulators dev+20024..20031 must be > 0 in each and are masked from the
  // device comparison (timing). Under the gate the ON arm's pre-pass models the wait ('W': waitT 1).
  bool gpuWait = false;
  uint8_t vizId = 0;        // dev+11915 (a visibility query active): every DRAW_INDX's viz dword 0x100 | dev[11850 + id]
  bool tilePred = false;    // byte dev+10808 |= 2: tile auto-predication (the S1 flush declines; the per-tile list)
  std::vector<Op> ops;
  std::string expect;       // wiring letters (above)
  uint32_t minSegs = 1;     // segments the SDK arm must publish (> 1: the fixture kicked)
  uint32_t draws = 0;       // draw packets in the SDK arm (DRAW_INDX / DRAW_INDX_2)
  // guest PM4 packets of producers OUTSIDE the S1 flush the ON arm's segments must hold exactly (8261B050's visibility
  // regions: 82102448 / 82102608 write SET_BIN_MASK_LO / HI each, 4 packets a region); the flush's own stay 0
  uint32_t guestPk = 0;
  // asserted per draw packet in the SDK and ON arms (empty: not asserted): the DMA base, VGT_INDX_OFFSET 0x2102
  std::vector<uint32_t> dma, r2102;
  // the fixture re-sends a constant group the stream still holds (8261B050's c4 in its second tile, no kick between):
  // under cppm4 (SR_NATIVE_CONST_DEDUP_CHECK) the OFF arm's guest constant leaf (today's emitType0Dedup) announces the
  // predicted run with a type-3 NOP 'DDCK' and splits its run there - by design, a layout that is not the SDK's - so
  // that arm must show the marks and is held to the per-draw state and the consumer (as records mode holds it)
  bool ddHit = false;
};

// The device after D3DDeviceResetState's empty range, a fresh open segment and the shadows the sections read filled with
// distinct values (low two bits 0: no fetch constant reads as a vertex fetch, so the consumer's emitDraw stops early).
void prepare(const Fx& fx) {
  std::memset(host(kDev), 0, 0x5000u);
  std::memset(host(kCell), 0, 0x1000u);
  std::memset(host(kFb), 0, 0x1000u);
  std::memset(host(kRing), 0, (kRingMask + 1u) * 4u);
  std::memset(host(kBuf), 0, kBufSize);
  std::memset(host(kStack - 0x2000u), 0xA5, 0x2000u);
  std::memset(host(kIbObj), 0, 0x200u);
  wr(0x82000948u, kCell);
  wr(kCell, kDev);
  wr(kDev + 0x2A10u, kFb);
  wr(kDev + 0x2A14u, kFb + 0x80u);
  wr(kDev + 0x34A4u, kRing);
  wr(kDev + 0x34A8u, kRingMask);
  wr(kDev + 0x34ACu, kBuf);
  wr(kDev + 0x34B0u, kBuf + kBufSize);
  // the open segment: at the buffer's bottom, or (Fx::wrap) near its top so the first new segment wraps to the bottom
  const uint32_t seg0 = fx.wrap ? kBuf + 0x7000u : kSeg0;
  wr(kDev + 0x34C4u, seg0);
  for (uint32_t k = 0; k < fx.used / 4u; k++) wr(seg0 + 4u * k, 0x80000000u);  // the guest's own NOPs
  const uint32_t c0 = seg0 - 4u + fx.used, lim = c0 + fx.room;
  wr(kDev + 0x28u, c0);
  wr(kDev + 0x2Cu, lim);
  wr(kDev + 0x30u, lim - fx.thrGap);
  wr(kDev + 0x2A1Cu, fx.S);
  wr(kDev + 0x3298u, seg0 - 4u);
  wr(kDev + 0x329Cu, 1u);
  wr(kDev + 0x34BCu, 1u);
  wr(kDev + 0x3354u, kObject);  // 820FCDF0 / 820FCF90: the callback path (+152 = 0, +172 = the callback)
  std::memset(host(kObject), 0, 0x100u);
  wr(kObject + 164u, 0x11223344u);
  wr(kObject + 172u, kAllocCb);
  wr(kDev + 13492u, fx.segSize);
  wr(kDev + 13516u, 0x7FFFFFFFu);  // no early segment close (an allocation total never exceeds it)
  wr(kDev + 0x34CCu, 0x7FFFFFFFu);
  wr(kDev + 0x2D18u, 0u);  // the SDK's empty coherency range 0x00000000FFFFFFFF
  wr(kDev + 0x2D1Cu, 0xFFFFFFFFu);
  wr(kDev + 20416u, 1u);  // behaviour flag bit 0: no swap-fence throttle in 820FCF90
  if (fx.ring) {
    wr(kDev + 0x3354u, 0u);    // no device allocator object: 820FCBE8
    wr(kDev + 13496u, 0u);     // the carve top: limit + 60 first
    wr(kDev + 13504u, kBuf);   // the wrap start (never reached)
    wr(kFb + 4u, 1u);          // the GPU progress word: generation 1 (dev+13500), cursor 0
    if (fx.wrap) {
      wr(kDev + 0x34B0u, lim + 64u);  // the buffer's end just past the open segment: the next segment wraps
      wr(kFb + 4u, 0xFFFFFFFDu);     // generation 1, cursor bits all ones: no wait in generation 1 or 2
    }
    if (fx.gpuWait) wr(kFb + 4u, 0u);  // generation 0: every allocation in generation 1 waits until the helper's write
  }
  // the scratch segment 820FCF90 hands out after an allocation failure (byte dev+0x2A39 & 0x40): 4800 bytes
  wr(kDev + 15152u, kBuf + 0x60000u);
  std::memset(host(kZList), 0, kListBytes);
  std::memset(host(kTileList), 0, kListBytes);
  wr(kDev + 0x32C8u + 8u, kZList);
  wr(kDev + 0x32C8u + 0xCu, kZList + kListBytes);
  wr(kDev + 12960u + 8u, kTileList);
  wr(kDev + 12960u + 0xCu, kTileList + kListBytes);
  if (fx.pinned) {
    wr(kDev + 0x3290u, kSeg0);  // the recording pin: the CB cursor at the last fence
    wr(kDev + 0x3294u, 2u);     // its generation + 1 (820FCBE8 refuses nothing)
  }
  if (fx.vizId) {
    host(kDev)[11915u] = fx.vizId;
    host(kDev)[11850u + fx.vizId] = 0x2Au;
  }
  g_tailFails = fx.tailFailsAfterKick;
  g_segAllocs = g_tailRefused = 0;
  g_segWasPinned = false;
  wr(kHsio, 0u);
  host(kDev)[0x2A38u] = uint8_t(fx.b38 | (fx.tilePred ? 0x02u : 0u));  // 820FC000 lbz 0x2A38 ; rlwinm. 30,30
  // the shadows the sections read (dev+0x400 .. 0x29FF): distinct values, low bits 0
  for (uint32_t off = 0x400u; off < 0x2A00u; off += 4u) wr(kDev + off, 0x40000000u | ((off * 0x9E3779B1u) & 0x3FFFFFFCu));
  wr(kDev + 0x2844u, 0u);           // the window scissor shadow (row 9: no B0 scissor, the restore's values)
  wr(kDev + 0x2848u, 0x00200040u);  // {64, 32}
  wr(kDev + 0x3090u, 0x00000003u);  // the bin mask the default block re-sends
  // the index buffer object (820FC000: dev+12164 -> ib[0] flags, ib[6] data base)
  wr(kDev + 12164u, kIbObj);
  wr(kIbObj, fx.ib32 ? 0x80000000u : 0u);
  wr(kIbObj + 24u, kIdx);
  for (uint32_t p = 0; p < 16; p++) {
    wr(kSplitTableVA + 8u * p, kTable[p][0]);
    wr(kSplitTableVA + 8u * p + 4u, kTable[p][1]);
  }
  // Resolve's destination texture: a 2D 64x32 fetch constant (pitch 64, format 6), list fields for the S == 0 path
  std::memset(host(kTex), 0, 0x40u);
  wr(kTex + 0x1Cu, 0x00800000u);                        // fc0: pitch 64 (rotl 15 & 0x3FE0), signX 0
  wr(kTex + 0x20u, 0x1A000000u | 6u);                   // fc1: base | format
  wr(kTex + 0x24u, (31u << 13) | 63u);                  // fc2: height - 1, width - 1
  wr(kTex + 0x30u, 1u << 9);                            // fc5: dimension 1 (2D)
  if (fx.listFull) {
    wr(kTex, 1u);
    wr(kDev + 0x2A20u, 1u);
    wr(kDev + 0x335Cu, 0x70807400u);
    wr(kDev + 0x3360u, 0x70807400u);
  }
  wr(kStack + 0x64u, 0u);  // Resolve's pParameters (caller's frame)
  // the .rdata floating-point constants the row-9 bodies load (IDA read_memory_bytes; the ports hold them as literals)
  wr(0x820B5184u, 0x3F000000u);  // flt_820B5184 0.5f (the pixel-centre offset)
  wr(0x820BCB98u, 0x00000000u);  // flt_820BCB98 0.0f
  wr64(0x820BD398u, 0x3E70000010000010ull);  // dbl_820BD398
  wr64(0x820BD3A0u, 0x3FB0000000000000ull);  // dbl_820BD3A0 0.0625
  for (uint32_t k = 0; k < 27u; k++) wr(kVsUcode + 4u * k, 0xC0DE0000u | k);
  for (uint32_t k = 0; k < 9u; k++) wr(kPsUcode + 4u * k, 0xC0DE1000u | k);
  std::memset(host(kSurf), 0, 0x40u);
  if (fx.rt0) {  // ClearF: RT0 a 64x32 surface (colorInfo format 0: under one tile, so no colour-as-depth pack)
    wr(kSurf + 0x1Cu, 0x00000040u);
    wr(kSurf + 0x24u, (63u << 18) | (31u << 3));
    wr(kDev + 0x2F88u, kSurf);
    wr(kDev + 0x3060u, 64u);
    wr(kDev + 0x3064u, 32u);
  }
  {  // each arm starts from an empty transport and an empty dedup mirror
    std::lock_guard<std::mutex> lk(g_segMu);
    g_segFeet.clear();
    g_segById.clear();
    g_segDeferred.clear();
    auditClearLocked();  // step 6 I12: the audit, both arms (the ring / the map)
  }
  t_segPublishing.reset();
  g_head = g_reclaim = g_tailSeen = 0u;
  g_cpTail.store(0u);
  sr::native::cdedup::lostStream();
  g_top = kBuf + kBufSize;
  g_allocs.clear();
  g_pub.clear();
  g_chained = 0;
}

// ---- the canonical stream ----
enum : uint32_t { kTokW = 0xA0000001u, kTokD = 0xA0000002u, kTokD2 = 0xA0000003u, kTokP = 0xA0000004u, kTokT1 = 0xA0000005u };
struct Canon {
  std::vector<uint32_t> tok;
  uint64_t gapPackets = 0, recItems = 0, opaqueItems = 0;
  uint64_t ddck = 0;  // SR_NATIVE_CONST_DEDUP_CHECK's type-3 NOP {0xC0001000, 'DDCK'} marks (today's constant leaf)
  bool bad = false;
};
void canonPm4(const uint8_t* p, size_t n, Canon& cn, bool gap) {
  size_t off = 0;
  while (off + 4u <= n) {
    const uint32_t h = be32(p + off);
    const uint32_t type = h >> 30;
    if (gap && type != 2u) cn.gapPackets++;  // the guest's own type-2 NOPs (the cursor % 16 variants) write nothing
    if (type == 2u) {
      off += 4u;
      continue;
    }
    if (type == 1u) {
      if (off + 12u > n) break;
      cn.tok.insert(cn.tok.end(), {kTokT1, h, be32(p + off + 4u), be32(p + off + 8u)});
      off += 12u;
      continue;
    }
    const uint32_t cnt = ((h >> 16) & 0x3FFFu) + 1u;
    if (off + 4u + 4u * cnt > n) break;
    const uint8_t* b = p + off + 4u;
    if (type == 0u) {
      const uint32_t reg = h & 0x7FFFu;
      const bool one = (h >> 15) & 1u;
      for (uint32_t k = 0; k < cnt; k++) cn.tok.insert(cn.tok.end(), {kTokW, one ? reg : reg + k, be32(b + 4u * k)});
    } else {
      const uint32_t op = (h >> 8) & 0x7Fu, pred = h & 1u;
      if (op == 0x22u) {  // DRAW_INDX {viz, initiator, [dmaBase, size]} - the record's render-back carries viz 0
        cn.tok.insert(cn.tok.end(), {kTokD, pred, cnt >= 2u ? be32(b + 4u) : 0u, cnt >= 4u ? be32(b + 8u) : 0u,
                                     cnt >= 4u ? be32(b + 12u) : 0u});
      } else if (op == 0x36u) {  // DRAW_INDX_2 {initiator, [dmaBase, size]}
        cn.tok.insert(cn.tok.end(), {kTokD2, pred, be32(b), cnt >= 3u ? be32(b + 4u) : 0u, cnt >= 3u ? be32(b + 8u) : 0u});
      } else if (op != 0x10u) {  // a type-3 NOP has no effect
        cn.tok.insert(cn.tok.end(), {kTokP, op, pred, cnt});
        for (uint32_t k = 0; k < cnt; k++) cn.tok.push_back(be32(b + 4u * k));
      } else if (be32(b) == sr::native::cdedup::kCheckMagic) {
        cn.ddck++;
      }
    }
    off += 4u + 4u * cnt;
  }
  if (off != n) cn.bad = true;
}
// A record part: each item rendered as the CP renders it back (cpRecordPm4), row 9's opaque op lists by their renderer.
void canonRecord(const SegRecord& rec, Canon& cn) {
  RecHdr h;
  std::memcpy(&h, rec.data(), sizeof(h));
  if (h.magic != kRecMagic || h.bytes < sizeof(RecHdr) || h.bytes > 4u * rec.size()) {
    cn.bad = true;
    return;
  }
  const uint32_t* w = reinterpret_cast<const uint32_t*>(rec.data() + sizeof(RecHdr));
  const uint32_t* e = reinterpret_cast<const uint32_t*>(rec.data() + h.bytes);
  std::vector<uint8_t> pm4;
  while (w < e) {
    const uint32_t tag = *w;
    uint32_t words;
    if (tag & 0x80000000u) {
      const uint32_t kind = (tag >> 24) & 0x7Fu, n = (tag >> 8) & 0xFFFFu;
      words = recWords(kind, n);
      if (size_t(e - w) < 1u + words) {
        cn.bad = true;
        return;
      }
      if (kind >= kRkResolve && kind <= kRkClearFull) {  // row 9: the owner's renderer (the CPPM4 split's)
        const uint32_t bytes = sr::native::recb::renderBytes(w + 1, n);
        OpaqueRenderer r = opaqueRenderer(kind);
        if (!r) {
          cn.bad = true;
          return;
        }
        const size_t at = pm4.size();
        pm4.resize(at + bytes);
        r(kind, w + 1, n, pm4.data() + at, bytes);
        cn.opaqueItems++;
      } else {
        sr::gfx::Pm4Engine::NativeRecordStats st;
        bool opaqueSeen = false;
        uint32_t setc = 0;
        cpRecordPm4(w, w + 1 + words, pm4, st, opaqueSeen, setc);
        if (opaqueSeen || st.bad) cn.bad = true;
      }
    } else {
      words = (tag >> 16) & 0x3FFFu;
      if (size_t(e - w) < 1u + words) {
        cn.bad = true;
        return;
      }
      sr::gfx::Pm4Engine::NativeRecordStats st;
      bool opaqueSeen = false;
      uint32_t setc = 0;
      cpRecordPm4(w, w + 1 + words, pm4, st, opaqueSeen, setc);
      if (st.bad) cn.bad = true;
    }
    cn.recItems++;
    w += 1 + words;
  }
  canonPm4(pm4.data(), pm4.size(), cn, false);
}
void canonSegment(const SegParts& sp, Canon& cn) {
  for (const SegPart& part : sp.parts) {
    if (part.kind == 0u) {
      if (!part.record) {
        cn.bad = true;
        continue;
      }
      canonRecord(*part.record, cn);
    } else if (part.kind == 1u && part.off <= sp.bytes.size() && part.len <= sp.bytes.size() - part.off) {
      canonPm4(sp.bytes.data() + part.off, part.len, cn, true);
    } else {
      cn.bad = true;
    }
  }
}
// The register state at every draw (the comparison where the constant dedup leaves resident groups out: a group is
// skipped only when the stream already holds its 64 bytes, so every draw must still see the same registers).
struct DrawState {
  uint32_t kind, pred, init, dma, size;
  uint64_t hash;
  uint32_t r2102;  // VGT_INDX_OFFSET at the draw (in the hash too)
  bool operator==(const DrawState& o) const {
    return kind == o.kind && pred == o.pred && init == o.init && dma == o.dma && size == o.size && hash == o.hash;
  }
};
std::vector<DrawState> drawStates(const std::vector<uint32_t>& tok) {
  std::vector<uint32_t> regs(0x8000u, 0u);
  std::vector<DrawState> out;
  for (size_t i = 0; i < tok.size();) {
    const uint32_t t = tok[i];
    if (t == kTokW) {
      regs[tok[i + 1] & 0x7FFFu] = tok[i + 2];
      i += 3;
    } else if (t == kTokD || t == kTokD2) {
      uint64_t hsh = 0xCBF29CE484222325ull;
      for (uint32_t v : regs) hsh = (hsh ^ v) * 0x100000001B3ull;
      out.push_back(DrawState{t, tok[i + 1], tok[i + 2], tok[i + 3], tok[i + 4], hsh, regs[0x2102u]});
      i += 5;
    } else if (t == kTokP) {
      i += 4 + tok[i + 3];
    } else {
      i += 4;
    }
  }
  return out;
}

// ---- one arm ----
struct Ctrs {
  uint64_t sec[3], chunks[3], kicks[3], slow, big, bigParts, bigKicks, bigDecl, fallback, count, aborts, tileRec, taken,
      tilePred;
  uint64_t zp;  // SR_DIAG_ZPASS_LEAF: sections the lever sent to the guest leaf (the three leaves summed)
};
Ctrs ctrs() {
  Ctrs c;
  c.zp = 0;
  for (int i = 0; i < 3; i++) {
    c.sec[i] = g_splitSec[i].get();
    c.chunks[i] = g_splitChunks[i].get();
    c.kicks[i] = g_splitKicks[i].get();
    c.zp += g_zpassLeafSec[i].get();
  }
  c.slow = g_splitSlowTrailer.get();
  c.big = g_bigDraws.get();
  c.bigParts = g_bigParts.get();
  c.bigKicks = g_bigKicks.get();
  c.bigDecl = g_bigDecl.get();
  c.fallback = g_fallbackSec.get();
  c.count = g_why[kCount].get();
  c.aborts = g_sectionAborts.get();
  c.tileRec = g_why[kTileRec].get();
  c.taken = g_why[kTaken].get();
  c.tilePred = g_tilePredCalls.get();
  return c;
}
Ctrs minus(const Ctrs& b, const Ctrs& a) {
  Ctrs d;
  for (int i = 0; i < 3; i++) {
    d.sec[i] = b.sec[i] - a.sec[i];
    d.chunks[i] = b.chunks[i] - a.chunks[i];
    d.kicks[i] = b.kicks[i] - a.kicks[i];
  }
  d.slow = b.slow - a.slow;
  d.big = b.big - a.big;
  d.bigParts = b.bigParts - a.bigParts;
  d.bigKicks = b.bigKicks - a.bigKicks;
  d.bigDecl = b.bigDecl - a.bigDecl;
  d.fallback = b.fallback - a.fallback;
  d.count = b.count - a.count;
  d.aborts = b.aborts - a.aborts;
  d.tileRec = b.tileRec - a.tileRec;
  d.taken = b.taken - a.taken;
  d.tilePred = b.tilePred - a.tilePred;
  d.zp = b.zp - a.zp;
  return d;
}

// Row 9's record-mode counters summed over the three entries (sr::native::recb::recCounters).
struct Rec {
  uint64_t calls = 0, taken = 0, kickT = 0, chunks = 0, kickTStop = 0, liveStop = 0, declPre = 0, waitT = 0;
};
Rec recNow() {
  Rec r;
  for (int e = 0; e < 3; e++) {
    uint64_t v[sr::native::recb::kRecCounters];
    sr::native::recb::recCounters(e, v);
    r.calls += v[0];
    r.taken += v[1];
    r.kickT += v[2];
    r.chunks += v[3];
    r.kickTStop += v[4];
    r.liveStop += v[5];
    r.declPre += v[7];
    r.waitT += v[8];
  }
  return r;
}

struct Arm {
  std::vector<Pub> pubs;
  std::vector<uint8_t> device, buffer, bufferAfter, ring;
  std::vector<std::pair<uint32_t, uint32_t>> allocs;
  std::vector<uint8_t> allocBytes;  // the tail allocations' contents after the consumer
  std::vector<uint8_t> lists;       // the pinned closes' scratch list and the tile auto-predication list
  std::vector<Pub> pinned;          // the pinned closes' mode-B records {n, phys} in list order (their segments by id)
  uint32_t tailRefused = 0;         // tail allocations the callback refused (Fx::tailFailsAfterKick)
  uint64_t waitAcc = 0;             // dev+20024 + dev+20032 (820FCB30's wait-time accumulators), masked in `device`
  Rec rec;
  uint32_t top = 0, cursor = 0;
  Canon canon;
  std::vector<DrawState> states;
  std::vector<uint32_t> regs;
  uint64_t engDraws = 0;
  bool applied = true, missing = false;
  Ctrs d{};
};

enum ArmKind { kSdk, kOn, kOff };
uint8_t* g_base = nullptr;

// 8261B050's inputs, written identically before the call in every arm (IDA 8261B050 / 820F2A50; the render object as
// RwRenderObjectDrawInstanced 8261CED0 hands it over: +0xC vertices per instance, +0x10 stream 1, +0x14 stream 0).
void instSetup(const Op& op) {
  std::memset(host(kInst), 0, kInstBytes);
  const uint32_t mesh = kInst + 0x40u, index = kInst + 0x80u;
  wr(kInst + 0xCu, op.vpi);
  wr(kInst + 0x10u, index);
  wr(kInst + 0x14u, mesh);
  // the vertex buffers: +0x18 / +0x1C their fetch constant (base | type 3, size in dwords << 2 | endian 8in32), +0x20 the
  // stride (u16; 820F2A50 takes the mesh's from there, the index stream's is the literal 4)
  wr(mesh + 0x18u, 0x1A400000u | 3u);
  wr(mesh + 0x1Cu, ((op.vpi * 8u) << 2) | 2u);
  wr(mesh + 0x20u, 32u << 16);
  wr(index + 0x18u, 0x1A600000u | 3u);
  wr(index + 0x1Cu, ((128u * op.vpi) << 2) | 2u);
  wr(index + 0x20u, 4u << 16);
  uint32_t src = kInstConst;
  for (uint32_t t = 0; t < op.inst.size(); t++) {
    wr(kInst + 0xC0u + 4u * t, op.inst[t]);               // r4[t]: the tile's instances
    wr(kInst + 0xE0u + 4u * t, src);                      // r5[t]: its instance constants (regs vectors an instance)
    wr(kInst + 0x100u + 4u * t, kInst + 0x140u + 4u * t);  // a28[t]: its region list (one region)
    wr(kInst + 0x140u + 4u * t, 5u + t);                  // the region's id (the visibility query's)
    for (uint32_t k = 0; k < op.inst[t] * op.regs * 4u; k++) wr(src + 4u * k, 0x3F800000u + ((t << 16) | k));
    src += op.inst[t] * op.regs * 16u;
  }
  // the caller's frame (r1 = kStack): a28 at +0x54 (the region lists; read only when r10 != 0) and a30, the byte at
  // +0x5F (0: Begin / EndRegion 82102448 / 82102608, not TilePush / Pop)
  wr(kStack + 0x54u, op.regions ? kInst + 0x100u : 0u);
  wr(kStack + 0x5Cu, 0u);
  // the RW globals it reads: the device, the stream count of the previous draw (3: its loop unbinds stream 2), the
  // stream cache; the .rdata 0.0f (c4's y / z / w)
  wr(kRwDevice, kDev);
  wr(kRwStreams, 3u);
  wr(kRwVbCache, 0u);
  wr(kRwVbCache + 4u, 0u);
  wr(kFlt0, 0u);
  // the strides the bound vertex shader was patched with (dev+0x2D20 + stream, 820F2A50's compare): the mesh's 32 and
  // the index stream's 4, so the binds dirty no re-patch (dev+0x10 bit 51) - this fixture binds no shader
  host(kDev)[0x2D20u] = 32u / 4u;
  host(kDev)[0x2D21u] = 4u / 4u;
}

void callOp(const Op& op, ArmKind arm) {
  PPCContext c = context();
  c.r3.u64 = kDev;
  if (op.t == 'I') {
    instSetup(op);
    c.r3.u64 = kInst;                  // the render object
    c.r4.u64 = kInst + 0xC0u;          // instances per tile
    c.r5.u64 = kInst + 0xE0u;          // the tiles' instance constants
    c.r6.u64 = op.inst.size();         // tiles
    c.r7.u64 = op.regs;                // constant registers per instance
    c.r8.u64 = 0u;                     // no VS bind (8260B800)
    c.r9.u64 = 0u;                     // no PS bind (8260B898)
    c.r10.u64 = op.regions ? 1u : 0u;  // regions per tile
    PPCFunc* const body = PPC_LOOKUP_FUNC(g_base, 0x8261B050u);  // the generated body (no override)
    if (arm == kSdk) {
      t_fsGuestOnly = true;  // its 820FBBF8 / 8210D400 calls: the XDK bodies
      sr::native::cdedup::suspend(true);
      body(c, g_base);
      sr::native::cdedup::suspend(false);
      t_fsGuestOnly = false;
    } else {
      body(c, g_base);
    }
    return;
  }
  if (op.t == 'D') {
    for (uint32_t k = 0; k < 5; k++) wr64(kDev + 8u * k, op.q[k]);
    c.r4.u64 = op.prim;
    if (op.kind == 0) {
      c.r5.u64 = op.a;
      c.r6.u64 = op.b;
      c.r7.u64 = op.count;
    } else {
      c.r5.u64 = op.a;
      c.r6.u64 = op.count;
    }
    const uint32_t va = op.kind == 0 ? 0x820FC000u : 0x820FBBF8u;
    if (arm == kSdk) {
      sr::native::cdedup::suspend(true);  // the plain XDK leaf, as the dry CHECK compares it
      if (op.kind == 0) __imp__sub_820FC000(c, g_base);
      else __imp__sub_820FBBF8(c, g_base);
      sr::native::cdedup::suspend(false);
    } else {
      PPC_LOOKUP_FUNC(g_base, va)(c, g_base);
    }
    return;
  }
  for (uint32_t k = 2; k < 5; k++) wr64(kDev + 8u * k, op.q[k]);  // the row-9 ports' phaseC masks
  if (op.t == 'G') {
    c.r5.u64 = 16u;  // VS GPRs
    c.r6.u64 = 112u;  // PS GPRs
    if (arm == kSdk) __imp__sub_820F5570(c, g_base);
    else PPC_LOOKUP_FUNC(g_base, 0x820F5570u)(c, g_base);
  } else if (op.t == 'R') {
    c.r4.u64 = 0x10u;  // source RT0, 1X (the MSAA field set: no surface read), no clears
    c.r5.u64 = 0u;     // the whole destination
    c.r6.u64 = kTex;
    c.r7.u64 = 0u;
    c.r8.u64 = 0u;
    c.r9.u64 = 0u;
    c.r10.u64 = 0u;
    c.f1.f64 = 1.0;
    if (arm == kSdk) __imp__sub_8210C5F8(c, g_base);
    else PPC_LOOKUP_FUNC(g_base, 0x8210C5F8u)(c, g_base);
  } else if (op.t == 'C') {
    c.r4.u64 = 1u;  // RT0 colour
    c.r5.u64 = 0u;  // NULL rect: the whole surface
    c.r6.u64 = 0u;  // NULL colour: ClearImpl's local zeros
    c.r8.u64 = 0u;
    c.f1.f64 = 0.5;
    if (arm == kSdk) __imp__sub_82101A58(c, g_base);
    else PPC_LOOKUP_FUNC(g_base, 0x82101A58u)(c, g_base);
  }
}

Arm runArm(const Fx& fx, ArmKind arm, int abBit) {
  if (arm != kSdk)
    for (int i = 0; i < 2 && sr::abt::on(abBit) != (arm == kOn); i++) sr::abt::onGuestSwap();
  prepare(fx);
  Arm a;
  const Ctrs c0 = ctrs();
  const Rec r0 = recNow();
  // Fx::gpuWait: the helper thread playing the CP - generation 1 into the progress word after kGpuWaitMs, by the host
  // pointer (a plain guest store from a non-guest thread; 820F33E8's loop re-reads the word around its calls)
  std::thread cp;
  if (fx.gpuWait) {
    volatile uint32_t* const progress = reinterpret_cast<volatile uint32_t*>(host(kFb + 4u));
    cp = std::thread([progress] {
      std::this_thread::sleep_for(std::chrono::milliseconds(kGpuWaitMs));
      std::atomic_thread_fence(std::memory_order_seq_cst);
      *progress = _byteswap_ulong(1u);
      std::atomic_thread_fence(std::memory_order_seq_cst);
    });
  }
  for (const Op& op : fx.ops) callOp(op, arm);
  {  // the open segment's own close
    PPCContext c = context();
    c.r3.u64 = kDev;
    PPC_LOOKUP_FUNC(g_base, 0x820FD8C0u)(c, g_base);
  }
  if (cp.joinable()) cp.join();
  a.d = minus(ctrs(), c0);
  {
    const Rec r1 = recNow();
    a.rec.calls = r1.calls - r0.calls;
    a.rec.taken = r1.taken - r0.taken;
    a.rec.kickT = r1.kickT - r0.kickT;
    a.rec.chunks = r1.chunks - r0.chunks;
    a.rec.kickTStop = r1.kickTStop - r0.kickTStop;
    a.rec.liveStop = r1.liveStop - r0.liveStop;
    a.rec.declPre = r1.declPre - r0.declPre;
    a.rec.waitT = r1.waitT - r0.waitT;
  }
  a.pubs = g_pub;
  a.device.assign(host(kDev), host(kDev) + 0x5000u);
  // 820FCB30's wait-time accumulators (its report 820F3328: dev+20024 the kind-2 total, +20032 kind 3): timing, so
  // kept apart and masked from the device comparison; Fx::gpuWait requires > 0 in every arm (each one waited)
  a.waitAcc = be64(a.device.data() + 20024u) + be64(a.device.data() + 20032u);
  std::memset(a.device.data() + 20024u, 0, 16u);
  a.buffer.assign(host(kBuf), host(kBuf) + kBufSize);
  a.ring.assign(host(kRing), host(kRing) + (kRingMask + 1u) * 4u);
  a.lists.assign(host(kZList), host(kZList) + kListBytes);
  a.lists.insert(a.lists.end(), host(kTileList), host(kTileList) + kListBytes);
  a.tailRefused = g_tailRefused;
  a.top = g_top;
  a.cursor = rd(kDev + 0x28u);
  // the pinned closes: the mode-B records {0x82000000 | n, phys} the closes wrote into the scratch list, each published
  // as the pump's replay of the Z-pass list publishes it (820FD218's hand-off: segmentPublishId finds the deferred
  // registration by (phys, dwords))
  for (uint32_t va = kZList; va + 8u <= rd(kDev + 0x32C8u + 8u) && va + 8u <= kZList + kListBytes; va += 8u) {
    const uint32_t h = rd(va), phys = rd(va + 4u);
    if ((h & 0xFF000000u) != 0x82000000u) {
      a.missing = true;
      break;
    }
    const uint32_t n = h & 0x00FFFFFFu;
    const uint64_t id = segmentPublishId(phys, n);
    if (!id) a.missing = true;
    a.pinned.push_back(Pub{id, phys, n});
  }
  // the canonical stream, segment by segment in publication order (then the pinned closes' segments in list order)
  std::vector<Pub> all = a.pubs;
  all.insert(all.end(), a.pinned.begin(), a.pinned.end());
  for (const Pub& p : all) {
    std::shared_ptr<SegParts> sp;
    {
      std::lock_guard<std::mutex> lk(g_segMu);
      auto it = g_segById.find(p.id);
      if (it != g_segById.end()) sp = it->second;
    }
    if (!sp) {
      a.missing = true;
      continue;
    }
    canonSegment(*sp, a.canon);
  }
  a.states = drawStates(a.canon.tok);
  // the consumer: every published segment in order (records through cpApplyRecord - rendered back to PM4 under CPPM4)
  auto& xr = sr::gfx::getXenosRegisters();
  for (uint32_t i = 0; i < sr::gfx::XenosRegisterFile::kNumRegisters; i++) xr.write(i, 0u);
  auto eng = std::make_unique<sr::gfx::Pm4Engine>();
  eng->submitDraws = false;
  eng->cpMode = false;
  eng->physToHost = [](void*, uint32_t p) -> uint8_t* { return p < 0x20000000u ? host(0x80000000u | p) : nullptr; };
  for (const Pub& p : all)
    if (!cpSegment(*eng, p.id, p.phys, p.dwords)) a.applied = false;
  a.bufferAfter.assign(host(kBuf), host(kBuf) + kBufSize);
  a.allocs = g_allocs;
  for (const auto& r : a.allocs) a.allocBytes.insert(a.allocBytes.end(), host(r.first), host(r.first) + r.second);
  a.regs.resize(sr::gfx::XenosRegisterFile::kNumRegisters);
  for (uint32_t i = 0; i < a.regs.size(); i++) a.regs[i] = xr.read(i);
  a.engDraws = eng->draws;
  return a;
}

int g_diag = 0;
void diag(const std::string& n, const char* what, const Arm& s, const Arm& x) {
  if (g_diag++ >= 24) return;
  std::printf("[FLUSH_SPLIT_DIAG] %s %s: pubs %zu/%zu tokens %zu/%zu draws %zu/%zu gapPk %llu/%llu cursor %08X/%08X "
              "top %08X/%08X\n", n.c_str(), what, s.pubs.size(), x.pubs.size(), s.canon.tok.size(), x.canon.tok.size(),
              s.states.size(), x.states.size(), (unsigned long long)s.canon.gapPackets,
              (unsigned long long)x.canon.gapPackets, s.cursor, x.cursor, s.top, x.top);
  for (size_t i = 0; i < s.pubs.size() && i < x.pubs.size(); i++)
    if (!(s.pubs[i] == x.pubs[i])) {
      std::printf("[FLUSH_SPLIT_DIAG]   pub %zu: %08X/%u vs %08X/%u\n", i, s.pubs[i].phys, s.pubs[i].dwords, x.pubs[i].phys,
                  x.pubs[i].dwords);
      break;
    }
  for (size_t i = 0; i < s.canon.tok.size() && i < x.canon.tok.size(); i++)
    if (s.canon.tok[i] != x.canon.tok[i]) {
      std::printf("[FLUSH_SPLIT_DIAG]   token %zu: %08X vs %08X (around:", i, s.canon.tok[i], x.canon.tok[i]);
      for (size_t k = i >= 4 ? i - 4 : 0; k < i + 6 && k < s.canon.tok.size() && k < x.canon.tok.size(); k++)
        std::printf(" %08X/%08X", s.canon.tok[k], x.canon.tok[k]);
      std::printf(")\n");
      break;
    }
  for (size_t i = 0; i < s.device.size() && i < x.device.size(); i += 4)
    if (std::memcmp(s.device.data() + i, x.device.data() + i, 4) != 0) {
      std::printf("[FLUSH_SPLIT_DIAG]   device +%04zX: %08X vs %08X\n", i, be32(s.device.data() + i), be32(x.device.data() + i));
      break;
    }
  for (int pass = 0; pass < 2; pass++) {
    const std::vector<uint8_t>& sb = pass ? s.bufferAfter : s.buffer;
    const std::vector<uint8_t>& xb = pass ? x.bufferAfter : x.buffer;
    size_t first = SIZE_MAX, last = 0, words = 0;
    for (size_t i = 0; i + 4 <= sb.size() && i + 4 <= xb.size(); i += 4)
      if (std::memcmp(sb.data() + i, xb.data() + i, 4) != 0) {
        if (first == SIZE_MAX) first = i;
        last = i;
        words++;
      }
    if (first != SIZE_MAX)
      std::printf("[FLUSH_SPLIT_DIAG]   buffer%s: %zu words differ in [%08zX, %08zX]; first %08X vs %08X\n",
                  pass ? " after the consumer" : "", words, size_t(kBuf) + first, size_t(kBuf) + last + 4u,
                  be32(sb.data() + first), be32(xb.data() + first));
  }
}

bool has(const std::string& s, char c) { return s.find(c) != std::string::npos; }

// SR_DIAG_ZPASS_LEAF (C1 stage 1, L3b's lever; kernel/native_flush.cpp at flushSplitWindow): the sections it sent to the
// guest leaf over every arm of every fixture - in a mode without the lever it must stay 0 (inert by default).
uint64_t g_zpAll = 0;

void runFixture(const Fx& fx, int abBit, bool gateOn, bool raw, bool pm4, bool dedupSkip, bool ddCheck, bool zpLeaf) {
  const std::string n = fx.name;
  const Arm sdk = runArm(fx, kSdk, abBit);
  const Arm on = runArm(fx, kOn, abBit);
  const Arm off = runArm(fx, kOff, abBit);
  g_zpAll += sdk.d.zp + on.d.zp + off.d.zp;
  // The lever fires only where a Z-pass recording pins the buffer AND an S1 section crosses the segment limit (the
  // fixture's c / C / r / f letters); then the ON arm takes the guest leaf for those sections - today's path for them -
  // while its draws over 0xFFFF and row 9 stay converted (b / k / g / W).
  const bool secCross = has(fx.expect, 'c') || has(fx.expect, 'C') || has(fx.expect, 'r') || has(fx.expect, 'f');
  const bool leverFires = zpLeaf && gateOn && fx.pinned && secCross && !has(fx.expect, 'x');
  if (zpLeaf) {
    check(leverFires ? on.d.zp > 0 : on.d.zp == 0,
          n + ": ON arm: SR_DIAG_ZPASS_LEAF " +
              (leverFires ? std::string("sent the sections at the segment limit to the guest leaf (inside the recording)")
               : fx.pinned ? std::string("did not fire (inside the recording, but no S1 section crossed the limit)")
                           : std::string("did not fire (no Z-pass recording)")));
    check(sdk.d.zp == 0 && off.d.zp == 0,
          n + ": SDK / OFF arms: SR_DIAG_ZPASS_LEAF did not fire (the guest body / the split window off)");
  }
  // the SDK arm means what the fixture says (its segments hold the native close's own records unless they are PM4)
  const size_t sdkSegs = sdk.pubs.size() + sdk.pinned.size();
  check(!sdk.missing && !sdk.canon.bad && sdkSegs >= fx.minSegs && sdk.states.size() == fx.draws &&
            (pm4 || sdk.canon.recItems > 0 || fx.pinned) && (!fx.pinned || (sdk.pubs.empty() && !sdk.pinned.empty())),
        n + ": SDK arm: " + std::to_string(sdkSegs) + " segments (>= " + std::to_string(fx.minSegs) + "), " +
            std::to_string(sdk.states.size()) + " draw packets (" + std::to_string(fx.draws) + ")" +
            (fx.pinned ? ", every close pinned" : ""));
  if (fx.wrap)
    check(be32(sdk.device.data() + 13500u) == 2u,
          n + ": SDK arm: the generation ring wrapped (dev+13500 generation 2)");
  // 8261B050's SDK arm: every draw flush and SetPredication its calls reached ran the XDK body (t_fsGuestOnly), the ON
  // arm took them natively (SetPredication only where the fixture has tiles)
  for (const Op& op : fx.ops)
    if (op.t == 'I') {
      check(sdk.d.taken == 0 && sdk.d.tilePred == 0,
            n + ": SDK arm: 8261B050's draws and SetPredication calls all ran the XDK bodies (none taken natively)");
      check(on.d.taken > 0 && (op.inst.size() > 1 ? on.d.tilePred == op.inst.size() + 1u : on.d.tilePred == 0),
            n + ": ON arm: 8261B050's draws taken natively, SetPredication natively once per tile and once at the end");
    }
  // the per-part values the fixture names, explicitly (the boundary itself, not only ON == SDK)
  for (int field = 0; field < 2; field++) {
    const std::vector<uint32_t>& want = field == 0 ? fx.dma : fx.r2102;
    if (want.empty()) continue;
    std::string list;
    for (uint32_t v : want) {
      char h[16];
      std::snprintf(h, sizeof h, "%s%08X", list.empty() ? "" : " ", v);
      list += h;
    }
    for (int k = 0; k < 2; k++) {
      const Arm& x = k == 0 ? sdk : on;
      bool ok = x.states.size() == want.size();
      for (size_t i = 0; ok && i < want.size(); i++) ok = (field == 0 ? x.states[i].dma : x.states[i].r2102) == want[i];
      check(ok, n + (k == 0 ? ": SDK arm: " : ": ON arm: ") + (field == 0 ? "the parts' DMA bases " : "the parts' 0x2102 ") +
                    list);
    }
  }
  if (fx.tailFailsAfterKick)
    check(sdk.tailRefused > 0 && (sdk.device[0x2A39u] & 0x40u) != 0,
          n + ": SDK arm: a tail allocation after the kick failed (the sticky byte dev+0x2A39 & 0x40)");
  // 820FCB30's wait-time accumulators (masked from the device comparison): every arm waited on a gpuWait fixture, no
  // arm waited anywhere else (so the mask hides nothing there)
  check(fx.gpuWait ? sdk.waitAcc > 0 && on.waitAcc > 0 && off.waitAcc > 0
                   : sdk.waitAcc == 0 && on.waitAcc == 0 && off.waitAcc == 0,
        n + (fx.gpuWait ? ": SDK / ON / OFF arms: each waited for the GPU (820FCB30's accumulator dev+20024 > 0)"
                        : ": SDK / ON / OFF arms: none waited for the GPU (dev+20024 / 20032 stay 0)"));
  const bool onConverts = gateOn;  // the ON arm's window under the gate; gate-off: today's path in both
  for (int k = 0; k < 2; k++) {
    const Arm& x = k == 0 ? on : off;
    const std::string an = n + (k == 0 ? ": ON arm" : ": OFF arm");
    // With the constant dedup skipping (records mode), today's guest constant leaf (emitType0Dedup) leaves resident
    // groups out of the PM4 it writes, so its cursor - and every segment boundary after it - is not the SDK's; the
    // native record path keeps the SDK's cursor whatever it skips. The arms on today's path are held to the register
    // state at every draw and the consumer's there (the SDK-exact layout is the other modes'). Under cppm4 the same holds
    // for today's leaf where the fixture repeats a resident group (fx.ddHit: its CHECK marks, asserted present).
    const bool ddMarks = ddCheck && fx.ddHit && !(k == 0 && onConverts);
    if (ddMarks)
      check(x.canon.ddck > 0, an + ": today's guest constant leaf predicted a resident group (" +
                                  std::to_string(x.canon.ddck) + " 'DDCK' marks): held to the per-draw state");
    const bool layout = (!dedupSkip || (k == 0 && onConverts)) && !ddMarks;
    if (layout) {
      bool same = x.pubs == sdk.pubs && x.pinned == sdk.pinned;
      check(same, an + ": the published segments (phys, dwords) equal the SDK's, in order");
      if (!same) diag(n, k == 0 ? "on pubs" : "off pubs", sdk, x);
      if (fx.pinned || fx.tilePred || fx.tailFailsAfterKick)
        check(x.lists == sdk.lists && x.tailRefused == sdk.tailRefused,
              an + ": the scratch lists (pinned closes' mode-B records, the tile list) and the refused allocations equal "
                   "the SDK's");
      same = x.device == sdk.device && x.top == sdk.top && x.cursor == sdk.cursor;
      check(same, an + ": device, cursor and the allocator equal the SDK's");
      if (!same) diag(n, k == 0 ? "on device" : "off device", sdk, x);
      check(x.ring == sdk.ring, an + ": ring descriptors equal the SDK's");
    }
    bool same;
    check(!x.missing && !x.canon.bad, an + ": every segment rendered (records through cpRecordPm4)");
    if (raw && !ddMarks) {  // (ddMarks: the closes' cursor-derived words follow the marks' extra bytes)
      same = x.canon.tok == sdk.canon.tok;
      check(same, an + ": the canonical stream (register writes, draws, packets) equals the SDK's exactly");
      if (!same) diag(n, k == 0 ? "on canon" : "off canon", sdk, x);
    }
    same = x.states == sdk.states;
    check(same, an + ": every draw (initiator, DMA, predicate) sees the SDK's register state");
    if (!same) diag(n, k == 0 ? "on states" : "off states", sdk, x);
    check(x.applied && sdk.applied && x.regs == sdk.regs && x.engDraws == sdk.engDraws,
          an + ": the consumer (cpSegment) applied every segment: register file and draw count equal the SDK's");
    if (layout && !sdk.allocs.empty()) {
      same = x.allocs == sdk.allocs && x.allocBytes == sdk.allocBytes;
      check(same, an + ": the tail allocations (820FCDF0: the vertex data) and their contents after the consumer equal "
                       "the SDK's");
      if (!same) {
        size_t i = 0;
        while (i + 4 <= x.allocBytes.size() && i + 4 <= sdk.allocBytes.size() &&
               std::memcmp(x.allocBytes.data() + i, sdk.allocBytes.data() + i, 4) == 0)
          i += 4;
        std::printf("[FLUSH_SPLIT_DIAG] %s allocs %zu/%zu bytes %zu/%zu first difference at +%zu: %08X vs %08X\n",
                    an.c_str(), sdk.allocs.size(), x.allocs.size(), sdk.allocBytes.size(), x.allocBytes.size(), i,
                    i + 4 <= sdk.allocBytes.size() ? be32(sdk.allocBytes.data() + i) : 0u,
                    i + 4 <= x.allocBytes.size() ? be32(x.allocBytes.data() + i) : 0u);
      }
    }
    if (pm4) {
      check(x.buffer == sdk.buffer, an + ": the command buffer equals the SDK's byte for byte (literal spans)");
      if (x.buffer != sdk.buffer) diag(n, k == 0 ? "on buffer" : "off buffer", sdk, x);
    }
  }
  // wiring: what each arm took
  const std::string& e = fx.expect;
  const Ctrs& d = on.d;
  std::printf("[FLUSH_SPLIT_INFO] %s: SDK segs=%zu draws=%zu gapPk=%llu | ON sec=%llu/%llu/%llu chunks=%llu/%llu/%llu "
              "kicks=%llu/%llu/%llu slow=%llu big=%llu parts=%llu bigKicks=%llu fallback=%llu count=%llu gapPk=%llu "
              "recItems=%llu opaque=%llu rec9 calls=%llu taken=%llu kickT=%llu chunks=%llu kickTStop=%llu liveStop=%llu "
              "| OFF fallback=%llu count=%llu aborts=%llu gapPk=%llu opaque=%llu rec9 taken=%llu\n",
              n.c_str(), sdk.pubs.size(), sdk.states.size(), (unsigned long long)sdk.canon.gapPackets,
              (unsigned long long)d.sec[0], (unsigned long long)d.sec[1], (unsigned long long)d.sec[2],
              (unsigned long long)d.chunks[0], (unsigned long long)d.chunks[1], (unsigned long long)d.chunks[2],
              (unsigned long long)d.kicks[0], (unsigned long long)d.kicks[1], (unsigned long long)d.kicks[2],
              (unsigned long long)d.slow, (unsigned long long)d.big, (unsigned long long)d.bigParts,
              (unsigned long long)d.bigKicks, (unsigned long long)d.fallback, (unsigned long long)d.count,
              (unsigned long long)on.canon.gapPackets, (unsigned long long)on.canon.recItems,
              (unsigned long long)on.canon.opaqueItems, (unsigned long long)on.rec.calls, (unsigned long long)on.rec.taken,
              (unsigned long long)on.rec.kickT, (unsigned long long)on.rec.chunks, (unsigned long long)on.rec.kickTStop,
              (unsigned long long)on.rec.liveStop, (unsigned long long)off.d.fallback, (unsigned long long)off.d.count,
              (unsigned long long)off.d.aborts, (unsigned long long)off.canon.gapPackets,
              (unsigned long long)off.canon.opaqueItems, (unsigned long long)off.rec.taken);
  const bool conv = onConverts && !has(e, 'x');
  const bool sconv = conv && !leverFires;  // the S1 sections: converted unless SR_DIAG_ZPASS_LEAF sends them to the leaf
  if (has(e, 'c')) check(sconv ? d.sec[0] > 0 && d.kicks[0] + d.chunks[0] > 0 && d.aborts == 0 : d.aborts > 0 && d.sec[0] == 0,
                         n + ": ON arm: the constant sections " + (sconv ? "chunked natively (82107058)" : "went to the guest leaf"));
  if (has(e, 'C'))  // as 'c' where today's path declines the draws over 0xFFFF whole (before their sections: no abort)
    check(sconv ? d.sec[0] > 0 && d.kicks[0] + d.chunks[0] > 0 && d.aborts == 0 : d.sec[0] == 0,
          n + ": ON arm: the constant sections " + (sconv ? "chunked natively (82107058)" : "not chunked"));
  if (has(e, 'r')) check(sconv ? d.sec[1] > 0 : d.sec[1] == 0, n + ": ON arm: register runs " + (sconv ? "chunked natively" : "not chunked"));
  if (has(e, 'f')) check(sconv ? d.sec[2] > 0 : d.sec[2] == 0, n + ": ON arm: fetch runs " + (sconv ? "chunked natively" : "not chunked"));
  if (has(e, 't')) check(sconv ? d.slow > 0 : d.slow == 0, n + ": ON arm: the slow fetch trailer " + (sconv ? "written" : "not written"));
  if (has(e, 'b')) check(conv ? d.big > 0 && d.bigParts > 1 && d.count == 0 : d.big == 0 && d.count > 0,
                         n + ": ON arm: the draw over 0xFFFF indices " + (conv ? "split natively" : "declined to the guest body"));
  if (has(e, 'k')) check(conv ? d.bigKicks > 0 : true, n + ": ON arm: a split part's kick check kicked");
  if (has(e, 'c') || has(e, 'r') || has(e, 'f') || has(e, 'b'))
    check(sconv ? d.fallback == 0 && d.count == 0 : d.fallback + d.count > 0,
          n + ": ON arm: sectionFallbacks / count>0xFFFF " + (sconv ? "did not move" : "moved"));
  if (has(e, 'n'))  // at most 0xFFFF indices: today's path takes it whole in both arms - no split, no decline
    check(d.taken == 1 && d.big == 0 && d.count == 0 && d.fallback == 0 && off.d.taken == 1 && off.d.count == 0 &&
              (pm4 || on.canon.gapPackets == 0),
          n + ": ON / OFF arms: the draw taken natively whole (no split, no count>0xFFFF decline)");
  if (has(e, 'p'))  // tile auto-predication: declined before anything is written, in both arms (rule 1)
    check(d.tileRec == 1 && d.taken == 0 && d.big == 0 && off.d.tileRec == 1 && off.d.taken == 0,
          n + ": ON / OFF arms: the draw declined by tile auto-predication (the guest body's per-tile form)");
  // row 9 (record-mode counters; the opaque items too where the segments hold records)
  const Rec& ro = on.rec;
  if (has(e, 'g'))
    check(conv ? ro.calls == 1 && ro.taken == 1 && ro.kickT == 1 && ro.kickTStop == 0 && ro.liveStop == 0 &&
                     (pm4 || on.canon.opaqueItems > 0)
               : ro.calls == 1 && ro.taken == 0 && on.canon.opaqueItems == 0,
          n + ": ON arm: the row-9 entry " + (conv ? "taken as records through the kick (kickT 1, no live stop)" : "declined (kick)"));
  if (has(e, 'd'))
    check(ro.calls == 1 && ro.taken == 0 && ro.liveStop == 0 && on.canon.opaqueItems == 0,
          n + ": ON arm: the row-9 entry declined by its pre-pass (guest body)");
  if (has(e, 'W'))  // a tail allocation that waits for the GPU: modelled by the record pre-pass under the gate
    check(conv ? ro.calls == 1 && ro.taken == 1 && ro.waitT == 1 && ro.kickT == 0 && ro.liveStop == 0 &&
                     ro.declPre == 0 && (pm4 || on.canon.opaqueItems > 0)
               : ro.calls == 1 && ro.taken == 0 && ro.declPre == 1 && ro.waitT == 0 && on.canon.opaqueItems == 0,
          n + ": ON arm: the row-9 entry " +
              (conv ? "taken as records through a modelled GPU wait (waitT 1, no live stop)"
                    : "declined by its pre-pass at the GPU wait (today's stop)"));
  if (gateOn && has(e, 'W'))
    check(off.rec.calls == 1 && off.rec.taken == 0 && off.rec.declPre == 1 && off.rec.waitT == 0 &&
              off.canon.opaqueItems == 0,
          n + ": OFF arm: the row-9 entry declined at the GPU wait (the guest body waits)");
  if (!pm4 && (has(e, 'c') || has(e, 'r') || has(e, 'f') || has(e, 'b') || has(e, 'g') || has(e, 'W'))) {
    // fx.guestPk: other producers' own guest PM4 (8261B050's visibility regions) - the flush's packets stay 0
    const std::string other = fx.guestPk ? " beyond the " + std::to_string(fx.guestPk) + " of the visibility regions" : "";
    check(sconv ? on.canon.gapPackets == fx.guestPk : on.canon.gapPackets > fx.guestPk,
          n + ": ON arm: guest PM4 packets in its segments" + other + " " + (sconv ? "0 (all records)" : "> 0 (the fallback)"));
  }
  // the OFF arm (today's path) shows the fallback the fixture forces
  if (gateOn && (has(e, 'c') || has(e, 'r') || has(e, 'f') || has(e, 'b')))
    check(off.d.fallback + off.d.count > 0 && off.d.sec[0] + off.d.sec[1] + off.d.sec[2] + off.d.big == 0,
          n + ": OFF arm: the fixture forces the fallback (guest leaf / body), nothing converted");
  if (gateOn && has(e, 'g'))
    check(off.rec.calls == 1 && off.rec.taken == 0 && off.canon.opaqueItems == 0,
          n + ": OFF arm: the row-9 entry declined at the kick");
  std::fflush(stdout);
}

Op drawOp(int kind, uint32_t prim, uint32_t count, std::initializer_list<uint64_t> q, uint32_t a = 0, uint32_t b = 0) {
  Op op;
  op.t = 'D';
  op.kind = kind;
  op.prim = prim;
  op.count = count;
  op.a = a;
  op.b = b;
  uint32_t i = 0;
  for (uint64_t v : q) op.q[i++] = v;
  return op;
}
Op entryOp(char t, uint64_t q10, uint64_t q18, uint64_t q20) {
  Op op;
  op.t = t;
  op.q[2] = q10;
  op.q[3] = q18;
  op.q[4] = q20;
  return op;
}
}  // namespace fsp

int runFlushSplitTest(const char* mode) {
  namespace K = sr::kernel;
  using fsp::Fx;
  // Fully buffered, flushed after every fixture: ~1500 result lines unbuffered into a redirected FILE (the suite runner's
  // Start-Process) ran at seconds per fixture on the worker (one write per printf, each seen by the AV filter).
  static char s_out[1 << 16];
  std::setvbuf(stdout, s_out, _IOFBF, sizeof(s_out));
  const bool gateOff = std::strcmp(mode, "gate-off") == 0, raw = std::strcmp(mode, "records-raw") == 0,
             cppm4 = std::strcmp(mode, "cppm4") == 0, pm4 = std::strcmp(mode, "pm4") == 0,
             records = std::strcmp(mode, "records") == 0,
             // C1 stage 1, L3b's lever: records-raw's configuration with SR_DIAG_ZPASS_LEAF=1 - the pinned fixtures'
             // sections at the segment limit go to the guest leaf, every unpinned one still chunks natively
             zpLeaf = std::strcmp(mode, "zpass-leaf") == 0;
  if (!gateOff && !raw && !cppm4 && !pm4 && !records && !zpLeaf) {
    std::printf("FAIL: unknown mode %s (records | records-raw | cppm4 | pm4 | gate-off | zpass-leaf)\n"
                "[FLUSH_SPLIT_TEST] 0 passed, 1 failed\n",
                mode);
    return 1;
  }
  {  // a bare run re-runs itself with exactly the runner's configuration for the mode (tools/difftest/suite_bare.inc)
    bare::Conf conf = {{L"SR_PM4_RING", L"0"}, {L"SR_PM4_RING_EXEC", L"0"}, {L"SR_PM4_CP", L"0"},
                       {L"SR_ALLOW_FORCING", L"0"}, {L"SR_AB_TOGGLE", L"SR_NATIVE_FLUSH_SPLIT"}, {L"SR_AB_PRESENT", L"1"},
                       {L"SR_NATIVE_SEGMENTLESS", L"1"}, {L"SR_NATIVE_FLUSH", L"1"}, {L"SR_NATIVE_TILERECTS", L"0"},
                       {L"SR_NATIVE_FLUSH_SPLIT", gateOff ? L"0" : L"1"}};
    if (raw || pm4 || zpLeaf) conf.push_back({L"SR_NATIVE_CONST_DEDUP", L"0"});
    if (zpLeaf) conf.push_back({L"SR_DIAG_ZPASS_LEAF", L"1"});
    if (cppm4) {
      conf.push_back({L"SR_NATIVE_FLUSH_CPPM4", L"1"});
      conf.push_back({L"SR_NATIVE_CONST_DEDUP_CHECK", L"1"});
    }
    if (pm4) conf.push_back({L"SR_NATIVE_FLUSH_PM4", L"1"});
    int code = 0;
    if (bare::handled("FLUSH_SPLIT_TEST", "--flush-split-test", mode, L"SR_NATIVE_FLUSH_SPLIT", conf, code)) return code;
  }
  K::registerStubs();
  sr::guest::initializeDispatchTable();
  K::kpcrBindCurrent(fsp::kStack, fsp::kStack - 0x10000u, 3);
  fsp::g_base = K::getGuestBase();
  const int abBit = sr::abt::reg("SR_NATIVE_FLUSH_SPLIT");
  fsp::check(g_on && g_segmentless && g_split == !gateOff && g_cpPm4 == cppm4 && g_pm4Out == pm4 && abBit >= 0 &&
                 sr::abt::active() && !tileRectsOn(),
             "requested mode: record flush, segmentless, SR_NATIVE_FLUSH_SPLIT as named, the CPPM4 / PM4 split as named, "
             "the per-present A/B, row 5's leaf off (the SDK arm's nested 820F0208 stays the guest's)");
  if (zpLeaf) fsp::check(g_zpassLeaf, "requested mode: SR_DIAG_ZPASS_LEAF on (C1 stage 1, L3b's lever)");
  fsp::check(PPC_LOOKUP_FUNC(fsp::g_base, 0x820FC000u) == ::sub_820FC000 &&
                 PPC_LOOKUP_FUNC(fsp::g_base, 0x820FBBF8u) == ::sub_820FBBF8 &&
                 PPC_LOOKUP_FUNC(fsp::g_base, 0x820F5570u) == ::sub_820F5570 &&
                 PPC_LOOKUP_FUNC(fsp::g_base, 0x8210C5F8u) == ::sub_8210C5F8 &&
                 PPC_LOOKUP_FUNC(fsp::g_base, 0x82101A58u) == ::sub_82101A58 &&
                 PPC_LOOKUP_FUNC(fsp::g_base, 0x820FD8C0u) == ::sub_820FD8C0 &&
                 PPC_LOOKUP_FUNC(fsp::g_base, 0x8261B050u) == ::sub_8261B050,
             "production dispatch resolves the draw flushes, row 9's entries and the close to their overrides, and "
             "RwDrawInstancedConstBatched 8261B050 to its generated body");
  bool mapped = true;
  for (uint32_t va : {fsp::kDev, fsp::kDev + 0x4FFFu, fsp::kCell, fsp::kFb, fsp::kRing, fsp::kBuf,
                      fsp::kBuf + fsp::kBufSize - 1u, fsp::kStack - 0x2000u, fsp::kIbObj, fsp::kTex, fsp::kSurf,
                      fsp::kZList, fsp::kZList + fsp::kListBytes - 1u, fsp::kTileList,
                      fsp::kTileList + fsp::kListBytes - 1u,
                      kSplitTableVA, fsp::kVsUcode, fsp::kVsUcode + 107u, fsp::kPsUcode, fsp::kPsUcode + 35u,
                      fsp::kInst, fsp::kInst + fsp::kInstBytes - 1u, fsp::kRwDevice, fsp::kRwStreams, fsp::kRwVbCache,
                      fsp::kFlt0})
    mapped = mapped && fsp::host(va) != nullptr;
  fsp::check(mapped, "fixture memory mapped");
  if (fsp::failed) {
    std::printf("[FLUSH_SPLIT_TEST] %d passed, %d failed\n", fsp::passed, fsp::failed);
    return 1;
  }
  const size_t index = (fsp::kAllocCb - PPC_CODE_BASE) >> 2;
  PPCFunc* const saved = K::g_dispatchTable[index];
  K::g_dispatchTable[index] = fsp::flushSplitAllocate;
  if (!g_arena) g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRecArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  setSegmentPublicationObserver(fsp::observe);

  using fsp::drawOp;
  using fsp::entryOp;
  const uint64_t kAll = ~0ull;
  std::vector<Fx> fxs;
  // ---- 821078C0: constants. Two 4-group runs; the first fits, the second does not (no room even for a group: the
  // chunk path kicks first, then writes the whole run unpadded in the new segment) - every cursor % 16.
  for (uint32_t used : {0u, 4u, 8u, 12u}) {
    Fx f;
    f.name = "const-second-run-u" + std::to_string(used);
    f.used = used;
    f.room = 300u;
    f.ops = {drawOp(0, 4, 3, {0xF0F0000000000000ull})};
    f.expect = "c";
    f.minSegs = 2;
    f.draws = 1;
    fxs.push_back(f);
  }
  {  // the first run already crosses: 48 registers before the kick, 80 after
    Fx f;
    f.name = "const-partial";
    f.room = 200u;
    f.ops = {drawOp(1, 4, 6, {0xFF00000000000000ull})};
    f.expect = "c";
    f.minSegs = 2;
    f.draws = 1;
    fxs.push_back(f);
  }
  {  // VS and PS constants whole (64 groups each) through 456-byte segments: a run needing many kicks
    Fx f;
    f.name = "const-many-kicks";
    f.room = 120u;
    f.segSize = 0x200u;
    f.ops = {drawOp(0, 4, 3, {kAll, kAll})};
    f.expect = "c";
    f.minSegs = 10;
    f.draws = 1;
    fxs.push_back(f);
  }
  // ---- 82107520: register runs (0x4900's 40 registers: 24 before the kick, 16 after; then the 0x2100 run)
  {
    Fx f;
    f.name = "reg-4900";
    f.room = 100u;
    f.ops = {drawOp(0, 4, 3, {0, 0, 0, 2ull | (0x1FFFFFull << 2)})};
    f.expect = "r";
    f.minSegs = 2;
    f.draws = 1;
    fxs.push_back(f);
  }
  {  // one register left before the limit: a 1-register chunk (an 8-byte span), then the rest
    Fx f;
    f.name = "reg-one-register-chunk";
    f.room = 12u + 8u;
    f.ops = {drawOp(1, 4, 3, {0, 0, 0, 0x7FFF800000ull})};
    f.expect = "r";
    f.minSegs = 2;
    f.draws = 1;
    fxs.push_back(f);
  }
  // ---- 82107760: fetch constants (32 slots = 192 dwords: 16 slots before the kick, 16 after, the slow trailer)
  {
    Fx f;
    f.name = "fetch-last-chunked";
    f.room = 400u;
    f.ops = {drawOp(0, 4, 3, {0, 0, 0x00000FFFFFFFF000ull})};
    f.expect = "ft";
    f.minSegs = 2;
    f.draws = 1;
    fxs.push_back(f);
  }
  for (uint32_t used : {0u, 4u}) {  // the first run chunked, the second on the fast path (pad by cursor % 8): fast trailer
    Fx f;
    f.name = "fetch-mid-chunked-u" + std::to_string(used);
    f.used = used;
    f.room = 200u;
    f.ops = {drawOp(0, 4, 3, {0, 0, (0xFFFFull << 28) | (0xFull << 20)})};
    f.expect = "f";
    f.minSegs = 2;
    f.draws = 1;
    fxs.push_back(f);
  }
  {  // everything at once, two draws (the second's constants are resident in records mode), the second crosses
    Fx f;
    f.name = "mixed-two-draws";
    f.room = 1600u;
    f.ops = {drawOp(0, 4, 3, {0xF000000000000000ull, 0x0F00000000000000ull, 0x00000000000FF0FFull, 0x7FFF800002ull,
                            0x3F00000000ull}),
             drawOp(1, 6, 30, {0xF0F0000000000000ull, 0x0F0F000000000000ull, 0x00000FFFFFFFF0FFull, 0x7FFFFFFFFEull,
                             0x3FFFFFFFFFull})};
    f.expect = "c";
    f.minSegs = 2;
    f.draws = 2;
    fxs.push_back(f);
  }
  // ---- draws over 0xFFFF indices: 820FC000 / 820FBBF8's split by the indices-per-prim table
  struct Big {
    const char* name;
    int kind;
    uint32_t prim, count;
    bool ib32;
    uint32_t parts;
  };
  for (const Big& g : {Big{"big-idx16-trilist", 0, 4, 100000u, false, 2}, Big{"big-idx16-tristrip", 0, 6, 70000u, false, 2},
                       Big{"big-idx32-trilist", 0, 4, 200000u, true, 4}, Big{"big-auto-trilist", 1, 4, 70000u, false, 2},
                       Big{"big-auto-linestrip", 1, 3, 131072u, false, 3}, Big{"big-auto-quadlist", 1, 13, 0x20000u, false, 3},
                       Big{"big-auto-rectlist", 1, 8, 70000u, false, 2}, Big{"big-idx16-pointlist", 0, 1, 0x30000u, false, 4},
                       // line lists (ipp 2) and triangle fans (ipp 1, overlap 2: every part re-sends the fan's first two)
                       Big{"big-idx16-linelist", 0, 2, 100000u, false, 2}, Big{"big-auto-linelist", 1, 2, 0x20000u, false, 3},
                       Big{"big-auto-trifan", 1, 5, 70000u, false, 2}, Big{"big-idx32-trifan", 0, 5, 140000u, true, 3},
                       // the 0x10000 side of the transition: the smallest split (one full part + a 4 / 2 index rest)
                       Big{"big-10000-idx16-trilist", 0, 4, 0x10000u, false, 2},
                       Big{"big-10000-auto-pointlist", 1, 1, 0x10000u, false, 2},
                       Big{"big-10000-idx32-linelist", 0, 2, 0x10000u, true, 2}}) {
    Fx f;
    f.name = g.name;
    f.ib32 = g.ib32;
    f.ops = {drawOp(g.kind, g.prim, g.count, {0xC000000000000000ull, 0, 0x0000000000000003ull}, 7u, 100u)};
    f.expect = "b";
    f.draws = g.parts;
    fxs.push_back(f);
  }
  // the 0xFFFF side of the transition: today's path takes the draw whole in both arms (no split, no decline)
  for (int kind : {0, 1}) {
    Fx f;
    f.name = kind == 0 ? "big-ffff-idx16-trilist" : "big-ffff-auto-tristrip";
    f.ops = {drawOp(kind, kind == 0 ? 4u : 6u, 0xFFFFu, {0xC000000000000000ull, 0, 0x0000000000000003ull}, 7u, 100u)};
    f.expect = "n";
    f.draws = 1;
    fxs.push_back(f);
  }
  // the index offsets per part under a split (VGT_INDX_OFFSET 0x2102 and the index DMA address): a negative baseVertex
  // with a startIndex whose DMA address wraps 2^32 BETWEEN the parts (Astra round 2: startIndex 0x37B7FFF0, 16-bit
  // indices at kIdx: 2 * 0x37B7FFF0 + 0x90900000 = 0xFFFFFFE0, the second part 65532 indices on: 0x1_0001FFD8 ->
  // 0x0001FFD8; 820FC000's physical form ((a >> 20) + 0x200 & 0x1000) + (a & 0x1FFFFFFF): 0x20000FE0, 0x0001FFD8);
  // an auto-indexed strip whose per-part startVertex (0x2102) wraps 2^32 (0xFFFFFFF0, then + 65534 - 2 = 0x0000FFEC)
  {
    Fx f;
    f.name = "big-idx16-negbase-dmawrap";
    f.ops = {drawOp(0, 4, 100000u, {0xC000000000000000ull}, 0xFFFFF000u, 0x37B7FFF0u)};
    f.expect = "b";
    f.draws = 2;
    f.dma = {0x20000FE0u, 0x0001FFD8u};
    f.r2102 = {0xFFFFF000u, 0xFFFFF000u};
    fxs.push_back(f);
  }
  {
    Fx f;
    f.name = "big-auto-tristrip-startwrap";
    f.ops = {drawOp(1, 6, 70000u, {0xC000000000000000ull}, 0xFFFFFFF0u)};
    f.expect = "b";
    f.draws = 2;
    f.r2102 = {0xFFFFFFF0u, 0x0000FFECu};
    fxs.push_back(f);
  }
  // ---- the game's instanced path: RwDrawInstancedConstBatched 8261B050's generated body (Op 'I'), DrawVertices of
  // instances x vertices-per-instance (prim 4) over 0xFFFF per batch of 128 / regs instances
  {
    Fx f;  // one tile, one register an instance, 600 vertices: batches of 128, 128, 44 -> 76800 (2 parts) x 2, 26400
    f.name = "inst-8261B050-split";
    fsp::Op op;
    op.t = 'I';
    op.vpi = 600u;
    op.regs = 1u;
    op.inst = {300u};
    f.ops = {op};
    f.expect = "b";
    f.draws = 5;
    fxs.push_back(f);
  }
  {
    Fx f;  // two tiles (SetPredication 3, 12, then 0), two registers an instance, 1100 vertices: batches of 64 -> 70400
           // (2 parts) and the rests 8 / 2 instances; 1200 bytes of room: the first c100 run (2060 bytes) is chunked
           // across the kick, and later ones across the 8 KB segments' kicks
    f.name = "inst-8261B050-tiles-chunked";
    f.room = 1200u;
    fsp::Op op;
    op.t = 'I';
    op.vpi = 1100u;
    op.regs = 2u;
    op.inst = {200u, 130u};
    f.ops = {op};
    f.expect = "bC";
    f.minSegs = 2;
    f.draws = 12;
    fxs.push_back(f);
  }
  {
    Fx f;  // the same two tiles, each inside a visibility region (Begin / EndRegion: the viz dword of its draws; their
           // SET_BIN_MASK_LO / HI pairs are guest PM4 of their own, 4 packets a region)
    f.name = "inst-8261B050-regions";
    fsp::Op op;
    op.t = 'I';
    op.vpi = 1100u;
    op.regs = 2u;
    op.inst = {200u, 130u};
    op.regions = true;
    f.ops = {op};
    f.expect = "b";
    f.draws = 12;
    f.guestPk = 8;
    f.ddHit = true;  // no kick between the tiles: the second tile's c4 group is still resident
    fxs.push_back(f);
  }
  // a visibility query active (predicated rendering): every part's DRAW_INDX carries the viz dword 0x100 | id - byte
  // for byte under SR_NATIVE_FLUSH_PM4 (records drop it: the PM4 path never reads it)
  for (int kind : {0, 1}) {
    Fx f;
    f.name = kind == 0 ? "big-idx16-trilist-viz" : "big-auto-tristrip-viz";
    f.vizId = 3;
    f.ops = {drawOp(kind, kind == 0 ? 4u : 6u, 100000u, {0xC000000000000000ull})};
    f.expect = "b";
    f.draws = 2;
    fxs.push_back(f);
  }
  // tile auto-predication (byte dev+0x2A38 & 2): the S1 flush declines before writing anything in both arms; the guest
  // body writes its predicated per-tile form and its tile list (a split draw and a plain one)
  for (uint32_t count : {100000u, 3u}) {
    Fx f;
    f.name = count > 0xFFFFu ? "big-idx16-trilist-tilepred" : "draw-idx16-trilist-tilepred";
    f.tilePred = true;
    f.ops = {drawOp(0, 4, count, {0xC000000000000000ull})};
    f.expect = "p";
    f.draws = count > 0xFFFFu ? 2u : 1u;
    fxs.push_back(f);
  }
  {  // a part's kick check kicks inside the split loop
    Fx f;
    f.name = "big-kick-between-parts";
    f.room = 180u;
    f.ops = {drawOp(0, 4, 200000u, {}, 0u, 0u)};
    f.expect = "bk";
    f.minSegs = 2;
    f.draws = 4;
    fxs.push_back(f);
  }
  // ---- row 9: kick sites that kick (the pre-pass looks through them), a register run crossing the limit inside one
  {
    Fx f;
    f.name = "gpr-kick";
    f.room = 160u + 40u;
    f.ops = {entryOp('G', 0x100ull | 0xFFFull | (0x1Full << 44), 0x7FFFFCull, 0x3FFull)};
    f.expect = "g";
    f.minSegs = 2;
    f.draws = 24;
    fxs.push_back(f);
  }
  {
    Fx f;
    f.name = "gpr-run-crosses-limit";
    f.room = 160u + 8u;
    f.ops = {entryOp('G', 0xFFFull | (0x1Full << 44), 0x7FFFFCull, 0)};
    f.expect = "g";
    f.minSegs = 2;
    f.draws = 24;
    fxs.push_back(f);
  }
  {  // the tiled scissor restore is reached only after the post-kick allocations: the pre-pass must assume they succeed
    Fx f;
    f.name = "gpr-tiled-restore-after-kick";
    f.room = 160u + 40u;
    f.b38 = 0x10u;
    f.ops = {entryOp('G', 0xFFFull, 0x7FFFFCull, 0)};
    f.expect = "d";
    f.minSegs = 2;
    f.draws = 24;
    fxs.push_back(f);
  }
  {
    Fx f;
    f.name = "resolve-kick";
    f.room = 160u + 24u;
    f.ops = {entryOp('R', 0xFFFull, 0x7FFFFCull, 0x3FFull)};
    f.expect = "g";
    f.minSegs = 2;
    f.draws = 1;
    fxs.push_back(f);
  }
  {  // S = 0xFFFFFFFE: after the kick S is 0 and the SDK takes the list path (a full list chains a new one); the pre-pass
     // (S read before the kick) must decline on the even S
    Fx f;
    f.name = "resolve-even-S-list-path";
    f.room = 160u + 24u;
    f.S = 0xFFFFFFFEu;
    f.listFull = true;
    f.ops = {entryOp('R', 0xFFFull, 0x7FFFFCull, 0)};
    f.expect = "d";
    f.minSegs = 2;
    f.draws = 1;
    fxs.push_back(f);
  }
  {  // ClearF 82101A58 (the third row-9 entry): its first kick site kicks
    Fx f;
    f.name = "clearF-kick";
    f.room = 160u + 24u;
    f.rt0 = true;
    f.ops = {entryOp('C', 0xFFFull, 0x7FFFFCull, 0x3FFull)};
    f.expect = "g";
    f.minSegs = 2;
    f.draws = 2;  // RECTLIST strips + the interior (82100D20)
    fxs.push_back(f);
  }
  // inside a Z-pass bracket (the recording pin): every kick of a chunked run, a split draw or a row-9 entry is a PINNED
  // close - a mode-B list record, its parts registered deferred, nothing published, no fence (S unchanged)
  {
    std::vector<Fx> pin;
    for (const Fx& f : fxs)
      if (f.name == "const-many-kicks" || f.name == "reg-4900" || f.name == "fetch-last-chunked" ||
          f.name == "big-kick-between-parts" || f.name == "gpr-kick" || f.name == "resolve-kick" || f.name == "clearF-kick")
        pin.push_back(f);
    for (Fx& f : pin) {
      f.name += "-pinned";
      f.pinned = true;
      fxs.push_back(f);
    }
  }
  // every fixture so far on both allocators (the device callback, the game's generation ring)
  {
    const size_t n = fxs.size();
    for (size_t i = 0; i < n; i++) {
      Fx f = fxs[i];
      f.name += "-ring";
      f.ring = true;
      fxs.push_back(f);
    }
  }
  // the generation ring WRAPS at the kick (ring only): the open segment near the buffer's end, the next segment at the
  // buffer's start in generation 2 (dev+0x2A3A |= 4: the next close's whole-buffer invalidate)
  {
    std::vector<Fx> wrap;
    for (const Fx& f : fxs)
      if (f.name == "const-second-run-u0-ring" || f.name == "reg-4900-ring" || f.name == "fetch-last-chunked-ring" ||
          f.name == "big-kick-between-parts-ring" || f.name == "gpr-kick-ring" || f.name == "resolve-kick-ring" ||
          f.name == "clearF-kick-ring")
        wrap.push_back(f);
    for (Fx& f : wrap) {
      f.name += "-wrap";
      f.wrap = true;
      fxs.push_back(f);
    }
  }
  // the post-kick allocation failure (the device callback refuses every tail allocation once it handed out the kick's
  // segment): the pre-pass assumed success past the transparent kick; the live pass takes the XDK's failure branch
  // (the entry returns / skips its draw, the sticky byte dev+0x2A39 & 0x40, the next close drops its segment)
  for (char t : {'G', 'R', 'C'}) {
    Fx f;
    f.name = std::string(t == 'G' ? "gpr" : t == 'R' ? "resolve" : "clearF") + "-kick-alloc-fails";
    f.room = t == 'G' ? 160u + 40u : 160u + 24u;
    f.rt0 = t == 'C';
    f.tailFailsAfterKick = true;
    f.ops = {entryOp(t, 0xFFFull, 0x7FFFFCull, 0x3FFull)};
    f.expect = "g";
    f.minSegs = 1;  // the kick's segment; the close after the failure drops its own (820FD660 with 0x40 publishes nothing)
    f.draws = 0;
    fxs.push_back(f);
  }
  // the kick at every later site (the game's ring allocator: an allocation before the kick carves past the segment):
  // each entry with the threshold k bytes ahead of the cursor, the dirty masks empty but one run, up to the last k at
  // which the SDK body still kicks (Resolve writes ~250 bytes here, ClearF ~500, SetShaderGPRAllocation more)
  for (uint32_t k : {8u, 64u, 136u, 192u, 248u, 320u, 400u, 480u, 560u}) {
    for (char t : {'G', 'R', 'C'}) {
      if ((t == 'R' && k > 248u) || (t == 'C' && k > 480u)) continue;
      Fx f;
      f.name = std::string(t == 'G' ? "gpr" : t == 'R' ? "resolve" : "clearF") + "-kick-at-" + std::to_string(k) + "-ring";
      f.room = 160u + k;
      f.ring = true;
      f.rt0 = t == 'C';
      f.ops = {entryOp(t, 0x100ull, 0, 0)};
      f.expect = "g";
      f.minSegs = 2;
      f.draws = t == 'G' ? 24u : t == 'C' ? 2u : 1u;
      fxs.push_back(f);
    }
  }
  // the GPU a generation behind (frs1: stress_snap's clearF and rec_chk's resolve declined "alloc would wait for the
  // GPU"): each entry's first tail allocation waits in every arm; under the gate the record pre-pass models the wait
  // and the live pass waits inside its Entry while the helper thread plays the CP (ring only: the device callback never
  // waits)
  for (char t : {'G', 'R', 'C'}) {
    Fx f;
    f.name = std::string(t == 'G' ? "gpr" : t == 'R' ? "resolve" : "clearF") + "-gpu-wait-ring";
    f.ring = true;
    f.gpuWait = true;
    f.rt0 = t == 'C';
    f.ops = {entryOp(t, 0x100ull, 0, 0)};
    f.expect = "W";
    f.draws = t == 'G' ? 24u : t == 'C' ? 2u : 1u;
    fxs.push_back(f);
  }
  {  // a transparent kick first, then the wait: the pre-pass's placeholder allocations (no modelled wait), the live
     // pass's kick allocating the next segment waits inside the Entry (820FCF90 -> 820FCBE8 mode 2 -> 820FCB30)
    Fx f;
    f.name = "gpr-kick-then-gpu-wait-ring";
    f.room = 160u + 40u;
    f.ring = true;
    f.gpuWait = true;
    f.ops = {entryOp('G', 0x100ull | 0xFFFull | (0x1Full << 44), 0x7FFFFCull, 0x3FFull)};
    f.expect = "g";
    f.minSegs = 2;
    f.draws = 24;
    fxs.push_back(f);
  }
  for (const Fx& f : fxs)
    fsp::runFixture(f, abBit, !gateOff, raw || cppm4 || pm4 || zpLeaf, pm4, records || gateOff, cppm4, zpLeaf);
  // C1 stage 1, L3b's lever: unset (every mode but zpass-leaf) it never fired over every fixture, pinned or not; set, it
  // fired (on the pinned section fixtures only, each asserted above)
  fsp::check(zpLeaf ? g_zpassLeaf && fsp::g_zpAll > 0 : fsp::g_zpAll == 0,
             zpLeaf ? "SR_DIAG_ZPASS_LEAF=1: the lever fired, only on the pinned fixtures (asserted per fixture)"
                    : "SR_DIAG_ZPASS_LEAF unset: the lever never fired over every fixture (inert by default)");

  // ---- a draw over 0xFFFF indices whose prim has no table entry (the XDK's divwu traps): declined before anything is
  // written, in the ON window (the guest body is not run here: it would trap)
  {
    Fx f;
    f.name = "big-ipp0";
    fsp::prepare(f);
    for (int i = 0; i < 2 && !sr::abt::on(abBit); i++) sr::abt::onGuestSwap();
    const uint64_t decl0 = g_bigDecl.get(), count0 = g_why[kCount].get();
    const uint32_t c0 = fsp::rd(fsp::kDev + 0x28u);
    const uint64_t head0 = g_head;
    PPCContext c = fsp::context();
    c.r3.u64 = fsp::kDev;
    c.r4.u64 = 7u;  // prim 7: entry {0, 0}
    c.r5.u64 = 0u;
    c.r6.u64 = 70000u;
    const bool taken = sr::native::flush::draw(c, fsp::g_base, 1);
    fsp::check(!taken && fsp::rd(fsp::kDev + 0x28u) == c0 && g_head == head0 && g_why[kCount].get() == count0 + 1u &&
                   g_bigDecl.get() == decl0 + (gateOff ? 0u : 1u),
               "big-ipp0: a prim whose indices-per-prim entry is 0 declines before anything is written (count>0xFFFF, "
               "bigDecl under the gate)");
  }
  {  // the live proof's whole-run line (kernel/native_flush.cpp splitSnapshot), printed with a log-style timestamp so
     // tools/difftest/flush_split_run.py parses it with tools/perf/flush_split_snapshot.py (the format contract)
    const std::string t = splitSnapshotText();
    std::printf("[2026-01-01 00:00:00.000] [INFO ] [BOOT] [NATIVE_FLUSH] %s\n"
                "[2026-01-01 00:00:00.100] [INFO ] [BOOT] [FLUSH_SPLIT_TEST] the snapshot line above is the format contract\n",
                t.c_str());
    bool ok = t.rfind("split snapshot #", 0) == 0;
    for (const char* k : {" period=500ms ", " count>0xFFFF=", " ipp0=", " noSplit=", " sectionFallbacks=", " sectionAborts=",
                          " | big draws=", " | sections const=", " | resolve calls=", " | clearF calls=", " | gpr calls=",
                          " kickTStop=", " waitT="})
      ok = ok && t.find(k) != std::string::npos;
    fsp::check(ok, "the [NATIVE_FLUSH] split snapshot line holds every field the live proof grades");
  }
  setSegmentPublicationObserver(nullptr);
  K::g_dispatchTable[index] = saved;
  std::printf("[FLUSH_SPLIT_TEST] %d passed, %d failed\n", fsp::passed, fsp::failed);
  return fsp::failed ? 1 : 0;
}
}  // namespace sr::native::flush
