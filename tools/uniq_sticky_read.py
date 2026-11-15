"""Read and ACCEPT the H-UNIQ durable census record (iter-458, Astra 060-062).

SR_PHYS_UNIQ_STICKY_FILE=<path> (with SR_PHYS_UNIQ_CENSUS=1) makes the game
publish its cumulative census state at every registration into two
alternating slots, so a TerminateProcess'd run still leaves the state after its
last COMPLETED update. Layout (little-endian; kernel/phys_window.hpp):

  record  header  u32 magic 'UQS1', u32 version 1, u32 recBytes (320), u32 nonce
          slot x2 u64 seq, record[recBytes], u64 seq   (complete iff both match)
  seal    <record>.seal: u32 'UQSE', u32 1, u32 nonce, u32 publicationFailed,
          u64 seq, u64 failSeq, u32 recBytes, u32 0, record[recBytes],
          u64 FNV-1a-64 of everything before it

Publication is fail-closed (Astra 061): the first failed write latches, stops
publication and poisons the header ('UQSX'). The SEAL (Astra 062) is written
when the live SRHT recorder first closes - the end of the acquisition - and
freezes publication. Terminal acceptance needs POSITIVE evidence; every failed
rule is named and anything missing rejects:
  S-present  the seal exists, is complete, and its checksum verifies
  S-ok       the seal records no publication failure
  R-decode   the record decodes (not poisoned, a complete slot exists)
  S-expected the seal nonce == the EXPECTED nonce the launcher supplied
             (SR_PHYS_UNIQ_NONCE; never read back from the output) - a stale
             pair at a reused path belongs to another acquisition
  S-nonce    seal nonce == record header nonce (the same acquisition), nonzero
  S-match    seal seq == record seq and the record bytes are identical
  L-nofail   (only when a log is given) no [PHYS_UNIQ_STICKY] failure line
A log can be truncated, so it can only ADD refusals - never supply acceptance.

Output of the plain read is the same key=value text as
`build/difftest.exe --uniq-sticky-dump`; the two decoders must agree.

Usage: python tools/uniq_sticky_read.py <record>
       python tools/uniq_sticky_read.py --accept <record> <expectedNonceHex> [<run.log>]
       python tools/uniq_sticky_read.py --self-test
       python tools/uniq_sticky_read.py --integration build/difftest.exe
"""
import os
import struct
import subprocess
import sys

MAGIC = 0x31535155
POISON = 0x58535155
SEAL_MAGIC = 0x45535155
REC = 320
HDR = 16
SLOT = 8 + REC + 8
SEAL_HDR = 40
SEAL = SEAL_HDR + REC + 8
PAIRS = ['11', '12', '13', '22', '23', '33']


def fnv1a64(b):
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def decode_record(b):
    o = 0

    def u64():
        nonlocal o
        v = struct.unpack_from('<Q', b, o)[0]
        o += 8
        return v

    def u32():
        nonlocal o
        v = struct.unpack_from('<I', b, o)[0]
        o += 4
        return v

    r = {'registrations': u64()}
    r['llPairs'] = [u64() for _ in range(6)]
    r['llBytes'] = [u64() for _ in range(6)]
    r['exNew'] = [(u32(), u32(), u32()) for _ in range(6)]
    r['exOld'] = [(u32(), u32(), u32()) for _ in range(6)]
    r['physBelowA'] = u64()
    r['stackBelowA'] = u64()
    r['firstBelowAVA'] = u32()
    r['minPhysRegionLo'] = u32()
    r['heapSpanPairs'] = u64()
    r['heapSpanBytes'] = u64()
    r['heapSpanBytesMaxTick'] = u64()
    r['registryOverlaps'] = [u64() for _ in range(3)]
    assert o == REC, o
    return r


def read_raw(data):
    """-> (seq, completeSlots, recordBytes) or raises ValueError."""
    if len(data) < HDR:
        raise ValueError('short header')
    magic, ver, rec, _ = struct.unpack_from('<IIII', data, 0)
    if magic == POISON:
        raise ValueError('poisoned (a publication failed)')
    if magic != MAGIC or ver != 1 or rec != REC:
        raise ValueError('bad header magic=%08X ver=%d rec=%d' % (magic, ver, rec))
    best, complete = None, 0
    for k in range(2):
        off = HDR + k * SLOT
        if len(data) < off + SLOT:
            continue
        a = struct.unpack_from('<Q', data, off)[0]
        b = struct.unpack_from('<Q', data, off + 8 + REC)[0]
        if a == 0 or a != b:
            continue
        complete += 1
        if best is None or a > best[0]:
            best = (a, bytes(data[off + 8: off + 8 + REC]))
    if best is None:
        raise ValueError('no complete slot')
    return best[0], complete, best[1]


