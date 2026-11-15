# MIGRATION: API-boundary HLE renderer (the UnleashedRecomp model, done right)

> **SUPERSEDED AS THE MAIN TRACK — user decision 2026-09-29.** The PM4 command
> stream is the draw path again; this HLE layer is kept as the observe / label /
> mod layer, not the renderer. Current working doc: `docs/RENDERER_PM4_HYBRID.md`.
> This file stays the record of the HLE hook contracts and history below.

**USER DECISION 2026-08-02: migrate.** Direct quote of the standing rule: the
official UnleashedRecomp repo is the reference — *follow it structurally*, not
as inspiration. When our renderer deviates from `UnleashedRecomp/gpu/video.cpp`,
the deviation is the bug until proven otherwise
(memory: `feedback_stick_to_reference_repo`).

## Why (one paragraph of history)

The 2026-07-30 pivot said "UnleashedRecomp model", but the implementation
hooked one layer BELOW the D3D9 API — the ring/emit internals
(`EmitDrawVertices`, resolve emit, VdSwap) — and then had to *infer* frame
boundaries, clears, resolve semantics, texture contents, and the present
source. Every artifact class of iter-437..439 (half-screen seams, maroon
flashes, torn title, queue-full wedges, bind-overlap storms, phantom binds)
was an inference failure. The API layer states all of these directly.

## The reference model (verified against the official source, 2026-08-02)

Source: `github.com/hedge-dev/UnleashedRecomp`, `UnleashedRecomp/gpu/video.cpp`
(7,882 lines; local clone used for study). Key structure:

1. **Hook surface = the D3D9 API, ~30 functions** (video.cpp:7798-7881):
   CreateDevice; CreateTexture/VertexBuffer/IndexBuffer/Surface;
   Lock/Unlock{TextureRect,VertexBuffer,IndexBuffer}; Get*Desc;
   GetVertexDeclaration/HashVertexDeclaration/CreateVertexDeclaration/
   SetVertexDeclaration; Create/SetVertexShader; Create/SetPixelShader;
   SetRenderTarget/SetDepthStencilSurface; **Clear**; SetViewport; SetTexture;
   SetScissorRect; DrawPrimitive/DrawIndexedPrimitive/DrawPrimitiveUP;
   SetStreamSource/SetIndices; StretchRect; GetBackBuffer; **Present**;
   D3DXFillTexture etc.; plus a handful of GUEST_FUNCTION_STUBs for
   known-buggy title functions.
2. **One unbounded typed queue** — `moodycamel::BlockingConcurrentQueue
   <RenderCommand>` (video.cpp:1006). RenderCommandType (video.cpp:811) is a
   ~30-entry enum mirroring the API. **No capacity cap → no mid-frame splits,
   no pushBlocking stalls, ever.**
3. **One render thread** consumes commands, translates 1:1 onto their RHI
   (D3D12/Vulkan) with a dirty-state shadow flushed at draw time.
4. **Present is the guest's own call** (video.cpp:2791 `Video::Present`):
   enqueue ExecutePendingStretchRectCommands + ExecuteCommandList, **block the
   guest thread** until the render thread executed the list
   (`g_executedCommandList.wait`), present the swap chain, rotate per-frame
   fences/allocators, reset upload allocators, optional FPS pacing. Guest-
   driven frame boundary + natural backpressure; a frame can never be split
   or outrun.
5. **Clear is the guest's own call.** The host NEVER invents clears.
6. **Resources are guest-created objects** (CreateTexture → GuestTexture;
   Lock/Unlock carry the bytes). The host never scans guest memory to guess
   texture contents, and eviction/overlap heuristics don't exist.
7. **StretchRect** is the API-level resolve (RT→texture copy), executed as
   pending commands at Present.
8. Movies: picture data arrives through an API-adjacent title function
   (MakePictureData hook) that builds a GuestTexture — not through memory
   scraping.

## What SR already has that maps

- Same statically-linked D3D9 library, `0x820F0000..0x82110000` region.
  Known anchors (ENGINE_MAP §6d/h): SetVertexShader `sub_820F5218`,
  SetPixelShader `sub_820F4E88`, CreateVertexShader `sub_820F5148`,
  CreatePixelShader `sub_820F4D90`, GetFunction pair `sub_820F4CE8/4C90`,
  SetStreamSource `sub_820F2A50` (**iter-440 correction** — was misnamed
  SetRenderTargetSurfaceTile), SetTextureFetchConstant
  `sub_82100310`, GpuCommandBufferFlush `sub_820FD8C0`, fence
  `sub_820FDD90/DE38`; emit internals we currently hook:
  `sub_820FBBF8` (DrawVertices emit), `sub_820FC000` (DrawIndexedVertices
  emit), `sub_820FB6E8` (inline/UP emit), resolve emit `sub_8210C5F8`.
- The API functions are the *callers* of those internals — Phase 0 is a
  caller walk, not a blind search.
- Keep: XenosRecomp shader cache (297 DXIL), texture format decode, the
  D3D12 device/swap-chain scaffolding, guest-thread bridging, all
  iter-439 host-bug fixes (hleAppend, THREAD_FATAL minidump, div guards).

## Phases

**Phase 0 — map the API surface (investigation, no behavior change).**

*Findings so far (2026-08-02, session 1):*
- **The draw emits ARE the API layer on 360.** All callers of
  sub_820FBBF8/820FC000/820FB6E8 are TITLE/ENGINE code (0x822F/0x8235
  YUV/0x82473 presenter/0x82612-13 utility cluster/0x826E4
  GfxDrawVertexArray/0x82753-54 render-script passes) — there is no thicker
  wrapper above them; Unleashed's hooked "DrawPrimitive" is this same layer
  in their binary. The bypass risk is RESOLVED: hooking these three (already
  done) IS the API draw hook. The migration's missing piece is the REST of
  the surface + the downstream model, not deeper draw hooks.
- **CORRECTION (session 2): the 0x820BDC00 table is the XEX `.pdata`
  unwind table, not a D3D9 registry.** Full dump from the golden dump
  (`docs/sdk_pdata_table.csv`): 21,837 records, 0x820BDC00..0x820E8668,
  zero-terminated; record = {BE fnPtr, meta = flags<<30 |
  lengthWords<<8 | prologByte}. Decode PROVEN: zero overlaps across all
  21,837 records (fn+len never crosses the next fn). It enumerates every
  non-leaf function in 0x820F0000..0x8290F5B8 — SDK *and* title code —
  so "all our anchors are in it" was true but meant nothing D3D9-specific.
  Gaps between records = leaf functions without unwind info.
- **The API surface = the externally-called subset (session 2).** Ghidra
  xref scan over 0x820F0000..0x82116000: 802 functions, of which **218
  are called from outside the library** — those are the API entry points
  (title code can only call documented API). Clusters: ~60 tiny per-state
  setters 0x820F04E8..0x820F1D30 (SetRenderState_*/SetSamplerState_*
  family); RT/surface 0x820F27xx..0x820F2Fxx (0x820F2A50 ext=34 =
  SetStreamSource, corrected); hot pipeline state 0x820F3D28 (ext=35) / 0x820F3DA0
  (ext=102, hottest) / 0x820F3ED8/3FA0/4000 (15/21/17); shader-constant
  family 0x820F4920 (ext=58) / 0x820F4A30 (ext=30) / 0x820F4B40..4C40;
  known shader create/set 0x820F4C90..0x820F5218; draw emits (known);
  swap 0x820FDE38 whose ONLY external caller 0x824735D0 = the title's
  presenter (the Present hook point lead); resource/create/lock families
  0x820FE3xx..0x82102Dxx (0x827B7xxx movie-player callers → the
  MakePictureData analogue lead).
- **The reference does NOT hook per-state setters by address**
  (video.cpp:1250-1268, 2125-2138): the guest device struct carries
  `setRenderStateFunctions[]`/`setSamplerStateFunctions[]` pointer
  tables; Unleashed replaces exactly 17 render-state slots with host
  bridges and leaves sampler setters as guest code (state read from the
  device struct at draw flush). SR's title calls the setters via direct
  static calls (Ghidra shows call refs), so kernel strong-defs work for
  us — but the same lesson applies: do NOT name/hook all ~60 setters for
  first light; capture state from the device struct / a small hooked set.

*Findings (session 2, continued):*
- **The render-state + sampler-state setter tables are STATIC DATA — the
  whole family is named mechanically** (`docs/d3d_renderstate_table.csv`).
  At **0x8295BF10**: 97 records, 0xC stride, `{setterFnA, setterFnB,
  defaultValue}`, indexed by **D3DRS/4** (state = 4×index, states 0..384;
  states 0..36 = unimplemented pair {0x8267D338, 0x8231C140}). Identity
  proven by default-value fingerprints against the reference's enum
  (video.h:318): ZENABLE=40 def 1, ZFUNC=44 def 3 (Xenon LESSEQUAL),
  CULLMODE=56 def 6 (=their D3DCULL_CCW), SRCBLEND=1/DESTBLEND=0,
  ALPHAFUNC def 7 (ALWAYS), BLENDFACTOR def 0xFFFFFFFF, COLORWRITEENABLE
  0..3 def 0xF, POINTSIZE_MAX def 64.0f. At **0x8295C3A0**: the sampler
  table, **exactly 20 records = Unleashed's setSamplerStateFunctions[0x14]**;
  known members: idx8=SetSamplerMinMipLevel(820F2370/2300),
  idx9=SetSamplerFilterState(820F21A8/2140), idx13=SetSamplerMaxMipLevel
  (820F23F0/2380, default 13 = log2(8192)). **Column A = GETTER, column
  B = SETTER** (proven by decompile: the CULLMODE pair reads/writes
  dev+0x28C8 bits 0..2; the setter also marks dirty dev+0x10 |= 0x40) —
  so render/sampler state lives in a guest-device shadow block around
  dev+0x28xx, same model as the reference's draw-time state flush.
- **Shader-constant family fully identified + renamed in IDA** (subagent,
  all HIGH): SetVertexShaderConstantF sub_820F4920 / SetPixelShaderConstantF
  sub_820F4A30 / SetVertexShaderConstantB sub_820F4B40 /
  SetPixelShaderConstantB sub_820F4B98 / SetVertexShaderConstantI
  sub_820F4BF0 / SetPixelShaderConstantI sub_820F4C40. **Architecture
  answer: constants NEVER touch the ring** — pure CPU writes into the
  guest device struct shadow: VS floats dev+0x700 (c0..255), PS floats
  dev+0x1700 (ALU 256..511), bool masks dev+0x2700/+0x2710, loop ints
  dev+0x2720/+0x2760; dirty qwords dev+0x00 (VS-F), +0x08 (PS-F), +0x10
  (general/binds), +0x18 bit1 (flow-control). Set*Shader stores at
  dev+0x3080/0x3084 and **replays the shader's embedded default-state
  block into the shadow** (HLE caveat: watching setters alone misses
  those defaults). Shader objects: pixel hdr at obj+40, vertex at
  obj+872; ucode physical ptr obj+24/obj+32; GetFunction pair verified.
  => Our HLE captures constants at the API boundary exactly like the
  reference (hook the six setters), not by snooping shadow or ring.

*Findings (session 2, resource/movie cluster — subagent, renamed in IDA):*
- **Clear = `D3DDeviceClear_sub_82101998`** (dev, count, rects, flags,
  color, z, stencil) — vupkd3d color unpack + per-rect tiled clear.
- **VB/IB family:** Create `sub_820F3ED8` (sizeBytes, usage → 32B hdr,
  phys alloc), Lock `sub_820F3FA0` (offset, size, flags → raw pointer,
  360-style), Unlock `sub_820F4000`, GetDesc `sub_820F4010`; IB create
  `sub_820F4040` (fmt<<29 into hdr word 0).
- **Texture family:** Create-with-backing `sub_820FFF08` (w,h,depth,
  levels,usage,fmtDword,?,resType → 52B hdr + data + mip tail; wraps the
  fetch-constant builder `sub_820FF668` = XGSetTextureHeader core);
  LockRect `sub_820FFCB0` (tex, level, {Pitch,pBits}* out, POINT*,
  flags); UnlockRect `sub_820FFCC8` (cache restore); GetSurfaceLevel
  `sub_820FFBE0`; GetLevelDesc thunk `0x820FFCE0`; surface Lock/Unlock
  `sub_82100228`/`sub_821002F8`; volume LockBox `sub_821002D8`;
  GetLevelCount `sub_820FFBD0`.
- **SetStreamSource = `sub_820F2A50`** (dev, stream, vb, OffsetInBytes,
  Stride, dirtyBit) — **ENGINE_MAP misname corrected** (was
  "SetRenderTargetSurfaceTile"); stream slots dev[4*(3047+s)] = vertex
  fetch constants, stride/4 byte at dev+12256+s.
- **GetDeviceCaps `sub_82101FD0`** (memcpy 304B from 0x820BA540) and
  **GetDisplayMode `sub_82101FE8`**; Query/predicated-tiling family
  named (0x82102010..0x82102DC8 = predication push/pop +
  CreateQuery/Release/Issue/GetData — occlusion, not resources);
  0x820F9xxx tiny-fn family = the runtime shader ASSEMBLER context (used
  by Pixel/VertexShaderCreateFromMicrocode 0x827BD7C0/0x827BDB18).
