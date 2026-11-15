#pragma once
// mem.resources step 2a (docs/RELEASE_COMPOSITION.md section 9.5 item 1): THE REGISTRY IN SHADOW BESIDE THE PAGE MODEL.
//
// SR_MEM_RESOURCES=census (dev build; absent / any other value = off, today's path): a resource registry
// (gfx/resource_registry.hpp) receives the guest's own boundaries -
//   noteCoher     every coherency request the CP applies (RecordEngine::writeReg's COHER_BASE_HOST: the kRkInvalidate
//                 records of SR_NATIVE_INVAL_RANGE / SR_NATIVE_LOCK_COHER / the close's flush, kRkCoherHost's texture
//                 lock, and any COHER block that still arrives as PM4), classified by COHER_STATUS_HOST (memres1, 2026-
//                 10-16: the guest issues exactly four, and the first census's "anything else -> host" turned the
//                 Resolve's request into millions of CPU notes - every clobber of memres1): 0x03000000 -> Src::kInval
//                 (a CPU-write range: the VB lock, InvalidateGpuCache, the deferred range); 0x02000000 -> Src::kTexLock
//                 (the texture lock); 0x01000000 -> Src::kCmdBuf over the request's range - the close's vertex-cache
//                 invalidate of the WHOLE command buffer, the console's only boundary for the inline vertices
//                 BeginVertices / DrawVerticesUP carve from it (no request per allocation; the bit dev+0x2A3A & 4 is set
//                 at the buffer's first wrap and stays set: every unpinned close carries it); 0x03000100 -> the Resolve
//                 8210C5F8's request after its copy (0x8210D2F4: the GPU just wrote the destination) - counted
//                 (note.resolve), never a CPU note; any other status -> Src::kOther (noted, the conservative side;
//                 counted and sampled - none expected). census_coher counts every status by value;
//   noteReadFile  NtReadFile into guest memory (kernel/stubs.cpp, the synchronous read and the async read worker),
//                 after the bytes landed; virtual-heap destinations (below 0x80000000) are not GPU-addressable here and
//                 only counted;
//   noteLock /    the texture Lock / Unlock hooks (sub_820FFCB0 / sub_820FFCC8): the texture's extent from its own
//   noteUnlock    fetch constant by the real layout (gfx/cube_layout.hpp computeGuestLayout's per-level strides, bounded
//                 from above by the sum of the per-level slice strides): the Lock opens a guest write window over it,
//                 the Unlock notes the CPU write (Src::kUnlock) and closes the window; a movie plane's Unlock also
//                 marks its range as the decoder's (a KNOWN boundary-less writer, 9.2 item 5) under the allocators'
//                 owner version - the mark expires when the allocation over the page changes or another texture is
//                 unlocked over it;
//   noteGpuWrite  a GPU write into guest physical memory (pm4ShmRangeWrittenByGpu: the native resolve, the EDRAM path)
// - and decides IN SHADOW at every decision event of the page model, page by page, on the generations it read BEFORE
// the model's bytes were hashed (snapPage; Registry::ensureAt commits that generation - the bytes copied):
//   model up   + registry up        agree_up        model keep + registry keep   agree_keep
//   model keep + registry up        over            (cost only), unless the arena holds the GPU's data (a GPU write
//                                                   since the last upload): CLOBBER - the upload would replace valid
//                                                   GPU data with RAM (a gate counter, never `over`)
//   model up   + registry keep      MISSED          (a CPU write the guest never announced - the `boundaries_complete`
//                                                   unknown; a gate counter), unless a note on the page landed between
//                                                   the read of the generations and the decision (race.note: the bytes
//                                                   may hold the noted write; the note stays pending) or the page lies
//                                                   inside an open guest Lock..Unlock (race.lock: a host read of a
//                                                   texture the guest is still writing - the Unlock's note follows and
//                                                   the console's draw comes after it)
// Every decision lands in exactly one cell (outcome x fetch kind x provenance x the last note's source x site x the
// model's reason x movie); the census line's totals are sums of cells, so a snapshot is consistent by construction.
// Beside the cells: registry first-use uploads (by kind / provenance); THE SOURCE WINDOW - a registry upload serves
// the allocation's provenance (0x8 when it names none: 9.2 drops the zero-page heuristic), the model the window it
// hashed; when the two differ the census compares the two windows' bytes (win.alias_same: harmless now; win.alias_diff:
// the registry would upload other bytes - a gate counter); clobber.both (both replace GPU data); reuse.noted (an epoch
// reuse across a note: 2b's ensure would upload there, the model does not).
// COVERAGE from the model's own events: every page of every request is disposed of exactly once - decided (the write
// watch's accept, the range cache's keep), a candidate (decided at pm4ShmValidate: decided or failed), reused (a stamp
// of this epoch the census saw: memo / epoch pre-check / lazy window / the walk's dedup), deferred (the prepass leaves
// a page this frame's resolves write to the bind) - per request and per validation (WalkScope; cov.unaccounted counts
// a page no hook disposed of), and every reuse is checked against the stamps the census recorded (cov.unseen: the model
// reused a decision the census never saw - an unhooked path). The checkers: each keep of the write watch and of the
// range cache is classified checked / stale / mismatch / unchecked from the model's own checks (SR_SHM_WT_CHECK=1,
// SR_NB_SHM_RANGE_CHECK=1).
// THE MISSED CLASS's attributes (memres2): every miss is also counted by kind x source x three attributes -
//   cb   the page inside the command buffer [cbLo, cbHi) (learned from the whole-buffer request and from noteUpAlloc:
//        dev+0x34AC / 0x34B0) - in / out / unk (not learned yet);
//   seg  the page's last inline-vertex allocation (noteUpAlloc, the native BeginVertices / DrawVerticesUP): up, zp (made
//        while a Z-pass list was recording - a pinned segment, whose close carries no coherency block), none;
//   rng  the fetched bytes (memres3: sound): every request's WalkScope ORs the 16-byte chunks of its byte range into the
//        page's fetched mask (all requests - decided, reused, deferred - so the mask is the union of every chunk any
//        request ever fetched); at every registry upload the census stores the page's 256 chunk hashes (the registry's
//        copy). A decision where the model re-uploads and the registry keeps compares the fetched chunks with the
//        registry's copy: none moved = PAGE_ONLY (its own outcome, reported, not gated: rng same), one moved = MISSED
//        (rng moved), no copy = MISSED (rng unk). A page-only decision leaves the page DIVERGED (the registry's copy
//        differs from RAM in unfetched bytes): every later decision on it while the registry keeps - the model's keeps
//        too - re-checks the fetched chunks, so a chunk a later request fetches for the first time (the first request
//        did not) and that moved since the registry's copy is a MISSED decision (why stale), not an agreement -
// and by the guest physical grant holding the page (noteGrant: MmAllocatePhysicalMemoryEx grants of 1 MiB and up,
// with the guest lr that asked; census_missed_grant). The samples are per attribute cell (the first 4, then powers of
// two of the cell's count). NAMING THE WRITER: the first 8 misses outside the command buffer, then the powers of two of
// that count (at most SR_MEM_RES_TRAPS, default 32; 0 = none), arm a one-shot trap - the page's 0x8 and 0xA copies
// write-protected; the first write faults into the census's vectored handler, which records the faulting thread, the
// host rip (exe-relative) and the guest lr / ctr / r1 of its context, and unprotects the page (or, when the write watch
// holds it, leaves the release to the write watch's handler); a kernel-mode host write (NtReadFile, which would fail on
// a protected page) disarms first through hostWillWrite (gww::HostWriteScope) and is recorded as writer=host. The
// census line after a fire prints `[MEM_RES] WRITER #n ... in_fetch=0|1` (whether the written chunk was ever fetched).
// census_missed_attr and census_missed_grant count the missed and the page-only decisions together (rng same = exactly
// the page-only ones); traps arm on either. Traps cost a fault each and change no decision.
// memres4's diagnostics (observe-only, no gate counter moves): a dword-exact fetched mask beside the chunk mask, and per
// page a ring of the last 8 request ranges that grew the page's fetched dwords (+ the newest request); a MISSED /
// PAGE_ONLY sample then prints `at=` (the moved fetched chunk, else the first chunk that differs from the registry's
// copy), `mdw=` (which of its 4 dwords any request fetched), `bytes=` (its 32 bytes now) and `reqs=` (the newest 4
// recorded ranges covering it) / `last=`; a WRITER prints `on=` (missed / page_only / alias), `dw_in_fetch=`, `req=` and
// `hdr=` (the 32 bytes from va - 8 before the write); an ALIAS sample prints the last note's source, the grant holding
// the page and the allocators' records (phys_window.hpp gpuGrantFlags / physPageOwner / physPageContested), and the
// first 4 ALIAS pages (then the powers of two of that count) arm a trap on both window copies - its WRITER's va names
// the window the CPU wrote.
// memres5 (the gate's fetched bytes and the registry's window, after memres4): the fetched set and the registry's copy
// are DWORD-exact (a 16-bit hash per dword in a block allocated at the page's first copy; the 16-byte chunk mask stays
// for the WRITER's in_fetch only), and a vertex fetch's bytes are INDEX-BOUNDED (DrawScope / SlotScope: only the
// vertices the draw's indices address, where noteVsUcode's walk proves the slot's every vfetch indexes by an untouched
// r0.x; fail-closed to the declared range, counted fetch.fallback.shader / .draw - the grader FAILs an era whose
// fallbacks exceed its fixed share). The registry's window (its copy and the source window
// check) is the page's live owner (the model's provenance 8 / A), else its last grant's (noteGrantWindow), else 0x8;
// win.live / win.grant / win.default count the rule that named it at each registry upload.
// Index buffers are read by the CPU conversion (pm4StageVsf), not through the page model: ibRead grades the registry
// as the key of a hypothetical IB conversion cache (the census hashes the IB bytes per draw) - its own numbers
// (census_ib), outside the page gate (the conversion runs per draw today: no such cache ships).
// Nothing the model or the renderer reads is written: every hook takes values and returns void; the model still
// decides every upload, so =census decides as the default does (the census costs time: its hashes, locks and lines).
// Lines (cumulative since process start; tools/perf/mem_res_census.py grades them per era):
//   [MEM_RES] census <why> swaps=.. decisions=.. agree_up=.. agree_keep=.. over=.. missed=.. clobber=.. race.note=..
//             race.lock=.. missed.content=.. ... win.alias_diff=.. cov.unseen=.. cov.unaccounted=.. keep.wt.unchecked=..
//   [MEM_RES] census_missed / census_over / census_first / census_missed_cells / census_hazard / census_ib /
//             census_coher / census_missed_attr / census_missed_grant <why> ...
//   [MEM_RES] MISSED #n (per attribute cell) / CLOBBER #n / ALIAS #n / UNSEEN #n / COHER #n ... (samples: the first 64,
//             then powers of two) / WRITER #n (a trap fired)
// emitted with the CP's [GPU_CP] census (swaps 1-4, every 120 swaps, exit, the terminal snapshot). Under SR_ORACLE=0
// the census is compiled out (inline no-ops below; the unit is in ORACLE_SOURCES): the release configuration is
// unchanged.
#include <cstdint>
#include <string>
#include <memory>
#include <vector>

