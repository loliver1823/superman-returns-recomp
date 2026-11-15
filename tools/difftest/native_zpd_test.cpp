// p2.zpd (docs/NATIVE_ROADMAP.md P2.12): the occlusion-query lifecycle against the generated guest bodies.
//
//   --native-zpd-test
//     Z0  the control: the gate off (=0, the guest path; the default before the bake) queues nothing, the fence
//         lands, the sentinel stays and GetData never completes - the checks below fail on it
//     Z1  Issue(BEGIN) through the wrapper sub_82779A28 -> D3DQueryIssueBeginEnd_82102BC8: the END record's first
//         four dwords preset to 0xFFFFFEED, one tile, and the PM4 the guest wrote (RB_SAMPLE_COUNT_ADDR = slot+0x20,
//         EVENT_WRITE_ZPD 0xC0005B00 {21}); Issue(END) through sub_82779A38: RB_SAMPLE_COUNT_ADDR = slot, q+0x14 =
//         the open submission
//     Z2  the CP wiring: those PM4 words executed by a Pm4Engine (cpMode, SR_NATIVE_ZPD strict) queue a BEGIN and an
//         END marker at their stream positions (Pm4Engine::applyZpd); the drain puts them in the backend's open set
//         (D3D12Backend::zpdDrainMarker) around a helper batch, a guest draw (4X MSAA target) and a helper batch
//     Z3  GetData through the wrapper sub_82779A48 -> D3DQueryGetData_82102DC8 before the write: -1 (S_FALSE) while
//         the fence is pending, and -1 with the fence passed and the sentinel still there (the hazard: a late write
//         is guest-visible)
//     Z4  strict, the CP path, the PRODUCTION covering fence: the EVENT_WRITE_SHD packet executed by the Pm4Engine
//         calls null_renderer's cpZpdFence (the hook kernel/gpu_command_processor installs) before its store, which
//         runs D3D12Backend::pm4ZpdFlushPartial on the attached backend (synchronous path): the partial frame's
//         queries are collected while the fence word has not moved, the slot holds the count, then the store lands;
//         GetData -> the count (S_OK, the raw 16-byte form {n, 0, 0, n}); the set consumed
//     Z4b the swap right after that fence with no draw between (Astra round 1 P1): pm4PresentFrame renders an empty
//         set - the partial's batches are not rendered a second time
//     Z4c the query brackets the guest draw only (Astra round 1 P1): BeginQuery right before it, EndQuery + resolve
//         right after it, the helper batches outside, nothing at the BEGIN marker
//     Z5  the latency model: the production hook marks the query covered, the fence store goes first, GetData stays
//         S_FALSE until the segment's submission completes, then 0 host samples -> 0 (the HUD's "occluded"); late=1
//     Z6  stale: the guest re-Begins the query before the first one resolved -> the old count is dropped (stale=1),
//         the slot keeps the new sentinel; the new query writes
//     Z7  orphan END, abandoned BEGIN, a zero record address, no host segment / an unseen BEGIN / over kMaxSegs
//         ("visible" 1), normalization
//     Z9  the publication protocol (Astra round 1 P2): the generated GetData called after EVERY 32-bit store of the
//         slot write answers S_FALSE or exactly {n, 0, 0, n}; a model of its load order (the disassembly of
//         82102EC0..82102FA8, checked against the generated body at every state) under every two-phase interleaving
//         of its loads with the stores; the control: the round-1 order (END+4 before END+0) is caught
//     Z10 the console's samples (Astra round 1 P2): one query over a 1X and a 2X draw counts host * samples-per-pixel
//         / area (not host / area)
//     Z11 strict, the SR_NATIVE_EXEC executor path (Pm4Engine::execPerformStores' store item), the render thread
//         taking the partial frame with 60 ms of GPU time: the store waits, the slot is written before it
//     Z12 the same through the CP path (applyEventWriteShd) with the render thread
//     Z13 strict fallback (Astra round 1 P1): the render thread's partial outlives the flush timeout -> before the
//         store the slot gets Xenia's strict fallback (the slot's last count), the late real count is dropped
//         (superseded); no backend at all -> the fallback 1
//     Z8  the accounting identity ended == resolved + lost + inflight; the census line prints
//     Z14 the player log profile (SR_LOG=player, every live roll's): the real Log keeps the [NATIVE_ZPD] census line
//         and the [NATIVE_ZPD_Q] query lines (their tags in app/log.cpp kMilestoneTags; zpd3 lost every one); the
//         census carries the probe and answer-lag fields
//     Z15 the visibility probe (zpd4: every query of every roll answered 0): a query draw carrying the probe pipelines
//         is drawn again after its measured draw on a ref and a raster segment, in that order; their counts never
//         reach the answer; the census classifies a 0 answer as depthKilled (ref > 0), psKilled (ref 0, raster > 0)
//         or noRaster (both 0); one probe of each kind per query (its first draw); a probe never takes an answer
//         segment (kMaxSegs draws still measured); the answer lag (CP END -> slot write) is measured
//     Z16 the GPU (WARP, a real D3D12 device the worker has): the HUD threat marker's query draw - ONE vertex as a
//         point list - through the real point-sprite GS source (tools/xenosrecomp/point_sprite.gs.hlsl), c19 from
//         edram::pointSpriteNdcRadius (PA_SU_POINT_SIZE 0x00080008 = 1 pixel), the viewport edram::computeViewport
//         gives a D24FS8 surface (float24: depth 0..0.5), Xenos zfunc 6 mapped as getPm4Pso maps it (GEQUAL), over
//         a stored scene depth: in front 1 sample and the slot 1 through zpd::collect; equal 1; behind 0 (ref probe
//         1 -> depthKilled); a 2-pixel point 4; w = 2 still 1; size 0 / off screen 0 (noRaster); a discarding pixel
//         shader 0 with the raster probe 1 (psKilled)
//     Z18 the Astra merge review P1: the guest's Issue and the CP's parse as separate steps - a re-issued pending
//         query (latency) whose predecessor's count lands between the guest's END preset and the CP's BEGIN: the
//         BEGIN re-arms the slot (sentinel back), so after the new query's fence GetData says not ready, never the
//         old count; then the new query's own count
//     Z17 the Xenia oracle's half (zpd7): a query whose BEGIN came inside a live-traced frame (onEvent's trace = the
//         trace's swap) logs its outcome in the player profile - [NATIVE_ZPD_T] swap= id= slot= written guest=, and
//         fallback guest= for strict's stand-in - every one, past the 64 [NATIVE_ZPD_Q] lines; an untraced query
//         logs none
//     Z19 the live trace writer the oracle plays (zpdx2 -> zpdx3): packets executed from host copies (the CP's ring
//         copy) are recorded at kHostPacketPhys, never 0; IM_LOAD_IMMEDIATE's inline ucode is no memory read; a trace
//         holds SR_PM4_TRACE_FRAMES frames, a swap event after each frame's swap packet, each frame's loose reads first
//         in that frame; tracedSwap is the trace's first frame
//   p2.zpd's bake (native/zpd-bake, 2026-10-16: SR_NATIVE_ZPD default ON in the latency model):
//     Z20 the decision table (requestOf, pure): unset / "" -> the latency model, the DEFAULT form; "0..." -> off;
//         "1" / "strict" / any other value -> strict; "2..." / "latency" -> the latency model (explicit)
//     Z21 the latch in a FRESH process per form (this exe's `--native-zpd-test latch` under the env the parent sets;
//         the latch is decided once a process): the mode, the form, the producer port's gate (kernel/native_flush
//         zpdPortOn: `!e || e[0] != '0'`) and the boot line - unset "on (the baked default; latency)", never "(=";
//         =1 / =strict "on (=...; strict)"; =2 / =latency "on (=...; latency)"; =0 / =00 off, the port off
//     Z22 the kRkZpd record reaches the 0x5B packet's one implementation (RecordEngine::applyZpdEvent): through the
//         record engine's own consumer (nativeRecord, the release path) and the development oracle
//         (nativeRecordOracle), a BEGIN and an END of one query queue the same markers, count the same logical query,
//         VGT_EVENT_INITIATOR, cpZpdEvents / cpZpdNative and opcode 0x5B as the guest's PM4 executed; with the mode
//         off the record answers nothing natively (the register only)
//     Z23 the producer port follows the mode: the real gate (SR_NATIVE_ZPD unset in this process: its default ON) and
//         the mode overridden - the port off exactly when the mode is off
//     Z24 the zpd census at each edge of a sim-freeze hold (kernel/highfps freezeEdgeCensus): one real hold in a fresh
//         process (`--native-zpd-test freeze`, SR_DIAG_SIM_FREEZE=1:900:0) logs `[NATIVE_ZPD] freeze #1 begin|end`
//         after its [SIM_FREEZE] lines, each with the counters of that edge
//     Z25 the frame stamp (Astra round 2 on native/zpd-bake, P2-1): the EVENT_WRITE_ZPD events the CP parsed in one
//         frame, by form - the literal packet pm4=, the kRkZpd record (release consumer and oracle) rec=, each BEGIN
//         zpd= - zeroed at each take with the frame ordinal advancing; nothing counted with the stamp off; the
//         presenting stamp thread-local; and, in Z24's hold (SR_PRESENT_DUMP_FREEZE=2), a dumped present's line ends
//         `zpd=3 rec=4 pm4=0 fswap=7` for its frame's stamp and `zpd=-` without one
//     Z26 the production backend frame queue carries distinct stamps to its actual render thread;
//         synchronous presents preserve caller TLS, and unstamped frames / partial sets carry none.
// The port itself against the SDK body (records / CPPM4 / PM4): --zpd-port-test (tools/difftest/zpd_port_test.inc).
// The device side of renderPm4Batches (the BeginQuery / draw / EndQuery calls on a real list) needs a GPU the cloud
// worker does not have: D3D12Backend's SR_DIFFTEST seam (d3d12_backend.hpp ZpdTestSeam) records the query commands
// and frames instead, through the same zpdMarker / zpdDrawBegin / zpdDrawEnd / zpdCollectNow the render loop calls.
// Mutation check: tools/difftest/native_zpd_run.py.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/guest_context.hpp"
#include "kernel/stubs.hpp"
#include "kernel/native_exec.hpp"
#include "kernel/native_flush.hpp"          // p2.zpd's bake: the producer port's gate (zpdPortOn, setZpdPortForTest)
#include "kernel/native_record_format.hpp"  // the kRkZpd record (Z22)
#include "kernel/highfps.hpp"               // Z24: the sim freeze's hold and its zpd census edges
#include "gfx/native_zpd.hpp"
#include "gfx/pm4_engine.hpp"
#include "gfx/pm4_live_trace.hpp"
#include "gfx/xenos_state.hpp"
#include "gfx/d3d12_backend.hpp"
#include "gfx/null_renderer.hpp"
#include "gfx/res_scale.hpp"
#include "app/log.hpp"

extern "C" void __imp__sub_82779A28(PPCContext& __restrict ctx, uint8_t* base);  // Issue(q, BEGIN)
extern "C" void __imp__sub_82779A38(PPCContext& __restrict ctx, uint8_t* base);  // Issue(q, END)
extern "C" void __imp__sub_82779A48(PPCContext& __restrict ctx, uint8_t* base);  // GetData(q, out, 16) -> out[0] / -1
extern "C" void __imp__sub_82102DC8(PPCContext& __restrict ctx, uint8_t* base);  // D3DQueryGetData

#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <process.h>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace K = sr::kernel;
namespace Z = sr::gfx::zpd;
namespace G = sr::gfx;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const std::string& name) {
  if (ok) {
    g_pass++;
    std::printf("  ok  %s\n", name.c_str());
  } else {
    g_fail++;
    std::printf("FAIL: %s\n", name.c_str());
  }
}
uint32_t rd(uint32_t va) { return K::readGuestU32BE(va); }
void wr(uint32_t va, uint32_t v) { K::writeGuestBE32(va, v); }

constexpr uint32_t kDev = 0x70600000u;    // a scratch device (only the fields the query code reads)
constexpr uint32_t kQ = 0x70610000u;      // the 152-byte query object
constexpr uint32_t kW = 0x70610200u;      // the game's wrapper object {?, query}
constexpr uint32_t kOut = 0x70610400u;    // GetData's out (16 bytes)
constexpr uint32_t kBuf = 0x70640000u;    // the command buffer the Issue writes into
constexpr uint32_t kStack = 0x70580000u;
constexpr uint32_t kSlotPhys = 0x00F20000u;  // the query's counter slot (one tile)
constexpr uint32_t kFbPhys = 0x00F10000u;    // the write-back block: fb+0 = the completed fence
constexpr uint32_t kSlotVa = 0xC0000000u + kSlotPhys;  // the uncached view GetData reads the slot through
constexpr uint32_t kFbVa = 0xC0000000u + kFbPhys;
constexpr uint32_t kArea = 9;  // the logical tests' 3x3 draw resolution scale