- **🎬 THE MOVIE PIXEL PATH (MakePictureData analogue), fully mapped:**
  `MoviePlayerRenderStateTick_sub_8235F488` (state 1) creates THREE
  CPU-visible planar L8 textures via `sub_820FFF08` — Y at (w,h), U+V at
  (w/2,h/2), fmt dword 0x28000002 (GPUTEXTUREFORMAT_8) — into globals
  0x829761E4/E8/EC, plus a 144B quad VB (4×36B). Each frame
  `RenderYuvVideoFrameTec0_sub_8235CA48`: LockRect all three planes →
  hands {pBitsY/U/V, sizes, pitches} to the WMV decoder
  `sub_824707B0(player, req)` which **decodes directly into texture
  memory** → UnlockRect ×3 → SetTextureFetchConstant slots 0/1/2 →
  SetStreamSource(quad, stride 0x24) → QUADLIST draw through effect
  "tec0" (the game's own YUV→RGB shader). (Second, direct presenter
  path `sub_82473208` uses stride 0x14 — see ENGINE_MAP §movie table.)
  => movie HLE hook set = the Create/Lock/Unlock texture trio; the
  compositing is the game's own draw. Decoder API: open `sub_824715F0`,
  dims `sub_82470898`, decode `sub_824707B0`, status `sub_824709F0`.

*Findings (session 2, geometry-state cluster — subagent, renamed in IDA;
independently CROSS-VALIDATES the resource cluster on 820F2A50):*
- **Two more misnames corrected:** `SetRenderTarget =
  sub_820F2CA0` (was "SetTexture"; RT slots dev+12168+4*i, index 0..3,
  RT0-null → depth-tile fallback) and `SetDepthStencilSurface =
  sub_820F2FD0` (was "SetTextureSlot4"; dev+12184, DB_DEPTH_INFO regs).
  **The real SetTexture API boundary = SetTextureFetchConstant
  sub_82100310** (device, sampler, texture, dirtyMask ROR(1,21+s); tex
  table dev+12272+4*s, 24B tfetch constant at dev+1024+24*s).
- **Named:** SetIndices `sub_820F2C08` (dev+12164; draw-packet builder
  reads it directly), SetScissorRect `sub_820F2720` (RECT at dev+12400 →
  PA_SC words), SetViewport `sub_820F2810` (D3DVIEWPORT9 at
  dev+12376..96 → PA_CL_VPORT xs/xo/ys/yo/zs/zo), SetVertexDeclaration
  `sub_820F5438` (dev+11536, dirty bit 13), SetFVF `sub_820F53E8`,
  CreateVertexDeclaration `sub_820F5490` (12B X360 D3DVERTEXELEMENT9
  until Stream==0xFF), IB Lock/Unlock/GetDesc `sub_820F40F0/4150/4160`,
  Device AddRef/Release `sub_820F35A8/35C0`,
  Acquire/ReleaseThreadOwnership `sub_820F3618/3658`, Direct3D_GetDevice
  `sub_820F3E20` (loads *VdGlobalDevice), EndVertices `sub_820FBBA0`,
  DrawVerticesUP `sub_820FBBB0` (Begin+memcpy+End).
- **The ext=102 hottest fn = D3DResourceRelease `sub_820F3DA0`** (atomic
  refcount dec + destroy dispatch); `sub_820F3D28` = AddRef. Caller-count
  heat ranked lifetime management, not state setting.
- **HLE contract gotchas (from decompiles):** (1) this XDK's inline API
  shims append a TRAILING DIRTY-MASK ARG to setters (SetTexture's
  ROR(1,21+s), SetStreamSource's 0x1000/0x2000) — hooks must accept it;
  (2) SetIndices/SetStreamSource set NO gpu dirty bits for the binding
  itself (IB read from device struct at draw; VB hdr dwords 6-7 are
  preformatted vfetch constants) while STRIDE changes dirty the VERTEX
  SHADER (bit 13 — stride is baked into patched microcode): a D3D12
  backend must treat stride as pipeline-state; (3) key device offsets:
  +12160 FVF, +11536 decl, +12164 IB, +12168..12184 RT0-3+DS,
  +12188+4n stream VBs, +12256+n strides, +12272+4n textures,
  +12416/12420 PS/VS, +1024 fetch-constant file, +40/+13076 ring
  write/reserve, +52 refcount, +10760 owner tid.

*Findings (session 2, frame/Present/Clear cluster — subagent, renamed in IDA):*
- **THE PRESENT HOOK POINT = `D3DDeviceSwap_sub_82112050`** (dev,
  frontBuf surface, srcRect opt → VdSwap). Every present path converges
  there: main renderer via the TWO title wrappers
  `D3DPresentSwapOnly_sub_82611FB8` (EndTiling→InsertFence→present-quad
  →Swap; fb ring unk_82998720, 64B stride, idx dword_82998860 mod
  dword_829293CC) and `D3DPresentResolveThenSwap_sub_82612090`
  (Resolve RT→fb then Swap) — both called ONLY from
  `RenderScriptRunTechnique_sub_8277B998`; movies via
  `MoviePresentLoop_SelfPresent_sub_82481F40` →
  `D3DDeviceBlitAndSwap_sub_82112750`; plus VdInterruptPumpWait's
  internal re-swap. Hook Swap for the Unleashed altitude (or the wrapper
  pair for title altitude — note there are two). **Pacing contracts:**
  Swap blocks on the PREVIOUS frame's fence internally (dev+13520,
  reason-3 wait = one frame in flight); the movie virtual present
  (`MoviePlayerPresentFrame_sub_82473208`, vtbl 0x8204FD50 +0x50) NEVER
  blocks — returns E_PENDING (0x8000000A) at 3 queued swaps, drained by
  swap-callback (vtbl+0x5C, thunk `sub_82471890`).
- **`sub_820FDE38` is NOT swap-exclusive** — it is
  `GpuEmitKickWithCallback` (EndTiling 0x8210DCB4 and EndZPass
  0x8210E2A4 also emit through it). Prior "swap emit" shorthand corrected.
- **Clear needs BOTH hooks:** `D3DDeviceClear_sub_82101998` (Count,
  pRects, Flags, D3DCOLOR, Z, Stencil — exact XDK signature) AND
  `D3DDeviceClearF_sub_82101A58` (Flags incl. 0xF=TARGET0-3
  0x10=ZBUFFER 0x20=STENCIL, pRect, float4 color, Z, Stencil) — the
  title calls both directly. Emission: `ClearImpl_sub_821013D0` →
  `ClearSurface_sub_82100F70` (rect draw); clear-on-resolve arrives via
  resolve flags 0x100/0x200 at `sub_8210C5F8`.
- **Fence family named:** InsertFence `sub_820FDD30` (returns submit
  counter dev+0x2A1C), BlockOnFence `sub_820FDD68`, IsFencePending
  `sub_820FDD90` (**POLARITY CORRECTION vs iter-390: TRUE = pending**;
  ENGINE_MAP + kernel comment fixed; no kernel logic consumed the old
  reading), BlockUntilIdle `sub_820FDCB8`, GpuBlockOnFence
  `sub_820FD9A8` (was "GpuRingReserveSpace" misnomer; reasons 3=swap
  4=idle 5=resource 6=explicit), ResourceBlockUntilNotBusy
  `sub_820FDE08` (fence at res+8, Lock paths), ResourceIsBusy
  `sub_820FDE30`.
- **Tiling family:** BeginTiling `sub_8210D588` (tile rects →
  dev+12480, initial ClearF(0x3F)), EndTiling `sub_8210DA98` (per-tile
  Resolve, auto |0x100/|0x200 on non-final tiles), SetPredication
  `sub_8210D400` (2 bits/tile at dev+12432), Begin/EndZPass
  `sub_8210DF38`/`sub_8210E1A0`.

**PHASE 0 STATUS: the first-light set is FULLY NAMED with verified arg
contracts** (4 subagents, 2026-08-02): Present/Swap ✓, Clear+ClearF ✓,
SetRenderTarget/SetDepthStencilSurface ✓, SetViewport/SetScissorRect ✓,
SetVertexDeclaration/CreateVertexDeclaration/SetFVF ✓,
Set/CreateVertex+PixelShader ✓ (prior), all six Set*ShaderConstant[F/B/I]
✓, SetStreamSource/SetIndices ✓, SetTexture(=SetTextureFetchConstant) ✓,
CreateTexture/VB/IB + Lock/Unlock/GetDesc families ✓, Resolve(+tiling)
✓ (360 has no StretchRect — Resolve is it), fence family ✓. SR uses an
fb RING (unk_82998720) rather than GetBackBuffer. Small leftovers, none
first-light-blocking: per-state stencil/point/wrap + sampler sub-state
names (mechanical via the state tables), CreateDevice/init path (device
reached via Direct3D_GetDevice sub_820F3E20 / VdGlobalDevice import),
A-column getter names.

**Phase 1 — scaffold `gfx/hle_video.{cpp,hpp}`** mirroring video.cpp:
RenderCommandType enum, unbounded queue (mutex+cv deque is fine to start),
render thread owning the existing D3D12 device, guest object structs
(GuestDevice/GuestTexture/GuestSurface/GuestBuffer/GuestVertexDecl) in
guest-visible memory via the existing allocator bridge.

