// D6 fixture: the gate's REAL default, and whether it guards the site at all.
// Round 1's fallback returned OWNGATE (opt-in) for the first getenv anywhere in
// the function, so guest_context.cpp initKernelDataStructures - one function
// with inline `static const bool s_x = [] { getenv(..) }()` switches, several
// default-ON, and UNGATED writes further down (the 0x8249AAC8 allocator
// "vtable" patch) - read "opt-in, default OFF" while it runs on every boot.
// Predicate defaults were read from a 500-character window that spilled into
// the next function (spillPred below borrowed nextPred's getenv).
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { void writeGuestBE32(uint32_t, uint32_t); uint32_t readGuestU32BE(uint32_t); } }
struct PPCContext { struct { uint32_t u32; } r3; };
extern bool g_flag;

bool spillPred() { return g_flag; }
bool nextPred() {
  const char* e = std::getenv("SR_FIXTURE_D6_NEXT");
  return e && e[0] == '1';
}

void initData(uint32_t va) {
  {
    static const bool s_on = [] {
      const char* e = std::getenv("SR_FIXTURE_D6_ON");
      return !e || e[0] != '0';
    }();
    static bool s_done = false;
    if (s_on && !s_done) {
      s_done = true;
      sr::kernel::writeGuestBE32(0x82000948u, 1u);  // EXPECT: OWNGATE_ON
    }
  }
  // UNSAFE-direction case: no gate at all (the allocator-vtable shape).
  uint32_t slot0 = sr::kernel::readGuestU32BE(0x8249AAC8u);
  if (slot0 == 0u) {
    sr::kernel::writeGuestBE32(0x8249AAC8u, 0x82000000u);  // EXPECT: UNGATED
  }
  // Control: a default-OFF inline switch is opt-in.
  {
    static const bool s_diag = [] {
      const char* e = std::getenv("SR_FIXTURE_D6_OFF");
      return e && e[0] == '1';
    }();
    if (s_diag) sr::kernel::writeGuestBE32(0x10u, 2u);  // EXPECT: OWNGATE
  }
  // A predicate that returns early when the env is unset (default ON).
  {
    static const bool s_early = [] {
      const char* e = std::getenv("SR_FIXTURE_D6_EARLY");
      if (!e) return true;
      return e[0] != '0';
    }();
    if (s_early) sr::kernel::writeGuestBE32(0x14u, 3u);  // EXPECT: OWNGATE_ON
    // Control: the NEGATED default-ON switch is the opt-in (=0) path.
    if (!s_early) sr::kernel::writeGuestBE32(0x18u, 4u);  // EXPECT: OWNGATE
  }
  // getenv read straight in the condition.
  if (const char* e = std::getenv("SR_FIXTURE_D6_DIRECT"); e && e[0] == '1')
    sr::kernel::writeGuestBE32(0x1Cu, 5u);  // EXPECT: OWNGATE
  if (!std::getenv("SR_FIXTURE_D6_KILL"))
    sr::kernel::writeGuestBE32(0x20u, 6u);  // EXPECT: OWNGATE_ON
  // UNSAFE-direction case: the predicate's default must come from its own body.
  if (spillPred()) sr::kernel::writeGuestBE32(va, 7u);  // EXPECT: UNGATED
}

void sub_82000060(PPCContext& ctx, uint8_t* base) { initData(ctx.r3.u32); }
