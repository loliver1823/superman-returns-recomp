"""Mutation receipts for the texture census (iter-458, Astra 068/069/070).

Each mutant is one or more exact substitutions in one file; it is KILLED only
when a NAMED check fails in one of its named killers:
  difftest  build/difftest.exe --tex-census-test       ("FAIL: <id> ...")
  exe       SR_HLE_TEX_UNLOCK_TEST=1 SupermanReturns.exe ("[TEXUNLOCK_TEST] FAIL: C14 ...")
  taps      python tools/test_txc_taps.py              ("VIOLATION R<n>: ...")
  pyself    python tools/tex_census.py --self-test     ("FAIL:  <name> ...")
A crash, a timeout or a nonzero exit WITHOUT a named failure line is never a
kill (Astra 070): it is reported (CRASH/TIMEOUT) and the mutant SURVIVES.

The BASELINE is certified first, on binaries built from the tree under test:
every killer must exit 0 AND print its completion marker with zero failures
([TXC_TEST] N passed, 0 failed / [TEXUNLOCK_TEST] N/N passed / [TXC_TAPS] 0
violations / [TEX_CENSUS_SELFTEST] 0 failed) AND print no failure line.
Every touched file is restored byte-exact after each mutant (retrying writes)
and md5-guarded at the end; both binaries are rebuilt from the restored tree.

Usage: python tools/mutate_tex_census.py [--set census|hr1] [--out receipts.txt] [--only name,...] [--skip-exe]
       python tools/mutate_tex_census.py --check-anchors   (every anchor of every set matches exactly once)
  --set hr1: the SR_GPU_PHYS_ROUTE mutants (Astra 071), killed by
             build/difftest.exe --gpu-phys-route-test ("FAIL: <id> ...").
Run from the repo root on the tree that is being certified.
"""
import hashlib
import os
import re
import subprocess
import sys
import time

TC = 'kernel/tex_census.cpp'
PW = 'kernel/phys_window.cpp'
ST = 'kernel/stubs.cpp'
GC = 'kernel/guest_context.cpp'
GF = 'gfx/hle_video.cpp'
HO = 'kernel/hle_api_observe.cpp'
PY = 'tools/tex_census.py'
DIFFTEST = os.path.abspath('build/difftest.exe')
EXE = os.path.abspath('build/SupermanReturns.exe')

# The open edge where recording goes live, and the close edge (hle_video.cpp).
OPEN_EDGE = ('    if (sr::kernel::txc::enabled()) {\n'
             '      s_txcWin = sr::kernel::txc::WindowCounters{};\n'
             '      sr::kernel::txc::onWindowEdge(true, s_txcExec.load(std::memory_order_relaxed), s_txcWin,\n'
             '                                    R.records);\n'
             '    }\n'
             '    return;')
CLOSE_EDGE = ('      if (sr::kernel::txc::enabled())   // before the close hook flushes the census\n'
              '        sr::kernel::txc::onWindowEdge(false, s_txcExec.load(std::memory_order_relaxed), s_txcWin,\n'
              '                                      R.records);\n'
              '      noteRecorderClosed();')
DECODE_APPEND = ('  if (void (*hook)() = g_decodeHook.load(std::memory_order_relaxed)) hook();\n'
                 '  std::lock_guard<std::mutex> lk(g_mu);\n'
                 '  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();   // at the append\n')

