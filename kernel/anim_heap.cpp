// iter-465 character switch (PORT FEATURE, docs/MODS.md "Character switch"; docs/OVERRIDES.json).
//
// The game keeps every animation project's banks in one DOGMA heap, HeapAnimation (pool global
// 0x8296D8A0). MemoryPoolSystemInit (sub_82186500) builds the 11 heaps once at boot from the
// table at 0x8291189C (24-byte records {pool*, size, extra, name*, skip, index}); HeapAnimation is
// record 0 and its size is the dword at 0x829118A0 = 0x00400000 (4 MiB). Each heap's core comes
// from the guest VirtualAlloc, so a larger size just moves the later heaps up.
// Retail fills ~3.09 MiB of it (Xenia golden dumps; Superman's BASEWORLDFLOW_ANIM is 2.44 MB) and
// a second full character project (~2.5 MB of banks) does not fit: its loads fail and the game
// hangs (ANIMATION_BINDING.md "Plan B data").
//
// When own-project characters are preloaded (kernel/mod_manager.cpp modCharacterAnimBytes), the
// record's size is raised by their banks + 1 MiB headroom before the original body runs. With no
// such character (and on every natural roll) nothing is written and the original runs unchanged.
// SR_ANIM_HEAP_MB=<n> sets the size explicitly (A/B; 0 or unset = automatic).
#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "mod_manager.hpp"
#include "../app/log.hpp"

#include <cstdint>
#include <cstdlib>

extern "C" void __imp__sub_82186500(PPCContext& __restrict ctx, uint8_t* base);

namespace {
constexpr uint32_t kHeapAnimationSizeVA = 0x829118A0u;  // record 0 (+4) of the heap table
constexpr uint32_t kHeapAnimationPoolVA = 0x8296D8A0u;  // record 0 (+0): the pool global
constexpr uint32_t kRetailSize = 0x00400000u;
constexpr uint32_t kMiB = 1024u * 1024u;
}  // namespace

PPC_FUNC(sub_82186500) {
  uint32_t want = 0;
  const char* env = std::getenv("SR_ANIM_HEAP_MB");
  const long mb = env ? std::strtol(env, nullptr, 10) : 0;
  if (mb > 0 && mb <= 64) {
    want = static_cast<uint32_t>(mb) * kMiB;
  } else if (sr::kernel::modCharacterSwitchEnabled()) {
    const uint64_t extra = sr::kernel::modCharacterAnimBytes();
    if (extra > 0 && extra < 48ull * kMiB)
      want = kRetailSize + ((static_cast<uint32_t>(extra) + kMiB + kMiB - 1) & ~(kMiB - 1));
  }
  const uint32_t pool = PPC_LOAD_U32(kHeapAnimationSizeVA - 4);
  const uint32_t size = PPC_LOAD_U32(kHeapAnimationSizeVA);
  if (want > kRetailSize && pool == kHeapAnimationPoolVA && size == kRetailSize) {
    sr::kernel::writeGuestBE32(kHeapAnimationSizeVA, want);
    SR_LOG_BOOT(sr::LogSeverity::Info, "[ANIM_HEAP] HeapAnimation %u -> %u bytes (%s)", size, want,
                mb > 0 ? "SR_ANIM_HEAP_MB" : "own-project characters preloaded");
  } else if (want > kRetailSize) {
    SR_LOG_BOOT(sr::LogSeverity::Warn, "[ANIM_HEAP] heap table not retail (pool %08X size %08X); left as is",
                pool, size);
  }
  __imp__sub_82186500(ctx, base);
}
