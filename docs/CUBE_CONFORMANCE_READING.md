# CUBE CONFORMANCE — a reading, and one default-OFF diagnostic (iter-457, 2026-09-01)

**r2, after a director verdict of OVERSTATED / REWORK on r1.** Three blocking
errors were found in r1; all three are corrected below and each correction is
marked ⛔ **r1 ERROR**. No live roll, no fresh capture, no sweep, no default
change, no real cube producer or upload. `SR_HLE_RESOLVE_STACKED` remains
default OFF. The one renderer change is `SR_HLE_CUBE_NULL_ZERO`, **default OFF**
and diagnostic only — its result is in §7.

This note exists because the director gated the cube design behind reading the
record that already exists. That gate was set for cause: the iter-457 arc
re-derived three things `docs/MIGRATION_API_HLE.md` already recorded, and built
a tool to rediscover one of them. Everything below is either **cited to a line
that resolves**, or marked **UNKNOWN — fresh capture required**.

Every number in the concordance is recomputed by
[`tools/check_cube_layout.py`](../tools/check_cube_layout.py), which is
mutation-validated by [`tools/mutate_cube_layout.py`](../tools/mutate_cube_layout.py)
(21/21 mutants detected, covering face-count, face-stride, mip-address,
endianness and the packed-mip offsets).

    python tools/check_cube_layout.py --self-test                       # 40/40
    python tools/check_cube_layout.py archive/iter456_title_fresh.srht --expect
    python tools/mutate_cube_layout.py                                  # 21/21

---

## 0. What the existing project record already said — and what is SUPERSEDED

Read first, per the gate: `docs/MIGRATION_API_HLE.md` §"THE VOLUME/CUBE SLICE"
(line 1172) **and** its iter-457 correction (line 1442); `docs/ENGINE_MAP.md`
§6g.3c (line 1804) and §6g.4 (line 1878); the cube claims and verdicts in
`docs/CLAIMS_LEDGER.md` (20807, 20882, 20944, 21719-21745).

### STILL STANDING

| Statement | Where | Why it survives |
|---|---|---|
| Cubes are fmt 6 and fmt 18 (DXT1), tiled | MIGRATION 1176-1177 | The four keys in the frozen fixture are all fmt 6; DXT1 cubes are not contradicted, just not present here |
| Cubes pack `size_2d` w:13/h:13 in dword 2, **not** the 3D 11/11/10 split | MIGRATION 1177-1179 | Confirmed against `xenos.h:1283-1289`. A mutant that decodes cubes with the 3D split is detected |
| Cube resource = `TEXTURE_2D` + arraySize 6 + CUBE flag; view = `TEXTURE_CUBE`; faces upload as array slices 0-5 | MIGRATION 1194-1195 | Confirmed twice independently: Unleashed `video.cpp:5630,5640` and Xenia `d3d12_texture_cache.cc:1349-1360, 2122` |
| Shared constants: dwords 0-15 2D / 16-31 3D / 32-47 cube / 48-63 samplers / 64 booleans | MIGRATION 1185-1187 | Byte-for-byte with the reference `SharedConstants` at `video.cpp:160-171` |
| Every SetTexture writes ALL THREE ranges — real index into the matching range, nulls into the other two | MIGRATION 1188-1190 | `video.cpp:3774-3796`. Dimension is a cached property of the created view (`texture->viewDimension`), never re-derived at bind |
| The reference has ZERO untiling; our tiled guest memory is our own work | MIGRATION 1201-1203 | Unleashed loads pre-converted DDS. Xenia is the only layout authority available |
| Zero of 752 `TextureUpload` records are tagged CUBE; each cube key uploads once at 65,536 B | LEDGER 20950-20968 | Reconfirmed. And now **explained**: 65,536 B is exactly ONE face of six (§2) |
| 526 of 1,118 scene-target draws are bound to a real cube at a cube-fetching slot, and all sample the white dummy | LEDGER 20882 (OVERSTATED-narrowed) | The band mechanism is reconfirmed by whole-surface search (§1, row "all-three-band publication") |

### SUPERSEDED — do not inherit

| Superseded statement | Where | What replaces it |
|---|---|---|
| "**Slice order**: … (2) cube: same shape into [+8..+15], DXT1 + 8888, dwords 32-47" — presented as a **backend-shaped** slice | MIGRATION 1204-1207 | **Wrong scope.** It is producer-first: the trace carries neither the CUBE tag nor five-sixths of the pixels (LEDGER 20944). The heap reservation at `d3d12_backend.cpp:28` is still correct and still unused |
| "Census cube DIMENSIONS were garbage until the size_2d fix … prints d=6 by construction now" | MIGRATION 1177-1180 | **`d=6` by construction is the bug, not the fix.** The census hardcodes `d3 = isCube ? 6u : …` (`hle_api_observe.cpp:5290`). Xenia requires `size_2d.stack_depth == 5` and asserts it (`texture_util.cc:52`); a cube whose fetch does not say 5 must REFUSE, not be given a 6. Our four keys do carry 5 — measured, not assumed |
| "the 3D untiler is OUR work, no reference to lean on" | MIGRATION 1201-1202 | Half-superseded. `gfx/xenos_tiling.hpp::tiled2D`/`tiled3D` are line-for-line ports of `texture_address.h:190` / `:213` — there *is* a reference and we already followed it. What has no reference is the **layout** (strides, mip offsets, packed tail), and Xenia is the authority for that too (`texture_util.cc:208-447`) |
| Any framing of the cube gap as a **LUT / grading** problem | iter-457 arc, corrected at MIGRATION 1442-1452 | The volumes are cloud-NOISE. The cube gap is an **environment-map** gap and is a separate hypothesis from both the green sky and the fractured geometry |
| "sub-slice 2 can be done from the frozen fixture" | implied by MIGRATION 1204-1207 | The fixture provably cannot exercise it (LEDGER 20944). **Narrowed below**: §2/§3 show the fixture settles the BASE SPAN without a capture. ⛔ **r1 ERROR** — r1 claimed the fixture settles the layout in full. It does not: face ORDER is still unknown and the mip BYTES still require a capture. What the fixture settles without a capture is the BASE SPAN and the mip EXTENTS, nothing more |

