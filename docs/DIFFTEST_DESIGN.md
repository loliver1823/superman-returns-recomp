# L6 — Differential CPU-Test Design

> Status: **scaffold + design** (iter-215). Concrete first half (golden capture) is built and
> scriptable today; the recomp-side execution half needs a small new test entrypoint in the
> harness (sketched in §6). This doc is the contract for both halves.
>
> See `docs/TOOLING_PLAN.md` §L6 (per-function form) and §L9-C (whole-execution form). L6 is the
> *unit-test* tier of the differential oracle: one guest function, controlled inputs, diff the
> deltas. L9-C is the *integration* tier: a whole boot, bisected against a golden Xenia trace.
> They share the same divergence vocabulary (register/memory delta + "first differing field").

---

## 1. Goal & why it pays off

The port's six recurring bug classes (endianness, pointer math, vtable dispatch, struct alignment,
missing null-check, sign/shift — see `CLAUDE.md`) currently surface **only when they crash**. A
divergence in a pure arithmetic helper that merely returns a wrong-by-one size never crashes; it
silently corrupts an allocation 200 ms later and we chase the *symptom*. Differential CPU-testing
catches the bug **at the instruction that produced it**:

> Run guest function `sub_XXXX` on identical inputs in (a) our recomp and (b) a trusted reference,
> then diff the resulting register + memory deltas. Any field that differs is a recompiler bug,
> localized to one function.

This is the cheapest possible correctness signal we have, because **we own the recompiler and the
`PPCContext`** — the function is already a callable C++ symbol; we just need to call it in isolation.

---

## 2. Test-candidate selection — pure leaf functions first

Not every function is testable standalone. We stage by how much of the world the function touches.

### Tier 0 — pure leaf functions (start here)

A function qualifies as a **pure leaf** when its recomp body contains **none** of:

| Marker in recomp body | Why it disqualifies (for Tier 0) |
|---|---|
| `PPC_CALL_INDIRECT_FUNC`, `PPC_CALL_FUNC` | calls another function → needs the callee mapped + the dispatch table |
| `sub_XXXX(ctx, base)` (direct call) | same — non-leaf |
| `__savegprlr` / `__restgprlr` / `save/restfpr` / `save/restvmx` | spills registers to the stack → touches memory + the spill thunks |
| `PPC_STORE_U*` / `PPC_LOAD_U*` / `PPC_MM_*` | reads/writes guest memory → Tier 1, needs a seeded memory image |
| `ppc_trap` | trap path |
| `.long 0x…` (raw data) | not a real function — a jump-table/data island the disassembler mislabeled |

Detected mechanically by `scripts/difftest/scan_candidates.py` (the scanner used to build this
doc; see §5). Of 34.5k functions, **~300** are pure-arithmetic leaves with ≥5 real ops that read an
argument register and write `r3`. These are the ideal first targets: the Ghidra PowerPC emulator is
a *trustworthy* oracle for them (no memory, no syscalls, no host state), and a divergence can only
mean a codegen bug in integer/bitfield lowering — exactly the sign/shift/rotate class.

**Three worked examples** (all confirmed in `ppc_func_mapping.cpp`, all pure leaves):

| Guest VA | What it computes (from Ghidra) | Bug class it exercises |
|---|---|---|
| `0x822949D8` | `r3 = (uint32_t)(r5 - r4) >> 31` — i.e. `1` if `r5 < r4` unsigned-wrap sign bit, via `cntlzw`+`rlwinm` | shift/rotate lowering, `cntlzw` edge (input 0 → 32), masked rotate |
| `0x826690D0` | `r3 = align_up(r3*8, 32) + r3*8` — size rounding (`rlwinm` masks + `addi 31`) | `rlwinm` mask boundaries, alignment math |
| `0x82670774` | `r3 = r3*r4*3` — index scale (`mullw` + `rlwinm` ×2 + `add`); Ghidra: `p1*p2 + (p1*p2 & 0x7fffffff)*2` | `mullw` 32→64 sign-extend, `rlwinm` mask, overflow wrap |

