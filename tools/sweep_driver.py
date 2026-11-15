"""sweep_driver.py - iter-457: the guarded treatment launcher.

The director's requirement, verbatim:

    "The existing split count guard is sufficient for the sweep, provided the
     eventual sweep driver - not merely the qualification script - always
     performs the fresh-process probe and verifies the same executable hash,
     trace hash, gates, and execution-affecting environment before launching
     treatment."

WHY A GUARD AT ALL. A replay can only detect a wrong admitted-draw count once it
has FINISHED, by which point it has already suppressed. An in-process counting
pass was tried and rejected as unsound: replay pass 2 does not reproduce pass 1
(measured 0 of 16 frames identical). So the count is established in a SEPARATE
clean process, and a treatment whose manifest disagrees is never launched - which
is the only way "a wrong manifest suppresses zero" can actually hold.

    python tools/sweep_driver.py --self-test              # guard fixtures, no replay
    python tools/sweep_driver.py --bucket <id> --dry-run  # prove the guards only
    python tools/sweep_driver.py --bucket <id> --i-have-authorization <ruling>

⚠ THE GATE. A treatment is an ATTRIBUTION experiment opened by the director, not
by this script. `--bucket` refuses without `--i-have-authorization` naming the
ruling that opened it, so an unauthorized sweep cannot be run by accident.

v1 of this file shipped an authorized path that printed "launching treatment ..."
and returned ZERO without setting a manifest, launching anything, verifying a
tally, capturing a frame or comparing an ROI - a scaffold behind a success
message (director, iter-457). The path below does all of it or fails.
"""
import argparse, hashlib, os, re, shutil, subprocess, sys
from collections import defaultdict
from PIL import Image, ImageStat

TRACE = "archive/iter456_title_fresh.srht"
PINNED_FRAME = "archive/iter456_title_fresh_dump/present_cl_010_p000010_1280x720.bmp"
PINNED_SHA = "c677fc5e15e9152cb7ae9b10ad5d1f0040886f48d43d0ce91a9d118d28040943"
PRESENTS = 16
# ⛔ NOT A MEASUREMENT TARGET. This names the frame whose SHA proves the
# fixture is the frozen one; present 10 is a REPEAT frame containing zero
# draws, so measuring a treatment there was the iter-457 defect. The present
# a treatment is graded at is DERIVED per manifest -- see derive_target().
BASELINE_IDENTITY_PRESENT = 10

# the director's corrected ROI set; tools/roi_freeze.py is the authority
ROIS = {
    "facade_left":   (0,    70,  175, 340),
    "facade_right":  (1020, 80, 1279, 620),
    "sky":           (340,  52,  680,  89),
    "spikes":        (176,  20,  330, 160),
    "superman_cape": (880, 360,  930, 405),
}

# Environment that CHANGES WHAT THE REPLAY DOES. The probe and the treatment must
# agree on every one, or the count the probe established does not describe the
# run the treatment performs.
EXECUTION_AFFECTING = [
    "SR_ALLOW_FORCING", "SR_HLE_TRACE_REPLAY", "SR_HLE_TRACE_LOOPS",
    "SR_DRAWID_SUPPRESS", "SR_DRAW_CONTRACT", "SR_S6_STRIPAB",
    "SR_HLE_SAMPSTATE", "SR_HLE_TEX1640_REG", "SR_HLE_VB_PUBLISH",
    "SR_HLE_RESOLVE_OWNERSHIP_V2", "SR_HLE_UNLOCK_UPLOAD_ALL",
    "SR_HLE_TEX_REFRESH", "SR_PROV_REPLAY_LIVE",
]

npass = nfail = 0


def check(ok, what):
    global npass, nfail
    if ok:
        npass += 1
        print("  PASS  " + what)
    else:
        nfail += 1
        print("  FAIL  " + what)
    return ok


def sha_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def env_fingerprint(env):
    return tuple((k, env.get(k)) for k in EXECUTION_AFFECTING)


def manifest_selectors(path):
    """(selected set or None for `all`, violations).

    v1 accumulated into a SET, so `select 1 / select 1 / select 2-3` normalized
    to {1,2,3} and passed the guard - a manifest the EXECUTOR itself refuses as
    overlapping (director). Duplicates, overlaps and descending order are now
    detected here, in the same author order the executor checks them.
    """
    sel = set()
    ranges = []
    bad = []
    for lineno, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.split("#", 1)[0].strip()
        if not line.startswith("select"):
            continue
        parts = line.split(None, 1)
        arg = parts[1].strip() if len(parts) > 1 else ""
        if arg == "all":
            return None, bad
        try:
            if "-" in arg:
                lo_s, hi_s = arg.split("-", 1)
                lo, hi = int(lo_s), int(hi_s)
            elif arg:
                lo = hi = int(arg)
            else:
                bad.append("line %d: empty selector" % lineno)
                continue
        except ValueError:
            bad.append("line %d: unparsable selector %r" % (lineno, arg))
            continue
        if lo > hi:
            bad.append("line %d: inverted range %d-%d" % (lineno, lo, hi))
            continue
        if ranges and lo <= ranges[-1][1]:
            bad.append("line %d: selector %d-%d overlaps or repeats the previous "
                       "%d-%d" % (lineno, lo, hi, ranges[-1][0], ranges[-1][1]))
        ranges.append((lo, hi))
        for o in range(lo, hi + 1):
            if o in sel:
                bad.append("line %d: ordinal %d selected more than once"
                           % (lineno, o))
            sel.add(o)
    return sel, bad


def selector_violations(path):
    return manifest_selectors(path)[1]


def manifest_pin(path):
    """(traceSha, admittedDraws) as the MANIFEST ITSELF declares them."""
    sha, adm = None, None
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if line.startswith("trace-sha256"):
            sha = line.split()[1]
        elif line.startswith("admitted-draws"):
            adm = int(line.split()[1])
    return sha, adm


