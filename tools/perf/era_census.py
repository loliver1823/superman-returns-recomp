#!/usr/bin/env python3
"""era_census.py - C1 stage 1's per-era census (docs/RELEASE_COMPOSITION.md 6 "Stage 1"; the stage plan 9.9): on census
rolls of the DEVELOPMENT build (SR_NATIVE_BRACKETS=1, ended by the graceful quit), pm4 = 0 and gaps = 0 in EVERY era
of each run FROM BOOT, no ring packet / kind-1 / BS item, and the ZPD / EXT queries counted (p2.zpd's answer) - the
precondition of the stage-1 cut, under which the release configuration stops on a literal-PM4 gap, a kind-1 item or a
ring packet.

    python tools/perf/era_census.py <kind>:<roll> [<kind>:<roll> ...] [--boot-frames 2] [--no-graceful]
           [--zpd report|zero] [--require <kind>=<era>,<era>,...] [--exe-sha256 <hex>] [--json out.json]

<roll> = a perf_run --out folder (drive/attempt1/run.log; else the newest *.log in the folder - the title scenario's
copied log) or a log file. <kind> picks the era table: title | newgame | save | seed01 | ending.

WHY NOT transport_census.py: it grades one window from the ingame marker (a roll with none is an error), reads no gap,
item or query counter. This tool reuses its parser and its COVERAGE WITNESS (transport_census.read_roll /
coverage_state: a dark or unwitnessed census is no data, never a zero) and adds the whole-run, per-era view.

WHOLE RUN = THE TERMINAL SNAPSHOT (Astra round 1 on release/emu-stage1, P2-2). The run's end is covered only by the
graceful quit's terminal snapshot (tools/perf/terminal_snapshot.py: the request line, the complete line and the quit
receipt of a clean exit 0). Every whole-run counter is read from its family's TERMINAL line INSIDE that snapshot and
nowhere else: [GPU_CP] terminal (C6, zpd_agree), [NATIVE_FLUSH] terminal seg (C5), [NATIVE_FLUSH] terminal draws ...
spans (C5), [PM4_COV] CUM terminal and ENDS terminal (C2, the frame count), [NATIVE_METER_RUN] terminal (C2; required
when the meter printed any line). A family with no terminal line in the snapshot, a terminal line that does not parse,
or two terminal lines of one family that disagree is an error - never a fallback to an earlier (periodic) report. The
frame counts must reconcile with the complete line's frames=N swaps=S vdswaps=V tail=T: the last F record, CUM
terminal frames, ENDS terminal frames (and armed terminal frames when printed) = N + T; [GPU_CP] terminal swaps = S;
[NATIVE_METER_RUN] terminal frames = V - 1 (the meter starts at the first VdSwap). Any of these failing = C9 false:
INCOMPLETE (a census that cannot be read whole), never PASS. --no-graceful (information only, rolls without the
snapshot): the last parseable periodic lines are read and the verdict is at best INCOMPLETE.

ERAS (sequential; each starts at the FIRST matching line after the previous era's start; a census frame belongs to the
era its F line's timestamp falls in - the F line is printed at the frame's end):
  boot         F frames 1 .. --boot-frames (default 2: frame 1 = the device creation's work before the first present,
               frame 2 = the first Swap), whatever their time
  intro        the frames after boot until the title marker (the intro movies)
  title        [APT_REQ] #n path='screens/bootflow/ScrPressStart'
  fe_menu      [SR_FSCMD] ... claim LoadCreate (the custom front end's MAIN MENU)
  newgame:  loading ([SR_FSCMD] ... answer cppId=.. button=0), opening_fmv ([APT_REQ] ... 'screens/hud/main_hud'),
            ingame (perf_report.INGAME_START after it); windows pause_<n>: [APT_SCREEN] ... ScrPauseMenu .. the next
            [PC_CONTROLS] context gameplay
  save / seed01 / ending: load_menu ([SR_FSCMD] ... answer cppId=.. button=1), loading ([SR_FSCMD] ... SR_SaveEvent
            'load'), landing ([APT_REQ|APT_SCREEN] ... main_hud or frontend/ScrMainMenu), ingame ([PC_CONTROLS] context
            gameplay ... mode <a mode> after the landing); windows cinematic_<n>: every [MOVIE_PACE] EOM record of >= 2
            frames ending after the landing, [end - wall, end]
  title:    window attract = the first [MOVIE_PACE] EOM record starting >= 30 s after the title marker, before any fe_menu
            marker
  MOVIES (Astra round 1, P2-3): only an EOM summary places a movie in time - it prints at the movie's end, so its
  interval is [ts - wall, ts]. A GAP summary prints when the NEXT movie starts (> 2 s after the last frame-get, kernel
  cs_timeout_underflow_fix.cpp moviePaceObserve), so its interval is unknown: it opens no window and identifies nothing
  (listed as unplaced). A cinematic is IDENTIFIED by its EOM record's frames and media length (CINEMATICS: the frames a
  movie consumes to its EOM and its media span are properties of the file - CinematicLoisAsks read frames=3248
  media=116.37 s in all four rolls that played it; there is no catch-up drop).
  seed01:   warworld = CinematicLoisAsks identified (an EOM after the landing), THEN the chapter boundary's save
            ([SR_FSCMD] ... SR_SaveEvent 'save' after its end), AND the destination: the roll's own saved game at its
            end (the content root of its launch provenance, SR_CONTENT_DIR) reads chapter 2 (Warworld) with more play
            time than the seed's (save_start.json: chapter 1) - written in this roll; the era starts at the first
            gameplay-context line with a mode after that save event
  ending:   the dev tool's '[DEVTOOL] skip_chapter ticket=1 start chapter=C' after the landing, C = 18 (seed 18) or 17
            (seed 17: ticket 1 replays make2's ticket 16 - CinematicSupesDecides, the chapter boundary, TWSobj04, the
            ending - and a second ticket takes 18 to 19); ending = the identified CinematicLexIsland EOM record starting
            after it, [end - wall, ...); post_game = from the first gameplay-context line with a mode after that movie's
            end, and ONLY with the progression and the destination: a ticket's '[DEVTOOL] skip_chapter ticket=<n> done
            chapter A -> N' (N >= 19; a timeout or a stop is no progression) printed after the movie's end, AND the
            roll's saved game at its end reading chapter >= 19 with more play time than the seed's (chapter C)
  Required eras (C4, --require overrides): title: boot,intro,title,attract | newgame: boot,intro,title,fe_menu,
  opening_fmv,ingame | save: boot,intro,title,fe_menu,load_menu,landing,ingame | seed01: save's without ingame (the
  seed lands in the HUD and the cinematic starts at once: its first gameplay context with a mode is Warworld's) +
  cinematic_1,warworld | ending: save's + ending,post_game. A window is reached when it holds >= 1 census frame.

CHECKS (per roll; each a JSON boolean or null = no evidence):
  C0 usable     transport_census coverage live AND the armed witness says brackets=1 - else NO_CENSUS (exit 2)
  C1 frames     F records 1 .. last contiguous, each parsing in full
  C2 run_zero   the F lines' packets summed from frame 1 == 0, the terminal [PM4_COV] CUM report's pk == 0 (cumulative
                from boot), and the terminal [NATIVE_METER_RUN] pm4 == 0 when the meter ran
  C3 era_zero   every era's and window's F packets == 0 (the first non-zero frame and its keys are reported)
  C4 reached    every required era / window holds >= 1 census frame (else INCOMPLETE)
  C5 gaps_zero  the terminal [NATIVE_FLUSH] seg line: gapApplied == 0 and gapKB == 0 (cumulative from boot: the
                literal-PM4 gap parts the CP executed - the census's `gaps`), idMiss / mismatch / footStray / feetLost /
                capDrop / litLenBad 0; the terminal '... draws: ... | spans rec= pm4= litLenBad=' pm4 == 0. And the
                cross-check gaps_agree: gapApplied == 0 exactly when C2's packet sum == 0 (a gap's bytes ARE the packets
                the census counts) - a disagreement fails. No readable terminal line: null (INCOMPLETE, never a pass)
  C6 items_zero the terminal [GPU_CP] line: packets == 0 (ring packets), ib == seg (kind-1 plain-IB items = ib - seg),
                bs == 0, both desync 0, badIB 0
  C7 zpd        per era the F ops 5B (EVENT_WRITE_ZPD) / 5A (EVENT_WRITE_EXT); the terminal [GPU_CP] zpd= / ext= must
                equal the F sums (zpd_agree, else FAIL); --zpd zero: zpd + ext == 0 is required (default: reported only).
                p2.zpd's bake (native/zpd-bake: SR_NATIVE_ZPD on, the log's "[NATIVE_ZPD] SR_NATIVE_ZPD on" line): the
                query's EVENT_WRITE_ZPD travels as a kRkZpd RECORD, which the CP counts in zpd= too (RecordEngine::
                applyZpdEvent) and the F lines do not (no packet) - so there the F sum is a lower bound: zpd >= F 5B, the
                difference reported as the records (zpd_records); ext= stays exact (the extent query has no record)
  C8 waits      the [GPU_CP] waits= (blocked N) at the end of each era and of the run: reported only
  C9 terminal   the terminal snapshot complete (with the quit receipt of a clean exit) and every whole-run family read
                inside it, parsed and reconciled (above) - else INCOMPLETE
  C10 identity  the roll's LAUNCH-TIME exe identity (Astra round 1, P2-4): provenance.json beside the log (sr_drive
                writes it before it launches: exeSha256 of the exe it started), bound to the log's own process (the
                provenance pid = the pid in the log's '[QUIT] channel Local\\SR_QUIT_<instance>_<pid> ready' line; a log
                without that line: the provenance's startedUtc within 300 s before the log's first line). No provenance,
                an unreadable one or one that does not bind: null (INCOMPLETE). --exe-sha256: a different hash FAILs.
                Across rolls (one_exe): every identity present and equal - a difference FAILs, an absent one is
                INCOMPLETE. (The title scenario's run_recomp_x launch writes no provenance: such a roll is INCOMPLETE.)
Exit: 1 FAIL when a check failed in any roll (the JSON is written); else 2 NO_CENSUS when a roll has no usable census;
else 3 INCOMPLETE; else 0 PASS. Read-only apart from --json.
"""
import argparse
import datetime
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import perf_report  # noqa: E402
import perf_run  # noqa: E402  (find_saves / save_slots: the saved-game container reader)
import terminal_snapshot as tsnap  # noqa: E402
import transport_census as tc  # noqa: E402

