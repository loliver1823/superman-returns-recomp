// D6 fixture, part 2: the gate forms the real tree uses, each read for its real
// default. Every function is reached from the guest-called override at the end.
#include <atomic>
#include <cstdint>
#include <cstdlib>
namespace sr { namespace kernel { bool forcingAllowed(); void writeGuestBE32(uint32_t, uint32_t); } }
struct PPCContext { struct { uint32_t u32; } r3; };
void logit();

// A helper that takes the env NAME (stubs.cpp srGateDefaultOn, kpcr.cpp envOn).
bool envOn(const char* name) {
  const char* e = std::getenv(name);
  return !e || e[0] != '0';
}

// A test-override hook in front of the env switch (kpcrEnabled,
// obKThreadEnabled): -1 = "use the env", stored only by *ForTest().
std::atomic<int> g_fixTestMode{-1};
void fixSetEnabledForTest(int mode) { g_fixTestMode.store(mode); }
bool fixEnabled() {
  const int m = g_fixTestMode.load();
  if (m >= 0) return m != 0;
  static const bool v = envOn("SR_FIXTURE_D6_HOOKED");
  return v;
}

// UNSAFE-direction control: the same shape, but production code stores the
// hook, so its default is unknown and the gate is not trusted.
std::atomic<int> g_liveMode{-1};
void setLiveMode(int m) { g_liveMode.store(m); }
bool liveEnabled() {
  const int m = g_liveMode.load();
  if (m >= 0) return m != 0;
  static const bool v = envOn("SR_FIXTURE_D6_LIVE");
  return v;
}

bool killOn() {
  const char* e = std::getenv("SR_FIXTURE_D6_KILLSW");
  return e && e[0] == '1';
}

void forms(uint32_t va, uint32_t count) {
  if (envOn("SR_FIXTURE_D6_PARAM")) sr::kernel::writeGuestBE32(va, 1u);  // EXPECT: OWNGATE_ON
  if (fixEnabled()) sr::kernel::writeGuestBE32(va, 2u);  // EXPECT: OWNGATE_ON
  if (liveEnabled()) sr::kernel::writeGuestBE32(va, 3u);  // EXPECT: UNGATED

  // Flow-sensitive local flags (cs_timeout_underflow_fix.cpp sub_8258C278,
  // apt_as_watch.cpp auto-frame restore).
  bool kill = false;
  if (count > 0x10000u) kill = true;
  if (kill && !killOn()) {
    logit();
    kill = false;
  }
  if (kill) sr::kernel::writeGuestBE32(va, 4u);  // EXPECT: OWNGATE

  bool framed = false;
  if (sr::kernel::forcingAllowed()) {
    if (count == 0) framed = true;
  }
  logit();
  if (framed) sr::kernel::writeGuestBE32(va, 5u);  // EXPECT: MASTER

  // UNSAFE-direction controls: the definition that reaches the use decides.
  bool ok = false;
  if (sr::kernel::forcingAllowed()) ok = true;
  ok = true;
  if (ok) sr::kernel::writeGuestBE32(va, 6u);  // EXPECT: UNGATED

  bool partial = false;
  if (count > 4u) partial = true;
  if (partial && !killOn()) {
    if (count & 1u) partial = false;           // not an unconditional reset
  }
  if (partial) sr::kernel::writeGuestBE32(va, 7u);  // EXPECT: UNGATED

  bool armed = false;
  for (uint32_t i = 0; i < count; ++i) {
    if (armed) sr::kernel::writeGuestBE32(va, 8u);  // EXPECT: UNGATED
    armed = true;                                   // loop-carried: reaches the use
  }
}

void sub_82000064(PPCContext& ctx, uint8_t* base) { forms(ctx.r3.u32, 0); setLiveMode(1); }