def guard(probe, treat):
    """Refuse the launch unless probe and treatment describe the same run.

    Returns NAMED mismatches; empty means safe to launch.

    v1 compared `probe["manifestTraceSha"]`, which the treatment side never
    supplied - so a manifest pinning the wrong trace was silently accepted and
    the fixture that claimed to test it proved nothing (director). The pin is now
    read from the TREATMENT's own manifest and compared to the trace actually
    being replayed.
    """
    bad = []
    if probe["exeSha"] != treat["exeSha"]:
        bad.append("executable hash differs")
    if probe["traceSha"] != treat["traceSha"]:
        bad.append("trace hash differs")
    if treat["manifestTraceSha"] != treat["traceSha"]:
        bad.append("manifest pins a different trace than the one being replayed")
    if probe["admitted"] != treat["manifestAdmitted"]:
        bad.append("manifest admitted-draws (%s) != probed count (%s)"
                   % (treat["manifestAdmitted"], probe["admitted"]))
    if probe["env"] != treat["env"]:
        diff = [k for (k, a), (_, b) in zip(probe["env"], treat["env"]) if a != b]
        bad.append("execution-affecting environment differs: " + ",".join(diff))
    if not treat["ordinals"]:
        bad.append("the treatment selects no ordinals")
    # THE MANIFEST FILE IS THE TREATMENT. Compare what it actually selects
    # against what was intended, so a fabricated or stale ordinals list cannot
    # pass while the file on disk says something else.
    for v in treat.get("manifestSelectorViolations", []):
        bad.append("manifest selector list is malformed - " + v)
    sel = treat.get("manifestSelectors", "unset")
    if sel == "unset":
        bad.append("the manifest's selector set was not parsed")
    elif sel is None:
        bad.append("the manifest selects `all`, which is not a bucket treatment")
    elif set(treat["ordinals"]) != sel:
        only_t = sorted(set(treat["ordinals"]) - sel)[:4]
        only_m = sorted(sel - set(treat["ordinals"]))[:4]
        bad.append("manifest selector set differs from the intended ordinals "
                   "(intended-only %s, manifest-only %s)" % (only_t, only_m))
    return bad


def self_test():
    print("SELF-TEST (guard fixtures; no replay)")
    base_probe = {"exeSha": "a" * 64, "traceSha": "b" * 64, "admitted": 6073,
                  "env": env_fingerprint({})}
    base_treat = {"exeSha": "a" * 64, "traceSha": "b" * 64,
                  "manifestTraceSha": "b" * 64, "manifestAdmitted": 6073,
                  "env": env_fingerprint({}), "ordinals": [1, 2, 3],
                  "manifestSelectors": {1, 2, 3}}

    def t(**kw):
        d = dict(base_treat)
        d.update(kw)
        return d

    cases = [
        (base_probe, t(), False, "matching probe and treatment launches"),
        (base_probe, t(exeSha="c" * 64), True, "a different EXECUTABLE hash refuses"),
        (base_probe, t(traceSha="c" * 64, manifestTraceSha="c" * 64), True,
         "a different TRACE hash refuses"),
        (base_probe, t(manifestTraceSha="c" * 64), True,
         "a manifest pinning a trace OTHER than the replayed one refuses - v1 "
         "accepted this because guard() never read the field the fixture set"),
        (base_probe, t(manifestAdmitted=10), True,
         "a manifest admitted-count that disagrees with the probe refuses"),
        (base_probe, t(env=env_fingerprint({"SR_ALLOW_FORCING": "1"})), True,
         "a changed execution-affecting variable refuses"),
        (base_probe, t(ordinals=[]), True, "an empty treatment refuses"),
        (base_probe, t(ordinals=[999999]), True,
         "a FABRICATED ordinals list that the manifest does not select refuses - "
         "v1 accepted exactly this"),
        (base_probe, t(manifestSelectors={1, 2}), True,
         "a manifest selecting a SUBSET of the intended ordinals refuses"),
        (base_probe, t(manifestSelectorViolations=["ordinal 1 selected twice"]),
         True,
         "a manifest with DUPLICATE selectors refuses - v1 normalized them into "
         "a set and reported no violation"),
        (base_probe, t(manifestSelectorViolations=["1-3 overlaps 2-4"]), True,
         "a manifest with OVERLAPPING ranges refuses"),
        (base_probe, t(manifestSelectors=None), True,
         "a manifest selecting `all` is not a bucket treatment and refuses"),
        (base_probe, {k: v for k, v in t().items()
                      if k != "manifestSelectors"}, True,
         "an unparsed selector set refuses rather than defaulting to trust"),
    ]
    for p, tr, expect_bad, desc in cases:
        check(bool(guard(p, tr)) == expect_bad, desc)
    total = npass + nfail
    print("\n%d/%d guard fixtures behave as specified" % (npass, total))
    return 0 if nfail == 0 else 1



# ── THE INTEGRATION FIXTURE (director, iter-457, post-incident) ─────────────
# Drives the PRODUCTION orchestrate() with an INJECTED FAKE launcher. It never
# touches the real executable, never runs a replay, and needs NO authorization
# bypass - injecting the launcher is exactly the thing the `--rehearse` flag was
# a bad substitute for. The 13 guard fixtures only exercised guard() with
# preconstructed violation arrays; these exercise the real parser and the real
# baseline/revalidation/treatment sequence.
FRAME_SRC = "archive/iter457_q/base"
# The present the fixtures GRADE at. Injected, never a production default:
# production derives it and today refuses. 3 is the present scanout_route
# derives for record 6020, so the fixtures rehearse a realistic target
# rather than the retired constant 10.
FAKE_TARGET = 3


def synth_srht(path, admit_path, rexec_path, psamp_path):
    """A REAL, minimal SRHT trace whose route is known by construction.

    v1 wrote 8 literal bytes (b"trace-v1"), which walk() rejects outright --
    so the entry-path fixtures could never exercise derive_target() at all.
    This writes a valid header and records with ONE routable chain:

        rec0 SetRenderTarget RT0 <- 0xAAAA
        rec1 SetRenderState   colour mask 0xF
        rec2..4 draws         (admit ordinals 1,2,3 -- the manifest set)
        rec5..7 presents      front 0x1111 (presents 0,1,2: NOT the sink)
        rec8 Resolve          0xAAAA -> FRONT, and the sidecar says it RAN
        rec9 Present          front FRONT  -> present 3 == FAKE_TARGET

    So the derived target is 3, and it is derived rather than asserted."""
    import struct
    FRONT = 0x0A94E000
    recs, payloads = [], []

    def rec(t, *d, **kw):
        recs.append((t, tuple(list(d) + [0] * (7 - len(d)))))
        payloads.append(kw.get("payload", b""))

    st = [0] * 59
    st[4] = 0xF                       # RB_COLOR_MASK: RT0 writes
    rec(3, 0, 0, 0xAAAA)              # SetRenderTarget, index 0
    rec(19, payload=struct.pack(">59I", *st))
    rec(10); rec(10); rec(10)         # the three selected draws
    rec(0, 0, 0, 0, 0x1111)
    rec(0, 0, 0, 0, 0x1111)
    rec(0, 0, 0, 0, 0x1111)
    rec(20, 0xAAAA, FRONT, 0, 0, 0, 0, 0)
    rec(0, 0, 0, 0, FRONT)

    body = b""
    for (t, d), pay in zip(recs, payloads):
        body += struct.pack("<8I", t, *d)
        body += struct.pack("<IIQ", len(pay), 0, 0) + pay
    with open(path, "wb") as f:
        f.write(struct.pack("<4sIQQII", b"SRHT", 1, len(recs),
                            sum(len(p) for p in payloads), 4, 0))
        f.write(body)

    with open(admit_path, "w", newline="\n") as f:
        f.write("# rec ord outcome\n")
        for i, o in ((2, 1), (3, 2), (4, 3)):
            f.write("%d %d admitted\n" % (i, o))
    # The resolve at rec8 EXECUTED. Stating it explicitly is the point:
    # without this line the router refuses, which is its own fixture.
    open(rexec_path, "w", newline="\n").write("8 1\n")
    # No consumer draws in this chain, so the map is empty but PRESENT --
    # absent and empty must not mean the same thing.
    open(psamp_path, "w", newline="\n").write("")


