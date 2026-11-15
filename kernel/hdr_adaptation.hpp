#pragma once

#include <cstdint>

// Render-interval correction for the game's HDR history shader (high-frame-rate PORT FEATURE companion).
// SR_HDR_ADAPT_DT is default ON since the X2 bake (2026-10-18, docs/HIGH_FRAMERATE.md); =0 keeps the A/B.
namespace sr::kernel::hdr {

struct TickSnapshot {
  uint8_t* base = nullptr;
  uint32_t manager = 0;
  uint32_t ordinal = 0;
  double seconds = 0;
  bool enabled = false;
};

// One coherent live-mode/interval snapshot per render tick, restored on nesting.
class RenderTickScope {
 public:
  explicit RenderTickScope(uint8_t* base) noexcept;
  ~RenderTickScope();
  RenderTickScope(const RenderTickScope&) = delete;
  RenderTickScope& operator=(const RenderTickScope&) = delete;

 private:
  TickSnapshot previous_;
};

struct EffectSnapshot {
  uint8_t* base = nullptr;
  uint32_t effect = 0;
  uint32_t block = 0;
};

class CompiledEffectScope {
 public:
  CompiledEffectScope(uint8_t* base, uint32_t effect, uint32_t block) noexcept;
  ~CompiledEffectScope();
  CompiledEffectScope(const CompiledEffectScope&) = delete;
  CompiledEffectScope& operator=(const CompiledEffectScope&) = delete;

 private:
  EffectSnapshot previous_;
};

// Called after the synchronous SDK setter, with its original arguments. Leaves
// the authored source, PPC registers and dirty-mask semantics untouched.
void afterPsConstantUpload(uint8_t* base, uint32_t device, uint32_t start,
                           uint32_t source, uint32_t count) noexcept;

bool scaledTimeScale(float authored, double seconds, float& result) noexcept;

// G3 (step 6): how the metadata reads were served - direct copies of proved-readable ranges (SR_HDR_DIRECT_READ,
// default ON), OS copies (every read with SR_HDR_DIRECT_READ=0, the A/B; with it on, the ranges the predicate refused
// and the direct copies that faulted), the predicate's refusals and the faults.
struct ReadStats {
  uint64_t direct = 0, os = 0, unreadable = 0;
  uint64_t faults = 0;  // direct copies that faulted (a stale readable-cache answer) and fell back to the OS copy
};
ReadStats readStats() noexcept;
// Test seam (difftest only): -1 = the env gate, 0 = the OS copy, 1 = direct.
void setDirectReadsForTest(int mode) noexcept;

}  // namespace sr::kernel::hdr
