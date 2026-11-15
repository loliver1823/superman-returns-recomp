#pragma once
// mem.resources: the frozen-pass per-present A/B of the native resource route against the page model (lane D
// resproof, 2026-10-17; ledger `mem.resources`, pixels guard `frozen-ab`; docs/RELEASE_COMPOSITION.md 9.5 step 2b).
//
// SR_MEM_RESOURCES=1 SR_AB_TOGGLE=SR_MEM_RESOURCES SR_AB_PRESENT=1 (dev / Oracle build only): DUAL mode. Both
// bookkeepings run in every present, and only the DECIDER alternates:
//   - the registry (gfx/resource_memory) hears every note in every present - resourceMode() keeps its meaning
//     ("the registry is maintained"), so every note site is unchanged;
//   - the page model keeps its own state in every present too: the write watch stays enabled (gww::enabled()), the
//     GPU-write rebaseline runs after the registry's GPU note, the rebaseline's unlocked order stays the model's;
//   - resourceDecides() names who decides on THIS thread now: the native route (true) or the model (false). The CP
//     latches the arm of each frame SET at its first drain (pm4ShmBeginSet -> resourceAbBeginSet); a set carries its
//     arm to the render thread (Pm4Frame::resArm -> resourceAbEnter). With SR_AB_PRESENT=1 the arm alternates per
//     PRESENTING set, strictly: the set after a presented native set is model and vice versa (resourceAbPresented, the
//     CP's pm4PresentFrame); partial (Z-pass) sets keep their present's arm. So consecutive presents - consecutive frozen
//     dumps - come from alternate deciders, which is what freeze_diff --exact --ab-control compares (p->p+1 the A/B,
//     p->p+2 the same-arm control). Without SR_AB_PRESENT=1 the arm is the in-roll pace-window mask (sr::abt::on) read
//     at each set's first drain. The guest-side mask that [AB_PRESENT] logs at VdSwap is NOT the arm (the decisions run
//     on the CP / render thread, which lag the guest): the arm's witness is [MEM_RES_AB] (below).
// Cross-arm consistency (what makes alternating safe; difftest --resource-memory-d3d12-test --ab proves each rule):
//   INV-M  after a NATIVE upload of page p from window W, the model's state must still describe the arena: the copied
//          page's hash equals the model's stored hash for W (and the model's last upload was from W, and its 0x8 hash is
//          not the zero page's - the legacy branch served 0xA there) - else the model FORGETS p (its stored hashes become
//          a sentinel no page hashes to, its protection-generation freshness 0, and the write watch releases p's block so
//          neither the clean keep nor the range cache can keep p): its next validation uploads.
//   INV-R  after a MODEL upload of page p from window W': a page whose GPU write already EXECUTED (the model's own
//          clobber rule fired: m_shmGpuWritten) and that the registry holds as GPU data (gpuGen >= cpuGen) was just
//          overwritten with CPU bytes - cross_clobber (counted; the GPU's data cannot be restored); a page the registry
//          holds as GPU data whose write is still PENDING (announced at its drain, not executed: the model preserving
//          the CPU bytes around a partial resolve, as the native arm's preparation does) - preserve; else a window the
//          registry would not serve revokes the registry's upload record (its next decision uploads) - cross_window; else
//          an upload with no pending registry note is cross_unannounced (the census's `missed` class: the arena now holds
//          bytes newer than any note - the registry's keeps in the next native present show them). A page INV-M forgot
//          is re-established without a count only when the model copies the very bytes the native route captured (the
//          same hash) - reestablished; other bytes are a repair like any other (cross_unannounced).
//   STAMPS the validation stamps are per arm: in dual mode the native route stamps its own array (m_resEpochOf), the
//          model m_shmEpochOf, so a later set of the OTHER arm (the CP's next set while the render thread still renders
//          this one) can never satisfy this set's render-side request (Astra review1 P1-1).
//   EMPTY  a drain that consumed no command outside an open set is inert (no arm consulted: the boot's drains before the
//          first set, the drains between a present and the next set); a present no drain began a set for latches its own
//          arm (resourceAbPresentingSet), so strict alternation holds for empty presents too (Astra review1 P2-6).
//   GPU    a resolve's GPU write is noted to the registry at its drain in BOTH arms (the native arm's preservation does it;
//          the model arm notes the same generation without preserving) and again at execution; the model rebaselines in
//          both arms.
//   PRESENT the texture clean marks are invalidated by the union of the registry's dirty blocks and the write watch's
//          released blocks (over-invalidation re-checks a bind under its own arm's key; it never decides).
// The stated limit: the arena is SHARED, so a per-present A/B cannot show PERSISTENT native staleness the model repaired
// one present earlier (cross_unannounced counts every such repair); that class is the checker's (boundaries_complete).
//
// Witness ([MEM_RES_AB], app/log.cpp kMilestoneTags): the decisions by who decided x the thread's latched arm. In a model
// set no native decision may run and in a native set no model decision (native_in_model / model_in_native = 0), each arm
// decides > 0 pages, no decision runs on an unlatched thread (unlatched = 0), and every pair of consecutive dumped frozen
// presents has alternate arms (alt_break = 0; `[MEM_RES_AB] present=<n> arm=N|M` per dumped present, joined to
// [SIM_FREEZE_DUMP] present=<n> by tools/perf/mem_res_ab_check.py). A no-op toggle (resourceDecides ignoring the arm)
// fails native_in_model; an always-model toggle fails model_in_native. Periodic totals every 600 presented sets and the
// graceful quit's terminal family `resource_ab`.
//
// SR_ORACLE=0 (the release configuration): the native route always decides and none of this exists.
#include <cstdint>