Each is 4–5 instructions, reads `r3`/`r4`/`r5`, returns in `r3`. `sub_822949D8` in particular is a
great regression canary: it leans entirely on `cntlzw` (count-leading-zeros, undefined-for-0 handled
as 32 in the recomp) and a `rlwinm 27,31,31` rotate-mask — a classic spot to get the shift amount or
mask off by one.

### Tier 1 — memory-touching leaf functions (next)

Functions that `PPC_LOAD`/`PPC_STORE` but still make no calls. Testable once we can **seed and diff
guest memory** (the entrypoint in §6 already provides this). The reference must read/write the same
big-endian image. Candidate selection: same scanner, but allow `PPC_LOAD_U*`/`PPC_STORE_U*` and
disallow only calls/spills. This tier is where **endianness** and **struct-alignment** bugs live —
the highest-value class — so it is the real prize; Tier 0 exists to validate the harness end-to-end
first on functions where the oracle is unimpeachable.

### Tier 2 — call-graph leaves (later, optional)

Functions whose only calls are to *other already-validated* pure functions. Requires linking a
minimal dispatch table of validated callees. Deferred until Tier 0+1 are green.

---

## 3. Reference (golden) oracle

### Primary: Ghidra PowerPC emulator (`mcp__ghidra__emulate_function`)

The same `default.xex` is loaded in the Ghidra MCP server as a true `PowerPC:BE:64:A2ALT-32addr`
program. `emulate_function` takes `{address, registers, memory, return_registers}` and runs the
function's real PowerPC bytes on Ghidra's p-code emulator — an **independent implementation** of the
ISA from our XenonRecomp lowering, which is exactly what makes it a valid differential oracle.

**Invocation (documented payload):**

```jsonc
// MCP tool: mcp__ghidra__emulate_function
{
  "address": "0x82670774",
  "registers": { "r3": "0x7", "r4": "0x5" },   // hex strings; only set what the fn reads
  "memory":    {},                              // Tier 0: empty. Tier 1: { "<VA hex>": "<hex bytes, BE>" }
  "return_registers": "r3"                      // which regs to read back (comma-sep)
}
```

### ⚠️ Known blocker (iter-215, must fix before live use)

As of this writing the MCP `emulate_function` endpoint **reproducibly errors with
`"Emulation failed: Undefined register: ESP"`** for *every* input on this PowerPC program — it
appears to initialize a host-x86 stack-pointer register that does not exist in the PowerPC language
model. This is a server-side bug in the emulate wrapper, **not** in our inputs (verified: dry-run,
explicit `r1`, explicit `program` arg all hit the same error).

**Consequence for the design:** the golden-capture script must **not** assume a live MCP call
succeeds. It is built two-way (see §5):
1. **Emit mode** — print the exact tool-call payloads for a human/agent to run via the MCP tool, and
2. **Ingest mode** — parse a saved JSON response back into the canonical golden format.

This keeps the pipeline useful *today* and makes it a one-line switch to "live" once the emulate
endpoint is fixed (or pointed at an alternate oracle).

### Fallback oracles (oracle-agnostic golden format)

Because the golden format (§4) is just *inputs → register/memory deltas*, **any** oracle can fill
it. In priority order:
1. **Fixed Ghidra emulator** (once the ESP bug is resolved) — fully automatable.
2. **Xenia interpreter** (the L9-A trace-instrumented fork, when it lands) — runs the identical
   guest bytes; for a single function, drive it with a tiny stub that sets registers and single-steps.
3. **Hand-derived golden** — for the 4–5-instruction Tier-0 functions the semantics are simple
   enough to compute by hand from the Ghidra decompile and pin as a literal expected delta (this is
   how the three §2 examples are sanity-checked: `7*5*3 = 105 = 0x69`). Lowest tech, zero
   infrastructure, and a perfectly valid oracle for the canary set.

---

## 4. Golden / result format (the divergence vocabulary)

