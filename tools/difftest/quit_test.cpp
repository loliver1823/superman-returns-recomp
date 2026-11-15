// tools/graceful-quit: the graceful quit (kernel/quit.{hpp,cpp}; docs/TOOLKIT.md 1.8m).
//
// Run: build/difftest.exe --quit-test     (exit = failed checks)
//
// What it proves:
//   N   the names: Local\SR_QUIT_<id>_<pid> / Local\SR_QUIT_ACK_<id>_<pid>, the id sanitised like the harness
//       keyboard channel ("default" when empty, at most 64) - tools/sr_quit.ps1 mirrors them.
//   P   the SR_QUIT_*_MS bounds: digits only, in range; "-1", a sign, a unit, overflow, out of range = the default.
//   D   the snapshot check (snapshotDiff): every integer field compared by line shape and occurrence; floats, time
//       keys and *_TIME lines skipped; a line only the re-render has is a difference, the one-shot tail line is not.
//   S   the coordinator's SEQUENCE through injected hooks on a virtual clock: the stage order and every failure arm -
//       a claim refused, a CP that never drains, producers that never settle, a thread never ACKNOWLEDGED (with every
//       counter still), a thread that wakes after the snapshot, a printed field outside the digest that moves after
//       the snapshot, a snapshot with nothing to verify: each FAILED with its reason and exit 81.
//   C   a FAKE QUIT AGAINST THE COUNTERS with the real park points and the real thread census: registered threads
//       behind the real import gate (entry and exit), the real doorbell gate, a real exempt safe point, a real host
//       loop's guest-call gate, and the smoke1 cases: a render thread holding a critical section across its frame
//       loop, a thread blocked acquiring it, a thread blocked in a wait import, one waiting while holding a lock.
//       Checked: complete; every producer parked (the render thread at its next gate after the cut, still holding its
//       lock - no frame after the cut); the blocked ones acknowledged as waiting; the printed values final; each
//       waiter, released after the snapshot, parks at the import's exit before any guest code. A registered thread
//       that never reaches a gate FAILS the quit (threads-not-quiescent) though no counter moves.
//   R   the ring cut under the publish mutex: a doorbell blocked on the mutex across the cut never publishes; under
//       contention nothing publishes after the cut; before the thread cut a lock holder's doorbell is dropped (the
//       CP still drains), after it the holder parks.
//   F   native/quit-drain, the STRANDED FENCE (memres1/mrc2, s3check2 rel_gate span / seed03 freeze): the REAL
//       generated GpuBlockOnFence on a device in guest memory, a render thread holding its lock whose publish the ring
//       cut drops. Without the wait marks (the generated body called directly) it is the bug: FAILED settle
//       threads-not-quiescent, "running guest code (no gate reached)". Through the production strong-def the quit
//       completes with the thread STRANDED, named (tid, wait, dev, target, completed, next, entry seq, locks) and
//       counted (stranded=1 on the settle and complete lines); the counts final, the thread still spinning, its lock
//       held. SR_QUIT_STRANDED=0 keeps the failure. Each condition failing ALONE leaves the quit FAILED / the thread
//       unacknowledged: the CP drain not over (the run and the census), the wait left and re-entered, the fence
//       already passed - against a control where all three hold. The marks nest. (S18-S20: the lines' shapes; the
//       stranded set changing after the snapshot fails the verify.) Round 3: F21, the coordinator and the waiter on
//       one CPU (the waiter turns only while the coordinator sleeps) - every census check follows a sleep, so the
//       stranded wait is acknowledged.
//   RW  native/quit-ringspace, the STRANDED RING-SPACE WAIT (mrt8 ch18, mrt10 nb, s3m_gate): the REAL generated
//       GpuRingWaitForSpace reserving each segment of a lapping command buffer, the lock holder's closes (each a fence
//       tail {progress word, fence}) dropped by the ring cut through the production record (recordDroppedIb). RW1a the
//       body alone: FAILED settle threads-not-quiescent (the bug). RW1b through the production strong-def, with ctx.lr
//       stale at 0x820FDA44 (the body is localized): STRANDED, named (wait=GpuRingWaitForSpace target=end
//       completed=the frozen progress word next=its generation), dropped_progress on the settle line, the quit
//       complete; RW1c nothing moves after; RW1d SR_QUIT_STRANDED=0 keeps the failure. RW2 the native close's progress
//       word recorded from the close, never the reused bytes; RW3 a broken progress packet records the fence alone;
//       RW4 a call with another record never promotes (the hardening at the fence site too). RS0-RS9: each condition
//       of the ring rule failing alone against a control on s3m_gate's numbers; F11: GpuRingWaitReadPtrWrap keeps no
//       rule.
//   HW  native/quit-ringspace: the GPU hang watchdog stands down at the ring cut (the provoked qrd1 d2 ch18 roll: H0
//       fired 1.19 s after the request on the fence the quit parked) - the state that fired then: H0 without the cut
//       (a hang before the request still fires), nothing with it; one stand-down per watchdog through the real tick;
//       the tick's wiring.
//   NC  round 3: the ring cut drops a NATIVE publication through the production path (publishSegment -> 820FD800 ->
//       820FD218's strong-def -> ringless cut branch): a reused segment's stale EVENT_WRITE_SHD ... F tail is never
//       recorded, only the S the native close carries; the same tail as a plain IB is (its bytes are its packets).
//   I   the gates after the thread cut: an exempt loop and a lock holder park like any thread; a release passes the
//       entry and parks at its exit; an ordinary import parks at its entry; a thread out of a wait parks at the exit;
//       the guest-call gate; the exit claim.
//   E   the REAL named events: the channel is created for this process, the harness side sets the request by name,
//       the watcher answers, the harness side sees the ack.
//   W   the production family printers: every built-in prints with why=terminal (the segment lines included), a
//       capture-only re-render of them is field-for-field equal (no false verify failure), the production bounds;
//       the production wiring by source: the lock thunks' order, callKernel's entry / exit gates (before any APC, and
//       on the exception path), the doorbell's publish inside publishUnlessCut and the cut under the same mutex.
//   B   the hot-path cost (measured, printed): the import gate pair and the doorbell's cut test.
#include "kernel/quit.hpp"
#include "kernel/gpu_hang_wd.hpp"
#include "kernel/gpu_submit.hpp"
#include "kernel/gpu_ringless.hpp"
#include "kernel/native_flush.hpp"
#include "kernel/guest_context.hpp"
#include "gfx/resource_memory.hpp"
#include "gfx/resource_memory_check.hpp"
#include "ppc/ppc_config.h"
#include "ppc/ppc_context.h"
#include "ppc/ppc_recomp_shared.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Q = sr::kernel::quit;

// The generated GpuBlockOnFence body (kernel/gpu_hang_wd.cpp's strong-def sub_820FD9A8 calls it after its marks).
extern "C" void __imp__sub_820FD9A8(PPCContext& __restrict ctx, uint8_t* base);
// The generated GpuRingWaitProgress body (round 2: kernel/gpu_hang_wd.cpp's pass-through strong-def sub_820F33E8).
extern "C" void __imp__sub_820F33E8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_820FCB30(PPCContext& __restrict ctx, uint8_t* base);  // native/quit-ringspace: RW1