def read(data):
    seq, complete, raw = read_raw(data)
    return seq, complete, decode_record(raw)


def record_nonce(data):
    if len(data) < HDR or struct.unpack_from('<I', data, 0)[0] != MAGIC:
        return 0
    return struct.unpack_from('<I', data, 12)[0]


def read_seal(data):
    """-> dict or raises ValueError (truncated / magic / checksum)."""
    if len(data) != SEAL:
        raise ValueError('seal size %d != %d' % (len(data), SEAL))
    magic, ver, nonce, failed, seq, fseq, rec, _ = struct.unpack_from('<IIIIQQII', data, 0)
    if magic != SEAL_MAGIC or ver != 1 or rec != REC:
        raise ValueError('seal header magic=%08X ver=%d rec=%d' % (magic, ver, rec))
    if struct.unpack_from('<Q', data, SEAL_HDR + REC)[0] != fnv1a64(data[:SEAL_HDR + REC]):
        raise ValueError('seal checksum mismatch')
    return {'nonce': nonce, 'failed': failed, 'seq': seq, 'failSeq': fseq,
            'rec': bytes(data[SEAL_HDR: SEAL_HDR + REC])}


def accept(record, seal, expected, log_text=None):
    """record/seal: bytes (seal None = missing); expected: the launcher-supplied
    acquisition nonce (int; 0/None = rejected). -> (accepted, seq, [failed rules])."""
    bad = []
    if not expected:
        bad.append('S-expected: no expected acquisition nonce supplied')
    s = None
    if seal is None:
        bad.append('S-present: no seal')
    else:
        try:
            s = read_seal(seal)
        except ValueError as e:
            bad.append('S-present: %s' % e)
    if s is not None and s['failed']:
        bad.append('S-ok: the seal records a publication failure (at seq %d)' % s['failSeq'])
    seq = raw = None
    try:
        seq, _, raw = read_raw(record)
    except ValueError as e:
        bad.append('R-decode: %s' % e)
    if s is not None:
        n = record_nonce(record)
        if n == 0 or s['nonce'] != n:
            bad.append('S-nonce: seal %08X vs record %08X (another acquisition)' % (s['nonce'], n))
        if expected and s['nonce'] != expected:
            bad.append('S-expected: seal %08X is not the expected acquisition %08X' % (s['nonce'], expected))
        if raw is not None and (seq != s['seq'] or raw != s['rec']):
            bad.append('S-match: record seq %d is not the sealed state (seal seq %d)' % (seq, s['seq']))
    if log_text is not None:
        fails = [l for l in log_text.splitlines() if '[PHYS_UNIQ_STICKY]' in l and
                 ('PUBLICATION FAILED' in l or 'FAILED to open' in l or 'SEAL FAILED' in l)]
        if fails:
            bad.append('L-nofail: %d failure line(s), first: %s' % (len(fails), fails[0].strip()[:160]))
    return (not bad), seq, bad


def fmt(seq, complete, r):
    out = ['seq=%d complete=%d' % (seq, complete), 'registrations=%d' % r['registrations']]
    for i, p in enumerate(PAIRS):
        n, o = r['exNew'][i], r['exOld'][i]
        out.append('ll%s=%d/0x%X new=%08X+0x%X/s%d old=%08X+0x%X/s%d' % (
            p, r['llPairs'][i], r['llBytes'][i], n[0], n[1], n[2], o[0], o[1], o[2]))
    out.append('physBelowA=%d stackBelowA=%d firstBelowAVA=%08X' % (
        r['physBelowA'], r['stackBelowA'], r['firstBelowAVA']))
    out.append('minPhysRegionLo=%08X' % r['minPhysRegionLo'])
    out.append('heapSpanPairs=%d heapSpanBytes=0x%X heapSpanBytesMaxTick=0x%X' % (
        r['heapSpanPairs'], r['heapSpanBytes'], r['heapSpanBytesMaxTick']))
    out.append('registryOverlaps=%d,%d,%d' % tuple(r['registryOverlaps']))
    return '\n'.join(out)


def _load(path):
    try:
        return open(path, 'rb').read()
    except OSError:
        return None


