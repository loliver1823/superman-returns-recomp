# Xenia EDRAM / render-target / viewport / resolve contract (extracted 2026-09-29)

The reference semantics the PM4 backbone ports (docs/RENDERER_PM4_HYBRID.md P1).
All citations are relative to `reference/xenia-canary/src/xenia/gpu/`. Extracted from
source, not re-derived; when our code disagrees with this file, our code is the suspect.

**Quirks to know first**
- `IsOwnedBy` is effectively `range.render_target == key` (the host-depth branch can
  never be true) - section 2.
- If the pixel shader writes depth, the host viewport Z range is [0,1] and ZSCALE/ZOFFSET
  are not applied - section 3.
- Bin predication is a single test: a predicated packet executes iff
  `(bin_select & bin_mask) != 0`; the `*_BIN` draw opcodes are unimplemented - section 5.
- Which dword of a 64bpp clear comes from RB_COLOR_CLEAR vs RB_COLOR_CLEAR_LO is marked
  unverified in Xenia - section 4.

## 1. RenderTargetKey and per-draw key computation

Key layout (`render_target_cache.h:251-262`, one uint32; `key == 0` = empty, `:278`):

| bits | field |
|---|---|
| 0-10 | `base_tiles` (EDRAM = 2048 tiles) |
| 11-18 | `pitch_tiles_at_32bpp` |
| 19-20 | `msaa_samples` (0=1x, 1=2x, 2=4x) |
| 21 | `is_depth` |
| 22-25 | `resource_format` (ColorRenderTargetFormat / DepthRenderTargetFormat) |

- 64bpp formats: `16_16_16_16`, `16_16_16_16_FLOAT`, `32_32_FLOAT` (`xenos.h:321`).
  `GetPitchTiles() = pitch_tiles_at_32bpp << Is64bpp()`.
- `GetWidth() = pitch_tiles_at_32bpp * (80 >> (msaa >= 4x))` (`:306-311`) - 32 and 64bpp
  targets have the same pixel width; 64bpp spans twice the tiles.
- EDRAM (`xenos.h:453-463, 1022-1029`): tile = 80x16 **samples**; 2048 tiles; addressing
  wraps mod 2048 (`xenos.h:269`). 2x MSAA pixel = 1x2 samples; 4x = 2x2. Resolve alignment 8
  px; max resolve 16384-8. **Depth tiles store their two 40-sample halves swapped**
  (`xenos.h:262-267`, `shaders/edram.xesli:42-48`).
- Pitch (`render_target_cache.cc:585-602`):
  `pitch_tiles_at_32bpp = ((surface_pitch << (msaa >= 4x)) + 79) / 80;`
  (= `xenos::GetSurfacePitchTiles`, which also shifts left by 1 for 64bpp,
  `xenos.h:465-476`). `surface_pitch == 0` = no rasterization.
- Storage-format normalization: `GetStorageColorFormat` (`xenos.h:392-402`) maps
  `k_2_10_10_10_AS_10_10_10_10 (10) -> k_2_10_10_10 (2)` and
  `k_2_10_10_10_FLOAT_AS_16_16_16_16 (12) -> k_2_10_10_10_FLOAT (3)`.
  `GetColorResourceFormat` (`render_target_cache.h:679-686`) maps `8_8_8_8_GAMMA -> 8_8_8_8`
  only when gamma is not stored separately (D3D12 default stores GAMMA as R16G16B16A16_UNORM,
  `d3d12/d3d12_render_target_cache.cc:1688-1690, 1816`).
  Color key format = `GetColorResourceFormat(GetStorageColorFormat(fmt))` (`:669-670`).
  Depth key format = `depth_format` (0 = D24S8, 1 = D24FS8, float 20e4 in [0,2)).