# (name, file, killers, expected tokens, [(old, new), ...])
MUTANTS = [
    # ── tex_census.cpp ──────────────────────────────────────────────────────
    ('seq-not-consumed-on-drop', TC, ['difftest'], ['FAIL: V1 '], [
        ('  const uint64_t seq = ++g_seq;          // a dropped event still consumes a seq\n'
         '  if (g_flushed) return;                 // the acquisition has ended\n'
         '  if (g_ev.size() >= g_cap) { g_overflow++; return; }',
         '  if (g_flushed) return;\n  if (g_ev.size() >= g_cap) { g_overflow++; return; }\n'
         '  const uint64_t seq = ++g_seq;')]),
    ('record-after-flush', TC, ['difftest'], ['FAIL: F5 ', 'FAIL: F6 '], [
        ('  if (g_flushed) return;                 // the acquisition has ended\n', '')]),
    ('overflow-not-counted', TC, ['difftest'], ['FAIL: V1 '], [('{ g_overflow++; return; }', '{ return; }')]),
    ('issued-as-suppressed', TC, ['difftest'], ['FAIL: D1 '], [
        ('    decision = kIssued;\n', '    decision = kSuppressedChanged;\n')]),
    ('owner-digest-ignored', TC, ['difftest'], ['FAIL: S1 ', 'FAIL: S5 '], [
        ('it->second.ownerDigest != o.setDigest || ', '')]),
    ('content-ignored', TC, ['difftest'], ['FAIL: S4 '], [
        (' || it->second.content != content)', ')')]),
    ('no-baseline-as-changed', TC, ['difftest'], ['FAIL: N1 ', 'FAIL: K0 '], [
        ('    decision = kNoBaseline;', '    decision = kSuppressedChanged;')]),
    ('unchanged-recorded', TC, ['difftest'], ['FAIL: S2 '], [
        ('    return;   // unchanged suppressed bind: nothing new to record',
         '    decision = kSuppressedChanged;')]),
    ('no-flag-on-change', TC, ['difftest'], ['FAIL: K1 '], [
        ('if (decision == kSuppressedChanged || o.nLive > 1 || (flags & kDecAliased))',
         'if (o.nLive > 1 || (flags & kDecAliased))')]),
    ('unlock-carries-apply-ordinal', TC, ['difftest'], ['FAIL: W2 '], [
        ('  const uint32_t ord = source == kFromUpload ? g_applyOrd + 1 : 0u;',
         '  const uint32_t ord = g_applyOrd + 1;')]),
    ('upload-resident-ordinal-off-by-one', TC, ['difftest'], ['FAIL: D2 ', 'FAIL: K1 '], [
        ('  const uint32_t ord = source == kFromUpload ? g_applyOrd + 1 : 0u;',
         '  const uint32_t ord = source == kFromUpload ? g_applyOrd : 0u;')]),
    ('window-ignored', TC, ['difftest'], ['FAIL: U1 '], [
        ('  if (!window && !g_flag[key >> 12].load(std::memory_order_relaxed)) return;\n', '')]),
    ('budget-off-by-one', TC, ['difftest'], ['FAIL: K1 '], [
        ('g_drawBudget[key]++ >= kDrawPerKeyOutside', 'g_drawBudget[key]++ > kDrawPerKeyOutside')]),
    ('budget-in-window', TC, ['difftest'], ['FAIL: F2 '], [
        ('    if (!window && g_drawBudget[key]++ >= kDrawPerKeyOutside) return;',
         '    if (g_drawBudget[key]++ >= kDrawPerKeyOutside) return;')]),
    ('window-bit-dropped', TC, ['difftest'], ['FAIL: W2 ', 'FAIL: Q2 ', 'FAIL: F2 '], [
        ('((window ? 1u : 0u) << 11)', '0u')]),
    # Astra 070: P1 is read inside g_mu, after the observation, at the append.
    ('decode-P1-is-P0', TC, ['difftest', 'taps'], ['FAIL: Q1 ', 'FAIL: D1 ', 'VIOLATION R10'], [
        ('  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();   // at the append',
         '  const uint64_t p1 = c.p0;')]),
    ('decode-P1-before-the-observation-ends', TC, ['difftest', 'taps'], ['FAIL: Q3 ', 'VIOLATION R10'], [
        (DECODE_APPEND,
         '  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();\n'
         '  if (void (*hook)() = g_decodeHook.load(std::memory_order_relaxed)) hook();\n'
         '  std::lock_guard<std::mutex> lk(g_mu);\n')]),
    ('decode-outside-g_obs', TC, ['difftest', 'taps'], ['FAIL: Q4 ', 'VIOLATION R10'], [
        ('  std::lock_guard<std::mutex> obs(g_obs);\n', '')]),
    ('decode-P0-zero', TC, ['difftest'], ['FAIL: D1 '], [
        ('                  c.p0, p1, o.setDigest});', '                  0, p1, o.setDigest});')]),
    ('issued-content-reread', TC, ['difftest'], ['FAIL: D1 '], [
        ('issued ? t_lastPubHash : 0u,',
         'issued ? sampleGuest(aliasBase(window) | (base & kMask), span) : 0u,')]),
    ('issued-fifo-is-the-clock', TC, ['difftest'], ['FAIL: D1 '], [
        ('issued ? t_lastPubFifo : 0u};', 'issued ? sr::gfx::hle::txcFifoPublished() : 0u};')]),
    ('suppressed-hashes-the-other-alias', TC, ['difftest'], ['FAIL: S1 ', 'FAIL: S3 '], [
        ('      content = sampleGuest(aliasBase(window) | base, span);',
         '      content = sampleGuest(0xA0000000u | base, span);')]),
    ('descriptor-key-lossy', TC, ['difftest'], ['FAIL: K0 '], [
        ('    return base == o.base && fmt == o.fmt && dims == o.dims;',
         '    return base == o.base && (((uint64_t)fmt << 24) ^ dims) == (((uint64_t)o.fmt << 24) ^ o.dims);'),
        ('    for (uint32_t v : {k.base, k.fmt, k.dims}) { h ^= v; h *= 1099511628211ull; }',
         '    for (uint64_t v : {(uint64_t)k.base, ((uint64_t)k.fmt << 24) ^ k.dims}) { h ^= v; h *= 1099511628211ull; }')]),
    ('unstable-flag-dropped', TC, ['difftest'], ['FAIL: Z2 '], [
        ('    if (o.gen != c.gen0) flags |= kDecOwnersUnstable;\n', '')]),
    ('aliasing-is-coverage', TC, ['difftest'], ['FAIL: O2 ', 'FAIL: O4 '], [
        ('  if (o.aliasedPairs) flags |= kDecAliased;', '  if (o.nLive > 1) flags |= kDecAliased;')]),
    ('draw-residents-2d-only', TC, ['difftest'], ['FAIL: W2 '], [
        ('    for (uint32_t b = 0; b < kBands; b++) {\n      auto it = g_resident[b].find(key);',
         '    for (uint32_t b = 0; b < 1; b++) {\n      auto it = g_resident[b].find(key);')]),
    ('publish-payload-hash-dropped', TC, ['difftest'], ['FAIL: D1 ', 'FAIL: E1 '], [
        ('  const uint64_t ph = probeHash(payload, bytes);', '  const uint64_t ph = 0;')]),
    ('flushed-only-on-success', TC, ['difftest'], ['FAIL: F6 '], [
        ('  g_flushed = true;   // the acquisition ends here whether or not the write lands\n', ''),
        ('  fc.ok = ok;\n}', '  g_flushed = ok;\n  fc.ok = ok;\n}')]),
    ('header-count-wrong', TC, ['difftest'], ['FAIL: F4 '], [
        ('  putU64(hdr, (uint64_t)g_ev.size());', '  putU64(hdr, (uint64_t)g_ev.size() + 1);')]),
    ('no-cost-mark', TC, ['difftest'], ['FAIL: F3 '], [
        ('  appendLocked(Ev{kMark, kMarkCost,', '  if (false) appendLocked(Ev{kMark, kMarkCost,')]),
    ('window-counters-swapped', TC, ['difftest'], ['FAIL: F2 '], [
        ('execIdx,\n                  n.bindingsSeen, n.whiteoutSkips});',
         'execIdx,\n                  n.whiteoutSkips, n.bindingsSeen});')]),
    ('window-edge-record-dropped', TC, ['difftest'], ['FAIL: F2b '], [
        ('(uint32_t)recordIndex,\n', '0u,\n')]),
    ('probe-uncached', TC, ['difftest'], ['FAIL: H1 '], [
        ('!sr::kernel::hostRangeReadableCached(p, n)', '!sr::kernel::hostRangeReadable(p, n)')]),
    # Astra 070: observations in flight at the write are waited for and recorded.
    ('flush-does-not-wait', TC, ['difftest', 'taps'], ['FAIL: IF1 ', 'VIOLATION R10'], [
        ('  while (g_inflight.load(std::memory_order_acquire) > 0) {', '  while (false) {')]),
    ('inflight-not-recorded', TC, ['difftest'], ['FAIL: IF1 '], [
        ('(uint32_t)(inflight > 0 ? inflight : 0), (uint32_t)fc.waitedMs,', '0u, (uint32_t)fc.waitedMs,')]),
    ('timeout-not-recorded', TC, ['difftest'], ['FAIL: IF1 '], [
        ('fc.timedOut ? 1u : 0u, 0, 0, 0,', '0u, 0, 0, 0,')]),
    ('bindbegin-not-counted', TC, ['difftest', 'taps'], ['FAIL: IF1 ', 'VIOLATION R10'], [
        ('  g_inflight.fetch_add(1, std::memory_order_acq_rel);\n', '')]),
    ('token-end-not-counted', TC, ['difftest', 'taps'], ['FAIL: IF2 ', 'VIOLATION R10'], [
        ('  if (on) g_inflight.fetch_sub(1, std::memory_order_acq_rel);', '  (void)on;')]),
    # Structural only (no behavioral race test for grant events): R10.
    ('grant-P1-before-the-mutex', TC, ['taps'], ['VIOLATION R10'], [
        ('  std::lock_guard<std::mutex> lk(g_mu);\n  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();\n'
         '  appendLocked(Ev{kGrantReg',
         '  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();\n  std::lock_guard<std::mutex> lk(g_mu);\n'
         '  appendLocked(Ev{kGrantReg')]),
    # ── phys_window.cpp ─────────────────────────────────────────────────────
    ('no-grant-reg-event', PW, ['difftest'], ['FAIL: G1 '], [
        ('  if (txcOn) txc::onGrantReg((uint32_t)kind, va, size, serial, txcP0);\n', '')]),
    ('grant-P0-zero', PW, ['difftest'], ['FAIL: G1 '], [
        ('  if (txcOn) txc::onGrantReg((uint32_t)kind, va, size, serial, txcP0);',
         '  if (txcOn) txc::onGrantReg((uint32_t)kind, va, size, serial, 0);')]),
    ('no-reclaim-event', PW, ['difftest'], ['FAIL: G2 '], [
        ('      if (txc::enabled()) txc::onGrantRel(2, va, serial, 2, txcP0);   // reclaimed\n', '')]),
    ('no-release-event-phys', PW, ['difftest'], ['FAIL: R1 '], [
        ('      if (txc::enabled()) txc::onGrantRel(2, va, it->second.serial, 1, txcP0);   // released-retained\n', '')]),
    ('no-release-event-heap', PW, ['difftest'], ['FAIL: R2 '], [
        ('        if (txc::enabled()) txc::onGrantRel(1, va, ht->second.serial, 1, txcP0);\n', '')]),
    ('gen-not-bumped-on-register', PW, ['difftest', 'taps'], ['FAIL: Z2 ', 'VIOLATION R9'], [
        ('  registryOf(kind)[va] = GrantRec{va + size, serial, 0};\n  g_grantGen.fetch_add(1, std::memory_order_acq_rel);\n',
         '  registryOf(kind)[va] = GrantRec{va + size, serial, 0};\n')]),
    ('disjoint-check-counting-only', PW, ['difftest'], ['FAIL: O3 '], [
        ('  if (counting || txcOn) registryDisjointCheckLocked(kind, va);',
         '  if (counting) registryDisjointCheckLocked(kind, va);')]),
    ('pairwise-overlap-is-coverage', PW, ['difftest'], ['FAIL: O2 ', 'FAIL: O4 '], [
        ('    if (any && v.lo < reach) o.aliasedPairs++;', '    if (any) o.aliasedPairs++;')]),
    ('aliasing-sweep-capped-at-64', PW, ['difftest'], ['FAIL: O5 '], [
        ('  for (const Iv& v : iv) {',
         '  for (const Iv& v : std::vector<Iv>(iv.begin(), iv.begin() + std::min<size_t>(iv.size(), 64))) {')]),
    ('owners-outside-lock', PW, ['difftest'], ['FAIL: Z1 '], [
        ('  std::shared_lock<std::shared_mutex> lk(g_grantMu);\n  fn(ownersOfSpanLocked(p, span), ctx);',
         '  std::shared_lock<std::shared_mutex> lk(g_grantMu);\n  const SpanOwners o = ownersOfSpanLocked(p, span);\n'
         '  lk.unlock();\n  fn(o, ctx);')]),
    ('no-flush-at-close', PW, ['difftest'], ['FAIL: F1 '], [
        ('  if (txc::enabled()) txc::flush("recorder-closed");\n', '')]),
    # ── stubs.cpp / guest_context.cpp ───────────────────────────────────────
    ('free-release-needs-uniq', ST, ['difftest'], ['FAIL: R1 '], [
        ('(sr::kernel::physwin::uniqCensusEnabled() || sr::kernel::txc::enabled())',
         '(sr::kernel::physwin::uniqCensusEnabled())')]),
    ('cached-ignores-force', GC, ['difftest'], ['FAIL: H1 '], [
        ('inRes && (force || hrr::cacheOn());', 'inRes && hrr::cacheOn();')]),
    ('plain-uses-cache', GC, ['difftest'], ['FAIL: H1 '], [
        ('  return hrrQuery(p, bytes, false);', '  return hrrQuery(p, bytes, true);')]),
    # ── hle_video.cpp ───────────────────────────────────────────────────────
    ('publish-index-off-by-one', GF, ['difftest'], ['FAIL: E1 '], [
        ('                                        at + i + 1, pay);', '                                        at + i, pay);')]),
    ('publish-count-not-stored', GF, ['difftest'], ['FAIL: E1 ', 'FAIL: E2 '], [
        ('      s_txcPub.store(at + count, std::memory_order_release);\n', '')]),
    ('publish-no-payload', GF, ['difftest', 'taps'], ['FAIL: E1 ', 'VIOLATION R6'], [
        ('                                        at + i + 1, pay);', '                                        at + i + 1, nullptr);')]),
    ('publish-tap-dropped', GF, ['difftest'], ['FAIL: E1 '], [
        ('        if (cmds[i].type == RenderCommandType::TextureUpload) {\n'
         '          const auto& u = cmds[i].textureUpload;\n'
         '          // The IMMUTABLE',
         '        if (cmds[i].type == RenderCommandType::Present) {\n'
         '          const auto& u = cmds[i].textureUpload;\n'
         '          // The IMMUTABLE')]),
    ('prologue-no-exec', GF, ['difftest'], ['FAIL: E1 ', 'FAIL: F2 '], [
        ('  s_txcExec.fetch_add(1, std::memory_order_relaxed);\n  if (txcIsDrawCmd(c.type) && traceRecorderOpen())',
         '  if (txcIsDrawCmd(c.type) && traceRecorderOpen())')]),
    ('prologue-no-window-count', GF, ['difftest', 'taps'], ['FAIL: F2 ', 'VIOLATION R4'], [
        ('  if (txcIsDrawCmd(c.type) && traceRecorderOpen()) s_txcWin.drawCmds++;\n', '')]),
    ('draw-predicate-without-UP', GF, ['difftest', 'taps'], ['FAIL: F2 ', 'VIOLATION R4'], [
        ('         t == RenderCommandType::DrawVerticesUP || t == RenderCommandType::BeginVertices;',
         '         t == RenderCommandType::BeginVertices;')]),
    ('prologue-counts-isDrawType', GF, ['difftest', 'taps'], ['FAIL: F2 ', 'VIOLATION R4'], [
        # the old isDrawType() predicate, inlined (it is defined after the prologue)
        ('if (txcIsDrawCmd(c.type) && traceRecorderOpen())',
         'if ((c.type == RenderCommandType::DrawVertices || c.type == RenderCommandType::DrawIndexedVertices ||\n'
         '       c.type == RenderCommandType::BeginVertices) && traceRecorderOpen())')]),
    ('apply-witness-dropped', GF, ['difftest', 'taps'], ['FAIL: E1 ', 'VIOLATION R3'], [
        ('          if (sr::kernel::txc::enabled())\n            sr::kernel::txc::onTexApply(',
         '          if (false)\n            sr::kernel::txc::onTexApply(')]),
    ('drawtap-bindings-not-counted', GF, ['difftest', 'taps'], ['FAIL: F2 ', 'VIOLATION R5'], [
        ('    if (txcWin && tk) s_txcWin.bindingsSeen++;\n', '')]),
    ('drawtap-dim-dropped', GF, ['difftest'], ['FAIL: W3 ', 'FAIL: K1 '], [
        ('s_rs.sampValid && s_rs.texDim[s] <= 3u, s_rs.texDim[s] & 3u,', 's_rs.sampValid, 0u,')]),
    ('drawtap-stacked-dropped', GF, ['difftest', 'taps'], ['FAIL: W3 ', 'VIOLATION R5'], [
        ('                                 s_rs.texDepth[s] > 1u, s_txcFetchDims[s], q.psHash,',
         '                                 false, s_txcFetchDims[s], q.psHash,')]),
    ('drawtap-fetch-base-is-the-key', GF, ['difftest'], ['FAIL: W3 '], [
        ('onDrawTex((uint32_t)s_cov.issued, s, tk, s_rs.texBase[s],',
         'onDrawTex((uint32_t)s_cov.issued, s, tk, tk,')]),
    ('fetch-fmt-not-decoded', GF, ['difftest'], ['FAIL: K1 '], [
        ('                  s_txcFetchFmt[t] = uint8_t(d1 & 0x3Fu);\n', '')]),
    ('open-edge-dropped', GF, ['difftest', 'taps'], ['FAIL: W1 ', 'FAIL: F2 ', 'VIOLATION R7'], [
        ('      sr::kernel::txc::onWindowEdge(true, s_txcExec.load(std::memory_order_relaxed), s_txcWin,\n'
         '                                    R.records);',
         '      (void)s_txcWin;')]),
    ('open-edge-before-the-preamble', GF, ['difftest', 'taps'], ['FAIL: F2b ', 'VIOLATION R7'], [
        ('    g_traceOpenPub.store(true, std::memory_order_release);\n',
         '    g_traceOpenPub.store(true, std::memory_order_release);\n'
         '    if (sr::kernel::txc::enabled()) {\n'
         '      s_txcWin = sr::kernel::txc::WindowCounters{};\n'
         '      sr::kernel::txc::onWindowEdge(true, s_txcExec.load(std::memory_order_relaxed), s_txcWin,\n'
         '                                    R.records);\n'
         '    }\n'),
        (OPEN_EDGE, '    return;')]),
    ('open-edge-without-record-count', GF, ['difftest', 'taps'], ['FAIL: F2b ', 'VIOLATION R7'], [
        (OPEN_EDGE, OPEN_EDGE.replace('R.records);', '0);'))]),
    ('close-edge-after-hook', GF, ['difftest', 'taps'], ['FAIL: F2 ', 'VIOLATION R7'], [
        (CLOSE_EDGE,
         '      noteRecorderClosed();\n'
         '      if (sr::kernel::txc::enabled())\n'
         '        sr::kernel::txc::onWindowEdge(false, s_txcExec.load(std::memory_order_relaxed), s_txcWin,\n'
         '                                      R.records);')]),
    ('close-edge-without-record-count', GF, ['difftest', 'taps'], ['FAIL: F2b ', 'VIOLATION R7'], [
        (CLOSE_EDGE, CLOSE_EDGE.replace('R.records);', '0);'))]),
    ('slotwrite-no-resident', GF, ['taps'], ['VIOLATION R2'], [
        ('    sr::kernel::txc::onResident(key, band, source, s_txcExec.load(std::memory_order_relaxed));\n}',
         '    (void)source;\n}')]),
    ('direct-slot-write', GF, ['taps'], ['VIOLATION R1'], [
        ('                slotWrite(sr::kernel::txc::kBand2D, key, slot, sr::kernel::txc::kFromUpload);',
         '                s_texSlot[key] = slot;')]),
    ('slot-insert_or_assign', GF, ['taps'], ['VIOLATION R1'], [
        ('                slotWrite(sr::kernel::txc::kBand2D, key, slot, sr::kernel::txc::kFromUpload);',
         '                s_texSlot.insert_or_assign(key, slot);')]),
    # ── hle_api_observe.cpp (the producer; killed by the exe suite's C14) ──
    ('producer-no-token', HO, ['exe', 'taps'], ['FAIL: C14', 'VIOLATION R8'], [
        ('const sr::kernel::txc::BindToken txcTok = sr::kernel::txc::bindBegin();',
         'const sr::kernel::txc::BindToken txcTok{};')]),
    ('producer-no-alias', HO, ['exe', 'taps'], ['FAIL: C14', 'VIOLATION R8'], [
        ('                txcWindow = ap.window;\n', '')]),
    ('producer-decode-dropped', HO, ['exe'], ['FAIL: C14'], [
        ('          if (sr::kernel::txc::enabled() && (txcIssued || !doIt))',
         '          if (false && (txcIssued || !doIt))')]),
    # ── tools/tex_census.py ─────────────────────────────────────────────────
    ('py-order-inclusive', PY, ['pyself'], ['order_amb ->', 'late_append ->', 'order_after ->'], [
        ('                        i = bisect.bisect_left(ws, E)         # P1 < E: the prefix',
         '                        i = bisect.bisect_right(ws, E)')]),
    ('py-ambiguity-ignored', PY, ['pyself'], ['order_amb ->', 'late_append ->'], [
        ('                        amb = i < len(lst) and lst[i].z < E and lst[i] is not d0',
         '                        amb = False')]),
    ('py-content-ignored', PY, ['pyself'], ['stale_content ->'], [
        ('                        elif ld.x != d0.x:', '                        elif False:')]),
    ('py-owner-set-ignored', PY, ['pyself'], ['stale_owner ->', 'third_owner ->'], [
        ('                        elif ld.v != d0.v:', '                        elif False:')]),
    ('py-unreadable-as-fresh', PY, ['pyself'], ['unreadable ->'], [
        ('                        elif not (ld.x and d0.x):', '                        elif False:')]),
    ('py-mismatch-dims-ignored', PY, ['pyself'], ['mismatch_dims ->'], [
        ('                elif (ap.e & 0x3F) != fmt6 or ap.d != dims:', '                elif (ap.e & 0x3F) != fmt6:')]),
    ('py-mismatch-fmt-ignored', PY, ['pyself'], ['mismatch_fmt ->'], [
        ('                elif (ap.e & 0x3F) != fmt6 or ap.d != dims:', '                elif ap.d != dims:')]),
    ('py-multiband-ignored', PY, ['pyself'], ['multiband ->'], [
        ("        elif bin(populated).count('1') > 1:", '        elif False:')]),
    ('py-band-ignored', PY, ['pyself'], ['band_cube ->', 'band_stacked ->'], [
        ('        elif populated != 1 << want_band:', '        elif False:')]),
    ('py-stacked-ignored', PY, ['pyself'], ['band_stacked ->'], [
        ('    if dim == 2 or (dim == 1 and stacked):', '    if dim == 2:')]),
    ('py-fetch-base-ignored', PY, ['pyself'], ['fetch_base ->'], [
        ('        base_ok = (d.d & PAGE_MASK) == (key & PAGE_MASK)', '        base_ok = True')]),
    ('py-aliasing-is-coverage', PY, ['pyself'], ['cover ->', 'many_owners ->', 'many owners called aliased'], [
        ('        if any_ and lo < reach:', '        if any_:')]),
    ('py-grant-ambiguity-ignored', PY, ['pyself'], ['grant_amb ->'], [
        ('            if overlaps(va, size, p, span):\n                return True',
         '            if False:\n                return True')]),
    ('py-reuse-always', PY, ['pyself'], ['F2 benign'], [('                    if reuse_memo[mk]:', '                    if True:')]),
    ('py-nonupload-not-excluded', PY, ['pyself'], ['F8 '], [
        ("               'non-upload residents': sum(1 for r in elig if r['content'].startswith('NONUPLOAD')),",
         "               'non-upload residents': 0,")]),
    ('py-upload-na-not-excluded', PY, ['pyself'], ['F8 '], [
        ("        'upload-time N/A': sum(1 for r in elig if r['ownUpload'].startswith('N/A') and",
         "        'upload-time N/A': 0 * sum(1 for r in elig if r['ownUpload'].startswith('N/A') and")]),
    ('py-unknown-not-blocking', PY, ['pyself'], ['F5b '], [
        ("    st_excl = {'UNKNOWN': sum(1 for r in elig if r['content'].startswith('UNKNOWN')),",
         "    st_excl = {'UNKNOWN': 0,")]),
    ('py-own-unknown-not-blocking', PY, ['pyself'], ['F5b '], [
        ("        'UNKNOWN': sum(1 for r in elig if 'ALIASED' not in (r['ownDraw'], r['ownUpload']) and",
         "        'UNKNOWN': 0 * sum(1 for r in elig if 'ALIASED' not in (r['ownDraw'], r['ownUpload']) and")]),
    ('py-whiteout-not-excluded', PY, ['pyself'], ['F9 '], [
        ("        'white-out skips': whiteout, 'no-opportunity': no_opp}\n    own_decided",
         "        'white-out skips': 0, 'no-opportunity': no_opp}\n    own_decided")]),
    ('py-refused-not-named', PY, ['pyself'], ['F10 '], [
        ("        outside = '; outside the cohort: %d draw(s) refused at admission' % refused",
         "        outside = ''")]),
    ('py-gate-ignores-witness', PY, ['pyself'], ['F6 ', 'F7b '], [('    gate = complete and witnessed', '    gate = complete')]),
    ('py-lockstep-z-ignored', PY, ['pyself'], ['M apply z ->'], [
        ('                elif (pb.a, pb.d, pb.e, pb.z) != (e.a, e.d, e.e, e.z):',
         '                elif (pb.a, pb.d, pb.e) != (e.a, e.d, e.e):')]),
    ('py-seq-check-dropped', PY, ['pyself'], ['M seq gap ->'], [('        if e.seq != i + 1:', '        if False:')]),
    ('py-owner-digest-ignored', PY, ['pyself'], ['M recorded digest ->', 'M recorded aliasing ->'], [
        ("            if rec != want:\n                fails['C-owners']",
         "            if rec[:4] != want[:4]:\n                fails['C-owners']")]),
    ('py-release-serial-unchecked', PY, ['pyself'], ['M release of an unknown grant ->'], [
        ('        if ent is None or ent[1] != serial or (how == 1 and ent[2]):', '        if ent is None:')]),
    ('py-render-z-unchecked', PY, ['pyself'], ['M render z backwards ->'], [
        ('            if e.z < last_render_z:\n                fails', '            if False:\n                fails')]),
    ('py-decode-bracket-unchecked', PY, ['pyself'], ['M decode bracket inverted ->'], [
        ("            if e.z > e.w:\n                fails['C-zorder'].append('seq %d: decode",
         "            if False:\n                fails['C-zorder'].append('seq %d: decode")]),
    ('py-late-publish-unchecked', PY, ['pyself'], ['M decode read a later publish ->'], [
        ('            if e.z <= max_dec_p1:', '            if False:')]),
    ('py-observation-order-unchecked', PY, ['pyself'], ['M decode bracket goes backwards ->'], [
        ('            if st[i][2] < st[i - 1][2] or st[i][1] < st[i - 1][1]:', '            if False:')]),
    ('py-append-contract-unchecked', PY, ['pyself'], ['M P1 < E but appended after the binding ->'], [
        ('            if j < len(st) and st[j][2] < d.z:', '            if False:')]),
    ('py-pairing-payload-unchecked', PY, ['pyself'], ['M ISSUED payload hash differs ->'], [
        ('                        or pb.x != e.x or not (e.z < e.y <= e.w)):',
         '                        or not (e.z < e.y <= e.w)):')]),
    ('py-resident-draw-unchecked', PY, ['pyself'], ['M binding records a wrong resident ->'], [
        ("                if rec != want:\n                    fails['C-resident']",
         "                if False:\n                    fails['C-resident']")]),
    ('py-inflight-unchecked', PY, ['pyself'], ['M inflight at write ->'], [
        ('    elif cost.c:', '    elif False:')]),
    ('py-window-count-unchecked', PY, ['pyself'], ['M in-window binding dropped ->'], [
        ('        if len(win_draws) != close.w - close.v:', '        if False:')]),
    ('py-live-records-unchecked', PY, ['pyself'], ['M live record count disagrees ->'], [
        ('        if rec_close - rec_open != close.z - open_z:', '        if False:')]),
    ('py-srht-validation-dropped', PY, ['pyself'], ['M trace record count lies ->'], [
        ("    if probs:\n        return ['trace does not validate", "    if False:\n        return ['trace does not validate")]),
    ('py-srht-draw-count-unchecked', PY, ['pyself'], ['M trace has a draw the census did not execute ->'], [
        ('                if ndraw != close.x:', '                if False:')]),
    ('py-srht-join-dropped', PY, ['pyself'], ['M a binding joined to a non-draw record ->'], [
        ('                    if not (rec_open <= idx < rec_close) or types[idx] not in SRHT_DRAW_TYPES:',
         '                    if False:')]),
    ('py-srht-join-off-by-one', PY, ['pyself'], ['T0w ', 'F7 '], [
        ('                    idx = rec_open + d.z - open_z - 2', '                    idx = rec_open + d.z - open_z - 1')]),
]

