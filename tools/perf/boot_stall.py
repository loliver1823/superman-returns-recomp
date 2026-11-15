#!/usr/bin/env python3
"""boot_stall.py - BOOT_STALL: boot.device_create's boot_order_gap, the INTERMITTENT-STALL criterion (design
dcl-stall-v1), pre-registered SEPARATELY from BOOT_EQUIV's mean criterion (Astra round 1 on native/devcreate-close,
2026-10-14, P1-1: ON +30 s on 10% of launches has a true mean regression of 3 s, yet 8 clean pairs - which a mean rule
then passes - happen 0.9^8 = 43% of the time). The run: tools/perf/native_proof.py --plan devcreate_close (step
boot_stall). The rule and its reasons: docs/NATIVE_ROADMAP.md boot.device_create CLOSE DESIGN.

    python tools/perf/boot_stall.py --proof <run's *_proof.json> [--proof ...] --on <roll> [--on ...]
                                    [--design dcl-stall-v1 | dcf-stall-v2] [--allow-key <SR_*> ...] [--json <json>]
    python -m unittest discover -s tools/perf -p test_boot_stall.py

Two registered designs (DESIGNS): dcl-stall-v1 below (29 ON boots, the gate default OFF); dcf-stall-v2 (native/
devcreate-fix, 2026-10-15, the bake plan devcreate_bake; v1 - 7 boots - was withdrawn by Astra's round 1 on the branch
before any roll): the gate default ON - an ON boot is a roll whose provenance has SR_NATIVE_DEVCREATE unset or =1 and is
not a CHECK roll (boot_pairs.declared_on_boot; a roll whose SR_AB_TOGGLE names the gate is the A/B arm, neither) - EVERY
ON boot of the proof counted: the warm-up, the pairs' default rolls, census1, cppm4, the stall block s1_on..s20_on, the
flicker gate's two boots (gate/freeze, gate/span: proof_rows.gate_boots enumerates them from the gate's row) and every
retained attempt (a span the gate re-placed), with the plan's declared observe-only instruments (--allow-key, each in
INSTRUMENT_KEYS) allowed to differ between them; the events below (F / A / W / R, and C / H since v2) and OPEN are the
same; BOUNDED needs >= 29 and the controlled cache condition (every counted boot staged on one SSD path).

THE RULE (frozen before any roll):
  The event, per ON boot (one definition for every boot counted):
    F  a failure: the run's row for the roll is not a success (ROLL_OK: DONE for a plain roll, PASS for a pixel-graded
       one - ABORT / FAIL / anything else is the event), an abort marker anywhere in its log (THREAD_FATAL, ICALL_FAIL,
       a CP stall, a GPU hang trigger, [FATAL]), its outcome not SUCCESS or missing, no HUD request, or a backend wait
       that did not end "attached";
    A  a late attach: the PM4 backend attached more than 5.0 s after the log's first line (every async-load roll on J:
       attached within 0.85 s - bat1, bat2, bat2w, tcmp1 - and dcc4's stalled ON rolls at 26.7 / 42.0 s);
    W  a backend wait: the CP waited for the backend more than 1.0 s (boot_pairs' uncovered wait U - for a load on
       the loader thread the whole interval [CP up, attach]);
    R  the black-world race: an attach after guest VdSwaps or with draw commands queued;
    C  a post-attach wait (WP1; Astra round 1 on native/devcreate-fix): the CP's first command waited more than 1.0 s
       for the installed shader cache after the attach (`[SHADER_CACHE] CP start: waited N ms`) - the attach moved off
       the load's path (f9c08c2d), so a slow load now lands here;
    H  a late HUD: the HUD request later than 95.0 s after the log's first line (HUD_STALL) - the catch-all for a stall
       anywhere in boot the markers do not name (the ON boots of the current exes on J: reach it at 82.3-83.6 s, tcmp2
       and tcmp3, 15 boots; the slowest archived boot without a backend wait on any disk 85.6 s; a 12 s stall is the
       smallest this catches - the stalls the design is sized for are 20-30 s).
  Counting - every boot, crashes included: each --proof is one native_proof run's proof JSON (proof_rows: its `out` is
    the folder it sits in, its rows name rolls by canonical umbrella-relative path, the run's exe and start time
    match every roll). The evaluator enumerates the run's ON boots itself - every roll row that is not SKIP whose
    provenance says the arm is ON, the gate's boots included - and --on must name exactly that set (an ON boot omitted,
    or a named roll that is no ON boot of a declared run, is a design fault); a named roll whose row failed counts as an
    F event even without a folder. An ON attempt the runner VOIDED and retried (the proof's `voids`: native_proof's
    dump-stall void, the roll moved aside before the retry) is a boot too and counts with the F event (Astra round 2,
    P1); so is a RETAINED attempt of the gate (span_try1), counted by its own log - neither needs naming in --on.
    Pooling across runs: every --proof on ONE exe hash (a different build is a different intervention), the event
    identical.
  Design (else INCONCLUSIVE): one exe hash; every counted boot's SR_* configuration equal except the path keys and the
    dump keys (SR_PRESENT_DUMP*, SR_FRAME_DUMP: the meter rolls keep the sparse dump); every ON boot engaged
    (boot_pairs' gate: the ring init line or a first census with packets=0); every boot classifiable (the markers a
    late attach, a wait, the WP1 wait (the bake design) or a late HUD is read from present). THE CACHE CONDITION
    (dcf-stall-v2): the run staged (the proof's `stage`), every counted boot launched from ONE exe path under that
    stage root (provenance exePath) - the bound then speaks for staged-SSD boots, the condition the archive's
    stall-free boots ran under; a boot from another path is a design fault, never a quiet mixture of disks.
  OPEN, never discarded: any event in any counted boot.
  INCONCLUSIVE: a design fault, an unclassifiable boot, or fewer than N_MIN ON boots counted.
  BOUNDED (the tag BOOT_STALL; the only line that prints it): none of N >= N_MIN ON boots had the event. The exact
    (Clopper-Pearson) one-sided 95% upper bound on the per-boot event probability is then 1 - 0.05^(1/N) (N = 29:
    9.81%), ASSUMING the boots are independent with one common probability - a stall that depends on time of day,
    disk load, the boot's position in a run or the build is bounded only for the conditions sampled. It bounds this
    event only; a slow boot without an event is BOOT_EQUIV's (the paired means).
  Why 29 (both designs): a stall of at least 20 s (dcc4's smallest backend wait, 20.4 s) on a fraction p of boots adds
    at least 20 p s to the mean boot; p = 10% is 2.0 s, BOOT_EQUIV's time-to-HUD margin. N = 29 is the least N with
    1 - 0.05^(1/N) < 10%; an ON stall on 10% of boots then passes with probability 0.9^29 = 4.7% (< 5%). Astra's
    round 1 on native/devcreate-fix: the 7-boot v1 let that regression through 0.9^7 = 47.8% of the time - the root
    cause explains the archived waits' synchronous-cache component, not a devcreate interaction under a cold or
    contended cache, which only the sample excludes.
Read-only; no game runs. Exit 0 with a verdict; 2 for bad arguments.
"""
import argparse
import json
import os
import sys
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boot_pairs as bp  # noqa: E402
import proof_rows as pr  # noqa: E402

