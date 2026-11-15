#!/usr/bin/env python3
"""native_ledger.py - the NATIVE OBLIGATION LEDGER (iter-467): docs/native_ledger.json, machine-checked.

Each record is one native obligation (a row converted, EDRAM at zero, audio native, a defect that gates
native work). Its STATUS IS COMPUTED here from the evidence files the record points at; nobody writes it.
An obligation is CLOSED only when
  - every evidence item exists and matches (its verdict / pattern / field), read fail-closed: a missing,
    unreadable or unparsable item never passes, and an archive path absent on this machine is reported
    'unavailable', never as a pass; an item with a `require` (its roll's precondition: a line of the roll's
    run.log) grades NO DATA, a FAIL, when the roll did not meet it;
  - for every observation it guards (pixels, meter, audio, ...) at least one passing item OBSERVES that
    thing at an authority >= the guard's minimum (the rule that keeps a packed-word compare from
    "proving" frames: it observes generation, not pixels);
  - no residual unknown and no contradiction is open (each is open until its resolved_by evidence passes);
  - every dependency is closed.
Otherwise the status is BLOCKED (a dependency is not closed), IN-PROGRESS (some evidence passes) or
OPEN (none does). Absence of evidence never closes anything.

A frozen or span roll proves nothing over a black world (a black 3D scene is perfectly stable). Freeze and gate
items are graded by the flicker gate's OWN grader (tools/perf/flicker_gate.py, read-only on the stored diff
outputs), so a ledger status and a gate verdict apply one world rule (tools/perf/world_floor.py): `min_lit` graded
freezes must show the world (the player context `gameplay`, or the pause menu with the world showing around its
panel), a gate's span must be 80% in play, and meter items with `world_min` need that share of their ingame sparse
frames drawn. Frames the frame-scratch GC deleted grade from the attempt's frames_measured.json (the stored
measurements, world_floor.py); a frame with no measurement grades 'unmeasured', never as drawn.

    python tools/native_ledger.py                     # the table (id, status, why); exit 2 on schema errors
    python tools/native_ledger.py --strict            # + exit 1 if a record the docs call done/baked is not
                                                      #   closed, a claim quote is no longer in its doc, or a
                                                      #   status_docs block is stale
    python tools/native_ledger.py -v [--id ID]        # every evidence item's result
    python tools/native_ledger.py --json out.json     # full results + the closure digest
    python tools/native_ledger.py --write-status DOC  # regenerate DOC's <!-- native-ledger:begin:STYLE --> block
    python tools/native_ledger.py --markdown readme   # print a status block (roadmap | readme style)
    python tools/native_ledger.py sheet <roll> <jpg>  # contact sheet of a freeze roll's graded freezes + world class

Authority scale (low -> high; docs/TOOLKIT.md 1.8i):
  static        source / asm / IDA reading, a commit on dev, a default in the code
  generation    generated output vs the XDK's bytes (dry CHECK), L6 / golden-call difftest
  replay        Xenia .xtr replays byte-identical (deterministic; runs NO guest code)
  live-observe  a live roll's counters, witnesses, meter windows, the flicker span + scene floor
  live-exact    exact checks inside a live roll: the CPPM4 execution split + dedup CHECK, VERIFY modes
                (the oracle path decides beside the native one), the dense-freeze pixel stability
  frozen-ab     the frozen-pass per-present A/B against the guest / oracle path (pixels exact, present
                for present, with the toggle witnessed)
  xenia         the Xenia oracle: golden frames, live Xenia audio / pixels, the golden dump
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
LEDGER = os.path.join(REPO, "docs", "native_ledger.json")
SCHEMA = "sr-native-ledger/1"

AUTHORITY = ["static", "generation", "replay", "live-observe", "live-exact", "frozen-ab", "xenia"]
OBSERVES = ["build", "generation", "pixels", "meter", "audio", "guest-answers", "timing", "census"]
# What the docs say about an obligation (the strongest claim any doc makes; `claim_refs` quote it):
#   none / planned / built (code exists, default OFF) / in-progress (being proven) /
#   shipping-unproven (default ON in the build, and the docs SAY its proof is incomplete) /
#   done (proven; not a shipping toggle) / baked (proven and default ON).
# --strict fails when the docs claim done / baked and the evidence does not close the record.
CLAIMS = ["none", "planned", "built", "in-progress", "shipping-unproven", "done", "baked"]
DONE_CLAIMS = ("done", "baked")
STATUS_ORDER = ("closed", "in-progress", "blocked", "open")
KINDS = {
    # kind: required keys (besides id / kind / authority / observes)
    "file": ["path"],
    "text": ["path"],
    # two counters of ONE log (native/dreg-bake): lhs / rhs = {select, field}; the last line lhs selects against the
    # first line rhs selects after it - lhs.field <= rhs.field
    "text_le": ["path", "lhs", "rhs"],
    "json": ["path", "checks"],
    "flicker_gate": ["path"],
    "freeze_grade": ["path"],
    "replay_cmp": ["path"],
    "check_report": ["path"],
    "meter": ["path", "field"],
    "meter_delta": ["path", "base", "field"],
    "git_ancestor": ["commit"],
    "source": ["path", "pattern"],
}
OPS = ("eq", "ne", "ge", "le", "gt", "lt", "in", "contains", "not_contains", "exists", "empty", "nonempty",
       "len_ge", "regex")

PASS, FAIL, UNAVAIL = "pass", "fail", "unavailable"
NO_DATA = "NO DATA"  # a FAIL detail's prefix: the item's roll did not meet its precondition (`require`, require_problem)
# An item's expected result (check_item): pass (evidence, the default), fail / no_data (witness lists only).
EXPECTS = ("pass", "fail", "no_data")


# ---------------------------------------------------------------------------------------------- schema

def schema_errors(doc):
    errs = []
    if not isinstance(doc, dict):
        return ["the ledger is not a JSON object"]
    if doc.get("schema") != SCHEMA:
        errs.append("schema must be %r" % SCHEMA)
    roots = doc.get("roots", {})
    if not isinstance(roots, dict) or "repo" not in roots:
        errs.append("roots must be an object with at least 'repo'")
    obs = doc.get("obligations")
    if not isinstance(obs, list) or not obs:
        return errs + ["obligations must be a non-empty list"]
    ids = {}
    for i, o in enumerate(obs):
        where = "obligations[%d]" % i
        if not isinstance(o, dict):
            errs.append(where + " is not an object")
            continue
        oid = o.get("id")
        if not isinstance(oid, str) or not re.match(r"^[a-z0-9][a-z0-9_.\-]*$", oid or ""):
            errs.append(where + ": id must be a lowercase dotted string")
            continue
        where = oid
        if oid in ids:
            errs.append(where + ": duplicate id")
        ids[oid] = o
        for k in ("status", "computed_status", "closed"):
            if k in o:
                errs.append(where + ": '%s' is COMPUTED by tools/native_ledger.py - remove it from the record" % k)
        if not isinstance(o.get("version"), int) or o["version"] < 1:
            errs.append(where + ": version must be an integer >= 1")
        for k in ("title", "strand", "lane", "observation"):
            if not isinstance(o.get(k), str) or not o[k].strip():
                errs.append(where + ": '%s' must be a non-empty string" % k)
        if o.get("claimed", "none") not in CLAIMS:
            errs.append(where + ": claimed must be one of %s" % CLAIMS)
        guards = o.get("guards")
        if not isinstance(guards, list) or not guards:
            errs.append(where + ": guards must be a non-empty list of {observes, min_authority}")
        else:
            for g in guards:
                if not isinstance(g, dict) or g.get("observes") not in OBSERVES or g.get("min_authority") not in AUTHORITY:
                    errs.append(where + ": bad guard %r (observes in %s, min_authority in %s)" % (g, OBSERVES, AUTHORITY))
        ev = o.get("evidence")
        if not isinstance(ev, list):
            errs.append(where + ": evidence must be a list")
            ev = []
        seen = set()
        for e in ev:
            errs += item_errors(where, e, roots, seen)
            if isinstance(e, dict) and e.get("expect") in ("fail", "no_data"):
                errs.append(where + "/" + str(e.get("id")) + ": expect '%s' belongs in a witness list, never in "
                            "the evidence that closes an obligation" % e.get("expect"))
        for key in ("residual_unknowns", "contradictions"):
            lst = o.get(key, [])
            if not isinstance(lst, list):
                errs.append(where + ": %s must be a list" % key)
                continue
            for u in lst:
                if not isinstance(u, dict) or not isinstance(u.get("id"), str) or not isinstance(u.get("text"), str):
                    errs.append(where + ": %s entries need id + text" % key)
                    continue
                if u["id"] in seen:
                    errs.append(where + ": duplicate id %s" % u["id"])
                seen.add(u["id"])
                for sub in ("resolved_by", "witness"):
                    for e in u.get(sub, []) or []:
                        errs += item_errors(where + "/" + u["id"] + "/" + sub, e, roots, seen)
        for r in o.get("claim_refs", []) or []:
            if not isinstance(r, dict) or not isinstance(r.get("doc"), str) or not isinstance(r.get("quote"), str):
                errs.append(where + ": claim_refs entries need doc + quote")
        for c in o.get("trunk_commits", []) or []:
            if not isinstance(c, str) or not re.match(r"^[0-9a-f]{7,40}$", c):
                errs.append(where + ": trunk_commits entry %r is not a commit id" % c)
    m = doc.get("meter")
    if m is not None:
        seen = set()
        if not isinstance(m, dict) or not isinstance(m.get("items"), list):
            errs.append("meter must be an object with an 'items' list of meter evidence items")
        else:
            for e in m["items"] + [b.get("item") for b in m.get("branch", []) or [] if isinstance(b, dict)]:
                errs += item_errors("meter", e, roots, seen)
                if isinstance(e, dict) and e.get("kind") != "meter":
                    errs.append("meter/%s: the meter section takes kind 'meter' items only" % e.get("id"))
    for rel in doc.get("status_docs", []) or []:
        if not isinstance(rel, str):
            errs.append("status_docs entries are repo-relative doc paths")
    for oid, o in ids.items():
        for d in o.get("depends_on", []) or []:
            if d not in ids:
                errs.append(oid + ": depends_on unknown id %r" % d)
    # cycles
    state = {}

    def visit(n, stack):
        if state.get(n) == 1:
            errs.append("dependency cycle: " + " -> ".join(stack + [n]))
            return
        if state.get(n) == 2 or n not in ids:
            return
        state[n] = 1
        for d in ids[n].get("depends_on", []) or []:
            visit(d, stack + [n])
        state[n] = 2
    for n in ids:
        visit(n, [])
    return errs


def item_errors(where, e, roots, seen):
    errs = []
    if not isinstance(e, dict):
        return [where + ": evidence item is not an object"]
    eid = e.get("id")
    if not isinstance(eid, str) or not eid:
        return [where + ": evidence item without id"]
    w = where + "/" + eid
    if eid in seen:
        errs.append(w + ": duplicate evidence id")
    seen.add(eid)
    kind = e.get("kind")
    if kind not in KINDS:
        return errs + [w + ": unknown kind %r (one of %s)" % (kind, sorted(KINDS))]
    for k in KINDS[kind]:
        if k not in e:
            errs.append(w + ": kind %s needs '%s'" % (kind, k))
    if e.get("authority") not in AUTHORITY:
        errs.append(w + ": authority must be one of %s" % AUTHORITY)
    if e.get("observes") not in OBSERVES:
        errs.append(w + ": observes must be one of %s" % OBSERVES)
    if e.get("root", "repo") not in roots:
        errs.append(w + ": unknown root %r" % e.get("root"))
    if e.get("expect", "pass") not in EXPECTS:
        errs.append(w + ": expect must be one of %s" % " / ".join(EXPECTS))
    if e.get("expect") == "no_data" and not e.get("require"):
        errs.append(w + ": expect 'no_data' needs the item's require (the precondition its roll did not meet)")
    if kind == "json":
        for c in e.get("checks", []):
            if not isinstance(c, dict) or "field" not in c or c.get("op", "eq") not in OPS:
                errs.append(w + ": bad json check %r" % (c,))
    if kind == "text_le":
        for side in ("lhs", "rhs"):
            s = e.get(side)
            if not (isinstance(s, dict) and isinstance(s.get("select"), str) and s["select"] and
                    isinstance(s.get("field"), str) and re.fullmatch(r"\w+", s["field"])):
                errs.append(w + ": text_le %s must be {select: <regex>, field: <name>}" % side)
                continue
            try:
                re.compile(s["select"])
            except re.error as x:
                errs.append(w + ": bad regex in %s.select: %s" % (side, x))
    for k in ("pattern", "select", "forbid"):
        pats = e.get(k)
        for p in (pats if isinstance(pats, list) else [pats] if pats else []):
            try:
                re.compile(p)
            except re.error as x:
                errs.append(w + ": bad regex in %s: %s" % (k, x))
    # the pooled meter's options (native/devcreate-close, Astra round 1: P1-4 / P2-5)
    if e.get("pool_weight", "frame") not in ("frame", "pair"):
        errs.append(w + ": pool_weight must be 'frame' or 'pair'")
    if "receipt" in e and not (isinstance(e["receipt"], str) and e["receipt"]):
        errs.append(w + ": receipt must be a path")
    prov = e.get("provenance")
    if prov is not None:
        if not isinstance(prov, dict) or not isinstance(prov.get("arm_key"), str) or "arm_on" not in prov:
            errs.append(w + ": provenance needs arm_key and arm_on")
        else:
            if prov.get("first", "base") not in ("base", "target"):
                errs.append(w + ": provenance first must be 'base' or 'target'")
            if "arm_unset" in prov and not isinstance(prov["arm_unset"], str):
                errs.append(w + ": provenance arm_unset must be the value an unset arm_key reads as (a string)")
            try:
                re.compile(prov.get("engaged") or "")
            except re.error as x:
                errs.append(w + ": bad regex in provenance engaged: %s" % x)
    if any(k in e for k in ("pool_weight", "receipt", "provenance")) and not (kind == "meter_delta" and e.get("pool")):
        errs.append(w + ": pool_weight / receipt / provenance belong to a pooled meter_delta (with `pool`)")
    req = e.get("require")
    if req is not None:  # a roll's precondition (require_problem)
        if not isinstance(req, dict) or not isinstance(req.get("select"), str) or not isinstance(req.get("pattern"), str):
            errs.append(w + ": require needs select and pattern (regexes on the roll's run.log)")
        else:
            for k in ("select", "pattern", "show"):
                try:
                    re.compile(req.get(k) or "")
                except re.error as x:
                    errs.append(w + ": bad regex in require %s: %s" % (k, x))
        if kind not in REQUIRE_KINDS:
            errs.append(w + ": require reads a roll's run.log (kinds %s)" % " / ".join(REQUIRE_KINDS))
    return errs


# ---------------------------------------------------------------------------------------------- evidence

class Ctx:
    def __init__(self, doc, repo):
        self.repo = repo
        self.roots = {}
        for k, v in doc.get("roots", {}).items():
            self.roots[k] = os.path.normpath(v if os.path.isabs(v) else os.path.join(repo, v))
        self._fg = None
        self._md = None
        self.memo = {}

    def cached(self, key, fn):
        """The same roll is read by several records (p2d_meter: the meter line, EDRAM, XMA, C1): read it once."""
        if key not in self.memo:
            self.memo[key] = fn()
        return self.memo[key]

    def resolve(self, e):
        root = e.get("root", "repo")
        base = self.roots[root]
        p = e["path"]
        return root, base, os.path.normpath(os.path.join(base, p))

    def missing(self, e, path):
        """A path that does not exist: 'unavailable' for local evidence (archive/, another worktree),
        a FAIL for a tracked repo file."""
        root, base, _ = self.resolve(e)
        rel = e["path"].replace("\\", "/")
        if root != "repo" or rel.startswith("archive/") or not os.path.isdir(base):
            return UNAVAIL, "evidence unavailable on this machine: %s" % disp(path)
        return FAIL, "missing: %s" % disp(path)

    def flicker_gate(self):
        if self._fg is None:
            sys.path.insert(0, os.path.join(self.repo, "tools", "perf"))
            import flicker_gate  # noqa: E402
            self._fg = flicker_gate
        return self._fg

    def meter_delta(self):
        if self._md is None:
            sys.path.insert(0, os.path.join(self.repo, "tools", "perf"))
            import meter_delta  # noqa: E402
            self._md = meter_delta
        return self._md


def disp(p):
    return p.replace("\\", "/")


def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def jfield(obj, dotted):
    cur = obj
    for part in dotted.split("."):
        if isinstance(cur, list):
            if not re.match(r"^-?\d+$", part):
                raise KeyError(dotted)
            cur = cur[int(part)]
        elif isinstance(cur, dict):
            if part not in cur:
                raise KeyError(dotted)
            cur = cur[part]
        else:
            raise KeyError(dotted)
    return cur


def jcheck(obj, c):
    op = c.get("op", "eq")
    try:
        v = jfield(obj, c["field"])
    except (KeyError, IndexError):
        return op == "missing", "%s absent" % c["field"]
    want = c.get("value")
    try:
        ok = {
            "eq": lambda: v == want, "ne": lambda: v != want,
            "ge": lambda: v >= want, "le": lambda: v <= want, "gt": lambda: v > want, "lt": lambda: v < want,
            "in": lambda: v in want, "contains": lambda: want in v, "not_contains": lambda: want not in v,
            "exists": lambda: True, "empty": lambda: not v, "nonempty": lambda: bool(v),
            "len_ge": lambda: len(v) >= want, "regex": lambda: re.search(want, str(v)) is not None,
        }[op]()
    except TypeError:
        ok = False
    return ok, "%s=%s %s %s" % (c["field"], json.dumps(v)[:80], op, json.dumps(want)[:60] if op not in ("exists", "empty", "nonempty") else "")


def world_floor():
    """tools/perf/world_floor.py - THE world rule, shared with the flicker gate (one source of truth)."""
    sys.path.insert(0, os.path.join(REPO, "tools", "perf"))
    import world_floor as wf  # noqa: E402
    return wf


def roll_world(ctx, d):
    """A plain roll's sparse dumped presents (SR_PRESENT_DUMP_SPARSE) from its in-game era start (the flicker
    gate's freeze-era start): how many show the drawn world -> (world, total, unmeasured). A meter taken on a run
    whose world went black (the startup race) or sat on a black pause menu is not a measurement of the drawn game
    (p3a_meter1: 0 of 46). A frame whose BMP is gone counts through the attempt's frames_measured.json; one with no
    measurement stays in the total as not drawn, and is counted in `unmeasured`."""
    wf, fg = world_floor(), ctx.flicker_gate()
    log = fg.run_log(d)
    if not log:
        return 0, 0, 0
    era = fg.era_start_ms(d, log)
    if era is None:
        return 0, 0, 0
    # one row per inventory present (never per log line: a repeated line cannot weight a frame - Astra round 5): a
    # logged dump at or after the era start, or one whose timing is unknown (no surviving log line names it, or it is
    # ambiguous - its in-game membership cannot be decided, so it counts, unmeasured: Astra 2026-10-14 round 4). An
    # archived roll whose sidecar is rejected grades every frame unmeasured: world 0 of its count.
    rows = [r for r in wf.inventory_rows(os.path.join(d, "drive", "attempt1", "frames"), log, era)
            if r[1] is None or wf.ts_ms(r[1]) >= era]
    return sum(1 for r in rows if r[3] in wf.WORLD), len(rows), sum(1 for r in rows if r[3] == "unmeasured")


ROLL_FILES = ("run.log", "provenance.json", "outcome.json", "steps.log")


def roll_files(d):
    """{file: sha256 or None} of a roll's drive/attempt1 record - the pooled meter's receipt (tools/perf/native_proof.py
    writes it with this function; the check below recomputes it): the rolls a ledger item cites are the rolls its run
    graded, never a later roll written to the same folder."""
    out = {}
    for name in ROLL_FILES:
        try:
            h = hashlib.sha256()
            with open(os.path.join(d, "drive", "attempt1", name), "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
            out[name] = h.hexdigest()
        except OSError:
            out[name] = None
    return out


def receipt_problem(ctx, e):
    """None, or (result, detail) when a pooled meter's receipt does not pass (native/devcreate-close, Astra round 1
    P1-4): the run-unique JSON its step wrote - committed, its runner verdict PASS, its pool the item's pairs in order,
    and every roll's files unchanged since the run. A pool whose rolls crashed or never ran grades FAIL here."""
    rit = dict(e, path=e["receipt"])
    _, _, rpath = ctx.resolve(rit)
    if not os.path.exists(rpath):
        return ctx.missing(rit, rpath)
    try:
        rec = json.load(open(rpath, encoding="utf-8"))
    except (OSError, ValueError) as x:
        return FAIL, "receipt %s unreadable (%s)" % (disp(e["receipt"]), x)
    if not isinstance(rec, dict):
        return FAIL, "receipt %s is not an object" % disp(e["receipt"])
    runner = rec.get("runner") if isinstance(rec.get("runner"), dict) else {}
    if rec.get("committed") is not True or runner.get("verdict") != "PASS":
        return FAIL, "receipt %s: the run's pool step %s (%s)%s" % (
            disp(e["receipt"]), runner.get("verdict") or "did not finish",
            str(runner.get("why") or "")[:200], "" if rec.get("committed") is True else ", uncommitted")
    want = [[p["base"], p["path"]] for p in [e] + list(e["pool"])]
    if rec.get("pool") != want:
        return FAIL, "receipt %s is another pool's (%s, want %s)" % (disp(e["receipt"]), rec.get("pool"), want)
    rolls = rec.get("rolls") if isinstance(rec.get("rolls"), dict) else {}
    for rel in [x for pr in want for x in pr]:
        got = rolls.get(rel)
        if not isinstance(got, dict) or got.get("row") != "DONE":
            return FAIL, "receipt %s: roll %s's run row %s" % (disp(e["receipt"]), disp(rel), (got or {}).get("row"))
        _, _, rp = ctx.resolve(dict(e, path=rel))
        now = roll_files(rp.rstrip("\\/"))
        if now != got.get("files"):
            changed = sorted(k for k in ROLL_FILES if now.get(k) != (got.get("files") or {}).get(k))
            return FAIL, "roll %s changed since its run (%s): not the roll the pool graded" % (disp(rel), ", ".join(changed))
    return None


