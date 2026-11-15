#!/usr/bin/env python3
"""mem_res_literals.py - the literal constants the host translation bakes into each vertex shader's prologue
(mem.resources memres6, docs/RELEASE_COMPOSITION.md 9.5 item 1; gfx/resource_census.cpp progLiterals).

XenosRecomp (tools/xenosrecomp/superman.patch, ShaderRecompiler::recompile(AluInstruction)) reads a float constant
operand cN from the live constant file when the container's constant table NAMES register N (a Float4 entry covering
it), else from the container's DEFINITION TABLE when that defines N (a literal: `float4 cN = asfloat(...)`), else from
the live file (g_UnnamedConstants). The census bounds the world family's computed vertex index by evaluating each
VS's prologue - the ALU instructions before its first fetch - per vertex; it must read the literals the translation
baked in, whatever the guest's constant file holds. This tool reads the containers the shader cache is built from
(data/shaders_carved, data/shaders_menu: XenosRecomp.exe's corpus), and writes gfx/resource_census_literals.inc:

  {hash, reg, flags, {x, y, z, w}}   a literal register (unnamed, defined) a prologue's operands name; flags 1 = the
                                     corpus's containers of this ucode disagree (named in one, another value...): the
                                     census falls back (reason lit)
  {hash, 0xFFFFFFFF, 0, {0,...}}     the VS's container was read (its prologue names a constant): a computed program
                                     with no such row falls back (lit) - its literals are not known

hash = XXH3-64 of the ucode bytes (ShaderCache::hashUcode: the key IM_LOAD's noteVsUcode receives). Rows sorted by
(hash, reg). The prologue's registers here are a superset of the census's own (every ALU before the first fetch in the
control-flow walk's order: the census stops earlier on a jump or an unsupported instruction - then it falls back).

    python tools/perf/mem_res_literals.py --write          # regenerate the table
    python tools/perf/mem_res_literals.py --check          # exit 1 when the checked-in table is stale
    python tools/perf/mem_res_literals.py --json out.json  # the per-container reading (the family test's input)
"""
import argparse
import hashlib
import json
import os
import struct
import sys

try:
    import xxhash
