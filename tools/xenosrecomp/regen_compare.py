"""Regenerate the cached shader corpus with two XenosRecomp builds and compare.

iter-464 resscale (tools/xenosrecomp/0003-iter464-resscale-pixel-position.patch): a
translator change must change exactly the programs it is about. This runs the BASE
translator (the dev clone) and the NEW one (this tree's clone) over the same container
corpus (default archive/v1_linkfix/stage_in, the corpus the committed cache was built
from), then reports, per output file:
  - identical in base and new,
  - differing (the change's footprint; listed),
  - base vs the committed data/shader_cache/dxil (should be identical: the corpus
    reproduces the cache).
With --stage DIR it copies every changed .dxil (and both HLSL texts) to DIR for review;
nothing is written to data/shader_cache/dxil.

    python tools/xenosrecomp/regen_compare.py --work archive/v1_resscale/regen \
        --base-exe <main checkout>/tools/XenosRecomp-clone/build/XenosRecomp/XenosRecomp.exe \
        --base-inc <main checkout>/tools/XenosRecomp-clone/XenosRecomp/shader_common.h
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def sha(p):
    with open(p, "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()


def run(exe, inc, corpus, out):
    os.makedirs(out, exist_ok=True)
    hashes = sorted({f[:16] for f in os.listdir(corpus)})
    env = dict(os.environ, XENOS_DUMP_HLSL=",".join(hashes))
    with open(os.path.join(out, "run.log"), "w") as log:
        subprocess.run([exe, corpus, os.path.join(out, "cache.cpp"), inc], cwd=out, env=env,
                       stdout=log, stderr=subprocess.STDOUT, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", required=True)
    ap.add_argument("--corpus", default=os.path.join(REPO, "archive", "v1_linkfix", "stage_in"))
    ap.add_argument("--base-exe", required=True)
    ap.add_argument("--base-inc", required=True)
    ap.add_argument("--new-exe", default=os.path.join(REPO, "tools", "XenosRecomp-clone", "build",
                                                      "XenosRecomp", "XenosRecomp.exe"))
    ap.add_argument("--new-inc", default=os.path.join(REPO, "tools", "XenosRecomp-clone", "XenosRecomp",
                                                      "shader_common.h"))
    ap.add_argument("--cache", default=os.path.join(REPO, "data", "shader_cache", "dxil"))
    ap.add_argument("--stage")
    ap.add_argument("--skip-run", action="store_true")
    args = ap.parse_args()
    work = os.path.abspath(args.work)
    base, new = os.path.join(work, "base"), os.path.join(work, "new")
    if not args.skip_run:
        run(args.base_exe, args.base_inc, args.corpus, base)
        run(args.new_exe, args.new_inc, args.corpus, new)
    bd, nd = os.path.join(base, "dxil"), os.path.join(new, "dxil")
    files = sorted(f for f in set(os.listdir(bd)) | set(os.listdir(nd)) if f.endswith(".dxil") or f.endswith(".vfetch"))
    same, diff, only = [], [], []
    vs_cache = {"identical": 0, "differ": [], "not_in_cache": 0}
    for f in files:
        pb, pn = os.path.join(bd, f), os.path.join(nd, f)
        if not (os.path.exists(pb) and os.path.exists(pn)):
            only.append(f)
            continue
        (same if sha(pb) == sha(pn) else diff).append(f)
        pc = os.path.join(args.cache, f)
        if not os.path.exists(pc):
            vs_cache["not_in_cache"] += 1
        elif sha(pc) == sha(pb):
            vs_cache["identical"] += 1
        else:
            vs_cache["differ"].append(f)
    rep = {"identical_base_vs_new": len(same), "changed": diff, "only_one_side": only,
           "base_vs_committed_cache": vs_cache}
    with open(os.path.join(work, "compare.json"), "w") as fp:
        json.dump(rep, fp, indent=2)
    print(f"identical base vs new: {len(same)}; changed: {len(diff)}; one side only: {len(only)}")
    print("changed:", " ".join(diff))
    print(f"base vs committed cache: identical {vs_cache['identical']}, differ {len(vs_cache['differ'])} "
          f"{' '.join(vs_cache['differ'][:20])}, not in cache {vs_cache['not_in_cache']}")
    if args.stage:
        st = os.path.abspath(args.stage)
        os.makedirs(st, exist_ok=True)
        for f in diff:
            shutil.copy2(os.path.join(nd, f), os.path.join(st, f))
            stem = f.rsplit(".dxil", 1)[0]
            for side, d in (("base", bd), ("new", nd)):
                h = os.path.join(d, stem + ".hlsl")
                if os.path.exists(h):
                    shutil.copy2(h, os.path.join(st, f"{stem}.{side}.hlsl"))
        print("staged", len(diff), "files in", st)


if __name__ == "__main__":
    main()
