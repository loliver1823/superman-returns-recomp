// D13 fixture: a bool flag's initializer is not its value when something else
// can set it (round-3 verifier probe tree v3/p4 Q1 / Q2 / Q7): both read
// "observe-only (dead)" = OWNGATE, while the write runs in the default exe.
#include <cstdint>
namespace sr { namespace kernel { void writeGuestBE32(uint32_t, uint32_t); } }

// UNSAFE-direction case Q1: a global initialised false HERE, set true from
// ANOTHER file (b_boot.cpp) at boot.
bool g_d13Fab = false;
void q1(uint32_t va) {
  if (g_d13Fab) sr::kernel::writeGuestBE32(va, 1u);  // EXPECT: UNGATED
}
// UNSAFE-direction case Q7: the same through an early return.
bool g_d13Q7 = false;
void q7(uint32_t va) {
  if (!g_d13Q7) return;
  sr::kernel::writeGuestBE32(va, 7u);  // EXPECT: UNGATED
}
// UNSAFE-direction case Q2: a local flag set through a by-reference argument.
static void setOn(bool& b) { b = true; }
void q2(uint32_t va) {
  bool on = false;
  setOn(on);
  if (on) sr::kernel::writeGuestBE32(va, 2u);  // EXPECT: UNGATED
}

// Controls (stay dead): a file-local (static) flag no other file can name, and
// a flag passed BY VALUE / by const reference.
static bool s_d13Local = false;
static void show(bool v) { (void)v; }
static void peek(const bool& v) { (void)v; }
void c1(uint32_t va) {
  if (s_d13Local) sr::kernel::writeGuestBE32(va, 3u);  // EXPECT: OWNGATE
}
void c2(uint32_t va) {
  bool off = false;
  show(off);
  peek(off);
  if (off) sr::kernel::writeGuestBE32(va, 4u);  // EXPECT: OWNGATE
}
int SDL_main(int, char**) { q1(0x10); q7(0x14); q2(0x18); c1(0x1C); c2(0x20); return 0; }
