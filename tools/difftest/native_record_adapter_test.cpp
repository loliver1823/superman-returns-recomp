// Actual-engine differential for the extracted record executor. Valid fixtures
// use the preserved old walker as oracle. Malformed inputs run only the new
// bounded path. No GPU worker or pixel/shader-binding acceptance.
#include "gfx/pm4_engine.hpp"
#include "gfx/xenos_state.hpp"
#include "kernel/native_flush.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
namespace nf = sr::native::flush;
using sr::gfx::Pm4Engine;
int passed = 0, failed = 0;
void check(bool ok, const std::string& label) {
  std::printf("%s%s\n", ok ? "  ok  " : "FAIL: ", label.c_str());
  ok ? ++passed : ++failed;
}
struct Memory {
  std::array<uint8_t, 2048> bytes{};
  std::vector<std::array<uint32_t, 3>> swaps;
  static uint8_t* map(void* user, uint32_t address) {
    auto& self = *static_cast<Memory*>(user);
    return address < self.bytes.size() ? self.bytes.data() + address : nullptr;
  }
  static void swap(void* user, uint32_t address, uint32_t width, uint32_t height) {
    static_cast<Memory*>(user)->swaps.push_back({address, width, height});
  }
};
struct Snapshot {
  std::vector<uint32_t> registers;
  std::array<uint32_t, 256> opcodes;
  std::array<uint64_t, 12> counters;
  std::array<uint8_t, 2048> memory;
  std::vector<std::array<uint32_t, 3>> swaps;
  std::array<uint64_t, 4> stats;
  uint32_t dirty;
  bool operator==(const Snapshot&) const = default;
};
std::unique_ptr<Pm4Engine> engine(Memory& memory) {
  auto result = std::make_unique<Pm4Engine>();
  result->cpMode = true;
  result->submitDraws = false;
  result->user = result->cp.user = &memory;
  result->physToHost = Memory::map;
  result->cp.swap = Memory::swap;
  return result;
}
void seed(Memory& memory) {
  auto& registers = sr::gfx::getXenosRegisters();
  for (uint32_t i = 0; i < sr::gfx::XenosRegisterFile::kNumRegisters; ++i) registers.write(i, 0);
  registers.takeAluDirty();
  for (size_t i = 0; i < memory.bytes.size(); ++i) memory.bytes[i] = uint8_t(i * 7u + 3u);
}
Snapshot snapshot(Pm4Engine& engine, Memory& memory, const Pm4Engine::NativeRecordStats& stats) {
  auto& registers = sr::gfx::getXenosRegisters();
  Snapshot result;
  result.registers.assign(registers.data(), registers.data() + sr::gfx::XenosRegisterFile::kNumRegisters);
  std::copy(std::begin(engine.opcodeHist), std::end(engine.opcodeHist), result.opcodes.begin());
  result.counters = {engine.draws, engine.regWrites, engine.vertexShaderLoads, engine.pixelShaderLoads,
                     engine.binMask, engine.binSelect, engine.predSkipped, engine.predDrawsRun,
                     engine.cpMemWrites, engine.cpWaits, engine.cpSwaps, engine.cpScratchWrites};
  result.memory = memory.bytes;
  result.swaps = memory.swaps;
  result.stats = {stats.runs, stats.draws, stats.packets, stats.bad};
  result.dirty = registers.takeAluDirty();
  return result;
}
bool opaque(uint32_t kind, const uint32_t* words, uint32_t count, Pm4Engine& engine) {
  if (kind != nf::kRkResolve || count != 2 || words[0] > 0x7FFFu) return false;
  engine.writeReg(words[0], words[1]);
  return true;
}
bool nestedFailure(uint32_t, const uint32_t*, uint32_t, Pm4Engine& engine) {
  const uint32_t rejected[] = {nf::recTag(nf::kRkPm4, 1u, 0u), 0u};
  Pm4Engine::NativeRecordStats stats;
  engine.nativeRecord(rejected, rejected + std::size(rejected), stats, false);
  // A legacy helper returning true cannot clear its nested consumer's failure.
  return true;
}
void compare(const std::vector<uint32_t>& words, const std::string& label) {
  Memory expectedMemory;
  seed(expectedMemory);
  auto oracle = engine(expectedMemory);
  Pm4Engine::NativeRecordStats oracleStats;
  oracle->nativeRecordOracle(words.data(), words.data() + words.size(), oracleStats, false);
  const auto expected = snapshot(*oracle, expectedMemory, oracleStats);
  Memory actualMemory;
  seed(actualMemory);
  auto actual = engine(actualMemory);
  Pm4Engine::NativeRecordStats actualStats;
  const bool complete = actual->nativeRecord(words.data(), words.data() + words.size(), actualStats, false);
  check(complete && !actual->nativeRecordFailed && !actualStats.bad && actualStats.drawTsc == 0 &&
            snapshot(*actual, actualMemory, actualStats) == expected, label);
}
void reject(const std::vector<uint32_t>& words, const std::string& label, bool predicated = false) {
  Memory memory;
  seed(memory);
  auto actual = engine(memory);
  if (predicated) actual->binMask = actual->binSelect = 0;
  Pm4Engine::NativeRecordStats stats;
  const auto before = snapshot(*actual, memory, stats);
  const bool complete = actual->nativeRecord(words.data(), words.data() + words.size(), stats, false);
  check(!complete && actual->nativeRecordFailed && stats.bad == 1 &&
            snapshot(*actual, memory, {}) == before,
        label);
}
}  // namespace

