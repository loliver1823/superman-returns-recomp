// iter-463 GPU_HANG_WD (kernel/gpu_hang_wd.{hpp,cpp}): the trigger logic as a
// pure function over a snapshot, the episode rule, and the latches the
// watchdog reads (fence-word match, waiter table, IB ring + its 0x58 walk).
//
// Run: build/difftest.exe --gpu-hang-wd-test   (exit code = failed checks)
//
// The bug state (Kryptonian-arena freeze): D3DDeviceSwap in BlockOnFence for
// a fence the CP never wrote, the CP idle and drained because it parsed the
// IB from the contested 0x8 window (file data). H0 must fire on that state and
// on nothing a healthy frame produces.
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "kernel/gpu_hang_wd.hpp"
#include "kernel/guest_context.hpp"
#include "kernel/host_work.hpp"

PPC_EXTERN_FUNC(sub_820FD9A8);   // GpuBlockOnFence - resolves to the observe strong-def

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

namespace K = sr::kernel;

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}

constexpr uint64_t kS = 1000000000ull;

// The freeze, as the watchdog would see it 1.2 s into the wait.
K::GpuHangSnap hungSnap() {
  K::GpuHangSnap s;
  s.nowNs = 100 * kS;
  s.cpStarted = true;
  s.cpPhase = 0;
  s.cpPhaseSinceNs = s.nowNs - kS / 2;   // idle, but not yet 5 s (H3 quiet)
  s.published = true;
  s.rptr = s.wptr = 0x1234;
  s.fenceValid = true;
  s.completed = 0x100;
  s.completedSinceNs = s.nowNs - kS;     // H1 quiet (< 3 s)
  s.issuedValid = true;
  s.lastIssued = 0x102;
  s.fenceWaiter = true;
  s.fenceWaiterEntryNs = s.nowNs - kS - kS / 5;
  s.fenceWaiterTarget = 0x102;
  s.anyWaiter = true;
  s.anyWaiterEntryNs = s.fenceWaiterEntryNs;
  s.presents = 5000;
  s.presentsSinceNs = s.nowNs - kS;      // H2 quiet (< 10 s)
  s.threadInGpuWait = true;
  return s;
}

// A healthy frame: the CP busy in an IB, a fresh waiter, fences moving.
K::GpuHangSnap healthySnap() {
  K::GpuHangSnap s = hungSnap();
  s.cpPhase = 2;
  s.cpPhaseSinceNs = s.nowNs - kS / 100;
  s.rptr = 0x1200;
  s.fenceWaiterEntryNs = s.anyWaiterEntryNs = s.nowNs - kS / 200;
  s.completedSinceNs = s.nowNs - kS / 60;
  s.presentsSinceNs = s.nowNs - kS / 60;
  return s;
}

