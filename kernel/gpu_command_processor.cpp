// The development Oracle's packet decoder. This unit is excluded from release.
#include "gpu_command_processor.hpp"
#include "guest_context.hpp"
#include "native_meter.hpp"
#include "../gfx/pm4_engine.hpp"
#include "../gfx/pm4_coverage.hpp"
#include <cstring>

namespace sr::gfx {
void executeGuestIndirectBuffer(uint32_t ibPhys, uint32_t ibSizeDwords);
}

namespace sr::kernel {
void gpuCpPeekOraclePacket(uint32_t base, uint32_t index, OracleRingPacket& packet) {
  const uint32_t header = readGuestU32BE(base + index * 4u);
  packet.header = header;
  switch (header >> 30) {
    case 0: case 3: packet.dwords = header ? 2u + ((header >> 16) & 0x3FFFu) : 1u; break;
    case 1: packet.dwords = 3; break;
    default: packet.dwords = 1; break;
  }
}

bool gpuCpOraclePacketIndirect(const OracleRingPacket& packet) {
  const uint32_t opcode = (packet.header >> 8) & 0xFFu;
  return (packet.header >> 30) == 3u && (opcode == 0x3Fu || opcode == 0x37u) &&
         packet.dwords >= 3 && !(packet.header & 1u);
}

void gpuCpPrepareOraclePacket(OracleRingPacket& packet, uint32_t base, uint32_t mask, uint32_t index) {
  const uint32_t count = packet.dwords;
  packet.bytes.resize(size_t(count) * 4u);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t va = base + ((index + i) & mask) * 4u;
    if (const uint8_t* source = guestVAToHost(va))
      std::memcpy(&packet.bytes[size_t(i) * 4u], source, 4);
  }
  if (gpuCpOraclePacketIndirect(packet)) {
    if (sr::gfx::pm4cov::on()) sr::gfx::pm4cov::ringIndirect(index, count);
    sr::native::meter::add(sr::native::meter::kPm4);
  }
}

void gpuCpExecuteOraclePacket(sr::gfx::Pm4Engine& engine, const OracleRingPacket& packet, uint32_t index) {
  if (gpuCpOraclePacketIndirect(packet)) {
    const auto& bytes = packet.bytes;
    const uint32_t phys = uint32_t(bytes[4]) << 24 | uint32_t(bytes[5]) << 16 |
                          uint32_t(bytes[6]) << 8 | bytes[7];
    const uint32_t dwords = (uint32_t(bytes[8]) << 24 | uint32_t(bytes[9]) << 16 |
                             uint32_t(bytes[10]) << 8 | bytes[11]) & 0xFFFFFu;
    // Keep the Oracle's allocation/window resolution in its existing IB entry.
    sr::gfx::executeGuestIndirectBuffer(phys, dwords);
  } else {
    const bool coverage = sr::gfx::pm4cov::on();
    if (coverage) sr::gfx::pm4cov::setRingPacket(index, packet.dwords);
    engine.execute(packet.bytes.data(), packet.bytes.size(), 0);
    if (coverage) sr::gfx::pm4cov::clearRingPacket();
  }
}
}