GPUCP = re.compile(r"\[(?:GPU_CP|NATIVE_SUBMIT)\] (?P<why>\w+) rptr=\d+ wptr=\d+ doorbells=(?P<doorbells>\d+) packets=(?P<packets>\d+) "
                   r"draws=\d+ isr=\d+ \(faults \d+\) swaps=(?P<swaps>\d+) waits=(?P<waits>\d+) \(blocked (?P<blocked>\d+)\) "
                   r"scratch=\d+ memWrites=\d+ zpd=(?P<zpd>\d+) \(fake (?P<zpd_fake>\d+)\) ext=(?P<ext>\d+) "
                   r"badIB=(?P<badIB>\d+) desync=(?P<desync>\d+) \| ringless ib=(?P<ib>\d+) bs=(?P<bs>\d+) "
                   r"desync=(?P<rl_desync>\d+) seg=(?P<seg>\d+)")
SEG = re.compile(r"\[NATIVE_FLUSH\] (?:(\w+) )?seg closed=(\d+) parts=(\d+) gapKB=(\d+) recApplied=(\d+) gapApplied=(\d+) "
                 r"idMiss=(\d+) mismatch=(\d+) .*?footStray=(\d+) feetLost=(\d+) .*?capDrop=(\d+) litLenBad=(\d+)")
