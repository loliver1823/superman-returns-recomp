#!/usr/bin/env python3
"""Tests of tools/perf/mem_res_ab_check.py (the mem.resources frozen-pass A/B witness, schema mem-res-ab/2). Each mutant
roll is one a broken toggle or a broken roll would produce; each must fail (or be NO DATA) with its reason, the healthy
roll must pass. Astra review1 P2-4's counterexamples are T13, T15 and T17.
    python tools/perf/test_mem_res_ab_check.py
"""
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mem_res_ab_check as M  # noqa: E402

TS = "[2026-10-17 05:00:00.000] [INFO ] [BOOT] "


def totals(why, drop=None, **kw):
    v = dict(sn=40, sm=40, se=1, pn=40, pm=40, rt=80, dn=900, dm=1200, nim=0, min_=0, ul=0, dul=0, un=12, um=30, fg=3,
             ad=9, cw=0, cc=0, cu=2, pr=4, re=1, mr=0, gn=5, dpn=20, dpm=20, ap=38, ab=0)
    v.update(kw)
    line = ("[MEM_RES_AB] %s sets native=%d model=%d empty=%d presented native=%d model=%d rt_enter=%d | decisions "
            "native=%d model=%d native_in_model=%d model_in_native=%d unlatched=%d dec_unlatched=%d | uploads native=%d "
            "model=%d | inv_m forgot=%d adopted=%d | inv_r cross_window=%d cross_clobber=%d cross_unannounced=%d "
            "preserve=%d reestablished=%d | model_readback=%d gpu_note_model_arm=%d | dumped native=%d model=%d "
            "alt_pairs=%d alt_break=%d" % (
                why, v["sn"], v["sm"], v["se"], v["pn"], v["pm"], v["rt"], v["dn"], v["dm"], v["nim"], v["min_"],
                v["ul"], v["dul"], v["un"], v["um"], v["fg"], v["ad"], v["cw"], v["cc"], v["cu"], v["pr"], v["re"],
                v["mr"], v["gn"], v["dpn"], v["dpm"], v["ap"], v["ab"]))
    if drop:
        line = line.replace(drop, "")
    return TS + line + "\n"