int runNativeRecordAdapterTest() {
  nf::setOpaqueHandler(nf::kRkResolve, opaque);
  compare({0x00024000u, 0x11223344u, 0x55667788u, 0x00022180u, 0x12340000u, 0x56780000u,
           nf::recTag(nf::kRkSetConst, 2u, 0u), 0u, 0xA1B2C3D4u, 0x10203040u,
           nf::recTag(nf::kRkSetConst, 2u, 0u), 0x10000u, 0x01020304u, 0x05060708u,
           nf::recTag(nf::kRkConstLoad, 0u, 0u), 0x200u, 0x20002u, 3u,
           nf::recTag(nf::kRkRegRmw, 0u, 0u), 0x4900u & 0x1FFFu, 0xFFFF00FFu, 0x3300u,
           nf::recTag(nf::kRkEvent, 0u, 0u), 7u, 0x00004004u},
          "actual oracle matches mixed state/constant/event commands and complete register file");
  compare({nf::recTag(nf::kRkBinReg, 0u, 0u), 0u,
           nf::recTag(nf::kRkDraw, 0u, 2u), 0u, 0u, 0u,
           nf::recTag(nf::kRkEvent, 0u, 2u), 4u,
           nf::recTag(nf::kRkResolve, 2u, 2u), 0x2100u, 0xBADu,
           nf::recTag(nf::kRkBinReg, 0u, 0u), 1u,
           nf::recTag(nf::kRkBinReg, 2u, 0u), 1u,
           nf::recTag(nf::kRkDraw, 0u, 2u), 0u, 0u, 0u,
           nf::recTag(nf::kRkResolve, 2u, 0u), 0x2100u, 0x11223344u},
          "actual oracle matches dynamic bin predication, draws and registered opaque handler");
  compare({nf::recTag(nf::kRkFence, 0u, 0u), 0x102u, 0x12345678u, 0x106u, 0x89ABCDEFu,
           nf::recTag(nf::kRkInvalidate, 0u, 0u), 0x200u, 32u, 0u,
           nf::recTag(nf::kRkInvalidate, 0u, 0u), 0u, 0u, 1u},
          "actual oracle matches fence stores and coherency effects");
  for (uint32_t endian = 0; endian < 4u; ++endian)
    compare({nf::recTag(nf::kRkEventShd, 0u, 0u), 3u, 0x100u | endian, 0x12345678u},
            "actual oracle matches event-store endian " + std::to_string(endian));
  // p2.zpd's bake: EVENT_WRITE_ZPD as a record (no submitted draws here: the register and the census, no host query)
  compare({0x00012325u, 0x40F20000u, nf::recTag(nf::kRkZpd, 0u, 0u), 21u,
           nf::recTag(nf::kRkZpd, 0u, 2u), 0x55u},
          "actual oracle matches the zpd event record (RB_SAMPLE_COUNT_ADDR, VGT_EVENT_INITIATOR, opcode 0x5B; predicated)");
  // Empty shader payloads check the ABI/dispatch only; real shader bindings and
  // pixels must be established by later replay/live rendering evidence.
  compare({nf::recTag(nf::kRkShaderPtr, 0u, 0u), 0x300u, 0u,
           nf::recTag(nf::kRkShaderImm, 0u, 0u), 1u, 0u},
          "actual oracle matches empty shader payload dispatch (binding unproved)");

  reject({0x00025FFFu, 1u, 2u}, "out-of-range run rejects before register mutation");
  reject({nf::recTag(nf::kRkSetConst, 1u, 0u), 0xFF0000u, 1u}, "unknown constant type rejects before mutation");
  std::vector<uint32_t> oversized{nf::recTag(nf::kRkSetConst, 7000u, 0u), 0x307FFu};
  oversized.resize(7002u, 1u);
  reject(oversized, "constant destination overflow rejects before mutation");
  reject({nf::recTag(nf::kRkShaderImm, 1u, 0u), 0u, 2u, 0u}, "shader size cannot exceed admitted inline payload");
  {
    // The premise is an UNREGISTERED kind. A linked producer may register this one at static init (native-b row 9,
    // kernel/native_rec_b.cpp: kinds 7..9), so unregister it for this case and restore it after.
    const nf::OpaqueHandler clearHandler = nf::opaqueHandler(nf::kRkClear);
    const nf::OpaqueRenderer clearRenderer = nf::opaqueRenderer(nf::kRkClear);
    nf::setOpaqueHandler(nf::kRkClear, nullptr);
    reject({nf::recTag(nf::kRkClear, 1u, 2u), 0u}, "false predicate cannot conceal an unregistered oracle helper", true);
    nf::setOpaqueHandler(nf::kRkClear, clearHandler, clearRenderer);
  }
  reject({nf::recTag(nf::kRkPm4, 1u, 2u), 0u}, "false predicate cannot conceal a literal packet record", true);
  reject({nf::recTag(nf::kRkResolve, 1u, 0u), 0u}, "opaque helper decline propagates before mutation");
  {
    Memory memory;
    seed(memory);
    auto actual = engine(memory);
    Pm4Engine::NativeRecordStats stats;
    const uint32_t words[] = {0x00014000u, 0x11223344u, nf::recTag(nf::kRkPm4, 1u, 0u), 0u,
                              0x00014001u, 0x55667788u};
    check(!actual->nativeRecord(words, words + std::size(words), stats, false) &&
              sr::gfx::getXenosRegisters().read(0x4000u) == 0x44332211u &&
              sr::gfx::getXenosRegisters().read(0x4001u) == 0u && stats.runs == 1 && stats.bad == 1,
          "actual engine preserves completed prefix and rejects suffix");
    const uint8_t packet[] = {0, 0, 0x40, 1, 0xAA, 0xBB, 0xCC, 0xDD};
    actual->execute(packet, sizeof(packet));
    const uint32_t suffix[] = {0x00014001u, 0xAABBCCDDu};
    check(!actual->nativeRecord(suffix, suffix + std::size(suffix), stats, false) &&
              sr::gfx::getXenosRegisters().read(0x4001u) == 0u && actual->nativeRecordFailed,
          "failure latch blocks both later native records and packet replay");
  }
  {
    nf::setOpaqueHandler(nf::kRkResolve, nestedFailure);
    Memory memory;
    seed(memory);
    auto actual = engine(memory);
    Pm4Engine::NativeRecordStats stats;
    const uint32_t words[] = {0x00014000u, 0x11223344u,
                              nf::recTag(nf::kRkResolve, 1u, 0u), 0u,
                              0x00014001u, 0x55667788u};
    check(!actual->nativeRecord(words, words + std::size(words), stats, false) &&
              actual->nativeRecordFailed && stats.bad == 1 && stats.runs == 1 &&
              sr::gfx::getXenosRegisters().read(0x4000u) == 0x44332211u &&
              sr::gfx::getXenosRegisters().read(0x4001u) == 0u,
          "nested failure overrides helper success and prevents outer suffix execution");
  }
  nf::setOpaqueHandler(nf::kRkResolve, nullptr);
  std::printf("[NATIVE_RECORD_ADAPTER_TEST] %d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}
