"""iter-463 regression check for XenosRecomp predicate and texture-LOD semantics.

Translates three real Superman Returns pixel shaders plus one synthetic variant
with the given XenosRecomp.exe and checks the generated HLSL / DXIL against
Xenia's semantics (reference/xenia-canary/src/xenia/gpu/shader_interpreter.cc,
dxbc_shader_translator_fetch.cc):

  setp_inv   PS 084F6366 (Warworld ring towers): the counter is 0 when the
             predicate is taken - `ps = p0 ? 0.0 : (s == 0.0 ? 1.0 : s)` - and
             the lit/glow block it guards survives dxc: 20 sample ops (17 with
             upstream's `ps = s == 0.0 ? 1.0 : s`).
  setp_push  synthetic: 084F6366 with slot 89 (setp_eq_push r3.___w,
             c250.yyyy, r1.wwww) patched to `setp_eq_push r3.x___, c250.yyyy,
             r1.xyzw`, so .x != .w. The predicate must test .w, the result .x.
  lod-level  PS 3BDD2B7F (Superman's body): setTexLOD + tfetchCube
             UseComputedLOD=false UseRegisterLOD=true -> SampleLevel(texLod).
  lod-bias   PS 0F334426: tfetch2D UseComputedLOD=true UseRegisterLOD=true ->
             SampleBias(texLod).
  unsup-grad synthetic: 0F334426 with slot 25 (tfetch2D, computed LOD) given
             UseRegisterGradients=1. Register gradients are not translated,
             so the batch must print a named SKIP and write no DXIL (upstream
             silently emitted an implicit-derivative Sample).
  unsup-op   synthetic: 0F334426 with slot 25 turned into getCompTexLOD
             (opcode 17). Same: named SKIP, no DXIL, never a silent drop.
  getbcf     synthetic: 0F334426 with slot 25 turned into getBCF2D (opcode
             16). getBCF stores 0.0 to its result, as Xenia does (upstream
             dropped the write); the shader still compiles.

A census of the tracked shader corpora (data/shaders_carved, data/shaders_menu)
also counts the fetches the translator refuses (register gradients,
getCompTexLOD, getGradients, setGradientH/V); it is informational (no FAIL) -
a non-zero count names the shaders a later level will SKIP.

    python tools/xenosrecomp/check_translator.py [--exe PATH] [--keep]

Exit 0 = every check passed. Mutation-validated (iter-463): the pre-fix exe
fails all twelve checks.
"""
import argparse, os, re, shutil, struct, subprocess, sys, tempfile

try:
    import xxhash
except ImportError:
    sys.exit("check_translator: needs the python 'xxhash' module (pip install xxhash)")

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CLONE = os.path.join(REPO, "tools", "XenosRecomp-clone")
DEFAULT_EXE = os.path.join(CLONE, "build", "XenosRecomp", "XenosRecomp.exe")
INCLUDE = os.path.join(CLONE, "XenosRecomp", "shader_common.h")
DXC = os.path.join(CLONE, "thirdparty", "dxc-bin", "bin", "x64", "dxc.exe")

SETP_INV = os.path.join(REPO, "data", "shaders_carved", "ps_e30c0b2d4210b532_B7AA4A08.xpu")  # 084f6366540a148a
LOD_LEVEL = os.path.join(REPO, "data", "shaders_menu", "ps_7a28867a9957c91f_AD450180.xpu")  # 3bdd2b7f3280c939
LOD_BIAS = os.path.join(REPO, "data", "shaders_menu", "ps_a3120d316b9aed9d_B7AB9E48.xpu")   # 0f334426ac3e1a1c
PUSH_SLOT = 89  # setp_eq_push r3.___w, c250.yyyy, r1.wwww in 084f6366
SETP_EQ_PUSH = 20
UNSUP_SLOT = 25  # tfetch2D, UseComputedLOD=1, no register LOD, mip filter from the fetch constant, in 0f334426
CENSUS_DIRS = (os.path.join(REPO, "data", "shaders_carved"), os.path.join(REPO, "data", "shaders_menu"))
UNSUPPORTED_FETCH_OPS = {17: "getCompTexLOD", 18: "getGradients", 25: "setGradientH", 26: "setGradientV"}

sys.path.insert(0, os.path.join(REPO, "tools"))
import xtr_draws  # noqa: E402  (the Xenia-pinned CF walk + tfetch decoder)


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def ucode_span(c):
    """(offset, size) of the ucode inside a D3D9 Xbox 360 shader container."""
    virtual_size, shader_offset = be32(c, 4), be32(c, 24)
    return virtual_size + be32(c, shader_offset), be32(c, shader_offset + 4)


