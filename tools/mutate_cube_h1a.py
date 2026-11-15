#!/usr/bin/env python3
"""mutate_cube_h1a.py -- prove the H1(a) cube contract tests can FAIL.

The SR_HLE_TEX_UNLOCK_TEST suite reports 359/359 for the cube slice. That number
is worth nothing until each rule has been watched to fail. Every mutant below is
a WRONG cube rule that a reasonable person could write -- several are rules I
DID write and the fixtures caught, which is the only reason they are here as
mutants rather than as shipped defects.

The director named six fields that must be mutation-validated: the recorder
total, the replay wiring, the packed-mips bit, the mip-count bits, the version
gate, and the fail-closed executor arm. Each has at least one mutant.

⛔ A CRASH IS NOT DETECTION. A mutant that fails to compile, or that makes the
suite crash, proves only that the file was edited. Detection means the suite
RUNS and REPORTS failures.

Each mutant: patch -> rebuild -> run the suite -> expect failures -> restore.
A rebuild is ~7 s for a .cpp and ~40 s for a header, so this takes a few minutes.

    python tools/mutate_cube_h1a.py

Exit 0 = every mutant detected.
"""
import os, re, shutil, subprocess, sys, tempfile, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(REPO, "build", "SupermanReturns.exe")
TRACE = "archive/iter456_title_fresh.srht"

V = "gfx/hle_video.cpp"
HH = "gfx/hle_video.hpp"
CL = "gfx/cube_layout.hpp"
CR = "kernel/cube_capture_rule.hpp"
KP = "kernel/hle_api_observe.cpp"