class FakeLauncher:
    """Records every invocation; a treatment is one carrying SR_DRAW_MANIFEST."""

    def __init__(self, rc=0, frames=True, invariants=True, tally_ok=True,
                 on_baseline=None, corrupt_pinned=False, drop_one=False,
                 treatment_changes=(3,)):
        self.rc, self.frames = rc, frames
        # Separated so wrong-frame-CONTENT and wrong-frame-COUNT are exercised
        # INDEPENDENTLY. `frames=False` trips both at once, which left both the
        # pinned-frame and frame-count checks unexercised (director).
        self.corrupt_pinned, self.drop_one = corrupt_pinned, drop_one
        self.invariants, self.tally_ok = invariants, tally_ok
        # Which presents the TREATMENT pass perturbs. v1 copied identical
        # frames for both passes, so a treatment that changed nothing was
        # indistinguishable from one that worked -- exactly what the
        # all-frame comparison exists to catch, and it could not be
        # exercised at all. () models a treatment that did not bite.
        self.treatment_changes = tuple(treatment_changes)
        self.on_baseline = on_baseline      # fires AFTER a successful baseline
        self.calls = []

    @property
    def treatments(self):
        return [c for c in self.calls if c["manifest"]]

    def __call__(self, exe, env, outdir=None):
        man = env.get("SR_DRAW_MANIFEST")
        # A PROBE is a manifest with NO selectors: production refuses it, so it
        # must exit NONZERO and report totalSubmitted. Emulating that matters -
        # probe_admitted() calls sys.exit() on a zero rc, which would kill the
        # whole fixture process rather than fail a single assertion.
        if man and not (manifest_selectors(man)[0] or set()):
            self.calls.append({"exe": exe, "outdir": outdir, "manifest": None,
                               "probe": True, "env": dict(env)})
            return 2, ("[DRAWMAN] REFUSED manifest: empty-selection - "
                       "suppressing ZERO draws\n"
                       "[DRAWMAN] INVALID: manifest refused; suppressed=0 "
                       "(must be 0) totalSubmitted=6073\n")
        is_treatment = bool(man)
        self.calls.append({"exe": exe, "outdir": outdir,
                           "manifest": env.get("SR_DRAW_MANIFEST"),
                           "env": dict(env)})
        if outdir:
            shutil.rmtree(outdir, ignore_errors=True)
            os.makedirs(outdir, exist_ok=True)
            if self.frames:
                names = [f for f in sorted(os.listdir(FRAME_SRC))
                         if f.endswith(".bmp")]
                if self.drop_one:
                    names = [f for f in names
                             if not f.startswith("present_cl_000")]
                for f in names:
                    dst = os.path.join(outdir, f)
                    shutil.copyfile(os.path.join(FRAME_SRC, f), dst)
                    perturb = self.corrupt_pinned and f.startswith(
                        "present_cl_%03d" % BASELINE_IDENTITY_PRESENT)
                    if is_treatment:
                        for pr in self.treatment_changes:
                            if f.startswith("present_cl_%03d" % pr):
                                perturb = True
                    if perturb:
                        with open(dst, "r+b") as h:      # flip one payload byte
                            h.seek(64)
                            b = h.read(1)
                            h.seek(64)
                            h.write(bytes([b[0] ^ 0xFF]))
        recs = EXPECTED_RECORDS if self.invariants else 1
        log = ("[HLE_REPLAY] executed %d records / 1 payload bytes / %d presents "
               "| hashMismatches=0\n" % (recs, PRESENTS))
        if is_treatment:
            n = len(manifest_selectors(env["SR_DRAW_MANIFEST"])[0] or [])
            k = n if self.tally_ok else n + 1
            log += ("[DRAWMAN] TALLY requested=%d matched=%d suppressed=%d "
                    "totalSubmitted=6073 -> %s\n"
                    % (n, k, k, "EXACT" if self.tally_ok else "INVALID"))
        elif self.on_baseline:
            self.on_baseline()
        return self.rc, log