#include "../kernel/sr_oracle.hpp"
#if SR_ORACLE
#include <array>
#include <mutex>
#endif
#include "resource_registry.hpp"

namespace sr::gfx::memres {

enum class Kind : uint8_t { kOther = 0, kVb, kIb, kTex, kResolve, kScanout, kCount };
enum class Prov : uint8_t { k8 = 0, kA, kLegacy8, kLegacyA, kUndecided, kCount };
enum class Site : uint8_t { kDrain = 0, kPrepass, kRender, kCount };
// Why the model uploaded (kGpu is the census's: the registry kept the GPU's data; kStale is the census's too: the model
// kept a page whose registry copy is stale in bytes a request fetched - the divergence an earlier page-only decision left).
enum class Why : uint8_t { kContent = 0, kWindow, kGpu, kStale, kCount };
// One page decision's class (exactly one per decision). kPageOnly (memres3): the model re-uploaded, the registry would
// keep, and no byte any request ever fetched from the page differs from the registry's copy - a page-granular false miss
// (only bytes the GPU does not fetch moved: SetStreamSource's header-fence stamp, RenderModel_SwapMeshChunkBindings),
// reported, never gated.
enum class Outcome : uint8_t {
  kAgreeUp = 0, kAgreeKeep, kOver, kMissed, kClobber, kRaceNote, kRaceLock, kPageOnly, kCount
};
// How a request / validation disposed of a page (coverage).
enum class Cov : uint8_t {
  kDecided = 0,  // decided in the request (the write watch's accept, the range cache's keep)
  kCand,         // made a validation candidate
  kVal,          // a candidate decided at pm4ShmValidate
  kReuse,        // skipped on a stamp of this epoch the census recorded
  kReuseFailed,  // ... whose validation failed (no host pointer: nothing was served)
  kUnseen,       // skipped on a stamp the census never recorded: an unhooked decision path (a gap)
  kDeferred,     // skipped by the prepass: a page this frame's resolves write (validated at the bind)
  kFailed,       // a candidate with no host pointer (no decision, nothing uploaded)
  kRebase,       // stamped by a readback (pm4ShmReadback: the GPU's bytes copied back into guest RAM)
  kCount
};
enum class Feed : uint8_t { kWt = 0, kRc, kCount };  // the model's keeps without a hash: the write watch, the range cache
enum class Chk : uint8_t { kChecked = 0, kStale, kMismatch, kUnchecked, kCount };
// The missed class's attributes (above).
enum class InCb : uint8_t { kUnknown = 0, kIn, kOut, kCount };
enum class Seg : uint8_t { kNone = 0, kUp, kZpass, kCount };
enum class Rng : uint8_t { kUnknown = 0, kSame, kMoved, kCount };

// The guest's coherency statuses (COHER_STATUS_HOST) and the census's class of each. kCoherShaderSync (memres2's fifth):
// the SDK's shader-constant sync before its constant runs (821075E0 / 82107058; kRkShaderConstSync) - SIZE 0, BASE
// 0x1000: a shader-cache action with no memory range, counted (note.shsync), never a note.
constexpr uint32_t kCoherInval = 0x03000000u, kCoherTexLock = 0x02000000u, kCoherCmdBuf = 0x01000000u,
                   kCoherResolve = 0x03000100u, kCoherShaderSync = 0x00010000u;

constexpr uint32_t kKinds = uint32_t(Kind::kCount), kProvs = uint32_t(Prov::kCount),
                   kSrcs = uint32_t(Src::kCount), kSites = uint32_t(Site::kCount), kWhys = uint32_t(Why::kCount),
                   kOutcomes = uint32_t(Outcome::kCount), kCovs = uint32_t(Cov::kCount),
                   kFeeds = uint32_t(Feed::kCount), kChks = uint32_t(Chk::kCount), kInCbs = uint32_t(InCb::kCount),
                   kSegs = uint32_t(Seg::kCount), kRngs = uint32_t(Rng::kCount);
constexpr uint32_t kCoherSlots = 8;   // distinct statuses counted by value (more: coher.overflow)
constexpr uint32_t kMaxGrants = 128;  // grants of 1 MiB and up remembered (more: grant.overflow)
constexpr uint32_t kMaxTraps = 64;    // trap slots (SR_MEM_RES_TRAPS caps the armed ones)
constexpr uint32_t kFbReasonsMax = 16;  // memres6: the fallback reasons' span (Totals::fbReason)
const char* kindName(Kind k) noexcept;
const char* provName(Prov p) noexcept;
const char* siteName(Site s) noexcept;
const char* outcomeName(Outcome o) noexcept;
const char* covName(Cov c) noexcept;
const char* chkName(Chk c) noexcept;
const char* inCbName(InCb c) noexcept;
const char* segName(Seg s) noexcept;
const char* rngName(Rng r) noexcept;
inline Prov provOfWindow(int win) noexcept { return win == 0xA ? Prov::kA : win == 8 ? Prov::k8 : Prov::kUndecided; }

// A trap's record (WRITER): the page, what armed it, and who wrote it.
struct TrapRec {
  uint32_t page, kind, src, armClock;
  bool fired, host;      // host = a kernel-mode host write (hostWillWrite) disarmed it
  bool inFetch;          // the written 16-byte chunk lies in a chunk some request fetched from the page
  bool dwInFetch;        // memres4: the written dword itself lies in a byte some request fetched (dword-exact)
  uint32_t on;           // memres4: what armed it - 0 a missed decision, 1 a page-only one, 2 an ALIAS
  uint32_t va, lr, ctr, r1, tid;
  uint64_t ripRva;       // the faulting host rip minus the exe's base (0 = a host write scope / outside the exe)
  uint64_t req;          // memres4: the newest recorded request range covering the written dword (0 = none)
  uint8_t hdr[32];       // memres4: the 32 bytes at va - 8 before the write (a D3D header when va is its fence)
};

// The cumulative counters (one snapshot; tests and the census line). The page decisions' totals are sums of `cell`.
struct Totals {
  uint64_t cell[kOutcomes][kKinds][kProvs][kSrcs][kSites][kWhys][2];  // [..][movie]
  uint64_t decisions, outcome[kOutcomes];
  uint64_t missedWhy[kWhys], missedMovie;
  uint64_t missedBy[kKinds][kProvs][kSrcs][kSites];  // the MISSED outcome's attribution
  uint64_t overBy[kKinds][kSrcs];
  uint64_t first, firstBy[kKinds][kProvs];  // registry first-use uploads (whatever the outcome)
  uint64_t winHeur, winAliasSame, winAliasDiff, winUnmapped, clobberBoth, reuseNoted;
  uint64_t cov[kCovs], covReq, covValCands, covUnaccounted;
  uint64_t keep[kFeeds][kChks];
  uint64_t ibReads, ibAgreeUp, ibAgreeKeep, ibMissed, ibOver, ibFirst, ibEvict, ibMissedBy[kProvs][kSrcs];
  uint64_t noteCalls[kSrcs], notePages[kSrcs];
  uint64_t noteWhole, noteRefused, noteLowVa, noteUnlockNoFetch, noteLock, noteLockNoFetch, noteUnlockUnmatched;
  uint64_t lockOpenPages, movieMarks;  // gauges: pages inside an open guest Lock now; movie-plane marks set
  // memres2: the Resolve's requests (never a CPU note); every request by status value; the missed class's attributes
  // and grants; the inline-vertex allocations; the traps.
  uint64_t noteResolve, noteResolvePages, noteShSync, coherRequests, coherOverflow;
  uint32_t coherStatus[kCoherSlots];
  bool coherUsed[kCoherSlots];
  uint64_t coherCalls[kCoherSlots], coherPages[kCoherSlots];
  uint64_t missedAttr[kKinds][kSrcs][kInCbs][kSegs][kRngs];
  uint32_t grants, grantVa[kMaxGrants], grantBytes[kMaxGrants], grantLr[kMaxGrants];
  uint64_t grantOverflow, missedGrant[kMaxGrants + 1];  // [kMaxGrants] = in no remembered grant
  uint64_t upAllocs, upAllocsZpass;
  uint32_t cbLo, cbHi;  // the command buffer's physical range as last learned (0, 0 = not yet)
  uint64_t trapArmed, trapFired, trapHost, trapArmFail, trapDeclined;
  uint32_t traps;
  TrapRec trap[kMaxTraps];
  // memres5: the vertex fetch requests whose fetched bytes were index-bounded, and the fail-closed fallbacks to the
  // declared range (the VS's vfetch of the slot not provably indexed by r0.x; no draw index range); the registry's window
  // by the rule that named it (a live owner, the last grant, the 0x8 default).
  uint64_t fetchBounded, fetchFbShader, fetchFbDraw;
  uint64_t winLive, winGrant, winDefault;
  // memres6: the requests bounded through a computed index (fetch.bounded counts them too), the soundness witness's
  // draws (each evaluated exactly per vertex) and those with an exact index outside the bound, the fallbacks by reason
  // (resource_census.cpp kFb*: slot op rel ctl fmt read many range state clob long vs lit).
  uint64_t fetchBoundedComp, boundSampled, boundViolations;
  uint64_t fbReason[kFbReasonsMax];
};

#if SR_ORACLE
// Full-page observation beside the existing hash census. This never changes a
// registry decision. "known" means the bounded observations agreed, not an
// atomic guest-memory snapshot or complete allocation-epoch fetch coverage.
struct FetchPageVersion {
  uint64_t ownerSum = 0, grantGeneration = 0;
  uint32_t window = 0, cpu = 0, gpu = 0, uploaded = 0, epoch = 0, ownerSeq = 0;
  bool known = false;
};
struct FetchPagePayload {
  uint32_t page = 0;  // physical byte address, 4096-aligned
  std::array<uint8_t, 4096> before{}, after{};
  std::array<uint64_t, 16> changed{}, fetched{};
  std::array<uint64_t, 8> requestRing{};  // packed physical<<32 | length<<4 | kind
  uint64_t requestLast = 0;
  uint32_t requestHead = 0;
  FetchPageVersion baseline, current;
  uint64_t grantDigest = 0;
  uint32_t grantCount = 0, grantSerial[2] = {};
  bool known = false;
  std::string reason;
};
struct FetchPayloadStats {
  uint64_t budget = 0, charged = 0, peak = 0, seen = 0, retained = 0, capped = 0, invalid = 0;
};
// Shared production/fixture storage. The fixed page index and every retained
// entry are charged before allocation; no eviction or later capacity rescue.
class FetchPayloadStore {
 public:
  explicit FetchPayloadStore(uint64_t budget);
  ~FetchPayloadStore();
  FetchPayloadStore(const FetchPayloadStore&) = delete;
  FetchPayloadStore& operator=(const FetchPayloadStore&) = delete;
  static uint64_t indexBytes() noexcept;
  static uint64_t entryBytes() noexcept;
  void record(uint32_t page, const uint8_t* bytes, const FetchPageVersion& version, bool stable);
  std::shared_ptr<const FetchPagePayload> capture(uint32_t page, const uint8_t* bytes,
      const FetchPageVersion& version, bool stable, const std::array<uint64_t, 16>& fetched,
      const std::array<uint64_t, 8>& ring = {}, uint64_t last = 0, uint32_t head = 0);
  FetchPayloadStats stats() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// A diagnostic target is selected once from an observed post-HUD miss. The
// originating command or deferred validation batch is excluded; this cannot
// reconstruct its earlier fetches. Ordinal zero names a validation batch.
struct FetchTargetOrigin {
  uint64_t sequence = 0, ownerSum = 0, grantGeneration = 0, grantDigest = 0;
  uint32_t ordinal = 0, target = 0, missed = 0, cpu = 0, gpu = 0, uploaded = 0, clock = 0;
  uint32_t epoch = 0, ownerSeq = 0, grantCount = 0, serial[2] = {};
  uint32_t censusGrantVa = 0, censusGrantBytes = 0, censusGrantLr = 0;
  bool known = false;
  const char* reason = "origin_pending";
  std::shared_ptr<const FetchPagePayload> payload;
};
class FetchTargetLatch {
 public:
  bool arm(const char* exactPath);
  bool canSelect();
  bool select(const FetchTargetOrigin& origin);
  bool pending(uint64_t sequence, FetchTargetOrigin& origin);
  bool finalize(const FetchTargetOrigin& origin);
  bool later(uint64_t sequence, FetchTargetOrigin& origin);
 private:
  std::mutex mutex_;
  bool armed_ = false, selected_ = false, finalized_ = false;
  FetchTargetOrigin origin_;
};
bool fetchAutoTargetMode() noexcept;
void noteFetchHudRequest(uint32_t pathVA, uint8_t* base) noexcept;
bool fetchTargetForCurrentDraw(FetchTargetOrigin& origin);
class FetchAdmissionScope {
 public:
  explicit FetchAdmissionScope(uint32_t ordinal);
  ~FetchAdmissionScope();
  FetchAdmissionScope(const FetchAdmissionScope&) = delete;
  FetchAdmissionScope& operator=(const FetchAdmissionScope&) = delete;
 private:
  uint64_t previousSequence_ = 0, sequence_ = 0;
  uint32_t previousOrdinal_ = 0;
};
// Opt-in, sampled-draw diagnostics only. These do not alter any census mask,
// ownership decision, coverage counter or acceptance grade. A complete result
// bounds only this captured draw, never an allocation epoch.
struct FetchCaptureInput {
  uint64_t drawId = 0, vsHash = 0;
  const uint32_t* indices = nullptr;  // actual converted host-endian uint32 IB
  uint32_t indexCount = 0, firstVertex = 0;
  bool indexed = false, stripCut = false;
  const uint32_t* constants = nullptr;  // actual staged 1024 dwords
  const uint32_t* fetch = nullptr;      // actual bound 192 dwords
  uint32_t constantBlocks = 0;         // actual valid 16 blocks; explicit, 0 means none
};
struct FetchSpan { uint32_t phys = 0, len = 0; };
struct FetchFrozenSource {
  uint64_t drawId = 0;
  uint32_t phys = 0;
  const uint8_t* bytes = nullptr;
  size_t byteCount = 0;
  // Backend witness for the complete F1 range at this draw's command position.
  // Nonzero and equal only after its pending copies/earlier resolves are recorded
  // and the private GPU snapshot completes. Current guest RAM is not a witness.
  uint64_t recordedBefore = 0, recordedAfter = 0;
  bool complete = false;
};
struct FetchUse {
  uint32_t instruction = 0, slot = 0, cls = 0, phys = 0, len = 0;
};
struct FetchAnalysis {
  uint64_t drawId = 0, vsHash = 0;
  bool complete = false;
  std::string reason;
  std::vector<FetchUse> uses;  // candidate bytes, with interval arithmetic widening
};
struct FetchChangedWord {
  uint32_t phys = 0, beforeBE = 0, afterBE = 0;
  bool cumulativeFetched = false, candidate = false;
};
struct FetchPayloadIntersection {
  bool complete = false;
  std::string reason;
  std::vector<FetchChangedWord> changedWords;
  uint32_t candidateWords = 0;
  std::array<uint64_t, 16> candidateMask{};
};
FetchPayloadIntersection intersectFetchPayload(const FetchPagePayload& payload, const FetchAnalysis& analysis);
struct FetchCapture;
// Inputs are copied synchronously; invalid/budget-limited captures retain an
// UNKNOWN reason. The caller owns sampling, draw IDs and command-order binding.
struct FetchShaderStats { uint64_t bytes = 0, capacity = 0, retained = 0, capped = 0; };
FetchShaderStats consumerFetchShaderStats() noexcept;
std::shared_ptr<const FetchCapture> captureFetchDraw(const FetchCaptureInput& input);
// Confirm the same staged bytes immediately before the actual native bindings.
// Analysis alone cannot establish that the admitted capture was submitted.
bool confirmFetchDraw(const FetchCapture& capture, const FetchCaptureInput& input) noexcept;
FetchSpan requestedFetchF1(const FetchCapture& capture) noexcept;
FetchAnalysis analyzeFetchDraw(const FetchCapture& capture, const FetchFrozenSource& source);

bool census() noexcept;  // SR_MEM_RESOURCES=census (read once)

void noteCoher(uint32_t status, uint32_t base, uint32_t size) noexcept;
void noteReadFile(uint32_t guestVA, uint32_t len) noexcept;
// tex = the texture header's guest address (the Lock / Unlock pair's key); fetch6 = the header's fetch constant (six
// dwords, host-endian; nullptr = unreadable).
void noteLock(uint32_t tex, const uint32_t* fetch6) noexcept;
void noteUnlock(uint32_t tex, const uint32_t* fetch6, bool moviePlane) noexcept;
void noteGpuWrite(uint32_t phys, uint32_t len) noexcept;
// mem-native: an EXACT CPU write of the dwords [guestVA, +len) inside a page that no coherency record announces (the
// D3D driver's header fence stamps, kernel/native_resource_stamps.cpp): the registry copies just those dwords at the
// page's next decision (a keep's copy takes their bytes then), the page's generations unchanged - the native request's
// patch (gfx/resource_memory.hpp notePatch). Reported in census_patch, never gated.
void notePatch(uint32_t guestVA, uint32_t len) noexcept;
// mem-native (the orchestrator's 2026-10-17 decision): [guestVA, +len) was written by a CPU-only writer the GPU never
// reads (RenderModel_SwapMeshChunkBindings' draw records, kernel/native_resource_stamps.cpp). Classification only: a
// miss of only such dwords is missed_cpu_only. Moves no generation, notes nothing.
void noteCpuOnly(uint32_t guestVA, uint32_t len) noexcept;
// memres2's attributes: an inline-vertex allocation [phys, +bytes) carved from the command buffer (the native
// BeginVertices / DrawVerticesUP: zpass = a Z-pass list recording, dev+0x3290 != 0) and the command buffer's range
// [cbLo, cbHi) (physical, from dev+0x34AC / 0x34B0; 0, 0 = unknown); a guest physical grant [va, +bytes) and the guest lr
// that asked (MmAllocatePhysicalMemoryEx; under 1 MiB ignored).
void noteUpAlloc(uint32_t phys, uint32_t bytes, bool zpass, uint32_t cbLo, uint32_t cbHi) noexcept;
void noteGrant(uint32_t va, uint32_t bytes, uint32_t lr) noexcept;
// memres5 (the registry's window): a guest grant [va, +bytes) of GPU-visible memory - MmAllocatePhysicalMemoryEx
// (noteGrant calls it), the XMemAlloc override's physical route and its heap-routed (0x8) grants
// (kernel/gpu_ring_alloc_fix.cpp): its pages' last-grant window is the VA's (0xA at or above 0xA0000000, else 0x8), kept
// after the grant is released. The registry serves a page from its live owner's window, else the last grant's, else 0x8.
void noteGrantWindow(uint32_t va, uint32_t bytes) noexcept;
// memres5 (index-bounded fetched bytes): a vertex shader's ucode at its load (RecordEngine's IM_LOAD; big-endian bytes):
// per vertex fetch slot, whether every vfetch of it takes its index from r0.x directly (not relative, r0 not written
// before it, no loop / call / jump in the program) and its stride / offsets / element sizes. Analysed once per hash.
void noteVsUcode(uint64_t hash, const uint8_t* ucodeBE, size_t bytes) noexcept;
// The draw's vertex index range [vMin, vMax] as the GPU will read it (`any` = false: the draw reads no vertex) and its
// VS: while it lives, a vertex fetch request of slot `SlotScope` marks fetched only the bytes those indices address
// ([base + (vMin * stride + minOffset) * 4, base + (vMax * stride + maxEnd) * 4)); without one, or for a slot the
// analysis could not prove, the request's declared range (fail-closed; counted fetch.fallback.*).
// memres6: `consts` (the draw's VS float constants c0-c255, four dwords each, as the constant upload holds them) and
// `fetch` (its 96 vertex fetch constants, two dwords each) let the census bound a computed index - the world family's
// prologue evaluated per vertex as the host translation computes it, then its index stream read back (both pointers
// must outlive the scope; nullptr = no computed bound: those slots fall back, reason state).
class DrawScope {
 public:
  DrawScope() noexcept = default;  // inactive (the census off)
  DrawScope(uint64_t vsHash, bool rangeKnown, bool any, uint32_t vMin, uint32_t vMax, const uint32_t* consts = nullptr,
            const uint32_t* fetch = nullptr) noexcept;
  // The draw's index stream as the conversion will read it (vertexIndexRange's arguments; indexed && !ib = the IB
  // unreadable: no range known) - the census computes the range itself (nothing it returns reaches the draw).
  DrawScope(uint64_t vsHash, bool indexed, const uint8_t* ib, uint32_t n, bool idx32, uint32_t endian, uint32_t offset,
            bool clampOn, uint32_t minV, uint32_t maxV, bool stripCut, uint32_t resetIndex,
            const uint32_t* consts = nullptr, const uint32_t* fetch = nullptr) noexcept;
  ~DrawScope();
  DrawScope(const DrawScope&) = delete;
  DrawScope& operator=(const DrawScope&) = delete;