---

## 1. Concordance

Columns: **contract / authority (exact lines) / existing project statement /
our current behavior / deviation / killer test.**

### 1.1 Fetch dimension and the six-face count

| | |
|---|---|
| **Contract** | `DataDimension::kCube` (dword 5 bits 9-10 == 3) means **exactly six faces**, and the fetch must carry `size_2d.stack_depth == 5`. The count comes from the DIMENSION, never from a depth field |
| **Authority** | `xenos.h:1325` (`dimension : 2`); `texture_util.cc:50-56` (the kCube case, `assert_true(fetch.size_2d.stack_depth == 5)` at **:52**); `texture_util.cc:229` (`layout.array_size = 6`) |
| **Project statement** | ENGINE_MAP §6g.3c: `dim = (fc[5] >> 9) & 3`, 3 = CUBE. Census 3,360 CUBE bindings at admitted draws |
| **Our behavior** | Producer decodes the dimension correctly (`hle_api_observe.cpp:5277`) but then **hardcodes** `d3 = isCube ? 6u : …` (`:5290`) without ever reading or checking `stack_depth`. The capture path at `:5306` requires `texDim == 2` (3D only), so cubes fall through to the 2D guard at `:5373` |
| **Deviation** | **The six is asserted, not verified.** A cube fetch that did not say 5 would be silently accepted as six faces |
| **Killer test** | `check_cube_layout.py` refuses a cube fetch with `stack_depth != 5`; the mutant "the stack_depth == 5 Xenos invariant is not enforced" is detected. Measured on the fixture: all four keys carry 5 |

### 1.2 Base versus mip addresses

| | |
|---|---|
| **Contract** | `base_address` and `mip_address` are **page indices** (>>12), in dword 1 bits 12-31 and dword 5 bits 12-31. `mip_page == 0` forces `mip_max_level = 0` — no mips exist regardless of the level field. If `mip_max_level != 0` and `base_page == 0`, the base is absent and `mip_min_level` is forced to ≥1 |
| **Authority** | `xenos.h:1275` (`base_address : 20`), `xenos.h:1327` (`mip_address : 20`); `texture_util.cc:76-99` |
| **Project statement** | ⚠ **NONE.** No existing project document records the mip address for any texture. Our producer never reads dword 5 bits 12-31 at all |
| **Our behavior** | `basePage = (f[1] >> 12) & 0xFFFFF`, `base = basePage * 4096` — correct. **`mip_address` is never read anywhere in the tree** (whole-surface search: no reference to dword-5 bits 12-31 outside the new checker) |
| **Deviation** | We capture base level only and are blind to the mip chain's existence, so we cannot tell a mipped texture from an unmipped one |
| **Killer test** | Mutants "mip_address is read as a byte address instead of a page index" and "mip_page == 0 no longer forces mip_max_level to 0" are both detected |

### 1.3 Packed-mip boundary

| | |
|---|---|
| **Contract** | Mips at and below `GetPackedMipLevel(w,h) = log2_ceil(min(w,h)) - 4` (when > 4, else 0) are stored **packed into one tail**, addressed by `GetPackedMipOffset`, not as independent levels |
| **Authority** | `texture_util.h:41-44`; `texture_util.cc:251-252`, `:359-398` |
| **Project statement** | ⚠ **NONE** |
| **Our behavior** | No concept of packed mips exists in the tree. `packed_mips` (dword 5 bit 11) is never read |
| **Deviation** | Total. For our 128×128 cubes the packed level is **3**, so mips 3..7 share one tail |
| **Killer test** | `packed_mip_level(128,128) == 3` is a fixture. ⛔ **r1 ERROR** — r1 marked the tail extent UNKNOWN. The director rejected that: `GetPackedMipOffset` is vendored and the fetch carries every input, so it was not undecidable, only **unported**. It is now ported (`texture_util.cc:116-205`) and the tail extent is **computed**: 5 fixtures cover both packing arms, the axis flip at `packed_mip >= 3`, the texel→block shift, and the `log2_size > 4` guard; 6 mutants over that math are detected |

### 1.4 Per-face guest stride

