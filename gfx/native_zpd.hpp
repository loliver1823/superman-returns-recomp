#pragma once
// p2.zpd (docs/NATIVE_ROADMAP.md P2.12): the game's occlusion queries (EVENT_WRITE_ZPD, 0x5B) answered by host D3D12
// occlusion queries. SR_NATIVE_ZPD, BAKED default ON in the latency model since native/zpd-bake (2026-10-16; NATIVE_ROADMAP
// "p2.zpd BAKE PLAN"): unset = the latency model (the default form), 0 = off (the guest path, the A/B arm), 1 = strict,
// 2 = the latency model (the explicit forms). The producer side of the bake - GpuEmitTileSurfaceConfig_82101E08's packets
// as records, EVENT_WRITE_ZPD as kRkZpd (RecordEngine::applyZpdEvent) - lives in kernel/native_flush (zpdPortOn).
//
// The guest (D3DQueryIssueBeginEnd_82102BC8 / D3DQueryGetData_82102DC8): a 64-byte slot per tile, the END record at
// +0x00, the BEGIN record at +0x20, each {Total_A, Total_B, ZFail_A, ZFail_B, ZPass_A, ZPass_B, StencilFail_A,
// StencilFail_B} little-endian (xenia xenos.h:1570). Issue(BEGIN) presets the END record's first four dwords to
// 0xFFFFFEED; GetData answers S_FALSE until the query's fence (the open submission at Issue(END)) has passed AND one
// of those four dwords changed, then END.ZPass - BEGIN.ZPass.
//
// This module is the logical side, shared by every CP mode and by the difftest:
//   - the CP, in parse order, reports each EVENT_WRITE_ZPD (onEvent) and pushes the returned marker into the draw
//     queue; the drain reports the marker at its stream position (markerDrained);
//   - the render loop brackets every GUEST draw between a BEGIN and an END marker with its own host query segment
//     (segmentOpen right before the draw call, segmentClosed right after it: helper passes, transfers, resolves and
//     clears never run inside one, and no segment spans a command-list close), reports the END marker
//     (endRecorded), stamps the closed segments with the fence of the submission that carries them (stamp) and hands
//     the readback over once that fence completed (collect), which writes the slot words;
//   - before an EVENT_WRITE_SHD store, the drain owner asks fenceNeedsFlush(): in strict mode it then renders the
//     set drained so far and waits for the GPU (the backend's partial frame) so the words are written before the
//     fence, and whatever that could not write gets Xenia's strict fallback (fenceFallback: the slot's last count,
//     else 1) before the store - a strict fence never publishes over an unwritten slot; the latency model marks
//     them covered instead (fenceCovered: written later = late).
// One mutex; the queries are rare (a HUD threat marker: one query per marker per two frames).
#include <cstdint>

