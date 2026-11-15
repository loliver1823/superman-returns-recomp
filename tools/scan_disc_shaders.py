"""Carve Xbox 360 D3D9 shader containers out of the game's own files.

The corpus in data/shaders_carved came from the golden memory dump (menu era),
so shaders the game loads later - the FMV composite, gameplay materials - are
missing from the shader cache and their draws are rejected (XLAT_REJECT
gate=1). The containers live in the game's files (.AST archives, the XEX, the
.img), possibly compressed; this scans the raw bytes for the container magic
and validates the header the same way tools/extract_shader_containers.py does.

Every container found is written as <kind>_<hash16>_<file>_<offset>.xpu, where
hash16 is the XXH3-64 (the shader cache key) of the raw big-endian ucode -
the key the replay/runtime logs as vs=/ps=.

    python tools/scan_disc_shaders.py --out archive/disc_shaders [--want 4D9002F1AD6DBE28,...]
"""
import argparse
import os
import struct
import sys

try:
    import xxhash
except ImportError:  # pragma: no cover
    xxhash = None

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join(REPO, "Superman Returns (USA)")
MAGICS = ((b"\x10\x2a\x11\x01", "vs"), (b"\x10\x2a\x11\x00", "ps"))


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0] if o + 4 <= len(b) else None


def carve(buf):
    """Yield (offset, kind, blob, ucode) for every valid container in buf."""
    for magic, kind in MAGICS:
        pos = buf.find(magic)
        while pos >= 0:
            if pos % 4 == 0:
                virt, phys = u32(buf, pos + 4), u32(buf, pos + 8)
                ct_o, sh_o = u32(buf, pos + 16), u32(buf, pos + 24)
                if (virt and 0x40 <= virt <= 0x10000 and virt % 4 == 0 and phys is not None
                        and 0 < phys <= 0x100000 and sh_o and 0 < sh_o < virt
                        and ct_o is not None and ct_o < virt):
                    po, sz = u32(buf, pos + sh_o), u32(buf, pos + sh_o + 4)
                    if po is not None and sz and po + sz <= phys and pos + virt + phys <= len(buf):
                        blob = buf[pos:pos + virt + phys]
                        ucode = blob[virt + po:virt + po + sz]
                        yield pos, kind, blob, ucode
            pos = buf.find(magic, pos + 4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(REPO, "archive", "disc_shaders"))
    ap.add_argument("--want", default="", help="comma list of 16-hex ucode hashes to report")
    a = ap.parse_args()
    if xxhash is None:
        sys.exit("pip install xxhash")
    want = {w.strip().lower() for w in a.want.split(",") if w.strip()}
    os.makedirs(a.out, exist_ok=True)
    seen = {}
    for root, _, files in os.walk(GAME):
        for fn in files:
            path = os.path.join(root, fn)
            with open(path, "rb") as f:
                buf = f.read()
            n = 0
            for off, kind, blob, ucode in carve(buf):
                h = "%016x" % xxhash.xxh3_64_intdigest(ucode)
                if h in seen:
                    continue
                name = "%s_%s_%s_%08X.xpu" % (kind, h, fn.replace(".", "-")[:24], off)
                with open(os.path.join(a.out, name), "wb") as o:
                    o.write(blob)
                seen[h] = name
                n += 1
            if n:
                print("%-40s %5d new containers" % (fn[:40], n))
    print("unique containers:", len(seen), "->", a.out)
    for w in sorted(want):
        print("want %s: %s" % (w, seen.get(w, "NOT FOUND")))


if __name__ == "__main__":
    main()