void testTriggers() {
  std::printf("-- triggers (pure) --\n");
  check(K::gpuHangEvaluate(healthySnap()) == 0, "T0 healthy frame: no trigger");
  check(K::gpuHangEvaluate(hungSnap()) == K::kGhH0, "T1 freeze state: H0 alone (1.2 s wait, CP idle+drained, fence ahead)");

  K::GpuHangSnap s = hungSnap();
  s.fenceWaiterEntryNs = s.nowNs - (kS - 1);
  check(!(K::gpuHangEvaluate(s) & K::kGhH0), "T2 H0 needs >= 1.0 s in BlockOnFence");
  s = hungSnap();
  s.fenceWaiterTarget = s.completed;
  check(!(K::gpuHangEvaluate(s) & K::kGhH0), "T3a H0 quiet when target == completed");
  s = hungSnap();
  s.completed = 0xFFFFFFFEu; s.fenceWaiterTarget = 0x00000001u;
  check((K::gpuHangEvaluate(s) & K::kGhH0) != 0, "T3b H0 fence compare is wrap-safe (target 1 ahead of FFFFFFFE)");
  s = hungSnap();
  s.completed = 0x00000001u; s.fenceWaiterTarget = 0xFFFFFFFEu;
  check(!(K::gpuHangEvaluate(s) & K::kGhH0), "T3c H0 quiet when target FFFFFFFE is behind completed 1");
  s = hungSnap();
  s.cpPhase = 2;
  check(!(K::gpuHangEvaluate(s) & K::kGhH0), "T4a H0 quiet while the CP executes an IB");
  s = hungSnap();
  s.rptr = s.wptr - 4;
  check(!(K::gpuHangEvaluate(s) & K::kGhH0), "T4b H0 quiet while ring data is queued (rptr != wptr)");
  s = hungSnap();
  s.fenceValid = false;
  check(!(K::gpuHangEvaluate(s) & K::kGhH0), "T4c H0 needs a readable fence word");
  s = hungSnap();
  s.cpStarted = false;
  check(!(K::gpuHangEvaluate(s) & K::kGhH0), "T4d H0 needs the CP started");

  s = hungSnap();
  s.fenceWaiter = false; s.anyWaiter = false;
  s.completedSinceNs = s.nowNs - 3 * kS;
  check(K::gpuHangEvaluate(s) == K::kGhH1, "T5a H1 completed flat 3.0 s with a later fence issued");
  s.completedSinceNs = s.nowNs - 3 * kS + 1;
  check(!(K::gpuHangEvaluate(s) & K::kGhH1), "T5b H1 quiet at 2.99 s");
  s.completedSinceNs = s.nowNs - 30 * kS;
  s.lastIssued = s.completed;
  check(!(K::gpuHangEvaluate(s) & K::kGhH1), "T5c H1 quiet when nothing later is issued (GPU drained)");
  s.lastIssued = s.completed + 2; s.issuedValid = false;
  check(!(K::gpuHangEvaluate(s) & K::kGhH1), "T5d H1 needs the device's issued fence");

  s = hungSnap();
  s.fenceWaiter = false; s.anyWaiter = false;
  s.presentsSinceNs = s.nowNs - 10 * kS;
  check(K::gpuHangEvaluate(s) == K::kGhH2, "T6a H2 presents flat 10 s + a thread in a GPU wait");
  s.threadInGpuWait = false;
  check(!(K::gpuHangEvaluate(s) & K::kGhH2), "T6b H2 quiet with no thread in a GPU wait");
  s.threadInGpuWait = true;
  s.presentsSinceNs = s.nowNs - 10 * kS + 1;
  check(!(K::gpuHangEvaluate(s) & K::kGhH2), "T6c H2 quiet at 9.99 s");
  s.presentsSinceNs = s.nowNs - 20 * kS;
  s.presents = 0;
  check(!(K::gpuHangEvaluate(s) & K::kGhH2), "T6d H2 is armed only after the first present (boot)");

  s = healthySnap();
  s.cpPhaseSinceNs = s.nowNs - 5 * kS;
  check(K::gpuHangEvaluate(s) == K::kGhH3, "T7a H3 a non-idle CP phase for 5 s");
  s = hungSnap();
  s.fenceWaiter = false; s.anyWaiter = false;
  s.cpPhaseSinceNs = s.nowNs - 60 * kS;
  check(K::gpuHangEvaluate(s) == 0, "T7b H3 quiet: idle, drained, no waiter (guest-side quiet)");
  s.rptr = s.wptr - 8;
  check(K::gpuHangEvaluate(s) == K::kGhH3, "T7c H3 idle with queued ring data");
  s = hungSnap();
  s.fenceWaiter = false;   // a ring-space waiter only (H0 cannot fire)
  s.cpPhaseSinceNs = s.nowNs - 60 * kS;
  s.anyWaiterEntryNs = s.nowNs - kS;
  check(K::gpuHangEvaluate(s) == K::kGhH3, "T7d H3 idle+drained under a waiter >= 1 s (the new early-return)");
  s.anyWaiterEntryNs = s.nowNs - kS / 2;
  check(K::gpuHangEvaluate(s) == 0, "T7e H3 quiet for a waiter that just entered (0.5 s)");
  s = hungSnap();
  s.cpPhaseSinceNs = s.nowNs - 60 * kS;
  check(K::gpuHangEvaluate(s) == (K::kGhH0 | K::kGhH3), "T7f the freeze after 60 s idle: H0 + H3");

  s = hungSnap();
  s.fenceWaiter = false; s.anyWaiter = false;
  s.cpPhaseSinceNs = 0;
  s.rptr = s.wptr - 8;
  check(!(K::gpuHangEvaluate(s) & K::kGhH3), "T7g H3 quiet before the CP's first phase (phaseSinceNs 0)");

  K::GpuHangSnap z;
  check(K::gpuHangEvaluate(z) == 0, "T8 an all-zero snapshot (watchdog before any latch): quiet");

  // H2's persistence: one sampled LR is not a wait.
  uint64_t since = 0;
  check(!K::gpuHangGpuWaitPersists(since, true, 100 * kS), "P1 the first sample does not count");
  check(!K::gpuHangGpuWaitPersists(since, true, 100 * kS + kS - 1), "P2 0.99 s of consecutive samples: not yet");
  check(K::gpuHangGpuWaitPersists(since, true, 101 * kS), "P3 1.0 s of consecutive samples: a wait");
  check(!K::gpuHangGpuWaitPersists(since, false, 101 * kS + kS / 4) && since == 0 &&
            !K::gpuHangGpuWaitPersists(since, true, 102 * kS),
        "P4 one sample without it restarts the clock");

  // Episode keys: H0/H1 by the completed fence, H2/H3 alone by the present count.
  check(K::gpuHangEpisodeKey(K::kGhH0 | K::kGhH2, 0x100, 77) == 0x100u, "K1 H0 (+H2): keyed by completed");
  check(K::gpuHangEpisodeKey(K::kGhH1, 0x104, 77) == 0x104u, "K2 H1: keyed by completed");
  check(K::gpuHangEpisodeKey(K::kGhH2 | K::kGhH3, 0x108, 77) == 77u,
        "K3 H2/H3 alone: keyed by presents (a moving fence is one episode)");

  // The hang dump: H0/H1/H2, never H3 alone.
  check(!K::gpuHangWantsDump(K::kGhH3), "D1 H3 alone (a boot CP stall) does not spend the hang dump");
  check(K::gpuHangWantsDump(K::kGhH0) && K::gpuHangWantsDump(K::kGhH1) &&
            K::gpuHangWantsDump(K::kGhH2 | K::kGhH3),
        "D2 H0, H1, H2 (with or without H3) take it");
}

