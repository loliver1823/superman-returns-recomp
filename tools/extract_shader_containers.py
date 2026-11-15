#!/usr/bin/env python3
"""Carve Xbox360 D3D9 shader CONTAINERS out of the golden Xenia dump and
cross-match their ucode against the distinct-shader census of the golden trace.

Shader-spike step 1+2 (iter-359, recon workflow wf_801228d7):
  - Containers are XenosRecomp's native input (tools/XenosRecomp-clone/XenosRecomp/
    shader.h): big-endian header, magic dword 0x102A11xx at +0 (bit0 1=VS 0=PS),
    virtualSize +4, physicalSize +8, constantTableOffset +0x10, definitionTable
    +0x14, shaderOffset +0x18; Shader struct at base+shaderOffset holds
    physicalOffset/size; raw ucode at base+virtualSize+physicalOffset.
  - The golden trace holds RAW UCODE only (the GPU never reads the container's
    virtual part), so containers must come from guest MEMORY: the resident
    bundle at phys 0x40000000, the XEX image, and a small low-heap group.
  - Cross-match key = sha1(ucode_bytes)[:16], identical to the trace census
    (scratchpad shader_census.py / the SR_DUMP_UCODE path). The game patches
    1-2 dwords (fetch-slot indices) into otherwise identical ucode, so
    near-misses (same size, few differing dwords) are reported too.

Usage:
  python tools/extract_shader_containers.py [--out data/shaders_carved]
                                            [--census <dir of kind_hash_sizeB.bin>]
"""
import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden  # noqa: E402

MAGIC_VS = b"\x10\x2a\x11\x01"
MAGIC_PS = b"\x10\x2a\x11\x00"  # (flags & 0xFFFFFF00)==0x102A1100, bit0 = VS


def u32(buf, off):
    return struct.unpack_from(">I", buf, off)[0] if off + 4 <= len(buf) else None


