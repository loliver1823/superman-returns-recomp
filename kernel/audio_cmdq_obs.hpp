#pragma once
// iter-463 D-AUDIO-CMDBUF (recurrence on bc1f8fc with SR_XMA_NEW on): observe probes and named
// stress diagnostics for the RWAudioCore command queue and EA's stream-block reader.
// Implementation + full rationale: kernel/audio_cmdq_obs.cpp. Difftest: build/difftest --cmdq-test.
//
// Gates (all read once):
//   SR_RWAC_Q_OBS            default ON  observe only: [RWAC_Q] 1 Hz, [RWAC_Q_STALL], [EA_RDR]
//   SR_PROTECT_ZERO          default OFF S1: guest VA [0, 0x10000) no-access (Xenia protect_zero)
//   SR_PROTECT_ZERO_DELAY_S  default 0   S1: seconds after the first RWAC drain before arming
//   SR_DIAG_NULLPAGE_POISON  default OFF S2 (FORCING, needs SR_ALLOW_FORCING=1): fill guest
//                                        [0, N) with 0xFF bytes every 100 ms (=1 -> N = 0x40)
//   SR_DIAG_DAC_WAKE_DELAY_US default 0  S3: host delay on the DAC thread before each RWAC drain
//   SR_DIAG_IO_DELAY_MS      default 0   S3 control: host delay before each NtReadFile on a
//                                        handle whose path contains SR_DIAG_IO_DELAY_MATCH
//                                        (default "sound.AST")
#include <cstdint>
#include <string>

namespace sr {
namespace kernel {
namespace cmdq {

// The RWAC system singleton pointer (RwacSystem, dword_8299FDC8) and its queue fields
// (RwacSystem_ProcessCmdQueue_sub_8264C540 / RwacCmd_EnqueueSetParamF_sub_8264CE48).
constexpr uint32_t kRwacSingletonVA = 0x8299FDC8u;
constexpr uint32_t kQTicks = 0x14;      // ++ once per drain (golden: 187.5 Hz)
constexpr uint32_t kQBuffer = 0x20;
constexpr uint32_t kQCapacity = 0x24;   // 0x100000
constexpr uint32_t kQWrite = 0x28;      // producer offset, zeroed by each drain
constexpr uint32_t kQHighWater = 0x2C;  // golden 0xA10

constexpr uint32_t kNullPageEnd = 0x10000u;   // Xenia memory.cc: protect_zero range

// True when a guest pointer lies in a range the console could hand out for a heap or
// static object (the same windows as [CS_GARBAGE]); false for the null page, 0xFFFFFFFF-class
// garbage and FOURCC values below the image.
bool guestObjectPlausible(uint32_t va);

// The [EA_RDR] anomaly predicate for EaStrm_DequeueReadyBlock_sub_825FC760's reader:
// a reader in the null page, or a live-looking reader whose owner (reader+4) is not a
// plausible object. ownerKnown=false when the owner was not read (null-page reader).
bool readerAnomalous(uint32_t reader, uint32_t owner, bool ownerKnown);

// The [RWAC_Q_STALL] detector: the drain tick counter froze for >= thresholdMs while
// XAudio render callbacks kept advancing (the DAC thread is the only drainer and runs
// once per render frame, so a frozen tick under live callbacks is a DAC-side stall).
struct StallDetector {
  enum Event { kNone, kStart, kEnd };
  uint32_t thresholdMs = 250;
  bool primed = false, stalled = false;
  uint32_t lastTicks = 0;
  uint64_t lastTickChangeMs = 0, cbAtLastTickChange = 0, stallStartMs = 0;
  Event feed(uint64_t nowMs, uint32_t ticks, uint64_t callbacks);
};

// S1 mechanics: make host [base, base + bytes) no-access under the hostRangeReadable
// removal protocol. Returns false (and changes nothing) if VirtualProtect fails.
bool protectRange(uint8_t* hostBase, uint32_t bytes);

// S3 control (called from kernel/stubs.cpp): note an opened path / a closed handle, and
// delay a read on a matching handle. All no-ops unless SR_DIAG_IO_DELAY_MS > 0.
void ioDiagNoteOpen(uint32_t handle, const std::string& path);
void ioDiagNoteClose(uint32_t handle);
void ioDiagMaybeDelay(uint32_t handle);


}  // namespace cmdq
}  // namespace kernel
}  // namespace sr
