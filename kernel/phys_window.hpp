#pragma once
// Allocation provenance and alias mutation stamps are native.
// H-WIN/H-UNIQ capture and comparison interfaces below are Oracle-only.
// Historical diagnostic contract: WHICH MEMORY WINDOW DID A GPU READ USE?
//
// On the console the physical windows 0xA0000000/0xC0000000/0xE0000000 alias
// ONE 512 MB physical RAM, so a GPU physical address names exactly one byte.
// Our flat guest arena does not fold them, and our allocators hand out GPU
// memory in two places:
//   * guestPhysAlloc - a bump region growing down from 0xBF000000 across up to
//     944 MiB, so it crosses into the 0x8 window and can hold two VAs with the
//     same masked physical address;
//   * guestHeapAlloc - the main heap in the 0x8 window, used by the
//     sub_82467F40 override for physical requests of 256 KB .. 64 MB (larger
//     requests bypass the override to MmAllocatePhysicalMemoryEx).
// So a fetch constant's physical address p can live at 0x80000000|p OR at
// 0xA0000000|p, and every HLE read site decides (0x8 first; 0xA when the 0x8
// bytes are all zero; plus the IB range rescue and VB-follows-IB rules). This
// module records, per read, which candidate windows are GPU-visible according
// to the allocators' OWN records (never content), and whether the bytes the
// site chose differ from the other window's bytes.
//
// Scope: allocation lifetimes are HOST-allocator lifetimes (both allocators are
// bump-only and never free). Reuse inside the game's own pools is invisible
// here. Gate: SR_PHYSWIN_CENSUS=1 (default OFF). It never changes which window
// a site reads; it only counts.
#include "sr_oracle.hpp"
#include <cstdint>
#include <string>

