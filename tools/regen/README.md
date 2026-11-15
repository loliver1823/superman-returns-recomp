# Guest code: regenerated at build time (release lane R1)

`guest/ppc` is generated, gitignored, and never hand-edited. The repository carries **no
recompiled game code**: the build recompiles the player's own `default.xex` at configure time.
Every behaviour the port adds lives in host code. docs/SPLICE_CONVERSION.md records how the
310 hand-edited bodies of the old pipeline became host hooks.

## The pipeline (tools/regen/regen.py; CMake runs it at configure time)

1. **Inputs.**
   - `default.xex`: CMake's `SR_XEX_PATH`, default `Superman Returns (USA)/default.xex` (the
     extracted disc that docs/BUILDING.md describes), else `private/default.xex` (also regen.py's
     own default when run by hand). Both are gitignored (`private/` is an allowlist .gitignore).
   - `superman_regen.toml` + `superman_switch_tables.toml`.
   - `patches/*.patch`.
   - `hand/` (ppc_config.h, ppc_context.h, the empty ppc_missing_recomp_stubs.cpp).
   - `abi_bridge.json`.
2. **XenonRecomp bootstrap.**
   - Upstream hedge-dev/XenonRecomp at the commit the `tools/XenonRecomp` submodule pins
     (ddd128b) is exported from that checkout with `git archive` into
     `build/regen/xenonrecomp/src`; the thirdparty submodule contents are copied in.
   - `patches/0001..0007` are applied there, and XenonRecomp is built with Ninja + clang-cl.
     The submodule itself stays pristine; there is no fork. The build is cached by
     (pin, patch hashes).
   - The patches:

     | Patch | Fix |
     |---|---|
     | 0001 | 33 dropped instructions |
     | 0002 / 0003 | the switch-hole family |
     | 0004 | vcmpgtub./vcmpgtuh. CR6 |
     | 0005 | vpkd3d128 / vupkd3d128 FLOAT16_2 and non-record vcmpbfp |
     | 0006 | divw / divwu / divd / divdu with PowerPC semantics (no x86 #DE on a zero divisor or INT_MIN / -1; result 0, as Xenia) |
     | 0007 | raw guest-memory addresses (lwarx/stwcx, lvx/stvx, dcbz, setjmp/longjmp) as `base + PPC_GUEST_OFFSET(X)` |
3. **Recompile** into `build/regen/ppc_out`, about 8 s. XenonRecomp joins toml paths onto the
   toml's directory as strings, so regen.py writes a build copy of the toml with paths
   relative to it.
4. **assemble.py.**
   - Checks the output and edits nothing: a raw `(base + X` or an unguarded division fails
     the step (an emitter patches 0006 / 0007 do not cover, e.g. after a XenonRecomp update).
     Until iter-467 assemble.py and patch_div_guards.py wrote both forms in as text.
   - Copies the hand headers.
5. **flag_transform.py --deployed guest/ppc.**
   - Register localization with the committed ABI bridges, about 2 min.
   - The functions listed in `[localize] keep_ctx_style` stay ctx-style. This includes every
     function that was hand-spliced until R1, exactly as they were.
6. **Stamp.** `guest/ppc/.regen_stamp` holds the hash of the XEX, the pin and every input. The
   next configure is a no-op until an input changes.

Options:
- `-DSR_REGEN=OFF` skips the step.
- `-DSR_XENONRECOMP_SRC=<dir>` points the bootstrap at another XenonRecomp checkout that has
  the pinned commit (a worktree without initialized submodules).
- `SR_REGEN_TOML=<path>` (environment) regenerates from another config, for A/B checks.

## Adding behaviour: hooks, never edited generated code

- **Strong-def override** (entry/exit logic): in `kernel/`,
  `PPC_FUNC(sub_X) { ...; __imp__sub_X(ctx, base); ... }`.
- **Mid-function hook**: a `[[midasm_hook]]` in superman_regen.toml with its C++ side in
  `kernel/`.
  - Registers are passed by reference; there is no ctx or base. Guest memory goes through
    PPC_LOAD_* / PPC_STORE_*.
  - Control options: `return`, `jump_address`, `return_on_true/false`,
    `jump_address_on_true/false`, `after_instruction`.
  - XenonRecomp emits a label for every jump target and prints it before the hook.
  - One hook per address.
- **Recompiler bug**: a new `patches/NNNN-*.patch` against the pinned upstream.
- **Function boundaries**: `functions = [{ address, size }]` in the toml.

Every override and every midasm hook must be registered in docs/OVERRIDES.json (classified;
`scripts/gen_override_manifest.py` keeps the mechanical fields). Every hook and every R1 port
also carries `default_exe` (live / conditional / support / observe / platform / gated).
scripts/audit_forcing.py fails on an unregistered hook or one without `default_exe`, and counts
the `live` and `conditional` band-aids in the forcing number that
`scripts/forcing_baseline_count.txt` ratchets. The band-aids ported by R1 live in
`kernel/r1_hooks_*.cpp`; each counts its fires at runtime (`[R1_FIRE]` log lines).

## Recompiler facts (current; history in git and docs/SPLICE_CONVERSION.md)

- A fresh XenonRecomp run with `patches/0001-0005` reports 0 unrecognized instructions; the
  only comment-without-code sites are deliberate no-ops (traps, cache hints, nop, sync).
  0001 implements 33 instructions upstream dropped (`dropped_instrs.py` measures the census).
- 0002 / 0003 (switch holes): jump-table targets outside the analyzed body extend the function
  bounds, and switches dispatch on `.u32`. A fresh regen has 0 `// ERROR: 0x` sites and 0 u64
  switches; any residue is a label outside every discovered function - investigate it.
- 0004: `vcmpgtub.` / `vcmpgtuh.` update CR6. 0005: `vpkd3d128` / `vupkd3d128` FLOAT16_2 and the
  non-record `vcmpbfp` (all three were `__builtin_debugtrap()`).
- The framed `// ERROR 8XXXXXXX` direct-branch tails left in the output are in X-8 artifact
  functions (a data-word pair lifted as code, 8 bytes before the real entry), unreachable; the
  live one, `sub_8272C4E0` (7 switch cases returned a stale f1), has a toml `functions` entry
  since D3 (docs/SPLICE_CONVERSION.md finding 2 and "D3 follow-ups").
- XenonAnalyse produces the same 555 switch tables as `superman_switch_tables.toml`.
- Chunk numbering (`ppc_recomp.N.cpp`) shifts between regens: compare per function.

## Register localization (iter-464 codegen lane)

**What.** XenonRecomp's code-generation options (`skip_lr`, `skip_msr`, `ctr_as_local`,
`xer_as_local`, `reserved_as_local`, `cr_as_local`, `non_argument_as_local`,
`non_volatile_as_local`) make guest registers C++ locals of each recompiled function
instead of `ctx` fields: they live in host registers, dead CR/XER/`ctx` stores vanish and
the `__savegprlr`/`__restgprlr` helper calls disappear. The recompiler still runs with the
`[main]` flags OFF (its ctx-style output is what the patch scripts and the hand bodies are
written in); `tools/regen/flag_transform.py` applies the flags of the `[localize]` table
TEXTUALLY to every generated function afterwards. The transform is the recompiler's own
flag semantics: `flag_transform.py --equivalence tools/regen/ppc_out_base
tools/regen/ppc_out_all --toml tools/regen/variants/all.toml` (two raw regens of the same
XEX, flags off / flags on; `variants/*.toml`) reports **0 differing functions of 34,511**.

