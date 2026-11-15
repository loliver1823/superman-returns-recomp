#pragma once

namespace sr {
namespace guest {

// Populate the production mappings before any guest thread runs. Also used by
// isolated SDK tests that need the actual indirect-call routing.
void initializeDispatchTable();

// Invoke guest entry point (sub_820F0000). Allocates guest memory, sets up
// PPCContext, populates dispatch table, and calls. Expect controlled crash
// on first missing kernel import or memory access.
bool runGuestHarness();

}  // namespace guest
}  // namespace sr