uint8_t* slotHost() { return K::guestVAToHost(kSlotVa); }
bool endSentinel() {
  for (uint32_t i = 0; i < 4; i++)
    if (rd(kSlotVa + 4u * i) != Z::kSentinel) return false;
  return true;
}
uint32_t le(uint32_t va) {  // the counts are little-endian in guest memory
  const uint8_t* p = K::guestVAToHost(va);
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
// What the test expects from host samples already times samples-per-pixel (independent of Z::normalize).
uint32_t expectGuest(uint64_t s, uint32_t area) {
  if (!s) return 0;
  const uint64_t n = area > 1 ? (s + area / 2u) / area : s;
  return n < 1 ? 1u : uint32_t(n);
}

void resetDevice(uint32_t s) {
  std::memset(K::guestVAToHost(kDev), 0, 0x5000);
  std::memset(K::guestVAToHost(kQ), 0, 0x200);
  std::memset(K::guestVAToHost(kBuf), 0xEE, 0x400);
  std::memset(slotHost(), 0xAB, Z::kSlotBytes);  // whatever a recycled slot held
  wr(kDev + 0x28u, kBuf);              // the command-buffer cursor (the Issue writes at cursor + 4)
  wr(kDev + 0x30u, kBuf + 0x8000u);    // the kick threshold: no flush inside the Issue
  wr(kDev + 0x2A1Cu, s);               // S: the open submission
  wr(kDev + 0x2A10u, kFbVa);           // the write-back block
  wr(kFbVa, s - 2u);                   // completed: the submission before S
  wr(kQ + 0x00u, kDev);
  wr(kQ + 0x04u, 9u);                  // D3DQUERYTYPE_OCCLUSION
  wr(kQ + 0x18u, kSlotVa);             // q[6]: tile 0's slot
  wr(kW + 0x04u, kQ);
}

void call(void (*fn)(PPCContext&, uint8_t*), uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t* ret = nullptr) {
  PPCContext ctx{};
  ctx.r1.u64 = kStack;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  fn(ctx, K::getGuestBase());
  if (ret) *ret = ctx.r3.u32;
}
uint32_t getDataWrapper() {
  uint32_t r = 0;
  call(__imp__sub_82779A48, kW, 0, 0, &r);
  return r;
}
// D3DQueryGetData itself, the 16-byte form: {ok, out[0..3]}.
struct Raw {
  bool ok = false;
  uint32_t o[4] = {};
};
Raw getDataRaw() {
  for (uint32_t i = 0; i < 4; i++) wr(kOut + 4u * i, 0xCCCCCCCCu);
  uint32_t r = 1;
  call(__imp__sub_82102DC8, kQ, kOut, 16u, &r);
  Raw x;
  x.ok = r == 0u;
  for (uint32_t i = 0; i < 4; i++) x.o[i] = rd(kOut + 4u * i);
  return x;
}
bool consistent(const Raw& x, uint32_t n) { return !x.ok || (x.o[0] == n && x.o[1] == 0 && x.o[2] == 0 && x.o[3] == n); }

// The PM4 an Issue wrote after `from` (BE words in the command buffer): RB_SAMPLE_COUNT_ADDR and the ZPD initiator.
struct Pm4Seen {
  bool zpd = false;
  uint32_t addr = 0, initiator = 0, firstWord = 0, words = 0;
};
Pm4Seen scan(uint32_t from) {
  Pm4Seen s;
  const uint32_t cursor = rd(kDev + 0x28u);
  s.firstWord = from + 4u;
  s.words = (cursor + 4u - s.firstWord) / 4u;
  for (uint32_t va = s.firstWord; va + 4u <= cursor; va += 4u) {
    if (rd(va) == 0x00002325u) s.addr = rd(va + 4u);
    if (rd(va) == 0xC0005B00u) {
      s.zpd = true;
      s.initiator = rd(va + 4u);
    }
  }
  return s;
}

// A Pm4Engine as the live CP: physical addresses through the same uncached view the guest reads.
uint8_t* physHost(void*, uint32_t phys) { return K::guestVAToHost(0xC0000000u + (phys & 0x1FFFFFFFu)); }

// Issue(BEGIN / END) through the game's wrapper only - the guest's CPU side: the END record's preset and the PM4 words
// in the command buffer. The CP parses them later (cpExecute): the interval between the two is the Astra merge
// review's P1 (Z18).
Pm4Seen guestIssue(bool begin) {
  const uint32_t c0 = rd(kDev + 0x28u);
  call(begin ? __imp__sub_82779A28 : __imp__sub_82779A38, kW);
  return scan(c0);
}
void cpExecute(G::Pm4Engine& eng, const Pm4Seen& s) {
  eng.execute(K::guestVAToHost(s.firstWord), size_t(s.words) * 4u);
}
// Issue(BEGIN / END) through the game's wrapper, then the guest's own PM4 words executed by the engine.
Pm4Seen issueOne(G::Pm4Engine& eng, bool begin) {
  const Pm4Seen s = guestIssue(begin);
  cpExecute(eng, s);
  return s;
}

// ── the logical drain (Z1-Z3 checks, Z5-Z7): the marker consumed with the drain's markerDrained call ──
bool popMarker(uint8_t& op, uint32_t& id) {
  auto& q = G::getDrawCommandQueue();
  const G::GuestDrawCommand* c = q.front();
  if (!c) return false;
  op = c->zpdOp;
  id = c->zpdId;
  const bool marker = !c->hasDraw && c->zpdOp != 0;
  q.popFront();
  if (marker) Z::markerDrained(Z::Op(op), id);
  return marker;
}

// ── the backend: the live backend the production cpZpdFence reaches (setPm4LiveBackend), with the seam ──
G::D3D12Backend* g_be = nullptr;
uint8_t g_lastOp = 0;
uint32_t g_lastId = 0;
// The drain's stand-in (drainDrawQueue needs a device): every queued marker into the open set through the drain's
// own marker branch (D3D12Backend::zpdDrainMarker -> zpd::markerDrained).
uint32_t drainToBackend() {
  auto& q = G::getDrawCommandQueue();
  uint32_t n = 0;
  while (const G::GuestDrawCommand* c = q.front()) {
    if (!c->hasDraw && c->zpdOp) {
      g_lastOp = c->zpdOp;
      g_lastId = c->zpdId;
      g_be->zpdTestPushMarker(c->zpdOp, c->zpdId);
      n++;
    }
    q.popFront();
  }
  return n;
}
void testFlush(void*) { drainToBackend(); }                // the engine's cp.flush: the drain before a store
void prodZpdFence(void*) { G::cpZpdFence(); }                // = kernel/gpu_command_processor.cpp onZpdFence

// The partial frame as the GPU finishes it (the seam's onFrame, on the thread that renders it).
std::atomic<int> g_partials{0}, g_presents{0};
std::atomic<uint32_t> g_fenceInside{0};
std::atomic<bool> g_slotWrittenInside{false};
void onFrame(void*, bool partial) {
  if (!partial) {
    g_presents++;
    return;
  }
  g_fenceInside = rd(kFbVa);
  g_slotWrittenInside = !endSentinel();
  g_partials++;
}
void resetSeen() {
  g_partials = 0;
  g_presents = 0;
  g_fenceInside = 0;
  g_slotWrittenInside = false;
  auto& t = g_be->zpdTest();
  std::lock_guard<std::mutex> lk(t.mu);
  t.frames.clear();
  t.rec.clear();
}
bool waitPartials(int n, int ms) {
  for (int i = 0; i < ms && g_partials.load() < n; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  return g_partials.load() >= n;
}

// One query into the backend's open set: BEGIN, a helper batch (a resolve), the guest draws (their RB_SURFACE_INFO
// MSAA), a helper batch, END - the guest bodies and the CP producing the markers, the drain's marker branch taking
// them. Returns the logical id (0 = failure).
uint32_t queryIntoSet(G::Pm4Engine& eng, std::initializer_list<uint32_t> drawMsaa, Pm4Seen* b = nullptr,
                      Pm4Seen* e = nullptr, bool probe = false) {
  const Pm4Seen sb = issueOne(eng, true);
  const bool m1 = drainToBackend() == 1 && g_lastOp == Z::kBegin;
  const uint32_t id = g_lastId;
  g_be->zpdTestPushDraw(0, true);
  for (uint32_t m : drawMsaa) g_be->zpdTestPushDraw(m, false, probe);
  g_be->zpdTestPushDraw(0, true);
  const Pm4Seen se = issueOne(eng, false);
  const bool m2 = drainToBackend() == 1 && g_lastOp == Z::kEnd && g_lastId == id;
  if (b) *b = sb;
  if (e) *e = se;
  return m1 && m2 ? id : 0u;
}

// EVENT_WRITE_SHD {3, fb|2, value}: the fence tail's store through the CP (k8in32: the word lands big-endian).
void execFenceStore(G::Pm4Engine& eng, uint32_t value) {
  const uint32_t pkt[4] = {0xC0025800u, 3u, kFbPhys | 2u, value};
  uint8_t be[16];
  for (int i = 0; i < 4; i++) {
    be[4 * i + 0] = uint8_t(pkt[i] >> 24);
    be[4 * i + 1] = uint8_t(pkt[i] >> 16);
    be[4 * i + 2] = uint8_t(pkt[i] >> 8);
    be[4 * i + 3] = uint8_t(pkt[i]);
  }
  eng.execute(be, sizeof be);
}
// The same store as the SR_NATIVE_EXEC executor performs it: a kStores item {kOpEvShd, fb|2, value}.
void execStoreItem(G::Pm4Engine& eng, uint32_t value) {
  static G::GuestDrawCommand c;
  c = G::GuestDrawCommand{};
  c.effectKind = sr::native::exec::kStores;
  c.effectN = 1;
  c.drawParams[0] = sr::native::exec::kOpEvShd;
  c.drawParams[1] = kFbPhys | 2u;
  c.drawParams[2] = value;
  c.drawParams[3] = 0u;
  eng.execPerformStores(c);
}

// The logical render side of one query (Z5-Z7): one guest draw's segment resolved with `samples`, then its END.
uint64_t g_readback[Z::kHeapCapacity] = {};
void renderQuery(uint32_t id, uint64_t samples, uint32_t spp = 1) {
  const uint32_t seg = Z::segmentOpen(id, spp);
  if (seg != Z::kNoSeg) g_readback[seg] = samples;
  Z::segmentClosed(id, seg);
  Z::endRecorded(id, true);
}

// ── Z9: the publication protocol ──────────────────────────────────────────────────────────────────────────────────
using Slot = std::array<uint8_t, 64>;
Slot snap() {
  Slot s;
  std::memcpy(s.data(), slotHost(), 64);
  return s;
}
uint32_t beAt(const Slot& s, uint32_t o) {
  return uint32_t(s[o]) << 24 | uint32_t(s[o + 1]) << 16 | uint32_t(s[o + 2]) << 8 | s[o + 3];
}
uint32_t leAt(const Slot& s, uint32_t o) {
  return uint32_t(s[o]) | uint32_t(s[o + 1]) << 8 | uint32_t(s[o + 2]) << 16 | uint32_t(s[o + 3]) << 24;
}
uint32_t bswap(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
// D3DQueryGetData_82102DC8 for one occlusion tile, size 16, as its loads run (82102EC0..82102FA8): lwz END+0; while
// it is the sentinel lwz END+4, +8, +C; then lwbrx +34 +30 +14 +10 (out[0]) and +C +2C +1C +3C +4 +24 +28 +20 +38 +8
// +18 (out[1..3]), out[3] taking the FIRST END+0 load byte-reversed. Loads [0, j) see state A, the rest state B.
Raw model(const Slot& A, const Slot& B, int j) {
  int k = 0;
  auto ld = [&](uint32_t o, bool be) {
    const Slot& s = k++ < j ? A : B;
    return be ? beAt(s, o) : leAt(s, o);
  };
  Raw r;
  const uint32_t w0 = ld(0, true);
  if (w0 == Z::kSentinel && ld(4, true) == Z::kSentinel && ld(8, true) == Z::kSentinel &&
      ld(12, true) == Z::kSentinel)
    return r;  // S_FALSE
  r.ok = true;
  const uint32_t a34 = ld(0x34, false), a30 = ld(0x30, false), a14 = ld(0x14, false), a10 = ld(0x10, false);
  r.o[0] = 0u - a34 - a30 + a14 + a10;
  const uint32_t c0c = ld(0x0C, false), c2c = ld(0x2C, false), c1c = ld(0x1C, false), c3c = ld(0x3C, false),
                 c04 = ld(0x04, false), c24 = ld(0x24, false), c28 = ld(0x28, false), c20 = ld(0x20, false),
                 c38 = ld(0x38, false), c08 = ld(0x08, false), c18 = ld(0x18, false);
  r.o[1] = c0c - c2c - c28 + c08;
  r.o[2] = c1c - c3c - c38 + c18;
  r.o[3] = c04 - c24 - c20 + bswap(w0);
  return r;
}
constexpr int kModelLoads = 19;  // the longest load sequence (the sentinel path's 4 + 4 + 11)
struct StepLog {
  std::vector<Slot> states;  // states[k] = the slot after k stores
  std::vector<Raw> real;     // the generated GetData at each state
};
void stepRead(void* user, uint32_t) {
  auto* L = static_cast<StepLog*>(user);
  L->states.push_back(snap());
  L->real.push_back(getDataRaw());
}
// The round-1 order (Astra round 1 P2: END+4 changes before END+0): the control the checker must catch.
void round1WriteSlot(uint8_t* p, uint32_t n, StepLog* L) {
  auto put = [&](uint32_t o, uint32_t v) {
    std::memcpy(p + o, &v, 4);
    stepRead(L, 0);
  };
  for (uint32_t i = 0; i < 8; i++) put(0x20u + 4u * i, 0u);
  put(16, n);
  put(20, 0u);
  put(24, 0u);
  put(28, 0u);
  put(4, 0u);
  put(8, 0u);
  put(12, 0u);
  put(0, n);
}
// Issue(BEGIN) + Issue(END) on a fresh device; the query's fence passed (GetData now reads the slot).
void freshQueryFencePassed(G::Pm4Engine& eng, uint32_t s) {
  resetDevice(s);
  issueOne(eng, true);
  issueOne(eng, false);
  auto& q = G::getDrawCommandQueue();
  while (q.front()) q.popFront();  // (gate off: nothing queued)
  wr(kDev + 0x2A1Cu, s + 2u);
  wr(kFbVa, s);
}

// ── Z16: the GPU (WARP) ──────────────────────────────────────────────────────────────────────────────────────────
// The HUD threat marker's occlusion query (HudGlowOcclusionQuery_sub_82304058): Issue(BEGIN), ONE vertex
// {x, y, z, 0xFF000000} drawn as a point list (the "HUD Threat Occlusion Compiled State": primitive 1, z test GEQUAL -
// engine state 3 = Xenos zfunc 6 against the reversed-Z scene depth - no z write), Issue(END). Here a real D3D12
// device (WARP) draws such a point through the real point-sprite GS source with c19 from edram::pointSpriteNdcRadius,
// in the viewport edram::computeViewport gives a D24FS8 depth surface (z range halved, the float24 mapping), with the
// depth func getPm4Pso maps zfunc 6 to, over a depth buffer holding a stored scene depth; its occlusion query is
// resolved into a readback buffer that zpd::collect turns into the guest slot. The probes run as renderPm4Batches
// runs them (zpdProbeDesc: depth ALWAYS / no pixel shader).
namespace warp {
using Microsoft::WRL::ComPtr;
constexpr char kShaders[] = R"(
cbuffer PointPos : register(b0) { float4 g_ClipPos; };
struct Vtx {
  float4 Position : SV_Position;
  float4 TexCoord0 : TEXCOORD0;   float4 TexCoord1 : TEXCOORD1;   float4 TexCoord2 : TEXCOORD2;
  float4 TexCoord3 : TEXCOORD3;   float4 TexCoord4 : TEXCOORD4;   float4 TexCoord5 : TEXCOORD5;
  float4 TexCoord6 : TEXCOORD6;   float4 TexCoord7 : TEXCOORD7;   float4 TexCoord8 : TEXCOORD8;
  float4 TexCoord9 : TEXCOORD9;   float4 TexCoord10 : TEXCOORD10; float4 TexCoord11 : TEXCOORD11;
  float4 TexCoord12 : TEXCOORD12; float4 TexCoord13 : TEXCOORD13; float4 TexCoord14 : TEXCOORD14;
  float4 TexCoord15 : TEXCOORD15;
  float4 Color0 : COLOR0;
  float4 Color1 : COLOR1;
  float4 Normal0 : NORMAL0;
};
Vtx VS(uint id : SV_VertexID) {
  Vtx o = (Vtx)0;
  o.Position = g_ClipPos;
  o.Color0 = float4(0.0, 0.0, 0.0, 1.0);
  return o;
}
float4 PS(Vtx i) : SV_Target0 { return i.Color0; }
float4 PSKill(Vtx i) : SV_Target0 { clip(-1.0); return i.Color0; }
)";
constexpr uint32_t kSize = 64;           // the render target / depth buffer (pixels)
constexpr float kStoredDepth = 0.25f;    // the scene depth the buffer holds (guest float24 depth 0.5, halved)
constexpr uint32_t kXenosGequal = 6;     // RB_DEPTHCONTROL zfunc of the query draw
constexpr uint32_t kXenosAlways = 7;     // the probes' (zpdProbeDesc)

struct Gpu {
  ComPtr<ID3D12Device> dev;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  uint64_t fenceValue = 0;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3DBlob> vs, gs, ps, psKill;
  ComPtr<ID3D12Resource> rt, ds, cb, readback;
  ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
  ComPtr<ID3D12QueryHeap> queries;
  uint8_t* cbPtr = nullptr;
  const uint64_t* counts = nullptr;
  D3D12_VIEWPORT vp{};
  std::string why;
};

ComPtr<ID3DBlob> compile(const std::string& src, const char* name, const char* entry, const char* target,
                         std::string& why) {
  ComPtr<ID3DBlob> blob, errors;
  const HRESULT hr = D3DCompile(src.data(), src.size(), name, nullptr, nullptr, entry, target, 0, 0, &blob, &errors);
  if (FAILED(hr)) {
    why = std::string(name) + " " + entry + ": " +
          (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "");
    return nullptr;
  }
  return blob;
}

ComPtr<ID3D12Resource> buffer(ID3D12Device* dev, D3D12_HEAP_TYPE type, uint64_t bytes, D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = type;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> r;
  if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
    return nullptr;
  return r;
}

ComPtr<ID3D12Resource> texture(ID3D12Device* dev, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                               D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = kSize;
  d.Height = kSize;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.Flags = flags;
  ComPtr<ID3D12Resource> r;
  if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
    return nullptr;
  return r;
}

bool init(Gpu& g) {
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter> adapter;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))) ||
      FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.dev)))) {
    g.why = "no WARP D3D12 device";
    return false;
  }
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue))) ||
      FAILED(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc))) ||
      FAILED(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc.Get(), nullptr,
                                      IID_PPV_ARGS(&g.list))) ||
      FAILED(g.list->Close()) || FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)))) {
    g.why = "queue / list / fence";
    return false;
  }
  g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  // The real point-sprite GS, from the source its DXIL is built from (tools/build_point_gs.ps1).
  std::string gsSrc;
  {
    std::ifstream in("tools/xenosrecomp/point_sprite.gs.hlsl", std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    gsSrc = ss.str();
  }
  if (gsSrc.empty()) {
    g.why = "tools/xenosrecomp/point_sprite.gs.hlsl not readable (run from the worktree root)";
    return false;
  }
  const std::string sh = kShaders;
  g.vs = compile(sh, "zpd_warp", "VS", "vs_5_1", g.why);
  g.ps = g.vs ? compile(sh, "zpd_warp", "PS", "ps_5_1", g.why) : nullptr;
  g.psKill = g.ps ? compile(sh, "zpd_warp", "PSKill", "ps_5_1", g.why) : nullptr;
  g.gs = g.psKill ? compile(gsSrc, "point_sprite.gs.hlsl", "main", "gs_5_1", g.why) : nullptr;
  if (!g.gs) return false;
  // b0 = the point's clip position (root constants), b2 space4 = the shared block whose c19 the GS reads.
  D3D12_ROOT_PARAMETER rp[2] = {};
  rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  rp[0].Constants = {0, 0, 4};
  rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  rp[1].Descriptor = {2, 4};
  rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rd{2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ComPtr<ID3DBlob> rs, rsErr;
  if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &rsErr)) ||
      FAILED(g.dev->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&g.root)))) {
    g.why = "root signature";
    return false;
  }
  g.rt = texture(g.dev.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_RENDER_TARGET);
  // The host format of a D24FS8 surface (d3d12_pm4_edram.cpp pm4DepthDsv).
  g.ds = texture(g.dev.Get(), DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                 D3D12_RESOURCE_STATE_DEPTH_WRITE);
  g.cb = buffer(g.dev.Get(), D3D12_HEAP_TYPE_UPLOAD, 64u * 512u, D3D12_RESOURCE_STATE_GENERIC_READ);
  g.readback = buffer(g.dev.Get(), D3D12_HEAP_TYPE_READBACK, uint64_t(Z::kHeapCapacity) * 8u,
                      D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_DESCRIPTOR_HEAP_DESC hd{};
  hd.NumDescriptors = 1;
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  bool heaps = SUCCEEDED(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap)));
  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  heaps = heaps && SUCCEEDED(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.dsvHeap)));
  D3D12_QUERY_HEAP_DESC qh{};
  qh.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  qh.Count = Z::kHeapCapacity;
  if (!g.rt || !g.ds || !g.cb || !g.readback || !heaps ||
      FAILED(g.dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&g.queries)))) {
    g.why = "resources";
    return false;
  }
  g.dev->CreateRenderTargetView(g.rt.Get(), nullptr, g.rtvHeap->GetCPUDescriptorHandleForHeapStart());
  g.dev->CreateDepthStencilView(g.ds.Get(), nullptr, g.dsvHeap->GetCPUDescriptorHandleForHeapStart());
  void* p = nullptr;
  if (FAILED(g.cb->Map(0, nullptr, &p))) return false;
  g.cbPtr = static_cast<uint8_t*>(p);
  if (FAILED(g.readback->Map(0, nullptr, &p))) return false;
  g.counts = static_cast<const uint64_t*>(p);
  return true;
}