def ucode_hash(c):
    o, n = ucode_span(c)
    return "%016x" % xxhash.xxh3_64_intdigest(bytes(c[o:o + n]))


def patch_push(c):
    """setp_eq_push at PUSH_SLOT: src2 swizzle -> identity (r1.xyzw) and write
    mask .w -> .x, so neither the result nor the predicate component can be
    taken from the write mask by accident."""
    c = bytearray(c)
    o, _ = ucode_span(c)
    w = o + PUSH_SLOT * 12
    d0, d1, d2 = be32(c, w), be32(c, w + 4), be32(c, w + 8)
    if (d2 >> 24) & 0x1F != SETP_EQ_PUSH:
        sys.exit("check_translator: slot %d of 084f6366 is not setp_eq_push - input changed?" % PUSH_SLOT)
    if (d0 >> 16) & 0xF != 0b1000:
        sys.exit("check_translator: slot %d write mask is not .w - input changed?" % PUSH_SLOT)
    struct.pack_into(">I", c, w, (d0 & ~0xF0000) | (0b0001 << 16))
    struct.pack_into(">I", c, w + 4, d1 & ~0xFF00)
    return bytes(c)


def patch_fetch(c, mutate):
    """Apply mutate(d0, d2) -> (d0, d2) to the tfetch at UNSUP_SLOT of 0f334426."""
    c = bytearray(c)
    o, _ = ucode_span(c)
    w = o + UNSUP_SLOT * 12
    d0, d1, d2 = be32(c, w), be32(c, w + 4), be32(c, w + 8)
    if d0 & 0x1F != 1 or not (d1 >> 28) & 1 or (d1 >> 29) & 1 or d2 & 1:
        sys.exit("check_translator: slot %d of 0f334426 is not a computed-LOD tfetch - input changed?" % UNSUP_SLOT)
    d0, d2 = mutate(d0, d2)
    struct.pack_into(">I", c, w, d0)
    struct.pack_into(">I", c, w + 8, d2)
    return bytes(c)


def census():
    """Fetches the translator refuses, over the tracked shader corpora."""
    n, hits = 0, []
    for d in CENSUS_DIRS:
        for f in sorted(os.listdir(d)):
            if not f.endswith(".xpu"):
                continue
            c = open(os.path.join(d, f), "rb").read()
            try:
                o, sz = ucode_span(c)
                a = xtr_draws.analyze_texture_fetches(c[o:o + sz])
            except Exception:
                continue
            n += 1
            for ins in a["instructions"]:
                if ins["opcode"] in UNSUPPORTED_FETCH_OPS:
                    hits.append("%s:%s@%d" % (f, UNSUPPORTED_FETCH_OPS[ins["opcode"]], ins["slot"]))
                elif ins["opcode"] == 1 and ins["use_register_gradients"] and ins["use_computed_lod"]                         and ins["mip_filter"] != 2:
                    hits.append("%s:regGrad@%d" % (f, ins["slot"]))
    return n, hits