| | |
|---|---|
| **Contract** | `array_slice_stride_bytes = align(row_pitch_bytes × z_slice_stride_block_rows, 4096)`, where `row_pitch_bytes = align(align(pitch_texels, block_w)/block_w, 32) × bytes_per_block` and `z_slice_stride_block_rows = align(align(height, block_h)/block_h, 32)`. The load loop advances the guest offset by **exactly this stride**, once per array slice, for `array_size` slices |
| **Authority** | `texture_util.cc:312-352`; the decisive loop `d3d12_texture_cache.cc:1795-1811` (`guest_offset += level_array_slice_stride_bytes_scaled` at **:1810**); `xenos.h:1177-1178` (4096), `xenos.h:1198-1199` (32) |
| **Project statement** | ⚠ **NONE.** MIGRATION 1204-1207 sketches the slice without a stride |
| **Our behavior** | The 2D capture path computes `spanRows × blockPitch × blockBytes` for ONE image (`hle_api_observe.cpp:5410-5417`). For 128×128 k_8_8_8_8 tiled that is 65,536 B — **numerically identical to one face stride, and 1/6 of what the texture is** |
| **Deviation** | We capture face 0 and call it the texture |
| **Killer test** | ⛔ The director forbade deriving this from `6*w*h*bpp`. That intuition gives the **right** answer for 128×128 and the **wrong** answer for 33×33 (196,608 vs 26,136) — the fixture asserts the disagreement, and the mutant that substitutes the intuition is detected |

### 1.5 Tiled addressing and endian order

| | |
|---|---|
| **Contract** | Tiled surfaces address through `Tiled2D`; the guest's byte order is `endianness` (dword 1 bits 6-7), where 2 = `k8in32` — a four-byte word swap that must be applied at use |
| **Authority** | `texture_address.h:190-209` (Tiled2D), `:174-184` (TiledCombine); `texture_util.cc:450-477` (upper bound); `xenos.h:1271` (`endianness : 2`) |
| **Project statement** | MIGRATION 1201-1202 called the untiler "our work"; the iter-457 correction (1447-1450) records that `tiled3D` is a line-for-line port of Xenia's. **Both `tiled2D` and `tiled3D` are ports** |
| **Our behavior** | `gfx/xenos_tiling.hpp:35-48` (`tiled2D`) matches `texture_address.h:190` line for line. `hleUploadHostTextureFmt` applies block-grid tiled2D + `xeSwap32`, and the endian mode rides `fmtBits` bits 12-13 |
| **Deviation** | **None known for 2D.** MIGRATION 1471-1478 independently validated the transform by offline decode of real textures. Cube faces are the same 2D tiling per face — nothing new is needed |
| **Killer test** | Two endianness mutants (wrong bit offset; assumed kNone) are both detected. `tiled_upper_bound_2d(128,128,128,2) == 65536` is a fixture |

### 1.6 Host resource and SRV shape

| | |
|---|---|
| **Contract** | Cube resource = `TEXTURE2D` with `DepthOrArraySize = 6`; SRV = `D3D12_SRV_DIMENSION_TEXTURECUBE`. Faces are array slices 0-5; subresource index = `mip + face × mipLevels` |
| **Authority** | Xenia `d3d12_texture_cache.cc:1349-1360` (2D resource, array size from the key), `:2122-2125` (TEXTURECUBE SRV). Unleashed `video.cpp:5630` (`arraySize × 6`), `:5640` (CUBE flag), `:5738` (`subresourceIndex % numMips`, `/ numMips`), `:1929-1935` (the null cube: 2D, arraySize 6, CUBE flag, TEXTURE_CUBE view) |
| **Project statement** | MIGRATION 1194-1195, correct and unchanged |
| **Our behavior** | ⛔ **r1 ERROR** — r1 said no cube resource path exists at all. **It does, for the dummy**: `d3d12_backend.cpp:2421` creates a `TEXTURE2D` with `DepthOrArraySize = 6`, `:2461` uploads all six faces, and `:2495-2502` creates eight `D3D12_SRV_DIMENSION_TEXTURECUBE` descriptors at `[kTex2DSlots+8..+15]`. What does not exist is a path for a **guest** cube: `hleUploadHostVolume` creates `TEXTURE3D` and `hleUploadHostTextureFmt` creates plain 2D |
| **Deviation** | The host cube SHAPE is already correct and already proven to work — the gap is upload/cache/bind of real guest pixels, not resource creation |
| **Killer test** | UNKNOWN until the code exists — this row is a design input, not a measured deviation |

### 1.7 All-three-band bind publication

| | |
|---|---|
| **Contract** | On every texture bind, write ALL THREE ranges for that slot: the real descriptor index into the range matching the view's dimension, and the per-dimension NULL index into the other two |
| **Authority** | `video.cpp:3780-3796` (`SetTextureInRenderThread`), `:3798-3805` (`SetSurface` — a surface is 2D and explicitly NULLs 3D and cube) |
| **Project statement** | MIGRATION 1188-1190; `d3d12_backend.cpp:4469-4470` names the cube table as "the named open gap" |
| **Our behavior** | `shared[s] = d.texSlot[s]` (`d3d12_backend.cpp:4472`), `shared[16+s] = d.texSlot3D[s]` (`:4515`). Nothing writes `shared[32..47]` in either publication site (live HLE `:4465`, PM4 replay `:6638`), so all 16 cube slots carry index 0 |
| **Deviation** | ⛔ **r1 ERROR** — r1 concluded "no cube at all". Index 0 is not nothing: it selects the **existing white 6-face cube dummy** (§1.6). Every cube-fetching draw therefore samples a valid, uniformly WHITE cube. The mechanism is the absence of a real guest-cube upload and of any nonzero index publication — not the absence of a cube |
| **Killer test** | §7. Forcing that existing dummy's mapping to zero changes 15 of 16 frames, which **proves the dummy is on the live sampling path** — something the absence argument could never have shown, and which r1's wrong reading would have made nonsense of |

