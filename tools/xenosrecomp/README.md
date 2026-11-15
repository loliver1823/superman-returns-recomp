# XenosRecomp pipeline (shader spike, iter-359)

Static Xenos shader → HLSL/DXIL recompilation for the menu render path.
Upstream: https://github.com/hedge-dev/XenosRecomp (MIT), pinned as the submodule
`third_party/XenosRecomp` (990d03b). This dir holds our patch series and the tools around it.

## The game build's sources (`prepare.py`)

The game compiles the recompiler in (runtime translation, below). At configure time
`cmake/xenos_xlat.cmake` runs `prepare.py`, which exports the pinned commit's `XenosRecomp/` and
`LICENSE.md` from the submodule (`git archive`; the submodule stays pristine), applies the patches
in `prepare.PATCHES` order (`superman.patch`, `0002-...`, `0003-...`) and writes the result to
`<build>/xenosrecomp`, byte for byte the clone below (CRLF except `main.cpp` and
`shader_recompiler.cpp`, as the clone has them; see the script). fmt, smol-v, zstd and xxHash come from
the submodule's own pinned submodules; DXC from the official release (`cmake/dxc_release.cmake`).
A changed patch is picked up at the next configure (the key in `<build>/xenosrecomp/key.txt` covers
the patches). `-DSR_XENOSRECOMP_DIR=<clone>` builds from a clone instead;
`-DSR_XENOSRECOMP_SRC=<checkout>` prepares from another checkout of the pinned upstream (a worktree
without initialized submodules; `tools/cloud/sr_cloud.py` uses one shared checkout).

    python tools/xenosrecomp/prepare.py --out <dir> --compare tools/XenosRecomp-clone   # byte-compare

## Regenerate the offline toolchain (a clone)

The offline `XenosRecomp.exe` (it builds `data/shader_cache`, and `tools/difftest/shader_xlat_test_run.py`
compares it with the in-game translator) is built from a clone, `tools/XenosRecomp-clone/` (untracked):

```
git clone https://github.com/hedge-dev/XenosRecomp tools/XenosRecomp-clone
cd tools/XenosRecomp-clone
git submodule update --init --recursive     # 5 submodules, shallow clone ships them EMPTY
git apply ../xenosrecomp/superman.patch     # our modifications (see below)
git apply ../xenosrecomp/0002-iter463-setp-and-texture-lod.patch   # iter-463, item 7 below
git apply ../xenosrecomp/0003-iter464-resscale-pixel-position.patch  # iter-464 resolution scale (docs/RESOLUTION.md)
git apply ../xenosrecomp/0004-step4-const-relative-per-operand.patch   # step 4: per-operand constant addressing, item 8
git apply ../xenosrecomp/0005-step4-alpha-test-variant.patch   # step 4: the Xenos alpha test variant, item 9
git apply ../xenosrecomp/0006-step4-alpha-test-strict-compare.patch   # step 4 round 4: its compare in integers, item 9
copy ..\xenosrecomp\build_xenos.bat .
build_xenos.bat                             # vcvars64 + Ninja + LLVM clang-cl + /EHsc
# -> build/XenosRecomp/XenosRecomp.exe
```

MSVC cannot build it (`__builtin_bswap*`); the VS ClangCL toolset is not
installed — the batch uses the standalone LLVM clang-cl like the main repo.

## superman.patch contents (all proven on the 109-shader menu corpus)

1. **Fail-soft batch mode** (`main.cpp`): per-shader try/catch — one bad shader
   reports `SKIP <file> (<xxh3>): <reason>` instead of killing the batch;
   DXC-failure asserts converted to exceptions; each cache entry remembers its
   source file for the skip report.
2. **Unnamed bool constants** (`shader_recompiler.cpp`): bools set by register
   (absent from the D3DX ctab) emit the same `g_Booleans` bit test as named
   ones (`1 << (boolAddress + 16*isPS)`) instead of an undeclared bare `bN`.
3. **Relative constant indexing outside declared arrays**
   (`shader_recompiler.cpp`): new `g_UnnamedConstants(INDEX)` macro — SPIR-V
   branch: dynamic `vk::RawBufferLoad`; DXIL branch: a full-file
   `float4[256/224]` cbuffer at `register(b3/b4, space4)` that the RUNTIME MUST
   BIND to the same GPU VA as b0/b1 (dxc rejects packoffset overlap, hence the
   extra binding). Replaces the upstream hard assert (upstream gap).

