#!/usr/bin/env python3
"""
Regression smoke harness — docs/TOOLING_PLAN.md L2.

The north-star metric (present count) + crash/milestone checks, automated so a change can't
silently go backwards across the 60+ bandaids. Also the measurement rig for the determinism
work (L1): `--runs N` reports roll variance.

  python scripts/smoke_test.py                 # one run, default baseline
  python scripts/smoke_test.py --build         # build first
  python scripts/smoke_test.py --runs 5         # 5 rolls, report variance + pass rate
  python scripts/smoke_test.py --rtam           # SR_ENABLE_RTAM=1 path
  python scripts/smoke_test.py --timeout 60     # shorter run

Detection-only modes (NO build, NO game run -- score an existing log):
  python scripts/smoke_test.py --check-log path/to.log   # PASS/FAIL one log
  python scripts/smoke_test.py --self-test               # run built-in fixture matrix

Exit code 0 = all runs PASS, 1 = any FAIL (usable as a pre-commit / CI gate).
Baseline thresholds live in scripts/smoke_baseline.json (tunable as milestones land).

Crash detection is SINGLE-SOURCED with scripts/parse_crash_log.py via
scripts/crash_markers.py -- the two tools can never drift apart.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import crash_markers  # SINGLE SOURCE OF TRUTH for crash-marker regexes

ROOT = Path(__file__).resolve().parent.parent
LOG = ROOT / "superman_returns.log"
RUN_PS = ROOT / "tools" / "run-test-capture-log.ps1"
BASELINE_FILE = ROOT / "scripts" / "smoke_baseline.json"

DEFAULT_BASELINE = {
    "timeout_s": 90,
    "min_present": 600,
    "forbid_fatal_crash": True,
    "require_walk_end": False,
    "rtam": False,
    # Hang heuristic: if the last present number is below this AND the log shows
    # no fatal marker, the run is treated as a HANG/STALL rather than a clean
    # low-progress pass. A WALKER_WATCHDOG line is an unconditional hang signal.
    "hang_present_floor": 50,
}

# All progress/milestone regexes come from the shared module too.
PRESENT_RE = crash_markers.PRESENT_LOOSE
PRESENT_STRICT_RE = crash_markers.PRESENT
WALK_END_RE = crash_markers.WALK_END
CTOR_END_RE = crash_markers.CTOR_END
WALKER_WATCHDOG_RE = crash_markers.WALKER_WATCHDOG


def kill_stray():
    subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "Get-Process SupermanReturns -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue"],
        capture_output=True)


def build():
    print("[smoke] building ...", flush=True)
    r = subprocess.run(["cmake", "--build", "build"], cwd=str(ROOT), capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        raise SystemExit("[smoke] BUILD FAILED")
    print("[smoke] build ok", flush=True)


def run_once(timeout_s, rtam):
    kill_stray()
    try:
        LOG.unlink()
    except FileNotFoundError:
        pass
    env = dict(os.environ)
    if rtam:
        env["SR_ENABLE_RTAM"] = "1"
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(RUN_PS),
           "-RunFullDuration", "-TimeoutSeconds", str(timeout_s), "-MinLines", "9000000"]
    try:
        subprocess.run(cmd, cwd=str(ROOT), env=env, capture_output=True, text=True,
                       timeout=timeout_s + 90)
    except subprocess.TimeoutExpired:
        kill_stray()
    return parse_log()


def parse_log(path=None):
    """Scan a log for progress + fatal markers. Single-sourced via crash_markers.

    `fatal` is a list of dicts: {"marker": <name>, "line": <trimmed text>}.
    `last_present` is the present number on the LAST present line seen (used by
    the hang heuristic), distinct from `max_present`.
    """
    path = Path(path) if path is not None else LOG
    m = {
        "max_present": 0,
        "last_present": 0,
        "present_lines": 0,
        "fatal": [],
        "walker_watchdog": False,
        "walk_end": False,
        "ctor_ends": 0,
        "lines": 0,
        "exists": False,
    }
    if not path.exists():
        return m
    m["exists"] = True
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m["lines"] += 1
            pm = PRESENT_RE.search(line)
            if pm:
                v = int(pm.group(1))
                m["present_lines"] += 1
                m["last_present"] = v
                if v > m["max_present"]:
                    m["max_present"] = v
            # Fatal markers: comprehensive, single-sourced with parse_crash_log.
            name = crash_markers.scan_fatal(line)
            if name:
                m["fatal"].append({"marker": name, "line": line.strip()[:160]})
            if WALKER_WATCHDOG_RE.search(line):
                m["walker_watchdog"] = True
            if WALK_END_RE.search(line):
                m["walk_end"] = True
            elif CTOR_END_RE.search(line):
                m["ctor_ends"] += 1
    return m


def classify(m, base):
    """Return (verdict, fails). verdict in PASS/CRASH/HANG/LOW-PROGRESS/NO-LOG.

    Order of precedence: a hard CRASH marker dominates; then a HANG (watchdog or
    stalled present count); then LOW-PROGRESS (below floor but no crash/hang);
    else PASS. fails is a list of human-readable reasons (empty on PASS).
    """
    fails = []

    if not m["exists"]:
        return "NO-LOG", ["log file not found / never written"]

    # 1) Hard crash markers win.
    if base.get("forbid_fatal_crash") and m["fatal"]:
        markers = sorted({f["marker"] for f in m["fatal"]})
        first = m["fatal"][0]
        fails.append(
            f"{len(m['fatal'])} fatal crash marker(s) {markers} "
            f"(first [{first['marker']}]: {first['line']})"
        )
        return "CRASH", fails

    # 2) Hang / stall heuristics (no crash marker present).
    #
    # NOTE on the walker watchdog: in real logs it is a SELF-HEALING diagnostic
    # ("CTOR 'FMMessage' stalled NNNN ms - panic-released N semaphores") that
    # appears even on healthy runs that then reach 6000+ presents. So its mere
    # presence is NOT a hang. It only counts as a hang if the run ALSO failed to
    # make progress (present count below the regression floor). The decisive hang
    # signals are: no presents at all, or the present counter stalling low.
    hang_floor = base.get("hang_present_floor", 50)
    if m["present_lines"] == 0:
        fails.append("no [D3D_DIAG] present lines at all - never reached render loop (hang)")
        return "HANG", fails
    if m["last_present"] < hang_floor:
        reason = (
            f"present stalled: last present #{m['last_present']} < hang floor {hang_floor} "
            f"with no clean shutdown (hang)"
        )
        if m["walker_watchdog"]:
            reason += " ; [WALKER_WATCHDOG] fired and did not recover"
        fails.append(reason)
        return "HANG", fails
    if m["walker_watchdog"] and m["max_present"] < base["min_present"]:
        fails.append(
            f"[WALKER_WATCHDOG] fired and run stayed under floor "
            f"(max present {m['max_present']} < {base['min_present']}) - unrecovered stall (hang)"
        )
        return "HANG", fails

    # 3) Low progress: ran, did not crash or hang, but under the regression floor.
    if m["max_present"] < base["min_present"]:
        fails.append(f"present {m['max_present']} < floor {base['min_present']}")
        # require_walk_end can add a reason too, but verdict stays LOW-PROGRESS.
        if base.get("require_walk_end") and not m["walk_end"]:
            fails.append("WALK_END not reached")
        return "LOW-PROGRESS", fails

    # 4) Milestone requirement (run is otherwise healthy).
    if base.get("require_walk_end") and not m["walk_end"]:
        fails.append("WALK_END not reached")
        return "LOW-PROGRESS", fails

    return "PASS", fails


def evaluate(m, base):
    """Back-compat shim: return the list of failure reasons (empty == PASS)."""
    _, fails = classify(m, base)
    return fails


def load_baseline(baseline_path):
    base = dict(DEFAULT_BASELINE)
    try:
        with open(baseline_path) as f:
            base.update({k: v for k, v in json.load(f).items() if not k.startswith("_")})
    except FileNotFoundError:
        print(f"[smoke] no baseline file at {baseline_path}; using defaults")
    return base


def report_run(m, base, label="run"):
    """Print one run's verdict line(s). Returns (verdict, ok)."""
    verdict, fails = classify(m, base)
    ok = verdict == "PASS"
    print(f"  {verdict} | present={m['max_present']} (last #{m['last_present']}) "
          f"fatal={len(m['fatal'])} watchdog={m['walker_watchdog']} "
          f"walk_end={m['walk_end']} ctor_ends={m['ctor_ends']} loglines={m['lines']}")
    for fdesc in fails:
        print(f"    - {fdesc}")
    return verdict, ok


