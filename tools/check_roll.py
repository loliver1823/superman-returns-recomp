#!/usr/bin/env python3
"""Combined correctness roll: several lanes' CHECK / census / meter gates in ONE whole-run roll.

The game slot is the bottleneck (one game process at a time), and most queued rolls only ask
"does X still match the original". Those checks do not interfere, so one PORT FEATURE whole run
of a single exe can carry all of them; each lane is graded from its own log lines.

    python tools/check_roll.py --exe <snapshot exe> --tag <name> [--only lane1,lane2] [--dry-run]
    python tools/check_roll.py --grade archive/v1_checkroll/<tag>      # re-grade an existing roll

Manifest: tools/check_roll.json (each lane adds its entries; enabled=false until the code is on
the exe under test). The roll itself is tools/player/roll_player.ps1 (holds the game slot,
background window, harness input only). Logs: <out>/roll/drive/attempt*/run*.log. Perf A/Bs never
go here: CHECK modes slow the game, so a check roll's fps is not evidence.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "tools" / "check_roll.json"
OUT_ROOT = ROOT / "archive" / "v1_checkroll"


def load_checks(only, manifest=None):
    data = json.loads(Path(manifest or MANIFEST).read_text(encoding="utf-8"))
    checks = [c for c in data["checks"] if c.get("enabled")]
    if only:
        want = {s.strip() for s in only.split(",") if s.strip()}
        checks = [c for c in checks if c["lane"] in want]
    return checks


def merge_gates(checks):
    """Union of every check's gates; two checks setting one variable differently is an error."""
    merged, owner = {}, {}
    for c in checks:
        for item in filter(None, (s.strip() for s in c.get("gates", "").split(","))):
            key, _, val = item.partition("=")
            if key in merged and merged[key] != val:
                raise SystemExit("gate conflict: %s=%s (%s) vs %s=%s (%s)"
                                 % (key, merged[key], owner[key], key, val, c["name"]))
            merged[key], owner[key] = val, c["name"]
    return ",".join("%s=%s" % kv for kv in merged.items())


def logs_of(out):
    # roll_player writes into <out>/roll (it refuses an existing -Out, so check_roll keeps its own files beside it)
    return sorted((out / "roll" / "drive").glob("attempt*/run*.log")) + sorted((out / "drive").glob("attempt*/run*.log"))


def audio_logs_of(out):
    """Every game log of a roll for the audio-safety row: the roll's (logs_of) and the flicker gate's span / freeze
    rolls (perf_run: <out>/flicker/<k>/drive/attempt*/run*.log; Astra E0 r1: they were left out)."""
    return logs_of(out) + sorted((out / "flicker").glob("*/drive/attempt*/run*.log"))


def flicker_row(out):
    """The flicker gate's verdict (tools/perf/flicker_gate.py, run by --flicker into <out>/flicker) as a
    check row: lane 'flicker'. Every merge candidate that touches rendering, timing or caching must pass it."""
    p = out / "flicker" / "flicker_gate.json"
    if not p.exists():
        return None
    j = json.loads(p.read_text(encoding="utf-8"))
    sp, fz = j.get("span", {}), j.get("freeze", {})
    return {"lane": "flicker", "name": "flicker gate (every-present span + sim-freeze exact)",
            "verdict": j.get("verdict", "NO DATA"),
            "pass_hits": int(sp.get("verdict") == "PASS") + int(fz.get("verdict") == "PASS"),
            "fail_hits": int(sp.get("verdict") == "FAIL") + int(fz.get("verdict") == "FAIL"),
            "last_pass_line": "span %s %s | freeze %s counted=%s bad=%s" % (
                sp.get("verdict"), sp.get("line", ""), fz.get("verdict"), fz.get("counted"), fz.get("bad"))}


AUDIO_EXE = "audio_exe.json"   # the exe check_roll launched the roll with: the audio row's exe under test


def resolve_exe(exe):
    """The one absolute path of --exe, used for the record, every launch and the grading (Astra E0 r8): a relative
    path is relative to ROOT, where the launches run (cwd=ROOT), whatever the caller's working directory."""
    p = Path(exe)
    return str((p if p.is_absolute() else ROOT / p).resolve())


def record_exe(out, exe):
    """Record the exe a roll is launched with (before the launch), so a re-grade knows the exe under test."""
    (Path(out) / AUDIO_EXE).write_text(json.dumps({"exePath": exe}), encoding="utf-8")