**Phase 2 — hook the API surface** (kernel strong-defs, registered in
OVERRIDES.json as platform-fix), translate 1:1. Movies come through the
API like everything else (the YUV planes are SetTexture'd guest textures).
Gate: `SR_API_HLE=1` opt-in until first light, then default-ON with =0.

**Phase 3 — delete the inference machinery**: drain queue caps + frame
markers + invented clears + resolve readback/texsnap/overlap flushes +
fb-present guessing + PM4 live path (replay fixture stays for texture/
shader regression only).

**Every phase's visual claims go through `tools/verify_run.py` sheets —
eyes on every sheet before any verdict (feedback_verification_scope).**

## PHASE ORDER AMENDED — STAGE A REMOVAL PULLED AHEAD (user decision 2026-08-03, iter-441)

Rationale: with the old renderer still live, sessions kept getting pulled into
patching it (the iter-440k..p FMV dig happened INSTEAD of Phase 1) — the old
path itself is the drift vector. So the deletion component of Phase 3 moved
FIRST, before translation exists. The interim cost is deliberate and recorded
in NATURAL_BASELINE: no live draws reach the screen until Phase 2 first light
(present cadence, boot metrics, and the movie DECODE path are unaffected).

**Removed (iter-441 Stage A):**
- `kernel/hle_d3d9.cpp` DELETED — the emit-hook draw-synth renderer
  (SR_HLE_RENDER) and its 5 overrides (draw emits ×3, resolve emit, +1)
  auto-dropped from OVERRIDES.json by gen_override_manifest.
- Live PM4 draw submission OFF structurally: `Pm4Engine::submitDraws` is a
  per-instance property — the live instance (null_renderer) is command-
  processor SERVICE ONLY (fence writeback, interrupts, health counters);
  the replay fixture keeps submitting. This replaced the env-gated
  SR_HLE_RENDER/SR_PM4_DRAW_OFF suppression, which was instance-blind and
  had been suppressing the REPLAY fixture's draws too since iter-435c.
- Dead globals `g_hleSuppressPm4Draws` / `g_hleShaderFix` deleted.

**Kept (still live):** frame markers + the iter-440c API-present block (the
guest-driven frame boundary — the reference's own model, and the interim
present path until the hle_video render thread takes device ownership);
`kernel/hle_api_observe.cpp`; the PM4 command-processor service; DXIL shader
cache, texture decode, D3D12 device scaffolding; the movie decode path.

## THE OWNERSHIP DECISION — DELEGATE + MIRROR (settled with data, iter-442)

**The trunk question nobody had asked:** the reference is all-or-nothing. Its
`GUEST_FUNCTION_HOOK` expands with **no `__imp__` call** — the title's D3D9 never
runs, so `GuestTexture` *is* the handle and the host owns pitch, staging and
lifetime. **Every one of our hooks delegates** (`__imp__` at each site). We were
about to inherit a model whose every property depends on replacement we do not do.
That failure mode does not crash: it produces wrong pitch / stale planes / wrong
tiling — pixels that look exactly like translation bugs and get attributed to the
translation layer for a full session.

**DECISION: delegate + mirror.** The guest's D3D9 keeps running and stays the owner
of resource memory, pitch, lock counts, refcounts and the swap fence. Our hooks
OBSERVE the call, mirror it into the render queue, and let the guest's own code
execute. The reference remains structural law for the **queue / thread / dispatch /
present-rendezvous** shape — the parts that are about *our* host architecture — and
is explicitly NOT law for **resource ownership**. Measured contract: ENGINE_MAP §6g.2.

Consequences, each measured rather than assumed (`[RESMODEL]`, 48 samples):
- upload at the **guest's** pitch (chroma is 768 for a 640-wide plane, not 640);
- keep a **VA→record map** (pixels live ~15 MB from the 52-byte header);
- movie planes are **linear** — no untiling on this path;
- **no dirty-span shortcut**: the guest's per-lock range reads empty
  (`0xFFFF0000`) at every Lock and every pre-delegate Unlock, so Unlock uploads the
  full locked subresource.

## THE MEASURED CALL STREAM (iter-442 S1 census, one K=1 200 s roll)

Sizing numbers for the queue, from real traffic rather than estimation —
`swap=7500` frames:

| API | total | per frame |
|---|---|---|
| DrawVertices / DrawIndexed / UP | 2.80M / 3.53M / 455K | **905 draws** |
| SetTexture (`sub_82100310`) | 11.31M | 1508 |
| SetStreamSource | 11.08M | 1478 |
| SetIndices | 1.63M | 217 |
| SetRenderTarget / SetDepthStencil | 308K / 107K | 41 / 14 |
| SetViewport / SetScissorRect | 257K / 350K | 34 / 47 |
| Clear / ClearF | 4.5K / 22.4K | 0.6 / 3 |
| CreateIndexBuffer | 7 total | — |

⇒ **~4,250 commands/frame ≈ 136 KB/frame** at our 32-byte command (the reference's is
40 B; ours is smaller because it carries no host pointers yet). The unbounded queue is
right.

**≥2 guest threads produce** (draws on one thread, SetTexture/SetScissorRect also seen
on another). ⚠ **CORRECTION to this doc's first draft**, which called per-draw batching
"mandatory, not an optimisation": that overstated it. Our hooks run *inside* each guest
API call, so our publish order is exactly the guest's call order — and if the guest
serialises its own device (it has `Acquire/ReleaseThreadOwnership sub_820F3618/3658`
available), cross-thread interleaving cannot corrupt ordering at all. **Whether it
actually calls them is UNVERIFIED** — an open question worth one probe before S4.
Batching is therefore justified on lock traffic (~4,250 → ~1,000 acquisitions/frame) and
on per-draw atomicity, not on a proven correctness bug.

Note `setVS=setPS=vbCreate=0` here only because those symbols are already strong-def'd
elsewhere (`cs_timeout_underflow_fix.cpp`, `apt_render_watch.cpp`) — Phase 2 must take
them over at those sites, not add a second definition.

## THE OPEN COMMAND LIST (iter-445 C1-C7, `SR_HLE_FRAME_LIST`)

**THE CONTRACT.** One open window per frame, opened LAZILY by `ensureFrameOpen()`
from whichever of `hle::execute()` / `presentFramebuffer` reaches it first;
exactly ONE Close+Execute per present; `dumpBackBuffer` after Close+Execute and
before Present; one frame in flight via the existing present rendezvous. The
window REQUIRES `SR_HLE_OWN_PRESENT` — with the main thread presenting, a lazy
open on the render thread would Reset the list the main thread is recording into,
so the two gates interlock and log rather than race.

**WHY IT EXISTS.** `execute()` ran with the list CLOSED, so ~4,249 of the frame's
~4,250 commands executed against a list that could not record. MEASURED on the
fixture: **16,316 of 27,055 commands — 60% of the stream — were unrecordable by
construction**, not unimplemented.

**PROVEN, not asserted.** `[FRAME_LIST]` ledger: `delta=(60,60,60)` opens ==
closes == executes == presents, zero violations/resetFails/closeFails/discards.
`windowCmds` **0 → 27,055** is the positive evidence (0 is reachable with the
gate off, so the test can fail). A clear issued from inside `execute()` repaints
**100% of pixels on 40/40 frames** of the geometry trace. Recording into a
deliberately closed list drives violations and debug-layer errors 0 → 2,923.

⛔ **THE BOUNDARY, STATED HONESTLY.** This sequence delivers ONLY the RECORDING
SURFACE — a list that is legally open, correctly barriered and descriptor-heap
bound while `execute()` runs. It does NOT decode the nine blocked commands.
And the reason they are undecoded is **our own deviation, not a fact of nature**:

> The reference decodes them IN THE PRODUCER, on the guest thread where reading
> guest memory is legal, and publishes SCALARS — `ProcClear` takes
> `be<float>* color, double z`; `SetViewport`/`SetScissorRect` read their structs
> field by field. **WE ALREADY DO THAT DECODE AND THROW IT AWAY.**
> `kernel/hle_api_observe.cpp` builds `float rgba[4]` from `ctx.r7` and passes
> `float(ctx.f1.f64)` into `pushGuestClear` — whose sink is the DELETED model's
> queue and is gated OFF by default — and then, **two lines later**, `MIRROR1`
> publishes `c.clear.color = ctx.r7.u32; c.clear.z = 0;`. The Z the guest passed
> in `f1` is discarded into a literal zero. Same story for `ClearF` via
> `guestF32`, and the surface commands already have `dumpSurfaceHeader` decoding
> width/height/format on the producer side.
> **Re-pointing the existing decode at `MIRROR1` is ~2 lines per command. The
> guest VA in the mirror is the BUG TO FIX, not the wall to document.**

Also NOT done here: growing the 4-descriptor RTV heap (`kFrameCount+2`, two owned
by the swapchain) or the 1-descriptor DSV heap, against a measured 16 render
targets and 3 depth surfaces (ENGINE_MAP §6g.4). The render-target subsystem is
the next slice and it needs both.

## PHASE 2 SLICE LOG (what is actually built)

**S1 — census + resource probe (iter-442).** Answered the resource model with
data (§ above, ENGINE_MAP §6g.2) and measured the real call stream.

**S2 — typed commands + mirror (ca01ccf).** `RenderCommand` is a POD tagged
union of named per-command structs, no default member initializers (a union
member cannot carry them, and a defaulted tag turns a corrupt command into a
fabricated frame boundary), `static_assert` trivially-copyable at ≤32 B. All 15
first-light hooks publish behind `SR_HLE_MIRROR`. **Proof: 28,605,233 enqueued /
28,605,232 drained, every per-type count equal to its `[API_OBS]` counter**;
peak depth 136 against ~4,250 cmds/frame.

**S3 — device ownership + the present rendezvous.** Three changes:
1. **The main loop is an EVENT PUMP.** It used to present every iteration by
   scraping the guest framebuffer, on its own cadence — `present` read 11,700
   against 7,500 guest swaps, i.e. **~4,200 host-invented frames per run**. That
   is the "fb-present guessing" Phase 3 lists for deletion; it is gone.
   `present` is now **7,200 = exactly the guest's swap count**.
2. **The render thread owns D3D12.** `main()` still creates the device (it holds
   the HWND) and publishes it atomically; the render thread tolerates a null
   backend, so guest threads that produce before the window exists cannot race
   it. One thread touches D3D12 — the reference's model.
3. **A real rendezvous.** ⚠ The iter-440c present block it replaces was waiting
   on the **retired** `DrawCommandQueue`'s boundary counters — and since
   `Pm4Engine::submitDraws` is false on the live engine, that queue no longer
   receives draws. **The old wait had silently become a no-op that still looked
   like backpressure in the logs.** The guest now blocks until the render thread
   has executed *this* frame's Present, failing OPEN after 200 ms so a stalled
   renderer can never wedge the guest.

   A/B, same build, K=1 200 s each — **OFF:** present/swap 7200/7200, feFrames
   5090, defExec 20336. **ON:** identical 7200/7200, guest waits **489 µs/frame
   average, ZERO timeouts**, feFrames 5014, defExec 20899. Half a millisecond
   per frame buys the console's one-frame-in-flight throttle, so it is baked
   default-ON (`SR_API_PRESENT_BLOCK=0` keeps the A/B).

Queue depth rose 136 → ~1,900 once the render thread did real present work
(it blocks on fences). Bounded and sub-frame, and it independently validates the
S1 decision to snapshot resource bytes on the **producer** side: with a backlog
that deep, a render thread reading guest memory later would read frames the
decoder had already overwritten.

**S4.0 — shader identity (d72dc96).** The one experiment that could have killed
the slice. Both shaders for the movie draw resolve to DXIL already in the cache,
from device-shadow objects reachable at the API boundary alone. Details +
corrections in ENGINE_MAP §6g.3.

**S4.1/S4.2 — the latch and the producer-side snapshot.** Unlock is passed only
a texture handle, so the Lock hook now latches `pitch`/`pBits`/`level` into the
VA→record table; Unlock snapshots the whole locked subresource into a host
arena (48 × 1 MB slots) *before* delegating, and the command carries an arena
offset instead of a guest VA. This is the HARD RULE — the decoder rewrites the
same allocation every frame, and S3 measured queue depth ~1,900, so a render
thread dereferencing the guest pointer later would render a LATER frame.
Measured over 200 s: 2,184 snapshots, 1,023 MB, **peak 3 slots (one movie
frame — 16× headroom), zero rejects**, and `snaps == 3 × movieQuads` exactly.
`[HLE_MIRROR]` still 1:1 on every type including `texUnlock` 2318/2318.

Also landed: the **activity gate** (`[S4_STAGE] GATE=`). Movie decode is still
roll-dependent (the open no-movie class, iter-440q), and on a roll that decoded
nothing there are no Locks at all — so a *correct* renderer draws black. A
first-light verdict is only attributable when the same roll reports
`MOVIE-ACTIVE`. `rejects > 0` (`MOVIE-BUT-ARENA-LAPPED`) means pixel data was
dropped and the darkness IS ours.

**NEXT — ORDER AMENDED: S5 before S4.3–S4.7.** The original plan finished the
movie slice first and built the trace/replay fixture after. That is backwards
now that the snapshot exists. S4.4–S4.7 is five independent failure points
(upload format, PSO, RT binding, vertex data, present copy) which **all fail to
an identical black screen**; debugged against 200 s boots + contact sheets that
is five-plus rolls that each say only "still black". A trace recorded *before*
S4.2 would have been useless — guest pointers with no pixels behind them — but
with the arena it carries the real planes and the real quad, so it replays into
the backend headlessly in seconds. This is the recorded iter-408 lesson
(`feedback_use_the_replay_loop`): a whole session lost debugging the renderer
against boot runs. Same destination, the risky half debugged in seconds.

**S5 — the trace/replay fixture. LANDED.** `SR_HLE_TRACE_REC=<path>` records the
command stream with its plane payloads; `SR_HLE_TRACE_REPLAY=<path>` replays it
through the **same `hle::execute()`** the render thread runs, with the guest
never booting. Wrapper: `tools/hle_replay.ps1` (`-Record` / default replay /
`-Loops 0` for a live window).

Two design properties do the real work:
- **The recorder ARMS on movie activity**, then starts on a Present boundary. A
  no-movie roll therefore produces *no trace* instead of a useless one that
  replays to black and proves nothing.
- **Payloads are hashed at capture and re-checked at replay.** rc=2 means the
  pixels did not round-trip, so any pixel verdict from that run is void. This is
  what stops the fixture from silently becoming a different program than the one
  that ships.

MEASURED: 8 presents captured -> **2,912 records / 5,898,240 payload bytes**, and
that payload is exactly **4 x 1,474,560** = four whole movie frames; `texUnlock`
12, `draws` 4. Replay executes all of it with **hashMismatches=0 in ~110 ms**
(1.1 s wall including process start), against ~200 s for a boot roll. A 3-pass
run returns exact 3x multiples.

⚠ The 8-presents-to-4-movie-frames ratio in this very trace is the **S4.6
hazard, now measured rather than predicted**: the movie advances at roughly half
the present rate, so ~every other Present carries no movie draw and must still
paint the last frame or the screen strobes.

**S6-RT — THE RENDER TARGET SUBSYSTEM (iter-446).** The slice the open command
list existed to unblock. Order: R0 decode → R1/R2 pool → R3/R4 arms → R5 resolve
→ R6 clear.

**R0/R0b — the header decodes** (`b6a4977`). `surface+24` = `RB_SURFACE_INFO`;
there is no height field, so height is derived as
`bytes / (pitch * bpp * samples)`. Full proof + the measured 15-surface table:
ENGINE_MAP §6g.9. `SetViewport`/`SetScissorRect` stopped publishing raw guest
VAs. The trace format gained a CHECKED version (`kTraceVersion=2`): this slice
widened three commands IN PLACE at the same 32 bytes, so a v1 trace would have
loaded and read a guest VA as a viewport X.

**R1/R2/R3/R4 — the pool and the bind arms.** One host resource per guest
surface VA, created lazily from the published header dwords, in **its own
RTV/DSV descriptor heaps** (64/16) rather than by growing the shared ones — the
shared `m_rtvHeap` is exactly `kFrameCount+2` with every slot owned and
`m_dsvHeap` is a single conditionally-created descriptor, so resizing either
means renumbering slots in paths the movie depends on today. RTV/DSV descriptors
are CPU-only, so isolation is free. `SR_HLE_RT_POOL`, default OFF.

MEASURED on the fixture (16-present v2 trace, ~4 s), gate ON vs OFF:

| | OFF | ON |
|---|---|---|
| surfaces created | — | **15** (rtv 13/64, dsv 2/16) |
| refused badHeader / noSlot / createFail | — | **0 / 0 / 0** |
| `setRT` bound | **0** | **189** of 621 |
| `setDS` bound | **0** | **72** of 216 |
| debug layer msgs / err / warn | 0 / 0 / 0 | **0 / 0 / 0** |
| `[FRAME_LIST]` | delta=(16,16,16) v=0 | **identical** |
| present BMPs | 16 | **byte-identical, 16/16** |

The gate can fail (`bound=0` with it off), it does work with it on, the frame
ledger is unchanged, and the frames are pixel-identical — on frames that are
NOT blank (⛔ RETRACTED — `nonBlackPx` is a CONSTANT: the back buffer clears to
(10,10,26) and the test is `px[0]|px[1]|px[2]`, so an EMPTY frame also reports
921600/921600. The byte-identical BMP comparison stands; the "not blank" half
never did. The oracle that actually discriminates is `[PRESENT_SIG] distinct=` /
`fnv=`, proven by draws-on 192 vs draws-off 1), so inertness is not confused with
darkness. The bind shortfall is fully accounted, not a silent refusal:
`rt 189 + ds 72 + null 576 = 837 = setRT 621 + setDS 216`, i.e. every unbound
bind is a genuine NULL surface (MRT teardown, §6g.4 measured it at 65K/run).

⛔ **BOUNDARY.** The pool CREATES and BINDS. Nothing draws to a bound target
yet, no resolve executes, and viewport/scissor are LATCHED not applied — the
draw path still renders through the backbuffer-sized geo target, so applying an
80x16 viewport to it would clip the movie into a corner. `[S6_DROP]` fell from
nine types to three (`SetStreamSource`, `SetIndices`, `EndVertices`); "has an
arm" means DECODED, not EXECUTED.

**R5a — Resolve becomes a command type, and the census reframes R5b.** It was
the ONE D3D9 entry point with no `RenderCommandType` at all. Both ends are keyed
producer-side (source from the device slot array via `Flags & 7`; destination as
the fetch-constant base page, the SAME key `SetTexture` binds by).

MEASURED over a 200 s roll: **105,925 resolves, 14 sources, 16 destinations,
srcPooled = 105,925 (100%)** — R1/R2 already covers the source side completely —
and **destKnown = 0**.

⛔ That zero was CHECKED, not assumed: it is the exact signature of iter-445's
texture-identity bug. Cross-check says the keys agree — **9 of the 16
destinations are keys the guest also BINDS**, in a window with only 12 distinct
bound keys total.

✅ So the finding is structural, not a defect: a resolve destination's pixels are
produced BY THE GPU, so it can never arrive through the upload path — there is
no Lock/Unlock for a render product. **R5b must CREATE the destination and
register it under its bind key**, which is the missing link between the pass
graph and the sampler. Contract: ENGINE_MAP §6g.10.

**R5b — the copy executes.** The destination is CREATED by the resolve (it can
arrive no other way), registered under its bind key, and copied into on the
frame's open list. `SR_HLE_RESOLVE`, default OFF.

MEASURED, 200 s roll: **69,902 copies of 95,655 resolves (73%)**, 14 destination
textures, debug layer 0/0/0, and the accounting is exact —
`69,902 + 3,679 sub-rect + 14,716 sliced + 7,358 depth = 95,655`, with the
`other` bucket equal to `depthSrc` to the unit.

**The slice's metric is `sampled`, not `bindHit`** — registering destinations
raises `bindHit` by construction, so it cannot grade the work. `sampled` counts
the guest binding a render product as a texture: **266,631 times per run**, every
one of which previously resolved to the 1x1 white fallback or stale memory.

⛔ It does NOT put geometry on screen. Bind miss stays ~97.5% because 1,107
distinct keys are bound and only 10-16 are render products — the rest is the
GENERAL TEXTURE gap, a different slice. What this removes is §6g.4's "wrong by
construction" property. Named v1 limits (depth / sliced / sub-rect / format /
MSAA) are refused and counted, never approximated: ENGINE_MAP §6g.11.

