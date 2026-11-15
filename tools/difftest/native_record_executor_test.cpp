// Standalone framing/order checks. The real-engine differential is separate:
// this target intentionally links no PM4 engine, guest code or renderer.
#include "gfx/native_record_executor.hpp"

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
namespace nf = sr::native::flush;
using sr::gfx::NativeRecordHandlers;
using sr::gfx::NativeRecordStats;
using sr::gfx::NativeRenderContext;
using sr::gfx::executeNativeRecords;

int passed = 0, failed = 0;
void check(bool ok, const std::string& what) {
  std::printf("%s%s\n", ok ? "  ok  " : "FAIL: ", what.c_str());
  ok ? ++passed : ++failed;
}

struct Context final : NativeRenderContext {
  std::vector<std::vector<uint32_t>> calls;
  bool predicate = true, accept = true;
  int failAfter = -1;
  uint32_t skipped = 0;
  mutable uint64_t timestamp = 100;
  bool predicatePasses() const override { return predicate; }
  void predicatedSkipped() override { ++skipped; }
  bool stateRun(uint32_t first, std::span<const uint32_t> values, bool dedup) override {
    if (!accept || int(calls.size()) == failAfter) return false;
    calls.push_back({0x100u, first, uint32_t(dedup)});
    calls.back().insert(calls.back().end(), values.begin(), values.end());
    return true;
  }
  bool draw(bool predicated, uint32_t initiator, uint32_t base, uint32_t size) override {
    if (!accept || int(calls.size()) == failAfter) return false;
    calls.push_back({0x200u, uint32_t(predicated), initiator, base, size});
    return true;
  }
  bool command(nf::RecKind kind, uint32_t n, std::span<const uint32_t> values) override {
    if (!accept || int(calls.size()) == failAfter) return false;
    calls.push_back({0x300u, kind, n});
    calls.back().insert(calls.back().end(), values.begin(), values.end());
    if (kind == nf::kRkBinReg) predicate = values[0] != 0;
    return true;
  }
  uint64_t drawTimestamp() const override { timestamp += 9; return timestamp; }
};

bool opaque(nf::RecKind kind, std::span<const uint32_t> values, NativeRenderContext& context) {
  return values.size() == 2 && context.command(kind, uint32_t(values.size()), values);
}

void reject(std::vector<uint32_t> words, const std::string& what, bool predicate = true) {
  Context context;
  context.predicate = predicate;
  NativeRecordStats stats;
  NativeRecordHandlers handlers;
  handlers.set(nf::kRkResolve, opaque);
  const bool ok = executeNativeRecords(words, context, handlers, stats, false);
  check(!ok && stats.bad == 1 && context.calls.empty() && !context.skipped, what);
}
}  // namespace

