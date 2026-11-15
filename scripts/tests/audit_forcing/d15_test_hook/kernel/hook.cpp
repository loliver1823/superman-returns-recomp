// D15 fixture (round-3 verifier probe tree v3/p4 Q6): a `-1 = use the env`
// test-override hook was trusted to hold -1 in the default exe because its only
// store sits in a function whose NAME says Test - even when production code
// calls that setter.
#include <atomic>
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { void writeGuestBE32(uint32_t, uint32_t); } }

// UNSAFE-direction case: production (SDL_main) forces the hook ON.
std::atomic<int> g_d15TestMode{-1};
void d15SetEnabledForTest(int v) { g_d15TestMode.store(v); }
bool d15Enabled() {
  const int m = g_d15TestMode.load();
  if (m >= 0) return m != 0;
  const char* e = std::getenv("SR_FIXTURE_D15");
  return e && e[0] == '1';
}
void d15(uint32_t va) {
  if (d15Enabled()) sr::kernel::writeGuestBE32(va, 6u);  // EXPECT: UNGATED
}

// Control: a hook whose setter nothing in the production tree calls keeps its
// -1 sentinel, so the env switch (opt-in) decides.
std::atomic<int> g_d15bTestMode{-1};
void d15bSetEnabledForTest(int v) { g_d15bTestMode.store(v); }
bool d15bEnabled() {
  const int m = g_d15bTestMode.load();
  if (m >= 0) return m != 0;
  const char* e = std::getenv("SR_FIXTURE_D15B");
  return e && e[0] == '1';
}
void d15b(uint32_t va) {
  if (d15bEnabled()) sr::kernel::writeGuestBE32(va, 7u);  // EXPECT: OWNGATE
}
int SDL_main(int, char**) { d15SetEnabledForTest(1); d15(0x1C); d15b(0x20); return 0; }
