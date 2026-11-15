#!/usr/bin/env python3
"""mutate_cube_layout.py -- prove tools/check_cube_layout.py can FAIL.

A layout checker that passes is worth nothing until you have watched it fail.
Each mutant below is a WRONG cube layout rule that a reasonable person could
write -- several are rules I very nearly wrote myself, and one (the naive
`6*w*h*bpp` face stride) is the exact intuition the director forbade deriving
from. A mutant that survives means the checker cannot see that error, and the
corresponding row of docs/CUBE_CONFORMANCE_READING.md is unverified prose.

The four fields the acceptance names -- face-count, face-stride, mip-address,
endianness -- each have at least one mutant here.

Detection means the mutant changes an outcome WITHOUT crashing: a traceback
proves only that the file was edited, not that the checker discriminates.

    python tools/mutate_cube_layout.py

Exit 0 = every mutant detected.
"""
import os, shutil, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(REPO, "tools", "check_cube_layout.py")
TRACE = "archive/iter456_title_fresh.srht"

# (name, find, replace) -- `find` must appear EXACTLY once in the live tool.
MUTANTS = [
    # ── FACE COUNT ──────────────────────────────────────────────────────────
    ("face count comes from the array arg instead of the CUBE dimension",
     "        layout.array_size = 6          # :229 -- SIX, from the DIMENSION",
     "        layout.array_size = depth_or_array_size"),

    ("face count is 1 (a cube treated as a plain 2D image)",
     "        layout.array_size = 6          # :229 -- SIX, from the DIMENSION",
     "        layout.array_size = 1"),

    ("the stack_depth == 5 Xenos invariant is not enforced",
     "        if sd != 5:\n            raise ValueError(",
     "        if False:\n            raise ValueError("),

    # ── FACE STRIDE ─────────────────────────────────────────────────────────
    # THE FORBIDDEN INTUITION. For the observed 128x128 cubes this produces the
    # RIGHT number, so it can only be caught by a shape where they disagree --
    # which is exactly why the 33x33 fixture exists.
    ("face stride is the naive 6*w*h*bpp intuition the director forbade",
     "        L.array_slice_stride_bytes = (L.row_pitch_bytes\n"
     "                                      * L.z_slice_stride_block_rows)",
     "        L.array_slice_stride_bytes = width_texels * height_texels * bpb"),

    ("the row pitch is not tile-aligned (align(...,32) dropped)",
     "        row_pitch_blocks_tile_aligned = align(\n"
     "            align(row_pitch_texels_unaligned, block_w) // block_w, K_TILE_WH)",
     "        row_pitch_blocks_tile_aligned = ("
     "align(row_pitch_texels_unaligned, block_w) // block_w)"),

    ("the block-row count is not tile-aligned",
     "        L.z_slice_stride_block_rows = align(\n"
     "            align(z_rows_unaligned, block_h) // block_h, K_TILE_WH)",
     "        L.z_slice_stride_block_rows = ("
     "align(z_rows_unaligned, block_h) // block_h)"),

    ("the subresource 4 KB alignment is dropped from the face stride",
     "        L.array_slice_stride_bytes = align(L.array_slice_stride_bytes,\n"
     "                                           K_SUBRES_ALIGN)   # :350-352",
     "        pass   # alignment dropped"),

    # ── MIP ADDRESS ─────────────────────────────────────────────────────────
    ("mip_address is read as a byte address instead of a page index",
     '        "mip_page": mip_page,',
     '        "mip_page": mip_page >> 12,'),

    ("mip_page == 0 no longer forces mip_max_level to 0",
     "    if mip_page == 0:                                            # :82-90\n"
     "        mip_min = mip_max = 0",
     "    if False:\n        mip_min = mip_max = 0"),

    ("the packed mip tail's extent is filled with the unpacked formula",
     "        if pl is not None and level == pl:",
     "        if False:"),

    ("packed_level==0 does not emit a separate mip tail (the r2 C++ defect)",
     "        loop_level_last = 1 if max_level != 0 else 0",
     "        loop_level_last = 0"),

    # ── THE PACKED MIP OFFSETS, now ported and therefore mutable ─────────────
    ("the wide/tall branch is inverted for packed offsets < 3",
     "    if packed_mip < 3:                                           # :167-177\n"
     "        if log2_width > log2_height:",
     "    if packed_mip < 3:                                           # :167-177\n"
     "        if log2_width >= log2_height:"),

    ("the wide/tall branch is inverted for packed offsets >= 3",
     "        if log2_width > log2_height:\n"
     "            offset = (1 << (log2_width - packed_mip_base)) >> (packed_mip - 2)",
     "        if log2_width >= log2_height:\n"
     "            offset = (1 << (log2_width - packed_mip_base)) >> (packed_mip - 2)"),

    ("the packed offset is not converted from texels to BLOCKS",
     "    x_blocks >>= log2_floor(block_w)\n"
     "    y_blocks >>= log2_floor(block_h)",
     "    x_blocks >>= 0\n    y_blocks >>= 0"),

    # Removing the early return outright makes packed_mip NEGATIVE and the
     # tool CRASHES on `16 >> -1`, which proves only that the file was edited.
     # The realistic error is reporting an unpacked mip as packed at offset 0.
    ("a mip outside the tail is reported as PACKED at offset zero",
     "    if log2_size > 4 + mip:                                      # :156-162\n"
     "        return False, 0, 0, 0",
     "    if log2_size > 4 + mip:                                      # :156-162\n"
     "        return True, 0, 0, 0"),

    # ⛔ RESTORED. I removed this mutant in r2 claiming it was a semantically
    # EQUIVALENT REFACTOR, having measured seven near-square shapes (128x128,
    # 256x64, 64x256, 32x32, 512x512, 16x16, 1024x128) and found the tail
    # extent always maximised at the packed level. That was a UNIVERSAL claim
    # drawn from seven samples, and it is false: the director produced a skinny
    # 1x17 where the max is 17x18 and the packed-level-only rule gives 17x17.
    # The lesson is the standing one -- an "always/never" needs a whole-surface
    # argument, and seven near-square samples is not one. The killing fixture
    # lives in check_cube_layout.py's self-test.
    ("the tail extent uses only the packed level, not every sublevel in it",
     "            packed_sublevel_last = 0 if is_base else max_level",
     "            packed_sublevel_last = pl"),

    ("packed_mip_base ignores the >4 guard",
     "    packed_mip_base = log2_size - 4 if log2_size > 4 else 0       # :163",
     "    packed_mip_base = log2_size - 4"),

    # ── ENDIANNESS ──────────────────────────────────────────────────────────
    ("endianness is read from the wrong bit offset of dword 1",
     '        "endian": (d1 >> 6) & 3,                  # xenos.h:1271',
     '        "endian": (d1 >> 8) & 3,'),

    ("endianness is assumed kNone (no byte swap)",
     '        "endian": (d1 >> 6) & 3,                  # xenos.h:1271',
     '        "endian": 0,'),

    # ── BASE ADDRESS / GEOMETRY ─────────────────────────────────────────────
    ("base_address is not shifted from a page index to bytes",
     '    row["base_address"] = f["base_page"] << 12',
     '    row["base_address"] = f["base_page"]'),

    ("cube size is decoded with the 3D 11/11/10 split instead of 13/13",
     "        w1 = d2 & 0x1FFF\n        h1 = (d2 >> 13) & 0x1FFF\n        da1 = 5",
     "        w1 = d2 & 0x7FF\n        h1 = (d2 >> 11) & 0x7FF\n        da1 = 5"),

    # ── THE CONSERVATION CHECK ITSELF ───────────────────────────────────────
    ("the conservation check always reports HOLDS",
     '        row["conserved"] = (row["mip_minus_base"] == span\n'
     '                            if row["mip_minus_base"] is not None else None)',
     '        row["conserved"] = True'),
]


