#!/usr/bin/env python3
"""shader_boot_load_run.py - run `difftest --shader-boot-load-test`, then mutation-validate it (render/boot-attach).

The suite (tools/difftest/shader_boot_load_test.cpp) proves the shader cache's staged boot load and its wait points
(docs/NATIVE_ROADMAP.md "Boot attach"). Each mutant below is one deliberate bug in that code; the suite must FAIL on
every one. The runner edits the source IN PLACE, rebuilds the difftest target, runs the suite and restores the file
(always, byte for byte), so it belongs in a scratch checkout - the cloud worker's tree - and refuses to run without
--in-place.

    python tools/difftest/shader_boot_load_run.py --build build --out <dir> --in-place [--only M1,M3]
    python tools/difftest/shader_boot_load_run.py --build build --out <dir> --clean-only     # no mutation

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

# name -> (file, original text, mutated text, what must catch it). '\n' is matched as the file's own line ending.
MUTANTS = {
    "M1-find-skips-installed-wait": (
        "gfx/shader_cache.cpp",
        "  waitStage(kStageInstalled);\n  const CachedShader* r = nullptr;\n  auto it = vs_.find(h);",
        "  const CachedShader* r = nullptr;\n  auto it = vs_.find(h);",
        "T3 / T6: a VS lookup during the load must block until stage 1, not read a partial map"),
    "M2-miss-skips-translator-stage": (
        "gfx/shader_cache.cpp",
        "  if (it == vs_.end()) waitStage(kStageAll);\n",
        "",
        "T4: an installed miss must wait for stage 2 (the persisted translations)"),
    "M3-imload-waits-for-everything": (
        "gfx/pm4_engine.cpp",
        "    auto& cache = getShaderCache();\n    // render/boot-attach: no ensureLoaded here",
        "    auto& cache = getShaderCache();\n    cache.ensureLoaded();\n    // render/boot-attach: no ensureLoaded here",
        "T6: an IM_LOAD hit must not wait for the translator stage"),
    "M4-begin-loads-synchronously": (
        "gfx/shader_cache.cpp",
        "  const bool ownThread = startLoadAsync();\n",
        "  ensureLoaded();\n  const bool ownThread = true;\n",
        "T2: beginBootLoad (the attach path) must return at once"),
    "M5-installed-published-early": (
        "gfx/shader_cache.cpp",
        "  try {\n    loadInstalled();",
        "  publish(kStageInstalled);\n  try {\n    loadInstalled();",
        "T3: stage 1 is published only once its maps are complete"),
    "M6-present-ignores-the-load": (
        "gfx/shader_cache.cpp",
        "  if (stage_.load(std::memory_order_acquire) >= kStageInstalled) return !loadedEmpty_.load(std::memory_order_acquire);",
        "  if (stage_.load(std::memory_order_acquire) >= kStageInstalled) return true;",
        "T5: a cache that loads nothing turns the xlat path off at stage 1"),
    "M7-watchdog-reads-host-wait-as-hang": (
        "kernel/gpu_hang_wd.cpp",
        "  if (!hostWait && s.cpStarted && s.cpPhaseSinceNs && age(s.cpPhaseSinceNs) >= kGhH3Ns) {",
        "  if (s.cpStarted && s.cpPhaseSinceNs && age(s.cpPhaseSinceNs) >= kGhH3Ns) {",
        "T7b: the CP's own stall in phase 5 (host wait) fires no H3"),
    "M8-loader-waits-on-itself": (
        "gfx/shader_cache.cpp",
        "  if (loaderId_.load(std::memory_order_acquire) == std::this_thread::get_id()) return;\n",
        "",
        "T5: the loader's own hooks must not wait on the loader (a deadlock: the suite's 30 s guard)"),
    "M9-init-loads-before-the-attach": (
        "gfx/d3d12_backend.cpp",
        "  if (!ShaderCache::bootLoadAsync()) shaderCache.ensureLoaded();",
        "  shaderCache.ensureLoaded();",
        "T8: D3D12Backend::init must not wait for the load (needs a D3D12 device + swap chain on this machine)"),
    "M10-backend-wait-gives-up": (
        "kernel/gpu_submit.cpp",
        "    if (failed()) break;\n",
        "    if (failed() || now() - t0 >= 60ull * 1000000000ull) break;\n",
        "T9a: a late attach (init not failed) is waited for with no time limit - no exit at 60 s"),
    "M11-init-failure-runs-on": (
        "kernel/gpu_submit.cpp",
        "    cpBackendFatal(reason());\n",
        "    (void)reason();\n",
        "T9c: an explicit D3D12 init failure ends the process (exit 3), never runs the ring without a backend"),
    "M12-backend-wait-leaves-phase": (
        "kernel/gpu_submit.cpp",
        "  const uint32_t before = setPhase(c, kCpPhaseHostWait);\n",
        "  const uint32_t before = c.phase.load();\n",
        "T9a / T9b: the backend wait runs in CP phase 5 (the host wait the watchdog reads)"),
    # Astra round 2
    "M13-suppression-without-escalation": (
        "kernel/gpu_hang_wd.cpp",
        "    if (gpuHangHostWaitDue(waitAge, st.hostWaitFired)) {\n",
        "    if (false && gpuHangHostWaitDue(waitAge, st.hostWaitFired)) {\n",
        "T9d: a long host wait escalates as [HOST_WAIT_STALL] (the triggers it sets aside are not the only word)"),
    "M14-blanket-host-wait-suppression": (
        "kernel/gpu_hang_wd.cpp",
        "uint32_t gpuHangEvaluate(const GpuHangSnap& s) {\n  uint32_t m = 0;\n",
        "uint32_t gpuHangEvaluate(const GpuHangSnap& s) {\n  uint32_t m = 0;\n"
        "  if (s.cpPhase == kCpPhaseHostWait) return 0;\n",
        "T7a: a fence / present stall during a host wait still fires"),
    "M15-stage-end-after-publication": (
        "gfx/shader_cache.cpp",
        "  const double s1End = msSinceProcessStart();\n  installedEndMs_.store(s1End, std::memory_order_relaxed);\n"
        "  publish(kStageInstalled);\n  if (auto* h = g_publishHook.load(std::memory_order_acquire)) h(kStageInstalled);\n",
        "  publish(kStageInstalled);\n  if (auto* h = g_publishHook.load(std::memory_order_acquire)) h(kStageInstalled);\n"
        "  const double s1End = msSinceProcessStart();\n  installedEndMs_.store(s1End, std::memory_order_relaxed);\n",
        "T10: stage 1's end is sampled before its publication"),
    # Astra round 3
    "M16-first-command-before-completeness": (
        "kernel/gpu_submit.cpp",
        "  if (need > avail) return false;  // the rest is not published yet: nothing is consumed\n"
        "  if (!first.taken) first = GpuCpFirstCommand{true, nowMs ? nowMs() : -1.0, kind};\n",
        "  if (!first.taken) first = GpuCpFirstCommand{true, nowMs ? nowMs() : -1.0, kind};\n"
        "  if (need > avail) return false;  // the rest is not published yet: nothing is consumed\n",
        "T11: the first command is recorded when its words are all published, not when the ring became nonempty"),
    "M17-dynamic-report-tag": (
        "kernel/gpu_hang_wd.cpp",
        '      SR_LOG_BOOT(E, "[HOST_WAIT_STALL] " fmt __VA_OPT__(,) __VA_ARGS__);       \\\n',
        '      SR_LOG_BOOT(E, "[%s] " fmt, "HOST_WAIT_STALL" __VA_OPT__(,) __VA_ARGS__); \\\n',
        "T9e: a report's format strings carry a literal milestone tag (else a player log caps them at 64)"),
    "M18-host-report-dumps-threads-every-time": (
        "kernel/gpu_hang_wd.cpp",
        "  if (!host || !s_hostThreadsDumped.exchange(true)) {\n",
        "  if (true) {\n",
        "T9e: repeated host-wait reports must not spend the capped thread / CS dump lines a [GPU_HANG] needs"),
    "M19-a-helper-per-report": (
        "kernel/gpu_hang_wd.cpp",
        "  if (!g_helperBusy.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {\n",
        "  if (false) {\n",
        "T9f: at most one helper thread while a lock is held"),
    "M20-hang-report-not-annotated": (
        "kernel/gpu_hang_wd.cpp",
        "  if (cpPhase != kCpPhaseHostWait || !out || !n) return false;\n",
        "  if (true) return false;\n",
        "T7d: a [GPU_HANG] fired in a host wait names the wait"),
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
    try:
        r = subprocess.run([exe, "--shader-boot-load-test"], cwd=REPO, capture_output=True, text=True,
                           timeout=timeout_s)
        rc, text = r.returncode, r.stdout + r.stderr[-4000:]
    except subprocess.TimeoutExpired as e:
        rc, text = "TIMEOUT", (e.stdout or "") if isinstance(e.stdout, str) else ""
    with open(out_txt, "w", encoding="utf-8") as f:
        f.write(text)
    lines = [l for l in text.splitlines() if l.strip()]
    tally = next((l for l in reversed(lines) if l.endswith(" failed") and " passed, " in l), "")
    fails = [l.strip() for l in lines if l.strip().startswith("FAIL ")]
    notrun = [l.strip() for l in lines if "NOT RUN" in l]
    return {"rc": rc, "tally": tally, "fails": fails[:8], "not_run": notrun, "seconds": round(time.time() - t, 1)}


def mutate(path, old, new):
    raw = open(path, "rb").read()
    s = raw.decode("utf-8")
    crlf = "\r\n" in s
    o = old.replace("\n", "\r\n") if crlf else old
    n = new.replace("\n", "\r\n") if crlf else new
    if s.count(o) != 1:
        return raw, "anchor matches %d times" % s.count(o)
    open(path, "wb").write(s.replace(o, n).encode("utf-8"))
    return raw, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default="build", help="the configured build dir (repo-relative or absolute)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="", help="comma-separated mutant names (or name prefixes such as M3)")
    ap.add_argument("--clean-only", action="store_true")
    ap.add_argument("--in-place", action="store_true", help="required: the mutants edit this checkout's sources")
    ap.add_argument("--timeout", type=int, default=240, help="per suite run, seconds")
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
    print("CLEAN: %s (rc %s, %s, %.0f s)%s" % ("PASS" if clean_ok else "FAIL", clean["rc"], clean["tally"],
                                               clean["seconds"], "; " + "; ".join(clean["not_run"]) if clean["not_run"] else ""))
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
        rel, old, new, why = MUTANTS[name]
        path = os.path.join(REPO, rel)
        original, err = mutate(path, old, new)
        if err:
            print("MUTANT %s: ANCHOR ERROR in %s (%s)" % (name, rel, err))
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
            open(path, "wb").write(original)
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