SPANS = re.compile(r"\[NATIVE_FLUSH\] (\w+) draws: .*\| spans rec=(\d+) pm4=(\d+) litLenBad=(\d+)")
METER_RUN = re.compile(r"\[NATIVE_METER_RUN\] (\w+) ([\d.]+)s frames=(\d+) \| pm4=(\d+)")
ARMED_T = re.compile(r"\[PM4_COV\] armed terminal brackets=\d frames=(\d+)")
MOVIE = re.compile(r"\[MOVIE_PACE\] movie#(\d+) (\S+) wall=([\d.]+)s frames=(\d+) .*?\bmedia=([\d.]+)s")
SKIP_START = re.compile(r"\[DEVTOOL\] skip_chapter ticket=1 start chapter=(-?\d+)")
SKIP_END = re.compile(r"\[DEVTOOL\] skip_chapter ticket=(\d+) (\w+) chapter (-?\d+) -> (-?\d+) actions=\d+")
CHANNEL = re.compile(r"\[QUIT\] channel Local\\SR_QUIT_\S+?_(\d+) ready")
# The whole-run families: the prefix of the family's terminal line -> its parser. Each must print exactly what its
# parser reads; a prefix match that does not parse is MALFORMED (never skipped).
TERMINAL_FAMILIES = (
    ("cp", "[GPU_CP] terminal ", GPUCP),
    ("cp", "[NATIVE_SUBMIT] terminal ", GPUCP),
    ("seg", "[NATIVE_FLUSH] terminal seg closed=", SEG),
    ("spans", "[NATIVE_FLUSH] terminal draws:", SPANS),
    ("cum", "[PM4_COV] CUM terminal ", tc.CUM),
    ("ends", "[PM4_COV] ENDS terminal ", tc.ENDS),
    ("armed", "[PM4_COV] armed terminal ", ARMED_T),
    ("meter", "[NATIVE_METER_RUN] terminal ", METER_RUN),
)
REQUIRED_FAMILIES = ("cp", "seg", "spans", "cum", "ends")  # + meter when the meter printed any line
MARKERS = {
    "title": re.compile(r"\[APT_REQ\] #\d+ path='screens/bootflow/ScrPressStart'"),
    "fe_menu": re.compile(r"\[SR_FSCMD\].*claim LoadCreate"),
    "loading_new": re.compile(r"\[SR_FSCMD\].*answer cppId=\S+ button=0"),
    "main_hud": re.compile(r"\[APT_REQ\].*path='screens/hud/main_hud'"),
    "ingame_new": re.compile(perf_report.INGAME_START),
    "pause": re.compile(r"\[APT_SCREEN\].*path='screens/misc/ScrPauseMenu'"),
    "context": re.compile(r"\[PC_CONTROLS\] context gameplay "),
    "load_menu": re.compile(r"\[SR_FSCMD\].*answer cppId=\S+ button=1"),
    "loading_save": re.compile(r"\[SR_FSCMD\].*SR_SaveEvent 'load'"),
    "landing": re.compile(r"\[APT_(?:REQ|SCREEN)\].*path='screens/(?:hud/main_hud|frontend/ScrMainMenu)'"),
    "context_mode": re.compile(r"\[PC_CONTROLS\] context gameplay \(igc type \d+\) config \S+ invX=\d+ invY=\d+ mode [a-z]"),
    "save_event": re.compile(r"\[SR_FSCMD\] #\d+ SR_SaveEvent 'save'"),
}
# Cinematics identified by their EOM record (module doc, MOVIES): the frames the movie consumed and its media span.
# CinematicLoisAsks (MOVIE_Q name '0500'): frames=3248 media=116.37 s in dev/archive/saves/_rolls make1_ch1-5 movie#9,
# make2_ch1-19 movie#8 and the two seed-01 verify rolls (_verify/01_*_10142159 and _10142258, movie#7).
# CinematicLexIsland (the ending, '2600'): frames=7156 media=245.80 s in make2_ch1-19 movie#20 - the movie the game
# plays after the dev tool passed 22 TWSobj04 (ticket 16, kernel/devtools_levelskip.cpp kSpine: TWSobj04 ->
# CinematicLexIsland, the spine's last row).
CINEMATICS = {
    "CinematicLoisAsks": {"movie": "0500", "frames": 3248, "media": 116.37},
    "CinematicLexIsland": {"movie": "2600", "frames": 7156, "media": 245.80},
}
FRAMES_TOL, MEDIA_TOL = 0.005, 0.25  # 0.5 % of the frames, 0.25 s of media
# The kinds whose last era needs a progression: the cinematic, the chapter the seed starts in and the destination.
DEST = {
    "seed01": {"cinematic": "CinematicLoisAsks", "era": "warworld", "from": (1,), "to": 2, "exact": True, "skip": False},
    # the ending from chapter 18 (seed 18) or 17 (seed 17: ticket 1 replays make2's ticket 16 - CinematicSupesDecides,
    # the chapter boundary, TWSobj04, the ending - and a second ticket takes chapter 18 to 19)
    "ending": {"cinematic": "CinematicLexIsland", "era": "post_game", "from": (17, 18), "to": 19, "exact": False,
               "skip": True},
}
# kind -> the sequential eras after intro: (era name, marker key)
SEQ = {
    "title": [("title", "title"), ("fe_menu", "fe_menu")],
    "newgame": [("title", "title"), ("fe_menu", "fe_menu"), ("loading", "loading_new"), ("opening_fmv", "main_hud"),
                ("ingame", "ingame_new")],
}
SEQ["save"] = [("title", "title"), ("fe_menu", "fe_menu"), ("load_menu", "load_menu"), ("loading", "loading_save"),
               ("landing", "landing"), ("ingame", "context_mode")]
SEQ["seed01"] = list(SEQ["save"])
SEQ["ending"] = list(SEQ["save"])
REQUIRED = {
    "title": ["boot", "intro", "title", "attract"],
    "newgame": ["boot", "intro", "title", "fe_menu", "opening_fmv", "ingame"],
    "save": ["boot", "intro", "title", "fe_menu", "load_menu", "landing", "ingame"],
}
# seed 01 lands in the HUD and CinematicLoisAsks starts at once: its first gameplay context with a mode is Warworld's
# (both seed-01 verify rolls: ingame and warworld start on the same line), so ingame is not required apart from it.
REQUIRED["seed01"] = [e for e in REQUIRED["save"] if e != "ingame"] + ["cinematic_1", "warworld"]
REQUIRED["ending"] = REQUIRED["save"] + ["ending", "post_game"]
KINDS = tuple(SEQ)
SEC = datetime.timedelta(seconds=1)


def roll_log(roll):
    """The roll's log: a file, a perf_run folder's drive/attempt1/run.log, else the newest *.log in the folder."""
    if os.path.isfile(roll):
        return roll
    p = os.path.join(roll, "drive", "attempt1", "run.log")
    if os.path.isfile(p):
        return p
    logs = sorted(glob.glob(os.path.join(roll, "*.log")), key=os.path.getmtime)
    return logs[-1] if logs else p


def roll_dir(roll, log):
    """The perf_run --out folder: the roll when it is a folder, else the folder two above a drive/attempt<N>/ log."""
    if os.path.isdir(roll):
        return roll
    d = os.path.dirname(os.path.abspath(log))
    if re.fullmatch(r"attempt\d+", os.path.basename(d)) and os.path.basename(os.path.dirname(d)) == "drive":
        return os.path.dirname(os.path.dirname(d))
    return d


def ts_of(s):
    return tc.parse_ts(s) if s else None


