"""check_band_conformance.py - the acceptance test for the resolve-product band.

The conformance rule, in one sentence:

    **Every resolve product must be registered in the descriptor band its
    consumers actually read it from.**

    python tools/check_band_conformance.py --self-test
    python tools/check_band_conformance.py <trace> --rexec <f> --admit <f> \\
        --ps-bands <f>

## ⚠ r2 — the director found this tool could not pass

r1 hardcoded `REGISTRABLE_BY_RESOLVE = {"2D"}` as a module constant, so the test
could only ever go green if a human EDITED IT. Handed real gate-on execution
evidence it still exited 1. An acceptance test that cannot be turned green by
evidence is not an acceptance test; it is an assertion about the author's
beliefs.

r2 derives the registrable band **per product, from the execution sidecar**:

    a resolve whose outcome is `ok`    -> that product was registered in 2D
    a resolve whose outcome is `ok3d`  -> that product was registered in 3D

joined to the trace by destination key. So the same command answers differently
on gate-off and gate-on evidence, which is the whole point:

    gate-off sidecar -> exit 1, exactly one violation (0B5BE000)
    gate-on  sidecar -> exit 0
    missing/empty    -> exit 2, REFUSED (never a vacuous pass)

⚠ What it does NOT check: that the registered descriptor has the right CONTENT,
extent, depth or slice. **Band membership only.** A product registered in the
right band with the wrong pixels passes this and is still broken.
"""
import argparse, collections, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

BAND_2D, BAND_3D, BAND_CUBE = "2D", "3D", "CUBE"

# Resolve-sidecar outcome -> the descriptor band that resolve registered into.
# `ok` is the 2D path (s_texSlot); `ok3d` is the stacked path (s_texSlot3D).
OUTCOME_BAND = {"ok": BAND_2D, "ok3d": BAND_3D}

# ⛔ THE FAIL-OPEN THE DIRECTOR CAUGHT (r3). r2 built the product set from
# `OUTCOME_BAND.get(outcome) is not None`, so an EXECUTED resolve with an
# outcome this tool did not recognise made its product -- and every reader of
# it -- VANISH, and the run returned 0/CONFORMANT. Reproduced: rewriting one
# executed `ok3d` row as `1 ok4d_future` still exited 0.
#
# My own fixture missed it because it only proved `registrable_bands()` returns
# {} for an unknown string; it never drove the production filtering that erased
# the row. Testing a function is not testing the path.
#
# r3: the schema is CLOSED and validated up front. Product existence comes from
# the EXECUTED BIT; an unrecognised outcome REFUSES by name.
KNOWN_OUTCOMES = {
    "ok", "ok3d",                       # executed, and which band it registered
    "sliced", "rect",                   # guest-side structural refusals
    "refused", "refused3d",             # backend refusals (may carry :<code>)
    "noBackend", "noSource", "gateOff", "unreached",
}


def outcome_base(outcome):
    """`refused:7` -> `refused`. The backend appends a ResolveSkip code."""
    return outcome.split(":", 1)[0]


def schema_violations(rows):
    """[(record, outcome)] for outcomes this tool does not recognise.

    Checked BEFORE products are built, so an unknown outcome can never quietly
    remove a product from consideration.
    """
    return sorted((rec, o) for rec, (_x, o) in rows.items()
                  if outcome_base(o) not in KNOWN_OUTCOMES)


def registrable_bands(resolve_rows, dest_of_record):
    """product key -> the set of bands it was ACTUALLY registered in.

    `resolve_rows`: record -> outcome string, from the execution sidecar.
    `dest_of_record`: record -> destination key, from the trace.

    Derived from evidence, never assumed. A product with no executed resolve
    appears nowhere, which is correct: nothing registered it.
    """
    out = {}
    for rec, val in resolve_rows.items():
        executed, outcome = val if isinstance(val, tuple) else (True, val)
        if not executed:
            continue
        band = OUTCOME_BAND.get(outcome_base(outcome))
        if band is None:
            continue
        key = dest_of_record.get(rec)
        if key is None:
            continue
        out.setdefault(key, set()).add(band)
    return out


def violations(products, reader_bands, registrable):
    """[(key, reader bands, draws, registered bands)] for unreachable products.

    A product is a violation when it HAS admitted readers and NONE of the bands
    they read it from is a band it was actually registered in. A product with no
    reader is NOT a violation - the front key is presented, never sampled.
    """
    out = []
    for key in sorted(products):
        bands = reader_bands.get(key)
        if not bands:
            continue
        reg = registrable.get(key, set())
        if not (set(bands) & reg):
            out.append((key, dict(bands), sum(bands.values()), sorted(reg)))
    return out


