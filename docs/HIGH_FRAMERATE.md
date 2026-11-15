# High frame rate (PORT FEATURE): render above the game's 30 Hz

User decision 2026-10-02: *"prioritise framerate and resolution. target 120fps, 60fps minimum.
target 4k but support all resolutions"*. This doc covers the frame-rate half: how the game's
loop works (verified), what depends on the simulation clock and what on the wall clock, the
options, the one we built, and how it was checked. Resolution scaling is the resscale lane;
making each frame cheap is perf/platform and perf/codegen.

**Status (iter-464):** prototype behind `SR_HIGHFPS` (default OFF). Automated natural rolls run
without it. See "Verification" for what is proven and what is still open.

## 1. The answer in one paragraph

The game already decouples rendering from simulation. FGGameSim ticks a **fixed** 1/30 s step;
FGGameRender is a **separate** GameKernelLoop thread at 30 Hz with a **variable** step; the two
meet in **EA::StateStream**: every sim tick publishes a time-stamped frame of render state, and
every render tick reads the two frames that bracket its own clock and blends between them
(camera matrices are nlerped, animation channels, cloth, VFX and shadow segments are
interpolated). The 30 fps cap comes from the render loop's own rate (one byte in the image,
`0x82911889 = 30`) plus the present pacing (`D3DPRESENT_INTERVAL_ONE` against a 60 Hz vblank)
and our host swap chain (`Present(1, 0)`). So the port feature raises **only** the render
loop's rate and lifts those two platform caps. Game logic, Havok, scripts, timers and the
camera all keep running at the console's 30 Hz, on the same fixed step, so gameplay is the
console's and the 30 Hz logic can still be checked against Xenia.

## 2. The loop (verified contracts)

### 2.1 Four GameKernelLoop services

`FSGameApp_Init2_StartServices_sub_8218CFE8` starts four services, each a `GameKernel::Loop`
thread running `GameKernelLoop_Run_sub_82720B50` on its own mgr object (176 bytes). Rates come
from the byte table at `0x82911888` (4 bytes per `g_FeFlowConfig+420` mode, all three modes
`3C 1E 1E 3C`):

| service | mgr global | rate | step mode (+0x34) | role |
|---|---|---|---|---|
| FSGameCore | `0x8296D8D8` | 60 Hz | 0 fixed | core services |
| FGGameRender | `0x8296D8DC` | 30 Hz | **1 variable** (dt = elapsed) | `FSGameRenderMain_Tick_sub_821491A0` |
| FGGameSim | `0x8296D8E0` | 30 Hz | **0 fixed** (33,333,334 ns) | `FeMasterTick_821494C0` = all game logic |
| FGGameSound | `0x8296D8E4` | 60 Hz | 1 variable | audio |

Golden `sound_gameplay`: render mgr `0x05B47830`, sim mgr `0x05B47780`; sim +0x50 =
2385733381048 = exactly 71572 x 33,333,334 (pure fixed-step accumulation).