namespace {
int g_pass = 0, g_fail = 0;
void check(bool ok, const char* name) {
  if (ok) { g_pass++; std::printf("  ok  %s\n", name); }
  else { g_fail++; std::printf("FAIL: %s\n", name); }
}
bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

// ── S: a fake game behind the hooks ──────────────────────────────────────────────────────────────────────────────
struct Fake {
  uint64_t now = 1000;
  std::vector<std::string> calls, lines;
  bool claimOk = true;
  uint32_t drainAfterMs = 5;    // the CP parks this long after the request; ~0u = never; 0xFFFFFFFE = no CP
  uint64_t drainReqAt = 0;
  uint64_t counter = 100;       // the one producer counter in the digest
  uint32_t moveForMs = 0;       // the counter keeps moving this long after the thread cut; ~0u = forever
  uint32_t unackedForMs = 0;    // one thread stays unacknowledged this long after the thread cut; ~0u = forever
  bool unackAfterSnapshot = false;  // a thread wakes and runs once the snapshot is printed
  bool digestMovesAfterSnapshot = false;
  bool fieldMovesAfterSnapshot = false;  // a printed field the digest does not cover
  bool printNothing = false;
  int stranded = 0;                        // native/quit-drain: threads the census reports stranded
  bool strandedChangesAfterSnapshot = false;
  bool strandedSwapAfterSnapshot = false;  // round 2: the same count, another identity ({tid 70, seq 7} -> {170, 12})
  uint64_t threadCutAt = 0;
  bool snapshotPrinted = false;
  int families = 0;
  int exitCode = -1;
  bool suppressedSnapshot = true, suppressedVerify = false;
  std::string snapCapture;
  bool hasCall(const char* call) const {
    for (const auto& c : calls) if (c == call) return true;
    return false;
  }
  int index(const char* call) const {
    for (size_t i = 0; i < calls.size(); ++i) if (calls[i] == call) return int(i);
    return -1;
  }
  bool lineHas(const char* s) const {
    for (const auto& l : lines) if (l.find(s) != std::string::npos) return true;
    return false;
  }
  void once(const char* call) {
    if (!hasCall(call)) calls.push_back(call);
  }
};

Q::Hooks fakeHooks(Fake& f) {
  Q::Hooks h;
  h.user = &f;
  h.nowMs = [](void* u) { return static_cast<Fake*>(u)->now; };
  h.sleepMs = [](void* u, uint32_t ms) { static_cast<Fake*>(u)->now += ms ? ms : 1; };
  h.log = [](void* u, bool, const char* line) { static_cast<Fake*>(u)->lines.push_back(std::string("[QUIT] ") + line); };
  h.claim = [](void* u) {
    Fake& f = *static_cast<Fake*>(u);
    f.calls.push_back("claim");
    return f.claimOk;
  };
  h.ack = [](void* u) { static_cast<Fake*>(u)->calls.push_back("ack"); };
  h.cutRing = [](void* u) { static_cast<Fake*>(u)->calls.push_back("cutRing"); };
  h.requestDrain = [](void* u) {
    Fake& f = *static_cast<Fake*>(u);
    f.calls.push_back("requestDrain");
    f.drainReqAt = f.now;
  };
  h.drainState = [](void* u, Q::CpDrained* d) {
    Fake& f = *static_cast<Fake*>(u);
    if (f.drainAfterMs == 0xFFFFFFFEu) return Q::Drain::NoCp;
    if (f.drainAfterMs != ~0u && f.now - f.drainReqAt >= f.drainAfterMs) {
      d->rptr = d->wptr = 77;
      d->swaps = 41;
      d->rlPending = 1;
      f.once("drained");
      return Q::Drain::Parked;
    }
    d->rptr = 70;
    d->wptr = 77;
    return Q::Drain::Draining;
  };
  h.cutThreads = [](void* u) {
    Fake& f = *static_cast<Fake*>(u);
    f.calls.push_back("cutThreads");
    f.threadCutAt = f.now;
  };
  h.digest = [](void* u, Q::Digest* d) {
    Fake& f = *static_cast<Fake*>(u);
    if (f.moveForMs == ~0u || f.now - f.threadCutAt < f.moveForMs) f.counter++;
    if (f.digestMovesAfterSnapshot && f.snapshotPrinted) f.counter += 5;
    d->add("fake.counter", f.counter);
    d->add("fake.const", 7);
  };
  h.threadsBegin = [](void* u) { static_cast<Fake*>(u)->once("threadsBegin"); };
  h.threadsCheck = [](void* u, bool) {
    Fake& f = *static_cast<Fake*>(u);
    f.once("threadsCheck");
    Q::ThreadSummary t;
    t.threads = 5;
    t.parked = 3;
    t.waiting = 1;
    t.hostIdle = 1;
    const bool unacked = (f.unackedForMs == ~0u || f.now - f.threadCutAt < f.unackedForMs) ||
                         (f.unackAfterSnapshot && f.snapshotPrinted);
    if (unacked) {
      t.waiting = 0;
      t.unacked = 1;
      std::snprintf(t.firstUnacked, sizeof(t.firstUnacked), "tid=42 name=FAKE running guest code (no gate reached)");
    }
    t.stranded = f.stranded + ((f.strandedChangesAfterSnapshot && f.snapshotPrinted) ? 1 : 0);
    for (int i = 0; i < t.stranded; ++i) {
      if (i >= Q::ThreadSummary::kStrandedMax) {  // the census's own rule: counted, and flagged
        t.strandedOverflow = true;
        break;
      }
      const bool swapped = f.strandedSwapAfterSnapshot && f.snapshotPrinted && i == 0;
      Q::StrandedId& s = t.strandedId[i];
      s.tid = uint32_t(swapped ? 170 : 70 + i);
      s.kind = Q::kGpuWaitFence;
      s.dev = 0x70700000u;
      s.target = 0x1D315u;
      s.completed = 0x1D313u;
      s.next = 0x1D31Bu;
      s.seq = swapped ? 12u : 7u;
      s.spin = swapped ? 13u : 8u;
      s.locks = 3;
    }
    return t;
  };
  h.frames = [](void*) {
    Q::FrameInfo fi;
    fi.frames = fi.swaps = 41;
    fi.vdswaps = 42;
    fi.tail = 1;
    return fi;
  };
  h.printFamilies = [](void* u, const char* why, std::string* cap, bool suppress) {
    Fake& f = *static_cast<Fake*>(u);
    f.calls.push_back(std::string(suppress ? "verify:" : "families:") + why);
    const bool verify = f.snapshotPrinted;
    if (!verify) f.suppressedSnapshot = suppress;
    else f.suppressedVerify = suppress;
    if (cap && !f.printNothing) {
      char line[200];
      std::snprintf(line, sizeof(line), "[FAKE] %s counter=%llu field=%d rate=%.1f wallMs=%llu\n", why,
                    (unsigned long long)f.counter, (verify && f.fieldMovesAfterSnapshot) ? 9 : 3, verify ? 2.5 : 1.5,
                    (unsigned long long)f.now);
      *cap += line;
      if (!verify) *cap += "[FAKE] tail frame=42 pk=7\n";  // one-shot: only the snapshot has it
    }
    if (!verify) {
      f.snapCapture = cap ? *cap : "";
      f.snapshotPrinted = true;
    }
    return f.families = 6;
  };
  h.exit = [](void* u, int code) {
    Fake& f = *static_cast<Fake*>(u);
    f.calls.push_back("exit");
    f.exitCode = code;
  };
  h.cfg = Q::Config{};
  return h;
}

void sequenceTests() {
  {  // native/quit-ringspace: the DIAGNOSTIC thread-cut delay (Config::cutDelayMs) - off: none; on: after the drain,
     // before the thread cut, named, the sequence otherwise unchanged
    Fake off, on;
    Q::Hooks hOff = fakeHooks(off), hOn = fakeHooks(on);
    hOn.cfg.cutDelayMs = 150u;
    const Q::Result rOff = Q::run(hOff), rOn = Q::run(hOn);
    const uint64_t gapOff = off.threadCutAt - off.drainReqAt, gapOn = on.threadCutAt - on.drainReqAt;
    check(rOff.code == 0 && rOn.code == 0 && !off.lineHas("diag-cut-delay") &&
              on.lineHas("[QUIT] stage diag-cut-delay: the thread cut held back 150 ms") && gapOn >= gapOff + 150u &&
              on.index("drained") + 1 == on.index("cutThreads"),
          "SD1 the diagnostic thread-cut delay: off by default (no line, no wait); set, the thread cut waits that long "
          "after the CP parked, the stage named, the quit otherwise the same");
    bool bad = false;
    check(Q::parseMs("0", 0, 0, 5000, &bad) == 0 && !bad && Q::parseMs("5001", 0, 0, 5000, &bad) == 0 && bad,
          "SD2 its bound: 0 (off) .. 5000 ms; out of range = off");
  }
  {
    Fake f;
    f.moveForMs = 120;  // producers still finishing just after the thread cut: settle waits them out
    const Q::Result r = Q::run(fakeHooks(f));
    const int iClaim = f.index("claim"), iAck = f.index("ack"), iRing = f.index("cutRing"),
              iReq = f.index("requestDrain"), iDrained = f.index("drained"), iThreads = f.index("cutThreads"),
              iBegin = f.index("threadsBegin"), iCheck = f.index("threadsCheck"), iFam = f.index("families:terminal"),
              iVer = f.index("verify:terminal"), iExit = f.index("exit");
    check(r.code == 0 && f.exitCode == 0 && std::strcmp(r.stage, "complete") == 0, "S1 happy path: complete, exit 0");
    check(iClaim == 0 && iAck == 1 && iRing == 2 && iReq == 3 && iDrained == 4 && iThreads == 5 && iBegin == 6 &&
              iCheck == 7 && iFam == 8 && iVer == 9 && iExit == 10 && f.calls.size() == 11,
          "S2 stage order: claim, ack, ring cut, drain request, CP parked, thread cut, settle (census), snapshot, "
          "verify re-render, exit");
    check(f.lineHas("[QUIT] request received: frames=41") && f.lineHas("[QUIT] stage ring-cut:") &&
              f.lineHas("[QUIT] stage cp-drain: drained rptr=77 wptr=77 swaps=41") &&
              f.lineHas("[QUIT] stage thread-cut:") && f.lineHas("[QUIT] stage settle: every thread acknowledged"),
          "S3 one [QUIT] line per stage");
    check(!f.lines.empty() &&
              f.lines.back().rfind("[QUIT] terminal snapshot complete frames=41 swaps=41 vdswaps=42 tail=1 "
                                   "rl_pending=1 threads=5 parked=3 waiting=1 host_idle=1 families=6 fields=2 "
                                   "elapsed_ms=",
                                   0) == 0,
          "S4 the complete line is LAST: frames / tail / rl_pending / the thread census / families / fields verified");
    check(f.lineHas("settled after: fake.counter("), "S5 settle names the counter that moved before it settled");
    check(!f.suppressedSnapshot && f.suppressedVerify, "S6 the snapshot is logged, the verify re-render only captured");
  }
  {
    Fake f;
    f.claimOk = false;
    const Q::Result r = Q::run(fakeHooks(f));
    check(r.code == -1 && std::strcmp(r.reason, "already-exiting") == 0 && f.calls.size() == 1 && !f.hasCall("ack") &&
              !f.hasCall("exit") && f.lineHas("request ignored"),
          "S7 the game already exiting: no ack, no cut, no exit");
  }
  {
    Fake f;
    f.drainAfterMs = ~0u;
    const uint64_t t0 = f.now;
    const Q::Result r = Q::run(fakeHooks(f));
    check(r.code == Q::kExitFailed && std::strcmp(r.reason, "drain-timeout") == 0 && !f.hasCall("cutThreads") &&
              !f.hasCall("families:terminal") && f.lineHas("reason=drain-timeout") && f.lineHas("rptr=70 wptr=77") &&
              f.now - t0 >= 10000 && f.now - t0 < 10100,
          "S8 a CP that never drains: FAILED drain-timeout at the 10 s bound with rptr/wptr, exit 81, no family");
  }
  {
    Fake f;
    f.drainAfterMs = 0xFFFFFFFEu;
    const Q::Result r = Q::run(fakeHooks(f));
    check(r.code == 0 && f.lineHas("no command processor thread") && f.hasCall("families:terminal"),
          "S9 no CP thread (boot / SR_PM4_CP=0): nothing to drain, the snapshot proceeds");
  }
  {
    Fake f;
    f.moveForMs = ~0u;
    const uint64_t t0 = f.now;
    const Q::Result r = Q::run(fakeHooks(f));
    check(r.code == Q::kExitFailed && std::strcmp(r.reason, "producers-not-quiesced") == 0 &&
              !f.hasCall("families:terminal") && f.lineHas("moved: fake.counter(") && !f.lineHas("fake.const(") &&
              f.now - t0 < 3000 + 10000,
          "S10 producers that never settle: FAILED producers-not-quiesced naming only the moving field, no family");
  }
  {
    Fake f;
    f.digestMovesAfterSnapshot = true;
    const Q::Result r = Q::run(fakeHooks(f));
    check(r.code == Q::kExitFailed && std::strcmp(r.reason, "counters-moved-after-snapshot") == 0 &&
              f.hasCall("families:terminal") && !f.lineHas("snapshot complete") && f.lineHas("counter"),
          "S11 counters moving after the snapshot: FAILED at verify, the complete line is never printed");
  }
  {
    Q::Digest a, b;
    a.add("x", 1);
    a.add("y", 2);
    b = a;
    char m[128];
    const bool same = !Q::digestDiff(a, b, m, sizeof(m));
    b.value[1] = 5;
    const bool diff = Q::digestDiff(a, b, m, sizeof(m)) && std::strcmp(m, "y(2->5)") == 0;
    check(same && diff, "S12 digestDiff: identical -> false; a moved field -> its name and both values");
  }
  {
    Fake f;
    f.unackedForMs = ~0u;  // every counter still: only the acknowledgement can catch it
    const Q::Result r = Q::run(fakeHooks(f));
    check(r.code == Q::kExitFailed && std::strcmp(r.reason, "threads-not-quiescent") == 0 &&
              !f.hasCall("families:terminal") && f.lineHas("not acknowledged: tid=42 name=FAKE"),
          "S13 a thread never acknowledged (every counter still): FAILED threads-not-quiescent naming it, no family");
  }
  {
    Fake f;
    f.unackedForMs = 150;
    const Q::Result r = Q::run(fakeHooks(f));
    check(r.code == 0 && f.lineHas("acknowledged after: tid=42"),
          "S14 a thread acknowledged late: the settle waits for it, then completes (and says so)");
  }
  {
    Fake f;
    f.fieldMovesAfterSnapshot = true;  // a printed field the digest does not cover
    const Q::Result r = Q::run(fakeHooks(f));
    check(r.code == Q::kExitFailed && std::strcmp(r.reason, "counters-moved-after-snapshot") == 0 &&
              f.lineHas("[FAKE] field(3->9)") && !f.lineHas("snapshot complete"),
          "S15 a printed field outside the digest moves after the snapshot: the verify re-render FAILS it by name");
  }
  {
    Fake f;
    f.unackAfterSnapshot = true;
    const Q::Result r = Q::run(fakeHooks(f));
    check(r.code == Q::kExitFailed && std::strcmp(r.stage, "verify") == 0 &&
              std::strcmp(r.reason, "threads-not-quiescent") == 0 && !f.lineHas("snapshot complete"),
          "S16 a thread that wakes and runs after the snapshot: FAILED at verify");
  }
  {
    Fake f;
    f.printNothing = true;
    const Q::Result r = Q::run(fakeHooks(f));
    check(r.code == Q::kExitFailed && std::strcmp(r.reason, "snapshot-empty") == 0,
          "S17 families that printed nothing to verify: FAILED snapshot-empty (a broken capture never passes)");
  }
  {
    Fake f;
    f.stranded = 1;  // native/quit-drain: never silent
    const Q::Result r = Q::run(fakeHooks(f));
    int iSettle = -1, iStranded = -1;
    for (size_t i = 0; i < f.lines.size(); ++i) {
      if (f.lines[i].find("[QUIT] stage settle:") == 0) iSettle = int(i);
      if (f.lines[i].find("[QUIT] stranded tid=70 wait=GpuBlockOnFence dev=70700000 target=0001D315 "
                          "completed=0001D313 next=0001D31B seq=7 spin=8 locks=3 ") == 0)
        iStranded = int(i);
    }
    const std::string& last = f.lines.empty() ? std::string() : f.lines.back();
    check(r.code == 0 && iSettle >= 0 && iStranded == iSettle + 1 && f.lines[iSettle].find(" stranded=1 ") != std::string::npos &&
              last.find("[QUIT] terminal snapshot complete ") == 0 && last.size() > 11 &&
              last.compare(last.size() - 11, 11, " stranded=1") == 0,
          "S18 a stranded thread is never silent: stranded=1 on the settle line, one [QUIT] stranded line naming its "
          "tid, wait, dev, target, completed fence, next, residence seq, spin seq and locks right after it, and "
          "stranded=1 LAST on the complete line");
  }
  {
    Fake f;
    f.strandedChangesAfterSnapshot = true;
    const Q::Result r = Q::run(fakeHooks(f));
    check(r.code == Q::kExitFailed && std::strcmp(r.stage, "verify") == 0 &&
              std::strcmp(r.reason, "threads-not-quiescent") == 0 && f.lineHas("the stranded threads changed") &&
              !f.lineHas("snapshot complete"),
          "S19 the stranded set changing between the snapshot and its verify: FAILED at verify");
  }
  {  // round 2 (Astra P2): the SAME count, another thread - a count compare passes it, the identity compare must not
    Fake f;
    f.stranded = 1;
    f.strandedSwapAfterSnapshot = true;
    const Q::Result r = Q::run(fakeHooks(f));
    check(r.code == Q::kExitFailed && std::strcmp(r.stage, "verify") == 0 &&
              std::strcmp(r.reason, "threads-not-quiescent") == 0 &&
              f.lineHas("the stranded threads changed after the snapshot: {tid=70 ") && f.lineHas(" seq=7 spin=8 ") &&
              f.lineHas("} -> {tid=170 ") && f.lineHas(" seq=12 spin=13 ") && !f.lineHas("snapshot complete"),
          "S21 the {tid A, seq 7} -> {tid B, seq 12} swap (stranded=1 both times): FAILED at verify naming both "
          "identities - verify compares the set, never the count");
  }
  {  // round 2 (Astra P2): no silent cap - every identity up to kStrandedMax named; one more FAILS the settle
    Fake f;
    f.stranded = Q::ThreadSummary::kStrandedMax;
    const Q::Result r = Q::run(fakeHooks(f));
    int lines = 0;
    for (const auto& l : f.lines)
      if (l.find("[QUIT] stranded tid=") == 0) lines++;
    Fake g;
    g.stranded = Q::ThreadSummary::kStrandedMax + 1;
    const Q::Result r2 = Q::run(fakeHooks(g));
    check(r.code == 0 && lines == Q::ThreadSummary::kStrandedMax && f.lineHas("[QUIT] stranded tid=133 ") &&
              f.lineHas(" stranded=64") && r2.code == Q::kExitFailed && std::strcmp(r2.stage, "settle") == 0 &&
              std::strcmp(r2.reason, "stranded-overflow") == 0 && g.lineHas("stranded=65: more stranded threads") &&
              !g.lineHas("[QUIT] stranded tid=") && !g.lineHas("snapshot complete"),
          "S22 the cap's boundary: 64 stranded threads - 64 [QUIT] stranded lines, stranded=64, complete; 65 - FAILED "
          "settle stranded-overflow, no line, no complete (never a silent cap)");
  }
  {
    Fake f;
    const Q::Result r = Q::run(fakeHooks(f));
    const std::string& last = f.lines.empty() ? std::string() : f.lines.back();
    check(r.code == 0 && !f.lineHas("[QUIT] stranded ") && last.size() > 11 &&
              last.compare(last.size() - 11, 11, " stranded=0") == 0,
          "S20 nothing stranded: no stranded line, stranded=0 on the complete line");
  }
}

// ── P: bounds ────────────────────────────────────────────────────────────────────────────────────────────────────
void boundTests() {
  bool bad = true;
  check(Q::parseMs("5000", 30000, 1000, 600000, &bad) == 5000 && !bad, "P1 digits in range are taken");
  check(Q::parseMs("-1", 30000, 1000, 600000, &bad) == 30000 && bad, "P2 \"-1\" (infinite to a strtoul) is rejected");
  check(Q::parseMs(nullptr, 30000, 1000, 600000, &bad) == 30000 && !bad &&
            Q::parseMs("", 30000, 1000, 600000, &bad) == 30000 && !bad,
        "P3 unset / empty = the default, not an error");
  bool b1 = false, b2 = false, b3 = false, b4 = false;
  check(Q::parseMs("5000ms", 30000, 1000, 600000, &b1) == 30000 && b1 &&
            Q::parseMs(" 5000", 30000, 1000, 600000, &b2) == 30000 && b2 &&
            Q::parseMs("+5000", 30000, 1000, 600000, &b3) == 30000 && b3 &&
            Q::parseMs("abc", 30000, 1000, 600000, &b4) == 30000 && b4,
        "P4 a unit, a space, a sign, letters: rejected");
  check(Q::parseMs("999", 30000, 1000, 600000, &b1) == 30000 && b1 &&
            Q::parseMs("600001", 30000, 1000, 600000, &b2) == 30000 && b2,
        "P5 out of range: rejected");
  check(Q::parseMs("99999999999", 30000, 1000, 600000, &b1) == 30000 && b1 &&
            Q::parseMs("4294967297", 30000, 1000, 600000, &b2) == 30000 && b2,
        "P6 overflow (and 2^32+1, which wraps to 1 in 32 bits): rejected");
  check(Q::parseMs("1000", 30000, 1000, 600000, &b1) == 1000 && !b1 &&
            Q::parseMs("600000", 30000, 1000, 600000, &b2) == 600000 && !b2,
        "P7 the bounds themselves are accepted");
}

// ── D: the snapshot check ────────────────────────────────────────────────────────────────────────────────────────
void diffTests() {
  char m[512];
  int fields = 0;
  const std::string a =
      "[GPU_CP] terminal rptr=5 wptr=5 doorbells=10 packets=99 (faults 0) | ringless ib=3 bs=4\n"
      "[GPU_CP_TIME] terminal wall=12ms swaps=+3 (1.0/s) idle=40 ib=2\n"
      "[NATIVE_TRANSPORT] terminal append=7 appendUs=33 appendMaxUs=1.5 over1ms=0\n"
      "[NATIVE_EXEC] terminal syncs=4 wait=1.5ms lagUs=17 between=0.5\n"
      "[PM4_COV] terminal tail frame=8 pk=12\n"
      "[NATIVE_FLUSH] terminal seg closed=9 idMiss=0\n"
      "[NATIVE_FLUSH] terminal seg closed=9 idMiss=0\n";
  check(Q::snapshotDiff(a, a, m, sizeof(m), &fields) == 0 && fields > 10, "D1 identical renders: no difference");
  std::string b = a;
  b.replace(b.find("packets=99"), 10, "packets=98");
  check(Q::snapshotDiff(a, b, m, sizeof(m), &fields) == 1 && has(m, "[GPU_CP] packets(99->98)"),
        "D2 one integer field moved: one difference, named with both values");
  std::string c =
      "[GPU_CP] terminal rptr=5 wptr=5 doorbells=10 packets=99 (faults 0) | ringless ib=3 bs=4\n"
      "[GPU_CP_TIME] terminal wall=99ms swaps=+3 (7.0/s) idle=77 ib=9\n"
      "[NATIVE_TRANSPORT] terminal append=7 appendUs=34 appendMaxUs=9.5 over1ms=0\n"
      "[NATIVE_EXEC] terminal syncs=4 wait=7.5ms lagUs=99 between=0.9\n"
      "[NATIVE_FLUSH] terminal seg closed=9 idMiss=0\n"
      "[NATIVE_FLUSH] terminal seg closed=9 idMiss=0\n";
  check(Q::snapshotDiff(a, c, m, sizeof(m), &fields) == 0,
        "D3 times are not counters: *_TIME lines, floats, ...Us / ms keys and the one-shot tail line only in the "
        "snapshot are not differences");
  std::string d = a + "[NATIVE_FLUSH] terminal residue idle=1\n";
  check(Q::snapshotDiff(a, d, m, sizeof(m), &fields) == 1 && has(m, "only in the re-render"),
        "D4 a line only the re-render has (a family that started printing) is a difference");
  std::string e = a;
  e.replace(e.rfind("idMiss=0"), 8, "idMiss=1");
  check(Q::snapshotDiff(a, e, m, sizeof(m), &fields) == 1 && has(m, "idMiss(0->1)"),
        "D5 two lines of the same shape are compared by occurrence (the second one moved)");
  const std::string f1 = "[X] terminal n=+3 list=1:2,3:4 hex=0x10 last=5\n", f2 = "[X] terminal n=+4 list=1:2,3:4 hex=0x10 last=5\n";
  check(Q::snapshotDiff(f1, f2, m, sizeof(m), &fields) == 1 && fields == 2,
        "D6 +N fields compare; list entries and hex are not integer fields");
}

// ── C / R / I: real park points ──────────────────────────────────────────────────────────────────────────────────
std::atomic<bool> g_stop{false};
// A thread that should finish: joined within `ms`, else left running (a mutant that parks it) - the caller releases
// it (resetForTest) and joins later, so a wrong park fails a check instead of hanging the suite.
bool joinWithin(std::thread& t, DWORD ms) {
  if (WaitForSingleObject(static_cast<HANDLE>(t.native_handle()), ms) != WAIT_OBJECT_0) return false;
  t.join();
  return true;
}
uint64_t realNow(void*) { return GetTickCount64(); }
void realSleep(void*, uint32_t ms) { Sleep(ms); }

// A registered guest thread (the all-threads registry, as runGuestThreadEntry / GuestCaller register).
struct Registered {
  uint64_t ctx[8] = {};
  Registered() { sr::kernel::setCurrentGuestContext(ctx, 0, 0, 0, 0, 0); }
  ~Registered() { sr::kernel::setCurrentGuestContext(nullptr, 0); }
};

// One kernel import as guest/ppc_import_glue.cpp brackets it.
template <class F>
void kimport(const char* name, F&& body) {
  const char* prev = Q::importEnter(name);
  body();
  Q::importLeave(name, prev);
}

struct Producers {
  std::atomic<uint64_t> imports{0}, published{0}, loop{0}, hostLoop{0}, render{0}, afterWait{0}, afterWaitLocked{0},
      acquired{0}, rogue{0};
  std::atomic<uint32_t> tImport{0}, tDoor{0}, tLoop{0}, tHost{0}, tRender{0}, tWait{0}, tWaitLocked{0}, tAcquirer{0};
  std::atomic<bool> renderHolds{false};
  uint64_t printed[5] = {};
  std::mutex pubMu;      // the CP's publish mutex
  std::mutex frameLock;  // a critical section the render thread holds across its whole frame loop (smoke1)
  std::mutex ownLock;    // the lock a waiter holds while it waits (smoke1: tid 24256, KeWaitForSingleObject, locks=1)
  HANDLE waitEvt = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  HANDLE waitEvt2 = CreateEventA(nullptr, TRUE, FALSE, nullptr);
};

struct CtxC {
  Producers* p;
  int exitCode = -1;
  int families = 0;
  std::vector<std::string> lines;
  Q::ThreadSummary last;
};

void countersTest(bool withRogue) {
  Q::resetForTest();
  g_stop = false;
  Producers p;
  std::vector<std::thread> th;
  th.emplace_back([&] {  // a guest thread: the real import gate on every import
    Registered reg;
    p.tImport = GetCurrentThreadId();
    while (!g_stop) {
      kimport("NtYieldExecution", [] {});
      if (!g_stop) p.imports++;
      Sleep(1);
    }
  });
  th.emplace_back([&] {  // a guest thread ringing the doorbell (the real publishUnlessCut)
    Registered reg;
    p.tDoor = GetCurrentThreadId();
    while (!g_stop) {
      Q::publishUnlessCut(p.pubMu, [&] { p.published++; });
      Sleep(1);
    }
  });
  th.emplace_back([&] {  // an exempt host loop (VD_ISR / the audio worker): the real safe point
    Q::markExempt("TEST_LOOP");
    p.tLoop = GetCurrentThreadId();
    while (!g_stop) {
      Sleep(1);
      Q::safePoint("TEST_LOOP");
      if (!g_stop) p.loop++;
    }
  });
  th.emplace_back([&] {  // a GuestCaller host loop: idle between guest calls, the real guest-call gate
    Registered reg;
    Q::markHostLoop();
    p.tHost = GetCurrentThreadId();
    while (!g_stop) {
      Sleep(2);
      Q::guestCallEnter();
      if (!g_stop) p.hostLoop++;
      Q::guestCallLeave();
    }
  });
  th.emplace_back([&] {  // smoke1's render thread: a critical section held across its whole frame loop, imports
    Registered reg;      // and a doorbell inside it
    p.tRender = GetCurrentThreadId();
    Q::lockAcquiring();
    kimport("RtlEnterCriticalSection", [&] { p.frameLock.lock(); });
    p.renderHolds = true;
    while (!g_stop) {
      kimport("KeQueryPerformanceFrequency", [] {});
      Q::publishUnlessCut(p.pubMu, [&] { p.published++; });
      if (!g_stop) p.render++;
      Sleep(1);
    }
    Q::lockReleasing();
    kimport("RtlLeaveCriticalSection", [&] { p.frameLock.unlock(); });
  });
  th.emplace_back([&] {  // wants the render thread's critical section: blocked inside RtlEnterCriticalSection
    while (!p.renderHolds) Sleep(1);
    Registered reg;
    p.tAcquirer = GetCurrentThreadId();
    Q::lockAcquiring();
    kimport("RtlEnterCriticalSection", [&] { p.frameLock.lock(); });
    p.acquired++;  // guest code after the acquire: never while the holder is parked
    Q::lockReleasing();
    kimport("RtlLeaveCriticalSection", [&] { p.frameLock.unlock(); });
  });
  th.emplace_back([&] {  // blocked in a wait import across the whole quit (released after it)
    Registered reg;
    p.tWait = GetCurrentThreadId();
    kimport("KeWaitForSingleObject", [&] { WaitForSingleObject(p.waitEvt, INFINITE); });
    p.afterWait++;  // guest code after the wait: must never run once the thread cut is set
  });
  th.emplace_back([&] {  // smoke1 tid 24256: waiting in KeWaitForSingleObject while holding a critical section
    Registered reg;
    p.tWaitLocked = GetCurrentThreadId();
    Q::lockAcquiring();
    kimport("RtlEnterCriticalSection", [&] { p.ownLock.lock(); });
    kimport("KeWaitForSingleObject", [&] { WaitForSingleObject(p.waitEvt2, INFINITE); });
    p.afterWaitLocked++;  // guest code after the wait
    Q::lockReleasing();
    kimport("RtlLeaveCriticalSection", [&] { p.ownLock.unlock(); });
  });
  if (withRogue)
    th.emplace_back([&] {  // a registered thread no gate reaches; its counter is in no digest
      Registered reg;
      while (!g_stop) {
        p.rogue++;
        Sleep(1);
      }
    });
  Sleep(60);
  check(p.imports > 0 && p.published > 0 && p.loop > 0 && p.hostLoop > 0 && p.render > 0,
        withRogue ? "C8a (rogue) the producers are counting" : "C1 the producers are counting before the quit");

  CtxC ctx{&p};
  const Q::Hooks prod = Q::productionHooks();
  Q::Hooks h;
  h.user = &ctx;
  h.nowMs = realNow;
  h.sleepMs = realSleep;
  h.log = [](void* u, bool, const char* line) {
    static_cast<CtxC*>(u)->lines.push_back(line);
    std::printf("      [QUIT] %s\n", line);
  };
  h.claim = [](void*) { return true; };
  h.ack = [](void*) {};
  h.cutRing = [](void* u) { Q::cutRingUnder(static_cast<CtxC*>(u)->p->pubMu); };
  h.requestDrain = [](void*) {};
  h.drainState = [](void*, Q::CpDrained*) { return Q::Drain::NoCp; };
  h.cutThreads = [](void*) { Q::g_threadCut.store(true, std::memory_order_seq_cst); };
  h.digest = [](void* u, Q::Digest* d) {
    Producers& p = *static_cast<CtxC*>(u)->p;
    d->add("imports", p.imports.load());
    d->add("published", p.published.load());
    d->add("loop", p.loop.load());
    d->add("hostLoop", p.hostLoop.load());
    d->add("render", p.render.load());
  };
  h.threadsBegin = prod.threadsBegin;  // the production thread census over the real registry and gate slots
  h.threadsCheck = prod.threadsCheck;
  h.frames = [](void*) { return Q::FrameInfo{}; };
  h.printFamilies = [](void* u, const char* why, std::string* cap, bool suppress) {
    CtxC& c = *static_cast<CtxC*>(u);
    Producers& p = *c.p;
    const uint64_t v[5] = {p.imports.load(), p.published.load(), p.loop.load(), p.hostLoop.load(), p.render.load()};
    if (!suppress)
      for (int i = 0; i < 5; ++i) p.printed[i] = v[i];
    char line[256];
    std::snprintf(line, sizeof(line), "[TEST] %s imports=%llu published=%llu loop=%llu hostLoop=%llu render=%llu\n",
                  why, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
                  (unsigned long long)v[3], (unsigned long long)v[4]);
    if (cap) *cap += line;
    return c.families = 1;
  };
  h.exit = [](void* u, int code) { static_cast<CtxC*>(u)->exitCode = code; };
  h.cfg.settleMs = 100;
  h.cfg.settleMaxMs = 1500;
  h.cfg.stepMs = 20;
  const Q::Result r = Q::run(h);
  auto lineHas = [&](const char* s) {
    for (const auto& l : ctx.lines) if (l.find(s) != std::string::npos) return true;
    return false;
  };
  if (!withRogue) {
    check(r.code == 0 && ctx.exitCode == 0 && ctx.families == 1, "C2 the quit completes against the real park points");
    check(Q::isParked(p.tImport) && Q::isParked(p.tDoor) && Q::isParked(p.tLoop) && Q::isParked(p.tHost) &&
              Q::isParked(p.tRender) && !Q::isParked(p.tWait) && !Q::isParked(p.tWaitLocked) &&
              !Q::isParked(p.tAcquirer),
          "C3 every producer is parked (import, doorbell, exempt loop, host loop, the render thread holding its "
          "critical section); the two waiters and the lock acquirer are not");
    check(lineHas("waiting=3 ") && lineHas("parked=5 ") && lineHas("holding_locks=3 "),
          "C4 acknowledged as waiting: blocked in KeWaitForSingleObject, the same holding a lock, blocked in "
          "RtlEnterCriticalSection on the parked holder's lock; 5 parked; 3 threads holding locks");
    const uint64_t v[5] = {p.imports, p.published, p.loop, p.hostLoop, p.render};
    Sleep(120);
    check(p.printed[0] == v[0] && p.printed[1] == v[1] && p.printed[2] == v[2] && p.printed[3] == v[3] &&
              p.printed[4] == v[4] && p.imports == v[0] && p.published == v[1] && p.loop == v[2] &&
              p.hostLoop == v[3] && p.render == v[4],
          "C5 the snapshot printed the final counts, and nothing moved after it");
    const bool held = !p.frameLock.try_lock();
    if (!held) p.frameLock.unlock();
    check(held && p.acquired == 0,
          "C6 smoke1 (2): the render thread holding a critical section parked at its next gate after the thread cut - "
          "no frame drawn after the cut; it still holds the lock and the thread wanting it never gets it");
    SetEvent(p.waitEvt);  // the wait ends after the snapshot: the exit gate must park it before any guest code
    Sleep(100);
    check(p.afterWait == 0 && Q::isParked(p.tWait),
          "C7 a thread that waited in an import through the quit parks at the import's exit: no guest code after");
    SetEvent(p.waitEvt2);
    Sleep(100);
    check(p.afterWaitLocked == 0 && Q::isParked(p.tWaitLocked),
          "C9 smoke1 (1): a thread waiting while holding a lock was acknowledged, and when its wait ends after the "
          "snapshot it parks at the import's exit, lock held, no guest code after");
  } else {
    check(r.code == Q::kExitFailed && std::strcmp(r.reason, "threads-not-quiescent") == 0 && ctx.families == 0 &&
              lineHas("running guest code (no gate reached)"),
          "C8 a registered thread no gate reaches (its count in no digest): FAILED threads-not-quiescent, no "
          "snapshot printed");
  }
  g_stop = true;
  SetEvent(p.waitEvt);
  SetEvent(p.waitEvt2);
  Q::resetForTest();
  for (auto& t : th) t.join();
  Q::resetForTest();
  CloseHandle(p.waitEvt);
  CloseHandle(p.waitEvt2);
}

void ringTest() {
  {
    Q::resetForTest();
    std::mutex mu;
    std::atomic<uint64_t> published{0};
    std::atomic<int> result{-1};
    mu.lock();  // the publisher blocks on the publish mutex ...
    std::thread pub([&] { result = Q::publishUnlessCut(mu, [&] { published++; }) ? 1 : 0; });
    Sleep(50);
    Q::g_ringCut.store(true, std::memory_order_relaxed);  // ... across the cut (cutRingUnder's critical section)
    mu.unlock();
    Sleep(50);
    check(published == 0 && result == -1 && Q::parkedCount() == 1,
          "R1 a doorbell waiting for the publish mutex across the cut publishes nothing and parks");
    Q::resetForTest();
    pub.join();
    Q::resetForTest();
  }
  {
    Q::resetForTest();
    g_stop = false;
    std::mutex mu;
    std::atomic<uint64_t> published{0};
    std::vector<std::thread> th;
    for (int k = 0; k < 6; ++k)
      th.emplace_back([&] {
        while (!g_stop) Q::publishUnlessCut(mu, [&] { published++; });
      });
    Sleep(30);
    Q::cutRingUnder(mu);
    const uint64_t atCut = published.load();
    Sleep(100);
    check(published.load() == atCut && atCut > 0,
          "R2 nothing publishes after cutRingUnder returns (6 threads hammering, no in-flight wait)");
    check(Q::parkedCount() == 6, "R3 every doorbell thread is parked");
    g_stop = true;
    Q::resetForTest();
    for (auto& t : th) t.join();
    Q::resetForTest();
  }
  {
    Q::resetForTest();
    std::mutex mu;
    std::atomic<uint64_t> published{0};
    Q::cutRingUnder(mu);
    const uint64_t drops0 = Q::doorbellDrops();
    bool returned = false, ok = true;
    std::thread holder([&] {
      Q::lockAcquiring();  // holding a guest spinlock (820FD800 under dev+0x2A78)
      ok = Q::publishUnlessCut(mu, [&] { published++; });
      returned = true;
      Q::lockReleasing();
    });
    const bool done = joinWithin(holder, 2000);
    check(done && returned && !ok && published == 0 && Q::doorbellDrops() == drops0 + 1 && Q::parkedCount() == 0,
          "R4 a lock holder's doorbell after the cut is dropped: not published, not parked, counted");
    Q::resetForTest();
    if (!done) holder.join();
    check(Q::publishUnlessCut(mu, [&] { published++; }) && published == 1, "R5 no cut: the doorbell publishes");
  }
  {
    Q::resetForTest();
    std::mutex mu;
    std::atomic<uint64_t> published{0};
    Q::cutRingUnder(mu);
    Q::g_threadCut.store(true);  // the CP has drained: the thread cut is armed
    std::atomic<int> returned{0};
    std::thread holder([&] {
      Q::lockAcquiring();
      Q::publishUnlessCut(mu, [&] { published++; });
      returned = 1;
      Q::lockReleasing();
    });
    Sleep(60);
    check(returned == 0 && published == 0 && Q::parkedCount() == 1,
          "R6 after the thread cut a lock holder's doorbell parks it (the drain is over: no pass-through)");
    Q::resetForTest();
    holder.join();
  }
}

// ── F: the stranded fence (native/quit-drain) ────────────────────────────────────────────────────────────────────
// memres1/mrc2 and s3check2 (rel_gate span, rel_gate_seed03 freeze): the quit lands while the render thread holds the
// device locks; the ring cut DROPS its next publish (a lock holder before the thread cut runs on - doorbellCut), so the
// fence that segment carried is never written; the thread then calls GpuBlockOnFence on it and spins forever in
// GpuRingWaitProgress - guest code, no import, no gate - and the settle FAILED threads-not-quiescent. Here the REAL
// generated GpuBlockOnFence runs (through the production strong-def, kernel/gpu_hang_wd.cpp) on a device in guest memory
// (next = dev+0x2A1C, the fence block *(dev+0x2A10)); the doorbell is the real publishUnlessCut and the "CP" is the
// publish itself writing the fence it executes.
constexpr uint32_t kDev = 0x70700000u, kFb = 0x70706000u, kStack = 0x70780000u, kFence0 = 0x0001D311u;
// r13: a KPCR whose +0x100 is a KTHREAD - GpuRingWaitProgress reads KTHREAD+0x58 (its tick) and +0x14C (the owner)
constexpr uint32_t kKpcr = 0x70790000u, kKthread = 0x70791000u;
// round 2: the close's segment (dev+0x34C4) - its last 4 dwords GpuBuildFenceWritePacket's EVENT_WRITE_SHD fb+0
constexpr uint32_t kSeg = 0x70740000u, kSegDwords = 16u;
void wr32(uint32_t va, uint32_t v) { sr::kernel::writeGuestBE32(va, v); }
uint32_t rd32(uint32_t va) { return sr::kernel::readGuestU32BE(va); }
uint32_t mangle(uint32_t va) { return (((va >> 20) + 0x200u) & 0x1000u) + (va & 0x1FFFFFFFu); }
void fenceFixture(uint32_t next, uint32_t completed) {
  std::memset(sr::kernel::guestVAToHost(kDev), 0, 0x5000);
  std::memset(sr::kernel::guestVAToHost(kFb), 0, 0x100);
  std::memset(sr::kernel::guestVAToHost(kKpcr), 0, 0x2000);
  std::memset(sr::kernel::guestVAToHost(kSeg), 0, 0x100);
  wr32(kKpcr + 0x100u, kKthread);  // KTHREAD+0x58 stays 0: the hang trap's tick never advances (as in the game)
  wr32(kDev + 0x2A10u, kFb);   // the fence block (+0 completed, +0x3C the read-pointer write-back)
  wr32(kDev + 0x2A1Cu, next);  // the next fence the SDK hands out
  wr32(kFb, completed);
}
// A segment close carrying fence f (as 820FD660 mode C leaves it for the publish: dev+0x34C4 = the segment start).
void writeSegment(uint32_t f, uint32_t header = 0xC0025800u, uint32_t fbWord = 0u) {
  wr32(kDev + 0x34C4u, kSeg);
  const uint32_t t = kSeg + kSegDwords * 4u - 16u;
  wr32(t, header);
  wr32(t + 4u, 3u);
  wr32(t + 8u, fbWord ? fbWord : (mangle(kFb) | 2u));
  wr32(t + 12u, f);
}
// native/quit-ringspace: the whole 10-dword fence tail GpuBuildFenceWritePacket 820FD580 writes - INVALIDATE_STATE, then
// EVENT_WRITE_SHD {3, mangled(fb+4)|2, progress} (the progress word: (gen & 3) | cursor) and EVENT_WRITE_SHD
// {3, mangled(fb)|2, f}; the progress packet's header / count / address overridable (RW3: a packet that is not it).
void writeTail(uint32_t f, uint32_t progress, uint32_t pHeader = 0xC0025800u, uint32_t pCount = 3u,
               uint32_t pAddr = 0u) {
  writeSegment(f);
  const uint32_t p = kSeg + kSegDwords * 4u - 32u;
  wr32(p - 8u, 0xC0003B00u);  // INVALIDATE_STATE
  wr32(p - 4u, 0x7FFFu);
  wr32(p, pHeader);
  wr32(p + 4u, pCount);
  wr32(p + 8u, pAddr ? pAddr : (mangle(kFb + 4u) | 2u));
  wr32(p + 12u, progress);
}
std::string hex8(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08X", v);
  return b;
}
// round 3 (Astra round 2 P2): the coordinator and a waiter on ONE CPU - the waiter turns only while the coordinator
// sleeps (the hooks' sleepMs hands it exactly one turn and waits for it), never between two of its checks.
std::atomic<uint64_t> g_stepReq{0}, g_stepDone{0};
void lockstepSleep(void*, uint32_t ms) {
  Sleep(ms);
  const uint64_t want = g_stepReq.fetch_add(1) + 1u;
  const ULONGLONG t0 = GetTickCount64();
  while (g_stepDone.load() < want && GetTickCount64() - t0 < 2000) YieldProcessor();
}
void spinMs(double ms) {  // a busy guest-like loop: no import, no gate
  LARGE_INTEGER f, t0, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  do QueryPerformanceCounter(&t);
  while (double(t.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart) < ms);
}

// A guest call under SEH: a fault is reported (code, address, the faulting data address) instead of ending the suite.
using GuestFn = void (*)(PPCContext&, uint8_t*);
int guardedCall(GuestFn fn, PPCContext& c, uint8_t* base, uintptr_t* where, uintptr_t* data) {
  __try {
    fn(c, base);
    return 0;
  } __except (*where = uintptr_t(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
              *data = GetExceptionInformation()->ExceptionRecord->NumberParameters > 1
                          ? uintptr_t(GetExceptionInformation()->ExceptionRecord->ExceptionInformation[1])
                          : 0,
              EXCEPTION_EXECUTE_HANDLER) {
    return int(GetExceptionCode());
  }
}

struct FenceRig {
  std::mutex pubMu, frameLock;
  std::atomic<uint64_t> frames{0}, published{0};
  std::atomic<uint32_t> tid{0}, lastFence{0}, lost{0};
  std::atomic<bool> stop{false}, dropped{false}, faulted{false};
  std::atomic<bool> loseNext{false};  // round 2: the "CP" loses the next published fence (a hang BEFORE any request)
  bool viaBody = false;  // the generated body without the strong-def's marks: the census before native/quit-drain
  uint8_t* base = sr::kernel::getGuestBase();  // the test thread's guest memory, bound in the render thread
  size_t size = sr::kernel::getGuestSize();
};

// smoke1's / mrc2's render thread: the device lock held across the frame loop; each frame a fence, its publish, and
// the real GpuBlockOnFence on it.
void renderFrames(FenceRig& g) {
  // a guest thread binds the guest memory to its own TLS base (PPC_FUNC_PROLOGUE reads g_guestBaseTls)
  sr::kernel::setGuestMemory(g.base, g.size);
  Registered reg;
  g.tid = GetCurrentThreadId();
  Q::lockAcquiring();
  kimport("RtlEnterCriticalSection", [&] { g.frameLock.lock(); });
  uint8_t* const base = g.base;
  while (!g.stop) {
    kimport("KeQueryPerformanceCounter", [] {});  // a gate every frame: after the thread cut it parks here
    const uint32_t f = rd32(kDev + 0x2A1Cu);       // the segment's fence; the SDK's next moves on
    wr32(kDev + 0x2A1Cu, f + 2u);
    writeSegment(f);                               // the close: the segment ends with f's fence packet
    g.lastFence = f;
    if (!Q::publishUnlessCut(g.pubMu, [&] {  // published: the CP writes the fence (unless told to lose it)
          g.published++;
          if (g.loseNext.exchange(false)) g.lost = f;
          else wr32(kFb, f);
        })) {
      // the ring cut dropped it: what the production drop branch does (gpu_ringless updateWritePointer) - the fence
      // the dropped IB carries, read from its own tail, recorded - then the lock holder runs on
      Q::recordDroppedIb(kDev, mangle(kSeg), kSegDwords);
      if (!g.dropped)
        std::printf("      F: tid %u: the publish of fence %08X dropped (completed %08X) - into the real wait\n",
                    unsigned(g.tid.load()), f, rd32(kFb));
      g.dropped = true;
    }
    PPCContext c{};
    c.r1.u64 = kStack;
    c.r13.u64 = kKpcr;
    c.r3.u64 = kDev;
    c.r4.u64 = f;
    c.r5.u64 = 10u;
    uintptr_t where = 0, data = 0;
    // the REAL GpuBlockOnFence: through the production strong-def, or (viaBody) the generated body alone
    const int ex = guardedCall(g.viaBody ? &__imp__sub_820FD9A8 : &sub_820FD9A8, c, base, &where, &data);
    if (ex) {
      std::printf("      F: tid %u: guest fault %08X at %p (module +0x%llx) data %p\n", unsigned(g.tid.load()),
                  unsigned(ex), reinterpret_cast<void*>(where),
                  (unsigned long long)(where - uintptr_t(GetModuleHandleA(nullptr))), reinterpret_cast<void*>(data));
      g.faulted = true;
      break;
    }
    if (!g.stop) g.frames++;
    Sleep(1);
  }
  Q::lockReleasing();
  kimport("RtlLeaveCriticalSection", [&] { g.frameLock.unlock(); });
}

struct CtxF {
  FenceRig* g = nullptr;
  bool cpParks = true;
  bool parkNow = false;  // round 2: the CP parks at once (a quit with nothing to drop)
  uint64_t drainFrom = 0;
  int exitCode = -1, families = 0;
  uint64_t printed[2] = {};
  std::vector<std::string> lines;
  bool lineHas(const std::string& s) const {
    for (const auto& l : lines) if (l.find(s) != std::string::npos) return true;
    return false;
  }
  std::string lineWith(const char* s) const {
    for (const auto& l : lines) if (l.find(s) != std::string::npos) return l;
    return "";
  }
};

Q::Hooks fenceHooks(CtxF& ctx, bool allowStranded) {
  const Q::Hooks prod = Q::productionHooks();
  Q::Hooks h;
  h.user = &ctx;
  h.nowMs = realNow;
  h.sleepMs = realSleep;
  h.log = [](void* u, bool, const char* line) {
    static_cast<CtxF*>(u)->lines.push_back(std::string("[QUIT] ") + line);
    std::printf("      [QUIT] %s\n", line);
  };
  h.claim = [](void*) { return true; };
  h.ack = [](void*) {};
  h.cutRing = [](void* u) { Q::cutRingUnder(static_cast<CtxF*>(u)->g->pubMu); };
  h.requestDrain = [](void* u) { static_cast<CtxF*>(u)->drainFrom = GetTickCount64(); };
  // The CP drains while the lock holder runs on (production: ~16 ms); it parks once the holder's publish was dropped.
  h.drainState = [](void* u, Q::CpDrained* d) {
    CtxF& c = *static_cast<CtxF*>(u);
    if (c.cpParks && (c.parkNow || c.g->dropped || GetTickCount64() - c.drainFrom > 2000)) {
      d->rptr = d->wptr = 5;
      return Q::Drain::Parked;
    }
    return Q::Drain::Draining;
  };
  h.cutThreads = [](void*) { Q::g_threadCut.store(true, std::memory_order_seq_cst); };
  h.digest = [](void* u, Q::Digest* d) {
    FenceRig& g = *static_cast<CtxF*>(u)->g;
    d->add("frames", g.frames.load());
    d->add("published", g.published.load());
  };
  h.threadsBegin = prod.threadsBegin;  // the production census
  h.threadsCheck = prod.threadsCheck;
  h.frames = [](void*) { return Q::FrameInfo{}; };
  h.printFamilies = [](void* u, const char* why, std::string* cap, bool suppress) {
    CtxF& c = *static_cast<CtxF*>(u);
    const uint64_t v[2] = {c.g->frames.load(), c.g->published.load()};
    if (!suppress) { c.printed[0] = v[0]; c.printed[1] = v[1]; }
    char line[160];
    std::snprintf(line, sizeof(line), "[TEST] %s frames=%llu published=%llu\n", why, (unsigned long long)v[0],
                  (unsigned long long)v[1]);
    if (cap) *cap += line;
    return c.families = 1;
  };
  h.exit = [](void* u, int code) { static_cast<CtxF*>(u)->exitCode = code; };
  h.cfg.drainMs = 3000;
  h.cfg.settleMs = 100;
  h.cfg.settleMaxMs = 1500;
  h.cfg.stepMs = 20;
  h.cfg.allowStranded = allowStranded;
  return h;
}

// Ends the render thread: the fence it waits on is written (its BlockOnFence returns), the gates are released.
void endRender(FenceRig& g, std::thread& t) {
  g.stop = true;
  wr32(kFb, g.lastFence.load());
  Q::resetForTest();
  t.join();
  Q::resetForTest();
}

// One quit against the render thread; returns the result (ctx filled).
Q::Result fenceQuit(FenceRig& g, CtxF& ctx, std::thread& t, bool allowStranded) {
  Q::resetForTest();
  fenceFixture(kFence0, kFence0 - 2u);
  t = std::thread([&] { renderFrames(g); });
  Sleep(80);  // frames running: every publish written, every BlockOnFence returning
  ctx.g = &g;
  return Q::run(fenceHooks(ctx, allowStranded));
}

// ── RW: native/quit-ringspace, the STRANDED RING-SPACE WAIT (mrt8 ch18 / mrt10 nb / s3m_gate) ──────────────────────
// The render thread holding its lock closes segments into a command buffer of kRingSegs segments per lap; before each
// segment it reserves the space through the REAL GpuRingWaitForSpace(dev, end, generation), and each close carries the
// fence tail {progress (gen & 3) | end, fence}. Published, the "CP" writes both (fb+4, fb+0); dropped by the ring cut,
// the production branch's record (recordDroppedIb: the tail parse) records both. After the cut the writer laps the
// frozen progress word and its reservation spins forever - only a dropped close could have freed that space.
constexpr uint32_t kRingBuf = 0x70A00000u, kRingStep = 0x400u, kRingSegs = 8u, kGen0 = 0x1EBCu;
struct RingRig : FenceRig {  // FenceRig's `dropped` = "the writer lapped the progress word": the CP may park
  std::atomic<uint32_t> drops{0}, waitEnd{0}, waitGen{0}, frozen{0};
  uint32_t lr = 0;  // ctx.lr before each reservation: the localized body leaves it as it finds it (stale)
};
void renderRing(RingRig& g) {
  sr::kernel::setGuestMemory(g.base, g.size);
  Registered reg;
  g.tid = GetCurrentThreadId();
  Q::lockAcquiring();
  kimport("RtlEnterCriticalSection", [&] { g.frameLock.lock(); });
  uint8_t* const base = g.base;
  for (uint64_t n = 0; !g.stop; ++n) {
    kimport("KeQueryPerformanceCounter", [] {});  // a gate per segment: after the thread cut it parks here
    const uint32_t gen = kGen0 + uint32_t(n / kRingSegs), end = kRingBuf + (uint32_t(n % kRingSegs) + 1u) * kRingStep;
    if (!Q::ringSpaceFree(rd32(kFb + 4u), end, gen) && !g.dropped) {  // it will spin: the writer lapped the GPU
      g.waitEnd = end;
      g.waitGen = gen;
      g.frozen = rd32(kFb + 4u);
      g.dropped = true;
      std::printf("      RW: tid %u: the reservation of [.., %08X) gen %08X spins (progress %08X, %u closes dropped)\n",
                  unsigned(g.tid.load()), end, gen, rd32(kFb + 4u), unsigned(g.drops.load()));
    }
    PPCContext c{};
    c.r1.u64 = kStack;
    c.r13.u64 = kKpcr;
    c.r3.u64 = kDev;
    c.r4.u64 = end;
    c.r5.u64 = gen;
    c.lr = g.lr;
    uintptr_t where = 0, data = 0;
    // the REAL GpuRingWaitForSpace: through the production strong-def, or (viaBody) the generated body alone
    const int ex = guardedCall(g.viaBody ? &__imp__sub_820FCB30 : &sub_820FCB30, c, base, &where, &data);
    if (ex) {
      std::printf("      RW: tid %u: guest fault %08X at %p (module +0x%llx) data %p\n", unsigned(g.tid.load()),
                  unsigned(ex), reinterpret_cast<void*>(where),
                  (unsigned long long)(where - uintptr_t(GetModuleHandleA(nullptr))), reinterpret_cast<void*>(data));
      g.faulted = true;
      break;
    }
    if (g.stop) break;
    const uint32_t f = rd32(kDev + 0x2A1Cu);  // the close: its fence tail, then the publish
    wr32(kDev + 0x2A1Cu, f + 2u);
    const uint32_t p = (gen & 3u) | end;
    writeTail(f, p);
    g.lastFence = f;
    if (!Q::publishUnlessCut(g.pubMu, [&] {
          g.published++;
          wr32(kFb + 4u, p);
          wr32(kFb, f);
        })) {
      Q::recordDroppedIb(kDev, mangle(kSeg), kSegDwords);  // the production drop branch's record: fence + progress
      g.drops++;
    }
    g.frames++;
    Sleep(1);
  }
  Q::lockReleasing();
  kimport("RtlLeaveCriticalSection", [&] { g.frameLock.unlock(); });
}
void endRing(RingRig& g, std::thread& t) {
  g.stop = true;
  wr32(kFb + 4u, g.waitGen.load() & 3u);  // the GPU in the wait's generation: its space test passes
  Q::resetForTest();
  t.join();
  Q::resetForTest();
}
Q::Result ringQuit(RingRig& g, CtxF& ctx, std::thread& t, bool allowStranded) {
  Q::resetForTest();
  fenceFixture(kFence0, kFence0 - 2u);
  wr32(kFb + 4u, ((kGen0 - 1u) & 3u) | (kRingBuf + kRingSegs * kRingStep));  // the GPU finished the previous lap
  t = std::thread([&] { renderRing(g); });
  Sleep(80);  // segments running: every reservation free at once, every close published
  ctx.g = &g;
  return Q::run(fenceHooks(ctx, allowStranded));
}
uint32_t fieldOf(const std::string& line, const char* key) {  // key=N (decimal) on a line, ~0u when absent
  const size_t at = line.find(key);
  return at == std::string::npos ? ~0u : uint32_t(std::strtoul(line.c_str() + at + std::strlen(key), nullptr, 10));
}

void ringStrandedTest() {
  Q::resetGatesForTest();
  {  // RW1a THE BUG: the generated body alone (no marks) - the census before native/quit-ringspace: s3m_gate's FAILED
    RingRig g;
    g.viaBody = true;
    CtxF ctx;
    std::thread t;
    const Q::Result r = ringQuit(g, ctx, t, true);
    const std::string tid = "tid=" + std::to_string(g.tid.load()) + " ";
    check(g.frames > 0 && g.dropped && g.drops > 0 && !g.faulted && r.code == Q::kExitFailed &&
              std::strcmp(r.stage, "settle") == 0 && std::strcmp(r.reason, "threads-not-quiescent") == 0 &&
              ctx.lineHas("not acknowledged: " + tid) && ctx.lineHas("running guest code (no gate reached)") &&
              !ctx.lineHas("[QUIT] stranded tid=") && !Q::isParked(g.tid),
          "RW1a the bug reproduced: the ring cut drops the lock holder's closes, the writer laps the frozen progress word "
          "and the REAL GpuRingWaitForSpace spins - FAILED settle threads-not-quiescent, 'running guest code (no gate "
          "reached)' (s3m_gate / mrt8 ch18 / mrt10 nb)");
    endRing(g, t);
  }
  {  // RW1b THE FIX: the same quit through the production strong-def, the stale lr 0x820FDA44 left in ctx.lr
    RingRig g;
    g.lr = Q::kFenceSpinReturn;
    CtxF ctx;
    std::thread t;
    const Q::Result r = ringQuit(g, ctx, t, true);
    const std::string want = "[QUIT] stranded tid=" + std::to_string(g.tid.load()) +
                             " wait=GpuRingWaitForSpace dev=70700000 target=" + hex8(g.waitEnd) +
                             " completed=" + hex8(g.frozen) + " next=" + hex8(g.waitGen) + " seq=";
    const std::string sl = ctx.lineWith("[QUIT] stranded tid="), settle = ctx.lineWith("stage settle:");
    const std::string last = ctx.lines.empty() ? std::string() : ctx.lines.back();
    const uint32_t dp = fieldOf(settle, " dropped_progress="), df = fieldOf(settle, " dropped_fences=");
    if (sl.rfind(want, 0) != 0) std::printf("      RW1b want '%s...'\n", want.c_str());
    check(g.dropped && g.drops > 0 && !g.faulted && r.code == 0 && ctx.exitCode == 0 && sl.rfind(want, 0) == 0 &&
              sl.find(" locks=1 ") != std::string::npos && settle.find(" stranded=1 ") != std::string::npos &&
              dp != ~0u && dp >= 1u && dp == g.drops.load() && df == dp && last.find("[QUIT] terminal snapshot complete ") == 0 &&
              last.size() > 11 && last.compare(last.size() - 11, 11, " stranded=1") == 0,
          "RW1b the fix: the same quit completes - the render thread STRANDED in GpuRingWaitForSpace, named with its tid, "
          "dev, target (the space's end), completed (the frozen progress word), next (its generation), seq and its "
          "lock; dropped_progress = dropped_fences = the closes the cut dropped; stranded=1 on the settle and complete "
          "lines - with ctx.lr stale at 0x820FDA44 (the localized body never sets it)");
    const uint64_t v0 = g.frames, v1 = g.published;
    Sleep(150);
    const bool held = !g.frameLock.try_lock();
    if (!held) g.frameLock.unlock();
    check(ctx.printed[0] == v0 && ctx.printed[1] == v1 && g.frames == v0 && g.published == v1 && held &&
              !Q::isParked(g.tid),
          "RW1c the snapshot printed the final counts; nothing moves after it; the stranded thread still spins in the "
          "real ring-space wait (not parked), its lock held");
    endRing(g, t);
  }
  {  // RW1d the forbid switch
    RingRig g;
    CtxF ctx;
    std::thread t;
    const Q::Result r = ringQuit(g, ctx, t, false);
    check(g.dropped && r.code == Q::kExitFailed && std::strcmp(r.reason, "threads-not-quiescent") == 0 &&
              ctx.lineHas("stranded (SR_QUIT_STRANDED=0: not acknowledged) tid=") &&
              ctx.lineHas("wait=GpuRingWaitForSpace") && !ctx.lineHas("snapshot complete"),
          "RW1d SR_QUIT_STRANDED=0: the stranded ring-space waiter is not acknowledged - FAILED threads-not-quiescent, "
          "naming it as stranded");
    endRing(g, t);
  }
}

void strandedTest() {
  Q::resetGatesForTest();  // no earlier test thread is alive: forget their slots (an exited exempt loop's role)
  {  // F1 THE BUG: the census before native/quit-drain (the generated body, no marks) - mrc2's line
    FenceRig g;
    g.viaBody = true;
    CtxF ctx;
    std::thread t;
    const Q::Result r = fenceQuit(g, ctx, t, true);
    const std::string tid = "tid=" + std::to_string(g.tid.load()) + " ";
    check(g.frames > 0 && g.dropped && !g.faulted && r.code == Q::kExitFailed && std::strcmp(r.stage, "settle") == 0 &&
              std::strcmp(r.reason, "threads-not-quiescent") == 0 &&
              ctx.lineHas("not acknowledged: " + tid) && ctx.lineHas("running guest code (no gate reached)") &&
              !Q::isParked(g.tid),
          "F1 the bug reproduced: frames running, the quit lands while the render thread holds its lock, the ring cut "
          "drops its publish, the REAL GpuBlockOnFence spins on the fence that segment carried - FAILED settle "
          "threads-not-quiescent naming it, 'running guest code (no gate reached)' (memres1/mrc2's line)");
    endRender(g, t);
  }
  {  // F2 THE FIX: the same quit through the production strong-def
    FenceRig g;
    CtxF ctx;
    std::thread t;
    const Q::Result r = fenceQuit(g, ctx, t, true);
    const uint32_t f = g.lastFence;
    const std::string want = "[QUIT] stranded tid=" + std::to_string(g.tid.load()) +
                             " wait=GpuBlockOnFence dev=70700000 target=" + hex8(f) + " completed=" + hex8(f - 2u) +
                             " next=" + hex8(f + 2u) + " seq=";
    const std::string sl = ctx.lineWith("[QUIT] stranded tid=");
    const std::string last = ctx.lines.empty() ? std::string() : ctx.lines.back();
    check(g.dropped && !g.faulted && r.code == 0 && ctx.exitCode == 0 && ctx.families == 1 && sl.rfind(want, 0) == 0 &&
              sl.find(" locks=1 ") != std::string::npos &&
              ctx.lineHas(" host_idle=0 stranded=1 ") && last.find("[QUIT] terminal snapshot complete ") == 0 &&
              last.size() > 11 && last.compare(last.size() - 11, 11, " stranded=1") == 0,
          "F2 the fix: the same quit completes - the render thread STRANDED in GpuBlockOnFence, named with its tid, "
          "dev, target (the dropped segment's fence), the completed fence, next, entry seq and its lock; stranded=1 on "
          "the settle and complete lines");
    const uint64_t v0 = g.frames, v1 = g.published;
    Sleep(150);
    const bool held = !g.frameLock.try_lock();
    if (!held) g.frameLock.unlock();
    check(ctx.printed[0] == v0 && ctx.printed[1] == v1 && g.frames == v0 && g.published == v1 && held &&
              !Q::isParked(g.tid),
          "F3 the snapshot printed the final counts; nothing moves after it; the stranded thread still spins in the "
          "real wait (not parked), its lock held");
    endRender(g, t);
  }
  {  // F4 the forbid switch (SR_QUIT_STRANDED=0): as before native/quit-drain
    FenceRig g;
    CtxF ctx;
    std::thread t;
    const Q::Result r = fenceQuit(g, ctx, t, false);
    check(g.dropped && r.code == Q::kExitFailed && std::strcmp(r.reason, "threads-not-quiescent") == 0 &&
              ctx.lineHas("stranded (SR_QUIT_STRANDED=0: not acknowledged) tid=") && !ctx.lineHas("snapshot complete"),
          "F4 SR_QUIT_STRANDED=0 (Config::allowStranded false): the stranded thread is not acknowledged - FAILED "
          "threads-not-quiescent, naming it as stranded");
    endRender(g, t);
  }
  {  // F5 condition 1 failing alone: the CP never parks (the run) ...
    FenceRig g;
    CtxF ctx;
    ctx.cpParks = false;
    std::thread t;
    const Q::Result r = fenceQuit(g, ctx, t, true);
    check(r.code == Q::kExitFailed && std::strcmp(r.reason, "drain-timeout") == 0 && !ctx.lineHas("snapshot complete"),
          "F5 condition 1 alone (the run): a CP that never parks - FAILED drain-timeout, the stranded rule never "
          "rescues it");
    endRender(g, t);
  }
  {  // ... and the census alone: the stranded state with the CP not parked
    Q::resetForTest();
    fenceFixture(kFence0, kFence0 - 2u);
    FenceRig g;
    std::thread t([&] { renderFrames(g); });
    Sleep(60);
    Q::cutRingUnder(g.pubMu);  // the ring cut only: no CP drain reported, no run
    for (int i = 0; i < 200 && !g.dropped; ++i) Sleep(5);
    const Q::Hooks prod = Q::productionHooks();
    prod.threadsBegin(prod.user);
    Sleep(60);
    const Q::ThreadSummary s = prod.threadsCheck(prod.user, true);
    check(g.dropped && s.stranded == 0 && s.unacked >= 1 &&
              std::string(s.firstUnacked).find("tid=" + std::to_string(g.tid.load()) + " ") != std::string::npos &&
              std::string(s.firstUnacked).find("in GpuBlockOnFence (not stranded: the CP drain is not over)") !=
                  std::string::npos,
          "F6 condition 1 alone (the census): the thread in the real wait on a dropped fence, but the CP drain not "
          "over - not stranded, unacknowledged 'running guest code ... in GpuBlockOnFence (not stranded: the CP "
          "drain is not over)'");
    endRender(g, t);
  }
  {  // F12 (round 2, Astra P1): a GPU hang that began BEFORE the request - the CP lost a published fence, the render
     // thread already spins on it in the REAL GpuBlockOnFence, and the quit drops nothing: never "stranded"
    Q::resetForTest();
    fenceFixture(kFence0, kFence0 - 2u);
    FenceRig g;
    std::thread t([&] { renderFrames(g); });
    Sleep(60);
    g.loseNext = true;
    for (int i = 0; i < 200 && !g.lost; ++i) Sleep(5);
    Sleep(60);  // spinning on the lost fence, before any request
    CtxF ctx;
    ctx.g = &g;
    ctx.parkNow = true;
    const Q::Result r = Q::run(fenceHooks(ctx, true));
    const std::string tid = "tid=" + std::to_string(g.tid.load()) + " ";
    check(g.lost != 0 && !g.dropped && !g.faulted && r.code == Q::kExitFailed && std::strcmp(r.stage, "settle") == 0 &&
              std::strcmp(r.reason, "threads-not-quiescent") == 0 && ctx.lineHas(tid) &&
              ctx.lineHas("in GpuBlockOnFence (not stranded: its spin began before the ring cut)") &&
              !ctx.lineHas("[QUIT] stranded tid=") && !ctx.lineHas("snapshot complete"),
          "F12 (P1) a GPU hang that began before the request (a fence the CP lost; the render thread already spinning in "
          "the real GpuBlockOnFence; nothing dropped): FAILED threads-not-quiescent, 'not stranded: its spin began "
          "before the ring cut' - never acknowledged as stranded");
    endRender(g, t);
  }
  // The conditions one by one (round 2): registered threads marked by the real gpuWaitEnter / gpuSpinTick /
  // gpuSpinExit / gpuWaitLeave (kernel/quit.hpp - what the strong-defs call), the CP drain over (no CP), the REAL ring
  // cut (cutRingUnder: its wait-sequence mark), the drop the production branch records (recordDroppedFence), a fence on
  // the real device fixture. Each case breaks ONE condition of strandedWait and must leave the quit FAILED naming that
  // condition; the control holds them all and completes.
  struct MarkCase {
    int threads = 1;
    uint32_t kind = Q::kGpuWaitFence;
    uint32_t completed = kFence0 - 2u;  // kFence0: the fence passed
    bool spinBeforeCut = false;         // the spin starts before the ring cut (a hang the quit did not cause)
    bool tick = true;                   // false: the wait never reaches its spin loop (the flush, the early return)
    bool progress = true;               // false: one tick after the cut, then the loop stops turning
    bool exitZero = false;              // GpuRingWaitProgress returned 0 (gpuSpinExit)
    bool reenter = false;               // the wait left and entered again, every ~5 ms
    bool nested = false;                // a nested ring-space wait, every ~5 ms
    bool noGpu = false;                 // byte dev+0x2A39 bit 2 set
    bool lockstep = false;              // round 3 (P2): one turn per coordinator sleep, none between its checks
    uint32_t dropFence = kFence0;       // the fence the cut's drop records (0: nothing dropped)
    // native/quit-ringspace (kind kGpuWaitRingSpace): s3m_gate's wait - progress word BEBC2A2D (generation 1, cursor
    // BEBC2A2C) frozen at fb+4, the reservation [.., BEBC32A0) in generation 1EBE: one behind and short of the end
    uint32_t ringP = 0xBEBC2A2Du, ringEnd = 0xBEBC32A0u, ringGen = 0x1EBEu;
    uint32_t dropProgress = 0xBEBC3301u;  // the progress word the cut's drop records (generation 1, past the end; 0: none)
    uint32_t dropDev = kDev;              // the device whose close carried it (Astra round 1 P2: another device's word)
  };
  struct CtxM {
    const MarkCase* mc = nullptr;
    std::mutex mu;
    std::vector<std::string> lines;
    int exitCode = -1, families = 0;
  };
  struct MarkOut {
    int code = -1;
    std::string reason, failed, all;
    int strandedLines = 0;
    std::vector<uint32_t> tids;
  };
  auto markedQuit = [](const MarkCase& mc) -> MarkOut {
    Q::resetForTest();
    fenceFixture(kFence0 + 2u, mc.completed);  // F = kFence0: issued (next ahead of it)
    const bool ring = mc.kind == Q::kGpuWaitRingSpace;
    if (ring) wr32(kFb + 4u, mc.ringP);
    if (mc.noGpu) wr32(kDev + 0x2A38u, rd32(kDev + 0x2A38u) | 0x00020000u);  // byte dev+0x2A39 |= 2
    g_stepReq = 0;
    g_stepDone = 0;
    std::atomic<bool> stop{false};
    std::atomic<int> ready{0};
    std::mutex tidMu;
    MarkOut out;
    std::vector<std::thread> ths;
    for (int k = 0; k < mc.threads; ++k)
      ths.emplace_back([&] {
        Registered reg;
        {
          std::lock_guard<std::mutex> lk(tidMu);
          out.tids.push_back(GetCurrentThreadId());
        }
        Q::lockAcquiring();
        auto enter = [&] {  // the strong-defs' marks: the ring wait's end, generation and record (any: ticked directly)
          return ring ? Q::gpuWaitEnter(mc.kind, kDev, mc.ringEnd, mc.ringGen, kStack - Q::kWaitRecBelowSp)
                      : Q::gpuWaitEnter(mc.kind, kDev, kFence0);
        };
        Q::GpuWaitMark m = enter();
        if (mc.tick && mc.spinBeforeCut) Q::gpuSpinTick(kDev);
        ready++;
        if (mc.tick && !mc.spinBeforeCut) {
          while (!Q::g_ringCut.load() && !stop) YieldProcessor();
          Q::gpuSpinTick(kDev);
        }
        if (mc.exitZero) Q::gpuSpinExit();
        LARGE_INTEGER fq, t0, t;
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t0);
        uint64_t turns = 0;
        while (!stop) {
          if (mc.lockstep) {  // round 3 (P2): it runs only while the coordinator sleeps - one turn per sleep
            const uint64_t want = g_stepReq.load();
            if (want != turns) {
              Q::gpuSpinTick(kDev);
              turns = want;
              g_stepDone.store(want);
            }
            YieldProcessor();
            continue;
          }
          if (mc.tick && mc.progress) Q::gpuSpinTick(kDev);  // the loop turning: one call per iteration
          YieldProcessor();
          QueryPerformanceCounter(&t);
          if ((mc.reenter || mc.nested) && double(t.QuadPart - t0.QuadPart) * 1000.0 / double(fq.QuadPart) >= 5.0) {
            t0 = t;
            if (mc.reenter) {  // the wait left and entered again: a thread that ran between two waits
              Q::gpuWaitLeave(m);
              m = enter();
            } else {  // a nested wait: the outer resumes as a new residence
              const Q::GpuWaitMark in = Q::gpuWaitEnter(Q::kGpuWaitRingSpace, kDev, 0x40u);
              Q::gpuWaitLeave(in);
            }
          }
        }
        Q::gpuWaitLeave(m);
        Q::lockReleasing();
      });
    while (ready < mc.threads) Sleep(1);
    Sleep(20);
    CtxM ctx;
    ctx.mc = &mc;
    Q::Hooks h;
    h.user = &ctx;
    const Q::Hooks prod = Q::productionHooks();
    h.nowMs = realNow;
    h.sleepMs = mc.lockstep ? lockstepSleep : realSleep;
    h.log = [](void* u, bool, const char* line) {
      static_cast<CtxM*>(u)->lines.push_back(std::string("[QUIT] ") + line);
      std::printf("      [QUIT] %s\n", line);
    };
    h.claim = [](void*) { return true; };
    h.ack = [](void*) {};
    h.cutRing = [](void* u) {
      CtxM& c = *static_cast<CtxM*>(u);
      Q::cutRingUnder(c.mu);  // the real cut: g_ringCut and its wait-sequence mark
      if (c.mc->dropFence) Q::recordDroppedFence(kDev, c.mc->dropFence);  // what the drop branch records
      if (c.mc->kind == Q::kGpuWaitRingSpace && c.mc->dropProgress)
        Q::recordDroppedProgress(c.mc->dropDev, c.mc->dropProgress);
    };
    h.requestDrain = [](void*) {};
    h.drainState = [](void*, Q::CpDrained*) { return Q::Drain::NoCp; };  // the drain over: nothing left to write
    h.cutThreads = [](void*) { Q::g_threadCut.store(true, std::memory_order_seq_cst); };
    h.digest = [](void*, Q::Digest* d) { d->add("still", 1); };
    h.threadsBegin = prod.threadsBegin;
    h.threadsCheck = prod.threadsCheck;
    h.frames = [](void*) { return Q::FrameInfo{}; };
    h.printFamilies = [](void* u, const char* why, std::string* cap, bool) {
      if (cap) *cap += std::string("[TEST] ") + why + " still=1\n";
      return static_cast<CtxM*>(u)->families = 1;
    };
    h.exit = [](void* u, int c) { static_cast<CtxM*>(u)->exitCode = c; };
    h.cfg.settleMs = 100;
    h.cfg.settleMaxMs = 1200;
    h.cfg.stepMs = 20;
    const Q::Result r = Q::run(h);
    out.code = r.code;
    out.reason = r.reason;
    for (const auto& l : ctx.lines) {
      out.all += l + " | ";
      if (l.find("FAILED") != std::string::npos) out.failed += l + " | ";
      if (l.find("[QUIT] stranded tid=") == 0) out.strandedLines++;
    }
    stop = true;
    for (auto& th : ths) th.join();
    Q::resetForTest();
    return out;
  };
  auto failsWith = [](const MarkOut& o, const char* why) {
    return o.code == Q::kExitFailed && o.reason == "threads-not-quiescent" && o.strandedLines == 0 &&
           o.failed.find(std::string("(not stranded: ") + why) != std::string::npos;
  };
  {
    MarkCase mc;
    mc.reenter = true;
    check(failsWith(markedQuit(mc), "the wait or its spin changed during the window"),
          "F7 the wait LEFT AND RE-ENTERED within the window (a new residence seq), every other condition held - not "
          "stranded: FAILED threads-not-quiescent 'the wait or its spin changed during the window'");
  }
  {
    MarkCase mc;
    mc.completed = kFence0;
    check(failsWith(markedQuit(mc), "the fence passed"),
          "F8 the fence already PASSED (completed == target: GpuBlockOnFence's own test true), every other condition "
          "held - not stranded: FAILED 'the fence passed'");
  }
  {
    MarkCase mc;
    const MarkOut o = markedQuit(mc);
    check(o.code == 0 && o.strandedLines == 1 && o.all.find(" dropped_fences=1 ") != std::string::npos &&
              o.all.find(" stranded=1 | ") != std::string::npos,
          "F9 the control: every condition held by one marked thread - stranded (dropped_fences=1 on the settle line), "
          "the quit completes (so each F7 / F8 / F13-F19 case fails on its ONE condition)");
  }
  {
    MarkCase mc;
    mc.kind = Q::kGpuWaitReadPtrWrap;  // native/quit-ringspace: the one wait left without a rule
    const MarkOut o = markedQuit(mc);
    check(o.code == Q::kExitFailed && o.reason == "threads-not-quiescent" && o.strandedLines == 0 &&
              o.failed.find("in GpuRingWaitReadPtrWrap") != std::string::npos &&
              o.failed.find("(not stranded:") == std::string::npos,
          "F11 a GpuRingWaitReadPtrWrap wait (the same residence, the CP drain over) has no stranded rule at all: FAILED "
          "threads-not-quiescent, the thread named with its wait and no stranded condition read for it");
  }
  // native/quit-ringspace: GpuRingWaitForSpace's conditions one by one - s3m_gate's wait (MarkCase's ring fields), every
  // other condition held; the control completes.
  auto ringCase = [] {
    MarkCase mc;
    mc.kind = Q::kGpuWaitRingSpace;
    return mc;
  };
  {
    const MarkOut o = markedQuit(ringCase());
    check(o.code == 0 && o.strandedLines == 1 && o.all.find(" dropped_progress=1 ") != std::string::npos &&
              o.all.find(" stranded=1 | ") != std::string::npos &&
              o.all.find(" wait=GpuRingWaitForSpace dev=70700000 target=BEBC32A0 completed=BEBC2A2D next=00001EBE seq=") !=
                  std::string::npos,
          "RS0 the ring-space control: s3m_gate's wait (progress BEBC2A2D frozen, [.., BEBC32A0) of generation 1EBE) and "
          "a dropped close's progress word that frees it (BEBC3301) - stranded, named target / completed / next, "
          "dropped_progress=1, the quit completes (so each RS case fails on its ONE condition)");
  }
  {
    MarkCase mc = ringCase();
    mc.tick = false;
    check(failsWith(markedQuit(mc), "not in its spin loop (its space test)"),
          "RS1 the ring wait never reached its spin loop (its space test, PRE): FAILED 'not in its spin loop'");
  }
  {
    MarkCase mc = ringCase();
    mc.exitZero = true;
    check(failsWith(markedQuit(mc), "its spin loop exited"),
          "RS2 GpuRingWaitProgress returned 0 in the ring wait's loop (the no-GPU mode / the hang trap): EXITED - FAILED");
  }
  {
    MarkCase mc = ringCase();
    mc.reenter = true;
    MarkCase nested = ringCase();
    nested.nested = true;
    const MarkOut n = markedQuit(nested);
    check(failsWith(markedQuit(mc), "the wait or its spin changed during the window") && n.code == Q::kExitFailed &&
              n.strandedLines == 0 &&
              (n.failed.find("(not stranded: the wait or its spin changed during the window") != std::string::npos ||
               n.failed.find("(not stranded: not in its spin loop") != std::string::npos),
          "RS3 the ring wait left and re-entered / a nested wait inside the window (a new residence): FAILED, never "
          "stranded across the excursion");
  }
  {
    MarkCase mc = ringCase();
    mc.spinBeforeCut = true;
    check(failsWith(markedQuit(mc), "its spin began before the ring cut"),
          "RS4 the ring wait's spin began BEFORE the ring cut (a ring hang the quit did not cause), its space freed by a "
          "dropped word all the same: FAILED 'its spin began before the ring cut'");
  }
  {
    MarkCase mc = ringCase();
    mc.progress = false;
    check(failsWith(markedQuit(mc), "its spin loop made no progress"),
          "RS5 the ring wait's loop stopped turning: FAILED 'its spin loop made no progress since the last check'");
  }
  {
    MarkCase same = ringCase(), atEnd = ringCase();
    same.ringP = 0xBEBC2A2Eu;   // the GPU already in generation 2 (= 1EBE & 3): difference 0
    atEnd.ringP = 0xBEBC32A1u;  // generation 1 (one behind), its cursor AT the end: end <= cursor
    check(failsWith(markedQuit(same), "the space is free") && failsWith(markedQuit(atEnd), "the space is free"),
          "RS6 the space already free by its own test on the frozen progress word - the generation difference 0, and 1 "
          "with the cursor at the end (end <= cursor): FAILED 'the space is free (its own test is true)'");
  }
  {
    MarkCase mc = ringCase();
    mc.noGpu = true;
    check(failsWith(markedQuit(mc), "the no-GPU mode is set"),
          "RS7 the no-GPU mode set (the CPU writes fb+4 there), every other condition held: FAILED 'the no-GPU mode is "
          "set'");
  }
  {
    MarkCase none = ringCase(), below = ringCase(), two = ringCase(), other = ringCase();
    none.dropProgress = 0u;            // nothing dropped
    below.dropProgress = 0xBEBC3001u;  // generation 1, its cursor below the end: would not free it
    two.dropProgress = 0xBEBC4000u;    // generation 0: two behind 1EBE - never frees, whatever its cursor
    other.dropDev = kDev + 0x10000u;   // Astra round 1 P2: the freeing word (BEBC3301) dropped from ANOTHER device's ring
    const char* why = "no progress word the ring cut dropped frees its space";
    check(failsWith(markedQuit(none), why) && failsWith(markedQuit(below), why) && failsWith(markedQuit(two), why) &&
              failsWith(markedQuit(other), why),
          "RS8 the dropped progress words do not free the space (none dropped; one generation behind with its cursor "
          "below the end; two generations behind; the freeing word dropped from another device): FAILED 'no progress "
          "word the ring cut dropped frees its space'");
  }
  {
    MarkCase mc = ringCase();
    mc.lockstep = true;
    const MarkOut o = markedQuit(mc);
    check(o.code == 0 && o.strandedLines == 1 && o.failed.empty(),
          "RS9 the ring waiter and the coordinator on ONE CPU (one turn per coordinator sleep): stranded, the quit "
          "completes");
  }
  {
    MarkCase mc;
    mc.spinBeforeCut = true;
    check(failsWith(markedQuit(mc), "its spin began before the ring cut"),
          "F13 (P1) its spin began BEFORE the ring cut - even with its fence recorded as dropped: FAILED 'its spin "
          "began before the ring cut' (a wait already spinning at the request is a hang the quit did not cause)");
  }
  {
    MarkCase mc;
    mc.dropFence = kFence0 + 2u;  // the cut dropped ANOTHER fence
    MarkCase none;
    none.dropFence = 0u;          // the cut dropped nothing
    check(failsWith(markedQuit(mc), "its fence was in no publish the ring cut dropped") &&
              failsWith(markedQuit(none), "its fence was in no publish the ring cut dropped"),
          "F14 (P1) its fence not among what the ring cut dropped (another fence dropped / nothing dropped), every "
          "other condition held: FAILED 'its fence was in no publish the ring cut dropped'");
  }
  {
    MarkCase mc;
    mc.tick = false;
    check(failsWith(markedQuit(mc), "not in its spin loop"),
          "F15 (P2) the wait never reached its spin loop (the fence test, the open-submission early return, the "
          "preliminary flush): FAILED 'not in its spin loop'");
  }
  {
    MarkCase mc;
    mc.exitZero = true;
    check(failsWith(markedQuit(mc), "its spin loop exited"),
          "F16 (P2) GpuRingWaitProgress returned 0 (the no-GPU mode / the hang trap): EXITED - FAILED 'its spin loop "
          "exited'");
  }
  {
    MarkCase mc;
    mc.progress = false;
    check(failsWith(markedQuit(mc), "its spin loop made no progress"),
          "F17 (P2) the spin loop stopped turning (one call after the cut, then none - a thread at an exit or "
          "descheduled there): FAILED 'its spin loop made no progress since the last check'");
  }
  {
    MarkCase mc;
    mc.noGpu = true;
    check(failsWith(markedQuit(mc), "the no-GPU mode is set"),
          "F18 (P2) the no-GPU mode (byte dev+0x2A39 & 2, where GpuRingWaitProgress returns 0) set, every other "
          "condition held: FAILED 'the no-GPU mode is set'");
  }
  {
    MarkCase mc;
    mc.nested = true;
    const MarkOut o = markedQuit(mc);
    check(o.code == Q::kExitFailed && o.strandedLines == 0 &&
              (o.failed.find("(not stranded: the wait or its spin changed during the window") != std::string::npos ||
               o.failed.find("(not stranded: not in its spin loop") != std::string::npos),
          "F19 (P2) a nested wait inside the window: the outer fence wait resumes as a NEW residence (fresh seq, PRE) - "
          "FAILED, never stranded across the excursion");
  }
  {
    MarkCase mc;
    mc.threads = 9;
    const MarkOut o = markedQuit(mc);
    int named = 0;
    for (const uint32_t t : o.tids)
      if (o.all.find("[QUIT] stranded tid=" + std::to_string(t) + " ") != std::string::npos) named++;
    check(o.code == 0 && o.strandedLines == 9 && named == 9 && o.all.find(" stranded=9 | ") != std::string::npos,
          "F20 (P2) nine stranded threads (Astra's case): nine [QUIT] stranded lines naming every tid, stranded=9 - "
          "past the old eight-record cap");
  }
  {
    MarkCase mc;
    mc.lockstep = true;
    const MarkOut o = markedQuit(mc);
    check(o.code == 0 && o.strandedLines == 1 && o.all.find(" stranded=1 | ") != std::string::npos &&
              o.failed.empty(),
          "F21 (round 3, Astra round 2 P2) the coordinator and the waiter on ONE CPU: the waiter turns only while the "
          "coordinator sleeps (one spin per sleep, never between two checks) - every census check follows a sleep, the "
          "one that ends the stable window is the record: stranded, the quit completes (a record taken straight after "
          "the stable check found no progress and restarted the window until the deadline: FAILED)");
  }
  {  // F10 the marks nest - and a resumed outer wait is a NEW residence (round 2, Astra P2)
    std::atomic<int> ok{0};
    std::thread th([&] {
      Q::GateSlot& s = Q::gate();
      constexpr uint32_t kOuterRec = kStack - 0x40u, kInnerRec = kStack - 0x1C0u;  // the outer / the nested wait's record
      const Q::GpuWaitMark outer = Q::gpuWaitEnter(Q::kGpuWaitFence, kDev, 0x77u, 0u, kOuterRec);
      const bool site = Q::gpuSpinSite(Q::kFenceSpinReturn, kOuterRec) == Q::kGpuWaitFence;
      Q::gpuSpinTick(kDev);
      const uint64_t seqOuter = s.gpuWaitSeq.load(), spinOuter = s.gpuSpinSeq.load();
      const bool spun = s.gpuWaitPhase.load() == Q::kGpuPhaseSpin && spinOuter > seqOuter;
      const Q::GpuWaitMark inner = Q::gpuWaitEnter(Q::kGpuWaitRingSpace, kDev, 0x40u, 0x1EBEu, kInnerRec);
      const bool in = s.gpuWait.load() == Q::kGpuWaitRingSpace && s.gpuWaitArg.load() == 0x40u &&
                      s.gpuWaitGen.load() == 0x1EBEu && s.gpuWaitRec.load() == kInnerRec &&
                      s.gpuWaitSeq.load() > spinOuter && s.gpuWaitPhase.load() == Q::kGpuPhasePre;
      // the outer's spin call (its lr, its record) inside the inner mark is not the inner wait's spin
      const bool notInner = Q::gpuSpinSite(Q::kFenceSpinReturn, kOuterRec) == 0u &&
                            Q::gpuSpinSite(0x820FCBA4u, kInnerRec) == Q::kGpuWaitRingSpace;
      Q::gpuSpinTick(kDev);  // the inner's own spin call: the ring wait promoted with a fresh spin number
      const bool innerSpun = s.gpuWaitPhase.load() == Q::kGpuPhaseSpin && s.gpuSpinSeq.load() > s.gpuWaitSeq.load();
      const uint64_t seqInner = s.gpuWaitSeq.load();
      Q::gpuWaitLeave(inner);
      const bool back = s.gpuWait.load() == Q::kGpuWaitFence && s.gpuWaitArg.load() == 0x77u &&
                        s.gpuWaitDev.load() == kDev && s.gpuWaitRec.load() == kOuterRec && s.gpuWaitGen.load() == 0u &&
                        s.gpuWaitSeq.load() > seqInner && s.gpuWaitPhase.load() == Q::kGpuPhasePre &&
                        s.gpuSpinSeq.load() == 0u;
      Q::gpuSpinTick(kDev);
      const bool respun = s.gpuWaitPhase.load() == Q::kGpuPhaseSpin && s.gpuSpinSeq.load() > spinOuter;
      Q::gpuWaitLeave(outer);
      const bool out = s.gpuWait.load() == Q::kGpuWaitNone && Q::gpuSpinSite(Q::kFenceSpinReturn, kOuterRec) == 0u;
      ok = site && spun && in && notInner && innerSpun && back && respun && out ? 1 : 2;
      if (ok != 1)
        std::printf("      F10: site %d spun %d in %d notInner %d innerSpun %d back %d respun %d out %d\n", site, spun, in,
                    notInner, innerSpun, back, respun, out);
    });
    th.join();
    check(ok == 1, "F10 the wait marks nest: an inner wait has its own residence (kind, dev, target, generation, "
                   "record); the outer's spin call (its return address, its record) is not the inner's spin; leaving "
                   "it resumes the outer wait as a NEW residence - a fresh seq, PRE, no spin - so an unchanged seq "
                   "means an uninterrupted residence; leaving the outer clears the slot");
  }
}

// ── T: what the ring cut dropped (native/quit-drain round 2, Astra P1) ─────────────────────────────────────────────
void dropTests() {
  Q::resetForTest();
  fenceFixture(kFence0 + 2u, kFence0 - 2u);
  writeSegment(kFence0);
  const uint32_t phys = mangle(kSeg);
  bool t1 = Q::ibTailFence(kDev, phys, kSegDwords) == kFence0;
  t1 = t1 && Q::ibTailFence(kDev, phys + 0x40u, kSegDwords) == 0u;  // not the close's own segment
  t1 = t1 && Q::ibTailFence(kDev, phys, 9u) == 0u;                   // too short to end in the fence tail
  t1 = t1 && Q::ibTailFence(0u, phys, kSegDwords) == 0u;
  writeSegment(kFence0, 0xC0003B00u);                                // not EVENT_WRITE_SHD
  t1 = t1 && Q::ibTailFence(kDev, phys, kSegDwords) == 0u;
  writeSegment(kFence0, 0xC0025800u, mangle(kFb + 4u) | 2u);         // the progress word's packet, not fb+0
  t1 = t1 && Q::ibTailFence(kDev, phys, kSegDwords) == 0u;
  writeSegment(kFence0);
  check(t1, "T1 ibTailFence: a dropped IB carries the fence of its own tail (the close's segment at dev+0x34C4, the "
            "last EVENT_WRITE_SHD {3, mangled(fb)|2, F}); another segment, a short IB, another packet or the progress "
            "word's carries none");
  Q::recordDroppedIb(kDev, phys, kSegDwords);  // before any cut: never recorded
  const bool none = !Q::droppedFence(kDev, kFence0) && Q::droppedFenceCount() == 0u;
  std::mutex mu;
  Q::cutRingUnder(mu);
  Q::recordDroppedIb(kDev, phys, kSegDwords);
  Q::recordDroppedIb(kDev, phys, kSegDwords);  // the same fence twice: one record
  Q::recordDroppedFence(kDev, 0u);             // a publish that carried none
  check(none && Q::droppedFence(kDev, kFence0) && Q::droppedFenceCount() == 1u && !Q::droppedFence(kDev, kFence0 + 2u) &&
            !Q::droppedFence(kDev + 0x10000u, kFence0) && Q::droppedFenceOverflow() == 0u,
        "T2 recorded only once the ring is cut, per device, once per fence; a publish with no fence records nothing");
  for (uint32_t i = 0; i < 1100u; ++i) Q::recordDroppedFence(kDev, 0x00100001u + 2u * i);
  check(Q::droppedFenceCount() == 1024u && Q::droppedFenceOverflow() == 77u &&
            Q::droppedFence(kDev, 0x00100001u + 2u * 1022u) && !Q::droppedFence(kDev, 0x00100001u + 2u * 1023u),
        "T3 a full device table (1024): the rest counted as overflow, never recorded - their waits stay FAILED");
  Q::resetForTest();
  Q::cutRingUnder(mu);
  for (uint32_t d = 0; d < 5u; ++d) Q::recordDroppedFence(0x71000000u + d * 0x10000u, 0x11u);
  check(Q::droppedFenceCount() == 4u && Q::droppedFenceOverflow() == 1u && !Q::droppedFence(0x71040000u, 0x11u),
        "T4 four device tables; a fifth device's drop is overflow, never recorded");
  Q::resetForTest();
  // RW3 (native/quit-ringspace): the tail's progress packet - read only when it IS GpuBuildFenceWritePacket's first
  // EVENT_WRITE_SHD {3, mangled(fb+4)|2, P}; otherwise the fence is still recorded, the progress word not (fail closed)
  fenceFixture(kFence0 + 2u, kFence0 - 2u);
  constexpr uint32_t kP = 0xBEBC3301u;
  uint32_t pw = 1u;
  writeTail(kFence0, kP);
  bool r3 = Q::ibTailFence(kDev, phys, kSegDwords, &pw) == kFence0 && pw == kP;
  Q::recordDroppedIb(kDev, phys, kSegDwords);  // before any cut: neither recorded
  r3 = r3 && Q::droppedProgressCount() == 0u && !Q::droppedProgress(kDev, kP);
  Q::recordDroppedProgress(kDev, kP);
  r3 = r3 && Q::droppedProgressCount() == 0u;
  Q::cutRingUnder(mu);
  Q::recordDroppedIb(kDev, phys, kSegDwords);
  Q::recordDroppedIb(kDev, phys, kSegDwords);  // the same close twice: one record each
  r3 = r3 && Q::droppedFence(kDev, kFence0) && Q::droppedProgress(kDev, kP) && Q::droppedProgressCount() == 1u &&
       Q::droppedFenceCount() == 1u && !Q::droppedProgress(kDev + 0x10000u, kP);
  struct Broken {
    uint32_t header, count, addr;
  };
  const Broken broken[] = {{0xC0003B00u, 3u, 0u},                    // not EVENT_WRITE_SHD
                           {0xC0025800u, 2u, 0u},                    // another event
                           {0xC0025800u, 3u, mangle(kFb) | 2u},      // the fence's address, not fb+4's
                           {0xC0025800u, 3u, mangle(kFb + 8u) | 2u}};  // another word of the block
  for (const Broken& b : broken) {
    Q::resetForTest();
    Q::cutRingUnder(mu);
    writeTail(kFence0, kP, b.header, b.count, b.addr);
    pw = 1u;
    const bool f = Q::ibTailFence(kDev, phys, kSegDwords, &pw) == kFence0;
    Q::recordDroppedIb(kDev, phys, kSegDwords);
    r3 = r3 && f && pw == 0u && Q::droppedFence(kDev, kFence0) && Q::droppedProgressCount() == 0u;
  }
  check(r3, "RW3 ibTailFence reads the progress word from the tail's first EVENT_WRITE_SHD {3, mangled(fb+4)|2, P} - "
            "recorded with the fence once the ring is cut (never before), once per value, per device; a tail whose "
            "progress packet is not that one (another packet, another event, the fence's or another address) records "
            "the fence and NO progress word");
  Q::resetForTest();
  Q::cutRingUnder(mu);
  for (uint32_t i = 0; i < 1100u; ++i) Q::recordDroppedProgress(kDev, 0xBEBC0001u + 4u * i);
  check(Q::droppedProgressCount() == 1024u && Q::droppedFenceOverflow() == 76u &&
            Q::droppedProgress(kDev, 0xBEBC0001u + 4u * 1023u) && !Q::droppedProgress(kDev, 0xBEBC0001u + 4u * 1024u),
        "RW3b a full progress table (1024): the rest counted in the shared overflow, never recorded - their waits stay "
        "FAILED");
  Q::resetForTest();
}

// ── PT: GpuRingWaitProgress's override (kernel/gpu_hang_wd.cpp) is a pass-through (round 2) ────────────────────────
// A NEW strong-def on the default path - every GPU busy-wait for the whole game calls it. From the same guest memory
// and registers, the override and the bare generated body must leave the same r3, the same whole context and the same
// guest memory; only this thread's quit gate slot may differ, and only for a call from GpuBlockOnFence's spin loop.
void passThroughTests() {
  Q::resetForTest();
  Q::resetGatesForTest();
  uint8_t* const base = sr::kernel::getGuestBase();
  // the wait record {dev, reason, last completed, tick seed} in the CALLER's frame, above r1 (as GpuBlockOnFence's)
  constexpr uint32_t kRec = kStack - 0x100u, kSp = kStack - 0x200u;
  struct State {
    const char* name;
    uint32_t last, completed;
    bool noGpu;
    uint32_t lr;
    uint32_t r3, phase;  // the body's return; the wait's phase after the override's call
    uint32_t kind = Q::kGpuWaitFence;
    uint32_t markRec = kRec;  // the record the wait's mark holds (kRec: the call's own; else a foreign one)
  };
  // native/quit-ringspace: the ring wait's states (its spin call = its record alone, ctx.lr stale), and the hardened
  // fence site (return address AND record)
  constexpr uint32_t kOther = kRec - 0x140u;  // another wait's record (a nested wait's, deeper in the stack)
  const State states[] = {
      {"progress (the fence moved: rec updated)", kFence0 - 4u, kFence0 - 2u, false, Q::kFenceSpinReturn, 1u,
       Q::kGpuPhaseSpin},
      {"steady (no progress)", kFence0 - 2u, kFence0 - 2u, false, Q::kFenceSpinReturn, 1u, Q::kGpuPhaseSpin},
      {"no-GPU mode (returns 0)", kFence0 - 2u, kFence0 - 2u, true, Q::kFenceSpinReturn, 0u, Q::kGpuPhaseExited},
      {"another caller (a ring wait's call site)", kFence0 - 4u, kFence0 - 2u, false, 0x820FCB98u, 1u,
       Q::kGpuPhasePre},
      {"fence site's lr, a foreign record (a stale lr in PRE)", kFence0 - 4u, kFence0 - 2u, false, Q::kFenceSpinReturn,
       1u, Q::kGpuPhasePre, Q::kGpuWaitFence, kOther},
      {"ring wait, its record (lr stale at 0x8271EE0C)", kFence0 - 4u, kFence0 - 2u, false, 0x8271EE0Cu, 1u,
       Q::kGpuPhaseSpin, Q::kGpuWaitRingSpace},
      {"ring wait, its record, stale lr 0x820FDA44", kFence0 - 2u, kFence0 - 2u, false, Q::kFenceSpinReturn, 1u,
       Q::kGpuPhaseSpin, Q::kGpuWaitRingSpace},
      {"ring wait, no-GPU mode (returns 0)", kFence0 - 2u, kFence0 - 2u, true, 0x8271EE0Cu, 0u, Q::kGpuPhaseExited,
       Q::kGpuWaitRingSpace},
      {"ring wait, a foreign record", kFence0 - 4u, kFence0 - 2u, false, 0x8271EE0Cu, 1u, Q::kGpuPhasePre,
       Q::kGpuWaitRingSpace, kOther},
      {"GpuRingWaitReadPtrWrap, its record (no rule: never marked)", kFence0 - 4u, kFence0 - 2u, false, 0x820FD1D0u,
       1u, Q::kGpuPhasePre, Q::kGpuWaitReadPtrWrap},
  };
  struct Region {
    uint32_t va, n;
  };
  const Region regions[] = {{kDev, 0x5000u}, {kFb, 0x100u}, {kKpcr, 0x2000u}, {kStack - 0x2000u, 0x2100u}};
  auto snap = [&](std::vector<uint8_t>& out) {
    out.clear();
    for (const Region& r : regions) {
      const uint8_t* p = sr::kernel::guestVAToHost(r.va);
      out.insert(out.end(), p, p + r.n);
    }
  };
  auto setup = [&](const State& s) {
    fenceFixture(kFence0 + 2u, s.completed);
    std::memset(sr::kernel::guestVAToHost(kStack - 0x2000u), 0, 0x2100u);
    if (s.noGpu) wr32(kDev + 0x2A38u, rd32(kDev + 0x2A38u) | 0x00020000u);
    wr32(kRec, kDev);
    wr32(kRec + 4u, 10u);
    wr32(kRec + 8u, s.last);
    wr32(kRec + 12u, 0u);
  };
  bool all = true, r4 = true;
  int foreign = 0;
  for (const State& s : states) {
    PPCContext a{}, b{};
    std::vector<uint8_t> ma, mb;
    uintptr_t where = 0, data = 0;
    setup(s);
    a.r1.u64 = kSp;
    a.r13.u64 = kKpcr;
    a.r3.u64 = kRec;
    a.lr = s.lr;
    const int ea = guardedCall(&__imp__sub_820F33E8, a, base, &where, &data);  // the bare body
    snap(ma);
    setup(s);
    b.r1.u64 = kSp;
    b.r13.u64 = kKpcr;
    b.r3.u64 = kRec;
    b.lr = s.lr;
    Q::GateSlot& g = Q::gate();
    const Q::GpuWaitMark m = s.kind == Q::kGpuWaitRingSpace ? Q::gpuWaitEnter(s.kind, kDev, 0xBEBC32A0u, 0x1EBEu, s.markRec)
                                                            : Q::gpuWaitEnter(s.kind, kDev, kFence0, 0u, s.markRec);  // PRE
    const int eb = guardedCall(&sub_820F33E8, b, base, &where, &data);  // the override
    const uint32_t phase = g.gpuWaitPhase.load();
    Q::gpuWaitLeave(m);
    snap(mb);
    const bool same = ea == 0 && eb == 0 && a.r3.u32 == s.r3 && b.r3.u32 == a.r3.u32 &&
                      std::memcmp(&a, &b, sizeof(PPCContext)) == 0 && ma == mb && phase == s.phase;
    if (!same)
      std::printf("      PT %s: bare ex=%d r3=%u | override ex=%d r3=%u ctx %s mem %s phase %u (want r3 %u phase %u)\n",
                  s.name, ea, a.r3.u32, eb, b.r3.u32, std::memcmp(&a, &b, sizeof(PPCContext)) ? "DIFFER" : "equal",
                  ma == mb ? "equal" : "DIFFER", phase, s.r3, s.phase);
    all = all && same;
    if (s.markRec != kRec) {  // RW4: a call with another record never promotes the innermost wait
      foreign++;
      r4 = r4 && eb == 0 && phase == Q::kGpuPhasePre;
    }
  }
  check(r4 && foreign == 2,
        "RW4 (native/quit-ringspace) the real override, called with a record that is not the innermost wait's - a ring "
        "wait's mark (a nested waiter's call), a fence wait in PRE with the fence site's return address 0x820FDA44 left "
        "stale (the hardening) - never promotes it: the wait stays PRE");
  check(all, "PT1 GpuRingWaitProgress 820F33E8's override = the bare body: the same r3, the same whole context, the "
             "same guest memory (the device, the fence block, the KPCR, the stack and its wait record) in ten states - "
             "the fence moved, steady, the no-GPU mode, another caller, the fence site's lr with a foreign record; the "
             "ring wait through its record (lr stale, twice), its no-GPU mode, a foreign record; a ReadPtrWrap wait; "
             "only the quit slot moves (SPIN / EXITED from the innermost wait's own spin call, untouched otherwise)");
  Q::resetForTest();
}

// ── NC: the ring cut drops a NATIVE publication (round 3, Astra round 2 P1) ─────────────────────────────────────────
// Record mode leaves the guest's bytes under a native footprint untouched, so a reused segment can still end in an
// older close's EVENT_WRITE_SHD ... F while the native close publishing it carries S. The production path, whole:
// publishSegment (as segmentClose calls it, kernel/native_flush.cpp) -> the generated GpuRingAppendJumpSegment
// 820FD800 (out of chain mode) -> 820FD218's strong-def (kernel/gpu_ring_doorbell.cpp) -> ringless updateWritePointer's
// cut branch -> publishSegment's own cut branch.
int guardedPublish(PPCContext& c, uint8_t* base, uint32_t cursorEnd, uint32_t phys, uint32_t fence, uint32_t* after,
                   uint32_t progress = 0u) {
  __try {
    *after = sr::native::flush::publishSegmentForTest(c, base, kDev, cursorEnd, phys, kSegDwords, fence, progress);
    return 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return int(GetExceptionCode());
  }
}

void nativeCutTests() {
  const bool path = sr::kernel::gpuCpActive() && sr::kernel::ringless::gateOn() && sr::native::flush::segmentlessOn();
  check(path, "NC0 the production drop path is live in this process (the async CP, the ringless gate and segmentless "
              "on: 820FD218's strong-def takes the publish) - NC1 / NC2 run through it");
  if (!path) return;
  uint8_t* const base = sr::kernel::getGuestBase();
  const uint32_t phys = mangle(kSeg), cursorEnd = kSeg + kSegDwords * 4u, F = kFence0, S = kFence0 + 8u;
  constexpr uint32_t kStaleP = 0xBEBC2A2Du, kCloseP = 0xBEBC3302u;  // the old close's progress word / this close's
  constexpr uint32_t kRing = 0x70760000u, kDesc = kStack - 0x100u, kSp = kStack - 0x200u;
  std::mutex mu;
  auto fixture = [&] {
    Q::resetForTest();
    fenceFixture(S + 2u, F - 2u);
    std::memset(sr::kernel::guestVAToHost(kStack - 0x2000u), 0, 0x2100u);
    wr32(kDev + 0x34A4u, kRing);  // the ring (base, mask in dwords, write index): never touched under the cut
    wr32(kDev + 0x34A8u, 0x3FFu);
    wr32(kDev + 0x2A44u, 0u);
    writeTail(F, kStaleP);  // the reused segment still ends in an older close's fence tail {..P_stale.., ..F}
    Q::cutRingUnder(mu);
    Q::lockAcquiring();  // a lock holder before the thread cut: its doorbell is dropped and it runs on (doorbellCut)
  };
  {
    fixture();
    const uint64_t drops0 = Q::doorbellDrops();
    PPCContext c{};
    c.r1.u64 = kSp;
    c.r13.u64 = kKpcr;
    uint32_t after = 0;
    const int ex = guardedPublish(c, base, cursorEnd, phys, S, &after);
    const bool ok = ex == 0 && after == cursorEnd && Q::doorbellDrops() == drops0 + 1u && Q::droppedFence(kDev, S) &&
                    !Q::droppedFence(kDev, F) && Q::droppedFenceCount() == 1u &&
                    Q::droppedProgressCount() == 0u;  // nor the stale tail's progress word (the close passed none)
    if (!ok)
      std::printf("      NC1: ex=%08X after=%08X drops +%llu S %d F %d count %u\n", unsigned(ex), after,
                  (unsigned long long)(Q::doorbellDrops() - drops0), int(Q::droppedFence(kDev, S)),
                  int(Q::droppedFence(kDev, F)), Q::droppedFenceCount());
    Q::lockReleasing();
    check(ok, "NC1 (Astra round 2 P1) a native segmentless close carrying S publishes a reused segment whose guest bytes "
              "still end in an older close's EVENT_WRITE_SHD ... F, and the ring cut drops it (the production "
              "publishSegment -> 820FD800 -> 820FD218's strong-def -> ringless cut branch): only S is recorded - the "
              "native publication's bytes are never read for a fence");
  }
  {
    fixture();
    const uint64_t drops0 = Q::doorbellDrops();
    wr32(kDesc, kSegDwords);  // GpuRingUpdateWritePointer's descriptor {dwords, phys}
    wr32(kDesc + 4u, phys);
    PPCContext c{};
    c.r1.u64 = kSp;
    c.r13.u64 = kKpcr;
    c.r3.u64 = kDev;
    c.r4.u64 = kDesc;
    uintptr_t where = 0, data = 0;
    const int ex = guardedCall(&sub_820FD218, c, base, &where, &data);
    const bool ok = ex == 0 && Q::doorbellDrops() == drops0 + 1u && Q::droppedFence(kDev, F) &&
                    Q::droppedFenceCount() == 1u;
    if (!ok)
      std::printf("      NC2: ex=%08X drops +%llu F %d count %u\n", unsigned(ex),
                  (unsigned long long)(Q::doorbellDrops() - drops0), int(Q::droppedFence(kDev, F)),
                  Q::droppedFenceCount());
    Q::lockReleasing();
    check(ok, "NC2 the control: the same segment and tail as a PLAIN IB (no native hand-off) through the same "
              "strong-def under the cut - its bytes are its packets: F is recorded (the tail scan still runs for the "
              "guest's own closes)");
    check(ok && Q::droppedProgress(kDev, kStaleP) && Q::droppedProgressCount() == 1u,
          "NC2b (native/quit-ringspace) ... and the plain IB's progress word is its tail's (P_stale here: its bytes ARE "
          "its packets)");
  }
  {  // RW2 (native/quit-ringspace): the native close's progress word, from the close itself, never from the bytes
    fixture();
    PPCContext c{};
    c.r1.u64 = kSp;
    c.r13.u64 = kKpcr;
    uint32_t after = 0;
    const int ex = guardedPublish(c, base, cursorEnd, phys, S, &after, kCloseP);
    const bool ok = ex == 0 && after == cursorEnd && Q::droppedFence(kDev, S) && Q::droppedProgress(kDev, kCloseP) &&
                    !Q::droppedProgress(kDev, kStaleP) && Q::droppedProgressCount() == 1u &&
                    Q::droppedProgressFrees(kDev, 0xBEBC32A0u, 0x1EBEu);
    if (!ok)
      std::printf("      RW2: ex=%08X after=%08X S %d P %d stale %d count %u\n", unsigned(ex), after,
                  int(Q::droppedFence(kDev, S)), int(Q::droppedProgress(kDev, kCloseP)),
                  int(Q::droppedProgress(kDev, kStaleP)), Q::droppedProgressCount());
    Q::lockReleasing();
    // the coherency block's publish carries neither (publishSegment's 0, 0): nothing recorded
    fixture();
    PPCContext c2{};
    c2.r1.u64 = kSp;
    c2.r13.u64 = kKpcr;
    const int ex2 = guardedPublish(c2, base, cursorEnd, phys, 0u, &after, 0u);
    const bool none = ex2 == 0 && Q::droppedProgressCount() == 0u && Q::droppedFenceCount() == 0u;
    Q::lockReleasing();
    check(ok && none,
          "RW2 a native segmentless close carrying S and its progress word P, dropped by the ring cut through the "
          "production path (publishSegment -> 820FD800 -> 820FD218's strong-def -> the cut branch): P recorded (it "
          "frees s3m_gate's wait), the reused segment's stale tail word never; a publish carrying none (the coherency "
          "block) records nothing");
  }
  Q::resetForTest();
}

void importTest() {
  Q::resetForTest();
  check(Q::releasesLock("RtlLeaveCriticalSection") && Q::releasesLock("KfLowerIrql") &&
            Q::releasesLock("KeReleaseSpinLockFromRaisedIrql") && !Q::releasesLock("RtlEnterCriticalSection") &&
            !Q::releasesLock("KeWaitForSingleObject") && !Q::releasesLock(nullptr) &&
            Q::isLockAcquire("KfAcquireSpinLock") && Q::isLockAcquire("KeAcquireSpinLockAtRaisedIrql") &&
            Q::isLockAcquire("RtlEnterCriticalSection") && !Q::isLockAcquire("RtlTryEnterCriticalSection") &&
            !Q::isLockAcquire("KeWaitForSingleObject") && !Q::isLockAcquire(nullptr),
        "I1 the release list and the lock-acquire list");
  Q::g_threadCut.store(true);
  std::atomic<int> state{0};
  std::thread exemptT([&] {  // VD_ISR / the audio worker inside a guest callback after the thread cut
    Q::markExempt("TEST_CP");
    kimport("KeWaitForSingleObject", [] {});
    state |= 1;
  });
  std::thread releaseT([&] {  // RtlLeaveCriticalSection's thunk with nothing counted: passes the entry, parks after
    kimport("RtlLeaveCriticalSection", [&] { state |= 2; });
    state |= 32;
  });
  std::thread parkT([&] {
    kimport("KeWaitForSingleObject", [&] { state |= 64; });
    state |= 4;
  });
  std::thread holderT([&] {  // holds a lock (EndSync's KeSetEvent under its spinlock): parks at its next gate
    Q::lockAcquiring();
    kimport("KeSetEvent", [&] { state |= 8; });
    state |= 128;
  });
  Sleep(80);
  check(!(state & 1), "I2 after the thread cut an exempt loop parks at an import gate too (the CP drain is over)");
  check((state & 2) && !(state & 32), "I3 a release passes the entry gate (it releases) and parks at its exit");
  check(!(state & 64) && !(state & 4), "I4 an ordinary import parks the thread at its entry (the import never runs)");
  check(!(state & 8) && !(state & 128),
        "I5 smoke1 (2): a lock holder parks at its next gate after the thread cut - no pass-through (it would run "
        "guest code holding the lock: a whole frame)");
  check(Q::parkedCount() == 4, "I6 four parked (exempt loop, release exit, ordinary entry, lock holder)");
  Q::resetForTest();
  exemptT.join();
  releaseT.join();
  parkT.join();
  holderT.join();
  {
    std::thread waitT([&] {  // entered before the cut, leaves after it: the exit gate parks it
      const char* prev = Q::importEnter("KeWaitForSingleObject");
      Q::g_threadCut.store(true);
      Q::importLeave("KeWaitForSingleObject", prev);
      state |= 256;
    });
    Sleep(60);
    check(!(state & 256) && Q::parkedCount() == 1, "I7 a thread leaving an import after the cut parks at the exit");
    Q::resetForTest();
    waitT.join();
  }
  {
    std::atomic<int> s2{0};
    std::thread clampT([&] {
      Q::lockReleasing();  // nothing held: stays 0
      Q::lockAcquiring();  // RtlTryEnterCriticalSection's thunk: counted, then the failed try uncounts it
      Q::lockReleasing();
      s2 = Q::heldLocks() == 0 ? 1 : 2;
    });
    clampT.join();
    check(s2 == 1, "I8 the lock count never goes negative; a failed try leaves it at 0");
  }
  {
    Q::g_threadCut.store(true);
    std::atomic<int> s3{0};
    std::thread hostT([&] {
      Q::markHostLoop();
      Q::guestCallEnter();
      s3 = 1;
      Q::guestCallLeave();
    });
    Sleep(60);
    check(s3 == 0 && Q::parkedCount() == 1, "I9 a host loop parks at its next guest call");
    Q::resetForTest();
    hostT.join();
  }
  Q::resetForTest();
  check(Q::claimExit(), "I10 a window close with no quit in progress exits");
  Q::resetForTest();
  Q::setQuittingForTest();
  check(!Q::claimExit(), "I11 a window close DURING a quit does not exit (the quit owns the process exit)");
  Q::resetForTest();
}

// ── E: the real named events ─────────────────────────────────────────────────────────────────────────────────────
std::atomic<int> g_requests{0};
void onRequest(void*) {
  g_requests++;
  Q::setAckForTest();
}

void eventTest() {
  const uint32_t pid = GetCurrentProcessId();
  const std::string inst = "qtest" + std::to_string(pid);
  check(Q::openChannelForTest(inst.c_str(), pid, onRequest, nullptr), "E1 the channel is created for this PID");
  HANDLE wrong = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, Q::requestEventName(inst.c_str(), pid + 1).c_str());
  check(wrong == nullptr, "E2 another PID's name opens nothing (a harness can only signal the game it launched)");
  if (wrong) CloseHandle(wrong);
  HANDLE req = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, Q::requestEventName(inst.c_str(), pid).c_str());
  HANDLE ack = OpenEventA(SYNCHRONIZE, FALSE, Q::ackEventName(inst.c_str(), pid).c_str());
  check(req && ack, "E3 the harness opens both events by name");
  check(ack && WaitForSingleObject(ack, 0) == WAIT_TIMEOUT, "E4 the ack starts unsignalled");
  if (req) SetEvent(req);
  const bool acked = ack && WaitForSingleObject(ack, 3000) == WAIT_OBJECT_0;
  check(acked && g_requests == 1, "E5 setting the request reaches the watcher once, and the harness sees the ack");
  Q::closeChannelForTest();
  if (req) CloseHandle(req);
  if (ack) CloseHandle(ack);
}

