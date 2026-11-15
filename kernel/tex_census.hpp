// iter-458 TEXTURE-PATH ALLOCATION/UPLOAD CENSUS (Astra 068, corrected per
// Astra 069). SR_TEX_OWN_CENSUS=<file>, default OFF, observe-only: nothing here
// changes what the guest or the renderer does.
//
// SCOPE (narrower than 068 asked, and named so): an ALLOCATION/UPLOAD census.
// Memory generations are grant serials; slot content versions are apply
// ordinals; descriptors are (base, fmtBits, dims). D3D9 texture-OBJECT
// generations (Create/Release) are NOT recorded.
//
// Question: when a draw binds a texture, (ownership axis) was its physical span
// ALIASED - two live grants overlapping EACH OTHER inside the span - at the
// upload or at the draw's publish; (content axis) does the slot band it binds
// hold a version whose uploaded payload differs from the guest bytes the
// producer last observed, before the draw's publish, in the SAME alias the
// upload was taken from (the one-slot-per-base / suppressed-upload defect)?
//
// One totally ordered event stream (seq stamped and appended under one mutex:
// file order == seq order). Every event that observes something carries the
// render-FIFO clock: P = commands PUBLISHED when read. Every published command
// executes exactly once, in publish order, so a render-side event's z is its
// command's FIFO index E. An observation bracketed by [P0, P1] happened before
// draw E's publish iff P1 < E, after it iff P0 >= E, and is AMBIGUOUS (UNKNOWN)
// iff P0 < E <= P1. Astra 070: P1 is read INSIDE the event mutex immediately
// before the append, so [P0, P1] covers the observation AND its append: an
// observation with P1 < E was appended before draw E was published (and so
// before its binding); observations of one kind are serialized (observe +
// append is one step), so append order IS observation order.
//
//   GrantReg/GrantRel  every registry mutation, under the registry's unique
//                      lock, bracketed [P0,P1] around the map write.
//   TexPublish         every TextureUpload entering the FIFO (inside the queue
//                      lock): FIFO index, and a hash of the IMMUTABLE payload
//                      snapshot (the bytes the render thread will upload).
//   TexDecode          a producer bind: ISSUED (this bind published the upload;
//                      carries that upload's FIFO index, its payload hash and
//                      the alias window the snapshot came from; owners are
//                      STABLE iff no registry mutation happened between the
//                      start of the bind and the owner sample), SUPPRESSED_
//                      CHANGED (tuple already uploaded; the selected alias's
//                      bytes or the full owner set differ from the last emitted
//                      state), NO_BASELINE. Owners: full set digest, pairwise
//                      aliasing inside the span, two smallest serials.
//   TexApply           the executor arm's exit: result and slot BAND.
//   Resident           every slot-map write (one helper writes all three bands).
//   DrawTex            each bound sampler reaching the draw tap: key, fetch
//                      base/fmt/dims/dim, the resident (source, ordinal) of
//                      EACH band, the in-window bit. A binding, not a fetch.
//   Mark               1 = census cost; 2/3 = recorder window open/close edges
//                      with the executor's FIFO position and the renderer's
//                      INDEPENDENT window counters (conservation).
//
// Durability: written ONCE when the live recorder first closes, checked
// writes; no file = no acquisition = UNKNOWN.
#pragma once

#include <cstdint>

#include "sr_oracle.hpp"