# (name, file, find, replace) -- `find` must appear EXACTLY once.
MUTANTS = [
    # ── THE RECORDER TOTAL (the director's find) ────────────────────────────
    ("recorder serializes `bytes` only, dropping the mip region", V,
     "      rec.payloadBytes = (uint64_t)c.textureUpload.bytes +\n"
     "                         (uint64_t)(uploadIsCube(c.textureUpload.fmtBits)\n"
     "                                        ? c.textureUpload.mipBytes : 0u);",
     "      rec.payloadBytes = c.textureUpload.bytes;"),

    # ── THE FAIL-CLOSED EXECUTOR ARM ────────────────────────────────────────
    ("the executor's cube arm is removed (a cube uploads as 2D again)", V,
     "        if (uploadIsCube(a.fmtBits)) {",
     "        if (false) {"),

    # The anchor moved when the wrong-pool release was fixed. The MUTANT is
    # unchanged: delete the terminator so a cube falls into the non-cube
    # path. It is caught by the backend-independent entry witness, which is
    # the only thing that can see it with no initialized backend.
    ("the executor's cube arm falls THROUGH instead of refusing", V,
     "          else texRelease(a.arenaOffset);\n          break;",
     "          else texRelease(a.arenaOffset);"),

    # ── THE VERSION GATE ────────────────────────────────────────────────────
    ("the trace version is not bumped (v6 keeps claiming cube support)", HH,
     "constexpr uint32_t kTraceVersion = 7;",
     "constexpr uint32_t kTraceVersion = 6;"),

    ("the version FLOOR is raised, invalidating every banked v5 fixture", HH,
     "constexpr uint32_t kTraceVersionMin = 5;",
     "constexpr uint32_t kTraceVersionMin = 7;"),

    ("the replayer accepts a pre-v7 record claiming a cube", V,
     '          if (h.version < 7u) {\n            why = "preV7CubeRecord";',
     '          if (false) {\n            why = "preV7CubeRecord";'),

    # ── THE PACKED-MIPS BIT ─────────────────────────────────────────────────
    ("packedMips always reads false", HH,
     "inline bool uploadPackedMips(uint32_t fmtBits) {\n"
     "  return (fmtBits & kUploadPackedMipsBit) != 0;\n}",
     "inline bool uploadPackedMips(uint32_t) {\n  return false;\n}"),

    ("packedMips is read from the wrong bit", HH,
     "constexpr uint32_t kUploadPackedMipsBit  = 1u << 30;",
     "constexpr uint32_t kUploadPackedMipsBit  = 1u << 31;"),

    # ── THE MIP-COUNT BITS ──────────────────────────────────────────────────
    ("mipLevels always reads 1 (base only)", HH,
     "inline uint32_t uploadMipLevels(uint32_t fmtBits) {\n"
     "  return ((fmtBits >> kUploadMipLevelsShift) & kUploadMipLevelsMask) + 1u;\n}",
     "inline uint32_t uploadMipLevels(uint32_t) {\n  return 1u;\n}"),

    ("mipLevels is read from bits 9-11, which collide with endian at 12", HH,
     "constexpr uint32_t kUploadMipLevelsShift = 27;",
     "constexpr uint32_t kUploadMipLevelsShift = 9;"),

    # ── THE VALIDATOR'S INDIVIDUAL CHECKS ───────────────────────────────────
    ("the base-region length is not checked", V,
     '  if (a.bytes != L.baseSpanBytes)           { r.why = "baseBytes"; return r; }',
     "  if (false) { return r; }"),

    ("the mip-region length is not checked", V,
     '  if (a.mipBytes != L.mipsTotalExtentBytes) { r.why = "mipBytes"; return r; }',
     "  if (false) { return r; }"),

    ("the payload TOTAL is not checked against the two segments", V,
     "  if (payloadBytes != (uint64_t)a.bytes + (uint64_t)a.mipBytes) {\n"
     '    r.why = "payloadTotal"; return r;\n  }',
     "  if (false) { return r; }"),

    ("the six-face count is not verified", V,
     '  if (faces != sr::gfx::cube::kCubeFaces) { r.why = "faceCount"; return r; }',
     "  if (false) { return r; }"),

    # ── THE LAYOUT BODY ─────────────────────────────────────────────────────
    ("the face stride is the forbidden 6*w*h*bpp intuition", CL,
     "      L.faceStrideBytes = stride;\n"
     "      L.baseSpanBytes = stride * kCubeFaces;",
     "      L.faceStrideBytes = width * height * bpb;\n"
     "      L.baseSpanBytes = stride * kCubeFaces;"),

    # packed_level == 0 now HAS a mip-region loop iteration (H1(b)-r3).
    # Collapsing it back to "loop starts at level 1" is the r2 defect.

    ("packed_level==0 does not emit a separate mip tail (the r2 defect)", CL,
     "  if (packedMips && L.packedLevel == 0)\n"
     "    loopLevelLast = (maxLevel != 0u) ? 1u : 0u;",
     "  if (false)\n"
     "    loopLevelLast = (maxLevel != 0u) ? 1u : 0u;"),
    ("the packed-mip offset's wide/tall branch is inverted", CL,
     "    if (log2W > log2H) { xBlocks = 0; yBlocks = 16u >> pm; }",
     "    if (log2W >= log2H) { xBlocks = 0; yBlocks = 16u >> pm; }"),

    ("an undecodable format is treated as supported", CL,
     "  return g.supported;",
     "  return true;"),

    ("the face count constant is 1", CL,
     "constexpr uint32_t kCubeFaces = 6;",
     "constexpr uint32_t kCubeFaces = 1;"),

    # ── THE PRODUCER ────────────────────────────────────────────────────────
    # The producer rule moved OUT of the guest override and into
    # kernel/cube_capture_rule.hpp, which is what made it drivable at all.
    ("the producer does not verify stack_depth == 5 (six faces assumed)", CR,
     "  if (stackDepth != 5u) { r.refuse = CubeCapRefuse::StackDepth; return r; }",
     "  if (false) { return r; }"),

    ("stack_depth is checked only AFTER guest memory is read", CR,
     "  // \u26d4 THE XENOS INVARIANT, checked before anything is sized or read.\n"
     "  const uint32_t stackDepth = (f[2] >> 26) & 0x3Fu;\n"
     "  if (stackDepth != 5u) { r.refuse = CubeCapRefuse::StackDepth; return r; }",
     "  const uint32_t stackDepth = (f[2] >> 26) & 0x3Fu;"),

    # ⚠ REFORMULATED. The obvious form of this mutant is
    #     pM = pB + L.baseSpanBytes;
    # and it CRASHES rather than being detected: C13 deliberately leaves the
    # range adjacent to the base UNCOMMITTED, so raw pointer arithmetic
    # faults, and the harness VEH makes any host AV fatal before SEH filters
    # run. A crash proves the edit landed, not that the suite discriminates.
    #
    # This form keeps the FAULT-FREE contract (the read still goes through
    # the injected reader, which checks readability) while making the same
    # mistake: it asks for the mips at base + baseSpan instead of at the
    # address the guest declared. The reader refuses, the capture refuses,
    # and C13's publication assertion fails normally.
    ("the producer reads the mips at base + baseSpan, not at mip_address", CR,
     "    pM = rd(mipAddr, L.mipsTotalExtentBytes);",
     "    pM = rd(base + L.baseSpanBytes, L.mipsTotalExtentBytes);"),

    ("mip_address == 0 no longer forces a base-only capture", CR,
     "  const uint32_t mipLevels = (mipPage && mipMax) ? (mipMax + 1u) : 1u;",
     "  const uint32_t mipLevels = mipMax ? (mipMax + 1u) : 1u;"),

    # ── THE EXECUTOR'S POOL OWNERSHIP (the director's source-review find) ──
    ("the cube refusal releases the WRONG pool (poolRelease)", V,
     "          if (unlockOrigin) classARelease(a.arenaOffset, a.bytes);\n"
     "          else texRelease(a.arenaOffset);\n"
     "          break;",
     "          poolRelease(a.arenaOffset);\n          break;"),

    ("the cube refusal leaks its payload entirely (no release)", V,
     "          if (unlockOrigin) classARelease(a.arenaOffset, a.bytes);\n"
     "          else texRelease(a.arenaOffset);\n"
     "          break;",
     "          break;"),

    # ── THE REPLAY-SIDE WIRING ─────────────────────────────────────────────
    ("the replayer cube branch is never entered", V,
     "        if (rec.cmd.type == RenderCommandType::TextureUpload &&\n"
     "            uploadIsCube(rec.cmd.textureUpload.fmtBits)) {",
     "        if (false) {"),

    ("a REFUSED replay record is still staged into the arena", V,
     "            rec.cmd.textureUpload.bytes = 0;\n"
     "            rec.cmd.textureUpload.mipBytes = 0;\n"
     "            rec.cmd.textureUpload.arenaOffset = kArenaFull;",
     "            rec.cmd.textureUpload.mipBytes = 0;"),

    # ── THE WRAPPER-TO-OBSERVER CALL (the director wrote this one too) ─────
    # r3 left this uncovered: replacing it with 0 BUILT and still reported
    # 410/410. C13 now enters sub_82100310 itself, so the observer never
    # runs and every one of its assertions fails.
    ("the wrapper-to-observer call is deleted", KP,
     "  const uint32_t fetchBase = observeSetTextureFetch(dev, slot);",
     "  const uint32_t fetchBase = 0;"),

    # ── VALID PRODUCTION PUBLICATION SUPPRESSED ───────────────────────────
    # r3 asserted only that the dispatch witness moved, so a cube that was
    # refused as baseUnreadable read as a pass. C13 now asserts publication,
    # the exact command and byte-exact base || mips.
    ("valid production publication is suppressed", CR,
     "  r.published = true;",
     "  r.published = false;"),

    ("the publisher is never invoked", CR,
     "  pub(r);",
     "  (void)0;"),

    ("the two regions are concatenated in the WRONG order", CR,
     "  for (uint32_t i = 0; i < L.baseSpanBytes; i++) scratch[i] = pB[i];\n"
     "  for (uint32_t i = 0; i < L.mipsTotalExtentBytes; i++)\n"
     "    scratch[L.baseSpanBytes + i] = pM[i];",
     "  for (uint32_t i = 0; i < L.mipsTotalExtentBytes; i++) scratch[i] = pM[i];\n"
     "  for (uint32_t i = 0; i < L.baseSpanBytes; i++)\n"
     "    scratch[L.mipsTotalExtentBytes + i] = pB[i];"),

    # ── THE PRODUCTION DISPATCH (the director wrote this one, not me) ──────
    # r2 tested the extracted rule body and this exact mutation still left
    # the suite at 399/399. C13 now drives the real post-delegate observer,
    # so the dispatch witness stops moving and the assertion fails normally.
    ("the production SetTexture cube dispatch is deleted", KP,
     "      if (cubeCaptureEnabled() && texCaptureEnabled() &&",
     "      if (false && cubeCaptureEnabled() && texCaptureEnabled() &&"),

    ("the cube dispatch fires for NON-cube fetches too", KP,
     "      if (cubeCaptureEnabled() && texCaptureEnabled() &&\n"
     "          ((f[5] >> 9) & 3u) == 3u) {",
     "      if (cubeCaptureEnabled() && texCaptureEnabled()) {"),

    ("cubeHandled is never set, so a cube ALSO takes the 2D guard", KP,
     "        cubeHandled = true;               // a cube NEVER falls to the 2D guard",
     "        cubeHandled = false;"),

    ("the gate parse treats any set value as ON", KP,
     "bool cubeGateWantsOn(const char* e) { return e && e[0] == \'1'; }",
     "bool cubeGateWantsOn(const char* e) { return e != nullptr; }"),

    ("the cube capture gate defaults ON", KP,
     "bool cubeGateWantsOn(const char* e) { return e && e[0] == \'1'; }",
     "bool cubeGateWantsOn(const char* e) { return !e || e[0] != \'0'; }"),
]


