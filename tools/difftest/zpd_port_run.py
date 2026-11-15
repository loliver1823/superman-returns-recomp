#!/usr/bin/env python3
"""p2.zpd's bake, offline proof of the producer port: GpuEmitTileSurfaceConfig_82101E08 as an Entry
(kernel/native_flush.cpp Driver::zpdSurface; EVENT_WRITE_ZPD as the kRkZpd record) against the SDK body.
Suite: difftest --zpd-port-test <records|cppm4|pm4> (tools/difftest/zpd_port_test.inc).

Run on the cloud worker (never on the desktop), from the synced worktree root:
    python tools/cloud/sr_cloud.py run --worktree . --name zpdbake python tools/difftest/zpd_port_run.py --out archive/zbk/port
then fetch the --out folder. CPU only: no game, harness, GPU or native_proof run. One build at a time.

Matrix: the positive build in every mode - records (the record rendered back and applied by the record engine's own
consumer, RecordEngine::nativeRecord), cppm4 (SR_NATIVE_FLUSH_CPPM4=1: the CP renders the record to PM4 and executes
it), pm4 (SR_NATIVE_FLUSH_PM4=1: the producer-side literal span equals the SDK's bytes) - and, across the processes,
the CP's PM4 (cppm4) equal to the producer's (pm4) and to the SDK's for every case (a kick case's: every segment the
closes published, concatenated). The kick cases (Astra review round 1, P2-3) fire 82101E08's entry / per-tile kick
checks; their faults delete the per-tile kick, drop or offset the cursor an in-loop kick returned, delete the entry
kick. Then compiled faults in the port
(kernel/native_flush.cpp), each rebuilt and run in all three modes: every mode with required assertions must exit 1
with them among its failures and no SDK-arm assertion failing (the oracle is the guest body); every other mode must
pass whole. Then the source restored byte for byte, rebuilt, the positive matrix rerun.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

SUMMARY = re.compile(r'^\[ZPD_PORT_TEST\] (\d+) passed, (\d+) failed$', re.M)
BYTES = re.compile(r'^\[ZPD_PORT\] (\S+) (sdk|cp|producer)=([0-9A-F]+)$', re.M)
MODES = {
    'records': {},
    'cppm4': {'SR_NATIVE_FLUSH_CPPM4': '1'},
    'pm4': {'SR_NATIVE_FLUSH_PM4': '1'},
}
BASE = ('begin-1tile', 'end-1tile', 'end-0tiles-e0', 'begin-1tile-e0', 'begin-2tiles', 'end-3tiles-e0')  # no kick
ONE = ('begin-1tile', 'end-1tile', 'end-0tiles-e0', 'begin-1tile-e0')
MULTI = ('begin-2tiles', 'end-3tiles-e0')
BEGIN = ('begin-1tile', 'begin-1tile-e0', 'begin-2tiles')
E0 = ('end-0tiles-e0', 'begin-1tile-e0', 'end-3tiles-e0')
# the kick cases (Astra review round 1, P2-3; zpd_port_test.inc KICKS): the entry check kicks / a per-tile check kicks
KICK_ENTRY_ONLY = ('kick-entry-1tile', 'kick-entry-3tiles-e0')  # the entry kick only: r3 = its cursor + 16
KICK_MID = ('kick-mid-2tiles-t0', 'kick-mid-2tiles-t1', 'kick-mid-3tiles-t0', 'kick-mid-3tiles-t1-at-thr',
            'kick-every-3tiles-minseg')  # a per-tile kick: r3 = the cursor the last one returned
KICK_ENTRY = KICK_ENTRY_ONLY + ('kick-every-3tiles-minseg',)
KICKS = KICK_ENTRY_ONLY + KICK_MID
KICK_MULTI = tuple(c for c in KICKS if c != 'kick-entry-1tile')
CASES = BASE + KICKS

# the suite's assertion names (zpd_port_test.inc runCase), per case
R3 = "r3 and dev+0x28 equal the SDK's"
DIRTY = "the dirty masks dev+0..+39 equal the SDK's (and the bin masks dev+12432 / +12436 unchanged)"
PRODUCER = "the producer's literal span (SR_NATIVE_FLUSH_PM4) equals the SDK's bytes"
KZPD = 'the record ends with one kRkZpd {21} (EVENT_WRITE_ZPD as a packet record, not literal PM4)'
RENDER = "the record rendered back (cpRecordPm4) equals the SDK's bytes"
APPLY = {'records': "the CP applies the record (RecordEngine::nativeRecord): the registers, the bin masks and one "
                    "EVENT_WRITE_ZPD equal the SDK's PM4 executed",
         'cppm4': "the CP applies the record (the CPPM4 split): the registers, the bin masks and one EVENT_WRITE_ZPD "
                  "equal the SDK's PM4 executed"}
# the kick cases' (zpd_port_test.inc runKickCase)
PUBS = ("the published segments equal the SDK's (count, phys, dwords, each one's PM4: the guest bytes and the records "
        "rendered back by cpRecordPm4)")
TAIL = ("the post-kick tail in the new segment (the multi-tile LO {dev+12432} / HI {dev+12436}, EVENT_WRITE_ZPD {21}) "
        "before the close's fence: one kRkZpd {21} record packet (records modes) or literal PM4 (SR_NATIVE_FLUSH_PM4), "
        "none before the kick")
SDK_ARM = re.compile(r': SDK arm: ')


def req(cases, records=(), cppm4=(), pm4=()):
    """{mode: the assertions that must fail} over `cases`; a mode left empty must pass whole."""
    out = {}
    for mode, names in (('records', records), ('cppm4', cppm4), ('pm4', pm4)):
        if names:
            out[mode] = {f'{c}: {APPLY[mode] if n == "APPLY" else n}' for c in cases for n in names}
    return out


def every(cases, *names):
    """The same assertions required in all three modes."""
    return req(cases, records=names, cppm4=names, pm4=names)


def merge(*reqs):
    out = {}
    for r in reqs:
        for mode, names in r.items():
            out.setdefault(mode, set()).update(names)
    return out


# Driver::zpdSurface's per-tile kick check (the asm: dev+40 = cursor; cursor > dev+48 -> r3 = GpuCommandBufferFlush)
TILE_KICK = '          if (!kick()) return false;\n          r3 = c;\n'
ENTRY_KICK = '  bool zpdSurface(uint32_t q, uint32_t which, uint32_t& r3) {\n    if (!kickPoint()) return false;\n'


NF = 'kernel/native_flush.cpp'
# name, ((file, before, after), ...), {mode: required failures}
MUTATIONS = (
    # the one-tile EVENT_WRITE_ZPD's initiator (21 = ZPASS_DONE's report) wrong
    ('initiator_one_tile', ((NF, '        packet(o[n], kRkZpd, 0xC0005B00u, {21u});\n',
                             '        packet(o[n], kRkZpd, 0xC0005B00u, {20u});\n'),),
     merge(req(ONE, records=(KZPD, RENDER, 'APPLY'), cppm4=(KZPD, RENDER, 'APPLY'), pm4=(PRODUCER,)),
           every(('kick-entry-1tile',), PUBS, TAIL))),
    # ... the multi-tile one
    ('initiator_tiles', ((NF, '      packet(o[2], kRkZpd, 0xC0005B00u, {21u});\n',
                          '      packet(o[2], kRkZpd, 0xC0005B00u, {20u});\n'),),
     merge(req(MULTI, records=(KZPD, RENDER, 'APPLY'), cppm4=(KZPD, RENDER, 'APPLY'), pm4=(PRODUCER,)),
           every(KICK_MULTI, PUBS, TAIL))),
    # dev+24's dirty bits (rldicr 1,38 / 1,37) dropped
    ('dirty24_dropped', ((NF, '    put64(d + 24, be64(d + 24) | (1ull << 38) | (1ull << 37));\n', ''),),
     req(CASES, records=(DIRTY,), cppm4=(DIRTY,), pm4=(DIRTY,))),
    # ... the low word (0x60) instead: the misreading of extldi the port first had
    ('dirty24_low_word', ((NF, '    put64(d + 24, be64(d + 24) | (1ull << 38) | (1ull << 37));\n',
                           '    put64(d + 24, be64(d + 24) | 0x60ull);\n'),),
     req(CASES, records=(DIRTY,), cppm4=(DIRTY,), pm4=(DIRTY,))),
    # the BEGIN record (+0x20) not selected: which ignored
    ('slot_ignores_which', ((NF, '      const uint32_t v = sr::kernel::readGuestU32BE(q + 24u + 4u * i) + 32u * which;\n',
                             '      const uint32_t v = sr::kernel::readGuestU32BE(q + 24u + 4u * i) + 0u * which;\n'),),
     req(BEGIN, records=(RENDER, 'APPLY'), cppm4=(RENDER, 'APPLY'), pm4=(PRODUCER,))),
    # the XDK's VA -> physical fold (+0x1000 for the 0xE0 view) dropped
    ('slot_no_fold', ((NF, '      return (((v >> 20) + 0x200u) & 0x1000u) + (v & 0x1FFFFFFFu);\n',
                       '      return v & 0x1FFFFFFFu;\n'),),
     req(E0, records=(RENDER, 'APPLY'), cppm4=(RENDER, 'APPLY'), pm4=(PRODUCER,))),
    # the multi-tile tail's bin masks swapped (LO {dev+12436}, HI {dev+12432})
    ('tail_masks_swapped', ((NF, '      binReg(o[0], 0u, be32(d + 12432));\n      binReg(o[1], 1u, be32(d + 12436));\n',
                             '      binReg(o[0], 0u, be32(d + 12436));\n      binReg(o[1], 1u, be32(d + 12432));\n'),),
     merge(req(MULTI, records=(RENDER, 'APPLY'), cppm4=(RENDER, 'APPLY'), pm4=(PRODUCER,)),
           every(KICK_MULTI, PUBS, TAIL))),  # ... and the post-kick tail in the new segment
    # r3 (the asm's cursor after the entry check + 16) wrong (a later in-loop kick's cursor replaces it: not KICK_MID)
    ('r3_wrong', ((NF, '    r3 = c + 16u;\n    {\n', '    r3 = c + 20u;\n    {\n'),),
     every(BASE + KICK_ENTRY_ONLY, R3)),
    # the CP's render-back has no kRkZpd case: the record's EVENT_WRITE_ZPD silently vanishes under CPPM4
    ('cp_render_no_case', ((NF, '        case kRkZpd:  // p2.zpd\'s bake: EVENT_WRITE_ZPD {initiator}\n'
                                '          put(0xC0005B00u | pred);\n          put(p[0]);\n          break;\n', ''),),
     req(BASE, records=(RENDER,), cppm4=(RENDER, 'APPLY'))),
    # the release consumer (gfx/record_context.cpp) has no kRkZpd case: the record is rejected
    ('record_context_no_case', (('gfx/record_context.cpp',
                                 '      case nf::kRkZpd:  // p2.zpd\'s bake: EVENT_WRITE_ZPD {initiator} - the host occlusion '
                                 'query (applyZpdEvent)\n        ++engine_.opcodeHist[0x5B];\n'
                                 '        engine_.applyZpdEvent(words[0], true);\n        break;\n', ''),),
     req(BASE, records=('APPLY',))),
    # ---- the kicks (Astra review round 1, P2-3) ----
    # the per-tile kick deleted: the port writes on past dev+48 in the old segment (r3 = its cursor, no segment switch)
    ('per_tile_kick_deleted', ((NF, TILE_KICK, '          r3 = c;\n'),),
     every(KICK_MID, R3, PUBS)),
    # the cursor an in-loop kick returned not taken: r3 stays the cursor after the entry check + 16
    ('kick_cursor_broken', ((NF, TILE_KICK, '          if (!kick()) return false;\n'),),
     every(KICK_MID, R3)),
    # ... taken off by 4
    ('kick_cursor_off4', ((NF, TILE_KICK, '          if (!kick()) return false;\n          r3 = c + 4u;\n'),),
     every(KICK_MID, R3)),
    # the entry kick check deleted: the head written past dev+48 in the old segment (the next per-tile check kicks)
    ('entry_kick_deleted', ((NF, ENTRY_KICK, '  bool zpdSurface(uint32_t q, uint32_t which, uint32_t& r3) {\n'),),
     every(KICK_ENTRY, R3, PUBS)),
)


def parse(raw):
    text = raw.decode(errors='replace').replace('\r\n', '\n')
    lines = text.splitlines()
    good = [line[6:] for line in lines if line.startswith('  ok  ')]
    bad = [line[6:] for line in lines if line.startswith('FAIL: ')]
    data = {}
    for case, kind, value in BYTES.findall(text):
        data.setdefault(case, {})[kind] = value
    return good, bad, SUMMARY.findall(text), data


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--only', default='', help='comma-separated mutation names (default: all)')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = (root / args.out).resolve() if not args.out.is_absolute() else args.out
    out.mkdir(parents=True, exist_ok=False)
    only = {s for s in args.only.split(',') if s}
    mutations = [m for m in MUTATIONS if not only or m[0] in only]
    files = sorted({f for _, repls, _ in mutations for f, _, _ in repls})
    originals = {f: (root / f).read_bytes() for f in files}

    def fit(f, text):  # the working tree's line endings
        b = text.encode()
        return b.replace(b'\n', b'\r\n') if b'\r\n' in originals[f] else b

    for name, repls, required in mutations:
        for f, before, _after in repls:
            n = originals[f].count(fit(f, before))
            if n != 1:
                raise SystemExit(f'{name}: anchor occurs {n} times in {f}')
        if not required:
            raise SystemExit(f'{name}: no mode with required failures')
    tracked = sorted(set(files) | {'kernel/native_flush.cpp', 'kernel/native_flush.hpp', 'kernel/native_record_format.hpp',
                                   'gfx/record_context.cpp', 'gfx/record_engine.cpp', 'gfx/record_engine.hpp',
                                   'gfx/pm4_engine.cpp', 'gfx/pm4_engine.hpp', 'gfx/native_zpd.cpp', 'gfx/native_zpd.hpp',
                                   'tools/difftest/zpd_port_test.inc', 'tools/difftest/setconst_render_test.inc',
                                   'tools/difftest/zpd_port_run.py', 'tools/difftest/difftest_main.cpp',
                                   'tools/difftest/suite_bare.inc'})
    result = dict(scope='Offline CPU proof of p2.zpd\'s bake, the producer port: 82101E08 through production dispatch, the '
                  'SDK body against the native Entry (SR_NATIVE_ZPD unset: the baked default), the kRkZpd record rendered '
                  'back and applied by the CP (the record engine\'s own consumer; the CPPM4 split), the producer-side '
                  'literal split; and the kicks inside 82101E08 (the entry check, the per-tile check after tile 0 / '
                  'the last tile / tile 1 at the threshold, every check at once on minimum segments): the published '
                  'segments, r3, dev+0x28, the dirty masks, the post-kick tail and the CP\'s effects of the segments, '
                  'the per-tile / entry kick and the in-loop kick\'s cursor mutated. No live acceptance.',
                  source_sha256={p: sha((root / p).read_bytes()) for p in tracked}, modes={}, cross={}, mutations=[])

    def save():
        (out / 'zpd_port_matrix.json').write_text(json.dumps(result, indent=2) + '\n')

    base_env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    base_env['PATH'] = r'C:\Program Files\LLVM\bin;' + base_env.get('PATH', '')
    base_env.update(SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0',
                    SR_AB_TOGGLE='SR_NATIVE_ZPD', SR_AB_PRESENT='1', SR_NATIVE_SEGMENTLESS='1', SR_NATIVE_FLUSH='1')

    def run(tag, command, env, timeout):
        try:
            p = subprocess.run([str(c) for c in command], cwd=root, env=env, capture_output=True, timeout=timeout)
            code, stdout, stderr = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired as error:
            code, stdout, stderr = 124, error.stdout or b'', error.stderr or b''
        (out / (tag + '_stdout.txt')).write_bytes(stdout)
        (out / (tag + '_stderr.txt')).write_bytes(stderr)
        return code, stdout

    def build(tag):
        # The shared worker's sccache server sometimes drops a connection (os error 10054): retried, recorded.
        for attempt in range(4):
            code, raw = run(tag if not attempt else f'{tag}_retry{attempt}',
                            ['cmake', '--build', 'build', '--target', 'difftest', '-j', str(args.jobs)], base_env, 2400)
            if code == 0 or (b'sccache' not in raw and b'forcibly closed' not in raw):
                break
        exe = root / 'build/difftest.exe'
        row = dict(exit=code, attempts=attempt + 1, exe_sha256=sha(exe.read_bytes()) if exe.exists() else None)
        print(json.dumps({'build': tag, **row}), flush=True)
        if code != 0:
            raise RuntimeError(f'{tag} failed; see {tag}_stdout.txt')
        return row

    def test(tag, mode):
        env = dict(base_env)
        env.update(MODES[mode])
        code, raw = run(tag, [root / 'build/difftest.exe', '--zpd-port-test', mode], env, 600)
        good, bad, summary, data = parse(raw)
        return dict(mode=mode, exit=code, passed=len(good), failed=len(bad), summary=summary, stdout_sha256=sha(raw),
                    failures=bad), good, bad, summary, data

    def positive(tag, mode, datas=None):
        row, good, bad, summary, data = test(tag, mode)
        row['valid'] = bool(row['exit'] == 0 and not bad and good and summary == [(str(len(good)), '0')]
                            and len(set(good)) == len(good) and set(data) == set(CASES))
        if datas is not None:
            datas[mode] = data
        print(json.dumps({'test': tag, 'exit': row['exit'], 'summary': summary, 'valid': row['valid']}), flush=True)
        return row

    def cross(datas):
        """The CP's PM4 (cppm4) equals the producer's literal split (pm4) and the SDK's bytes for every case."""
        rows = {}
        for case in CASES:
            cp = datas.get('cppm4', {}).get(case, {})
            pr = datas.get('pm4', {}).get(case, {})
            rows[case] = dict(cp=cp.get('cp'), producer=pr.get('producer'),
                              equal=bool(cp.get('cp')) and cp.get('cp') == pr.get('producer') == cp.get('sdk'),
                              same_sdk=bool(cp.get('sdk')) and cp.get('sdk') == pr.get('sdk'))
        return dict(cases=rows, valid=all(r['equal'] and r['same_sdk'] for r in rows.values()))

    def restore_all():
        for f, data in originals.items():
            if (root / f).read_bytes() != data:
                (root / f).write_bytes(data)

    totals = {}
    try:
        result['positive_build'] = build('positive_build')
        datas = {}
        for mode in MODES:
            result['modes'][mode] = positive('positive_' + mode, mode, datas)
            totals[mode] = result['modes'][mode]['passed']
            save()
        result['cross'] = cross(datas)
        print(json.dumps({'cross': result['cross']['valid']}), flush=True)
        save()
        if not (all(r['valid'] for r in result['modes'].values()) and result['cross']['valid']):
            raise RuntimeError('positive matrix invalid; mutations not attempted')
        for name, repls, required in mutations:
            for f, data in originals.items():
                if (root / f).read_bytes() != data:
                    raise RuntimeError('concurrent source edit; refusing mutation')
            mrow = dict(name=name, required={m: sorted(r) for m, r in required.items()}, tests={}, controls={})
            result['mutations'].append(mrow)
            try:
                for f, before, after in repls:
                    (root / f).write_bytes((root / f).read_bytes().replace(fit(f, before), fit(f, after)))
                mrow['mutated_sha256'] = {f: sha((root / f).read_bytes()) for f, _, _ in repls}
                mrow['build'] = build(name + '_build')
                for mode in MODES:
                    if mode in required:
                        trow, good, bad, summary, _data = test(f'{name}_{mode}', mode)
                        missing = sorted(required[mode] - set(bad))
                        trow['missing_required'] = missing
                        trow['valid'] = (trow['exit'] == 1 and summary == [(str(len(good)), str(len(bad)))]
                                         and len(good) + len(bad) == totals[mode] and not missing
                                         and not [b for b in bad if SDK_ARM.search(b)])
                        mrow['tests'][mode] = trow
                    else:
                        mrow['controls'][mode] = positive(f'{name}_{mode}', mode)
                mrow['detected'] = bool(all(t['valid'] for t in mrow['tests'].values()) and mrow['tests']
                                        and all(c['valid'] for c in mrow['controls'].values()))
                print(json.dumps({'mutation': name, 'detected': mrow['detected'],
                                  'missing': {m: t['missing_required'] for m, t in mrow['tests'].items()}}), flush=True)
            finally:
                restore_all()
                mrow['source_restored'] = all((root / f).read_bytes() == data for f, data in originals.items())
                save()
    finally:
        result['valid'] = False
        restore_all()
        result['all_sources_restored'] = all(sha((root / p).read_bytes()) == v for p, v in result['source_sha256'].items())
        try:
            if result['all_sources_restored'] and result['mutations']:
                result['restored_build'] = build('restored_build')
                rdatas = {}
                result['restored'] = {m: positive('restored_' + m, m, rdatas) for m in MODES}
                result['restored_cross'] = cross(rdatas)
        finally:
            result['valid'] = bool(
                result['all_sources_restored'] and result['modes'] and all(r['valid'] for r in result['modes'].values())
                and result['cross'].get('valid') and len(result['mutations']) == len(mutations)
                and all(m.get('detected') and m.get('source_restored') for m in result['mutations'])
                and result.get('restored') and all(r['valid'] for r in result['restored'].values())
                and result.get('restored_cross', {}).get('valid'))
            save()
            print(json.dumps({'valid': result['valid']}), flush=True)
    return 0 if result['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