def scan(log):
    """One pass: the GPU_CP / seg / spans / meter lines (dicts with ts), every terminal family line, the movie records,
    the dev tool's chapter lines, the QUIT channel's pid and every marker's lines."""
    out = {"gpucp": [], "seg": [], "spans": [], "meter": [], "movies": [], "marks": {k: [] for k in MARKERS},
           "terminal": [], "skip_start": [], "skip_end": [], "channel_pid": None, "first_ts": None, "f_last": None,
           "zpd_native": False}
    with open(log, encoding="utf-8", errors="replace") as f:
        for ln, line in enumerate(f):
            t = tc.TS.match(line)
            ts = t.group(1) if t else None
            if ts and out["first_ts"] is None:
                out["first_ts"] = ts
            if "terminal" in line:
                for fam, prefix, rx in TERMINAL_FAMILIES:
                    if prefix in line:
                        m = rx.search(line)
                        out["terminal"].append({"fam": fam, "ln": ln, "ts": ts, "m": m})
                        break
            if "[GPU_CP] " in line or "[NATIVE_SUBMIT] " in line:
                g = GPUCP.search(line)
                if g:
                    d = {k: (v if k == "why" else int(v)) for k, v in g.groupdict().items()}
                    d.update(ln=ln, ts=ts)
                    out["gpucp"].append(d)
                continue
            if "[NATIVE_FLUSH] " in line:
                s = SEG.search(line)
                if s:
                    out["seg"].append(seg_dict(s, ln, ts))
                    continue
                sp = SPANS.search(line)
                if sp:
                    out["spans"].append(spans_dict(sp, ln, ts))
                continue
            if "[NATIVE_METER_RUN] " in line:
                m = METER_RUN.search(line)
                out["meter"].append({"ln": ln, "ts": ts, "why": m.group(1) if m else None,
                                     "frames": int(m.group(3)) if m else None, "pm4": int(m.group(4)) if m else None})
                continue
            if "[NATIVE_ZPD] SR_NATIVE_ZPD on" in line:  # p2.zpd's native path (records) on: module doc, C7
                out["zpd_native"] = True
                continue
            if "[QUIT] channel " in line and out["channel_pid"] is None:
                c = CHANNEL.search(line)
                if c:
                    out["channel_pid"] = int(c.group(1))
                continue
            if "[DEVTOOL] skip_chapter ticket=" in line and ts:
                s = SKIP_START.search(line)
                if s:
                    out["skip_start"].append({"ln": ln, "ts": ts, "chapter": int(s.group(1))})
                    continue
                e = SKIP_END.search(line)
                if e:
                    out["skip_end"].append({"ln": ln, "ts": ts, "ticket": int(e.group(1)), "how": e.group(2),
                                            "from": int(e.group(3)), "to": int(e.group(4))})
                continue
            mv = MOVIE.search(line)
            if mv and ts:
                out["movies"].append({"ln": ln, "end": ts, "n": int(mv.group(1)), "kind": mv.group(2),
                                      "wall": float(mv.group(3)), "frames": int(mv.group(4)),
                                      "media": float(mv.group(5))})
            for k, rx in MARKERS.items():
                if rx.search(line):
                    out["marks"][k].append((ln, ts))
    return out


def seg_dict(s, ln, ts):
    v = [int(x) for x in s.groups()[1:]]
    return {"ln": ln, "ts": ts, "why": s.group(1) or "", "closed": v[0], "gapKB": v[2], "gapApplied": v[4],
            "idMiss": v[5], "mismatch": v[6], "footStray": v[7], "feetLost": v[8], "capDrop": v[9], "litLenBad": v[10]}


def spans_dict(sp, ln, ts):
    return {"ln": ln, "ts": ts, "why": sp.group(1), "rec": int(sp.group(2)), "pm4": int(sp.group(3)),
            "litLenBad": int(sp.group(4))}


def family_value(fam, m, ln, ts):
    """A parsed terminal line -> the dict the checks read."""
    if fam == "cp":
        d = {k: (v if k == "why" else int(v)) for k, v in m.groupdict().items()}
        d.update(ln=ln, ts=ts)
        return d
    if fam == "seg":
        return seg_dict(m, ln, ts)
    if fam == "spans":
        return spans_dict(m, ln, ts)
    if fam == "cum":
        return {"ln": ln, "ts": ts, "frames": int(m.group(2)), "pk": int(m.group(3))}
    if fam == "ends":
        return {"ln": ln, "ts": ts, "frames": int(m.group(2)), "presents": int(m.group(5))}
    if fam == "armed":
        return {"ln": ln, "ts": ts, "frames": int(m.group(1))}
    if fam == "meter":
        return {"ln": ln, "ts": ts, "frames": int(m.group(3)), "pm4": int(m.group(4))}
    raise ValueError(fam)


def terminal_read(sc, snap, f_last, meter_ran):
    """The whole-run families inside the complete snapshot (module doc, WHOLE RUN): ({family: value}, problems)."""
    probs = []
    if not tsnap.is_complete(snap):
        return {}, ["no complete terminal snapshot (%s)" % tsnap.summary(snap)]
    lo, hi = snap["request_line"], snap["complete_line"]  # 1-based; a family line lies strictly between them
    fams = {}
    for x in sc["terminal"]:
        n = x["ln"] + 1
        if not lo < n < hi:
            if n > hi:
                probs.append("a terminal %s line after the snapshot's complete line (line %d)" % (x["fam"], n))
            continue
        if x["m"] is None:
            probs.append("the terminal %s line %d does not parse" % (x["fam"], n))
            continue
        v = family_value(x["fam"], x["m"], x["ln"], x["ts"])
        prev = fams.get(x["fam"])
        if prev is not None and {k: w for k, w in prev.items() if k not in ("ln", "ts")} != \
                {k: w for k, w in v.items() if k not in ("ln", "ts")}:
            probs.append("two terminal %s lines disagree (lines %d and %d)" % (x["fam"], prev["ln"] + 1, n))
        fams[x["fam"]] = v
    need = list(REQUIRED_FAMILIES) + (["meter"] if meter_ran else [])
    for fam in need:
        if fam not in fams and not any(p.startswith("the terminal %s line" % fam) for p in probs):
            probs.append("no terminal %s line inside the snapshot" % fam)
    want = snap["frames"] + (snap["tail"] or 0)
    for fam in ("cum", "ends", "armed"):
        if fam in fams and fams[fam]["frames"] != want:
            probs.append("%s terminal frames=%d, the snapshot's frames + tail = %d" % (fam, fams[fam]["frames"], want))
    if f_last != want:
        probs.append("the last F record is frame %s, the snapshot's frames + tail = %d" % (f_last, want))
    if "cp" in fams and fams["cp"]["swaps"] != snap["swaps"]:
        probs.append("[GPU_CP] terminal swaps=%d, the snapshot's swaps=%d" % (fams["cp"]["swaps"], snap["swaps"]))
    if "meter" in fams and fams["meter"]["frames"] != snap["vdswaps"] - 1:
        probs.append("[NATIVE_METER_RUN] terminal frames=%d, the snapshot's vdswaps - 1 = %d" % (
            fams["meter"]["frames"], snap["vdswaps"] - 1))
    return fams, probs


