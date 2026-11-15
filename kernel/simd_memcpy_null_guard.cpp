// iter-37: NULL/garbage-arg guard on the SIMD memcpy sub_82468290.
//
// Background:
// iter-3 first identified sub_82468290 as "the SIMD memcpy with byte-loop
// fallback" — 16-byte vector copy when r5 (count) >= 16, scalar fallback
// otherwise. iter-23 caught it corrupting image .text/.rdata pages and
// patched the symptom with a VirtualProtect watchdog. iter-36's
// MODSTRUCT_WATCH caught it AGAIN — this time NULL-arg-memcpy is wiping the
// modA/modB heap structs (200 bytes each) at boot, leaving NULL vtables and
// blocking every per-frame module tick.
//
// Captured signal at the moment of the clobber (iter-36b log):
//   main lr=0x82468298 (memcpy body)
//   r3=0x00000000 (NULL dst)
//   r1=0xBE0FFC10 (main stack)
//   surrounding heap bytes -0x10..+0x80 ALL zero (wide memset)
//   call stack: memcpy <- 0x8230301C <- 0x82609A74 <- 0x82605094 <- 0x82602CC8
//               (FMUI hash insert chain, same iter-DOGMA-RE-16/18/19 family)
//
// Fix: strong-override sub_82468290. Reject calls where dst (r3) or src (r4)
// is NULL or outside the legal guest range; ack zero-count copies as no-op;
// otherwise delegate to the lifted body. Same risk class as iter-32's
// sub_827AC498 override — some path may legitimately memcpy(NULL,...) but
// nothing observed so far suggests it does.

#include "guest/ppc/ppc_recomp_shared.h"
#include "guest_context.hpp"
#include "ntavm_xenia_heap.hpp"
#include "../app/log.hpp"
#include <atomic>
#include <cstdlib>

extern "C" void __imp__sub_82468290(PPCContext& __restrict ctx, uint8_t* base);

namespace {

// Valid guest memory range: union of image, fallback heap, primary heap, phys
// regions. Same window iter-25a used for the garbage-CS filter. Combined with
// a top-byte sentinel check that catches sentinel values (0xFFFFFFFF, etc.)
// which alias into the range — see iter-32's stub_RtlEnterCriticalSection.
// iter-458 (Astra 066): SR_MEMCPY_GUARD_TOP_FIX=1 (default OFF). The legal guest
// range has ended at 0xBF000000 (kMinGuestSizeToCoverAllUsedVAs) since iter-173;
// this guard kept the old 0xB4000000 top and SILENTLY DROPPED every copy to or
// from live memory above it. Gate-OFF runs keep nothing live there (0 such
// rejects in the archive), but any physical free that reuses the top of the bump
// region does: every in-range reject in 91 archived logs comes from H-LIFO
// (hlifo1/2) or SR_PHYS_FREE (pf3b/c) runs - the ones whose FE froze.
std::atomic<int> g_topFixForce{-1};
bool topFixEnabled() {
  const int f = g_topFixForce.load(std::memory_order_relaxed);
  if (f >= 0) return f == 1;
  // iter-460: default ON with SR_PHYS_LIFO_FREE (the reclaimed pool lives at
  // 0xB0008000-0xBE00xxxx, above the stale 0xB4000000 top). =0 keeps the A/B.
  static const bool s_on = [] {
    const char* e = std::getenv("SR_MEMCPY_GUARD_TOP_FIX");
    return !e || e[0] != '0';
  }();
  return s_on;
}

inline bool isPlausibleGuestPtr(uint32_t va) {
  if (va == 0) return false;
  if ((va >> 24) == 0xFFu) return false;            // 0xFFxxxxxx sentinels
  uint32_t aliased = sr::kernel::aliasGuestVA(va);
  const uint32_t top = topFixEnabled()
      ? static_cast<uint32_t>(sr::kernel::kMinGuestSizeToCoverAllUsedVAs)
      : 0xB4000000u;
  if (sr::kernel::xheap::enabled() && sr::kernel::xheap::owns(va)) return true;   // iter-463
  return aliased >= 0x82000000u && aliased < top;
}

// Sane upper bound on a single memcpy call. Real game copies (vertex buffers,
// audio frames, file I/O scratch) all fit comfortably below 16 MB. A larger
// count is more likely a recomp register-overflow bug than a real intent.
constexpr uint32_t kSaneMaxBytes = 16u * 1024u * 1024u;

}  // namespace

void sub_82468290(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t dst   = ctx.r3.u32;
  const uint32_t src   = ctx.r4.u32;
  const uint32_t count = ctx.r5.u32;

  // (iter-463 burndown: the count==0 shortcut that returned dst without the
  // guest body is gone - memcpy(*, *, 0) is the guest's own no-op; its body
  // returns dst itself.)
  if (count == 0) {
    __imp__sub_82468290(ctx, base);
    return;
  }

  // iter-463 charter (burndown U1): the reject skipped the guest memcpy and
  // returned dst with no gate. "[MEMCPY_GUARD]" is in 3/81 run logs before the
  // heap-corruption fix 60a90bf and 0/69 after. Master-gated: the default exe
  // logs the shape and runs the guest's own copy.
  if (!isPlausibleGuestPtr(dst) || !isPlausibleGuestPtr(src) || count > kSaneMaxBytes) {
    static std::atomic<uint64_t> s_rejects{0};
    uint64_t n = s_rejects.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool reject = sr::kernel::forcingAllowed();
    if (n <= 8 || (n & (n - 1)) == 0) {
      uint32_t lr = sr::kernel::getCurrentGuestLR();
      SR_LOG_BOOT(sr::LogSeverity::Warn,
        "[MEMCPY_GUARD] %s #%llu dst=0x%08X src=0x%08X count=0x%X caller_lr=0x%08X",
        reject ? "reject" : "OBSERVED (not rejected: SR_ALLOW_FORCING off)",
        (unsigned long long)n, dst, src, count, lr);
    }
    if (reject) {
      ctx.r3.u32 = dst;
      return;
    }
  }

  __imp__sub_82468290(ctx, base);
}

namespace sr { namespace kernel {
void setMemcpyGuardTopFixForTest(int mode) { g_topFixForce.store(mode, std::memory_order_relaxed); }
} }
