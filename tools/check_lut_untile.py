"""check_lut_untile.py - does an untiling produce real structure?

⚠ **THE HYPOTHESIS THIS WAS BUILT FOR IS CLOSED** (director, 2026-09-01). It was
written to test whether `023C8000` is a colour-grading LUT our 3D untiler was
scrambling. Both halves of that premise were already answered in the repo and I
did not check:

* `docs/MIGRATION_API_HLE.md` §iter-456 already records these as **cloud-noise
  volumes, not grading LUTs** ("no decode variant of 023C8000 is a smooth ramp").
* `gfx/xenos_tiling.hpp::tiled3D` is a **line-for-line port of Xenia's
  authoritative `texture_address::Tiled3D`** (`reference/xenia-canary/`), so
  "our untiler might be wrong" was never an even fork against the reference.

So a noise result here is the EXPECTED reading for a noise volume, and a third
case -- intentionally valid noise -- sits alongside "bad addressing" and "bad
bytes". Do not spend another iteration treating this volume as a grading LUT.

The tool remains useful for its general question:

    after untiling, does a volume have real spatial STRUCTURE?

    python tools/check_lut_untile.py --self-test
    python tools/check_lut_untile.py <trace> --key 023c8000

## What "LUT structure" means, and why it is testable

A colour-grading LUT maps input RGB to graded output RGB. Whatever the grade, a
LUT is a *function of position*: moving along the x axis changes the input red,
so the stored red output must rise with x. So for a well-formed LUT the
Pearson correlation between an axis and its corresponding output channel is
**strongly positive**, and it stays that way for any plausible artistic grade —
a grade that inverted or destroyed that relationship would not be usable.

A WRONG untiling permutes voxels. It preserves every value (so channel means
cannot see it — see `iter-457-lut-suspect`) but it destroys the position/value
relationship, driving those correlations toward zero.

## ⚠ The control is the point

Reimplementing `tiled3D` in Python and reporting that it "works" would only test
my transcription. So this scores THREE interpretations of the same bytes:

    ours    - the exact tiled3D from gfx/xenos_tiling.hpp
    linear  - no untiling at all (z*H*W + y*W + x)
    swapped - linear with the x/z axes exchanged

If `linear` scores as well as `ours`, the data was never tiled. If NONE of them
shows structure, the volume is not positionally structured -- which for this
game's scene volumes is the DOCUMENTED expectation, not a discovery.

⚠ This tool alone cannot separate "bad addressing" from "bad bytes" from
"intentional noise". The repo's own reference source and prior findings must be
consulted before it is used to accuse anything -- which is exactly the step I
skipped.

Read-only.
"""
import argparse, math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MACRO_W_LOG2 = 5
MACRO_H3D_LOG2 = 4
MACRO_D_LOG2 = 2


def tiled_combine64(outer_inner, bank, pipe, y_lsb):
    """Mirror of tiledCombine64 (gfx/xenos_tiling.hpp)."""
    return (((y_lsb << 4) | (pipe << 6) | (bank << 11))
            | (outer_inner & 0b1111)
            | (((outer_inner >> 4) & 0b1) << 5)
            | (((outer_inner >> 5) & 0b111) << 8)
            | ((outer_inner >> 8) << 12))


def tiled3d(x, y, z, pitch_aligned, height_aligned, bpb_log2):
    """Mirror of tiled3D (gfx/xenos_tiling.hpp) - OUR untiler, transcribed."""
    outer = ((((z >> MACRO_D_LOG2) * (height_aligned >> MACRO_H3D_LOG2)
               + (y >> MACRO_H3D_LOG2)) * (pitch_aligned >> MACRO_W_LOG2))
             + (x >> MACRO_W_LOG2)) << 7
    inner = ((z & 0b11) << 5) | (((y >> 1) & 0b11) << 3) | (x & 0b111)
    outer_inner = (outer | inner) << bpb_log2
    bank = ((y >> 3) ^ (z >> 2)) & 0b1
    pipe = ((x >> 3) & 0b11) ^ (bank << 1)
    return tiled_combine64(outer_inner, bank, pipe, y & 1)