def check_log_mode(path, base):
    """Score a single existing log -- NO build, NO game run. Exit 0/1."""
    print(f"[smoke] --check-log {path}")
    m = parse_log(path)
    verdict, ok = report_run(m, base, label="check-log")
    print(f"\n[smoke] {'PASS' if ok else 'FAIL'} ({verdict})")
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# Built-in fixture matrix for --self-test. Each fixture is a small synthetic log
# drawn from the exact CLAUDE.md crash formats. Proves detection without a build.
# ---------------------------------------------------------------------------
_PRESENT_BLOCK = "\n".join(
    f"[2026-06-26 00:00:{i:02d}.000] [INFO ] [BOOT] [D3D_DIAG] present #{i * 100} "
    f"queueSize=0 resolveReq=0 pendingSubmits=0"
    for i in range(0, 10)
)  # reaches present #900

SELF_TEST_FIXTURES = {
    "clean_pass": (
        _PRESENT_BLOCK + "\n"
        "[2026-06-26 00:00:11.000] [INFO ] [BOOT] [FM_CTOR] WALK_END\n"
        "[2026-06-26 00:00:12.000] [INFO ] [BOOT] [D3D_DIAG] present #999 queueSize=2\n",
        "PASS",
    ),
    "guest_harness_exception": (
        _PRESENT_BLOCK + "\n"
        "Guest harness: exception 0xC0000005 at host IP 0x00007FF6ABCD1234\n"
        "guest ctx: lr=0x82282DF0 r1=0x70000000 r3=0x00000000\n",
        "CRASH",
    ),
    "excreatethread_crash": (
        _PRESENT_BLOCK + "\n"
        "[ExCreateThread] guest thread startVA=0x82A1B000 exception code=0xC0000005 "
        "at host=00007FF6ABCD5678 fault=0x00000010 lr=0x8267F478 r3=0x00000000 r1=0x71000000\n",
        "CRASH",
    ),
    "final_crash_marker": (
        _PRESENT_BLOCK + "\n"
        "[CRASH] Exception 0xC0000005 at 00007FF6ABCDEF00 (thread 7)\n",
        "CRASH",
    ),
    "bugcheck": (
        _PRESENT_BLOCK + "\n"
        "[BUGCHECK] Halting: unrecoverable guest state\n",
        "CRASH",
    ),
    "host_unhandled": (
        _PRESENT_BLOCK + "\n"
        "Unhandled exception at 0x00007FF6 (EXCEPTION_ACCESS_VIOLATION)\n",
        "CRASH",
    ),
    "watchdog_recovered_pass": (
        # Real-world case: watchdog fired but self-healed; run reached high presents.
        # This MUST be PASS (a recovered watchdog is not a hang).
        "[2026-06-26 00:00:00.000] [INFO ] [BOOT] [WALKER_WATCHDOG] CTOR 'FMMessage' "
        "stalled 1563 ms - panic-released 19 semaphores\n"
        + _PRESENT_BLOCK + "\n"
        "[2026-06-26 00:00:12.000] [INFO ] [BOOT] [FM_CTOR] WALK_END\n",
        "PASS",
    ),
    "watchdog_unrecovered_hang": (
        # Watchdog fired AND present count never cleared the floor -> HANG.
        "[2026-06-26 00:00:00.000] [INFO ] [BOOT] [D3D_DIAG] present #120 queueSize=0\n"
        "[2026-06-26 00:00:01.000] [INFO ] [BOOT] [WALKER_WATCHDOG] CTOR 'FMUI' "
        "stalled 117000 ms - panic-released 40 semaphores\n",
        "HANG",
    ),
    "hang_no_presents": (
        "[2026-06-26 00:00:00.000] [INFO ] [BOOT] booting ...\n"
        "[2026-06-26 00:00:01.000] [INFO ] [BOOT] [FM_CTOR] END FMFoo ctor=0x8200\n",
        "HANG",
    ),
    "hang_stalled_low": (
        # Got a couple of presents then froze well under the hang floor.
        "[2026-06-26 00:00:00.000] [INFO ] [BOOT] [D3D_DIAG] present #0 queueSize=0\n"
        "[2026-06-26 00:00:00.100] [INFO ] [BOOT] [D3D_DIAG] present #3 queueSize=0\n",
        "HANG",
    ),
    "low_progress": (
        # Healthy render loop, no crash/hang, but max present below the floor.
        "\n".join(
            f"[2026-06-26 00:00:{i:02d}.000] [INFO ] [BOOT] [D3D_DIAG] present #{i} queueSize=0"
            for i in range(0, 120)
        ) + "\n",
        "LOW-PROGRESS",
    ),
}