**R6 — the guest's own clear executes.** `SR_HLE_GUEST_CLEAR`, default OFF.
Armed in the **ClearF arm and only there** (Clear is a wrapper that tail-calls
it, and our hooks delegate — arming both double-clears). It could not be switched
on before the pool existed: "clear the guest's render target" had nowhere to land
but the backbuffer, which would paint an offscreen pass's clear over the picture.

Two things had to be right first, and neither was:
- **The stencil came from the wrong register.** Both clears take `Z` in `f1`,
  and under the PPC64 ABI that STILL consumes an integer slot — so `Clear` skips
  r8 (stencil is r9) and `ClearF` skips r7 (stencil is **r8**). We published r7.
- **The flags are the 360 layout**: bits 0-3 = TARGET0..3, 0x10 = ZBUFFER,
  0x20 = STENCIL. Measured `0x30` is a DEPTH-ONLY clear that would AND to zero
  under PC D3D9 values and vanish silently. ENGINE_MAP §6g.8.

MEASURED on the fixture, 16 presents: **45 RT clears + 18 depth clears
executed**, `dbgMsgs 0/0/0`, frames byte-identical to the gate-off arm.
Drops are arithmetic, not mystery: `45 + 99 = 144 = 36 x 4` target-slot requests
(the guest asks to clear all four slots; only slot 0 and the MRT slot 1 are
bound) and `18 + 27 = 45 = 36 + 9` depth requests. With `badHeader/noSlot/
createFail` all 0, every bound surface IS pooled, so every drop is a genuinely
unbound slot.

⚠ The debug layer caught a reasoning error the moment this armed: surfaces were
created with `pOptimizedClearValue = nullptr`, justified as avoiding mismatch
warnings. Backwards — NULL warns on EVERY clear (ids 820/821); a supplied value
warns only on the ones that differ. `dbgMsgs` went 0 -> 63, exactly one per
clear issued. Fixed by supplying the measured common case; back to 0.

⛔ **IT DOES NOT RETIRE THE HOST-INVENTED CLEARS YET, and that is worth stating
plainly because it is the charter's whole point.** The host clears
(`d3d12_backend.cpp` ~2797/4732/4748, gated by `SR_API_CLEAR`) target
`m_geoRT` / `m_sceneHdrRT` / `m_geoDepth` — the LEGACY draw path's surfaces. The
guest's clears land on POOL surfaces. Draws still render into the legacy geo
target, so its depth still needs clearing or frame N+1 fails the depth test
almost everywhere. The two retire TOGETHER with the draw-path move onto the
bound render target; deleting them now would break a working path to claim a
charter win that has not been earned.

**PRE-BAKE AUDIT (iter-446, 25 agents, 44 raw findings, 9 adversarially
verified, 6 survived).** ⛔ **NEITHER GATE MAY BE BAKED WITHOUT THESE**, and the
audit's central finding retracted this slice's headline claim.

| # | finding | status |
|---|---|---|
| 1 | resolve/clear recorded into the SHARED frame list gated only on raw `m_clState` — with `SR_HLE_OWN_PRESENT=0` the MAIN thread owns that list, so two threads record ⇒ `Close()` E_FAIL or device removal | FIXED: gated on `frameListEnabled()`; PROVEN to fire (`copies=0`, `noList=45`) |
| 2 | re-create released a live `ID3D12Resource` and rewrote its live SRV descriptor with no fence, while frame N-1 may still sample it | FIXED: retire queue drained on the frame fence + a FRESH slot per re-create |
| 2b | `HleTex::fmt` had ONE write site (the resolve), so upload-created entries read UNKNOWN and the destructive branch was unconditionally true; and the resolve keyed the map with the RAW destKey while everyone else normalises `& 0x1FFFFFFF` | FIXED both |
| 3 | **the copy source is never written** — "render products become real pixels" was FALSE | CLAIM RETRACTED + `srcNeverDrawn=` added |
| 4 | `m_resolveCopies` vs executor `copies` is a TAUTOLOGY quoted as a self-audit pass | RETRACTED in code, doc and PROGRESS |
| 5 | `[S6_DROP]` printed "Resolve has no command type" on every default run, in a build where it has one | FIXED, count now derived from the array |

**Finding 3 is the one that matters.** At `3f7a5da` the pool was entirely
write-only — no draw targets it and R6's clear had not landed — so the copy moved
deterministic BLACK into 14 textures and 315,122 guest binds resolved to black
rather than the white fallback. ⚠ And every counter grading the slice increments
identically whether the source holds an image or a blank, so the claim was
**unfalsifiable by its own evidence**. `srcNeverDrawn=` now reads **100%** and
can only fall when geometry lands in the pool.

✅ **DEPTH RESOLVE — DONE (iter-446, `ffcfb87`).** Was "the highest-value next
target"; the typeless depth resource + depth-plane SRV landed and the refusal
bucket went to zero (`skip(other=)` 7,982 → 0, destinations 14 → 15, debug layer
0/0/0). ⚠ Encoding is NOT claimed equivalent — see ENGINE_MAP §6g.12.

⚠ **UNVERIFIED, below the audit's verification cut — verify before trusting the
pool's formats**: the `k_16_16`/`k_16_16_16_16` UNORM-vs-SNORM mapping, the
EDRAM-tile-padded derived height, and `k_8_8_8_8_GAMMA` → `_SRGB` double
transfer. Each would make every pooled surface subtly wrong in a way no current
instrument can see.

### iter-446 S1/S2/S5 — DRAWS REACH THE POOL (added iter-447; the doc had gone 5 commits stale)

⚠ This section exists because the working doc was NOT updated when S1/S2/S5
landed: `git log -1 -- docs/MIGRATION_API_HLE.md` sat at `d087586` while four
later code commits shipped, and this file still named a COMPLETED task as the
next target. Neither gate below appeared anywhere in `docs/` until now.

* **S1** — each `HleBatch` carries its OWN rt/ds, latched AT SUBMIT.
  ⚠ UNPROVABLE BY MEASUREMENT today (`bindsAfterLastDraw=0` over 600 frames):
  the movie quad is the last GPU-affecting thing before every swap, so
  captured == mirror necessarily.
* **S2 `SR_HLE_DRAW_INORDER`** — draws record at STREAM POSITION. ✅ This
  REMOVES a deviation: the reference's `ProcDrawPrimitive` calls
  `drawInstanced()` directly in the dispatch switch and `video.cpp` has NO
  staging list. Ours had put every Resolve BEFORE the draws it should resolve.
* **S5 `SR_HLE_DRAW_TO_POOL`** — draws land on the guest's bound surface.
  `srcNeverDrawn` moved off 100% for the first time (fixture 189 → 173; boot
  roll 77,067/85,177 = 90.5%).

⛔ **THE WHOLE iter-446 SUBSYSTEM IS DEFAULT-OFF.** Verified in source, not
assumed — every one of these reads `return e && e[0] != '0'` (set-and-not-zero),
NOT the project's default-ON idiom `return !e || e[0] != '0'`:

| gate | site | default |
|---|---|---|
| `SR_HLE_RT_POOL` | `d3d12_backend.cpp:459-465` | **OFF** |
| `SR_HLE_RESOLVE` | `hle_video.cpp:202-206` | **OFF** |
| `SR_HLE_RESOLVE_DEPTH` | `d3d12_backend.cpp:451-457` | **OFF** |
| `SR_HLE_GUEST_CLEAR` | `hle_video.cpp:231-235` (`== '1'`) | **OFF** |
| `SR_HLE_DRAW_INORDER` | `d3d12_backend.cpp:442-449` | **OFF** |
| `SR_HLE_DRAW_TO_POOL` | `d3d12_backend.cpp:433-440` | **OFF** |

So the **zero-env exe does none of it**, and every iter-446 number was measured
with six gates on (`tools/last_run_x.json`). Honest in the code comments; it was
not stated in PROGRESS, and a reader could take the results as the default path.
Nothing here is forcing — these are renderer platform gates — but
NATURAL_BASELINE must not absorb them until they bake.

### ⛔ THE NEXT TRUNK — as AUDITED (iter-447), not as first written

iter-446 closed by naming **the vertex/index buffer path** as the next trunk, on
the strength of `[S6_COVER] recorded=735/5,745,071 (0.013%) noVerts=5,030,669`.
A 12-agent audit re-derived that block against the code. **The direction survives;
the argument does not.** Corrections that change the work:

1. ⛔ **"General geometry has no vertex data" is FALSE for ~714K draws/run.** The
   quoted `[S6_COVER]` line drops two columns the emitter prints
   (`hle_video.cpp:564-574`): `notQuad` and `wide=`. `notQuad` is reachable ONLY
   on the `d.vertBytes != 0` branch (`hle_video.cpp:1148-1149`), and the entry's
   own arithmetic recovers it: `5,745,071 − 5,030,669 − 735 = 713,667`. Those
   draws HAVE vertices and are refused by **our own default-OFF `SR_S6_WIDE`
   shape gate** (`hle_video.cpp:297-303`). 0.013% measures our gate, not the guest.