def self_test():
    npass = nfail = 0

    def ck(ok, what):
        nonlocal npass, nfail
        if ok:
            npass += 1
            print("  PASS  " + what)
        else:
            nfail += 1
            print("  FAIL  " + what)

    print("BAND CONFORMANCE r2 (synthetic; no trace)")
    C = collections.Counter

    prods = {0xA, 0xB, 0xC, 0xD}
    readers = {
        0xA: C({BAND_2D: 18}),
        0xB: C({BAND_3D: 672}),             # the stacked product
        0xC: C({BAND_2D: 6, BAND_3D: 2}),   # mixed
    }
    # gate-OFF evidence: every executed resolve registered 2D
    dest = {1: 0xA, 2: 0xB, 3: 0xC, 4: 0xD}
    off = registrable_bands({1: "ok", 2: "ok", 3: "ok", 4: "ok"}, dest)
    v = violations(prods, readers, off)
    ck([k for k, _b, _n, _r in v] == [0xB],
       "GATE-OFF evidence: the 3D-only product is the sole violation (got %s)"
       % ["%#x" % k for k, _b, _n, _r in v])
    ck(v and v[0][2] == 672, "the violation carries its affected draw count")

    # KILLER: the same tool, given GATE-ON evidence, must go GREEN with no edit.
    # r1 could not do this at all -- it would have needed its constant changed.
    on = registrable_bands({1: "ok", 2: "ok3d", 3: "ok", 4: "ok"}, dest)
    ck(violations(prods, readers, on) == [],
       "KILLER GATE-ON evidence (`ok3d`) clears the violation with NO source "
       "edit - the r1 hardcoded constant could never do this")

    # KILLER: a product with NO reader must not be flagged. The FRONT key is
    # presented, never sampled - flagging it would keep the test permanently red
    # for a correct thing, which is how a red test gets ignored.
    ck(0xD not in [k for k, _b, _n, _r in v],
       "KILLER a product with NO admitted reader is NOT a violation (the front "
       "key is presented, not sampled)")

    ck(0xC not in [k for k, _b, _n, _r in v],
       "KILLER a product with at least one reachable band is NOT a violation")

    # KILLER: a resolve that did NOT execute registers nothing.
    none_exec = registrable_bands({1: "sliced", 2: "refused", 3: "rect"}, dest)
    ck(none_exec == {},
       "KILLER refused resolves (`sliced`/`refused`/`rect`) register NOTHING "
       "(got %s)" % none_exec)

    # KILLER: an outcome we do not recognise must register nothing rather than
    # defaulting to a band - a new backend outcome must fail loudly, not quietly
    # be treated as 2D.
    unknown = registrable_bands({1: "ok4d_future"}, dest)
    ck(unknown == {},
       "KILLER an UNRECOGNISED outcome registers nothing rather than "
       "defaulting to a band (got %s)" % unknown)

    ck(violations(set(), {}, {}) == [],
       "no products means no violations (vacuous, and the caller must say so)")

    # KILLER: the r2 FAIL-OPEN. An unrecognised outcome on an EXECUTED resolve
    # must be caught by the schema validator, because r2's unit fixture (below,
    # `unknown`) passed while production still returned 0/CONFORMANT -- the row
    # and its readers simply vanished. Testing the function was not testing the
    # path.
    bad = schema_violations({7: (True, "ok4d_future"), 8: (True, "ok")})
    ck(bad == [(7, "ok4d_future")],
       "KILLER an EXECUTED resolve with an unrecognised outcome is a SCHEMA "
       "VIOLATION named by record (got %s)" % bad)
    ck(schema_violations({1: (False, "sliced"), 2: (True, "refused:7")}) == [],
       "known outcomes, including a `refused:<code>` suffix, are accepted")
    ck(registrable_bands({1: (False, "ok")}, {1: 0xA}) == {},
       "KILLER a row whose EXECUTED BIT is 0 registers nothing even when its "
       "outcome says `ok`")

    total = npass + nfail
    print("\n%d/%d conformance fixtures behave as specified" % (npass, total))
    return 0 if nfail == 0 else 1


