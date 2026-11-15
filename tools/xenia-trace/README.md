# L9-A — Instrumented-Xenia Golden Oracle (trace-emit patch)

First concrete increment of **L9-A** from `docs/L9_DIFFERENTIAL_ORACLE.md` §A: a
trace-emit patch against the **xenia-canary** emulator that writes a ground-truth,
ordered, per-guest-function-entry trace of a *working* boot to the Superman Returns
menu. That `xenia_boot.trace` is the golden stream the **L9-C divergence bisector**
walks in lockstep against a deterministic replay of our recomp build.

This directory contains the **patch, header, and instructions only**. Xenia is
*not* built here (it needs a heavy toolchain — see "What remains").

## Validation status (honest)

- **Patch applies cleanly** — `git apply --check` + a real `git apply` both pass against the cloned
  xenia-canary rev, then reverted (verified).
- **Header compiles + emitter is correct** — `xenia_trace.h` compiles clean with the project's own
  clang++ (`-std=c++17`), and a standalone driver proved `GuestTracer::EmitFuncEntry` writes the exact
  canonical binary format (`XENTRC01` header + 32-byte records) that the **L9-C bisector reads back
  successfully**. So the emitter *logic* is trusted.
- **BUILT + CAPTURED (2026-06-26):** instrumented `xenia_canary.exe` built (Release; needed manual
  submodule init, a CMake/vcvars64 drive around xenia-build's broken VS detection, regenerated
  `version.h`, and Vulkan SPIR-V tools extracted from the installer w/o admin). Ran against
  `SupermanReturns NO SOUND.iso` (`--gpu=d3d12 --apu=nop --hid=nop`, `XENIA_GUEST_TRACE=1`) ~5 min,
  healthy boot, clean close → **`xenia_boot.trace`, 40,304 records, valid `XENTRC01`, bisector
  self-diff PASS**.
- **~~COVERAGE LIMIT~~ — FIXED (2026-06-26, rich JIT-prolog hook):** the original `GuestFunction::Call`
  C++ hook only fired on **host→guest re-entries** (guest→guest `bl` jumps stay inside the JIT and
  bypassed it), so the first capture was only **~5 distinct PCs** (`lr=0xbcbcbcbc` uninit sentinel).
  This is now resolved by a **second hook emitted at the x64 JIT function prolog** (see
  `xenia_trace_jitprolog.patch`), which fires on **EVERY** guest function entry, including guest→guest
  calls. Re-captured `xenia_boot_rich.trace`: **348,803,967 records, 2,223 distinct guest PCs across 6
  threads** (vs ~5 before), valid `XENTRC01`, bisector self-diff **PASS** (strict + subsequence). The
  `lr` field now carries real caller return addresses (e.g. `0x82468d18`), not the sentinel. Sampled
  PCs resolve across the whole codebase (`sub_820F0048` … `sub_828E9790`, `ppc_recomp.0`→`.129`),
  spanning multiple subsystems. The on-disk format is **byte-identical**, so L9-C is unaffected by
  which hook produced the trace.
  - **Boot reach:** the rich capture ran ~70 s, reached the D3D12 surface with the `Superman Returns
    <Direct3D 12 …>` window title (game loaded + rendering); closed cleanly (destructor flushed,
    32-byte-aligned). It was stopped before the on-screen `ScrPressStart` purely to cap the trace size
    — see the perf note below. To reach the visible menu, run longer (trace will be tens of GB).
  - **PERF (honest):** emitting a host call at every prolog is a hot path. The rich boot produced
    ~349M records / **11 GB at ~100 MB/s**. The boot stays *healthy* (no ABI break, no crash — the
    call routes through the standard guest→host thunk which saves/restores volatile regs + MXCSR), so
    it is fully usable, but the trace is large. Capture only as long as needed, and **analyze by
    streaming**: `scripts/l9/divergence_bisect.py --stream` does the full diff in O(window) memory
    (verified ~19 MB peak diffing the entire 11 GB trace; the in-memory default would need ~11 GB+).
    Use `--stream` for any multi-GB trace. With `XENIA_GUEST_TRACE` unset, the emission is skipped
    entirely and JIT'd code is byte-for-byte stock Xenia (zero overhead).

  The exe is built and ready at `reference/xenia-canary/build/bin/Windows/Release/`.

## Files

