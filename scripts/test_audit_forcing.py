#!/usr/bin/env python3
"""Regression fixtures for scripts/audit_forcing.py gate classification (iter-463).

A manual review of the audit's 55 DEFAULT-ON sites found every one misread by
seven audit bugs (B1..B7, listed at audit_forcing.FIX). Each bug has a tiny
synthetic tree under scripts/tests/audit_forcing/bN_*/kernel/*.cpp. Every
primitive site in a fixture carries its expected class on the same line:

    sr::kernel::writeGuestBE32(va, 1u);  // EXPECT: MASTER [fn=<enclosing function>]

A site without a marker, or a marker without a site, is a failure too, so a
fixture cannot silently stop exercising its shape.

Two checks, both run by default:
  1. every fixture passes with every fix on;
  2. MUTATION: each fix is switched off in turn (audit_forcing.FIX[bN] = False)
     and its own fixture must then FAIL - proof the fixture detects the bug.
     B1 also covers audit_host_threads.master_tokens (a bare-name master
     predicate); its case lives in scripts/test_audit_host_threads.py and is
     mutated here through audit_host_threads.B1_FIX.

Run: python scripts/test_audit_forcing.py [--verbose]
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import audit_forcing as af  # noqa: E402
import audit_host_threads as aht  # noqa: E402

FIXTURES = os.path.join(HERE, "tests", "audit_forcing")
EXPECT_RE = re.compile(r"(?://|/\*)\s*EXPECT:\s*(\w+(?:,\w+)*)(?:\s+fn=(\w+))?")


def fixture_dirs():
    """(fix key, dir name, path): bN_* trees test FIX['BN'], dN_* test FIX['DN']."""
    for name in sorted(os.listdir(FIXTURES)):
        m = re.match(r"([bd])(\d+)_", name)
        if m and os.path.isdir(os.path.join(FIXTURES, name)):
            yield m.group(1).upper() + m.group(2), name, os.path.join(FIXTURES, name)


def marker_files(root):
    """Every file a fixture may carry EXPECT markers in: the scanned dirs plus
    hand-written guest/*.cpp, independent of the FIX switches (a mutation that
    stops scanning a file then shows up as missing sites). guest/ppc/** is
    generated and not classified: a site there has no marker and FAILS."""
    out = []
    for d in af.SCAN_DIRS:
        for dirpath, _, files in os.walk(os.path.join(root, d)):
            out += [os.path.join(dirpath, fn) for fn in sorted(files)]
    g = os.path.join(root, af.GUEST_HAND_DIR)
    if os.path.isdir(g):
        out += [os.path.join(g, fn) for fn in sorted(os.listdir(g))
                if os.path.isfile(os.path.join(g, fn))]
    # D16: hand-written guest code (the stubs source, the indirect-call macro)
    for rel in af.HAND_GUEST_SOURCES + [af.HAND_CONTEXT_H]:
        if os.path.isfile(os.path.join(root, rel)):
            out.append(os.path.join(root, rel))
    return out


def run_fixture(root):
    """[(where, want, got, ok)] for every marker and every site of one fixture.

    Round 4 (D12: every write occurrence is its own site): a line may carry
    several sites. `// EXPECT: X` means every site on the line is X (at least
    one); `// EXPECT: X,Y` lists them in column order, so a line that loses or
    gains an occurrence, or judges one wrongly, fails."""
    _parsed, sites = af.collect(root)
    got = {}
    for s in sorted(sites, key=lambda s: (s[1], s[2], s[7] if len(s) > 7 else 0)):
        got.setdefault((s[1], s[2]), []).append(s)
    want = {}
    for path in marker_files(root):
        rel = os.path.relpath(path, root).replace("\\", "/")
        with open(path, encoding="utf-8") as fh:
            for i, line in enumerate(fh.read().split("\n")):
                m = EXPECT_RE.search(line)
                if m:
                    want[(rel, i + 1)] = (m.group(1).split(","), m.group(2))
    rows = []
    for key in sorted(set(want) | set(got)):
        w, ss = want.get(key), got.get(key)
        where = f"{key[0]}:{key[1]}"
        if not ss:
            rows.append((where, ",".join(w[0]), "(no site)", False))
            continue
        g = ",".join(s[0] for s in ss) + f" fn={ss[0][4]}"
        if w is None:
            rows.append((where, "(no EXPECT marker)", g, False))
            continue
        classes = w[0] if len(w[0]) > 1 else w[0] * len(ss)
        ok = [s[0] for s in ss] == classes and (w[1] is None or all(s[4] == w[1] for s in ss))
        rows.append((where, ",".join(w[0]) + (f" fn={w[1]}" if w[1] else ""),
                     g + f" [{ss[0][5]}]", ok))
    rows += run_generated_expectations(root)
    return rows


GEN_EXPECT_RE = re.compile(r"//\s*EXPECT-GEN:\s*(GATED|UNGATED-FAIL|UNGATED|NOSITE)")


def run_generated_expectations(root):
    """D18: `// EXPECT-GEN: GATED|UNGATED|UNGATED-FAIL|NOSITE` markers inside the
    fixture's generated code (tools/regen/splice_bodies.json bodies, guest/ppc)
    are checked against scan_generated (UNGATED-FAIL = listed as an ungated
    failure, UNGATED = an ungated hand edit that is not a failure)."""
    if not any(os.path.exists(os.path.join(root, p)) for p in (af.SPLICE_BODIES, af.GENERATED_DIR)):
        return []
    _parsed, sites = af.collect(root)
    ctx = af.LAST_CTX.get("ctx")
    writers = af.generated_call_writers(sites)
    rows = []
    for source in ("splice", "ppc"):
        gsites, gung = af.scan_generated(root, source, ctx=ctx, writers=writers)
        by = {}
        for path, ln, sym, gated in gsites:
            by.setdefault((path, ln), []).append(gated)
        fails = {(p, ln) for p, ln, _s in gung}
        for label, lines in af._generated_sources(root, source):
            for i, line in enumerate(lines):
                m = GEN_EXPECT_RE.search(line)
                if not m:
                    continue
                key = (label, i + 1)
                want = m.group(1)
                hits = by.get(key, [])
                if not hits:
                    got = "NOSITE"
                elif key in fails:
                    got = "UNGATED-FAIL"
                elif all(hits):
                    got = "GATED"
                else:
                    got = "UNGATED"
                base = label.split("#")[0]
                if os.path.isabs(base):
                    base = os.path.relpath(base, root)
                where = base.replace("\\", "/") + ("#" + label.split("#", 1)[1] if "#" in label else "") + f":{i + 1}"
                rows.append((where, want, got, got == want))
    return rows


def host_b1_case():
    """The B1 case of scripts/test_audit_host_threads.py: True if it passes."""
    import test_audit_host_threads as tht
    for name, code, reg, want in tht.CASES:
        if name.startswith("B1:"):
            _rc, res = tht.run_case(code, reg)
            got = ",".join(l["status"] for l in sorted(res["launches"], key=lambda l: l["line"]))
            return got == want, got, want
    raise SystemExit("B1 case missing from scripts/test_audit_host_threads.py")


# iter-463 round 3/4: the manual classification of the sites that run in the
# default exe, pinned against the LIVE tree by (file, exact code). PLATFORM
# sites must read ALLOW (their allowlist entry covers them); FORCING and
# DORMANT sites must keep reading UNGATED / DEFAULT-ON until a kernel change
# gates or deletes them - an allowlist anchor whose +-5-line window drifts over
# one of them fails here. A pinned line that no longer exists is skipped (the
# kernel change landed), never failed.
# Round 4 (verifier item 7): lines are compared with comments and a macro's
# trailing `\` stripped - the midasm_hooks pin never matched its commented real
# line, so "23/23" silently skipped the SR_SWEEP_GUARD FORCING pin. A pin whose
# code is still IN the file but matches no line now FAILS (a skipped pin must
# mean the code is gone), and every site on a pinned line must be in class.
FLAGGED = ("UNGATED", "OWNGATE_ON")
LIVE_PINS = [
    # FORCING that ran by default until the iter-463 burndown (items 4, 8):
    # now master-gated - a pin that drops back to UNGATED / DEFAULT-ON fails
    ("guest/ppc_import_glue.cpp", "std::memset(h, 0, inR5);", ("MASTER",)),
    ("kernel/midasm_hooks.cpp", "h[0] = h[1] = h[2] = h[3] = 0;", ("MASTER",)),
    ("kernel/cs_timeout_underflow_fix.cpp", "hostB[0] = (newBegin >> 24) & 0xFF;", ("MASTER",)),
    ("kernel/cs_timeout_underflow_fix.cpp", "hostE[3] =  newEnd        & 0xFF;", ("MASTER",)),
    # DORMANT (never fire; slated for deletion)
    ("guest/ppc_import_glue.cpp", "sr::kernel::writeGuestBE32(outParamVA, blockVA);", FLAGGED),
    ("kernel/guest_context.cpp", "writeGuestBE32(kAllocatorVtableVA, kAllocatorSlot0StubVA);", FLAGGED),
    ("kernel/guest_context.cpp", "writeGuestBE32(kAllocatorVtableSlot4VA, slot0);", FLAGGED),
    ("kernel/guest_context.cpp", "writeGuestBE32(kAllocatorVtableSlot4VA, kAllocatorSlot0StubVA);", FLAGGED),
    ("kernel/guest_context.cpp", "writeGuestBE32(kArrayVA + i * 4, kEntries[i]);", FLAGGED),
    ("kernel/guest_context.cpp", "while (e.str[n] != '\\0') { h[n] = static_cast<uint8_t>(e.str[n]); ++n; }", FLAGGED),
    ("kernel/guest_context.cpp", "h[n] = '\\0';", FLAGGED),
    # PLATFORM duplicate (clobbers the SR_DATA_IMPORTS KeDebugMonitorData cell; delete)
    ("kernel/guest_context.cpp", "writeGuestBE32(kernelData + kKernelProcessInfoPtrOffset, processInfoVA);", FLAGGED),
    ("kernel/guest_context.cpp", "std::memset(processInfo, 0, 256);", FLAGGED),
    # PLATFORM (allowlisted)
    ("kernel/guest_context.cpp", "writeGuestBE32(g_guestBase + kVdGlobalCell, kVdGlobalVarVA);", ("ALLOW",)),
    ("kernel/guest_context.cpp", "writeGuestBE32(g_guestBase + s.cell, s.var);", ("ALLOW",)),
    ("kernel/guest_context.cpp", "std::memcpy(g_guestBase + 0xD0018000u, kGpuClk, 4);", ("ALLOW",)),
    ("kernel/guest_context.cpp", "writeGuestBE32(g_guestBase + kBundleCell, kBundleVA);", ("ALLOW",)),
    ("kernel/guest_context.cpp", "writeGuestBE32(g_guestBase + kXexModuleCell, kXexModuleVarVA);", ("ALLOW",)),
    ("kernel/guest_context.cpp", "std::memset(g_guestBase + kXexHeaderStubVA, 0, 0x100);", ("ALLOW",)),
    ("kernel/guest_context.cpp", "writeGuestBE32(hmod + 0x58, kXexHeaderStubVA);", ("ALLOW",)),
    ("kernel/guest_context.cpp", "std::memcpy(g_guestBase + 0xD0017000u, kHsioLock, sizeof(kHsioLock));", ("ALLOW",)),
    ("kernel/guest_context.cpp", "if (host) std::memset(host, 0, size);", ("ALLOW",)),
    ("kernel/gpu_submit.cpp", "if (uint8_t* p = eng.physToHost(eng.user, phys)) storeBE32(p, rptr);", ("ALLOW",)),
    ("kernel/gpu_ringless.cpp", "sr::kernel::writeGuestBE32(dev + kDevWptr, w1);", ("ALLOW",)),
    ("kernel/gpu_ringless.cpp", "sr::kernel::writeGuestBE32(S + 0x16Cu, value);", ("ALLOW",)),
    ("kernel/gpu_ringless.cpp", "ctx.r5.u64 = 2u;", ("ALLOW",)),
    ("kernel/gpu_ringless.cpp", "sr::kernel::writeGuestBE32(dev + kDevWptr, (wptr + 2u) & mask);", ("ALLOW",)),
    ("kernel/stubs.cpp", 'sr::kernel::GuestCaller gc("VD_ISR");', ("ALLOW",)),
    # ---- round 4: sites the widened scan made visible (D11 / D16 / D17)
    # FORCING: the Sound-flag zeroing thread (fired in 22/22 recent run logs,
    # "[CFG_SND] Sound flag ... 1->0"); iter-463 burndown item 8 moved its launch
    # inside the forcingAllowed() block
    ("kernel/fe_flow_watch.cpp", "h[0] = 0; h[1] = 0; h[2] = 0; h[3] = 0;", ("MASTER",)),
    # DORMANT: walkStaticCtors_impl's FM-list-head clears ("[CTORWALK] SKIPPED"
    # 22/22, "cleared FM list heads" 0/22 recent logs)
    ("kernel/fm_module_walker.cpp",
     "base[0x829A2BB0u] = base[0x829A2BB1u] = base[0x829A2BB2u] = base[0x829A2BB3u] = 0;", FLAGGED),
    ("kernel/fm_module_walker.cpp",
     "base[0x829A2B98u] = base[0x829A2B99u] = base[0x829A2B9Au] = base[0x829A2B9Bu] = 0;", FLAGGED),
    # FORCING: hand-written guest stubs fabricating a return at mid-function VAs
    # (IDA: every one of these addresses is inside another function)
    ("tools/regen/hand/ppc_missing_recomp_stubs.cpp", "PPC_FUNC(sub_824B9560) { ctx.r3.u32 = 0; }", FLAGGED),
    ("tools/regen/hand/ppc_missing_recomp_stubs.cpp", "PPC_FUNC(sub_823A5198) { ctx.r3.u32 = 0; }", FLAGGED),
    ("tools/regen/hand/ppc_missing_recomp_stubs.cpp", "PPC_FUNC(sub_8215BDF8) { ctx.r3.u32 = 0; }", FLAGGED),
    ("tools/regen/hand/ppc_missing_recomp_stubs.cpp", "PPC_FUNC(sub_8215BE38) { ctx.r3.u32 = 0; }", FLAGGED),
    ("tools/regen/hand/ppc_missing_recomp_stubs.cpp", "PPC_FUNC(sub_8215BE40) { ctx.r3.u32 = 0; }", FLAGGED),
    ("tools/regen/hand/ppc_missing_recomp_stubs.cpp", "PPC_FUNC(sub_8215BEA8) { ctx.r3.u32 = 0; }", FLAGGED),
    ("tools/regen/hand/ppc_missing_recomp_stubs.cpp", "ctx.r3.u32 = PPC_LOAD_U8(ctx.r3.u32 + 0x66);", FLAGGED),
    ("tools/regen/hand/ppc_missing_recomp_stubs.cpp", "PPC_STORE_U8(destVA, c);", FLAGGED),
    ("tools/regen/hand/ppc_missing_recomp_stubs.cpp", "ctx.r3.u32 = PPC_LOAD_U32(ctx.r31.u32 + 0x58);", FLAGGED),
    ("tools/regen/hand/ppc_missing_recomp_stubs.cpp", "ctx.r3.u32 = PPC_LOAD_U32(ctx.r31.u32 + 0x50);", FLAGGED),
    ("tools/regen/hand/ppc_missing_recomp_stubs.cpp", "ctx.r3.s64 = 0;", FLAGGED),
    # FORCING / DORMANT: the non-dispatching branches of PPC_CALL_INDIRECT_FUNC
    ("tools/regen/hand/ppc_context.h", "ctx.r3.u32 = 0;", FLAGGED),
    ("tools/regen/hand/ppc_context.h", "PPC_allocator_slot0_stub(ctx, _base);", FLAGGED),
    # PLATFORM (allowlisted)
    ("guest/guest_harness.cpp", "if (sr::kernel::loadXexImage(xexPath, base,", ("ALLOW",)),
    ("tools/regen/hand/ppc_missing_recomp_stubs.cpp", "PPC_STORE_U32(ctx.r11.u32, ctx.r10.u32);", ("ALLOW",)),
    ("kernel/kpcr.cpp", "if (uint8_t* p = guestVAToHost(va)) *p = v;", ("ALLOW",)),
    ("kernel/mod_bridge.cpp", "writeGuestBE32(frame, savedR1);", ("ALLOW",)),
]
# iter-463 burndown (D20): the kernel fake returns. gpu_ring_alloc_fix's pre-claim
# heap route (docs/OVERRIDES.json sub_82467F40) was the OPEN DEFECT counted UNGATED
# on purpose until iter-467 (BANDAIDS.md "U1 CLOSED": VdInitializeRingBuffer takes the
# ring from its physical-address argument, SR_VD_RING_ARG baked ON) put the route
# behind forcingAllowed(). Pinned MASTER now: a change that drops the gate (the route
# running by default again) reads UNGATED / DEFAULT-ON here and fails.
LIVE_PINS += [
    ("kernel/gpu_ring_alloc_fix.cpp", "ctx.r3.u32 = addr;", ("MASTER",)),
    # the indirect-call handler's legacy r3=0 continue: master-only
    ("kernel/indirect_call.cpp", "ctx.r3.u32 = 0;", ("MASTER",)),
]
PIN_STRICT = True        # round-4 comparison; False = the round-3 `line.strip() == code`


def _norm(line):
    """A line's code: comments and a macro continuation `\\` removed."""
    code = re.sub(r"/\*.*?\*/", " ", line.split("//")[0])
    return re.sub(r"\s*\\\s*$", "", code).strip()