2. ⛔ **A `wide=1` roll already exists and reads 12.469%, with an EMPTY reject
   line** (iter-445 S6k, PROGRESS.md:498-507): `recorded=378650/3036645`,
   `noExpand=0`. General geometry ALREADY reaches `DrawInstanced`. VB/IB raises a
   ceiling; it does not unblock.
3. ⛔ **"Snapshot at VB/IB Unlock into a VA-keyed arena" was already MEASURED AND
   REJECTED** (PROGRESS.md:1047-1053, under "assumptions the measurements killed
   — record these so nobody re-chases them"): `ibCreate=7` for a whole run
   against ~938K indexed draws, and bound IBs live in a different address range
   than the D3D9 create path returns. *"The game builds its own buffer headers
   and memcpys into them; most geometry never touches D3D9's buffer API.
   Lock/Unlock can only ever be an INVALIDATION HINT. Resolution stays at DRAW
   time off the fetch constant (implemented, measured)."* We also hook none of
   the four VB/IB Lock/Unlock entry points.
4. ⛔ **"The movie quad only works because it is a UP draw carrying vertices
   INLINE" is inverted.** It arrives as `DrawVertices` from `sub_820FBBF8`, which
   resolves a `SetStreamSource`-bound VB **via the vertex fetch constant**
   (`hle_api_observe.cpp:1800-1830`; §6g.5, ENGINE_MAP:951-952). The genuine UP
   path `sub_820FBBB0` is a pure delegate with no snapshot. The one working
   end-to-end proof argues FOR extending fetch-constant resolution.
5. ⚠ **The sizing quantity is wrong.** 9.2M *binds* is not the residency driver;
   distinct buffers × bytes is — `[S6_UNIQ]` reads `distinctVBs=139,
   uniqueBytes=5.34 MB / 60 frames` (~0.09 MB/frame). PROGRESS.md:1054-1058
   already rejected elaborate residency: *"Build the simple cache; add machinery
   only if a measurement demands it."*
6. ⚠ **`[S6_DROP]` never printed `drawIV`/`drawV`.** Its array is fixed at
   `{SetStreamSource, SetIndices, EndVertices}` (`hle_video.cpp:486-488`);
   `drawIV`/`drawV` are `[API_OBS] SUMMARY2` fields from another file
   (`hle_api_observe.cpp:615-617`). The block is a hand-assembled digest
   presented as verbatim output, and it silently drops `EndVertices=322,000` —
   the third of the "three remaining armless types" it then reasons about.
   `[S6_PRIM]`'s `avgV` is likewise invalid: `primCensusType` counts all draw
   types but `primCensusVerts` only `DrawVertices` (`hle_video.cpp:1099-1101`),
   so it is a mixed-denominator ratio.
7. ⚠ **The quoted digits are from a roll whose log no longer exists.**
   `recorded=735` appears in no surviving artifact; the surviving roll ends at
   `recorded=751/5,509,462`, and `recorded` only ever increments, so 735 with a
   HIGHER `issued` cannot be the same process. K=1 overwrites
   `superman_returns_run0.log`. Baseline now preserved at
   `archive/iter447_baseline/`.

### ✅ THE KILLER WAS RUN (iter-447) — and it moved coverage 964x with NO CODE CHANGE

`SR_S6_WIDE=1` is an env gate, so the experiment cost **zero rebuild**. One
230 s K=1 roll, all six iter-446 gates ON so it is comparable to the archived
baseline (`archive/iter447_baseline/`, `archive/iter447_wide1/instruments.log`):

| final window | baseline `wide=0` | **`wide=1`** |
|---|---|---|
| `recorded` | 751 / 5,509,462 (0.014%) | **724,206 / 5,732,109 (12.634%)** |
| `notQuad` | 672,896 | **0** |
| `noExpand` / `[S6_REJECT]` | 0 / — | **0 / line never printed** |
| `present` · `feFrames` · `crash` | 6900 · 4865 · 0 | 7500 · 5076 · 0 |
| gate | MOVIE-ACTIVE | MOVIE-ACTIVE (`mQuads=808`) |

⛔ **This settles it: the 714K draws were never missing vertex data.** They were
refused by our own default-OFF gate, and when it opens they stage and record
with **zero rejects of any kind** — `noExpand=0`, and `[S6_REJECT]` does not
print at all (it is conditional on `noExpand`). iter-446's 0.013% was a
measurement of our gate. **VB/IB is not what blocks these draws.**

⚠ **NO VISUAL CLAIM IS MADE.** `wide=1` paints the 1x1 white fallback for
unresolved textures, so this roll is an INSTRUMENT reading, not a picture. No
contact sheet was eyeballed and none should be quoted; the gate stays default-OFF.
⚠ First roll of the pair died in early boot (`present=0`, `walksRet=0`) — plain
DOGMA roll-noise, preserved at `archive/iter447_baseline/wide1_roll1_deadboot.log`
so the pair is not quietly reported as 1-for-1.

### ⛔ AND THE ROLL NAMED THE REAL NEXT BLOCKER — one no prior instrument had

```
[SURF_POOL] poolDraws=1378  refused(noRt=0 fmt=723398)   srcNeverDrawn=80166/88602
```

**723,398 of the 724,206 recorded draws — 99.8% — are refused from the guest's
bound render target on FORMAT**, at `d3d12_backend.cpp:2944-2948`: the pool PSO
is built for exactly one pair (`R8G8B8A8_UNORM` + D32S8X24) and anything else is
counted rather than served by a guessed variant. That refusal is *correct* — the
comment is right that a guessed format renders plausibly and is wrong — but its
premise has expired. It reads *"no draw demands it today"*; **723,398 draws/run
now do.** This is why `srcNeverDrawn` stays at 90.5% even with S5 on.

**So the next slice is PSO FORMAT COVERAGE for the pool, not the VB/IB arena.**
The pool has five colour formats and one has a PSO. That is a bounded, named,
already-instrumented gap sitting in front of ~100% of the draws we can already
record — whereas VB/IB serves the `noVerts` population behind it.

> ✅ **DONE, iter-447.** `getXlatPso` now takes `rtvFmt`/`dsvFmt` and keys on them
> — the reference's shape (`PipelineState` members at `video.cpp:144-145`, set
> from the bound surface at `:3339`/`:3361`, whole struct hashed at `:4139`), not
> a variant system. `DXGI_FORMAT_UNKNOWN` keeps the legacy movie path
> bit-identical. The refusal now tests **hardware renderability**
> (`formatRenderable`, one cached `CheckFeatureSupport` per format) instead of
> equality with one hardcoded format, so it still refuses BY NAME and never
> serves a guessed variant.
>
> **A/B, same build, debug layer active, wide=1:** `refused(fmt=)` **723,398 → 0**;
> `fmtPairs` **0 → 2**; `srcNeverDrawn` **100.0% control → 82.9%**;
> **err=0** on both arms and the control carries MORE warnings (14,136 vs 13,719),
> so this added no debug-layer message. ⚠ Still 82.9% never-drawn, and NO visual
> claim — both gates remain default-OFF. Artifacts: `archive/iter447_psofmt/`.

**AFTER that, the VB/IB sub-slices order by yield/cost roughly INVERSE to how
iter-446 presented them** (`noVerts=5,007,903` is three causes, not one — all
three measured on this same roll):

| # | slice | population (wide=1 roll) | cost |
|---|---|---|---|
| 1 | ~~UP producer~~ **✅ DONE (iter-447, `SR_S6_UP`)** | 457,646 published/run | coverage 13.2% → 18.2%, accounting exact |
| 2 | ~~indexed producer~~ **✅ DONE (iter-449, `SR_S6_IDX`)** | 918,477 published/run (44% of seen) | coverage 18.2% → **41.1%**, conservation exact; see below |
| 3 | 4 KB snapshot cap (`kMaxVertSnapshot`) | `[S4_VERT] skipped 1,440,000` draws over cap | third by yield, first by cost |
| 4 | fetch-slot addressing | `[S6_STREAM] slot miss #800,000` (`size=21536 need=252504`) | an arena keyed by VA does NOT fix this |

#### SLICE 2 — SHIPPED SHAPE (iter-449). GATHER, not a host index buffer: the
backend CPU-expands packed guest vertices per draw anyway, so the producer
resolves `vertex(baseVertex + idx)` per index at the hook, in draw order —
topology preserved 1:1, ONE host draw with the full r7 (no split replication),
and the command reuses the `draw` struct with GENUINE fields (`start=0`,
`count`=vertex count). `vertBytes>0` is the executor's trust witness; the
gate-off census mirror keeps publishing raw r5/r6 with `vertBytes=0` and stays
refused — so no new command struct and NO kTraceVersion bump (banked traces
carry only the inert form; rationale at the version constant).

**MEASURED (armed1/full3, 200 s)**: published 918K/598K, conservation EXACT
(`seen == published + cut + range + poolFull` to the unit, both rolls); all
16-bit indices, expected endian (`endian=0 fmt32=0` refusals zero);
`maxCount=5556` indices. Movie gate EXACT with the producer live
(`snaps==3×mQuads` on every roll — the third arena tenant subtracts like UP).
Downstream: coverage **41.1%** recorded (`dropped=0`, `noExpand`=multiStream
only), `poolDraws=629,044`, resolve copies=52,207, `srcNeverDrawn` 81%→77%.
🏆 **EYEBALLED** (`archive/iter449_idx/full3/contact_8.png`): with
`SR_HLE_PRESENT_FRONT` armed the ENTIRE intro movie sequence arrives through
the natural chain (EA → tunnel → DC → WB → WBIE → **title card**) — iter-448's
armed roll had ONE frame and lost the rest to black. FE era: near-black but
`pxNotClear=921600/921600 distinct=52` — dim indexed content reaches the
primary.

**THE TWO NAMED REMAINDERS** (they, not the producer, now bound indexed
coverage): **cut=33%** — strips carrying the all-ones restart sentinel; a
gathered stream cannot express a restart (options: split-free trilist
conversion at gather, or D3D12 `IBStripCutValue` after the residency cache
lands a real index path). **range=24%** — the referenced vertex range exceeds
the stream-0 fetch constant's declared size; multi-stream indexed geometry is
the lead suspect (same class the backend refuses as MultiStream, here visible
producer-side). Plus `poolFull` (~330-935/roll, ARENA-LAPPED flagged): gathered
spans >8 KB crowd the 48-slot 1 MB arena — the residency cache (slice 3) or a
fourth size class retires it.

#### iter-449d — THE CUT CLASS SETTLED. The "cut=33%" refusal decomposed into
two truths, each proven by an instrument that could have refuted it: **strips
restart** (reg `0x2103` shadow = 0xFFFF on every cut draw; 4-index substrips;
converted to triangle lists at gather via the D3D parity rule, published as
prim 4) and **lists do NOT** — a 0xFFFF entry in a prim-4 IB is a REAL index
into a ≥65536-vertex mesh (the segmentation model's own kill switch fired:
convRem=3.09M dropped indices while every p4 cut draw has count%3==0). The
one silent ambiguity (a ≥65536-vertex strip) is counted: `ambig=0` measured.
Result: indexed published 44% → 67.3%, coverage 49.8%, first 3D scenes
eyeballed (`archive/iter449_idx/conv3/`). ⚠ The IDA decompile note lives ON
sub_820FC000 in the IDB. Remaining: `range` (now 32%) — multi-stream.

#### iter-449b — THE DEPTH-LESS PASS (`noRt` was a MISLABELED refusal).
`[S6_NULLBIND]` (probe kept live, counters always-on) proved every null
SetRenderTarget bind is idx 1-3 — RT0 is NEVER null-bound, guest shadow
`dev+12168` agreeing on every bind. The `noRt=642K` bucket (HALF of recorded
draws) was therefore depth-LESS passes: RT0 bound, DS NULL. Served now via
`getXlatPso(..., noDsv=true)` (DSVFormat=UNKNOWN, depth forced off, depth bits
out of the key) + a null-DSV `OMSetRenderTargets`. MEASURED: `noRt` 642K→0,
`poolDraws` 629K→960K (noDs=461,825 census), `srcNeverDrawn` 77%→16%,
`distinct` 52→545, and the FE-era space background became visible in RAW
dumps — the "dim FE" of the first eyeball was this missing draw class, not a
constants/gamma defect.

⚠ Slices 1+2 are **missing producers**, not buffer residency — the hooks publish
`vertBytes = 0` unconditionally (`hle_api_observe.cpp:1480` indexed, `:1537` UP).
Only slice 3 is the residency problem, and `[S6_UNIQ]` sizes it small:
`distinctVBs=117, 4.24 MB / 60 frames` (~0.07 MB/frame). Build the simple cache.

#### SLICE 1 PREREQUISITES — MEASURED (iter-447 `[S6_UP]`, probe was never run before)

```
begins=449602 nullAlloc=0 drainEnd=364690 drainBBB0=84912 overwritten=0 pending=0
zeroSpan=0 repeating=0 unreadable=0 | distinctVertHash>=100000 (cap) span=32..207360
```

✅ **The drain-site model is SOUND** — conservation EXACT
(`364,690 + 84,912 = 449,602`), `overwritten=0`, `pending=0`, so the End-latch is
right for every caller across both drain points, and the bytes at drain time are
always readable/non-zero/non-repeating. The snapshot point is settled: **drain,
never Begin** — the caller memcpys vertices in AFTER `sub_820FB6E8` returns.