// The query draw's pipeline (zfunc: the Xenos function, mapped as getPm4Pso maps it) or a probe's.
ComPtr<ID3D12PipelineState> pipeline(Gpu& g, uint32_t xenosZFunc, ID3DBlob* ps) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
  d.pRootSignature = g.root.Get();
  d.VS = {g.vs->GetBufferPointer(), g.vs->GetBufferSize()};
  d.GS = {g.gs->GetBufferPointer(), g.gs->GetBufferSize()};
  if (ps) d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  auto& b = d.BlendState.RenderTarget[0];
  b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_ONE;
  b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_ZERO;
  b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
  b.LogicOp = D3D12_LOGIC_OP_NOOP;
  b.RenderTargetWriteMask = 0;  // the query draw writes no colour (blend ZERO / ONE); the probes none either
  d.DepthStencilState.DepthEnable = TRUE;
  d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC((xenosZFunc & 7u) + 1u);
  d.DepthStencilState.FrontFace = d.DepthStencilState.BackFace = {
      D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
  d.SampleMask = UINT32_MAX;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  d.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  if (FAILED(g.dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) return nullptr;
  return pso;
}

struct Result {
  bool ran = false;
  uint64_t host = 0, ref = 0, raster = 0;
  uint32_t guest = 0;
  Z::Stats st{};
};

// One logical query around the point draw and its two probes, as renderPm4Batches records them.
Result query(Gpu& g, uint32_t index, const float clipPos[4], uint32_t pointSize, bool kill) {
  Result res;
  ComPtr<ID3D12PipelineState> answer = pipeline(g, kXenosGequal, kill ? g.psKill.Get() : g.ps.Get());
  ComPtr<ID3D12PipelineState> ref = pipeline(g, kXenosAlways, kill ? g.psKill.Get() : g.ps.Get());
  ComPtr<ID3D12PipelineState> raster = pipeline(g, kXenosAlways, nullptr);
  if (!answer || !ref || !raster) return res;
  Z::resetForTest();
  Z::setModeForTest(Z::kStrict);
  uint8_t* h = slotHost();
  const Z::Event b = Z::onEvent(kSlotPhys + 0x20u, h);
  const uint32_t segs[3] = {Z::segmentOpen(b.id, 1), Z::segmentOpenProbe(b.id, Z::kRef),
                            Z::segmentOpenProbe(b.id, Z::kRaster)};
  if (segs[0] == Z::kNoSeg || segs[1] == Z::kNoSeg || segs[2] == Z::kNoSeg) return res;
  // c19 = the NDC radius of PA_SU_POINT_SIZE over this viewport (the backend's function); the rest of the block 0.
  float shared[128] = {};
  sr::gfx::edram::pointSpriteNdcRadius(pointSize, g.vp.Width, g.vp.Height, 1u, 1u, &shared[76]);
  std::memcpy(g.cbPtr + index * 512u, shared, sizeof shared);
  if (FAILED(g.alloc->Reset()) || FAILED(g.list->Reset(g.alloc.Get(), nullptr))) return res;
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = g.dsvHeap->GetCPUDescriptorHandleForHeapStart();
  const float black[4] = {0, 0, 0, 0};
  g.list->ClearRenderTargetView(rtv, black, 0, nullptr);
  g.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, kStoredDepth, 0, 0, nullptr);
  g.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  const D3D12_RECT sc{0, 0, LONG(kSize), LONG(kSize)};
  g.list->RSSetViewports(1, &g.vp);
  g.list->RSSetScissorRects(1, &sc);
  g.list->SetGraphicsRootSignature(g.root.Get());
  g.list->SetGraphicsRoot32BitConstants(0, 4, clipPos, 0);
  g.list->SetGraphicsRootConstantBufferView(1, g.cb->GetGPUVirtualAddress() + index * 512u);
  g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
  ID3D12PipelineState* psos[3] = {answer.Get(), ref.Get(), raster.Get()};
  for (int i = 0; i < 3; i++) {
    g.list->BeginQuery(g.queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, segs[i]);
    g.list->SetPipelineState(psos[i]);
    g.list->DrawInstanced(1, 1, 0, 0);
    g.list->EndQuery(g.queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, segs[i]);
    g.list->ResolveQueryData(g.queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, segs[i], 1, g.readback.Get(),
                             uint64_t(segs[i]) * 8u);
  }
  if (FAILED(g.list->Close())) return res;
  ID3D12CommandList* lists[] = {g.list.Get()};
  g.queue->ExecuteCommandLists(1, lists);
  g.fenceValue++;
  if (FAILED(g.queue->Signal(g.fence.Get(), g.fenceValue))) return res;
  if (g.fence->GetCompletedValue() < g.fenceValue) {
    g.fence->SetEventOnCompletion(g.fenceValue, g.event);
    WaitForSingleObject(g.event, 10000);
  }
  if (g.fence->GetCompletedValue() < g.fenceValue) return res;
  for (uint32_t s : segs) Z::segmentClosed(b.id, s);
  const Z::Event e = Z::onEvent(kSlotPhys, h);
  Z::endRecorded(e.id, true);
  Z::stamp(0);
  Z::collect(UINT64_MAX, g.counts, 1u);
  res.ran = true;
  res.host = g.counts[segs[0]];
  res.ref = g.counts[segs[1]];
  res.raster = g.counts[segs[2]];
  res.guest = le(kSlotVa);
  res.st = Z::stats();
  return res;
}
}  // namespace warp

void runZpdWarp() {
  warp::Gpu g;
  const bool up = warp::init(g);
  if (!up) std::printf("      WARP setup failed: %s\n", g.why.c_str());
  check(up, "Z16: a WARP D3D12 device, the real point-sprite GS source compiled (gs_5_1), the query heap and readback");
  if (!up) return;
  // The viewport a 64x64 guest viewport gets on a D24FS8 surface with z tested (computeViewport: depth range halved,
  // the float24 mapping) - the scene's and the query draw's alike.
  G::edram::Regs r{};
  r.vteCntl = 0x3Fu;  // x / y / z scale and offset enabled
  r.vpScale[0] = 32.0f;
  r.vpScale[1] = -32.0f;
  r.vpScale[2] = 1.0f;
  r.vpOffset[0] = 32.0f;
  r.vpOffset[1] = 32.0f;
  r.vpOffset[2] = 0.0f;
  r.vtxCntl = 1u;          // pix_center: no half-pixel shift
  r.clipCntl = 1u << 19;   // dx_clip_space_def
  const G::edram::HostViewport hv = G::edram::computeViewport(r, false, true, 1u, 1u);
  g.vp = D3D12_VIEWPORT{hv.x, hv.y, hv.w, hv.h, hv.minZ, hv.maxZ};
  check(!hv.empty && hv.x == 0.0f && hv.y == 0.0f && hv.w == 64.0f && hv.h == 64.0f && hv.minZ == 0.0f &&
            hv.maxZ == 0.5f,
        "Z16: the fixture's viewport is computeViewport's for a float24 depth surface (64x64, depth 0..0.5)");
  // Clip positions: pixel centre (32.5, 32.5) and the pixel corner (32, 32) of the 64x64 target.
  const float c = 2.0f * 32.5f / 64.0f - 1.0f;
  const float front[4] = {c, -c, 0.75f, 1.0f};      // depth 0.375 >= the stored 0.25
  const float equal[4] = {c, -c, 0.5f, 1.0f};       // depth 0.25 = the stored depth
  const float behind[4] = {c, -c, 0.25f, 1.0f};     // depth 0.125 < the stored 0.25
  const float corner[4] = {0.0f, 0.0f, 0.75f, 1.0f};
  const float persp[4] = {2.0f * c, -2.0f * c, 1.5f, 2.0f};  // the same point at w = 2
  const float offscreen[4] = {3.0f, -c, 0.75f, 1.0f};
  constexpr uint32_t kOnePixel = 0x00080008u, kTwoPixels = 0x00100010u;
  uint32_t idx = 0;
  warp::Result v = warp::query(g, idx++, front, kOnePixel, false);
  std::printf("      WARP front: host %llu ref %llu raster %llu guest %u\n", (unsigned long long)v.host,
              (unsigned long long)v.ref, (unsigned long long)v.raster, v.guest);
  check(v.ran && v.host == 1u && v.guest == 1u && v.st.nonzero == 1u && v.st.probes == 1u && v.st.depthKilled == 0u,
        "Z16: a 1-pixel point (PA_SU_POINT_SIZE 0x00080008) in front of the stored depth: GEQUAL counts 1 sample, "
        "zpd::collect writes 1 to the slot");
  v = warp::query(g, idx++, equal, kOnePixel, false);
  check(v.ran && v.host == 1u && v.guest == 1u, "Z16: the point exactly at the stored depth: GEQUAL passes (1)");
  v = warp::query(g, idx++, behind, kOnePixel, false);
  std::printf("      WARP behind: host %llu ref %llu raster %llu guest %u\n", (unsigned long long)v.host,
              (unsigned long long)v.ref, (unsigned long long)v.raster, v.guest);
  check(v.ran && v.host == 0u && v.guest == 0u && v.ref == 1u && v.raster == 1u && v.st.depthKilled == 1u &&
            v.st.zero == 1u,
        "Z16: the point behind the stored depth: 0 samples, the slot 0; the ref probe (depth ALWAYS) still counts it "
        "-> depthKilled");
  v = warp::query(g, idx++, corner, kTwoPixels, false);
  check(v.ran && v.host == 4u && v.guest == 4u,
        "Z16: a 2-pixel point (0x00100010) on a pixel corner covers 2x2 samples (the radius math of c19)");
  v = warp::query(g, idx++, persp, kOnePixel, false);
  check(v.ran && v.host == 1u, "Z16: the 1-pixel point at w = 2 still covers 1 sample (the GS scales by w)");
  v = warp::query(g, idx++, front, 0u, false);
  check(v.ran && v.host == 0u && v.ref == 0u && v.raster == 0u && v.st.noRaster == 1u,
        "Z16: PA_SU_POINT_SIZE 0: a zero-area sprite, 0 samples, both probes 0 -> noRaster");
  v = warp::query(g, idx++, offscreen, kOnePixel, false);
  check(v.ran && v.host == 0u && v.ref == 0u && v.raster == 0u && v.st.noRaster == 1u,
        "Z16: a point off screen (clip x = 3): 0, both probes 0 -> noRaster");
  v = warp::query(g, idx++, front, kOnePixel, true);
  check(v.ran && v.host == 0u && v.ref == 0u && v.raster == 1u && v.st.psKilled == 1u,
        "Z16: a pixel shader that discards: 0 and ref 0, the raster probe (no pixel shader) 1 -> psKilled");
  Z::setModeForTest(Z::kStrict);
}