// ── W5 / W6: the production wiring, by source (difftest runs from the tree root) ───────────────────────────────────
std::string readFile(const char* path) {
  std::string s;
  if (FILE* f = std::fopen(path, "rb")) {
    char buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) s.append(buf, n);
    std::fclose(f);
  }
  return s;
}
// The text of one function, from its signature to the first line that is a lone "}".
std::string bodyOf(const std::string& src, const std::string& signature) {
  const size_t a = src.find(signature);
  if (a == std::string::npos) return "";
  size_t b = src.find("\n}", a);
  return src.substr(a, (b == std::string::npos ? src.size() : b) - a);
}
bool before(const std::string& s, const char* first, const char* second) {
  const size_t a = s.find(first), b = s.find(second);
  return a != std::string::npos && b != std::string::npos && a < b;
}

void wiringTest() {
  const std::string glue = readFile("guest/ppc_import_glue.cpp");
  bool locks = !glue.empty();
  for (const char* fn : {"KfAcquireSpinLock", "KeAcquireSpinLockAtRaisedIrql", "RtlEnterCriticalSection",
                         "RtlTryEnterCriticalSection"})
    locks = locks && before(bodyOf(glue, std::string("PPC_FUNC(__imp__") + fn + ")"), "lockAcquiring()", "callKernel(");
  for (const char* fn : {"KfReleaseSpinLock", "KeReleaseSpinLockFromRaisedIrql", "RtlLeaveCriticalSection"})
    locks = locks && before(bodyOf(glue, std::string("PPC_FUNC(__imp__") + fn + ")"), "lockReleasing()", "callKernel(");
  const std::string tryEnter = bodyOf(glue, "PPC_FUNC(__imp__RtlTryEnterCriticalSection)");
  locks = locks && tryEnter.find("lockReleasing()", tryEnter.find("callKernel(")) != std::string::npos;
  check(locks, "W5 the seven lock thunks count before their import (acquire +1, release -1; a failed try uncounts)");
  const std::string ck = bodyOf(glue, "uint32_t callKernel(const char* name");
  const size_t exitGate = ck.find("importLeave(name, quitPrev)");
  check(before(ck, "importEnter(name)", "lookupCached(name)") && exitGate != std::string::npos &&
            exitGate > ck.find("result = fn(") && exitGate < ck.find("processGuestAPCs(ctx") &&
            ck.find("importLeave(name, quitPrev)", ck.find("__except")) != std::string::npos,
        "W6 callKernel: the entry gate before the export, the exit gate right after it - before any guest APC - and "
        "on the exception path too");
  const std::string cp = readFile("kernel/gpu_submit.cpp");
  const std::string door = bodyOf(cp, "void gpuCpDoorbell(");
  check(door.find("publishUnlessCut(c.mu,") != std::string::npos && door.find("c.wptr = writeIndex") != std::string::npos &&
            before(door, "publishUnlessCut(c.mu,", "c.wptr = writeIndex") &&
            cp.find("void gpuCpCutRing() { sr::kernel::quit::cutRingUnder(cp().mu); }") != std::string::npos,
        "W7 gpuCpDoorbell publishes inside publishUnlessCut on c.mu; gpuCpCutRing cuts under the same mutex");
  const std::string rl = readFile("kernel/gpu_ringless.cpp");
  const std::string uwp = bodyOf(rl, "bool updateWritePointer(PPCContext& ctx, uint8_t* base)");
  const std::string bs = bodyOf(rl, "PPC_FUNC(sub_82110140)");
  check(before(uwp, "quit::g_ringCut", "sub_820FD150(ctx, base)") && before(uwp, "quit::g_ringCut", "gpuCpPushSegment(") &&
            before(uwp, "quit::g_ringCut", "gpuCpPushItem(1u") && before(bs, "quit::g_ringCut", "gpuCpPushItem(2u") &&
            before(bs, "quit::g_ringCut", "writeGuestBE32(S + 0x16Cu"),
        "W8 smoke1 (2): the ringless producers test the ring cut before they queue an item (rl_pending 0->2 after "
        "the cut) - the IB / segment publish and the bin select");
  const std::string wd = readFile("kernel/gpu_hang_wd.cpp");
  bool marks = !wd.empty();
  const char* const waits[][2] = {{"PPC_FUNC(sub_820FD9A8)", "kGpuWaitFence"},
                                  {"PPC_FUNC(sub_820FCB30)", "kGpuWaitRingSpace"},
                                  {"PPC_FUNC(sub_820FD150)", "kGpuWaitReadPtrWrap"}};
  for (const auto& w : waits) {
    const std::string body = bodyOf(wd, w[0]);
    const std::string fn = std::string(w[0]).substr(9, 12);  // sub_820FD9A8
    const std::string call = "__imp__" + fn + "(ctx, base)";
    const std::string enter = std::string("gpuWaitEnter(sr::kernel::quit::") + w[1] + ", ctx.r3.u32, ctx.r4.u32";
    marks = marks && before(body, enter.c_str(), call.c_str()) &&
            body.find("sr::kernel::quit::gpuWaitLeave(qw)", body.find(call)) != std::string::npos;
    // native/quit-ringspace: the fence and ring-space marks carry the wait's record (the caller's r1 - 0x40), the
    // ring-space mark its generation r5; GpuRingWaitReadPtrWrap's carries neither (no rule: its spin never recognised)
    const size_t at = body.find(enter);
    const std::string rest = at == std::string::npos ? std::string() : body.substr(at + enter.size(), 160);
    const bool rec = rest.find("ctx.r1.u32 - sr::kernel::quit::kWaitRecBelowSp)") != std::string::npos;
    if (std::string(w[1]) == "kGpuWaitFence") marks = marks && rec && rest.rfind(", 0u,", 0) == 0;
    else if (std::string(w[1]) == "kGpuWaitRingSpace") marks = marks && rec && rest.rfind(", ctx.r5.u32,", 0) == 0;
    else marks = marks && !rec && rest.rfind(");", 0) == 0;
  }
  check(marks, "W9 native/quit-drain: the three XDK GPU-wait strong-defs mark the thread (kind, dev r3, target r4) "
               "before the generated body and restore after it; native/quit-ringspace: the fence and ring-space marks "
               "carry the wait's record (r1 - kWaitRecBelowSp), the ring-space mark its generation r5, ReadPtrWrap's "
               "neither");
  // round 2: the spin-phase marks - GpuRingWaitProgress's pass-through strong-def
  const std::string gp = bodyOf(wd, "PPC_FUNC(sub_820F33E8)");
  const size_t gpImp = gp.find("\n  __imp__sub_820F33E8(ctx, base);");  // a statement of its own, unconditional
  check(gpImp != std::string::npos && gp.find("__imp__sub_820F33E8(", gpImp + 10) == std::string::npos &&
            gp.find("sr::kernel::quit::gpuSpinSite(uint32_t(ctx.lr), ctx.r3.u32)") != std::string::npos &&
            before(gp, "sr::kernel::quit::gpuSpinSite(", "sr::kernel::quit::gpuSpinTick(") &&
            before(gp, "sr::kernel::quit::gpuSpinTick(", "__imp__sub_820F33E8(ctx, base)") &&
            gp.find("sr::kernel::quit::gpuSpinExit()", gpImp) != std::string::npos &&
            gp.find("ctx.r3.u64 =") == std::string::npos && gp.find("writeGuest") == std::string::npos,
        "W10 rounds 2 / ringspace: GpuRingWaitProgress 820F33E8's strong-def delegates exactly once, unconditionally, "
        "writes no guest register or memory; the spin tick (the innermost wait's own spin call, gpuSpinSite: its lr "
        "and record) before the body, the exit mark after");
  // round 2: the drop sites record what the dropped publish carried
  const std::string uwpCut = uwp.substr(0, uwp.find("sub_820FD150(ctx, base)"));
  check(before(uwpCut, "quit::g_ringCut", "quit::recordDroppedIb(dev, phys, dwords)") &&
            before(uwpCut, "quit::recordDroppedIb(dev, phys, dwords)", "quit::doorbellCut()") &&
            before(uwpCut, "cutPublicationKind(phys, dwords) == 0", "quit::recordDroppedIb(dev, phys, dwords)"),
        "W11 rounds 2 / 3: ringless updateWritePointer's cut branch records the dropped IB's fence (recordDroppedIb) "
        "before the cut rule (which may park the thread), and only for a plain IB (cutPublicationKind 0; NC1 / NC2 "
        "drive the behaviour)");
  const std::string nf = readFile("kernel/native_flush.cpp");
  const std::string ps = bodyOf(nf, "uint32_t publishSegment(Driver& dr, uint32_t cursorEnd, uint32_t phys, uint32_t dwords,");
  const size_t cutBranch = ps.find("} else if (ringCut()) {");
  check(cutBranch != std::string::npos &&
            ps.find("sr::kernel::quit::recordDroppedFence(dr.dev, fence);", cutBranch) != std::string::npos &&
            ps.find("sr::kernel::quit::recordDroppedFence(dr.dev, fence);", cutBranch) <
                ps.find("g_cutSegs.fetch_add(", cutBranch) &&
            ps.find("sr::kernel::quit::recordDroppedProgress(dr.dev, progress);", cutBranch) != std::string::npos &&
            ps.find("sr::kernel::quit::recordDroppedProgress(dr.dev, progress);", cutBranch) <
                ps.find("g_cutSegs.fetch_add(", cutBranch) &&
            nf.find("publishSegment(dr, cursorEnd, phys, (cursorEnd - seg) >> 2, sp, S, progress);") != std::string::npos &&
            nf.find("const uint32_t progress = (gen & 3u) | cursor;") != std::string::npos &&
            nf.find("publishSegment(dr, va4, phys, n, sp, 0u, 0u);") != std::string::npos,
        "W12 rounds 2 / ringspace: the segmentless close's publish carries its fence S and its progress word (gen & 3) | "
        "cursor (the coherency block neither) and the cut branch of publishSegment records both");
}

