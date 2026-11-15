// The HITCH recorder (kernel/hitch_log.cpp) for tools/difftest/shader_xlat_test_run.py's standalone
// test: gfx/shader_runtime_xlat.cpp times each translation with sr::hitch::Scope (iter-466), and the
// real recorder drags in the profiler and the frame clock. The game's default (SR_HITCH_LOG unset) is
// g_on = false, under which Scope never reads a clock or calls noteSlow; this is that state, fixed.
#include "kernel/hitch_log.hpp"

namespace sr::hitch {
bool g_on = false;
Slot g_slot[kCount];
uint64_t qpcNow() { return 0; }
void noteSlow(Ev, uint64_t, uint64_t) {}
}  // namespace sr::hitch
