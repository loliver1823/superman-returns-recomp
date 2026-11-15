"""Build the data for tools/difftest/shader_mkey_test.cpp (iter-463).

    python tools/difftest/shader_mkey_fixture.py --out archive/v1_arena/stage/missing_vs/unit \
        --misses archive/v1_arena/shaders_bindings/misses --staged archive/v1_arena/stage/missing_vs/dxil

Writes into --out:
  ucode/<hash>.vs.bin   raw BE ucode: every miss, and every installed-cache VS whose container is
                        in --containers (archive/v1_linkfix/stage_in = the inputs of the current cache)
  expect.txt            "<hash> <mkey|NOKEY>" per ucode, from tools/shader_mkey.py (the C++ port must agree)
  sidecars.txt          "<hash> <path of its .vs.vfetch>" for every ucode that has a translation
  cache/                a small ShaderCache dir copied from the installed cache, each with its .vs.mkey:
                        FAF94BDB (DBE06113's stride-8 twin), A214E11F (the unpatched program 28A1A12B and
                        09A04710 run) and 3A60C7C1 (container vs_3b76320fac13b7c5_synth.xpu, the template
                        the iter-462 dry run picked for 28A1A12B: its ALU instruction 9 differs)
"""
import argparse
import os
import shutil
import sys

import xxhash

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools"))
import shader_mkey as sm  # noqa: E402

CACHE = os.path.join(REPO, "data", "shader_cache", "dxil")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--misses", required=True)
    ap.add_argument("--staged", required=True)
    ap.add_argument("--containers", default=os.path.join(REPO, "archive", "v1_linkfix", "stage_in"))
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    shutil.rmtree(out, ignore_errors=True)
    ud = os.path.join(out, "ucode")
    os.makedirs(ud)
    ucodes = {}
    for fn in sorted(os.listdir(a.misses)):
        if fn.endswith(".vs.bin"):
            ucodes[fn.split(".")[0]] = open(os.path.join(a.misses, fn), "rb").read()
    for p, u in sm.iter_vs_containers([a.containers]):
        ucodes.setdefault("%016x" % xxhash.xxh3_64_intdigest(u), u)
    tmpl = sm.container_ucode(open(os.path.join(REPO, "data", "shaders_carved", "vs_3b76320fac13b7c5_synth.xpu"),
                                   "rb").read())
    assert "%016x" % xxhash.xxh3_64_intdigest(tmpl) == "3a60c7c1096a40c9"
    ucodes["3a60c7c1096a40c9"] = tmpl
    exp, side = [], []
    for h, u in sorted(ucodes.items()):
        assert "%016x" % xxhash.xxh3_64_intdigest(u) == h, h
        open(os.path.join(ud, h + ".vs.bin"), "wb").write(u)
        mk = sm.mkey(u)
        exp.append("%s %s" % (h, "%016x" % mk if mk is not None else "NOKEY"))
        for d in (a.staged, CACHE):
            p = os.path.join(d, h + ".vs.vfetch")
            if os.path.exists(p):
                side.append("%s %s" % (h, os.path.abspath(p)))
                break
    open(os.path.join(out, "expect.txt"), "w").write("\n".join(exp) + "\n")
    open(os.path.join(out, "sidecars.txt"), "w").write("\n".join(side) + "\n")
    cd = os.path.join(out, "cache")
    os.makedirs(cd)
    for h in ("faf94bdb30e37a25", "a214e11f328d46b0", "3a60c7c1096a40c9"):
        for ext in (".vs.dxil", ".vs.vfetch", ".vs.vsf.dxil"):
            shutil.copy(os.path.join(CACHE, h + ext), cd)
    for h in ("faf94bdb30e37a25", "a214e11f328d46b0", "3a60c7c1096a40c9"):
        with open(os.path.join(cd, h + ".vs.mkey"), "w") as f:
            f.write("%016x\n" % sm.mkey(ucodes[h]))
    print("fixture: %d ucodes, %d with a sidecar, cache of 3 VS -> %s" % (len(ucodes), len(side), out))


if __name__ == "__main__":
    main()
