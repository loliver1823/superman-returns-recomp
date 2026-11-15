#!/usr/bin/env python3
"""canary_routes.py - the route versions of tools/perf/canary_compare.py (Fable ruling 24(3c): a repeatable flight
route for the perf drives; the design: s4-audit archive/step4_claude_20261018/FLIGHT_ROUTE_DESIGN.md).

a1   canary_compare's own ROUTE / SCENES / build_tape, byte for byte (the 10-09 baseline's route: it stays
     reproducible, so every old comparison can be rerun as it was).
a2*  a route FILE (JSON): the a1 timeline up to the takeoff's release (anchor + 81.2 s: everything the title / FMV /
     street / on-foot scenes see is a1's, entry for entry), then explicit post-takeoff segments - the attractor route:
     a hover that absorbs the takeoff's jitter, at most one yaw while slow, a STRAIGHT full-speed flight, a slow push
     into a building face (a wall is an attractor: the mover stops against it wherever the approach began), the pause
     at the wall - and its own flight / wall / pause scene windows.

    {"name": "a2-roof-1", "segments": [{"from": 81.2, "to": 84.0, "note": "hover"},
                                       {"from": 85.0, "to": 101.0, "ly": 32767, "rt": 255, "note": "straight flight"},
                                       {"from": 123.0, "to": 123.45, "buttons": ["START"]}, ...],
     "end": 140.0,
     "scenes": {"flight": {"from": 88.0, "to": 98.0, "label": "..."}, "wall": {...}, "pause": {...}}}

Segment fields: from / to (s after the anchor, from >= 81.2, to <= end), buttons (A, B, X, Y, START, BACK, LB, RB,
LS, RS, DPAD_*), lt / rt (0-255), lx / ly / rx / ry (-32768..32767); a segment's inputs hold for [from, to); no two
segments overlap; time outside every segment is neutral. Scene windows are in seconds after the anchor (the tape's
start, as a1's flight / wall / pause), at least 3 s long, inside [81.2, end - 2] (the shot is taken 1.2 s after a
window's end). Each scene's window, its shot and the capture's ~1 s recovery (window end + 2.2 s) lie inside ONE
interval of constant input (one segment, or one neutral gap) and before the end, and no two of them overlap. An a2
tape ends with an explicit neutral entry at 'end' (a1's bytes are unchanged).
"""
import hashlib
import json
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)

A1 = "a1"
BUTTONS = {"DPAD_UP": 0x0001, "DPAD_DOWN": 0x0002, "DPAD_LEFT": 0x0004, "DPAD_RIGHT": 0x0008, "START": 0x0010,
           "BACK": 0x0020, "LS": 0x0040, "RS": 0x0080, "LB": 0x0100, "RB": 0x0200, "A": 0x1000, "B": 0x2000,
           "X": 0x4000, "Y": 0x8000}
AXES = ("lx", "ly", "rx", "ry")
POST_SCENES = ("flight", "wall", "pause")
MIN_WINDOW_S, SHOT_LAG_S = 3.0, 1.2
CAPTURE_RECOVERY_S = 1.0  # canary_compare: a capture perturbs the next ~1 s
MAX_END_S = 170.0   # canary_ours.json's route dwell is 175 s: the roll must end inside it


def _cc():
    import canary_compare as cc  # imported late: canary_compare imports this module
    return cc


def prefix_end():
    """The a1 timeline a2 shares: up to the takeoff's release (Y held 1.2 s from ROUTE['takeoff'])."""
    return _cc().ROUTE["takeoff"] + 1.2


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def load_route(path):
    """The route file, validated; _sha256 = the sha256 of the file's BYTES as on disk (what `sha256sum` prints:
    a line-ending change is a different file)."""
    with open(path, "rb") as f:
        data = f.read()
    route = json.loads(data.decode("utf-8"))
    validate(route)
    route["_file"] = os.path.abspath(path)
    route["_sha256"] = sha256_bytes(data)
    return route


