#!/usr/bin/env python3
"""replay_batch.py - a batch of deterministic replays (tools/replay_ab.ps1) run under the ONE game
slot, then compared. iter-466 gfx lane.

A replay is a SupermanReturns process, so it takes the game slot like a roll (CLAUDE.md: one game
process at a time; a replay next to a perf roll skews that roll). This tool queues for the slot,
reads the job file only once the slot is held (so jobs added while queueing are included), runs
every job back to back, releases the slot, and writes a summary.

    python tools/replay_batch.py run --owner gfxset --jobs archive/gfxset/jobs.json
    python tools/replay_batch.py compare --out archive/gfxset/hook --a base --b hook

Job file (JSON):
    {"out": "archive/gfxset/hook",
     "jobs": [{"tag": "base", "exe": "build/SupermanReturns_base.exe", "gates": "",
               "traces": "menu,eahd,t16,t21,t25,gp1,gp2,lg1,lg2"}, ...]}
Each job writes <out>/<tag>_<trace>.png (+ _err.txt, the replay log) via pm4_replay_score.py's
trace table. `compare` reports, per trace, whether A and B are byte-identical and the mean / max
absolute difference, and writes <out>/<a>_vs_<b>_<trace>.png side by side (with a x8 diff map).
Both commands exit nonzero on failed or empty results. When batch.json is present,
comparison also requires completed, crash-free replay logs for the requested pair.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import uuid

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools"))
import pm4_replay_score as score  # noqa: E402  (the trace table and fixture paths)
import replay_receipt
from perf import run_health

ALL = "menu,eahd,t16,t21,t25,gp1,gp2,lg1,lg2"


def slot(*args):
    return subprocess.run([sys.executable, os.path.join(REPO, "tools", "game_slot.py"), *args],
                          cwd=REPO, capture_output=True, text=True)


def replay_error(frame, log, stdout, wrapper_exit_code, *, receipt=None, exe=None, trace=None, child_stdout=None):
    """A PNG alone is not evidence that its replay completed successfully."""
    if type(wrapper_exit_code) is not int:
        return "missing or invalid wrapper exit status"
    if wrapper_exit_code != 0:
        return f"wrapper exit {wrapper_exit_code}"
    if "TIMEOUT" in stdout:
        return "replay timed out"
    problems = replay_receipt.errors(receipt, exe=exe, trace=trace, stderr=log, stdout=child_stdout)
    if problems:
        return '; '.join(problems)
    if not os.path.isfile(frame):
        return "missing frame"
    try:
        from PIL import Image
        with Image.open(frame) as capture:
            capture.verify()
    except (OSError, ValueError) as error:
        return f"invalid frame: {error}"
    if not os.path.isfile(log):
        return "missing replay log"
    with open(log, encoding="utf-8", errors="replace") as stream:
        lines = stream.read().splitlines()
    health = run_health.scan_log(log)
    if not health['healthy']:
        return 'unhealthy replay: ' + str(health['reason'])
    if health['sha256'] != receipt['stderr_sha256']:
        return 'replay log changed before health scan'
    if sum("[GPU:REPLAY] SUMMARY" in line for line in lines) != 1:
        return "missing or ambiguous completion summary"
    return None


def run_job(job, out):
    exe = job.get("exe", "build/SupermanReturns.exe")
    gates = job.get("gates", "")
    rows = []
    for short in [t.strip() for t in job.get("traces", ALL).split(",") if t.strip()]:
        trace, _ = {**score.TRACES, **score.EXTRA}[short]
        # A failed replay must never pick up a PNG left by an earlier process.
        run_tag = f"rb_{job['tag']}_{short}_{uuid.uuid4().hex}"
        for suffix in (".png", "_err.txt", "_out.txt", "_process.json", "_wrapper_out.txt", "_wrapper_err.txt", "_f2.png", "_f3.png"):
            if os.path.exists(os.path.join(out, f"{job['tag']}_{short}{suffix}")):
                raise FileExistsError("Preserve prior replay evidence; choose a fresh output directory")
        receipt_path = os.path.join(out, f"{job['tag']}_{short}_process.json")
        expected_exe = os.path.abspath(os.path.join(REPO, exe))
        expected_trace = os.path.abspath(score._fixture(trace))
        cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
               os.path.join("tools", "replay_ab.ps1"), "-Tag", run_tag, "-Trace", score._fixture(trace),
               "-ExePath", exe, "-ReceiptPath", receipt_path]
        if gates:
            cmd += ["-Gates", gates]
        # iter-467 native-b: a looped replay (SR_GPU_TRACE_REPLAY_LOOPS) also dumps frames 2 and 3 -
        # the steady state a cache-dependent change (native targets) is judged on; kept as
        # <tag>_<trace>_f2.png / _f3.png. Stale ones from an earlier run are removed first.
        for k in (2, 3):
            stale = os.path.join(REPO, f"replay_frame_{k}.bmp")
            if os.path.exists(stale):
                os.remove(stale)
        t0 = time.time()
        p = subprocess.run(cmd, cwd=REPO, capture_output=True)
        dt = time.time() - t0
        for suffix, contents in [('_wrapper_out.txt', p.stdout), ('_wrapper_err.txt', p.stderr)]:
            with open(os.path.join(out, f"{job['tag']}_{short}" + suffix), 'xb') as stream:
                stream.write(contents if isinstance(contents, bytes) else contents.encode('utf-8'))
        wrapper_stdout = p.stdout.decode('utf-8', errors='replace') if isinstance(p.stdout, bytes) else p.stdout
        for k in (2, 3):
            fk = os.path.join(REPO, f"replay_frame_{k}.bmp")
            if os.path.exists(fk):
                from PIL import Image
                Image.open(fk).convert("RGB").save(os.path.join(out, f"{job['tag']}_{short}_f{k}.png"))
                os.remove(fk)
        for suffix, dst in ((".png", f"{job['tag']}_{short}.png"), ("_err.txt", f"{job['tag']}_{short}_err.txt"),
                            (".bmp", None), ("_out.txt", f"{job['tag']}_{short}_out.txt")):
            src = os.path.join(REPO, f"scratch_replay_{run_tag}{suffix}")
            if os.path.exists(src):
                if dst:
                    shutil.move(src, os.path.join(out, dst))
                else:
                    os.remove(src)
        stray = os.path.join(REPO, "replay_frame_1.bmp")
        if os.path.exists(stray):
            os.remove(stray)
        child_stdout = os.path.join(out, f"{job['tag']}_{short}_out.txt")
        error = replay_error(os.path.join(out, f"{job['tag']}_{short}.png"),
                             os.path.join(out, f"{job['tag']}_{short}_err.txt"), wrapper_stdout, p.returncode,
                             receipt=replay_receipt.read(receipt_path), exe=expected_exe, trace=expected_trace,
                             child_stdout=child_stdout)
        ok = error is None
        line = f"{job['tag']:>14} {short:5} {'ok' if ok else 'FAIL'} {dt:5.1f}s {gates}"
        if error:
            line += f" | {error}"
        print(line, flush=True)
        rows.append({"tag": job["tag"], "trace": short, "ok": ok, "secs": round(dt, 1), "gates": gates,
                     "exe": expected_exe, "trace_path": expected_trace, "stdout": wrapper_stdout[-400:], "run_tag": run_tag,
                     "wrapper_exit_code": p.returncode, "error": error})
    return rows


def cmd_run(a):
    if a.slot_held:  # iter-467: the caller holds the slot (one acquire for a chain of parts) - no acquire, no release
        print(f"[replay_batch] running under the caller's slot ({a.owner})", flush=True)
    else:
        print(f"[replay_batch] queueing for the game slot as {a.owner}", flush=True)
        r = subprocess.run([sys.executable, os.path.join(REPO, "tools", "game_slot.py"), "acquire", "--owner", a.owner,
                            "--poll", str(a.poll), "--max-hours", str(a.max_hours)] +
                           (["--min-free-gb", str(a.min_free_gb)] if a.min_free_gb else []), cwd=REPO)
        if r.returncode != 0:
            sys.exit(f"[replay_batch] slot not acquired (exit {r.returncode})")
    try:
        with open(os.path.join(REPO, a.jobs), encoding="utf-8") as f:
            spec = json.load(f)
        out = os.path.join(REPO, spec["out"])
        os.makedirs(out, exist_ok=True)
        if os.path.exists(os.path.join(out, "batch.json")):
            raise FileExistsError("Preserve prior batch receipt; choose a fresh output directory")
        rows = []
        for job in spec["jobs"]:
            rows += run_job(job, out)
            slot("heartbeat", "--owner", a.owner)
        with open(os.path.join(out, "batch.json"), "w", encoding="utf-8") as f:
            json.dump(rows, f, indent=1)
    finally:
        if not a.slot_held:
            slot("release", "--owner", a.owner)
            print("[replay_batch] slot released", flush=True)
    return 0 if rows and all(row["ok"] for row in rows) else 1


def cmd_compare(a):
    from PIL import Image, ImageChops, ImageStat
    out = os.path.join(REPO, a.out)
    shorts = [t.strip() for t in a.traces.split(",") if t.strip()]
    if len(set(shorts)) != len(shorts):
        print("INVALID duplicate comparison traces")
        return 1
    batch = None
    receipt = os.path.join(out, "batch.json")
    if os.path.exists(receipt):
        with open(receipt, encoding="utf-8") as stream:
            batch = json.load(stream)
        if not isinstance(batch, list) or not all(isinstance(row, dict) for row in batch):
            print("INVALID batch receipt")
            return 1
    same = 0
    for short in shorts:
        pa = os.path.join(out, f"{a.a}_{short}{a.suffix}.png")
        pb = os.path.join(out, f"{a.b}_{short}{a.suffix}.png")
        if batch is not None:
            errors = []
            for tag, frame in ((a.a, pa), (a.b, pb)):
                rows = [row for row in batch if row.get("tag") == tag and row.get("trace") == short]
                if len(rows) != 1 or rows[0].get("ok") is not True:
                    errors.append(f"{tag}: missing, failed or ambiguous batch result")
                    continue
                row = rows[0]
                error = replay_error(frame, os.path.join(out, f"{tag}_{short}_err.txt"),
                                     row.get("stdout", ""), row.get("wrapper_exit_code"),
                                     receipt=replay_receipt.read(os.path.join(out, f"{tag}_{short}_process.json")),
                                     exe=row.get('exe'), trace=row.get('trace_path'),
                                     child_stdout=os.path.join(out, f"{tag}_{short}_out.txt"))
                if error:
                    errors.append(f"{tag}: {error}")
            if errors:
                print(f"{short:5} INVALID {'; '.join(errors)}")
                continue
        if not (os.path.exists(pa) and os.path.exists(pb)):
            print(f"{short:5} MISSING {'A' if not os.path.exists(pa) else ''}{'B' if not os.path.exists(pb) else ''}")
            continue
        ia, ib = Image.open(pa).convert("RGB"), Image.open(pb).convert("RGB")
        if ia.size != ib.size:
            print(f"{short:5} SIZE {ia.size} vs {ib.size}")
            continue
        d = ImageChops.difference(ia, ib)
        ident = d.getbbox() is None
        same += ident
        mad = sum(ImageStat.Stat(d).mean) / 3.0
        mx = max(e[1] for e in d.getextrema())
        changed = sum(1 for px in d.getdata() if px != (0, 0, 0)) * 100.0 / (ia.size[0] * ia.size[1])
        print(f"{short:5} {'IDENTICAL' if ident else 'differs  '} MAD {mad:7.3f}  max {mx:3d}  changed {changed:6.2f}%")
        if not ident or a.sheets:
            w, h = ia.size
            sheet = Image.new("RGB", (w * 3, h))
            sheet.paste(ia, (0, 0))
            sheet.paste(ib, (w, 0))
            sheet.paste(d.point(lambda v: min(255, v * 8)), (w * 2, 0))
            if w * 3 > 3840:
                sheet = sheet.resize((3840, int(h * 3840 / (w * 3))))
            sheet.save(os.path.join(out, f"{a.a}_vs_{a.b}_{short}{a.suffix}.png"))
    print(f"{same}/{len(shorts)} byte-identical ({a.a} vs {a.b})")
    return 0 if shorts and same == len(shorts) else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    r = sp.add_parser("run")
    r.add_argument("--owner", required=True)
    r.add_argument("--jobs", required=True)
    r.add_argument("--poll", type=int, default=20)
    r.add_argument("--max-hours", type=float, default=12)
    r.add_argument("--slot-held", action="store_true", help="the caller already holds the slot under --owner")
    r.add_argument("--min-free-gb", type=float, default=0,
                   help="game_slot.py acquire's free-RAM floor (0 = its default); a replay needs less than a roll")
    c = sp.add_parser("compare")
    c.add_argument("--out", required=True)
    c.add_argument("--a", required=True)
    c.add_argument("--b", required=True)
    c.add_argument("--traces", default=ALL)
    c.add_argument("--sheets", action="store_true", help="also write sheets for identical pairs")
    c.add_argument("--suffix", default="", help="iter-467: compare the looped frames (_f2 / _f3)")
    a = ap.parse_args()
    return {"run": cmd_run, "compare": cmd_compare}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