# ── HR1 (Astra 071/072/073): SR_GPU_PHYS_ROUTE in the sub_82467F40 override ─
GR = 'kernel/gpu_ring_alloc_fix.cpp'
HW = 'tools/hr1_witness.py'
HR1_DEF = '    hr1 = gpuPhysRouteOn() && claimed;'
HR1_IF = '  if ((flags & 0x80000000u) && !hr1) {'
HR1_DONE = '    hc.delegatedDone = true;\n'
HR1_WIT = '      if (claimed) g_hr1.heapAfterClaim++;\n      else if (ringUpAtRoute) g_hr1.heapSpanning++;'
MID = '    if (void (*h)() = g_closeMidHook.load(std::memory_order_relaxed)) h();   // test seam\n'
MUTANTS_HR1 = [
    # ── the routing ──────────────────────────────────────────────────────
    ('hr1-boundary-dropped', GR, ['hr1'], ['FAIL: B1 '], [(HR1_DEF, '    hr1 = gpuPhysRouteOn();')]),
    ('hr1-gate-ignored', GR, ['hr1'], ['FAIL: A2 ', 'FAIL: A4 '], [(HR1_DEF, '    hr1 = claimed;')]),
    ('hr1-never-delegates', GR, ['hr1'], ['FAIL: C size'], [
        (HR1_IF, '  if (flags & 0x80000000u) {')]),
    ('hr1-heap-fallback-for-large', GR, ['hr1'], ['FAIL: C size 0x40001', 'FAIL: C size 0x398000'], [
        (HR1_IF, '  if ((flags & 0x80000000u) && (!hr1 || size > 0x40000u)) {')]),
    ('hr1-override-allocates-small', GR, ['hr1'], ['FAIL: C size 0x1 ', 'FAIL: C size 0x1000', 'FAIL: E2 '], [
        (HR1_IF, '  if ((flags & 0x80000000u) && (!hr1 || size <= 0x40000u)) {')]),
    ('hr1-double-registration', GR, ['hr1'], ['FAIL: C size'], [
        (HR1_DONE, HR1_DONE + '    if (ctx.r3.u32) sr::kernel::physwin::notePhysGrant(ctx.r3.u32, sr3);\n')]),
    ('hr1-override-zero-fills', GR, ['hr1'], ['FAIL: E2 '], [
        (HR1_DONE, HR1_DONE + '    if (ctx.r3.u32 && sr3 <= 0x10000000u) if (uint8_t* h = '
                              'sr::kernel::guestVAToHost(ctx.r3.u32)) std::memset(h, 0, sr3);\n')]),
    # ── the accounting (one lock; admitted at admission, settled on exit) ─
    ('hr1-admission-not-counted', GR, ['hr1'], ['FAIL: G1 ', 'FAIL: G2 ', 'FAIL: G2b '], [
        ('    g_hr1.admitted++;\n', '')]),
    ('hr1-started-at-delegation', GR, ['hr1'], ['FAIL: G2b '], [
        ('    if (hr1) g_hr1.started++;\n', ''),
        ('    hc.size = sr3;',
         '    { std::lock_guard<std::mutex> lk(g_hr1Mu); g_hr1.started++; }\n    hc.size = sr3;')]),
    ('hr1-settled-not-counted', GR, ['hr1'], ['FAIL: G1 ', 'FAIL: G3 '], [('    g_hr1.settled++;\n', '')]),
    ('hr1-completion-not-counted', GR, ['hr1'], ['FAIL: C size', 'FAIL: G1 ', 'FAIL: F1 '], [
        ('      const uint64_t n = ++g_hr1.completed;', '      const uint64_t n = g_hr1.completed + 1;')]),
    ('hr1-null-not-counted', GR, ['hr1'], ['FAIL: F1 '], [
        ('      if (null) g_hr1.null++;', '      if (null) (void)0;')]),
    ('hr1-belowA-not-counted', GR, ['hr1'], ['FAIL: F4 '], [
        ('      else if (result < 0xA0000000u) g_hr1.belowA++;', '      else if (result < 0xA0000000u) (void)0;')]),
    ('hr1-witness-dropped', GR, ['hr1'], ['FAIL: A4 '], [
        (HR1_WIT, '      if (claimed) (void)0;\n      else if (ringUpAtRoute) g_hr1.heapSpanning++;')]),
    ('hr1-boundary-read-twice', GR, ['hr1'], ['FAIL: G3 '], [
        (HR1_WIT, '      if (ringUpAtRoute) g_hr1.heapAfterClaim++;')]),
    ('hr1-spanning-not-counted', GR, ['hr1'], ['FAIL: G3 '], [
        (HR1_WIT, '      if (claimed) g_hr1.heapAfterClaim++;\n      else if (ringUpAtRoute) (void)0;')]),
    # ── the capture endpoint ─────────────────────────────────────────────
    ('hr1-snapshot-every-close', GR, ['hr1'], ['FAIL: G1b '], [('  if (g_hr1Snap.taken) return;\n', '')]),
    ('hr1-env-gate-does-not-arm', GR, ['hr1'], ['FAIL: G0 '], [
        ('    if (on) sr::kernel::physwin::armRecorderCloseHook();\n', '')]),
    ('hr1-snapshot-line-format', GR, ['hr1int'], ['I1 '], [
        ('"[GPU_PHYS_ROUTE] CAPTURE SNAPSHOT why=%s atSeal=%d nonce=%08X sealSeq=%llu gate=%d "',
         '"[GPU_PHYS_ROUTE] CAPTURE SNAPSHOT why=%s atSeal=%d nonce=%08x sealSeq=%llu gate=%d "')]),
    ('hr1-endpoint-split', PW, ['hr1'], ['FAIL: G4 '], [(MID, '    hl.unlock();\n    lk.unlock();\n' + MID)]),
    ('hr1-snapshot-not-seal-bound', PW, ['hr1', 'hr1int'], ['FAIL: G5 ', 'I5 ', 'I6 '], [
        ('                                                  sealedNow ? g_stickyNonce : 0u,',
         '                                                  0u,')]),
    ('hr1-snapshot-not-at-close', PW, ['hr1'], ['FAIL: G0 ', 'FAIL: G1 '], [
        ('    sr::kernel::gpuPhysRouteCaptureSnapshotLocked("recorder-closed", sealedNow,',
         '    if (false) sr::kernel::gpuPhysRouteCaptureSnapshotLocked("recorder-closed", sealedNow,')]),
    ('mm-truncation-not-counted', ST, ['hr1'], ['FAIL: F5 '], [
        ('    g_mmTruncated.fetch_add(1, std::memory_order_relaxed);\n', '')]),
    ('mm-wrap-not-counted', ST, ['hr1'], ['FAIL: F6 '], [
        ('  if (r4 != 0 && sizeBytes < r4) g_mmWrapped.fetch_add(1, std::memory_order_relaxed);\n', '')]),
    # ── the evidence checker (tools/hr1_witness.py), killed by its self-test ─
    ('hw-heap-after-claim-ignored', HW, ['hr1py'], ['W2 '], [
        ("    if s['heapAfterClaim']:\n        res['routing'] = 'KILLED'",
         "    if False:\n        res['routing'] = 'KILLED'")]),
    ('hw-spanning-kills', HW, ['hr1py'], ['W1 ', 'W3 '], [
        ("    if s['heapAfterClaim']:\n        res['routing'] = 'KILLED'",
         "    if s['heapAfterClaim'] or s['heapSpanning']:\n        res['routing'] = 'KILLED'")]),
    ('hw-outstanding-ignored', HW, ['hr1py'], ['W6 '], [
        ("        if s['outstanding']:\n            problems.append", "        if False:\n            problems.append")]),
    ('hw-consistency-ignored', HW, ['hr1py'], ['W7 '], [
        ("        if s['outstanding'] != s['admitted'] - s['settled'] or",
         "        if False and s['outstanding'] != s['admitted'] - s['settled'] or")]),
    # Astra 074: conservation (killed by the synthetic case AND the real G6 regression)
    ('hw-completion-conservation-ignored', HW, ['hr1py', 'hr1int'], ['W26 ', 'I8 '], [
        ("        if s['outstanding'] == 0 and s['completed'] != s['started']:", '        if False:')]),
    ('hw-heap-population-ignored', HW, ['hr1py', 'hr1int'], ['W27 ', 'I9 '], [
        ("        if s['heapAfterClaim'] + s['heapSpanning'] > s['admitted'] - min(s['started'], s['admitted']):",
         '        if False:')]),
    ('hw-heap-population-off-by-one', HW, ['hr1py'], ['W28 ', 'W1 '], [
        ("        if s['heapAfterClaim'] + s['heapSpanning'] > s['admitted'] - min(s['started'], s['admitted']):",
         "        if s['heapAfterClaim'] + s['heapSpanning'] >= s['admitted'] - min(s['started'], s['admitted']):")]),
    ('hw-gate-ignored', HW, ['hr1py'], ['W8 '], [("        if s['gate'] != 1:", '        if False:')]),
    ('hw-duplicate-snapshots-accepted', HW, ['hr1py'], ['W5 ', 'W15 '], [
        ('    if count != 1 or len(snaps) != 1:', '    if count < 1 or len(snaps) < 1:')]),
    ('hw-malformed-snapshot-dropped', HW, ['hr1py'], ['W15 '], [
        ('    return log_text.count(MARKER), out',
         '    return len([o for o in out if o]), [o for o in out if o]')]),
    ('hw-trailing-accepted', HW, ['hr1py'], ['W16 ', 'W25 '], [(r"""_INT) for k in COUNTERS) + r'[ \t]*$')""",
                                                                  "_INT) for k in COUNTERS))")]),
    ('hw-why-ignored', HW, ['hr1py'], ['W17 '], [("        if s['why'] != 'recorder-closed':", '        if False:')]),
    ('hw-atseal-ignored', HW, ['hr1py'], ['W23 '], [("        if s['atSeal'] != 1:", '        if False:')]),
    ('hw-nonce-binding-ignored', HW, ['hr1py'], ['W21 '], [
        ("            if s['nonce'] != nonce or s['nonce'] != sl['nonce']:", '            if False:')]),
    ('hw-seq-binding-ignored', HW, ['hr1py'], ['W22 '], [
        ("            if s['sealSeq'] != sl['seq']:", '            if False:')]),
    ('hw-accept-skipped', HW, ['hr1py'], ['W11 ', 'W12 ', 'W13 '], [
        ("        if not ok:\n            problems.append('H-UNIQ record not accepted: '",
         "        if False:\n            problems.append('H-UNIQ record not accepted: '")]),
    ('hw-driver-substring', HW, ['hr1py'], ['W18 ', 'W19 '], [
        ("    return t.count('RESULT:') == 1 and len(DRIVER_OK.findall(t)) == 1",
         "    return 'RESULT: SUCCESS' in t")]),
    ('hw-driver-contradiction-ignored', HW, ['hr1py'], ['W18 '], [
        ("    return t.count('RESULT:') == 1 and len(DRIVER_OK.findall(t)) == 1",
         '    return len(DRIVER_OK.findall(t)) >= 1')]),
    ('hw-vacuous-eligible', HW, ['hr1py'], ['W20 '], [("    elif s['completed'] == 0:", '    elif False:')]),
    ('hw-import-anomalies-ignored', HW, ['hr1py'], ['W9 mmTruncated', 'W9 mmWrapped'], [
        ("    for k in ('null', 'delegatedBelowA', 'mmTruncated', 'mmWrapped'):\n        if s[k]:",
         "    for k in ('null', 'delegatedBelowA'):\n        if s[k]:")]),
    ('hw-sealed-floor-ignored', HW, ['hr1py'], ['W10 sealed minPhysRegionLo'], [
        ("    if r['minPhysRegionLo'] < 0xA0000000:", '    if False:')]),
    ('hw-gameplay-ignored', HW, ['hr1py'], ['W14 '], [
        ('    gameplay = driver_success(driver_text)', '    gameplay = True')]),
]
MUTANT_SETS = {'census': None, 'hr1': MUTANTS_HR1}   # None = MUTANTS (defined above)