// ── HW: the GPU hang watchdog stands down at the quit's ring cut (native/quit-ringspace) ──────────────────────────────
void hangWdTests() {
  namespace K = sr::kernel;
  // qrd1 d2 ch18 (SR_QUIT_DIAG_CUT_DELAY_MS=1000): "[GPU_HANG] ==== trigger H0 | episode key=000303DF ... completed=
  // 000303DF flat 1.0s | presents=27110 flat 1.0s" - the render thread stranded in BlockOnFence on 000303E5, the CP
  // idle and drained (parked by the quit).
  constexpr uint64_t kS = 1000000000ull;
  K::GpuHangSnap s;
  s.nowNs = 355 * kS;
  s.cpStarted = true;
  s.cpPhase = 0;
  s.cpPhaseSinceNs = s.nowNs - kS;
  s.published = true;
  s.rptr = s.wptr = 0x2000u;
  s.fenceValid = true;
  s.completed = 0x000303DFu;
  s.completedSinceNs = s.nowNs - kS;
  s.issuedValid = true;
  s.lastIssued = 0x000303F1u;
  s.fenceWaiter = true;
  s.fenceWaiterEntryNs = s.nowNs - kS - kS / 5;
  s.fenceWaiterTarget = 0x000303E5u;
  s.anyWaiter = true;
  s.anyWaiterEntryNs = s.fenceWaiterEntryNs;
  s.presents = 27110;
  s.presentsSinceNs = s.nowNs - kS;
  s.threadInGpuWait = true;
  K::GpuHangSnap all = s;  // every trigger's condition at once: H0-H3
  all.nowNs = s.nowNs + 20 * kS;
  all.cpPhase = 2;         // stuck in an IB (H3), long after the fence (H1) and the presents (H2) went flat
  all.cpPhaseSinceNs = s.nowNs;
  const uint32_t preCut = K::gpuHangEvaluate(s), preCutAll = K::gpuHangEvaluate(all);
  s.quitCut = true;
  all.quitCut = true;
  const uint32_t postCut = K::gpuHangEvaluate(s), postCutAll = K::gpuHangEvaluate(all);
  check((preCut & K::kGhH0) != 0 && preCutAll != 0 && postCut == 0u && postCutAll == 0u,
        "HW1 the watchdog stands down at the quit's ring cut: qrd1 d2 ch18's state (a BlockOnFence waiter 1.2 s on a "
        "fence the parked CP will never write, the fence and the presents flat 1.0 s) fires H0 without the cut - a hang "
        "before the request still fires - and nothing with it; every trigger's condition at once: nothing after the "
        "cut");
  // the real tick (its own watchdog state): no stand-down before the cut is not ticked here (a pre-cut tick on this
  // process's live state could report); after the cut, one stand-down line, once
  Q::resetForTest();
  const uint64_t d0 = K::gpuHangWdQuitStandDownsForTest();
  std::mutex mu;
  Q::cutRingUnder(mu);
  K::gpuHangWdTickForTest();
  K::gpuHangWdTickForTest();
  const uint64_t d1 = K::gpuHangWdQuitStandDownsForTest();
  Q::resetForTest();
  check(d1 == d0 + 1u, "HW2 the real watchdog tick after the ring cut stands down and logs its one '[QUIT] gpu-hang "
                       "watchdog stands down' line once (two ticks, one stand-down)");
  const std::string wd = readFile("kernel/gpu_hang_wd.cpp");
  const std::string tk = bodyOf(wd, "void tick(State& st) {");
  // step 4 close (lane D): the mask is no longer const since 2k(4) (SR_GPU_HANG_HOST_WORK can clear its bits)
  check(before(tk, "s.quitCut = sr::kernel::quit::g_ringCut.load(", "uint32_t mask = gpuHangEvaluate(s);") &&
            tk.find("if (!s.quitCut && s.cpPhase == kCpPhaseHostWait && s.cpPhaseSinceNs) {") != std::string::npos &&
            tk.find("if (!s.quitCut && i1 != st.i1Seen) {") != std::string::npos,
        "HW3 the tick's wiring: the snapshot's quitCut read from the ring cut before the triggers are evaluated; the "
        "host-wait report and the I1 dump gated by it too");
}