# THE REGISTERED DESIGNS (--design; the ledger's resolver item names the design and its minimum):
#   dcl-stall-v1 (native/devcreate-close, 2026-10-14): 29 ON boots, the gate default OFF (an ON boot's provenance says
#     SR_NATIVE_DEVCREATE=1), one plain configuration.
#   dcf-stall-v2 (native/devcreate-fix, 2026-10-15; the bake plan devcreate_bake; after Astra's round 1 on the branch):
#     the gate default ON (an ON boot's provenance has the key unset or =1 and is not a CHECK roll; a roll whose
#     SR_AB_TOGGLE names the gate is the A/B arm, neither), EVERY ON boot of the bake proof counted - the warm-up, the
#     four pairs' default rolls, census1, cppm4, the stall block s1_on..s20_on, the gate's freeze and span boots and any
#     retained attempt - whatever its observe-only instrument (declared with --allow-key, each from INSTRUMENT_KEYS);
#     N_MIN 29 as dcl's (0.9^29 = 4.7%: the 10%-of-boots regression passes with < 5%), the events C and H added, the
#     staged-SSD cache condition required. v1 (N_MIN 7, bound 34.8%) was withdrawn before any roll: 7 boots let a 30 s
#     stall on 10% of boots through 47.8% of the time, and the root cause does not exclude a devcreate interaction under
#     a cold or contended cache - the sample does.
DESIGNS = {"dcl-stall-v1": {"n_min": 29, "baked": False}, "dcf-stall-v2": {"n_min": 29, "baked": True}}
DESIGN_ID = "dcl-stall-v1"   # the default design (--design); the registry above is the one place its numbers live
N_MIN = DESIGNS[DESIGN_ID]["n_min"]
BAKED = DESIGNS[DESIGN_ID]["baked"]
# dcf-stall-v2: the keys the counted boots may differ in beyond the path / dump keys, each DECLARED by the plan
# (--allow-key) and each an observe-only instrument of the bake proof's rolls - the producer census, the exact-pixel
# CPPM4 split and its dedup check, the freeze instrument (cppm4 and the gate's freeze roll), the graceful quit. Never a
# native kind, never the gate.
INSTRUMENT_KEYS = ("SR_NATIVE_BRACKETS", "SR_NATIVE_FLUSH_CPPM4", "SR_NATIVE_CONST_DEDUP_CHECK", "SR_DIAG_SIM_FREEZE",
                   "SR_DRIVE_GRACEFUL_QUIT")