def _print_doc():
    """Print the module docstring even on a cp1252 console.

    The usage path CRASHED with UnicodeEncodeError on the arrows and
    warning glyphs in these docstrings, so a user who forgot a flag got a
    traceback and exit 1 instead of usage and the documented exit code.
    The director's acceptance commands set PYTHONUTF8=1, which hid it.
    """
    try:
        print(__doc__)
    except UnicodeEncodeError:
        enc = sys.stdout.encoding or "ascii"
        print(__doc__.encode(enc, "replace").decode(enc))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace", nargs="?")
    ap.add_argument("--rexec")
    ap.add_argument("--admit")
    ap.add_argument("--ps-bands", help="pshash -> 'slot:BAND,...' per line")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not (a.trace and a.rexec and a.admit and a.ps_bands):
        _print_doc()
        return 2

    import scanout_route as sr

    # ALL THREE fields: record, executed bit, outcome. r2 kept only the
    # outcome, which is why product existence ended up keyed on whether this
    # tool happened to recognise a string.
    rows = {}
    for line in open(a.rexec, encoding="utf-8", errors="replace"):
        p = line.split("#", 1)[0].split()
        if len(p) >= 3:
            rows[int(p[0])] = (p[1] not in ("0", "false", "no"), p[2])

    bad = schema_violations(rows)
    if bad:
        print("REFUSED: %d resolve row(s) carry an outcome this tool does not "
              "recognise:" % len(bad))
        for rec, o in bad[:12]:
            print("   record %d: %r" % (rec, o))
        print("Known outcomes: %s" % sorted(KNOWN_OUTCOMES))
        print("An unrecognised outcome must NOT be guessed at and must NOT "
              "silently drop its product - that is the r2 fail-open.")
        return 2

    bands = {}
    for line in open(a.ps_bands, encoding="utf-8", errors="replace"):
        p = line.split("#", 1)[0].split()
        if len(p) >= 2:
            m = {}
            for tok in p[1].split(","):
                if ":" in tok:
                    sl, bd = tok.split(":", 1)
                    m.setdefault(int(sl), set()).add(bd)
            bands[int(p[0], 16)] = m
    adm = sr.load_admit(a.admit)

    # one walk: resolve destinations, products, and the reader join
    dest_of_record, products = {}, set()
    cur, tex = 0, {}
    reader_bands = collections.defaultdict(collections.Counter)
    for rec, t, d, _p in sr.walk(a.trace):
        if t == 20:
            dest_of_record[rec] = d[1] & 0x1FFFFFFF
            # ⛔ From the EXECUTED BIT, never from whether this tool recognises
            # the outcome string. That coupling was the fail-open.
            ex = rows.get(rec)
            if ex and ex[0]:
                products.add(d[1] & 0x1FFFFFFF)
        elif t == 15:
            cur = (d[4] << 32) | d[3]
        elif t == 9:
            tex[d[1]] = d[2] & 0x1FFFFFFF
        elif t in sr.DRAW_TYPES and rec in adm:
            for s, k in tex.items():
                if k in products:
                    for b in bands.get(cur, {}).get(s, ()):
                        reader_bands[k][b] += 1

    reg = registrable_bands(rows, dest_of_record)
    v = violations(products, reader_bands, reg)

    print("resolve products with >=1 executed resolve : %d" % len(products))
    print("products with at least one admitted reader : %d" % len(reader_bands))
    print("registrable bands DERIVED from the sidecar : %s"
          % {"%#010x" % k: sorted(b) for k, b in sorted(reg.items())})
    print()

    # ⛔ VACUITY GUARD. Caught in the act on r1: with the bands sidecar missing
    # this found ZERO readers and printed CONFORMANT - a test going green
    # because its INPUT was absent, which is worse than having no test at all.
    if products and not reader_bands:
        print("REFUSED: %d resolve products, but NOT ONE admitted reader was "
              "resolved from the bands sidecar." % len(products))
        print("That is a broken or empty --ps-bands input, NOT conformance. "
              "Regenerate with:")
        print("    python tools/gen_ps_sampler_map.py -o <samplers> "
              "--bands-out <bands>")
        return 2
    if not products:
        print("REFUSED: the execution sidecar records no EXECUTED resolve, so "
              "there is nothing to judge. Check --rexec.")
        return 2

    if not v:
        print("CONFORMANT: every product with a reader is readable in a band it "
              "was actually registered in.")
        return 0
    print("VIOLATIONS: %d product(s) no reader can reach" % len(v))
    for k, b, n, r in v:
        print("   %#010x  read via %s by %d admitted draw(s) - but it was "
              "registered only in %s" % (k, b, n, r or ["nothing"]))
    return 1


if __name__ == "__main__":
    sys.exit(main())