def roll(arms=None, dual=True, present_mode=1, terminal=True, drop_arm=None, step=1, dup_dump=None, conflict=None,
         periodic=None, drop=None, **kw):
    """(log text, freeze table text): two holds of 20 dumped presents, each consecutive pair compared static."""
    out, table = [], []
    if dual:
        out.append(TS + "[MEM_RES_AB] dual=1 bit=0 present_mode=%d: ...\n" % present_mode)
    out.append(totals("periodic", drop=drop, **(periodic if periodic is not None else
                                                {k: max(0, v // 2) for k, v in kw.items()})))
    p = 1000
    for f in range(2):
        pres = []
        for k in range(20):
            arm = "NM"[p % 2] if arms is None else arms[k % len(arms)]
            out.append(TS + "[SIM_FREEZE_DUMP] freeze=%d k=%d present=%d held_ms=900 shown_stamp=1.0 s zpd=-\n" % (f, k, p))
            if p == dup_dump:
                out.append(TS + "[SIM_FREEZE_DUMP] freeze=%d k=%d present=%d held_ms=900 shown_stamp=1.0 s zpd=-\n" % (
                    f, k, p))
            if p != drop_arm:
                out.append(TS + "[MEM_RES_AB] present=%d arm=%s\n" % (p, arm))
            if p == conflict:
                out.append(TS + "[MEM_RES_AB] present=%d arm=%s\n" % (p, "M" if arm == "N" else "N"))
            out.append(TS + "[AB_PRESENT] swap=%d ab=0x%d\n" % (p, p % 2))
            pres.append(p)
            p += step
        table.append("%d | 20 | 19 | 0 | - | 0 | 0 | -" % f)
        table += ["compared | %d | %d | %d | static" % (f, a, b) for a, b in zip(pres, pres[1:])]
        p += 50
    if terminal:
        out.append(totals("terminal", drop=drop, **kw))
    return "".join(out), "\n".join(table) + "\n"


def run(rt, table=True, **kw):
    text, tab = rt
    fd, path = tempfile.mkstemp(suffix=".log")
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        fh.write(text)
    try:
        return M.grade(M.parse(path), tab if table else None, **kw)
    finally:
        os.remove(path)


def expect(name, res, verdict, needle=None):
    ok = res["verdict"] == verdict and (needle is None or any(needle in p for p in res["problems"]))
    print("%s %s -> %s %s" % ("PASS" if ok else "FAIL", name, res["verdict"], res["problems"][:2]))
    return ok


def cli_case():
    """The --roll layout: <roll>/drive/attempt1/run.log and <roll>_freeze.txt; --json written; exit 0."""
    tmp = tempfile.mkdtemp(prefix="mra_")
    try:
        r = os.path.join(tmp, "abp")
        os.makedirs(os.path.join(r, "drive", "attempt1"))
        text, tab = roll()
        with open(os.path.join(r, "drive", "attempt1", "run.log"), "w", encoding="utf-8") as fh:
            fh.write(text)
        with open(r + "_freeze.txt", "w", encoding="utf-8") as fh:
            fh.write(tab)
        js = os.path.join(tmp, "w.json")
        rc = M.main(["--roll", r, "--json", js, "--min-pairs", "30"])
        d = json.load(open(js, encoding="utf-8"))
        ok = rc == 0 and d["verdict"] == "PASS" and d["schema"] == M.SCHEMA and d["static_pairs"] == 38 and \
            all(d["checks"].values())
        rc2 = M.main(["--roll", r, "--json", js])  # the default minimum (100 static pairs): 38 fail
        ok = ok and rc2 == 1
        os.remove(r + "_freeze.txt")
        rc3 = M.main(["--roll", r, "--json", js, "--min-pairs", "30"])
        ok = ok and rc3 == 2
        print("%s T21 the --roll layout: PASS exit 0 / too few pairs exit 1 / no table exit 2 (%d %d %d)" % (
            "PASS" if ok else "FAIL", rc, rc2, rc3))
        return ok
    finally:
        shutil.rmtree(tmp)


def main():
    mp = dict(min_pairs=30)
    # T19: the pixel grade skipped the middle pair (a shape change: no compared row) - each listed pair alternates (N M,
    # M N) but both static pairs' controls (the present before a -> b; a hold's first pair: a -> the present after b)
    # span two deciders
    t19 = (roll(arms="NMMN")[0], "compared | 0 | 1000 | 1001 | static\ncompared | 0 | 1002 | 1003 | static\n")
    results = [
        expect("T1 healthy alternating roll", run(roll(), **mp), "PASS"),
        expect("T2 no-op toggle (native decisions in model sets)", run(roll(nim=500), **mp), "FAIL", "wrong-arm"),
        expect("T3 always-model toggle", run(roll(min_=400), **mp), "FAIL", "wrong-arm"),
        expect("T4 a model arm that never decided", run(roll(dm=0), **mp), "FAIL", "decided too little"),
        expect("T5 consecutive dumped presents with one arm", run(roll(arms="NNMM"), **mp), "FAIL", "ONE decider"),
        expect("T6 a compared present without its arm line", run(roll(drop_arm=1003), **mp), "NO DATA",
               "without a [SIM_FREEZE_DUMP] or an arm line"),
        expect("T7 no dual line", run(roll(dual=False), **mp), "NO DATA", "never latched"),
        expect("T8 pace-window mode", run(roll(present_mode=0), **mp), "NO DATA", "present_mode=0"),
        expect("T9 cross_clobber", run(roll(cc=1), **mp), "FAIL", "cross_clobber"),
        expect("T10 unlatched decisions", run(roll(ul=2, dul=8), **mp), "FAIL", "unlatched"),
        expect("T11 too few static pairs", run(roll(), min_pairs=100), "FAIL", "only 38 static"),
        expect("T12 render thread alt_break", run(roll(ab=1), **mp), "FAIL", "alt_break"),
        expect("T13 no terminal totals: NO DATA (Astra P2-4)", run(roll(terminal=False), **mp), "NO DATA",
               "no terminal"),
        expect("T13b no terminal, a periodic FAIL is final", run(roll(terminal=False, periodic={"nim": 3}), **mp),
               "FAIL", "wrong-arm"),
        expect("T14 counters backwards", run((roll()[0] + totals("periodic", dn=1), roll()[1]), **mp), "NO DATA",
               "backwards"),
        expect("T15 a totals line missing a field (Astra P2-4)", run(roll(drop=" cross_clobber=0"), **mp), "NO DATA",
               "lacks inv_r.cross_clobber"),
        expect("T16 a present dumped twice", run(roll(dup_dump=1004), **mp), "NO DATA", "dumped twice"),
        expect("T17 same-arm dumps at nonadjacent presents (Astra P2-4)", run(roll(arms="N", step=2), **mp), "FAIL",
               "ONE decider"),
        expect("T18 no freeze table", run(roll(), table=False, **mp), "NO DATA", "no freeze table"),
        expect("T19 a static pair's control spans two deciders", run(t19, min_pairs=1), "FAIL", "control(s) from two"),
        expect("T20 two arm lines for one present", run(roll(conflict=1007), **mp), "FAIL", "two arm lines"),
        expect("T22 an empty freeze table", run((roll()[0], "freeze | frames\n"), **mp), "NO DATA", "no compared pair"),
    ]
    results.append(cli_case())
    n_fail = results.count(False)
    print("test_mem_res_ab_check: %d checks, %d failed" % (len(results), n_fail))
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
