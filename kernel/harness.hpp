#pragma once

namespace sr {
namespace kernel {

// Run kernel export registry harness: registers stubs, calls a few, logs output.
// Returns true if all looked-up exports were found.
bool runHarness();

}  // namespace kernel
}  // namespace sr
