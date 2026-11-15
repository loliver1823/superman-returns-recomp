"""Masked vertex-shader keys: one key per vertex program, whatever vertex layout it was bound with.

The XDK patches every vertex shader's vfetch instructions at bind time to match the vertex
declaration (ENGINE_MAP section 6d): data format, stride and offset change, the rest of the
program does not. The runtime shader cache is keyed by XXH3-64 of the PATCHED ucode, so a known
program bound with a new layout misses the cache and its draw is rejected ([XLAT_REJECT] gate=1).

The masked key (mkey) is XXH3-64 of the big-endian ucode with, in every vertex-fetch instruction
the control-flow program executes, exactly these fields zeroed:
    dword 1: format_comp_all (bit 12), num_format_all (13), format (16-21)
    dword 2: stride (0-7), offset (8-30)
Everything else stays in the key: control flow, every ALU and texture-fetch instruction, and in
the vfetch itself the opcode, source/destination registers, fetch-constant slot, prefetch count,
destination swizzle, index rounding, the mini-fetch bit, the predicate bits, and the two value
transforms exp_adjust (dword 1 bits 24-29, a 2^n scale) and signed_rf_mode_all (bit 14). Two
programs with the same mkey therefore compute the same thing from the same attribute stream
positions, and differ only in each attribute's memory layout and data format (stride, offset,
format, signed, normalized/integer) - the columns of the .vs.vfetch sidecar. iter-463 review:
exp_adjust/signed_rf_mode were masked at first; no sidecar column carries them, so a layout-keyed
reuse would have lost them silently (0 of the 879 executed vfetches in the 215 fixture ucodes set
either, so no key changed).

The control-flow walk is xtr_draws.cf_exec_walk (Xenia Shader::AnalyzeUcode +
GatherExecInformation); gfx/shader_cache.cpp ports the same walk and mask, and
tools/difftest/shader_mkey_test.cpp checks the two agree on real ucode.

    python tools/shader_mkey.py key <ucode.bin>...               # print hash, mkey, vfetch fields
    python tools/shader_mkey.py sidecars --containers DIR --out DIR   # <hash>.vs.mkey per VS container
    python tools/shader_mkey.py match <ucode.bin> --containers DIR     # cached VS with the same mkey
"""
import argparse
import os
import struct
import sys

import xxhash

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xtr_draws import cf_exec_walk, decode_vfetch  # noqa: E402

VF_DW1_MASK = 0x003F3000  # format_comp_all | num_format_all | format (exp_adjust, signed_rf stay)
VF_DW2_MASK = 0x7FFFFFFF  # stride | offset (pred_condition, bit 31, stays)


