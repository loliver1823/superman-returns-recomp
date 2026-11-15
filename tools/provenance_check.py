#!/usr/bin/env python3
"""provenance_check.py - is any of a foreign source file's code inside our tracked sources?

Copy detection for licence questions ("this file credits X; is X's code in it?"), with a
positive control so the check is shown to fail on a real copy before its PASS means anything.

  python tools/provenance_check.py --foreign F [--foreign F2 ...] --control C [--control C2 ...]
         [--target GLOB ...] [--all-tracked] [--k 20] [--max-run 40] [--max-share 0.10]
         [--json OUT]

Comments are stripped before tokenizing; a string literal is one token. Per foreign x target:
  run    the longest run of tokens the two share after every identifier, number and string is
         replaced by a placeholder and every name chain (a->b.c, ns::x) counts as one name (a
         renamed copy, ctx->field -> field_, still matches; an identical copy matches too)
  share  the fraction of the foreign file's k-token windows (same normalisation) found in the target
  raw    the longest run of IDENTICAL tokens (names kept), for reading
  consts the foreign file's numeric literals above 64 that the target also contains (reported,
         not judged: format constants such as a 2048-byte packet size are facts, not expression)
PASS = every target stays under --max-run and --max-share for every foreign file, AND every
control is over both (otherwise the thresholds are blind and the run FAILS).
Calibration (iter-467, xma2dec.c): the real 2015 adaptation scores run 76 / share 0.205; the worst
of all 451 tracked C/C++ files scores run 28 / share 0.015 (generic `x.f = v;` runs, imgui). Short
windows are size-biased: at k=12 imgui.cpp alone reaches 0.19 on generic code, hence k=20.

Targets: --target globs over `git ls-files` (default: the XMA / audio path), or --all-tracked for
every tracked C/C++ file. The foreign files are NOT committed (an unlicensed file must not enter
the repo); fetch them into archive/ first. iter-467 (licence lane, xma): koolkdev/libertyv
libav_wrapper/xma2dec.c @629cfae5 against our XMA path, control = the 2015 Xenia decoder that
did adapt it (xenia f31b34256b src/xenia/apu/audio_decoder.cpp). Evidence: archive/v1_xma/.
"""
import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
CPP_EXT = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl")
DEFAULT_TARGETS = ("kernel/apu/*", "kernel/ffmpeg_xenia.*", "kernel/audio_*", "kernel/mm_phys.*",
                   "tools/xma_offline/*", "tools/ffmpeg_xenia/*", "tools/difftest/xma_test.cpp",
                   "tools/difftest/audio_out_test.cpp")

