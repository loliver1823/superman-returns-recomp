#!/usr/bin/env python3
"""test_mem_resources_wiring.py - mem.resources step 2a's hooks sit where the design puts them, and nothing on the draw
path reads the shadow registry (docs/RELEASE_COMPOSITION.md 9.5 item 1; gfx/resource_census.hpp).

    python tools/perf/test_mem_resources_wiring.py [--root <repo>]

A green census proves nothing if a decision event or a boundary is not wired (AGENTS.md: a green suite proves the
bodies, not the wiring), so the sweep is mechanical:
  (1) the page model's decision events - every `meter::kShm` feed of a VALIDATION in gfx/d3d12_pm4_shmem.cpp (the
      range cache's keep, the write watch's accept, the hash decision) has its census hook: modelKeepRange just before
      the range-cache feed, modelPage just before the write-watch accept's counter and feed (the step-1 sweep pins each
      feed to its neighbours), one modelPage per decision branch of pm4ShmValidate (the
      0xA owner, the 0x8 owner, the legacy rule: 3) placed BEFORE the stored hash is replaced, and tagPage before the
      candidate push; the GPU write (pm4ShmRangeWrittenByGpu) carries noteGpuWrite; every hook of that unit but the
      (empty under SR_ORACLE=0) KindScopes sits under #if SR_ORACLE, so the release configuration compiles the model's
      own code (tools/player/oracle_objcmp.py on the worker compares the objects);
  (2) the boundaries - RecordEngine::writeReg's COHER_BASE_HOST branch notes the coherency request; NtReadFile's two
      completions (the synchronous read, the async worker) note the bytes before the IOSB is written; the texture
      Unlock hook notes before its delegate; the CP census prints the census line;
  (3) the fetch kinds - every pm4ShmRequest call site of the request files lies after a KindScope in its function
      (VB in pm4StageVsf, textures, resolves, the scan-out), and the IB conversion feeds ibRead;
  (4) observe-only - outside gfx/resource_census.cpp and the difftest, no source calls memres::registry() / snapshot()
      or reads a census hook's result (every hook returns void; census() is the gate), so no draw decision can depend
      on the shadow: the default and =census decide every upload the same way;
  (5) the build shape - resource_census.cpp in ORACLE_SOURCES and APP_SOURCES, resource_registry.cpp in APP_SOURCES,
      the manifest classes (registry native, census oracle with its markers), the SR_ORACLE=0 branch of the header has
      a no-op for every hook a shipping unit calls.
Fails on the step-1 tree (no hook anywhere). Exit code = failed checks.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FAILS = 0


def check(ok, name):
    global FAILS
    print(("  ok  " if ok else "FAIL: ") + name)
    if not ok:
        FAILS += 1


def read(root, rel):
    try:
        with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as fh:
            return fh.read().splitlines()
    except OSError:
        return []


def find(lines, text, start=0, end=None):
    end = len(lines) if end is None else min(end, len(lines))
    for i in range(start, end):
        if text in lines[i]:
            return i
    return -1


def body(lines, signature):
    """[first, last] line indexes of the column-0 definition whose line contains `signature` (to its column-0 '}')."""
    a = find(lines, signature)
    if a < 0:
        return -1, -1
    for i in range(a + 1, len(lines)):
        if lines[i].startswith("}"):
            return a, i
    return a, len(lines) - 1


def count(lines, text, a, b):
    return sum(1 for i in range(a, b + 1) if text in lines[i])


def oracle_branch(stack, t):
    """Track one preprocessor line `t` on `stack` (one bool per open #if: is this branch SR_ORACLE-only). `#if SR_ORACLE`
    opens an oracle branch and its #else leaves it; `#if !SR_ORACLE` opens the release branch and its #else IS the
    oracle branch (tools/mem-res-wiring-stale: the old tracking called every #else non-oracle). Two limits, neither of
    which changes a verdict in the swept files today: #elif is not tracked (it keeps the branch's kind; the one #elif
    is kernel/native_flush.cpp's non-oracle __has_feature probe), and `#if SR_ORACLE == 0` (any `#if SR_ORACLE...`
    spelling) reads as oracle (right for kernel/stubs.cpp's `#if SR_ORACLE && defined(_WIN32)`)."""
    if t.startswith("#if"):
        neg = t.startswith("#if !SR_ORACLE")
        stack.append(["oracle" if t.startswith("#if SR_ORACLE") else ("release" if neg else "other")])
    elif t.startswith("#else") and stack:
        stack[-1][0] = {"oracle": "release", "release": "oracle"}.get(stack[-1][0], "other")
    elif t.startswith("#endif") and stack:
        stack.pop()


def under_oracle(stack):
    return any(s[0] == "oracle" for s in stack)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(HERE)))
    a = ap.parse_args(argv)
    R = a.root

    print("-- (1) the page model's decision events (gfx/d3d12_pm4_shmem.cpp)")
    S = read(R, "gfx/d3d12_pm4_shmem.cpp")
    check(bool(S), "the source is readable")
    ra, rb = body(S, "bool D3D12Backend::pm4ShmRequestT(")
    # (the step-1 sweep pins each feed to its neighbours - test_shm_meter_wiring.py - so the hooks sit just before)
    rc_feed = find(S, "meter::add(sr::native::meter::kShm, p1 - p0 + 1u);", ra, rb)
    check(rc_feed >= 0 and find(S, "memres::modelKeepRange(p0, p1,", rc_feed - 8, rc_feed) >= 0,
          "range-cache hit: modelKeepRange just before its meter feed")
    # The model's unit is the release configuration's too: its decision hooks are preprocessed out there (#if
    # SR_ORACLE), so its release code is the model's token for token (an empty KindScope is the only thing left).
    stack, unguarded = [], []
    # Step 2b's native protocol is a separate API; only census APIs must compile away. Keep an explicit allow-list so a
    # new census hook is still caught if its SR_ORACLE guard is missing. The frozen-pass A/B helpers (gfx/resource_ab.hpp)
    # are allowed only because their SR_ORACLE=0 forms are inline constants / no-ops - asserted just below - and
    # ResourceSpan is the native request's plain span (gfx/resource_memory.hpp; step 6 I4's pm4ResourceRequestSite).
    native = {"resourceMode", "resourceMemory", "ResourceMemory", "Registry", "isUpload", "ResourceSpan",
              "resourceAbDecision", "resourceAbDual", "resourceDecides", "AbWho"}
    for i, ln in enumerate(S):
        t = ln.strip()
        if t.startswith("#"):
            oracle_branch(stack, t)
        elif "memres::" in ln and "KindScope" not in ln and not under_oracle(stack):
            if set(re.findall(r"memres::(\w+)", ln)) - native:
                unguarded.append(i + 1)
    check(not unguarded, "every census hook of gfx/d3d12_pm4_shmem.cpp but the KindScopes is under #if SR_ORACLE %s"
          % unguarded[:6])
    AB = "\n".join(read(R, "gfx/resource_ab.hpp"))
    ab_off = AB[AB.rfind("#else"):AB.rfind("#endif")] if "#else" in AB else ""
    check(re.search(r"inline bool resourceAbDual\(\) noexcept \{ return false; \}", ab_off) is not None and
          re.search(r"inline bool resourceDecides\(\) noexcept \{ return true; \}", ab_off) is not None and
          re.search(r"inline void resourceAbDecision\(AbWho, uint32_t\) noexcept \{\}", ab_off) is not None,
          "the A/B helpers pm4_shmem calls unguarded are SR_ORACLE=0 constants / no-ops (resource_ab.hpp)")
    RM = "\n".join(read(R, "gfx/resource_memory.hpp"))
    check("struct ResourceSpan { uint32_t phys = 0, len = 0; };" in RM, "ResourceSpan is the plain native span")
    wt_feed = find(S, "m_shmFramePagesClean++;", ra, rb)
    check(wt_feed >= 0 and find(S, "memres::acceptPage(p, mrSite, m_shmEpochOf[p],", wt_feed - 5, wt_feed) >= 0,
          "write-watch accept: acceptPage(keep, its stamp) just before its counter and meter feed")
    push = find(S, "cands.push_back(ShmCand{p, rangeOwn});", ra, rb)
    check(push >= 0 and find(S, "memres::candPage(p, mrSite, m_shmEpochOf[p],", push - 3, push) >= 0,
          "the candidate push is tagged (kind, site, stamp) just before it")
    # COVERAGE (Astra P1): every page of a request is disposed of by a hook - the scope opens before the first early
    # return, every early return before the walk carries its skip hook, every `continue` of the walk its hook.
    scope = find(S, "memres::WalkScope mrReq(p1 - p0 + 1u, false, phys, len);", ra, rb)
    check(scope >= 0, "the request's WalkScope names its byte range (phys, len: the missed class's rng attribute)")
    p1def = find(S, "const uint32_t p1 = uint32_t(std::min<uint64_t>(uint64_t(phys) + len - 1u, kShmBytes - 1u) >> 12);",
                 ra, rb)
    walk = find(S, "if (rcClean && sr::kernel::gww::cleanOwn(p) != rangeOwn) rcClean = false;", ra, rb)
    check(0 <= p1def < scope < walk and not any("return" in S[k] for k in range(p1def + 1, scope)),
          "pm4ShmRequestT opens its WalkScope right after the page range, before any return")
    early = [k for k in range(scope, walk) if re.search(r"\breturn true;", S[k])]
    check(len(early) >= 4, "the early returns before the walk found (%d >= 4: memo, range cache, lean, lazy)" % len(early))
    for k in early:
        hooked = any(re.search(r"\b(mrSkips|memres::modelKeepRange)\(", S[j]) for j in range(max(scope, k - 12), k))
        check(hooked, "the early return at line %d disposes of its pages (mrSkips / modelKeepRange before it)" % (k + 1))
    wend = next((k for k in range(walk, rb) if S[k].startswith("  }")), rb)
    conts = [k for k in range(walk, wend) if S[k].split("//")[0].rstrip().endswith("continue;")]
    check(len(conts) == 3, "the walk's three skips found (epoch, resolve mask, write watch): %d" % len(conts))
    # each skip's own hook, between the previous skip and it (epoch dedup: skipReuse; the prepass's resolve mask:
    # skipDeferred; the write watch's accept: acceptPage)
    prev = walk
    for k, hook in zip(conts, ("skipReuse", "skipDeferred", "acceptPage")):
        hooked = any("memres::%s(" % hook in S[j] for j in range(prev, k))
        check(hooked, "the walk's skip at line %d is disposed of by its own hook (%s)" % (k + 1, hook))
        prev = k + 1
    va, vb = body(S, "void D3D12Backend::pm4ShmValidate(")
    check(count(S, "memres::modelPage(", va, vb) == 3, "pm4ShmValidate: one modelPage per decision branch (3)")
    check(find(S, "memres::WalkScope mrVal(uint32_t(cands.size()), true);", va, va + 8) >= 0,
          "pm4ShmValidate opens its WalkScope over the candidates at its top")
    prot = find(S, "sr::kernel::gww::protectForValidation(", va, vb)
    snap = find(S, "if (mr) memres::snapPage(p);", va, vb)
    check(0 <= snap < prot, "the registry's generations are read (snapPage) before the pages are protected and hashed")
    fp = find(S, "if (mr) memres::failedPage(c.p);", va, vb)
    check(fp >= 0 and find(S, "if (!e.ok) {", fp - 3, fp) >= 0, "a candidate with no host pointer is disposed of")
    for store in ("m_shmHashA[p] = k;", "m_shmHash8[p] = k;", "m_shmHash8[p] = k8;"):
        i = find(S, store, va, vb)
        j = max((k for k in range(va, i if i >= 0 else va) if "memres::modelPage(" in S[k]), default=-1)
        check(i >= 0 and j >= 0 and i - j <= 4, "the hook precedes '%s' (it reads the stored hash first)" % store)
    # The checkers (Astra P1): the write watch's re-hash reports its result, the range cache's loop its results.
    ca_, cb_ = body(S, "void D3D12Backend::shmWtCheck(")
    nc = find(S, "sr::kernel::gww::noteCheck(stale);", ca_, cb_)
    check(nc >= 0 and find(S, "memres::wtChecked(p, stale);", nc, nc + 4) >= 0, "shmWtCheck reports each re-hash")
    check(count(S, "memres::rcMismatch(p);", ra, rb) == 1 and count(S, "memres::rcChecked(p);", ra, rb) == 1,
          "the range cache's check loop reports its mismatches and re-hashes")
    rba, rbb = body(S, "void D3D12Backend::pm4ShmReadback(")
    check(count(S, "memres::rebasePage(p, m_shmEpochOf[p], !m_rtOn);", rba, rbb) == 1,
          "pm4ShmReadback records the stamp it sets (a reuse of it is seen)")
    ga, gb = body(S, "void D3D12Backend::pm4ShmRangeWrittenByGpu(")
    check(count(S, "memres::noteGpuWrite(phys, len)", ga, gb) == 1, "pm4ShmRangeWrittenByGpu notes the GPU write")
    check(any("resource_census.hpp" in ln for ln in S[:80]), "the unit includes the census header")
    stamps = [k for k in range(len(S)) if re.search(r"m_shmEpochOf\[p\] = ", S[k])]
    for k in stamps:
        hooked = any(re.search(r"memres::(acceptPage|candPage|rebasePage)\(", S[j]) for j in range(k + 1, k + 6))
        check(hooked, "the stamp written at line %d is recorded by the census just after it" % (k + 1))

    print("-- (2) the boundaries")
    E = read(R, "gfx/record_engine.cpp")
    wa, wb = body(E, "void RecordEngine::writeReg(")
    br = find(E, "if (reg == 0x0A30u) {", wa, wb)
    check(br >= 0 and find(E, "sr::gfx::memres::noteCoher(getXenosRegisters().read(0x0A31u), value, size);", br,
                           br + 14) >= 0, "RecordEngine::writeReg's COHER_BASE_HOST branch notes the request")
    K = read(R, "kernel/stubs.cpp")
    notes = [i for i, ln in enumerate(K) if "sr::gfx::memres::noteReadFile(" in ln]
    check(len(notes) == 2, "NtReadFile: two notes (the synchronous read, the async worker)")
    for i in notes:
        iosb = find(K, "writeGuestBE32(", i, i + 6)
        check(iosb >= 0, "... line %d precedes the IOSB write (the guest learns of the bytes after the note)" % (i + 1))
    H = read(R, "kernel/hle_api_observe.cpp")
    ua, ub = body(H, "void sub_820FFCC8(PPCContext& __restrict ctx, uint8_t* base) {")
    un = find(H, "sr::gfx::memres::noteUnlock(tex, ", ua, ub)
    dl = find(H, "__imp__sub_820FFCC8(ctx, base);", ua, ub)
    check(un >= 0 and dl > un, "the texture Unlock hook notes (and closes its Lock window) before its delegate")
    la, lb = body(H, "void sub_820FFCB0(PPCContext& __restrict ctx, uint8_t* base) {")
    ld = find(H, "__imp__sub_820FFCB0(ctx, base);", la, lb)
    lk = find(H, "sr::gfx::memres::noteLock(tex, ", la, lb)
    check(0 <= ld < lk, "the texture Lock hook opens the guest write window after its delegate (the lock succeeded)")
    cp_path = ("kernel/gpu_submit.cpp" if os.path.isfile(os.path.join(R, "kernel/gpu_submit.cpp"))
               else "kernel/gpu_command_processor.cpp")
    C = read(R, cp_path)
    ca, cb = body(C, "void census(const char* why) {")
    check(count(C, "sr::gfx::memres::censusLine(why, swaps);", ca, cb) == 1,
          "the CP census prints the census line with its swap count (the first present's evidence)")
    # A player-profile roll (perf_run --config port: SR_LOG=player) keeps a line only by its FORMAT's leading tag.
    # memres2: the inline vertices' allocation (the native BeginVertices / DrawVerticesUP record), the grants, and the
    # kernel-mode host writes that must release a census trap first - each under #if SR_ORACLE.
    def guarded(lines, i):
        depth = []
        for k in range(i):
            t = lines[k].strip()
            if t.startswith("#"):
                oracle_branch(depth, t)
        return under_oracle(depth)
    NF = read(R, "kernel/native_flush.cpp")
    bv = find(NF, "dr.beginVertices(a.prim, a.count, a.a, allocVA, preDraw);")
    up = find(NF, "sr::gfx::memres::noteUpAlloc(Driver::phys(allocVA), 4u * dwords, be32(devH + 0x3290u) != 0u,", bv)
    cur = find(NF, "put32(devH + 13088, dwords);", bv)
    check(0 <= bv < cur < up < bv + 30 and guarded(NF, up),
          "native_flush: the BeginVertices record notes its allocation (pinned or not, the buffer's range), under "
          "#if SR_ORACLE")
    gi = find(K, "if (addr) sr::kernel::physwin::notePhysGrant(addr, sizeBytes);")
    gn = find(K, "sr::gfx::memres::noteGrant(addr, sizeBytes, sr::kernel::getCurrentGuestLR());", gi, gi + 6)
    check(gi >= 0 and gn > gi and guarded(K, gn), "MmAllocatePhysicalMemoryEx notes its grant, under #if SR_ORACLE")
    WW = read(R, "kernel/gpu_write_watch.cpp")
    ha, hb = body(WW, "HostWriteScope::HostWriteScope(uint32_t guestVA, uint32_t len) {")
    hw = find(WW, "sr::gfx::memres::hostWillWrite(guestVA, len);", ha, hb)
    early = find(WW, "if (!len || !g_installed.load(std::memory_order_acquire) || !g_base) return;", ha, hb)
    check(ha >= 0 and 0 <= hw < early and guarded(WW, hw),
          "HostWriteScope releases a census trap first (before its own early return), under #if SR_ORACLE")
    G = "\n".join(read(R, "app/log.cpp"))
    mt = re.search(r"kMilestoneTags\[\] = \{(.*?)\n\};", G, re.S)
    check(bool(mt) and '"MEM_RES"' in mt.group(1), "app/log.cpp: MEM_RES is a milestone tag (kept by SR_LOG=player)")
    U = "\n".join(read(R, "gfx/resource_census.cpp"))
    fmts = re.findall(r"SR_LOG_BOOT\(sr::LogSeverity::\w+,\s*\"([^\"]*)\"", U)
    fetch_formats = [f for f in fmts if f.startswith("[MEM_RES_FETCH] ")]
    census_formats = [f for f in fmts if f not in fetch_formats]
    check(len(census_formats) >= 4 and all(f.startswith("[MEM_RES] ") for f in census_formats),
          "every census log format starts with '[MEM_RES] ' (%d formats)" % len(census_formats))
    # The three admission records, each exactly once; every other [MEM_RES_FETCH] record is one of the payload capture's
    # (payload_config / payload_summary / payload_artifact, added with the admission in 26d31de9). All share the
    # diagnostic prefix, so none can pass for a conservative [MEM_RES] census record (the check above).
    admission = ("armed=", "target_selected=", "origin_finalized=")
    check(all(sum(f.startswith("[MEM_RES_FETCH] " + key) for f in fetch_formats) == 1 for key in admission) and
          all(any(f.startswith("[MEM_RES_FETCH] " + key) for key in admission + ("payload_",)) for f in fetch_formats),
          "three diagnostic admission formats stay distinct from conservative census records")

    print("-- (3) the fetch kinds")
    files = ["gfx/d3d12_pm4_shmem.cpp", "gfx/native_targets.inc", "gfx/d3d12_backend.cpp", "gfx/d3d12_frame_render.cpp"]
    sites = 0
    for f in files:
        L = read(R, f)
        starts = [i for i, ln in enumerate(L) if ln and ln[0] not in " \t#/}" and "(" in ln and not ln.rstrip().endswith(";")]
        for i, ln in enumerate(L):
            if "pm4ShmRequest(" not in ln or ln.lstrip().startswith("//") or "bool D3D12Backend::pm4ShmRequest(" in ln:
                continue
            code = ln.split("//")[0]
            if "pm4ShmRequest(" not in code:
                continue
            sites += 1
            fs = max((s for s in starts if s <= i), default=0)
            scoped = any("memres::KindScope" in L[k] for k in range(fs, i + 1))
            check(scoped, "%s:%d pm4ShmRequest after a KindScope in its function" % (f, i + 1))
    check(sites >= 14, "the request call sites found (%d >= 14)" % sites)
    D = read(R, "gfx/d3d12_frame_render.cpp")
    ib = find(D, "livetrace::memRead(c.idxBasePhys, ib, n * (c.idxFmt32 ? 4u : 2u));")
    check(ib >= 0 and find(D, "memres::ibRead(c.idxBasePhys,", ib, ib + 8) >= 0, "the IB conversion feeds ibRead")

    print("-- (4) observe-only: nothing outside the census reads the shadow")
    readers = []
    for top in ("gfx", "kernel", "app"):
        d = os.path.join(R, top)
        for dp, _, fn in os.walk(d):
            for n in fn:
                if not n.endswith((".cpp", ".hpp", ".inc", ".h")) or n.startswith("resource_census."):
                    continue
                p = os.path.join(dp, n)
                try:
                    txt = open(p, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                if "memres::" not in txt:
                    continue
                for m in re.finditer(r"memres::(registry|snapshot|censusResetForTest|censusSetTestHooks)\s*\(", txt):
                    readers.append("%s: %s" % (os.path.relpath(p, R), m.group(1)))
                for m in re.finditer(r"(=|return|\(|&&|\|\|)\s*(?:sr::gfx::)?memres::(note\w+|acceptPage|candPage|"
                                     r"skipReuse|skipMemo|skipDeferred|modelPage|modelKeepRange|wtChecked|rcMismatch|rcChecked|"
                                     r"snapPage|failedPage|rebasePage|ibRead|censusLine|hostWillWrite|"
                                     r"censusTrapHitForTest)\s*\(", txt):
                    readers.append("%s: uses the result of %s" % (os.path.relpath(p, R), m.group(2)))
    check(not readers, "no shipping or backend source reads the shadow registry or a hook's result %s" % readers[:6])
    disabled = []
    for f in files + ["gfx/record_engine.cpp", "kernel/stubs.cpp", "kernel/hle_api_observe.cpp",
                      cp_path, "kernel/native_flush.cpp", "kernel/gpu_write_watch.cpp",
                      "kernel/gpu_ring_alloc_fix.cpp"]:
        for i, ln in enumerate(read(R, f)):
            if "memres::" in ln and re.search(r"if \((0|false)\)|#if 0", ln):
                disabled.append("%s:%d" % (f, i + 1))
    check(not disabled, "no hook is present but disabled (if (0) / if (false)) %s" % disabled[:6])
    hdr = "\n".join(read(R, "gfx/resource_census.hpp"))
    m = re.search(r"#if SR_ORACLE(.*?)#else(.*?)#endif", hdr, re.S)
    check(bool(m), "the header has an SR_ORACLE branch and a compiled-out one")
    if m:
        on, off = m.group(1), m.group(2)
        for fn in ("census", "noteCoher", "noteReadFile", "noteLock", "noteUnlock", "noteGpuWrite", "acceptPage",
                   "candPage", "skipReuse", "skipMemo", "skipDeferred", "modelKeepRange", "wtChecked", "rcMismatch",
                   "rcChecked",
                   "snapPage", "failedPage", "modelPage", "rebasePage", "ibRead", "censusLine",
                   "noteUpAlloc", "noteGrant", "hostWillWrite"):
            check(re.search(r"\b(?:void|bool) %s\(" % fn, on) is not None and
                  re.search(r"inline (?:void|bool) %s\(" % fn, off) is not None,
                  "%s: declared for the oracle, an inline no-op under SR_ORACLE=0" % fn)
        check("class KindScope" in off and "class WalkScope" in off and
              "inline bool census() noexcept { return false; }" in off,
              "under SR_ORACLE=0 the gate is constant false, KindScope and WalkScope empty")
        diagnostic_only = ("captureFetchDraw", "confirmFetchDraw", "requestedFetchF1", "analyzeFetchDraw",
                           "fetchAutoTargetMode", "noteFetchHudRequest", "fetchTargetForCurrentDraw")
        for fn in diagnostic_only:
            check(re.search(r"\b%s\(" % fn, on) is not None and fn not in off,
                  "%s is declared only for SR_ORACLE=1" % fn)
            sites = []
            for source in ("gfx/d3d12_backend.cpp", "gfx/d3d12_frame_render.cpp", "gfx/d3d12_pm4_shmem.cpp",
                           "kernel/fe_flow_watch.cpp"):
                lines = read(R, source)
                sites.extend(guarded(lines, i) for i, line in enumerate(lines)
                             if "memres::" + fn + "(" in line)
            check(bool(sites) and all(sites), "%s backend calls are all Oracle-only" % fn)
        log_lines = read(R, "app/log.cpp")
        fetch_tag = find(log_lines, '"MEM_RES_FETCH"')
        check(fetch_tag >= 0 and guarded(log_lines, fetch_tag),
              "the player profile retains every sampled fetch record only in Oracle builds")
        backend = read(R, "gfx/d3d12_backend.cpp")
        draw_scope = find(backend, "const memres::FetchAdmissionScope fetchAdmission(c.drawOrdinal);")
        try_draw = find(backend, "auto tryXlatDraw =")
        # The scope is the lambda's first Oracle statement: what may precede it is only the release build's own
        # `#if !SR_ORACLE` prelude (plain assignments) and its `#else` - no request, no return, no call.
        prelude = backend[try_draw + 1:draw_scope] if 0 <= try_draw < draw_scope else ["?"]
        clean = all(t.startswith(("#if !SR_ORACLE", "#else")) or re.fullmatch(r"\w+ = -?\w+;", t) is not None
                    for t in (ln.strip() for ln in prelude) if t)
        check(try_draw >= 0 and try_draw < draw_scope < try_draw + 8 and clean and guarded(backend, draw_scope),
              "actual draw admission owns the Oracle target sequence before requests and all early returns")
        shmem = read(R, "gfx/d3d12_pm4_shmem.cpp")
        flush = find(shmem, "void D3D12Backend::pm4ShmCollectFlush()")
        validation = find(shmem, "const memres::FetchAdmissionScope fetchValidation(0);")
        flush_end = find(shmem, "void D3D12Backend::pm4ShmValidate(")
        locks = [i for i in range(flush, flush_end) if "lock_guard" in shmem[i]]
        check(flush >= 0 and flush < validation < flush_end and guarded(shmem, validation) and
              locks and all(validation < i for i in locks),
              "deferred validation owns a fresh batch sequence and finalizes after every request lock")
        flow = read(R, "kernel/fe_flow_watch.cpp")
        apt = find(flow, "void shimAptLoadByPath(")
        hud = find(flow, "sr::gfx::memres::noteFetchHudRequest(ctx.r3.u32, base);")
        check(apt >= 0 and apt < hud < apt + 5 and guarded(flow, hud),
              "exact HUD observation precedes the APT logging gates and caps")
        census_src = "\n".join(read(R, "gfx/resource_census.cpp"))
        # The selection block: from the miss condition (d5cdbe1e added `unclassified &&` to it) to the per-cell count.
        # Its only lock is the census-local g_payloadSelection; the grant mutex (phys_window.cpp g_grantMu, taken by
        # ownersOfSpan) must not be reached from it.
        sel = re.search(r"if \(!po &&[^{]*kind == Kind::kVb && site == Site::kDrain && rg == Rng::kMoved", census_src)
        sel_end = census_src.find("const uint32_t cn =", sel.start()) if sel else -1
        selected = census_src[sel.start():sel_end] if sel and sel_end > 0 else ""
        locks = re.findall(r"std::(?:lock_guard|unique_lock|scoped_lock|shared_lock)<[^>]*>\s+\w+\((\w+)\)", selected)
        check(bool(selected) and all(x in selected for x in ("site == Site::kDrain", "rg == Rng::kMoved",
                                                             "t_fetchSequence", "g_fetchTarget.select(origin)")) and
              "ownersOfSpan(" not in selected and "g_grantMu" not in selected and
              all(m == "g_payloadSelection" for m in locks),
              "origin selection requires the unchanged moved VB drain miss and does not take the grant mutex")
        shmem_src = "\n".join(read(R, "gfx/d3d12_pm4_shmem.cpp"))
        check('if (!batch.resourceFetchOriginMatched) unavailable = "selected_miss_origin_changed_or_unknown";'
              in shmem_src,
              "consumption rejects a selected origin whose owner or grant no longer corresponds")
        for fn in re.findall(r"\n(?:void|bool|Registry\*) (\w+)\(", on):
            if fn in ("snapshot", "registry", "censusResetForTest", "censusSetTestHooks") + diagnostic_only:
                continue
            check(re.search(r"\b(void|bool) %s\(" % fn, off) is not None, "%s has its SR_ORACLE=0 no-op" % fn)

    print("-- (5) the build shape")
    cm = "\n".join(read(R, "CMakeLists.txt"))
    orc = re.search(r"set\(ORACLE_SOURCES(.*?)\)", cm, re.S)
    app = re.search(r"set\(APP_SOURCES(.*?)\n\)", cm, re.S)
    check(bool(orc) and "gfx/resource_census.cpp" in orc.group(1), "resource_census.cpp in ORACLE_SOURCES")
    check(bool(app) and "gfx/resource_census.cpp" in app.group(1) and "gfx/resource_registry.cpp" in app.group(1),
          "both units in APP_SOURCES (the dev build links the census)")
    check("tools/difftest/mem_resources_test.cpp" in cm, "the difftest suite is built")
    try:
        man = json.load(open(os.path.join(R, "docs/release_composition.json"), encoding="utf-8"))
    except (OSError, ValueError):
        man = {}
    src = {e.get("path"): e for e in man.get("sources", []) if isinstance(e, dict)}
    con = {e.get("path"): e for e in man.get("content", []) if isinstance(e, dict)}
    check(src.get("gfx/resource_registry.cpp", {}).get("class") == "native", "manifest: the registry unit is native")
    rc = src.get("gfx/resource_census.cpp", {})
    check(rc.get("class") == "oracle" and "[MEM_RES]" in rc.get("markers", []) and
          "SR_MEM_RESOURCES\u0000" in rc.get("markers", []), "manifest: the census unit is oracle, with its markers")
    check(con.get("gfx/resource_registry.hpp", {}).get("class") == "native" and
          con.get("gfx/resource_census.hpp", {}).get("class") == "oracle", "manifest: the two headers")
    dm = "\n".join(read(R, "tools/difftest/difftest_main.cpp"))
    check('"--mem-resources-test"' in dm and "runMemResourcesTest()" in dm, "difftest dispatches --mem-resources-test")

    print("-- (6) memres5: the index bound's hooks and the grants' windows")
    D = read(R, "gfx/d3d12_frame_render.cpp")
    xa, xb = body(D, "bool D3D12Backend::pm4StageVsf(")
    ds = find(D, "const memres::DrawScope mrDraw(c.vsUcodeHash, c.idxBasePhys != 0u, mib, c.vertexCount,", xa, xb)
    loop = find(D, "for (const uint32_t slot : vs.fetchSlots) {", xa, xb)
    ss = find(D, "const memres::SlotScope mrSlot(slot);", xa, xb)
    req = find(D, "pm4ShmRequest(phys, size);", max(ss, xa), xb)
    check(0 <= ds < loop < ss < req and guarded(D, ds) and guarded(D, ss),
          "pm4StageVsf: the draw's DrawScope (its index stream) before the fetch loop, each slot's SlotScope before its "
          "requests, both under #if SR_ORACLE")
    # memres5's lines: the draw's scope and the fetch loop, up to the index conversion (whose step-2a IB census, ibRead
    # behind memres::census(), predates them and compiles to nothing under SR_ORACLE=0: the object comparison's)
    conv = find(D, "const uint32_t n = c.vertexCount;", max(ss, xa), xb)
    unguarded = [k + 1 for k in range(xa, (conv if conv > 0 else xb) + 1) if "memres::" in D[k] and
                 "KindScope" not in D[k] and not guarded(D, k)]
    check(conv > ss and not unguarded,
          "pm4StageVsf: every census line before the index conversion but the KindScope under #if SR_ORACLE %s"
          % unguarded[:6])
    # memres6: the DrawScope carries the draw's VS constants as the constant upload reads them (a tracked command's merged
    # file, else its own copy - d3d12_backend.cpp's upload uses the same expression) and its fetch constants
    stmt = " ".join(D[k].strip() for k in range(ds, min(ds + 6, xb))) if ds >= 0 else ""
    stmt = stmt[:stmt.find(";") + 1] if ";" in stmt else stmt
    B = read(R, "gfx/d3d12_backend.cpp")
    check(stmt.endswith("c.primResetIndex, c.constTracked ? m_cvCons : c.aluVs, c.fetchRaw);") and
          find(B, "const uint32_t* aluVs = c.constTracked ? m_cvCons : c.aluVs;") >= 0,
          "pm4StageVsf: the DrawScope hands the census the draw's VS constants (as uploaded) and its fetch constants: %s"
          % stmt[-90:])
    im = find(E, "lastVsUcodeHash = h;")
    nv = find(E, "sr::gfx::memres::noteVsUcode(h, ucodeHost, ucodeBytes);", im, im + 8)
    check(im >= 0 and nv > im and guarded(E, nv), "RecordEngine's IM_LOAD hands the VS ucode to the census, under #if SR_ORACLE")
    GA = read(R, "kernel/gpu_ring_alloc_fix.cpp")
    for anchor, what in (("if (addr) sr::kernel::physwin::notePhysGrant(addr, (uint32_t)size);", "its physical route"),
                         ("if (addr) sr::kernel::physwin::noteHeapRoutedGrant(addr, (uint32_t)size);",
                          "its heap-routed (0x8) grants")):
        g0 = find(GA, anchor)
        g1 = find(GA, "sr::gfx::memres::noteGrantWindow(addr, (uint32_t)size);", g0, g0 + 5) if g0 >= 0 else -1
        check(g0 >= 0 and g1 > g0 and guarded(GA, g1),
              "the XMemAlloc override notes the window of %s, under #if SR_ORACLE" % what)
    na, nb = body(U.splitlines(), "void noteGrant(uint32_t va, uint32_t bytes, uint32_t lr) noexcept {")
    check(na >= 0 and count(U.splitlines(), "noteGrantWindow(va, bytes);", na, nb) == 1,
          "MmAllocatePhysicalMemoryEx's grant notes its window (noteGrant calls noteGrantWindow)")
    if m:
        check("class DrawScope" in off and "class SlotScope" in off,
              "under SR_ORACLE=0 the DrawScope and the SlotScope are empty")

    print("mem.resources wiring test: %s" % ("PASS" if not FAILS else "%d FAILED" % FAILS))
    return FAILS


if __name__ == "__main__":
    sys.exit(main())