| File | What it is |
|---|---|
| `xenia_trace.h` | The trace writer + the on-disk record format (header-only). Copied into `src/xenia/cpu/`. |
| `xenia_trace.patch` | Unified diff adding the gated emit call into `GuestFunction::Call` (host→guest re-entry hook; low-risk first increment). |
| `xenia_trace_jitprolog.patch` | **Unified diff adding the RICH per-function hook at the x64 JIT prolog (`X64Emitter::Emit`). This is what makes the trace complete (2223 distinct PCs).** Apply after `xenia_trace.patch`. |
| `README.md` | This file. |

## Where the hook goes (verified against cloned source)

Cloned `reference/xenia-canary` @ commit `0b4e6c440e91f118b42a5e9e32a6b629ce4c387c`
(2026-06-24).

**Primary hook (this patch): `xe::cpu::GuestFunction::Call`**
- File: `src/xenia/cpu/function.cc` (lines ~132–147 in the cloned rev).
- This is the backend-agnostic C++ choke point through which a guest function is
  entered *when control re-enters the guest from host/kernel C++* (the
  host→guest thunk boundary). It is exactly the site named in §A
  ("`xe::cpu::Function::Call`").
- Registers are read from `thread_state->context()` — a `xe::cpu::ppc::PPCContext`
  whose layout we verified in `src/xenia/cpu/ppc/ppc_context.h`:
  `uint64_t r[32]` (so `r[3]`, `r[1]`), `uint64_t lr`, and `uint32_t thread_id`.
  Thread id is also available as `thread_state->thread_id()`
  (`src/xenia/cpu/thread_state.h`).

### COVERAGE CAVEAT — read this (verified architectural fact)

