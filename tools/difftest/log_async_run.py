#!/usr/bin/env python3
"""log_async_run.py - run `difftest --log-async-test`, then mutation-validate it (app/log-async).

The suite (tools/difftest/log_async_test.cpp) proves the asynchronous log writer (app/log.cpp; docs/TOOLKIT.md
3.2): order, no loss, the full-queue wait and its accounting, the slow-disk case, the crash / abort / exit / mid-batch
drains, flush(), the player profile and same bytes as the d71946e2 logger. Each mutant below is one deliberate bug
in that code; the suite must FAIL on every one. The runner edits the sources IN PLACE, rebuilds the difftest target,
runs the suite and restores the files (always, byte for byte), so it belongs in a scratch checkout - the cloud
worker's tree - and refuses to run without --in-place.

    python tools/difftest/log_async_run.py --build build --out <dir> --in-place [--only M1,M3]
    python tools/difftest/log_async_run.py --build build --out <dir> --clean-only     # no mutation

Writes <out>/clean.txt, <out>/<mutant>.txt and <out>/summary.json. Exit 0 when the clean run passes and every
mutant fails it; 1 when a mutant survives or the clean run fails; 2 for an input error (a mutation anchor that
no longer matches exactly once, a build that fails).
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
LOG = "app/log.cpp"

# name -> ([(file, original text, mutated text), ...], what must catch it). '\n' matches the file's own line ending.
MUTANTS = {
    "M1-order-per-thread-batching": ([(
        LOG,
        "  const int len = formatLineV(lineBuf, cat, sev, fmt, args);\n  if (len < 0) return;\n  emitLine(lineBuf, len);",
        "  const int len = formatLineV(lineBuf, cat, sev, fmt, args);\n  if (len < 0) return;\n"
        "  thread_local std::string t_batch;\n  thread_local int t_n = 0;\n  t_batch.append(lineBuf, size_t(len));\n"
        "  if (++t_n < 4) return;\n  emitLine(t_batch.data(), int(t_batch.size()));\n  t_batch.clear();\n  t_n = 0;")],
        "O1: a per-thread batch reorders lines across threads (and loses a partial batch)"),
    "M2-order-no-lock": ([(
        LOG,
        "static void logSpinAcquire() {\n  int rounds = 0;\n  while (g_logSpinLock.test_and_set(std::memory_order_acquire)) {",
        "static void logSpinAcquire() {\n  int rounds = 0;\n  while (false) {")],
        "S1: without the lock, concurrent producers tear and lose records"),
    "M3-loss-drop-when-full": ([(
        LOG,
        "    if (roomLocked(size_t(len), false)) break;\n",
        "    if (roomLocked(size_t(len), false)) break;\n    logSpinRelease();\n    return;\n")],
        "S1 / W1: a full queue must make the producer wait, never drop its line"),
    "M4-loss-no-wait-witness": ([(
        LOG,
        "    if (n > 0 && roomLocked(size_t(n), true))\n      putRecordLocked(w, size_t(n), true);\n    else\n",
        "    if (n > 0 && roomLocked(size_t(n), true))\n      (void)w;\n    else\n")],
        "S1 / W1: every producer wait leaves a [LOG_STALL] producer wait line (or is counted unwitnessed)"),
    "M5-accounting-wait-not-counted": ([(
        LOG,
        "    g_q.waits.fetch_add(1, std::memory_order_relaxed);\n",
        "")],
        "S1: the census's waits= equals the producer-wait lines in the file"),
    "M6-drain-crash-filter-no-flush": ([
        ("app/crash_handler_win.cpp", "  Log::flush(2000);\n\n  // Write minidump", "\n  // Write minidump"),
        ("app/crash_handler_win.cpp", "busy on another thread for 2 s\");\n  Log::flush(3000);\n",
         "busy on another thread for 2 s\");\n")],
        "C1: the crash filter drains the queue (a writer stall is pending at the fault)"),
    "M7-drain-no-tls-callback": ([(
        LOG,
        "  if (reason != DLL_PROCESS_DETACH) return;\n",
        "  return;\n")],
        "C3 / C4: std::_Exit leaves only the TLS process-detach drain"),
    "M8-drain-no-sigabrt-handler": ([(
        LOG,
        "    g_prevAbrt = std::signal(SIGABRT, &onSigAbrt);\n",
        "    g_prevAbrt = SIG_DFL;\n")],
        "C2: abort() drains only through the SIGABRT handler"),
    "M9-drain-resume-ignores-file-position": ([(
        LOG,
        "    done = uint64_t(cur.QuadPart) - g_q.inflightAt;\n",
        "    done = 0;\n")],
        "C4 / C5 / X1: the batch a dead writer had in flight resumes at the file position (no line written twice)"),
    "M10-flush-returns-at-once": ([(
        LOG,
        "  const uint64_t target = g_q.headPub.load(std::memory_order_seq_cst);\n"
        "  const uint64_t deadline = GetTickCount64() + timeoutMs;\n",
        "  if (timeoutMs != 0xFFFFFFFFu) return true;\n"
        "  const uint64_t target = g_q.headPub.load(std::memory_order_seq_cst);\n"
        "  const uint64_t deadline = GetTickCount64() + timeoutMs;\n")],
        "F1 / C1: flush() returns only once the lines are in the file"),
    "M11-bytes-no-crlf": ([(
        LOG,
        "      if (r[i] == '\\n') g_q.stage[st++] = '\\r';\n",
        "")],
        "B / census: the file keeps the CRT text-mode bytes (\\r\\n)"),
    "M12-writer-ignores-wakes": ([(
        LOG,
        "      WaitOnAddress(&g_q.headPub, &h, sizeof(h), kIdleWaitMs);\n",
        "      Sleep(500);\n")],
        "K1: an idle writer writes as soon as a line arrives (the Stop-Process loss window)"),
    "M13-producer-waits-for-the-disk": ([(
        LOG,
        "  logSpinRelease();\n  wakeWriter();\n}",
        "  logSpinRelease();\n  wakeWriter();\n  Log::flush(10000);\n}")],
        "D1: a producer below capacity never waits for the writer's disk write"),
    "M14-watchdog-writes-on-its-own-thread": ([(
        "kernel/gpu_hang_wd.cpp",
        "  std::thread([job] { writeDumpFiles(*job); }).detach();",
        "  writeDumpFiles(*job);")],
        "H1: the hang dump's files are written by the helper thread, never the watchdog's"),
    "M15-watchdog-minidump-default-on": ([(
        "kernel/gpu_hang_wd.cpp",
        "    const char* e = std::getenv(\"SR_GPU_HANG_DUMP\");\n    return e && e[0] == '1';",
        "    const char* e = std::getenv(\"SR_GPU_HANG_DUMP\");\n    return !e || e[0] != '0';")],
        "H1: no process-snapshot minidump without SR_GPU_HANG_DUMP=1"),
    # Astra round 1 (the emergency paths): one mutant per fix; each must fail the suite.
    "M16-replacement-skips-the-journal": ([(
        LOG,
        "  resumeJournal(true);  // a replacement first finishes what the ended writer had in flight\n",
        "  g_q.inflight.store(0);\n")],
        "C5 / X1 / X2 / R1: a replacement writer finishes the ended writer's batch at the file position"),
    "M17-producer-writes-for-a-dead-writer": ([(
        LOG,
        "      if (ensureWriter() == WriterState::None) {\n",
        "      if (writerDead()) drainAsOwner(GetTickCount64() + kWaitSliceMs);\n"
        "      if (ensureWriter() == WriterState::None) {\n")],
        "R1: a producer never writes the file for a dead writer (Astra P1: synchronous I/O on a game thread)"),
    "M18-flush-writes-for-a-dead-writer": ([(
        LOG,
        "      if (ensureWriter() == WriterState::None) break;  // no writer will come: waiting cannot help\n",
        "      if (writerDead()) drainAsOwner(deadline);\n"
        "      if (ensureWriter() == WriterState::None) break;  // no writer will come: waiting cannot help\n")],
        "R2 / X2: a flusher never writes the file for a dead writer, and flush(200) keeps its budget"),
    "M19-crash-lines-wait-for-room": ([(
        LOG,
        "  if (t_emergency || g_lockOwner.load(std::memory_order_relaxed) == me ||\n",
        "  if (g_lockOwner.load(std::memory_order_relaxed) == me ||\n")],
        "C6: the crash filter's lines never wait for room (Astra P1: the [CRASH] write waited forever)"),
    "M20-reentry-not-detected": ([(
        LOG,
        "  if (t_emergency || g_lockOwner.load(std::memory_order_relaxed) == me ||\n",
        "  if (t_emergency ||\n")],
        "C7: a thread that logs while it holds the log lock (a fault inside the logger) does not spin forever"),
    "M21-emergency-without-reserve": ([(
        LOG,
        "    if (open && roomLocked(size_t(len), true)) {\n",
        "    if (open && roomLocked(size_t(len), false)) {\n")],
        "C6b: an emergency line on a full queue goes into the reserve (and lands)"),
    "M22-failed-write-counts-as-written": ([(
        LOG,
        "  if (ok) {\n    g_q.linesWritten.fetch_add(g_q.inflightLines, std::memory_order_relaxed);\n",
        "  if (true) {\n    g_q.linesWritten.fetch_add(g_q.inflightLines, std::memory_order_relaxed);\n")],
        "F3: a batch that could not be written counts lost and says so (Astra P2)"),
    "M23-retry-restarts-the-batch": ([(
        LOG,
        "    *done += w;\n    if (ok) return true;\n",
        "    *done = ok ? *done + w : 0;\n    if (ok) return true;\n")],
        "F2: a retry continues where the failed write stopped (nothing written twice)"),
    "M24-flush-ignores-loss": ([(
        LOG,
        "  return g_q.tail.load(std::memory_order_acquire) >= target && g_q.lostLines.load() == 0;\n",
        "  return g_q.tail.load(std::memory_order_acquire) >= target;\n")],
        "F3: flush() is false once a line was lost (Astra P2: success for lost records)"),
    "M25-torn-line-not-ended": ([(
        LOG,
        "  const bool prefix = g_q.tornLine;\n",
        "  const bool prefix = false;\n")],
        "F3: the next write ends the line a lost batch tore"),
    "M26-witness-feeds-itself": ([(
        LOG,
        "    if (external || injected) aggregateSlow(0, st, us, injected);\n",
        "    aggregateSlow(0, st, us, injected);\n")],
        "S2: a slow write of only the logger's own lines raises no new witness (Astra P2: shutdown never ended)"),
    "M27-witness-not-rate-limited": ([(
        LOG,
        "    if (!force && a.lastLineMs && now - a.lastLineMs < kWitnessPeriodMs) continue;\n",
        "")],
        "S2: at most one [LOG_STALL] write line per second under a disk that stays slow"),
    "M28-dbghelp-lock-not-exclusive": ([(
        "app/crash_handler_win.cpp",
        "    if (g_dbgHelpOwner.compare_exchange_strong(expected, me, std::memory_order_acq_rel)) {\n",
        "    if (true) {\n")],
        "H2: the DbgHelp lock is exclusive (Astra P2: two dump helpers in DbgHelp at once)"),
}


def build(build_dir, log):
    t = time.time()
    r = subprocess.run(["cmake", "--build", build_dir, "--target", "difftest"], cwd=REPO, capture_output=True,
                       text=True)
    with open(log, "w", encoding="utf-8") as f:
        f.write(r.stdout[-20000:] + r.stderr[-20000:])
    return r.returncode == 0, time.time() - t


def suite(exe, out_txt, timeout_s):
    t = time.time()
    p = subprocess.Popen([exe, "--log-async-test"], cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        out, err = p.communicate(timeout=timeout_s)
        rc, text = p.returncode, out + err[-4000:]
    except subprocess.TimeoutExpired:
        # the whole tree: the suite's child processes too (each commits guest memory; none may outlive the run)
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(p.pid)], capture_output=True)
        out, _err = p.communicate()
        rc, text = "TIMEOUT", out or ""
    with open(out_txt, "w", encoding="utf-8") as f:
        f.write(text)
    lines = [l for l in text.splitlines() if l.strip()]
    tally = next((l for l in reversed(lines) if l.endswith(" failed") and " passed, " in l), "")
    fails = [l.strip() for l in lines if l.strip().startswith("FAIL ")]
    return {"rc": rc, "tally": tally, "fails": fails[:8], "seconds": round(time.time() - t, 1)}


def mutate(edits):
    """Applies every (file, old, new) edit; returns the originals {path: bytes} and an error (nothing changed)."""
    originals, staged = {}, {}
    for rel, old, new in edits:
        path = os.path.join(REPO, rel)
        if path not in staged:
            originals[path] = open(path, "rb").read()
            staged[path] = originals[path].decode("utf-8")
        s = staged[path]
        crlf = "\r\n" in s
        o = old.replace("\n", "\r\n") if crlf else old
        n = new.replace("\n", "\r\n") if crlf else new
        if s.count(o) != 1:
            return {}, "%s: anchor matches %d times" % (rel, s.count(o))
        staged[path] = s.replace(o, n)
    for path, s in staged.items():
        open(path, "wb").write(s.encode("utf-8"))
    return originals, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build", help="the configured build dir (repo-relative or absolute)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="", help="comma-separated mutant names (or name prefixes such as M3)")
    ap.add_argument("--clean-only", action="store_true")
    ap.add_argument("--in-place", action="store_true", help="required: the mutants edit this checkout's sources")
    ap.add_argument("--timeout", type=int, default=300, help="per suite run, seconds")
    a = ap.parse_args()
    build_dir = a.build if os.path.isabs(a.build) else os.path.join(REPO, a.build)
    exe = os.path.join(build_dir, "difftest.exe" if os.name == "nt" else "difftest")
    os.makedirs(a.out, exist_ok=True)
    summary = {"repo": REPO, "build": build_dir, "mutants": {}}

    ok, secs = build(build_dir, os.path.join(a.out, "build_clean.log"))
    if not ok:
        print("BUILD FAILED (clean) - see build_clean.log")
        return 2
    clean = suite(exe, os.path.join(a.out, "clean.txt"), a.timeout)
    summary["clean"] = clean
    clean_ok = clean["rc"] == 0 and clean["tally"].endswith(" 0 failed")
    print("CLEAN: %s (rc %s, %s, %.0f s)" % ("PASS" if clean_ok else "FAIL", clean["rc"], clean["tally"], clean["seconds"]))
    if a.clean_only:
        json.dump(summary, open(os.path.join(a.out, "summary.json"), "w"), indent=1)
        return 0 if clean_ok else 1
    if not a.in_place:
        print("refusing to mutate without --in-place (this edits the checkout's sources; use a scratch tree)")
        return 2

    want = [w.strip() for w in a.only.split(",") if w.strip()]
    names = [n for n in MUTANTS if not want or any(n == w or n.startswith(w + "-") for w in want)]
    survived, errors = [], []
    for name in names:
        edits, why = MUTANTS[name]
        originals, err = mutate(edits)
        if err:
            print("MUTANT %s: ANCHOR ERROR (%s)" % (name, err))
            summary["mutants"][name] = {"error": err}
            errors.append(name)
            continue
        try:
            okb, bsecs = build(build_dir, os.path.join(a.out, name + ".build.log"))
            if not okb:
                print("MUTANT %s: BUILD FAILED (not counted)" % name)
                summary["mutants"][name] = {"error": "build failed"}
                errors.append(name)
                continue
            r = suite(exe, os.path.join(a.out, name + ".txt"), a.timeout)
        finally:
            for path, raw in originals.items():
                open(path, "wb").write(raw)
        killed = not (r["rc"] == 0 and r["tally"].endswith(" 0 failed"))
        r.update({"killed": killed, "catches": why, "build_seconds": round(bsecs, 1)})
        summary["mutants"][name] = r
        print("MUTANT %s: %s (rc %s, %s)%s" % (name, "KILLED" if killed else "SURVIVED", r["rc"], r["tally"],
                                               "" if killed else " - " + why))
        if not killed:
            survived.append(name)

    # Leave the build clean, and show the restored sources still pass.
    okb, _ = build(build_dir, os.path.join(a.out, "build_restored.log"))
    after = suite(exe, os.path.join(a.out, "clean_after.txt"), a.timeout) if okb else {"rc": "BUILD FAILED"}
    summary["clean_after"] = after
    summary["survived"], summary["errors"] = survived, errors
    json.dump(summary, open(os.path.join(a.out, "summary.json"), "w"), indent=1)
    after_ok = after.get("rc") == 0 and str(after.get("tally", "")).endswith(" 0 failed")
    print("CLEAN AFTER: %s; mutants killed %d of %d%s" % (
        "PASS" if after_ok else "FAIL", len(names) - len(survived) - len(errors), len(names),
        "; survived: " + ", ".join(survived) if survived else ""))
    if errors:
        return 2
    return 0 if clean_ok and after_ok and not survived else 1


if __name__ == "__main__":
    sys.exit(main())
