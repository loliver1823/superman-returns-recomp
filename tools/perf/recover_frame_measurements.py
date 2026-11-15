#!/usr/bin/env python3
"""recover_frame_measurements.py - rebuild the world-rule measurements of frames the frame-scratch GC deleted.

The GC in tools/sr_artifacts.ps1 deleted the raw BMPs of archived rolls (from 2026-10-13 18:27), leaving
frames_deleted.txt in each attempt folder, so tools/perf/world_floor.py graded those rolls "world 0 / unmeasured":
missing input, not a rendering failure. world_floor's measurement cache (%LOCALAPPDATA%\\sr_native_ledger_frames.json,
keys "center|<normcased path>|<size>|<mtime_ns>" and "sides|...") still holds the measurements the ledger and the
gate took while the BMPs existed. This tool writes them into each such attempt's frames_measured.json (the sidecar
world_floor reads when a BMP is absent; format in world_floor's docstring) - only where the cache entry provably is
THAT file, and it never invents a measurement:

  1. THE FIXED INVENTORY (the frames the folder held; world_floor's invariant): outcome.json's frame list (the
     driver's end-of-run listing, with sizes) on disk, plus off disk every frame only run.log names. Its byte total
     must equal the one total frames_deleted.txt records; a surplus counts only as a 'residual' non-BMP file when it
     is smaller than the smallest BMP and run.log names the file it wrote into frames/ (RUN_CLASS.json). Every
     inventory frame is in the sidecar, recovered or not (recovered=false, with the reason), never omitted. A
     sidecar that does not reconcile (no outcome.json, a byte mismatch) is still written and reported NOT
     RECONCILED: the reader rejects it and the frames stay UNMEASURED.
  2. A frame's candidate paths: <attempt>/frames/<name> (the cache key's normcased absolute form), the path its
     own [PRESENT_DUMP] line names (the roll's original location, if it moved), and <the scratch folder named in
     frames_deleted.txt>/<name>.
  3. FILE-VERSION IDENTITY. A cache entry is the frame's measurement only if its path is a candidate, its size
     equals the frame's size (outcome.json, when known), the run logged that file name exactly ONCE (a second
     [PRESENT_DUMP] line means the file was rewritten in the run - an overwrite the cache may never have seen), and
     the entry's mtime lies within WINDOW_MS of that one line's timestamp. WINDOW_MS = 50: the measured spread is
     far inside it (2026-10-14: mtime - log time in [-2.0, +0.9] ms over the 1,192 recovered measurements; the
     median over 256,449 BMPs still on disk is 0.0 ms, p1 -1.0, p99 +0.8), so the entry is the write that line
     logged, not an earlier version of the file. A re-run in place replaces run.log and is minutes away.
  4. Exactly one file version (size, mtime_ns) passing over center AND sides together -> recovered, with its keys
     recorded; none -> unrecovered, with the reason; two or more versions anywhere (even one only a sides entry
     carries) -> the whole frame unrecovered (ambiguous: neither measurement is used). No [PRESENT_DUMP] line for a
     frame -> unrecovered (nothing binds an entry). Cache values that are not finite pairs in 0..255 / 0..1 are
     never evidence.

    python tools/perf/recover_frame_measurements.py ROOT [ROOT ...]          # dry run: the per-roll coverage table
    python tools/perf/recover_frame_measurements.py --write ROOT [ROOT ...]  # + write each attempt's sidecar
      --cache PATH      the cache to recover from (default: world_floor's - SR_WORLD_FLOOR_CACHE or %LOCALAPPDATA%)
      --force           rewrite a sidecar an earlier recovery wrote (one measured from BMPs is never overwritten)
      --json OUT        the coverage rows as JSON

Each ROOT is searched for frames_deleted.txt; the attempt folder is its folder. Nothing else in the archive is
written. Run tools/native_ledger.py afterwards to see what the recovered measurements grade.
"""
import argparse
import datetime
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import world_floor as wf  # noqa: E402

