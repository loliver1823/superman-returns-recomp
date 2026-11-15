#!/usr/bin/env python3
"""mem_res_family.py - memres6's offline differential test: the world family's draws from replay traces with their
EXACT index ranges as the host translation computes them (docs/RELEASE_COMPOSITION.md 9.5 item 1, memres6 (e)).

For every draw of a computed-index vertex shader in the given .xtr traces - the ALU instructions before the first fetch
(the PROLOGUE) compute the index the first fetch (F1, the index stream) reads, and the later fetches index by F1's
element (L2) or by a register the prologue computed (L1) - this tool evaluates the prologue EXACTLY per vertex, with
its own code (numpy float32, one rounding per operation) and XenosRecomp's semantics as tools/xenosrecomp/superman.patch
builds them:
  - operands: a vector operand's component for result i is ((swizzle >> 2i) + i) & 3; a scalar op reads src3's
    ((swizzle >> 6) + 3) & 3 (S0) and swizzle & 3 (S1); the *sc ops c[src3] (S0) and the temp register
    (op & 1) | (src3_sel << 1) | (swizzle & 0x3C) (S1); a temp's bit 7 = abs, a constant's abs = abs_constants; then
    the negate; the vector result is written before the scalar op reads; r0 starts (the vertex index, 0, 0, 0), every
    other register and ps 0;
  - a constant register the container's constant table names reads the live constant file (the draw's register file),
    else the definition table's literal (data/shaders_carved, data/shaders_menu: tools/perf/mem_res_literals.py's
    reading), else the live file; a VS no container defines is evaluated with the live file and marked;
  - ops: add, mul, max / min (HLSL: a NaN operand yields the other), frc, trunc, floor, mad (x * y, then + z), the
    scalar adds / adds_prev / muls / muls_prev(2) / maxs / maxa(s/f) / mins / frcs / truncs / floors / subs / subs_prev
    / mul-add-sub sc / retain_prev, rcp(c/f) = clamp(1 / x, FLT_MIN, FLT_MAX), saturate; any other op in a prologue:
    the draw is UNSUPPORTED here (the census must fall back, reason op);
  - XeVfAddr's index conversion int(floor(index [+ 0.5 when is_index_rounded])); XeVfLoad's 32-bit integer element
    (the endian swap, float(uint) / float(int)); the draw's vertex index range as pm4StageVsf converts its index buffer
    (the offset, the 24-bit mask, the clamp, a strip's reset cut).
Each draw's computed slot gets its exact index range over the draw, or a status: 2 = the exact index is not known (a
non-finite value, an index beyond 2^24, F1 read outside its declared range - the census must fall back), 1 = the
prologue is outside the evaluator (the census must fall back). The fixture (`--out`) holds every distinct draw (the VS
ucode, the 1024 VS constant dwords, the 192 fetch constant dwords, the index buffer and its conversion's parameters,
F1's stream bytes) and its expectation; `difftest --mem-resources-family-test <fixture>` replays it through the census's
production DrawScope and requires every exact range inside the census's bound (and the census's own witness clean).
ADVERSARIAL VARIANTS of each VS's first draw (the same stream and index buffer) change c4 - the instance's vertex count
(c4.x) and base (c4.y): x = 1, 0.5 (each quotient (v + 0.5) / 0.5 an exact integer), the float above 0.5 (each just
below one), 2.5, 1e30, -4 (rcp clamps to FLT_MIN), 0 (rcp FLT_MAX: not finite - the census must fall back), base 3.

    python tools/perf/mem_res_family.py --out archive/memres/family_n7.bin [--json summary.json]
                                        [--sentinel PATH] <trace.xtr> [...]

--sentinel: the PC's quiet-window file - the run stops (exit 3, nothing written) when it appears.
"""
import argparse
import collections
import hashlib
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, HERE)
import xtr_draws as X  # noqa: E402
import mem_res_literals as LIT  # noqa: E402