def integration_test():
    """Every assertion the director required, with zero real replays."""
    global npass, nfail, TRACE
    import tempfile

    if not os.path.isdir(FRAME_SRC):
        print("FAIL: %s missing - cannot run the integration fixture" % FRAME_SRC)
        return 1

    tmp = tempfile.mkdtemp(prefix="sweepint_")
    fake_exe = os.path.join(tmp, "fake.exe")
    fake_trace = os.path.join(tmp, "fake.srht")
    open(fake_exe, "wb").write(b"exe-v1")
    fake_admit = os.path.join(tmp, "admit.txt")
    fake_rexec = os.path.join(tmp, "rexec.txt")
    fake_psamp = os.path.join(tmp, "psamp.txt")
    synth_srht(fake_trace, fake_admit, fake_rexec, fake_psamp)
    real_trace = TRACE
    TRACE = fake_trace                      # orchestrate() re-hashes this

    ords = [1, 2, 3]

    def make_manifest(path, body=None):
        open(path, "w", newline="\n").write(
            body if body is not None else
            ("trace-sha256 %s\nadmitted-draws 6073\nselect 1-3\n"
             % sha_file(fake_trace)))
        return path

    def make_ctx(man):
        mts, mad = manifest_pin(man)
        msel, mviol = manifest_selectors(man)
        env = {"SR_HLE_TRACE_REPLAY": fake_trace, "SR_ALLOW_FORCING": "0"}
        return {"exe": fake_exe, "env": env, "man": man, "ords": ords,
                "bucket": "int", "base_dir": os.path.join(tmp, "b"),
                "treat_dir": os.path.join(tmp, "t"),
                "probe": {"exeSha": sha_file(fake_exe),
                          "traceSha": sha_file(fake_trace),
                          "admitted": 6073, "env": env_fingerprint(env)},
                "exeSha": sha_file(fake_exe), "traceSha": sha_file(fake_trace),
                "mts": mts, "mad": mad, "msel": msel, "mviol": mviol,
                "admitted": 6073,
                "trace": fake_trace, "admit": fake_admit,
                "resolveExec": fake_rexec, "psSamplers": fake_psamp}

    def run(fake, man=None, mutate=None):
        """One orchestration under the fake; returns (exit, treatment count)."""
        global npass, nfail
        man = man or make_manifest(os.path.join(tmp, "m.manifest"))
        ctx = make_ctx(man)
        if mutate:
            fake.on_baseline = lambda: mutate(ctx, man)
        saved = (npass, nfail)
        npass = nfail = 0
        rc = orchestrate(ctx, fake, target_present=FAKE_TARGET)
        npass, nfail = saved
        return rc, len(fake.treatments)

    results = []

    def ck(ok, what):
        global npass, nfail
        if ok:
            npass += 1
            print("  PASS  " + what)
        else:
            nfail += 1
            print("  FAIL  " + what)

    print("INTEGRATION FIXTURE (fake launcher; no real replay, no bypass)")

    # H-I2: the literal duplicate/overlap manifest TEXT, through the real parser
    dup = make_manifest(os.path.join(tmp, "dup.manifest"),
                        "trace-sha256 %s\nadmitted-draws 6073\n"
                        "select 1\nselect 1\nselect 2-3\n" % sha_file(fake_trace))
    sel, viol = manifest_selectors(dup)
    ck(len(viol) >= 2 and any("more than once" in v for v in viol)
       and any("overlaps" in v for v in viol),
       "H-I2: the literal duplicate/overlap manifest yields BOTH named "
       "violations from the real parser (%d found)" % len(viol))
    g = guard(make_ctx(dup)["probe"],
              {"exeSha": sha_file(fake_exe), "traceSha": sha_file(fake_trace),
               "manifestTraceSha": sha_file(fake_trace), "manifestAdmitted": 6073,
               "env": env_fingerprint({"SR_HLE_TRACE_REPLAY": fake_trace,
                                       "SR_ALLOW_FORCING": "0"}),
               "ordinals": ords, "manifestSelectors": sel,
               "manifestSelectorViolations": viol})
    ck(bool(g), "H-I2: and the guard REFUSES it before any launch")

    # the clean control
    f = FakeLauncher()
    rc, n = run(f)
    ck(rc == 0 and n == 1,
       "clean control: exit 0 and the fake treatment is invoked EXACTLY ONCE "
       "(rc=%d, treatments=%d)" % (rc, n))

    # ── THE ALL-FRAME COMPARISON, WITH TEETH ───────────────────────────
    # The iter-457 defect was graded at ONE frame chosen by a constant, so
    # neither of these could be seen. Both are now failures, not prints.

    # KILLER: a treatment that changes NOTHING anywhere did not bite, and
    # its "0 of 5 ROIs changed" is a broken instrument, not a null result.
    f = FakeLauncher(treatment_changes=())
    rc, n = run(f)
    ck(rc != 0,
       "KILLER a treatment that changes NO frame FAILS -- a null ROI result from an inert treatment is not evidence (rc=%d)" % rc)

    # KILLER: a change UPSTREAM of the derived present falsifies the route.
    # If suppressing this draw alters a frame BEFORE the one we derived, the
    # derivation is wrong and the grade at the derived frame is meaningless.
    f = FakeLauncher(treatment_changes=(1,))
    rc, n = run(f)
    ck(rc != 0,
       "KILLER a treatment that changes a frame BEFORE the derived present FAILS -- it falsifies the route rather than being graded by it (rc=%d)" % rc)

    # ...and a change at the derived present AND after it is fine: a product
    # that reaches the screen normally persists into later frames.
    f = FakeLauncher(treatment_changes=(3, 5))
    rc, n = run(f)
    ck(rc == 0,
       "a treatment changing the derived present AND later frames passes -- downstream persistence is expected, not a violation (rc=%d)" % rc)
    if f.treatments:
        t = f.treatments[0]
        ck(t["manifest"] == os.path.join(tmp, "m.manifest"),
           "the treatment carries the FINAL manifest path")
        ck(manifest_selectors(t["manifest"])[0] == set(ords),
           "the treatment's manifest selects EXACTLY the intended ordinals")
        ck(t["env"].get("SR_DRAW_MANIFEST") == t["manifest"],
           "SR_DRAW_MANIFEST is set in the treatment environment")

    # each failed baseline prerequisite -> ZERO treatments
    for desc, kw in (("nonzero replay rc", {"rc": 3}),
                     ("missing frames", {"frames": False}),
                     ("bad execution invariants", {"invariants": False})):
        f = FakeLauncher(**kw)
        rc, n = run(f)
        ck(rc == 8 and n == 0,
           "H-I1: baseline failure (%s) aborts with 8 and invokes ZERO "
           "treatments (rc=%d, treatments=%d)" % (desc, rc, n))

    # wrong frame CONTENT with the RIGHT count - isolates the pinned-frame check
    f = FakeLauncher(corrupt_pinned=True)
    rc, n = run(f)
    ck(rc == 8 and n == 0,
       "H-I1: correct frame COUNT but corrupted pinned-frame CONTENT aborts "
       "with 8 and invokes ZERO treatments (rc=%d, treatments=%d)" % (rc, n))

    # wrong frame COUNT with valid content - isolates the frame-count check
    f = FakeLauncher(drop_one=True)
    rc, n = run(f)
    ck(rc == 8 and n == 0,
       "H-I1: valid frame CONTENT but one frame missing aborts with 8 and "
       "invokes ZERO treatments (rc=%d, treatments=%d)" % (rc, n))

    # post-baseline mutations -> exit 9 and ZERO treatments
    def mutate_exe(ctx, man):
        open(fake_exe, "wb").write(b"exe-v2-TAMPERED")

    def mutate_trace(ctx, man):
        open(fake_trace, "wb").write(b"trace-v2-TAMPERED")

    def mutate_manifest(ctx, man):
        open(man, "a", newline="\n").write("select 9\n")

    for desc, mut, restore in (
            ("executable", mutate_exe, lambda: open(fake_exe, "wb").write(b"exe-v1")),
            # ⚠ RESTORE THE SYNTHETIC TRACE, not the old 8-byte dummy. Writing
            # b"trace-v1" back left every LATER fixture with an unparsable
            # trace, so the entry path could not derive a target at all.
            ("trace", mutate_trace,
             lambda: synth_srht(fake_trace, fake_admit, fake_rexec, fake_psamp)),
            ("manifest", mutate_manifest, lambda: None)):
        f = FakeLauncher()
        rc, n = run(f, man=make_manifest(os.path.join(tmp, "m_%s.manifest" % desc)),
                    mutate=mut)
        ck(rc == 9 and n == 0,
           "H-I1: %s mutated AFTER a successful baseline -> exit 9 and ZERO "
           "treatments (rc=%d, treatments=%d)" % (desc, rc, n))
        restore()

    # A fixture that ISOLATES the selector-set comparison. My own mutation run
    # showed the manifest-mutation case was ALSO caught by the "selector set
    # unchanged" revalidation check, so disabling the comparison changed
    # nothing and the check was not independently exercised. Here the manifest
    # selects a SUPERSET of the intended ordinals from the start, so the
    # "unchanged" check passes and only the comparison can refuse it.
    wide = make_manifest(os.path.join(tmp, "wide.manifest"),
                         "trace-sha256 %s\nadmitted-draws 6073\n"
                         "select 1-3\nselect 9\n" % sha_file(fake_trace))
    f = FakeLauncher()
    rc, n = run(f, man=wide)
    ck(rc == 9 and n == 0,
       "H-I1: a manifest selecting a SUPERSET of the intended ordinals is "
       "refused by the selector-set comparison alone -> exit 9, ZERO "
       "treatments (rc=%d, treatments=%d)" % (rc, n))

    # THE ENTRY PATH. The defect above (undefined base_dir/treat_dir) survived
    # because every fixture called orchestrate() directly. This drives main()
    # itself with a fake launcher, a fake exe and a fake trace, so the wiring
    # between the two is exercised rather than assumed. Nothing real is invoked.
    entry_fake = FakeLauncher()
    real_replay = globals()["replay"]
    tsv = os.path.join(tmp, "buckets.tsv")
    with open(tsv, "w", newline="\n") as fh:
        fh.write("ordinal\tbucket\tclass\tcohort\ttraceRecord\toutcome\t"
                 "vertBytes\tvertSha256\tcolorMask\tdepthControl\n")
        for o in ords:
            fh.write("%d\t7\tadmitted\ts15_0b5be000\t%d\tadmitted\t4\tx\t"
                     "0000000f\t00000000\n" % (o, o))
    globals()["replay"] = entry_fake
    try:
        argv = sys.argv
        sys.argv = ["sweep_driver.py", "--bucket", "7", "--buckets-tsv", tsv,
                    "--exe", fake_exe, "--work", os.path.join(tmp, "w"),
                    "--admit", fake_admit,
                    # The entry path must DERIVE its target like production
                    # does; handing it the fixture's sidecars is what makes
                    # that derivation real rather than skipped.
                    "--resolve-exec", fake_rexec,
                    "--ps-samplers", fake_psamp,
                    "--i-have-authorization", "INTEGRATION-FIXTURE-NOT-A-RULING"]
        try:
            entry_rc = main()
        except NameError as e:
            entry_rc = "NameError: %s" % e

        finally:
            sys.argv = argv

        # ── KILLER: NO SIDECARS => REFUSE BEFORE LAUNCH ────────────────
        # The most safety-critical behaviour in the file, and the only
        # fixture that makes it load-bearing: without it, a derive_target
        # that quietly falls back to a CONSTANT present -- the exact
        # iter-457 defect -- passes the whole suite untouched (it did).
        nosc_fake = FakeLauncher()
        globals()["replay"] = nosc_fake
        saved2 = sys.argv
        sys.argv = ["sweep_driver.py", "--bucket", "7", "--buckets-tsv",
                    tsv, "--exe", fake_exe,
                    "--work", os.path.join(tmp, "w2"),
                    "--admit", fake_admit,
                    "--i-have-authorization",
                    "INTEGRATION-FIXTURE-NOT-A-RULING"]
        try:
            nosc_rc = main()
        except Exception as e:
            nosc_rc = "%s: %s" % (type(e).__name__, e)
        sys.argv = saved2
        globals()["replay"] = entry_fake
    finally:
        globals()["replay"] = real_replay
    ck(entry_rc == 0,
       "ENTRY PATH: main() runs end to end under a fake launcher without a "
       "NameError (got %r) - this is the defect the direct-orchestrate fixtures "
       "could not see" % (entry_rc,))
    ck(len(entry_fake.treatments) == 1,
       "ENTRY PATH: exactly ONE fake treatment invoked through main() (got %d)"
       % len(entry_fake.treatments))

    ck(nosc_rc == 3,
       "KILLER ENTRY PATH: with NO sidecars main() REFUSES before launch and returns 3 (got %r)" % (nosc_rc,))
    ck(len(nosc_fake.treatments) == 0,
       "KILLER ENTRY PATH: a refused derivation launches ZERO treatments, so no authorization is consumed (got %d)" % len(nosc_fake.treatments))

    # ASSERT THE EXACT OUTPUT DIRECTORIES. Restoring the definitions was not
    # enough: the fixture accepted a wrong baseline path, a wrong treatment path
    # and the two SWAPPED, all at 34/34 - the same defect class it was written to
    # catch (director). The fake records every outdir, so compare them.
    entry_work = os.path.join(tmp, "w")
    want_base = os.path.join(entry_work, "baseline")
    want_treat = os.path.join(entry_work, "bucket_7_frames")
    base_calls = [c for c in entry_fake.calls
                  if not c["manifest"] and not c.get("probe")]
    ck(len(base_calls) == 1 and base_calls[0]["outdir"] == want_base,
       "ENTRY PATH: the BASELINE outdir is exactly %r (got %r)"
       % (want_base, base_calls[0]["outdir"] if base_calls else None))
    ck(entry_fake.treatments and
       entry_fake.treatments[0]["outdir"] == want_treat,
       "ENTRY PATH: the TREATMENT outdir is exactly %r (got %r)"
       % (want_treat, entry_fake.treatments[0]["outdir"]
          if entry_fake.treatments else None))
    ck(base_calls and entry_fake.treatments and
       base_calls[0]["outdir"] != entry_fake.treatments[0]["outdir"],
       "ENTRY PATH: baseline and treatment write to DIFFERENT directories, so "
       "the treatment cannot overwrite the frames it is compared against")

    TRACE = real_trace
    shutil.rmtree(tmp, ignore_errors=True)
    total = npass + nfail
    print("\n%d/%d integration assertions hold" % (npass, total))
    return 0 if nfail == 0 else 1