def recorded_exe(out):
    try:
        p = json.loads((Path(out) / AUDIO_EXE).read_text(encoding="utf-8"))
        return p.get("exePath") if isinstance(p, dict) and isinstance(p.get("exePath"), str) else None
    except (OSError, ValueError):
        return None


def audio_safety_row(logs, exe=None):
    """Lane E E0: an always-on row (lane 'audio-safety', tools/audio_safety_check.py). FAIL = the roll opened a real
    audio endpoint off the allowlist (the owner's speakers / headset), or a log cannot be read; NO-EVIDENCE = no game
    log, or one (empty ones included) without an [AUDIO_OPEN] receipt whose audio never visibly ran: the absence of
    an open is not proof; N/A = the exe under test (`exe`: the one this roll was launched with) is a verified pre-E0
    build and each receipt-less run is one of it (its provenance names that sha256): not a FAIL of the gate
    (coordinator 2026-10-23), while a real-device open in its old-format lines still FAILs."""
    sys.path.insert(0, str(ROOT / "tools"))
    import audio_safety_check
    r = audio_safety_check.check([str(p) for p in logs], all_game=True, exe=exe)   # every run log is a game log
    v = r["violations"]
    na = r["verdict"] == "N/A"
    return {"lane": "audio-safety", "name": "no real audio endpoint off the allowlist",
            "verdict": "N/A (pre-E0 build)" if na else r["verdict"],
            "pass_hits": len(r["game_logs"]) - len(r["no_evidence"]), "fail_hits": len(v),
            "last_pass_line": ("%s:%d %s" % (v[0]["file"], v[0]["line"], v[0]["why"])) if v else
            ("%d game log(s), %d without a receipt, %d pre-E0, %d device open(s), %d refusal(s)" % (
                len(r["game_logs"]), len(r["no_evidence"]), len(r["pre_e0"]), len(r["opened"]), len(r["warnings"])))}


def row_passes(r):
    """PASS, or the audio row's N/A for a pre-E0 build (only that row has an N/A)."""
    return r["verdict"] == "PASS" or (r["lane"] == "audio-safety" and r["verdict"] == "N/A (pre-E0 build)")


def run_flicker_gate(exe, out):
    """Two perf_run gameplay rolls (~4 min each) under the game slot as owner check-roll."""
    slot = [sys.executable, str(ROOT / "tools" / "game_slot.py")]
    subprocess.call(slot + ["acquire", "--owner", "check-roll", "--poll", "30", "--note", "check_roll flicker gate"],
                    cwd=str(ROOT))
    try:
        rc = subprocess.call([sys.executable, str(ROOT / "tools" / "perf" / "flicker_gate.py"), "--exe", exe,
                              "--out", str(out)], cwd=str(ROOT))
        print("flicker_gate exit %d" % rc)
    finally:
        subprocess.call(slot + ["release", "--owner", "check-roll"], cwd=str(ROOT))