 private:
  bool on_ = false;
};
class SlotScope {
 public:
  explicit SlotScope(uint32_t slot) noexcept;
  ~SlotScope();
  SlotScope(const SlotScope&) = delete;
  SlotScope& operator=(const SlotScope&) = delete;

 private:
  uint32_t prev_ = 0;
  bool on_ = false;
};
// The draw's vertex index range from its index stream as the conversion reads it (pm4StageVsf: each raw index plus
// the offset, masked to 24 bits, clamped to [minV, maxV] when clampOn; a strip's reset index skipped when cut). ib ==
// nullptr: auto-indexed (raw index i). Returns false when no index survives (the draw reads no vertex).
bool vertexIndexRange(const uint8_t* ib, uint32_t n, bool idx32, uint32_t endian, uint32_t offset, bool clampOn,
                      uint32_t minV, uint32_t maxV, bool stripCut, uint32_t resetIndex, uint32_t& vMin,
                      uint32_t& vMax) noexcept;
// A kernel-mode host write into guest memory is about to happen (gww::HostWriteScope - NtReadFile): an armed trap over
// it is released first (recorded as writer=host) - a protected page would fail the read.
void hostWillWrite(uint32_t guestVA, uint32_t len) noexcept;

// A request's (pm4ShmRequestT) or a validation's (pm4ShmValidate) page tally: every page must be disposed of by
// exactly one hook while the scope lives (the innermost scope of the thread counts); a difference is cov.unaccounted.
// A request names its byte range [phys, +len): the range each page's fetch covers (the missed class's rng attribute).
class WalkScope {
 public:
  WalkScope(uint32_t pages, bool validation, uint32_t phys = 0, uint32_t len = 0) noexcept;
  ~WalkScope();
  WalkScope(const WalkScope&) = delete;
  WalkScope& operator=(const WalkScope&) = delete;
  void count(uint32_t n) noexcept { got_ += n; }
  bool validation() const noexcept { return val_; }
  uint32_t seq() const noexcept { return seq_; }
  uint32_t phys() const noexcept { return phys_; }
  uint32_t len() const noexcept { return len_; }