def replay(exe, env, outdir=None):
    e = dict(env)
    if outdir:
        shutil.rmtree(outdir, ignore_errors=True)
        os.makedirs(outdir, exist_ok=True)
        e["SR_PRESENT_DUMP"] = "1"
        e["SR_PRESENT_DUMP_EVERY"] = "1"
        e["SR_PRESENT_DUMP_CAP"] = "32"
        e["SR_PRESENT_DUMP_DIR"] = outdir
    p = subprocess.run([os.path.abspath(exe)], env=e, capture_output=True,
                       text=True, errors="replace", timeout=1800)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def probe_admitted(exe, env, man_dir):
    """A deliberately REFUSED manifest: suppresses ZERO, still reports the count."""
    man = os.path.join(man_dir, "probe.manifest")
    open(man, "w", newline="\n").write(
        "trace-sha256 %s\nadmitted-draws 1\n" % sha_file(TRACE))   # no selector
    e = dict(env)
    e["SR_DRAW_MANIFEST"] = man
    rc, log = replay(exe, e)
    if rc == 0:
        sys.exit("probe exited 0; a deliberately refused manifest MUST exit "
                 "nonzero - refusing to trust its count")
    m = re.search(r"suppressed=(\d+) \(must be 0\) totalSubmitted=(\d+)", log)
    if not m:
        sys.exit("probe did not report totalSubmitted; refusing to continue")
    if int(m.group(1)) != 0:
        sys.exit("probe suppressed %s draws; it must suppress ZERO" % m.group(1))
    return int(m.group(2))