- Bound targets (`render_target_cache.cc:632-673`, inputs from `draw_util.cc`):
  - Rasterization (`draw_util.cc:35-58`): `edram_mode` in {ColorDepth 4, DepthOnly 5},
    `vs_export_mode != kMultipass`, `surface_pitch != 0`, and for polygons not both culls.
  - Normalized depth control (`draw_util.cc:60-79`): whole register = 0 unless edram_mode
    is 4/5; `z_enable && !z_write_enable && zfunc == Always` -> `z_enable = 0`. Depth bound
    iff `z_enable || stencil_enable`.
  - Normalized color mask (`draw_util.cc:727-768`): 0 unless edram_mode == 4; per RT the PS
    statically writes: `m = (RB_COLOR_MASK >> 4i) & ((1 << components) - 1)`; skip if 0, else
    OR in non-existent components. RT bound iff its nibble != 0.
  - Same-base elimination (`:698-712`): for each bound color RT in index order, any
    higher-index color RT and depth with the same base is unbound (lower index wins; depth
    loses to color).
- Host RT size: width = `GetWidth() * scale_x`; height (`render_target_cache.cc:988-1014`):
  ```cpp
  tile_rows = (2048 + pitch_tiles_at_32bpp - 1) / pitch_tiles_at_32bpp;
  max_height_scaled = min(8192 * scale_y, host_max_height);
  tile_rows = min(tile_rows, (max_height_scaled << (msaa >= 2x)) / (16 * scale_y));
  height = tile_rows * (16 >> (msaa >= 2x));
  ```
  Example: 1280-wide 1x -> 16 tiles pitch -> 1280x2048. DXGI (`d3d12_render_target_cache.cc
  :1681-1788`): D24S8 R24G8_TYPELESS; D24FS8 R32G8X24 (D32_S8); 2_10_10_10_FLOAT
  R16G16B16A16_FLOAT; 16_16 / 16_16_16_16 SNORM views on TYPELESS. 2x MSAA falls back to 4
  samples if native 2x is unsupported.

## 2. Ownership and overlap

- Map (`render_target_cache.h:607-717`): `std::map<start_tile, OwnershipRange{end_tiles,
  render_target key, host-depth keys}>`, gap-free, initially `[0,2048)` owned by the empty
  key (`.cc:501-507`); adjacent equal owners merge.
- Length claimed per draw (`.cc:749-846`): `height_used = min(GetRenderTargetHeight(),
  DrawExtentEstimator::EstimateMaxY())`. EstimateMaxY (`draw_extent_estimator.cc:269-354`)
  starts from scissor `br_y` (+ window y offset unless `window_offset_disable`), min screen
  scissor `br_y`, clamp >= 0; with clipping on also min
  `trunc(max(0, [win_y_off if vtx_window_offset_enable] + [0.5 if pix_center == D3DZero] +
  YOFFSET + |YSCALE|))`. `length_used_tiles_at_32bpp = ceil((height_used << (msaa >= 2x)) /
  16) * pitch_tiles_at_32bpp`. Targets sorted by base; each length =
  `min(min(length_used << is64, rt_max_distance_tiles_at_64bpp >> (is64 ^ 1)),
  next_sorted_base (or 2048 + first_base for the last) - base)`.
- ChangeOwnership (`.cc:1510-1667`), per range intersecting the claimed span (wrap = two
  extents): skip if already owned by dest; else split at extent edges; if the previous owner
  is non-empty and != dest, append `Transfer{start, end, source RT}` (merged when contiguous,
  same source); claim; merge neighbours.
- **Transfer = copy before the draw**: rectangles that reinterpret the old RT's guest bit
  pattern into the new format (`.h:235-249`; `PerformTransfersAndResolveClears`,
  `d3d12_render_target_cache.cc:1185-1197`). **Empty previous owner = discard** (nothing
  copied). Rectangles (`GetRangeRectangles`, `.cc:332-416`): tile width px =
  `80 >> ((msaa >= 4x) + is64)`, tile height = `16 >> (msaa >= 2x)`; up to 3 rects (first
  partial row, full rows, last partial row).