 private:
  uint32_t want_ = 0, got_ = 0, seq_ = 0, phys_ = 0, len_ = 0;
  WalkScope* prev_ = nullptr;
  bool on_ = false, val_ = false;
};

// The request walk's events. `stamp` = the epoch the model stamped the page with (m_shmEpochOf), `cpStamp` = a stamp of
// the CP's drain epoch (collect mode, or the synchronous path) rather than the render's frame start.
void acceptPage(uint32_t page, Site site, uint32_t stamp, bool cpStamp, Prov prov) noexcept;  // the write watch's keep
void candPage(uint32_t page, Site site, uint32_t stamp, bool cpStamp) noexcept;               // a validation candidate
// A page the walk skipped as decided earlier: `stamp` = the stamp it relied on; `collect` = the drain's check (the
// stamp must be this epoch's), else the render's (any stamp at or after the frame start).
void skipReuse(uint32_t page, uint32_t stamp, bool collect) noexcept;
// The range memo's reuse: the range was disposed of in this drain `epoch` - by the walk (a stamp of that epoch) or by the
// range cache's keep (which stamps nothing in the model: the census keeps its epoch apart, so a walk's stamp survives it).
void skipMemo(uint32_t page, uint32_t epoch) noexcept;
void skipDeferred(uint32_t page) noexcept;  // the prepass's resolve mask
// The range cache's keep of [p0, p1] (collect mode; `epoch` = the drain's).
void modelKeepRange(uint32_t p0, uint32_t p1, Site site, Prov prov, uint32_t epoch) noexcept;
// The checkers: shmWtCheck's result for a page (wtChecked, inside shmWtCheck), the range cache check's window mismatch
// (rcMismatch) and its re-hash (rcChecked, right after the loop's shmWtCheck: the result moves into the range's
// tally). The next keep of that page (acceptPage) / the range's keep (modelKeepRange) is classified by them; a keep
// with none is unchecked.
void wtChecked(uint32_t page, bool stale) noexcept;
void rcMismatch(uint32_t page) noexcept;
void rcChecked(uint32_t page) noexcept;
// The validation's events: snapPage before the page is protected and hashed (the generations the decision binds to),
// failedPage for a candidate with no host pointer, modelPage for each decision.
void snapPage(uint32_t page) noexcept;
void failedPage(uint32_t page) noexcept;
void modelPage(uint32_t page, bool upload, Why why, Prov prov) noexcept;
void rebasePage(uint32_t page, uint32_t stamp, bool cpStamp) noexcept;  // pm4ShmReadback re-baselined and stamped it
// The IB conversion's read of [phys, +bytes) from `host` (the bytes it converts), served from `prov`'s window.
void ibRead(uint32_t phys, uint32_t bytes, const uint8_t* host, Prov prov) noexcept;

// The fetch kind of this thread's requests while it lives (nests; a no-op while the census is off).
class KindScope {
 public:
  explicit KindScope(Kind k) noexcept;
  ~KindScope();
  KindScope(const KindScope&) = delete;
  KindScope& operator=(const KindScope&) = delete;

