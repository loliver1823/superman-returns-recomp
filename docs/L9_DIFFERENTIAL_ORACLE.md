# L9 — Differential Oracle Stack (engineering design)

> Status: **DESIGN + first step.** This is the multi-week, menu-aimed proposal from
> `TOOLING_PLAN.md` §L9. A and B are each a real project (days–weeks); this doc makes them
> concrete and sequenced so they can be picked up deliberately. It is **not** built yet.

## Premise

The menu blocker (iter-199→214) is an *absence/race* bug — L8 (crash-grind) structurally
can't touch it. But the gap is **not** an information limit: Xenia runs the **same guest
binary** to the menu every roll, so "what should happen" exists; it's merely
**un-instrumented**. That reframes the whole 214-iteration wall as an *observability +
determinism* problem — i.e. tool-shaped. L9 builds the two missing instruments and the glue.

---

## A — instrumented-Xenia golden oracle  *(days–2 wk, low risk; BUILD FIRST)*

**Goal:** a ground-truth, per-guest-function (and per-guest-memory-write) trace of a *working*
boot to the menu. Against it, "command `0x40008203` is never dispatched from the handler table
at `0x820DC0C8`" (iter-214) becomes "*here* is the function that dispatches it, in *this* state,
after *this* predecessor."

**Why this overturns "Xenia un-traceable" (App. B / iter-213O):** that finding was
"nobody built a debug Xenia with a trace hook + the canary release emits nothing," **not** a
law. Xenia is OSS (xenia-canary). We compile our own hook in.

**Where the hook goes (Xenia architecture):**
- Xenia has two PPC backends: an **interpreter** (`src/xenia/cpu/ppc/ppc_frontend` +
  interpreter) and an **x64 JIT** (the default, `backend/x64`). The interpreter is the easy,
  low-risk hook target — slower but we only need one boot trace.
- Hook point: the interpreter's instruction-dispatch loop and/or `Processor::Execute` /
  function-entry (`xe::cpu::Function::Call`). Emit `(guest_pc, optional: r3/r1/lr)` at each
  guest function entry, gated by an env var, to a binary trace file. For memory writes, the
  interpreter's store path (`StoreGuest*`/the MMIO+memory write helpers) is the choke point.
- Run Xenia **forcing the interpreter backend** (`--cpu=interpreter` style flag) with the
  sound-stripped ISO that reaches the menu; capture until `ScrPressStart`.

**Output:** `xenia_boot.trace` — an ordered list of guest function entries (and gated memory
writes) of a *working* boot. The Rosetta stone the project has lacked for 214 iterations.

**First concrete step (tractable now):** clone xenia-canary, locate the interpreter
function-call site + the guest-store helper, and add the gated emit. (≈ the bulk of A is here;
the rest is run + capture + a small reader.)

---

## B — record-replay deterministic scheduler  *(2–4 wk, medium risk)*

**Goal:** the *tractable* reframing of deferred L1. You do **not** have to *solve* the DOGMA
race to *neutralize* it. We own the recompiler and the `PPCContext` through every function, so
build an **rr-style cooperative scheduler**: guest threads yield only at instrumented points;
one seeded scheduler picks who runs next. **Same seed → byte-identical boot.** The race becomes
a **knob**, not a bug.

**Where the yield points go:** the existing chokepoints already in our recomp —
`PPC_CALL_INDIRECT_FUNC` (done: L4 hook lives here), the kernel import boundary (the harness
already snapshots context here), and lock acquire/release in `kernel/cs_*`/`dogma_*`. A
cooperative scheduler replaces real thread preemption with: at each yield point, call
`sched_yield(threadId)` → a single seeded picker (PRNG seeded from an env var) chooses the next
runnable thread. Worker threads (`ExCreateThread`, the DOGMA pool worker `0x827A7420`) register
with the scheduler instead of running freely. The existing `beginWalkerPhase`/`endWalkerPhase`
deferral is a crude prototype of exactly this.

**Payoff for the WALK_END ~1/3 stall (measured by L2):** **schedule-fuzz** the interleavings
(iterate seeds via `smoke_test`) until one *completes* WALK_END; capture that seed; then diff a
*completing* schedule against a *hanging* one — **the differing scheduling decision at the
divergence point IS the race, pinpointed.** That's the iter-25 bug localized, not by static
RE, but by controlled replay.

