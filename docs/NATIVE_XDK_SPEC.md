# Native XDK replacement spec (N0.1 output)

**Status:** 2026-10-01 (iter-463). All static: IDA decompiles and xrefs, scans of the
generated recomp, golden-dump reads already cited in `docs/NATIVE_ROADMAP.md`. Nothing was
run or built. Evidence tags follow the roadmap (**[static]**, **[measured]**, **[open]**).

This is the contract sheet for every XDK D3D entry point that the native path must
replace. It goes with three data files:

| File | What it holds |
|---|---|
| `data/native/xdk_entry_classes.tsv` | The sorted entry list: **213 rows** (the 216 external entry points minus the 3 non-D3D helpers). One class, a phase, evidence, guest-visible outputs, the Xenia reference, and what Unleashed does, for each row |
| `data/native/xdk_table_setters.tsv` | **Addendum: 146 setters and getters** that the 216 list misses because game code reaches them only through the device's setter tables (§2.3) |
| `data/native/xdk_guest_visible_ledger.tsv` | The guest-visible output ledger (roadmap §6.3, critique D3): 25 items with producers, reader sites and native obligations (§5 below); 25 items since the verifier pass added L25 |

The per-slice TSVs (`xdk_entry_classes.{rs_setters,shader,rt_draw_clear,present_sync,resource}.tsv`)
stay in the repo as the evidence behind the merge. The merge scripts are local scratch in
`archive/v1_native/n0/merge/`, which is gitignored:

- `merge_entry_classes.py` regenerates both class TSVs from the slices;
- `scan_table_setters.py` decodes the two tables from the IDA bytes in `rs_table_hex.txt`;
- `write_ledger.py` writes the ledger.

## 1. Result

| Class | Entries | Meaning |
|---|---|---|
| `cpu-only-keep` | 147 | Stays guest code. Writes only the device shadow, dirty bits or caller memory; native reads the shadow at draw time |
| `emitter-replace` | 23 | Writes PM4 into command memory; replaced natively |
| `resource-sync-replace` | 37 | Resource lifetime, locks, fences, queries, present, tiling, resolve; replaced natively with its guest-visible outputs preserved |
| `dead` | 6 | No live caller (static) or 0 entries in the Xenia traces (roadmap §11.1) |
| dropped (`non-d3d`) | 3 | `XMScalarSin_sub_82108D30`, `j_XMemCpy_sub_82468290` (at 0x820F5620), `ArrayForEachStride_sub_820FA7E8` |

So **60 entries need a native implementation**, by phase:

| Phase | Emitters | Resource/sync | Entries |
|---|---|---|---|
| N1 (record; draw core and resource registry) | 5 | 19 | draws, BeginVertices/EndVertices, VB/IB/texture create, lock, unlock, views, Release, InvalidateGpuCache |
| N4a (targets, resolve, present) | 4 | 3 | SetRenderTarget (and its thunk), SetDepthStencilSurface, present block, Resolve, Swap, EDRAM surface create |
| N3 (movies) | 1 | 1 | movie kick, BlitAndSwap |
| N2 (UI) | 4 | 0 | Clear, ClearF, SetViewport, SetScissorRect (+ table-only SCISSORTESTENABLE, §2.3) |
| N4b (post) | 1 | 0 | SetShaderGPRAllocation register block |
| N5 (world) | 8 | 6 | predication, tiling, Z-pass, BeginShaderConstantF4, queries, conditional surveys/rendering |
| N7 (cutover) | 0 | 8 | CreateDevice, device Release, InsertFence, BlockOnFence, IsFencePending, BlockUntilIdle, BlockUntilNotBusy, IsBusy |

The phase is where the native implementation first has to be correct. Every replace entry
is also hooked in record-only mode from N1 (roadmap §6, mode A), and native takes over its
side effects at N7.

## 2. Merge decisions

### 2.1 Conflicts, overlaps and gaps (re-read in IDA and the recomp on 2026-10-01)

| VA | Slices | Decision and why |
|---|---|---|
| `0x820F4280` GpuInvalidateMemoryRange (XDK InvalidateGpuCache) | resource: emitter; present_sync: resource-sync | **resource-sync-replace.** On the owning thread it writes WAIT_UNTIL + COHER_STATUS/SIZE/BASE_HOST + WAIT_REG_MEM (13 dwords); otherwise it merges the range into `dev+0x2D18`. That is the same coherency packet the Lock core emits, and Lock is resource-sync in every slice. Its native contract is an invalidation boundary (§4.5), not drawing state |
| `0x820F3618` / `0x820F3658` Acquire/ReleaseThreadOwnership | resource: keep; present_sync: resource-sync | **cpu-only-keep, observed.** The body stores a thread token (or 0) to `dev+0x2A08` (10760). Every reader of `dev+10760` in the recomp is an XDK internal that native replaces or never runs: `GpuRingWaitProgress_820F33E8`, the lock core `820F3988`, `820F4280`, `PixGpuCounterSampleCallback_8210B870`. The writers are `82106010`/`82106280` (device create). Native observes these calls as the (device, thread) bracket boundaries of roadmap §4.1 |
| `0x820F2A50` SetStreamSource, `0x820F2C08` SetIndices | rt: resource-sync | **Reclassified cpu-only-keep** for consistency with the shader binds `820F4E88`/`820F5218` and SetTextureFetchConstant `82100310`, which the shader and resource slices kept. All five have the same body: stamp the outgoing resource's `hdr+8` with `dev+10780` when that is non-zero; otherwise, only if `dev+10784 & hdr[0]`, append `(-1, res>>2)` to the command-memory list at `dev+13148` via `820FAC80`. The only writer of `dev+10784` is `GpuCmdContextBeginTiled_820FAEE0` (the dead recorder, roadmap C.1). Every golden dump shows `dev+10784 = 0` and `dev+10780 ≠ 0`. Keep condition: native owns `dev+0x2A1C` and keeps `dev+0x2A20 = 0` (ledger L04/L05) |
| `0x820F2720` SetScissorRect | rs_setters, rt | Both emitter-replace; merged |
| `0x820F5570` SetShaderGPRAllocation | shader, present_sync | Both emitter-replace. `dev+10792` is `dev+0x2A28`, so it is one field; merged |
| `0x821161D0` ShaderDisassembleToRtf | none (it fell between two slices) | **cpu-only-keep.** Decompile: it maps flag bits to disassembler options, memsets an 8 KB stack buffer and tail-calls `ShaderDisasmRun_82115BC8`. It touches no device, ring or MMIO |