def integration(difftest):
    """Run the real kernel tests, then judge F3's double-fault acquisition with a
    log TRUNCATED before its failure line (Astra 062's regression)."""
    r = subprocess.run([os.path.abspath(difftest), '--phys-uniq-test'], capture_output=True, text=True,
                       errors='replace')
    ok_suite = '[PHYS_UNIQ_TEST]' in r.stdout and ', 0 failed' in r.stdout
    f3 = os.path.join(os.environ.get('TEMP', '.'), 'sr_uniq_sticky_f3.bin')
    record, seal = _load(f3), _load(f3 + '.seal')
    seq, _, _ = read(record)          # the record ALONE decodes (stale)
    log = r.stdout + '\n' + r.stderr
    cut = log.find('PUBLICATION FAILED')
    truncated = log[:log.rfind('\n', 0, cut) + 1] if cut >= 0 else log
    ok_t, _, bad_t = accept(record, seal, 0xF3F3F3F3, truncated)
    ok_n, _, bad_n = accept(record, None, 0xF3F3F3F3, truncated)
    clean = os.path.join(os.environ.get('TEMP', '.'), 'sr_uniq_sticky_clean.bin')
    # No log: this test process's log also carries OTHER acquisitions' failures.
    ok_c, seq_c, bad_c = accept(_load(clean) or b'', _load(clean + '.seal'), 0xC1EA0001)
    # A later acquisition (nonce ...02) whose output never appeared: the stale pair must not pass.
    ok_s, _, bad_s = accept(_load(clean) or b'', _load(clean + '.seal'), 0xC1EA0002)
    fails = 0
    for cond, name in [
            (ok_suite, 'I1 --phys-uniq-test ran with 0 failed'),
            (seq == 2, 'I2 the double-fault record alone decodes to the stale seq 2'),
            (cut >= 0 and 'PUBLICATION FAILED' not in truncated,
             'I3 the log is truncated BEFORE its failure line (the log alone would not refuse)'),
            (not ok_t and any(b.startswith('S-ok') for b in bad_t),
             'I4 --accept REJECTS it from the seal (S-ok) despite the truncated log'),
            (not ok_n and any(b.startswith('S-present') for b in bad_n),
             'I5 ...and with the seal missing it is REJECTED (S-present)'),
            (ok_c and seq_c == 2 and not bad_c,
             'I6 a clean kernel-sealed acquisition is ACCEPTED by this independent implementation'),
            (not ok_s and any(b.startswith('S-expected') for b in bad_s),
             'I7 ...but that stale pair is REJECTED for a later acquisition (expected nonce)')]:
        print(('  ok  ' if cond else 'FAIL: ') + name)
        fails += 0 if cond else 1
    print('[UNIQ_STICKY_INTEGRATION] %s' % ('0 failed' if not fails else '%d failed' % fails))
    return fails


# ── self-test ────────────────────────────────────────────────────────────────
def _rec(registrations, ll12=0):
    b = bytearray(REC)
    struct.pack_into('<Q', b, 0, registrations)
    struct.pack_into('<Q', b, 8 + 8, ll12)          # llPairs[1]
    struct.pack_into('<I', b, 268, 0xA0001000)      # minPhysRegionLo
    return bytes(b)


def _file(slots, nonce=0x1234567):
    d = bytearray(struct.pack('<IIII', MAGIC, 1, REC, nonce))
    for seq_a, rec, seq_b in slots:
        d += struct.pack('<Q', seq_a) + rec + struct.pack('<Q', seq_b)
    return bytes(d)


def _seal(nonce, seq, rec, failed=0, fseq=0):
    b = struct.pack('<IIIIQQII', SEAL_MAGIC, 1, nonce, failed, seq, fseq, REC, 0) + rec
    return b + struct.pack('<Q', fnv1a64(b))


