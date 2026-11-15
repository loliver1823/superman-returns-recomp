# L9 guest-execution trace format (canonical)

Both oracles emit this so the L9-C divergence bisector (`divergence_bisect.py`) can diff them:
- **A** — instrumented Xenia (golden, working boot), env `XENIA_GUEST_TRACE=1` → `xenia_boot.trace`.
  Writer: `tools/xenia-trace/xenia_trace.h` (applied to xenia-canary via `xenia_trace.patch`).
- **B** — our recomp, env `SR_GUEST_TRACE=1` → `sr_boot.trace`.
  Writer: `kernel/guest_context.cpp::emitGuestTrace` (hung off the L4 indirect-call hook).

## Canonical: fixed-width BINARY (little-endian)

A 32-byte header then N fixed 32-byte records (so the bisector indexes by record number, no parsing):

```
Header (32 B):  magic[8]="XENTRC01"  record_size u32 =32  endian_tag u32 =0x04030201  reserved u64 ×2
Record (32 B):  guest_pc u32 | lr u32 | r3 u64 | r1 u64 | thread_id u32 | tag u32
```

- `guest_pc` — guest VA of the function entered (the bisector's primary key, compared in order).
- `lr`/`r3`/`r1` — advisory context shown at the divergence frontier (not gated on; arg values can differ benignly). The recomp emitter (B) sets `r3=r1=0` (not available at its indirect-call hook).
- `thread_id` — guest thread id, for v2 interleaving reconciliation.
- `tag` — `0` = function-entry; `1` reserved for future guest memory-write records.

Struct (Python): `struct.Struct("<IIQQII")`. Header magic `b"XENTRC01"`.

## Granularity (important)

- **A (Xenia):** entry points / host-kernel call-ins via `GuestFunction::Call` (full coverage needs the
  JIT-prolog hook — see `tools/xenia-trace/README.md`). xenia-canary is **JIT-only — no interpreter
  backend exists** (corrects the iter-213O "force interpreter" plan).
- **B (recomp):** indirect/vtable/computed call targets (the control-flow *decisions* where the menu
  diverges); full per-function-entry needs a XenonRecomp codegen change.
  The two granularities are **not** identical record-for-record: B is a SUBSET of A. The bisector
  handles this directly via subsequence alignment (below) — it does **not** require matched granularity.

## Diffing semantics (L9-C)

The bisector keys on `guest_pc` order. Because B (ours) records only a SUBSET of the entries A (golden)
records, naive index-lockstep would false-diverge at the first granularity gap. So the **default** is
**granularity-robust subsequence alignment**:

- If B is a faithful subset of A's control flow, then B's `guest_pc` sequence must appear as a
  **subsequence** of A's `guest_pc` sequence up to the real divergence.
- Two pointers `i` over golden (A) and `j` over ours (B). For each `ours[j]`, scan forward in golden
  from `i` (bounded by a configurable look-ahead `--window`, default 2000) for a matching pc. Found ->
  sync (advance both past it). **Not** found within the window -> that is the **divergence frontier**:
  ours entered a pc golden never enters next from here. Reported with the frontier VA, the last K synced
  frames as shared context, and both continuations.
- **Honest limit:** this greedy subsequence match is a best-effort *localizer*, not a proof. On
  loop-heavy traces (a hot dispatcher revisited thousands of times) a greedy first-match can sync to the
  wrong occurrence and drift. `--window` bounds both the cost and the over-advance; there is no
  backtracking / minimal-edit-distance guarantee. Cross-check the frontier VA.
- **`--strict`** restores the old index-lockstep (first index where `golden[i].pc != ours[i].pc`); use
  it only when granularities are **known-matched** (both full per-function-entry, or matched fixtures).
  Length mismatch under `--strict` is also a divergence.

v1 assumes single serialized streams; multi-thread reconciliation via `thread_id` is a v2 concern.

Exit codes: `0` = ours is a faithful subsequence of golden (no divergence) / `--strict` traces agree;
`2` = divergence found; `1` = error (e.g. missing file).

## Legacy TEXT format (still accepted by the bisector)

`<seq> <pc> <r3> <r1> <lr>` per line (regs `-` if absent); `#` comments ignored. Convenient for
hand-authored fixtures; the bisector auto-detects it when the file does not start with `XENTRC01`.
