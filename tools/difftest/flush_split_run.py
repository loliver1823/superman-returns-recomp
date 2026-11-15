#!/usr/bin/env python3
"""p1.flush_residue offline proof (native/flush-residue): SR_NATIVE_FLUSH_SPLIT against the actual SDK bodies.

Run on the cloud worker (never on the desktop), from the synced worktree root:
    python tools/cloud/sr_cloud.py run --worktree . --name frs python tools/difftest/flush_split_run.py --out archive/frs/split_matrix
then fetch the --out folder. CPU only: no game, harness, GPU or native_proof run. One build at a time.

Matrix: the positive build in every mode of --flush-split-test (tools/difftest/flush_split_test.inc: records = the
default path with the constant dedup skipping, records-raw = the canonical stream exact, cppm4 = the CP renders every
record back to PM4, pm4 = literal spans, the command buffer byte for byte, gate-off = SR_NATIVE_FLUSH_SPLIT=0, both
native arms today's path, zpass-leaf = records-raw with SR_DIAG_ZPASS_LEAF=1, C1 stage 1's L3b lever: the pinned
fixtures' sections at the segment limit to the guest leaf, every other one converted); then compiled faults in the gated
code - each rebuilt, required to fail its named assertions
in the listed modes without crashing and without any SDK-arm assertion failing, and to pass the gate-off control (the
fault sits behind the gate); then every source restored byte for byte, rebuilt and the positive modes rerun.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

SUMMARY = re.compile(r'^\[FLUSH_SPLIT_TEST\] (\d+) passed, (\d+) failed$', re.M)
MODES = ('records', 'records-raw', 'cppm4', 'pm4', 'gate-off', 'zpass-leaf')

NF = 'kernel/native_flush.cpp'
RB = 'kernel/native_rec_b.cpp'
RS = 'kernel/nb_rec_resolve.inc'


def A(case, text):
    return f'{case}: ON arm: {text}'


PUBS = 'the published segments (phys, dwords) equal the SDK\'s, in order'
CANON = 'the canonical stream (register writes, draws, packets) equals the SDK\'s exactly'
STATES = 'every draw (initiator, DMA, predicate) sees the SDK\'s register state'
BYTES = 'the command buffer equals the SDK\'s byte for byte (literal spans)'
SPLIT = 'the draw over 0xFFFF indices split natively'
TAKEN = 'the row-9 entry taken as records through the kick (kickT 1, no live stop)'
PRE_DECL = 'the row-9 entry declined by its pre-pass (guest body)'
WAITED = 'the row-9 entry taken as records through a modelled GPU wait (waitT 1, no live stop)'
NO_GAP = 'guest PM4 packets in its segments 0 (all records)'
DMAWRAP = 'the parts\' DMA bases 20000FE0 0001FFD8'
ZP_UNPINNED = 'SR_DIAG_ZPASS_LEAF did not fire (no Z-pass recording)'
ZP_PINNED = 'SR_DIAG_ZPASS_LEAF sent the sections at the segment limit to the guest leaf (inside the recording)'
ZP_INERT = 'SR_DIAG_ZPASS_LEAF unset: the lever never fired over every fixture (inert by default)'

# name, ((file, before, after), ...), {mode: assertions that must fail}
MUTATIONS = (
    # 82107058's chunk size one dword short: the SDK's segment boundary moves
    ('chunk_size_short', ((NF, b'const int32_t avail = (int32_t(lim() - c) >> 2) - 1;',
                           b'const int32_t avail = (int32_t(lim() - c) >> 2) - 2;'),),
     {'records-raw': {A('reg-4900', PUBS), A('reg-one-register-chunk', PUBS), A('reg-4900-ring', PUBS)},
      'pm4': {A('reg-4900', BYTES), A('reg-one-register-chunk', BYTES)}}),
    # the registers after the first chunk dropped
    ('chunk_remainder_dropped', ((NF, b'        count -= k;\n        reg += k;', b'        count = 0;\n        reg += k;'),),
     {'records-raw': {A('reg-4900', CANON), A('const-partial', CANON), A('reg-4900', STATES)}}),
    # the fetch section's slow trailer written in the fast form
    ('slow_trailer_fast_form', ((NF, b'out[0].lit[0] = out[0].lit[1] = out[0].lit[2] = 0u;',
                                 b'out[0].lit[0] = out[0].lit[2] = 0u;\n      out[0].lit[1] = 0x00025000u;'),),
     {'records-raw': {A('fetch-last-chunked', CANON), A('fetch-last-chunked-ring', CANON)},
      'pm4': {A('fetch-last-chunked', BYTES)}}),
    # the split's overlap left out of the next part's count (strips: the parts no longer join)
    ('split_overlap_ignored', ((NF, b'count = overlap + (count - chunk);', b'count = count - chunk;'),),
     {'records-raw': {A('big-idx16-tristrip', STATES), A('big-auto-linestrip', STATES)}}),
    # the part size not rounded to an even number of primitives (& ~1)
    ('split_chunk_unrounded', ((NF, b'? ((0xFFFFu / ipp) & ~1u) * ipp : count;', b'? (0xFFFFu / ipp) * ipp : count;'),),
     {'records-raw': {A('big-idx16-trilist', STATES), A('big-idx32-trilist', STATES), A('inst-8261B050-split', STATES)},
      'pm4': {A('big-idx16-trilist', BYTES)}}),
    # the draw over 0xFFFF indices never admitted (today's decline under the gate)
    ('big_admit_off', ((NF, b'if (!(split && splittable)) {', b'if (true || !(split && splittable)) {'),),
     {'records-raw': {A('big-idx16-trilist', SPLIT), A('big-auto-quadlist', SPLIT), A('big-idx16-trilist', NO_GAP),
                      A('inst-8261B050-split', SPLIT), A('inst-8261B050-split', NO_GAP),
                      A('inst-8261B050-regions', SPLIT)}}),
    # the index DMA address saturated at 2^32 instead of wrapping: only a part whose address carries past 2^32 differs
    # (Astra round 2: the wrap BETWEEN the parts, asserted explicitly)
    ('dma_no_wrap', ((NF, b'      uint32_t addr = b * 2u + ib6;',
                      b'      uint32_t addr = uint64_t(b) * 2u + ib6 > 0xFFFFFFFFull ? 0xFFFFFFFFu : b * 2u + ib6;'),),
     {'records-raw': {A('big-idx16-negbase-dmawrap', STATES), A('big-idx16-negbase-dmawrap', DMAWRAP),
                      A('big-idx16-negbase-dmawrap-ring', DMAWRAP)},
      'pm4': {A('big-idx16-negbase-dmawrap', BYTES)}}),
    # the pre-pass past a transparent kick treats 820FCDF0 as failing: it stops evaluating the rest of the entry, so the
    # tiled restore reached only after the allocations is never seen and the live pass meets it
    ('prepass_alloc_fails', ((RB, b'    return va;\n  }\n  if (r8(10809) & 0x40u) return 0;',
                              b'    return 0u * va;\n  }\n  if (r8(10809) & 0x40u) return 0;'),),
     {'records-raw': {A('gpr-tiled-restore-after-kick', PRE_DECL)}}),
    # the resolve's even-S guard removed: the pre-pass follows S before the kick, the live pass S after it
    ('prepass_parity_guard_off', ((RS, b'  if (p.kicked && (s & 1u) == 0u) {', b'  if (false && p.kicked && (s & 1u) == 0u) {'),),
     {'records-raw': {A('resolve-even-S-list-path', PRE_DECL)}}),
    # the pre-pass does not chunk a register run crossing the limit (the entry declines as today)
    ('prepass_no_chunk', ((RB, b'    p.split = p.transparent = split;', b'    p.split = false;\n    p.transparent = split;'),),
     {'records-raw': {A('gpr-run-crosses-limit', TAKEN), A('gpr-run-crosses-limit', NO_GAP)}}),
    # the pre-pass not transparent at a kick (today's decline under the gate)
    ('prepass_opaque_kick', ((RB, b'    p.split = p.transparent = split;', b'    p.split = split;\n    p.transparent = false;'),),
     {'records-raw': {A('gpr-kick', TAKEN), A('resolve-kick', TAKEN), A('clearF-kick', TAKEN),
                      A('resolve-kick-at-248-ring', TAKEN)}}),
    # no wait model: the record pre-pass stops at 820FCB30's wait as the default path does (native/flush-residue-2)
    ('prepass_no_wait_model', ((RB, b'        if (!transparent || r32(13068)) {',
                                b'        if (true || !transparent || r32(13068)) {'),),
     {'records-raw': {A('gpr-gpu-wait-ring', WAITED), A('resolve-gpu-wait-ring', WAITED),
                      A('clearF-gpu-wait-ring', WAITED)}}),
    # C1 stage 1, L3b's lever (SR_DIAG_ZPASS_LEAF) without its pin test: it fires outside a Z-pass recording too
    ('zpass_leaf_pin_ignored', ((NF, b'if (!g_zpassLeaf || !split || be32(d + 0x3290u) == 0u) return false;',
                                 b'if (!g_zpassLeaf || !split) return false;'),),
     {'zpass-leaf': {A('const-many-kicks', ZP_UNPINNED), A('reg-4900', ZP_UNPINNED), A('fetch-last-chunked', ZP_UNPINNED),
                     A('const-second-run-u0', ZP_UNPINNED), A('const-many-kicks-ring', ZP_UNPINNED)}}),
    # the lever never consulted (its sections chunk natively inside the recording too)
    ('zpass_leaf_never', ((NF, b'if (!split || zpassLeaf(0)) {', b'if (!split) {'),
                          (NF, b'if (!split || zpassLeaf(1)) return guestLeaf(1, mask, reg, srcOff);',
                           b'if (!split) return guestLeaf(1, mask, reg, srcOff);'),
                          (NF, b'if (!split || n >= 32 || zpassLeaf(2)) return guestLeaf(2, mask, 0, 0);',
                           b'if (!split || n >= 32) return guestLeaf(2, mask, 0, 0);')),
     {'zpass-leaf': {A('const-many-kicks-pinned', ZP_PINNED), A('reg-4900-pinned', ZP_PINNED),
                     A('fetch-last-chunked-pinned', ZP_PINNED)}}),
    # the lever on by default: the default path (records) must see it fire
    ('zpass_leaf_default_on', ((NF, b"return e && e[0] == '1' && e[1] == '\\0';", b'return true;'),),
     {'records': {ZP_INERT}}),
)
SDK_ARM = re.compile(r': SDK arm: ')


def parse(raw):
    lines = raw.decode(errors='replace').replace('\r\n', '\n').splitlines()
    good = [line[6:] for line in lines if line.startswith('  ok  ')]
    bad = [line[6:] for line in lines if line.startswith('FAIL: ')]
    return good, bad, SUMMARY.findall('\n'.join(lines))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--only', default='',
                        help='comma-separated mutation names: build and test only these (every positive mode still runs; '
                             'the result records the selection)')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = (root / args.out).resolve() if not args.out.is_absolute() else args.out
    selected = [s for s in args.only.split(',') if s]
    unknown = sorted(set(selected) - {name for name, _, _ in MUTATIONS})
    if unknown:
        raise SystemExit(f'--only: unknown mutation(s) {", ".join(unknown)}')
    mutations = tuple(m for m in MUTATIONS if not selected or m[0] in selected)
    out.mkdir(parents=True, exist_ok=False)
    files = sorted({f for _, repls, _ in mutations for f, _, _ in repls})
    originals = {f: (root / f).read_bytes() for f in files}

    def fit(f, text):  # the working tree's line endings
        return text.replace(b'\n', b'\r\n') if b'\r\n' in originals[f] else text

    # Every fault's anchor must occur exactly once before anything is built.
    for name, repls, _ in mutations:
        for f, before, _after in repls:
            n = originals[f].count(fit(f, before))
            if n != 1:
                raise SystemExit(f'{name}: anchor occurs {n} times in {f}')
    tracked = sorted(set(files) | {'kernel/native_flush.hpp', 'kernel/native_rec_b.hpp', 'tools/difftest/difftest_main.cpp',
                                   'tools/difftest/flush_split_test.inc', 'tools/difftest/flush_split_run.py',
                                   'tools/perf/flush_split_snapshot.py'})
    result = dict(scope='Offline CPU proof of SR_NATIVE_FLUSH_SPLIT (native/flush-residue): the actual SDK 820FC000 / '
                  '820FBBF8 (with 821078C0 / 82107520 / 82107760 / 82107058 / 820FD8C0 under them), 820F5570, 8210C5F8 '
                  'and 82101A58 against the native paths, and the game\'s generated RwDrawInstancedConstBatched 8261B050 '
                  'over them. No live acceptance.',
                  source_sha256={p: sha((root / p).read_bytes()) for p in tracked}, modes={}, mutations=[],
                  selection=selected or 'all')

    def save():
        (out / 'split_matrix.json').write_text(json.dumps(result, indent=2) + '\n')

    # The suite's bare guard re-runs itself with exactly the mode's configuration (tools/difftest/suite_bare.inc).
    base_env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    base_env['PATH'] = r'C:\Program Files\LLVM\bin;' + base_env.get('PATH', '')

    def run(tag, command, timeout):
        try:
            p = subprocess.run([str(c) for c in command], cwd=root, env=base_env, capture_output=True, timeout=timeout)
            code, stdout, stderr = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired as error:
            code, stdout, stderr = 124, error.stdout or b'', error.stderr or b''
        (out / (tag + '_stdout.txt')).write_bytes(stdout)
        (out / (tag + '_stderr.txt')).write_bytes(stderr)
        return code, stdout

    def build(tag):
        # The shared worker's sccache server sometimes drops a connection: retried twice, recorded.
        for attempt in range(3):
            code, raw = run(tag if not attempt else f'{tag}_retry{attempt}',
                            ['cmake', '--build', 'build', '--target', 'difftest', '-j', str(args.jobs)], 1800)
            if code == 0 or b'sccache: error' not in raw:
                break
        exe = root / 'build/difftest.exe'
        row = dict(exit=code, attempts=attempt + 1, exe_sha256=sha(exe.read_bytes()) if exe.exists() else None)
        print(json.dumps({'build': tag, **row}), flush=True)
        if code != 0:
            raise RuntimeError(f'{tag} failed; see {tag}_stdout.txt')
        return row

    def test(tag, mode):
        code, raw = run(tag, [root / 'build/difftest.exe', '--flush-split-test', mode], 900)
        good, bad, summary = parse(raw)
        return dict(mode=mode, exit=code, passed=len(good), failed=len(bad), summary=summary, stdout_sha256=sha(raw),
                    failures=bad), good, bad, summary

    def positive(tag, mode):
        row, good, bad, summary = test(tag, mode)
        row['valid'] = bool(row['exit'] == 0 and not bad and good and summary == [(str(len(good)), '0')]
                            and len(set(good)) == len(good))
        print(json.dumps({'test': tag, 'exit': row['exit'], 'summary': summary, 'valid': row['valid']}), flush=True)
        return row

    def snapshot_contract(tag):
        """The suite prints the live proof's [NATIVE_FLUSH] split snapshot line; tools/perf/flush_split_snapshot.py must
        parse every field the plan grades from it (the format contract between the game and the grader)."""
        js = out / (tag + '_snapshot.json')
        p = subprocess.run([sys.executable, str(root / 'tools/perf/flush_split_snapshot.py'), str(out / (tag + '_stdout.txt')),
                            '--json', str(js)], cwd=root, capture_output=True, text=True)
        try:
            d = json.loads(js.read_text())
        except (OSError, ValueError):
            d = {}
        fin = d.get('last') or {}
        need = ['draws', 'count_ffff', 'ipp0', 'nosplit', 'convertible_declined', 'fallbacks', 'aborts', 'after_planning',
                'big_draws', 'big_parts', 'sec_const', 'sec_reg', 'sec_fetch']
        missing = [k for k in need if not isinstance(fin.get(k), int)]
        for e in ('resolve', 'clearF', 'gpr'):
            x = fin.get(e) or {}
            missing += ['%s.%s' % (e, k) for k in ('calls', 'taken', 'open', 'pre', 'live', 'kickT', 'kickTStop', 'waitT')
                        if not isinstance(x.get(k), int)]
        row = dict(exit=p.returncode, snapshots=d.get('snapshots'), missing=missing,
                   valid=bool(p.returncode == 0 and d.get('snapshots') == 1 and not missing))
        print(json.dumps({'snapshot_contract': tag, **row}), flush=True)
        return row

    def restore_all():
        for f, data in originals.items():
            if (root / f).read_bytes() != data:
                (root / f).write_bytes(data)

    totals = {}
    try:
        result['positive_build'] = build('positive_build')
        for mode in MODES:
            result['modes'][mode] = positive('positive_' + mode, mode)
            totals[mode] = result['modes'][mode]['passed']
            save()
        result['snapshot_contract'] = snapshot_contract('positive_records-raw')
        save()
        if not all(r['valid'] for r in result['modes'].values()) or not result['snapshot_contract']['valid']:
            raise RuntimeError('positive matrix invalid; mutations not attempted')
        for name, repls, required in mutations:
            changed = dict(originals)
            for f, before, after in repls:
                b, a = fit(f, before), fit(f, after)
                if changed[f].count(b) != 1:
                    raise RuntimeError(f'{name}: production anchor changed; refusing mutation')
                changed[f] = changed[f].replace(b, a)
            if any((root / f).read_bytes() != originals[f] for f in files):
                raise RuntimeError('concurrent source edit; refusing mutation')
            mrow = dict(name=name, required={m: sorted(v) for m, v in required.items()},
                        mutated_sha256={f: sha(changed[f]) for f, _, _ in repls}, tests={})
            result['mutations'].append(mrow)
            for f, _, _ in repls:
                (root / f).write_bytes(changed[f])
            try:
                mrow['build'] = build(name + '_build')
                ok = True
                for mode, req in required.items():
                    trow, good, bad, summary = test(f'{name}_{mode}', mode)
                    missing = sorted(req - set(bad))
                    trow['missing_required'] = missing
                    trow['sdk_arm_failures'] = [b for b in bad if SDK_ARM.search(b)]
                    trow['valid'] = (trow['exit'] == 1 and summary == [(str(len(good)), str(len(bad)))]
                                     and len(good) + len(bad) == totals[mode] and not missing
                                     and not trow['sdk_arm_failures'])
                    mrow['tests'][mode] = trow
                    ok = ok and trow['valid']
                mrow['control'] = positive(f'{name}_gate_off', 'gate-off')
                mrow['detected'] = bool(ok and mrow['control']['valid'])
                print(json.dumps({'mutation': name, 'failed': {m: t['failed'] for m, t in mrow['tests'].items()},
                                  'missing': {m: t['missing_required'] for m, t in mrow['tests'].items()},
                                  'detected': mrow['detected']}), flush=True)
            finally:
                restore_all()
                mrow['source_restored'] = all((root / f).read_bytes() == originals[f] for f in files)
                save()
    finally:
        result['valid'] = False
        restore_all()
        result['all_sources_restored'] = all(sha((root / p).read_bytes()) == v for p, v in result['source_sha256'].items())
        try:
            if result['all_sources_restored'] and result['mutations']:
                result['restored_build'] = build('restored_build')
                result['restored'] = {m: positive('restored_' + m, m) for m in MODES}
        finally:
            result['valid'] = bool(
                result['all_sources_restored'] and result['modes'] and all(r['valid'] for r in result['modes'].values())
                and (result.get('snapshot_contract') or {}).get('valid')
                and len(result['mutations']) == len(mutations)
                and all(m.get('detected') and m.get('source_restored') for m in result['mutations'])
                and result.get('restored') and all(r['valid'] for r in result['restored'].values()))
            save()
            print(json.dumps({'valid': result['valid']}), flush=True)
    return 0 if result['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