def bucket_rows(tsv, bucket):
    by = defaultdict(list)
    with open(tsv, encoding="utf-8") as f:
        head = next(f).rstrip("\n").split("\t")
        ci = {n: i for i, n in enumerate(head)}
        for line in f:
            p = line.rstrip("\n").split("\t")
            by[int(p[ci["bucket"]])].append(
                {"ordinal": int(p[ci["ordinal"]]), "class": p[ci["class"]],
                 "cohort": p[ci["cohort"]]})
    return by.get(bucket, [])


def to_ranges(ords):
    out = []
    for o in sorted(ords):
        if out and o == out[-1][1] + 1:
            out[-1][1] = o
        else:
            out.append([o, o])
    return [(a, b) for a, b in out]


def roi_hashes(frame):
    im = Image.open(frame).convert("RGB")
    out = {}
    for k, (x0, y0, x1, y1) in ROIS.items():
        crop = im.crop((x0, y0, x1 + 1, y1 + 1))
        out[k] = (hashlib.sha256(crop.tobytes()).hexdigest(),
                  tuple(int(v) for v in ImageStat.Stat(crop).mean))
    return out


EXPECTED_RECORDS = 52466


def execution_invariants(log, expect_presents=PRESENTS):
    """(ok, message) for the replay's own executed/presents/hash line."""
    m = re.search(r"executed (\d+) records .*?/ (\d+) presents \| "
                  r"hashMismatches=(\d+)", log)
    if not m:
        return False, "the replay did not report its executed/presents/hash line"
    recs, pres, mism = int(m.group(1)), int(m.group(2)), int(m.group(3))
    ok = (recs == EXPECTED_RECORDS and pres == expect_presents and mism == 0)
    return ok, ("executed=%d/%d presents=%d/%d hashMismatches=%d"
                % (recs, EXPECTED_RECORDS, pres, expect_presents, mism))


