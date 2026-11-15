// Included in native_flush.cpp in difftest only. These fixtures call the real
// owned-record and segment consumer to check acknowledgement/retirement failure.
#include "gfx/xenos_state.hpp"
namespace sr::native::flush {
int runNativeRecordFailureTest() {
  int passed = 0, failed = 0;
  auto check = [&](bool ok, const char* label) {
    std::printf("%s%s\n", ok ? "  ok  " : "FAIL: ", label);
    ok ? ++passed : ++failed;
  };
  if (!g_segById.empty()) return 2;
  constexpr uint64_t pos = 128u;
  auto record = [&](std::initializer_list<uint32_t> payload) {
    const RecHdr h{kRecMagic, uint32_t(sizeof(RecHdr) + payload.size() * 4u), pos, 0u, 0u};
    std::vector<uint8_t> bytes(h.bytes);
    std::memcpy(bytes.data(), &h, sizeof(h));
    std::memcpy(bytes.data() + sizeof(h), payload.begin(), payload.size() * 4u);
    return segRecordFromBytes(bytes.data(), h.bytes);  // I7: sized for its words (Astra I7 P1-1)
  };
  auto engine = [] {
    sr::gfx::Pm4Engine engine;
    engine.cpMode = true;
    engine.submitDraws = false;
    return engine;
  };
  // The development packet builder admits literal packet records. Its malformed
  // case therefore uses an unknown kind and rejects before executing any prefix.
  const uint32_t badKind = g_cpPm4 ? 127u : kRkPm4;
  auto bad = record({0x00014000u, 0x11223344u, recTag(badKind, 1u, 0u), 0u, 0x00014001u, 0x55667788u});
  g_cpTail.store(0u);
  auto consumer = engine();
  auto& registers = sr::gfx::getXenosRegisters();
  registers.write(0x4000u, 0u); registers.write(0x4001u, 0u);
  check(!cpApplyRecord(consumer, pos, bad.get()) && consumer.nativeRecordFailed &&
            !bad->completed.load() && g_cpTail.load() == 0u && t_curRec == 0u,
        "rejected owned record is not completed or retired");
  check(registers.read(0x4000u) == (g_cpPm4 ? 0u : 0x44332211u) && registers.read(0x4001u) == 0u,
        "owned failure preserves only executed prefix and stops suffix");
  auto good = record({0x00014001u, 0x55667788u});
  check(!cpApplyRecord(consumer, pos, good.get()) && !good->completed.load() && g_cpTail.load() == 0u,
        "failure latch prevents later owned record acknowledgement");
  auto valid = engine();
  check(cpApplyRecord(valid, pos, good.get()) && good->completed.load() &&
            g_cpTail.load() == pos + good->size() * 4u && registers.read(0x4001u) == 0x88776655u,
        "successful owned record still publishes completion and retirement");
  {
    RecHdr h{kRecMagic, uint32_t(sizeof(RecHdr) + 4u), pos, 0u, 0u};
    auto shortRecordOwned = segRecordFromBytes(reinterpret_cast<const uint8_t*>(&h), sizeof(h));
    SegRecord& shortRecord = *shortRecordOwned;
    auto shortConsumer = engine();
    const uint64_t before = g_cpTail.load();
    check(!cpApplyRecord(shortConsumer, pos, &shortRecord) && shortConsumer.nativeRecordFailed &&
              !shortRecord.completed.load() && g_cpTail.load() == before,
          "declared payload cannot exceed owned storage");
  }
  {
    auto missing = engine();
    check(!cpSegment(missing, UINT64_MAX, 0u, 0u) && missing.nativeRecordFailed,
          "unknown segment identity is a latched failure");
  }
  {
    auto sp = std::make_shared<SegParts>();
    sp->id = UINT64_MAX - 1u; sp->phys = 0x1000u; sp->dwords = 12u; sp->published = 1u;
    sp->parts.push_back(SegPart{0u, 24u, pos, 0u, 0u, bad});
    sp->parts.push_back(SegPart{0u, 8u, pos, 0u, 0u, record({0x00014002u, 0xAABBCCDDu})});
    g_segById.emplace(sp->id, sp);
    registers.write(0x4002u, 0u);
    auto segmentConsumer = engine();
    const uint64_t before = g_cpTail.load();
    check(!cpSegment(segmentConsumer, sp->id, sp->phys, sp->dwords) && segmentConsumer.nativeRecordFailed &&
              sp->started == 1u && sp->finished == 0u && !sp->parts[1].record->completed.load() &&
              registers.read(0x4002u) == 0u && g_cpTail.load() == before && !t_segCur.sp,
          "segment failure stops later parts and never marks the pass finished");
    g_segById.erase(sp->id);
  }
  {
    setOpaqueHandler(kRkResolve, [](uint32_t, const uint32_t*, uint32_t, sr::gfx::Pm4Engine& consumer) {
      const uint32_t rejected[] = {recTag(kRkPm4, 1u, 0u), 0u};
      sr::gfx::Pm4Engine::NativeRecordStats stats;
      consumer.nativeRecord(rejected, rejected + 2u, stats, false);
      return true;
    });
    auto poisoned = record({0x00014000u, 0x11223344u, recTag(kRkResolve, 1u, 0u), 0u,
                            0x00014001u, 0x55667788u});
    registers.write(0x4000u, 0u); registers.write(0x4001u, 0u);
    auto nested = engine();
    const uint64_t before = g_cpTail.load();
    check(!cpApplyRecord(nested, pos, poisoned.get()) && nested.nativeRecordFailed &&
              !poisoned->completed.load() && g_cpTail.load() == before && t_curRec == 0u,
          "nested consumer failure prevents outer owned completion and retirement");
    check(registers.read(0x4000u) == 0x44332211u && registers.read(0x4001u) == 0u,
          "nested consumer failure stops outer owned suffix despite helper success");
    setOpaqueHandler(kRkResolve, nullptr);
  }
  std::printf("[NATIVE_RECORD_FAILURE_TEST] %d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
}  // namespace sr::native::flush