def first_after(marks, ln_min):
    return next(((ln, ts) for ln, ts in marks if ln > ln_min and ts), None)


def identify(m):
    """An EOM movie record -> the CINEMATICS name its frames and media match, else None (a GAP record: None)."""
    if m["kind"] != "EOM":
        return None
    for name, fp in CINEMATICS.items():
        if abs(m["frames"] - fp["frames"]) <= max(2, FRAMES_TOL * fp["frames"]) and \
                abs(m["media"] - fp["media"]) <= MEDIA_TOL:
            return name
    return None


def roll_save(rdir, prov):
    """The roll's saved game at its end and the seed it started from: {seed_chapter, seed_play, chapter, play, file}
    or {error}. The content root is the launch provenance's SR_CONTENT_DIR (sr_drive's attempt content), else
    <roll>/drive/attempt1/content; the seed is perf_run's <roll>/save_start.json."""
    out = {}
    try:
        with open(os.path.join(rdir, "save_start.json"), encoding="utf-8-sig") as f:
            st = json.load(f)
        out["seed_chapter"], out["seed_play"] = int(st["chapter"]), float(st["TotalPlayTime"])
    except (OSError, ValueError, KeyError, TypeError):
        return {"error": "no readable save_start.json in %s (the seed of --from-save)" % rdir}
    env = (prov or {}).get("effectiveSrEnv") or {}
    root = env.get("SR_CONTENT_DIR")
    if not (isinstance(root, str) and os.path.isdir(root)):  # (a roll folder moved since: its own attempt content)
        root = os.path.join(rdir, "drive", "attempt1", "content")
    saves = perf_run.find_saves(root)
    if not saves:
        return dict(out, error="no saved game under the roll's content root %s" % root)
    try:
        used = [x for x in perf_run.save_slots(saves[0][1]) if not x["empty"]]
    except ValueError as e:
        return dict(out, error=str(e))
    if not used:
        return dict(out, error="%s: every slot is empty" % saves[0][1])
    pick = max(used, key=lambda x: (x["TotalPlayTime"], x["slot"]))
    return dict(out, chapter=int(pick["chapter"]), play=float(pick["TotalPlayTime"]), file=saves[0][1])


def progression(kind, sc, start, movies, rdir, prov):
    """seed01 / ending (module doc): {ok, era, t, ln, why, evidence}; t / ln = the destination era's start."""
    dest = DEST[kind]
    ev = {}
    res = {"ok": False, "era": dest["era"], "t": None, "ln": None, "evidence": ev, "ending_t0": None,
           "ending_ln": None}
    land_t, land_ln = start["landing"]
    t_min = land_t
    if dest["skip"]:
        sk = next((s for s in sc["skip_start"] if s["ln"] > land_ln), None)
        if sk is None:
            res["why"] = "no '[DEVTOOL] skip_chapter ticket=1 start chapter=..' after the landing"
            return res
        ev["skip_start"] = sk
        if sk["chapter"] not in dest["from"]:
            res["why"] = "the dev tool started at chapter %d, not %s" % (sk["chapter"], " / ".join(map(str, dest["from"])))
            return res
        t_min = ts_of(sk["ts"])
    cine = next((m for m in movies if m["t0"] > t_min and m.get("cinematic") == dest["cinematic"]), None)
    if cine is None:
        res["why"] = "no %s EOM record (frames %d, media %.2f s) after the %s" % (
            dest["cinematic"], CINEMATICS[dest["cinematic"]]["frames"], CINEMATICS[dest["cinematic"]]["media"],
            "dev tool's start" if dest["skip"] else "landing")
        return res
    ev["cinematic"] = {k: cine[k] for k in ("n", "end", "wall", "frames", "media", "cinematic")}
    res["ending_t0"], res["ending_ln"] = cine["t0"], cine["ln"]
    if dest["skip"]:
        # the progression: a ticket's "done chapter A -> B" (B >= the destination) printed after the movie's end; a
        # timeout or a stop is none (every ticket end after the dev tool's start is kept as evidence)
        ends = [e for e in sc["skip_end"] if e["ln"] > ev["skip_start"]["ln"]]
        ev["skip_ends"] = ends
        done = next((e for e in ends if e["ln"] > cine["ln"] and e["how"] == "done" and e["to"] > e["from"] and
                     e["to"] >= dest["to"]), None)
        ev["skip_end"] = done
        if done is None:
            res["why"] = "no successful progression to chapter %d after the %s's end (ticket ends: %s)" % (
                dest["to"], dest["cinematic"], ", ".join("#%d %s %d -> %d" % (e["ticket"], e["how"], e["from"], e["to"])
                                                       for e in ends) or "none")
            return res
    else:
        sv = first_after(sc["marks"]["save_event"], cine["ln"])
        ev["save_event"] = sv
        if sv is None:
            res["why"] = "no [SR_FSCMD] SR_SaveEvent 'save' after the %s's end (the chapter boundary's save)" % (
                dest["cinematic"])
            return res
    rs = roll_save(rdir, prov)
    ev["saved_game"] = rs
    if "error" in rs:
        res["why"] = "no destination evidence: " + rs["error"]
        return res
    want_seed = ev["skip_start"]["chapter"] if dest["skip"] else None
    if rs["seed_chapter"] not in dest["from"] or (want_seed is not None and rs["seed_chapter"] != want_seed):
        res["why"] = "the seed is chapter %d, not %s" % (rs["seed_chapter"], want_seed if want_seed is not None else
                                                        " / ".join(map(str, dest["from"])))
        return res
    if "chapter" not in rs or not (rs["chapter"] == dest["to"] if dest["exact"] else rs["chapter"] >= dest["to"]) \
            or not rs["play"] > rs["seed_play"]:
        res["why"] = "no destination evidence: the roll's saved game reads chapter %s, play time %s (the seed: %d, %s)" % (
            rs.get("chapter"), rs.get("play"), rs["seed_chapter"], rs["seed_play"])
        return res
    after = cine["ln"] if dest["skip"] else ev["save_event"][0]
    hit = first_after(sc["marks"]["context_mode"], after)
    if hit is None:
        res["why"] = "no gameplay-context line with a mode after the %s" % (
            "ending" if dest["skip"] else "chapter boundary's save")
        return res
    res.update(ok=True, t=ts_of(hit[1]), ln=hit[0], why="")
    return res


