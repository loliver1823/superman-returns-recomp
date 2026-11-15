#!/usr/bin/env python3
"""test_shm_meter_wiring.py - every path of the shared-memory page model that hashes, validates or accepts a page is
fed to the native meter's `shm` column or deliberately excluded (mem.resources step 1; Astra round 1 on
release/mem-meter asked for the sweep and for a test that fails before each feed).

    python tools/perf/test_shm_meter_wiring.py [--source gfx/d3d12_pm4_shmem.cpp] [--header kernel/native_meter.hpp]
                                                [--composition docs/RELEASE_COMPOSITION.md] [--roadmap docs/NATIVE_ROADMAP.md]

THE RULE (kernel/native_meter.hpp `shm`, one statement): a count per 4 KiB page per DECISION EVENT in which the model
decides whether its copy of the page is current - a validation before a fetch / scan-out (by hash, by the write watch's
clean state, by the range cache's proof) or a re-baseline after a GPU write (by hash, by the kept generation, by the
readback's bytes). Not counted: same-epoch dedup hits, the oracles' re-asking (SR_SHM_WT_CHECK, SR_NB_SHM_RANGE_CHECK,
the live-trace recorder), hashes that are not page decisions (fetch-constant keys, the zero page).

The sweep is mechanical so that a path added later cannot slip by unclassified: every `hashUcode(` call in
gfx/d3d12_pm4_shmem.cpp is attributed to its enclosing column-0 function definition; a function that hashes must be in
FED (and carry exactly the expected number of `meter::kShm` feeds, each at its anchored place in the control flow, so
that the feed covers the hashes it is meant to cover and no oracle block) or in EXCLUDED (with its reason, and no feed);
anything else fails. The acceptance paths that never hash (pm4ShmRequestT's dedup returns, the kept generation) are
checked by anchor: a dedup block carries no feed, an acceptance that is a decision does. The header and the two docs
must state the one rule (not the superseded "one count per page per frame including the checker's hashes"). Prints the
sweep table (the one the step's commit message carries); exit code = failed checks.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
FEED = "meter::add(sr::native::meter::kShm"
HASH = "hashUcode("
FAILS = 0


def check(ok, name):
    global FAILS
    print(("  ok  " if ok else "FAIL: ") + name)
    if not ok:
        FAILS += 1


# Functions whose hashes are page decisions: the number of feeds each must carry, and why.
FED = {
    "pm4ShmRequestT": (2, "validations decided without a hash: every page of a range the range cache proves clean "
                          "(SR_NB_SHM_RANGE_CACHE, across epochs), and a page the write watch holds clean (SR_SHM_WRITE_TRACK)"),
    "pm4ShmValidate": (1, "validations decided by a hash: one per candidate page (its 1-3 hashes, incl. the census re-hash, "
                          "are one decision)"),
    "pm4ShmRangeWrittenByGpu": (1, "re-baseline by hash under m_shmMu (SR_SHM_REBASE_UNLOCKED=0): one per page of the resolve"),
    "rebaseHashPages": (2, "re-baseline by hash outside the lock (the unlocked path and pm4ShmResolve's pre-hash): one per "
                           "page hashed, in the fresh-first walk and in the plain walk; a page kept fresh is decided at the commit"),
    "pm4ShmRangeWrittenByGpuUnlocked": (1, "re-baseline of a page the pre-hash found fresh, decided at the commit: kept under "
                                           "its generation or hashed after losing the protection - one decision either way"),
    "pm4ShmReadback": (1, "re-baseline of the bytes a readback wrote into guest memory: one per page"),
}
# Functions that hash and are deliberately NOT fed.
EXCLUDED = {
    "zeroPageHash": "the constant key of the all-zero page: not a page decision",
    "shmTracePage": "the live-trace recorder re-hashes decided pages for the trace's memRead (SR_PM4_TRACE_FULL): an oracle",
    "shmWtCheck": "SR_SHM_WT_CHECK re-asks a page already decided clean (the checker, default OFF): an oracle",
    "shmCollectTextures": "keys of 24-byte texture fetch constants: not pages",
    "pm4ShmLoadArray": "keys of array / cube fetch constants: not pages",
}
# Anchors of the acceptance paths that never hash: (text the line contains, fed?, what it is).
ACCEPTS = [
    ("memo->epoch == m_shmEpoch && m_shmEpoch) return true;", False, "range memo: the same drain asked the same range in this epoch (dedup)"),
    ("v.sum == rce->ownSum) {", True, "range cache hit: the range proven clean since its last validation, across epochs (a decision per page)"),
    ("for (uint32_t p = p0; p <= p1 && all; p++) all = __atomic_load_n(eo + p, __ATOMIC_RELAXED) == ep;", False,
     "SR_CP_SHM_LEAN: every page already carries this drain's epoch (dedup)"),
    ("if (!done && !(t_shmPrepass && m_shmResolveMask[p])) need = true;", False,
     "lazy-window pre-check: every page validated in this epoch / frame (dedup)"),
    ("if (livetrace::recording() && traceFullEnabled()) shmTracePage(p, rangeOwn);", False,
     "per-page epoch stamp: validated earlier in this epoch (dedup; the trace recorder re-reads it)"),
    ("if (t_shmPrepass && m_shmResolveMask[p]) continue;  // resolved this frame: at the bind", False,
     "texture prepass: a page a resolve wrote this frame is decided at the bind, not here"),
    ("m_shmFramePagesClean++;", True, "write watch: the page stayed protected and unwritten since its last hash (a decision)"),
    ("m_shmWtRebaseKept++;", True, "SR_SHM_WT_REBASE keep: the page's hashes were taken under the generation it still holds (a decision)"),
]


def functions(lines):
    """[(name, first, last)] of the column-0 function definitions (a body opened on the signature's line or a later one;
    one-line definitions close on their own line)."""
    out, cur = [], None
    skip = ("namespace", "static_assert", "struct ", "class ", "thread_local", "constexpr", "using ", "typedef ",
            "template", "return ", "#", "//", "}", "{", "static const", "static constexpr", "static thread_local")
    for i, ln in enumerate(lines):
        if not ln or ln[0] in " \t":
            continue
        if ln.startswith("}"):
            if cur:
                out.append((cur[0], cur[1], i))
                cur = None
            continue
        if cur is not None or ln.startswith(skip) or "(" not in ln or "=" in ln.split("(", 1)[0] or ln.rstrip().endswith(";"):
            continue  # (a declaration, an initializer, a statement: not a definition)
        m = re.match(r"^[A-Za-z_][\w:<>*&,\s]*?\b([A-Za-z_]\w*)\s*\(", ln)
        if not m:
            continue
        if ln.count("{") and ln.count("{") == ln.count("}"):
            out.append((m.group(1), i, i))  # a one-line definition
        else:
            cur = (m.group(1), i)
    return out


def enclosing(funcs, i):
    for name, a, b in funcs:
        if a <= i <= b:
            return name
    return None


def find(lines, text, start=0, end=None):
    end = len(lines) if end is None else end
    for i in range(start, end):
        if text in lines[i]:
            return i
    return -1


def lambda_span(lines, start, end):
    """(first, last) of the lambda whose `auto x = [` line is `start`, closed by the first `};` at its indentation."""
    ind = len(lines[start]) - len(lines[start].lstrip())
    for i in range(start + 1, end):
        if lines[i].strip() == "};" and len(lines[i]) - len(lines[i].lstrip()) == ind:
            return start, i
    return start, end


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default=os.path.join(REPO, "gfx", "d3d12_pm4_shmem.cpp"))
    ap.add_argument("--header", default=os.path.join(REPO, "kernel", "native_meter.hpp"))
    ap.add_argument("--composition", default=os.path.join(REPO, "docs", "RELEASE_COMPOSITION.md"))
    ap.add_argument("--roadmap", default=os.path.join(REPO, "docs", "NATIVE_ROADMAP.md"))
    a = ap.parse_args()
    lines = open(a.source, encoding="utf-8").read().split("\n")
    funcs = functions(lines)
    span = {n: (s, e) for n, s, e in funcs}
    hashes = [i for i, ln in enumerate(lines) if HASH in ln]
    feeds = [i for i, ln in enumerate(lines) if FEED in ln]
    check('#include "../kernel/native_meter.hpp"' in open(a.source, encoding="utf-8").read(), "the page model includes the meter")

    # 1. The sweep: every hashing site classified.
    print("hashing sites (line  function  class  reason):")
    by_fn = {}
    for i in hashes:
        fn = enclosing(funcs, i)
        by_fn.setdefault(fn, []).append(i)
        cls = "FED" if fn in FED else "EXCLUDED" if fn in EXCLUDED else "UNCLASSIFIED"
        why = FED[fn][1] if fn in FED else EXCLUDED.get(fn, "-")
        print("  %5d  %-32s %-12s %s" % (i + 1, fn, cls, why))
    unclassified = sorted(set(by_fn) - set(FED) - set(EXCLUDED), key=str)
    check(not unclassified, "every hashing site sits in a FED or EXCLUDED function (unclassified: %s)" % unclassified)
    print("feeds (line  function):")
    feed_fn = {}
    for i in feeds:
        fn = enclosing(funcs, i)
        feed_fn.setdefault(fn, []).append(i)
        print("  %5d  %s" % (i + 1, fn))
    for fn, (n, _) in sorted(FED.items()):
        check(fn in span, "%s is defined in the page model" % fn)
        check(len(feed_fn.get(fn, [])) == n, "%s carries %d feed(s) (has %d)" % (fn, n, len(feed_fn.get(fn, []))))
    for fn in sorted(EXCLUDED):
        check(fn in span, "%s is defined in the page model" % fn)
        check(not feed_fn.get(fn), "%s (excluded: %s) carries no feed" % (fn, EXCLUDED[fn]))
    stray = sorted(set(feed_fn) - set(FED), key=str)
    check(not stray, "no feed outside the FED functions (stray: %s)" % stray)
    check(len(feeds) == sum(n for n, _ in FED.values()), "%d feeds in all" % sum(n for n, _ in FED.values()))

    # 2. Each feed at its place in the control flow (the anchors the fixes were made against).
    def within(name):
        return span.get(name, (0, 0))

    # pm4ShmRequestT: the range-cache hit feeds every page of the range right before the cached return; the write
    # watch's clean page feeds right after its counter, before its `continue`; the dedup blocks feed nothing.
    s, e = within("pm4ShmRequestT")
    f = feed_fn.get("pm4ShmRequestT", [])
    rc = [i for i in f if "p1 - p0 + 1u" in lines[i]]
    check(len(rc) == 1 and lines[rc[0] + 1].strip().startswith("memoStore.armed = true;") and lines[rc[0] + 2].strip() == "return true;",
          "range-cache hit: add(kShm, p1 - p0 + 1u) immediately before the cached `return true`")
    hit = find(lines, "v.sum == rce->ownSum) {", s, e)
    check(hit >= 0 and rc and hit < rc[0] < find(lines, "(rel ? g_rcMissRel : g_rcMissOwn)", s, e),
          "range-cache feed inside the hit block (after the ownSum test, before the miss accounting)")
    chk = find(lines, "if (nbShmRangeCheckOn()) {", s, e)
    chk_end = find(lines, "g_rcChecked.fetch_add", s, e)
    check(chk >= 0 and chk_end > chk and not any(chk <= i <= chk_end for i in f),
          "SR_NB_SHM_RANGE_CHECK's re-check of the hit (shmWtCheck per page) carries no feed")
    ww = [i for i in f if i not in rc]
    check(len(ww) == 1 and lines[ww[0] - 1].strip() == "m_shmFramePagesClean++;" and lines[ww[0] + 1].strip() == "continue;",
          "write-watch clean page: add(kShm) after m_shmFramePagesClean++, before its continue")
    memo = find(lines, "memo->epoch == m_shmEpoch && m_shmEpoch) return true;", s, e)
    check(memo >= 0 and FEED not in lines[memo], "range memo (same-epoch dedup): no feed")
    lean = find(lines, "__ATOMIC_RELAXED) == ep;", s, e)
    lean_end = find(lines, "return true;", lean, e)
    check(lean >= 0 and not any(lean <= i <= lean_end for i in f), "SR_CP_SHM_LEAN all-stamped return (dedup): no feed")
    lazy = find(lines, "if (!done && !(t_shmPrepass && m_shmResolveMask[p])) need = true;", s, e)
    lazy_end = find(lines, "return true;", lazy, e)
    check(lazy >= 0 and not any(lazy <= i <= lazy_end for i in f), "lazy-window pre-check return (dedup): no feed")
    ep = find(lines, "if (collect ? m_shmEpochOf[p] == m_shmEpoch : m_shmEpochOf[p] >= m_shmEpochFrameStart) {", s, e)
    ep_end = find(lines, "continue;", ep, e)
    check(ep >= 0 and not any(ep <= i <= ep_end for i in f), "per-page epoch stamp skip (dedup): no feed")
    pre = find(lines, "if (t_shmPrepass && m_shmResolveMask[p]) continue;", s, e)
    check(pre >= 0 and FEED not in lines[pre], "prepass resolve-mask skip (decided at the bind): no feed")

    # pm4ShmValidate: one feed of cands.size() where the hashed-pages counter is kept.
    f = feed_fn.get("pm4ShmValidate", [])
    check(len(f) == 1 and "cands.size()" in lines[f[0]] and "m_shmFramePagesHashed +=" in lines[f[0] - 1],
          "pm4ShmValidate: add(kShm, cands.size()) beside m_shmFramePagesHashed")

    # pm4ShmRangeWrittenByGpu (locked): the feed and every hash inside the per-page `rebase` lambda.
    s, e = within("pm4ShmRangeWrittenByGpu")
    lam = find(lines, "auto rebase = [&]", s, e)
    la, lb = lambda_span(lines, lam, e) if lam >= 0 else (0, 0)
    f = feed_fn.get("pm4ShmRangeWrittenByGpu", [])
    check(lam >= 0 and len(f) == 1 and la < f[0] < lb, "locked rebaseline: add(kShm) inside the per-page rebase lambda")
    check(all(la < i < lb and f and i > f[0] for i in by_fn.get("pm4ShmRangeWrittenByGpu", [])),
          "locked rebaseline: every hash follows the feed in that lambda")

    # rebaseHashPages: one feed in hashNeeded (needed pages only: the fresh ones left the walk), one in hashPage after
    # its fresh return; every hash inside one of the two lambdas, after its feed.
    s, e = within("rebaseHashPages")
    hn = find(lines, "auto hashNeeded =", s, e)
    hp = find(lines, "auto hashPage =", s, e)
    hna, hnb = lambda_span(lines, hn, e) if hn >= 0 else (0, 0)
    hpa, hpb = lambda_span(lines, hp, e) if hp >= 0 else (0, 0)
    f = feed_fn.get("rebaseHashPages", [])
    fn_ = [i for i in f if hna < i < hnb]
    fp_ = [i for i in f if hpa < i < hpb]
    check(hn >= 0 and len(fn_) == 1, "rebaseHashPages: one feed inside hashNeeded")
    fresh_ret = find(lines, "return;", find(lines, "r.fresh = true;", hpa, hpb), hpb)
    check(hp >= 0 and len(fp_) == 1 and fresh_ret >= 0 and fp_[0] > fresh_ret,
          "rebaseHashPages: one feed inside hashPage, after the fresh page's return (kept pages are decided at the commit)")
    check(all((hna < i < hnb and fn_ and i > fn_[0]) or (hpa < i < hpb and fp_ and i > fp_[0]) for i in by_fn.get("rebaseHashPages", [])),
          "rebaseHashPages: every hash follows its lambda's feed")

    # pm4ShmRangeWrittenByGpuUnlocked: the feed at the top of `if (r.fresh) {`, before the keep test, so the keep and
    # the lost-protection fallback are one decision; the checker's hashes inside the keep carry no feed; the non-fresh
    # commit (hashed and counted in rebaseHashPages) carries none.
    s, e = within("pm4ShmRangeWrittenByGpuUnlocked")
    fr = find(lines, "if (r.fresh) {", s, e)
    kept = find(lines, "m_shmWtRebaseKept++;", s, e)
    chk = find(lines, "if (sr::kernel::gww::checkMode()) {", s, e)
    lost = find(lines, "// Lost the protection since the unlocked check", s, e)
    commit = find(lines, "if (r.has8) m_shmHash8[p] = r.k8;", s, e)
    f = feed_fn.get("pm4ShmRangeWrittenByGpuUnlocked", [])
    check(fr >= 0 and kept > fr and len(f) == 1 and fr < f[0] < kept,
          "prehash-fresh page: add(kShm) at the top of `if (r.fresh)`, before the keep test (keep and fallback: one decision)")
    hs = by_fn.get("pm4ShmRangeWrittenByGpuUnlocked", [])
    check(chk > kept and lost > chk and commit > lost and hs and all(fr < i < commit for i in hs),
          "prehash-fresh page: every hash of the function (the checker's and the fallback's) lies in that `if (r.fresh)` block")
    check(sum(1 for i in hs if i > lost) == 2 and sum(1 for i in hs if chk < i < lost) == 2,
          "prehash-fresh page: two fallback hashes after the lost-protection comment, two checker hashes in the checkMode block")
    check(not any(commit <= i <= e for i in f), "non-fresh commit (hashed and counted in rebaseHashPages): no feed")

    # pm4ShmReadback: the feed inside the per-page loop, before the hash.
    s, e = within("pm4ShmReadback")
    loop = find(lines, "for (uint32_t p = start >> 12;", s, e)
    f = feed_fn.get("pm4ShmReadback", [])
    hs = by_fn.get("pm4ShmReadback", [])
    check(loop >= 0 and len(f) == 1 and len(hs) == 1 and loop < f[0] < hs[0], "readback: add(kShm) in the per-page loop, before its hash")

    # 3. The acceptance table (the paths that never hash), by anchor.
    print("acceptance paths (line  fed  what):")
    for text, fed, what in ACCEPTS:
        i = find(lines, text)
        print("  %5d  %-8s %s" % (i + 1, "fed" if fed else "excluded", what))
        check(i >= 0, "anchor present: " + what)

    # 4. One rule, stated once: the header and the docs name the decision events and exclude the checker; the
    # superseded phrasing (one count per page per frame, the checker's hashes counted) is gone.
    hdr = open(a.header, encoding="utf-8").read()
    comp = open(a.composition, encoding="utf-8").read()
    road = open(a.roadmap, encoding="utf-8").read()
    check("ONE RULE" in hdr and "RE-BASELINE" in hdr and "SR_SHM_WT_CHECK" in hdr and "range cache" in hdr,
          "native_meter.hpp states the one rule (decision events: validation / re-baseline incl. the range cache; the checker excluded)")
    check("one count per page per frame" not in comp and "pages hashed by `SR_SHM_WT_CHECK` when it is" not in comp,
          "RELEASE_COMPOSITION.md no longer counts one per page per frame with the checker's hashes")
    check("ONE RULE" in comp and "checker" in comp.split("**The `shm` meter column", 1)[-1][:3000],
          "RELEASE_COMPOSITION.md section 7 states the one rule with the checker excluded")
    check("ONE COUNTING RULE" in road and "`SR_SHM_WT_CHECK` / `SR_NB_SHM_RANGE_CHECK` hashes" in road,
          "NATIVE_ROADMAP states the one rule with the oracles excluded")

    print("shm meter wiring test: %d failed" % FAILS)
    return FAILS


if __name__ == "__main__":
    sys.exit(main())