// The observe strong-def of GpuBlockOnFence, called as the guest calls it:
// the fence already passed, so the real body returns at once.
void testStrongDef() {
  std::printf("-- observe strong-def (sub_820FD9A8) --\n");
  constexpr uint32_t kDev = 0x70400000u, kFence = 0x70410000u, kStack = 0x70480000u;
  K::writeGuestBE32(kDev + 0x2A1Cu, 0x10u);    // next fence
  K::writeGuestBE32(kDev + 0x2A10u, kFence);   // the wait VA
  K::writeGuestBE32(kFence, 0x10u);            // completed
  const K::GpuHangWaitForTest w0 = K::gpuHangWdWaitForTest(K::kGwBlockOnFence);
  PPCContext c{};
  c.r1.u32 = kStack;
  c.r14.u32 = 0x829C0000u;
  c.r3.u32 = kDev;
  c.r4.u32 = 0x08u;   // target: already completed
  c.r5.u32 = 0x3u;
  c.lr = 0x82101234u;
  sub_820FD9A8(c, K::getGuestBase());
  const K::GpuHangWaitForTest w1 = K::gpuHangWdWaitForTest(K::kGwBlockOnFence);
  check(w1.enters == w0.enters + 1 && w1.lastKind == K::kGwBlockOnFence && w1.dev == kDev &&
            w1.a == 0x08u && w1.b == 0x3u,
        "S1 the guest's call enters the waiter table with {dev r3, target r4, mode r5}");
  check(w1.occupied == 0, "S2 ...and leaves it when the real body returns");
}