#include "../kernel/sr_oracle.hpp"

namespace sr::gfx::memres {

enum class AbWho : uint8_t { kNative = 0, kModel = 1 };
enum class AbEvent : uint8_t {
  kNativeUploadPages = 0,  // pages the native route copied into the arena
  kModelUploadPages,       // pages the model copied into the arena
  kModelForgot,            // INV-M: a native upload left the model's state describing other bytes - forgotten
  kModelAdopted,           // INV-M: a native upload copied exactly what the model's state describes - kept
  kCrossWindow,            // INV-R: the model uploaded from a window the registry would not serve - revoked
  kCrossClobber,           // INV-R: the model overwrote a page the registry holds as GPU data
  kCrossUnannounced,       // INV-R: the model uploaded a page the registry had no pending note for
  kModelReadbackPages,     // the model arm's host readers wrote GPU data back into guest RAM (pm4ShmReadback)
  kGpuNoteModelArm,        // a resolve's GPU write noted at its drain in a model set (no native preservation)
  kModelPreserve,          // INV-R: the model uploaded CPU bytes around a resolve the registry holds as pending GPU data
  kModelReestablished,     // INV-R: the model re-established a page INV-M forgot with the bytes the native route captured
  kCount
};

// The witness totals (the [MEM_RES_AB] line's fields; tools/difftest's --ab fixture reads them directly).
struct AbTotals {
  uint64_t setsNative = 0, setsModel = 0, decNative = 0, decModel = 0, nativeInModel = 0, modelInNative = 0,
           unlatched = 0, decUnlatched = 0, altPairs = 0, altBreak = 0, emptySets = 0;
  uint64_t event[uint32_t(AbEvent::kCount)] = {};
};

#if SR_ORACLE
// Latched: SR_MEM_RESOURCES=1 and SR_AB_TOGGLE lists SR_MEM_RESOURCES.
bool resourceAbDual() noexcept;
AbTotals resourceAbTotals() noexcept;
// The native route decides on this thread now. Without the toggle: resourceMode().
bool resourceDecides() noexcept;
// CP: a frame set's first drain latches its arm on this thread.
void resourceAbBeginSet() noexcept;
// CP: the set that just went to present (submitted to the render thread, or rendered in place) was a presenting set.
void resourceAbPresented() noexcept;
// CP, pm4PresentFrame after the set's last drain: the presenting set's arm. A present no drain began a set for since the
// last present (no new command) latches its own arm here (counted `empty`); a Z-pass partial set's present keeps it.
int8_t resourceAbPresentingSet() noexcept;
// This thread's latched arm: -1 none, 0 model, 1 native (the CP stores it in the frame it submits).
int8_t resourceAbArm() noexcept;
// The render thread: the taken frame's arm.
void resourceAbEnter(int8_t arm) noexcept;
// A decision of `pages` pages by `who` on this thread.
void resourceAbDecision(AbWho who, uint32_t pages) noexcept;
void resourceAbCount(AbEvent event, uint64_t n = 1) noexcept;
// The render thread dumped frozen present `presentNo` (SIM_FREEZE_DUMP's present=): its arm.
void resourceAbDumped(uint64_t presentNo) noexcept;
// [MEM_RES_AB] <why> totals (periodic; the terminal family).
void resourceAbReport(const char* why) noexcept;
#else
inline bool resourceAbDual() noexcept { return false; }
inline AbTotals resourceAbTotals() noexcept { return {}; }
inline bool resourceDecides() noexcept { return true; }
inline void resourceAbBeginSet() noexcept {}
inline void resourceAbPresented() noexcept {}
inline int8_t resourceAbPresentingSet() noexcept { return -1; }
inline int8_t resourceAbArm() noexcept { return -1; }
inline void resourceAbEnter(int8_t) noexcept {}
inline void resourceAbDecision(AbWho, uint32_t) noexcept {}
inline void resourceAbCount(AbEvent, uint64_t = 1) noexcept {}
inline void resourceAbDumped(uint64_t) noexcept {}
inline void resourceAbReport(const char*) noexcept {}
#endif

}  // namespace sr::gfx::memres
