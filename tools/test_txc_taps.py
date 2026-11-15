"""Whole-surface wiring check for the texture census taps (iter-458, Astra 069/070).

A green census suite proves the tap BODIES; this proves the WIRING cannot be
bypassed. Every rule is an every/only/never claim over the source, checked by
search, and --self-test injects a violation for each rule and requires the
check to fail.

  R1  gfx/hle_video.cpp: outside slotWrite - the ONE writer, whose body emits
      the Resident - s_texSlot / s_texSlot3D / s_texSlotCube appear ONLY as a
      read-only member call (find / end / count / size / empty / begin) or in
      their declarations. A WHITELIST: any other member call (insert_or_assign,
      emplace, erase, ...), subscript, reference, pointer or alias is a
      violation (Astra 070).
  R2  slotWrite's body calls txc::onResident.
  R3  onTexApply is called ONLY from the TextureUpload arm's RAII witness.
  R4  execute() runs txcExecPrologue(c); the draw-tap seam runs the same
      prologue; the prologue advances s_txcExec and counts window draws with
      txcIsDrawCmd, which is EXACTLY the draw arm's four types (DrawVerticesUP
      included).
  R5  buildDraw calls onDrawTex exactly once, passing the fetch's stacked bit,
      and counts bindingsSeen before the diagnostic white-outs, each of which
      counts its skip.
  R6  publish() hands onTexPublish the command's payload pointer.
  R7  the recorder publishes the census's open edge ONCE, where recording goes
      LIVE (after the preamble, just before the open path returns), and the
      close edge BEFORE noteRecorderClosed(); both carry R.records.
  R8  kernel/hle_api_observe.cpp: bindBegin() opens the capture block before
      any snapshot; the ISSUED bind records the snapshot's alias; the decode
      call passes the token and the alias.
  R9  kernel/phys_window.cpp: every registry mutation bumps the generation and
      reports to the census (register, main-stack note, reclaim, release x2).
  R10 kernel/tex_census.cpp: a decode observes and appends under g_obs and
      reads P1 inside g_mu immediately before its append; grant events read P1
      inside g_mu; the flush waits for in-flight observations and writes under
      the registry's shared lock; bindBegin / ~BindToken count observations in
      flight.

Usage: python tools/test_txc_taps.py            (exit 1 on any violation)
       python tools/test_txc_taps.py --self-test
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8') as f:
        return f.read().replace('\r\n', '\n')


def body_of(src, signature):
    """The brace-balanced body following `signature` (first occurrence)."""
    i = src.find(signature)
    if i < 0:
        return None
    j = src.find('{', i)
    depth, k = 0, j
    while k < len(src):
        if src[k] == '{':
            depth += 1
        elif src[k] == '}':
            depth -= 1
            if depth == 0:
                return src[j:k + 1]
        k += 1
    return None


def strip_comments(src):
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    return re.sub(r'//[^\n]*', '', src)


SLOT_MAP = r's_texSlot(?:3D|Cube)?\b'
READ_ONLY = ('find', 'end', 'count', 'size', 'empty', 'begin')
DECL = re.compile(r'\s*(?:static\s+)?std::unordered_map<uint32_t, uint32_t>\s+' + SLOT_MAP + r'\s*;')
P1_IN_MU = (r'std::lock_guard<std::mutex> lk\(g_mu\);\s*'
            r'const uint64_t p1 = sr::gfx::hle::txcFifoPublished\(\);')


def slot_map_misuse(outside):
    """Every slot-map mention outside slotWrite that is not a read-only call."""
    bad = []
    for m in re.finditer(SLOT_MAP, outside):
        ls = outside.rfind('\n', 0, m.start()) + 1
        le = outside.find('\n', m.end())
        line = outside[ls:le if le >= 0 else len(outside)]
        if DECL.match(line):
            continue
        call = re.match(r'\s*\.\s*(\w+)\s*\(', outside[m.end():m.end() + 48])
        if not call or call.group(1) not in READ_ONLY:
            bad.append(line.strip()[:72])
    return bad


def check(video, observe, physwin, census):
    errs = []
    v = strip_comments(video)
    sw = body_of(v, 'void slotWrite(uint32_t band, uint32_t key, uint32_t slot, uint32_t source)')
    if sw is None:
        errs.append('R1/R2: slotWrite not found')
        sw = ''
    outside = v.replace(sw, '', 1) if sw else v
    bad = slot_map_misuse(outside)
    if bad:
        errs.append('R1: slot maps used other than read-only outside slotWrite: %s' % bad[:4])
    if 'txc::onResident(' not in sw:
        errs.append('R2: slotWrite does not emit the census Resident')
    calls = [m.start() for m in re.finditer(r'txc::onTexApply\(', v)]
    dtor = body_of(v, '~TxcApply()')
    if len(calls) != 1 or not dtor or 'txc::onTexApply(' not in dtor:
        errs.append('R3: onTexApply must be called exactly once, from ~TxcApply (%d calls)' %
                    len(calls))
    ex = body_of(v, 'void execute(const RenderCommand& c)')
    pro = body_of(v, 'void txcExecPrologue(const RenderCommand& c)')
    seam = body_of(v, 'bool executeDrawThroughTapForTest(const RenderCommand& c)')
    if not ex or 'txcExecPrologue(c);' not in ex:
        errs.append('R4: execute() does not run txcExecPrologue(c)')
    if not seam or 'txcExecPrologue(c);' not in seam or 'buildDraw(c, q)' not in seam:
        errs.append('R4: the draw-tap seam does not run the prologue then buildDraw')
    if not pro or 's_txcExec.fetch_add(1' not in pro or 's_txcWin.drawCmds++' not in pro or \
            'txcIsDrawCmd(c.type)' not in pro:
        errs.append('R4: the prologue does not advance the FIFO clock and count window draws')
    dp = body_of(v, 'bool txcIsDrawCmd(RenderCommandType t)')
    kinds = re.findall(r'RenderCommandType::(\w+)', dp or '')
    if sorted(kinds) != sorted(['DrawVertices', 'DrawIndexedVertices', 'DrawVerticesUP',
                                'BeginVertices']):
        errs.append('R4: the census draw predicate is not exactly the draw arm (UP included): %s'
                    % kinds)
    bd = body_of(v, 'bool buildDraw(const RenderCommand& c, sr::gfx::D3D12Backend::HleDraw& q)')
    if not bd or bd.count('txc::onDrawTex(') != 1 or \
            not re.search(r'txc::onDrawTex\([^;]*s_rs\.texDepth\[s\] > 1u,', bd):
        errs.append('R5: buildDraw must call onDrawTex exactly once, with the stacked bit')
    else:
        seen = bd.find('s_txcWin.bindingsSeen++')
        white = [m.start() for m in re.finditer(r'q\.texSlot3D\[s\] = 0u;[^}]*continue;', bd)]
        skips = len(re.findall(r's_txcWin\.whiteoutSkips\+\+;\s*continue;', bd))
        if seen < 0 or any(w < seen for w in white) or skips != len(white) or not white:
            errs.append('R5: bindingsSeen must precede every white-out, each counting its skip '
                        '(white-outs %d, counted %d)' % (len(white), skips))
    pub = body_of(v, 'void publish(const RenderCommand* cmds, uint32_t count)')
    if not pub or not re.search(r'txc::onTexPublish\([^;]*at \+ i \+ 1,\s*pay\)', pub) or \
            'texRead(u.arenaOffset)' not in pub:
        errs.append('R6: publish() must pass the command payload (pay) and FIFO index')
    live = v.find('s_shadowOpen = false;')
    rec = v.find('TraceRecord rec{};', live) if live >= 0 else -1
    seg = v[live:rec] if live >= 0 and rec >= 0 else ''
    if v.count('txc::onWindowEdge(true') != 1 or not re.search(
            r'txc::onWindowEdge\(true,[^;]*R\.records\);\s*\}\s*return;\s*\}\s*$', seg):
        errs.append('R7: the open edge must be published once, where recording goes LIVE (after '
                    'the preamble, just before the open path returns), with R.records')
    cl = v.find('g_traceOpenPub.store(false, std::memory_order_release);\n      if (sr::kernel::txc')
    nc = v.find('noteRecorderClosed();', cl) if cl >= 0 else -1
    if cl < 0 or nc < 0 or not re.search(r'txc::onWindowEdge\(false,[^;]*R\.records\);', v[cl:nc]):
        errs.append('R7: the close edge must be published before noteRecorderClosed(), with '
                    'R.records')
    o = strip_comments(observe)
    cap = o.find('if (texCaptureEnabled() && known && span >= 64) {')
    tok = o.find('sr::kernel::txc::bindBegin()', cap)
    snap = o.find('pickAliasWindow(base, span,', cap)
    if cap < 0 or tok < 0 or snap < 0 or not tok < snap:
        errs.append('R8: bindBegin() must open the capture block before the snapshot')
    if not re.search(r'enqueue\(tc\);\s*txcIssued = true;\s*txcWindow = ap\.window;', o):
        errs.append('R8: the ISSUED bind must record the snapshot\'s alias')
    if not re.search(r'txc::onTexDecode\(\s*txcTok,[^;]*txcIssued, txcWindow\)', o):
        errs.append('R8: the decode must pass the bind token and the alias')
    p = strip_comments(physwin)
    reg = body_of(p, 'void registerGrant(int kind, uint32_t va, uint32_t size)')
    if not reg or reg.count('g_grantGen.fetch_add(') != 2 or reg.count('txc::onGrantReg(') != 2:
        errs.append('R9: registerGrant must bump the generation and report both of its writes')
    fg = body_of(p, 'void forgetPhysGrant(uint32_t va, bool report)')
    if not fg or 'g_grantGen.fetch_add(' not in fg or 'txc::onGrantRel(2, va, serial, 2' not in fg \
            or fg.find('erase(it)') > fg.find('txc::onGrantRel('):
        errs.append('R9: forgetPhysGrant must erase, bump and then report the reclaim')
    mk = body_of(p, 'bool markPhysGrantReleased(uint32_t va, bool report)')
    if not mk or mk.count('g_grantGen.fetch_add(') != 2 or mk.count('txc::onGrantRel(') != 2:
        errs.append('R9: markPhysGrantReleased must bump and report both release kinds')
    c = strip_comments(census)
    du = body_of(c, 'void decodeUnderRegistry(const sr::kernel::physwin::SpanOwners& o, void* v)')
    obs = du.find('std::lock_guard<std::mutex> obs(g_obs);') if du else -1
    smp = du.find('sampleGuest(') if du else -1
    if obs < 0 or smp < 0 or obs > smp:
        errs.append('R10: a decode must observe and append under g_obs')
    elif not re.search(P1_IN_MU, du) or du.count('txcFifoPublished(') != 1 or \
            du.find('txcFifoPublished(') < du.find('hook();'):
        errs.append('R10: a decode must read P1 inside g_mu, after the hook, before its append')
    for fn in ('void onGrantReg(', 'void onGrantRel('):
        g = body_of(c, fn)
        if not g or not re.search(P1_IN_MU, g):
            errs.append('R10: %s must read P1 inside g_mu' % fn[5:-1])
    fl = body_of(c, 'bool flush(const char* why)')
    if not fl or not re.search(r'while \(g_inflight\.load\(', fl) or \
            'withRegistryShared(&flushUnderRegistry' not in fl:
        errs.append('R10: the flush must wait for in-flight observations and write under the '
                    'registry lock')
    dt = body_of(c, 'BindToken::~BindToken()')
    bb = body_of(c, 'BindToken bindBegin()')
    if not dt or 'g_inflight.fetch_sub(' not in dt or not bb or 'g_inflight.fetch_add(' not in bb:
        errs.append('R10: bindBegin / ~BindToken must count observations in flight')
    return errs


def load():
    return (read('gfx/hle_video.cpp'), read('kernel/hle_api_observe.cpp'),
            read('kernel/phys_window.cpp'), read('kernel/tex_census.cpp'))


def self_test():
    v, o, p, c = load()
    base = check(v, o, p, c)
    fails = 0

    def expect(name, rule, vv=v, oo=o, pp=p, cc=c):
        nonlocal fails
        mutated = (vv, oo, pp, cc) != (v, o, p, c)
        errs = check(vv, oo, pp, cc)
        ok = mutated and any(e.startswith(rule) for e in errs)
        print('  %s  %s -> %s%s' % ('ok' if ok else 'FAIL:', name, rule,
                                   '' if mutated else ' (injection anchor not found)'))
        if not ok:
            fails += 1

    ok0 = not base
    print('  %s  S0 the tree passes every rule (%s)' % ('ok' if ok0 else 'FAIL:', base))
    fails += not ok0
    anchor = '    q.texKey[s] = tk;\n'
    expect('a direct 2D slot write', 'R1', vv=v.replace(anchor, anchor + '    s_texSlot[tk] = 1u;\n', 1))
    expect('a slot erase', 'R1', vv=v.replace(anchor, anchor + '    s_texSlotCube.erase(tk);\n', 1))
    expect('an insert_or_assign', 'R1',
           vv=v.replace(anchor, anchor + '    s_texSlot.insert_or_assign(tk, 1u);\n', 1))
    expect('a reference alias of a slot map', 'R1',
           vv=v.replace(anchor, anchor + '    auto& slotAlias = s_texSlot3D;\n', 1))
    expect('slotWrite loses its Resident', 'R2',
           vv=v.replace('    sr::kernel::txc::onResident(key, band, source,', '    (void)(key, band, source,', 1))
    expect('a second onTexApply caller', 'R3',
           vv=v.replace(anchor, anchor + '    sr::kernel::txc::onTexApply(0, 0, 0, 0, 0, 0);\n', 1))
    expect('execute skips the prologue', 'R4', vv=v.replace('  txcExecPrologue(c);\n\n  switch', '\n  switch', 1))
    expect('the prologue stops counting window draws', 'R4',
           vv=v.replace('s_txcWin.drawCmds++;', ';', 1))
    expect('UP dropped from the census draw predicate', 'R4',
           vv=v.replace('         t == RenderCommandType::DrawVerticesUP || ', '         ', 1))
    expect('the prologue counts with isDrawType (no UP)', 'R4',
           vv=v.replace('if (txcIsDrawCmd(c.type) && traceRecorderOpen())',
                        'if (isDrawType(c.type) && traceRecorderOpen())', 1))
    expect('the stacked bit not passed', 'R5', vv=v.replace('s_rs.texDepth[s] > 1u,', 'false,', 1))
    expect('bindingsSeen counted after the white-outs', 'R5',
           vv=v.replace('    if (txcWin && tk) s_txcWin.bindingsSeen++;\n', '', 1))
    expect('a white-out that does not count its skip', 'R5',
           vv=v.replace('if (txcWin && tk) s_txcWin.whiteoutSkips++; continue; }', 'continue; }', 1))
    expect('publish passes no payload', 'R6', vv=v.replace('at + i + 1, pay);', 'at + i + 1, nullptr);', 1))
    expect('no open edge', 'R7', vv=v.replace('sr::kernel::txc::onWindowEdge(true,', '(void)(true,', 1))
    expect('the open edge back at the store (before the preamble)', 'R7',
           vv=v.replace('    g_traceOpenPub.store(true, std::memory_order_release);\n',
                        '    g_traceOpenPub.store(true, std::memory_order_release);\n'
                        '    sr::kernel::txc::onWindowEdge(true, 0, s_txcWin, R.records);\n', 1))
    expect('the open edge without its record count', 'R7',
           vv=v.replace('s_txcWin,\n                                    R.records);\n    }\n    return;',
                        's_txcWin,\n                                    0);\n    }\n    return;', 1))
    expect('close edge after the hook', 'R7',
           vv=v.replace('        sr::kernel::txc::onWindowEdge(false,', '        (void)(false,', 1))
    expect('bindBegin after the snapshot', 'R8',
           oo=o.replace('const sr::kernel::txc::BindToken txcTok = sr::kernel::txc::bindBegin();',
                        'sr::kernel::txc::BindToken txcTok;', 1))
    expect('the alias is not recorded', 'R8', oo=o.replace('txcWindow = ap.window;', '', 1))
    expect('the decode drops the alias', 'R8', oo=o.replace('txcIssued, txcWindow);', 'txcIssued, 0u);', 1))
    expect('a registration that does not bump the generation', 'R9',
           pp=p.replace('  registryOf(kind)[va] = GrantRec{va + size, serial, 0};\n  g_grantGen.fetch_add(1, std::memory_order_acq_rel);\n',
                        '  registryOf(kind)[va] = GrantRec{va + size, serial, 0};\n', 1))
    expect('a reclaim reported before its erase', 'R9',
           pp=p.replace('      g_physGrants.erase(it);\n      g_grantGen.fetch_add(1, std::memory_order_acq_rel);\n      if (txc::enabled()) txc::onGrantRel(2, va, serial, 2, txcP0);',
                        '      if (txc::enabled()) txc::onGrantRel(2, va, serial, 2, txcP0);\n      g_physGrants.erase(it);\n      g_grantGen.fetch_add(1, std::memory_order_acq_rel);', 1))
    expect('a heap-routed release without its report', 'R9',
           pp=p.replace('        if (txc::enabled()) txc::onGrantRel(1, va, ht->second.serial, 1, txcP0);', '', 1))
    expect('a decode outside g_obs', 'R10',
           cc=c.replace('  std::lock_guard<std::mutex> obs(g_obs);\n', '', 1))
    expect('decode P1 read before the event mutex', 'R10',
           cc=c.replace('  std::lock_guard<std::mutex> lk(g_mu);\n  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();   // at the append',
                        '  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();\n  std::lock_guard<std::mutex> lk(g_mu);', 1))
    expect('decode P1 read before the hook', 'R10',
           cc=c.replace('  if (void (*hook)() = g_decodeHook.load(std::memory_order_relaxed)) hook();\n  std::lock_guard<std::mutex> lk(g_mu);\n  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();   // at the append',
                        '  const uint64_t p1x = sr::gfx::hle::txcFifoPublished();\n  if (void (*hook)() = g_decodeHook.load(std::memory_order_relaxed)) hook();\n  std::lock_guard<std::mutex> lk(g_mu);\n  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();   // at the append', 1))
    expect('grant P1 read before the event mutex', 'R10',
           cc=c.replace('  if (!resolveOn()) return;\n  std::lock_guard<std::mutex> lk(g_mu);\n  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();\n  appendLocked(Ev{kGrantReg',
                        '  if (!resolveOn()) return;\n  const uint64_t p1 = sr::gfx::hle::txcFifoPublished();\n  std::lock_guard<std::mutex> lk(g_mu);\n  appendLocked(Ev{kGrantReg', 1))
    expect('the flush does not wait for in-flight observations', 'R10',
           cc=c.replace('  while (g_inflight.load(std::memory_order_acquire) > 0) {', '  while (false) {', 1))
    expect('the flush writes outside the registry lock', 'R10',
           cc=c.replace('  sr::kernel::physwin::withRegistryShared(&flushUnderRegistry, &fc);',
                        '  flushUnderRegistry(&fc);', 1))
    expect('bindBegin does not count', 'R10',
           cc=c.replace('  g_inflight.fetch_add(1, std::memory_order_acq_rel);\n', '', 1))
    expect('~BindToken does not end its count', 'R10',
           cc=c.replace('  if (on) g_inflight.fetch_sub(1, std::memory_order_acq_rel);', '  (void)on;', 1))
    print('[TXC_TAPS_SELFTEST] %d failed' % fails)
    return fails


def main(argv):
    if len(argv) > 1 and argv[1] == '--self-test':
        return 1 if self_test() else 0
    errs = check(*load())
    for e in errs:
        print('VIOLATION ' + e)
    print('[TXC_TAPS] %d violations' % len(errs))
    return 1 if errs else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