void testLrSet() {
  std::printf("-- GPU-wait LR set --\n");
  check(K::gpuHangLrInGpuWait(0x820F33E8u), "L1 GpuRingWaitProgress entry is in");
  check(K::gpuHangLrInGpuWait(0x820F347Cu), "L2 last word before 0x820F3480 is in");
  check(!K::gpuHangLrInGpuWait(0x820F3480u), "L3 0x820F3480 is out");
  check(K::gpuHangLrInGpuWait(0x820FDA44u), "L4 BlockOnFence loop return address is in");
  check(!K::gpuHangLrInGpuWait(0x820FDA60u), "L5 0x820FDA60 is out");
  check(!K::gpuHangLrInGpuWait(0x820FD9A4u), "L6 0x820FD9A4 is out");
}

void testEpisode() {
  std::printf("-- episodes --\n");
  K::GpuHangEpisode e;
  check(K::gpuHangEpisodeShouldLog(e, 0x100, 10 * kS), "E1 first fire logs");
  check(!K::gpuHangEpisodeShouldLog(e, 0x100, 11 * kS), "E2 same completed value +1 s: quiet");
  check(!K::gpuHangEpisodeShouldLog(e, 0x100, 39 * kS), "E3 +29 s: quiet");
  check(K::gpuHangEpisodeShouldLog(e, 0x100, 40 * kS), "E4 +30 s: re-log");
  check(!K::gpuHangEpisodeShouldLog(e, 0x100, 70 * kS), "E5 +60 s: quiet");
  check(K::gpuHangEpisodeShouldLog(e, 0x100, 130 * kS), "E6 +120 s: re-log");
  check(!K::gpuHangEpisodeShouldLog(e, 0x100, 500 * kS), "E7 later: quiet for good");
  check(K::gpuHangEpisodeShouldLog(e, 0x104, 501 * kS), "E8 a new completed value = a new episode");
}