DELETED = "frames_deleted.txt"
WINDOW_MS = 50
TOOL = "tools/perf/recover_frame_measurements.py"
DUMP_LINE = re.compile(wf.TS + r".*" + wf.DUMP_NAME.pattern, re.M)   # the reader's [PRESENT_DUMP] name, timestamped


def norm(p):
    return os.path.normcase(os.path.abspath(p))


def load_cache(path):
    """{(what, normcased path): [(size, mtime_ns, [luma, dark], key)]} of a world_floor cache file."""
    with open(path, encoding="utf-8") as f:
        raw = json.load(f)
    if wf.cache_version(raw) != wf.MEASURE_VERSION:   # measured under another definition: no evidence for this rule
        raise SystemExit("cache %s holds measure_version %r measurements; this rule measures version %d" % (
            path, wf.cache_version(raw), wf.MEASURE_VERSION))
    idx = {}
    for key, val in raw.items():
        parts = key.split("|")
        if len(parts) != 4 or parts[0] not in ("center", "sides"):
            continue
        try:
            size, mt = int(parts[2]), int(parts[3])
        except ValueError:
            continue
        if not wf.valid_measure(val):   # NaN / out of range / malformed: never evidence
            continue
        idx.setdefault((parts[0], parts[1]), []).append((size, mt, list(val), key))
    return idx, len(raw)


def dump_lines(txt):
    """{frame name: [(timestamp, logged path), ...] in log order} from run.log's [PRESENT_DUMP] lines, and the
    number of lines. A name with more than one line was rewritten in the run."""
    out, n = {}, 0
    for m in DUMP_LINE.finditer(txt):
        n += 1
        out.setdefault(re.split(r"[\\/]", m.group(2))[-1], []).append((m.group(1), m.group(2)))
    return out, n


def match_entry(entries, size, logged_ms, window_ms=WINDOW_MS, prefer_path=None):
    """The cache entry that provably is this frame's file version, or None. entries: [(size, mtime_ns, value, key)]
    over all of its candidate paths; size: the frame's size (None = unknown); logged_ms: the epoch ms of EVERY
    [PRESENT_DUMP] line naming the file in this run (it must be exactly one). -> (entry, None) or (None, why)."""
    if not logged_ms:
        return None, "no [PRESENT_DUMP] line names it: no timestamp binds a cache entry to this file"
    if len(logged_ms) > 1:
        return None, "logged %d times in this run: the file was rewritten, and no entry is provably the last " \
            "version" % len(logged_ms)
    logged_ms = logged_ms[0]
    if not entries:
        return None, "no cache entry for its path"
    sized = [e for e in entries if size is None or e[0] == size]
    if not sized:
        return None, "the cache entries for its path have another size (%s, the frame is %d)" % (
            sorted({e[0] for e in entries}), size)
    near = [e for e in sized if abs(e[1] / 1e6 - logged_ms) <= window_ms]
    if not near:
        d = min(sized, key=lambda e: abs(e[1] / 1e6 - logged_ms))
        return None, "%d cache entr%s for its path, none written within %g ms of its [PRESENT_DUMP] line " \
            "(nearest %+.1f ms)" % (len(sized), "y" if len(sized) == 1 else "ies", window_ms, d[1] / 1e6 - logged_ms)
    versions = sorted({(e[0], e[1]) for e in near})
    if len(versions) > 1:
        return None, "ambiguous: %d file versions within %g ms of its [PRESENT_DUMP] line %s" % (
            len(versions), window_ms, versions)
    if len({tuple(e[2]) for e in near}) > 1:
        return None, "ambiguous: one file version, measured differently under %d keys" % len(near)
    own = [e for e in near if prefer_path and e[3].split("|")[1] == prefer_path]
    return (own or near)[0], None


def window_versions(entries, size, logged_ms, window_ms=WINDOW_MS):
    """Every file version (size, mtime_ns) among entries that a single logged write could bind (size-matched, within
    window_ms of it) - used ACROSS center and sides: a frame is recovered only when, over both measurement types,
    at most one version qualifies."""
    if not logged_ms or len(logged_ms) != 1:
        return set()
    return {(e[0], e[1]) for e in entries if (size is None or e[0] == size) and
            abs(e[1] / 1e6 - logged_ms[0]) <= window_ms}


