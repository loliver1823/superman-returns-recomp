"""HR1 evidence contract (iter-458, Astra 072/073): routing and provisioning
verdicts for ONE capture, from three artifacts and nothing else. FAIL-CLOSED:
anything malformed, missing, duplicated, contradictory or unbound is
INCONCLUSIVE, never a pass.

  * The run log's `[GPU_PHYS_ROUTE] CAPTURE SNAPSHOT` line. Taken once, INSIDE
    the critical section that seals H-UNIQ (kernel/phys_window.cpp
    uniqOnRecorderClose: registry lock, then HR1's accounting lock), it names
    that seal (nonce, seq). Every occurrence of the marker is counted; exactly
    one is allowed, and it must parse completely: fixed field order, canonical
    decimal integers (no sign, no leading zero, no hex), nothing trailing.
  * The SEALED H-UNIQ record, accepted with the launcher-supplied nonce
    (tools/uniq_sticky_read.py accept(); values read from the seal itself). The
    snapshot must name it: atSeal=1, nonce == the expected nonce == the seal's,
    sealSeq == the seal's seq. Otherwise the snapshot is not bound.
  * The driver log: exactly one `RESULT:` occurrence, and it must be exactly
    `RESULT: SUCCESS after <n> attempt(s)`.

  Snapshot validity (all required, else every row is INCONCLUSIVE):
    one marker, strict parse, why=recorder-closed, gate=1, bound to the
    accepted seal, counters consistent (outstanding == admitted - settled,
    settled <= admitted, started <= admitted,
    null + delegatedBelowA <= completed), CONSERVED (Astra 074: at the
    zero-outstanding endpoint completed == started - no delegation settled
    without a recorded guest return; heapAfterClaim + heapSpanning <=
    admitted - started - intercepted heap outcomes fit the non-delegated
    population) and outstanding == 0.

  H-HR1-ROUTING  (requests ADMITTED after the ring claim; one admission
                 observation per request)
    KILLED        heapAfterClaim > 0.
    SURVIVES      heapAfterClaim == 0 over >= 1 delegation (spanning reported).
    SURVIVES (vacuous)  heapAfterClaim == 0 but 0 delegations - no exposure.
  H-HR1-BUDGET   ADEQUATE means only that the DECLARED bounds passed for the
                 accepted capture - not that allocation semantics, alignment,
                 ownership or contents are correct:
                   sealed physBelowA=0, stackBelowA=0, minPhysRegionLo >= A0000000;
                   snapshot null=0, delegatedBelowA=0, mmTruncated=0, mmWrapped=0.
                 INADEQUATE names each violation.
  H-HR1-SUFFICIENT
    ELIGIBLE for the visual kill test only with ROUTING SURVIVES (non-vacuous),
    BUDGET ADEQUATE and the driver's single SUCCESS. The visual verdict itself
    belongs to the blind review of both capture methods, not to this tool.

Usage: python tools/hr1_witness.py --log run.log --uniq uniq.bin --nonce HEX --driver driver.txt [--json out.json]
       python tools/hr1_witness.py --self-test
       python tools/hr1_witness.py --integration build/difftest.exe
"""
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import uniq_sticky_read as U  # noqa: E402

MARKER = '[GPU_PHYS_ROUTE] CAPTURE SNAPSHOT'
_INT = r'(0|[1-9][0-9]{0,18})'
COUNTERS = ('admitted', 'settled', 'outstanding', 'started', 'completed', 'null',
            'delegatedBelowA', 'heapAfterClaim', 'heapSpanning', 'mmTruncated', 'mmWrapped')
SNAP = re.compile(
    r'\[GPU_PHYS_ROUTE\] CAPTURE SNAPSHOT why=(\S+) atSeal=([01]) nonce=([0-9A-F]{8}) '
    r'sealSeq=' + _INT + r' gate=([01]) ' +
    ' '.join('%s=%s' % (k, _INT) for k in COUNTERS) + r'[ \t]*$')
DRIVER_OK = re.compile(r'^\[[0-9:.]+\] RESULT: SUCCESS after [1-9][0-9]* attempt\(s\)[ \t]*$', re.M)