def run(path, args):
    r = subprocess.run([sys.executable, path] + args, cwd=REPO,
                       capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def outcomes(path):
    """(self-test rc, trace rc, names-393216, says-HOLDS, crashed)."""
    st_rc, st_out = run(path, ["--self-test"])
    tr_rc, tr_out = run(path, [TRACE, "--expect"])
    crashed = any("Traceback (most recent call last)" in o
                  for o in (st_out, tr_out))
    return (st_rc, tr_rc, "393216" in tr_out,
            tr_out.count("HOLDS") == 4, crashed)


def main():
    if not os.path.exists(os.path.join(REPO, TRACE)):
        print("REFUSED: the fixture %s is missing; every comparison below "
              "would be vacuous." % TRACE)
        return 2

    live = open(TOOL, encoding="utf-8", newline="").read()
    with tempfile.TemporaryDirectory() as d:
        ctl = os.path.join(d, "check_cube_layout.py")
        shutil.copy(TOOL, ctl)
        shutil.copy(os.path.join(REPO, "tools", "scanout_route.py"), d)
        base = outcomes(ctl)
        print("control: self-test=%d trace=%d names-393216=%s all-4-HOLD=%s "
              "crashed=%s" % base)
        if base[4] or base[:4] != (0, 0, True, True):
            print("FAIL: the control does not produce the REQUIRED outcomes "
                  "(self-test 0, trace 0, the 393,216 B six-face span named, "
                  "and the conservation equation holding for all four keys). "
                  "Every comparison below would be meaningless.")
            return 1

        print("\n%d mutants:\n" % len(MUTANTS))
        ok = True
        for name, find, repl in MUTANTS:
            if live.count(find) != 1:
                print("  FAIL  %-64s anchor appears %d times (expected 1)"
                      % (name, live.count(find)))
                ok = False
                continue
            open(ctl, "w", encoding="utf-8", newline="").write(
                live.replace(find, repl, 1))
            got = outcomes(ctl)
            if got[4]:
                print("  FAIL  %-64s crashed (proves nothing)" % name)
                ok = False
                continue
            which = []
            labels = ["self-test", "trace", "names-span", "conservation"]
            for i in range(4):
                if got[i] != base[i]:
                    which.append(labels[i])
            det = bool(which)
            print("  %s  %-64s -> (%d,%d,%s,%s) by:%s"
                  % ("PASS" if det else "FAIL", name, got[0], got[1],
                     got[2], got[3], ",".join(which) or "NOTHING"))
            if not det:
                ok = False

        shutil.copy(TOOL, ctl)   # restore, so a later read is not the mutant
        print("\nCUBE LAYOUT MUTATION: %s"
              % ("every mutant changes at least one outcome"
                 if ok else "SOME MUTANTS SURVIVED - the checker is blind to "
                            "them and the corresponding note rows are "
                            "UNVERIFIED"))
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
