#!/usr/bin/env python3
"""test_native_proof.py - the proof runner's plans, abort rule and ledger edits, offline (iter-467 native-a).

    python tools/perf/test_native_proof.py

- every plan: unique tags, a replay first, A/B steps carry the per-present toggle and the freeze instrument, every
  ledger item has the keys tools/native_ledger.py demands for its kind and an authority that meets a draw-path
  row's `pixels >= frozen-ab` guard somewhere in the plan;
- the abort scan: a CP stall / hang trigger / thread death is found, a clean log is not; a roll without an
  'ingame' era never counts as play, except a save-started roll's HUD ('gameplay' era); a plan tape missing from
  --repo resolves to this checkout's copy;
- the ledger text editor on a temp copy of the real ledger (this checkout's docs/native_ledger.json, else the dev
  checkout's): insert, replace, supersede-to-witness (an existing contradiction, and a created one), a residual
  unknown resolved - each re-parses, keeps the file's one-line-per-item style, passes the ledger tool's schema
  check, and changes only the lines it should.
Exit code = failed checks.
"""
import argparse
import contextlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import native_proof as npf  # noqa: E402

FAILS = 0


def check(ok, name):
    global FAILS
    print(("  ok  " if ok else "FAIL: ") + name)
    if not ok:
        FAILS += 1


def milestone_tags():
    """app/log.cpp's kMilestoneTags (the tags a player-profile log keeps), read from this checkout's source."""
    src = open(os.path.join(npf.LANE, "app", "log.cpp"), encoding="utf-8").read()
    body = src[src.index("kMilestoneTags[] = {"):]
    body = re.sub(r"//[^\n]*", "", body[:body.index("};")])
    return set(re.findall(r'"([A-Z0-9_]+)"', body))


MILESTONES = milestone_tags()


def meter_roll(d, per_frame, kinds=None, frames=1000, fired=None):
    """A perf_run roll folder holding ONE [NATIVE_METER] window (tools/perf/meter_delta.py LINE) of `frames` frames whose
    counts give `per_frame` {column: value per frame} exactly (count = value x frames, an integer), and, with `kinds`
    {kind: per-frame value}, its [NATIVE_METER_KINDS] line: the ledger's meter_delta grade reads these unrounded. fired:
    a last [GPU_CP] census line with zpd=<fired> (p2.zpd's precondition, ZPD_FIRED)."""
    att = os.path.join(d, "drive", "attempt1")
    os.makedirs(att, exist_ok=True)
    cols = " ".join("%s=%d" % (k, int(round(v * frames))) for k, v in per_frame.items())
    lines = ["[2026-10-16 01:00:00.000] [INFO ] [BOOT] [NATIVE_METER] w=1 2.0s frames=%d | fixture | window %s"
             % (frames, cols)]
    if kinds:
        lines.append("[2026-10-16 01:00:00.000] [INFO ] [BOOT] [NATIVE_METER_KINDS] w=1 frames=%d | pm4 per_frame by "
                     "kind: %s" % (frames, " ".join("%s=%s" % (k, repr(float(v))) for k, v in kinds.items())))
    if fired is not None:
        lines.append("[2026-10-16 01:00:01.000] [INFO ] [BOOT] [GPU_CP] census rptr=1 wptr=1 doorbells=9 packets=0 "
                     "draws=99 isr=9 (faults 0) swaps=120 waits=9 (blocked 0) scratch=9 memWrites=9 zpd=%d (fake 0) "
                     "ext=0 badIB=0 desync=0" % fired)
    open(os.path.join(att, "run.log"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
    return d


@contextlib.contextmanager
def meter_fixture_world():
    """The meter grade on fixture rolls: every window counts as ingame (meter_delta.ingame_start_ms -> 0) and the world
    rule sees the drawn world (native_ledger.roll_world -> 10 of 10) - neither has frames or perf metadata to read."""
    import meter_delta as md  # noqa: E402
    nlm = npf.ledger_module()
    with patch.object(md, "ingame_start_ms", lambda d, log: 0.0), \
            patch.object(nlm, "roll_world", lambda ctx, d: (10, 10, 0)):
        yield


def zpd_census_line(why, values):
    """The [NATIVE_ZPD] census line gfx/native_zpd.cpp logLine prints, built from ITS format string (this checkout's
    source) with `values` by field name (0 / 0.0 for the rest): the zpd plan's patterns are checked against the line
    the code prints, not against a copy of it."""
    src = open(os.path.join(npf.LANE, "gfx", "native_zpd.cpp"), encoding="utf-8").read()
    body = src[src.index("void logLine("):]
    body = body[body.index("SR_LOG_BOOT("):body.index("why ? why")]
    fmt = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', body))
    out = []
    for tok in fmt.split(" "):
        m = re.match(r"([A-Za-z0-9]+)=(%[^ ]+)$", tok)
        if tok == "%s" and not out[1:]:
            out.append(why)
        elif m and m.group(1) == "scale":
            out.append("scale=1x1")
        elif m:
            v = values.get(m.group(1), 0)
            prec = re.match(r"%\.(\d)f", m.group(2))
            out.append("%s=%s" % (m.group(1), ("%%.%df" % int(prec.group(1))) % v if prec else "%d" % v)
                       if m.group(1) != "mode" else "mode=%s" % values["mode"])
        else:
            out.append(tok)
    return "[2026-10-15 00:00:00.000] [INFO ] [BOOT] " + " ".join(out) + "\n"


ZPD_Q = 7  # a healthy roll's census values (7 queries of each kind)
ZPD_STRICT_OK = dict(mode="strict", issued=ZPD_Q, ended=ZPD_Q, drained=ZPD_Q, resolved=ZPD_Q, written=ZPD_Q, flushes=4,
                     waitMs=12.5, maxWaitMs=3.25, segs=ZPD_Q, spp1=ZPD_Q, zero=ZPD_Q, viaPartial=ZPD_Q, probes=ZPD_Q,
                     depthKilled=ZPD_Q, lagAvgMs=4.0, lagMaxMs=9.0)
ZPD_LATENCY_OK = dict(ZPD_STRICT_OK, mode="latency", late=ZPD_Q, covered=ZPD_Q, flushes=0, waitMs=0.0, maxWaitMs=0.0,
                      viaPartial=0, viaPresent=ZPD_Q)


def gpu_cp_census(zpd):
    """A [GPU_CP] census line in kernel/gpu_command_processor.cpp's format (zpd6_require_checks checks the format
    still holds) with `zpd` occlusion queries counted, none of them faked."""
    return ("[2026-10-15 00:00:00.000] [INFO ] [BOOT] [GPU_CP] census rptr=64 wptr=64 doorbells=9 packets=99 draws=9 "
            "isr=9 (faults 0) swaps=9 waits=0 (blocked 0) scratch=0 memWrites=0 zpd=%d (fake 0) ext=0 badIB=0 "
            "desync=0 | ringless ib=0 bs=0 desync=0 seg=0\n" % zpd)


def zpd_census_patterns(plans):
    """Every zpd text item matches the census line the code prints for a healthy roll of its mode, and the strict
    items do not match a strict line with a failure in it (the patterns are not vacuous)."""
    p = plans.get("zpd")
    if not p:
        check(False, "zpd: the plan exists")
        return
    strict, latency = ZPD_STRICT_OK, ZPD_LATENCY_OK
    line = {"strict": zpd_census_line("census", strict), "latency": zpd_census_line("census", latency)}
    bad = zpd_census_line("census", dict(strict, flushFail=1, fallback=1))
    check(line["strict"].startswith("[2026") and " probes=7 depthKilled=7 psKilled=0 noRaster=0 lagAvgMs=4.0 "
          "lagMaxMs=9.0" in line["strict"] and " mode=strict issued=7 " in line["strict"],
          "zpd: the census line built from gfx/native_zpd.cpp's own format carries the probe and lag fields")
    seen = 0
    for s in p["steps"]:
        for it in s.ledger:
            if it["kind"] != "text" or "NATIVE_ZPD" not in (it.get("select") or ""):
                continue
            mode = "latency" if "latency" in it["pattern"] or "latency" in it["select"] else "strict"
            ok = bool(re.search(it["select"], line[mode]) and re.search(it["pattern"], line[mode]))
            check(ok, "zpd/%s: item %s matches the %s census line the code prints" % (s.tag, it["id"], mode))
            if it["id"] == "zpd_strict":
                check(not re.search(it["pattern"], bad),
                      "zpd/%s: item zpd_strict does not match a census line with flushFail=1 fallback=1" % s.tag)
            seen += 1
    check(seen >= 7, "zpd: the census items checked (%d)" % seen)


def zpd6_require_checks(plans):
    """p2.zpd zpd6 (2026-10-15): a roll whose game issued no occlusion query ([GPU_CP] census zpd=0) has nothing to
    grade. Every zpd roll and item carries that precondition (`require`: native_proof.ZPD_FIRED); the ledger grades an
    item of such a roll NO DATA - never PASS, nor a witness's 'fails as expected' - and the runner repeats the roll once
    (VOID, kept aside), then records the step NO DATA, whose items apply_ledger never lets resolve anything."""
    sys.path.insert(0, os.path.dirname(HERE))
    import native_ledger as nl  # noqa: E402
    census_source = os.path.join(npf.LANE, "kernel", "gpu_submit.cpp")
    if not os.path.isfile(census_source):
        census_source = os.path.join(npf.LANE, "kernel", "gpu_command_processor.cpp")
    with open(census_source, encoding="utf-8") as source_file:
        src = source_file.read()
    def has_census_format(body):
        legacy_tag = '"[GPU_CP] %s rptr=%u wptr=%u ' in body
        split_tag = ('SR_SUBMIT_TAG " %s rptr=%u wptr=%u ' in body and
                     re.search(r'^#define SR_SUBMIT_TAG "\[GPU_CP\]"\s*$', body, re.M) and
                     re.search(r'^#define SR_SUBMIT_TAG "\[NATIVE_SUBMIT\]"\s*$', body, re.M))
        return bool((legacy_tag or split_tag) and "memWrites=%llu zpd=%llu (fake %llu) ext=%llu " in body)

    split_fixture = ('#if SR_ORACLE\n#define SR_SUBMIT_TAG "[GPU_CP]"\n#else\n'
                     '#define SR_SUBMIT_TAG "[NATIVE_SUBMIT]"\n#endif\n'
                     'SR_SUBMIT_TAG " %s rptr=%u wptr=%u memWrites=%llu zpd=%llu (fake %llu) ext=%llu "')
    check(has_census_format(split_fixture) and
          not has_census_format(split_fixture.replace('#define SR_SUBMIT_TAG "[GPU_CP]"', '')) and
          not has_census_format(split_fixture.replace('#define SR_SUBMIT_TAG "[NATIVE_SUBMIT]"', '')) and
          not has_census_format(split_fixture.replace("zpd=%llu", "zpd_missing=%llu")),
          "zpd6: split source format requires both exact tags and unchanged query fields")
    check(has_census_format(src) and
          re.search(npf.ZPD_FIRED["select"], gpu_cp_census(3)) and re.search(npf.ZPD_FIRED["pattern"], gpu_cp_census(3))
          and not re.search(npf.ZPD_FIRED["pattern"], gpu_cp_census(0)),
          "zpd6: the [GPU_CP] census fixture follows kernel/%s; ZPD_FIRED takes zpd=3, not zpd=0" %
          os.path.basename(census_source))
    p = plans["zpd"]
    steps = {s.tag: s for s in p["steps"]}
    rolls = [s for s in p["steps"] if s.kind == "roll"]
    check(len(rolls) == 5 and all(s.require is npf.ZPD_FIRED for s in rolls) and
          all(it.get("require") == npf.ZPD_FIRED for s in rolls for it in s.ledger),
          "zpd6: every zpd roll (%s) and every item of one carries the [GPU_CP] zpd>0 precondition" %
          ", ".join(s.tag for s in rolls))
    check(all(s.from_save == npf.ZPD_SEED and s.play == npf.ZPD_TAPE for s in rolls) and
          npf.ZPD_SEED.endswith("12_ch12_after_bizarro1"), "zpd6: every zpd roll plays the tape from the seed-12 save")
    roots = {"repo": ".", "umbrella": "."}
    errs = [x for s in rolls for it in s.ledger for x in nl.item_errors("p2.zpd", dict(it, path="x"), roots, set())]
    check(not errs, "zpd6: the items with `require` pass the ledger's schema check%s" % (": %s" % errs[:2] if errs else ""))
    base = {"id": "i", "kind": "text", "path": "x", "authority": "static", "observes": "build"}
    check(nl.item_errors("o", dict(base, require={"select": "a"}), roots, set()) and
          nl.item_errors("o", dict(base, require=dict(npf.ZPD_FIRED, pattern="(")), roots, set()) and
          nl.item_errors("o", dict(base, kind="json", checks=[], require=npf.ZPD_FIRED), roots, set()),
          "zpd6: a require without a pattern, with a bad regex, or on a kind without a roll log is a schema error")

    # the ledger: a zpd=0 roll's items are NO DATA whatever their own check says
    strict, off, fz = steps["on"].ledger[0], steps["off"].ledger[0], steps["freeze"].ledger[0]
    with tempfile.TemporaryDirectory() as td:
        ctx = nl.Ctx({"roots": {"repo": td, "umbrella": td}}, td)
        for name, zpd in (("zero", 0), ("fired", 12)):
            d = os.path.join(td, name, "drive", "attempt1")
            os.makedirs(d)
            with open(os.path.join(d, "run.log"), "w", encoding="utf-8") as f:  # a census line zpd_strict accepts
                f.write(gpu_cp_census(zpd) + zpd_census_line("census", ZPD_STRICT_OK))
        os.makedirs(os.path.join(td, "nolog"))

        def at(item, roll, text=True):
            return dict(item, path=roll + "/drive/attempt1/run.log" if text else roll)
        res, det = nl.check_item(ctx, at(strict, "zero"))
        check(res == nl.FAIL and det.startswith("NO DATA: the game issued no occlusion query") and
              det.endswith(": zpd=0 (fake 0)"), "zpd6: a zpd=0 roll's zpd_strict is NO DATA, never PASS (%s)" % det[:120])
        res, det = nl.check_item(ctx, at({k: v for k, v in strict.items() if k != "require"}, "zero"))
        check(res == nl.PASS, "...its census line passes zpd_strict without the precondition (not vacuous: %s)" % det[:80])
        res, det = nl.check_item(ctx, at(strict, "fired"))
        check(res == nl.PASS and det.endswith(" | precondition [GPU_CP] zpd>0 met: zpd=12 (fake 0)"),
              "zpd6: with zpd=12 the same line passes and shows the precondition (%s)" % det[-60:])
        res, det = nl.check_item(ctx, dict(at(strict, "zero"), expect="fail"))
        check(res == nl.FAIL and det.startswith("NO DATA"), "zpd6: a NO DATA witness (expect fail) never 'fails as expected'")
        res, det = nl.check_item(ctx, at(off, "zero"))
        check(res == nl.FAIL and det.startswith("NO DATA"),
              "zpd6: zpd_off_fires on a zpd=0 roll is NO DATA (no data for never_fired either way)")
        check(nl.check_item(ctx, at(off, "fired"))[0] == nl.PASS, "...and passes on a zpd=12 roll")
        res, det = nl.check_item(ctx, at(fz, "zero", text=False))
        check(res == nl.FAIL and det.startswith("NO DATA"), "zpd6: a zpd=0 freeze roll's zpd_freeze is NO DATA")
        res, det = nl.check_item(ctx, at(fz, "fired", text=False))
        check(res == nl.FAIL and "no freeze table" in det, "...a zpd=12 one grades its freeze table as always (%s)" % det[:60])
        res, det = nl.check_item(ctx, at(fz, "nolog", text=False))
        check(res == nl.FAIL and det.startswith("NO DATA"), "zpd6: a roll directory without a run.log is NO DATA")
        check(nl.check_item(ctx, at(strict, "absent"))[0] == nl.UNAVAIL, "...an absent archive roll stays unavailable")

    # the runner: unmet = VOID, the roll kept aside and repeated once; unmet twice = NO DATA, never PASS / DONE
    def roll(step, zpds, pixels="PASS", aborts=None):
        aborts = list(aborts or [])
        with tempfile.TemporaryDirectory() as td:
            calls, logs = [], []

            def fake_roll_long(exe, out, gates, repo, play, *a, **kw):
                d = os.path.join(out, "drive", "attempt1")
                os.makedirs(d)
                with open(os.path.join(d, "run.log"), "w", encoding="utf-8") as f:
                    f.write(gpu_cp_census(zpds[len(calls)]))
                calls.append((gates, play, kw.get("from_save")))
                return 0
            runner = SimpleNamespace(
                a=SimpleNamespace(dry_run=False), exe="fixture.exe", repo=td, path=lambda tag: os.path.join(td, "zpd_" + tag),
                gates_for=lambda s: s.gates, rel=lambda x: x, voids=[], abort=None, log=logs.append,
                write_proof=lambda: None, write_stamp=lambda *a, **k: None, record=lambda s, v, d, paths=None: {"verdict": v, "detail": d})
            with patch.object(npf, "roll_long", side_effect=fake_roll_long), \
                    patch.object(npf.fg, "roll", side_effect=AssertionError("a long roll re-rolled as the gate's")), \
                    patch.object(npf, "scan_abort", side_effect=lambda log: aborts.pop(0) if aborts else (None, None)), \
                    patch.object(npf, "dump_stall_void", return_value=(True, ["the frame-dump swap stall"])), \
                    patch.object(npf, "reached_play", return_value=(True, "gameplay")), \
                    patch.object(npf, "grade_freeze", return_value={"verdict": pixels, "counted": 26, "bad": 0}):
                r = npf.Runner.do_roll(runner, step)
            return r, runner.voids, calls, sorted(os.listdir(td)), logs
    r, voids, calls, kept, logs = roll(steps["on"], [0, 0])
    check(r["verdict"] == "NO DATA" and len(calls) == 2 and calls[0] == calls[1],
          "zpd6: a zpd=0 roll is repeated once (the same roll) and a second zpd=0 records NO DATA (%s)" % r["verdict"])
    check(len(voids) == 1 and voids[0]["rule"] == "require" and voids[0]["roll"].endswith("zpd_on_noquery1") and
          "zpd=0 (fake 0)" in voids[0]["evidence"][0] and {"zpd_on", "zpd_on_noquery1"} <= set(kept),
          "...the first is a VOID kept as zpd_on_noquery1 with its evidence")
    check(r["detail"].endswith(" | precondition [GPU_CP] zpd>0 NOT MET twice - NO DATA: zpd=0 (fake 0)") and
          any("VOID (precondition not met" in x for x in logs), "...the roll's row shows the precondition (%s)" % r["detail"])
    r, voids, calls, _kept, _logs = roll(steps["freeze"], [0, 0], pixels="PASS")
    check(r["verdict"] == "NO DATA", "zpd6: a zpd=0 freeze roll can't pass even when its pixels do (%s)" % r["verdict"])
    r, voids, calls, _kept, _logs = roll(steps["freeze"], [0, 12], pixels="PASS")
    check(r["verdict"] == "PASS" and len(voids) == 1 and len(calls) == 2 and
          " | precondition [GPU_CP] zpd>0 met: zpd=12 (fake 0) | freeze grade PASS" in r["detail"],
          "zpd6: a zpd=0 roll whose re-roll fires is graded on the re-roll (%s)" % r["verdict"])
    r, voids, calls, _kept, _logs = roll(steps["lat"], [12])
    check(r["verdict"] == "DONE" and not voids and len(calls) == 1 and "precondition [GPU_CP] zpd>0 met: zpd=12" in r["detail"],
          "zpd6: a roll that fires is not repeated; its row shows the precondition met")
    r, voids, calls, kept, _logs = roll(steps["cppm4"], [5, 5], aborts=[("GPU hang trigger", "x")])
    check(r["verdict"] == "PASS" and len(calls) == 2 and calls[0] == calls[1] and calls[0][1] == npf.ZPD_TAPE and
          calls[0][2].replace("\\", "/").endswith(npf.ZPD_SEED) and [v.get("rule") for v in voids] == [None] and
          "zpd_cppm4_void1" in kept,
          "zpd6: a dump-stall VOID of a long roll repeats it with its own tape and save (not the gate's new-game roll)")
    r, voids, calls, _kept, _logs = roll(npf.Step("roll", "plainfix", "", "plain", play=npf.ZPD_TAPE, dwell=260), [0])
    check(r["verdict"] == "DONE" and not voids and len(calls) == 1 and "precondition" not in r["detail"],
          "zpd6: a roll without a precondition is untouched")

    # NO DATA is recorded, but resolves nothing
    ns = SimpleNamespace(plan=p, results=[{"step": "on", "verdict": "NO DATA"}, {"step": "off", "verdict": "DONE"}])
    check(npf.Runner.nodata_ids(ns) == {it["id"] for it in steps["on"].ledger},
          "zpd6: Runner.nodata_ids names the NO DATA step's items only")


def zpd7_xenia_checks(plans):
    """p2.zpd zpd7 (2026-10-15): the plan zpd_xenia - the ledger's `pixels >= xenia` guard. One live roll traces frames
    that hold HUD glow queries (SR_PM4_TRACE_HOTKEY + SR_PM4_TRACE_ZPD, its traces in <roll>_xtr), and the xenia step
    pairs Xenia's strict answers with ours (tools/perf/zpd_xenia.py; test_zpd_xenia.py grades it)."""
    sys.path.insert(0, os.path.dirname(HERE))
    import native_ledger as nl  # noqa: E402
    p = plans.get("zpd_xenia")
    check(p is not None and p["obligations"] == ["p2.zpd"], "zpd7: plan zpd_xenia proves p2.zpd")
    if not p:
        return
    steps = {s.tag: s for s in p["steps"]}
    x, xe = steps.get("xtrace"), steps.get("xenia")
    check(x is not None and x.kind == "roll" and x.xtr and x.require is npf.ZPD_FIRED and x.from_save == npf.ZPD_SEED
          and x.play == npf.ZPD_TAPE and all(g in x.gates for g in ("SR_NATIVE_ZPD=1", "SR_PM4_TRACE_HOTKEY=1",
                                                                       "SR_PM4_TRACE_ZPD=%d" % npf.ZPD_TRACES,
                                                                       "SR_PM4_TRACE_FRAMES=%d" % npf.ZPD_TRACE_FRAMES)),
          "zpd7: the xtrace roll: seed 12, the zpd tape, the [GPU_CP] zpd>0 precondition, strict, the trace hotkey "
          "armed, SR_PM4_TRACE_ZPD and SR_PM4_TRACE_FRAMES")
    # zpdx2: four single frames held 4 queries; a trace now holds ZPD_TRACE_FRAMES consecutive frames. zpdx3 measured the
    # yield of a 16-frame trace: 2 / 4 / 5 / 3 pairs (14 < 20: FAIL) - the plan's count is sized on that, not on zpdx2's
    # 1 query a frame: twice the item's 20 at the mean (3.5 a trace), 20 at the worst trace (2)
    zpdx3_pairs = (2, 4, 5, 3)
    check(npf.ZPD_TRACE_FRAMES == 16 and npf.ZPD_TRACES * sum(zpdx3_pairs) / len(zpdx3_pairs) >= 2 * 20 and
          npf.ZPD_TRACES * min(zpdx3_pairs) >= 20,
          "zpdx4: %d traces of 16 frames - %.0f pairs at zpdx3's measured yield (3.5 a trace), %d at its worst trace" % (
              npf.ZPD_TRACES, npf.ZPD_TRACES * 3.5, npf.ZPD_TRACES * min(zpdx3_pairs)))
    # zpdx3's query window ran 10,150 swaps (8,688 - 18,840) and tracing stalls the game while the wall-clock tape plays on
    # (~10 s a trace): the quota must fit well inside it, and the roll keeps playing after the tape
    check(("SR_PM4_TRACE_ZPD_GAP=%d" % npf.ZPD_TRACE_GAP) in x.gates and
          (npf.ZPD_TRACES - 1) * npf.ZPD_TRACE_GAP <= 10150 // 2 and x.dwell == npf.ZPD_TRACE_DWELL >= 260 + 120 and
          x.seconds >= x.dwell + 600,
          "zpdx4: the traces at least %d swaps apart (the quota in half of zpdx3's query window), %d s of play after the "
          "HUD (the tape's 260 s + the traces' stalls), a drive timeout above it" % (npf.ZPD_TRACE_GAP,
                                                                                   npf.ZPD_TRACE_DWELL))
    item = xe.ledger[0] if xe is not None and xe.ledger else {}
    check(xe is not None and xe.kind == "xenia_zpd" and xe.base == "xtrace" and item.get("id") == "zpd_xenia" and
          item.get("kind") == "json" and item.get("authority") == "xenia" and item.get("observes") == "pixels",
          "zpd7: the xenia step reads the xtrace roll; its item zpd_xenia is json, authority xenia, observes pixels "
          "(the p2.zpd guard)")
    checks = {(c["field"], c.get("op", "eq")): c["value"] for c in item.get("checks", [])}
    check(checks.get(("verdict", "eq")) == "PASS" and checks.get(("calibration.ok", "eq")) is True and
          checks.get(("paired", "ge")) == 20 and checks.get(("unpaired", "eq")) == 0 and
          checks.get(("agree_rate", "ge")) == 0.98,
          "zpd7: the item re-checks the tool's verdict, the calibration, >= 20 pairs, none unpaired, agree_rate >= 0.98")
    check(not nl.item_errors("p2.zpd", dict(item, path="x"), {"repo": ".", "umbrella": "."}, set()),
          "zpd7: the item passes the ledger's schema check")
    with tempfile.TemporaryDirectory() as td:
        r = npf.Runner(Args(plan="zpd_xenia", out=td, dry_run=True, xenia_dump=os.path.join(td, "nope.exe"),
                            xenia_adapter=-2), p)
        g = r.gates_for(x)
        check("SR_PM4_TRACE_DIR=" + npf.fwd(os.path.join(td, "zpdx_xtrace_xtr")) in g,
              "zpd7: the roll's traces go to <out>/zpdx_xtrace_xtr (SR_PM4_TRACE_DIR)")
        verdict = r.do_xenia_zpd(xe)
        detail = r.results[-1]["detail"]
        check(verdict == "DRY" and "(MISSING)" in detail and "--adapter -2" in detail,
              "zpd7: dry, the xenia step names the dump (MISSING here) and the adapter")
        r.a.dry_run = False
        verdict = r.do_xenia_zpd(xe)
        check(verdict == "FAIL" and "no Xenia trace-dump" in r.results[-1]["detail"],
              "zpd7: without the dump the xenia step is a FAIL, never a SKIP")
        r.results[:] = [{"step": "xtrace", "kind": "roll", "verdict": "DONE", "detail": "", "paths": {}},
                        {"step": "xenia", "kind": "xenia_zpd", "verdict": "PASS", "detail": "", "paths": {}}]
        paths = {it["id"]: it["path"] for _o, it in r.items()}
        check(paths.get("zpd_xenia", "").endswith("zpdx_xenia.json"), "zpd7: the item cites the step's JSON")


def zpd_review_checks(plans):
    """Astra's native/zpd merge review (2026-10-15), the native_proof P2s; each check fails on the code before it.
    (a) zpd_balance_item selected its census line by lost=0 / inflight=0, so an earlier balanced census stood in for an
    unbalanced final one: the final census is selected first, then balance and zero outstanding asserted on it.
    (b) the zpd gate step and its item zpd_gate skipped the no-query precondition (zpd6's gate freeze roll issued no
    query, zpd=0): both of the gate's rolls must meet it - in the runner (VOID, run again once, NO DATA) and in the
    ledger's grading (NO DATA naming the roll) - and the gate plays the zpd tape (flicker_gate --tape)."""
    sys.path.insert(0, os.path.dirname(HERE))
    import native_ledger as nl  # noqa: E402
    p = plans["zpd"]
    steps = {s.tag: s for s in p["steps"]}
    items = {it["id"]: it for s in p["steps"] for it in s.ledger}
    bal = items["zpd_strict_balance"]
    old_bal = dict(bal, select=npf.ZPD_SELECT + "strict" + r" .* lost=0 .* inflight=0 ",
                   pattern=r" ended=(\d+) drained=\d+ resolved=\1 ")
    with tempfile.TemporaryDirectory() as td:
        ctx = nl.Ctx({"roots": {"repo": td, "umbrella": td}}, td)

        def census_roll(name, *census):
            d = os.path.join(td, name, "drive", "attempt1")
            os.makedirs(d)
            with open(os.path.join(d, "run.log"), "w", encoding="utf-8") as f:
                f.write(gpu_cp_census(12) + "".join(zpd_census_line("census", c) for c in census))
            return name + "/drive/attempt1/run.log"
        ok = ZPD_STRICT_OK
        unbalanced = dict(ok, issued=11, ended=11, drained=11, resolved=10, written=10, inflight=1)
        cases = (("final_unbalanced", (ok, unbalanced)), ("final_lost", (ok, dict(ok, lost=1))),
                 ("final_inflight", (ok, dict(ok, inflight=2))))
        for name, census in cases:
            path = census_roll(name, *census)
            old_res, old_det = nl.check_item(ctx, dict(old_bal, path=path))
            res, det = nl.check_item(ctx, dict(bal, path=path))
            check(old_res == nl.PASS and res == nl.FAIL and not det.startswith(nl.NO_DATA),
                  "review P2 balance (%s): an earlier balanced census no longer stands in for the final one - the old "
                  "rule passed it (%s), zpd_strict_balance FAILS (%s)" % (name, old_res, det[:90]))
        res, det = nl.check_item(ctx, dict(bal, path=census_roll("healthy", ok, dict(ok, issued=9, ended=9, drained=9,
                                                                                   resolved=9, written=9))))
        check(res == nl.PASS, "review P2 balance: a balanced final census with nothing lost or in flight passes (%s)" %
              det[:80])

        # (b) the ledger: zpd_gate's precondition on both rolls
        gate = steps.get("gate")
        gi = items.get("zpd_gate", {})
        check(gate is not None and gate.require is npf.ZPD_FIRED and gate.play == npf.ZPD_TAPE and
              gate.from_save == npf.ZPD_SEED and gi.get("kind") == "flicker_gate" and gi.get("require") == npf.ZPD_FIRED,
              "review P2 gate: the zpd gate step (seed 12, the zpd tape) and its item zpd_gate carry the [GPU_CP] zpd>0 "
              "precondition")
        check(not nl.item_errors("p2.zpd", dict(gi, path="x"), {"repo": ".", "umbrella": "."}, set()),
              "review P2 gate: zpd_gate with `require` passes the ledger's schema check")

        def gate_dir(name, fz, sp):
            for sub, z in (("freeze", fz), ("span", sp)):
                d = os.path.join(td, name, sub, "drive", "attempt1")
                os.makedirs(d)
                with open(os.path.join(d, "run.log"), "w", encoding="utf-8") as f:
                    f.write(gpu_cp_census(z))
            return name
        res, det = nl.check_item(ctx, dict(gi, path=gate_dir("g_fz0", 0, 9234)))
        check(res == nl.FAIL and det.startswith(nl.NO_DATA) and det.endswith(": zpd=0 (fake 0) (the gate's freeze roll)"),
              "review P2 gate: a gate whose freeze roll issued no query (zpd6's) is NO DATA naming that roll (%s)" % det)
        res, det = nl.check_item(ctx, dict(gi, path=gate_dir("g_sp0", 12, 0)))
        check(res == nl.FAIL and det.endswith("(the gate's span roll)"),
              "review P2 gate: ... and one whose span roll issued none (%s)" % det[-60:])
        res, det = nl.check_item(ctx, dict(gi, path=gate_dir("g_both", 12, 30)))
        check(not det.startswith(nl.NO_DATA) and
              det.endswith(" | precondition [GPU_CP] zpd>0 met: freeze zpd=12 (fake 0), span zpd=30 (fake 0)"),
              "review P2 gate: both rolls fired -> the gate's own grade, the precondition shown for each (%s)" % det[-90:])

    # (b) the runner: do_gate checks both rolls, VOIDs and runs the gate again once, then NO DATA
    def gate_run(step, zpds, last="FLICKER_GATE PASS"):
        with tempfile.TemporaryDirectory() as td:
            calls, logs = [], []

            def fake_call(cmd, cwd=None, stdout=None, stderr=None):
                out = cmd[cmd.index("--out") + 1]
                fz, sp = zpds[len(calls)]
                for sub, z in (("freeze", fz), ("span", sp)):
                    d = os.path.join(out, sub, "drive", "attempt1")
                    os.makedirs(d)
                    with open(os.path.join(d, "run.log"), "w", encoding="utf-8") as f:
                        f.write(gpu_cp_census(z))
                stdout.write("span   : PASS\nfreeze : PASS counted=26 bad=0\n%s\n" % last)
                calls.append(list(cmd))
                return 0 if last.endswith("PASS") else 1
            runner = SimpleNamespace(
                a=SimpleNamespace(dry_run=False, instance="native"), exe="fixture.exe", repo=td, plan=plans["zpd"],
                path=lambda tag: os.path.join(td, "zpd_" + tag), rel=lambda x: x, voids=[], log=logs.append,
                write_proof=lambda: None, write_stamp=lambda *a, **k: None, record=lambda s, v, d, paths=None: {"verdict": v, "detail": d})
            with patch.object(npf.subprocess, "call", side_effect=fake_call):
                r = npf.Runner.do_gate(runner, step)
            return r, runner.voids, calls, sorted(os.listdir(td)), logs
    if gate is None:
        return
    r, voids, calls, kept, logs = gate_run(gate, [(0, 9234), (0, 9234)])
    check(r["verdict"] == "NO DATA" and len(calls) == 2 and calls[0] == calls[1] and
          "NOT MET twice - NO DATA: freeze zpd=0 (fake 0), span zpd=9234 (fake 0)" in r["detail"],
          "review P2 gate: a gate whose freeze roll issues no query twice is NO DATA though it printed PASS (%s)" %
          r["verdict"])
    check(len(voids) == 1 and voids[0]["rule"] == "require" and voids[0]["roll"].endswith("zpd_gate_noquery1") and
          voids[0]["evidence"] == ["the game issued no occlusion query ([GPU_CP] census zpd=0): freeze zpd=0 (fake 0)"]
          and {"zpd_gate", "zpd_gate.txt", "zpd_gate_noquery1", "zpd_gate_noquery1.txt"} <= set(kept) and
          any("VOID (precondition not met" in x for x in logs),
          "review P2 gate: the first is a VOID kept as zpd_gate_noquery1 (+ .txt) with the freeze roll's evidence")
    cmd = calls[0]
    check("--tape" in cmd and os.path.normcase(cmd[cmd.index("--tape") + 1]) ==
          os.path.normcase(npf.tape_path(npf.LANE, npf.ZPD_TAPE)) and "--from-save" in cmd,
          "review P2 gate: the gate plays the zpd tape from the seed (flicker_gate --from-save --tape)")
    r, voids, calls, _k, _l = gate_run(gate, [(12, 0), (12, 0)])
    check(r["verdict"] == "NO DATA" and len(calls) == 2, "review P2 gate: the span roll's precondition counts too")
    r, voids, calls, _k, _l = gate_run(gate, [(0, 9234), (12, 30)])
    check(r["verdict"] == "PASS" and len(voids) == 1 and len(calls) == 2 and
          r["detail"].endswith(" | precondition [GPU_CP] zpd>0 met: freeze zpd=12 (fake 0), span zpd=30 (fake 0)"),
          "review P2 gate: a re-run whose rolls both fire is graded on the re-run (%s)" % r["detail"][-80:])
    r, voids, calls, _k, _l = gate_run(gate, [(12, 30)], last="FLICKER_GATE FAIL")
    check(r["verdict"] == "FAIL" and not voids and len(calls) == 1,
          "review P2 gate: a gate that fired and failed is a FAIL, not repeated")
    plain = npf.Step("gate", "gate", "SR_NATIVE_FOO=1")
    r, voids, calls, _k, _l = gate_run(plain, [(0, 0)])
    check(r["verdict"] == "PASS" and not voids and len(calls) == 1 and "precondition" not in r["detail"] and
          "--tape" not in calls[0] and "--from-save" not in calls[0],
          "review P2 gate: a gate step without a precondition (the other plans') runs once, as before")


def zpdx3_ledger_checks():
    """p2.zpd after zpd7 / zpdx2 (2026-10-15): a witness whose roll had no data is kept as such - `"expect": "no_data"`
    (tools/native_ledger.py check_item) - because NO DATA never 'fails as expected'; apply_ledger moves a superseded NO
    DATA item into a witness with that expectation (ledger_nodata). Each check fails on the code before it."""
    sys.path.insert(0, os.path.dirname(HERE))
    import native_ledger as nl  # noqa: E402
    gi = next(it for s in npf.plans()["zpd"]["steps"] for it in s.ledger if it["id"] == "zpd_gate")
    with tempfile.TemporaryDirectory() as td:
        ctx = nl.Ctx({"roots": {"repo": td, "umbrella": td}}, td)
        for name, fz, sp in (("g_none", 0, 9234), ("g_both", 12, 30)):
            for sub, z in (("freeze", fz), ("span", sp)):
                d = os.path.join(td, name, sub, "drive", "attempt1")
                os.makedirs(d)
                with open(os.path.join(d, "run.log"), "w", encoding="utf-8") as f:
                    f.write(gpu_cp_census(z))
        w = dict(gi, id="superseded_black.zpd_gate_void", expect="no_data")
        res, det = nl.check_item(ctx, dict(w, path="g_none"))
        check(res == nl.PASS and det.startswith("no data, as expected: NO DATA"),
              "zpdx3: a witness expecting no data passes on its roll's unmet precondition (zpd6's gate) (%s)" % det[:60])
        res, det = nl.check_item(ctx, dict(w, path="g_both"))
        check(res == nl.FAIL and det.startswith("expected NO DATA, but: "),
              "zpdx3: ... and fails when both rolls issued queries (%s)" % det[:50])
        res, det = nl.check_item(ctx, dict(w, expect="fail", path="g_none"))
        check(res == nl.FAIL and det.startswith(nl.NO_DATA), "zpdx3: NO DATA still never 'fails as expected'")
        check(nl.check_item(ctx, dict(w, path="absent"))[0] == nl.UNAVAIL,
              "zpdx3: an absent roll stays unavailable whatever the expectation")
        roots = {"repo": ".", "umbrella": "."}
        check(not nl.item_errors("p2.zpd", dict(w, path="x"), roots, set()) and
              nl.item_errors("p2.zpd", dict({k: v for k, v in w.items() if k != "require"}, path="x"), roots, set()) and
              nl.item_errors("p2.zpd", dict(w, path="x", expect="maybe"), roots, set()),
              "zpdx3: expect no_data is valid with a require, an error without one; an unknown expectation is an error")
        doc = json.load(open(os.path.join(npf.LANE, "docs", "native_ledger.json"), encoding="utf-8"))
        base_errs = nl.schema_errors(doc)
        ob = next(o for o in doc["obligations"] if o["id"] == "p2.zpd")
        ev = next(e for e in ob["evidence"] if e["id"] == "zpd_gate")
        ev["expect"] = "no_data"
        errs = nl.schema_errors(doc)
        check(not base_errs and any("expect 'no_data' belongs in a witness list" in x for x in errs),
              "zpdx3: the live ledger is schema-clean, and expect no_data in an obligation's evidence is an error")

        # apply_ledger: a superseded NO DATA item becomes a no_data witness; a superseded failing one an expect fail
        syn = "\n".join(['{', '  "obligations": [', '    {', '      "id": "x.ob",', '      "evidence": [',
                         '        ' + npf.dumps_item(dict(gi, id="g", root="umbrella", path="g_none")),
                         '      ]', '    }', '  ]', '}', ''])

        class Stub:
            plan = {}

            def __init__(self, its):
                self._its = its

            def items(self):
                return self._its
        new = dict(gi, id="g", root="umbrella", path="g_new")
        for flag, want in ((True, "no_data"), (False, "fail")):
            lp = os.path.join(td, "ledger_%s.json" % want)
            open(lp, "w", encoding="utf-8", newline="").write(syn)
            npf.apply_ledger(lp, Stub([("x.ob", dict(new))]), grade=lambda item: "fail", nodata_of=lambda item: flag)
            o = json.loads(open(lp, encoding="utf-8").read())["obligations"][0]
            wit = o.get("contradictions", [{}])[0].get("witness", [])
            check([x.get("expect") for x in wit] == [want] and o["evidence"][0]["path"] == "g_new",
                  "zpdx3: apply_ledger keeps a superseded %s item as a witness, expect %s" % (
                      "NO DATA" if flag else "failing", want))
        lp = os.path.join(td, "ledger_real.json")
        open(lp, "w", encoding="utf-8").write(json.dumps({"roots": {"repo": ".", "umbrella": td}}))
        nod = npf.ledger_nodata(lp, npf.LANE)
        check(nod(dict(gi, root="umbrella", path="g_none")) and not nod(dict(gi, root="umbrella", path="g_both")) and
              not nod({k: v for k, v in dict(gi, root="umbrella", path="g_none").items() if k != "require"}),
              "zpdx3: ledger_nodata: the ledger tool's NO DATA (zpd6's gate), not a roll with data nor an item without a "
              "precondition")


FIXTURE_REV = "66b2925f"  # the ledger these editor tests were written against (rows467's abp void, s1's unknown open)
# boot.device_create's ledger as PRE-REGISTERED for the bake (native/devcreate-fix 52d9358c, after Astra's round 1 and before
# the dcf2 / dcf3 live sets): the two resolvers OWED in the bake evaluators' form (dcf-v1, dcf-stall-v2), default_bake open,
# the single-pair meter still evidence. The live ledger moved past it with the bake (dcf2 resolved default_bake and retired
# the meter, dcf3 bound boot_order.powered; dev 14ae67bd), so the checks of that state read this revision, never the live file.
BAKE_FIXTURE_REV = "52d9358c"


def ledger_at(rev):
    """(label, root, text): docs/native_ledger.json at git revision `rev` from this checkout's or dev's history, else the
    live file (a checkout without that history: the checks then read what is there and say so in the label)."""
    for root in (npf.LANE, npf.DEV):
        try:
            r = subprocess.run(["git", "show", rev + ":docs/native_ledger.json"], cwd=root,
                               capture_output=True, text=True, encoding="utf-8")
        except OSError:
            continue
        if r.returncode == 0 and r.stdout.strip():
            return "git:" + rev, root, r.stdout
    for root in (npf.LANE, npf.DEV):
        p = os.path.join(root, "docs", "native_ledger.json")
        if os.path.exists(p):
            return p, root, open(p, encoding="utf-8").read()
    return None, None, None


def ledger_fixture():
    """(label, root, text): the ledger at FIXTURE_REV from this checkout's or dev's history - the live file moves with
    every proof (the re-proofs of 2026-10-09 resolved exactly the items these tests supersede) - else the live file."""
    return ledger_at(FIXTURE_REV)


def bake_fixture_doc():
    """boot.device_create's pre-registered ledger (BAKE_FIXTURE_REV) parsed, with its label; None without a ledger."""
    label, _root, text = ledger_at(BAKE_FIXTURE_REV)
    return (label, json.loads(text)) if text else (None, None)


def schema_errors(root, path):
    sys.path.insert(0, os.path.join(root, "tools"))
    import native_ledger as nl  # noqa: E402
    return nl.schema_errors(json.load(open(path, encoding="utf-8")))


class Args:
    def __init__(self, **kw):
        self.plan, self.exe, self.base, self.out, self.repo = "s1_ab", None, None, os.path.join(npf.LANE, "archive", "v1_native"), npf.DEV
        self.owner, self.instance, self.only, self.skip, self.dry_run, self.slot_held = "native-a", "native", "", "", True, True
        self.__dict__.update(kw)


def main():
    # the snapshot step's row text (frs1: a 0.0 tail printed as -1.000): the grader's own tail_s, n/a when absent
    base = {"snapshots": 3, "fresh_bound_s": 1.0, "fresh": True}
    check("tail 0.000 s (bound 1.0 s)" in npf.snapshot_detail(dict(base, tail_s=0.0)),
          "snapshot detail: a 0.0 tail (the last snapshot is the log's last line) prints 0.000 s, not a sentinel")
    check("tail n/a " in npf.snapshot_detail(dict(base, tail_s=None, fresh=False)) and
          "tail n/a " in npf.snapshot_detail({k: v for k, v in base.items()}),
          "snapshot detail: a missing tail (None or absent) prints n/a")
    check("tail 0.474 s" in npf.snapshot_detail(dict(base, tail_s=0.474)), "snapshot detail: a real tail prints itself")
    # the census's S1 residue line: 821075E0's pad is construns' only as the witnessed key o6 (Astra round 1 on
    # native/flush-residue-2); any o1 - frs1 census1's unwitnessed pads, an opening report's excess - is residue
    rx = re.compile(npf.flush_residue_rx())
    line = "[PM4_COVMAP] periodic inner=820FC000 name=D3DDeviceEmitDrawIndexedVertices_sub_820FC000 pk=%d dw=9 keys=%s"
    check(not rx.search(line % (42729, "3C:8406,r0A:16812,r20:8406,r23:8406,o6:699")),
          "flush residue rx: 821075E0's witnessed pads (o6) beside construns runs are not S1 residue")
    check(not rx.search(line % (4, "3C:1,r0A:2,r20:1,r23:1")), "flush residue rx: the construns keys alone are not residue")
    check(bool(rx.search(line % (42729, "3C:8406,r0A:16812,r20:8406,r23:8406,o1:699"))),
          "flush residue rx: frs1 census1's unwitnessed pads (o1, exe 2087ca4acdb4) are residue")
    check(bool(rx.search(line % (30, "r23:10,o1:20"))),
          "flush residue rx: an opening report with more NOPs than runs (r23:10, o1:20) is residue")
    check(bool(rx.search(line % (6, "3C:1,r0A:2,r20:1,r23:1,o6:1,r44:1"))), "flush residue rx: an S1 leaf's key is residue")
    check(bool(rx.search(line % (2, "22:1,o6:1"))), "flush residue rx: a declined draw over 0xFFFF is residue")
    check(bool(rx.search(line % (1, "o1:1"))), "flush residue rx: a NOP alone is residue")
    # the census step's checks include the from-boot zero (an opening residue before the interval)
    fc = {c["field"]: c["value"] for c in npf.flush_census_item("x")["checks"]}
    check(fc.get("checks.target_flush_residue_zero_from_boot") is True and fc.get("target.flush_residue_from_boot.pk") == 0,
          "flush census item: the target's flush_residue is graded from boot too")
    plans = npf.plans()
    check(len(MILESTONES) > 50 and {"LOG_PROFILE", "GPU_CP"} <= MILESTONES,
          "app/log.cpp kMilestoneTags parsed (%d tags, LOG_PROFILE and GPU_CP among them)" % len(MILESTONES))
    required = {"freeze_grade": ["path"], "flicker_gate": ["path"], "meter_delta": ["path", "base", "field"], "text": ["path"],
                "replay_cmp": ["path"], "json": ["path", "checks"], "text_le": ["path", "lhs", "rhs"]}
    # --apply-ledger upserts by item id within an obligation: two plans proving one obligation must not share an id,
    # or one plan's apply replaces (or moves to a witness) the other's evidence (2026-10-14: the callback plan's
    # generic abp / gate / cppm4 ids next to other p2.transport plans)
    owner = {}
    for name, p in plans.items():
        for oid in p.get("obligations", []):
            for s in p["steps"]:
                for it in s.ledger:
                    prev = owner.setdefault((oid, it["id"]), name)
                    check(prev == name, "%s/%s: item id %s is unique in %s (also in plan %s)" % (name, s.tag, it["id"], oid, prev))
    for name, p in plans.items():
        tags = [s.tag for s in p["steps"]]
        check(len(tags) == len(set(tags)), "%s: unique step tags" % name)
        # A plan with no obligation is a control / diagnostic roll (row3_ctrl), not a proof: it needs neither the
        # replay nor the frozen-pass A/B; every proof plan does.
        proof = bool(p.get("obligations"))
        if proof and not p.get("no_replay"):  # no_replay: a stated reason (devcreate_close: the replay runs no guest code)
            check(p["steps"][0].kind == "replay", "%s: the replay comes first" % name)
        for s in p["steps"]:
            if "SR_AB_TOGGLE=" in s.gates:
                check("SR_AB_PRESENT=1" in s.gates and s.mode == "freeze", "%s/%s: the A/B is per present under the freeze" % (name, s.tag))
            for it in s.ledger:
                check(all(k in it for k in required.get(it["kind"], ["path"])) and it["authority"] in npf.fg.__dict__.get("AUTH", ()) or
                      it["authority"] in ("static", "generation", "replay", "live-observe", "live-exact", "frozen-ab", "xenia"),
                      "%s/%s: item %s has its keys and a known authority" % (name, s.tag, it["id"]))
        # Every live roll runs the player log profile (SR_LOG=player): app/log.cpp drops an Info line whose tag is not
        # in kMilestoneTags, so an item that selects its line by a tag outside that list reads nothing live (zpd3,
        # 2026-10-15: the [NATIVE_ZPD] census - five FAILs with the queries firing).
        missing = sorted({"%s:%s" % (it["id"], m.group(1)) for s in p["steps"] for it in s.ledger
                          for key in ("select", "pattern")
                          for m in [re.match(r"\\\[([A-Z0-9_]+)", it.get(key) or "")]
                          if m and m.group(1) not in MILESTONES})
        check(not missing, "%s: every log tag its items select is a player-profile milestone (app/log.cpp "
                           "kMilestoneTags)%s" % (name, (" - missing: " + ", ".join(missing)) if missing else ""))
        frozen = any(it["authority"] == "frozen-ab" and it["observes"] == "pixels" for s in p["steps"] for it in s.ledger)
        # a bake plan re-proves a row whose A/B lives in its own plan; no_ab: a gate-less default-path fix states why
        if proof and not p.get("bake") and not p.get("no_ab"):
            check(frozen, "%s: a frozen-pass A/B item observes pixels (the draw-path guard)" % name)
        meters = [s for s in p["steps"] if s.kind == "meter"]
        for m in meters:
            # the base may be another plan's roll, "<prefix>:<tag>" (a bake's pre-bake default roll)
            check((m.base in tags or ":" in m.base or "/" in m.base) and m.target in tags and
                  all(b in tags and t in tags for b, t in m.pool),
                  "%s: the meter's base and target rolls (and any pooled pair's) are steps (or the base another "
                  "plan's roll)" % name)
        for c in [s for s in p["steps"] if s.kind == "census"]:
            idx = {s.tag: i for i, s in enumerate(p["steps"])}
            check(c.base in idx and c.target in idx and idx[c.base] < idx[c.tag] and idx[c.target] < idx[c.tag]
                  and all(i["kind"] == "json" for i in c.ledger),
                  "%s: the census step reads two earlier rolls and yields json items" % name)

    zpd_census_patterns(plans)
    zpd6_require_checks(plans)
    zpd7_xenia_checks(plans)
    zpd_review_checks(plans)
    zpdx3_ledger_checks()

    # The residue plan's PRODUCER census items (pre-registered): each converted producer present in census0, absent from
    # census1, graded by the ledger tool itself on synthetic [PM4_COVMAP] producer lines.
    sys.path.insert(0, os.path.join(os.path.dirname(HERE)))
    import native_ledger as nl  # noqa: E402
    steps = {s.tag: s for s in plans["residue"]["steps"]}
    names = ("idle", "inval", "vblock", "texlock")
    c0 = [it for it in steps["census0"].ledger if it["id"].startswith("residue_census0_")]
    c1 = [it for it in steps["census1"].ledger if it["id"] in ["residue_census1_" + n for n in names]]
    check([it["id"] for it in c0] == ["residue_census0_" + n for n in names] and
          [it["id"] for it in c1] == ["residue_census1_" + n for n in names],
          "residue plan: one producer census item per converted producer in each census roll")
    check(not steps["census"].ledger, "residue plan: the kind census step is a diagnostic (no ledger item)")

    def covmap(rows):
        return "".join("[t] [INFO ] [BOOT] [PM4_COVMAP] periodic %s=%s name=x_sub_%s pk=9 dw=20 keys=%s\n" % (w, va, va, k)
                       for w, va, k in rows)

    def grade(items, text):
        with tempfile.TemporaryDirectory() as td:
            open(os.path.join(td, "run.log"), "w", encoding="utf-8").write(text)
            ctx = nl.Ctx({"roots": {"umbrella": td}}, npf.LANE)
            return {it["id"].split("_")[-1]: nl.check_item(ctx, dict(it, path="run.log"))[0] for it in items}
    base = covmap([("inner", "820FDCB8", "r05:3"), ("inner", "820F4280", "3C:1,r05:1,r0A:2"),
                   ("inner", "820F3FA0", "3C:4,r0A:8"), ("inner", "820FFDA0", "3C:2,r0A:4"),
                   ("owner", "82112050", "3C:9,r0A:9"), ("inner", "00000000", "3C:5,r05:2,r0A:7")])
    check(set(grade(c0, base).values()) == {nl.PASS}, "producer census: a base census showing each producer's PM4 passes")
    for drop in names:
        rows = {"idle": "820FDCB8", "inval": "820F4280", "vblock": "820F3FA0", "texlock": "820FFDA0"}
        thin = "".join(ln for ln in base.splitlines(True) if "inner=" + rows[drop] not in ln)
        g = grade(c0, thin)
        check(g[drop] == nl.FAIL and all(v == nl.PASS for k, v in g.items() if k != drop),
              "producer census: a base census without %s's lines fails its item only" % drop)
    gated = covmap([("inner", "82112050", "3C:119,r05:54,r0A:164"), ("owner", "820F3FA0", "3C:3,r05:2"),
                    ("inner", "00000000", "3C:5,r05:2,r0A:7"), ("inner", "820F3FA0", "r3C:1,r05:1"),
                    ("inner", "820FDCB8", "r0A:1")])
    check(set(grade(c1, gated).values()) == {nl.PASS},
          "producer census: a gated census with no converted producer's own keys passes (owner lines, UNSTAMPED, "
          "other producers, a type-0 page r3C and another key are not the producer's packets)")
    for drop, va, k in (("idle", "820FDCB8", "r05:1"), ("inval", "820F4280", "r0A:2"), ("inval", "820F4280", "r05:1"),
                        ("vblock", "820F3FA0", "3C:1"), ("texlock", "82100228", "r0A:2"),
                        ("texlock", "821002D8", "3C:1,r0A:2"), ("texlock", "820FFCB0", "r05:1,r0A:2")):
        g = grade(c1, gated + covmap([("inner", va, k)]))
        check(g[drop] == nl.FAIL and all(v == nl.PASS for kk, v in g.items() if kk != drop),
              "producer census: a gated census where %s (%s) still writes %s fails its item only" % (drop, va, k))
    check(set(grade(c1, "[t] [INFO ] [BOOT] [PM4_COV] CUM periodic frames=1\n" +
                    covmap([("owner", "82112050", "3C:9")])).values()) == {nl.FAIL},
          "producer census: a gated census without producer lines (an older exe, no census) never passes")

    # residue_census.py (the kind diagnostic): strict - a malformed or truncated census line is an error (exit 2),
    # never read as zeros; a well-formed pair gives the ratios.
    import residue_census as rcs  # noqa: E402

    def cum(frames, coher, wrm, wu, ops_extra="54:2", regs_extra="0F:1/1", t0=None, t3=None):
        ops = "3C:%d,%s" % (wrm, ops_extra) if ops_extra else "3C:%d" % wrm
        regs = "05:%d/%d,0A:%d/%d,%s" % (wu, wu * 2, coher, coher * 3 // 2, regs_extra) if regs_extra else \
            "05:%d/%d,0A:%d/%d" % (wu, wu * 2, coher, coher * 3 // 2)
        n_t3 = sum(int(x.split(":")[1]) for x in ops.split(",")) if t3 is None else t3
        pk0 = [x.split(":")[1].split("/") for x in regs.split(",")]
        n_t0 = sum(int(p) for p, _ in pk0) if t0 is None else t0
        n_dw = sum(int(d) for _, d in pk0)
        return ("[t] [INFO ] [BOOT] [PM4_COV] CUM periodic frames=%d pk=%d pk/f mean=1.0 min=1 max=9 t0=%d t0dw=%d "
                "t3=%d t3dw=%d pred=0 ringIb=0 zero=0 trunc=0 chainTail=0 t1=0 t2=0 t1skip=0 acc=10 unst=%d "
                "setConst=0/0/0/0/0 loadConst=0/0/0/0 snapOverflow=0 ops=%s regs=%s\n" % (
                    frames, n_t0 + n_t3, n_t0, n_dw, n_t3, n_t3 * 2, n_t0 + n_t3 - 10, ops, regs))

    def residue(idle, inval, lock, pk, lit):
        return ("[t] [INFO ] [BOOT] [NATIVE_FLUSH] periodic residue idle=%d late=0 decl ab=0 seg=0 mem=0 mode=0 "
                "entry=0 owner=0 noPk=0 | inval=%d late=0 merge=0 decl ab=0 seg=0 mem=0 mode=0 entry=0 owner=0 "
                "noPk=0 | lock=%d late=0 merge=0 host=0 decl ab=0 seg=0 mem=0 mode=0 entry=0 owner=0 noPk=0 | "
                "pk=%d litPk=%d | hostApplied=0\n" % (idle, inval, lock, pk, lit))
    with tempfile.TemporaryDirectory() as td:
        b, t = os.path.join(td, "b.log"), os.path.join(td, "t.log")
        open(b, "w", encoding="utf-8").write(cum(10000, 160000, 140000, 90000))
        good_t = cum(10000, 80000, 100000, 88000) + residue(2000, 0, 40000, 160000, 0)
        open(t, "w", encoding="utf-8").write(good_t)
        d = rcs.compute(b, t)
        check("error" not in d and abs(d["ratio"]["coher_reg"] - 1.0) < 1e-9 and
              abs(d["ratio"]["wait_reg_mem"] - 1.0) < 1e-9 and abs(d["ratio"]["wait_until"] - 1.0) < 1e-9,
              "residue census: a well-formed pair - exactly the converted blocks left the PM4 stream: ratios 1.0")
        open(t, "w", encoding="utf-8").write(good_t.replace(" setConst=", " chain=3 setConst=", 1))
        d = rcs.compute(b, t)
        check("error" not in d and abs(d["ratio"]["coher_reg"] - 1.0) < 1e-9,
              "residue census: the CUM line's chain= count (the census's chain-node packets) parses")
        malformed = {
            "a lower-case key in the ops list": good_t.replace("ops=3C:", "ops=3c:"),
            "a regs entry without its dwords": good_t.replace(",0F:1/1", ",0F:1"),
            "a truncated regs list (its packets no longer add up to t0)": good_t.replace(",0F:1/1", ""),
            "an ops key out of order": good_t.replace("ops=3C:100000,54:2", "ops=54:2,3C:100000"),
            "a regs entry with fewer dwords than packets": good_t.replace(",0F:1/1", ",0F:2/1"),
            "a cut-off residue line": good_t.replace(" | hostApplied=0", ""),
            "a residue line with a garbled field": good_t.replace("lock=40000", "lock=4O000"),
            "a garbled chain= count": good_t.replace(" setConst=", " chain=x setConst=", 1),
        }
        for what, text in malformed.items():
            open(t, "w", encoding="utf-8").write(text)
            d = rcs.compute(b, t)
            r = subprocess.run([sys.executable, os.path.join(HERE, "residue_census.py"), b, t], capture_output=True)
            check(str(d.get("error", "")).startswith(("malformed", "missing")) and r.returncode == 2,
                  "residue census: %s is an error (exit 2), not zeros" % what)
        open(t, "w", encoding="utf-8").write(cum(10000, 80000, 100000, 88000))
        check("error" in rcs.compute(b, t), "residue census: a gated roll without its residue line is an error")
        # the step: a malformed census FAILs the diagnostic step (not graded as ratios)
        open(t, "w", encoding="utf-8").write(malformed["a truncated regs list (its packets no longer add up to t0)"])
        r = npf.Runner.__new__(npf.Runner)
        r.a, r.results, r.out = Args(dry_run=False), [], td
        r.path = lambda tag, suffix="": os.path.join(td, {"census0": "b.log", "census1": "t.log"}.get(tag, tag + suffix))
        r.rel = lambda p: p
        r.log = lambda msg: None
        r.write_proof = lambda: None
        v = r.do_census(steps["census"])
        check(v == "FAIL" and "malformed" in r.results[-1]["detail"],
              "residue census step: a malformed census line FAILs the diagnostic step")
        open(t, "w", encoding="utf-8").write(good_t)
        check(r.do_census(steps["census"]) == "PASS", "residue census step: a well-formed pair PASSes (no ledger checks)")

    # SR_NATIVE_SETCONSTMEM is default ON since 2026-10-14 (the setconst bake). An arm whose bound was taken with it OFF
    # says so: the setconstmem meter's base census (base_min), the segmentless meters' arms (the census sgc1 bound; its
    # record form exists only under segmentless, so it would ride in the ON arm). The bake plan grades =0 against the
    # default on one exe: the const kind to 0 and the total not rising, as two meter steps (a step grades one bound).
    def gates_of(plan, tag):
        return next(s.gates for s in plans[plan]["steps"] if s.tag == tag)
    check("SR_NATIVE_SETCONSTMEM=0" in gates_of("setconstmem", "meter0").split(","),
          "setconstmem: meter0 turns setconst off (its base census must show the SET_CONSTANTs)")
    for plan, tags in (("segmentless_bake", ("meter_off", "meter_on")), ("segmentless", ("meter0", "on"))):
        check(all("SR_NATIVE_SETCONSTMEM=0" in gates_of(plan, t).split(",") for t in tags),
              "%s: both meter arms pin setconst off (the census bound)" % plan)
    check("SR_NATIVE_SEGMENTLESS=0" in gates_of("segmentless", "meter0").split(","),
          "segmentless: meter0 is the ring path (a bare arm is segmentless since its bake)")
    # SR_NATIVE_FLUSH_SPLIT default ON since 2026-10-14 (the flush-split bake, native/flush-residue-2): the flush_split
    # plan's OFF arms pin =0, the segmentless meters' arms too (their census bound predates it), and the bake plan grades
    # =0 against the default on one exe (meter, census pair), every default roll on its snapshots
    check(all("SR_NATIVE_FLUSH_SPLIT=0" in gates_of("flush_split", t).split(",") for t in ("census0", "meter0")) and
          all("SR_NATIVE_FLUSH_SPLIT=1" in gates_of("flush_split", t).split(",") for t in ("census1", "meter1", "probe")),
          "flush_split: census0 / meter0 pin the split off, the ON arms name it (default ON since the bake)")
    for plan, tags in (("segmentless_bake", ("meter_off", "meter_on")), ("segmentless", ("meter0", "on"))):
        check(all("SR_NATIVE_FLUSH_SPLIT=0" in gates_of(plan, t).split(",") for t in tags),
              "%s: both meter arms pin the flush split off (the census bound predates its bake)" % plan)
    fsb = plans["flush_split_bake"]
    fsb_steps = {s.tag: s for s in fsb["steps"]}
    check(fsb.get("bake") is True and fsb["obligations"] == ["p1.flush_residue"] and fsb["prefix"] == "fsb" and
          fsb["gates"] == "" and
          [s.tag for s in fsb["steps"]] == ["replay", "gate", "gate_freeze_snap", "gate_span_snap", "cppm4", "cppm4_snap",
                                            "meter_off", "meter_on", "meter_on_snap", "meter", "census0", "census1",
                                            "census1_snap", "census"],
          "flush_split_bake: replay, gate + its two snapshots, CPPM4 + its snapshot, the meter pair + snapshot + step, "
          "the census pair + snapshot + tcensus, in that order")
    # the CPPM4 split on the default path (Astra round 1 on native/flush-residue-2): a freeze roll graded exact, the
    # transport's tripwires and tile, the CP's consumer counts (S1 records run, row 9's applied, render lengths), the
    # split's line, row 9's strict lines and the strict snapshot
    cp = fsb_steps["cppm4"]
    cpi = {it["id"]: it for it in cp.ledger}
    check(cp.kind == "roll" and cp.mode == "freeze" and
          sorted(cp.gates.split(",")) == ["SR_NATIVE_FLUSH_CPPM4=1", "SR_NATIVE_SEG_TILE=1"] and
          {"bake_cppm4", "bake_cppm4_seg", "bake_cppm4_tile", "bake_cppm4_xport", "bake_cppm4_split", "bake_cppm4_cp",
           "bake_cppm4_rec_cp", "bake_cppm4_resolve", "bake_cppm4_clearF", "bake_cppm4_gpr"} == set(cpi) and
          cpi["bake_cppm4"]["kind"] == "freeze_grade" and "render-len-bad=0" in cpi["bake_cppm4_rec_cp"]["pattern"] and
          "CP recs=[1-9]" in cpi["bake_cppm4_cp"]["pattern"] and
          fsb_steps["cppm4_snap"].base == "cppm4" and fsb_steps["cppm4_snap"].kind == "snapshot",
          "flush_split_bake/cppm4: the default path under CPPM4, pixels exact, consumer counts, strict row 9, snapshot")
    frs1 = ("[2026-10-14 20:28:24.655] [WARN ] [BOOT] [NB_REC] rec resolve: calls=451277 taken=451277 items=3296615 "
            "bytes=186069580 | declined open=0 pre=0: | live-stop=0 | cp applied=4716757 bad=0 ops=21217790 pred-skipped=0 "
            "no-effect=606183 render-len-bad=0 | us/call (all entries) pre=1.67 open=0.56 live=3.86 | split kickT=375 "
            "chunks=0 kickTStop=0")
    check(bool(re.search(cpi["bake_cppm4_rec_cp"]["pattern"], frs1)) and
          not re.search(cpi["bake_cppm4_rec_cp"]["pattern"], frs1.replace("render-len-bad=0", "render-len-bad=2")),
          "flush_split_bake/cppm4: the row-9 consumer item reads frs1's cppm4 line, a render-length mismatch fails it")
    check(gates_of("flush_split_bake", "gate") == "SR_NATIVE_SEG_TILE=1" and gates_of("flush_split_bake", "meter_on") == ""
          and gates_of("flush_split_bake", "meter_off") == "SR_NATIVE_FLUSH_SPLIT=0"
          and gates_of("flush_split_bake", "census1") == "SR_NATIVE_BRACKETS=1"
          and sorted(gates_of("flush_split_bake", "census0").split(",")) == ["SR_NATIVE_BRACKETS=1", "SR_NATIVE_FLUSH_SPLIT=0"],
          "flush_split_bake: the default path with no native env (the observe-only witness / census), =0 the OFF arms")
    check(fsb_steps["census"].kind == "tcensus" and fsb_steps["census"].mode == "flush" and
          (fsb_steps["census"].base, fsb_steps["census"].target) == ("census0", "census1") and
          (fsb_steps["meter"].base, fsb_steps["meter"].target) == ("meter_off", "meter_on") and
          all(fsb_steps[t].kind == "snapshot" and any(it["id"].endswith("_snapshot") for it in fsb_steps[t].ledger)
              for t in ("gate_freeze_snap", "gate_span_snap", "cppm4_snap", "meter_on_snap", "census1_snap")),
          "flush_split_bake: the census step is --flush-residue-zero over the pair, the meter =0 vs default, five snapshots")
    scb = plans["setconstmem_bake"]
    check(scb.get("bake") is True and scb["obligations"] == ["p2.setconst_mem"] and scb["prefix"] == "scb" and
          gates_of("setconstmem_bake", "meter_on") == "" and gates_of("setconstmem_bake", "meter_off") == "SR_NATIVE_SETCONSTMEM=0",
          "setconstmem_bake: meter_on runs the default, meter_off only SR_NATIVE_SETCONSTMEM=0")
    scb_steps = {s.tag: s for s in scb["steps"]}
    check([s.tag for s in scb["steps"]] == ["replay", "gate", "cppm4", "meter_off", "meter_on", "meter", "meter_pm4"],
          "setconstmem_bake: replay, gate, cppm4, the meter pair and its two steps, in that order")
    # the gate: the default path plus only the observe-only strict witness, its items as segmentless_bake's gate
    # (the tile + drained reconciliation in both of the gate's rolls, the freeze roll's records all executed) and
    # 82113010's Entries witnessed in the freeze roll
    g = scb_steps["gate"]
    groll = {it["id"]: it.get("roll") for it in g.ledger}
    check(g.kind == "gate" and g.gates == "SR_NATIVE_SEG_TILE=1" and
          groll == {"bake_gate": None, "bake_gate_tile": "freeze", "bake_gate_span_tile": "span",
                    "bake_gate_xport": "freeze", "bake_gate_calls": "freeze"} and
          all(it["pattern"] == npf.tile_drained_item("x")["pattern"] for it in g.ledger if it["id"].endswith("_tile")) and
          [it for it in g.ledger if it["id"] == "bake_gate_xport"][0]["pattern"] == npf.xport_item("x")["pattern"],
          "setconstmem_bake/gate: SR_NATIVE_SEG_TILE=1 only; the drained tile in freeze + span, xport + setconst in freeze")
    # the CPPM4 split on the default path: the CP renders kRkSetConstMem back to PM4 (the first live roll of it)
    c = scb_steps["cppm4"]
    cgates = c.gates.split(",")
    citems = {it["id"]: it for it in c.ledger}
    check(c.kind == "roll" and c.mode == "freeze" and sorted(cgates) == ["SR_NATIVE_FLUSH_CPPM4=1", "SR_NATIVE_SEG_TILE=1"] and
          not any(x.startswith(("SR_AB_TOGGLE", "SR_NATIVE_SETCONSTMEM", "SR_NATIVE_SEGMENTLESS", "SR_NATIVE_FLUSH_PM4"))
                  for x in cgates),
          "setconstmem_bake/cppm4: the default path plus the CPPM4 split and the strict witness, frozen")
    check(citems.get("bake_cppm4", {}).get("kind") == "freeze_grade" and citems["bake_cppm4"]["authority"] == "live-exact" and
          citems.get("bake_cppm4_seg", {}).get("pattern") == npf.segcounters_item("x")["pattern"] and
          citems.get("bake_cppm4_tile", {}).get("pattern") == npf.tile_item("x")["pattern"] and
          citems.get("bake_cppm4_calls", {}).get("pattern") == npf.setconst_item("x")["pattern"] and
          citems.get("bake_cppm4_rendered", {}).get("pattern") == npf.setconst_cppm4_item("x")["pattern"] and
          citems["bake_cppm4_rendered"]["authority"] == "live-exact",
          "setconstmem_bake/cppm4: freeze_exact, the seg tripwires, the tile, the setconst activity and the CP's "
          "render-back count")
    # the witnesses land on the right logs: the gate's text items on the named roll's run.log, the cppm4 items on its own
    rb = npf.Runner(Args(plan="setconstmem_bake"), scb)
    for s in scb["steps"]:
        rb.results.append({"step": s.tag, "kind": s.kind, "verdict": "DRY", "detail": "", "paths": {}})
    bpath = {it["id"]: it["path"] for _o, it in rb.items()}
    check(bpath["bake_gate"].endswith("scb_gate") and bpath["bake_gate_tile"].endswith("scb_gate/freeze/drive/attempt1/run.log") and
          bpath["bake_gate_span_tile"].endswith("scb_gate/span/drive/attempt1/run.log") and
          bpath["bake_gate_calls"].endswith("scb_gate/freeze/drive/attempt1/run.log") and
          bpath["bake_cppm4"].endswith("scb_cppm4") and bpath["bake_cppm4_calls"].endswith("scb_cppm4/drive/attempt1/run.log") and
          bpath["bake_cppm4_rendered"].endswith("scb_cppm4/drive/attempt1/run.log"),
          "setconstmem_bake: the items' paths name the gate's freeze / span run.log and the cppm4 roll")
    # the setconst activity witness: a roll whose periodic line took Entries passes, one with setConstMem=0 does not
    sc = npf.setconst_item("x")
    line = ("[t] [INFO ] [BOOT] [NATIVE_FLUSH] periodic draws: taken=262144 | beginV=1 allocFail=0 | tileRects=5 tiled=5 decl=0"
            " | setConstMem=%d decl=0 fail=0 | seg closed=1 parts=2")
    check(re.search(sc["select"], line % 90120) and re.search(sc["pattern"], line % 90120) and
          not re.search(sc["pattern"], line % 0), "setconst_item: setConstMem > 0 passes, 0 fails")
    # ...and on the periodic line since the CP's case, which adds cpPm4= (the consumer's count of setconst records it
    # rendered back to PM4): the activity item reads both forms; the render-back item needs cpPm4 > 0 - a CPPM4 roll
    # whose setconst records all took the opaque fallback (cpPm4=0), or an exe without the counter, fails it
    rc = npf.setconst_cppm4_item("x")
    line2 = line.replace("fail=0 |", "fail=0 cpPm4=%d |")
    check(re.search(sc["pattern"], line2 % (90120, 0)) and re.search(sc["pattern"], line2 % (90120, 88000)),
          "setconst_item: matches the line with and without cpPm4=")
    check(re.search(rc["select"], line2 % (90120, 88000)) and re.search(rc["pattern"], line2 % (90120, 88000)) and
          not re.search(rc["pattern"], line2 % (90120, 0)) and not re.search(rc["pattern"], line % 90120) and
          not re.search(rc["pattern"], line2 % (0, 88000)),
          "setconst_cppm4_item: cpPm4 > 0 passes; cpPm4=0, no cpPm4 field, or no setconst calls fail")
    sitems = {it["id"]: it for s in scb["steps"] for it in s.ledger}
    check(sitems.get("bake_meter", {}).get("field") == "kind_const" and sitems["bake_meter"].get("base_min") == 5.0 and
          sitems["bake_meter"].get("max") == 0.1 and "max_delta" not in sitems["bake_meter"] and
          sitems.get("bake_meter_pm4", {}).get("field") == "pm4" and sitems["bake_meter_pm4"].get("max_delta") == 0.0,
          "setconstmem_bake: the const kind item (base >= 5.0, target <= 0.1) and the total pm4 item (delta <= 0)")
    # the wiring: do_meter grades each step's own item by the ledger's check on the unrounded per-frame values (Astra
    # round 1 on native/zpd-bake, P2-4); meter_delta.py's one-decimal transcript is information - here one claiming
    # "+0.0" whatever the rolls hold
    os.makedirs(os.path.join(npf.LANE, "archive"), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        runner = npf.Runner(Args(plan="setconstmem_bake", out=td, dry_run=False), scb)
        steps = {s.tag: s for s in scb["steps"]}
        transcript = ("A x: ingame windows=1 frames=1000 | per_frame pm4=0.0\nB y: ingame windows=1 frames=1000 | "
                      "per_frame pm4=0.0\nB - A per_frame: pm4=+0.0\nby kind (A -> B per_frame): const=0.0->0.0\n")
        for pm4_on, k_on, want_kind, want_pm4, why in ((84.6, 0.0, "PASS", "PASS", "the scm4 numbers"),
                                                       (101.0, 0.0, "PASS", "FAIL", "the total rose"),
                                                       (84.6, 2.0, "FAIL", "PASS", "the default still holds SET_CONSTANTs"),
                                                       (100.04, 0.0, "PASS", "FAIL",
                                                        "the total rose by 0.04 a frame (printed +0.0)"),
                                                       (84.6, 0.104, "FAIL", "PASS",
                                                        "const 0.104 a frame > max 0.1 (printed 0.1)"),
                                                       (84.6, 0.1, "PASS", "PASS", "const exactly at max 0.1")):
            for tag, pm4, kc in (("meter_off", 100.0, 6.9), ("meter_on", pm4_on, k_on)):
                shutil.rmtree(runner.path(tag), ignore_errors=True)
                meter_roll(runner.path(tag), {"pm4": pm4, "nrec": 3000.0}, kinds={"const": kc, "sync": 18.0})
            with meter_fixture_world(), \
                    patch.object(npf.subprocess, "run",
                                 lambda *a, **kw: subprocess.CompletedProcess(a, 0, stdout=transcript, stderr="")):
                got = (runner.do_meter(steps["meter"]), runner.do_meter(steps["meter_pm4"]))
            check(got == (want_kind, want_pm4), "setconstmem_bake meter wiring, %s: kind %s / total %s (got %s; %s)" % (
                why, want_kind, want_pm4, got, runner.results[-1]["detail"][:90]))

    construns_bake_checks(plans, gates_of)
    transport_bake_checks(plans)
    display_registers_bake_checks(plans)

    # Historical bake plans (Astra round 1 on native/setconst-bake): a plan whose meter base is an archived roll
    # ("<prefix>:<tag>" or "<dir>/<roll>") is pinned to the exe it ran on; main() refuses a live run on any other exe
    # (a later default path carries every bake since, so the bound would measure those).
    for name, p in plans.items():
        if npf.archived_bases(p):
            check(re.fullmatch(r"[0-9a-f]{12}", p.get("exe_pin") or "") is not None,
                  "%s: its meter base %s is an archived roll, so the plan is pinned to an exe" % (name, npf.archived_bases(p)))
    check(plans["row3_bake"].get("exe_pin") == "47e80a6f28a0" and plans["rows53b_bake"].get("exe_pin") == "87b0592ada63",
          "row3_bake / rows53b_bake: pinned to the exes they ran on (the r3e / r5e proof documents)")
    check(not plans["setconstmem_bake"].get("exe_pin") and not npf.archived_bases(plans["setconstmem_bake"]) and
          not plans["segmentless_bake"].get("exe_pin") and not npf.archived_bases(plans["segmentless_bake"]),
          "same-exe bake meters (setconstmem_bake, segmentless_bake) carry no pin")
    # (under the lane's archive: the dry run's ledger items are paths relative to the umbrella, so the same drive)
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        other = os.path.join(td, "SupermanReturns.exe")
        with open(other, "wb") as f:
            f.write(b"not the pinned exe")
        for name in ("row3_bake", "rows53b_bake"):
            try:
                npf.main(["--plan", name, "--exe", other, "--repo", td, "--out", td, "--slot-held"])
                check(False, "%s: a live run on another exe is refused" % name)
            except SystemExit as e:
                msg = str(e.code)
                check(msg.startswith("REFUSED: ") and plans[name]["exe_pin"] in msg and npf.exe_id(other) in msg,
                      "%s: a live run on another exe is refused, naming the pin and the exe (%s...)" % (name, msg[:70]))
        try:
            npf.main(["--plan", "row3_bake", "--repo", td, "--out", td, "--slot-held"])
            code = 0
        except SystemExit as e:
            code = e.code
        check(str(code).startswith("REFUSED: ") and "is missing" in str(code), "row3_bake: a live run with no --exe is refused")
        # the pinned exe passes the pin (and then stops at the next check: this temp repo has no disc beside it)
        with patch.object(npf, "exe_id", lambda _p: plans["row3_bake"]["exe_pin"]):
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    npf.main(["--plan", "row3_bake", "--exe", other, "--repo", td, "--out", td, "--slot-held"])
                code = 0
            except SystemExit as e:
                code = e.code
        check(code == 2, "row3_bake: the pinned exe passes the pin check (argparse then refuses the disc-less repo)")
        # a dry run on another exe runs nothing live and only notes the pin
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = npf.main(["--plan", "row3_bake", "--dry-run", "--exe", other, "--out", td])
        check(rc == 0 and "NOTE: plan row3_bake is pinned to exe 47e80a6f28a0" in buf.getvalue(),
              "row3_bake: a dry run on another exe runs, the pin noted")
        check(npf.pin_refusal(plans["setconstmem_bake"], "setconstmem_bake", other) == "",
              "an unpinned plan is never refused")

    # the abort scan and the play check
    with tempfile.TemporaryDirectory() as td:
        log = os.path.join(td, "run.log")
        open(log, "w", encoding="utf-8").write("[t] [INFO ] [BOOT] [GPU_CP] census rptr=1\n[t] [WARN ] [BOOT] [CP_WAIT] stalled 2.0s: mem 0x1EF2C002\n")
        why, line = npf.scan_abort(log)
        check(why == "CP stall" and "stalled" in line, "a CP stall aborts")
        open(log, "w", encoding="utf-8").write("[t] [ERROR] [BOOT] [GPU_HANG] ==== trigger H1 | episode\n")
        check(npf.scan_abort(log)[0] == "GPU hang trigger", "a hang trigger aborts")
        open(log, "w", encoding="utf-8").write("[t] [INFO ] [BOOT] [GPU_CP] census\n[t] [INFO ] [BOOT] [NATIVE_FLUSH] exit\n")
        check(npf.scan_abort(log) == (None, None), "a clean log does not abort")

        # The narrow dump-stall void rule finds an episode's "(e) CP phase" line BY TIME (sgd7 span, 2026-10-11: the hang
        # dump froze the process and the line came 27 s after its trigger, 20+ lines later).
        def stall_log(delay_s, filler):
            t = lambda s: "[2026-10-11 11:%02d:%06.3f]" % (37 + int(s // 60), s % 60)
            out = [t(46.0) + " [INFO ] [BOOT] [PRESENT_DUMP] D:/x/present_000.bmp",
                   t(47.8) + " [ERROR] [BOOT] [GPU_HANG] ==== trigger H1 | episode key=00032EDB relog=0 | now=130.769s "
                             "completed=00032EDB flat 3.0s | presents=33873 flat 3.0s (VdSwap #16937 XE_SWAP #16936) | last IB"]
            out += [t(47.9) + " [ERROR] [BOOT] [GPU_HANG] (f) filler %d" % k for k in range(filler)]
            out += [t(47.8 + delay_s) + " [ERROR] [BOOT] [GPU_HANG] (e) CP phase=swap for 3.05s rptr=1 wptr=2",
                    t(47.9 + delay_s) + " [ERROR] [BOOT] [GPU_HANG] ==== end",
                    t(50.0 + delay_s) + " [INFO ] [BOOT] [PERF] t=200s fps=50 (swaps=17000 x)"]
            return "\n".join(out) + "\n"
        open(log, "w", encoding="utf-8").write(stall_log(27.0, 20))
        v, ev = npf.dump_stall_void(log)
        check(v, "dump-stall void: the (e) CP phase line 27 s and 20 lines after its trigger is found by time (%s)" % (ev[:1],))
        open(log, "w", encoding="utf-8").write(stall_log(45.0, 2))
        v, ev = npf.dump_stall_void(log)
        check(not v and "phase None" in ev[0], "dump-stall void: an (e) line past %.0f s is not this episode's (%s)" % (
            npf.PHASE_WITHIN_S, ev[:1]))

        # sgd7 span episode 2 (2026-10-11): the last dump completed 54 ms BEFORE trigger - flat, the BMP write in flight
        # when the stall began logged only after the hang dump's own 27 s freeze ("==== end" 11:38:45.883, the dump
        # 11:38:45.994) - inside (b)'s window, which ends at max(trigger + 10 s, "==== end" + 10 s).
        def freeze_log(post_end_s):
            t = lambda s: "[2026-10-11 11:%02d:%06.3f]" % (38 + int(s // 60), s % 60)
            out = [t(15.604) + " [INFO ] [BOOT] [PRESENT_DUMP] D:/x/present_115.bmp",
                   t(18.658) + " [ERROR] [BOOT] [GPU_HANG] ==== trigger H1 | episode key=00033C8D relog=0 | now=161.598s "
                               "completed=00033C8D flat 3.0s | presents=34263 flat 3.0s (VdSwap #17132 XE_SWAP #17131) | last IB",
                   t(26.890) + " [ERROR] [BOOT] [GPU_HANG] (b) wait BlockOnFence tid=1",
                   t(45.882) + " [ERROR] [BOOT] [GPU_HANG] (e) CP phase=swap for 3.05s rptr=8032 wptr=8053",
                   t(45.883) + " [ERROR] [BOOT] [GPU_HANG] ==== end",
                   t(45.883 + post_end_s) + " [INFO ] [BOOT] [PRESENT_DUMP] D:/x/present_116.bmp",
                   t(56.030) + " [INFO ] [BOOT] [PERF] t=203s swaps/s=120 (swaps=17500 x)"]
            return "\n".join(out) + "\n"
        open(log, "w", encoding="utf-8").write(freeze_log(0.111))
        v, ev = npf.dump_stall_void(log)
        check(v, "dump-stall void: the in-flight write logged 0.1 s after the hang dump's own freeze ends is in (b)'s window (%s)" % (
            ev[1:2],))
        open(log, "w", encoding="utf-8").write(freeze_log(11.0))
        v, ev = npf.dump_stall_void(log)
        check(not v and "no [PRESENT_DUMP]" in ev[0],
              "dump-stall void: a dump 11 s after the freeze ends is outside the window - no widening beyond the freeze")
        check(npf.scan_abort(os.path.join(td, "missing.log"))[0] == "no run.log", "a missing log aborts")
        roll = os.path.join(td, "roll")
        os.makedirs(roll)
        json.dump({"eras": [{"era": "boot"}, {"era": "intro"}]}, open(os.path.join(roll, "report.json"), "w"))
        check(npf.reached_play(roll)[0] is False, "a roll without an ingame era did not reach play")
        json.dump({"eras": [{"era": "boot"}, {"era": "ingame"}]}, open(os.path.join(roll, "report.json"), "w"))
        check(npf.reached_play(roll)[0] is True, "an ingame era = play reached")
        # p2.zpd zpd1 (2026-10-14): a save-started roll (Superman in the air: PC_CONTROLS `mode -`, no ingame marker)
        json.dump({"eras": [{"era": "boot"}, {"era": "title"}, {"era": "gameplay"}]},
                  open(os.path.join(roll, "report.json"), "w"))
        check(npf.reached_play(roll)[0] is False, "a NEW GAME roll with the HUD but no ingame era did not reach play")
        open(os.path.join(roll, "save_start.json"), "w").write("{}")
        check(npf.reached_play(roll)[0] is True, "a save-started roll's gameplay era (the HUD) = play reached")
        json.dump({"eras": [{"era": "boot"}, {"era": "title"}]}, open(os.path.join(roll, "report.json"), "w"))
        check(npf.reached_play(roll)[0] is False, "a save-started roll that never reached the HUD did not reach play")
        # a lane's tape that the --repo checkout lacks resolves to this checkout's copy (zpd1: harness_play exited on
        # the missing file); a tape both have is the repo's
        with tempfile.TemporaryDirectory() as repo:
            check(npf.tape_path(repo, "tools/perf/zpd_play.txt") == os.path.join(npf.LANE, "tools/perf/zpd_play.txt"),
                  "a tape missing from --repo is read from this checkout")
            os.makedirs(os.path.join(repo, "tools", "perf"))
            open(os.path.join(repo, "tools", "perf", "zpd_play.txt"), "w").write("wait 1\n")
            check(npf.tape_path(repo, "tools/perf/zpd_play.txt") == os.path.join(repo, "tools/perf/zpd_play.txt"),
                  "a tape the --repo checkout has is read from there")

    # the items: paths relative to the umbrella root, one per obligation
    with tempfile.TemporaryDirectory(dir=npf.LANE) as td:
        runner = npf.Runner(Args(plan="row3", out=td, dry_run=False), plans["row3"])
        step = next(s for s in plans["row3"]["steps"] if s.kind == "gate")
        def fake_gate(*args, **kw):
            kw["stdout"].write("FLICKER_GATE PASS\n")
            return 1
        with patch.object(npf.subprocess, "call", fake_gate):
            check(runner.do_gate(step) == "FAIL", "a nonzero gate exit fails even if its last line says PASS")

    r = npf.Runner(Args(plan="rows53b"), plans["rows53b"])
    for s in plans["rows53b"]["steps"]:
        r.results.append({"step": s.tag, "kind": s.kind, "verdict": "DRY", "detail": "", "paths": {}})
    items = r.items()
    check(all(not it["path"].startswith("/") and ":" not in it["path"] and "\\" not in it["path"] for _o, it in items),
          "ledger paths are umbrella-relative with forward slashes")
    # The cloud worker keeps the same umbrella-relative contract at wt/<lane>.
    # Require this exact checkout prefix, including its lane, in either layout.
    lane_prefix = os.path.relpath(npf.LANE, npf.UMBRELLA).replace(os.sep, "/") + "/"
    check(all(it["path"].startswith(lane_prefix) for _o, it in items),
          "ledger paths start at this worktree under its umbrella")
    oids = {o for o, _it in items}
    check(oids == {"p1.row5", "p1.row3b"}, "rows53b records into both row 5 and row 3b")
    chk = [it for _o, it in items if it["id"] == "chk"][0]
    check(chk["path"].endswith("r5b_chk/drive/attempt1/run.log"), "the CHECK item points at the roll's run.log")
    meter = [it for _o, it in items if it["id"] == "meter"][0]
    check(meter["path"].endswith("r5b_meter1") and meter["base"].endswith("r5b_meter0"), "the meter item: gated roll vs the default roll")

    # the ledger editor on a temp copy of the real ledger
    src, root, text = ledger_fixture()
    if not src:
        check(False, "a ledger fixture exists (docs/native_ledger.json at %s here or in the dev checkout)" % FIXTURE_REV)
        return FAILS
    with tempfile.TemporaryDirectory() as td:
        path = os.path.join(td, "native_ledger.json")
        open(path, "w", encoding="utf-8", newline="").write(text)
        before = open(path, encoding="utf-8").read().split("\n")
        lt = npf.LedgerText(open(path, encoding="utf-8").read())
        it = npf.freeze_ab("abp", "test")
        it["path"] = "_worktrees/native/archive/v1_native/r467b_abp"
        action, old = lt.upsert_evidence("p1.rows467", it, superseded_fails={"abp"})
        check(action == "moved" and old and old["id"] == "abp", "rows467 abp: the failing item is superseded (moved)")
        doc = json.loads(lt.text())
        o = [x for x in doc["obligations"] if x["id"] == "p1.rows467"][0]
        ev = [e for e in o["evidence"] if e["id"] == "abp"]
        check(len(ev) == 1 and ev[0]["path"] == it["path"], "rows467 abp now cites the new roll")
        w = o["contradictions"][0]["witness"]
        check(any(x["id"] == "abp_black.abp_void" and x.get("expect") == "fail" and x["path"] == old["path"] for x in w),
              "the void p1d_abp sits in abp_black's witness list with expect fail")
        # A second supersession of the same step (a re-run after a void) gets a unique witness id.
        it3 = npf.freeze_ab("abp", "test")
        it3["path"] = "_worktrees/native/archive/v1_native/r467c_abp"
        action3, old3 = lt.upsert_evidence("p1.rows467", it3, superseded_fails={"abp"})
        w3 = [x for x in json.loads(lt.text())["obligations"] if x["id"] == "p1.rows467"][0]["contradictions"][0]["witness"]
        ids3 = [x["id"] for x in w3]
        check(action3 == "moved" and old3["path"] == it["path"] and ids3.count("abp_black.abp_void") == 1 and
              "abp_black.abp_void2" in ids3 and len(ids3) == len(set(ids3)),
              "a second supersession of abp gets the witness id abp_black.abp_void2 (no duplicate ids)")
        a2 = npf.freeze_ab("abps", "test")
        a2["path"] = "_worktrees/native/archive/v1_native/s1b_abps"
        a1 = npf.freeze_ab("abp", "test")
        a1["path"] = "_worktrees/native/archive/v1_native/s1b_abp"
        check(lt.upsert_evidence("p1.s1_flush", a1)[0] == "inserted" and lt.upsert_evidence("p1.s1_flush", a2)[0] == "inserted",
              "s1_flush: abp + abps inserted")
        check(lt.resolve_unknown("p1.s1_flush", "no_frozen_ab", [a1, a2]) == "resolved", "s1_flush: no_frozen_ab resolved")
        check(lt.resolve_unknown("p1.s1_flush", "no_frozen_ab", [a1]) == "kept", "a resolved unknown is not resolved twice")
        s_it = npf.freeze_ab("abp", "test")
        s_it["path"] = "_worktrees/native/archive/v1_native/segb_abp"
        action, old = lt.upsert_evidence("p2.segmentless", s_it, superseded_fails={"abp"})
        doc = json.loads(lt.text())
        seg = [x for x in doc["obligations"] if x["id"] == "p2.segmentless"][0]
        check(action == "moved" and seg["contradictions"][0]["id"] == "superseded_black", "segmentless: a contradiction was created for the void roll")
        check(seg["contradictions"][0]["resolved_by"][0]["commit"] == npf.RACE_FIX, "...resolved by the race fix on HEAD")
        check([e["id"] for e in seg["evidence"]].count("abp") == 1, "segmentless: one abp item")
        s1 = [x for x in doc["obligations"] if x["id"] == "p1.s1_flush"][0]
        ru = [u for u in s1["residual_unknowns"] if u["id"] == "no_frozen_ab"][0]
        check([x["id"] for x in ru["resolved_by"]] == ["no_frozen_ab.abp", "no_frozen_ab.abps"], "the unknown's resolved_by names both items")
        open(path, "w", encoding="utf-8", newline="").write(lt.text())
        errs = schema_errors(root, path)
        check(not errs, "the ledger tool's schema check passes: %s" % (errs[:2] if errs else "clean"))
        after = open(path, encoding="utf-8").read().split("\n")
        # the untouched lines are byte-identical: exactly the superseded abp lines (rows467, segmentless), the
        # resolved unknown, and the two trailing commas the insertions need (s1_flush's last evidence item, the
        # abp_black witness before the new void one) changed
        after_set = set(after)
        changed = [ln for ln in before if ln not in after_set]
        marks = ['"id": "abp"', '"id": "abp"', '"id": "no_frozen_ab"', '"id": "meter"', '"id": "abp_black.p1d_abp"']
        check(len(changed) == 5 and all(any(m in ln for ln in changed) for m in marks),
              "exactly the 5 expected original lines changed (%d: %s)" % (len(changed), [ln.strip()[:40] for ln in changed]))
        check(all(ln.startswith("        {\"id\": \"") or not ln.lstrip().startswith("{\"id\": \"abp") for ln in after
                  if "s1b_abp" in ln), "inserted evidence items sit at the file's item indent")
        # a second apply of the same items is idempotent in shape: replace in place, no second witness
        lt2 = npf.LedgerText(open(path, encoding="utf-8").read())
        action, _ = lt2.upsert_evidence("p1.rows467", it, superseded_fails=set())
        check(action == "replaced", "re-applying a passing item replaces it in place")
        d2 = json.loads(lt2.text())
        o2 = [x for x in d2["obligations"] if x["id"] == "p1.rows467"][0]
        check(sum(1 for x in o2["contradictions"][0]["witness"] if x["id"].endswith("abp_void")) == 1, "no duplicate void witness")
        # a terminal twin leads with its id; one written before 2026-10-15 (the id after the predicate keys) is still
        # found and replaced, not inserted a second time (tfx2 + tfx3's twins: 8 duplicate ids in p2.transport)
        tw = npf.terminal_twin({"id": "tw_src", "kind": "text", "select": r"\[NATIVE_FLUSH\] seg closed=", "which": "last",
                                "pattern": "x"}, "tw_src.terminal")
        check(tw is not None and next(iter(tw)) == "id", "a terminal twin's first key is its id")
        late = {k: v for k, v in tw.items() if k != "id"}
        late.update(id=tw["id"], path="_worktrees/native/archive/v1_native/twa/run.log")
        check(lt2.upsert_evidence("p1.rows467", late)[0] == "inserted", "an id-last twin is inserted")
        check(lt2.upsert_evidence("p1.rows467", dict(tw, path="_worktrees/native/archive/v1_native/twb/run.log"))[0] == "replaced",
              "a later apply of the same twin finds the id-last line and replaces it")
        ev2 = [e for x in json.loads(lt2.text())["obligations"] if x["id"] == "p1.rows467" for e in x["evidence"]]
        check([e["id"] for e in ev2].count("tw_src.terminal") == 1 and
              [e["path"] for e in ev2 if e["id"] == "tw_src.terminal"] == ["_worktrees/native/archive/v1_native/twb/run.log"],
              "one tw_src.terminal item, citing the later roll")

    # the ledger tool's JSON: `results` is a list of obligations (the shape that crashed the first chain's apply)
    doc = {"results": [{"id": "p1.s1_flush", "evidence": [{"id": "abp", "result": "pass"}, {"id": "abps", "result": "fail"}]},
                       {"id": "p1.rows467", "evidence": [{"id": "abp", "result": "fail"}]}]}
    idx = npf.results_index(doc)
    check(idx == {("p1.s1_flush", "abp"): "pass", ("p1.s1_flush", "abps"): "fail", ("p1.rows467", "abp"): "fail"},
          "results_index reads the list-shaped results")
    check(npf.results_index({"results": {"x": {"evidence": [{"id": "e", "result": "pass"}]}}}) == {(None, "e"): "pass"} and
          npf.results_index({}) == {} and npf.results_index([]) == {}, "results_index tolerates a dict, an empty and a wrong document")
    # transport_composed (2026-10-14, the orchestrator's decisions on the gap map): the composed env is every transport
    # kind + SR_NATIVE_DEVCREATE=1 with SR_NATIVE_SHADER_CONST_RUNS=0; each pair the plan grades (census, meter,
    # pacing) differs from its all-OFF arm in the transport kinds only; the tcensus / pacing steps read earlier steps
    # and grade their tools' JSON (a passing pair PASSes, a transport producer left / a doubled gate FAILs).
    tcp = plans["transport_composed"]
    tcs = {s.tag: s for s in tcp["steps"]}
    kinds = ["SR_NATIVE_CALLBACK", "SR_NATIVE_GATE", "SR_NATIVE_SWAP_BLOCK", "SR_NATIVE_SWAP", "SR_NATIVE_BRACKET",
             "SR_NATIVE_IDLE_WAIT", "SR_NATIVE_INVAL_RANGE", "SR_NATIVE_LOCK_COHER", "SR_NATIVE_TRANSPORT",
             "SR_NATIVE_DEVCREATE"]

    def env_of(gates):
        e = {}
        for kv in [p for p in gates.split(",") if "=" in p]:
            k, v = kv.split("=", 1)
            e[k.strip()] = v.strip()  # the last wins, as perf_run applies them
        return e
    on_tags = ["probe", "abp", "cppm4", "stress", "census1", "meter1", "pace1", "gate"]
    off_tags = ["stress_off", "census0", "meter0", "pace0", "pace0b"]
    check(all(all(env_of(tcs[t].gates).get(k) == "1" for k in kinds) and
              env_of(tcs[t].gates).get("SR_NATIVE_SHADER_CONST_RUNS") == "0" for t in on_tags),
          "transport_composed: every composed roll runs every kind + devcreate, shader-const runs off")
    check(all(all(env_of(tcs[t].gates).get(k) == "0" for k in kinds) for t in off_tags),
          "transport_composed: every control roll runs every kind off")
    for a, b in (("census0", "census1"), ("meter0", "meter1"), ("pace0", "pace1"), ("stress_off", "stress")):
        ea, eb = env_of(tcs[a].gates), env_of(tcs[b].gates)
        diff = sorted(k for k in set(ea) | set(eb) if ea.get(k) != eb.get(k))
        check(diff == sorted(kinds) and tcs[a].mode == tcs[b].mode,
              "transport_composed: %s / %s differ in the transport kinds only (%s)" % (a, b, diff))
    check(env_of(tcs["pace0"].gates).get("SR_HIGHFPS") == "0" and env_of(tcs["pace1"].gates).get("SR_HIGHFPS") == "0"
          and tcs["pace0"].trace and tcs["pace1"].trace,
          "transport_composed: the pacing pair runs the console path (SR_HIGHFPS=0) with the P2 trace")
    check(env_of(tcs["census0"].gates).get("SR_NATIVE_BRACKETS") == "1" == env_of(tcs["census1"].gates).get("SR_NATIVE_BRACKETS"),
          "transport_composed: both census rolls carry the producer census")
    toggled = set(env_of(tcs["abp"].gates)["SR_AB_TOGGLE"].split("+"))
    check(toggled == set(kinds) - {"SR_NATIVE_SWAP_BLOCK"},
          "transport_composed: the A/B toggles every kind but the VdSwap block (a kernel call, no per-present arm)")
    idx = {s.tag: i for i, s in enumerate(tcp["steps"])}
    check(tcs["census"].kind == "tcensus" and (tcs["census"].base, tcs["census"].target) == ("census0", "census1") and
          idx["census0"] < idx["census"] and idx["census1"] < idx["census"],
          "transport_composed: the tcensus step reads the two earlier census rolls")
    check(tcs["pacing"].kind == "pacing" and (tcs["pacing"].base, tcs["pacing"].target) == ("pace0_trace", "pace1_trace")
          and idx["pace0_trace"] < idx["pacing"] and idx["pace1_trace"] < idx["pacing"] and
          tcs["pace0_trace"].base == "pace0" and tcs["pace1_trace"].base == "pace1",
          "transport_composed: the pacing step reads the two earlier trace summaries (control first)")
    # Astra round 2: the calibration control - the same config as the first control, rolled after the composed roll
    # (the composed roll between the two controls), its trace read by the pacing step as the calibration
    check(tcs["pacing"].calib == "pace0b_trace" and tcs["pace0b_trace"].base == "pace0b" and
          tcs["pace0b"].gates == tcs["pace0"].gates and tcs["pace0b"].mode == tcs["pace0"].mode and tcs["pace0b"].trace
          and idx["pace0"] < idx["pace1"] < idx["pace0b"] < idx["pacing"] and idx["pace0b_trace"] < idx["pacing"],
          "transport_composed: a second identical all-OFF pacing control (after the composed roll) calibrates the step")
    check(any(c["field"] == "calibration.verdict" and c["value"] == "PASS" for c in tcs["pacing"].ledger[0]["checks"]),
          "transport_composed: the pacing item requires the control-vs-control calibration to pass")
    # Astra round 3: the pacing window is sized for the margins (pacing_cmp.PLAN_WINDOW) and the rolls hold it
    import pacing_cmp as pcm  # noqa: E402
    check(npf.PACE_WINDOW == pcm.PLAN_WINDOW and
          all(tcs[t].window == npf.PACE_WINDOW for t in ("pace0_trace", "pace1_trace", "pace0b_trace")) and
          all((tcs[t].play, tcs[t].dwell, tcs[t].seconds) == (npf.PACE_LONG["play"], npf.PACE_LONG["dwell"],
                                                              npf.PACE_LONG["seconds"]) for t in ("pace0", "pace1", "pace0b"))
          and npf.PACE_LONG["dwell"] * 24 >= npf.PACE_WINDOW,
          "transport_composed: the pacing rolls are long enough for the plan's window, the same length every arm")
    ids = {it["id"] for s in tcp["steps"] for it in s.ledger}
    check(all(e in ids for _o, _u, es in tcp["resolves"] for e in es) and
          {u for _o, u, _e in tcp["resolves"]} == {"composed_zero", "pacing"},
          "transport_composed: it resolves composed_zero and pacing by its own items")
    sys.path.insert(0, HERE)
    import test_transport_census as ttc  # noqa: E402
    import test_pacing as tpa  # noqa: E402
    os.makedirs(os.path.join(npf.LANE, "archive"), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        r = npf.Runner.__new__(npf.Runner)
        r.a, r.results, r.out, r.prefix = Args(dry_run=False), [], td, "tcmp"
        r.rel = lambda p: p
        r.log = lambda msg: None
        r.write_proof = lambda: None

        def census_roll(tag, per_frame):
            d = os.path.join(td, "tcmp_" + tag, "drive", "attempt1")
            os.makedirs(d, exist_ok=True)
            ttc.write_log(os.path.join(d, "run.log"), 60, per_frame)
        census_roll("census0", ttc.BASE)
        census_roll("census1", ttc.TARGET)
        v = r.do_tcensus(tcs["census"])
        check(v == "PASS", "transport_composed tcensus: the transport producers gone, the flush residue left: PASS (%s)"
              % r.results[-1]["detail"])

        def run_unique(kind_stem, verdict):
            """the step's row names a run-unique JSON (<tag><stem>.<UTC stamp>.json) holding its stamped verdict,
            committed last, and the item's own checks grade it so (the owner / census discipline, merged from dev)"""
            js = r.results[-1]["paths"]["json"]
            d = json.load(open(js, encoding="utf-8"))
            step = tcs["census"] if kind_stem == "_tcensus" else tcs["pacing"]
            bad = npf.json_checks(d, step.ledger[0]["checks"])
            return (re.search(re.escape(kind_stem) + r"\.\d{8}T\d{6}\.\d+Z(-\d+)?\.json$", js) is not None and
                    d.get("committed") is True and (d.get("runner") or {}).get("verdict") == verdict and
                    (not bad if verdict == "PASS" else bool(bad))), js
        ok1, js1 = run_unique("_tcensus", "PASS")
        check(ok1, "transport_composed tcensus: a run-unique JSON, verdict stamped, committed; its item grades PASS")
        census_roll("census1", dict(ttc.TARGET, **{"82111EC8": {"54": 1, "r05": 5}}))
        v = r.do_tcensus(tcs["census"])
        check(v == "FAIL" and "target_transport_zero" in r.results[-1]["detail"],
              "transport_composed tcensus: the vblank gate left in the target FAILs")
        ok2, js2 = run_unique("_tcensus", "FAIL")
        check(ok2 and js2 != js1, "transport_composed tcensus: the FAIL is its own run-unique JSON and its item grades FAIL")
        census_roll("census1", dict(ttc.TARGET, **{"8CAFE000": {"54": 1}}))
        check(r.do_tcensus(tcs["census"]) == "FAIL", "transport_composed tcensus: a producer the map does not know FAILs")
        # native/composed-census: tcmp2's census1 - the counter ran, the CP presented, no frame ever ended (every
        # present a kRkSwap record) - is refused as DARK, never graded as the transport zero its silence resembles
        c1log = os.path.join(td, "tcmp_census1", "drive", "attempt1", "run.log")
        ttc.write_log(c1log, 60, ttc.ZERO, dark_from=1)
        v = r.do_tcensus(tcs["census"])
        check(v == "FAIL" and "DARK_CENSUS" in r.results[-1]["detail"] and "coverage DARK" in r.results[-1]["detail"],
              "transport_composed tcensus: a dark census1 (armed, the CP presenting, no F line) FAILs as DARK_CENSUS "
              "(%s)" % r.results[-1]["detail"][:160])
        ttc.write_log(c1log, 60, ttc.ZERO, dark_from=1, armed=False)
        v = r.do_tcensus(tcs["census"])
        check(v == "FAIL" and "NO_CENSUS" in r.results[-1]["detail"] and "coverage OFF" in r.results[-1]["detail"],
              "transport_composed tcensus: a census1 with no witness and no line of the counter's (off, or dark on an "
              "exe without the witness) FAILs as NO_CENSUS")
        # Astra round 1 (P2-1 / P2-2): frame ends that are not presents, and a final report after the last CP sample
        ttc.write_log(c1log, 60, ttc.TARGET, record_swaps=True, unsigned_at=(5,), cp_every=1000)
        check(r.do_tcensus(tcs["census"]) == "FAIL" and "UNACCOUNTED" in r.results[-1]["detail"],
              "transport_composed tcensus: an unsigned XE_SWAP among record presents FAILs (the ENDS accounting)")
        ttc.write_log(c1log, 60, ttc.TARGET, record_swaps=True, unsigned_at=(15,))
        check(r.do_tcensus(tcs["census"]) == "FAIL" and "SURPLUS" in r.results[-1]["detail"],
              "transport_composed tcensus: a frame end with no present FAILs at the next CP sample (exact, not >=)")
        ttc.write_log(c1log, 60, ttc.ZERO, dark_from=30, record_swaps=True)
        check(r.do_tcensus(tcs["census"]) == "FAIL" and "DARK_CENSUS" in r.results[-1]["detail"],
              "transport_composed tcensus: a census1 going dark mid-run FAILs as DARK_CENSUS")
        ttc.write_log(c1log, 60, ttc.TARGET, record_swaps=True)
        check(r.do_tcensus(tcs["census"]) == "PASS",
              "transport_composed tcensus: the fixed census1 (every frame ended at a present record) PASSes")


        # native/composed-census: the census rolls' own coverage witness items (armed; ENDS by the env's present path)
        def grade_census(item, text):
            with tempfile.TemporaryDirectory() as tdg:
                open(os.path.join(tdg, "run.log"), "w", encoding="utf-8").write(text)
                ctx = nl.Ctx({"roots": {"umbrella": tdg}}, npf.LANE)
                return nl.check_item(ctx, dict(item, path="run.log"))[0]
        c0i = {it["id"]: it for it in tcs["census0"].ledger}
        c1i = {it["id"]: it for it in tcs["census1"].ledger}
        armed_ln = ("[t] [INFO ] [BOOT] [PM4_COV] armed brackets=1 SR_PM4_COVERAGE=unset SR_NATIVE_BRACKETS=1 "
                    "SR_NATIVE_RECORD=unset SR_NATIVE_DRAW=unset | frames end at an XE_SWAP packet or at a present\n")

        def ends_ln(xe, rec):
            return "[t] [INFO ] [BOOT] [PM4_COV] ENDS periodic frames=%d xeSwap=%d present=%d presents=%d\n" % (
                xe + rec, xe, rec, xe + rec)
        for cid, items in (("census0", c0i), ("census1", c1i)):
            a = items["composed_%s_armed" % cid]
            check(grade_census(a, armed_ln) == nl.PASS and grade_census(a, "") == nl.FAIL and
                  grade_census(a, armed_ln.replace("brackets=1", "brackets=0")) == nl.FAIL,
                  "transport_composed %s: the armed witness passes with the producer census on, fails absent / "
                  "brackets off" % cid)
        e1, e0 = c1i["composed_census1_ends"], c0i["composed_census0_ends"]
        check(grade_census(e1, ends_ln(0, 60)) == nl.PASS and grade_census(e1, ends_ln(2, 58)) == nl.PASS and
              grade_census(e1, ends_ln(60, 0)) == nl.FAIL and grade_census(e1, armed_ln) == nl.FAIL,
              "transport_composed census1: ENDS must show frames ended at present records; none (XE_SWAP only) or no "
              "ENDS line (the dark census: no report) FAILs")
        check(grade_census(e0, ends_ln(60, 0)) == nl.PASS and grade_census(e0, ends_ln(59, 1)) == nl.FAIL and
              grade_census(e0, "") == nl.FAIL,
              "transport_composed census0: every frame at an XE_SWAP packet (present=0); a record frame or no ENDS "
              "FAILs")
        check(any(c["field"] == "target.coverage.state" and c["value"] == "live" for c in
                  next(it for it in tcs["census"].ledger if it["id"] == "composed_census")["checks"]),
              "transport_composed: the census item requires the target's coverage live")

        def summary(tag, block):
            json.dump({"pacing": block}, open(os.path.join(td, "tcmp_%s_summary.json" % tag), "w", encoding="utf-8"))
        summary("pace0_trace", tpa.trace(3000))
        summary("pace0b_trace", tpa.trace(3000))
        summary("pace1_trace", tpa.trace(3000))
        check(r.do_pacing(tcs["pacing"]) == "PASS", "transport_composed pacing: matching pacing PASSes")
        check(run_unique("_pacing", "PASS")[0],
              "transport_composed pacing: a run-unique JSON, verdict stamped, committed; its item grades PASS")
        # a calibration control the instrument cannot match with the first: INCONCLUSIVE, and no ledger item
        summary("pace0b_trace", tpa.trace(3000, wait=lambda k: 900))
        v = r.do_pacing(tcs["pacing"])
        r.plan, r.exe_id = tcp, "test"
        check(v == "INCONCLUSIVE" and "calibration was FAIL" in r.results[-1]["detail"] and
              not any(it["id"] == "composed_pacing" for _o, it in r.items()),
              "transport_composed pacing: a failing control-vs-control calibration is INCONCLUSIVE and writes no item")
        # Astra round 3: a hard failure (a 100-vblank stall) with an unrelated calibration failure stays a FAIL, and
        # its evidence is written
        summary("pace1_trace", tpa.trace(3000, vbl=lambda k: 100 if k == 2500 else 2))
        v = r.do_pacing(tcs["pacing"])
        check(v == "FAIL" and "vblanks_max_match" in r.results[-1]["detail"] and
              any(it["id"] == "composed_pacing" for _o, it in r.items()),
              "transport_composed pacing: a hard failure stays a FAIL whatever the calibration, its item written")
        # an underpowered difference (50 -> 75 delayed gates in 2,048) is INCONCLUSIVE with a passing calibration
        summary("pace0b_trace", tpa.trace(3000))
        summary("pace0_trace", tpa.block([2] * 2048, tpa.delayed(2048, 50), [300] * 2049))
        summary("pace0b_trace", tpa.block([2] * 2048, tpa.delayed(2048, 50), [300] * 2049))
        summary("pace1_trace", tpa.block([2] * 2048, tpa.delayed(2048, 75), [300] * 2049))
        v = r.do_pacing(tcs["pacing"])
        check(v == "INCONCLUSIVE" and "gate_doubles_match" in r.results[-1]["detail"] and
              "presents needed" in r.results[-1]["detail"],
              "transport_composed pacing: an underpowered difference is INCONCLUSIVE (not PASS), naming the presents needed")
        summary("pace0_trace", tpa.trace(3000))
        summary("pace1_trace", tpa.trace(3000, gates=lambda k: 2))
        v = r.do_pacing(tcs["pacing"])
        check(v == "FAIL" and "target_one_gate_per_present" in r.results[-1]["detail"],
              "transport_composed pacing: a gate applied twice a present FAILs")
        # Astra round 1: a regression keeping the mean vblanks and the median wait (alternating 0 / 4, 100x spikes)
        summary("pace1_trace", tpa.trace(3000, vbl=lambda k: 0 if k % 2 else 4,
                                         wait=lambda k: 30000 if k % 4 == 0 else 300))
        v = r.do_pacing(tcs["pacing"])
        check(v == "FAIL" and "vblanks_distribution_match" in r.results[-1]["detail"] and
              "throttle_wait_p95_match" in r.results[-1]["detail"],
              "transport_composed pacing: a distribution / tail regression with the mean and median kept FAILs")
        summary("pace1_trace", {"n": 0})
        check(r.do_pacing(tcs["pacing"]) == "FAIL", "transport_composed pacing: a trace without kind 29 FAILs")
    # Astra round 1: the pacing arms are uninstrumented (no SR_NATIVE_SEG_TILE), so their trace items must not ask for
    # the forced drained points only that instrument writes; the instrumented probe keeps them
    def fields(step):
        return {c["field"] for it in tcs[step].ledger for c in it["checks"]}
    check(all("SR_NATIVE_SEG_TILE=1" not in tcs[t].gates.split(",") for t in ("pace0", "pace1", "pace0b")) and
          all(not any(f.startswith("segment_order.") for f in fields(t))
              for t in ("pace0_trace", "pace1_trace", "pace0b_trace")) and
          fields("pace0_trace") == fields("pace1_trace") == fields("pace0b_trace") and
          "idle.a74_nonzero_exit" in fields("pace1_trace"),
          "transport_composed: the pacing traces (uninstrumented arms) ask no forced drains, the same items every arm")
    check("SR_NATIVE_SEG_TILE=1" in tcs["probe"].gates.split(",") and
          {"segment_order.forced_drains", "segment_order.drain_at_end"} <= fields("probe_trace"),
          "transport_composed: the instrumented probe's trace keeps the forced-drain reconciliation")

    # transport_fix (2026-10-14, the tcmp1 diagnosis's follow-up; Astra round 1): tcmp1's own probe / stress rolls
    # re-graded by the decline counters, the binding record in the probe trace, the probe again ended by the graceful
    # quit (dev 95e221f5) with its terminal items and the terminal segment_order; it resolves nothing.
    tfp = plans["transport_fix"]
    tfx = {s.tag: s for s in tfp["steps"]}
    check(tfx["probe"].gates == tcs["probe"].gates and tfx["probe"].trace and tfx["stress"].gates == tcs["stress"].gates
          and tfx["stress"].mode == tcs["stress"].mode,
          "transport_fix: the probe and stress rolls are transport_composed's own (the composed env)")
    pg = tfx["probe_graceful"]
    check(not tfp.get("resolves") and pg.kind == "roll" and pg.trace and pg.mode == tfx["probe"].mode and
          pg.gates == tfx["probe"].gates + ",SR_DRIVE_GRACEFUL_QUIT=1" and npf.step_graceful(pg) and
          not npf.step_graceful(tfx["probe"]) and not any(s.kind == "owed" for s in tfp["steps"]),
          "transport_fix: probe_graceful is the probe's own roll ended by the graceful quit (its gate), nothing owed")
    check(sorted(i["id"].replace("probe_graceful", "probe") for i in pg.ledger) ==
          sorted(i["id"] for i in tfx["probe"].ledger),
          "transport_fix: probe_graceful carries every probe item, under its own ids")
    tfi = {it["id"]: it for s in tfp["steps"] for it in s.ledger}
    split = ["devRead", "devWrite", "front", "stack", "tiling", "flags2A39", "movieCapture", "pix", "timestamps", "3B28",
             "hsioRead", "hsio", "movieGlobalRead", "movieGlobal", "rect", "sysbuf", "rootRead", "root", "export",
             "cursorAlign", "cursorWrite", "entry", "fallbackFront", "fallbackScaler"]
    older = [r for r in split if not r.endswith("Read") or r == "devRead"]  # 8648af78's exe: no read / state split

    def why_line(names=split, **counts):
        return ("[t] [INFO ] [BOOT] [NATIVE_FLUSH] periodic swapSdkWhy " +
                " ".join("%s=%d" % (r, counts.get(r, 0)) for r in names) + "\n")
    for wid in ("tfix_probe_swap_why", "tfix_stress_swap_why", "tfix_probe_graceful_swap_why"):
        why = tfi[wid]
        check(".*" not in why["pattern"] and all(r + "=" in why["pattern"] for r in split),
              "transport_fix %s: every reason named, no wildcard (Astra round 1)" % wid)
        check(grade([why], why_line(flags2A39=1))["why"] == nl.PASS and
              grade([why], why_line(timestamps=2, root=1, hsio=1, movieGlobal=1, export=3))["why"] == nl.PASS,
              "transport_fix %s: refused modes / states (the boot decline's kind) pass" % wid)
        for r in ("devRead", "devWrite", "front", "stack", "hsioRead", "movieGlobalRead", "rect", "rootRead",
                  "cursorAlign", "cursorWrite", "entry", "fallbackFront", "fallbackScaler"):
            check(grade([why], why_line(**{r: 1}))["why"] == nl.FAIL,
                  "transport_fix %s: a %s decline (a read / writability / admission failure) fails" % (wid, r))
        check(grade([why], why_line(older))["why"] == nl.PASS and
              grade([why], why_line(older, flags2A39=1))["why"] == nl.PASS and
              all(grade([why], why_line(older, **{r: 1}))["why"] == nl.FAIL
                  for r in ("hsio", "movieGlobal", "root", "rect")),
              "transport_fix %s: an exe before the split - its combined hsio / movieGlobal / root at 0 (fail-closed)" % wid)
    boot = tfi["tfix_probe_swap_boot"]
    sdk = "[t] [INFO ] [BOOT] [NATIVE_FLUSH] periodic swapSdk calls=19402 decl=%d\n"
    check([grade([boot], sdk % n)["boot"] for n in (0, 1, 2, 10, 26)] == [nl.PASS, nl.PASS, nl.FAIL, nl.FAIL, nl.FAIL],
          "transport_fix swap_boot: at most one decline in the run (decl 0 / 1 pass; 2, 10, 26 fail)")
    mem = tfi["tfix_probe_bracket_memory"]
    bm = "[t] [INFO ] [BOOT] [NATIVE_FLUSH] periodic bracketMemory frame=0 dev=0 stack=0 lists=0 align=0/0/0 %s entry=0\n"
    check(grade([mem], bm % "prefix=0/0/0 cursor=0/0/0")["memory"] == nl.PASS and
          grade([mem], bm % "prefix=1/0/0 cursor=0/0/0")["memory"] == nl.FAIL and
          grade([mem], bm % "prefix=0/0/0 cursor=0/0/34")["memory"] == nl.FAIL,
          "transport_fix bracket_memory: zero at every site passes, a list prefix / cursor decline fails")
    check({"field": "segment_order.bracket_binds", "op": "gt", "value": 0} in tfi["tfix_probe_trace"]["checks"] and
          {"field": "segment_order.skipped", "op": "eq", "value": 0} in tfi["tfix_probe_trace"]["checks"],
          "transport_fix: the probe trace requires the binding record and keeps skipped 0 (the tail apart)")
    gtr = tfi["tfix_probe_graceful_trace"]["checks"]
    check(tfx["probe_graceful_trace"].base == "probe_graceful" and
          all(c in gtr for c in tfi["tfix_probe_trace"]["checks"]) and
          all({"field": "segment_order." + f, "op": "eq", "value": 0} in gtr
              for f in ("tail_bound_unapplied", "brackets_open", "bound_unapplied", "published_untaken")),
          "transport_fix: the graceful trace keeps the probe trace's checks and grades the terminal segment_order whole")
    terminal_seg = {"segment_order": {"segments": 9, "late_first": 0, "skipped": 0, "forced_drains": 3, "forced_miss": 0,
                                      "drain_at_end": True, "terminal_unapplied": 0, "bracket_binds": 4,
                                      "bracket_segments": 12, "bracket_late_first": 0,
                                      "tail_bound_unapplied": 0, "brackets_open": 0, "bound_unapplied": 0,
                                      "published_untaken": 0}}
    for f, v in (("tail_bound_unapplied", 11), ("brackets_open", 1), ("bound_unapplied", 11), ("published_untaken", 1),
                 ("brackets_open", -1)):
        bad = {"segment_order": dict(terminal_seg["segment_order"], **{f: v})}
        check(not npf.json_checks(terminal_seg, [c for c in gtr if c["field"].startswith("segment_order.")]) and
              npf.json_checks(bad, [c for c in gtr if c["field"] == "segment_order." + f]),
              "transport_fix graceful trace: %s=%d fails (a drained run cuts nothing)" % (f, v))
    os.makedirs(os.path.join(npf.LANE, "archive"), exist_ok=True)  # the items' paths are umbrella-relative: the same drive
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        r = npf.Runner(Args(plan="transport_fix", out=td), tfp)
        for s in tfp["steps"]:
            r.results.append({"step": s.tag, "kind": s.kind, "verdict": "DRY", "detail": "", "paths": {}})
        ids = [it["id"] for _o, it in r.items()]
        twins = [i["id"] + ".terminal" for i in pg.ledger if i.get("kind") == "text"]
        check("tfix.probe_graceful.terminal" in ids and "tfix.probe_graceful.quit_receipt" in ids and
              all(tw in ids for tw in twins) and len(twins) == len(pg.ledger) and
              not any(i.startswith("tfix.probe.") or i.startswith("tfix_probe_census.") for i in ids),
              "transport_fix: only probe_graceful gets the graceful quit's items (terminal, quit receipt, every twin)")

    # --reapply: the stored proof document's steps stand in for a run, the exe id comes from it
    # (a temp dir under the lane's archive: the items' paths are relative to the umbrella, so the same drive)
    os.makedirs(os.path.join(npf.LANE, "archive"), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        a = argparse.Namespace(plan="s1_ab", out=td, repo=td, exe=None, base=None, dry_run=False, only="", skip="",
                               owner="native-a", instance="native", slot_held=False, apply_ledger=False, ledger="", reapply=True)
        r = npf.Runner(a, npf.plans()["s1_ab"])
        doc = {"plan": "s1_ab", "exe_id": "d855d0e75a9b", "steps": [
            {"step": "replay", "kind": "replay", "verdict": "SKIP", "detail": "by --only / --skip", "paths": {}},
            {"step": "abp", "kind": "roll", "verdict": "PASS", "detail": "x", "paths": {"roll": "r"}},
            {"step": "abps", "kind": "roll", "verdict": "FAIL", "detail": "y", "paths": {"roll": "r"}}]}
        json.dump(doc, open(os.path.join(td, "s1b_proof.json"), "w", encoding="utf-8"))
        rc = r.reload()
        its = r.items()
        check(rc == 1 and [i["id"] for _o, i in its] == ["abp", "abps"] and all("d855d0e75a9b" in i["note"] for _o, i in its),
              "--reapply rebuilds the ledger items from the stored steps with the stored exe id (and fails on a FAIL step)")
        json.dump(dict(doc, plan="row3"), open(os.path.join(td, "s1b_proof.json"), "w", encoding="utf-8"))
        try:
            r.reload()
            check(False, "--reapply refuses another plan's document")
        except SystemExit:
            check(True, "--reapply refuses another plan's document")

    # display_registers (display-registers lane, 2026-10-14): its [NATIVE_VBLANK] items graded by the ledger tool on
    # synthetic lines in the exact format kernel/native_vblank.cpp finalLine prints - the pre-registered pure-ON and
    # toggled lines pass, each named defect fails; the whole-run dispreg item; the owed pacing step on a base without
    # native/transport-composed's comparator (DRY in a dry run, FAIL live, never SKIP).
    def vb_line(why="exit", state="on", toggled=0, ticks=9000, native=9000, guest=0, released=40, callbacks=0, bad=0,
                calls=300, host=300, pguest=0, scanout=0, sysbuf=0, bit=0, early=0, mismatch=0):
        return ("[t] [INFO ] [BOOT] [NATIVE_VBLANK] %s state=%s toggled=%d ticks=%d native=%d guest=%d released=%d "
                "callbacks=%d traceBad=%d late=0 maxGapUs=17012 vcount=6480000 | pacing calls=%d host=%d guest=%d "
                "wait=3 now=290 next=7 early=%d mismatch=%d | dispreg scanout=%d sysbuf=%d vsyncBit=%d\n" % (
                    why, state, toggled, ticks, native, guest, released, callbacks, bad, calls, host, pguest, early,
                    mismatch, scanout, sysbuf, bit))

    def grade_one(item, text):
        with tempfile.TemporaryDirectory() as td:
            open(os.path.join(td, "run.log"), "w", encoding="utf-8").write(text)
            ctx = nl.Ctx({"roots": {"umbrella": td}}, npf.LANE)
            return nl.check_item(ctx, dict(item, path="run.log"))[0]

    on_item, ab_item = npf.vblank_item("dreg_t_on"), npf.vblank_item("dreg_t_ab", toggled=True)
    good_on = vb_line(why="census", ticks=10) + vb_line()
    check(grade_one(on_item, good_on) == nl.PASS, "display_registers: a pure-ON [NATIVE_VBLANK] line passes its item")
    for what, line in (("a guest tick", vb_line(guest=3)), ("a guest pacing call", vb_line(pguest=1)),
                       ("a per-vblank check failure", vb_line(bad=1)), ("a scan-out answer", vb_line(scanout=2)),
                       ("the vsync-enable bit", vb_line(bit=1)), ("a swap callback", vb_line(callbacks=1)),
                       ("no tick", vb_line(ticks=0, native=0)), ("no pacing call", vb_line(calls=0, host=0)),
                       ("a refused gate", vb_line(state="refused")), ("a toggled roll", vb_line(toggled=1)),
                       ("an early release", vb_line(early=1)), ("a pacing mismatch", vb_line(mismatch=1)),
                       ("a mismatch count of 10", vb_line(mismatch=10))):
        check(grade_one(on_item, line) == nl.FAIL, "display_registers: the pure-ON item fails on %s" % what)
    check(grade_one(on_item, vb_line() + "[t] [WARN ] [BOOT] [NATIVE_VBLANK] trace BAD dev=40002880 count 1->1\n")
          == nl.FAIL, "display_registers: a trace BAD line anywhere fails the item")
    check(grade_one(on_item, vb_line() + "[t] [WARN ] [BOOT] [NATIVE_VBLANK] pacing MISMATCH arm=host arg=00010008\n")
          == nl.FAIL, "display_registers: a pacing MISMATCH line anywhere fails the item")
    check(grade_one(on_item, vb_line() + "[t] [ERROR] [BOOT] [NATIVE_DISPLAY] source0 failed result=0 exc=0 dev=0\n")
          == nl.FAIL, "display_registers: a failed native source 0 fails the item")
    check(grade_one(on_item, vb_line() + vb_line(why="final", bad=2)) == nl.FAIL,
          "display_registers: the LAST line decides (a later failing line fails)")
    good_ab = vb_line(toggled=1, native=4500, guest=4500, host=150, pguest=150, scanout=9000, bit=1)
    check(grade_one(ab_item, good_ab) == nl.PASS, "display_registers: a toggled line with both arms passes the A/B item")
    for what, line in (("no guest arm", vb_line(toggled=1, scanout=9000, bit=1)),
                       ("no host arm", vb_line(toggled=1, native=0, guest=9000, host=0, pguest=300, scanout=9000, bit=1)),
                       ("the OFF arm's inputs not kept", vb_line(toggled=1, native=4500, guest=4500, host=150,
                                                                 pguest=150)),
                       ("a per-vblank check failure", vb_line(toggled=1, native=4500, guest=4500, host=150, pguest=150,
                                                              scanout=9000, bit=1, bad=1)),
                       ("an early release", vb_line(toggled=1, native=4500, guest=4500, host=150, pguest=150,
                                                    scanout=9000, bit=1, early=2)),
                       ("a pacing mismatch", vb_line(toggled=1, native=4500, guest=4500, host=150, pguest=150,
                                                     scanout=9000, bit=1, mismatch=1))):
        check(grade_one(ab_item, line) == nl.FAIL, "display_registers: the A/B item fails on %s" % what)
    run_item = npf.dispreg_run_item("dreg_t_run")
    meter_run = ("[t] [INFO ] [BOOT] [NATIVE_METER_RUN] exit 240.1s frames=21764 | pm4=5369075 edram_res=0 edram_xfer=0 "
                 "edram_load=0 nrec=52946875 dispreg=%d xma=0 | per_frame pm4=246.7 edram_res=0.0 edram_xfer=0.0 "
                 "edram_load=0.0 nrec=2432.8 dispreg=%s xma=0.0\n")
    check(grade_one(run_item, meter_run % (0, "0.0")) == nl.PASS and
          grade_one(run_item, meter_run % (1, "0.0")) == nl.FAIL and
          grade_one(run_item, meter_run % (569816, "26.2")) == nl.FAIL,
          "display_registers: the whole-run dispreg item passes only on a cumulative 0 (one boot-time answer fails)")
    dplan = npf.plans()["display_registers"]
    tags = [s.tag for s in dplan["steps"]]
    owed = [s for s in dplan["steps"] if s.kind == "owed"]
    have_cmp = hasattr(npf.Runner, "do_pacing")
    check((not have_cmp and [s.tag for s in owed] == ["pacing"]) or (have_cmp and not owed and "pacing" in tags),
          "display_registers: the pacing step is the comparator's when native/transport-composed is merged, else one "
          "owed step")
    check(all("SR_NATIVE_VBLANK=1" in s.gates for s in dplan["steps"] if s.kind in ("roll", "gate") and
              s.tag not in ("meter0", "pace0", "pace0b")) and
          all("SR_NATIVE_VBLANK=0" in s.gates for s in dplan["steps"] if s.tag in ("meter0", "pace0", "pace0b")) and
          all(k + "=1" in dplan["gates"] for k in ("SR_NATIVE_GATE", "SR_NATIVE_SWAP", "SR_NATIVE_SWAP_BLOCK",
                                                   "SR_NATIVE_DEVCREATE", "SR_NATIVE_BRACKET", "SR_NATIVE_TRANSPORT")),
          "display_registers: every gated roll carries SR_NATIVE_VBLANK=1 on the composed transport, its controls =0")
    if owed:
        for dry in (True, False):
            with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
                a = argparse.Namespace(plan="display_registers", out=td, repo=td, exe=None, base=None, dry_run=dry,
                                       only="", skip="", owner="native-a", instance="native", slot_held=True,
                                       apply_ledger=False, ledger="", reapply=False)
                r = npf.Runner(a, dplan)
                v = r.do_owed(owed[0])
                check(v == ("DRY" if dry else "FAIL") and "OWED" in r.results[-1]["detail"],
                      "display_registers: the owed pacing step records %s in a %s run" % (v, "dry" if dry else "live"))
    # The proof's atomic replace rides out a transient sharing violation (frs1 2026-10-14: WinError 5 once, the plan lost)
    calls = []

    def flaky(n, exc):
        def rep(src, dst):
            calls.append((src, dst))
            if len(calls) <= n:
                raise exc
        return rep
    npf.replace_retry("a.tmp", "a", wait=0, _replace=flaky(3, PermissionError(5, "Access is denied")))
    check(len(calls) == 4, "replace_retry: three PermissionErrors, then the replace succeeds (4 attempts)")
    calls.clear()
    try:
        npf.replace_retry("a.tmp", "a", tries=5, wait=0, _replace=flaky(99, PermissionError(5, "Access is denied")))
        check(False, "replace_retry: a persistent PermissionError is raised after the last attempt")
    except PermissionError:
        check(len(calls) == 5, "replace_retry: a persistent PermissionError is raised after the last attempt")
    calls.clear()
    try:
        npf.replace_retry("a.tmp", "a", wait=0, _replace=flaky(1, FileNotFoundError(2, "gone")))
        check(False, "replace_retry: any other OSError is raised at once")
    except FileNotFoundError:
        check(len(calls) == 1, "replace_retry: any other OSError is raised at once")
    devcreate_close_checks(plans)
    devcreate_bake_checks(plans)
    zpd_bake_checks(plans)
    zpd_park_checks(plans)
    zpd_keep_checks(plans)

    boot_gaps_bake_checks(plans)

    # Residual unknowns resolved by a plan (tcmp1 2026-10-14): a multi-line unknown (its witness list one item per
    # line) gets its resolved_by in the same style; an aborted run that carries only some resolving items attaches none.
    syn = "\n".join([
        '{',
        '  "obligations": [',
        '    {',
        '      "id": "x.ob",',
        '      "evidence": [',
        '        {"id": "a", "kind": "text"},',
        '        {"id": "b", "kind": "text"}',
        '      ],',
        '      "residual_unknowns": [',
        '        {"id": "multi", "text": "t", "witness": [',
        '          {"id": "multi.w", "kind": "json"}',
        '        ]},',
        '        {"id": "single", "text": "s"}',
        '      ]',
        '    }',
        '  ]',
        '}', ''])
    ia, ib = {"id": "a", "kind": "text", "path": "p/a"}, {"id": "b", "kind": "text", "path": "p/b"}
    lt = npf.LedgerText(syn)
    check(lt.resolve_unknown("x.ob", "multi", [ia, ib]) == "resolved", "a multi-line unknown is resolved")
    u = json.loads(lt.text())["obligations"][0]["residual_unknowns"]
    check([x["id"] for x in u[0]["resolved_by"]] == ["multi.a", "multi.b"] and u[0]["witness"][0]["id"] == "multi.w" and
          u[1] == {"id": "single", "text": "s"},
          "...its resolved_by follows the witness list; the witness and the next unknown are untouched")
    check(any(ln == '          {"id": "multi.a", "kind": "text", "path": "p/a"},' for ln in lt.text().split("\n")),
          "...one resolver per line at the witness indent (the file's own style)")
    check(lt.resolve_unknown("x.ob", "multi", [ia]) == "kept", "a resolved multi-line unknown is not resolved twice")
    check(lt.resolve_unknown("x.ob", "single", [ia]) == "resolved", "a one-line unknown still resolves in place")
    # --rebind-resolves (Astra round 1 on native/zpd-bake: a pre-registered regrade's stricter result stands): a resolved
    # one-line unknown's resolved_by is REPLACED by this run's items; without the flag it is kept; a multi-line one is
    # refused (never rewritten)
    check(lt.resolve_unknown("x.ob", "single", [ib]) == "kept", "a resolved one-line unknown is kept without rebind")
    check(lt.resolve_unknown("x.ob", "single", [ia, ib], rebind=True) == "rebound (1 -> 2 resolving items)",
          "--rebind-resolves replaces a resolved one-line unknown's resolved_by")
    u = json.loads(lt.text())["obligations"][0]["residual_unknowns"]
    check([x["id"] for x in u[1]["resolved_by"]] == ["single.a", "single.b"] and u[1]["text"] == "s" and
          [x["id"] for x in u[0]["resolved_by"]] == ["multi.a", "multi.b"],
          "...its resolved_by is this run's set (ids <uid>.<id>), the text and the other unknown untouched")
    try:
        lt.resolve_unknown("x.ob", "multi", [ib], rebind=True)
        check(False, "--rebind-resolves refuses a multi-line unknown")
    except ValueError:
        check(True, "--rebind-resolves refuses a multi-line unknown")
    odd = npf.LedgerText(syn.replace('        ]},\n        {"id": "single"', '        ], "more": 1},\n        {"id": "single"'))
    try:
        odd.resolve_unknown("x.ob", "multi", [ia])
        check(False, "a multi-line unknown with another close is refused")
    except ValueError:
        check(True, "a multi-line unknown with another close is refused")

    # A pretty-printed unknown ('{' alone, one key per line; crb1 2026-10-15) resolves as the last key, in its own style
    pretty = syn.replace('        {"id": "single", "text": "s"}',
                         '        {\n          "id": "pretty",\n          "text": "p"\n        }')
    lt = npf.LedgerText(pretty)
    check(lt.resolve_unknown("x.ob", "pretty", [ia, ib]) == "resolved", "a pretty-printed unknown is resolved")
    u = json.loads(lt.text())["obligations"][0]["residual_unknowns"]
    check(u[1]["id"] == "pretty" and u[1]["text"] == "p" and
          [x["id"] for x in u[1]["resolved_by"]] == ["pretty.a", "pretty.b"] and u[0]["id"] == "multi",
          "...its resolved_by is its last key; the other keys and the other unknown are untouched")
    check(lt.resolve_unknown("x.ob", "pretty", [ia]) == "kept", "a resolved pretty-printed unknown is not resolved twice")
    try:
        npf.LedgerText(syn).resolve_unknown("x.ob", "absent", [ia])
        check(False, "an unknown that is not in the record is refused")
    except KeyError:
        check(True, "an unknown that is not in the record is refused")

    class StubRunner:
        def __init__(self, its):
            self._its = its
            self.plan = {"resolves": [("x.ob", "multi", ["a", "b"])]}

        def items(self):
            return self._its

    with tempfile.TemporaryDirectory() as td:
        lp = os.path.join(td, "ledger.json")
        open(lp, "w", encoding="utf-8", newline="").write(syn)
        done = npf.apply_ledger(lp, StubRunner([("x.ob", dict(ia))]))
        u = json.loads(open(lp, encoding="utf-8").read())["obligations"][0]["residual_unknowns"][0]
        check("resolved_by" not in u and any(d[1] == "multi" and "NOT resolved" in d[2] and "lacks b" in d[2] for d in done),
              "apply_ledger: a run with only some of an unknown's resolving items attaches none and says which are missing")
        # p2.zpd zpd6: a NO DATA item (its roll issued no query twice) is recorded but never resolves the unknown, so
        # a later run with data still can (resolve_unknown attaches once)
        nodata = StubRunner([("x.ob", dict(ia)), ("x.ob", dict(ib))])
        nodata.nodata_ids = lambda: {"a"}
        done = npf.apply_ledger(lp, nodata)
        u = json.loads(open(lp, encoding="utf-8").read())["obligations"][0]["residual_unknowns"][0]
        check("resolved_by" not in u and ("x.ob", "multi", "unknown NOT resolved: NO DATA for a") in done and
              any(d[1] == "a" for d in done),
              "apply_ledger: a NO DATA item is recorded but never resolves an unknown (zpd6)")
        done = npf.apply_ledger(lp, StubRunner([("x.ob", dict(ia)), ("x.ob", dict(ib))]))
        u = json.loads(open(lp, encoding="utf-8").read())["obligations"][0]["residual_unknowns"][0]
        check([x["id"] for x in u.get("resolved_by", [])] == ["multi.a", "multi.b"] and
              ("x.ob", "multi", "unknown resolved") in done,
              "apply_ledger: a run with every resolving item resolves the unknown")

    print("native proof test: %d failed" % FAILS)
    return FAILS


def construns_bake_checks(plans, gates_of):
    """The construns bake (native/construns-bake, 2026-10-14): the plan's arms, the shaderConstRuns line's items on
    lines in kernel/native_flush.cpp's exact format, the census step's --construns-zero wiring, its item against the
    tool's own output, and the resolvers."""
    crb = plans["construns_bake"]
    tags = [s.tag for s in crb["steps"]]
    check(crb.get("bake") is True and crb["obligations"] == ["p1.row3_construns"] and crb["prefix"] == "crb" and
          not crb.get("exe_pin") and not npf.archived_bases(crb),
          "construns_bake: a same-exe bake plan of p1.row3_construns (no pin)")
    check(tags == ["replay", "gate", "probe", "probe_trace", "cppm4", "stress", "abp", "census0", "census1", "census",
                   "meter_off", "meter_on", "meter"],
          "construns_bake: replay, gate, probe + trace, cppm4, stress, A/B, the census pair + step, the meter pair + step")
    check(gates_of("construns_bake", "meter_on") == "" and
          gates_of("construns_bake", "meter_off") == "SR_NATIVE_SHADER_CONST_RUNS=0" and
          gates_of("construns_bake", "census0").split(",") == ["SR_NATIVE_SHADER_CONST_RUNS=0", "SR_NATIVE_BRACKETS=1"] and
          gates_of("construns_bake", "census1") == "SR_NATIVE_BRACKETS=1" and
          gates_of("construns_bake", "gate") == "SR_NATIVE_SEG_TILE=1" and
          gates_of("construns_bake", "abp").split(",") == ["SR_AB_TOGGLE=SR_NATIVE_SHADER_CONST_RUNS", "SR_AB_PRESENT=1"],
          "construns_bake: the default path in every ON arm (only observe-only witnesses / the census), =0 in the off arms")
    # the shaderConstRuns line exactly as report() prints it
    fmt = ("[t] [INFO ] [BOOT] [NATIVE_FLUSH] periodic shaderConstRuns calls={calls} chunked={chunked} | gate={gate} "
           "drv=900 sdk={sdk} abGuest={abGuest} decl={decl} (off {decl} oracle 0 mask 0 span 0 open 0 emit 0) | "
           "kick={kick} marginBad={margin} | tessDraws={tess} tessSdk={tessSdk} | cpPm4={cp}")
    good = dict(calls=903, chunked=0, gate=1, sdk=3, abGuest=0, decl=0, kick=0, margin=0, tess=0, tessSdk=0, cp=0)

    def ok(it, **kw):
        line = fmt.format(**dict(good, **kw))
        return bool(re.search(it["select"], line) and re.search(it["pattern"], line))
    strict, cp, stress, ab = (npf.scr_item("x"), npf.scr_item("x", "cppm4"), npf.scr_item("x", "stress"),
                              npf.scr_item("x", "ab"))
    check(ok(strict) and not ok(strict, decl=1) and not ok(strict, abGuest=1) and not ok(strict, chunked=1) and
          not ok(strict, margin=1) and not ok(strict, tess=1) and not ok(strict, tessSdk=1) and not ok(strict, gate=0) and
          not ok(strict, calls=0) and ok(strict, kick=5, cp=7),
          "scr_item strict: calls > 0, gate 1, no SDK body (decl / abGuest 0), chunked / marginBad / tess 0")
    check(ok(cp, cp=7) and not ok(cp) and not ok(cp, cp=7, decl=1), "scr_item cppm4: strict and cpPm4 > 0")
    check(ok(stress, kick=4) and not ok(stress) and not ok(stress, kick=4, chunked=1), "scr_item stress: strict and kick > 0")
    check(ok(ab, abGuest=5, decl=9) and not ok(ab) and not ok(ab, abGuest=5, decl=9, tess=1),
          "scr_item ab: both arms (abGuest > 0; decl counts the off window)")
    # the census step runs --construns-zero; its item's checks all exist in the tool's output and pass on a good pair
    items = {it["id"]: it for s in crb["steps"] for it in s.ledger}
    os.makedirs(os.path.join(npf.LANE, "archive"), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        runner = npf.Runner(Args(plan="construns_bake", out=td, dry_run=True), crb)
        runner.do_tcensus({s.tag: s for s in crb["steps"]}["census"])
        check("--construns-zero" in runner.results[-1]["detail"], "construns_bake: the census step runs --construns-zero")
        import test_transport_census as ttc  # noqa: E402
        import transport_census as tcz  # noqa: E402
        b, t, j = (os.path.join(td, n) for n in ("b.log", "t.log", "out.json"))
        ttc.write_log(b, 60, dict(ttc.BASE, **{"820FC000": {"r20": 1, "r0A": 2, "3C": 1, "r23": 1, "r44": 3}}))
        ttc.write_log(t, 60, dict(ttc.BASE, **{"820FC000": {"r44": 3}}))
        with contextlib.redirect_stdout(io.StringIO()):
            rc = tcz.main(["--base", b, "--target", t, "--construns-zero", "--json", j])
        d = json.load(open(j, encoding="utf-8"))
        cz = [c for c in items["crb_census"]["checks"] if not (c["field"].startswith("runner.") or c["field"] == "committed")]
        check(rc == 0 and npf.json_checks(d, cz) == [], "crb_census: every check reads the tool's output and passes a good pair")
        ttc.write_log(t, 60, dict(ttc.BASE, **{"820FC000": {"r0A": 2, "3C": 1, "r44": 3}}))
        with contextlib.redirect_stdout(io.StringIO()):
            rc = tcz.main(["--base", b, "--target", t, "--construns-zero", "--json", j])
        d = json.load(open(j, encoding="utf-8"))
        bad = npf.json_checks(d, cz)
        check(rc == 1 and any("target_construns_sync_zero" in x for x in bad) and any("sync_pk" in x for x in bad),
              "crb_census: a target still holding the SDK-body preamble fails it")
        # Astra round 1 (P2): the preamble with its attribution lost (an UNSTAMPED line) fails the item too
        ttc.write_log(t, 60, dict(ttc.BASE, **{"820FC000": {"r44": 3}, "UNSTAMPED": {"r0A": 2, "3C": 1}}))
        with contextlib.redirect_stdout(io.StringIO()):
            rc = tcz.main(["--base", b, "--target", t, "--construns-zero", "--json", j])
        bad = npf.json_checks(json.load(open(j, encoding="utf-8")), cz)
        check(rc == 1 and any("target_unattributed_sync_zero" in x for x in bad) and
              any("unattributed_sync_pk" in x for x in bad) and not any("target_construns_sync_zero" in x for x in bad),
              "crb_census: an unattributed (UNSTAMPED) preamble in the target fails it")
    # every resolver names items the plan yields
    for oid, uid, eids in crb["resolves"]:
        check(oid == "p1.row3_construns" and uid in ("default_bake", "segmentless_backpressure", "library_coverage") and
              all(e in items for e in eids), "construns_bake resolves %s: its items are the plan's" % uid)


def env_of(gates):
    """A gate string as perf_run applies it (the last KEY=value wins)."""
    e = {}
    for kv in [p for p in (gates or "").split(",") if "=" in p]:
        k, v = kv.split("=", 1)
        e[k.strip()] = v.strip()
    return e


def transport_bake_checks(plans):
    """The transport bake (native/transport-bake, 2026-10-15): the nine kinds default ON, DEVCREATE not. The plan's shape
    (one exe, no pin; the default arms carry no kind setting, the =0 arms every baked kind off, each graded pair differing
    in exactly those nine), the census step's --pm4-zero wiring and its item against the tool's own output, the
    resolvers against the ledger's pre-registered unknowns, and the older plans pinned to the arms they ran."""
    baked = list(npf.TRANSPORT_BAKED)
    tcp_kinds = ["SR_NATIVE_CALLBACK", "SR_NATIVE_GATE", "SR_NATIVE_SWAP_BLOCK", "SR_NATIVE_SWAP", "SR_NATIVE_BRACKET",
                 "SR_NATIVE_IDLE_WAIT", "SR_NATIVE_INVAL_RANGE", "SR_NATIVE_LOCK_COHER", "SR_NATIVE_TRANSPORT",
                 "SR_NATIVE_DEVCREATE"]
    check(baked == [k for k in tcp_kinds if k != "SR_NATIVE_DEVCREATE"] and
          env_of(npf.TRANSPORT_OFF) == {k: "0" for k in baked},
          "transport_bake: the baked kinds are the composed env's ten but DEVCREATE (boot.device_create's)")
    tbk = plans["transport_bake"]
    st = {s.tag: s for s in tbk["steps"]}
    tags = [s.tag for s in tbk["steps"]]
    check(tbk.get("bake") is True and tbk["obligations"] == ["p2.transport"] and tbk["prefix"] == "tbk" and
          tbk["gates"] == "" and not tbk.get("exe_pin") and not npf.archived_bases(tbk) and tbk.get("same_exe") is True,
          "transport_bake: a same-exe bake plan of p2.transport (no pin; same_exe enforced)")
    check(tags == ["replay", "gate", "probe", "probe_trace", "abp", "cppm4", "stress", "stress_off", "census0", "census1",
                   "census", "meter_off", "meter_on", "meter", "pace0", "pace1", "pace0b", "pace0_trace", "pace1_trace",
                   "pace0b_trace", "pacing"],
          "transport_bake: replay, gate, probe + trace, A/B, CPPM4, stress + its =0 control, the census pair + step, the "
          "meter pair + step, the three pacing rolls, their traces and the pacing step, in that order")
    default_tags = ["gate", "probe", "abp", "cppm4", "stress", "census1", "meter_on", "pace1"]
    off_tags = ["stress_off", "census0", "meter_off", "pace0", "pace0b"]
    check(all(not any(k in env_of(st[t].gates) for k in tcp_kinds) for t in default_tags),
          "transport_bake: every default arm sets no transport kind and no DEVCREATE (the baked default itself)")
    check(all(all(env_of(st[t].gates).get(k) == "0" for k in baked) and "SR_NATIVE_DEVCREATE" not in env_of(st[t].gates)
              for t in off_tags),
          "transport_bake: every =0 arm turns all nine baked kinds off (DEVCREATE at its default in both arms)")
    for a, b in (("census0", "census1"), ("meter_off", "meter_on"), ("pace0", "pace1"), ("stress_off", "stress")):
        ea, eb = env_of(st[a].gates), env_of(st[b].gates)
        diff = sorted(k for k in set(ea) | set(eb) if ea.get(k) != eb.get(k))
        check(diff == sorted(baked) and st[a].mode == st[b].mode,
              "transport_bake: %s / %s differ in the nine baked kinds only (%s)" % (a, b, diff))
    check(st["pace0b"].gates == st["pace0"].gates and st["pacing"].calib == "pace0b_trace" and
          (st["pacing"].base, st["pacing"].target) == ("pace0_trace", "pace1_trace") and
          all(env_of(st[t].gates).get("SR_HIGHFPS") == "0" and st[t].trace and
              (st[t].play, st[t].dwell, st[t].seconds) == (npf.PACE_LONG["play"], npf.PACE_LONG["dwell"],
                                                           npf.PACE_LONG["seconds"]) for t in ("pace0", "pace1", "pace0b"))
          and all(st[t].window == npf.PACE_WINDOW for t in ("pace0_trace", "pace1_trace", "pace0b_trace")) and
          all("SR_NATIVE_SEG_TILE" not in env_of(st[t].gates) for t in ("pace0", "pace1", "pace0b")),
          "transport_bake: pacing on the console path, long uninstrumented rolls, =0 / default / =0 calibration")
    toggled = set(env_of(st["abp"].gates).get("SR_AB_TOGGLE", "").split("+"))
    check(toggled == set(baked) - {"SR_NATIVE_SWAP_BLOCK"} and env_of(st["abp"].gates).get("SR_AB_PRESENT") == "1" and
          st["abp"].mode == "freeze",
          "transport_bake: the frozen per-present A/B toggles every baked kind but the VdSwap block")
    check(all(env_of(st[t].gates).get("SR_NATIVE_SEG_TILE") == "1" for t in
              ("gate", "probe", "abp", "cppm4", "stress", "stress_off")) and st["probe"].trace,
          "transport_bake: the strict witness in every pixel / probe roll, the P2 trace on the probe")
    check(env_of(st["census0"].gates).get("SR_NATIVE_BRACKETS") == "1" == env_of(st["census1"].gates).get(
          "SR_NATIVE_BRACKETS") and st["census"].kind == "tcensus" and st["census"].mode == "pm4zero" and
          (st["census"].base, st["census"].target) == ("census0", "census1"),
          "transport_bake: the census pair carries the producer census; the tcensus step is mode pm4zero")
    items = {it["id"]: it for s in tbk["steps"] for it in s.ledger}
    mi = items.get("tbk_meter", {})
    check(mi.get("kind") == "meter_delta" and mi.get("field") == "pm4" and mi.get("max_delta") == -82.6 and
          mi.get("base") == "meter_off" and (st["meter"].base, st["meter"].target) == ("meter_off", "meter_on"),
          "transport_bake: the meter =0 vs the default, pre-registered bound -82.6 (93.0 less the re-emit 6.0, less 5%)")
    os.makedirs(os.path.join(npf.LANE, "archive"), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        runner = npf.Runner(Args(plan="transport_bake", out=td, dry_run=True), tbk)
        runner.do_tcensus(st["census"])
        check("--pm4-zero --allow-devcreate-reemit" in runner.results[-1]["detail"],
              "transport_bake: the census step runs --pm4-zero --allow-devcreate-reemit")
        import test_transport_census as ttc  # noqa: E402
        import transport_census as tcz  # noqa: E402
        b, t, j = (os.path.join(td, n) for n in ("b.log", "t.log", "out.json"))
        cz = [c for c in items["tbk_census"]["checks"] if not (c["field"].startswith("runner.") or c["field"] == "committed")]
        reemit = {"82112050": {"60": 1, "61": 1, "62": 1, "63": 1, "r05": 1, "r0F": 1}}

        def grade(target, base=None, **kw):
            ttc.write_log(b, 60, ttc.BASE if base is None else base)
            ttc.write_log(t, kw.pop("frames", 60), target, **kw)
            with contextlib.redirect_stdout(io.StringIO()):
                rc = tcz.main(["--base", b, "--target", t, "--pm4-zero", "--allow-devcreate-reemit", "--json", j])
            return rc, npf.json_checks(json.load(open(j, encoding="utf-8")), cz)
        rc, bad = grade(reemit)
        check(rc == 0 and bad == [], "tbk_census: every check reads the tool's output and passes the default with only "
                                     "820F0048's re-emit (%s)" % bad)
        rc, bad = grade(ttc.ZERO)
        check(rc == 0 and bad == [], "tbk_census: a target with no packet at all passes too (DEVCREATE baked later)")
        rc, bad = grade(dict(reemit, **{"820FC000": {"r20": 1, "r0A": 2, "3C": 1, "r23": 1}}))
        check(rc == 1 and any("target_pm4_zero_interval" in x for x in bad) and
              any("interval_other_pk" in x for x in bad),
              "tbk_census: tcmp3's remainder (construns on 820FC000) left in the target fails it")
        rc, bad = grade(dict(reemit, **{"82111EC8": {"3C": 4, "54": 1, "r05": 5}}))
        check(rc == 1 and any("target_pm4_zero_from_ingame" in x for x in bad),
              "tbk_census: a transport producer left (the vblank gate) fails it")
        rc, bad = grade({"RING_UNSTAMPED": {"60": 1, "61": 1, "62": 1, "63": 1}})
        check(rc == 1 and any("target_pm4_zero_interval" in x for x in bad),
              "tbk_census: the re-emit's keys from the pump's unstamped bin selects fail it (82112050's line only)")
        rc, bad = grade(reemit, base=ttc.TARGET)
        check(rc == 1 and any("base_transport_exercised" in x for x in bad),
              "tbk_census: a base without the transport group fails it")
        # Astra round 1 (P2): the allowance attributed over the whole gameplay span - each of these PASSED the old item
        rc, bad = grade(ttc.ZERO, ingame_frame=15, extra_at={16: {"820FDCB8": {"r05": 1}}})
        check(rc == 1 and any("target_reemit_attributed" in x for x in bad) and any("target_reemit_whole" in x for x in bad),
              "tbk_census: a BlockUntilIdle r05 in gameplay before A fails it (%s)" % bad)
        rc, bad = grade(ttc.ZERO, frames=65, tail=5, late={"820FDCB8": {"r05": 1}})
        check(rc == 1 and any("target_reemit_whole" in x for x in bad),
              "tbk_census: five BlockUntilIdle r05 after B fail it")
        whole = {"RING_UNSTAMPED": {"60": 1, "61": 1, "62": 1, "63": 1}, "820FDCB8": {"r05": 1}, "UNSTAMPED": {"r0F": 1}}
        rc, bad = grade(ttc.ZERO, ingame_frame=15, extra_at={16: whole})
        check(rc == 1 and any("target_reemit_attributed" in x for x in bad) and
              not any("target_reemit_whole" in x for x in bad),
              "tbk_census: a whole six-key set from other producers before A fails it by attribution")
        rc, bad = grade(reemit, frames=65, tail=5)
        check(rc == 3 and any(x.startswith("verdict=") for x in bad) and any("reemit_unverified_pk" in x for x in bad),
              "tbk_census: the re-emit after the final report (unattributed) is INCONCLUSIVE and fails the item (%s)" % bad)
        rc, bad = grade(reemit, frames=65, tail=5, terminal_tail=True)
        check(rc == 0 and bad == [], "tbk_census: the same tail under the graceful quit's terminal report passes (%s)" % bad)

        # the runner: an INCONCLUSIVE census is its own row, and its item is exported (it grades FAIL: it supersedes an
        # earlier grader's PASS of the same id, never hides behind it); pacing's INCONCLUSIVE still writes none
        live = npf.Runner(Args(plan="transport_bake", out=td, dry_run=False), tbk)
        for tag, per in (("census0", ttc.BASE), ("census1", reemit)):
            lg = os.path.join(live.path(tag), "drive", "attempt1")
            os.makedirs(lg, exist_ok=True)
            ttc.write_log(os.path.join(lg, "run.log"), 65, per, tail=5 if tag == "census1" else 0)
        live.do_tcensus(st["census"])
        row = live.results[-1]
        exported = [it for _o, it in live.items() if it["id"] == "tbk_census"]
        sys.path.insert(0, os.path.dirname(HERE))
        import native_ledger as nl  # noqa: E402
        res = nl.check_item(nl.Ctx({"roots": {"umbrella": npf.UMBRELLA}}, npf.LANE), exported[0])[0] if exported else None
        check(row["verdict"] == "INCONCLUSIVE" and len(exported) == 1 and res == nl.FAIL,
              "tbk_census: the runner records INCONCLUSIVE and exports the item, which the ledger grades FAIL (%s, %s)" % (
                  row["verdict"], res))

    # THE STRICT WITNESS in every pixel roll (Astra round 1, P2): the complete drained predicate and the reclaim audit;
    # a witness line with idreorder / drainMiss / forcedMiss = 1 PASSED the old tile items (tile_item)
    sys.path.insert(0, os.path.dirname(HERE))
    import native_ledger as nl  # noqa: E402
    seg = ("[t] [INFO ] [BOOT] [NATIVE_FLUSH] seg tile segs=812 recs=90211 unsegmented=0 pads=3 gap=0 overlap=0 order=0 "
           "| first segs=812 recs=90211 dup=0 reorder=0 | water=12 head=34 | idreorder=%d deferred=2 | drains=5 "
           "drainBracket=1 drainMiss=%d | forced=7 forcedMiss=%d forcedPostponed=1\n")
    draws = "[t] [INFO ] [BOOT] [NATIVE_FLUSH] periodic draws=262144 reclaimed=9 NEVER-EXECUTED=%d | spans rec=5 pm4=0 \n"

    def wgrade(item, text):
        with tempfile.TemporaryDirectory() as td2:
            open(os.path.join(td2, "run.log"), "w", encoding="utf-8").write(text)
            return nl.check_item(nl.Ctx({"roots": {"umbrella": td2}}, npf.LANE), dict(item, path="run.log"))[0]
    drained_pat = npf.tile_drained_item("x")["pattern"]
    for tag in ("abp", "cppm4", "stress", "stress_off"):
        its = {it["id"]: it for it in st[tag].ledger}
        tile, xp = its.get("tbk_%s_tile" % tag), its.get("tbk_%s_xport" % tag)
        check(tile is not None and tile["pattern"] == drained_pat and xp is not None and
              xp["pattern"] == npf.xport_item("x")["pattern"],
              "transport_bake %s: the complete drained predicate and the reclaim audit (ids kept)" % tag)
        if tile is None or xp is None:
            continue
        good = seg % (0, 0, 0) + draws % 0
        check(wgrade(tile, good) == nl.PASS and wgrade(xp, good) == nl.PASS,
              "transport_bake %s: a clean drained witness and reclaim pass" % tag)
        check(all(wgrade(tile, seg % v + draws % 0) == nl.FAIL for v in ((1, 1, 1), (1, 0, 0), (0, 1, 0), (0, 0, 1))),
              "transport_bake %s: idreorder / drainMiss / forcedMiss = 1 (together or alone) fail the tile item" % tag)
        check(wgrade(xp, good.replace("NEVER-EXECUTED=0", "NEVER-EXECUTED=3")) == nl.FAIL,
              "transport_bake %s: a reclaimed record never executed fails the reclaim item" % tag)
    for tag in ("census0", "census1", "meter_off", "meter_on", "pace0", "pace1", "pace0b"):
        check("SR_NATIVE_SEG_TILE" not in env_of(st[tag].gates),
              "transport_bake %s: no strict witness (as tbk1 ran it; the census / meter / pacing exception)" % tag)

    # ONE EXE, ENFORCED (Astra round 1, P2): exe stamps, the comparison refusal, the --out guard, --reapply's --exe
    check(tbk.get("same_exe") is True, "transport_bake: same_exe")
    os.makedirs(os.path.join(npf.LANE, "archive"), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        exe_a, exe_b = os.path.join(td, "a.exe"), os.path.join(td, "b.exe")
        open(exe_a, "wb").write(b"exe A")
        open(exe_b, "wb").write(b"exe B")
        out = os.path.join(td, "out")
        os.makedirs(out)  # main() creates it (OutLock) before a Runner runs

        def runner(exe, **kw):
            return npf.Runner(Args(plan="transport_bake", out=out, exe=exe, dry_run=False, **kw), tbk)

        def safe(fn, default=None):  # a runner without the exe check (the old grader) fails the check, never crashes
            try:
                return fn()
            except AttributeError:
                return default

        def fake_roll(self, step):  # the roll's folder and its stamp, as do_roll leaves them
            safe(lambda: self.write_stamp(step.tag))
            os.makedirs(self.path(step.tag), exist_ok=True)
            return self.record(step, "DONE", "fake roll")

        def fake_meter(self, step):
            return self.record(step, "PASS", "fake meter")

        def quiet_run(r):
            with contextlib.redirect_stdout(io.StringIO()):
                return r.run()
        with patch.object(npf.Runner, "do_roll", fake_roll), patch.object(npf.Runner, "do_meter", fake_meter):
            ra = runner(exe_a, only="meter_off")
            quiet_run(ra)
            sa = safe(lambda: ra.stamp_of("meter_off"))
            check(bool(sa) and sa.get("exe_sha256") == safe(lambda: npf.exe_sha256(exe_a)) and
                  os.path.exists(ra.path("meter_off", ".exe.json")),
                  "exe stamp: a roll is stamped with its exe's sha256")
            # the reviewer's scenario: exe B, --only meter_on,meter into the same --out
            rb = runner(exe_b, only="meter_on,meter")
            try:
                quiet_run(rb)
                refused = None
            except SystemExit as e:
                refused = str(e)
            check(bool(refused) and "meter_off" in refused and "fresh --out" in refused,
                  "one exe: a live run into an --out holding another exe's roll is refused (%s)" % refused)
            # the step check on its own (an --out guard bypassed): the meter refuses exe A's OFF roll, yields no item
            rb2 = runner(exe_b, only="meter_on,meter")
            with contextlib.redirect_stdout(io.StringIO()):
                fake_roll(rb2, [s for s in tbk["steps"] if s.tag == "meter_on"][0])
            why = safe(lambda: rb2.exe_refusal(st["meter"]), "")
            check("meter_off" in why and "meter_on" not in why,
                  "one exe: the meter step refuses an input of another exe (%s)" % why)
            with contextlib.redirect_stdout(io.StringIO()):
                try:
                    rb2.record(st["meter"], "FAIL", "REFUSED: " + why, refused=True)
                except TypeError:  # the old runner: no refused rows
                    rb2.record(st["meter"], "FAIL", "REFUSED: " + why)
            check(not [it for _o, it in rb2.items() if it["id"] == "tbk_meter"],
                  "one exe: a refused comparison yields no item")
            # the same exe again (exe B's meter_on removed): accepted; pacing reads the p2trace summaries' stamps
            shutil.rmtree(rb2.path("meter_on"))
            if os.path.exists(rb2.path("meter_on", ".exe.json")):
                os.remove(rb2.path("meter_on", ".exe.json"))
            ra2 = runner(exe_a, only="meter_on,meter")
            quiet_run(ra2)
            mrow = next(r for r in ra2.results if r["step"] == "meter")
            check(mrow["verdict"] == "PASS" and not safe(lambda: ra2.exe_refusal(st["meter"]), ""),
                  "one exe: an --only re-run on the same exe passes the guard and the step check")
            check(set(safe(lambda: ra2.input_tags(st["pacing"]), [])) == {"pace0_trace", "pace1_trace", "pace0b_trace"}
                  and safe(lambda: ra2.input_tags(st["probe_trace"])) == ["probe"] and
                  safe(lambda: ra2.input_tags(st["census"])) == ["census0", "census1"],
                  "one exe: the pacing / p2trace / census steps' inputs")
            check("pace0_trace" in safe(lambda: ra2.exe_refusal(st["pacing"]), ""),
                  "one exe: a pacing step with no stamped summary refuses")
        # a roll from before the stamp: attested by the stored proof's row on the proof's exe only
        out2 = os.path.join(td, "legacy")
        os.makedirs(os.path.join(out2, "tbk_census0"))
        os.makedirs(os.path.join(out2, "tbk_census1"))
        for exe, want in ((exe_a, ""), (exe_b, "census0")):
            json.dump({"plan": "transport_bake", "exe_id": npf.exe_id(exe_a), "started": "x",
                       "steps": [{"step": "census0", "verdict": "DONE"}, {"step": "census1", "verdict": "DONE"}]},
                      open(os.path.join(out2, "tbk_proof.json"), "w", encoding="utf-8"))
            r = npf.Runner(Args(plan="transport_bake", out=out2, exe=exe, dry_run=False), tbk)
            why = safe(lambda: r.exe_refusal(st["census"]), "")
            check((want in why) if want else why == "",
                  "legacy rolls: the stored proof's rows attest them on its exe only (exe %s: %r)" % (
                      os.path.basename(exe), why[:80]))
        json.dump({"plan": "transport_bake", "exe_id": npf.exe_id(exe_a), "started": "x",
                   "steps": [{"step": "census0", "verdict": "SKIP"}, {"step": "census1", "verdict": "DONE"}]},
                  open(os.path.join(out2, "tbk_proof.json"), "w", encoding="utf-8"))
        r = npf.Runner(Args(plan="transport_bake", out=out2, exe=exe_a, dry_run=False), tbk)
        check("census0" in safe(lambda: r.exe_refusal(st["census"]), "") and
              "census0" in safe(lambda: r.mixed_out_refusal(), ""),
              "legacy rolls: a SKIP row attests nothing (the roll may be another run's)")
        # --reapply / --recompare: another --exe than the stored proof's is refused
        json.dump({"plan": "transport_bake", "exe_id": npf.exe_id(exe_a), "started": "x", "steps": []},
                  open(os.path.join(out2, "tbk_proof.json"), "w", encoding="utf-8"))
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                npf.Runner(Args(plan="transport_bake", out=out2, exe=exe_b, dry_run=False), tbk).reload()
            refused = None
        except SystemExit as e:
            refused = str(e)
        check(bool(refused) and "REFUSED" in refused, "--reapply: an --exe other than the stored proof's is refused")
    # the resolvers: every item the plan's, each unknown pre-registered in the ledger (one line, not yet resolved)
    led = json.load(open(os.path.join(npf.LANE, "docs", "native_ledger.json"), encoding="utf-8"))
    o = [x for x in led["obligations"] if x["id"] == "p2.transport"][0]
    unk = {u["id"]: u for u in o.get("residual_unknowns", [])}
    for oid, uid, eids in tbk["resolves"]:
        check(oid == "p2.transport" and uid in ("default_bake", "default_pacing") and uid in unk and
              all(e in items for e in eids), "transport_bake resolves %s: its items are the plan's, the unknown is in "
                                               "the ledger" % uid)
    bake_res = next(e for _o, u, e in tbk["resolves"] if u == "default_bake")
    check(all("tbk_%s_%s" % (t, w) in bake_res for t in ("abp", "cppm4", "stress", "stress_off") for w in ("tile", "xport")),
          "transport_bake: default_bake needs every pixel roll's drained witness and reclaim item")
    check(sorted(u for _o, u, _e in tbk["resolves"]) == ["default_bake", "default_pacing"],
          "transport_bake: it resolves default_bake and default_pacing")
    # the static default_on items: one per baked kind, each reading its own gate's `!e || e[0] != '0'`
    ev_ids = {e["id"]: e for e in o["evidence"]}
    want = {"default_on_" + k[len("SR_NATIVE_"):].lower(): k for k in baked}
    check(all(i in ev_ids and ev_ids[i]["kind"] == "source" and ev_ids[i]["authority"] == "static" and
              k in ev_ids[i]["pattern"] for i, k in want.items()),
          "ledger p2.transport: a static default_on item for each of the nine baked kinds")

    # the older plans keep the arms they ran: a kind proven ALONE pins the other kinds =0; the OFF arms the pre-bake
    # transport (segmentless with every baked kind =0)
    def kenv(plan, tag):
        return env_of(next(s.gates for s in plans[plan]["steps"] if s.tag == tag))

    def only_on(e, on):
        return all(e.get(k) == ("1" if k in on else "0") for k in baked)
    check(only_on(kenv("residue", "meter1"), {"SR_NATIVE_IDLE_WAIT", "SR_NATIVE_INVAL_RANGE", "SR_NATIVE_LOCK_COHER"})
          and only_on(kenv("residue", "meter0"), set()) and only_on(kenv("residue", "census0"), set()),
          "residue: its gated arms run the three residue gates alone, census0 / meter0 every baked kind off")
    check(only_on(kenv("bracket", "meter1"), {"SR_NATIVE_BRACKET"}) and only_on(kenv("bracket", "meter0"), set()),
          "bracket: BRACKET alone, meter0 every baked kind off")
    check(only_on(kenv("callback", "probe"), {"SR_NATIVE_CALLBACK"}) and only_on(kenv("callback", "gate"),
                                                                                  {"SR_NATIVE_CALLBACK"}),
          "callback: CALLBACK alone (with GATE / BRACKET native it would see no block)")
    check(only_on(kenv("present_gate", "probe"), {"SR_NATIVE_GATE"}) and
          only_on(kenv("swap_sdk", "probe"), {"SR_NATIVE_SWAP", "SR_NATIVE_SWAP_BLOCK"}) and
          only_on(kenv("transport_pair", "probe"), {"SR_NATIVE_SWAP", "SR_NATIVE_SWAP_BLOCK", "SR_NATIVE_TRANSPORT"}) and
          only_on(kenv("transport_pair", "stress_off"), {"SR_NATIVE_SWAP", "SR_NATIVE_SWAP_BLOCK"}),
          "present_gate / swap_sdk / transport_pair: their kinds alone - the pair's chain / park items need BRACKET off")
    check(all(only_on(kenv("segmentless", t), set()) for t in ("probe", "abp", "on", "meter0", "gate")) and
          all(only_on(kenv("segmentless_bake", t), set()) for t in ("meter_off", "meter_on")) and
          only_on(kenv("segmentless_drained", "abp"), set()),
          "segmentless plans: every arm the pre-bake transport (the census bound and the segmentless A/B predate it)")
    # genv merges the pins: no step names a baked kind twice (perf_run applies the last; a reader of the gate string
    # should not have to). Other keys repeat by design in older plans (a CPPM4 roll overriding its base's =0).
    dup = [(name, s.tag) for name, p in plans.items() for s in p["steps"]
           if any(sum(1 for kv in (s.gates or "").split(",") if kv.split("=", 1)[0] == k) > 1 for k in baked)]
    check(not dup, "no plan step names a baked transport kind twice: %s" % dup[:6])


def display_registers_bake_checks(plans):
    """The display-registers bake (native/dreg-bake, 2026-10-15): SR_NATIVE_VBLANK default ON. The plan's shape (one exe,
    no pin; the default arms leave the key unset, the =0 arms VBLANK_OFF, each graded pair differing in that key only),
    the strict witness in every roll (vblank + CPU-2 lock on the default, the off witness on =0, the transport's tile /
    reclaim on the probe and the pixel rolls), the items against synthetic lines in finalLine's / the meter's / the swap
    line's own formats (the CPU-2 bounds, the off witness, the run-cumulative dispreg <= d), the resolvers' partition,
    and every older plan pinned to the gate OFF it ran with."""
    import native_ledger as nl  # noqa: E402
    dbk = plans["display_registers_bake"]
    st = {s.tag: s for s in dbk["steps"]}
    tags = [s.tag for s in dbk["steps"]]
    check(dbk.get("bake") is True and dbk["obligations"] == ["display.registers"] and dbk["prefix"] == "dbk" and
          dbk["gates"] == "" and not dbk.get("exe_pin") and not npf.archived_bases(dbk) and dbk.get("same_exe") is True,
          "display_registers_bake: a same-exe bake plan of display.registers (no pin; same_exe enforced)")
    check(dbk.get("fresh_out") is True and tuple(dbk.get("default_keys") or ()) == ("SR_NATIVE_VBLANK",),
          "display_registers_bake: a fresh --out, and SR_NATIVE_VBLANK the default under test (Astra round 1, P2 x2)")
    check(tags == ["replay", "gate", "probe", "probe_trace", "abp", "cppm4", "meter0", "meter1", "meter", "pace0",
                   "pace1", "pace0b", "pace0_trace", "pace1_trace", "pace0b_trace", "pacing"],
          "display_registers_bake: replay, gate, probe + trace, A/B, CPPM4, the meter pair + step, the three pacing "
          "rolls, their traces and the pacing step, in that order")
    on_tags, off_tags = ["gate", "probe", "abp", "cppm4", "meter1", "pace1"], ["meter0", "pace0", "pace0b"]
    check(all("SR_NATIVE_VBLANK" not in env_of(st[t].gates) for t in on_tags) and
          all(env_of(st[t].gates).get("SR_NATIVE_VBLANK") == "0" for t in off_tags),
          "display_registers_bake: every default arm leaves SR_NATIVE_VBLANK unset (the baked default), every =0 arm "
          "sets it 0")
    check(not any(k in env_of(st[t].gates) for t in on_tags + off_tags for k in
                  list(npf.TRANSPORT_BAKED) + ["SR_NATIVE_DEVCREATE"]),
          "display_registers_bake: no arm sets a transport kind or DEVCREATE (the default path itself)")
    for a, b in (("meter0", "meter1"), ("pace0", "pace1")):
        ea, eb = env_of(st[a].gates), env_of(st[b].gates)
        diff = sorted(k for k in set(ea) | set(eb) if ea.get(k) != eb.get(k))
        check(diff == ["SR_NATIVE_VBLANK"] and st[a].mode == st[b].mode,
              "display_registers_bake: %s / %s differ in SR_NATIVE_VBLANK only (%s)" % (a, b, diff))
    check(st["pace0b"].gates == st["pace0"].gates and st["pacing"].calib == "pace0b_trace" and
          (st["pacing"].base, st["pacing"].target) == ("pace0_trace", "pace1_trace") and
          all(env_of(st[t].gates).get("SR_HIGHFPS") == "0" and st[t].trace and
              (st[t].play, st[t].dwell, st[t].seconds) == (npf.PACE_LONG["play"], npf.PACE_LONG["dwell"],
                                                           npf.PACE_LONG["seconds"]) for t in ("pace0", "pace1", "pace0b"))
          and all(st[t].window == npf.PACE_WINDOW for t in ("pace0_trace", "pace1_trace", "pace0b_trace")) and
          all("SR_NATIVE_SEG_TILE" not in env_of(st[t].gates) for t in ("pace0", "pace1", "pace0b", "meter0", "meter1")),
          "display_registers_bake: pacing on the console path, long uninstrumented rolls, =0 / default / =0 calibration; "
          "the meter arms uninstrumented too")
    check(env_of(st["abp"].gates).get("SR_AB_TOGGLE") == "SR_NATIVE_VBLANK" and
          env_of(st["abp"].gates).get("SR_AB_PRESENT") == "1" and st["abp"].mode == "freeze" and
          st["cppm4"].mode == "freeze" and env_of(st["cppm4"].gates).get("SR_NATIVE_FLUSH_CPPM4") == "1",
          "display_registers_bake: the frozen per-present A/B toggles SR_NATIVE_VBLANK alone; CPPM4 on the default")
    ids = {s.tag: [it["id"] for it in s.ledger] for s in dbk["steps"]}
    gate_items = {it["id"]: it for it in st["gate"].ledger}

    def has(tag, *suffixes):
        return all("dbk_%s_%s" % (tag, x) in ids[tag] for x in suffixes)
    check(all(has(t, "vblank", "default", "cpu2") for t in ("probe", "abp", "cppm4", "meter1", "pace1")) and
          all(has(t, "off") for t in off_tags) and
          not any("dbk_%s_default" % t in ids[t] for t in off_tags) and
          all(has(t, "tile", "xport") for t in ("probe", "abp", "cppm4")) and
          all(gate_items.get("dbk_gate_%s" % w, {}).get("roll") == "freeze"
              for w in ("vblank", "default", "cpu2", "tile", "xport")) and
          all(gate_items.get("dbk_gate_span_%s" % w, {}).get("roll") == "span"
              for w in ("vblank", "default", "cpu2", "tile")) and "dbk_gate_span_xport" not in gate_items,
          "display_registers_bake: the strict witness in every roll - vblank (its default form) + CPU-2 lock on each "
          "default roll and both gate rolls, the off witness on each =0 roll, tile + reclaim on the probe and the A/B / "
          "CPPM4 rolls, the gate's tile in both rolls and its reclaim in freeze only (as the plan text says)")
    items = {it["id"]: it for s in dbk["steps"] for it in s.ledger}
    vb_ids = ["dbk_%s_vblank" % t for t in ("probe", "abp", "cppm4", "meter1", "pace1")] + ["dbk_gate_vblank",
                                                                                           "dbk_gate_span_vblank"]
    check(all(npf.VBLANK_EXPLICIT_ON in items[i]["forbid"] for i in vb_ids) and
          npf.VBLANK_EXPLICIT_ON not in npf.vblank_item("x")["forbid"] and
          all(npf.VBLANK_EXPLICIT_ON not in it.get("forbid", []) for s in plans["display_registers"]["steps"]
              for it in s.ledger),
          "display_registers_bake: every default roll's vblank item forbids the explicit On line; display_registers' "
          "(=1 by design) does not")
    check("about" in dbk and "drained tile witness in both rolls and the reclaim audit in the freeze roll" in
          " ".join(dbk["about"].split()),
          "display_registers_bake: the plan text says what the gate grades (tile in both rolls, reclaim in freeze)")
    abv = items["dbk_abp_vblank"]
    check("toggled=1" in abv["pattern"] and "toggled=0" in items["dbk_probe_vblank"]["pattern"],
          "display_registers_bake: the A/B's vblank item is the toggled form, the others pure ON")
    mi = items["dbk_meter"]
    check(mi["kind"] == "meter_delta" and mi["field"] == "dispreg" and mi.get("max") == 0.0 and
          mi.get("base_min") == 2.0 and (st["meter"].base, st["meter"].target) == ("meter0", "meter1"),
          "display_registers_bake: the meter =0 vs the default - dispreg per ingame frame live (>= 2) at =0, exactly 0")
    run = items["dbk_meter1_run"]
    check(run["kind"] == "text_le" and run["lhs"]["field"] == "dispreg" and run["rhs"]["field"] == "decl" and
          not nl.item_errors("t", dict(run, path="x"), {"umbrella": "", "repo": ""}, set()) and
          nl.item_errors("t", dict(run, path="x", rhs={"select": "x"}), {"umbrella": "", "repo": ""}, set()) and
          nl.item_errors("t", dict(run, path="x", lhs={"select": "(", "field": "a"}), {"umbrella": "", "repo": ""}, set()),
          "display_registers_bake: the run-cumulative item is the ledger's text_le (dispreg vs decl), schema-checked")

    # the items graded by the ledger tool on lines in the exact formats the exe prints
    def vb(state="on", toggled=0, native=9000, guest=0, scanout=0, sysbuf=1, bit=0, tick=283, wait=3, cpu2=True):
        return ("[t] [INFO ] [BOOT] [NATIVE_VBLANK] census state=%s toggled=%d ticks=9000 native=%d guest=%d "
                "released=40 callbacks=0 traceBad=0 late=0 maxGapUs=0 vcount=0 | pacing calls=300 host=300 guest=0 "
                "wait=22 now=270 next=8 early=0 mismatch=0 | dispreg scanout=%d sysbuf=%d vsyncBit=%d%s\n" % (
                    state, toggled, native, guest, scanout, sysbuf, bit,
                    (" | cpu2 tickMaxUs=%d tickSlow=0 waitMaxUs=%d waitSlow=0" % (tick, wait)) if cpu2 else ""))

    def meter_run(cum, per="0.0"):
        return ("[t] [INFO ] [BOOT] [NATIVE_METER_RUN] periodic 210.0s frames=18292 | pm4=110296 edram_res=0 "
                "edram_xfer=0 edram_load=0 nrec=46615681 dispreg=%d xma=0 | per_frame pm4=6.0 edram_res=0.0 "
                "edram_xfer=0.0 edram_load=0.0 nrec=2548.4 dispreg=%s xma=0.0\n" % (cum, per))

    def swap(decl):
        return "[t] [INFO ] [BOOT] [NATIVE_FLUSH] periodic swapSdk calls=19486 decl=%d\n" % decl

    def grade(item, text):
        with tempfile.TemporaryDirectory() as td:
            open(os.path.join(td, "run.log"), "w", encoding="utf-8").write(text)
            ctx = nl.Ctx({"roots": {"umbrella": td}}, npf.LANE)
            return nl.check_item(ctx, dict(item, path="run.log"))[0]
    cpu2, vbi = items["dbk_probe_cpu2"], items["dbk_probe_vblank"]
    check(grade(cpu2, vb()) == nl.PASS and grade(cpu2, vb(tick=937, wait=102)) == nl.PASS and
          grade(cpu2, vb(tick=19999, wait=99999)) == nl.PASS and grade(vbi, vb()) == nl.PASS,
          "display_registers_bake: the fixed exe's lines (final1-3: tick <= 937 us, wait <= 102 us) pass the CPU-2 item")
    for what, line in (("a 20 ms tick hold", vb(tick=20000)), ("diag1's 49,890 us tick", vb(tick=49890)),
                       ("a 100 ms pacing wait", vb(wait=100000)), ("diag1's 32 s wait", vb(wait=32488017)),
                       ("a line without the lock's tail", vb(cpu2=False))):
        check(grade(cpu2, line) == nl.FAIL, "display_registers_bake: the CPU-2 item fails on %s" % what)
    check(grade(cpu2, vb() + vb(tick=21000)) == nl.FAIL,
          "display_registers_bake: the CPU-2 item reads the LAST line (the run's maxima)")
    # THE DEFAULT FORM (Astra round 1 on the branch, P2): the On line computeState prints names the form
    on_default = ("[t] [INFO ] [BOOT] [NATIVE_VBLANK] SR_NATIVE_VBLANK on (the baked default; =0 keeps the guest "
                  "bodies): the pacing 82111E38 and the vblank ISR's source 0 as host code\n")
    on_explicit = ("[t] [INFO ] [BOOT] [NATIVE_VBLANK] SR_NATIVE_VBLANK on (=1): the pacing 82111E38 and the vblank "
                   "ISR's source 0 as host code\n")
    dfi = items["dbk_probe_default"]
    check(grade(dfi, on_default + vb()) == nl.PASS and grade(vbi, on_default + vb()) == nl.PASS,
          "display_registers_bake: a default roll whose On line names the baked default passes its default-form and "
          "vblank items")
    for what, text, item in (("an explicit On line (SR_NATIVE_VBLANK=1 inherited)", on_explicit + vb(), dfi),
                             ("no On line at all", vb(), dfi),
                             ("a default and an explicit On line", on_default + on_explicit + vb(), dfi),
                             ("the vblank item on an explicit On line", on_explicit + vb(), vbi)):
        check(grade(item, text) == nl.FAIL, "display_registers_bake: the default form fails on %s" % what)
    check(grade(npf.vblank_item("dreg_t"), on_explicit + vb()) == nl.PASS,
          "display_registers: its own vblank item (the gate =1 by design) still passes the explicit form")
    # Runner.env_refusal: a live run refuses BEFORE anything else while the parent env carries the gate under test; a
    # dry run only notes it; transport_bake (no default_keys) is unaffected
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        for val in ("1", "0"):
            out = os.path.join(td, "env" + val)
            os.makedirs(out)
            with patch.dict(os.environ, {"SR_NATIVE_VBLANK": val}):
                r = npf.Runner(Args(plan="display_registers_bake", out=out, dry_run=False, slot_held=True), dbk)
                asked = []
                try:
                    with patch.object(r, "slot", lambda *a: asked.append(a)), \
                            patch.object(r, "write_proof", lambda: asked.append("write_proof")):
                        r.run()
                    refused = ""
                except SystemExit as e:
                    refused = str(e)
                check("REFUSED" in refused and "SR_NATIVE_VBLANK=%s" % val in refused and not asked and
                      not os.listdir(out),
                      "display_registers_bake: a live run with SR_NATIVE_VBLANK=%s in the parent env is refused before "
                      "any step, write or slot request (%s)" % (val, refused[:70]))
                rd = npf.Runner(Args(plan="display_registers_bake", out=out, dry_run=True), dbk)
                check(rd.env_refusal() and npf.Runner(Args(plan="transport_bake", out=out, dry_run=True),
                                                      plans["transport_bake"]).env_refusal() == "",
                      "env_refusal: the bake plan names the key (a dry run notes it); a plan without default_keys "
                      "ignores it (SR_NATIVE_VBLANK=%s)" % val)
        with patch.dict(os.environ, {}):
            os.environ.pop("SR_NATIVE_VBLANK", None)
            check(npf.Runner(Args(plan="display_registers_bake", out=td, dry_run=True), dbk).env_refusal() == "",
                  "env_refusal: nothing to refuse with SR_NATIVE_VBLANK unset")
        # fresh_out (Astra round 1, P2): an --only re-run into the set's --out is refused before anything is deleted
        out = os.path.join(td, "used")
        att = os.path.join(out, "dbk_meter1", "drive", "attempt1")
        os.makedirs(att)
        open(os.path.join(att, "run.log"), "w", encoding="utf-8").write("the graded roll\n")
        open(os.path.join(out, "dbk_proof.json"), "w", encoding="utf-8").write('{"plan": "display_registers_bake"}')

        def tree(root):
            got = []
            for d, _dirs, files in sorted(os.walk(root)):
                for f in sorted(files):
                    p = os.path.join(d, f)
                    got.append((os.path.relpath(p, root), open(p, "rb").read()))
            return got
        before = tree(out)
        removed = []
        with patch.dict(os.environ, {}):
            os.environ.pop("SR_NATIVE_VBLANK", None)
            r = npf.Runner(Args(plan="display_registers_bake", out=out, dry_run=False, slot_held=True,
                                only="meter1"), dbk)
            try:
                with patch.object(npf, "safe_rmtree", lambda *a, **k: removed.append(a)), \
                        patch.object(npf.shutil, "rmtree", lambda *a, **k: removed.append(a)), \
                        patch.object(r, "slot", lambda *a: removed.append(("slot",) + a)):
                    r.run()
                refused = ""
            except SystemExit as e:
                refused = str(e)
        check("REFUSED" in refused and "fresh --out" in refused and "dbk_meter1" in refused and not removed and
              tree(out) == before,
              "display_registers_bake: a live --only meter1 run into the set's --out is refused before anything is "
              "deleted or written (%s)" % refused[:80])
    off = items["dbk_meter0_off"]
    check(grade(off, meter_run(442510, "48.4")) == nl.PASS, "display_registers_bake: a =0 roll with no [NATIVE_VBLANK] "
                                                             "line and the answers live passes the off witness")
    for what, text in (("a census line (the gate on)", vb() + meter_run(442510, "48.4")),
                       ("the default's stand-down line", "[t] [WARN ] [BOOT] [NATIVE_VBLANK] default stands down: "
                                                         "SR_NATIVE_GATE (0)\n" + meter_run(442510, "48.4")),
                       ("no emulated answer", meter_run(0))):
        check(grade(off, text) == nl.FAIL, "display_registers_bake: the off witness fails on %s" % what)
    for what, text, want in (
            ("d = 1, dispreg 1 (the default path's boot decline)", swap(1) + meter_run(1) + swap(1), nl.PASS),
            ("d = 0, dispreg 0", meter_run(0) + swap(0), nl.PASS),
            ("dispreg 2 > d 1", swap(1) + meter_run(2) + swap(1), nl.FAIL),
            ("dispreg 1 with no decline", meter_run(1) + swap(0), nl.FAIL),
            ("the gate off (dispreg 442,510)", meter_run(442510, "48.4") + swap(1), nl.FAIL),
            ("no swap line after the meter line", swap(1) + meter_run(1), nl.FAIL),
            ("the per-frame 0.0 not read as the count", meter_run(3, "0.0") + swap(1), nl.FAIL),
            ("the LAST meter line counted", meter_run(0) + swap(0) + meter_run(1) + swap(0), nl.FAIL)):
        check(grade(run, text) == want, "display_registers_bake: the run-cumulative item %s on %s" % (
            "passes" if want == nl.PASS else "fails", what))
    # the resolvers: the pacing steps' items default_pacing, every other item default_bake - a partition of the plan
    res = {u: e for _o, u, e in dbk["resolves"]}
    pace = {"pace0", "pace1", "pace0b", "pace0_trace", "pace1_trace", "pace0b_trace", "pacing"}
    check(sorted(res) == ["default_bake", "default_pacing"] and
          all(o == "display.registers" for o, _u, _e in dbk["resolves"]) and
          sorted(res["default_bake"] + res["default_pacing"]) == sorted(items) and
          not set(res["default_bake"]) & set(res["default_pacing"]) and
          set(res["default_pacing"]) == {i for s in dbk["steps"] if s.tag in pace for i in ids[s.tag]} and
          "dbk_pacing" in res["default_pacing"] and "dbk_meter1_run" in res["default_bake"],
          "display_registers_bake: default_bake / default_pacing partition the plan's items")
    # a roll's text_le item is graded on the roll's run.log, like a text item (Runner.items)
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        r = npf.Runner(Args(plan="display_registers_bake", out=td, dry_run=True), dbk)
        r.results = [{"step": s.tag, "verdict": "DRY"} for s in dbk["steps"]]
        got = {it["id"]: it for _o, it in r.items()}
        check(got["dbk_meter1_run"]["path"].replace("\\", "/").endswith("dbk_meter1/drive/attempt1/run.log") and
              got["dbk_gate_cpu2"]["path"].replace("\\", "/").endswith("dbk_gate/freeze/drive/attempt1/run.log") and
              got["dbk_gate_span_vblank"]["path"].replace("\\", "/").endswith("dbk_gate/span/drive/attempt1/run.log"),
              "display_registers_bake: the text_le and the gate rolls' items point at their roll's run.log")
    # every older plan RUNS with the gate OFF it ran with (run_gates, where the env is built); display_registers' arms
    # and the bake's as written; no key twice; the dry run shows the pin
    rg = npf.run_gates
    unpinned = [(n, s.tag) for n, p in plans.items() if not p.get("vblank_default") for s in p["steps"]
                if s.kind in ("roll", "gate") and "SR_NATIVE_VBLANK" not in env_of(rg(p, s.gates))]
    # the plans after the bake: display_registers_bake and zpd_bake (native/zpd-bake: zbk1 on the real default build),
    # and zpd_park / zpd_keep (zbk2's / zpk1's follow-ups: information rolls configured as zpd_bake's)
    check(not unpinned and sorted(n for n, p in plans.items() if p.get("vblank_default")) ==
          ["boot_gaps_bake", "display_registers_bake", "memres_ab", "memres_bake", "memres_bounds",
           "memres_readers", "zpd_bake", "zpd_keep", "zpd_park"],
          "every plan before the bake runs each roll / gate with SR_NATIVE_VBLANK named: %s" % unpinned[:6])
    tp = plans["transport_bake"]
    tbk = {s.tag: s for s in tp["steps"]}
    check(all(env_of(rg(tp, s.gates)).get("SR_NATIVE_VBLANK") == "0" for s in tbk.values() if s.kind in ("roll", "gate"))
          and rg(tp, tbk["meter_on"].gates) == npf.VBLANK_OFF and tbk["meter_on"].gates == "" and tp["gates"] == "",
          "transport_bake: every arm runs SR_NATIVE_VBLANK=0 (as tbk1-3 ran), its steps and replay env as written")
    dp = plans["display_registers"]
    dreg = {s.tag: s for s in dp["steps"]}
    check(all(rg(dp, dreg[t].gates) == dreg[t].gates for t in dreg if dreg[t].kind in ("roll", "gate")) and
          all(env_of(dreg[t].gates).get("SR_NATIVE_VBLANK") == "1" for t in ("probe", "abp", "cppm4", "meter1", "gate"))
          and all(env_of(dreg[t].gates).get("SR_NATIVE_VBLANK") == "0" for t in ("meter0", "pace0", "pace0b")),
          "display_registers: its arms keep the =1 / =0 they name (no pin)")
    check(all(rg(dbk, s.gates) == s.gates for s in dbk["steps"]), "display_registers_bake: its arms run as written")
    dup = [(n, s.tag) for n, p in plans.items() for s in p["steps"] if s.kind in ("roll", "gate") and
           sum(1 for kv in (rg(p, s.gates) or "").split(",") if kv.split("=", 1)[0] == "SR_NATIVE_VBLANK") > 1]
    check(not dup, "no roll / gate runs with SR_NATIVE_VBLANK named twice: %s" % dup[:6])
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        r = npf.Runner(Args(plan="transport_bake", out=td, dry_run=True), tp)
        check(npf.VBLANK_OFF in r.gates_for(tbk["meter_on"]).split(",") and
              npf.VBLANK_OFF in r.gates_for(tbk["pace1"]).split(","),
              "transport_bake: the env a roll runs with carries the pin (Runner.gates_for)")
        r.exe = os.path.join(td, "SupermanReturns.exe")  # the dry run prints the command; no exe is read
        r.do_gate(tbk["gate"])
        check(("--gates " + npf.genv("SR_NATIVE_SEG_TILE=1", npf.VBLANK_OFF)) in r.results[-1]["detail"],
              "transport_bake: the gate's two rolls run with the pin (do_gate's --gates)")
        rb = npf.Runner(Args(plan="display_registers_bake", out=td, dry_run=True), dbk)
        check("SR_NATIVE_VBLANK" not in rb.gates_for(st["meter1"]) and
              npf.VBLANK_OFF in rb.gates_for(st["meter0"]).split(","),
              "display_registers_bake: its default arm runs with the key unset, its =0 arm with =0")
    # memres_readers (lane D resproof follow-up): the ending route (era matrix R3) - seed 17, its tape, the level-skip
    # plan with the roll's own output folder, cutscenes never skipped; the readers items gate the whole-run census
    mp = plans["memres_readers"]
    mst = {s.tag: s for s in mp["steps"]}
    end = mst.get("end")
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        rm = npf.Runner(Args(plan="memres_readers", out=td, dry_run=True), mp)
        ge = env_of(rm.gates_for(end)) if end else {}
        gn = env_of(rm.gates_for(mst["nb"]))
    check(end is not None and end.from_save == npf.CH17_SEED and end.play == npf.END_TAPE and
          (end.dwell, end.seconds) == (900, 1800) and end.devtools == npf.END_PLAN and
          ge.get("SR_DEVTOOLS") == "levelskip" and ge.get("SR_DEVTOOLS_CMDS", "").endswith(npf.END_PLAN) and
          ge.get("SR_DEVTOOLS_OUT", "").endswith("_end_devtools") and ge.get("SR_DRIVE_SKIP_CUTSCENES") == "0" and
          ge.get("SR_GUEST_READER_PROBE") == "1" and "SR_DEVTOOLS" not in gn and
          mst["end_readers"].era == "ending" and mst["end_texid"].era == "ending",
          "memres_readers: the ending roll (R3's seed, tape, dwell; SR_DEVTOOLS=levelskip with the plan and the roll's own "
          "output) and its graders on the ending era; the other rolls run without the dev tools")
    rchecks = {c["field"]: (c["op"], c["value"]) for c in npf.memres_readers_item("x")["checks"]}
    check(all(rchecks.get(f) == ("eq", v) for f, v in (("verdict", "CENSUS"), ("complete", True),
                                                       ("copy_gpu_total", 0), ("texlock_gpu_total", 0),
                                                       ("inval_gpu_total", 0), ("other_gpu_total", 0),
                                                       ("lockcore_gpu_total", 0),
                                                       ("gaps_total", 0), ("events_capped", 0))),
          "memres_readers: a readers item needs the complete whole-run census and no reader, intent or gap in any era")


def devcreate_close_checks(plans):
    """native/devcreate-close (2026-10-14): the devcreate_close plan as pre-registered (docs/NATIVE_ROADMAP.md CLOSE
    DESIGN), the boot_attach warm-up, the bootequiv / pooled-meter steps' wiring, the abort rule's `always` step, and
    the ledger routes (resolver_bind, retire_on_pass) on a copy of this checkout's ledger."""
    import boot_equiv as be  # noqa: E402
    import boot_stall as bs  # noqa: E402
    sys.path.insert(0, os.path.dirname(HERE))
    import native_ledger as nl  # noqa: E402
    p = plans["devcreate_close"]
    steps = p["steps"]
    by = {s.tag: s for s in steps}
    idx = {s.tag: i for i, s in enumerate(steps)}
    # since the devcreate bake (native/devcreate-fix) the OFF arm pins =0: the arms dcl ran, on an exe whose default is ON
    gs, gon = "SR_NATIVE_SEGMENTLESS=1,SR_NATIVE_DEVCREATE=0", "SR_NATIVE_SEGMENTLESS=1,SR_NATIVE_DEVCREATE=1"
    check(p["obligations"] == ["boot.device_create"] and p["prefix"] == "dcl" and p.get("no_replay") and p.get("no_ab")
          and not any(s.kind in ("replay", "gate") for s in steps) and p.get("fresh_out") is True,
          "devcreate_close: boot.device_create, prefix dcl, no replay / gate with their reasons stated, a fresh --out")
    check(npf.BOOT_EQUIV_DESIGN == be.DESIGN_ID == "dcl-v2" and npf.BOOT_EQUIV_PAIRS == be.N_PAIRS and
          npf.BOOT_STALL_DESIGN == bs.DESIGN_ID and npf.BOOT_STALL_N_MIN == bs.N_MIN,
          "devcreate_close: the items' designs / sizes are boot_equiv.py's (dcl-v2) and boot_stall.py's")
    eq = by["boot_equiv"]
    check(eq.kind == "bootequiv" and eq.always and eq.warmup == "warmup" and
          [[a for a, _t in pr] for pr in eq.pairs] == be.DESIGN,
          "devcreate_close: the bootequiv step runs always, names the warm-up, declares dcl-v2's alternation")
    # Astra round 1 P1-1: the stall block and its evaluator - every ON boot of the run, 29 of them, counted
    st = by["boot_stall"]
    on_tags = [s.tag for s in steps if s.kind == "roll" and s.gates == gon]
    check(st.kind == "bootstall" and st.always and st.pool == on_tags and len(on_tags) == bs.N_MIN and
          idx["boot_stall"] == len(steps) - 1 and all(idx[t] < idx["boot_stall"] for t in on_tags) and
          [s.tag for s in steps if s.tag.startswith("s") and s.tag.endswith("_on")] ==
          ["s%d_on" % (k + 1) for k in range(npf.BOOT_STALL_EXTRA)] and idx["s1_on"] > idx["meter_pool"] and
          all(by["s%d_on" % (k + 1)].gates == gon and by["s%d_on" % (k + 1)].mode == "plain"
              for k in range(npf.BOOT_STALL_EXTRA)),
          "devcreate_close: 17 stall-block ON boots after the meter, then boot_stall (always) over all 29 ON boots")
    sit = st.ledger[0]
    check(sit["kind"] == "json" and {"field": "verdict", "op": "eq", "value": "BOUNDED"} in sit["checks"] and
          {"field": "tag", "op": "eq", "value": "BOOT_STALL"} in sit["checks"] and
          {"field": "on_boots", "op": "ge", "value": 29} in sit["checks"] and
          {"field": "events", "op": "eq", "value": 0} in sit["checks"],
          "devcreate_close: boot_stall passes only on BOUNDED / BOOT_STALL, 0 events in >= 29 ON boots")
    run_order = [t for pr in eq.pairs for _a, t in pr]
    check(steps[0].tag == "warmup" and steps[0].kind == "roll" and steps[0].gates == gs and
          [s.tag for s in steps[1:1 + len(run_order)]] == run_order and idx["boot_equiv"] == 1 + len(run_order),
          "devcreate_close: the warm-up (the OFF arm) first, the 16 boot rolls in declared order, then the evaluator")
    check(all(by[t].gates == (gon if a == "on" else gs) and by[t].mode == "plain" for pr in eq.pairs for a, t in pr),
          "devcreate_close: the boot arms differ only by SR_NATIVE_DEVCREATE=1 (devcreate_ctl's arms)")
    mp = by["meter_pool"]
    mpairs = [(mp.base, mp.target)] + mp.pool
    check(mp.kind == "meter" and len(mpairs) == 4 and
          [[s.tag for s in steps if s.tag in pr] for pr in mpairs] ==
          [["m1_off", "m1_on"], ["m2_on", "m2_off"], ["m3_off", "m3_on"], ["m4_on", "m4_off"]] and
          all(by[b].gates == gs and by[t].gates == gon for b, t in mpairs) and
          all(idx[t] < idx["meter_pool"] and idx[b] < idx["meter_pool"] for b, t in mpairs) and
          idx["m1_off"] > idx["boot_equiv"],
          "devcreate_close: four meter pairs after the boot set, alternating m1 OFF/ON, m2 ON/OFF, ..., dc3's arms")
    pit = mp.ledger[0]
    check(pit["id"] == "meter_pool" and pit["kind"] == "meter_delta" and pit["field"] == "pm4" and
          pit["max_delta"] == -3 and pit["world_min"] == 0.5 and pit["min_roll_frames"] == 8000 and
          pit["pool_weight"] == "pair" and mp.always and
          pit["provenance"] == {"arm_key": "SR_NATIVE_DEVCREATE", "arm_on": "1", "engaged": npf.DEVCREATE_ENGAGED,
                                "first": "base", "alternate": True,
                                "ignore": ["SR_PERF_DIR", "SR_CONTENT_DIR", "SR_PRESENT_DUMP_DIR"]},
          "devcreate_close: meter_pool - total pm4, the mean of the per-pair deltas, bound -3 kept, world_min 0.5, "
          "every roll >= 8,000 frames, provenance (one exe, engaged ON only, m1 OFF-first alternation), always")
    check(p.get("resolver_bind") == {"boot_equiv": ("boot_order_gap", "boot_order.powered"),
                                     "boot_stall": ("boot_order_gap", "boot_order.stall")} and
          p.get("retire_on_pass") and p["retire_on_pass"][0]["item"] == "meter_pool" and
          p["retire_on_pass"][0]["old"] == "meter" and p.get("supersede_into") == {"meter_pool": "meter_single_pair"},
          "devcreate_close: boot_equiv / boot_stall bind the unknown's two resolvers; meter_pool retires meter only on "
          "a pass")
    r = npf.Runner(Args(plan="devcreate_close"), p)
    gb, gm = r.gates_for(by["b1_on"]).split(","), r.gates_for(by["m1_on"]).split(",")
    check(npf.NO_SPARSE in gb and npf.NO_TIMED in gb and npf.NO_SPARSE not in gm and npf.NO_TIMED in gm and
          npf.NO_SPARSE in r.gates_for(by["warmup"]).split(","),
          "devcreate_close: boot rolls and the warm-up without dumps (dcc4 / bat1's config), meter rolls keep the "
          "sparse dump the world rule counts (a pooled pair feeds the meter)")
    # the ledger's two resolvers are the BAKE plan's since native/devcreate-fix (devcreate_bake_checks reads them); this
    # parked plan's items would not bind them any more (its designs are not theirs). Read at BAKE_FIXTURE_REV: the live
    # ledger has since bound them to dcf2 / dcf3's runs and retired the meter (native/devcreate-post)
    label, led = bake_fixture_doc()
    check(label == "git:" + BAKE_FIXTURE_REV, "the bake's pre-registered ledger is read from git (%s)" % label)
    o = [x for x in led["obligations"] if x["id"] == "boot.device_create"][0]
    ru = [u for u in o["residual_unknowns"] if u["id"] == "boot_order_gap"][0]
    pre = [x for x in ru["resolved_by"] if x["id"] == "boot_order.powered"]
    pst = [x for x in ru["resolved_by"] if x["id"] == "boot_order.stall"]
    check(len(pre) == 1 and pre[0]["checks"] != eq.ledger[0]["checks"] and len(pst) == 1 and
          pst[0]["checks"] != st.ledger[0]["checks"] and any(e["id"] == "meter" for e in o["evidence"]),
          "ledger: the resolvers are no longer devcreate_close's designs (the bake plan's since native/devcreate-fix); "
          "meter still evidence")
    # the boot_attach plan: replay, the warm-up INSIDE the invocation, then the mirror of bat1's order
    ba = [s.tag for s in plans["boot_attach"]["steps"]]
    check(ba == ["replay", "warmup", "off1", "on1", "on2", "off2", "off3", "on3", "gate"] and
          plans["boot_attach"]["steps"][1].gates == "",
          "boot_attach: replay, the warm-up (the default build) inside the invocation, OFF/ON, ON/OFF, OFF/ON, gate")

    os.makedirs(os.path.join(npf.LANE, "archive"), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        # -- the bootequiv step: run-unique JSON, validated, verdict stamped, committed last
        rb = npf.Runner(Args(plan="devcreate_close", out=td, dry_run=False), p)
        js_written = {}

        def fake_eval(doc, rc=0):
            def run(cmd, **kw):
                js = cmd[cmd.index("--json") + 1]
                if doc is not None:
                    d = dict(doc)
                    if d.get("declared") == "OWN":
                        d["declared"] = [[[a, rb.path(t)] for a, t in pr] for pr in eq.pairs]
                    with open(js, "w", encoding="utf-8") as f:
                        json.dump(d, f)
                js_written["js"] = js
                return subprocess.CompletedProcess(cmd, rc, stdout="BOOT_SET design=dcl-v2 pairs=8 -> x\n", stderr="")
            return run
        good = {"design": npf.BOOT_EQUIV_DESIGN, "verdict": "EQUIVALENT", "tag": "BOOT_EQUIV", "pairs": 8,
                "declared": "OWN", "warmup_roll": rb.path("warmup")}
        for doc, rc, want, why in ((good, 0, "PASS", "EQUIVALENT with this step's pairs"),
                                   (dict(good, verdict="OPEN", tag=None), 0, "FAIL", "OPEN"),
                                   (dict(good, verdict="INCONCLUSIVE", tag=None), 0, "FAIL", "INCONCLUSIVE"),
                                   (dict(good, declared=[]), 0, "FAIL", "another set's pairs"),
                                   (dict(good, warmup_roll="elsewhere"), 0, "FAIL", "another warm-up"),
                                   (good, 1, "FAIL", "a nonzero exit after a passing json"),
                                   (None, 0, "FAIL", "no json written (the placeholder)")):
            with patch.object(npf.subprocess, "run", fake_eval(doc, rc)):
                got = rb.do_bootequiv(eq)
            d = json.load(open(js_written["js"], encoding="utf-8"))
            check(got == want and d.get("committed") is True and d["runner"]["verdict"] == want and
                  rb.results[-1]["paths"]["json"].endswith(os.path.basename(js_written["js"])),
                  "bootequiv step, %s: %s, the run-unique json stamped %s and committed" % (why, want, want))
        its = {it["id"]: it for _o, it in rb.items()}
        check(its["boot_equiv"]["path"].endswith(os.path.basename(js_written["js"])),
              "bootequiv: the item names the last run-unique json")
        # -- the bootstall step: this run's proof JSON and exactly the ON rolls it took; BOUNDED alone passes
        rows_on = [{"step": t, "kind": "roll", "verdict": "DONE", "detail": "", "paths": {"roll": rb.rel(rb.path(t))}}
                   for t in st.pool[:20]]  # 20 ran (the others never did: no row)
        rb.results += rows_on
        seen_cmd = {}

        def fake_stall(doc, rc=0):
            def run(cmd, **kw):
                js = cmd[cmd.index("--json") + 1]
                seen_cmd["on"] = [cmd[i + 1] for i, x in enumerate(cmd) if x == "--on"]
                seen_cmd["proof"] = [cmd[i + 1] for i, x in enumerate(cmd) if x == "--proof"]
                if doc is not None:
                    d = dict(doc)
                    if d.get("on") == "OWN":
                        d["on"] = seen_cmd["on"]
                    with open(js, "w", encoding="utf-8") as f:
                        json.dump(d, f)
                js_written["js"] = js
                return subprocess.CompletedProcess(cmd, rc, stdout="BOOT_STALL_SET x\n", stderr="")
            return run
        sgood = {"design": npf.BOOT_STALL_DESIGN, "verdict": "BOUNDED", "tag": "BOOT_STALL", "on_boots": 29,
                 "events": 0, "on": "OWN", "proofs": [rb.path("proof", ".json")]}
        for doc, rc, want, why in ((sgood, 0, "PASS", "BOUNDED over this run's ON boots"),
                                   (dict(sgood, verdict="OPEN", tag=None, events=1), 0, "FAIL", "OPEN"),
                                   (dict(sgood, verdict="INCONCLUSIVE", tag=None), 0, "FAIL", "INCONCLUSIVE"),
                                   (dict(sgood, on_boots=28), 0, "FAIL", "28 ON boots"),
                                   (dict(sgood, on=[]), 0, "FAIL", "another set of ON boots"),
                                   (dict(sgood, proofs=["elsewhere_proof.json"]), 0, "FAIL", "another run's proof"),
                                   (sgood, 1, "FAIL", "a nonzero exit"),
                                   (None, 0, "FAIL", "no json written")):
            with patch.object(npf.subprocess, "run", fake_stall(doc, rc)):
                got = rb.do_bootstall(st)
            d = json.load(open(js_written["js"], encoding="utf-8"))
            check(got == want and d.get("committed") is True and d["runner"]["verdict"] == want and
                  len(seen_cmd["on"]) == 20 and seen_cmd["proof"] == [rb.path("proof", ".json")],
                  "bootstall step, %s: %s, over the 20 ON rolls that ran and this run's proof" % (why, want))
        rb.results = [x for x in rb.results if x not in rows_on]

        # -- the pooled meter step (Astra round 1 P1-4): graded by the ledger's own check (patched here), a run-unique
        # receipt naming every roll's row and file hashes, committed last; a crashed meter roll is a FAIL with an item
        mtags = [t for pr in mpairs for t in pr]
        with patch.object(npf.subprocess, "run", lambda *a, **k: (_ for _ in ()).throw(AssertionError("ran"))):
            check(rb.do_meter(mp) == "SKIP" and "no meter roll ran" in rb.results[-1]["detail"],
                  "pooled meter step: none of its rolls ran -> SKIP (nothing measured), nothing launched")
        rows_m = {t: {"step": t, "kind": "roll", "verdict": "DONE", "detail": "", "paths": {"roll": rb.rel(rb.path(t))}}
                  for t in mtags}
        rb.results += list(rows_m.values())
        fake_md = lambda cmd, **kw: subprocess.CompletedProcess(cmd, 0, stdout="B - A per_frame: pm4=-6.0\n", stderr="")
        for res, want in (("pass", "PASS"), ("fail", "FAIL"), ("unavailable", "FAIL")):
            with patch.object(npf.subprocess, "run", fake_md), \
                    patch.object(npf, "ledger_check", lambda it, *a, **k: (res, "pm4 pooled over 4 pairs")):
                got = rb.do_meter(mp)
            rec = json.load(open(os.path.join(npf.UMBRELLA, rb.results[-1]["paths"]["json"]), encoding="utf-8"))
            check(got == want and rec.get("committed") is True and rec["runner"]["verdict"] == want and
                  sorted(rec["rolls"]) == sorted(rb.rel(rb.path(t)) for t in mtags) and
                  rec["pool"][0] == [rb.rel(rb.path("m1_off")), rb.rel(rb.path("m1_on"))],
                  "pooled meter step: the ledger's %s -> %s, its receipt committed with every roll" % (res, want))
        rows_m["m2_on"]["verdict"] = "ABORT"  # the meter roll crashed (THREAD_FATAL): its folder is there
        with patch.object(npf.subprocess, "run", fake_md), \
                patch.object(npf, "ledger_check", lambda it, *a, **k: ("pass", "pm4 pooled over 4 pairs")):
            got = rb.do_meter(mp)
        rec_rel = rb.results[-1]["paths"]["json"]
        rec = json.load(open(os.path.join(npf.UMBRELLA, rec_rel), encoding="utf-8"))
        its = {i["id"]: i for _o, i in rb.items()}
        check(got == "FAIL" and "m2_on: its run row says ABORT" in rb.results[-1]["detail"] and
              rec["runner"]["verdict"] == "FAIL" and its["meter_pool"]["receipt"] == rec_rel,
              "pooled meter step: a crashed meter roll -> FAIL (even when the pool's numbers pass), the item names the "
              "receipt")
        rows_m["m2_on"]["verdict"] = "DONE"
        rb.results = [x for x in rb.results if x not in rows_m.values()]
        it = rb.pooled_item(mp, mp.ledger[0], "x/dcl_meter_pool.1.json")
        check(it["path"].endswith("dcl_m1_on") and it["base"].endswith("dcl_m1_off") and
              [x["path"][-9:] for x in it["pool"]] == ["dcl_m2_on", "dcl_m3_on", "dcl_m4_on"] and
              [x["base"][-10:] for x in it["pool"]] == ["dcl_m2_off", "dcl_m3_off", "dcl_m4_off"] and
              it["receipt"] == "x/dcl_meter_pool.1.json" and
              not nl.item_errors("t", it, {"umbrella": "", "repo": ""}, set()),
              "pooled item: pair 1 as path / base, three pairs in pool, the receipt, schema-clean")
        check(nl.item_errors("t", dict(it, pool_weight="median"), {"umbrella": "", "repo": ""}, set()) and
              nl.item_errors("t", dict(mp.ledger[0], kind="meter", receipt="x"), {"umbrella": "", "repo": ""}, set()),
              "pooled item schema: an unknown pool_weight, a receipt on an unpooled item are refused")

        # -- fresh_out (P1-4): a live run into an --out holding this plan's proof JSON or a roll folder is refused
        rf = npf.Runner(Args(plan="devcreate_close", out=td, dry_run=False, slot_held=True), p)
        try:
            with patch.object(rf, "write_proof", lambda: None):
                rf.run()
            check(False, "fresh_out: a live run into a used --out is refused")
        except SystemExit as e:
            check("REFUSED" in str(e) and "fresh --out" in str(e) and "dcl_proof.json" in str(e),
                  "fresh_out: a live run into a used --out is refused (%s)" % str(e)[:80])

        # -- the abort rule: a crashed boot roll ends the plan; boot_equiv and boot_stall (always) still grade; the
        #    meter pool, none of whose rolls ran, skips
        ra = npf.Runner(Args(plan="devcreate_close", out=os.path.join(td, "abort1"), dry_run=False, slot_held=True), p)
        ran = []

        def roll(step):
            ran.append(step.tag)
            if step.tag == ra.crash_at:
                ra.abort = "THREAD_FATAL in " + step.tag
                return ra.record(step, "ABORT", ra.abort, {"roll": ra.rel(ra.path(step.tag))})
            return ra.record(step, "DONE", "perf_run exit 0", {"roll": ra.rel(ra.path(step.tag))})
        ra.crash_at = "b3_on"
        with patch.object(ra, "do_roll", roll), \
                patch.object(ra, "do_bootequiv", lambda step: ra.record(step, "FAIL", "OPEN: b3_on outcome FAILURE")), \
                patch.object(ra, "do_bootstall", lambda step: ra.record(step, "FAIL", "OPEN: F b3_on")), \
                patch.object(npf.subprocess, "run", lambda *a, **k: (_ for _ in ()).throw(AssertionError("ran"))), \
                patch.object(ra, "write_proof", lambda: None):
            ra.run()
        v = {x["step"]: x["verdict"] for x in ra.results}
        check(ran[-1] == "b3_on" and v["b3_off"] == "SKIP" and v["boot_equiv"] == "FAIL" and v["m1_off"] == "SKIP" and
              v["meter_pool"] == "SKIP" and v["s1_on"] == "SKIP" and v["boot_stall"] == "FAIL",
              "abort rule: after b3_on's crash the later rolls skip, boot_equiv and boot_stall (always) grade, the "
              "meter pool (no meter roll ran) skips")
        # ...a crash INSIDE the meter block: the pool is written (FAIL, with its receipt), never a SKIP without an item
        rm = npf.Runner(Args(plan="devcreate_close", out=os.path.join(td, "abort2"), dry_run=False, slot_held=True), p)
        ran = []
        ra = rm
        rm.crash_at = "m2_on"
        with patch.object(rm, "do_roll", roll), \
                patch.object(rm, "do_bootequiv", lambda step: rm.record(step, "PASS", "EQUIVALENT")), \
                patch.object(rm, "do_bootstall", lambda step: rm.record(step, "FAIL", "OPEN: F m2_on")), \
                patch.object(npf.subprocess, "run", fake_md), \
                patch.object(npf, "ledger_check", lambda it, *a, **k: ("unavailable", "m3 / m4 never ran")):
            os.makedirs(rm.out)
            rm.run()
        v = {x["step"]: x["verdict"] for x in rm.results}
        its = {i["id"]: i for _o, i in rm.items()}
        check(v["m2_on"] == "ABORT" and v["m3_off"] == "SKIP" and v["meter_pool"] == "FAIL" and
              "meter_pool" in its and its["meter_pool"]["receipt"].startswith(rm.rel(rm.out)) and
              v["boot_stall"] == "FAIL",
              "abort rule: a meter roll's crash -> meter_pool FAIL with an item naming its receipt (P1-4)")

        # -- native_ledger's pool: min_roll_frames fails a roll cut short (synthetic meter windows, whole-run era)
        def meter_roll(name, pm4, frames):
            d = os.path.join(td, "mr", name, "drive", "attempt1")
            os.makedirs(d)
            os.makedirs(os.path.join(td, "mr", name, "perf"))
            w = frames // 100
            with open(os.path.join(d, "run.log"), "w", encoding="utf-8") as f:
                for k in range(100):
                    f.write("[2026-10-14 01:%02d:%02d.000] [INFO ] [BOOT] [NATIVE_METER] w=%d 2.00s frames=%d | per_frame "
                            "avg/max | window pm4=%d nrec=0\n" % (k // 60, k % 60, k + 1, w, int(pm4 * w)))
            return "mr/" + name
        pairs_m = [(meter_roll("off%d" % k, 373.0 + (k % 2), 11000), meter_roll("on%d" % k, 367.0 + (k % 2), 11000))
                   for k in range(4)]
        base_it = dict(pit, era="whole", path=pairs_m[0][1], base=pairs_m[0][0],
                       pool=[{"path": t, "base": b} for b, t in pairs_m[1:]])
        base_it.pop("world_min")
        base_it.pop("provenance")  # these synthetic rolls have no provenance.json (provenance: below)
        ctx = nl.Ctx({"roots": {"umbrella": td}}, npf.LANE)
        res, det = nl.check_item(ctx, base_it)
        check(res == nl.PASS and "every roll >= 8000 ingame frames" in det, "ledger pool: four full pairs -6.0 pass (%s)" % det[:90])
        short = meter_roll("on_short", 367.0, 5000)
        res, det = nl.check_item(nl.Ctx({"roots": {"umbrella": td}}, npf.LANE),
                                 dict(base_it, pool=base_it["pool"][:2] + [{"path": short, "base": pairs_m[3][0]}]))
        check(res == nl.FAIL and "min_roll_frames 8000" in det, "ledger pool: a roll cut short fails min_roll_frames (%s)" % det[:90])
        res, _det = nl.check_item(nl.Ctx({"roots": {"umbrella": td}}, npf.LANE), dict(base_it, max_delta=-7))
        check(res == nl.FAIL, "ledger pool: -6.0 fails a -7 bound")

        # -- Astra round 1 P2-5 / P1-4: the pool's statistic, provenance and receipt (synthetic rolls with provenance)
        clock = [0]

        def prov_roll(name, pm4, frames, on, exe="e" * 64, engaged=None, extra_env=None):
            d = os.path.join(td, "pv", name, "drive", "attempt1")
            os.makedirs(d)
            os.makedirs(os.path.join(td, "pv", name, "perf"))
            env = {"SR_NATIVE_SEGMENTLESS": "1", "SR_PERF_DIR": os.path.join(d, "perf")}
            if on:
                env["SR_NATIVE_DEVCREATE"] = "1"
            env.update(extra_env or {})
            with open(os.path.join(d, "provenance.json"), "w", encoding="utf-8") as f:
                json.dump({"exeSha256": exe, "effectiveSrEnv": env}, f)
            h, w = clock[0], frames // 100
            clock[0] += 1
            with open(os.path.join(d, "run.log"), "w", encoding="utf-8") as f:
                if on if engaged is None else engaged:
                    f.write("[2026-10-14 %02d:00:00.000] [INFO ] [BOOT] [NATIVE_DEVCREATE] ring init native: dev=1\n" % h)
                for k in range(100):
                    f.write("[2026-10-14 %02d:%02d:%02d.000] [INFO ] [BOOT] [NATIVE_METER] w=%d 2.00s frames=%d | "
                            "per_frame avg/max | window pm4=%d nrec=0\n" % (h, k // 60, k % 60, k + 1, w, int(pm4 * w)))
            return "pv/" + name

        def prov_pairs(tag, spec, over=None, order=None):
            """spec [(pm4 off, frames off, pm4 on, frames on)] created in the declared alternation (pair 1 OFF/ON,
            pair 2 ON/OFF, ...; order: [True = OFF first] per pair overrides it); over {(k, arm): kwargs}."""
            out = []
            for k, (po, fo, pn, fn) in enumerate(spec):
                mk = {"off": lambda: prov_roll("%s_off%d" % (tag, k), po, fo, False, **(over or {}).get((k, "off"), {})),
                      "on": lambda: prov_roll("%s_on%d" % (tag, k), pn, fn, True, **(over or {}).get((k, "on"), {}))}
                off_first = (k % 2 == 0) if order is None else order[k]
                first = mk["off" if off_first else "on"]()
                second = mk["on" if off_first else "off"]()
                out.append((first, second) if off_first else (second, first))  # (base, target)
            return out

        def pool_item(prs_, **kw):
            it = dict(pit, era="whole", path=prs_[0][1], base=prs_[0][0],
                      pool=[{"path": t, "base": b} for b, t in prs_[1:]])
            it.pop("world_min")
            it.update(kw)
            return it

        def grade_of(it):
            return nl.check_item(nl.Ctx({"roots": {"umbrella": td}}, npf.LANE), it)
        # P2-5: zero-delta pairs with opposite frame counts - the frame-weighted arm means read -33.33 (a false PASS
        # of the -3 bound); the registered statistic, the mean of the per-pair deltas, reads 0 (FAIL)
        zp = prov_pairs("zw", [(400.0, 20000, 400.0, 10000), (300.0, 10000, 300.0, 20000)])
        res_f, det_f = grade_of(pool_item(zp, pool_weight="frame", provenance=None, min_roll_frames=0))
        res_p, det_p = grade_of(pool_item(zp, provenance=None, min_roll_frames=0))
        check(res_f == nl.PASS and "delta -33.33" in det_f and res_p == nl.FAIL and
              "the mean of the per-pair deltas +0.00" in det_p,
              "ledger pool (P2-5): zero-delta pairs - frame-weighted -33.33 passes, the pair mean 0.00 FAILS (%s)" %
              det_p[:120])
        # provenance: one exe, the arm in provenance and engaged in the log (ON only), the declared alternation
        spec = [(373.0, 11000, 367.0, 11000)] * 4
        good = prov_pairs("pg", spec)
        res, det = grade_of(pool_item(good))
        check(res == nl.PASS and "provenance: one exe, SR_NATIVE_DEVCREATE engaged ON only, the declared alternation"
              in det, "ledger pool provenance: the declared pool passes (%s)" % det[:100])
        for tag, kw, word in (("pe", dict(over={(1, "off"): dict(engaged=True)}), "(base): the engagement line"),
                              ("pn", dict(over={(2, "on"): dict(engaged=False)}), "(target): the engagement line"),
                              ("pk", dict(over={(0, "on"): dict(extra_env={"SR_NATIVE_DEVCREATE": "0"})}),
                               "(target) has SR_NATIVE_DEVCREATE=0"),
                              ("px", dict(over={(3, "off"): dict(exe="f" * 64)}), "exe hashes"),
                              ("pc", dict(over={(1, "on"): dict(extra_env={"SR_HIGHFPS": "60"})}),
                               "differs beyond SR_NATIVE_DEVCREATE"),
                              ("po", dict(order=[True, True, True, False]), "pair 2 ran base first (declared target")):
            res, det = grade_of(pool_item(prov_pairs(tag, spec, **kw)))
            check(res == nl.FAIL and word in det, "ledger pool provenance: %s -> FAIL (%s)" % (word, det[:110]))
        # P1-4: the receipt - committed, PASS, this pool's pairs, every roll's files unchanged since the run
        abs_of = lambda rel: os.path.join(td, rel.replace("/", os.sep))
        rec_pairs = [[b, t] for b, t in good]

        def receipt(name, **kw):
            doc = {"committed": True, "runner": {"verdict": "PASS"}, "pool": rec_pairs,
                   "rolls": {r: {"row": "DONE", "files": nl.roll_files(abs_of(r))} for pr in rec_pairs for r in pr}}
            doc.update(kw)
            with open(os.path.join(td, "pv", name), "w", encoding="utf-8") as f:
                json.dump(doc, f)
            return "pv/" + name
        check(npf.roll_files(abs_of(good[0][1])) == nl.roll_files(abs_of(good[0][1])),
              "receipt: native_proof's roll hashes are the ledger's (one function)")
        res, det = grade_of(pool_item(good, receipt=receipt("r_ok.json")))
        check(res == nl.PASS, "ledger pool receipt: a committed PASS receipt of these rolls passes (%s)" % det[:90])
        res, det = grade_of(pool_item(good, receipt=receipt("r_fail.json", runner={"verdict": "FAIL", "why": "m2_on: "
                                                                                    "its run row says ABORT"})))
        check(res == nl.FAIL and "the run's pool step FAIL (m2_on: its run row says ABORT)" in det,
              "ledger pool receipt: the run's FAIL is a FAIL (%s)" % det[:90])
        res, det = grade_of(pool_item(good, receipt=receipt("r_unc.json", committed=False)))
        check(res == nl.FAIL and "uncommitted" in det, "ledger pool receipt: an uncommitted receipt FAILS")
        res, det = grade_of(pool_item(good, receipt=receipt("r_other.json", pool=rec_pairs[::-1])))
        check(res == nl.FAIL and "another pool's" in det, "ledger pool receipt: another pool's receipt FAILS")
        res, det = grade_of(pool_item(good, receipt="pv/never_written.json"))
        check(res == nl.UNAVAIL, "ledger pool receipt: a receipt not on this machine is unavailable, never a pass")
        # a crashed pool whose later rolls never ran: the receipt's FAIL grades FAIL - not 'unavailable'
        gone = prov_pairs("pz", spec)
        rec_pairs_g = [[b, t] for b, t in gone]
        with open(os.path.join(td, "pv", "r_crash.json"), "w", encoding="utf-8") as f:
            json.dump({"committed": True, "runner": {"verdict": "FAIL", "why": "pz_on1: its run row says ABORT"},
                       "pool": rec_pairs_g, "rolls": {}}, f)
        for b, t in gone[2:]:
            shutil.rmtree(abs_of(b))
            shutil.rmtree(abs_of(t))
        res, det = grade_of(pool_item(gone, receipt="pv/r_crash.json"))
        check(res == nl.FAIL and "its run row says ABORT" in det,
              "ledger pool receipt: a crashed pool with rolls never run grades FAIL, not unavailable (%s)" % det[:90])
        # a roll overwritten after the run (a later attempt into the same folder): FAIL
        r_ok = receipt("r_ok2.json")
        with open(os.path.join(abs_of(good[2][0]), "drive", "attempt1", "run.log"), "a", encoding="utf-8") as f:
            f.write("[2026-10-14 23:00:00.000] [INFO ] [BOOT] a later roll\n")
        res, det = grade_of(pool_item(good, receipt=r_ok))
        check(res == nl.FAIL and "changed since its run (run.log)" in det,
              "ledger pool receipt: a roll changed since its run FAILS (%s)" % det[:90])

        # -- the ledger routes on a copy of the PRE-REGISTERED ledger (BAKE_FIXTURE_REV: the resolvers OWED, the single-pair
        # meter in the evidence, no meter_single_pair contradiction yet) - the live file moved past that state with the
        # bake's live sets (native/devcreate-post)
        label, _root, src_text = ledger_at(BAKE_FIXTURE_REV)
        check(label == "git:" + BAKE_FIXTURE_REV, "the apply checks run on the pre-registered ledger from git (%s)" % label)
        lp = os.path.join(td, "native_ledger.json")
        with open(lp, "w", encoding="utf-8", newline="") as f:
            f.write(src_text)
        before = open(lp, encoding="utf-8").read().split("\n")

        def runner_with(n):
            """A devcreate_close run in its own fresh --out dcl<n> (its rolls, receipt and JSONs there)."""
            rr = npf.Runner(Args(plan="devcreate_close", out=os.path.join(npf.LANE, "archive", "v1_native", "dcl%d" % n)), p)
            rr.results = [{"step": "boot_equiv", "kind": "bootequiv", "verdict": "FAIL", "detail": "",
                           "paths": {"json": rr.rel(rr.path("boot_equiv", ".%d.json" % n))}},
                          {"step": "meter_pool", "kind": "meter", "verdict": "FAIL", "detail": "",
                           "paths": {"json": rr.rel(rr.path("meter_pool", ".%d.json" % n))}},
                          {"step": "boot_stall", "kind": "bootstall", "verdict": "FAIL", "detail": "",
                           "paths": {"json": rr.rel(rr.path("boot_stall", ".%d.json" % n))}}]
            return rr

        def ledger_doc():
            return json.load(open(lp, encoding="utf-8"))

        def dc(doc):
            return [x for x in doc["obligations"] if x["id"] == "boot.device_create"][0]

        def unknown_witnesses(o, prefix):
            return [w for u in o["residual_unknowns"] for w in u.get("witness", []) if w["id"].startswith(prefix)]
        # 1. a FAILING pooled run, an OPEN boot set and stall set: all recorded; meter stays; both resolvers bound
        done = npf.apply_ledger(lp, runner_with(1), {}, grade=lambda it: "fail")
        o1 = dc(ledger_doc())
        ev = {e["id"]: e for e in o1["evidence"]}
        res_by = {x["id"]: x for u in o1["residual_unknowns"] for x in u.get("resolved_by", [])}
        rv, rs = res_by["boot_order.powered"], res_by["boot_order.stall"]
        check("meter" in ev and "meter_pool" in ev and len(ev["meter_pool"]["pool"]) == 3 and "boot_equiv" not in ev and
              "boot_stall" not in ev and ev["meter_pool"]["receipt"].endswith("dcl1/dcl_meter_pool.1.json") and
              not any(c["id"] == "meter_single_pair" for c in o1["contradictions"]) and
              rv["path"].endswith("dcl_boot_equiv.1.json") and rv["kind"] == "json" and
              rs["path"].endswith("dcl_boot_stall.1.json") and not nl.schema_errors(ledger_doc()),
              "apply (FAIL): meter_pool (with its receipt) + meter both in the evidence, no retirement; both resolvers "
              "bound to the run (%s)" % [d for d in done if d[1] != "meter_pool"])
        after = open(lp, encoding="utf-8").read().split("\n")
        changed = [ln for ln in before if ln not in set(after)]
        check(len(changed) == 3 and any('"id": "boot_order.powered"' in ln for ln in changed) and
              any('"id": "boot_order.stall"' in ln for ln in changed),
              "apply (FAIL): only the two resolver lines and one comma line changed (%d)" % len(changed))
        # 2. a second failing run applied to a CUSTOM ledger (results_now = {}: Astra round 1 P1-3 - the first pool was
        #    REPLACED): the existing pool is graded directly and moves into meter_single_pair (created), both earlier
        #    resolvers into the unknown's witnesses (expect fail) - nothing dropped
        npf.apply_ledger(lp, runner_with(2), {}, grade=lambda it: "fail")
        o2 = dc(ledger_doc())
        msp = [c for c in o2["contradictions"] if c["id"] == "meter_single_pair"]
        uw = unknown_witnesses(o2, "boot_order.powered_void")
        us = unknown_witnesses(o2, "boot_order.stall_void")
        check(len(msp) == 1 and [w["id"] for w in msp[0]["witness"]] == ["meter_single_pair.meter_pool_void"] and
              msp[0]["witness"][0]["path"].endswith("dcl1/dcl_m1_on") and msp[0]["witness"][0]["expect"] == "fail" and
              msp[0]["resolved_by"][0]["id"] == "meter_single_pair.meter_pool" and
              any(e["id"] == "meter" for e in o2["evidence"]) and
              len(uw) == 1 and uw[0]["expect"] == "fail" and uw[0]["path"].endswith("dcl_boot_equiv.1.json") and
              len(us) == 1 and us[0]["path"].endswith("dcl_boot_stall.1.json") and
              not nl.schema_errors(ledger_doc()),
              "apply (FAIL again, results_now {}): the old pool a witness of meter_single_pair, the old resolvers "
              "witnesses of the unknown (expect fail); meter still evidence (P1-3)")
        # 3. a PASSING pooled run (the ledger's grade): meter retires into meter_single_pair, which the new pool resolves
        npf.apply_ledger(lp, runner_with(3), {("boot.device_create", "meter_pool"): "fail"},
                         grade=lambda it: "pass" if it["id"] == "meter_pool" else "fail")
        o3 = dc(ledger_doc())
        msp = [c for c in o3["contradictions"] if c["id"] == "meter_single_pair"][0]
        wid = {w["id"]: w for w in msp["witness"]}
        uw = [w["id"] for w in unknown_witnesses(o3, "boot_order.powered_void")]
        check("meter" not in [e["id"] for e in o3["evidence"]] and "meter_pool" in [e["id"] for e in o3["evidence"]] and
              "meter_single_pair.meter" in wid and wid["meter_single_pair.meter"]["expect"] == "fail" and
              wid["meter_single_pair.meter"]["path"].endswith("dc3/dc_meter1") and
              "meter_single_pair.meter_pool_void2" in wid and len(msp["resolved_by"]) == 1 and
              uw == ["boot_order.powered_void", "boot_order.powered_void2"] and not nl.schema_errors(ledger_doc()),
              "apply (PASS): meter retired into meter_single_pair (expect fail), resolved by the new meter_pool; every "
              "earlier run kept as a witness")
        # 4. the SAME run re-applied (--reapply after a crash), no grader: replaced in place - no duplicate witness
        n_wit = (len(msp["witness"]), len(unknown_witnesses(o3, "boot_order.")))
        npf.apply_ledger(lp, runner_with(3), {}, grade=lambda it: None)
        o4 = dc(ledger_doc())
        msp4 = [c for c in o4["contradictions"] if c["id"] == "meter_single_pair"][0]
        check((len(msp4["witness"]), len(unknown_witnesses(o4, "boot_order."))) == n_wit and
              [e for e in o4["evidence"] if e["id"] == "meter_pool"][0]["path"].endswith("dcl3/dcl_m1_on"),
              "apply (the same run again): replaced in place, no witness added")
        # 5. a new run with NO grader and results_now {} (fail closed, P1-3): the existing pool is kept as a witness,
        #    never dropped - even the one that passed, since nothing graded it here
        npf.apply_ledger(lp, runner_with(4), {}, grade=lambda it: None)
        o5 = dc(ledger_doc())
        msp5 = [c for c in o5["contradictions"] if c["id"] == "meter_single_pair"][0]
        w5 = [w for w in msp5["witness"] if w["path"].endswith("dcl3/dcl_m1_on")]
        check(len(w5) == 1 and w5[0]["expect"] == "fail" and not nl.schema_errors(ledger_doc()) and
              [e for e in o5["evidence"] if e["id"] == "meter_pool"][0]["path"].endswith("dcl4/dcl_m1_on"),
              "apply (no grader, a new run): the existing pool kept as a witness (fail closed), the new one in the "
              "evidence")
        # ...and a positively passing existing pool, graded directly, is replaced (nothing failing is dropped)
        npf.apply_ledger(lp, runner_with(5), {}, grade=lambda it: "pass" if it["path"].endswith("dcl4/dcl_m1_on")
                         else "fail")
        o6 = dc(ledger_doc())
        msp6 = [c for c in o6["contradictions"] if c["id"] == "meter_single_pair"][0]
        check(not any(w["path"].endswith("dcl4/dcl_m1_on") for w in msp6["witness"]) and
              len(msp6["witness"]) == len(msp5["witness"]),
              "apply: an existing pool the ledger grades PASS is replaced, not kept as a witness")
        # the ledger tool evaluates the edited record (trimmed to it: no dependency, the new item as its evidence):
        # meter_single_pair stays open while the pool's rolls are not on disk, boot_order_gap while its run is absent
        doc = ledger_doc()
        rec = dc(doc)
        rec["depends_on"], rec["evidence"] = [], [e for e in rec["evidence"] if e["id"] == "meter_pool"]
        doc["obligations"] = [rec]
        rdc = nl.evaluate(doc, nl.Ctx(doc, npf.LANE))[0]
        check(rdc["status"] != "closed" and
              any(c["id"] == "meter_single_pair" and not c["resolved"] for c in rdc["contradictions"]) and
              any(u["id"] == "boot_order_gap" and not u["resolved"] for u in rdc["residual_unknowns"]),
              "the ledger tool evaluates the edited record: meter_single_pair and boot_order_gap open (%s)" % rdc["status"])


def devcreate_bake_checks(plans):
    """native/devcreate-fix (2026-10-15): the devcreate_bake plan as pre-registered (docs/NATIVE_ROADMAP.md
    boot.device_create BAKE PLAN): one exe into a fresh --out; the warm-up then four alternating default / =0 pairs that
    feed both the paired boot design dcf-v1 and the pooled zero-form meter (one configuration); the bake's standard
    items; the strict census pair; the stall evaluator (dcf-stall-v1) over every ON boot of the run; the designs the
    tools register; the ledger routes and pre-registration (the two resolvers, default_bake, the default_on items)."""
    import boot_equiv as be  # noqa: E402
    import boot_stall as bs  # noqa: E402
    p = plans["devcreate_bake"]
    steps = p["steps"]
    by = {s.tag: s for s in steps}
    idx = {s.tag: i for i, s in enumerate(steps)}
    off = "SR_NATIVE_DEVCREATE=0"
    check(p["obligations"] == ["boot.device_create"] and p["prefix"] == "dcf" and p.get("bake") is True and
          p.get("same_exe") is True and p.get("fresh_out") is True and not p.get("exe_pin") and p["gates"] == "",
          "devcreate_bake: boot.device_create, prefix dcf, a same-exe bake into a fresh --out, no pin, no env")
    check(be.DESIGNS[npf.BOOT_EQUIV_BAKE_DESIGN] == {"pairs": npf.BOOT_EQUIV_BAKE_PAIRS, "baked": True} and
          bs.DESIGNS[npf.BOOT_STALL_BAKE_DESIGN] == {"n_min": npf.BOOT_STALL_BAKE_N_MIN, "baked": True} and
          npf.BOOT_STALL_BAKE_DESIGN == "dcf-stall-v2" and npf.BOOT_STALL_BAKE_N_MIN == 29 and
          "dcf-stall-v1" not in bs.DESIGNS and npf.BOOT_STALL_BAKE_EXTRA == 20 and
          set(npf.BOOT_STALL_INSTRUMENTS) <= set(bs.INSTRUMENT_KEYS) and
          be.DESIGNS["dcl-v2"] == {"pairs": 8, "baked": False} and bs.DESIGNS["dcl-stall-v1"] == {"n_min": 29, "baked": False},
          "devcreate_bake: the designs the tools register (dcf-v1 4 pairs / dcf-stall-v2 >= 29, baked; v1 withdrawn; "
          "dcl's unchanged)")
    stall = ["s%d_on" % (k + 1) for k in range(npf.BOOT_STALL_BAKE_EXTRA)]
    order = ["replay", "warmup", "p1_on", "p1_off", "p2_off", "p2_on", "p3_on", "p3_off", "p4_off", "p4_on", "boot_equiv",
             "meter_pool", "chk", "abp", "cppm4", "census0", "census1", "census"] + stall + ["gate", "boot_stall"]
    check([s.tag for s in steps] == order,
          "devcreate_bake: replay, the warm-up, the pairs ON/OFF OFF/ON ON/OFF OFF/ON, the two evaluators, chk, abp, "
          "cppm4, the census pair + step, the stall block s1_on..s20_on, the gate, boot_stall last")
    check(all(by[t].gates == "" and by[t].mode == "plain" and not by[t].sparse and
              {"dcf_%s_dc" % t, "dcf_%s_ring0" % t, "dcf_%s_seg" % t} == {i["id"] for i in by[t].ledger} for t in stall),
          "devcreate_bake: every stall-block boot is the default build, plain, with the strict witness and the counters")
    eq = by["boot_equiv"]
    design = [["on", "off"] if k % 2 == 0 else ["off", "on"] for k in range(npf.BOOT_EQUIV_BAKE_PAIRS)]
    check(eq.kind == "bootequiv" and eq.always and eq.warmup == "warmup" and eq.design == npf.BOOT_EQUIV_BAKE_DESIGN and
          [[a for a, _t in pr] for pr in eq.pairs] == design and
          {"field": "design", "op": "eq", "value": "dcf-v1"} in eq.ledger[0]["checks"] and
          {"field": "pairs", "op": "eq", "value": 4} in eq.ledger[0]["checks"] and eq.ledger[0]["id"] == "dcf_boot_equiv",
          "devcreate_bake: the bootequiv step runs always, names the warm-up, declares dcf-v1's 4-pair alternation")
    check(all(by[t].gates == ("" if a == "on" else off) and by[t].mode == "plain" for pr in eq.pairs for a, t in pr) and
          by["warmup"].gates == "" and by["warmup"].mode == "plain" and by["warmup"].sparse is True,
          "devcreate_bake: the pair arms differ only by SR_NATIVE_DEVCREATE=0; the warm-up is the default build")
    r = npf.Runner(Args(plan="devcreate_bake"), p)
    gw, gon, goff = r.gates_for(by["warmup"]), r.gates_for(by["p1_on"]), r.gates_for(by["p1_off"])
    check(gw == gon and sorted(set(goff.split(",")) ^ set(gon.split(","))) == [off] and npf.NO_SPARSE not in gon and
          npf.NO_TIMED in gon and npf.NO_SPARSE in r.gates_for(by["census1"]),
          "devcreate_bake: the warm-up and the pair rolls share one configuration but the arm key (the sparse dump kept "
          "for the meter's world rule); the census rolls drop it")
    on_tags = ["warmup"] + [t for pr in eq.pairs for a, t in pr if a == "on"]
    ids = lambda t: {i["id"] for i in by[t].ledger}
    check(all({"dcf_%s_dc" % t, "dcf_%s_ring0" % t, "dcf_%s_seg" % t} <= ids(t) for t in on_tags) and
          all({"dcf_%s_me_init" % t, "dcf_%s_seg" % t} <= ids(t) for pr in eq.pairs for a, t in pr if a == "off") and
          {"dcf_census1_dc", "dcf_census1_ring0"} <= ids("census1") and "dcf_cppm4_dc" in ids("cppm4") and
          {"dcf_gate_dc", "dcf_gate_ring0", "dcf_gate_span_dc", "dcf_gate_span_ring0"} <= ids("gate"),
          "devcreate_bake: the devcreate witness (ring init, ring at 0) in every ON roll and both gate rolls, the ME_INIT "
          "witness in every =0 roll")
    mp = by["meter_pool"]
    pit = mp.ledger[0]
    check(mp.kind == "meter" and mp.always and [(mp.base, mp.target)] + mp.pool ==
          [("p1_off", "p1_on"), ("p2_off", "p2_on"), ("p3_off", "p3_on"), ("p4_off", "p4_on")] and
          pit["id"] == "dcf_meter_pool" and pit["kind"] == "meter_delta" and pit["field"] == "pm4" and
          pit["max_delta"] == -5.7 and pit["max"] == 0.1 and pit["base_min"] == 5.5 and pit["min_roll_frames"] == 8000 and
          pit["pool_weight"] == "pair" and pit["world_min"] == 0.5 and
          pit["provenance"] == {"arm_key": "SR_NATIVE_DEVCREATE", "arm_on": "1", "arm_unset": "1",
                                "engaged": npf.DEVCREATE_ENGAGED, "first": "target", "alternate": True,
                                "ignore": ["SR_PERF_DIR", "SR_CONTENT_DIR", "SR_PRESENT_DUMP_DIR"]} and
          idx["meter_pool"] > idx["p4_on"],
          "devcreate_bake: the pooled zero-form meter over the four pairs (<= -5.7, every default roll <= 0.1, every =0 "
          "roll >= 5.5, >= 8,000 frames each; the default read as the target when unset, pair 1 target-first)")
    st = by["boot_stall"]
    sit = st.ledger[0]
    # every roll step that boots the default and is neither CHECK nor the A/B, in plan order, then the gate's two boots
    on_steps = [s.tag for s in steps if s.kind == "roll" and off not in s.gates.split(",") and
                "SR_NATIVE_FLUSH_CHECK=1" not in s.gates.split(",") and
                "SR_AB_TOGGLE=SR_NATIVE_DEVCREATE" not in s.gates.split(",")] + ["gate/freeze", "gate/span"]
    check(st.kind == "bootstall" and st.always and st.design == npf.BOOT_STALL_BAKE_DESIGN and
          st.pool == ["warmup", "p1_on", "p2_on", "p3_on", "p4_on", "cppm4", "census1"] + stall + ["gate/freeze", "gate/span"]
          == on_steps and len(st.pool) == npf.BOOT_STALL_BAKE_N_MIN == 29 and st.allow_keys == npf.BOOT_STALL_INSTRUMENTS and
          idx["boot_stall"] == len(steps) - 1 and idx["gate"] == len(steps) - 2 and sit["id"] == "dcf_boot_stall" and
          {"field": "on_boots", "op": "ge", "value": npf.BOOT_STALL_BAKE_N_MIN} in sit["checks"] and
          {"field": "design", "op": "eq", "value": "dcf-stall-v2"} in sit["checks"] and
          {"field": "events", "op": "eq", "value": 0} in sit["checks"] and
          "C (the WP1 wait > 1.0 s) / H (the HUD after 95 s)" in sit["note"],
          "devcreate_bake: boot_stall (always, last) over every roll step that boots the default and is neither CHECK "
          "nor the A/B, plus the gate's two boots - 29 - with the plan's instruments declared")
    r0 = npf.Runner(Args(plan="devcreate_bake"), p)
    check(os.path.normcase(r0.boot_path("gate/freeze")) == os.path.normcase(os.path.join(r0.path("gate"), "freeze")) and
          os.path.normcase(r0.boot_path("s3_on")) == os.path.normcase(r0.path("s3_on")),
          "devcreate_bake: a gate boot's pool tag resolves under the gate step's folder (Runner.boot_path)")
    ck = by["chk"]
    check([i["id"] for i in ck.ledger] == ["dcf_chk", "dcf_chk_k8", "dcf_chk_k9"] and
          ck.ledger[0]["pattern"] == "BAD bytes=0 len=0 dev=0 " and
          all(i["kind"] == "text" and i["which"] == "last" and i["select"] == r"\[NATIVE_FLUSH\] periodic CHECK"
              for i in ck.ledger) and
          "k8=[1-9]" in ck.ledger[1]["pattern"] and "k9=[1-9]" in ck.ledger[2]["pattern"] and
          all(i["pattern"].endswith("/0(?: |$)") for i in ck.ledger[1:]),
          "devcreate_bake: the dry CHECK needs no BAD bracket AND a completed, clean comparison of EACH emitter kind (8, 9)")
    c0, c1, cs = by["census0"], by["census1"], by["census"]
    check(npf.step_graceful(c0) and npf.step_graceful(c1) and
          "SR_NATIVE_BRACKETS=1" in c0.gates.split(",") and "SR_NATIVE_BRACKETS=1" in c1.gates.split(",") and
          off in c0.gates.split(",") and off not in c1.gates.split(",") and
          cs.kind == "tcensus" and cs.mode == "pm4zero_strict" and cs.base == "census0" and cs.target == "census1" and
          cs.ledger[0]["id"] == "dcf_census" and
          {"field": "checks.base_devcreate_exercised", "op": "eq", "value": True} in cs.ledger[0]["checks"] and
          {"field": "target.pm4_zero.allow", "op": "eq", "value": []} in cs.ledger[0]["checks"] and
          {"field": "target.pm4_zero.ingame_pk", "op": "eq", "value": 0} in cs.ledger[0]["checks"] and
          "dcf_census0_me_init" in ids("census0") and {"dcf_census0_armed", "dcf_census1_armed"} <= ids("census0") | ids("census1"),
          "devcreate_bake: the census pair (=0 vs the default, the producer census, both by the graceful quit) graded "
          "with NO allowance, the base showing the re-emit")
    check(by["chk"].gates == "SR_NATIVE_FLUSH_CHECK=1" and by["abp"].gates == "SR_AB_TOGGLE=SR_NATIVE_DEVCREATE,SR_AB_PRESENT=1"
          and by["abp"].mode == "freeze" and by["cppm4"].gates == npf.CPPM4 and by["cppm4"].mode == "freeze" and
          by["gate"].kind == "gate" and by["gate"].gates == "" and by["replay"].kind == "replay",
          "devcreate_bake: the dry CHECK, the per-present A/B (freeze), CPPM4 (freeze), the plain gate, the replay")
    all_ids = [i["id"] for s in steps for i in s.ledger]
    res = p["resolves"]
    check(len(res) == 1 and res[0][0] == "boot.device_create" and res[0][1] == "default_bake" and
          sorted(res[0][2]) == sorted(i for i in all_ids if i not in ("dcf_boot_equiv", "dcf_boot_stall")) and
          p.get("resolver_bind") == {"boot_equiv": ("boot_order_gap", "boot_order.powered"),
                                     "boot_stall": ("boot_order_gap", "boot_order.stall")} and
          p["retire_on_pass"][0]["item"] == "dcf_meter_pool" and p["retire_on_pass"][0]["old"] == "meter" and
          p["retire_on_pass"][0]["into"] == "meter_single_pair" and
          p.get("supersede_into") == {"dcf_meter_pool": "meter_single_pair"},
          "devcreate_bake: every item but the two resolvers resolves default_bake; the resolvers bind the unknown's two; "
          "the pooled meter retires the single pair on a pass")
    os.makedirs(os.path.join(npf.LANE, "archive"), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        rd = npf.Runner(Args(plan="devcreate_bake", out=td, dry_run=True), p)
        ec = rd.bootequiv_cmd(eq, "x.json")
        sc = rd.bootstall_cmd(st, "y.json", st.pool)
        gate_on = [x for x in sc if os.path.normcase(x).startswith(os.path.normcase(rd.path("gate") + os.sep))]
        check(ec[ec.index("--design") + 1] == "dcf-v1" and ec.count("--pair") == 4 and "--warmup" in ec and
              sc[sc.index("--design") + 1] == "dcf-stall-v2" and sc.count("--allow-key") == len(npf.BOOT_STALL_INSTRUMENTS)
              and sc.count("--on") == 29 and all(k in sc for k in npf.BOOT_STALL_INSTRUMENTS) and
              sorted(os.path.basename(x) for x in gate_on) == ["freeze", "span"],
              "devcreate_bake: the evaluators are run with their designs, the warm-up, the 4 pairs, the 29 ON boots (the "
              "gate's two under the gate folder) and the declared instruments")
        got = rd.do_tcensus(cs)
        check(got == "DRY" and "--pm4-zero --base-shows devcreate" in rd.results[-1]["detail"] and
              "--allow-devcreate-reemit" not in rd.results[-1]["detail"],
              "devcreate_bake: the census step runs --pm4-zero --base-shows devcreate (no allowance)")
    # the ledger as pre-registered (BAKE_FIXTURE_REV - the live file holds the bake's results since dcf2 / dcf3): the
    # resolvers in the bake evaluators' form (OWED), default_bake open with no resolvers (the plan's `resolves` attaches
    # them at apply), the two default_on items, the single-pair meter kept
    label, led = bake_fixture_doc()
    check(label == "git:" + BAKE_FIXTURE_REV, "devcreate_bake: the pre-registered ledger is read from git (%s)" % label)
    o = [x for x in led["obligations"] if x["id"] == "boot.device_create"][0]
    ru = [u for u in o["residual_unknowns"] if u["id"] == "boot_order_gap"][0]
    pre = [x for x in ru["resolved_by"] if x["id"] == "boot_order.powered"]
    pst = [x for x in ru["resolved_by"] if x["id"] == "boot_order.stall"]
    db = [u for u in o["residual_unknowns"] if u["id"] == "default_bake"]
    don = [e for e in o["evidence"] if e["id"].startswith("default_on")]
    check(len(pre) == 1 and pre[0]["kind"] == "json" and pre[0]["checks"] == eq.ledger[0]["checks"] and
          pre[0]["note"].startswith("OWED") and len(pst) == 1 and pst[0]["checks"] == sit["checks"] and
          pst[0]["note"].startswith("OWED") and len(ru["resolved_by"]) == 2 and len(db) == 1 and
          not db[0].get("resolved_by") and any(e["id"] == "meter" for e in o["evidence"]) and len(don) == 2 and
          all(e["kind"] == "source" and "SR_NATIVE_DEVCREATE" in e["pattern"] for e in don) and
          "ROOT CAUSE" in ru["text"],
          "ledger (at %s): boot_order.powered (dcf-v1) and boot_order.stall (dcf-stall-v2) pre-registered OWED with the "
          "bake items' checks, the unknown's text holds the root cause, default_bake open, two default_on items, meter "
          "kept" % BAKE_FIXTURE_REV)


def zpd_bake_checks(plans):
    """p2.zpd's bake (native/zpd-bake, 2026-10-16): SR_NATIVE_ZPD default ON in the latency model. The plan's shape (one
    exe into a fresh --out, no pin, the parent env's SR_NATIVE_ZPD refused; every roll save-started in Metropolis with the
    query precondition; the default arms leave the key unset, the =0 arms ZPD_OFF, each graded pair differing in that key
    only), the strict witness in every roll, the items against lines in the exe's own formats (gfx/native_zpd.cpp's latch
    and census lines), the census step's mode, the meter's zero form, the resolvers' partition against the ledger delta
    draft, and every older plan pinned to the gate OFF it ran with (zpd_gates; with the display-registers bake's pin,
    baked_gates). Since dev e87d89c3 merged in, the plan proves the real default build: SR_NATIVE_VBLANK unset in every
    arm (vblank_default) and refused in the parent env too."""
    import native_ledger as nl  # noqa: E402
    zbk = plans["zpd_bake"]
    st = {s.tag: s for s in zbk["steps"]}
    tags = [s.tag for s in zbk["steps"]]
    check(zbk.get("bake") is True and zbk["obligations"] == ["p2.zpd"] and zbk["prefix"] == "zbk" and
          zbk["gates"] == "" and not zbk.get("exe_pin") and not npf.archived_bases(zbk) and
          zbk.get("same_exe") is True and zbk.get("fresh_out") is True and
          tuple(zbk.get("default_keys") or ()) == ("SR_NATIVE_ZPD", "SR_NATIVE_VBLANK") and
          zbk.get("zpd_default") is True and zbk.get("vblank_default") is True,
          "zpd_bake: a same-exe bake plan of p2.zpd into a fresh --out (no pin), SR_NATIVE_ZPD the default under test on "
          "the real default build (SR_NATIVE_VBLANK's bake default too: neither key in the parent env, neither pinned), "
          "its arms as written (zpd_default, vblank_default)")
    check(tags == ["replay", "gate", "chk", "census0", "probe", "census", "abp", "abp_zpd", "cppm4", "cppm4_zpd",
                   "meter_off", "meter_on", "meter", "pace0", "pace1", "pace0b", "pace0_trace", "pace1_trace",
                   "pace0b_trace", "pacing", "pace1_zpd"],
          "zpd_bake: replay, gate, the dry CHECK, the probe's census pair + step, A/B + its query window, CPPM4 + its "
          "query window, the meter pair + step, the three pacing rolls, their traces, the pacing step and the default's "
          "query window, in that order")
    # Astra round 1 (P2-2): the queries inside the graded windows - zpd_activity over the A/B's and CPPM4's holds
    # (exact edges, >= 1,000 query-bearing compared presents; the A/B both arms) and pace1's graded window; the frozen
    # rolls on the zbk2 schedule (a 2-s hold every 4 s from 141 s on the sim clock, ~HUD+40 s); --recompare re-runs the
    # windows and the meter
    zw = {t: st[t] for t in ("abp_zpd", "cppm4_zpd", "pace1_zpd")}
    check(zw["abp_zpd"].kind == "zpdwin" and zw["abp_zpd"].mode == "freeze_ab" and zw["abp_zpd"].target == "abp" and
          zw["cppm4_zpd"].mode == "freeze" and zw["cppm4_zpd"].target == "cppm4" and
          (zw["pace1_zpd"].mode, zw["pace1_zpd"].target, zw["pace1_zpd"].base) == ("pacing", "pace1", "pace1_trace") and
          "zpdwin" in npf.COMPARISON_KINDS and zbk.get("recompare_extra") == ("meter",) and
          all(env_of(st[t].gates).get("SR_DIAG_SIM_FREEZE") == "2:2000:141" for t in ("abp", "cppm4")),
          "zpd_bake: the query-window steps (A/B freeze_ab, CPPM4 freeze, pace1 pacing against its trace's window), "
          "comparison kinds --recompare re-runs with the meter; the frozen rolls on the zbk2 schedule")
    wi = {it["id"]: it for t in zw for it in st[t].ledger}
    wf = {k: {c["field"]: c for c in v["checks"]} for k, v in wi.items()}
    check(sorted(wi) == ["zbk_abp_window", "zbk_cppm4_window", "zbk_pace1_window"] and
          all(f["committed"]["value"] is True and f["runner.verdict"]["value"] == "PASS" and f["verdict"]["value"] == "PASS"
              for f in wf.values()) and
          wf["zbk_abp_window"]["query_bearing_presents"]["value"] == 1000 and
          wf["zbk_abp_window"]["query_bearing_presents"]["op"] == "ge" and wf["zbk_abp_window"]["ab"]["value"] is True and
          wf["zbk_abp_window"]["checks.both_arms"]["value"] is True and
          wf["zbk_abp_window"]["min_arm"]["value"] == npf.ZPD_AB_MIN_ARM == 250 and
          all(wf[k]["checks.stamps_logged"]["value"] is True and wf[k]["checks.compared_listed"]["value"] is True
              for k in ("zbk_abp_window", "zbk_cppm4_window")) and
          "ab" not in wf["zbk_cppm4_window"] and "checks.both_arms" not in wf["zbk_cppm4_window"] and
          not any("checks.edges_logged" in f for f in wf.values()) and
          wf["zbk_pace1_window"]["checks.every_quarter_active"]["value"] is True,
          "zpd_bake: the window items - committed, the runner's PASS; the A/B >= 1,000 query-bearing COMPARED presents "
          "(Astra round 2: each by its own frame stamp, every one stamped, the pixel grade's compared presents), both "
          "arms >= 250; CPPM4 the same without the arms; pace1 every quarter active")
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        rw = npf.Runner(Args(plan="zpd_bake", out=td, dry_run=True), zbk)
        g_ab = rw.gates_for(st["abp"])
        for t in ("abp_zpd", "cppm4_zpd", "pace1_zpd"):
            rw.do_zpdwin(st[t])
        det = {r["step"]: r["detail"] for r in rw.results}
    check(g_ab.count("SR_DIAG_SIM_FREEZE=") == 1 and "SR_DIAG_SIM_FREEZE=2:2000:141" in g_ab and
          "SR_PRESENT_DUMP_FREEZE=2" in g_ab and
          "freeze --roll" in det["abp_zpd"] and "--ab" in det["abp_zpd"] and "--min-presents 1000" in det["abp_zpd"] and
          "--min-arm 250" in det["abp_zpd"] and "--ab" not in det["cppm4_zpd"] and
          all("--freeze-table " in det[t] and "_%s_freeze.txt" % t.split("_")[0] in det[t]
              for t in ("abp_zpd", "cppm4_zpd")) and
          "pacing --roll" in det["pace1_zpd"] and det["pace1_zpd"].count("zbk_pace1_trace_summary.json") == 1,
          "zpd_bake: the A/B roll runs the zbk2 schedule once (it replaces FREEZE's in place); the window steps' dry "
          "commands (%s)" % det.get("abp_zpd", "")[:80])
    rolls = [s for s in zbk["steps"] if s.kind in ("roll", "gate")]
    pace = ("pace0", "pace1", "pace0b")
    check(all(s.from_save == npf.ZPD_SEED and s.play == npf.ZPD_TAPE and s.require == npf.ZPD_FIRED for s in rolls) and
          all((s.dwell, s.seconds) == ((1200, 2100) if s.tag in pace else (260, 900)) for s in rolls if s.kind == "roll"),
          "zpd_bake: every roll and the gate from the chapter-12 seed on the zpd tape with the [GPU_CP] zpd > 0 "
          "precondition (the pacing rolls long: 1,200 s in play)")
    on_tags, off_tags = ["gate", "chk", "probe", "abp", "cppm4", "meter_on", "pace1"], ["census0", "meter_off", "pace0",
                                                                                       "pace0b"]
    check(all("SR_NATIVE_ZPD" not in env_of(st[t].gates) for t in on_tags) and
          all(env_of(st[t].gates).get("SR_NATIVE_ZPD") == "0" for t in off_tags),
          "zpd_bake: every default arm leaves SR_NATIVE_ZPD unset (the baked default), every =0 arm sets it 0")
    check(not any(k in env_of(st[t].gates) for t in on_tags + off_tags
                  for k in list(npf.TRANSPORT_BAKED) + ["SR_NATIVE_DEVCREATE"]),
          "zpd_bake: no arm sets a transport kind or DEVCREATE (the default path itself)")
    for a, b in (("census0", "probe"), ("meter_off", "meter_on"), ("pace0", "pace1")):
        ea, eb = env_of(st[a].gates), env_of(st[b].gates)
        diff = sorted(k for k in set(ea) | set(eb) if ea.get(k) != eb.get(k))
        check(diff == ["SR_NATIVE_ZPD"] and st[a].mode == st[b].mode and st[a].trace == st[b].trace,
              "zpd_bake: %s / %s differ in SR_NATIVE_ZPD only (%s)" % (a, b, diff))
    check(st["pace0b"].gates == st["pace0"].gates and st["pacing"].calib == "pace0b_trace" and
          (st["pacing"].base, st["pacing"].target) == ("pace0_trace", "pace1_trace") and
          all(env_of(st[t].gates).get("SR_HIGHFPS") == "0" and st[t].trace for t in pace) and
          all(st[t].window == npf.PACE_WINDOW for t in ("pace0_trace", "pace1_trace", "pace0b_trace")) and
          all("SR_NATIVE_SEG_TILE" not in env_of(st[t].gates) for t in pace + ("meter_off", "meter_on", "census0",
                                                                                "probe")),
          "zpd_bake: pacing on the console path, long uninstrumented rolls, =0 / default / =0 calibration; the meter and "
          "census arms uninstrumented too")
    check(env_of(st["abp"].gates).get("SR_AB_TOGGLE") == "SR_NATIVE_ZPD" and
          env_of(st["abp"].gates).get("SR_AB_PRESENT") == "1" and st["abp"].mode == "freeze" and
          st["cppm4"].mode == "freeze" and env_of(st["cppm4"].gates).get("SR_NATIVE_FLUSH_CPPM4") == "1" and
          all(env_of(st[t].gates).get("SR_NATIVE_SEG_TILE") == "1" for t in ("abp", "cppm4", "gate")) and
          env_of(st["chk"].gates).get("SR_NATIVE_FLUSH_CHECK") == "1" and
          all(npf.step_graceful(st[t]) and env_of(st[t].gates).get("SR_NATIVE_BRACKETS") == "1"
              for t in ("census0", "probe")),
          "zpd_bake: the per-present A/B toggles the port (SR_NATIVE_ZPD) alone; CPPM4 on the default; the strict "
          "transport witness on the gate / A/B / CPPM4; the census pair with the producer census and the graceful quit")
    ids = {s.tag: [it["id"] for it in s.ledger] for s in zbk["steps"]}
    items = {it["id"]: it for s in zbk["steps"] for it in s.ledger}
    gate_items = {it["id"]: it for it in st["gate"].ledger}

    def has(tag, *suffixes):
        return all("zbk_%s_%s" % (tag, x) in ids[tag] for x in suffixes)
    check(all(has(t, "default", "zpd") for t in ("probe", "abp", "cppm4", "meter_on", "pace1")) and
          has("chk", "default") and "zbk_chk_zpd" not in ids["chk"] and
          all(has(t, "off") for t in off_tags) and not any("zbk_%s_default" % t in ids[t] for t in off_tags) and
          all(has(t, "tile", "xport") for t in ("abp", "cppm4")) and
          all(gate_items.get("zbk_gate_%s" % w, {}).get("roll") == "freeze" for w in ("default", "zpd", "tile", "xport"))
          and all(gate_items.get("zbk_gate_span_%s" % w, {}).get("roll") == "span" for w in ("default", "zpd", "tile")) and
          gate_items["zbk_gate"].get("require") == npf.ZPD_FIRED,
          "zpd_bake: the strict witness in every roll - the default form + the latency census with the port on each default "
          "roll and both gate rolls (CHECK: the form only - its transport is off), the off witness on each =0 roll, tile + "
          "reclaim on the A/B / CPPM4 rolls and the gate")
    check("portDecl=[1-9]" in items["zbk_abp_zpd"]["pattern"] and
          all("portDecl=0" in items["zbk_%s_zpd" % t]["pattern"] for t in ("probe", "cppm4", "meter_on", "pace1")),
          "zpd_bake: the A/B's census item needs both arms (portDecl > 0), the others no decline")
    ck = st["chk"]
    check([i["id"] for i in ck.ledger][:2] == ["zbk_chk", "zbk_chk_k11"] and "k11=[1-9]" in ck.ledger[1]["pattern"] and
          ck.ledger[1]["pattern"].endswith("/0(?: |$)"),
          "zpd_bake: the dry CHECK needs no BAD bracket AND a completed, clean comparison of 82101E08 (kind 11)")

    # the items on lines in the exe's own formats (gfx/native_zpd.cpp latchRequest / logLine)
    src = open(os.path.join(npf.LANE, "gfx", "native_zpd.cpp"), encoding="utf-8").read()
    check('"[NATIVE_ZPD] SR_NATIVE_ZPD on (the baked default; %s): ' in src and
          '"[NATIVE_ZPD] SR_NATIVE_ZPD off (=%.16s): ' in src and '"[NATIVE_ZPD] SR_NATIVE_ZPD on (=%.16s; %s): ' in src
          and "lagAvgMs=%.1f lagMaxMs=%.1f port=%llu portDecl=%llu" in src,
          "zpd_bake: the synthetic lines below are the formats gfx/native_zpd.cpp prints")
    fired = ("[t] [INFO ] [BOOT] [GPU_CP] census rptr=1 wptr=1 doorbells=9 packets=0 draws=99 isr=9 (faults 0) swaps=120 "
             "waits=9 (blocked 0) scratch=9 memWrites=9 zpd=%d (fake 0) ext=0 badIB=0 desync=0\n")
    on_default = ("[t] [INFO ] [BOOT] [NATIVE_ZPD] SR_NATIVE_ZPD on (the baked default; latency): host D3D12 occlusion "
                  "queries answer EVENT_WRITE_ZPD, 82101E08's packets as records (=0 keeps the guest path)\n")
    on_explicit = "[t] [INFO ] [BOOT] [NATIVE_ZPD] SR_NATIVE_ZPD on (=%s; %s): host D3D12 occlusion queries answer EVENT_WRITE_ZPD\n"
    off_line = ("[t] [INFO ] [BOOT] [NATIVE_ZPD] SR_NATIVE_ZPD off (=0): the guest path - EVENT_WRITE_ZPD stays guest "
                "PM4, no host query answers it\n")

    def zc(mode="latency", port=7184, decl=0, wait="0.0", lost=0, issued=14392, missed=0, over=0, segs=None):
        segs = issued if segs is None else segs
        return ("[t] [INFO ] [BOOT] [NATIVE_ZPD] census mode=%s issued=%d ended=%d drained=%d resolved=%d written=%d "
                "late=%d stale=0 orphan=0 abandoned=0 evicted=0 dropped=0 lost=%d badAddr=0 covered=%d fallback=0 "
                "superseded=0 poolFull=0 missed=%d overSegs=%d flushes=0 flushFail=0 waitMs=%s maxWaitMs=0.00 segs=%d "
                "spp1=%d spp2=0 spp4=0 zero=%d nonzero=0 lastHost=0 lastGuest=0 live=0 inflight=0 scale=1x1 viaPartial=0 "
                "viaPresent=%d probes=%d depthKilled=14319 psKilled=0 noRaster=73 lagAvgMs=37.8 lagMaxMs=127.9 port=%d "
                "portDecl=%d\n" % ((mode,) + (issued,) * 6 + (lost, issued, missed, over, wait, segs) + (issued,) * 4 +
                                   (port, decl)))

    def grade2(item, text):
        with tempfile.TemporaryDirectory() as td:
            open(os.path.join(td, "run.log"), "w", encoding="utf-8").write(text)
            ctx = nl.Ctx({"roots": {"umbrella": td}}, npf.LANE)
            return nl.check_item(ctx, dict(item, path="run.log"))

    def grade(item, text):
        return grade2(item, text)[0]
    dfi, zpi, abi, offi =(items["zbk_probe_default"], items["zbk_probe_zpd"], items["zbk_abp_zpd"],
                           items["zbk_meter_off_off"])
    good = on_default + fired % 7184 + zc()
    check(grade(dfi, good) == nl.PASS and grade(zpi, good) == nl.PASS,
          "zpd_bake: a default roll (the default form, the latency census with the port, queries fired) passes")
    for what, text, item in (("an explicit =2 (SR_NATIVE_ZPD=2 inherited)", on_explicit % ("2", "latency") + fired % 9 +
                              zc(), dfi),
                             ("an explicit =1", on_explicit % ("1", "strict") + fired % 9 + zc("strict"), dfi),
                             ("the off line", off_line + fired % 9, dfi),
                             ("no latch line at all", fired % 9 + zc(), dfi),
                             ("a default and an explicit line", on_default + on_explicit % ("2", "latency") + zc(), dfi),
                             ("a port decline in a default roll", on_default + fired % 9 + zc(decl=3), zpi),
                             ("no port (the packets stayed guest PM4)", on_default + fired % 9 + zc(port=0), zpi),
                             ("the strict mode's census", on_default + fired % 9 + zc("strict"), zpi),
                             ("a CP wait", on_default + fired % 9 + zc(wait="12.5"), zpi),
                             ("a lost query", on_default + fired % 9 + zc(lost=1), zpi),
                             # Astra round 1 P2-1: answers the GPU never measured, though resolved / written count them
                             ("a heap failure (segmentMissed: missed=2, the fallback's visible answers resolved and "
                              "written)", on_default + fired % 9 + zc(missed=2, segs=14390), zpi),
                             ("unmeasured draws (segs=0: every answer the conservative fallback's)",
                              on_default + fired % 9 + zc(segs=0), zpi),
                             ("a query over its segment cap (overSegs=1)", on_default + fired % 9 + zc(over=1), zpi),
                             ("a heap failure in the A/B (both arms ran)",
                              on_default + fired % 9 + zc(decl=4100, missed=1, segs=14391), abi)):
        check(grade(item, text) == nl.FAIL, "zpd_bake: the default roll's witness fails on %s" % what)
    v, det = grade2(zpi, on_default + fired % 0 + zc())
    check(v == nl.FAIL and det.startswith(nl.NO_DATA),
          "zpd_bake: a roll that issued no query is NO DATA for the census witness, never PASS (%s)" % det[:60])
    check(grade(abi, on_default + fired % 9 + zc(decl=4100)) == nl.PASS and
          grade(abi, on_default + fired % 9 + zc(decl=0)) == nl.FAIL,
          "zpd_bake: the A/B's census item passes with both arms (portDecl > 0) and fails with one")
    check(grade(offi, off_line + fired % 24) == nl.PASS, "zpd_bake: an =0 roll (the off line, queries as guest PM4, no "
                                                          "census) passes the off witness")
    for what, text in (("a census line (native answers)", off_line + fired % 24 + zc()),
                       ("the default On line", on_default + fired % 24),
                       ("no latch line", fired % 24)):
        check(grade(offi, text) == nl.FAIL, "zpd_bake: the off witness fails on %s" % what)
    # the graceful census rolls' terminal twins (terminal_items): the latch line has no terminal family (no twin); the
    # census witness reads the snapshot's `[NATIVE_ZPD] terminal mode=latency` line (the CP census family's)
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        rt = npf.Runner(Args(plan="zpd_bake", out=td, dry_run=True, exe=os.path.join(td, "x.exe")), zbk)
        tw = {it["id"]: it for _o, it in rt.terminal_items(st["probe"])}
        tw0 = {it["id"]: it for _o, it in rt.terminal_items(st["census0"])}
    check("zbk_probe_default.terminal" not in tw and "zbk_census0_off.terminal" not in tw0 and
          tw.get("zbk_probe_zpd.terminal", {}).get("select") == r"\[NATIVE_ZPD\] terminal mode=latency " and
          "zbk.probe.terminal" in tw and "zbk.census0.quit_receipt" in tw0 and
          npf.terminal_select(npf.ZPD_SELECT + "strict ") == r"\[NATIVE_ZPD\] terminal mode=strict " and
          npf.ZPD_LATCH_SELECT in npf.ONCE_PER_PROCESS,
          "zpd_bake: the census rolls' terminal twins - none for the latch line (printed once, no terminal family), the "
          "census witness on the snapshot's `[NATIVE_ZPD] terminal mode=latency` line")
    term = zc().replace("[NATIVE_ZPD] census mode=", "[NATIVE_ZPD] terminal mode=")
    twz = tw.get("zbk_probe_zpd.terminal", zpi)
    check(grade(twz, on_default + fired % 9 + zc() + term) == nl.PASS and
          grade(twz, on_default + fired % 9 + zc() + term.replace("portDecl=0", "portDecl=2")) == nl.FAIL,
          "zpd_bake: the census twin grades the terminal snapshot's line (a decline there fails it)")
    for what, bad in (("missed=1", zc(missed=1, segs=14391)), ("segs=0", zc(segs=0)), ("overSegs=1", zc(over=1))):
        tb = bad.replace("[NATIVE_ZPD] census mode=", "[NATIVE_ZPD] terminal mode=")
        check(grade(twz, on_default + fired % 9 + zc() + tb) == nl.FAIL,
              "zpd_bake: the census twin fails on an unmeasured answer in the terminal snapshot (%s) though the periodic "
              "census was clean" % what)

    # the census step: --pm4-zero --base-shows zpd, no allowance; its item reads base_zpd_exercised
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        rd = npf.Runner(Args(plan="zpd_bake", out=td, dry_run=True), zbk)
        got = rd.do_tcensus(st["census"])
        check(got == "DRY" and "--pm4-zero --base-shows zpd" in rd.results[-1]["detail"] and
              "--allow-devcreate-reemit" not in rd.results[-1]["detail"],
              "zpd_bake: the census step runs --pm4-zero --base-shows zpd (no allowance)")
    ci = items["zbk_census"]
    fields = {c["field"]: c for c in ci["checks"]}
    check("checks.base_zpd_exercised" in fields and "checks.base_devcreate_exercised" not in fields and
          fields["target.pm4_zero.ingame_pk"]["value"] == 0 and fields["target.pm4_zero.allow"]["value"] == [] and
          fields["base_zpd.zpd_key_pk"]["op"] == "gt",
          "zpd_bake: the census item needs the base's glow queries and the target's gameplay PM4 0 with no allowance")
    # the meter: the zero form on the pm4 column (the default <= 0.05, delta <= 0); do_meter grades it as the ledger does
    mi = items["zbk_meter"]
    check(mi["kind"] == "meter_delta" and mi["field"] == "pm4" and mi.get("max") == 0.05 and mi.get("max_delta") == 0.0
          and mi.get("world_min") == 0.5 and (st["meter"].base, st["meter"].target) == ("meter_off", "meter_on"),
          "zpd_bake: the meter =0 vs the default - the default <= 0.05 PM4 per ingame frame, delta <= 0")

    # Astra round 1 (P2-4): do_meter grades the ledger's unrounded per-frame values, never meter_delta.py's one-decimal
    # print (OFF 0.004 / ON 0.040 printed "0.0 -> 0.0, +0.0" and passed); the fractional boundaries of both bounds - the
    # delta (<= 0) and the default's maximum (<= 0.05) - and the runner's verdict equal to the ledger's
    one_decimal = "B - A per_frame: pm4=+0.0 nrec=+0.0\n"  # what the transcript would print for every case below
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        for a, b, want, why in ((0.0, 0.0, "PASS", "both zero"),
                                (0.004, 0.0, "PASS", "the =0 arm's glow events, the default none"),
                                (0.04, 0.004, "PASS", "delta -0.036"),
                                (0.004, 0.04, "FAIL", "delta +0.036 (printed +0.0)"),
                                (0.02, 0.02, "PASS", "delta exactly 0"),
                                (0.02, 0.021, "FAIL", "delta +0.001"),
                                (0.06, 0.05, "PASS", "the default exactly at max 0.05"),
                                (0.07, 0.051, "FAIL", "the default 0.051 > max 0.05 with a negative delta"),
                                (0.3, 0.2, "FAIL", "the default 0.2 > max")):
            r = npf.Runner(Args(plan="zpd_bake", out=td, dry_run=False), zbk)
            for tag, v in (("meter_off", a), ("meter_on", b)):
                shutil.rmtree(r.path(tag), ignore_errors=True)
                meter_roll(r.path(tag), {"pm4": v, "nrec": 3400.0}, fired=24)
            with meter_fixture_world(), \
                    patch.object(npf.subprocess, "run", lambda *x, **k: SimpleNamespace(stdout=one_decimal, stderr="",
                                                                                         returncode=0)), \
                    patch.object(r, "write_proof", lambda: None):
                got = r.do_meter(st["meter"])
                led = npf.ledger_check(dict(mi, path=r.rel(r.path("meter_on")), base=r.rel(r.path("meter_off"))))[0]
            check(got == want and (led == nl.PASS) == (want == "PASS"),
                  "zpd_bake: do_meter on =0 %s -> default %s per ingame frame (%s): %s, the ledger agrees (got %s / %s)"
                  % (a, b, why, want, got, led))
        # zbk2's side note (2026-10-16): its meter graded PASS over meter_off NO DATA (the =0 arm issued no query twice)
        # and the item stood as PASS evidence. Now the step inherits its input rolls' NO DATA - never PASS, its item never
        # resolving - and the item carries the precondition on BOTH rolls, so the ledger grades it NO DATA too.
        check(mi.get("require") == npf.ZPD_FIRED and "meter_delta" in nl.REQUIRE_KINDS,
              "zpd_bake: the meter item carries the [GPU_CP] zpd > 0 precondition (meter_delta a precondition kind)")
        for off_v, on_v, want_row, why in (("NO DATA", "DONE", "NO DATA", "=0 roll NO DATA"),
                                           ("DONE", "NO DATA", "NO DATA", "default roll NO DATA"),
                                           ("DONE", "DONE", "PASS", "both rolls with data")):
            r = npf.Runner(Args(plan="zpd_bake", out=td, dry_run=False), zbk)
            for tag in ("meter_off", "meter_on"):
                shutil.rmtree(r.path(tag), ignore_errors=True)
                meter_roll(r.path(tag), {"pm4": 0.0, "nrec": 3400.0}, fired=24)
            r.results = [{"step": "meter_off", "verdict": off_v}, {"step": "meter_on", "verdict": on_v}]
            ran = []
            with meter_fixture_world(), \
                    patch.object(npf.subprocess, "run", lambda *x, **k: ran.append(x) or SimpleNamespace(
                        stdout=one_decimal, stderr="", returncode=0)), \
                    patch.object(r, "write_proof", lambda: None):
                got = r.do_meter(st["meter"])
            check(got == want_row and (("zbk_meter" in r.nodata_ids()) == (want_row == "NO DATA")) and
                  (not ran if want_row == "NO DATA" else bool(ran)),
                  "zpd_bake: the meter over %s: %s (its item %s), nothing graded over a NO DATA roll (got %s)" % (
                      why, want_row, "NO DATA - never resolving" if want_row == "NO DATA" else "graded", got))
        for off_f, on_f, want, why in ((0, 24, False, "the =0 roll issued no query"),
                                       (24, 0, False, "the default roll issued none"),
                                       (24, 24, True, "both issued queries")):
            for tag, f in (("meter_off", off_f), ("meter_on", on_f)):
                shutil.rmtree(os.path.join(td, "lm_" + tag), ignore_errors=True)
                meter_roll(os.path.join(td, "lm_" + tag), {"pm4": 0.0, "nrec": 3400.0}, fired=f)
            r = npf.Runner(Args(plan="zpd_bake", out=td, dry_run=True), zbk)
            with meter_fixture_world():
                v, det = npf.ledger_check(dict(mi, path=r.rel(os.path.join(td, "lm_meter_on")),
                                               base=r.rel(os.path.join(td, "lm_meter_off"))))
            check((v == nl.PASS) == want and (want or det.startswith(nl.NO_DATA)),
                  "zpd_bake: the ledger grades the meter item %s when %s (%s)" % (
                      "PASS" if want else "NO DATA", why, det[:90]))
    # the resolvers: a partition of the plan's items, the unknowns those of the ledger delta draft
    res = {u: e for _o, u, e in zbk["resolves"]}
    pace_tags = ("pace0", "pace1", "pace0b", "pace0_trace", "pace1_trace", "pace0b_trace", "pacing", "pace1_zpd")
    want_pace = [i for t in pace_tags for i in ids[t]]
    check(sorted(res) == ["default_bake", "default_pacing"] and all(o == "p2.zpd" for o, _u, _e in zbk["resolves"]) and
          sorted(res["default_pacing"]) == sorted(want_pace) and
          sorted(res["default_bake"] + res["default_pacing"]) == sorted(items) and
          not set(res["default_bake"]) & set(res["default_pacing"]),
          "zpd_bake: default_bake = every item but the pacing steps', default_pacing = the pacing steps' (a partition)")
    # the unknowns: in the ledger once pre-registered (archive/zbk/ledger_edit_zbk.py), else in the lane's draft (archive/
    # is untracked: a checkout without the draft reads the ledger alone)
    led = json.load(open(os.path.join(npf.LANE, "docs", "native_ledger.json"), encoding="utf-8"))
    rec = [x for x in led["obligations"] if x["id"] == "p2.zpd"][0]
    have = {u["id"] for u in rec.get("residual_unknowns", [])}
    draft_path = os.path.join(npf.LANE, "archive", "zbk", "ledger_delta_draft.json")
    if not set(res) <= have and os.path.exists(draft_path):
        draft = json.load(open(draft_path, encoding="utf-8"))
        have |= {u["id"] for u in draft["residual_unknowns_add"]} if draft.get("obligation") == "p2.zpd" else set()
    if set(res) <= have or rec.get("claimed") != "done":
        check(set(res) <= have, "zpd_bake: the two unknowns the plan resolves are pre-registered (the ledger, or the "
                                "lane's draft before it is applied)")
    else:
        # SUPERSEDED (step 4 M10, 2026-10-18): 679e110c pre-registered default_bake / default_pacing on p2.zpd for this
        # plan, but merge 5efc97b7 (2026-10-16, "Preserve this lane's ledger and roadmap status") kept the release
        # lane's p2.zpd record - claimed done on the zpd8 evidence, residual unknown never_fired only - and the bake was
        # completed by tools/perf/zpd_full_bake.py instead (PROGRESS 2026-10-17). On such a ledger the plan cannot be
        # applied: --apply-ledger must REFUSE it (resolve_unknown raises for an unknown the obligation lacks), never
        # skip it silently or write the unknowns back.
        refused = []
        for uid in sorted(res):
            try:
                npf.LedgerText(open(os.path.join(npf.LANE, "docs", "native_ledger.json"), encoding="utf-8").read()
                               ).resolve_unknown("p2.zpd", uid, [{"id": "probe", "kind": "text", "path": "x"}])
            except KeyError:
                refused.append(uid)
        check(refused == sorted(res), "zpd_bake: superseded on this ledger (p2.zpd claimed done without the plan's "
                                      "unknowns %s): applying it is refused for each (%s)" % (sorted(res), refused))
    # Runner.env_refusal: a live run refuses BEFORE anything else while the parent env carries SR_NATIVE_ZPD (any value)
    # or SR_NATIVE_VBLANK (an inherited =0 would un-bake the display-registers default under the whole set); a dry run
    # only notes it; transport_bake (no default_keys) is unaffected
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        for key, val in (("SR_NATIVE_ZPD", "1"), ("SR_NATIVE_ZPD", "2"), ("SR_NATIVE_ZPD", "0"),
                         ("SR_NATIVE_VBLANK", "0")):
            out = os.path.join(td, "env" + key[10:] + val)
            os.makedirs(out)
            other = "SR_NATIVE_VBLANK" if key == "SR_NATIVE_ZPD" else "SR_NATIVE_ZPD"
            with patch.dict(os.environ, {key: val}):
                os.environ.pop(other, None)
                r = npf.Runner(Args(plan="zpd_bake", out=out, dry_run=False, slot_held=True), zbk)
                asked = []
                try:
                    with patch.object(r, "slot", lambda *a: asked.append(a)), \
                            patch.object(r, "write_proof", lambda: asked.append("write_proof")):
                        r.run()
                    refused = ""
                except SystemExit as e:
                    refused = str(e)
                check("REFUSED" in refused and "%s=%s" % (key, val) in refused and not asked and not os.listdir(out),
                      "zpd_bake: a live run with %s=%s in the parent env is refused before any step, write or "
                      "slot request (%s)" % (key, val, refused[:70]))
                rd = npf.Runner(Args(plan="zpd_bake", out=out, dry_run=True), zbk)
                check(rd.env_refusal() and npf.Runner(Args(plan="transport_bake", out=out, dry_run=True),
                                                      plans["transport_bake"]).env_refusal() == "",
                      "env_refusal: zpd_bake names the key (a dry run notes it); a plan without default_keys ignores it "
                      "(%s=%s)" % (key, val))
        with patch.dict(os.environ, {}):
            os.environ.pop("SR_NATIVE_ZPD", None)
            os.environ.pop("SR_NATIVE_VBLANK", None)
            check(npf.Runner(Args(plan="zpd_bake", out=td, dry_run=True), zbk).env_refusal() == "",
                  "env_refusal: nothing to refuse with SR_NATIVE_ZPD and SR_NATIVE_VBLANK unset")
        # fresh_out: an --only re-run into the set's --out is refused before anything is deleted
        out = os.path.join(td, "used")
        att = os.path.join(out, "zbk_meter_on", "drive", "attempt1")
        os.makedirs(att)
        open(os.path.join(att, "run.log"), "w", encoding="utf-8").write("the graded roll\n")
        open(os.path.join(out, "zbk_proof.json"), "w", encoding="utf-8").write('{"plan": "zpd_bake"}')
        removed = []
        with patch.dict(os.environ, {}):
            os.environ.pop("SR_NATIVE_ZPD", None)
            r = npf.Runner(Args(plan="zpd_bake", out=out, dry_run=False, slot_held=True, only="meter_on"), zbk)
            try:
                with patch.object(npf, "safe_rmtree", lambda *a, **k: removed.append(a)), \
                        patch.object(npf.shutil, "rmtree", lambda *a, **k: removed.append(a)), \
                        patch.object(r, "slot", lambda *a: removed.append(("slot",) + a)):
                    r.run()
                refused = ""
            except SystemExit as e:
                refused = str(e)
        check("REFUSED" in refused and "fresh --out" in refused and "zbk_meter_on" in refused and not removed and
              open(os.path.join(att, "run.log"), encoding="utf-8").read() == "the graded roll\n",
              "zpd_bake: a live --only meter_on run into the set's --out is refused before anything is deleted or written")
    # THE PIN (zpd_gates): every plan before the bake runs SR_NATIVE_ZPD=0 where its step does not name the key - the arm
    # it ran (the gate unset was OFF); the zpd plans' explicit arms and the bake's own arms as written
    r = npf.Runner(Args(plan="transport_bake"), plans["transport_bake"])
    zr = npf.Runner(Args(plan="zpd"), plans["zpd"])
    br = npf.Runner(Args(plan="zpd_bake"), zbk)
    zst = {s.tag: s for s in plans["zpd"]["steps"]}
    tst = {s.tag: s for s in plans["transport_bake"]["steps"]}
    check(env_of(r.gates_for(tst["probe"])).get("SR_NATIVE_ZPD") == "0" and
          env_of(r.gates_for(tst["census0"])).get("SR_NATIVE_ZPD") == "0" and
          env_of(zr.gates_for(zst["off"])).get("SR_NATIVE_ZPD") == "0" and
          env_of(zr.gates_for(zst["on"])).get("SR_NATIVE_ZPD") == "1" and
          env_of(zr.gates_for(zst["lat"])).get("SR_NATIVE_ZPD") == "2" and
          "SR_NATIVE_ZPD" not in env_of(br.gates_for(st["probe"])) and
          env_of(br.gates_for(st["census0"])).get("SR_NATIVE_ZPD") == "0" and
          tst["probe"].gates == "SR_NATIVE_SEG_TILE=1" and zst["off"].gates == "",
          "zpd_gates: older plans' arms pinned to SR_NATIVE_ZPD=0 as they ran (the zpd plan's `off` too), explicit arms "
          "and the bake's own arms as written; the steps keep their gates as written")
    # baked_gates composes the two bakes' pins (the display-registers bake's run_gates, then zpd_gates): an older plan
    # runs both gates OFF as it ran; the zpd plan its SR_NATIVE_ZPD as named and VBLANK OFF (it ran before that bake);
    # every zpd_bake arm - its =0 arms too - leaves SR_NATIVE_VBLANK unset: the real default build, the A/B pairs
    # differing in SR_NATIVE_ZPD only; no key twice anywhere
    check(env_of(r.gates_for(tst["probe"])).get("SR_NATIVE_VBLANK") == "0" and
          env_of(zr.gates_for(zst["lat"])).get("SR_NATIVE_VBLANK") == "0" and
          all("SR_NATIVE_VBLANK" not in env_of(br.gates_for(s)) for s in zbk["steps"] if s.kind in ("roll", "gate")) and
          all(npf.baked_gates(zbk, s.gates) == s.gates for s in zbk["steps"]) and
          npf.baked_gates(plans["transport_bake"], "") == npf.genv(npf.VBLANK_OFF, npf.ZPD_OFF) and
          npf.baked_gates(plans["display_registers_bake"], "") == npf.ZPD_OFF,
          "baked_gates: older plans run SR_NATIVE_VBLANK=0 and SR_NATIVE_ZPD=0 as they ran (display_registers_bake the "
          "ZPD pin only); zpd_bake's arms as written, SR_NATIVE_VBLANK unset in every roll and gate")
    dup = [(n, s.tag, k) for n, p in plans.items() for s in p["steps"] if s.kind in ("roll", "gate")
           for k in ("SR_NATIVE_VBLANK", "SR_NATIVE_ZPD")
           if sum(1 for kv in (npf.baked_gates(p, s.gates) or "").split(",") if kv.split("=", 1)[0] == k) > 1]
    check(not dup, "baked_gates: no roll / gate runs a pinned key twice: %s" % dup[:6])
    unpinned = [(name, s.tag) for name, p in plans.items() if not p.get("zpd_default") for s in p["steps"]
                if s.kind in ("roll", "gate") and "SR_NATIVE_ZPD" not in env_of(npf.zpd_gates(p, s.gates))]
    check(not unpinned, "zpd_gates: every roll / gate step of every pre-bake plan runs with SR_NATIVE_ZPD named "
                        "(%s)" % unpinned[:4])
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        exe = os.path.join(td, "SupermanReturns.exe")
        rg = npf.Runner(Args(plan="transport_bake", out=td, dry_run=True, exe=exe), plans["transport_bake"])
        rg.do_gate(tst["gate"])
        zg_ = npf.Runner(Args(plan="zpd_bake", out=td, dry_run=True, exe=exe), zbk)
        zg_.do_gate(st["gate"])
        check(re.search(r"--gates SR_NATIVE_SEG_TILE=1,SR_NATIVE_VBLANK=0,SR_NATIVE_ZPD=0(?: |$)",
                        rg.results[-1]["detail"]) and
              re.search(r"--gates SR_NATIVE_SEG_TILE=1(?: |$)", zg_.results[-1]["detail"]) and
              "SR_NATIVE_ZPD" not in zg_.results[-1]["detail"] and "SR_NATIVE_VBLANK" not in zg_.results[-1]["detail"],
              "baked_gates: the flicker gate of an older plan runs SR_NATIVE_VBLANK=0 and SR_NATIVE_ZPD=0, the zpd "
              "bake's gate both defaults")


PARK_PRELUDE = ("await", "wait", "keymap", "sample", "mark", "releaseall")


def zpd_park_checks(plans):
    """The parked workload's information plan (zbk2's follow-up, 2026-10-16): no obligation (nothing applied); each roll
    configured exactly as the zpd_bake roll whose witness zbk2 failed (pace1, abp, meter_off: the same baked gates bar
    the trace path, mode, trace, dwell, seconds, precondition, sparse dump), only the seed (chapter 18) and the tape (the
    prelude, then nothing pressed) differ; the window steps grade them with zbk2's own zpd_activity modes."""
    zpk, zbk = plans["zpd_park"], plans["zpd_bake"]
    st, zb = {s.tag: s for s in zpk["steps"]}, {s.tag: s for s in zbk["steps"]}
    check(zpk["obligations"] == [] and zpk["prefix"] == "zpk" and not zpk.get("bake") and not zpk.get("resolves") and
          zpk.get("same_exe") is True and zpk.get("fresh_out") is True and
          zpk.get("zpd_default") is True and zpk.get("vblank_default") is True and
          tuple(zpk.get("default_keys") or ()) == ("SR_NATIVE_ZPD", "SR_NATIVE_VBLANK"),
          "zpd_park: an information plan (no obligation, nothing applied) on one exe into a fresh --out, its arms as "
          "zpd_bake's (neither default key pinned nor inherited)")
    check([s.tag for s in zpk["steps"]] == ["park_pace1", "park_pace1_trace", "park_pace1_zpd", "park_abp",
                                             "park_abp_zpd", "park_off"],
          "zpd_park: pace1 + its trace + its window grade, the A/B + its window grade, the =0 roll")
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        rk = npf.Runner(Args(plan="zpd_park", out=td, dry_run=True), zpk)
        rb = npf.Runner(Args(plan="zpd_bake", out=td, dry_run=True), zbk)

        def env(r, s):
            e = env_of(r.gates_for(s))
            e.pop("SR_P2_TRACE", None)  # the roll's own trace file
            e.pop("SR_PRESENT_DUMP_FREEZE_CAP", None)  # zbk3 dumps 80 a hold (ZPD_DUMP_CAP); zpk1 ran zbk2's 40
            return e
        same = []
        for park, bake in (("park_pace1", "pace1"), ("park_abp", "abp"), ("park_off", "meter_off")):
            a, b = st[park], zb[bake]
            same.append(env(rk, a) == env(rb, b) and (a.mode, a.trace, a.dwell, a.seconds, a.require) ==
                        (b.mode, b.trace, b.dwell, b.seconds, b.require))
        cap_park = env_of(rk.gates_for(st["park_abp"])).get("SR_PRESENT_DUMP_FREEZE_CAP")
        cap_bake = [env_of(rb.gates_for(zb[t])).get("SR_PRESENT_DUMP_FREEZE_CAP") for t in ("abp", "cppm4")]
        same.append(cap_park == "40" and cap_bake == ["80", "80"] and
                    all(rb.gates_for(zb[t]).count("SR_PRESENT_DUMP_FREEZE_CAP=") == 1 for t in ("abp", "cppm4")))
        for t in ("park_pace1_zpd", "park_abp_zpd"):
            rk.do_zpdwin(st[t])
        det = {r["step"]: r["detail"] for r in rk.results}
    check(all(same), "zpd_park: each roll runs exactly as zbk2's pace1 / abp / meter_off (gates, mode, trace, dwell, "
                     "precondition; the A/B with zbk2's 40 dumps a hold, zbk3's frozen rolls 80, the key once): %s" % same)
    rolls = [s for s in zpk["steps"] if s.kind == "roll"]
    check(all(s.from_save == npf.ZPD_PARK_SEED and s.play == npf.ZPD_PARK_TAPE for s in rolls) and
          npf.ZPD_PARK_SEED == "archive/saves/18_ch18_before_twister" and npf.ZPD_PARK_SEED != npf.ZPD_SEED,
          "zpd_park: every roll from the chapter-18 seed on the parked tape")
    check((st["park_pace1_zpd"].mode, st["park_pace1_zpd"].target, st["park_pace1_zpd"].base) ==
          ("pacing", "park_pace1", "park_pace1_trace") and st["park_pace1_trace"].window == npf.PACE_WINDOW and
          (st["park_abp_zpd"].mode, st["park_abp_zpd"].target) == ("freeze_ab", "park_abp") and
          "--ab" in det["park_abp_zpd"] and "--min-presents 1000" in det["park_abp_zpd"] and
          det["park_pace1_zpd"].count("zpk_park_pace1_trace_summary.json") == 1,
          "zpd_park: the window grades are zbk2's (pacing over the 28,672-present window, freeze_ab >= 1,000)")
    tape = open(os.path.join(npf.LANE, npf.ZPD_PARK_TAPE), encoding="utf-8").read().splitlines()
    cmds = [ln.split("#", 1)[0].strip() for ln in tape]
    cmds = [c for c in cmds if c]
    zpd = [ln.split("#", 1)[0].strip() for ln in open(os.path.join(npf.LANE, npf.ZPD_TAPE), encoding="utf-8")]
    check(cmds[:3] == [c for c in zpd if c][:3] and cmds.count("mark dump") == 1 and
          all(c.split()[0] in PARK_PRELUDE for c in cmds) and
          sum(int(c.split()[1]) for c in cmds if c.split()[0] == "wait") >= 200000,
          "zpd_park: the parked tape is the zpd tape's save prelude, its dump mark, then nothing pressed (no key, mouse "
          "or click command) for over 200 s")


KEEP_CMDS = ("await", "wait", "keymap", "sample", "mark", "tap", "releaseall")


def zpd_keep_checks(plans):
    """The keep-alive's information plan (zpk2, zpk1's follow-up, 2026-10-16): no obligation (nothing applied); each roll
    configured exactly as the zpd_bake roll it stands for (pace1, abp - zbk3's, 80 dumps a hold - and meter_off: the same
    baked gates bar the trace path, mode, trace, dwell, seconds, precondition, sparse dump), the seed zpk1's (chapter 18)
    and the tape the keep-alive: the save prelude, the dump mark, then a D-pad up tap through the harness TEST keymap at
    most 2.5 s apart (the game's idle camera engages after 10 s of game time with no pad input) for the longest roll."""
    zpq, zbk = plans["zpd_keep"], plans["zpd_bake"]
    st, zb = {s.tag: s for s in zpq["steps"]}, {s.tag: s for s in zbk["steps"]}
    check(zpq["obligations"] == [] and zpq["prefix"] == "zpk2" and not zpq.get("bake") and not zpq.get("resolves") and
          zpq.get("same_exe") is True and zpq.get("fresh_out") is True and
          zpq.get("zpd_default") is True and zpq.get("vblank_default") is True and
          tuple(zpq.get("default_keys") or ()) == ("SR_NATIVE_ZPD", "SR_NATIVE_VBLANK"),
          "zpd_keep: an information plan (no obligation, nothing applied) on one exe into a fresh --out, its arms as "
          "zpd_bake's (neither default key pinned nor inherited)")
    check([s.tag for s in zpq["steps"]] == ["keep_pace1", "keep_pace1_trace", "keep_pace1_zpd", "keep_abp",
                                             "keep_abp_zpd", "keep_off"],
          "zpd_keep: pace1 + its trace + its window grade, the A/B + its window grade, the =0 roll")
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        rk = npf.Runner(Args(plan="zpd_keep", out=td, dry_run=True), zpq)
        rb = npf.Runner(Args(plan="zpd_bake", out=td, dry_run=True), zbk)

        def env(r, s):
            e = env_of(r.gates_for(s))
            e.pop("SR_P2_TRACE", None)  # the roll's own trace file
            return e
        same = []
        for keep, bake in (("keep_pace1", "pace1"), ("keep_abp", "abp"), ("keep_off", "meter_off")):
            a, b = st[keep], zb[bake]
            same.append(env(rk, a) == env(rb, b) and (a.mode, a.trace, a.dwell, a.seconds, a.require) ==
                        (b.mode, b.trace, b.dwell, b.seconds, b.require))
        same.append(env_of(rk.gates_for(st["keep_abp"])).get("SR_PRESENT_DUMP_FREEZE_CAP") == "80" and
                    rk.gates_for(st["keep_abp"]).count("SR_PRESENT_DUMP_FREEZE_CAP=") == 1)
        same.append(st["keep_off"].sparse is True)  # its sparse frames (as zpk1's park_off): the HUD over the roll
        for t in ("keep_pace1_zpd", "keep_abp_zpd"):
            rk.do_zpdwin(st[t])
        det = {r["step"]: r["detail"] for r in rk.results}
    check(all(same), "zpd_keep: each roll runs exactly as zpd_bake's pace1 / abp / meter_off (gates, mode, trace, dwell, "
                     "precondition; the A/B with zbk3's 80 dumps a hold, the key once; keep_off's sparse frames): %s" %
          same)
    rolls = [s for s in zpq["steps"] if s.kind == "roll"]
    check(all(s.from_save == npf.ZPD_PARK_SEED and s.play == npf.ZPD_KEEP_TAPE for s in rolls) and
          npf.ZPD_KEEP_TAPE not in (npf.ZPD_PARK_TAPE, npf.ZPD_TAPE),
          "zpd_keep: every roll from zpk1's chapter-18 seed on the keep-alive tape")
    check((st["keep_pace1_zpd"].mode, st["keep_pace1_zpd"].target, st["keep_pace1_zpd"].base) ==
          ("pacing", "keep_pace1", "keep_pace1_trace") and st["keep_pace1_trace"].window == npf.PACE_WINDOW and
          (st["keep_abp_zpd"].mode, st["keep_abp_zpd"].target) == ("freeze_ab", "keep_abp") and
          "--ab --min-arm %d" % npf.ZPD_AB_MIN_ARM in det["keep_abp_zpd"] and
          "--min-presents 1000" in det["keep_abp_zpd"] and "zpk2_keep_abp_freeze.txt" in det["keep_abp_zpd"] and
          det["keep_pace1_zpd"].count("zpk2_keep_pace1_trace_summary.json") == 1,
          "zpd_keep: the window grades are zbk3's (pacing over the 28,672-present window; freeze_ab over the compared "
          "presents by their own frame stamps, >= 1,000, each arm >= 250)")
    tape = open(os.path.join(npf.LANE, npf.ZPD_KEEP_TAPE), encoding="utf-8").read().splitlines()
    cmds = [ln.split("#", 1)[0].strip() for ln in tape]
    cmds = [c for c in cmds if c]
    zpd = [ln.split("#", 1)[0].strip() for ln in open(os.path.join(npf.LANE, npf.ZPD_TAPE), encoding="utf-8")]
    mark = cmds.index("mark dump") if "mark dump" in cmds else -1
    first_tap = next((i for i, c in enumerate(cmds) if c.split()[0] == "tap"), -1)
    gaps, run, after = [], 0, 0
    for c in cmds[max(first_tap, 0):]:
        w = c.split()
        if w[0] == "tap":
            gaps.append(run)
            run = int(w[2])
            after += int(w[2])
        elif w[0] == "wait":
            run += int(w[1])
            after += int(w[1])
    longest = max(s.dwell for s in rolls)
    check(cmds[:3] == [c for c in zpd if c][:3] and cmds.count("mark dump") == 1 and
          all(c.split()[0] in KEEP_CMDS for c in cmds) and
          [c for c in cmds if c.split()[0] == "keymap"] == ["keymap legacy"] and
          0 <= cmds.index("keymap legacy") < mark < first_tap and
          all(c == "tap Up 100" for c in cmds if c.split()[0] == "tap") and len(gaps) > 400 and
          max(gaps[1:]) <= 2500 and after >= longest * 1000,
          "zpd_keep: the keep-alive tape is the zpd tape's save prelude, the TEST keymap, its dump mark, then only a "
          "D-pad up tap (100 ms) at most 2.5 s apart (tap start to tap start) for the longest roll's %d s (taps %d, "
          "max gap %s ms, %d ms)" % (longest, len(gaps), max(gaps[1:]) if len(gaps) > 1 else None, after))

def boot_gaps_bake_checks(plans):
    """native/boot-gaps (2026-10-16): the boot_gaps_bake plan as pre-registered (docs/NATIVE_ROADMAP.md "BOOT GAPS BUILD
    NOTE"): one exe into a fresh --out with no inherited gate; the census pair, each graded by era_census.py; the meter
    pair; CHECK kind 10; the A/B, CPPM4 and the gate; the strict witness in every default roll; each item routed to its
    own obligation (Runner.item_oids) and the witness lines graded by the ledger tool on synthetic lines in the exact
    formats kernel/native_display_emit.cpp / native_flush.cpp print - the pre-registered lines pass, each named defect
    fails; the era step's run-unique JSON (its flat summary and the ramp comparison) on a stand-in era_census."""
    sys.path.insert(0, os.path.join(os.path.dirname(HERE)))
    import native_ledger as nl  # noqa: E402
    p = plans["boot_gaps_bake"]
    steps = p["steps"]
    by = {s.tag: s for s in steps}
    bt, bd, br, bz = npf.BOOT_GAPS_OBLIGATIONS
    off = ["SR_NATIVE_DISPLAY=0", "SR_NATIVE_MODE_STATE=0"]
    check(p["obligations"] == [bt, bd, br, bz] and p["prefix"] == "bgb" and p.get("bake") is True and
          p.get("same_exe") is True and p.get("fresh_out") is True and not p.get("exe_pin") and p["gates"] == "" and
          p.get("default_keys") == ["SR_NATIVE_DISPLAY", "SR_NATIVE_MODE_STATE", "SR_NATIVE_VBLANK"] and
          p.get("vblank_default") is True and npf.BOOT_GAPS_OFF.split(",") == off,
          "boot_gaps_bake: four obligations, prefix bgb, a same-exe bake into a fresh --out, no pin, no env, the two gates "
          "and the vblank bake's its default keys, vblank_default")
    check([s.tag for s in steps] == ["replay", "census0", "census1", "era0", "era1", "meter0", "meter1", "chk", "abp",
                                     "cppm4", "gate"],
          "boot_gaps_bake: replay, the census pair, the two era steps, the meter pair, chk, abp, cppm4, the gate")
    g = lambda t: by[t].gates.split(",")
    census = ["SR_NATIVE_BRACKETS=1", npf.GRACEFUL]
    check(sorted(g("census0")) == sorted(off + census) and sorted(g("census1")) == sorted(census) and
          npf.step_graceful(by["census0"]) and npf.step_graceful(by["census1"]) and
          sorted(g("meter0")) == sorted(off) and by["meter1"].gates == "" and
          all(npf.run_gates(p, s.gates) == s.gates and "SR_NATIVE_VBLANK" not in s.gates for s in steps
              if s.kind in ("roll", "gate")) and
          by["chk"].gates == "SR_NATIVE_FLUSH_CHECK=1" and
          by["abp"].gates == "SR_AB_TOGGLE=SR_NATIVE_DISPLAY+SR_NATIVE_MODE_STATE,SR_AB_PRESENT=1" and
          by["abp"].mode == "freeze" and by["cppm4"].gates == npf.CPPM4 and by["cppm4"].mode == "freeze" and
          by["gate"].kind == "gate" and by["gate"].gates == "" and by["replay"].kind == "replay" and
          all(by[t].mode == "plain" for t in ("census0", "census1", "meter0", "meter1", "chk")),
          "boot_gaps_bake: each pair's arms differ only by the two =0 keys; the census pair by the graceful quit with the "
          "producer census, the meter pair on the vblank bake's default (no roll names SR_NATIVE_VBLANK, none pinned to "
          "=0 by run_gates); CHECK, A/B (freeze), CPPM4 (freeze), the plain "
          "gate")
    e0, e1 = by["era0"], by["era1"]
    c0 = {c["field"]: c["value"] for c in e0.ledger[0]["checks"]}
    c1 = {c["field"]: c["value"] for c in e1.ledger[0]["checks"]}
    check(e0.kind == e1.kind == "era" and e0.era == e1.era == "newgame" and e0.base == "census0" and not e0.target and
          e1.base == "census1" and e1.target == "census0" and
          c0.get("roll.verdict") == "FAIL" and c0.get("roll.nonzero_eras") == ["boot"] and
          c0.get("roll.boot_pk") == 529 and c0.get("roll.gap_applied") == 2 and c0.get("roll.boot_first_nonzero") == 1 and
          c1.get("verdict") == "PASS" and c1.get("roll.nonzero_eras") == [] and c1.get("roll.boot_pk") == 0 and
          c1.get("roll.gap_applied") == 0 and c1.get("lut.equal") is True and
          all(c.get(k) is True for c in (c0, c1) for k in ("committed", "roll.checks.C0_usable", "roll.checks.C1_frames",
                                                          "roll.checks.C4_reached", "roll.checks.C6_items_zero",
                                                          "roll.checks.C9_terminal", "roll.checks.C10_identity")) and
          all(c1.get("roll.checks." + k) is True for k in ("C2_run_zero", "C3_era_zero", "C5_gaps_zero")) and
          "era" in npf.COMPARISON_KINDS and "era" in npf.INPUT_KINDS,
          "boot_gaps_bake: era_census newgame over each census roll - the =0 arm FAILs in boot alone (529 packets, 2 gaps, "
          "from frame 1), the default PASSes every era from boot with the ramp equal; both whole, identified, reached")
    oid_of = {i["id"]: i.get("obligation") for s in steps for i in s.ledger}
    check(all(o in p["obligations"] for o in oid_of.values()) and
          {i for i, o in oid_of.items() if o == bd} == {"bgb_census0_mode", "bgb_chk", "bgb_chk_k10", "bgb_chk_disp"} and
          {i for i, o in oid_of.items() if o == br} == {"bgb_meter0_vblank", "bgb_meter0_default", "bgb_meter0_dispreg",
                                                       "bgb_meter0_decl", "bgb_meter0_why", "bgb_meter1_vblank",
                                                       "bgb_meter1_default", "bgb_meter1_dispreg"} and
          {i for i, o in oid_of.items() if o == bz} == {"bgb_era0", "bgb_era1", "bgb_census1_frames", "bgb_census1_cum"},
          "boot_gaps_bake: every item names one of the plan's obligations - frame 1's pair and its CHECK "
          "boot.device_create's, the meter pair display.registers', the census zero acceptance.zero's, the rest "
          "p2.transport's")
    wit = lambda t: {"bgb_%s_%s" % (t, k) for k in ("disp", "swap", "why", "gaps", "run")}
    ids = lambda t: {i["id"] for i in by[t].ledger}
    check(wit("census1") <= ids("census1") and wit("meter1") <= ids("meter1") and
          wit("cppm4") - {"bgb_cppm4_run"} <= ids("cppm4") and "bgb_cppm4_run" not in ids("cppm4") and
          wit("gate") | wit("gate_span") <= ids("gate") and
          {i["roll"] for i in by["gate"].ledger if i["id"] in wit("gate")} == {"freeze"} and
          {i["roll"] for i in by["gate"].ledger if i["id"] in wit("gate_span")} == {"span"},
          "boot_gaps_bake: the strict witness in every default roll (census1, meter1, cppm4 without the meter's pm4 - the "
          "CP renders its records back to PM4 there -, both gate rolls)")
    res = dict(((o, u), e) for o, u, e in p["resolves"])
    own_bd = sorted(i for i, o in oid_of.items() if o == bd)
    check(set(res) == {(bt, "boot_gaps_bake"), (bd, "mode_state_bake")} and
          sorted(res[(bt, "boot_gaps_bake")]) == sorted(i for i, o in oid_of.items() if o == bt) and
          sorted(res[(bd, "mode_state_bake")]) == sorted(own_bd + list(npf.BOOT_GAPS_MODE_DEFAULT)),
          "boot_gaps_bake: the p2.transport items resolve boot_gaps_bake; mode_state_bake the boot.device_create items AND "
          "the default path's census1 / era1 items")
    # Astra round 1 on native/boot-gaps (P2): mode_state_bake's own items are census0's (the mode state OFF) and the
    # CHECK's (record mode off) - they never run the leaf as a record, so alone they could close it while the default
    # path failed. Its resolvers hold the default path's evidence of the same run: census1's display line (the leaf
    # taken once, no decline), census1's F records from frame 1, era1's zero from boot - routed to p2.transport and
    # acceptance.zero
    check(set(own_bd) == {"bgb_census0_mode", "bgb_chk", "bgb_chk_k10", "bgb_chk_disp"} and
          set(npf.BOOT_GAPS_MODE_DEFAULT) == {"bgb_census1_disp", "bgb_census1_frames", "bgb_era1"} and
          {oid_of[i] for i in npf.BOOT_GAPS_MODE_DEFAULT} == {bt, bz},
          "boot_gaps_bake: mode_state_bake's default-path resolvers are census1's display line and F records and era1, "
          "each routed to its own obligation")

    # the routing in the runner: each item to its obligation (the key taken off), the graceful census rolls' terminal
    # twins and their snapshot / receipt items following their roll's items; an item routed elsewhere refused
    r = npf.Runner(Args(plan="boot_gaps_bake", graceful_quit=False), p)
    r.results = [{"step": s.tag, "verdict": "PASS", "paths": {"json": "x/%s.json" % s.tag}} for s in steps]
    got = r.items()
    routed = {}
    for o, it in got:
        routed.setdefault(it["id"], set()).add(o)
    check(all("obligation" not in it for _o, it in got) and
          all(routed.get(i) == {o} for i, o in oid_of.items()) and
          routed.get("bgb_census0_mode.terminal") == {bd} and routed.get("bgb_census0_disp.terminal") == {bt} and
          routed.get("bgb.census0.terminal") == {bt, bd} and routed.get("bgb.census1.terminal") == {bt, bz} and
          routed.get("bgb_census1_cum.terminal") == {bz} and "bgb_meter1_dispreg.terminal" not in routed,
          "boot_gaps_bake: Runner.items routes each item (and each terminal twin) to its own obligation, the census rolls' "
          "terminal / receipt items to their items' obligations; no route key reaches the ledger")
    try:
        r.item_oids({"id": "x", "obligation": "p1.s1_flush"})
        check(False, "boot_gaps_bake: an item routed outside the plan's obligations is refused")
    except KeyError:
        check(True, "boot_gaps_bake: an item routed outside the plan's obligations is refused")
    check(r.item_oids({"id": "x"}) == [bt, bd, br, bz], "an item with no route goes to every obligation of its plan")
    # the env refusal: a default key in the parent environment refuses the live run, named; none, no refusal
    with patch.dict(os.environ, {}, clear=False):
        for k in npf.BOOT_GAPS_KEYS:
            os.environ.pop(k, None)
        clean = r.env_refusal()
        os.environ["SR_NATIVE_DISPLAY"] = "1"
        refused = r.env_refusal()
    check(clean == "" and "SR_NATIVE_DISPLAY=1" in refused and "grades the DEFAULT" in refused,
          "boot_gaps_bake: a parent environment carrying SR_NATIVE_DISPLAY refuses the live run (named); none, none")

    # the witness items on synthetic lines in the exact formats the build prints
    def grade_one(item, text):
        with tempfile.TemporaryDirectory() as td:
            open(os.path.join(td, "run.log"), "w", encoding="utf-8").write(text)
            ctx = nl.Ctx({"roots": {"umbrella": td}}, npf.LANE)
            return nl.check_item(ctx, dict({k: v for k, v in item.items() if k != "obligation"}, path="run.log"))[0]

    def disp(gate=1, mode=1, persist=1, ramp="1/0", sg=0, sc=1, ms=1, decl=(0, 0, 0, 0), prog=0, rf=0, cp=(1, 1),
             why="periodic", lut="0123456789abcdef"):
        return ("[t] [INFO ] [BOOT] [NATIVE_FLUSH] %s display gate=%d modeGate=%d persist=%d ramp=%s setGamma=%d "
                "scaler=%d modeState=%d | decl ramp=%d setGamma=%d scaler=%d modeState=%d | scalerProgram=%d "
                "reserveFail=%d | cp ramp=%d scaler=%d | lut=%s\n" % ((why, gate, mode, persist, ramp, sg, sc, ms) +
                                                                      tuple(decl) + (prog, rf) + tuple(cp) + (lut,)))
    on = npf.bootgaps_display_item("t_on", "on")
    check(grade_one(on, disp()) == nl.PASS, "boot gaps display item: the pre-registered default line passes")
    for what, line in (("a ramp decline", disp(decl=(1, 0, 0, 0))), ("a mode-state decline", disp(decl=(0, 0, 0, 1))),
                       ("no persisted display", disp(persist=0)), ("the PWL ramp", disp(ramp="0/1")),
                       ("two ramps", disp(ramp="2/0")), ("a set-gamma", disp(sg=1)), ("no scaler", disp(sc=0)),
                       ("no mode state", disp(ms=0)), ("a scaler program", disp(prog=1)), ("a reserve failure", disp(rf=1)),
                       ("the CP never applied the ramp", disp(cp=(0, 1))), ("the gate off", disp(gate=0)),
                       ("no ramp hash", disp(lut="?"))):
        check(grade_one(on, disp(why="census") + line) == nl.FAIL, "boot gaps display item: %s fails" % what)
    check(grade_one(npf.bootgaps_display_item("t_off", "off"),
                    disp(gate=0, mode=0, persist=0, ramp="0/0", sc=0, ms=0, cp=(0, 0))) == nl.PASS and
          grade_one(npf.bootgaps_display_item("t_off", "off"), disp(gate=0, mode=0, persist=0, ramp="1/0", sc=0, ms=0,
                                                                   cp=(0, 0))) == nl.FAIL and
          grade_one(npf.bootgaps_display_item("t_c", "cppm4"), disp(cp=(0, 0))) == nl.PASS and
          grade_one(npf.bootgaps_display_item("t_c", "cppm4"), disp()) == nl.FAIL and
          grade_one(npf.bootgaps_display_item("t_k", "check"), disp(persist=0, ramp="0/0", sc=0, ms=0, cp=(0, 0))) ==
          nl.PASS,
          "boot gaps display item: the =0 arm's, CPPM4's and CHECK's forms pass their lines and fail the default's")
    why = lambda f, fp=0: ("[t] [INFO ] [BOOT] [NATIVE_FLUSH] periodic swapSdkWhy devRead=0 devWrite=0 front=0 stack=0 "
                           "tiling=0 flags2A39=%d movieCapture=0 pix=0 timestamps=0 3B28=0 hsioRead=0 hsio=0 "
                           "movieGlobalRead=0 movieGlobal=0 rect=0 sysbuf=0 rootRead=0 root=0 export=0 cursorAlign=0 "
                           "cursorWrite=0 entry=0 fallbackFront=0 fallbackScaler=0 fallbackPersist=%d\n" % (f, fp))
    sdk = lambda d: "[t] [INFO ] [BOOT] [NATIVE_FLUSH] periodic swapSdk calls=9000 decl=%d\n" % d
    seg = lambda kb, ga: ("[t] [INFO ] [BOOT] [NATIVE_FLUSH] seg closed=79916 parts=150000 gapKB=%d recApplied=73790 "
                          "gapApplied=%d idMiss=0 mismatch=0 handoff=0 replayHit=0 replayMiss=0 expired=0 guestMode=0 "
                          "coher=0 pinned=0 chained=0 footStray=0 feetLost=0 coherDropOld=0 capDrop=0 litLenBad=0 "
                          "coherRange=0 coherTop=0\n" % (kb, ga))
    run = lambda pm4, dr=0: ("[t] [INFO ] [BOOT] [NATIVE_METER_RUN] periodic 227.0s frames=6001 | pm4=%d edram_res=0 "
                             "edram_xfer=0 edram_load=0 nrec=8550091 dispreg=%d xma=0 | per_frame pm4=0.0\n" % (pm4, dr))
    check(grade_one(npf.bootgaps_swap_item("t"), sdk(0)) == nl.PASS and
          grade_one(npf.bootgaps_swap_item("t"), sdk(1)) == nl.FAIL and
          grade_one(npf.bootgaps_swap_item("t", 1), sdk(1)) == nl.PASS and
          grade_one(npf.bootgaps_swap_item("t", 1), sdk(10)) == nl.FAIL and
          grade_one(npf.bootgaps_why_item("t"), why(0)) == nl.PASS and
          grade_one(npf.bootgaps_why_item("t"), why(1)) == nl.FAIL and
          grade_one(npf.bootgaps_why_item("t"), why(0, 1)) == nl.FAIL and
          grade_one(npf.bootgaps_why_item("t", 1), why(1)) == nl.PASS and
          grade_one(npf.bootgaps_gaps_item("t"), seg(0, 0)) == nl.PASS and
          grade_one(npf.bootgaps_gaps_item("t"), seg(0, 2)) == nl.FAIL and
          grade_one(npf.bootgaps_gaps_item("t"), seg(9, 0)) == nl.FAIL and
          grade_one(npf.bootgaps_gaps_item("t", 2), seg(9, 2)) == nl.PASS and
          grade_one(npf.bootgaps_gaps_item("t", 2), seg(0, 20)) == nl.FAIL and
          grade_one(npf.bootgaps_run_item("t"), run(0)) == nl.PASS and
          grade_one(npf.bootgaps_run_item("t"), run(529)) == nl.FAIL and
          grade_one(npf.bootgaps_run_item("t", 529), run(529)) == nl.PASS and
          grade_one(npf.bootgaps_dispreg_item("t"), run(0, 0)) == nl.PASS and
          grade_one(npf.bootgaps_dispreg_item("t"), run(0, 1)) == nl.FAIL and
          grade_one(npf.bootgaps_dispreg_item("t", 1), run(529, 1)) == nl.PASS,
          "boot gaps witnesses: SWAP's declines, the reasons (flags2A39, fallbackPersist), the gaps from boot, the meter's "
          "pm4 and dispreg from boot - each pre-registered value passes, any other fails")
    ab = npf.bootgaps_display_item("t_ab", "ab")
    check(grade_one(ab, disp()) == nl.PASS and
          grade_one(ab, disp(persist=0, ramp="0/0", sc=0, ms=0, cp=(0, 0))) == nl.PASS and
          grade_one(ab, disp(ramp="0/0", cp=(0, 1))) == nl.PASS and
          grade_one(ab, disp(decl=(1, 0, 0, 0))) == nl.FAIL and grade_one(ab, disp(prog=1)) == nl.FAIL and
          grade_one(ab, disp(rf=1)) == nl.FAIL and grade_one(ab, disp(ramp="0/1")) == nl.FAIL and
          grade_one(ab, disp(sg=1)) == nl.FAIL and grade_one(ab, disp(gate=0)) == nl.FAIL and
          grade_one(npf.bootgaps_why_item("t", None), why(1)) == nl.PASS and
          grade_one(npf.bootgaps_why_item("t", None), why(0, 1)) == nl.FAIL,
          "boot gaps A/B witness: either arm's boot (any mix after the first VdSwap's flip) passes; a decline, a "
          "program, a reserve failure, the PWL ramp, a set-gamma or the gate off fails; fallbackPersist 0 either way")
    vb = lambda sysbuf, scan=0: ("[t] [INFO ] [BOOT] [NATIVE_VBLANK] exit state=on toggled=0 ticks=9000 native=9000 "
                                 "guest=0 released=40 callbacks=0 traceBad=0 late=0 maxGapUs=17012 vcount=6480000 | "
                                 "pacing calls=300 host=300 guest=0 wait=3 now=290 next=7 early=0 mismatch=0 | dispreg "
                                 "scanout=%d sysbuf=%d vsyncBit=0\n" % (scan, sysbuf))
    check(grade_one(npf.bootgaps_vblank_item("t"), vb(0)) == nl.PASS and
          grade_one(npf.bootgaps_vblank_item("t"), vb(1)) == nl.FAIL and
          grade_one(npf.bootgaps_vblank_item("t"), vb(0, 2)) == nl.FAIL and
          grade_one(npf.bootgaps_vblank_item("t", 1), vb(1)) == nl.PASS,
          "boot gaps vblank item: the system command buffer answered 0 times on the default (1 on the =0 arm), no "
          "scan-out answer")
    on_default = ("[t] [INFO ] [BOOT] [NATIVE_VBLANK] SR_NATIVE_VBLANK on (the baked default; =0 keeps the guest "
                  "bodies): the pacing 82111E38 and the vblank ISR's source 0 as host code\n")
    on_explicit = ("[t] [INFO ] [BOOT] [NATIVE_VBLANK] SR_NATIVE_VBLANK on (=1): the pacing 82111E38 and the vblank "
                   "ISR's source 0 as host code\n")
    check(grade_one(npf.bootgaps_vblank_item("t"), on_default + vb(0)) == nl.PASS and
          grade_one(npf.bootgaps_vblank_item("t"), on_explicit + vb(0)) == nl.FAIL and
          grade_one(npf.vblank_default_item("t"), on_default + vb(0)) == nl.PASS and
          grade_one(npf.vblank_default_item("t"), on_explicit + vb(0)) == nl.FAIL and
          grade_one(npf.vblank_default_item("t"), vb(0)) != nl.PASS,
          "boot gaps meter rolls: the vblank bake's DEFAULT form - the explicit On line (=1) fails both the vblank item "
          "and the default-form item, which needs the baked default's On line")
    fl = lambda n, pk: "[t] [INFO ] [BOOT] [PM4_COV] F frame=%d pk=%d t0=0 t0dw=0 t3=0 t3dw=0 pred=0\n" % (n, pk)
    cum = lambda pk: "[t] [INFO ] [BOOT] [PM4_COV] CUM periodic frames=6002 pk=%d pk/f mean=0.0 min=0 max=0\n" % pk
    fz, cz = npf.census_zero_items("bgb", "t")
    check(grade_one(fz, fl(1, 0) + fl(2, 0) + fl(3, 0)) == nl.PASS and
          grade_one(fz, fl(1, 2) + fl(2, 0)) == nl.FAIL and grade_one(fz, fl(1, 0) + fl(2, 527)) == nl.FAIL and
          grade_one(fz, "") != nl.PASS and grade_one(cz, cum(0)) == nl.PASS and grade_one(cz, cum(529)) == nl.FAIL,
          "boot gaps census zero: no F record with a packet from frame 1 (an absent stream never passes), CUM pk 0")

    # the era step: the run-unique JSON with the runner's flat summary and the two arms' ramp hashes, graded by its item
    os.makedirs(os.path.join(npf.LANE, "archive"), exist_ok=True)
    with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
        rd = npf.Runner(Args(plan="boot_gaps_bake", out=td, dry_run=True), p)
        rd.do_era(e1)
        check(rd.results[-1]["verdict"] == "DRY" and "newgame:" in rd.results[-1]["detail"] and
              "era_census.py" in rd.results[-1]["detail"] and "lut vs" in rd.results[-1]["detail"],
              "boot_gaps_bake: the era step (dry) names era_census.py newgame:<census1> and the ramp comparison")
    for name, doc, lut0, lut1, step, want in (
            ("pass", {"verdict": "PASS", "rows": [("boot", 0, None), ("intro", 0, None), ("ingame", 0, None)],
                      "gap": 0}, "aa" * 8, "aa" * 8, e1, "PASS"),
            ("pass, ramp differs", {"verdict": "PASS", "rows": [("boot", 0, None)], "gap": 0}, "aa" * 8, "bb" * 8, e1,
             "FAIL"),
            ("pass, a boot packet", {"verdict": "FAIL", "rows": [("boot", 2, 1)], "gap": 1}, "aa" * 8, "aa" * 8, e1,
             "FAIL"),
            ("control", {"verdict": "FAIL", "rows": [("boot", 529, 1), ("intro", 0, None)], "gap": 2}, None, None, e0,
             "PASS"),
            ("control, a packet in another era", {"verdict": "FAIL", "rows": [("boot", 529, 1), ("ingame", 6, 900)],
                                                  "gap": 2}, None, None, e0, "FAIL"),
            ("control, another boot", {"verdict": "FAIL", "rows": [("boot", 533, 1)], "gap": 2}, None, None, e0, "FAIL")):
        with tempfile.TemporaryDirectory(dir=os.path.join(npf.LANE, "archive")) as td:
            rr = npf.Runner(Args(plan="boot_gaps_bake", out=td, dry_run=False), p)
            checks = {"C0_usable": True, "C1_frames": True, "C4_reached": True, "C6_items_zero": True,
                      "C9_terminal": True, "C10_identity": True, "zpd_agree": True, "gaps_agree": True,
                      "C2_run_zero": doc["verdict"] == "PASS", "C3_era_zero": doc["verdict"] == "PASS",
                      "C5_gaps_zero": doc["gap"] == 0}
            tool = {"verdict": doc["verdict"], "one_exe": True, "rolls": [{
                "verdict": doc["verdict"], "checks": checks, "failed": [],
                "eras": [{"name": n, "pk": pk, "first_nonzero": {"frame": f} if f else None} for n, pk, f in doc["rows"]],
                "windows": [], "whole_run": {"seg": {"gapApplied": doc["gap"]}}, "missing_eras": []}]}
            for tag, lut in (("census0", lut0), ("census1", lut1)):
                log = npf.run_log_of(rr.path(tag))
                os.makedirs(os.path.dirname(log), exist_ok=True)
                with open(log, "w", encoding="utf-8") as f:
                    if lut:
                        f.write(disp(why="periodic", lut="00" * 8) + disp(why="terminal", lut=lut))
            canned = os.path.join(td, "canned.json")
            with open(canned, "w", encoding="utf-8") as f:
                json.dump(tool, f)
            # a stand-in for era_census.py: copies the canned document to the --json path the runner reserved
            rr.era_cmd = lambda s, js, canned=canned: [sys.executable, "-c", "import shutil,sys; shutil.copy(sys.argv[1], "
                                                       "sys.argv[2]); sys.exit(0)", canned, js]
            rr.write_proof()
            rr.do_era(step)
            row = rr.results[-1]
            out = json.load(open(os.path.join(npf.UMBRELLA, row["paths"]["json"].replace("/", os.sep)),
                                 encoding="utf-8"))
            check(row["verdict"] == want and out.get("committed") is True and out["runner"]["verdict"] == want and
                  out["roll"]["boot_pk"] == doc["rows"][0][1],
                  "boot_gaps_bake era step (%s): %s, the run-unique JSON committed with the verdict and the summary" % (
                      name, want))

    # THE REGRESSION (Astra round 1 on native/boot-gaps, P2): passing census0 / CHECK items cannot close mode_state_bake
    # when census1 / era1 fail or are absent. (1) apply_ledger with the real plan on a synthetic ledger of the plan's four
    # obligations: a run carrying census0's and the CHECK's items alone attaches no resolver; a whole run attaches every
    # one, the census1 / era1 copies included. (2) the ledger tool's own grader over those copies on synthetic evidence:
    # an unknown resolves only when every resolver passes (native_ledger.py), so census0 / CHECK passing with census1's
    # display line, its frames or era1 failing - or their evidence absent - leaves it open.
    def syn_ledger():
        lines = ['{', '  "obligations": [']
        for k, oid in enumerate(npf.BOOT_GAPS_OBLIGATIONS):
            unk = {bt: "boot_gaps_bake", bd: "mode_state_bake"}.get(oid)
            lines += ['    {', '      "id": "%s",' % oid, '      "evidence": [',
                      '        {"id": "seed_%d", "kind": "text"}' % k, '      ]' + ("," if unk else "")]
            if unk:
                lines += ['      "residual_unknowns": [', '        {"id": "%s", "text": "open"}' % unk, '      ]']
            lines.append('    }' + ("," if k < len(npf.BOOT_GAPS_OBLIGATIONS) - 1 else ""))
        return "\n".join(lines + ['  ]', '}', ''])

    class RunOf:
        def __init__(self, its):
            self._its, self.plan = its, p

        def items(self):
            return self._its

    def unknown_of(lp, uid):
        led = json.loads(open(lp, encoding="utf-8").read())
        return next(u for o in led["obligations"] for u in o.get("residual_unknowns", []) if u["id"] == uid)

    with tempfile.TemporaryDirectory() as td:
        lp = os.path.join(td, "partial.json")
        open(lp, "w", encoding="utf-8", newline="").write(syn_ledger())
        done = npf.apply_ledger(lp, RunOf([(o, it) for o, it in got if it["id"] in own_bd]))
        said = [d[2] for d in done if d[1] == "mode_state_bake"]
        check("resolved_by" not in unknown_of(lp, "mode_state_bake") and len(said) == 1 and "NOT resolved" in said[0] and
              all(i in said[0] for i in npf.BOOT_GAPS_MODE_DEFAULT),
              "boot_gaps_bake regression: a run with census0's and the CHECK's items alone (census1 / era1 absent) "
              "attaches no resolver to mode_state_bake and names the missing default-path items")
        lp = os.path.join(td, "whole.json")
        open(lp, "w", encoding="utf-8", newline="").write(syn_ledger())
        npf.apply_ledger(lp, RunOf(got))
        rb = unknown_of(lp, "mode_state_bake").get("resolved_by", [])
        check(sorted(x["id"] for x in rb) == sorted("mode_state_bake." + i for i in res[(bd, "mode_state_bake")]),
              "boot_gaps_bake regression: a whole run attaches every resolver to mode_state_bake, census1's display line "
              "and frames and era1 among them")

    def era_doc(checks, **over):
        """A run-unique era JSON meeting every one of an era item's checks (each an eq), then `over` (dotted field ->
        value) applied."""
        doc = {}
        for c in list(checks) + [{"field": f.replace("__", "."), "op": "eq", "value": v} for f, v in over.items()]:
            assert c["op"] == "eq", c
            node, parts = doc, c["field"].split(".")
            for part in parts[:-1]:
                node = node.setdefault(part, {})
            node[parts[-1]] = c["value"]
        return doc

    era1_item = next(i for i in e1.ledger if i["id"] == "bgb_era1")
    chk_line = ("[t] [INFO ] [BOOT] [NATIVE_FLUSH] periodic CHECK brackets=12 BAD bytes=0 len=0 dev=0 | kinds full/bad: "
                "k8=1/0 k10=1/0\n")
    frame = lambda n, pk: "[t] [INFO ] [BOOT] [PM4_COV] F frame=%d pk=%d t0=0 t0dw=0 t3=0 t3dw=0 pred=0\n" % (n, pk)
    good = {"bgb_census0_mode": "[t] [INFO ] [BOOT] [PM4_COVMAP] periodic inner=00000000 name=UNSTAMPED pk=2 dw=6 "
                                "keys=r05:1,r0F:1\n",
            "bgb_chk": chk_line, "bgb_chk_k10": chk_line,
            "bgb_chk_disp": disp(persist=0, ramp="0/0", sc=0, ms=0, cp=(0, 0)),
            "bgb_census1_disp": disp(why="terminal"),
            "bgb_census1_frames": frame(1, 0) + frame(2, 0) + frame(3, 0),
            "bgb_era1": era_doc(era1_item["checks"])}

    def resolves(content):
        """The ledger's rule over mode_state_bake's resolvers as apply_ledger attached them: every one graded by
        native_ledger.check_item on its synthetic evidence (None: absent) must pass."""
        with tempfile.TemporaryDirectory() as td:
            ctx = nl.Ctx({"roots": {"umbrella": td}}, npf.LANE)
            results = []
            for item in rb:
                base = item["id"].split(".", 1)[1]
                body = content.get(base)
                name = base + (".json" if isinstance(body, dict) else ".log")
                if body is not None:
                    with open(os.path.join(td, name), "w", encoding="utf-8") as f:
                        f.write(json.dumps(body) if isinstance(body, dict) else body)
                results.append(nl.check_item(ctx, dict(item, root="umbrella", path=name))[0])
            return bool(results) and all(r == nl.PASS for r in results)

    check(resolves(good), "boot_gaps_bake regression: every resolver passing (census0, the CHECK, census1's display line "
                          "and frames, era1) resolves mode_state_bake")
    for what, over in (("census1's mode state not taken", {"bgb_census1_disp": disp(why="terminal", ms=0)}),
                       ("census1's mode-state leaf declined", {"bgb_census1_disp": disp(why="terminal", decl=(0, 0, 0, 1))}),
                       ("frame 1's pair in census1", {"bgb_census1_frames": frame(1, 2) + frame(2, 0)}),
                       ("era1 with a boot packet", {"bgb_era1": era_doc(era1_item["checks"], roll__boot_pk=2)}),
                       ("era1 with a boot gap", {"bgb_era1": era_doc(era1_item["checks"], roll__gap_applied=1)}),
                       ("census1's display line absent", {"bgb_census1_disp": None}),
                       ("census1's frames absent", {"bgb_census1_frames": None}),
                       ("era1 absent", {"bgb_era1": None})):
        check(not resolves(dict(good, **over)),
              "boot_gaps_bake regression: census0 and the CHECK passing, %s - mode_state_bake stays open" % what)


def lane_tempdir():
    """Every scratch folder of this test goes under the lane's own archive/ (step 4 M10, 2026-10-18): on the rig
    %TEMP% is on C: while the umbrella is on D:, so Runner.rel()'s os.path.relpath(<temp path>, UMBRELLA) raised
    "path is on mount 'C:', start on mount 'D:'" and ended the test at zpd7_xenia_checks; AGENTS.md also keeps run
    output off C:. The folder is created per run and removed at the end."""
    top = tempfile.mkdtemp(prefix="tmp_test_native_proof_", dir=os.path.join(npf.LANE, "archive"))
    tempfile.tempdir = top
    return top


if __name__ == "__main__":
    os.makedirs(os.path.join(npf.LANE, "archive"), exist_ok=True)
    _tmp = lane_tempdir()
    try:
        _rc = main()
    finally:
        tempfile.tempdir = None
        shutil.rmtree(_tmp, ignore_errors=True)
    sys.exit(_rc)