Rows that came from more than one slice keep all the evidence, appended in `evidence`,
and the `merge_note` column records the decision.

### 2.2 Corrections to the roadmap and to IDA names (collected from the slices)

1. **§3.3: SetViewport `820F2810` is not shadow-only.** It always re-applies the scissor
   through SetScissorRect, and `820F0208` writes PM4 at `dev+40`. SetRenderTarget, and
   SetDepthStencilSurface when RT0 is null, reach the same chain through `820F0170`.
2. **§3.3: 62 per-state setters are shadow-only.** The list actually has 63 entries in
   `0x820F04E8..0x820F2720`: 60 are shadow-only, 2 are dead (tessellation), and
   SetScissorRect is an emitter.
3. **§3.3 groups state blocks, AddRef and thread ownership as group 3. The decompiles
   disagree.** State blocks (`82112BE8`/`C60`/`C78`/`CD0`/`D18`) only snapshot and replay
   the shadow. AddRef (`820F3D28`, `820F35A8`) is an atomic refcount. Thread ownership is
   covered in §2.1. All of them are cpu-only-keep. The Release replacement belongs on the
   destroy dispatch `D3DResourceDestroyDispatch_820F3788`, which is where Unleashed hooks
   it, and not on the refcount.
4. **§3.3 / §0: Unleashed keeps only the sampler setters as guest code.** It replaces 17
   render states with host versions, makes every other state a no-op, and replaces
   SetScissorRect, SetViewport, SetStreamSource, SetIndices, the shader create/bind calls
   and SetVertexDeclaration. Our plan keeps more as guest code (every shadow-only setter,
   the shader create/bind calls, the vertex declarations). That is valid here because our
   resources and shaders stay in guest format (A6, §4.4), and the XDK applies
   device-dependent logic natively we would otherwise have to re-derive: ZEnable and
   StencilEnable forced off without a depth surface, blend composition, the 1/255
   alpha-ref scale, and the format toggle.
5. **A.4 row 4 and N2: `820F1718` is not a gamma setter.** It is
   `D3DRS_HIGHPRECISIONBLENDENABLE` (308). It toggles formats 2↔10 and 3↔12 in the bound
   surface header and `RB_COLOR_INFO`, and never selects `k_8_8_8_8_GAMMA`. Where the real
   gamma format gets selected is still open (Q-N1 below).
6. **A.1: "every setter is reached by a direct call" is false.** See §2.3.
7. **N5 / §1: `QueryGetData` has two game readers, not one.** The wrapper
   `sub_82779A48` is called by `HudGlowOcclusionQuery_82304058` as well as
   `OpticalFX_DrawOcclusionFadedFlare_82721118`.