def self_test_mode(base):
    """Write fixtures to a temp dir, score each, print the matrix. Exit 0/1."""
    import tempfile
    print("[smoke] --self-test : detection-only fixture matrix (NO build, NO run)\n")
    tmp = Path(tempfile.mkdtemp(prefix="smoke_selftest_"))
    rows = []
    all_ok = True
    for name, (content, expected) in SELF_TEST_FIXTURES.items():
        fp = tmp / f"{name}.log"
        fp.write_text(content, encoding="utf-8")
        m = parse_log(fp)
        verdict, _ = classify(m, base)
        expected_pass = expected == "PASS"
        got_pass = verdict == "PASS"
        # Correct if verdict class matches AND PASS/FAIL polarity matches.
        correct = (verdict == expected)
        all_ok = all_ok and correct
        rows.append((name, expected, verdict, "ok" if correct else "MISMATCH",
                     "PASS" if got_pass else "FAIL"))

    name_w = max(len(r[0]) for r in rows)
    print(f"  {'fixture'.ljust(name_w)}  {'expected':<13} {'verdict':<13} {'gate':<5} result")
    print(f"  {'-' * name_w}  {'-' * 13} {'-' * 13} {'-' * 5} ------")
    for name, expected, verdict, result, gate in rows:
        print(f"  {name.ljust(name_w)}  {expected:<13} {verdict:<13} {gate:<5} {result}")

    print(f"\n[smoke] self-test {'PASS' if all_ok else 'FAIL'} "
          f"({sum(1 for r in rows if r[3] == 'ok')}/{len(rows)} fixtures correct)")
    return 0 if all_ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", action="store_true", help="cmake --build build first")
    ap.add_argument("--runs", type=int, default=1, help="number of rolls (variance)")
    ap.add_argument("--timeout", type=int, default=None, help="seconds per run (overrides baseline)")
    ap.add_argument("--rtam", action="store_true", help="SR_ENABLE_RTAM=1 path")
    ap.add_argument("--baseline", default=str(BASELINE_FILE))
    ap.add_argument("--check-log", metavar="PATH",
                    help="detection-only: score an existing log (no build, no run)")
    ap.add_argument("--self-test", action="store_true",
                    help="detection-only: run built-in crash-fixture matrix (no build, no run)")
    args = ap.parse_args()

    base = load_baseline(args.baseline)

    # Detection-only modes: never build, never run the game.
    if args.self_test:
        sys.exit(self_test_mode(base))
    if args.check_log:
        sys.exit(check_log_mode(args.check_log, base))

    timeout_s = args.timeout or base["timeout_s"]
    rtam = args.rtam or base.get("rtam", False)

    if args.build:
        build()

    print(f"[smoke] baseline: min_present={base['min_present']} fatal_forbidden={base['forbid_fatal_crash']} "
          f"require_walk_end={base['require_walk_end']} hang_floor={base.get('hang_present_floor', 50)} "
          f"| timeout={timeout_s}s rtam={rtam} runs={args.runs}\n")

    results = []
    all_pass = True
    for i in range(args.runs):
        print(f"[smoke] run {i + 1}/{args.runs} ...", flush=True)
        m = run_once(timeout_s, rtam)
        verdict, ok = report_run(m, base)
        all_pass = all_pass and ok
        results.append((m, ok))

    if args.runs > 1:
        presents = [m["max_present"] for m, _ in results]
        npass = sum(1 for _, ok in results if ok)
        print(f"\n[smoke] variance over {args.runs} runs: "
              f"present min={min(presents)} median={int(statistics.median(presents))} max={max(presents)} "
              f"| pass {npass}/{args.runs}")

    print(f"\n[smoke] {'PASS' if all_pass else 'FAIL'}")
    sys.exit(0 if all_pass else 1)


if __name__ == "__main__":
    main()
