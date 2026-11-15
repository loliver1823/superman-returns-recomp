// iter-467 N6: the audio-hardware census, readable from code (the native meter) and logged as
// [AUDIO_NATIVE] (kernel/apu/audio_system.cpp, every 30 s). Totals since process start (relaxed
// atomics; a reader diffs two snapshots for a per-frame count). The field names are the log keys
// and are STABLE - the native meter reads them.
//   guest_mmio_fns     entries into the six guest functions that touch the XMA register block
//                      0x7FEA0000 (NATIVE_ROADMAP B.1); 0 = no guest XMA register access
//   xma_regfile_writes register-level kick / lock / clear writes inside the XMA device (the Xenia
//                      register path); 0 = the XMA* exports run natively (SR_XMA_NATIVE_FRONT)
//   xma_hal_voices     XDK XMA HAL voice constructions;   xma_hal_rest  entries into the five
//                      register-free HAL functions (B.2); both 0 = the HAL is dead
//   reg600             native / lwbrx / not-run, and the context-array PA it published
#pragma once
#include <cstddef>
#include <cstdint>

#include "sr_oracle.hpp"

namespace sr::kernel {

struct AudioNativeCensus {
  uint64_t guest_mmio_fns = 0;      // = the six below
  uint64_t reg600_lwbrx = 0;        // original reg600 boundary; release counts then refuses it
  uint64_t hal_clear = 0;           // Xma_CreateContexts_sub_82119980 (Clear stwbrx)
  uint64_t hal_lock = 0;            // XmaHw_LockContexts_sub_82119BA8
  uint64_t hal_waitidle = 0;        // XmaHw_WaitIdleOrResetDecoder_sub_82119C28
  uint64_t hal_poll = 0;            // XmaHw_PollIdleAndSyncShadow_sub_82119CE8
  uint64_t hal_kick = 0;            // XmaHw_WritebackShadowAndKick_sub_82119E68
  uint64_t xma_regfile_writes = 0;
  uint64_t xma_hal_voices = 0;
  uint64_t xma_hal_rest = 0;
  uint64_t reg600_native = 0;       // native sub_82116800 runs
  uint32_t reg600_value = 0;        // the PA dword_829AC0E8 received
};

// Retained native diagnostic counters in both configurations. The release observes
// the six MMIO entry points; register-free HAL detail remains Oracle-only.
// XMA register dispatch itself exists only in SR_DIFFTEST, never in the game.
AudioNativeCensus audioNativeCensus();
int audioNativeCensusLine(char* out, size_t n);

}  // namespace sr::kernel