def dxil_ops(path):
    out = subprocess.run([DXC, "-dumpbin", path], capture_output=True, text=True).stdout
    return re.findall(r"call [^@]*@dx\.op\.(sample\w*)\.", out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--keep", action="store_true", help="keep the temp dir")
    a = ap.parse_args()
    a.exe = os.path.abspath(a.exe)

    tmp = tempfile.mkdtemp(prefix="xr_check_")
    inp = os.path.join(tmp, "in")
    os.makedirs(inp)
    cases = {}
    for name, src in (("setp_inv", SETP_INV), ("lod-level", LOD_LEVEL), ("lod-bias", LOD_BIAS)):
        data = open(src, "rb").read()
        shutil.copy(src, inp)
        cases[name] = ucode_hash(data)
    push = patch_push(open(SETP_INV, "rb").read())
    open(os.path.join(inp, "ps_synth_push.xpu"), "wb").write(push)
    cases["setp_push"] = ucode_hash(push)
    lod_bias = open(LOD_BIAS, "rb").read()
    for case, mutate in (("unsup-grad", lambda d0, d2: (d0, d2 | 1)),
                         ("unsup-op", lambda d0, d2: ((d0 & ~0x1F) | 17, d2)),
                         ("getbcf", lambda d0, d2: ((d0 & ~0x1F) | 16, d2))):
        synth = patch_fetch(lod_bias, mutate)
        open(os.path.join(inp, "ps_synth_%s.xpu" % case), "wb").write(synth)
        cases[case] = ucode_hash(synth)

    env = dict(os.environ, XENOS_DUMP_HLSL=",".join(cases.values()), XENOS_PS_ONLY="1")
    log = subprocess.run([a.exe, inp, os.path.join(tmp, "cache.cpp"), INCLUDE], cwd=tmp, env=env,
                         capture_output=True, text=True, errors="replace").stdout

    def hlsl(case):
        p = os.path.join(tmp, "dxil", cases[case] + ".ps.hlsl")
        return open(p).read() if os.path.exists(p) else ""

    def dxil(case):
        return os.path.join(tmp, "dxil", cases[case] + ".ps.dxil")

    fails = []

    def check(case, ok, what):
        print("%-4s %-10s %s" % ("ok" if ok else "FAIL", case, what))
        if not ok:
            fails.append(case)

    # setp_inv: every setp_inv result zeroes the counter when p0 is taken.
    h = hlsl("setp_inv")
    inv = re.findall(r"^\s*ps = (.*== 0\.0 \? 1\.0 : .*);$", h, re.M)
    check("setp_inv", bool(inv) and all(l.startswith("p0 ? 0.0 : (") for l in inv),
          "%d setp_inv results, all `p0 ? 0.0 : (s == 0.0 ? 1.0 : s)`" % len(inv))
    n = len(dxil_ops(dxil("setp_inv"))) if os.path.exists(dxil("setp_inv")) else -1
    check("setp_inv", n == 20, "084f6366 DXIL sample ops = %d (Xenia-live 20, upstream 17)" % n)

    # setp_push (synthetic): predicate from .w, result from .x.
    h = hlsl("setp_push")
    check("setp_push", re.search(r"p0 = \S+ == 0\.0 && r1\.w == 0\.0;", h) is not None,
          "predicate tests r1.w")
    check("setp_push", re.search(r"= \(\S+ == 0\.0 && r1\.x == 0\.0\) \? 0\.0 : \S+ \+ 1\.0;", h) is not None,
          "result tests r1.x")

    # lod-level: setTexLOD feeds an explicit-LOD cube fetch.
    h = hlsl("lod-level")
    check("lod-level", re.search(r"^\s*texLod = r\d+\.[xyzw];$", h, re.M) is not None, "setTexLOD -> texLod")
    check("lod-level", re.search(r"tfetchCubeLevel\([^;]*, texLod\)", h) is not None, "tfetchCubeLevel(..., texLod)")
    ops = dxil_ops(dxil("lod-level")) if os.path.exists(dxil("lod-level")) else []
    check("lod-level", "sampleLevel" in ops, "DXIL has sampleLevel")

    # lod-bias: register LOD biases a computed-LOD fetch.
    h = hlsl("lod-bias")
    check("lod-bias", re.search(r"tfetch2DBias\([^;]*, texLod\)", h) is not None, "tfetch2DBias(..., texLod)")
    ops = dxil_ops(dxil("lod-bias")) if os.path.exists(dxil("lod-bias")) else []
    check("lod-bias", "sampleBias" in ops, "DXIL has sampleBias")

    # unsupported fetches: a named SKIP and no DXIL, never a silent wrong result.
    for case, reason in (("unsup-grad", "UseRegisterGradients"), ("unsup-op", "unsupported texture fetch opcode 17")):
        named = re.search(r"^SKIP .*ps_synth_%s\.xpu .*: .*%s" % (case, re.escape(reason)), log, re.M) is not None
        check(case, named and not os.path.exists(dxil(case)), "named SKIP (%s), no DXIL" % reason)

    # getBCF: result written as 0.0 (Xenia), the shader still compiles.
    o, _ = ucode_span(lod_bias)
    bcf_dst = (be32(lod_bias, o + UNSUP_SLOT * 12) >> 12) & 0x3F
    h = hlsl("getbcf")
    check("getbcf", re.search(r"^\s*r%d\.[xyzw] = 0\.0;$" % bcf_dst, h, re.M) is not None
          and os.path.exists(dxil("getbcf")), "getBCF r%d = 0.0, DXIL written" % bcf_dst)

    n, hits = census()
    print("info census      %d tracked containers, %d refused fetches%s" % (n, len(hits), (": " + ", ".join(hits[:8])) if hits else ""))

    if a.keep:
        print("kept", tmp)
    else:
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failed" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