**The real risk (honest):** quiesce fights the same DOGMA threads — but *controlling* the race
(cooperative yield) is far less invasive than *solving* it (which is why L1 was deferred and B
is not). Start single-threaded-deterministic and add worker threads one at a time, gated.

---

## C — divergence bisector  *(glue; small once A+B exist)*

Binary-search the **first** guest-execution divergence between the Xenia golden trace (A) and a
deterministic replay of our build (B). Converts "menu is blank" (unanswerable) into "function
X, state Y, branch A in Xenia / branch B in us" — a crash-class signal **with an address**, on
**non-crashing** divergence. This is also the differential **oracle** L6/L8 want: once C exists,
L8's grind loop can attack divergence, not just crashes.

**Mechanics:** both traces are ordered `(guest_pc, key regs)` streams. The two oracles record at
**different granularities** — A (Xenia) logs *all* guest function entries, B (ours) logs only
indirect/computed call targets, a SUBSET — so the bisector aligns by **subsequence**, not strict
index-lockstep: it advances golden looking for each ours pc within a bounded look-ahead `--window`;
the first ours pc golden never produces next is the divergence frontier ("ours entered X but golden
never enters X from here"). Feed that VA to `va_to_source` + Ghidra → the exact function + branch.
Re-run with the L4 call tracer / a watchpoint at that VA for the surrounding state. `--strict` keeps
the old index-lockstep for known-matched granularities. The subsequence match is a greedy best-effort
*localizer* (loop-heavy traces can mis-sync; `--window` bounds it), not a proof.

---

## Sequence & dependencies

```
A (Xenia golden trace) ──┐
                         ├──► C (bisector) ──► menu divergence, with an address
B (replay scheduler) ────┘                     └─► also upgrades L8 to attack non-crashing bugs
```

Build **A first** (independently useful the day it works — a working-boot trace stands alone),
then **B** (independently useful: deterministic boot kills the reroll tax + enables schedule-fuzz
on the WALK_END stall), then **C** (cheap glue). Each delivers value before the next exists.

## Status / landed increments

- **C — divergence bisector: DONE, granularity-robust** (`scripts/l9/divergence_bisect.py` +
  `scripts/l9/trace_format.md`). Diffs a golden vs ours trace, reports the first guest-execution
  divergence with the address + shared spine + both continuations, `--resolve` via the va_to_source
  index. **Now aligns by SUBSEQUENCE by default** (was strict index-lockstep): because ours (B) records
  only a subset of the entries golden (A) records, strict lockstep false-diverged at the first
  granularity gap — that "may be a granularity artifact" caveat is **handled**, not just warned about.
  Two-pointer subsequence walk with a bounded `--window` look-ahead finds the first ours pc that golden
  never produces next ("ours entered X, golden never does"). `--strict` keeps old lockstep for
  known-matched granularities. Honest limit: the greedy match is a best-effort *localizer* on loop-heavy
  traces (can mis-sync; `--window` bounds it), not a minimal-edit-distance proof. Validated on synthetic
  binary+text fixtures modeling the real A/B mismatch: (1) faithful subset -> PASS, (2) ours-only target
  absent from golden -> divergence at that VA, (3) golden internal divergence -> divergence where ours
  expects the missing entry, (4) `--strict` matched-granularity regression -> PASS. Ready the moment A
  and B produce traces.
- **B — trace emitter increment: DONE.** Our recomp now emits the canonical "ours" trace under
  `SR_GUEST_TRACE=1` (`kernel/guest_context.cpp::emitGuestTrace`, hung off the existing L4 indirect-call
  hook → `sr_boot.trace`). Granularity = indirect/vtable/computed calls (the control-flow *decisions*;
  full per-function-entry needs a XenonRecomp codegen change). The **record-replay scheduler** itself
  (the determinism core) remains the multi-week build.
- **A — Xenia trace fork:** in progress (clone + locate hook + write the trace-emit patch); a full
  Xenia build is a separate heavy step.

The two remaining heavy builds (A's Xenia fork to capture a golden boot, B's cooperative scheduler for
byte-identical replay) are genuine sub-projects; the bisector (C) and emitter (B-increment) make the
pipeline *immediately* exercisable as soon as a golden trace exists. They are the deliberate,
high-investment path to the one bug class (menu/absence/race) every other tool here hands off.