def find_containers(g):
    """Magic-scan the ENTIRE mapped guest space (the golden dump maps physical
    memory at multiple VA aliases — 0xB7/0xD7/0xF7 etc. — content-hash dedupe
    downstream collapses those). Yield (baseVA, kind, virt, phys, ctO, shO, po, sz)."""
    out = []
    for magic, kind in ((MAGIC_VS, "vs"), (MAGIC_PS, "ps")):
        for base in g.find(magic, limit=1000000):
            try:
                hdr = g.read(base, 0x28)
            except Exception:
                continue
            virt, phys = u32(hdr, 4), u32(hdr, 8)
            ctO, shO = u32(hdr, 16), u32(hdr, 24)
            if not (virt and 0x40 <= virt <= 0x10000 and virt % 4 == 0
                    and phys is not None and phys <= 0x100000
                    and shO and 0 < shO < virt
                    and ctO is not None and ctO < virt):
                continue
            try:
                sh = g.read(base + shO, 8)
            except Exception:
                continue
            po, sz = u32(sh, 0), u32(sh, 4)
            if po is None or not sz or sz > 0x40000 or po + sz > phys:
                continue
            if ctO:
                # validate the D3DX constant table when present
                try:
                    ct = g.read(base + ctO, 0x10)
                except Exception:
                    continue
                if u32(ct, 4) != 0x1C:
                    continue
            out.append((base, kind, virt, phys, ctO, shO, po, sz))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="data/shaders_carved")
    ap.add_argument("--census", default=None,
                    help="dir of trace-census blobs named kind_hash_sizeB.bin")
    args = ap.parse_args()

    g = golden.GoldenDump()
    os.makedirs(args.out, exist_ok=True)

    found = find_containers(g)
    print(f"magic scan: {len(found)} validated container sites (incl. VA aliases)")
    carved = {}  # ucode_hash -> record (first wins; VA aliases dedupe here)
    dup = 0
    for base, kind, virt, phys, ctO, shO, po, sz in found:
        try:
            blob = g.read(base, virt + phys)
        except Exception:
            continue
        ucode = blob[virt + po: virt + po + sz]
        h = hashlib.sha1(ucode).hexdigest()[:16]
        if h in carved:
            dup += 1
            continue
        fn = f"{kind}_{h}_{base:08X}.xpu"
        with open(os.path.join(args.out, fn), "wb") as f:
            f.write(blob)
        carved[h] = dict(kind=kind, base=base, virt=virt,
                         phys=phys, ctO=ctO, ucode=ucode, file=fn)

    vs = sum(1 for c in carved.values() if c["kind"] == "vs")
    ps = len(carved) - vs
    noctab = sum(1 for c in carved.values() if c["ctO"] == 0)
    print(f"\nCARVED: {len(carved)} unique-by-ucode containers ({vs} VS, {ps} PS), "
          f"{dup} duplicate-ucode skipped, {noctab} with no constant table (XEX built-ins)")
    print(f"wrote -> {args.out}")

    if not args.census:
        return
    # ---- cross-match against the trace census ------------------------------
    # The live IM_LOAD ucode is the container template with vfetch operand
    # dwords RUNTIME-PATCHED (fetch slot/offset/stride per bound vertex decl;
    # verified: diffs cluster in 3-dword vfetch groups, XOR confined to operand
    # fields). The shader cache must key on the PATCHED bytes, so unmatched
    # census blobs get a SYNTHESIZED container: best same-size template with
    # its ucode section replaced by the patched trace bytes — metadata stays
    # valid, the translated ucode becomes exactly what the game executes.
    census = {}
    for fn in os.listdir(args.census):
        if not fn.endswith(".bin"):
            continue
        kind, h, _ = fn.split("_", 2)
        with open(os.path.join(args.census, fn), "rb") as f:
            census[h] = dict(kind=kind, blob=f.read(), file=fn)
    exact = [h for h in census if h in carved]
    missing = [h for h in census if h not in carved]
    print(f"\nCROSS-MATCH vs trace census ({len(census)} uniques): "
          f"{len(exact)} exact, {len(missing)} unmatched")

    manifest = {h: dict(kind=carved[h]["kind"], file=carved[h]["file"],
                        source="exact") for h in exact}
    synth, miss = 0, 0
    MAX_PATCH_DW = 32
    for h in sorted(missing):
        cb = census[h]["blob"]
        best = None  # (diff_dwords, carved_hash)
        for ch, c in carved.items():
            if c["kind"] != census[h]["kind"] or len(c["ucode"]) != len(cb):
                continue
            d = sum(1 for i in range(0, len(cb), 4) if cb[i:i+4] != c["ucode"][i:i+4])
            if best is None or d < best[0]:
                best = (d, ch)
        if best and best[0] <= MAX_PATCH_DW:
            tmpl = carved[best[1]]
            blob = bytearray(open(os.path.join(args.out, tmpl["file"]), "rb").read())
            virt = struct.unpack(">I", blob[4:8])[0]
            shO = struct.unpack(">I", blob[24:28])[0]
            po = struct.unpack(">I", blob[shO:shO + 4])[0]
            blob[virt + po: virt + po + len(cb)] = cb
            fn = f"{census[h]['kind']}_{h}_synth.xpu"
            with open(os.path.join(args.out, fn), "wb") as f:
                f.write(blob)
            manifest[h] = dict(kind=census[h]["kind"], file=fn, source="synth",
                               template=best[1], patched_dwords=best[0])
            synth += 1
            print(f"  SYNTH {census[h]['file']}: template {best[1]} + "
                  f"{best[0]} patched dword(s)")
        else:
            miss += 1
            print(f"  MISS {census[h]['file']}: no same-size candidate "
                  f"(best diff {best[0] if best else 'n/a'})")
    import json
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    print(f"\nCOVERAGE: {len(exact)} exact + {synth} synthesized "
          f"(vfetch-patched) + {miss} missing of {len(census)} trace uniques "
          f"= {len(exact) + synth}/{len(census)}")
    print(f"manifest (ucode-hash -> container file) -> "
          f"{os.path.join(args.out, 'manifest.json')}")


if __name__ == "__main__":
    main()
