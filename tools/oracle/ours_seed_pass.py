#!/usr/bin/env python3
"""ours_seed_pass.py - step-4 lane A (xenia-oracle): OUR half of the chapter matrix (tools/oracle/xenia_seed_pass.py
is the Xenia half). For every chapter seed: one tools/perf/perf_run.py --from-save roll on the PC (the custom front
end's LOAD GAME -> slot 0 -> METROPOLIS on a carousel landing), then NO input (tools/oracle/idle_play.txt), sparse
GPU-readback present dumps (the reliable capture) and the guest's mixed audio frames (SR_AUDIO_FRAMES = the same
capture point and AFR1 format as the Xenia side). Afterwards it picks the dumped present nearest to the anchor +
each offset (the anchor = the guest's request of screens/hud/main_hud after the load, [APT_SCREEN] in our log, the
same AptLoadByPath event the Xenia side hooks) and writes it as matched/hud_+NNN.png.

    py -3.11 tools/oracle/ours_seed_pass.py --out <pass dir> --exe <snapshot>/SupermanReturns.exe
        [--seeds a,b] [--offsets 10,30,60,120] [--fps 0] [--every 60] [--stage J:\\sr_stage --stage-source <dev root>]
        [--hold-size 6] [--gap 150] [--no-slot]
    py -3.11 tools/oracle/ours_seed_pass.py --out <pass dir> --pick-only      # re-pick from finished rolls

Gates (perf_run --gates): SR_HIGHFPS=<--fps> (default 0 = the console's 30 Hz render loop, Xenia's rate),
SR_PRESENT_DUMP_SPARSE_EVERY=<--every> (60 at 30 Hz = one present every 2 s), SR_PRESENT_DUMP_SPARSE_CAP=400,
SR_AUDIO_FRAMES=<seed>/ours.afr. perf_run itself pins SR_FRAME_DUMP=0 (no timed guest-framebuffer dumps),
SR_INSTANCE_ID=perf (background, input isolated), SR_LOG=player, SR_PORT_FEATURES=1 (--config port: the custom
front end is how a roll LOADs; a PORT FEATURE roll), SR_PERF_STATS. Takes the game slot per --hold-size group.
"""
from __future__ import annotations

import argparse
import datetime as dt
import glob
import json
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, HERE)
import xenia_seed_pass as xsp  # noqa: E402  (SEEDS, SAVES, QUIET, slot helpers)

OWNER = "xenia-oracle"
LOG_TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
LOAD_LINE = re.compile(r"\[SR_FSCMD\].*SR_SaveEvent 'load'")
HUD_LINE = re.compile(r"\[APT_SCREEN\] path='screens/hud/main_hud'")
CAROUSEL_LINE = re.compile(r"\[APT_SCREEN\] path='screens/frontend/ScrMainMenu'")
DUMP_NAME = re.compile(r"present_(cl|UP)_(\d+)_p(\d+)_(\d+)x(\d+)\.bmp$")


def say(*m):
    print("[%s]" % time.strftime("%H:%M:%S"), *m, flush=True)


def ts(line):
    m = LOG_TS.match(line)
    return dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp() if m else None


def slot(verb, note=""):
    cmd = [sys.executable, os.path.join(TOOLS, "game_slot.py"), verb, "--owner", OWNER]
    if verb == "acquire":
        cmd += ["--poll", "30", "--note", note or "step4 ours seed pass"]
    say("slot", verb)
    return subprocess.run(cmd, cwd=REPO).returncode


def roll_log(roll):
    logs = sorted(glob.glob(os.path.join(roll, "drive", "attempt*", "run.log")))
    return logs[-1] if logs else None


def anchor_of(log):
    """(t_load, t_anchor, landing, t_first_gameplay): the slot's load event, the first main_hud request after it."""
    t_load = t_hud = t_car = t_play = None
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            if t_load is None:
                if LOAD_LINE.search(line):
                    t_load = ts(line)
                continue
            if t_car is None and CAROUSEL_LINE.search(line):
                t_car = ts(line)
            if t_hud is None and HUD_LINE.search(line):
                t_hud = ts(line)
            if t_hud is not None and t_play is None and "[PC_CONTROLS] context gameplay " in line:
                t_play = ts(line)
                break
    landing = None if t_hud is None else ("carousel" if t_car is not None and t_car < t_hud else "hud")
    return t_load, t_hud, landing, t_play