def validate(route):
    """Refuses a malformed route (a ValueError naming the problem): nothing is guessed."""
    if not isinstance(route, dict) or not isinstance(route.get("name"), str) or not route["name"].startswith("a2"):
        raise ValueError("a route file needs a 'name' starting with 'a2'")
    end = route.get("end")
    if not isinstance(end, (int, float)) or not prefix_end() + 10 <= end <= MAX_END_S:
        raise ValueError(f"'end' must be a number in [{prefix_end() + 10}, {MAX_END_S}]")
    segs = route.get("segments")
    if not isinstance(segs, list) or not segs:
        raise ValueError("'segments' must be a non-empty list")
    last = None
    for i, s in enumerate(sorted(segs, key=lambda s: s.get("from", 0))):
        f, t = s.get("from"), s.get("to")
        if not isinstance(f, (int, float)) or not isinstance(t, (int, float)) or not prefix_end() <= f < t <= end:
            raise ValueError(f"segment {i}: from / to must satisfy {prefix_end()} <= from < to <= end")
        if last is not None and f < last:
            raise ValueError(f"segment {i} ({f}..{t}) overlaps the one before (ending {last})")
        last = t
        unknown = set(s) - {"from", "to", "buttons", "lt", "rt", "note"} - set(AXES)
        if unknown:
            raise ValueError(f"segment {i}: unknown fields {sorted(unknown)}")
        for b in s.get("buttons", []):
            if b not in BUTTONS:
                raise ValueError(f"segment {i}: unknown button {b!r}")
        for k in ("lt", "rt"):
            if not 0 <= int(s.get(k, 0)) <= 255:
                raise ValueError(f"segment {i}: {k} must be 0..255")
        for k in AXES:
            if not -32768 <= int(s.get(k, 0)) <= 32767:
                raise ValueError(f"segment {i}: {k} must be -32768..32767")
    scenes = route.get("scenes")
    if not isinstance(scenes, dict) or set(scenes) != set(POST_SCENES):
        raise ValueError(f"'scenes' must define exactly {list(POST_SCENES)}")
    for name in POST_SCENES:
        w = scenes[name]
        f, t = w.get("from"), w.get("to")
        if not isinstance(f, (int, float)) or not isinstance(t, (int, float)):
            raise ValueError(f"scene {name}: from / to must be numbers")
        if t - f < MIN_WINDOW_S or f < prefix_end() or t + SHOT_LAG_S > end - 0.8:
            raise ValueError(f"scene {name}: the window must be >= {MIN_WINDOW_S} s, start >= {prefix_end()} and "
                             f"leave {SHOT_LAG_S} s for its shot before the end")
        if not isinstance(w.get("label"), str) or not w["label"]:
            raise ValueError(f"scene {name}: a 'label' is required")
    # Fable ruling 38: a route may name scenes it REPORTS but never grades (a2-roof-late's wall); flight always graded
    info = route.get("information_scenes", [])
    if (not isinstance(info, list) or len(set(info)) != len(info)
            or not set(info) <= set(POST_SCENES) - {"flight"}):
        raise ValueError("'information_scenes' must be a list of distinct names from ['wall', 'pause'] "
                         "(the flight scene is always graded)")
    # Each scene - its window, its shot (SHOT_LAG_S after the window) and the capture's recovery (CAPTURE_RECOVERY_S)
    # - must sit inside ONE interval of constant input (a segment, or a neutral gap between segments), so the shot
    # shows the measured state and no input changes under it; no window may overlap another scene's window or shot.
    bounds = sorted({prefix_end(), float(end)} | {float(s["from"]) for s in segs} | {float(s["to"]) for s in segs})
    spans = {}
    for name in POST_SCENES:
        f, t = float(scenes[name]["from"]), float(scenes[name]["to"])
        last_t = t + SHOT_LAG_S + CAPTURE_RECOVERY_S
        if any(f < b < last_t for b in bounds):
            raise ValueError(f"scene {name}: its window, shot and capture recovery ({f}..{last_t} s) cross an input "
                             f"change at {[b for b in bounds if f < b < last_t]} s")
        if last_t > float(end):
            raise ValueError(f"scene {name}: its shot and capture recovery end at {last_t} s, after the route's end")
        spans[name] = (f, last_t)
    for a_name, b_name in ((x, y) for i, x in enumerate(POST_SCENES) for y in POST_SCENES[i + 1:]):
        (f1, t1), (f2, t2) = spans[a_name], spans[b_name]
        if f1 < t2 and f2 < t1:
            raise ValueError(f"scenes {a_name} and {b_name} overlap (windows with their shots)")
    return route


def information_scenes(route):
    """The scenes a route reports but never grades (Fable ruling 38: a2-roof-late's wall); () for a1."""
    if route is None or route == A1:
        return ()
    return tuple(route.get("information_scenes", ()))


def scenes_of(route):
    """The scene list of a route: a1's SCENES, with a2's own flight / wall / pause windows and labels."""
    cc = _cc()
    if route is None or route == A1:
        return [dict(s) for s in cc.SCENES]
    out = []
    for s in cc.SCENES:
        if s["name"] in POST_SCENES:
            w = route["scenes"][s["name"]]
            out.append({"name": s["name"], "label": w["label"], "from": ["tape_start", float(w["from"])],
                        "to": ["tape_start", float(w["to"])]})
        else:
            out.append(dict(s))
    return out


def end_of(route):
    return _cc().ROUTE["end"] if route is None or route == A1 else float(route["end"])


def name_of(route):
    return A1 if route is None or route == A1 else route["name"]


def a1_tape_bytes():
    cc = _cc()
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "a1.inpr")
        cc.build_tape(p)
        with open(p, "rb") as f:
            return f.read()