def select_design(design_id):
    """Select a registered design (--design): its minimum count and the gate's default."""
    global DESIGN_ID, N_MIN, BAKED
    d = DESIGNS[design_id]
    DESIGN_ID, N_MIN, BAKED = design_id, d["n_min"], d["baked"]


ATTACH_STALL = 5.0          # s after the log's first line
U_MAX = bp.U_MAX             # 1.0 s: the backend wait (W) and the post-attach WP1 wait (C)
HUD_STALL = 95.0             # s after the log's first line: the HUD request later than this is the event H
ROLL_OK = ("DONE", "PASS")   # a roll row that is a success: DONE (a plain roll), PASS (a pixel-graded roll: freeze / A/B)
TAG = "BOOT_STALL"
DUMP_KEYS = ("SR_PRESENT_DUMP", "SR_FRAME_DUMP")  # prefixes allowed to differ between counted boots


def upper95(n):
    """The exact one-sided 95% upper bound on p after 0 events in n independent trials."""
    return 1.0 - 0.05 ** (1.0 / n) if n > 0 else 1.0


def events_of(r, row):
    """The stall events of one ON boot (boot_pairs.roll() dict, its run row) -> ([events], [problems])."""
    ev, probs = [], []
    v = (row or {}).get("verdict")
    if v not in ROLL_OK:
        ev.append("F: the run's row says %s (%s)" % (v, str((row or {}).get("detail", ""))[:80]))
    if r.get("notrun"):
        return ev, ([] if ev else ["not run (no roll folder) and its row did not fail"])
    what, line = bp.abort_marker(r["roll"])
    if what:
        ev.append("F: %s in the log (%s)" % (what, line))
    if r.get("open"):
        ev.append("F: " + ", ".join(r["open"]))
    if isinstance(r.get("attach"), (int, float)) and not r["attach"] <= ATTACH_STALL:
        ev.append("A: attach at +%.2f s (> %.1f s)" % (r["attach"], ATTACH_STALL))
    if isinstance(r.get("U"), (int, float)) and not r["U"] <= U_MAX:
        ev.append("W: the CP waited %.2f s for the backend (> %.1f s)" % (r["U"], U_MAX))
    if r.get("swaps_before") or r.get("queued_before"):
        ev.append("R: attach after %d guest VdSwaps / %d queued draws" % (r.get("swaps_before", 0),
                                                                         r.get("queued_before", 0)))
    if isinstance(r.get("C"), (int, float)) and not r["C"] <= U_MAX:
        ev.append("C: the CP's first command waited %.2f s for the installed shader cache after the attach (> %.1f s)" % (
            r["C"], U_MAX))
    if isinstance(r.get("hud"), (int, float)) and not r["hud"] <= HUD_STALL:
        ev.append("H: the HUD request at +%.2f s (> %.1f s)" % (r["hud"], HUD_STALL))
    if not ev:  # classifiable only with the markers the events are read from
        need = [("attach", "attach"), ("U", "uncovered wait (CP up / attach / wait)"), ("hud", "HUD request")]
        if BAKED:
            need.append(("C", "CP start (WP1) line"))
        for k, name in need:
            if k not in r:
                probs.append("unclassifiable: no %s" % name)
        if "attach" in r and "swaps_before" not in r:
            probs.append("unclassifiable: the attach line has no VdSwaps / queued-draws fields")
    return ev, probs