def build():
    """(ok, log). Retries ONCE, because a failed build here is ambiguous.

    \u26d4 iter-457 r5: this harness scored a mutant "DID NOT COMPILE (proves
    nothing)" that compiles perfectly well by hand. The cause was a transient
    Windows LINK failure - the previous suite process still held
    SupermanReturns.exe open - and nothing in the output distinguished that from
    a genuine compile error. A mutation harness whose failure mode looks
    identical to a real defect cannot be used to certify anything, so: retry
    once, and if it still fails, PRINT the offending lines instead of hiding
    them behind a verdict word.
    """
    for attempt in (1, 2):
        r = subprocess.run(["cmake", "--build", "build/"], cwd=REPO,
                           capture_output=True, text=True, encoding="utf-8",
                           errors="replace")
        log = (r.stdout or "") + (r.stderr or "")
        if r.returncode == 0:
            return True, log
        if attempt == 1:
            # The exe is released asynchronously after the suite exits.
            time.sleep(3.0)
    return False, log


def run_suite():
    """(passed, failed, crashed)."""
    env = dict(os.environ)
    env["SR_HLE_TEX_UNLOCK_TEST"] = "1"
    env.setdefault("SR_MODS", "0")  # iter-463: never mount the player's mods (docs/MODS.md)
    env.setdefault("SR_DEVTOOLS", "0")  # iter-463 phase 2: nor enable a dev tool (docs/DEVTOOLS.md)
    env["SR_HLE_TRACE_REPLAY"] = TRACE
    try:
        r = subprocess.run([EXE], cwd=REPO, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=env,
                           timeout=300)
    except subprocess.TimeoutExpired:
        return 0, 0, True
    out = (r.stdout or "") + (r.stderr or "")
    m = re.search(r"\[TEXUNLOCK_TEST\] (\d+)/(\d+) passed", out)
    if not m:
        return 0, 0, True          # no verdict line at all = crashed
    p, t = int(m.group(1)), int(m.group(2))
    return p, t - p, False