def _segment_state(route, ms):
    for s in route["segments"]:
        if s["from"] * 1000 <= ms < s["to"] * 1000:
            b = 0
            for name in s.get("buttons", []):
                b |= BUTTONS[name]
            return b, int(s.get("lt", 0)), int(s.get("rt", 0)), [int(s.get(k, 0)) for k in AXES]
    return 0, 0, 0, [0, 0, 0, 0]


def tape_bytes(route):
    """The route's tape, as bytes. a1 = canary_compare.build_tape exactly."""
    if route is None or route == A1:
        return a1_tape_bytes()
    from gen_collision_route_tape import entry, MAGIC, VERSION, STEP_MS
    a1 = a1_tape_bytes()
    size = 20  # kernel/input_recorder.cpp TimedEntry
    a1_entries = [a1[16 + i * size:16 + (i + 1) * size] for i in range((len(a1) - 16) // size)]
    cut = prefix_end() * 1000
    ents = []
    for i, ms in enumerate(range(0, int(route["end"] * 1000), STEP_MS)):
        if ms < cut:
            ents.append(a1_entries[i])  # the shared a1 timeline, entry for entry
            continue
        b, lt, rt, (lx, ly, rx, ry) = _segment_state(route, ms)
        ents.append(entry(ms, b, lt, rt, lx, ly, rx, ry))
    # an explicit neutral entry at the declared end: kernel/input_recorder.cpp serves the last entry until 1 s after
    # its time, so without it a segment running to the end would be held up to ~1 s past it
    ents.append(entry(int(route["end"] * 1000), 0, 0, 0, 0, 0, 0, 0))
    return struct.pack("<IIII", MAGIC, VERSION, len(ents), 0) + b"".join(ents)


def build_route_tape(path, route):
    """Writes the route's tape; returns its sha256."""
    data = tape_bytes(route)
    with open(path, "wb") as f:
        f.write(data)
    return sha256_bytes(data)


# ------------------------------------------------------------------------------------ the a2 log pin
# The coordinator, 2026-10-22 (FLIGHT_ROUTE_DESIGN.md section 8): EVERY a2 roll of OURS runs these gates, the K
# baselines and the new builds alike. Then the frozen flight criterion's F2 / F3 ([COLL_PATH], written only under
# SR_LOG=dev) can be checked on every roll (Fable 35 (3)), and K and a new build compare like with like.
# canary_compare applies the pin to every a2 roll of ours and records it in run.json ("log_pin"). --no-a2-log-pin
# leaves it off and records that ("log_pin_off"); it exists only for two pre-registered measured blocks: the overhead
# check's reference arm (tools/perf/route_log_cost.py) and the unpinned K block (tools/perf/route_repeat.py --profile
# kperf: the performance target's rolls, Fable ruling 42, 2026-10-23; the pinned k block stays the route-validity
# receipt). Every other grader refuses such a run (route_repeat k, route_survey). a1 and Xenia rolls never carry it.
A2_LOG_PIN = {"SR_LOG": "dev"}


# ------------------------------------------------------------------------------------ the view classifier
# FROZEN 2026-10-22 ~00:3x (coordinator; FLIGHT_ROUTE_DESIGN.md section 5): calibrated on the existing K shots
# (audit\route\view_distance_calib.json, sha256 5c9da579...): route A's same-view pairs <= 1.65 / 0.49 / 0.46,
# different views >= 4.90 / 3.04 / 2.97.
THUMB = (64, 36)
TAU = {"flight": 3.0, "wall": 1.5, "pause": 1.5}
LUMA_BAND = 1.5
# A blank capture (a flip-model black / uniform PrintWindow image) is no evidence of a view: its 64x36 grayscale
# thumbnail spans <= BLANK_RANGE levels. Every existing K route-A shot spans >= 115 (the HUD alone gives contrast).
BLANK_RANGE = 8


def is_blank(t):
    return max(t) - min(t) <= BLANK_RANGE


def thumb(path):
    from PIL import Image
    with Image.open(path) as im:
        return list(im.convert("L").resize(THUMB).getdata())


def luma_of(path):
    t = thumb(path)
    return sum(t) / len(t)


def distance(a, b):
    """The mean absolute difference (0-255) of two 64x36 grayscale thumbnails."""
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def on_view(shot, ref_shot, scene, ref_luma=None, shot_luma=None):
    """(on view?, distance, luma difference) of one shot against a reference shot of the same scene."""
    t, r = thumb(shot), thumb(ref_shot)
    d = distance(t, r)
    lu = shot_luma if shot_luma is not None else sum(t) / len(t)
    rl = ref_luma if ref_luma is not None else sum(r) / len(r)
    dl = abs(lu - rl)
    return d <= TAU[scene] and dl <= LUMA_BAND and not is_blank(t), round(d, 3), round(dl, 3)