FOLDER_FILE = re.compile(r"[\\/]frames[\\/]([^\\/\s\"']+)")   # a file the run.log says it wrote into frames/


def inventory(attempt, note_bytes, logged, log_txt):
    """The FIXED INVENTORY of the folder the GC deleted (world_floor.sidecar_doc_problem's invariant) -> (frames
    [(name, size or None, on_disk)], other_files {name: {...}}, a line saying how it is known).
    outcome.json's BMP list (the driver's end-of-run listing, with sizes) is the inventory, on disk; frames only
    run.log names are added off disk (unmeasurable). The GC's byte total counts every file, so a surplus is non-BMP
    files: it is recorded as a 'residual' other file only when it is smaller than the smallest listed BMP AND run.log
    names the non-BMP file(s) it wrote into the frames folder (the exe's RUN_CLASS.json); otherwise it is left
    unexplained and the sidecar will not reconcile. Without a usable outcome.json the inventory cannot be
    established: the log's frames are written off disk, unreconciled - the reader rejects it, and they stay
    UNMEASURED."""
    out = wf.outcome_frames(attempt)
    if out is None:
        frames = [(n, None, False) for n in sorted(logged)]
        return frames, {}, "no usable outcome.json: the inventory cannot be established (%d frames named by run.log, " \
            "off disk, unreconciled)" % len(frames)
    frames = [(n, s, True) for n, s in sorted(out.items())]
    frames += [(n, None, False) for n in sorted(set(logged) - set(out))]
    ob = sum(out.values())
    head = "outcome.json: %d frames, %s bytes" % (len(out), format(ob, ","))
    if len(frames) > len(out):
        head += " (+%d named only by run.log, off disk)" % (len(frames) - len(out))
    if note_bytes is None:
        return frames, {}, head + "; frames_deleted.txt records no byte total - UNRECONCILED"
    extra = note_bytes - ob
    if extra == 0:
        return frames, {}, head + " = frames_deleted.txt"
    named = sorted({n for n in FOLDER_FILE.findall(log_txt) if not n.lower().endswith(".bmp")})
    if 0 < extra and out and extra < min(out.values()) and named:
        others = {" | ".join(named): {"size": extra, "how": "residual",
                                      "evidence": "frames_deleted.txt total minus outcome.json's BMPs; run.log names "
                                                  "%s written into the frames folder" % ", ".join(named)}}
        return frames, others, head + " = frames_deleted.txt less %d bytes of %s (named by run.log; under one " \
            "frame)" % (extra, ", ".join(named))
    return frames, {}, head + " - frames_deleted.txt records %s bytes: UNRECONCILED (%+d bytes unexplained)" % (
        format(note_bytes, ","), extra)