def parse_snapshots(log_text):
    """-> (marker count, [parsed dict or None per marker line])."""
    lines = [l for l in log_text.splitlines() if MARKER in l]
    out = []
    for l in lines:
        m = SNAP.search(l)
        if not m:
            out.append(None)
            continue
        g = m.groups()
        d = {'why': g[0], 'atSeal': int(g[1]), 'nonce': int(g[2], 16), 'sealSeq': int(g[3]),
             'gate': int(g[4])}
        for k, v in zip(COUNTERS, g[5:]):
            d[k] = int(v)
        out.append(d)
    return log_text.count(MARKER), out


def driver_success(driver_text):
    t = driver_text or ''
    return t.count('RESULT:') == 1 and len(DRIVER_OK.findall(t)) == 1


def analyze(log_text, record, seal, nonce, driver_text):
    res = {'snapshot': None, 'routing': 'INCONCLUSIVE', 'budget': 'INCONCLUSIVE',
           'sufficient': 'INCONCLUSIVE', 'why': {}, 'uniqAccepted': False}
    ok, seq, bad = U.accept(record or b'', seal, nonce, log_text)
    res['uniqAccepted'] = ok
    count, snaps = parse_snapshots(log_text)
    problems = []
    s = None
    if count != 1 or len(snaps) != 1:
        problems.append('%d snapshot marker(s) (need exactly one)' % count)
    elif snaps[0] is None:
        problems.append('the snapshot line does not parse strictly')
    else:
        s = snaps[0]
        res['snapshot'] = s
        if s['why'] != 'recorder-closed':
            problems.append("why=%s (need 'recorder-closed')" % s['why'])
        if s['gate'] != 1:
            problems.append('gate=%d (the run did not have SR_GPU_PHYS_ROUTE on)' % s['gate'])
        if s['atSeal'] != 1:
            problems.append('atSeal=0 (not taken at the H-UNIQ seal)')
        if not ok:
            problems.append('H-UNIQ record not accepted: ' + '; '.join(bad))
        else:
            sl = U.read_seal(seal)
            if s['nonce'] != nonce or s['nonce'] != sl['nonce']:
                problems.append('snapshot nonce %08X is not the accepted acquisition %08X' %
                                (s['nonce'], sl['nonce']))
            if s['sealSeq'] != sl['seq']:
                problems.append('snapshot sealSeq %d != the seal seq %d' % (s['sealSeq'], sl['seq']))
        # (completed <= started is not a separate clause: at zero outstanding the
        # conservation rule below requires completed == started, and any
        # outstanding request is INCONCLUSIVE on its own - W24 covers it.)
        if s['outstanding'] != s['admitted'] - s['settled'] or s['settled'] > s['admitted'] or \
                s['started'] > s['admitted'] or \
                s['null'] + s['delegatedBelowA'] > s['completed']:
            problems.append('inconsistent counters')
        # Astra 074: conservation. At a zero-outstanding endpoint every delegated
        # request recorded its guest return, and the intercepted heap outcomes
        # (after-claim, spanning) fit inside the NON-delegated population.
        if s['outstanding'] == 0 and s['completed'] != s['started']:
            problems.append('%d delegation(s) settled without a recorded guest return' %
                            (s['started'] - s['completed']))
        if s['heapAfterClaim'] + s['heapSpanning'] > s['admitted'] - min(s['started'], s['admitted']):
            problems.append('intercepted heap counts %d exceed the non-delegated population %d' %
                            (s['heapAfterClaim'] + s['heapSpanning'],
                             s['admitted'] - min(s['started'], s['admitted'])))
        if s['outstanding']:
            problems.append('%d request(s) outstanding at the seal' % s['outstanding'])
    if problems:
        for k in ('routing', 'budget', 'sufficient'):
            res['why'][k] = 'invalid snapshot: ' + '; '.join(problems)
        return res
    if s['heapAfterClaim']:
        res['routing'] = 'KILLED'
        res['why']['routing'] = '%d heap-routed grant(s) admitted after the claim' % s['heapAfterClaim']
    elif s['completed'] == 0:
        res['routing'] = 'SURVIVES (vacuous: 0 delegations)'
        res['why']['routing'] = 'no treatment exposure'
    else:
        res['routing'] = 'SURVIVES'
        res['why']['routing'] = ('0 heap-routed grants admitted after the claim over %d delegations '
                                 '(%d spanning the claim, reported)' %
                                 (s['completed'], s['heapSpanning']))
    r = U.decode_record(U.read_seal(seal)['rec'])
    viol = []
    if r['physBelowA']:
        viol.append('sealed physBelowA=%d' % r['physBelowA'])
    if r['stackBelowA']:
        viol.append('sealed stackBelowA=%d' % r['stackBelowA'])
    if r['minPhysRegionLo'] < 0xA0000000:
        viol.append('sealed minPhysRegionLo=%08X < A0000000' % r['minPhysRegionLo'])
    for k in ('null', 'delegatedBelowA', 'mmTruncated', 'mmWrapped'):
        if s[k]:
            viol.append('%s=%d' % (k, s[k]))
    res['sealed'] = {'physBelowA': r['physBelowA'], 'stackBelowA': r['stackBelowA'],
                     'minPhysRegionLo': '%08X' % r['minPhysRegionLo'], 'seq': seq}
    res['budget'] = 'INADEQUATE' if viol else 'ADEQUATE'
    res['why']['budget'] = ('; '.join(viol) if viol else
                            'the declared bounds passed for this accepted capture')
    gameplay = driver_success(driver_text)
    res['gameplay'] = gameplay
    if res['routing'] == 'SURVIVES' and res['budget'] == 'ADEQUATE' and gameplay:
        res['sufficient'] = "ELIGIBLE (the visual verdict is the blind review's)"
    else:
        res['why']['sufficient'] = ('routing %s, budget %s, driver single SUCCESS %s' %
                                    (res['routing'], res['budget'], gameplay))
    return res