def eras_and_windows(kind, sc, rdir=None, prov=None):
    """[(era, start datetime, line)] in order after boot / intro, the windows {name: (t0, t1)}, the progression
    ({} for the kinds without one) and the unplaced (GAP) movie records."""
    eras, last_ln = [], -1
    for name, key in SEQ[kind]:
        hit = first_after(sc["marks"][key], last_ln)
        if hit:
            eras.append((name, ts_of(hit[1]), hit[0]))
            last_ln = hit[0]
    start = {e[0]: (e[1], e[2]) for e in eras}
    windows = {}
    movies, unplaced = [], []
    for m in sc["movies"]:
        if m["frames"] < 2:
            continue
        if m["kind"] != "EOM":
            unplaced.append({k: m[k] for k in ("n", "kind", "end", "wall", "frames", "media")})
            continue
        t1 = ts_of(m["end"])
        movies.append(dict(m, t1=t1, t0=t1 - m["wall"] * SEC, cinematic=identify(m)))
    if kind == "title" and "title" in start:
        fe = start.get("fe_menu", (None, None))[0]
        att = next((m for m in movies if m["t0"] >= start["title"][0] + 30 * SEC and (fe is None or m["t0"] < fe)), None)
        if att:
            windows["attract"] = (att["t0"], att["t1"])
    if kind == "newgame":
        ctx = sc["marks"]["context"]
        for i, (ln, ts) in enumerate(sc["marks"]["pause"]):
            end = first_after(ctx, ln)
            windows["pause_%d" % (i + 1)] = (ts_of(ts), ts_of(end[1]) if end else None)
    prog = {}
    if kind in ("save", "seed01", "ending") and "landing" in start:
        land = start["landing"][0]
        cine = [m for m in movies if m["t1"] > land]
        for i, m in enumerate(cine):
            windows["cinematic_%d" % (i + 1)] = (m["t0"], m["t1"])
        if kind in DEST:
            prog = progression(kind, sc, start, movies, rdir or ".", prov)
            if kind == "ending" and prog.get("ending_t0") is not None and prog["ok"]:
                eras.append(("ending", prog["ending_t0"], prog["ending_ln"]))
            if prog["ok"]:
                eras.append((prog["era"], prog["t"], prog["ln"]))
    elif kind in DEST:
        prog = {"ok": False, "era": DEST[kind]["era"], "why": "no landing marker", "evidence": {}}
    eras.sort(key=lambda e: e[1])
    return eras, windows, prog, unplaced


def frame_zero(keys):
    return {k: v for k, v in (keys or {}).items() if v}


def read_provenance(log):
    """provenance.json beside the log (sr_drive's attempt folder) -> (dict, None) or (None, why)."""
    p = os.path.join(os.path.dirname(os.path.abspath(log)), "provenance.json")
    if not os.path.isfile(p):
        return None, "no launch provenance (provenance.json) beside the log"
    try:
        with open(p, encoding="utf-8-sig") as f:
            d = json.load(f)
    except (OSError, ValueError) as e:
        return None, "provenance.json unreadable: %s" % e
    return (d, None) if isinstance(d, dict) else (None, "provenance.json is not an object")


def utc_to_local(s):
    """sr_drive's startedUtc ('2026-10-15T07:38:49.3806933Z') -> a naive local datetime (the log's clock)."""
    m = re.fullmatch(r"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)(?:\.(\d+))?Z?", s or "")
    if not m:
        return None
    t = datetime.datetime.strptime(m.group(1), "%Y-%m-%dT%H:%M:%S")
    t += datetime.timedelta(microseconds=int((m.group(2) or "0")[:6].ljust(6, "0")))
    return t.replace(tzinfo=datetime.timezone.utc).astimezone().replace(tzinfo=None)


def launch_identity(log, sc, prov, why):
    """C10 (module doc): {sha256, bound_by} or {sha256: None, error}."""
    if prov is None:
        return {"sha256": None, "error": why}
    sha = prov.get("exeSha256")
    if not (isinstance(sha, str) and re.fullmatch(r"[0-9A-Fa-f]{64}", sha)):
        return {"sha256": None, "error": "provenance.json has no exeSha256"}
    pid = prov.get("pid")
    if sc["channel_pid"] is not None:
        if str(pid) != str(sc["channel_pid"]):
            return {"sha256": None, "error": "the provenance's pid %s is not the log's process (its quit channel: pid %d)"
                    % (pid, sc["channel_pid"])}
        return {"sha256": sha.lower(), "bound_by": "pid %d (the log's quit channel)" % sc["channel_pid"],
                "exe": prov.get("exePath")}
    started, first = utc_to_local(prov.get("startedUtc")), ts_of(sc["first_ts"])
    if started is None or first is None or not datetime.timedelta(0) <= first - started <= 300 * SEC:
        return {"sha256": None, "error": "the provenance does not bind to the log (no quit channel line; started %s, "
                "the log's first line %s)" % (prov.get("startedUtc"), sc["first_ts"])}
    return {"sha256": sha.lower(), "bound_by": "start time (%.1f s before the log's first line)" % (
        (first - started).total_seconds()), "exe": prov.get("exePath")}