def self_test():
    fails = 0

    def check(ok, name):
        nonlocal fails
        print(('  ok  ' if ok else 'FAIL: ') + name)
        fails += 0 if ok else 1

    s, c, r = read(_file([(4, _rec(4, 1), 4), (3, _rec(3), 3)]))
    s2, c2, r2 = read(_file([(6, _rec(6), 6), (7, _rec(7, 1), 7)]))
    check(s == 4 and c == 2 and r['registrations'] == 4 and r['llPairs'][1] == 1 and
          s2 == 7 and c2 == 2 and r2['registrations'] == 7 and r2['llPairs'][1] == 1,
          'U1 two complete slots: the higher seq wins in EITHER slot')
    s, c, r = read(_file([(2, _rec(2), 2), (5, _rec(5), 0)]))
    check(s == 2 and c == 1 and r['registrations'] == 2, 'U2 a torn newest slot is ignored')
    s, c, r = read(_file([(6, _rec(6), 6), (5, _rec(5), 5)]))
    check(r['minPhysRegionLo'] == 0xA0001000, 'U3 field offsets: minPhysRegionLo at byte 268')
    poisoned = bytearray(_file([(4, _rec(4), 4), (3, _rec(3), 3)]))
    struct.pack_into('<I', poisoned, 0, POISON)
    for name, data, why in [('U4 no complete slot is an error', _file([(1, _rec(1), 2), (0, _rec(0), 0)]), ''),
                            ('U5 bad magic is an error', b'\0' * (HDR + 2 * SLOT), ''),
                            ('U6 a poisoned header is an error (named: poisoned) despite complete slots',
                             bytes(poisoned), 'poisoned')]:
        try:
            read(data)
            check(False, name)
        except ValueError as e:
            check(why in str(e), name)

    N = 0x1234567
    good = _file([(4, _rec(4, 1), 4), (3, _rec(3), 3)], N)
    seal = _seal(N, 4, _rec(4, 1))

    def has(bad, rule):
        return any(b.startswith(rule) for b in bad)

    ok, s, bad = accept(good, seal, N)
    check(ok and s == 4 and not bad, 'A1 a sealed, matching, failure-free acquisition is accepted')
    ok, _, bad = accept(good, _seal(N, 4, _rec(4, 1), failed=1, fseq=5), N,
                        "[x] [PHYS_UNIQ_STICKY] durable record -> 'a.bin' nonce=01234567\n")
    check(not ok and has(bad, 'S-ok'),
          'A2 a seal recording a failure REJECTS even with a log truncated before its failure line')
    ok, _, bad = accept(bytes(poisoned), seal, N)
    check(not ok and any(b.startswith('R-decode') and 'poisoned' in b for b in bad),
          'A3 a poisoned record is rejected, named as poisoned')
    ok, _, bad = accept(good, None, N)
    check(not ok and has(bad, 'S-present'), 'A4 no seal: rejected (missing evidence)')
    broken = bytearray(seal)
    broken[60] ^= 0x5A
    ok, _, bad = accept(good, bytes(broken), N)
    check(not ok and has(bad, 'S-present'), 'A5 a corrupted seal fails its checksum: rejected')
    ok, _, bad = accept(good, seal[:-1], N)
    check(not ok and has(bad, 'S-present'), 'A5b a truncated seal: rejected')
    ok, _, bad = accept(_file([(4, _rec(4, 1), 4), (3, _rec(3), 3)], N + 2), seal, N)
    check(not ok and has(bad, 'S-nonce'), 'A6 a seal from another acquisition (nonce): rejected')
    ok, _, bad = accept(_file([(4, _rec(4, 1), 4), (5, _rec(5), 5)], N), seal, N)
    check(not ok and has(bad, 'S-match'), 'A7 a record newer than its seal: rejected')
    ok, _, bad = accept(_file([(4, _rec(4, 0), 4), (3, _rec(3), 3)], N), seal, N)
    check(not ok and has(bad, 'S-match'), 'A7b same seq, different record bytes: rejected')
    ok, _, bad = accept(good, seal, N, '[x] [PHYS_UNIQ_STICKY] PUBLICATION FAILED at seq=5 (slot write)\n')
    check(not ok and has(bad, 'L-nofail'), 'A8 a failure line in the log adds a refusal')
    ok, _, bad = accept(good, seal, N + 1)
    check(not ok and has(bad, 'S-expected'), 'A9 a valid pair from ANOTHER acquisition (expected nonce): rejected')
    ok, _, bad = accept(good, seal, 0)
    check(not ok and has(bad, 'S-expected'), 'A10 no expected nonce: rejected')
    print('[UNIQ_STICKY_READ] %s' % ('0 failed' if not fails else '%d failed' % fails))
    return fails


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['--self-test']:
        sys.exit(self_test())
    if len(a) == 2 and a[0] == '--integration':
        sys.exit(integration(a[1]))
    if len(a) in (3, 4) and a[0] == '--accept':
        rec = _load(a[1])
        log = open(a[3], encoding='utf-8', errors='replace').read() if len(a) == 4 else None
        ok, seq, bad = accept(rec or b'', _load(a[1] + '.seal'), int(a[2], 16), log)
        print(('ACCEPT seq=%d' % seq) if ok else 'REJECT')
        for b in bad:
            print('  ' + b)
        sys.exit(0 if ok else 1)
    if len(a) != 1:
        print(__doc__)
        sys.exit(2)
    try:
        print(fmt(*read(open(a[0], 'rb').read())))
    except ValueError as e:
        print('error=%s' % e)
        sys.exit(1)
