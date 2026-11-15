"""qualify_draw_manifest.py - iter-457 gate 4: fail-closed qualification replays.

Runs the director's pre-sweep qualification list against the real executor, via
offline SRHT replay. Nothing here is a sweep result; it only establishes that the
suppression instrument is HONEST before any attribution is attempted.

  Q1 treatment absent      -> 16 presents, and the pinned frame is byte-identical
                              to the frozen pre-instrument baseline
  Q2 invalid manifests     -> nonzero exit AND zero draws suppressed, every time
  Q3 singleton + multirange-> requested == matched == suppressed exactly
  Q4 suppress-all          -> still executes every record, validates every payload
                              hash, reaches 16 presents, and CHANGES at least one
                              frozen target ROI

    python tools/qualify_draw_manifest.py [--exe build/SupermanReturns.exe]

Exit code 0 only if every check passes. Read-only with respect to the trace.
"""
import argparse, hashlib, os, re, shutil, subprocess, sys, tempfile
from PIL import Image

TRACE = "archive/iter456_title_fresh.srht"
PINNED_FRAME = "archive/iter456_title_fresh_dump/present_cl_010_p000010_1280x720.bmp"
PINNED_SHA = "c677fc5e15e9152cb7ae9b10ad5d1f0040886f48d43d0ce91a9d118d28040943"
PRESENTS = 16
RECORDS = 52466

# the director's corrected ROI set (tools/roi_freeze.py is the authority)
ROIS = {
    "facade_left":   (0,    70,  175, 340),
    "facade_right":  (1020, 80, 1279, 620),
    "sky":           (340,  52,  680,  89),
    "spikes":        (176,  20,  330, 160),
    "superman_cape": (880, 360,  930, 405),
}

npass, nfail = 0, 0


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
    return hashlib.sha256(open(p, "rb").read()).hexdigest()


def roi_hashes(frame):
    im = Image.open(frame).convert("RGB")
    return {k: hashlib.sha256(im.crop((x0, y0, x1 + 1, y1 + 1)).tobytes()).hexdigest()
            for k, (x0, y0, x1, y1) in ROIS.items()}


def replay(exe, outdir, manifest=None, dump=True):
    """One offline replay pass. Returns (exitcode, log text)."""
    if dump:
        shutil.rmtree(outdir, ignore_errors=True)
        os.makedirs(outdir, exist_ok=True)
    env = dict(os.environ)
    env["SR_HLE_TRACE_REPLAY"] = TRACE
    env["SR_HLE_TRACE_LOOPS"] = "1"
    env.setdefault("SR_MODS", "0")  # iter-463: never mount the player's mods (docs/MODS.md)
    env.setdefault("SR_DEVTOOLS", "0")  # iter-463 phase 2: nor enable a dev tool (docs/DEVTOOLS.md)
    if dump:
        env["SR_PRESENT_DUMP"] = "1"
        env["SR_PRESENT_DUMP_EVERY"] = "1"
        env["SR_PRESENT_DUMP_CAP"] = "32"
        env["SR_PRESENT_DUMP_DIR"] = outdir
    if manifest:
        env["SR_DRAW_MANIFEST"] = manifest
    p = subprocess.run([os.path.abspath(exe)], env=env, capture_output=True, text=True,
                       errors="replace", timeout=900)
    return p.returncode, (p.stdout or "") + (p.stderr or "")



def manifest_admitted(path):
    """The admitted-draws value a manifest pins, or None."""
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if line.startswith("admitted-draws"):
            try:
                return int(line.split()[1])
            except (IndexError, ValueError):
                return None
    return None


def guarded_replay(exe, manifest, admitted, outdir, dump=False):
    """Refuse to LAUNCH a treatment whose pinned admitted count is wrong.

    The replay can only detect a wrong count once it has finished, by which time
    it has already suppressed. Every sweep treatment therefore goes through this
    guard, which compares the pinned count against the separately probed one and
    declines to run at all on a mismatch - so a wrong manifest suppresses ZERO.

    Returns (launched, exitcode).
    """
    pinned = manifest_admitted(manifest)
    if pinned is None or pinned != admitted:
        print("    GUARD: refusing to launch %s (pins %s, probe observed %d)"
              % (os.path.basename(manifest), pinned, admitted))
        return False, 4
    rc, _log = replay(exe, outdir, manifest, dump=dump)
    return True, rc