def be32s(b):
    return list(struct.unpack(">%dI" % (len(b) // 4), b[:len(b) // 4 * 4]))


def vfetch_slots(ucode):
    """Instruction slots of every executed vertex fetch, in program order. None if malformed."""
    walk = cf_exec_walk(ucode)
    if walk["malformed"] or not walk["saw_exec"]:
        return None
    dw = be32s(ucode)
    out = []
    for _i, _op, slot, is_fetch, _ser in walk["slots"]:
        if is_fetch and (dw[slot * 3] & 0x1F) == 0 and slot not in out:
            out.append(slot)
    return out


def masked_ucode(ucode):
    slots = vfetch_slots(ucode)
    if slots is None:
        return None
    dw = be32s(ucode)
    for s in slots:
        dw[s * 3 + 1] &= ~VF_DW1_MASK & 0xFFFFFFFF
        dw[s * 3 + 2] &= ~VF_DW2_MASK & 0xFFFFFFFF
    return struct.pack(">%dI" % len(dw), *dw)


def mkey(ucode):
    m = masked_ucode(ucode)
    return None if m is None else xxhash.xxh3_64_intdigest(m)


def vfetch_fields(ucode):
    """Per executed vfetch: the fields the mask removes, plus slot/mini (mini inherits stride)."""
    dw = be32s(ucode)
    out, last_stride = [], 0
    for s in vfetch_slots(ucode) or []:
        f = decode_vfetch(dw[s * 3], dw[s * 3 + 1], dw[s * 3 + 2], s)
        if not f["mini"]:
            last_stride = f["stride"]
        f["stride_eff"] = f["stride"] if not f["mini"] else last_stride
        out.append(f)
    return out


def container_ucode(blob):
    virt = struct.unpack_from(">I", blob, 4)[0]
    sh_o = struct.unpack_from(">I", blob, 24)[0]
    po, sz = struct.unpack_from(">II", blob, sh_o)
    return blob[virt + po:virt + po + sz]


def is_vs_container(blob):
    return (struct.unpack_from(">I", blob, 0)[0] & 1) == 1


def iter_vs_containers(dirs):
    for d in dirs:
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".xpu"):
                continue
            blob = open(os.path.join(d, fn), "rb").read()
            try:
                if not is_vs_container(blob):
                    continue
                u = container_ucode(blob)
            except struct.error:
                continue
            yield os.path.join(d, fn), u


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("key")
    k.add_argument("ucode", nargs="+")
    s = sub.add_parser("sidecars")
    s.add_argument("--containers", nargs="+", required=True)
    s.add_argument("--ucode", nargs="*", default=[], help="dirs of raw BE <hash>.vs.bin (no container)")
    s.add_argument("--out", required=True)
    s.add_argument("--only-cached", help="write only hashes that have <hash>.vs.dxil in this dir")
    m = sub.add_parser("match")
    m.add_argument("ucode")
    m.add_argument("--containers", nargs="+", required=True)
    a = ap.parse_args(argv)

    if a.cmd == "key":
        for p in a.ucode:
            u = open(p, "rb").read()
            mk = mkey(u)
            print("%016x mkey=%s %s" % (xxhash.xxh3_64_intdigest(u), "%016x" % mk if mk is not None else "MALFORMED",
                                        os.path.basename(p)))
            for f in vfetch_fields(u):
                print("   i%-3d slot=%d %s stride=%d offset=%d fmt=%d signed=%d int=%d exp=%d dst_swiz=%03x" % (
                    f["slot"], f["fetch_const"], "mini" if f["mini"] else "full", f["stride_eff"], f["offset"],
                    f["fmt"], f["signed"], f["num_format"], f["exp_adjust"], f["dst_swiz"]))
        return 0
    if a.cmd == "sidecars":
        os.makedirs(a.out, exist_ok=True)
        n = bad = 0
        seen = set()
        raw = [(os.path.join(d, fn), open(os.path.join(d, fn), "rb").read())
               for d in a.ucode for fn in sorted(os.listdir(d)) if fn.endswith(".vs.bin")]
        for path, u in list(iter_vs_containers(a.containers)) + raw:
            h = xxhash.xxh3_64_intdigest(u)
            if h in seen:
                continue
            if a.only_cached and not os.path.exists(os.path.join(a.only_cached, "%016x.vs.dxil" % h)):
                continue
            seen.add(h)
            mk = mkey(u)
            if mk is None:
                bad += 1
                print("MALFORMED %016x %s" % (h, path))
                continue
            with open(os.path.join(a.out, "%016x.vs.mkey" % h), "w") as f:
                f.write("%016x\n" % mk)
            n += 1
        print("wrote %d .vs.mkey sidecars (%d malformed)" % (n, bad))
        return 1 if bad else 0
    if a.cmd == "match":
        u = open(a.ucode, "rb").read()
        mk = mkey(u)
        hits = sorted({"%016x" % xxhash.xxh3_64_intdigest(cu) for p, cu in iter_vs_containers(a.containers)
                       if len(cu) == len(u) and mkey(cu) == mk})
        print("%016x mkey=%016x matches: %s" % (xxhash.xxh3_64_intdigest(u), mk, " ".join(hits) or "none"))
        return 0 if hits else 1
    return 2


if __name__ == "__main__":
    sys.exit(main())