def pool_provenance(ctx, e, pairs):
    """[problems] of a pooled meter's provenance (native/devcreate-close, Astra round 1 P2-5): one exe hash for every
    roll; each target roll's effective SR_* env holds arm_key=arm_on and each base roll's does not; the engaged regex in
    every target log and in no base log; the env equal beyond arm_key and the `ignore` keys; and with `alternate` the
    declared run order - pair 1 `first` (base / target), the order flipping every pair, the pairs in sequence."""
    prov = e["provenance"]
    key, on = prov["arm_key"], str(prov["arm_on"])
    # a baked gate (native/devcreate-fix, 2026-10-15): `arm_unset` is what the key reads as when a roll's env does not
    # set it (the default build is the target arm, "=0" the base); without it an unset key is never the target's
    unset = prov.get("arm_unset")
    ign = set(prov.get("ignore") or []) | {key}
    eng = re.compile(prov["engaged"]) if prov.get("engaged") else None
    bads, exes, envs, order = [], set(), set(), []
    for k, (pb, pa) in enumerate(pairs):
        st = {}
        for side, rel in (("target", pb), ("base", pa)):
            _, _, pp = ctx.resolve(dict(e, path=rel))
            att = os.path.join(pp, "drive", "attempt1")
            try:
                pv = json.load(open(os.path.join(att, "provenance.json"), encoding="utf-8-sig"))
            except (OSError, ValueError):
                bads.append("%s has no readable provenance.json" % disp(rel))
                continue
            env = pv.get("effectiveSrEnv") if isinstance(pv.get("effectiveSrEnv"), dict) else {}
            exes.add(str(pv.get("exeSha256") or "").lower())
            envs.add(json.dumps({a: b for a, b in env.items() if a not in ign}, sort_keys=True))
            if (env.get(key, unset) == on) != (side == "target"):
                bads.append("%s (%s) has %s=%s" % (disp(rel), side, key, env.get(key)))
            try:
                txt = read_text(os.path.join(att, "run.log"))
            except OSError:
                bads.append("%s has no run.log" % disp(rel))
                continue
            m = re.search(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]", txt, re.M)
            st[side] = m.group(1) if m else None
            if eng is not None and (eng.search(txt) is not None) != (side == "target"):
                bads.append("%s (%s): the engagement line /%s/ %s" % (
                    disp(rel), side, prov["engaged"][:40], "absent" if side == "target" else "present"))
        if st.get("target") and st.get("base"):
            order.append(("base" if st["base"] < st["target"] else "target", min(st.values()), max(st.values())))
        else:
            order.append(None)
    if len(exes) != 1 or "" in exes:
        bads.append("exe hashes %s (want exactly one)" % sorted(x[:12] for x in exes))
    if len(envs) > 1:
        bads.append("the effective SR_* env differs beyond %s and the ignored keys" % key)
    if prov.get("alternate"):
        if None in order:
            bads.append("the run order is unreadable (a log without a timestamped line)")
        else:
            first = prov.get("first", "base")
            for k, (who, lo, hi) in enumerate(order):
                want = first if k % 2 == 0 else ("target" if first == "base" else "base")
                if who != want:
                    bads.append("pair %d ran %s first (declared %s first)" % (k + 1, who, want))
                if k and not order[k - 1][2] < lo:
                    bads.append("pair %d's first roll started before pair %d's second" % (k + 1, k))
    return bads


