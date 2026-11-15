# `refresh-ghidra` — Ghidra fidelity pipeline (TOOLING_PLAN.md L5)

Brings Ghidra's `default.xex` analysis in line with the recompiler: complete function coverage,
resolved xrefs, and **fully-decompiling non-leaf functions**. **Re-run after any XenonRecomp regen.**
Every step below is **verified working** on this binary.

Prereq: Ghidra MCP server started with `GHIDRA_MCP_ALLOW_SCRIPTS=1` (env var on the Ghidra
*application*; restart Ghidra after setting it). Copy these scripts to `~/ghidra_scripts` to run via
`run_ghidra_script`.

## Pipeline (in order, on program `default.xex`)

1. **`seed_functions.java`** — create/repair every function in `ppc_func_mapping.cpp` (34,743
   entries) with an explicit `[start,next)` body. **Verified: 15,988 → ~34,68x functions.** ~8 s.

2. **`recover_disasm.java`** — the seed makes function *objects* but doesn't disassemble bytes the
   original import never reached, so ~70% of seeded bodies are undefined. This disassembles every
   undefined address inside a function body. **Verified: ~5.9M addresses disassembled.** ~70 s.

3. **`run_constref.java`** — PowerPC Constant Reference Analyzer with **"Mark dual instruction
   references" ON** (off by default — the setting that turns `lis+addi` pairs into real xrefs).
   Runs only this analyzer (won't undo earlier steps). ~30 s.

4. **`define_handler_table.java`** — types the RTAM `{handler,id}` dispatch table (304 entries
   `0x820DB940–0x820DC2B8`) so each slot's pointer is a real xref. **Verified:** `get_xrefs_to
   sub_826D8A10` resolves. `scan_table_refs.java` separately **proved 0 static code accessors** to
   the table base → its reference is XEX-load-relocated, so the handler-table dispatcher is *not
   statically findable* (use the L4 runtime tracer / L9). This rigorously settles the iter-201→214
   static wall.

5. **Decompile fix for save/restore millicode** — Xenon `savegprlr/restgprlr/save·rest·fpr·vmx`
   have N entry points sharing one tail `blr`; Ghidra's one-body-per-function model can't represent
   it, so a `bl` into one is treated as non-returning and **truncates every non-leaf caller**.
   The fix is a transparent **call-fixup** (the touched regs r14-r31/f14-f31 are already
   `<unaffected>`, so the call has no caller-visible effect).
   1. Ensure the install cspec (`…/PowerPC/data/languages/ppc_64_32.cspec`) has the
      `ppc_savres_millicode` block — mirrored here in `cspec/ppc_64_32.cspec`. (Re-apply after a
      Ghidra upgrade overwrites it.)
   2. **Restart Ghidra** and reopen `default.xex` (cspec is session-cached; close/reopen of the
      *program* is NOT enough — verified).
   3. **`apply_savres_redisasm.java`** — for every millicode call-target: ensure a function, set
      call-fixup `ppc_savres_millicode`, clear `noReturn`; then **clear + re-disassemble every call
      site** so its stale `CALL_TERMINATOR` flowType recomputes to `UNCONDITIONAL_CALL` with a
      fallthrough. **The re-disassembly is the essential step** — without it the flowType stays stale
      and callers still truncate. **Verified: 46 targets, 11,107 call sites; non-leaf functions
      (`sub_826D8A10`, `sub_826AF590`, …) now decompile fully**, and `get_xrefs_to sub_826D8A10`
      gained the real code callers `sub_826D9158` / `sub_826D91F0`.

6. **`clean_overreach.java`** — step 2 (`recover_disasm`) over-reaches into tail/inline DATA padding
   and marks it "bad instruction". This clears those data over-reach units (primary opcode 0/22 =
   illegal/data) back to undefined. **Verified: cleared 52,365 cosmetic bad-instruction bookmarks;
   sample decompile unaffected (10/10 still full).** It deliberately KEEPS primary-opcode 4/5/6 units
   (~44K) — see the VMX128 note below.

7. **`seed_missing.java`** — closes the seed-coverage gap. `createFunction` needs disassembled bytes,
   so where Ghidra auto-analysis mis-detected a code region as DATA (e.g. a false "string" over a real
   function entry, as at `0x82294C20`), `seed_functions` silently skipped it. This re-reads
   `ppc_func_mapping.cpp`, and for every mapping VA with no function it clears the blocking data,
   disassembles, and creates the function. **Verified: 112 missing functions recovered, 0 failures
   (34,681 -> 34,794).** Needed so the differential CPU tester's `--emulate` oracle (L6) can reach
   every recompiled function.

### Known scope limit — VMX128 (be honest, not surprised)
~44K remaining "bad instruction" markers are **real Xenon VMX128 vector instructions** (primary
opcodes 4/5/6) that stock Ghidra's PowerPC decoder does not understand. Consequence: **scalar PPC
functions decompile fully** (control-flow / menu / kernel logic — the stuff investigations need;
verified on a 12-function spread), but **vector-heavy functions decompile partially** (a hole at each
VMX128 op). This is a stock-Ghidra-on-Xenon limitation, not a pipeline bug. To actually *decode*
VMX128 you'd integrate a VMX128 SLEIGH spec into the PPC language (separate, larger task). Until then
the **recomp source via `mcp__va-to-source` is authoritative for vector-heavy functions** (and for
everything else — it's the actual executing C++; Ghidra is the second opinion + xrefs/disasm/call-graph).

## Files
`seed_functions.java` (1) · `recover_disasm.java` (2) · `run_constref.java` (3) ·
`define_handler_table.java` + `scan_table_refs.java` (4) · `apply_savres_redisasm.java` (5) ·
`clean_overreach.java` (6) · `cspec/ppc_64_32.cspec` (the customized PPC cspec with the
`ppc_savres_millicode` fixup).