def stage_root(doc):
    """The run's --stage root from its proof JSON (native_proof writes `stage` as the runner's keywords or a string)."""
    st = doc.get("stage")
    if isinstance(st, dict):
        st = st.get("stage")
    return str(st) if st else ""


def evaluate(proofs, on_rolls, allow_keys=()):
    design, events, unclass = [], [], []
    named = {pr.key(p): p for p in on_rolls}
    if len(named) != len(on_rolls):
        design.append("a roll named twice in --on")
    allow = tuple(allow_keys or ())
    if allow and not BAKED:
        design.append("--allow-key is dcf-stall-v2's (design %s counts one configuration)" % DESIGN_ID)
    for k in allow:
        if k not in INSTRUMENT_KEYS:
            design.append("--allow-key %s is not an observe-only instrument (allowed: %s)" % (k, ", ".join(INSTRUMENT_KEYS)))
    counted, runs, voided, retained, stages = {}, [], [], [], []
    for js in proofs:
        doc, rows, bad = pr.run_rows(js)
        if bad:
            design.append(bad)
            continue
        runs.append((js, doc, rows))
        design += pr.identity_problems(doc, rows, list(rows))
        if BAKED:  # the cache condition: a staged run
            st = stage_root(doc)
            if not st:
                design.append("%s: the run was not staged (--stage): the bake design's cache condition is the staged SSD" %
                              os.path.basename(js))
            stages.append(st)
        for k, row in rows.items():
            if row.get("verdict") == "SKIP":
                continue
            prov = pr.read_json(os.path.join(k, "drive", "attempt1", "provenance.json")) or {}
            env = prov.get("effectiveSrEnv")
            is_on = isinstance(env, dict) and bp.declared_on_boot(env, BAKED)
            if row.get("retained"):  # a retained attempt of the gate: counted by its own log, never named in --on
                if is_on:
                    retained.append((k, (js, row)))
                elif not isinstance(env, dict) and row.get("verdict") not in ROLL_OK:
                    design.append("%s: a failed retained attempt of %s whose arm cannot be read (no provenance)" % (
                        k, os.path.basename(js)))
                continue
            if is_on and k not in named:
                design.append("%s: an ON boot of %s that --on does not count" % (k, os.path.basename(js)))
            if k in named:
                counted[k] = (js, row)
            elif not isinstance(env, dict) and row.get("verdict") not in ROLL_OK:
                design.append("%s: a failed roll of %s whose arm cannot be read (no provenance)" % (k, os.path.basename(js)))
        # Astra round 2 (P1): an attempt the runner VOIDED and retried (native_proof's dump-stall void: the roll moved to
        # <roll>_void<k> and listed in the proof's `voids`, not its rows) was a boot too - an ON one counts, as F
        for v in doc.get("voids") or []:
            rel = str((v or {}).get("roll") or "") if isinstance(v, dict) else ""
            if not rel:
                design.append("%s: a voided attempt without a roll path" % os.path.basename(js))
                continue
            k = pr.key(os.path.join(pr.UMBRELLA, rel.replace("/", os.sep)))
            env = (pr.read_json(os.path.join(k, "drive", "attempt1", "provenance.json")) or {}).get("effectiveSrEnv")
            if isinstance(env, dict):
                is_on = bp.declared_on_boot(env, BAKED)
            else:  # its arm from the step it was an attempt of
                is_on = any(row.get("step") == v.get("step") and kk in named for kk, row in rows.items())
                if not is_on:
                    design.append("%s: a voided attempt of %s whose arm cannot be read" % (k, os.path.basename(js)))
            if is_on:
                voided.append((k, v))
    for k, p in named.items():
        if k not in counted:
            design.append("%s: no declared run (--proof) records it" % p)
    boots = []
    for k, v in voided:  # every voided ON attempt: a boot with the failure event (the retry is counted on its own)
        r = bp.roll(k, baked=BAKED) if os.path.isdir(k) else {"roll": k, "notrun": True, "open": [], "problems": [], "env": {}}
        r["roll"] = k
        ev = ["F: an attempt voided and retried by the runner (%s)" % "; ".join(
            str(x) for x in (v.get("evidence") or []))[:160]]
        boots.append((k, r, ev))
        events.append("%s: %s" % (k, "; ".join(ev)))
    for k, (js, row) in sorted(counted.items()) + sorted(retained):
        r = bp.roll(k, baked=BAKED) if os.path.isdir(k) else {"roll": k, "notrun": True, "open": [], "problems": [], "env": {}}
        r["roll"] = k
        if row.get("retained"):
            r["retained"] = row.get("detail", "a retained attempt")
        if os.path.isdir(k):
            if not bp.declared_on_boot(r["env"], BAKED):
                design.append("%s: named in --on but its provenance is not an ON boot (SR_NATIVE_DEVCREATE=%s, CHECK %s, "
                              "A/B %s; design %s)" % (k, r["env"].get(bp.GATE), r["env"].get("SR_NATIVE_FLUSH_CHECK") == "1",
                                                     bp.arm_of(r["env"], BAKED) is None, DESIGN_ID))
            elif r.get("gate") != "on" and row.get("verdict") in ROLL_OK and not r.get("open"):
                design.append("%s: devcreate not engaged (%s)" % (k, r.get("gate")))
        ev, probs = events_of(r, row)
        boots.append((k, r, ev))
        if ev:
            events.append("%s: %s" % (k, "; ".join(ev)))
        unclass += ["%s: %s" % (k, x) for x in probs]
    ran = [r for _k, r, _e in boots if not r.get("notrun")]
    exes = set(r.get("exe", "") for r in ran)
    if ran and (len(exes) != 1 or "" in exes):
        design.append("exe hashes %s (want exactly one)" % sorted(x[:12] for x in exes))
    # the cache condition (dcf-stall-v2): one exe PATH for every counted boot, under the run's stage root
    paths = sorted(set(os.path.normcase(r.get("exepath") or "") for r in ran))
    if BAKED and ran:
        if len(paths) != 1 or paths == [""]:
            design.append("the counted boots were launched from %d exe paths %s (the cache condition: one staged path)" % (
                len(paths), [p[-60:] for p in paths]))
        roots = [os.path.normcase(os.path.abspath(s)) for s in stages if s]
        if roots and paths != [""] and not all(any(p.startswith(rt) for rt in roots) for p in paths):
            design.append("an exe path outside the run's stage root %s: %s" % (roots, [p[-60:] for p in paths]))
    vol = set(bp.VOLATILE) | set(allow)
    norm = lambda e: {a: b for a, b in (e or {}).items() if a not in vol and not a.startswith(DUMP_KEYS)}
    if ran and len(set(json.dumps(norm(r.get("env")), sort_keys=True) for r in ran)) != 1:
        keys = sorted(set(a for r in ran for a in norm(r.get("env"))
                          if len(set(json.dumps(norm(x.get("env")).get(a)) for x in ran)) != 1))
        design.append("the counted boots' SR_* configuration differs beyond the path and dump keys%s (%s)" % (
            (" and the declared instruments " + "+".join(allow)) if allow else "", ", ".join(keys)))
    # information: which declared instruments the counted boots actually differ in
    differ = sorted(a for a in allow if len(set(json.dumps((r.get("env") or {}).get(a)) for r in ran)) > 1) if ran else []
    n = len(boots)
    lines = ["%-3s %-58s %7s %6s %6s %6s %8s  %s" % ("#", "ON boot", "attach", "U", "L", "C", "HUD", "events")]
    num = lambda r, k, f="%.2f": (f % r[k]) if isinstance(r.get(k), (int, float)) else "-"
    for i, (k, r, ev) in enumerate(boots):
        lines.append("%-3d %-58s %7s %6s %6s %6s %8s  %s%s" % (
            i + 1, k[-58:], num(r, "attach"), num(r, "U"), num(r, "L", "%.3f"), num(r, "C"), num(r, "hud"),
            "; ".join(ev) or "-", " [retained attempt]" if r.get("retained") else ""))
    head = "design=%s on_boots=%d%s%s" % (DESIGN_ID, n, (" instruments=" + "+".join(differ)) if differ else "",
                                          (" retained=%d" % len(retained)) if retained else "")
    if events:
        verdict, why = "OPEN", "(kept, never discarded): %d of %d ON boots had the stall event: %s%s" % (
            len(events), n, " | ".join(events), (" | also: " + "; ".join(design + unclass)) if design or unclass else "")
    elif design or unclass:
        verdict, why = "INCONCLUSIVE", "(not evidence): " + "; ".join(design + unclass)
    elif n < N_MIN:
        verdict, why = "INCONCLUSIVE", "(under-counted): %d ON boots, the design needs %d (0 events in %d bounds the " \
                                       "event probability only below %.2f%%)" % (n, N_MIN, n, 100 * upper95(n))
    else:
        verdict, why = "BOUNDED", (
            "no stall event (F / A / W / R / C / H) in %d ON boots of exe %s%s: the exact one-sided 95%% upper bound on "
            "the per-boot event probability is %.2f%%, assuming independent boots with one common probability; this "
            "event only - the paired mean regression is BOOT_EQUIV's" % (
                n, sorted(exes)[0][:12], (" staged at %s" % stages[0]) if BAKED and stages and stages[0] else "",
                100 * upper95(n)))
    line = "%s %s -> %s %s" % (TAG if verdict == "BOUNDED" else "BOOT_STALL_SET", head, verdict, why)
    lines.append(line)
    clean = lambda r: {a: (b.isoformat() if isinstance(b, datetime) else b) for a, b in r.items() if a != "env"}
    dump = {"design": DESIGN_ID, "verdict": verdict, "tag": TAG if verdict == "BOUNDED" else None, "on_boots": n,
            "events": len(events), "n_min": N_MIN, "baked": BAKED, "allow_keys": list(allow), "instruments_differ": differ,
            "hud_stall_s": HUD_STALL, "roll_ok": list(ROLL_OK), "retained": [k for k, _jr in retained],
            "stage": stages[0] if stages else None, "exe_paths": paths,
            "upper95": upper95(n) if not events else None,
            "proofs": [os.path.abspath(p) for p in proofs], "on": sorted(pr.key(p) for p in on_rolls),
            "event_lines": events, "design_problems": design, "unclassifiable": unclass,
            "boots": [dict(clean(r), events=ev) for _k, r, ev in boots], "line": line}
    return lines, line, dump


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--proof", action="append", required=True, help="a declared run's *_proof.json (repeatable)")
    ap.add_argument("--on", action="append", required=True, help="an ON boot of a declared run (repeatable)")
    ap.add_argument("--design", default="dcl-stall-v1", choices=sorted(DESIGNS),
                    help="the registered design: dcl-stall-v1 (29 ON boots, the gate default OFF) or dcf-stall-v2 (the "
                         "bake: the gate default ON, every ON boot of the proof - the gate's included - >= 29, staged)")
    ap.add_argument("--allow-key", action="append", default=[],
                    help="dcf-stall-v2: an observe-only instrument key the counted boots may differ in (INSTRUMENT_KEYS)")
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    select_design(a.design)
    lines, _line, dump = evaluate(a.proof, a.on, a.allow_key)
    sys.stdout.write("\n".join(lines) + "\n")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(dump, f, indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