**Two styles, one tree.** The functions listed in `[localize] keep_ctx_style` (since R1 this
includes every function that was hand-spliced before, so the committed bridge spec stays valid)
and the `__save*`/`__rest*` helpers stay ctx-style; for them the `ctx` fields ARE the registers.
Every function with a `[[midasm_hook]]` is in that list: a hook receives the registers by
reference, and the host code that reads ctx (kernel overrides) sees them there. Generated bodies are localized. The two interoperate through the ABI
BRIDGES that `tools/regen/abi_audit.py` solves over the whole program (the deployed tree,
ctx-style bodies included) and `flag_transform.py` inserts (`// abi-bridge` lines):

- a localized function that reads a register before writing it (`IN`: MSVC SEH funclets
  reading the parent's r12/r30, the hand-written XMV/WMV/WMA decoder helpers taking inputs
  in r11/r15/r20/v64+, nested asm helpers passing them through) loads it from `ctx` at
  entry; a localized direct caller flushes it to `ctx` before the call (a ctx-style caller
  already has it there);
- a function that hands a result back in a register other than r3/r4/f1 (`OUT`: the
  caller reads it after the call and the callee changes it - the SEH funclets writing the
  parent's r29/r11) writes it to `ctx` before returning when localized; a localized caller
  reloads it after the call;
- around such a call the non-volatile `ctx` fields involved are saved and restored, so a
  ctx-style ancestor still sees them preserved (as the flags-off epilogues did);
- `IN` roots with no direct caller (XenonAnalyse chunk duplicates and pointer-table
  prefixes lifted as functions, SEH filters, vtable-only targets) load from `ctx` and log
  `[ABI_UNBRIDGED]` once if they ever run (`SR_ABI_UNBRIDGED_ENTRY`, hand `ppc_config.h`;
  `sr_abi_unbridged_entry` in `guest/guest_harness.cpp`).

Unsaved non-volatile writes are NOT bridged by themselves: with locals the clobber is
invisible to the caller, which is the ABI-correct behaviour (165 VMX leaves write v14-v31
without saving; no caller reads them after). Excluded on purpose: the guest setjmp
`sub_827A45E0` (its reads only save registers to the jmp_buf; the only working longjmp is
the registered C++-throw path of the kernel `sub_82762400` (kernel/r1_platform.cpp) and the
`sub_827A4270` override) and the guest
longjmp (no-return). `ctx.lr` is no longer maintained by generated code (`skip_lr`): the
LR-based diagnostics (`[THREAD_SAMPLER] lr=`, caller-LR log fields, `SR_TRACE_CALLS`) show
the last ctx-style-body / host-written value; host crash and hang triage uses the host stack
(every guest function is a host function `sub_XXXXXXXX`). Host code must not read the
localized `ctx` fields of a generated function (`SR_PPC_LOCALIZED` in `ppc_config.h`).

**Kept ctx-style on purpose.** `[localize] keep_ctx_style = ["sub_822C6E30"]` lists generated
functions that stay ctx-style (treated as hand bodies by the bridges). The cloth tangent
function is there because dev's configure-time `cloth_capacity.py` copies its ctx-style
body text into `kernel/cloth_capacity.cpp` and hash-checks it (iter-465).

**iter-466 tick2: two hot ctx-style bodies localized.** `sub_8231A5F8` (BoundsVolumeIntersectTest)
left the splice list: its only hand edit was `PPC_GUEST_OFFSET` on the lvx/stvx addresses, which
patch 0007 now emits for every generated body. `sub_823055A8` (ClothUpdateElementLightMask)
uses v122-v127 only as its own callee-saved scratch, so `[localize] localize_despite_regs` exempts it
from the v64-v127 rule. A fresh `--solve` on the re-assembled tree changes exactly these two bodies
and no bridge (archive/v1_tick/regen_t0..t2). **Every checkout's untracked guest/ppc** picks them up
with the committed spec: `python tools/regen/flag_transform.py --deployed guest/ppc` (no `--solve`;
rewrites only those two bodies, idempotent). Proof: docs/PERFORMANCE.md 2.9 (SR_LOCALIZE_CHECK).

**Staleness guard (iter-466 tick2).** Every `flag_transform.py --deployed DIR` writes
`DIR/.transform_stamp` (`flag_transform v1 <sha256>` over flag_transform.py, abi_bridge.json,
superman_regen.toml). guest/CMakeLists.txt recomputes the hash at configure time
(re-run by ninja whenever an input or the stamp changes) and FAILS with the command to run when the
deployed tree is stale or unstamped: `python tools/regen/flag_transform.py --deployed guest/ppc`
(seconds-to-minutes, idempotent). `-DSR_ALLOW_STALE_GUEST=ON` downgrades it to a warning (tests only).
regen.py runs this step itself, so after R1 a configure keeps both stamps current.

**Workflow.** `--solve` re-derives the spec from the deployed tree (do it when
`keep_ctx_style` changes: a function that leaves the list becomes localized); without `--solve` the committed `abi_bridge.json` is re-applied (idempotent).
`--collisions guest/ppc` lists generated-body C++ locals that shadow a register name (they
are renamed `X_h`). To turn a flag off, edit `[localize]`, re-deploy the ctx-style tree
(regen.py's stage, before flag_transform) and re-run `--solve`: a localized tree cannot be un-localized in place
(`ctx.lr` lines are gone).
