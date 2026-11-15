#!/usr/bin/env python3
"""native_proof.py - one native obligation's proof batch, graded by the world-rule gate and recorded into the
ledger (iter-467 native-a; RELEASE_PLAN phase 3; TOOLKIT 1.8f / 1.8i).

    python tools/perf/native_proof.py --plan <name> --exe <exe> [--base <exe>] [--out <dir>] [--repo <checkout>]
           [--owner native-a] [--only tag,..] [--skip tag,..] [--dry-run] [--slot-held]
           [--apply-ledger [--ledger <docs/native_ledger.json>]] [--stage J:\\sr_stage [--stage-source <disc folder>]]
           [--graceful-quit]   # rolls end by the graceful quit: whole-run census + added terminal twins (TOOLKIT 1.8m)
    python tools/perf/native_proof.py --plan <name> --out <dir> (--reapply | --recompare) [--apply-ledger]
    python tools/perf/native_proof.py --list

Plans (the re-proofs owed on 2026-10-09, each on a base with the black-world fix 41572437 and the world rule):
    s1_ab        p1.s1_flush      the S1 draw flush's first frozen-pass A/B (SR_AB_TOGGLE=SR_NATIVE_FLUSH), then under stress
    rows467_ab   p1.rows467       rows 4+6+7's frozen-pass A/B again (p1d_abp graded a black world)
    row3         p1.row3          row 3: stress, A/B and the meter again (p3a's graded black), the gate
    rows53b      p1.row5, p1.row3b   rows 5+3b's whole bar: dry CHECK, CPPM4, stress, A/B, meter, gate
    segmentless  p2.segmentless   P2-B step 1: probe (trace), A/B, CPPM4, stress, whole run + trace invariants, meter, gate
    setconstmem_bake p2.setconst_mem   the setconst bake: replay vs the pre-bake exe, the new-game gate with the strict
                                  witness, the default path under CPPM4, the meter (SR_NATIVE_SETCONSTMEM=0 vs the
                                  default: the const kind to 0, the total not rising)
    construns_bake p1.row3_construns   the construns bake: replay vs the pre-bake exe, the strict new-game gate with the
                                  construns counters, a traced probe, CPPM4, STRESS, the per-present A/B, the producer
                                  census pair (--construns-zero), the meter (=0 vs the default, bound -10.0)
    present_gate p2.transport     defined gate emission: probe, per-present A/B, CPPM4, stress and drawn-world gate
    swap_sdk     p2.transport     normal SDK swap: probe, per-present A/B, CPPM4, stress and drawn-world gate
    swap_sdk_profile diagnostic    SDK ON/OFF host profiles without frames; no acceptance evidence
    transport_pair p2.transport   native CPU submission/park pair: traced probe (drain rule), per-present A/B, CPPM4 +
                                  its SR_NATIVE_TRANSPORT=0 control, stress, gate
    residue      p2.transport     BlockUntilIdle / InvalidateGpuCache / lock-core blocks as records: probe + trace, A/B,
                                  CPPM4, stress, the owner census (own packets), meter, gate
    flush_split  p1.flush_residue SR_NATIVE_FLUSH_SPLIT (S1 sections chunked across the kick, >0xFFFF draws split, row 9
                                  through its kicks): the S1 and row-9 dry CHECKs, probe + trace, A/B, CPPM4, stress, the
                                  producer census pair, meter, gate; every pure-ON roll graded on its counter
                                  snapshots (tools/perf/flush_split_snapshot.py: the strict bar from boot through
                                  the last snapshot; freshness relative to the last logged timestamp allows 1.0 s; the
                                  remaining execution tail is unmeasured)
    flush_split_bake p1.flush_residue  the flush-split bake (SR_NATIVE_FLUSH_SPLIT default ON): replay vs the pre-bake
                                  exe, the new-game gate with the strict witness, the default path under CPPM4, the
                                  default-path meter (=0 vs the default), the default-path census pair; every default
                                  roll on its snapshots (pre=0)
    transport_composed p2.transport  every transport kind + devcreate ON: probe + trace, the composed A/B, CPPM4,
                                  stress + its all-OFF control, the census pair (the transport-producer zero), meter,
                                  pacing (console path, kind-29 trace records, a second all-OFF control as the
                                  calibration: INCONCLUSIVE if the controls disagree), gate
    zpd          p2.zpd           the occlusion queries as host D3D12 queries (SR_NATIVE_ZPD): replay, then rolls from
                                  the chapter-4 seed flying tools/perf/zpd_play.txt - the default path's zpd census
                                  (the queries fire), strict's census (resolved, written before the covering fence,
                                  late 0), the latency model's, the frozen presents exact (plain and CPPM4) and the
                                  save-started flicker gate
    transport_bake p2.transport   the transport bake (the nine kinds default ON, DEVCREATE off): replay vs the pre-bake
                                  exe, the strict gate, a traced probe, the per-present A/B (default vs =0), CPPM4,
                                  STRESS + its =0 control, the census pair (--pm4-zero --allow-devcreate-reemit), the
                                  meter (=0 vs the default, bound -82.6), pacing (=0 / default / =0 calibration)
    transport_fix p2.transport    the tcmp1 diagnosis's follow-up: the composed probe + trace and stress with the
                                  decline counters (SWAP only mode declines, BRACKET memory 0), the binding record;
                                  the probe again ended by the graceful quit (terminal items, terminal segment_order)
    display_registers display.registers  SR_NATIVE_VBLANK on the composed transport: probe + trace (drain rule), the
                                  per-present A/B (the per-vblank check on both arms), CPPM4, the dispreg meter
                                  (-> 0, the whole run too), pacing (pacing_cmp.py on the console path against the
                                  gate-OFF composed roll; an OWED step on a base without the comparator), gate
    display_registers_bake display.registers  the display-registers bake (SR_NATIVE_VBLANK default ON): replay vs the
                                  pre-bake exe, the strict gate, a traced probe, the per-present A/B (default vs =0),
                                  CPPM4, the dispreg meter (=0 vs the default: 0 per ingame frame; the run's cumulative
                                  <= its SWAP declines d), pacing (=0 / default / =0 calibration); the vblank witness
                                  (its default form) and the CPU-2 lock in every default roll, the off witness in every
                                  =0 roll; a fresh --out; refused while the parent env sets SR_NATIVE_VBLANK
    zpd_bake     p2.zpd           the zpd bake (SR_NATIVE_ZPD default ON, latency; 82101E08 as records) on the real
                                  default build (SR_NATIVE_VBLANK's default too): replay vs the pre-bake exe, the gate,
                                  the dry CHECK, the producer census pair (the default's gameplay PM4 0), the per-present
                                  A/B of the port, CPPM4, the meter, pacing; the default form and the latency census in
                                  every default roll, the off witness in every =0 roll; a fresh --out; refused while the
                                  parent env sets SR_NATIVE_ZPD or SR_NATIVE_VBLANK
    boot_attach  (none)          render/boot-attach: replay, a declared warm-up roll inside the invocation, three
                                  alternating boot pairs in the mirrored order OFF/ON, ON/OFF, OFF/ON
                                  (SR_SHADER_LOAD_ASYNC), the flicker gate; graded by tools/perf/boot_attach_eval.py
                                  --warmup (never grades the warm-up)
    devcreate_close boot.device_create   the two owed items on the current exe, staged: a declared warm-up, 8
                                  alternating SR_NATIVE_DEVCREATE ON / OFF boot pairs graded by tools/perf/boot_equiv.py
                                  (BOOT_EQUIV design dcl-v1, bound to the boot_order_gap resolver), 4 alternating meter
                                  pairs pooled frame-weighted by the ledger's grader (meter_pool, bound -3); PARKED
                                  since the bake plan below (its OFF arms pin =0 since the bake)
    devcreate_bake boot.device_create    the bake (SR_NATIVE_DEVCREATE default ON on the transport-baked exe; one exe,
                                  staged, a fresh --out): replay vs the pre-bake exe, a declared warm-up (the default),
                                  4 alternating default / =0 pairs graded by boot_equiv.py --design dcf-v1 AND as the
                                  ledger's pooled zero-form meter (<= -5.7, default <= 0.1, =0 >= 5.5), the dry CHECK
                                  (no BAD, each emitter kind 8 / 9 compared), the per-present A/B, CPPM4, the census
                                  pair (--pm4-zero --base-shows devcreate: NO allowance, both ended by the graceful
                                  quit), the stall block s1_on..s20_on, the gate, boot_stall.py --design dcf-stall-v2
                                  over every ON boot of the run (29: the gate's two included; events F/A/W/R/C/H;
                                  staged); 37 rolls, ~2 h 45 min; resolves the new unknown default_bake
    boot_gaps_bake p2.transport + boot.device_create + display.registers + acceptance.zero (each item names its own)
                                  the boot gaps' bake (SR_NATIVE_MODE_STATE / SR_NATIVE_DISPLAY default ON; one exe,
                                  staged, a fresh --out, no inherited gate, SR_NATIVE_VBLANK included): replay, the
                                  census pair (=0 then the default, both by the graceful quit) each graded by
                                  era_census.py newgame (the =0 arm FAILs in boot alone, the default PASSes every era
                                  from boot, its ramp equal), the meter pair on the vblank bake's default
                                  (run-cumulative dispreg 1 -> 0, SWAP decl 1 -> 0),
                                  the dry CHECK (kind 10), the per-present A/B, CPPM4, the gate; the strict witness in
                                  every default roll; 9 rolls, ~50 min; resolves boot_gaps_bake / mode_state_bake
    memres_bake  mem.resources    the native resource route's bake (SR_MEM_RESOURCES default ON in the dev build; =0 the
                                  page model): replay 9/9 vs the pre-bake exe (its page model with the ordered replay
                                  restore), the new-game gate, the shm meter (=0 vs the default) on the new-game route
                                  and the Warworld seed; every default roll the route's default form, its patches and
                                  the whole run's shm=0
    log_async    (none)         app/log-async: the asynchronous log writer - a census roll (SR_NATIVE_BRACKETS=1), a
                                  plain roll, the census roll with a 5 s writer stall injected in gameplay; graded by
                                  tools/perf/log_stall_eval.py --proof <out> --prefix lsa

Every roll is perf_run's gameplay scenario (port config, uncapped, SR_INSTANCE_ID=native), run FROM --repo (the
checkout with the game disc beside it - a lane worktree has none; default the dev checkout) with the lane's exe
snapshot; outputs go under --out (default: this lane's archive/v1_native) as <prefix>_<tag>. Freeze rolls are
graded by the flicker gate's own grader (pixels exact + the world rule: >= 8 graded freezes showing the 3D world,
tools/perf/world_floor.py), the gate by flicker_gate.py, meters by meter_delta.py against a same-exe default roll.
A roll that stalls the CP ([CP_WAIT] stalled, a GPU_HANG trigger), dies (THREAD_FATAL, ICALL_FAIL) or never reaches
play ends the plan: the slot is not spent on rolls that cannot grade. The game slot is held for the whole plan
(tools/game_slot.py acquire --owner; a heartbeat per step; released in a finally) unless --slot-held.

--apply-ledger upserts the plan's evidence items into the ledger (one line per item, in the file's own style). A
failed step's items land too and grade FAIL against its own output; the comparison steps (owner, census, tcensus, pacing,
snapshot) write a
RUN-UNIQUE output (<tag>.<UTC stamp>.json, created exclusively, the step's verdict stamped in it, "committed": true
written last, after the step's final row is in the proof) and their item names that exact file, so an older run's file
is never read (devcreate-boot 2026-10-14, Astra rounds 4-6). One native_proof per --out (an OS-held lock on
<out>/native_proof.lock); a proof JSON that cannot be written fails the run (exit 3) and nothing is applied; --reapply
refuses an older proof after such a failure. --recompare re-runs only the comparison steps over the rolls in --out,
every other stored row kept. A contradiction resolver named in the plan's resolver_paths takes the applied item's
run-unique path. A superseded item of the same id that currently FAILS moves into the obligation's contradiction
witness list as
<id>_void with "expect": "fail" (a new contradiction, resolved by the race fix 41572437 being on HEAD, when the
record has none, or into the plan's supersede_into contradiction for that item id) - never deleted (TOOLKIT 1.8i);
a passing one is replaced. The superseded item's own status is results_now's (the ledger tool's run over the default
ledger) or, when that has none (a custom --ledger), the ledger tool's direct grade of the existing line - only a
positive pass, or the same path re-applied, is replaced in place; a failing, unavailable or ungradeable one is kept as
a witness (Astra round 1 on native/devcreate-close, P1-3: a second failed pool replaced the first). Residual unknowns
the plan answers gain their resolved_by. Then tools/native_ledger.py -v --id runs for each obligation. Nothing touches the ledger
without --apply-ledger; --dry-run prints the steps, the gates and the items and runs nothing.
Two more routes (native/devcreate-close, 2026-10-14; apply_ledger's docstring): `resolver_bind` - an item that is no
evidence but binds a residual unknown's pre-registered resolver on every outcome (the run it was bound to before moves
into the unknown's witnesses, expect fail unless it passes); `retire_on_pass` - when tools/native_ledger.py's own grade
of the applied item is a pass, the item it re-measures moves out of the evidence into a contradiction (witness, expect
fail) that the applied item resolves; without the pass nothing moves. A step with `always` (the bootequiv and
bootstall comparisons, the pooled meter) runs even after a roll ended the plan, so a crash is graded, never skipped. A
meter step with `pool` grades the ledger's pool of several pairs by the ledger's own check and writes a run-unique
receipt (its verdict, each roll's run row and file hashes) the item names. A plan with `fresh_out` refuses a live run
into an --out that already holds its proof JSON or any of its roll folders (its rolls are never overwritten).
Every roll, the gate's folder and every p2trace summary carry <prefix>_<tag>.exe.json (the exe's sha256, written before
the roll starts). A plan with `same_exe` (the transport bake; Astra round 1 on native/transport-bake) enforces its one
exe: a comparison step (meter, p2trace, census, owner, tcensus, pacing, snapshot, bootequiv, bootstall) whose input
names another exe - or none (a roll from before the stamp counts only when the stored proof's row of it ran on the
proof's exe) - is a FAIL that yields no item, and a live run refuses an --out holding another exe's roll or summary of
the plan (an --only re-run into another exe's evidence). --reapply / --recompare refuse an --exe other than the stored
proof's.
"""
import argparse
import datetime
import hashlib
import inspect
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
LANE = os.path.dirname(os.path.dirname(HERE))            # this checkout
UMBRELLA = os.path.dirname(os.path.dirname(LANE))        # <umbrella>: <umbrella>/_worktrees/<lane>
DEV = os.path.join(UMBRELLA, "_worktrees", "dev")        # the checkout with the disc (default --repo)
# <prefix>_proof.json + this: written when a run could not write its proof - a later --reapply refuses the older proof
UNWRITABLE = ".unwritable"
REPLACE_TRIES, REPLACE_WAIT_S = 20, 0.25  # ~5 s in all


def replace_retry(src, dst, tries=REPLACE_TRIES, wait=REPLACE_WAIT_S, _replace=None):
    """os.replace that rides out Windows' TRANSIENT sharing violation: a scanner or file watcher holding `dst` open
    without FILE_SHARE_DELETE makes the rename fail with PermissionError (WinError 5 / 32) for a moment. frs1
    (2026-10-14) lost the rest of its plan to one such failure. Only PermissionError is retried; after `tries`
    attempts the last error is raised (the caller's failure path stands); any other OSError is raised at once."""
    rep = _replace or os.replace
    for k in range(tries):
        try:
            return rep(src, dst)
        except PermissionError:
            if k == tries - 1:
                raise
            time.sleep(wait)
# a comparison step's run-unique JSON before its result is in (Runner.reserve_output): a FAIL until the step stamps it
PLACEHOLDER = {"runner": {"verdict": "FAIL", "why": "reserved: the comparison did not finish"}}
# the comparison steps: each writes a RUN-UNIQUE output (reserve_output / guarded / stamp_verdict / commit_output) that
# its item names; --recompare re-runs them over the stored rolls (transport-composed: tcensus and pacing, 2026-10-14;
# flush-residue: snapshot, the merge with dev ce46c966)
COMPARISON_KINDS = ("owner", "census", "tcensus", "pacing", "snapshot", "bootequiv", "bootstall", "era", "zpdwin",
                    "abwit", "memcensus", "texid", "readers")
# --recompare re-runs the comparison steps over the stored rolls, and a plan's `recompare_extra` kinds too (zpd_bake:
# its meter - Astra round 1 on native/zpd-bake, P2-4: the meter's verdict is the ledger's own check of the unrounded
# values, a rule a stored row can predate)
RECOMPARE_KINDS = COMPARISON_KINDS
# the ledger kinds graded on a roll's run.log (text: one line's pattern; text_le: two counters of the same log -
# native/dreg-bake's run-cumulative dispreg criterion)
LOG_KINDS = ("text", "text_le")
sys.path.insert(0, HERE)
from gpu_hang_check import PHASE_WITHIN_S, dump_stall_void
import flicker_gate as fg  # noqa: E402
from run_health import scan_log

BASE_GATES = "SR_INSTANCE_ID={inst},SR_HIGHFPS=uncapped,SR_HIGHFPS_PACE=1"
FREEZE = "SR_DIAG_SIM_FREEZE=8:2000:80,SR_PRESENT_DUMP_FREEZE=2,SR_PRESENT_DUMP_FREEZE_CAP=40"
STRESS = "SR_NATIVE_SEG_STRESS=1024"
# The orchestrator 2026-10-11 (segk3's recovered H1: the CP 3 s in pm4WaitThirdFree): the rolls that grade no pixels
# run without the frame dumps - the suspected stall (a synchronous readback + BMP write on the render thread holding
# the host queue). NO_TIMED = the 120 / 240 / 360 s BMPs (perf_run never turned them off), NO_SPARSE = the scenario's
# sparse present dump. A plain roll a meter step reads keeps the sparse dump: the meter's world check (world_min)
# counts the drawn world in it. Pixel-graded rolls (freeze A/B, CPPM4 / stress exact, the gate) keep every dump.
NO_TIMED = "SR_FRAME_DUMP=0"
NO_SPARSE = "SR_PRESENT_DUMP=0"
CPPM4 = "SR_NATIVE_FLUSH_CPPM4=1,SR_NATIVE_CONST_DEDUP_CHECK=1"           # the CP renders each record back to PM4
PM4OUT = "SR_NATIVE_FLUSH_PM4=1,SR_NATIVE_CONST_DEDUP_CHECK=1"            # the records written as PM4 (segmentless bridge)
TRACES = "menu,eahd,t16,t21,t25,gp1,gp2,lg1,lg2"
RACE_FIX = "41572437"
# transport_composed's pacing rolls (Astra round 3): the window every pacing check needs (tools/perf/pacing_cmp.py
# PLAN_WINDOW, kept equal by test_native_proof.py; pacing_cmp itself needs numpy, which this runner does not import)
# and the long roll that holds it in gameplay
PACE_WINDOW = 28672
PACE_LONG = dict(play="tools/player/density_soak_play.txt", dwell=1200, seconds=2100)
# The transport bake (native/transport-bake, 2026-10-15): the nine p2.transport kinds default ON (=0 off). DEVCREATE is not
# among them: its bake is boot.device_create's own (native/devcreate-fix, 2026-10-15, plan devcreate_bake: default ON
# since then, =0 the guest bodies). The older devcreate plans pin their OFF arms =0 (DEVCREATE_OFF) as the per-kind
# transport plans pin the baked kinds.
TRANSPORT_BAKED = ("SR_NATIVE_CALLBACK", "SR_NATIVE_GATE", "SR_NATIVE_SWAP_BLOCK", "SR_NATIVE_SWAP", "SR_NATIVE_BRACKET",
                   "SR_NATIVE_IDLE_WAIT", "SR_NATIVE_INVAL_RANGE", "SR_NATIVE_LOCK_COHER", "SR_NATIVE_TRANSPORT")
TRANSPORT_OFF = ",".join(k + "=0" for k in TRANSPORT_BAKED)   # the bake's =0 arm: the pre-bake transport
# The display-registers bake (native/dreg-bake, 2026-10-15; NATIVE_ROADMAP "DISPLAY-REGISTERS BAKE PLAN"): SR_NATIVE_VBLANK
# default ON (=0 keeps the guest 82111E38, the guest ISR's source 0, the scan-out answers and the vsync-enable bit; an
# explicit =1 without its requirements is refused, the default stands down to today's path). Every plan before it ran
# with the gate OFF - its default then - so each of their roll and gate steps that does not name the key RUNS with
# VBLANK_OFF (run_gates): the arms as they ran (the transport / setconst / flush-split bakes' rule, applied where the env
# is built so the steps read as written). display_registers names it in every roll already; the bake plan
# ("vblank_default") leaves its default arms unset.
VBLANK_OFF = "SR_NATIVE_VBLANK=0"


def genv(*parts):
    """Gate strings merged left to right: a later KEY=value replaces an earlier one in place (each key where it first
    appeared), so a plan's pins and its own settings never name a key twice (perf_run would apply the last)."""
    keys, vals = [], {}
    for part in parts:
        for kv in [x for x in (part or "").split(",") if x]:
            k, _, v = kv.partition("=")
            if k not in vals:
                keys.append(k)
            vals[k] = v
    return ",".join("%s=%s" % (k, vals[k]) for k in keys)
COUNTER_TAGS = ("[NATIVE_FLUSH] exit", "[NATIVE_FLUSH] periodic", "[NATIVE_METER_RUN]", "[CONST_DEDUP] exit",
                "[GPU_CP] census", "[NATIVE_SUBMIT] census", "[RINGLESS]", "[NATIVE_TRANSPORT]", "[NATIVE_FLUSH_BAD]", "[NATIVE_FLUSH_DROP]")


# ------------------------------------------------------------------------------------------ the plans

def ev(eid, kind, authority, observes, **extra):
    d = {"id": eid, "kind": kind, "root": "umbrella", "path": None, "authority": authority, "observes": observes}
    d.update(extra)
    return d


def freeze_ab(eid, note=""):
    return ev(eid, "freeze_grade", "frozen-ab", "pixels", ab_present_min=1000, note=note)


def freeze_exact(eid, note=""):
    return ev(eid, "freeze_grade", "live-exact", "pixels", note=note)


def gate_item(eid, note=""):
    return ev(eid, "flicker_gate", "live-exact", "pixels", note=note)


def meter_item(eid, base_tag, max_delta, note=""):
    return ev(eid, "meter_delta", "live-observe", "meter", base=base_tag, field="pm4", max_delta=max_delta,
              world_min=0.5, note=note)


def kind_zero_item(eid, base_tag, kind, base_min, target_max=0.1, note=""):
    """A converted producer measured by its OWN packet kind (the orchestrator, 2026-10-11): the base roll's census shows
    it (kind per ingame frame >= base_min) and the gated roll holds none of it (<= target_max). The total pm4 change is
    information, not the bound (a total bound from an older census fits the bar to whatever else landed since)."""
    return ev(eid, "meter_delta", "live-observe", "meter", base=base_tag, field="kind_" + kind, base_min=base_min,
              max=target_max, world_min=0.5, note=note)


def chk_item(eid, note=""):
    return ev(eid, "text", "generation", "generation", select=r"\[NATIVE_FLUSH\] periodic CHECK", which="last",
              pattern="BAD bytes=0 len=0 dev=0 ", note=note)


def chk_kind_item(eid, kind, note=""):
    """The dry CHECK reached THIS bracket kind (native/devcreate-fix, Astra round 1 P2: an aggregate BAD 0 says nothing
    about an emitter the CHECK never compared - removing a kind's CHECK calls leaves the draw checks to satisfy it): the
    last periodic CHECK line's per-kind tail `kinds full/bad: ... k<kind>=<completed>/<bad>` must show at least one
    COMPLETED comparison of the kind and none bad. Kinds: 8 ResetState 8210E520, 9 the segment re-emit 820F0048."""
    return ev(eid, "text", "generation", "generation", select=r"\[NATIVE_FLUSH\] periodic CHECK", which="last",
              pattern=r"\| kinds full/bad:(?: k\d+=\d+/\d+)* k%d=[1-9]\d*/0(?: |$)" % int(kind), note=note)


def cb_item(eid, chk=False, pm4=False, note=""):
    """p2.transport step 1 (SR_NATIVE_CALLBACK): the last [NATIVE_FLUSH] callback line - records taken and applied
    (chk=False), or the leaf's CHECK against the renderer under SR_NATIVE_FLUSH_CHECK (chk=True: generation). pm4=True:
    the CPPM4 split renders the records back to PM4, so the CP's handler applies none (applied may be 0)."""
    if pm4:
        return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] \w+ callback calls=", which="last",
                  pattern=r"calls=[1-9]\d* decl=\d+ applied=\d+ predSkip=\d+ chk=\d+ chkBad=0", note=note)
    if chk:
        return ev(eid, "text", "generation", "generation", select=r"\[NATIVE_FLUSH\] \w+ callback calls=", which="last",
                  pattern=r" chk=[1-9]\d* chkBad=0", note=note)
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] \w+ callback calls=", which="last",
              pattern=r"calls=[1-9]\d* decl=\d+ applied=[1-9]\d* predSkip=\d+ chk=\d+ chkBad=0", note=note)


def replay_item(eid):
    return ev(eid, "replay_cmp", "replay", "pixels")


# p2.zpd (docs/NATIVE_ROADMAP.md P2.12): the [NATIVE_ZPD] census line (gfx/native_zpd.cpp logLine, cumulative counters
# printed at every [GPU_CP] census: count-safe by value, never by line count). Field order:
#   mode= issued= ended= drained= resolved= written= late= stale= orphan= abandoned= evicted= dropped= lost= badAddr=
#   covered= fallback= superseded= poolFull= missed= overSegs= flushes= flushFail= waitMs= maxWaitMs= segs= spp1=
#   spp2= spp4= zero= nonzero= lastHost= lastGuest= live= inflight= scale= viaPartial= viaPresent= probes=
#   depthKilled= psKilled= noRaster= lagAvgMs= lagMaxMs=
# (the zpd4 round: the visibility probe classifies every 0 answer; [NATIVE_ZPD] draw #n lines log the first query
# draws' state - neither matches ZPD_SELECT)
ZPD_SELECT = r"\[NATIVE_ZPD\] (census|final|exit) mode="
# zpd6 (2026-10-15): seed 12, not 04 - the seed library's load checks (one ~45 s play each, exe 2087ca4acdb4): seed 12
# zpd +30 (a far marker ahead of the spawn), seed 04 +0 twice; zpd5's strict `on` / `cppm4` on 04 issued no query.
ZPD_SEED = "archive/saves/12_ch12_after_bizarro1"   # Metropolis after Bizarro 1: metro events, BIZobj15's marker
ZPD_TAPE = "tools/perf/zpd_play.txt"            # save-started play: 5 blocks of the seed verify's play, ~3.8 min
# The precondition of every zpd roll and census item (zpd5, 2026-10-15): the game issued a query at all - the roll's
# last [GPU_CP] census reads zpd > 0. A roll without one is VOID (repeated once: Runner.do_roll, REQUIRE_REROLLS), a
# second one NO DATA; the ledger grades such an item NO DATA (tools/native_ledger.py `require`), never PASS, so it
# resolves nothing.
ZPD_FIRED = {"name": "[GPU_CP] zpd>0", "select": r"\[(?:GPU_CP|NATIVE_SUBMIT)\] census rptr=", "pattern": r" zpd=[1-9]\d* ",
             "show": r"zpd=\d+ \(fake \d+\)", "why": "the game issued no occlusion query ([GPU_CP] census zpd=0)"}
REQUIRE_REROLLS = 1  # a roll whose precondition (Step.require) is unmet is repeated once, then it is NO DATA
# zpd7, the Xenia oracle (docs/NATIVE_ROADMAP.md P2.12 item 14; tools/perf/zpd_xenia.py): Xenia's trace-dump built by
# tools/xenia-trace/build_zpd_trace_dump.ps1 (0bd090dbe + the headless and zpd oracle patches), --xenia-dump overrides
XENIA_DUMP = os.path.join(LANE, "archive", "zpd", "xenia_trace_dump", "xenia-gpu-d3d12-trace-dump.exe")
# ... calibrated on Xenia's own gameplay trace and its golden render (the oracle build's, on the GPU)
XENIA_GOLDEN = (os.path.join(DEV, "archive", "xenia_oracle_gameplay_20260925", "xtr", "454107ED_25023.xtr"),
                os.path.join(DEV, "archive", "xenia_oracle_gameplay_20260925", "xtr", "454107ED_25023.png"))
ZPD_TRACES = 12  # SR_PM4_TRACE_ZPD: traces per xtrace roll, each from the frame after a HUD glow query
# zpdx2 (2026-10-15): four single traced frames held 0 / 0 / 2 / 2 queries - the HUD issues its glow queries in short
# bursts (zpd7's strict roll: 8 markers' queries in 3 consecutive frames, then ~4 frames without), never the 20 pairs
# the item needs. Each trace now holds SR_PM4_TRACE_FRAMES consecutive frames (Xenia plays them all: the oracle patch).
ZPD_TRACE_FRAMES = 16
# zpdx3 (2026-10-16, exe 9e97d2c10419): 4 traces x 16 frames, every frame recorded and played, 14 pairs (2 / 4 / 5 / 3),
# all agreeing - FAIL on paired 14 < 20. The sizing assumed zpdx2's ~1 query a frame; the measured yield is 3.5 pairs a
# 16-frame trace (one marker in view: one slot, the next query only after GetData answered the last, ~1 per 3 traced
# frames). The four traces were spent on the roll's sparse start (swaps 8690-12373; its 1,209 queries came mostly after
# swap 15,960). zpdx4: 12 traces (expected 12 x 3.5 = 42 pairs; 24 at zpdx3's worst trace, 2) at least ZPD_TRACE_GAP
# swaps apart (SR_PM4_TRACE_ZPD_GAP; 600 by default), so the quota fits the roll's query window even though tracing
# stalls the game while the wall-clock tape plays on (~10 s a trace), and a longer dwell (ZPD_TRACE_DWELL s after the
# HUD) so the roll keeps playing after the tape.
ZPD_TRACE_GAP = 300
ZPD_TRACE_DWELL = 420


def zpd_xenia_item(eid, note=""):
    """The Xenia oracle's grade (tools/perf/zpd_xenia.py): our answers and Xenia's (occlusion_query=strict) for the same
    END reports of the same traced frames agree on visible / occluded."""
    return ev(eid, "json", "xenia", "pixels", checks=[
        {"field": "verdict", "value": "PASS"}, {"field": "calibration.ok", "value": True},
        {"field": "paired", "op": "ge", "value": 20}, {"field": "unpaired", "value": 0},
        {"field": "agree_rate", "op": "ge", "value": 0.98}], note=note)


def require_met(log, req):
    """(met, what) of a precondition {name, select, pattern, show, why} on a roll's run.log: the LAST line matching
    select must match pattern (tools/native_ledger.py's `require` reads it the same way). what: the line's `show` match,
    else the line (or 'no line')."""
    last = None
    if log and os.path.exists(log):
        with open(log, "rb") as f:
            for raw in f:
                ln = raw.decode("utf-8", "replace")
                if re.search(req["select"], ln):
                    last = ln
    if last is None:
        return False, "no line /%s/" % req["select"]
    m = re.search(req["show"], last) if req.get("show") else None
    return re.search(req["pattern"], last) is not None, m.group(0) if m else last.strip()[:120]


def zpd_item(eid, mode, tail, note=""):
    """The roll's last census line: queries issued, ended, resolved and written in `mode`, then `tail`."""
    return ev(eid, "text", "live-observe", "generation", select=ZPD_SELECT, which="last",
              pattern=r"mode=%s issued=[1-9]\d* ended=[1-9]\d* drained=[1-9]\d* resolved=[1-9]\d* written=[1-9]\d* %s" % (
                  mode, tail), require=ZPD_FIRED, note=note)


def zpd_balance_item(eid, mode, note=""):
    """resolved == ended on the last census line with nothing in flight (inflight=0) and nothing lost (lost=0).
    Astra merge review P2 (2026-10-15): the select is the mode's census line alone, so `last` is the FINAL census and
    balance and zero outstanding are asserted on it (the pattern) - lost=0 / inflight=0 in the select let an earlier
    balanced census stand in for an unbalanced final one."""
    return ev(eid, "text", "live-observe", "generation",
              select=ZPD_SELECT + mode + " ", which="last",
              pattern=r" ended=(\d+) drained=\d+ resolved=\1 .* lost=0 .* inflight=0 ", require=ZPD_FIRED, note=note)


# p2.zpd's BAKE (native/zpd-bake, 2026-10-16; docs/NATIVE_ROADMAP.md "p2.zpd BAKE PLAN"): SR_NATIVE_ZPD default ON in the
# latency model. Every plan before it ran the gate unset - OFF, its default then - so a re-run on a baked exe pins
# ZPD_OFF (zpd_gates: the arms as they ran, the transport / devcreate / display-registers bakes' rule); the bake's own
# plan ("zpd_default") leaves its default arms unset. Its =0 arms (the guest path - 82101E08's guest body, EVENT_WRITE_ZPD
# as guest PM4, no host query: the A/B arm) set ZPD_OFF.
ZPD_OFF = "SR_NATIVE_ZPD=0"
# THE DEFAULT FORM (as the display-registers bake's, Astra round 1 on native/dreg-bake): the latch logs its form once a
# process (gfx/native_zpd.cpp latchRequest): "SR_NATIVE_ZPD on (the baked default; latency)", "on (=<value>; <mode>)" or
# "off (=<value>)". The bake's default arms require the default form and forbid the others (zpd_default_item); a live
# run of a plan with "default_keys" refuses to start while the parent environment carries one (Runner.env_refusal).
ZPD_LATCH_SELECT = r"\[NATIVE_ZPD\] SR_NATIVE_ZPD (?:on|off) \("
# Selects of lines printed ONCE a process, when a gate latches (not by a family printer): the graceful quit's terminal
# snapshot never repeats them, so Runner.terminal_items adds no terminal twin for them
# mem.resources BAKED (memres_bake): the route line prints once (the shared-memory init), the patch totals only in the
# terminal snapshot (kernel/quit.cpp's resource_memory family) - neither has a terminal twin to grade
MEMRES_ROUTE_SELECT = r"\[MEM_RES_NATIVE\] route "
MEMRES_PATCH_SELECT = r"\[MEM_RES_NATIVE\] patches terminal=1"
ONCE_PER_PROCESS = (ZPD_LATCH_SELECT, MEMRES_ROUTE_SELECT, MEMRES_PATCH_SELECT)


def zpd_gates(plan, gates):
    """A roll's or the gate's env as it RUNS (Runner.gates_for / do_gate): the step's own gates and, for a plan before
    the zpd bake (no "zpd_default"), ZPD_OFF appended when the step does not name SR_NATIVE_ZPD - the gate OFF it ran
    with, its default then - so a re-run on a baked exe repeats the arms as they ran. A step that names the key (the zpd
    plans' arms) and the bake plan's arms are left as written; the plan's own gates (the replay) are not a roll's. The
    steps keep their gates as written: the pin is visible in the dry run."""
    names = [kv.split("=", 1)[0] for kv in (gates or "").split(",")]
    if plan.get("zpd_default") or "SR_NATIVE_ZPD" in names:
        return gates
    return genv(gates, ZPD_OFF)


def zpd_default_item(eid, note=""):
    """A default arm of the bake ran the DEFAULT form: the latch's line names the baked default in the latency model;
    an explicit form (=1 / =2 / ...) or the off line anywhere in the roll fails it - an exported SR_NATIVE_ZPD would
    have made the "default" arm explicit (perf_run passes the parent env to every roll)."""
    return ev(eid, "text", "live-observe", "generation", select=ZPD_LATCH_SELECT, which="last",
              pattern=r"\[NATIVE_ZPD\] SR_NATIVE_ZPD on \(the baked default; latency\)",
              forbid=[r"\[NATIVE_ZPD\] SR_NATIVE_ZPD on \(=", r"\[NATIVE_ZPD\] SR_NATIVE_ZPD off \("], note=note)


def zpd_bake_item(eid, ab=False, note=""):
    """THE STRICT WITNESS of a default roll: the roll's last census in the latency model - queries issued, ended,
    drained, resolved and written; none evicted, dropped, lost or misaddressed; no pool exhaustion, no flush failure;
    the CP never waited (waitMs 0.0); every answer MEASURED by the GPU - answer segments opened (segs > 0), no draw the
    heap could not measure (missed 0: gfx/native_zpd.cpp segmentMissed, the conservative fallback's 'visible' answer
    counted resolved / written all the same) and no query over its segment cap (overSegs 0), as the zpd plan's strict
    witness (zpd_strict) requires - and the producer port took the queries' packets as records (port > 0, the kRkZpd
    record of 82101E08's port) and declined none (portDecl 0). ab=True (the per-present A/B, SR_AB_TOGGLE=
    SR_NATIVE_ZPD): both arms ran - portDecl > 0 counts the guest arm's 82101E08 calls. Astra round 1 on native/zpd-bake
    (P2-1): without missed / overSegs / segs the witness passed fallback answers."""
    return ev(eid, "text", "live-observe", "generation", select=ZPD_SELECT + "latency ", which="last",
              pattern=r"mode=latency issued=[1-9]\d* ended=[1-9]\d* drained=[1-9]\d* resolved=[1-9]\d* written=[1-9]\d* "
                      r".* evicted=0 dropped=0 lost=0 badAddr=0 .* poolFull=0 missed=0 overSegs=0 flushes=\d+ "
                      r"flushFail=0 waitMs=0\.0 maxWaitMs=\S+ segs=[1-9]\d* .* "
                      r"port=[1-9]\d* portDecl=%s(?:\s|$)" % (r"[1-9]\d*" if ab else "0"),
              require=ZPD_FIRED, note=note)


def zpd_off_item(eid, note=""):
    """THE OFF WITNESS of an =0 roll: the latch's line says off (=0) - the guest path, 82101E08's guest body, no host query
    - with no [NATIVE_ZPD] census line (nothing answered natively) and no On line, while the game's queries reached the
    CP as guest PM4 (the precondition: [GPU_CP] zpd > 0)."""
    return ev(eid, "text", "live-observe", "generation", select=ZPD_LATCH_SELECT, which="last",
              pattern=r"\[NATIVE_ZPD\] SR_NATIVE_ZPD off \(=0\)",
              forbid=[ZPD_SELECT, r"\[NATIVE_ZPD\] SR_NATIVE_ZPD on \("], require=ZPD_FIRED, note=note)


def zpd_meter_item(eid, base_tag, note=""):
    """The meter on the Metropolis seed: the default roll's PM4 per ingame frame <= 0.05 (the zero form: the queries'
    packets travel as records) and never above the =0 roll's (delta <= 0). The =0 roll's level is information: its HUD
    issues each marker's query once and waits for an answer that never comes (zpd7 off: 24 EVENT_WRITE_ZPDs over the
    roll, ~0.004 PM4 a frame) - the census pair is the exact producer measure. The precondition on BOTH rolls (zbk2,
    2026-10-16: the meter graded PASS over a NO DATA =0 roll): either roll without a query grades the item NO DATA
    (tools/native_ledger.py require_rolls)."""
    return ev(eid, "meter_delta", "live-observe", "meter", base=base_tag, field="pm4", max_delta=0.0, max=0.05,
              world_min=0.5, note=note, require=ZPD_FIRED)


# THE QUERIES INSIDE THE GRADED WINDOWS (Astra round 1 on native/zpd-bake, P2-2; tools/perf/zpd_activity.py): lifetime
# census counters do not show that the queries ran where a step grades. The pacing roll's queries are graded inside
# the 28,672-present window p2_trace grades (each quarter active); a frozen pass's inside each hold's COMPARED presents,
# bracketed exactly by the zpd census at the hold's edges (kernel/highfps freezeEdgeCensus) - the query-bearing holds'
# compared presents >= the A/B's own minimum, 1,000 (ab_present_min).
# THE FROZEN ROLLS' SCHEDULE (zbk2; archive/zbk/freeze_schedule_sim.py over zbk1 / zpd7 / zpd8's periodic census): a
# 2-s hold every 4 s (2 s gap) from 141 s on the sim clock (its first tick ~12 s into the log; the HUD at ~101 s on it
# when no hold precedes it - probe / meter_on / pace1 all at +112.5-112.8 s), so the first hold lands ~HUD+40 s and
# ~54 holds fit the 260-s dwell. zbk1's schedule (8:2000:80, from before the HUD) put 13-19 of 31 holds where the
# queries ran in the unperturbed rolls and only 7 in its own A/B; this one 33-48 of 54 in each (pace1 48, meter_on 33,
# probe 33, zpd8 lat 36, zpd7 lat 44): >= 33 x 40 = 1,320 compared presents. The holds take half the dwell's wall time
# (the tape plays on), which those unperturbed rolls cannot show - the grade is zpd_activity's on the roll itself.
ZPD_FREEZE = "SR_DIAG_SIM_FREEZE=2:2000:141"
# zbk3 (Astra round 2's per-present witness): a marker's query rides ~1 present in 2 while the frozen pass is dumped
# (the HUD re-issues it once answered: zbk2's A/B answered in 29 ms on average, its dumps ~19 ms apart), so the holds'
# 40 dumps (zbk2's A/B: 400 .. ~1,130 ms of each 2-s hold) would give ~1,100 query-bearing compared presents over ~55
# holds against the 1,000. 80 dumps a hold (400 .. ~1,900 ms: the same holds, nothing else changed) double them; the
# frozen rolls write ~16 GB each to the stage (zbk2's ~8 GB).
ZPD_DUMP_CAP = "SR_PRESENT_DUMP_FREEZE_CAP=80"
ZPD_AB_MIN_PRESENTS = 1000  # freeze_ab's ab_present_min: the A/B's own minimum
# Astra round 2 (P2-1): the per-present A/B's query-bearing compared presents by arm (the frame stamp's form: the port's
# kRkZpd records, the guest body's literal packets) - each arm a quarter of the minimum, so a query cadence locked to
# the A/B's alternation (every query on one arm) cannot pass
ZPD_AB_MIN_ARM = ZPD_AB_MIN_PRESENTS // 4
# THE PARKED WORKLOAD (zbk2's follow-up, 2026-10-16): zbk2's blind flight left the markers behind (pace1: 12 queries in
# its graded window; the A/B 16 query-bearing holds of 55; both =0 meter rolls none). Seed 18 at the spawn with nothing
# pressed: the only seed whose HUD glow query fired before any input in the seed library's load checks (zpd +2 at
# HUD+5.8 s, exe 2087ca4acdb4), and every Metropolis seed loads at the same spawn and heading - so the view, and the
# marker in it, do not depend on how the frames were paced. Plan zpd_park (information, no ledger) measures it with
# zbk2's three failed witnesses before any bake roll uses it.
ZPD_PARK_SEED = "archive/saves/18_ch18_before_twister"   # Metropolis before the Twister (TWSobj04 next)
ZPD_PARK_TAPE = "tools/perf/zpd_park_play.txt"           # the prelude, then nothing pressed (~4 min of sample marks)
# THE KEEP-ALIVE (zpk1's follow-up, 2026-10-16): zpk1's parked rolls queried only HUD+5..~20 s - the game's IDLE CAMERA
# (10 s of game time with no pad input and no movement: IdleCam_TriggerCheck_sub_82258240 -> IdleCam_Activate_sub_8224A978,
# SetInterfaceState 2) hid the HUD for the rest of each roll. The same seed and view with a D-pad UP tap every 2.5 s
# through the harness test keymap (the pad report: the game's own input commands stamp its input-idle clock); D-pad up
# selects one power, the same on every press. Plan zpd_keep (information, no ledger) measures it before any bake roll.
ZPD_KEEP_TAPE = "tools/perf/zpd_keepalive_play.txt"      # the prelude, then a D-pad up tap every 2.5 s for 1,320 s


def zpd_window_item(eid, mode, note=""):
    """tools/perf/zpd_activity.py's grade of the queries inside a graded window (its run-unique JSON, the step's verdict
    stamped and committed last): mode "pacing" - issued, measured (segs) and ported inside the 28,672-present window
    p2_trace graded, each quarter of it active, the census reaching both ends; "freeze" / "freeze_ab" (Astra round 2,
    P2-1 / P2-2) - the presents the pixel grade compared (freeze_diff.py's compared rows in the holds it counted), each
    by its own frame stamp: every one stamped, >= 1,000 of them query-bearing (freeze_ab: the static pairs only, and
    each arm - the port's record frames, the guest body's packet frames - >= ZPD_AB_MIN_ARM of them)."""
    checks = [{"field": "committed", "op": "eq", "value": True},
              {"field": "runner.verdict", "op": "eq", "value": "PASS"},
              {"field": "verdict", "op": "eq", "value": "PASS"}]
    if mode == "pacing":
        checks += [{"field": "checks.%s" % k, "op": "eq", "value": True} for k in (
            "samples_cover_window", "issued_in_window", "segs_in_window", "port_in_window", "every_quarter_active")]
    else:
        checks += [{"field": "checks.compared_listed", "op": "eq", "value": True},
                   {"field": "checks.stamps_logged", "op": "eq", "value": True},
                   {"field": "query_bearing_presents", "op": "ge", "value": ZPD_AB_MIN_PRESENTS}]
        if mode == "freeze_ab":
            checks += [{"field": "ab", "op": "eq", "value": True},
                       {"field": "checks.both_arms", "op": "eq", "value": True},
                       {"field": "min_arm", "op": "ge", "value": ZPD_AB_MIN_ARM}]
    return ev(eid, "json", "live-observe", "generation", checks=checks, note=note)


def present_gate_item(eid, pm4=False):
    """Sampled gate producer/application activity; not pacing or complete-run totals."""
    applied = r"\d+" if pm4 else r"[1-9]\d*"
    return ev(eid, "text", "live-observe", "generation",
              select=r"\[NATIVE_FLUSH\] \w+ presentGate calls=", which="last",
              pattern=r"calls=[1-9]\d* skipped=\d+ decl=\d+ applied=" + applied,
              note="Retained periodic sample only; SDK declines and live pacing remain separate.")


def swap_sdk_items(prefix, pm4=False):
    """Nonvacuous sampled SDK producer and reserved-block consumer witnesses.

    Declines remain visible and must be classified; these samples are neither
    drained totals nor proof that the entire SDK/release path is native.
    """
    applied = r"\d+" if pm4 else r"[1-9]\d*"
    note = "Periodic activity only; declines, helpers, pacing and complete-run accounting remain separate."
    return [ev(prefix + "_sdk", "text", "live-observe", "generation",
               select=r"\[NATIVE_FLUSH\] \w+ swapSdk calls=", which="last",
               pattern=r"calls=[1-9]\d* decl=\d+", note=note),
            ev(prefix + "_block", "text", "live-observe", "generation",
               select=r"\[NATIVE_FLUSH\] \w+ swapBlock calls=", which="last",
               pattern=r"calls=[1-9]\d* decl=\d+ applied=" + applied, note=note)]


def transport_pair_item(eid):
    """The pair's pure-ON roll (SR_NATIVE_TRANSPORT=1, no A/B): the last [NATIVE_TRANSPORT] census line - both
    submission routes (chain: a close while the pump has work, dev+0x2A74 != 0; direct: none outstanding), the pump's
    park and no native call declined, and no call sent to the SDK by an A/B window (abOffAppend / abOffEnd 0: every
    gated call was native). Bounds from the V14-V17 censuses of this scenario (park ~1 per present, chain > 11k)."""
    return ev(eid, "text", "live-observe", "generation",
              select=r"\[NATIVE_TRANSPORT\] \w+ append=", which="last",
              pattern=r"append=[1-9]\d* chain=[1-9]\d* direct=[1-9]\d* park=[1-9]\d* resume=\d+ decline=0 .*"
                      r"\| abOffAppend=0 abOffEnd=0",
              note="Sampled native submission/park activity, both submission routes observed. "
                   "Not drained totals, full pump, arena lifetime, Native0 or composition proof.")


def transport_pair_ab_item(eid):
    """The pair's per-present A/B roll (SR_AB_TOGGLE=SR_NATIVE_TRANSPORT, SR_AB_PRESENT=1): both arms ran - the native
    arm took both routes and parked with no decline, and the SDK arm's calls are counted (abOffAppend / abOffEnd > 0)."""
    return ev(eid, "text", "live-observe", "generation",
              select=r"\[NATIVE_TRANSPORT\] \w+ append=", which="last",
              pattern=r"append=[1-9]\d* chain=[1-9]\d* direct=[1-9]\d* park=[1-9]\d* resume=\d+ decline=0 .*"
                      r"\| abOffAppend=[1-9]\d* abOffEnd=[1-9]\d*",
              note="Both arms of the per-present A/B observed in sampled counts; not drained totals.")


def transport_pair_trace_item(eid, note=""):
    """trace_order_item plus the drain rule the pair carries: it replaces the pump's EndSync 82110768, the call that
    drains dev+0x2A74, so every BlockUntilIdle return saw the count at 0 (idle.a74_nonzero_exit 0; NATIVE_ROADMAP
    'BlockUntilIdle drained'), besides the entries completing and no drain spin over a frame (trace_item)."""
    it = trace_order_item(eid, note)
    it["checks"] = it["checks"] + [{"field": "idle.a74_nonzero_exit", "op": "eq", "value": 0}]
    return it


def setconst_item(eid, note=""):
    """p2.setconst_mem: 82113010's Entries actually ran in the roll - the last [NATIVE_FLUSH] periodic draws line's
    setConstMem= count (calls taken as kRkSetConstMem items; decl / fail are information), so an item graded on the roll
    is not vacuous for the per-tile constants. (cpPm4= follows on exes since native/setconst-bake's CP case.)"""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] periodic draws", which="last",
              pattern=r" \| setConstMem=[1-9]\d* decl=\d+ fail=\d+ (?:cpPm4=\d+ )?\| ", note=note)


def setconst_cppm4_item(eid, note=""):
    """The consumer side of the CPPM4 split for P2-B item 5 (Astra round 1 on native/setconst-bake): the last periodic
    draws line's cpPm4= - kRkSetConstMem records the CP rendered back to PM4 itself (cpRecordPm4's case) and executed -
    above 0. Without it a CPPM4 roll can pass while every setconst record ran through nativeRecord (the opaque fallback)."""
    return ev(eid, "text", "live-exact", "generation", select=r"\[NATIVE_FLUSH\] periodic draws", which="last",
              pattern=r" \| setConstMem=[1-9]\d* decl=\d+ fail=\d+ cpPm4=[1-9]\d* \| ", note=note)


def scr_item(eid, mode="strict", note=""):
    """p1.row3_construns (native/construns-bake): the last [NATIVE_FLUSH] shaderConstRuns line (kernel/native_flush.cpp
    report; every field cumulative from boot to the periodic / exit line): the gate ON, native calls taken, the chunk
    path never taken and the dev+44 - dev+48 margin always the XDK's 160 (the static caller-domain bound's live
    preconditions: every caller's <= 6-group mask fits the fast path), no tessellated draw (tessDraws 0, tessSdk 0: the
    live witness of rendered_tessellation for this roll). mode "strict" (a pure-ON roll): no SDK body anywhere (decl 0,
    abGuest 0) - what the census pair measures in packets; "cppm4": strict and the CP rendered the sync item back to PM4
    (cpPm4 > 0, the consumer side); "stress": strict and the entry check switched segments (kick > 0, the entry-switch
    path live under SR_NATIVE_SEG_STRESS); "ab": both arms of the per-present A/B ran (abGuest > 0: the off window's
    Driver calls; decl counts the off window's leaf calls by design)."""
    guest = {"ab": r"abGuest=[1-9]\d* decl=\d+"}.get(mode, r"abGuest=0 decl=0")
    kick = r"kick=[1-9]\d*" if mode == "stress" else r"kick=\d+"
    cp = r"cpPm4=[1-9]\d*" if mode == "cppm4" else r"cpPm4=\d+"
    return ev(eid, "text", "live-exact" if mode in ("cppm4",) else "live-observe", "generation",
              select=r"\[NATIVE_FLUSH\] \w+ shaderConstRuns calls=", which="last",
              pattern=r"calls=[1-9]\d* chunked=0 \| gate=1 drv=\d+ sdk=\d+ " + guest +
                      r" \(off \d+ oracle \d+ mask \d+ span \d+ open \d+ emit \d+\) \| " + kick +
                      r" marginBad=0 \| tessDraws=0 tessSdk=0 \| " + cp + r"(\s|$)",
              note=note or "Cumulative counters (the last periodic / exit line)")


def construns_census_item(eid, note=""):
    """p1.row3_construns' OBSERVATION from tools/perf/transport_census.py --construns-zero over the census pair (census0
    SR_NATIVE_SHADER_CONST_RUNS=0, census1 the baked default; both SR_NATIVE_BRACKETS=1): both censuses complete and
    reconciled, every producer mapped, the base's S1 producer lines carrying 821075E0's SDK-body preamble (3C + r0A, 3 a
    call) and the target's none over its census-covered INGAME interval (with no preamble no SDK body ran there; the
    r20 / r23 left are the S1 fallbacks' runs, reported), none on the target's UNSTAMPED / RING_UNSTAMPED lines either
    (Astra round 1, P2: lost attribution fails closed), and no tessellated draw (820FC478's line) in either roll. The
    JSON is the step's run-unique output: committed last, the step's own verdict stamped in it."""
    return ev(eid, "json", "live-observe", "census", note=note, checks=[
        {"field": "committed", "op": "eq", "value": True},
        {"field": "runner.verdict", "op": "eq", "value": "PASS"},
        {"field": "verdict", "op": "eq", "value": "PASS"},
        {"field": "checks.base_construns_exercised", "op": "eq", "value": True},
        {"field": "checks.target_construns_sync_zero", "op": "eq", "value": True},
        # Astra round 1 (P2): a preamble whose attribution was lost (UNSTAMPED / RING_UNSTAMPED) fails closed
        {"field": "checks.target_unattributed_sync_zero", "op": "eq", "value": True},
        {"field": "target.construns.unattributed_sync_pk", "op": "eq", "value": 0},
        {"field": "checks.base_tessellated_zero", "op": "eq", "value": True},
        {"field": "checks.target_tessellated_zero", "op": "eq", "value": True},
        {"field": "checks.base_reconciled", "op": "eq", "value": True},
        {"field": "checks.target_reconciled", "op": "eq", "value": True},
        {"field": "checks.base_frames_complete", "op": "eq", "value": True},
        {"field": "checks.target_frames_complete", "op": "eq", "value": True},
        {"field": "checks.base_covers_run", "op": "eq", "value": True},
        {"field": "checks.target_covers_run", "op": "eq", "value": True},
        {"field": "checks.base_mapped", "op": "eq", "value": True},
        {"field": "checks.target_mapped", "op": "eq", "value": True},
        {"field": "target.construns.sync_pk", "op": "eq", "value": 0}])  # always written (native/construns-bake)


def census_item(eid, note=""):
    return ev(eid, "text", "live-observe", "guest-answers", select=r"\[(?:GPU_CP|NATIVE_SUBMIT)\] census", which="last",
              pattern=r"badIB=0 desync=0", note=note)


# tools/graceful-quit (docs/TOOLKIT.md 1.8m; --graceful-quit): items that hold only on a roll ended by the graceful
# quit. They are ADDED next to a roll's own items, never in place of them: the pre-registered items keep their regexes.
def terminal_complete_item(eid, note="", forbid_stranded=False):
    """The roll ended on a complete terminal snapshot (kernel/quit.cpp): the [QUIT] line after the quit request is the
    complete one, never FAILED, with at least one frame - every census item of the roll then reads the whole run.
    forbid_stranded (native/quit-drain): also no thread stranded in a GPU wait the ring cut made unreachable - the
    complete line's stranded=0 (a stranded thread is acknowledged, never silent: stranded=N is on the line either way)."""
    pattern = r"\[QUIT\] terminal snapshot complete frames=[1-9]\d* "
    if forbid_stranded:
        pattern += r".* stranded=0\s*$"
    return ev(eid, "text", "live-observe", "guest-answers", select=r"\[QUIT\] terminal snapshot (?:complete|FAILED) ",
              which="last", pattern=pattern, note=note)


def census_terminal_item(eid, note=""):
    """census_item's terminal twin: the CP census of the drained CP over the WHOLE run (its why token `terminal`)."""
    return ev(eid, "text", "live-observe", "guest-answers", select=r"\[(?:GPU_CP|NATIVE_SUBMIT)\] terminal ", which="last",
              pattern=r"badIB=0 desync=0", note=note)


# The quit receipt (tools/sr_quit.ps1 Write-SrQuitRecord, beside the roll's run.log): the game ended ITSELF, exit 0,
# after the terminal snapshot - not by its hard deadline (82) after the log closed, not by a Stop-Process fallback
# (Astra round 1, P1-3: the complete line alone is not exit evidence).
QUIT_RECEIPT_CHECKS = [{"field": "schema", "op": "eq", "value": "sr-quit/1"},
                       {"field": "result", "op": "eq", "value": "complete"},
                       {"field": "graceful", "op": "eq", "value": True},
                       {"field": "fellBack", "op": "eq", "value": False},
                       {"field": "exitCode", "op": "eq", "value": 0}]


def quit_receipt_item(eid, note="", forbid_stranded=False):
    """forbid_stranded (native/quit-drain): the receipt's stranded count (tools/sr_quit.ps1 Add-SrQuitTerminalInfo,
    read from the game's complete line) must be 0."""
    checks = [dict(c) for c in QUIT_RECEIPT_CHECKS]
    if forbid_stranded:
        checks.append({"field": "stranded", "op": "eq", "value": 0})
    return ev(eid, "json", "live-observe", "guest-answers", checks=checks, note=note)


# Why tokens a select can name for the reporting line it reads (the CP census's "census", the periodic reports',
# the exit lines'), or a regex standing for any of them.
WHY_TOKENS = ("census", "periodic", "final", "exit", r"\w+", r"\S+")
# Stage2b preserves the counter fields but names the native executor separately.
CP_TAG = r"\[(?:GPU_CP|NATIVE_SUBMIT)\]"
TAGGED = re.compile(r"^(" + re.escape(CP_TAG) + r"|\\\[[A-Z0-9_]+\\\])(?: (.*))?$", re.S)


def terminal_select(select):
    """A text item's select rewritten to read the SAME family's line of the terminal snapshot (Astra round 1, P2-5): the
    why token after the tag becomes `terminal` (`\\[GPU_CP\\] census` -> `\\[GPU_CP\\] terminal`, `\\[NATIVE_FLUSH\\] \\w+
    bracket begin=` -> `... terminal bracket begin=`), and a line printed without one gets it after the tag (`\\[NATIVE_FLUSH\\]
    seg tile segs=` -> `... terminal seg tile segs=`, `\\[AB_DRAIN\\] stats` -> `... terminal stats`: the snapshot prints
    those with the token). Per-frame records (`\\[PM4_COV\\] F frame=`) have no why token and are whole-run by
    themselves once the snapshot is complete: the select is kept. None = not a family line (the [QUIT] items)."""
    m = TAGGED.match(select or "")
    if not m:
        return None
    tag, rest = m.group(1), m.group(2) or ""
    if tag == r"\[QUIT\]":
        return None
    # a why token given as an alternation of why tokens (ZPD_SELECT: `\[NATIVE_ZPD\] (census|final|exit) mode=`): the
    # snapshot prints that line with the token `terminal` (the CP census family calls zpd::logLine(why))
    alt = re.match(r"^\((?:\?:)?([A-Za-z|]+)\) ", rest)
    if alt and all(w in WHY_TOKENS for w in alt.group(1).split("|")):
        return tag + " terminal " + rest[alt.end():]
    if tag == r"\[PM4_COV\]" and rest.startswith("F "):
        return select
    # native/composed-census: the counter's armed witness prints at its first hook and again in the terminal snapshot
    # (`[PM4_COV] armed terminal brackets=`); its report lines name the kind first, the why token after it
    # (`[PM4_COV] ENDS <why> frames=`, as CUM)
    if tag == r"\[PM4_COV\]" and rest.startswith("armed "):
        return tag + " armed terminal " + rest[len("armed "):]
    for kind in ("CUM ", "ENDS "):
        if tag == r"\[PM4_COV\]" and rest.startswith(kind):
            tail = rest[len(kind):]
            for w in WHY_TOKENS:
                if tail.startswith(w + " "):
                    return tag + " " + kind + "terminal " + tail[len(w) + 1:]
    if rest.startswith("terminal"):
        return select
    for w in WHY_TOKENS:
        if rest == w:
            return tag + " terminal"
        if rest.startswith(w + " "):
            return tag + " terminal " + rest[len(w) + 1:]
    return tag + " terminal" + (" " + rest if rest else "")


def terminal_twin(item, eid, note=""):
    """`item`'s predicate on the terminal snapshot: the same pattern / which / forbid / min_count, the select
    rewritten by terminal_select. None when the item is no family text item."""
    if item.get("kind") != "text":
        return None
    sel = terminal_select(item.get("select"))
    if sel is None:
        return None
    tw = {"id": eid}  # first: the ledger's one-line items lead with their id (LedgerText.find_item)
    tw.update((k, v) for k, v in item.items() if k not in ("id", "note", "path", "roll"))
    tw["select"] = sel
    tw["note"] = ("terminal twin of %s (the same predicate on the graceful quit's terminal snapshot)%s" % (
        item.get("id"), "; " + note if note else ""))
    return tw


def step_graceful(step):
    """A roll step that ends by the graceful quit by its own gates (GRACEFUL: sr_drive reads SR_DRIVE_GRACEFUL_QUIT
    from -Gates), not only under the plan-wide --graceful-quit."""
    return "SR_DRIVE_GRACEFUL_QUIT=1" in (step.gates or "").split(",")


def tile_item(eid, note=""):
    """The STRICT transport witness (SR_NATIVE_SEG_TILE=1, kernel/seg_tile.hpp; p2.segmentless v2, the orchestrator
    2026-10-11) on the last [NATIVE_FLUSH] seg tile line: the producer tile (every arena record claimed exactly once by
    a registered segment, the padding apart: gap / overlap / order 0) and the CP first application (dup / reorder 0),
    with records actually checked, and no [NATIVE_FLUSH_DROP] anywhere in the roll."""
    return ev(eid, "text", "live-exact", "generation", select=r"\[NATIVE_FLUSH\] seg tile segs=", which="last",
              pattern=r"segs=[1-9]\d* recs=[1-9]\d* unsegmented=\d+ pads=\d+ gap=0 overlap=0 order=0 "
                      r"\| first segs=[1-9]\d* recs=[1-9]\d* dup=0 reorder=0 ",
              forbid=[r"\[NATIVE_FLUSH_DROP\]"], note=note)


def tile_drained_item(eid, note=""):
    """tile_item plus the 2026-10-13 parts of the strict witness (Astra's re-bake review): across segments the first
    applications follow the close order (idreorder 0; deferred bracket segments apart), and at every return of the
    guest's BlockUntilIdle outside a Z-pass bracket every registered segment and record part had had its first
    application (drains > 0, drainMiss 0) - the drained reconciliation; and the same at the FORCED drained points
    through gameplay (SR_NATIVE_SEG_TILE_DRAIN_MS, default every 10 s: forced > 0, forcedMiss 0), the game draining
    itself only while it loads."""
    return ev(eid, "text", "live-exact", "generation", select=r"\[NATIVE_FLUSH\] seg tile segs=", which="last",
              pattern=r"segs=[1-9]\d* recs=[1-9]\d* unsegmented=\d+ pads=\d+ gap=0 overlap=0 order=0 "
                      r"\| first segs=[1-9]\d* recs=[1-9]\d* dup=0 reorder=0 \| water=\d+ head=\d+ "
                      r"\| idreorder=0 deferred=\d+ \| drains=[1-9]\d* drainBracket=\d+ drainMiss=0 "
                      r"\| forced=[1-9]\d* forcedMiss=0 forcedPostponed=\d+",
              forbid=[r"\[NATIVE_FLUSH_DROP\]", r"seg tile drain MISS", r"seg tile forced drain #\d+: .*MISS"],
              note=note)


def xport_item(eid, note=""):
    """Every record the arena reclaimed was executed (the reclaim audit's NEVER-EXECUTED=0 on the last periodic draws
    line) and no [NATIVE_FLUSH_DROP] anywhere in the roll."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] periodic draws", which="last",
              pattern=r" NEVER-EXECUTED=0 ", forbid=[r"\[NATIVE_FLUSH_DROP\]"], note=note)


def segcounters_item(eid, note=""):
    """The P2-B tripwires on the last [NATIVE_FLUSH] seg line (every 2048 closes, since 2026-10-10: the periodic
    draws line comes every 2^18 draws, too late for a boot-time wedge): idMiss, mismatch, footStray and feetLost
    must read 0; capDrop (the registry lifetime fix) and litLenBad (literal spans whose items' PM4 differed from the
    span: the CPPM4 split, segk 2026-10-10) too."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] seg closed=", which="last",
              pattern=r" idMiss=0 mismatch=0 .* footStray=0 feetLost=0 coherDropOld=\d+ capDrop=0 litLenBad=0",
              note=note)


def memres_ab_item(eid, note=""):
    """mem.resources' frozen-pass A/B (gfx/resource_ab.hpp): THE TOGGLE WITNESS on the graceful quit's terminal
    [MEM_RES_AB] totals - both arms decided (pages > 0), no decision by the wrong arm (a no-op toggle shows native
    decisions in the model sets; an always-model one the reverse), none on a thread with no latched arm, no CROSS_CLOBBER
    (the model overwriting GPU data the registry holds: outside what a shared-arena A/B can show), dumped presents of both
    arms and every pair of consecutive dumped presents from alternate arms (alt_break 0). The select names the periodic
    line; its terminal twin (terminal_select: `periodic` -> `terminal`) reads the graceful quit's snapshot (Astra review1
    P1-3: an alternation in the select made the twin's selector impossible). The abwit step (memres_abwit_item) joins
    every pair the pixel grade compared to its two presents' arms."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[MEM_RES_AB\] periodic sets", which="last",
              pattern=r"\| decisions native=[1-9]\d* model=[1-9]\d* native_in_model=0 model_in_native=0 unlatched=0 "
                      r"dec_unlatched=0 \|.* cross_clobber=0 .*\| dumped native=[1-9]\d* model=[1-9]\d* "
                      r"alt_pairs=[1-9]\d* alt_break=0$",
              forbid=[r"\[MEM_RES_AB\] UNLATCHED", r"\[MEM_RES_AB\] CROSS_CLOBBER"], note=note)


# the abwit step's minimum: static A/B pairs joined to their arms (tools/perf/mem_res_ab_check.py MIN_PAIRS; a 40-dump
# hold compares <= 39)
MEMRES_AB_MIN_PAIRS = 100


def memres_abwit_item(eid, note=""):
    """tools/perf/mem_res_ab_check.py over one memres_ab roll (Astra review1 P2-4), its run-unique JSON (the step's
    verdict stamped, committed last): the terminal [MEM_RES_AB] totals complete (every field of the schema; no terminal =
    NO DATA), no wrong-arm / unlatched decision, no cross-clobber, no alt_break; the dumped presents unique; and the
    pixel grade's OWN table (<roll>_freeze.txt, freeze_diff's `compared` rows) joined to the arm lines - every compared
    pair from two deciders, every static pair's same-arm control from one, >= MEMRES_AB_MIN_PAIRS static pairs."""
    checks = [{"field": "committed", "op": "eq", "value": True},
              {"field": "runner.verdict", "op": "eq", "value": "PASS"},
              {"field": "verdict", "op": "eq", "value": "PASS"},
              {"field": "schema", "op": "eq", "value": "mem-res-ab/2"}]
    checks += [{"field": "checks.%s" % k, "op": "eq", "value": True} for k in (
        "terminal", "schema_complete", "counters", "dumps_unique", "compared_joined", "compared_alternate",
        "controls_same_arm", "pairs_min")]
    checks.append({"field": "static_pairs", "op": "ge", "value": MEMRES_AB_MIN_PAIRS})
    return ev(eid, "json", "live-observe", "generation", checks=checks, note=note)


def memres_census_item(eid, eras, note=""):
    """tools/perf/mem_res_census.py over one memres_bounds roll (boundaries_complete), its run-unique JSON (the step's
    verdict stamped, committed last): every named design era (by_era: boot / movies / menus / gameplay / warworld, or
    the step's --label era) PASS - no missed boundary (the registry's arena never differed from RAM in fetched bytes),
    no clobber / alias difference, every write-watch and range-cache keep re-checked (else GAP), the era's windows
    complete through the terminal snapshot (else NO DATA) - and decided something."""
    checks = [{"field": "committed", "op": "eq", "value": True},
              {"field": "runner.verdict", "op": "eq", "value": "PASS"}]
    for e in eras:
        checks += [{"field": "by_era.%s.verdict" % e, "op": "eq", "value": "PASS"},
                   {"field": "by_era.%s.decisions" % e, "op": "gt", "value": 0},
                   {"field": "by_era.%s.missed" % e, "op": "eq", "value": 0}]
    return ev(eid, "json", "live-observe", "generation", checks=checks, note=note)


def memres_texid_item(eid, eras, note=""):
    """tools/perf/mem_res_texid.py over one memres_readers roll (textures_from_resolves), its run-unique JSON: the run
    PASS and complete through the terminal window, and every named era PASS - resolve output bound (resolve_binds > 0)
    and none of it a mirror decoded from guest memory (guest_bytes 0), stale or of an unseen origin. The strict reading
    (identity = every resolve bind) is in by_era.<era>.strict_identity, reported, not gated (the module doc)."""
    checks = [{"field": "committed", "op": "eq", "value": True},
              {"field": "runner.verdict", "op": "eq", "value": "PASS"},
              {"field": "verdict", "op": "eq", "value": "PASS"},
              {"field": "partial", "op": "eq", "value": False}]
    for e in eras:
        checks += [{"field": "by_era.%s.verdict" % e, "op": "eq", "value": "PASS"},
                   {"field": "by_era.%s.guest_bytes" % e, "op": "eq", "value": 0}]
    return ev(eid, "json", "live-observe", "generation", checks=checks, note=note)


def memres_readers_item(eid, note=""):
    """tools/perf/mem_res_reader_census.py over one memres_readers roll (resolve_writeback_readers), its run-unique
    JSON: the site probe's WHOLE-RUN census (resproof 2026-10-17: every page of every range graded, the event cap sized
    for whole runs; its own line still says exhaustive_readers=0 - it watches TextureCopySurfaceRows' row copy and the
    texture-lock sites, not every guest load) complete - the graceful quit's terminal reader_summary printed with
    events_capped / capacity / counter_overflow / failures / invalid / stream_errors 0 and no capped notice (verdict
    CENSUS; overflow = INCOMPLETE = FAIL, never silent) - and over every era: no copy read of resolve output
    (copy_gpu_total 0), no texture lock, vertex-buffer lock or other CPU-note coherency over it and no lock-core lock
    (read-only ones included) over it (texlock_gpu_total, inval_gpu_total, other_gpu_total, lockcore_gpu_total 0) and no page the probe could not classify (gaps_total 0). by_era.<era>.verdict READERS / INTENT / GAP / NONE names what it saw per era."""
    checks = [{"field": "committed", "op": "eq", "value": True},
              {"field": "runner.verdict", "op": "eq", "value": "PASS"},
              {"field": "verdict", "op": "eq", "value": "CENSUS"},
              {"field": "complete", "op": "eq", "value": True},
              {"field": "summary_present", "op": "eq", "value": True},
              {"field": "events_capped", "op": "eq", "value": 0},
              {"field": "copy_gpu_total", "op": "eq", "value": 0},
              {"field": "texlock_gpu_total", "op": "eq", "value": 0},
              {"field": "inval_gpu_total", "op": "eq", "value": 0},
              {"field": "other_gpu_total", "op": "eq", "value": 0},
              {"field": "lockcore_gpu_total", "op": "eq", "value": 0},
              {"field": "gaps_total", "op": "eq", "value": 0}]
    return ev(eid, "json", "live-observe", "generation", checks=checks, note=note)


def memres_dual_item(eid, note=""):
    """The A/B's activation: the dual mode latched with the per-present arm (SR_MEM_RESOURCES=1, SR_AB_TOGGLE lists
    SR_MEM_RESOURCES, SR_AB_PRESENT=1)."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[MEM_RES_AB\] dual=1", which="last",
              pattern=r"dual=1 bit=\d+ present_mode=1", note=note)


def memres_route_item(eid, form, note=""):
    """mem.resources BAKED (lane C native/mem-native, 2026-10-17): the route this roll decided resources by, as the
    shared-memory init logs it once ([MEM_RES_NATIVE] route <native|page_model> form=<default|explicit|off|census>):
    every default arm the default form (SR_MEM_RESOURCES unset), every =0 arm the page model's off form."""
    route = "page_model" if form in ("off", "census") else "native"
    return ev(eid, "text", "live-observe", "generation", select=MEMRES_ROUTE_SELECT, which="last",
              pattern=r"route %s form=%s$" % (route, form), note=note)


def memres_patch_item(eid, note=""):
    """The exact CPU-write patches (kernel/native_resource_stamps.hpp: the D3D header fence stamps) on the graceful
    quit's terminal line: noted and taken (the request copied them), the route's own counters."""
    return ev(eid, "text", "live-observe", "generation", select=MEMRES_PATCH_SELECT, which="last",
              pattern=r"notes=[1-9]\d* dwords=[1-9]\d* refused=\d+ taken=[1-9]\d*(?: superseded=\d+)?$", note=note)


def memres_run_zero_item(eid, note=""):
    """The whole run - boot, intro, title, menus, loading, gameplay: every era - holds no shared-memory page validation:
    the terminal [NATIVE_METER_RUN] totals read shm=0 (the meter's per-ingame-frame item is the window's; this one is
    the run's)."""
    return ev(eid, "text", "live-observe", "meter", select=r"\[NATIVE_METER_RUN\] terminal", which="last",
              pattern=r"\| pm4=\d+ .* shm=0 xma=\d+ \|", note=note)


def memres_shm_meter_item(eid, base_tag, note=""):
    """The shm meter column (the page model's validations, ledger mem.resources) per ingame frame: the =0 arm shows the
    page model deciding (>= 1,000 a frame; p0_ww_default 25,667.8), the baked default none."""
    return ev(eid, "meter_delta", "live-observe", "meter", base=base_tag, field="shm", base_min=1000.0, max=0.0,
              world_min=0.5, note=note)


def ring0_item(eid, note=""):
    """boot.device_create: the CP parsed no ring packet at all (the ME_INIT was the one left under ringless)."""
    return ev(eid, "text", "live-observe", "guest-answers", select=r"\[(?:GPU_CP|NATIVE_SUBMIT)\] census", which="last",
              pattern=r" packets=0 ", note=note)


def devcreate_item(eid, note=""):
    """boot.device_create: the ports' final counters - the ring init, the interrupt threads and the state reset once
    each, no post-allocation decline (late 0)."""
    return ev(eid, "text", "live-observe", "guest-answers", select=r"\[NATIVE_DEVCREATE\]", which="last",
              pattern=r"ringInit=1 \(decl \d+, late 0\) irqThreads=1 \(decl \d+, late 0\) resetState=1 segmentBegin=[1-9]",
              note=note)


def trace_item(eid, note=""):
    """The P2.7 invariants from p2_trace.py --json (S odd and +2 per close, C monotone and <= S, COMPLETEs in order)."""
    return ev(eid, "json", "live-exact", "guest-answers", note=note, checks=[
        {"field": "close.even_S", "op": "eq", "value": 0}, {"field": "C.backwards", "op": "eq", "value": 0},
        {"field": "C.ahead_of_S", "op": "eq", "value": 0}, {"field": "complete.out_of_order", "op": "eq", "value": 0},
        {"field": "complete.not_a_closed_S", "op": "eq", "value": 0},
        # 2026-10-10 (orchestrator): with the 0x820FDD18 hook gone (release/forcing), BlockUntilIdle spins on dev+0x2A74
        # until the ISR / pump drain it - every entry completes, and none spins longer than a frame
        {"field": "idle.entries", "op": "gt", "value": 0},  # an exe without the idle trace kinds proves nothing here
        {"field": "idle.open_at_end", "op": "eq", "value": 0}, {"field": "idle.spin_over_frame", "op": "eq", "value": 0}])


def trace_order_item(eid, note=""):
    """trace_item plus the close order of the whole segment stream (p2_trace.py segment_order, 2026-10-13): the CP's
    first take of every SEGMENT item - deferred (pump-published) segments included - in registry-id (close) order,
    no id skipped, segments actually taken; and the terminal reconciliation: forced drained points traced, none a
    miss, the last within ~20 s of the run's end, every segment published before it first taken before it."""
    it = trace_item(eid, note)
    it["checks"] = it["checks"] + [
        {"field": "segment_order.segments", "op": "gt", "value": 0},
        {"field": "segment_order.late_first", "op": "eq", "value": 0},
        {"field": "segment_order.skipped", "op": "eq", "value": 0},
        {"field": "segment_order.forced_drains", "op": "gt", "value": 0},
        {"field": "segment_order.forced_miss", "op": "eq", "value": 0},
        {"field": "segment_order.drain_at_end", "op": "eq", "value": True},
        {"field": "segment_order.terminal_unapplied", "op": "eq", "value": 0}]
    return it


def bracket_item(eid, note=""):
    """p2.transport BRACKET (SR_NATIVE_BRACKET): the last [NATIVE_FLUSH] bracket line - brackets opened and ended
    natively, consumed by the CP with their bin selects and nested segments, and none chained behind a guest pump
    (chained only arises in an A/B transition, where it is information). Declines stay visible by reason."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] \w+ bracket begin=", which="last",
              pattern=r"begin=[1-9]\d* beginDecl=\d+ abOff=\d+ end=[1-9]\d* failed=\d+ literal=\d+ .*"
                      r"\| consumed=[1-9]\d* bs=[1-9]\d* segs=[1-9]\d*",
              forbid=[r"\[NATIVE_FLUSH_BAD\] bracket"], note=note or "Sampled activity; declines classified separately.")


def bracket_trace_item(eid, note=""):
    """trace_order_item with BRACKET's nested class (p2_trace.py segment_order, kind 24 aux 4): bracket segments
    applied inside BRACKET_END, in close order within their class, and the deferred-lookup stream not skipping them."""
    it = trace_order_item(eid, note)
    it["checks"] = it["checks"] + [
        {"field": "segment_order.bracket_segments", "op": "gt", "value": 0},
        {"field": "segment_order.bracket_late_first", "op": "eq", "value": 0},
        {"field": "idle.a74_nonzero_exit", "op": "eq", "value": 0}]
    return it


def residue_item(eid, strict=False, note=""):
    """p2.transport RESIDUE (SR_NATIVE_IDLE_WAIT / SR_NATIVE_INVAL_RANGE / SR_NATIVE_LOCK_COHER): the last [NATIVE_FLUSH]
    residue line - BlockUntilIdle's and the lock core's blocks taken as record spans, none refused late (InvalidateGpuCache
    may not run on the tape: its count is information). strict (a pure-ON roll): litPk=0 - no call of the three left its
    PM4 to the consumer (a decline with a packet counts there; in the A/B roll the SDK arm's calls count by design)."""
    lit = r"litPk=0" if strict else r"litPk=\d+"
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] \w+ residue idle=", which="last",
              pattern=r"idle=[1-9]\d* late=0 decl .* \| inval=\d+ late=0 merge=\d+ decl .* \| lock=[1-9]\d* late=0 "
                      r"merge=\d+ host=\d+ decl .* \| pk=[1-9]\d* " + lit,
              note=note or "Sampled counters (the periodic / exit line); declines stay visible by reason.")


def residue_trace_item(eid, note=""):
    """trace_order_item plus the BlockUntilIdle drain rule on the converted IDLE (p2_trace.py idle, kinds 26 / 27): every
    entry completes, none spins longer than a frame (trace_item), and none returns with dev+0x2A74 != 0."""
    it = trace_order_item(eid, note)
    it["checks"] = it["checks"] + [{"field": "idle.a74_nonzero_exit", "op": "eq", "value": 0}]
    return it


def producer_census_rx(producers, keys):
    """A [PM4_COVMAP] producer line (gfx/pm4_coverage: `inner=<VA>`, the innermost XDK scope that wrote the packets)
    of one of `producers` (entry VAs) holding one of the packet `keys` (r05 = the WAIT_UNTIL page, r0A = COHER_STATUS /
    SIZE / BASE, 3C = WAIT_REG_MEM), each key matched at a list boundary (r3C never reads as 3C)."""
    return r"\[PM4_COVMAP\] \w+ inner=(?:%s) name=\S+ pk=\d+ dw=\d+ keys=(?:\S*,)?(?:%s):\d+" % (
        "|".join(producers), "|".join(keys))


def producer_census_item(eid, producers, keys, present, note=""):
    """A converted producer's OWN packets by the PRODUCER census (SR_NATIVE_BRACKETS=1): each packet attributed to the
    innermost XDK scope that wrote it (Stamp::inner - a lock's COHER block is the lock's even under a draw's bracket),
    on the segmentless transport too (native/residue-census: a gap part the CP runs from the close's host copy is looked
    up by the guest address it was copied from, with the stamps the close moved into the segment; a stamp ends when
    either XDK allocator hands its memory out again, so a reused block never inherits an old owner).
    present=True: the base roll's census shows the producer writing those packets as PM4; False: the gated roll's shows
    none, and its census printed producer lines at all (an absent census never passes; PM4_COVMAP is a kMilestoneTags
    tag). PRE-REGISTERED 2026-10-14 (native/residue-census) from the res1 pair (exe 4292f86c1eec): census1's residue
    line has every PM4-writing decline at 0 (ab / seg / mem / mode / entry 0, late 0, litPk 0; owner merges and noPk
    write no PM4), so a gated roll holding any such line is a converted path still writing PM4."""
    rx = producer_census_rx(producers, keys)
    if present:
        return ev(eid, "text", "live-observe", "meter", select=rx, note=note)
    return ev(eid, "text", "live-observe", "meter", select=r"\[PM4_COVMAP\] \w+ inner=[0-9A-F]{8} ", forbid=[rx], note=note)


ANY_PRODUCER = "[0-9A-F]{8}"  # producer_census_item's `producers` for "any producer line" (UNSTAMPED included)


def producer_all_keys_item(eid, producer, keys, note="", name=r"\S+"):
    """boot.device_create (dcb; ported to the merged producer census, 2026-10-14): ONE [PM4_COVMAP] producer line
    (`inner=<producer> name=<name>`) holding EVERY key, each on its own (lookaheads, an AND - a regex OR let a base pass
    with one key missing, dcb round 1). Producer 00000000 name=UNSTAMPED holds what no stamp claims (RING_UNSTAMPED
    shares the VA). Since native/residue-census the census attributes segmentless gap parts, so at boot UNSTAMPED is
    what D3DDeviceCreate 820F34C0 writes: its bracket opens before the device exists (kernel/native_brackets.cpp
    openBracket: noDevice; cutPiece returns while t.dev = 0), and 820F0048 is no table scope, so its per-frame packets
    are its caller Swap's (inner=82112050)."""
    rx = r"\[PM4_COVMAP\] \w+ inner=%s name=%s pk=\d+ dw=\d+ keys=%s" % (
        producer, name, "".join(r"(?=(?:\S*,)?%s:\d)" % re.escape(k) for k in keys))
    return ev(eid, "text", "live-observe", "meter", select=rx, note=note)


def owner_compare_item(eid, note=""):
    """boot.device_create: tools/perf/owner_census.py's base / target comparison of the PRODUCER census (the owner
    step's JSON; every producer's last `inner=` line, summed - the whole census - and the UNSTAMPED line alone), OVER
    THE CENSUS-COVERED INTERVAL (Astra round 3: the census reports every 256 frames or 20 s with no final report - the
    harness kills the game - so each roll's counts end at its last report). The base exercises 820F0048
    (SET_BIN_SELECT_HI, opcode 63, > 0); the target holds no 63 on any line; the target's RB_BC_CONTROL page r0F equals
    the base's r0F minus the base's 63 (the residual) in the whole census, and the same on the UNSTAMPED line alone (the
    residual stays unstamped, at boot, where it was). The interval is bounded: each roll's last report lies within one
    reporting period of its run's end (tail < 256 frames, <= 21 s), its [PM4_COV] F records (one per completed frame,
    every producer together) are contiguous and valid from frame 1 through the last (Astra round 4: a gap anywhere is
    INCOMPLETE and fails) and reconcile with the census over the covered frames for EVERY comparable opcode / page key
    (rcen1: all 43; the census-only oN keys are reported as not comparable), and the target's tail F lines hold no
    opcode 63 and no RB_BC_CONTROL (unattributed; the residual is a boot write, frame 1). The partial frame at the kill
    is in no line. The JSON is the step's run-unique output and carries its verdict (runner.verdict, the first check):
    a failed comparison grades FAIL for good, and a later passing run moves it into contradiction r0F_second_producer
    as a witness (the plan's supersede_into).
    An AGGREGATE RESIDUAL CONSISTENCY CHECK only: the counts agree with 820F0048's r0F gone and the residual r0F
    (82103238's by the source, called from 821058B0) unchanged, conditional on that residual being equal in both
    runs. It does not name the producer of the target's r0F: 82103238's write lost while one devcreate r0F stays
    unstamped gives the same counts (Astra round 2). No producer witness, and a table row would not give one: the
    residual is written inside D3DDeviceCreate 820F34C0's bracket, which opens with no device (noDevice; t.dev is
    re-read only at a ring-write scope - the flush at 0x821061D4, after 821058B0 - and at the close), so every cut
    inside it before the flush returns unstamped: rcen1 census0 (exe 6705f9e06683, the merged census) has 820F34C0
    a table scope and still r0E:1 r0C:1 63:1 r0F:2 UNSTAMPED. Stamping it needs the bracket to take the device
    mid-call - a census mechanics change, not a table entry.
    The equal-residual condition, statically (IDA, read-only, 2026-10-14): 82103238 (GpuRingEmitModeStateReg: WAIT_UNTIL
    0x5C8 = 0x20000, RB_BC_CONTROL 0x0F01 = dev[2697]) has one code caller, D3DDeviceInitDefaultRenderStates 821058B0
    (0x821059D4); 821058B0's two, D3DDeviceCreate 82106010 (0x8210616C) and D3DDeviceInitAllocContexts 82106280
    (0x82106318), are reached only from D3DDeviceCreate 820F34C0 - flags 1 from RenderDeviceInit 82621258 (the title
    device), flags 2 from the recorder 82304C38, which has no code caller (dead: docs/NATIVE_ROADMAP.md C.1). Every
    other xref of the five is its .pdata entry. So no reset path re-runs it: once per title-device creation in either
    arm (821058B0 is not a devcreate producer), which fits dc5c's whole-run residual r0F - 63 = 1."""
    return ev(eid, "json", "live-observe", "meter", note=note, checks=[
        {"field": "committed", "op": "eq", "value": True},  # written LAST, after the step's final row (commit_output)
        {"field": "runner.verdict", "op": "eq", "value": "PASS"},  # the step's own verdict (do_owner stamps it)
        {"field": "checks.base_63_exercised", "op": "eq", "value": True},
        {"field": "checks.target_63_zero", "op": "eq", "value": True},
        {"field": "checks.target_r0F_equals_other", "op": "eq", "value": True},
        {"field": "checks.target_unstamped_r0F_equals_other", "op": "eq", "value": True},
        {"field": "checks.base_census_covers_run", "op": "eq", "value": True},
        {"field": "checks.target_census_covers_run", "op": "eq", "value": True},
        {"field": "checks.frames_match_census", "op": "eq", "value": True},
        {"field": "checks.frames_complete", "op": "eq", "value": True},
        {"field": "checks.target_tail_clean", "op": "eq", "value": True}])


def frames_forbid_item(eid, ops=(), regs=(), note=""):
    """boot.device_create (dcb, Astra round 3): no completed frame's [PM4_COV] F line (gfx/pm4_coverage frameEnd: one
    per frame, every producer together - unattributed) holds any of the type-3 `ops` or the type-0 pages `regs` - the
    census's own count (frames 1..N sum to its totals: owner_census frames_match_census) over EVERY completed frame,
    the tail after the last periodic census report included (the partial frame at the kill is in no line). Only for
    keys no other producer writes in the census (63: 820F0048; r0E / r0C: 8210E520). select: F lines exist (an absent
    stream never passes). A text item cannot see a MISSING frame record: that the records are contiguous and valid
    through the last frame is census_cmp's frames_complete (owner_census), on the same roll."""
    forbid = []
    if ops:
        forbid.append(r"\[PM4_COV\] F frame=\d+ .* ops=(?:\S*,)?(?:%s):" % "|".join(ops))
    if regs:
        forbid.append(r"\[PM4_COV\] F frame=\d+ .* regs=(?:\S*,)?(?:%s):" % "|".join(regs))
    return ev(eid, "text", "live-observe", "meter", select=r"\[PM4_COV\] F frame=\d+ ", forbid=forbid, note=note)


# The converted producers (kernel/native_flush.cpp RESIDUE): BlockUntilIdle's WAIT_UNTIL, GpuInvalidateMemoryRange's
# WAIT_UNTIL + owner COHER block, the lock core 820F3988's owner COHER block through VertexBuffer_Lock and through the
# texture / surface / volume lock entries (820FF468 under 820FF530 / 820FF5C0; IndexBuffer_Lock always passes no-packet).
RESIDUE_PRODUCERS = (
    ("idle", ("820FDCB8",), ("r05",), "BlockUntilIdle 820FDCB8's WAIT_UNTIL"),
    ("inval", ("820F4280",), ("r05", "r0A", "3C"), "GpuInvalidateMemoryRange 820F4280's WAIT_UNTIL + owner COHER block"),
    ("vblock", ("820F3FA0",), ("r0A", "3C"), "VertexBuffer_Lock 820F3FA0's owner COHER block"),
    ("texlock", ("820FFCB0", "820FFDA0", "820FFF00", "82100228", "821002D8"), ("r0A", "3C"),
     "the texture / surface / volume locks' owner COHER block (820FFCB0, 820FFDA0, 820FFF00, 82100228, 821002D8)"),
)


def residue_census_items(roll, present):
    return [producer_census_item("residue_%s_%s" % (roll, name), vas, keys, present,
                                 ("as guest PM4 on the default transport: " if present else "none left as PM4 when gated: ")
                                 + what)
            for name, vas, keys, what in RESIDUE_PRODUCERS]


BOOT_EQUIV_DESIGN = "dcl-v2"  # tools/perf/boot_equiv.py DESIGN_ID (test_native_proof checks they agree)
BOOT_EQUIV_PAIRS = 8          # tools/perf/boot_equiv.py N_PAIRS
BOOT_STALL_DESIGN = "dcl-stall-v1"  # tools/perf/boot_stall.py DESIGN_ID
BOOT_STALL_N_MIN = 29               # tools/perf/boot_stall.py N_MIN
BOOT_STALL_EXTRA = 17               # the plan's stall block: 8 boot + 4 meter + 17 ON boots = 29
DEVCREATE_ENGAGED = r"\[NATIVE_DEVCREATE\] ring init native"
# The bake's designs (native/devcreate-fix, 2026-10-15; NATIVE_ROADMAP "boot.device_create BAKE PLAN"): the gate default
# ON, so ON = the default (the key unset) and OFF = SR_NATIVE_DEVCREATE=0; four pairs (boot_equiv.py DESIGNS: the
# reasoning), the stall criterion over EVERY ON boot of the proof (boot_stall.py DESIGNS), the plan's observe-only
# instruments declared to it (its INSTRUMENT_KEYS). test_native_proof checks the tools agree.
BOOT_EQUIV_BAKE_DESIGN = "dcf-v1"
BOOT_EQUIV_BAKE_PAIRS = 4
# the stall design after Astra's round 1 on the branch (2026-10-15): dcf-stall-v2 - every ON boot of the proof, the gate's
# two boots included, >= 29 (0.9^29 = 4.7%: a 30 s stall on 10% of boots passes with < 5%; v1's 7 boots let it through
# 47.8% of the time), the WP1 (C) and late-HUD (H) events, the staged cache condition. The plan's ON boots: the warm-up,
# the four pairs' ON, cppm4, census1, the stall block s1_on..s20_on (BOOT_STALL_BAKE_EXTRA), gate/freeze, gate/span = 29.
BOOT_STALL_BAKE_DESIGN = "dcf-stall-v2"
BOOT_STALL_BAKE_N_MIN = 29
BOOT_STALL_BAKE_EXTRA = 20          # the bake's stall block: 7 + 20 + the gate's 2 = 29
BOOT_STALL_INSTRUMENTS = ("SR_NATIVE_BRACKETS", "SR_NATIVE_FLUSH_CPPM4", "SR_NATIVE_CONST_DEDUP_CHECK",
                          "SR_DIAG_SIM_FREEZE", "SR_DRIVE_GRACEFUL_QUIT")
GATE_BOOTS = ("gate/freeze", "gate/span")   # the flicker gate's two boots as stall-pool tags (Runner.boot_path)
DEVCREATE_OFF = "SR_NATIVE_DEVCREATE=0"   # the bake's =0 arm (the guest bodies; the A/B arm)


def boot_equiv_item(eid, note="", design=BOOT_EQUIV_DESIGN, pairs=BOOT_EQUIV_PAIRS):
    """boot.device_create's boot_order_gap (native/devcreate-close, 2026-10-14): tools/perf/boot_equiv.py's run-unique
    JSON over the plan's warm-up and boot pairs - the pre-registered powered paired design dcl-v2 (docs/NATIVE_ROADMAP.md
    CLOSE DESIGN; dcl-v1 revised after Astra round 1), or the bake's dcf-v1 (4 pairs, the gate default ON). It passes
    only on EQUIVALENT with the tag BOOT_EQUIV, this design and its pairs, the step's own verdict stamped
    (runner.verdict) and the commit marker written last; OPEN and INCONCLUSIVE grade FAIL for good. The plan routes it
    to the unknown's resolver boot_order.powered (resolver_bind), not into the evidence. It covers the MEAN regression
    only; the stall criterion is boot_stall_item's."""
    return ev(eid, "json", "live-observe", "timing", note=note, checks=[
        {"field": "committed", "op": "eq", "value": True},
        {"field": "runner.verdict", "op": "eq", "value": "PASS"},
        {"field": "verdict", "op": "eq", "value": "EQUIVALENT"},
        {"field": "tag", "op": "eq", "value": "BOOT_EQUIV"},
        {"field": "design", "op": "eq", "value": design},
        {"field": "pairs", "op": "eq", "value": pairs}])


def boot_stall_item(eid, note="", design=BOOT_STALL_DESIGN, n_min=BOOT_STALL_N_MIN):
    """boot_order_gap's INTERMITTENT-STALL criterion (Astra round 1, P1-1): tools/perf/boot_stall.py's run-unique JSON
    over every ON boot of the run (design dcl-stall-v1: no stall event - F / A / W / R - in >= 29 SR_NATIVE_DEVCREATE=1
    boots; the bake's dcf-stall-v2: every ON boot of the proof, the gate's two and any retained attempt included, >= 29,
    the events C / H added, staged; every boot counted, crashes included). Passes only on BOUNDED with the tag
    BOOT_STALL; routed to the unknown's second resolver boot_order.stall (resolver_bind)."""
    return ev(eid, "json", "live-observe", "timing", note=note, checks=[
        {"field": "committed", "op": "eq", "value": True},
        {"field": "runner.verdict", "op": "eq", "value": "PASS"},
        {"field": "verdict", "op": "eq", "value": "BOUNDED"},
        {"field": "tag", "op": "eq", "value": "BOOT_STALL"},
        {"field": "design", "op": "eq", "value": design},
        {"field": "events", "op": "eq", "value": 0},
        {"field": "on_boots", "op": "ge", "value": n_min}])


def pooled_meter_item(eid, max_delta, min_roll_frames, note="", provenance=None, target_max=None, base_min=None):
    """The total pm4 per ingame frame as the ledger's POOL of several pairs on one exe (meter_delta with `pool`, the form
    of p2.segmentless's bake_meter), every roll - none dropped - held to the world rule and to min_roll_frames. Astra
    round 1 (P2-5): "pool_weight": "pair" - the statistic is the MEAN OF THE PER-PAIR DELTAS, the one the design's power
    was computed for (the frame-weighted arm means turned zero-delta pairs with opposite frame counts into -33.33);
    `provenance` - one exe, each arm's gate in provenance and engaged in the log, the declared alternation; `receipt` -
    the step's run-unique, committed JSON, its verdict and each roll's file hashes (the rolls cited are the rolls run).
    target_max / base_min (the bake's zero form, native/devcreate-fix): every target roll holds at most target_max of
    the field and every base roll shows at least base_min of it. The step fills path / base (pair 1), pool (the others)
    and receipt."""
    it = ev(eid, "meter_delta", "live-observe", "meter", base=None, field="pm4", max_delta=max_delta, world_min=0.5,
            min_roll_frames=min_roll_frames, pool_weight="pair", note=note)
    if provenance:
        it["provenance"] = provenance
    if target_max is not None:
        it["max"] = target_max
    if base_min is not None:
        it["base_min"] = base_min
    return it


def me_init_item(eid, note=""):
    """boot.device_create's OFF arm (SR_NATIVE_DEVCREATE=0 on the baked exe): the guest's ME_INIT is the one ring packet
    the CP parses all run - the last [GPU_CP] census reads packets=1 (tbk1's default arm with the gate off: packets=1 in
    every roll). The bake's =0 witness, the counterpart of ring0_item."""
    return ev(eid, "text", "live-observe", "guest-answers", select=r"\[(?:GPU_CP|NATIVE_SUBMIT)\] census", which="last",
              pattern=r" packets=1 ", note=note)


def alternating_pairs(n, on_gates, off_gates, prefix, on_first=True, mode="plain"):
    """(roll steps, pairs) for n alternating ON / OFF pairs in run order: pair 1 ON/OFF when on_first, then the other
    order, and so on (ABBA blocks: each arm first in half the pairs; a linear drift cancels in every block). Tags
    <prefix><k>_on / _off; pairs = [[(arm, tag), (arm, tag)], ...]."""
    steps, pairs = [], []
    for k in range(n):
        arms = ("on", "off") if (k % 2 == 0) == on_first else ("off", "on")
        pr = []
        for arm in arms:
            tag = "%s%d_%s" % (prefix, k + 1, arm)
            steps.append(Step("roll", tag, on_gates if arm == "on" else off_gates, mode))
            pr.append((arm, tag))
        pairs.append(pr)
    return steps, pairs

# p1.flush_residue (SR_NATIVE_FLUSH_SPLIT; NATIVE_ROADMAP "p1.flush_residue BUILD NOTE"): the S1 draw flushes' own residue
# - every key of their producer lines but the shader-constant-run keys p1.row3_construns owns (r20 / r23 / r0A / 3C: the
# split native/transport-composed's gap map applies, archive/tcmp/unbuilt_keys.py) - and row 9's entries, whose PM4 is
# all from declines (the rcen1 pair: kick declines only). The same producer sets as the ledger record's observation and
# native/transport-composed's transport_census.py flush_residue group (D3DDeviceClear 82101998 included: it only calls
# ClearF 82101A58 per rect (IDA), whose row-9 hook and scope take the packets - absent from rcen1, expected absent).
FLUSH_PRODUCERS = ("820FBBF8", "820FC000", "820FB6E8")
FLUSH_SPLIT_KEYS = ("r20", "r23", "r0A", "3C")
# 821075E0's alignment pad (native/flush-residue-2, Astra round 1): the census's WITNESSED key o6 - a type-2 NOP whose
# next dword is the leaf's run header (gfx/pm4_coverage.hpp kConstRunPadKey; IDA 0x82107708 / 0x82107720) - is
# construns'; every other NOP (o1) on an S1 line is residue, in every cumulative report from boot (transport_census.py
# KEY_SPLIT and its from-boot check). An exe without the witness prints only o1: its pads read as residue.
FLUSH_PAD_KEY = "o6"
ROW9_PRODUCERS = ("8210C5F8", "820F5570", "82101A58", "82101998")


def flush_residue_rx():
    """A [PM4_COVMAP] producer line of the S1 flushes holding any key outside the construns split (the lookahead runs at
    every key boundary of the list; 821075E0's witnessed pad o6 inside the split)."""
    return (r"\[PM4_COVMAP\] \w+ inner=(?:%s) name=\S+ pk=\d+ dw=\d+ keys=(?:\S*,)?(?!(?:%s|%s):)\w+:\d+"
            % ("|".join(FLUSH_PRODUCERS), "|".join(FLUSH_SPLIT_KEYS), FLUSH_PAD_KEY))


def row9_residue_rx():
    return r"\[PM4_COVMAP\] \w+ inner=(?:%s) name=\S+ pk=[1-9]\d* " % "|".join(ROW9_PRODUCERS)


def flush_residue_census_items(prefix, roll, present):
    """The producer census pair of p1.flush_residue (SR_NATIVE_BRACKETS=1): present=True - the base roll (gate OFF) shows
    the S1 flushes' residue keys and row 9's declined entries as PM4 (the census is not vacuous); False - the gated roll
    holds none of either, and printed producer lines at all. PRE-REGISTERED 2026-10-14 (native/flush-residue) from the
    rcen1 pair (native/transport-composed's GAP MAP): census1 238.95 a present inside the three flushes after the split,
    row 9 3.69. native/flush-residue-2 (Astra round 1): 821075E0's alignment pad is construns' only as the census's
    witnessed key o6 (FLUSH_PAD_KEY); an o1 in any line - cumulative from boot, the opening included - is residue."""
    out = []
    for name, rx, what in (("s1", flush_residue_rx(), "the S1 draw flushes' non-construns keys (section fallbacks, "
                                                     "draws over 0xFFFF indices)"),
                           ("row9", row9_residue_rx(), "row 9's entries (Resolve, ClearF, SetShaderGPRAllocation: "
                                                       "their kick declines; D3DDeviceClear 82101998)")):
        note = ("as guest PM4 with the gate OFF: " if present else "none left as PM4 under SR_NATIVE_FLUSH_SPLIT=1: ") + what
        if present:
            out.append(ev("%s_%s_%s" % (prefix, roll, name), "text", "live-observe", "meter", select=rx, note=note))
        else:
            out.append(ev("%s_%s_%s" % (prefix, roll, name), "text", "live-observe", "meter",
                          select=r"\[PM4_COVMAP\] \w+ inner=[0-9A-F]{8} ", forbid=[rx], note=note))
    return out


def flush_census_item(eid, note=""):
    """p1.flush_residue's OBSERVATION from tools/perf/transport_census.py --flush-residue-zero over the census pair
    (census0 the gate OFF, census1 ON; both SR_NATIVE_BRACKETS=1; the merge with dev ce46c966, whose tool the plan used
    by hand before): both censuses complete and reconciled (F records contiguous, the last report within one census
    reporting period of the run's end, per key F lines = CUM delta = the producer lines), every producer mapped, the
    base showing the flush_residue group, the target's flush_residue group 0 packets over its census-covered INGAME
    interval (the S1 flushes' scope less 821075E0's construns keys and their alignment pad, row 9's declines).
    Unattributed packets are reported, not graded (the pump's RING_UNSTAMPED bin-select dwords remain without BRACKET).
    The JSON is the step's run-unique output: committed last, the step's own verdict stamped in it."""
    return ev(eid, "json", "live-observe", "census", note=note, checks=[
        {"field": "committed", "op": "eq", "value": True},  # written LAST, after the step's final row (commit_output)
        {"field": "runner.verdict", "op": "eq", "value": "PASS"},  # the step's own verdict (do_tcensus stamps it)
        {"field": "verdict", "op": "eq", "value": "PASS"},
        {"field": "checks.base_flush_residue_exercised", "op": "eq", "value": True},
        {"field": "checks.target_flush_residue_zero", "op": "eq", "value": True},
        # boot .. the last report (Astra round 1 on native/flush-residue-2): no opening residue either
        {"field": "checks.target_flush_residue_zero_from_boot", "op": "eq", "value": True},
        {"field": "target.flush_residue_from_boot.pk", "op": "eq", "value": 0},
        {"field": "checks.base_reconciled", "op": "eq", "value": True},
        {"field": "checks.target_reconciled", "op": "eq", "value": True},
        {"field": "checks.base_frames_complete", "op": "eq", "value": True},
        {"field": "checks.target_frames_complete", "op": "eq", "value": True},
        {"field": "checks.base_covers_run", "op": "eq", "value": True},
        {"field": "checks.target_covers_run", "op": "eq", "value": True},
        {"field": "checks.base_mapped", "op": "eq", "value": True},
        {"field": "checks.target_mapped", "op": "eq", "value": True},
        {"field": "target.flush_residue.pk", "op": "eq", "value": 0}])  # always written (a zero group is absent)


def flush_split_item(eid, note=""):
    """SR_NATIVE_FLUSH_SPLIT's own census line (the last [NATIVE_FLUSH] split line): constant sections chunked across the
    segment limit and draws over 0xFFFF indices split natively, both non-zero (the gameplay rates of rcen1: 21.2 constant
    section aborts and 1.07 big draws a present). decl= (a prim whose XDK table entry is 0) is information."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] \w+ split sections const=", which="last",
              pattern=r"split sections const=[1-9]\d* reg=\d+ fetch=\d+ chunks const=[1-9]\d* reg=\d+ fetch=\d+ "
                      r"kicks const=\d+ reg=\d+ fetch=\d+ slowTrailer=\d+ \| big draws=[1-9]\d* parts=[1-9]\d* kicks=\d+ "
                      r"decl=\d+", note=note or "Sampled counters (periodic / exit line).")


def flush_snapshot_item(eid, activity=True, note=""):
    """The strict bar of a pure-ON roll (the gate on from boot; Astra rounds 1-3 on native/flush-residue) from BOOT
    THROUGH THE LAST SNAPSHOT: the roll's [NATIVE_FLUSH] split snapshot lines (kernel/native_flush.cpp splitSnapshot:
    the first draw >= 0.5 s after the previous line, the clock read on every draw; counters cumulative from boot) as
    tools/perf/flush_split_snapshot.py reads them. The line is not terminal - the rolls end by Stop-Process and the
    harness has no graceful quit (acceptance.zero's own infra item). Freshness is relative to the LAST LOGGED TIMESTAMP,
    not to termination: the last snapshot within fresh_bound_s = period + 0.5 s = 1.0 s of the log's last timestamped
    line (fresh; the bound itself checked here, pinned by the grader's self-test); the remaining execution tail is
    unmeasured and NOTHING here claims the whole run. Graded: the counters never falling (monotone), no snapshot ever
    showing a residue, and in the last one: no count>0xFFFF decline the gate can convert (convertible = all less the
    XDK-table-0 prims and BeginVertices, which are counted, not hidden), no section fallback / abort, and for Resolve /
    ClearF / GPR every call taken (calls == taken: open 0, pre 0) with no live stop and none after a transparent kick.
    activity: the gate's paths ran in the roll (constant sections chunked, draws split, every row-9 entry called)."""
    checks = [{"field": "committed", "op": "eq", "value": True},  # the run-unique output, committed last (merge ce46c966)
              {"field": "runner.verdict", "op": "eq", "value": "PASS"},  # the step's own verdict (do_snapshot stamps it)
              {"field": "fresh", "op": "eq", "value": True}, {"field": "fresh_bound_s", "op": "le", "value": 1.0},
              {"field": "monotone", "op": "eq", "value": True},
              {"field": "snapshots", "op": "ge", "value": 3}, {"field": "ever_nonzero_n", "op": "eq", "value": 0},
              {"field": "last.convertible_declined", "op": "eq", "value": 0},
              {"field": "last.fallbacks", "op": "eq", "value": 0}, {"field": "last.aborts", "op": "eq", "value": 0}]
    for e in ("resolve", "clearF", "gpr"):
        checks += [{"field": "last.%s.calls_eq_taken" % e, "op": "eq", "value": True},
                   {"field": "last.%s.open" % e, "op": "eq", "value": 0},
                   {"field": "last.%s.pre" % e, "op": "eq", "value": 0},
                   {"field": "last.%s.live" % e, "op": "eq", "value": 0},
                   {"field": "last.%s.kickTStop" % e, "op": "eq", "value": 0},
                   {"field": "last.%s.calls" % e, "op": "gt", "value": 0}]
    if activity:
        checks += [{"field": "last.sec_const", "op": "gt", "value": 0}, {"field": "last.big_draws", "op": "gt", "value": 0}]
    return ev(eid, "json", "live-observe", "generation", checks=checks,
              note=note or "Boot through the last snapshot; freshness relative to the last logged timestamp allows "
                           "1.0 s (fresh_bound_s = the 0.5 s period + 0.5 s); the remaining execution tail is unmeasured "
                           "(no graceful quit).")


def flush_row9_item(eid, entry, strict, note=""):
    """Row 9 under the gate, a sampled witness beside the snapshot (the last [NB_REC] rec <entry> line, every 16384
    calls): entries taken through a transparent kick (kickT > 0), no live stop at all and none after such a pre-pass
    (kickTStop 0: the transparency argument's witness). strict (a pure-ON roll): no decline either (open=0 and pre=0)
    and every call taken (calls == taken, a backreference)."""
    if strict:
        pat = (r"calls=(\d+) taken=\1 items=\d+ bytes=\d+ \| declined open=0 pre=0:\s*\| live-stop=0 \|.* \| split "
               r"kickT=[1-9]\d* chunks=\d+ kickTStop=0")
    else:
        pat = r"declined open=\d+ pre=\d+:.*\| live-stop=0 \|.* \| split kickT=[1-9]\d* chunks=\d+ kickTStop=0"
    return ev(eid, "text", "live-observe", "generation", select=r"\[NB_REC\] rec %s: calls=" % entry, which="last",
              pattern=pat, note=note)


def flush_rec_check_items(prefix):
    """Row 9's dry CHECK (SR_NB_REC_CHECK=1: record mode off, every call the guest body with the port run dry beside it
    and compared; kick sites still stop the comparison): per entry the last [NB_REC] check line - completed comparisons
    (compared > 0, all of them ok: a backreference) with no length / byte / device / memory mismatch - and no
    [NB_REC_BAD] example line anywhere in the roll (every mismatch kind prints one, the first 48 of the run)."""
    return [ev("%s_rec_chk_%s" % (prefix, e), "text", "generation", "generation",
               select=r"\[NB_REC\] check %s: calls=" % e, which="last",
               pattern=r"compared=([1-9]\d*) ok=\1 bad: len=0 bytes=0 device=0 mem=0 ",
               forbid=[r"\[NB_REC_BAD\]"], note="row 9's dry CHECK under SR_NATIVE_FLUSH_SPLIT: " + e)
            for e in ("resolve", "clearF", "gpr")]


def flush_row9_items(prefix, roll, strict):
    return [flush_row9_item("%s_%s_%s" % (prefix, roll, e), e, strict) for e in ("resolve", "clearF", "gpr")]


def flush_consumer_items(prefix, roll):
    """The consumer side of a CPPM4 roll on the split (Astra round 1 on native/flush-residue-2): the CP ran the S1
    records (rendered back to PM4 through execute()) - the last periodic draws line's CP recs > 0 with none bad or torn -
    and applied row 9's records (the last [NB_REC] rec line's cp counters, shared by the three entries) with no bad apply
    and no render-length mismatch. frs1's cppm4 roll printed both (recs 48,213,134, row 9 applied 4,716,757)."""
    return [ev("%s_%s_cp" % (prefix, roll), "text", "live-exact", "generation", select=r"\[NATIVE_FLUSH\] periodic draws",
               which="last", pattern=r"\| CP recs=[1-9]\d* runs=\d+ draws=\d+ bad=0 torn=0 ",
               note="the CP ran the S1 records under the CPPM4 split: none bad or torn"),
            ev("%s_%s_rec_cp" % (prefix, roll), "text", "live-exact", "generation",
               select=r"\[NB_REC\] rec resolve: calls=", which="last",
               pattern=r"\| cp applied=[1-9]\d* bad=0 ops=\d+ pred-skipped=\d+ no-effect=\d+ render-len-bad=0 \|",
               note="row 9's records applied by the CP under the CPPM4 split: no bad apply, no render-length mismatch")]


# ---- p2.transport COMPOSED (transport-composed lane, 2026-10-14; the orchestrator's decisions on the gap map: the
# transport-producer zero, every transport kind + SR_NATIVE_DEVCREATE=1, SR_NATIVE_SHADER_CONST_RUNS off). PRE-REGISTERED
# before any composed roll: in a pure-ON roll every producer call is native - each kind's declines read 0 - so no
# transport producer leaves PM4 (the census grades the packets themselves, these lines the calls).

def composed_gate_item(eid, pm4=False, note=""):
    """The vblank gate's line: calls taken and none declined (a declined gate runs the SDK body: its PM4 apart from the
    callback block CALLBACK converts)."""
    applied = r"\d+" if pm4 else r"[1-9]\d*"
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] \w+ presentGate calls=", which="last",
              pattern=r"calls=[1-9]\d* skipped=\d+ decl=0 applied=" + applied, note=note or "every gate call native")


def composed_swap_items(prefix, pm4=False):
    """SWAP's lines: the SDK body (swapSdk) with at most the boot decline SWAP alone shows too, that decline a refused
    device / global MODE (swapSdkWhy: no memory reason; tcmp1's 26 / 10 gameplay declines were memory, fixed in
    native/transport-fix, tfx1 / tfx3 decl=1), and VdSwap's reserved block (swapBlock) with no decline."""
    applied = r"\d+" if pm4 else r"[1-9]\d*"
    return [swap_boot_only_item(prefix + "_sdk", "every Swap body native but at most the boot decline"),
            swap_why_item(prefix + "_why"),
            ev(prefix + "_block", "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] \w+ swapBlock calls=",
               which="last", pattern=r"calls=[1-9]\d* decl=0 applied=" + applied, note="every VdSwap block native")]


def composed_bracket_item(eid, note=""):
    """BRACKET pure ON: brackets opened and ended natively, no decline of any reason, no literal SDK kick, none chained
    behind a pump (no SDK kick ever leaves pump work: dev+0x2A74 stays drained), consumed with their bin selects."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] \w+ bracket begin=", which="last",
              pattern=r"begin=[1-9]\d* beginDecl=0 abOff=0 end=[1-9]\d* failed=\d+ literal=0 decl noGen=0 poisoned=0 "
                      r"gate=0 mode=0 memory=0 walk=0 bind=0 published=0 pending=0 \| stale=\d+ poison=0 retired=\d+ "
                      r"chained=0 \| consumed=[1-9]\d* bs=[1-9]\d* segs=[1-9]\d*",
              forbid=[r"\[NATIVE_FLUSH_BAD\] bracket"], note=note or "every bracket native")


def composed_pair_item(eid, note=""):
    """The pair under fully native BRACKET (pre-registered in the transport pair diagnosis): appends and the direct route
    taken, no decline, every call native (abOff 0); chain and park are information - unreachable by design when no SDK
    kick leaves pump work."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_TRANSPORT\] \w+ append=", which="last",
              pattern=r"append=[1-9]\d* chain=\d+ direct=[1-9]\d* park=\d+ resume=\d+ decline=0 .*"
                      r"\| abOffAppend=0 abOffEnd=0", note=note or "every pair call native; chain / park information")


def composed_ab_pair_item(eid):
    """The pair in the composed per-present A/B: both arms counted (abOffAppend / abOffEnd > 0), no native decline."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_TRANSPORT\] \w+ append=", which="last",
              pattern=r"append=[1-9]\d* chain=\d+ direct=[1-9]\d* park=\d+ resume=\d+ decline=0 .*"
                      r"\| abOffAppend=[1-9]\d* abOffEnd=[1-9]\d*", note="both arms of the composed A/B observed")


def composed_spans_item(eid):
    """No literal PM4 span from any native entry (the periodic draws line's `spans ... pm4=0`): what the plan's 'kind-10
    records 0' meant - kind 10 (kRkPm4) is reserved with no producer; the literal carrier is the span."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] periodic draws", which="last",
              pattern=r"\| spans rec=[1-9]\d* pm4=0 ", note="no native entry wrote literal PM4")


def composed_census_item(eid, note=""):
    """THE TRANSPORT-PRODUCER ZERO (p2.transport's observation since 2026-10-14) from tools/perf/transport_census.py
    --transport-zero over the census pair (census0 every kind off, census1 the composed env; both SR_NATIVE_BRACKETS=1):
    both censuses complete and reconciled (F records contiguous, the last report within one period, per key F lines =
    CUM delta = the producer lines), the base showing the transport group, the target's transport group 0 packets over
    its census-covered interval, nothing unattributed or unmapped, no transport signature key on another producer.
    The JSON is the step's run-unique output: committed last, the step's own verdict stamped in it."""
    return ev(eid, "json", "live-observe", "census", note=note, checks=[
        {"field": "committed", "op": "eq", "value": True},  # written LAST, after the step's final row (commit_output)
        {"field": "runner.verdict", "op": "eq", "value": "PASS"},  # the step's own verdict (do_tcensus stamps it)
        {"field": "verdict", "op": "eq", "value": "PASS"},
        {"field": "checks.base_transport_exercised", "op": "eq", "value": True},
        {"field": "checks.target_transport_zero", "op": "eq", "value": True},
        {"field": "checks.target_unattributed_zero", "op": "eq", "value": True},
        {"field": "checks.target_unmapped_zero", "op": "eq", "value": True},
        {"field": "checks.target_no_foreign_transport_keys", "op": "eq", "value": True},
        {"field": "checks.base_reconciled", "op": "eq", "value": True},
        {"field": "checks.target_reconciled", "op": "eq", "value": True},
        {"field": "checks.base_frames_complete", "op": "eq", "value": True},
        {"field": "checks.target_frames_complete", "op": "eq", "value": True},
        {"field": "checks.base_covers_run", "op": "eq", "value": True},
        {"field": "checks.target_covers_run", "op": "eq", "value": True},
        {"field": "checks.base_mapped", "op": "eq", "value": True},
        {"field": "checks.target_mapped", "op": "eq", "value": True},
        # native/composed-census: both censuses counted every present the CP applied (transport_census.py's COVERAGE
        # WITNESS: F lines tracking every [GPU_CP] present count) - a zero read from a dark counter is no data
        {"field": "checks.base_coverage_live", "op": "eq", "value": True},
        {"field": "checks.target_coverage_live", "op": "eq", "value": True},
        {"field": "target.coverage.state", "op": "eq", "value": "live"},
        # target.transport.pk is always written (group_pk holds only the groups that occur: a zero group is absent)
        {"field": "target.transport.pk", "op": "eq", "value": 0}])


def pm4_zero_census_item(eid, note=""):
    """THE DEFAULT PATH'S PM4 ZERO (the transport bake, native/transport-bake 2026-10-15) from tools/perf/transport_census.py
    --pm4-zero --allow-devcreate-reemit over the census pair (census0 every baked kind =0, census1 the default; both
    SR_NATIVE_BRACKETS=1): both censuses live, complete, reconciled, covering the run and mapped; the base showing the
    transport group; the target holding NO packet in gameplay - every F line from the ingame marker to the run's end and
    every producer line over the census interval - but 820F0048's per-present re-emit under Swap (60 / 61 / 62 / 63,
    r05, r0F on 82112050's line: SR_NATIVE_DEVCREATE is not baked), as itself only - none or one WHOLE re-emit a frame,
    attributed to 82112050 in every report segment of the gameplay span (Astra round 1: an allowed packet no producer
    line attributes is INCONCLUSIVE, which this item grades as a FAIL). The JSON is the step's run-unique output:
    committed last, the step's own verdict stamped in it."""
    return ev(eid, "json", "live-observe", "census", note=note, checks=[
        {"field": "committed", "op": "eq", "value": True},
        {"field": "runner.verdict", "op": "eq", "value": "PASS"},
        {"field": "verdict", "op": "eq", "value": "PASS"},
        {"field": "checks.base_transport_exercised", "op": "eq", "value": True},
        {"field": "checks.target_ingame_frames", "op": "eq", "value": True},
        {"field": "checks.target_pm4_zero_from_ingame", "op": "eq", "value": True},
        {"field": "checks.target_pm4_zero_interval", "op": "eq", "value": True},
        {"field": "checks.base_reconciled", "op": "eq", "value": True},
        {"field": "checks.target_reconciled", "op": "eq", "value": True},
        {"field": "checks.base_frames_complete", "op": "eq", "value": True},
        {"field": "checks.target_frames_complete", "op": "eq", "value": True},
        {"field": "checks.base_covers_run", "op": "eq", "value": True},
        {"field": "checks.target_covers_run", "op": "eq", "value": True},
        {"field": "checks.base_mapped", "op": "eq", "value": True},
        {"field": "checks.target_mapped", "op": "eq", "value": True},
        {"field": "checks.base_coverage_live", "op": "eq", "value": True},
        {"field": "checks.target_coverage_live", "op": "eq", "value": True},
        {"field": "target.coverage.state", "op": "eq", "value": "live"},
        # the block is always written: the counts themselves, not only the derived checks
        {"field": "target.pm4_zero.ingame_other_pk", "op": "eq", "value": 0},
        {"field": "target.pm4_zero.ingame_frames_over", "op": "eq", "value": 0},
        {"field": "target.pm4_zero.interval_other_pk", "op": "eq", "value": 0},
        # Astra round 1 (P2): the allowance as 820F0048's whole re-emit only, attributed to 82112050 over the whole
        # gameplay span; an allowed packet no producer line attributes is INCONCLUSIVE (the verdict above), never a PASS
        {"field": "checks.target_reemit_whole", "op": "eq", "value": True},
        {"field": "checks.target_reemit_attributed", "op": "eq", "value": True},
        {"field": "target.pm4_zero.ingame_frames_partial", "op": "eq", "value": 0},
        {"field": "target.pm4_zero.reemit_foreign_pk", "op": "eq", "value": 0},
        {"field": "target.pm4_zero.reemit_unverified_pk", "op": "eq", "value": 0}])


def pm4_zero_strict_census_item(eid, note=""):
    """THE DEFAULT PATH'S PM4 ZERO WITH NO ALLOWANCE (the devcreate bake, native/devcreate-fix 2026-10-15) from
    tools/perf/transport_census.py --pm4-zero --base-shows devcreate over the census pair (census0 SR_NATIVE_DEVCREATE=0,
    census1 the default; both SR_NATIVE_BRACKETS=1, both ended by the graceful quit so the terminal report covers the
    tail): both censuses live, complete, reconciled, covering the run and mapped; the base showing 820F0048's re-emit
    (all six keys on 82112050's line - the transport is native in both arms, so the transport group cannot be the
    base's witness); the target holding NO packet in gameplay - every F line from the ingame marker to the run's end and
    every producer line over the census interval, nothing let through (ingame_allowed_pk is 0 by construction). The JSON
    is the step's run-unique output: committed last, the step's own verdict stamped in it."""
    return ev(eid, "json", "live-observe", "census", note=note, checks=[
        {"field": "committed", "op": "eq", "value": True},
        {"field": "runner.verdict", "op": "eq", "value": "PASS"},
        {"field": "verdict", "op": "eq", "value": "PASS"},
        {"field": "checks.base_devcreate_exercised", "op": "eq", "value": True},
        {"field": "checks.target_ingame_frames", "op": "eq", "value": True},
        {"field": "checks.target_pm4_zero_from_ingame", "op": "eq", "value": True},
        {"field": "checks.target_pm4_zero_interval", "op": "eq", "value": True},
        {"field": "checks.base_reconciled", "op": "eq", "value": True},
        {"field": "checks.target_reconciled", "op": "eq", "value": True},
        {"field": "checks.base_frames_complete", "op": "eq", "value": True},
        {"field": "checks.target_frames_complete", "op": "eq", "value": True},
        {"field": "checks.base_covers_run", "op": "eq", "value": True},
        {"field": "checks.target_covers_run", "op": "eq", "value": True},
        {"field": "checks.base_mapped", "op": "eq", "value": True},
        {"field": "checks.target_mapped", "op": "eq", "value": True},
        {"field": "checks.base_coverage_live", "op": "eq", "value": True},
        {"field": "checks.target_coverage_live", "op": "eq", "value": True},
        {"field": "target.coverage.state", "op": "eq", "value": "live"},
        # the block is always written: the counts themselves, not only the derived checks; no allowance at all
        {"field": "target.pm4_zero.allow", "op": "eq", "value": []},
        {"field": "target.pm4_zero.ingame_pk", "op": "eq", "value": 0},
        {"field": "target.pm4_zero.ingame_other_pk", "op": "eq", "value": 0},
        {"field": "target.pm4_zero.ingame_frames_over", "op": "eq", "value": 0},
        {"field": "target.pm4_zero.interval_other_pk", "op": "eq", "value": 0},
        {"field": "target.pm4_zero.reemit_pk", "op": "eq", "value": 0}])


def pm4_zero_zpd_census_item(eid, note=""):
    """THE DEFAULT PATH'S PM4 ZERO IN METROPOLIS (p2.zpd's bake, native/zpd-bake 2026-10-16) from
    tools/perf/transport_census.py --pm4-zero --base-shows zpd over the census pair (census0 SR_NATIVE_ZPD=0, the probe
    the default; both from the chapter-12 seed playing the zpd tape, SR_NATIVE_BRACKETS=1, ended by the graceful quit so
    the terminal report covers the tail): both censuses live, complete, reconciled, covering the run and mapped; the
    base showing the HUD glow queries as guest PM4 (EVENT_WRITE_ZPD on D3DQueryIssueBeginEnd 82102BC8's line - the bracket
    entry whose scope holds 82101E08's packets; the title caller 82304058 is no entry, so no census prints its line -
    and the zpd group > 0; the transport and devcreate native in both arms); the target holding NO packet in gameplay - every F line from the ingame marker to
    the run's end and every producer line over the census interval, nothing let through: the queries' packets travel as
    records (no literal-PM4 gap in the seed-12 era). The JSON is the step's run-unique output: committed last, the step's
    own verdict stamped in it."""
    it = pm4_zero_strict_census_item(eid, note)
    it["checks"] = [{"field": "checks.base_zpd_exercised", "op": "eq", "value": True} if
                    c["field"] == "checks.base_devcreate_exercised" else c for c in it["checks"]] + [
        {"field": "base_zpd.zpd_key_pk", "op": "gt", "value": 0}]
    return it


def census_armed_item(eid, note=""):
    """native/composed-census: the producer census's positive witness - gfx/pm4_coverage's [PM4_COV] armed line (once,
    at the counter's first hook) with the producer census on (brackets=1). A roll without it never had the counter on
    (or ran an exe before the witness); its silence is not a census of zero."""
    return ev(eid, "text", "live-observe", "census", select=r"\[PM4_COV\] armed ", which="last",
              pattern=r" brackets=1 ", note=note)


def census_ends_item(eid, records, note=""):
    """native/composed-census: how the census's frames ended - the last [PM4_COV] ENDS line (printed beside every CUM
    report; a census that never ended a frame prints none, and this item FAILs on 'no line matches select').
    records=True (the composed env: SR_NATIVE_SWAP_BLOCK makes each present a kRkSwap record with no XE_SWAP packet):
    frames ended at presents (present > 0) - the frame boundary tcmp2's census1 never had; records=False (every kind
    off): every frame at an XE_SWAP packet, none at a record (present=0)."""
    pat = (r" present=[1-9]\d* presents=[1-9]\d*\s*$" if records else
           r" xeSwap=[1-9]\d* present=0 presents=[1-9]\d*\s*$")
    return ev(eid, "text", "live-observe", "census", select=r"\[PM4_COV\] ENDS \w+ frames=", which="last",
              pattern=pat, note=note)


def composed_pacing_item(eid, note=""):
    """p2.transport's `pacing` unknown: tools/perf/pacing_cmp.py over the console-paced pair (SR_HIGHFPS=0) - the
    composed roll's vblanks per present (mean, distribution, tail, hitch rate, severity max), gates per present (mean,
    doubled proportion) and throttle wait (median, p95 / p99 exceedance, max, spike share) against the all-OFF control's.
    Three outcomes (Astra round 3): a statistical check PASSES only when the bootstrap bound of the regression is inside
    its practical margin, FAILS when the bound is beyond it, else INCONCLUSIVE; a FAIL anywhere is the verdict (and is
    written to the ledger whatever the calibration says); an INCONCLUSIVE check, or a would-be PASS whose CALIBRATION (a
    second all-OFF control of the same session against the first) is not a PASS, makes the step INCONCLUSIVE - no item,
    never counted toward closing pacing. Coverage counted in present ids up to the last present the trace saw. The
    tolerances and the window size are pre-registered in the tool; the golden Xenia rate is information."""
    names = ("control_pairs", "target_pairs", "control_no_duplicate_ids", "target_no_duplicate_ids",
             "control_one_gate_per_present", "target_one_gate_per_present", "gate_doubles_match", "vblanks_match",
             "vblanks_distribution_match", "vblanks_tail_match", "vblanks_max_match", "vblanks_hitch_rate_match",
             "throttle_wait_match", "throttle_wait_p95_match", "throttle_wait_p99_match", "throttle_wait_max_match",
             "throttle_wait_spikes_match")
    return ev(eid, "json", "live-observe", "timing", note=note, checks=[
        {"field": "committed", "op": "eq", "value": True},  # the run-unique output, committed last
        {"field": "runner.verdict", "op": "eq", "value": "PASS"},  # the step's own verdict (do_pacing stamps it)
        {"field": "verdict", "op": "eq", "value": "PASS"},
        {"field": "calibration.verdict", "op": "eq", "value": "PASS"}] + [
        {"field": "checks." + n, "op": "eq", "value": True} for n in names])


def pacing_trace_item(eid, note=""):
    """A pacing roll's P2 trace (Astra round 1, 2026-10-14): the pacing arms stay uninstrumented (no
    SR_NATIVE_SEG_TILE: they measure timing), so no forced drained point exists to reconcile - trace_item's P2.7
    invariants and BlockUntilIdle completing, plus the drained check (no BlockUntilIdle return saw dev+0x2A74
    non-zero) and the pacing records present. The forced-drain / terminal reconciliation stays the instrumented
    probe's (composed_trace_item)."""
    it = trace_item(eid, note)
    it["checks"] = it["checks"] + [
        {"field": "idle.a74_nonzero_exit", "op": "eq", "value": 0},
        {"field": "pacing.pairs", "op": "gt", "value": 0}]
    return it


def composed_trace_item(eid, note=""):
    """bracket_trace_item (the P2.7 invariants, the close order with the bracket class, BlockUntilIdle drained) plus
    the drained reconciliation: no BlockUntilIdle return saw dev+0x2A74 non-zero (the pair replaces EndSync, BRACKET
    never moves the count)."""
    return bracket_trace_item(eid, note)


def snapshot_detail(d):
    """A snapshot step's row text from flush_split_snapshot.py's JSON: the grader's own tail_s (0.0 when the last
    snapshot is the log's last timestamped line - fresh, not a sentinel), or n/a when it has none (frs1: a 0.0 tail
    printed as -1.000 through `or`)."""
    t = d.get("tail_s")
    tail = "%.3f s" % t if isinstance(t, (int, float)) and not isinstance(t, bool) else "n/a"
    return "snapshots %s tail %s (bound %s s) fresh %s" % (d.get("snapshots"), tail, d.get("fresh_bound_s"), d.get("fresh"))


# ---- the tcmp1 diagnosis's fixes (transport-fix lane, 2026-10-14): the decline counters and the binding record ----

# The swapSdkWhy reasons in admission order (kernel/native_flush.hpp SwapDeclWhy), each named in the item's pattern:
# 0 = a reason that is (or may be) a failed read, a refused writability or a failed record admission; None = a refused
# device / global MODE, free. Since Astra round 1 a failed read is its own reason (hsioRead / movieGlobalRead /
# rootRead) apart from the refused state (hsio / movieGlobal / root); an exe before the split prints only the combined
# reason, which the pattern then requires at 0 (it cannot tell a read failure from a state).
SWAP_WHY = (("devRead", 0), ("devWrite", 0), ("front", 0), ("stack", 0), ("tiling", None), ("flags2A39", None),
            ("movieCapture", None), ("pix", None), ("timestamps", None), ("3B28", None), ("hsio", "split"),
            ("movieGlobal", "split"), ("rect", 0), ("sysbuf", None), ("root", "split"), ("export", None),
            ("cursorAlign", 0), ("cursorWrite", 0), ("entry", 0), ("fallbackFront", 0), ("fallbackScaler", 0))


def swap_why_pattern():
    parts = []
    for name, rule in SWAP_WHY:
        if rule == 0:
            parts.append(name + "=0")
        elif rule == "split":  # the split exe: <name>Read=0 <name>=n; the older one: <name>=0
            parts.append("(?:%sRead=0 %s=\\d+|%s=0)" % (name, name, name))
        else:
            parts.append(name + "=\\d+")
    return " ".join(parts) + "(\\s|$)"


def swap_why_item(eid, note=""):
    """The [NATIVE_FLUSH] swapSdkWhy line (kernel/native_flush.hpp SwapDeclWhy): no SDK swap declined for memory - every
    reason named, no wildcard (Astra round 1: `rect=1`, a failed rectangle read, passed under `.*`). At 0: the device /
    stack / front reads and writability, every failed global / root read, the rectangle read, the cursor's alignment and
    its reserved block's writability (tcmp1's gameplay declines: the block on pages the GPU write tracker owned
    read-only), the Entry, the two post-swap fallbacks. Free: the refused device / global MODES (the boot decline SWAP
    alone shows too is one; the line names it)."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] \w+ swapSdkWhy ", which="last",
              pattern=swap_why_pattern(), note=note or "no memory decline; mode reasons named")


def swap_boot_only_item(eid, note=""):
    """The swapSdk line: at most one decline in the whole run (the pre-registered expectation: only the boot decline
    SWAP alone shows too - sw1 probe / cppm4 / stress each 1)."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] \w+ swapSdk calls=", which="last",
              pattern=r"calls=[1-9]\d* decl=[01](\s|$)", note=note or "at most the boot decline")


def bracket_memory_item(eid, note=""):
    """The [NATIVE_FLUSH] bracketMemory line (kernel/native_flush.hpp BracketMemorySite): no memory decline at any site
    (tcmp1: probe 1, stress 34; BRACKET alone under stress 10)."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] \w+ bracketMemory ", which="last",
              pattern=r"frame=0 dev=0 stack=0 lists=0 align=0/0/0 prefix=0/0/0 cursor=0/0/0 entry=0",
              note=note or "no memory decline at any site")


def binding_trace_item(eid, note=""):
    """composed_trace_item with the BRACKET_END binding record traced (kind 24 aux 5: the exe carries it) - the final
    bracket a forced end cuts is then tail_bound_unapplied, never `skipped`, when its CP lifecycle / plan records prove
    it unfinished and in order (p2_trace.py segment_order, Astra round 1)."""
    it = composed_trace_item(eid, note)
    it["checks"] = it["checks"] + [{"field": "segment_order.bracket_binds", "op": "gt", "value": 0}]
    return it


def graceful_trace_item(eid, note=""):
    """binding_trace_item on a roll ended by the graceful quit (tools/graceful-quit: ring cut, CP drained to the final
    write index): nothing is cut, so the terminal segment_order is whole - no tail excused (tail_bound_unapplied 0), no
    bracket begun without its completion, every bound id applied, every published segment taken (the CP's lifecycle
    records present: brackets_open 0, not the -1 of an exe without them)."""
    it = binding_trace_item(eid, note)
    it["checks"] = it["checks"] + [
        {"field": "segment_order.tail_bound_unapplied", "op": "eq", "value": 0},
        {"field": "segment_order.brackets_open", "op": "eq", "value": 0},
        {"field": "segment_order.bound_unapplied", "op": "eq", "value": 0},
        {"field": "segment_order.published_untaken", "op": "eq", "value": 0}]
    return it


GRACEFUL = "SR_DRIVE_GRACEFUL_QUIT=1"  # a roll step's gate: that roll ends by the graceful quit (sr_drive reads -Gates)
# mem.resources' frozen-pass A/B (plan memres_ab; gfx/resource_ab.hpp): the native route ON, its decider toggled per
# present (dual bookkeeping: the page model keeps its state in the native presents too)
MEMRES_AB = "SR_MEM_RESOURCES=1,SR_AB_TOGGLE=SR_MEM_RESOURCES,SR_AB_PRESENT=1"
# The Warworld seed (archive/saves/02_ch02_warworld_start: straight into the arena HUD, docs/SAVES.md) and its verify
# tape (the memres census rolls' route), repo-relative to --repo (the dev checkout's save library)
WW_SEED = "archive/saves/02_ch02_warworld_start"
WW_TAPE = "archive/saves/_verify/02_ch02_warworld_start_2087CA4ACDB4_10142334/verify_play.txt"
# mem.resources boundaries_complete (plan memres_bounds): the shadow census beside the page model
# (SR_MEM_RESOURCES=census: the registry decides in shadow on the guest's own records, the model decides) with both
# model checkers on - the write watch's keep re-hash (SR_SHM_WT_CHECK=1) and the range cache's
# (SR_NB_SHM_RANGE_CHECK=1). tools/perf/mem_res_census.py grades each era: a missed boundary = the registry's arena
# differs from RAM in bytes a request fetched; an unchecked keep = GAP.
MEMRES_CENSUS = "SR_MEM_RESOURCES=census,SR_SHM_WT_CHECK=1,SR_NB_SHM_RANGE_CHECK=1"
# the later chapters' seeds and their verify tapes (archive/saves/INDEX.md: LOADS OK, SUCCESS)
CH18_SEED = "archive/saves/18_ch18_before_twister"
CH18_TAPE = "archive/saves/_verify/18_ch18_before_twister_2087CA4ACDB4_10150313/verify_play.txt"
CH17_SEED = "archive/saves/17_ch17"
CH17_TAPE = "archive/saves/_verify/17_ch17_2087CA4ACDB4_10150308/verify_play.txt"
# the ending (lane E's era matrix row R3, tools/perf/era: dev 04287aac): the chapter-17 seed, two SKIP CHAPTER tickets
# (dev-tool assisted) - CinematicSupesDecides, the ending CinematicLexIsland, chapter 19 and the post-game; its gates
# (era_matrix.py ROWS / COMMON): the level-skip plan, port features, cutscenes never skipped by the drive, mods off
END_TAPE = "tools/perf/era/r3_ending_play.txt"
END_PLAN = "tools/perf/era/r3_empty_plan.txt"
END_GATES = "SR_PORT_FEATURES=1,SR_DRIVE_SKIP_CUTSCENES=0,SR_MODS=0"
# mem.resources textures_from_resolves + resolve_writeback_readers (plan memres_readers): the native route deciding
# (SR_MEM_RESOURCES=1) with the texture-identity census (gfx/resource_texid.hpp) and Codex's bounded guest-reader
# probe (gfx/resource_reader_probe, kernel/resource_reader_observe), both observe-only
MEMRES_READERS = "SR_MEM_RESOURCES=1,SR_MEM_RES_TEXID=1,SR_GUEST_READER_PROBE=1"


def json_checks(d, checks):
    """The failed checks of `checks` (the ledger's json item form) against the dict d, as text."""
    bad = []
    for c in checks:
        v = d
        for k in c["field"].split("."):
            v = v.get(k) if isinstance(v, dict) else None
        want, op = c["value"], c.get("op", "eq")
        ok = {"eq": lambda: v == want, "ne": lambda: v != want,
              "gt": lambda: v is not None and v > want, "ge": lambda: v is not None and v >= want,
              "lt": lambda: v is not None and v < want, "le": lambda: v is not None and v <= want}[op]()
        if not ok:
            bad.append("%s=%r (want %s %r)" % (c["field"], v, op, want))
    return bad


def coher_cp_item(eid, note=""):
    """native/coher-sentinel: the last [GPU_COHER] line (the census's companion, printed with every [GPU_CP] census):
    the coherency requests the CP applied - the guest kick's 11-dword blocks, the lock core's inline owner blocks and
    the native close's kRkInvalidate records alike - none reaching 4 GB (top) and none the bogus {base 0xFFFFF000,
    size 0x1000} (page). Before the fix every native close after a reset / guest kick and every guest kick after a
    native close carried that page. requests > 0 keeps the witness non-vacuous."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[GPU_COHER\] \w+ requests=", which="last",
              pattern=r"requests=[1-9]\d* whole=\d+ top=0 page=0", note=note)


def coher_close_item(eid, note=""):
    """native/coher-sentinel: the native close's own coherency range blocks on the last [NATIVE_FLUSH] seg line -
    coherTop (ranges reaching 4 GB: the all-ones sentinel's bogus page or a merge into all ones) must read 0."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] seg closed=", which="last",
              pattern=r" coherRange=\d+ coherTop=0(\s|$)", note=note)


# ---- display.registers (display-registers lane, 2026-10-14; docs/NATIVE_ROADMAP.md "DISPLAY.REGISTERS BUILD NOTE"):
# SR_NATIVE_VBLANK = GATE (b) with (c). PRE-REGISTERED before any roll, from the build note: the [NATIVE_VBLANK] line
# (kernel/native_vblank.cpp finalLine, with the CP census and at exit) of a pure-ON roll shows every vblank tick native
# (guest=0) and every pacing call host (guest=0), the per-vblank check clean (traceBad=0) - the same check the OFF arm of
# the toggled A/B runs on the guest ISR (the oracle trace) - no swap callback (null in this title, Appendix C.2), and
# its two dispreg sources retired (scanout=0, vsyncBit=0); the dispreg column per ingame frame exactly 0 on the
# composed transport (SWAP carries the syscmdbuf fill), the whole run's cumulative too.

DREG_SELECT = r"\[NATIVE_VBLANK\] \w+ state="
DREG_FORBID = [r"\[NATIVE_VBLANK\] trace BAD", r"\[NATIVE_VBLANK\] REFUSED", r"\[NATIVE_VBLANK\] pacing MISMATCH",
               r"\[NATIVE_DISPLAY\] source0 (failed|requires)"]


# THE DEFAULT FORM (Astra round 1 on native/dreg-bake, P2): perf_run passes the parent environment to every roll, so an
# exported SR_NATIVE_VBLANK=1 would make each "default" arm explicit - and vblank_item accepts either form. The exe prints
# one On line a process when the gate latches on (kernel/native_vblank.cpp computeState): "SR_NATIVE_VBLANK on (=1): ..."
# or "SR_NATIVE_VBLANK on (the baked default; ...)". The bake's default arms require the default form
# (vblank_default_item) and their vblank items forbid the explicit one (default_form=True); a live run of a plan with
# "default_keys" refuses to start while the parent environment carries one of them (Runner.env_refusal).
VBLANK_ON_SELECT = r"\[NATIVE_VBLANK\] SR_NATIVE_VBLANK on \("
VBLANK_EXPLICIT_ON = r"\[NATIVE_VBLANK\] SR_NATIVE_VBLANK on \(=1\)"


def vblank_default_item(eid, note=""):
    """A default arm of the bake ran the DEFAULT form: at least one On line, every one naming the baked default, none
    naming =1."""
    return ev(eid, "text", "live-observe", "guest-answers", select=VBLANK_ON_SELECT, which="all",
              pattern=r"SR_NATIVE_VBLANK on \(the baked default;", forbid=[VBLANK_EXPLICIT_ON],
              note=note or "the gate on by its default (SR_NATIVE_VBLANK unset), not =1")


def vblank_item(eid, toggled=False, note="", default_form=False):
    """The last [NATIVE_VBLANK] line. Untoggled: state=on, ticks all native, every pacing call host, traceBad 0, no swap
    callback, scanout 0 and vsyncBit 0 (sysbuf is SWAP's: information). Toggled (SR_AB_TOGGLE=SR_NATIVE_VBLANK): both
    arms ran - native and guest ticks, host and guest pacing calls - under one per-vblank check (which also compares the
    live scan-out registers with the host counter), and the OFF arm's inputs kept live (scanout > 0, vsyncBit 1). Both:
    early=0 and mismatch=0 - the host counter answers today's V_COUNTER by construction (nextVCounter), so every pacing
    call's decision equals the one today's registers give (the ON arm's shadow, against the live registers when toggled)
    and the guest body's equals the host prediction (the OFF arm's). released / late / vcount are information.
    default_form: a default arm of the display-registers bake - the explicit On line is forbidden as well
    (vblank_default_item requires the default one)."""
    if toggled:
        pat = (r"state=on toggled=1 ticks=[1-9]\d* native=[1-9]\d* guest=[1-9]\d* released=\d+ callbacks=0 traceBad=0 "
               r"late=\d+ maxGapUs=\d+ vcount=\d+ \| pacing calls=[1-9]\d* host=[1-9]\d* guest=[1-9]\d* wait=\d+ "
               r"now=\d+ next=\d+ early=0 mismatch=0 \| dispreg scanout=[1-9]\d* sysbuf=\d+ vsyncBit=[1-9]\d*\b")
    else:
        pat = (r"state=on toggled=0 ticks=[1-9]\d* native=[1-9]\d* guest=0 released=\d+ callbacks=0 traceBad=0 "
               r"late=\d+ maxGapUs=\d+ vcount=\d+ \| pacing calls=[1-9]\d* host=[1-9]\d* guest=0 wait=\d+ now=\d+ "
               r"next=\d+ early=0 mismatch=0 \| dispreg scanout=0 sysbuf=\d+ vsyncBit=0\b")
    return ev(eid, "text", "live-observe", "guest-answers", select=DREG_SELECT, which="last", pattern=pat,
              forbid=DREG_FORBID + ([VBLANK_EXPLICIT_ON] if default_form else []),
              note=note or "the host vblank service and pacing; the per-vblank check clean")


def dreg_trace_item(eid, note=""):
    """trace_item (the P2.7 invariants; BlockUntilIdle completes, no drain spin over a frame) plus the drain rule's own
    check: no BlockUntilIdle return saw dev+0x2A74 non-zero. Neither host form touches dev+0x2A74 (the build note);
    this is the regression check the orchestrator's 'BlockUntilIdle drained' asks of every ISR replacement."""
    it = trace_item(eid, note)
    it["checks"] = it["checks"] + [{"field": "idle.a74_nonzero_exit", "op": "eq", "value": 0}]
    return it


def dispreg_zero_item(eid, base_tag, note=""):
    """dispreg per ingame frame: the composed gate-OFF roll shows the column live (>= 2: the two scan-out answers per
    vblank at least) and the gated roll holds exactly 0."""
    return ev(eid, "meter_delta", "live-observe", "meter", base=base_tag, field="dispreg", base_min=2.0, max=0.0,
              world_min=0.5, note=note)


def dispreg_run_item(eid, note=""):
    """The whole run (boot included): the last [NATIVE_METER_RUN] line's cumulative dispreg is 0."""
    return ev(eid, "text", "live-observe", "meter", select=r"\[NATIVE_METER_RUN\]", which="last",
              pattern=r" nrec=\d+ dispreg=0 ", note=note)


# ---- boot gaps (native/boot-gaps, 2026-10-16; docs/NATIVE_ROADMAP.md "BOOT GAPS BUILD NOTE"): the default build's last
# literal-PM4 producers - frame 1's UNSTAMPED WAIT_UNTIL / RB_BC_CONTROL pair (GpuRingEmitModeStateReg 82103238 inside
# the device-less D3DDeviceCreate bracket; SR_NATIVE_MODE_STATE) and the first Swap's persisted display (SWAP_DECL #1
# why=flags2A39, frame 2's 527 packets: 82105DD0's gamma ramp, the scaler block, Swap's own front / REG_RMW 0x1E4E /
# scaler reset; SR_NATIVE_DISPLAY) - as records, both default ON (=0 the guest bodies and the decline: the A/B arm).
# PRE-REGISTERED from dcf2's census1 (exe 864eb1c17db4, the devcreate-baked default: frame 1 pk=2 UNSTAMPED regs
# 05:1,0F:1; frame 2 pk=527 ops 21:1,3C:2,45:256 regs 18:5,19:263; every later frame 0; gapApplied=2 gapKB=9 from the
# first seg line to the terminal one; swapSdk decl=1, swapSdkWhy flags2A39=1) and the first Swap's device modes there
# (2A38..3B = 00 B4 34 00: the persist bit, the scaler bit, no gamma bit; the table ramp - 256 COND_WRITE).
BOOT_GAPS_KEYS = ("SR_NATIVE_DISPLAY", "SR_NATIVE_MODE_STATE")
# the obligations the plan's items go to, in this order: the first Swap (p2.transport), frame 1's pair
# (boot.device_create), the meter pair (display.registers), the census zero (acceptance.zero)
BOOT_GAPS_OBLIGATIONS = ("p2.transport", "boot.device_create", "display.registers", "acceptance.zero")
BOOT_GAPS_OFF = ",".join(k + "=0" for k in BOOT_GAPS_KEYS)   # the bake's =0 arm (the guest bodies, the decline)
BOOT_GAPS_BOOT_PK = 529     # dcf2 census1's boot: frame 1's 2 + frame 2's 527 (the =0 arm reproduces it)
# mode_state_bake's resolvers beyond the boot.device_create items (census0's, the CHECK's - neither runs the leaf as a
# record): the default census roll's display line (the mode-state leaf taken once as a record, no decline), its F
# records from frame 1 (no packet, frame 1's pair included) and era1's zero from boot (boot pk 0, gapApplied 0) - items
# routed to p2.transport and acceptance.zero, resolving this unknown too (apply_ledger takes a resolver from any of the
# plan's obligations). Astra round 1 on native/boot-gaps, P2; bgb1's NOTES amendment before its step 3.
BOOT_GAPS_MODE_DEFAULT = ("bgb_census1_disp", "bgb_census1_frames", "bgb_era1")
BOOT_GAPS_BOOT_GAPS = 2     # ... as two literal gap parts the CP executed (gapApplied)
DISPLAY_SELECT = r"\[NATIVE_FLUSH\] \w+ display gate="


def bootgaps_display_item(eid, form="on", note=""):
    """The last [NATIVE_FLUSH] <why> display line (kernel/native_display_emit.cpp displayReport). form "on" (the default
    build): both gates on, the persisted display taken once, its table ramp (821056E0) and the scaler block (8210A788)
    each once as records, no set-gamma (0x2A3B bit 0x40 is never set on this port), the mode state once (821058B0's one
    call at device creation); no decline, no platform scaler program, no reserve failure; the CP applied the ramp and the
    scaler record once each. "cppm4": the same, the CP rendering them back to PM4 (applied by the walker: cp 0 / 0).
    "off" (SR_NATIVE_DISPLAY=0 SR_NATIVE_MODE_STATE=0): both gates off, nothing taken or counted (the guest bodies).
    "check" (SR_NATIVE_FLUSH_CHECK=1: record mode off): both gates on, every leaf standing down uncounted. "ab" (the
    per-present A/B of both gates: the boot runs whichever arm the toggle's mask holds - seeded, SR_AB_SEED - and the
    first VdSwap flips it inside the first Swap, so the persisted display's leaves after VdSwap may run the other arm:
    each producer taken once or not at all, in any combination): no decline, no program, no reserve failure in either
    arm. Every form prints the presenter's ramp hash (lut=, information here; the era step compares the two census
    arms')."""
    if form == "ab":
        head = r"gate=1 modeGate=1 persist=[01] ramp=[01]/0 setGamma=0 scaler=[01] modeState=[01]"
        cp = r"[01] scaler=[01]"
    else:
        head = re.escape({"on": "gate=1 modeGate=1 persist=1 ramp=1/0 setGamma=0 scaler=1 modeState=1",
                          "cppm4": "gate=1 modeGate=1 persist=1 ramp=1/0 setGamma=0 scaler=1 modeState=1",
                          "off": "gate=0 modeGate=0 persist=0 ramp=0/0 setGamma=0 scaler=0 modeState=0",
                          "check": "gate=1 modeGate=1 persist=0 ramp=0/0 setGamma=0 scaler=0 modeState=0"}[form])
        cp = {"on": "1 scaler=1", "cppm4": "0 scaler=0", "off": "0 scaler=0", "check": "0 scaler=0"}[form]
    pat = (r"display %s \| decl ramp=0 setGamma=0 scaler=0 modeState=0 \| scalerProgram=0 reserveFail=0 \| cp ramp=%s "
           r"\| lut=[0-9a-f]{16}\s*$" % (head, cp))
    return ev(eid, "text", "live-observe", "generation", select=DISPLAY_SELECT, which="last", pattern=pat, note=note)


def bootgaps_swap_item(eid, declines=0, note=""):
    """SWAP's whole-run declines (the last [NATIVE_FLUSH] <why> swapSdk line, cumulative from boot): 0 on the default
    build - the first Swap's persisted display no longer refused - or exactly `declines` (the =0 arm: dcf2's one)."""
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] \w+ swapSdk calls=", which="last",
              pattern=r"calls=[1-9]\d* decl=%d\s*$" % int(declines), note=note)


def bootgaps_why_item(eid, flags=0, note=""):
    """The declines by reason (the last swapSdkWhy line): flags2A39 = `flags` (0 default, 1 the =0 arm's persisted
    display; None: either - the A/B roll's boot) and the persisted display's literal fallback (fallbackPersist: its
    REG_RMW written as the SDK's bytes when no Entry took it) 0."""
    fl = r"[01]" if flags is None else "%d" % int(flags)
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] \w+ swapSdkWhy ", which="last",
              pattern=r"\bflags2A39=%s\b.*\bfallbackPersist=0\b" % fl, note=note)


def bootgaps_gaps_item(eid, gaps=0, note=""):
    """The literal-PM4 gap parts the CP executed, cumulative FROM BOOT (the last [NATIVE_FLUSH] seg line: every seg line
    is cumulative): gapApplied = `gaps` (0 on the default build; the =0 arm's two boot gaps), gapKB 0 with none, and
    segcounters_item's P2-B tripwires 0."""
    kb = r"0" if not gaps else r"\d+"
    return ev(eid, "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] seg closed=", which="last",
              pattern=(r" gapKB=%s recApplied=\d+ gapApplied=%d idMiss=0 mismatch=0 .* footStray=0 feetLost=0 "
                       r"coherDropOld=\d+ capDrop=0 litLenBad=0" % (kb, int(gaps))), note=note)


def bootgaps_run_item(eid, pm4=0, note=""):
    """The native meter's whole run (the last [NATIVE_METER_RUN] line: cumulative from the first VdSwap): pm4 = `pm4`
    (0 on the default build; dcf2 census1's 529 on the =0 arm)."""
    return ev(eid, "text", "live-observe", "meter", select=r"\[NATIVE_METER_RUN\]", which="last",
              pattern=r"frames=[1-9]\d* \| pm4=%d " % int(pm4), note=note)


def bootgaps_vblank_item(eid, sysbuf=0, note=""):
    """display.registers' breakdown on the meter rolls (SR_NATIVE_VBLANK by its baked default - the display-registers
    bake, merged from dev ddfc6be0 - the explicit On line forbidden as vblank_item's default_form): the host vblank
    service on, no scan-out answer, no vsync-enable bit, and the system command buffer descriptor answered `sysbuf`
    times - 0 on the default build (no SDK-body Swap calls VdGetSystemCommandBuffer), 1 on the =0 arm
    (meter_run_swap_decl's one)."""
    return ev(eid, "text", "live-observe", "guest-answers", select=DREG_SELECT, which="last",
              pattern=r"state=on .* \| dispreg scanout=0 sysbuf=%d vsyncBit=0\b" % int(sysbuf),
              forbid=DREG_FORBID + [VBLANK_EXPLICIT_ON], note=note)


def bootgaps_dispreg_item(eid, dispreg=0, note=""):
    """The whole run's dispreg (the last [NATIVE_METER_RUN] line, cumulative): `dispreg` - 0 on the default build
    (display.registers' run-cumulative zero, contradiction meter_run_swap_decl's criterion), 1 on the =0 arm."""
    return ev(eid, "text", "live-observe", "meter", select=r"\[NATIVE_METER_RUN\]", which="last",
              pattern=r" nrec=\d+ dispreg=%d " % int(dispreg), note=note)


def bootgaps_witness(prefix, tag, form="on", note=""):
    """The strict witness of a default-build roll: the display line, SWAP with no decline, the reasons, the gaps from boot
    and the meter's pm4 from boot (not under CPPM4: there the CP renders every record back to PM4 for its walker, which
    the meter counts - the zero is the record path's)."""
    p = "%s_%s_" % (prefix, tag)
    items = [bootgaps_display_item(p + "disp", "cppm4" if form == "cppm4" else "on", note),
             bootgaps_swap_item(p + "swap", 0, note), bootgaps_why_item(p + "why", 0, note),
             bootgaps_gaps_item(p + "gaps", 0, note)]
    return items + ([] if form == "cppm4" else [bootgaps_run_item(p + "run", 0, note)])


def census_zero_items(prefix, tag, note=""):
    """A default census roll holds no packet from boot, whatever wrote it: no [PM4_COV] F record (one per completed frame,
    every producer together, frame 1 included) with pk > 0, and the last CUM report (cumulative from boot; its terminal
    twin under the graceful quit) at pk=0. Producer lines are not graded here: a census with no PM4 at all prints none."""
    p = "%s_%s_" % (prefix, tag)
    return [ev(p + "frames", "text", "live-observe", "census", select=r"\[PM4_COV\] F frame=\d+ ",
               forbid=[r"\[PM4_COV\] F frame=\d+ pk=[1-9]"], note=note),
            ev(p + "cum", "text", "live-observe", "census", select=r"\[PM4_COV\] CUM \w+ frames=", which="last",
               pattern=r" frames=[1-9]\d* pk=0 ", note=note)]


def era_item(eid, control=False, note=""):
    """tools/perf/era_census.py over one census roll (kind newgame: boot, intro, title, fe_menu, opening_fmv, ingame),
    read from the step's run-unique JSON (the tool's own document, with the runner's flat summary `roll` and, for the
    default arm, the two arms' presenter ramps `lut`). Default (control=False): the roll PASSes - every era 0 FROM BOOT
    (C2 F lines + CUM terminal + the meter run; C3 per era), the gaps 0 from boot and agreeing (C5), no ring packet /
    kind-1 / BS item (C6), every required era reached (C4), the terminal snapshot whole (C9), the launch identity bound
    and the run's exe (C10) - and the presenter's display ramp equal to the =0 arm's (the first Swap's DC_LUT state
    through the record = through the guest's literal PM4). control=True (the =0 arm): the roll FAILs exactly in boot -
    the only non-zero era, BOOT_GAPS_BOOT_PK packets from frame 1, BOOT_GAPS_BOOT_GAPS gap parts - every other check
    as the default's (the control is live, the census the same instrument)."""
    common = [{"field": "committed", "op": "eq", "value": True},
              {"field": "runner.verdict", "op": "eq", "value": "PASS"},
              {"field": "roll.checks.C0_usable", "op": "eq", "value": True},
              {"field": "roll.checks.C1_frames", "op": "eq", "value": True},
              {"field": "roll.checks.C4_reached", "op": "eq", "value": True},
              {"field": "roll.checks.C6_items_zero", "op": "eq", "value": True},
              {"field": "roll.checks.C9_terminal", "op": "eq", "value": True},
              {"field": "roll.checks.C10_identity", "op": "eq", "value": True},
              {"field": "roll.checks.zpd_agree", "op": "eq", "value": True},
              {"field": "roll.checks.gaps_agree", "op": "eq", "value": True}]
    if control:
        checks = common + [{"field": "roll.verdict", "op": "eq", "value": "FAIL"},
                           {"field": "roll.nonzero_eras", "op": "eq", "value": ["boot"]},
                           {"field": "roll.boot_pk", "op": "eq", "value": BOOT_GAPS_BOOT_PK},
                           {"field": "roll.boot_first_nonzero", "op": "eq", "value": 1},
                           {"field": "roll.gap_applied", "op": "eq", "value": BOOT_GAPS_BOOT_GAPS}]
    else:
        checks = common + [{"field": "verdict", "op": "eq", "value": "PASS"},
                           {"field": "roll.verdict", "op": "eq", "value": "PASS"},
                           {"field": "roll.checks.C2_run_zero", "op": "eq", "value": True},
                           {"field": "roll.checks.C3_era_zero", "op": "eq", "value": True},
                           {"field": "roll.checks.C5_gaps_zero", "op": "eq", "value": True},
                           {"field": "roll.nonzero_eras", "op": "eq", "value": []},
                           {"field": "roll.boot_pk", "op": "eq", "value": 0},
                           {"field": "roll.gap_applied", "op": "eq", "value": 0},
                           {"field": "lut.equal", "op": "eq", "value": True}]
    return ev(eid, "json", "live-observe", "census", checks=checks, note=note)


def dreg_pacing_steps(goff, gon):
    """The pacing check against native/transport-composed's interface: tools/perf/pacing_cmp.py through
    Runner.do_pacing, Step(calib=, window=), p2_trace.py's kind-29 pacing records, PACE_WINDOW / PACE_LONG and the
    pacing_trace_item / composed_pacing_item helpers - the composed roll with the gate OFF (= GATE (a)) as the control,
    the gated roll as the target, a second control as the calibration, on the console path (SR_HIGHFPS=0, where line %
    matters). On a base without them (dev a9546edd) ONE owed step stands in: a live run records it FAIL, so the plan
    cannot pass without the comparator. PRE-REGISTERED expectation (the build note): the gate outcome per present is
    today's by construction (the host counter advances with today's V_COUNTER rule, wrap points included; the
    [NATIVE_VBLANK] line's early=0 mismatch=0), so the target matches the control within the comparator's margins; a
    FAIL is a finding to report, not a margin to tune."""
    g = globals()
    have = ("calib" in inspect.signature(Step.__init__).parameters and hasattr(Runner, "do_pacing") and
            all(k in g for k in ("PACE_WINDOW", "PACE_LONG", "pacing_trace_item", "composed_pacing_item")))
    if not have:
        return [Step("owed", "pacing", "pacing_cmp.py (native/transport-composed): not on this base")]
    window, long_roll = g["PACE_WINDOW"], g["PACE_LONG"]
    pti, cpi = g["pacing_trace_item"], g["composed_pacing_item"]
    off, on = goff + ",SR_HIGHFPS=0", gon + ",SR_HIGHFPS=0"
    return [Step("roll", "pace0", off, "plain", trace=True, **long_roll),
            Step("roll", "pace1", on, "plain", [vblank_item("dreg_pace1_vblank", note="console path (line % live): "
                                                            "early=0 mismatch=0, today's answers reproduced")],
                 trace=True, **long_roll),
            Step("roll", "pace0b", off, "plain", trace=True, **long_roll),
            Step("p2trace", "pace0_trace", ledger=[pti("dreg_pace0_trace")], base="pace0", window=window),
            Step("p2trace", "pace1_trace", ledger=[pti("dreg_pace1_trace")], base="pace1", window=window),
            Step("p2trace", "pace0b_trace", ledger=[pti("dreg_pace0b_trace")], base="pace0b", window=window),
            Step("pacing", "pacing", base="pace0_trace", target="pace1_trace", calib="pace0b_trace",
                 ledger=[cpi("dreg_pacing")])]


# The display-registers bake's witnesses (native/dreg-bake, 2026-10-15; NATIVE_ROADMAP "DISPLAY-REGISTERS BAKE PLAN").
# THE CPU-2 LOCK (native/dreg-hang): dreg2's GPU hang was the vblank thread holding the lock ~21 ms a tick and taking it
# back to back - the CP's pacing call waited 0.9-33 s (diag1, the old code with the witness: tickMaxUs 49,890, waitMaxUs
# 32,488,017). The fixed exe aabc298d's probes final1-3: tickMaxUs 283 / 92 / 937, waitMaxUs 3 / 3 / 102, tickSlow and
# waitSlow 0. PRE-REGISTERED bounds: the longest tick hold under 20 ms and the longest pacing wait under 100 ms (6 frames:
# a visible hitch on its own); the slow counts (over 1 ms) are information.
CPU2_PATTERN = r"\| cpu2 tickMaxUs=(?:1\d{4}|\d{1,4}) tickSlow=\d+ waitMaxUs=\d{1,5} waitSlow=\d+(?:\s|$)"


def cpu2_item(eid, note=""):
    """The last [NATIVE_VBLANK] line's CPU-2 lock witness (kernel/native_vblank.hpp finalLine's tail): no vblank tick
    held the lock 20 ms, no pacing call waited 100 ms for it (the hang class: ticks of ~21-50 ms back to back, waits of
    seconds); the slow counts are information. Every ON roll of the bake."""
    return ev(eid, "text", "live-observe", "timing", select=DREG_SELECT, which="last", pattern=CPU2_PATTERN,
              forbid=DREG_FORBID, note=note or "the CPU-2 lock: tick hold < 20 ms, pacing wait < 100 ms")


def vblank_off_item(eid, note=""):
    """A =0 arm of the bake ran today's path: no [NATIVE_VBLANK] line at all (no on line, no stand-down line, no census
    line - the state Off prints nothing) and the emulated answers counted (the run's cumulative dispreg > 0)."""
    return ev(eid, "text", "live-observe", "meter", select=r"\[NATIVE_METER_RUN\] ", which="last",
              pattern=r" dispreg=[1-9]\d* ", forbid=[r"\[NATIVE_VBLANK\]"],
              note=note or "SR_NATIVE_VBLANK=0: the guest bodies, the scan-out answers live")


def dispreg_swap_run_item(eid, note=""):
    """THE RUN-CUMULATIVE CRITERION (the orchestrator, 2026-10-15, after dreg3's meter1 rolled: its dispreg_run_item
    demands a cumulative 0 with no allowance for d): the whole run's emulated display-register answers (the last
    [NATIVE_METER_RUN] line's cumulative dispreg) <= the run's SWAP declines (swapSdk decl=), both counted from the same
    run's log - the swapSdk line taken is the first one AFTER that meter line, so it counts at least the meter line's
    interval (both counters only grow). Each declined swap runs the SDK body, whose one VdGetSystemCommandBuffer call is
    one emulated answer (sysbuf); scanout=0 and vsyncBit=0 (the other two sources) are the same roll's vblank_item.
    On the default path d = 1 a run: SWAP's boot decline (SWAP_DECL #1 why=flags2A39, the refused device / global MODE
    at the first present; tbk1 / tbk3a / tbk3b each decl=1 with the transport baked)."""
    return ev(eid, "text_le", "live-observe", "meter",
              lhs={"select": r"\[NATIVE_METER_RUN\] ", "field": "dispreg"},
              rhs={"select": r"\[NATIVE_FLUSH\] \w+ swapSdk calls=", "field": "decl"},
              note=note or "the run's cumulative dispreg <= its SWAP declines (d), same run")


class Step:
    """One step of a plan. kind: replay | roll | gate | meter | p2trace | census | owner | tcensus | pacing | snapshot
    | bootequiv | era | owed (era: tools/perf/era_census.py over the census roll `base`, kind `era`, into a run-unique
    JSON; snapshot: p1.flush_residue's counter snapshots of a roll, graded from boot through the last
    snapshot: `base` = the roll's tag, or "gate/<roll>"; owed: a step this base cannot run - its `gates` names what is
    owed; DRY in a dry run, FAIL in a live one, no item). `ledger` = the evidence items it yields
    (their path is filled from the step's outputs at apply time); `base` = the meter step's default roll tag (census:
    the base census roll, `target` the gated one). A meter step with `pool` ([(base, target), ...], more pairs on the
    same exe) grades the ledger's pool of every pair (with a run-unique receipt). A bootequiv step reads `pairs`
    ([[(arm, roll tag), (arm, roll tag)], ...] in declared run order) and the declared `warmup` roll tag; a bootstall
    step reads `pool` = the declared ON roll tags (every one that ran is counted). `always`: the step runs even after a
    roll ended the plan (a comparison over the rolls taken so far: a crash is graded, never skipped)."""

    def __init__(self, kind, tag, gates="", mode="plain", ledger=(), base=None, trace=False, target=None, play=None,
                 dwell=0, seconds=0, calib=None, window=None, pool=None, pairs=None, warmup=None, always=False,
                 from_save=None, require=None, xtr=False, design=None, allow_keys=(), sparse=False, era=None,
                 devtools=None):
        self.kind, self.tag, self.gates, self.mode, self.ledger = kind, tag, gates, mode, list(ledger)
        # an era step (boot gaps): the roll kind tools/perf/era_census.py grades `base` as (title / newgame / save /
        # seed01 / ending); mode "pass" (the default arm, `target` = the =0 arm's roll for the ramp comparison) or
        # "control" (the =0 arm)
        self.era = era
        # p2.zpd zpd7: the roll's live PM4 traces go to <out>_xtr (SR_PM4_TRACE_DIR) for the Xenia oracle step
        self.xtr = xtr
        # a roll's precondition {name, select, pattern, show, why} on its run.log (p2.zpd: ZPD_FIRED): unmet = VOID, the
        # roll repeated once (REQUIRE_REROLLS); unmet again = the step's verdict NO DATA (Runner.do_roll)
        self.require = require
        # p2.zpd: a roll / gate started from a saved game (perf_run / flicker_gate --from-save; a content root,
        # repo-relative - the chapter seed library archive/saves/<seed>, docs/SAVES.md)
        self.from_save = from_save
        self.base, self.trace, self.target = base, trace, target  # meter: base = the default roll, target = the gated roll
        self.calib = calib  # pacing: a second control step (control vs control must pass, else INCONCLUSIVE)
        self.window = window  # p2trace: the pacing window in present ids (p2_trace.py --pacing-window)
        # a long roll (the bake soak): another harness tape (repo-relative), seconds in play after the HUD, the drive's timeout
        self.play, self.dwell, self.seconds = play, dwell, seconds
        self.pool, self.pairs, self.warmup, self.always = list(pool or []), pairs, warmup, always
        # bootequiv / bootstall: the evaluator's registered design (--design; None = the tool's default, dcl's) and, for
        # the stall evaluator, the plan's observe-only instruments the counted boots may differ in (--allow-key)
        self.design, self.allow_keys = design, tuple(allow_keys or ())
        # a plain roll that keeps the sparse dump though no meter reads it (the bake's warm-up: one configuration with
        # the pair rolls that feed the meter, as boot_equiv.py requires beyond the arm key)
        self.sparse = sparse
        # a dev-tool assisted roll (the era matrix's R3 ending: SKIP CHAPTER tickets): the level-skip plan, repo-
        # relative; the roll runs with SR_DEVTOOLS=levelskip, that plan and its own output folder (Runner.gates_for)
        self.devtools = devtools


def roll_long(exe, out, gates, repo, play, dwell, seconds, stage_args=(), from_save=None):
    """perf_run gameplay / port with another tape and a longer dwell (the soak); otherwise flicker_gate.roll's command.
    from_save (p2.zpd): the roll LOADs that content root's newest save instead of NEW GAME (perf_run --from-save; the
    tape then starts with flicker_gate's SAVE_PRELUDE awaits, as tools/perf/zpd_play.txt does)."""
    cmd = [sys.executable, os.path.join(repo, "tools", "perf", "perf_run.py"), "--scenario", "gameplay", "--config", "port",
           "--exe", exe, "--out", out, "--hz", "0", "--gates", gates, "--play", tape_path(repo, play)]
    if from_save:
        cmd += ["--from-save", from_save]
    if dwell:
        cmd += ["--dwell", str(dwell)]
    if seconds:
        cmd += ["--seconds", str(seconds)]
    cmd += list(stage_args)   # --stage / --stage-source (SSD staging), only when the plan runs staged
    with open(out + ".txt", "w", encoding="utf-8") as f:
        return subprocess.call(cmd, cwd=repo, stdout=f, stderr=subprocess.STDOUT)


def tape_path(repo, play):
    """A plan's harness tape: the --repo checkout's copy, else this checkout's (p2.zpd zpd1, 2026-10-14: a lane's new
    tape is not in the dev checkout the rolls run from - harness_play exited on the missing file and the roll flew
    nothing)."""
    p = os.path.join(repo, play)
    if os.path.exists(p):
        return p
    mine = os.path.join(LANE, play)
    return mine if os.path.exists(mine) else p


def save_of(repo, step):
    """p2.zpd: the step's --from-save content root (repo-relative; Step.from_save), or None - also for a step object
    without the attribute (the exit tests' fixtures)."""
    fs = getattr(step, "from_save", None)
    return os.path.normpath(os.path.join(repo, fs)) if fs else None


def staged(runner):
    """The runner's --stage / --stage-source as flicker_gate.roll keywords; {} = an unstaged plan, exactly as before
    (a runner built without them - the exit tests' fixture - is unstaged)."""
    return getattr(runner, "stage_kw", None) or {}


def plans():
    g3 = "SR_NATIVE_WSCISSOR=1"
    g5 = "SR_NATIVE_TILERECTS=1,SR_NATIVE_TILEPRED=1"
    gs = "SR_NATIVE_SEGMENTLESS=1"
    # The transport bake (native/transport-bake, 2026-10-15) made the nine transport kinds default ON. The plans that
    # proved ONE kind alone, or whose OFF arm / bound predates the bake (the segmentless meters' census bound, the
    # shader-const-runs plan's ring-path arm), pin the other kinds =0 as their arms ran - the setconst / flush-split
    # bakes' rule. gs_pre = segmentless with the pre-bake transport (every baked kind =0).
    gs_pre = genv(gs, TRANSPORT_OFF)
    gcr = genv(TRANSPORT_OFF, "SR_NATIVE_SHADER_CONST_RUNS=1,SR_NATIVE_SEGMENTLESS=0,SR_HDR_ADAPT_DT=0,"
               "SR_AUDIO_OUT=0,SR_WINDOW_BACKGROUND=1,SR_HARNESS_INPUT=1,SR_HARNESS_REAL_INPUT=0")
    ggate = genv(TRANSPORT_OFF,
                 "SR_NATIVE_GATE=1,SR_NATIVE_FLUSH=1,SR_NATIVE_FLUSH_CHECK=0,SR_NATIVE_SEGMENTLESS=1,"
                 "SR_NATIVE_SEG_TILE=1,SR_NATIVE_SHADER_CONST_RUNS=1,SR_NATIVE_CALLBACK=0,SR_NATIVE_DEVCREATE=0,"
                 "SR_NATIVE_FLUSH_PM4=0,SR_NATIVE_FLUSH_CPPM4=0,SR_HDR_ADAPT_DT=0,SR_AUDIO_OUT=0,"
                 "SR_WINDOW_BACKGROUND=1,SR_HARNESS_INPUT=1,SR_HARNESS_REAL_INPUT=0,SR_ALLOW_FORCING=0,"
                 "SR_DIAG_SIM_FREEZE_SHTIME=1")
    gswap = genv(ggate, "SR_NATIVE_GATE=0,SR_NATIVE_SWAP=1,SR_NATIVE_SWAP_BLOCK=1")
    gpair = genv(gswap, "SR_NATIVE_TRANSPORT=1")
    gbr = genv(gs_pre, "SR_NATIVE_BRACKET=1")
    grs = genv(gs_pre, "SR_NATIVE_IDLE_WAIT=1,SR_NATIVE_INVAL_RANGE=1,SR_NATIVE_LOCK_COHER=1")
    gcb = genv(gs_pre, "SR_NATIVE_CALLBACK=1")
    rs_ab = "SR_AB_TOGGLE=SR_NATIVE_IDLE_WAIT+SR_NATIVE_INVAL_RANGE+SR_NATIVE_LOCK_COHER,SR_AB_PRESENT=1"
    # devcreate_close: 8 boot pairs b1 ON/OFF, b2 OFF/ON, ... (boot_equiv.py's DESIGN) and 4 meter pairs m1 OFF/ON,
    # m2 ON/OFF, ...; both with devcreate_ctl's / dc3's arms (the boot rolls without dumps, the meter rolls keep the
    # sparse dump the world rule counts - gates_for / feeds_meter)
    # Since the devcreate bake (native/devcreate-fix, 2026-10-15) the OFF arm pins SR_NATIVE_DEVCREATE=0 (gs_dc_off): the
    # arms these plans ran, on an exe whose default is now ON
    gs_dc_off = gs + "," + DEVCREATE_OFF
    dcl_boot, dcl_boot_pairs = alternating_pairs(BOOT_EQUIV_PAIRS, gs + ",SR_NATIVE_DEVCREATE=1", gs_dc_off, "b", on_first=True)
    dcl_meter, dcl_meter_pairs = alternating_pairs(4, gs + ",SR_NATIVE_DEVCREATE=1", gs_dc_off, "m", on_first=False)
    dcl_mpairs = [(dict(pr)["off"], dict(pr)["on"]) for pr in dcl_meter_pairs]  # (base, target) per pair
    # the stall block (Astra round 1, P1-1): 17 more ON boots, the boot pairs' ON arm, so the run holds 8 + 4 + 17 = 29
    dcl_stall = [Step("roll", "s%d_on" % (k + 1), gs + ",SR_NATIVE_DEVCREATE=1", "plain") for k in range(BOOT_STALL_EXTRA)]
    dcl_on_tags = ([t for pr in dcl_boot_pairs for a, t in pr if a == "on"] +
                   [t for pr in dcl_meter_pairs for a, t in pr if a == "on"] + [s.tag for s in dcl_stall])
    # boot.device_create BAKED (native/devcreate-fix, 2026-10-15; NATIVE_ROADMAP "boot.device_create BAKE PLAN"): four
    # alternating pairs of the default (ON) against =0 - p1 ON/OFF, p2 OFF/ON, p3 ON/OFF, p4 OFF/ON (each arm first
    # twice) - that serve BOTH the pooled meter (the sparse dump kept: feeds_meter) and the paired boot design dcf-v1;
    # the devcreate witness in every ON roll (the ring init line, the ring at 0 packets), the ME_INIT witness in every
    # OFF roll, the P2-B tripwires in all
    dcf_pairs_steps, dcf_pairs = alternating_pairs(BOOT_EQUIV_BAKE_PAIRS, "", DEVCREATE_OFF, "p", on_first=True)
    for s in dcf_pairs_steps:
        s.ledger = ([devcreate_item("dcf_%s_dc" % s.tag), ring0_item("dcf_%s_ring0" % s.tag)] if s.tag.endswith("_on")
                    else [me_init_item("dcf_%s_me_init" % s.tag)]) + [segcounters_item("dcf_%s_seg" % s.tag)]
    dcf_mpairs = [(dict(pr)["off"], dict(pr)["on"]) for pr in dcf_pairs]  # (base, target) per pair
    # the stall block (Astra round 1 on the branch, P2: 7 ON boots let a 30 s stall on 10% of boots through 47.8% of the
    # time): 20 more default boots, the pair rolls' configuration without the sparse dump, each with the strict witness,
    # so the proof holds 7 + 20 + the gate's 2 = 29 ON boots (0.9^29 = 4.7%)
    dcf_stall = [Step("roll", "s%d_on" % (k + 1), "", "plain",
                      [devcreate_item("dcf_s%d_on_dc" % (k + 1)), ring0_item("dcf_s%d_on_ring0" % (k + 1)),
                       segcounters_item("dcf_s%d_on_seg" % (k + 1))]) for k in range(BOOT_STALL_BAKE_EXTRA)]
    # every ON boot of the proof in plan order: the gate's two boots are "gate/<roll>" (Runner.boot_path; the evaluator
    # enumerates them from the gate's row - proof_rows.gate_boots - and refuses a pool that omits one)
    dcf_on_tags = (["warmup"] + [t for pr in dcf_pairs for a, t in pr if a == "on"] + ["cppm4", "census1"] +
                   [s.tag for s in dcf_stall] + list(GATE_BOOTS))
    dcf_census = "SR_NATIVE_BRACKETS=1," + GRACEFUL  # the producer census, the run ended by the graceful quit
    dcf_steps = (
        [Step("replay", "replay", ledger=[replay_item("dcf_replay")]),
         # the declared warm-up: the default build (ON), the invocation's first launch - never graded by the mean
         # criterion (boot_equiv --warmup), counted by the stall criterion, its witness items evidence; the pair rolls'
         # exact configuration (the sparse dump they keep for the meter's world rule: sparse=True)
         Step("roll", "warmup", "", "plain",
              [devcreate_item("dcf_warmup_dc"), ring0_item("dcf_warmup_ring0"), segcounters_item("dcf_warmup_seg")],
              sparse=True)] +
        dcf_pairs_steps +
        [Step("bootequiv", "boot_equiv", pairs=dcf_pairs, warmup="warmup", always=True, design=BOOT_EQUIV_BAKE_DESIGN,
              ledger=[boot_equiv_item("dcf_boot_equiv", "BOOT_EQUIV design %s over the warm-up (the default build) and "
                                      "%d alternating default / SR_NATIVE_DEVCREATE=0 pairs (staged, one exe); the "
                                      "mean criterion of boot_order_gap on the bake exe" % (
                                          BOOT_EQUIV_BAKE_DESIGN, BOOT_EQUIV_BAKE_PAIRS),
                                      design=BOOT_EQUIV_BAKE_DESIGN, pairs=BOOT_EQUIV_BAKE_PAIRS)]),
         Step("meter", "meter_pool", base=dcf_mpairs[0][0], target=dcf_mpairs[0][1], pool=dcf_mpairs[1:], always=True,
              ledger=[pooled_meter_item(
                  "dcf_meter_pool", -5.7, 8000,
                  "820F0048's per-present re-emit (4 bin + 2 t0 = 6.0 per ingame frame, the ONLY gameplay PM4 left on the "
                  "transport-baked default path: tbk1 meter_on 6.0 exactly, t0 2.0 bin 4.0) against 0: the mean of the "
                  "per-pair deltas of four alternating pairs (p1 ON/OFF, p2 OFF/ON, p3 ON/OFF, p4 OFF/ON; the default vs "
                  "SR_NATIVE_DEVCREATE=0) <= -5.7 (6.0 less 5%, the transport bake's convention), AND every default roll "
                  "<= 0.1 per ingame frame (the zero form, kind_zero's target bound), AND every =0 roll >= 5.5 (the base "
                  "shows the re-emit); pre-registered 2026-10-15 (docs/NATIVE_ROADMAP.md BAKE PLAN; the old -3 total "
                  "bound measured a 373-packet path whose other kinds moved 2-6 a frame between identical rolls - with "
                  "nothing else left the total IS the producer's own count)",
                  provenance={"arm_key": "SR_NATIVE_DEVCREATE", "arm_on": "1", "arm_unset": "1",
                              "engaged": DEVCREATE_ENGAGED, "first": "target", "alternate": True,
                              "ignore": ["SR_PERF_DIR", "SR_CONTENT_DIR", "SR_PRESENT_DUMP_DIR"]},
                  target_max=0.1, base_min=5.5)]),
         # the dry CHECK of the two emitter ports (kinds 8 / 9) on the default path against the guest bytes: no BAD
         # bracket of any kind AND at least one completed comparison of EACH emitter (chk_kind_item: Astra round 1 P2 -
         # the aggregate alone passes a build whose kind-8 / kind-9 CHECK calls were removed)
         Step("roll", "chk", "SR_NATIVE_FLUSH_CHECK=1", "plain",
              [chk_item("dcf_chk"),
               chk_kind_item("dcf_chk_k8", 8, "ResetState 8210E520 compared at least once (the device-create reset) and never bad"),
               chk_kind_item("dcf_chk_k9", 9, "the segment re-emit 820F0048 compared at least once (Swap's per-present call) "
                                              "and never bad")]),
         # the per-frame half (820F0048 at Swap): the frozen-pass per-present A/B of the default against =0
         Step("roll", "abp", "SR_AB_TOGGLE=SR_NATIVE_DEVCREATE,SR_AB_PRESENT=1", "freeze",
              [freeze_ab("dcf_abp"), segcounters_item("dcf_abp_seg")]),
         # CPPM4 on the default: the records rendered back to PM4 by the CP (tcmp3's cppm4 ran this with DEVCREATE=1)
         Step("roll", "cppm4", CPPM4, "freeze",
              [freeze_exact("dcf_cppm4"), segcounters_item("dcf_cppm4_seg"), devcreate_item("dcf_cppm4_dc")]),
         # the producer census pair: census0 the =0 arm shows the re-emit and the boot writes as guest PM4 (the devcreate
         # plan's census0 items), census1 the default holds none of them on any producer line or completed frame (its
         # census1 items), both ended by the graceful quit so the terminal report covers the tail (tbk2's lesson)
         Step("roll", "census0", genv(DEVCREATE_OFF, dcf_census), "plain",
              [producer_all_keys_item("dcf_census0_swap", "82112050", ["r0F", "63"], note=(
                  "over the census-covered interval: 820F0048's per-frame call from Swap - RB_BC_CONTROL / "
                  "SET_BIN_SELECT_HI as guest PM4 on Swap's producer line (820F0048 is no table scope)")),
               producer_all_keys_item("dcf_census0_segbegin", "00000000", ["r0F", "63"], name="UNSTAMPED", note=(
                   "over the census-covered interval: 820F0048's boot call (82106010, inside the device-less "
                   "D3DDeviceCreate bracket) - RB_BC_CONTROL / SET_BIN_SELECT_HI as guest PM4, unstamped")),
               producer_all_keys_item("dcf_census0_reset", "00000000", ["r0E", "r0C"], name="UNSTAMPED", note=(
                   "over the census-covered interval: 8210E520's VC_FIFO_DEPTHS / PA_CL_ENHANCE as guest PM4 (boot, "
                   "unstamped)")),
               me_init_item("dcf_census0_me_init"), segcounters_item("dcf_census0_seg"),
               census_armed_item("dcf_census0_armed"), census_ends_item("dcf_census0_ends", records=True)]),
         Step("roll", "census1", dcf_census, "plain",
              [producer_census_item("dcf_census1_op63", [ANY_PRODUCER], ["63"], False,
                                    "over the census-covered interval: 820F0048's SET_BIN_SELECT_HI on no producer line "
                                    "- its boot and per-frame calls both gone"),
               producer_census_item("dcf_census1_reset", [ANY_PRODUCER], ["r0E", "r0C"], False,
                                    "over the census-covered interval: 8210E520's VC_FIFO_DEPTHS / PA_CL_ENHANCE on no "
                                    "producer line"),
               frames_forbid_item("dcf_census1_frames_op63", ops=["63"], note=(
                   "every completed frame, the tail included: no SET_BIN_SELECT_HI - 820F0048 its only writer")),
               frames_forbid_item("dcf_census1_frames_reset", regs=["0E", "0C"], note=(
                   "every completed frame, the tail included: no VC_FIFO_DEPTHS / PA_CL_ENHANCE page - 8210E520 their "
                   "only writer")),
               devcreate_item("dcf_census1_dc"), ring0_item("dcf_census1_ring0"), segcounters_item("dcf_census1_seg"),
               census_armed_item("dcf_census1_armed"), census_ends_item("dcf_census1_ends", records=True)]),
         Step("tcensus", "census", mode="pm4zero_strict", base="census0", target="census1",
              ledger=[pm4_zero_strict_census_item("dcf_census", "the default path's gameplay PM4 with NO allowance: "
                                                                "none (DEVCREATE baked onto the transport bake)")])] +
        # the stall block: s1_on..s20_on, the default build, the strict witness in each
        dcf_stall +
        # the world-rule flicker gate on the default exe, the devcreate witness in both of its rolls (both are ON boots of
        # the stall criterion too: gate/freeze, gate/span)
        [Step("gate", "gate", "", ledger=[gate_item("dcf_gate", "the bake, no native env; the devcreate witness in both rolls"),
                                           dict(devcreate_item("dcf_gate_dc"), roll="freeze"),
                                           dict(ring0_item("dcf_gate_ring0"), roll="freeze"),
                                           dict(devcreate_item("dcf_gate_span_dc"), roll="span"),
                                           dict(ring0_item("dcf_gate_span_ring0"), roll="span")]),
         Step("bootstall", "boot_stall", pool=dcf_on_tags, always=True, design=BOOT_STALL_BAKE_DESIGN,
              allow_keys=BOOT_STALL_INSTRUMENTS,
              ledger=[boot_stall_item("dcf_boot_stall", "BOOT_STALL design %s over EVERY ON boot of the proof (the "
                                      "warm-up, the four pairs' default rolls, cppm4, census1, the stall block "
                                      "s1_on..s%d_on, the gate's freeze and span boots: %d, plus any retained attempt; "
                                      "the A/B roll and the CHECK roll are neither arm at boot; the events F / A / W / R "
                                      "/ C (the WP1 wait > 1.0 s) / H (the HUD after 95 s); staged on one exe path, one "
                                      "exe)" % (BOOT_STALL_BAKE_DESIGN, BOOT_STALL_BAKE_EXTRA, BOOT_STALL_BAKE_N_MIN),
                                      design=BOOT_STALL_BAKE_DESIGN, n_min=BOOT_STALL_BAKE_N_MIN)])])
    # every item of the plan but the two the unknown's resolvers take (resolver_bind) resolves default_bake
    dcf_resolves = [it["id"] for s in dcf_steps for it in s.ledger if it["id"] not in ("dcf_boot_equiv", "dcf_boot_stall")]

    # p2.zpd BAKED (native/zpd-bake, 2026-10-16; NATIVE_ROADMAP "p2.zpd BAKE PLAN"): every roll from the chapter-12 seed
    # playing the zpd tape (Metropolis: the HUD glow queries fire; the precondition [GPU_CP] zpd > 0, a roll without one
    # VOID, re-run once, then NO DATA). The default arms set no SR_NATIVE_ZPD, the =0 arms ZPD_OFF.
    zg = dict(play=ZPD_TAPE, dwell=260, seconds=900, from_save=ZPD_SEED, require=ZPD_FIRED)
    zpace = dict(play=ZPD_TAPE, dwell=1200, seconds=2100, from_save=ZPD_SEED, require=ZPD_FIRED)
    zcensus = "SR_NATIVE_BRACKETS=1," + GRACEFUL  # the producer census, the run ended by the graceful quit
    zwit = lambda t, ab=False: [zpd_default_item("zbk_%s_default" % t), zpd_bake_item("zbk_%s_zpd" % t, ab=ab)]
    zbk_steps = [
        # the nine traces against the pre-bake exe (--base): no trace holds a query, so the bake moves nothing there
        Step("replay", "replay", ledger=[replay_item("zbk_replay")]),
        # the flicker gate on the default exe, save-started on the zpd tape (both rolls must issue queries), the strict
        # witnesses (the default form, the latency census with the port, the drained transport tile) in both rolls
        Step("gate", "gate", "SR_NATIVE_SEG_TILE=1",
             ledger=[dict(gate_item("zbk_gate", "the bake, no native env but the strict witness; save-started, queries "
                                                "firing in both rolls"), require=ZPD_FIRED)] +
                    [dict(it, roll="freeze") for it in zwit("gate")] +
                    [dict(it, roll="span") for it in zwit("gate_span")] +
                    [dict(tile_drained_item("zbk_gate_tile"), roll="freeze"),
                     dict(tile_drained_item("zbk_gate_span_tile"), roll="span"),
                     dict(xport_item("zbk_gate_xport"), roll="freeze")],
             from_save=ZPD_SEED, play=ZPD_TAPE, require=ZPD_FIRED),
        # the dry CHECK of the port (kind 11) against the guest bytes in Metropolis: no BAD bracket of any kind AND at
        # least one completed comparison of 82101E08 (the transport is off under CHECK: the port declines, the guest body
        # runs bracketed; the answers stay native)
        Step("roll", "chk", "SR_NATIVE_FLUSH_CHECK=1", "plain",
             [chk_item("zbk_chk"), chk_kind_item("zbk_chk_k11", 11, "82101E08 (the occlusion query's packets) compared "
                                                                     "at least once and never bad"),
              zpd_default_item("zbk_chk_default")], **zg),
        # THE PROBE: the default path with the producer census, its =0 control census0; graded together by
        # transport_census.py --pm4-zero --base-shows zpd (the census step)
        Step("roll", "census0", genv(ZPD_OFF, zcensus), "plain",
             [zpd_off_item("zbk_census0_off"), census_armed_item("zbk_census0_armed"),
              census_ends_item("zbk_census0_ends", records=True)], **zg),
        Step("roll", "probe", zcensus, "plain",
             zwit("probe") + [xport_item("zbk_probe_xport"), segcounters_item("zbk_probe_seg"),
                              census_armed_item("zbk_probe_armed"), census_ends_item("zbk_probe_ends", records=True)],
             **zg),
        Step("tcensus", "census", mode="pm4zero_zpd", base="census0", target="probe",
             ledger=[pm4_zero_zpd_census_item("zbk_census", "the default path's gameplay PM4 in Metropolis with NO "
                                                            "allowance: none (the HUD glow queries as records)")]),
        # the frozen-pass per-present A/B of the default against =0's PRODUCER: SR_AB_TOGGLE=SR_NATIVE_ZPD toggles the
        # port of 82101E08 per present (odd presents its guest body - the =0 producer - even the records); the answering
        # stays native in both arms (the mode is latched: the slot words GetData reads are guest-visible, so an answer
        # A/B is no per-present feature - kernel/ab_toggle.hpp's rule; the =0 answering is the census / meter / pacing
        # arms' A/B)
        # (zbk2: the frozen rolls' holds every 4 s from ~HUD+40 s, ZPD_FREEZE; zbk3: 80 dumps a hold, ZPD_DUMP_CAP; the
        # compared presents graded for queries by the zpdwin step after the roll, each by its own frame stamp)
        Step("roll", "abp", ",".join(("SR_AB_TOGGLE=SR_NATIVE_ZPD,SR_AB_PRESENT=1,SR_NATIVE_SEG_TILE=1", ZPD_FREEZE,
                                      ZPD_DUMP_CAP)), "freeze",
             [dict(freeze_ab("zbk_abp"), require=ZPD_FIRED)] + zwit("abp", ab=True) +
             [tile_drained_item("zbk_abp_tile"), xport_item("zbk_abp_xport"), segcounters_item("zbk_abp_seg")], **zg),
        Step("zpdwin", "abp_zpd", mode="freeze_ab", target="abp", ledger=[zpd_window_item(
            "zbk_abp_window", "freeze_ab", "the A/B's compared presents held queries: the pixel grade's static pairs, "
            "each present by its own frame stamp (Astra round 2); >= 1,000 query-bearing, each arm >= 250")]),
        # CPPM4 on the default: the records rendered back to PM4 by the CP (kRkZpd -> EVENT_WRITE_ZPD -> the same
        # applyZpdEvent), the frozen presents exact
        Step("roll", "cppm4", CPPM4 + ",SR_NATIVE_SEG_TILE=1," + ZPD_FREEZE + "," + ZPD_DUMP_CAP, "freeze",
             [dict(freeze_exact("zbk_cppm4"), require=ZPD_FIRED)] + zwit("cppm4") +
             [tile_drained_item("zbk_cppm4_tile"), xport_item("zbk_cppm4_xport"), segcounters_item("zbk_cppm4_seg")],
             **zg),
        Step("zpdwin", "cppm4_zpd", mode="freeze", target="cppm4", ledger=[zpd_window_item(
            "zbk_cppm4_window", "freeze", "CPPM4's compared presents held queries: the pixel grade's compared pairs, "
            "each present by its own frame stamp (Astra round 2); >= 1,000 query-bearing")]),
        # the meter =0 vs the default on the seed
        Step("roll", "meter_off", ZPD_OFF, "plain", [zpd_off_item("zbk_meter_off_off")], **zg),
        Step("roll", "meter_on", "", "plain", zwit("meter_on"), **zg),
        Step("meter", "meter", base="meter_off", target="meter_on", ledger=[zpd_meter_item(
            "zbk_meter", "meter_off", "the baked default against SR_NATIVE_ZPD=0 on the same exe, Metropolis: "
            "PRE-REGISTERED the default <= 0.05 PM4 per ingame frame and delta <= 0")]),
        # pacing on the console path (SR_HIGHFPS=0), uninstrumented, the P2 trace's kind-29 records: =0, the default, a
        # second identical =0 as the calibration; long save-started rolls (the queries keep firing while markers are in
        # view) for the 28,672-present window. The latency model never makes the CP wait, but the render loop's per-query
        # work (the host query and its resolve, the two probe draws, the readback) and the HUD's own cadence (answers
        # arrive: one query per marker per two frames, against the =0 HUD's single unanswered one) can move pacing.
        Step("roll", "pace0", genv(ZPD_OFF, "SR_HIGHFPS=0"), "plain", [zpd_off_item("zbk_pace0_off")], trace=True,
             **zpace),
        Step("roll", "pace1", "SR_HIGHFPS=0", "plain", zwit("pace1"), trace=True, **zpace),
        Step("roll", "pace0b", genv(ZPD_OFF, "SR_HIGHFPS=0"), "plain", [zpd_off_item("zbk_pace0b_off")], trace=True,
             **zpace),
        Step("p2trace", "pace0_trace", ledger=[pacing_trace_item("zbk_pace0_trace")], base="pace0", window=PACE_WINDOW),
        Step("p2trace", "pace1_trace", ledger=[pacing_trace_item("zbk_pace1_trace")], base="pace1", window=PACE_WINDOW),
        Step("p2trace", "pace0b_trace", ledger=[pacing_trace_item("zbk_pace0b_trace")], base="pace0b", window=PACE_WINDOW),
        Step("pacing", "pacing", base="pace0_trace", target="pace1_trace", calib="pace0b_trace",
             ledger=[composed_pacing_item("zbk_pacing")]),
        # the default's queries ran INSIDE the window the pacing comparison graded, every quarter of it
        Step("zpdwin", "pace1_zpd", mode="pacing", target="pace1", base="pace1_trace", ledger=[zpd_window_item(
            "zbk_pace1_window", "pacing", "the default's queries inside the 28,672-present window p2_trace graded: "
            "issued, measured and ported, each quarter active")]),
    ]
    zbk_pace_tags = ("pace0", "pace1", "pace0b", "pace0_trace", "pace1_trace", "pace0b_trace", "pacing", "pace1_zpd")
    zbk_bake_ids = [it["id"] for s in zbk_steps if s.tag not in zbk_pace_tags for it in s.ledger]
    zbk_pace_ids = [it["id"] for s in zbk_steps if s.tag in zbk_pace_tags for it in s.ledger]
    # THE PARKED WORKLOAD's information set (zbk2's follow-up, 2026-10-16; ZPD_PARK_SEED / ZPD_PARK_TAPE): zbk2's three
    # failed or NO DATA witnesses, each over a roll configured exactly as zbk2's (the same gates, mode, dwell and
    # precondition), from the parked seed: pace1 (the console-path default, its P2 trace, the 28,672-present window and
    # zpd_activity's pacing grade), abp (the frozen-pass per-present A/B and zpd_activity's freeze_ab grade) and meter_off
    # (the =0 arm, whose precondition [GPU_CP] zpd > 0 zbk2 never met). No obligation: information, nothing applied.
    zpkwit = lambda t, ab=False: [zpd_default_item("zpk_%s_default" % t), zpd_bake_item("zpk_%s_zpd" % t, ab=ab)]
    zpark = dict(play=ZPD_PARK_TAPE, dwell=260, seconds=900, from_save=ZPD_PARK_SEED, require=ZPD_FIRED)
    zpark_long = dict(zpark, dwell=1200, seconds=2100)
    zpk_steps = [
        Step("roll", "park_pace1", "SR_HIGHFPS=0", "plain", zpkwit("pace1"), trace=True, **zpark_long),
        Step("p2trace", "park_pace1_trace", ledger=[pacing_trace_item("zpk_pace1_trace")], base="park_pace1",
             window=PACE_WINDOW),
        Step("zpdwin", "park_pace1_zpd", mode="pacing", target="park_pace1", base="park_pace1_trace",
             ledger=[zpd_window_item("zpk_pace1_window", "pacing", "pace1_zpd's grade on the parked roll")]),
        Step("roll", "park_abp", "SR_AB_TOGGLE=SR_NATIVE_ZPD,SR_AB_PRESENT=1,SR_NATIVE_SEG_TILE=1," + ZPD_FREEZE, "freeze",
             [dict(freeze_ab("zpk_abp"), require=ZPD_FIRED)] + zpkwit("abp", ab=True), **zpark),
        Step("zpdwin", "park_abp_zpd", mode="freeze_ab", target="park_abp",
             ledger=[zpd_window_item("zpk_abp_window", "freeze_ab", "abp_zpd's grade on the parked roll")]),
        Step("roll", "park_off", ZPD_OFF, "plain", [zpd_off_item("zpk_park_off_off")], sparse=True, **zpark),
    ]
    # THE KEEP-ALIVE's information set (zpk2, zpk1's follow-up, 2026-10-16; ZPD_PARK_SEED / ZPD_KEEP_TAPE): zpk1's three
    # rolls and grades again, the seed and view unchanged, the tape adding the keep-alive tap; the A/B as zbk3's abp
    # (80 dumps a hold, ZPD_DUMP_CAP), the grades the Path B ones zbk3 runs (each compared present by its own frame
    # stamp, the arms). No obligation: information, nothing applied.
    zpk2wit = lambda t, ab=False: [zpd_default_item("zpk2_%s_default" % t), zpd_bake_item("zpk2_%s_zpd" % t, ab=ab)]
    zkeep = dict(play=ZPD_KEEP_TAPE, dwell=260, seconds=900, from_save=ZPD_PARK_SEED, require=ZPD_FIRED)
    zkeep_long = dict(zkeep, dwell=1200, seconds=2100)
    zpk2_steps = [
        Step("roll", "keep_pace1", "SR_HIGHFPS=0", "plain", zpk2wit("pace1"), trace=True, **zkeep_long),
        Step("p2trace", "keep_pace1_trace", ledger=[pacing_trace_item("zpk2_pace1_trace")], base="keep_pace1",
             window=PACE_WINDOW),
        Step("zpdwin", "keep_pace1_zpd", mode="pacing", target="keep_pace1", base="keep_pace1_trace",
             ledger=[zpd_window_item("zpk2_pace1_window", "pacing", "pace1_zpd's grade on the keep-alive roll")]),
        Step("roll", "keep_abp", ",".join(("SR_AB_TOGGLE=SR_NATIVE_ZPD,SR_AB_PRESENT=1,SR_NATIVE_SEG_TILE=1", ZPD_FREEZE,
                                           ZPD_DUMP_CAP)), "freeze",
             [dict(freeze_ab("zpk2_abp"), require=ZPD_FIRED)] + zpk2wit("abp", ab=True), **zkeep),
        Step("zpdwin", "keep_abp_zpd", mode="freeze_ab", target="keep_abp",
             ledger=[zpd_window_item("zpk2_abp_window", "freeze_ab", "abp_zpd's grade on the keep-alive roll")]),
        Step("roll", "keep_off", ZPD_OFF, "plain", [zpd_off_item("zpk2_keep_off_off")], sparse=True, **zkeep),
    ]

    # boot gaps BAKED (native/boot-gaps, 2026-10-16; docs/NATIVE_ROADMAP.md "BOOT GAPS BUILD NOTE", pre-registered before
    # any roll): SR_NATIVE_MODE_STATE and SR_NATIVE_DISPLAY default ON on top of the devcreate bake, so the default build
    # writes no literal PM4 in any era from boot (release/emu-stage1's precondition: there a literal-PM4 gap is fatal).
    # Both producers run once, at boot - frame 1 and the first Swap - so the per-present A/B grades only that the gates
    # leave every later present alone; the boot itself is graded by the census pair (=0 vs the default, the producer
    # census, the graceful quit), era_census.py over each (the =0 arm FAILs in boot alone, by dcf2's 529 packets / 2
    # gaps; the default PASSes in every era with the presenter's ramp equal to the =0 arm's), the meter pair on
    # display.registers' baked default (the run-cumulative dispreg 1 -> 0, SWAP's declines 1 -> 0), the dry CHECK of
    # the mode-state port (kind 10), CPPM4 and the gate - the strict witness (bootgaps_witness) in every default roll.
    # The plan follows the display-registers bake (dev ddfc6be0, merged): "vblank_default" - no roll is pinned to
    # SR_NATIVE_VBLANK=0 (run_gates), every arm runs the host vblank service by its default, the meter rolls check that
    # default form (vblank_default_item), and SR_NATIVE_VBLANK is one of the keys the parent environment may not carry.
    bgb_census = "SR_NATIVE_BRACKETS=1," + GRACEFUL
    bgb_swap_keys = ["45", "21", "3C", "r18", "r19"]  # frame 2's: COND_WRITE, REG_RMW, WAIT_REG_MEM, the 0x18xx / 0x19xx
    # Each item names the obligation it goes to (Runner.item_oids): the mode state's boot.device_create (frame 1's pair,
    # its CHECK), the persisted display's p2.transport (the first Swap; the witnesses, the A/B, CPPM4, the gate), the
    # meter pair's display.registers (contradiction meter_run_swap_decl's criterion), the census zero's acceptance.zero.
    bt, bd, br, bz = BOOT_GAPS_OBLIGATIONS

    def own(oid, items):
        return [dict(i, obligation=oid) for i in items]
    bgb_steps = [
        Step("replay", "replay", ledger=own(bt, [replay_item("bgb_replay")])),
        Step("roll", "census0", genv(BOOT_GAPS_OFF, bgb_census), "plain",
             own(bd, [producer_all_keys_item("bgb_census0_mode", "00000000", ["r05", "r0F"], name="UNSTAMPED", note=(
                 "over the census-covered interval: GpuRingEmitModeStateReg 82103238's WAIT_UNTIL / RB_BC_CONTROL as "
                 "guest PM4 inside the device-less D3DDeviceCreate bracket (unstamped; dcf2 census1's frame 1)"))]) +
             own(bt, [producer_all_keys_item("bgb_census0_swap", "82112050", bgb_swap_keys, note=(
                 "over the census-covered interval: the first Swap's persisted display as guest PM4 on Swap's line - "
                 "82105DD0's table ramp (0x1925 + COND_WRITE x 256), the scaler block (WAIT_REG_MEM, 0x18xx / 0x19xx), "
                 "the scaler reset, REG_RMW 0x1E4E (dcf2 census1's frame 2)")),
                      bootgaps_display_item("bgb_census0_disp", "off"),
                      bootgaps_swap_item("bgb_census0_decl", 1), bootgaps_why_item("bgb_census0_why", 1),
                      bootgaps_gaps_item("bgb_census0_gaps", BOOT_GAPS_BOOT_GAPS),
                      bootgaps_run_item("bgb_census0_run", BOOT_GAPS_BOOT_PK),
                      segcounters_item("bgb_census0_seg"), census_armed_item("bgb_census0_armed"),
                      census_ends_item("bgb_census0_ends", records=True)])),
        Step("roll", "census1", bgb_census, "plain",
             own(bt, bootgaps_witness("bgb", "census1") +
                 [segcounters_item("bgb_census1_seg"), census_armed_item("bgb_census1_armed"),
                  census_ends_item("bgb_census1_ends", records=True)]) +
             own(bz, census_zero_items("bgb", "census1"))),
        Step("era", "era0", base="census0", mode="control", era="newgame",
             ledger=own(bz, [era_item("bgb_era0", control=True, note=(
                 "the =0 arm: era_census FAILs in boot alone - frames 1-2's %d packets as %d literal gap parts - every "
                 "other era 0, the census whole" % (BOOT_GAPS_BOOT_PK, BOOT_GAPS_BOOT_GAPS)))])),
        Step("era", "era1", base="census1", target="census0", mode="pass", era="newgame",
             ledger=own(bz, [era_item("bgb_era1", note=(
                 "the default: pm4 = 0 and gaps = 0 in every era FROM BOOT (boot, intro, title, fe_menu, opening_fmv, "
                 "ingame), no ring packet / kind-1 / BS item, the presenter's display ramp equal to the =0 arm's"))])),
        Step("roll", "meter0", BOOT_GAPS_OFF, "plain",
             own(br, [bootgaps_vblank_item("bgb_meter0_vblank", 1), vblank_default_item("bgb_meter0_default"),
                      bootgaps_dispreg_item("bgb_meter0_dispreg", 1), bootgaps_swap_item("bgb_meter0_decl", 1),
                      bootgaps_why_item("bgb_meter0_why", 1)])),
        Step("roll", "meter1", "", "plain",
             own(br, [bootgaps_vblank_item("bgb_meter1_vblank", 0), vblank_default_item("bgb_meter1_default"),
                      bootgaps_dispreg_item("bgb_meter1_dispreg", 0)]) +
             own(bt, bootgaps_witness("bgb", "meter1"))),
        Step("roll", "chk", "SR_NATIVE_FLUSH_CHECK=1", "plain",
             own(bd, [chk_item("bgb_chk"),
                      chk_kind_item("bgb_chk_k10", 10, "GpuRingEmitModeStateReg 82103238 compared at least once (the "
                                                       "device create's 821058B0 call) and never bad"),
                      bootgaps_display_item("bgb_chk_disp", "check")])),
        # the A/B's witness is what holds in either arm at boot (bootgaps_display_item "ab"): its boot is the seeded
        # mask's, the first VdSwap flipping it inside the first Swap
        Step("roll", "abp", "SR_AB_TOGGLE=SR_NATIVE_DISPLAY+SR_NATIVE_MODE_STATE,SR_AB_PRESENT=1", "freeze",
             own(bt, [freeze_ab("bgb_abp", "the gates act at boot only: every later present the same under both arms"),
                      segcounters_item("bgb_abp_seg"), bootgaps_display_item("bgb_abp_disp", "ab"),
                      bootgaps_why_item("bgb_abp_why", None)])),
        Step("roll", "cppm4", CPPM4, "freeze",
             own(bt, [freeze_exact("bgb_cppm4"), segcounters_item("bgb_cppm4_seg")] +
                 bootgaps_witness("bgb", "cppm4", "cppm4"))),
        Step("gate", "gate", "", ledger=own(bt, [gate_item("bgb_gate", "the bake, no native env; the strict witness in "
                                                                       "both rolls")] +
                                            [dict(it, roll="freeze") for it in bootgaps_witness("bgb", "gate")] +
                                            [dict(it, roll="span") for it in bootgaps_witness("bgb", "gate_span")])),
    ]
    # the two new unknowns the ledger pre-registers (archive/bgaps/ledger_delta_draft.md): each resolved by every item
    # routed to its obligation and, for mode_state_bake, the DEFAULT path's evidence of the same run as well
    # (BOOT_GAPS_MODE_DEFAULT: Astra round 1 on native/boot-gaps, P2 - its own items are census0's, which runs the mode
    # state OFF, and the CHECK's, which records nothing, so they alone could close it while the default path failed);
    # display.registers' and acceptance.zero's items are evidence only (era1 also resolving mode_state_bake)
    bgb_resolves = [(oid, uid, [i["id"] for s in bgb_steps for i in s.ledger if i["obligation"] == oid] + list(extra))
                    for oid, uid, extra in ((bt, "boot_gaps_bake", ()), (bd, "mode_state_bake", BOOT_GAPS_MODE_DEFAULT))]

    gfs = gs + ",SR_NATIVE_FLUSH_SPLIT=1"
    # p2.transport COMPOSED (the orchestrator's decisions of 2026-10-14): every transport kind + devcreate, construns OFF
    gcmp_off = ("SR_NATIVE_FLUSH=1,SR_NATIVE_FLUSH_CHECK=0,SR_NATIVE_SEGMENTLESS=1,SR_NATIVE_SHADER_CONST_RUNS=0,"
                "SR_NATIVE_REPLAY_CALLBACK=0,SR_NATIVE_FLUSH_PM4=0,SR_NATIVE_FLUSH_CPPM4=0,SR_HDR_ADAPT_DT=0,"
                "SR_AUDIO_OUT=0,SR_WINDOW_BACKGROUND=1,SR_HARNESS_INPUT=1,SR_HARNESS_REAL_INPUT=0,SR_ALLOW_FORCING=0,"
                "SR_DIAG_SIM_FREEZE_SHTIME=1")
    kinds = ("SR_NATIVE_CALLBACK", "SR_NATIVE_GATE", "SR_NATIVE_SWAP_BLOCK", "SR_NATIVE_SWAP", "SR_NATIVE_BRACKET",
             "SR_NATIVE_IDLE_WAIT", "SR_NATIVE_INVAL_RANGE", "SR_NATIVE_LOCK_COHER", "SR_NATIVE_TRANSPORT",
             "SR_NATIVE_DEVCREATE")
    goff = gcmp_off + "," + ",".join(k + "=0" for k in kinds)   # every transport kind OFF (segmentless stays: baked)
    gcmp = gcmp_off + "," + ",".join(k + "=1" for k in kinds)   # the composed env
    # the composed per-present A/B: every kind but SWAP_BLOCK (as the swap_sdk plan: the reserved block in both arms)
    cmp_ab = "SR_AB_TOGGLE=" + "+".join(k for k in kinds if k != "SR_NATIVE_SWAP_BLOCK") + ",SR_AB_PRESENT=1"
    # display.registers: the composed env above, gate OFF (gdc = GATE (a), the pacing control) and ON (gdv = (b) with (c))
    gdc = gcmp + ",SR_NATIVE_VBLANK=0"
    gdv = gcmp + ",SR_NATIVE_VBLANK=1"
    # display.registers BAKED (native/dreg-bake, 2026-10-15): the default arms set nothing (the baked default ON), the =0
    # arms VBLANK_OFF; the pacing arms on the console path
    dbk_pace0 = genv(VBLANK_OFF, "SR_HIGHFPS=0")
    dbk_steps = [
        Step("replay", "replay", ledger=[replay_item("dbk_replay")]),
        # the flicker gate on the default exe: the vblank witness (its default form), the CPU-2 lock and the drained tile
        # witness in both of its rolls; the reclaim audit (xport) in the freeze roll
        Step("gate", "gate", "SR_NATIVE_SEG_TILE=1",
             ledger=[gate_item("dbk_gate", "the bake, no native env but the strict witness"),
                     dict(vblank_item("dbk_gate_vblank", default_form=True), roll="freeze"),
                     dict(vblank_default_item("dbk_gate_default"), roll="freeze"),
                     dict(cpu2_item("dbk_gate_cpu2"), roll="freeze"),
                     dict(vblank_item("dbk_gate_span_vblank", default_form=True), roll="span"),
                     dict(vblank_default_item("dbk_gate_span_default"), roll="span"),
                     dict(cpu2_item("dbk_gate_span_cpu2"), roll="span"),
                     dict(tile_drained_item("dbk_gate_tile"), roll="freeze"),
                     dict(tile_drained_item("dbk_gate_span_tile"), roll="span"),
                     dict(xport_item("dbk_gate_xport"), roll="freeze")]),
        # the traced probe: every tick native, every pacing call host, the per-vblank check, the CPU-2 lock, the GATE
        # native (its prerequisite), SWAP at most its boot decline (d), the strict transport witness, the P2 trace
        Step("roll", "probe", "SR_NATIVE_SEG_TILE=1", "plain",
             [vblank_item("dbk_probe_vblank", default_form=True), vblank_default_item("dbk_probe_default"),
              cpu2_item("dbk_probe_cpu2"), census_item("dbk_probe_census"),
              segcounters_item("dbk_probe_seg"), tile_drained_item("dbk_probe_tile"), xport_item("dbk_probe_xport"),
              composed_gate_item("dbk_probe_gate"), swap_boot_only_item("dbk_probe_swap"),
              swap_why_item("dbk_probe_swap_why")], trace=True),
        Step("p2trace", "probe_trace", ledger=[dreg_trace_item("dbk_probe_trace")], base="probe"),
        # the frozen-pass per-present A/B of the default against =0 (rule R): odd presents the guest 82111E38 + the guest
        # ISR's source 0 with their inputs kept live, even the host forms; the per-vblank check on both arms
        Step("roll", "abp", "SR_AB_TOGGLE=SR_NATIVE_VBLANK,SR_AB_PRESENT=1,SR_NATIVE_SEG_TILE=1", "freeze",
             [freeze_ab("dbk_abp"), vblank_item("dbk_abp_vblank", toggled=True, default_form=True),
              vblank_default_item("dbk_abp_default"), cpu2_item("dbk_abp_cpu2"),
              segcounters_item("dbk_abp_seg"), tile_drained_item("dbk_abp_tile"), xport_item("dbk_abp_xport")]),
        # CPPM4 on the default: the GATE record rendered back to PM4 - its callback reaches the host form through the
        # CP's INTERRUPT and the guest ISR's source 1
        Step("roll", "cppm4", CPPM4 + ",SR_NATIVE_SEG_TILE=1", "freeze",
             [freeze_exact("dbk_cppm4"), vblank_item("dbk_cppm4_vblank", default_form=True),
              vblank_default_item("dbk_cppm4_default"), cpu2_item("dbk_cppm4_cpu2"),
              segcounters_item("dbk_cppm4_seg"), tile_drained_item("dbk_cppm4_tile"), xport_item("dbk_cppm4_xport")]),
        # the meter =0 vs the default (no SR_NATIVE_SEG_TILE: the counted interval as dreg's meter pair ran it): the
        # dispreg column per ingame frame live at =0 and exactly 0 by default (the meter bound); the run-cumulative
        # criterion dispreg <= d (SWAP's declines, same run) with scanout=0 / vsyncBit=0; d at most the boot decline
        Step("roll", "meter0", VBLANK_OFF, "plain", [vblank_off_item("dbk_meter0_off")]),
        Step("roll", "meter1", "", "plain",
             [vblank_item("dbk_meter1_vblank", default_form=True), vblank_default_item("dbk_meter1_default"),
              cpu2_item("dbk_meter1_cpu2"), dispreg_swap_run_item("dbk_meter1_run"),
              swap_boot_only_item("dbk_meter1_swap"), swap_why_item("dbk_meter1_swap_why")]),
        Step("meter", "meter", base="meter0", target="meter1", ledger=[dispreg_zero_item(
            "dbk_meter", "meter0", "the dispreg column per ingame frame (native_meter.hpp's counting rule) on the default "
            "path: live at SR_NATIVE_VBLANK=0 (>= 2), exactly 0 by default - PRE-REGISTERED bound 0; the boot decline "
            "d (SWAP_DECL #1, before the ingame interval) is the run-cumulative item's, not this one's")]),
        # pacing on the console path (SR_HIGHFPS=0), uninstrumented (no SR_NATIVE_SEG_TILE), the P2 trace's kind-29
        # records: =0, the default, a second identical =0 as the calibration; the 28,672-present window
        Step("roll", "pace0", dbk_pace0, "plain", [vblank_off_item("dbk_pace0_off")], trace=True, **PACE_LONG),
        Step("roll", "pace1", "SR_HIGHFPS=0", "plain",
             [vblank_item("dbk_pace1_vblank", note="console path (line % live): early=0 mismatch=0, today's answers "
                                                   "reproduced", default_form=True),
              vblank_default_item("dbk_pace1_default"), cpu2_item("dbk_pace1_cpu2")], trace=True, **PACE_LONG),
        Step("roll", "pace0b", dbk_pace0, "plain", [vblank_off_item("dbk_pace0b_off")], trace=True, **PACE_LONG),
        Step("p2trace", "pace0_trace", ledger=[pacing_trace_item("dbk_pace0_trace")], base="pace0", window=PACE_WINDOW),
        Step("p2trace", "pace1_trace", ledger=[pacing_trace_item("dbk_pace1_trace")], base="pace1", window=PACE_WINDOW),
        Step("p2trace", "pace0b_trace", ledger=[pacing_trace_item("dbk_pace0b_trace")], base="pace0b",
             window=PACE_WINDOW),
        Step("pacing", "pacing", base="pace0_trace", target="pace1_trace", calib="pace0b_trace",
             ledger=[composed_pacing_item("dbk_pacing")]),
    ]
    # the resolvers: the pacing steps' items resolve default_pacing, every other item default_bake
    dbk_pace_tags = ("pace0", "pace1", "pace0b", "pace0_trace", "pace1_trace", "pace0b_trace", "pacing")
    dbk_bake_ids = [it["id"] for s in dbk_steps if s.tag not in dbk_pace_tags for it in s.ledger]
    dbk_pace_ids = [it["id"] for s in dbk_steps if s.tag in dbk_pace_tags for it in s.ledger]
    ps = {
        "flush_split": {
            "prefix": "frs", "obligations": ["p1.flush_residue"], "gates": gfs,
            "about": "p1.flush_residue (NATIVE_ROADMAP 'p1.flush_residue BUILD NOTE'; SR_NATIVE_FLUSH_SPLIT, default ON "
                     "since the 2026-10-14 bake - every arm names =1 or =0 -, per-present A/B): the S1 flush's sections that "
                     "do not fit before the segment limit chunked across the "
                     "SDK's own kick as records (82107058 ported) instead of the guest leaf, draws over 0xFFFF indices split "
                     "as 820FC000 / 820FBBF8 split them, row 9's record pre-pass through the kicks and GPU waits it cannot "
                     "model. The "
                     "offline proof (tools/difftest/flush_split_run.py: the SDK bodies against the native paths, five modes, "
                     "twelve compiled faults; the game's instanced path through the generated RwDrawInstancedConstBatched "
                     "8261B050 - streams, c100 instance constants, SetPredication per tile, visibility regions - with its "
                     "VS / PS binds and the TilePush / Pop region form NOT built offline) is separate; these rolls are the "
                     "live part. Every pure-ON roll's snapshot step grades the counters from boot through the last "
                     "snapshot; freshness relative to the last logged timestamp allows 1.0 s; the remaining execution "
                     "tail is unmeasured (no graceful quit) - never the whole run. The "
                     "census pair is rcen1's form (segmentless + SR_NATIVE_BRACKETS=1, gate OFF / ON), graded per "
                     "producer line and by tools/perf/transport_census.py --flush-residue-zero (the record's "
                     "observation: the flush_residue group 0 over the census-covered ingame interval)",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("flush_split_replay")]),
                # the S1 flush's dry CHECK under the gate (it skips calls whose guest body kicked: the chunks themselves are
                # proven offline)
                Step("roll", "chk", gfs + ",SR_NATIVE_FLUSH_CHECK=1", "plain", [chk_item("flush_split_chk")]),
                # row 9's dry CHECK under the gate (SR_NB_REC_CHECK=1 turns record mode off: every Resolve / ClearF / GPR
                # call is the guest body, the port run dry beside it and compared; Astra round 1)
                Step("roll", "rec_chk", gfs + ",SR_NB_REC_CHECK=1", "plain", flush_rec_check_items("flush_split")),
                # the probe: activity, every S1 section native (no guest leaf), row 9 through its kicks, the strict
                # transport witness and the P2 trace (the chunk kicks are closes: S +2 each, the close order)
                Step("roll", "probe", gfs + ",SR_NATIVE_SEG_TILE=1", "plain",
                     [census_item("flush_split_probe_census"), segcounters_item("flush_split_probe_seg"),
                      tile_drained_item("flush_split_probe_tile"), xport_item("flush_split_probe_xport"),
                      flush_split_item("flush_split_probe_split")]
                     + flush_row9_items("flush_split", "probe", strict=True), trace=True),
                Step("p2trace", "probe_trace", ledger=[trace_order_item("flush_split_probe_trace")], base="probe"),
                # the strict bar from boot through the probe's last snapshot (every pure-ON roll below has its own)
                Step("snapshot", "probe_snap", base="probe", ledger=[flush_snapshot_item("flush_split_probe_snapshot")]),
                # the frozen-pass per-present A/B: odd presents today's path (guest leaf / body), even the native chunks,
                # splits and transparent pre-pass
                Step("roll", "abp", gfs + ",SR_AB_TOGGLE=SR_NATIVE_FLUSH_SPLIT,SR_AB_PRESENT=1,SR_NATIVE_SEG_TILE=1",
                     "freeze",
                     [freeze_ab("flush_split_abp"), segcounters_item("flush_split_abp_seg"),
                      tile_item("flush_split_abp_tile"), xport_item("flush_split_abp_xport"),
                      flush_split_item("flush_split_abp_split", "the ON arm converted; the OFF arm's guest leaves count "
                                                                "in sectionFallbacks by design")]
                     + flush_row9_items("flush_split", "abp", strict=False)),
                Step("roll", "cppm4", gfs + "," + CPPM4, "freeze",
                     [freeze_exact("flush_split_cppm4"), segcounters_item("flush_split_cppm4_seg"),
                      flush_split_item("flush_split_cppm4_split")]
                     + flush_row9_items("flush_split", "cppm4", strict=True)),
                Step("snapshot", "cppm4_snap", base="cppm4", ledger=[flush_snapshot_item("flush_split_cppm4_snapshot")]),
                # STRESS lowers the kick threshold only (the chunker reads dev+0x2C as the SDK does): more kicks inside
                # row 9's entries and between split parts
                Step("roll", "stress", gfs + "," + STRESS + "," + CPPM4, "freeze",
                     [freeze_exact("flush_split_stress"), segcounters_item("flush_split_stress_seg"),
                      flush_split_item("flush_split_stress_split")]
                     + flush_row9_items("flush_split", "stress", strict=True)),
                Step("snapshot", "stress_snap", base="stress", ledger=[flush_snapshot_item("flush_split_stress_snapshot")]),
                # the producer census pair: the S1 flushes' non-construns keys and row 9's producers as PM4 with the gate
                # OFF, none with it ON
                # the OFF arms pin SR_NATIVE_FLUSH_SPLIT=0 (default ON since the 2026-10-14 bake; an older exe ignores it)
                Step("roll", "census0", gs + ",SR_NATIVE_FLUSH_SPLIT=0,SR_NATIVE_BRACKETS=1", "plain",
                     flush_residue_census_items("flush_split", "census0", True)),
                Step("roll", "census1", gfs + ",SR_NATIVE_BRACKETS=1", "plain",
                     flush_residue_census_items("flush_split", "census1", False)
                     + [flush_split_item("flush_split_census1_split")]
                     + flush_row9_items("flush_split", "census1", strict=True)),
                Step("snapshot", "census1_snap", base="census1",
                     ledger=[flush_snapshot_item("flush_split_census1_snapshot")]),
                # the record's observation itself: transport_census.py's flush_residue group over the pair (complete,
                # reconciled, mapped; the base > 0, the target 0) - a run-unique comparison step
                Step("tcensus", "census", mode="flush", base="census0", target="census1",
                     ledger=[flush_census_item("flush_split_census")]),
                Step("roll", "meter0", gs + ",SR_NATIVE_FLUSH_SPLIT=0", "plain"),
                Step("roll", "meter1", gfs, "plain",
                     [flush_split_item("flush_split_meter1_split")] + flush_row9_items("flush_split", "meter1", strict=True)),
                Step("snapshot", "meter1_snap", base="meter1", ledger=[flush_snapshot_item("flush_split_meter1_snapshot")]),
                Step("meter", "meter", base="meter0", target="meter1", ledger=[meter_item(
                    "flush_split_meter", "meter0", -230.0, "PLANNING ESTIMATE from the rcen1 census pair (census1: 238.95 "
                    "a present inside 820FBBF8 / 820FC000 / 820FB6E8 after the construns split, row 9 3.69; bound = the "
                    "flush residue less ~4%); the producer census items (flush_split_census0_* / _census1_*) are the "
                    "producers' own measure")]),
                # the flicker gate, and the strict bar on both of its pure-ON rolls (activity not required: the span roll
                # is short)
                Step("gate", "gate", gfs, ledger=[gate_item("flush_split_gate")]),
                Step("snapshot", "gate_freeze_snap", base="gate/freeze",
                     ledger=[flush_snapshot_item("flush_split_gate_freeze_snapshot", activity=False)]),
                Step("snapshot", "gate_span_snap", base="gate/span",
                     ledger=[flush_snapshot_item("flush_split_gate_span_snapshot", activity=False)]),
            ],
        },
        "flush_split_bake": {
            "prefix": "fsb", "obligations": ["p1.flush_residue"], "gates": "", "bake": True,
            "about": "p1.flush_residue BAKED (SR_NATIVE_FLUSH_SPLIT default ON since 2026-10-14, native/flush-residue-2; =0 "
                     "keeps today's path): on the baked exe with no env - replay 9/9 vs the pre-bake exe (--base), the "
                     "new-game world-rule gate carrying the observe-only strict transport witness (SR_NATIVE_SEG_TILE=1, as "
                     "setconstmem_bake's gate) with the strict snapshot bar on both of its rolls, the default path under "
                     "the CPPM4 split (pixels exact per freeze, the CP's consumer counts for the S1 and row-9 records, "
                     "the strict row-9 lines and snapshot; Astra round 1), the default-path meter "
                     "(meter_off = SR_NATIVE_FLUSH_SPLIT=0 against meter_on = no env, the flush_split plan's bound; meter_on "
                     "graded on its snapshots and row 9's lines too) and a census pair on the default path "
                     "(census0 =0 against census1 with no native env; both SR_NATIVE_BRACKETS=1, observe-only) graded by "
                     "transport_census.py --flush-residue-zero over the interval and from boot (821075E0's alignment pad "
                     "construns' only as the witnessed key o6). Every default-path roll: pre=0 in every snapshot (frs1 / "
                     "frs1b met row 9's GPU-wait stop in 3 of 7, modelled since). The dry CHECKs, the frozen-pass A/B "
                     "and stress stay the flush_split plan's",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("bake_replay")]),
                Step("gate", "gate", "SR_NATIVE_SEG_TILE=1",
                     ledger=[gate_item("bake_gate", "the bake, no native env but the strict witness"),
                             dict(tile_drained_item("bake_gate_tile"), roll="freeze"),
                             dict(tile_drained_item("bake_gate_span_tile"), roll="span"),
                             dict(xport_item("bake_gate_xport"), roll="freeze")]),
                Step("snapshot", "gate_freeze_snap", base="gate/freeze",
                     ledger=[flush_snapshot_item("bake_gate_freeze_snapshot", activity=False)]),
                Step("snapshot", "gate_span_snap", base="gate/span",
                     ledger=[flush_snapshot_item("bake_gate_span_snapshot", activity=False)]),
                # the CPPM4 split on the default path (Astra round 1): the CP renders every record back to PM4 and runs
                # it - pixels exact per freeze, the transport's tripwires and tile, the consumer's own counts, the split's
                # paths and row 9 (the modelled waits included) under the strict bar and the strict snapshot
                Step("roll", "cppm4", "SR_NATIVE_FLUSH_CPPM4=1,SR_NATIVE_SEG_TILE=1", "freeze",
                     [freeze_exact("bake_cppm4"), segcounters_item("bake_cppm4_seg"), tile_item("bake_cppm4_tile"),
                      xport_item("bake_cppm4_xport"), flush_split_item("bake_cppm4_split")]
                     + flush_consumer_items("bake", "cppm4") + flush_row9_items("bake", "cppm4", strict=True)),
                Step("snapshot", "cppm4_snap", base="cppm4", ledger=[flush_snapshot_item("bake_cppm4_snapshot")]),
                Step("roll", "meter_off", "SR_NATIVE_FLUSH_SPLIT=0", "plain"),
                Step("roll", "meter_on", "", "plain",
                     [flush_split_item("bake_meter_on_split")] + flush_row9_items("bake", "meter_on", strict=True)),
                Step("snapshot", "meter_on_snap", base="meter_on", ledger=[flush_snapshot_item("bake_meter_on_snapshot")]),
                Step("meter", "meter", base="meter_off", target="meter_on", ledger=[meter_item(
                    "bake_meter", "meter_off", -230.0, "the baked default against SR_NATIVE_FLUSH_SPLIT=0 on the same exe; "
                    "the flush_split plan's bound (frs1b: -240.9, t0 142.3 -> 35.0, t2nop 182.2 -> 52.0)")]),
                Step("roll", "census0", "SR_NATIVE_FLUSH_SPLIT=0,SR_NATIVE_BRACKETS=1", "plain",
                     flush_residue_census_items("bake", "census0", True)),
                Step("roll", "census1", "SR_NATIVE_BRACKETS=1", "plain",
                     flush_residue_census_items("bake", "census1", False) + flush_row9_items("bake", "census1", strict=True)),
                Step("snapshot", "census1_snap", base="census1", ledger=[flush_snapshot_item("bake_census1_snapshot")]),
                Step("tcensus", "census", mode="flush", base="census0", target="census1",
                     ledger=[flush_census_item("bake_census")]),
            ],
        },
        "transport_composed": {
            "prefix": "tcmp", "obligations": ["p2.transport"], "gates": gcmp,
            "resolves": [("p2.transport", "composed_zero",
                          ["composed_census", "composed_abp", "composed_cppm4", "composed_stress", "composed_stress_off",
                           "composed_probe_trace", "composed_world"]),
                         ("p2.transport", "pacing", ["composed_pacing"])],
            "about": "p2.transport COMPOSED (NATIVE_ROADMAP 'p2.transport GAP MAP', the orchestrator's DECISIONS of "
                     "2026-10-14): every transport kind ON together - CALLBACK, GATE, SWAP_BLOCK + SWAP, BRACKET, the "
                     "residue, the pair - plus SR_NATIVE_DEVCREATE=1, SR_NATIVE_SHADER_CONST_RUNS off. Closes on the "
                     "TRANSPORT-PRODUCER ZERO (the census pair graded by tools/perf/transport_census.py --transport-zero) "
                     "and pacing (tools/perf/pacing_cmp.py on a console-paced pair); the global [PM4_COV] 0 is "
                     "acceptance.zero's. The offline composition proof (tools/difftest/present_composed_run.py, "
                     "composed_suites_run.py) is separate; these rolls are the live part.",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("composed_replay")]),
                # the probe: every kind's line with no decline, the strict transport witness, the P2 trace
                Step("roll", "probe", gcmp + ",SR_NATIVE_SEG_TILE=1", "plain",
                     [census_item("composed_probe_census"), segcounters_item("composed_probe_seg"),
                      tile_drained_item("composed_probe_tile"), xport_item("composed_probe_xport"),
                      composed_spans_item("composed_probe_spans"), ring0_item("composed_probe_ring0"),
                      devcreate_item("composed_probe_dc"), composed_gate_item("composed_probe_gate")] +
                     composed_swap_items("composed_probe_swap") +
                     [composed_bracket_item("composed_probe_bracket"), bracket_memory_item("composed_probe_bracket_memory"),
                      residue_item("composed_probe_residue", strict=True), composed_pair_item("composed_probe_pair")],
                     trace=True),
                # binding_trace_item (the exe traces the BRACKET_END binding, transport-fix): the final bracket the
                # roll's forced end cuts is tail_bound_unapplied, not `skipped` (tcmp1's FAIL)
                Step("p2trace", "probe_trace", ledger=[binding_trace_item("composed_probe_trace")], base="probe"),
                # the composed frozen-pass per-present A/B: odd presents every kind the SDK's, even every kind native
                Step("roll", "abp", gcmp + "," + cmp_ab + ",SR_NATIVE_SEG_TILE=1", "freeze",
                     [freeze_ab("composed_abp"), segcounters_item("composed_abp_seg"), tile_item("composed_abp_tile"),
                      xport_item("composed_abp_xport"), present_gate_item("composed_abp_gate"),
                      bracket_item("composed_abp_bracket", "both arms ran: abOff counts the SDK arm's begins"),
                      composed_ab_pair_item("composed_abp_pair")] + swap_sdk_items("composed_abp_swap")),
                Step("roll", "cppm4", gcmp + "," + CPPM4 + ",SR_NATIVE_SEG_TILE=1", "freeze",
                     [freeze_exact("composed_cppm4"), segcounters_item("composed_cppm4_seg"),
                      tile_item("composed_cppm4_tile"), composed_gate_item("composed_cppm4_gate", pm4=True),
                      residue_item("composed_cppm4_residue", strict=True)] +
                     composed_swap_items("composed_cppm4_swap", pm4=True)),
                Step("roll", "stress", gcmp + "," + CPPM4 + "," + STRESS, "freeze",
                     [freeze_exact("composed_stress"), segcounters_item("composed_stress_seg"),
                      composed_gate_item("composed_stress_gate", pm4=True),
                      swap_why_item("composed_stress_swap_why"), bracket_memory_item("composed_stress_bracket_memory")]),
                # the stress roll's all-OFF control (pre-registered): a stress failure the control shares is not the
                # composition's (tpl1's lesson)
                Step("roll", "stress_off", goff + "," + CPPM4 + "," + STRESS, "freeze",
                     [freeze_exact("composed_stress_off"), segcounters_item("composed_stress_off_seg")]),
                # the producer census pair: census0 every kind OFF, census1 the composed env; graded by the transport-
                # producer zero with the per-frame completeness checks (F records, reconciliation, coverage)
                # native/composed-census (tcmp2: census1 printed no census at all - every present a kRkSwap record, no
                # XE_SWAP packet to end a frame): each roll's counter witnessed on (armed) and its frames ended the way
                # its env presents (census0 at XE_SWAP packets, census1 at the records); the tcensus step tracks the F
                # lines against the CP's own present count
                Step("roll", "census0", goff + ",SR_NATIVE_BRACKETS=1", "plain",
                     [census_armed_item("composed_census0_armed"),
                      census_ends_item("composed_census0_ends", records=False)]),
                Step("roll", "census1", gcmp + ",SR_NATIVE_BRACKETS=1", "plain",
                     [residue_item("composed_census1_residue", strict=True),
                      composed_bracket_item("composed_census1_bracket"),
                      census_armed_item("composed_census1_armed"),
                      census_ends_item("composed_census1_ends", records=True)]),
                Step("tcensus", "census", base="census0", target="census1",
                     ledger=[composed_census_item("composed_census",
                                                  "the transport-producer zero over the census-covered interval")]),
                # the meter: information beside the census (pre-registered from the rcen1 gap map: the transport group
                # is 93.0 packets a present - VdSwap's block + 820F0048 60, the gate 11, the kick 10, the residue 12 -
                # the bound is it less 5%)
                Step("roll", "meter0", goff, "plain"),
                Step("roll", "meter1", gcmp, "plain"),
                Step("meter", "meter", base="meter0", target="meter1", ledger=[meter_item(
                    "composed_meter", "meter0", -88.4, "the transport group of the rcen1 gap map (93.0 a present on "
                    "exe 6705f9e06683) less 5%; the census items are the grade, this is the packet total's move")]),
                # pacing on the console path (SR_HIGHFPS=0: the port feature's uncapped mode releases the gate at once):
                # the composed roll against the all-OFF control, each with the P2 trace's pacing records (kind 29); a
                # SECOND identical all-OFF control after it (Astra round 2: the instrument's control-vs-control
                # variation measured live in the same session, the composed roll between the two controls)
                # Each pacing roll is LONG (Astra round 3): the graded window is pacing_cmp.PLAN_WINDOW present ids
                # (28,672: what every statistical check needs to be conclusive on the calibration fixtures - the
                # doubled-gate proportion at a 2.5% base rate the most, ~24,000), so the soak tape with a 1,200 s
                # gameplay dwell (36,000 presents at the console path's 30 a second; >= 28,672 down to 23.9 a second)
                Step("roll", "pace0", goff + ",SR_HIGHFPS=0", "plain", trace=True, **PACE_LONG),
                Step("roll", "pace1", gcmp + ",SR_HIGHFPS=0", "plain", trace=True, **PACE_LONG),
                Step("roll", "pace0b", goff + ",SR_HIGHFPS=0", "plain", trace=True, **PACE_LONG),
                # (every arm uninstrumented, so the same trace item for all: no forced-drain requirement)
                Step("p2trace", "pace0_trace", ledger=[pacing_trace_item("composed_pace0_trace")], base="pace0",
                     window=PACE_WINDOW),
                Step("p2trace", "pace1_trace", ledger=[pacing_trace_item("composed_pace1_trace")], base="pace1",
                     window=PACE_WINDOW),
                Step("p2trace", "pace0b_trace", ledger=[pacing_trace_item("composed_pace0b_trace")], base="pace0b",
                     window=PACE_WINDOW),
                Step("pacing", "pacing", base="pace0_trace", target="pace1_trace", calib="pace0b_trace",
                     ledger=[composed_pacing_item("composed_pacing")]),
                Step("gate", "gate", gcmp, ledger=[gate_item("composed_world")]),
            ],
        },
        "transport_fix": {
            "prefix": "tfix", "obligations": ["p2.transport"], "gates": gcmp,
            "no_ab": "the composed frozen-pass A/B is transport_composed's (tcmp1 composed_abp PASS); the fix changes only "
                     "default-OFF admissions, re-graded here on the counters",
            "about": "p2.transport COMPOSED follow-up (the tcmp1 diagnosis, transport-fix lane 2026-10-14): the composed "
                     "probe and stress again on the exe with the fixes - SWAP / BRACKET admit pages the GPU write tracker "
                     "owns read-only (gateWritable + gww::cpuWritableRange; default-OFF gates only), the declines counted "
                     "by reason / site with capped samples ([NATIVE_FLUSH] swapSdkWhy / bracketMemory, SWAP_DECL / "
                     "BRACKET_MEMORY), the BRACKET_END binding / plan / CP lifecycle trace records (kind 24 aux 5-9). "
                     "Pre-registered: SWAP declines only for a device / global mode (at most the boot decline SWAP alone "
                     "shows: sw1 1 / 1 / 1), BRACKET memory 0 (tcmp1 probe 1, stress 34), the probe trace's skipped 0 "
                     "with the final in-flight bracket as tail_bound_unapplied; the same probe ended by the graceful quit "
                     "(tools/graceful-quit, dev 95e221f5): its terminal snapshot complete, the game's own exit 0, every "
                     "text item's terminal twin, and the terminal segment_order whole (no open bracket, no unapplied bound "
                     "id, no untaken publish, no tail). Closes nothing on its own: composed_zero and pacing stay "
                     "transport_composed's.",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("tfix_replay")]),
                Step("roll", "probe", gcmp + ",SR_NATIVE_SEG_TILE=1", "plain",
                     [census_item("tfix_probe_census"), segcounters_item("tfix_probe_seg"),
                      tile_drained_item("tfix_probe_tile"), xport_item("tfix_probe_xport"),
                      swap_why_item("tfix_probe_swap_why"), swap_boot_only_item("tfix_probe_swap_boot"),
                      composed_bracket_item("tfix_probe_bracket"), bracket_memory_item("tfix_probe_bracket_memory")],
                     trace=True),
                Step("p2trace", "probe_trace", ledger=[binding_trace_item("tfix_probe_trace")], base="probe"),
                # tcmp1's stress roll (CPPM4 + the segment stress): its 24 SWAP / 34 BRACKET memory declines
                Step("roll", "stress", gcmp + "," + CPPM4 + "," + STRESS, "freeze",
                     [freeze_exact("tfix_stress"), segcounters_item("tfix_stress_seg"),
                      swap_why_item("tfix_stress_swap_why"), bracket_memory_item("tfix_stress_bracket_memory")]),
                # the same probe ended by the graceful quit (sr_drive -GracefulQuit through the gate): the CP drained to
                # the final write index, so no BRACKET_END is cut - the terminal items (Runner.terminal_items) and the
                # terminal segment_order
                Step("roll", "probe_graceful", gcmp + ",SR_NATIVE_SEG_TILE=1," + GRACEFUL, "plain",
                     [census_item("tfix_probe_graceful_census"), segcounters_item("tfix_probe_graceful_seg"),
                      tile_drained_item("tfix_probe_graceful_tile"), xport_item("tfix_probe_graceful_xport"),
                      swap_why_item("tfix_probe_graceful_swap_why"), swap_boot_only_item("tfix_probe_graceful_swap_boot"),
                      composed_bracket_item("tfix_probe_graceful_bracket"),
                      bracket_memory_item("tfix_probe_graceful_bracket_memory")],
                     trace=True),
                Step("p2trace", "probe_graceful_trace", ledger=[graceful_trace_item("tfix_probe_graceful_trace")],
                     base="probe_graceful"),
            ],
        },
        "residue": {
            "prefix": "tres", "obligations": ["p2.transport"], "gates": grs,
            "about": "p2.transport RESIDUE (NATIVE_ROADMAP transport plan item 5, its build note): BlockUntilIdle 820FDCB8's "
                     "WAIT_UNTIL as a register run (its fence wait and dev+0x2A74 spin the SDK's own), GpuInvalidateMemoryRange "
                     "820F4280's and the lock core 820F3988's owner-thread COHER blocks as kRkInvalidate / kRkCoherHost. Three "
                     "gates, default OFF, each requiring segmentless, toggled together per present here. The offline difftest "
                     "proof (tools/difftest/native_residue_run.py) is separate; these rolls are the live part.",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("residue_replay")]),
                # the probe: activity, the strict transport witness, and the P2 trace (the P2.7 invariants, the close
                # order, BlockUntilIdle drained: every entry completes, no spin over a frame, none exits with dev+0x2A74 set)
                Step("roll", "probe", grs + ",SR_NATIVE_SEG_TILE=1", "plain",
                     [census_item("residue_probe_census"), segcounters_item("residue_probe_seg"),
                      tile_drained_item("residue_probe_tile"), xport_item("residue_probe_xport"),
                      residue_item("residue_probe_counters", strict=True)], trace=True),
                Step("p2trace", "probe_trace", ledger=[residue_trace_item("residue_probe_trace")], base="probe"),
                # the frozen-pass per-present A/B: odd presents the SDK bodies, even the three native producers
                Step("roll", "abp", grs + "," + rs_ab + ",SR_NATIVE_SEG_TILE=1", "freeze",
                     [freeze_ab("residue_abp"), segcounters_item("residue_abp_seg"), tile_item("residue_abp_tile"),
                      xport_item("residue_abp_xport"),
                      residue_item("residue_abp_counters", note="both arms ran: decl ab= counts the SDK arm's calls")]),
                Step("roll", "cppm4", grs + "," + CPPM4, "freeze",
                     [freeze_exact("residue_cppm4"), segcounters_item("residue_cppm4_seg"),
                      residue_item("residue_cppm4_counters", strict=True)]),
                Step("roll", "stress", grs + "," + STRESS + "," + CPPM4, "freeze",
                     [freeze_exact("residue_stress"), segcounters_item("residue_stress_seg"),
                      residue_item("residue_stress_counters", strict=True)]),
                # the producers' own packets (the meter's kinds cannot isolate them: WAIT_UNTIL and COHER are type-0 / sync
                # among many): the PRODUCER census (SR_NATIVE_BRACKETS=1) on the default transport - each converted
                # producer present - then gated - each absent (producer_census_item); a fresh census pair
                Step("roll", "census0", gs_pre + ",SR_NATIVE_BRACKETS=1", "plain", residue_census_items("census0", True)),
                Step("roll", "census1", grs + ",SR_NATIVE_BRACKETS=1", "plain",
                     residue_census_items("census1", False) + [residue_item("residue_census1_counters", strict=True)]),
                # diagnostic only (no ledger item): the same pair by packet kind against the gated roll's own counts
                Step("census", "census", base="census0", target="census1"),
                Step("roll", "meter0", gs_pre, "plain"),
                Step("roll", "meter1", grs, "plain", [residue_item("residue_meter1_counters", strict=True)]),
                Step("meter", "meter", base="meter0", target="meter1", ledger=[meter_item(
                    "residue_meter", "meter0", -10.0, "PLANNING ESTIMATE from P1 census 2 (VertexBuffer_Lock 12 pk per "
                    "ingame frame = ~4 owner COHER blocks of 3; BlockUntilIdle ~0.24 / frame; InvalidateGpuCache unmeasured);"
                    " the producer census items (residue_census0_* / residue_census1_*) are the producers' own measure")]),
                Step("gate", "gate", grs, ledger=[gate_item("residue_gate")]),
            ],
        },
        "bracket": {
            "prefix": "tbr", "obligations": ["p2.transport"], "gates": gbr,
            "about": "p2.transport BRACKET (NATIVE_ROADMAP 'BRACKET BUILD NOTE'): the Z-pass bracket's kick 82110C78 as a "
                     "BRACKET_END record whose steps the CP replays (BS 0x15555555, depth records, BS FFFFFFFF, neutral "
                     "records once, BS 0x2AAAAAAA, depth records again, BS FFFFFFFF), dev+0x2A74 never moved, no interrupt / "
                     "pump for native brackets. Default OFF, requires segmentless, its own per-present A/B. The offline "
                     "difftest proof (tools/difftest/native_bracket_run.py) is separate; these rolls are the live part.",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("bracket_replay_tbr")]),
                # the probe: activity, the strict transport witness and the P2 trace (the P2.7 invariants, BlockUntilIdle
                # drained, the close order with the bracket class)
                Step("roll", "probe", gbr + ",SR_NATIVE_SEG_TILE=1", "plain",
                     [census_item("bracket_probe_census"), segcounters_item("bracket_probe_seg"), tile_drained_item("bracket_probe_tile"),
                      xport_item("bracket_probe_xport"), bracket_item("bracket_probe_bracket")], trace=True),
                Step("p2trace", "probe_trace", ledger=[bracket_trace_item("bracket_probe_trace")], base="probe"),
                # the frozen-pass per-present A/B: odd presents the SDK bracket (guest kick + pump), even BRACKET
                Step("roll", "abp", gbr + ",SR_AB_TOGGLE=SR_NATIVE_BRACKET,SR_AB_PRESENT=1,SR_NATIVE_SEG_TILE=1", "freeze",
                     [freeze_ab("bracket_abp"), segcounters_item("bracket_abp_seg"), tile_item("bracket_abp_tile"), xport_item("bracket_abp_xport"),
                      bracket_item("bracket_abp_bracket", "both arms ran: abOff counts the SDK arm's begins")]),
                Step("roll", "cppm4", gbr + "," + CPPM4, "freeze",
                     [freeze_exact("bracket_cppm4"), segcounters_item("bracket_cppm4_seg"), bracket_item("bracket_cppm4_bracket")]),
                Step("roll", "stress", gbr + "," + STRESS + "," + CPPM4, "freeze",
                     [freeze_exact("bracket_stress"), segcounters_item("bracket_stress_seg"), bracket_item("bracket_stress_bracket")]),
                Step("roll", "meter0", gs_pre, "plain"),
                Step("roll", "meter1", gbr, "plain", [bracket_item("bracket_meter1_bracket")]),
                Step("meter", "meter", base="meter0", target="meter1", ledger=[meter_item(
                    "bracket_meter", "meter0", -9.5, "PLANNING ESTIMATE from the build note (one bracket a frame; its kick's "
                    "literal coherency + callback PM4, ~10 packets); confirm against a census before grading")]),
                Step("gate", "gate", gbr, ledger=[gate_item("bracket_gate")]),
            ],
        },
        "coher_sentinel": {
            "prefix": "csn", "obligations": ["p2.segmentless"], "gates": "",
            "no_ab": "a default-path fix with no gate: nothing to toggle per present; the before is the offline "
                     "pre-fix mutant (tools/difftest/coher_sentinel_run.py)",
            "about": "native/coher-sentinel (a DEFAULT-PATH fix, no gate): the segmentless close's coherency range "
                     "dev+0x2D18 compared and restored with the SDK's empty value 0x00000000FFFFFFFF instead of all "
                     "ones. POST-FIX live validation on the fixed exe with no env: the probe whose [GPU_COHER] line "
                     "reads top=0 page=0 (no bogus 0xFFFFF000 page, no range to 4 GB) and whose close reads coherTop=0, "
                     "a CPPM4 freeze (records rendered back to PM4, the same witnesses), the world-rule gate. The "
                     "'before' evidence is offline only: the pre-fix mutant of tools/difftest/coher_sentinel_run.py. "
                     "The replay (Xenia traces, --base = the pre-fix exe) runs no guest code, so it never reaches "
                     "coherencyBlock: it shows only that the default rendering path's traces are unchanged",
            "steps": [
                # Xenia-trace replay: no guest execution, so not a before/after of the fix - the default rendering path's
                # traces unchanged only
                Step("replay", "replay", ledger=[replay_item("coher_replay")]),
                Step("roll", "probe", "", "plain",
                     [census_item("coher_probe_census"), segcounters_item("coher_probe_seg"),
                      coher_cp_item("coher_probe_cp"), coher_close_item("coher_probe_close")]),
                Step("roll", "cppm4", CPPM4, "freeze",
                     [freeze_exact("coher_cppm4"), segcounters_item("coher_cppm4_seg"),
                      coher_cp_item("coher_cppm4_cp"), coher_close_item("coher_cppm4_close")]),
                Step("gate", "gate", "", ledger=[gate_item("coher_gate")]),
            ],
        },
        "s1_ab": {
            "prefix": "s1b", "obligations": ["p1.s1_flush"], "gates": "",
            "about": "the S1 draw flush's first frozen-pass per-present A/B against the PM4 path (owed since S1 was baked)",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("replay_s1b")]),
                Step("roll", "abp", "SR_AB_TOGGLE=SR_NATIVE_FLUSH,SR_AB_PRESENT=1", "freeze", [freeze_ab("abp")]),
                Step("roll", "abps", "SR_AB_TOGGLE=SR_NATIVE_FLUSH,SR_AB_PRESENT=1," + STRESS, "freeze", [freeze_ab("abps")]),
            ],
            "resolves": [("p1.s1_flush", "no_frozen_ab", ["abp", "abps"])],
        },
        "s1_stress_ledger": {
            "prefix": "s1l", "obligations": ["p1.s1_flush"], "gates": "",
            "about": "the S1 stress A/B again with the per-draw ledger on: which draw / register carries the +-2 LSB noise s1b_abps and r467b_abp showed in one freeze of eleven",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("replay_s1l")]),
                Step("roll", "abpl", "SR_AB_TOGGLE=SR_NATIVE_FLUSH,SR_AB_PRESENT=1,SR_DRAW_LEDGER=1," + STRESS, "freeze",
                     [freeze_ab("abps_ledger", "the draw ledger's [DRAW_LEDGER] candidates name the draw")]),
            ],
        },
        "rows467_ab": {
            "prefix": "r467b", "obligations": ["p1.rows467"], "gates": "",
            "about": "rows 4+6+7's frozen-pass A/B again, under stress, on a drawn world (p1d_abp was black)",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("replay_r467b")]),
                Step("roll", "abp", "SR_AB_TOGGLE=SR_NATIVE_BEGINV,SR_AB_PRESENT=1," + STRESS, "freeze", [freeze_ab("abp")]),
            ],
        },
        "row3": {
            "prefix": "r3b", "obligations": ["p1.row3"], "gates": g3,
            "about": "row 3 (821072F8 as Driver items): stress, frozen-pass A/B, meter and gate again on a drawn world",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("replay_r3b")]),
                Step("roll", "stress", g3 + "," + STRESS + "," + CPPM4, "freeze", [freeze_exact("stress")]),
                Step("roll", "abp", g3 + ",SR_AB_TOGGLE=SR_NATIVE_WSCISSOR,SR_AB_PRESENT=1," + STRESS, "freeze", [freeze_ab("abp")]),
                Step("roll", "meter0", "", "plain"),
                Step("roll", "meter1", g3, "plain"),
                Step("meter", "meter", base="meter0", target="meter1", ledger=[meter_item("meter", "meter0", 0)]),
                Step("gate", "gate", g3, ledger=[gate_item("gate")]),
            ],
        },
        "shader_const_runs": {
            "prefix": "cr1", "obligations": ["p1.row3_construns"],
            "gates": gcr,
            "about": "821075E0 as native synchronization and four-register runs, including SDK chunk boundaries; default OFF",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("replay_cr1")]),
                Step("roll", "chk", gcr + ",SR_NATIVE_FLUSH_CHECK=1", "plain",
                     [chk_item("chk"), ev("chk_calls", "text", "generation", "generation",
                       select=r"\[NATIVE_FLUSH\] \w+ shaderConstRuns calls=", which="last",
                       pattern=r"calls=[1-9]\d* chunked=\d+", note="the new section must actually run in the CHECK roll")]),
                Step("roll", "cppm4", gcr + "," + CPPM4, "freeze", [freeze_exact("cppm4")]),
                Step("roll", "stress", gcr + "," + STRESS + "," + CPPM4, "freeze", [freeze_exact("stress")]),
                Step("roll", "abp", gcr + ",SR_AB_TOGGLE=SR_NATIVE_SHADER_CONST_RUNS,SR_AB_PRESENT=1," + STRESS,
                     "freeze", [freeze_ab("abp")]),
                Step("roll", "meter0", gcr.replace("SR_NATIVE_SHADER_CONST_RUNS=1", "SR_NATIVE_SHADER_CONST_RUNS=0"), "plain"),
                Step("roll", "meter1", gcr, "plain",
                     [ev("meter_calls", "text", "live-observe", "generation",
                         select=r"\[NATIVE_FLUSH\] \w+ shaderConstRuns calls=", which="last",
                         pattern=r"calls=[1-9]\d* chunked=\d+")]),
                Step("meter", "meter", base="meter0", target="meter1", ledger=[meter_item("meter", "meter0", 0)]),
                Step("gate", "gate", gcr, ledger=[gate_item("gate")]),
            ],
        },
        "row3_ctrl": {
            "prefix": "r3c", "obligations": [], "gates": "SR_NATIVE_WSCISSOR=0",
            "about": "row 3 control: the gates-OFF stress roll of the same exe (the PM4 path under STRESS) - does the "
                     "frozen window step the same way without row 3 (the stress_lsb_step question)",
            "steps": [
                Step("roll", "stress0", "SR_NATIVE_WSCISSOR=0," + STRESS + "," + CPPM4, "freeze"),
            ],
        },
        "row3_bake": {
            # Pinned (Astra round 1 on native/setconst-bake): its meter base r3b_meter0 is an archived roll of exe
            # d855d0e75a9b; on any later exe the default path also carries the bakes since (rows 5+3b, segmentless, row 9,
            # setconst), so the pm4 <= 0 bound would get easier. It runs only on the exe it ran on (main refuses others).
            "prefix": "r3e", "obligations": ["p1.row3"], "gates": "", "bake": True, "exe_pin": "47e80a6f28a0",
            "about": "row 3 BAKED (SR_NATIVE_WSCISSOR default ON): on the baked exe with no env - replay vs the pre-bake "
                     "exe, the world-rule flicker gate, the default-path meter roll (the drop vs r3b_meter0; the dispreg "
                     "column's first default-path number)",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("replay_bake")]),
                Step("gate", "gate", "", ledger=[gate_item("gate_bake", "the bake, no gates")]),
                Step("roll", "meter", "", "plain"),
                Step("meter", "meter_bake", base="r3b:meter0", target="meter",
                     ledger=[meter_item("meter_bake", "r3b:meter0", 0, "the baked default vs the pre-bake default roll r3b_meter0")]),
            ],
        },
        "rows53b_bake": {
            # Pinned as row3_bake: its meter base r5b_rerun/r5b_meter0 is an archived roll of exe d855d0e75a9b.
            "prefix": "r5e", "obligations": ["p1.row5", "p1.row3b"], "gates": "", "bake": True, "exe_pin": "87b0592ada63",
            "about": "rows 5 + 3b BAKED (SR_NATIVE_TILERECTS / SR_NATIVE_TILEPRED default ON): on the baked exe with no env - "
                     "replay vs the pre-bake exe (row 3 baked, 47e80a6f28a0), the world-rule flicker gate, the default-path "
                     "meter roll (the drop vs r5b_rerun/r5b_meter0; with row 3 the combined default-path drop)",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("replay_bake")]),
                Step("gate", "gate", "", ledger=[gate_item("gate_bake", "the bake, no gates")]),
                Step("roll", "meter", "", "plain"),
                Step("meter", "meter_bake", base="r5b_rerun/r5b_meter0", target="meter",
                     ledger=[meter_item("meter_bake", "r5b_rerun/r5b_meter0", 0,
                                        "the baked default vs the pre-bake default roll r5b_rerun/r5b_meter0")]),
            ],
        },
        "rows53b": {
            "prefix": "r5b", "obligations": ["p1.row5", "p1.row3b"], "gates": g5,
            "about": "rows 5 + 3b (820F0208 / 8210D400 as Entries): the whole bar on a drawn world",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("replay_r5b")]),
                Step("roll", "chk", "SR_NATIVE_FLUSH_CHECK=1," + g5, "plain", [chk_item("chk")]),
                Step("roll", "cppm4", g5 + "," + CPPM4, "freeze", [freeze_exact("cppm4")]),
                Step("roll", "stress", g5 + "," + STRESS + "," + CPPM4, "freeze", [freeze_exact("stress")]),
                Step("roll", "abp", g5 + ",SR_AB_TOGGLE=SR_NATIVE_TILERECTS+SR_NATIVE_TILEPRED,SR_AB_PRESENT=1," + STRESS,
                     "freeze", [freeze_ab("abp")]),
                Step("roll", "meter0", "", "plain"),
                Step("roll", "meter1", g5, "plain"),
                Step("meter", "meter", base="meter0", target="meter1", ledger=[meter_item("meter", "meter0", -50)]),
                Step("gate", "gate", g5, ledger=[gate_item("gate")]),
            ],
        },
        "memres_ab": {
            # mem.resources (lane D resproof, 2026-10-17; docs/RELEASE_COMPOSITION.md 9.5 step 2b, ledger guard pixels >=
            # frozen-ab): the frozen-pass per-present A/B of the native resource route against the page model.
            # SR_MEM_RESOURCES=1 + SR_AB_TOGGLE=SR_MEM_RESOURCES + SR_AB_PRESENT=1 is DUAL mode (gfx/resource_ab.hpp):
            # the registry and the page model keep their state in every present; the decider alternates per presenting
            # set, latched on the CP at the set's first drain and carried to the render thread, so consecutive frozen
            # dumps come from alternate deciders (freeze_diff --exact --ab-control: p->p+1 the A/B, p->p+2 the same-arm
            # control). The cross-arm rules (INV-M: a native upload of other bytes makes the model forget the page;
            # INV-R: a model upload from another window revokes the registry's; the model's GPU-write rebaseline and
            # the registry's drain-time GPU note in both arms; the texture clean marks invalidated by the union) are
            # proven offline by difftest --resource-memory-d3d12-test --ab (and its compiled no-op / always-model
            # controls). Every roll ends by the graceful quit (the witness reads the terminal [MEM_RES_AB] totals);
            # vblank_default / zpd_default: the real default build otherwise. Two routes: the new-game route (title,
            # menus, NEW GAME, the tutorial's gameplay) and the Warworld seed (the arena, the census's hardest era).
            # The stated limit: one arena, so a native keep the model repaired one present earlier cannot show
            # (cross_unannounced counts each repair); persistent staleness is boundaries_complete's checker's.
            "prefix": "mra", "obligations": ["mem.resources"], "gates": "SR_MEM_RESOURCES=1",
            "same_exe": True, "fresh_out": True, "vblank_default": True, "zpd_default": True,
            # no_replay: the per-present toggle cannot act inside a one-frame trace replay; the native route's replay
            # pairs are its own proof items (Codex's routing5 / ordered-restore replays, RELEASE_COMPOSITION 9.5)
            "no_replay": "the per-present decider toggle has no second present inside a one-frame trace replay",
            "about": "mem.resources: the frozen-pass per-present A/B of the native resource route (SR_MEM_RESOURCES=1) "
                     "against the page model - SR_AB_TOGGLE=SR_MEM_RESOURCES, SR_AB_PRESENT=1, dual bookkeeping "
                     "(gfx/resource_ab.hpp), the decider alternating per presenting set; pixels exact present for "
                     "present (freeze_ab, the A/B control columns, >= 1,000 [AB_PRESENT] lines) on the new-game route "
                     "and the Warworld seed, each with the toggle witness on the terminal totals (both arms decided, no "
                     "wrong-arm or unlatched decision, no cross-clobber, every consecutive dumped pair alternating)",
            "steps": [
                # mra2 (2026-10-17, PC): FREEZE's 8:2000:80 over the default tape gave 15 counted holds, 570 static
                # pairs exact, but 828 [AB_PRESENT] lines < the 1,000: the zpd bake's schedule (a 2-s hold every 4 s
                # from 141 s) over a 260-s dwell holds ~54 times
                Step("roll", "abp", genv(MEMRES_AB, GRACEFUL, ZPD_FREEZE), "freeze",
                     [freeze_ab("mra_abp", "the new-game route: title, menus, NEW GAME, the tutorial"),
                      memres_dual_item("mra_abp_dual"), memres_ab_item("mra_abp_witness")], dwell=260, seconds=900),
                # Astra review1 P2-4: the pairs the pixel grade compared, each joined to its two presents' arms
                Step("abwit", "abp_wit", target="abp", ledger=[memres_abwit_item(
                    "mra_abp_join", "the new-game route's compared pairs, each from two deciders")]),
                Step("roll", "abp_ww", genv(MEMRES_AB, GRACEFUL, ZPD_FREEZE), "freeze",
                     [freeze_ab("mra_abp_ww", "the Warworld seed: the arena"),
                      memres_dual_item("mra_abp_ww_dual"), memres_ab_item("mra_abp_ww_witness")],
                     from_save=WW_SEED, play=WW_TAPE, dwell=260, seconds=900),
                Step("abwit", "abp_ww_wit", target="abp_ww", ledger=[memres_abwit_item(
                    "mra_abp_ww_join", "the Warworld seed's compared pairs, each from two deciders")]),
            ],
        },
        "memres_bounds": {
            # mem.resources residual unknown boundaries_complete (lane D resproof, 2026-10-17): do the guest's own
            # coherency records cover every CPU write the GPU later reads? The census beside the record-driven
            # invalidation (MEMRES_CENSUS: the registry decides in shadow from the guest's records while the page model
            # decides, both model checkers re-verifying every keep) over whole runs of every era, each roll ended by
            # the graceful quit (the census's terminal window closes the last era), graded per era by
            # tools/perf/mem_res_census.py: 0 missed boundaries = the records were complete there. Routes: the new
            # game (boot, the movies, the menus, the tutorial's gameplay), the Warworld seed (the arena: the era the
            # header-fence stamps failed before native/mem-native d69ef001's exact dword patches), the chapter-18 seed
            # (Metropolis before the Twister) and the chapter-17 seed (the last chapters' content). A census, not a
            # bake: the default decides in every roll (vblank_default / zpd_default: the real default build otherwise).
            "prefix": "mrq", "obligations": ["mem.resources"], "gates": "SR_MEM_RESOURCES=census",
            "same_exe": True, "fresh_out": True, "vblank_default": True, "zpd_default": True,
            "no_replay": "a per-era census of whole live runs: a one-frame trace replay holds no era",
            "no_ab": "a census, not a route: the shadow registry decides nothing (the page model draws every "
                     "present); the native route's frozen-pass A/B is plan memres_ab's",
            "about": "mem.resources boundaries_complete: the shadow census (SR_MEM_RESOURCES=census) beside the page "
                     "model with both model checkers (SR_SHM_WT_CHECK=1, SR_NB_SHM_RANGE_CHECK=1) over whole runs of "
                     "the new game, the Warworld seed, the chapter-18 and chapter-17 seeds, graded per era by "
                     "mem_res_census.py: every era PASS (0 missed boundaries, every keep re-checked, exit evidence)",
            "steps": [
                Step("roll", "nb", genv(MEMRES_CENSUS, GRACEFUL), "plain", dwell=300),
                Step("memcensus", "nb_census", target="nb", ledger=[memres_census_item(
                    "mrq_nb", ("boot", "movies", "menus", "gameplay"),
                    "the new game: boot, the movies, the menus, the tutorial's gameplay")]),
                Step("roll", "ww", genv(MEMRES_CENSUS, GRACEFUL), "plain", from_save=WW_SEED, play=WW_TAPE, dwell=240,
                     seconds=900),
                Step("memcensus", "ww_census", target="ww", ledger=[memres_census_item(
                    "mrq_ww", ("menus", "gameplay", "warworld"), "the Warworld seed: the load, the arena")]),
                Step("roll", "ch18", genv(MEMRES_CENSUS, GRACEFUL), "plain", from_save=CH18_SEED, play=CH18_TAPE,
                     dwell=240, seconds=900),
                Step("memcensus", "ch18_census", target="ch18", era="ch18", ledger=[memres_census_item(
                    "mrq_ch18", ("menus", "ch18"), "the chapter-18 seed: Metropolis before the Twister")]),
                Step("roll", "ch17", genv(MEMRES_CENSUS, GRACEFUL), "plain", from_save=CH17_SEED, play=CH17_TAPE,
                     dwell=240, seconds=900),
                Step("memcensus", "ch17_census", target="ch17", era="ch17", ledger=[memres_census_item(
                    "mrq_ch17", ("menus", "ch17"), "the chapter-17 seed")]),
            ],
        },
        "memres_readers": {
            # mem.resources residual unknowns textures_from_resolves and resolve_writeback_readers (lane D resproof,
            # 2026-10-17): the native route deciding (SR_MEM_RESOURCES=1) over whole runs with two observe-only
            # censuses - the texture-identity census (SR_MEM_RES_TEXID=1: every 2D / stacked bind classified by the
            # resolve output on its pages and its content's origin; graded per era by tools/perf/mem_res_texid.py) and
            # Codex's guest-reader probe (SR_GUEST_READER_PROBE=1: TextureCopySurfaceRows' row copy and the texture-lock
            # sites; whole ranges and a whole-run event cap since resproof's follow-up, overflow = INCOMPLETE;
            # tools/perf/mem_res_reader_census.py) - on the routes of memres_bounds plus the ending (era matrix R3).
            # Each roll ends by the graceful quit (both censuses' terminal families).
            "prefix": "mrt", "obligations": ["mem.resources"], "gates": "SR_MEM_RESOURCES=1",
            "same_exe": True, "fresh_out": True, "vblank_default": True, "zpd_default": True,
            "no_replay": "a per-era census of whole live runs: a one-frame trace replay holds no era",
            "no_ab": "observe-only censuses of the native route: its frozen-pass A/B is plan memres_ab's",
            "about": "mem.resources textures_from_resolves + resolve_writeback_readers: the native route "
                     "(SR_MEM_RESOURCES=1) with the texture-identity census and the whole-run reader probe over the "
                     "new game, the Warworld seed, the chapter-18 seed, the ending (R3) and the chapter-17 seed, graded "
                     "per era",
            "steps": [
                Step("roll", "nb", genv(MEMRES_READERS, GRACEFUL), "plain", dwell=300),
                Step("texid", "nb_texid", target="nb", ledger=[memres_texid_item(
                    "mrt_nb_texid", ("gameplay",), "the new game: the tutorial's gameplay")]),
                Step("readers", "nb_readers", target="nb", ledger=[memres_readers_item("mrt_nb_readers")]),
                Step("roll", "ww", genv(MEMRES_READERS, GRACEFUL), "plain", from_save=WW_SEED, play=WW_TAPE,
                     dwell=240, seconds=900),
                Step("texid", "ww_texid", target="ww", ledger=[memres_texid_item(
                    "mrt_ww_texid", ("gameplay", "ingame"), "the Warworld seed: its gameplay and ingame eras")]),
                Step("readers", "ww_readers", target="ww", ledger=[memres_readers_item("mrt_ww_readers")]),
                Step("roll", "ch18", genv(MEMRES_READERS, GRACEFUL), "plain", from_save=CH18_SEED, play=CH18_TAPE,
                     dwell=240, seconds=900),
                Step("texid", "ch18_texid", target="ch18", era="ch18", ledger=[memres_texid_item(
                    "mrt_ch18_texid", ("ch18",), "the chapter-18 seed")]),
                Step("readers", "ch18_readers", target="ch18", era="ch18",
                     ledger=[memres_readers_item("mrt_ch18_readers")]),
                # the ending (R3: dwell 900, seconds 1800 as era_matrix.py's row; unstaged as that row)
                Step("roll", "end", genv(MEMRES_READERS, GRACEFUL, END_GATES), "plain", from_save=CH17_SEED,
                     play=END_TAPE, dwell=900, seconds=1800, devtools=END_PLAN),
                Step("texid", "end_texid", target="end", era="ending", ledger=[memres_texid_item(
                    "mrt_end_texid", ("ending",), "the ending route (R3): seed 17, the ending, the post-game")]),
                Step("readers", "end_readers", target="end", era="ending",
                     ledger=[memres_readers_item("mrt_end_readers")]),
                Step("roll", "ch17", genv(MEMRES_READERS, GRACEFUL), "plain", from_save=CH17_SEED, play=CH17_TAPE,
                     dwell=240, seconds=900),
                Step("texid", "ch17_texid", target="ch17", era="ch17", ledger=[memres_texid_item(
                    "mrt_ch17_texid", ("ch17",), "the chapter-17 seed")]),
                Step("readers", "ch17_readers", target="ch17", era="ch17",
                     ledger=[memres_readers_item("mrt_ch17_readers")]),
            ],
        },
        "memres_bake": {
            # mem.resources BAKED (lane C native/mem-native, 2026-10-17; docs/RELEASE_COMPOSITION.md section 7): the dev
            # build's native resource route default ON (SR_MEM_RESOURCES unset = native; =0 keeps the page model as the
            # A/B and oracle arm; =census the page model with the shadow census). The release build has routed natively
            # only since 26d31de9. Every arm on ONE exe (same_exe) into a fresh --out; a live run refuses while the
            # parent environment sets SR_MEM_RESOURCES (default_keys); vblank_default / zpd_default: the real default
            # build. Every roll ends by the graceful quit (the terminal patch and meter totals).
            "prefix": "mrb", "obligations": ["mem.resources"], "gates": "", "bake": True, "same_exe": True,
            "fresh_out": True, "default_keys": ("SR_MEM_RESOURCES",), "vblank_default": True, "zpd_default": True,
            # the pre-bake exe's page model replays with the ordered Oracle restore boundaries (release/mem-replay-
            # restore, cherry-picked as 7f76b7b3..8c6e414a): without them lg1 / lg2 differ by the replay restore's own
            # semantic discrepancy (mrr_restore3: ordered == native, byte for byte), not by the route
            # SR_PM4_RENDER_THREAD=0 on both arms: the ordered restore refuses the render worker (mrb1's base arm
            # threw on every trace), as mrr_restore3 replayed its three arms
            "base_gates": "SR_MEM_RESOURCES=0,SR_REPLAY_ORDERED_RESTORES=1,SR_PM4_RENDER_THREAD=0",
            "replay_gates": "SR_PM4_RENDER_THREAD=0",
            "about": "mem.resources BAKED (SR_MEM_RESOURCES default ON in the dev build since native/mem-native; =0 the "
                     "page model): on the baked exe with no env - replay 9/9 vs the pre-bake exe (--base, its page model "
                     "with SR_REPLAY_ORDERED_RESTORES=1); the new-game world-rule flicker gate on the default route (the "
                     "route witness in both rolls); the shm meter on the new-game route and on the Warworld seed "
                     "(=0 against the default on the same exe: the page model's validations per ingame frame >= 1,000 "
                     "-> 0), each default arm with the route's default form, its exact CPU-write patches noted and "
                     "taken, and the whole run's [NATIVE_METER_RUN] shm=0 (every era); each =0 arm the off form. The "
                     "frozen-pass per-present A/B is memres_ab's (lane D), the shadow census the census rolls' "
                     "(tools/perf/mem_res_census.py)",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("mrb_replay")]),
                Step("gate", "gate", "",
                     ledger=[gate_item("mrb_gate", "the bake, no env"),
                             dict(memres_route_item("mrb_gate_route", "default"), roll="freeze"),
                             dict(memres_route_item("mrb_gate_span_route", "default"), roll="span")]),
                Step("roll", "meter_off", genv("SR_MEM_RESOURCES=0", GRACEFUL), "plain",
                     [memres_route_item("mrb_meter_off_route", "off")]),
                Step("roll", "meter_on", GRACEFUL, "plain",
                     [memres_route_item("mrb_meter_on_route", "default"), memres_patch_item("mrb_meter_on_patches"),
                      memres_run_zero_item("mrb_meter_on_run")]),
                Step("meter", "meter", base="meter_off", target="meter_on", ledger=[memres_shm_meter_item(
                    "mrb_meter", "meter_off", "the new-game route: the page model's validations -> 0 on the default")]),
                Step("roll", "ww_off", genv("SR_MEM_RESOURCES=0", GRACEFUL), "plain",
                     [memres_route_item("mrb_ww_off_route", "off")],
                     from_save=WW_SEED, play=WW_TAPE, dwell=120, seconds=900),
                Step("roll", "ww_on", GRACEFUL, "plain",
                     [memres_route_item("mrb_ww_on_route", "default"), memres_patch_item("mrb_ww_on_patches"),
                      memres_run_zero_item("mrb_ww_on_run")],
                     from_save=WW_SEED, play=WW_TAPE, dwell=120, seconds=900),
                Step("meter", "meter_ww", base="ww_off", target="ww_on", ledger=[memres_shm_meter_item(
                    "mrb_meter_ww", "ww_off", "the Warworld seed (the census's hardest era): -> 0 on the default")]),
            ],
        },
        "setconstmem": {
            "prefix": "scm", "obligations": ["p2.setconst_mem"], "gates": "SR_NATIVE_SETCONSTMEM=1",
            "about": "P2-B item 5: EmitPm4Packet 82113010 (Lion's per-tile constants) as a kRkSetConstMem record",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("replay_scm")]),
                Step("roll", "chk", "SR_NATIVE_FLUSH_CHECK=1,SR_NATIVE_SETCONSTMEM=1", "plain", [chk_item("chk")]),
                # the ring path: the span stays literal (the host writes the XDK bytes, the caller the body) - pixels exact.
                # SR_NATIVE_SEGMENTLESS=0 since the segmentless bake (3975b905): without it this roll ran segmentless with
                # SR_NATIVE_SEG_STRESS live (inert on the ring path, as on scm1), reached swap ~10k instead of ~18k and
                # logged < 1000 [AB_PRESENT] lines (scm4: 936, 894) - and graded the segmentless path abp_seg already covers
                Step("roll", "abp", "SR_NATIVE_SEGMENTLESS=0,SR_NATIVE_SETCONSTMEM=1,SR_AB_TOGGLE=SR_NATIVE_SETCONSTMEM,SR_AB_PRESENT=1,"
                     + STRESS, "freeze", [freeze_ab("abp")]),
                # segmentless: the record form (the meter's drop) against the guest's PM4 through the gap bridge
                Step("roll", "abp_seg", gs + ",SR_NATIVE_SETCONSTMEM=1,SR_AB_TOGGLE=SR_NATIVE_SETCONSTMEM,SR_AB_PRESENT=1", "freeze",
                     [freeze_ab("abp_seg"), segcounters_item("abp_seg_seg")]),
                Step("roll", "cppm4", gs + ",SR_NATIVE_SETCONSTMEM=1," + PM4OUT, "freeze",
                     [freeze_exact("cppm4"), segcounters_item("cppm4_seg")]),
                # the base census must show 82113010's SET_CONSTANTs: SR_NATIVE_SETCONSTMEM=0 since the setconst bake
                # (default ON from 2026-10-14; a bare gs base would hold the records and fail base_min)
                Step("roll", "meter0", gs + ",SR_NATIVE_SETCONSTMEM=0", "plain"),
                Step("roll", "meter1", gs + ",SR_NATIVE_SETCONSTMEM=1", "plain"),
                # 82113010's own packets: SET_CONSTANT (kind const; its NOP pads share t2nop with every other producer)
                Step("meter", "meter", base="meter0", target="meter1", ledger=[kind_zero_item(
                    "meter", "meter0", "const", 5.0, 0.1, "82113010's SET_CONSTANTs by kind: the base census (meter0) "
                    "shows them, meter1 none; the total pm4 change is information (scm1: -16.3)")]),
                Step("gate", "gate", "SR_NATIVE_SETCONSTMEM=1", ledger=[gate_item("gate")]),
            ],
        },
        "setconstmem_bake": {
            "prefix": "scb", "obligations": ["p2.setconst_mem"], "gates": "", "bake": True,
            "about": "per-tile constants BAKED (SR_NATIVE_SETCONSTMEM default ON since 2026-10-14; =0 keeps the guest "
                     "body): on the baked exe with no env - replay 9/9 vs the pre-bake exe (--base: the dev exe "
                     "328f9c1b4d64, native/archive/exe_snapshots/328f9c1b4d64-dev465c), the new-game world-rule gate "
                     "carrying the strict transport witness (SR_NATIVE_SEG_TILE=1, observe-only: the tile, first "
                     "application, close order and drained reconciliation, as segmentless_bake's gate - the kRkSetConstMem "
                     "records now ride in every segment), the default path under the CPPM4 split (the first live roll of "
                     "the CP-side render-back of kRkSetConstMem - cpRecordPm4's case, added on native/setconst-bake with "
                     "its cpPm4 counter and the offline proof tools/difftest/setconst_render_run.py; before it the record "
                     "took the opaque fallback through nativeRecord - which every CPPM4 roll of another plan now runs; "
                     "the setconstmem plan's 'cppm4' was the producer-side SR_NATIVE_FLUSH_PM4), and the meter on the same "
                     "exe: meter_off (SR_NATIVE_SETCONSTMEM=0) against meter_on (no env) - 82113010's SET_CONSTANTs by "
                     "kind (the off arm shows them, the default none; the setconstmem plan's meter step) and the total pm4 "
                     "not rising. The dry CHECK and the A/Bs (SR_AB_TOGGLE=SR_NATIVE_SETCONSTMEM on the ring path and "
                     "under segmentless) stay the setconstmem plan's (scm4); the per-present A/B of segmentless itself "
                     "is void in both forms (p2.segmentless abp_toggle_drops) and is not an instrument here",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("bake_replay")]),
                # the strict witness in the gate's two rolls, as segmentless_bake's gate (observe-only: the default path
                # otherwise), and every arena record executed in its freeze roll
                Step("gate", "gate", "SR_NATIVE_SEG_TILE=1",
                     ledger=[gate_item("bake_gate", "the bake, no native env but the strict witness"),
                             dict(tile_drained_item("bake_gate_tile"), roll="freeze"),
                             dict(tile_drained_item("bake_gate_span_tile"), roll="span"),
                             dict(xport_item("bake_gate_xport"), roll="freeze"),
                             dict(setconst_item("bake_gate_calls", "82113010's Entries ran in the gate's freeze roll"),
                                  roll="freeze")]),
                # the CP renders every record back to PM4 (kRkSetConstMem: cpRecordPm4's case, setConstMemRender at apply
                # time over the body the caller filled) and Pm4Engine::execute runs it: pixels exact, the transport's
                # tripwires and tile, the Entries taken, and the consumer's own count of setconst records rendered back
                Step("roll", "cppm4", "SR_NATIVE_FLUSH_CPPM4=1,SR_NATIVE_SEG_TILE=1", "freeze",
                     [freeze_exact("bake_cppm4"), segcounters_item("bake_cppm4_seg"), tile_item("bake_cppm4_tile"),
                      setconst_item("bake_cppm4_calls", "82113010's Entries ran under the CPPM4 split"),
                      setconst_cppm4_item("bake_cppm4_rendered", "the CP rendered kRkSetConstMem back to PM4 itself")]),
                Step("roll", "meter_off", "SR_NATIVE_SETCONSTMEM=0", "plain"),
                Step("roll", "meter_on", "", "plain"),
                # 82113010's own packets by kind (const; its NOP pads share t2nop with every other producer), and the
                # total as a second item on the same pair (one meter step grades one bound)
                Step("meter", "meter", base="meter_off", target="meter_on", ledger=[kind_zero_item(
                    "bake_meter", "meter_off", "const", 5.0, 0.1, "82113010's SET_CONSTANTs by kind on the baked default: "
                    "the SR_NATIVE_SETCONSTMEM=0 arm shows them, the default none (scm4: 6.9 -> 0.0)")]),
                Step("meter", "meter_pm4", base="meter_off", target="meter_on", ledger=[meter_item(
                    "bake_meter_pm4", "meter_off", 0.0, "the total pm4 per ingame frame does not rise on the baked default "
                    "against SR_NATIVE_SETCONSTMEM=0 on the same exe (scm4: -15.4)")]),
            ],
        },
        "construns_bake": {
            # p1.row3_construns BAKED (native/construns-bake, 2026-10-14; NATIVE_ROADMAP "CONSTRUNS BAKE PLAN"): every
            # item on ONE exe (the baked lane build), every pure-ON roll carrying the strict witness and the construns
            # counters; the meter and the census pair same-exe (=0 arms), so no pin.
            "prefix": "crb", "obligations": ["p1.row3_construns"], "gates": "", "bake": True,
            "resolves": [
                ("p1.row3_construns", "default_bake",
                 ["crb_replay", "crb_gate", "crb_gate_tile", "crb_gate_span_tile", "crb_gate_scr", "crb_cppm4",
                  "crb_cppm4_scr", "crb_stress", "crb_stress_scr", "crb_abp", "crb_census", "crb_meter"]),
                ("p1.row3_construns", "segmentless_backpressure",
                 ["crb_probe_trace", "crb_probe_tile", "crb_gate_tile", "crb_gate_span_tile", "crb_stress_scr",
                  "crb_stress_seg", "crb_probe_scr"]),
                ("p1.row3_construns", "library_coverage",
                 ["crb_census", "crb_census1_scr", "crb_gate_scr", "crb_gate_span_scr", "crb_probe_scr",
                  "crb_meter_on_scr"]),
            ],
            "about": "the construns BAKE (SR_NATIVE_SHADER_CONST_RUNS default ON; =0 keeps the SDK body) on the baked exe "
                     "with no env: replay 9/9 vs the pre-bake exe (--base: the dev 4aa4f963 build); the new-game "
                     "world-rule gate with the strict transport witness (SR_NATIVE_SEG_TILE=1: tile, first application, "
                     "close order, drained reconciliation) and the construns counters strict (no SDK body: decl 0, "
                     "abGuest 0; chunked 0 and marginBad 0, the caller-domain bound's live preconditions; tessDraws 0); "
                     "a traced probe (the P2 trace's close order and terminal reconciliation with construns records "
                     "riding every segment); CPPM4 exact (the CP renders kRkShaderConstSync back itself: cpPm4 > 0); "
                     "STRESS exact (the entry check switching segments: kick > 0); the frozen-pass per-present A/B "
                     "(SR_AB_TOGGLE=SR_NATIVE_SHADER_CONST_RUNS, both arms counted); the PRODUCER census pair (=0 vs "
                     "the default, SR_NATIVE_BRACKETS=1) graded by transport_census.py --construns-zero (821075E0's "
                     "SDK-body preamble on the S1 lines in the base, none in the target, no tessellated draw in "
                     "either); the meter =0 vs the default (pre-registered -10.0 per ingame frame: the census's 15.0 "
                     "a present less a per-pair noise allowance; the census pair is the producer-exact measure). The "
                     "offline proof is tools/difftest/construns_bake_run.py (the default, the caller domain against "
                     "the SDK, the counters, six compiled faults) and shader_const_runs_test_run.py's matrix",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("crb_replay")]),
                Step("gate", "gate", "SR_NATIVE_SEG_TILE=1",
                     ledger=[gate_item("crb_gate", "the bake, no native env but the strict witness"),
                             dict(tile_drained_item("crb_gate_tile"), roll="freeze"),
                             dict(tile_drained_item("crb_gate_span_tile"), roll="span"),
                             dict(xport_item("crb_gate_xport"), roll="freeze"),
                             dict(scr_item("crb_gate_scr"), roll="freeze"),
                             dict(scr_item("crb_gate_span_scr"), roll="span")]),
                # the traced probe: the default path with the strict witness and the P2 trace (close order, forced
                # drained points, the terminal reconciliation) - construns' records ride in every segment
                Step("roll", "probe", "SR_NATIVE_SEG_TILE=1", "plain",
                     [census_item("crb_probe_census"), segcounters_item("crb_probe_seg"),
                      tile_drained_item("crb_probe_tile"), xport_item("crb_probe_xport"), scr_item("crb_probe_scr")],
                     trace=True),
                Step("p2trace", "probe_trace", ledger=[trace_order_item("crb_probe_trace")], base="probe"),
                Step("roll", "cppm4", CPPM4 + ",SR_NATIVE_SEG_TILE=1", "freeze",
                     [freeze_exact("crb_cppm4"), segcounters_item("crb_cppm4_seg"), tile_item("crb_cppm4_tile"),
                      scr_item("crb_cppm4_scr", "cppm4")]),
                Step("roll", "stress", STRESS + "," + CPPM4, "freeze",
                     [freeze_exact("crb_stress"), segcounters_item("crb_stress_seg"),
                      scr_item("crb_stress_scr", "stress")]),
                Step("roll", "abp", "SR_AB_TOGGLE=SR_NATIVE_SHADER_CONST_RUNS,SR_AB_PRESENT=1", "freeze",
                     [freeze_ab("crb_abp"), segcounters_item("crb_abp_seg"), scr_item("crb_abp_scr", "ab")]),
                Step("roll", "census0", "SR_NATIVE_SHADER_CONST_RUNS=0,SR_NATIVE_BRACKETS=1", "plain"),
                Step("roll", "census1", "SR_NATIVE_BRACKETS=1", "plain", [scr_item("crb_census1_scr")]),
                Step("tcensus", "census", mode="construns", base="census0", target="census1",
                     ledger=[construns_census_item("crb_census")]),
                Step("roll", "meter_off", "SR_NATIVE_SHADER_CONST_RUNS=0", "plain"),
                Step("roll", "meter_on", "", "plain", [scr_item("crb_meter_on_scr")]),
                Step("meter", "meter", base="meter_off", target="meter_on", ledger=[meter_item(
                    "crb_meter", "meter_off", -10.0, "the baked default against SR_NATIVE_SHADER_CONST_RUNS=0 on the "
                    "same exe; PRE-REGISTERED bound -10.0 pm4 per ingame frame (the census's 15.0 a present for "
                    "821075E0's preamble + runs, less a per-pair noise allowance; the census pair is the exact "
                    "producer measure)")]),
            ],
        },
        "transport_bake": {
            # p2.transport BAKED (native/transport-bake, 2026-10-15; NATIVE_ROADMAP "TRANSPORT BAKE PLAN"): the nine
            # transport kinds default ON - CALLBACK, GATE, SWAP_BLOCK + SWAP, BRACKET, IDLE_WAIT / INVAL_RANGE /
            # LOCK_COHER, the pair (=0 keeps each guest body and the A/B); DEVCREATE stays OFF (boot.device_create's).
            # Every item on ONE exe (the baked lane build): the default arms run no native env but the observe-only
            # witnesses (SR_NATIVE_SEG_TILE, the P2 trace, the producer census), the =0 arms pin the nine kinds off on the
            # same exe, so no exe pin. ENFORCED since Astra round 1 (P2): same_exe - every roll stamped with the exe's
            # sha256 (STAMP), each comparison refuses another exe's input, a live run refuses an --out holding another
            # exe's rolls.
            # THE STRICT WITNESS (Astra round 1, P2): every pixel roll (A/B, CPPM4, stress, its =0 control) grades the
            # complete drained predicate (tile_drained_item: the tile, the close order idreorder 0, BlockUntilIdle and
            # the forced drained points drainMiss / forcedMiss 0) and the reclaim audit (xport_item: NEVER-EXECUTED 0),
            # as the gate and the probe do - a stress-only ordering defect cannot pass on unchanged pixels. Not in the
            # census and meter arms, which ran without SR_NATIVE_SEG_TILE (as rcen1 / dmb1 did, the measurements the
            # -82.6 bound is registered against): the witness's forced drained point stalls the producer thread up to
            # 200 ms every 10 s (kernel/native_flush segTileForcedDrain) - an arm-only change to the counted interval -
            # and their default arm is the same exe and env as the witnessed probe / pixel rolls. Pacing runs
            # uninstrumented on purpose (THE EXCEPTION: a 200 ms stall every 10 s is a pacing defect of its own); its
            # transport is the witnessed default of every other roll on the same exe.
            "prefix": "tbk", "obligations": ["p2.transport"], "gates": "", "bake": True, "same_exe": True,
            "resolves": [
                ("p2.transport", "default_bake",
                 ["tbk_replay", "tbk_gate", "tbk_gate_tile", "tbk_gate_span_tile", "tbk_probe_tile", "tbk_probe_trace",
                  "tbk_abp", "tbk_abp_tile", "tbk_abp_xport", "tbk_cppm4", "tbk_cppm4_tile", "tbk_cppm4_xport",
                  "tbk_stress", "tbk_stress_tile", "tbk_stress_xport", "tbk_stress_off", "tbk_stress_off_tile",
                  "tbk_stress_off_xport", "tbk_census", "tbk_meter"]),
                ("p2.transport", "default_pacing", ["tbk_pacing"]),
            ],
            "about": "p2.transport BAKED (the nine transport kinds default ON since native/transport-bake; =0 keeps each "
                     "guest body; SR_NATIVE_DEVCREATE stays default OFF - boot.device_create is not closed) on the baked "
                     "exe with no env: replay 9/9 vs the pre-bake exe (--base); the new-game world-rule gate with the "
                     "strict transport witness (SR_NATIVE_SEG_TILE=1) in both of its rolls; a traced probe (every kind's "
                     "line pure ON: no decline, no literal, the residue strict, the pair decline 0; the P2.7 invariants, "
                     "the close order with the bracket class and its binding records, BlockUntilIdle drained); the "
                     "frozen-pass per-present A/B of the default against =0 (every baked kind but the VdSwap block, the "
                     "swap_sdk precedent); CPPM4 on the default; STRESS on the default and its =0 control; every pixel "
                     "roll with the complete drained witness and the reclaim audit; the producer "
                     "census pair (=0 vs the default, SR_NATIVE_BRACKETS=1) graded by transport_census.py --pm4-zero "
                     "--allow-devcreate-reemit (the default arm's gameplay holds NO PM4 but 820F0048's per-present "
                     "re-emit, 6 a present - 60-63, r05, r0F - DEVCREATE's - whole, and attributed to 82112050 over the "
                     "whole gameplay span: an unattributed allowed packet is INCONCLUSIVE); the meter =0 vs the default "
                     "(pre-registered "
                     "-82.6 per ingame frame: the transport group 93.0 less the re-emit's 6.0, less 5%); pacing on the "
                     "console path, the "
                     "default against =0 with a second =0 control as the calibration (pacing_cmp.py, the 28,672-present "
                     "window: tcmp2's composed PASS ran construns OFF, the default runs it ON). Every arm on one exe, "
                     "enforced (each roll's exe stamp). The offline proof (the "
                     "composed present matrix, composed_suites_run.py, every suite vs the pre-bake build) is separate",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("tbk_replay")]),
                # the flicker gate on the default exe, the strict witness in both rolls, the converted present path seen
                # in the freeze roll (the gate and the VdSwap block native, no decline)
                Step("gate", "gate", "SR_NATIVE_SEG_TILE=1",
                     ledger=[gate_item("tbk_gate", "the bake, no native env but the strict witness"),
                             dict(tile_drained_item("tbk_gate_tile"), roll="freeze"),
                             dict(tile_drained_item("tbk_gate_span_tile"), roll="span"),
                             dict(xport_item("tbk_gate_xport"), roll="freeze"),
                             dict(composed_gate_item("tbk_gate_present"), roll="freeze"),
                             dict(composed_swap_items("tbk_gate_swap")[2], roll="freeze")]),
                # the traced probe: the default path pure ON, every kind's own line, the strict witness, the P2 trace
                Step("roll", "probe", "SR_NATIVE_SEG_TILE=1", "plain",
                     [census_item("tbk_probe_census"), segcounters_item("tbk_probe_seg"),
                      tile_drained_item("tbk_probe_tile"), xport_item("tbk_probe_xport"),
                      composed_spans_item("tbk_probe_spans"), composed_gate_item("tbk_probe_gate")] +
                     composed_swap_items("tbk_probe_swap") +
                     [composed_bracket_item("tbk_probe_bracket"), bracket_memory_item("tbk_probe_bracket_memory"),
                      residue_item("tbk_probe_residue", strict=True), composed_pair_item("tbk_probe_pair")],
                     trace=True),
                Step("p2trace", "probe_trace", ledger=[binding_trace_item("tbk_probe_trace")], base="probe"),
                # the frozen-pass per-present A/B: odd presents every baked kind =0 (the SDK bodies), even the default
                Step("roll", "abp", "SR_AB_TOGGLE=" + "+".join(k for k in TRANSPORT_BAKED if k != "SR_NATIVE_SWAP_BLOCK")
                     + ",SR_AB_PRESENT=1,SR_NATIVE_SEG_TILE=1", "freeze",
                     [freeze_ab("tbk_abp"), segcounters_item("tbk_abp_seg"), tile_drained_item("tbk_abp_tile"),
                      xport_item("tbk_abp_xport"), present_gate_item("tbk_abp_gate"),
                      bracket_item("tbk_abp_bracket", "both arms ran: abOff counts the SDK arm's begins"),
                      composed_ab_pair_item("tbk_abp_pair")] + swap_sdk_items("tbk_abp_swap")),
                Step("roll", "cppm4", CPPM4 + ",SR_NATIVE_SEG_TILE=1", "freeze",
                     [freeze_exact("tbk_cppm4"), segcounters_item("tbk_cppm4_seg"), tile_drained_item("tbk_cppm4_tile"),
                      xport_item("tbk_cppm4_xport"),
                      composed_gate_item("tbk_cppm4_gate", pm4=True), residue_item("tbk_cppm4_residue", strict=True)] +
                     composed_swap_items("tbk_cppm4_swap", pm4=True)),
                Step("roll", "stress", STRESS + "," + CPPM4 + ",SR_NATIVE_SEG_TILE=1", "freeze",
                     [freeze_exact("tbk_stress"), segcounters_item("tbk_stress_seg"), tile_drained_item("tbk_stress_tile"),
                      xport_item("tbk_stress_xport"),
                      composed_gate_item("tbk_stress_gate", pm4=True), swap_why_item("tbk_stress_swap_why"),
                      bracket_memory_item("tbk_stress_bracket_memory")]),
                # the stress roll's =0 control (pre-registered, tpl1's lesson): a stress failure the control shares is
                # not the bake's
                Step("roll", "stress_off", genv(TRANSPORT_OFF, STRESS + "," + CPPM4 + ",SR_NATIVE_SEG_TILE=1"), "freeze",
                     [freeze_exact("tbk_stress_off"), segcounters_item("tbk_stress_off_seg"),
                      tile_drained_item("tbk_stress_off_tile"), xport_item("tbk_stress_off_xport")]),
                # the producer census pair: census0 the nine kinds =0, census1 the default; the coverage witness on both
                # (armed; ENDS: census0 every frame at an XE_SWAP packet, census1 at the VdSwap block's present records)
                Step("roll", "census0", genv(TRANSPORT_OFF, "SR_NATIVE_BRACKETS=1"), "plain",
                     [census_armed_item("tbk_census0_armed"), census_ends_item("tbk_census0_ends", records=False)]),
                Step("roll", "census1", "SR_NATIVE_BRACKETS=1", "plain",
                     [residue_item("tbk_census1_residue", strict=True), composed_bracket_item("tbk_census1_bracket"),
                      census_armed_item("tbk_census1_armed"), census_ends_item("tbk_census1_ends", records=True)]),
                Step("tcensus", "census", mode="pm4zero", base="census0", target="census1",
                     ledger=[pm4_zero_census_item("tbk_census", "the default path's gameplay PM4: none but 820F0048's "
                                                                "re-emit (DEVCREATE unbaked)")]),
                Step("roll", "meter_off", TRANSPORT_OFF, "plain"),
                Step("roll", "meter_on", "", "plain"),
                Step("meter", "meter", base="meter_off", target="meter_on", ledger=[meter_item(
                    "tbk_meter", "meter_off", -82.6, "the baked default against the nine kinds =0 on the same exe; "
                    "PRE-REGISTERED bound -82.6 pm4 per ingame frame: the transport group 93.0 (rcen1, the default meter "
                    "dmb1 = 93.0) less 820F0048's re-emit 6.0 (60-63, r05, r0F) that stays with DEVCREATE off, less 5%; "
                    "expected default ~6.0; the census pair is the exact producer measure")]),
                # pacing on the console path (SR_HIGHFPS=0), uninstrumented, the P2 trace's kind-29 records: =0, the
                # default, a second identical =0 as the calibration; long rolls for the 28,672-present window
                Step("roll", "pace0", genv(TRANSPORT_OFF, "SR_HIGHFPS=0"), "plain", trace=True, **PACE_LONG),
                Step("roll", "pace1", "SR_HIGHFPS=0", "plain", trace=True, **PACE_LONG),
                Step("roll", "pace0b", genv(TRANSPORT_OFF, "SR_HIGHFPS=0"), "plain", trace=True, **PACE_LONG),
                Step("p2trace", "pace0_trace", ledger=[pacing_trace_item("tbk_pace0_trace")], base="pace0",
                     window=PACE_WINDOW),
                Step("p2trace", "pace1_trace", ledger=[pacing_trace_item("tbk_pace1_trace")], base="pace1",
                     window=PACE_WINDOW),
                Step("p2trace", "pace0b_trace", ledger=[pacing_trace_item("tbk_pace0b_trace")], base="pace0b",
                     window=PACE_WINDOW),
                Step("pacing", "pacing", base="pace0_trace", target="pace1_trace", calib="pace0b_trace",
                     ledger=[composed_pacing_item("tbk_pacing")]),
            ],
        },
        "segmentless_drained": {
            "prefix": "sgdr", "obligations": [], "gates": gs_pre,
            "about": "the DRAINED per-present A/B of segmentless (the orchestrator 2026-10-11; SR_AB_PRESENT_DRAINED: the flip "
                     "commits after a quiesced close, not inside VdSwap): the frozen-ab instrument the record drops voided "
                     "(abp_toggle_drops) - it counts only with NEVER-EXECUTED=0 and no NATIVE_FLUSH_DROP; [AB_DRAIN] waits reported",
            "steps": [
                Step("roll", "abp", gs_pre + ",SR_AB_TOGGLE=SR_NATIVE_SEGMENTLESS,SR_AB_PRESENT=1,SR_AB_PRESENT_DRAINED=1",
                     "freeze",
                     [ev("abp", "freeze_grade", "frozen-ab", "pixels", ab_present_min=1000, forbid=[r"\[NATIVE_FLUSH_DROP\]"],
                         note="the drained per-present toggle"),
                      segcounters_item("abp_seg"),
                      ev("abp_noexec", "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] periodic draws",
                         which="last", pattern=" NEVER-EXECUTED=0 ", note="no record dropped by the drained toggle"),
                      ev("abp_drain", "text", "live-observe", "generation", select=r"\[AB_DRAIN\] stats", which="last",
                         pattern=r"commits=[1-9]\d* ", note="the drain waits (information: postponed / overwritten counts)")]),
            ],
        },
        "segmentless_bake": {
            "prefix": "sgb", "obligations": ["p2.segmentless"], "gates": "", "bake": True,
            "about": "the segmentless BAKE proof (the orchestrator's go, 2026-10-11): on the baked exe with no env - replay "
                     "9/9 vs the pre-bake exe (--base), the new-game world-rule gate, the default-path meter (=0 vs the "
                     "default) against the census bound, and a SOAK: the 10-minute flight tape, no dumps, the P2 trace "
                     "(BlockUntilIdle) and the seg tripwires. The save-started gate runs from release/forcing's tools "
                     "(flicker_gate --from-save) beside this plan, recorded by hand. Since 2026-10-13 (Astra's re-bake "
                     "review) the gate and the soak carry the full strict witness (SR_NATIVE_SEG_TILE=1: the tile, the "
                     "first application, the close order across segments and the drained reconciliation at every "
                     "BlockUntilIdle) and the transport items come from these rolls",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("bake_replay")]),
                # v2 (the orchestrator 2026-10-11): the new-game gate carries SR_NATIVE_SEG_TILE=1 - the strict transport
                # witness's third roll (boot and the opening movie are emission paths the soak and the save gate lack)
                Step("gate", "gate", "SR_NATIVE_SEG_TILE=1",
                     ledger=[gate_item("bake_gate"), dict(tile_drained_item("bake_gate_tile"), roll="freeze"),
                             dict(tile_drained_item("bake_gate_span_tile"), roll="span"),
                             dict(xport_item("xport_newgame"), roll="freeze")]),
                # SR_NATIVE_SETCONSTMEM=0 on both arms since the setconst bake (default ON from 2026-10-14): its record
                # form exists only under segmentless, so it would add its own drop (const 6.9 a frame, scm4) to the ON
                # arm; the bound comes from the census sgc1, taken with setconst OFF. SR_NATIVE_FLUSH_SPLIT=0 likewise
                # since the flush-split bake (default ON from 2026-10-14; sgc1 predates it), and the nine transport kinds
                # =0 since the transport bake (default ON from 2026-10-15; sgc1 predates it too)
                Step("roll", "meter_off", genv("SR_NATIVE_SEGMENTLESS=0,SR_NATIVE_SETCONSTMEM=0,SR_NATIVE_FLUSH_SPLIT=0",
                                               TRANSPORT_OFF), "plain"),
                Step("roll", "meter_on", genv("SR_NATIVE_SETCONSTMEM=0,SR_NATIVE_FLUSH_SPLIT=0", TRANSPORT_OFF), "plain"),
                Step("meter", "meter", base="meter_off", target="meter_on", ledger=[meter_item(
                    "bake_meter", "meter_off", -58.4, "the default path after the bake against SR_NATIVE_SEGMENTLESS=0 "
                    "on the same exe; bound from the census sgc1 (predicted -61.5, less 5%)")]),
                Step("roll", "soak", "SR_NATIVE_SEG_TILE=1", "plain",
                     [census_item("soak_census"), segcounters_item("soak_seg"), tile_drained_item("soak_tile"),
                      xport_item("xport_soak")], trace=True,
                     play="tools/player/density_soak_play.txt", dwell=720, seconds=1500),
                Step("p2trace", "soak_trace", ledger=[trace_order_item("soak_trace")], base="soak"),
            ],
        },
        "present_gate": {
            "prefix": "pg", "obligations": ["p2.transport"], "gates": ggate,
            "about": "Defined complete present-gate emission with shared shader constants and strict segmentless "
                     "transport. Unit SDK recording-pin proof is separate; these port-diagnostic runs do not close "
                     "pacing, arbitrary GPU backpressure, whole-game fidelity, defaults or release composition.",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("gate_replay")]),
                Step("roll", "probe", ggate, "plain",
                     [census_item("gate_probe_census"), segcounters_item("gate_probe_seg"),
                      tile_item("gate_probe_tile"), present_gate_item("gate_probe_activity")]),
                Step("roll", "abp", ggate + ",SR_AB_TOGGLE=SR_NATIVE_GATE,SR_AB_PRESENT=1", "freeze",
                     [freeze_ab("gate_abp"), segcounters_item("gate_abp_seg"), tile_item("gate_abp_tile"),
                      present_gate_item("gate_abp_activity"),
                      ev("gate_abp_xport", "text", "live-observe", "generation",
                         select=r"\[NATIVE_FLUSH\] periodic draws", which="last",
                         pattern=" NEVER-EXECUTED=0 ", forbid=[r"\[NATIVE_FLUSH_DROP\]"])]),
                Step("roll", "cppm4", ggate + ",SR_NATIVE_FLUSH_CPPM4=1", "freeze",
                     [freeze_exact("gate_cppm4"), segcounters_item("gate_cppm4_seg"),
                      tile_item("gate_cppm4_tile"), present_gate_item("gate_cppm4_activity", pm4=True)]),
                Step("roll", "stress", ggate + ",SR_NATIVE_FLUSH_CPPM4=1," + STRESS, "freeze",
                     [freeze_exact("gate_stress"), segcounters_item("gate_stress_seg"),
                      tile_item("gate_stress_tile"), present_gate_item("gate_stress_activity", pm4=True)]),
                Step("gate", "gate", ggate, ledger=[gate_item("gate_world")]),
            ],
        },
        "swap_sdk": {
            "prefix": "sw", "obligations": ["p2.transport"], "gates": gswap,
            "about": "Default-OFF normal SDK swap with strict segmentless transport and shared shader constants. "
                     "Present-gate replacement stays OFF while its separate failed freeze remains unresolved. "
                     "The A/B toggles only the SDK body, keeping the reserved block active in both arms. "
                     "cppm4_sdk_off is the original-SDK control for reconstructed-path failures. "
                     "Sampled activity and pixel/transport diagnostics do not close whole SDK, pacing, "
                     "arbitrary backpressure, defaults, Native0, M2 or shipping composition.",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("swap_replay")]),
                Step("roll", "probe", gswap, "plain",
                     [census_item("swap_probe_census"), segcounters_item("swap_probe_seg"),
                      tile_item("swap_probe_tile")] + swap_sdk_items("swap_probe")),
                Step("roll", "abp", gswap + ",SR_AB_TOGGLE=SR_NATIVE_SWAP,SR_AB_PRESENT=1", "freeze",
                     [freeze_ab("swap_abp"), segcounters_item("swap_abp_seg"), tile_item("swap_abp_tile"),
                      ev("swap_abp_xport", "text", "live-observe", "generation",
                         select=r"\[NATIVE_FLUSH\] periodic draws", which="last",
                         pattern=" NEVER-EXECUTED=0 ", forbid=[r"\[NATIVE_FLUSH_DROP\]"])] +
                     swap_sdk_items("swap_abp")),
                Step("roll", "cppm4", gswap + ",SR_NATIVE_FLUSH_CPPM4=1", "freeze",
                     [freeze_exact("swap_cppm4"), segcounters_item("swap_cppm4_seg"),
                      tile_item("swap_cppm4_tile")] + swap_sdk_items("swap_cppm4", pm4=True)),
                Step("roll", "cppm4_sdk_off", gswap.replace("SR_NATIVE_SWAP=1", "SR_NATIVE_SWAP=0") +
                     ",SR_NATIVE_FLUSH_CPPM4=1", "freeze",
                     [freeze_exact("swap_cppm4_sdk_off"), segcounters_item("swap_cppm4_sdk_off_seg"),
                      tile_item("swap_cppm4_sdk_off_tile"),
                      swap_sdk_items("swap_cppm4_sdk_off", pm4=True)[1]]),
                Step("roll", "stress", gswap + ",SR_NATIVE_FLUSH_CPPM4=1," + STRESS, "freeze",
                     [freeze_exact("swap_stress"), segcounters_item("swap_stress_seg"),
                      tile_item("swap_stress_tile")] + swap_sdk_items("swap_stress", pm4=True)),
                Step("gate", "gate", gswap, ledger=[gate_item("swap_world")]),
            ],
        },
        "transport_pair": {
            "prefix": "tp", "obligations": ["p2.transport"], "gates": gpair,
            "about": "Default-OFF native CPU SDK submission/park pair under the actual shared Kf lock. "
                     "The software-list pump, deferred identity and arena lifetimes remain separate. "
                     "The control changes only SR_NATIVE_TRANSPORT; strict transport and SDK settings are shared. "
                     "The probe carries the P2 trace (close order, BlockUntilIdle drained: the pair replaces the "
                     "EndSync that drains dev+0x2A74); the frozen per-present A/B toggles only SR_NATIVE_TRANSPORT "
                     "(each call wholly native or wholly the SDK body). No meter: the pair emits no PM4 of its own. "
                     "No default cutover, broader fidelity, Native0, M2 or shipping composition acceptance.",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("transport_pair_replay")]),
                Step("roll", "probe", gpair, "plain",
                     [census_item("transport_pair_probe_census"), segcounters_item("transport_pair_probe_seg"),
                      tile_drained_item("transport_pair_probe_tile"), xport_item("transport_pair_probe_xport"),
                      transport_pair_item("transport_pair_probe_activity")] +
                     swap_sdk_items("transport_pair_probe"), trace=True),
                Step("p2trace", "probe_trace", ledger=[transport_pair_trace_item("transport_pair_probe_trace")],
                     base="probe"),
                # the frozen-pass per-present A/B: odd presents the SDK pair, even the native pair (the draw-path guard)
                Step("roll", "abp", gpair + ",SR_AB_TOGGLE=SR_NATIVE_TRANSPORT,SR_AB_PRESENT=1", "freeze",
                     [freeze_ab("transport_pair_abp"), segcounters_item("transport_pair_abp_seg"),
                      tile_item("transport_pair_abp_tile"), xport_item("transport_pair_abp_xport"),
                      transport_pair_ab_item("transport_pair_abp_activity")] + swap_sdk_items("transport_pair_abp")),
                Step("roll", "cppm4", gpair + ",SR_NATIVE_FLUSH_CPPM4=1", "freeze",
                     [freeze_exact("transport_pair_cppm4"), segcounters_item("transport_pair_cppm4_seg"),
                      tile_item("transport_pair_cppm4_tile"), transport_pair_item("transport_pair_cppm4_activity")] +
                     swap_sdk_items("transport_pair_cppm4", pm4=True)),
                Step("roll", "cppm4_off", gpair.replace("SR_NATIVE_TRANSPORT=1", "SR_NATIVE_TRANSPORT=0") +
                     ",SR_NATIVE_FLUSH_CPPM4=1", "freeze",
                     [freeze_exact("transport_pair_cppm4_off"), segcounters_item("transport_pair_cppm4_off_seg"),
                      tile_item("transport_pair_cppm4_off_tile")] + swap_sdk_items("transport_pair_cppm4_off", pm4=True)),
                Step("roll", "stress", gpair + ",SR_NATIVE_FLUSH_CPPM4=1," + STRESS, "freeze",
                     [freeze_exact("transport_pair_stress"), segcounters_item("transport_pair_stress_seg"),
                      tile_item("transport_pair_stress_tile"), transport_pair_item("transport_pair_stress_activity")] +
                     swap_sdk_items("transport_pair_stress", pm4=True)),
                # the stress roll's SR_NATIVE_TRANSPORT=0 control (2026-10-14, after tpl1's stress freezes 4 / 19 with
                # every transport witness clean): a stress failure the control shares is not the pair's
                Step("roll", "stress_off", gpair.replace("SR_NATIVE_TRANSPORT=1", "SR_NATIVE_TRANSPORT=0") +
                     ",SR_NATIVE_FLUSH_CPPM4=1," + STRESS, "freeze",
                     [freeze_exact("transport_pair_stress_off"), segcounters_item("transport_pair_stress_off_seg"),
                      tile_item("transport_pair_stress_off_tile")] +
                     swap_sdk_items("transport_pair_stress_off", pm4=True)),
                Step("gate", "gate", gpair, ledger=[gate_item("transport_pair_world")]),
            ],
        },
        "swap_sdk_profile": {
            "prefix": "swp", "obligations": [], "gates": gswap,
            "about": "Timing diagnosis only: serial SDK ON/OFF host profiles without frames. "
                     "Sampled stacks can perturb scheduling; no pixel, pacing or native acceptance.",
            "steps": [
                Step("roll", "sdk_on", gswap + ",SR_NATIVE_FLUSH_CPPM4=1,SR_HOST_PROFILE_HZ=50", "plain"),
                Step("roll", "sdk_off", gswap.replace("SR_NATIVE_SWAP=1", "SR_NATIVE_SWAP=0") +
                     ",SR_NATIVE_FLUSH_CPPM4=1,SR_HOST_PROFILE_HZ=50", "plain"),
            ],
        },
        "callback": {
            "prefix": "tcb", "obligations": ["p2.transport"], "gates": gcb,
            "about": "p2.transport step 1 (NATIVE_ROADMAP 'p2.transport BUILD PLAN' 1): GpuEmitCpuCallbackPackets 820FDA80's "
                     "CPU-callback block as a kRkCallback record the CP applies (scratch write-backs, the WAITs, INTERRUPT = "
                     "the guest ISR 820FDBA0), for the vblank gate's block in the open segment (the Z-pass kick's tail block "
                     "is BRACKET's: declined). Requires segmentless; its own per-present A/B",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("callback_replay_cb")]),
                Step("roll", "chk", "SR_NATIVE_FLUSH_CHECK=1", "plain", [chk_item("callback_chk"), cb_item("callback_chk_cb", chk=True)]),
                Step("roll", "probe", gcb, "plain",
                     [census_item("callback_probe_census"), segcounters_item("callback_probe_seg"), cb_item("callback_probe_cb")]),
                # the orchestrator 2026-10-11: one 820FDA80 call is entirely a record or entirely guest bytes (the gate and the
                # A/B mask read once per call); the abp counts only with the transport exact - SR_NATIVE_SEG_TILE=1, the tile
                # counters 0, NEVER-EXECUTED=0, no NATIVE_FLUSH_DROP
                Step("roll", "abp", gcb + ",SR_AB_TOGGLE=SR_NATIVE_CALLBACK,SR_AB_PRESENT=1,SR_NATIVE_SEG_TILE=1",
                     "freeze",
                     [freeze_ab("callback_abp"), segcounters_item("callback_abp_seg"), cb_item("callback_abp_cb"), tile_item("callback_abp_tile"),
                      ev("callback_abp_xport", "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] periodic draws",
                         which="last", pattern=" NEVER-EXECUTED=0 ", forbid=[r"\[NATIVE_FLUSH_DROP\]"])]),
                Step("roll", "cppm4", gcb + "," + PM4OUT, "freeze",
                     [freeze_exact("callback_cppm4"), segcounters_item("callback_cppm4_seg"), cb_item("callback_cppm4_cb", pm4=True)]),
                Step("gate", "gate", gcb, ledger=[gate_item("callback_gate")]),
            ],
        },
        "segmentless_strict": {
            "prefix": "sgs", "obligations": [], "gates": gs + ",SR_NATIVE_SEG_TILE=1",
            "about": "p2.segmentless v2's STRICT transport witness under pure ON (the orchestrator 2026-10-11): the soak "
                     "re-run (the 12-min flight tape, no dumps) with SR_NATIVE_SEG_TILE=1 - the producer tile check and the CP "
                     "first-application check must read gap / overlap / order / dup / reorder 0 with NEVER-EXECUTED=0 and no "
                     "drop. The natural save gate under ON (flicker_gate --from-save, the 0x29 hold) carries the same env and "
                     "is recorded by hand beside it",
            "steps": [
                Step("roll", "soak", gs + ",SR_NATIVE_SEG_TILE=1", "plain",
                     [census_item("soak_census"), segcounters_item("soak_seg"), tile_item("soak_tile"),
                      ev("soak_xport", "text", "live-observe", "generation", select=r"\[NATIVE_FLUSH\] periodic draws",
                         which="last", pattern=" NEVER-EXECUTED=0 ", forbid=[r"\[NATIVE_FLUSH_DROP\]"])],
                     play="tools/player/density_soak_play.txt", dwell=720, seconds=1500),
            ],
        },
        "devcreate_ctl": {
            "prefix": "dcc", "obligations": [], "gates": gs + ",SR_NATIVE_DEVCREATE=1",
            "about": "the control for dc1's boot H1 (the orchestrator 2026-10-11): on a quiet disk, in one hold, the devcreate "
                     "probe ON then the same probe OFF (segmentless only) - backend attach, first-120-presents render and "
                     "first-frame times compared. Run each step with --only (a hang must not skip the control)",
            "steps": [
                Step("roll", "on", gs + ",SR_NATIVE_DEVCREATE=1", "plain",
                     [census_item("on_census"), ring0_item("on_ring0"), devcreate_item("on_dc"), segcounters_item("on_seg")]),
                Step("roll", "off", gs_dc_off, "plain", [census_item("off_census"), segcounters_item("off_seg")]),
            ],
        },
        "devcreate": {
            # a superseded FAILING census comparison (census_r0F) is kept as a witness of the r0F contradiction
            "supersede_into": {"census_r0F": "r0F_second_producer"},
            # the r0F contradiction's resolver takes the applied census_r0F's run-unique JSON (its name is the run's)
            "resolver_paths": {"census_r0F": "r0F_second_producer.reroll_r0F"},
            "prefix": "dc", "obligations": ["boot.device_create"], "gates": gs + ",SR_NATIVE_DEVCREATE=1",
            "about": "boot.device_create: the device-create producers (820FDE90, 8210E520, 82110EB8, 820F0048) as host code under segmentless",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("replay_dc")]),
                # the dry CHECK of the two emitter ports (kinds 8 / 9) against the guest bytes
                Step("roll", "chk", "SR_NATIVE_FLUSH_CHECK=1,SR_NATIVE_DEVCREATE=1", "plain", [chk_item("chk")]),
                # the boot: no ring packet parsed, the ports' counters, the transport's tripwires
                Step("roll", "probe", gs + ",SR_NATIVE_DEVCREATE=1", "plain",
                     [census_item("probe_census"), ring0_item("probe_ring0"), devcreate_item("probe_dc"), segcounters_item("probe_seg")]),
                # the per-frame half (820F0048 at Swap): the frozen-pass A/B against the bridge's literal PM4
                Step("roll", "abp", gs + ",SR_NATIVE_DEVCREATE=1,SR_AB_TOGGLE=SR_NATIVE_DEVCREATE,SR_AB_PRESENT=1", "freeze",
                     [freeze_ab("abp"), segcounters_item("abp_seg")]),
                Step("roll", "cppm4", gs + ",SR_NATIVE_DEVCREATE=1," + PM4OUT, "freeze",
                     [freeze_exact("cppm4"), segcounters_item("cppm4_seg")]),
                Step("roll", "meter0", gs_dc_off, "plain"),
                Step("roll", "meter1", gs + ",SR_NATIVE_DEVCREATE=1", "plain"),
                # the producer's own kind (the 2026-10-11 rule), pre-registered 2026-10-14 after dc3 missed the total
                # bound (-2.28 vs <= -3: bin 4.0 -> 0.1 as on dc2, t2nop +1.9 where dc2 had -0.7); defaults of kind_zero
                Step("meter", "meter", base="meter0", target="meter1", ledger=[kind_zero_item(
                    "meter_bin", "meter0", "bin", 3.0, note="820F0048's packets at Swap by kind: the base census (meter0) "
                    "shows them, meter1 none; the total pm4 change is information (dc2 -5.2, dc3 -2.28)")]),
                # dcb (2026-10-14; ported to the merged PRODUCER census, dev 4f6c7972): the producers' own TYPE-0
                # packets, which the kind meter cannot isolate (t0 carries ~140 per ingame frame from other producers and
                # moves 1-3 between same-config rolls). Read from the `inner=` lines (gfx/pm4_coverage: Stamp::inner).
                # 820F0048 writes SET_BIN_SELECT_HI (opcode 63) and RB_BC_CONTROL (page r0F) once per call. It is no
                # table scope: its per-frame call's packets are Swap's (inner=82112050); its boot call (82106010) and
                # everything else D3DDeviceCreate 820F34C0 writes are UNSTAMPED (that bracket opens before the device
                # exists). rcen1 census0 (exe 6705f9e06683 = dev 6528ffef): 63 only on Swap's line (= its frames) and
                # UNSTAMPED's 63:1; UNSTAMPED r0F:2 - the boot call's and 82103238's (called from 821058B0: WAIT_UNTIL +
                # RB_BC_CONTROL; dc5c census0's first line r0F:2 with 63:1). So census1 forbids 63 on every producer
                # line, and r0F gets only an AGGREGATE RESIDUAL CONSISTENCY CHECK (census_cmp: census1 r0F == census0
                # r0F - census0 63, whole census and UNSTAMPED alone): consistent with 820F0048's r0F gone and the
                # residual unchanged, if the residual is equal in both runs; it does not name census1's r0F producer. No
                # producer witness: a table row for 82103238 / 821058B0 cuts nothing inside the device-less bracket (see
                # owner_compare_item). Statically (IDA) 82103238's only caller 821058B0 runs once per title-device
                # creation, from either arm's 82106010 (no reset path; the flags-2 recorder 82304C38 is dead).
                # 8210E520's VC_FIFO_DEPTHS (r0E) and PA_CL_ENHANCE (r0C) once per run (UNSTAMPED, boot). Every key is
                # required on its own in census0 (one line, AND) and none on any producer line in census1. Its WAIT_UNTIL
                # (r05) shares a page with ~7 per frame from other producers: it rides in the same entry as
                # RB_BC_CONTROL, byte-checked by the dry CHECK (chk).
                # THE INTERVAL (Astra round 3): the producer lines are periodic (every 256 frames or 20 s) with no final
                # report - the harness kills the game - so every producer-line claim holds OVER THE CENSUS-COVERED
                # INTERVAL (up to the last report; rcen1's tails: 155 and 207 frames). census_cmp bounds it (each last
                # report within one period of the run's end; the per-frame [PM4_COV] F lines summing to the census over
                # the covered frames) and checks census1's tail frames for 63 and r0F; census1_frames_* read 63 and r0E /
                # r0C on EVERY completed frame's F line (unattributed: no other producer writes them in the census).
                Step("roll", "census0", gs_dc_off + ",SR_NATIVE_BRACKETS=1", "plain",
                     [producer_all_keys_item("census0_segbegin", "00000000", ["r0F", "63"], name="UNSTAMPED", note=(
                         "over the census-covered interval: 820F0048's boot call (82106010, inside the device-less "
                         "D3DDeviceCreate bracket) - RB_BC_CONTROL / SET_BIN_SELECT_HI as guest PM4, unstamped")),
                      producer_all_keys_item("census0_swap", "82112050", ["r0F", "63"], note=(
                          "over the census-covered interval: 820F0048's per-frame call from Swap - RB_BC_CONTROL / "
                          "SET_BIN_SELECT_HI as guest PM4 on Swap's producer line (820F0048 is no table scope)")),
                      producer_all_keys_item("census0_reset", "00000000", ["r0E", "r0C"], name="UNSTAMPED", note=(
                          "over the census-covered interval: 8210E520's VC_FIFO_DEPTHS / PA_CL_ENHANCE as guest PM4 "
                          "(boot, unstamped)"))]),
                Step("roll", "census1", gs + ",SR_NATIVE_DEVCREATE=1,SR_NATIVE_BRACKETS=1", "plain",
                     [producer_census_item("census1_op63", [ANY_PRODUCER], ["63"], False,
                                           "over the census-covered interval: 820F0048's SET_BIN_SELECT_HI on no "
                                           "producer line - its boot and per-frame calls both gone (rcen1 census0: 63 "
                                           "only on Swap's line and UNSTAMPED's 63:1)"),
                      producer_census_item("census1_reset", [ANY_PRODUCER], ["r0E", "r0C"], False,
                                           "over the census-covered interval: 8210E520's VC_FIFO_DEPTHS / PA_CL_ENHANCE "
                                           "on no producer line (rcen1 census0: UNSTAMPED's r0E:1 r0C:1 the only ones)"),
                      frames_forbid_item("census1_frames_op63", ops=["63"], note=(
                          "every completed frame, the tail after the last census report included (unattributed F lines): "
                          "no SET_BIN_SELECT_HI - 820F0048 its only writer in the census")),
                      frames_forbid_item("census1_frames_reset", regs=["0E", "0C"], note=(
                          "every completed frame, the tail included (unattributed F lines): no VC_FIFO_DEPTHS / "
                          "PA_CL_ENHANCE page - 8210E520 their only writer in the census")),
                      devcreate_item("census1_dc")]),
                Step("owner", "census_cmp", base="census0", target="census1", ledger=[owner_compare_item(
                    "census_r0F", "over the census-covered interval, an aggregate residual consistency check, not a "
                    "producer witness: census1's r0F equals census0's r0F minus census0's opcode 63, over the whole "
                    "producer census and on the UNSTAMPED line alone (consistent with 820F0048's RB_BC_CONTROL gone and "
                    "the residual unchanged, if that residual is equal in both runs; statically the residual's producer "
                    "82103238 has one caller, 821058B0, run once per title-device creation in either arm - no reset "
                    "path, the flags-2 recorder 82304C38 dead; no table row can stamp it inside the device-less "
                    "D3DDeviceCreate bracket); each roll's last report within one period of its end, the F lines "
                    "matching the census, census1's tail frames without 63 or r0F")]),
                Step("gate", "gate", gs + ",SR_NATIVE_DEVCREATE=1", ledger=[gate_item("gate")]),
            ],
        },
        "devcreate_close": {
            # Pre-registered 2026-10-14 (native/devcreate-close; docs/NATIVE_ROADMAP.md boot.device_create CLOSE DESIGN):
            # the two items boot.device_create still owes, on the current exe, staged (--stage J:\sr_stage).
            "prefix": "dcl", "obligations": ["boot.device_create"], "gates": gs + ",SR_NATIVE_DEVCREATE=1",
            # Astra round 1 (P1-4): the rolls are evidence the ledger cites by path - a live run writes only into a
            # FRESH --out (no proof JSON, no roll folder of this plan there), so no later attempt overwrites them
            "fresh_out": True,
            "no_replay": "the Xenia-trace replay runs no guest code, so it cannot exercise device creation; replay_dc "
                         "(exe 42f048a64456) stands and the ledger has no exe-currency rule",
            "no_ab": "the per-frame half's frozen-pass A/B is the devcreate plan's (dc3 abp, exe 42f048a64456); this plan "
                     "re-measures only the meter and the boot",
            # boot_equiv's JSON is the unknown's resolver on every outcome (a bound run that fails moves into the
            # unknown's witnesses before the next is bound) - it is not an evidence item
            "resolver_bind": {"boot_equiv": ("boot_order_gap", "boot_order.powered"),
                              "boot_stall": ("boot_order_gap", "boot_order.stall")},
            # meter_pool lands in the evidence on every outcome; only when the ledger's own grader passes it does the
            # single-pair `meter` move into contradiction meter_single_pair (witness, expect fail; resolved_by
            # meter_pool) - the gate_v1_superseded pattern. A superseded failing meter_pool joins the same contradiction.
            "retire_on_pass": [{
                "item": "meter_pool", "old": "meter", "into": "meter_single_pair",
                "text": "dc3 (2026-10-14, exe 42f048a64456): the single-pair total meter missed its bound - pm4 per "
                        "ingame frame 373.82 -> 371.54, delta -2.28 vs <= -3 - while 820F0048's own kind fell 4.0 -> 0.1 "
                        "(meter_bin, dc3 and dc4). The other kinds' run-to-run spread (t2nop +1.9 on dc3, -0.7 on dc2; "
                        "the total's pair-to-pair SD ~1.5 per frame over sgb5's four pairs and dc2 / dc3 / dc4) decides a "
                        "single-pair total bound. Re-measured as the pre-registered mean of the per-pair deltas of four "
                        "alternating pairs on one exe against the same bound (plan devcreate_close, docs/NATIVE_ROADMAP.md "
                        "boot.device_create CLOSE DESIGN). The single pair and any failed pooled run are kept here as "
                        "witnesses; resolved by a passing pooled run"}],
            "supersede_into": {"meter_pool": "meter_single_pair"},
            "about": "boot.device_create's two owed items on the current exe, staged (docs/NATIVE_ROADMAP.md boot."
                     "device_create CLOSE DESIGN, fixed before any roll; revised after Astra round 1): a declared "
                     "warm-up roll (the OFF arm = the default build; run, recorded, never graded), eight alternating "
                     "SR_NATIVE_DEVCREATE ON / OFF boot pairs (b1 ON/OFF, b2 OFF/ON, ...; devcreate_ctl's arms, no "
                     "dumps) graded by tools/perf/boot_equiv.py (design dcl-v2, the MEAN criterion: EQUIVALENT with "
                     "the tag BOOT_EQUIV / OPEN / INCONCLUSIVE; its run-unique JSON binds the boot_order_gap resolver "
                     "boot_order.powered on every outcome; it runs even after a roll ended the plan), four alternating "
                     "meter pairs (m1 OFF/ON, m2 ON/OFF, ...; dc3's arms, sparse dumps kept for the world rule) graded "
                     "by the ledger's own grader as the MEAN OF THE PER-PAIR DELTAS against the kept bound -3, every "
                     "roll >= 8,000 ingame frames, one exe, both arms engaged, the declared alternation (meter_pool; "
                     "written on every outcome once a meter roll ran, with a run-unique receipt of each roll's files; "
                     "on its PASS the single-pair meter moves into contradiction meter_single_pair), then %d more ON "
                     "boots and tools/perf/boot_stall.py over all %d ON boots of the run (design %s, the "
                     "INTERMITTENT-STALL criterion: BOUNDED with the tag BOOT_STALL / OPEN / INCONCLUSIVE; binds the "
                     "second resolver boot_order.stall on every outcome; runs even after a roll ended the plan). No "
                     "replay and no gate: the ledger owes no re-proof on the current exe. A fresh --out per run (its "
                     "rolls are never overwritten). Run it staged: --stage J:\\sr_stage" % (
                         BOOT_STALL_EXTRA, BOOT_STALL_N_MIN, BOOT_STALL_DESIGN),
            "steps": (
                # the declared warm-up: the OFF arm's configuration (the default build of 2026-10-14; =0 since the bake),
                # before pair 1, never graded
                [Step("roll", "warmup", gs_dc_off, "plain")] +
                dcl_boot +
                [Step("bootequiv", "boot_equiv", pairs=dcl_boot_pairs, warmup="warmup", always=True,
                      ledger=[boot_equiv_item("boot_equiv", "BOOT_EQUIV design %s over the warm-up and %d alternating "
                                              "SR_NATIVE_DEVCREATE ON / OFF boot pairs (staged, one exe)" % (
                                                  BOOT_EQUIV_DESIGN, BOOT_EQUIV_PAIRS))])] +
                dcl_meter +
                [Step("meter", "meter_pool", base=dcl_mpairs[0][0], target=dcl_mpairs[0][1], pool=dcl_mpairs[1:],
                      always=True, ledger=[pooled_meter_item(
                          "meter_pool", -3, 8000, "820F0048's 6 packets per frame at Swap (4 bin + 2 t0): the total pm4 "
                          "per ingame frame, the mean of the per-pair deltas of four alternating pairs (m1 OFF/ON, m2 "
                          "ON/OFF, m3 OFF/ON, m4 ON/OFF; SR_NATIVE_SEGMENTLESS=1 vs +SR_NATIVE_DEVCREATE=1, dc3's arms) "
                          "on one exe against dc3's bound -3, kept; pre-registered 2026-10-14 (docs/NATIVE_ROADMAP.md "
                          "CLOSE DESIGN: pair SD ~1.5 per frame, SE ~0.75 over four pairs; no pair dropped)",
                          provenance={"arm_key": "SR_NATIVE_DEVCREATE", "arm_on": "1", "engaged": DEVCREATE_ENGAGED,
                                      "first": "base", "alternate": True,
                                      "ignore": ["SR_PERF_DIR", "SR_CONTENT_DIR", "SR_PRESENT_DUMP_DIR"]})])] +
                dcl_stall +
                [Step("bootstall", "boot_stall", pool=dcl_on_tags, always=True,
                      ledger=[boot_stall_item("boot_stall", "BOOT_STALL design %s over every SR_NATIVE_DEVCREATE=1 boot "
                                              "of the run (8 boot pairs' ON + 4 meter pairs' ON + %d; staged, one exe)" % (
                                                  BOOT_STALL_DESIGN, BOOT_STALL_EXTRA))])]),
        },
        "zpd": {
            # p2.zpd (native/zpd, 2026-10-14; docs/NATIVE_ROADMAP.md P2.12): the HUD's glow occlusion queries answered
            # by host D3D12 occlusion queries (SR_NATIVE_ZPD, default OFF: 1 strict, 2 the latency model).
            "prefix": "zpd", "obligations": ["p2.zpd"], "gates": "SR_NATIVE_ZPD=1",
            "no_ab": "SR_NATIVE_ZPD changes guest-visible answers (the query slot words GetData reads), so by "
                     "kernel/ab_toggle.hpp's rule it is not an in-roll A/B feature; the draw-path guard is the exact "
                     "frozen presents of a save-started roll WITH queries firing (freeze, cppm4) and the flicker gate",
            "about": "p2.zpd (docs/NATIVE_ROADMAP.md P2.12): every roll starts from the chapter-12 seed (" + ZPD_SEED + ": "
                     "Metropolis after Bizarro 1, metro events and BIZobj15's marker; zpd6 - zpd2..zpd5 used chapter 4) "
                     "and plays " + ZPD_TAPE + " so far threat markers stay on screen and HudGlowOcclusionQuery_82304058 "
                     "queries. Every roll's precondition: the game issued a query ([GPU_CP] census zpd > 0); a roll "
                     "without one is VOID and repeated once, a second one is NO DATA (never PASS; its items grade NO "
                     "DATA in the ledger and resolve nothing). The replay (no queries in the Xenia traces) shows "
                     "the gate changes nothing else; `off` (the default exe path) shows the scene fires EVENT_WRITE_ZPD "
                     "([GPU_CP] zpd > 0: the never_fired unknown); `on` (strict) grades the final census: every ended "
                     "query resolved and written, late 0 (the slot words before the covering fence), no flush failure "
                     "and no strict fallback, nothing lost / dropped / evicted / pool-exhausted, every measured draw on "
                     "a host segment (segs > 0, missed 0), and resolved == ended on the last line with nothing in "
                     "flight (the census's spp1/2/4 record the HUD query's MSAA mode); `lat` (the latency model) records how late the answers come (late > 0 by "
                     "design); `freeze` and `cppm4` hold the frozen presents exact with queries and partial frames "
                     "running (the world rule), and the flicker gate runs save-started. The Xenia pixel pair (the "
                     "ledger's guard: a threat marker's glow in view vs behind a building, Xenia occlusion_query=strict "
                     "at the same scene) is the orchestrator's, recorded by hand.",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("zpd_replay")]),
                Step("roll", "off", "", "plain",
                     [ev("zpd_off_fires", "text", "live-observe", "generation", select=r"\[(?:GPU_CP|NATIVE_SUBMIT)\] census rptr=",
                         which="last", pattern=r" zpd=[1-9]\d* \(fake 0\) ext=", require=ZPD_FIRED,
                         note="the seed's flight fires EVENT_WRITE_ZPD on the default path (today: no write-back)")],
                     play=ZPD_TAPE, dwell=260, seconds=900, from_save=ZPD_SEED, require=ZPD_FIRED),
                Step("roll", "on", "SR_NATIVE_ZPD=1", "plain",
                     [zpd_item("zpd_strict", "strict",
                               r"late=0 stale=\d+ orphan=0 abandoned=\d+ evicted=0 dropped=0 lost=0 badAddr=0 covered=0 "
                               r"fallback=0 superseded=0 poolFull=0 missed=0 overSegs=0 flushes=[1-9]\d* flushFail=0 "
                               r"waitMs=\S+ maxWaitMs=\S+ segs=[1-9]\d* ",
                               note="strict: queries resolved by host D3D12 occlusion queries over the guest draws "
                                    "they bracket (segs > 0), written before the covering fence (late 0) by the "
                                    "partial frame itself (no strict fallback, no flush failure); the census's "
                                    "spp1= / spp2= / spp4= and the [NATIVE_ZPD_Q] lines' spp= record the HUD "
                                    "query's sample mode (Astra round 1 P2)"),
                      zpd_balance_item("zpd_strict_balance", "strict"),
                      zpd_item("zpd_probe", "strict",
                               r".* zero=\d+ nonzero=\d+ .* probes=[1-9]\d* depthKilled=\d+ psKilled=\d+ noRaster=\d+ "
                               r"lagAvgMs=\S+ lagMaxMs=\S+",
                               note="the zpd4 round's visibility probe ran (probes > 0): of the queries answering 0, "
                                    "depthKilled = the depth / stencil test removed every sample (the ref draw, depth "
                                    "ALWAYS, counted it), psKilled = the pixel shader discarded them (only the raster "
                                    "draw without a pixel shader counted), noRaster = the draw covered no sample; the "
                                    "[NATIVE_ZPD] draw #n lines hold the first 16 query draws' state, the [NATIVE_ZPD_Q] "
                                    "lines ref= / raster= per query")],
                     play=ZPD_TAPE, dwell=260, seconds=900, from_save=ZPD_SEED, require=ZPD_FIRED),
                Step("roll", "lat", "SR_NATIVE_ZPD=2", "plain",
                     [zpd_item("zpd_latency", "latency", r"late=[1-9]\d* .* lost=0 .* poolFull=0 ",
                               note="the latency model: answered after the covering fence (late > 0 by design)"),
                      zpd_item("zpd_latency_lag", "latency", r".* waitMs=0\.0 .* lagAvgMs=\S+ lagMaxMs=\S+",
                               note="the latency model's cost: the CP never waits (waitMs 0.0); its answers come "
                                    "lagAvgMs / lagMaxMs after the CP parsed the END (the strict roll's lag is its "
                                    "flush wait)")],
                     play=ZPD_TAPE, dwell=260, seconds=900, from_save=ZPD_SEED, require=ZPD_FIRED),
                # the frozen presents guard the draw path WITH queries firing (no_ab): a freeze roll whose game issued
                # none is NO DATA for them too
                Step("roll", "freeze", "SR_NATIVE_ZPD=1", "freeze",
                     [dict(freeze_exact("zpd_freeze"), require=ZPD_FIRED),
                      zpd_item("zpd_freeze_census", "strict", r"late=0 .* lost=0 .* flushFail=0 ")],
                     play=ZPD_TAPE, dwell=260, seconds=900, from_save=ZPD_SEED, require=ZPD_FIRED),
                Step("roll", "cppm4", "SR_NATIVE_ZPD=1," + CPPM4, "freeze",
                     [dict(freeze_exact("zpd_cppm4"), require=ZPD_FIRED),
                      zpd_item("zpd_cppm4_census", "strict", r"late=0 .* lost=0 .* flushFail=0 ")],
                     play=ZPD_TAPE, dwell=260, seconds=900, from_save=ZPD_SEED, require=ZPD_FIRED),
                # Astra merge review P2 (2026-10-15): the gate's two rolls play the zpd tape (flicker_gate --tape; it
                # carries the span's `mark dump`) and BOTH must issue queries - zpd6's gate freeze roll issued none on
                # the gate tape - else VOID / re-run once / NO DATA (do_gate), and its item grades NO DATA
                Step("gate", "gate", "SR_NATIVE_ZPD=1", ledger=[dict(gate_item("zpd_gate"), require=ZPD_FIRED)],
                     from_save=ZPD_SEED, play=ZPD_TAPE, require=ZPD_FIRED),
            ],
            "resolves": [("p2.zpd", "never_fired", ["zpd_off_fires", "zpd_strict"])],
        },
        "zpd_xenia": {
            # p2.zpd zpd7 (docs/NATIVE_ROADMAP.md P2.12 item 14): the ledger's guard `pixels >= xenia` - our occlusion
            # answers against Xenia's on the same frames.
            "prefix": "zpdx", "obligations": ["p2.zpd"], "gates": "SR_NATIVE_ZPD=1",
            "no_replay": "the replay's Xenia traces hold no query (the zpd plan's replay covers the gate); this plan's "
                         "frames are traced live and played by Xenia itself",
            "no_ab": "a Xenia comparison, not a draw-path change: the zpd plan's freeze / cppm4 / gate rolls guard the "
                     "draw path",
            "about": "p2.zpd's Xenia oracle: one live roll from the seed-12 save (" + ZPD_SEED + ", " + ZPD_TAPE + ", "
                     "the [GPU_CP] zpd>0 precondition) with SR_NATIVE_ZPD=1 and the trace hotkey armed "
                     "(SR_PM4_TRACE_HOTKEY=1: the render thread stays off and the native executor is off - the queries' "
                     "EDRAM / depth path is the production one) records %d traces of %d consecutive frames each, from "
                     "the frame after a HUD glow query (SR_PM4_TRACE_ZPD / SR_PM4_TRACE_FRAMES, at least %d swaps "
                     "apart: SR_PM4_TRACE_ZPD_GAP; %d s of play after the HUD) as .xtr in <roll>_xtr and logs each of "
                     "their queries ([NATIVE_ZPD_T]). Xenia's own "
                     "trace-dump (tools/xenia-trace/build_zpd_trace_dump.ps1: xenia-canary 0bd090dbe, "
                     "--occlusion_query=strict: real host occlusion queries over Xenia's EDRAM model; the oracle patch "
                     "plays every frame of a trace) plays each trace and reports every EVENT_WRITE_ZPD (SR_ZPD_REPORT); "
                     "tools/perf/zpd_xenia.py pairs each "
                     "END with our answer for the same query (frame, order, slot). PASS: >= 20 pairs, none unpaired, "
                     ">= 98%% agree on visible (> 0) / occluded (0) - the distinction the glow reads - and the dump "
                     "calibrated: its render of Xenia's own golden trace 454107ED_25023 within 1%% of the golden PNG "
                     "(WARP drew it without Superman or the street, 39%% off: the xenia step needs the GPU)." % (
                         ZPD_TRACES, ZPD_TRACE_FRAMES, ZPD_TRACE_GAP, ZPD_TRACE_DWELL),
            "steps": [
                Step("roll", "xtrace", "SR_NATIVE_ZPD=1,SR_PM4_TRACE_HOTKEY=1,SR_PM4_TRACE_ZPD=%d,SR_PM4_TRACE_FRAMES=%d,"
                     "SR_PM4_TRACE_ZPD_GAP=%d" % (ZPD_TRACES, ZPD_TRACE_FRAMES, ZPD_TRACE_GAP), "plain",
                     [], play=ZPD_TAPE, dwell=ZPD_TRACE_DWELL, seconds=1200, from_save=ZPD_SEED, require=ZPD_FIRED,
                     xtr=True),
                Step("xenia_zpd", "xenia", base="xtrace", ledger=[zpd_xenia_item(
                    "zpd_xenia", note="the console question (zpd5 / zpd6: depthKilled ~90 %): Xenia's strict "
                                      "occlusion queries on the frames we traced answer as ours - visible / occluded "
                                      "per END, paired by frame, order and slot")]),
            ],
        },
        "zpd_bake": {
            # p2.zpd BAKED (native/zpd-bake, 2026-10-16; NATIVE_ROADMAP "p2.zpd BAKE PLAN"): SR_NATIVE_ZPD default ON in
            # the latency model, GpuEmitTileSurfaceConfig_82101E08's packets as records (EVENT_WRITE_ZPD as kRkZpd, the
            # one implementation RecordEngine::applyZpdEvent); =0 keeps the guest body and the guest path (no host query
            # - the A/B arm), an explicit =1 stays strict. Why now: the stage-1 lane (release/emu-stage1) found the HUD
            # glow query 82304058 firing as guest PM4 in Metropolis chapters 3-18 on the default build, and the release
            # build makes a literal-PM4 gap fatal. Every item on ONE exe (same_exe: each roll stamped, each comparison
            # refusing another exe's input) into a fresh --out (fresh_out); a live run refuses while the parent
            # environment carries SR_NATIVE_ZPD (default_keys: perf_run passes the parent env to every roll, so a default
            # arm would run an explicit form). THE STRICT WITNESS in every roll: every default roll its DEFAULT form
            # (zpd_default_item: the latch's line, the explicit and off lines forbidden) and the latency census with the
            # port (zpd_bake_item: written, nothing lost, no wait, port > 0, portDecl 0 - the A/B both arms); every =0 roll
            # the off line with no census (zpd_off_item); the gate, A/B and CPPM4 rolls also the drained transport tile and
            # the reclaim audit (SR_NATIVE_SEG_TILE=1; not in the census, meter and pacing arms, as the transport bake's).
            # zbk1 proves the REAL default build (the orchestrator, 2026-10-16): dev's display-registers bake (e87d89c3,
            # SR_NATIVE_VBLANK default ON) is merged in, so every arm - the =0 arms too - runs SR_NATIVE_VBLANK unset
            # ("vblank_default": no VBLANK_OFF pin), the two A/B arms differing in SR_NATIVE_ZPD only; and a parent env
            # carrying either key is refused (an inherited SR_NATIVE_VBLANK=0 would un-bake the other default silently).
            "prefix": "zbk", "obligations": ["p2.zpd"], "gates": "", "bake": True, "same_exe": True, "fresh_out": True,
            "default_keys": ("SR_NATIVE_ZPD", "SR_NATIVE_VBLANK"),
            "zpd_default": True,   # zpd_gates: its arms as written (the default arms leave SR_NATIVE_ZPD unset)
            "vblank_default": True,   # run_gates: no SR_NATIVE_VBLANK=0 pin - the display-registers bake's default
            # --recompare re-grades the meter too (Astra round 1 P2-4: the ledger's unrounded check, not the transcript)
            "recompare_extra": ("meter",),
            "resolves": [("p2.zpd", "default_bake", zbk_bake_ids), ("p2.zpd", "default_pacing", zbk_pace_ids)],
            "about": "p2.zpd BAKED (SR_NATIVE_ZPD default ON in the latency model since native/zpd-bake; =0 keeps "
                     "82101E08's guest body and the guest path, no host query; =1 strict) on the baked exe with no env, "
                     "every roll from the chapter-12 seed (" + ZPD_SEED + ", " + ZPD_TAPE + ": Metropolis, the HUD glow "
                     "queries firing; the [GPU_CP] zpd > 0 precondition, VOID re-run once, then NO DATA): replay 9/9 vs "
                     "the pre-bake exe (--base); the world-rule flicker gate save-started, the strict witnesses in both "
                     "rolls; the dry CHECK of the port (kind 11) against the guest bytes; THE PROBE - the default with the "
                     "producer census against its =0 control, transport_census.py --pm4-zero --base-shows zpd: the =0 "
                     "arm shows the queries as guest PM4 (EVENT_WRITE_ZPD on D3DQueryIssueBeginEnd 82102BC8's line), the "
                     "default holds NO PM4 "
                     "in gameplay with no allowance (the queries as kRkZpd records, no literal-PM4 gap); the frozen-pass "
                     "per-present A/B of the default against =0's producer (SR_AB_TOGGLE=SR_NATIVE_ZPD: the port per "
                     "present, the answering native in both arms - an answer A/B is no per-present feature); CPPM4 on the "
                     "default; the meter =0 vs the default (the default <= 0.05 PM4 per ingame frame, delta <= 0); pacing "
                     "on the console path, the default against =0 with a second =0 control as the calibration "
                     "(pacing_cmp.py, the 28,672-present window). Every default roll its default form and the latency "
                     "census with the port (portDecl 0; the A/B > 0) and every answer measured (segs > 0, missed 0, "
                     "overSegs 0), every =0 roll the off witness; the queries INSIDE the graded windows "
                     "(tools/perf/zpd_activity.py: pacing's 28,672-present window, each quarter active; the A/B's and "
                     "CPPM4's compared presents - the ones the pixel grade compared - each by its own frame stamp "
                     "(Astra round 2), >= 1,000 query-bearing, the A/B's arms >= 250 each; the frozen rolls' holds "
                     "every 4 s from ~HUD+40 s); the meter graded on the unrounded values (the ledger's check), NO DATA "
                     "when either of its rolls is. Every arm on one exe "
                     "and a fresh --out, enforced; every arm on the real default build (SR_NATIVE_VBLANK's bake default "
                     "too: no =0 pin); a live run refuses while the parent environment sets SR_NATIVE_ZPD or "
                     "SR_NATIVE_VBLANK. The offline proof (difftest --zpd-port-test and --native-zpd-test with their "
                     "mutants, every suite vs dev) is separate; the ledger's pixels >= xenia guard is zpdx4's",
            "steps": zbk_steps,
        },
        "zpd_park": {
            # THE PARKED WORKLOAD's information set (zbk2's follow-up; zpk_steps above): no obligation, nothing applied
            # (a diagnostic plan, as row3_ctrl). The arms run as zpd_bake's do (zpd_default / vblank_default: the
            # default arms set neither key), one exe, a fresh --out, the parent env clean.
            "prefix": "zpk", "obligations": [], "gates": "", "same_exe": True, "fresh_out": True,
            "default_keys": ("SR_NATIVE_ZPD", "SR_NATIVE_VBLANK"), "zpd_default": True, "vblank_default": True,
            "about": "p2.zpd's parked workload, information before any bake roll uses it: the chapter-18 seed (" +
                     ZPD_PARK_SEED + ") with nothing pressed after the HUD (" + ZPD_PARK_TAPE + "), Superman at the "
                     "Metropolis spawn with its fixed view, a far threat marker in it (the seed library's only query "
                     "before any input). zbk2's three failed / NO DATA witnesses on rolls configured as zbk2's: "
                     "park_pace1 (pace1: the console path, its P2 trace, zpd_activity pacing over the 28,672-present "
                     "window, each quarter active), park_abp (abp: the frozen-pass per-present A/B, zpd_activity "
                     "freeze_ab, >= 1,000 query-bearing compared presents) and park_off (meter_off: the =0 arm's "
                     "precondition [GPU_CP] zpd > 0). The bake's own strict witnesses ride along on each roll",
            "steps": zpk_steps,
        },
        "zpd_keep": {
            # THE KEEP-ALIVE's information set (zpk2; zpk2_steps above): no obligation, nothing applied, the arms as
            # zpd_bake's (zpd_default / vblank_default), one exe, a fresh --out, the parent env clean.
            "prefix": "zpk2", "obligations": [], "gates": "", "same_exe": True, "fresh_out": True,
            "default_keys": ("SR_NATIVE_ZPD", "SR_NATIVE_VBLANK"), "zpd_default": True, "vblank_default": True,
            "about": "p2.zpd's parked workload WITH A KEEP-ALIVE, information before any bake roll uses it: the "
                     "chapter-18 seed (" + ZPD_PARK_SEED + ") at the Metropolis spawn, its view holding a far threat "
                     "marker, and a D-pad up tap every 2.5 s through the harness test keymap (" + ZPD_KEEP_TAPE + ") so "
                     "the game's idle camera - which hid the HUD from ~HUD+20 s in every zpk1 roll - never engages. "
                     "zpk1's three rolls and grades: keep_pace1 (pace1: the console path, its P2 trace, zpd_activity "
                     "pacing over the 28,672-present window, each quarter active), keep_abp (zbk3's abp: the frozen-pass "
                     "per-present A/B with 80 dumps a hold, zpd_activity freeze_ab - the compared presents by their own "
                     "frame stamps, >= 1,000 query-bearing, each arm >= 250) and keep_off (meter_off: the =0 arm's "
                     "precondition [GPU_CP] zpd > 0). The bake's own strict witnesses ride along on each roll",
            "steps": zpk2_steps,
        },
        "devcreate_bake": {
            # boot.device_create BAKED (native/devcreate-fix, 2026-10-15; docs/NATIVE_ROADMAP.md "boot.device_create
            # BAKE PLAN", written before any roll): SR_NATIVE_DEVCREATE default ON on top of the transport bake, so the
            # default path's gameplay PM4 is 0. ONE exe (the baked lane build), every arm on it (same_exe enforced), a
            # FRESH --out, staged on J:. The pairs serve the pooled meter and the paired boot design together: one
            # configuration (the meter's sparse dump kept), the devcreate witness in every ON roll and the ME_INIT
            # witness in every OFF roll. The boot_order_gap unknown's root cause (archive/dcf/ROOT_CAUSE.md: the
            # installed shader cache's synchronous boot load on the attach path of the 2026-10-11..14 exes, in both arms,
            # gone since f9c08c2d) sized the boot design: 4 pairs (dcf-v1); the stall criterion (dcf-stall-v2, after
            # Astra's round 1 on the branch) keeps dcl's 29 ON boots - the root cause explains the archived waits'
            # synchronous-cache component, not a devcreate interaction under a cold or contended cache - over every ON
            # boot of the proof, the gate's included, with the WP1 and late-HUD events and the staged cache condition.
            "prefix": "dcf", "obligations": ["boot.device_create"], "gates": "", "bake": True, "same_exe": True,
            "fresh_out": True,
            "resolves": [("boot.device_create", "default_bake", dcf_resolves)],
            "resolver_bind": {"boot_equiv": ("boot_order_gap", "boot_order.powered"),
                              "boot_stall": ("boot_order_gap", "boot_order.stall")},
            # the single-pair meter (dc3, -2.28 vs -3) retires into meter_single_pair only when the pooled zero-form
            # meter passes (devcreate_close's route, its item renamed)
            "retire_on_pass": [{
                "item": "dcf_meter_pool", "old": "meter", "into": "meter_single_pair",
                "text": "dc3 (2026-10-14, exe 42f048a64456): the single-pair total meter missed its bound - pm4 per "
                        "ingame frame 373.82 -> 371.54, delta -2.28 vs <= -3 - while 820F0048's own kind fell 4.0 -> 0.1 "
                        "(meter_bin, dc3 and dc4). The other kinds' run-to-run spread (t2nop +1.9 on dc3, -0.7 on dc2; "
                        "the total's pair-to-pair SD ~1.5 per frame over sgb5's four pairs and dc2 / dc3 / dc4) decides a "
                        "single-pair total bound on a 373-packet path. Re-measured on the transport-baked default path, "
                        "where the re-emit's 6.0 is the only gameplay PM4 left: the mean of the per-pair deltas of four "
                        "alternating pairs on one exe <= -5.7, every default roll <= 0.1, every =0 roll >= 5.5 (plan "
                        "devcreate_bake, docs/NATIVE_ROADMAP.md boot.device_create BAKE PLAN). The single pair and any "
                        "failed pooled run are kept here as witnesses; resolved by a passing pooled run"}],
            "supersede_into": {"dcf_meter_pool": "meter_single_pair"},
            "about": "boot.device_create BAKED (SR_NATIVE_DEVCREATE default ON since native/devcreate-fix; =0 keeps the "
                     "guest bodies and the A/B arm) on the baked exe with no env, staged (--stage J:\\sr_stage), a fresh "
                     "--out: replay 9/9 vs the pre-bake exe (--base: the transport-baked exe without this flip); a "
                     "declared warm-up (the default build; the invocation's first launch; never graded by the mean "
                     "criterion, counted by the stall criterion); four alternating pairs of the default against "
                     "SR_NATIVE_DEVCREATE=0 (p1 ON/OFF, p2 OFF/ON, p3 ON/OFF, p4 OFF/ON) graded twice - by "
                     "tools/perf/boot_equiv.py --design %s (the MEAN criterion: EQUIVALENT / OPEN / INCONCLUSIVE; its "
                     "run-unique JSON binds boot_order_gap's resolver boot_order.powered on every outcome) and by the "
                     "ledger's pooled meter (the mean of the per-pair deltas <= -5.7, every default roll <= 0.1, every =0 "
                     "roll >= 5.5 pm4 per ingame frame, every roll >= 8,000 ingame frames, the provenance checked: one "
                     "exe, the default engaged and =0 not, the declared alternation; on its PASS the single-pair meter "
                     "retires into meter_single_pair); the dry CHECK of the two emitter ports (kinds 8 / 9: no BAD bracket "
                     "and at least one completed comparison of EACH kind); the frozen per-present A/B of the default "
                     "against =0; CPPM4 on the default; the producer census pair (SR_NATIVE_DEVCREATE=0 vs the default, "
                     "SR_NATIVE_BRACKETS=1, both ended by the graceful quit) graded by transport_census.py --pm4-zero "
                     "--base-shows devcreate - the default arm's gameplay holds NO PM4 with NO allowance, the =0 arm shows "
                     "the re-emit; the stall block s1_on..s%d_on (the default build); the world-rule flicker gate; the "
                     "devcreate witness (the ring init line, the ring at 0 packets) in every ON roll and the ME_INIT "
                     "witness in every =0 roll; then tools/perf/boot_stall.py --design %s over EVERY ON boot of the run "
                     "(%d: the warm-up, the pairs' default rolls, cppm4, census1, the stall block, the gate's two boots, "
                     "plus any retained attempt; the events F / A / W / R / C / H; staged; its run-unique JSON binds "
                     "boot_order.stall on every outcome). 37 game rolls (~2 h 45 min of slot) + the replay. Every item but "
                     "the two resolvers resolves the new unknown default_bake" % (
                         BOOT_EQUIV_BAKE_DESIGN, BOOT_STALL_BAKE_EXTRA, BOOT_STALL_BAKE_DESIGN, BOOT_STALL_BAKE_N_MIN),
            "steps": dcf_steps,
        },
        "boot_gaps_bake": {
            # boot gaps BAKED (native/boot-gaps, 2026-10-16; docs/NATIVE_ROADMAP.md "BOOT GAPS BUILD NOTE", written
            # before any roll): the default build's last literal-PM4 producers as records, both gates default ON. ONE exe
            # (the lane build; same_exe), a FRESH --out, staged; the parent environment may not carry either gate
            # (default_keys: Runner.env_refusal - perf_run passes it to every roll, so a default arm would run the
            # explicit form), nor SR_NATIVE_VBLANK (the display-registers bake's default, which the meter pair grades).
            "prefix": "bgb", "obligations": list(BOOT_GAPS_OBLIGATIONS), "gates": "", "bake": True, "same_exe": True,
            "fresh_out": True, "default_keys": list(BOOT_GAPS_KEYS) + ["SR_NATIVE_VBLANK"],
            "vblank_default": True,   # run_gates: its arms as written (every arm leaves SR_NATIVE_VBLANK unset)
            "resolves": bgb_resolves,
            "about": "boot gaps BAKED (SR_NATIVE_MODE_STATE and SR_NATIVE_DISPLAY default ON since native/boot-gaps; =0 "
                     "keeps the guest bodies, the first Swap's flags2A39 decline and the A/B arm) on the lane exe with no "
                     "env, staged (--stage J:\\sr_stage), a fresh --out: replay 9/9 vs the pre-bake exe (--base: dev "
                     "ddfc6be0's build); the census pair (the =0 arm, then the default; SR_NATIVE_BRACKETS=1, both ended "
                     "by the graceful quit) - the =0 arm shows frame 1's UNSTAMPED WAIT_UNTIL / RB_BC_CONTROL and the "
                     "first Swap's persisted display on Swap's line, its witnesses read dcf2's boot (SWAP decl 1, "
                     "flags2A39 1, gapApplied %d, the meter's pm4 %d), the default holds no packet in any frame from "
                     "frame 1 with the strict witness; tools/perf/era_census.py newgame over each (the =0 arm FAILs in "
                     "boot alone; the default PASSes every era from boot, its presenter ramp hash equal to the =0 "
                     "arm's); the meter pair on display.registers' baked default (SR_NATIVE_VBLANK unset, its default "
                     "form checked; no roll pinned to =0) - the =0 arm's run-cumulative dispreg 1 (sysbuf 1, SWAP decl "
                     "1: contradiction meter_run_swap_decl reproduced), the default's 0 (sysbuf 0, decl 0); the dry CHECK "
                     "of the mode-state "
                     "port (kind 10 compared, never bad; the leaves standing down uncounted); the frozen per-present A/B "
                     "of both gates (they act at boot only: the later presents graded equal; its witness what holds in "
                     "either arm - no decline, program or reserve failure, no fallback); CPPM4 on the default; the "
                     "world-rule flicker gate - the strict "
                     "witness (the display line, SWAP with no decline, no fallback, gapApplied 0 from boot, the meter's "
                     "pm4 0 from boot) in every default roll, both gate rolls included. 9 game rolls (~50 min of slot) + "
                     "the replay. Each item names its obligation (Runner.item_oids); the p2.transport items resolve "
                     "boot_gaps_bake, the boot.device_create items mode_state_bake (both pre-registered by the ledger "
                     "delta before --apply-ledger)" % (BOOT_GAPS_BOOT_GAPS, BOOT_GAPS_BOOT_PK),
            "steps": bgb_steps,
        },
        "boot_attach": {
            "prefix": "bat", "obligations": [], "gates": "",
            "about": "render/boot-attach (NATIVE_ROADMAP 'Boot attach'): the shader cache loads off the attach's critical "
                     "path (SR_SHADER_LOAD_ASYNC, default ON; =0 = the old order). No ledger items (no obligation; the "
                     "defect.black_world text is proposed by hand). The replay vs the base exe (identical pixels: the "
                     "lookups are a fully loaded cache's), a DECLARED WARM-UP roll in THIS invocation (the default build; "
                     "run, recorded, never graded - bat2: the first launch of each native_proof invocation was ~1 s late "
                     "to its first CP command), three alternating boot pairs in the MIRROR of bat1's order (OFF/ON, "
                     "ON/OFF, OFF/ON: every earlier set put ON first, so position and arm were confounded), the flicker "
                     "gate on the default (ON) path. Grade the pairs with tools/perf/boot_attach_eval.py "
                     "--warmup <bat_warmup> --pair off:<bat_off1> on:<bat_on1> --pair on:<bat_on2> off:<bat_off2> "
                     "--pair off:<bat_off3> on:<bat_on3> --budget=1.0 "
                     "(rule in its docstring, registered with this plan: the design, markers, OPEN classes and endpoints; "
                     "it reports the ON rolls' measured bounds - attach, init window, probe, loader start, CP ready "
                     "after WP1, CP first command, process ms - the cache's state read just after each attach and any "
                     "[HOST_WAIT_STALL] escalations, and never claims the attach did not wait for the disk; the warm-up "
                     "is checked - before pair 1, the set's exe, configuration and seeding, the HUD reached, a row of "
                     "this run's proof JSON with every pair roll - and never graded; a position line reports the first "
                     "graded slot against its arm) and every "
                     "roll with check_roll's "
                     "black-world and boot-attach entries (check_roll.py --grade)",
            "steps": [
                Step("replay", "replay"),
                # 2026-10-14 (bat2: the first launch of each invocation ~1 s late to its first CP command, and bat1 / bat2
                # both put ON first): the declared warm-up INSIDE this invocation, then the MIRROR of bat1's order, so
                # the first graded slot is an OFF roll for the first time (boot_attach_eval.py accepts either order)
                Step("roll", "warmup", "", "plain"),
                Step("roll", "off1", "SR_SHADER_LOAD_ASYNC=0", "plain"),
                Step("roll", "on1", "", "plain"),
                Step("roll", "on2", "", "plain"),
                Step("roll", "off2", "SR_SHADER_LOAD_ASYNC=0", "plain"),
                Step("roll", "off3", "SR_SHADER_LOAD_ASYNC=0", "plain"),
                Step("roll", "on3", "", "plain"),
                Step("gate", "gate", ""),
            ],
        },
        "log_async": {
            "prefix": "lsa", "obligations": [], "gates": "",
            "about": "app/log-async (docs/TOOLKIT.md 3.2): no thread waits on the log's disk writes any more - one writer "
                     "thread writes, producers only enqueue. No ledger items (no obligation). census0 = the tcmp1 stall's "
                     "configuration (SR_NATIVE_BRACKETS=1: the CP logs a [PM4_COV] F line per frame); plain = the default "
                     "path; inject = census0 with the writer sleeping 5 s inside one write in gameplay "
                     "(SR_LOG_INJECT_STALL=5000@120), the live slow-disk case. Grade with python "
                     "tools/perf/log_stall_eval.py --proof <out> --prefix lsa (rule in its docstring, registered with "
                     "this plan: the [LOG_STALL] census witness from the first 25 s and every ~10 s, byte-exact "
                     "completeness through the census bytes= and lost=0 and consecutive F frame numbers, no producer "
                     "wait over 100 ms with the census counters agreeing (max_wait_ms <= 100, waits= = the wait lines "
                     "+ unwitnessed=), after +60 s the CP's frame interval (F line stamps) and the [HIGHFPS_PACE] max "
                     "under 1000 ms, and in inject the 5 s write with F lines logged while it slept and no producer "
                     "wait, census waits=0 included; tightened after Astra round 1)",
            "steps": [
                Step("roll", "census0", "SR_NATIVE_BRACKETS=1", "plain"),
                Step("roll", "plain", "", "plain"),
                Step("roll", "inject", "SR_NATIVE_BRACKETS=1,SR_LOG_INJECT_STALL=5000@120", "plain"),
            ],
        },
        "owed_rerolls": {
            "prefix": "ow1", "obligations": [], "gates": "",
            "about": "the owed re-rolls on the pause-free tape (dev 160be5e3, the orchestrator 2026-10-10), on the default "
                     "build (rows 1+2, 3, 5+3b, S1 baked): the flicker gate with no env (the rows 5+3b bake gate AND the "
                     "S1 gate_trunk successor - one default-path gate serves both) and the rows 1+2 frozen-pass A/B "
                     "(the p1b_abp successor). No ledger items here: they land by hand with the MAX_MENU world rule in "
                     "one commit, the menu-heavy rolls moved to witness lists",
            "steps": [
                Step("gate", "gate", ""),
                Step("roll", "abp12", "SR_AB_TOGGLE=SR_NATIVE_BIND,SR_AB_PRESENT=1", "freeze"),
            ],
        },
        "segmentless_cppm4_diag": {
            "prefix": "segk", "obligations": [], "gates": "SR_NATIVE_SEGMENTLESS=1",
            "about": "diagnostic (2026-10-10): segj's CPPM4 roll stalled in a kick's WAIT pc+0 == 4 with no id miss - the "
                     "same roll, plain, with the P2 trace so [SCRATCH_CFG] names the segment part and its bytes",
            "steps": [
                Step("roll", "cppm4t", "SR_NATIVE_SEGMENTLESS=1," + PM4OUT, "plain", [segcounters_item("cppm4t_seg")],
                     trace=True),
            ],
        },
        "segmentless_census": {
            "prefix": "sgc", "obligations": [], "gates": "",
            "about": "the segmentless meter's prediction (the orchestrator 2026-10-11: derived from a default-path census, "
                     "not fitted to segl's -60.9): the DEFAULT path with the P2 trace (closes and coherency blocks per "
                     "present) and the literal-span census line (the ring path's <16-byte spans by meter kind)",
            "steps": [
                Step("roll", "census", "", "plain", [census_item("census_census")], trace=True),
            ],
        },
        "segmentless": {
            "prefix": "segb", "obligations": ["p2.segmentless"], "gates": gs_pre,
            "about": "P2-B step 1 (820FD8C0 as host code, segments as part lists by id): the proof the wedge interrupted",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("replay_segb")]),
                # every arm the pre-bake transport (gs_pre: the nine kinds =0 since the transport bake, as it ran)
                Step("roll", "probe", gs_pre, "plain", [census_item("probe_census"), segcounters_item("probe_seg")],
                     trace=True),
                Step("roll", "abp", gs_pre + ",SR_AB_TOGGLE=SR_NATIVE_SEGMENTLESS,SR_AB_PRESENT=1", "freeze",
                     [freeze_ab("abp"), segcounters_item("abp_seg")], trace=True),
                Step("roll", "cppm4", gs_pre + "," + PM4OUT, "freeze", [freeze_exact("cppm4"), segcounters_item("cppm4_seg")]),
                Step("roll", "stress", gs_pre + "," + STRESS + "," + PM4OUT, "freeze",
                     [freeze_exact("stress"), segcounters_item("stress_seg")]),
                # The meter's arms pin SR_NATIVE_SETCONSTMEM=0 since the setconst bake (default ON from 2026-10-14; its
                # record form, under segmentless only, would add its own drop to `on`: the census bound below was taken
                # with it OFF), and meter0 pins SR_NATIVE_SEGMENTLESS=0 (a bare "" has been segmentless itself since the
                # segmentless bake 92e35dbd); both pin SR_NATIVE_FLUSH_SPLIT=0 since the flush-split bake (default ON from
                # 2026-10-14; the census bound predates it), and the nine transport kinds =0 since the transport bake
                # (2026-10-15; the same census bound)
                Step("roll", "on", gs_pre + ",SR_NATIVE_SETCONSTMEM=0,SR_NATIVE_FLUSH_SPLIT=0", "plain",
                     [census_item("on_census"), segcounters_item("on_seg")], trace=True),
                Step("p2trace", "on_trace", ledger=[trace_item("on_trace")], base="on"),
                Step("roll", "meter0", genv("SR_NATIVE_SEGMENTLESS=0,SR_NATIVE_SETCONSTMEM=0,SR_NATIVE_FLUSH_SPLIT=0",
                                            TRANSPORT_OFF), "plain"),
                # The bound from the default-path census sgc1 (2026-10-11), not the plan's old -100 estimate: step 1
                # converts every close's fence tail (other 1 + sync 2) and coherency block (t0 2 + sync 1) - 8.93 closes
                # per present, each with one 11-dword block - and the ring path's < 16-byte literal spans (7.91 bin);
                # NOT 82110C78's own coherency block (1.00 a frame, p2.transport's BRACKET). Predicted 61.5 a frame
                # (other 8.9, sync 26.8, t0 17.9, bin 7.9); the bound = the prediction less 5%.
                Step("meter", "meter", base="meter0", target="on", ledger=[meter_item(
                    "meter", "meter0", -58.4, "bound from the census sgc1 (default path, exe 0ec08d087f23): predicted "
                    "-61.5 a frame (other 8.9, sync 26.8, t0 17.9, bin 7.9), tolerance 5%; the old -100 was a planning "
                    "estimate")]),
                Step("gate", "gate", gs_pre, ledger=[gate_item("gate")]),
            ],
        },
        "display_registers": {
            "prefix": "dreg", "obligations": ["display.registers"], "gates": gdv,
            "about": "display.registers (NATIVE_ROADMAP 'DISPLAY.REGISTERS BUILD NOTE'; DECISIONS (2) of 2026-10-10): GATE "
                     "(b) with (c) - SR_NATIVE_VBLANK=1, the pacing 82111E38 on today's scan-out answers held by a host "
                     "counter (wrap points included) and the vblank ISR's "
                     "source 0 as the host vblank service, no scan-out answer, no vsync-enable bit - on the composed "
                     "transport (every kind + SR_NATIVE_DEVCREATE=1, construns off). LAST in the build order: run after "
                     "p2.transport closes ([PM4_COV] 0). The offline proof (difftest --native-vblank-test) is separate; "
                     "these rolls are the live part. The ledger's timing guard (>= xenia) needs a Xenia pacing reference "
                     "no roll here produces (the pacing step grades against the all-OFF control).",
            "steps": [
                Step("replay", "replay", ledger=[replay_item("dreg_replay")]),
                # the probe: every tick native, every pacing call host, the per-vblank check, the GATE leaf taken, the
                # strict transport witness and the P2 trace (BlockUntilIdle drained)
                Step("roll", "probe", gdv + ",SR_NATIVE_SEG_TILE=1", "plain",
                     [vblank_item("dreg_probe_vblank"), census_item("dreg_probe_census"),
                      segcounters_item("dreg_probe_seg"), tile_drained_item("dreg_probe_tile"),
                      present_gate_item("dreg_probe_gate")], trace=True),
                Step("p2trace", "probe_trace", ledger=[dreg_trace_item("dreg_probe_trace")], base="probe"),
                # the frozen-pass per-present A/B (rule R): odd presents the guest 82111E38 + guest ISR source 0 (their
                # inputs kept live), even the host forms; the per-vblank check on both arms = the oracle trace
                Step("roll", "abp", gdv + ",SR_AB_TOGGLE=SR_NATIVE_VBLANK,SR_AB_PRESENT=1,SR_NATIVE_SEG_TILE=1", "freeze",
                     [freeze_ab("dreg_abp"), vblank_item("dreg_abp_vblank", toggled=True),
                      segcounters_item("dreg_abp_seg")]),
                # CPPM4: the GATE record rendered back to PM4 - its callback reaches the host form through the CP's
                # INTERRUPT and the guest ISR's source 1
                Step("roll", "cppm4", gdv + "," + CPPM4, "freeze",
                     [freeze_exact("dreg_cppm4"), vblank_item("dreg_cppm4_vblank"), segcounters_item("dreg_cppm4_seg")]),
                # the meter: dispreg 28.6 -> 0 per ingame frame (the build note's arithmetic: 2R/F + d on the composed
                # transport, 0 + 0 + d gated, d = SWAP's declines), and the whole run's cumulative 0
                Step("roll", "meter0", gdc, "plain"),
                Step("roll", "meter1", gdv, "plain",
                     [vblank_item("dreg_meter1_vblank"), dispreg_run_item("dreg_meter1_run")]),
                Step("meter", "meter", base="meter0", target="meter1", ledger=[dispreg_zero_item(
                    "dreg_meter", "meter0", "the dispreg column (native_meter.hpp's counting rule) on the composed "
                    "transport: live without the gate, exactly 0 with it")]),
            ] + dreg_pacing_steps(gdc, gdv) + [
                Step("gate", "gate", gdv, ledger=[gate_item("dreg_gate")]),
            ],
        },
        "display_registers_bake": {
            # display.registers BAKED (native/dreg-bake, 2026-10-15; NATIVE_ROADMAP "DISPLAY-REGISTERS BAKE PLAN"):
            # SR_NATIVE_VBLANK default ON (=0 keeps the guest 82111E38, the guest ISR's source 0, the scan-out answers and
            # the vsync-enable bit - the A/B's OFF arm). Every item on ONE exe (same_exe: each roll stamped, each
            # comparison refusing another exe's input): the default arms set no SR_NATIVE_VBLANK, the =0 arms VBLANK_OFF.
            # THE STRICT WITNESS in every roll: every default roll the [NATIVE_VBLANK] line pure ON (vblank_item; the A/B
            # its toggled form), its DEFAULT form (vblank_default_item; the explicit On line forbidden) and the CPU-2
            # lock (cpu2_item); every =0 roll no [NATIVE_VBLANK] line at all with the emulated answers live
            # (vblank_off_item); the probe and the A/B / CPPM4 rolls the strict transport witness (tile_drained_item +
            # xport_item, as transport_bake), the gate the drained tile in both rolls and the reclaim audit in freeze.
            # The meter and pacing arms run without SR_NATIVE_SEG_TILE (its forced drained point stalls the producer
            # 200 ms every 10 s: an arm-only change to a counted interval and a pacing defect of its own -
            # transport_bake's exception).
            # Astra round 1 on the branch (P2 x2): "default" proven unset - a live run refuses while the parent
            # environment carries SR_NATIVE_VBLANK (default_keys; perf_run passes the parent env to every roll) - and a
            # fresh --out (fresh_out: a later --only run can never replace a graded roll).
            "prefix": "dbk", "obligations": ["display.registers"], "gates": "", "bake": True, "same_exe": True,
            "fresh_out": True, "default_keys": ("SR_NATIVE_VBLANK",),
            "vblank_default": True,   # run_gates: its arms as written (the default arms leave SR_NATIVE_VBLANK unset)
            "resolves": [("display.registers", "default_bake", dbk_bake_ids),
                         ("display.registers", "default_pacing", dbk_pace_ids)],
            "about": "display.registers BAKED (SR_NATIVE_VBLANK default ON since native/dreg-bake; =0 keeps the guest "
                     "82111E38, the guest ISR's source 0, the scan-out answers and the vsync-enable bit; an explicit =1 "
                     "without its requirements is refused, the default stands down) on the baked exe with no env: replay "
                     "9/9 vs the pre-bake exe (--base); the new-game world-rule gate with the vblank witness (the "
                     "default form), the CPU-2 lock witness and the drained tile witness in both rolls and the reclaim "
                     "audit in the freeze roll; a traced probe (every tick native, "
                     "every pacing call host, traceBad / early / mismatch 0, no scan-out answer, no vsync-enable bit; the "
                     "lock's tick hold < 20 ms and pacing wait < 100 ms; the GATE native; SWAP at most its boot decline; "
                     "the P2.7 invariants and BlockUntilIdle drained); the frozen-pass per-present A/B of the default "
                     "against =0 (SR_AB_TOGGLE=SR_NATIVE_VBLANK: the per-vblank check on both arms); CPPM4 on the "
                     "default; the meter =0 vs the default - the dispreg column per ingame frame live at =0 and exactly 0 "
                     "by default (the meter bound), and the run-cumulative criterion: the default roll's cumulative "
                     "dispreg <= the same run's SWAP declines d (swapSdk decl=, the line after the meter line) with "
                     "scanout=0 and vsyncBit=0 from [NATIVE_VBLANK] - d = 1 a run on the default path (SWAP_DECL #1 "
                     "why=flags2A39, the first present's refused MODE; tbk1 / tbk3a / tbk3b each decl=1), so the "
                     "run-cumulative dispreg is d, not 0; pacing on the console path, the default against =0 with a "
                     "second =0 control as the calibration (pacing_cmp.py, the 28,672-present window). Every =0 arm "
                     "carries the off witness, every default roll the default form (its On line names the baked "
                     "default, never =1). Every arm on one exe and a fresh --out, enforced; a live run refuses while "
                     "the parent environment sets SR_NATIVE_VBLANK. The offline proof (difftest "
                     "--native-vblank-test, its mutants, every suite vs the pre-bake build) is separate. The ledger's "
                     "timing guard (>= xenia) needs a Xenia pacing reference no roll here produces",
            "steps": dbk_steps,
        },
    }
    return ps


def run_gates(plan, gates):
    """A roll's or the gate's env as it RUNS (Runner.gates_for / do_gate): the step's own gates and, for a plan before
    the display-registers bake (no "vblank_default"), VBLANK_OFF appended when the step does not name SR_NATIVE_VBLANK -
    the gate OFF it ran with, its default then - so a re-run on a baked exe repeats the arms as they ran. A step that
    names the key (display_registers' arms) and the bake plan's arms are left as written; the plan's own gates (the
    replay) are not a roll's. The steps keep their gates as written: the pin is visible in the dry run."""
    names = [kv.split("=", 1)[0] for kv in (gates or "").split(",")]
    if plan.get("vblank_default") or "SR_NATIVE_VBLANK" in names:
        return gates
    return genv(gates, VBLANK_OFF)


def baked_gates(plan, gates):
    """Every bake's pin on a roll's or the gate's env, in bake order (Runner.gates_for / do_gate): the display-registers
    bake's (run_gates: VBLANK_OFF unless the plan is "vblank_default" or the step names the key), then the zpd bake's
    (zpd_gates: ZPD_OFF unless "zpd_default" or named). Each pin reads only its own key, so the order changes no value;
    a plan after both bakes (zpd_bake: both flags) runs its arms as written - the real default build."""
    return zpd_gates(plan, run_gates(plan, gates))


# ------------------------------------------------------------------------------------------ helpers

def fwd(p):
    return p.replace("\\", "/")


def exe_sha256(exe):
    h = hashlib.sha256()
    with open(exe, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def exe_id(exe):
    return exe_sha256(exe)[:12]


# THE EXE STAMP (Astra round 1 on native/transport-bake, P2: "one exe" was asserted, not enforced - `--only meter_off`
# with exe A, then `--only meter_on,meter` with exe B into the same --out, graded A's OFF roll against B's ON roll and
# labelled the item with B). Every roll (and the gate's folder) gets <prefix>_<tag>.exe.json beside it, written BEFORE
# the roll starts (a roll that dies half way is still stamped with the exe that wrote it); a p2trace step copies its
# roll's stamp beside its summary (the pacing comparison reads summaries). A `same_exe` plan's comparison steps (meter,
# p2trace, census, owner, tcensus, pacing, snapshot, bootequiv, bootstall) refuse an input whose stamp names another exe
# - or no exe - than the run's --exe, and its live run refuses an --out holding another exe's (or an unattested) roll
# of the plan. A roll from before the stamp is attested by the stored proof JSON alone: its row ran (not SKIP) in a run
# whose exe_id the proof names; a live run writes that attestation into a stamp before the proof is rewritten.
STAMP = ".exe.json"
INPUT_KINDS = ("meter", "p2trace") + COMPARISON_KINDS


def stamp_matches(stamp, sha, eid):
    """A stamp names the run's exe: the full sha256 when both sides have it, else the 12-hex exe id."""
    if not isinstance(stamp, dict):
        return False
    if stamp.get("exe_sha256") and sha:
        return stamp["exe_sha256"].lower() == sha.lower()
    return bool(stamp.get("exe_id")) and bool(eid) and stamp["exe_id"].lower() == eid.lower()[:12]


def archived_bases(plan):
    """The meter bases a plan reads from rolls it does not take itself: another plan's "<prefix>:<tag>" or a path under
    the out dir "<dir>/<roll>" - archived rolls of an older exe."""
    return sorted({s.base for s in plan["steps"] if s.kind == "meter" and s.base and (":" in s.base or "/" in s.base)})


def pin_refusal(plan, name, exe):
    """'' or why a live run of `plan` on `exe` is refused. A plan whose meter compares against archived rolls is pinned
    to the exe it ran on ("exe_pin", the sha256 prefix exe_id prints; Astra round 1 on native/setconst-bake): on a later
    exe the default path also carries every bake since, so its bound would measure those, not this one."""
    pin = plan.get("exe_pin")
    if not pin:
        return ""
    got = exe_id(exe) if (exe and os.path.exists(exe)) else None
    if got == pin:
        return ""
    return ("plan %s is pinned to exe %s: its meter compares against archived rolls (%s) that a later default path no "
            "longer matches - on another exe the bound would measure the bakes since. --exe %s is %s; run it on the "
            "snapshot of %s, or write a new plan with a same-exe base" % (
                name, pin, ", ".join(archived_bases(plan)) or "-", exe, got or "missing", pin))


def is_reparse(p):
    try:
        st = os.lstat(p)
    except OSError:
        return False
    return bool(getattr(st, "st_file_attributes", 0) & 0x400) or os.path.islink(p)


def safe_rmtree(d):
    """Remove a roll directory - never through a junction / symlink (LANE_BRIEF: a recursive delete that follows
    a reparse point empties its target). Refuses when any reparse point lives inside."""
    if not os.path.isdir(d):
        return
    if is_reparse(d):
        raise SystemExit("refusing to delete %s: it is a reparse point" % d)
    for root, dirs, _files in os.walk(d):
        for x in dirs:
            if is_reparse(os.path.join(root, x)):
                raise SystemExit("refusing to delete %s: %s is a reparse point" % (d, os.path.join(root, x)))
    shutil.rmtree(d)


def scan_abort(log):
    """The same fail-closed log check used by perf_run and run_recomp_x."""
    health = scan_log(log)
    if health.get("scan_error"):
        return health["scan_error"], None  # never VOID an incomplete scan's earlier GPU_HANG
    return health["reason"], health["line"][:200] if health["line"] else None


VOID_REPEATS = 2  # recovered dump stalls are repeated at most twice



def reached_play(roll_dir):
    """perf_run's report.json names an 'ingame' era when the roll reached play. A SAVE-STARTED roll (perf_run
    --from-save wrote save_start.json; p2.zpd zpd1, 2026-10-14) has no opening movie and can spawn Superman in the air,
    where PC_CONTROLS logs `mode -` and perf_report's ingame marker never matches: there the HUD's 'gameplay' era is
    play, the rule flicker_gate.py's freeze era already applies to saved rolls (`gameplay if saved else ingame`)."""
    p = os.path.join(roll_dir, "report.json")
    if not os.path.exists(p):
        return False, "no report.json"
    try:
        rep = json.load(open(p, encoding="utf-8"))
    except (OSError, ValueError) as e:
        return False, "report.json unreadable: %s" % e
    eras = [e.get("era") or e.get("name") for e in rep.get("eras", [])]
    why = "eras: " + ", ".join(str(e) for e in eras)
    if "ingame" in eras:
        return True, why
    if fg.save_started(roll_dir) and "gameplay" in eras:
        return True, why + " (save-started: the HUD's gameplay era is play)"
    return False, why


def counter_lines(log, out_path):
    keep = []
    with open(log, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            if any(t in line for t in COUNTER_TAGS):
                keep.append(line[:900])
    keep = keep[-12:]
    open(out_path, "w", encoding="utf-8").write("\n".join(keep) + ("\n" if keep else ""))
    return keep


def grade_freeze(roll_dir, ab=False):
    """The flicker gate's own freeze grade (pixels + the world rule when this checkout's gate has it); ab = a
    per-present A/B roll (SR_AB_PRESENT=1): the same-transport control (flicker_gate.grade_freeze's ab)."""
    kw = {}
    params = inspect.signature(fg.grade_freeze).parameters
    if "min_world" in params:
        kw["min_world"] = 8
    if ab and "ab" in params:
        kw["ab"] = True
    return fg.grade_freeze(roll_dir, 1, 64, 8, **kw)


def run_log_of(roll_dir):
    return os.path.join(roll_dir, "drive", "attempt1", "run.log")


# ------------------------------------------------------------------------------------------ the runner

class ProofWriteError(Exception):
    """The proof JSON could not be written: the run fails (main exits 3) and --apply-ledger applies nothing."""


class OutLock:
    """One native_proof per --out (Astra round 6): an OS-held exclusive lock on <out>/native_proof.lock for the whole
    invocation - msvcrt.locking on Windows, fcntl.flock elsewhere - so it is released when the process dies; a second
    invocation on the same --out fails fast (SystemExit) instead of interleaving its rows into the same proof JSON."""

    NAME = "native_proof.lock"

    def __init__(self, out):
        self.out, self.f = os.path.abspath(out), None
        self.path = os.path.join(self.out, self.NAME)

    def acquire(self):
        os.makedirs(self.out, exist_ok=True)
        f = open(self.path, "a+b")
        try:
            if os.name == "nt":
                import msvcrt
                f.seek(0)
                msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as e:
            f.close()
            raise SystemExit("REFUSED: %s is locked by another native_proof run (%s: %s) - one run per --out" % (
                self.out, self.path, e))
        self.f = f
        return self

    def release(self):
        if self.f is None:
            return
        try:
            if os.name == "nt":
                import msvcrt
                self.f.seek(0)
                msvcrt.locking(self.f.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.f.fileno(), fcntl.LOCK_UN)
        finally:
            self.f.close()
            self.f = None


class Runner:
    def __init__(self, a, plan):
        self.a = a
        self.plan = plan
        self.prefix = plan["prefix"]
        self.out = os.path.abspath(a.out)
        self.repo = os.path.abspath(a.repo)
        self.exe = os.path.abspath(a.exe) if a.exe else None
        self.base = os.path.abspath(a.base) if a.base else None
        self.results = []
        # rolls voided by the narrow dump-stall rule (dump_stall_void) or an unmet precondition (Step.require, "rule":
        # "require"), with their evidence
        self.voids = []
        self.started = datetime.datetime.now()
        self.complete = False  # set at the end of run(): a stored proof without it is a run that did not finish
        self.exe_sha = exe_sha256(self.exe) if (self.exe and os.path.exists(self.exe)) else None
        self.exe_id = self.exe_sha[:12] if self.exe_sha else "?"
        # the stored proof as it was before this invocation writes its own (a live run rewrites it first thing): the
        # attestation of the plan's rolls from before the exe stamp (stamp_of)
        self.prior = None
        try:
            with open(self.path("proof", ".json"), encoding="utf-8") as f:
                doc = json.load(f)
            if isinstance(doc, dict) and doc.get("plan") == a.plan:
                self.prior = doc
        except (OSError, ValueError):
            pass
        self.progress = os.path.join(self.out, "proof_progress.txt")
        # 2026-10-14 roll-stage: --stage puts every roll's read set on an SSD (perf_run --stage); {} = as before
        stage = getattr(a, "stage", "") or ""
        source = getattr(a, "stage_source", "") or ""
        self.stage_kw = {"stage": os.path.abspath(stage),
                         "stage_source": os.path.abspath(source) if source else None} if stage else {}

    # -- bookkeeping
    def log(self, msg):
        line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
        print(line, flush=True)
        if not self.a.dry_run:
            os.makedirs(self.out, exist_ok=True)
            with open(self.progress, "a", encoding="utf-8") as f:
                f.write(line + "\n")

    def path(self, tag, suffix=""):
        if "/" in tag:  # another roll by its path under the out dir, "<dir>/<roll>" (a re-run's meter base)
            return os.path.join(self.out, tag.replace("/", os.sep) + suffix)
        if ":" in tag:  # another plan's roll in the same out dir, "<prefix>:<tag>" (a bake's pre-bake meter base)
            pre, t = tag.split(":", 1)
            return os.path.join(self.out, "%s_%s%s" % (pre, t, suffix))
        return os.path.join(self.out, "%s_%s%s" % (self.prefix, tag, suffix))

    def rel(self, p):
        """A path as the ledger cites it: relative to the umbrella root, forward slashes."""
        return fwd(os.path.relpath(p, UMBRELLA))

    def record(self, step, verdict, detail, paths=None, pending=False, refused=False):
        """One step's row in the proof JSON (written at once). pending=True: a comparison step's row before it runs
        (FAIL, naming its run-unique output) - the step's next row replaces it, a crash leaves it for --reapply.
        refused=True: a same_exe plan's step whose inputs are another exe's (exe_refusal) - a FAIL that yields no item."""
        row = {"step": step.tag, "kind": step.kind, "verdict": verdict, "detail": detail, "paths": paths or {}}
        if pending:
            row["pending"] = True
        if refused:
            row["refused"] = True
        self.results[:] = [r for r in self.results if not (r.get("pending") and r["step"] == step.tag)]
        self.results.append(row)
        self.log("%s %s: %s | %s" % (step.kind, step.tag, verdict, detail))
        if not self.a.dry_run:
            self.write_proof()
        return verdict

    def write_proof(self):
        """The proof JSON, atomically (a temp file, then os.replace). Any failure is a ProofWriteError - never ignored:
        the run stops, main exits 3 and applies nothing (Astra round 6)."""
        doc = {"plan": self.a.plan, "about": self.plan["about"], "prefix": self.prefix, "exe": self.exe, "exe_id": self.exe_id,
               "base_exe": self.base, "repo": self.repo, "out": self.out, "started": self.started.isoformat(timespec="seconds"),
               "complete": bool(getattr(self, "complete", False)), "steps": self.results, "voids": self.voids}
        if staged(self):
            doc["stage"] = staged(self)
        if self.graceful():  # native/quit-drain round 2: the stranded acceptance policy the terminal items applied
            doc["stranded_policy"] = self.stranded_policy()
        path = self.path("proof", ".json")
        try:
            with open(path + ".tmp", "w", encoding="utf-8") as f:
                json.dump(doc, f, indent=1)
                f.flush()
                os.fsync(f.fileno())
            replace_retry(path + ".tmp", path)
        except OSError as e:
            raise ProofWriteError("%s: %s" % (path, e))
        try:  # a successful write clears an earlier run's unwritable marker
            os.remove(path + UNWRITABLE)
        except OSError:
            pass

    # -- the exe stamp (STAMP above)
    def stamp_path(self, tag):
        return self.path(tag, STAMP)

    def write_stamp(self, tag, doc=None):
        """<prefix>_<tag>.exe.json: the exe this roll / summary came from (a live run only)."""
        if self.a.dry_run:
            return
        d = {"plan": self.a.plan, "step": tag, "exe": self.exe, "exe_sha256": self.exe_sha, "exe_id": self.exe_id,
             "written": datetime.datetime.now().isoformat(timespec="seconds")}
        if doc:
            d.update(doc)
            d["step"] = tag
        p = self.stamp_path(tag)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p + ".tmp", "w", encoding="utf-8") as f:
            json.dump(d, f, indent=1)
        replace_retry(p + ".tmp", p)

    def stamp_of(self, tag):
        """The exe a roll / summary of this plan came from: its stamp, else (a roll from before the stamp) the stored
        proof's attestation - a row of that tag that ran (not SKIP, not pending) in the run whose exe_id the proof
        names - else None. An unreadable stamp is {"error": ...} (matches no exe)."""
        try:
            with open(self.stamp_path(tag), encoding="utf-8") as f:
                d = json.load(f)
            return d if isinstance(d, dict) else {"error": "stamp is not an object"}
        except FileNotFoundError:
            pass
        except (OSError, ValueError) as e:
            return {"error": "unreadable stamp: %s" % e}
        prior = self.prior or {}
        row = next((r for r in prior.get("steps") or [] if r.get("step") == tag), None)
        if prior.get("exe_id") and prior.get("exe_id") != "?" and row and not row.get("pending") and \
                row.get("verdict") not in (None, "SKIP") and not row.get("refused"):
            return {"exe_id": prior["exe_id"], "exe_sha256": None,
                    "source": "the stored proof's %s row (%s, started %s)" % (tag, row.get("verdict"),
                                                                              prior.get("started"))}
        return None

    def input_tags(self, step):
        """The rolls / summaries of this plan a comparison step reads (archived "<prefix>:<tag>" / "<dir>/<roll>"
        bases excluded: the plan's exe_pin governs those)."""
        k = step.kind
        if k == "meter":
            tags = [step.base, step.target] + [t for p in step.pool for t in p]
        elif k == "p2trace":
            tags = [step.base]
        elif k == "pacing":
            tags = [step.base, step.target, step.calib]
        elif k == "snapshot":
            tags = ["gate" if (step.base or "").startswith("gate/") else step.base]
        elif k == "bootequiv":
            tags = [step.warmup] + [t for pr in (step.pairs or []) for _a, t in pr]
        elif k == "bootstall":
            tags = self.stall_tags(step)
        elif k in ("census", "owner", "tcensus", "era"):
            tags = [step.base, step.target]
        elif k == "zpdwin":  # the roll, and (pacing) the p2trace summary whose window it grades
            tags = [step.target, step.base]
        elif k in ("abwit", "memcensus", "texid", "readers"):  # the roll it grades (abwit: its compared pairs too)
            tags = [step.target]
        else:
            tags = []
        return [t for t in tags if t and ":" not in t and "/" not in t]

    def exe_refusal(self, step):
        """'' or why a `same_exe` plan's comparison step must not read its inputs: one whose stamp (or attestation)
        names another exe than the run's, or none. A live step only."""
        if not self.plan.get("same_exe") or self.a.dry_run or step.kind not in INPUT_KINDS:
            return ""
        if not self.exe_sha and self.exe_id in (None, "", "?"):
            return "the run's exe is unknown (no --exe, no stored exe_id): nothing to check its inputs against"
        bad = []
        for t in self.input_tags(step):
            s = self.stamp_of(t)
            if s is None:
                bad.append("%s: no exe stamp, no proof row attests it" % t)
            elif not stamp_matches(s, self.exe_sha, self.exe_id):
                bad.append("%s: exe %s" % (t, s.get("exe_id") or s.get("error")))
        return ("one-exe plan: inputs not from the run's exe %s - %s" % (self.exe_id, "; ".join(bad))) if bad else ""

    def evidence_tags(self):
        """The plan's tags whose outputs are evidence a later step reads: rolls, the gate's folder, p2trace summaries."""
        return [("gate" if s.kind == "gate" else s.tag, s.kind) for s in self.plan["steps"]
                if s.kind in ("roll", "gate", "p2trace")]

    def mixed_out_refusal(self):
        """'' or why a `same_exe` plan may not run live into --out: it holds a roll / summary of this plan from another
        exe, or one no stamp or stored proof row attests (an --only re-run into another exe's evidence)."""
        if not self.plan.get("same_exe"):
            return ""
        bad = []
        for tag, kind in self.evidence_tags():
            out = self.path(tag, "_summary.json") if kind == "p2trace" else self.path(tag)
            if not (os.path.exists(out) or os.path.exists(self.stamp_path(tag))):
                continue
            s = self.stamp_of(tag)
            if s is None:
                bad.append("%s (unattested)" % os.path.basename(out))
            elif not stamp_matches(s, self.exe_sha, self.exe_id):
                bad.append("%s (exe %s)" % (os.path.basename(out), s.get("exe_id") or s.get("error")))
        if not bad:
            return ""
        return ("plan %s runs every arm on one exe: %s holds %d roll(s) / summar(ies) not from --exe %s: %s - use a "
                "fresh --out" % (self.a.plan, self.out, len(bad), self.exe_id, ", ".join(bad[:6]) +
                                 (" ..." if len(bad) > 6 else "")))

    def adopt_attested(self):
        """A live same_exe run, before it rewrites the proof: write the stored proof's attestation of each older,
        unstamped roll / summary into its stamp (the attestation is gone once the proof holds this run's rows)."""
        if not self.plan.get("same_exe") or self.a.dry_run:
            return
        for tag, _kind in self.evidence_tags():
            if os.path.exists(self.stamp_path(tag)):
                continue
            s = self.stamp_of(tag)
            if s is not None and "error" not in s:
                self.write_stamp(tag, dict(s, exe=None))

    def slot(self, *args):
        return subprocess.run([sys.executable, os.path.join(self.repo, "tools", "game_slot.py"), *args], cwd=self.repo)

    def feeds_meter(self, tag):
        """A plain roll a meter step reads (its base or target, or a pooled pair's): the meter's world check needs its
        sparse dumps."""
        return any(s.kind == "meter" and (tag in (s.base, s.target) or any(tag in p for p in s.pool))
                   for s in self.plan["steps"])

    def gates_for(self, step):
        g = BASE_GATES.format(inst=self.a.instance)
        if step.mode == "freeze":
            g += "," + FREEZE
        elif step.mode == "plain":
            g += "," + NO_TIMED + ("" if (self.feeds_meter(step.tag) or step.sparse) else "," + NO_SPARSE)
        extra = baked_gates(self.plan, step.gates)  # the vblank and zpd bakes: older plans run each gate OFF (as they ran)
        if step.trace:
            extra = (extra + "," if extra else "") + "SR_P2_TRACE=" + fwd(self.path(step.tag, "_trace.bin"))
        if getattr(step, "xtr", False):
            extra = (extra + "," if extra else "") + "SR_PM4_TRACE_DIR=" + fwd(self.path(step.tag, "_xtr"))
        if getattr(step, "devtools", None):
            extra = (extra + "," if extra else "") + "SR_DEVTOOLS=levelskip,SR_DEVTOOLS_CMDS=%s,SR_DEVTOOLS_OUT=%s" % (
                fwd(tape_path(self.repo, step.devtools)), fwd(self.path(step.tag, "_devtools")))
        # merged (genv): a step's own SR_DIAG_SIM_FREEZE (the zpd bake's schedule) replaces FREEZE's in place instead of
        # naming the key twice; with no key in common the string is the plain concatenation it always was
        return genv(g, extra) if extra else g

    # -- steps
    def do_owed(self, step):
        """A step the plan needs that this base cannot run (display_registers' pacing before native/transport-composed's
        comparator is merged; dreg_pacing_steps): DRY in a dry run, FAIL in a live one - never SKIP, so the plan
        cannot pass without it. Its `gates` field names what is owed. No ledger item."""
        return self.record(step, "DRY" if self.a.dry_run else "FAIL", "OWED: " + step.gates)

    def do_replay(self, step):
        if not self.base:
            return self.record(step, "SKIP", "no --base exe for the replay comparison")
        out = self.path("replay")
        cmp_txt = self.path("replay_cmp", ".txt")
        if self.a.dry_run:
            return self.record(step, "DRY", "replay %s: base %s (gates %r) vs %s (gates %r) -> %s" % (
                TRACES, self.base, self.plan.get("base_gates", ""), self.exe,
                self.plan.get("replay_gates", self.plan["gates"]), cmp_txt), {"cmp": self.rel(cmp_txt)})
        sys.path.insert(0, os.path.join(self.repo, "tools"))
        import replay_batch as rb  # noqa: E402  (the repo's: its REPO is where the fixtures and the disc live)
        os.makedirs(out, exist_ok=True)
        rows = []
        # "base_gates" (memres_bake): the pre-bake exe's replay with an Oracle-only correction it names; "replay_gates":
        # the target arm's replay env when it differs from the plan's roll gates (the same synchronous storage)
        for tag, exe, gates in (("base", self.base, self.plan.get("base_gates", "")),
                                (self.prefix, self.exe, self.plan.get("replay_gates", self.plan["gates"]))):
            rows += rb.run_job({"tag": tag, "exe": exe, "gates": gates, "traces": TRACES}, out)
        json.dump(rows, open(os.path.join(out, "batch.json"), "w", encoding="utf-8"), indent=1)
        p = subprocess.run([sys.executable, os.path.join(self.repo, "tools", "replay_batch.py"), "compare", "--out", out,
                            "--a", "base", "--b", self.prefix, "--traces", TRACES], cwd=self.repo, capture_output=True, text=True)
        open(cmp_txt, "w", encoding="utf-8").write(p.stdout + p.stderr)
        m = re.findall(r"(\d+)/(\d+) byte-identical", p.stdout)
        ok = bool(m) and m[-1][0] == m[-1][1] == str(len(TRACES.split(",")))
        return self.record(step, "PASS" if ok else "FAIL", (m[-1][0] + "/" + m[-1][1] + " byte-identical") if m else "no summary",
                           {"cmp": self.rel(cmp_txt)})

    def do_roll(self, step):
        out = self.path(step.tag)
        gates = self.gates_for(step)
        fs = save_of(self.repo, step)
        if self.a.dry_run:
            return self.record(step, "DRY", "perf_run gameplay/port %s -> %s gates=%s%s%s%s" % (
                step.mode, out, gates, " " + " ".join(fg.stage_cli(**staged(self))) if staged(self) else "",
                " --from-save %s%s" % (fs, "" if os.path.isdir(fs) else " (MISSING)") if fs else "",
                " --play %s%s --dwell %d --seconds %d" % (
                    tape_path(self.repo, step.play), "" if os.path.exists(tape_path(self.repo, step.play)) else
                    " (MISSING)", step.dwell, step.seconds) if step.play else
                " --dwell %d --seconds %d" % (step.dwell, step.seconds) if step.dwell else ""), {"roll": self.rel(out)})
        safe_rmtree(out)
        # the P2 trace too: a roll that dies before writing its own must not leave an older roll's to its p2trace step
        for suffix in (".txt", "_freeze.txt", "_lines.txt", "_grade.txt", "_trace.bin"):
            if os.path.exists(out + suffix):
                os.remove(out + suffix)
        safe_rmtree(out + "_freeze")
        self.write_stamp(step.tag)  # before the roll: whatever lands in <out> from here on is this exe's

        def launch():
            """The step's roll, as its kind runs it (also every re-roll: a void re-roll is the same roll)."""
            if step.play or step.dwell:
                return roll_long(self.exe, out, gates, self.repo, step.play or os.path.relpath(fg.PLAY_TAPE, self.repo),
                                 step.dwell, step.seconds, stage_args=fg.stage_cli(**staged(self)), from_save=fs)
            if fs:  # the gate's tape made save-started (flicker_gate.save_tape), as flicker_gate --from-save runs it
                return fg.roll(self.exe, out, gates, self.repo, from_save=fs, tape=fg.save_tape(out + "_tape"),
                               **staged(self))
            return fg.roll(self.exe, out, gates, self.repo, **staged(self))

        def set_aside(suffix, k):
            """The roll (and its runner output) kept under <out>_<suffix><k> before it is repeated."""
            vdir = out + "_%s%d" % (suffix, k)
            while os.path.exists(vdir):
                vdir += "x"
            os.rename(out, vdir)
            if os.path.exists(out + ".txt"):
                os.replace(out + ".txt", vdir + ".txt")
            return vdir

        req = getattr(step, "require", None)
        paths = {"roll": self.rel(out)}
        met, seen = True, ""
        for attempt in range(REQUIRE_REROLLS + 1):
            rc = launch()
            log = run_log_of(out)
            why, line = scan_abort(log)
            # The narrow void (dump_stall_void): kept under <out>_void<k> with its evidence, then the roll is repeated.
            k = 0
            while why == "GPU hang trigger" and k < VOID_REPEATS:
                void, evidence = dump_stall_void(log)
                if not void:
                    break
                k += 1
                vdir = set_aside("void", k)
                self.voids.append({"step": step.tag, "roll": self.rel(vdir), "evidence": evidence})
                self.log("roll %s: VOID #%d (the frame-dump swap stall; kept as %s) | %s" % (step.tag, k, vdir, " | ".join(evidence)))
                self.write_proof()
                rc = launch()
                log = run_log_of(out)
                why, line = scan_abort(log)
            if why:
                self.abort = "%s in %s: %s" % (why, step.tag, line)
                return self.record(step, "ABORT", self.abort, paths)
            if rc != 0:
                self.abort = "%s perf_run exit %d" % (step.tag, rc)
                return self.record(step, "FAIL", self.abort, paths)
            played, eras = reached_play(out)
            if not played:
                self.abort = "%s never reached play (%s)" % (step.tag, eras)
                return self.record(step, "ABORT", self.abort, paths)
            if not req:
                break
            # The step's precondition (p2.zpd, zpd5: a roll whose game issued no occlusion query has nothing to grade):
            # unmet = VOID - the roll kept as <out>_noquery<k>, repeated once (REQUIRE_REROLLS); unmet again = NO DATA.
            met, seen = require_met(log, req)
            if met or attempt == REQUIRE_REROLLS:
                break
            vdir = set_aside("noquery", attempt + 1)
            evidence = ["%s: %s" % (req["why"], seen)]
            self.voids.append({"step": step.tag, "roll": self.rel(vdir), "evidence": evidence, "rule": "require"})
            self.log("roll %s: VOID (precondition not met; kept as %s, re-rolled once) | %s" % (step.tag, vdir, evidence[0]))
            self.write_proof()
        detail = "perf_run exit %d" % rc
        if req:  # visible at a glance in every roll's row: "precondition [GPU_CP] zpd>0 met: zpd=4786 (fake 0)"
            detail += " | precondition %s %s: %s" % (req.get("name", "require"), "met" if met else
                                                     "NOT MET twice - NO DATA", seen)
        if os.path.exists(log):
            counter_lines(log, out + "_lines.txt")
        verdict = "DONE"
        if step.mode == "freeze":
            r = grade_freeze(out, ab="SR_AB_PRESENT=1" in gates)
            with open(out + "_grade.txt", "w", encoding="utf-8") as grade_file:
                json.dump({k: r.get(k) for k in ("verdict", "counted", "bad", "world_line", "world_fail", "unsettled",
                                               "static_pairs", "unsettled_pairs", "control_line")}, grade_file, indent=1)
            verdict = r.get("verdict", "NO DATA")
            detail += " | freeze grade %s counted=%s bad=%s %s%s%s" % (
                verdict, r.get("counted"), r.get("bad"), r.get("world_line", ""),
                (" -> " + r["world_fail"]) if r.get("world_fail") else "",
                (" | " + r["control_line"]) if r.get("control_line") else "")
            if "SR_AB_PRESENT=1" in gates:
                with open(log, "rb") as log_file:
                    n = sum(1 for ln in log_file.read().decode("utf-8", "replace").splitlines() if "[AB_PRESENT]" in ln)
                detail += " | AB_PRESENT %d" % n
                if n < 1000:
                    verdict = "FAIL"
                    detail += " (toggle not witnessed)"
            paths["freeze_table"] = self.rel(out + "_freeze.txt")
        if not met:
            verdict = "NO DATA"  # never PASS / DONE: run() exits 1; its items grade NO DATA through their `require`
        return self.record(step, verdict, detail, paths)

    def pooled_item(self, step, item, receipt=None):
        """A pooled meter step's item with its paths: pair 1 as path / base, the other pairs as `pool`, and the step's
        run-unique receipt when it has one."""
        it = dict(item)
        it["path"], it["base"] = self.rel(self.path(step.target)), self.rel(self.path(step.base))
        it["pool"] = [{"path": self.rel(self.path(t)), "base": self.rel(self.path(b))} for b, t in step.pool]
        if receipt:
            it["receipt"] = receipt
        return it

    def do_meter_pool(self, step):
        """The pool of every pair (step.base / step.target + step.pool), graded by tools/native_ledger.py's own check of
        the step's item (one rule for the runner and the ledger: pair-weighted, provenance, world rule, roll length);
        meter_delta.py's per-pair transcripts are written beside it (information: the pool, not any one pair, is the
        measurement). Astra round 1 (P1-4): an `always` step - once ANY of its rolls ran, it is written on every outcome
        (a crashed meter roll is a FAIL, never a SKIP without an item); only a pool none of whose rolls ran (the plan
        ended before the meter) is a SKIP. Its run-unique RECEIPT (reserve_output: pending FAIL row, exclusive file;
        stamped, committed last) records each roll's run row and file hashes, and the item names it: the ledger grades
        the receipt's verdict first (a pool whose rolls never ran grades FAIL, not unavailable) and refuses a roll whose
        files changed since the run."""
        pairs = [(step.base, step.target)] + list(step.pool)
        it = self.pooled_item(step, step.ledger[0])
        if self.a.dry_run:
            js, _txt = self.reserve_output(step)
            return self.record(step, "DRY", "the ledger's pool of %d pairs (%s), %s per ingame frame, weighted by %s, "
                                            "need delta <= %s, world_min %s, every roll >= %s ingame frames, provenance "
                                            "%s; receipt %s" % (
                                                len(pairs), ", ".join("%s -> %s" % p for p in pairs), it["field"],
                                                it.get("pool_weight", "frame"), it.get("max_delta"), it.get("world_min"),
                                                it.get("min_roll_frames"), json.dumps(it.get("provenance")), js),
                               {"json": self.rel(js)})
        rows = {r["step"]: r for r in self.results}
        tags = [t for p in pairs for t in p]
        if not any((rows.get(t) or {}).get("verdict") not in (None, "SKIP") for t in tags):
            return self.record(step, "SKIP", "no meter roll ran (%s): nothing was measured" % (
                getattr(self, "abort", None) or "every roll skipped"))
        nd = self.nodata_inputs(step)
        if nd:  # as do_meter: a NO DATA roll in the pool is the pool's NO DATA, never a PASS
            return self.record(step, "NO DATA", "NO DATA: pool roll %s NO DATA (its precondition unmet twice) - the "
                               "meter grades nothing" % ", ".join(nd))
        return self.guarded(step, lambda: self.meter_pool_body(step, pairs, tags, rows))

    def meter_pool_body(self, step, pairs, tags, rows):
        js, txt = self.reserve_output(step)
        out = []
        for b, t in pairs:
            p = subprocess.run([sys.executable, os.path.join(HERE, "meter_delta.py"), self.path(b), self.path(t)],
                               capture_output=True, text=True)
            out.append("== pair %s -> %s (meter_delta exit %d)\n%s%s" % (b, t, p.returncode, p.stdout, p.stderr))
        bad, rolls = [], {}
        for t in tags:
            v = (rows.get(t) or {}).get("verdict") or "no row"
            rolls[self.rel(self.path(t))] = {"step": t, "row": v, "files": roll_files(self.path(t))}
            if v != "DONE":
                bad.append("%s: its run row says %s" % (t, v))
        it = self.pooled_item(step, step.ledger[0])  # graded WITHOUT the receipt: the receipt is what this writes
        res, det = ledger_check(it)
        out.append("POOLED (tools/native_ledger.py's own grade of %s): %s | %s" % (it["id"], res, det))
        with open(txt, "w", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")
        verdict = "PASS" if res == "pass" and not bad else "FAIL"
        why = "; ".join(bad + ([] if res == "pass" else ["the ledger's grade: %s" % res]))
        self.stamp_verdict(js, {"pool": [[self.rel(self.path(b)), self.rel(self.path(t))] for b, t in pairs],
                                "rolls": rolls, "grade": {"result": res, "detail": det}}, verdict, 0, why)
        v = self.record(step, verdict, det + (" | " + why if why else ""), {"json": self.rel(js), "pool": self.rel(txt)})
        self.commit_output(js)  # LAST: the verdict counts from here on
        return v

    def nodata_inputs(self, step):
        """The meter's input rolls (its base and target, a pool's pairs) whose row in THIS run is NO DATA (their
        precondition unmet twice: do_roll). zbk2 (2026-10-16): its meter graded PASS over meter_off NO DATA."""
        tags = [step.base, step.target] + [t for pair in (step.pool or []) for t in pair]
        by = {r["step"]: r for r in self.results}
        return [t for t in tags if (by.get(t) or {}).get("verdict") == "NO DATA"]

    def do_meter(self, step):
        if step.pool:
            return self.do_meter_pool(step)
        a_dir, b_dir = self.path(step.base), self.path(step.target)
        txt = self.path(step.tag, "_delta.txt")
        if self.a.dry_run:
            return self.record(step, "DRY", "meter_delta %s -> %s" % (a_dir, b_dir), {"delta": self.rel(txt)})
        nd = self.nodata_inputs(step)
        if nd:  # the step inherits its rolls' NO DATA: never PASS, its items never resolve (nodata_ids)
            return self.record(step, "NO DATA", "NO DATA: input roll %s NO DATA (its precondition unmet twice) - the "
                               "meter grades nothing" % ", ".join(nd), {"a": self.rel(a_dir), "b": self.rel(b_dir)})
        p = subprocess.run([sys.executable, os.path.join(HERE, "meter_delta.py"), a_dir, b_dir], capture_output=True, text=True)
        open(txt, "w", encoding="utf-8").write(p.stdout + p.stderr)
        need = [e for e in step.ledger if e["kind"] == "meter_delta"]
        if need:
            # Astra round 1 on native/zpd-bake (P2-4): meter_delta.py PRINTS one decimal (OFF 0.004 / ON 0.040 read
            # "0.0 -> 0.0, +0.0" and passed a positive delta the ledger rejects). The step's verdict is the ledger's own
            # check of the step's item on the unrounded per-frame values (tools/native_ledger.py check_item: the same
            # windows, bounds and world rule) - one rule for the runner and the ledger, as the pooled meter's; the
            # transcript stays as information.
            it = dict(need[0], path=self.rel(b_dir), base=self.rel(a_dir))
            res, det = ledger_check(it)
            return self.record(step, "PASS" if res == ledger_module().PASS else "FAIL", det,
                               {"a": self.rel(a_dir), "b": self.rel(b_dir), "delta": self.rel(txt)})
        # a meter step with no ledger item (information only): the transcript printed a delta, nothing to grade
        ok = re.search(r"B - A per_frame: pm4=([+-]?[\d.]+)", p.stdout) is not None
        return self.record(step, "PASS" if ok else "FAIL", p.stdout.strip().splitlines()[-1] if p.stdout.strip() else "no output",
                           {"a": self.rel(a_dir), "b": self.rel(b_dir), "delta": self.rel(txt)})

    def reserve_output(self, step, stem=""):
        """A RUN-UNIQUE output pair for a comparison step (devcreate-boot 2026-10-14, Astra rounds 4-5): <prefix>_<tag>
        <stem>.<UTC stamp>[-n].json / .txt. First a PENDING FAIL row naming the JSON goes into the proof JSON, so a crash
        from here on leaves a row --reapply exports; then the JSON is created EXCLUSIVELY (O_EXCL: an existing file is
        never reused, the next name is taken) with the FAIL placeholder written and fsynced in the same call. A file left
        empty by a crash inside that call is still the row's: unreadable, it grades FAIL. The step's item names this
        file - no older run's output can be read. Dry: the would-be name, nothing recorded or created."""
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        for n in range(100):
            base = self.path(step.tag, "%s.%s%s" % (stem, stamp, "-%d" % n if n else ""))
            js, txt = base + ".json", base + ".txt"
            if self.a.dry_run:
                return js, txt
            if os.path.exists(js):
                continue
            self.record(step, "FAIL", "pending: the comparison has not finished (a crash leaves this FAIL)",
                        {"json": self.rel(js)}, pending=True)
            try:
                fd = os.open(js, os.O_WRONLY | os.O_CREAT | os.O_EXCL)
            except FileExistsError:
                continue
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(PLACEHOLDER, f)
                f.flush()
                os.fsync(f.fileno())
            return js, txt
        raise RuntimeError("%s: no free run-unique output name" % step.tag)

    def guarded(self, step, fn):
        """Run a comparison step's body; ANY exception (the reservation, the launch, the transcript, the parse) is the
        step's FAIL: stamped into the run-unique JSON its pending row names, where that can still be written, and
        recorded with that path - so its item is exported and grades FAIL, and a crash never leaves an earlier PASS as
        the only evidence (Astra round 5)."""
        try:
            return fn()
        except ProofWriteError:
            raise  # the run fails (main exits 3): nothing to record into
        except Exception as e:  # noqa: BLE001 - any failure is the step's FAIL
            last = next((r for r in reversed(self.results) if r["step"] == step.tag), None)
            rel = ((last or {}).get("paths") or {}).get("json")
            why = "exception: %s: %s" % (type(e).__name__, e)
            if rel:
                try:
                    self.stamp_verdict(os.path.join(UMBRELLA, rel.replace("/", os.sep)), {}, "FAIL", None, why)
                except Exception as e2:  # noqa: BLE001 - the row stays FAIL; an empty / placeholder file grades FAIL
                    why += " (its json could not be stamped: %s)" % e2
            return self.record(step, "FAIL", why, {"json": rel} if rel else {})

    def commit_output(self, js):
        """The LAST write of a comparison step (Astra round 6): `committed: true` in its run-unique JSON, only after the
        step's final row is in the proof JSON. The item requires it, so a crash anywhere before - a PASS stamped but
        its row never recorded included - leaves an uncommitted file that grades FAIL."""
        with open(js, encoding="utf-8") as f:
            doc = json.load(f)
        doc["committed"] = True
        with open(js + ".tmp", "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
            f.flush()
            os.fsync(f.fileno())
        replace_retry(js + ".tmp", js)

    def stamp_verdict(self, js, doc, verdict, exit_code, why):
        """Write the step's verdict into its own run-unique JSON (runner.verdict): the ledger item checks it, so a step
        that failed - a nonzero exit after a passing JSON included - grades FAIL durably."""
        doc = dict(doc) if isinstance(doc, dict) else {}
        doc.pop("committed", None)  # committed only by commit_output, after the final row
        doc["runner"] = {"verdict": verdict, "exit": exit_code, "why": why}
        with open(js, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)

    def do_census(self, step):
        """tools/perf/residue_census.py over the base and the gated census rolls: FAIL when a census is missing or
        malformed (its exit 2), else graded by the step's json item checks - none in the residue plan, where the step is
        a diagnostic beside the producer census items."""
        a_dir, b_dir = self.path(step.base), self.path(step.target)
        if self.a.dry_run:
            js, _txt = self.reserve_output(step, "_census")
            return self.record(step, "DRY", "residue_census %s -> %s" % (a_dir, b_dir), {"json": self.rel(js)})
        return self.guarded(step, lambda: self.census_body(step, a_dir, b_dir))

    def census_body(self, step, a_dir, b_dir):
        js, txt = self.reserve_output(step, "_census")
        p = subprocess.run([sys.executable, os.path.join(HERE, "residue_census.py"), a_dir, b_dir, "--json", js]
                           + self.terminal_args(), capture_output=True, text=True)
        open(txt, "w", encoding="utf-8").write(p.stdout + p.stderr)
        try:
            d = json.load(open(js, encoding="utf-8"))
            if not isinstance(d, dict):
                raise ValueError("the census json is not an object (%s)" % type(d).__name__)
            if set(d) == {"runner"}:
                raise ValueError("residue_census wrote none (exit %d)" % p.returncode)
        except (OSError, ValueError) as e:
            why = "no census json: %s" % e
            self.stamp_verdict(js, {}, "FAIL", p.returncode, why)
            v = self.record(step, "FAIL", why, {"json": self.rel(js)})
            self.commit_output(js)
            return v
        if p.returncode != 0 or "error" in d:
            why = d.get("error") or "residue_census exit %d" % p.returncode
            self.stamp_verdict(js, d, "FAIL", p.returncode, why)
            v = self.record(step, "FAIL", why, {"json": self.rel(js)})
            self.commit_output(js)
            return v
        bad = [b for it in step.ledger for b in json_checks(d, [c for c in it["checks"] if not (
            c["field"].startswith("runner.") or c["field"] == "committed")])]
        fmt = lambda v: "%.3f" % v if isinstance(v, (int, float)) else "-"
        r = d.get("ratio", {})
        detail = "ratios coher_reg %s wait_reg_mem %s wait_until %s" % (
            fmt(r.get("coher_reg")), fmt(r.get("wait_reg_mem")), fmt(r.get("wait_until")))
        self.stamp_verdict(js, d, "PASS" if not bad else "FAIL", p.returncode, ", ".join(bad))
        v = self.record(step, "PASS" if not bad else "FAIL",
                        detail + ("" if not bad else " | violated: " + ", ".join(bad)), {"json": self.rel(js)})
        self.commit_output(js)
        return v

    def tool_json(self, step, js, p, tool):
        """A comparison tool's JSON written over the reservation: the dict, or (None, why) - unreadable, not an
        object, or the placeholder left untouched (the tool wrote none)."""
        try:
            with open(js, encoding="utf-8") as f:
                d = json.load(f)
        except (OSError, ValueError) as e:
            return None, "unreadable %s json: %s" % (tool, e)
        if not isinstance(d, dict):
            return None, "the %s json is not an object (%s)" % (tool, type(d).__name__)
        if set(d) == {"runner"}:
            return None, "%s wrote no json (exit %d)" % (tool, p.returncode)
        return d, None

    def finish(self, step, js, d, verdict, exit_code, detail, why=""):
        """stamp the step's verdict into its run-unique JSON, record the row, then commit the JSON (LAST)."""
        self.stamp_verdict(js, d if isinstance(d, dict) else {}, verdict, exit_code, why)
        v = self.record(step, verdict, detail, {"json": self.rel(js)})
        self.commit_output(js)
        return v

    def do_tcensus(self, step):
        """tools/perf/transport_census.py --transport-zero over the base (census0) and target (census1) census rolls,
        into a RUN-UNIQUE output (reserve_output, guarded, the verdict stamped, committed last - the owner / census
        steps' discipline since devcreate-boot's Astra rounds 4-6): FAIL when either has no usable census (exit 2) or
        the JSON is missing / not an object / untouched, else graded by the step's json item checks (the JSON verdict
        and the transport-producer zero's own checks)."""
        a_dir, b_dir = self.path(step.base), self.path(step.target)
        # mode "flush" (p1.flush_residue's census pair): --flush-residue-zero, the flush_residue group, in place of the
        # transport-producer zero; mode "construns" (the construns bake's pair): --construns-zero; mode "pm4zero" (the
        # transport bake's default-path pair): --pm4-zero with 820F0048's re-emit the one exception (DEVCREATE unbaked)
        # mode "pm4zero_strict" (the devcreate bake's pair, native/devcreate-fix): --pm4-zero with NO allowance, the
        # base (SR_NATIVE_DEVCREATE=0) showing 820F0048's re-emit - the transport is native in both arms
        # mode "pm4zero_zpd" (p2.zpd's bake, native/zpd-bake): --pm4-zero with NO allowance, the base (SR_NATIVE_ZPD=0)
        # showing the HUD glow queries as guest PM4 - the transport and devcreate native in both arms
        zero = {"flush": "--flush-residue-zero", "construns": "--construns-zero",
                "pm4zero": "--pm4-zero --allow-devcreate-reemit",
                "pm4zero_strict": "--pm4-zero --base-shows devcreate",
                "pm4zero_zpd": "--pm4-zero --base-shows zpd"}.get(step.mode, "--transport-zero")
        if self.a.dry_run:
            js, _txt = self.reserve_output(step, "_tcensus")
            return self.record(step, "DRY", "transport_census --base %s --target %s %s --json %s" % (
                a_dir, b_dir, zero, js), {"json": self.rel(js)})
        return self.guarded(step, lambda: self.tcensus_body(step, a_dir, b_dir, zero))

    def tcensus_body(self, step, a_dir, b_dir, zero="--transport-zero"):
        js, txt = self.reserve_output(step, "_tcensus")
        p = subprocess.run([sys.executable, os.path.join(HERE, "transport_census.py"), "--base", a_dir, "--target", b_dir]
                           + zero.split() + ["--json", js], capture_output=True, text=True)
        with open(txt, "w", encoding="utf-8") as f:
            f.write("exit=%d\n%s%s" % (p.returncode, p.stdout or "", p.stderr or ""))
        d, why = self.tool_json(step, js, p, "transport_census")
        if d is None:
            return self.finish(step, js, None, "FAIL", p.returncode, why, why)
        if p.returncode == 2:
            # DARK_CENSUS (a counter that ran without a frame end) or NO_CENSUS, each side's reason named: refused,
            # never graded as a zero (native/composed-census)
            un = d.get("unusable") or {}
            why = "no usable census: %s%s" % (d.get("verdict"), "".join(
                " | %s: %s" % (s, str(un[s])[:240]) for s in ("base", "target") if s in un))
            return self.finish(step, js, d, "FAIL", p.returncode, why, why)
        bad = [b for it in step.ledger for b in json_checks(d, [c for c in it["checks"] if not (
            c["field"].startswith("runner.") or c["field"] == "committed")])]
        rem = [(r["producer"], r["per_frame"]) for r in d.get("remaining", []) or []]
        detail = "verdict %s; target remaining %s" % (d.get("verdict"), rem[:6])
        if p.returncode == 3 and d.get("verdict") == "INCONCLUSIVE":
            # --allow-devcreate-reemit: no check failed, but an allowed packet no producer line attributes (the frames
            # after the final report of a roll the harness killed): never a PASS; the item is exported and grades FAIL
            why = d.get("inconclusive") or "unattributed allowed packets"
            return self.finish(step, js, d, "INCONCLUSIVE", p.returncode, detail + " | " + why, why)
        verdict = "PASS" if not bad else "FAIL"
        return self.finish(step, js, d, verdict, p.returncode,
                           detail + ("" if not bad else " | violated: " + ", ".join(bad)), ", ".join(bad))

    def era_cmd(self, step, js):
        cmd = [sys.executable, os.path.join(HERE, "era_census.py"), "%s:%s" % (step.era, self.path(step.base)),
               "--json", js]
        if self.exe_sha:
            cmd += ["--exe-sha256", self.exe_sha]  # C10: the roll launched the run's exe
        return cmd

    def do_era(self, step):
        """boot gaps: tools/perf/era_census.py over the census roll `base` as kind `era`, into a RUN-UNIQUE output (as
        do_tcensus: reserved, guarded, the verdict stamped, committed last). The tool's own document is kept whole and
        the runner adds a flat summary of its one roll (`roll`: verdict, checks, failed, the non-zero eras / windows,
        the boot era's packets and first non-zero frame, the whole run's gapApplied) for the ledger's dotted checks, and
        - with `target` (the =0 arm's census roll) - the two rolls' presenter ramp hashes (`lut`: the last display line's
        lut= of each, equal or not). Graded by the step's json item checks: mode "pass" wants the tool's PASS, mode
        "control" its FAIL in boot alone (the item's checks say which); either way the grade is the items'."""
        if self.a.dry_run:
            js, _txt = self.reserve_output(step, "_era")
            return self.record(step, "DRY", " ".join(self.era_cmd(step, js)) +
                               (" | lut vs %s" % self.path(step.target) if step.target else ""), {"json": self.rel(js)})
        return self.guarded(step, lambda: self.era_body(step))

    @staticmethod
    def last_lut(log):
        """The lut= of a roll's last [NATIVE_FLUSH] <why> display line (its terminal one under the graceful quit), or
        None."""
        rx = re.compile(r"\[NATIVE_FLUSH\] \w+ display gate=.* \| lut=([0-9a-f]{16})")
        hit = None
        if os.path.isfile(log):
            with open(log, "rb") as f:
                for ln in f.read().decode("utf-8", "replace").splitlines():
                    m = rx.search(ln)
                    if m:
                        hit = m.group(1)
        return hit

    def era_body(self, step):
        js, txt = self.reserve_output(step, "_era")
        p = subprocess.run(self.era_cmd(step, js), capture_output=True, text=True)
        with open(txt, "w", encoding="utf-8") as f:
            f.write("exit=%d\n%s%s" % (p.returncode, p.stdout or "", p.stderr or ""))
        d, why = self.tool_json(step, js, p, "era_census")
        if d is None:
            return self.finish(step, js, None, "FAIL", p.returncode, why, why)
        r = (d.get("rolls") or [{}])[0] or {}
        rows = (r.get("eras") or []) + (r.get("windows") or [])
        boot = next((e for e in (r.get("eras") or []) if e.get("name") == "boot"), None) or {}
        seg = (r.get("whole_run") or {}).get("seg") or {}
        d["roll"] = {"verdict": r.get("verdict"), "checks": r.get("checks") or {}, "failed": r.get("failed") or [],
                     "nonzero_eras": [e.get("name") for e in rows if e.get("pk")],
                     "boot_pk": boot.get("pk"), "boot_first_nonzero": (boot.get("first_nonzero") or {}).get("frame"),
                     "gap_applied": seg.get("gapApplied"), "missing_eras": r.get("missing_eras")}
        if step.target:
            a = self.last_lut(run_log_of(self.path(step.target)))
            b = self.last_lut(run_log_of(self.path(step.base)))
            d["lut"] = {"base": a, "target": b, "equal": a is not None and a == b}
        bad = [x for it in step.ledger for x in json_checks(d, [c for c in it["checks"] if not (
            c["field"].startswith("runner.") or c["field"] == "committed")])]
        detail = "era_census exit %d verdict %s; non-zero %s; boot pk %s; gapApplied %s%s" % (
            p.returncode, d["roll"]["verdict"], d["roll"]["nonzero_eras"], d["roll"]["boot_pk"],
            d["roll"]["gap_applied"], ("; lut %s" % ("equal" if d["lut"]["equal"] else "DIFFERENT %s / %s" % (
                d["lut"]["base"], d["lut"]["target"]))) if step.target else "")
        verdict = "PASS" if not bad else "FAIL"
        return self.finish(step, js, d, verdict, p.returncode,
                           detail + ("" if not bad else " | violated: " + ", ".join(bad)), ", ".join(bad))

    def do_pacing(self, step):
        """tools/perf/pacing_cmp.py over p2trace steps' summaries (base = the control's, target = the composed roll's,
        calib = a second control's from the same session), into a RUN-UNIQUE output (as do_tcensus). The comparator's
        three outcomes: FAIL (any hard failure - written to the ledger whatever the calibration says), PASS (every check
        and the calibration), INCONCLUSIVE (an underpowered check, or a would-be PASS whose calibration is not a PASS):
        no evidence either way - items() writes no item for it, so it never closes pacing."""
        a_js, b_js = self.path(step.base, "_summary.json"), self.path(step.target, "_summary.json")
        cal = ["--calibration", self.path(step.calib, "_summary.json")] if step.calib else []
        if self.a.dry_run:
            js, _txt = self.reserve_output(step, "_pacing")
            return self.record(step, "DRY", "pacing_cmp %s %s %s--json %s" % (a_js, b_js, " ".join(cal) + " " if cal else "",
                                                                         js), {"json": self.rel(js)})
        return self.guarded(step, lambda: self.pacing_body(step, a_js, b_js, cal))

    def pacing_body(self, step, a_js, b_js, cal):
        js, txt = self.reserve_output(step, "_pacing")
        p = subprocess.run([sys.executable, os.path.join(HERE, "pacing_cmp.py"), a_js, b_js] + cal + ["--json", js],
                           capture_output=True, text=True)
        with open(txt, "w", encoding="utf-8") as f:
            f.write("exit=%d\n%s%s" % (p.returncode, p.stdout or "", p.stderr or ""))
        d, why = self.tool_json(step, js, p, "pacing_cmp")
        if d is None:
            return self.finish(step, js, None, "FAIL", p.returncode, why, why)
        detail = "verdict %s vblanks delta %s wait p50 %s" % (d.get("verdict"), d.get("vblanks_delta"),
                                                            d.get("throttle_wait_p50"))
        if d.get("verdict") == "INCONCLUSIVE" and p.returncode == 3:
            # an underpowered check (bounds straddling its margin) or a calibration that is not a PASS: no evidence
            # either way; a FAIL never lands here (pacing_cmp keeps it a FAIL whatever the calibration says)
            und = sorted(k for k, v in (d.get("states") or {}).items() if v != "PASS")
            calib = d.get("calibration") or {}
            cal_bad = sorted(k for k, v in (calib.get("states") or {}).items() if v != "PASS")
            whys = []
            if und:
                whys.append("inconclusive checks: " + ", ".join(und) + " (presents needed %s)" % d.get("presents_needed"))
            if calib and calib.get("verdict") != "PASS":
                whys.append("the control-vs-control calibration was %s: %s" % (calib.get("verdict"), ", ".join(cal_bad)))
            return self.finish(step, js, d, "INCONCLUSIVE", p.returncode, detail + " | " + "; ".join(whys),
                               "; ".join(whys))
        bad = [b for it in step.ledger for b in json_checks(d, [c for c in it["checks"] if not (
            c["field"].startswith("runner.") or c["field"] == "committed")])]
        if p.returncode not in (0, 1):
            bad.insert(0, "pacing_cmp exit=%d" % p.returncode)
        verdict = "PASS" if not bad else "FAIL"
        return self.finish(step, js, d, verdict, p.returncode,
                           detail + ("" if not bad else " | violated: " + ", ".join(bad)), ", ".join(bad))

    def do_zpdwin(self, step):
        """tools/perf/zpd_activity.py over one roll (Astra round 1 on native/zpd-bake, P2-2): the occlusion queries
        inside the window a step grades - mode "pacing": the roll (target) against its p2trace summary's graded window
        (base); "freeze" / "freeze_ab": the frozen pass's compared presents, hold by hold (freeze_ab: --ab, both A/B
        arms). Into a RUN-UNIQUE output (as do_tcensus), graded by the step's json item checks; --recompare re-runs it
        over the stored roll."""
        roll = self.path(step.target)
        if step.mode == "pacing":
            args = ["pacing", "--roll", roll, "--trace-summary", self.path(step.base, "_summary.json")]
        else:  # the roll's own stored pixel-grade table (do_roll's <roll>_freeze.txt): the presents it compared
            args = ["freeze", "--roll", roll, "--freeze-table", roll + "_freeze.txt",
                    "--min-presents", str(ZPD_AB_MIN_PRESENTS)] + \
                (["--ab", "--min-arm", str(ZPD_AB_MIN_ARM)] if step.mode == "freeze_ab" else [])
        if self.a.dry_run:
            js, _txt = self.reserve_output(step, "_zpdwin")
            return self.record(step, "DRY", "zpd_activity %s --json %s" % (" ".join(args), js), {"json": self.rel(js)})
        return self.guarded(step, lambda: self.zpdwin_body(step, args))

    def do_abwit(self, step):
        """tools/perf/mem_res_ab_check.py over one memres_ab roll (Astra review1 P2-4): the roll's log (the terminal
        [MEM_RES_AB] totals, the per-dumped-present arm lines) joined to its own stored pixel-grade table (do_roll's
        <roll>_freeze.txt: the pairs freeze_diff compared). Into a RUN-UNIQUE output (as do_zpdwin), graded by the
        step's json item checks; --recompare re-runs it over the stored roll."""
        roll = self.path(step.target)
        args = ["--roll", roll, "--freeze-table", roll + "_freeze.txt", "--min-pairs", str(MEMRES_AB_MIN_PAIRS)]
        if self.a.dry_run:
            js, _txt = self.reserve_output(step, "_abwit")
            return self.record(step, "DRY", "mem_res_ab_check %s --json %s" % (" ".join(args), js),
                               {"json": self.rel(js)})
        return self.guarded(step, lambda: self.abwit_body(step, args))

    def do_texid(self, step):
        """tools/perf/mem_res_texid.py over one memres_readers roll (textures_from_resolves; step.era = the seeded
        roll's --label era): RUN-UNIQUE output, graded by the step's json item checks; --recompare re-runs it."""
        return self.lane_grade(step, "mem_res_texid.py", "_texid")

    def do_readers(self, step):
        """tools/perf/mem_res_reader_census.py over one memres_readers roll (resolve_writeback_readers; the bounded
        probe): RUN-UNIQUE output, graded by the step's json item checks; --recompare re-runs it."""
        return self.lane_grade(step, "mem_res_reader_census.py", "_readers")

    def lane_grade(self, step, tool, suffix):
        log = run_log_of(self.path(step.target))
        args = [log or self.path(step.target, "/drive/attempt1/run.log")] + (["--label", step.era] if step.era else [])
        if self.a.dry_run:
            js, _txt = self.reserve_output(step, suffix)
            return self.record(step, "DRY", "%s %s --json %s" % (tool[:-3], " ".join(args), js), {"json": self.rel(js)})

        def body():
            js, txt = self.reserve_output(step, suffix)
            p = subprocess.run([sys.executable, os.path.join(HERE, tool)] + args + ["--json", js], capture_output=True,
                               text=True)
            with open(txt, "w", encoding="utf-8") as f:
                f.write("exit=%d\n%s%s" % (p.returncode, p.stdout or "", p.stderr or ""))
            d, why = self.tool_json(step, js, p, tool[:-3])
            if d is None:
                return self.finish(step, js, None, "FAIL", p.returncode, why, why)
            bad = [b for it in step.ledger for b in json_checks(d, [c for c in it["checks"] if not (
                c["field"].startswith("runner.") or c["field"] == "committed")])]
            detail = (p.stdout or "").strip().splitlines()[0] if (p.stdout or "").strip() else "no output"
            verdict = "PASS" if not bad else "FAIL"
            return self.finish(step, js, d, verdict, p.returncode,
                               detail + ("" if not bad else " | violated: " + ", ".join(bad)), ", ".join(bad))
        return self.guarded(step, body)

    def do_memcensus(self, step):
        """tools/perf/mem_res_census.py over one memres_bounds roll (boundaries_complete): the roll's census windows per
        era (step.era: the seeded roll's --label era, from the gameplay HUD to the end). Into a RUN-UNIQUE output (as
        do_abwit), graded by the step's json item checks (by_era.<era>.verdict / decisions / missed); --recompare
        re-runs it over the stored roll."""
        log = run_log_of(self.path(step.target))
        args = [log or self.path(step.target, "/drive/attempt1/run.log")] + (["--label", step.era] if step.era else [])
        if self.a.dry_run:
            js, _txt = self.reserve_output(step, "_memcensus")
            return self.record(step, "DRY", "mem_res_census %s --json %s" % (" ".join(args), js),
                               {"json": self.rel(js)})
        return self.guarded(step, lambda: self.memcensus_body(step, args))

    def memcensus_body(self, step, args):
        js, txt = self.reserve_output(step, "_memcensus")
        p = subprocess.run([sys.executable, os.path.join(HERE, "mem_res_census.py")] + args + ["--json", js],
                           capture_output=True, text=True)
        with open(txt, "w", encoding="utf-8") as f:
            f.write("exit=%d\n%s%s" % (p.returncode, p.stdout or "", p.stderr or ""))
        d, why = self.tool_json(step, js, p, "mem_res_census")
        if d is None:
            return self.finish(step, js, None, "FAIL", p.returncode, why, why)
        bad = [b for it in step.ledger for b in json_checks(d, [c for c in it["checks"] if not (
            c["field"].startswith("runner.") or c["field"] == "committed")])]
        eras = d.get("by_era") or {}
        detail = "eras " + ", ".join("%s=%s" % (k, (v or {}).get("verdict")) for k, v in sorted(eras.items()))
        verdict = "PASS" if not bad else "FAIL"
        return self.finish(step, js, d, verdict, p.returncode,
                           detail + ("" if not bad else " | violated: " + ", ".join(bad)), ", ".join(bad))

    def abwit_body(self, step, args):
        js, txt = self.reserve_output(step, "_abwit")
        p = subprocess.run([sys.executable, os.path.join(HERE, "mem_res_ab_check.py")] + args + ["--json", js],
                           capture_output=True, text=True)
        with open(txt, "w", encoding="utf-8") as f:
            f.write("exit=%d\n%s%s" % (p.returncode, p.stdout or "", p.stderr or ""))
        d, why = self.tool_json(step, js, p, "mem_res_ab_check")
        if d is None:
            return self.finish(step, js, None, "FAIL", p.returncode, why, why)
        bad = [b for it in step.ledger for b in json_checks(d, [c for c in it["checks"] if not (
            c["field"].startswith("runner.") or c["field"] == "committed")])]
        if p.returncode not in (0, 1):
            bad.insert(0, "mem_res_ab_check exit=%d (%s)" % (p.returncode, "; ".join(d.get("problems", [])[:2])))
        detail = (p.stdout or "").strip().splitlines()[0] if (p.stdout or "").strip() else "no output"
        verdict = "PASS" if not bad else "FAIL"
        return self.finish(step, js, d, verdict, p.returncode,
                           detail + ("" if not bad else " | violated: " + ", ".join(bad)), ", ".join(bad))

    def zpdwin_body(self, step, args):
        js, txt = self.reserve_output(step, "_zpdwin")
        p = subprocess.run([sys.executable, os.path.join(HERE, "zpd_activity.py")] + args + ["--json", js],
                           capture_output=True, text=True)
        with open(txt, "w", encoding="utf-8") as f:
            f.write("exit=%d\n%s%s" % (p.returncode, p.stdout or "", p.stderr or ""))
        d, why = self.tool_json(step, js, p, "zpd_activity")
        if d is None:
            return self.finish(step, js, None, "FAIL", p.returncode, why, why)
        bad = [b for it in step.ledger for b in json_checks(d, [c for c in it["checks"] if not (
            c["field"].startswith("runner.") or c["field"] == "committed")])]
        if p.returncode not in (0, 1):
            bad.insert(0, "zpd_activity exit=%d (%s)" % (p.returncode, d.get("why", "")))
        detail = (p.stdout or "").strip().splitlines()[-1] if (p.stdout or "").strip() else "no output"
        verdict = "PASS" if not bad else "FAIL"
        return self.finish(step, js, d, verdict, p.returncode,
                           detail + ("" if not bad else " | violated: " + ", ".join(bad)), ", ".join(bad))

    def do_gate(self, step):
        out = self.path("gate")
        txt = out + ".txt"
        cmd = [sys.executable, os.path.join(HERE, "flicker_gate.py"), "--repo", self.repo, "--instance", self.a.instance,
               "--exe", self.exe, "--out", out, "--gates", baked_gates(self.plan, step.gates)] + \
            fg.stage_cli(**staged(self))
        if save_of(self.repo, step):  # p2.zpd: the gate from a saved game (its own --from-save tape)
            cmd += ["--from-save", save_of(self.repo, step)]
            if getattr(step, "play", None):  # ... or the step's tape as is (p2.zpd: the zpd tape)
                cmd += ["--tape", tape_path(self.repo, step.play)]
        if self.a.dry_run:
            return self.record(step, "DRY", " ".join(cmd), {"gate": self.rel(out)})
        # p2.zpd (Astra merge review P2): the step's precondition on BOTH of the gate's rolls - unmet = VOID (the gate
        # kept as <gate>_noquery<k>, run again once), unmet twice = NO DATA, never PASS
        req = getattr(step, "require", None)
        met, seen = True, {}
        for attempt in range(REQUIRE_REROLLS + 1):
            safe_rmtree(out)
            self.write_stamp("gate")  # the gate's folder: both of its rolls are this exe's
            with open(txt, "w", encoding="utf-8") as f:
                exit_code = subprocess.call(cmd, cwd=self.repo, stdout=f, stderr=subprocess.STDOUT)
            if not req:
                break
            seen = {sub: require_met(run_log_of(os.path.join(out, sub)), req) for sub in ("freeze", "span")}
            met = all(m for m, _w in seen.values())
            if met or attempt == REQUIRE_REROLLS:
                break
            vdir = out + "_noquery%d" % (attempt + 1)
            while os.path.exists(vdir):
                vdir += "x"
            os.rename(out, vdir)
            os.replace(txt, vdir + ".txt")
            evidence = ["%s: %s %s" % (req["why"], sub, w) for sub, (m, w) in seen.items() if not m]
            self.voids.append({"step": step.tag, "roll": self.rel(vdir), "evidence": evidence, "rule": "require"})
            self.log("gate %s: VOID (precondition not met; kept as %s, run again once) | %s" % (
                step.tag, vdir, " | ".join(evidence)))
            self.write_proof()
        tail = open(txt, encoding="utf-8", errors="replace").read().strip().splitlines()
        last = tail[-1] if tail else "no output"
        passed = exit_code == 0 and last.strip() == "FLICKER_GATE PASS"
        detail = "exit=%d | %s" % (exit_code, " | ".join(tail[-3:]) if tail else last)
        if req:
            detail += " | precondition %s %s: %s" % (req.get("name", "require"), "met" if met else
                                                     "NOT MET twice - NO DATA",
                                                     ", ".join("%s %s" % (sub, w) for sub, (_m, w) in seen.items()))
        verdict = "PASS" if passed else "FAIL"
        if not met:
            verdict = "NO DATA"
        return self.record(step, verdict, detail, {"gate": self.rel(out)})

    def snapshot_log(self, base):
        """The run.log a snapshot step reads: a roll of this plan by tag, or one of the gate's rolls ("gate/<roll>")."""
        if base.startswith("gate/"):
            return run_log_of(os.path.join(self.path("gate"), base[5:]))
        return run_log_of(self.path(base))

    def do_snapshot(self, step):
        """p1.flush_residue's strict bar from boot through the last snapshot (tools/perf/flush_split_snapshot.py over the
        roll's run.log): graded by the step's json item checks - freshness relative to the last logged timestamp allows
        1.0 s (the last [NATIVE_FLUSH] split snapshot within fresh_bound_s = period + 0.5 s of the log's last
        timestamped line), the counters monotone, no residue in any snapshot. The remaining execution tail is
        unmeasured (no graceful quit). A comparison step since the merge with dev ce46c966: a RUN-UNIQUE output
        (reserve_output, guarded, the verdict stamped, committed last - do_tcensus's discipline), so an older run's
        snapshot JSON is never graded, and --recompare re-grades the stored rolls."""
        log = self.snapshot_log(step.base)
        if self.a.dry_run:
            js, _txt = self.reserve_output(step, "_snapshot")
            return self.record(step, "DRY", "flush_split_snapshot %s --json %s" % (log, js), {"json": self.rel(js)})
        return self.guarded(step, lambda: self.snapshot_body(step, log))

    def snapshot_body(self, step, log):
        js, txt = self.reserve_output(step, "_snapshot")
        p = subprocess.run([sys.executable, os.path.join(HERE, "flush_split_snapshot.py"), log, "--json", js],
                           capture_output=True, text=True)
        with open(txt, "w", encoding="utf-8") as f:
            f.write("exit=%d\n%s%s" % (p.returncode, p.stdout or "", p.stderr or ""))
        d, why = self.tool_json(step, js, p, "flush_split_snapshot")  # a missing log writes none (exit 2)
        if d is None:
            return self.finish(step, js, None, "FAIL", p.returncode, why, why)
        if p.returncode != 0 or "error" in d:  # no snapshot line (exit 2)
            why = d.get("error") or "flush_split_snapshot exit %d" % p.returncode
            return self.finish(step, js, d, "FAIL", p.returncode, why, why)
        bad = [b for it in step.ledger for b in json_checks(d, [c for c in it["checks"] if not (
            c["field"].startswith("runner.") or c["field"] == "committed")])]
        detail = snapshot_detail(d)
        verdict = "PASS" if not bad else "FAIL"
        return self.finish(step, js, d, verdict, p.returncode,
                           detail + ("" if not bad else " | violated: " + ", ".join(bad)), ", ".join(bad))

    def do_owner(self, step):
        """tools/perf/owner_census.py's base / target comparison of the producer census, graded by the step's json item's
        checks (the ledger re-reads the same JSON). The output is RUN-UNIQUE (reserve_output: <tag>.<UTC stamp>.json,
        created exclusively with a FAIL placeholder) and the item names it, so no other run's JSON is ever read (Astra
        rounds 2-4: no deletion, rename or refusal is needed). The step passes only when owner_census.py exits 0 AND
        replaced the placeholder with a JSON naming this step's base / target rolls whose checks hold; its verdict is
        stamped into the JSON (runner.verdict, the item's first check), so a failure - a nonzero exit after a passing
        JSON included - grades FAIL in the ledger for good, and a later passing run supersedes it as a witness. A pending
        FAIL row names the file before it is created, and the body runs guarded (Astra round 5): an exception anywhere -
        the reservation, the launch, the transcript - is the step's FAIL with its item exported; a non-object JSON
        (null, a list) is a FAIL."""
        a_dir, b_dir = self.path(step.base), self.path(step.target)
        if self.a.dry_run:
            js, _txt = self.reserve_output(step)
            cmd = [sys.executable, os.path.join(HERE, "owner_census.py"), "--base", a_dir, "--target", b_dir, "--json",
                   js] + self.terminal_args()
            return self.record(step, "DRY", " ".join(cmd), {"json": self.rel(js)})
        return self.guarded(step, lambda: self.owner_body(step, a_dir, b_dir))

    def owner_body(self, step, a_dir, b_dir):
        js, txt = self.reserve_output(step)
        cmd = [sys.executable, os.path.join(HERE, "owner_census.py"), "--base", a_dir, "--target", b_dir, "--json",
               js] + self.terminal_args()
        p = subprocess.run(cmd, capture_output=True, text=True)
        with open(txt, "w", encoding="utf-8") as f:
            f.write("exit=%d\n%s%s" % (p.returncode, p.stdout or "", p.stderr or ""))
        out = (p.stdout or "").strip() or (p.stderr or "").strip() or "no output"
        bad, d, parsed = [], None, False
        try:
            with open(js, encoding="utf-8") as f:
                d = json.load(f)
            parsed = True
        except (OSError, ValueError) as e:
            bad.append("unreadable json: %s" % e)
        if parsed and not isinstance(d, dict):  # Astra round 5: null, a list, a number - never validated as a PASS
            bad.append("the json is not an object (%s)" % type(d).__name__)
            d = None
        elif parsed and set(d) == {"runner"}:  # the reservation untouched
            bad.append("owner_census.py wrote no json")
            d = None
        elif parsed:
            if d.get("base_roll") != a_dir or d.get("target_roll") != b_dir:
                bad.append("json rolls %r / %r (want %r / %r)" % (d.get("base_roll"), d.get("target_roll"), a_dir, b_dir))
            else:
                for c in step.ledger[0]["checks"]:
                    if c["field"].startswith("runner.") or c["field"] == "committed":
                        continue
                    v = d
                    for k in c["field"].split("."):
                        v = v.get(k) if isinstance(v, dict) else None
                    if v != c["value"]:
                        bad.append("%s=%r (want %r)" % (c["field"], v, c["value"]))
        if p.returncode != 0:
            bad.insert(0, "owner_census.py exit=%d" % p.returncode)
        verdict = "PASS" if not bad else "FAIL"
        self.stamp_verdict(js, d if isinstance(d, dict) else {"base_roll": a_dir, "target_roll": b_dir, "ok": False},
                           verdict, p.returncode, ", ".join(bad))
        v = self.record(step, verdict, out + ("" if not bad else " | " + ", ".join(bad)), {"json": self.rel(js)})
        self.commit_output(js)  # LAST: the verdict counts from here on
        return v

    def bootequiv_cmd(self, step, js):
        cmd = [sys.executable, os.path.join(HERE, "boot_equiv.py")]
        if step.design:
            cmd += ["--design", step.design]
        if step.warmup:
            cmd += ["--warmup", self.path(step.warmup)]
        for pr in step.pairs:
            cmd += ["--pair"] + ["%s:%s" % (arm, self.path(tag)) for arm, tag in pr]
        return cmd + ["--json", js]

    def do_bootequiv(self, step):
        """tools/perf/boot_equiv.py (BOOT_EQUIV, design dcl-v1) over the plan's declared warm-up and boot pairs, as the
        owner step runs owner_census.py: a RUN-UNIQUE JSON (reserve_output: a pending FAIL row, then the file created
        exclusively with a FAIL placeholder), the tool's JSON validated - an object naming exactly this step's declared
        pairs and warm-up, the item's checks (verdict EQUIVALENT, tag BOOT_EQUIV, design, pairs) - the step's verdict
        stamped into it (runner.verdict) and `committed` written last. PASS only on EQUIVALENT with exit 0; OPEN and
        INCONCLUSIVE are a FAIL recorded for good. Runs even after a roll ended the plan (Step.always): the rolls taken
        are graded - a crash is OPEN - and the ones never run are named."""
        if self.a.dry_run:
            js, _txt = self.reserve_output(step)
            return self.record(step, "DRY", " ".join(self.bootequiv_cmd(step, js)), {"json": self.rel(js)})
        return self.guarded(step, lambda: self.bootequiv_body(step))

    def bootequiv_body(self, step):
        js, txt = self.reserve_output(step)
        p = subprocess.run(self.bootequiv_cmd(step, js), capture_output=True, text=True)
        with open(txt, "w", encoding="utf-8") as f:
            f.write("exit=%d\n%s%s" % (p.returncode, p.stdout or "", p.stderr or ""))
        last = ((p.stdout or "").strip().splitlines() or [(p.stderr or "").strip() or "no output"])[-1]
        bad, d = [], None
        try:
            with open(js, encoding="utf-8") as f:
                d = json.load(f)
        except (OSError, ValueError) as e:
            bad.append("unreadable json: %s" % e)
        key = lambda x: os.path.normcase(os.path.abspath(x))
        if d is not None and not isinstance(d, dict):
            bad.append("the json is not an object (%s)" % type(d).__name__)
            d = None
        elif isinstance(d, dict) and set(d) == {"runner"}:
            bad.append("boot_equiv.py wrote no json")
            d = None
        elif isinstance(d, dict):
            want = [[[arm, key(self.path(tag))] for arm, tag in pr] for pr in step.pairs]
            got = [[[arm, key(path)] for arm, path in pr] for pr in (d.get("declared") or [])]
            if got != want:
                bad.append("json pairs %s (want this step's %s)" % (d.get("declared"), want))
            if key(d.get("warmup_roll") or "") != key(self.path(step.warmup)):
                bad.append("json warm-up %r (want %r)" % (d.get("warmup_roll"), self.path(step.warmup)))
            bad += json_checks(d, [c for c in step.ledger[0]["checks"]
                                   if not (c["field"].startswith("runner.") or c["field"] == "committed")])
        if p.returncode != 0:
            bad.insert(0, "boot_equiv.py exit=%d" % p.returncode)
        verdict = "PASS" if not bad else "FAIL"
        self.stamp_verdict(js, d if isinstance(d, dict) else {"verdict": None, "ok": False}, verdict, p.returncode,
                           ", ".join(bad))
        v = self.record(step, verdict, last + ("" if not bad else " | " + ", ".join(bad)), {"json": self.rel(js)})
        self.commit_output(js)  # LAST: the verdict counts from here on
        return v

    def boot_path(self, tag):
        """A stall-pool tag's roll folder: a roll step's tag, or "gate/<roll>" - one of the flicker gate's two boots under
        the gate step's folder (snapshot_log's convention; native/devcreate-fix, Astra round 1: every ON boot includes
        the gate's)."""
        if tag.startswith("gate/"):
            return os.path.join(self.path("gate"), tag[5:])
        return self.path(tag)

    def stall_tags(self, step):
        """The declared ON roll tags the run took (a row that is not SKIP: DONE, PASS, ABORT and FAIL rolls all count); a
        gate boot "gate/<roll>" is taken when the gate step's row is (its boots are the gate's rows, proof_rows.gate_boots)."""
        rows = {r["step"]: r for r in self.results}
        return [t for t in step.pool if (rows.get(t.split("/", 1)[0]) or {}).get("verdict") not in (None, "SKIP")]

    def bootstall_cmd(self, step, js, tags):
        cmd = [sys.executable, os.path.join(HERE, "boot_stall.py"), "--proof", self.path("proof", ".json")]
        if step.design:
            cmd += ["--design", step.design]
        for k in step.allow_keys:
            cmd += ["--allow-key", k]
        for t in tags:
            cmd += ["--on", self.boot_path(t)]
        return cmd + ["--json", js]

    def do_bootstall(self, step):
        """tools/perf/boot_stall.py (BOOT_STALL, design dcl-stall-v1; Astra round 1, P1-1) over THIS run's proof JSON and
        every declared ON roll it took - the tool enumerates the run's ON boots itself and refuses a set that omits
        one - as do_bootequiv: a run-unique JSON, validated (this run's proof, exactly these ON rolls), the item's checks,
        the verdict stamped, `committed` last. PASS only on BOUNDED with exit 0. Runs even after a roll ended the plan
        (Step.always): a crashed ON boot is a stall event, never a skipped boot."""
        if self.a.dry_run:
            js, _txt = self.reserve_output(step)
            return self.record(step, "DRY", " ".join(self.bootstall_cmd(step, js, step.pool)), {"json": self.rel(js)})
        return self.guarded(step, lambda: self.bootstall_body(step))

    def bootstall_body(self, step):
        tags = self.stall_tags(step)
        js, txt = self.reserve_output(step)
        p = subprocess.run(self.bootstall_cmd(step, js, tags), capture_output=True, text=True)
        with open(txt, "w", encoding="utf-8") as f:
            f.write("exit=%d\n%s%s" % (p.returncode, p.stdout or "", p.stderr or ""))
        last = ((p.stdout or "").strip().splitlines() or [(p.stderr or "").strip() or "no output"])[-1]
        bad, d = [], None
        try:
            with open(js, encoding="utf-8") as f:
                d = json.load(f)
        except (OSError, ValueError) as e:
            bad.append("unreadable json: %s" % e)
        key = lambda x: os.path.normcase(os.path.abspath(x))
        if d is not None and not isinstance(d, dict):
            bad.append("the json is not an object (%s)" % type(d).__name__)
            d = None
        elif isinstance(d, dict) and set(d) == {"runner"}:
            bad.append("boot_stall.py wrote no json")
            d = None
        elif isinstance(d, dict):
            if [key(x) for x in d.get("proofs") or []] != [key(self.path("proof", ".json"))]:
                bad.append("json proofs %s (want this run's %s)" % (d.get("proofs"), self.path("proof", ".json")))
            if sorted(key(x) for x in d.get("on") or []) != sorted(key(self.boot_path(t)) for t in tags):
                bad.append("json ON boots %d (want this step's %d)" % (len(d.get("on") or []), len(tags)))
            bad += json_checks(d, [c for c in step.ledger[0]["checks"]
                                   if not (c["field"].startswith("runner.") or c["field"] == "committed")])
        if p.returncode != 0:
            bad.insert(0, "boot_stall.py exit=%d" % p.returncode)
        verdict = "PASS" if not bad else "FAIL"
        self.stamp_verdict(js, d if isinstance(d, dict) else {"verdict": None, "ok": False}, verdict, p.returncode,
                           ", ".join(bad))
        v = self.record(step, verdict, last + ("" if not bad else " | " + ", ".join(bad)), {"json": self.rel(js)})
        self.commit_output(js)  # LAST: the verdict counts from here on
        return v

    def do_xenia_zpd(self, step):
        """p2.zpd zpd7, the Xenia oracle: Xenia's trace-dump (--occlusion_query=strict) plays every frame the roll
        step.base traced (<roll>_xtr) and its reports are paired with our [NATIVE_ZPD_T] answers for the same frames
        (tools/perf/zpd_xenia.py; its JSON is the item). Needs a D3D12 device: the GPU, or WARP (--xenia-adapter -2)."""
        import zpd_xenia  # noqa: E402  (tools/perf)
        roll = self.path(step.base)
        js = self.path(step.tag, ".json")
        dump = getattr(self.a, "xenia_dump", None) or XENIA_DUMP
        adapter = getattr(self.a, "xenia_adapter", None)
        if self.a.dry_run:
            return self.record(step, "DRY", "zpd_xenia.py --roll %s --dump %s%s%s --golden-xtr %s%s --out %s" % (
                roll, dump, "" if os.path.exists(dump) else " (MISSING)",
                " --adapter %d" % adapter if adapter is not None else "", XENIA_GOLDEN[0],
                "" if os.path.exists(XENIA_GOLDEN[0]) else " (MISSING)", js), {"json": self.rel(js)})
        if not os.path.exists(dump):
            return self.record(step, "FAIL", "no Xenia trace-dump at %s (tools/xenia-trace/build_zpd_trace_dump.ps1)"
                               % dump, {"json": self.rel(js)})
        r = zpd_xenia.evaluate(roll, roll + "_xtr", os.path.splitext(js)[0] + "_dump", dump, adapter,
                               golden=XENIA_GOLDEN)
        with open(js, "w", encoding="utf-8") as f:
            json.dump(r, f, indent=1)
        detail = ("%s (%s): calibration %s%% off; %d frames, %d pairs, %d agree, %d unpaired; ours 0 / Xenia > 0: %d, "
                  "ours > 0 / Xenia 0: %d" % (
                      r["verdict"], r["why"], (r.get("calibration") or {}).get("changed_pct"), r["frames"], r["paired"],
                      r["agree"], r["unpaired"], r["ours_zero_xenia_nonzero"], r["ours_nonzero_xenia_zero"]))
        return self.record(step, r["verdict"], detail, {"json": self.rel(js)})

    def do_p2trace(self, step):
        src = self.path(step.base, "_trace.bin")
        js = self.path(step.tag, "_summary.json")
        txt = self.path(step.tag, "_summary.txt")
        win = ["--pacing-window", str(step.window)] if step.window else []  # a pacing roll's window (present ids)
        if self.a.dry_run:
            return self.record(step, "DRY", "p2_trace.py %s %s--json %s" % (src, " ".join(win) + " " if win else "", js),
                               {"json": self.rel(js)})
        # an older summary (and its stamp) never stands in for this one: the pacing step reads the summary by name
        for p_old in (js, txt, self.stamp_path(step.tag)):
            if os.path.exists(p_old):
                os.remove(p_old)
        if not os.path.exists(src):
            return self.record(step, "FAIL", "no trace file %s" % src)
        base_stamp = self.stamp_of(step.base)
        p = subprocess.run([sys.executable, os.path.join(HERE, "p2_trace.py"), src] + win + ["--json", js],
                           capture_output=True, text=True)
        open(txt, "w", encoding="utf-8").write(p.stdout + p.stderr)
        if base_stamp is not None and "error" not in base_stamp:
            self.write_stamp(step.tag, dict(base_stamp, source="the %s roll's stamp" % step.base))
        try:
            d = json.load(open(js, encoding="utf-8"))
        except (OSError, ValueError) as e:
            return self.record(step, "FAIL", "no summary json: %s" % e)
        bad = []
        for c in step.ledger[0]["checks"]:
            v = d
            for k in c["field"].split("."):
                v = v.get(k) if isinstance(v, dict) else None
            want, op = c["value"], c.get("op", "eq")
            ok = {"eq": lambda: v == want, "ne": lambda: v != want,
                  "gt": lambda: v is not None and v > want, "ge": lambda: v is not None and v >= want,
                  "lt": lambda: v is not None and v < want, "le": lambda: v is not None and v <= want}[op]()
            if not ok:
                bad.append("%s=%r (want %s %r)" % (c["field"], v, op, want))
        return self.record(step, "PASS" if not bad else "FAIL", ("invariants hold" if not bad else "violated: " + ", ".join(bad)),
                           {"json": self.rel(js)})

    # -- the plan
    def fresh_out_refusal(self):
        """'' or why a `fresh_out` plan may not run live into --out: its proof JSON or a roll folder of it is there
        already (Astra round 1, P1-4: the ledger cites the rolls by path; a second attempt would overwrite them)."""
        if not self.plan.get("fresh_out"):
            return ""
        taken = [p for p in [self.path("proof", ".json")] + [self.path(s.tag) for s in self.plan["steps"]
                                                            if s.kind == "roll"] if os.path.exists(p)]
        return ("plan %s runs only into a fresh --out (its rolls are evidence cited by path, never overwritten): %s "
                "already hold%s %s" % (self.a.plan, self.out, "s" if len(taken) == 1 else "",
                                      ", ".join(os.path.basename(p) for p in taken[:4]) +
                                      (" ..." if len(taken) > 4 else ""))) if taken else ""

    def env_refusal(self):
        """'' or why a plan whose arms grade a DEFAULT (its "default_keys") may not run live from here: the parent
        environment carries such a key, and perf_run passes the parent environment to every roll, so a default arm would
        run the explicit form (Astra round 1 on native/dreg-bake, P2). Refused, never stripped: the operator's
        environment is not edited behind their back."""
        keys = [k for k in self.plan.get("default_keys", ()) if k in os.environ]
        if not keys:
            return ""
        return ("plan %s grades the DEFAULT of %s, but the parent environment sets %s - perf_run would pass it to every "
                "roll; unset it and run again" % (self.a.plan, ", ".join(self.plan["default_keys"]),
                                                  ", ".join("%s=%s" % (k, os.environ[k]) for k in keys)))

    def run(self):
        self.abort = None
        if not self.a.dry_run:
            refused = self.env_refusal() or self.fresh_out_refusal() or self.mixed_out_refusal()
            if refused:
                raise SystemExit("REFUSED: " + refused)
            self.adopt_attested()  # before the proof below replaces the stored rows that attest the older rolls
            self.write_proof()  # first thing: an unwritable proof stops the run before any step (ProofWriteError)
        else:
            for why in (self.env_refusal(), self.fresh_out_refusal(), self.mixed_out_refusal()):
                if why:
                    self.log("NOTE: %s (a live run there is refused)" % why)
        only = set(self.a.only.split(",")) if self.a.only else None
        skip = set(self.a.skip.split(",")) if self.a.skip else set()
        self.log("plan %s (%s) exe=%s id=%s base=%s repo=%s out=%s%s" % (
            self.a.plan, self.plan["about"], self.exe, self.exe_id, self.base, self.repo, self.out, " DRY RUN" if self.a.dry_run else ""))
        held = self.a.slot_held or self.a.dry_run
        if not held:
            r = self.slot("acquire", "--owner", self.a.owner, "--poll", "30", "--note",
                          "native-a proof %s (%d steps)" % (self.a.plan, len(self.plan["steps"])))
            if r.returncode != 0:
                raise SystemExit("game slot not acquired (exit %d)" % r.returncode)
            self.log("slot acquired as %s" % self.a.owner)
        try:
            for step in self.plan["steps"]:
                if (only and step.tag not in only) or step.tag in skip:
                    self.record(step, "SKIP", "by --only / --skip")
                    continue
                if self.abort and not step.always:  # an `always` step grades the rolls taken (a crash is graded)
                    self.record(step, "SKIP", "plan aborted: " + self.abort)
                    continue
                why = self.exe_refusal(step)
                if why:  # a one-exe plan's comparison over another exe's roll: FAIL, no item
                    self.record(step, "FAIL", "REFUSED: " + why, refused=True)
                    continue
                getattr(self, "do_" + step.kind)(step)
                if not held:
                    self.slot("heartbeat", "--owner", self.a.owner)
        finally:
            if not held:
                self.slot("release", "--owner", self.a.owner)
                self.log("slot released")
        verdicts = [r["verdict"] for r in self.results]
        summary = "plan %s: %s" % (self.a.plan, ", ".join("%s=%s" % (r["step"], r["verdict"]) for r in self.results))
        self.log(summary)
        self.complete = True
        if not self.a.dry_run:
            self.write_proof()
        return 0 if all(v in ("PASS", "DONE", "DRY", "SKIP") for v in verdicts) else 1

    def reload(self):
        """--reapply: the step results of the plan's stored proof document stand in for a run (its exe id too)."""
        proof = self.path("proof", ".json")
        if os.path.exists(proof + UNWRITABLE):
            raise SystemExit("REFUSED: the last run on %s could not write its proof (%s) - %s is an older run's; "
                             "not reapplied" % (self.out, open(proof + UNWRITABLE, encoding="utf-8").read().strip(), proof))
        doc = json.load(open(proof, encoding="utf-8"))
        if doc.get("plan") != self.a.plan:
            raise SystemExit("%s holds plan %s, not %s" % (self.path("proof", ".json"), doc.get("plan"), self.a.plan))
        if self.exe_sha and doc.get("exe_id") and doc["exe_id"] != "?" and self.exe_id != doc["exe_id"]:
            # Astra round 1 on native/transport-bake: the stored rows ran on the proof's exe; another --exe would label
            # (and, in a one-exe plan, check) them as its own
            raise SystemExit("REFUSED: %s ran on exe %s; --exe %s is %s" % (proof, doc["exe_id"], self.exe, self.exe_id))
        self.results = list(doc.get("steps") or [])
        self.exe_id = doc.get("exe_id") or self.exe_id  # without --exe the stamps are compared by this 12-hex id
        self.complete = doc.get("complete") is not False  # a proof from before the field: as complete as it was
        self.log("plan %s: reapplying %d stored step results (exe %s)" % (self.a.plan, len(self.results), self.exe_id))
        verdicts = [r["verdict"] for r in self.results]
        if doc.get("complete") is False:  # a run that did not finish (a crash): its pending rows are FAILs
            self.log("plan %s: the stored proof is INCOMPLETE (the run did not finish)" % self.a.plan)
            return 1
        return 0 if all(v in ("PASS", "DONE", "DRY", "SKIP") for v in verdicts) else 1

    def recompare(self):
        """--recompare: no rolls. The stored proof's rows stand (as --reapply), and the plan's comparison steps (owner,
        census, tcensus, pacing; --only narrows them) run again over the rolls already in --out - each a NEW run-unique output under the
        current rules, its old row replaced, its old file left unreferenced. For a proof whose comparison predates a
        rule change (dc5d's census_cmp JSON has no commit marker) without re-rolling the game."""
        self.reload()
        only = set(self.a.only.split(",")) if self.a.only else None
        kinds = RECOMPARE_KINDS + tuple(self.plan.get("recompare_extra", ()))
        for step in self.plan["steps"]:
            if step.kind not in kinds or (only and step.tag not in only):
                continue
            self.results[:] = [r for r in self.results if r["step"] != step.tag]
            self.log("recompare %s over the stored rolls" % step.tag)
            why = self.exe_refusal(step)
            if why:
                self.record(step, "FAIL", "REFUSED: " + why, refused=True)
                continue
            getattr(self, "do_" + step.kind)(step)
        if not self.a.dry_run:
            self.write_proof()
        verdicts = [r["verdict"] for r in self.results]
        return 0 if self.complete and all(v in ("PASS", "DONE", "DRY", "SKIP") for v in verdicts) else 1

    # -- the ledger items with their paths
    def items(self):
        """[(obligation id, evidence item with its path)] for every step that ran (or every step, dry). A comparison
        step's (owner, census, tcensus, pacing) item names the run-unique JSON its row recorded (reserve_output)."""
        out = []
        by_tag = {r["step"]: r for r in self.results}
        today = datetime.date.today().isoformat()
        for step in self.plan["steps"]:
            r = by_tag.get(step.tag)
            # pacing's INCONCLUSIVE: no evidence either way, no item; a census's INCONCLUSIVE (an unattributed allowed
            # packet) exports its item, which grades FAIL - it supersedes an earlier grader's PASS, never hides behind
            # it; a refused step (another exe's inputs) yields none
            if r is None or r["verdict"] == "SKIP" or r.get("refused") or \
                    (r["verdict"] == "INCONCLUSIVE" and step.kind == "pacing"):
                continue
            for item in step.ledger:
                it = dict(item)
                if step.kind == "replay":
                    it["path"] = self.rel(self.path("replay_cmp", ".txt"))
                elif step.kind == "roll":
                    if it["kind"] in LOG_KINDS:
                        it["path"] = self.rel(run_log_of(self.path(step.tag)))
                    else:
                        it["path"] = self.rel(self.path(step.tag))
                elif step.kind == "meter" and step.pool:  # its receipt: the row's own run-unique JSON, or a name no
                    # run writes (an exception before the reservation: unavailable, never another run's receipt)
                    it = self.pooled_item(step, it, (r.get("paths") or {}).get("json") or
                                          self.rel(self.path(step.tag, ".NO-OUTPUT.json")))
                elif step.kind == "meter":
                    it["path"] = self.rel(self.path(step.target))
                    it["base"] = self.rel(self.path(step.base))
                elif step.kind == "gate":
                    if it["kind"] in LOG_KINDS:  # a counter line of one of the gate's two rolls (it["roll"]: freeze / span)
                        it["path"] = self.rel(run_log_of(os.path.join(self.path("gate"), it.pop("roll", "freeze"))))
                    else:
                        it["path"] = self.rel(self.path("gate"))
                elif step.kind == "p2trace":
                    it["path"] = self.rel(self.path(step.tag, "_summary.json"))
                elif step.kind == "xenia_zpd":
                    it["path"] = self.rel(self.path(step.tag, ".json"))
                elif step.kind in COMPARISON_KINDS:  # this run's own output; never another name (a row without
                    # one - an exception before its name existed - points at a name no run writes: unavailable)
                    it["path"] = (r.get("paths") or {}).get("json") or self.rel(self.path(step.tag, ".NO-OUTPUT.json"))
                note = "%s %s on exe %s (base with %s, world rule)" % (today, self.a.plan, self.exe_id, RACE_FIX)
                it["note"] = (it.get("note") + "; " + note) if it.get("note") else note
                for oid in self.item_oids(it):
                    out.append((oid, it))
            out += self.terminal_items(step)
        return out

    def item_oids(self, it):
        """The obligations an item goes to: the plan's, or - an item that names its own (`obligation`, one of the plan's:
        boot gaps, whose producers are two obligations' and whose meter / era evidence two more's) - that one alone.
        The key is taken off the item (the ledger's item carries no route)."""
        own = it.pop("obligation", None)
        if own is None:
            return list(self.plan["obligations"])
        if own not in self.plan["obligations"]:
            raise KeyError("item %s routes to %s, not one of the plan's obligations %s" % (
                it.get("id"), own, self.plan["obligations"]))
        return [own]

    def graceful(self):
        return bool(getattr(self.a, "graceful_quit", False))

    def stranded_policy(self):
        """native/quit-drain round 2: the plan's EXPLICIT acceptance policy for a thread stranded in a terminal snapshot
        (tools/perf/terminal_snapshot.py STRANDED_POLICIES): the plan key "stranded_policy", "allow" when it has none -
        the census counts close at the ring cut and the stranded thread's dropped work is after it - or "forbid".
        Recorded in the proof JSON and in every terminal item's note; passed to owner_census / residue_census."""
        p = self.plan.get("stranded_policy", "allow")
        if p not in ("allow", "forbid"):
            raise SystemExit("plan %s: stranded_policy must be allow or forbid, not %r" % (self.a.plan, p))
        return p

    def terminal_args(self):
        """owner_census / residue_census: grade each roll's terminal report (--graceful-quit only), under the plan's
        stranded policy."""
        return ["--require-terminal", "--stranded-policy", self.stranded_policy()] if self.graceful() else []

    def terminal_items(self, step):
        """--graceful-quit (tools/graceful-quit): the ADDED items of a roll (and of each gate roll) with text items:
        it ended on a complete terminal snapshot (<id>.terminal), the game's own clean exit 0 says so
        (<id>.quit_receipt: quit.json beside the run.log), and EVERY original text item gets its terminal twin
        (<item id>.terminal: the same predicate on the snapshot's line of the same family, terminal_twin) - so a
        terminal BAD bytes=1 fails the obligation even when the last periodic line said 0 (Astra round 1, P2-5). The
        pre-registered items are unchanged. Nothing without --graceful-quit."""
        if step.kind not in ("roll", "gate") or not (self.graceful() or step_graceful(step)):
            return []
        texts = [i for i in step.ledger if i.get("kind") == "text"]
        if not texts:
            return []
        note = "%s %s on exe %s, ended by the graceful quit" % (datetime.date.today().isoformat(), self.a.plan,
                                                                 self.exe_id)
        new, rolls = [], []
        routes = {}  # roll label -> the obligations its routed items go to (the roll's terminal / receipt items follow)
        for item in texts:
            if step.kind == "roll":
                log, label = run_log_of(self.path(step.tag)), step.tag
            else:  # a gate roll: freeze / span
                roll = item.get("roll", "freeze")
                log, label = run_log_of(os.path.join(self.path("gate"), roll)), "%s.%s" % (step.tag, roll)
            if label not in [r[0] for r in rolls]:
                rolls.append((label, log))
            oids = self.item_oids(dict(item))
            routes.setdefault(label, [])
            routes[label] += [o for o in oids if o not in routes[label]]
            # a boot-time latch line (ONCE_PER_PROCESS: printed once, when its gate latches) has no line in the terminal
            # snapshot - no family printer repeats it - so its twin would grade a line the exe never prints: none
            tw = None if item.get("select") in ONCE_PER_PROCESS else terminal_twin(item, "%s.terminal" % item["id"], note)
            if tw is None:
                continue
            tw["path"] = self.rel(log)
            tw.pop("obligation", None)
            new.append((tw, oids))
        heads, out = [], []
        # native/quit-drain round 2: the stranded policy is EXPLICIT - the plan's (allow unless it says forbid), named in
        # each head item's note and applied to both (forbid: the complete line's stranded=0 and the receipt's stranded 0)
        policy = self.stranded_policy()
        forbid = policy == "forbid"
        hnote = "%s; stranded policy %s" % (note, policy)
        for label, log in rolls:
            base = "%s.%s" % (self.plan["prefix"], label)
            done = terminal_complete_item(base + ".terminal", hnote, forbid_stranded=forbid)
            done["path"] = self.rel(log)
            rec = quit_receipt_item(base + ".quit_receipt", hnote, forbid_stranded=forbid)
            rec["path"] = self.rel(os.path.join(os.path.dirname(log), "quit.json"))
            oids = [o for o in self.plan["obligations"] if o in routes[label]]
            heads += [(done, oids), (rec, oids)]
        for it, oids in heads + new:
            for oid in oids:
                out.append((oid, it))
        return out

    def nodata_ids(self):
        """The item ids of every step whose verdict is NO DATA (its precondition unmet twice: Step.require, do_roll).
        apply_ledger records them - the ledger grades them NO DATA through their own `require` - but never lets them
        count toward an unknown or a resolver."""
        by_tag = {r["step"]: r for r in self.results}
        return {it["id"] for step in self.plan["steps"] if (by_tag.get(step.tag) or {}).get("verdict") == "NO DATA"
                for it in step.ledger}


# ------------------------------------------------------------------------------------------ the ledger text

def dumps_item(item):
    return json.dumps(item, ensure_ascii=False)


class LedgerText:
    """Line-level edits of docs/native_ledger.json in the file's own style (one evidence / witness item per line
    at a fixed indent), so a proof adds a few lines and the diff shows exactly them. Every edit re-parses the JSON."""

    IND_OBL, IND_KEY, IND_ITEM, IND_CKEY, IND_CITEM = "    ", "      ", "        ", "          ", "            "

    def __init__(self, text):
        self.lines = text.split("\n")

    def text(self):
        return "\n".join(self.lines)

    def check(self):
        json.loads(self.text())

    # -- locating
    def obligation(self, oid):
        """(start, end) line indexes of the obligation dict: start = its '"id"' line, end = its closing '    }' line."""
        want = '"id": "%s",' % oid
        for i, ln in enumerate(self.lines):
            if ln.startswith(self.IND_KEY + '"id"') and ln.strip() == want and not ln.startswith(self.IND_KEY + " "):
                for j in range(i, len(self.lines)):
                    if self.lines[j] in (self.IND_OBL + "}", self.IND_OBL + "},"):
                        return i, j
                break
        raise KeyError("obligation %s not found" % oid)

    def list_block(self, start, end, key, ind=None):
        """(open, close) line indexes of `"key": [` ... `]` at indent `ind` inside [start, end)."""
        ind = self.IND_KEY if ind is None else ind
        opener = ind + '"%s": [' % key
        for i in range(start, end):
            if self.lines[i] in (opener + "]", opener + "],"):  # an inline empty list: opened in place (2026-10-11)
                comma = self.lines[i].endswith(",")
                self.lines[i:i + 1] = [opener, ind + "]" + ("," if comma else "")]
                return i, i + 1
            if self.lines[i] == opener:
                for j in range(i + 1, end):
                    if self.lines[j] in (ind + "]", ind + "],"):
                        return i, j
        return None, None

    # -- comma hygiene
    def _insert_before_close(self, close, new_line):
        prev = close - 1
        if not self.lines[prev].rstrip().endswith("[") and not self.lines[prev].rstrip().endswith(","):
            self.lines[prev] = self.lines[prev].rstrip() + ","
        self.lines.insert(close, new_line)

    def _remove_item(self, idx, close):
        del self.lines[idx]
        close -= 1
        prev = close - 1
        if self.lines[prev].rstrip().endswith(",") and not self.lines[prev].rstrip().endswith("["):
            self.lines[prev] = self.lines[prev].rstrip()[:-1]

    # -- edits
    def find_item(self, open_, close, eid, ind):
        for i in range(open_ + 1, close):
            if self.lines[i].startswith(ind + '{"id": "%s",' % eid):
                return i
        # a one-line item whose "id" is not its first key (terminal twins before 2026-10-15) - else it is
        # inserted a second time and the ledger rejects the duplicate id
        want = '"id": "%s"' % eid
        for i in range(open_ + 1, close):
            ln = self.lines[i]
            if ln.startswith(ind + "{") and not ln.startswith(ind + " ") and want in ln:
                body = ln.strip().rstrip(",")
                if body.endswith("}"):
                    try:
                        if json.loads(body).get("id") == eid:
                            return i
                    except ValueError:
                        pass
        return None

    def upsert_evidence(self, oid, item, superseded_fails=None, into=None, void_expect="fail"):
        """Replace / insert `item` in the obligation's evidence. A superseded item that fails (its id in
        superseded_fails) moves to a contradiction witness as <id>_void - into contradiction `into` when given and
        present, else the first. -> ('replaced'|'inserted'|'moved', old)."""
        s, e = self.obligation(oid)
        o, c = self.list_block(s, e, "evidence")
        if o is None:
            raise KeyError("%s has no evidence list" % oid)
        line = self.IND_ITEM + dumps_item(item)
        i = self.find_item(o, c, item["id"], self.IND_ITEM)
        if i is None:
            self._insert_before_close(c, line)
            self._fix_commas(o)
            self.check()
            return "inserted", None
        old_text = self.lines[i].strip().rstrip(",")
        old = json.loads(old_text)
        self.lines[i] = line + ("," if self.lines[i].rstrip().endswith(",") else "")
        self.check()
        if superseded_fails and item["id"] in superseded_fails:
            void = dict(old)
            # A unique witness id: a second supersession of the same step (the row-3 stress re-run after the
            # black-world void) gets <id>_void2, <id>_void3 ... - the ledger rejects duplicate ids.
            base, cand, n = item["id"] + "_void", item["id"] + "_void", 2
            s2, e2 = self.obligation(oid)
            taken = "\n".join(self.lines[s2:e2 + 1])
            while ('.%s"' % cand) in taken or ('"id": "%s"' % cand) in taken:  # add_witness prefixes <cid>.
                cand = "%s%d" % (base, n)
                n += 1
            void["id"] = cand
            # p2.zpd zpdx3: a superseded item whose roll had no data (its `require` unmet) is kept as what it is,
            # expect no_data - NO DATA never 'fails as expected' (tools/native_ledger.py check_item)
            void["expect"] = void_expect if void_expect in ("fail", "no_data") else "fail"
            void["note"] = (void.get("note", "") + "; " if void.get("note") else "") + "superseded by %s" % item.get("path", "?")
            self.add_witness(oid, void, into)
            return "moved", old
        return "replaced", old

    def _fix_commas(self, open_):
        """Every item line inside the list gets a trailing comma except the last."""
        depth_close = None
        ind = self.lines[open_][:len(self.lines[open_]) - len(self.lines[open_].lstrip())]
        for j in range(open_ + 1, len(self.lines)):
            if self.lines[j] in (ind + "]", ind + "],"):
                depth_close = j
                break
        if depth_close is None:
            return
        items = [k for k in range(open_ + 1, depth_close) if self.lines[k].startswith(ind + "  {") and
                 (self.lines[k].rstrip().endswith("}") or self.lines[k].rstrip().endswith("},"))]
        for n, k in enumerate(items):
            body = self.lines[k].rstrip().rstrip(",")
            self.lines[k] = body + ("," if n < len(items) - 1 else "")

    def add_witness(self, oid, item, cid_want=None):
        """Append `item` to the witness list of the obligation's contradiction `cid_want` (when present) or its first;
        create the contradiction when the record has none (resolved by the race fix being on HEAD)."""
        s, e = self.obligation(oid)
        o, c = self.list_block(s, e, "contradictions")
        if o is None:
            cid = "superseded_black"
            eo, ec = self.list_block(s, e, "evidence")
            closing_comma = self.lines[ec].rstrip().endswith(",")
            block = [
                self.IND_KEY + '"contradictions": [',
                self.IND_ITEM + "{",
                self.IND_CKEY + '"id": "%s",' % cid,
                self.IND_CKEY + '"text": "rolls cited for this record graded black frames (the startup race, fixed %s) or wedged; superseded by the re-proof on a base with the fix, kept here as witnesses",' % RACE_FIX,
                self.IND_CKEY + '"witness": [',
                self.IND_CITEM + dumps_item(dict(item, id="%s.%s" % (cid, item["id"]))),
                self.IND_CKEY + "],",
                self.IND_CKEY + '"resolved_by": [',
                self.IND_CITEM + dumps_item({"id": "%s.race_fix" % cid, "kind": "git_ancestor", "commit": RACE_FIX, "authority": "static",
                                             "observes": "build", "note": "the startup-race fix on HEAD: the void rolls predate it"}),
                self.IND_CKEY + "]",
                self.IND_ITEM + "}",
                self.IND_KEY + "]" + ("," if closing_comma else ""),
            ]
            self.lines[ec] = self.IND_KEY + "],"
            self.lines[ec + 1:ec + 1] = block
            self.check()
            return "created"
        # the wanted (else the first) contradiction's id + its own witness list
        ids = []
        for i in range(o + 1, c):
            m = re.match(r'^\s{10}"id": "([^"]+)",$', self.lines[i])
            if m:
                ids.append((m.group(1), i))
        pick = [x for x in ids if x[0] == cid_want] or ids[:1]
        cid, at = pick[0] if pick else (None, o)
        end = next((j for j in range(at, c) if self.lines[j] in (self.IND_ITEM + "}", self.IND_ITEM + "},")), c)
        wo, wc = self.list_block(at, end, "witness", self.IND_CKEY)
        if wo is None or cid is None:
            raise KeyError("%s: no witness list in its first contradiction" % oid)
        self._insert_before_close(wc, self.IND_CITEM + dumps_item(dict(item, id="%s.%s" % (cid, item["id"]))))
        self._fix_commas(wo)
        self.check()
        return "appended"

    def bind_resolver(self, oid, rid, item):
        """The contradiction resolver `rid` (an item line inside obligation `oid`) takes the applied `item`'s path and
        checks - a run-unique output's name is only known when the run is applied (plan resolver_paths)."""
        s, e = self.obligation(oid)
        want = '"id": "%s"' % rid
        hits = [i for i in range(s, e) if self.lines[i].lstrip().startswith("{") and want in self.lines[i]]
        if len(hits) != 1:
            raise KeyError("%s: resolver %s found %d times" % (oid, rid, len(hits)))
        i = hits[0]
        raw = self.lines[i].rstrip()
        ind = raw[:len(raw) - len(raw.lstrip())]
        comma = raw.endswith(",")
        d = json.loads(raw.strip().rstrip(","))
        d["path"] = item["path"]
        if "checks" in item:
            d["checks"] = item["checks"]
        d["note"] = "%s's applied run: %s" % (item["id"], item.get("note", ""))
        self.lines[i] = ind + dumps_item(d) + ("," if comma else "")
        self.check()
        return "bound to " + item["path"]

    # -- multi-line contradictions / unknowns (native/devcreate-close, 2026-10-14)
    def _obj_in_list(self, o, c, wanted):
        """(open, close) line indexes of the multi-line object of list block (o, c) whose '"id": "<wanted>",' line sits
        at IND_CKEY ('{' at IND_ITEM on the line before it), or (None, None)."""
        for i in range(o + 1, c):
            if self.lines[i] == self.IND_CKEY + '"id": "%s",' % wanted and self.lines[i - 1] == self.IND_ITEM + "{":
                for j in range(i, c):
                    if self.lines[j] in (self.IND_ITEM + "}", self.IND_ITEM + "},"):
                        return i - 1, j
        return None, None

    def unique_id(self, oid, base):
        """base, or base2, base3 ... - an id no line of the obligation uses yet."""
        s, e = self.obligation(oid)
        taken = "\n".join(self.lines[s:e + 1])
        cand, n = base, 2
        while ('"id": "%s"' % cand) in taken:
            cand, n = "%s%d" % (base, n), n + 1
        return cand

    def evidence_item(self, oid, eid):
        """The obligation's evidence item `eid` (a one-line item), or None."""
        s, e = self.obligation(oid)
        o, c = self.list_block(s, e, "evidence")
        i = None if o is None else self.find_item(o, c, eid, self.IND_ITEM)
        return None if i is None else json.loads(self.lines[i].strip().rstrip(","))

    def line_item(self, oid, eid):
        """The one-line item with id `eid` anywhere inside obligation `oid` (evidence, witness, resolved_by), or None."""
        s, e = self.obligation(oid)
        hits = [i for i in range(s, e) if self.lines[i].lstrip().startswith('{"id": "%s",' % eid)]
        return json.loads(self.lines[hits[0]].strip().rstrip(",")) if len(hits) == 1 else None

    def ensure_contradiction(self, oid, cid, text, resolver):
        """Contradiction `cid` exists with exactly one resolver - a copy of `resolver` as <cid>.<id>: created (text, an
        empty witness list) after the record's last contradiction, or its resolved_by rebound. -> 'created' / 'rebound'."""
        s, e = self.obligation(oid)
        o, c = self.list_block(s, e, "contradictions")
        if o is None:
            raise KeyError("%s has no contradictions list" % oid)
        line = self.IND_CITEM + dumps_item(dict(resolver, id="%s.%s" % (cid, resolver["id"])))
        a, b = self._obj_in_list(o, c, cid)
        if a is not None:
            ro, rc = self.list_block(a, b + 1, "resolved_by", self.IND_CKEY)
            if ro is None:
                raise KeyError("%s/%s has no resolved_by list" % (oid, cid))
            self.lines[ro + 1:rc] = [line]
            self.check()
            return "rebound"
        if c - 1 > o and self.lines[c - 1] == self.IND_ITEM + "}":
            self.lines[c - 1] += ","
        self.lines[c:c] = [self.IND_ITEM + "{", self.IND_CKEY + '"id": "%s",' % cid,
                           self.IND_CKEY + '"text": %s,' % json.dumps(text, ensure_ascii=False),
                           self.IND_CKEY + '"witness": [', self.IND_CKEY + "],",
                           self.IND_CKEY + '"resolved_by": [', line, self.IND_CKEY + "]", self.IND_ITEM + "}"]
        self.check()
        return "created"

    def retire_evidence(self, oid, eid, cid, note):
        """Move evidence item `eid` into contradiction `cid`'s witnesses (id <cid>.<eid>, "expect": "fail", `note`
        appended) - never deleted. -> 'moved' / 'absent'."""
        s, e = self.obligation(oid)
        o, c = self.list_block(s, e, "evidence")
        i = self.find_item(o, c, eid, self.IND_ITEM)
        if i is None:
            return "absent (already retired)"
        old = json.loads(self.lines[i].strip().rstrip(","))
        self._remove_item(i, c)
        self.check()
        w = dict(old, expect="fail", note=(old.get("note", "") + "; " if old.get("note") else "") + note)
        w["id"] = self.unique_id(oid, "%s.%s" % (cid, eid))[len(cid) + 1:]
        self.add_witness(oid, w, cid)
        return "moved into %s" % cid

    def unknown_witness(self, oid, uid, item):
        """Append `item` (its id as given) to residual unknown `uid`'s witness list - the multi-line form (a 'witness'
        list at IND_CKEY) or a one-line unknown."""
        s, e = self.obligation(oid)
        o, c = self.list_block(s, e, "residual_unknowns")
        if o is None:
            raise KeyError("%s has no residual_unknowns" % oid)
        a, b = self._obj_in_list(o, c, uid)
        if a is not None:
            wo, wc = self.list_block(a, b + 1, "witness", self.IND_CKEY)
            if wo is None:
                raise KeyError("%s/%s has no witness list" % (oid, uid))
            self._insert_before_close(wc, self.IND_CITEM + dumps_item(item))
            self._fix_commas(wo)
        else:
            i = self.find_item(o, c, uid, self.IND_ITEM)
            if i is None:
                raise KeyError("%s: residual unknown %s not found" % (oid, uid))
            raw = self.lines[i].rstrip()
            d = json.loads(raw.rstrip(","))
            d.setdefault("witness", []).append(item)
            self.lines[i] = self.IND_ITEM + dumps_item(d) + ("," if raw.endswith(",") else "")
        self.check()
        return "kept as witness %s" % item["id"]

    def resolve_unknown(self, oid, uid, items, rebind=False):
        """Give residual unknown `uid` its resolved_by (copies of `items`, ids <uid>.<id>) when it has none. rebind
        (--rebind-resolves; Astra round 1 on native/zpd-bake, the orchestrator's pre-registered regrade): an unknown
        already resolved has its resolved_by REPLACED by this run's items whatever they grade - a recompare's stricter
        witnesses stand, so a stricter FAIL re-opens it (the ledger reads an unknown resolved only while every resolver
        passes). One-line unknowns only; the replaced copies stay as the evidence items they copied."""
        s, e = self.obligation(oid)
        o, c = self.list_block(s, e, "residual_unknowns")
        if o is None:
            raise KeyError("%s has no residual_unknowns" % oid)
        i = self.find_item(o, c, uid, self.IND_ITEM)
        if i is None:  # a pretty-printed unknown: '{' alone, its "id" on the next line (crb1 2026-10-15)
            i = next((k for k in range(o + 1, c - 1) if self.lines[k] == self.IND_ITEM + "{"
                      and self.lines[k + 1].startswith(self.IND_ITEM + '  "id": "%s",' % uid)), None)
        if i is None:
            raise KeyError("%s: residual unknown %s not found" % (oid, uid))
        j, d, comma = self.item_span(i)
        res = [dict(it, id="%s.%s" % (uid, it["id"])) for it in items]
        if d.get("resolved_by"):
            if not rebind:
                return "kept"
            if j != i:
                raise ValueError("%s: residual unknown %s spans lines %d-%d: --rebind-resolves rewrites one-line unknowns "
                                 "only" % (oid, uid, i + 1, j + 1))
            old = len(d["resolved_by"])
            d["resolved_by"] = res
            self.lines[i] = self.IND_ITEM + dumps_item(d) + ("," if comma else "")
            self.check()
            return "rebound (%d -> %d resolving items)" % (old, len(res))
        if self.lines[i] == self.IND_ITEM + "{":
            # Pretty-printed: one key per line, closed by '        }'. resolved_by goes in as the last key, its items one
            # per line, in the same style; any other shape is refused, never rewritten.
            if self.lines[j].rstrip().rstrip(",") != self.IND_ITEM + "}":
                raise ValueError("%s: residual unknown %s spans lines %d-%d with an unsupported close %r"
                                 % (oid, uid, i + 1, j + 1, self.lines[j]))
            key = self.IND_ITEM + "  "
            if not self.lines[j - 1].rstrip().endswith(","):
                self.lines[j - 1] = self.lines[j - 1].rstrip() + ","
            self.lines[j:j] = ([key + '"resolved_by": ['] +
                               [key + "  " + dumps_item(x) + ("," if k < len(res) - 1 else "") for k, x in enumerate(res)] +
                               [key + "]"])
        elif j == i:
            d["resolved_by"] = res
            self.lines[i] = self.IND_ITEM + dumps_item(d) + ("," if comma else "")
        else:
            # A multi-line unknown (its witness list one item per line, closed by '        ]}'; tcmp1 2026-10-14): the
            # resolved_by list goes after it in the same style. Any other multi-line shape is refused, never rewritten.
            if self.lines[j].rstrip().rstrip(",") != self.IND_ITEM + "]}":
                raise ValueError("%s: residual unknown %s spans lines %d-%d with an unsupported close %r"
                                 % (oid, uid, i + 1, j + 1, self.lines[j]))
            prev = self.lines[j - 1]
            ind = prev[:len(prev) - len(prev.lstrip())] if j - 1 > i else self.IND_CKEY  # the witness lines' own indent
            self.lines[j:j + 1] = ([self.IND_ITEM + '], "resolved_by": ['] +
                                   [ind + dumps_item(x) + ("," if k < len(res) - 1 else "")
                                    for k, x in enumerate(res)] +
                                   [self.IND_ITEM + "]}" + ("," if comma else "")])
        self.check()
        return "resolved"

    def item_span(self, i):
        """(j, item, comma): the item opening at line i, one line or several (a nested list one entry per line), ends
        at line j - the first line at which the text from i parses as one JSON object."""
        buf = []
        for j in range(i, len(self.lines)):
            buf.append(self.lines[j])
            raw = "\n".join(buf).rstrip()
            try:
                d = json.loads(raw.rstrip(","))
            except ValueError:
                continue
            if isinstance(d, dict):
                return j, d, raw.endswith(",")
        raise ValueError("unterminated item at line %d" % (i + 1))


def ledger_tool_repo():
    return LANE if os.path.exists(os.path.join(LANE, "tools", "native_ledger.py")) else DEV


def ledger_module(repo=None):
    repo = repo or ledger_tool_repo()
    if os.path.join(repo, "tools") not in sys.path:
        sys.path.insert(0, os.path.join(repo, "tools"))
    import native_ledger as nl  # noqa: E402
    return nl


def ledger_check(item, doc=None, repo=None):
    """(result, detail) of ONE evidence item by tools/native_ledger.py's own check_item ('pass' / 'fail' /
    'unavailable') - the runner grades a pooled meter, and decides a retirement, by the ledger's rule, never its own."""
    repo = repo or ledger_tool_repo()
    nl = ledger_module(repo)
    return nl.check_item(nl.Ctx(doc or {"roots": {"repo": ".", "umbrella": UMBRELLA}}, repo), item)


def roll_files(d):
    """A roll's file hashes as the ledger's receipt check computes them (tools/native_ledger.py roll_files: one rule)."""
    return ledger_module().roll_files(d)


def ledger_grader(ledger_path, repo=None):
    """item -> 'pass' / 'fail' / 'unavailable' by the ledger tool against this ledger's roots, or None for every item
    when the tool cannot be loaded (fail closed: an ungraded item never retires anything)."""
    try:
        with open(ledger_path, encoding="utf-8") as f:
            doc = json.load(f)
        ledger_check({"id": "probe", "kind": "file", "root": "repo", "path": "docs", "authority": "static",
                      "observes": "build"}, doc, repo)
    except Exception:  # noqa: BLE001 - no grader: nothing is retired
        return lambda item: None
    return lambda item: ledger_check(item, doc, repo)[0]


def ledger_results(tool_repo):
    """{(oid, eid): result} from tools/native_ledger.py --json, or {} when the tool is absent / fails."""
    tool = os.path.join(tool_repo, "tools", "native_ledger.py")
    if not os.path.exists(tool):
        return {}
    with tempfile.TemporaryDirectory() as td:
        js = os.path.join(td, "ledger.json")
        subprocess.run([sys.executable, tool, "--json", js], cwd=tool_repo, capture_output=True, text=True)
        try:
            d = json.load(open(js, encoding="utf-8"))
        except (OSError, ValueError):
            return {}
    return results_index(d)


def results_index(d):
    """{(oid, eid): result} from the ledger tool's JSON document: `results` is a LIST of obligations (each with
    its id and evidence rows), as tools/native_ledger.py --json writes it; a dict keyed by id is accepted too."""
    res = d.get("results") if isinstance(d, dict) else None
    rows = res.values() if isinstance(res, dict) else (res or [])
    out = {}
    for o in rows:
        if not isinstance(o, dict):
            continue
        oid = o.get("id")
        for e in o.get("evidence") or []:
            if isinstance(e, dict):
                out[(oid, e.get("id"))] = e.get("result")
    return out


def ledger_nodata(ledger_path, repo=None):
    """item -> True when the ledger tool grades it NO DATA (an item with a `require` whose roll did not meet it); False
    otherwise, and for every item when the tool cannot be loaded."""
    try:
        with open(ledger_path, encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError):
        return lambda item: False

    def nodata(item):
        if not item.get("require"):
            return False
        try:
            res, det = ledger_check(item, doc, repo)
        except Exception:  # noqa: BLE001 - ungradeable: not NO DATA
            return False
        return res == "fail" and str(det).startswith("NO DATA")
    return nodata


def apply_ledger(ledger_path, runner, results_now=None, grade=None, nodata_of=None, rebind=False):
    """Upsert the plan's items; -> [(oid, eid, action)]. results_now: {(oid, eid): 'pass'|'fail'|...} of the ledger
    BEFORE the edit (a failing superseded item moves to a witness; a passing or unknown one is replaced). grade: item ->
    'pass' / 'fail' / 'unavailable' by the ledger tool (ledger_grader), or None.
    The plan's routes (native/devcreate-close, 2026-10-14):
    - retire_on_pass [{item, old, into, text}]: when the ledger's own grade of the applied `item` is 'pass', the evidence
      item `old` moves into contradiction `into` (witness, expect fail; created with `text` when absent) whose resolver
      is the applied item; a superseded failing `item` joins the same contradiction (supersede_into). Without a pass
      nothing moves: `old` stays in the evidence, failing, beside the failing new item.
    - resolver_bind {item id: (unknown id, resolver id)}: the item is not evidence; it binds the unknown's resolver on
      every outcome. A resolver already bound to an earlier run (its note not OWED) moves into the unknown's witness list
      first (expect fail unless it passes) - never dropped.
    THE SUPERSEDED ITEM (Astra round 1 on native/devcreate-close, P1-3: with results_now={} - a custom --ledger - a
    second failing pool REPLACED the first): an existing evidence item of the same id is graded before it is replaced -
    its status is results_now's, else the ledger tool's own grade of the existing line (grade) - and it is replaced in
    place only when it positively passes or names the same path (the same run re-applied). A failing, unavailable or
    ungradeable one (no grader: fail closed) moves into a witness list, expect fail - never dropped."""
    lt = LedgerText(open(ledger_path, encoding="utf-8").read())
    results_now = results_now or {}
    grade = grade or (lambda item: None)
    done = []
    items = runner.items()
    binds = runner.plan.get("resolver_bind", {})
    # NO DATA (p2.zpd zpd6: a step whose precondition was unmet twice, Runner.nodata_ids): its items are recorded as
    # evidence - grading NO DATA through their `require` - but never retire, bind or resolve anything below
    nodata = runner.nodata_ids() if hasattr(runner, "nodata_ids") else set()

    def superseded(oid, it):
        """True when the existing evidence item of it's id must be KEPT (as a witness) rather than replaced."""
        old = lt.evidence_item(oid, it["id"])
        if old is None or old.get("path") == it.get("path"):
            return False
        status = results_now.get((oid, it["id"]))
        if status is None:
            status = grade(old)
        return status != "pass"
    for rule in runner.plan.get("retire_on_pass", []):
        for oid, it in items:
            if it["id"] != rule["item"]:
                continue
            passed = grade(it) == "pass" and it["id"] not in nodata
            if passed or superseded(oid, it):
                done.append((oid, rule["into"], "contradiction " + lt.ensure_contradiction(
                    oid, rule["into"], rule["text"], it)))
            if passed:
                done.append((oid, rule["old"], lt.retire_evidence(
                    oid, rule["old"], rule["into"], "retired %s: the pooled re-measure %s passes (%s)" % (
                        datetime.date.today().isoformat(), it["path"], rule["item"]))))
    for oid, it in items:
        if it["id"] in binds:
            continue
        fails = {it["id"]} if superseded(oid, it) else set()
        prev = lt.evidence_item(oid, it["id"]) if fails else None
        action, _old = lt.upsert_evidence(oid, it, superseded_fails=fails,
                                          into=runner.plan.get("supersede_into", {}).get(it["id"]),
                                          void_expect="no_data" if prev and nodata_of and nodata_of(prev) else "fail")
        done.append((oid, it["id"], action))
    for oid, it in items:  # a contradiction resolver bound to this item takes its run-unique path (resolver_paths)
        rid = runner.plan.get("resolver_paths", {}).get(it["id"])
        if rid and it["id"] in nodata:
            done.append((oid, rid, "resolver NOT bound: %s is NO DATA" % it["id"]))
        elif rid:
            done.append((oid, rid, lt.bind_resolver(oid, rid, it)))
    for oid, it in items:  # an unknown's resolver bound on every outcome; the run it was bound to before is kept
        if it["id"] not in binds:
            continue
        if it["id"] in nodata:
            done.append((oid, binds[it["id"]][1], "resolver NOT bound: %s is NO DATA" % it["id"]))
            continue
        uid, rid = binds[it["id"]]
        cur = lt.line_item(oid, rid)
        if cur is None:
            raise KeyError("%s: resolver %s not found (pre-register it before applying)" % (oid, rid))
        if not str(cur.get("note", "")).startswith("OWED") and cur.get("path") != it["path"]:
            w = dict(cur, id=lt.unique_id(oid, rid + "_void"))
            if grade(cur) != "pass":
                w["expect"] = "fail"
            w["note"] = (cur.get("note", "") + "; " if cur.get("note") else "") + "superseded by %s" % it["path"]
            done.append((oid, w["id"], lt.unknown_witness(oid, uid, w)))
        done.append((oid, rid, lt.bind_resolver(oid, rid, it)))
    for oid, uid, eids in runner.plan.get("resolves", []):
        res = [it for o, it in items if o == oid and it["id"] in eids]
        # a resolver routed to another of the plan's obligations (Runner.item_oids: native/boot-gaps' mode_state_bake
        # takes the default path's census1 / era1 items, which are p2.transport's and acceptance.zero's): each id once,
        # its first routing. Plans whose resolvers are all their unknown's own obligation's read as before.
        have = {it["id"] for it in res}
        for _o, it in items:
            if it["id"] in eids and it["id"] not in have:
                res.append(it)
                have.add(it["id"])
        # Every resolving item or none (tcmp1 2026-10-14: an aborted run carried 5 of composed_zero's 7): the ledger
        # attaches a resolver only once, so a partial set would close the unknown without its missing items, or keep a
        # later complete run from ever attaching them.
        missing = sorted(set(eids) - {it["id"] for it in res})
        blank = sorted({it["id"] for it in res} & nodata)
        if blank:  # NO DATA never counts toward an unknown (zpd6): a later run with data attaches the set
            done.append((oid, uid, "unknown NOT resolved: NO DATA for " + ", ".join(blank)))
            continue
        if missing:
            if res:
                done.append((oid, uid, "unknown NOT resolved: the run lacks " + ", ".join(missing)))
            continue
        done.append((oid, uid, "unknown " + lt.resolve_unknown(oid, uid, res, rebind=rebind)))
    lt.check()
    with open(ledger_path, "w", encoding="utf-8", newline="") as f:
        f.write(lt.text())
    return done


# ------------------------------------------------------------------------------------------ main

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--plan")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--exe")
    ap.add_argument("--base", help="the base exe for the replay comparison (9 Xenia traces)")
    ap.add_argument("--out", default=os.path.join(LANE, "archive", "v1_native"))
    ap.add_argument("--repo", default=DEV, help="the checkout the rolls run from (the disc beside it)")
    ap.add_argument("--owner", default="native-a")
    ap.add_argument("--instance", default="native")
    ap.add_argument("--only", default="")
    ap.add_argument("--skip", default="")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--slot-held", action="store_true", help="the caller holds the game slot under --owner")
    ap.add_argument("--xenia-dump", help="p2.zpd's Xenia oracle: xenia-gpu-d3d12-trace-dump.exe (default %s)" % XENIA_DUMP)
    ap.add_argument("--xenia-adapter", type=int, help="its --d3d12_adapter (-2 = WARP, no GPU needed)")
    ap.add_argument("--apply-ledger", action="store_true")
    ap.add_argument("--ledger", default=os.path.join(LANE, "docs", "native_ledger.json"))
    ap.add_argument("--recompare", action="store_true",
                    help="no rolls: re-run the plan's comparison steps (owner / census / tcensus / pacing / zpdwin) and its "
                         "meter over the rolls already in --out, keeping every other stored row (a comparison that "
                         "predates a rule change)")
    ap.add_argument("--rebind-resolves", action="store_true",
                    help="with --apply-ledger: an unknown this plan resolves that is ALREADY resolved has its resolved_by "
                         "replaced by this run's items, whatever they grade - a pre-registered regrade's stricter result "
                         "stands (a stricter FAIL re-opens it); without it an existing resolution is kept")
    ap.add_argument("--reapply", action="store_true",
                    help="no rolls: apply the ledger from the plan's stored <prefix>_proof.json (a crash after the rolls)")
    ap.add_argument("--stage", default="", help="every roll reads the game from this SSD folder (e.g. J:\\sr_stage; "
                    "perf_run --stage, tools/perf/stage_roll.py); logs, frames, perf and saves stay under --out")
    ap.add_argument("--graceful-quit", action="store_true",
                    help="tools/graceful-quit (docs/TOOLKIT.md 1.8m): every roll ends by the graceful quit (its harnesses "
                    "inherit SR_DRIVE_GRACEFUL_QUIT=1), so each census covers the whole run; the owner / census steps "
                    "grade the terminal reports (--require-terminal) and each roll (and gate roll) gets the ADDED items "
                    "<prefix>.<tag>.terminal (a complete terminal snapshot), <prefix>.<tag>.quit_receipt (quit.json: the "
                    "game's own clean exit 0) and, for EVERY text item, <item id>.terminal (the same predicate on the "
                    "snapshot's line of that family). The pre-registered items are unchanged")
    ap.add_argument("--stage-source", default="", help="--stage: the folder holding the disc to stage from (then "
                    "--repo needs no disc beside it)")
    a = ap.parse_args(argv)
    if a.stage_source and not a.stage:
        ap.error("--stage-source needs --stage")
    if a.graceful_quit:  # tools/graceful-quit: perf_run / flicker_gate -> sr_drive read it (sr_drive -GracefulQuit)
        os.environ["SR_DRIVE_GRACEFUL_QUIT"] = "1"
        print("NOTE: --graceful-quit: every roll ends by the graceful quit (SR_DRIVE_GRACEFUL_QUIT=1), whole-run census")
    ps = plans()
    if a.list or not a.plan:
        for k, p in ps.items():
            print("%-12s %-22s %s" % (k, ",".join(p["obligations"]), p["about"]))
            if p.get("exe_pin"):
                print("    pinned to exe %s (its meter base is an archived roll)" % p["exe_pin"])
            for s in p["steps"]:
                print("    %-8s %-10s %-7s %s" % (s.kind, s.tag, s.mode if s.kind == "roll" else "", s.gates))
        return 0
    if a.plan not in ps:
        ap.error("unknown plan %s (--list)" % a.plan)
    refused = pin_refusal(ps[a.plan], a.plan, a.exe)
    if refused and not (a.reapply or a.recompare):  # --reapply / --recompare roll nothing
        if not a.dry_run:
            raise SystemExit("REFUSED: " + refused)
        print("NOTE: %s (a live run is refused)" % refused)
    if not a.dry_run and not a.reapply and not a.recompare:
        if not a.exe or not os.path.exists(a.exe):
            ap.error("--exe must exist")
        if a.stage_source:
            if not os.path.isdir(os.path.join(a.stage_source, "Superman Returns (USA)")):
                ap.error("--stage-source %s has no 'Superman Returns (USA)'" % a.stage_source)
        elif not os.path.isdir(os.path.join(a.repo, "Superman Returns (USA)")):
            ap.error("--repo %s has no 'Superman Returns (USA)' beside it (the rolls need the disc)" % a.repo)
    lock = None if a.dry_run else OutLock(a.out).acquire()  # one native_proof per --out, held to the end
    try:
        return run_and_apply(a, Runner(a, ps[a.plan]))
    finally:
        if lock:
            lock.release()


def run_and_apply(a, r):
    try:
        rc = r.recompare() if a.recompare else r.reload() if a.reapply else r.run()
    except ProofWriteError as e:
        proof = r.path("proof", ".json")
        try:  # best effort, beside the proof: a later --reapply then refuses the older stored proof
            with open(proof + UNWRITABLE, "w", encoding="utf-8") as f:
                f.write("%s %s\n" % (datetime.datetime.now().isoformat(timespec="seconds"), e))
            marker = "marked %s" % (proof + UNWRITABLE)
        except OSError as e2:
            marker = "the marker could not be written either (%s)" % e2
        print("FAILED: the proof JSON could not be written (%s) - the run failed; --apply-ledger refused, nothing "
              "applied; %s" % (e, marker), flush=True)
        return 3
    if a.dry_run or a.apply_ledger:
        print("ledger items (%s):" % ("would apply" if a.dry_run else "applying"))
        binds = r.plan.get("resolver_bind", {})
        for oid, it in r.items():
            route = ("[resolver %s / %s, bound on every outcome] " % binds[it["id"]]) if it["id"] in binds else ""
            print("  %s: %s%s" % (oid, route, dumps_item(it)))
        for rule in r.plan.get("retire_on_pass", []):
            print("  on the ledger's PASS of %s: evidence %s -> contradiction %s (witness, expect fail; resolved_by %s); "
                  "on its FAIL both stay in the evidence" % (rule["item"], rule["old"], rule["into"], rule["item"]))
    if a.apply_ledger and not a.dry_run:
        tool_repo = ledger_tool_repo()
        done = apply_ledger(a.ledger, r, ledger_results(tool_repo) if os.path.abspath(a.ledger) == os.path.join(tool_repo, "docs", "native_ledger.json") else {},
                            grade=ledger_grader(a.ledger, tool_repo), nodata_of=ledger_nodata(a.ledger, tool_repo),
                            rebind=a.rebind_resolves)
        for oid, eid, action in done:
            print("  ledger %s/%s: %s" % (oid, eid, action))
        for oid in r.plan["obligations"]:
            subprocess.call([sys.executable, os.path.join(tool_repo, "tools", "native_ledger.py"), "-v", "--id", oid], cwd=tool_repo)
        print("next: python tools/native_ledger.py --write-status docs/NATIVE_ROADMAP.md && python tools/native_ledger.py --strict")
    return rc


if __name__ == "__main__":
    sys.exit(main())