def main():
    files = {p: open(os.path.join(REPO, p), encoding="utf-8", newline="").read()
             for p in {m[1] for m in MUTANTS}}

    ok_build, log = build()
    if not ok_build:
        print("REFUSED: the control does not build.\n" + log[-2000:])
        return 2
    bp, bf, bc = run_suite()
    print("control: %d passed, %d FAILED, crashed=%s" % (bp, bf, bc))
    if bc or bf != 0 or bp == 0:
        print("FAIL: the control must run clean with ZERO failures. Every "
              "comparison below would be meaningless.")
        return 1

    print("\n%d mutants:\n" % len(MUTANTS))
    ok = True
    try:
        for name, path, find, repl in MUTANTS:
            src = files[path]
            if src.count(find) != 1:
                print("  FAIL  %-62s anchor appears %d times (expected 1)"
                      % (name[:62], src.count(find)))
                ok = False
                continue
            full = os.path.join(REPO, path)
            open(full, "w", encoding="utf-8", newline="").write(
                src.replace(find, repl, 1))
            built, blog = build()
            if not built:
                # A mutant that does not compile proves the edit landed, not
                # that the suite discriminates. Say so rather than score it.
                print("  FAIL  %-62s DID NOT COMPILE (proves nothing)" % name[:62])
                # Say WHICH error. Without this the transient-link case and a
                # real compile error read exactly the same.
                for line in blog.splitlines():
                    low = line.lower()
                    if "error" in low or "cannot open" in low:
                        print("          | " + line.strip()[:150])
                ok = False
            else:
                p, f, crashed = run_suite()
                if crashed:
                    print("  FAIL  %-62s crashed (proves nothing)" % name[:62])
                    ok = False
                elif f == 0:
                    print("  FAIL  %-62s SURVIVED (%d/%d, 0 failed)"
                          % (name[:62], p, p + f))
                    ok = False
                else:
                    print("  PASS  %-62s -> %d failed" % (name[:62], f))
            open(full, "w", encoding="utf-8", newline="").write(src)
    finally:
        for p, src in files.items():
            open(os.path.join(REPO, p), "w", encoding="utf-8",
                 newline="").write(src)
        build()

    print("\nH1(a) CUBE MUTATION: %s"
          % ("every mutant is detected by a running suite"
             if ok else "SOME MUTANTS SURVIVED - the suite is blind to them"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