namespace sr {
namespace kernel {
namespace txc {

enum EvType : uint32_t {
  kGrantReg = 1, kGrantRel = 2, kTexDecode = 3, kTexPublish = 4,
  kTexApply = 5, kResident = 6, kDrawTex = 7, kMark = 8, kTypes = 9
};
enum Decision : uint32_t { kIssued = 1, kSuppressedChanged = 2, kNoBaseline = 3 };
enum ResidentSource : uint32_t { kFromUpload = 1, kFromUnlock = 2, kFromResolve = 3 };
enum ApplyResult : uint32_t { kApplied = 1, kRefused = 2, kOther = 3 };
enum Band : uint32_t { kBand2D = 0, kBand3D = 1, kBandCube = 2, kBands = 3 };
enum MarkKind : uint32_t { kMarkCost = 1, kMarkWindowOpen = 2, kMarkWindowClose = 3 };
// TexDecode e-field flags (bits 28..31).
constexpr uint32_t kDecOwnersUnstable = 1u << 28;   // ISSUED: registry mutated during the bind
constexpr uint32_t kDecAliased = 1u << 29;          // two live owners overlap each other in-span (exact)
constexpr uint32_t kDecHashUnreadable = 1u << 30;   // the observed hash could not be read

// 80 bytes, little-endian, field meaning per type:
//   GrantReg    a kind  b va    c size   d serial                z P0 w P1
//   GrantRel    a kind  b va    d serial e how (1 released, 2 reclaimed) z P0 w P1
//   TexDecode   a base  b span  c fmtBits d dims
//               e decision | min(nLive,255)<<8 | kindBits<<16 | window<<24 | flags
//               f serial0 g serial1  x content hash (ISSUED: the payload's)
//               y upload FIFO index (ISSUED)  z P0  w P1  v owner-set digest
//   TexPublish  a key b arenaOffset c bytes d dims e fmtBits f pubOrdinal
//               x payload hash  z FIFO index
//   TexApply    a key b result c band d dims e fmtBits f applyOrdinal  z E
//   Resident    a key b source c band f ordinal (upload: its apply ordinal) z E
//   DrawTex     a drawOrdinal b sampler c key d fetchBase
//               e fetchFmt | fetchValid<<8 | fetchDim<<9 | inWindow<<11 |
//                 bandsPopulated<<12 | stacked(depth>1)<<15 | src2D<<16 |
//                 src3D<<20 | srcCube<<24
//               f fetchDims g 2D ordinal x psHash y 3D ordinal | cube ordinal<<32 z E
//   Mark 1      b decodes probed c observations still in flight when written
//               d ms waited for them e 1 = that wait timed out
//               x probe ns y max probe ns w decode-tap ns v draw-tap ns
//   Mark 2/3    z executor FIFO position at the edge; b|c<<32 the recorder's
//               record count at the edge (open: when recording goes LIVE, after
//               the preamble - the command with FIFO index E is record
//               b + E - z - 2); the renderer's window counters (reset at open,
//               so Mark 3 carries the window's): x draw commands executed,
//               y draws reaching the draw tap, w bound samplers seen at the
//               tap, v bindings skipped by diagnostic white-outs
struct Ev {
  uint32_t type, a, b, c, d, e, f, g;
  uint64_t seq, x, y, z, w, v;
};
static_assert(sizeof(Ev) == 80, "Ev is the 80-byte offline contract");

#if SR_ORACLE
bool enabled();
// Test seam: path (nullptr = off) and event cap; resets all state.
void setForTest(const char* path, uint32_t cap);

// Grants - phys_window calls these UNDER its registry's unique lock, after the
// map write; p0 = the FIFO clock read before the write.
void onGrantReg(uint32_t kind, uint32_t va, uint32_t size, uint32_t serial, uint64_t p0);
void onGrantRel(uint32_t kind, uint32_t va, uint32_t serial, uint32_t how, uint64_t p0);

// Producer. bindBegin() at the START of the capture block (before any
// snapshot): the observation's lower clock bound and the registry generation.
// The token is IN FLIGHT until it is destroyed (the end of the capture block);
// the flush waits for in-flight observations and records any it gave up on.
struct BindToken {
  uint64_t p0 = 0, gen0 = 0;
  bool on = false;
  BindToken() = default;
  BindToken(const BindToken&) = delete;
  BindToken& operator=(const BindToken&) = delete;
  BindToken(BindToken&& o) noexcept : p0(o.p0), gen0(o.gen0), on(o.on) { o.on = false; }
  ~BindToken();
};
BindToken bindBegin();
// window = the alias the snapshot came from (8 or 0xA) for an ISSUED bind, 0
// otherwise. An ISSUED bind's upload FIFO index is this thread's last publish.
void onTexDecode(const BindToken& tok, uint32_t base, uint32_t span, uint32_t fmtBits,
                 uint32_t dims, bool issued, uint32_t window);

// Render FIFO taps. payload = the immutable snapshot the command carries.
void onTexPublish(uint32_t key, uint32_t arenaOffset, uint32_t bytes, uint32_t dims,
                  uint32_t fmtBits, uint64_t fifoIdx, const uint8_t* payload);
void onTexApply(uint32_t key, uint32_t result, uint32_t band, uint32_t dims, uint32_t fmtBits,
                uint64_t execIdx);
void onResident(uint32_t key, uint32_t band, uint32_t source, uint64_t execIdx);
// A draw's sampler: records only when in scope (recorder open, or a flagged page).
void onDrawTex(uint32_t drawOrdinal, uint32_t sampler, uint32_t key, uint32_t fetchBase,
               uint32_t fetchFmt, bool fetchValid, uint32_t fetchDim, bool fetchStacked,
               uint32_t fetchDims, uint64_t psHash, uint64_t execIdx);
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): the census is compiled out (an unset SR_TEX_OWN_CENSUS: enabled() is false and
// every tap is a no-op). The test seams (setForTest, statsForTest, ...) exist only in the oracle build.
inline bool enabled() { return false; }
inline void onGrantReg(uint32_t, uint32_t, uint32_t, uint32_t, uint64_t) {}
inline void onGrantRel(uint32_t, uint32_t, uint32_t, uint32_t, uint64_t) {}
struct BindToken {
  uint64_t p0 = 0, gen0 = 0;
  bool on = false;
  BindToken() = default;
  BindToken(const BindToken&) = delete;
  BindToken& operator=(const BindToken&) = delete;
  BindToken(BindToken&& o) noexcept : p0(o.p0), gen0(o.gen0), on(o.on) { o.on = false; }
  ~BindToken() = default;
};
inline BindToken bindBegin() { return BindToken(); }
inline void onTexDecode(const BindToken&, uint32_t, uint32_t, uint32_t, uint32_t, bool, uint32_t) {}
inline void onTexPublish(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t, const uint8_t*) {}
inline void onTexApply(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t) {}
inline void onResident(uint32_t, uint32_t, uint32_t, uint64_t) {}
inline void onDrawTex(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, bool, uint32_t, bool, uint32_t, uint64_t,
                      uint64_t) {}
#endif
// Recorder window edges (hle_video, at the published open/close).
struct WindowCounters {
  uint64_t drawCmds = 0, drawsAtTap = 0, bindingsSeen = 0, whiteoutSkips = 0;
};
#if SR_ORACLE
void onWindowEdge(bool open, uint64_t execIdx, const WindowCounters& counters,
                  uint64_t recordIndex);

// The hash both sides use: FNV-1a over three 64-byte probes (start, middle,
// end) of a contiguous host range; nonzero.
uint64_t probeHash(const uint8_t* p, uint32_t span);

// Written once; returns true when the file was written completely.
bool flush(const char* why);

// Test seam: runs inside a decode, under the registry's shared lock, before the
// event is appended (difftest proves a racing registration is ordered after).
void setDecodeHookForTest(void (*fn)());
#else
inline void onWindowEdge(bool, uint64_t, const WindowCounters&, uint64_t) {}
inline bool flush(const char*) { return false; }
#endif

struct Stats {
  uint64_t events = 0, overflow = 0, byType[kTypes] = {};
  uint64_t lastSeq = 0;
  bool flushed = false;
};
#if SR_ORACLE
Stats statsForTest();
// The recorded events so far (test inspection).
uint64_t eventsForTest(Ev* out, uint64_t max);
#endif

}  // namespace txc
}  // namespace kernel
}  // namespace sr