def grade(out, checks, need_flicker=False, exe=None):
    """need_flicker: the flicker gate was run (--flicker / --flicker-only), so a missing result is NO-EVIDENCE.
    exe: the exe under test for the audio row (default: the one recorded when check_roll launched this roll)."""
    logs = logs_of(out)
    texts = []
    for p in logs:
        try:
            texts.append(p.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            pass
    body = "\n".join(texts)
    results = []
    for c in checks:
        pm = re.findall(c["pass"], body, re.M) if c.get("pass") else []
        fm = re.findall(c["fail"], body, re.M) if c.get("fail") else []
        last_pass = None
        if c.get("pass"):
            for line in body.splitlines():
                if re.search(c["pass"], line):
                    last_pass = line.strip()[:200]
        # fail closed (iter-467): an entry without a pass pattern can never grade PASS - the absence of
        # fail lines is not evidence (a check that never ran also has none)
        verdict = "FAIL" if fm else ("PASS" if pm else ("NO-EVIDENCE" if c.get("pass") else "NO-PASS-PATTERN"))
        results.append({"lane": c["lane"], "name": c["name"], "verdict": verdict,
                        "pass_hits": len(pm), "fail_hits": len(fm), "last_pass_line": last_pass})
    fr = flicker_row(out)
    if fr is None and need_flicker:
        fr = {"lane": "flicker", "name": "flicker gate (every-present span + sim-freeze exact)",
              "verdict": "NO-EVIDENCE", "pass_hits": 0, "fail_hits": 0,
              "last_pass_line": "no %s" % (out / "flicker" / "flicker_gate.json")}
    if fr:
        results.append(fr)
    results.append(audio_safety_row(audio_logs_of(out), exe or recorded_exe(out)))
    report = {"out": str(out), "logs": [str(p) for p in logs], "results": results}
    (out / "check_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("logs: %d" % len(logs))
    for r in results:
        print("%-12s %-11s %-40s pass=%d fail=%d" % (r["lane"], r["verdict"], r["name"][:40],
                                                    r["pass_hits"], r["fail_hits"]))
    return 0 if all(row_passes(r) for r in results) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", help="the exe under test (a relative path is relative to the repo root)")
    ap.add_argument("--tag")
    ap.add_argument("--only", default="")
    ap.add_argument("--extra-gates", default="", help="added after the manifest gates (e.g. resolution)")
    ap.add_argument("--timeout", type=int, default=1500)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--grade", help="re-grade an existing roll directory")
    ap.add_argument("--manifest", help="another checkout's check_roll.json (a lane's new entries before they reach dev);"
                                       " the roll still runs from THIS tree, which has the disc next to it")
    ap.add_argument("--flicker", action="store_true",
                    help="iter-467: also run the flicker gate (tools/perf/flicker_gate.py: every present of a gameplay "
                         "span + sim-freeze exact, ~8 min) on the exe into <out>/flicker, graded as lane 'flicker'. "
                         "Mandatory for a merge candidate that touches rendering, timing or caching.")
    ap.add_argument("--flicker-only", action="store_true", help="run only the flicker gate (no check roll)")
    a = ap.parse_args()

    if a.exe:
        a.exe = resolve_exe(a.exe)
    checks = load_checks(a.only, a.manifest)
    if a.grade:
        return grade(Path(a.grade), checks, exe=a.exe)   # --exe on a re-grade: the exe the roll is of (audio row)
    if not (a.exe and a.tag):
        ap.error("--exe and --tag are required for a roll")
    if a.flicker_only:
        out = OUT_ROOT / a.tag
        # nothing is written or launched into a used flicker dir, or beside a record of another exe (Astra E0 r8: a
        # failed child must not leave an old flicker PASS / another exe's logs to be graded as this run's)
        fl = out / "flicker"
        if fl.exists() and any(fl.iterdir()):
            raise SystemExit("%s exists and is not empty: use a fresh --tag" % fl)
        if (out / AUDIO_EXE).exists() and recorded_exe(out) != a.exe:
            raise SystemExit("%s records another exe (%s): use a fresh --tag" % (out / AUDIO_EXE, recorded_exe(out)))
        out.mkdir(parents=True, exist_ok=True)
        record_exe(out, a.exe)
        run_flicker_gate(a.exe, out / "flicker")
        return grade(out, [], need_flicker=True, exe=a.exe)
    if not checks:
        raise SystemExit("no enabled checks in %s" % (a.manifest or MANIFEST))
    gates = merge_gates(checks)
    if a.extra_gates:
        gates = gates + ("," if gates else "") + a.extra_gates
    out = OUT_ROOT / a.tag
    print("checks: %s" % ", ".join("%s/%s" % (c["lane"], c["name"]) for c in checks))
    print("gates : %s" % gates)
    print("out   : %s" % out)
    if a.dry_run:
        return 0
    if out.exists() and any(out.iterdir()):
        raise SystemExit("%s exists and is not empty: use a fresh --tag" % out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "checks.json").write_text(json.dumps(checks, indent=2), encoding="utf-8")
    record_exe(out, a.exe)
    cmd = ["powershell", "-NoProfile", "-File", str(ROOT / "tools" / "player" / "roll_player.ps1"),
           "-ExePath", a.exe, "-Out", str(out / "roll"), "-Owner", "check-roll",
           "-TimeoutSeconds", str(a.timeout)]
    if gates:
        cmd += ["-Gates", gates]
    rc = subprocess.call(cmd, cwd=str(ROOT))
    print("roll_player exit %d" % rc)
    if a.flicker:
        run_flicker_gate(a.exe, out / "flicker")
    return grade(out, checks, need_flicker=a.flicker, exe=a.exe)


if __name__ == "__main__":
    sys.exit(main())