def swap32(v, endian):
    e = endian & 3
    if e == 1:
        return ((v & 0xFF00FF00) >> 8) | ((v & 0x00FF00FF) << 8)
    if e == 2:
        return struct.unpack("<I", struct.pack(">I", v))[0]
    if e == 3:
        return ((v >> 16) | (v << 16)) & 0xFFFFFFFF
    return v


def pearson(a, b):
    n = len(a)
    if n < 2:
        return 0.0
    ma, mb = sum(a) / n, sum(b) / n
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    da = math.sqrt(sum((x - ma) ** 2 for x in a))
    db = math.sqrt(sum((y - mb) ** 2 for y in b))
    return 0.0 if da == 0 or db == 0 else num / (da * db)


def score(fetch, n, label):
    """Correlate each axis with its channel. Returns (r_corr, g_corr, b_corr)."""
    xs, ys, zs, rs, gs, bs = [], [], [], [], [], []
    step = max(1, n // 16)                     # a lattice, not all 262k voxels
    for z in range(0, n, step):
        for y in range(0, n, step):
            for x in range(0, n, step):
                px = fetch(x, y, z)
                if px is None:
                    return None
                xs.append(x); ys.append(y); zs.append(z)
                rs.append(px[0]); gs.append(px[1]); bs.append(px[2])
    return (pearson(xs, rs), pearson(ys, gs), pearson(zs, bs))


def coherence(fetch, n, seed=5, step=3):
    """Mean |delta| between x-neighbours, and the same under a RANDOM
    permutation of the same voxels.

    This needs NO assumption that the volume is a LUT: any real texture is
    spatially coherent, and a wrong untiling destroys that. The random control
    is the whole point -- an absolute number means nothing without it, and a
    FLAT texture scores a low ratio for an innocent reason, so the absolute
    delta must be reported beside it.
    """
    import random
    rnd = random.Random(seed)
    perm = list(range(n * n * n))
    rnd.shuffle(perm)

    def run(f):
        tot = cnt = 0
        for z in range(0, n, step):
            for y in range(0, n, step):
                for x in range(0, n - 1):
                    a_, b_ = f(x, y, z), f(x + 1, y, z)
                    if a_ and b_:
                        tot += sum(abs(p - q) for p, q in zip(a_, b_))
                        cnt += 3
        return tot / cnt if cnt else 0.0

    def rand_fetch(x, y, z):
        i = perm[(z * n + y) * n + x]
        return fetch(i % n, (i // n) % n, i // (n * n))

    return run(fetch), run(rand_fetch)


def tiled2d(x, y, pitch_aligned, bpb_log2):
    """Mirror of tiled2D (gfx/xenos_tiling.hpp) - for the CALIBRATION survey."""
    outer = ((y >> 5) * (pitch_aligned >> MACRO_W_LOG2) + (x >> MACRO_W_LOG2)) << 6
    inner = (((y >> 1) & 0b111) << 3) | (x & 0b111)
    oi = (outer | inner) << bpb_log2
    bank = (y >> 4) & 0b1
    pipe = ((x >> 3) & 0b11) ^ (((y >> 3) & 0b1) << 1)
    return tiled_combine64(oi, bank, pipe, y & 1)


def survey_2d(trace, limit=24):
    """Coherence of tiled 32bpp 2D uploads - THE CALIBRATION.

    A 1.08x coherence ratio means nothing on its own; it only becomes a finding
    beside 2D textures from the SAME fixture that untile at 8x-11x. The director
    noted this survey was run ad-hoc in a shell and was therefore absent from
    the tool's own repro, so the published conclusion could not be reproduced by
    the command that claimed to produce it. It lives here now.
    """
    import random
    import scanout_route as sr
    rnd = random.Random(3)
    seen, rows = set(), []
    for rec, t, d, p in sr.walk(trace, payload_types=(18,)):
        if t != 18 or not p or len(p) < 16384:
            continue
        key = d[0] & 0x1FFFFFFF
        w, h, fb = d[3] >> 16, d[3] & 0xFFFF, d[4]
        if ((fb & 0xFF) != 6 or not ((fb >> 8) & 1) or ((fb >> 14) & 3) != 0
                or w < 64 or w != h or key in seen):
            continue
        seen.add(key)
        endian = (fb >> 12) & 3
        pitch = (w + 31) & ~31

        def texel(off, _p=p, _e=endian):
            if off < 0 or off + 4 > len(_p):
                return None
            v = swap32(struct.unpack("<I", _p[off:off + 4])[0], _e)
            b = struct.pack(">I", v)
            return b[0], b[1], b[2]

        perm = list(range(w * h))
        rnd.shuffle(perm)

        def mean_delta(f):
            tot = cnt = 0
            for y in range(0, h, 4):
                for x in range(0, w - 1, 2):
                    a_, b_ = f(x, y), f(x + 1, y)
                    if a_ and b_:
                        tot += sum(abs(u - v) for u, v in zip(a_, b_))
                        cnt += 3
            return tot / cnt if cnt else 0.0

        # mean_delta wants TEXELS; tiled2d returns an OFFSET.
        got = mean_delta(lambda x, y: texel(tiled2d(x, y, pitch, 2)))
        rnd_d = mean_delta(lambda x, y: texel(perm[y * w + x] * 4))
        rows.append((key, w, got, rnd_d, (rnd_d / got) if got else 0.0))
        if len(rows) >= limit:
            break
    return rows


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

    print("LUT UNTILE CHECK (synthetic; no trace)")
    N = 16

    # A synthetic IDENTITY lut laid out linearly.
    ident = bytearray(N * N * N * 4)
    for z in range(N):
        for y in range(N):
            for x in range(N):
                o = ((z * N + y) * N + x) * 4
                ident[o] = x * 255 // (N - 1)
                ident[o + 1] = y * 255 // (N - 1)
                ident[o + 2] = z * 255 // (N - 1)
                ident[o + 3] = 255

    def lin(buf):
        def f(x, y, z):
            o = ((z * N + y) * N + x) * 4
            return buf[o], buf[o + 1], buf[o + 2]
        return f

    s = score(lin(ident), N, "identity")
    ck(all(c > 0.99 for c in s),
       "an IDENTITY lut read correctly scores ~1.0 on all three axes (got %s)"
       % [round(c, 3) for c in s])

    # KILLER: a scrambled read must NOT score as a LUT. This is the whole
    # premise - if scrambling still scored high, the metric would prove nothing.
    import random
    rnd = random.Random(11)
    perm = list(range(N * N * N))
    rnd.shuffle(perm)

    def scrambled(x, y, z):
        i = perm[(z * N + y) * N + x] * 4
        return ident[i], ident[i + 1], ident[i + 2]

    s2 = score(scrambled, N, "scrambled")
    ck(all(abs(c) < 0.35 for c in s2),
       "KILLER a SCRAMBLED read of the same bytes collapses to ~0 (got %s) - "
       "so the metric detects a wrong untiling, which is its entire purpose"
       % [round(c, 3) for c in s2])

    # KILLER: channel means cannot tell these apart - the reason this tool
    # exists rather than reusing the mean check from iter-457-lut-suspect.
    m1 = sum(ident[0::4]) / (N ** 3)
    m2 = sum(ident[perm[i] * 4] for i in range(N ** 3)) / (N ** 3)
    ck(abs(m1 - m2) < 1e-6,
       "KILLER the two have IDENTICAL channel means (%.2f vs %.2f) - a mean "
       "test cannot see a wrong untiling at all" % (m1, m2))

    # A strong artistic grade must still score high: the metric must not
    # mistake "graded" for "broken".
    graded = bytearray(ident)
    for i in range(0, len(graded), 4):
        graded[i] = min(255, int(graded[i] * 0.6))          # crush red
        graded[i + 2] = min(255, int(graded[i + 2] ** 0.9))  # lift blue
    s3 = score(lin(graded), N, "graded")
    ck(all(c > 0.95 for c in s3),
       "a STRONGLY GRADED lut still scores high (got %s) - the test measures "
       "structure, not neutrality" % [round(c, 3) for c in s3])

    # an axis-swapped read is a realistic wrong-layout case
    def swapped(x, y, z):
        o = ((x * N + y) * N + z) * 4
        return ident[o], ident[o + 1], ident[o + 2]

    s4 = score(swapped, N, "axis-swapped")
    ck(s4[0] < 0.35 and s4[2] < 0.35,
       "an AXIS-SWAPPED read loses the x/z correlations (got %s)"
       % [round(c, 3) for c in s4])

    # ── the COHERENCE metric, which the director noted was never exercised ──
    # It is the metric that carried the published conclusion, and it had no
    # fixture at all. A metric with no test is an opinion with a number.
    def lin_f(buf):
        def f(x, y, z):
            o = ((z * N + y) * N + x) * 4
            return buf[o], buf[o + 1], buf[o + 2]
        return f

    smooth = bytearray(N * N * N * 4)
    for z in range(N):
        for y in range(N):
            for x in range(N):
                o = ((z * N + y) * N + x) * 4
                v = (x * 255) // (N - 1)
                smooth[o] = smooth[o + 1] = smooth[o + 2] = v
                smooth[o + 3] = 255
    g, r = coherence(lin_f(smooth), N)
    ck(g < r / 2.0,
       "COHERENCE: a smooth ramp is far smoother than a shuffle of its own "
       "bytes (%.2f vs %.2f)" % (g, r))

    def scram_f(x, y, z):
        i = perm[(z * N + y) * N + x] * 4
        return smooth[i], smooth[i + 1], smooth[i + 2]

    g2, r2 = coherence(scram_f, N)
    ck(abs(g2 - r2) < max(1.0, r2 * 0.25),
       "KILLER COHERENCE: a SCRAMBLED volume is indistinguishable from a "
       "shuffle (%.2f vs %.2f) - which is exactly the 1.08x reading on the "
       "real volume" % (g2, r2))

    total = npass + nfail
    print("\n%d/%d untile fixtures behave as specified" % (npass, total))
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
    ap.add_argument("--key", default="023c8000")
    ap.add_argument("--survey-2d", action="store_true",
                    help="the 2D coherence CALIBRATION on the same trace")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not a.trace:
        _print_doc()
        return 2

    import scanout_route as sr

    if a.survey_2d:
        rows = survey_2d(a.trace)
        print("2D CALIBRATION - tiled 32bpp uploads from the SAME fixture")
        print("%-12s %-10s %8s %8s %7s  %s"
              % ("key", "size", "untiled", "random", "ratio", "verdict"))
        coh = noise = flat = 0
        for k, w, g, r, ratio in sorted(rows, key=lambda t: -t[4]):
            if g < 3.0:
                v = "FLAT (ratio not meaningful)"; flat += 1
            elif ratio >= 2.0:
                v = "coherent"; coh += 1
            else:
                v = "NOISE-LIKE"; noise += 1
            print("%#010x  %4dx%-4d %8.2f %8.2f %6.2fx  %s"
                  % (k, w, w, g, r, ratio, v))
        print()
        print("%d surveyed: %d coherent, %d noise-like, %d flat"
              % (len(rows), coh, noise, flat))
        print("This is the CALIBRATION the volume result must be read against.")
        return 0

    key = int(a.key, 16) & 0x1FFFFFFF
    payload = dims = fmtbits = None
    for rec, t, d, p in sr.walk(a.trace, payload_types=(18,)):
        if t == 18 and (d[0] & 0x1FFFFFFF) == key and p:
            payload, dims, fmtbits = p, d[3], d[4]
            break
    if payload is None:
        print("FAIL: no TextureUpload with a payload for key %#010x" % key)
        return 1

    w, h = dims >> 16, dims & 0xFFFF
    depth = ((fmtbits >> 16) & 0x3FF) + 1
    tiled = ((fmtbits >> 8) & 1) != 0
    endian = (fmtbits >> 12) & 3
    print("key %#010x  %ux%ux%u  tiled=%d endian=%u  payload=%d bytes"
          % (key, w, h, depth, tiled, endian, len(payload)))
    if w != h or h != depth:
        print("NOTE: not a cube (%ux%ux%u); the axis correlations still apply."
              % (w, h, depth))

    pitch = (w + 31) & ~31 if tiled else w
    hA = (h + 31) & ~31

    def texel(off):
        if off < 0 or off + 4 > len(payload):
            return None
        v = struct.unpack("<I", payload[off:off + 4])[0]
        v = swap32(v, endian)
        b = struct.pack(">I", v)
        return b[0], b[1], b[2]

    def f_ours(x, y, z):
        return texel(tiled3d(x, y, z, pitch, hA, 2))

    def f_linear(x, y, z):
        return texel(((z * h + y) * w + x) * 4)

    def f_swapped(x, y, z):
        return texel(((x * h + y) * w + z) * 4)

    print()
    print("%-10s %-34s %s" % ("layout", "axis->channel correlation", "verdict"))
    results = {}
    for name, f in (("ours", f_ours), ("linear", f_linear),
                    ("swapped", f_swapped)):
        s = score(f, min(w, h, depth), name)
        if s is None:
            print("%-10s (out of range)" % name)
            continue
        results[name] = s
        m = sum(s) / 3.0
        v = ("LUT structure" if m > 0.7 else
             "weak" if m > 0.35 else "NO LUT structure")
        print("%-10s x->R %+.3f  y->G %+.3f  z->B %+.3f   mean %+.3f  %s"
              % (name, s[0], s[1], s[2], m, v))

    # ── the LUT-free check: spatial coherence vs a random control ────────
    n = min(w, h, depth)
    got, rnd = coherence(f_ours, n)
    ratio = (rnd / got) if got else 0.0
    print()
    print("spatial coherence (no LUT assumption): mean |delta| between "
          "x-neighbours")
    print("   our untiling %6.2f   random permutation %6.2f   ratio %.2fx"
          % (got, rnd, ratio))
    if got < 3.0:
        print("   -> the volume is nearly FLAT, so the ratio is not meaningful.")
    elif ratio < 2.0:
        print("   -> NOISE-LIKE. A correctly untiled real texture is far "
              "smoother than a shuffle of its own bytes.")
    else:
        print("   -> COHERENT: the untiling is recovering real structure.")

    print()
    if not results:
        print("REFUSED: no layout could be evaluated.")
        return 2
    best = max(results, key=lambda k: sum(results[k]) / 3.0)
    bm = sum(results[best]) / 3.0
    if bm <= 0.35:
        print("FINDING: NO layout shows LUT structure. Either this volume is "
              "not a colour LUT, or the payload is not what we think it is.")
        print("         Our untiler is NOT exonerated and NOT convicted - the "
              "test cannot separate them in this case.")
        return 1
    if best == "ours":
        print("Our untiler produces the best LUT structure (mean %+.3f). It is "
              "EXONERATED as the source of the cast." % bm)
        return 0
    print("FINDING: '%s' beats our untiler (%+.3f vs %+.3f). Our 3D untiling "
          "is doing damage." % (best, bm, sum(results['ours']) / 3.0))
    return 1


if __name__ == "__main__":
    sys.exit(main())
