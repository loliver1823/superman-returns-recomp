// iter-32 — kill the post-WALK_END Enter+Leave spin.
//
// Background (from the iter-32 CS-spin instrumentation in kernel/stubs.cpp):
// after WALK_END the main thread enters a 2.2 M-call/sec Enter+Leave loop.
// Top callers fingerprinted by the histogram:
//
//   EnterLR 0x827AC4E0 ≈ 870 k/s   ← inside sub_827AC498 (CRT file-lock acquire)
//   LeaveLR 0x827AC480 ≈ 870 k/s   ← inside sub_827AC468 (CRT file-lock release)
//   EnterLR 0x8246D400 / LeaveLR 0x8246D628  ← downstream heap free path
//   CSPtr   0x82111000 / 0x8E2BB8D0
//
// sub_827AC498 (recomp body at ppc_recomp.113.cpp:13311) is a CRT-style
// _lock_file wrapper:
//
//   index = r3
//   slotVA = 0x82946AE0 + index * 8           ; 8-byte stride per fd slot
//   slot   = *slotVA                           ; slot[0] = guest CS pointer
//   if (slot == 0) {
//     if (sub_827AC358() == 0)                 ; one-shot table init helper
//       sub_827A15D8(17);                      ; raise(SIGTERM) → recurses
//                                              ;   back into sub_827AC498(8)
//   }
//   slot = *slotVA;                            ; reload after init
//   RtlEnterCriticalSection(slot);             ; ← lr=0x827AC4E0
//
// The init helper sub_827AC358 returns 0 every call in our build (its first
// callee sub_8246DF08 is one of many CRT-init pieces that never finishes
// wiring). So slot[index] stays NULL, slot[8] stays NULL, the SIGTERM path
// keeps re-entering sub_827AC498(8), and from outside it looks like a tight
// Enter/Leave spin on garbage CS pointers — except the iter-25a guard doesn't
// catch them because *slotVA is being reloaded after the kernel CS stub gets
// past the range check (caller passes whatever non-zero value the kernel
// happens to leave in r3).
//
// Fix: strong-override sub_827AC498 to populate slot[index] with a host-
// allocated 32-byte placeholder before delegating to the lifted body. The
// kernel's stub_RtlEnterCriticalSection treats any non-zero in-range pointer
// as a normal CS handle and constructs a real HostCriticalSection on first
// contact. Once slot[index] is non-zero, the lifted body's `if (slot==0)`
// branch is permanently skipped — no recurse-into-SIGTERM, no spin.
//
// We don't try to fix sub_827AC358 itself; doing so would require resurrecting
// the full CRT FILE table init (timezone DAT, locale DAT, and 7 more pieces
// — see iter-1's CRT inventory). The override is bounded: it only touches
// guest VAs in DAT_82946AE0..DAT_82946AE0+N*8 and only on slots where the
// guest itself reports 0.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>

extern "C" void __imp__sub_827AC498(PPCContext& __restrict ctx, uint8_t* base);

namespace {

constexpr uint32_t kSlotTableVA = 0x82946AE0u;
constexpr uint32_t kSlotStride  = 8u;

}  // namespace

// True if `va` looks like a real CS object pointer — landed in image+heap range.
// Anything else (zero, self-pointer like the slot table address, sentinel
// values like 0xFFFFFFFF, indices, etc.) means slot[index] has not been
// properly initialized for our purposes.
static inline bool isPlausibleCsPtr(uint32_t va) {
  return va >= 0x82000000u && va < 0xB4000000u && (va >> 24) != 0xFFu;
}

void sub_827AC498(PPCContext& __restrict ctx, uint8_t* base) {
  // FORCING: fabricates a critical-section object when the CRT's slot is unset.
  // Master-gated; the natural default runs the real recompiled init only.
  // iter-398e: SR_CRT_LOCK_FAB=0 knocks this out under the master gate — one
  // of three master-alone-active sites bisected for the deep-load rescue.
  static const bool s_on = [] {
    const char* e = std::getenv("SR_CRT_LOCK_FAB");
    return !e || e[0] != '0';
  }();
  if (!sr::kernel::forcingAllowed() || !s_on) { __imp__sub_827AC498(ctx, base); return; }
  const uint32_t index = ctx.r3.u32;

  // Defensive cap on index. Real slot table holds ~32 entries (one per stdio
  // file handle); indices much larger than that are caller bugs (e.g. an
  // unrelated function dispatched here through a bogus vtable). Just ack so
  // the caller's loop exits.
  if (index >= 64) {
    static std::atomic<uint32_t> s_oobCount{0};
    uint32_t n = s_oobCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 4 || (n & (n - 1)) == 0) {
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[CRT_FILE_LOCK_FIX] index %u out of range — no-op #%u", index, n);
    }
    ctx.r3.u32 = 0;  // STATUS_SUCCESS
    return;
  }

  const uint32_t slotVA = kSlotTableVA + index * kSlotStride;
  uint32_t cs = sr::kernel::readGuestU32BE(slotVA);

  if (!isPlausibleCsPtr(cs)) {
    uint32_t fakeCS = sr::kernel::guestHeapAlloc(32);
    if (fakeCS != 0) {
      if (uint8_t* h = sr::kernel::guestVAToHost(fakeCS)) std::memset(h, 0, 32);
      sr::kernel::writeGuestBE32(slotVA, fakeCS);

      static std::atomic<uint32_t> s_initCount{0};
      uint32_t n = s_initCount.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 8 || (n & (n - 1)) == 0) {
        SR_LOG_BOOT(sr::LogSeverity::Info,
          "[CRT_FILE_LOCK_FIX] init #%u slot[%u] @0x%08X (was 0x%08X) -> 0x%08X",
          n, index, slotVA, cs, fakeCS);
      }
    } else {
      static std::atomic<uint32_t> s_failCount{0};
      uint32_t n = s_failCount.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 4) {
        SR_LOG_BOOT(sr::LogSeverity::Warn,
          "[CRT_FILE_LOCK_FIX] guestHeapAlloc(32) failed for slot[%u]", index);
      }
      // Fallback: ack and return so the caller doesn't retry forever.
      ctx.r3.u32 = 0;
      return;
    }
  }

  __imp__sub_827AC498(ctx, base);
}