def pick(seed_out, offsets, tol=1.6):
    """matched/hud_+NNN.png = the dump nearest to anchor + each offset (file mtime = the moment it was written)."""
    from PIL import Image
    roll = os.path.join(seed_out, "roll")
    log = roll_log(roll)
    res = {"log": log}
    if not log:
        res["reason"] = "no run.log"
        return res
    t_load, t_hud, landing, t_play = anchor_of(log)
    res.update({"t_load": t_load, "anchor_epoch": t_hud, "landing": landing, "t_gameplay_ctx": t_play})
    if t_hud is None:
        res["reason"] = "no main_hud request after the load"
        return res
    dumps = []
    for p in glob.glob(os.path.join(roll, "drive", "attempt*", "frames", "*.bmp")):
        m = DUMP_NAME.search(os.path.basename(p))
        if m:
            dumps.append((os.path.getmtime(p), int(m.group(3)), p))
    dumps.sort()
    res["dumps"] = len(dumps)
    os.makedirs(os.path.join(seed_out, "matched"), exist_ok=True)
    got = {}
    for o in offsets:
        want = t_hud + o
        best = min(dumps, key=lambda d: abs(d[0] - want)) if dumps else None
        if not best or abs(best[0] - want) > tol:
            got["hud_%+04d" % o] = {"ok": False, "nearest_s": None if not best else round(best[0] - t_hud, 2)}
            continue
        dst = os.path.join(seed_out, "matched", "hud_%+04d.png" % o)
        Image.open(best[2]).convert("RGB").save(dst, compress_level=1)
        got["hud_%+04d" % o] = {"ok": True, "at_s": round(best[0] - t_hud, 2), "present": best[1],
                                "src": os.path.relpath(best[2], seed_out)}
    res["matched"] = got
    # the survey: every dump after the anchor as a 640-wide JPEG, for the sheets and for scene matching
    sv = os.path.join(seed_out, "survey")
    os.makedirs(sv, exist_ok=True)
    for t, pn, p in dumps:
        rel = t - t_hud
        if -6 <= rel <= max(offsets) + 10:
            dst = os.path.join(sv, "s_%+07.1f_p%06d.jpg" % (rel, pn))
            if not os.path.exists(dst):
                im = Image.open(p).convert("RGB")
                im.resize((640, int(im.height * 640 / im.width))).save(dst, quality=88)
    return res


