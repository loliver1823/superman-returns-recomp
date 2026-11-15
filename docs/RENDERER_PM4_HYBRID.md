# RENDERER: PM4-backbone hybrid (the main track from 2026-09-29)

**USER DECISION 2026-09-29.** The long-term goal is a Superman framework that mods and
custom content build on, so we own the whole rendering stack. The PM4 command stream is
what draws; the D3D9-HLE layer is kept on top of it as the observe / label / mod layer.
Supersedes the HLE-only rebuild in `docs/MIGRATION_API_HLE.md` (kept for its hook
contracts and history).

**2026-10-01: the native conversion plan (this PM4 path becomes the test oracle; P4 moves after N7) is [`NATIVE_ROADMAP.md`](NATIVE_ROADMAP.md).**

## 1. The two layers

| Layer | Code | Job |
|---|---|---|
| **PM4 backbone** (draws) | `gfx/pm4_engine.*`, `gfx/xenos_state.hpp`, `gfx/xenos_ucode.*`, `gfx/xenos_tiling.hpp`, the backend's PM4/xlat path in `gfx/d3d12_backend.cpp`, `gfx/gpu_trace_replay.*` (fixture), `gfx/null_renderer.cpp` (live instance) | Execute the game's command stream: registers, shaders, fetch constants, draws, EDRAM + resolves, swap. It is the complete record of what the game told the GPU. |
| **HLE layer** (observes) | `gfx/hle_video.*`, `kernel/hle_api_observe.cpp` | Name what the PM4 layer draws: texture identity (glyph atlas, movie planes), draw/resource labels. Later: mod hooks and remaster features (ultrawide). Resolution scaling landed in the PM4 layer instead (section 12). It replaces a draw only through a named, gated feature. |

**Why not HLE-only:** it needs every XDK entry point intercepted, and many are inlined
into game code (MIGRATION_API_HLE "HLE contract gotchas"). The open HLE defects - guest
ClearF never executed, 526/1118 draws fetching never-written cube constants, refused
resolves - fit that missed-intercept class. **Why not ReXGlue (Xenia's GPU wholesale):**
churning API and an unclear licence are the wrong foundation for a framework other
people build on. **Precedent:** Dead Rising 2 Case Zero (PM4 executor plus a D3D-line arm)
is the one non-ReXGlue recomp in `docs/REFERENCE_RECOMP_AUDIT_2026-09-11.md` that claims
start-to-finish playable.

## 2. References (deviation is the bug)

- **PM4 / Xenos semantics:** `reference/xenia-canary/src/xenia/gpu/` - command processor,
  shader translator (DXBC/SPIR-V), `texture_util`/`texture_address`, render-target and
  resolve (EDRAM) logic. When our PM4 layer deviates from Xenia, the deviation is the bug
  until proven otherwise. Port with attribution (Xenia is BSD-3).
- **D3D12 host structure and HLE hook surface:** UnleashedRecomp `gpu/video.cpp` (PSO
  cache, present, render thread, API hook shapes).

## 3. What exists today (2026-09-29)

- The live game already runs `Pm4Engine` on its real command stream
  (`gfx/null_renderer.cpp` `livePm4()`), with `submitDraws = false` since iter-441
  Stage A: it services fences and interrupts only. Draws reach the backend only from the
  replay fixture.
- Replay fixture: `tools/replay_ab.ps1 -Tag <t> -Trace <xtr>` (TOOLKIT 1.6), deterministic
  (TOOLKIT 1.6b), with golden traces + Xenia screenshots in `data/gpu_traces/`.
- The backend comment at `gfx/d3d12_backend.hpp` (HleBatch) still calls the xlat path
  "slated for removal" - that is now wrong; update it when the path is next touched.

## 4. Plan

| Phase | Work | Done when |
|---|---|---|
| **P0** | Baseline: replay every golden trace through the PM4 path, score against Xenia's own frame. | Table in section 5 filled, frames looked at. |
| **P1** | Replay conformance: close the gaps P0 shows, porting Xenia's logic where ours differs. | Golden frames match Xenia's to the eye, with meanAbsDiff and distinct-colour counts reported together (TOOLKIT 1.6d). |
| **P2** | Live switch behind a gate: live `Pm4Engine` submits draws, HLE draw submission off, HLE observers still run. | Boot runs show the same eras replay showed, checked on whole-run GPU-readback frames against Xenia. |
| **P3** | Bake P2 default-ON; retire the HLE draw path; HLE hooks become labels. | NATURAL_BASELINE entry. |
| **P4** | Mod and enhancement surfaces (hash-keyed texture packs, file shader swaps, res scaling). | Only after the game is golden (memory: modding requirement). |

Experiments at every phase are free (CLAUDE.md "How we work"). A phase is done when its
"Done when" column holds against the Xenia oracle; visual milestones get one blind Astra
second opinion before they are recorded.

## 5. P0 baseline - replay vs Xenia golden (2026-09-29)

Exe `9883fe7f...` = HEAD `7b19e2c` plus the parked `kernel/hle_api_observe.cpp` idxstream
change (gated `SR_S6_IDXSTREAM`, default OFF, not set). No gates unless listed. Artifacts:
`archive/pm4_p0_20260929/` (side-by-sides `*_vs_golden.png`, logs, resolve dumps).
Golden PNGs are 1024x576 and were resized to 1280x720 for scoring.

| Trace | Parse | MAD | Our colours | Golden colours | Our non-black | Golden non-black | What it looks like |
|---|---|---|---|---|---|---|---|
| menu_pressstart (3501 draws) | CLEAN-EOF | 73.83 | 201,527 | 168,868 | 35.2% | 99.1% | top half black, bottom half green/magenta noise bands |
| fmv_eahd (151 draws) | CLEAN-EOF | 28.75 | 3 | 63,506 | 12.8% | 100% | one grey striped band across the middle; no logo |
| fmv_t16 | CLEAN-EOF | 14.50 | 3 | 1 | 12.8% | 0% | same band; the golden is an all-black frame |
| fmv_t21 | CLEAN-EOF | 6.94 | 3 | 1,064 | 12.8% | 48.8% | same band |
| fmv_t25 | CLEAN-EOF | 11.77 | 3 | 24,418 | 12.8% | 37.4% | same band |

The low FMV MADs are the TOOLKIT 1.6d trap (a mostly-black output scoring well against a
mostly-dark target); none of these frames is close.

**What works:** the parser (all five traces CLEAN-EOF) and shader translation - on the
menu, `pipeline census: xlat=3475 fixed=0`, zero fallback reasons, 26 resolves queued.

**What is broken, from `SR_RESOLVE_DUMP=1` (`resolvedump_menu_sheet.png`):** the passes
contain real scene content (sky, building edges) but each render target is cut into
quadrants, with scene content in one quarter-size region and white/black/yellow fill
elsewhere. Earlier gates change nothing useful: `SR_XLAT_ONLY=1` gives a histogram
identical to default; `SR_XLAT_RTT=1` gives 70.3% white (the known iter-359n state).

**Diagnosis on record, never implemented:** iter-359n (2026-07-18, memory
`project_render_pm4_engine`) found every pass sharing two host render targets
("surface conflation") and named the fix: a render-target cache keyed like Xenia's
`render_target_cache` {EDRAM base, pitch, MSAA, depth/colour, format}, with draws and
resolves routed to their own surface. The HLE pivot (07-30) came before it was built.
Not established: whether today's output is decay since July, or the July "clean
press-start UI" default was the trace's own restored framebuffer (the iter-359g false
positive class).

**P1 starts here:** port Xenia's render-target / EDRAM model
(`reference/xenia-canary/src/xenia/gpu/render_target_cache.{h,cc}`,
`d3d12/d3d12_render_target_cache.*`, `draw_util.cc` viewport + resolve math, MSAA sample
scaling) into the PM4 path, then re-run this table.

**Correction:** the P0 table above scored against 1024x576 Xenia *screenshots*, and the
FMV ones show other moments (`fmv_t16.png` is all black). Since 2026-09-29 the oracle is
Xenia's own `trace-dump` render of the same `.xtr` (`data/gpu_traces/*_oracle.png`,
deterministic), scored by `tools/pm4_replay_score.py`. Oracle baseline (gate off):
menu **101.65**, FMV eahd 15.77 / t16 37.99 / t21 6.80 / t25 11.76.

## 6. P1 progress (2026-09-29)

Gate: `SR_PM4_RTCACHE=1` (default OFF; gate-off control is unchanged, menu MAD 101.65).
Score: `python tools/pm4_replay_score.py --tag <t> --gates "SR_PM4_RTCACHE=1"`.

