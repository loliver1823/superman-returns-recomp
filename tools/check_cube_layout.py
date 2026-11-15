#!/usr/bin/env python3
"""check_cube_layout.py -- recompute Xenia's GUEST layout for the cube textures
observed in an SRHT fixture, READ ONLY.

WHY THIS EXISTS. `docs/CUBE_CONFORMANCE_READING.md` states, as a concordance,
what the Xbox 360 guest memory layout of our four observed cube textures is. A
prose table is not evidence; this tool recomputes every number in it from the
captured fetch constants, so the note can be re-derived rather than trusted.

THE AUTHORITY IS XENIA, NOT INTUITION. Every function below is a port of a
named function in reference/xenia-canary, cited at its definition. In
particular the six-face span is NOT `6 * w * h * bpp`: it is
`array_slice_stride_bytes * 6`, where the stride is the tile-aligned row pitch
times the tile-aligned block-row count, aligned up to 4096. For the observed
128x128 k_8_8_8_8 cubes those happen to agree -- which is exactly why deriving
it from intuition would look right and generalise wrong.

WHAT IT DOES NOT DO. It reads a trace and prints numbers. It touches no
renderer, changes no default, and makes no claim about pixels. It cannot tell
you whether the guest memory at those addresses actually held six faces at
capture time -- the fixture only proves what the guest ASKED for.

    python tools/check_cube_layout.py archive/iter456_title_fresh.srht
    python tools/check_cube_layout.py --self-test

The PACKED MIP TAIL is computed, not withheld: `GetPackedMipOffset` is ported
from the same authority (texture_util.cc:116-205). The director's correction --
"not derivable" was false, it was only unported.

Exit: 0 ok / 1 a decoded row disagrees with the note's committed values /
2 refused (bad usage, unreadable trace, or a fetch that violates a Xenos
invariant).
"""
import argparse, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ── Xenos constants (reference/xenia-canary/src/xenia/gpu/xenos.h) ───────────
K_TILE_WH_LOG2 = 5                       # xenos.h:1198 kTextureTileWidthHeightLog2
K_TILE_WH = 1 << K_TILE_WH_LOG2          # xenos.h:1199 = 32
K_TILE_DEPTH_LOG2 = 2                    # xenos.h:1202
K_TILE_DEPTH = 1 << K_TILE_DEPTH_LOG2    # xenos.h:1203 = 4
K_SUBRES_ALIGN_LOG2 = 12                 # xenos.h:1177
K_SUBRES_ALIGN = 1 << K_SUBRES_ALIGN_LOG2          # xenos.h:1178 = 4096
K_LINEAR_ROW_ALIGN = 1 << 8              # xenos.h:1225 = 256

# DataDimension (xenos.h:1325 `dimension : 2`)
DIM_1D, DIM_2D_OR_STACKED, DIM_3D, DIM_CUBE = 0, 1, 2, 3
DIM_NAME = {0: "1D", 1: "2DOrStacked", 2: "3D", 3: "CUBE"}

# Endian (xenos.h `endianness : 2`, dword_1 +6)
ENDIAN_NAME = {0: "kNone", 1: "k8in16", 2: "k8in32", 3: "k16in32"}

# The formats this tool knows the block geometry of. Deliberately NOT a
# complete FormatInfo table: an unknown format REFUSES rather than guessing a
# block size, because a wrong bytes_per_block silently produces a plausible
# span. Extend from xenia's texture_info.cc when a fixture needs it.
#   fmt: (block_width, block_height, bytes_per_block, name)
FORMAT_INFO = {
    2:  (1, 1, 1, "k_8"),
    6:  (1, 1, 4, "k_8_8_8_8"),
    18: (4, 4, 8, "k_DXT1"),
    20: (4, 4, 16, "k_DXT2_3"),
    21: (4, 4, 16, "k_DXT4_5"),
    37: (1, 1, 4, "k_32_FLOAT"),
}


def log2_floor(v):
    n = 0
    while v > 1:
        v >>= 1
        n += 1
    return n


def log2_ceil(v):
    n = log2_floor(v)
    return n if (1 << n) == v else n + 1


def next_pow2(v):
    return 1 << log2_ceil(v) if v else 1


def align(v, a):
    return (v + a - 1) & ~(a - 1)


# ── texture_address.h:174-184 TiledCombine, :190-209 Tiled2D ─────────────────
def tiled_combine(outer_inner_bytes, bank, pipe, y_lsb):
    return (((y_lsb << 4) | (pipe << 6) | (bank << 11))
            | (outer_inner_bytes & 0b1111)
            | (((outer_inner_bytes >> 4) & 0b1) << 5)
            | (((outer_inner_bytes >> 5) & 0b111) << 8)
            | ((outer_inner_bytes >> 8) << 12))


def tiled_2d(x, y, pitch_aligned, bytes_per_block_log2):
    """texture_address.h:190. Byte offset of block (x, y) in a 2D-tiled image.

    Line-for-line with `gfx/xenos_tiling.hpp::tiled2D`, which is itself a port
    of this function -- so a disagreement between them is a real defect in one
    of the two, which the self-test checks for.
    """
    outer_blocks = (((y >> 5) * (pitch_aligned >> 5) + (x >> 5)) << 6)
    inner_blocks = (((y >> 1) & 0b111) << 3) | (x & 0b111)
    outer_inner_bytes = (outer_blocks | inner_blocks) << bytes_per_block_log2
    bank = (y >> 4) & 0b1
    pipe = ((x >> 3) & 0b11) ^ (((y >> 3) & 0b1) << 1)
    return tiled_combine(outer_inner_bytes, bank, pipe, y & 1)