### 1.8 Null / reset behavior

| | |
|---|---|
| **Contract** | Three reserved null descriptors (2D / 3D / CUBE), each a 1×1 R8_UNORM with an all-ZERO component mapping; the cube null is a 2D resource with arraySize 6 and the CUBE flag. All 48 index dwords are reset to the nulls at frame start |
| **Authority** | `video.cpp:1898-1945` (null creation; the cube case at **:1929-1935**), `:1607-1612` (the 16×3 reset) |
| **Project statement** | MIGRATION 1183-1185, accurate |
| **Our behavior** | **Different in form.** We do not reset a persistent table per frame; `q.texSlot[s]` / `q.texSlot3D[s]` are resolved **per draw** from `s_texSlot` / `s_texSlot3D`, defaulting to 0 when the key is absent (`hle_video.cpp:784-788`). Index 0 is a WHITE 1×1 dummy, not a zero-mapped black one |
| **Deviation** | ⛔ **r1 ERROR** — r1 listed "there is no cube dummy of any kind" as a second deviation. There is one, and it is correctly shaped. **The one real deviation is the component mapping**: ours exposes uploaded WHITE texels through `D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING`; the reference forces `(ZERO,ZERO,ZERO,ZERO)`. A missing texture multiplies as 1.0 here and 0.0 there |
| **Killer test** | §7 — measured, not flagged. ⚠ And a delta there is **not** evidence that zero is the correct substitute for a real guest cube; the two must not be merged |

### 1.9 Producer snapshot extent

| | |
|---|---|
| **Contract** | To reproduce a cube, the producer must snapshot `array_slice_stride_bytes × 6` from `base_address`, plus `mips_total_extent_bytes` from `mip_address` if mips are wanted |
| **Authority** | §1.4 above; `d3d12_texture_cache.cc:1795-1811` |
| **Project statement** | LEDGER 20944 established the shortfall; this note supplies the number |
| **Our behavior** | 65,536 B from `base_address`. The arena slot cap is 8 MiB (`hle_video.hpp:598`), so 393,216 B is **6% of one slot** — capacity is not the obstacle |
| **Deviation** | We capture 1/6 of the base and 0 of the mips |
| **Killer test** | The conservation equation in §3 — the guest's own `mip_address` proves the six-face extent without a fresh capture |

### 1.10 Trace payload schema

| | |
|---|---|
| **Contract** | A trace record must carry enough to rebuild the resource: dimension, face count, per-face stride, format, tiling, endianness, and the pixels |
| **Authority** | Our own format. `gfx/hle_video.hpp:449-451` — `TextureUpload { key, arenaOffset, bytes, dims, fmtBits, pitch }`, six of the seven available union dwords |
| **Project statement** | `hle_video.hpp:447` documents `fmtBits = fmt \| tiled<<8 \| endian<<12`; LEDGER 20970-20976 adds bits 14-15 = texDim and `hle_api_observe.cpp:1177` as the packing site |
| **Our behavior** | Three packing sites: `:1177` and `:5516` (2D, never set bits 14-15) and `:5362` (3D, sets `2u << 14` and `(d3-1) << 16`). Bit 26 = unlock-origin (`hle_video.hpp:613`). **Free: bits 9-11 and 27-31, plus one whole spare union dword** |
| **Deviation** | The schema **already reserves** `texDim = 3` for CUBE and a 10-bit depth field. No format change is needed — only a producer that sets them |
| **Killer test** | The conservation equation in §3 |

---

## 2. The four cube keys, decoded

From `archive/iter456_title_fresh.srht` via the fetch shadow
(ENGINE_MAP §6g.3c: base 59, stride 6, 16 slots). Every row recomputed by
`tools/check_cube_layout.py --expect` (exit 0).

All four are identical in shape: **128×128, fmt 6 (`k_8_8_8_8`), tiled=1,
endian=2 (`k8in32`), pitch 128 texels, `stack_depth`=5, `packed_mips`=1,
`mip_min/max` 0/7, packed tail at level 3.**