def recover_attempt(attempt, idx, cache_meta, window_ms=WINDOW_MS):
    """One attempt with frames_deleted.txt -> (its sidecar doc: every inventory frame, recovered or not; the
    coverage row). Writes nothing."""
    attempt = os.path.abspath(attempt)
    fdir = os.path.join(attempt, "frames")
    row = {"attempt": attempt}
    _, note_bytes, note = wf.deletion_record(attempt)   # the reader's own parse: one note, or no total
    notes = wf.DELETED_NOTE.findall(note or "")
    scratch = notes[0][0] if len(notes) == 1 else None
    log = os.path.join(attempt, "run.log")
    log_txt = wf.read_text(log) if os.path.exists(log) else ""
    logged, nlines = dump_lines(log_txt)
    inv, others, inv_line = inventory(attempt, note_bytes, logged, log_txt)
    row.update(inventory=inv_line, frames=len(inv), dump_lines=nlines, deleted_bytes=note_bytes)
    own_dir = norm(fdir)
    frames = {}
    cov = {"logged": 0, "center": 0, "sides": 0, "unrecovered": 0}
    for name, size, on_disk in inv:
        lg = logged.get(name) or []
        cands = [norm(os.path.join(fdir, name))]
        if lg:
            cov["logged"] += 1
            cands += [norm(p) for _, p in lg]
        if scratch:
            cands.append(norm(os.path.join(scratch, name)))
        cands = list(dict.fromkeys(cands))
        logged_ms = [wf.ts_ms(t) for t, _ in lg]
        ent = {"on_disk": on_disk, "logged": [t for t, _ in lg], "size": size, "mtime_ns": None, "center": None,
               "sides": None, "source": "cache", "recovered": False, "keys": {"center": None, "sides": None},
               "missing": {}}
        got = {}
        pools = {what: [e for c in cands for e in idx.get((what, c), [])] for what in ("center", "sides")}
        versions = window_versions(pools["center"] + pools["sides"], size, logged_ms, window_ms)
        if len(versions) > 1:
            # Astra 2026-10-14: uniqueness over BOTH measurement types first - any second candidate version (even one
            # that only a sides entry has) leaves no measurement of this frame provably its own; use neither
            why = "ambiguous: %d file versions (center and sides together) within %g ms of its [PRESENT_DUMP] " \
                  "line %s" % (len(versions), window_ms, sorted(versions))
            ent["missing"] = {"center": why, "sides": why}
        else:
            for what in ("center", "sides"):
                e, why = match_entry(pools[what], size, logged_ms, window_ms,
                                     prefer_path=own_dir + os.sep + os.path.normcase(name))
                if e is None:
                    ent["missing"][what] = why
                else:
                    got[what] = e
        if "center" in got and "sides" in got and got["center"][:2] != got["sides"][:2]:
            # cannot happen after the joint check above; kept as a guard - two versions: use neither
            why = "its center and sides entries are different file versions (%s vs %s)" % (
                list(got["center"][:2]), list(got["sides"][:2]))
            got = {}
            ent["missing"] = {"center": why, "sides": why}
        for what, e in got.items():
            ent[what] = e[2]
            ent["keys"][what] = e[3]
            ent["size"], ent["mtime_ns"] = e[0], e[1]
            cov[what] += 1
        ent["recovered"] = "center" in got
        if not ent["recovered"]:
            cov["unrecovered"] += 1
        if not ent["missing"]:
            del ent["missing"]
        frames[name] = ent
    row.update(cov)
    prov = {"from": "the world_floor measurement cache, matched frame by frame (%s docstring)" % TOOL,
            "measured_by": "world_floor.measure (frame_scene / side_scene, measure_version %d) on the machine that "
                           "graded the roll while its BMPs existed; the cache records no Pillow version" % wf.MEASURE_VERSION,
            "cache": cache_meta, "frames_deleted": note, "scratch_path": scratch, "inventory": inv_line,
            "match": {"candidate_dirs": sorted({own_dir} | {norm(os.path.dirname(p)) for v in logged.values()
                                                            for _, p in v} | ({norm(scratch)} if scratch else set())),
                      "window_ms": window_ms,
                      "binding": "size = the inventory's size; the file logged by exactly one [PRESENT_DUMP] line in "
                                 "the run; mtime within window_ms of it; exactly one file version; center and sides "
                                 "the same version, else neither"},
            "coverage": dict(cov, frames=len(inv))}
    doc = wf.sidecar_header(fdir, "recovered", TOOL, prov)
    bmp, other, total = wf.inventory_totals(frames, others)
    doc["inventory"] = {"source": "outcome.json's frame list (the driver's end-of-run listing) + frames named only "
                                  "by run.log (off disk); reconciled against frames_deleted.txt",
                        "count": len(frames), "on_disk": sum(1 for e in frames.values() if e["on_disk"]),
                        "bmp_bytes": bmp, "other_files": others, "total_bytes": total}
    doc["frames"] = frames
    row["reconciled"] = wf.sidecar_doc_problem(doc, fdir) or "yes"
    return doc, row


