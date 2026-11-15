#!/usr/bin/env python3
"""world_floor.py - is the 3D WORLD drawn in a dumped present? (iter-467 ledger)

The one implementation of the world rule that tools/perf/flicker_gate.py (the gate) and tools/native_ledger.py
(the obligation ledger) both apply, so a gate verdict and a ledger status can never disagree about it.

Why it exists: a black 3D world under a working HUD is perfectly STABLE, so the dense-freeze oracle (consecutive
frozen presents pixel-identical) and the A-B-A span both pass it, and so does a pause menu over a black world: the
panel alone clears a plain luma floor (p3a_abp: luma 17). Eight archived rolls cited as passes graded black frames
(PROGRESS 2026-10-09 [ledger]).

The rule, per dumped present (`classify`):
  dark        the central region (HUD margins cut) has mean luma < 8 or a near-black (< 12) fraction > 0.6, AND
              its structure is not a drawn scene's: the EDGE SHARE of the same region (pixels whose luma differs
              from the right or lower neighbour by >= STRUCT_GRAD on the 160x90 downscale) is below STRUCT_EDGE_MIN,
              either uncovered side band lacks a 95th-5th percentile luma spread >= STRUCT_SIDE_SPREAD_MIN, or
              structure is not measured under the current definition (never a guess). Side support prevents
              central text/loading overlays alone rescuing a dark frame with stale gameplay context. A dark
              DRAWN scene - the opening corridor, a dusk wall (2026-10-16: s3m_gate, qdl1, s3check3's dev gate,
              scm2 / scm2b, dc2 cppm4: the luma floor alone read them as no world) - has the world's edges; a black
              world (the startup race, job65's w65_3 / w65_5) has the HUD's few at most: world_floor_validate.py;
  gameplay    otherwise, when the player-input context (`[PC_CONTROLS] context <name>`) at the present is gameplay;
  menu_world  otherwise (the pause menu) when the side bands the panel leaves uncovered (x 2-18% / 82-98%,
              y 25-75%) show the world too: luma >= 5 and near-black <= 0.9 (the dimmed world reads 10-15 /
              0.56-0.82 there, a black world 0-2.3 / 0.96-1.0);
  menu_black  the pause menu over a black world.
A dumped present SHOWS THE WORLD when it is gameplay or menu_world. A freeze roll needs MIN_WORLD_FREEZES graded
freezes showing it (the middle dumped present of each); a span needs SPAN_IN_PLAY_MIN of its presents IN PLAY
(dumped at or after the run's own in-game era start, in the gameplay context).

Measurements are cached on disk (path + size + mtime key) in %LOCALAPPDATA%\\sr_native_ledger_frames.json
(SR_WORLD_FLOOR_CACHE overrides; =0 disables): the archives hold thousands of 1280x720 BMPs.

THE MEASUREMENT SIDECAR (2026-10-14). Raw frames are scratch: the frame-scratch GC (tools/sr_artifacts.ps1) deletes
them 48 h after a roll, which left 67 archived attempts grading "world 0 / unmeasured" - missing input, not a
rendering failure. <attempt>/frames_measured.json keeps the MEASUREMENTS (never a verdict) of the attempt's frames:

    {"schema": "sr-frames-measured/2", "measure_version": 1, "frames_dir": "frames",
     "written": "<local ISO time>", "how": "measured" | "recovered", "writer": {...}, "provenance": {...},
     "rule": {the measurement geometry + the thresholds in force when written - for the reader, never applied},
     "run_log": {"size": N, "mtime_ns": M} | null,     # binds the sidecar to the run it measured
     "inventory": {"source", "count", "on_disk", "bmp_bytes", "total_bytes",
                   "other_files": {"<non-BMP name>": {"size", "how": "stat" | "residual", "evidence"}}},
     "frames": {"<bmp name>": {"on_disk": bool, "size": N | null, "mtime_ns": M | null, "center": [luma, dark] | null,
                               "sides": [luma, dark] | null, "source": "bmp" | "cache" | "named", ...provenance}}}

THE INVARIANT (Astra 2026-10-14, round 4): the INVENTORY IS FIXED WHEN THE SIDECAR IS WRITTEN AND CHECKED AGAINST THE
DELETION RECORD. The frames table is the complete inventory: every file the frames folder held (on_disk, with its
size and mtime_ns as stat'ed by the GC's measurer, or for a recovery outcome.json's list and sizes) plus, off disk,
every frame run.log / outcome.json name that was not there; the header states the sums. The reader
(sidecar_doc_problem) requires (a) the header's total_bytes to EQUAL frames_deleted.txt's byte total when that file
exists, and (b) every frame any source names - run.log [PRESENT_DUMP], outcome.json, the BMPs on disk - to be in
the inventory, with no two frames claiming one present, and every source to agree on each frame's VERSION
(outcome.json's byte count; a BMP on disk's size + mtime_ns, which the inventory must have recorded; an unreadable
outcome.json agrees with nothing);
otherwise the sidecar is rejected and its frames grade UNMEASURED. The fixed inventory is then THE expected set: a
truncated log or a lost outcome.json cannot shrink it, and a same-named frame of another version cannot swap into it.

How the rule reads it (frame_names / present_index / frames_by_present / inventory_rows / measure / classify):
  - AN ARCHIVED ROLL (frames_deleted.txt or frames_measured.json exists) is graded from its applicable sidecar only:
    a rejected sidecar, or a deletion record with no sidecar, makes EVERY frame of the roll unmeasured - never the
    BMPs that happen to be on disk (archived_problem; Astra 2026-10-14 round 5);
  - ONE ROW PER PRESENT (present_index): every share, span sample and freeze middle is computed over the inventory's
    presents, never over log lines; a present two frames claim, or whose frame run.log names more than once, is
    ambiguous and grades unmeasured (a repeated line cannot weight a frame);
  - a BMP on disk wins (in an archived roll, only as the version its sidecar fixed): its present maps to it and it is
    measured as before (cached);
  - a present with no BMP is taken from the sidecar's frame inventory, and measure() returns the stored
    measurement, or None when there is none - classify() then says 'unmeasured', never a guess;
  - a sidecar is IGNORED (every sidecar-only frame unmeasured) when its schema or measure_version differs, its
    frames_dir is not this folder, the attempt's run.log changed since it was written (the folder was re-run or its
    log was cut), any measurement in it is not a finite pair inside 0..255 / 0..1, or the invariant fails;
  - THE EXPECTED INVENTORY: the applicable sidecar's fixed inventory; without one, the BMPs on disk plus every frame
    the run.log / outcome.json name. A frame in it with no BMP and no stored measurement is still there, as
    'unmeasured': a freeze's middle logged dump ([SIM_FREEZE_DUMP]) is never replaced by a neighbour, a span samples
    the same 40 names, and a share (a meter's world share, a span's in-play share) is computed over EVERY inventory
    frame - one no surviving log line names has unknown timing and counts as unmeasured / not in play.
The class comes from the same thresholds either way, so a sidecar-backed frame grades exactly as its BMP did.
Writers: `python tools/perf/world_floor.py sidecar <frames dir>` (from the BMPs; the GC runs it before it deletes)
and tools/perf/recover_frame_measurements.py (from this cache, for frames already deleted).
"""
import datetime
import json
import math
import os
import re
import sys