⛔ **Vertices VARY — ≥100,000 distinct hashes / 449,602 drains** (count saturated
its cap). The first 12 log samples were all `prim=13 count=4 stride=20` with one
identical hash, which read as "unit quad placed by constants"; the counter
refuted it. Do not size this slice off log samples.

⛔ **`span = 32 .. 207,360 bytes`** — `kMaxVertSnapshot = 4096`
(`gfx/hle_video.hpp:335`) would refuse the large end. The producer needs the
1 MB-slot arena, not the small-block pool.

⚠ **THE REAL SCOPE, unchanged: UP draws need their OWN STATE.** The
decl/constants/render-state/shader publishes are all inside `if (vBytes)` in the
*DrawVertices* hook (`hle_api_observe.cpp:2019`), so a UP draw currently inherits
the last DrawVertices' state — in the FE era that is the frozen movie quad's.
Vertices alone will render the front end with the movie's transform. This is why
iter-445's "snapshot count × stride, done" costing was wrong.

#### ✅ SLICE 1 SHIPPED — `SR_S6_UP` (default OFF), iter-447

Drain-site snapshot + **`publishDrawState()` factored out of `sub_820FBBF8`** so
the UP draw carries its OWN decl/constants/registers/shaders, published
immediately before it. New `DrawVerticesUP` command type.

```
[S6_UP]    published=457646 arenaFull=0  (== begins == drainEnd 372389 + drainBBB0 85257)
[S6_COVER] recorded=1264650/6941362 (18.219%)  notQuad=0 noExpand=0   [was 13.168%]
[S6_PRIM]  p13:n=243452 avgV=12 maxV=276        [was n=117934 avgV=0 maxV=4]
[S4_STAGE] snaps=2277 upSnaps=20503 blocks=6325199 rejects=0 | movieQuads=759 | MOVIE-ACTIVE
```

**Three decisions, each forced by a trap:**
1. **Its own command type, NOT `DrawVertices`.** `isMovieQuad` is
   `primType==13 && count==4`, which the UP path emits constantly — reusing
   DrawVertices would bind the YUV planes to UI geometry and inflate `mQuads`,
   destroying `snaps == 3 × mQuads`. The predicate is now `== DrawVertices`
   explicitly rather than `fieldsTrusted`.
2. **Enum value APPENDED** — existing types keep their values, v5 traces stay
   interpretable, `kTraceVersion` deliberately NOT bumped.
3. **State published BEFORE the draw**, per the publish-order contract.

⛔⛔ **`poolWrite` ROUTES BY SIZE — the trap that broke the movie gate twice.**
`gfx/hle_video.cpp:1846-1852`: `<= kBlockBytes` → small-BLOCK pool, larger → the
1 MB ARENA. `snaps` is `arenaStats().writes`, so **only large spans contaminate
it**. First producer roll: `snaps=23431` vs `movieQuads=736`, invariant gone.
Then the "fix" subtracted ALL 467,518 publishes and drove `snaps` to **0**, so a
739-quad roll reported **`NO-MOVIE-THIS-ROLL`** — worse, because it lies in the
direction that makes a good roll look empty. Correct fix: count arena writes
specifically by testing `kPoolBlockFlag` on the tag `poolWrite` returns. Measured
split **20,503 arena / 437,143 block**. Never assume which pool `poolWrite` used.

### THE REFERENCE'S BUFFER MODEL — read in full (iter-447), and it SPECIFIES slices 1+2

⚠ **Why this section exists.** §"OWNERSHIP DECIDED" above (line ~337) settles that
the reference is NOT law for resource ownership. That decision is correct and
stands — but for one session it was used to skip reading the buffer path AT ALL,
which is a different and wrong conclusion. Ownership is one of four separable
machines in that path; the other three are ownership-INDEPENDENT and specify work
we have to do anyway. A 12-agent read, every finding adversarially verified:

**✅ 1. THE INDEXED-DRAW MAPPING — DIRECTLY transferable, and it is the spec for
53% of our draws.** `ProcDrawIndexedPrimitive` (`video.cpp:4679`):
```cpp
drawIndexedInstanced(args.primCount, 1, args.startIndex, args.baseVertexIndex, 0)
```
* the field NAMED `primCount` is the **INDEX COUNT**;
* `baseVertexIndex` is **`int32_t` — signed** (`video.cpp:961`), passed through;
* **there is NO primCount→indexCount conversion anywhere in the file.** The Xenon
  entry points already take counts in indices; the PC-D3D9 names are vestigial.

⛔ **And OUR guest signature was proven from the binary, not cited**: `sub_820FC000`
emits a type-0 packet `0x2102` (`VGT_INDX_OFFSET`) followed by `a3` ⇒ **r5 =
baseVertexIndex**; r6 appears only as `2*a4 + ib[+24]` (or `4*a4` when bit31
selects 32-bit indices) ⇒ **r6 = startIndex, in INDEX UNITS**; r7 becomes
`VGT_DRAW_INITIATOR num_indices` ⇒ **r7 = the index count**. This is exactly the
mapping the shared macro gets wrong (`hle_api_observe.cpp:1472-1480`) and that
`gfx/hle_video.cpp:1138`'s field-trust guard refuses on.

⛔⛔ **A TRAP THE REFERENCE CANNOT WARN US ABOUT — found in our own binary.**
`sub_820FC000` **SPLITS any draw with r7 > 0xFFFF** into multiple hardware draws,
using the per-prim table at `0x820B3668` (8-byte records of
(indicesPerPrimitive, overlap); idx4=(3,0) idx13=(4,0)). One D3D12
`DrawIndexedInstanced` with the full r7 is equivalent and simpler — but the arm
**must not assume r7 ≤ 65535** and **must not replicate the split**. ⚠ That table
is used ONLY inside the split; it is NEVER applied to the caller's count, which
independently forbids any primCount→indexCount conversion on our side too.

**✅ 2. THE UP PATH — the reference's shape is the one we want.**
`DrawPrimitiveUP` (`video.cpp:4682-4694`) copies the guest's inline vertices AT
THE HOOK into an intermediary upload allocator **on the guest thread**, so the
render thread never touches guest memory — the same hard rule we arrived at
independently (`gfx/hle_video.cpp` TU rule). `ProcDrawPrimitiveUP` (`:4699-4723`)
then fills the vertex-buffer VIEW from a raw upload allocation — proving the bind
path needs no per-buffer record at all — and `:4720-4723` **generates index data
for QUADLIST and TRIANGLEFAN** (`g_quadIndexData`/`g_triangleFanIndexData`)
rather than refusing them.

**✅ 3. PERSISTENT PER-BUFFER RESIDENCY IS OWNERSHIP-INDEPENDENT — and we already
compute the key.** The reference keeps ONE `RenderBuffer` per `GuestBuffer` for
its whole lifetime and uploads the WHOLE buffer at Unlock; the guest handle is
used only to reach it, and no per-draw extent (`count*stride`) ever sizes or
gates an upload. Keyed on `(physBase, size)` instead of a handle we don't own,
that structure is available to us today: `hle_api_observe.cpp:1852-1854` ALREADY
forms `(vbBase & 0x1FFFFFFF) << 32 | sz` for the `[S6_UNIQ]` census. The key
exists; only the cache doesn't. Building it retires `kMaxVertSnapshot = 4096`
(`gfx/hle_video.hpp:335`) — **1,440,000 draws/run currently skipped over the cap**.
⚠ Note the reference's allocator **GROWS** (`buffers.resize(index+1)`) and so can
never fail, where ours returns `kArenaFull`/`kBlockFull` and REJECTS. That
difference is the shape of iter-444's 2.3M-reject leak — carry it into the design.