def check_item(ctx, e):
    """-> (result, detail). Never raises: anything unexpected is a FAIL with its reason.
    `"expect": "fail"` (witnesses of a contradiction: "this roll's freezes are black") inverts a READ
    result - the item passes when its check fails for a stated reason; an unreadable or unavailable
    item, or one whose roll did not meet its precondition (`require`: NO DATA), never passes either way.
    `"expect": "no_data"` (p2.zpd zpdx3: a witness kept for the record whose roll had nothing to grade - the zpd6
    gate's freeze roll issued no query) passes only on exactly that: its own precondition unmet (NO DATA); a roll
    that met it, and anything unreadable or missing, is a FAIL, unavailable stays unavailable."""
    key = json.dumps({k: v for k, v in e.items() if k not in ("id", "note", "expect", "authority", "observes")},
                     sort_keys=True)
    try:
        res, det = ctx.cached(("item", key), lambda: _check_required(ctx, e))
    except Exception as x:  # fail-closed
        return FAIL, "unreadable (%s: %s)" % (type(x).__name__, x)
    if e.get("expect") == "no_data" and res != UNAVAIL:
        if res == FAIL and det.startswith(NO_DATA) and e.get("require"):
            return PASS, "no data, as expected: " + det
        return FAIL, "expected NO DATA, but: " + det
    if e.get("expect", "pass") == "fail" and res != UNAVAIL and not det.startswith(("unreadable", "missing:", NO_DATA)):
        if res == PASS:
            return FAIL, "expected to FAIL, but passes: " + det
        return PASS, "fails as expected: " + det
    return res, det


