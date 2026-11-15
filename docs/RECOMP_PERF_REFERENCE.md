# Recomp performance reference: how the full-speed recomps do it, and what we change

Research lane `refstudy` (iter-465, 2026-10-04). Static study only: no game runs and no project
builds. The machine-code facts below come from disassembling the main tree's existing guest object
(`build/guest/CMakeFiles/SupermanReturnsGuest.dir/ppc/ppc_recomp.10.cpp.obj`, built 2026-10-03
22:09) and from relocation counts over all 136 guest objects. The codegen experiments are scratch
compiles of the exact generated patterns with our clang (22.1.6, `-O3 -std=gnu++20`, the guest TU
flags), kept in the session scratchpad. **Every gain below is an estimate until a roll measures it.**

Starting point (docs/PERFORMANCE.md 2.3): gameplay 16.9 fps. The guest render frame takes ~58 ms
(94.7% recompiled guest code). The command processor (CP) is busy 44-49 ms per frame (ib 20-23,
swap 24-26), and the GPU takes 30-35 ms per frame at 2x internal resolution.

## 0. Bottom line

1. **The references' guest code accesses memory in 2-3 instructions. Ours takes 13-15, and every
   VMX load or store runs a ~127-instruction scalar emulation of `pshufb`.** UnleashedRecomp,
   MarathonRecomp and ReXGlue all compile the generated code with `base + addr` accesses and at
   least SSSE3 (`-march=sandybridge` / `x86-64-v2`). We compile at the SSE2 baseline, read the
   guest base from a guarded thread-local, and fold every address through `aliasGuestVA`.
   These are cheap fixes, and they are perf2's.
2. **Register localization is not what makes the references fast.** MarathonRecomp ships with
   all eight XenonRecomp flags OFF (`MarathonRecompLib/config/Marathon.toml:6-13`) and still runs
   Sonic '06 at full speed. What the three references share is plain memory macros, `-march` and
   LTO, plus native D3D (the XenonRecomp titles) or Xenia's GPU-side CP (ReXGlue).
3. **Ceilings.** Fixing the guest alone cannot reach 60 fps. Once the guest frame falls below the
   CP's 44-49 ms, the CP limits us to about 20-22 fps, and the GPU (30-35 ms) is the next wall.
   Only the native lane removes both: no PM4 interpretation and no EDRAM emulation. Until then,
   the PM4 lane needs Xenia's CP techniques (section 5).

## 1. Ranked changes (expected fps gain per unit of effort)