One JSON object per `(function, input-case)`. Shared by both sides and by L9-C.

```jsonc
{
  "guest_va": "0x82670774",
  "fn": "sub_82670774",
  "case_id": "mul3_7x5",
  "inputs": {
    "regs": { "r3": "0x00000007", "r4": "0x00000005" },
    "mem":  {}                       // Tier 1: { "0x82AE0000": "deadbeef..." } big-endian bytes
  },
  "oracle": "ghidra-emulate",        // or "xenia", "hand", "recomp"
  "deltas": {
    "regs":  { "r3": "0x00000069" }, // only registers that CHANGED vs input (or are flagged outputs)
    "mem":   {}                      // Tier 1: { "<VA>": "<new BE bytes>" } for written ranges
  }
}
```

**What counts as a divergence** (recomp result vs golden), in order of severity:

1. **Output-register mismatch** — any reg in the function's output set (minimally `r3`; for
   multi-value returns also `r4`, and `f1` for FP) differs. *Definite bug.*
2. **Memory-delta mismatch** (Tier 1+) — a written VA differs in value, **or** in which bytes were
   written (catches endianness: right value, wrong byte order ⇒ the bytes are a swap of each other).
   *Definite bug — usually a missing/extra `bswap`.*
3. **`cr`/`xer` mismatch** *(advisory)* — condition/carry flags differ. The recomp only needs flags
   the *caller* consumes, and PowerPC leaves many flag bits architecturally don't-care, so this is a
   **warning**, not a failure, unless the function's contract is "return a flag." Compared but not
   gating by default.
4. **Volatile-scratch divergence** — `r9`–`r12`, etc., differing is **expected and ignored** (the
   two implementations use scratch registers differently). Only the ABI-defined output set is gated.

The comparison is therefore **masked**: define, per function, an *output mask* (default `{r3}`,
overridable per case) and diff only those plus all memory writes. Everything else is informational.

---

## 5. Pipeline & files

```
scripts/difftest/
  scan_candidates.py   # (design helper) lists pure-leaf candidates from the recomp source
  golden_capture.py    # THIS DELIVERABLE — build the golden set (emit payloads / ingest responses)
  cases/               # input case files: <fn>.cases.json  (list of input dicts)
  golden/              # captured goldens: <fn>.golden.json
  README.md
```

Flow:

1. `scan_candidates.py` → pick functions, hand-write or generate `cases/<fn>.cases.json`.
2. `golden_capture.py --emit` → prints the exact `emulate_function` payloads for each case.
   Run them via the MCP tool (or the fixed emulator), save each response.
3. `golden_capture.py --ingest <responses.json>` → writes canonical `golden/<fn>.golden.json`.
   (Or `--hand` to pin a literal expected delta for the canary set with no oracle at all.)
4. **[pending recomp side]** `difftest_main` (§6) runs the same cases through `sub_XXXX` and emits
   results in the §4 format; a `compare.py` diffs `golden/` vs the recomp results with the §4 mask.

---

## 6. The recomp-side execution half (PENDING — needs a new entrypoint)