def live_pins(sites):
    """[(where, code, want, got, ok)] for every LIVE_PINS line still in the tree."""
    by_line = {}
    for s in sites:
        by_line.setdefault((s[1].replace("\\", "/"), s[2]), []).append(s[0])
    rows = []
    for rel, code, want in LIVE_PINS:
        try:
            text = open(os.path.join(af.ROOT, rel), encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        hit = False
        for i, line in enumerate(text.split("\n")):
            if (_norm(line) == code) if PIN_STRICT else (line.strip() == code):
                hit = True
                got = by_line.get((rel, i + 1), [])
                ok = bool(got) and all(g in want for g in got)
                rows.append((f"{rel}:{i + 1}", code, "/".join(want), ",".join(got) or "(no site)", ok))
        if not hit and PIN_STRICT and code in text:
            # the code is still there but no line matched it exactly: the pin is
            # broken (formatting / a trailing comment), not satisfied
            rows.append((f"{rel}:?", code, "/".join(want), "(pin matches no line)", False))
    return rows


def run_live_pins(extra_allow=None, drop_allow=None, root=None):
    saved = list(af.ALLOWLIST)
    try:
        if drop_allow:
            af.ALLOWLIST[:] = [e for e in af.ALLOWLIST if e[1] != drop_allow]
        if extra_allow:
            af.ALLOWLIST.append(extra_allow)
        _parsed, sites = af.collect(root or af.ROOT)
    finally:
        af.ALLOWLIST[:] = saved
    return live_pins(sites)


def run_anchor_fixture(extra_allow=None, drop_allow=None):
    """Item 9: the anchors_item9 fixture under the real ALLOWLIST (+/- a mutant)."""
    saved = list(af.ALLOWLIST)
    try:
        if drop_allow:
            af.ALLOWLIST[:] = [e for e in af.ALLOWLIST if e[1] != drop_allow]
        if extra_allow:
            af.ALLOWLIST.append(extra_allow)
        return run_fixture(os.path.join(FIXTURES, "anchors_item9"))
    finally:
        af.ALLOWLIST[:] = saved


def host_case(prefix):
    """(all ok, [(name, got, want)]) of the scripts/test_audit_host_threads.py
    cases whose name starts with prefix."""
    import test_audit_host_threads as tht
    out = []
    for name, code, reg, want in tht.CASES:
        if name.startswith(prefix):
            _rc, res = tht.run_case(code, reg)
            got = ",".join(l["status"] for l in sorted(res["launches"], key=lambda l: l["line"]))
            out.append((name, got, want))
    if not out:
        raise SystemExit(f"{prefix} cases missing from scripts/test_audit_host_threads.py")
    return all(g == w for _n, g, w in out), out


def main():
    verbose = "--verbose" in sys.argv
    failed = 0
    print("--- fixtures, all fixes on ---")
    for bug, name, root in fixture_dirs():
        rows = run_fixture(root)
        bad = [r for r in rows if not r[3]]
        failed += len(bad)
        print(f"  {'ok  ' if not bad else 'FAIL'} {name}: {len(rows) - len(bad)}/{len(rows)} sites as expected")
        for where, w, g, ok in rows:
            if verbose or not ok:
                print(f"      {'ok  ' if ok else 'FAIL'} {where}  want {w}  got {g}")
    ok_h, g_h, w_h = host_b1_case()
    failed += not ok_h
    print(f"  {'ok  ' if ok_h else 'FAIL'} audit_host_threads B1 case: got {g_h} want {w_h}")
    # iter-463 round 2: main()'s `--json` branch did `import json`, making json
    # LOCAL to main(), so the band-aid-shipping check's json.load raised,
    # was swallowed, and never fired. Guard both halves of that.
    local_json = "json" in af.main.__code__.co_varnames
    syms = af.load_band_aid_syms(af.ROOT)
    ok_j = not local_json and len(syms) > 0
    failed += not ok_j
    print(f"  {'ok  ' if ok_j else 'FAIL'} band-aid registry load: {len(syms)} band-aid override(s), "
          f"main() binds json locally: {local_json}")

    print("--- mutation: each fix reverted, its fixture must fail ---")
    undetected = 0
    for bug, name, root in fixture_dirs():
        af.FIX[bug] = False
        try:
            rows = run_fixture(root)
        finally:
            af.FIX[bug] = True
        bad = [r for r in rows if not r[3]]
        undetected += not bad
        print(f"  {'caught' if bad else 'MISSED'} {bug} off -> {name}: {len(bad)}/{len(rows)} site(s) misclassified")
        for where, w, g, _ok in bad:
            print(f"      {where}  want {w}  got {g}")
    aht.B1_FIX = False
    try:
        ok_m, g_m, w_m = host_b1_case()
    finally:
        aht.B1_FIX = True
    undetected += ok_m
    print(f"  {'MISSED' if ok_m else 'caught'} B1 off (audit_host_threads) -> got {g_m} want {w_m}")
    # round 4 (verifier item 8): audit_host_threads.gate_of shapes, cases in
    # scripts/test_audit_host_threads.py ("H2:" = unsafe shapes, "H2 control:")
    ok_h2, rows_h2 = host_case("H2")
    failed += not ok_h2
    print(f"  {'ok  ' if ok_h2 else 'FAIL'} audit_host_threads H2 cases: "
          + ", ".join(f"{g}/{w}" for _n, g, w in rows_h2))
    aht.H2_FIX = False
    try:
        _ok, rows_m = host_case("H2:")
    finally:
        aht.H2_FIX = True
    caught = any(g != w for _n, g, w in rows_m)
    undetected += not caught
    print(f"  {'caught' if caught else 'MISSED'} H2 off (audit_host_threads) -> "
          + ", ".join(f"{g}/{w}" for _n, g, w in rows_m))
    # round 4 (verifier item 9): allowlist anchors, on the anchors_item9 fixture
    # under the REAL allowlist; each round-3 broad anchor put back must hide a site
    rows = run_anchor_fixture()
    bad = [r for r in rows if not r[3]]
    failed += len(bad)
    print(f"  {'ok  ' if not bad else 'FAIL'} anchors_item9 (real allowlist): {len(rows) - len(bad)}/{len(rows)}")
    for where, w, g, ok in rows:
        if verbose or not ok:
            print(f"      {'ok  ' if ok else 'FAIL'} {where}  want {w}  got {g}")
    for label, mutant in (("round-3 `fn:writeU8` anchor restored",
                           ("kernel/kpcr.cpp", "fn:writeU8", "mutant")),
                          ("round-3 `// back chain` comment anchor restored",
                           ("kernel/mod_bridge.cpp", "// back chain", "mutant"))):
        mrows = run_anchor_fixture(extra_allow=mutant)
        caught = any(not r[3] for r in mrows)
        undetected += not caught
        print(f"  {'caught' if caught else 'MISSED'} allowlist mutation: {label} -> "
              f"{sum(not r[3] for r in mrows)} fixture site(s) misclassified")

    # round 3: the live-tree classification pins, plus two allowlist mutations
    # (an over-broad function anchor must un-flag a FORCING/DORMANT pin; a
    # dropped platform entry must un-ALLOW its pin).
    print("--- live-tree classification pins (round 3) ---")
    rows = run_live_pins()
    bad = [r for r in rows if not r[4]]
    failed += len(bad)
    print(f"  {'ok  ' if not bad else 'FAIL'} {len(rows) - len(bad)}/{len(rows)} pinned live sites as classified")
    for where, code, w, g, ok in rows:
        if verbose or not ok:
            print(f"      {'ok  ' if ok else 'FAIL'} {where}  want {w}  got {g}  | {code[:60]}")
    for label, kw in (("broad fn:initKernelDataStructures anchor",
                       {"extra_allow": ("kernel/guest_context.cpp", "fn:initKernelDataStructures", "mutant")}),
                      ("VdGlobalDevice entry dropped", {"drop_allow": "kVdGlobalVarVA"}),
                      ("broad fn:cfgSoundOffThread anchor",
                       {"extra_allow": ("kernel/fe_flow_watch.cpp", "fn:cfgSoundOffThread", "mutant")}),
                      ("broad hand-stub file anchor",
                       {"extra_allow": ("tools/regen/hand/", "", "mutant")}),
                      ("XEX image-load entry dropped",
                       {"drop_allow": "line:sr::kernel::loadXexImage(xexPath, base,"})):
        mut = kw.get("extra_allow")
        if mut and not any(
                w == FLAGGED and r[0].startswith(mut[0]) for r, (_rel, _c, w) in
                ((r, next(p for p in LIVE_PINS if p[1] == r[1])) for r in rows)):
            # iter-463 burndown: a broad-anchor mutation needs a FORCING pin in
            # that file to un-flag; once the kernel change deleted every one of
            # them there is nothing left to hide (the anchors_item9 fixture keeps
            # testing anchor breadth)
            print(f"  n/a    allowlist mutation: {label} -> no FORCING/DORMANT pin left in {mut[0]} "
                  f"(the kernel change deleted or master-gated it)")
            continue
        mrows = run_live_pins(**kw)
        caught = any(not r[4] for r in mrows)
        undetected += not caught
        print(f"  {'caught' if caught else 'MISSED'} allowlist mutation: {label} -> "
              f"{sum(not r[4] for r in mrows)} pin(s) misclassified")
    # round 4 (verifier item 7): with the round-3 comparison (`line.strip() ==
    # code`) the commented midasm_hooks line is silently skipped - the pin
    # check must notice that a pinned line went unmatched
    global PIN_STRICT
    PIN_STRICT = False
    try:
        lax = run_live_pins()
    finally:
        PIN_STRICT = True
    lax_where = {r[0] for r in lax}
    strict_where = {r[0] for r in rows}
    lost = sorted(strict_where - lax_where)
    caught = bool(lost)
    undetected += not caught
    print(f"  {'caught' if caught else 'MISSED'} pin mutation: round-3 exact-strip comparison -> "
          f"{len(lost)} pinned line(s) silently skipped ({', '.join(lost[:3])})")

    covered = {b for b, _n, _r in fixture_dirs()}
    missing = sorted(set(af.FIX) - covered)
    if missing:
        print(f"  MISSING fixture for: {', '.join(missing)}")
    print(f"RESULT: {failed} failed fixture site(s), {undetected} undetected mutation(s), "
          f"{len(missing)} fix(es) without a fixture")
    return 1 if (failed or undetected or missing) else 0


if __name__ == "__main__":
    sys.exit(main())