int main() {
  NativeRecordHandlers handlers;
  check(!handlers.set(nf::kRkDraw, opaque) && !handlers.set(nf::kRkPm4, opaque) &&
            !handlers.set(127u, opaque) && !handlers.set(UINT32_MAX, opaque),
        "handler registry excludes builtins, legacy packets and unknown kinds");
  check(handlers.set(nf::kRkResolve, opaque) && handlers.get(nf::kRkResolve) == opaque &&
            !handlers.get(UINT32_MAX), "opaque handlers use the independent native context");

  Context context;
  NativeRecordStats stats;
  const uint32_t mixed[] = {
      0x40024000u, 0x12345678u, 0xABCDEF01u,
      nf::recTag(nf::kRkSetConst, 2, 0), 0x10001u, 0x44332211u, 0x88776655u,
      nf::recTag(nf::kRkDraw, 0, 1), 0x42u, 0x12340000u, 17u,
      nf::recTag(nf::kRkResolve, 2, 0), 7u, 8u,
      0x00004900u};
  const std::vector<std::vector<uint32_t>> expected = {
      {0x100u, 0x4000u, 1u, 0x12345678u, 0xABCDEF01u},
      {0x300u, nf::kRkSetConst, 2u, 0x10001u, 0x44332211u, 0x88776655u},
      {0x200u, 0u, 0x42u, 0x12340000u, 17u},
      {0x300u, nf::kRkResolve, 2u, 7u, 8u},
      {0x100u, 0x4900u, 0u}};
  check(executeNativeRecords(mixed, context, handlers, stats, true) && context.calls == expected &&
            stats.runs == 2 && stats.draws == 1 && stats.packets == 2 && stats.bad == 0 && stats.drawTsc == 9,
        "mixed records preserve operation order, raw array bytes, dedup bit and counters");
  check(executeNativeRecords({}, context, handlers, stats, false) && context.calls == expected,
        "empty input performs no operation");

  // Independent expected lengths, including empty/nonempty variable arrays.
  // Do not derive these expectations from the parser's recWords helper.
  struct Layout { nf::RecKind kind; uint32_t n, words; };
  const Layout layouts[] = {
      {nf::kRkDraw, 99, 3}, {nf::kRkShaderPtr, 0, 2},
      {nf::kRkShaderImm, 0, 2}, {nf::kRkShaderImm, 3, 5},
      {nf::kRkConstLoad, 0, 3}, {nf::kRkSetConst, 0, 1}, {nf::kRkSetConst, 3, 4},
      {nf::kRkBinReg, 0, 1}, {nf::kRkEvent, 0, 1}, {nf::kRkFence, 0, 4},
      {nf::kRkInvalidate, 0, 3}, {nf::kRkRegRmw, 0, 3}, {nf::kRkEventShd, 0, 3}, {nf::kRkZpd, 0, 1},
      {nf::kRkResolve, 2, 2}};
  for (const Layout layout : layouts) {
    const std::string name = "kind=" + std::to_string(layout.kind) + " n=" + std::to_string(layout.n);
    std::vector<uint32_t> record{nf::recTag(layout.kind, layout.n, 0)};
    for (uint32_t i = 0; i < layout.words; ++i) record.push_back(0x12340000u + i);
    Context framed;
    NativeRecordStats framedStats;
    const bool ok = executeNativeRecords(record, framed, handlers, framedStats, false);
    std::vector<uint32_t> expectedCall = layout.kind == nf::kRkDraw
        ? std::vector<uint32_t>{0x200u, 0u} : std::vector<uint32_t>{0x300u, layout.kind, layout.n};
    expectedCall.insert(expectedCall.end(), record.begin() + 1, record.end());
    check(ok && framed.calls == std::vector<std::vector<uint32_t>>{expectedCall} && !framedStats.bad,
          name + " complete payload preserved");
    for (uint32_t size = 0; size < layout.words; ++size) {
      std::vector<uint32_t> shortRecord(record.begin(), record.begin() + 1 + size);
      reject(shortRecord, name + " truncated to " + std::to_string(size) + " rejects before mutation");
    }
  }

  Context pred;
  pred.predicate = false;
  NativeRecordStats predStats;
  const uint32_t predWords[] = {
      nf::recTag(nf::kRkDraw, 0, 2), 1u, 2u, 3u,
      nf::recTag(nf::kRkEvent, 0, 2), 1u,
      nf::recTag(nf::kRkBinReg, 2, 0), 1u,
      nf::recTag(nf::kRkDraw, 0, 2), 4u, 5u, 6u};
  check(executeNativeRecords(predWords, pred, handlers, predStats, false) && pred.skipped == 2 &&
            pred.calls == std::vector<std::vector<uint32_t>>{{0x300u, nf::kRkBinReg, 2u, 1u},
                                                          {0x200u, 1u, 4u, 5u, 6u}} &&
            predStats.draws == 2 && predStats.packets == 2 && predStats.bad == 0 && pred.timestamp == 100,
        "each record observes current predication; skipped draws retain consumed count");
  Context skippedOpaque;
  skippedOpaque.predicate = false;
  NativeRecordStats skippedOpaqueStats;
  const uint32_t opaqueWords[] = {nf::recTag(nf::kRkResolve, 2, 2), 1u, 2u};
  check(executeNativeRecords(opaqueWords, skippedOpaque, handlers, skippedOpaqueStats, false) &&
            skippedOpaque.calls.empty() && skippedOpaque.skipped == 1 && skippedOpaqueStats.packets == 1 &&
            skippedOpaqueStats.bad == 0, "registered opaque handler is skipped under a false predicate");

  for (uint32_t size = 0; size < 3; ++size) {
    std::vector<uint32_t> truncated{nf::recTag(nf::kRkDraw, 0, 0)};
    truncated.insert(truncated.end(), size, 0xAAAAu);
    reject(truncated, "draw truncated to " + std::to_string(size) + " rejects before payload access");
  }
  reject({0x00034000u, 1u, 2u}, "truncated state run rejected before context mutation");
  reject({nf::recTag(nf::kRkShaderImm, 65535u, 0), 1u, 2u}, "maximum shader length cannot overrun a short record");
  reject({nf::recTag(nf::kRkSetConst, 65535u, 0), 1u}, "maximum constant length cannot overrun a short record");
  reject({nf::recTag(127u, 0, 0)}, "unknown kind rejected");
  reject({nf::recTag(127u, 0, 2)}, "false predicate cannot conceal unknown kind", false);
  reject({nf::recTag(nf::kRkPm4, 1, 0), 0xC0005400u}, "legacy packet payload explicitly rejected");
  reject({nf::recTag(nf::kRkPm4, 1, 2), 0xC0005400u}, "false predicate cannot conceal legacy packet payload", false);
  reject({nf::recTag(nf::kRkResolve, 0, 0)}, "zero-sized opaque payload rejected");
  reject({nf::recTag(nf::kRkResolve, 1, 0), 1u}, "opaque semantic rejection stops consumption");
  reject({nf::recTag(nf::kRkClear, 1, 2), 1u}, "false predicate cannot conceal missing handler", false);

  Context declined;
  declined.accept = false;
  NativeRecordStats declinedStats;
  const uint32_t declinedWords[] = {nf::recTag(nf::kRkEvent, 0, 0), 1u, 0x00014000u, 99u};
  check(!executeNativeRecords(declinedWords, declined, handlers, declinedStats, false) &&
            declined.calls.empty() && declinedStats.bad == 1 && declinedStats.runs == 0,
        "declined context operation prevents later operations");
  Context prefix;
  NativeRecordStats prefixStats;
  const uint32_t prefixWords[] = {0x00014000u, 99u, nf::recTag(nf::kRkPm4, 1, 0), 0u, 0x00014001u, 88u};
  check(!executeNativeRecords(prefixWords, prefix, handlers, prefixStats, false) &&
            prefix.calls == std::vector<std::vector<uint32_t>>{{0x100u, 0x4000u, 0u, 99u}} &&
            prefixStats.runs == 1 && prefixStats.bad == 1,
        "failure preserves completed prefix and never executes a fallback or suffix");
  const std::vector<std::vector<uint32_t>> failOperations = {
      {0x00014001u, 88u},
      {nf::recTag(nf::kRkDraw, 0, 0), 1u, 2u, 3u},
      {nf::recTag(nf::kRkResolve, 2, 0), 1u, 2u}};
  for (size_t i = 0; i < failOperations.size(); ++i) {
    const auto& operation = failOperations[i];
    std::vector<uint32_t> record{0x00014000u, 99u};
    record.insert(record.end(), operation.begin(), operation.end());
    record.insert(record.end(), {nf::recTag(nf::kRkEvent, 0, 0), 77u});
    Context afterPrefix;
    afterPrefix.failAfter = 1;
    NativeRecordStats afterPrefixStats;
    check(!executeNativeRecords(record, afterPrefix, handlers, afterPrefixStats, false) &&
              afterPrefix.calls == std::vector<std::vector<uint32_t>>{{0x100u, 0x4000u, 0u, 99u}} &&
              afterPrefixStats.runs == 1 && afterPrefixStats.bad == 1,
          "failing operation " + std::to_string(i) + " preserves prefix and stops suffix");
  }

#ifdef _WIN32
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, system.dwPageSize * 2u,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  DWORD previous = 0;
  const bool protectedPage = memory && VirtualProtect(memory + system.dwPageSize, system.dwPageSize,
                                                     PAGE_NOACCESS, &previous);
  check(protectedPage, "truncation test owns an inaccessible guard page");
  if (protectedPage) {
    auto* finalWord = reinterpret_cast<uint32_t*>(memory + system.dwPageSize - sizeof(uint32_t));
    *finalWord = nf::recTag(nf::kRkDraw, 0, 0);
    Context guarded;
    NativeRecordStats guardedStats;
    check(!executeNativeRecords({finalWord, 1}, guarded, handlers, guardedStats, false) &&
              guarded.calls.empty() && guardedStats.bad == 1,
          "truncated draw at guard-page boundary returns without reading protected memory");
  }
  if (memory) VirtualFree(memory, 0, MEM_RELEASE);
#endif
  std::printf("[NATIVE_RECORD_EXECUTOR_TEST] %d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
