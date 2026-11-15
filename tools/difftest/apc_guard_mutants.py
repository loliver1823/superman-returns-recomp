"""step 6 G14b (the APC spin guard): source mutants of guest/ppc_import_glue.cpp's ApcSpinGuard, each killed by ITS named
fixture mode.

For each mutant: apply it (exactly one match, else unusable), build difftest, and run the mutant's mode
(tools/difftest/apc_guard_run.py's run_mode). KILLED = that mode FAILS (its own assertion); anything else (a crash, a
hang, a pass) is no kill. The source is restored byte for byte after each mutant, and the unmutated fixture must pass at
the end. B1 is the old guard's break-out exactly, so its kill is the failing-first evidence. Runs in the worker tree:

    python tools/difftest/apc_guard_mutants.py --build-dir build [--only B1]

Exit 0 = every mutant killed and the final run PASS; 1 = a survivor or the final run failed; 2 = unusable.
"""
import argparse
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import apc_guard_run  # noqa: E402

SRC = pathlib.Path("guest/ppc_import_glue.cpp")
# name: (the source text, its mutant, the fixture mode that must catch it)
MUTANTS = {
    # the old guard: past its budget it breaks out and enters without the lock
    "B1": ("      SwitchToThread();\n    }", "      break;\n    }", "guard-waits"),
    # the guard does not wait at all (a single attempt)
    "B2": ("      if (++spins < kSpinBudget) continue;", "      break;", "guard-waits"),
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
    original = SRC.read_bytes()
    crlf = b"\r\n" in original

    def text(t):
        return (t.replace("\n", "\r\n") if crlf else t).encode()

    only = [m for m in a.only.split(",") if m]
    build = ["cmake", "--build", a.build_dir, "-j", "12", "--target", "difftest"]
    chosen = [(n, m) for n, m in MUTANTS.items() if not only or n in only]
    for name, (old, new, mode) in chosen:
        if original.count(text(old)) != 1 or mode not in apc_guard_run.MODES:
            print(f"UNUSABLE {name}: the target text occurs {original.count(text(old))} times / mode {mode}")
            return 2
    survivors, unusable = [], []
    try:
        for name, (old, new, mode) in chosen:
            SRC.write_bytes(original.replace(text(old), text(new)))
            if run(build):
                print(f"UNUSABLE {name}: the mutant does not build")
                return 2
            status, detail = apc_guard_run.run_mode(exe, mode)
            verdict = {"fail": "killed", "ok": "SURVIVED", "infra": "UNUSABLE"}[status]
            print(f"{verdict} {name} by {mode}: {detail}", flush=True)
            if status == "ok":
                survivors.append(name)
            elif status == "infra":
                unusable.append(name)
    finally:
        SRC.write_bytes(original)
    if run(build) or run([sys.executable, "tools/difftest/apc_guard_run.py", exe]):
        print("apc_guard_mutants: FAIL (the unmutated fixture does not pass)")
        return 1
    if unusable:
        print(f"apc_guard_mutants: UNUSABLE {unusable} (no verdict) survivors={survivors}")
        return 2
    print(f"apc_guard_mutants: {'PASS' if not survivors else 'FAIL'} survivors={survivors}")
    return 0 if not survivors else 1


if __name__ == "__main__":
    sys.exit(main())