void testLatches(bool deep) {
  std::printf("-- latches (deep per-IB fields %s) --\n", deep ? "ON" : "off");
  // Fence block phys 0x1E829000 (the arena run's fence block), rptrWb = +0x3C.
  K::gpuHangWdNoteRptrWriteback(0x1E82903Cu);
  check(K::gpuHangWdIsFence(0xBE829000u), "F1 fenceP = rptrWb - 0x3C (0xA view)");
  check(K::gpuHangWdIsFence(0x1E829002u), "F2 endian-select bits ignored");
  check(!K::gpuHangWdIsFence(0x1E829004u), "F3 the next word is not the fence");

  int slots[17];
  int got = 0;
  for (int i = 0; i < 17; i++) {
    slots[i] = K::gpuHangWdWaitEnter(K::kGwBlockOnFence, 0x839F2880u, 0x102u, 0, 0x82110000u,
                                     0x70000000u);
    if (slots[i] >= 0) got++;
  }
  check(got == 16 && slots[16] == -1, "W1 16 waiter slots, the 17th overflows");
  for (int i = 0; i < 17; i++) K::gpuHangWdWaitLeave(slots[i]);
  const int again = K::gpuHangWdWaitEnter(K::kGwRingSpace, 0x839F2880u, 0, 0, 0, 0);
  check(again >= 0, "W2 Leave frees the slots");
  K::gpuHangWdWaitLeave(again);

  // An IB at 0xBE82B000 whose second packet is the frame's fence write
  // (EVENT_WRITE_SHD to the fence word, value 0x1234), and different bytes at
  // its 0x8 alias - the double-booked shape.
  uint8_t* ib = K::guestVAToHost(0xBE82B000u);
  uint8_t* alias = K::guestVAToHost(0x9E82B000u);
  const uint32_t words[] = {
      0xC0012D00u, 0x00002000u, 0x00000000u,                 // SET_CONSTANT (2 body dwords)
      0xC0025800u, 0x00000006u, 0x1E829000u, 0x00001234u,     // EVENT_WRITE_SHD -> fence
      0xC0025800u, 0x00000006u, 0x1E829100u, 0x00005555u,     // EVENT_WRITE_SHD elsewhere
  };
  const uint32_t n = sizeof(words) / 4;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t w = words[i];
    const uint8_t be[4] = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w)};
    std::memcpy(ib + i * 4, be, 4);
  }
  std::memset(alias, 0x5A, n * 4);
  K::gpuHangWdNoteIb(0x1E82B000u, n, 0xA0000000u, ib);
  K::GpuHangIbForTest r{};
  const bool have = K::gpuHangWdLastIbForTest(&r);
  check(have && r.phys == 0x1E82B000u && r.dwords == n && r.win == 0xA0000000u,
        "I1 IB ring records phys/dwords/window");
  if (deep) {
    check(have && r.used0 == 0xC0012D00u && r.other0 == 0x5A5A5A5Au && (r.readable & 3) == 3,
          "I2 first dword through BOTH windows (used vs the other alias)");
    check(have && r.n58 == 2 && r.fenceHit == 1 && r.fenceVal == 0x1234u,
          "I3 0x58 count and the fence-targeting write (value 0x1234)");
    K::gpuHangWdNoteIb(0x1E82B000u, n, 0x80000000u, alias);
    const bool have2 = K::gpuHangWdLastIbForTest(&r);
    check(have2 && r.n58 == 0 && r.fenceHit == 0 && r.used0 == 0x5A5A5A5Au && r.other0 == 0xC0012D00u,
          "I4 the same IB read from the 0x8 alias: no fence write (the freeze mechanism)");
  } else {
    // the default record (SR_GPU_HANG_WD_DEEP unset in a game / =0 here): phys / dwords / window only
    check(have && r.used0 == 0 && r.other0 == 0 && r.readable == 0 && r.n58 == 0 && r.fenceHit == 0 &&
              r.fenceVal == 0,
          "D1 deep off: no window reads and no 0x58 walk (the CP-cost default, c136b649)");
    K::gpuHangWdNoteIb(0x1E82B000u, n, 0x80000000u, alias);
    const bool have2 = K::gpuHangWdLastIbForTest(&r);
    check(have2 && r.win == 0x80000000u && r.readable == 0 && r.n58 == 0 && r.used0 == 0,
          "D2 deep off: the 0x8-alias IB is recorded (window 0x8) without its deep fields");
  }
}

