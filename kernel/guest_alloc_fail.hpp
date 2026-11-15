#pragma once

// step4 stuck-load lane (2026-10-20): ONE Warn line per failed guest allocation, kept by every log profile.
//
// The shim's allocators already log their failures ([XAM_ALLOC] ... FAILED, NtAllocateVirtualMemory FAILED, the
// [PHYS_ALLOC] census), but at Info: the player profile (SR_LOG=player - every release launch, every live roll)
// drops Info lines before formatting, so a guest that ran out of memory left no trace in a player log (lane D's
// fe_reload12c_b stuck load could not be checked for it). [GUEST_ALLOC_FAIL] is a kMilestoneTags tag (app/log.cpp):
// the per-call-site Warn cap (4) does not apply. The rate limit is this module's own: per API, failures 1-16 and
// then every power of two (#32, #64, ...), each line carrying that API's running count.
//
// LOG-ONLY: callers report a failure they already return; no allocator decision, return value or guest-visible
// state depends on this module.
//
//   [GUEST_ALLOC_FAIL] #<n> <api> size=0x.. flags=0x.. prot=0x.. lr=<guest lr> tid=<host tid> thread=<guest name>
//       handle=<guest thread handle> (<why>)

#include <cstdint>

namespace sr::kernel {

enum class GuestAllocApi : uint32_t {
  XamAlloc = 0,
  NtAllocateVirtualMemory,
  MmAllocatePhysicalMemoryEx,
  RtlAllocateHeap,
  kCount,
};

const char* guestAllocApiName(GuestAllocApi api);

// The rate rule: is the n-th failure (1-based) of one API logged?
bool guestAllocFailLogged(uint32_t n);

// Report one failed guest allocation (size = the guest's request, flags = the call's type / flags argument,
// prot = its protection argument or 0, why = a short reason). Returns true when the line was written to the log
// (subject to the rate rule; the count always advances).
bool noteGuestAllocFailure(GuestAllocApi api, uint32_t size, uint32_t flags, uint32_t prot, const char* why);

// Failures reported so far for one API (every failure, logged or not).
uint32_t guestAllocFailCount(GuestAllocApi api);

void resetGuestAllocFailForTest();

}  // namespace sr::kernel