| Step | What | Menu MAD | Evidence |
|---|---|---|---|
| EDRAM model | `gfx/edram.hpp` (78/78 `difftest --edram-test`), per-key host RTs, ownership transfers, Xenia resolves into guest memory | 87.60 | `archive/pm4_score/rtc1` |
| clear-rect claim | clip-disabled screen-space draws claim EDRAM only to their lowest vertex | 63.90 | `rtc2` |
| draw-time drain + attribute width | the replay translates each draw right after its packet (the UI vertex ring is rewritten later in the stream); the fetch bound uses the format's own width (a flat 16 zeroed every UI quad's 6th vertex) | 60.65 | `rtc3`: shield, title, legal text match |
| primitive reset + fans | strips honour `VGT_MULTI_PRIM_IB_RESET_INDX`; fans expand to lists | - | sky renders |
| **in-shader vertex fetch** | XenosRecomp `fetchMode` emits `<hash>.vs.vsf.dxil`: vfetch reads staged guest memory at the index the shader computed (Xenia's model). Superman's world VSes fetch their own index from slot 94 and derive the instance from r0.x - no CPU pre-decode can follow that. `SR_PM4_VSF=0` = A/B | 60.48 | scene depth resolve now **exact** vs Xenia (below) |

**Per-resolve oracle (the tool that found the vertex defect).** Xenia's trace-dump, patched
(`tools/xenia-trace/xenia_resolve_dump.patch`), writes every resolve's destination bytes;
our replay diffs each of its resolves against them:

```
xenia-gpu-d3d12-trace-dump.exe --async_shader_compilation=false --readback_resolve=full \
  --trace_dump_resolve_dir=<dir> --trace_dump_path=<dir> --target_trace_file=<xtr>
tools/replay_ab.ps1 ... -Gates "SR_PM4_RTCACHE=1,SR_PM4_ORACLE=<dir>,SR_PM4_ORACLE_DIR=<bmp dir>"
```

`[PM4_ORACLE] #n` lines give differ% / per-channel MAD / means for ours and Xenia's; the
BMP pairs show both. Menu frame, 26 resolves, one-to-one with Xenia's: scene depth (#6)
MAD 0.0566 -> **0.0000** and the half-res float target (#8) 60% -> 1.5% differing after
in-shader fetch.

**Per-draw EDRAM oracle** (`tools/xenia-trace/README.md`): Xenia and our replay dump the
whole EDRAM after chosen draw ordinals; `difftest --edram-surface` decodes one surface from
both. It found the shadow chain's four defects in order:

1. the depth clear (draw 25) crafts depth 0x7FFF7F + **stencil 0xFF** so the aliased
   k_16_16 shadow map reads (+32,+32), the MIN-blend floor. We kept no stencil and our
   D32 -> unorm24 conversion was one ulp off. Fix: Xenia's host depth formats
   (D24S8 -> `D24_UNORM_S8_UINT`, D24FS8 -> `D32_FLOAT_S8X24_UINT`), both planes through
   store/load, stencil state from RB_DEPTHCONTROL / RB_STENCILREFMASK. Draw 25 now matches
   bit-exactly.
2. the Poisson shadow filter uses **3D-or-stacked** tfetches: XenosRecomp read the 3D
   table (a white placeholder) and dropped the texel offsets.
3. the shadow map is **one stacked 640x640x4 array** (cascade = layer): new
   `getOrCreateTexture2DArray` + a Texture2DArray table (t0 space6).
4. the layer index is **floored** on Xenos (D3D12 rounds to nearest even - Xenia
   dxbc_shader_translator_fetch.cc:1853).

After them, resolves #0-#6 and #8 match Xenia (MAD <= 0.0024).

The next pass (per-draw bisect of the HDR surface, then of the 640x360 bloom source) found
four more, in chain order:

5. **The PS constant file was cut at c223.** The capture (`aluPs`, 896 dwords) and the
   upload (3584 bytes) followed UnleashedRecomp's 224-register PS file; Xenos has 256, and
   this game binds named PS constants above c223 (the building shader's `KaS` at c229). The
   root CBV read past the file into the shared descriptor-index block. Now 1024 dwords /
   4096 bytes, and XenosRecomp's PS arrays/bounds are 256. HDR scene #7 MAD 0.167 -> 0.043.
6. **Fetch-constant swizzle and _EXPAND** (PM4 path): SRVs now carry Xenia's
   GuestToHostSwizzle (dword_3 bits 1-12) over the host-format column (every
   D3DFMT_A8R8G8B8 texture is ZYXW, i.e. we were R/B-swapped; the k_8 glyph atlas is 000R -
   PRESS START is text again), and k_16*_EXPAND samples as FLOAT (Xenia GetBaseFormat).
7. **Real volumes and cubes** (fetch dimension 3D / cube): `getOrCreateTexture3D` (Tiled3D,
   size_3d) and `getOrCreateTexture2DArray(cube)` (six faces at the slice stride, TEXTURECUBE
   SRV), reached through t0 space7 / space8 tables over the 2D band (root params 12/13;
   `tfetch3D` bit 29, `tfetchCube` bit 31).
   **2026-10-18 (step-4 lane C, baked):** a volume's sampler honours clamp_z (`SR_XLAT_SAMPLER_W`:
   sampler-table bit 3 = AddressW WRAP for a k3D fetch whose clamp_z repeats; every sampler had W
   CLAMP, so the 64^3 noise volumes behind the lit windows, pavement and car paint read their edge
   slices) and volumes carry their whole mip chain (`SR_TEX3D_MIPS`, `gfx/volume_layout.hpp`: Xenia's
   k3D layout, base read to its data extent). Replay vs Xenia: gp1 4.45 -> 0.49, gp2 2.83 -> 0.39,
   menu 2.10 -> 1.11, lg1 5.46 -> 0.34, lg2 4.61 -> 0.67; the lit-window set now matches Xenia's
   window for window (`_worktrees/s4-render/archive/step4_claude_20261018/render/NOTES.md`).
   **Round 2 (2026-10-18, baked):** Astra's six P2 on that bake are fixed - capture / unlock spans of a
   volume from `volume_layout.hpp` (`SR_TEX3D_V2`; a 1x1x64 volume with mip_max 6 had no mip span), the
   endian swap on guest words incl. sub-dword tails, a volume whose level count changes is recreated;
   a k3D fetch samples through a full Xenia sampler in table slots 16-63 (`SR_XLAT_SAMPLER_V2`: clamp
   U / V / W incl. mirror / mirror-once / border + border colour, separate min / mag / mip filters,
   anisotropy, MinLOD = mip_min_level, kBaseMap as MaxLOD = MinLOD + 0.25 with the chain kept). On the
   nine replay traces both are pixel-identical (the traces' two volume sampler states - wrap^3 trilinear,
   and wrap^3 linear + kBaseMap - sample the same texels either way). The same round fixed the shader
   translator's relative constant addressing (`tools/xenosrecomp/0004`, per operand as Xenia): every car
   after the first of an instanced car draw, and instanced trees, read `c[254 + a0]`-style constants;
   28 vertex shaders re-translated. Replay vs Xenia: gp1 0.485 -> 0.397 (cars 1.05 -> 0.48), gp2 0.392 ->
   0.338 (cars 0.95 -> 0.61), lg2 0.666 -> 0.647, menu 1.105 -> 1.104; the missing / black cars are
   drawn.
   **2026-10-20 (step-4 lane C, baked): the Xenos ALPHA TEST.** No translated draw had ever been alpha
   tested: XenosRecomp's test is a spec-constant clip our DXIL build compiles out. `tools/xenosrecomp/0005`
   translates every colour-0 PS a second time with Xenia's `CompletePixelShader` test
   (`<hash>.ps.at.dxil`, plain translation unchanged); a draw whose RB_COLORCONTROL enables the test draws
   with it, RB_ALPHA_REF / the function in the shared constants c17.x / c17.y (`gfx/alpha_test.hpp`,
   `SR_XLAT_ALPHA_TEST`, =0 the dev A/B). The Warworld arena grate (PS F6E8327B, GREATER 0.502) covered the
   lava under its slots - lane A's arena-floor defect: 16-frame trace 3840 frame 16 vs Xenia 9.86 -> 2.39
   (floor band 20.47 -> 2.65), trace 2745 6.89 -> 2.94; gp2 0.338 -> 0.337, lg2 0.647 -> 0.647, gp1 0.3971
   -> 0.3972 (its > 8-level changes all move toward Xenia). Alpha-to-mask is used by no draw of any trace.
   **Round 4 (Astra on 0005, four P2):** the test acts only in kColorDepth (Xenia binds no PS in kDepthOnly)
   and only for a PS writing colour 0 (`alphatest::applies()`); a draw it applies to whose PS has no variant
   gets one from the runtime translator at the PS's IM_LOAD (made only when its plain translation reproduces
   the cached PS, persisted as `<hash>.ps.atv.dxil`), and is DROPPED - `[XLAT_ALPHA] DROP` Error +
   `[SHADER_DROP] ... alpha-test-variant-unavailable` - only when that fails too (a whole-game run must count
   0); `tools/xenosrecomp/0006` compares in integer arithmetic on the float bits (DXC's float compares are
   `fast`: no NaN guarantee), checked on the compiled bytes by `tools/difftest/alpha_test_dxil_check.py`; and
   B1's XDK-clear bookkeeping no longer calls depth / stencil fully written when the test can reject.
8. **Depth clip follows PA_CL_CLIP_CNTL.clip_disable** (Xenia pipeline_cache.cc:1435). The
   D3D runtime clears a 640x360 target by a 4X-MSAA depth-only quad at z = -0.0 plus a 1X
   color strip for the last 8 rows; a clipping host dropped both, so the target kept stale
   EDRAM bits and the soft-particle pass composited magenta into the HDR scene (#9/#12).

Per-resolve result now (`archive/pm4_oracle/menu11_err.txt`): #0-#11 match except #7
(MAD 0.043, means 0.366 vs 0.373); #12-#13 MAD <= 0.056; log-luminance #14-#18 -1.18 vs
-1.14; bloom/tonemap #19-#25 MAD <= 0.035. **Menu final frame MAD 61.04 -> 13.88**
(`archive/pm4_score/rtc6`; gate-off control unchanged at 101.65, `ctl6`).

Then three more, from the final-frame residual:

9. **Mip chains.** Every texture was base level only. `getOrCreateTexture2D` now uploads
   Xenia's level range (mip_address != 0 -> min(mip_max_level, log2 longest axis)), each
   level placed by `cube_layout.hpp` generalised to N faces (`computeGuestLayout`):
   separate mips, the shared packed tail, and tiny textures whose BASE lives in a packed
   tail. HDR scene #7 MAD 0.043 -> 0.0115 (means 0.3728 vs 0.3729).
10. **Display gamma ramp.** Our #25 front buffer matched Xenia's to 3/255, but Xenia's
    presented frame is #25 through the game's DC_LUT ramp (a non-linear 256-entry table
    for an 8_8_8_8 front buffer). `gfx/gamma_ramp.hpp` ports the ramp state, the DC_LUT
    register side effects (Pm4Engine::writeReg) and the table application; the replay
    loads the trace's kGammaRamp snapshot and applies it at capture (`SR_GAMMA_RAMP=0` =
    raw). Menu MAD 12.38 -> 3.20. The live presenter does not apply it yet (P2).
