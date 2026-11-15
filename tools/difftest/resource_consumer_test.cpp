// Pure production checker core. No guest arena, game, GPU, or environment is
// required. The backend WARP fixture separately proves copy/draw correspondence.
#include "../../gfx/resource_memory_check.hpp"
#include "../../gfx/resource_census.hpp"
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace sr::gfx::memres;
struct Test {
  unsigned pass = 0, fail = 0;
  void require(bool good, const char* name) {
    std::printf("[RESOURCE_CONSUMER_TEST] %s %s\n", good ? "PASS" : "FAIL", name);
    good ? ++pass : ++fail;
  }
};
ConsumerWitness witness(uint32_t page = 0x17156u) {
  ConsumerWitness w;
  w.page = page; w.cpu = 41; w.window = 10; w.ownerSum = 7;
  w.ownerSeq = 9; w.grantGeneration = 11; w.grantDigest = 0x123456789ABCDEFull;
  w.grantCount = 1; w.grantSerial[0] = 13; w.known = true;
  return w;
}
FetchAnalysis fetch(uint32_t page, uint32_t offset = 0x600, uint32_t length = 4) {
  FetchAnalysis a;
  a.complete = true; a.drawId = 1; a.vsHash = 2;
  FetchUse use{}; use.instruction = 4; use.slot = 0; use.cls = 0;
  use.phys = (page << 12) + offset; use.len = length;
  a.uses.push_back(use);
  return a;
}
std::shared_ptr<const ConsumerCheck> ticket(ConsumerStore& store, const uint8_t* bytes,
    const ConsumerWitness& before, const ConsumerWitness& after, bool stable) {
  auto result = store.keep(bytes, before, after, stable);
  if (result && !store.bind(result, 1, 2)) throw std::runtime_error("fixture draw binding failed");
  return result;
}
void membership(Test& t) {
  ConsumerStore store(8ull << 20);
  auto w = witness();
  std::array<uint8_t, 4096> bytes{};
  auto base = store.capture(bytes.data(), w);
  t.require(base && base->bytes[0x218] == 0, "real_capture_baseline");
  if (!base) return;
  bytes[0x218] = 1;
  auto excluded = ticket(store, bytes.data(), w, w, true);
  t.require(excluded && excluded->changedWords == 1, "header_change_observed");
  auto result = store.validate(excluded, fetch(w.page), base, w);
  t.require(result.status == ConsumerStatus::Disjoint && result.matchedWords == 0,
            "current_draw_excludes_header");
  t.require(store.stats().pending == 1 && store.stats().disjoint == 0,
            "analysis_is_not_recorded_draw");
  t.require(store.recorded(excluded, 1) && store.stats().disjoint == 1, "recorded_draw_finalizes");
  t.require(!store.recorded(excluded, 1), "duplicate_recording_refused");

  auto later = ticket(store, bytes.data(), w, w, true);
  result = store.validate(later, fetch(w.page, 0x218), base, w);
  t.require(result.status == ConsumerStatus::Stale && result.changedWords == 1 && result.matchedWords == 1,
            "disjoint_does_not_rebase_later_consumer");
  t.require(base->bytes[0x218] == 0, "original_baseline_immutable");

  bytes[0x600] = 2;
  auto both = ticket(store, bytes.data(), w, w, true);
  result = store.validate(both, fetch(w.page), base, w);
  t.require(result.status == ConsumerStatus::Stale && result.changedWords == 2 && result.matchedWords == 1,
            "all_changed_dwords_not_only_first");
  auto subword = ticket(store, bytes.data(), w, w, true);
  result = store.validate(subword, fetch(w.page, 0x21B, 1), base, w);
  t.require(result.status == ConsumerStatus::Stale, "partial_dword_conservative_intersection");

  bytes.fill(0);
  bytes[255] = 1; bytes[256] = 1; bytes[4095] = 1;
  auto boundaries = ticket(store, bytes.data(), w, w, true);
  auto spans = fetch(w.page, 252, 8);
  auto tail = fetch(w.page, 4095, 1);
  spans.uses.push_back(tail.uses.front());
  result = store.validate(boundaries, spans, base, w);
  t.require(result.status == ConsumerStatus::Stale && result.matchedWords == 3,
            "mask_group_and_page_end_boundaries");
  t.require(store.stats().pending == 0, "membership_all_tickets_finalized");
}
void queued(Test& t) {
  ConsumerStore store(8ull << 20);
  auto a = witness();
  std::array<uint8_t, 4096> bytes{};
  auto baseA = store.capture(bytes.data(), a);
  bytes[8] = 1;
  auto oldA = ticket(store, bytes.data(), a, a, true);
  auto wrongCopy = ticket(store, bytes.data(), a, a, true);
  auto b = a; b.cpu += 1; b.window = 8; b.ownerSeq += 1; b.grantGeneration += 1;
  auto baseB = store.capture(bytes.data(), b);
  auto result = store.validate(oldA, fetch(a.page), baseA, b);
  t.require(result.status == ConsumerStatus::Disjoint && store.recorded(oldA, 1),
            "queued_old_copy_survives_future_cpu_capture");
  result = store.validate(wrongCopy, fetch(a.page), baseB, b);
  t.require(result.status == ConsumerStatus::Unknown, "new_cp_capture_not_old_gpu_recorded_copy");
  auto newB = ticket(store, bytes.data(), b, b, true);
  result = store.validate(newB, FetchAnalysis{}, baseB, b);
  t.require(result.status == ConsumerStatus::Same && store.recorded(newB, 1),
            "equal_whole_page_needs_no_fetch_exemption");

  bytes[8] = 2;
  auto globalChange = ticket(store, bytes.data(), b, b, true);
  auto unrelated = b; unrelated.ownerSeq += 50; unrelated.grantGeneration += 50;
  result = store.validate(globalChange, fetch(b.page), baseB, unrelated);
  t.require(result.status == ConsumerStatus::Disjoint && store.recorded(globalChange, 1),
            "unrelated_global_registry_change_not_local_reuse");
  auto localChange = ticket(store, bytes.data(), b, b, true);
  auto reused = b; ++reused.ownerSum;
  result = store.validate(localChange, fetch(b.page), baseB, reused);
  t.require(result.status == ConsumerStatus::Unknown, "local_owner_mutation_refused");
  auto grantChange = ticket(store, bytes.data(), b, b, true);
  reused = b; ++reused.grantSerial[0];
  result = store.validate(grantChange, fetch(b.page), baseB, reused);
  t.require(result.status == ConsumerStatus::Unknown, "same_page_grant_reuse_refused");
  t.require(store.stats().pending == 0, "queued_all_tickets_finalized");
}
void unknowns(Test& t) {
  ConsumerStore store(8ull << 20);
  auto w = witness();
  std::array<uint8_t, 4096> bytes{};
  t.require(!ticket(store, bytes.data(), w, w, true), "missing_baseline_unknown");
  auto base = store.capture(bytes.data(), w);
  bytes[8] = 1;
  auto after = w; ++after.ownerSeq;
  t.require(!ticket(store, bytes.data(), w, after, true), "admission_global_version_race_unknown");
  after = w; ++after.cpu;
  t.require(!ticket(store, bytes.data(), w, after, true), "admission_cpu_race_unknown");
  after = w; after.window = 8;
  t.require(!ticket(store, bytes.data(), w, after, true), "admission_alias_race_unknown");
  t.require(!ticket(store, bytes.data(), w, w, false), "source_byte_race_unknown");
  after = w; after.known = false;
  t.require(!ticket(store, bytes.data(), after, after, true), "unknown_owner_not_admitted");

  auto run = [&](FetchAnalysis analysis, const char* name) {
    auto check = ticket(store, bytes.data(), w, w, true);
    t.require(store.validate(check, analysis, base, w).status == ConsumerStatus::Unknown, name);
  };
  auto bad = fetch(w.page); bad.complete = false; run(bad, "incomplete_fetch_unknown");
  bad = fetch(w.page); bad.drawId = 0; run(bad, "missing_draw_identity_unknown");
  bad = fetch(w.page); bad.drawId = 2; run(bad, "wrong_draw_cannot_rescue_ticket");
  bad = fetch(w.page); bad.vsHash = 3; run(bad, "wrong_shader_cannot_rescue_ticket");
  bad = fetch(w.page); bad.uses.clear(); run(bad, "empty_changed_consumer_unknown");
  bad = fetch(w.page); bad.uses[0].len = 0; run(bad, "zero_length_fetch_unknown");
  bad = fetch(w.page); bad.uses[0].phys = UINT32_MAX; bad.uses[0].len = 8;
  run(bad, "wrapped_fetch_unknown");
  bad = fetch(w.page); bad.uses[0].phys = 0x1FFFFFFF; bad.uses[0].len = 2;
  run(bad, "physical_end_fetch_unknown");
  bad = fetch(w.page);
  auto invalidTail = bad.uses.front(); invalidTail.phys = UINT32_MAX; invalidTail.len = 8;
  bad.uses.push_back(invalidTail);
  run(bad, "valid_prefix_cannot_hide_invalid_tail");
  bad = fetch(w.page); bad.uses.resize(65537, bad.uses.front()); run(bad, "analysis_budget_unknown");
  auto duplicate = ticket(store, bytes.data(), w, w, true);
  t.require(store.validate(duplicate, fetch(w.page), base, w).passed(), "duplicate_first_analysis_valid");
  t.require(store.validate(duplicate, fetch(w.page), base, w).status == ConsumerStatus::Unknown,
            "duplicate_validation_unknown");
  duplicate.reset();
  t.require(store.stats().abandoned == 1 && store.stats().pending == 0,
            "validated_but_unrecorded_abandoned");
  { auto abandoned = ticket(store, bytes.data(), w, w, true); }
  t.require(store.stats().abandoned == 2 && store.stats().pending == 0, "unvalidated_abandoned");
  auto unvalidated = ticket(store, bytes.data(), w, w, true);
  t.require(!store.recorded(unvalidated, 1), "record_before_validation_refused");
  unvalidated.reset();
  t.require(store.stats().abandoned == 3, "early_record_does_not_hide_abandonment");
  auto unbound = store.keep(bytes.data(), w, w, true);
  t.require(store.validate(unbound, fetch(w.page), base, w).status == ConsumerStatus::Unknown,
            "unbound_draw_unknown");
  auto rebound = ticket(store, bytes.data(), w, w, true);
  t.require(!store.bind(rebound, 2, 3), "immutable_draw_binding");
  t.require(store.validate(rebound, fetch(w.page), base, w).passed(), "original_draw_still_bound");
  t.require(!store.recorded(rebound, 2), "wrong_recorded_draw_unknown");
  t.require(store.recorded(rebound, 1), "only_bound_recorded_draw_finalizes");

  after = w; after.known = false;
  t.require(!store.capture(bytes.data(), after) && !ticket(store, bytes.data(), w, w, true),
            "unknown_capture_invalidates_latest_entry");
  auto invalid = w; invalid.page = 0x20000;
  t.require(!store.capture(bytes.data(), invalid), "out_of_range_page_unknown");
  invalid = w; invalid.grantCount = 3;
  t.require(!store.capture(bytes.data(), invalid), "unsupported_grant_count_unknown");
  t.require(store.stats().pending == 0, "unknowns_all_tickets_closed");
}
void budgets(Test& t) {
  bool threw = false;
  try { ConsumerStore invalid(1); } catch (const std::invalid_argument&) { threw = true; }
  t.require(threw, "fixed_index_budget_refused");
  threw = false;
  try { ConsumerStore invalid(257ull << 20); } catch (const std::invalid_argument&) { threw = true; }
  t.require(threw, "maximum_budget_enforced");
  ConsumerStore store(5ull << 20);
  auto w = witness();
  std::array<uint8_t, 4096> bytes{};
  auto base = store.capture(bytes.data(), w);
  std::vector<std::shared_ptr<const ConsumerCheck>> pending;
  const auto cap = store.stats().capacity;
  for (uint64_t i = 0; i < cap + 1; ++i) {
    auto check = ticket(store, bytes.data(), w, w, true);
    if (!check) break;
    pending.push_back(std::move(check));
  }
  auto stats = store.stats();
  t.require(stats.capped > 0 && stats.live == cap && stats.peak <= stats.capacity && stats.reserved <= stats.budget,
            "pool_capacity_real_limit_including_control_blocks");
  t.require(!ticket(store, bytes.data(), w, w, true), "cap_never_accepts_untracked_keep");
  pending.clear();
  t.require(store.stats().pending == 0 && store.stats().abandoned > 0, "budgeted_tickets_abandon_explicitly");
  auto retained = ticket(store, bytes.data(), w, w, true);
  t.require(retained && retained->baseline == base, "no_baseline_eviction_to_admit_tickets");
  retained.reset();

  std::shared_ptr<const ConsumerCheck> escaped;
  std::shared_ptr<const ConsumerBaseline> escapedBase;
  {
    ConsumerStore temporary(5ull << 20);
    escapedBase = temporary.capture(bytes.data(), w);
    escaped = ticket(temporary, bytes.data(), w, w, true);
  }
  t.require(escaped && escaped->baseline == escapedBase && escapedBase->bytes[0] == 0,
            "queued_handles_keep_pool_alive_after_store");
  escaped.reset(); escapedBase.reset();
  ConsumerStore captureStore(8ull << 20);
  std::vector<std::shared_ptr<const ConsumerFetchLease>> captures;
  for (unsigned i = 0; i < 256; ++i) captures.push_back(captureStore.acquireFetch());
  t.require(captures.back() && !captureStore.acquireFetch() && captureStore.stats().fetchLive == 256 &&
            captureStore.stats().fetchPeak == 256 && captureStore.stats().fetchCapped == 1 &&
            captureStore.stats().fetchBudget == (16ull << 20), "separate_draw_capture_budget_enforced");
  captures.clear();
  t.require(captureStore.stats().fetchLive == 0 && bool(captureStore.acquireFetch()), "capture_budget_released_by_lifetime");
}
}
int resource_consumer_test_main(int, char**) {
  Test t;
  try { membership(t); queued(t); unknowns(t); budgets(t); }
  catch (const std::exception& e) { std::printf("[RESOURCE_CONSUMER_TEST] EXCEPTION %s\n", e.what()); ++t.fail; }
  std::printf("[RESOURCE_CONSUMER_TEST] SUMMARY pass=%u fail=%u\n", t.pass, t.fail);
  return t.fail ? 1 : 0;
}