def find_attempts(roots):
    for root in roots:
        for dp, dns, fns in os.walk(root):
            if DELETED in fns:
                yield os.path.abspath(dp)
            dns[:] = [d for d in dns if d != "frames"]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("roots", nargs="+")
    ap.add_argument("--cache", default=wf._cache_path())
    ap.add_argument("--write", action="store_true", help="write each attempt's frames_measured.json")
    ap.add_argument("--force", action="store_true", help="rewrite a sidecar an earlier recovery wrote")
    ap.add_argument("--window-ms", type=float, default=WINDOW_MS)
    ap.add_argument("--json", help="write the coverage rows here")
    a = ap.parse_args(argv)
    if not a.cache or not os.path.exists(a.cache):
        print("no cache to recover from: %s" % a.cache)
        return 2
    idx, n_entries = load_cache(a.cache)
    st = os.stat(a.cache)
    cache_meta = {"path": os.path.abspath(a.cache), "size": st.st_size, "mtime_ns": st.st_mtime_ns,
                  "mtime": datetime.datetime.fromtimestamp(st.st_mtime).isoformat(timespec="seconds"),
                  "entries": n_entries}
    rows = []
    for att in sorted(set(find_attempts(a.roots))):
        fdir = os.path.join(att, "frames")
        if wf._bmps(fdir):
            rows.append({"attempt": att, "skipped": "BMPs present: measure them (world_floor.py sidecar)"})
            continue
        sp = wf.sidecar_path(fdir)
        if os.path.exists(sp):
            try:
                with open(sp, encoding="utf-8") as f:
                    how = json.load(f).get("how")
            except (OSError, ValueError, AttributeError):
                how = "unreadable"
            if how != "recovered" or not a.force:
                rows.append({"attempt": att, "skipped": "%s exists (how=%s)%s" % (
                    wf.SIDECAR, how, "" if how != "recovered" else "; --force rewrites it")})
                continue
        doc, row = recover_attempt(att, idx, cache_meta, a.window_ms)
        if not row["frames"]:
            row["note"] = "no sidecar: " + ("the GC deleted 0 bytes (no frame was ever dumped)"
                                            if row["deleted_bytes"] == 0 else "no inventory of the deleted frames")
        elif a.write:
            row["sidecar"] = wf.write_sidecar(fdir, doc)
        rows.append(row)
    print("cache %s: %d entries | window %d ms | %s" % (a.cache, n_entries, a.window_ms,
                                                       "WRITING sidecars" if a.write else "dry run (--write writes)"))
    print("%6s %6s %6s %6s %6s  %s" % ("frames", "logged", "center", "sides", "missing", "attempt / inventory"))
    tot = {"frames": 0, "logged": 0, "center": 0, "sides": 0, "unrecovered": 0}
    for r in rows:
        if "skipped" in r:
            print("%6s %6s %6s %6s %6s  %s  SKIPPED: %s" % ("-", "-", "-", "-", "-", r["attempt"], r["skipped"]))
            continue
        for k in tot:
            tot[k] += r[k]
        rec = "" if r.get("note") else (" | reconciled" if r.get("reconciled") == "yes" else
                                        " | NOT RECONCILED (the reader rejects it: UNMEASURED): %s" % r.get("reconciled"))
        print("%6d %6d %6d %6d %6d  %s\n%35s%s%s%s" % (r["frames"], r["logged"], r["center"], r["sides"],
                                                     r["unrecovered"], r["attempt"], "", r["inventory"],
                                                     (" | " + r["note"]) if r.get("note") else "", rec))
    print("%6d %6d %6d %6d %6d  TOTAL over %d attempts; %d sidecars do not reconcile" % (
        tot["frames"], tot["logged"], tot["center"], tot["sides"], tot["unrecovered"],
        sum(1 for r in rows if "skipped" not in r),
        sum(1 for r in rows if r.get("frames") and r.get("reconciled") not in (None, "yes"))))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"cache": cache_meta, "window_ms": a.window_ms, "rows": rows}, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