def run_seed(a, name, pass_dir):
    guard = xsp.guard_owner(a, name)   # --owner-guard: SystemExit while the owner runs a game
    seed_dir = os.path.join(a.saves, name)
    so = os.path.join(pass_dir, name)
    if os.path.exists(os.path.join(so, "roll")):
        k = 2
        while os.path.exists("%s_r%d" % (so, k)):
            k += 1
        so = "%s_r%d" % (so, k)
    os.makedirs(so, exist_ok=True)
    gates = ["SR_HIGHFPS=%s" % a.fps] + ([] if a.no_afr else ["SR_AUDIO_FRAMES=" + os.path.join(so, "ours.afr")])
    gates += [g.replace("{run}", so) for g in a.gates_extra]   # e.g. SR_AUDIT_IMPORTS_OUT={run}\ours_census.json
    play = os.path.join(HERE, "idle_play.txt")
    if a.dense_at is not None:
        # ONE every-present window (SR_PRESENT_DUMP_EVERY=1 + FROM_MARK=1: the harness DUMP MARK opens it) of
        # --dense-cap presents, marked --dense-at s after the main_hud request by a generated harness_play tape
        gates += ["SR_PRESENT_DUMP_EVERY=1", "SR_PRESENT_DUMP_FROM_MARK=1", "SR_PRESENT_DUMP_CAP=%d" % a.dense_cap]
        play = os.path.join(so, "dense_play.txt")
        lines = ["# ours_seed_pass.py --dense-at %.1f: the idle tape with the dump mark" % a.dense_at,
                 r"await \[SR_FSCMD\].*answer cppId=\S+ button=1 1500",
                 r"await \[APT_(REQ|SCREEN)\].*path='screens/hud/main_hud' 900"]
        if a.dense_at > 0:
            lines.append("wait %d" % int(a.dense_at * 1000))
        lines += ["mark dump", "sample dense", "wait 60000"]
        with open(play, "w") as f:
            f.write("\n".join(lines) + "\n")
    else:
        gates += ["SR_PRESENT_DUMP_SPARSE_EVERY=%d" % a.every, "SR_PRESENT_DUMP_SPARSE_CAP=400"]
    if a.tape:
        gates.append("SR_INPUT_REPLAY=" + os.path.abspath(a.tape))   # the same .inpr the Xenia side plays
    gates += [g for g in a.gates.split(",") if g.strip()]   # extra diagnostics (e.g. SR_PM4_TRACE_SWAP=...)
    cmd = [sys.executable, os.path.join(TOOLS, "perf", "perf_run.py"), "--scenario", "gameplay", "--config", "port",
           "--from-save", seed_dir, "--exe", os.path.abspath(a.exe), "--out", os.path.join(so, "roll"),
           "--dwell", str(a.dwell), "--play", play, "--gates", ",".join(gates)]
    if a.stage:
        cmd += ["--stage", a.stage] + (["--stage-source", a.stage_source] if a.stage_source else [])
    meta = {"seed": name, "cmd": cmd, "start": time.time(), "owner_guard": guard,
            "machine": os.environ.get("COMPUTERNAME", "")}
    t0 = time.time()
    mw = []
    if a.metro_watch:
        # M1: tools/oracle/metro_watch.py on the game perf_run launches (found by name + start time + exe path)
        def watch():
            import psutil
            want = os.path.normcase(os.path.abspath(a.exe))
            end = time.time() + 600
            while time.time() < end:
                for p in psutil.process_iter(["name", "create_time", "exe"]):
                    try:
                        if (p.info["name"] or "").lower() == "supermanreturns.exe" and p.info["create_time"] >= t0 - 1 \
                                and os.path.normcase(p.info["exe"] or "") == want:
                            mw.append(xsp.start_metro_watch(a, p.pid, so, "ours"))
                            meta["game_pid"] = p.pid
                            return
                    except Exception:  # noqa: BLE001
                        continue
                time.sleep(0.5)
        threading.Thread(target=watch, daemon=True).start()
    with open(os.path.join(so, "perf_run.out"), "w", encoding="utf-8", errors="replace") as f:
        rc = subprocess.run(cmd, cwd=REPO, stdout=f, stderr=subprocess.STDOUT).returncode
    meta.update({"rc": rc, "seconds": round(time.time() - t0, 1)})
    for w in mw:
        xsp.stop_metro_watch(so, w)
    r = pick(so, a.offsets)
    meta.update(r)
    if a.metro_watch:
        meta["metro_events"] = xsp.metro_summary(os.path.join(so, "metro.jsonl"), r.get("anchor_epoch"))
    afr = os.path.join(so, "ours.afr")
    meta["afr_seconds_total"] = round(os.path.getsize(afr) / 6160 / 187.5, 1) if os.path.exists(afr) else 0.0
    ok = rc == 0 and r.get("anchor_epoch") and all(v.get("ok") for v in (r.get("matched") or {"x": {}}).values())
    meta["verdict"] = "PASS" if ok and (meta["afr_seconds_total"] > 0 or a.no_afr) else "FAIL"
    if meta["verdict"] == "FAIL" and not meta.get("reason"):
        meta["reason"] = "perf_run rc %s, matched %s, AFR %s s" % (
            rc, {k: v.get("ok") for k, v in (r.get("matched") or {}).items()}, meta["afr_seconds_total"])
    with open(os.path.join(so, "ours_run.json"), "w") as f:
        json.dump(meta, f, indent=1)
    return meta