except ImportError:  # pragma: no cover - the tool needs it
    xxhash = None

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CORPUS = [os.path.join(REPO, "data", "shaders_carved"), os.path.join(REPO, "data", "shaders_menu")]
OUT = os.path.join(REPO, "gfx", "resource_census_literals.inc")
KNOWN = 0xFFFFFFFF


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def be16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def parse(blob):
    """A container's VS ucode, its named Float4 registers and its float4 literals ({reg: (4 dwords)}); None = not a VS
    container this reading accepts (the runtime parser's envelope, gfx/shader_xlat_core.cpp parseContainer)."""
    if len(blob) < 0x24:
        return None
    flags = be32(blob, 0)
    if (flags & 0xFFFFFF00) != 0x102A1100 or not (flags & 1):
        return None
    virt, phys = be32(blob, 4), be32(blob, 8)
    if virt < 0x24 or virt >= 0x20000 or phys == 0 or phys >= 0x20000 or virt + phys > len(blob):
        return None
    ct_off, def_off, sh_off = be32(blob, 0x10), be32(blob, 0x14), be32(blob, 0x18)
    if sh_off == 0 or sh_off + 0x18 > virt:
        return None
    po, sz = be32(blob, sh_off), be32(blob, sh_off + 4)
    if sz == 0 or sz & 3 or po + sz > phys:
        return None
    ucode = bytes(blob[virt + po: virt + po + sz])
    named = set()
    if ct_off:
        ct = ct_off + 4
        n, ci = be32(blob, ct + 12), be32(blob, ct + 16)
        for i in range(n):
            e = ct + ci + i * 20
            regset, idx, cnt = be16(blob, e + 4), be16(blob, e + 6), be16(blob, e + 8)
            if regset == 2:  # RegisterSet::Float4 (shader_recompiler.cpp float4Constants)
                named.update(range(idx, idx + cnt))
    lits = {}
    if def_off:
        pos = def_off + 20
        while be32(blob, pos) != 0:
            reg, count, phys_off = be16(blob, pos), be16(blob, pos + 2), be32(blob, pos + 4)
            v = virt + phys_off
            for i in range((count + 3) // 4):
                lits[reg + i] = tuple(be32(blob, v + 16 * i + 4 * k) for k in range(4))
            pos += 8
    return ucode, named, lits


def prologue_regs(ucode):
    """The constant registers the ALU instructions before the first fetch name (the control-flow walk's order: CF pairs
    in order, each exec's instructions in order - gfx/resource_census.cpp analyzeProg). A *sc op's src3 register is a
    constant whatever its select bit."""
    dw = struct.unpack(">%dI" % (len(ucode) // 4), ucode[: len(ucode) // 4 * 4])
    n = len(dw)
    pairs = n // 3
    bound = pairs
    cfs = []
    i = 0
    while i < bound:
        d0, d1, d2 = dw[3 * i], dw[3 * i + 1], dw[3 * i + 2]
        for a, b in ((d0, d1 & 0xFFFF), (((d1 >> 16) | (d2 << 16)) & 0xFFFFFFFF, d2 >> 16)):
            op = (b >> 12) & 0xF
            cfs.append((a, op))
            if (1 <= op <= 6 or op in (13, 14)) and (a & 0xFFF) < bound:
                bound = a & 0xFFF
        i += 1
    regs = set()
    for a, op in cfs[: 2 * bound]:
        if not (1 <= op <= 6 or op in (13, 14)):
            continue
        address, count, seq = a & 0xFFF, (a >> 12) & 7, (a >> 16) & 0xFFF
        for k in range(count):
            at = address + k
            if at * 3 + 3 > n:
                return regs
            if (seq >> (2 * k)) & 1:  # the first fetch ends the prologue
                return regs
            d0, d2 = dw[at * 3], dw[at * 3 + 2]
            sop = (d0 >> 26) & 0x3F
            for reg, sel, is3 in ((d2 & 0xFF, (d2 >> 29) & 1, True), ((d2 >> 8) & 0xFF, (d2 >> 30) & 1, False),
                                  ((d2 >> 16) & 0xFF, (d2 >> 31) & 1, False)):
                if not sel or (is3 and 42 <= sop <= 47):
                    regs.add(reg)
    return regs


def scan(dirs=CORPUS):
    """{hash: [record per container]} for every VS container of the corpus, and the input digest."""
    if xxhash is None:
        sys.exit("mem_res_literals.py: the xxhash module is required (pip install xxhash)")
    by_hash = {}
    dig = hashlib.sha256()
    files = 0
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".xpu"):
                continue
            with open(os.path.join(d, fn), "rb") as f:
                blob = f.read()
            dig.update(fn.encode() + b"\0" + hashlib.sha256(blob).digest())
            files += 1
            p = parse(blob)
            if p is None:
                continue
            ucode, named, lits = p
            h = xxhash.xxh3_64_intdigest(ucode)
            by_hash.setdefault(h, []).append(dict(file=os.path.relpath(os.path.join(d, fn), REPO).replace("\\", "/"),
                                                  named=named, lits=lits, regs=prologue_regs(ucode)))
    return by_hash, dig.hexdigest(), files


def rows_of(by_hash):
    """The table's rows: per VS whose prologue names a constant, its literal registers (flags 1 when its containers
    disagree on the register: named in one, defined in one and not another, or two values) and its KNOWN row."""
    rows = []
    for h in sorted(by_hash):
        recs = by_hash[h]
        regs = set().union(*(r["regs"] for r in recs))
        if not regs:
            continue
        for reg in sorted(regs):
            views = set()
            for r in recs:
                views.add(None if reg in r["named"] or reg not in r["lits"] else r["lits"][reg])
            if views == {None}:
                continue  # read live in every container: no row
            if len(views) > 1:
                rows.append((h, reg, 1, (0, 0, 0, 0)))
            else:
                rows.append((h, reg, 0, views.pop()))
        rows.append((h, KNOWN, 0, (0, 0, 0, 0)))
    return rows


def render(rows, digest, files, nvs, nprol):
    out = ["// GENERATED by tools/perf/mem_res_literals.py - do not edit (python tools/perf/mem_res_literals.py "
           "--write).",
           "// The literal constants XenosRecomp bakes into each VS prologue (gfx/resource_census.cpp progLiterals): "
           "{ucode hash, register,",
           "// flags (1 = the corpus defines it two ways), dwords}; register 0xFFFFFFFF = the VS's container was read.",
           "// corpus: data/shaders_carved + data/shaders_menu, %d files, %d VS ucodes, %d with a prologue naming a "
           "constant; %d rows" % (files, nvs, nprol, len(rows)),
           "// inputs sha256 %s" % digest]
    for h, reg, fl, v in rows:
        out.append("    {0x%016Xull, 0x%Xu, %du, {0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu}}," % ((h, reg, fl) + tuple(v)))
    return "\n".join(out) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--write", action="store_true", help="write gfx/resource_census_literals.inc")
    ap.add_argument("--check", action="store_true", help="exit 1 when the checked-in table differs")
    ap.add_argument("--json", help="write the per-container reading (hash -> containers, named, literals, prologue)")
    a = ap.parse_args(argv)
    by_hash, digest, files = scan()
    rows = rows_of(by_hash)
    nprol = sum(1 for h in by_hash if set().union(*(r["regs"] for r in by_hash[h])))
    text = render(rows, digest, files, len(by_hash), nprol)
    print("mem_res_literals: %d files, %d VS ucodes, %d with prologue constants, %d rows (%d literal, %d disagreeing)"
          % (files, len(by_hash), nprol, len(rows), sum(1 for r in rows if r[1] != KNOWN and not r[2]),
             sum(1 for r in rows if r[2])))
    if a.json:
        js = {"%016X" % h: [dict(file=r["file"], named=sorted(r["named"]),
                                 lits={str(k): ["%08X" % x for x in v] for k, v in sorted(r["lits"].items())},
                                 prologue_regs=sorted(r["regs"])) for r in recs] for h, recs in sorted(by_hash.items())}
        with open(a.json, "w") as f:
            json.dump(js, f, indent=1, sort_keys=True)
    if a.write:
        with open(OUT, "w", newline="\n") as f:
            f.write(text)
        print("wrote", os.path.relpath(OUT, REPO))
    if a.check:
        try:
            cur = open(OUT, newline="").read().replace("\r\n", "\n")
        except OSError:
            cur = ""
        if cur != text:
            print("STALE: gfx/resource_census_literals.inc differs from the corpus's reading (run --write)")
            return 1
        print("CURRENT: gfx/resource_census_literals.inc matches the corpus")
    return 0


if __name__ == "__main__":
    sys.exit(main())