MARKERS = {
    'difftest': re.compile(r'^\[TXC_TEST\] \d+ passed, 0 failed$', re.M),
    'exe': re.compile(r'\[TEXUNLOCK_TEST\] (\d+)/\1 passed'),
    'taps': re.compile(r'^\[TXC_TAPS\] 0 violations$', re.M),
    'pyself': re.compile(r'^\[TEX_CENSUS_SELFTEST\] 0 failed$', re.M),
    'hr1': re.compile(r'^\[GPU_PHYS_ROUTE_TEST\] \d+ passed, 0 failed$', re.M),
    'hr1py': re.compile(r'^\[HR1_WITNESS_SELFTEST\] 0 failed$', re.M),
    'hr1int': re.compile(r'^\[HR1_WITNESS_INTEGRATION\] 0 failed$', re.M),
}


def wbytes(path, data):
    for _ in range(60):
        try:
            with open(path, 'wb') as f:
                f.write(data)
            return
        except OSError:
            time.sleep(0.5)
    raise RuntimeError('could not write ' + path)


def mutate(path, subs):
    raw = open(path, 'rb').read()
    crlf = b'\r\n' in raw
    txt = raw.decode('utf-8').replace('\r\n', '\n')
    for old, new in subs:
        n = txt.count(old)
        if n != 1:
            raise RuntimeError('%s: anchor found %d times: %r' % (path, n, old[:70]))
        txt = txt.replace(old, new, 1)
    wbytes(path, (txt.replace('\n', '\r\n') if crlf else txt).encode('utf-8'))
    return raw