def grade(kind, roll, boot_frames=2, graceful=True, zpd_mode="report", required=None, exe_sha256=None):
    log = roll_log(roll)
    res = {"roll": roll, "kind": kind, "log": log, "verdict": None, "checks": {}, "eras": [], "windows": [],
           "incomplete": []}
    if not os.path.isfile(log):
        res.update(verdict="NO_CENSUS", error="no log")
        return res
    reports, frames, bad, last_ts, cov = tc.read_roll(log)
    state, why, coverage = tc.coverage_state(cov, reports)
    res["coverage"] = {"state": state, "why": why, "brackets": cov["armed"]}
    if state != "live" or cov["armed"] != 1:
        res["checks"]["C0_usable"] = False
        res.update(verdict="NO_CENSUS", error=why if state != "live" else
                   "the armed witness reads brackets=%s, not 1: no producer census (SR_NATIVE_BRACKETS=1)" % cov["armed"])
        return res
    c = res["checks"]
    c["C0_usable"] = True
    sc = scan(log)
    rdir = roll_dir(roll, log)
    prov, prov_why = read_provenance(log)
    nums = sorted(frames)
    last = nums[-1] if nums else 0
    c["C1_frames"] = bool(nums) and not bad and nums == list(range(1, last + 1))
    f_pk = sum(frames[n]["pk"] for n in nums)
    # the whole-run families: the terminal snapshot's (graceful), else the last parseable periodic lines
    if graceful:
        snap = tsnap.read(log)
        res["terminal_snapshot"] = tsnap.summary(snap)
        fams, probs = terminal_read(sc, snap, last, bool(sc["meter"]))
        res["terminal_problems"] = probs
        c["C9_terminal"] = not probs
        if probs:
            res["incomplete"] += ["terminal: " + p for p in probs]
        cum = fams.get("cum")
        seg, spans, g, meter = fams.get("seg"), fams.get("spans"), fams.get("cp"), fams.get("meter")
        cum_pk = cum["pk"] if cum else None
        meter_pm4 = meter["pm4"] if meter else None
        meter_bad = bool(sc["meter"]) and meter is None
    else:
        res["incomplete"].append("--no-graceful: the whole-run counters are the last periodic lines; the run's end "
                                 "is not covered")
        cum_pk = reports[-1]["pk"] if reports else None
        good_meter = [m for m in sc["meter"] if m["pm4"] is not None]
        meter_pm4 = good_meter[-1]["pm4"] if good_meter else None
        meter_bad = False
        seg = sc["seg"][-1] if sc["seg"] else None
        spans = sc["spans"][-1] if sc["spans"] else None
        g = sc["gpucp"][-1] if sc["gpucp"] else None
    c["C2_run_zero"] = None if cum_pk is None or meter_bad else \
        (f_pk == 0 and cum_pk == 0 and (meter_pm4 in (None, 0)))
    # eras
    eras, windows, prog, unplaced = eras_and_windows(kind, sc, rdir, prov)
    res["progression"] = {k: (v.strftime("%H:%M:%S.%f")[:-3] if isinstance(v, datetime.datetime) else v)
                          for k, v in prog.items()} if prog else None
    res["movies_unplaced"] = unplaced
    bounds = [("intro", None, -1)] + eras
    acc = {"boot": []}
    for name, _, _ in bounds:
        acc.setdefault(name, [])
    for n in nums:
        if n <= boot_frames:
            acc["boot"].append(n)
            continue
        ft = ts_of(frames[n]["ts"])
        era = "intro"
        for name, t0, _ in eras:
            if ft is not None and ft >= t0:
                era = name
        acc[era].append(n)
    wacc = {w: [n for n in nums if n > boot_frames and frames[n]["ts"] and t0 <= ts_of(frames[n]["ts"]) and
                (t1 is None or ts_of(frames[n]["ts"]) <= t1)] for w, (t0, t1) in windows.items()}
    gp = sc["gpucp"]

    def cp_at(t):  # the last [GPU_CP] line at / before t (waits / blocked so far)
        hit = None
        for x in gp:
            if x["ts"] and (t is None or ts_of(x["ts"]) <= t):
                hit = x
        return hit

    def era_row(name, ns, t0=None, t1=None):
        pk = sum(frames[n]["pk"] for n in ns)
        nz = next((n for n in ns if frames[n]["pk"]), None)
        z = sum(frames[n]["keys"].get("5B", 0) for n in ns)
        x = sum(frames[n]["keys"].get("5A", 0) for n in ns)
        end = ts_of(frames[ns[-1]]["ts"]) if ns else None
        gg = cp_at(end)
        return {"name": name, "t0": t0.strftime("%H:%M:%S.%f")[:-3] if t0 else (frames[ns[0]]["ts"][11:] if ns else None),
                "frames": len(ns), "first_frame": ns[0] if ns else None, "pk": pk, "zpd": z, "ext": x,
                "first_nonzero": None if nz is None else {"frame": nz, "ts": frames[nz]["ts"], "pk": frames[nz]["pk"],
                                                          "keys": frame_zero(frames[nz]["keys"])},
                "waits_by_end": gg["waits"] if gg else None, "blocked_by_end": gg["blocked"] if gg else None}

    res["eras"] = [era_row("boot", acc["boot"])] + [era_row(name, acc[name], t0) for name, t0, _ in bounds]
    res["windows"] = [dict(era_row(w, wacc[w], windows[w][0]), t1=windows[w][1].strftime("%H:%M:%S.%f")[:-3]
                           if windows[w][1] else None) for w in sorted(windows)]
    rows = res["eras"] + res["windows"]
    c["C3_era_zero"] = all(r["pk"] == 0 for r in rows)
    need = required if required is not None else REQUIRED[kind]
    have = {r["name"] for r in rows if r["frames"]}
    res["missing_eras"] = [e for e in need if e not in have]
    c["C4_reached"] = not res["missing_eras"]
    if prog and not prog.get("ok"):
        res["incomplete"].append("%s not reached: %s" % (prog["era"], prog.get("why")))
    # gaps
    res["whole_run"] = {"f_pk": f_pk, "cum_pk": cum_pk, "meter_pm4": meter_pm4, "frames": last,
                        "seg": seg, "spans": spans, "source": "terminal snapshot" if graceful else "last periodic"}
    if seg is None or spans is None:
        c["C5_gaps_zero"] = None
    else:
        c["C5_gaps_zero"] = (seg["gapApplied"] == 0 and seg["gapKB"] == 0 and spans["pm4"] == 0 and spans["litLenBad"] == 0
                             and all(seg[k] == 0 for k in ("idMiss", "mismatch", "footStray", "feetLost", "capDrop",
                                                            "litLenBad")))
        c["gaps_agree"] = (seg["gapApplied"] == 0) == (f_pk == 0)
    # items
    res["whole_run"]["cp"] = g
    c["C6_items_zero"] = None if g is None else (g["packets"] == 0 and g["ib"] == g["seg"] and g["bs"] == 0 and
                                                 g["desync"] == 0 and g["rl_desync"] == 0 and g["badIB"] == 0)
    fz = sum(r["zpd"] for r in res["eras"])
    fx = sum(r["ext"] for r in res["eras"])
    res["whole_run"]["f_zpd"], res["whole_run"]["f_ext"] = fz, fx
    res["whole_run"]["zpd_native"] = sc["zpd_native"]
    if g is not None and sc["zpd_native"]:
        res["whole_run"]["zpd_records"] = g["zpd"] - fz
    c["zpd_agree"] = None if g is None else ((g["zpd"] >= fz if sc["zpd_native"] else g["zpd"] == fz) and g["ext"] == fx)
    c["C7_zpd_zero"] = (fz + fx == 0) if zpd_mode == "zero" else None
    # producers (cumulative from boot, the last report): who wrote what reached the CP
    prods = []
    if reports:
        for (va, nm), v in sorted(reports[-1]["inner"].items()):
            if v["pk"]:
                name, obligation, _ = tc.PRODUCER_MAP.get(va, tc.PRODUCER_MAP.get(nm, (nm, "UNMAPPED", "")))
                prods.append({"va": va, "name": nm, "pk": v["pk"], "keys": v["keys"], "obligation": obligation})
    res["producers_from_boot"] = prods
    # the launch-time exe identity
    ident = launch_identity(log, sc, prov, prov_why)
    res["exe_identity"] = ident
    if ident["sha256"] is None:
        c["C10_identity"] = None
        res["incomplete"].append("exe identity: " + ident["error"])
    else:
        c["C10_identity"] = exe_sha256 is None or ident["sha256"] == exe_sha256.lower()
    for k in ("C2_run_zero", "C5_gaps_zero", "C6_items_zero"):
        if c.get(k) is None:
            res["incomplete"].append("%s: no readable whole-run data" % k)
    if not c["C4_reached"]:
        res["incomplete"].append("required eras with no census frame: %s" % ", ".join(res["missing_eras"]))
    fail = [k for k in ("C1_frames", "C2_run_zero", "C3_era_zero", "C5_gaps_zero", "gaps_agree", "C6_items_zero",
                        "zpd_agree", "C7_zpd_zero", "C10_identity") if c.get(k) is False]
    if fail:
        res["verdict"] = "FAIL"
    elif res["incomplete"]:
        res["verdict"] = "INCOMPLETE"
    else:
        res["verdict"] = "PASS"
    res["failed"] = fail
    return res