// ── B: the hot-path cost ─────────────────────────────────────────────────────────────────────────────────────────
double nsPer(LARGE_INTEGER t0, LARGE_INTEGER t1, uint64_t n) {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return double(t1.QuadPart - t0.QuadPart) * 1e9 / double(f.QuadPart) / double(n);
}

void benchTest() {
  Q::resetForTest();
  constexpr uint64_t kN = 20000000;
  static const char* const kName = "RtlEnterCriticalSection";
  volatile uint64_t sink = 0;
  LARGE_INTEGER t0, t1, t2, t3, t4, t5;
  QueryPerformanceCounter(&t0);
  for (uint64_t i = 0; i < kN; ++i) sink = sink + i;
  QueryPerformanceCounter(&t1);
  for (uint64_t i = 0; i < kN; ++i) {
    const char* prev = Q::importEnter(kName);
    sink = sink + i;
    Q::importLeave(kName, prev);
  }
  QueryPerformanceCounter(&t2);
  for (uint64_t i = 0; i < kN; ++i) {
    Q::lockAcquiring();
    Q::lockReleasing();
    sink = sink + i;
  }
  QueryPerformanceCounter(&t3);
  std::mutex mu;
  uint64_t pub = 0;
  constexpr uint64_t kM = 5000000;
  for (uint64_t i = 0; i < kM; ++i) {
    std::lock_guard<std::mutex> lk(mu);
    ++pub;
  }
  QueryPerformanceCounter(&t4);
  for (uint64_t i = 0; i < kM; ++i) Q::publishUnlessCut(mu, [&] { ++pub; });
  QueryPerformanceCounter(&t5);
  LARGE_INTEGER t6, t7;
  QueryPerformanceCounter(&t6);
  for (uint64_t i = 0; i < kN; ++i) {  // native/quit-drain: a GPU wait's mark pair (per GpuBlockOnFence call)
    const Q::GpuWaitMark m = Q::gpuWaitEnter(Q::kGpuWaitFence, 0x70700000u, uint32_t(i));
    sink = sink + i;
    Q::gpuWaitLeave(m);
  }
  QueryPerformanceCounter(&t7);
  // round 2: the spin tick, per GpuRingWaitProgress call from a fence wait's loop (the override's whole addition)
  LARGE_INTEGER t8, t9;
  const Q::GpuWaitMark sm = Q::gpuWaitEnter(Q::kGpuWaitFence, 0x70700000u, 1u);
  QueryPerformanceCounter(&t8);
  for (uint64_t i = 0; i < kN; ++i) {
    Q::gpuSpinTick(0x70700000u);
    sink = sink + i;
  }
  QueryPerformanceCounter(&t9);
  Q::gpuWaitLeave(sm);
  // native/quit-ringspace: gpuSpinSite, the override's test on EVERY GpuRingWaitProgress call (no lr filter now): in no
  // wait, and inside a ring wait with another record (a nested caller's call)
  LARGE_INTEGER t10, t11, t12;
  volatile uint32_t lrv = 0x820F0000u;
  QueryPerformanceCounter(&t10);
  for (uint64_t i = 0; i < kN; ++i) sink = sink + Q::gpuSpinSite(lrv, uint32_t(i));
  QueryPerformanceCounter(&t11);
  const Q::GpuWaitMark rm = Q::gpuWaitEnter(Q::kGpuWaitRingSpace, 0x70700000u, 0x40u, 2u, 0x7077FFC0u);
  for (uint64_t i = 0; i < kN; ++i) sink = sink + Q::gpuSpinSite(lrv, uint32_t(i));
  QueryPerformanceCounter(&t12);
  Q::gpuWaitLeave(rm);
  const double base = nsPer(t0, t1, kN), gate = nsPer(t1, t2, kN) - base, locks = nsPer(t2, t3, kN) - base,
               plain = nsPer(t3, t4, kM), cut = nsPer(t4, t5, kM) - plain, wait = nsPer(t6, t7, kN) - base,
               spin = nsPer(t8, t9, kN) - base, site = nsPer(t10, t11, kN) - base, siteRing = nsPer(t11, t12, kN) - base;
  std::printf("  B   import gate (entry + exit, no quit): %.2f ns per import | lock count pair: %.2f ns | doorbell "
              "cut test: %+.2f ns over a %.1f ns lock | GPU wait mark pair: %.2f ns | spin tick: %.2f ns | spin site "
              "test: %.2f ns (no wait) / %.2f ns (a ring wait, another record) (%llu)\n",
              gate, locks, cut, plain, wait, spin, site, siteRing,
              (unsigned long long)(sink & 1) + (unsigned long long)(pub & 0));
  check(gate < 20.0 && locks < 20.0 && cut < 20.0 && wait < 20.0 && spin < 20.0 && site < 20.0 && siteRing < 20.0,
        "B1 the hot-path cost is a few ns (measured above)");
}
}  // namespace

