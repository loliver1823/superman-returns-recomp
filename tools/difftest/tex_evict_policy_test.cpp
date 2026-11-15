// step 4 G7 (lane D): the SR_TEX_CACHE_EVICT policy (gfx/tex_evict_policy.hpp) - which entries a sweep evicts, by
// age and by the byte budget (LRU), and the fence-retired slot list with its debug check.
#include "gfx/tex_evict_policy.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace TE = sr::gfx::texevict;
namespace {
unsigned passed = 0, failed = 0;
void check(bool ok, const char* name) {
  std::printf("[TEX_EVICT_POLICY_TEST] %s %s\n", ok ? "PASS" : "FAIL", name);
  ok ? ++passed : ++failed;
}
bool has(const std::vector<uint64_t>& v, uint64_t k) {
  for (uint64_t x : v)
    if (x == k) return true;
  return false;
}
}  // namespace

int main() {
  constexpr uint64_t MB = 1ull << 20;
  // (a) age: unused for >= 7200 frames leaves, 7199 stays; no budget pass when under budget.
  {
    const std::vector<TE::Candidate> c = {{1, 1000, 4 * MB}, {2, 1001, 4 * MB}, {3, 8000, 4 * MB}};
    const auto s = TE::select(c, 8200, 7200, 2, 12 * MB, 0);
    check(s.aged.size() == 1 && has(s.aged, 1) && s.agedBytes == 4 * MB && s.budget.empty(),
          "age_rule_evicts_only_entries_unused_for_the_age");
    const auto off = TE::select(c, 8200, 0, 2, 12 * MB, 0);
    check(off.aged.empty() && off.budget.empty(), "age_0_and_budget_0_evict_nothing");
  }
  // (b) budget: over budget -> the oldest go first, down to 9/10 of the budget, never one used within keepFrames.
  {
    std::vector<TE::Candidate> c;
    for (uint64_t k = 1; k <= 10; ++k) c.push_back({k, 100 + k, 10 * MB});  // 100 MB live, key 1 oldest
    const auto s = TE::select(c, 120, 0, 2, 100 * MB, 50 * MB);
    // target = 45 MB: evict until live <= 45 MB -> 6 entries (100 - 60 = 40)
    check(s.budget.size() == 6 && s.budget[0] == 1 && s.budget[5] == 6 && s.budgetBytes == 60 * MB,
          "budget_evicts_lru_first_down_to_nine_tenths");
    const auto keep = TE::select(c, 112, 0, 8, 100 * MB, 50 * MB);  // keys used at >= 105 are within 8 frames
    check(keep.budget.size() == 4 && !has(keep.budget, 5), "budget_never_evicts_an_entry_used_within_keep_frames");
    const auto under = TE::select(c, 120, 0, 2, 50 * MB, 50 * MB);
    check(under.budget.empty(), "at_budget_evicts_nothing");
  }
  // (a) then (b): the aged bytes count against the budget first.
  {
    const std::vector<TE::Candidate> c = {{1, 10, 30 * MB}, {2, 9000, 30 * MB}, {3, 9001, 30 * MB}};
    const auto s = TE::select(c, 9010, 7200, 2, 90 * MB, 64 * MB);
    check(s.aged.size() == 1 && s.budget.empty(), "aged_bytes_count_before_the_budget_pass");
    const auto s2 = TE::select(c, 9010, 7200, 2, 90 * MB, 40 * MB);
    check(s2.aged.size() == 1 && s2.budget.size() == 1 && s2.budget[0] == 2, "budget_after_age_takes_the_oldest_rest");
  }
  // Astra 5d0c2a63 P2 (the budget by resource ownership): an 80 MiB resource with a cold and a hot alias plus a cold
  // own 40 MiB resource, budget 100 MiB, live 120 MiB. The per-alias split "freed" 40 MiB with the cold alias and
  // stopped (120 MiB still live); by resource, the cold 40 MiB one goes first and nothing of the hot resource does.
  {
    const std::vector<TE::Candidate> c = {{1, 10, 80 * MB, 77, 1}, {2, 990, 80 * MB, 77, 2}, {3, 20, 40 * MB, 0, 3}};
    const auto s = TE::select(c, 1000, 0, 2, 120 * MB, 100 * MB);
    check(s.budget.size() == 1 && s.budget[0] == 3 && s.budgetBytes == 40 * MB,
          "budget_by_resource_takes_the_cold_own_resource_not_one_alias_of_a_hot_one");
    const auto s2 = TE::select(c, 1000, 0, 2, 120 * MB, 30 * MB);  // target 27 MiB: both resources must go
    check(s2.budget.size() == 3 && has(s2.budget, 1) && has(s2.budget, 2) && s2.budgetBytes == 120 * MB,
          "budget_takes_a_shared_resource_with_all_its_aliases");
    const auto aged = TE::select(c, 1000, 900, 2, 120 * MB, 0);  // age: only the cold alias + the own one
    check(aged.aged.size() == 2 && has(aged.aged, 1) && has(aged.aged, 3) && aged.agedBytes == 40 * MB,
          "an_aged_alias_frees_nothing_while_another_alias_lives");
  }
  // maxN: a sweep evicts at most maxN and says so (the caller sweeps again next frame).
  {
    std::vector<TE::Candidate> c;
    for (uint64_t k = 1; k <= 10; ++k) c.push_back({k, 1, MB});
    const auto s = TE::select(c, 10000, 100, 2, 10 * MB, 0, 4);
    check(s.aged.size() == 4 && s.capped, "sweep_cap_limits_the_victims_and_reports_capped");
  }
  // Astra 5d0c2a63 P1: slot exhaustion never takes an entry bound in the current frame nor a slot the draw being
  // assembled selected; none left = transient (the caller caches nothing).
  {
    const std::vector<TE::Candidate> c = {{1, 50, 0, 0, 11}, {2, 40, 0, 0, 12}, {3, 100, 0, 0, 13}};
    uint64_t v = 0;
    const uint32_t none[1] = {0};
    check(TE::exhaustVictim(c, 100, none, 0, &v) && v == 2, "exhaust_takes_the_lru_entry");
    const uint32_t pin12[1] = {12};
    check(TE::exhaustVictim(c, 100, pin12, 1, &v) && v == 1, "exhaust_never_takes_a_slot_the_draw_pinned");
    const uint32_t pinBoth[2] = {11, 12};
    check(!TE::exhaustVictim(c, 100, pinBoth, 2, &v), "exhaust_never_takes_an_entry_bound_this_frame_transient");
    check(!TE::exhaustVictim({}, 100, none, 0, &v), "exhaust_with_nothing_is_transient");
  }
  // fix3 (Astra 0a384c24 P2): a resource with 513 aliases under a 512 cap goes alias by alias - no stall.
  {
    std::vector<TE::Candidate> c;
    for (uint64_t k = 1; k <= 513; ++k) c.push_back({k, 10, 64 * MB, 99, uint32_t(k)});
    const auto s = TE::select(c, 1000, 0, 2, 64 * MB, 32 * MB, 512);
    check(s.budget.size() == 512 && s.capped && s.budgetBytes == 0,
          "cap_takes_512_of_513_aliases_and_counts_no_bytes_yet");
    std::vector<TE::Candidate> rest;
    for (const auto& e : c)
      if (!has(s.budget, e.key)) rest.push_back(e);
    const auto s2 = TE::select(rest, 1001, 0, 2, 64 * MB, 32 * MB, 512);
    check(rest.size() == 1 && s2.budget.size() == 1 && s2.budgetBytes == 64 * MB && !s2.capped,
          "the_next_sweep_takes_the_last_alias_and_frees_the_bytes");
  }
  // fix3 (Astra 0a384c24 P2): a transient exhaustion retries only when the pool's state changed.
  {
    TE::RetryGate g;
    const uint64_t a = TE::poolState({100, 90, 0, 5, 8192, 4000}), b = TE::poolState({100, 91, 0, 5, 8192, 4000});
    check(g.shouldTry(a), "retry_gate_tries_first");
    g.failed(a);
    check(!g.shouldTry(a), "retry_gate_suppresses_a_retry_in_the_same_state");
    check(a != b && g.shouldTry(b), "retry_gate_retries_after_a_fence_completion");
    g.succeeded();
    check(g.shouldTry(a), "retry_gate_clears_on_success");
  }
  // fix3 (Astra 0a384c24 P2, the movie planes): no slot = 0 and nothing changed (the caller stays retryable).
  {
    std::vector<uint32_t> freeSlots;
    uint32_t next = 8192;
    check(TE::allocSlot(freeSlots, next, 8192, true) == 0 && next == 8192, "no_slot_returns_0_and_changes_nothing");
    freeSlots.push_back(7);
    check(TE::allocSlot(freeSlots, next, 8192, false) == 0 && freeSlots.size() == 1,
          "gate_off_never_takes_the_free_list");
    check(TE::allocSlot(freeSlots, next, 8192, true) == 7 && freeSlots.empty(), "a_retired_slot_is_reused_first");
    next = 100;
    check(TE::allocSlot(freeSlots, next, 8192, true) == 100 && next == 101, "then_a_fresh_slot");
  }
  // The P1 rule under the test knobs (age 1, keep 0): an entry bound in the current frame is never a victim.
  {
    const std::vector<TE::Candidate> c = {{1, 99, 10 * MB}, {2, 100, 10 * MB}};
    const auto s = TE::select(c, 100, 1, 0, 20 * MB, 1 * MB);
    check(s.aged.size() == 1 && s.aged[0] == 1 && s.budget.empty(),
          "test_knobs_never_evict_an_entry_bound_in_the_current_frame");
  }
  // SlotRetire: nothing leaves before its fence; at most maxN per drain, oldest first.
  {
    TE::SlotRetire<std::string> r;
    r.retire(5, 10, "a");
    r.retire(6, 11, "b");
    r.retire(7, 12, "c");
    std::vector<uint32_t> freed;
    auto rel = [&](auto& e) { freed.push_back(e.slot); };
    auto noCheck = [](uint32_t) { return uint64_t(0); };
    check(r.drain(9, 64, noCheck, rel) == 0 && r.pending() == 3, "retire_holds_until_the_fence");
    check(r.drain(12, 2, noCheck, rel) == 2 && freed.size() == 2 && freed[0] == 5 && freed[1] == 6,
          "retire_drains_oldest_first_and_at_most_maxN");
    check(r.drain(12, 64, noCheck, rel) == 1 && r.pending() == 0, "retire_drains_the_rest_later");
  }
  // The debug check: a slot bound after the fence it waits for is held back (counted), released once passed.
  {
    TE::SlotRetire<int> r;
    r.retire(9, 10, 1);
    uint64_t lastUse = 15;  // a bind of slot 9 at frame fence 15, after its eviction at 10
    std::vector<uint32_t> freed;
    auto rel = [&](auto& e) { freed.push_back(e.slot); };
    auto lu = [&](uint32_t) { return lastUse; };
    check(r.drain(12, 64, lu, rel) == 0 && r.violations() == 1 && r.pending() == 1,
          "debug_check_holds_a_slot_bound_after_its_retire_fence");
    check(r.drain(14, 64, lu, rel) == 0 && r.pending() == 1, "debug_check_waits_for_the_late_use");
    check(r.drain(15, 64, lu, rel) == 1 && freed.size() == 1 && freed[0] == 9,
          "debug_check_releases_once_the_late_use_passed");
  }
  std::printf("[TEX_EVICT_POLICY_TEST] %u passed, %u failed\n", passed, failed);
  return failed ? 1 : 0;
}