This is **the honest hard part**. Our recomp functions are *not* standalone — `sub_XXXX(ctx, base)`
assumes the harness already (a) allocated the 4 GB flat guest memory, (b) called
`sr::kernel::setGuestMemory(base, size)` so `PPC_GUEST_BASE()` and the alias table resolve, and
(c) populated the dispatch table (only needed if the function calls out — Tier 0 doesn't).

For **Tier 0 pure leaves none of the world is needed except a valid `base`** — there are no loads,
stores, or calls. So the entrypoint is small. The plan: add a separate executable target
`difftest_main` (not wired into the game) that links the recomp objects + `kernel/guest_context`
and exposes a tiny driver.

### Sketch — `guest/difftest_main.cpp` (new file, not yet created)

```cpp
// difftest_main: run one recompiled guest function in isolation, dump register/memory deltas.
// Build as a standalone target that links the ppc_recomp.*.o + kernel/guest_context.o.
// NOT part of the game harness. Tier 0 (pure leaves) needs only valid guest memory + setGuestMemory.
#include "ppc/ppc_recomp_shared.h"
#include "kernel/guest_context.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

extern PPCFuncMapping PPCFuncMappings[];           // {guestVA, fn}
extern uint8_t* allocGuestMemory(size_t);          // from guest_harness (or a local copy)

static PPCFunc* lookup(uint32_t va) {
  for (PPCFuncMapping* m = PPCFuncMappings; m->host; ++m)
    if ((uint32_t)m->guest == va) return m->host;
  return nullptr;
}

// Apply a {regName -> u32} map onto ctx. Extend for f*/v* as Tiers grow.
static void setReg(PPCContext& ctx, const std::string& name, uint64_t v) {
  if      (name == "r3") ctx.r3.u64 = v;  else if (name == "r4") ctx.r4.u64 = v;
  else if (name == "r5") ctx.r5.u64 = v;  else if (name == "r6") ctx.r6.u64 = v;
  // ... r0..r31, lr, ctr as needed
}

int main(int argc, char** argv) {
  // 1) one-time world: 4 GB flat guest memory + register base so PPC_GUEST_BASE() works.
  const size_t kSize = 0x100000000ull;         // matches PPC_MEMORY_SIZE
  uint8_t* base = allocGuestMemory(kSize);      // VirtualAlloc/mmap, 4GB-aligned (low32==0)
  sr::kernel::setGuestMemory(base, kSize);

  // 2) parse one case from argv/stdin: guest_va, input regs (Tier 1: also seed mem bytes BE).
  uint32_t va = /* parse */ 0x82670774;
  PPCContext ctx{}; std::memset(&ctx, 0, sizeof(ctx));
  ctx.r1.u32 = 0x70000000;                      // a sane stack slot (unused by pure leaves)
  // for each input reg: setReg(ctx, name, value);
  // Tier 1: for each input mem {VA,bytes}: write BE bytes at base + aliasGuestVA(VA);

  // 3) snapshot inputs, call, snapshot outputs.
  PPCContext before = ctx;
  PPCFunc* fn = lookup(va);
  if (!fn) { fprintf(stderr, "no mapping for %08X\n", va); return 2; }
  fn(ctx, base);

  // 4) dump deltas in the §4 format: every output reg in the mask (default r3) + Tier-1 mem writes.
  //    (For mem, diff a recorded write-set or rescan seeded ranges.)
  printf("{\"guest_va\":\"0x%08X\",\"deltas\":{\"regs\":{\"r3\":\"0x%08X\"},\"mem\":{}}}\n",
         va, ctx.r3.u32);
  (void)before;
  return 0;
}
```

Build wiring (CMake): a new target that compiles `difftest_main.cpp` + reuses the already-built
recomp/kernel objects, with the *same* `PPC_CONFIG_*` defines as the game so `PPCContext`'s layout
matches. **Critical constraint:** the recomp `.cpp` files `#include "ppc_recomp_shared.h"` and pull
in the full kernel — so the cleanest path is to link the *existing* recomp static library and add
only `difftest_main.o`, rather than recompiling the guest. The watchpoint macros in `ppc_context.h`
reference `sr::kernel::moduleGlobalWatchpoint` etc.; those symbols come from the kernel objects, so
linking the kernel lib satisfies them (they simply won't fire for Tier-0 inputs outside the watched
VAs).

### Why Tier 0 is genuinely tractable now and Tier 1 needs care

- **Tier 0**: no memory, no calls. `setGuestMemory` + a zeroed `PPCContext` is the entire world.
  The only risk is `PPCContext` layout drift between the difftest target and the game — solved by
  sharing the exact compile defines.
- **Tier 1**: must (a) seed guest memory big-endian at the right VAs, (b) capture *which* bytes the
  function wrote (record a write-set, or re-hash seeded ranges before/after), and (c) make the
  reference oracle read/write the identical image. The §6 entrypoint already has the hooks; the
  work is the memory diff plumbing + ensuring the oracle's `memory` input mirrors the seed.

---

## 7. Staged plan (locked order)

1. **Stage A — canary (no infrastructure):** the three §2 functions, `--hand` goldens (semantics
   computed from the Ghidra decompile). Build `difftest_main`, run them, diff. Proves the recomp
   side end-to-end with an unimpeachable oracle. *Smallest possible vertical slice.*
2. **Stage B — automate the oracle:** fix/replace the `emulate_function` ESP bug (or point at the
   Xenia single-step path); regenerate the same three goldens from the live emulator; confirm they
   match the hand goldens. Now goldens are machine-produced.
3. **Stage C — scale Tier 0:** sweep all ~300 pure-arith leaves, randomized + edge-case inputs
   (0, −1, INT_MIN, INT_MAX, powers of two — the shift/rotate/`cntlzw` edges). Any divergence is a
   real codegen bug; file it. This is a one-time bulk correctness audit of integer lowering.
4. **Stage D — Tier 1 (memory):** add the memory seed/diff plumbing (§6) and the matching oracle
   memory input. This is where endianness/alignment bugs get caught systematically — the real payoff.
5. **Stage E — wire into CI / L8:** a `difftest` smoke set runs on every recomp regen; a divergence
   blocks the change. Becomes the per-function complement to L2's whole-run smoke gate and feeds
   L8's reward signal (it can now attack *non-crashing* divergence, per `TOOLING_PLAN.md` §L8).

---

## 8. What's done vs pending (be honest)

| Piece | Status |
|---|---|
| Candidate selection criteria + scanner | ✅ done (`scan_candidates.py`; ~300 Tier-0 leaves found) |
| Worked-example functions | ✅ **9 pure leaves** validated (`0x822949D8`, `0x826690D0`, `0x82670774`, `0x82294C20`, `0x82294C34`, `0x822BCC78`, `0x82644698`, `0x82686E50`, `0x82686E68`) |
| Golden format / divergence vocabulary | ✅ specified (§4); +`status`/`purity` fields (§9) |
| `golden_capture.py` (emit payloads + ingest response + hand-pin) | ✅ done; `--hand` also pins `expect_status:"faulted"` negative goldens |
| Ghidra emulate as live oracle | ⚠️ blocked — server-side `ESP` bug; emit/ingest design works around it |
| Recomp-side `difftest_main` entrypoint | ✅ **done** — `tools/difftest/difftest_main.cpp` + `difftest` CMake target (links the existing recomp/kernel objects; same `PPC_CONFIG_*`) |
| **SEH safety net** (run ANY function without an uncontrolled crash) | ✅ **done** (§9) — `__try/__except` in an `extern "C"` POD helper; a non-pure fn reports `status:"faulted"` instead of killing the process |
| **Purity pre-check** (`leaf`/`stores`/`calls` advisory) | ✅ **done** (§9) — source scan of the target `sub_XXXX`, emitted before the delta is trusted |
| `compare.py` (masked diff) | ✅ done — gates r3/r4 **and `deltas.mem` (byte-for-byte)**, gates `faulted` for negative cases, advisory cr/xer/scratch/purity |
| Validation | ✅ green — **11 functions, 50 positive cases (incl. 5 Tier-1 store cases) + 1 negative (faulted) case = 51/51 PASS** |
| **Tier 1 memory seed/diff** | ✅ **done (Tier-1 hand-oracle)** — `difftest_main` seeds input `mem` and captures changed bytes from `watch` regions into `deltas.mem`; `--hand` pins `expect_mem`; `compare.py` gates it. Validated on `sub_8213CA30` (`*(u32)(r3)+=1`) and `sub_82165F80` (`*(u32)(r3+1480)-=r4`). **Machine memory oracle still pending** — `--emulate`/`--ingest` capture registers only (no `emulate_function` memory-readback); Tier-1 goldens must use `--hand` until that lands (TODO in `golden_capture.py:emulate`). |

The **`difftest_main` entrypoint** is built and the cross-validator runs end-to-end:
`cmake --build build --target difftest && python scripts/difftest/compare.py --all` → **46/46 PASS**.

### Build/link notes (how the standalone target actually resolves)

The recompiled functions can't be cherry-picked at object-file granularity: pulling
`ppc_recomp.82.cpp.obj` (which contains `sub_82670774`) drags in every other function in that TU,
and those reference the kernel import-glue. So the `difftest` target links the **whole**
`SupermanReturnsGuest` static lib plus the **same** harness sources the game uses
(`HARNESS_SOURCES_NO_MAIN` = `APP_SOURCES` minus `app/main.cpp`), which supplies the real
`setGuestMemory`/`getGuestBase`, the store-macro watchpoint symbols, the import glue, and the
gfx/SDL stubs the recomp objects reference. Tier-0 leaves call **none** of that machinery at
runtime — it only needs to be *linked* so the symbols resolve. The leaf functions themselves are
invoked by symbol directly (no `PPCFuncMappings`/dispatch table), and `difftest_main.cpp` only
drives `setGuestMemory` + a zeroed `PPCContext`, exactly as §6 specifies.

---

## 9. Safety: running ANY function without an uncontrolled crash

The recomp side originally trusted that every target was a pure leaf. Point it at a
non-pure function and it would dereference unmapped guest memory or the (null in this
world) dispatch table and **crash `difftest.exe` with no output**. Two layers now make
that impossible — a hard safety net plus a soft heads-up.

### 9.1 SEH safety net (the guarantee)

Every guest call goes through an `extern "C"` helper, `callGuarded(fn, ctx, base, &fi)`,
whose body is a Windows `__try { fn(...); } __except(...) { ... }`. clang on
`x86_64-pc-windows-msvc` supports SEH there as long as the frame holds **no C++ object
with a destructor** (the one construct it rejects in a `__try` frame), so the helper is
deliberately POD-only. The `__except` filter records the exception code and the faulting
instruction address (`GetExceptionInformation()`), and the handler returns cleanly.

Result: a non-pure function never kills the process. It produces a clean JSON verdict

```json
{"guest_va":"0x8279E494","fn":"sub_8279E494","oracle":"recomp",
 "purity":"calls","status":"faulted","exception":"0xC0000005","at":"0x..."}
```

`status` is `"ok"` for a normal run (with `deltas`) or `"faulted"` for a caught
exception. `compare.py` gates this: a positive case that faults is a **FAIL**; a
negative case (golden `"status":"faulted"`) that *doesn't* fault is a **FAIL**.

**Proof it fires:** `sub_8279E494` (`mtctr r4; bctr`) is a non-leaf tail-call
trampoline. Invoked with an in-range code VA in `r4`, it dispatches through
`sr::kernel::g_dispatchTable`, which is `nullptr` in the difftest world (only the game's
boot populates it) → access violation `0xC0000005`. `difftest --self-test` and
`compare.py --all` both show it caught cleanly with exit 0 / PASS. It is checked in as
`cases/sub_8279E494.cases.json` + `golden/sub_8279E494.golden.json`.

### 9.2 Purity pre-check (the heads-up)

`classifyPurity()` scans the recomp source (`guest/ppc/ppc_recomp.*.cpp`, located via
the exe's working dir or `SR_DIFFTEST_RECOMP_DIR`) for the target `sub_XXXX` and reports
an advisory `"purity"` field — same criteria as `scan_candidates.py`:

- `"leaf"`  — no calls, no stores, no spills → the delta is trustworthy.
- `"stores"`— writes guest memory (Tier-1; the delta may be incomplete until §6 memory diffing lands).
- `"calls"` — calls/spills/indirect-dispatch → **not** a pure leaf; distrust the delta, the SEH net is all that protects you.
- `"unknown"`— source not found (degrades gracefully; the case still runs).

This is advisory only — it never blocks a run; it tells you *before* you read the number
whether to trust it. The SEH net (9.1) is the actual guarantee.
