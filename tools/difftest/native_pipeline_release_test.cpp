// Step 6 (Astra on 6af7487e, P2): the pipeline cache's test seams - setCreateGateForTest (a create gate that can
// hold or fail any create) and failInsertsForTest (an armable bad_alloc injector) - exist only where SR_ORACLE=1 (the
// dev build, difftest). This target compiles gfx/native_pipeline.hpp/.cpp as the release configuration does
// (SR_ORACLE=0, CMake SR_RELEASE_COMPOSITION=ON) and fails to BUILD if either seam is declared there.
#include "gfx/native_pipeline.hpp"
#include <cstdio>

static_assert(SR_ORACLE == 0, "sr_native_pipeline_release_test is the SR_ORACLE=0 (release configuration) build");

namespace {
template <class T> concept HasCreateGate = requires { T::setCreateGateForTest(nullptr); };
template <class T> concept HasCreateGateType = requires { typename T::CreateGateForTest; };
template <class T> concept HasFailInserts = requires { T::failInsertsForTest(1u); };
// I1 (Fable ruling 21 (2)): the mutant seam
template <class T> concept HasKeyComponents = requires { typename T::KeyComponentForTest; };
template <class T> concept HasDropComponent = requires { T::dropKeyComponentForTest(nullptr, 0u); };
}  // namespace

static_assert(!HasCreateGate<sr::gfx::NativePipelineCache>, "release: setCreateGateForTest must not exist");
static_assert(!HasCreateGateType<sr::gfx::NativePipelineCache>, "release: CreateGateForTest must not exist");
static_assert(!HasFailInserts<sr::gfx::NativePipelineCache>, "release: failInsertsForTest must not exist");
static_assert(!HasKeyComponents<sr::gfx::NativePipelineCache>, "release: keyComponentsForTest must not exist");
static_assert(!HasDropComponent<sr::gfx::NativePipelineCache>, "release: dropKeyComponentForTest must not exist");

int main() {
  // The shipping API is unchanged: the counters the backend reads are there (insertFailures included).
  sr::gfx::NativePipelineCache cache;
  const auto c = cache.counters();
  const bool ok = c.insertFailures == 0 && c.failures == 0 && c.borrowedKeyLookups == 0 && c.rootMemoHits == 0 &&
                  cache.size() == 0;
  std::printf("[NATIVE_PIPELINE_RELEASE_TEST] %s release_has_no_test_seams_and_keeps_the_counters\n",
              ok ? "PASS" : "FAIL");
  std::printf("[NATIVE_PIPELINE_RELEASE_TEST] passed=%u failed=%u\n", ok ? 1u : 0u, ok ? 0u : 1u);
  return ok ? 0 : 1;
}