F32 = np.float32
FLT_MIN = np.float32(1.1754943508222875e-38)
FLT_MAX = np.float32(3.4028234663852886e+38)
TWO24 = 16777216.0
MAGIC = 0x3646524D  # "MRF6"
VERSION = 1
MAX_STREAM = 1 << 20
MAX_VERTS = 1 << 17   # (the census's own limit is 65,536: a larger draw is recorded, its expectation a fallback)
VARIANTS = (("c4.x=1", 1.0, None), ("c4.x=0.5", 0.5, None),
            ("c4.x=0.5+ulp", float(np.nextafter(np.float32(0.5), np.float32(1.0))), None),
            ("c4.x=2.5", 2.5, None), ("c4.x=1e30", 1e30, None), ("c4.x=-4", -4.0, None), ("c4.x=0", 0.0, None),
            ("c4.y=3", None, 3.0))
ST_EXACT, ST_UNSUPPORTED, ST_UNKNOWN = 0, 1, 2


class Quiet(Exception):
    pass


def be32s(b):
    return struct.unpack(">%dI" % (len(b) // 4), b[: len(b) // 4 * 4])


# ---- the program: the prologue, F1, the computed slots (an independent reading of the ucode) ----
def cf_walk(dw):
    """The instructions in the control-flow walk's order: [(kind 'fetch' | 'alu', d0, d1, d2, cf op, cf index)], and
    the control flow ops met (a jump / loop / call)."""
    n = len(dw)
    pairs = n // 3
    bound = pairs
    cfs = []
    i = 0
    while i < bound:
        d0, d1, d2 = dw[3 * i], dw[3 * i + 1], dw[3 * i + 2]
        for a, b in ((d0, d1 & 0xFFFF), (((d1 >> 16) | (d2 << 16)) & 0xFFFFFFFF, d2 >> 16)):
            op = (b >> 12) & 0xF
            cfs.append((a, b, op))
            if (1 <= op <= 6 or op in (13, 14)) and (a & 0xFFF) < bound:
                bound = a & 0xFFF
        i += 1
    out, ctl = [], []
    for ci, (a, b, op) in enumerate(cfs[: 2 * bound]):
        if op in (7, 8, 9, 10, 11):
            ctl.append((ci, op, len(out)))
            continue
        if not (1 <= op <= 6 or op in (13, 14)):
            continue
        address, count, seq = a & 0xFFF, (a >> 12) & 7, (a >> 16) & 0xFFF
        for k in range(count):
            at = address + k
            if at * 3 + 3 > n:
                return None, ctl
            kind = "fetch" if (seq >> (2 * k)) & 1 else "alu"
            out.append((kind, dw[at * 3], dw[at * 3 + 1], dw[at * 3 + 2], op, ci))
    return out, ctl


def vfetch_fields(d0, d1, d2):
    off = (d2 >> 8) & 0x7FFFFF
    if off & (1 << 22):
        off -= 1 << 23
    return dict(op=d0 & 0x1F, src=(d0 >> 5) & 0x3F, src_rel=(d0 >> 11) & 1, dst=(d0 >> 12) & 0x3F,
                dst_rel=(d0 >> 18) & 1, slot=((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 3), comp=(d0 >> 30) & 3,
                swz=d1 & 0xFFF, signed=(d1 >> 12) & 1, integer=(d1 >> 13) & 1, rounded=(d1 >> 15) & 1,
                fmt=(d1 >> 16) & 0x3F, exp=(d1 >> 24) & 0x3F, mini=(d1 >> 30) & 1, pred=(d1 >> 31) & 1,
                stride=d2 & 0xFF, off=off)


def alu_fields(d0, d1, d2):
    return dict(vdst=d0 & 0x3F, vrel=(d0 >> 6) & 1, absc=(d0 >> 7) & 1, sdst=(d0 >> 8) & 0x3F, srel=(d0 >> 14) & 1,
                exp=(d0 >> 15) & 1, vmask=(d0 >> 16) & 0xF, smask=(d0 >> 20) & 0xF, vsat=(d0 >> 24) & 1,
                ssat=(d0 >> 25) & 1, sop=(d0 >> 26) & 0x3F,
                s=[dict(swz=(d1 >> 16) & 0xFF, neg=(d1 >> 26) & 1, reg=(d2 >> 16) & 0xFF, temp=(d2 >> 31) & 1),
                   dict(swz=(d1 >> 8) & 0xFF, neg=(d1 >> 25) & 1, reg=(d2 >> 8) & 0xFF, temp=(d2 >> 30) & 1),
                   dict(swz=d1 & 0xFF, neg=(d1 >> 24) & 1, reg=d2 & 0xFF, temp=(d2 >> 29) & 1)],
                pred=(d1 >> 28) & 1, crel=(d1 >> 29) & 7, vop=(d2 >> 24) & 0x1F)


VOPS_OK = {0, 1, 2, 3, 8, 9, 10, 11}
SOPS_OK = {0, 1, 2, 3, 4, 5, 6, 11, 12, 13, 17, 18, 19, 23, 24, 25, 26, 42, 43, 44, 45, 46, 47, 50}


def program(ucode):
    """The computed-index program of a VS, or None (a direct program, no fetch, malformed): the prologue's ALU fields,
    F1's fields, every later full vertex fetch's slot with its index source - ('L1', reg, comp) / ('L2', element comp)
    / ('clob', ...) - and conversion; 'why' names what the census must refuse (or None)."""
    dw = be32s(ucode)
    ins, ctl = cf_walk(dw)
    if not ins:
        return None
    prol, f1, k = [], None, 0
    why = None
    for k, (kind, d0, d1, d2, op, ci) in enumerate(ins):
        if kind == "fetch":
            f1 = vfetch_fields(d0, d1, d2)
            break
        a = alu_fields(d0, d1, d2)
        prol.append(a)
        if op not in (1, 2) and why is None:
            why = "ctl"
    if f1 is None or f1["op"] != 0 or f1["mini"]:
        return None
    if any(c[2] <= k or c[1] != 11 for c in ctl) and why is None:
        why = "ctl"  # a jump before F1, or a loop / call / return anywhere (the census follows only forward jumps)
    # direct: F1 indexes r0.x and no prologue instruction wrote r0.x
    wrote_r0x = any((not a["exp"]) and ((a["vmask"] & 1 and a["vdst"] == 0) or (a["smask"] & 1 and a["sdst"] == 0))
                    for a in prol)
    if f1["src"] == 0 and f1["comp"] == 0 and not f1["src_rel"] and not wrote_r0x:
        return None
    if not prol:
        return None
    for a in prol:
        if why is None and (a["vrel"] or a["srel"] or a["crel"] or any(s["temp"] and s["reg"] & 0x40 for s in a["s"])):
            why = "rel"
        if why is None and ((a["vmask"] and a["vop"] not in VOPS_OK) or (not a["vmask"] and a["vop"] > 19 and
                                                                         a["vop"] != 28) or a["sop"] not in SOPS_OK):
            why = "op"
        if why is None and a["pred"]:
            why = "ctl"
    if len(prol) > 48 and why is None:
        why = "long"
    # the later fetches: each register component's source after F1
    src = {}  # (reg, comp) -> ('F1', element comp) | 'clob'
    for c in range(4):
        sel = (f1["swz"] >> (3 * c)) & 7
        if sel <= 3:
            src[(f1["dst"], c)] = ("F1", sel)
        elif sel in (4, 5):
            src[(f1["dst"], c)] = "clob"
    slots = collections.OrderedDict()
    slots[f1["slot"]] = dict(cls="L1", reg=f1["src"], comp=f1["comp"], rounded={f1["rounded"]}, stride=f1["stride"])
    for kind, d0, d1, d2, op, ci in ins[k + 1:]:
        if kind == "alu":
            a = alu_fields(d0, d1, d2)
            if a["exp"]:
                continue
            for c in range(4):
                if (a["vmask"] >> c) & 1:
                    src[(a["vdst"], c)] = "clob"
                if (a["smask"] >> c) & 1:
                    src[(a["sdst"], c)] = "clob"
            continue
        f = vfetch_fields(d0, d1, d2)
        if f["op"] == 0 and not f["mini"]:
            s = src.get((f["src"], f["comp"]))
            if f["src_rel"] or s == "clob":
                ent = dict(cls="clob")
            elif s is None:
                ent = dict(cls="L1", reg=f["src"], comp=f["comp"])
            else:
                ent = dict(cls="L2", elem=s[1])
            ent.update(rounded={f["rounded"]}, stride=f["stride"])
            old = slots.get(f["slot"])
            if old is None:
                slots[f["slot"]] = ent
            else:
                same = old["cls"] == ent["cls"] and old.get("reg") == ent.get("reg") and \
                    old.get("comp") == ent.get("comp") and old.get("elem") == ent.get("elem") and \
                    old["stride"] == ent["stride"]
                if not same:
                    old["cls"] = "clob"
                old["rounded"] |= ent["rounded"]
        for c in range(4):  # any fetch's destination is unknown from here
            if f["dst_rel"]:
                src = collections.defaultdict(lambda: "clob")
                break
            if ((f["swz"] >> (3 * c)) & 7) != 7:
                src[(f["dst"], c)] = "clob"
    return dict(prol=prol, f1=f1, slots=slots, why=why)


def prologue_consts(prog):
    """The constant registers the prologue's operands name (a *sc op's src3 register is a constant)."""
    regs = set()
    for a in prog["prol"]:
        for j, s in enumerate(a["s"]):
            if not s["temp"] or (j == 2 and 42 <= a["sop"] <= 47):
                regs.add(s["reg"] & 0xFF)
    return sorted(regs)


# ---- the exact evaluation (numpy float32, per vertex) ----
def ff(u):
    return np.frombuffer(struct.pack("<I", u & 0xFFFFFFFF), dtype=np.float32)[0]


class Unsupported(Exception):
    pass


def evaluate(prog, consts, lit, named, verts):
    """The prologue over the vertices (uint32 array): the registers after it ((64, 4, n) float32)."""
    nv = len(verts)
    R = np.zeros((64, 4, nv), dtype=np.float32)
    R[0, 0] = verts.astype(np.float32)
    ps = np.zeros(nv, dtype=np.float32)

    def cval(reg, comp):
        reg &= 0xFF
        if reg in lit and reg not in named:
            return ff(lit[reg][comp])
        return ff(consts[reg * 4 + comp])

    def operand(s, comp, absc):
        if s["temp"]:
            v = R[s["reg"] & 0x3F, comp].copy()
            if s["reg"] & 0x80:
                v = np.abs(v)
        else:
            v = np.full(nv, cval(s["reg"], comp), dtype=np.float32)
            if absc:
                v = np.abs(v)
        return -v if s["neg"] else v

    with np.errstate(all="ignore"):
        for a in prog["prol"]:
            if a["vmask"] and not a["exp"]:
                res = {}
                for i in range(4):
                    if not (a["vmask"] >> i) & 1:
                        continue
                    x, y, z = (operand(a["s"][j], ((a["s"][j]["swz"] >> (2 * i)) + i) & 3, a["absc"]) for j in range(3))
                    vop = a["vop"]
                    if vop == 0:
                        r = x + y
                    elif vop == 1:
                        r = x * y
                    elif vop == 2:
                        r = np.fmax(x, y)
                    elif vop == 3:
                        r = np.fmin(x, y)
                    elif vop == 8:
                        r = x - np.floor(x)
                    elif vop == 9:
                        r = np.trunc(x)
                    elif vop == 10:
                        r = np.floor(x)
                    elif vop == 11:
                        t = (x * y).astype(np.float32)
                        r = t + z
                    else:
                        raise Unsupported("vector op %d" % vop)
                    r = r.astype(np.float32)
                    if a["vsat"]:
                        r = np.clip(r, F32(0), F32(1))
                    res[i] = r
                for i, r in res.items():
                    R[a["vdst"], i] = r
            sop = a["sop"]
            if sop != 50:
                s3 = a["s"][2]
                c0, c1 = ((s3["swz"] >> 6) + 3) & 3, s3["swz"] & 3
                if 42 <= sop <= 47:
                    k = np.full(nv, cval(s3["reg"], c0), dtype=np.float32)
                    t = R[((sop & 1) | (s3["temp"] << 1) | (s3["swz"] & 0x3C)) & 0x3F, c1].copy()
                    if a["absc"]:
                        k, t = np.abs(k), np.abs(t)
                    if s3["neg"]:
                        k, t = -k, -t
                    p = k * t if sop <= 43 else (k + t if sop <= 45 else k - t)
                else:
                    s0, s1 = operand(s3, c0, a["absc"]), operand(s3, c1, a["absc"])
                    if sop == 0:
                        p = s0 + s1
                    elif sop == 1:
                        p = s0 + ps
                    elif sop == 2:
                        p = s0 * s1
                    elif sop in (3, 4):
                        p = s0 * ps
                    elif sop in (5, 23, 24):
                        p = np.fmax(s0, s1)
                    elif sop == 6:
                        p = np.fmin(s0, s1)
                    elif sop == 11:
                        p = s0 - np.floor(s0)
                    elif sop == 12:
                        p = np.trunc(s0)
                    elif sop == 13:
                        p = np.floor(s0)
                    elif sop in (17, 18, 19):
                        p = np.fmin(np.fmax((F32(1) / s0).astype(np.float32), FLT_MIN), FLT_MAX)
                    elif sop == 25:
                        p = s0 - s1
                    elif sop == 26:
                        p = s0 - ps
                    else:
                        raise Unsupported("scalar op %d" % sop)
                p = p.astype(np.float32)
                if a["ssat"]:
                    p = np.clip(p, F32(0), F32(1))
                ps = p
            if a["smask"] and not a["exp"]:
                for i in range(4):
                    if (a["smask"] >> i) & 1:
                        R[a["sdst"], i] = ps
    return R


def convert(x, rounded):
    """XeVfAddr's index (int64 array) and the vertices whose index cannot be formed."""
    with np.errstate(all="ignore"):
        t = (x + F32(0.5)).astype(np.float32) if rounded else x
        bad = ~(np.abs(t) <= TWO24)
        idx = np.floor(np.where(bad, F32(0), t)).astype(np.int64)
    return idx, bad


def stream_values(stream, base, size, endian, signed, idx, stride, off, comp):
    """F1's element component at each index (float32), and the indices outside its declared range."""
    o = (idx * stride + off + comp) * 4
    bad = (o < 0) | (o + 4 > size) | (o + 4 > len(stream))
    oo = np.where(bad, 0, o)
    b = np.frombuffer(stream, dtype=np.uint8)
    w = (b[oo].astype(np.uint32) | (b[oo + 1].astype(np.uint32) << 8) | (b[oo + 2].astype(np.uint32) << 16) |
         (b[oo + 3].astype(np.uint32) << 24)) if len(b) >= 4 else np.zeros(len(idx), dtype=np.uint32)
    if endian in (1, 2):
        w = ((w & 0x00FF00FF) << 8) | ((w >> 8) & 0x00FF00FF)
    if endian in (2, 3):
        w = ((w << 16) | (w >> 16)) & 0xFFFFFFFF
    w = w.astype(np.uint32)
    v = w.view(np.int32).astype(np.float32) if signed else w.astype(np.float32)
    return v, bad


def vertex_range(draw, ib):
    """pm4StageVsf's vertex indices (gfx/resource_census.cpp vertexIndexRange, re-derived): the uint32 array."""
    r = draw["regs"]
    n = draw["count"]
    offset, minv, maxv = r[0x2102], r[0x2101], r[0x2100]
    clamp_on = maxv >= minv
    strip_cut = draw["prim"] == 6 and bool((r[0x2205] >> 21) & 1)
    reset = r[0x2103]
    endian = (draw["dma_size"] >> 30) & 3
    if draw["src_sel"] != 0:
        raw = np.arange(n, dtype=np.uint64)
    elif draw["idx_type"]:
        a = np.frombuffer(ib[: n * 4], dtype="<u4").astype(np.uint64)
        if endian == 2:
            a = ((a >> 24) | ((a >> 8) & 0xFF00) | ((a << 8) & 0xFF0000) | ((a << 24) & 0xFF000000))
        elif endian == 3:
            a = ((a >> 16) | (a << 16)) & 0xFFFFFFFF
        elif endian == 1:
            a = (((a & 0x00FF00FF) << 8) | ((a >> 8) & 0x00FF00FF)) & 0xFFFFFFFF
        raw = a
    else:
        a = np.frombuffer(ib[: n * 2], dtype="<u2").astype(np.uint64)
        if endian in (1, 2):
            a = ((a >> 8) | (a << 8)) & 0xFFFF
        raw = a
    if strip_cut and draw["src_sel"] == 0:
        raw = raw[raw != reset]
    v = (raw + offset) & 0xFFFFFF
    if clamp_on:
        v = np.minimum(np.maximum(v, minv), maxv)
    return np.unique(v).astype(np.uint32)


def expect(prog, consts, lit, named, verts, stream, fconst):
    """Per computed slot: (status, lo, hi) of its exact index range over the draw's vertices."""
    out = collections.OrderedDict()
    if prog["why"] is not None:
        for slot in prog["slots"]:
            out[slot] = (ST_UNSUPPORTED, 0, 0)
        return out
    if len(verts) == 0:
        return out
    try:
        R = evaluate(prog, consts, lit, named, verts)
    except Unsupported:
        for slot in prog["slots"]:
            out[slot] = (ST_UNSUPPORTED, 0, 0)
        return out
    f1 = prog["f1"]
    d0, d1 = fconst[f1["slot"] * 2], fconst[f1["slot"] * 2 + 1]
    size = ((d1 >> 2) & 0xFFFFFF) * 4
    i1, bad1 = convert(R[f1["src"], f1["comp"]], f1["rounded"])
    f1_ok = f1["fmt"] in (33, 34, 35) and f1["integer"] and f1["exp"] == 0
    elem = {}
    for slot, s in prog["slots"].items():
        if s["cls"] == "clob" or len(s["rounded"]) == 0:
            out[slot] = (ST_UNSUPPORTED, 0, 0)
            continue
        lo, hi, unknown = None, None, False
        for rnd in sorted(s["rounded"]):
            if s["cls"] == "L1":
                x = R[s["reg"], s["comp"]]
            else:
                if not f1_ok or bad1.any():
                    unknown = True
                    break
                if s["elem"] not in elem:
                    elem[s["elem"]] = stream_values(stream, d0 & 0x1FFFFFFC, size, d1 & 3, f1["signed"], i1,
                                                    f1["stride"], f1["off"], s["elem"])
                x, badr = elem[s["elem"]]
                if badr.any():
                    unknown = True
                    break
            idx, bad = convert(x, rnd)
            if bad.any():
                unknown = True
                break
            lo = int(idx.min()) if lo is None else min(lo, int(idx.min()))
            hi = int(idx.max()) if hi is None else max(hi, int(idx.max()))
        out[slot] = (ST_UNKNOWN, 0, 0) if unknown else (ST_EXACT, lo, hi)
    return out


# ---- the traces ----
class FamEngine(X.Engine):
    """The trace engine, capturing each computed-index draw's state at the draw (the memory as it stands then)."""
    sentinel = None
    progs = {}

    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.fam = []
        self.n_draws = 0

    def draw(self, op, init, dma, offset):
        super().draw(op, init, dma, offset)
        d = self.draws[-1]
        self.n_draws += 1
        if self.sentinel and self.n_draws % 2000 == 0 and os.path.exists(self.sentinel):
            raise Quiet()
        keep = None
        if d["kind"] == "draw" and d["vs"] is not None:
            h = d["vs"]
            if h not in FamEngine.progs:
                FamEngine.progs[h] = (program(self.vs), bytes(self.vs))
            prog = FamEngine.progs[h][0]
            if prog is not None:
                keep = self.capture(d, prog)
        for k in ("vs_consts", "ps_consts", "fetch", "bools", "loops"):  # (the engine keeps every draw: drop the bulk)
            d[k] = None
        if keep:
            self.fam.append(keep)

    def capture(self, d, prog):
        ib = b""
        if d["src_sel"] == 0 and d["count"]:
            ib, missing = self.mem.read(d["dma_base"], d["count"] * (4 if d["idx_type"] else 2))
            if missing:
                return None
        f = [int(x) for x in d["fetch"]]
        s0, s1 = f[prog["f1"]["slot"] * 2], f[prog["f1"]["slot"] * 2 + 1]
        base, size = s0 & 0x1FFFFFFC, min(((s1 >> 2) & 0xFFFFFF) * 4, MAX_STREAM)
        stream, missing = self.mem.read(base, size) if size else (b"", 0)
        if missing:
            stream = b""  # (the trace never supplied it: the census reads both windows - unreadable here too)
        return dict(vs=d["vs"], consts=[int(x) for x in d["vs_consts"]], fetch=f, ib=bytes(ib),
                    count=d["count"], src_sel=d["src_sel"], idx_type=d["idx_type"], dma_size=d["dma_size"],
                    prim=d["prim"], regs={k: d["regs"][k] for k in (0x2100, 0x2101, 0x2102, 0x2103, 0x2205)},
                    stream_base=base, stream=bytes(stream))


def put_u32(b, v):
    b += struct.pack("<I", v & 0xFFFFFFFF)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("traces", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--json")
    ap.add_argument("--sentinel")
    a = ap.parse_args(argv)
    by_hash, digest, files = LIT.scan()
    lits, named, known = {}, {}, set()
    for h, recs in by_hash.items():
        known.add(h)
        r = recs[0]
        lits[h] = r["lits"]
        named[h] = r["named"]
    FamEngine.sentinel = a.sentinel
    X.Engine = FamEngine  # (xtr_draws.parse builds its engine by this name)
    draws, per_trace = [], collections.OrderedDict()
    for p in a.traces:
        try:
            eng, hdr, stats = X.parse(p)
        except Quiet:
            print("QUIET: the sentinel appeared during %s - stopped, nothing written" % p)
            return 3
        per_trace[p] = dict(draws=eng.n_draws, family=len(eng.fam))
        print("%s: %d draws, %d of a computed-index VS" % (p, eng.n_draws, len(eng.fam)))
        for d in eng.fam:
            d["trace"] = os.path.basename(p)
        draws += eng.fam
    # distinct draws (a draw's state as the census reads it), then each VS's variants
    seen, uniq = {}, []
    for d in draws:
        key = hashlib.sha256(repr((d["vs"], d["consts"], d["fetch"], d["ib"], d["count"], d["src_sel"], d["idx_type"],
                                   d["dma_size"], d["prim"], sorted(d["regs"].items()), d["stream_base"],
                                   d["stream"])).encode()).hexdigest()
        if key in seen:
            seen[key]["dup"] += 1
            continue
        d["dup"] = 1
        d["variant"] = "trace"
        seen[key] = d
        uniq.append(d)
    first = collections.OrderedDict()
    for d in uniq:
        first.setdefault(d["vs"], d)
    for h, d in first.items():
        for name, x, y in VARIANTS:
            v = dict(d)
            v["consts"] = list(d["consts"])
            if x is not None:
                v["consts"][16] = struct.unpack("<I", struct.pack("<f", x))[0]
            if y is not None:
                v["consts"][17] = struct.unpack("<I", struct.pack("<f", y))[0]
            v["variant"] = name
            v["dup"] = 1
            uniq.append(v)
    # the expectations
    blob = bytearray()
    put_u32(blob, MAGIC)
    put_u32(blob, VERSION)
    vs_list = sorted({d["vs"] for d in uniq})
    put_u32(blob, len(vs_list))
    for h in vs_list:
        u = FamEngine.progs[h][1]
        blob += struct.pack("<QI", h, len(u)) + u
    put_u32(blob, len(uniq))
    summary = collections.Counter()
    rows = []
    for d in uniq:
        prog = FamEngine.progs[d["vs"]][0]
        verts = vertex_range(d, d["ib"])
        lit = {int(k): v for k, v in lits.get(d["vs"], {}).items()}
        nm = set(named.get(d["vs"], set()))
        if len(verts) > MAX_VERTS:
            ex = collections.OrderedDict((s, (ST_UNKNOWN, 0, 0)) for s in prog["slots"])
        else:
            ex = expect(prog, d["consts"], lit, nm, verts, d["stream"], d["fetch"])
        flags = (1 if d["src_sel"] == 0 else 0) | (2 if d["idx_type"] else 0) | \
                (4 if d["prim"] == 6 and (d["regs"][0x2205] >> 21) & 1 else 0) | \
                (8 if d["regs"][0x2100] >= d["regs"][0x2101] else 0) | (16 if d["vs"] in known else 0)
        blob += struct.pack("<QIIIIIIII", d["vs"], d["dup"], flags, (d["dma_size"] >> 30) & 3, d["regs"][0x2102],
                            d["regs"][0x2101], d["regs"][0x2100], d["regs"][0x2103], d["count"])
        put_u32(blob, len(d["ib"]))
        blob += d["ib"]
        blob += struct.pack("<1024I", *d["consts"])
        blob += struct.pack("<192I", *d["fetch"])
        put_u32(blob, d["stream_base"])
        put_u32(blob, len(d["stream"]))
        blob += d["stream"]
        vmin = int(verts.min()) if len(verts) else 0xFFFFFFFF
        vmax = int(verts.max()) if len(verts) else 0
        put_u32(blob, vmin)
        put_u32(blob, vmax)
        tag = ("%s %s" % (d["trace"], d["variant"])).encode()[:63]
        put_u32(blob, len(tag))
        blob += tag
        put_u32(blob, len(ex))
        for slot, (st, lo, hi) in ex.items():
            blob += struct.pack("<IIqq", slot, st, lo, hi)
            summary["%s.%s" % (d["variant"] if d["variant"] == "trace" else "variant", ("exact", "unsupported",
                                                                                         "unknown")[st])] += 1
        # the literals the prologue reads against the live file at the traced draw (information: the host - and the
        # census - read the literal; a difference is where a census on the live file would have been wrong)
        if d["variant"] == "trace":
            for reg in prologue_consts(prog):
                if reg in lit and reg not in nm:
                    same = tuple(d["consts"][reg * 4: reg * 4 + 4]) == tuple(lit[reg])
                    summary["literal.%s" % ("same_as_live" if same else "differs_from_live")] += 1
        rows.append(dict(trace=d["trace"], variant=d["variant"], vs="%016X" % d["vs"], dup=d["dup"],
                         vrange=[vmin, vmax], verts=len(verts), slots={str(s): list(v) for s, v in ex.items()},
                         c4=[float(ff(d["consts"][16])), float(ff(d["consts"][17]))],
                         literals={str(k): v for k, v in lit.items() if k not in nm},
                         live255=float(ff(d["consts"][255 * 4])), known=d["vs"] in known))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "wb") as fh:
        fh.write(bytes(blob))
    print("fixture %s: %d VS, %d draws (%d distinct from the traces, %d variants), %d bytes" % (
        a.out, len(vs_list), len(uniq), sum(1 for d in uniq if d["variant"] == "trace"),
        sum(1 for d in uniq if d["variant"] != "trace"), len(blob)))
    print("expectations: %s" % ", ".join("%s=%d" % kv for kv in sorted(summary.items())))
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(dict(traces=per_trace, corpus=dict(files=files, digest=digest), summary=summary,
                           draws=rows), fh, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