| # | Change | Lane | Effort | Expected gain (estimate) |
|---|---|---|---|---|
| 1 | `-mssse3` on the guest lib (native `pshufb`) | perf2 | one flag + rebuild + gates | 10-25% of the render frame |
| 2 | `constinit` on `g_guestBaseTls` (or `-fno-ms-tls-guards`) | perf2 | one keyword, 8-min guest rebuild | 8-15% of guest time |
| 3 | Hoist the `physAliasA` function-static out of `aliasGuestVA` | perf2 | a few lines | 2-6% |
| 4 | Compile out the store watches and trace checks in the player build | perf2 | build option | 1-4% |
| 5 | Native CRT leaves (memcpy/memmove/memset/XMemCpy/XMemSet) and the D3D constant-copy leaf `sub_820F4920` | perf2 (CRT), native (D3D leaf) | days, difftest per leaf | up to ~10% before #1, ~3-5% after |
| 6 | Use the `base` argument instead of TLS in the memory macros (the reference form) | perf2 | macro change, full guest rebuild | 2-5% on top of #2 |
| 7 | ThinLTO across the guest lib + `-fno-strict-aliasing` | perf2 | CMake; RAM-heavy link | 3-10% |
| 8 | Render-thread priority / keep spinners off its SMT sibling | perf2 (platform) | experiment | 0-15%, unknown |
| 9 | Xenia physical views (one physical section, 0xA/0xC/0xE views) | physmem | weeks (allocator surgery) | small perf (#3's final form); main value is correctness (`[PHYS_CAP]`) |
| 10 | Native D3D: hook the XDK device, read shadows and dirty masks at draw, no PM4 | native | N1-N7 | removes ~30% of the render thread, the whole CP (44-49 ms) and the EDRAM GPU cost; the only path past ~22 fps |

**Status 2026-10-04:** perf2 has built items 1-4 on `perf/perf2` (worktree `sr-wt-perf`). They
are not merged and the gameplay rolls are pending:
- b1676ea1: watches compiled out (`SR_STORE_WATCHPOINTS`, default OFF).
- 47153f12: `constinit` base, plus a per-entry base local.
- f8cffec6: the aliasGuestVA slow path moved out of line, with a namespace bool.
- d13eb0c1: `-mssse3` (`SR_GUEST_ISA`).

The exe shrinks from 454 to 152 MB. L6 54/0, golden_call byte-identical, and the 48 suites match
dev. Items 5-6 are not started.

Items 1-7 compound. A guest frame of roughly 30-40 ms is plausible, but the CP ceiling then limits
fps to the low 20s (item 10, section 5).

## 2. The items in detail

### 2.1 Native SSSE3 for VMX byte shuffles (perf2, rank 1)

- **Ours.** The guest TUs compile at the x86-64 SSE2 baseline (`-O3 -DNDEBUG -std=gnu++20`, no
  `-march`; `build/compile_commands.json`). Every `lvx`/`stvx`/`lvlx`/`lvrx` is generated as
  `simde_mm_shuffle_epi8(load, VectorMaskL)` (e.g. `guest/ppc/ppc_recomp.10.cpp`, every
  `lvx128`). Without `__SSSE3__`, simde uses its portable loop
  (`tools/XenonRecomp/thirdparty/simde/x86/ssse3.h`, `simde_mm_shuffle_epi8` `#else` branch).
  `VectorMaskL` is a mutable `inline uint8_t[]` (`guest/ppc/ppc_context.h:768`), so clang cannot
  constant-fold the mask.
- **Measured (static).** The real object `ppc_recomp.10.cpp.obj` contains **0 `pshufb`** and
  25,655 byte loads from the stack, about 15.7 per shuffle site (1,638 sites in that TU). There are
  **42,290 shuffle sites game-wide**. The scratch compile of the `lvx128` pattern produces
  **127 instructions (it spills xmm6-15) at the SSE2 baseline, against 5 with `-march=x86-64-v2`.**
- **Why it is hot.** The vertex-constant upload `__imp__sub_820F4920` (7-9% of the render
  frame) is an unrolled `lvlx`/`lvrx`/`vor`/`stvx` copy loop with 30 shuffles in its body, so each
  16-byte constant costs about 3 x 127 instructions. The other top functions are VMX too
  (GpuEmitType0RegPacketsVec 10.5%, the pixel constants 5.3%, MathVmxDotMinReduce 3.1%,
  XMemCpy_Vmx128 1.6%, Havok and cloth).
- **Reference.** UnleashedRecomp `CMakeLists.txt:36-40` (`-march=sandybridge` for all projects,
  i.e. SSE4.2 + AVX, no FMA); MarathonRecomp `CMakeLists.txt:39` (the same); ReXGlue
  `CMakePresets.json:17-18` (`x86-64-v2`; the v3 preset is at :63-64).
- **Risk.** `pshufb` is an integer byte permute, so the native and emulated results are
  bit-identical by definition. SSSE3 adds no FMA, so there is no contraction. The 2026-10-03
  `x86-64-v3` rejection (docs/PERFORMANCE_CODEGEN.md) was about AVX2/FMA-era float lowering
  diverging, which does not apply here.
  The golden_call timings in that test showed no win. They are probably dominated by per-case
  harness setup (ig_s takes ~9 ms per case): that is an inference, so judge this change with a
  gameplay roll plus `host_profile.py`, not golden_call.
  Hardware floor: SSSE3 means Core 2 / Bulldozer or newer, below what every reference requires.
- **Step 2 (separate A/B).** `-msse4.1` turns 2,127 `simde_mm_dp_ps` sites into `dpps`. Its
  summation order and rounding can differ from simde's emulation, so it needs L6, golden_call
  and the movie sheet first.

### 2.2 The thread-local guest base carries an MSVC TLS guard on every access (perf2, rank 2)

- **Ours.** `PPC_GUEST_BASE()` is `sr::kernel::g_guestBaseTls`
  (`guest/ppc/ppc_context.h:81`), declared `extern thread_local uint8_t* g_guestBaseTls`
  (`kernel/guest_context.hpp:21`) and defined `= nullptr` (`kernel/guest_context.cpp:149`).
  For an extern `thread_local`, clang's MSVC ABI emits an on-demand-init guard
  (`__tls_guard` check + `__dyn_tls_on_demand_init`) at each use.
- **Measured (static).** Across the 136 guest objects there are **604,839 `__tls_guard` checks and
  1,181,098 `_tls_index` loads**. A typical store in `__imp__sub_82176358` runs:
  `cmpb g_storeWatchOn; je`, TLS index -> `gs:0x58` -> slot -> guard compare -> `jne`, the TLS
  sequence again to load the base, `cmp $0xA0000000; jae`, then the store. That is **13-15
  instructions with 3-5 dependent loads, against 2 in the reference** (`mov eax, ebx;
  mov [rdi+rax], ebp`).
- **Fix.** Add `constinit` to the declaration and definition (the variable is
  constant-initialized), or pass `-fno-ms-tls-guards` to the guest lib. Either one removes the
  guards completely (scratch compile: 0 guards in both variants).
- **Risk.** Nil semantically. Editing `guest_context.hpp` costs an 8-minute guest rebuild.

### 2.3 aliasGuestVA's function-local static bloats every access site (perf2, rank 3)

- **Ours.** `kernel/guest_context.hpp:234-288`. The fast path (`va < 0xA0000000`, :277) is
  cheap. The function-local `static const bool physAliasA` (:281-284), however, compiles to an MSVC
  thread-safe static (`_Init_thread_header/_footer`, `getenv`), and clang inlines a copy of that
  init block into the cold path of **every** access. In large functions clang then stops inlining
  the whole fold: TU 10 has 31,504 relocations into `aliasGuestVA`, including 676 full
  out-of-line calls on the hot path (for example in `__imp__sub_821691A8`:
  `callq aliasGuestVA; mov (%rsi,%rax),%eax; bswap`).
- **Fix.** Make `physAliasA` a namespace-scope bool set at boot, like `g_physUnifyActive`. With
  the Xenia layout (2.8) the fold becomes ReXGlue's branchless form, two instructions:
  `va + (va >= 0xE0000000 ? 0x1000 : 0)` (`rexglue-sdk/resources/templates/codegen/pch_h.inja:126-130`;
  Xenia's JIT does the same: `src/xenia/cpu/backend/x64/x64_seq_memory.cc:201-215`).

### 2.4 Debug watches and trace checks (perf2, rank 4)

`PPC_STORE_U8/16/32/64` (`guest/ppc/ppc_context.h:121-250`) is already gated by one bool
(iter-461), but it costs a compare and branch per store (**257,726 `g_storeWatchOn` references**)
and a large cold block per site, which eats i-cache and the inliner's budget (see 2.3).
`PPC_FUNC_PROLOGUE` (`g_guestTraceOn`, :44-50) and `PPC_CALL_INDIRECT_FUNC` (`g_traceCallsOn`
plus a range check, :311-322) add one test each. The references define these macros as the bare
access (`reference/XenonRecomp/XenonUtils/ppc_context.h:32-114`). Fix: a compile-time switch
(e.g. `SR_GUEST_DEBUG_HOOKS`, ON for dev/difftest builds, OFF for the player build); A/B with
`tools/regen/relink_exe.py`.

### 2.5 Native CRT and copy leaves (perf2; the D3D leaf is native's)

- **References.** UnleashedRecomp `UnleashedRecomp/misc_impl.cpp:35-44` replaces seven
  memcpy variants, memmove and two memsets with host versions. ReXGlue ships a native CRT:
  `src/kernel/crt/memory.cpp:76-85` (memcpy, memmove, memset, memchr, XMemCpy, XMemSet,
  XMemSet128, memset_vmx, `_s` variants), `heap.cpp:361-364` (RtlAllocate/Free/Size/ReAllocateHeap)
  and `string.cpp:241-256`. A byte copy is endian-neutral, so a host memcpy is exact.
- **Ours.** `sub_82468290` (memcpy) still runs the guest body (`simd_memcpy_null_guard.cpp`,
  OVERRIDES.json "band-aid"). XMemCpy_Vmx128 (1.6%) and `sub_820F4920` (7-9%; a 16-byte-aligned
  copy into the command buffer plus a few header/pointer words) are guest code.
- **Fix.** Identify the CRT leaves (the profiler's top list plus a signature pass) and write
  native bodies. Prove each byte-identical with L6 difftest and register it as `platform-fix`.
  `sub_820F4920` belongs with the native lane's B1 work, but a byte-identical native body helps
  the PM4 path today.

### 2.6 Pass `base`, not TLS (perf2, rank 6)

All three references use the function argument `base`, which stays in a register:
XenonUtils `ppc_context.h:35-80`, ReXGlue `pch_h.inja:133-144`. Our own generated vector
accesses already use `base` (`ppc_recomp.10.cpp` `lvx128`), and only the scalar macros switched
to the global (the `ppc_context.h:75-80` comment: "base can be clobbered in the call chain"). That
was a historical bug class, so re-check it with L6 and a roll. Gain on top of 2.2: one load per
use, and the TLS address no longer has to be kept live.

### 2.7 ThinLTO and the faithfulness flags (perf2, rank 7)

- **References.** UnleashedRecomp `CMakePresets.json:58` and MarathonRecomp :58 set
  `CMAKE_INTERPROCEDURAL_OPTIMIZATION` in Release, which for clang is `-flto=thin` (CMake 4.2
  `Modules/Compiler/Clang.cmake:79`). Every reference also compiles the generated code with
  `-fno-strict-aliasing` and strict FP: UnleashedRecomp `UnleashedRecompLib/CMakeLists.txt:3-11`,
  ReXGlue `CMakeLists.txt:133-137` (`-O3`). **None uses PGO** (no `-fprofile*` in any of the
  three).
- **Ours.** No LTO, default strict aliasing (UB exposure grows under LTO), default FP. Without
  FMA, `-ffp-contract=on` fuses nothing, so the FP flag only matters if a later `-march` adds
  FMA.
- **Risk.** RAM: the machine is RAM-starved (memory note) and ThinLTO over 136 large TUs needs
  `-flto-jobs` limits. The UB exposure is why `-fno-strict-aliasing` should land first.

### 2.8 Physical memory: what Xenia does and what UnleashedRecomp does (physmem)

- **Xenia** (`src/xenia/memory.cc`; ReXGlue `src/system/xmemory.cpp` is the same code):
  - **Backing.** A pagefile-backed mapping of `0x11FFFFFFF` bytes with `SEC_RESERVE` (:194;
    `base/memory_win.cc:212-222`), committed per allocation.
  - **Placement.** Views are mapped at `1<<n` for n = 32..63 until all fit (:207).
  - **The views** (`map_info`, :311-366):
    - 0x00-0x7F virtual: file offset = VA.
    - **0x80000000 and 0x90000000 (XEX) both map file offset 0x80000000**, which is VIRTUAL and
      not physical.
    - **0xA0000000, 0xC0000000 and 0xE0000000 all map file offset 0x100000000**, the one 512 MB
      physical region (0xE at 0x100001000, rounded down to the 64 KB granularity; the 4 KB is
      added per access, see 2.3).
    - 0x100000000 is the raw physical view the GPU uses.
  - Views are mapped with `MapViewOfFileEx` / `MapViewOfFile3FromApp` (`memory_win.cc:233-275`).
- **Our attempt.** `SR_PHYS_UNIFY` (`guest/guest_harness.cpp:555-`) also mapped **0x8 onto the
  physical section** (docs/MEMORY_MODEL_PLAN.md 5b), which Xenia does not do. That deviation
  produced the 0x8E/0xAE collision that made the A/B negative. The Xenia layout keeps 0x8
  virtual. The guest "mixing 0x8/0xA VAs for the same bytes" is then the symptom to fix in the
  physical allocator and `MmGetPhysicalAddress` (`va & 0x1FFFFFFF`), not something to paper over
  in the mapping.
- **UnleashedRecomp** (`kernel/memory.cpp:7`, `kernel/heap.cpp:11-12,103,125-130`,
  `kernel/imports.cpp:839-843`):
  - One flat 4 GB commit at 0x100000000, no views.
  - The game's own heap functions and `XAllocMem` are hooked onto o1heap.
  - Physical allocations come from a 1.5 GB o1heap at 0xA0000000-0xFFFFFFFF.
  - `MmGetPhysicalAddress` returns its input.
  This only works because nothing interprets GPU physical addresses (native D3D). For us it is
  the post-native end state, not a fix for the PM4/oracle era.

### 2.9 Native D3D: how UnleashedRecomp avoids building PM4 (native)

- **Hooks.** About 46 XDK D3D functions are replaced and about 15 stubbed
  (`UnleashedRecomp/gpu/video.cpp:7798-7882`): CreateDevice, the Create/Lock/Unlock calls for
  textures and buffers, Set{RenderTarget, DepthStencilSurface, Viewport, Texture, ScissorRect,
  StreamSource, Indices, VertexDeclaration, Vertex/PixelShader}, Draw{Primitive, IndexedPrimitive,
  PrimitiveUP}, Clear, StretchRect (resolve) and Present. MarathonRecomp hooks 85, including a
  per-state `SetRenderState<D3DRS_*>` hook.
- **Inlined setters are left alone.** They write the guest device's shadow (constants, sampler
  words, bool constants) and its dirty masks.
  - Each Draw hook reads the dirty masks and copies only the dirty constant range
    (`FlushRenderStateForMainThread`, `video.cpp:4297-4357`; `dirtyFlags[0]`/`[1]` = VS/PS float
    constants via countl_zero/countr_zero, `[3]` samplers, `[4]` booleans).
  - The device's render-state function table is filled with host thunks (`video.cpp:2123-2136`).
  - The XDK's per-draw flush, our GpuEmitType0RegPacketsVec plus the constant uploads (~30% of
    our render thread), therefore never runs, and the CP has nothing to interpret.
- **Threading and pacing.**
  - The game thread enqueues `RenderCommand`s into a moodycamel `BlockingConcurrentQueue`
    (`video.cpp:1006`). A render thread at `THREAD_PRIORITY_ABOVE_NORMAL` drains them in bulk
    (:5248-5260).
  - Pipelines compile asynchronously (:6245).
  - The frame limiter sleeps to 2 ms before the target and then yield-spins (:2858-2877;
    MarathonRecomp :3294).
  - The high frame rate there is a variable-dt simulation plus dt fixes
    (`patches/fps_patches.cpp`). It does not transfer: our simulation is fixed at 30 Hz, so we use
    interpolation (docs/HIGH_FRAMERATE.md).
- **This matches our plan.** docs/NATIVE_ROADMAP.md 3.2 B1 already chose "read shadows and dirty
  masks at draw time". The references add one thing: the Draw hook is the only consumer, so B1
  costs nothing per setter.

## 3. ReXGlue: frame rate while keeping Xenia's command processor

ReXGlue titles get their speed from two things:

1. **The AOT guest code is lean.** Accesses are `base + addr + (addr >= 0xE0000000 ? 0x1000 :
   0)` (`pch_h.inja:126-144`). MMIO is detected statically: an access followed by `eieio`, or one
   whose base register is a known MMIO base, uses the checked `REX_MM_*` macro
   (`src/codegen/builders/builder_context.h:195-213`, `pch_h.inja:155-205`). Every other access
   has no runtime check.
2. **Xenia's GPU runs on the GPU.** EDRAM and resolves are GPU-side, and shared memory is
   uploaded by **invalidated page ranges**: page watches through the physical views
   (`src/xenia/gpu/shared_memory.cc:51,238,374`, `memory.cc:675`), not validated per draw.

Our CP spends 10% on `pm4ShmValidate`, 25% on `xlatBindBatchTextures` and 24-26 ms per frame on
swap (PERFORMANCE.md 2.3, 6). We already use `MEM_WRITE_WATCH` for invalidation
(`guest/guest_harness.cpp:702-705`), which is compatible with our fatal-AV VEH. Xenia's
page-protection watches are not. While the PM4 path draws, these are the CP's levers; native
removes them.

## 4. How to measure (no estimate above is a result yet)

- **Compile-flag changes (1, 2, 3, 4, 6, 7).** Build an identical-host A/B exe with
  `tools/regen/relink_exe.py`. Run the gameplay scenario (`perf_run.py --scenario gameplay`,
  PERFORMANCE.md 2), `era_fps.py` and `host_profile.py`, and check that the shares of the top
  functions in 2.1 move.
- **Correctness gates** (PERFORMANCE_CODEGEN.md): L6 `compare.py --all`, golden_call (ig_s may
  differ only in the w-lane class), the 48 suites, replay `replay_ab.ps1`, and one roll with the
  movie contact sheet.
- **CRT leaves.** Difftest each leaf byte-for-byte, then register it in `docs/OVERRIDES.json`
  and run `gen_override_manifest.py`.
- **Ceiling check after perf2.** Read the CP busy ms/frame and the GPU ms/frame. If the CP
  exceeds the guest frame, further guest work is wasted until the CP or native moves.

## Credits

The short fragments quoted here come from ReXGlue SDK (BSD-3-Clause:
`REX_PHYS_HOST_OFFSET`), Xenia (BSD-3-Clause: the `map_info` layout) and XenonRecomp's XenonUtils
(MIT: the plain load/store macro). All three are already credited in `CREDITS.md`
(`credits:rexglue`, `credits:xenia`, `credits:xenonrecomp`). UnleashedRecomp and MarathonRecomp
are GPL-3.0, so they are described by file:line only and no code is copied. MarathonRecomp is
`reference/MarathonRecomp`, a shallow clone of `sonicnext-dev/MarathonRecomp` for reading only.
The shallow clones `reference/{XenonRecomp,MarathonRecomp,rexglue-sdk}` are excluded locally
through `.git/info/exclude`.