def require_problem(ctx, path, req):
    """(problem, shown) of an item's `require` {select, pattern, show?, name?, why?} - its roll's precondition (p2.zpd,
    zpd6: tools/perf/native_proof.py ZPD_FIRED, the game issued an occlusion query at all): the LAST line of the roll's
    run.log (`path` itself, or <path>/drive/attempt1/run.log for a roll directory) matching `select` must match
    `pattern`. Unmet = problem (FAIL, 'NO DATA: <why>: <shown>'): the roll has nothing to grade, so the item never
    passes, a witness of it (expect fail) never 'fails as expected', and a resolver of it keeps its unknown open."""
    log = path if os.path.isfile(path) else os.path.join(path, "drive", "attempt1", "run.log")
    why = req.get("why") or "precondition %s unmet" % req.get("name", "")
    if not os.path.isfile(log):
        return (FAIL, "%s: %s (no run.log: %s)" % (NO_DATA, why, disp(log))), ""
    last = ctx.cached(("require", log, req["select"]), lambda: next(
        (ln for ln in reversed(read_text(log).splitlines()) if re.search(req["select"], ln)), None))
    if last is None:
        return (FAIL, "%s: %s (no line /%s/)" % (NO_DATA, why, req["select"][:60])), ""
    m = re.search(req["show"], last) if req.get("show") else None
    shown = m.group(0) if m else last.strip()[:140]
    if re.search(req["pattern"], last) is None:
        return (FAIL, "%s: %s: %s" % (NO_DATA, why, shown)), shown
    return None, shown


REQUIRE_KINDS = ("text", "freeze_grade", "flicker_gate", "meter_delta")


def require_rolls(kind, path, others=()):
    """The rolls an item's `require` reads: its own (a run.log, or a roll directory), or - a flicker_gate item (Astra
    merge review P2, the gate bypassed the no-query rule) - BOTH of the gate's rolls, freeze and span; a meter_delta
    item (p2.zpd zbk2, 2026-10-16: the meter graded PASS over a NO DATA =0 roll) - its target AND the rolls it compares
    with (`others`: [(label, resolved path)] - the base, a pool's pairs)."""
    if kind == "meter_delta":
        return [("", path)] + list(others)
    if kind != "flicker_gate":
        return [("", path)]
    gdir = path if os.path.isdir(path) else os.path.dirname(path)
    return [(sub + " ", os.path.join(gdir, sub)) for sub in ("freeze", "span")]


def _check_required(ctx, e):
    """_check_item behind the item's `require` (require_problem): an unmet precondition is NO DATA whatever the item's
    own check would say; a met one is shown after the item's detail. Missing / unavailable paths grade as always."""
    req = e.get("require")
    if not req:
        return _check_item(ctx, e)
    if e["kind"] not in REQUIRE_KINDS:  # fail-closed (item_errors names it too)
        return FAIL, "%s: require on kind %s (no roll log to read)" % (NO_DATA, e["kind"])
    _root, base, path = ctx.resolve(e)
    if not os.path.isdir(base) or not os.path.exists(path):
        return _check_item(ctx, e)
    others = []
    if e["kind"] == "meter_delta":  # the rolls the meter compares the target with: the base, and a pool's pairs
        pairs = [("base ", e.get("base"))] + [("pool %d " % (i + 1), x.get(k)) for i, x in enumerate(e.get("pool") or [])
                                              for k in ("path", "base")]
        others = [(label, ctx.resolve(dict(e, path=p))[2]) for label, p in pairs if p]
    whose = "the meter's" if e["kind"] == "meter_delta" else "the gate's"
    shown = []
    for label, roll in require_rolls(e["kind"], path, others):
        bad, s = require_problem(ctx, roll, req)
        if bad:
            return bad[0], bad[1] + ((" (%s %sroll)" % (whose, label)) if label else "")
        shown.append(label + s)
    res, det = _check_item(ctx, e)
    return res, "%s | precondition %s met: %s" % (det, req.get("name", "require"), ", ".join(shown))