def run(cmd, env=None, timeout=900):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', env=env,
                           timeout=timeout, shell=isinstance(cmd, str))
        return r.returncode, r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return None, ''


def build(target=None):
    """cmake --build, retried ONLY when the linker could not write the output
    (a just-exited binary still held open, e.g. by a scanner): a transient
    host lock is not a mutant outcome. Any other build error stands."""
    cmd = 'cmake --build build' + (' --target %s' % target if target else '')
    for attempt in range(6):
        rc, out = run(cmd, timeout=3000)
        if not rc or 'permission denied' not in out:
            return rc, out
        time.sleep(5)
    return rc, out


def killers_output(killers):
    """[(killer, rc, text)] - rc None = timed out."""
    out = []
    for k in killers:
        if k == 'difftest':
            rc, t = run([DIFFTEST, '--tex-census-test'], timeout=300)
        elif k == 'exe':
            env = dict(os.environ)
            env['SR_HLE_TEX_UNLOCK_TEST'] = '1'
            env.setdefault('SR_MODS', '0')  # iter-463: never mount the player's mods (docs/MODS.md)
            env.setdefault('SR_DEVTOOLS', '0')  # iter-463 phase 2: nor enable a dev tool (docs/DEVTOOLS.md)
            rc, t = run([EXE], env=env)
        elif k == 'taps':
            rc, t = run([sys.executable, 'tools/test_txc_taps.py'])
        elif k == 'hr1':
            rc, t = run([DIFFTEST, '--gpu-phys-route-test'], timeout=300)
        elif k == 'hr1py':
            rc, t = run([sys.executable, 'tools/hr1_witness.py', '--self-test'])
        elif k == 'hr1int':
            rc, t = run([sys.executable, 'tools/hr1_witness.py', '--integration', DIFFTEST],
                        timeout=600)
        else:
            rc, t = run([sys.executable, PY, '--self-test'])
        out.append((k, rc, t))
    return out