xenia-canary has **no interpreter backend**. The `--cpu` flag is a no-op stub:
`cpu_flags.cc` literally documents it as *"Does nothing. CPU backend [any, x64]."*
The standalone PPC interpreter that older xenia-master had (and that §A / iter-213O
assumed we'd "force") has been **removed**. The only real backend is the **x64 JIT**
(`src/xenia/cpu/backend/x64/`), with an experimental `a64`.

Consequence: the JIT compiles each guest function to host code, and guest→guest
`bl` calls jump **directly** between JIT'd functions **without** going back through
`GuestFunction::Call`. So this `Call`-site hook captures **guest entry points /
call-ins from host/kernel code**, *not literally every* guest function activation.

That is still highly useful as a first golden oracle (it gives the ordered
high-level call spine, with r3/r1/lr/thread at each re-entry), and it is the
**low-risk** increment §A asks for. For a **complete** per-function trace, use the
upgrade path below.

### Complete per-function trace — IMPLEMENTED (`xenia_trace_jitprolog.patch`)

The rich hook lives at the JIT per-function **prolog** in
`X64Emitter::Emit(builder, func_info)` (`src/xenia/cpu/backend/x64/x64_emitter.cc`),
**immediately after** the existing `DebugInfoFlags::kDebugInfoTraceFunctions` block
(stack already allocated and 16B-aligned). The prolog runs on **every** guest
function entry, including guest→guest `bl` calls, so this captures the full
per-function spine.

How `guest_pc` is obtained (verified against the source): rather than relying on
`RDX` at runtime (the README originally guessed RDX; the `kDebugInfoTraceFunctions`
block does load the guest addr into `edx`, but that is incidental), we use
`current_guest_function_` — the `uint32_t` member set to `function->address()` at
the top of the guest `Emit(GuestFunction*…)` overload (line ~117). It is a
**compile-time constant per function**, so we bake it as an immediate `CallNative`
arg0. That is unconditionally correct and independent of register state.

The emitted code is a gated `CallNative(&XeTraceJitProlog, current_guest_function_)`.
`CallNative` → `CallNativeSafe` routes through the standard **guest→host thunk**
(`backend()->guest_to_host_thunk()`), which `EmitSaveVolatileRegs` /
`EmitLoadVolatileRegs` and restores guest MXCSR around the call, and passes the
`PPCContext*` (rsi) as the host fn's first arg. So no hand-rolled register saving
is needed and the Windows prolog / `X64CodeCache` unwind constraints are respected
(we add code *after* the prolog, not inside it). The host trampoline
`XeTraceJitProlog(void* raw_context, uint64_t guest_pc)` reads `lr`/`r3`/`r1`/
`thread_id` from the context and calls `GuestTracer::EmitFuncEntry`.

Emission is gated on `GuestTracer::Get().enabled()` (the `XENIA_GUEST_TRACE` env,
checked once at JIT time), so with tracing **off** the JIT'd code is byte-for-byte
stock Xenia (zero overhead). The **on-disk format is identical** to the `Call`-site
hook, so the L9-C bisector is unaffected by which hook produced the trace. The
original `Call`-site hook (`xenia_trace.patch`) is **kept** alongside (harmless —
its records are a subset, same format).

### Guest memory writes — why they are NOT in this patch

§A also wants gated `(addr, value)` records on guest stores. In the JIT there is
**no single C++ store helper**: stores are compiled inline to host writes
(`ppc_emit_memory.cc` → HIR `f.Store` → host `mov`), so there is no cheap choke
point. Options (future work): (a) a HIR pass that wraps `OPCODE_STORE` with a gated
helper call, or (b) a host watchpoint on the guest store range. The
`XeTraceRecord.tag` field is already reserved (`kXeTraceMemWrite = 1`) so memory
writes can be interleaved into the same fixed-width stream later without a format
change.

## On-disk trace format (consumed by L9-C)

Little-endian (Xenia host is x86-64 LE). One 32-byte **header**, then N 32-byte
**records**. Fixed width → the bisector can index by record number with no parsing.

```
Header (32 B, once):
  0  char     magic[8]    = "XENTRC01"
  8  uint32   record_size = 32
  12 uint32   endian_tag  = 0x04030201   (LE sanity check)
  16 uint64   reserved0   = 0
  24 uint64   reserved1   = 0

Record (32 B, one per guest-function ENTRY, in execution order):
  0  uint32   guest_pc    guest function address (0x82xxxxxx)
  4  uint32   lr          caller return address (low 32 of PPCContext.lr)
  8  uint64   r3          arg0 / return reg (full 64-bit GPR)
  16 uint64   r1          stack pointer (full 64-bit GPR)
  24 uint32   thread_id   guest thread id (interleaving context)
  28 uint32   tag         0 = func-entry (kXeTraceFuncEntry); 1 reserved (mem-write)
```

The exact struct + constants live in `xenia_trace.h` (`XeTraceHeader`,
`XeTraceRecord`, `XeTraceTag`), with `static_assert`s pinning both to 32 bytes.

## Cross-check against L9-C (divergence bisector) — §C

§C says: *"both traces are ordered `(guest_pc, key regs)` streams. Walk them in
lockstep; first index where they differ is the divergence frontier. Feed that VA to
`va_to_source` + Ghidra."*

This format satisfies that contract:
- **Ordered stream of guest_pc** — records are appended in execution order; the
  bisector reads them sequentially and compares `guest_pc` at each index.
- **Key regs for state disambiguation** — `r3`, `r1`, `lr` are exactly the regs the
  project's crash-parser / call-tracer already key on (see `CLAUDE.md`), and the
  recomp side can emit the same tuple at its `PPC_CALL_INDIRECT_FUNC` / function-entry
  hook (L9-B) so the two streams are directly comparable.
- **`thread_id`** lets the bisector compare per-thread or tolerate benign
  cross-thread interleaving differences (important given the DOGMA race) instead of
  diffing a globally-interleaved order.
- **Fixed 32-byte records + a magic/endian header** make the L9-C reader trivial
  (`mmap`, skip 32-byte header, treat the rest as `XeTraceRecord[]`).

**One alignment note for whoever builds L9-B/C:** the recomp-side emitter must
sample the *same* point (function entry, regs read *before* the body runs) and the
*same* `guest_pc` convention (function start address, not mid-function PC) so the
two streams line up. This patch samples `address()` (function start) and reads regs
before `CallImpl`, which matches that convention.

## Build + run (what you do once you have a Xenia toolchain)

> Not done here. These are the steps to produce `xenia_boot.trace`.

1. **Apply the patch** (from the xenia-canary repo root,
   `reference/xenia-canary/`):
   ```sh
   cp ../../tools/xenia-trace/xenia_trace.h src/xenia/cpu/xenia_trace.h
   git apply ../../tools/xenia-trace/xenia_trace.patch
   ```
   `git apply --check` passes against rev `0b4e6c44`. The header is auto-added to
   the build: `src/xenia/cpu/CMakeLists.txt` uses `xe_platform_sources`, which
   globs `*.h` in the dir (`cmake/XeniaHelpers.cmake`), so just re-run CMake
   configure. No CMakeLists edit required.

2. **Build Xenia** per upstream instructions (Windows: VS2022 + the `xenia-build`
   bootstrap; this pulls a large `third_party` tree and takes a while). Heavy
   toolchain — out of scope for this task.

3. **Run to the menu with tracing on.** Force a fresh trace file and run the
   sound-stripped ISO that is known to reach the menu:
   ```sh
   # Windows (PowerShell)
   $env:XENIA_GUEST_TRACE = "1"
   $env:XENIA_GUEST_TRACE_FILE = "xenia_boot.trace"   # optional; this is the default
   .\xenia_canary.exe "path\to\superman_returns_soundstripped.iso"
   ```
   (There is no interpreter to force — x64 JIT is the only backend. The `--cpu`
   flag is a no-op.) Drive the boot until `ScrPressStart` / the front-end menu is
   on screen, then quit. The destructor flushes and closes the file; for a clean
   capture, exit Xenia normally rather than killing it.

4. **Result:** `xenia_boot.trace` next to the working dir — the golden stream for
   L9-C. Sanity-check it: first 8 bytes must be `XENTRC01`, byte 8 (`record_size`)
   must be 32, and `(filesize - 32) % 32 == 0`.

## What remains to actually build + run it

- Stand up the Xenia build toolchain (VS2022 + `xenia-build setup/build`) and
  compile the patched tree. **Not attempted here** (heavy).
- Provide the sound-stripped, menu-reaching ISO at run time.
- Write the tiny **L9-C reader** (`mmap` + walk `XeTraceRecord[]`); the format
  above is all it needs.
- (Optional, for full coverage) port the hook to the JIT prolog emit site per the
  upgrade path above, or add the HIR-store / watchpoint path for memory-write
  records.
- (Optional) emit the recomp-side companion stream (L9-B) at the matching
  function-entry point so the two are directly diffable.

## `xenia_trace_dump_seek.patch` (iter-456p, 2026-08-31)

Adds `--trace_dump_last_command=N` to `xenia-gpu-d3d12-trace-dump`, so playback
can stop at a chosen command instead of always running to the last one. Intended
for attributing pixels to a draw range on the ORACLE side.

**Build:** the Xenia tree here builds with MSVC `cl.exe`, not the clang toolchain
our own project uses, so ninja needs the VS environment:

```
cd reference/xenia-canary/build
cmd /c build_td.bat      # calls vcvars64.bat then: ninja -f build-Release.ninja xenia-gpu-d3d12-trace-dump
```

Incremental rebuild is 3 steps (compile, lib, link).

**⚠ IT DOES NOT YET GIVE PER-DRAW IMAGES.** The seek itself works — the log
confirms `TraceDump: seeking to command 1800 of 3501` — but `TraceDump::Run()`
saves the PNG via `presenter->CaptureGuestOutput()`, which has output only after
a SWAP. Truncated playback never reaches the swap, so every truncated dump writes
no file. Measured: a 15-point sweep produced a PNG at command 3501 and nothing at
0/100/200/300/400/500/600/800/1000/1400/1800/2200/2600/3000.

Getting per-draw images needs mid-frame RENDER TARGET capture rather than
presenter guest output. Not attempted — out of the bounded pass's scope.

**What the tool IS good for right now:** `--async_shader_compilation=false` makes
the stock dumper render this trace correctly (2,612 skipped draws -> 0), which is
how `archive/iter456p_ORACLE_TITLE.png` was produced. Without that flag it emits
a one-colour magenta frame and 2,612 `Skipping draw - pipeline not ready` lines.

## `xenia_trace_ablation.patch` (iter-456p, supersedes the seek-only patch)

The COMPLETE trace-player instrumentation for ROI attribution, across all three
files it touches. It replaces `xenia_trace_dump_seek.patch`, which covered only
`trace_dump.cc` and therefore would NOT have restored Slices B or C.

⚠ **This patch is the only durable copy.** `reference/xenia-canary/` is a nested
checkout ignored by the outer repo, so edits there are invisible to
`git status` at the root and vanish if that tree is reset. The director caught
exactly this: Slices B and C were reported as committed when their operative
files were only dirty in the ignored checkout.

Contents:

- `trace_dump.cc` — `--trace_dump_last_command=N` (seek), plus FAIL-CLOSED
  ablation accounting: a skip request not honoured EXACTLY refuses to write an
  image, so a silent no-op cannot masquerade as "omitting these draws changed
  nothing".
- `trace_player.cc` / `.h` — `--trace_skip_draw_first/last` (1-based inclusive,
  0/0 disables) implemented at `kPacketEnd` via
  `PacketDisassembler::GetPacketCategory`, so only `kDraw` packets are omitted
  and every generic packet, memory/register record, event and the SWAP still
  runs; per-playback counters; and `--trace_log_sampled_textures`, which logs the
  ACTIVE VS/PS parsed bindings using `Shader::TextureBinding::fetch_constant`.

**Build:** `cd reference/xenia-canary/build && cmd /c build_td.bat` (Xenia needs
vcvars/MSVC; our own tree is clang). Incremental rebuild is ~6 ninja steps.

**Always pass `--async_shader_compilation=false`**, or 2,612 draws are skipped
while pipelines build and the dump is a magenta blank.

## `xenia_resolve_dump.patch` (P1, 2026-09-29) - per-resolve oracle

Adds `--trace_dump_resolve_dir=<dir>` to the D3D12 command processor. With
`--readback_resolve=full`, every resolve's destination bytes (guest layout, exactly what
was read back into guest memory) are written as `<dir>/r<NNN>_<address>_<length>.bin` in
resolve order. The trace itself cannot supply these: its MemoryRead restores never carry
resolve outputs (the player re-executes resolves), so this is the only Xenia ground truth
for intermediate passes (shadow maps, HDR scene, luminance, bloom).

    reference\xenia-canary\build\build_td.bat          (ninja, trace-dump target)
    xenia-gpu-d3d12-trace-dump.exe --async_shader_compilation=false --readback_resolve=full \
      --trace_dump_resolve_dir=<dir> --trace_dump_path=<dir> --target_trace_file=<xtr>