def _check_item(ctx, e):
    kind = e["kind"]
    if kind == "git_ancestor":
        c = e["commit"]
        r = subprocess.run(["git", "-C", ctx.repo, "cat-file", "-e", c + "^{commit}"], capture_output=True)
        if r.returncode:
            return FAIL, "commit %s not in this repository" % c
        r = subprocess.run(["git", "-C", ctx.repo, "merge-base", "--is-ancestor", c, e.get("ref", "HEAD")],
                           capture_output=True)
        return (PASS, "%s is on %s" % (c, e.get("ref", "HEAD"))) if r.returncode == 0 else \
            (FAIL, "%s is NOT an ancestor of %s" % (c, e.get("ref", "HEAD")))

    root, base, path = ctx.resolve(e)
    if not os.path.isdir(base):
        return UNAVAIL, "evidence unavailable on this machine: root %s (%s)" % (root, disp(base))
    if kind == "meter_delta" and e.get("pool") and e.get("receipt"):
        bad = receipt_problem(ctx, e)  # BEFORE the rolls: a pool whose rolls never ran grades FAIL, not unavailable
        if bad:
            return bad
    if not os.path.exists(path):
        return ctx.missing(e, path)

    if kind == "file":
        return PASS, "exists: %s" % disp(e["path"])

    if kind == "source":
        txt = read_text(path)
        m = re.search(e["pattern"], txt, re.M)
        what = e.get("note") or "/%s/" % e["pattern"][:60]
        return (PASS, "%s: %s" % (disp(e["path"]), what)) if m else (FAIL, "%s does NOT say: %s" % (disp(e["path"]), what))

    if kind == "text":
        txt = read_text(path)
        lines = txt.splitlines()
        for fp in (e.get("forbid") or []):
            for ln in lines:
                if re.search(fp, ln):
                    return FAIL, "forbidden /%s/ present: %s" % (fp, ln.strip()[:140])
        sel = [ln for ln in lines if re.search(e["select"], ln)] if e.get("select") else lines
        if e.get("select") and not sel:
            return FAIL, "no line matches select /%s/" % e["select"][:60]
        pat = e.get("pattern")
        if not pat:
            return PASS, "%d selected line(s)" % len(sel)
        which = e.get("which", "any")
        if which == "last":
            ok = re.search(pat, sel[-1]) is not None
            return (PASS if ok else FAIL), ("last selected line %s /%s/: %s" % ("matches" if ok else "does NOT match",
                                                                              pat[:60], sel[-1].strip()[:160]))
        hits = [ln for ln in sel if re.search(pat, ln)]
        if which == "all":
            ok = len(hits) == len(sel)
            return (PASS if ok else FAIL), "%d of %d selected lines match /%s/" % (len(hits), len(sel), pat[:60])
        need = int(e.get("min_count", 1))
        if len(hits) >= need:
            return PASS, "%d match(es) /%s/: %s" % (len(hits), pat[:50], hits[-1].strip()[:140])
        return FAIL, "%d match(es) of /%s/ (need %d)" % (len(hits), pat[:60], need)

    if kind == "text_le":
        # Two counters of ONE run's log (native/dreg-bake: the run-cumulative dispreg <= the run's SWAP declines). lhs:
        # the LAST line its select matches; rhs: the FIRST line its select matches AFTER that one - both counters only
        # grow, so the rhs counts at least the lhs's interval. A field is the first `<field>=<integer>` on its line (a
        # decimal like dispreg=0.0, the per-frame form, never matches).
        lines = read_text(path).splitlines()
        for fp in (e.get("forbid") or []):
            for ln in lines:
                if re.search(fp, ln):
                    return FAIL, "forbidden /%s/ present: %s" % (fp, ln.strip()[:140])
        lhs, rhs = e["lhs"], e["rhs"]

        def counter(ln, field):
            m = re.search(r"(?<![\w.])%s=(\d+)(?![\d.])" % re.escape(field), ln)
            return int(m.group(1)) if m else None
        li = [i for i, ln in enumerate(lines) if re.search(lhs["select"], ln)]
        if not li:
            return FAIL, "no line matches lhs select /%s/" % lhs["select"][:60]
        lv = counter(lines[li[-1]], lhs["field"])
        if lv is None:
            return FAIL, "the last lhs line has no %s=<integer>: %s" % (lhs["field"], lines[li[-1]].strip()[:140])
        ri = [i for i in range(li[-1] + 1, len(lines)) if re.search(rhs["select"], lines[i])]
        if not ri:
            return FAIL, "no rhs line /%s/ after the last lhs line (not comparable)" % rhs["select"][:60]
        rv = counter(lines[ri[0]], rhs["field"])
        if rv is None:
            return FAIL, "the rhs line has no %s=<integer>: %s" % (rhs["field"], lines[ri[0]].strip()[:140])
        ok = lv <= rv
        return (PASS if ok else FAIL), "%s=%d %s %s=%d (lines %d / %d)" % (
            lhs["field"], lv, "<=" if ok else "NOT <=", rhs["field"], rv, li[-1] + 1, ri[0] + 1)

    if kind == "json":
        obj = json.load(open(path, encoding="utf-8"))
        notes = []
        for c in e["checks"]:
            ok, why = jcheck(obj, c)
            if not ok:
                return FAIL, "json check failed: " + why
            notes.append(why)
        return PASS, "; ".join(notes)[:200]

    if kind == "flicker_gate":
        # The gate's OWN grader (tools/perf/flicker_gate.grade, read-only on the stored diff outputs): the span's
        # A-B-A count, scene floor and in-play share, the freeze's pixels and the world rule - never the verdict a
        # gate JSON recorded when it ran (older gates predate the world rule).
        fg = ctx.flicker_gate()
        gdir = path if os.path.isdir(path) else os.path.dirname(path)
        if not os.path.isdir(os.path.join(gdir, "span")) and not os.path.isdir(os.path.join(gdir, "freeze")):
            return ctx.missing(e, os.path.join(gdir, "span"))
        g = fg.grade(gdir, span_max=int(e.get("span_max", 5)), min_world=int(e.get("min_lit", 8)), readonly=True,
                     max_menu=int(e.get("max_menu", fg.wf.MAX_MENU_FREEZES)))
        span, fz = g.get("span") or {}, g.get("freeze") or {}
        min_counted = int(e.get("min_counted", 10))
        probs = []
        if span.get("verdict") != "PASS":
            probs.append("span %s" % span.get("verdict"))
        if fz.get("verdict") != "PASS":
            probs.append("freeze %s%s" % (fz.get("verdict"), (" (%s)" % fz["world_fail"]) if fz.get("world_fail") else ""))
        if int(fz.get("counted") or 0) < min_counted:
            probs.append("freeze counted=%s < %d" % (fz.get("counted"), min_counted))
        detail = "span %s | freeze counted=%s bad=%s %s" % (
            span.get("line") or "-", fz.get("counted"), fz.get("bad"), fz.get("world_line", "-"))
        return (FAIL, "; ".join(probs) + " | " + detail) if probs else (PASS, detail)

    if kind == "freeze_grade":
        fg = ctx.flicker_gate()
        d = path.rstrip("\/")
        log = fg.run_log(d)
        if not log:
            return FAIL, "no run.log under %s" % disp(d)
        pre = e.get("table") == "pre-control"  # the roll graded by the table the earlier rule produced (a witness)
        tab = d + ("_freeze.pre-control.txt" if pre else "_freeze.txt")
        if not os.path.exists(tab):
            # The roll is here (its run.log was found) but graded nothing: an aborted / void roll is a failing
            # item, not evidence that lives on another machine (rows53b's r5b_stress, 2026-10-10).
            return FAIL, "no freeze table: the roll ran here but graded nothing (aborted / void): %s" % disp(tab)
        # the gate's own freeze grade: pixels (grade_freeze_table) + the world rule (grade_freeze_world)
        if pre:
            r = {"dir": d, "verdict": "NO DATA"}
            fg.grade_freeze_table(d, log, read_text(tab), int(e.get("allow_pixels", 64)), int(e.get("allow_spike", 8)), r, quiet=True)
            r = fg.grade_freeze_world(d, log, r, int(e.get("min_lit", 8)), int(e.get("max_menu", fg.wf.MAX_MENU_FREEZES)))
        else:
            r = fg.grade_freeze(d, 1, int(e.get("allow_pixels", 64)), int(e.get("allow_spike", 8)),
                                min_world=int(e.get("min_lit", 8)), readonly=True,
                                max_menu=int(e.get("max_menu", fg.wf.MAX_MENU_FREEZES)))
        min_counted = int(e.get("min_counted", 10))
        probs = []
        if r.get("verdict") != "PASS":
            probs.append("grade %s bad=%s%s" % (r.get("verdict"), r.get("bad"),
                                                (" (%s)" % r["world_fail"]) if r.get("world_fail") else ""))
        if int(r.get("counted") or 0) < min_counted:
            probs.append("counted=%s < %d" % (r.get("counted"), min_counted))
        ab_need = int(e.get("ab_present_min", 0))
        abn = 0
        if ab_need:
            abn = sum(1 for ln in read_text(log).splitlines() if "[AB_PRESENT]" in ln)
            if abn < ab_need:
                probs.append("[AB_PRESENT] lines %d < %d (the toggle is not witnessed)" % (abn, ab_need))
            # the per-present A/B grades on the same-transport control (flicker_gate.grade_freeze_table, 2026-10-10):
            # a stored table without the control columns was graded by the earlier rule - regenerate it
            # (tools/perf/freeze_diff.py --exact --allow-maxdiff 1 --ab-control) before it can pass. A witness that
            # names the pre-control table ("table": "pre-control") is that earlier grading on purpose.
            if not r.get("ab_control") and e.get("table") != "pre-control":
                probs.append("the stored freeze table has no A/B control columns (regenerate with --ab-control)")
        for fp in (e.get("forbid") or []):
            for ln in read_text(log).splitlines():
                if re.search(fp, ln):
                    probs.append("forbidden /%s/: %s" % (fp, ln.strip()[:120]))
                    break
        detail = "freeze grade %s counted=%s bad=%s | %s%s%s" % (
            r.get("verdict"), r.get("counted"), r.get("bad"), r.get("world_line", "-"),
            (" | AB_PRESENT %d" % abn) if ab_need else "",
            (" | " + r["control_line"]) if r.get("control_line") else "")
        return (FAIL, "; ".join(probs) + " | " + detail) if probs else (PASS, detail)

    if kind == "replay_cmp":
        txt = read_text(path)
        want = int(e.get("traces", 9))
        m = re.findall(r"(\d+)/(\d+) byte-identical", txt)
        rows = [ln for ln in txt.splitlines() if re.match(r"^\s*\w+\s+(IDENTICAL|DIFF|DIFFERS|MISSING|FAIL)", ln)]
        bad = [ln.strip() for ln in rows if "IDENTICAL" not in ln]
        if not m:
            return FAIL, "no 'N/M byte-identical' summary"
        n, tot = int(m[-1][0]), int(m[-1][1])
        if bad or n != tot or tot < want:
            return FAIL, "replay %d/%d (need %d/%d)%s" % (n, tot, want, want, (" first diff: " + bad[0][:80]) if bad else "")
        return PASS, "replay %d/%d byte-identical" % (n, tot)

    if kind == "check_report":
        rep = json.load(open(path, encoding="utf-8"))
        res = rep.get("results") or []
        if not res:
            return FAIL, "check_report has no results"
        allow_ne = [re.compile(p) for p in (e.get("allow_no_evidence") or [])]
        want = [re.compile(p) for p in (e.get("entries") or [])]
        pool = res
        if want:
            pool = []
            for w in want:
                hit = [r for r in res if w.search(r.get("name", ""))]
                if not hit:
                    return FAIL, "no check entry matches /%s/" % w.pattern
                pool += hit
        bad = []
        for r in pool:
            v = r.get("verdict")
            if v == "PASS":
                continue
            if v == "NO-EVIDENCE" and any(a.search(r.get("name", "")) for a in allow_ne):
                continue
            bad.append("%s: %s" % (r.get("name", "?")[:60], v))
        if bad:
            return FAIL, "%d entr%s not PASS: %s" % (len(bad), "y" if len(bad) == 1 else "ies", "; ".join(bad[:3]))
        return PASS, "%d entr%s PASS" % (len(pool), "y" if len(pool) == 1 else "ies")

    if kind in ("meter", "meter_delta"):
        md = ctx.meter_delta()
        d = path.rstrip("\\/")

        def per_frame(dd):
            start, rows = ctx.cached(("windows", os.path.normcase(dd)), lambda: md.windows(dd))
            if start is None and e.get("era", "ingame") == "ingame":
                raise ValueError("no ingame marker in %s (meter_delta would fall back to the whole run)" % disp(dd))
            frames, per = md.summarize(rows)
            if not frames:
                raise ValueError("no [NATIVE_METER] windows in %s" % disp(dd))
            return frames, per

        def world_note(dd):
            """'' or a FAIL reason: the run behind a meter number must show the drawn world."""
            if "world_min" not in e:
                return "", ""
            w, n, un = ctx.cached(("world", os.path.normcase(dd)), lambda: roll_world(ctx, dd))
            frac = (w / n) if n else 0.0
            note = " | world drawn in %d of %d ingame sparse frames (%s%s)" % (
                w, n, os.path.basename(dd), ("; %d unmeasured" % un) if un else "")
            return (note, "" if (n and frac >= float(e["world_min"])) else
                    "the world was drawn in only %d of %d ingame frames of %s (need %.0f%%)" % (
                        w, n, disp(os.path.basename(dd)), 100 * float(e["world_min"])))

        f = e["field"]
        if kind == "meter_delta" and e.get("pool"):
            # A frame-weighted pool of several pairs on one exe (the per-frame meter's run-to-run scene spread):
            # each arm's windows summed over every pair - none dropped - and every roll held to the world rule.
            # min_roll_frames (native/devcreate-close, 2026-10-14): every roll's ingame window holds at least that many
            # frames - a roll cut short covers less of the scene than the others and would weigh in as a pair it is not.
            # pool_weight "pair" (Astra round 1, P2-5): the statistic is the mean of the per-pair deltas - the one a
            # pair-to-pair SD powers - never the frame-weighted arm means, which turn zero-delta pairs with opposite
            # frame counts into a delta; `provenance` - one exe, engaged arms, the declared alternation.
            pairs = [(e["path"], e["base"])] + [(x["path"], x["base"]) for x in e["pool"]]
            arm, bads, each, diffs = {"b": [], "a": []}, [], [], []
            min_rf = int(e.get("min_roll_frames", 0))
            by_pair = e.get("pool_weight", "frame") == "pair"
            if e.get("provenance"):
                for pb, pa in pairs:  # every roll there first (a missing one is unavailable, as below)
                    for rel in (pb, pa):
                        it = dict(e, path=rel)
                        _, _, pp = ctx.resolve(it)
                        if not os.path.exists(pp):
                            return ctx.missing(it, pp)
                pbad = pool_provenance(ctx, e, pairs)
                if pbad:
                    return FAIL, "pool provenance: " + "; ".join(pbad)
            for pb, pa in pairs:
                got = {}
                for side, rel in (("b", pb), ("a", pa)):
                    it = dict(e, path=rel)
                    _, _, pp = ctx.resolve(it)
                    if not os.path.exists(pp):
                        return ctx.missing(it, pp)
                    pp = pp.rstrip("\\/")
                    start, rows = ctx.cached(("windows", os.path.normcase(pp)), lambda pp=pp: md.windows(pp))
                    if start is None and e.get("era", "ingame") == "ingame":
                        raise ValueError("no ingame marker in %s (meter_delta would fall back to the whole run)" %
                                         disp(pp))
                    frames1, per1 = md.summarize(rows)
                    if not frames1:  # every arm measured: no window, or no field, is a FAIL, never a zero
                        return FAIL, "no [NATIVE_METER] windows in %s" % disp(pp)
                    if f not in per1:
                        return FAIL, "meter field %s absent in %s" % (f, disp(pp))
                    if frames1 < min_rf:
                        bads.append("%s holds %d ingame frames (min_roll_frames %d)" % (disp(rel), frames1, min_rf))
                    arm[side] += rows
                    got[side] = per1[f]
                    bad1 = world_note(pp)[1]
                    if bad1:
                        bads.append(bad1)
                each.append("%+.1f" % (got["b"] - got["a"]))
                diffs.append(got["b"] - got["a"])
                # the zero form on a pool (native/devcreate-fix, the bake): every target roll holds none of the field
                # (max) and every base roll shows it (base_min) - a pair whose base measured nothing is no pair
                if "max" in e and not got["b"] <= float(e["max"]):
                    bads.append("%s: %s %.2f per ingame frame > max %s (the target arm must hold none)" % (
                        disp(pb), f, got["b"], e["max"]))
                if "base_min" in e and not got["a"] >= float(e["base_min"]):
                    bads.append("%s: %s %.2f per ingame frame < base_min %s (the base arm must show the producer)" % (
                        disp(pa), f, got["a"], e["base_min"]))
            (fb, pb_), (fa, pa_) = md.summarize(arm["b"]), md.summarize(arm["a"])
            fw = pb_[f] - pa_[f]
            delta = sum(diffs) / len(diffs) if by_pair else fw
            ok = not bads and delta <= float(e["max_delta"])
            det = "%s pooled over %d pairs (%d / %d frames) %.2f -> %.2f (%s %+.2f, need delta <= %s; each pair %s)%s%s%s" % (
                f, len(pairs), fa, fb, pa_.get(f, 0.0), pb_.get(f, 0.0),
                ("delta = the mean of the per-pair deltas %+.2f; frame-weighted, information only:" % delta)
                if by_pair else "delta", fw, e["max_delta"], ", ".join(each),
                ("; every roll >= %d ingame frames" % min_rf) if min_rf else "",
                ("; every target roll <= %s" % e["max"]) if "max" in e else "",
                ("; every base roll >= %s" % e["base_min"]) if "base_min" in e else "")
            if e.get("provenance"):
                det += "; provenance: one exe, %s engaged ON only, %s" % (
                    e["provenance"]["arm_key"], "the declared alternation" if e["provenance"].get("alternate") else
                    "order not checked")
            return (PASS, det) if ok else (FAIL, "; ".join(bads) + (" | " if bads else "") + det)
        frames, per = per_frame(d)
        if f not in per:
            return FAIL, "meter field %s absent" % f
        val = per[f]
        note, bad = world_note(d)
        if kind == "meter_delta":
            broot = dict(e, path=e["base"])
            _, _, bpath = ctx.resolve(broot)
            if not os.path.exists(bpath):
                return ctx.missing(broot, bpath)
            _, bper = per_frame(bpath.rstrip("\\/"))
            bnote, bbad = world_note(bpath.rstrip("\\/"))
            delta = val - bper.get(f, 0.0)
            # The bounds (2026-10-11): max_delta on the change; or, for a converted producer measured by its own
            # packet kind, base_min (the base census shows it) and max (the target holds none of it).
            need = []
            ok = not bad and not bbad
            if "max_delta" in e:
                ok = ok and delta <= float(e["max_delta"])
                need.append("delta <= %s" % e["max_delta"])
            if "base_min" in e:
                ok = ok and bper.get(f, 0.0) >= float(e["base_min"])
                need.append("base >= %s" % e["base_min"])
            if "max" in e:
                ok = ok and val <= float(e["max"])
                need.append("target <= %s" % e["max"])
            det = "%s per ingame frame %.2f -> %.2f (delta %+.2f, need %s)%s%s" % (
                f, bper.get(f, 0.0), val, delta, ", ".join(need) or "nothing", bnote, note)
            return (PASS, det) if ok else (FAIL, "; ".join(x for x in (bbad, bad) if x) + (" | " if (bad or bbad) else "") + det)
        ok = not bad
        if "max" in e and val > float(e["max"]):
            ok = False
        if "min" in e and val < float(e["min"]):
            ok = False
        det = "%s per ingame frame %.1f over %d frames (bounds %s..%s)%s" % (
            f, val, frames, e.get("min", "-"), e.get("max", "-"), note)
        return (PASS, det) if ok else (FAIL, (bad + " | " if bad else "") + det)

    return FAIL, "unhandled kind %s" % kind