_analyze = analyze   # the self-test wraps it (per-case exception isolation)


# ── self-test ──────────────────────────────────────────────────────────────
N = 0x6A11C0DF


def _snapline(**kw):
    d = {'why': 'recorder-closed', 'atSeal': 1, 'nonce': '%08X' % N, 'sealSeq': 7, 'gate': 1,
         'admitted': 41, 'settled': 41, 'started': 40, 'completed': 40, 'null': 0,
         'delegatedBelowA': 0, 'heapAfterClaim': 0, 'heapSpanning': 1, 'mmTruncated': 0,
         'mmWrapped': 0}
    d.update(kw)
    if 'outstanding' not in kw:
        try:
            d['outstanding'] = int(d['admitted']) - int(d['settled'])
        except (TypeError, ValueError):
            d['outstanding'] = 0
    return ('[2026-09-25 12:00:00.000] [WARN ] [BOOT] [GPU_PHYS_ROUTE] CAPTURE SNAPSHOT '
            'why=%(why)s atSeal=%(atSeal)s nonce=%(nonce)s sealSeq=%(sealSeq)s gate=%(gate)s '
            'admitted=%(admitted)s settled=%(settled)s outstanding=%(outstanding)s '
            'started=%(started)s completed=%(completed)s null=%(null)s '
            'delegatedBelowA=%(delegatedBelowA)s heapAfterClaim=%(heapAfterClaim)s '
            'heapSpanning=%(heapSpanning)s mmTruncated=%(mmTruncated)s mmWrapped=%(mmWrapped)s\n'
            % d)


def _rec(physBelowA=0, stackBelowA=0, minLo=0xAB4FF000):
    b = bytearray(U.REC)
    struct.pack_into('<Q', b, 0, 4000)
    struct.pack_into('<Q', b, 248, physBelowA)
    struct.pack_into('<Q', b, 256, stackBelowA)
    struct.pack_into('<I', b, 268, minLo)
    return bytes(b)


def _uniq(nonce=N, seq=7, **kw):
    rec = _rec(**kw)
    return U._file([(seq, rec, seq)], nonce), U._seal(nonce, seq, rec)


OK_DRV = '[12:00:00.000] RESULT: SUCCESS after 1 attempt(s)\n'