- Resolve **clears** also ChangeOwnership, with the clear rect as a cutout (transfers fully
  inside it are skipped, `.cc:1573-1577, 1331-1342`). Resolve **copies do not** change
  ownership: they dump whichever RT owns each tile of the span into the EDRAM buffer
  (`GetResolveCopyRectanglesToDump`, `.cc:1045-1129`), and the resolve shader interprets
  that data with the copy source's RB_COLOR_INFO format.

## 3. Viewport - GetHostViewportInfo (`draw_util.cc:171-549`)

Inputs (`:307-333`):
```cpp
scale_xy = { vte.x_scale_ena ? XSCALE : 1, vte.y_scale_ena ? YSCALE : 1 };
scale_z  = vte.z_scale_ena ? ZSCALE : 1;
offset_base_xy = { vte.x_offset_ena ? XOFFSET : 0, vte.y_offset_ena ? YOFFSET : 0 };
offset_z = vte.z_offset_ena ? ZOFFSET : 0;
offset_add_xy = (su_sc_mode_cntl.vtx_window_offset_enable ? window_offset : 0)
              + (half_pixel_offset && pa_su_vtx_cntl.pix_center == kD3DZero ? 0.5 : 0);
xy_max_unscaled = x_max / scale_x, y_max / scale_y;   // D3D12: 32767
```
Clipping enabled (`:382-428`), per axis:
```cpp
offset = offset_base + offset_add; s = scale; sa = |scale|;
a0 = uint(clamp(offset - sa, 0, max)); a1 = uint(clamp(offset + sa, 0, max));
viewport TopLeft = a0 * res_scale, Extent = (a1 - a0) * res_scale;
if (a1 - a0) { ndc_scale  = s * 2 / (a1 - a0);
               ndc_offset = (offset - (a0 + (a1 - a0) * 0.5)) * 2 / (a1 - a0); }
else         { ndc_scale = 1; ndc_offset = 0; }   // + empty scissor
```
Z (`:431-514`): `dx_clip_space_def` -> host Z range `[offset_z, offset_z + scale_z]`, ndc z
1/0; else (GL -W..W) host offset `offset_z - scale_z`, host scale `2*scale_z`, ndc z 0.5/0.5.
PS writes depth -> `z_min = 0, z_max = 1`, guest Z transform not applied. Else `z_min =
saturate(hoff)`, `z_max = saturate(hoff + hscale)`; if `z_min > z_max` (no reverse-Z on
D3D12) swap, `ndc_scale.z = -ndc_scale.z`, `ndc_offset.z = 1 - ndc_offset.z`.
Clipping disabled (`clip_cntl.clip_disable`, `:350-376`): `E = min(8192, xy_max_unscaled)`;
viewport offset 0, extent `E * res_scale`; `ndc_scale = scale_xy * (2 / E)`; `ndc_offset =
(offset_base - E * 0.5 + offset_add) * (2 / E)`; z: [0,1], `ndc_scale.z = scale_z`,
`ndc_offset.z = offset_z`.
D24FS8 (`:517-537`, only with z_enable): with host RTs `z_min *= 0.5; z_max *= 0.5`
(guest [0,2) -> host [0,1)).
Y flip: `origin_bottom_left` (true on D3D12) negates `ndc_scale.y` and `ndc_offset.y`
(`:541-544`).
Applied at the end of the VS (`dxbc_shader_translator.cc:1005-1068`): `if (!vtx_w0_fmt) w =
1/w; if (vtx_xy_fmt) xy *= w; if (vtx_z_fmt) z *= w; pos.xyz = pos.xyz * ndc_scale +
ndc_offset * pos.w`.

Scissor - GetScissor (`draw_util.cc:640-725`):
```cpp
tl = WINDOW_SCISSOR_TL.(tl_x, tl_y); br = WINDOW_SCISSOR_BR.(br_x, br_y);
if (!WINDOW_SCISSOR_TL.window_offset_disable /*bit31*/) { tl += win_off; br += win_off; }
tl = max(tl, SCREEN_SCISSOR_TL); br = min(br, SCREEN_SCISSOR_BR);
if (clamp_to_surface_pitch /*true for draws*/) { tl.x = min(tl.x, pitch); br.x = min(br.x, pitch); }
tl = max(tl, 0); br = max(br, tl); offset = tl; extent = br - tl;
```