TOKEN = re.compile(r"""
    (?P<str>"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')
  | (?P<num>0[xX][0-9a-fA-F]+[uUlL]*|\d+\.\d*(?:[eE][-+]?\d+)?[fF]?|\.\d+(?:[eE][-+]?\d+)?[fF]?|\d+[uUlLfF]*)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<op>->|\+\+|--|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|::|\S)
""", re.X)
COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/|("(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\')', re.S)


def tokens(path):
    src = open(path, encoding="latin-1").read()
    src = COMMENT.sub(lambda m: m.group(1) or " ", src)
    raw, norm, pos = [], [], []   # pos[i] = index in raw of norm[i]'s first token
    for m in TOKEN.finditer(src):
        kind = m.lastgroup
        tok = {"str": "S", "num": "N", "id": "I"}.get(kind, m.group(kind))
        raw.append(m.group(kind))
        # A name chain (a->b.c, ns::x) is one name: an adaptation that turns ctx->field into
        # field_ or min into std::min still lines up.
        if tok == "I" and len(norm) >= 2 and norm[-1] in ("->", ".", "::") and norm[-2] == "I":
            norm.pop()
            pos.pop()
            continue
        norm.append(tok)
        pos.append(len(raw) - 1)
    pos.append(len(raw))
    return raw, norm, pos


def numbers(raw):
    out = set()
    for t in raw:
        try:
            v = int(t.rstrip("uUlL"), 0)
        except ValueError:
            continue
        if v > 64:
            out.add(v)
    return out


def longest_run(a, b):
    """Longest common contiguous run of a and b; returns (length, start index in b)."""
    def grams(seq, n):
        return {tuple(seq[i:i + n]): i for i in range(len(seq) - n + 1)}
    lo, hi, best = 0, min(len(a), len(b)), (0, 0)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        ga = set(tuple(a[i:i + mid]) for i in range(len(a) - mid + 1))
        hit = next((i for i in range(len(b) - mid + 1) if tuple(b[i:i + mid]) in ga), None)
        if hit is None:
            hi = mid - 1
        else:
            lo, best = mid, (mid, hit)
    return best


def share(a, b, k):
    if len(a) < k or len(b) < k:
        return 0.0
    gb = set(tuple(b[i:i + k]) for i in range(len(b) - k + 1))
    wins = [tuple(a[i:i + k]) for i in range(len(a) - k + 1)]
    return sum(w in gb for w in wins) / len(wins)


def compare(foreign, target, k):
    fr, fn, _ = foreign
    tr, tn, tpos = target
    run, at = longest_run(fn, tn)
    rraw, rat = longest_run(fr, tr)
    return {
        "run": run, "run_text": " ".join(tr[tpos[at]:tpos[at + run]])[:400] if run else "",
        "share": round(share(fn, tn, k), 4),
        "raw": rraw, "raw_text": " ".join(tr[rat:rat + rraw])[:400],
        "consts": sorted(hex(v) for v in numbers(fr) & numbers(tr)),
    }


def tracked(globs, all_cpp):
    out = subprocess.run(["git", "-C", ROOT, "ls-files"], capture_output=True, text=True,
                         check=True).stdout.split()
    files = [f for f in out if f.endswith(CPP_EXT)]
    if all_cpp:
        return files
    return [f for f in files if any(fnmatch.fnmatch(f, g) for g in globs)]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--foreign", action="append", required=True)
    ap.add_argument("--control", action="append", required=True)
    ap.add_argument("--target", action="append")
    ap.add_argument("--all-tracked", action="store_true")
    ap.add_argument("--k", type=int, default=20)
    ap.add_argument("--max-run", type=int, default=40)
    ap.add_argument("--max-share", type=float, default=0.10)
    ap.add_argument("--json")
    a = ap.parse_args(argv)

    foreign = {f: tokens(f) for f in a.foreign}
    targets = tracked(a.target or DEFAULT_TARGETS, a.all_tracked)
    if not targets:
        print("provenance_check: no targets matched")
        return 2
    res = {"foreign": a.foreign, "k": a.k, "max_run": a.max_run, "max_share": a.max_share,
           "controls": {}, "targets": {}}
    blind = []
    for c in a.control:
        ct = tokens(c)
        res["controls"][c] = {f: compare(ft, ct, a.k) for f, ft in foreign.items()}
        best = max(res["controls"][c].values(), key=lambda r: r["run"])
        if best["run"] <= a.max_run or max(r["share"] for r in res["controls"][c].values()) <= a.max_share:
            blind.append(c)
    hits = []
    for t in targets:
        tt = tokens(os.path.join(ROOT, t))
        res["targets"][t] = {f: compare(ft, tt, a.k) for f, ft in foreign.items()}
        for f, r in res["targets"][t].items():
            if r["run"] > a.max_run or r["share"] > a.max_share:
                hits.append((t, f))
    worst = max(((r["run"], r["share"], t, f) for t, rs in res["targets"].items() for f, r in rs.items()),
                default=(0, 0, "", ""))
    res["verdict"] = "FAIL-BLIND-CONTROL" if blind else ("FAIL" if hits else "PASS")
    res["blind_controls"], res["hits"] = blind, hits

    for c, rs in res["controls"].items():
        for f, r in rs.items():
            print("control %-48s vs %-26s run %4d share %.3f raw %4d consts %s"
                  % (os.path.basename(c), os.path.basename(f), r["run"], r["share"], r["raw"], r["consts"]))
    print("targets: %d files; worst run %d (%s), worst share %.3f"
          % (len(targets), worst[0], worst[2],
             max((r["share"] for rs in res["targets"].values() for r in rs.values()), default=0)))
    for t, f in hits:
        r = res["targets"][t][f]
        print("HIT %s vs %s: run %d share %.3f :: %s" % (t, os.path.basename(f), r["run"], r["share"], r["run_text"][:160]))
    print("provenance_check: %s (thresholds run > %d or share > %.2f; controls %s)"
          % (res["verdict"], a.max_run, a.max_share, "blind: " + ", ".join(blind) if blind else "detected"))
    if a.json:
        os.makedirs(os.path.dirname(os.path.abspath(a.json)), exist_ok=True)
        with open(a.json, "w") as fh:
            json.dump(res, fh, indent=1)
    return 0 if res["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