**✅ 4. OUR 12-BYTE VERTEX ELEMENT IS INDEPENDENTLY CONFIRMED.** `video.h:258-267`
declares `GuestVertexElement` as exactly `{be16 stream, be16 offset, be32 type,
u8 method, u8 usage, u8 usageIndex, u8 padding}` — 12 bytes, matching the contract
PROGRESS.md:1059-1062 records us deriving painfully (and once getting wrong as
D3DVERTEXELEMENT9's 8, which silently truncated the third attribute).

**⚠ 5. TWO PLACES WE ARE ALREADY AHEAD — do NOT "fix" ours toward the reference.**
* **Endianness.** The reference hardcodes `UnlockBuffer<uint32_t>` for vertex
  buffers (`video.cpp:2302`) — a blind dword swap, correct only if every attribute
  is 4 bytes. Ours is data-driven: `vbEndian = d1 & 3u`
  (`hle_api_observe.cpp:1826`) feeding `xeSwap32(v, endian)` with all four Xenos
  endian ops (`gfx/d3d12_backend.cpp:1407-1413`). Strictly more general, and it
  has to be — we did not choose the formats.
* **Topology.** Our `p6` population (982,586) is triangle **STRIP** in the Xenos
  enum, native in D3D12 and already handled (`d3d12_backend.cpp:2648`). Prim 5
  (FAN) has **zero** draws. Topology is nearly covered, not a gap.

**⛔ 6. WHAT GENUINELY CANNOT TRANSFER** (the ownership machine, unchanged):
the host object BEING the guest handle (`CreateVertexBuffer` allocates a
`GuestBuffer` inside guest-physical memory via `AllocPhysical` and returns it
`MapVirtual`'d, `video.cpp:3159-3182`); `Lock` returning host scratch
(`:2208-2216`); and the guest's own refcount/lifetime signal. A delegating
harness must own instead: the VA→record side table, the residency key and its
agreement across re-creation, window aliasing, and named refusal. ⚠ The frame-
indexed **retirement discipline** (`g_tempResources`, drained on the frame fence,
`video.cpp:670-705`) IS transferable for the host objects we do own — do not
discard it along with the ownership model.

### ⛔⛔ FIXING THE DEAD `vbCreate` GAUGE IMMEDIATELY OVERTURNED A CLAIM

`vbCreate` had no increment site anywhere — it read 0 BY CONSTRUCTION, and that
zero was quoted (in this doc's own iteration, and to the user twice) as proof
the guest creates no vertex buffers. iter-447 gave it a real notifier
(`apt_render_watch.cpp` → `sr::kernel::noteVertexBufferCreate()`). First roll:

```
[API_OBS] SUMMARY2 drawV=2253588 drawIV=2844034 drawUP=385240
          setVS=0(unhooked) setPS=0(unhooked)  vbCreate=3506  ibCreate=7
```

**`vbCreate=3506`, not 0.** So the standing claim — *"the game builds its own
buffer headers and memcpys into them; most geometry never touches D3D9's buffer
API"* — is **PROVEN for INDEX buffers** (`ibCreate=7` for a WHOLE RUN against
2,844,034 indexed draws) and **NOT PROVEN for VERTEX buffers**. It was only ever
tested on the index side (PROGRESS.md:1047-1053 compared bound IBs
`A77FAA0C`/`A982D57C` against D3D9-create returns `83A9FF80`/`8E95DB40`); the
vertex half rode along on a counter that could not fail.

⚠ **What 3,506 does and does NOT mean.** It does NOT resurrect "capture at VB
Unlock" — 3,506 creates against 2.25M `DrawVertices` is consistent with buffers
created once and reused, which is what a RESIDENCY CACHE wants, not what an
Unlock-keyed capture needs. And `[S6_UNIQ]` still reads ~117 distinct VBs per 60
frames, so the working set is unchanged.

### ✅✅ THE VERTEX SIDE IS NOW SETTLED TOO — `[S6_VBID]`, iter-447, 100% MISS

The test named above was run. `sub_820F3ED8` records every returned VB header VA
AND harvests the plausible pointers inside each header (the D3D9 create call
returns a HEADER, while draws resolve a DATA base — comparing only the returned
VA would report a miss and prove nothing). The drawn side is tested at the
`[S6_UNIQ]` DISTINCT-KEY point (~117/60 frames), never per draw — iter-445
measured that a lock on the 2.25M/run path costs ~60% of throughput.

```
[S6_VBID] creates: hdrVAs=3504 ptrsInHdr=4042 |
          drawn distinct tested=11362 hitHdr=0 hitPtr=0 MISS=11362
          eg hdr=83A34D20,83A2C460   drawn=8BCB68B0,8BB465B0
```

⛔ **ZERO of 11,362 distinct drawn vertex buffers come from the D3D9 create path**
— not the header VA, not any pointer inside the header. Three independent
corroborations: (1) the address ranges are DISJOINT (`0x83A2-83A3xxxx` created vs
`0x8BB4-8BCBxxxx` drawn) — the same signature PROGRESS.md:1047-1053 found for
index buffers (`A77FAA0C`/`A982D57C` bound vs `83A9FF80`/`8E95DB40` created);
(2) there are MORE distinct drawn buffers (11,362) than creates (3,504), so they
cannot all originate there even in principle; (3) `ibCreate=7` against
`drawIV=3,600,683`.

✅ **So the question the `vbCreate` fix opened is CLOSED, and closed the other
way.** `vbCreate=3506` is real — the guest DOES create vertex buffers through
D3D9 — but **none of the geometry we draw comes from them.** The corrected claim
is stronger than the original, because it now rests on a positive test rather
than on a counter that could not fail: **the reference's per-buffer
Create/Lock/Unlock model has NO purchase on our vertex path either.** Resolution
stays at DRAW time off the vertex fetch constant — which is what already works
(885,360 draws recorded).

⚠ **What the probe cannot exclude**: a data pointer stored deeper than offset 28
in the header, or reached indirectly. That residue is small against disjoint
ranges plus the 11,362 > 3,504 counting argument, but it is not zero — the
result is "no relationship found by two direct tests", not a proof of absence.

⚠ **Design consequence, and it does NOT weaken the residency plan:** a per-buffer
host cache keyed on `(physBase, size)` from the fetch constant is still exactly
right (§3 above) — what dies is only the idea of hanging it off Create/Lock/Unlock.

⚠ `setVS`/`setPS` are still dead by the same mechanism and are now printed as
`0(unhooked)` so they cannot be misread as measurements. Give them notifiers or
delete them — do not quote them.

The field-trust guard (`hle_video.cpp:1107-1133`) is REAL and must be honoured:
the shared macro publishes `sub_820FC000`'s `baseVertexIndex`/`startIndex` as
`start`/`count` and never captures the true index count (r7), so slice 2 cannot
begin by simply setting `vertBytes`.

Then S7 delete the inference machinery.

**Phase 1 LANDED (same commit):** `gfx/hle_video.{cpp,hpp}` — RenderCommandType
enum (the full Phase-0-named surface), UNBOUNDED mutex+cv deque (no caps, no
pushBlocking, the reference property), detached render thread (drain+count;
device ownership arrives in Phase 2), guest object structs, [HLE_VIDEO] stats.
First producer wired: the guest's own Present (Swap hook enqueues
RenderCommandType::Present). NEXT = Phase 2: hook the named surface as
producers, move translation + device ownership into the render thread,
then finish Phase 3 (markers, queue caps, backend inference internals).

### THE VOLUME/CUBE SLICE — designed from census + reference (iter-449w, 2026-08-08)

**The census answered the design question** ([S6_TEX3D], 2 rolls, 8 distinct
binds each, stable): every guest Texture3D is **64×64×64 fmt=6 (k_8_8_8_8),
tiled, endian=2** — the color-grading/light-LUT class, ~1 MB linear each. Cubes
are fmt=6 and fmt=18 (DXT1), tiled. (Census cube DIMENSIONS were garbage until
the size_2d fix — cubes pack w:13/h:13 in dword_2, not the 3D 11/11/10 split;
fixed in `hle_api_observe.cpp`, prints d=6 by construction now.) Both classes
bind during the press-start FE backdrop scene already — no gameplay needed to
exercise the slice.

**Reference mechanics to replicate 1:1** (full report in session notes; key
cites `reference/.../gpu/video.cpp`):
- ONE texture heap, three typed index ranges in shared constants (ours already
  match byte-for-byte: dwords 0-15 2D / 16-31 3D / 32-47 cube / 48-63 samplers /
  64 booleans = the reference `SharedConstants` at V:160-171).
- Reserved null descriptors 0/1/2 = 1×1 R8_UNORM dummies with all-ZERO component
  mapping, one per dimension (V:340-346, V:1898-1945); allocator starts at 3;
  ALL 48 index dwords reset to the nulls at frame start (V:1607-1612).
- Every SetTexture writes ALL THREE ranges for that slot — real index into the
  range matching the SRV's dimension, nulls into the other two (V:3774-3796).
  Dimension is a cached property of the created SRV, never re-derived at bind.
- Cube resource = TEXTURE_2D + arraySize 6 + CUBE flag; view = TEXTURE_CUBE
  (V:5431-5464). Faces upload as array slices 0-5.
- Shader side: three HLSL arrays over the same heap via register spaces
  (shader_common.h:54-57); the Xenos `cube` ALU op is short-circuited through
  CubeMapData (direction stash), NOT emulated arithmetically (SC:155-194) —
  XenosRecomp already emits this; our executor just has to feed dwords 16-47.
- ⚠ The reference has ZERO untiling (its assets are pre-converted DDS). Our
  volumes/cubes are tiled=1 guest memory — the 3D untiler is OUR work, no
  reference to lean on. 64³ is small (256 KB/slice · 64) — untile on upload.

**Slice order**: (1) 3D: create+untile+upload the 64³ 8888 volumes into heap
range [kTex2DSlots..+7], route by owning map, fill shared dwords 16-31, null
dummies + per-frame reset; (2) cube: same shape into [+8..+15], DXT1 + 8888,
dwords 32-47; (3) then sRGB/samplers per the ranked list.

---

# Texture cache coherency — how we deviate (iter-450UG)

Written BEFORE proposing a production design, at the director's instruction.
Our own capture comment (`kernel/hle_api_observe.cpp:201-206`) already named the
hole — "1.9% of keys DO mutate, and this slice does not re-capture them ... left
to the invalidation slice rather than papered over." This is that slice.

## 1. The console-accurate design — xenia-canary: page write-watch, NO hashing

Xenia has no texture-level invalidation primitive at all. It invalidates guest
**physical pages** through the host MMU and lets textures subscribe.

- **Arm**: every uploaded range is re-protected read-only with a per-page
  `notify_on_invalidation` bit (`gpu/shared_memory.cc:343` → `memory.cc:2030`).
- **Trap**: a guest CPU store faults; `MMIOHandler::ExceptionCallback`
  (`cpu/mmio_handler.cc:402`) routes to `PhysicalHeap::TriggerCallbacks`
  (`memory.cc:624`).
- **Fan-out**: `SharedMemory::MemoryInvalidationCallback` clears validity bits
  and `FireWatches` (`gpu/shared_memory.cc:554`) invokes each overlapping
  range's callback and **unlinks the watch** — one-shot.
- **What "invalidation" MEANS**: not destruction, not re-keying — **two bools**.
  `Texture::WatchCallback` sets `base_outdated_` / `mips_outdated_` plus one
  cache-level atomic (`gpu/texture_cache.cc:569`). The host resource and the key
  SURVIVE; only contents are re-fetched, lazily, at the next bind (`:794`,
  `:520`). A texture is born outdated so the first bind uploads (`:490`).
- **GPU-side writes take the same road explicitly** — a resolve cannot fault, so
  it fires watches by hand (`RangeWrittenByGpu`, `gpu/shared_memory.cc:284`).

**Does Xenia hash contents to decide freshness? Definitively NO.** Hashing in
that tree covers the ~12-byte descriptor key only (`gpu/texture_cache.h:209`);
`TextureInfo::hash()` is referenced solely by trace/debug tooling. **Hashing is
used for IDENTITY, never for FRESHNESS.**

**Its cost model, quoted in its own source** (`gpu/shared_memory.cc:529`):
faults are expensive and redundant uploads are cheap, so invalidation is
deliberately over-broad — 4 KB callbacks put a software-rendered title at 4 FPS;
64 KB → 3 ms/frame; 256 KB → 0.7 ms. Steady state on an unchanged draw is one
atomic load and one mask AND.

## 2. The structural reference — UnleashedRecomp: guest-signalled, unconditional

There is **no cache keyed by guest address**: the `GuestTexture` *is* the guest
object, living in the guest heap and handed back as the guest's
`IDirect3DTexture9*` (`gpu/video.h:137-163`, `video.cpp:3095-3097`).

It learns of new pixels from the **hooked guest `Lock`/`Unlock`**:
`ProcUnlockTextureRect` re-uploads the entire slice **unconditionally**
(`video.cpp:2180-2206`). **No dedupe of any kind** — no dirty flag, no version,
no upload stamp, no content hash on `GuestTexture` (`video.h:154-163`). Lock N
times → N full uploads, even byte-identical. Its hashing budget goes to PSO /
sampler / vertex-decl dedupe, where the avoided work is shader compilation.

## 3. What WE do, and precisely where it breaks

We snapshot **producer-side at BIND**, inside the `sub_82100310` hook, and
freshness is a function of the binding tuple and nothing else:

```cpp
// kernel/hle_api_observe.cpp:3926-3927 — the tuple
const uint64_t key = ((uint64_t)base << 24) ^ ((uint64_t)fmt << 40) ^
                     ((uint64_t)width << 12) ^ (uint64_t)height;
// :3954-3959 — the INSERT *is* the test
if (s_texCaptured.size() < 100000) doIt = s_texCaptured.insert(key).second;
```

Three properties compose into the defect: **one-shot capture**; **permanent
dedupe** (the set is never aged or invalidated — its only erase is a pool-full
retry); and **no write watch of any kind**. The consumer *cannot* help: the
render thread is contractually forbidden guest-memory access
(`gfx/hle_video.cpp:1097-1098`).

> **The deviation, in one sentence:** we decide texture freshness from the
> BINDING TUPLE, while both references decide it from an OBSERVED WRITE — so any
> guest surface written after its first bind under an unchanged
> (base, fmt, w, h) tuple is frozen at its first-observed contents, permanently
> and silently.

**Why it exists:** we delegate to a live guest D3D9 rather than replacing it, so
we get no `Unlock` signal for surfaces the guest writes directly, and our
harness has no page-level write watch to fault on.

| | Xenia | UnleashedRecomp | Us today |
|---|---|---|---|
| Freshness source | guest writes to pages, via MMU | the guest's own `Unlock` | **the binding tuple** |
| Same-identity content change | contents re-uploaded, key kept | whole slice re-uploaded | **structurally unobservable** |
| Re-arm after upload | yes | n/a (guest re-signals) | **never** |
| Content hashing for freshness | **no** | **no** | no — and no watches either |

## 4. What the PRODUCTION design should be — and what this arc is NOT

The proof gate below hashes the selected span at a throttled subset of binds.
**That is a diagnostic, not the production answer**, and must not be baked on
the strength of an H6 result. Neither reference hashes for freshness, and the
honest reason ours would is that we lack the signal both of them have.

The production candidates, in preference order:
1. **A write signal from the guest side we already hook** — the D3D9
   `Lock`/`Unlock` pair (the UnleashedRecomp model). Cheapest and reference-
   backed; needs evidence on how much of the mutating 1.9% actually passes
   through Lock/Unlock, which iter-444 S6g-a suggests is a minority (883 bound
   textures vs 133 non-movie unlocks).
2. **A page write-watch** over guest texture pages (the Xenia model), which
   would need harness support our VEH does not have today.
3. Hash-at-bind only as a last resort, and then only with a cost budget.

**The cost of hash-at-every-bind, concretely:** binds run ~7.8M per run against
883 distinct textures, so an unthrottled full-span digest is gigabytes of
re-reads per run — which is exactly why the capture comment chose one-shot in
the first place. The proof gate therefore throttles to once per key per 60-Swap
window (~883 probes/window, the same order as the existing content census) and
says so in its own summary line.

## Readback / guest-visible surfaces — StretchRect maps to Resolve (iter-452b)

Directed comparison against the reference, by code, for the flat/black era
that follows the mode-select UI. (Earlier revisions called this the "post-HUD"
arc; that label was wrong — the whole-run sheets show the era begins when the
mode-select dialog disappears into black, not after a HUD.)

**Reference:** `reference/UnleashedRecomp/UnleashedRecomp/gpu/video.cpp`
- `ProcStretchRect` (3265) does **not** copy pixels. It builds a deferred alias:
  `args.texture->sourceSurface = surface` and
  `surface->destinationTextures.emplace(args.texture)`, then defers the copy via
  `g_pendingSurfaceCopies.emplace(surface)`.
- If that texture is currently bound to a slot it calls `SetSurface(i, surface)`
  (3292) — binding the **live surface**, not a copy. MSAA sources instead go
  through `SetTextureInRenderThread` + `g_pendingMsaaResolves` (3287-3288).
- `PopulateBarriersForStretchRect` (3364) and
  `ExecutePendingStretchRectCommands` (3409) run the deferred work later;
  `ProcExecutePendingStretchRectCommands` (3565) drives it per surface.

**Ours:** there is no StretchRect, and that is CORRECT rather than a gap — the
Xbox 360 D3D9 API has none. `D3DDevice_Resolve` (`sub_8210C5F8`, hooked in
`kernel/hle_api_observe.cpp:2341`) *is* the RT→texture copy every offscreen pass
ends with. Its argument layout is verified against the recompiled PPC prologue
rather than the XDK header, including the proof that `flags & 7` selects the
source and that RT0..3 live at `dev+12168+4*idx` with depth at `dev+12184`.

**Open question this comparison raises, and does NOT answer.** The reference
guarantees freshness structurally: a sampled StretchRect destination reads the
live source surface until the deferred copy runs. Our Resolve publishes a copy
keyed on the destination's fetch-constant base page. Whether a sampled Resolve
destination is guaranteed fresh is therefore the same class of question as the
atlas-freshness arc (C-450UB/UC/H6), and it is **not settled** by this reading.
From `archive/iter450x_h3/attempt1/run.log`, `destEverBound=15/16` — fifteen of
sixteen resolve destinations are bound as textures — so the question is live, not
theoretical.

⛔ This is a code comparison only. It establishes no cause for the post-HUD
uniform frame and licenses no renderer change.


### Resolve coverage and the UP-draw/blend/state contract, measured (iter-454c/d)

Measured on `archive/iter454_flat/attempt1/flat.srht` (53,428 records, 8
presents) with `tools/srht_census.py`. Numbers are per-fixture, not general.

**Resolve coverage.** 78 attempts across 16 destinations. 66 are structurally
eligible; 12 are refused, all of one class — `sliced`, i.e. `level` or `slice`
non-zero — and all against a single destination, `8B5BE000` (640x640, slices 1
and 2). That destination is **sampled 180 times** by `SetTexture`, so the gap is
a real coverage deviation in the general Resolve implementation: the guest asks
for a sliced/array copy and we decline it while the guest goes on sampling the
resource. It is NOT established that this causes any visible defect.

⚠ **`eligible` is not `succeeded`.** The structural predicate offline cannot see
a missing source, a disabled gate, a failed create, no descriptor slot or a
closed command list. `hleResolveToTexture` can still refuse. Any coverage
statement built on the offline predicate alone is an upper bound.

⚠ **Front-key resolves are not per-frame.** All 8 presents declare front key
`0A94E000`, but eligible resolves into it exist in only **3 of 8** present
intervals. The presentation path must not be described as resolving every frame.

**UP draw.** `DrawVerticesUP` with `primType=13` and `count=4` is the shape the
movie gate keys on, and it is also the shape of the 2D UI layer that closes each
frame. ⚠ **Primitive 13 is `kQuadList`, NOT RECTLIST** — Xenia's
`src/xenia/gpu/xenos.h` defines RECTLIST as **8**. An earlier revision of this
document and of `tools/srht_census.py` called it RECTLIST, which misnames the
shape every UI/UP draw in this engine uses. The blackout draw is exactly this
shape: stride 8, 32 payload bytes, a 2-element declaration (POSITION0 at offset
0, COLOR0 at offset 4). Reference: `ProcDrawPrimitiveUP` (video.cpp 4699-4737).

**Blend / mask / depth / VTE state.** These are recoverable per draw from the
captured `SetRenderState` block and are now decoded offline by
`srht_census.py draw`, using the executor's own indexing (the two register runs
are contiguous in MEMORY, so `0x2200` sits at dword 26, not at `0x2200-0x2100`):