The final PNG is unchanged by the readback (menu: 1,847,448 bytes, same as the banked
oracle). Consumer: `SR_PM4_ORACLE=<dir>` in our replay (`docs/RENDERER_PM4_HYBRID.md` s6).

### Update (same day): `xenia_resolve_dump.patch` is now the CUMULATIVE `src/xenia/gpu` diff

It carries the ablation hunks above plus two P1 oracles, so apply it alone (instead of
`xenia_trace_ablation.patch`). Second oracle - **per-draw EDRAM**:

    xenia-gpu-d3d12-trace-dump.exe --async_shader_compilation=false \
      --trace_dump_edram_draws=25,26,34 --trace_dump_edram_dir=<dir> --trace_dump_path=<dir> \
      --target_trace_file=<xtr>

writes the whole EDRAM (guest layout, 10 MiB) as `<dir>/e<NNNNN>.bin` after each listed
1-based draw packet (`CommandProcessor::DumpEdram`, via the trace-writer's own download
path). Our side: `SR_PM4_EDRAM_DUMP=25+26+34` (`+`-separated; replay_ab.ps1 splits gates on
commas) and `SR_PM4_EDRAM_DUMP_DIR`. Compare one surface with the same decoder our
resolves use: `build\difftest.exe --edram-surface <xenia.bin> <ours.bin> <RtKey hex> <rows> <outprefix>`.