// Step 4 2k(4): the host-work classification (by state, never by time) and the scopes it reads.
void testHostWork() {
  // H1 at 3 s (T5a's state) with the CP in phase swap
  K::GpuHangSnap s = hungSnap();
  s.fenceWaiter = false; s.anyWaiter = false;
  s.completedSinceNs = s.nowNs - 3 * kS;
  s.cpPhase = 3;
  s.cpPhaseSinceNs = s.nowNs - 3 * kS;
  const uint32_t m = K::gpuHangEvaluate(s);
  check(m == K::kGhH1, "W0 the 4(b) dump stall's state: H1 with the CP in phase swap");
  check(K::gpuHangHostWorkExplains(m, s) == 0, "W1 no host work open: nothing explained (H1 fires as before)");
  s.hostWork = true; s.hostWorkKind = K::kHostWorkDumpWrite; s.hostWorkSinceNs = s.nowNs - 3 * kS;
  check(K::gpuHangHostWorkExplains(m, s) == 0, "W1b host work open but the host GPU queue unknown / stuck: not explained");
  s.gpuCaughtUp = true;
  check(K::gpuHangHostWorkExplains(m, s) == K::kGhH1, "W2 the render thread in a dump write + CP in swap + the host "
        "queue caught up: H1 explained");
  s.cpPhase = 2;
  check(K::gpuHangHostWorkExplains(m, s) == 0, "W3 CP in an IB (not swap), work on the render thread: not explained");
  s.hostWorkOnCp = true;
  check(K::gpuHangHostWorkExplains(m, s) == K::kGhH1, "W4 the work on the CP's own thread: H1 explained in any phase");
  s.quitCut = true;
  check(K::gpuHangHostWorkExplains(m, s) == 0, "W5 after the quit's ring cut: nothing (the watchdog stands down)");
  s.quitCut = false;
  // H3: only for work on the CP's own thread
  s.hostWorkOnCp = false;
  check(K::gpuHangHostWorkExplains(K::kGhH3, s) == 0, "W6 H3 with the work on the render thread: not explained");
  s.hostWorkOnCp = true;
  check(K::gpuHangHostWorkExplains(K::kGhH3 | K::kGhH1, s) == (K::kGhH3 | K::kGhH1),
        "W7 H3 + H1 with the work on the CP: both explained");
  // H0 / H2 never
  s.cpPhase = 3;
  check(K::gpuHangHostWorkExplains(K::kGhH0 | K::kGhH2, s) == 0, "W8 H0 and H2 are never explained (a 10 s freeze is "
        "a defect whatever holds it)");
  check(K::gpuHangHostWorkExplains(K::kGhH1 | K::kGhH2, s) == K::kGhH1, "W9 H1 + H2: only H1 explained, H2 still fires");
  // Astra P2 4: causality and the 10 s bound
  s.gpuCaughtUp = false;
  check(K::gpuHangHostWorkExplains(K::kGhH1 | K::kGhH3, s) == 0,
        "W15 a real GPU hang under open host work (the host queue outstanding, not moving): [GPU_HANG], not explained");
  s.gpuCaughtUp = true;
  s.hostWorkSinceNs = s.nowNs - 10 * kS;
  check(K::gpuHangHostWorkExplains(K::kGhH1, s) == 0, "W16 one scope open 10 s explains nothing more (H2's bound)");
  s.hostWorkSinceNs = s.nowNs - 10 * kS + 1;
  check(K::gpuHangHostWorkExplains(K::kGhH1, s) == K::kGhH1, "W16b ... and 9.99 s still does");

  // the scopes
  K::hostWorkForceEnableForTest(1);
  K::HostWorkView v;
  const int open0 = K::hostWorkOpenForTest();
  {
    std::thread([] { K::HostWorkScope w(K::kHostWorkDumpWrite); }).join();   // an unmarked thread
  }
  bool seen = false, onCp = true;
  uint32_t kind = 0;
  int during = -1;
  std::thread([&] {
    K::hostWorkMarkPresentingThread(false);
    K::HostWorkScope w(K::kHostWorkPsoCompile);
    during = K::hostWorkOpenForTest();
    seen = K::hostWorkOldest(&v);
    kind = v.kind;
    onCp = v.onCp;
  }).join();
  check(open0 == 0 && during == 1 && seen && kind == K::kHostWorkPsoCompile && !onCp,
        "W10 a scope on a marked presenting thread is visible (kind, not the CP); one on an unmarked thread is not");
  check(K::hostWorkOpenForTest() == 0 && !K::hostWorkOldest(&v), "W11 the scope closes with its block");
  std::thread([&] {
    K::hostWorkMarkPresentingThread(true);
    K::HostWorkScope a(K::kHostWorkShaderCompile);
    K::HostWorkScope b(K::kHostWorkDumpWrite);   // nested: the OLDEST is reported
    seen = K::hostWorkOldest(&v);
    kind = v.kind;
    onCp = v.onCp;
  }).join();
  check(seen && kind == K::kHostWorkShaderCompile && onCp, "W12 nested scopes on the CP: the oldest, marked on the CP");
  K::hostWorkForceEnableForTest(0);
  std::thread([&] {
    K::hostWorkMarkPresentingThread(true);
    K::HostWorkScope a(K::kHostWorkShaderCompile);
    during = K::hostWorkOpenForTest();
  }).join();
  check(during == 0, "W13 gate off: a scope records nothing (the watchdog judges exactly as before)");
  // Astra P2 5: no ABA - two writers open / close scopes of their own kind as fast as they can; every view a reader
  // takes pairs a writer's kind with that writer's tid (the seqlock), never one writer's kind with the other's tid
  K::hostWorkForceEnableForTest(1);
  {
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> tidA{0}, tidB{0};
    auto writer = [&](uint32_t kind, std::atomic<uint32_t>& tid) {
      K::hostWorkMarkPresentingThread(false);
      tid = K::hostwork_detail::hostTid();
      while (!stop.load(std::memory_order_relaxed)) { K::HostWorkScope w(kind); }
    };
    std::thread a(writer, uint32_t(K::kHostWorkDumpWrite), std::ref(tidA));
    std::thread b(writer, uint32_t(K::kHostWorkPsoCompile), std::ref(tidB));
    while (!tidA || !tidB) std::this_thread::yield();
    uint64_t reads = 0, mixed = 0;
    for (int i = 0; i < 200000; ++i) {
      K::HostWorkView v;
      if (!K::hostWorkOldest(&v)) continue;
      ++reads;
      if ((v.kind == K::kHostWorkDumpWrite && v.hostTid != tidA) || (v.kind == K::kHostWorkPsoCompile && v.hostTid != tidB))
        ++mixed;
    }
    stop = true;
    a.join(); b.join();
    std::printf("  (W17: %llu views read, %llu mixed)\n", (unsigned long long)reads, (unsigned long long)mixed);
    check(mixed == 0 && K::hostWorkOpenForTest() == 0, "W17 seqlock: no view mixes one scope's kind with another's tid");
  }
  K::hostWorkForceEnableForTest(-1);
  const uint64_t before = K::gpuHangWdHostWorkStallsForTest();
  K::gpuHangWdHostWorkReportForTest();   // the block's three lines (visible in the test log)
  check(K::gpuHangWdHostWorkStallsForTest() == before, "W14 the report seam logs a block without counting a tick's");
}

}  // namespace