def frame_at(d, present):
    for f in sorted(os.listdir(d)):
        if f.startswith("present_cl_%03d" % present) and f.endswith(".bmp"):
            return os.path.join(d, f)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--integration-test", action="store_true")
    ap.add_argument("--bucket", type=int, default=None)
    ap.add_argument("--buckets-tsv", default="archive/iter457_q/buckets.tsv")
    ap.add_argument("--exe", default="build/SupermanReturns.exe")
    ap.add_argument("--work", default="archive/iter457_sweep")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--require-cohort", default="s15_0b5be000",
                    help="refuse a bucket outside this cohort; '-' disables")
    # ⛔ NO DEFAULTS. These sidecars do not exist yet; defaulting either to a
    # path or to a permissive value is how a treatment gets graded at a
    # frame it never reached.
    ap.add_argument("--resolve-exec", default=None)
    ap.add_argument("--ps-samplers", default=None)
    ap.add_argument("--admit", default="archive/iter457_q/admit.txt",
                    help="admit map; supplies ordinal->record for routing")
    ap.add_argument("--i-have-authorization", default=None)
    a = ap.parse_args()

    if a.self_test:
        return self_test()
    if a.integration_test:
        return integration_test()
    if a.bucket is None:
        print(__doc__)
        return 2
    if not a.dry_run and not a.i_have_authorization:
        print("REFUSED: a treatment is an attribution experiment and is opened by "
              "the director.\nRe-run with --dry-run to prove the guards, or pass "
              "--i-have-authorization <ruling> once the gate is open.")
        return 3

    rows = bucket_rows(a.buckets_tsv, a.bucket)
    if not rows:
        print("REFUSED: bucket %d has no members in %s" % (a.bucket, a.buckets_tsv))
        return 4
    nonrender = [r for r in rows if r["class"] != "admitted"]
    if nonrender:
        print("REFUSED: bucket %d contains %d ordinals that rendered NOTHING"
              % (a.bucket, len(nonrender)))
        return 5
    cohorts = {r["cohort"] for r in rows}
    if a.require_cohort != "-" and cohorts != {a.require_cohort}:
        print("REFUSED: bucket %d cohort is %s, required %s"
              % (a.bucket, sorted(cohorts), a.require_cohort))
        return 7
    ords = [r["ordinal"] for r in rows]

    os.makedirs(a.work, exist_ok=True)
    env = dict(os.environ)
    env["SR_HLE_TRACE_REPLAY"] = TRACE
    env["SR_HLE_TRACE_LOOPS"] = "1"
    env["SR_ALLOW_FORCING"] = "0"
    env.setdefault("SR_MODS", "0")  # iter-463: never mount the player's mods (docs/MODS.md)
    env.setdefault("SR_DEVTOOLS", "0")  # iter-463 phase 2: nor enable a dev tool (docs/DEVTOOLS.md)
    env.pop("SR_DRAW_MANIFEST", None)

    trace_sha, exe_sha = sha_file(TRACE), sha_file(a.exe)
    print("bucket %d: %d ordinals, cohort=%s -> %d range(s)"
          % (a.bucket, len(ords), sorted(cohorts), len(to_ranges(ords))))
    print("exe   sha256 %s\ntrace sha256 %s" % (exe_sha, trace_sha))

    print("\nFRESH-PROCESS PROBE (separate process, suppresses ZERO)")
    admitted = probe_admitted(a.exe, env, a.work)
    print("  probed admitted-draw count = %d" % admitted)

    man = os.path.join(a.work, "bucket_%d.manifest" % a.bucket)
    with open(man, "w", newline="\n") as f:
        f.write("# iter-457 treatment: candidate bucket %d (cohort %s)\n"
                % (a.bucket, sorted(cohorts)))
        f.write("trace-sha256 %s\nadmitted-draws %d\n" % (trace_sha, admitted))
        for lo, hi in to_ranges(ords):
            f.write("select %d\n" % lo if lo == hi else "select %d-%d\n" % (lo, hi))
    mts, mad = manifest_pin(man)
    msel, mviol = manifest_selectors(man)
    print("  wrote %s" % man)

    probe = {"exeSha": exe_sha, "traceSha": trace_sha, "admitted": admitted,
             "env": env_fingerprint(env)}
    treat = {"exeSha": exe_sha, "traceSha": trace_sha, "manifestTraceSha": mts,
             "manifestAdmitted": mad, "env": env_fingerprint(env),
             "ordinals": ords,
             "manifestSelectors": msel, "manifestSelectorViolations": mviol}

    print("\nPRE-LAUNCH GUARD")
    bad = guard(probe, treat)
    check(not bad, "probe and treatment describe the SAME run (%s)"
          % ("; ".join(bad) if bad else "exe, trace, manifest pin, count and "
             "execution-affecting environment all agree"))
    if bad:
        print("REFUSED - nothing launched, zero draws suppressed")
        return 6

    if a.dry_run:
        print("\nDRY RUN: every guard passed and NOTHING was launched.")
        return 0

    # ---- THE TREATMENT ----------------------------------------------------
    # ⚠ These were deleted by the orchestrate() refactor, so the production
    # entry path passed the guard and then raised NameError - the authorized
    # path would have CRASHED. The integration fixture missed it because it
    # calls orchestrate() directly instead of main() (director, iter-457).
    base_dir = os.path.join(a.work, "baseline")
    treat_dir = os.path.join(a.work, "bucket_%d_frames" % a.bucket)
    print("\nauthorization: %s" % a.i_have_authorization)
    return orchestrate({
        "exe": a.exe, "env": env, "man": man, "ords": ords, "bucket": a.bucket,
        "base_dir": base_dir, "treat_dir": treat_dir, "probe": probe,
        "exeSha": exe_sha, "traceSha": trace_sha, "mts": mts, "mad": mad,
        "msel": msel, "mviol": mviol, "admitted": admitted,
        # Needed by derive_target(); absent in v1, so the production entry
        # path raised KeyError after passing every guard -- the same defect
        # class as the deleted base_dir/treat_dir above, caught the same way
        # (the integration fixture drives main(), not orchestrate()).
        "trace": TRACE, "admit": a.admit,
        "resolveExec": a.resolve_exec, "psSamplers": a.ps_samplers,
    }, launcher=replay)