## Oracle v2 (iter-463 burndown): VA-filtered entry trace + indirect-call targets

Tree: `reference/xenia-canary-oracle` (canary_experimental @ 0bd090dbe, the build behind the
`sound_menu` / `sound_gameplay` golden dumps). `xenia_trace_oracle.patch` + the v2
`xenia_trace.h` supersede `xenia_trace.patch` + `xenia_trace_jitprolog.patch` for this tree
(those two stay for `reference/xenia-canary` @ 0b4e6c4, which the v2 header still builds with:
all v1 behaviour is kept when the v2 variables are unset).

New, all read once at first use:

| Variable | Effect |
|---|---|
| `XENIA_GUEST_TRACE_VAS=list` | Prolog hook only in functions whose entry VA is listed (JIT-time decision; every other function is stock code). A mid-function VA shows up only if the guest really called it: the JIT creates a function at a call target on demand. |
| `XENIA_GUEST_TRACE_ICALL_FUNCS=list` | Every indirect call (`bctrl`/`bctr`) made inside the listed functions: tag 2, `guest_pc` = target, `lr` = call-site return address, `r3` = this. |
| `XENIA_GUEST_TRACE_ICALL_BAD=1` | Instrument every indirect call everywhere, record only targets outside `[0x820F0000,0x8290F5B8)` or misaligned: tag 3. That is exactly our `PPC_CALL_INDIRECT_FUNC` "out-of-range" condition. Costs one host call per indirect call. |
| `XENIA_GUEST_TRACE_CODE_LO/HI` | Override those bounds (hex). |

Lists are hex VAs separated by commas/whitespace/semicolons, or `@file` (`#` comments).
With any v2 filter active every record is `fflush`ed, so a crash or kill keeps the file.
The call-site hook parks the target in `PPCContext::scratch` around the `CallNative` (which
clobbers rcx/rdx/rax before the thunk saves registers) and reloads it; `ctx->lr` is valid
there because the frontend's `StoreLR` precedes the call and calls are volatile.