int runQuitTest() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash leaves the last line it reached
  std::printf("--quit-test (kernel/quit.cpp)\n");
  _putenv_s("SR_THREAD_SAMPLER", "0");  // the test registers threads with a dummy context: no sampler thread

  // ---- N: names ----
  check(Q::requestEventName("run0", 1234) == "Local\\SR_QUIT_run0_1234", "N1 request name");
  check(Q::ackEventName("run0", 1234) == "Local\\SR_QUIT_ACK_run0_1234", "N2 ack name");
  check(Q::sanitizeInstance("") == "default" && Q::sanitizeInstance(nullptr) == "default", "N3 empty -> default");
  check(Q::sanitizeInstance("a b\\c/d:e.f-g_h") == "a_b_c_d_e.f-g_h", "N4 sanitised like the harness channel");
  check(Q::sanitizeInstance(std::string(80, 'x').c_str()).size() == 64, "N5 at most 64 characters");

  boundTests();
  diffTests();
  sequenceTests();
  countersTest(false);
  countersTest(true);
  ringTest();
  strandedTest();
  ringStrandedTest();
  dropTests();
  passThroughTests();
  hangWdTests();
  nativeCutTests();
  importTest();
  eventTest();

  // ---- W: the production family printers (no CP, no guest) ----
  {
    const Q::Hooks h = Q::productionHooks();
    std::string snap, again;
    const int n = h.printFamilies(h.user, "terminal", &snap, true);
    check(n >= 9 && Q::familyCount() == n && has(snap, "[GPU_CP] terminal ") && has(snap, "[GPU_COHER] terminal ") &&
              has(snap, "[GAUGE] terminal "),
          "W1 every built-in family prints with why=terminal (gpu_cp, pm4_cov, native_flush, native_exec, "
          "native_meter, audio_native, p2_trace, ab_drain, gauges)");
    check(has(snap, "[NATIVE_FLUSH] terminal seg closed="),
          "W2 the segment transport's lines carry the terminal token (idMiss / mismatch / capDrop graded terminally)");
    h.printFamilies(h.user, "terminal", &again, true);
    char m[512];
    int fields = 0;
    const int diffs = Q::snapshotDiff(snap, again, m, sizeof(m), &fields);
    if (diffs) std::printf("      W3 differences: %s\n", m);
    check(diffs == 0 && fields > 20, "W3 a second render of the real families is field-for-field equal (no false "
                                     "verify failure from a time printed as an integer)");
    if (sr::gfx::memres::resourceMode()) {
      check(has(snap, "[MEM_RES_NATIVE] terminal=1 generation=") &&
                (!sr::gfx::memres::resourceCheckEnabled() || has(snap, "[MEM_RES_CHECK] terminal=1 frames=")),
            "W3a native resource ownership and enabled checker join the actual terminal snapshot");
      sr::gfx::memres::resourceMemory().reserveGpu();
      std::string changed;
      h.printFamilies(h.user, "terminal", &changed, true);
      check(Q::snapshotDiff(again, changed, m, sizeof(m), &fields) > 0,
            "W3b a late resource ownership generation invalidates the terminal snapshot");
    }
    check(h.cfg.drainMs == 10000 && h.cfg.settleMs == 200 && h.cfg.settleMaxMs == 3000 && h.cfg.stepMs == 50 &&
              h.cfg.cutDelayMs == 0u,
          "W4 the production bounds (SR_QUIT_* unset; the diagnostic thread-cut delay off)");
  }
  wiringTest();
  benchTest();

  std::printf("--quit-test: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