def failing_lines(text):
    """Only a check's OWN failure line counts: a line that STARTS with FAIL /
    VIOLATION, or the exe suite's '[TEXUNLOCK_TEST] FAIL:'. A passing line
    that merely mentions FAIL (the analyzer's 'ok  M ... -> C-seq FAIL') and a
    log line like '[TXC] WRITE FAILED' never kill a mutant."""
    out = []
    for l in text.splitlines():
        s = l.strip()
        if s.startswith('FAIL') or s.startswith('VIOLATION') or '[TEXUNLOCK_TEST] FAIL:' in s:
            out.append(s)
    return out


# Each killer's own summary line, whatever its counts. The suites exit with
# their failure COUNT (difftest, exe) or 1 (taps, pyself), so an exit code
# alone cannot tell a crash from failed checks: a missing summary line can.
SUMMARIES = {
    'difftest': re.compile(r'^\[TXC_TEST\] \d+ passed, \d+ failed$', re.M),
    'exe': re.compile(r'\[TEXUNLOCK_TEST\] \d+/\d+ passed'),
    'taps': re.compile(r'^\[TXC_TAPS\] \d+ violations$', re.M),
    'pyself': re.compile(r'^\[TEX_CENSUS_SELFTEST\] \d+ failed$', re.M),
    'hr1': re.compile(r'^\[GPU_PHYS_ROUTE_TEST\] \d+ passed, \d+ failed$', re.M),
    'hr1py': re.compile(r'^\[HR1_WITNESS_SELFTEST\] \d+ failed$', re.M),
    'hr1int': re.compile(r'^\[HR1_WITNESS_INTEGRATION\] \d+ failed$', re.M),
}