# ── texture_util.cc:450-477 GetTiledAddressUpperBound2D ──────────────────────
def tiled_upper_bound_2d(right, bottom, pitch_aligned, bpb_log2):
    if not right or not bottom:
        return 0
    ub = tiled_2d((right - 1) & ~(K_TILE_WH - 1),
                  (bottom - 1) & ~(K_TILE_WH - 1), pitch_aligned, bpb_log2)
    if bpb_log2 == 0:
        ub += 0xA00
    elif bpb_log2 == 1:
        ub += 0xC00
    else:
        ub += 0x400 << bpb_log2
    return ub


# ── texture_util.h:41-44 GetPackedMipLevel ──────────────────────────────────
def packed_mip_level(width, height):
    s = log2_ceil(min(width, height))
    return s - 4 if s > 4 else 0


# ── texture_util.cc:116-205 GetPackedMipOffset ──────────────────────────────
# The director required this ported rather than left UNKNOWN: the authority is
# vendored and the fetch carries every input, so "not derivable" was false --
# it was only unported. Ported line for line, including the two branches whose
# condition is `log2_width > log2_height` (a SQUARE texture takes the else arm,
# which is easy to get backwards and is fixture-checked below).
def packed_mip_offset(width, height, depth, fmt, mip):
    """(packed?, x_blocks, y_blocks, z_blocks) for `mip` inside the tail."""
    block_w, block_h, _bpb, _n = FORMAT_INFO[fmt]
    log2_width = log2_ceil(width)
    log2_height = log2_ceil(height)
    log2_size = min(log2_width, log2_height)
    if log2_size > 4 + mip:                                      # :156-162
        return False, 0, 0, 0
    packed_mip_base = log2_size - 4 if log2_size > 4 else 0       # :163
    packed_mip = mip - packed_mip_base                           # :164

    if packed_mip < 3:                                           # :167-177
        if log2_width > log2_height:
            x_blocks, y_blocks = 0, 16 >> packed_mip
        else:
            x_blocks, y_blocks = 16 >> packed_mip, 0
        z_blocks = 0
    else:                                                        # :178-201
        if log2_width > log2_height:
            offset = (1 << (log2_width - packed_mip_base)) >> (packed_mip - 2)
            x_blocks, y_blocks = offset, 0
        else:
            offset = (1 << (log2_height - packed_mip_base)) >> (packed_mip - 2)
            x_blocks, y_blocks = 0, offset
        if offset < 4:
            log2_depth = log2_ceil(depth)
            z_blocks = ((log2_depth - packed_mip) * 4
                        if log2_depth > 1 + packed_mip else 4)
        else:
            z_blocks = 0
    # :203-204 -- the shifts are log2 of the block dimensions
    # (texture_info_formats.cc:45-53, `GetShift(block_width)`).
    x_blocks >>= log2_floor(block_w)
    y_blocks >>= log2_floor(block_h)
    return True, x_blocks, y_blocks, z_blocks


class Level(object):
    __slots__ = ("row_pitch_bytes", "z_slice_stride_block_rows",
                 "array_slice_stride_bytes", "x_extent_blocks",
                 "y_extent_blocks", "z_extent",
                 "array_slice_data_extent_bytes", "level_data_extent_bytes")

    def __init__(self):
        for s in self.__slots__:
            setattr(self, s, 0)

    def as_dict(self):
        return {s: getattr(self, s) for s in self.__slots__}


class GuestLayout(object):
    def __init__(self):
        self.base = Level()
        self.mips = {}
        self.mip_offsets_bytes = {}
        self.mips_total_extent_bytes = 0
        self.max_level = 0
        self.packed_level = None
        self.array_size = 1
        # Levels whose EXTENT is not derivable here (the packed mip tail needs
        # GetPackedMipOffset, which is not ported). Their strides ARE derivable
        # and are still reported; only the extent is withheld. Making this a
        # per-level fact rather than a whole-function refusal is deliberate:
        # the BASE level -- the six-face span, the thing the cube slice needs
        # -- is fully derivable even when the mip tail is not.
        self.unknown_extent_levels = set()