## 4. Resolve (`draw_util.cc:926-1318`)

Trigger: `RB_MODECONTROL.edram_mode == kCopy (6)` -> IssueCopy -> Resolve
(`d3d12/d3d12_command_processor.cc:2514-2518`); `copy_command` must be Raw 0 or Convert 1.

Source rectangle:
1. Vertex fetch constant 0 must have `size == 6` (three xy pairs); endian-swap by
   `fetch.endian`; `+0.5` if `pix_center == kD3DZero`; to 16.8 fixed point (`:953-974`).
2. min/max of the three vertices; `v = (fixed + 127) >> 8`; x1/y1 exclusive (`:977-991`).
3. `+ window offset` if `vtx_window_offset_enable` (`:993-1001`).
4. Clamp to `GetScissor(regs, clamp_to_surface_pitch = false)` (`:1004-1014`).
5. `x0 &= ~7; y0 &= ~7; x1 = align(x1, 8); y1 = align(y1, 8)` (`:1024-1027`).
6. Clamp x to `surface_pitch & ~7`; height <= 16376; empty = no-op (`:1042-1069`).

Source select: `is_depth = copy_src_select >= 4`; 0-3 = RB_COLOR_INFO / COLOR1 / COLOR2 /
COLOR3 (`registers.cc:16-21`). `copy_sample_select`: k0..k3 = 0..3, k01 = 4, k23 = 5,
k0123 = 6. Sanitize (`:839-880`): 1x -> k0; 2x: k2->k0, k3->k1, >k01 -> k01, depth k01->k0;
4x: >k0123 -> k0123, depth k01/k0123 -> k0, k23 -> k2. Sample position in the EDRAM sample
grid: `(x + ((s >> 1) & 1), y + (s & 1))` (`edram.xesli:32`) - 2x: 0 top, 1 below; 4x:
0=TL 1=BL 2=TR 3=BR. Averaging (`resolve.xesli:583-636`): sum 2 or 4 samples, then
`* exp_bias_factor * 0.5` or `* 0.25` (linear on raw values, gamma included).

EDRAM source location (`:1209-1287`):
```cpp
xs = x0 << (msaa >= 4x); ys = y0 << (msaa >= 2x);
edram_offset_x_div_8 = (xs % 80) >> ((msaa >= 4x) + 3);
edram_offset_y_div_8 = (ys % 16) >> ((msaa >= 2x) + 3);
off_tiles = (ys / 16) * GetSurfacePitchTiles(pitch, msaa, false) + xs / 80;
color: base = color_base + (off_tiles << is64), pitch_tiles = surface_pitch_tiles << is64;
depth: base = depth_base + off_tiles;
```

Destination (`:1102-1206`): color format = `RB_COPY_DEST_INFO.copy_dest_format`; depth ->
k_24_8 / k_24_8_FLOAT, swap off. `pitch_aligned = align(copy_dest_pitch, 32)`,
`height_aligned = align(copy_dest_height, 32)`. `xmask = (1 << (7 - min(2, bpp_log2 +
is_array))) - 1`; `ymask = is_array ? 31 : (1 << (7 - min(2, bpp_log2))) - 1`
(`xenos.h:1211-1218`). Dest offset `(x0 & xmask) >> 3` (same for y); `dest_base =
RB_COPY_DEST_BASE + Tiled2D(x0 & ~xmask, y0 & ~ymask, pitch_aligned, bpp_log2)` (Tiled3D at
slice 0 for arrays; `copy_dest_slice` applied in the shader). So rect origin (x0,y0) in RT
coordinates (window offset included) lands at texel (x0,y0) of the tiled texture at
RB_COPY_DEST_BASE; D3D9 pre-biases the base (`xenos.h:983-994`).
Endian (`shaders/endian.xesli:38-73`): 8in16 = bytes within 16-bit halves; 8in32 = bswap +
rotl16; 16in32 = rotl16; 8in64 = swap dwords then 8in32; 8in128 = reverse dwords then 8in32.
`copy_dest_swap` = R/B swap. Exp bias factor `2^copy_dest_exp_bias` (signed 6-bit at bit 16),
color only (`resolve.xesli:135-137`); Xenia adds 5 for 16_16 / 16_16_16_16 emulated as -1..1
(`:1272-1282`). Convert = unpack -> *bias -> swap -> pack (`pixel_formats.xesli:209-296`);
bitwise fast path when bias 0, single sample, equivalent formats (`xenos.h:614-639`).

