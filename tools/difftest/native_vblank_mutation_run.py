#!/usr/bin/env python3
"""native_vblank_mutation_run.py - compiled mutants of kernel/native_vblank.cpp against difftest --native-vblank-test.

    python tools/difftest/native_vblank_mutation_run.py --out archive/dreg/mutants [--build-dir build] [-j 6]

display.registers (docs/NATIVE_ROADMAP.md "DISPLAY.REGISTERS BUILD NOTE"). The suite's own mutant sections (G) show its
tables catch defective COPIES of the arithmetic; this runner shows the suite is wired to the PRODUCTION code: each
mutant edits one line of kernel/native_vblank.cpp (or the source its fifth field names - gfx/record_engine.cpp's
SCRATCH_REG1 bracket in writeReg, gfx/pm4_engine.cpp's in the executor's deferred perform; exactly one occurrence,
else the run stops), rebuilds the difftest
target, runs the suite and must make it FAIL (exit 1) with at least one named FAIL line; then the source bytes are
restored, the target rebuilt and the suite must pass again (exit 0). Restoration runs in a finally even when a build
or run fails. Run it on the build worker (tools/cloud/sr_cloud.py run), never on the local PC. Writes
<out>/mutants.json (per mutant: the edit, build and suite exit codes, the FAIL lines) and per-step logs.
Exit 0 = every mutant failed the suite and the restored build passed it; 1 otherwise; 2 = a setup error.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

SOURCE = "kernel/native_vblank.cpp"
# (name, the production line's text, its mutated form, what it breaks[, the source when not SOURCE])
MUTANTS = [
    ("signed_remaining",
     "  if (int32_t(remaining) > 0) {                                  // 82111E68",
     "  if (remaining > 0u) {                                          // 82111E68",
     "82111E68's signed compare (add. / bgt) made unsigned: a negative remaining waits instead of testing line %"),
    ("stale_last",
     "  PPC_STORE_U32(dev + kDevLast, PPC_LOAD_U32(dev + kDevCount));  // 82111EB0 / 82111EB4 (reloaded after that store)",
     "  PPC_STORE_U32(dev + kDevLast, count);                          // 82111EB0 / 82111EB4 (reloaded after that store)",
     "the +0x3B18 reload after the pc+4 store replaced by the value read at 82111E4C (pc+4 aliasing the counter)"),
    ("override_always_guest",
     "  if (!sr::native::vblank::vblankGateRequested()) {",
     "  if (true) {",
     "the strong sub_82111E38 never reaching the host form (the wiring)"),
    ("counter_not_advanced",
     "  g_vcount.store(nextVCounter(g_vcount.load(std::memory_order_relaxed), highFps), std::memory_order_relaxed);  // today's answer for this tick, held by host code",
     "  (void)highFps;  // today's answer for this tick, held by host code",
     "the host counter no longer advanced by the vblank tick (the pacing reads a stale V)"),
    ("counter_wrap_saturates",
     "  return highFps ? 0u : v + kHeight;  // HIGHFPS: scanline 0; the console path: + the display height, wrapping at 2^32",
     "  return highFps ? 0u : (v > 0xFFFFFFFFu - kHeight ? 0xFFFFFFFFu : v + kHeight);  // HIGHFPS: scanline 0; the console path: + the display height, wrapping at 2^32",
     "today's V_COUNTER wrap at 2^32 broken: the counter saturates (the scan-out write shares it; the guest replica does not)"),
    ("product_wrap_lost",
     "  uint32_t q = height ? product / height : 0u;            // 82111E84 twllei: the console traps on 0, the generated code guards",
     "  uint32_t q = height ? uint32_t(uint64_t(v) * 100u / height + 0u * product) : 0u;  // 82111E84 twllei: the console traps on 0, the generated code guards",
     "the wrap points broken: V*100 in 64 bits, so the rare line % <= 8 / < 100 answers past 2^32 / 100 vanish"),
    ("highfps_counter",
     "  return highFps ? 0u : v + kHeight;  // HIGHFPS: scanline 0; the console path: + the display height, wrapping at 2^32",
     "  return (highFps && false) ? 0u : v + kHeight;  // HIGHFPS: scanline 0; the console path: + the display height, wrapping at 2^32",
     "HIGHFPS no longer answering scanline 0"),
    ("shadow_blind",
     "    same = outcome == want.outcome && effectsMatch(dev, want, count, last, pc4, gen, flight);",
     "    same = true;",
     "the ON arm's shadow check no longer comparing the host form with today's register answers"),
    ("trace_blind",
     "      bad |= count != pre_[1] + 1u;",
     "      bad |= false;",
     "the per-vblank check no longer testing the counter increment"),
    ("live_registers_blind",
     "    bad |= liveBad;",
     "    bad |= false && liveBad;",
     "the per-vblank check no longer comparing live scan-out registers with the host counter"),
    # Astra round 1 (2026-10-14): the three findings' fixes, each mutated back
    ("tick_any_registration",
     "  p.native = registrationNative || (d3d && active());",
     "  p.native = registrationNative || active();",
     "P1: the host source 0 selected for whatever registration is leased (not only the D3D ISR's)"),
    ("release_race_blind",
     "          bad |= last != count || (havePc_ && pc4 != 0u && !rearmed);",
     "          bad |= last != count || (havePc_ && pc4 != 0u);",
     "P2: the release check no longer excusing the CP's rearm after a correct release (a false traceBad)"),
    ("rearm_not_counted",
     "      if (n == 1u) sr::native::vblank::noteScratch1Begin(addr, value);",
     "      if (false) sr::native::vblank::noteScratch1Begin(addr, value);",
     "P2: the CP's SCRATCH_REG1 write-back no longer counted before its store, so its rearm cannot be told from a "
     "lost release",
     "gfx/record_engine.cpp"),
    # native/dreg-hang (fix1's traceBad=3): the write-back's bracket
    ("rearm_end_dropped",
     "      if (n == 1u) sr::native::vblank::noteScratch1End();",
     "      if (false) sr::native::vblank::noteScratch1End();",
     "the bracket never closed: every later pc+4 change excused as a write-back in flight (foreign writes unseen)",
     "gfx/record_engine.cpp"),
    ("flight_blind",
     "  if (!(genNow != gen || flight != 0u || flightNow != 0u)) return false;  // no write-back in the window",
     "  if (!(genNow != gen)) return false;  // no write-back in the window",
     "the in-flight term dropped: a tick wholly inside the CP's write-back bracket flags the CP's own rearm"),
    # Astra round 2 (2026-10-15): the explanation's target, and the executor's bracket on its own
    ("explain_value_blind",
     "  return ((uint32_t(t >> 32) ^ (pcVA + 4u)) & 0x1FFFFFFCu) == 0u && uint32_t(t) == observed;  // its address and value",
     "  return t == t || observed == 0u;  // its address and value",
     "round 1's excuse back: any pc+4 change inside a write-back's window excused (a foreign clear to 0 unseen)"),
    ("explain_addr_blind",
     "  return ((uint32_t(t >> 32) ^ (pcVA + 4u)) & 0x1FFFFFFCu) == 0u && uint32_t(t) == observed;  // its address and value",
     "  return uint32_t(t) == observed;  // its address and value",
     "the target's address ignored: a write-back elsewhere explains a foreign write of the same value to pc+4"),
    ("exec_begin_dropped",
     "      if (reg1) sr::native::vblank::noteScratch1Begin(a, v);",
     "      if (false) sr::native::vblank::noteScratch1Begin(a, v);",
     "the executor's deferred perform no longer counted before its store",
     "gfx/pm4_engine.cpp"),
    ("exec_end_dropped",
     "      if (reg1) sr::native::vblank::noteScratch1End();",
     "      if (false) sr::native::vblank::noteScratch1End();",
     "the executor's deferred perform never closing its bracket",
     "gfx/pm4_engine.cpp"),
    ("early_clear_blind",
     "      if (!release) bad |= havePc_ && pc4 != pre_[5] && !rearmed;",
     "      if (!release) bad |= false;",
     "P2: the per-vblank check no longer catching a pc+4 clear on a tick that releases nothing"),
    ("pacing_clear_blind",
     "    return rd(dev + kDevLast) == last && pcSame;",
     "    return rd(dev + kDevLast) == last && (pcSame || true);",
     "P2: the pacing shadow no longer catching a pc+4 clear on WAIT / NEXT"),
    # native/dreg-hang (2026-10-15): dreg2's stall class (section T), each fix mutated back
    ("source0_os_query",
     "  uint8_t* first = arenaRange(va, bytes);",
     "  uint8_t* first = arenaRange(va, bytes); if (first && (!readable(first, bytes) || (write && !readable(first, bytes)))) first = nullptr;",
     "the host source 0's OS permission queries restored (a VirtualQuery walk per range, two per written range): "
     "dreg2's ~21 ms tick under the CPU-2 lock",
     "kernel/native_display.cpp"),
    ("census_takes_lock",
     "  const uint32_t vcount = g_vcount.load(std::memory_order_relaxed);  // the census: lock-free",
     "  const uint32_t vcount = (std::lock_guard<std::mutex>{g_cpu2}, g_vcount.load(std::memory_order_relaxed));  // the census: lock-free",
     "the census line taking the CPU-2 lock again: dreg2's CP waited 9.7 s there behind the ticks"),
    ("protect_diag_ignored",
     "  if (gate && flush && segmentless && kpcr && scanoutRegs && protectFree) return State::On;",
     "  if (gate && flush && segmentless && kpcr && scanoutRegs && (protectFree || true)) return State::On;",
     "the gate no longer refusing under a page-protecting diagnostic (every host source 0 would decline: fatal)"),
    # native/dreg-bake (2026-10-15): SR_NATIVE_VBLANK default ON - the latch's request and the default's stand-down
    ("default_refuses",
     "  return request == Request::Explicit ? State::Refused : State::Off;  // the default stands down",
     "  return State::Refused;  // the default stands down",
     "the default refusing like an explicit =1: an Error line and state=refused in every arm that turns a requirement "
     "off (the transport's =0 arms)"),
    ("default_unbaked",
     "  return env && env[0] == '1' ? Request::Explicit : Request::Default;",
     "  return env && env[0] == '1' ? Request::Explicit : Request::Off;",
     "the latch's bake undone: unset reads as =0"),
    ("override_unbaked",
     "    return !e || e[0] != '0';",
     "    return e && e[0] == '1';",
     "the override's first test back to the pre-bake form: unset skips the host form even when the latch is on"),
    # Astra round 1 on native/dreg-bake (P3): an explicit =1 read as the default - a missing requirement stands down
    # instead of refusing and the On line names the default; only section E2's fresh processes can see it
    ("explicit_as_default",
     "  const Request req = requestOf(std::getenv(\"SR_NATIVE_VBLANK\"));",
     "  const Request req = requestOf(std::getenv(\"SR_NATIVE_VBLANK\")) == Request::Explicit ? Request::Default : "
     "requestOf(std::getenv(\"SR_NATIVE_VBLANK\"));",
     "computeState mapping an explicit =1 to the default: refusal lost (a missing requirement stands down silently), "
     "the On line naming the default"),
]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("-j", "--jobs", type=int, default=6)
    a = ap.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    mutants = [(m[0], m[1], m[2], m[3], m[4] if len(m) > 4 else SOURCE) for m in MUTANTS]
    paths = sorted({m[4] for m in mutants})
    originals = {p: (root / p).read_bytes() for p in paths}
    texts = {p: originals[p].decode("utf-8") for p in paths}
    newline = "\r\n" if "\r\n" in texts[SOURCE] else "\n"
    exe = root / a.build_dir / "difftest.exe"
    receipt = {"source": SOURCE, "source_sha256": sha(originals[SOURCE]),
               "sources": {p: sha(originals[p]) for p in paths},
               "tool_sha256": sha(Path(__file__).read_bytes()), "mutants": []}

    def restore_all():
        for p in paths:
            (root / p).write_bytes(originals[p])

    def run(name, cmd, timeout):
        log = out / (name + ".log")
        with open(log, "wb") as f:
            try:
                rc = subprocess.run(cmd, cwd=root, stdout=f, stderr=subprocess.STDOUT, timeout=timeout).returncode
            except subprocess.TimeoutExpired:
                rc = 124
        return rc, log.read_text(encoding="utf-8", errors="replace")

    def build(name):
        return run(name, ["cmake", "--build", a.build_dir, "--target", "difftest", "-j", str(a.jobs)], 3600)[0]

    def suite(name):
        rc, txt = run(name, [str(exe), "--native-vblank-test"], 600)
        fails = [ln.strip() for ln in txt.splitlines() if ln.startswith("FAIL:")]
        summary = [ln.strip() for ln in txt.splitlines() if ln.startswith("[NATIVE_VBLANK_TEST]")]
        return rc, fails, summary[-1] if summary else ""

    for name, old, new, why, path in mutants:
        if texts[path].count(old) != 1:
            print("setup error: %s: the production line occurs %d times in %s" % (name, texts[path].count(old), path))
            return 2
    ok = True
    try:
        for name, old, new, why, path in mutants:
            (root / path).write_bytes(texts[path].replace(old, new).encode("utf-8"))
            b = build("build_" + name)
            rc, fails, summary = suite("suite_" + name) if b == 0 else (None, [], "")
            caught = b == 0 and rc == 1 and len(fails) > 0
            ok &= caught
            row = {"mutant": name, "source": path, "breaks": why, "old": old.strip(), "new": new.strip(),
                   "build_exit": b, "suite_exit": rc, "fail_lines": fails[:12], "fail_count": len(fails),
                   "summary": summary, "caught": caught}
            receipt["mutants"].append(row)
            print(json.dumps({k: row[k] for k in ("mutant", "build_exit", "suite_exit", "fail_count", "caught")}),
                  flush=True)
            (root / path).write_bytes(originals[path])
    finally:
        restore_all()
        b = build("build_restored")
        rc, fails, summary = suite("suite_restored") if b == 0 else (None, [], "")
        now = {p: sha((root / p).read_bytes()) for p in paths}
        receipt["restored"] = {"source_sha256": now[SOURCE], "sources": now, "build_exit": b, "suite_exit": rc,
                               "fail_count": len(fails), "summary": summary}
        ok &= b == 0 and rc == 0 and now == receipt["sources"]
        receipt["verdict"] = "PASS" if ok else "FAIL"
        (out / "mutants.json").write_text(json.dumps(receipt, indent=1) + newline, encoding="utf-8")
    print("native vblank mutants: %s (%d mutants; restored suite exit %s)" % (
        receipt["verdict"], len(receipt["mutants"]), receipt["restored"]["suite_exit"]))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