**Header test** (no Xenia): `clang++ -std=c++17 tools/xenia-trace/test_xenia_trace.cpp` and
run it with a scratch dir: `PASS (0 failed, 6 records)`. Three mutations (CODE_HI bound,
bad-only filter, comment parsing) each make it fail.

**Build** (separate output dir `build-trace/`; the user's Xenia install is never touched):

1. Submodules: the oracle pins the same 34 submodule commits as `reference/xenia-canary`, so
   populate them offline from that tree with per-command `url.<local>.insteadOf=<github url>`
   overrides (`git -c protocol.file.allow=always ... submodule update --init`; same for
   DirectXShaderCompiler / rapidjson / snappy nested modules).
2. `cp tools/xenia-trace/xenia_trace.h src/xenia/cpu/` and `git apply tools/xenia-trace/xenia_trace_oracle.patch`.
3. With vcvars64 + ninja on PATH: `cmake -S . -B build-trace -G "Ninja Multi-Config" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DXENIA_BUILD_TESTS=OFF`.
4. Write `build-trace/version.h` (the xb script normally generates it; the three `XE_BUILD_*` defines).
5. `set VULKAN_SDK=%USERPROFILE%\VulkanSDK\1.4.350.0` (+ its `Bin` on PATH, for the SPIR-V
   shader step), then `ninja -f build-Release.ninja xenia-app` ->
   `build-trace/bin/Windows/Release/xenia_canary.exe`.

**Run** (one emulator process at a time, never overlapping a recomp roll):

    powershell -File tools/xenia-trace/run_oracle_trace.ps1 -Tag burndown_a
    powershell -File tools/xenia-trace/run_oracle_trace.ps1 -Tag burndown_b -IcallBad

It copies the user's `xenia-canary.config.toml` into the build's portable dir once (read only
at the source), starts `tools/xenia_pad.py`, boots the full-sound ISO, drives PRESS START ->
DOWN A (no profile) -> A (skip the opening cinematic) -> A (the flow in
`archive/v1_audio/oracle/findings.json`), screenshots each step, closes Xenia at
`-TotalSeconds`, and writes `oracle.trace`, `xenia.log`, `summary.txt` / `summary.json` to
`archive/v1_burndown2/xtrace/<Tag>/`. VA lists: `burndown_vas.txt` (item-11 stub VAs, item-10
fixed-VA cases, the out-of-range call sites' functions, the native-roadmap liveness VAs,
positive control RtlFreeHeap) and `burndown_icall_funcs.txt`.
`summarize_trace.py <trace> --vas <list>` prints every listed VA, including those never entered.

## Import census (logic audit 2.0c, 2026-10-18)

The oracle half of our `SR_AUDIT_IMPORTS` census (docs/LOGIC_AUDIT.md 2.0c): per export, entries, returns, the
caller-lr histogram, the result histogram and the first 8 calls' r3-r8 and result, written as JSON in the same
schema (`sr-import-audit/2`).

- `sr_import_census.h`: the census. It is self-contained and RECORDS ONLY: it never changes a call, a register or a
  result.
- `xenia_import_census.patch`: 9 lines in `src/xenia/kernel/util/shim_utils.h`, the export trampoline
  (`X::Trampoline`): `Enter` before the body, `Leave` after `result.Store`.
- `test_import_census.cpp`: the header test with no Xenia, `clang++ -std=c++17 -O1
  tools/xenia-trace/test_import_census.cpp` and then `test_import_census.exe <scratch>`. It passes 9/0.
- `build_import_census.ps1`: the cloud-worker build. It makes a blobless clone, checks out 0bd090dbe exactly, copies
  in the header and applies the patch, configures with CMake + Ninja Multi-Config (MSVC) and builds the target
  `xenia-app`. VULKAN_SDK points at a copy of glslangValidator / spirv-opt / spirv-dis for the SPIR-V shader step.
  It writes `build_census.txt` (the commit, hashes, exit codes, the tree status before and after the build).

Run: `XENIA_IMPORT_CENSUS=<path.json>` (optional `XENIA_IMPORT_CENSUS_PERIOD=<s>`, default 10). The file is rewritten
every period and once at exit. Unset, the build behaves as stock 0bd090dbe. Run it only from an agent folder, never
the owner's install, and compare with ours using the same JSON reader.