| key | slots | binds | base_address | mip_address | face stride | six-face base span | mip offsets (lvl: bytes) | mip strides | mips total |
|---|---|---|---|---|---|---|---|---|---|
| `0x03050000` | 0,1,2,3 | 1,680 | `0x03050000` | `0x030B0000` | 65,536 | **393,216** | 1: 0, 2: 98,304, 3: 122,880 | 16,384 / 4,096 / 4,096 | **147,456** |
| `0x0314A000` | 4,6 | 1,263 | `0x0314A000` | `0x031AA000` | 65,536 | **393,216** | 1: 0, 2: 98,304, 3: 122,880 | 16,384 / 4,096 / 4,096 | **147,456** |
| `0x1319C000` | 0 | 19 | `0x1319C000` | `0x131FC000` | 65,536 | **393,216** | 1: 0, 2: 98,304, 3: 122,880 | 16,384 / 4,096 / 4,096 | **147,456** |
| `0x1F1F0000` | 3 | 398 | `0x1F1F0000` | `0x1F250000` | 65,536 | **393,216** | 1: 0, 2: 98,304, 3: 122,880 | 16,384 / 4,096 / 4,096 | **147,456** |

⚠ The `3: 122,880` in the mip-offsets column is the byte OFFSET of level 3
inside the mip region, and is correct. It is not the stale r1 mips-total, which
was the same number by coincidence and is now **147,456** in the last column.

Derivation of the face stride, for `128×128 k_8_8_8_8` tiled, pitch field 4:

    row_pitch_texels      = 4 << 5                      = 128
    row_pitch_blocks      = align(128 / 1, 32)          = 128
    row_pitch_bytes       = 128 * 4                     = 512
    z_slice_block_rows    = align(128 / 1, 32)          = 128
    array_slice_stride    = align(512 * 128, 4096)      = 65,536      <- ONE FACE
    six-face base span    = 65,536 * 6                  = 393,216

⛔ **r1 ERROR, corrected.** r1 withheld the packed tail's extent as UNKNOWN and
reported `mips total = 122,880 B` as a lower bound. `GetPackedMipOffset` is now
ported, so the tail is **computed** and the table above carries the real figure:

    tail (level 3) packed offsets, 128x128 k_8_8_8_8:
      mip 3 -> x=16 y=0     mip 4 -> x=8 y=0     mip 5 -> x=4 y=0
      mip 6 -> x=0  y=8     mip 7 -> x=0 y=4
    tail extent = max over sublevels 3..7 of (offset + own size)
                = 32 x 16 blocks -> 4,096 B per face
    mips_total_extent_bytes = 147,456          (was reported as 122,880)

A 128x128 square takes the `log2_width > log2_height` **else** arm and packs
along X until `packed_mip >= 3`, when the layout flips to Y. Both arms, the
flip, the texel→block shift and the `log2_size > 4` guard are fixture-checked.

**Total guest footprint per cube: 393,216 B base + 147,456 B mips = 540,672 B.**

---

## 3. The conservation equation

If the base level really is six faces of `array_slice_stride_bytes`, then the
guest's own `mip_address` must begin exactly that far after its `base_address`
— because subresources are laid out consecutively and the mip chain follows the
base (`texture_util.cc:437-443`). **The guest chose both addresses. We chose
neither.**

    mip_address - base_address  ==  array_slice_stride_bytes * 6

Measured on all four keys:

    0x030B0000 - 0x03050000 = 0x60000 = 393,216  ==  65,536 * 6   HOLDS
    0x031AA000 - 0x0314A000 = 0x60000 = 393,216  ==  65,536 * 6   HOLDS
    0x131FC000 - 0x1319C000 = 0x60000 = 393,216  ==  65,536 * 6   HOLDS
    0x1F250000 - 0x1F1F0000 = 0x60000 = 393,216  ==  65,536 * 6   HOLDS

**This is the load-bearing result of the slice.** LEDGER 20944 concluded that
the frozen fixture could not settle the cube layout and that slice 2 would need
a fresh capture. That is **half wrong, and this corrects it**: the fixture
cannot supply the *pixels*, but it does settle the **observed base span and the
computed mip extents** without one. The six-face extent is confirmed by the
guest's own addressing rather than by Xenia's formula alone — two independent
derivations agreeing on 393,216 B.

⛔ It does **not** settle the layout in full, and this sentence used to say it
did: the face PIXELS and the face ORDER both remain UNKNOWN (§4 items 1, 2, 8).
I reported the phrase absent on the strength of a LITERAL grep, and the text
carries markdown emphasis inside it, so the pattern never matched — the check
was weaker than the claim I drew from it.

The equation is falsifiable and is tested as such: a synthetic fetch whose
`mip_address` sits four face strides after the base is reported as **DOES NOT
HOLD**, and the mutant that hardcodes `conserved = True` is detected.

**Scope.** It confirms the SPAN the guest reserved. It says nothing about
whether all six faces held valid pixels at capture time.

### The proposed payload, conserved across BOTH guest regions

⛔ **r1 ERROR.** r1 proposed a base-only payload and claimed "no schema change",
while these fetches advertise mip levels 0-7. The director rejected both: a
conservation path must say how **two disjoint guest regions** become one
recorded payload, and "no schema change" cannot be claimed without a
recorder/replay path that survives it.

A cube's pixels live in two places the guest chose independently:

    region A  base_address .. + 393,216      six faces, level 0
    region B  mip_address  .. + 147,456      six faces, levels 1..3(tail)

They are adjacent for all four observed keys, but that is an OBSERVATION, not a
contract — Xenia treats the two addresses as independent (`texture_util.cc:76-99`
lets base_page go to 0 while mip_page stays), so the payload must carry both
lengths rather than assume contiguity.