8. **IDA renames, proposed and not applied** (so sibling workflows keep stable names; this
   is N0.10's job):
   - `820F0588/0618/06A8/0738`: ALPHABLENDENABLE, BLENDOP, SRCBLEND, DESTBLEND (today
     ColorWriteEnable/ColorWriteMask*);
   - `820F1D40`/`820F1EC8`: SAMP MINFILTER/MAGFILTER;
   - `820F1D20`: PRESENTIMMEDIATETHRESHOLD;
   - `820F2B78`: GetStreamSource;
   - `820F2C98`: thunk to SetRenderTarget;
   - `820F2FD0`: SetDepthStencilSurface;
   - `820F2A50`: SetStreamSource (today SetRenderTargetSurfaceTile);
   - `820F2C08`: SetIndices (today D3DDeviceBindResourceTracked);
   - `820F27E8`/`2A38`/`32B0`/`32F0`: GetScissorRect/GetViewport/GetRenderTarget/GetDepthStencilSurface;
   - `820F4190`: D3DResource_IsSet;
   - `820FFF08`: D3DDevice_CreateTexture;
   - `82100028`: CreateRenderTarget/CreateDepthStencilSurface (EDRAM);
   - `82112C60`: D3DStateBlock_AddRef;
   - `82113010`: GpuBeginShaderConstantF4;
   - `820F5570`: SetShaderGPRAllocation;
   - `82102010/228/448/608`: Begin/EndConditionalSurvey, Begin/EndConditionalRendering;
   - the `0x820F90E8..0x820FA7E0` assembler cluster: its `D3DDevice*`/`CmdBuf*` names are
     stale (it works on a 19,824-byte shader-builder object, not the device).

### 2.3 Addendum: 146 setters reachable only through the device tables

`D3DDeviceInitDefaultRenderStates_821058B0` is the only reader of the static tables
`0x8295BF10` (render state: 97 entries of {get, set, default}) and `0x8295C3A0` (sampler:
20 entries). It applies every default and copies the tables into the device: the
render-state setters at `dev+56+state`, the getters at `dev+524+state`, and the sampler
setters at `dev+444+state`. Game and library code then call setters **through the device
copy** with `bctrl` **[static, recomp]**:

- `D3DXEffect_ApplyStateAssignment_823EB3C0` (render state and sampler);
- `D3DXEffect_ApplySamplerRecords_823EAC90`;
- the RenderWare state-block appliers `RenderStateBlock_ApplyRenderStates_82608FC0`,
  `...Untracked_826091A0`, `RenderState_RestoreStaleRenderStates_82609CF0`, and their
  sampler twins `826090E8`/`82609220`/`82609DF0`;
- the SetRenderState thunks `sub_8277A280` and `sub_822E3F10`, and the getter thunks
  `sub_8277A298` and `sub_822E3F28`;
- Lion `sub_827332D8` (sampler).

So 146 table functions are absent from the 216:

- 108 render-state functions: 48 setters and 60 getters with no direct external caller;
- 38 sampler functions: 18 setters (every one except MIN/MAGFILTER) and all 20 getters.
`scan_table_setters.py` checks each generated body for a call, an access to `dev+40`/`+48`
or MMIO:

- **145 are mechanically shadow-only** (`cpu-only-keep`): no call, no ring access, no MMIO.
- **`0x820F0F70` (D3DRS_SCISSORTESTENABLE) is an emitter.** It stores `dev+0x2D40`, then
  tail-calls SetScissorRect with `&dev+0x3070`. N2 must cover it.

Consequences:

- The N1 hook generator and `scripts/audit_native.py` must take their input from **both**
  TSVs.
- Table completeness (verifier, 2026-10-01): the two tables hold 210 distinct functions,
  which are the 62 table functions already in the 213 rows, the 146 here, and the two
  shared stubs for unused states, `sub_8267D338` (getter: `li r3,0; blr`) and
  `0x8231C140` (setter: bare `blr`). A dword scan of the golden-dump image for pointers to
  D3D-core functions finds no other function table outside `.pdata` and these two tables.
  It finds one more pointer: the `EX_TITLE_TERMINATE_REGISTRATION` at `0x8295BEA8` holds
  `sub_82105FE8` (`ExRegisterTitleTerminateNotification` + `VdShutdownEngines`).
  `D3DDeviceCreate_82106010` registers it and `D3DDeviceDestroyShutdownEngines_82106338`
  removes it. A native CreateDevice (N7) must not leave a guest `VdShutdownEngines`
  callback registered.
- A strong-def override of `sub_820F0F70` also catches the indirect calls, because
  `PPC_CALL_INDIRECT_FUNC` dispatches to the same symbol. Unleashed instead patches the
  device's table slots (video.cpp :2125). For keep entries, the indirection does not
  matter.

## 3. Cross-cutting native obligations

These hold for all phases. They come from the keep entries that stay guest code next to
native replacements.

1. **Maintained device fields** (the N7 checklist, roadmap R8):
   - `dev+0x2A1C`: the fence id of the open submission, never 0. Kept guest code stamps
     it into outgoing resources (`hdr+8`).
   - `dev+0x2A20` (10784): always 0.
   - `dev+52`: refcount, starts at 1.
   - The binding slots `dev+12164..12420` and the viewport and scissor shadows
     `dev+12376..12412`, `dev+0x3058..0x307C`: getters and `IsSet` read them.
   - Display mode: `dev+0x3370/+0x3374/+0x3378/+0x33A0`.
   - The present-immediate threshold, `dev+0x2D3C` bits 23-29.
   - The VdGlobalDevice cell `0x82000948`.
   - The EDRAM tile bitmap `*0x829ABA70` and the HiZ owner flag `0x829ABA6C`.
2. **Sentinel cursor.** `dev+0x28`/`+0x2C`/`+0x30` point at a sentinel, so any XDK
   path still writing packets reaches the flush or segment tripwire (`820FD8C0`,
   `820FCF90`; roadmap §1 item 2). Kept guest code reaches command memory only through
   the dead `dev+10784` branch (§2.1).
3. **Read the shadow, do not recompute it.** `SetVertexShaderConstantF` and
   `SetPixelShaderConstantF` take a caller-computed 64-bit dirty mask in `r7`, so native
   reads `dev+0/+8` and does not derive the mask from (start, count). The shadow writes
   are `stvx` (C3).
4. **Dirty-bit → register map.** It was proven from the flush in
   `D3DDeviceEmitDrawIndexedVertices_820FC000`:

   | Dirty field | Bits | Registers |
   |---|---|---|
   | `dev+0x10` | 0-11 | 0x2200-0x220B |
   | `dev+0x10` | 12-43 | fetch constants (0x4800); fetch slot s is bit 43-s |
   | `dev+0x10` | 44-48 | 0x2180-0x2184 |
   | `dev+0x10` | 49-52 | not a register run; consumed by `GpuEmitDrawState_82108668`: bit 49 → `GpuCmdUpdateSurfaceState_82107110`, bit 51 → vertex-shader load / fetch patch (set by SetStreamSource on a stride change and by SetVertexDeclaration), bit 52 → pixel-shader fetch constants and load |
   | `dev+0x18` | 1 | bool/loop constants (0x4900) |
   | `dev+0x18` | 2-22 | 0x2100-0x2114 |
   | `dev+0x18` | 23-38 | 0x2000-0x200F |
   | `dev+0x18` | 39-59 | 0x2280-0x2294 |
   | `dev+0x20` | 0-37 | 0x2300-0x2325 |
   | `dev+0x20` | 38-45 | 0x2380-0x2387 |
   | `dev+0x20` | 49-54 | shader constant runs |

   **Bit order (verifier, 2026-10-01).** Bits are numbered LSB-0 in the 64-bit qword
   (`rldicr r,r,n,63` = bit n), and inside each run the register index FALLS as the bit
   rises: `reg = run_base + (top_bit - bit)`, as for the fetch slots. Six independent
   witnesses, register names from Xenia `register_table.inc`:
   SetViewport sets `dev+0x18` bits 7..2 → 0x210F..0x2114 (PA_CL_VPORT_XSCALE..ZOFFSET);
   SetSlopeScaleDepthBias sets `dev+0x20` bits 45 and 43 → 0x2380/0x2382
   (PA_SU_POLY_OFFSET_FRONT/BACK_SCALE) and `dev+0x10` bit 6 → 0x2205 (PA_SU_SC_MODE_CNTL);
   VIEWPORTENABLE sets `dev+0x10` bits 7 and 5 → 0x2204/0x2206 (PA_CL_CLIP_CNTL,
   PA_CL_VTE_CNTL); HALFPIXELOFFSET sets `dev+0x20` bit 35 → 0x2302 (PA_SU_VTX_CNTL);
   SetDepthStencilSurface sets `dev+0x18` bit 36 → 0x2002 (RB_DEPTH_INFO);
   HIGHPRECISIONBLENDENABLE sets `dev+0x18` bit 37 → 0x2001 (RB_COLOR_INFO). IDA renders
   these 64-bit masks as `__ROR4__(1, k)` on the low dword, which reads as bit 32-k; the
   real bit is 64-k.

   `gfx/native_device` (roadmap N1, not written yet) will read with this map. Registers
   outside the dirty-flushed runs, such as the scissor TL/BR at `dev+0x2844/0x2848`, come
   from the emitters.
5. **Resource headers and backing stay guest-format** (A6). Every replaced create returns
   a byte-identical header over the same backing allocation, because kept guest code reads
   them (ledger L14).

## 4. Replacement contracts by phase

Columns:

- **In**: the arguments. Prototypes marked *XDK* follow the XDK API name and the slice
  decompile; every other prototype is decompile-confirmed.
- **Out**: the guest-visible outputs to preserve; L-numbers point into §5.
- **Side effects**: what native must also do.
- **D3D12**: the host mapping (Unleashed `video.cpp` structure).
- **Oracle check**: how the replacement is proven (roadmap §6.2 ladder: L0 conformance,
  L1 per-draw, L2 per-resolve, L3 per-frame, ledger diff).

### N1: draw core and resource registry (record-only first)

| Entry | In | Out (preserve) | Side effects | D3D12 | Oracle check |
|---|---|---|---|---|---|
| `820FB6E8` BeginVertices | dev, prim, vertexCount, stride | payload pointer (L01) | read the shadow and dirty masks without clearing them (mode A); state as of this call | upload-ring allocation; state snapshot at Begin | L1 per-draw vs `.xtr` DRAW_INDX (auto-index) |
| `820FBBA0` EndVertices | dev | none | the payload is complete here: read it (F) | copy the upload range; `DrawInstanced` | L1 vertex bytes = payload |
| `820FBBB0` DrawVerticesUP | dev, prim, count, data, stride | none | as Begin + copy + End | `DrawInstanced` from upload (Unleashed DrawPrimitiveUP :4682) | L1 |
| `820FBBF8` DrawVertices | dev, prim, start, count | none | splits above 0xFFFF are guest-side; native draws the same ranges | `DrawInstanced` (Unleashed :4611) | L1 |
| `820FC000` DrawIndexedVertices | dev, prim, baseVertex, startIndex, indexCount | none | IB from `dev+12164`; index format from the IB header | `DrawIndexedInstanced` (Unleashed :4653) | L1 incl. VGT_DMA base/size |
| `820F3ED8` / `820F4040` VB/IB create | *XDK* length, usage, (format), pool | header + backing (L14) | register the backing by physical page (A2) | buffer mirror created lazily at first use | header byte diff; registry hit at the first draw |
| `820FFF08` CreateTexture | w, h, d, levels, usage, format, pool, type | 52-byte header, fetch constant `+28..+48`, base/mip backing (L14) | registry entry keyed by the base/mip physical pages | texture mirror; untile/endian-swap at upload | header diff; L0 texture conformance |
| `820FFBE0`, `820FFCE8`, `820FFE38` surface/volume views | tex, level (and face) | view header, parent AddRef (L14) | registry alias onto the parent subresource | SRV/RTV of the parent mirror | header diff |
| `820F3FA0`, `820F40F0` VB/IB Lock | res, offset, size, flags | pointer, lock count (L02, L03) | wait on the native last-use fence unless flags say no-wait; mark the range for re-upload | none at lock (guest memory is the backing) | ledger VA equality |
| `820FFCB0`, `82100228`, `820FFDA0`, `820FFF00`, `821002D8` texture/surface/cube/volume Lock | tex, level, out, rect/box, flags | D3DLOCKED_RECT/BOX (L02) | as above, by subresource | none at lock | ledger VA/pitch equality |
| `820F4000`, `820F4150`, `820FFCC8`, `821002F8` Unlock | res | lock count, dirty-range reset, cache flush (L03) | §4.5 invalidation boundary: dirty the mirror for the locked range | re-upload on the next use (copy queue) | L1 LATE-MUTATION count = 0 on the movie and UI fixtures |
| `820F4280` InvalidateGpuCache | base, size (device from VdGlobalDevice) | none | §4.5 boundary: invalidate every mirror over the physical range | as Unlock | L1 after RenderWare uploads (`82613318`, `8261A028`, `8261B668`) |
| `820F3DA0` Release | res | refcount; destroy after the last use retires (L15) | hook the destroy dispatch `820F3788`, not the refcount; return EDRAM tiles | deferred destroy at frame end (Unleashed DestructResource :2149) | refcount sequence diff; no reuse before retire |

### N4a: render targets, resolve, present

| Entry | In | Out | Side effects | D3D12 | Oracle check |
|---|---|---|---|---|---|
| `820F2CA0` SetRenderTarget (+ thunk `820F2C98`) | dev, index, surface | `dev+12168+4i`, surf+28 format rewrite (3→12 family), viewport/scissor reset on index 0 (L16, L17) | RB_COLOR_INFO / RB_SURFACE_INFO / PA_SC_AA_CONFIG equivalents into the native target state | RTV bind; format from the header at draw time | L1 RT format and dims; getter ledger diff |
| `820F2FD0` SetDepthStencilSurface | dev, surface | `dev+12184`; RB_DEPTHCONTROL bits 0/1 forced off when null (L16) | HiZ control; viewport reset when RT0 is null | DSV bind | L1 depth state |
| `82100028` CreateRenderTarget/DepthStencil (EDRAM) | *XDK* w, h, format, multisample, params | 48-byte header, EDRAM base tile from the best-fit bitmap (L14) | keep the allocator as guest-equivalent: the bases are the R5 aliasing bases | RT/DS resource; aliasing only for the cases Q5 measures | EDRAM base diff vs shadow |
| `8210C5F8` Resolve | *XDK* dev, flags, srcRect, destTex, destPoint, level, slice, clearColor, z, stencil, params | dest `hdr+8`/`+0x0C` fence stamps; dest memory only if the CPU reads it (L04, L23); returns 0 | clear-on-resolve 0x100/0x200; depth flags 0x14; MSAA average; slice/cube | `CopyTextureRegion` or a conversion pass (7e3, 20e4, k_16_16 4x); clears as `Clear*View` | L2 per resolve (~13/frame); L0 resolve conformance |
| `82111EC8` present command block | dev | pacing (interval `dev+0x33A4`; L19) | none on the device beyond the `dev+0x2A39` flag | host pacing clock; no CP callback | §1 item 9 pacing |
| `82112050` Swap | *XDK* dev, frontBuffer, params | at most one frame in flight (blocks on the previous swap); front-buffer copy fields `dev+0x3470..`, counters `dev+0x4E54` (+2 per swap) (L19) | gamma ramp (`82111CB8`, Xenos PWL), scaler output size | swapchain Present after the front-buffer resolve; gamma as a PWL pass | L3 per frame; pacing; whole-run contact sheets |

### N3: movies

| Entry | In | Out | Side effects | D3D12 | Oracle check |
|---|---|---|---|---|---|
| `820FDE38` GpuEmitKickWithCallback | dev, mask (1<<(24+cpu)), callback, ctx | callback delivery after the host is done with the planes (L20) | run the callback on a KPCR thread at raised IRQL on the issuing CPU (roadmap N3) | `ID3D12Fence` completion → callback queue | ledger callback latency; movie pace vs Xenia (Q10); FMV bit-exact N = X |
| `82112750` BlitAndSwap | dev (movie thread, inside Acquire/ReleaseThreadOwnership) | as Swap; viewport/scissor restored; RT0 rebound to `dev+0x3464` | front-buffer resolve with clear 0,0,1,1 | as Swap, from the movie thread's bracket | FMV bit-exact; intro era viewed |

### N2: UI

| Entry | In | Out | Side effects | D3D12 | Oracle check |
|---|---|---|---|---|---|
| `82101998` Clear | dev, count, rects, flags, color, z, stencil | none | one ClearF per rect | as ClearF | L1/L3 |
| `82101A58` ClearF | dev, flags, rect, color4, z, stencil | dirty bits that force re-emission on the next draw (harmless natively) | per tile under predication; the 4x depth-only quad at z = −0.0 | `ClearRenderTargetView`/`ClearDepthStencilView` with rects; quad draw where the XDK's quad semantics differ | L0 clear cases; L1 |
| `820F2810` SetViewport | dev, D3DVIEWPORT9* | `dev+12376..12396` clamped to the RT/tile extent; PA_CL_VPORT_* shadow (L16) | re-applies the scissor | `RSSetViewports` with the half-pixel and VTE rules | getter diff; L1 viewport |
| `820F2720` SetScissorRect (+ table-only `820F0F70` SCISSORTESTENABLE) | dev, RECT* | `dev+0x3070..0x307C`, scissor TL/BR shadow `dev+0x2844/0x2848` clamped to the target when the test is enabled (L16) | per tile under tiling | `RSSetScissorRects` | getter diff; L1 scissor |

### N4b: post-processing

| Entry | In | Out | Side effects | D3D12 | Oracle check |
|---|---|---|---|---|---|
| `820F5570` SetShaderGPRAllocation | dev, flags, vsGPRs, psGPRs | `dev+0x2A28` GPR-split shadow (no game reader found) | its tail `82100488` (ClearFullSurface: dirty flush, state load, fixed-shader IM_LOAD, two dummy rect draws) must be expressed as the state and clear it encodes | record only; the dummy draws become the equivalent clear if they are visible | L1 on the render-script fixture (`8277B998`) |

### N5: world (predication, tiling, Z-pass, queries)

| Entry | In | Out | Side effects | D3D12 | Oracle check |
|---|---|---|---|---|---|
| `8210D400` SetPredication | dev, mask | none | the active tile mask for the following draws (live: 92,512 / 79,190 entries in Xenia, §11.1) | per-tile scissor replay, or folded when tiling is off (Q4) | (bracket, tile) join; L1 per tile |
| `8210D588` BeginTiling | *XDK* dev, flags, count, rects, clear color/z/stencil | none (debug callback only) | records tile rects and optionally clears the extent | native tiling is only a replay of draws per tile region, or nothing when one tile covers the target | L2 on the paired gameplay fixture |
| `8210DA98` EndTiling | *XDK* resolve args | HRESULT 0 (L12); dest fence stamps via Resolve | per-tile resolve | as Resolve | ledger HRESULT; L2 |
| `8210DF38` / `8210E1A0` Begin/EndZPass | dev, flags / dev | none; internal CP callback `82110B00` disappears | record once, replay depth-only then full (live: 11,189 / 8,943 pairs) | depth prepass over the recorded draws | L1/L3 on a gameplay fixture |
| `82113010` GpuBeginShaderConstantF4 | dev, isPixel, startReg, count | staging pointer the caller fills (L21) | apply at the next draw for the current tile | root constants / CBV update | L1 c100 per tile |
| `821028B8` QueryCreate | dev, type (8 event / 9 occlusion / 10 extent), count | object or 0 for other types (L11) | none | query heap slot | ledger null pattern |
| `82102BC8` QueryIssue | query, flags (begin 2 / end 1) | state and fence on the object | none visible | `BeginQuery`/`EndQuery` (occlusion); extent needs a custom pass | ledger |
| `82102DC8` QueryGetData | query, out, size, flags | S_OK/S_FALSE and data with a latency model (L10) | none | `ResolveQueryData` + readback | ledger sequence; flare and HUD glow vs Xenia (Q11) |
| `821029A8` QueryRelease | query | refcount | free the host slot | — | refcount diff |
| `82102010` / `82102228` Begin/EndConditionalSurvey | dev, index, flags | none | survey id ↔ host occlusion query | query heap | L1 skip decisions |
| `82102448` / `82102608` Begin/EndConditionalRendering | dev, index | none | skip draws whose survey saw 0 samples | `SetPredication` on the query result buffer, or CPU skip | L1 skip decisions vs `.xtr` viz_query draws |

### N7: device and fences (cutover)

| Entry | In | Out | Side effects | D3D12 | Oracle check |
|---|---|---|---|---|---|
| `820F34C0` CreateDevice | *XDK* adapter, type, focus, flags, params, out | device pointer, return 0 (L13); the layout every inline writer and kept setter expects | allocate the device struct, apply the default states from the tables (as `821058B0` does), publish via `0x82000948`; `D3DDeviceCreate_82106010` never runs; serve only `RenderDeviceInit_82621258` | device, queues, swapchain | field diff at the first present |
| `820F35C0` device Release | dev | refcount; teardown at 0 | native shutdown; no engine teardown | release host objects | refcount diff |
| `820FDD30` InsertFence | dev | fence id (L06) | close the current host submission | `Signal` on the host fence | id sequence diff |
| `820FDD68` BlockOnFence | fence | blocks (L09) | — | `SetEventOnCompletion` + wait | no hang; wait counts |
| `820FDD90` IsFencePending | fence | BOOL, TRUE = pending (L07) | — | `GetCompletedValue` compare | per-call diff |
| `820FDCB8` BlockUntilIdle | dev | blocks until idle (L09) | — | flush + wait all | no hang |
| `820FDE08` BlockUntilNotBusy | res | blocks on the resource's last use (L09) | — | per-resource last-use fence | no hang |
| `820FDE30` IsBusy | res | BOOL (L08) | — | per-resource last-use fence | per-call diff |

**The fence invariant** (roadmap N7, B3) covers every row in this table: a native fence
never completes before the native path has consumed the guest memory its work references.

### Dead (no native work)

| Entry | Why |
|---|---|
| `820FAEE0`, `820FB3A8`, `820FB5A8` command-buffer recording | Only caller is `GpuBuildTiledCmdSegments_82304C38`, which has no code caller (C.1) **[static]** |
| `820FC478` DrawTessellated, `820F1A10`/`820F1A48` tessellation setters | Only caller `82786B60`, 0/0 entries in both Xenia runs **[measured, boot to early Metropolis only]**; recheck with chapter saves |

## 5. The guest-visible output ledger (roadmap §6.3, D3)

The full rows, with reader sites and evidence, are in
`data/native/xdk_guest_visible_ledger.tsv`. `tools/native_ledger_diff.py` (N1) diffs a
replace run against the shadow run item by item.

| # | Value | Kind | Readers (sites) | Native obligation |
|---|---|---|---|---|
| L01 | BeginVertices payload pointer | return | 16 payload writers / 18 sites; EndVertices 26 / 33 | writable buffer of count*stride; ledger records the offset class |
| L02 | Lock pointers, Pitch, SlicePitch | return, out | all lock callers; `RwRasterUploadLevelPixels_8261C5B0` | same guest VA as the XDK (guest-format backing) |
| L03 | Lock count `hdr+0`, dirty range `hdr+20/+24` | resource field | XDK unlock core; game readers [Q7] | balanced as the XDK keeps them |
| L04 | Fence stamps `hdr+8` (`+0x0C` resolve dest) | resource field | lock core, IsBusy, BlockUntilNotBusy, destroy dispatch; game [Q7] | stamps name native fences (`dev+0x2A1C` owned natively) |
| L05 | `dev+0x2A1C` submit fence, `*(dev+0x2A10)` completed, `dev+0x2A20` = 0 | device field | kept binds; fence API | non-zero, monotonic, wrap-safe |
| L06 | InsertFence value | return | stored at rwdrv+164/168 (`0x8299AE94/98`) by the two present wrappers and **read by nothing else** (Q-N3, iter-464; the reaper/waiter consume `hdr+8`, L04) | any monotonic id |
| L07 | IsFencePending (TRUE = pending) | return | `GpuFenceReaper_82616208` on the `hdr+8` fences queued by the three RW LockDiscardRename paths (Q-N3); `D3DResourceIsBusy_820FDE30`; `GpuScalerSwapQueueAdvance_8210AB30` (XDK internal) | fence invariant, XDK compare semantics on `hdr+8` / `dev+0x2A1C` |
| L08 | IsBusy | return | `D3DTextureLockLevel_8261C430`, `RwVertexBuffer_Lock_8261F3B8` | last native use |
| L09 | Block* waits | timing | `826160B8`; `822EB348`, `8235F488`, `8261FAE0`; 7 BlockUntilNotBusy callers | return after real completion; no deadlock |
| L10 | QueryGetData S_OK/S_FALSE + data | return, out | `HudGlowOcclusionQuery_82304058`, `OpticalFX_DrawOcclusionFadedFlare_82721118` | latency model, XDK units |
| L11 | QueryCreate pointer / 0 | return | `827799B0`, `82779A98` | 0 for unsupported types |
| L12 | EndTiling HRESULT | return | `8277B998`, `8260A6B0`, `82611FB8` | 0 |
| L13 | Device pointer, layout, `dev+52` | out, field | `0x8299ADF4`; 72 inline writers; VdGlobalDevice cell | XDK layout, published the same way |
| L14 | Resource headers + backing | return (memory) | kept setters, getters, IsSet, GetType, B5 code | byte-identical; same EDRAM bases |
| L15 | Release refcount, destroy timing | return, timing | 50 fns / 102 sites | free after the last use retires |
| L16 | RT/DS/viewport/scissor shadows | device field | getters `820F32B0/32F0/2A38/27E8`, ClearImpl, IsSet | written exactly as the XDK does |
| L17 | Surface header `+0x1C` format toggle | memory | SetRenderTarget, Resolve, native draw | read at draw time |
| L18 | Display mode fields | device field | GetDisplayMode (`82471A50`), `8210A9F8` | filled by native CreateDevice |
| L19 | Present pacing | timing | the game frame loop | one frame in flight; same interval; threshold honoured |
| L20 | Movie kick callback | callback | movie player in-flight counter (`player+368`) | after host completion, at raised IRQL |
| L21 | BeginShaderConstantF4 pointer | return | Lion `827333E0` | staging buffer applied per tile |
| L22 | Dirty masks `dev+0x00..0x27` | device field | B1 inline read-modify-write | not compared; record-only never clears |
| L23 | Resolve destination memory | memory | CPU readers [Q7]; aliasing textures | copy back only where read |
| L24 | GetDeviceCaps | out | D3DX | constant table; nothing to do |
| L25 | Constant and fetch shadows `dev+0x400..0x26FF` | device field | B1 read-modify-write of fetch dwords (`8274E6D8`, `8274FE18`, `8261B050`); XDK draw flush until N7 | native reads only; never stores another format into the range |

## 6. Open questions (new or sharpened by this pass)

| # | Question | Settles | How |
|---|---|---|---|
| Q-N1 | Where is the `k_8_8_8_8_GAMMA` format selected, if `820F1718` is high-precision blend? (A.4 row 4) | N2/N4a gamma conformance | IDA: format writers of RT headers (SetRenderTarget, `82100028`, the B5 builders); Xenia trace RB_COLOR_INFO formats |
| Q-N2 | Are the 145 table-only setters all live, and does any of them write state outside the dirty-flushed runs? | N1 projection completeness | Mode-A record of the device-table `bctrl` sites; the load/store tap (Q7) |
| Q-N3 | Is the InsertFence → `rwdrv+164/168` → `826160B8`/`82616208` path the only consumer of fence values? | L06/L07 obligations | Q7 load tap; IDA on the RW fence reaper |
| Q-N4 | Do the volume-texture entries (`821002D8`, `82100248`, `820FFDA8`, `820FFE38`, `820FFF00`) and the queries/conditional surveys run at all? None has a Xenia count | N5 scope | add them to the next xenia-trace VA list |
| Q-N5 | Is the `SetTextureFetchConstant`/bind else-branch (`dev+10784`) provably dead beyond the golden dumps? | keep condition of 5 entries | sentinel tripwire in mode A (any `820FAC80` call from a kept bind is a hit) |
| Q-N6 | Does Begin/EndTiling run in gameplay (0 in our own 200 s roll is not oracle evidence)? | N5 tiling design (Q4) | Xenia entry counts on `8210D588`/`8210DA98` |
| Q-N7 | The `D3DResource_IsSet` slot list (`820F4190`) lists `dev+11536`, `+12416`, `+12420`: are those every binding a native Set* must keep current? | N7 checklist | Q7 |

Q2 (tessellation), Q3 (Z-pass) and the B3 recorder are settled for the traced span and
carried in §4 "Dead". Q4-Q11 in the roadmap are unchanged.

### 6.1 Answers (2026-10-01, iter-464 native lane; static unless marked)

Evidence: IDA (master, read-only), the recompiled guest, the golden traces. Scripts and raw
outputs: `archive/v1_native/census/` (local). Spot-checked by the lead: InsertFence has
exactly two code callers; `820F4190` jump-tables on `hdr[0]&0xF` through `byte_820BA290`.

| # | Answer | Confidence | Still open / how to settle |
|---|---|---|---|
| Q-N1 | **One selector: `TextureSurfaceFetchConstantBuild_sub_820FF978`** (builds every colour surface header; writes RB_COLOR_INFO into `surf+28` at `0x820FFBB0`, depth at `0x820FFB28`). The colour format comes from the 16-bit table `0x820B3890` indexed by TextureFormat: **62 `k_8_8_8_8_GAMMA_EDRAM` -> 1**; plus the override at `0x820FFB60-0x820FFB9C`: table format 0 with SignX=SignY=SignZ=3, SignW=0, numformat 0 -> 1 (the XDK `MAKESRGBFMT(A8R8G8B8)` case). `820F1718` (HIGHPRECISIONBLENDENABLE) and SetRenderTarget `820F2CA0` only toggle 2<->10 and 3<->12. Texture gamma likewise comes only from the creation format (`82100310` keeps only the sampler bits of the shadow; the B1 writer `8274E6D8` masks clamp bits only). No gamma D3DFORMAT constant exists in XEX code or data (scan with a positive control on A8R8G8B8). **[measured]** 0 of 4,000 traced draws write a format-1 target, 0 of 14,577 fetch constants carry kGamma | high | formats from game data files (render scripts behind `sub_8277B2A0` `a2[8]`, RW raster formats): a whole-run census of `surf+28` bits 16-19 == 1 at `820FF978` exit and fetch dword0 sign == 3 at draw time |
| Q-N2 | Liveness: not static. Writes: every one of the 49 register-shadow stores in the table setters is paired with a dirty-qword store (0 exceptions). **15 setters also write non-register D3D software words** native must mirror or replace: `dev+0x2D3C` (RS 64, 384; read by `82107110` UpdateSurfaceState and `82111EC8`), `+0x2D40` (RS 200; read by SetScissorRect `820F2720` and Clear `821013D0`), `+0x2D44..0x2D50` (RS 212-224; read by SetRenderTarget), `+0x2D5C` (RS 184), `+0x2D64/68/6C` (RS 176/180/188, floats), `+0x2DF0..0x2DF8` (RS 312-320), `+0x33A4` (RS 376; read by `821058B0`, `82111EC8`). The device register shadow is packed in register order from `dev+0x2800` (ENGINE_MAP §6d "NATIVE N0") | high (RS), partial (SAMP: their fetch-shadow store uses a computed address) | Xenia entry counts on the 66 setter VAs, or the mode-A record of the table `bctrl` sites |
| Q-N3 | **No: that chain does not exist.** `D3DDeviceInsertFence_820FDD30` is called only by `D3DPresentSwapOnly_82611FB8` and `D3DPresentResolveThenSwap_82612090`; they store it at rwdrv+164 (`0x8299AE94`) and shift the old one to +168 (`0x8299AE98`); nothing else reads either word (only those wrappers and the zero-init in `RenderDeviceInit_82621258`). The reaper `GpuFenceReaper_82616208` and the waiter `826160B8` walk the list `dword_82998814`, filled by `sub_82616180` from `RwVertexBufferLockDiscardRename_8261BCF0`, `RwTextureLockDiscardRename_8261BDC8` and `sub_8261BEE8` with the **resource-header fence `hdr+8`** (stamped by the XDK from `dev+0x2A1C`). **Ledger L06/L07 change:** the obligation is on `hdr+8` / `dev+0x2A1C` with XDK compare semantics; InsertFence may return any monotonic id | medium-high | the Q7 load tap on `0x8299AE94/98` |
| Q-N4 | Not static for liveness; every volume entry has live-looking callers (`821002D8` <- `D3DVolumeLockBoxBegin_823A1830`; `82100248` <- `823A1830`, `827B7810`; `820FFDA8` <- `822EE4C0`, `82369C80`, `827B7810`; `820FFE38` <- `82369C80`, `8236A0B0`; `820FFF00` <- `RwTextureLockDiscardRename_8261BDC8` (dimension 3D), `8261BF60`). **[measured]** the menu golden trace binds a 64x64x64 k_8_8_8_8 volume texture in 1,626 draw bindings (cubes in 1,559): volumes are live in the menu era | - | the five VAs plus `QueryIssue 82102BC8` / `QueryGetData 82102DC8` in the next xenia-trace VA list |
| Q-N5 | **Dead.** The only stores to `dev+10784` are the two in `GpuCmdContextBeginTiled_820FAEE0` (`0x820FB0A4` = -1, `0x820FB0B8` = 0x400000), whose only caller is the dead recorder `82304C38`; the device is `memset(dev,0,20480)` at creation (`0x820F3510`); no wide store overlaps the word. The six readers test `dev+10780` first, so the branch is dead twice over natively | high | stores through computed addresses (state-block copies) are not excluded mechanically: the mode-A tripwire on `820FAC80` from a kept bind |
| Q-N6 | Not static. BeginTiling `8210D588` has four call sites: `RenderTargetSetBindAndClear_82608E18` (x2), `RwCamera_BeginUpdateBuildMatrices_82613A38` (gated by `82608CD8()` and a flag), and the render-script case in `RenderScriptRunTechnique_8277B998` (`0x8277C60C`, gated by tile count `[r20+32] > 1`). EndTiling `8210DA98`: `D3DPresentSwapOnly 82611FB8` (when `dword_8299AE0C == 1`), `D3DDeviceResolve_8260A6B0`, the render-script case at `0x8277C5D0`. Z-pass is a different render-script case (`0x8277C644/0x8277C660`), so live Z-pass says nothing about tiling | high (sites) | Xenia entry counts on `8210D588`/`8210DA98`; log `[r20+32]` at `0x8277C618` |
| Q-N7 | **Eight slot groups, not three.** `820F4190(res, dev)` jump-tables on `hdr[0]&0xF`: VB -> `dev+12188` x16 (streams); IB -> `+12164`; texture -> `+12272` x26; surface -> RT0 `+12168` or depth `+12184` (RT1-3 not checked); vertex declaration `+11536`; VS `+12420`; PS `+12416`; types 8/9 return 0. Callers are game RenderWare unbinds (`RwDeviceUnbindTextureAllSlots_826131A8`, `RwDeviceUnbindVertexBufferAllStreams_82613240`, `8261B668`). A native Set* keeps all eight current | high | - |

## 7. Independent verification (2026-10-01, static)

A second pass re-derived classes from IDA before reading this sheet's evidence, then
checked the keep set and the ledger mechanically. Scratch scripts are in
`archive/v1_native/n0/verify/` (gitignored): `closure.py` and `reach.py`.

**Blind re-derivation: 30 entries, 30 class matches.**

| Class | Entries checked |
|---|---|
| cpu-only-keep (14) | `820F2A50` SetStreamSource, `82100310` SetTextureFetchConstant, `820F4920` SetVSConstantF, `820F0F88` SlopeScaleDepthBias, `820F1718` HIGHPRECISIONBLENDENABLE, `820F0588` ALPHABLENDENABLE, `820F16C8` VIEWPORTENABLE, `820F1A80` HALFPIXELOFFSET, `820F5438` SetVertexDeclaration, `820F3618` AcquireThreadOwnership, `82101FE8` GetDisplayMode, `820F2B78` GetStreamSource, `820F32B0` GetRenderTarget, `820F4E88` PS bind |
| emitter-replace (7) | `820F2810` SetViewport, `820F2720` SetScissorRect, `820F2FD0` SetDepthStencilSurface, `820F2C98` thunk to SetRenderTarget, `820FBBA0` EndVertices, `82101998` Clear, `8210DF38` BeginZPass |
| resource-sync-replace (6) | `820F4280` InvalidateGpuCache, `820F4000` VB Unlock, `820F40F0` IB Lock, `820FFF00` volume LockBox thunk, `820FDD30` InsertFence, `821029A8` QueryRelease |
| dead (3) | `820F1A10`, `820FC478` (only caller `82786B60`; their other xref is `.pdata`), `820FB5A8` (only caller `82304C38`) |

Spot checks that held:
- `820FFF00` reaches the lock core through `TextureSubresourceOffset3D_820FF5C0` →
  `820FF468` → `GpuRingEmitResourceUse_820F3988`.
- `820F2720` ends in an unconditional tail call to `D3DDeviceEmitScissorTileRects_820F0208`.
  That function writes type-0 or type-3 packets at `dev+40` on both of its branches.
- 100 Unleashed / Xenia `file:line` citations resolve to existing lines, plus 33 short
  `gpu/...` forms.

**Keep set: no ring or MMIO, transitive.** The direct-call closure (`bl` plus tail-call
`b`) was computed for all 147 keep rows and all 146 table-only setters against two
seeds, both limited to the D3D core:
- ring writers: a `dev+40` vs `dev+48` cursor check, a call to `820FD8C0`, or a type-3
  `lis 0xC0xx`;
- MMIO: `lis 0x7FC8..0x7FCF`.

Results:
- 144 keep rows reach neither seed.
- The 145 shadow-only table setters have no callees at all. `820F0F70` reaches the
  scissor emitter, as stated in §2.3.
- Three keep rows reach the ring only through another entry:
  - `82112D18` ApplyStateBlock reaches SetScissorRect and SetViewport;
  - `82112C78` StateBlock_Release and `82112CD0` StateBlock_Capture reach
    `D3DResourceRelease_820F3DA0` → destroy dispatch → `GpuBlockOnFence`.
  
  These stay keep, because the replaced entry's strong-def catches the internal `bl`.
  The TSV rows now say so in `keep_condition` (the two Release paths had none).
- The kept binds (SetStreamSource, SetIndices, SetTextureFetchConstant and the two
  shader binds) reach `GpuBufAllocChainedSegment2008_820FAC80` → `GpuCmdBufAllocate` →
  `GpuRingWaitForSpace`, but only on the `dev+10784` branch (Q-N5). The first seed run
  also flagged `RtlFreeHeap_8246D368`: its heap fields at +40/+48 match the cursor
  pattern. That was a false positive, so the seeds were limited to the D3D core.

**Ledger coverage of B1 and the game's read-backs.**
`gfx_devglobal_access.json` (57 functions) and `gfx_inline_sig_hits.json` (76; the union is
81 with false positives) touch these fields:
- the dirty qwords `dev+0/8/0x10` (L22);
- the VS/PS float shadows (written, never read back);
- fetch-constant dwords 0, 3 and 5, which are read-modify-written by `8274E6D8`, `8274FE18`
  and `8261B050`.

No ledger row covered that last read-back, so **L25** was added. Game reads of the binding
slots, the display mode and the fence fields were already covered by L04/L05/L16/L18.

**Corrections made by this pass:**
- §3 item 4: the bit order of the dirty map (LSB-0, register index falling as the bit
  rises), with six register witnesses;
- the bits 49-52 entry, which was mislabelled "derived surface state";
- `gfx/native_device` written as future work;
- §2.3: table completeness, the two stubs, and the title-terminate callback;
- L25;
- `keep_condition` on `82112C78` and `82112CD0`.

**Still missing / not checked here:**
- **Indirect calls inside keep closures.** Their `bctrl` targets were not resolved. They
  are all `XMemAlloc`/`XMemFree` dispatch (`8246AFD8`), shader-builder callbacks, or hang
  reporting, but no proof says that none of them reaches the ring.
- **Runtime liveness.** The 145 table-only setters (Q-N2) and the `dev+10784` branch
  (Q-N5) need a mode-A record or Xenia counts.
- **Q-N1** is unchanged.