Mgr fields (`GameKernelMgr_Ctor_sub_827209D8`, `GameKernelMgr_SetTickRate_sub_82720418`,
`GameKernelLoop_TimeGate_sub_827206B8`, `GameKernelLoop_AdvanceTickTime_sub_82720740`,
`GameKernelLoop_PostTickStats_sub_82720858`): +0x34 step mode (0 fixed: dt = period;
1 variable: dt = elapsed; 2: min(elapsed, cap +0x58)), +0x3C rate (float), +0x48 period ns
(`1e9/rate + 0.5`), +0x50 the loop's time (i64 ns, += dt after each tick), +0x60 last clock
sample, +0x68 elapsed, +0x70 debt (clamped to one period, so a loop never runs faster than
its rate to catch up), +0x78 dt ns, +0x80 tick counter, +0x84 dt seconds (float, what the
payload gets), +0xA0 average tick cost, +0xA8 ticks per second (0.5 s window; the dev
build's Performance HUD prints these four as `%12s: %4u ms (%4.1f FPS)`).

The clock is `GameKernelClockNs_sub_827203A0` = (KeTimeStampBundle ms + 1) x 1e6, so every
loop paces on a 1 ms grid. An idle loop yield-spins (`IdleYieldOrTimedWait` with config 0).

### 2.2 The sim tick publishes, the render tick reads (EA::StateStream)

`FeMasterTick_821494C0` (FGGameSim, dt = sim +0x84 = 1/30): ... `StateStreamGroup_BeginWrite_sub_82787650(group0, sim+0x50)`
... game logic, camera (`Camera_UpdateTransform_sub_821808F8`), view (`UpdateRenderViewFromCamera_sub_82176698`)
... `StateStreamGroup_EndWrite_sub_82787730(group0)`. So each sim tick k publishes one frame
stamped k x 33,333,334 ns.

`EA::StateStream::Manager` = `*0x829A93C8` (array of groups; group 0 = golden `0x05B3EDB0`,
4 streams of 0x208000 bytes; group 1 only when `g_FeFlowConfig+136 == 1`). Group: +0 CS,
+0x2C blend t, +0x30 read stamp, +0x38 next index, +0x3C cur index, +0x40 stamp being
written, +0x48 last published stamp, +0x50 write index, +0x54 stream count, +0x60 stream
array. Stream: +0 stamp, +8 data, +0xC state (0 being read, 1 being written, 2 ready,
3 fresh). The writer takes the oldest ready/fresh stream; the streams being read are never
taken (difftest S6). All stamp comparisons are 64-bit (`cmpd`).

`FSGameRenderMain_Tick_sub_821491A0` (FGGameRender): renderer Lock; `StateStreamGroup_BeginRead_sub_82787520(group0, render+0x50)`;
`render+0x50 = group+0x30` (`std r11,0x50(r31)`); renderer `vt[+0xC]` Draw(dt = render +0x84);
EndRead; renderer Unlock. BeginRead picks **cur** = the newest ready frame with stamp <= time
and **next** = the oldest with stamp > time:
- both: read stamp = time, t = (time - cur)/(next - cur);
- cur only (the render clock caught up with the sim): read stamp = cur's stamp, t = 0;
- next only: next becomes cur, read stamp = its stamp, t = 0;
- none: returns 0 and nothing is drawn.
Then `PostTickStats` adds the render dt to render +0x50. So the render clock chases the newest
sim frame, at most one sim tick behind, and snaps back whenever it overtakes it.

The renderer's Draw (`*0x829A9384` vt+0xC = `sub_8230C038`; golden sound_gameplay: renderer
`0x02370058`, vtable `0x82098120`, slot +0xC = `0x8230C038`) **ignores the dt it is passed**.
It hands `Renderer_BeginFrame_sub_82775FA8` (the render-script manager vt+0x10 and
`RenderBin_UpdateAll_sub_82783B48` -> every bin object's vt+0x10) and
`Renderer_DrawFrame_sub_82776038` (stored at renderer +0x50) the value
`t = group0+0x30 * 1e-9`: the read stamp, i.e. the interpolated **game time being shown**, in
seconds. So the render-side objects are driven by the time the sim published, not by how many
render ticks ran.

The renderables read both frames through `StateStreamGroup_GetReadBlocks_sub_827877A8(group, handle,
&cur, &next, &t)` and blend when t != 0 and the next block is valid ('VALD'). 23 readers,
including the camera/view (`sub_8277A168` -> `sub_8277A048`: two matrices nlerped when the
camera id matches, else copied = a cut snaps; one float lerped), shadow segments
(`sub_822F10F0`), animation blend channels (`ApplyAnimBlendChannel_*`), node keyframe tweens,
timelines, cloth (`ClothApplyParamUpdate_sub_82306248`), VFX (`VfxState_ReadInterpolated_sub_8279A790`).

### 2.3 Present pacing

Every present goes through `D3DDeviceSwap_sub_82112050` (render-script technique presents,
movie `D3DDeviceBlitAndSwap_sub_82112750`, the persist-display re-swap in
`VdInterruptPumpWait_sub_82110A30` every 30 ms while the game is not rendering).
`GpuPresentEmitCommandBlock_sub_82111EC8` reads the D3D PresentationInterval at dev+0x33A4:
golden = **1** (`D3DPRESENT_INTERVAL_ONE`; 0x80000000 = IMMEDIATE would skip pacing). It embeds
a CP-interrupt callback `GpuPresentPacingUpdate_sub_82111E38` between two WAIT_REG_MEMs: the GPU
holds the flip until at least `interval` vblanks have passed since the last flip; the vblank ISR
(`GpuInterruptCallback_sub_820FDBA0`, source 0) counts dev+15124 and releases the countdown.
`D3DDeviceSwap` then blocks on the previous swap's fence (one frame in flight). Golden counts:
dev+15124 = 144279 vblanks, dev+15136 = 58090 pacing updates (Xenia session average ~2.5 vblanks
per present). Nothing else in the image reads the vblank counter; the swap callback slot
dev+15120 is 0.

On our platform the vblank is `vdVblankThread` (kernel/stubs.cpp, 60 Hz), the CP executes the
WAIT_REG_MEMs (`Pm4Engine::cpWaitRegMem`, Xenia semantics), and `PM4_XE_SWAP` presents with
`IDXGISwapChain::Present`. **Xenia does the same thing:** its `framerate_limit` cvar is the rate
of `MarkVblank` (graphics_system.cc); with vsync off it fires every 1 ms.

So the console's cap is min(render loop 30 Hz, vblank 60 Hz / interval 1, display).

## 3. Inventory: what runs on which clock

**Sim clock (fixed 1/30 s, unchanged by the feature):** everything in `FeMasterTick`: game logic,
scripts/Lua, the Apt front end (`AptVmAdvanceMainScript_sub_82176DF8(dt)`), timers counted in sim
ticks, the cameras (`Camera_UpdateTransform`, FlightCameraTick, the idle camera: 10 s of sim time
on `sim+0x50`), input event timestamps (`sub_8229A7A8` stamps pad events with `sim+0x50`), Havok
(`hkWorld_stepDeltaTime_sub_82595F00`; the golden loop step mode 0, period 33,333,334, dt 1/30 -
tools/havok_snapshot.py), physics character proxies, the world HUD. Frame-count logic in the
sim counts sim ticks, so it cannot change.

**Render clock (variable dt = elapsed, the one the feature speeds up):** the render tick's
StateStream read and the work it does once per pass: the renderer's bins and the render-script
manager (both given the SHOWN GAME TIME `t`, not a dt - section 2.2), the per-frame LoadUnit
clip drain (`LoadUnitMgr_DrainPendingClipClears_sub_821A5D38`), the Apt resource update
dispatch (`AptResourceUpdateDispatch_sub_821869A8` -> `AptUpdateResourcesTraverse_sub_826EFEA8`),
and the movie frame-get (below). Every other reader of the render mgr `*0x8296D8DC` in the
image reads its +0x50 time (`ld r11,0x50(r11)`, IDA xrefs: `SMModelRenderObj_Ctor_sub_822E8BD0`,
`TerrainRenderObj_Ctor_sub_822F6DD0`, `RenderNodeCulledSubmeshes_sub_82307CF8`,
`RenderMeshGeometryPass_sub_8230F8E0`, `RenderMeshGeometryPassWithShaderConsts_sub_823103C8`,
`SmModelRenderObjBuildRenderState_sub_82312E88` - fade start times and shader time; the rest
of the xrefs are the service start / shutdown plumbing). None of them reads the tick counter or
the dt; the only dt read is the render tick's own (+0x84), which Draw ignores. The render loop
was built with a variable step, so the console
already ran it at varying rates whenever a frame was slow. Anything that counts render passes
instead of reading `t` would speed up with the render rate - see "Open" for the empirical check.

**Wall clock (mftb / QPC, 50 MHz since iter-463):** shader time param 0x29, the load-screen
sequencer, WMV decode budgets, the XMA lock timeout, Apt's variable-dt mode. Unaffected.

**Media clock:** the movie frame-get runs ON the render loop: the render-script `fmvmovie`
pass (`RenderYuvVideoFrameTec0_sub_8235CA48`) asks `MovieVideoDecodeYuvToRgbLoop_sub_82480D80`
for one frame per render pass. The decoder compares the frame's sample time with a QPC wall
clock (`CalChannel_AdvanceClockGetDrift_sub_82476408`): a frame more than 38 ms early is HELD
(offered again next pass); otherwise it is consumed and, when more than 5 ms early, the render
thread sleeps `drift - 5` ms (ENGINE_MAP 6g.1b, the movie lane's contract). A late frame is
never dropped. So a render loop faster than the movie is throttled by the hold + sleep (the
movie stays at its media rate), and a loop slower than the movie stretches it. The roll check
is section 6.

**Vblank:** only the present pacing above.

## 4. Options

**(a) Raise the sim tick (1/60, 1/120).** Rejected. Every fixed-step consumer would have to be
found and rescaled: Havok's step and solver tuning, scripts and Lua timers that count ticks, the
Apt front end, the idle camera, input timing, AI. The Apt movies and Lua run game design that
was tuned at 30. Any miss changes gameplay, and Xenia (30 Hz) could no longer check the logic.

**(b) Decouple render from sim and interpolate.** This is what the engine already does
(section 2.2). The UnleashedRecomp-style work (find the camera and entity transforms, keep two
snapshots, blend at submission) is done by the game's own StateStream readers. What is left
is to let the render loop run faster and stop the platform from capping the presents.

**(c) Hybrids** (e.g. sim at 60 for the camera only) - not needed; (b) already covers the
camera.

**Chosen: (b), using the engine's own mechanism.** Gameplay stays bit-for-bit the console's
30 Hz logic; only the render thread runs more often.

## 5. The implementation (`SR_HIGHFPS`, kernel/highfps.cpp)

| piece | where | what |
|---|---|---|
| render loop rate | `FGGameRenderMgr_Ctor_sub_82166178` override (gate off = exact pass-through) | the rate in f1: 30 -> the target |
| emulated vblank | `vdVblankThread` (kernel/stubs.cpp) | 60 Hz -> max(60, 2 x target), high-resolution waitable timer with absolute QPC deadlines |
| host present | `D3D12Backend::presentSwap` (gfx) | sync interval 1, or 0 + `ALLOW_TEARING` above the display refresh / uncapped |
| pacing log | `stub_VdSwap` -> `highfps::onGuestSwap`; `presentSwap` -> `highfps::onHostPresent` | `[HIGHFPS_PACE]` every 2 s: guest presents/s, interval p50/p95/p99/max, the shown game-time step per present, held frames, render and sim ticks/s and sim time / wall from the engine's own counters, and the HOST present cadence (presents/s, interval p50/p95/p99/max = what the display gets) |

Modes: `SR_HIGHFPS=monitor|1` (display refresh, vsync), `=<N>` (cap N; vsync when N <= refresh),
`=uncapped` (1000 Hz loop, no vsync). Experiment overrides: `SR_HIGHFPS_RENDER_HZ`,
`SR_HIGHFPS_VBLANK_HZ`, `SR_HIGHFPS_SYNC`; `SR_HIGHFPS_PACE=1` logs pacing with the feature off
(the 30 fps baseline).

Swap-chain integration rules (for every lane that touches the swap chain): the swap chain is
created with `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` only when the feature asks for it and the
factory supports it (`m_allowTearing`). Any `ResizeBuffers` call must pass the same flags
(`m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0`), or DXGI fails the resize - the
resolution lane's window-mode resize (render/resscale) passes 0 today, so whichever lane merges
second reconciles it. `presentSwap()` drops the tearing flag for good if DXGI refuses it
(exclusive full screen: sync 0 tears there anyway).

Why the vblank is 2x the target: the game asks for at least one vblank between flips; the render
loop paces on a 1 ms clock, so a vblank exactly at the target rate would sometimes make a frame
wait a whole extra vblank. At 2x, the render loop's own period is the limiter.

## 6. Verification: the clock inventory, measured (iter-466 hfr)

The owner's question (2026-10-04): *"what physics or other engine stuff is tied to 30hz"*.
Answer: all of the game's simulation - logic, Lua, Apt, cameras, Havok, character proxies,
timers, input stamps - is tied to the 30 Hz FGGameSim step BY DESIGN, and that is not what
caps the frame rate: the render loop is a separate variable-step loop that interpolates
between sim frames (sections 2-3). Nothing visible was found that advances per render pass.

**Method: hold the sim, keep rendering** (`SR_DIAG_SIM_FREEZE=<period_s>:<hold_ms>:<start_s>`,
a named DIAGNOSTIC, default off; kernel/highfps.cpp). The FGGameSim loop's thread sleeps after
a tick while the render loop keeps drawing. Nothing new is published, the render clock settles
on the newest StateStream frame (t = 0) and the shown game time stops, so every system that
runs on the sim clock or reads the shown time draws the SAME picture every pass. Whatever still
moves runs on another clock. `SR_PRESENT_DUMP_FREEZE=1` dumps up to 10 presents per freeze
(from 400 ms in, when the render clock has caught up); `tools/perf/freeze_diff.py <log> <frames>`
diffs each freeze's frames against its first one. Run at two render rates, a per-pass system
would differ from a wall-clock one.

| roll (gameplay scenario, port features) | freezes in gameplay | result |
|---|---|---|
| f3: render UNLIMITED (`archive/v1_hfr/f3_unc`) | 6 (streets, HUD + minimap, cape, traffic, pedestrians, blimp, 2.0-2.5 s held, 44-78 passes each) | pixel-identical: 0.000% of pixels > 8, max channel diff 1-2 |
| f4: render 15 Hz (`f4_15`) | 5 | pixel-identical (max diff 2-3) |
| f5: UNLIMITED, freeze every 10 s (`f5_unc`) | 16: the TITLE (PRESS START over the 3D city), the custom MAIN MENU, gameplay, the PAUSE MENU, a tutorial popup | all 16 pixel-identical (max diff 1-3): the Apt UI and its 3D backdrops are on the sim clock |

What moved while the sim was held - each on a clock other than the sim's (the eye adaptation below
counts render passes):
- **Movies** (boot logos, legal, the title's logo movie, the opening FMV): media clock. The
  decoder paces each frame against QPC (section 3); above the movie's rate the render loop is
  throttled by hold + sleep, below it the movie stretches (at a 15 Hz render loop the intro
  movies played at 13.8-15.2 fps; at UNLIMITED 22.5-30 fps = the decode rate, movie lane).
- **The LOADING screen's dots** (f4 freeze 7, the dot row only): the LoadScreen sequencer,
  wall clock at 1.000x (ENGINE_MAP timebase contract, `[LOADSEQ_OBS]` rolls).
- **The sea surface** (shader time param 0x29, ShaderParam_WriteTimeSeconds 8260DDC8: QPC minus
  the device-init stamp, in seconds - wall clock, the same contract). The f-rolls had no water in
  view; the save-started gates did, and their freezes moved on the water: every pair of a sea
  freeze differs, growing from k0 to thousands of pixels (iter-467 native-a: sgb1's save gate,
  ledger save_gate_drift). Correct for the game - the water's clock is the wall, not the render
  rate - but the freeze oracle asks for a still frame, so since f2b83734 SR_DIAG_SIM_FREEZE holds
  0x29 with the sim (still during a hold, the held time removed after it; diagnostic only,
  SR_DIAG_SIM_FREEZE_SHTIME=0 = the A/B). The proof, one roof over the marina for every arm
  (dev-tool-assisted warp, `archive/v1_native/sgd6b`): held, segmentless ON PASS 15/15 and OFF PASS
  16/16 with every sea freeze at 0 changed pixels; the same exe with SHTIME=0 FAILS [1, 2, 9, 10],
  its six sea freezes at 39/39 differing pairs (k39: 790-8,198 px, maxdiff up to 97) on the water.
- **Eye adaptation** (2026-10-13, M2): the exposure history steps once per RENDERED frame by the
  authored `xExposureTimeScale` (c0.x = 0.25, ENGINE_MAP 6g.8d), so it keeps converging while the sim
  is held. Correct for the game, but it moves frozen presents: whole frame, one direction, decaying,
  maxdiff 2-4. At ~100 fps the leftover gap at a hold's start is below one level; at ~40 fps it is
  not: the passing 74ef828f exe capped with SR_HIGHFPS=40 fails both of its gradable freezes that way
  (`archive/claude_baseline40`), as did every native candidate below ~70 fps. Since then
  SR_DIAG_SIM_FREEZE also sets c0.x = 0 during a hold (`kernel/hdr_adaptation.cpp`, diagnostic only;
  `[SIM_FREEZE] eye adaptation HELD`; SR_DIAG_SIM_FREEZE_ADAPT=0 = the A/B). On the native draw path
  at 39 fps: running = FAIL 11/15 (`archive/claude_texcache_v2`, an exe without the hold), held =
  PASS 15/15 with all 576 gameplay-freeze pairs identical (`archive/claude_adapthold_v1`). Astra
  (2026-10-13) confirms the one-directional decaying drift in every failing freeze. Controls on that
  one exe (431239844ab1): SR_DIAG_SIM_FREEZE_ADAPT=0 FAILS 5/15 with the same decaying signature
  (`archive/claude_adapthold_ab0`); the hold with the known flicker mutation
  SR_NATIVE_CONST_DEDUP_STORE_EARLY=1 still FAILS 10/15 gameplay freezes, local regions at maxdiff
  82-165 (`archive/claude_adapthold_mut`), so the held oracle still catches draw defects. Open: a
  native/reference check of the luminance chain itself (the hold suppresses that chain's
  contribution to exposure).

Clock inventory (verified = measured in a freeze roll or by a rolled contract; static = IDA):

| clock | systems | render-rate effect |
|---|---|---|
| sim (fixed 1/30 s) | game logic, Lua, Apt (AptVmAdvanceMainScript), HUD, cameras (incl. smoothing, idle camera), Havok step, character proxies, timers, input stamps (static, section 3); everything drawn in gameplay (verified, freezes) | none: unchanged at any render rate |
| shown game time t (render side) | renderer bins / render-script mgr / DrawFrame, camera + matrices nlerp, animation channels, cloth (the cape), VFX state, shadows, fades, shader fade times (static 2.2; verified: frozen t = frozen picture) | none: interpolated between two sim frames |
| wall clock (mftb/QPC) | LoadScreen sequencer, shader param 0x29 (the sea surface; verified sgd6b - held by the freeze oracle since f2b83734), WMV decode budgets, XMA lock timeout | none |
| media clock | movie frame-get on the render loop | none above the movie's rate |
| rendered frame (each evaluation) | eye adaptation (PS 040D6633 history step, c0.x = authored 0.25; held by the freeze oracle since 2026-10-13) | per present by itself (faster at higher fps); normalized to the console's 30 evaluations per second by `SR_HDR_ADAPT_DT`, default ON since 2026-10-18 (X2, "Acceptance and bake" below) |
| per render pass | LoadUnit clip drain, Apt resource-update dispatch (work queues: faster render = they drain sooner; no visible timing) | throughput only |
| sim (lightning rig) | chapter-18 storm lightning light-rig hops (ENGINE_MAP 6g.8e): ~19 hops per 1.01 s burst at ~95 presents/s, the same count with 61 or 162 presents in the burst (2026-10-17, `_worktrees/lightning/archive/v1_lightning/clight_bursts.txt`) | none: the strobe does not speed up with the render rate |
| per render pass | HDR eye adaptation, PS `040D6633B0AA5D6D`, `c0.x = 0.25` | fixed feedback weight per present; faster adaptation at higher render rates (2026-10-11 paired probes below); corrected by `SR_HDR_ADAPT_DT` (default ON since 2026-10-18; measured equal to 30 Hz in wall time) |
| vblank | present pacing only | lifted by the port feature (section 5) |

Pacing at UNLIMITED (dev 5f8533e2, `archive/v1_hfr/e2_trunk_unc`, ingame, 2 s windows): the
shown game-time step follows the wall clock (|step - wall| 1.3-2.5 ms), 0 held frames,
0 backsteps, mean blend t 0.46-0.49 (real interpolation), sim 29.7-30.1 ticks/s
(sim_time/wall 0.990-1.004). Not covered by a freeze yet: combat VFX scenes (arena),
weather (water: covered, sgd6b - see "What moved").

Limit of the method: a render-side smoother that converges in under ~400 ms after the shown
time stops would not show in a freeze at ~100 fps. One does exist: the eye adaptation steps per
rendered pass (above, 2026-10-13); at ~100 fps it settled below one level, which is why it was not
seen, and at ~40 fps it fails the freeze until the oracle holds it. The hold does not prove the
oracle valid at every rate: at low rates a hold yields fewer samples and the shown time may not
settle.

## 7. The 120 fps budget (iter-466 hfr)

Render loop UNLIMITED, vsync off, 1280x720 internal, gameplay scenario (tutorial), medians
of the ingame era. Sources: `[HIGHFPS_PACE] tick_cost_ms` (the engine's own FGGameRender tick
cost, GameKernelLoop +0xA0), `[PM4_SWAP_TIME]` (host render thread), `[GPU_CP_TIME]` (CP per
swap), `[PM4_GPU_TIME]`, the host profiler (`tools/perf/hot_lines.py`, line level). Rolls
`archive/v1_hfr/e2_trunk_unc` (unprofiled: 35.2 fps) and `b2_unc` (profiled).

Per stage, ingame (median ms per frame; a1_on / a2_on = this lane's build, unprofiled, 34.7 /
35.65 fps; other processes 5.2-5.8 busy cores):

| stage | ms/frame now | what dominates | to get under 8.3 ms |
|---|---|---|---|
| CP (GPU_CP thread: PM4 interpretation, drain, shm validation) | 23.2 ib + 4.8 swap (the texture prepass at submit) = ~28 = THE FRAME | pm4StageVsf (vertex staging) ~30% incl., shm page validation + XXH3 ~17%, mutex waits on the render thread 9% (16-18% before this lane's change), register writes / execute | 3.4x: the native lane's D3D replacement (no PM4 interpretation); meanwhile write-tracked page validation instead of hashing, cheaper vsf staging |
| guest render tick (FGGameRender: game + XDK, engine's own +0xA0) | 27.8-28.1 wall, of which ~60% spinning in GpuRingWaitProgress (CP backpressure) = ~11-12 ms of work | XDK command building (constant/type-0 leaves already native), `__savegprlr/__restgprlr` helpers, bounds tests, cloth light masks, particle quads | ~1.5x: codegen (inline the save/restore helpers, LTO/PGO) and native XDK draw emission |
| host render thread (pm4RenderFrame) | 17.0-17.8 render + 0.5 submit/present | texture binds ~30% (14,000 2D binds per frame: key hash + map lookups + revalidation), resolves ~20% (shm request + rebaseline hashing), D3D12 driver ~10%, mutex waits ~10% | ~2.1x: a per-frame bind memo, write-tracked revalidation instead of hashing, fewer driver calls |
| GPU (`[PM4_GPU_TIME]`) | 7.2-7.9 at 1280x720; **14.8-16.4 at the owner's 4K (3x internal, 3840x2160)** (k1_4k) | the EDRAM / resolve emulation passes and the draws at 9x the pixels | at 4K: ~2x - fewer full-screen emulation passes (native resolves, native lane), or a lower internal scale for 120 |
| present + vsync (UNLIMITED, tearing) | 0.3 present, 0.0-0.1 frame wait | - | nothing to do |
| sim tick (own thread, not on the render path) | 5.0 per 33.3 ms | - | none (fixed 30 Hz by design) |

Title: 50-63 fps (CP 13-16 + 3.3, render thread 13.6-14.6, GPU 4.2-4.5). Custom MAIN MENU:
35-40 fps (CP 20-23 + 3.8-4.5, render thread 15.4-16.9). At 4K/3x (k1_4k; its fps was taken while
a build ran - CPU-starved, ignore the fps) the GPU column is the new number: title 14.8, menu 16.4,
ingame 15.8 ms - so 4K at 120 also needs the GPU work halved, while 4K at 60 fits. Movies: 22.5-30 fps (decode rate,
the movie lane). Loading screens: the render loop reached 147 passes/s (f6 freeze 2).

**iter-466 cp (perf/cp, docs/PERFORMANCE.md 2.6):** shared-memory write tracking (pages are
write-protected before they are hashed, so a clean page is never hashed again), the rebaseline and
texture-mark reuse it allows, and cheaper vertex staging take the CP from ~26 to ~9.6 ms per frame
(ib 8.1-8.5 + swap 1.3) and the render thread from ~14 to ~7.2 ms; ingame 38.0 / 38.3 -> 52.3 /
51.2 fps on the same exe. The CP now idles ~9-10 ms per frame: the frame is the guest render tick
(FGGameRender, ~19 ms of real work on the guest render thread - codegen / native lanes), then the
render thread (7.2) and the GPU (6.4 at 1280x720).

**iter-466 tick (perf/tick, docs/PERFORMANCE.md 2.8):** ISO-volatile guest codegen and the
render tick's platform costs (native-leaf range checks, CS stubs, HLE S6 producers, the guest
memcpy) take the uncapped ingame median from ~75 to ~80 fps (baked d1/d2 80.3 / 78.2, env c1/c2
81.6 / 83.4); the render thread's work without waits 13.0 -> 10.6 ms (profiled pair). The CP no
longer idles (ib ~10 + swap ~1.5 ms per frame): it is the wall again, and 120 needs both the CP
and the guest tick under 8.3 ms.

**iter-466 cp round 2 (perf/cp2, docs/PERFORMANCE.md 2.9):** physwin answers memoised across
drains, changed-only constant blocks (Xenia up_to_date), shader-load / shader-find memos, lean
per-request and per-draw bookkeeping: the CP's ib + swap drops from ~10.4 to 7.3-8.2 ms per frame and
it waits ~2.3 ms (same exe, uncapped: ingame 92.4 / 95.3 -> 93.0 / 101.5 fps). The guest render
tick (~10 ms) is the frame again; 120 needs it under 8.3 ms.

## 8. Open

- Arena/combat VFX and weather under a freeze. (Water under a freeze: closed - the sea is on the
  wall-clock shader time 0x29, which the oracle holds since f2b83734; sgd6b, section 6.)
- A real 120 Hz output. **120 Hz pacing is unproven until a >= 120 Hz monitor is on the PC** (the timing
  machine has only the 60 Hz 4K TV; ruling 2026-10-18). The ROG Ally's 120 Hz panel is the 120 Hz
  BEHAVIOUR row (sim clock, VFX, freezes), labelled non-timing. Borderless/windowed present at the
  display's rate with sync 1; exclusive mode asks for a refresh that carries the target
  (`exclusiveRefreshHz`, `[DISPLAY] exclusive WxH @ Hz`).
- Eye adaptation: corrected and baked (2026-10-18, "Acceptance and bake" below). Not measured: the
  same transient against live Xenia at 30 Hz (our 30 Hz arm already follows the shader's law exactly).

## 9. The player setting (PORT FEATURE tier)

- **Key:** `target_fps` in `<save root>/settings/display.ini` (the save root is the xam_content
  root, next to `game_options.ini`; docs/CUSTOM_FRONTEND.md section 8 and 9.3). The same file
  holds the display settings of the resolution lane (render/resscale: `res_scale`,
  `display_mode`, window/output size); each feature reads only its own keys. Values:
  `monitor` (display refresh, vsync), `30`, `60`, `120`, any `<N>`, `uncapped`, `off`.
- **Precedence** (kernel/highfps.cpp `resolveMode`, difftest C13-C18): `SR_HIGHFPS` (env, any
  value, `=0` is the A/B off switch) > an automated roll (`SR_INSTANCE_ID` set, or
  `SR_PORT_FEATURES=0`): off, and the player's file is not even opened > the player's
  `target_fps` > `kPlayerDefault`.
- **Default:** iter-465 polish: `kPlayerDefault` = `"monitor"` (the display's refresh) with
  **VSYNC ON**. The player lane's `"120"` tore on the owner's 60 Hz 4K TV (a target above the
  refresh meant sync 0 with tearing). The menu offers 120 / UNLIMITED and VSYNC OFF for players
  who want them. A PORT FEATURE roll (`SR_INSTANCE_ID` + `SR_PORT_FEATURES=1`) resolves like a
  player; any other automated roll stays off. Measured rates per era: docs/PLAYER_BUILD.md.
- **VSYNC** (iter-465 polish): `vsync = on | off` in display.ini (default on). For a
  player-sourced mode (setting or default; an `SR_HIGHFPS` / `SR_HIGHFPS_SYNC` experiment keeps
  its own rule) VSYNC ON = present sync interval 1, never tearing (a target above the refresh is
  shown at the refresh while the render loop keeps its rate), OFF = sync 0 with
  `DXGI_PRESENT_ALLOW_TEARING`. The swap chain is created with ALLOW_TEARING whenever port
  features are on (`swapChainWantsTearingFlag`), so the menu can turn VSYNC off live.
- **Live** (iter-465 polish; the DISPLAY OPTIONS page, docs/CUSTOM_FRONTEND.md 5.9):
  `highfps::applyLive(target, vsync)` re-resolves the config and applies it while the game runs:
  the host present sync / tearing (`presentSyncInterval`, `wantTearing` read the live config),
  the emulated vblank (stub's `vdVblankThread` re-rates its clock from `liveVblankHz()`; 0 = the
  console's 60 Hz path) and the render loop: a host worker with its own guest stack
  (GuestCaller) calls the game's own `GameKernelMgr_SetTickRate_sub_82720418` on the
  FGGameRender mgr (`*0x8296D8DC`) - the setter the mgr's ctor uses at boot, under the mgr's CS
  (+0x3C rate, +0x48 period). Never on the vblank or render thread (the setter waits for the
  CS). `difftest --highfps-test` L1-L5: a mgr built by the game's own ctor goes 30 -> 60 ->
  1000 -> 30 Hz with periods 16,666,668 / 1,000,000 / 33,333,334 ns. Only with port features
  on: natural rolls never reach it (no menu).
- **Menu row (iter-465 polish, built):** SETTINGS > DISPLAY OPTIONS > FRAME RATE: 30 (CONSOLE)
  | 60 | 120 | DISPLAY (<refresh> HZ) | UNLIMITED, and VSYNC: ON | OFF (also in game: PAUSE >
  MORE OPTIONS > DISPLAY OPTIONS). Both apply at once (above). The host side is ready (kernel/highfps.hpp):
  `playerTargetFps()` reads the stored value, `setPlayerTargetFps(v)` validates it
  (`validTargetFps`: monitor | uncapped | off | 0 | an integer-or-decimal 1..1000) and rewrites
  `settings/display.ini` atomically through `withTargetFps`, which keeps every other line (the
  resolution lane's keys, comments, the file's own line ending) byte for byte (difftest
  C22-C27). The bridge command (e.g. `SR_SetDisplay target_fps <v>`, in the style of the fe
  lane's `SR_SetModOption`) belongs to the menu lane. (iter-465: the page calls the setters
  directly through the host-page bridge, and the value applies live.)

## Eye-adaptation rate: outstanding fidelity correction (2026-10-11)

The paired diagnostic probes used snapshot `365d9db54be5-forcing_ledgerw`, private copies of the
`post_warworld` dev-tool-derived save, `SR_DRAW_LEDGER_WINDOW=60`, live PS hash
`040D6633B0AA5D6D`, and registers `4400:4` (PS c0). Diagnostic logging is required (`SR_LOG=dev`);
the player profile suppresses the windowed ledger. No timed or raw present dumps ran during these measurements.

| mode | presents / 60 s | measured presents/s | selected draw/resolve packets per present | c0.x |
|---|---:|---:|---:|---:|
| `SR_HIGHFPS=0` | 1,666 | 27.781 | 2 | exactly 0.25 throughout |
| `SR_HIGHFPS=uncapped` | 7,673 | 127.888 | 2 | exactly 0.25 throughout |

Every present selected the adaptation shader; all four c0 words were uniform within each present.
Both runs reached gameplay and had no GPU-hang trigger. These are diagnostic save-started runs,
not a natural whole-game baseline.

The cached shader's DXIL samples current luminance and history, then computes
`history + (current - history) * (1 - exp2(-0.8743894696235657 * c0.x))`
(coefficient decoded from `adaptation_dxil.txt`; ENGINE_MAP 6g.8d).
The constant is not proportional to the render interval in these observations.
Assuming one history update per present, as in the examined command graphs,
its feedback repeats about 4.603 times faster in wall time in the uncapped probe.
This is a live frame-rate fidelity issue,
separate from the wall-clock shader time 0x29 hold. It must be addressed before treating a freeze-only
adaptation hold as the remaining brightness fix. The SDK upload path is identified below; the
timing correction is experimental, default OFF and not accepted for runtime fidelity.

Evidence: forcing lane `archive/v1_codex_resume_20261011/adaptation_summary.json`,
`adaptation_rolls.json`, `adapt_diag30_window.log`, `adapt_diagu_window.log`, and `adaptation_dxil.txt`.
The earlier `cadapt30` timed out before S20, and `cadaptu` selected no draws with the carved-shader
filter `4F096954B6CE27B0+AD22E52534231B13`; those runs do not answer the adaptation timing question.

### SDK source and shader association (2026-10-11)

Observe-only, independently watched three-second Frida probes on snapshot
`9e002ceb08d1-release-foundation` observed SDK PS constant setter `820F4A30` receiving
one float4 from `4002F170`: `[0.25, 0, 0, 0]`, start register 0, count 1.
Exact snapshot/PDB symbolization identifies its caller as `__imp__sub_823EAE38`,
the compiled D3DX effect-block upload. The recorded host return RVA is `19DBB72`;
return-minus-one is a symbol lookup byte, not an instruction-start claim. The
guest LR is stale under skip_lr codegen and is not used for attribution.

Probe 5 captured the PS object at device+0x3080 (`4001D270`) at setter entry.
Its 120 original BE ucode bytes at `BDF81040` have XXH3 `040D6633B0AA5D6D`
and SHA256 `3d87b9a25e49665e74f84b5a70949f1f34e0d5e072b0fc0cda606989901288e9`,
exactly equal to the archived adaptation shader. Astra independently checked the
bytes and hashes: PASS for this bounded association. The archived container's
constant table names c0 `timeScale`; its filename hash is not the runtime key.

Probe 6 additionally observed the same-thread effect/block association at SDK
entry (`4005A600` / `40059F58`), with one float register. Record `40046330` maps
the observed c0 source to a raw 16-byte source at `4004AB60`
(flags `0x18E40`, raw-copy bit `0x200`). This probe omitted the parameter-name
read for raw-copy records. The flag identifies the copy path; it does not prove
an immutable literal or absence of a parameter object. This observes a compiled mapping; it does not
prove that the dirty comparison triggered or that a marshal/copy ran on this call.

Both runs have helper / runner / slot release exit 0, canonical scenario SUCCESS,
no timeout, read error or cap hit, and hook / script / session cleanup. They used
private save copies with frame dumping off. Shader bytes and derived evidence
remain in ignored `archive/v1_release_codex_20261011/`: `publisher_probe5.json`,
`publisher_shader_identity5.json`, `publisher_probe6.json`,
`publisher_shader_identity6.json`, the corresponding watchdog receipts and
`publisher_symbols.json`. These observations do not close last-writer, completed
upload, gameplay timing, whole-game correctness or Native0 acceptance.

Probe 9 recovered the original named float-scalar `timeScale` parameter through
the raw assignment descriptor's single 24-byte dependency. Its original value
pointer, descriptor value pointer, descriptor+88 and observed raw source all
match (`4004AB60` in that run); copied fields match before name decoding.
The original parameter was `40020774`, root index 5. Astra PASS for this bounded
chain, with helper/runner/release exit 0, scenario SUCCESS, zero errors/skips/caps
and complete hook/script/session cleanup. These heap addresses are run identities,
not stable hooks. Probe 10 subsequently observes 64 completed selected SetFloat
calls and resolves the exact snapshot/PDB caller to the render-script interpreter
`8277B998`. All retained inputs are 0.25 with result 0 and unchanged source words;
an equal-value early return is possible. The intentional cap retains 64 of 266
matching calls. Compiler exports establish the live PPC F1 offset; mock fixture
offsets are not live ABI evidence.

Offline command-graph analysis subsequently separates the two selected packets:
Xenia FMV frame draw 111 runs the adaptation shader and draw 112 resolves to its
history texture `1EEF6000`; archived native gameplay draw 2999 shades and draw 3000
resolves to its history. Each captured frame therefore contains one shaded update
and one copy, not two shader evaluations. The original constants differ between
those captured scenes (`0.24749997` and `0.30000001` respectively), so the current
quarter-vector observation must not be treated as a universal authored literal.
`adaptation_graph.json` retains the input hashes, modes, texture bases and targets;
it observes recorded commands, not GPU-resolved pixels or uncaptured whole-game
cadence. Timing design still needs the live render interval, pause/load/live-rate
semantics, one-time application to each history update and a deterministic replay
contract. The implementation below remains experimental.

### Experimental render-interval correction: producer traced, acceptance pending (2026-10-11)

The retail render script passes authored `xExposureTimeScale` to the shader's
`timeScale`. Zone configuration contains the response controls; the engine
linearly blends those values and copies named vectors into script variables.
IDA verifies that parsing, blending, copying and scalar application do not
multiply by render dt. The effect's history blend applies the control once per
evaluation. Astra PASS for this static derivation: it supports an authored,
zone-blended response control, rather than automatically measured frame time.
Private source data remains outside Git; `hdr_units_derivation.md` records the
source identities and the corresponding IDA and exact PDB evidence.

Multiplying by `30 * render_dt` is an explicit HFR normalization to the retail
30-evaluations/second response. It depends on successful history-update cadence;
the shader's literal 30 alone does not prove that cadence. Probe 13 hooks the exact
compiled interpreter body resolved from the matching PDB. Its first selected
entry snapshots the complete 230-command vector and identifies one variable
command: index 119, variable 18, `xExposureTimeScale`, value 0.25. All 64 retained
scalar inputs match this snapshot. Later entries reference that snapshot; this
does not prove the active command or a fresh variable value at every entry.
Equal-value early returns do not prove a constant write. Astra passes this bounded
source link; failed probes 11 and 12 remain preserved. Localized caller registers
alone cannot identify the command.
Archived values `0.24749997` and `0.30000001` must not be presented as proved clock
products. The helper's `authored` name is not itself evidence of the contract.

`SR_HDR_ADAPT_DT=1` enables the default-OFF implementation in
`kernel/hdr_adaptation.cpp`. It snapshots actual render dt and the live HFR mode
inside `FSGameRenderMain_Tick`, with nested scopes restored on return. It changes
only c0.x after the synchronous PS constant setter: `authored * 30 * render_dt`.
The authored raw/packed buffers, yzw, dirty mask and PPC outputs remain unchanged.
The exact 120-byte shader hash, active compiled-effect context, device, packed
source, register count, shader reference and c0 record must match. Normal engine
N1 uploads outside that context remain unchanged on both direct and fallback paths.
Complete alias-normalized source/destination overlap is rejected; shader identity
is read without a pointer cache, including after object reuse. Metadata uses
fault-free OS copies. Invalid dt/results leave authored behavior unchanged; valid
zero dt gives zero feedback, and valid large intervals are not arbitrarily clamped.
This does not infer pause or a diagnostic simulation hold from an unchanged stamp.

The observed SDK candidates occur within 87 consecutive render ordinals at the
console-rate setting (median 33 ms) and 96 consecutive uncapped ordinals (median
9 ms). All 87 console-rate observations are tick-active; the first uncapped sample
is inactive and the remaining 95 are active. Attachment mid-tick is a possible
explanation, not a proved cause. Instrumentation perturbs timing; these are bounded
association results, not pacing or complete history-update coverage. Astra reviewed
the shader hashes, raw quarter value, intervals and independent cleanup receipts.

`build/difftest.exe --hdr-adaptation-test <private-120-byte-ucode>` exercises the
real SDK wrapper in separate gate/native/fallback/check processes. Tests include
independent numerical endpoints, authored values 0.2475/0.25/0.3, repeated uploads,
source/yzw/dirty/register preservation, scope nesting, mismatched effect metadata,
shader reuse, unreadable metadata, alias overlap and live-mode transitions. No game
shader bytes are embedded in the test source. Six fresh gate/native/fallback/check
processes pass 80 checks each (480 total); `hdr_validated_matrix.json` records the
exact gates and binary/source hashes. One truncated log is retained and superseded
by a complete direct stdout/stderr capture. Build and unit checks do not establish
fidelity. Matched 30/60/120 Hz luminance transitions, first-frame/loading/
pause/resume behavior, complete history-update coverage and drawn-world gates are
still required before default enabling or Native0 acceptance.

The experimental snapshot `7e0d02150c59` replays all nine historical fixtures
byte-identically to foundation `9e002ceb08d1`, with the gate set and HFR=120.
Replay has no live SDK render-tick/effect path, so this is backend regression
evidence only; it cannot validate the incoming units or corrected gain.

A background private-save live 120 Hz diagnostic completed with runner/release
exit 0, scenario SUCCESS and zero GPU-hang or abort triggers. The exact shader
and compiled-effect checks selected uploads without filtering their input value.
Its 24 rate-limited log samples include incoming 0.1, 0.1025, 0.24999997 and 0.25;
all match the implemented formula, through correction count 16,384. This observes
application of the experiment, not acceptance of its semantics. No visual frames
were captured. `hdr_live_120_analysis.json` retains the exact log hash, samples,
health and process-cleanup receipts. Astra passes the subsequent static producer
derivation and bounded Probe 10 receipt; live command linkage, complete cadence
coverage and luminance-transition acceptance remain NEEDS WORK. The default-OFF
engineering checkpoint remains valid.

### Acceptance and bake (2026-10-18, step 4 item X2)

`SR_HDR_ADAPT_DT` is **default ON** since this bake (`=0` keeps the A/B). Lane D's set `x2set5`
(PC, one QUIET_WINDOW hold; NOTES and evidence in `_worktrees/s4-timing/archive/step4_claude_20261018/timing/`)
measured the adaptation in wall time, pre-registered and graded by `tools/perf/adapt_curve.py freeze`.

- **Instrument.** `SR_PRESENT_LUMA=1` (`gfx/present_luma.cpp`, default OFF, observe-only): every present's
  mean luminance of 64 back-buffer rows, copied without a GPU wait (an 8-slot readback ring), so the
  uncapped frame rate under test is not slowed the way a present dump slows it. Checked against
  every-present BMP dumps: 60 of 60 equal within 5e-5 levels at one present offset, 0 skipped.
- **Stimulus.** A moving scene mixes content with exposure (the title's fade-in and camera cuts did not
  repeat closely enough at this time scale: two earlier sets were inconclusive). Under `SR_DIAG_SIM_FREEZE`
  with `SR_DIAG_SIM_FREEZE_ADAPT=0` the drawn scene is frozen while the adaptation runs, so inside a hold
  the presented luminance changes only by the exposure history relaxing toward the frozen scene: a pure
  adaptation transient. Holds of 1.5 s every 3 s on the title; each hold is fitted from 3 presents
  after the render loop's time (render mgr +0x50, recorded per present) stops, i.e. once the uncapped
  render has caught up with the held sim.
- **Result** (median tau over gradable holds, 2 rolls per arm):

| arm | tau | presents/s | law |
|---|---|---|---|
| the game's 30 Hz, `=0` | 0.2199 s | 30.0 | 1 / (30.0 x 0.8744 x 0.25 x ln 2) = 0.2200 s |
| 30 Hz, `=1` (a no-op by construction) | 0.2181 s | 30.0 | the same |
| uncapped, `=0` | 0.0717 s | 93.8 | 3.07x faster than 30 Hz = the present-rate ratio 3.13 |
| uncapped, `=1` | 0.2199 s | 89.3 | equal to 30 Hz (ratio 1.000) |

- **Semantics.** dt is the render loop's own +0x84 (elapsed time on the GameKernel 1 ms clock), so the
  correction keeps adapting while the sim is paused or held, as the console's 30 Hz render does; a long
  frame (a load hitch) takes one proportionally larger step (no clamp: the same wall-time response);
  a zero dt takes no step. At the game's own 30 Hz the correction is not applied at all.
- **Checks of the bake:** the difftest (`--hdr-adaptation-test`, gate unset / 0 / 1), replay 9/9
  (backend regression only: replay has no live SDK render tick), the flicker gate.
- **Grader review (Astra, 2026-10-18).** No P1. Since that review the grader also checks each arm's cadence
  (the uncapped arms at >= 2.5x and the 30 Hz control within 15 % of A's presents/s, before any tau is graded),
  drops any hold whose fit window is missing a present (consecutive `pn`, with no rise in `skipped`), and returns
  INCONCLUSIVE when a requested anchor era is missing (instead of grading every hold). `x2set5` still recomputes
  ACCEPT with the same taus, 0 discontinuous holds and cadence C/A 3.13, D/A 2.98, B/A 1.00
  (`x2set5/freeze_recheck_astra/`).