// ── p2.zpd's bake (native/zpd-bake, 2026-10-16) ─────────────────────────────────────────────────────────────────────
namespace NF = sr::native::flush;

// Z21's child: the latch in THIS process - its mode, its form and the producer port's gate. The latch's boot line
// reaches the same output (the log's console), which the parent reads.
int runNativeZpdLatch() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  K::registerStubs();
  const Z::Mode m = Z::mode();
  std::printf("[NATIVE_ZPD_LATCH] mode=%d explicit=%d port=%d\n", int(m), int(Z::explicitForm()), int(NF::zpdPortOn()));
  return 0;
}

void z20DecisionTable() {
  struct Row {
    const char* v;
    Z::Mode mode;
    bool explicitForm;
  };
  const Row rows[] = {{nullptr, Z::kLatency, false}, {"", Z::kLatency, false},   {"0", Z::kOff, true},
                      {"00", Z::kOff, true},          {"0x2", Z::kOff, true},       {"1", Z::kStrict, true},
                      {"strict", Z::kStrict, true},   {"3", Z::kStrict, true},      {"yes", Z::kStrict, true},
                      {"Latency", Z::kStrict, true},  {"2", Z::kLatency, true},     {"20", Z::kLatency, true},
                      {"latency", Z::kLatency, true}};
  for (const Row& r : rows) {
    const Z::Request q = Z::requestOf(r.v);
    check(q.mode == r.mode && q.explicitForm == r.explicitForm,
          std::string("Z20: SR_NATIVE_ZPD ") + (r.v ? std::string("=") + r.v : std::string("unset")) + " -> " +
              Z::modeName(r.mode) + (r.explicitForm ? " (explicit)" : " (the baked default form)"));
  }
}

void z21FreshLatch() {
  struct Case {
    const char* what;
    const char* value;  // nullptr = unset
    int mode, expl, port;
    const char* line;     // in the child's output
    const char* notLine;  // never in it
  };
  const Case cases[] = {
      {"unset", nullptr, 2, 0, 1, "[NATIVE_ZPD] SR_NATIVE_ZPD on (the baked default; latency)", "SR_NATIVE_ZPD on (="},
      {"=0", "0", 0, 1, 0, "[NATIVE_ZPD] SR_NATIVE_ZPD off (=0)", "SR_NATIVE_ZPD on ("},
      {"=00", "00", 0, 1, 0, "[NATIVE_ZPD] SR_NATIVE_ZPD off (=00)", "SR_NATIVE_ZPD on ("},
      {"=1", "1", 1, 1, 1, "[NATIVE_ZPD] SR_NATIVE_ZPD on (=1; strict)", "(the baked default"},
      {"=strict", "strict", 1, 1, 1, "[NATIVE_ZPD] SR_NATIVE_ZPD on (=strict; strict)", "(the baked default"},
      {"=2", "2", 2, 1, 1, "[NATIVE_ZPD] SR_NATIVE_ZPD on (=2; latency)", "(the baked default"},
      {"=latency", "latency", 2, 1, 1, "[NATIVE_ZPD] SR_NATIVE_ZPD on (=latency; latency)", "(the baked default"}};
  char* self = nullptr;
  const bool haveSelf = _get_pgmptr(&self) == 0 && self && *self;
  const char* old = std::getenv("SR_NATIVE_ZPD");
  const std::pair<bool, std::string> saved(old != nullptr, old ? old : "");
  for (const Case& c : cases) {
    _putenv_s("SR_NATIVE_ZPD", c.value ? c.value : "");  // "" removes the variable
    std::string out;
    int rc = -1;
    if (haveSelf) {
      const std::string cmd = std::string("\"\"") + self + "\" --native-zpd-test latch 2>&1\"";
      if (FILE* f = _popen(cmd.c_str(), "r")) {
        char buf[4096];
        while (std::fgets(buf, sizeof buf, f)) out += buf;
        rc = _pclose(f);
      }
    }
    int mode = -1, expl = -1, port = -1;
    const size_t at = out.find("[NATIVE_ZPD_LATCH] ");
    const bool parsed = at != std::string::npos &&
                        std::sscanf(out.c_str() + at, "[NATIVE_ZPD_LATCH] mode=%d explicit=%d port=%d", &mode, &expl,
                                    &port) == 3;
    const bool ok = rc == 0 && parsed && mode == c.mode && expl == c.expl && port == c.port &&
                    out.find(c.line) != std::string::npos && out.find(c.notLine) == std::string::npos;
    check(ok, std::string("Z21: a fresh process, SR_NATIVE_ZPD ") + c.what + " -> " + Z::modeName(Z::Mode(c.mode)) +
                  (c.port ? ", the producer port on" : ", the producer port off") + ", the boot line naming the form");
    if (!ok)
      std::printf("  child rc=%d parsed=%d mode=%d explicit=%d port=%d; output: %.400s\n", rc, int(parsed), mode, expl,
                  port, out.c_str());
  }
  _putenv_s("SR_NATIVE_ZPD", saved.first ? saved.second.c_str() : "");
}

// Z22: the guest's own Issue writes the PM4 (the port off for the lifecycle tests); the record arms carry what the
// port's records do at that point - the RB_SAMPLE_COUNT_ADDR run and kRkZpd {initiator}.
struct Z22Out {
  std::vector<std::pair<uint8_t, uint32_t>> markers;
  uint64_t issued = 0, ended = 0, events = 0, native = 0, op5B = 0;
  uint32_t initiator = 0;
  bool ok = true;
  bool operator==(const Z22Out& o) const {
    return markers == o.markers && issued == o.issued && ended == o.ended && events == o.events &&
           native == o.native && op5B == o.op5B && initiator == o.initiator && ok == o.ok;
  }
};
Z22Out z22Arm(int how, Z::Mode mode) {
  Z::resetForTest();
  Z::setModeForTest(mode);
  resetDevice(0x301u);
  auto& q = G::getDrawCommandQueue();
  while (q.front()) q.popFront();
  G::Pm4Engine e;
  e.cpMode = true;
  e.submitDraws = true;
  e.physToHost = physHost;
  e.cp.flush = testFlush;
  Z22Out o;
  for (bool begin : {true, false}) {
    const Pm4Seen s = guestIssue(begin);
    if (how == 0) {
      cpExecute(e, s);
    } else {
      const uint32_t w[4] = {(1u << 16) | 0x2325u, _byteswap_ulong(s.addr),
                             sr::native::flush::recTag(sr::native::flush::kRkZpd, 0u, 0u), s.initiator};
      G::Pm4Engine::NativeRecordStats st;
      if (how == 1)
        o.ok = e.nativeRecord(w, w + 4, st, false) && o.ok;
      else
        e.nativeRecordOracle(w, w + 4, st, false);
      o.ok = o.ok && !st.bad && !e.nativeRecordFailed;
    }
    uint8_t op = 0;
    uint32_t id = 0;
    while (q.front())
      if (popMarker(op, id)) o.markers.emplace_back(op, id);
  }
  const Z::Stats st = Z::stats();
  o.issued = st.issued;
  o.ended = st.ended;
  o.events = e.cpZpdEvents;
  o.native = e.cpZpdNative;
  o.op5B = e.opcodeHist[0x5B];
  o.initiator = G::getXenosRegisters().read(0x21F9u);
  return o;
}
void z22RecordApply() {
  for (Z::Mode mode : {Z::kLatency, Z::kOff}) {
    const std::string m = Z::modeName(mode);
    const Z22Out pm4 = z22Arm(0, mode), rec = z22Arm(1, mode), orc = z22Arm(2, mode);
    if (mode != Z::kOff)
      check(pm4.markers.size() == 2u && pm4.markers[0].first == Z::kBegin && pm4.markers[1].first == Z::kEnd &&
                pm4.markers[0].second == pm4.markers[1].second && pm4.issued == 1u && pm4.ended == 1u &&
                pm4.events == 2u && pm4.native == 2u && pm4.op5B == 2u && pm4.initiator == 21u,
            "Z22 (" + m + "): the guest's PM4 executed - one query, its BEGIN and END markers queued, two "
            "EVENT_WRITE_ZPDs taken by the native path, VGT_EVENT_INITIATOR 21");
    else
      check(pm4.markers.empty() && pm4.issued == 0u && pm4.events == 2u && pm4.native == 0u && pm4.op5B == 2u &&
                pm4.initiator == 21u,
            "Z22 (off): the guest's PM4 executed - counted, VGT_EVENT_INITIATOR 21, no native answer, no marker");
    check(rec == pm4, "Z22 (" + m + "): the kRkZpd record through the record engine's consumer (nativeRecord, the "
                      "release path) equals the packet: markers, the logical query, the counters, the initiator");
    check(orc == pm4, "Z22 (" + m + "): the kRkZpd record through the development oracle (nativeRecordOracle) equals "
                      "the packet");
  }
}

void z23PortFollowsMode() {
  NF::setZpdPortForTest(-1);  // the real gate: SR_NATIVE_ZPD (latched at this process's first call - Z1's Issue, unset)
  const bool envUnset = !std::getenv("SR_NATIVE_ZPD");
  Z::setModeForTest(Z::kOff);
  const bool offWhenOff = !NF::zpdPortOn();
  Z::setModeForTest(Z::kLatency);
  const bool onWhenLatency = NF::zpdPortOn();
  Z::setModeForTest(Z::kStrict);
  const bool onWhenStrict = NF::zpdPortOn();
  NF::setZpdPortForTest(0);
  check(envUnset && offWhenOff && onWhenLatency && onWhenStrict,
        "Z23: the producer port follows the mode: SR_NATIVE_ZPD unset (the gate's default ON), the port off exactly "
        "when the mode is off (strict / latency on)");
}

// Z24's child (`--native-zpd-test freeze`, SR_DIAG_SIM_FREEZE=1:900:0 and SR_PRESENT_DUMP_FREEZE=2 set by the parent):
// ONE real hold of the sim freeze diagnostic (diagAfterTick -> simFreezeAfterTick: start 0 s, the first tick holds 900 ms) on
// this thread, every line it logs captured (Log::setThreadCapture) and printed for the parent. A helper thread notes
// two port calls and one decline only AFTER the hold's begin edge was logged, so the two edges' counters must differ.
std::atomic<int> g_z24Begun{0};
void z24Capture(void* user, const char* message) {
  static_cast<std::string*>(user)->append(message).append("\n");
  if (std::strstr(message, "[NATIVE_ZPD] freeze #") && std::strstr(message, " begin mode=")) g_z24Begun.store(1);
}
int runNativeZpdFreeze() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  K::registerStubs();
  Z::resetForTest();
  Z::setModeForTest(Z::kLatency);
  for (int i = 0; i < 3; i++) Z::notePort(true);
  Z::notePort(false);
  constexpr uint32_t kMgr = 0x70900000u;  // a stand-in FGGameSim loop manager (its ticks read as 0)
  K::writeGuestBE32(0x8296D8E0u, kMgr);   // kSimMgrPtr: the hold applies to the sim loop's manager only
  std::string dumps;  // Z25: the helper's freeze dump lines (its own thread's capture)
  std::thread helper([&dumps] {
    for (int i = 0; i < 5000 && !g_z24Begun.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Z::notePort(true);
    Z::notePort(true);
    Z::notePort(false);
    // Z25: two presents dumped 500 ms into the hold (past the dump's 400 ms settle, inside the 900 ms hold) - the
    // first presenting a frame stamped {3 BEGINs, 4 record events, 0 packets, frame 7}, the second none
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    sr::Log::setThreadCapture(z24Capture, &dumps, true);
    Z::setPresentingStamp(Z::FrameStamp{3u, 4u, 0u, 7u});
    sr::kernel::highfps::freezeDumpWanted(901);
    Z::setPresentingStamp(Z::FrameStamp{});
    sr::kernel::highfps::freezeDumpWanted(902);
    sr::Log::setThreadCapture(nullptr, nullptr, false);
  });
  std::string out;
  sr::Log::setThreadCapture(z24Capture, &out, true);  // suppress: the lines reach the parent once, below
  sr::kernel::highfps::diagAfterTick(kMgr);
  sr::Log::setThreadCapture(nullptr, nullptr, false);
  helper.join();
  std::printf("%s%s[NATIVE_ZPD_FREEZE] done\n", out.c_str(), dumps.c_str());
  return 0;
}

// Z25's in-process half (Astra round 2 on native/zpd-bake, P2-1): gfx/native_zpd's FRAME STAMP - the EVENT_WRITE_ZPD
// events the CP parsed in one frame, taken at its swap packet. Through Z22's arms (the CP engine itself): the literal
// packet counts as pm4=, the kRkZpd record - through the release consumer and the development oracle - as rec=, each
// query's BEGIN as zpd=; the mode off opens no BEGIN but still counts the form. Each take zeroes the counts and advances
// the frame ordinal; with the stamp off (the default path) nothing is counted and a take is empty. The presenting
// stamp is the presenting thread's own.
struct Z25Got {
  uint32_t begins, rec, pm4, frame;
};
Z25Got z25Take() {
  const Z::FrameStamp s = Z::takeFrameStamp();
  return Z25Got{s.begins, s.rec, s.pm4, s.frame};
}
bool z25Is(const Z25Got& g, uint32_t b, uint32_t r, uint32_t p, uint32_t f) {
  return g.begins == b && g.rec == r && g.pm4 == p && g.frame == f;
}
void z25FrameStamp() {
  Z::setFrameStampForTest(1);
  for (Z::Mode mode : {Z::kLatency, Z::kOff}) {
    const std::string m = Z::modeName(mode);
    const uint32_t b = mode == Z::kOff ? 0u : 1u;
    z22Arm(0, mode);
    const Z25Got pm4 = z25Take();
    z22Arm(1, mode);
    const Z25Got rec = z25Take();
    z22Arm(2, mode);
    const Z25Got orc = z25Take();
    check(z25Is(pm4, b, 0u, 2u, 1u), "Z25 (" + m + "): the guest's literal packet - one frame stamped zpd=" +
                                          std::to_string(b) + " rec=0 pm4=2 (its BEGIN and END), frame 1");
    check(z25Is(rec, b, 2u, 0u, 1u), "Z25 (" + m + "): the kRkZpd record through the release consumer (nativeRecord) - "
                                     "rec=2 pm4=0 (the port's form)");
    check(z25Is(orc, b, 2u, 0u, 1u), "Z25 (" + m + "): the kRkZpd record through the development oracle "
                                     "(nativeRecordOracle) - rec=2 pm4=0");
  }
  Z::resetForTest();
  for (int i = 0; i < 3; i++) Z::noteSource(true);
  Z::noteSource(false);
  const Z25Got f1 = z25Take(), f2 = z25Take();
  check(z25Is(f1, 0u, 3u, 1u, 1u) && z25Is(f2, 0u, 0u, 0u, 2u),
        "Z25: a take zeroes the frame's counts and advances the ordinal (frame 1 rec=3 pm4=1, frame 2 empty)");
  Z::setFrameStampForTest(0);
  Z::resetForTest();
  Z::noteSource(true);
  Z::noteSource(false);
  const Z25Got off = z25Take();
  Z::setFrameStampForTest(1);
  const Z25Got after = z25Take();
  check(z25Is(off, 0u, 0u, 0u, 0u) && z25Is(after, 0u, 0u, 0u, 1u),
        "Z25: with the stamp off (the default path) nothing is counted and a take is empty; switched on, the next "
        "frame holds none of the events noted while off");
  Z::setPresentingStamp(Z::FrameStamp{5u, 0u, 6u, 9u});
  uint32_t other = 99u;
  std::thread([&other] { other = Z::presentingStamp().frame; }).join();
  const uint32_t mine = Z::presentingStamp().frame;
  Z::setPresentingStamp(Z::FrameStamp{});
  check(mine == 9u && other == 0u && Z::presentingStamp().frame == 0u,
        "Z25: the presenting stamp is the presenting thread's own (another thread sees none; cleared after the present)");
  Z::setFrameStampForTest(-1);
  Z::resetForTest();
}

