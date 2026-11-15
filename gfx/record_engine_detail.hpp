#pragma once

// The record engine's byte and register-store helpers, shared by gfx/record_engine.cpp (which defines the four
// functions) and the development PM4 walker gfx/pm4_engine.cpp (C1, docs/RELEASE_COMPOSITION.md 2.1, stage 2a). Private
// to the two units; nothing else includes it.

#include <cstdint>

#include "record_engine.hpp"

namespace sr::gfx::redetail {

inline uint32_t readBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Raw little-endian dword load (matches Xenia's xe::load<uint32_t> on x86,
// which is applied BEFORE GpuSwap — do not confuse with readBE32).
inline uint32_t rawLE32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Xenos GpuSwap — endian bits (addr & 3): 0 none, 1 8in16, 2 8in32, 3 16in32
// (reference/xenia-canary/src/xenia/gpu/xenos.h Endian enum).
inline uint32_t gpuSwap32(uint32_t v, uint32_t endian) {
  switch (endian & 3) {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);  // 8in16
    case 2: return (v << 24) | ((v & 0xFF00u) << 8) |                    // 8in32
                   ((v >> 8) & 0xFF00u) | (v >> 24);
    case 3: return (v << 16) | (v >> 16);                               // 16in32
    default: return v;                                                  // none
  }
}

// SR_PM4_BULK_TYPE0 (default ON; =0 keeps the per-register A/B): a register run with no side-effect register in it is
// pure register-file stores.
bool bulkType0Enabled();
// Does [r0, r0+n) touch a register writeReg gives a side effect (the CP scratch write-back, the gamma ramp, the
// fetch-constant-0 census, SR_PM4_REGLOG, the copy-destination latches, COHER_BASE_HOST)?
bool regRangeHasEffects(uint32_t r0, uint32_t n);
// The byte-swapping bulk store of a big-endian run into the register file (registers past the file dropped), with the
// constant-block dirty bits XenosRegisterFile::write would set.
void bulkBeStore(XenosRegisterFile& xr, uint32_t start, const uint8_t* srcBE, uint32_t n);
// The CP's pre-observation drain (cp.flush) with the SR_NATIVE_IDLE_DIAG timing by site (0 WAIT_REG_MEM, 1 INTERRUPT,
// 2 XE_SWAP, 3 EVENT_WRITE_SHD, 4 MEM_WRITE, 5 REG_TO_MEM); a plain flush under SR_ORACLE=0.
void cpFlushTimed(void (*flush)(void*), void* user, int site);

}  // namespace sr::gfx::redetail