# ---------------------------------------------------------------------------------------------- closure

def rank(a):
    return AUTHORITY.index(a)


def evaluate(doc, ctx, only=None):
    obs = {o["id"]: o for o in doc["obligations"]}
    results = {}

    def ev_list(items):
        out = []
        for e in items or []:
            res, det = check_item(ctx, e)
            out.append({"id": e["id"], "kind": e["kind"], "authority": e["authority"], "observes": e["observes"],
                        "result": res, "detail": det})
        return out

    def solve(oid, stack=()):
        if oid in results:
            return results[oid]
        o = obs[oid]
        items = ev_list(o.get("evidence"))
        why = []
        failing = [i for i in items if i["result"] == FAIL]
        unavail = [i for i in items if i["result"] == UNAVAIL]
        passing = [i for i in items if i["result"] == PASS]
        if not items:
            why.append("no evidence")
        for i in failing:
            why.append("FAIL %s: %s" % (i["id"], i["detail"]))
        for i in unavail:
            why.append("UNAVAILABLE %s" % i["id"])
        guards = []
        for g in o["guards"]:
            best = [i for i in passing if i["observes"] == g["observes"] and rank(i["authority"]) >= rank(g["min_authority"])]
            guards.append({"observes": g["observes"], "min_authority": g["min_authority"], "met_by": [i["id"] for i in best]})
            if not best:
                have = sorted({i["authority"] for i in passing if i["observes"] == g["observes"]}, key=rank)
                why.append("guard %s >= %s unmet%s" % (g["observes"], g["min_authority"],
                                                      (" (best passing: %s)" % have[-1]) if have else " (no passing item observes it)"))
        opened = {}
        for key, label in (("residual_unknowns", "unknown"), ("contradictions", "contradiction")):
            lst = []
            for u in o.get(key, []) or []:
                rb = ev_list(u.get("resolved_by"))
                wit = ev_list(u.get("witness"))
                resolved = bool(rb) and all(r["result"] == PASS for r in rb)
                entry = {"id": u["id"], "text": u["text"], "resolved": resolved, "resolved_by": rb, "witness": wit}
                if wit and not all(w["result"] == PASS for w in wit):
                    entry["note"] = "witness does not reproduce (or is unavailable) - still open until resolved_by passes"
                lst.append(entry)
                if not resolved:
                    why.append("%s %s open: %s" % (label, u["id"], u["text"][:110]))
            opened[key] = lst
        deps = []
        for d in o.get("depends_on", []) or []:
            ds = solve(d, stack + (oid,))["status"]
            deps.append({"id": d, "status": ds})
            if ds != "closed":
                why.append("depends on %s (%s)" % (d, ds))
        deps_ok = all(d["status"] == "closed" for d in deps)
        closed = bool(items) and not failing and not unavail and all(g["met_by"] for g in guards) and deps_ok \
            and not any(not u["resolved"] for lst in opened.values() for u in lst)
        if closed:
            status = "closed"
        elif not deps_ok:
            status = "blocked"
        elif passing:
            status = "in-progress"
        else:
            status = "open"
        claim = o.get("claimed", "none")
        stale = []
        for r in o.get("claim_refs", []) or []:
            p = os.path.join(ctx.repo, r["doc"])
            ok = False
            if os.path.exists(p):
                norm = lambda s: re.sub(r"\s+", " ", s)
                ok = norm(r["quote"]) in norm(read_text(p))
            if not ok:
                stale.append("%s: %r" % (r["doc"], r["quote"][:60]))
        res = {"id": oid, "version": o["version"], "title": o["title"], "public": o.get("public"),
               "status": status, "claimed": claim,
               "claim_mismatch": claim in DONE_CLAIMS and status != "closed", "stale_claim_refs": stale,
               "why": (sorted(why, key=lambda w: not w.startswith("depends on")) if status == "blocked" else why)
               if not closed else ["all evidence passes; guards met; no open unknown / contradiction"],
               "evidence": items, "guards": guards, "dependencies": deps,
               "residual_unknowns": opened["residual_unknowns"], "contradictions": opened["contradictions"]}
        results[oid] = res
        return res

    for oid in obs:
        if only and oid not in only:
            continue
        solve(oid)
    return [results[o["id"]] for o in doc["obligations"] if o["id"] in results]