def show(r):
    print("%s %s: %s%s" % (r["kind"], r["roll"], r["verdict"], (" - " + r["error"]) if r.get("error") else ""))
    if "coverage" in r:
        print("   coverage %s: %s" % (r["coverage"]["state"], r["coverage"]["why"]))
    for row in r.get("eras", []) + r.get("windows", []):
        nz = row["first_nonzero"]
        print("   %-12s t0=%-12s frames=%-6d pk=%-6d zpd=%-4d ext=%-3d blocked=%s%s" % (
            row["name"], row["t0"] or "-", row["frames"], row["pk"], row["zpd"], row["ext"], row["blocked_by_end"],
            "" if nz is None else "  first non-zero frame %d (%s) pk=%d keys=%s" % (
                nz["frame"], nz["ts"], nz["pk"], ",".join("%s:%d" % kv for kv in sorted(nz["keys"].items())))))
    w = r.get("whole_run")
    if w:
        cp = w.get("cp") or {}
        print("   whole run (%s): F pk=%s CUM pk=%s meter pm4=%s | gapApplied=%s gapKB=%s spans pm4=%s | CP packets=%s "
              "ib=%s seg=%s bs=%s zpd=%s ext=%s waits=%s (blocked %s)" % (
                  w["source"], w["f_pk"], w["cum_pk"], w["meter_pm4"], (w["seg"] or {}).get("gapApplied"),
                  (w["seg"] or {}).get("gapKB"), (w["spans"] or {}).get("pm4"), cp.get("packets"), cp.get("ib"),
                  cp.get("seg"), cp.get("bs"), cp.get("zpd"), cp.get("ext"), cp.get("waits"), cp.get("blocked")))
    if w and w.get("zpd_native"):
        print("   p2.zpd native path on: zpd= counts the kRkZpd records too - records %s (CP zpd - F 5B)"
              % w.get("zpd_records"))
    for p in r.get("producers_from_boot", []):
        print("   producer %s %s pk=%d -> %s" % (p["va"], p["name"], p["pk"], p["obligation"]))
    pr = r.get("progression")
    if pr:
        print("   progression -> %s: %s" % (pr["era"], "reached" if pr.get("ok") else "NOT reached - %s" % pr.get("why")))
    if r.get("movies_unplaced"):
        print("   movies with no EOM (GAP: interval unknown, not placed): %s" % ", ".join(
            "#%d wall=%.2fs" % (m["n"], m["wall"]) for m in r["movies_unplaced"]))
    if r.get("terminal_snapshot"):
        print("   %s" % r["terminal_snapshot"])
    ident = r.get("exe_identity")
    if ident:
        print("   exe identity: %s" % ("%s (%s)" % (ident["sha256"], ident["bound_by"]) if ident["sha256"] else
                                       "NONE - " + ident["error"]))
    for x in r.get("incomplete", []):
        print("   incomplete: %s" % x)
    if r.get("checks"):
        print("   checks: %s" % ", ".join("%s=%s" % kv for kv in r["checks"].items()))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("rolls", nargs="+", help="<kind>:<roll> (kind: %s)" % " | ".join(KINDS))
    ap.add_argument("--boot-frames", type=int, default=2)
    ap.add_argument("--graceful", action="store_true", default=True,
                    help="(the default) read every whole-run counter from the complete terminal snapshot")
    ap.add_argument("--no-graceful", dest="graceful", action="store_false",
                    help="information only: read the last periodic lines; the verdict is at best INCOMPLETE")
    ap.add_argument("--zpd", choices=("report", "zero"), default="report")
    ap.add_argument("--require", action="append", default=[], help="<kind>=<era>,<era>,... (replaces the kind's list)")
    ap.add_argument("--exe-sha256", help="the exe every roll must have launched (its provenance's exeSha256)")
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    req = {}
    for r in a.require:
        k, _, v = r.partition("=")
        req[k] = [e for e in v.split(",") if e]
    out = []
    for spec in a.rolls:
        kind, sep, roll = spec.partition(":")
        if not sep or kind not in KINDS:
            # a Windows path has a drive colon: <kind>:<D:\...> is split at the first colon only when <kind> is a kind
            ap.error("%s: want <kind>:<roll>, kind one of %s" % (spec, ", ".join(KINDS)))
        out.append(grade(kind, roll, a.boot_frames, a.graceful, a.zpd, req.get(kind), a.exe_sha256))
    hashes = [(r.get("exe_identity") or {}).get("sha256") for r in out]
    one_exe = None if None in hashes else len(set(hashes)) == 1
    for r in out:
        show(r)
    verdicts = [r["verdict"] for r in out]
    if "FAIL" in verdicts or one_exe is False:
        verdict, rc = "FAIL", 1
    elif "NO_CENSUS" in verdicts:
        verdict, rc = "NO_CENSUS", 2
    elif "INCOMPLETE" in verdicts or one_exe is None:
        verdict, rc = "INCOMPLETE", 3
    else:
        verdict, rc = "PASS", 0
    print("era_census: %s (%s; one exe: %s)" % (verdict, ", ".join("%s %s" % (r["kind"], r["verdict"]) for r in out),
                                               one_exe))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"rolls": out, "one_exe": one_exe, "verdict": verdict}, f, indent=1, default=str)
    return rc


if __name__ == "__main__":
    sys.exit(main())
