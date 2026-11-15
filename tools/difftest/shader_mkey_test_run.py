"""Build and run tools/difftest/shader_mkey_test.cpp, then mutation-validate it (iter-463).

The test is standalone (it links the real gfx/shader_cache.cpp and app/log.cpp, no CMake target):
each mutant is a copy of gfx/shader_mkey.hpp + gfx/shader_cache.{hpp,cpp} with one deliberate
bug, compiled the same way; every mutant must make the test FAIL, the clean build must PASS.

    python tools/difftest/shader_mkey_test_run.py --fixture archive/v1_arena/stage/missing_vs/unit \
        --work <scratch dir>
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
CXX = r"C:\Program Files\LLVM\bin\clang++.exe"

# name -> (file, original text, mutated text, what the test must catch)
MUTANTS = {
    "M1-stride-not-masked": ("shader_mkey.hpp", "kVfDw2Mask = 0x7FFFFFFFu;", "kVfDw2Mask = 0x7FFFFF00u;",
                             "T1/T2: DBE06113 and FAF94BDB stop matching; C++ disagrees with Python"),
    "M2-alu-slots-as-vfetch": ("shader_mkey.hpp", "if (!(seq & 1u)) continue;", "(void)seq;",
                               "T3: 28A1A12B instruction 9 (ALU) gets masked as a fetch"),
    "M3-no-cf-bound-shrink": ("shader_mkey.hpp", "        bound = cf[j][0] & 0xFFFu;", "        (void)0;",
                              "T1: the walk reads instruction words as control flow"),
    "M4-mini-own-slot-stride": ("shader_mkey.hpp", "    if (r.mini) {", "    if (false) {",
                                "T4: mini fetches must inherit slot/stride from the previous full fetch"),
    "M5-diag-cached-stride": ("shader_cache.cpp", "if (r.strideDwords != c.strideDwords)",
                              "if (c.strideDwords != c.strideDwords)",
                              "T6: the diagnosis must read the stride from the runtime ucode"),
    "M6-no-mkey-index": ("shader_cache.cpp", "if (sh.mkey) vsByMkey_.emplace(sh.mkey, hash);", "(void)0;",
                         "T6: the .vs.mkey sidecar must feed the twin index"),
    # iter-463 review: the live path (what the exe runs) was asserted by check(true) only.
    "M7-first-sighting-silenced": ("shader_cache.cpp", "for (uint64_t h : miss_->unreported)",
                                   "for (uint64_t h : std::vector<uint64_t>{})",
                                   "T7: a dropped VS must be named by a [SHADER_MISS] line"),
    "M8-summary-per-drain": ("shader_cache.cpp",
                             "      miss_->pendingDraws = 0;\n      miss_->drain.clear();\n    }\n  }\n}",
                             "    }\n  }\n  miss_->pendingDraws = 0;\n  miss_->drain.clear();\n}",
                             "T7: the summary must count drops across drains, not the last drain"),
    "M9-first-ps-mis-keyed": ("shader_cache.cpp", "miss_->firstPs.emplace(vsHash, psHash)",
                              "miss_->firstPs.emplace(vsHash, vsHash)",
                              "T7: the first-sighting line must name the PS of the first dropped draw"),
    "M10-summary-unthrottled": ("shader_cache.cpp", "now - miss_->lastSummary >= std::chrono::seconds(2)",
                                "now - miss_->lastSummary >= std::chrono::seconds(0)",
                                "T7: at most one summary per 2 s"),
    "M11-exp-adjust-masked": ("shader_mkey.hpp", "kVfDw1Mask = 0x003F3000u;", "kVfDw1Mask = 0x3F3F7000u;",
                              "T5b: exp_adjust / signed_rf_mode are value transforms and stay in the key"),
}


def build(src_dir, out_exe):
    common = [CXX, "-std=c++20", "-O1", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN", "-D_CRT_SECURE_NO_WARNINGS",
              "-Wno-deprecated-declarations", "-I" + src_dir, "-I" + REPO, "-I" + os.path.join(REPO, "gfx"),
              "-I" + os.path.join(REPO, "app")]
    # shader_cache.cpp links two units this runner predates: ShaderCache::hashUcode's AVX2 XXH3 (its own -mavx2
    # unit, selected at run time) and NativeShaderCode (CachedShader's bytecode owners). Without them the link
    # failed (render/boot-attach, 2026-10-14).
    objs = []
    for f, extra in ((os.path.join(REPO, "gfx", "hash_avx2.cpp"), ["-mavx2"]),
                     (os.path.join(REPO, "gfx", "native_pipeline.cpp"), [])):
        o = out_exe + "." + os.path.basename(f) + ".o"
        r = subprocess.run(common + extra + ["-c", f, "-o", o], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout[-3000:], r.stderr[-3000:])
            return False
        objs.append(o)
    cmd = common + [os.path.join(REPO, "tools", "difftest", "shader_mkey_test.cpp"),
                    os.path.join(src_dir, "gfx", "shader_cache.cpp"), os.path.join(REPO, "app", "log.cpp")] + objs + \
        ["-o", out_exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-3000:], r.stderr[-3000:])
    return r.returncode == 0


def run(exe, fixture):
    r = subprocess.run([exe, fixture], capture_output=True, text=True)
    return r.returncode, r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixture", required=True)
    ap.add_argument("--work", required=True)
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    fixture = os.path.abspath(a.fixture)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)

    def tree(name, mutate=None):
        d = os.path.join(work, name)
        os.makedirs(os.path.join(d, "gfx"))
        for f in ("shader_mkey.hpp", "shader_cache.hpp", "shader_cache.cpp"):
            shutil.copy(os.path.join(REPO, "gfx", f), os.path.join(d, "gfx", f))
        if mutate:
            f, old, new, _why = mutate
            p = os.path.join(d, "gfx", f)
            s = open(p, encoding="utf-8").read()
            assert s.count(old) == 1, "mutation anchor not unique in %s: %r" % (f, old)
            open(p, "w", encoding="utf-8", newline="\n").write(s.replace(old, new))
        return d

    clean = tree("clean")
    exe = os.path.join(work, "clean.exe")
    if not build(clean, exe):
        print("BUILD FAILED (clean)")
        return 2
    rc, out = run(exe, fixture)
    print(out)
    ok = rc == 0 and "0 failed" in out
    print("CLEAN: %s (exit %d)" % ("PASS" if ok else "FAIL", rc))
    killed = 0
    for name, mut in MUTANTS.items():
        d = tree(name, mut)
        mexe = os.path.join(work, name + ".exe")
        if not build(d, mexe):
            print("MUTANT %s: BUILD FAILED (not counted)" % name)
            continue
        mrc, mout = run(mexe, fixture)
        fails = [l.strip() for l in mout.splitlines() if l.strip().startswith("FAIL")]
        caught = mrc != 0
        killed += caught
        print("MUTANT %-26s %s (exit %d) - expects %s" % (name, "KILLED" if caught else "SURVIVED", mrc, mut[3]))
        for l in fails[:4]:
            print("      " + l)
    print("mutants killed: %d/%d" % (killed, len(MUTANTS)))
    return 0 if ok and killed == len(MUTANTS) else 1


if __name__ == "__main__":
    sys.exit(main())