void z26FrameStampHandoff() {
  Z::resetForTest();
  Z::setModeForTest(Z::kOff);
  resetSeen();
  auto same = [](const Z::FrameStamp& a, const Z::FrameStamp& b) {
    return a.begins == b.begins && a.rec == b.rec && a.pm4 == b.pm4 && a.frame == b.frame;
  };
  const Z::FrameStamp first{3u, 4u, 0u, 41u}, second{5u, 0u, 6u, 42u};
  const uint32_t caller = GetCurrentThreadId();
  Z::setPresentingStamp(first);
  g_be->pm4PresentFrame(0x00A00000u, 1280u, 720u, 1280u, false);
  {
    auto& t = g_be->zpdTest();
    std::lock_guard<std::mutex> lk(t.mu);
    check(t.frames.size() == 1u && same(t.frames[0].stamp, first) &&
              t.frames[0].threadId == caller && same(Z::presentingStamp(), first),
          "Z26: synchronous frame keeps its stamp and restores caller TLS");
  }
  resetSeen();
  // The partial frame deliberately installs an empty stamp on this same thread.
  // Unlike a present, its incoming stamp differs from the caller's TLS: restoring
  // the caller is observable and a missing RAII restoration cannot pass unnoticed.
  g_be->zpdTestPushDraw(0, false);
  const bool syncDrained = g_be->pm4ZpdFlushPartial();
  {
    auto& t = g_be->zpdTest();
    std::lock_guard<std::mutex> lk(t.mu);
    check(syncDrained && t.frames.size() == 1u && t.frames[0].partial &&
              same(t.frames[0].stamp, {}) && t.frames[0].threadId == caller &&
              same(Z::presentingStamp(), first),
          "Z26: synchronous partial clears its own stamp and restores nonempty caller TLS");
  }
  resetSeen();
  g_be->zpdTest().delayMs = 15;  // leave time for the producer to change its TLS while a frame is queued
  g_be->zpdTestRenderThread(true);
  g_be->pm4PresentFrame(0x00A00000u, 1280u, 720u, 1280u, false);
  Z::setPresentingStamp(second);
  g_be->pm4PresentFrame(0x00A00000u, 1280u, 720u, 1280u, false);
  Z::setPresentingStamp({});
  g_be->pm4PresentFrame(0x00A00000u, 1280u, 720u, 1280u, false);
  // A partial set uses the same queue, but must not inherit this caller's stamp.
  Z::setPresentingStamp(second);
  const bool drained = g_be->pm4ZpdFlushPartial();
  check(drained && same(Z::presentingStamp(), second),
        "Z26: partial queue drain completes without changing caller TLS");
  g_be->zpdTestRenderThread(false);
  g_be->zpdTest().delayMs = 0;
  {
    auto& t = g_be->zpdTest();
    std::lock_guard<std::mutex> lk(t.mu);
    const bool shape = t.frames.size() == 4u && !t.frames[0].partial && !t.frames[1].partial &&
                       !t.frames[2].partial && t.frames[3].partial;
    bool otherThread = shape;
    for (const auto& f : t.frames) otherThread = otherThread && f.threadId != caller;
    check(otherThread, "Z26: production queue renders three presents and one partial on another thread");
    check(shape && same(t.frames[0].stamp, first) && same(t.frames[1].stamp, second),
          "Z26: queued presents retain their own complete stamp after producer TLS changes");
    check(shape && same(t.frames[2].stamp, {}) && same(t.frames[3].stamp, {}),
          "Z26: unstamped frame and partial set never borrow an earlier present stamp");
  }
  Z::setPresentingStamp({});
  Z::resetForTest();
}

// Z24: the occlusion queries' census at each edge of a sim-freeze hold (kernel/highfps freezeEdgeCensus; Astra round
// 1 P2-2 on native/zpd-bake): in a FRESH process (the freeze config latches at its first use) one real hold logs
// [SIM_FREEZE] #1 begin, then `[NATIVE_ZPD] freeze #1 begin mode=latency ...` with the counters of that moment
// (port=3 portDecl=1), [SIM_FREEZE] #1 end, then `... freeze #1 end ...` with the counters after the hold (port=5
// portDecl=2): the two lines bracket the hold's frozen presents exactly. The bake witness's select ((census|final|exit)
// mode=) never reads them.
void z24FreezeEdges() {
  char* self = nullptr;
  const bool haveSelf = _get_pgmptr(&self) == 0 && self && *self;
  const char* old = std::getenv("SR_DIAG_SIM_FREEZE");
  const std::pair<bool, std::string> saved(old != nullptr, old ? old : "");
  const char* oldDump = std::getenv("SR_PRESENT_DUMP_FREEZE");
  const std::pair<bool, std::string> savedDump(oldDump != nullptr, oldDump ? oldDump : "");
  _putenv_s("SR_DIAG_SIM_FREEZE", "1:900:0");    // one 900-ms hold (Z25's dumps 500 ms into it)
  _putenv_s("SR_PRESENT_DUMP_FREEZE", "2");      // Z25: the dense freeze dump (the frame stamp on)
  std::string out;
  int rc = -1;
  if (haveSelf) {
    const std::string cmd = std::string("\"\"") + self + "\" --native-zpd-test freeze 2>&1\"";
    if (FILE* f = _popen(cmd.c_str(), "r")) {
      char buf[4096];
      while (std::fgets(buf, sizeof buf, f)) out += buf;
      rc = _pclose(f);
    }
  }
  _putenv_s("SR_DIAG_SIM_FREEZE", saved.first ? saved.second.c_str() : "");
  _putenv_s("SR_PRESENT_DUMP_FREEZE", savedDump.first ? savedDump.second.c_str() : "");
  const size_t fb = out.find("[SIM_FREEZE] #1 begin"), zb = out.find("[NATIVE_ZPD] freeze #1 begin mode=latency ");
  const size_t fe = out.find("[SIM_FREEZE] #1 end"), ze = out.find("[NATIVE_ZPD] freeze #1 end mode=latency ");
  auto lineAt = [&](size_t at) {
    return at == std::string::npos ? std::string() : out.substr(at, out.find('\n', at) - at);
  };
  const std::string lb = lineAt(zb), le = lineAt(ze);
  auto endsWith = [](const std::string& s, const char* t) {
    const size_t n = std::strlen(t);
    return s.size() >= n && s.compare(s.size() - n, n, t) == 0;
  };
  const bool order = fb != std::string::npos && zb != std::string::npos && fe != std::string::npos &&
                     ze != std::string::npos && fb < zb && zb < fe && fe < ze;
  check(rc == 0 && order, "Z24: one sim-freeze hold logs [SIM_FREEZE] #1 begin, the zpd census `freeze #1 begin`, "
                          "[SIM_FREEZE] #1 end, the zpd census `freeze #1 end` - in that order");
  check(endsWith(lb, " port=3 portDecl=1") && lb.find(" issued=0 ") != std::string::npos,
        "Z24: the begin edge carries the counters of the hold's start (port=3 portDecl=1)");
  check(endsWith(le, " port=5 portDecl=2"),
        "Z24: the end edge carries the counters after the hold (port=5 portDecl=2: the helper's calls during it)");
  check(out.find("[NATIVE_ZPD] freeze #2") == std::string::npos && out.find("[NATIVE_ZPD] census mode=") == std::string::npos,
        "Z24: one hold, two edge lines, no other census line");
  // Z25's live half (Astra round 2 P2-1): the freeze dump line of a present carries ITS frame's stamp, and a present
  // without one says so
  const std::string d1 = lineAt(out.find("[SIM_FREEZE_DUMP] freeze=1 k=0 present=901 "));
  const std::string d2 = lineAt(out.find("[SIM_FREEZE_DUMP] freeze=1 k=1 present=902 "));
  check(endsWith(d1, " s zpd=3 rec=4 pm4=0 fswap=7"),
        "Z25: a present dumped in a hold prints its frame's stamp: [SIM_FREEZE_DUMP] ... zpd=3 rec=4 pm4=0 fswap=7");
  check(endsWith(d2, " s zpd=-"), "Z25: a dumped present without a stamp prints zpd=- (never another frame's)");
  if (!(rc == 0 && order && endsWith(lb, " port=3 portDecl=1") && endsWith(le, " port=5 portDecl=2") &&
        endsWith(d1, " s zpd=3 rec=4 pm4=0 fswap=7") && endsWith(d2, " s zpd=-")))
    std::printf("  child rc=%d; output: %.1600s\n", rc, out.c_str());
}
}  // namespace