4. **r0.x = SV_VertexID** (iter-458): Xenos loads the vertex index into r0 at VS entry.
5. **In-shader vertex fetch variant** (P1, 2026-09-29): every VS is also compiled with
   `fetchMode` into `<hash>.vs.vsf.dxil` - vfetch reads `g_VertexMemory` (t0 space5) at
   the address `fetch-constant base + index * stride` computed from the instruction's own
   index operand, with Xenia's endian swap, format unpack, signed/normalized modes and
   exp_adjust (`dxbc_shader_translator_fetch.cc`). Fetch constants arrive in b5 space5.
   No input attributes. The PM4 path draws with it (`SR_PM4_VSF`, default ON under
   `SR_PM4_RTCACHE=1`). Add variants to an existing cache without touching it:
   `set XENOS_VSF_ONLY=1` then run XenosRecomp on each corpus into a scratch output and
   copy the new `*.vsf.dxil` into `data/shader_cache/dxil/`.
6. **By-index interpolator linkage** (linkfix, 2026-09-29): VS export register N
   (kVSInterpolator0..15) is written to `oTexCoordN`, and the PS reads `rN` from
   `iTexCoordN` for every register its container table lists. Neither side takes a
   D3D usage from its container any more. That table is pre-link metadata: the XDK
   patches the exports at bind time, and a `*_synth.xpu` container carries another
   program's table. Xenos and Xenia link by index: Xenia `shader_translator.cc`
   ParseAluInstruction (`kInterpolator`, export N -> interpolator N),
   `dxbc_shader_translator.cc` StoreResult (VS) and StartPixelShader (PS `rN` <-
   interpolator N), `d3d12/d3d12_command_processor.cc` IssueDraw (mask = VS writes &
   PS inputs). Before this change VS 3A60C7C1 wrote TEXCOORD0 while PS 2E372EA2 read
   COLOR0, so the NO-STORAGE dialog body drew with alpha 0 (draw #3236 of
   `live_454107ED_2160.xtr`). INTERPOLATORS, the declarations and
   `point_sprite.gs.hlsl` are unchanged, so the VS/GS/PS signatures stay identical;
   COLOR0/1 and NORMAL0 go unused. A VS export >= 16 other than position now fails
   soft (SKIP) instead of an unchecked map lookup. Regenerate VS and PS TOGETHER after
   this change (no `XENOS_VSF_ONLY`/`XENOS_PS_ONLY`): an old PS with a new VS mislinks.
   Not done: a Xenia-strict PS (`rN` from `iTexCoordN` for every N below the register
   bound). XenosRecomp's param-gen register `(fieldC >> 8) & 0xFF` is 0 in every
   corpus PS, and SQ_CONTEXT_MISC.param_gen_pos is a runtime register, so the param-gen
   register cannot be told apart statically.

7. **Predicate counters and texture LOD** (iter-463,
   `0002-iter463-setp-and-texture-lod.patch`, applied after superman.patch; Xenia
   `shader_interpreter.cc`, `dxbc_shader_translator_fetch.cc`, BSD-3):
   - `setp_inv`: `ps = p0 ? 0 : (s == 0 ? 1 : s)`. Upstream left the counter at 1
     when the predicate was taken, so nested predicated blocks under the else
     branch were dead (Warworld ring PS 084F6366: 17 -> 20 DXIL sample ops).
   - `setp_{eq,ne,gt,ge}_push`: predicate from the `.w` components, result from
     `.x`, replicated. Latent: every corpus push op uses replicated swizzles.
   - `setTexLOD` is translated (it was dropped) into a `texLod` local.
     `UseComputedLOD=false` (or any VS fetch) -> `tfetch*Level` (SampleLevel);
     computed LOD with `UseRegisterLOD` or an instruction LOD bias ->
     `tfetch*Bias` (SampleBias); basemap mip filter -> level 0. The helpers are
     in `shader_common.h`. NOT DONE: the fetch-constant LOD bias (dword 4 bits
     12-21, /32) - no fetch constant reaches a pixel shader (b5 space5 is
     vertex-only), so that term is 0.
   - No silent drops: a tfetch with `UseRegisterGradients` (computed LOD), and
     `getCompTexLOD`, `getGradients`, `setGradientH/V`, are a named batch
     `SKIP <file> (...): <reason>` instead of a wrong-LOD Sample or a missing
     write (Xenia implements them - port `sample_d` from
     `dxbc_shader_translator_fetch.cc` when the census finds one). `getBCF`
     stores 0.0 to its result, as in Xenia (also unimplemented there). The
     recompile runs under an SEH `__try`, so the reason travels in
     `ShaderRecompiler::unsupported` and `main.cpp` throws it, not a C++ throw.
     None of these occurs in the 408-container stage_in corpus.
   - Check: `python tools/xenosrecomp/check_translator.py [--exe PATH]` (12 checks
     plus an informational census of refused fetches over data/shaders_carved and
     data/shaders_menu; the pre-fix exe fails all 12).
   - Install / rollback of the regenerated cache: see
     `archive/v1_arena/stage/translator/manifest.txt` (`dxil/` = the 35 changed PS,
     `dxil_pre463/` = the committed files they replace).