11. **Culling, winding, polygon offset** from PA_SU_SC_MODE_CNTL / PA_SU_POLY_OFFSET_*
    (Xenia pipeline_cache.cc:1383, draw_util.cc:86). The cape is drawn twice with
    opposite cull modes; without culling the second pass painted both sides. Cape region
    MAD 16.1 -> 2.3; **menu final frame MAD 2.70** (`archive/pm4_score/rtc9`; control
    `ctl9` unchanged 101.65).

Blind second opinion (Astra, `archive/pm4_score/rtc8/astra_review.txt`, before the cull
fix): "a close compositional match" - camera, geometry, occlusion, shadows, shield and text
match. Still wrong:

- facade detail on the right building is scratchier / more fragmented than Xenia's (our
  samplers have no anisotropy, ignore the fetch constant's clamp Z / border / LOD bias and
  the tfetch instruction's own filter overrides);
- the thin UI rules at y~49 and y~669 are missing;
- a small detail left of PRESS START (x~555, y~587) differs (fixed 2026-10-18: it is drawn by one of
  the 28 vertex shaders the translator fix re-translated and now matches Xenia's green, item 7 round 2);
- MSAA surfaces render at 1X (replicated to all samples); volumes/cubes/arrays have no mips;
  the live presenter applies no gamma ramp. (Volumes have their mips and clamp Z since 2026-10-18,
  item 7.)

12. **FMV: the movie VS was never in the shader cache** (not memexport - the earlier note
    was wrong; `--dump_shaders` shows no shader in the FMV traces exports memory). The
    corpus came from the menu-era golden dump, so VS 4D9002F1 (the movie quad, 3-plane
    YUV PS D6E14C07) was rejected at `XLAT_REJECT gate=1`. `tools/synth_trace_shaders.py`
    takes Xenia's `--dump_shaders` output for any trace, finds every shader without DXIL
    and splices its ucode into the closest same-size container (exact container or <= 32
    vfetch-patched dwords); `tools/scan_disc_shaders.py` carves the containers present in
    the game files. **All four FMV traces are now bit-exact with Xenia** (max pixel
    difference 0, `archive/pm4_score/rtc10`); the previous run through the same capture
    path was flat grey, so the output is ours, not the trace's restored front buffer.
    Coverage gap for the full game: shaders first seen in gameplay need the same step
    (a trace per area, or a runtime translation fallback).

## 9. P4 - throughput: the frame census and the shared-memory model for everything (2026-09-29)

The PM4 frame was ~90 ms in gameplay (drain ~40 ms + render ~47 ms, both on the CP thread:
the drain translates each indirect buffer's draws when the CP executes it, the render runs at
the guest's swap). `[PM4_BIND_CENSUS]` (every 100 frames, next to `[PM4_FRAME]` and
`[PM4_SHM_FRAME]`) splits the bind by texture path (2D / 3D / array), the drain by prep /
vsfFetch / vsfIb / decode and the resolve by store / shm / inval / clear / cpu, and
`[PM4_SHM_FRAME]` reports the page validation (pages requested, lookup ms, hash ms). Each cost
it named was fixed by moving to Xenia's model (the shared memory is the GPU's copy of guest
RAM; the CPU never reads back, hashes or copies what the GPU can load):

| cost (title city / gameplay frame) | fix | gate (default ON, `=0` A/B) |
|---|---|---|
| array bind 5-12 ms: the k_16 640x640x4 shadow map read back, hashed, untiled, re-uploaded with two fence waits at EVERY bind | `pm4ShmLoadArray`: `texture_load_*` into the array's subresources from the shared memory, versioned by page stamps; an epoch/stamp-clock short-circuit for re-binds | `SR_PM4_SHM_ARRAY` |
| drain vsfFetch 12-15 ms: every fetched vertex range hashed and copied into the staging arena per draw | `SR_PM4_VSF_SHM`: the VS reads the shared memory at the guest physical address (t0 space5 = a raw view of it); a drain's requests stage into the arena and QUEUE their copies, recorded by the render right before the batch (draw order); the arena half flips at frame end; every drain is a validation epoch | `SR_PM4_VSF_SHM` |
| page validation 14 ms: per-page ownership lookups (a 256-entry command-buffer scan + the grant maps) and single-threaded 4 KB hashes | ownership once per request range; the hashes on the pool | - |
| 2D / volume bind 13-17 ms of map lookups (~25,000 2D + ~6,000 volume binds a frame) | the clean set carries the slot: one lookup | - |
| resolve 7 ms: the GPU-written re-baseline hashed both windows' copies per page | parallel | - |

Rules the model imposes (all in `gfx/d3d12_pm4_shmem.cpp`):

- A drain-time request cannot record a GPU copy (the frame list is closed between presents)
  and a page's upload must land in draw order, so it stages the pages and queues the copy
  (`m_shmPending`); `renderPm4Batches` records the queued copies up to a batch's cursor before
  that batch, and any left after the last batch before `pm4ShmEndFrame`. A drain-time
  arena-full opens the list, records and executes everything queued, closes it again.
- The arena half flips at the END of a frame's render, not the start: the next frame's drains
  write into the half whose GPU work this present's `moveToNextFrame` has waited for.
- A validation epoch per drain (a page the guest rewrote between two indirect buffers is
  re-hashed, as the staged path re-hashed every fetch) and a new epoch at frame end.
- A lambda cannot capture a `thread_local` by name: each pool worker saw its own empty
  candidate vector (two gate runs crashed on that before the reference was bound first).
- Page validation is collected and hashed together: a drain collects the pages its draws
  fetch (`m_shmCollect`, one parallel flush per drain, epoch per drain), and the render's
  texture pre-pass (`pm4ShmTexturePrepass`, `SR_PM4_SHM_PREPASS`) validates every texture the
  frame's batches bind in one flush at frame start - EXCEPT pages inside this frame's resolve
  extents (`m_shmResolveMask`), which keep the bind-time check in draw order after their
  resolve. Validating textures at DRAIN time instead uploaded a resolve destination's stale
  CPU copy over the GPU's resolve output (the gate broke: menu 82.4, FMV 48-63 -
  `archive/pm4_score/collect1`). `SR_PM4_SHM_COLLECT=0` restores per-request validation.
- ⛔ A window answer for a whole REQUEST RANGE must be one every page agrees with. a3647e9's
  per-request `physwin::gpuWindow(phys, len)` is per-RECORD containment, and the heap
  registers allocations per chunk: the movie planes (one ~900 KB k_8 texture across several
  heap chunks) failed the 0x8 containment while the physical pool's single grant passed the
  0xA one, so the range came back 0xA for pages whose own answer is 0 (contested: the legacy
  rule takes the non-zero 0x8 copy). Every intro movie and the loading screen rendered as
  green/magenta noise for two hours of commits (the replay gate cannot see it - the replay
  process has no allocator records). `gpuWindowRange` (f4837e7): a window owns the range only
  if it contains it AND the other window has no grant overlapping any part of it; else 0 =
  per page. Check the INTRO ERA on every change that touches windows or ownership.
- The pool itself had a lost-completion race (a late worker took the next region's chunk
  index; `[GPU_CP_STALL] phase=swap`): the cursor now carries the generation (`gfx/par.hpp`).