int runGpuHangWdTest() {
  // c136b649 (2026-10-04, CP costs) made the watchdog's deep per-IB work - the first dwords through both windows
  // and the 0x58 fence walk - opt-in: SR_GPU_HANG_WD_DEEP=1 (kernel/gpu_hang_wd.cpp gpuHangWdNoteIb, read once per
  // process). This test predates it and never set the gate, so I2-I4 read the default record's zeros: the 57 / 3
  // since then (step 4 M8, 2026-10-18) was the test, not the watchdog. Unset -> the test sets it before the first
  // IB note (nothing in this process notes an IB earlier) and checks the deep record (I2-I4); SR_GPU_HANG_WD_DEEP=0
  // -> it checks the default record instead (D1-D2). Run both:
  //   build/difftest.exe --gpu-hang-wd-test   and   SR_GPU_HANG_WD_DEEP=0 build/difftest.exe --gpu-hang-wd-test
  const char* deepEnv = std::getenv("SR_GPU_HANG_WD_DEEP");
  if (!deepEnv) _putenv_s("SR_GPU_HANG_WD_DEEP", "1");
  const bool deep = !deepEnv || deepEnv[0] == '1';
  std::printf("GPU_HANG_WD (iter-463) - gate SR_GPU_HANG_WD=%s, SR_GPU_HANG_WD_DEEP=%s%s\n",
              K::gpuHangWdEnabled() ? "on" : "OFF", deep ? "1" : "0", deepEnv ? "" : " (set by the test)");
  testTriggers();
  testLrSet();
  testEpisode();
  testHostWork();
  if (K::gpuHangWdEnabled()) { testLatches(deep); testStrongDef(); }
  else std::printf("  (latch checks skipped: gate off)\n");
  std::printf("gpu-hang-wd-test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
