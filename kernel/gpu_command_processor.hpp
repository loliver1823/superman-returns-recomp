#pragma once

// Oracle-only ring packet decoding. Native callers use gpu_submit.hpp.
#include "sr_oracle.hpp"
#if SR_ORACLE
#include <cstdint>
#include <vector>

namespace sr::gfx { class Pm4Engine; }
namespace sr::kernel {
struct OracleRingPacket {
  uint32_t header = 0, dwords = 0;
  std::vector<uint8_t> bytes;
};
void gpuCpPeekOraclePacket(uint32_t base, uint32_t index, OracleRingPacket& packet);
bool gpuCpOraclePacketIndirect(const OracleRingPacket& packet);
void gpuCpPrepareOraclePacket(OracleRingPacket& packet, uint32_t base, uint32_t mask, uint32_t index);
void gpuCpExecuteOraclePacket(sr::gfx::Pm4Engine& engine, const OracleRingPacket& packet, uint32_t index);
}
#endif