def self_test():
    fails = 0

    def chk(ok, name):
        nonlocal fails
        print('  %s  %s' % ('ok' if ok else 'FAIL:', name))
        fails += not ok

    def analyze(*a):
        # Per-case isolation: an exception fails THAT case by name (its checks
        # see the EXCEPTION verdicts) instead of ending the whole self-test.
        try:
            return _analyze(*a)
        except Exception as e:   # noqa: BLE001 - reported, never swallowed
            print('      (exception in analyze: %s: %s)' % (type(e).__name__, e))
            return {'routing': 'EXCEPTION', 'budget': 'EXCEPTION', 'sufficient': 'EXCEPTION',
                    'why': {'routing': '', 'budget': '', 'sufficient': ''}, 'uniqAccepted': None}

    rec, seal = _uniq()

    def inconclusive(r):
        return r['routing'] == 'INCONCLUSIVE' and r['budget'] == 'INCONCLUSIVE' and \
            r['sufficient'] == 'INCONCLUSIVE'

    r = analyze(_snapline(), rec, seal, N, OK_DRV)
    chk(r['routing'] == 'SURVIVES' and r['budget'] == 'ADEQUATE' and
        r['sufficient'].startswith('ELIGIBLE'), 'W1 a clean bound capture: SURVIVES, ADEQUATE, ELIGIBLE')
    # (Astra 074: every synthetic population is conserved - the intercepted
    # heap outcomes fit the admitted - started non-delegated requests.)
    r = analyze(_snapline(admitted=43, settled=43, heapAfterClaim=2), rec, seal, N, OK_DRV)
    chk(r['routing'] == 'KILLED' and r['sufficient'] == 'INCONCLUSIVE',
        'W2 a heap-routed grant admitted after the claim KILLS routing')
    r = analyze(_snapline(admitted=45, settled=45, heapSpanning=5), rec, seal, N, OK_DRV)
    chk(r['routing'] == 'SURVIVES', 'W3 spanning requests are reported, never a kill')
    chk(inconclusive(analyze('', rec, seal, N, OK_DRV)), 'W4 no snapshot: INCONCLUSIVE')
    chk(inconclusive(analyze(_snapline() + _snapline(), rec, seal, N, OK_DRV)),
        'W5 two snapshots: INCONCLUSIVE')
    chk(inconclusive(analyze(_snapline(admitted=42), rec, seal, N, OK_DRV)),
        'W6 a request outstanding at the seal: INCONCLUSIVE')
    chk(inconclusive(analyze(_snapline(admitted=42, outstanding=0), rec, seal, N, OK_DRV)),
        'W7 an inconsistent snapshot (outstanding != admitted - settled): INCONCLUSIVE')
    chk(inconclusive(analyze(_snapline(gate=0), rec, seal, N, OK_DRV)),
        'W8 gate off: INCONCLUSIVE for HR1')
    for k in ('null', 'delegatedBelowA', 'mmTruncated', 'mmWrapped'):
        r = analyze(_snapline(**{k: 1}), rec, seal, N, OK_DRV)
        chk(r['budget'] == 'INADEQUATE' and k in r['why']['budget'] and
            r['sufficient'] == 'INCONCLUSIVE', 'W9 %s > 0: INADEQUATE, named' % k)
    for kw, name in ((dict(physBelowA=1), 'physBelowA'), (dict(stackBelowA=1), 'stackBelowA'),
                     (dict(minLo=0x9FFFF000), 'minPhysRegionLo')):
        rc, sl = _uniq(**kw)
        r = analyze(_snapline(), rc, sl, N, OK_DRV)
        chk(r['budget'] == 'INADEQUATE' and name in r['why']['budget'],
            'W10 sealed %s violated: INADEQUATE, named' % name)
    chk(inconclusive(analyze(_snapline(), rec, None, N, OK_DRV)), 'W11 no seal: INCONCLUSIVE')
    chk(inconclusive(analyze(_snapline(), rec, seal, 0x11111111, OK_DRV)),
        "W12 another acquisition's nonce: INCONCLUSIVE")
    rec_bad = U._file([(7, _rec(physBelowA=3), 7)], N)
    chk(inconclusive(analyze(_snapline(), rec_bad, seal, N, OK_DRV)),
        'W13 a record that is not the sealed state is refused')
    r = analyze(_snapline(), rec, seal, N, '[12:00:00.000] RESULT: FAILURE after 1 attempt(s)\n')
    chk(r['routing'] == 'SURVIVES' and r['sufficient'] == 'INCONCLUSIVE',
        'W14 no gameplay: SUFFICIENT is INCONCLUSIVE whatever routing says')
    # Astra 073's false-eligibility inputs
    chk(inconclusive(analyze(_snapline() + _snapline(started='broken'), rec, seal, N, OK_DRV)),
        'W15 a second, malformed snapshot is still counted: INCONCLUSIVE')
    chk(inconclusive(analyze(_snapline(mmWrapped='0x1'), rec, seal, N, OK_DRV)),
        'W16 mmWrapped=0x1 does not parse as zero: INCONCLUSIVE')
    chk(inconclusive(analyze(_snapline(why='not-a-close'), rec, seal, N, OK_DRV)),
        'W17 a snapshot not taken at the recorder close: INCONCLUSIVE')
    r = analyze(_snapline(), rec, seal, N,
                OK_DRV + '[12:01:00.000] RESULT: FAILURE after 1 attempt(s)\n')
    chk(r['sufficient'] == 'INCONCLUSIVE' and not r.get('gameplay'),
        'W18 SUCCESS followed by FAILURE is contradictory: not gameplay')
    r = analyze(_snapline(), rec, seal, N, '[12:00:00.000] RESULT: SUCCESSFUL_NOT_REALLY\n')
    chk(r['sufficient'] == 'INCONCLUSIVE' and not r.get('gameplay'),
        'W19 RESULT: SUCCESSFUL_NOT_REALLY is not SUCCESS')
    r = analyze(_snapline(admitted=0, settled=0, started=0, completed=0, heapSpanning=0),
                rec, seal, N, OK_DRV)
    chk(r['routing'] == 'SURVIVES (vacuous: 0 delegations)' and r['sufficient'] == 'INCONCLUSIVE',
        'W20 zero delegations: routing is labelled vacuous and never ELIGIBLE')
    rc_b, sl_b = _uniq(nonce=0x0BADC0DE)
    chk(inconclusive(analyze(_snapline(), rc_b, sl_b, 0x0BADC0DE, OK_DRV)),
        "W21 this snapshot beside ANOTHER acquisition's accepted record/seal: INCONCLUSIVE")
    chk(inconclusive(analyze(_snapline(sealSeq=6), rec, seal, N, OK_DRV)),
        "W22 a snapshot naming another seal seq: INCONCLUSIVE")
    chk(inconclusive(analyze(_snapline(atSeal=0), rec, seal, N, OK_DRV)),
        'W23 a snapshot not taken at the seal (atSeal=0): INCONCLUSIVE')
    chk(inconclusive(analyze(_snapline(completed=41), rec, seal, N, OK_DRV)),
        'W24 more completions than delegations: INCONCLUSIVE')
    chk(inconclusive(analyze(_snapline(mmWrapped='0 extra'), rec, seal, N, OK_DRV)),
        'W25 trailing text after the last field: INCONCLUSIVE')
    # Astra 074: conservation
    chk(inconclusive(analyze(_snapline(completed=38), rec, seal, N, OK_DRV)),
        'W26 delegations settled without a recorded guest return (completed < started at zero '
        'outstanding): INCONCLUSIVE')
    chk(inconclusive(analyze(_snapline(heapSpanning=2), rec, seal, N, OK_DRV)),
        'W27 intercepted heap counts beyond the non-delegated population: INCONCLUSIVE')
    r = analyze(_snapline(admitted=42, settled=42, heapAfterClaim=1, heapSpanning=1), rec, seal,
                N, OK_DRV)
    chk(r['routing'] == 'KILLED',
        'W28 a conserved population at the bound (2 heap outcomes, 2 non-delegated) still KILLS')
    print('[HR1_WITNESS_SELFTEST] %d failed' % fails)
    return fails