int runNativeZpdTest(const char* mode) {
  if (mode && !std::strcmp(mode, "latch")) return runNativeZpdLatch();  // Z21's fresh-process child
  if (mode && !std::strcmp(mode, "freeze")) return runNativeZpdFreeze();  // Z24's fresh-process child
  if (mode) {
    std::printf("FAIL: unknown mode %s (none | latch | freeze)\n[NATIVE_ZPD_TEST] 0 passed, 1 failed\n", mode);
    return 2;
  }
  std::printf("[NATIVE_ZPD_TEST] the occlusion-query lifecycle: guest Issue / GetData bodies vs gfx/native_zpd and "
              "the D3D12 backend's query paths\n");
  // The lifecycle tests drive the guest bodies (Z1's PM4): the bake's producer port of 82101E08 stays off here (Z23
  // turns it on; --zpd-port-test proves it against the SDK body).
  NF::setZpdPortForTest(0);
  // pm4PresentFrame must not start the real render thread (it needs a device); the tests start the seam's.
  _putenv_s("SR_PM4_RENDER_THREAD", "0");
  _putenv_s("SR_RES_SCALE", "3");
  G::resscale::configure(3840, 2160);
  const uint32_t area = G::resscale::get().area();
  std::printf("  (the backend tests' draw resolution scale: %ux%u, area %u)\n", G::resscale::get().x,
              G::resscale::get().y, area);
  K::registerStubs();
  G::Pm4Engine eng;
  eng.cpMode = true;
  eng.submitDraws = true;
  eng.physToHost = physHost;
  eng.cp.flush = testFlush;
  auto& q = G::getDrawCommandQueue();
  while (q.front()) q.popFront();
  g_be = new G::D3D12Backend();  // never freed (no teardown at exit); no init: the seam stands in for the device
  g_be->zpdTestEnable();
  g_be->zpdTest().onFrame = onFrame;
  G::setPm4LiveBackend(g_be);

  // ── Z0: the control - SR_NATIVE_ZPD=0 (the guest path; the default before the bake): no hook (installed only
  // when on), no marker, the fence lands, the slot keeps the sentinel, GetData never completes ──────────────────
  Z::resetForTest();
  Z::setModeForTest(Z::kOff);
  eng.cp.zpdFence = nullptr;
  resetDevice(0x81u);
  resetSeen();
  {
    issueOne(eng, true);
    issueOne(eng, false);
    const bool noMarker = q.front() == nullptr;
    wr(kDev + 0x2A1Cu, 0x83u);
    execFenceStore(eng, 0x81u);
    check(noMarker && g_partials == 0 && rd(kFbVa) == 0x81u && endSentinel() && getDataWrapper() == 0xFFFFFFFFu &&
              Z::stats().issued == 0,
          "Z0: gate off (dev today): no marker, the fence lands, the sentinel stays, GetData -1 for good");
  }

  // ── Z1-Z4: strict, the CP path, the synchronous backend ───────────────────────────────────────────────────────
  Z::resetForTest();
  Z::setModeForTest(Z::kStrict);
  eng.cp.zpdFence = prodZpdFence;
  const uint32_t s0 = 0x101u;
  resetDevice(s0);
  resetSeen();
  g_be->zpdTest().samplesPerDraw = 9;  // one guest pixel's worth of host samples at 3x3
  g_be->zpdTest().delayMs = 0;
  Pm4Seen b, e;
  const uint32_t id = queryIntoSet(eng, {G::edram::kMsaa4X}, &b, &e);
  check(rd(kQ + 0x90u) == 1u && b.zpd && b.initiator == 21u && b.addr == kSlotPhys + 0x20u,
        "Z1: Issue(BEGIN): one tile, RB_SAMPLE_COUNT_ADDR = slot+0x20, EVENT_WRITE_ZPD {21}");
  check(endSentinel(), "Z1: Issue(BEGIN) presets the END record to 0xFFFFFEED x4");
  check(e.zpd && e.initiator == 21u && e.addr == kSlotPhys && rd(kQ + 0x10u) == 1u &&
            rd(kQ + 0x14u) == rd(kDev + 0x2A1Cu),
        "Z1: Issue(END): RB_SAMPLE_COUNT_ADDR = slot, EVENT_WRITE_ZPD {21}, q+0x14 = the open submission");
  check(id != 0 && g_be->zpdTestSetSize() == 5u,
        "Z2: the CP queues a BEGIN then an END marker for one logical query; the drain puts them in the open set");
  check(Z::fenceNeedsFlush(), "Z2: a drained END makes the next EVENT_WRITE_SHD its covering-fence check");
  wr(kDev + 0x2A1Cu, s0 + 2u);  // the close: S moves on; the query's fence is s0, not yet written
  check(getDataWrapper() == 0xFFFFFFFFu, "Z3: GetData while the fence is pending -> -1 (S_FALSE)");
  wr(kFbVa, s0);  // what a late answer looks like: the fence passed, the slot unwritten
  check(getDataWrapper() == 0xFFFFFFFFu && endSentinel(),
        "Z3: fence passed + sentinel -> -1 (S_FALSE): the guest SEES a write that comes after the fence");
  wr(kFbVa, s0 - 2u);
  const uint32_t n4 = expectGuest(9u * 4u, area);  // 4X: 4 guest samples per pixel
  execFenceStore(eng, s0);
  check(g_partials == 1 && g_fenceInside == s0 - 2u && g_slotWrittenInside,
        "Z4: the store's covering fence (production cpZpdFence -> pm4ZpdFlushPartial) renders the partial frame and "
        "writes the slot BEFORE the fence word moves");
  check(rd(kFbVa) == s0, "Z4: then the fence word lands (fb+0 = S)");
  check(le(kSlotVa + 0u) == n4 && rd(kSlotVa + 4u) == Z::kSentinel && rd(kSlotVa + 8u) == Z::kSentinel &&
            rd(kSlotVa + 12u) == Z::kSentinel && le(kSlotVa + 16u) == n4 && le(kSlotVa + 20u) == 0u &&
            le(kSlotVa + 32u) == 0u && rd(kSlotVa + 36u) == Z::kSentinel && rd(kSlotVa + 40u) == Z::kSentinel &&
            rd(kSlotVa + 44u) == Z::kSentinel && le(kSlotVa + 48u) == 0u,
        "Z4: slot words: END {n, S, S, S, n, 0, 0, 0}, BEGIN {0, S, S, S, 0, 0, 0, 0}");
  check(getDataWrapper() == n4, "Z4: GetData through the wrapper -> the 4X count (host * 4 / area)");
  {
    const Raw r = getDataRaw();
    check(r.ok && r.o[0] == n4 && r.o[1] == 0u && r.o[2] == 0u && r.o[3] == n4,
          "Z4: D3DQueryGetData itself: S_OK, {ZPass n, ZFail 0, StencilFail 0, Total n}");
  }
  Z::Stats st = Z::stats();
  check(st.issued == 1 && st.ended == 1 && st.drained == 1 && st.resolved == 1 && st.written == 1 && st.late == 0 &&
            st.flushes == 1 && st.flushFail == 0 && st.fallback == 0 && !Z::fenceNeedsFlush(),
        "Z4: strict: issued 1, ended 1, resolved 1, written 1, late 0, flushes 1, no fallback");
  check(st.viaPartial == 1 && st.viaPresent == 0,
        "Z4: the slot was written by the partial frame (viaPartial 1), not a present's collect (viaPresent 0)");
  check(g_be->zpdTestSetSize() == 0u, "Z4: the partial frame consumed the set");
  // ── Z4b: the swap right after the fence, no draw between ──
  g_be->pm4PresentFrame(0x00A00000u, 1280u, 720u, 1280u, false);
  {
    auto& t = g_be->zpdTest();
    std::lock_guard<std::mutex> lk(t.mu);
    const bool shape = t.frames.size() == 2u && t.frames[0].partial && t.frames[0].batches == 5u &&
                       t.frames[0].markers == 2u && t.frames[0].draws == 1u && t.frames[0].helpers == 2u &&
                       !t.frames[1].partial;
    check(shape, "Z4b: one partial frame (BEGIN, helper, draw, helper, END), then the swap's frame");
    check(shape && t.frames[1].batches == 0u,
          "Z4b: the swap after the fence renders an empty set (the partial's batches are not rendered again)");
    // ── Z4c: the query brackets the guest draw only ──
    const auto& r = t.rec;
    bool adj = !r.empty();
    int open = 0, helpersInside = 0, begins = 0;
    for (size_t i = 0; i < r.size(); i++) {
      if (r[i].first == 'B') {
        begins++;
        open++;
        adj = adj && i + 1 < r.size() && r[i + 1].first == 'E' && r[i + 1].second == r[i].second;
      } else if (r[i].first == 'E') {
        open--;
      } else if (r[i].first == 'H' && open) {
        helpersInside++;
      }
    }
    std::string seq;
    for (const auto& x : r) seq += x.first;
    std::printf("      query commands: %s\n", seq.c_str());
    check(r.size() == 4u && r[0].first == 'H' && r[1].first == 'B' && r[2].first == 'E' && r[3].first == 'H',
          "Z4c: the recorded order is helper, BeginQuery, EndQuery + resolve, helper (nothing at the BEGIN marker)");
    check(adj && begins == 1 && helpersInside == 0 && open == 0,
          "Z4c: every BeginQuery is immediately followed by its EndQuery: no helper pass inside a query");
  }
  resetSeen();
  execFenceStore(eng, s0 + 2u);
  check(g_partials == 0, "Z4: the next fence store has nothing to cover (no partial frame)");

  // ── Z5: the latency model ─────────────────────────────────────────────────────────────────────────────────────
  Z::resetForTest();
  Z::setModeForTest(Z::kLatency);
  resetDevice(0x201u);
  resetSeen();
  uint32_t idL = 0;
  {
    issueOne(eng, true);
    uint8_t op = 0;
    popMarker(op, idL);
    issueOne(eng, false);
    uint32_t id2 = 0;
    popMarker(op, id2);
    if (id2 != idL) idL = 0;
  }
  wr(kDev + 0x2A1Cu, 0x203u);
  check(idL && Z::fenceNeedsFlush(), "Z5: the END drained");
  execFenceStore(eng, 0x201u);  // the production hook: latency = covered, no partial frame; the store goes first
  check(g_partials == 0 && rd(kFbVa) == 0x201u && Z::stats().covered == 1 && !Z::fenceNeedsFlush(),
        "Z5: the latency model's hook marks it covered and the store lands without a flush");
  check(getDataWrapper() == 0xFFFFFFFFu, "Z5: fence passed, slot unwritten -> -1 (S_FALSE, the not-ready path)");
  renderQuery(idL, 0);  // the query point is behind a building
  Z::stamp(5);          // its submission's fence
  Z::collect(4, g_readback, kArea);
  check(endSentinel(), "Z5: the submission has not completed (fence 4 < 5): nothing written yet");
  Z::collect(5, g_readback, kArea);
  check(getDataWrapper() == 0u, "Z5: completed -> GetData 0 (0 samples: the HUD's 127-alpha 'occluded' answer)");
  st = Z::stats();
  check(st.written == 1 && st.late == 1 && st.zero == 1, "Z5: written 1, late 1 (after its covering fence), zero 1");

  // ── Z6: stale ─────────────────────────────────────────────────────────────────────────────────────────────────
  Z::resetForTest();
  Z::setModeForTest(Z::kLatency);
  resetDevice(0x301u);
  auto issueLogical = [&]() {
    uint8_t op = 0;
    uint32_t a = 0, c = 0;
    issueOne(eng, true);
    popMarker(op, a);
    issueOne(eng, false);
    popMarker(op, c);
    return a == c ? a : 0u;
  };
  const uint32_t idA = issueLogical();
  renderQuery(idA, 90);
  const uint32_t idB = issueLogical();  // the guest re-Begins the same query object
  check(idA && idB && idB != idA && endSentinel(), "Z6: a second Begin on the slot presets the sentinel again");
  Z::stamp(0);
  Z::collect(UINT64_MAX, g_readback, kArea);  // A's answer arrives now: stale
  check(endSentinel() && Z::stats().stale == 1, "Z6: the first query's count is dropped (stale 1), sentinel kept");
  renderQuery(idB, 18);
  Z::stamp(0);
  Z::collect(UINT64_MAX, g_readback, kArea);
  check(le(kSlotVa) == 2u && Z::stats().written == 1, "Z6: the current query writes (18 / 9 = 2)");

  // ── Z18: a re-issued query's slot between the guest's Issue and the CP's BEGIN (Astra merge review P1) ───────────
  // Latency mode: the HUD replaces a marker key and re-issues its still-pending query. The guest's Issue presets the
  // END record; query A's count lands BEFORE the CP parses query B's BEGIN (A is still the slot's current query); then
  // B's BEGIN, B's END, B's fence passes with B unresolved. GetData must answer "not ready", never A's count.
  {
    Z::resetForTest();
    Z::setModeForTest(Z::kLatency);
    resetDevice(0x401u);
    uint8_t op = 0;
    uint32_t a = 0, c = 0;
    cpExecute(eng, guestIssue(true));
    popMarker(op, a);
    cpExecute(eng, guestIssue(false));
    popMarker(op, c);
    const uint32_t idA = a == c ? a : 0u;
    renderQuery(idA, 90);                       // A's draw: 90 host samples -> 10 guest samples at area 9
    const Pm4Seen bBegin = guestIssue(true);    // the guest re-issues: its Issue presets the END record
    const bool presetB = endSentinel();
    Z::stamp(0);
    Z::collect(UINT64_MAX, g_readback, kArea);  // A's count arrives before the CP parsed B's BEGIN
    const bool aLanded = le(kSlotVa) == 10u;    // the interval: A's count over B's preset
    cpExecute(eng, bBegin);                     // the CP parses B's BEGIN
    popMarker(op, a);
    check(idA && presetB && aLanded && endSentinel(),
          "Z18: B's BEGIN re-arms the slot A's late count overwrote after B's Issue (the sentinel is back)");
    cpExecute(eng, guestIssue(false));
    popMarker(op, c);
    const uint32_t idB = a == c ? a : 0u;
    const uint32_t sB = rd(kQ + 0x14u);         // B's fence: the submission its END closed in
    wr(kDev + 0x2A1Cu, sB + 2u);
    wr(kFbVa, sB);                              // ... passed, B still unresolved (latency)
    check(idB && idB != idA && getDataWrapper() == 0xFFFFFFFFu,
          "Z18: B's fence passed, B unresolved: GetData -> -1 (S_FALSE), never A's count");
    renderQuery(idB, 18);
    Z::stamp(0);
    Z::collect(UINT64_MAX, g_readback, kArea);
    check(getDataWrapper() == 2u && Z::stats().written == 2,
          "Z18: then B's own count (18 / 9 = 2) - A's write was its only other");
  }

  // ── Z7: edge cases + normalization ────────────────────────────────────────────────────────────────────────────
  Z::resetForTest();
  Z::setModeForTest(Z::kStrict);
  uint8_t* h = slotHost();
  check(Z::onEvent(kSlotPhys, h).op == Z::kNone && Z::stats().orphan == 1, "Z7: an END with no BEGIN is an orphan");
  const Z::Event b1 = Z::onEvent(kSlotPhys + 0x20u, h);
  const Z::Event b2 = Z::onEvent(kSlotPhys + 0x20u, h);
  check(b1.op == Z::kBegin && b2.op == Z::kBegin && Z::stats().abandoned == 1,
        "Z7: a BEGIN while one is open abandons the open one");
  check(Z::onEvent(0x10u, h).op == Z::kNone && Z::stats().badAddr == 1,
        "Z7: an RB_SAMPLE_COUNT_ADDR whose record base is 0 is refused");
  {
    const Z::Event e7 = Z::onEvent(kSlotPhys, h);  // ends b2
    Z::markerDrained(Z::kEnd, e7.id);
    Z::endRecorded(e7.id, false);  // its BEGIN never reached the list owner: no draw was measured
    Z::stamp(0);
    Z::collect(UINT64_MAX, g_readback, kArea);
    check(e7.op == Z::kEnd && le(kSlotVa) == 1u, "Z7: a query whose BEGIN the render side never saw answers 'visible' (1)");
  }
  {
    const Z::Event bb = Z::onEvent(kSlotPhys + 0x20u, h);
    Z::segmentMissed(bb.id);  // a guest draw with no heap
    const Z::Event ee = Z::onEvent(kSlotPhys, h);
    Z::endRecorded(ee.id, true);
    Z::stamp(0);
    Z::collect(UINT64_MAX, g_readback, kArea);
    check(le(kSlotVa) == 1u && Z::stats().missed == 1, "Z7: a guest draw no host segment covered answers 'visible' (1)");
  }
  {
    const Z::Event bb = Z::onEvent(kSlotPhys + 0x20u, h);
    uint32_t opened = 0;
    for (uint32_t i = 0; i < Z::kMaxSegs + 3u; i++) {
      const uint32_t seg = Z::segmentOpen(bb.id, 1);
      if (seg != Z::kNoSeg) {
        opened++;
        g_readback[seg] = 0;
      }
      Z::segmentClosed(bb.id, seg);
    }
    const Z::Event ee = Z::onEvent(kSlotPhys, h);
    Z::endRecorded(ee.id, true);
    Z::stamp(0);
    Z::collect(UINT64_MAX, g_readback, kArea);
    check(opened == Z::kMaxSegs && le(kSlotVa) == 1u && Z::stats().overSegs == 1,
          "Z7: draws past kMaxSegs leave the query unmeasured -> 'visible' (1), not the partial 0");
  }
  struct N {
    uint64_t s;
    uint32_t a, want;
  } ns[] = {{0, 9, 0},  {1, 9, 1},   {4, 9, 1},  {13, 9, 1}, {14, 9, 2}, {36, 9, 4},
            {90, 9, 10}, {5, 1, 5},  {3, 4, 1},  {6, 4, 2},  {0x1FFFFFFFFull, 1, Z::kMaxCount}};
  bool nok = true;
  for (const N& n : ns) {
    const uint32_t got = Z::normalize(n.s, n.a);
    if (got != n.want) {
      nok = false;
      std::printf("      normalize(%llu, %u) = %u, want %u\n", (unsigned long long)n.s, n.a, got, n.want);
    }
  }
  check(nok, "Z7: normalize: 0 -> 0, else max(1, round(samples / area)), at most 0xEDFEFFFE");
  {
    Slot before = snap();
    Z::writeSlot(slotHost(), 0xFFFFFFFFu);
    check(rd(kSlotVa) != Z::kSentinel && le(kSlotVa) == Z::kMaxCount,
          "Z7: a count is never published as the sentinel (END+0 read big-endian never 0xFFFFFEED)");
    std::memcpy(slotHost(), before.data(), 64);
  }

  // ── Z9: the publication protocol against the generated GetData ──────────────────────────────────────────────
  Z::resetForTest();
  Z::setModeForTest(Z::kOff);  // the guest bodies only
  {
    const uint32_t n9 = 7u;
    freshQueryFencePassed(eng, 0x401u);
    StepLog L;
    L.states.push_back(snap());
    L.real.push_back(getDataRaw());
    Z::writeSlot(slotHost(), n9, stepRead, &L);
    bool allConsistent = true, falseThenOk = true;
    for (size_t k = 0; k < L.real.size(); k++) {
      allConsistent = allConsistent && consistent(L.real[k], n9);
      const bool last = k + 1 == L.real.size();
      falseThenOk = falseThenOk && L.real[k].ok == last;
    }
    check(L.real.size() == Z::kSlotStores + 1u && allConsistent,
          "Z9: GetData called after every one of the slot write's 16 stores: S_FALSE or exactly {n, 0, 0, n}");
    check(falseThenOk, "Z9: S_FALSE until the last store (END+0), S_OK after it");
    // The model of the generated body's loads: equal to the generated body at every state, then every two-phase
    // interleaving of its loads with the stores.
    bool modelMatches = true;
    for (size_t k = 0; k < L.states.size(); k++) {
      const Raw m = model(L.states[k], L.states[k], 0), &r = L.real[k];
      modelMatches = modelMatches && m.ok == r.ok && (!m.ok || std::memcmp(m.o, r.o, sizeof m.o) == 0);
    }
    check(modelMatches, "Z9: the load-order model of 82102DC8 answers what the generated body answers at every state");
    uint32_t cases = 0, bad = 0;
    for (size_t a = 0; a < L.states.size(); a++)
      for (size_t c = a; c < L.states.size(); c++)
        for (int j = 0; j <= kModelLoads; j++) {
          cases++;
          if (!consistent(model(L.states[a], L.states[c], j), n9)) bad++;
        }
    std::printf("      %u interleavings (state a <= b, split j), %u inconsistent\n", cases, bad);
    check(bad == 0, "Z9: every two-phase interleaving of GetData's loads with the stores reads S_FALSE or {n,0,0,n}");
    // The control: the round-1 order on the same query is caught by the same checks.
    freshQueryFencePassed(eng, 0x501u);
    StepLog R;
    round1WriteSlot(slotHost(), n9, &R);
    bool caught = false;
    for (const Raw& r : R.real) caught = caught || !consistent(r, n9);
    check(caught, "Z9 control: the round-1 order (END+4 before END+0) gives an inconsistent S_OK - the check can fail");
  }

  // ── Z10: the console's samples: host * samples-per-pixel / area ─────────────────────────────────────────────
  Z::resetForTest();
  Z::setModeForTest(Z::kStrict);
  resetDevice(0x601u);
  resetSeen();
  g_be->zpdTest().samplesPerDraw = 9;
  const uint32_t id10 = queryIntoSet(eng, {G::edram::kMsaa1X, G::edram::kMsaa2X});
  wr(kDev + 0x2A1Cu, 0x603u);
  execFenceStore(eng, 0x601u);
  const uint32_t n10 = expectGuest(9u * 1u + 9u * 2u, area), n10flat = expectGuest(18u, area);
  st = Z::stats();
  check(id10 && n10 != n10flat && getDataWrapper() == n10 && st.segsBySpp[0] == 1 && st.segsBySpp[1] == 1,
        "Z10: a 1X and a 2X draw of 9 host samples each -> (9*1 + 9*2) / area guest samples (not 18 / area)");

  // ── Z11: strict, the executor's store item, the render thread, 60 ms of GPU time ──────────────────────────────
  g_be->zpdTestRenderThread(true);
  Z::resetForTest();
  Z::setModeForTest(Z::kStrict);
  resetDevice(0x701u);
  resetSeen();
  g_be->zpdTest().samplesPerDraw = 45;
  g_be->zpdTest().delayMs = 60;
  const uint32_t n11 = expectGuest(45u, area);
  const uint32_t id11 = queryIntoSet(eng, {G::edram::kMsaa1X});
  wr(kDev + 0x2A1Cu, 0x703u);
  execStoreItem(eng, 0x701u);
  check(id11 && g_partials == 1 && g_fenceInside == 0x701u - 2u && g_slotWrittenInside,
        "Z11: the executor's EVENT_WRITE_SHD item waits for the render thread's partial frame: the slot is written "
        "before the fence word moves");
  st = Z::stats();
  check(rd(kFbVa) == 0x701u && getDataWrapper() == n11 && st.written == 1 && st.fallback == 0 &&
            st.flushFail == 0 && st.maxWaitUs >= 50000u,
        "Z11: then the store lands; GetData -> the count; the CP waited out the GPU time (no fallback)");
  check(st.lagN == 1u && st.lagMaxUs >= 50000u && st.lagSumUs >= 50000u,
        "Z11: the answer lag (the CP's END -> the slot write) spans the 60 ms of GPU time");

  // ── Z12: the CP path with the render thread ─────────────────────────────────────────────────────────────────
  resetDevice(0x801u);
  resetSeen();
  const uint32_t id12 = queryIntoSet(eng, {G::edram::kMsaa1X});
  wr(kDev + 0x2A1Cu, 0x803u);
  execFenceStore(eng, 0x801u);
  check(id12 && g_partials == 1 && g_fenceInside == 0x801u - 2u && g_slotWrittenInside && rd(kFbVa) == 0x801u &&
            getDataWrapper() == n11,
        "Z12: the CP's EVENT_WRITE_SHD waits for the render thread's partial frame, then stores; GetData -> the count");

  // ── Z13: strict fallback ───────────────────────────────────────────────────────────────────────────────────
  {
    // The same slot as Z12 (its last count n11); the partial outlives the flush timeout.
    wr(kDev + 0x2A1Cu, 0x803u);
    resetSeen();
    g_be->zpdTest().samplesPerDraw = 90;  // a different count: if it were written late the slot would change
    g_be->zpdTest().delayMs = 400;
    g_be->zpdTestFlushTimeout(80);
    const uint32_t id13 = queryIntoSet(eng, {G::edram::kMsaa1X});
    wr(kDev + 0x2A1Cu, 0x805u);
    execFenceStore(eng, 0x803u);
    const bool noFrameYet = g_partials == 0;
    st = Z::stats();
    check(id13 && noFrameYet && rd(kFbVa) == 0x803u && !endSentinel() && getDataWrapper() == n11 &&
              st.flushFail == 1 && st.fallback == 1,
          "Z13: the partial frame timed out: before the store the slot got the strict fallback (its last count), "
          "GetData -> that count, never the sentinel after the fence");
    const bool came = waitPartials(1, 3000);
    st = Z::stats();
    check(came && getDataWrapper() == n11 && st.superseded == 1 && st.written == 2,
          "Z13: the real count arriving after it is dropped (superseded 1): the slot keeps the fallback");
    g_be->zpdTestFlushTimeout(5000);
    g_be->zpdTest().delayMs = 0;
  }
  g_be->zpdTestRenderThread(false);

  // ── Z15: the visibility probe (the synchronous path; the seam stands in for the probe pipelines) ─────────────────
  {
    auto probeCase = [&](uint32_t s, uint64_t answer, uint64_t ref, uint64_t raster,
                         std::initializer_list<uint32_t> draws) {
      Z::resetForTest();
      Z::setModeForTest(Z::kStrict);
      resetDevice(s);
      resetSeen();
      auto& t = g_be->zpdTest();
      t.samplesPerDraw = answer;
      t.refSamples = ref;
      t.rasterSamples = raster;
      const uint32_t id = queryIntoSet(eng, draws, nullptr, nullptr, true);
      wr(kDev + 0x2A1Cu, s + 2u);
      execFenceStore(eng, s);
      return id;
    };
    auto recOf = [&] {
      auto& t = g_be->zpdTest();
      std::lock_guard<std::mutex> lk(t.mu);
      return t.rec;
    };
    // The depth test killed it: the answer 0 while the ref probe (depth ALWAYS, the same pixel shader) counts 6.
    const uint32_t ida = probeCase(0xB01u, 0u, 6u, 6u, {G::edram::kMsaa1X});
    auto r = recOf();
    st = Z::stats();
    const bool order = r.size() == 10u && r[0].first == 'H' && r[1].first == 'B' && r[2].first == 'E' &&
                       r[1].second == r[2].second && r[3].first == 'B' && r[4].first == 'r' && r[5].first == 'E' &&
                       r[3].second == r[4].second && r[4].second == r[5].second && r[3].second != r[1].second &&
                       r[6].first == 'B' && r[7].first == 'R' && r[8].first == 'E' && r[6].second == r[8].second &&
                       r[6].second != r[3].second && r[6].second != r[1].second && r[9].first == 'H';
    check(ida && order, "Z15: the probe draws follow the measured draw's EndQuery, each on its own segment (ref, then "
                        "raster), inside the query and before the helper pass");
    check(ida && getDataWrapper() == 0u && st.segs == 1u && st.zero == 1u,
          "Z15: the probe counts never reach the answer (ref 6, raster 6, the measured draw 0 -> GetData 0, segs 1)");
    check(st.probes == 1u && st.depthKilled == 1u && st.psKilled == 0u && st.noRaster == 0u,
          "Z15: answer 0 with ref > 0 -> depthKilled (the depth / stencil test removed every sample)");
    const uint32_t idb = probeCase(0xB11u, 0u, 0u, 5u, {G::edram::kMsaa1X});
    st = Z::stats();
    check(idb && getDataWrapper() == 0u && st.probes == 1u && st.psKilled == 1u && st.depthKilled == 0u &&
              st.noRaster == 0u,
          "Z15: answer 0, ref 0, raster > 0 -> psKilled (the pixel shader discarded every sample)");
    const uint32_t idc = probeCase(0xB21u, 0u, 0u, 0u, {G::edram::kMsaa1X});
    st = Z::stats();
    check(idc && getDataWrapper() == 0u && st.probes == 1u && st.noRaster == 1u && st.depthKilled == 0u &&
              st.psKilled == 0u,
          "Z15: answer 0, ref 0, raster 0 -> noRaster (the draw covered no sample)");
    const uint32_t idd = probeCase(0xB31u, 9u, 9u, 9u, {G::edram::kMsaa1X});
    st = Z::stats();
    check(idd && getDataWrapper() == expectGuest(9u, area) && st.probes == 1u && st.depthKilled == 0u &&
              st.psKilled == 0u && st.noRaster == 0u && st.nonzero == 1u,
          "Z15: a visible draw answers its own count; probed, no kill counted");
    const uint32_t ide = probeCase(0xB41u, 9u, 4u, 4u, {G::edram::kMsaa1X, G::edram::kMsaa1X});
    r = recOf();
    int refs = 0, rasters = 0, begins = 0;
    for (const auto& x : r) {
      refs += x.first == 'r';
      rasters += x.first == 'R';
      begins += x.first == 'B';
    }
    st = Z::stats();
    check(ide && refs == 1 && rasters == 1 && begins == 4 && getDataWrapper() == expectGuest(18u, area) &&
              st.segs == 2u,
          "Z15: two draws in one query: one probe of each kind (the first draw's), both draws measured (18 / area)");
    // The logical side alone: a probe never takes an answer segment; one probe of each kind; a ref-only 0 -> noRaster.
    Z::resetForTest();
    uint8_t* h = slotHost();
    const Z::Event b15 = Z::onEvent(kSlotPhys + 0x20u, h);
    uint32_t opened = 0;
    const uint32_t r1 = Z::segmentOpenProbe(b15.id, Z::kRef), r2 = Z::segmentOpenProbe(b15.id, Z::kRef);
    const uint32_t q1 = Z::segmentOpenProbe(b15.id, Z::kRaster), q2 = Z::segmentOpenProbe(b15.id, Z::kRaster);
    for (uint32_t i = 0; i < Z::kMaxSegs; i++) {
      const uint32_t seg = Z::segmentOpen(b15.id, 1);
      if (seg == Z::kNoSeg) continue;
      opened++;
      g_readback[seg] = 1;
      Z::segmentClosed(b15.id, seg);
    }
    if (r1 != Z::kNoSeg) g_readback[r1] = 0;
    if (q1 != Z::kNoSeg) g_readback[q1] = 0;
    Z::segmentClosed(b15.id, r1);
    Z::segmentClosed(b15.id, q1);
    const Z::Event e15 = Z::onEvent(kSlotPhys, h);
    Z::endRecorded(e15.id, true);
    Z::stamp(0);
    Z::collect(UINT64_MAX, g_readback, 1u);
    st = Z::stats();
    check(r1 != Z::kNoSeg && r2 == Z::kNoSeg && q1 != Z::kNoSeg && q2 == Z::kNoSeg && opened == Z::kMaxSegs &&
              st.overSegs == 0u && le(kSlotVa) == Z::kMaxSegs && st.segs == Z::kMaxSegs,
          "Z15: one probe of each kind per query; the probes leave all kMaxSegs answer segments to the draws");
    Z::resetForTest();
    const Z::Event b16 = Z::onEvent(kSlotPhys + 0x20u, h);
    const uint32_t a16 = Z::segmentOpen(b16.id, 1), p16 = Z::segmentOpenProbe(b16.id, Z::kRef);
    if (a16 != Z::kNoSeg) g_readback[a16] = 0;
    if (p16 != Z::kNoSeg) g_readback[p16] = 0;
    Z::segmentClosed(b16.id, a16);
    Z::segmentClosed(b16.id, p16);
    const Z::Event e16 = Z::onEvent(kSlotPhys, h);
    Z::endRecorded(e16.id, true);
    Z::stamp(0);
    Z::collect(UINT64_MAX, g_readback, 1u);
    st = Z::stats();
    check(a16 != Z::kNoSeg && p16 != Z::kNoSeg && st.probes == 1u && st.noRaster == 1u && st.lagN == 1u,
          "Z15: a ref probe alone that counts 0 -> noRaster; the answer lag is recorded per written query");
    Z::setModeForTest(Z::kStrict);
  }
  // ── Z8 (here: nothing of the backend tests in flight) ──
  st = Z::stats();
  check(st.inflight == 0 && st.ended == st.resolved + st.lost + st.inflight,
        "Z8: nothing in flight: ended == resolved + lost + inflight");
  Z::logLine("test");
  {
    // No backend at all: strict still answers before the store (a fresh slot history: 1).
    G::retirePm4LiveBackend(g_be);
    Z::resetForTest();
    Z::setModeForTest(Z::kStrict);
    resetDevice(0x901u);
    issueOne(eng, true);
    uint8_t op = 0;
    uint32_t a = 0, c = 0;
    popMarker(op, a);
    issueOne(eng, false);
    popMarker(op, c);
    wr(kDev + 0x2A1Cu, 0x903u);
    execFenceStore(eng, 0x901u);
    st = Z::stats();
    check(a && a == c && rd(kFbVa) == 0x901u && getDataWrapper() == 1u && st.fallback == 1 && st.flushes == 0,
          "Z13: strict with no backend attached: the fallback 1 before the store");
    check(st.ended == st.resolved + st.lost + st.inflight, "Z8: ended == resolved + lost + inflight");
  }

  // ── Z16: the GPU (WARP): the HUD query's point draw, its depth test and its probes on a real device ──
  runZpdWarp();

  // ── Z14: the player log profile. Every live roll runs SR_LOG=player, which drops an Info line whose tag is not in
  // app/log.cpp kMilestoneTags (zpd3, 2026-10-15: no [NATIVE_ZPD] line reached any roll's run.log, five FAILs). The
  // production format strings, logged through the real Log in the player profile into a file. ────────────────────
  {
    namespace fs = std::filesystem;
    const fs::path logPath = fs::temp_directory_path() / ("sr_native_zpd_player_" + std::to_string(_getpid()) + ".log");
    _putenv_s("SR_LOG", "player");
    sr::Log::init(logPath.string().c_str());
    const bool player = sr::Log::playerProfile();
    Z::resetForTest();
    Z::setModeForTest(Z::kStrict);
    resetDevice(0xA01u);
    uint8_t* h14 = slotHost();
    // a written query ([NATIVE_ZPD_Q] #1 ... via=present), a strict fallback ([NATIVE_ZPD_Q] #2 ... FALLBACK), the
    // census line
    const Z::Event b14 = Z::onEvent(kSlotPhys + 0x20u, h14);
    const uint32_t seg14 = Z::segmentOpen(b14.id, 1);
    if (seg14 != Z::kNoSeg) g_readback[seg14] = 9;
    Z::segmentClosed(b14.id, seg14);
    const Z::Event e14 = Z::onEvent(kSlotPhys, h14);
    Z::endRecorded(e14.id, true);
    Z::stamp(0);
    Z::collect(UINT64_MAX, g_readback, kArea);
    Z::onEvent(kSlotPhys + 0x20u, h14);
    const Z::Event e15 = Z::onEvent(kSlotPhys, h14);
    Z::markerDrained(Z::kEnd, e15.id);
    Z::fenceFallback();
    Z::logLine("player");
    sr::Log::shutdown();
    _putenv_s("SR_LOG", "");
    std::string text;
    {
      std::ifstream in(logPath, std::ios::binary);
      std::ostringstream ss;
      ss << in.rdbuf();
      text = ss.str();
    }
    std::error_code ec;
    fs::remove(logPath, ec);
    const bool profileLine = text.find("[LOG_PROFILE] player") != std::string::npos;
    const bool census = text.find("[NATIVE_ZPD] player mode=strict issued=2 ") != std::string::npos &&
                        text.find(" probes=0 depthKilled=0 psKilled=0 noRaster=0 lagAvgMs=") != std::string::npos &&
                        text.find(" lagMaxMs=") != std::string::npos;
    const bool q1 = text.find("[NATIVE_ZPD_Q] #1 id=") != std::string::npos && text.find(" lagMs=") != std::string::npos;
    const bool q2 = text.find("[NATIVE_ZPD_Q] #2 id=") != std::string::npos && text.find("FALLBACK") != std::string::npos;
    std::printf("      player-profile log: %zu bytes (profile line %d, census %d, query lines %d / %d)\n", text.size(),
                profileLine ? 1 : 0, census ? 1 : 0, q1 ? 1 : 0, q2 ? 1 : 0);
    check(player && profileLine && census,
          "Z14: SR_LOG=player (every live roll) keeps the [NATIVE_ZPD] census line (a kMilestoneTags tag)");
    check(player && profileLine && q1 && q2,
          "Z14: SR_LOG=player keeps the [NATIVE_ZPD_Q] query lines (a written query, a strict fallback)");
  }

  // ── Z17 (zpd7, the Xenia oracle): the traced frames' queries. tools/perf/zpd_xenia.py pairs each [NATIVE_ZPD_T] line
  // (swap + slot + order) with Xenia's own report for the same END in that frame's .xtr, so every traced query must
  // log - past the 64 [NATIVE_ZPD_Q] lines - and an untraced one must not. ─────────────────────────────────────────
  {
    namespace fs = std::filesystem;
    const fs::path logPath = fs::temp_directory_path() / ("sr_native_zpd_trace_" + std::to_string(_getpid()) + ".log");
    _putenv_s("SR_LOG", "player");
    sr::Log::init(logPath.string().c_str());
    Z::resetForTest();
    Z::setModeForTest(Z::kStrict);
    resetDevice(0xA02u);
    uint8_t* h17 = slotHost();
    constexpr uint32_t kSwap = 4242u;
    auto written = [&](uint32_t trace, uint64_t count) {
      const Z::Event b = Z::onEvent(kSlotPhys + 0x20u, h17, trace);
      const uint32_t seg = Z::segmentOpen(b.id, 1);
      if (seg != Z::kNoSeg) g_readback[seg] = count;
      Z::segmentClosed(b.id, seg);
      const Z::Event e = Z::onEvent(kSlotPhys, h17, trace);
      Z::endRecorded(e.id, true);
      Z::stamp(0);
      Z::collect(UINT64_MAX, g_readback, kArea);
      return e.id;
    };
    // 70 untraced queries first: the [NATIVE_ZPD_Q] lines (64) are spent before the traced ones
    for (int i = 0; i < 70; i++) written(0, 27);
    const uint32_t idW = written(kSwap, 27);   // 27 samples / area 9 -> the guest reads 3
    const uint32_t idZ = written(kSwap, 0);    // occluded: 0
    Z::onEvent(kSlotPhys + 0x20u, h17, kSwap);  // strict's stand-in: the slot's last count (0)... or 1
    const Z::Event eF = Z::onEvent(kSlotPhys, h17, kSwap);
    Z::markerDrained(Z::kEnd, eF.id);
    Z::fenceFallback();
    const uint32_t idU = written(0, 27);       // untraced again: no line
    sr::Log::shutdown();
    _putenv_s("SR_LOG", "");
    std::string text;
    {
      std::ifstream in(logPath, std::ios::binary);
      std::ostringstream ss;
      ss << in.rdbuf();
      text = ss.str();
    }
    std::error_code ec;
    fs::remove(logPath, ec);
    size_t tLines = 0;
    for (size_t p = text.find("[NATIVE_ZPD_T] "); p != std::string::npos; p = text.find("[NATIVE_ZPD_T] ", p + 1))
      tLines++;
    char want[3][128];
    std::snprintf(want[0], sizeof want[0], "[NATIVE_ZPD_T] swap=%u id=%u slot=%08X written guest=3 host=27", kSwap, idW,
                  kSlotPhys);
    std::snprintf(want[1], sizeof want[1], "[NATIVE_ZPD_T] swap=%u id=%u slot=%08X written guest=0 host=0", kSwap, idZ,
                  kSlotPhys);
    std::snprintf(want[2], sizeof want[2], "[NATIVE_ZPD_T] swap=%u id=%u slot=%08X fallback guest=", kSwap, eF.id,
                  kSlotPhys);
    const bool w0 = text.find(want[0]) != std::string::npos, w1 = text.find(want[1]) != std::string::npos,
               w2 = text.find(want[2]) != std::string::npos;
    char untraced[64];
    std::snprintf(untraced, sizeof untraced, "id=%u slot=", idU);
    const bool noU = text.find(std::string("[NATIVE_ZPD_T] swap=0 ")) == std::string::npos &&
                     text.find(untraced) == std::string::npos;
    std::printf("      traced-frame lines: %zu (written %d, occluded %d, fallback %d, untraced absent %d)\n", tLines,
                w0 ? 1 : 0, w1 ? 1 : 0, w2 ? 1 : 0, noU ? 1 : 0);
    check(w0 && w1 && tLines == 3,
          "Z17: a traced query logs [NATIVE_ZPD_T] swap= id= slot= written guest= host= (past the 64 query lines), "
          "the occluded one guest=0");
    check(w2, "Z17: strict's stand-in for a traced query logs fallback guest=");
    check(noU, "Z17: an untraced query logs no [NATIVE_ZPD_T] line");
  }

  // ── Z19 (zpdx2 -> zpdx3, the Xenia oracle's traces): the live trace writer. zpdx2's four traces crashed Xenia's
  // dump: the CP executes every primary-ring packet from a host copy, recorded at physical 0, and IM_LOAD_IMMEDIATE's
  // inline ucode was recorded as a read of physical 0 that overwrote them. And four single frames held 4 queries: a
  // trace now holds SR_PM4_TRACE_FRAMES consecutive frames. Driven here through the writer's own taps (the CP's), the
  // engine for the IM_LOAD_IMMEDIATE, then the file read back as Xenia's player reads it. ──────────────────────────
  {
    namespace fs = std::filesystem;
    namespace LT = sr::gfx::livetrace;
    const fs::path dir = fs::temp_directory_path() / ("sr_native_zpd_xtr_" + std::to_string(_getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    // the guest memory the writer places packets by (the game registers it with its thread runner)
    K::registerExCreateThreadRunner(nullptr, K::getGuestBase(), K::getGuestSize());
    LT::setInitProvider(nullptr);  // no initial GPU state: the seam's backend has no device to snapshot
    LT::configureForTest(true, 3, dir.string().c_str());
    constexpr uint32_t kPk = 0x00F30000u, kRd = 0x00F40000u, kRd2 = 0x00F50000u, kIm = 0x00F60000u;
    uint8_t* gpk = K::guestVAToHost(0xA0000000u + kPk);
    uint8_t* grd = K::guestVAToHost(0xA0000000u + kRd);
    uint8_t* grd2 = K::guestVAToHost(0xA0000000u + kRd2);
    uint8_t* gim = K::guestVAToHost(0xA0000000u + kIm);
    check(LT::hostPhys(gpk) == kPk && LT::hostPhys(nullptr) == 0u,
          "Z19: (setup) a guest pointer's physical address, none for other memory");
    auto be = [](uint8_t* p, uint32_t v) {
      p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
    };
    be(gpk, 0xC0001000u);  // a guest NOP {header, 0}
    be(gpk + 4, 0u);
    // IM_LOAD_IMMEDIATE {header (5 dwords of body), shader type 0, start 0 | size 3, 3 ucode dwords}
    const uint32_t im[6] = {0xC0042B00u, 0u, 3u, 0x11111111u, 0x22222222u, 0x33333333u};
    for (uint32_t i = 0; i < 6; i++) be(gim + 4u * i, im[i]);
    std::vector<uint8_t> swapPk(8), hostPk(8);  // host copies (the ring's): XE_SWAP {header, 0}, a type-0 write
    be(swapPk.data(), 0xC0006400u);
    be(hostPk.data(), 0x00002000u);
    be(hostPk.data() + 4, 0x12345678u);
    auto swap = [&] {
      LT::packetBegin(swapPk.data(), 2);
      LT::packetEnd(0, true);
    };
    auto hostPacket = [&](bool withRead) {
      LT::packetBegin(hostPk.data(), 2);
      if (withRead) LT::memRead(kRd2, grd2, 32);
      LT::packetEnd(0, false);
    };
    std::memset(grd, 0x11, 64);
    std::memset(grd2, 0x5A, 32);
    swap();                // swap 1: armed, nothing requested
    LT::requestCapture();  // the hotkey (requestOnQuery's request)
    swap();                // swap 2: the trace opens, its first frame is 3
    const uint32_t t0 = LT::tracedSwap();
    LT::memRead(kRd, grd, 64);  // frame 3: a loose read (no packet open)
    hostPacket(true);
    LT::packetBegin(gpk, 2);
    LT::packetEnd(0, false);
    eng.execute(gim, sizeof(im));  // the engine records the packet itself; its ucode is in it
    swap();                        // swap 3: the next frame goes into the same trace
    const uint32_t t1 = LT::tracedSwap();
    std::memset(grd, 0x22, 64);    // the CPU rewrote the page for frame 4
    LT::memRead(kRd, grd, 64);
    hostPacket(false);
    swap();  // swap 4
    LT::packetBegin(gpk, 2);
    LT::packetEnd(0, false);
    swap();  // swap 5: the third frame ends the trace
    const uint32_t t2 = LT::tracedSwap();
    LT::configureForTest(false, 1, "");
    check(t0 == 3u && t1 == 3u && t2 == 0u,
          "Z19: tracedSwap is the trace's first frame for every frame of it (3, 3), 0 once it is written");

    // the file, read as Xenia's TracePlayer reads it
    struct Rec {
      uint32_t kind, base, n;
      uint32_t w0;  // a packet's header (big-endian in the record) / a read's first byte
    };
    std::vector<Rec> recs;
    const fs::path xtr = dir / "live_454107ED_3.xtr";
    std::ifstream in(xtr, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto u32 = [&](size_t o) {
      uint32_t v = 0;
      if (o + 4 <= b.size()) std::memcpy(&v, b.data() + o, 4);
      return v;
    };
    bool parsed = b.size() > 48;
    for (size_t o = 48; parsed && o < b.size();) {
      const uint32_t k = u32(o);
      Rec r{k, 0, 0, 0};
      size_t len = 0;
      if (k == 4) {  // PacketStart {base, count} + count dwords
        r.base = u32(o + 4), r.n = u32(o + 8), len = 12 + size_t(r.n) * 4;
        r.w0 = uint32_t(b[o + 12]) << 24 | uint32_t(b[o + 13]) << 16 | uint32_t(b[o + 14]) << 8 | b[o + 15];
      } else if (k == 5) {
        len = 4;
      } else if (k == 6) {  // MemoryRead {base, format, encoded, decoded} + bytes
        r.base = u32(o + 4), r.n = u32(o + 16), len = 20 + size_t(u32(o + 12));
        r.w0 = r.n ? b[o + 20] : 0u;
      } else if (k == 8) {
        len = 12 + size_t(u32(o + 8));
      } else if (k == 9) {
        len = 8;
      } else if (k == 10) {
        len = 24 + size_t(u32(o + 20));
      } else if (k == 11) {
        len = 16 + size_t(u32(o + 12));
      } else {
        parsed = false;
        break;
      }
      recs.push_back(r);
      o += len;
      if (o > b.size()) parsed = false;
    }
    check(parsed && !recs.empty(), "Z19: the trace (live_454107ED_3.xtr, the first frame's name) parses record by record");
    auto swapEnds = [&] {  // the swap events, each right after a swap packet's PacketEnd
      std::vector<size_t> at;
      for (size_t i = 0; i < recs.size(); i++)
        if (recs[i].kind == 9) at.push_back(i);
      return at;
    }();
    bool eventsPlaced = swapEnds.size() == 3u && swapEnds.back() == recs.size() - 1u;
    for (size_t i = 0; eventsPlaced && i < swapEnds.size(); i++) {
      const size_t e = swapEnds[i];
      eventsPlaced = e >= 2 && recs[e - 1].kind == 5 && recs[e - 2].kind == 4 && recs[e - 2].w0 == 0xC0006400u;
    }
    check(eventsPlaced, "Z19: three frames in one trace: a swap event after each frame's swap packet (Xenia's writer "
                        "puts it there), the last ends the file");
    uint32_t hostAt = 0, guestAt = 0, imAt = 0, zeroBased = 0, inScratch = 0, starts = 0;
    for (const Rec& r : recs) {
      if (r.kind == 4) {
        starts++;
        if (r.w0 == 0xC0006400u || r.w0 == 0x00002000u) hostAt += r.base == LT::kHostPacketPhys;
        if (r.w0 == 0xC0001000u) guestAt += r.base == kPk;
        if (r.w0 == 0xC0042B00u) imAt += r.base == kIm;
      }
      if ((r.kind == 4 || r.kind == 6) && r.base == 0u) zeroBased++;
      if (r.kind == 6 && r.base + r.n > LT::kHostPacketPhys) inScratch++;
    }
    check(hostAt == 5u && guestAt == 2u && imAt == 1u,
          "Z19: packets executed from host copies (3 swaps, 2 ring writes) are recorded at kHostPacketPhys, guest "
          "packets at their own physical address");
    check(zeroBased == 0u && inScratch == 0u,
          "Z19: nothing is recorded at physical 0 (IM_LOAD_IMMEDIATE's inline ucode is no read) and no read reaches "
          "the host packets' address");
    // each frame's loose read first in that frame: frame 3's 0x11 page before its first packet, frame 4's 0x22 page
    // after the first swap event and before frame 4's first packet - never at the start of the file
    size_t first11 = SIZE_MAX, first22 = SIZE_MAX, firstPk = SIZE_MAX, firstPkF4 = SIZE_MAX, rd2 = SIZE_MAX;
    for (size_t i = 0; i < recs.size(); i++) {
      const Rec& r = recs[i];
      if (r.kind == 6 && r.base == kRd && r.w0 == 0x11u && first11 == SIZE_MAX) first11 = i;
      if (r.kind == 6 && r.base == kRd && r.w0 == 0x22u && first22 == SIZE_MAX) first22 = i;
      if (r.kind == 6 && r.base == kRd2 && rd2 == SIZE_MAX) rd2 = i;
      if (r.kind == 4 && firstPk == SIZE_MAX) firstPk = i;
      if (r.kind == 4 && !swapEnds.empty() && i > swapEnds[0] && firstPkF4 == SIZE_MAX) firstPkF4 = i;
    }
    check(first11 < firstPk && !swapEnds.empty() && first22 > swapEnds[0] && first22 < firstPkF4,
          "Z19: each frame's loose reads come first in that frame (frame 4's rewritten page after frame 3's swap "
          "event, not before frame 3's packets)");
    check(rd2 != SIZE_MAX && rd2 > firstPk && recs[rd2 - 1].kind == 4 && recs[rd2 - 1].w0 == 0x00002000u,
          "Z19: a read made while a packet executed stays inside that packet (after its PacketStart)");
    check(starts == 8u, "Z19: every packet recorded once (8 PacketStarts)");
    fs::remove_all(dir, ec);
  }

  // ── Z20-Z23: p2.zpd's bake ────────────────────────────────────────────────────────────────────────────────────────
  z20DecisionTable();
  z21FreshLatch();
  z22RecordApply();
  z23PortFollowsMode();
  z24FreezeEdges();
  z25FrameStamp();
  z26FrameStampHandoff();
  Z::setModeForTest(Z::kOff);
  std::printf("[NATIVE_ZPD_TEST] %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
