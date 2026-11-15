// iter-455n Phase 2 v1 r3: the PRODUCTION texture-identity domain as a CAPABILITY
// BOUNDARY (Dwight r2 REWORK). The identity registry, its mutex, cap, counters,
// and the three Identity DomainOrderedLock sites + both registerTexIdentity calls
// live ENTIRELY in kernel/tex_identity_domain.cpp — a translation unit that does
// NOT declare, include, or otherwise see any behavior-domain symbol (s_texMap,
// kTexMapCap, s_texMu, TexRec). Therefore the three frozen structural mutations
// are COMPILE errors at the seam, not survivors:
//   (1) a Lock-late `s_texMap[tex] = TexRec{}` before registerTexIdentity   -> s_texMap/TexRec undeclared
//   (2) an identity registration using kTexMapCap instead of kTexIdentityCap -> kTexMapCap undeclared
//   (3) an Identity DomainOrderedLock using s_texMu instead of s_texIdentityMu-> s_texMu undeclared
// The CREATE/LOCK hooks (kernel/hle_api_observe.cpp) call these functions; the
// self-test drives the SAME functions against the SAME real registry (dependency
// injection of the actual production domain, not a duplicate).
#pragma once

#include "tex_identity_rule.hpp"
#include "sr_oracle.hpp"
#include <cstddef>
#include <cstdint>

namespace sr::kernel::texid::domain {

#if SR_ORACLE
// CREATE identity attempt for EVERY key (invalid keys included, §5/§6). VERIFIED
// create args. Returns the identity terminal (the CREATE hook ignores it).
IdOutcome registerCreate(uint32_t key, uint32_t w, uint32_t h, uint32_t fmt, uint32_t resType);

// LOCK-late identity attempt. The caller performs the ONE checked span read
// OUTSIDE any lock and passes the decoded admission here; admit => LockLate
// registration, else LockOutputInvalid. Returns the terminal.
IdOutcome registerLockLate(uint32_t key, AdmitResult admit);

// LOCK invalid-key attempt (key < 0x1000): InvalidKey, no read.
void noteLockInvalidKey();

// Inspection/self-test accessors — read/reset the REAL registry (not a stand-in).
IdentityCounters counters();
size_t registrySize();
void resetForTest();
#else
// SR_ORACLE=0 (kernel/sr_oracle.hpp): tex_identity_domain.cpp is compiled out (CMakeLists.txt ORACLE_SOURCES). The
// hooks call these only under SR_HLE_TEX_IDENTITY=1 and ignore the terminal; nothing is registered (Conflict is the
// domain's own fail-closed terminal). The inspection / self-test accessors left with tex_identity_test.cpp.
inline IdOutcome registerCreate(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { return IdOutcome::Conflict; }
inline IdOutcome registerLockLate(uint32_t, AdmitResult) { return IdOutcome::Conflict; }
inline void noteLockInvalidKey() {}
#endif

}  // namespace sr::kernel::texid::domain
