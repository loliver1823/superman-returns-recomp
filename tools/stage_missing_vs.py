"""Stage translations for vertex shaders the runtime cache misses (iter-463).

Input: raw BIG-ENDIAN ucode files <hash>.vs.bin (SR_SHADER_DUMP_MISSES output, or
`xtr_draws.py --dump-ucode`), <hash> = XXH3-64 of the bytes (checked).

The translation is ALWAYS of the runtime ucode itself. XenosRecomp also needs the D3D container
metadata the raw ucode lacks (vertex-element usages keyed by fetch address, the constant table,
literal definitions, interpolator info). It is taken from a template container, and a template
is accepted only when this can be proved from the bytes:

  1. Same program. Template ucode and runtime ucode have the same size and are identical in every
     dword except, inside the vertex-fetch instructions the control-flow program executes, the
     fields the XDK rewrites at bind time for the vertex declaration: format, signedness,
     integer/normalized, exp_adjust, stride, offset, and also the prefetch count, the destination
     swizzle and the mini-fetch bit (an unpatched disc container has every fetch "full" with
     placeholder swizzles). Control flow, every ALU and texture-fetch instruction, and in every
     vfetch the registers, index rounding and predicate bits are compared exactly. The proof is
     printed per instruction slot (CF / ALU / TFETCH identical counts, every VFETCH field that
     differs).
     iter-467 (release/installer): the bind-time patch also moves a stream's fetches to that
     stream's vertex fetch constant (the compiler emits every fetch from constant 95; a second
     stream's full fetch runs from 94) and REORDERS the fetches of one contiguous fetch run (each
     stream's full fetch first, its mini fetches by offset) - measured on the 25 metropol.AST world
     shaders the installed build left black. So the fetch-constant slot is a patch field, and a
     runtime vfetch may pair with any unused template vfetch of its run whose unpatched bits are
     identical (the one at the same slot first). gfx/shader_xlat_core.cpp is the C++ port.
  2. One metadata. Every corpus container that passes (1) carries byte-identical metadata (all
     container bytes outside the ucode); otherwise the template is ambiguous and the shader is
     refused.
  3. Metadata covers the program. Every executed vfetch that writes a component has a vertex
     element at its instruction address (XenosRecomp asserts this; in a release build a miss
     would be undefined behaviour).

The runtime ucode is spliced into the template (XXH3 re-checked), translated by XenosRecomp
(.vs.dxil + .vs.vfetch + .vs.vsf.dxil) and checked after translation:

  4. The .vfetch sidecar equals the fetch list derived from the runtime ucode alone
     (slot, offset, stride, format, signed, integer per written fetch; mini fetches inherit the
     slot and stride of the previous full fetch) - the same derivation as gfx/shader_mkey.hpp.
  5. The .vs.vsf.dxil (the PM4 draw path) encodes the runtime layout: its index multipliers are
     exactly stride*4 of the full fetches, its fetch-constant cbuffer rows are slot>>1, and every
     vertex-memory load offset is one of the runtime fetches' words (dxc -dumpbin).
  6. Against the translation of a reference program with the same metadata (a cached sibling
     when one exists, else the template container), the .vs.dxil input signature is identical and
     every differing HLSL line is a vertex-fetch line.

Outputs in --out: containers/, dxil/ (<hash>.vs.dxil/.vfetch/.vsf.dxil/.mkey), hlsl/, ref/,
manifest.json, manifest.txt. It never writes data/shader_cache/dxil: installing is a copy of
dxil/* by hand once the checks read OK.

    python tools/stage_missing_vs.py --inputs archive/v1_arena/shaders_bindings/misses \
        --out archive/v1_arena/stage/missing_vs
    # exact re-run of a staging: --corpus-file <that stage dir>/corpus.txt (the resolved dir list
    # with container counts; also in manifest.json "corpus")
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys

import xxhash

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import shader_mkey as sm  # noqa: E402
from xtr_draws import cf_exec_walk, decode_vfetch  # noqa: E402

XR = os.path.join(REPO, "tools", "XenosRecomp-clone")
DEF_EXE = os.path.join(XR, "build", "XenosRecomp", "XenosRecomp.exe")
DEF_INC = os.path.join(XR, "XenosRecomp", "shader_common.h")
DEF_DXC = os.path.join(XR, "thirdparty", "dxc-bin", "bin", "x64", "dxc.exe")
DEF_CACHE = os.path.join(REPO, "data", "shader_cache", "dxil")
DEF_CORPUS = [os.path.join(REPO, "data", "shaders_carved"), os.path.join(REPO, "data", "shaders_menu")]

# Bind-time patch fields (see module doc). dword0: prefetch_count; dword1: dst_swiz, signedness,
# integer, signed_rf, format, exp_adjust, mini; dword2: stride + offset.
PATCH_DW0 = 0x38000000 | 0x07F00000  # prefetch | const_index_sel | const_index (iter-467)
PATCH_DW1 = 0x7F3F7FFF & ~0x00008000  # everything above except is_index_rounded (bit 15)
PATCH_DW2 = 0x7FFFFFFF
# is_predicated (dw1 bit 31) and pred_condition (dw2 bit 31) stay exact.
assert PATCH_DW1 & 0x80000000 == 0 and PATCH_DW1 & 0x8000 == 0


def be32s(b):
    return sm.be32s(b)


def rel(p):
    try:
        return os.path.relpath(p, REPO).replace("\\", "/")
    except ValueError:  # another drive
        return os.path.abspath(p).replace("\\", "/")


def container_parts(blob):
    virt = struct.unpack_from(">I", blob, 4)[0]
    sh_o = struct.unpack_from(">I", blob, 24)[0]
    po, sz = struct.unpack_from(">II", blob, sh_o)
    return virt, sh_o, po, sz


def meta_bytes(blob):
    virt, _sh, po, sz = container_parts(blob)
    return blob[:virt + po] + blob[virt + po + sz:]


def vertex_elements(blob):
    """VertexShader header (XenosRecomp shader.h): field18, vertexElementCount, field20, then
    vertexElementsAndInterpolators[field18 + i] = {address:12, usage:4, usageIndex:4} (LSB first)."""
    _virt, sh_o, _po, _sz = container_parts(blob)
    field18, count = struct.unpack_from(">II", blob, sh_o + 0x18)
    out = {}
    for i in range(count):
        v = struct.unpack_from(">I", blob, sh_o + 0x24 + 4 * (field18 + i))[0]
        out[v & 0xFFF] = ((v >> 12) & 0xF, (v >> 16) & 0xF)
    return out


def patch_masked(u):
    slots = sm.vfetch_slots(u)
    if slots is None:
        return None
    dw = be32s(u)
    for s in slots:
        dw[s * 3] &= ~PATCH_DW0 & 0xFFFFFFFF
        dw[s * 3 + 1] &= ~PATCH_DW1 & 0xFFFFFFFF
        dw[s * 3 + 2] &= ~PATCH_DW2 & 0xFFFFFFFF
    for run in vfetch_runs(slots):  # iter-467: a reorder inside a run is the same program
        ins = sorted(struct.pack(">3I", *dw[s * 3:s * 3 + 3]) for s in run)
        for s, b in zip(run, ins):
            dw[s * 3:s * 3 + 3] = list(struct.unpack(">3I", b))
    return struct.pack(">%dI" % len(dw), *dw)


def vfetch_runs(slots):
    """Executed vfetch slots grouped into runs of consecutive slot numbers (ascending)."""
    runs = []
    for v in sorted(set(slots)):
        if runs and runs[-1][-1] + 1 == v:
            runs[-1].append(v)
        else:
            runs.append([v])
    return runs


def vfetch_runs_by_clause(slots, walk):
    """vfetch_runs split at exec-clause boundaries (gfx/shader_xlat_core.cpp vfetchRunsByClause): a run across a
    clause would move a fetch into or out of a conditional / predicated clause; a slot two clauses run ends its run."""
    clauses = {}
    for ci, _op, slot, _f, _ser in walk["slots"]:
        clauses.setdefault(slot, set()).add(ci)
    out = []
    for run in vfetch_runs(slots):
        out.append([])
        for s in run:
            if out[-1]:
                c, p = clauses.get(s, set()), clauses.get(out[-1][-1], set())
                if len(c) != 1 or len(p) != 1 or c != p:
                    out.append([])
            out[-1].append(s)
    return out


def fetch_order_keeps_dependences(dw, run, frm):
    """gfx/shader_xlat_core.cpp fetchOrderKeepsDependences: the pairing (runtime fetch i <- template fetch frm[i] of
    the run) keeps the order of every two runtime fetches that depend on each other (src / dst register overlap,
    overlapping writes of one register by the runtime dst swizzles, relative addressing). Returns None or why not."""
    f = []
    for s in run:
        d0, d1 = dw[s * 3], dw[s * 3 + 1]
        v = decode_vfetch(d0, d1, dw[s * 3 + 2], s)
        comps = sum(1 << c for c in range(4) if ((v["dst_swiz"] >> (3 * c)) & 7) != 7)  # a source comp or a constant
        f.append((v["src"], (d0 >> 30) & 3, v["dst"], comps, bool((d0 >> 11) & 1 or (d0 >> 18) & 1)))
    for a in range(len(f)):
        for b in range(a + 1, len(f)):
            sa, xa, da, ca, ra = f[a]
            sb, xb, db, cb, rb = f[b]
            raw = da == sb and ca & (1 << xb)
            war = sa == db and cb & (1 << xa)
            waw = da == db and ca & cb
            dep = ra or rb or raw or war or waw
            if dep and frm[a] > frm[b]:
                return "the reorder swaps i%d and i%d, which depend on each other" % (run[a], run[b])
    return None


def writes_something(dst_swiz):
    return any(((dst_swiz >> (3 * i)) & 7) <= 3 for i in range(4))


def derive_fetches(u):
    """The .vfetch sidecar rows XenosRecomp writes, derived from the ucode alone:
    (slot, offset, stride, format, signed, integer) per fetch that writes a component."""
    dw = be32s(u)
    out, last_slot, last_stride = [], 0, 0
    for s in sm.vfetch_slots(u) or []:
        f = decode_vfetch(dw[s * 3], dw[s * 3 + 1], dw[s * 3 + 2], s)
        slot = last_slot if f["mini"] else f["fetch_const"]
        stride = last_stride if f["mini"] else f["stride"]
        if not f["mini"]:
            last_slot, last_stride = slot, stride
        if writes_something(f["dst_swiz"]):
            out.append((slot, f["offset"], stride, f["fmt"], f["signed"], f["num_format"]))
    return out


def read_sidecar(path):
    rows = []
    for line in open(path):
        p = line.split()
        if len(p) >= 8:
            rows.append((int(p[0]), int(p[1]), int(p[2]), int(p[3]), int(p[6]), int(p[7])))
    return rows


def prove_same_program(t, u):
    """Instruction-by-instruction comparison of template ucode t and runtime ucode u."""
    walk = cf_exec_walk(u)
    tw = cf_exec_walk(t)
    tdw, udw = be32s(t), be32s(u)
    rep = dict(ok=True, cf_dwords=0, alu=0, tfetch=0, vfetch=0, vfetch_diffs=[], problems=[])
    if walk["malformed"] or tw["malformed"] or walk["slots"] != tw["slots"]:
        rep["ok"] = False
        rep["problems"].append("control-flow walk differs or is malformed")
        return rep
    bound = walk["bound"]
    for i in range(bound * 3):
        if tdw[i] != udw[i]:
            rep["ok"] = False
            rep["problems"].append("CF dword %d differs" % i)
    rep["cf_dwords"] = bound * 3
    covered = set(range(bound * 3))
    seen = set()
    vslots = []
    for _ci, _op, slot, is_fetch, _ser in walk["slots"]:
        if slot in seen:
            continue
        seen.add(slot)
        a, b = tdw[slot * 3:slot * 3 + 3], udw[slot * 3:slot * 3 + 3]
        covered.update(range(slot * 3, slot * 3 + 3))
        if is_fetch and (udw[slot * 3] & 0x1F) == 0:
            rep["vfetch"] += 1
            vslots.append(slot)
        else:
            if a != b:
                rep["ok"] = False
                rep["problems"].append("%s instruction i%d differs" % ("TFETCH" if is_fetch else "ALU", slot))
            if is_fetch:
                rep["tfetch"] += 1
            else:
                rep["alu"] += 1

    # iter-467: each runtime vfetch pairs with an unused template vfetch of its contiguous run whose
    # bits outside the patch fields are identical (the same slot first, so an unreordered program
    # pairs in place).
    def masked(dw, s):
        return (dw[s * 3] & ~PATCH_DW0 & 0xFFFFFFFF, dw[s * 3 + 1] & ~PATCH_DW1 & 0xFFFFFFFF,
                dw[s * 3 + 2] & ~PATCH_DW2 & 0xFFFFFFFF)
    # render/shader-coverage: runs end at exec-clause boundaries; a run whose fetches depend on each other pairs
    # in place only (vfetch_runs_by_clause / fetch_reorder_safe, the C++ port's rule).
    for run in vfetch_runs_by_clause(vslots, walk):
        used = [False] * len(run)
        frm = [None] * len(run)
        for i, s in enumerate(run):
            mu = masked(udw, s)
            pick = i if (not used[i] and masked(tdw, s) == mu) else None
            if pick is None:
                pick = next((k for k in range(len(run)) if not used[k] and masked(tdw, run[k]) == mu), None)
            if pick is None:
                rep["ok"] = False
                rep["problems"].append("vfetch i%d unpatched bits differ (no unused fetch of its run i%d..i%d matches)"
                                       % (s, run[0], run[-1]))
                continue
            used[pick] = True
            frm[i] = pick
            ts = run[pick]
            ft = decode_vfetch(*tdw[ts * 3:ts * 3 + 3], ts)
            fu = decode_vfetch(*udw[s * 3:s * 3 + 3], s)
            d = {k: (ft[k], fu[k]) for k in ("fetch_const", "prefetch", "dst_swiz", "signed", "num_format",
                                              "signed_rf", "fmt", "exp_adjust", "mini", "stride", "offset")
                 if ft[k] != fu[k]}
            if ts != s:
                d["from"] = ts
            if d:
                rep["vfetch_diffs"].append(dict(instr=s, fields=d))
        if None not in frm:
            why = fetch_order_keeps_dependences(udw, run, frm)
            if why:
                rep["ok"] = False
                rep["problems"].append("vfetch run i%d..i%d: %s" % (run[0], run[-1], why))
    rest = [i for i in range(len(udw)) if i not in covered and tdw[i] != udw[i]]
    if rest:
        rep["ok"] = False
        rep["problems"].append("non-executed dwords differ: %s" % rest[:8])
    return rep


def dxc_dump(dxc, path):
    return subprocess.run([dxc, "-dumpbin", path], capture_output=True, text=True).stdout


def fmt_words(fmt):
    return 2 if fmt in (26, 32, 34, 37) else 3 if fmt == 57 else 4 if fmt in (35, 38) else 1


def analyse_vsf(txt):
    """Index multipliers, fetch-constant cbuffer rows and vertex-memory load offsets of a
    <hash>.vs.vsf.dxil disassembly (archive/v1_hr1d/shader_xlat/check_vsf.py, iter-462)."""
    defs = {m.group(1): m.group(2) for m in re.finditer(r"^\s*(%\d+) = (.*)$", txt, re.M)}
    muls = set()
    for m in re.finditer(r"= (mul|shl) i32 (%\d+), (-?\d+)", txt):
        if defs.get(m.group(2), "").startswith("fptosi"):
            k = int(m.group(3))
            muls.add(k if m.group(1) == "mul" else (1 << k))
    loads = re.findall(r"^\s*(%\d+) = call %dx\.types\.CBufRet\.i32 @dx\.op\.cbufferLoadLegacy\.i32"
                       r"\(i32 59, %dx\.types\.Handle (%\d+), i32 (\d+)\)", txt, re.M)
    fetch_handles = set()
    for v, hnd, _reg in loads:
        for w in re.findall(r"^\s*(%\d+) = extractvalue %dx\.types\.CBufRet\.i32 " + re.escape(v) + r", \d+",
                            txt, re.M):
            if re.search(r"= and i32 " + re.escape(w) + r", -4\b", txt):
                fetch_handles.add(hnd)
    regs = sorted({int(reg) for _v, hnd, reg in loads if hnd in fetch_handles})
    # iter-467: the constant byte offset summed through the address expression - every "add i32 A, B"
    # (value or constant operands) and "and i32 A, -4" when the offset is a multiple of 4 (dxc may
    # reassociate (base + 36) & ~3 + index * 40). gfx/shader_xlat_core.cpp analyseVsf is the port.
    def const_off(v, depth=0):
        if depth > 16:
            return 0
        mm = re.match(r"(add|and)(?: nuw| nsw)* i32 (%\d+|-?\d+), (%\d+|-?\d+)$", defs.get(v, ""))
        if not mm:
            return 0
        val = [const_off(x, depth + 1) if x.startswith("%") else int(x) for x in (mm.group(2), mm.group(3))]
        if mm.group(1) == "and":
            ok = mm.group(2).startswith("%") and mm.group(3) == "-4" and val[0] % 4 == 0
            return val[0] if ok else 0
        return val[0] + val[1]
    offs = set()
    for m in re.finditer(r"@dx\.op\.bufferLoad\.i32\(i32 68, %dx\.types\.Handle %\d+, i32 (%\d+)", txt):
        offs.add(const_off(m.group(1)))
    return sorted(muls), regs, sorted(offs)


def input_signature(txt):
    """ISG1 rows of a dxc -dumpbin as (declared, used): declared = Name Index Mask Register SysValue
    Format (what the D3D12 input layout must match), used = the components the code reads (a fetch
    whose destination swizzle fills .w with 1.0 reads .xyz only - informational)."""
    m = re.search(r"; Input signature:\n;\n; Name.*\n; -.*\n((?:; .*\n)+?);\n", txt)
    if not m:
        return [], []
    rows = [l[1:].split() for l in m.group(1).splitlines() if l.strip("; ")]
    return [" ".join(r[:6]) for r in rows], [" ".join(r[:2] + r[6:]) for r in rows]


VF_LINE = re.compile(r"(XeVfAddr|XeVfLoad|= i(Position|BlendWeight|BlendIndices|Normal|PointSize|TexCoord|Tangent|"
                     r"Binormal|TessFactor|PositionT|Color|Fog|Depth|Sample)\d+\.|^\s*r\d+\.[xyzw] = [01]\.0;$|vfAddr)")


def run_xr(exe, inc, indir, outdir, hashes):
    os.makedirs(outdir, exist_ok=True)
    env = dict(os.environ, XENOS_DUMP_HLSL=",".join(hashes))
    env.pop("XENOS_VSF_ONLY", None)
    env.pop("XENOS_PS_ONLY", None)
    with open(os.path.join(outdir, "run.log"), "w") as log:
        subprocess.run([exe, indir, os.path.join(outdir, "cache.cpp"), inc], stdout=log, stderr=subprocess.STDOUT,
                       env=env, cwd=outdir)
    return os.path.join(outdir, "dxil")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--inputs", nargs="+", required=True, help="dirs of <hash>.vs.bin raw BE ucode")
    ap.add_argument("--corpus", nargs="*", default=None, help="container dirs (default data/shaders_*)")
    ap.add_argument("--extra", nargs="*", default=[], help="more container dirs (globs allowed)")
    ap.add_argument("--corpus-file",
                    help="text file of container dirs, one per line (repo-relative; a previous run's "
                         "corpus.txt '<count> <dir>' lines work as is); replaces --corpus and --extra")
    ap.add_argument("--out", required=True)
    ap.add_argument("--exe", default=DEF_EXE)
    ap.add_argument("--include", default=DEF_INC)
    ap.add_argument("--dxc", default=DEF_DXC)
    ap.add_argument("--cache", default=DEF_CACHE, help="installed cache (read only: siblings, control)")
    ap.add_argument("--control", nargs="*", default=[],
                    help="cached VS hashes to re-translate as a translator-identity control")
    ap.add_argument("--reason", default="VS missing from the shader cache: every draw using it is dropped "
                                        "(d3d12_backend gate 1, [XLAT_REJECT] gate=1)",
                    help="why these files are new (recorded per shader in the manifest)")
    a = ap.parse_args(argv)
    a.exe, a.include, a.dxc, a.cache = (os.path.abspath(x) for x in (a.exe, a.include, a.dxc, a.cache))

    if a.corpus_file:
        dirs = []
        for ln in open(a.corpus_file):
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            f = ln.split(None, 1)
            d = f[1] if len(f) == 2 and f[0].isdigit() else ln
            dirs.append(d if os.path.isabs(d) else os.path.join(REPO, d))
    else:
        dirs = list(a.corpus if a.corpus is not None else DEF_CORPUS)
        for g in a.extra:
            dirs += [d for d in sorted(glob.glob(g)) if os.path.isdir(d)]
    by_size = {}
    corpus = []  # iter-463 review: the resolved dir list is part of the repro record
    for d in dirs:
        n = 0
        for p, u in sm.iter_vs_containers([d]):
            by_size.setdefault(len(u), []).append((p, u))
            n += 1
        corpus.append((rel(d), n))
    print("corpus: %d VS containers in %d dirs" % (sum(len(v) for v in by_size.values()), len(dirs)))
    for d, n in corpus:
        print("   %5d %s" % (n, d))

    out = os.path.abspath(a.out)
    cdir, hdir, rdir = (os.path.join(out, x) for x in ("containers", "hlsl", "ref"))
    for d in (cdir, rdir):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    os.makedirs(hdir, exist_ok=True)
    refin = os.path.join(rdir, "in")
    os.makedirs(refin)
    with open(os.path.join(out, "corpus.txt"), "w", newline="\n") as f:  # feeds --corpus-file
        f.write("# stage_missing_vs.py corpus: %d VS containers in %d dirs (<count> <dir>)\n"
                % (sum(n for _, n in corpus), len(corpus)))
        f.writelines("%d %s\n" % (n, d) for d, n in corpus)

    man, todo = {}, []
    for d in a.inputs:
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".vs.bin"):
                continue
            h = fn.split(".")[0].lower()
            u = open(os.path.join(d, fn), "rb").read()
            e = man.setdefault(h, dict(hash=h, input=os.path.relpath(os.path.join(d, fn), REPO).replace("\\", "/")))
            if "status" in e:
                continue
            hh = "%016x" % xxhash.xxh3_64_intdigest(u)
            if hh != h:
                e.update(status="REFUSED", why="input hash %s != name" % hh)
                continue
            key = patch_masked(u)
            if key is None:
                e.update(status="REFUSED", why="malformed control flow")
                continue
            groups = {}
            for p, cu in by_size.get(len(u), []):
                if patch_masked(cu) != key:
                    continue
                blob = open(p, "rb").read()
                groups.setdefault(hashlib.sha1(meta_bytes(blob)).hexdigest(), []).append((p, cu, blob))
            if not groups:
                e.update(status="REFUSED", why="no container runs the same program (no template)")
                continue
            if len(groups) > 1:
                e.update(status="REFUSED", why="%d different metadata among same-program containers" % len(groups))
                continue
            (mh, members), = groups.items()
            # template: a member whose ucode is in the installed cache (a proven translation), else the
            # first by repo-relative path (iter-463 review: the raw path mixed absolute default dirs
            # with relative --extra ones, so the pick depended on how a dir was spelled); any member
            # serves since the metadata is byte-identical across the group.
            members.sort(key=lambda m: (not os.path.exists(os.path.join(
                a.cache, "%016x.vs.dxil" % xxhash.xxh3_64_intdigest(m[1]))), rel(m[0])))
            tp, tu, tblob = members[0]
            proof = prove_same_program(tu, u)
            if not proof["ok"]:
                e.update(status="REFUSED", why="same-program proof failed: %s" % proof["problems"][:4], proof=proof)
                continue
            elems = vertex_elements(tblob)
            dw = be32s(u)
            uncovered, usages = [], {}
            for s in sm.vfetch_slots(u):
                f = decode_vfetch(dw[s * 3], dw[s * 3 + 1], dw[s * 3 + 2], s)
                if writes_something(f["dst_swiz"]):
                    if s not in elems:
                        uncovered.append(s)
                    else:
                        usages[s] = elems[s]
            if uncovered:
                e.update(status="REFUSED", why="vfetch at %s has no vertex element in the template" % uncovered)
                continue
            virt, _sh, po, sz = container_parts(tblob)
            blob = bytearray(tblob)
            blob[virt + po:virt + po + sz] = u
            assert "%016x" % xxhash.xxh3_64_intdigest(bytes(blob[virt + po:virt + po + sz])) == h
            open(os.path.join(cdir, "vs_%s_masked.xpu" % h), "wb").write(blob)
            sibs = sorted({"%016x" % xxhash.xxh3_64_intdigest(m[1]) for m in members})
            cached_sibs = [s for s in sibs if os.path.exists(os.path.join(a.cache, "%s.vs.dxil" % s))]
            ref = cached_sibs[0] if cached_sibs else "%016x" % xxhash.xxh3_64_intdigest(tu)
            ref_src = [m for m in members if "%016x" % xxhash.xxh3_64_intdigest(m[1]) == ref][0]
            shutil.copy(ref_src[0], os.path.join(refin, "vs_%s_ref.xpu" % ref))
            e.update(status="STAGED", reason=a.reason, template=os.path.relpath(tp, REPO).replace("\\", "/"),
                     template_ucode="%016x" % xxhash.xxh3_64_intdigest(tu), metadata_sha1=mh,
                     same_program_containers=len(members), same_program_ucodes=sibs, cached_siblings=cached_sibs,
                     reference=ref, reference_is_cached=bool(cached_sibs),
                     strict_mkey="%016x" % sm.mkey(u), proof=proof,
                     usages={"i%d" % s: dict(usage=v[0], index=v[1]) for s, v in sorted(usages.items())})
            todo.append(h)
    ctl_in = os.path.join(out, "control", "in")
    shutil.rmtree(os.path.join(out, "control"), ignore_errors=True)
    os.makedirs(ctl_in)
    for ch in a.control:
        for p, cu in (x for v in by_size.values() for x in v):
            if "%016x" % xxhash.xxh3_64_intdigest(cu) == ch:
                shutil.copy(p, os.path.join(ctl_in, "vs_%s_ctl.xpu" % ch))
                break

    # translate: staged containers, reference programs, controls
    sd = run_xr(a.exe, a.include, cdir, os.path.join(out, "xr_run"), todo)
    rd = run_xr(a.exe, a.include, refin, os.path.join(rdir, "out"),
                [man[h]["reference"] for h in todo])
    ctl = {}
    if a.control:
        cd = run_xr(a.exe, a.include, ctl_in, os.path.join(out, "control", "out"), [])
        for ch in a.control:
            ctl[ch] = {}
            for ext in (".vs.dxil", ".vs.vfetch", ".vs.vsf.dxil"):
                p1, p2 = os.path.join(cd, ch + ext), os.path.join(a.cache, ch + ext)
                ctl[ch][ext] = (os.path.exists(p1) and os.path.exists(p2)
                                and open(p1, "rb").read() == open(p2, "rb").read())
    dd = os.path.join(out, "dxil")
    shutil.rmtree(dd, ignore_errors=True)
    os.makedirs(dd)
    for h in todo:
        e = man[h]
        u = open(os.path.join(REPO, e["input"]), "rb").read()
        files = {ext: os.path.join(sd, h + ext) for ext in (".vs.dxil", ".vs.vfetch", ".vs.vsf.dxil")}
        miss = [x for x, p in files.items() if not os.path.exists(p)]
        if miss:
            e.update(status="FAILED", why="XenosRecomp produced no %s (see xr_run/run.log)" % miss)
            continue
        for ext, p in files.items():
            shutil.copy(p, os.path.join(dd, h + ext))
        with open(os.path.join(dd, h + ".vs.mkey"), "w") as f:
            f.write("%016x\n" % sm.mkey(u))
        if os.path.exists(os.path.join(sd, h + ".vs.hlsl")):
            shutil.move(os.path.join(sd, h + ".vs.hlsl"), os.path.join(hdir, h + ".vs.hlsl"))
        checks = {}
        # 4. sidecar == runtime derivation
        side = read_sidecar(files[".vs.vfetch"])
        der = derive_fetches(u)
        checks["sidecar_eq_runtime_ucode"] = side == der
        # 5. vsf DXIL encodes the runtime layout
        muls, regs, offs = analyse_vsf(dxc_dump(a.dxc, files[".vs.vsf.dxil"]))
        dwu = be32s(u)
        vf = [decode_vfetch(dwu[s * 3], dwu[s * 3 + 1], dwu[s * 3 + 2], s) for s in sm.vfetch_slots(u)]
        exp_mul = {f["stride"] * 4 for f in vf if not f["mini"] and f["stride"]}
        exp_regs = {r[0] >> 1 for r in der}
        exp_offs, first = set(), set()
        for r in der:
            exp_offs |= {r[1] * 4 + 4 * k for k in range(fmt_words(r[3]))}
            first.add(r[1] * 4)
        checks["vsf_index_multipliers"] = dict(got=muls, expected=sorted(exp_mul),
                                               ok=set(muls) - {0} == exp_mul)
        checks["vsf_fetch_cb_rows"] = dict(got=regs, expected=sorted(exp_regs), ok=set(regs) == exp_regs)
        checks["vsf_load_offsets"] = dict(got=offs, allowed=sorted(exp_offs), first_words=sorted(first),
                                          ok=set(offs) <= exp_offs and first <= set(offs))
        # 6. against the reference translation
        ref = e["reference"]
        ref_dxil = os.path.join(rd, ref + ".vs.dxil")
        ref_hlsl = os.path.join(rd, ref + ".vs.hlsl")
        if os.path.exists(ref_dxil):
            s1, u1 = input_signature(dxc_dump(a.dxc, files[".vs.dxil"]))
            s2, u2 = input_signature(dxc_dump(a.dxc, ref_dxil))
            checks["input_signature_eq_reference"] = dict(
                ok=bool(s1) and s1 == s2, sig=s1,
                used_differs=["%s -> %s" % (x, y) for x, y in zip(u2, u1) if x != y])
            if e["reference_is_cached"]:
                checks["reference_retranslation_eq_cache"] = all(
                    open(os.path.join(rd, ref + ext), "rb").read() == open(os.path.join(a.cache, ref + ext), "rb").read()
                    for ext in (".vs.dxil", ".vs.vfetch", ".vs.vsf.dxil"))
        else:
            checks["input_signature_eq_reference"] = dict(ok=False, sig=[], why="reference did not translate")
        if os.path.exists(ref_hlsl) and os.path.exists(os.path.join(hdir, h + ".vs.hlsl")):
            la = open(ref_hlsl).read().splitlines()
            lb = open(os.path.join(hdir, h + ".vs.hlsl")).read().splitlines()
            import difflib
            diff = [l for l in difflib.unified_diff(la, lb, lineterm="", n=0)
                    if l[:1] in "+-" and not l.startswith(("+++", "---"))]
            bad = [l for l in diff if not VF_LINE.search(l[1:])]
            checks["hlsl_diff_vs_reference"] = dict(lines=len(diff), non_vfetch=bad[:10], ok=not bad)
        else:
            checks["hlsl_diff_vs_reference"] = dict(ok=False, why="no HLSL dump")
        e["checks"] = checks
        e["files"] = [h + x for x in (".vs.dxil", ".vs.vfetch", ".vs.vsf.dxil", ".vs.mkey")]
        ok = (checks["sidecar_eq_runtime_ucode"] and checks["vsf_index_multipliers"]["ok"]
              and checks["vsf_fetch_cb_rows"]["ok"] and checks["vsf_load_offsets"]["ok"]
              and checks["input_signature_eq_reference"]["ok"] and checks["hlsl_diff_vs_reference"]["ok"]
              and checks.get("reference_retranslation_eq_cache", True))
        e["status"] = "STAGED-OK" if ok else "STAGED-CHECK"
    res = dict(translator=dict(exe=rel(a.exe),
                               sha256=hashlib.sha256(open(a.exe, "rb").read()).hexdigest(),
                               include_sha256=hashlib.sha256(open(a.include, "rb").read()).hexdigest(),
                               control_retranslation_eq_cache=ctl),
               corpus=dict(containers=sum(n for _, n in corpus), dirs=[[d, n] for d, n in corpus]),
               shaders=man)
    json.dump(res, open(os.path.join(out, "manifest.json"), "w"), indent=1)
    lines = ["translator %s sha256 %s" % (res["translator"]["exe"], res["translator"]["sha256"])]
    for ch, v in ctl.items():
        lines.append("control %s: %s" % (ch, " ".join("%s=%s" % (k, "IDENT" if ok else "DIFF") for k, ok in v.items())))
    for h, e in sorted(man.items()):
        lines.append("%s %s" % (h, e["status"]) + ("  (%s)" % e["why"] if "why" in e else ""))
        if "template" in e:
            lines.append("   files %s.vs.{dxil,vfetch,vsf.dxil,mkey} - new: %s" % (h, e["reason"]))
            p = e["proof"]
            lines.append("   template %s (ucode %s; %d same-program containers, 1 metadata %s; ref %s%s)" % (
                e["template"], e["template_ucode"], e["same_program_containers"], e["metadata_sha1"][:12],
                e["reference"], " cached" if e["reference_is_cached"] else " (template itself)"))
            lines.append("   proof: CF %d dwords identical, %d ALU + %d TFETCH identical, %d VFETCH (%d differ in"
                         " patch fields only)" % (p["cf_dwords"], p["alu"], p["tfetch"], p["vfetch"],
                                                  len(p["vfetch_diffs"])))
            for vd in p["vfetch_diffs"]:
                # A reordered fetch carries "from": the template slot it was moved from (an int, not a pair).
                lines.append("      i%d: %s" % (vd["instr"], ", ".join(
                    ("from i%d" % v) if k == "from" else
                    "%s %s->%s" % (k, ("%03x" % v[0]) if k == "dst_swiz" else v[0],
                                   ("%03x" % v[1]) if k == "dst_swiz" else v[1])
                    for k, v in vd["fields"].items())))
        if "checks" in e:
            c = e["checks"]
            lines.append("   checks: sidecar==ucode %s | vsf mul %s exp %s %s | cb rows %s %s | load offs %s | "
                         "ISG1==ref %s%s | hlsl diff %d lines, non-vfetch %d %s%s" % (
                             c["sidecar_eq_runtime_ucode"], c["vsf_index_multipliers"]["got"],
                             c["vsf_index_multipliers"]["expected"], c["vsf_index_multipliers"]["ok"],
                             c["vsf_fetch_cb_rows"]["got"], c["vsf_fetch_cb_rows"]["ok"],
                             c["vsf_load_offsets"]["ok"], c["input_signature_eq_reference"]["ok"],
                             " (used: %s)" % "; ".join(c["input_signature_eq_reference"]["used_differs"])
                             if c["input_signature_eq_reference"].get("used_differs") else "",
                             c["hlsl_diff_vs_reference"].get("lines", -1),
                             len(c["hlsl_diff_vs_reference"].get("non_vfetch", [])),
                             c["hlsl_diff_vs_reference"]["ok"],
                             " | ref retranslation==cache %s" % c["reference_retranslation_eq_cache"]
                             if "reference_retranslation_eq_cache" in c else ""))
    open(os.path.join(out, "manifest.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))
    bad = [h for h, e in man.items() if e["status"] != "STAGED-OK"]
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