namespace sr::gfx::zpd {

enum Mode : int { kOff = 0, kStrict = 1, kLatency = 2 };
// What an SR_NATIVE_ZPD value requests (pure; native_zpd_test's decision table): unset or "" = the BAKED DEFAULT, the
// latency model (explicitForm false); a value starting '0' = off; '2' or "latency" = the latency model; "1", "strict" or
// any other value = strict (those explicit). kernel/native_flush's zpdPortOn reads the same variable as
// `!e || e[0] != '0'`: the port is on exactly when the mode is not off.
struct Request {
  Mode mode;
  bool explicitForm;
};
Request requestOf(const char* value);
// SR_NATIVE_ZPD's mode, latched at the first call (guest-visible answers: never an in-roll A/B feature; the bake's
// per-present A/B toggles the producer port only, kernel/native_flush zpdPortOn). The latch logs the form once:
//   [NATIVE_ZPD] SR_NATIVE_ZPD on (the baked default; latency)   unset
//   [NATIVE_ZPD] SR_NATIVE_ZPD on (=<value>; <mode>)               an explicit form
//   [NATIVE_ZPD] SR_NATIVE_ZPD off (=<value>): the guest path      0
Mode mode();
bool explicitForm();  // the latched request's form (false: the baked default, or a test override)
inline bool on() { return mode() != kOff; }
const char* modeName(Mode m);
void setModeForTest(Mode m);  // difftest only (overrides the latch)

enum Op : uint8_t { kNone = 0, kBegin = 1, kEnd = 2 };
struct Event {
  Op op = kNone;
  uint32_t id = 0;
};

constexpr uint32_t kSlotBytes = 0x40;
constexpr uint32_t kRecordBytes = 0x20;
constexpr uint32_t kSentinel = 0xFFFFFEEDu;  // the guest's "not written" (stored big-endian: FF FF FE ED)
constexpr uint32_t kHeapCapacity = 4096;     // D3D12 occlusion queries (segments) in the heap
constexpr uint32_t kNoSeg = 0xFFFFFFFFu;
constexpr uint32_t kMaxSegs = 32;            // segments (guest draws) per logical query; more answer "visible"
// The largest count published: END+0 holds the count little-endian and GetData's big-endian load of it must never
// read 0xFFFFFEED (the count 0xEDFEFFFF would), so counts saturate one below.
constexpr uint32_t kMaxCount = 0xEDFEFFFEu;

// CP, parse order: an EVENT_WRITE_ZPD with RB_SAMPLE_COUNT_ADDR = addr (physical, endian bits ignored).
// slotHost = the host bytes of the 64-byte slot (addr & ~0x3F), or null. Returns the marker to queue.
// trace (zpd7, the Xenia oracle): the swap number of the live PM4 trace the CP is recording (gfx/pm4_live_trace.hpp
// tracedSwap), 0 outside one. A query whose BEGIN came inside a traced frame logs its outcome in full ([NATIVE_ZPD_T],
// uncapped by the 64 [NATIVE_ZPD_Q] lines): tools/perf/zpd_xenia.py pairs it with Xenia's report for the same END.
Event onEvent(uint32_t addr, uint8_t* slotHost, uint32_t trace = 0);
// The marker could not be queued: the query is dropped.
void markerDropped(const Event& e);
// The drain owner, stream order: the marker reached the batch list.
void markerDrained(Op op, uint32_t id);
// Before an EVENT_WRITE_SHD store (drain owner): a drained END is not written yet.
bool fenceNeedsFlush();
// Strict, after the partial frame (or when there is none): every drained END still unwritten gets Xenia's strict
// fallback (command_processor.cc PumpPendingRetire: the slot's last count, else 1) written now, before the store;
// its real count is dropped when it arrives (superseded).
void fenceFallback();
// The latency model: every drained END still unwritten is covered by this fence (written later = late).
void fenceCovered();
void noteFlush(uint64_t waitUs, bool ok);

// Render side (the thread recording the command list).
// A guest draw inside query `id`, its target holding samplesPerPixel samples per guest pixel (RB_SURFACE_INFO MSAA:
// 1 / 2 / 4): the heap index of its segment, kNoSeg = none (the query is dead, or over kMaxSegs / the pool: then it
// answers "visible").
uint32_t segmentOpen(uint32_t id, uint32_t samplesPerPixel);
// The visibility probe (SR_NATIVE_ZPD_PROBE, default on with the gate; zpd4: every query of every roll read 0): the
// same guest draw drawn again right after its measured draw, on segments of their own whose counts are NOT the
// answer (only the probe counters and the [NATIVE_ZPD_Q] line's ref= / raster= see them), with nothing written:
//   kRef    - depth test ALWAYS, stencil off, the draw's own pixel shader (its discards / alpha test still apply);
//   kRaster - the same without a pixel shader (what the rasterizer covers, nothing else).
// Per probed query whose answer is 0: ref > 0 = the depth / stencil test killed every sample (depthKilled); ref 0
// but raster > 0 = the pixel shader discarded them (psKilled); raster 0 (ref 0 without a raster probe) = the draw
// covered no sample at all - off screen, clipped, a zero-size point, scissored (noRaster).
enum Probe : uint8_t { kAnswer = 0, kRef = 1, kRaster = 2 };
uint32_t segmentOpenProbe(uint32_t id, Probe kind);
bool probeOn();  // SR_NATIVE_ZPD_PROBE (unset / 1 = on, 0 = off); latched
void segmentMissed(uint32_t id);                  // a guest draw no host segment covers (no heap): "visible"
void segmentClosed(uint32_t id, uint32_t seg);    // EndQuery + ResolveQueryData recorded right after the draw
void endRecorded(uint32_t id, bool begun);        // its END marker passed (begun = its BEGIN did too, else "visible")
void stamp(uint64_t fence);  // closed segments not stamped yet complete at `fence` (0 = already complete)
// The completed queries: guest samples = normalize(sum of readback[seg] * samplesPerPixel(seg), area) with area =
// scale.x * scale.y; the slot written. partial: the strict partial frame (or an idle flush) collects, before the
// covering fence's store; else the present's collect, after its frame's GPU fence (viaPartial / viaPresent).
void collect(uint64_t completedFence, const uint64_t* readback, uint32_t area, bool partial = false);

// The guest write (also the difftest's): BEGIN record {0, S, S, S, 0, 0, 0, 0}, END record {n, S, S, S, n, 0, 0, 0}
// (S = 0xFFFFFEED as the guest stores it), END+0 last after a release fence. END+0 is GetData's only "written"
// signal (END+4/+8/+C stay S, so they never are) and every other word is final before it: a reader that sees the
// new END+0 reads a consistent record, whatever it loads in between; the S words cancel between END and BEGIN, so
// GetData's 16-byte result is {n, 0, 0, n}. step (difftest): called after every 32-bit store with its index.
void writeSlot(uint8_t* slotHost, uint32_t n, void (*step)(void* user, uint32_t store) = nullptr,
               void* user = nullptr);
constexpr uint32_t kSlotStores = 16;  // writeSlot's 32-bit stores
// Guest samples from host samples already multiplied by the guest's samples per pixel: 0 stays 0, else
// max(1, round(samples / area)), at most kMaxCount (P2.12 "Resolution scale and MSAA").
uint32_t normalize(uint64_t samples, uint32_t area);

struct Stats {
  uint64_t issued = 0, ended = 0, drained = 0, resolved = 0, written = 0, late = 0, stale = 0, orphan = 0,
           abandoned = 0, evicted = 0, dropped = 0, lost = 0, badAddr = 0, poolFull = 0, flushes = 0,
           flushFail = 0, waitUs = 0, maxWaitUs = 0, segs = 0, zero = 0, nonzero = 0, lastHost = 0, covered = 0,
           fallback = 0, superseded = 0, missed = 0, overSegs = 0;
  // Who wrote the slot: the strict partial frame / an idle flush (before the covering fence's store) or a present's
  // collect (after its frame's GPU fence: under strict, a covering fence no EVENT_WRITE_SHD hook saw).
  uint64_t viaPartial = 0, viaPresent = 0;
  // The probe (segmentOpenProbe): queries probed, and of those answering 0 why (depthKilled / psKilled / noRaster,
  // see segmentOpenProbe); probes - the three = the probed queries that answered > 0.
  uint64_t probes = 0, depthKilled = 0, psKilled = 0, noRaster = 0;
  // Answer lag: the wall time from the CP parsing the END to the slot write (strict: within the covering fence's
  // partial frame; latency: after the frame's GPU fence).
  uint64_t lagSumUs = 0, lagMaxUs = 0, lagN = 0;
  uint64_t segsBySpp[3] = {};  // segments whose target held 1 / 2 / 4 samples per pixel
  // The bake's producer port (kernel/native_flush zpdSurfaceLeaf: 82101E08 as records, notePort): calls it took and
  // calls it declined to the guest body (the Entry declined, or the per-present A/B's guest arm).
  uint64_t port = 0, portDecl = 0;
  uint32_t lastGuest = 0, live = 0;
  uint32_t inflight = 0;  // ended queries not resolved yet (ended == resolved + lost + inflight while nothing evicts)
};
Stats stats();
void resetForTest();
// The producer port's outcome per 82101E08 call (kernel/native_flush zpdSurfaceLeaf): taken = records written.
void notePort(bool taken);
// [NATIVE_ZPD] <why> ... (cumulative counters; once per [GPU_CP] census; the port's port= / portDecl= last). Silent
// while nothing was issued and the mode is off.
void logLine(const char* why);

// THE FRAME STAMP (Astra round 2 on native/zpd-bake, P2-1: the freeze-edge census brackets a whole hold, not the
// presents the frozen pass compares; issued counts at the CP's parse, port at the producer). The EVENT_WRITE_ZPD events
// the CP parsed between two swap packets - one frame's, in stream order - are taken at that frame's swap
// (gfx/null_renderer cpPresentSwap, the CP thread) and travel with the frame to its present (the executor's deferred
// swap item included); the freeze dump line of that present prints them (kernel/highfps freezeDumpWanted:
// `[SIM_FREEZE_DUMP] ... zpd=<begins> rec=<records> pm4=<packets> fswap=<frame>`, `zpd=-` for a present without a
// stamp), so tools/perf/zpd_activity.py reads each COMPARED present's own queries and, in the per-present A/B, its arm
// (the port's frames carry kRkZpd records, the guest body's frames the literal packet). Diagnostic only: counted while
// the freeze dump runs (frameStampOn: the sim-freeze diagnostic with SR_PRESENT_DUMP_FREEZE); otherwise nothing is
// counted and every swap carries an empty stamp. No guest state.
struct FrameStamp {
  uint32_t begins = 0;  // query BEGINs onEvent opened in the frame (the native path's; =0 opens none)
  uint32_t rec = 0;     // EVENT_WRITE_ZPD events that came as kRkZpd records (82101E08 as records: the port)
  uint32_t pm4 = 0;     // ... as the literal 0x5B packet (the guest body: =0, the A/B's guest arm, CPPM4's rendering)
  uint32_t frame = 0;   // the frame's ordinal among the stamped swaps (1-based); 0 = no stamp
};
bool frameStampOn();                            // latched: kernel/highfps simFreezeOn() and SR_PRESENT_DUMP_FREEZE 1 / 2
void setFrameStampForTest(int on);              // difftest: -1 = the latch, 0 / 1 forced
void noteSource(bool record);                   // the CP, per EVENT_WRITE_ZPD (RecordEngine::applyZpdEvent): its form
FrameStamp takeFrameStamp();                    // the CP at the swap packet: the frame's counts (then zeroed); {} when off
void setPresentingStamp(const FrameStamp& s);   // thread-local; the backend carries it through its frame queue
FrameStamp presentingStamp();                   // the stamp of the frame this thread is presenting ({} = none)

}  // namespace sr::gfx::zpd
