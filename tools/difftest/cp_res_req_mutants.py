"""step 6 I4 (SR_CP_RES_REQ_LEAN): source mutants of the lean resource request, each killed by ITS named fixture check.

For each mutant: apply it to gfx/d3d12_pm4_shmem.cpp (exactly one match, else unusable), build difftest, and run
difftest --resource-memory-d3d12-test in the mutant's mode. KILLED = exit 1 with the named check's FAIL line; anything
else (a pass, a crash, a setup failure, another check failing alone) is no kill. The source is restored byte for byte
after each mutant, and the unmutated fixture must pass every run in FINAL at the end. Runs in the worker tree:

    python tools/difftest/cp_res_req_mutants.py --build-dir build [--only R1,R4]

Exit 0 = every mutant killed and FINAL passes; 1 = a survivor or FINAL failed; 2 = unusable.
"""
import argparse
import os
import pathlib
import subprocess
import sys

SRC = pathlib.Path("gfx/d3d12_pm4_shmem.cpp")
HIT = ("  if (memo.phys != phys || memo.len != len || memo.stamp != m_shmEpoch ||\n"
       "      !memory.unchangedSince(first, last, memo.sequence) || memory.patchPendingRange(first, last))")
# name: (the source text, its mutant, the fixture mode, the check that must FAIL)
MUTANTS = {
    # (a) the lock-free hit ignores a note in the range's blocks
    "R1": (HIT, HIT.replace("!memory.unchangedSince(first, last, memo.sequence) || ", ""),
           "--cp-res-req", "memo_never_hides_a_page_note"),
    # (a) the lock-free hit ignores a pending exact patch
    "R2": (HIT, HIT.replace(" || memory.patchPendingRange(first, last)", ""),
           "--cp-res-req", "memo_never_hides_a_pending_patch"),
    # (a) the lock-free hit survives a new drain epoch (the drain's stamp is never renewed)
    "R3": (HIT, HIT.replace(" memo.stamp != m_shmEpoch ||", ""),
           "--cp-res-req", "memo_restamps_a_new_drain_epoch_without_a_copy"),
    # (a) the hit waits for m_shmMu (the lock taken before the memo, as before I4)
    "R4": ("  if (lean) {\n    memres::resourceAbDecision(",
           "  std::lock_guard<std::recursive_mutex> early(m_shmMu);\n  if (lean) {\n    memres::resourceAbDecision(",
           "--cp-res-req", "i4_memo_hit_takes_no_lock"),
    # (b) a site's misses each take their own hold
    "R5": ("  if (!cpResReqLeanOn() || count > kSiteMax) {", "  if (true) {",
           "--cp-res-req", "i4_site_misses_share_one_hold"),
    # the CHECK oracle's verdict calls every false hit a race
    "R6": ("    if (!memory.unchangedSince(s.first, s.last, s.sequence)) {", "    if (true) {",
           "--cp-res-req-check", "i4_check_flags_a_false_memo_hit"),
    # the CHECK decides at once (Astra I4 r1 P2): a note between its two stores reads as BAD
    "R7": ("  m_cpResReqSuspects.push_back({first, last, page, sequence, m_cpResReq.drains});", "  ++m_cpResReq.bad;",
           "--cp-res-req-check", "i4_check_calls_a_late_note_raced"),
}
# the unmutated fixture: (mode, extra environment)
FINAL = [("--cp-res-req", {}), ("--cp-res-req-check", {}), ("--fast-path", {}),
         ("--fast-path", {"SR_CP_RES_REQ_LEAN": "0"}), ("", {})]


def fixture(exe, mode, env=None, timeout=900):
    """(exit code or None on a timeout, the combined output)."""
    cmd = [exe, "--resource-memory-d3d12-test"] + ([mode] if mode else [])
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env={**os.environ, **(env or {})})
    except subprocess.TimeoutExpired:
        return None, "timeout"
    return r.returncode, r.stdout + r.stderr


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
    crlf = b"\r\n" in original  # the checkout's line endings

    def text(t):
        return (t.replace("\n", "\r\n") if crlf else t).encode()

    only = [m for m in a.only.split(",") if m]
    build = ["cmake", "--build", a.build_dir, "-j", "12", "--target", "difftest"]
    chosen = [(n, m) for n, m in MUTANTS.items() if not only or n in only]
    for name, (old, new, _, _) in chosen:
        if original.count(text(old)) != 1 or old == new:
            print(f"UNUSABLE {name}: the target text occurs {original.count(text(old))} times")
            return 2
    survivors, unusable = [], []
    try:
        for name, (old, new, mode, check) in chosen:
            SRC.write_bytes(original.replace(text(old), text(new)))
            if run(build):
                print(f"UNUSABLE {name}: the mutant does not build")
                return 2
            code, out = fixture(exe, mode)
            named = f"[RESOURCE_D3D12_TEST] FAIL {check}" in out
            failing = [ln for ln in out.splitlines() if "[RESOURCE_D3D12_TEST] FAIL " in ln]
            if code == 1 and named:
                print(f"killed {name} by {mode} {check} (failing checks: {len(failing)})", flush=True)
            elif code in (0, 1):
                print(f"SURVIVED {name}: exit {code}, {check} {'FAILED' if named else 'passed'}; "
                      f"failing: {failing[:3]}", flush=True)
                survivors.append(name)
            else:
                print(f"UNUSABLE {name}: exit {code} {(out.strip().splitlines()[-1:] or [''])[0]}", flush=True)
                unusable.append(name)
    finally:
        SRC.write_bytes(original)
    if run(build):
        print("cp_res_req_mutants: FAIL (the unmutated fixture does not build)")
        return 1
    final_ok = True
    for mode, env in FINAL:
        code, out = fixture(exe, mode, env)
        complete = [ln for ln in out.splitlines() if "[RESOURCE_D3D12_TEST] COMPLETE" in ln]
        print(f"FINAL {mode or '(default)'} {env or ''}: exit {code} {complete[-1:] if complete else ''}", flush=True)
        final_ok &= code == 0
    if not final_ok:
        print("cp_res_req_mutants: FAIL (the unmutated fixture does not pass)")
        return 1
    if unusable:
        print(f"cp_res_req_mutants: UNUSABLE {unusable} (no verdict) survivors={survivors}")
        return 2
    print(f"cp_res_req_mutants: {'PASS' if not survivors else 'FAIL'} survivors={survivors}")
    return 0 if not survivors else 1


if __name__ == "__main__":
    sys.exit(main())