def digest(results):
    canon = json.dumps([{k: r[k] for k in ("id", "version", "status", "evidence", "guards", "dependencies",
                                           "residual_unknowns", "contradictions")} for r in results],
                       sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canon.encode("utf-8")).hexdigest()[:16]


# ---------------------------------------------------------------------------------------------- status blocks

BLOCK = re.compile(r"(<!-- native-ledger:begin:(\w+) -->)(.*?)(<!-- native-ledger:end -->)", re.S)
STATUS_WORD = {"closed": "closed", "in-progress": "in progress", "blocked": "blocked", "open": "open"}


def meter_eval(doc, ctx):
    """The ledger's `meter` section: [(label, value or None, detail)], the unmetered names, the branch lines."""
    m = doc.get("meter") or {}

    def one(e):
        res, det = check_item(ctx, e)
        mm = re.search(r"per ingame frame ([\d.]+)", det)
        return (e["id"], ("{:,.1f}".format(float(mm.group(1))) if (mm and res == PASS) else None), res, det)
    items = [one(e) for e in m.get("items", [])]
    branch = [(b["label"], one(b["item"])) for b in m.get("branch", [])]
    return m, items, branch


def counts_of(results):
    c = {k: 0 for k in STATUS_ORDER}
    for r in results:
        c[r["status"]] += 1
    return c


def cell(s, n=170):
    s = s.replace("|", "\\|").replace("\n", " ")
    return s if len(s) <= n else s[: n - 3] + "..."


def render_block(style, doc, results, ctx):
    """The generated status text for a doc block (`roadmap` or `readme`). Deterministic for the same evidence."""
    c = counts_of(results)
    dg = digest(results)
    unproven = [r for r in results if r["claimed"] == "shipping-unproven" and r["status"] != "closed"]
    m, items, branch = meter_eval(doc, ctx)
    vals = {i: v for i, v, _, _ in items}
    lines = []
    if style == "readme":
        lines.append("Computed by `tools/native_ledger.py` from the project's native obligation ledger "
                     "(`docs/native_ledger.json`): every status below is derived from archived test evidence, never "
                     "written by hand (digest `%s`)." % dg)
        lines.append("")
        lines.append("- **%d native obligations:** %s." % (len(results), ", ".join(
            "%d %s" % (c[k], STATUS_WORD[k]) for k in STATUS_ORDER)))
        if unproven:
            lines.append("- **Shipping before their proof is complete:** %d (%s)." % (
                len(unproven), ", ".join(r.get("public") or r["title"] for r in unproven)))
        lines.append("- **Still emulated per gameplay frame** (%s): %s PM4 packets interpreted (the goal is 0); "
                     "EDRAM-model operations %s / %s / %s (resolves / transfers / loads); XMA register accesses %s; "
                     "display-register answers %s; shared-memory page validations %s. Unmetered: %s." % (
                         m.get("public_label") or m.get("label", "the native meter"), vals.get("pm4") or "unmeasured",
                         vals.get("edram_res") or "-", vals.get("edram_xfer") or "-", vals.get("edram_load") or "-",
                         vals.get("xma") or "-", vals.get("dispreg") or "unmeasured", vals.get("shm") or "unmeasured",
                         " and ".join(m.get("unmetered", [])) or "none"))
        lines.append("")
        lines.append("| Item | Status |")
        lines.append("|---|---|")
        for r in results:
            st = STATUS_WORD[r["status"]]
            if r["claimed"] == "shipping-unproven" and r["status"] != "closed":
                st += " (ships; proof incomplete)"
            lines.append("| %s | %s |" % (cell(r.get("public") or r["title"]), st))
        return "\n".join(lines)
    lines.append("Computed from %d records (digest `%s`): **%s.**" % (len(results), dg, ", ".join(
        "%s %d" % (STATUS_WORD[k], c[k]) for k in STATUS_ORDER)))
    if unproven:
        lines.append("Default ON in the build without a closed proof (the docs say so): %s." % ", ".join(
            "`%s`" % r["id"] for r in unproven))
    mt = " | ".join("%s %s" % (i, v if v is not None else "(%s)" % res) for i, v, res, _ in items)
    um = "; not metered: %s" % ", ".join(m["unmetered"]) if m.get("unmetered") else ""
    lines.append("**Meter** (%s): %s%s." % (m.get("label", "-"), mt, um))
    for label, (i, v, res, _) in branch:
        lines.append("Off dev: %s: %s %s." % (label, i, v if v is not None else "(%s)" % res))
    lines.append("")
    lines.append("| Obligation | Status | Docs claim | First reason (`python tools/native_ledger.py -v --id <id>`) |")
    lines.append("|---|---|---|---|")
    for r in results:
        lines.append("| `%s` %s | %s | %s%s | %s |" % (r["id"], cell(r["title"], 110), STATUS_WORD[r["status"]],
                                                    r["claimed"], " !" if r["claim_mismatch"] else "",
                                                    cell(r["why"][0])))
    return "\n".join(lines)


def doc_blocks(path):
    """[(style, current inner text)] of the generated blocks in a doc."""
    txt = read_text(path).replace("\r\n", "\n")
    return [(m.group(2), m.group(3)) for m in BLOCK.finditer(txt)]


def write_status(path, doc, results, ctx):
    raw = read_text(path)
    crlf = "\r\n" in raw
    txt = raw.replace("\r\n", "\n")
    n = [0]

    def sub(m):
        n[0] += 1
        return m.group(1) + "\n" + render_block(m.group(2), doc, results, ctx) + "\n" + m.group(4)
    txt = BLOCK.sub(sub, txt)
    if not n[0]:
        return 0
    if crlf:
        txt = txt.replace("\n", "\r\n")
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(txt)
    return n[0]


def stale_blocks(doc, results, ctx, repo):
    """Status docs whose generated block differs from what the ledger computes now."""
    out = []
    for rel in doc.get("status_docs", []) or []:
        p = os.path.join(repo, rel)
        if not os.path.exists(p):
            out.append("%s (missing)" % rel)
            continue
        blocks = doc_blocks(p)
        if not blocks:
            out.append("%s (no native-ledger block)" % rel)
        for style, inner in blocks:
            if inner.strip("\n") != render_block(style, doc, results, ctx):
                out.append("%s (%s block stale: run --write-status %s)" % (rel, style, rel))
    return out


# ---------------------------------------------------------------------------------------------- scene sidecar