**The record cannot carry that today.** `TextureUpload` uses six of its seven
union dwords for `{key, arenaOffset, bytes, dims, fmtBits, pitch}`; there is
exactly ONE spare dword and no field for a second segment length. So:

    ONE spare union dword  ->  mipBytes
    bytes                  ->  baseBytes (region A only)
    fmtBits bits 14-15     ->  3 (CUBE)          [already reserved]
    fmtBits bits 16-25     ->  faces - 1 = 5     [already reserved]
    fmtBits bits 27-29     ->  mipLevels - 1 (0..7 needs 3 bits)     [free]
    fmtBits bit 30         ->  packedMips, the Xenos f[5] bit 11    [free]
    arena payload          ->  region A bytes THEN region B bytes, contiguous

(Bits 27-30 rather than the 9-11 that r2 named: `mipLevels-1` needs three bits
and `packedMips` needs a fourth, which would have run into bit 12, the endian
field. Bits 9-11 and 27-31 are both free; the high group has room for all four.)

⛔ **`packed_mips` MUST be a named field, not inferred.** It is Xenos `f[5]`
bit 11 and it decides how the recorded mip REGION is interpreted: with it set,
levels at and past `GetPackedMipLevel` share one tail; without it every level
is independent. Two textures with identical `mipBytes` can need different
decodes, so deriving it from a byte count is unsound. It belongs in the
descriptor identity and in the ownership key alongside format, dims and
tiling — otherwise a re-created texture that only flips `packed_mips` would
silently reuse the previous decode.

    CONSERVATION, checkable by the replayer on every record:
        TraceRecord.payloadBytes == baseBytes + mipBytes
        baseBytes          == faceStride(dims, pitch, fmt) * faces
        mipBytes           == mipsTotalExtent(dims, pitch, fmt, mipLevels,
                                              packedMips)
        for the four observed keys: 393,216 + 147,456 == 540,672

    ARENA FIT, corrected: the pool is a FIXED 8 MiB PER SLOT
    (`kTexSlotBytes`, hle_video.hpp:598) -- not one 8 MiB pool to share.
    540,672 B fits comfortably in ONE slot (6.4% of it), and four concurrent
    cube captures occupy FOUR OF THE EIGHT SLOTS, not 2.06 MiB of one.

### The rule at each stage — the part r2 left out

⛔ **r2 ERROR.** r2 described the payload but not the PIPELINE, and the director
found the hole: **today's recorder serialises only `TextureUpload.bytes`**, so
a two-region payload would have its mip region silently DROPPED between the
producer and the trace. Naming the fields is not a conservation path. Each
stage needs its rule stated:

**Recorder (`texWrite` + the SRHT writer).** The arena write and the record's
serialised length must both be the TOTAL, `baseBytes + mipBytes`, not `bytes`.
Any writer that emits `bytes` as the payload length silently truncates the mip
region. This is the single change that makes the schema real, and it must land
with the producer, never after.

**Replayer.** On every cube record, RECOMPUTE `baseBytes` and `mipBytes` from
`{dims, pitch, fmt, faces, mipLevels, packedMips}` and REFUSE the record by
name if `TraceRecord.payloadBytes != baseBytes + mipBytes`
(`gfx/hle_video.cpp:2661` — the real per-record envelope field; r3 named a
conceptual arena-bytes quantity that is not a field of the envelope, so the
rule was not wired to anything). A payload that disagrees with the
geometry that described it is corrupt, and the existing vertex path already
sets this precedent (`hle_video.cpp:1662-1671` refuses a digest mismatch rather
than storing it). Never repair, never truncate, never pad.

**Executor.** Create `mipLevels` mips x 6 faces and upload EVERY declared
subresource, index `mip + face * mipLevels`, region A supplying level 0 for all
six faces and region B supplying levels 1.. — matching both references
(`video.cpp:5738`, `d3d12_texture_cache.cc:1795-1811`). A producer that records
region A only, or a backend that uploads level 0 only, is NOT cube conformance
and must not be described as such.

**Ownership identity.** The cube's descriptor key must include `packedMips` and
`mipLevels` beside the existing format/dims/tiling, so a re-created texture
that changes only its mip shape cannot inherit the old decode.

⚠ **This IS a schema change** — one spare union dword consumed, four fmtBits
bits claimed, and a recorder length change. Saying otherwise was the r1 error.

### The A/B gate must exist at the CONSUMER too

⛔ Also missing from r2. A gate that only guards the PRODUCER makes the
experiment unfalsifiable: the OFF leg would need a different trace, so OFF and
ON could never be compared byte-for-byte from the same bytes. The requirement
is therefore:

- capture ONE fresh K=1 trace **with** cube pixels recorded;
- replay that SAME trace with real cube use OFF and ON;
- **replay-OFF must ignore the recorded cube uploads entirely and leave
  `shared[32..47]` at zero** — i.e. it reproduces today's white-dummy picture
  from a trace that contains cube data;
- one gate may drive both paths, but the consumer arm is the one that makes the
  comparison deterministic.

Gate-OFF on that fresh trace must be byte-identical to a gate-OFF replay of the
same trace, exactly as `SR_HLE_CUBE_NULL_ZERO=0` is byte-identical to default
today (§7).