Evidence: the replay gate identical at every step (menu 2.70 / 164472 colours, FMV x4 0.00,
gp1 4.89, gp2 3.61, lg1 8.43, lg2 7.36; `archive/pm4_score/shmarr1`, `perf5`, `vsfshm1`,
`clean1`); intro-movie frames renderMs 14-21 -> 5.5 (array bind 5-12 -> 0.3, readback 6 -> 0);
title city (`archive/p4_city_pdb1.log`) `[PM4_SWAP_TIME]` render 37-43 -> 24-26 ms, drain
20-27 -> 11-14 ms, `[GPU_CP_TIME]` 12 -> 13-17 swaps/s; with the collected validation
(prepass1 roll): render 16-20 ms, 2D bind 3.3 ms for 20K binds, 15-18 swaps/s.
Open: the page hashing itself (~35,000 pages a frame, one request at a time is too small to
parallelize - next: collect a drain's pages and hash them together), index conversion
(vsfIb 3-5 ms), the CPU decode of rect-list / clip-off draws (decode 0-10 ms), the CP's own
packet execution (~30 ms/frame in `[GPU_CP_TIME]` ib, of which the drain is 11-14), the drain
and render still serialized on the CP thread, and an intermittent `[GPU_CP_STALL] phase=swap`
(2 of 4 title rolls with this stack; a CP hang at swap ~1024 predates it). The build now emits
CodeView for the harness sources (`build/SupermanReturns.pdb`) so the next stall dump reads.

## 10. P4b - the render thread (2026-09-29, `SR_PM4_RENDER_THREAD`, default OFF)

The frame was ib + swap on the CP thread: the drain translates each indirect buffer as the
CP executes it (`pm4DrainNow`), and at `PM4_XE_SWAP` the CP rendered the set, scanned the
front buffer out, presented and waited for the frame before last. Now the CP hands the set
to a render thread at the swap and goes back to the ring (`gfx/d3d12_pm4_edram.cpp`
`pm4SubmitFrame` / `pm4RenderThreadMain`, commit 60f36c3 on top of 593f4d4):

- **Handoff:** a `Pm4Frame` (batches, queued shared-memory copies, the arena third, the
  set's first drain epoch, front-buffer info) through a depth-1 queue; the CP blocks only
  while the render thread has not TAKEN the previous frame. Frames in flight: one on the GPU,
  one recording, one draining.
- **Ownership:** CP thread = engine, draw queue, drain, PSO cache, the drain third of the
  arenas, shm collect / validate. Render thread = frame list, texture / RT caches, EDRAM,
  resolves, present, fence wait, the presented notification; it binds the CP's guest-memory
  mapping (per thread). Shared under `m_shmMu`: the shared-memory page tables. The pool
  serializes parallel regions (`runMu_`), so both threads may use it.
- **Thirds:** BOTH arenas (xlat VB / CB, shm upload) rotate at a set's first drain in both
  modes (`pm4ShmBeginSet`); the drain waits for the third's last frame fence
  (`m_thirdFence`, the CP's own fence event) - already complete on the synchronous path.
  `pm4ShmEndFrame` no longer rotates or bumps the epoch: the render accepts validations since
  the set's first drain epoch, the same decisions as before.
- **No drain-time flush:** the frame list belongs to the render thread, and waiting for it
  under `m_shmMu` would deadlock, so a third that fills at drain time spills into a chunk
  tagged with the third (`ShmPend.src`), released when the third is reused.
- **Stamps at record time:** a queued copy stamps its pages when the render RECORDS it, not
  when the drain queued it - a texture loaded earlier in command order (the previous frame's
  list) read the old bytes, and its version must differ from the stamp a later bind sees.
- Live trace (`SR_PM4_TRACE_SWAP`) disables the thread (the trace writer serializes the engine
  and the renderer on one thread); the replay never presents through it.

Evidence: replay gate identical on the synchronous path (`archive/pm4_score/rt1`, `rt4sync`);
two clean 200 s title rolls (0 stalls, 0 crashes, 3100 frames, no spill). City
`[GPU_CP_TIME]` per 120 swaps: OFF idle 2887 ib 3286 swap 2641 ms (13.6/s); ON idle 5657
ib 3820 swap 1 ms (12.6-15.5/s), `thirdWait` 0.0 and `submitWait` 0.5 ms per 120. **What it
showed:** the swap's ~22 ms left the CP thread and became CP idle - the guest's own frame is
~75 ms (its main thread is the only guest thread whose lr moves between sampler ticks; the
rest wait), so the guest is the limiter now. Next: profile that thread (where the recompiled
code or the kernel services spend it), then bake the thread ON once its gameplay frames are
compared with the synchronous ones.

## 8. P3 - the PM4 path is the default (2026-09-29)

**Baked default ON:** `SR_PM4_LIVE` (implies `SR_PM4_RTCACHE`, `SR_PM4_CP`) and `SR_PM4_SHMEM`;
`=0` keeps the HLE draw path / the CPU texture path for an A/B. The HLE layer keeps running as
the observe / label / mod layer (its draw submission is off under the live PM4 path, as P2
arranged). Reason: NATURAL_BASELINE 2026-09-29 - the natural game reaches gameplay through the
PM4 path and renders a broadly coherent 3D Metropolis (Astra's wording), while the HLE draw
path renders the title as flat pink/tan and black frames (`archive/p2_default1`, the last
old-default roll).

- Replay gate with **no gates**: menu 2.70 / 164472 colours; FMV x4 0.00; gp1 4.89; gp2 3.61
  (`archive/pm4_score/p3`) - identical to the gated numbers.
- Live, no gates: `archive/p3_nogates1` (title city clean, `[IB_RANGE]` 0); the reach check
  `tools/scenarios/frontend_navigate_pm4.json` with `-Gates ""`: `archive/p3_reach1`.
- What the bake does NOT change: the memory-window fixes (SR_ALIAS_OWNER, SR_CB_WINDOW,
  SR_PM4_WRITABLE_OWNER, SR_PHYS_LIFO_FREE) were baked in their own commits; the replay tools
  still pass `SR_PM4_RTCACHE=1,SR_PM4_SHMEM=1` explicitly, which is now redundant.
- Open after P3: the title city runs ~8-12 fps (guest-side), street shading darker than Xenia,
  facade speckling, a flight view obstructed by an overhead surface, the trace writer's
  missing-page gap (Xenia renders our live traces dark), TIMEGATE_STUCK log spam.

## 7. P2 - the live game through PM4 (2026-09-29)

Gate: `SR_PM4_LIVE=1` (implies `SR_PM4_RTCACHE` and `SR_PM4_CP`). Default boot unchanged
(P3 below made it the default).

**Why the first live attempt froze after the EA|HD logo.** The D3D runtime does not simply
push IBs: every kick ends with an interrupt handshake (GpuEmitCommandBlock_sub_820FDA80:
SCRATCH_REG4/5 = {sub_82110B00, arg} and SCRATCH_REG0 = cpu mask, written back into the
pump context; WAIT_REG_MEM on the write-back; INTERRUPT; CP_CPU_INT_ACK; WAIT_REG_MEM until
the ISR cleared the pending bit; SCRATCH_REG4 = 0x0BADF00D). After the first kick the
runtime is in jump-chain mode (dev+10868 != 0): later segments are linked into a software
list and only the pump thread (VdInterruptPumpWait_sub_82110A30 ->
GpuCommandBufferReplayDriver_sub_82110840 -> 0x82 record -> GpuRingUpdateWritePointer)
submits them, and it only wakes on that interrupt. Two things broke it:

1. the synchronous doorbell executed IBs inline, ignored WAIT_REG_MEM, had no scratch
   write-back and delivered interrupts late from the vblank thread;
2. spliced guest hand-edits from the no-GPU era (iter-109/123/124/157 in the pump, plus
   GpuRingWaitProgress / GpuRingReserveSpace / GpuCommandBufferFlush /
   GpuPresentEmitCommandBlock) disabled every CPU-side GPU wait, faked the retired fence and
   - iter-124 - skipped the replay driver outright. Only 5 doorbells ever happened per run,
   HLE and PM4 alike.

**Fix (Xenia model).** `kernel/gpu_command_processor.cpp`: the doorbell publishes the write
index; a CP thread executes the ring from index 0 (the first kick carries ME_INIT), writes
the read index back, runs the guest ISR at each INTERRUPT impersonating a DPC on CPU n
(KPCR+268, EmulateCPInterruptDPC) and presents at PM4_XE_SWAP, which VdSwap now writes
exactly like Xenia's VdSwap_entry. `Pm4Engine::cpMode` adds real WAIT_REG_MEM, scratch
write-back, Xenia EVENT_WRITE_SHD, MEM_WRITE, REG_TO_MEM, SET_CONSTANT2/SHADER_CONSTANTS,
VIZ_QUERY, type-0 one-reg and type-1 packets. The fake-GPU edits are gated on "no CP"
(`tools/regen/patch_fakegpu_gate.py`, docs/BANDAIDS.md iter-460), so with the CP the guest
runs pristine code. Ring IBs keep the proven 0x8-window read rule; everything else uses the
0xA window (pump context and fence block live at 0xAC42DCxx).

**Result (archive/p2_live5, K=1, GPU readback + PrintWindow agree; Astra blind review
`archive/p2_live5/astra_review.txt`).** The natural boot presents the intro in order: EA
outline, EA circle (99.4% identical to the Xenia oracle at thumbnail scale), DC, WB
Interactive, the legal screen, the starfield and the Superman-shield movies. CP census in
the sibling run p2_live4: 8192+ doorbells, ~1200 ISRs, 600+ swaps, 90k draws, 0 desync, 0
bad IB.

Still wrong:

- throughput: ~100 ms of host work per frame (EDRAM ownership transfers on the CPU ~60 ms,
  bind ~20 ms, resolves ~20 ms) -> ~6 swaps/s; the render thread now waits on real GPU
  completion, so the intro takes ~160 s and the title screen was not reached in 180 s;
- the shield-blur frame (p1080) has a hard-edged ~24 px left / ~8 px top strip the
  post-process pass does not cover (a window/resolve offset);
- the DC frame at 60 s shows dotted cyan structure that may or may not be the animation;
- EA|HD was not in the sampled presents; boot gauges (defExec, parser) do not reach the
  menu in 150-180 s at this frame rate.

Next: GPU-side EDRAM transfers and resolves (Xenia's host render-target path) for
throughput, then the offset defect, then the menu.

## 11. Runtime shader translation (iter-463, `SR_SHADER_RUNTIME_XLAT`, default ON; `=0` = the old drop)

**Why.** A draw whose VS or PS hash is missing from `data/shader_cache/dxil` is dropped
(`d3d12_backend` gate 1/2). Xenia translates every shader on first use and skips a draw only
until its pipeline exists (`gpu_flags.cc` `async_shader_compilation`, `pipeline_cache.cc`,
`d3d12_command_processor.cc` "pipeline not ready"). The offline cache cannot be complete: the
XDK patches every vertex shader's vfetch stride/offset/format at bind time and the cache key
is the patched ucode, so a known program bound with a new vertex layout is a new key (16
Warworld VS in iter-463; 40078E73 and 8B37013C still drop draws in the tutorial). This
section closes that gap with Xenia's contract: drop until ready, then draw.
(Superseded on 2026-10-17 by section 11.1: a miss now waits for its translation.)

**Pipeline.**

```
IM_LOAD miss (pm4_engine onShaderLoad, CP thread)
  -> ShaderCache::noteVsMissUcode   ([SHADER_MISS] twin naming, now wired)
  -> ShaderCache::requestTranslation -> rtxlat onMiss: copy the ucode, queue once per hash
worker thread "SR shader xlat" (host only, below-normal priority)
  -> xlat::translate(): EXACT or PROVED (below), XenosRecomp in-process, dxcompiler.dll
  -> persist <root>/xr-<id>/<hash>.{vs,ps}.* (temp + rename, .dxil last)
  -> ShaderCache::buildEntry (the boot loader's own parser) -> admitRuntime
next draw: findVs/findPs hit the admitted entry; getPm4Pso/getXlatPso build the PSO
```

| File | Role |
|---|---|
| `gfx/shader_runtime_xlat.{hpp,cpp}` | game side: hooks, job queue, worker, cache dir, persistence, admission, `[SHADER_XLAT]` log |
| `gfx/shader_xlat_core.{hpp,cpp}` | pure core (no log, no guest): container parse/validate, index, the stage_missing_vs proof, preflight, DXC via `LoadLibraryW`, checks |
| `gfx/xenos_bridge.{hpp,cpp}` | the one call into XenosRecomp's `ShaderRecompiler` (compiled with its pch) |
| `cmake/xenos_xlat.cmake`, `cmake/xenos_embed.cmake` | the `xenos_xlat` library from `tools/XenosRecomp-clone`, the embedded `shader_common.h`, the DLL + licence copy |
| `gfx/shader_cache.{hpp,cpp}` | `rtVs_/rtPs_` behind a `shared_mutex` (consulted only after an installed miss, only once `rtCount_ > 0`; never erased, so pointers stay valid), `admitRuntime` (installed wins), `buildEntry`/`loadEntryFromDir`, the hook table |
| `kernel/shader_container_dump.cpp` | the CreateVertexShader/CreatePixelShader capture now also COPIES each validated container to the translator (observe-only; OVERRIDES.json unchanged) |
| `gfx/d3d12_backend.cpp` | a missing shader is no longer negative-cached in `m_xlatPsoCache`/`m_pm4PsoCache` (a later admission must build the same key); gate 2 counts PS drops |

**Linked, not a helper process.** `ShaderRecompiler::recompile` is a pure function of one byte
buffer with no mutable global state (only `static constexpr` tables, no getenv/file I/O), so one
instance per job is thread-safe, and the parity test proves the in-process output
byte-identical to `XenosRecomp.exe`. A helper process would add IPC and a second binary to
ship for no fidelity gain. The price is that the recompiler runs where an access violation is
fatal: see "Preflight".

**Where containers come from.** Live: every container the game hands to CreateShader
(captured at entry, before CreateShader fills +0x1C/+0x20; deduped by content, capped at 8192).
Replay (no Create calls): `SR_SHADER_CONTAINER_DIRS` (`;`-separated dirs of `*.xpu`, one
container per file - the `stage_missing_vs.py` corpus rule; a `corpus.txt` works as an entry),
scanned once by the worker before its first job. A request with no container yet waits
(`[SHADER_XLAT] WAITING`) and is retried whenever new containers arrive.

**Two translation paths** (a shader neither proves is refused, never approximated):

- **EXACT** - a container holds this very ucode (every PS: the XDK never patches a PS; a VS the
  game binds unpatched). Exactly `XenosRecomp/main.cpp` per container: recompile, DXC, signed
  check, fetch-mode variant (a `.vs.vsf.dxil` failure is non-fatal, as offline). All containers
  holding the ucode must share byte-identical metadata; for a VS, so must every same-program
  (patch-masked) container, as on the PROVED path (a VS at IM_LOAD is always bind-time patched).
- **PROVED** - `tools/stage_missing_vs.py` ported step for step: same-program candidates by the
  patch-masked ucode (PATCH_DW0/1/2), one metadata among them (byte compare), template = a
  member cached in the installed set else the first by origin, `prove_same_program`
  (control-flow walk, CF/ALU/TFETCH dwords identical, exact vfetch fields, unpatched bits,
  non-executed dwords), a vertex element for every written fetch, splice + XXH3 re-check,
  translate, then checks 4-6: sidecar == rows derived from the runtime ucode; the `.vs.vsf.dxil`
  disassembly's index multipliers == stride*4, fetch-constant cbuffer rows == slot>>1, load
  offsets inside the fetched words; input signature == the reference translation's (ISG1 read
  from the DXIL container), minimal line diff vs the reference HLSL touches vfetch lines only;
  and when the reference is a cached sibling, its re-translation == the installed files. Any
  failure refuses (`[SHADER_XLAT] REFUSED <hash>: <reason>`), and the draws stay dropped.

**Preflight** (`recompilerPreflight`, before every recompile). The release recompiler has no
asserts, and `shader_recompiler.cpp:226` dereferences `vertexElements.end()` for a written
vertex fetch without an element. The parity corpus contains such containers: `0142416A...`
(carved garbage labelled PS, with a vfetch) crashed our first in-process run and was an SEH
"recompiler crashed" offline. XenosRecomp's own CF scan also differs from Xenia's walk (it
ignores exec address 0), so the Xenia-walk validation alone does not bound what it reads. And
XenosRecomp keeps its HLSL indentation in a `uint32_t`: a `LoopEnd` with no loop open, or a
forward-jump label that closes more blocks than are open, wraps it and the next `indent()`
writes ~4 billion tabs (garbage `07DE6F96...` left a 4 GB `.fail.hlsl` in the offline output;
`7FCA4485...` grew without bound in-process). The preflight replays XenosRecomp's two CF scans
and refuses an exec slot past the ucode, a written vfetch with no element (any in a PS), an
element usage past the 14-entry tables, and an indentation underflow. On `data/shaders_*`,
`archive/fmv_p1/disc_shaders` and `archive/v1_linkfix/stage_in` it refuses 104 of 555
containers (29 fail validation, 75 the preflight), all carved garbage (ASCII text or data
tables where the ucode should be); 4 garbage "PS" among them have translations in the
installed cache (the IM_LOAD structure gate keeps such payloads from ever being drawn). Over
the union of every corpus dir (4996 containers: 2582 accepted), an AddressSanitizer build of
the whole translator (recompiler, bridge, core; DXC uninstrumented) translated all 2582 with
0 reports, and the largest HLSL any accepted container produces is 74 KB.

**Threads and cost.** Hooks copy and return. The VS/PS miss hook blocks the requesting CP
thread until the job ends: since 2026-10-17 by default, bounded (section 11.1); before that
only with `SR_SHADER_RUNTIME_XLAT_WAIT=1`, for deterministic replays. One translation (recompile
x2 + DXC x2, plus the reference translation and disassembly on the PROVED path) took 87-234 ms
for the 16 arena VS in the unit test; the stage corpus scan takes 2 s warm.

**Cache dir.** `SR_SHADER_RUNTIME_DIR`; else with `SR_INSTANCE_ID` (every automated roll) a
fresh `%TEMP%/SupermanRecomp/shader_cache-<id>-<pid>` (the `kernel/xam_content.cpp` rule: rolls
never inherit translations); else `<exe>/shader_cache` with `portable.txt`; else
`%LOCALAPPDATA%/SupermanRecomp/shader_cache`. Path variables are read as UTF-16 (`_wgetenv`).
Entries live under `xr-<translator id>/`. The id is XXH3 of three things:

- SHA-256 over every source that decides what is translated and admitted: the clone's
  recompiler, `shader_common.h`, `dxc_compiler.{cpp,h}` and `main.cpp` (mirrored by hand in the
  core), and our `gfx/shader_xlat_core.{cpp,hpp}`, `gfx/xenos_bridge.{cpp,hpp}` and
  `gfx/shader_mkey.hpp` (CR stripped). All of them are CMake configure dependencies.
- the DXC DLL bytes;
- `kRulesVersion`, an extra salt.

Persisted entries are NOT re-proved at boot, so a translator change must, and does, start a
new directory. The file names are the same as in `data/shader_cache/dxil`, plus
`<hash>.<vs|ps>.xlat` (provenance: template, proof, checks) and
`containers/<vs|ps>_<hash>.xpu` (the container that was translated).

Writes go to a temp file, the close is checked, and then the file is renamed. The `.dxil` the
loader keys on is written LAST, and only when every other write succeeded. At boot
`ensureLoaded` admits an entry only when it is complete:

- a well-formed DXBC `.dxil` (magic, non-zero digest, size field equal to the file size);
- a `.xlat`;
- for a VS, a `.vfetch`;
- for a PROVED VS, a well-formed `.vsf.dxil`.

Anything else is skipped with a log line and retranslated on its next miss. The installed
cache wins a clash, and a byte difference is logged. When a per-process `%TEMP%` directory is
opened, the worker first removes the `shader_cache-*-<pid>` directories whose process has exited.

**Evidence (2026-09-30, `python tools/difftest/shader_xlat_test_run.py --work <scratch>`;
ASAN census: the same sources with `-fsanitize=address`, `--only-parity` over the union split).**
The translator built exactly as `cmake/xenos_xlat.cmake` builds it (also checked with a scratch
CMake project that includes the module: T1-T4 34/34; the final runner: 37/37 + T5, 16/16 mutants killed):

- T1: the 16 spliced iter-463 containers + 3 controls -> 19/19 byte-identical to the installed
  `.vs.dxil`/`.vs.vfetch`/`.vs.vsf.dxil` (DXC 1.8, 4662 commits, 416fab6b).
- T2: the 16 runtime ucodes (the `.vs.bin` misses) against the stage corpus (4333 containers)
  -> 16/16 PROVED, files byte-identical, `.vs.mkey` identical, check values equal to the Python
  manifest's (DBE06113: stride 7->9, vsf mul [4, 36], cb rows [47], hlsl diff 2 lines).
- T3: refusals (ALU change, fetch-const change, non-executed dword, tampered reference,
  missing vertex element, two metadata, PS twin metadata, bad hash) and a stride-11 variant of
  DBE06113 that must translate with the index multiplied by 44; the preflight refuses the two
  containers that took the recompiler down (7FCA4485 LoopEnd underflow, 0142416A exec past the
  ucode) without running it.
- T4: the game path through `ShaderCache` - WAIT admits before the request returns with the
  offline DXIL and vsf; async admits both after the worker idles; persisted files are
  byte-identical; a fresh cache admits the 3 at load with no job; the full installed cache wins;
  `SR_SHADER_RUNTIME_XLAT=0` translates nothing.
- T5 parity: 451 accepted containers, the offline exe run on the same set -> 380 outputs
  byte-identical, 0 differ, 0 only-runtime, 0 only-offline, 71 SKIPs on both sides; the one
  offline "recompiler crashed" (`DE085F89...`) is a C++ throw ("unsupported VS export register
  16") caught by the SEH wrapper offline and by `xenos_bridge` in-process.

**Status (2026-09-30): linked into the game, `SR_SHADER_RUNTIME_XLAT` default ON since the
replay proofs A-C and the live tutorial roll below (`=0` = the old behaviour, for the A/B). It
was default OFF in 84b752b while unproven.** `CMakeLists.txt` adds `gfx/shader_runtime_xlat.cpp` to
`APP_SOURCES` and `include(cmake/xenos_xlat.cmake)` + `sr_link_xenos_xlat(SupermanReturns)`
(difftest links the stub). The build copies `dxcompiler.dll`, `dxil.dll` and four licence files
next to the exe.

**Replay proofs** (`archive/v1_tape3/traces/live_454107ED_23799.xtr`, the Warworld arena; the
partial cache = `data/shader_cache/dxil` minus the 16 arena VS, 808 of 856 files; commands in
TOOLKIT 1.6g; frames in the session scratchpad `proof/`):

| Run | Result |
|---|---|
| base: full cache, xlat off | 1 frame, 1045 draws |
| partial cache, xlat off | `[SHADER_MISS]` for the 16; 96,768 px (10.5%) differ from base: the arena's orange walls, cyan lights and tower details are missing |
| **A**: partial + WAIT + stage corpus, empty `SR_SHADER_RUNTIME_DIR` | 16 `request` -> 16 `admitted ... (proved)` (first 2108 ms incl. the 1960 ms corpus scan, then 88-248 ms each), 0 draws dropped; frame **pixel-identical to base (0 px)**; the 48 persisted `.vs.dxil/.vs.vfetch/.vs.vsf.dxil` byte-identical to the offline-staged files |
| **B**: partial, async, `LOOPS=2` | pass 1: 208 draws dropped (DBE06113 92, E866F0D3 58, B8AFFB5A 32, ...), frame == the partial-off frame (0 px); all 16 admitted 2.1-4.3 s after their request; pass 2: 0 dropped; inside the 10.5% region the base/partial difference covers, 93.4% of pixels equal the full-cache `LOOPS=2` control's pass 2 and 0.5% equal the dropped frame; whole frame MAD 0.018 vs the control (4.8% of pixels differ by small amounts: pass-1 drops leave different carried state; the control itself drifts 36% pass 1 -> 2). Repeat run: pass 2 identical (0 px) |
| **C**: partial, async, `SR_SHADER_RUNTIME_DIR` = A's dir | `16 persisted translations admitted`, 0 requests; frame **pixel-identical to base (0 px)** |
| per-process dir (no `SR_SHADER_RUNTIME_DIR`, two sequential rolls) | each under `%TEMP%/SupermanRecomp/shader_cache-replayxr_D<n>-<pid>`, 16 admitted; the second logs `pruned 1` and only its own dir remains |
| exit while translating (async, 1 pass, trace done before the jobs) | before the fix: 32 s to exit (the 30 s atexit drain timed out; the in-flight job never finished inside `exit()`); after `rtxlat::stop()` at the end of the replay: 7 s, exit code 0 |
| goldens (`tools/pm4_replay_score.py`), xlat default (off) and `=1` | menu MAD 2.06, eahd/t16/t21/t25 0.00 in both; no shader misses, so no requests |

**Review fixes (Fix stage).**

- **CRLF reference control (must-fix).** The PROVED path compared the installed
  `<ref>.vs.vfetch` byte for byte with the recompiler's LF sidecar. A `core.autocrlf=true`
  checkout (the Git for Windows default) would have refused every proved shader. Now it
  compares with CR removed (`noCR`), and `.gitattributes` marks `data/shader_cache/** -text`.
- **Teardown (must-fix).** `getShaderCache()` is never destroyed. The GPU replay calls
  `rtxlat::stop()` before main returns. An `atexit` handler (3 s) is the fallback: the worker
  takes no new job and parks.
- **Partial writes (should-fix, 2 findings).** `persist()` stops at the first failed write, and
  the `.dxil` is written only when all the other files landed. Any stale `.dxil` of the hash is
  removed first. `writeAtomic` checks the close and the size. At load an entry must be complete
  (see Cache dir).
- **Translator id (should-fix, 2 findings).** It now hashes our translator sources and the
  clone's `dxc_compiler`/`main.cpp` (see Cache dir).
- **Default OFF until proven (should-fix).** Done.
- **EXACT path, VS (nit).** `stage_missing_vs.py` rule 2 now applies here too: one metadata among
  ALL same-program containers, not only those holding the exact bytes.
- **Encoding and exceptions (nit).** Path variables are read with `_wgetenv`. `onLoaded` (inside
  ensureLoaded's scope guard), the corpus scan and each job are wrapped in try/catch, so an
  exception disables or fails the job instead of calling terminate. A `corpus.txt` recursion
  stops at depth 4.
- **Directory growth (nit, 2 findings).** Per-process `%TEMP%` directories of finished rolls are
  pruned. Old `xr-<id>/` directories under `%LOCALAPPDATA%` are NOT pruned. Two builds may
  share that root (an A/B), and the stale ones cost only disk. Recorded as a known leftover.

**Tests after the fixes** (`python tools/difftest/shader_xlat_test_run.py --work <scratch>
--reuse-offline`):

- 42 checks passed, 0 failed; OFF 3/3.
- Mutants killed: 20/20.
- The new checks:
  - a CRLF installed sidecar still proves;
  - control: an exact VS container on its own translates EXACT;
  - an exact VS with a same-program sibling under other metadata refuses;
  - a failed `.vfetch` write persists no `.dxil`;
  - a VS entry without `.vfetch`, or with a truncated `.dxil`, is skipped at load.
- The new mutants: M17 (CRLF), M18 (EXACT rule 2), M19 (persist without short-circuit), M20 (load
  without validation).

**Live proof (natural game, no mod, `SR_MODS=0`).** Repro:

```powershell
.\tools\sr_drive.ps1 -Scenario tools\scenarios\tutorial_tape_full.json -Out archive\<dir> -TimeoutSeconds 540 `
  -Gates "SR_MODS=0,SR_INPUT_REPLAY=archive/v1_tape3/tutorial.inpr,SR_SHADER_RUNTIME_XLAT=1,SR_SHADER_RUNTIME_DIR=<rt>,SR_PRESENT_DUMP_DIR=archive/<dir>/frames,SR_PRESENT_DUMP_SPARSE_EVERY=120,SR_PRESENT_DUMP_SPARSE_CAP=600"
```

Run `archive/iter463_xlat_live3/attempt1` (log `run.log`, whole-run sheet `../sheet.png`):

> **Provenance caveat (2026-09-30, custom-frontend settings review finding 7).** While this
> roll ran (10:52:54-11:14:12), a parallel session linked `build/SupermanReturns.exe`, which
> replaced the file (and its .pdb) on disk under the running process. The process kept its
> own image, so the roll's log lines and frames stand. But `build/` no longer holds the
> binary that ran: any `va_to_source`, pdb symbolization or crash triage of this roll must
> use the exe recorded in `provenance.json` (`exeSha256` DE2CA2D3...8685E), not `build/`.
> Treat host-address evidence from this window as SUSPECT.

- `main_hud` was requested 2:20 in. The roll ran 21 min, through the tutorial (landing,
  heat-vision and "senses" prompts) into free flight. I stopped it: my wrapper had timed out.
  There was no crash and no PSO failure; the grep hits on "CRASH" are asset names
  (`*_METEOR_CRASHED_*`).
- It captured 187 live containers from CreateVertexShader/CreatePixelShader. Five VS missed
  the cache:
  - **40078E73** (tutorial): PROVED from template `live VS #122` (ucode 246E361C, installed).
    5 vfetch differ in patch fields only, stride 0->8; all checks True. It was admitted 197 ms
    after the request (translate 186 ms); 9 draws were dropped meanwhile and none later.
  - **ABECF9C8** (new, not in the earlier report): PROVED from `live VS #120`. Admitted after
    156 ms; 16 draws were dropped meanwhile and none later.
  - **8B37013C** (tutorial), **D3D16764** and **DA6B521F**: `WAITING ... no container runs the
    same program (no template)`. None of the 187 captured containers runs their program, so
    they stay dropped, as before. D3D16764 and DA6B521F drop ~260-300 draws per summary
    interval in free flight, 15,224 draws in total by the end.
- Nothing was relaxed. The open question for these three is where their containers come from:
  either a create path the capture does not hook, or a shader built before the capture could
  see it. Next step: `SR_SHADER_DUMP_MISSES=1` for their ucode, then
  `tools/stage_missing_vs.py` against the disc corpora.
- The tape is open-loop and this roll is not an A/B, so the frames cannot attribute pixels to
  40078E73. The evidence is the admission plus no further drops. The frames look like the
  earlier tutorial rolls, darkness included.

`tools/run_recomp_x.ps1` alone does not get past the title: nothing presses START. Two such
rolls sat on the title and attract loop and saw no miss. Use `sr_drive.ps1` with the
scenario above.

**Corrections to the design as planned.** DXC is loaded with `LoadLibraryW` behind our own
wrapper that passes `dxc_compiler.cpp`'s argument strings verbatim, instead of compiling
`dxc_compiler.cpp` against the import library (a missing DLL would stop the exe, and its
failure path is an assert). The preflight above was added: "only proven containers reach the
recompiler" was not enough. The EXACT path serves a VS too (offline-equivalent, before the
PROVED path). A `.vs.vsf.dxil` failure is non-fatal only on the EXACT path; the PROVED path
refuses it, as `stage_missing_vs.py` does. Metadata groups compare bytes, not SHA-1; the HLSL
diff is a minimal LCS diff (same line counts as difflib on all 16); ISG1 is read from the DXIL
container instead of a `dxc -dumpbin` table. The hooks are a function-pointer table on
`ShaderCache`, so the shared translation units link and behave exactly as before when the
translator is not built.

### 11.1 The miss policy and AOT coverage (render/shader-coverage, 2026-10-17)

"Drop until ready" no longer holds. The release composition stops on an untranslated draw
(`[NATIVE_RECORDING] FAILED stage=untranslated-draw`, `gfx/d3d12_backend.cpp`): the release
exe died on lane B's chapter-12 save load on `9F8BBF3F...`. On dev the drops showed: 192
draws in 250 ms for `ABECF9C8...`, 12 in 193 ms for `9F8BBF3F...`, and the whole run for the
shaders that never got a template.

**Policy (`SR_SHADER_RUNTIME_XLAT_WAIT`, default ON; `=0` = the old async drop).** The IM_LOAD
that misses (`record_engine` onShaderLoad -> `ShaderCache::requestTranslation` -> `rtxlat` onMiss)
blocks the CP thread until its request leaves the queue (admitted, or answered no: no template,
refused, failed) or until the budget runs out (`SR_SHADER_RUNTIME_XLAT_WAIT_MS`, default 30000,
digits only, 1..600000). The CP executes in order, so every draw behind that IM_LOAD sees the
answer.

- **Hard failure only for a real "no".** A shader that cannot be translated (no template,
  refused, failed) is still dropped on dev and still stops the release. The release line now
  names the translator's state: `... reason=missing-vs code=1 xlat=<state>` (`dropStateName`:
  `no-template`, `translation-refused`, `translation-failed`, `wait-budget-exceeded`, ...).
- **Logged.** `[SHADER_WAIT] VS|PS <hash>: <outcome> after N ms of waiting (wait #n, T ms
  waited in all)` (a milestone tag, so player logs keep it too), plus a Warn every 5 s while a
  wait lasts. Counters: `rtxlat::waitStats()`. `[SHADER_DROP]` names the first draw each missing
  shader drops, then a 2 s summary.
- **No re-wait.** A request that ran out its budget is not waited for again. Its draws report
  `wait-budget-exceeded` until the worker admits it.
- **Admit, then persist.** The worker admits a translation before it writes the files, so the
  wait ends at admission.

**Where it waits, and why it cannot deadlock** (the comment above `waitForTranslation`; Astra
round 1 found no lock cycle):

- The CP thread holds no CP, ring or queue lock while it executes a record
  (`kernel/gpu_submit.cpp` releases `c.mu` first). `requestTranslation` calls the hook outside
  every `ShaderCache` lock, and the wait releases the translator's own mutex (a condition
  variable). Meanwhile the guest waits on its own fences, as it would behind a busy GPU.
- The worker never waits on the CP, the render thread or a guest thread. It reads its own queue,
  the container index (guest threads push only copies), DXC and the files. `admitRuntime` waits
  only for the installed stage, and that is published before any request can exist.
- The boot load: a request exists only after the translator stage (WP2), so a wait never
  overlaps the staged load.
- The watchdog: for the length of the wait the CP is in its host-wait phase
  (`ShaderCache::hostWaitBegin/End` -> the CP's wait observer, `kCpHostWaitTranslation = 4`), so
  the hang watchdog does not read the CP's own stall as a GPU hang.
- The quit: once the graceful quit starts its CP drain (`quit::cpDrainRequested`, read through
  the observer's `draining()`), a wait in progress ends within 5 s, so the 10 s drain is never
  spent on one translation.

**Coverage.** The gap was not missing containers. It came from (1) a same-program rule that was
too narrow and (2) a runtime index that held only CreateShader captures. The game binds its
`.AST` shaders in place, so CreateShader never sees them.

- **Bind-time patch model** (ported from release/installer, tightened by Astra round 1).
  `gfx/shader_xlat_core.cpp` and `tools/stage_missing_vs.py` treat these alike:
  - the vfetch fetch-constant index is a patch field;
  - a run of consecutive vfetch slots inside ONE exec clause (`vfetchRunsByClause`) may be
    permuted, but only when the pairing keeps the order of every two runtime fetches that depend
    on each other at component level (`fetchOrderKeepsDependences`). The dependences checked
    are: RAW on the index component; WAR; WAW on overlapping written components; relative
    addressing counts as dependent.
- **Shipped template corpus** `data/shader_cache/containers/` holds the 184 disc containers
  (`tools/scan_disc_shaders.py` output, from `archive/fmv_p1/disc_shaders`). The runtime
  translator indexes it at its first job (`SR_SHADER_CORPUS_DEFAULT=0` = off).
  `make_player_build.ps1` copies it, and `docs/release_composition.json` classifies `*.xpu` as
  translation.
- **AOT additions**, staged offline with the same model and installed as
  `.vs.dxil`/`.vs.vfetch`/`.vs.vsf.dxil` only. The VS count went 216 -> 247 (2026-10-17):
  - the forcing lane's 21 dumps;
  - 3 captured on a dev chapter-12 roll;
  - 2 captured on a release chapter-12 roll;
  - 1 captured on a release era-R4 roll;
  - 1 captured on a dev era-R3 roll;
  - 3 captured on a dev era-R4 roll, including `6FD8C731`, which earlier dev rolls never got a template for.

  Every addition was staged under the guard. The first 23 were re-staged once the guard landed,
  and each file came out byte-identical to the installed one. The live captures that kept a
  runtime directory came out byte-identical to the game's own translation.
- **The point-sprite GS ships too.** `data/shader_cache/point_sprite.gs.dxil` is the geometry
  shader every point-list draw needs. The packager now copies it, and `pointSpriteGs()` finds it
  beside the installed cache's `dxil` folder (`ShaderCache::resolveInstalledDir()`). Before this,
  a package started outside the dev checkout had no GS, so the release stopped at its first
  point-list draw (`reason=missing-pso`).
- **Shipped files are read from beside the exe first.** `ShaderCache::shippedDataDir` resolves
  `data/shader_cache/dxil` (and with it the GS and the template corpus) and `data/shader_cache/nb` in this order:
  the env override, then the exe's own folder, then the working folder (the dev fallback, since `build/` carries
  none). Before, the working folder came first, so a release started from an older checkout compiled its 7 resolve
  kernels at boot and tripped H3. A PC A/B from a working folder holding an empty `data/shader_cache/nb`: the old
  exe compiled 7 kernels and hit H3 at 16.0 s; the new exe compiled 0 and loaded its own 247 VS. Evidence:
  `archive/m2_claude_20261017/shaders/cwdtest/` in the lane worktree. `SR_DATA_CWD_FIRST=1` restores the old order.
- **Capture loop.** Run with `SR_SHADER_DUMP_MISSES=1 SR_SHADER_DUMP_MISSES_DIR=<dir>`, then
  `python tools/stage_missing_vs.py --inputs <dir> --out <stage> --corpus data/shaders_carved
  data/shaders_menu archive/v1_linkfix/stage_in archive/fmv_p1/disc_shaders`, then install the
  three files per STAGED-OK hash. `python tools/shader_miss_census.py <run.log>...` names every
  runtime translation, wait, drop and release stop. Exit 0 = the policy held; `--strict` also
  fails when the AOT cache missed anything.

**Tests** (`tools/difftest/shader_xlat_test_run.py`; the corpus is pinned, see the lane NOTES):

- T4w (a): the default waits and admits.
- T4w (b): a 0.5 s budget against a 3 s job releases the IM_LOAD, is not waited for again, and is
  admitted later.
- T4w (c): an ALU-mutated VS with no template returns at once.
- T4w (d): six waits, each reported as a host wait.
- T4w (e): a PS miss.
- T4w (f): the quit cuts an 8 s job to about 5 s.
- T3g: a dependent swap is refused; the same pair in program order proves; runs end at exec-clause
  boundaries.
- Mutants M10 and M29-M36: wait ignored, default off, unbounded, not a host wait, waits again,
  drop state lost, quit does not cut, runs cross clauses, dependences ignored. Each counts as
  killed only when a FAIL line names its target check.

Not covered offline: the real IM_LOAD -> draw wiring, the watchdog itself and the release stop
line. The live rolls in the lane NOTES cover those.

## 12. Resolution scaling (iter-464 PORT FEATURE, `SR_RES_SCALE` / `settings/display.ini`)

Full description, settings and evidence: [`RESOLUTION.md`](RESOLUTION.md). It lives in the PM4
layer, not the HLE layer: every render target, the EDRAM buffer, every resolve and every texture a
resolve wrote are `s` times larger per axis, exactly as Xenia's `draw_resolution_scale_x/_y`
(ported, BSD-3), and the guest still sees 1280x720. The present scans the front buffer out at the
internal resolution and fits it to any window (aspect kept; area filter when shrinking).

- Needs the GPU EDRAM + shared-memory path (the default); the CPU mirror / resolve / texture
  paths and the EDRAM snapshot restore have no 1:1 mapping and fall back to 1x.
- Replay oracle: `tools/pm4_replay_score.py --scale <s>` against Xenia's trace dump at the same
  scale (`tools/xenia_scaled_oracles.py`). The 1x replay is byte-identical to the pre-feature
  build; 2x and 3x carry the 1x residual and no new one.
- `SR_PM4_GPU_TIME=1` reports the GPU time per PM4 frame (timestamp queries).

## 13. Step-4 lane C closures: the stale-heap tables (2i) and the empty carved PS (2026-10-20)

### 13.1 2i - descriptor tables bound from a heap that was not set (found chasing the 890M one-present specks)

Symptom (lane C NOTES 2026-10-18..20): on the ROG Ally (Radeon 890M) a rare per-quad one-present difference - 1 to ~360
pixels, a few levels, ~0.25 % of frames - reproducible in a deterministic gp2 replay loop with no guest CPU at all; the
PC (RTX) showed 0 odd frames in 1,200. Not the par.hpp pool race (lane G's fix A/B: 1 vs 3 odd of 1,000).

Cause: the D3D12 debug layer + GPU-based validation on the PC named ERROR 708 (SetGraphicsRootDescriptorTable: the
handle's heap is not the heap that is set; >= 1,024 times in 20 gp2 frames) and GBV ERROR 935 (a PIXEL draw read an
uninitialized root argument, parameter 2). Every translated draw binds its textures through
TranslatedTextureSnapshot::bind, which sets the native descriptor arena's heaps on the frame's command list; the EDRAM
passes after it (pm4GpuLoad's direct transfers, nbFillLoad, pm4ShmLoadTexture, pm4NativeResolve, edramGpuStore) bound
tables from m_xlatSrvHeap assuming the heaps set when the list opened. Undefined behaviour: NVIDIA resolved the stale
handles to the intended descriptors, the 890M occasionally did not.

Fix: D3D12Backend::pm4XlatHeaps() re-sets the xlat heaps at the start of each such pass, and before
native_targets.inc's two compute passes (the target resolve and the held-memory materialization). The gate
SR_PM4_XLAT_HEAPS is baked ON: in dev, =0 restores the old binds as an A/B arm only; in release it is always on.
Evidence (lane C NOTES 2026-10-20):
- the GBV A/B on the PC (gp2 looped, debug layer + GBV + an error-stack logger): with the gate off, ERROR 708 >= 1,024
  and GBV 935 >= 32 per 20 frames; with it on, 0 of each and no other error ID; the image is the same in both arms
  (84baf03832);
- the replay matrix r9set (1b561e4e, gate default off): 22/22 byte-identical - the default equals r8set and =1 equals
  the default on 9 traces + 2 arena frames, so the fix moves no PC pixel; the PC flicker gate with =1 PASS (span 0 of
  358, freeze 15 / bad 0);
- Astra on the fix + bake: two more passes bound xlat tables without the re-set - native_targets.inc's
  nbResolveToTarget (a successful target resolve never reaches pm4NativeResolve) and nbMaterializeHeld (reached from
  pm4ShmLoadTexture before its re-set); both re-set the heaps now. Neither raised a debug-layer error on gp2 (no
  compute-table error ID in either arm), so the Ally A/B below, which ran without them, is not weakened for gp2.

The 890M specks are NOT (only) this. The Ally A/B (s4r_heapab: gp2 looped, the arms alternating process by process,
DRED on in both) reached its pre-registered terminal readout in the first round: base (=0) 1 odd frame of 400, fix
(=1) 3 of 349 - the same one-present specks (5-9 px, max 8-13 levels, random frames and places); 0 device removals in
either arm (the DEVICE_HUNG question, ~1 per 13 launches, is underpowered here). The fix is baked anyway: it removes
real undefined behaviour and changes no PC pixel. The specks are 2i-b (13.1.1): with the complete fix they remain, a
known minor issue.

#### 13.1.1 2i-b - the 890M specks after the fix: a known minor issue, localised (the starting point for step 6)

Rate with the complete fix (d9584c97, all seven passes; the Ally, gp2 looped, set s4r_2ib_a's rate arm with no
checkpoint hashing, so the A/B's timing): 3 odd final frames of 1,600 (0.19 %), 0 device removals. The A/B base was
0.25 %. The stale heaps were a real bug but not the cause of the specks.

Localisation (RT0 checkpoint FNVs, SR_PM4_DUMP_HASH, r7/hash2i_analyse.py): every localised odd frame first diverges
on the main scene RT 00C08000 (depth 006082D0) at or just after one of the gp2 frame's two largest draws, and the
difference survives to the present:
- batch 2749 = draw 2798: VS 9370721E.., PS 81AAB911.. (upper 32 bits), 9,546 vertices, opaque (blend0 00010001),
  depthCtl 00700766, kColorDepth, eleven textures (t0 0FCDE000, t1 0FCEE000, t2 10154000, a cube t3 13FD0000, 3D t4
  13348000 / t6-t8 103A6000, t5 0FCAE000, t14 11F4A000, t15 1E830000). At EVERY=1 over b2721-2752, 6 of 6 odd
  sequences first differ exactly there (b2748 equal); at EVERY=32, 2 of 3 first differ in its window;
- the window b2753-2784, which holds batch 2784 = draw 2833 (VS 31168A8B.., PS C1F710FE.., 9,828 vertices): 1 of 3 at
  EVERY=32. One more frame diverged after b2752.
The specks are quad-sized: 2-8 px (1-3 x 2-4), a few to 61 levels, at scattered positions. One odd image recurs
exactly across processes and exes.

Timing: with a GPU sync after every batch of b2721-2752, one process showed 7 odd frames of 400 (1.75 %; P(>= 7) ~
1e-4 at the base rate). At EVERY=32 (3 of 1,200) and without hashing (3 of 1,600) the rate is the base one. So the
fault rate moves with GPU scheduling. Every draw of that run brings new t0 / t1 material textures, so a first-use
upload race does not single out 2749. GBV on the PC finds nothing after the fix, and the RTX shows 0 odd frames in
1,200.

Status: a known minor issue for step 4. It is Ally / 890M only, ~0.2 % of frames, one present, a few pixels, with no
guest-state effect (a replay with no guest CPU reproduces it); our known UB is removed and a driver-side race is
suspected. Starting points for step 6:
- The gp2 replay looped with SR_PM4_DUMP_FROM=SR_PM4_DUMP_TO=2749 and RT0 BMP dumps at b2748 / b2749, to see the quad.
- Vary draw 2749's inputs (one texture slot at a time bound to a constant texture, a flat-colour PS, half its
  vertices) to find what the fault needs.
- RGP / PIX on the Ally, which needs the owner's approval for Windows Graphics Tools.
Evidence: lane C NOTES 2026-10-20 (s4r_heapab, s4r_2ib_a, s4r_2ib_b; archive/.../render/r7/ally_2ib.py, ally_loop2i.py).

Instruments kept (dev builds, default OFF): SR_D3D12_GBV, SR_D3D12_DEBUG_STACKS (an InfoQueue1 callback logging the
stack of ERROR 708 / 935), SR_PM4_DUMP_HASH (RT0 checkpoint FNVs per batch for looped replays;
archive/.../render/r7/hash2i_analyse.py), and SR_D3D12_DRED for device removals.

### 13.2 The 43 cached pixel shaders whose translation is `ret void` - garbage carved entries, benign

0006's regeneration left 43 `.ps.at.dxil` byte-identical because their translation writes nothing: no `storeOutput`,
no `discard` (the plain `.ps.dxil` likewise). All 43 come from `data/shaders_carved`, and XenosRecomp emits no
instruction for them: `00c09db87b45d0cd` is the declarations and a closing brace, its literal constants holding a
container header (c252 = 0x102A1100, 0xA1C, 0x1228 - the carver cut across a container boundary);
`27fe16e9e98b56d4` is a jump table whose every case only breaks (dumps: archive/.../render/r7/retvoid_examples/). No
draw uses them: none appears in any replay's `[SHADER_SEEN]` (menu, eahd, t16 / t21 / t25, gp1, gp2, lg1, lg2, both
arena traces) nor among the xtr draws of gp1, gp2 and the arena traces. They are dead cache entries.

The 43: 00c09db87b45d0cd 07589714727970ca 1a09398381ad6af6 2627e2fb361a2eed 27fe16e9e98b56d4 2c414107473e2655
2f148748135aaa3d 2fea5155df8a8890 521a006519d2f19e 58bd98b7893e634f 5cc254b87ac3522c 5db24981db2acfa3
607c374958cf8ea5 60f00b53a59261e4 6478e6cb5d835383 6540c2c23c6d107e 68b5f2e18fbcbc2a 6d507ec36bcc62ce
7468b22747fd0aaf 778444dd902d7ff9 8c1def974f971222 938e82e20c835e30 970ff194c65dcba3 9b993fd9e5d5f058
9f5fadeda4bb6746 a53b2ca99f56f773 a764480455577ece ab78fd3accbbd5de b9f30b8e673e605c be31e51bf911ae8b
bfc09ccc66f4dabe c4e31349db671980 c6ea89f9bd746a22 c74891a6ccb7d35f d1a71b852ae41588 d71537da8825b555
db7bf4c07d2bfb2f dc6634539671c4b7 dcb62f8618994e4a e9127711bb4d47c2 f1b2f98622f6812e f96148c6909e178b
f9aa7f8de00a3bb9.

Release hygiene (low priority, not step 4): the carver or the cache build should reject an entry whose translation has
zero instructions.