Clear-after-copy (`draw_util.h:688-716`; `d3d12_render_target_cache.cc:1427-1451,
5350-5497`), always after the copy:
- depth clear iff `depth_clear_enable` (bit 9), even on a color copy: depth RT at
  `depth_base`; RB_DEPTH_CLEAR depth = `(v >> 8) & 0xFFFFFF`, stencil = `v & 0xFF`; host
  D24FS8 written as `f20e4 * 0.5`.
- color clear iff `!is_depth && color_clear_enable` (bit 8): the source color RT; 32bpp raw
  RB_COLOR_CLEAR bits; 64bpp low dword RB_COLOR_CLEAR, high RB_COLOR_CLEAR_LO (unverified).
- Area = the same 8-aligned rect in each target's coordinates, clamped to pitch and RT
  height (`render_target_cache.cc:1196-1217`); both cleared -> tile ranges trimmed not to
  overlap (`:1266-1289`).

## 5. Bin predication (`pm4_command_processor_implement.h`)

- Predicate = bit 0 of the type-3 header: packet skipped iff `(bin_select_ & bin_mask_) ==
  0`; a predicated `PM4_XE_SWAP` is always skipped (`:408-418`). Applies to every type-3
  opcode incl. draws.
- `SET_BIN_MASK_LO/HI` (0x60/0x61), `SET_BIN_SELECT_LO/HI` (0x62/0x63): one dword each,
  replacing the low/high 32 bits (`:511-532`). `SET_BIN_MASK` (0x50) / `SET_BIN_SELECT`
  (0x51): two dwords, first = high half (`:533-545`).
- Initial values `0xFFFFFFFF` (low 32 bits only, `command_processor.h:524-525`), never
  reset. `DRAW_INDX_BIN (0x34)`, `DRAW_INDX_2_BIN (0x35)`, `SET_BIN_BASE_OFFSET (0x4B)` are
  unimplemented -> skipped (`:564-565, 603-610`).

## 6. Registers (`register_table.inc:1231-1369`) and bitfields (`registers.h`)

| Register | Addr | Register | Addr |
|---|---|---|---|
| RB_SURFACE_INFO | 0x2000 | PA_SC_WINDOW_OFFSET | 0x2080 |
| RB_COLOR_INFO | 0x2001 | PA_SC_WINDOW_SCISSOR_TL | 0x2081 |
| RB_DEPTH_INFO | 0x2002 | PA_SC_WINDOW_SCISSOR_BR | 0x2082 |
| RB_COLOR1_INFO | 0x2003 | PA_SC_SCREEN_SCISSOR_TL | 0x200E |
| RB_COLOR2_INFO | 0x2004 | PA_SC_SCREEN_SCISSOR_BR | 0x200F |
| RB_COLOR3_INFO | 0x2005 | PA_CL_VPORT_XSCALE | 0x210F |
| RB_COLOR_MASK | 0x2104 | PA_CL_VPORT_XOFFSET | 0x2110 |
| RB_DEPTHCONTROL | 0x2200 | PA_CL_VPORT_YSCALE | 0x2111 |
| PA_CL_CLIP_CNTL | 0x2204 | PA_CL_VPORT_YOFFSET | 0x2112 |
| PA_SU_SC_MODE_CNTL | 0x2205 | PA_CL_VPORT_ZSCALE | 0x2113 |
| PA_CL_VTE_CNTL | 0x2206 | PA_CL_VPORT_ZOFFSET | 0x2114 |
| RB_MODECONTROL | 0x2208 | RB_COPY_DEST_INFO | 0x231B |
| PA_SU_VTX_CNTL | 0x2302 | RB_DEPTH_CLEAR | 0x231D |
| RB_COPY_CONTROL | 0x2318 | RB_COLOR_CLEAR | 0x231E |
| RB_COPY_DEST_BASE | 0x2319 | RB_COLOR_CLEAR_LO | 0x231F |
| RB_COPY_DEST_PITCH | 0x231A | | |

