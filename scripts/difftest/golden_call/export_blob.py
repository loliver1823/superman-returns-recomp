#!/usr/bin/env python3
"""export_blob.py - export guest-memory ranges of a golden dump (tools/golden.py) into the flat blob
both sides of the golden-call differential load: repeated [u32 va, u32 size, bytes] (LE headers).

Default ranges: the XEX image window 0x82000000..0x83000000 and the low heap 0x00010000..0x06630000
(Havok world, bodies, shapes, MOPP code and the character proxies of the sound_gameplay golden).

    python scripts/difftest/golden_call/export_blob.py --dump sound_gameplay --out archive/golden_call/golden_mem.bin
    python ... --range 82000000:83000000 --range 00010000:06630000
    python ... --image ours:archive/v1_invest/collision/ours_midtown.dmp --range 82000000:8D000000                --out archive/golden_call_mover/ours_midtown_mem.bin
--image takes any tools/havok_snapshot.py image spec (xenia:<name>, ours:<dmp>): only non-zero
64 KiB pages inside the ranges are exported (our dumps reserve the whole 4 GiB).
"""
import argparse
import os
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import golden  # noqa: E402


def export_image(spec, want, out_path):
    import havok_snapshot as hs  # noqa: E402  (tools/, same image layer as the snapshot tools)
    img = hs.open_image(spec)
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    total = 0
    with open(out_path, "wb") as out:
        for lo, hi in want:
            for va, data in img.iter_chunks(lo, hi):
                run_va, run = None, []
                for o in range(0, len(data), 0x10000):
                    pg = data[o:o + 0x10000]
                    if pg.count(0) != len(pg):
                        if run_va is None:
                            run_va = va + o
                        run.append(pg)
                        continue
                    if run_va is not None:
                        blob = b"".join(run)
                        out.write(struct.pack("<II", run_va, len(blob)))
                        out.write(blob)
                        total += len(blob)
                        run_va, run = None, []
                if run_va is not None:
                    blob = b"".join(run)
                    out.write(struct.pack("<II", run_va, len(blob)))
                    out.write(blob)
                    total += len(blob)
    print("%s: %d bytes (%s)" % (out_path, total, img.describe()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", default="sound_gameplay")
    ap.add_argument("--out", default=os.path.join(ROOT, "archive", "golden_call", "golden_mem.bin"))
    ap.add_argument("--range", action="append", default=[])
    ap.add_argument("--image", help="havok_snapshot image spec (ours:<dmp> / xenia:<name>) instead of --dump")
    a = ap.parse_args()
    want = [tuple(int(x, 16) for x in r.split(":")) for r in a.range] or [(0x82000000, 0x83000000), (0x00010000, 0x06630000)]
    if a.image:
        return export_image(a.image, want, a.out)
    g = golden.GoldenDump(a.dump)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    total = 0
    with open(a.out, "wb") as out:
        for host, size, foff in g.ranges:
            if host < golden.GUEST_HOST_BASE or host >= golden.GUEST_HOST_BASE + 0x100000000:
                continue
            va = host - golden.GUEST_HOST_BASE
            for lo, hi in want:
                s, e = max(va, lo), min(va + size, hi)
                if s < e:
                    out.write(struct.pack("<II", s, e - s))
                    out.write(g._m[foff + (s - va): foff + (e - va)])
                    total += e - s
    print("%s: %d bytes" % (a.out, total))


if __name__ == "__main__":
    main()
