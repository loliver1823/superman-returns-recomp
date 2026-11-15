"""step 6 G6a (the DIAG, SR_ORACLE): source mutants, each killed by ITS named fixture mode.

For each mutant: apply it to its file (exactly one match, else unusable), build difftest, and run the mutant's mode
(tools/difftest/diag_g6a_run.py's run_mode). KILLED = that mode FAILS (its own assertion); anything else (a crash, a
hang, a pass) is no kill. Each file is restored byte for byte after each mutant, and the unmutated fixture must pass at
the end. Runs in the worker tree (an oracle build):

    python tools/difftest/diag_g6a_mutants.py --build-dir build [--only D1,D4]

Exit 0 = every mutant killed and the final run PASS; 1 = a survivor or the final run failed; 2 = unusable.
"""
import argparse
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import diag_g6a_run  # noqa: E402

STUBS = pathlib.Path("kernel/stubs.cpp")
GLUE = pathlib.Path("guest/ppc_import_glue.cpp")
# name: (the file, the source text, its mutant, the fixture mode that must catch it)
MUTANTS = {
    # [DIAG_WAKE]: a stamp before the wait counted as its wake
    "D1": (STUBS, "  if (intoWait > span) return -1;\n", "", "wake-ticks"),
    # [DIAG_WAIT]: the record's tid not cut to 29 bits (it spills into the class)
    "D2": (STUBS, "uint64_t(cls & 7u) << 29 | (tid & 0x1FFFFFFFu);", "uint64_t(cls & 7u) << 29 | tid;", "sig-record"),
    # [DIAG_WAKE]: the bucket's us + 1 in 32 bits (the top value wraps to bucket 0)
    "D3": (STUBS, "  const uint64_t v = uint64_t(us) + 1u;", "  const uint32_t v = us + 1u;", "wake-bucket"),
    # [APC_STUCK]: a queue stuck at the first census that sees it
    "D4": (GLUE, "      if (held && s.censusNonEmpty && s.delivered == s.censusDelivered) {",
           "      if (held && s.delivered == s.censusDelivered) {", "apc-census"),
    # [APC_STUCK]: deliveries not counted (a queue refilled between censuses reads as stuck)
    "D5": (GLUE, "    g_apcStats[threadHandle].delivered += pending.size();\n", "", "apc-census"),
}


def run(cmd):
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd).returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    exe = str(pathlib.Path(a.build_dir) / "difftest.exe")
    originals = {p: p.read_bytes() for p in (STUBS, GLUE)}

    def text(p, t):
        return (t.replace("\n", "\r\n") if b"\r\n" in originals[p] else t).encode()

    only = [m for m in a.only.split(",") if m]
    build = ["cmake", "--build", a.build_dir, "-j", "12", "--target", "difftest"]
    chosen = [(n, m) for n, m in MUTANTS.items() if not only or n in only]
    for name, (path, old, new, mode) in chosen:
        if originals[path].count(text(path, old)) != 1 or mode not in diag_g6a_run.MODES:
            print(f"UNUSABLE {name}: the target text occurs {originals[path].count(text(path, old))} times in {path} "
                  f"/ mode {mode}")
            return 2
    survivors, unusable = [], []
    try:
        for name, (path, old, new, mode) in chosen:
            path.write_bytes(originals[path].replace(text(path, old), text(path, new)))
            try:
                if run(build):
                    print(f"UNUSABLE {name}: the mutant does not build")
                    return 2
                status, detail = diag_g6a_run.run_mode(exe, mode)
            finally:
                path.write_bytes(originals[path])
            verdict = {"fail": "killed", "ok": "SURVIVED", "infra": "UNUSABLE"}[status]
            print(f"{verdict} {name} by {mode}: {detail}", flush=True)
            if status == "ok":
                survivors.append(name)
            elif status == "infra":
                unusable.append(name)
    finally:
        for p, b in originals.items():
            p.write_bytes(b)
    if run(build) or run([sys.executable, "tools/difftest/diag_g6a_run.py", exe]):
        print("diag_g6a_mutants: FAIL (the unmutated fixture does not pass)")
        return 1
    if unusable:
        print(f"diag_g6a_mutants: UNUSABLE {unusable} (no verdict) survivors={survivors}")
        return 2
    print(f"diag_g6a_mutants: {'PASS' if not survivors else 'FAIL'} survivors={survivors}")
    return 0 if not survivors else 1


if __name__ == "__main__":
    sys.exit(main())
