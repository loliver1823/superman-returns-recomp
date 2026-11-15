#pragma once
// Step 6 I8 (joint plan I8(a), Fable ruling 21 (3)): RtlInitializeCriticalSection's re-init counts.
// SR_CS_REINIT_REUSE (default ON; =0 the old path): a re-init of a guest CS whose host object nobody owns or waits on
// keeps that object (reset under its own lock); a held or waited-on one keeps the old path (a new object, the old one
// to the graveyard), logged as [CS_REINIT_HELD]. Logged as [CS_INIT] milestone lines (kernel/stubs.cpp).
#include "sr_oracle.hpp"
#include <cstddef>
#include <cstdint>

namespace sr::kernel {

struct CsInitCounts {
  uint64_t all = 0;        // RtlInitializeCriticalSection calls on a non-null CS
  uint64_t first = 0;      // the CS had no host object yet
  uint64_t reused = 0;     // a re-init that kept the object (nobody owned or waited on it)
  uint64_t held = 0;       // a re-init of an owned / waited-on object: the old path, logged
  uint64_t replaced = 0;   // a re-init with SR_CS_REINIT_REUSE=0: the old path
  size_t graveyard = 0;    // host objects kept alive by the old path (SR_CS_FASTMAP)
};
// first + reused + held + replaced == all. Takes the CS map's lock.
CsInitCounts csInitCounts();

#if SR_ORACLE
// Tests only: the host object a guest CS maps to now (0 = none).
uintptr_t csHostIdentityForTest(uint32_t guestCs);
#endif

}  // namespace sr::kernel