def ordinals_to_records(admit_path, ords):
    """Manifest ordinals -> trace record numbers, via the admit map.

    Refuses on any ordinal the map does not carry: a silently dropped
    member would route a SUBSET and grade the whole manifest by it."""
    byord = {}
    with open(admit_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if len(p) >= 3 and p[-1] == "admitted":
                try:
                    byord[int(p[1])] = int(p[0])
                except ValueError:
                    pass
    missing = sorted(o for o in ords if o not in byord)
    if missing:
        return None, ("admit map has no record for ordinal(s) %s" % missing)
    return {byord[o] for o in ords}, ""


# iter-457: these lived here AND were absent from the router's own CLI,
# which is exactly how the two drifted. They now have ONE definition, in
# scanout_route, and this module re-exports it so both callers cannot
# disagree about what a sidecar line means.
from scanout_route import load_resolve_exec, load_ps_samplers  # noqa: E402


def derive_target(ctx):
    """The present this manifest may be graded at, or (None, why).

    ⛔ THE iter-457 DEFECT, STRUCTURALLY CLOSED. There is no constant here
    and no fallback: if the route cannot be derived the treatment does not
    run at all. Today scanout_route REFUSES for want of two sidecars it
    names, so this refuses too -- which is the correct outcome, not a bug."""
    import scanout_route as sr
    recs, why = ordinals_to_records(ctx["admit"], ctx["ords"])
    if recs is None:
        return None, why
    # Sidecars are INJECTED the same way the launcher is. Production
    # passes no paths, so these stay None and the router refuses by name;
    # the fixtures pass real ones, so the derivation itself is exercised
    # rather than shipped untested behind a refusal.
    p, per = sr.route_many(lambda: sr.walk(ctx["trace"]), recs,
                           admitted=sr.load_admit(ctx["admit"]),
                           resolve_exec=load_resolve_exec(
                               ctx.get("resolveExec")),
                           ps_samplers=load_ps_samplers(
                               ctx.get("psSamplers")))
    if isinstance(p, sr.Refusal):
        return None, repr(p)
    return p, ""


def orchestrate(ctx, launcher, target_present=None):
    """The production baseline -> revalidate -> treatment sequence.

    `launcher(exe, env, outdir) -> (rc, log)` is INJECTED so an integration
    fixture can drive this exact code with a fake and count treatment
    invocations, without the real executable and WITHOUT any authorization
    bypass. The bypass I added for that purpose is what caused the iter-457
    incident; injecting the launcher needs no gate exception at all.

    Returns an exit code. The treatment is launched only if the baseline AND the
    post-baseline revalidation both pass.
    """
    global npass, nfail
    exe, env, man, ords = ctx["exe"], ctx["env"], ctx["man"], ctx["ords"]
    base_dir, treat_dir = ctx["base_dir"], ctx["treat_dir"]

    # ── THE TARGET IS DERIVED BEFORE ANYTHING LAUNCHES ────────────────
    # A treatment that cannot be graded must never run: the run itself is
    # what consumes the authorization, and iter-457 spent one producing a
    # number that meant nothing. Tests inject the target; production
    # derives it and, today, refuses.
    if target_present is None:
        target_present, why = derive_target(ctx)
        if target_present is None:
            print("\nREFUSED before launch: %s" % why)
            print("Nothing was launched; no authorization was consumed.")
            return 3
    print("\ngrading present: %d (DERIVED, not pinned)" % target_present)

    print("\nBASELINE (no treatment)")
    rc, log = launcher(exe, env, base_dir)
    bframe = frame_at(base_dir, BASELINE_IDENTITY_PRESENT)
    nframes = len([f for f in os.listdir(base_dir) if f.endswith(".bmp")]) \
        if os.path.isdir(base_dir) else 0
    inv_ok, inv_msg = execution_invariants(log)
    check(rc == 0, "baseline replay exits 0")
    check(bframe is not None and sha_file(bframe) == PINNED_SHA,
          "baseline pinned frame is byte-identical to the frozen baseline")
    check(nframes == PRESENTS, "baseline presents %d frames (got %d)"
          % (PRESENTS, nframes))
    check(inv_ok, "baseline execution invariants: " + inv_msg)
    if nfail:
        print("\nABORTED: the baseline failed %d check(s); no treatment was "
              "launched and no draw was suppressed." % nfail)
        return 8

    # REVALIDATE IMMEDIATELY BEFORE THE TREATMENT. A full baseline replay ran
    # between the pre-launch guard and this point, so the manifest, executable
    # and trace are re-read and the guard re-run here.
    print("\nREVALIDATION (immediately before treatment)")
    re_exe, re_trace = sha_file(exe), sha_file(TRACE)
    re_mts, re_mad = manifest_pin(man)
    re_msel, re_mviol = manifest_selectors(man)
    check(re_exe == ctx["exeSha"],
          "the executable is unchanged since the pre-baseline guard")
    check(re_trace == ctx["traceSha"],
          "the trace is unchanged since the pre-baseline guard")
    check((re_mts, re_mad) == (ctx["mts"], ctx["mad"]),
          "the manifest's pin and admitted count are unchanged")
    check(re_msel == ctx["msel"] and not re_mviol,
          "the manifest's selector set is unchanged and still well-formed")
    re_bad = guard(ctx["probe"],
                   {"exeSha": re_exe, "traceSha": re_trace,
                    "manifestTraceSha": re_mts, "manifestAdmitted": re_mad,
                    "env": env_fingerprint(env), "ordinals": ords,
                    "manifestSelectors": re_msel,
                    "manifestSelectorViolations": re_mviol})
    check(not re_bad, "the pre-launch guard still passes (%s)"
          % ("; ".join(re_bad) if re_bad else "revalidated"))
    if nfail:
        print("\nABORTED: revalidation failed; no treatment was launched and no "
              "draw was suppressed.")
        return 9

    print("\nTREATMENT (manifest applied)")
    tenv = dict(env)
    tenv["SR_DRAW_MANIFEST"] = man
    rc, log = launcher(exe, tenv, treat_dir)
    check(rc == 0, "treatment replay exits 0 (rc=%d)" % rc)
    m = re.search(r"\[DRAWMAN\] TALLY requested=(\d+) matched=(\d+) "
                  r"suppressed=(\d+) totalSubmitted=(\d+) -> (\w+)", log)
    check(m is not None, "the treatment reported a tally")
    if m:
        req, mat, sup, tot, verdict = (int(m.group(1)), int(m.group(2)),
                                       int(m.group(3)), int(m.group(4)),
                                       m.group(5))
        check(req == mat == sup == len(ords) and verdict == "EXACT",
              "tally is EXACT: requested=matched=suppressed=%d" % len(ords))
        check(tot == ctx["admitted"],
              "totalSubmitted %d == the probed admitted count %d"
              % (tot, ctx["admitted"]))
    tinv_ok, tinv_msg = execution_invariants(log)
    check(tinv_ok, "treatment execution invariants: " + tinv_msg)
    tframes = [f for f in os.listdir(treat_dir) if f.endswith(".bmp")] \
        if os.path.isdir(treat_dir) else []
    check(len(tframes) == PRESENTS,
          "treatment still presents %d frames (got %d)" % (PRESENTS, len(tframes)))

    # ── MANDATORY: EVERY frame compared, not just the graded one ───────
    # A treatment that changes frames OTHER than the derived one means the
    # route is wrong; a treatment that changes NOTHING anywhere means the
    # suppression did not bite. Neither is visible from one frame.
    diffs = []
    for i in range(PRESENTS):
        bf, tf = frame_at(base_dir, i), frame_at(treat_dir, i)
        if bf and tf and sha_file(bf) != sha_file(tf):
            diffs.append(i)
    print("\nALL-FRAME COMPARISON (%d presents)" % PRESENTS)
    print("  frames differing baseline vs treatment: %s"
          % (diffs if diffs else "NONE"))
    check(diffs != [], "the treatment changes at least one frame (a "
          "treatment that changes nothing did not bite)")
    check(all(d >= target_present for d in diffs),
          "no frame BEFORE the derived present %d changes (a change "
          "upstream of the route falsifies the route)" % target_present)

    tframe = frame_at(treat_dir, target_present)
    check(tframe is not None, "treatment produced the graded frame")
    bframe = frame_at(base_dir, target_present)
    if bframe and tframe:
        bh, th = roi_hashes(bframe), roi_hashes(tframe)
        print("\nROI ATTRIBUTION (DERIVED present %d)" % target_present)
        print("  %-14s %-9s %-18s %-18s %s"
              % ("roi", "verdict", "baseline sha[:16]", "treatment sha[:16]",
                 "means"))
        changed = []
        for k in ROIS:
            same = bh[k][0] == th[k][0]
            if not same:
                changed.append(k)
            print("  %-14s %-9s %-18s %-18s %s -> %s"
                  % (k, "UNCHANGED" if same else "CHANGED",
                     bh[k][0][:16], th[k][0][:16], bh[k][1], th[k][1]))
        print("\nRESULT: bucket %s changes %d of %d frozen ROIs%s"
              % (ctx["bucket"], len(changed), len(ROIS),
                 (": " + ", ".join(changed)) if changed else ""))

    total = npass + nfail
    print("\n%d/%d treatment checks passed" % (npass, total))
    return 0 if nfail == 0 else 1



if __name__ == "__main__":
    sys.exit(main())