- RB_SURFACE_INFO (`:670`): `surface_pitch` [0:13] px, `msaa_samples` [16:17], `hiz_pitch`
  [18:31].
- RB_COLOR_INFO / COLOR1-3 (`:736`): `color_base` [0:10] tiles, bit 11 separate,
  `color_format` [16:19], `color_exp_bias` [20:25] signed. Formats: 0 8888, 1 8888_GAMMA,
  2 2_10_10_10, 3 2_10_10_10_FLOAT, 4 16_16, 5 16_16_16_16, 6 16_16_FLOAT,
  7 16_16_16_16_FLOAT, 10 2_10_10_10_AS_10_10_10_10, 12 2_10_10_10_FLOAT_AS_16_16_16_16,
  14 32_FLOAT, 15 32_32_FLOAT.
- RB_DEPTH_INFO (`:835`): `depth_base` [0:10], bit 11 separate, `depth_format` [16].
- RB_COLOR_MASK: 4 bits per RT (RGBA), RT0 [0:3] .. RT3 [12:15].
- RB_DEPTHCONTROL (`:799`): stencil_enable [0], z_enable [1], z_write_enable [2], zfunc
  [4:6], backface_enable [7], front stencil [8:19], back stencil [20:31].
- RB_MODECONTROL: `edram_mode` [0:2] (0 NoOp, 4 ColorDepth, 5 DepthOnly, 6 Copy).
- RB_COPY_CONTROL (`:853`): copy_src_select [0:2] (4 = depth), copy_sample_select [4:6],
  color_clear_enable [8], depth_clear_enable [9], copy_command [20:21] (0 Raw, 1 Convert,
  2 ConstantOne, 3 Null).
- RB_COPY_DEST_INFO (`:870`): endian [0:2], array [3], slice [4:6], format [7:12], number
  [13:15], exp_bias [16:21] signed, swap [24].
- RB_COPY_DEST_PITCH (`:887`): copy_dest_pitch [0:13], copy_dest_height [16:29].
- PA_SC_WINDOW_OFFSET (`:610`): window_x_offset [0:14], window_y_offset [16:30], signed 15.
- PA_SC_WINDOW_SCISSOR_TL: tl_x [0:13], tl_y [16:29], window_offset_disable [31];
  _BR: br_x [0:13], br_y [16:29]. PA_SC_SCREEN_SCISSOR_TL/BR: x [0:14], y [16:30], signed 15.
- PA_SU_SC_MODE_CNTL (`:456`): cull_front [0], cull_back [1], face [2], poly_mode [3:4],
  poly offset front/back/para [11]/[12]/[13], msaa_enable [15], vtx_window_offset_enable
  [16], provoking_vtx_last [19].
- PA_CL_VTE_CNTL (`:566`): x scale/offset ena [0]/[1], y [2]/[3], z [4]/[5], vtx_xy_fmt [8],
  vtx_z_fmt [9], vtx_w0_fmt [10].
- PA_CL_CLIP_CNTL (`:534`): ucp_ena [0:5], clip_disable [16], dx_clip_space_def [19].
- PA_SU_VTX_CNTL: pix_center [0] (0 = D3D -> +0.5), round_mode [1:2], quant_mode [3:5].