def abnormal(results):
    """Crashes/timeouts: a killer that timed out, or ended without its own
    summary line (it died before finishing)."""
    bad = []
    for k, rc, t in results:
        if rc is None:
            bad.append('%s TIMEOUT' % k)
        elif not SUMMARIES[k].search(t):
            bad.append('%s CRASH/INCOMPLETE rc=%d (no summary line)' % (k, rc))
    return bad


def check_anchors(mutants=None):
    mutants = MUTANTS if mutants is None else mutants
    bad = 0
    for name, path, _k, _t, subs in mutants:
        txt = open(path, 'rb').read().decode('utf-8').replace('\r\n', '\n')
        for old, _new in subs:
            n = txt.count(old)
            if n != 1:
                bad += 1
                print('ANCHOR %s %s found %d times: %r' % (name, path, n, old[:70]))
    names = [m[0] for m in mutants]
    dup = sorted({n for n in names if names.count(n) > 1})
    if dup:
        bad += 1
        print('DUPLICATE mutant names %s' % dup)
    print('[MUTANT_ANCHORS] %d mutants, %d bad' % (len(mutants), bad))
    return bad


def main(argv):
    out_path, only, skip_exe, mset = None, None, False, 'census'
    i = 1
    while i < len(argv):
        if argv[i] == '--out':
            out_path = argv[i + 1]
            i += 2
        elif argv[i] == '--only':
            only = set(argv[i + 1].split(','))
            i += 2
        elif argv[i] == '--skip-exe':
            skip_exe = True
            i += 1
        elif argv[i] == '--set':
            mset = argv[i + 1]
            i += 2
        elif argv[i] == '--check-anchors':
            return 1 if any(check_anchors(MUTANT_SETS[k]) for k in MUTANT_SETS) else 0
        else:
            print('unknown argument ' + argv[i])
            return 2
    if mset not in MUTANT_SETS:
        print('unknown --set ' + mset)
        return 2
    mutants = MUTANT_SETS[mset] or MUTANTS
    if check_anchors(mutants):
        return 2
    files = sorted({m[1] for m in mutants})
    orig = {p: open(p, 'rb').read() for p in files}
    sums = {p: hashlib.md5(b).hexdigest() for p, b in orig.items()}
    lines = []

    def emit(s):
        print(s, flush=True)
        lines.append(s)

    def finish(rc):
        if out_path:
            with open(out_path, 'w', encoding='utf-8') as f:
                f.write('\n'.join(lines) + '\n')
        return rc

    # The BASELINE, on binaries built from this tree: every killer exits 0,
    # prints its completion marker, and prints no failure line.
    rc, bout = build()
    if rc:
        emit('baseline: BUILD FAILED %s' % bout[-300:].replace('\n', ' '))
        return finish(2)
    base_killers = (['hr1', 'hr1py', 'hr1int'] if mset == 'hr1' else
                    ['difftest', 'taps', 'pyself']) + \
        ([] if skip_exe else ['exe'])
    problems = []
    for k, rc, t in killers_output(base_killers):
        if rc != 0:
            problems.append('%s exit %s' % (k, 'TIMEOUT' if rc is None else rc))
        if not MARKERS[k].search(t):
            problems.append('%s: no completion marker' % k)
        fl = failing_lines(t)
        if fl:
            problems.append('%s: %s' % (k, fl[:3]))
    emit('baseline (%s): %s' % (','.join(base_killers),
                               'clean (exit 0, completion markers, no failure lines)'
                               if not problems else 'FAILING %s' % problems))
    if problems:
        return finish(2)
    killed = total = 0
    try:
        for name, path, killers, tokens, subs in mutants:
            if only and name not in only:
                continue
            if skip_exe and 'exe' in killers:
                continue
            total += 1
            raw = mutate(path, subs)
            built = True
            if not path.endswith('.py'):
                target = 'SupermanReturns' if 'exe' in killers else 'difftest'
                rc, bout = build(target)
                if rc:
                    emit('%-40s BUILD FAILED %s' % (name, bout[-300:].replace('\n', ' ')))
                    built = False
            if built:
                res = killers_output(killers)
                fl = failing_lines('\n'.join(t for _k, _rc, t in res))
                hit = any(any(t in l for t in tokens) for l in fl)
                ab = abnormal(res)
                killed += hit
                emit('%-40s %s via %s%s | %s' % (
                    name, 'KILLED  ' if hit else 'SURVIVED', ','.join(killers),
                    (' [' + ', '.join(ab) + ']') if ab else '',
                    ' | '.join(l[:70] for l in fl)[:300]))
            wbytes(path, raw)
    finally:
        for p in files:
            if hashlib.md5(open(p, 'rb').read()).hexdigest() != sums[p]:
                wbytes(p, orig[p])
                emit('RESTORED ' + p)
        bad = [p for p in files if hashlib.md5(open(p, 'rb').read()).hexdigest() != sums[p]]
        emit('md5 guard: ' + ('all restored' if not bad else 'MISMATCH %s' % bad))
        build()
    emit('KILLED %d / %d' % (killed, total))
    return finish(0 if killed == total else 1)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