def cmd_sheet(roll, out):
    """A contact sheet of a freeze roll's GRADED freezes (one middle present each), labelled with the world class the
    gate's world rule gives it - the way to LOOK at what a freeze grade passed. roll = a freeze roll dir (or a
    gate's freeze/)."""
    from PIL import Image, ImageDraw
    wf = world_floor()
    sys.path.insert(0, os.path.join(REPO, "tools", "perf"))
    import flicker_gate as fg  # noqa: E402
    d = os.path.abspath(roll).rstrip("\/")
    log = fg.run_log(d)
    if not log or not os.path.exists(d + "_freeze.txt"):
        print("%s: needs drive/attempt1/run.log and %s_freeze.txt" % (disp(d), disp(d)))
        return 2
    r = fg.grade_freeze(d, 1, readonly=True)
    cls = {f: k for k, ids in r.get("world", {}).items() for f in ids}
    mids = wf.freeze_middles(read_text(log))   # the world rule's own pick: the middle LOGGED dump, one per present
    byp = wf.frames_by_present(os.path.join(d, "drive", "attempt1", "frames"))
    tiles = []
    for row in r.get("freezes", []):
        fid = row["freeze"]
        if fid not in cls or not mids.get(fid):
            continue
        p = mids[fid][0]
        c = wf.measure(byp[p], "center") if p in byp else None
        label = "freeze %d p%d %s luma %s" % (fid, p, cls[fid], ("%.1f" % c[0]) if c else "-")
        if p in byp and os.path.exists(byp[p]):
            im = Image.open(byp[p]).convert("RGB").resize((320, 180))
        else:  # the BMP is gone: the class came from frames_measured.json (or it is unmeasured) - nothing to show
            im = Image.new("RGB", (320, 180), (48, 48, 48))
            label += " (no BMP)"
        ImageDraw.Draw(im).text((4, 4), label, fill=(255, 255, 0))
        tiles.append(im)
    if not tiles:
        print("%s: no graded freeze with a dumped present" % disp(d))
        return 1
    sheet = Image.new("RGB", (4 * 320, ((len(tiles) + 3) // 4) * 180))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % 4) * 320, (i // 4) * 180))
    sheet.save(out, quality=85)
    print("%s: grade %s counted=%s bad=%s | %s -> %s" % (disp(d), r.get("verdict"), r.get("counted"), r.get("bad"),
                                                        r.get("world_line"), disp(out)))
    wf.save_cache()
    return 0


# ---------------------------------------------------------------------------------------------- main

def main():
    if len(sys.argv) > 1 and sys.argv[1] == "sheet":
        if len(sys.argv) != 4:
            print("usage: native_ledger.py sheet <freeze roll dir> <out.jpg>")
            return 2
        return cmd_sheet(sys.argv[2], sys.argv[3])
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ledger", default=LEDGER)
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 if a record the docs call done/baked computes not closed, a claim quote went stale, "
                         "or a status_docs block differs from what the ledger computes")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every evidence item")
    ap.add_argument("--id", action="append", help="only these obligations (and what they depend on)")
    ap.add_argument("--json", help="write the full results here")
    ap.add_argument("--write-status", nargs="+", metavar="DOC",
                    help="regenerate the <!-- native-ledger:begin:STYLE --> ... <!-- native-ledger:end --> blocks of "
                         "these docs (styles: roadmap, readme)")
    ap.add_argument("--markdown", choices=["roadmap", "readme"], help="print a status block in this style")
    a = ap.parse_args()
    try:
        doc = json.load(open(a.ledger, encoding="utf-8"))
    except (OSError, ValueError) as x:
        print("LEDGER UNREADABLE: %s" % x)
        return 2
    errs = schema_errors(doc)
    if errs:
        print("SCHEMA ERRORS (%d):" % len(errs))
        for x in errs:
            print("  " + x)
        return 2
    ctx = Ctx(doc, REPO)
    only = None
    if a.id:
        only = set()
        byid = {o["id"]: o for o in doc["obligations"]}

        def add(i):
            if i in byid and i not in only:
                only.add(i)
                for d in byid[i].get("depends_on", []) or []:
                    add(d)
        for i in a.id:
            if i not in byid:
                print("unknown id %s" % i)
                return 2
            add(i)
    import atexit
    atexit.register(world_floor().save_cache)
    results = evaluate(doc, ctx, only)
    n_un = sum(1 for r in results for i in r["evidence"] if i["result"] == UNAVAIL)
    if a.markdown or a.write_status:
        if only:
            print("--markdown / --write-status take the whole ledger (drop --id)")
            return 2
        if n_un:
            print("REFUSED: %d evidence item(s) unavailable on this machine - a status written here would understate "
                  "the evidence (run it where the archives are)" % n_un)
            return 2
        if a.markdown:
            print(render_block(a.markdown, doc, results, ctx))
            return 0
        for p in a.write_status:
            n = write_status(p, doc, results, ctx)
            print("%s: %s" % (disp(p), ("%d block(s) regenerated" % n) if n else "no native-ledger block - nothing written"))
        return 0
    w_id = max(len(r["id"]) for r in results)
    print("%-*s  %-11s  %-18s  %s" % (w_id, "id", "status", "docs claim", "why"))
    print("-" * (w_id + 120))
    for r in results:
        flag = " !" if r["claim_mismatch"] else ""
        why = r["why"]
        print("%-*s  %-11s  %-18s  %s" % (w_id, r["id"], r["status"], r["claimed"] + flag, why[0][:150]))
        for extra in why[1:4]:
            print("%-*s  %-11s  %-18s  %s" % (w_id, "", "", "", extra[:150]))
        if len(why) > 4:
            print("%-*s  %-11s  %-18s  (+%d more; -v --id %s)" % (w_id, "", "", "", len(why) - 4, r["id"]))
        if a.verbose:
            for i in r["evidence"]:
                print("%-*s      [%-11s] %-14s %-12s %-13s %s" % (w_id, "", i["result"], i["id"][:14], i["authority"],
                                                                 i["observes"], i["detail"][:200]))
            for key in ("residual_unknowns", "contradictions"):
                for u in r[key]:
                    print("%-*s      %s %s %s: %s" % (w_id, "", key[:-1], u["id"], "RESOLVED" if u["resolved"] else "OPEN",
                                                    u["text"][:150]))
                    for wv in u["witness"]:
                        print("%-*s          witness [%s] %s: %s" % (w_id, "", wv["result"], wv["id"], wv["detail"][:160]))
                    for rb in u["resolved_by"]:
                        print("%-*s          resolved_by [%s] %s: %s" % (w_id, "", rb["result"], rb["id"], rb["detail"][:140]))
        for s in r["stale_claim_refs"]:
            print("%-*s  %-11s  %-18s  STALE claim ref %s" % (w_id, "", "", "", s))
    counts = counts_of(results)
    mism = [r["id"] for r in results if r["claim_mismatch"]]
    stale = [r["id"] for r in results if r["stale_claim_refs"]]
    under = [r["id"] for r in results if r["status"] == "closed" and r["claimed"] not in DONE_CLAIMS]
    unproven = [r["id"] for r in results if r["claimed"] == "shipping-unproven" and r["status"] != "closed"]
    dg = digest(results)
    print("-" * (w_id + 120))
    print("%d obligations: %s | evidence unavailable here: %d | digest %s" % (
        len(results), ", ".join("%s %d" % (k, counts[k]) for k in STATUS_ORDER), n_un, dg))
    print("docs claim done/baked but not closed: %s" % (", ".join(mism) if mism else "none"))
    if unproven:
        print("default ON without a closed proof (the docs say so): %s" % ", ".join(unproven))
    if under:
        print("closed but the docs claim less (update the docs and the record's 'claimed'): %s" % ", ".join(under))
    if not only:
        m, items, branch = meter_eval(doc, ctx)
        print("meter (%s): %s%s" % (m.get("label", "-"), " | ".join("%s %s" % (i, v if v is not None else "(%s)" % res)
                                                                for i, v, res, _ in items),
                                    ("; not metered: " + ", ".join(m["unmetered"])) if m.get("unmetered") else ""))
        for label, (i, v, res, _) in branch:
            print("meter off dev (%s): %s %s" % (label, i, v if v is not None else "(%s)" % res))
    stale_docs = stale_blocks(doc, results, ctx, REPO) if (a.strict and not only) else []
    for s in stale_docs:
        print("status doc: " + s)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"ledger": disp(os.path.relpath(a.ledger, REPO)), "digest": dg, "counts": counts,
                       "results": results}, f, indent=1)
    if a.strict and (mism or stale or stale_docs):
        print("STRICT: FAIL (%s)" % "; ".join(x for x in (
            ("claimed done/baked but not closed: " + ", ".join(mism)) if mism else "",
            ("stale claim refs: " + ", ".join(stale)) if stale else "",
            ("stale status blocks: %d" % len(stale_docs)) if stale_docs else "") if x))
        return 1
    if a.strict:
        print("STRICT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