---

## 4. UNKNOWN — fresh capture required

Not derivable from `archive/iter456_title_fresh.srht`. Listed so none of it is
filled by assumption.

1. **The pixels of faces 1-5.** The trace carries 65,536 B per key. The
   remaining 327,680 B were never snapshotted. **UNKNOWN — fresh capture
   required.**
2. **The mip chain's bytes**, at `mip_address`, for all six faces.
   **UNKNOWN — fresh capture required.**
3. ~~The packed mip tail's extent.~~ **RESOLVED in r2** — ported, computed,
   fixture- and mutation-checked (§2). It was never undecidable.
4. **Whether the cube instructions actually execute.** 3,360 CUBE bindings and
   526 cube-fetch draw opportunities are *static* counts; branch coverage inside
   those shaders is unmeasured. **UNKNOWN.**
5. **Whether a correct cube changes the picture.** No visual evidence of any
   kind exists for this hypothesis. **UNKNOWN — one gated K=1 capture and a
   same-trace gate-off/on grading is the only thing that answers it.**
6. **Whether a DXT1 (fmt 18) cube ever binds.** MIGRATION 1177 says cubes are
   fmt 6 and fmt 18; this fixture has only fmt 6. **UNKNOWN for this era.**
7. ~~Whether white-vs-zero-swizzled null dummies matter.~~ **MEASURED in r2**
   for the CUBE null only (§7): it changes 15 of 16 frames but does not repair
   the picture. Still UNKNOWN for the 2D and 3D nulls, which were not touched.
8. **Face ORDER.** Xenia's loader walks slices 0..5 and D3D12's cube face order
   is +X,-X,+Y,-Y,+Z,-Z; that the guest's slice *n* is D3D12's face *n* is
   **assumed by both references and not independently verified here.**
   **UNKNOWN — a rendered comparison is the only check.**

---

## 5. Design — the exact changes, named only now

Not authorized by this note. Stated so the director can rule on scope.

**Producer** (`kernel/hle_api_observe.cpp`), one gated block:
1. At `:5306`, admit `texDim == 3` beside `texDim == 2`.
2. Read `stack_depth` and **REFUSE** the capture unless it is 5 — never
   hardcode 6 (§1.1). Delete the `d3 = isCube ? 6u` hardcode at `:5290`.
3. Snapshot BOTH regions per §3: `array_slice_stride_bytes × 6` from
   `base_address` **and** `mips_total_extent_bytes` from `mip_address`,
   concatenated. 540,672 B against an 8 MiB slot.
4. Pack `fmtBits |= (3u << 14) | (5u << 16) | ((mipLevels - 1u) << 27) |
   (packedMips ? (1u << 30) : 0u)` — the §3 allocation, **including the named
   packed-mips bit**. Put `mipBytes` in the spare union dword and leave
   `TextureUpload.bytes = baseBytes`. **This is a schema change** (§3).
   ⛔ **r3 ERROR** — r3 corrected §3 to bits 27-30 and left THIS step carrying
   the superseded low-bit shift, which would have collided with the endian
   field at bit 12 and dropped `packedMips` entirely. Fixing the reasoning and leaving
   the actionable step stale is the defect: the step is what gets implemented.
5. Gate: `SR_HLE_CUBE_CAPTURE`, **default OFF**.

**Executor** (`gfx/hle_video.cpp:1747`): add a `texDim == 3` arm calling a new
`hleUploadHostCube`, storing into a new `s_texSlotCube[key]`.

**Backend** (`gfx/d3d12_backend.cpp`): `hleUploadHostCube` creating
`TEXTURE2D` + `DepthOrArraySize = 6` + `D3D12_SRV_DIMENSION_TEXTURECUBE` into
the already-reserved `[kTex2DSlots+8 .. +15]` (`:28`); untile each face and each
mip with the existing `tiled2D`; publish `shared[32 + s] = d.texSlotCube[s]`
at `:4515`. ⛔ **r1 ERROR** — r1 also proposed creating a cube null dummy. One
already exists at index 0 (`:2421`, `:2495-2502`) and must be left alone; the
allocator simply has to hand out indices 1..7 for real cubes.

**Cost.** The producer change makes the frozen fixture unusable for the *new*
data, so it needs the one fresh K=1 capture already authorized in principle.
Grading is then deterministic same-trace gate-off/on across all 16 frames.

**⚠ What this would NOT prove.** Nothing here predicts a visual change. The
green sky and the fractured geometry remain separate hypotheses, and an
improvement in one must not be counted as evidence for the other.

---

## 7. H0 MEASURED — the white cube null IS on the live sampling path

`SR_HLE_CUBE_NULL_ZERO` (default OFF, diagnostic only) changes **one thing**:
the `Shader4ComponentMapping` of the eight existing cube-null SRVs
(`d3d12_backend.cpp:2495-2502`), from the default RGBA mapping to
`FORCE_VALUE_0` on all four channels. It does not touch the 2D or 3D nulls, add
any resource, or publish any real cube index.

Three deterministic replays of the frozen fixture, 16 frames each:

| leg | frames | vs default |
|---|---|---|
| default (gate unset) | 16 | — (present 10 sha256 `c677fc5e…`, the frozen baseline) |
| `SR_HLE_CUBE_NULL_ZERO=0` | 16 | **0 frames differ — byte-identical** |
| `SR_HLE_CUBE_NULL_ZERO=1` | 16 | **15 of 16 frames changed** (only present 0 unchanged) |

**H0 is NOT killed.** The white cube fallback is demonstrably on the live
sampling path. This also settles §1.7 in a way the absence argument could not:
index 0 is not "no cube", it is a real white cube being sampled.

### Magnitude, before anyone reads too much into "15 of 16"

⛔ **r2 ERROR.** r2 quoted the 97.1% AREA over the backdrop ROI and the
magnitude statistics over the WHOLE FRAME — two denominators presented as one
picture. Both, over both regions:

    metric                     BACKDROP ROI      WHOLE FRAME
    pixels                     436,176           921,600
    changed (delta > 0)        97.1%             88.1%
    mean abs diff / channel    4.799 / 255       2.997 / 255
    median per-pixel max       1                 1
    95th percentile            36                12
    delta > 24                 6.57%             3.40%
    delta > 64                 1.69%             0.80%

The ROI figures are about double the whole-frame ones, which is itself the
finding: the change really is concentrated in the backdrop. But even inside the
ROI the median pixel moves by 1/255 and fewer than 2% move by more than 64.

The 8× amplified difference (`archive/iter457_cube_null_diff_x8.png`) shows the
change concentrated on the **building facades** — the striped tower, the shards,
the central column — with the sky and the shield essentially untouched. That is
exactly where an environment cube would act, so the result is
mechanism-consistent as well as real.

### Oracle grading, present 10, green sky and geometry kept separate

| ROI | defect | oracle | default | gate ON | Δ |
|---|---|---|---|---|---|
| sky_upper_mid | GREEN SKY | (231,217,233) | 193.1 | 190.8 | **−2.3** |
| facade_left | GEOMETRY/MATERIAL | (135,104,76) | 93.0 | 86.0 | **−7.0** |
| facade_right | GEOMETRY/MATERIAL | (194,169,138) | 101.2 | 99.7 | −1.5 |
| road | GEOMETRY/MATERIAL | (130,111,107) | 94.5 | 94.8 | +0.3 |
| cape | CHARACTER | (128,66,77) | 143.7 | 144.8 | +1.0 |

⚠ **A correction to my own first pass.** I initially graded with an ROI I
labelled "sky" at (40,60)-(260,200) and reported an 82.9 → 58.4 improvement.
Opening the oracle shows that box is the **top-left building**, not sky. The
boxes above are placed against the oracle image. The real sky improvement is
**−2.3 on a 193-unit error — about 1%**.

### Verdict on H0

Real, mechanism-consistent, concentrated on facades, and **nowhere near a fix**.
Both contact sheets (`archive/iter457_cube_null_{default,on}_sheet.png`) were
generated and eyeballed: they are near-indistinguishable, and the picture is
still violently green with fractured geometry in both. Zero accepted pixel
progress.

⛔ The gate stays **default OFF**. A visible H0 delta does **not** prove that
zero is the correct value for a missing cube, and it does not prove anything
about real guest cube pixels. It proves only that these draws sample the cube
table, which makes H1 worth the capture it will cost.

---

## 6. Acceptance

| Requirement | Status |
|---|---|
| Every line citation resolves | Verified by direct read; the two I first cited wrong (`texture_util.cc:51`→**52**, `d3d12_texture_cache.cc:1792`→**1795-1811**) are corrected above |
| Read-only checker recomputes the four layout rows | `tools/check_cube_layout.py` — 40/40 fixtures, exit 0 on the fixture with `--expect` |
| Mutation-validated on face-count, face-stride, mip-address, endianness | `tools/mutate_cube_layout.py` — **21/21**, at least one mutant per named field, plus seven over the ported packed-mip math |
| Concordance precedes the design | §1 precedes §5 |
| Undecidable values marked UNKNOWN | §4, eight items |
| No live roll, no fresh capture, no sweep, no default change | Confirmed. The ONE renderer change is `SR_HLE_CUBE_NULL_ZERO`, default OFF, whose gate-off leg is byte-identical to default across all 16 frames |

⚠ **Mutants that initially SURVIVED, and what each taught.**

*r1, three of fifteen.* All three survived for one reason: the observed 128×128
`k_8_8_8_8` cube is already tile- AND 4 KB-aligned, so the alignment code never
ran in any fixture. Closed with shapes where each rule bites — a DXT1 cube at a
32-texel pitch, a 32×32 `k_8` cube, and mip level 3.

*r2, one of twenty-one, and this is the one worth remembering.* "The tail extent
uses only the packed level" survived. I measured seven near-square shapes, found
the extent always maximised at the packed level, and REMOVED the mutant as an
equivalent refactor. That was a universal claim drawn from seven samples, and it
is false: the director produced a skinny **1×17**, where the true extent is
17×18 and the packed-level-only rule gives 17×17. The mutant is restored and
killed by that fixture. An "always/never" needs a whole-surface argument; seven
near-square samples is not one — the same standing rule I broke when I searched
for the cube dummy by looking only for what I expected to be missing.