def tally(log):
    m = re.search(r"\[DRAWMAN\] TALLY requested=(\d+) matched=(\d+) "
                  r"suppressed=(\d+) totalSubmitted=(\d+) -> (\w+)", log)
    return tuple(int(x) for x in m.groups()[:4]) + (m.group(5),) if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default="build/SupermanReturns.exe")
    ap.add_argument("--work", default="archive/iter457_q")
    a = ap.parse_args()

    for p in (TRACE, PINNED_FRAME, a.exe):
        if not os.path.exists(p):
            print("missing required input: " + p)
            return 1

    trace_sha = sha_file(TRACE)
    print("trace  : %s\n  sha256 %s" % (TRACE, trace_sha))
    os.makedirs(a.work, exist_ok=True)
    mandir = os.path.join(a.work, "manifests")
    os.makedirs(mandir, exist_ok=True)

    def write_manifest(name, body):
        p = os.path.join(mandir, name)
        open(p, "w", newline="\n").write(body)
        return p

    # ---- Q1: treatment ABSENT is inert -------------------------------------
    print("\nQ1 treatment absent")
    base_dir = os.path.join(a.work, "base")
    rc, log = replay(a.exe, base_dir)
    frames = sorted(f for f in os.listdir(base_dir) if f.endswith(".bmp"))
    check(rc == 0, "no-treatment replay exits 0")
    check(len(frames) == PRESENTS, "no-treatment replay presents %d frames (got %d)"
          % (PRESENTS, len(frames)))
    f10 = [f for f in frames if f.startswith("present_cl_010")]
    check(bool(f10) and sha_file(os.path.join(base_dir, f10[0])) == PINNED_SHA,
          "the pinned frame is BYTE-IDENTICAL to the frozen pre-instrument baseline")
    check("[DRAWMAN]" not in log,
          "no manifest supplied means the instrument stays silent")
    base_rois = roi_hashes(os.path.join(base_dir, f10[0])) if f10 else {}

    # discover the real admitted-draw count rather than assuming it: the
    # replay's own draws= counter is NOT it (6260 vs 6073), which is exactly the
    # divergence the tally exists to catch.
    # The admitted count is discovered by a DELIBERATELY REFUSED manifest: it
    # suppresses ZERO draws and the refusal path still reports totalSubmitted.
    # It cannot be assumed - the replay's own draws= counter says 6260 while only
    # 6073 draws reach hleSubmitDraw.
    probe_man = write_manifest("probe.manifest",
        "trace-sha256 %s\nadmitted-draws 1\n" % trace_sha)   # no selector
    prc, plog = replay(a.exe, os.path.join(a.work, "probe"), probe_man, dump=False)
    pm = re.search(r"suppressed=(\d+) \(must be 0\) totalSubmitted=(\d+)", plog)
    if not pm:
        print("  FAIL  the refused probe did not report totalSubmitted")
        return 1
    check(prc != 0 and int(pm.group(1)) == 0,
          "the probe is REFUSED, suppresses ZERO, and still reports the count")
    admitted = int(pm.group(2))
    print("  admitted draws (observed totalSubmitted) = %d" % admitted)

    # ---- Q2: invalid manifests refuse, and suppress ZERO --------------------
    print("\nQ2 invalid manifests fail closed")
    head = "trace-sha256 %s\nadmitted-draws %d\n" % (trace_sha, admitted)
    bad = {
        "empty.manifest":      head,
        "malformed.manifest":  head + "selct 5\n",
        "overlap.manifest":    head + "select 0-9\nselect 5-14\n",
        "descending.manifest": head + "select 20-29\nselect 0-9\n",
        "oob.manifest":        head + "select %d\n" % admitted,
        "wrongtrace.manifest": ("trace-sha256 " + "0" * 64 +
                                "\nadmitted-draws %d\nselect 0\n" % admitted),
        "badsha.manifest":     "trace-sha256 abc\nadmitted-draws 10\nselect 0\n",
    }
    for name, body in bad.items():
        mp = write_manifest(name, body)
        rc, log = replay(a.exe, os.path.join(a.work, "bad"), mp, dump=False)
        supp = re.search(r"suppressed=(\d+)", log)
        suppressed = int(supp.group(1)) if supp else -1
        refused = "[DRAWMAN] REFUSED" in log or "[DRAWMAN] INVALID" in log
        # wrongtrace parses cleanly; it is caught by the trace-sha comparison,
        # which is reported separately below.
        if name == "wrongtrace.manifest":
            check(rc != 0 and "trace mismatch" in log and suppressed == 0,
                  "wrongtrace.manifest      refuses (rc=%d) and suppresses ZERO "
                  "- the pinned sha is compared against the replayed trace" % rc)
            continue
        check(rc != 0 and refused and suppressed == 0,
              "%-22s refuses (rc=%d) and suppresses ZERO" % (name, rc))

    # ---- Q2b: a WRONG pinned admitted count ---------------------------------
    # The director proved AdmittedMismatch was unreachable: a manifest pinning
    # 100000000 suppressed ordinal 0, reported 1=1=1 and exited 0.
    #
    # HONEST SCOPE. The observed count is only knowable once a pass has FINISHED,
    # so a single replay cannot refuse before it has already suppressed. An
    # in-process counting pass was tried and REJECTED as unsound: replay pass 2
    # does not reproduce pass 1 (measured 0 of 16 frames identical), so a
    # treatment pass run after a counting pass is not comparable to the baseline.
    # The rule is therefore enforced in two places, and both are asserted:
    #   * the REPLAY exits nonzero on a wrong pinned count (the rule is real);
    #   * the DRIVER probes in a SEPARATE clean process and refuses to launch the
    #     treatment at all, so zero draws are suppressed.
    print("\nQ2b wrong pinned admitted-draw count")
    for label, count in (("low", 10), ("high", 100000000)):
        mp = write_manifest("admitted_%s.manifest" % label,
                            "trace-sha256 %s\nadmitted-draws %d\nselect 0\n"
                            % (trace_sha, count))
        # (a) the replay itself must refuse
        rc, log = replay(a.exe, os.path.join(a.work, "adm"), mp, dump=False)
        check(rc != 0 and "pins admitted-draws" in log,
              "admitted-draws %-4s (%d): the REPLAY exits nonzero (rc=%d)"
              % (label, count, rc))
        # (b) the driver guard must refuse to launch it at all
        launched, guard_rc = guarded_replay(a.exe, mp, admitted,
                                            os.path.join(a.work, "adm"))
        check((not launched) and guard_rc != 0,
              "admitted-draws %-4s (%d): the DRIVER GUARD refuses to launch, so "
              "ZERO draws are suppressed" % (label, count))

    # ---- Q3: exact treatments ----------------------------------------------
    print("\nQ3 requested == matched == suppressed")
    single = write_manifest("single.manifest", head + "select 0\n")
    rc, log = replay(a.exe, os.path.join(a.work, "single"), single, dump=False)
    t = tally(log)
    check(rc == 0 and t and t[0] == t[1] == t[2] == 1 and t[4] == "EXACT",
          "singleton: requested=matched=suppressed=1 and the replay exits 0 (%s)" % (t,))

    multi = write_manifest("multi.manifest",
                           head + "select 0-9\nselect 100-149\nselect 1000\n")
    rc, log = replay(a.exe, os.path.join(a.work, "multi"), multi, dump=False)
    t = tally(log)
    check(rc == 0 and t and t[0] == t[1] == t[2] == 61 and t[4] == "EXACT",
          "multi-range: requested=matched=suppressed=61 and exits 0 (%s)" % (t,))

    # ---- Q4: suppress-all is exact AND perturbs a frozen ROI ---------------
    print("\nQ4 suppress-all")
    alldir = os.path.join(a.work, "all")
    allman = write_manifest("all.manifest", head + "select all\n")
    rc, log = replay(a.exe, alldir, allman)
    t = tally(log)
    check(rc == 0 and t and t[0] == t[1] == t[2] == admitted and t[4] == "EXACT",
          "suppress-all: requested=matched=suppressed=%d and exits 0 (%s)"
          % (admitted, t))
    m = re.search(r"executed (\d+) records .*? / (\d+) presents \| hashMismatches=(\d+)", log)
    check(bool(m) and int(m.group(1)) == RECORDS and int(m.group(2)) == PRESENTS
          and int(m.group(3)) == 0,
          "suppress-all still executes %d records, reaches %d presents, "
          "hashMismatches=0" % (RECORDS, PRESENTS))
    aframes = sorted(f for f in os.listdir(alldir) if f.endswith(".bmp"))
    check(len(aframes) == PRESENTS,
          "suppress-all still presents %d frames (got %d)" % (PRESENTS, len(aframes)))
    af10 = [f for f in aframes if f.startswith("present_cl_010")]
    if af10 and base_rois:
        arois = roi_hashes(os.path.join(alldir, af10[0]))
        changed = [k for k in ROIS if arois[k] != base_rois[k]]
        check(len(changed) >= 1,
              "suppress-all CHANGES at least one frozen target ROI (changed: %s)"
              % (", ".join(changed) if changed else "NONE"))
    else:
        check(False, "suppress-all produced the pinned frame for ROI comparison")

    total = npass + nfail
    print("\n%d/%d qualification checks passed" % (npass, total))
    return 0 if nfail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