 private:
  uint8_t prev_;
  bool on_;
};

void censusLine(const char* why, uint64_t swaps) noexcept;
// The census lines censusLine logs, '\n'-separated, without the log's prefix (tests print them as a roll would).
std::string censusText(const char* why, uint64_t swaps);
void snapshot(Totals& out) noexcept;
Registry* registry() noexcept;  // the shadow registry (nullptr while the census is off)
// Tests (tools/difftest/mem_resources_test.cpp): switch the census on or off in-process and start from zero; replace
// the guest-memory view (a window's page bytes) and the allocators' owner version (nullptr = the process's own).
void censusResetForTest(bool on) noexcept;
struct TestHooks {
  using HostFn = const uint8_t* (*)(uint32_t guestVA);  // a 4 KiB page's bytes, or nullptr
  using OwnerVerFn = uint32_t (*)(uint32_t phys);       // the owner version of the page (0 = unknown)
  using ProtectFn = bool (*)(uint32_t page, bool readOnly);  // a trap's protection of a page's two copies
  HostFn host;
  OwnerVerFn ownerVer;
  ProtectFn protect = nullptr;  // nullptr = VirtualProtect on the guest arena (the vectored handler installed)
  int trapLimit = -1;           // -1 = SR_MEM_RES_TRAPS (default 32)
  // mem-native: runs inside a decision between its exact-write patch take and its comparison (a stamp racing the
  // decision); nullptr = none
  using AfterTakeFn = void (*)(uint32_t page);
  AfterTakeFn afterTake = nullptr;
  // mem-native: the census's clock in milliseconds for the cpu_only names' freshness; nullptr = steady_clock
  using NowMsFn = uint32_t (*)();
  NowMsFn nowMs = nullptr;
};
void censusSetTestHooks(const TestHooks* hooks) noexcept;
// Tests: the vectored handler's own path for a write to guestVA by a thread at host rip `rip` (true = a trap fired).
bool censusTrapHitForTest(uint32_t guestVA, uint64_t rip) noexcept;
// Tests (memres4): the diagnostic tail of the last MISSED (0) / PAGE_ONLY (1) / ALIAS (2) sample logged while test
// hooks are set ("" before any).
std::string censusLastSampleForTest(int which);
// Tests (memres6): the witness's 1 in k (1 = every computed-bound draw; the census's own is 64 after the first 256).
void censusSetWitnessKForTest(uint32_t k) noexcept;
// Tests (memres6): narrow every computed bound's end by k indices (the witness must report the exact indices it loses;
// 0 = the census's bound).
void censusSetBoundShrinkForTest(uint32_t k) noexcept;
// Tests (memres6): the integer index range [lo, hi] the census bounds slot `slot` to in the open DrawScope of this thread
// (false: no computed bound - the slot falls back, *reason its kFb* reason, or it is not a computed slot).
bool censusSlotBoundForTest(uint32_t slot, int64_t& lo, int64_t& hi, uint32_t* reason = nullptr) noexcept;
// Tests (mem-native): the open draw scope holds a used-vertex bitmap (markUsedVertices applies).
bool censusDrawExactForTest() noexcept;
// mem-native: the open draw's converted indices against its used-vertex bitmap (vtx_stale: an index it lacks).
void censusCheckUsed(const uint32_t* idx, uint32_t n) noexcept;
// memres6: a literal constant the host translation bakes into a VS (gfx/resource_census_literals.inc, generated by
// tools/perf/mem_res_literals.py): its ucode hash, register, flags (1 = the corpus defines it two ways) and dwords.
struct LiteralRow {
  uint64_t hash;
  uint32_t reg, flags;
  uint32_t v[4];
};
// Tests (memres6): replace the literal table (rows sorted by hash, then register; nullptr = the generated table). Set
// before the VS's noteVsUcode (its analysis reads it).
void censusSetLiteralsForTest(const LiteralRow* rows, size_t n) noexcept;
// The generated table's rows (the family test checks its own corpus reading against them).
const LiteralRow* censusLiterals(size_t& n) noexcept;
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): the census is compiled out; its hooks do nothing.
inline bool census() noexcept { return false; }
inline void noteCoher(uint32_t, uint32_t, uint32_t) noexcept {}
inline void noteReadFile(uint32_t, uint32_t) noexcept {}
inline void noteLock(uint32_t, const uint32_t*) noexcept {}
inline void noteUnlock(uint32_t, const uint32_t*, bool) noexcept {}
inline void noteGpuWrite(uint32_t, uint32_t) noexcept {}
inline void notePatch(uint32_t, uint32_t) noexcept {}
inline void noteCpuOnly(uint32_t, uint32_t) noexcept {}
inline void noteUpAlloc(uint32_t, uint32_t, bool, uint32_t, uint32_t) noexcept {}
inline void noteGrant(uint32_t, uint32_t, uint32_t) noexcept {}
inline void noteGrantWindow(uint32_t, uint32_t) noexcept {}
inline void noteVsUcode(uint64_t, const uint8_t*, size_t) noexcept {}
class DrawScope {
 public:
  DrawScope() noexcept = default;
  DrawScope(uint64_t, bool, bool, uint32_t, uint32_t, const uint32_t* = nullptr, const uint32_t* = nullptr) noexcept {}
  DrawScope(uint64_t, bool, const uint8_t*, uint32_t, bool, uint32_t, uint32_t, bool, uint32_t, uint32_t, bool,
            uint32_t, const uint32_t* = nullptr, const uint32_t* = nullptr) noexcept {}
};
class SlotScope {
 public:
  explicit SlotScope(uint32_t) noexcept {}
};
inline bool vertexIndexRange(const uint8_t*, uint32_t, bool, uint32_t, uint32_t, bool, uint32_t, uint32_t, bool,
                             uint32_t, uint32_t&, uint32_t&) noexcept {
  return false;
}
inline void hostWillWrite(uint32_t, uint32_t) noexcept {}
class WalkScope {
 public:
  WalkScope(uint32_t, bool, uint32_t = 0, uint32_t = 0) noexcept {}
};
inline void acceptPage(uint32_t, Site, uint32_t, bool, Prov) noexcept {}
inline void candPage(uint32_t, Site, uint32_t, bool) noexcept {}
inline void skipReuse(uint32_t, uint32_t, bool) noexcept {}
inline void skipMemo(uint32_t, uint32_t) noexcept {}
inline void skipDeferred(uint32_t) noexcept {}
inline void modelKeepRange(uint32_t, uint32_t, Site, Prov, uint32_t) noexcept {}
inline void wtChecked(uint32_t, bool) noexcept {}
inline void rcMismatch(uint32_t) noexcept {}
inline void rcChecked(uint32_t) noexcept {}
inline void snapPage(uint32_t) noexcept {}
inline void failedPage(uint32_t) noexcept {}
inline void modelPage(uint32_t, bool, Why, Prov) noexcept {}
inline void rebasePage(uint32_t, uint32_t, bool) noexcept {}
inline void ibRead(uint32_t, uint32_t, const uint8_t*, Prov) noexcept {}
class KindScope {
 public:
  explicit KindScope(Kind) noexcept {}
};
inline void censusLine(const char*, uint64_t) noexcept {}
inline bool censusTrapHitForTest(uint32_t, uint64_t) noexcept { return false; }
struct LiteralRow;
inline void censusSetWitnessKForTest(uint32_t) noexcept {}
inline void censusSetBoundShrinkForTest(uint32_t) noexcept {}
inline bool censusSlotBoundForTest(uint32_t, int64_t&, int64_t&, uint32_t* = nullptr) noexcept { return false; }
inline bool censusDrawExactForTest() noexcept { return false; }
inline void censusCheckUsed(const uint32_t*, uint32_t) noexcept {}
inline void censusSetLiteralsForTest(const LiteralRow*, size_t) noexcept {}
#endif

}  // namespace sr::gfx::memres