def guest_texture_layout(dimension, base_pitch_texels_div_32, width_texels,
                         height_texels, depth_or_array_size, is_tiled, fmt,
                         has_packed_levels, has_base, max_level):
    """texture_util.cc:208-447 GetGuestTextureLayout.

    Ported for the 2D/stacked/cube path only. A 3D dimension REFUSES: its
    array_slice_stride multiplies by align(depth, kTextureTileDepth) and its
    extent uses GetTiledAddressUpperBound3D, neither of which is ported here,
    and silently returning a 2D answer for a volume is exactly the class of
    error this tool exists to prevent.
    """
    if dimension == DIM_3D:
        raise ValueError("3D layout is not ported here - refuse, never guess")
    if dimension == DIM_1D:
        raise ValueError("1D layout is not ported here - refuse, never guess")
    if fmt not in FORMAT_INFO:
        raise ValueError("format %d has no block geometry in this tool" % fmt)
    block_w, block_h, bpb, _name = FORMAT_INFO[fmt]

    layout = GuestLayout()
    depth = 1
    # texture_util.cc:222-231
    if dimension == DIM_2D_OR_STACKED:
        layout.array_size = depth_or_array_size
    elif dimension == DIM_CUBE:
        layout.array_size = 6          # :229 -- SIX, from the DIMENSION
    else:
        layout.array_size = 1
    if not width_texels or not height_texels or not depth or not layout.array_size:
        return GuestLayout()

    # :244-249 clamp
    max_level_for_dimensions = log2_floor(max(width_texels, height_texels, depth))
    max_level = min(max_level, max_level_for_dimensions)
    layout.max_level = max_level
    layout.packed_level = (packed_mip_level(width_texels, height_texels)
                           if has_packed_levels else None)

    pl = layout.packed_level
    max_stored_level = min(max_level, pl if pl is not None else 0xFFFFFFFF)

    # :281-292 the loop counter's two meanings
    if pl == 0:
        loop_level_last = 1 if max_level != 0 else 0
    else:
        loop_level_last = max_stored_level

    mip_offset_bytes = 0
    for loop_level in range(0 if has_base else 1, loop_level_last + 1):
        is_base = loop_level == 0
        level = 0 if pl == 0 else loop_level
        L = layout.base if is_base else Level()

        # :305-321 strides
        if is_base:
            row_pitch_texels_unaligned = base_pitch_texels_div_32 << 5
            z_rows_unaligned = height_texels
        else:
            row_pitch_texels_unaligned = max(next_pow2(width_texels) >> level, 1)
            z_rows_unaligned = max(next_pow2(height_texels) >> level, 1)
        row_pitch_blocks_tile_aligned = align(
            align(row_pitch_texels_unaligned, block_w) // block_w, K_TILE_WH)
        L.row_pitch_bytes = row_pitch_blocks_tile_aligned * bpb
        if not is_tiled and not is_base:                       # :332-335
            L.row_pitch_bytes = align(L.row_pitch_bytes, K_LINEAR_ROW_ALIGN)
        L.z_slice_stride_block_rows = align(
            align(z_rows_unaligned, block_h) // block_h, K_TILE_WH)
        L.array_slice_stride_bytes = (L.row_pitch_bytes
                                      * L.z_slice_stride_block_rows)
        z_stride_bytes = L.array_slice_stride_bytes
        # (:346-349 the k3D multiply is deliberately absent - we refuse 3D)
        L.array_slice_stride_bytes = align(L.array_slice_stride_bytes,
                                           K_SUBRES_ALIGN)   # :350-352

        # :399-411 extents. The packed-tail branch (:359-398) needs
        # GetPackedMipOffset, which is not ported; a level that lands on the
        # packed level therefore refuses rather than reporting a wrong extent.
        if pl is not None and level == pl:
            # THE PACKED MIP TAIL -- texture_util.cc:359-398, now PORTED.
            # The used region is the max over every sublevel that lives in the
            # tail of (its packed offset + its own size). It is much smaller
            # than the 32x32-aligned level would suggest, which is the entire
            # reason Xenia computes it this way instead of using the stride.
            L.x_extent_blocks = L.y_extent_blocks = L.z_extent = 0
            packed_sublevel_last = 0 if is_base else max_level
            for sub_l in range(pl, packed_sublevel_last + 1):
                _p, px, py, pz = packed_mip_offset(width_texels, height_texels,
                                                   depth, fmt, sub_l)
                L.x_extent_blocks = max(
                    L.x_extent_blocks,
                    px + align(max(width_texels >> sub_l, 1), block_w) // block_w)
                L.y_extent_blocks = max(
                    L.y_extent_blocks,
                    py + align(max(height_texels >> sub_l, 1), block_h) // block_h)
                L.z_extent = max(L.z_extent, pz + max(depth >> sub_l, 1))
            if is_tiled:
                L.array_slice_data_extent_bytes = tiled_upper_bound_2d(
                    L.x_extent_blocks, L.y_extent_blocks,
                    row_pitch_blocks_tile_aligned, log2_floor(bpb))
            else:
                L.array_slice_data_extent_bytes = (
                    z_stride_bytes * (L.z_extent - 1)
                    + L.row_pitch_bytes * (L.y_extent_blocks - 1)
                    + bpb * L.x_extent_blocks)
            L.level_data_extent_bytes = (
                L.array_slice_stride_bytes * (layout.array_size - 1)
                + L.array_slice_data_extent_bytes)
        else:
            L.x_extent_blocks = \
                align(max(width_texels >> level, 1), block_w) // block_w
            L.y_extent_blocks = \
                align(max(height_texels >> level, 1), block_h) // block_h
            L.z_extent = max(depth >> level, 1)

            if is_tiled:                                        # :412-427
                L.array_slice_data_extent_bytes = tiled_upper_bound_2d(
                    L.x_extent_blocks, L.y_extent_blocks,
                    row_pitch_blocks_tile_aligned, log2_floor(bpb))
            else:
                L.array_slice_data_extent_bytes = (
                    z_stride_bytes * (L.z_extent - 1)
                    + L.row_pitch_bytes * (L.y_extent_blocks - 1)
                    + bpb * L.x_extent_blocks)
            L.level_data_extent_bytes = (
                L.array_slice_stride_bytes * (layout.array_size - 1)
                + L.array_slice_data_extent_bytes)              # :433-435

        if not is_base:                                         # :437-443
            layout.mips[level] = L
            layout.mip_offsets_bytes[level] = mip_offset_bytes
            layout.mips_total_extent_bytes = max(
                layout.mips_total_extent_bytes,
                mip_offset_bytes + L.level_data_extent_bytes)
            mip_offset_bytes += L.array_slice_stride_bytes * layout.array_size
    return layout


# ── texture_util.cc:23-114 GetSubresourcesFromFetchConstant ─────────────────
def subresources_from_fetch(fc):
    """(w-1, h-1, d/a-1, base_page, mip_page, mip_min, mip_max) from 6 dwords.

    Raises on a Xenos invariant violation (:51 `stack_depth == 5` for a cube),
    because a cube whose fetch does not say six faces is not something to paper
    over with a default.
    """
    d0, d1, d2, d3, d4, d5 = fc
    dim = (d5 >> 9) & 3
    stacked = (d1 >> 10) & 1
    w1 = h1 = da1 = 0
    if dim == DIM_1D:
        w1 = d2 & 0xFFFFFF
    elif dim == DIM_2D_OR_STACKED:
        w1 = d2 & 0x1FFF
        h1 = (d2 >> 13) & 0x1FFF
        da1 = ((d2 >> 26) & 0x3F) if stacked else 0
    elif dim == DIM_3D:
        w1 = d2 & 0x7FF
        h1 = (d2 >> 11) & 0x7FF
        da1 = (d2 >> 22) & 0x3FF
    else:                                                        # kCube :50-56
        sd = (d2 >> 26) & 0x3F
        if sd != 5:
            raise ValueError(
                "CUBE fetch has size_2d.stack_depth=%d, Xenos requires 5 "
                "(texture_util.cc:52 assert_true) - six faces is NOT assumed" % sd)
        w1 = d2 & 0x1FFF
        h1 = (d2 >> 13) & 0x1FFF
        da1 = 5

    longest = max(w1, h1)
    if dim == DIM_3D:
        longest = max(longest, da1)
    size_mip_max_level = log2_floor(longest + 1)

    base_page = (d1 >> 12) & 0xFFFFF
    mip_page = (d5 >> 12) & 0xFFFFF
    if mip_page == 0:                                            # :82-90
        mip_min = mip_max = 0
    else:
        mip_min = min((d4 >> 2) & 0xF, size_mip_max_level)
        mip_max = max(min((d4 >> 6) & 0xF, size_mip_max_level), mip_min)
    if mip_max != 0:                                             # :91-99
        if base_page == 0:
            mip_min = max(mip_min, 1)
        if mip_min != 0:
            base_page = 0
    else:
        mip_page = 0
    return w1, h1, da1, base_page, mip_page, mip_min, mip_max


def decode_fetch(fc):
    """Every field this tool uses, named, from the 6 raw dwords."""
    d0, d1, d2, d3, d4, d5 = fc
    w1, h1, da1, base_page, mip_page, mip_min, mip_max = \
        subresources_from_fetch(fc)
    return {
        "type": d0 & 3,
        "pitch_div32": (d0 >> 22) & 0x1FF,        # xenos.h:1267 `pitch : 9`
        "tiled": (d0 >> 31) & 1,                  # xenos.h:1268
        "format": d1 & 0x3F,                      # xenos.h:1270
        "endian": (d1 >> 6) & 3,                  # xenos.h:1271
        "stacked": (d1 >> 10) & 1,                # xenos.h:1274
        "base_page": base_page,
        "width": w1 + 1,
        "height": h1 + 1,
        "depth_or_array": da1 + 1,
        "mip_min_level": mip_min,
        "mip_max_level": mip_max,
        "dimension": (d5 >> 9) & 3,               # xenos.h:1325
        "packed_mips": (d5 >> 11) & 1,            # xenos.h:1326
        "mip_page": mip_page,
    }


# ── the fixture side: the fetch shadow (docs/ENGINE_MAP.md 6g.3c) ───────────
FETCH_SHADOW_BASE = 59          # dwords; first 59 are the register runs
FETCH_SHADOW_STRIDE = 6
FETCH_SHADOW_SLOTS = 16


def fetch_entry(payload_dwords, slot):
    o = FETCH_SHADOW_BASE + slot * FETCH_SHADOW_STRIDE
    return tuple(payload_dwords[o:o + FETCH_SHADOW_STRIDE])


def scan_cubes(trace):
    """{base_address: (fetch dwords, slots seen, bind count)} for every CUBE."""
    import scanout_route as sr
    import struct
    out, slots_of, binds = {}, {}, {}
    for _rec, t, _d, payload in sr.walk(trace):
        if t != 19 or not payload:
            continue
        n = len(payload) // 4
        if n < FETCH_SHADOW_BASE + FETCH_SHADOW_SLOTS * FETCH_SHADOW_STRIDE:
            continue
        dw = struct.unpack(">%dI" % n, payload[:n * 4])
        for s in range(FETCH_SHADOW_SLOTS):
            fc = fetch_entry(dw, s)
            if (fc[0] & 3) != 2:                  # not a texture fetch
                continue
            if ((fc[5] >> 9) & 3) != DIM_CUBE:
                continue
            base = ((fc[1] >> 12) & 0xFFFFF) << 12
            if not base:
                continue
            out.setdefault(base, fc)
            slots_of.setdefault(base, set()).add(s)
            binds[base] = binds.get(base, 0) + 1
    return out, slots_of, binds


def describe(fc):
    """The full decoded + Xenia-derived row for one cube fetch."""
    f = decode_fetch(fc)
    if f["dimension"] != DIM_CUBE:
        raise ValueError("not a cube fetch")
    pitch = f["pitch_div32"] << 5
    lay = None
    err = None
    try:
        lay = guest_texture_layout(
            DIM_CUBE, f["pitch_div32"], f["width"], f["height"], 6,
            bool(f["tiled"]), f["format"], bool(f["packed_mips"]),
            f["base_page"] != 0, f["mip_max_level"])
    except ValueError as e:
        err = str(e)
    row = dict(f)
    row["pitch_texels"] = pitch
    row["base_address"] = f["base_page"] << 12
    row["mip_address"] = f["mip_page"] << 12
    row["fmt_name"] = FORMAT_INFO.get(f["format"], (0, 0, 0, "UNKNOWN"))[3]
    row["endian_name"] = ENDIAN_NAME[f["endian"]]
    row["packed_level"] = packed_mip_level(f["width"], f["height"])
    if lay is not None:
        row["face_stride_bytes"] = lay.base.array_slice_stride_bytes
        row["faces"] = lay.array_size
        row["base_span_bytes"] = lay.base.array_slice_stride_bytes * lay.array_size
        row["base_data_extent_bytes"] = lay.base.level_data_extent_bytes
        row["mip_offsets"] = dict(lay.mip_offsets_bytes)
        row["mip_strides"] = {k: v.array_slice_stride_bytes
                              for k, v in lay.mips.items()}
        row["unknown_extent_levels"] = sorted(lay.unknown_extent_levels)
        row["mips_total_extent_bytes"] = lay.mips_total_extent_bytes
        # ⭐ THE CONSERVATION EQUATION, checked against the GUEST'S OWN
        # addressing. If the base really is six faces of `face_stride`, the
        # guest's mip_address should begin exactly `6 * face_stride` after
        # base_address. The guest chose both addresses; we chose neither.
        span = lay.base.array_slice_stride_bytes * lay.array_size
        row["mip_minus_base"] = (row["mip_address"] - row["base_address"]
                                 if row["mip_address"] else None)
        row["conserved"] = (row["mip_minus_base"] == span
                            if row["mip_minus_base"] is not None else None)
        row["layout_error"] = None
    else:
        row["face_stride_bytes"] = None
        row["faces"] = 6
        row["base_span_bytes"] = None
        row["mip_minus_base"] = None
        row["conserved"] = None
        row["layout_error"] = err
    return row


# ── the committed values the note asserts, recomputed here ──────────────────
# Filled from an actual run; the checker's job is to FAIL if the trace stops
# agreeing with what the note says.
_CUBE_ROW = dict(width=128, height=128, format=6, tiled=1, endian=2,
                 faces=6, face_stride_bytes=65536, base_span_bytes=393216,
                 mip_max_level=7, packed_mips=1, packed_level=3,
                 mip_minus_base=0x60000, conserved=True,
                 mips_total_extent_bytes=147456, unknown_extent_levels=[])
EXPECTED = {
    0x03050000: dict(_CUBE_ROW, mip_address=0x030b0000),
    0x0314a000: dict(_CUBE_ROW, mip_address=0x031aa000),
    0x1319c000: dict(_CUBE_ROW, mip_address=0x131fc000),
    0x1f1f0000: dict(_CUBE_ROW, mip_address=0x1f250000),
}


def self_test():
    np = nf = 0

    def ck(ok, what):
        nonlocal np, nf
        if ok:
            np += 1
            print("  PASS  " + what)
        else:
            nf += 1
            print("  FAIL  " + what)

    print("CUBE LAYOUT (synthetic; no trace)")

    # A synthetic 128x128 k_8_8_8_8 tiled cube fetch, built field by field from
    # xenos.h so the decoder is tested against the SPEC, not against a trace it
    # also parsed.
    def mk(width=128, height=128, fmt=6, tiled=1, endian=2, stack_depth=5,
           base_page=0x03050, mip_page=0, pitch_div32=4, dim=DIM_CUBE,
           mip_max=0, packed=1):
        d0 = 2 | (pitch_div32 << 22) | (tiled << 31)
        d1 = (fmt & 0x3F) | (endian << 6) | (base_page << 12)
        d2 = ((width - 1) & 0x1FFF) | (((height - 1) & 0x1FFF) << 13) \
            | ((stack_depth & 0x3F) << 26)
        d3 = 0
        d4 = (mip_max & 0xF) << 6
        d5 = (dim << 9) | (packed << 11) | (mip_page << 12)
        return (d0, d1, d2, d3, d4, d5)

    f = decode_fetch(mk())
    ck(f["width"] == 128 and f["height"] == 128,
       "size_2d w/h decode 13/13 (NOT the 3D 11/11/10 split)")
    ck(f["dimension"] == DIM_CUBE, "dimension bits 9-10 of dword 5 read CUBE")
    ck(f["endian"] == 2 and ENDIAN_NAME[2] == "k8in32",
       "KILLER endianness is dword_1 bits 6-7 and 2 means k8in32 (a 4-byte "
       "word swap), not 'no swap'")
    ck(f["base_page"] == 0x03050 and (f["base_page"] << 12) == 0x03050000,
       "KILLER base_address is a PAGE index (>>12); the byte address is "
       "page<<12")

    # KILLER: the six-face count must come from the DIMENSION, and a fetch that
    # does not carry stack_depth==5 must refuse rather than assume six.
    lay = guest_texture_layout(DIM_CUBE, 4, 128, 128, 6, True, 6, True, True, 0)
    ck(lay.array_size == 6,
       "KILLER face count is 6 from DataDimension::kCube (texture_util.cc:226)")
    bad = False
    try:
        decode_fetch(mk(stack_depth=0))
    except ValueError:
        bad = True
    ck(bad, "KILLER a CUBE fetch with stack_depth != 5 REFUSES (the Xenos "
            "invariant at texture_util.cc:51), it does not default to 6 faces")

    # KILLER: the face stride is the Xenia-derived subresource stride, and the
    # `6*w*h*bpp` intuition must not be what produces it.
    ck(lay.base.array_slice_stride_bytes == 65536,
       "face stride for 128x128 k_8_8_8_8 tiled = 65,536 B (row pitch 128 "
       "blocks * 4 B = 512, times 128 tile-aligned block rows)")
    ck(lay.base.array_slice_stride_bytes * lay.array_size == 393216,
       "six-face base span = 393,216 B")
    # KILLER: the shape where intuition and authority DISAGREE. I first wrote
    # 65,536 here from my own head and the fixture caught me: the authority
    # says 32,768 (row pitch 512 B from the fetch pitch, times align(33,32)=64
    # tile-aligned block rows). That is the whole reason this tool exists.
    wide = guest_texture_layout(DIM_CUBE, 4, 33, 33, 6, True, 6, True, True, 0)
    naive = 6 * 33 * 33 * 4
    ck(wide.base.array_slice_stride_bytes == 32768,
       "a 33x33 cube's face stride is 32,768 B (512 B row pitch x align(33,32)"
       "=64 block rows), NOT the 65,536 of the 128x128 case (got %d)"
       % wide.base.array_slice_stride_bytes)
    ck(wide.base.array_slice_stride_bytes * 6 != naive,
       "KILLER for 33x33 the authority says %d B and 6*w*h*bpp says %d - they "
       "DISAGREE, so the intuition can never be what we ship"
       % (wide.base.array_slice_stride_bytes * 6, naive))

    # KILLER: mip_address == 0 means NO mips, whatever mip_max_level says.
    nomip = decode_fetch(mk(mip_page=0, mip_max=7))
    ck(nomip["mip_max_level"] == 0 and nomip["mip_page"] == 0,
       "KILLER mip_page == 0 forces mip_max_level to 0 (texture_util.cc:81-84) "
       "- a nonzero mip_max_level field alone does NOT mean mips exist")
    withmip = decode_fetch(mk(mip_page=0x03060, mip_max=7))
    ck(withmip["mip_max_level"] == 7
       and (withmip["mip_page"] << 12) == 0x03060000,
       "with a real mip_address the level count survives and the byte address "
       "is page<<12")

    # KILLER: our own tiled2D must agree with this port, or one is wrong.
    ck(tiled_2d(0, 0, 128, 2) == 0 and tiled_2d(8, 0, 128, 2) != 0,
       "tiled2D is not the identity (it interleaves within a 32x32 tile)")
    ck(tiled_upper_bound_2d(128, 128, 128, 2) == 65536,
       "KILLER the tiled upper bound for a full 128x128 4bpp face is exactly "
       "the 65,536 B face, so extent and stride agree for this shape (got %d)"
       % tiled_upper_bound_2d(128, 128, 128, 2))

    # KILLER: a 3D or 1D dimension must REFUSE, not silently take the 2D path.
    for d, nm in ((DIM_3D, "3D"), (DIM_1D, "1D")):
        r = False
        try:
            guest_texture_layout(d, 4, 64, 64, 64, True, 6, True, True, 0)
        except ValueError:
            r = True
        ck(r, "KILLER a %s dimension REFUSES rather than returning a 2D-shaped "
              "answer" % nm)

    # KILLER: an unknown format refuses rather than assuming 4 bytes/block.
    r = False
    try:
        guest_texture_layout(DIM_CUBE, 4, 128, 128, 6, True, 63, True, True, 0)
    except ValueError:
        r = True
    ck(r, "KILLER an unknown texture format REFUSES rather than assuming a "
          "block size")

    # The packed mip tail is UNKNOWN, and must say so rather than report a
    # wrong extent.
    ck(packed_mip_level(128, 128) == 3,
       "packed mip level for 128x128 is 3 (texture_util.h:41-44)")
    tail = guest_texture_layout(DIM_CUBE, 4, 128, 128, 6, True, 6, True, True, 7)
    ck(tail.unknown_extent_levels == set(),
       "the PACKED MIP TAIL is now COMPUTED - nothing is left UNKNOWN (got %s)"
       % sorted(tail.unknown_extent_levels))

    # ── THE PACKED MIP OFFSETS (texture_util.cc:116-205) ─────────────────────
    # A 128x128 square: log2_width == log2_height, so it takes the ELSE arm
    # ("taller than wide, laid out horizontally") and packs along X. Getting
    # that branch backwards is the easiest error in the whole function and is
    # invisible on any square texture unless asserted directly.
    ck(packed_mip_offset(128, 128, 1, 6, 3) == (True, 16, 0, 0),
       "KILLER a SQUARE 128x128 takes the log2_width > log2_height ELSE arm: "
       "mip 3 sits at x=16, y=0 (got %s)"
       % (packed_mip_offset(128, 128, 1, 6, 3),))
    ck(packed_mip_offset(128, 128, 1, 6, 4) == (True, 8, 0, 0)
       and packed_mip_offset(128, 128, 1, 6, 5) == (True, 4, 0, 0),
       "mips 4 and 5 halve along X (16>>1, 16>>2)")
    ck(packed_mip_offset(128, 128, 1, 6, 6) == (True, 0, 8, 0),
       "KILLER at packed_mip >= 3 the layout FLIPS to the Y axis: mip 6 sits "
       "at x=0, y=8 (got %s)" % (packed_mip_offset(128, 128, 1, 6, 6),))
    ck(packed_mip_offset(128, 128, 1, 6, 7) == (True, 0, 4, 0),
       "mip 7 sits at x=0, y=4")
    ck(packed_mip_offset(128, 128, 1, 6, 2)[0] is False,
       "KILLER mip 2 is NOT in the tail for a 128x128 (the tail starts at 3), "
       "and the function says so rather than returning an offset")

    # A WIDE texture takes the OTHER arm - the only way to prove the branch is
    # read at all rather than always falling one way.
    ck(packed_mip_offset(256, 64, 1, 6, 2) == (True, 0, 16, 0),
       "KILLER a WIDE 256x64 takes the log2_width > log2_height arm and packs "
       "along Y (got %s)" % (packed_mip_offset(256, 64, 1, 6, 2),))

    # The block shift: a DXT1 offset is in BLOCKS, so it is shifted down by
    # log2(4). Dropping it overstates the tail by 4x in each axis.
    ck(packed_mip_offset(128, 128, 1, 18, 3) == (True, 4, 0, 0),
       "KILLER a DXT1 packed offset is in BLOCKS - 16 texels >> log2(4) = 4 "
       "(texture_info_formats.cc:45-53) (got %s)"
       % (packed_mip_offset(128, 128, 1, 18, 3),))

    # The `log2_size > 4` guard on packed_mip_base is a NO-OP for anything
    # 32 texels or larger, so no observed texture can test it. At 8x8 it bites:
    # without the guard packed_mip_base goes NEGATIVE and every offset halves.
    ck(packed_mip_level(8, 8) == 0
       and packed_mip_offset(8, 8, 1, 6, 0) == (True, 16, 0, 0),
       "KILLER an 8x8 texture has packed_mip_base 0 via the `log2_size > 4` "
       "guard, so mip 0 sits at x=16 - dropping the guard makes the base "
       "NEGATIVE and halves every offset (got %s)"
       % (packed_mip_offset(8, 8, 1, 6, 0),))
    eight = guest_texture_layout(DIM_CUBE, 1, 8, 8, 6, True, 6, True, True, 3)
    ck(eight.packed_level == 0 and eight.mips_total_extent_bytes == 24576,
       "KILLER an 8x8 cube has packed_level==0 AND a separate 24,576 B mip "
       "tail (texture_util.cc:284-289 two-tail loop). Collapsing iteration 1 "
       "makes this 0 (got packed=%s extent=%s)"
       % (eight.packed_level, eight.mips_total_extent_bytes))
    ck(0 in eight.mips and eight.mips[0].array_slice_stride_bytes == 4096,
       "KILLER the packed_level==0 mip tail is stored as mips[0]")

    ck(tail.mips[3].x_extent_blocks == 32 and tail.mips[3].y_extent_blocks == 16,
       "KILLER the tail extent is the MAX over every sublevel in it, not just "
       "the packed level: x=32 (mip 3 at x=16 plus its own 16 wide), y=16 "
       "(got %d x %d)" % (tail.mips[3].x_extent_blocks,
                          tail.mips[3].y_extent_blocks))
    ck(tail.mips_total_extent_bytes == 147456,
       "the six-face mip chain of a 128x128 k_8_8_8_8 cube totals 147,456 B "
       "(got %d)" % tail.mips_total_extent_bytes)

    # ⛔ THE SHAPE I CLAIMED DID NOT EXIST. I measured seven near-square shapes,
    # found the tail extent always maximised at the packed level itself, and
    # concluded it ALWAYS is -- a universal claim from seven samples. The
    # director produced the counterexample: a SKINNY 1x17. Its packed level is
    # 0, so sublevel 0 gives y = 0 + 17 = 17, but sublevel 3 gives
    # y = 16 + 2 = 18. The max is NOT at the packed level, and a tail extent
    # that only looks at the packed level under-reports the region by a row.
    #
    # This is also why base and mips differ here: at packed_level == 0 the base
    # sums only sublevel 0 while the mips entry sums 0..max_level
    # (texture_util.cc:404 `packed_sublevel_last = is_base ? 0 : max_level`).
    skinny = guest_texture_layout(DIM_2D_OR_STACKED, 1, 1, 17, 1, True, 6,
                                  True, True, 4)
    ck(skinny.packed_level == 0,
       "a 1x17 texture packs from level 0 (min dimension <= 16)")
    ck(skinny.base.y_extent_blocks == 17,
       "KILLER the BASE of a 1x17 sums only sublevel 0 -> y extent 17 (got %d)"
       % skinny.base.y_extent_blocks)
    ck(0 in skinny.mips and skinny.mips[0].y_extent_blocks == 18,
       "KILLER the MIPS entry of a 1x17 sums sublevels 0..4, and sublevel 3 "
       "sits at y=16 with 2 rows of its own -> y extent 18, NOT the 17 the "
       "packed level alone gives. Seven near-square shapes could not see this "
       "(got %s)" % (skinny.mips[0].y_extent_blocks if 0 in skinny.mips else "ABSENT"))
    ck(tail.base.array_slice_stride_bytes == 65536,
       "KILLER the BASE six-face span survives a mip tail this tool cannot "
       "size - the cube slice's own number is not blocked by it")

    # ── BLIND SPOT 1, found by mutate_cube_layout.py ─────────────────────────
    # Six must come from DataDimension::kCube, not from whatever the caller
    # passed as depth_or_array_size. Every real call passes 6 for a cube, so
    # reading the argument instead is INVISIBLE on real data.
    lie = guest_texture_layout(DIM_CUBE, 4, 128, 128, 1, True, 6, True, True, 0)
    ck(lie.array_size == 6,
       "KILLER a CUBE is SIX faces even when the caller passes "
       "depth_or_array_size=1 - the count comes from the DIMENSION "
       "(texture_util.cc:226-228), not from the argument (got %d)"
       % lie.array_size)
    stk = guest_texture_layout(DIM_2D_OR_STACKED, 4, 128, 128, 3, True, 6,
                               False, True, 0)
    ck(stk.array_size == 3,
       "...while a STACKED 2D texture DOES take its array size from the "
       "argument, so the two paths are genuinely different (got %d)"
       % stk.array_size)

    # ── BLIND SPOT 2 ─────────────────────────────────────────────────────────
    # The row pitch is aligned to 32 BLOCKS. For a 4-bytes-per-block 128-texel
    # pitch that is a no-op, which is why the observed cubes cannot test it. A
    # DXT1 cube with a 32-texel pitch has 8 blocks per row and MUST still be
    # tile-aligned to 32.
    dxt = guest_texture_layout(DIM_CUBE, 1, 32, 32, 6, True, 18, False, True, 0)
    ck(dxt.base.row_pitch_bytes == 32 * 8,
       "KILLER a DXT1 cube with a 32-texel pitch has 8 blocks per row but its "
       "row pitch is tile-aligned to 32 blocks x 8 B = 256 B, NOT 8 x 8 = 64 "
       "(got %d)" % dxt.base.row_pitch_bytes)

    # ── BLIND SPOT 3 ─────────────────────────────────────────────────────────
    # The face stride is aligned up to kTextureSubresourceAlignmentBytes
    # (4096). Every observed face is 65,536 B, already a multiple, so dropping
    # the alignment changes nothing on real data. A small k_8 cube's face is
    # 1,024 B and MUST still be padded to a 4 KB subresource.
    tiny = guest_texture_layout(DIM_CUBE, 1, 32, 32, 6, True, 2, False, True, 0)
    ck(tiny.base.row_pitch_bytes * tiny.base.z_slice_stride_block_rows == 1024
       and tiny.base.array_slice_stride_bytes == 4096,
       "KILLER a 32x32 k_8 cube face is 1,024 B of data but a 4,096 B "
       "SUBRESOURCE (xenos.h:1177-1178) - dropping that alignment is "
       "invisible on the observed 65,536 B faces (got %d)"
       % tiny.base.array_slice_stride_bytes)

    # And the same alignment must bite on a MIP stride, where the tool's own
    # numbers show it: level 3 of a 128x128 cube is a 16-texel row rounded up
    # to a 32-block tile.
    m = guest_texture_layout(DIM_CUBE, 4, 128, 128, 6, True, 6, False, True, 3)
    ck(m.mips[3].row_pitch_bytes == 128
       and m.mips[3].array_slice_stride_bytes == 4096,
       "KILLER mip level 3 (16x16) is tile-aligned to a 32-block row (128 B) "
       "and a 4,096 B subresource, NOT 16x16x4 = 1,024 (got pitch %d, stride "
       "%d)" % (m.mips[3].row_pitch_bytes, m.mips[3].array_slice_stride_bytes))

    # KILLER: the conservation equation must be able to FAIL. If the guest's
    # mip_address does not sit exactly six face strides after base_address, a
    # six-face base is contradicted by the guest's own addressing.
    good = describe(mk(base_page=0x03050, mip_page=0x030b0, mip_max=7))
    ck(good["conserved"] is True and good["mip_minus_base"] == 0x60000,
       "CONSERVATION holds when mip_address - base_address == 6 x face stride")
    off_by = describe(mk(base_page=0x03050, mip_page=0x03090, mip_max=7))
    ck(off_by["conserved"] is False,
       "KILLER the conservation check FAILS when the guest's mip_address is "
       "nearer than six face strides (got %r) - it is a test, not a formality"
       % off_by["mip_minus_base"])

    print("\n%d/%d cube-layout fixtures behave as specified" % (np, np + nf))
    return 0 if nf == 0 else 1


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace", nargs="?")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--expect", action="store_true",
                    help="fail if a decoded row disagrees with EXPECTED")
    a = ap.parse_args()

    if a.self_test:
        return self_test()
    if not a.trace:
        print(__doc__)
        return 2
    if not os.path.exists(a.trace):
        print("REFUSED: no such trace: %s" % a.trace)
        return 2

    # scanout_route.walk signals a non-SRHT file with sys.exit(str), which
    # would surface as exit 1 - the code this tool documents for a
    # DISAGREEMENT, not a refusal. Translate it to the documented 2.
    try:
        cubes, slots_of, binds = scan_cubes(a.trace)
    except SystemExit as e:
        print("REFUSED: %s" % e)
        return 2
    if not cubes:
        print("REFUSED: the trace contains NO cube fetch entries. Either it "
              "predates the fetch shadow, or this era binds no cubes - either "
              "way there is nothing to check and a pass would be vacuous.")
        return 2

    print("CUBE TEXTURES in %s" % a.trace)
    print("(fetch shadow: base %d, stride %d, %d slots - ENGINE_MAP 6g.3c)\n"
          % (FETCH_SHADOW_BASE, FETCH_SHADOW_STRIDE, FETCH_SHADOW_SLOTS))
    rc = 0
    for base in sorted(cubes):
        try:
            r = describe(cubes[base])
        except ValueError as e:
            print("  %#010x  REFUSED: %s" % (base, e))
            rc = 2
            continue
        print("  key %#010x   slots %s   %d bind observation(s)"
              % (base, sorted(slots_of[base]), binds[base]))
        print("     %dx%d  fmt %d (%s)  tiled=%d  endian=%d (%s)  pitch=%d"
              % (r["width"], r["height"], r["format"], r["fmt_name"],
                 r["tiled"], r["endian"], r["endian_name"], r["pitch_texels"]))
        print("     base_address %#010x   mip_address %#010x   "
              "mip_min/max %d/%d   packed_mips=%d (tail level %d)"
              % (r["base_address"], r["mip_address"], r["mip_min_level"],
                 r["mip_max_level"], r["packed_mips"], r["packed_level"]))
        if r["layout_error"]:
            print("     LAYOUT: UNKNOWN - %s" % r["layout_error"])
        else:
            print("     faces %d   face stride %d B   SIX-FACE BASE SPAN %d B"
                  % (r["faces"], r["face_stride_bytes"], r["base_span_bytes"]))
            print("     base level data extent %d B   mips total extent %d B"
                  % (r["base_data_extent_bytes"],
                     r["mips_total_extent_bytes"]))
            print("     mip offsets %s   strides %s   UNKNOWN extent at %s"
                  % (r["mip_offsets"], r["mip_strides"],
                     r["unknown_extent_levels"] or "none"))
            if r["mip_minus_base"] is not None:
                print("     CONSERVATION: mip_address - base_address = %d B, "
                      "6 x face stride = %d B  ->  %s"
                      % (r["mip_minus_base"], r["base_span_bytes"],
                         "HOLDS" if r["conserved"] else "DOES NOT HOLD"))
                if not r["conserved"]:
                    print("     ^ the guest's own addressing DISAGREES with a "
                          "six-face base. That is a finding, not a rounding "
                          "error - do not proceed on the six-face assumption.")
        if a.expect:
            e = EXPECTED.get(base)
            if e is None:
                print("     MISMATCH: no EXPECTED row for this key")
                rc = 1
            else:
                for k, v in sorted(e.items()):
                    if r.get(k) != v:
                        print("     MISMATCH %s: expected %r, decoded %r"
                              % (k, v, r.get(k)))
                        rc = 1
        print("")

    print("%d distinct cube key(s)." % len(cubes))
    if a.expect and rc == 0:
        print("Every decoded row matches the values committed in "
              "docs/CUBE_CONFORMANCE_READING.md.")
    return rc


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        sys.exit(0)