SCENE_LUMA_MIN = 8.0       # central region: the flicker gate's scene floor (5f460b43)
SCENE_DARK_MAX = 0.6
SIDE_LUMA_MIN = 5.0        # the pause menu's uncovered side bands
SIDE_DARK_MAX = 0.9
MIN_WORLD_FREEZES = 8      # graded freezes that must show the world
MAX_MENU_FREEZES = 4       # ... of which at most this many may be the pause menu over the world: the rest are
                           # gameplay (the self-opening pause menu, 2026-10-10: a static menu over a dimmed world
                           # is not the draw path under test)
SPAN_IN_PLAY_MIN = 0.8     # share of a span's presents that must be in play
STRUCT_GRAD = 2            # structure: a pixel of the central region is an EDGE when its luma differs from its right
                           # or lower neighbour by this much or more (0..255, on the same 160x90 downscale - the
                           # bicubic average of ~8x8 source pixels, so a step of 2 there is geometry, not noise; a
                           # black world is exactly 0: w65_3's tail 0.000 at every level)
STRUCT_EDGE_MIN = 0.08     # ... and a frame under the luma floor whose edge share reaches this is DRAWN (a corridor,
                           # a dusk wall), not black. tools/perf/world_floor_validate.py on the archived witnesses
                           # (2026-10-16, gradient 2): the black worlds' tails 0.000-0.043 (w65_5: a HUD icon), the
                           # menu-over-black freezes 0.034-0.037, the dark drawn corridor freezes 0.131-0.303 (s3m_gate,
                           # qdl1, s3check3 dev), lit freezes >= 0.221 - the threshold sits ~2x above the black and
                           # ~0.6x below the darkest drawn scene (at gradient 8 the two overlapped: 0.035 vs 0.009)

STRUCT_VERSION = 2         # v2 requires spatial support in BOTH uncovered side bands; central text is not world
STRUCT_SIDE_SPREAD_MIN = 2 # 95th minus 5th percentile luma; smooth walls count, isolated HUD pixels do not

TS = r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]"
FREEZE_DUMP = re.compile(TS + r".*\[SIM_FREEZE_DUMP\] freeze=(\d+) k=\d+ present=(\d+)", re.M)
PRESENT_DUMP = re.compile(TS + r".*\[PRESENT_DUMP\] .*_p(\d+)_\d+x\d+\.bmp", re.M)
CONTEXT = re.compile(TS + r".*\[PC_CONTROLS\] context (\w+)", re.M)
METER_WINDOW = re.compile(TS + r".*\[NATIVE_METER\] w=\d+ [\d.]+s frames=(\d+)", re.M)
CP_CENSUS = re.compile(TS + r".*\[(?:GPU_CP|NATIVE_SUBMIT)\] census .* swaps=(\d+) ", re.M)
DUMP_NAME = re.compile(r"\[PRESENT_DUMP\] (.+?_p\d+_\d+x\d+\.bmp)")   # a present dump, named as PRESENT_DUMP reads it
FRAME_P = re.compile(r"_p(\d+)_")
WORLD = ("gameplay", "menu_world")

DELETED_NOTE = re.compile(r"\(were (.+?), ([\d,]+) bytes\)")   # frames_deleted.txt (tools/sr_artifacts.ps1)

SIDECAR = "frames_measured.json"
SIDECAR_SCHEMA = "sr-frames-measured/2"     # /2: the fixed inventory + byte reconciliation (round 4)
# What frame_scene / side_scene measure (crop geometry, resize, the near-black level). BUMP IT when either changes:
# a sidecar written under another version is ignored, so stored numbers are never read under a new definition.
# The structure measurement (structure_scene, 2026-10-16) is ADDITIVE: a sidecar or cache entry without it still
# grades under the luma floor alone (classify: an unmeasured structure never rescues a dark frame), so the version
# stays - and a sidecar written from here carries all three.
MEASURE_VERSION = 1
NEAR_BLACK = 12


def ts_ms(ts):
    """A log timestamp ('2026-10-09 08:52:45.873') as epoch ms (local time, as perf_report reads them)."""
    return datetime.datetime.strptime(ts, "%Y-%m-%d %H:%M:%S.%f").timestamp() * 1000.0


def ms_ts(ms):
    """ts_ms's inverse: epoch ms as a log timestamp string (they compare as strings: context_at)."""
    return datetime.datetime.fromtimestamp(ms / 1000.0).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


# ------------------------------------------------------------------------------------------------ one frame

def _luma(path):
    from PIL import Image
    with Image.open(path) as im:
        return im.convert("L")


