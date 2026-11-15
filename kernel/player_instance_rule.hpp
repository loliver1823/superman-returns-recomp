#pragma once

// Pure host launch policy shared by the player and the Oracle self-tests.
// Keep the existing namespace so the tests exercise the player's exact bodies.
namespace sr::kernel::texid {

constexpr bool anySelfTestModeActive(bool h7, bool iso, bool prov, bool wffsm,
                                     bool texUnlock, bool texIdentity) {
    return h7 || iso || prov || wffsm || texUnlock || texIdentity;
}

// Test processes and isolated instances do not claim the interactive player slot.
constexpr bool isPlayerInstanceLaunch(bool anySelfTestMode, bool hasInstanceId) {
    return !anySelfTestMode && !hasInstanceId;
}

}  // namespace sr::kernel::texid