| field | register | offline decode |
|---|---|---|
| colour write mask | `RB_COLOR_MASK` 0x2104 | run-1 dword 4, 4 bits per RT |
| depth test/write/func | `RB_DEPTHCONTROL` 0x2200 | run-2 dword 0; bit1 enable, bit2 write, bits 4-6 func |
| blend factors/ops | `RB_BLENDCONTROL0` 0x2201 | run-2 dword 1; src/comb/dst per colour and alpha |
| viewport transform class | `PA_CL_VTE_CNTL` 0x2206 | run-2 dword 6; bit 0x400 is the "written" witness |
| viewport scale/offset | `PA_CL_VPORT_*` 0x210F-0x2114 | run-1 dwords 15-20, big-endian floats |

✅ **Declaration types — SETTLED against the reference (iter-454d verdict).**
The declaration TYPE field is not mapped to a D3DDECLTYPE anywhere in our tree,
so `srht_census.py draw` prints several numeric readings and refuses to pick
one. For the blackout draw the reference settles it:

| type word | meaning | source |
|---|---|---|
| `0x1A2086` | `D3DDECLTYPE_UBYTE4N` → RGBA UNORM | UnleashedRecomp `gpu/video.h:213-237`, `video.cpp:4778-4821` |
| `0x182886` | D3DCOLOR → **BGRA** | same conversion table |

The blackout draw's COLOR0 is `0x1A2086`, i.e. **UBYTE4N/RGBA**, so
`00 00 00 FF` is **opaque black — not transparent**. The transparent-D3DCOLOR
reading I raised is REFUTED. Xenia likewise confirms blend factors **6 = source
alpha, 7 = inverse source alpha**. The cached pixel shader for this draw has
**no texture resources at all**: it reads `COLOR0` and writes it to `SV_Target`,
while the vertex shader passes saturated `COLOR0` and emits the covering
1280x720 quad from the integer pixel-space position.

⛔ This establishes a **guest-issued fullscreen opaque-black draw at the API
boundary**. It does NOT establish that the guest intends a "fade", and it does
not explain why the black persists for ~250 s. Those are guest-progression
questions, not renderer questions.

⛔ **Do not decode `1CF5B000` for this draw.** It is 512x256, Xenos format 20
(BC3), tiled, endian 1 — but the pixel shader has no resource bindings, so the
bound texture is irrelevant here and decoding it would answer nothing.

⛔ **Texture-format caveat for surface dumps.** `hleDumpSurfaces` decodes 8-bit
and `R16G16B16A16_FLOAT` correctly (the iter-449u fix) but reads any other
32-bit format as raw BGRA. Compressed formats are not decoded at all: texture
`1CF5B000` is 512x256, Xenos format 20 (BC3), tiled, endian 1 — its dumped BMP
interprets compressed blocks as BGRA and is **not a visual oracle**. Use the
format-independent `rawNonzero` count for black-or-not only, and decode via
Xenia's exact layout if pixels are needed.

### ⚠ iter-457 CORRECTION (2026-09-01) — read §iter-456 below BEFORE re-deriving

The iter-457 arc re-derived three things this section already recorded, because
I did not read it first: that the green is already in the raw HDR scene RT; that
`023C8000` and its sibling are **cloud-noise volumes, not grading LUTs**; and
that the 640x640 pass / `0B5BE000` is the standing suspect. The director marked
those ledger claims OVERSTATED for that reason. `gfx/xenos_tiling.hpp::tiled3D`
is also a line-for-line port of Xenia's `texture_address::Tiled3D`, so "our 3D
untiler may be scrambling it" was never an even fork.

**What iter-457 does add, and it is narrow:** the executor never registered a
stacked resolve product in the 3D descriptor band, so `0B5BE000`'s 672 readers
sampled the white 3D dummy. That is fixed behind `SR_HLE_RESOLVE_STACKED`
(default OFF) and it does NOT repair the backdrop.

### iter-456 — THE BACKDROP LANE OPENS: bind-misses exonerated, payload decode proven, the R32F shadow-pass product named the top suspect (2026-08-30)

Scene = the FE/title 3D backdrop (oracle `data/gpu_traces/menu_pressstart_oracle.png`:
daylight Metropolis canyon, textured towers, road, sky, flying Superman). Ours: same
camera and building volumes, but flat-shaded near-black materials, saturated-green
cloudy sky, hollow billboard quads, spike geometry, no Superman visible in the
composite (he IS in the scene RT), dark exposure. Fixture:
`archive/iter450uf_title.srht` (v5, 12 presents, replays deterministically in ~3 s);
fresh same-day fixture: `archive/iter456_title_fresh.srht`. Instruments used:
`SR_HLE_AUTOPSY=1` (+`_FRAME=n`), `SR_HLE_SURF_DUMP_AT=n`, `tools/srht_texkey.py`.

**Established (each by an instrument that could have refuted it):**
1. **White-fallback/bind-miss is DEAD for the backdrop.** Scene-window bind hit
   rate 97% (`[S6_WIN] hit=+1134 miss=+35`); the only repeating misses are two
   64x64x64 VOLUME keys (023C8000 sampler 4, 1F426000 sampler 8) which DO upload
   via the 3D path (`[S6_VOLUP]` idx 2/3) — the 2D-map miss log is a labeling
   artifact; whether the executor actually routes those samplers to the 3D SRV
   table at draw time is UNVERIFIED (open item 4).
2. **Captured texture payloads + our untile/endian transform are GOOD.** Offline
   independent decode (scratchpad `decode_dxt_payload.py`, Xenia tiled2D on the
   block grid + 8in16 swap) of banked payloads recovers real images: 0380F000
   (2048x1024 BC1 tiled) = a road/concrete material; 1DD57000 (256x256 BC3
   tiled) = asphalt detail. The runtime path (`hleUploadHostTextureFmt`) applies
   the same block-grid tiled2D + xeSwap32 — algorithmically aligned.
3. **The green sky is IN the raw HDR scene RT** (surf_82986F60 dump) — the post
   chain is exonerated for the hue. The sky has cloud STRUCTURE (the two scene
   volumes are cloud-NOISE textures, not grading LUTs: no decode variant of
   023C8000 is a smooth ramp, smoothness ~200/texel-step in every variant).
4. **⭐ TOP SUSPECT: the 640x640 offscreen pass renders a UNIFORM field.**
   rt=83A6D800, ds=NULL, 1,698 draws/frame (f2: 635-draw + 196-draw segments,
   verts~480K, noVerts=0, all one tuple vs=6A9C49D90E5F61F0 ps=BE398F0A17FF758A
   tex0=0B515000), resolved to 0B5BE000 as **fmt 37 = k_32_FLOAT — a
   depth-as-color SHADOW/LIGHT map** (classic 360 idiom, color RT carries
   depth), **sampled 146-180x per frame by the scene pass**. Our replay dump:
   `distinct=1` — every texel identical, where the console's would carry the
   city's depth. A uniform shadow product multiplied into every world material
   is exactly "flat, dark, unlit buildings".
5. Possibly related: `[S6_RTBIND] setVP=68 latched=2147483647x2147483647
   valid=0` — ALL 68 SetViewport records failed to decode/latch. Full-RT
   default viewport happens to fit the main pass; whether the shadow pass (or
   its VTE state) needs the real viewport is open.

**KILLS-IT next (in cost order):**
- H-shadow (the uniform R32F product causes the dark/flat look):
  KILL: in replay, substitute the resolve product 0B5BE000 with a synthetic
  far-depth (all-lit) constant via srht_texkey rewrite of its TextureUpload (if
  present) or a diagnostic that forces the 3D... simpler: A/B the scene with
  SR_TEXSLOT_WHITE=<key> style substitution for 0B5BE000 — if buildings LIGHT
  UP, confirmed. COST: one 3-s replay.
- H-vp (viewport decode broken): KILL: read the SetViewport producer/decoder
  pair; find why 68/68 fail validity; fix; replay A/B. COST: code read + replay.
- H-vol-route (samplers 4/8 not actually bound to 3D SRVs at draw): KILL: log
  texSlot3D hits at buildDraw for those samplers in replay. COST: one rebuild+replay.
- Why do the 1,698 shadow draws rasterize nothing? KILL candidates: dump the
  pass with depth-test state from the segment census (aSegs depth/mask/blend),
  check colorMask for RT0 in that segment, check the pass's VS constants (light
  matrix) vs the main pass. COST: one replay with the segment census read.

⚠ Two SR_HLE_TRACE_REC recording rolls in a row failed to full-init (walksRet
4/feFrames 1403, then walksRet 1/feFrames 0) where the five preceding untraced
rolls all full-inited; the third gated roll DID record
(`archive/iter456_title_fresh.srht`). 2-of-3 failure under the gate vs 0-of-5
without is NOISE-COMPATIBLE (historic deep-roll rate ~1/6) but worth an eye on
the recorder's boot-path cost if it recurs.

**Same-day addendum (fresh fixture + first killers run):**
- `archive/iter456_title_fresh.srht` VERIFIED (v5, 52,466 records, 16 presents,
  census-clean) and replays in ~1.4 s with hashMismatches=0 — it reproduces
  TODAY's defect family exactly (green cloudy sky, dark aliased-brick
  buildings, spikes, correct text): the green is live, not a fixture-era relic.
  This trace is the backdrop lane's iteration testbed.
- H-shadow FIRST KILLER RUN, result NULL: `SR_TEXSLOT_WHITE=0B5BE000` replay is
  pixel-identical to baseline at f10 — substituting the white dummy for the
  R32F product changes nothing visible. Two readings: the product's uniform
  value (raw bytes FF 7F FF — NOT a clean 1.0 depth clear; possibly -FLT_MAX
  class) already acts as a constant the scene tolerates, or its binds resolve
  through the swizzled-view path the whiten diagnostic does not intercept
  (composed=19001 views). H-shadow is WEAKENED for "dark flat buildings", not
  closed; the uniform-product-despite-1698-draws anomaly stands unexplained.
- NEW top hue suspect — PER-TEXTURE FETCH-CONSTANT SWIZZLE: blue->green IS a
  channel permutation (G'<-B), and the Xenos fetch constant carries per-texture
  channel selects that [S6_SWZ] mode=2 composes (19,001 composed, 6 views).
  The movie-era "default==mode2" proof covered k_8 (single channel) ONLY — a
  multi-channel (8888/BCn) swizzle misdecode would show exactly as a stable hue
  permutation on the affected textures. KILL: name the sky texture in the fresh
  fixture (autopsy per-draw on a sky-dominant frame), decode its payload
  offline (proven pipeline above), compare the offline TRUE colors against a
  replay pixel — a permutation names the swizzle path; identity kills it.