def _center_of(im):
    w, h = im.size
    px = list(im.crop((w // 10, h * 15 // 100, w * 9 // 10, h * 85 // 100)).resize((160, 90)).getdata())
    return sum(px) / len(px), sum(1 for p in px if p < NEAR_BLACK) / len(px)


def _sides_of(im):
    w, h = im.size
    px = []
    for x0, x1 in ((w * 2 // 100, w * 18 // 100), (w * 82 // 100, w * 98 // 100)):
        px += list(im.crop((x0, h * 25 // 100, x1, h * 75 // 100)).resize((40, 90)).getdata())
    return sum(px) / len(px), sum(1 for p in px if p < NEAR_BLACK) / len(px)


def frame_scene(path):
    """(mean luma, near-black fraction) of a present's central region, HUD margins cut."""
    return _center_of(_luma(path))


def side_scene(path):
    """(mean luma, near-black fraction) of the two side bands a pause-menu panel leaves uncovered."""
    return _sides_of(_luma(path))


def _edges_of(g):
    """Mean gradient and edge share for a resized luma crop."""
    px = list(g.getdata())
    W, H = g.size
    tot = edges = 0
    for y in range(H - 1):
        row = y * W
        for x in range(W - 1):
            p = px[row + x]
            d = max(abs(px[row + x + 1] - p), abs(px[row + W + x] - p))
            tot += d
            if d >= STRUCT_GRAD:
                edges += 1
    n = (W - 1) * (H - 1)
    return tot / n, edges / n


def _structure_of(im):
    """(center mean gradient, spatially supported center edge share).

    Central text/loading overlays can exceed the edge floor even with stale gameplay context. Require a luma
    spread in BOTH uncovered side bands too; otherwise record zero supported share. Smooth wall gradients
    qualify without sharp side edges, but an isolated HUD pixel does not. This is a conservative pixel
    heuristic, not semantic world recognition. Its definition is versioned separately from center/side luma.
    """
    w, h = im.size
    center = _edges_of(im.crop((w // 10, h * 15 // 100, w * 9 // 10, h * 85 // 100)).resize((160, 90)))
    for x0, x1 in ((w * 2 // 100, w * 18 // 100), (w * 82 // 100, w * 98 // 100)):
        side = sorted(im.crop((x0, h * 25 // 100, x1, h * 75 // 100)).resize((40, 90)).getdata())
        if side[len(side) * 95 // 100] - side[len(side) * 5 // 100] < STRUCT_SIDE_SPREAD_MIN:
            return center[0], 0.0
    return center


def structure_scene(path):
    """(center mean gradient, spatially supported center edge share) of a present (_structure_of)."""
    return _structure_of(_luma(path))


def measure_bmp(path):
    """The three measurements of one BMP from a single decode -> (center, sides, structure). Never cached: the
    sidecar writer."""
    im = _luma(path)
    return _center_of(im), _sides_of(im), _structure_of(im)


MEASURES = {"center": frame_scene, "sides": side_scene, "structure": structure_scene}


def rule_constants():
    """The measurement definition and the thresholds in force, as recorded in a sidecar header (for the reader:
    classification always applies the CURRENT thresholds to the stored measurements)."""
    return {"measure_version": MEASURE_VERSION,
            "center": "luma (PIL 'L') of crop (w/10, 15%h, 9w/10, 85%h) resized to 160x90",
            "sides": "luma of crops x 2-18% and 82-98%, y 25-75%, each resized to 40x90",
            "structure": "(center mean gradient, supported center edge share): center 160x90; share zero unless "
                         "both side crops at 40x90 have 95th-5th percentile luma >= struct_side_spread_min; "
                         "gradient = max(|right - here|, |below - here|), edge at >= struct_grad",
            "struct_version": STRUCT_VERSION, "struct_side_spread_min": STRUCT_SIDE_SPREAD_MIN,
            "near_black": NEAR_BLACK, "struct_grad": STRUCT_GRAD, "struct_edge_min": STRUCT_EDGE_MIN,
            "scene_luma_min": SCENE_LUMA_MIN, "scene_dark_max": SCENE_DARK_MAX,
            "side_luma_min": SIDE_LUMA_MIN, "side_dark_max": SIDE_DARK_MAX}


def valid_measure(v):
    """A measurement is a (mean luma, near-black fraction) pair of finite real numbers inside 0..255 and 0..1. Anything
    else (NaN, infinities, bools, strings, out of range) is not one: a NaN luma compares False against every
    threshold and would grade as drawn."""
    if not isinstance(v, (list, tuple)) or len(v) != 2:
        return False
    if not all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in v):
        return False
    try:   # lane C r21 (Astra 82f7b6d conv20 P2): an integer past float's range (a cached `10**400`) raised OverflowError
        if not all(math.isfinite(x) for x in v):   # and read as a damaged frame - it is simply not a measurement
            return False
    except (OverflowError, ValueError):
        return False
    return 0.0 <= v[0] <= 255.0 and 0.0 <= v[1] <= 1.0


def _cache_path():
    v = os.environ.get("SR_WORLD_FLOOR_CACHE")
    if v == "0":
        return None
    return v or os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "sr_native_ledger_frames.json")


_cache = None
_dirty = False
# The cache records the MEASURE_VERSION its values were measured under; a cache without the key predates it and was
# measured under version 1 (the definition is unchanged since the cache was created in e7d4fe8c, re-checked
# bit-identical on 1,500 entries 2026-10-14). A cache of another version is never served.
CACHE_VERSION_KEY = "__measure_version__"


def cache_version(cache):
    return cache.get(CACHE_VERSION_KEY, 1)


_sidecars = {}


def _stat_sig(path):
    """{"size", "mtime_ns"} of a file, or None when it does not exist. Any OTHER stat error raises: a file that
    cannot be stat'ed is never an absent one (a run.log binding of None must not match an unreadable log)."""
    try:
        st = os.stat(path)
    except (FileNotFoundError, NotADirectoryError):
        return None
    return {"size": st.st_size, "mtime_ns": st.st_mtime_ns}


def marker_exists(path):
    """Whether a marker file (frames_measured.json, frames_deleted.txt, outcome.json) EXISTS - its directory entry,
    from the file system, never from what it parses to (Astra 2026-10-14 round 6: a sidecar holding JSON null is a
    sidecar). A path that cannot be checked for any reason but absence counts as present: fail closed."""
    try:
        os.lstat(path)
        return True
    except (FileNotFoundError, NotADirectoryError):
        return False
    except OSError:
        return True


def sidecar_path(fdir):
    """<attempt>/frames_measured.json for the frames folder <attempt>/<frames_dir>."""
    return os.path.join(os.path.dirname(os.path.abspath(fdir).rstrip("\\/")), SIDECAR)


def read_sidecar(fdir):
    """(doc, problem) for fdir's sidecar: (None, None) only when NO sidecar file exists (marker_exists); (None, why)
    when the file exists but may not be used - unreadable, not JSON, ANY parsed value that is not a valid sidecar
    object (null, true, 0, "", a list, the wrong schema: sidecar_doc_problem), or a check that cannot be made;
    (doc, None) when it applies to fdir. Cached per process."""
    fdir = os.path.abspath(fdir).rstrip("\\/")
    key = os.path.normcase(fdir)
    if key in _sidecars:
        return _sidecars[key]
    sp = sidecar_path(fdir)
    out = (None, None)
    if marker_exists(sp):
        try:
            with open(sp, encoding="utf-8") as f:
                doc = json.load(f)
        except (OSError, ValueError) as x:
            out = (None, "%s unreadable: %s" % (SIDECAR, x))
        else:   # whatever it parsed to - null included - is judged; nothing short-circuits on a falsy value
            try:
                bad = sidecar_doc_problem(doc, fdir)
            except (OSError, TypeError, ValueError, AttributeError, KeyError) as x:
                bad = "it cannot be checked (%s: %s)" % (type(x).__name__, x)
            out = (None, "%s: %s" % (SIDECAR, bad)) if bad else (doc, None)
    _sidecars[key] = out
    return out


def _int(v, allow_none=False):
    return (v is None and allow_none) or (isinstance(v, int) and not isinstance(v, bool) and v >= 0)


def sidecar_frames_problem(frames):
    """Why a sidecar's frame table is malformed, or None. Every key a .bmp name; every entry an object with
    on_disk (bool: the file was in the frames folder when the inventory was fixed), size (a byte count; required
    when on_disk), mtime_ns (int or null), and center / sides null or a valid measurement (valid_measure). One bad
    entry rejects the whole sidecar - it is not trustworthy."""
    for name, e in frames.items():
        if not isinstance(name, str) or not name.endswith(".bmp"):
            return "frame %r is not a .bmp name" % (name,)
        if not isinstance(e, dict):
            return "frame %s is not an object" % name
        if not isinstance(e.get("on_disk"), bool):
            return "frame %s has no on_disk flag" % name
        if not _int(e.get("size"), allow_none=not e["on_disk"]) or not _int(e.get("mtime_ns"), allow_none=True):
            return "frame %s has no valid size / mtime_ns" % name
        for what in ("center", "sides", "structure"):
            v = e.get(what)
            if v is not None and not valid_measure(v):
                return "frame %s has an invalid %s measurement %r (need finite luma 0..255, fraction 0..1)" % (
                    name, what, v)
    return None


def inventory_totals(frames, other_files):
    """(BMP bytes on disk, other bytes, total) of an inventory - the sums the header must state."""
    bmp = sum(e["size"] for e in frames.values() if e.get("on_disk"))
    other = sum(o["size"] for o in other_files.values())
    return bmp, other, bmp + other


def inventory_header_problem(doc):
    """Why the header's FIXED INVENTORY is malformed or does not add up, or None. The header lists the frames folder
    as it was when the sidecar was written: inventory = {"source", "count", "on_disk", "bmp_bytes", "other_files":
    {name: {"size", "how": "stat" | "residual", "evidence"}}, "total_bytes"}; count / on_disk / the byte sums must
    match the frames table exactly. A 'residual' other file (recovery only: a byte surplus the deletion record shows
    beyond the listed BMPs) must be smaller than the smallest listed BMP, so no frame can hide in it."""
    inv, frames = doc.get("inventory"), doc["frames"]
    if not isinstance(inv, dict):
        return "no fixed inventory in the header"
    others = inv.get("other_files")
    if not isinstance(others, dict):
        return "the inventory has no other_files table"
    for name, o in others.items():
        if not isinstance(name, str) or name.lower().endswith(".bmp") or not isinstance(o, dict) or \
                not _int(o.get("size")) or o.get("how") not in ("stat", "residual"):
            return "other file %r is malformed (a non-BMP name, a byte size, how stat | residual)" % (name,)
    bmp, other, total = inventory_totals(frames, others)
    on = [e["size"] for e in frames.values() if e.get("on_disk")]
    if not all(_int(inv.get(k)) for k in ("count", "on_disk", "bmp_bytes", "total_bytes")):   # true is not 1
        return "the inventory header's counts are not byte / frame counts (%r)" % (
            {k: inv.get(k) for k in ("count", "on_disk", "bmp_bytes", "total_bytes")},)
    if inv.get("count") != len(frames) or inv.get("on_disk") != len(on) or inv.get("bmp_bytes") != bmp or \
            inv.get("total_bytes") != total:
        return "the inventory header (count %r, on_disk %r, bmp_bytes %r, total_bytes %r) does not add up to its " \
               "frames (%d, %d, %d, %d)" % (inv.get("count"), inv.get("on_disk"), inv.get("bmp_bytes"),
                                            inv.get("total_bytes"), len(frames), len(on), bmp, total)
    residual = sum(o["size"] for o in others.values() if o["how"] == "residual")
    if residual and (not on or residual >= min(on)):
        return "%d residual bytes are not smaller than the smallest listed BMP: a frame could hide in them" % residual
    return None


def deletion_record(att):
    """(exists, bytes or None, text) of <attempt>/frames_deleted.txt - the GC's record of what it deleted. bytes is
    None unless the file holds exactly ONE "(were <folder>, N bytes)" note (the GC writes one; two notes are two
    deletions and no single total to reconcile with)."""
    p = os.path.join(att, "frames_deleted.txt")
    if not marker_exists(p):
        return False, None, None
    txt = read_text(p).strip()
    notes = DELETED_NOTE.findall(txt)
    return True, (int(notes[0][1].replace(",", "")) if len(notes) == 1 else None), txt


def sidecar_doc_problem(doc, fdir):
    """Why `doc` may NOT serve as fdir's sidecar, or None. Structure: schema, measure_version, frames_dir, the
    run.log binding, the frame table (sidecar_frames_problem) and the fixed inventory (inventory_header_problem).
    THE INVARIANT (Astra 2026-10-14 round 4): the inventory is fixed when the sidecar is written and checked against
    the deletion record -
      (a) when frames_deleted.txt exists, its byte total must EQUAL the inventory's total_bytes (an unparsable record
          cannot reconcile); every residual other file must be named in run.log as written into the frames folder;
      (b) every frame any source names - run.log [PRESENT_DUMP] lines, outcome.json, the BMPs on disk - must be in
          the inventory; and no two inventory frames may claim one present.
    Every source must also agree on WHICH version of a frame the inventory holds: an outcome.json that exists but
    cannot be read cannot be checked (rejected); outcome.json's byte count of a frame must equal the inventory's size;
    a BMP on disk must be an on_disk frame of the inventory with the same size and mtime_ns - a same-named frame of
    another version is not the frame that was measured, and one whose version the inventory never recorded (an
    unrecovered frame: mtime_ns null) cannot be shown to be it.
    A sidecar that fails any of it is rejected: its frames grade UNMEASURED."""
    if not isinstance(doc, dict) or doc.get("schema") != SIDECAR_SCHEMA or not isinstance(doc.get("frames"), dict):
        return "not a %s document" % SIDECAR_SCHEMA
    if doc.get("measure_version") != MEASURE_VERSION or isinstance(doc.get("measure_version"), bool):   # true == 1
        return "measure_version %r, this rule measures version %d" % (doc.get("measure_version"), MEASURE_VERSION)
    fdir = os.path.abspath(fdir).rstrip("\\/")
    if os.path.normcase(doc.get("frames_dir", "")) != os.path.normcase(os.path.basename(fdir)):
        return "it describes %r, not %r" % (doc.get("frames_dir"), os.path.basename(fdir))
    att = os.path.dirname(fdir)
    run_log = os.path.join(att, "run.log")
    if doc.get("run_log") != _stat_sig(run_log):
        return "it was written for another run.log (%r, now %r): the attempt was re-run or its log changed" % (
            doc.get("run_log"), _stat_sig(run_log))
    bad = sidecar_frames_problem(doc["frames"]) or inventory_header_problem(doc)
    if bad:
        return bad
    inv, frames = doc["inventory"], doc["frames"]
    exists, deleted, _ = deletion_record(att)
    if exists:
        if deleted is None:
            return "frames_deleted.txt records no single byte total: the inventory cannot be reconciled"
        if deleted != inv["total_bytes"]:
            return "the inventory holds %s bytes, frames_deleted.txt records %s: it is not the folder that was " \
                   "deleted" % (format(inv["total_bytes"], ","), format(deleted, ","))
    log_txt = read_text(run_log) if marker_exists(run_log) else ""
    for key, o in inv["other_files"].items():
        for name in (key.split(" | ") if o["how"] == "residual" else []):
            if not re.search(r"[\\/]frames[\\/]" + re.escape(name) + r"(?=\s|$)", log_txt, re.M):
                return "residual other file %s is not named in run.log as written into the frames folder" % name
    outcome = outcome_frames(att)
    if outcome is None and marker_exists(os.path.join(att, "outcome.json")):
        return "outcome.json cannot be read: the inventory cannot be checked against its frame list"
    disk = set(_bmps(fdir))
    named = {"run.log": logged_names(att), "outcome.json": set(outcome or {}), "the frames folder": disk}
    for src, names in named.items():
        missing = sorted(set(names) - set(frames))
        if missing:
            return "%s names %d frame(s) the inventory does not list (first %s): the inventory is incomplete" % (
                src, len(missing), missing[0])
    for name, b in sorted((outcome or {}).items()):
        if frames[name]["size"] is not None and frames[name]["size"] != b:
            return "outcome.json lists %s at %d bytes, the inventory at %d: another version of the frame" % (
                name, b, frames[name]["size"])
    for name in sorted(disk):
        e, st = frames[name], _stat_sig(os.path.join(fdir, name))
        if st is None or not e["on_disk"] or e["size"] != st["size"] or e["mtime_ns"] != st["mtime_ns"]:
            return "the BMP %s on disk (%r) is not the version the inventory fixed (on_disk %r, size %r, " \
                   "mtime_ns %r)" % (name, st, e["on_disk"], e["size"], e["mtime_ns"])
    seen = {}
    for name in frames:
        m = FRAME_P.search(name)
        if m and seen.setdefault(int(m.group(1)), name) != name:
            return "frames %s and %s claim the same present %s" % (seen[int(m.group(1))], name, m.group(1))
    return None


def sidecar_problem(fdir):
    """Why fdir's sidecar is ignored, or None (none there, or it applies)."""
    return read_sidecar(fdir)[1]


def _bmps(fdir):
    """The BMP names in a frames folder; [] only when the folder does not exist. Any other enumeration error RAISES:
    an unreadable folder is never an empty one (the GC deletes what the sidecar writer says is not there)."""
    try:
        names = os.listdir(fdir)
    except FileNotFoundError:
        return []
    return [f for f in names if f.endswith(".bmp")]


def archived_problem(fdir):
    """Why NONE of fdir's frames may be read, or None (Astra 2026-10-14 round 5). A roll whose frames_deleted.txt or
    frames_measured.json exists is ARCHIVED: its frames are no longer live, and only an applicable sidecar grades
    them. A rejected sidecar, or a deletion record with no sidecar, makes the WHOLE roll unmeasured (measure() gives
    None for every frame) - never the BMPs that happen to be on disk: one put back, or left behind, cannot stand in for
    the inventory that was fixed and lost. ARCHIVAL IS DECIDED BY THE MARKER FILES' EXISTENCE (marker_exists), never
    by what they parse to (Astra round 6): a frames_measured.json holding null, true, 0, "" or a list is a sidecar
    that does not apply - the roll is archived and unmeasured. Cached per process with the sidecar read."""
    fdir = os.path.abspath(fdir).rstrip("\\/")
    key = ("archived", os.path.normcase(fdir))
    if key not in _sidecars:
        has_sidecar = marker_exists(sidecar_path(fdir))
        has_record = marker_exists(os.path.join(os.path.dirname(fdir), "frames_deleted.txt"))
        why = None
        if has_sidecar or has_record:
            doc, prob = read_sidecar(fdir)
            if doc is None or prob is not None:   # only a sidecar that APPLIES grades an archived roll
                if has_sidecar:
                    why = "the sidecar is rejected (%s)" % (prob or "%s exists but is not a sidecar" % SIDECAR)
                else:
                    why = "the frames were deleted (frames_deleted.txt) and no %s holds their measurements" % SIDECAR
        _sidecars[key] = why
    return _sidecars[key]


def _forget(fdir):
    """Drop fdir's cached sidecar read (after a write, or before the GC's last check)."""
    k = os.path.normcase(os.path.abspath(fdir).rstrip("\\/"))
    for key in (k, ("archived", k), ("claimed", k)):
        _sidecars.pop(key, None)


def inventory_problem(fdir):
    """Why a frames folder with NO frame to read is missing input rather than a roll that dumped nothing - or None.
    An archived roll whose frames cannot be read (archived_problem), or run.log [PRESENT_DUMP] lines with no frame on
    disk and no sidecar, all mean frames existed and their measurements are unavailable."""
    prob = archived_problem(fdir)
    if prob:
        return prob
    att = os.path.dirname(os.path.abspath(fdir).rstrip("\\/"))
    log = os.path.join(att, "run.log")
    if os.path.exists(log) and PRESENT_DUMP.search(read_text(log)):
        return "run.log names dumped presents, but no frame is on disk and no %s holds them" % SIDECAR
    return None


_names_cache = {}


def _cached_by_stat(kind, path, fn):
    key = (kind, os.path.normcase(os.path.abspath(path)), json.dumps(_stat_sig(path), sort_keys=True))
    if key not in _names_cache:
        _names_cache[key] = fn()
    return _names_cache[key]


LINE_TS = re.compile(r"\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]")


def logged_dumps(log):
    """{frame name: [timestamp or None, ...]} of a run.log's [PRESENT_DUMP] lines in log order - EVERY line, by file
    name (a moved roll logs its old path); the timestamp is the line's own (None when it has none). A name with two
    or more lines is an AMBIGUOUS write (present_index). Cached per process on the log's size + mtime."""
    if not log or not marker_exists(log):
        return {}

    def load():
        txt, out = read_text(log), {}
        for m in DUMP_NAME.finditer(txt):
            t = LINE_TS.match(txt, txt.rfind("\n", 0, m.start()) + 1)
            out.setdefault(re.split(r"[\\/]", m.group(1))[-1], []).append(t.group(1) if t else None)
        return out
    return _cached_by_stat("dumps", log, load)


def logged_names(att):
    """The frame names run.log's [PRESENT_DUMP] lines say were written (logged_dumps)."""
    return set(logged_dumps(os.path.join(att, "run.log")))


def outcome_frames(att):
    """{name: bytes} of outcome.json's BMP list (the driver's end-of-run listing), or None when there is none or it
    cannot be read. Parsed STRICTLY (Astra round 6: no falsy value may read as "no frames"): the document must be an
    object; "frames" a list of objects (or one object: PowerShell's ConvertTo-Json writes a one-element list as the
    element), each with a string name and, for a .bmp, a byte count; a name listed twice with two sizes is unreadable.
    Only an absent "frames" key lists nothing (one older outcome.json in the archives). Archive survey 2026-10-14:
    1,519 outcome.json - all objects, 1,518 frame lists + that one, 552,785 entries all {name: str, bytes: int}."""
    p = os.path.join(att, "outcome.json")
    if not marker_exists(p):
        return None

    def load():
        try:
            with open(p, encoding="utf-8-sig") as f:
                o = json.load(f)
        except (OSError, ValueError):
            return None
        if not isinstance(o, dict):
            return None
        if "frames" not in o:
            return {}
        fr = [o["frames"]] if isinstance(o["frames"], dict) else o["frames"]
        if not isinstance(fr, list):
            return None
        out = {}
        for x in fr:
            if not isinstance(x, dict) or not isinstance(x.get("name"), str):
                return None
            if x["name"].endswith(".bmp"):
                if not _int(x.get("bytes")) or out.get(x["name"], x["bytes"]) != x["bytes"]:
                    return None
                out[x["name"]] = x["bytes"]
        return out
    return _cached_by_stat("outcome", p, load)


def outcome_names(att):
    return set(outcome_frames(att) or {})


def expected_names(fdir):
    """The frame names the attempt's own records say were dumped: run.log [PRESENT_DUMP] lines and outcome.json."""
    att = os.path.dirname(os.path.abspath(fdir).rstrip("\\/"))
    return logged_names(att) | outcome_names(att)


def frame_names(fdir):
    """Sorted frame file names of a frames folder's EXPECTED inventory. When a sidecar applies, its FIXED inventory
    (every source's names are inside it, or it would not apply) - a truncated log or a lost outcome.json cannot
    shrink it. Otherwise the BMPs on disk plus every frame run.log / outcome.json name (and, for a rejected sidecar,
    the frames it claims: claimed_names). A frame that went missing stays in the inventory, unmeasured, and every
    sample keeps its identity."""
    doc = read_sidecar(fdir)[0]
    if doc is not None:
        return sorted(doc["frames"])
    return sorted(n for n in set(_bmps(fdir)) | expected_names(fdir) | claimed_names(fdir) if n.endswith(".bmp"))


def claimed_names(fdir):
    """The frame names a sidecar that may NOT be used still claims (a rejected one that parses), else none. They only
    widen an archived roll's inventory - every frame of it is unmeasured then (archived_problem) - so its counts say
    "0 of 360", not "1 of 1" (Astra 2026-10-14 round 5). Cached per process with the sidecar read."""
    fdir = os.path.abspath(fdir).rstrip("\\/")
    key = ("claimed", os.path.normcase(fdir))
    if key not in _sidecars:
        names = set()
        if read_sidecar(fdir)[1]:
            try:
                with open(sidecar_path(fdir), encoding="utf-8") as f:
                    fr = json.load(f).get("frames")
                names = {n for n in fr if isinstance(n, str) and n.endswith(".bmp")} if isinstance(fr, dict) else set()
            except (OSError, ValueError, AttributeError):
                names = set()
        _sidecars[key] = names
    return _sidecars[key]


FRAME_NAME_P = re.compile(r"_p(\d+)_\d+x\d+\.bmp$")   # a present dump's file name -> its present number


def present_index(fdir):
    """ONE ENTRY PER PRESENT of the expected inventory (frame_names) - the unit every share, span sample and freeze
    middle is graded on, never a log line (Astra 2026-10-14 round 5: a repeated line must not weight a frame).
    -> {present: (frame name, None)} or, for an AMBIGUOUS present, {present: (None, why)}: two or more inventory
    frames claim it, or run.log's [PRESENT_DUMP] lines name its frame more than once (written twice, or a repeated
    line: which version was measured, and when, is unknown). An ambiguous present grades unmeasured."""
    att = os.path.dirname(os.path.abspath(fdir).rstrip("\\/"))
    dumps = logged_dumps(os.path.join(att, "run.log"))
    out = {}
    for p, names in _present_groups(fdir).items():
        if len(names) > 1:
            out[p] = (None, "%d frames claim present %d (%s)" % (len(names), p, ", ".join(names)))
        elif len(dumps.get(names[0], [])) > 1:
            out[p] = (None, "run.log names %s %d times: an ambiguous write" % (names[0], len(dumps[names[0]])))
        else:
            out[p] = (names[0], None)
    return out


def _present_groups(fdir):
    """{present: sorted inventory frame names claiming it} (frame_names)."""
    by = {}
    for n in frame_names(fdir):
        m = FRAME_NAME_P.search(n)
        if m:
            by.setdefault(int(m.group(1)), []).append(n)
    return {p: sorted(v) for p, v in by.items()}


def present_frames(fdir):
    """[(present, frame name or None)] - present_index in frame-name order (an ambiguous present, None, sorts by the
    first name claiming it): the order a span's scene sample is drawn in, one entry per present."""
    groups, idx = _present_groups(fdir), present_index(fdir)
    return [(p, idx[p][0]) for p in sorted(idx, key=lambda q: (groups[q][0], q))]


def _what_key(what):
    """The cache key prefix of a measurement: the structure's names its gradient level (its definition)."""
    return "structure@v%d@g%d@s%s" % (STRUCT_VERSION, STRUCT_GRAD, STRUCT_SIDE_SPREAD_MIN) if what == "structure" else what


def compatible_structure(doc):
    """Stored structure may be reused only under its exact measurement definition."""
    rule = (doc or {}).get("rule") or {}
    return (rule.get("struct_grad") == STRUCT_GRAD and rule.get("struct_version") == STRUCT_VERSION
            and rule.get("struct_side_spread_min") == STRUCT_SIDE_SPREAD_MIN)


def stored(path, what):
    """The sidecar measurement ('center' / 'sides' / 'structure') of a frame whose BMP is absent, or None. A stored
    structure is served only when the header names its current gradient and spatial definition; incompatible
    structure is None, while independently compatible center/side luma measurements remain available."""
    doc = read_sidecar(os.path.dirname(os.path.abspath(path)))[0]
    if doc is None:
        return None
    if what == "structure" and not compatible_structure(doc):
        return None
    v = (doc["frames"].get(os.path.basename(path)) or {}).get(what)
    return tuple(v) if valid_measure(v) else None


def measure(path, what="center"):
    """frame_scene ('center') or side_scene ('sides') of one present, cached by path + size + mtime. A present
    whose BMP is gone reads its sidecar measurement instead (None when there is none: unmeasured, never a
    guess); a BMP on disk wins (in an archived roll only when the applicable sidecar fixed that very version). A frame
    of an archived roll whose frames may not be read (archived_problem) is None, BMP or not."""
    global _cache, _dirty
    if archived_problem(os.path.dirname(os.path.abspath(path))):
        return None
    if not os.path.exists(path):
        return stored(path, what)
    cp = _cache_path()
    if cp is None:
        return MEASURES[what](path)
    if _cache is None:
        try:
            with open(cp, encoding="utf-8") as f:
                _cache = json.load(f)
        except Exception:   # lane C r13 (Astra 762d3d4 r12 P2: a deeply nested cache raised RecursionError, read as
            _cache = {}     # damaged frames): an unusable optional cache is no cache - the frames are measured directly
        if not isinstance(_cache, dict) or cache_version(_cache) != MEASURE_VERSION:
            # measured under another definition (or not a cache): never served - and never overwritten either (its
            # entries may be the only record of deleted frames): this process measures without a cache
            _cache = {CACHE_VERSION_KEY: MEASURE_VERSION, "__foreign__": True}
        _cache[CACHE_VERSION_KEY] = MEASURE_VERSION
    st = os.stat(path)
    # the structure's key carries its gradient level: a value measured under another level is never served
    key = "%s|%s|%d|%d" % (_what_key(what), os.path.normcase(os.path.abspath(path)), st.st_size, st.st_mtime_ns)
    if not valid_measure(_cache.get(key)):  # absent, or a corrupt cached value: measure the BMP again
        _cache[key] = list(MEASURES[what](path))
        _dirty = True
    return tuple(_cache[key])


def save_cache():
    global _dirty
    cp = _cache_path()
    if not (_dirty and _cache and cp) or _cache.get("__foreign__"):
        return
    try:
        with open(cp + ".tmp", "w", encoding="utf-8") as f:
            json.dump(_cache, f)
        os.replace(cp + ".tmp", cp)
        _dirty = False
    except OSError:
        pass


def classify(path, context):
    """'gameplay' / 'menu_world' / 'dark' / 'menu_black' (module docstring), or 'unmeasured' when a measurement the
    rule needs exists neither as a BMP nor in the sidecar."""
    c = measure(path, "center")
    if not valid_measure(c):
        return "unmeasured"
    lu, dk = c
    if lu < SCENE_LUMA_MIN or dk > SCENE_DARK_MAX:
        # under the luma floor: drawn all the same when the region has a scene's edges (STRUCT_EDGE_MIN); a frame
        # whose structure is not measured (its BMP gone, its sidecar from before the measurement) stays dark
        st = measure(path, "structure")
        if not valid_measure(st) or st[1] < STRUCT_EDGE_MIN:
            return "dark"
    if context == "gameplay":
        return "gameplay"
    s = measure(path, "sides")
    if not valid_measure(s):
        return "unmeasured"
    sl, sd = s
    return "menu_world" if (sl >= SIDE_LUMA_MIN and sd <= SIDE_DARK_MAX) else "menu_black"


# ------------------------------------------------------------------------------------------------ a roll

def contexts(txt):
    """[(timestamp, context)] of the `[PC_CONTROLS] context` lines (timestamps compare as strings)."""
    return [(m.group(1), m.group(2)) for m in CONTEXT.finditer(txt)]


def context_at(timeline, ts):
    cur = None
    for t, c in timeline:
        if t > ts:
            break
        cur = c
    return cur


def frames_by_present(fdir):
    """{present: frame path} of the UNAMBIGUOUS presents of the expected inventory (present_index): one frame per
    present; an ambiguous present has no path, so it grades unmeasured. Paths without a BMP do not exist: measure()
    reads their stored measurement, or gives None (unmeasured)."""
    return {p: os.path.join(fdir, n) for p, (n, _) in present_index(fdir).items() if n}


def freeze_middles(txt):
    """{freeze id: (present, timestamp) of its MIDDLE logged dump ([SIM_FREEZE_DUMP] lines), or None} - one row per
    present (Astra 2026-10-14 round 5): a present the log lists more than once for a freeze makes that freeze's
    middle ambiguous (None), so a repeated line can never move the middle onto it."""
    pres = {}
    for m in FREEZE_DUMP.finditer(txt):
        pres.setdefault(int(m.group(2)), []).append((int(m.group(3)), m.group(1)))
    out = {}
    for fid, cand in pres.items():
        ps = [p for p, _ in cand]
        out[fid] = None if len(set(ps)) != len(ps) else sorted(cand)[len(cand) // 2]
    return out


def freeze_world(roll_dir, log, freezes):
    """The world class of each GRADED freeze of a freeze roll (flicker_gate's rows: 'ingame' and no 'note'),
    judged on its middle dumped present -> {class: [freeze ids]} incl. 'unmeasured' (no dumped frame, an ambiguous
    middle, or its BMP is gone and the sidecar holds no measurement of it). The middle is taken over the freeze's
    LOGGED dumps ([SIM_FREEZE_DUMP], one per present: freeze_middles), never over the ones still readable: a missing
    middle frame makes the freeze unmeasured, it is never replaced by a neighbour (Astra 2026-10-14)."""
    graded = [r["freeze"] for r in freezes if r.get("ingame") and "note" not in r]
    txt = read_text(log)
    timeline = contexts(txt)
    mids = freeze_middles(txt)
    byp = frames_by_present(os.path.join(roll_dir, "drive", "attempt1", "frames"))
    out = {"gameplay": [], "menu_world": [], "dark": [], "menu_black": [], "unmeasured": []}
    for fid in graded:
        mid = mids.get(fid)
        if not mid or mid[0] not in byp:
            out["unmeasured"].append(fid)
            continue
        p, t = mid
        out[classify(byp[p], context_at(timeline, t))].append(fid)
    return out


def world_count(ws):
    return sum(len(ws.get(k, [])) for k in WORLD)


def world_line(ws):
    """'world 11 (gameplay 1 + menu over world 10) dark [..] menu over black [..]'"""
    s = "world %d (gameplay %d + menu over world %d)" % (world_count(ws), len(ws.get("gameplay", [])),
                                                         len(ws.get("menu_world", [])))
    for k, label in (("dark", "dark"), ("menu_black", "menu over black"), ("unmeasured", "unmeasured")):
        if ws.get(k):
            s += " %s %s" % (label, ws[k])
    return s


def inventory_rows(fdir, log, era_start_ms, classify=True):
    """THE ROWS A SHARE IS COMPUTED OVER (a meter's world share, a span's in-play share): EXACTLY ONE ROW PER PRESENT
    of the expected inventory (present_index; Astra 2026-10-14 rounds 4-5) - never one per log line, so a repeated
    line cannot weight a frame and a truncated log cannot shrink the denominator. A present whose frame the log names
    exactly once carries that line's timestamp, in_play (dumped at or after era_start_ms - epoch ms; None = no
    in-game era: nothing is in play - in the gameplay context) and, with classify, its world class ('unmeasured'
    when it has neither a readable BMP nor a stored measurement). A present no log line names (timing unknown) and an
    ambiguous one are (present, None, None, 'unmeasured'): in the denominator, never in play, never drawn.
    -> [(present, timestamp or None, in_play True / False / None[, class])], sorted by present."""
    classify_frame = globals()["classify"]
    dumps = logged_dumps(log)
    timeline = contexts(read_text(log)) if log and marker_exists(log) else []
    rows = []
    for p, (name, _why) in sorted(present_index(fdir).items()):
        lines = (dumps.get(name) or []) if name else []
        ts = lines[0] if len(lines) == 1 else None
        if ts is None:
            rows.append((p, None, None, "unmeasured") if classify else (p, None, None))
            continue
        ctx = context_at(timeline, ts)
        play = era_start_ms is not None and ts_ms(ts) >= era_start_ms and ctx == "gameplay"
        rows.append((p, ts, play, classify_frame(os.path.join(fdir, name), ctx)) if classify else (p, ts, play))
    return rows


def presents_at(log, when_ms):
    """The roll's present count at epoch ms `when_ms`: from the cumulative `[NATIVE_METER] w=` window frames
    (SR_HIGHFPS_PACE=1 logs them every 2 s), else from `[GPU_CP] census ... swaps=` (every 120 swaps). One present
    per guest swap on this path (p2 checked: meter frames 17,390 vs census swaps 17,400 at the same second).
    -> (presents, total presents in the roll) or (None, None)."""
    txt = read_text(log)
    series, cum = [], 0
    for m in METER_WINDOW.finditer(txt):
        cum += int(m.group(2))
        series.append((ts_ms(m.group(1)), cum))
    if not series:
        series = [(ts_ms(m.group(1)), int(m.group(2))) for m in CP_CENSUS.finditer(txt)]
    if not series:
        return None, None
    for t, c in series:
        if t >= when_ms:
            return c, series[-1][1]
    return series[-1][1], series[-1][1]


# ------------------------------------------------------------------------------------------------ the sidecar (writers)

def _git_head():
    try:
        import subprocess
        r = subprocess.run(["git", "-C", os.path.dirname(os.path.abspath(__file__)), "rev-parse", "HEAD"],
                           capture_output=True, text=True, timeout=20)
        return (r.stdout.strip() or None) if r.returncode == 0 else None
    except Exception:  # provenance is best effort; the measurements do not depend on it
        return None


def sidecar_header(fdir, how, tool, provenance):
    """The header every sidecar carries (module docstring): what was measured, how, when, by which tool and rule."""
    fdir = os.path.abspath(fdir).rstrip("\\/")
    return {"schema": SIDECAR_SCHEMA, "measure_version": MEASURE_VERSION, "frames_dir": os.path.basename(fdir),
            "written": datetime.datetime.now().astimezone().isoformat(timespec="seconds"), "how": how,
            "writer": {"tool": tool, "commit": _git_head(), "python": sys.version.split()[0]},
            "provenance": provenance, "rule": rule_constants(),
            "run_log": _stat_sig(os.path.join(os.path.dirname(fdir), "run.log"))}


def sidecar_text(doc):
    """The sidecar as JSON text: the header indented, one line per frame (thousands of frames stay greppable)."""
    head = json.dumps({k: v for k, v in doc.items() if k != "frames"}, indent=1)
    rows = ["%s: %s" % (json.dumps(n), json.dumps(doc["frames"][n])) for n in sorted(doc["frames"])]
    return head[:-2] + ',\n "frames": {\n  ' + ",\n  ".join(rows) + "\n }\n}\n"


def write_sidecar(fdir, doc):
    """Write doc as fdir's sidecar (atomically: a temp file, then a rename) -> its path. Refuses a malformed frame
    table or an inventory header that does not add up; whether the inventory reconciles with the attempt's records
    (sidecar_doc_problem) is the READER's judgement - a recovery that cannot reconcile is still written, and rejected
    when read."""
    sp = sidecar_path(fdir)
    bad = sidecar_frames_problem(doc["frames"]) or inventory_header_problem(doc)
    if bad:
        raise ValueError("refusing to write %s: %s" % (SIDECAR, bad))
    txt = sidecar_text(doc)
    json.loads(txt)  # never leave a sidecar the reader cannot parse
    with open(sp + ".tmp", "w", encoding="utf-8") as f:
        f.write(txt)
    os.replace(sp + ".tmp", sp)
    _forget(fdir)
    return sp


def _folder_files(folder):
    """{name: os.stat} of the FILES directly in a folder; raises on any enumeration error and on a subfolder (whose
    content nothing here records, and the GC's recursive delete would destroy)."""
    out = {}
    for e in os.scandir(folder):   # an enumeration error propagates
        p = os.path.join(folder, e.name)
        if os.path.isdir(p):
            raise RuntimeError("%s holds a subfolder (%s): nothing records its content" % (folder, e.name))
        out[e.name] = os.stat(p)
    return out


def measure_into_sidecar(fdir, tool="tools/perf/world_floor.py sidecar"):
    """Measure EVERY BMP of a frames folder (center and sides, one decode each, straight from the pixels - the
    cache is neither read nor written) into its sidecar, and FIX ITS INVENTORY: every file in the folder with its
    size (and each BMP's mtime_ns) as stat'ed now, the other (non-BMP) files with their sizes, the total bytes - what
    the GC's deletion record must then equal - and, as on_disk false / unmeasured, every frame run.log or outcome.json
    names that is not in the folder. Frames an applicable sidecar already holds whose BMP is gone are kept (on_disk
    false: they were measured earlier). Raises if the folder cannot be enumerated or holds a subfolder, or any BMP
    cannot be measured or changes while it is measured, or the reader would reject the result (sidecar_doc_problem:
    e.g. two frames claiming one present, an unreadable outcome.json); nothing is written then. -> (sidecar path or None when the
    folder holds no BMP, BMPs measured, frames kept)."""
    fdir = os.path.abspath(fdir).rstrip("\\/")
    if not os.path.isdir(fdir):
        raise FileNotFoundError("no frames folder: %s" % fdir)
    files = _folder_files(fdir)
    old = read_sidecar(fdir)[0]
    frames, others = {}, {}
    for name in sorted(files):
        st = files[name]
        if not name.endswith(".bmp"):
            others[name] = {"size": st.st_size, "how": "stat", "evidence": "os.stat when the folder was measured"}
            continue
        p = os.path.join(fdir, name)
        c, s, t = measure_bmp(p)
        if _stat_sig(p) != {"size": st.st_size, "mtime_ns": st.st_mtime_ns}:
            raise RuntimeError("%s changed while it was measured" % p)
        frames[name] = {"on_disk": True, "size": st.st_size, "mtime_ns": st.st_mtime_ns, "center": list(c),
                        "sides": list(s), "structure": list(t), "source": "bmp"}
    n = len(frames)
    if not n:
        return None, 0, 0
    kept = 0
    for name, e in ((old or {}).get("frames") or {}).items():
        if name not in frames:
            frames[name] = dict(e, on_disk=False)
            if not compatible_structure(old):
                # The BMP is absent: never relabel an old gradient/spatial definition as today's measurement.
                frames[name].pop("structure", None)
            kept += 1
    att = os.path.dirname(fdir)
    sizes = outcome_frames(att) or {}
    for name in sorted(expected_names(fdir) - set(frames)):
        frames[name] = {"on_disk": False, "size": sizes.get(name), "mtime_ns": None, "center": None, "sides": None,
                        "source": "named", "missing": "named by run.log / outcome.json, not in the folder when measured"}
    bmp, other, total = inventory_totals(frames, others)
    prov = {"from": "the BMPs in this frames folder, measured by world_floor.measure_bmp", "bmps": n, "kept": kept}
    if kept:
        prov["kept_from"] = {k: old.get(k) for k in ("written", "how", "writer", "provenance", "rule")}
        prov["kept_structure_compatible"] = compatible_structure(old)
    doc = sidecar_header(fdir, "measured", tool, prov)
    doc["inventory"] = {"source": "the files in the frames folder when it was measured (os.stat)",
                        "count": len(frames), "on_disk": n, "bmp_bytes": bmp, "other_files": others,
                        "total_bytes": total}
    doc["frames"] = frames
    bad = sidecar_doc_problem(doc, fdir)
    if bad:   # never leave a sidecar the reader would reject: that would make the whole roll unmeasured
        raise RuntimeError("the measured inventory would be rejected by the reader: %s" % bad)
    return write_sidecar(fdir, doc), n, kept


def sidecar_covers(fdir, scratch, exact=False):
    """The GC's last check before it deletes `scratch` (Astra 2026-10-14 round 4: names are not enough). -> (why or
    None, total bytes of scratch's files). OK only when fdir's sidecar APPLIES (sidecar_doc_problem) and every file
    in `scratch` is in its fixed inventory as THAT version: each BMP an on_disk frame with the same size and
    mtime_ns and a valid center + sides measurement, each other file listed with the same size. exact: the folder
    holds exactly the inventory's on-disk files and their bytes equal its total_bytes (the folder the sidecar was
    measured from, unchanged - its deletion record will then reconcile). A subfolder or an enumeration error fails."""
    _forget(fdir)
    doc, prob = read_sidecar(fdir)
    if doc is None or prob is not None:
        return (prob or "no %s beside %s" % (SIDECAR, fdir)), None
    files = _folder_files(scratch)
    frames, others = doc["frames"], doc["inventory"]["other_files"]
    for name in sorted(files):
        st = files[name]
        if name.endswith(".bmp"):
            e = frames.get(name)
            if not e or not e["on_disk"]:
                return "%s is not an on-disk frame of the inventory" % name, None
            if (e["size"], e["mtime_ns"]) != (st.st_size, st.st_mtime_ns):
                return "%s is another version (size %d, mtime_ns %d) than the one measured (%r, %r)" % (
                    name, st.st_size, st.st_mtime_ns, e["size"], e["mtime_ns"]), None
            if not valid_measure(e.get("center")) or not valid_measure(e.get("sides")):
                return "%s holds no valid center + sides measurement of %s" % (SIDECAR, name), None
        else:
            o = others.get(name)
            if not o or o["size"] != st.st_size:
                return "other file %s (%d bytes) is not in the inventory as that size" % (name, st.st_size), None
    total = sum(st.st_size for st in files.values())
    if exact:
        on = {n for n, e in frames.items() if e["on_disk"]}
        if {n for n in files if n.endswith(".bmp")} != on or {n for n in files if not n.endswith(".bmp")} != set(others):
            return "the folder does not hold exactly the inventory's files", None
        if total != doc["inventory"]["total_bytes"]:
            return "the folder holds %d bytes, the inventory %d" % (total, doc["inventory"]["total_bytes"]), None
    return None, total


def main(argv):
    """python tools/perf/world_floor.py sidecar <frames dir | attempt dir>
    Measures every BMP into <attempt>/frames_measured.json (tools/sr_artifacts.ps1's frame-scratch GC runs this before
    it deletes a roll's frames, and deletes nothing when it fails). Exit 0: written (or no BMP to keep); 1: failed.
    python tools/perf/world_floor.py covers <attempt frames dir> <scratch folder> [--exact]
    Exit 0 (COVERS_OK <bmps> <total bytes> exact|subset <scratch>) only when the attempt's sidecar applies and holds
    every file of the scratch folder as that version (sidecar_covers)."""
    if len(argv) in (3, 4) and argv[0] == "covers" and (len(argv) == 3 or argv[3] == "--exact"):
        exact = len(argv) == 4
        try:
            why, total = sidecar_covers(argv[1], argv[2], exact=exact)
            n = len([f for f in os.listdir(argv[2]) if f.endswith(".bmp")])
        except Exception as x:
            why = "%s: %s" % (type(x).__name__, x)
        if why:
            print("COVERS_FAIL %s" % why, file=sys.stderr)
            return 1
        print("COVERS_OK %d %d %s %s" % (n, total, "exact" if exact else "subset", argv[2]))
        return 0
    if len(argv) != 2 or argv[0] != "sidecar":
        print(main.__doc__)
        return 2
    fdir = argv[1]
    if os.path.isdir(os.path.join(fdir, "frames")):
        fdir = os.path.join(fdir, "frames")
    try:
        sp, n, kept = measure_into_sidecar(fdir)
    except Exception as x:  # the GC reads the exit code: any failure keeps the frames
        print("world_floor sidecar FAILED for %s: %s: %s" % (fdir, type(x).__name__, x), file=sys.stderr)
        return 1
    if sp is None:
        print("SIDECAR_EMPTY 0 %s (no BMP to measure)" % os.path.abspath(fdir))
    else:
        print("SIDECAR_OK %d %s (%d BMPs measured, %d kept)" % (n, sp, n, kept))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