namespace sr {
namespace kernel {
namespace physwin {

// The sub_82467F40 override's heap-routed grants (physical requests the game
// made, satisfied from the main heap). Registered unconditionally - cheap and
// append-only - so the census can be switched on without a rebuild.
void noteHeapRoutedGrant(uint32_t va, uint32_t size);

enum Owner : int { kOwnNone = 0, kOwnPhysRegion = 1, kOwnHeapGrant = 2 };
// Which allocator record holds ALL of [va, va+size).
#if SR_ORACLE
Owner ownerOf(uint32_t va, uint32_t size);
#endif

// iter-458 (Astra 029): WHICH WINDOW OWNS a physical range, from the
// allocators' own records. Full-span and overflow-safe: the WHOLE range must lie
// inside ONE live physical-allocator block (0xA) or ONE registered heap grant
// (0x8). Both owned = explicit ambiguity (callers keep their legacy rule and
// count it - never silently prefer 0xA). Anything else = unknown. With
// SR_PHYS_FREE off the allocator has no live blocks, so 0xA is never returned.
enum OwnerWin : int { kWinUnknown = 0, kWin8 = 8, kWinA = 0xA, kWinBoth = -1 };
#if SR_ORACLE
int ownerWindow(uint32_t phys, uint64_t bytes);
#endif

// iter-460: the PM4 renderer's window choice from the allocators' OWN records,
// independent of the census/resolve gates: 0xA when only a GPU physical grant at
// 0xA0000000|p covers [phys, +bytes); 8 when only a grant at 0x80000000|p does
// (a heap-routed grant, or a physical grant that spilled below 0xA0000000);
// 0 = both or neither (the caller keeps its legacy rule).
int gpuWindow(uint32_t phys, uint64_t bytes);
// iter-461: the range-level answer (0 unless one window owns EVERY page of the
// range and the other overlaps none of it) - see phys_window.cpp.
int gpuWindowRange(uint32_t phys, uint64_t bytes);
bool gpuOwnerWindowEnabled();  // SR_PM4_OWNER_WINDOW (default ON)
// iter-466 cp round 2 (SR_CP_OWN_MEMO): the version of every window answer above (and
// commandBufferWindow's) for [phys, +bytes): the sum of its 64 KB physical pages' mutation
// counters (every registry mutation and command-buffer note bumps the pages it touches).
// An answer computed between two equal versions (sum AND cbSeq) stays exact while a later
// version's sum is the same. ok=false: not cacheable (over 64 pages, or a note half-written).
struct OwnerVer {
  uint64_t sum = 0;
  uint32_t cbSeq = 0;
  bool ok = false;
};
OwnerVer ownerVersion(uint32_t phys, uint64_t bytes);
// iter-460 census: bit0 = a heap-routed grant at 0x80000000|p covers the span,
// bit1 = a physical grant at 0x80000000|p, bit2 = a physical grant at 0xA0000000|p.
int gpuGrantFlags(uint32_t phys, uint32_t bytes);
// iter-460: 0xA / 8 when only one side's live grants touch the 64 KB physical
// page holding `phys`; 0 = both or neither. Lock-free.
int physPageOwner(uint32_t phys);
bool physPageContested(uint32_t phys);  // both sides hold live grants on the page

// iter-460 (SR_CB_WINDOW, default ON): the window each recently executed command
// buffer (ring indirect buffer) was parsed from. Data the game embeds INSIDE a
// command buffer - resolve rectangles, DrawPrimitiveUP vertices - lives where the
// command processor read the buffer; a per-address lookup can disagree on a page
// both windows claim. commandBufferWindow returns 8 / 0xA when [phys, +bytes) lies
// inside one of the last kCbHistory buffers (newest first), else 0. Lock-free.
#if SR_ORACLE
void noteCommandBuffer(uint32_t phys, uint32_t bytes, int window);
int commandBufferWindow(uint32_t phys, uint32_t bytes);
bool commandBufferWindowEnabled();
#else
// No packet buffers are parsed or registered by the native record consumer.
inline int commandBufferWindow(uint32_t, uint32_t) { return 0; }
inline bool commandBufferWindowEnabled() { return false; }
#endif

// iter-458 (Astra 029 B): SR_PHYS_RESOLVE=1 (default OFF; inert unless
// SR_PHYS_FREE=1). The HLE read sites choose the window ownerWindow names when it
// names exactly ONE; for BOTH and unknown they keep their legacy rule. Every
// decision is counted per site, and the bytes an ownership decision selected are
// checked by the site with a content test that never consults ownership
// (resolveValidate) - so "census WRONG = 0" cannot pass on its own, it is true
// by construction for resolved reads.
#if SR_ORACLE
enum ResolveSite : int { kRsIdxIb = 0, kRsIdxVb, kRsS4Vb, kRsTex, kRsRefresh, kRsSites };
bool physResolveEnabled();
// iter-458 (Astra 044): GRANT MODE. SR_PHYS_RESOLVE_GRANTS=1 (default OFF) makes the
// resolver live WITHOUT SR_PHYS_FREE: ownerWindow's 0xA owner becomes "inside a
// registered GPU physical grant" (notePhysGrant) instead of a live physalloc block.
// hwin1 proved the need: every grant-model WRONG read (chose 0x8, sole registered
// owner 0xA) read bytes that never matched Xenia's GPU reads, while the 0xA bytes
// did (full-span verified). With SR_PHYS_FREE=1 the live model takes precedence and
// this gate is inert. BOTH / unknown keep the legacy rule, counted.
bool physResolveGrantMode();
// 0xA or 8 when ownership decides; 0 = the site keeps its legacy rule. Gate off:
// always 0 and nothing is counted.
int resolveOwned(ResolveSite site, uint32_t phys, uint64_t bytes);
void resolveValidate(ResolveSite site, bool ok);
// Astra 030: for a resolved read, the window the site's LEGACY rule would have
// chosen (0 = none readable) and the SAME validation predicate on both. Counted
// as agree / disagree; on disagreement, ok/BAD for each side.
void resolveCompare(ResolveSite site, int selectedWin, int legacyWin, bool selOk,
                    bool legacyOk);
struct ResolveCounts {
  uint64_t a, w8, both, unknown, validOk, validBad;
  uint64_t agree, disagree, dSelOk, dSelBad, dLegOk, dLegBad;
  uint64_t scopeSkip;   // Astra 045: grant mode, out-of-scope site (tex/refresh) -> legacy
};
ResolveCounts resolveCountsForTest(ResolveSite site);

enum Site : int { kIdxIb = 0, kIdxVb, kS4Vb, kSites };

bool censusEnabled();

// `phys` is the consumed span's physical address (window bits are masked
// off), `size` the consumed span, `chose8` whether the site read through
// 0x80000000|phys. Classification:
//   ok      chosen window owned, other not
//   wrong   chosen window NOT owned, other owned  (split: bytes differ/same)
//   both    both owned  - an ambiguous physical address (split: differ/same)
//   none    neither owned - unregistered memory
void census(Site site, uint32_t phys, uint32_t size, bool chose8);

struct Counts {
  uint64_t calls, ok, wrongDiffer, wrongSame, bothDiffer, bothSame, none,
      unreadable;
};
Counts countsForTest(Site site);
void resetForTest();

// ── iter-458 (Astra 042/043): the PRECISE owner model + the payload sampler ──
#endif

// The legacy model above treats the whole guestPhysAlloc bump region as owned,
// which also holds thread stacks (stubs.cpp) - so with SR_PHYS_FREE off almost
// every 0xA|p is "owned" and BOTH mostly means "0x8 is a heap-routed grant".
// The grant model owns only what a genuine GPU physical granter recorded
// (MmAllocatePhysicalMemoryEx, the sub_82467F40 override's guestPhysAlloc path)
// plus the heap-routed grants. Ownership is a registration fact, NOT a claim
// about content. Both models are counted side by side; census() still never
// changes which window a site reads.
void notePhysGrant(uint32_t va, uint32_t size);
Owner ownerOfGrant(uint32_t va, uint32_t size);
// iter-458 texture-path census (Astra 068): the LIVE grants of every registry
// (kinds 1 heap-routed, 2 GPU phys, 3 stack) whose physical interval
// intersects [p, p+span), p physical, at all eight 512 MB VA aliases.
// Exhaustive: the same disjointness-checked lookup as the H-UNIQ census.
struct SpanOwners {
  uint32_t nLive = 0, kindBits = 0, nReleased = 0;
  uint32_t serial[2] = {0, 0};   // the two smallest live serials
  uint64_t setDigest = 0;        // order-independent digest of ALL live serials
  // Astra 069/070: ALIASING, not coverage - live owners (distinct serials)
  // whose physical intervals overlap EACH OTHER inside [p, p+span). Exact for
  // any owner count (a sort-and-sweep, no cap): aliasedPairs = the number of
  // owner intervals that overlap an earlier-starting interval of another
  // serial, so it is >= 1 iff the span is aliased. Two adjacent disjoint
  // grants covering the span give 0.
  uint32_t aliasedPairs = 0;
  uint64_t gen = 0;              // registryGeneration() at the sample
};
SpanOwners ownersOfSpan(uint32_t p, uint32_t span);
// Bumped under the registry's unique lock by EVERY registry mutation, so two
// equal readings bracket an interval in which no owner set changed.
uint64_t registryGeneration();
// The same owners, computed AND consumed under the registry's shared lock: no
// registration or release (each taken under the unique lock) can fall between
// them. fn may take the texture census's lock (order: registry -> census).
void withOwnersOfSpan(uint32_t p, uint32_t span, void (*fn)(const SpanOwners&, void*),
                      void* ctx);
// fn under the registry's shared lock: no registration or release is in flight
// while it runs (the census's flush uses it so no grant event straddles it).
void withRegistryShared(void (*fn)(void*), void* ctx);
// iter-458 H-LIFO (Astra 058): lifetimes. A reclaimed grant is removed from the
// registry; a grant the guest freed but the allocator retained (non-top free)
// is marked RELEASED - still registered, reported apart from live grants.
// Astra 059: guestPhysLifoFree calls these UNDER the allocator lock (report =
// false), so a re-grant of a reclaimed VA can never be registered before the old
// grant is removed; the caller reports afterwards with uniqLifetimeReport.
void forgetPhysGrant(uint32_t va, bool report = true);
// Marks the GPU physical OR heap-routed grant starting at va released; returns
// false (and counts a "release-unregistered" event) when neither holds it.
bool markPhysGrantReleased(uint32_t va, bool report = true);
#if SR_ORACLE
void uniqLifetimeReport(const char* why);
#else
inline void uniqLifetimeReport(const char*) {}
#endif
// iter-463 F1: the LIFO allocator re-grants pages a released (state 1) GPU
// physical grant held. Before it returns (under its lock) it drops every
// RELEASED kind-2 record overlapping [va, va+size), so the caller's
// notePhysGrant never finds a stale owner over the new grant. Live records are
// never touched. Returns the number dropped.
uint32_t forgetReleasedPhysGrantsIn(uint32_t va, uint32_t size);

// ── iter-463 F2: PHYSICAL DOUBLE-BOOK INVARIANT (observe-only, always on) ────
// On the console one physical page backs one allocation. At every registration
// of a heap-routed (kind 1) or GPU physical (kind 2) grant, its physical range
// [va & 0x1FFFFFFF, +size) (split at 512 MB window boundaries) is checked
// against every LIVE kind-1/kind-2 record at all eight window aliases. Each
// overlapping live record counts one double-book and is logged as
// [PHYS_DOUBLE_BOOK] (first 16, then powers of two). It never changes the grant.
// A live record at the new grant's OWN VA counts too (the same VA granted
// twice while the first grant lives; logged "SAME-VA").
struct PhysDoubleBook {
  uint32_t newVa = 0, newSize = 0, newSerial = 0, newLr = 0;
  uint32_t oldVa = 0, oldSize = 0, oldSerial = 0;
  uint8_t newKind = 0, oldKind = 0;
  uint8_t sameVa = 0;
};
uint64_t physDoubleBookCount();         // every overlapping pair, all kind pairs
uint64_t physDoubleBookCountPhysPhys(); // kind 2 vs kind 2 only (the v1_gpuhang class)
PhysDoubleBook physDoubleBookLast();    // the most recent pair
// Called (outside the registry lock, on the registering thread) for the FIRST
// overlapping live record of each registration that double-books; the hook
// may read the registry. nullptr clears.
void setPhysDoubleBookHook(void (*hook)(uint32_t newVa, uint32_t oldVa));

// ── iter-463 F3: ONE window rule for indirect buffers ───────────────────────
// Pure: the guest VA window base (0xA0000000 or 0x80000000) the command
// processor parses an indirect buffer [ibPhys, +bytes) from. Ownership first
// (ownerWindow under SR_PHYS_FREE, else gpuWindow under SR_PM4_ENGINE_OWNER +
// SR_PM4_OWNER_WINDOW); when neither or both windows own it, the SAME default
// Pm4Engine::physToHost applies to every other GPU read (SR_PM4_PHYS_A: 0xA).
// SR_IB_WINDOW_UNIFIED=0 restores the separate legacy 0x80000000 IB default.
// With the owner gates off (SR_PM4_ENGINE_OWNER=0 or SR_PM4_OWNER_WINDOW=0, not
// under SR_PHYS_FREE) ownership is not consulted: 0x80000000, the pre-iter-460
// fetch that A/B arm reproduces, verdict kIbNotConsulted (never counted).
// *verdict (optional): 0xA / 8 = one owner, kWinBoth = contested, 0 = unowned.
// SCOPE: ring-level IBs (executeGuestIndirectBuffer). A NESTED 0x3F/0x37 inside
// an IB resolves through the engine's physToHost (gfx/pm4_engine.cpp) - the
// same 0xA default this rule's contested/unowned case uses - and is not in the
// GPU_HANG_WD IB ring (the engine is shared with the trace replay, which has
// no allocator registry).
#if SR_ORACLE
constexpr int kIbNotConsulted = -2;
uint32_t resolveIbWindow(uint32_t ibPhys, uint32_t bytes, int* verdict = nullptr);
bool ibWindowUnifiedEnabled();
void setIbWindowUnifiedForTest(int mode);   // -1 env, 0 off, 1 on
// Counts one IB decision; contested IBs log [PM4_IB_CONTESTED] and unowned
// ones [PM4_IB_UNOWNED] (first 8, then powers of two).
void noteIbWindow(int verdict, uint32_t ibPhys, uint32_t bytes, uint32_t base);
uint64_t ibContestedCount();
uint64_t ibUnownedCount();

#endif

// Thread stacks carved from the same guestPhysAlloc cursor (stubs.cpp
// SR_STACK_PHYS): census-only registrations (kind 3). They are NOT GPU owners,
// so the grant model above never sees them; H-UNIQ does (Astra 058: every
// grant route).
#if SR_ORACLE
void noteStackGrant(uint32_t va, uint32_t size);
#else
// Stack grants are census-only; native GPU provenance contains kinds 1 and 2.
inline void noteStackGrant(uint32_t, uint32_t) {}
#endif

// ── iter-458 H-UNIQ census (Astra 058): SR_PHYS_UNIQ_CENSUS=1, default OFF,
// observe-only. The GPU address of a grant is va & 0x1FFFFFFF, so every
// registered grant (GPU physical AND heap-routed) is mapped to its PHYSICAL
// interval(s) - split wherever the VA range crosses a 512 MB window boundary
// (0x80000000/0xA0000000/0xC0000000/0xE0000000), where the physical address
// wraps - and all intervals are swept for overlapping pairs. Classes: live-live
// (a uniqueness violation), live-released, released-released. "Wrap" = live
// GPU physical grants whose VA lies below 0xA0000000 (their physical address
// aliases the top of the region).
#if SR_ORACLE
struct UniqSnapshot {
  uint64_t grantsLive = 0, grantsReleased = 0;
  uint64_t pairsLiveLive = 0, pairsLiveReleased = 0, pairsReleasedReleased = 0;
  uint64_t bytesLiveLive = 0;
  uint64_t physGrantsBelowA = 0;     // live GPU physical grants with VA < 0xA0000000
  uint32_t lowestPhysGrantVA = 0;    // 0 = none
  // the first live-live pair found (for the log): kinds 1 heap / 2 phys
  uint32_t exA = 0, exB = 0, exSizeA = 0, exSizeB = 0, exSerA = 0, exSerB = 0;
  uint8_t exKindA = 0, exKindB = 0;
  // Live GPU-physical / stack grants whose physical interval overlaps the
  // primary heap bump's physical interval [heapLo, heapHi) & 0x1FFFFFFF (the
  // heap's CPU allocations are not individually registered; heap-routed grants
  // live INSIDE it and are excluded from this comparison by construction).
  uint64_t heapBulkPairs = 0, heapBulkBytes = 0;
  uint32_t heapLo = 0, heapHi = 0;
  // The guestPhysAlloc bump region's low bound (guestPhysRegion): below
  // 0xA0000000 the region itself has wrapped, whatever route the grant took.
  uint32_t physRegionLo = 0;
  // Astra 059: live-live interval pairs and bytes PER KIND PAIR (index below).
  uint64_t pairsLL[6] = {}, bytesLL[6] = {};
};
// Kind-pair index: 11 12 13 22 23 33 (kinds 1 heap-routed, 2 GPU phys, 3 stack).
int uniqPairIndex(int kindA, int kindB);
UniqSnapshot uniqSnapshot();

// Astra 059: STICKY violation tracking. Snapshots are sampled; these counters
// are updated at EVERY registration (the only moment a live-live overlap can
// form - a release never creates one), so they cover the whole run. The census
// ticker (a host wall-clock thread started with SR_PHYS_UNIQ_CENSUS=1) prints
// them every 2 s and when the SRHT recorder closes as [PHYS_UNIQ_STICKY]: runs
// end in TerminateProcess, so the LAST ticker line is the terminal report and
// covers all but <= 2 s of the run.
struct UniqEx { uint32_t va = 0, size = 0, serial = 0; };
struct UniqSticky {
  uint64_t registrations = 0;
  uint64_t llPairs[6] = {}, llBytes[6] = {};   // live-live interval pairs formed, ever
  UniqEx exNew[6], exOld[6];                   // the first pair formed, per kind pair
  uint64_t physBelowA = 0, stackBelowA = 0;    // grants registered with VA < 0xA0000000
  uint32_t firstBelowAVA = 0;
  uint32_t minPhysRegionLo = 0xFFFFFFFFu;      // lowest bump-region bound at any registration
  uint64_t heapSpanPairs = 0, heapSpanBytes = 0;   // new phys/stack grant vs the heap span, at registration
  uint64_t heapSpanBytesMaxTick = 0;               // the snapshot's heap-span overlap, max over ticks
  // Astra 060: inserts whose VA range overlapped another entry of the SAME
  // registry, per kind (1..3 -> [0..2]). Once nonzero, that registry's overlap
  // lookups fall back from the single-predecessor search to a full scan.
  uint64_t registryOverlaps[3] = {};
};
UniqSticky uniqSticky();
void uniqStickyReport(const char* why);   // one [PHYS_UNIQ_STICKY] line

// Astra 060: the DURABLE terminal record. SR_PHYS_UNIQ_STICKY_FILE=<path>
// (with SR_PHYS_UNIQ_CENSUS=1): the sticky state is serialized and handed to the
// OS at every update, so a TerminateProcess'd run still leaves the state after
// its last completed registration. Layout: header {u32 magic 'UQS1', u32
// version 1, u32 recBytes, u32 0}, then two slots {u64 seq, record, u64 seq};
// a slot is complete iff its two seq words match (tools/uniq_sticky_read.py).
constexpr size_t kUniqStickyRecBytes = 320;
void serializeUniqSticky(const UniqSticky& s, uint8_t* out);   // little-endian, fixed order
UniqSticky deserializeUniqSticky(const uint8_t* in);
// Astra 061: publication is checked and FAIL-CLOSED - the first failed write
// stops publication for good and poisons the header ('UQSX'); a poisoned file
// reads as false with *poisoned = true (never an older "complete" slot).
bool readUniqStickyFile(const char* path, UniqSticky* out, uint64_t* seq, int* completeSlots,
                        bool* poisoned = nullptr);
bool uniqStickyPublicationFailed(uint64_t* failSeq, uint64_t* publishedSeq);
void setUniqStickyFileForTest(const char* path);   // nullptr closes; resets sequence + failure
void tearUniqStickyForTest();                      // writes an INCOMPLETE next slot
void setUniqStickyFailInjectForTest(int mode);     // 1 fail the next slot write; 2 also the poison
void reopenUniqStickyReadOnlyForTest(const char* path);   // keeps seq; real write failures

// Astra 062: the SEAL - positive, acquisition-bound terminal evidence. When the
// live SRHT recorder first closes, the kernel writes "<record>.seal" (checked)
// and freezes publication: {u32 'UQSE', u32 1, u32 nonce, u32 publicationFailed,
// u64 seq, u64 failSeq, u32 recBytes, u32 0, record[recBytes], u64 FNV-1a-64 of
// all preceding}. The nonce is also the record header's 4th word and is logged
// at open. Terminal acceptance (acceptUniqSticky / tools/uniq_sticky_read.py
// --accept) needs a present, checksummed, nonce-matching, failure-free seal
// whose seq and record bytes equal the record file's - anything missing rejects.
struct UniqSeal {
  bool present = false, checksumOk = false;
  uint32_t nonce = 0, failed = 0;
  uint64_t seq = 0, failSeq = 0;
  uint8_t rec[kUniqStickyRecBytes] = {};
};
bool readUniqSeal(const char* sealPath, UniqSeal* out);
uint32_t readUniqStickyNonce(const char* recordPath);   // 0 = unreadable / not a record
// Astra 063: acceptance REQUIRES the expected acquisition nonce - supplied by the
// launcher (SR_PHYS_UNIQ_NONCE), never read back from the output - so a stale
// record+seal left at a reused path cannot pass for this run. 0 = rejected.
bool acceptUniqSticky(const char* recordPath, uint32_t expectedNonce, std::string* why);
void uniqSealForTest(const char* why);
void setUniqStickyNonceForTest(uint32_t nonce);   // 0 = env / random
uint32_t uniqStickyNonceForTest();                // the current acquisition's nonce
void armUniqCloseHookForTest(bool on);            // register the recorder-close seal hook
// The recorder-close hook (H-UNIQ seal + HR1 capture snapshot + texture-census
// flush), armed once by whichever env-enabled census or gate needs it.
void armRecorderCloseHook();
// Astra 073 test seam: called inside the capture endpoint, between the H-UNIQ
// seal and HR1's snapshot, with the registry and HR1 accounting locks held.
void setUniqCloseMidHookForTest(void (*fn)());
// Astra 065: register the harness main stack [kMainStackBase, +kMainStackSize)
// as a kind-3 grant at the census's first registration (env census: always).
void setUniqNoteMainStackForTest(bool on);
bool uniqCensusEnabled();
void setUniqCensusForTest(int mode);   // -1 env, 0 off, 1 on
enum GrantCat : int { kGNone = 0, kGOk = 1, kGWrong = 2, kGBoth = 3, kGCats = 4 };
struct GrantCounts { uint64_t none, ok, wrong, both; };
GrantCounts grantCountsForTest(Site site);

// The calling thread's draw context, attached to every sample. newDraw assigns
// the next draw id (one draw = idx-ib then idx-vb, or one s4-vb).
struct DrawCtx {
  uint64_t presentId = 0;
  uint32_t prim = 0, stride = 0, count = 0, startIndex = 0, minIdx = 0, maxIdx = 0;
  int32_t baseVertex = 0;
  uint8_t idx32 = 0, idxEndian = 0, vbEndian = 0;
};
void setDrawContext(const DrawCtx& c, bool newDraw);

// SR_PHYSWIN_SAMPLE=<file> (default OFF; needs SR_PHYSWIN_CENSUS=1): fixed-size
// binary records (SampleRec below) of BOTH candidate windows for sampled reads -
// the first 2000 per (site, grant category), then 1 in 1024 - with full-span
// FNV-1a-64 hashes taken at sampling time, for offline content matching against
// a Xenia .xtr (tools/physwin_xtr_match.py). Fault-free: every read is
// hostRangeReadable-checked first.
#pragma pack(push, 1)
struct SampleRec {
  char magic[4];              // "PWS1"
  uint8_t site, catLegacy, catGrant, flags;   // flags: 1 chose8, 2 readable8, 4 readableA, 8 hashTruncated,
                                              // 16 a resolver decision applied to THIS read (Astra 044),
                                              // 32 ...and the legacy rule would have read 0x8 (else 0xA)
  uint32_t phys, size, sampledLen;
  uint64_t drawId, presentId, seenInStratum;
  uint64_t hash8, hashA;      // over min(size, 16 MB) of each candidate; 0 if unreadable
  uint32_t prim, stride, count, startIndex, minIdx, maxIdx;
  int32_t baseVertex;
  uint8_t idx32, idxEndian, vbEndian, pad;
  uint8_t bytes8[64], bytesA[64];
};
#pragma pack(pop)
static_assert(sizeof(SampleRec) == 220, "SampleRec layout is the offline contract");
struct SampleStats { uint64_t seen[kSites][kGCats], sampled[kSites][kGCats]; };
SampleStats sampleStatsForTest();
// Test seam: route samples to `path` (nullptr = back to the env gate).
void setSampleFileForTest(const char* path);

// ── iter-458 (Astra 054): THE PER-DRAW WINDOW CAPTURE (PWS2) ────────────────
// SR_PHYSWIN_SAMPLE_WIN=<file> (default OFF; needs SR_PHYSWIN_CENSUS=1): EVERY
// census() read - no stratum limit - is appended to an in-memory ring of the
// last kWinRing records, and the ring is written to <file> ONCE, on the first
// census() call after the HLE trace recorder has been seen OPEN and then
// CLOSED. The file therefore covers the SRHT window (plus the frames that led
// into it) read-for-read, so a draw the SRHT shows as foreign bytes can be
// joined to the read that produced it - by draw-tuple sequence alignment,
// verified by content - rather than by class. Each record carries both
// candidates' first 128 bytes, bounded hashes, and the grant record that
// contains each candidate's START address (kind, [start,end), allocation
// serial shared across both registries). Observe-only: never changes a read.
#pragma pack(push, 1)
struct WinGrant {
  uint32_t start, end, serial;
  uint8_t kind;        // 0 none, 1 heap-routed grant (0x8), 2 GPU physical grant (0xA)
  uint8_t coversSpan;  // 1 = [start,end) contains the WHOLE read span
  uint8_t pad[2];
};
struct WinRec {
  char magic[4];              // "PWS2"
  uint8_t site, catLegacy, catGrant, flags;   // flags as SampleRec
  uint32_t phys, size;
  uint64_t seq;               // global census sequence (every read, all sites)
  uint64_t drawId;            // producer draw serial (setDrawContext newDraw)
  uint64_t presentIdConsumer; // DrawCtx.presentId (render-thread count; lags)
  uint32_t prim, stride, count, startIndex, minIdx, maxIdx;
  int32_t baseVertex;
  uint8_t idx32, idxEndian, vbEndian, pad0;
  WinGrant g8, gA;
  uint64_t hash8, hashA;      // FNV-1a-64 over min(size, 4096) of each candidate
  uint8_t bytes8[128], bytesA[128];
  uint8_t pad1[8];
};
#pragma pack(pop)
static_assert(sizeof(WinRec) == 384, "WinRec layout is the offline contract");
constexpr size_t kWinRing = 131072;
// Test seam: `path` non-null routes the capture there (bypassing the env gate)
// and `traceOpen` replaces the recorder-open probe; nullptr restores both.
void setWinCaptureForTest(const char* path, bool (*traceOpen)());
uint64_t winFlushedForTest();   // records written by the last flush (0 = none yet)
uint64_t fnv1a64ForTest(const uint8_t* p, size_t n);

#else
// Shared allocator callers retain their native work without recorder capture.
inline void armRecorderCloseHook() {}
inline bool uniqCensusEnabled() { return false; }
#endif

}  // namespace physwin
}  // namespace kernel
}  // namespace sr