def integration(difftest):
    """The C++ snapshot line and this parser are one contract: run the real
    suite, parse the snapshots its REAL close edge emitted, and judge G4's and
    G6's lines against the seals the suite kept."""
    import subprocess
    fails = 0

    def chk(ok, name):
        nonlocal fails
        print('  %s  %s' % ('ok' if ok else 'FAIL:', name))
        fails += not ok

    r = subprocess.run([os.path.abspath(difftest), '--gpu-phys-route-test'], capture_output=True,
                       text=True, errors='replace', timeout=600)
    out = r.stdout + r.stderr
    chk(r.returncode == 0 and re.search(r'\[GPU_PHYS_ROUTE_TEST\] \d+ passed, 0 failed', out),
        'I0 the C++ suite passes')
    lines = [l for l in out.splitlines() if MARKER in l]
    parsed = [parse_snapshots(l)[1][0] for l in lines]
    chk(len(lines) == 6 and all(p is not None for p in parsed),
        "I1 the suite's six capture snapshots (G0 G1 G2 G2b G4 G6) all parse strictly: %d" %
        len(lines))
    if len(lines) == 6 and all(p is not None for p in parsed):
        g0, g1, g2, g2b, g4, g6 = parsed
        chk(g0['gate'] == 1 and g0['completed'] == 1 and g0['atSeal'] == 0,
            "I2 G0: the env gate's snapshot (1 delegation, no seal)")
        chk(g1['admitted'] == 9 and g1['settled'] == 9 and g1['outstanding'] == 0,
            'I3 G1: all nine, none outstanding')
        chk(g2['outstanding'] == 1 and g2b['outstanding'] == 1 and g2b['completed'] == 0,
            'I4 G2 and G2b: the request outstanding at the close (inside the guest; right after '
            'admission)')
        tmp = os.environ.get('TEMP', '.')

        def load(n):
            try:
                return open(os.path.join(tmp, n), 'rb').read()
            except OSError:
                return None
        r4 = analyze(lines[4], load('sr_hr1_g4_uniq.bin'), load('sr_hr1_g4_uniq.bin.seal'),
                     0x6A11C0DE, OK_DRV)
        chk(g4['atSeal'] == 1 and g4['outstanding'] == 1 and r4['uniqAccepted'] and
            r4['routing'] == 'INCONCLUSIVE',
            'I5 G4: bound to its real accepted seal, one request outstanding -> INCONCLUSIVE')
        r6 = analyze(lines[5], load('sr_hr1_uniq.bin'), load('sr_hr1_uniq.bin.seal'),
                     0x6A11C0DF, OK_DRV)
        chk(g6['atSeal'] == 1 and r6['uniqAccepted'] and r6['routing'] == 'SURVIVES',
            'I6 G6: a clean capture bound to its real accepted seal -> routing SURVIVES (budget %s)'
            % r6['budget'])
        r6x = analyze(lines[5], load('sr_hr1_g4_uniq.bin'), load('sr_hr1_g4_uniq.bin.seal'),
                      0x6A11C0DE, OK_DRV)
        chk(r6x['routing'] == 'INCONCLUSIVE',
            "I7 G6's snapshot beside G4's accepted seal (substitution across acquisitions) -> "
            'INCONCLUSIVE')
        # Astra 074's two conservation counterexamples, on the REAL G6 line and
        # its unchanged accepted record/seal.
        g6rec, g6seal = load('sr_hr1_uniq.bin'), load('sr_hr1_uniq.bin.seal')
        m1 = lines[5].replace(' completed=3 ', ' completed=1 ')
        m2 = lines[5].replace(' heapSpanning=0 ', ' heapSpanning=999 ')
        r8 = analyze(m1, g6rec, g6seal, 0x6A11C0DF, OK_DRV)
        chk(m1 != lines[5] and r8['routing'] == r8['budget'] == r8['sufficient'] == 'INCONCLUSIVE',
            'I8 real G6 with completed=3 -> 1 (delegations without a recorded return) -> '
            'INCONCLUSIVE')
        r9 = analyze(m2, g6rec, g6seal, 0x6A11C0DF, OK_DRV)
        chk(m2 != lines[5] and r9['routing'] == r9['budget'] == r9['sufficient'] == 'INCONCLUSIVE',
            'I9 real G6 with heapSpanning=0 -> 999 (heap outcomes with no non-delegated request) '
            '-> INCONCLUSIVE')
    print('[HR1_WITNESS_INTEGRATION] %d failed' % fails)
    return fails


def main(argv):
    if len(argv) > 1 and argv[1] == '--self-test':
        return 1 if self_test() else 0
    if len(argv) > 2 and argv[1] == '--integration':
        return 1 if integration(argv[2]) else 0
    a = dict(zip(argv[1::2], argv[2::2]))
    need = ('--log', '--uniq', '--nonce', '--driver')
    if any(k not in a for k in need):
        print(__doc__)
        return 2

    def load(p, text=False):
        try:
            return open(p, encoding='utf-8', errors='replace').read() if text else open(p, 'rb').read()
        except OSError:
            return None
    res = analyze(load(a['--log'], True) or '', load(a['--uniq']), load(a['--uniq'] + '.seal'),
                  int(a['--nonce'], 16), load(a['--driver'], True) or '')
    for k in ('routing', 'budget', 'sufficient'):
        print('H-HR1-%-11s %s  (%s)' % (k.upper(), res[k], res['why'].get(k, '')))
    if '--json' in a:
        with open(a['--json'], 'w', encoding='utf-8') as f:
            json.dump(res, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