def main():
    global OWNER
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--exe", default="")
    ap.add_argument("--seeds", default=",".join(xsp.SEEDS))
    ap.add_argument("--saves", default=xsp.SAVES)
    ap.add_argument("--offsets", default="10,30,60,120")
    ap.add_argument("--fps", default="0")
    ap.add_argument("--every", type=int, default=60)
    ap.add_argument("--dwell", type=int, default=150)
    ap.add_argument("--stage", default="")
    ap.add_argument("--stage-source", default="")
    ap.add_argument("--hold-size", type=int, default=6)
    ap.add_argument("--gap", type=float, default=150.0)
    ap.add_argument("--no-slot", action="store_true")
    ap.add_argument("--pick-only", action="store_true")
    ap.add_argument("--expect-sha", default="", help="the exe's sha256 prefix, checked inside the first hold")
    ap.add_argument("--expect-files", type=int, default=0, help="the snapshot folder's file count, checked with it")
    ap.add_argument("--tape", default="", help="a v2 .inpr played from the main_hud request (SR_INPUT_REPLAY)")
    ap.add_argument("--dense-at", type=float, default=None, help="one every-present dump window this many s after the HUD")
    ap.add_argument("--dense-cap", type=int, default=300, help="presents in the --dense-at window")
    ap.add_argument("--gates", default="", help="extra NAME=VALUE gates for perf_run (comma list; ';' inside values)")
    ap.add_argument("--owner", default=OWNER, help="the game-slot owner name (the lane)")
    ap.add_argument("--metro-watch", action="store_true", help="tools/oracle/metro_watch.py on the game (M1)")
    ap.add_argument("--no-afr", action="store_true", help="no SR_AUDIO_FRAMES (long idle rolls)")
    ap.add_argument("--gates-extra", action="append", default=[],
                    help="an extra NAME=VALUE gate per use; {run} = the seed's run folder")
    ap.add_argument("--frida-python", default=os.environ.get("SR_PYTHON") or "py -3.13",
                    help="the Python with frida, for --metro-watch")
    ap.add_argument("--owner-guard", default="", metavar="AGENT_ROOT",
                    help="before each seed refuse if the owner runs a game / emulator outside AGENT_ROOT (the Ally)")
    a = ap.parse_args()
    OWNER = xsp.OWNER = a.owner   # xsp.pending_reservation compares against its own OWNER
    a.frida_python = a.frida_python.split() if " " in a.frida_python.strip() and not os.path.isfile(a.frida_python) \
        else [a.frida_python]
    a.offsets = [int(x) for x in a.offsets.split(",")]
    seeds = [s.strip() for s in a.seeds.split(",") if s.strip()]
    pass_dir = os.path.abspath(a.out)
    os.makedirs(pass_dir, exist_ok=True)
    summary = os.path.join(pass_dir, "summary.json")
    results = json.load(open(summary)).get("results", []) if os.path.exists(summary) else []
    if a.pick_only:
        for name in seeds:
            for so in sorted(glob.glob(os.path.join(pass_dir, name + "*"))):
                if os.path.isdir(os.path.join(so, "roll")):
                    say(name, json.dumps(pick(so, a.offsets).get("matched")))
        return 0
    if not a.exe or not os.path.isfile(a.exe):
        raise SystemExit("--exe <snapshot>/SupermanReturns.exe is required")
    # Holds of at most --hold-size seeds, released between holds; a hold also ends EARLY when another lane's
    # reservation is pending (orchestrator 2026-10-18: never hold across a pending reservation) - the next acquire
    # then waits behind it (tools/game_slot.py), and the remaining seeds follow in the next hold.
    queue = list(seeds)
    hold = 0
    while queue:
        hold += 1
        while os.path.exists(xsp.QUIET):
            say("QUIET_WINDOW exists - waiting")
            time.sleep(60)
        if not a.no_slot and slot("acquire", "step4 ours seed pass hold %d (%d seeds left)" % (hold, len(queue))) != 0:
            raise SystemExit("could not take the game slot")
        hb_stop = threading.Event()

        def hb():
            while not hb_stop.wait(300):
                subprocess.run([sys.executable, os.path.join(TOOLS, "game_slot.py"), "heartbeat", "--owner", OWNER],
                               cwd=REPO, capture_output=True)
        threading.Thread(target=hb, daemon=True).start()
        if hold == 1 and a.expect_sha:
            # LANE_RULES: count the snapshot's files and hash the exe before use (inside the hold: no disk read
            # while another lane's game runs)
            exe_dir = os.path.dirname(os.path.abspath(a.exe))
            nfiles = sum(len(fs) for _d, _s, fs in os.walk(exe_dir))
            h = xsp.sha(a.exe)
            snap = {"exe": os.path.abspath(a.exe), "sha256": h, "files": nfiles, "when": time.strftime("%H:%M:%S")}
            with open(os.path.join(pass_dir, "snapshot_check.json"), "w") as f:
                json.dump(snap, f, indent=1)
            say("snapshot", json.dumps(snap))
            if not h.lower().startswith(a.expect_sha.lower()) or (a.expect_files and nfiles != a.expect_files):
                if not a.no_slot:
                    slot("release")
                raise SystemExit("snapshot check FAILED: %s (expected sha %s..., %s files)"
                                 % (snap, a.expect_sha, a.expect_files))
        done_here = 0
        try:
            while queue and done_here < a.hold_size:
                if os.path.exists(xsp.QUIET):
                    say("QUIET_WINDOW appeared - ending the hold")
                    break
                pend = xsp.pending_reservation()
                if done_here and pend:
                    say("reservation pending for %s - ending the hold early" % pend)
                    break
                name = queue.pop(0)
                say("seed", name)
                r = run_seed(a, name, pass_dir)
                done_here += 1
                results = [x for x in results if x.get("seed") != name] + [r]
                with open(summary, "w") as f:
                    json.dump({"results": results}, f, indent=1)
                say("seed", name, r["verdict"], r.get("reason", ""), "landing", r.get("landing"),
                    "matched", {k: v.get("at_s") for k, v in (r.get("matched") or {}).items()})
        finally:
            hb_stop.set()
            if not a.no_slot:
                slot("release")
        if queue and a.gap:
            say("slot released; %d s gap for other lanes" % a.gap)
            time.sleep(a.gap)
    return 0 if results and all(r["verdict"] == "PASS" for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