8. **Per-operand relative constant addressing** (step 4 lane C, 2026-10-18,
   `0004-step4-const-relative-per-operand.patch`; Xenia `ucode.h`
   `AluInstruction::src_const_is_addressed`, BSD-3): an ALU instruction's constant
   operands each take their own a0 / aL mode - operand 1 from `const_0_rel_abs`,
   operand 2 from `const_0_rel_abs` when operand 1 is a temp (else `const_1_rel_abs`),
   operand 3 and the scalar constant of mulsc / addsc / subsc from `const_0_rel_abs`
   when operands 1 and 2 are temps (else `const_1_rel_abs`); named constant arrays
   included. Item 3 indexed EVERY constant operand when either flag was set, so
   `c[105 + a0] * c254` read `c[254 + a0]` (0 past c255): every car after the first of
   an instanced car draw (VS 1CAF1468 / 7D53D379 / 86DCD90A / 92459524) drew with
   alpha ~0 on the gameplay traces. 28 cached programs carry an absolute constant
   beside a relative one (census `archive/step4_claude_20261018/render/cars/`
   relcensus). `ShaderRecompiler::perOperandConstRelative` (default true = this rule;
   the offline tool's value) - the game passes `SR_XLAT_CONST_REL` there for its
   runtime translation (`gfx/xenos_bridge.cpp`, `gfx/shader_runtime_xlat.cpp`) until
   that gate is baked.

9. **The Xenos alpha test, as a pixel-shader variant** (step 4 lane C, 2026-10-20,
   `0005-step4-alpha-test-variant.patch`; Xenia `DxbcShaderTranslator::CompletePixelShader`,
   `dxbc_shader_translator_om.cc`, BSD-3). Upstream's alpha test is a spec-constant
   `clip(oC0.w - g_AlphaThreshold)` that this DXIL build compiles out (`g_SpecConstants()` is 0), so no
   translated draw was ever alpha tested (the Warworld arena grate PS F6E8327B, RB_COLORCONTROL GREATER 0.502,
   covered the glowing lava under its slots). `ShaderRecompiler::alphaTestMode` translates a pixel shader
   that writes colour 0 a second time with `uint g_AlphaTest : packoffset(c17.y)` in the shared cbuffer and
   Xenia's test at every return: g_AlphaTest = 8 | RB_COLORCONTROL.alpha_func (0 = no test), ALWAYS skipped,
   NOT_EQUAL as `!=` (NaN passes), else the LESS / EQUAL / GREATER mask against g_AlphaThreshold (c17.x =
   RB_ALPHA_REF), exact compares (use_fuzzy_alpha_epsilon false), `discard` on a fail. `main.cpp` writes it as
   `<hash>.ps.at.dxil` beside the UNCHANGED plain translation (`XENOS_AT_ONLY=1`: only the variants;
   `AT-SKIP` + `.ps.at.fail.hlsl` when dxc refuses one); the game draws with it only when RB_COLORCONTROL
   enables the test (`gfx/alpha_test.hpp`, `SR_XLAT_ALPHA_TEST`). The 203 variants in data/shader_cache come
   from a container whose plain output reproduces the committed `.ps.dxil` byte for byte
   (`archive/step4_claude_20261018/render/r7/regen_at.py`); 6 garbage-container PS whose translation is
   nondeterministic under 0004 and 0005 alike have none (round 4: the game asks the runtime translator for a
   missing variant at the PS's IM_LOAD and drops - never draws untested - a draw the test applies to only when
   that fails too; `gfx/alpha_test.hpp` use()).
   **Round 4, `0006-step4-alpha-test-strict-compare.patch`** (Astra on 0005, P2): DXC marks every float compare
   `fast` (`fcmp fast une / olt / oeq / ogt` in the shipped variants), and fast-math may assume no NaN, so `!=`
   alone did not guarantee Xenia's unordered NOT_EQUAL. The epilogue now compares in INTEGER arithmetic on the
   `asuint` bits: NaN on either side is unordered (NOT_EQUAL passes, LESS / EQUAL / GREATER fail), a denormal
   magnitude is flushed to 0 first (DXBC float compares flush their inputs, and Xenia's epilogue is DXBC), and the
   rest compare as sign-magnitude keys (-0 == +0, +-inf ordered). No float compare is left on the reference:
   `tools/difftest/alpha_test_dxil_check.py` disassembles every shipped `.ps.at.dxil` and fails on one (the 0005
   files are its mutant); `gfx/alpha_test.hpp` passesBits() is the transcription the unit test holds against
   Xenia's DXBC sequence. The plain translation is unchanged (only the `alphaTestMode` epilogue differs).

## The corpus pipeline

```
python tools/extract_shader_containers.py --census <ucode_dump dir>
# golden dump -> data/shaders_carved/*.xpu (+ manifest.json: sha1(ucode) -> file)
# menu corpus: data/shaders_menu/ = the 109 manifest containers
XenosRecomp.exe data/shaders_menu data/shader_cache/shader_cache.cpp XenosRecomp/shader_common.h
```

Key facts (verified 2026-07-17, full contracts in ENGINE_MAP §6d):
- Containers = D3D9 Xbox360 compiled-shader blobs, magic `0x102A11xx` BE at +0
  (bit0: 1=VS 0=PS); ~400 unique-by-ucode live in the golden dump (bundle at
  phys 0x40000000, heap objects 0xB7-class VA aliases, XEX image).
- The golden trace holds RAW UCODE only (GPU never reads the container's
  virtual part) — containers must come from guest memory or the create hooks.
- **Every menu VS is vfetch-PATCHED at bind time** (fetch slot/offset/stride
  operands rewritten per vertex decl; PS never patched). Trace census: 110
  uniques = 54 VS + 56 PS; 56 PS match containers byte-exact, every VS needed a
  synthesized container (template container + patched trace ucode spliced in,
  `*_synth.xpu`). Runtime cache MUST key on the patched IM_LOAD bytes.
- iter-359b: residue ELIMINATED — 109/109 compile (Normal0 interpolator,
  Normal1 vk::location, Cube operand decode, empty-swizzle guard, literal-less
  cN -> g_UnnamedConstants). Only the 60-byte XEX built-in VS has no container.
- iter-359c: runtime contract additions — per-shader dxil files + .vfetch
  sidecars keyed by XXH3-64(raw BE ucode); ALL inputs plain float4 (host
  CPU-decodes; this game packs normals as 2_10_10_10, not R11G11B10); plain
  vs_6_0/ps_6_0 (no lib_6_3 spec-const linking); g_NdcScaleOffset at c18
  (VTE-off pixel-space remap). Host side: gfx/shader_cache + d3d12_backend
  translated pipeline (SR_XLAT_SHADERS, default ON).

## Missing vertex shaders (iter-463)

A VS the cache lacks drops every draw that uses it (`d3d12_backend` gate 1,
`[XLAT_REJECT] gate=1`); the Warworld arena lost 16 VS this way. They are usually a
known program bound with a new vertex layout: the XDK rewrites the vfetch
format/stride/offset at bind time, and the cache key is the PATCHED ucode.

- `python tools/stage_missing_vs.py --inputs <dir of <hash>.vs.bin> --out <stage dir>`
  translates the RUNTIME ucode (from `SR_SHADER_DUMP_MISSES=1` or
  `xtr_draws.py --dump-ucode`). The container metadata comes from a template that is
  proved to run the same program (CF, ALU and texture-fetch dwords identical; only the
  bind-time vfetch fields differ; one metadata across every such container; a vertex
  element for every fetch), then checks the output: the .vs.vfetch sidecar equals the
  rows derived from the runtime ucode, the .vs.vsf.dxil multiplies the index by the
  runtime strides, the input signature and HLSL match a same-program reference except
  for vertex-fetch lines. Writes `manifest.txt/json` and `corpus.txt` (the container dirs
  searched, with counts; pass it back with `--corpus-file` to re-run a staging exactly);
  never writes the installed cache.
- `[SHADER_MISS]` report (`SR_SHADER_MISS_REPORT`, default ON; `=0` = the old path, only
  the capped `[XLAT_REJECT]` line): the first draw a missing VS drops logs one line naming
  the VS and the PS of that draw; every 2 s while drops continue, one summary counts the
  draws dropped since the previous summary (across drains) and the top offenders.
- Twin naming is live (iter-463 runtime translation wired `ShaderCache::noteVsMissUcode` into
  `gfx/pm4_engine.cpp`'s VS miss branch). `<hash>.vs.mkey` (masked key: each vfetch's stride,
  offset, format, signed and integer bits zeroed; `tools/shader_mkey.py`, `gfx/shader_mkey.hpp`)
  lets the report name a cached VS running the same program with another layout; runtime
  translations write their own `.vs.mkey`. For the installed cache generate them with
  `python tools/shader_mkey.py sidecars --containers archive/v1_linkfix/stage_in --ucode
  archive/rootsweep_20260803/dirs/shader_misses --only-cached data/shader_cache/dxil --out <dir>`
  (199 keys; DA4E4EFE has no exec).
- "No cached VS with the same program AND the same fetch swizzles/mini/prefetch" is the
  strict mkey's verdict: it keeps dst_swiz, the mini bit and prefetch, which
  `stage_missing_vs.py` treats as patch fields, so the staging tool may still prove a
  same-program template (it does for 13 of the 16 arena misses).
- A twin's DXIL is NOT reused: the `.vs.vsf.dxil` the PM4 path draws with bakes each
  vfetch's stride, offset and format as literals (FAF94BDB multiplies the vertex index by
  32, its stride-9 twin DBE06113 by 36), so a miss still needs its own translation.
- Unit: `python tools/difftest/shader_mkey_fixture.py ...` then
  `python tools/difftest/shader_mkey_test_run.py --fixture <dir> --work <scratch>`
  (30 checks + 11 mutants; T7 reads the `[SHADER_MISS]` lines back from the log's tail
  ring).

### Closing the AOT gap (render/shader-coverage, 2026-10-17)

The runtime translator is the safety net. The AOT cache should hold every shader a route binds,
so that a release run translates nothing. The loop:

1. Capture: run with `SR_SHADER_DUMP_MISSES=1 SR_SHADER_DUMP_MISSES_DIR=<dir>`. Each miss's raw
   bind-time ucode lands as `<hash>.vs.bin` / `.ps.bin`.
2. Census: `python tools/shader_miss_census.py <run.log>...` names every runtime translation,
   wait, drop and release stop (`--strict` = fail on any runtime translation).
3. Stage: `python tools/stage_missing_vs.py --inputs <dir> --out <stage> --corpus data/shaders_carved
   data/shaders_menu archive/v1_linkfix/stage_in archive/fmv_p1/disc_shaders` (dev's offline
   exe). It never writes the installed cache.
4. Install: copy the stage's `dxil/<hash>.vs.{dxil,vfetch,vsf.dxil}` for each STAGED-OK hash
   into `data/shader_cache/dxil`. The `.mkey` is not a classified release file.

Since 2026-10-17 the same-program proof follows the bind-time patch model (ported from
release/installer), in this tool and in `gfx/shader_xlat_core.cpp` alike:

- the vfetch fetch-constant index is a patch field;
- consecutive vfetch slots inside one exec clause may be permuted, but only when the order of
  every two fetches that depend on each other is kept (component-level RAW / WAR / WAW; relative
  addressing counts as dependent).

Most "no template" misses were disc `.AST` shaders the game binds in place: CreateShader never
sees them. `data/shader_cache/containers/` ships those 184 disc containers as the runtime
translator's template corpus.

## Runtime translation (iter-463, `SR_SHADER_RUNTIME_XLAT`, default ON)

A shader the cache lacks is translated IN THE GAME on its first IM_LOAD, the way Xenia
translates on first use. Since 2026-10-17 that IM_LOAD waits for the translation (the miss
policy, `SR_SHADER_RUNTIME_XLAT_WAIT`, default ON, budget `SR_SHADER_RUNTIME_XLAT_WAIT_MS` =
30000; `=0` = the old drop until admitted), so a draw is dropped (dev) or fatal (release) only
when the shader cannot be translated. Design, threads and proofs: `docs/RENDERER_PM4_HYBRID.md`
section 11 and 11.1. This section is the toolchain contract.

- **What is linked.** `cmake/xenos_xlat.cmake` builds the static library `xenos_xlat` from the
  prepared sources (`prepare.py`, above; byte for byte the clone):
  `XenosRecomp/shader_recompiler.cpp` unchanged (its `pch.h` force-included, C++17, the
  offline tool's definitions `_CRT_SECURE_NO_WARNINGS` + `XENOS_RECOMP_DXIL`, fmt header-only),
  plus `gfx/xenos_bridge.cpp` (the one call into `ShaderRecompiler`, same pch),
  `gfx/shader_xlat_core.cpp` (container proof + DXC) and a generated file holding the exact
  bytes of `XenosRecomp/shader_common.h` and a translator source id (`cmake/xenos_embed.cmake`).
  `dxc_compiler.cpp` is NOT linked: `gfx/shader_xlat_core.cpp` loads `dxcompiler.dll` with
  `LoadLibraryW` + `GetProcAddress("DxcCreateInstance")` (a missing DLL disables runtime
  translation instead of stopping the exe) and passes `dxc_compiler.cpp`'s argument strings
  verbatim (`-T vs_6_0|ps_6_0`, `-HV 2021`, `-all-resources-bound`, `-Wno-ignored-attributes`,
  `-Qstrip_reflect`, `-Qstrip_debug`). No SPIR-V: the offline tool also compiles SPIR-V and
  SKIPs a shader whose SPIR-V fails; the game never uses SPIR-V.
- **Sync rule.** The game build applies the patch series itself (`prepare.py`). A clone must be
  upstream `990d03b` + `superman.patch` +
  `0002-iter463-setp-and-texture-lod.patch` + `0003-iter464-resscale-pixel-position.patch` +
  `0004-step4-const-relative-per-operand.patch` + `0005-step4-alpha-test-variant.patch` +
  `0006-step4-alpha-test-strict-compare.patch`, nothing else:
  `python tools/xenosrecomp/verify_clone.py` (exit 0 = identical, line endings ignored). A
  translator change is a new patch in this directory, applied to the clone, then BOTH
  `build_xenos.bat` (offline exe) and the game rebuilt. The runtime cache is keyed by a
  translator id, so a change starts a fresh `xr-<id>/` directory instead of re-admitting DXIL
  made under the old rules (persisted entries are NOT re-proved at boot). The id is XXH3 of:
  SHA-256 over the clone's `shader_recompiler.{cpp,h}`, `shader.h`, `shader_code.h`,
  `constant_table.h`, `pch.h`, `shader_common.h`, `dxc_compiler.{cpp,h}` and `main.cpp` (the
  last three are mirrored by hand in the core) plus our `gfx/shader_xlat_core.{cpp,hpp}`,
  `gfx/xenos_bridge.{cpp,hpp}` and `gfx/shader_mkey.hpp` (CR stripped, so a CRLF checkout keys
  the same); the DXC DLL bytes; and `kRulesVersion` (an extra salt). Every one of those files
  is a CMake configure dependency, so editing one regenerates the embed.
- **DLLs.** `sr_link_xenos_xlat()` copies the official release's `bin/x64/dxcompiler.dll` and
  `dxil.dll` (`cmake/dxc_release.cmake` downloads `dxc_2024_07_31.zip` and checks the SHA-256 of the
  zip and of both DLLs) next to the exe: DXC v1.8.2407 (1.8 / 4662 commits / 416fab6b; sha1
  `1a31fe38...` and `9c168995...`), byte-identical to the clone's `thirdparty/dxc-bin/bin/x64`
  copies that `XenosRecomp.exe` loads. With them go `LICENSE-XenosRecomp.md`, `LICENSE-fmt.txt`,
  `LICENSE-DXC.txt`, `LICENSE-DXC-MS.txt` and `LICENSE-DXC-MIT.txt` (the release zip's
  `LICENSE-MIT.txt`). The game must load exactly these: the DLL bytes are part of the translator id,
  and the parity test below is run against them.
- **Fidelity proof.** `python tools/difftest/shader_xlat_test_run.py --work <scratch>` builds the
  translator exactly as the CMake module does and checks: the 16 iter-463 spliced containers +
  3 controls translate byte-identical to the installed files; the 16 runtime ucodes are PROVED
  (the `stage_missing_vs.py` rules, ported to `gfx/shader_xlat_core.cpp`) and come out
  byte-identical, with the same check values as the Python manifest; refusals; the game path
  (`ShaderCache` admission, WAIT/async, persistence, installed-wins, `=0`); and parity: every
  container of `data/shaders_*`, `archive/fmv_p1/disc_shaders` and `archive/v1_linkfix/stage_in`
  that the in-process preflight accepts is fed to the offline exe AND translated in-process, and
  every output must be byte-identical (2026-09-30: 380/380 outputs identical, 71 SKIPs on both
  sides). Then the mutants, each of which must fail. Counts of the latest run are in
  RENDERER_PM4_HYBRID.md §11.
- **Preflight.** In the game an access violation is fatal (harness VEH), and the release-build
  recompiler has no asserts. `recompilerPreflight` replays what `recompile()` will read and
  refuses a container whose exec names an instruction past the ucode, whose written vertex fetch
  has no vertex element (`shader_recompiler.cpp:226`; a pixel shader has none), whose element
  usage indexes past the usage tables, or whose control flow closes more blocks than it opens
  (XenosRecomp's `uint32_t indentation` wraps and it writes ~4 billion tabs: `07DE6F96...` left a
  4 GB `.fail.hlsl` in an offline run). On the parity corpora it refuses 104 of 555 containers,
  all carved garbage (ASCII text, data tables where the ucode should be) - including 4 "PS" whose
  garbage translations sit in the installed cache (`0222937C...`, `16D64CB6...`,
  `4A76819A...`, `E5A43DF0...`; never loaded by the game: the IM_LOAD structure gate rejects
  such payloads). Offline, `0142416A...` (a "PS" with a vertex fetch) crashed the recompiler
  under SEH and the SAME batch then failed four later, valid containers with "dxc DXIL compile
  failed" - they translate fine alone or after the preflight removed it.

### Runtime translation - licences

| Component | Licence | Obligation | Where |
|---|---|---|---|
| XenosRecomp (hedge-dev) | MIT | keep the notice | copied next to the exe as `LICENSE-XenosRecomp.md` |
| Our patches (`*.patch`) | ports of Xenia code, BSD-3 | keep the attribution | the patch comments + this README |
| fmt (header-only) | MIT-style (fmt licence) | keep the notice | copied next to the exe as `LICENSE-fmt.txt` |
| dxcompiler.dll 1.8.2407 | University of Illinois/NCSA (LLVM release licence) + third-party notices | reproduce the notices with binary redistribution | `LICENSE-DXC.txt` (this directory; copied next to the exe) |
| The v1.8.2407 release package | MIT (`LICENSE-MIT.txt` of `dxc_2024_07_31.zip`, Copyright (c) Microsoft Corporation) | keep the notice | `LICENSE-DXC-MIT.txt` (this directory, the zip's file byte for byte; copied next to the exe) |
| dxil.dll (Microsoft DXIL signer) | Microsoft Software License Terms, "Microsoft DirectX Shader Compiler" (`LICENSE-MS.txt` of the v1.8.2407 release zip `dxc_2024_07_31.zip`, whose `bin/x64/dxil.dll` and `dxcompiler.dll` are byte-identical to ours: sha1 `9c168995...`, `1a31fe38...`) | "Distributable Code": object code may be shipped inside an application that adds significant primary functionality, under terms that protect it and Microsoft at least as much, with an indemnity for the application; no use of Microsoft trademarks; no source-licence contamination. OPEN POINT: the terms refer to "the distributables file list in the software", and the zip holds no such list, so that `dxil.dll` counts as distributable is inferred (it is the binary-release component applications need for signed DXIL), not read from a list | `LICENSE-DXC-MS.txt` (this directory, copied from the release zip; copied next to the exe). Before a public build: add the flow-down terms to the end-user licence |

