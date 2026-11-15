// iter-455n Phase 2 v1 r3: the PRODUCTION texture-identity domain (capability
// boundary — see tex_identity_domain.hpp). This TU OWNS the identity registry,
// its mutex, cap, and counters, and is the SOLE home of the three Identity
// DomainOrderedLock sites and both registerTexIdentity calls. It deliberately
// includes NOTHING that exposes the behavior domain (s_texMap / kTexMapCap /
// s_texMu / TexRec), so a forbidden reference to any of them from the identity
// seam fails to COMPILE — that is the retained contract that kills the three
// frozen structural mutants.
#include "tex_identity_domain.hpp"

#include <mutex>
#include <unordered_map>

namespace sr::kernel::texid::domain {
namespace {

// The real identity domain. Names match the frozen mutant targets so the exact
// mutations (kTexMapCap / s_texMu) reference undeclared behavior-domain symbols.
std::mutex s_texIdentityMu;
std::unordered_map<uint32_t, IdentityRecord> s_texIdentity;
IdentityCounters s_idCounters;
constexpr size_t kTexIdentityCap = 4096;   // INDEPENDENT identity cap

}  // namespace

IdOutcome registerCreate(uint32_t key, uint32_t w, uint32_t h, uint32_t fmt, uint32_t resType) {
    // Identity DomainOrderedLock site #1. Marker enters before the mutex; a
    // nested cross-domain attempt never acquires s_texIdentityMu (fail-fast).
    DomainOrderedLock<std::mutex> ig(LockDomain::Identity, s_texIdentityMu);
    if (!ig.locked()) return IdOutcome::Conflict;   // unreachable in the linear hook path
    return registerTexIdentity(s_texIdentity, s_idCounters, kTexIdentityCap, key,
        (uint8_t)IdOrigin::CreateHook, ResFamily::TextureLockRect,
        Verified(w, 0), Verified(h, 0), Verified(fmt, 0), Verified(resType, 0));
}

IdOutcome registerLockLate(uint32_t key, AdmitResult admit) {
    // Identity DomainOrderedLock site #2.
    DomainOrderedLock<std::mutex> ig(LockDomain::Identity, s_texIdentityMu);
    if (!ig.locked()) return IdOutcome::Conflict;   // unreachable in the linear hook path
    if (!admit.admit) {
        noteLockOutputInvalid(s_idCounters);
        return IdOutcome::LockOutputInvalid;
    }
    return registerTexIdentity(s_texIdentity, s_idCounters, kTexIdentityCap, key,
        (uint8_t)IdOrigin::LockLate, ResFamily::TextureLockRect,
        Unknown(), Unknown(), Unknown(), Unknown());
}

void noteLockInvalidKey() {
    // Identity DomainOrderedLock site #3.
    DomainOrderedLock<std::mutex> ig(LockDomain::Identity, s_texIdentityMu);
    if (ig.locked()) noteInvalidKeyLock(s_idCounters);
}

IdentityCounters counters() {
    std::lock_guard<std::mutex> lk(s_texIdentityMu);
    return s_idCounters;
}

size_t registrySize() {
    std::lock_guard<std::mutex> lk(s_texIdentityMu);
    return s_texIdentity.size();
}

void resetForTest() {
    std::lock_guard<std::mutex> lk(s_texIdentityMu);
    s_texIdentity.clear();
    s_idCounters = IdentityCounters{};
}

}  // namespace sr::kernel::texid::domain
