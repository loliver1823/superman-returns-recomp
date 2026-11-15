// iter-455l retirement (director D1 adjudication): the NAMED FORCING ISOLATION
// infrastructure (guest_context: forcingAllowed / forcingAllowedFor /
// isolationAllowsOrdinary / isolationAllowsNamed, plus the audit_forcing.py isolation
// report) is RETAINED after the retired iter-455l diagnostic gate's deletion, on the director's
// condition that it stays tested and documented INDEPENDENTLY of any particular gate.
// This file is that independent test: SR_ISOLATION_TEST=1 turns the exe into a pure
// host-side runner for the isolation semantics matrix and exits (exit code = failure
// count). The matrix is verbatim the suite that validated the mechanism while the retired
// gate existed (then part of its 123-assert suite; the iso_ordinary/iso_named mutants
// are still killed by exactly these assertions).
//
// WHY THIS EXISTS (the T1 lesson, 2026-08-13): raising SR_ALLOW_FORCING=1 bare
// resurrects every master-gated sub-gate that defaults ON (the legacy forcing stack —
// FE drive threads, texture-upload skip, movie name substitution …), which destroyed
// the T1 acquisition. SR_FORCING_ISOLATE=<gate-name> makes a diagnostic run
// "natural + exactly one named gate": every ordinary forcingAllowed() caller is denied
// and only the byte-exact named forcingAllowedFor(<name>) is authorizable. Unknown,
// malformed, empty, case-different, or whitespace-bearing values fail closed.
// There is currently NO named exception in use (audit ISOLATION_EXCEPTIONS is empty);
// the next named diagnostic gate re-adds its literal there, uses
// forcingAllowedFor("SR_<NAME>"), and inherits this tested mechanism.

#include "guest_context.hpp"
#include "../app/log.hpp"

#include <cstdlib>

namespace sr::kernel::isolation_test {

static int run()
{
    int pass = 0, fail = 0;
    auto CHECK = [&](bool ok, const char* what) {
        if (ok) ++pass;
        else { ++fail; SR_LOG_BOOT(sr::LogSeverity::Error, "[ISO_TEST] FAIL: %s", what); }
    };

    using sr::kernel::isolationAllowsOrdinary;
    using sr::kernel::isolationAllowsNamed;
    const char* G = "SR_TEST_NAMED_GATE";  // a NEUTRAL fixture name (director order:
                                           // the retired gate's identifier must not
                                           // survive anywhere in the tree)
    CHECK(!isolationAllowsOrdinary(false, nullptr) &&
          !isolationAllowsNamed(false, nullptr, G) &&
          !isolationAllowsNamed(false, G, G),
          "master off denies everything");
    CHECK(isolationAllowsOrdinary(true, nullptr) &&
          isolationAllowsNamed(true, nullptr, G),
          "master on + isolation unset preserves legacy behavior for both");
    CHECK(!isolationAllowsOrdinary(true, G),
          "isolation denies every ordinary forcing caller");
    CHECK(isolationAllowsNamed(true, G, G),
          "the exact-match named gate is allowed under isolation");
    CHECK(!isolationAllowsNamed(true, G, "SR_FAKE_SPLASH"),
          "another named gate is denied under isolation");
    CHECK(!isolationAllowsNamed(true, "SR_TEST_NAMED_GATE ", G),
          "trailing-space isolation value fails closed");
    CHECK(!isolationAllowsNamed(true, "sr_test_named_gate", G),
          "case-different isolation value fails closed");
    CHECK(!isolationAllowsOrdinary(true, "") &&
          !isolationAllowsNamed(true, "", G),
          "empty isolation value fails closed everywhere");
    CHECK(!isolationAllowsNamed(true, "SR_BOGUS_GATE", G),
          "unknown isolation name matches nothing");
    CHECK(!isolationAllowsNamed(true, G, nullptr) &&
          !isolationAllowsNamed(true, G, ""),
          "null/empty gate name is denied");

    const int total = pass + fail;
    SR_LOG_BOOT(fail ? sr::LogSeverity::Error : sr::LogSeverity::Info,
                "[ISO_TEST] %d/%d passed", pass, total);
    return fail;
}

int runSelfTestIfRequested()
{
    const char* e = std::getenv("SR_ISOLATION_TEST");
    if (!e || e[0] != '1') return -1;
    return run();
}

}  // namespace sr::kernel::isolation_test
