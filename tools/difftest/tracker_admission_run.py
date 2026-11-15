#!/usr/bin/env python3
"""The tcmp1 diagnosis's fixes, offline (transport-fix lane, 2026-10-14): tracker-owned pages admitted by the SWAP and
BRACKET admissions, the per-reason / per-site decline counters, the BRACKET_END binding trace record - with compiled
faults.

Run on the cloud worker (never on the desktop), from the synced worktree root, after building the difftest target:
    python tools/cloud/sr_cloud.py run --worktree . --name tcmp -- "python tools/difftest/tracker_admission_run.py --out archive/tfix/tracker"
then fetch the --out folder. CPU only: no game, harness, GPU or native_proof run. One build at a time (-j 4).

Positive: `--swap-sdk-test tracker` (the cursor's reserved block on pages the GPU write tracker owns read-only: the leaf
admits it, its result equals the scoped SDK oracle's, its stores release the protection as the SDK body's do; a block
mixing an owned page with an unowned read-only one declines; the post-swap fallbacks counted once and sampled),
`--swap-sdk-test direct` / SWAP off / bare (the admission fixtures counted by their reason, a failed read apart from a
refused state), `--native-bracket-test native` (tracked-lists: the recording lists' prefix / cursor pages tracker-owned
before the end: published natively, no memory decline, released by the handler in both arms) and with SR_P2_TRACE=1
(the binding record names exactly the bound closes; the plan records are the end's replay steps in order; the CP traced
the end's begin and completion). Then each compiled fault (kernel/native_flush.cpp only; Astra round 1 added
fallback_sample, root_read, lifecycle_end, plan_kind) rebuilt; it must fail exactly its named assertions (others only in
the named case), show its sample line where named, and leave its control modes whole; the source restored byte for byte,
rebuilt and the positives rerun. No live, pacing or release acceptance.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
from pathlib import Path

SUMMARY = re.compile(r'^\[([A-Z_]+_TEST)\] (\d+) passed, (\d+) failed', re.M)

SWAP = dict(SR_NATIVE_SWAP='1', SR_NATIVE_SWAP_BLOCK='1', SR_NATIVE_FLUSH='1', SR_NATIVE_SEGMENTLESS='1',
            SR_NATIVE_FLUSH_PM4='0', SR_NATIVE_FLUSH_CPPM4='0', SR_NATIVE_FLUSH_CHECK='0', SR_NATIVE_CONST_DEDUP='0',
            SR_NATIVE_GATE='0', SR_NATIVE_CALLBACK='0', SR_NATIVE_SHADER_CONST_RUNS='0', SR_NATIVE_DEVCREATE='0',
            SR_NATIVE_EXEC='0', SR_PM4_CP='1', SR_PM4_LIVE='0', SR_ALLOW_FORCING='0', SR_MODS='0', SR_DEVTOOLS='0',
            SR_HIGHFPS='0', SR_API_OBSERVE='0', SR_API_PRESENT_BLOCK='0', SR_LOG='dev',
            SR_MEM_RESOURCES='0')  # swap_sdk_test_run.py; the page model: see BRACKET's note
BRACKET = dict(SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0',
               SR_AB_TOGGLE='SR_NATIVE_BRACKET', SR_AB_PRESENT='1', SR_NATIVE_SEGMENTLESS='1', SR_NATIVE_FLUSH='1',
               SR_NATIVE_BRACKET='1', SR_NATIVE_TRANSPORT='0',
               SR_MEM_RESOURCES='0')                                                 # native_bracket_run.py
# SR_MEM_RESOURCES=0 in both: the mem.resources bake (native/mem-native a1fda568, 2026-10-17) made the native resource route the dev default, where the GPU write tracker is off: these cases test the page model's tracker, so they run at SR_MEM_RESOURCES=0 (the oracle arm); the native default's rows are composed_suites_run.py's *_native and native_route_admission_run.py

# tag -> (difftest arguments, env on top of a no-SR_ environment)
TESTS = {
    'swap_tracker': (['--swap-sdk-test', 'tracker'], SWAP),
    'swap_direct': (['--swap-sdk-test', 'direct'], SWAP),
    'swap_sdk_off': (['--swap-sdk-test', 'direct'], dict(SWAP, SR_NATIVE_SWAP='0', SR_NATIVE_SWAP_BLOCK='0')),
    'swap_bare': (['--swap-sdk-test'], {}),
    'bracket_native': (['--native-bracket-test', 'native'], BRACKET),
    'bracket_p2trace': (['--native-bracket-test', 'native'], dict(BRACKET, SR_P2_TRACE='1')),
}

TRACKER_ANCHOR = (b'  return sr::native::memory::writableRange(begin, bytes) || '
                  b'sr::kernel::gww::cpuWritableRange(begin, bytes);')
TRACKER_FAULT = b'  return sr::native::memory::writableRange(begin, bytes);'
ADMIT = 'the native SDK body admits the tracker-owned block'
SWAP_ADMITS = {f'tracker-owned block fmt={f} all={a} offset={o} pages={p}: {ADMIT}'
               for f, a in ((6, 0), (50, 1)) for o, p in ((0, 1), (4, 1), (3840, 2))}
TRACKED = 'tracked-lists: '
REASON = 'unsupported device/global mode declines before SDK effects, counted by its reason'
SITE = TRACKED + 'a memory decline is counted once, at a list\'s prefix / cursor site'
BINDING = 'the trace\'s binding record names exactly the bound closes (first id, count, generation)'
LIFE = 'the CP traced the BRACKET_END\'s begin and completion (id, steps, generation)'
PLAN = "the trace's plan records are the BRACKET_END's replay steps in order (step, value, generation)"
RINGCUT = 'ring-cut: '
RINGCUT_COUNT = RINGCUT + 'the coherency block, the END and a later close dropped at the cut, each counted once (quitCut)'
RINGCUT_CLEAN = RINGCUT + 'no END published, no mismatch, no literal route'
FALLBACK_COUNT = 'the post-swap fallbacks keep the call taken, each counted once (total and reason)'
FALLBACK_SAMPLE = 'each post-swap fallback writes its SWAP_DECL sample'

# A check: (test tag, assertions that must fail, the prefixes / suffixes every failure must carry, assertions that must
# pass, stderr patterns that must appear).
MUTATIONS = (
    # the defect this lane fixes: gateWritable without the tracker's read-only pages (tcmp1's 25 gameplay SWAP declines
    # and the bracket memory declines). The samples must name it: why=cursorWrite / a list site, writable=0 tracker=1.
    # (bracket_p2trace runs the same cases traced: the same failures)
    ('tracker_admission', [(TRACKER_ANCHOR, TRACKER_FAULT)],
     [('swap_tracker', SWAP_ADMITS, ('tracker-owned block',), set(),
       [rb'\[NATIVE_FLUSH\] SWAP_DECL #\d+ why=cursorWrite .*writable=0 tracker=1'])] +
     [(tag, {TRACKED + 'BRACKET_END published', TRACKED + 'no memory decline on tracker-owned list pages'},
       (TRACKED,), {SITE}, [rb'\[NATIVE_FLUSH\] BRACKET_MEMORY #\d+ site=(prefix|cursor)\d list=32[0-9A-F]{2} .*'
                            rb'writable=0 tracker=1']) for tag in ('bracket_native', 'bracket_p2trace')],
     ['swap_direct', 'swap_sdk_off', 'swap_bare']),
    # a decline counted under the wrong reason: the admission fixtures name the reason they set
    ('swap_reason', [(b'if (be32(snapshot + 0x4DF4u) || be32(snapshot + 0x4E84u)) return decline(kSwPix);',
                      b'if (be32(snapshot + 0x4DF4u) || be32(snapshot + 0x4E84u)) return decline(kSwTimestamps);')],
     [('swap_direct', {REASON}, (REASON,), set(), [])],
     ['swap_sdk_off', 'swap_bare', 'bracket_native']),
    # the list sites counted elsewhere (with the tracker fault, so that the end declines at a list): the site check
    ('bracket_site', [(TRACKER_ANCHOR, TRACKER_FAULT),
                      (b'if (!gateWritable(prefix, 4u)) return memory(kBmPrefix0 + i,',
                       b'if (!gateWritable(prefix, 4u)) return memory(kBmEntry + 0 * i,'),
                      (b'if (!gateWritable(cursor, 4u)) return memory(kBmCursor0 + i,',
                       b'if (!gateWritable(cursor, 4u)) return memory(kBmEntry + 0 * i,')],
     [('bracket_native', {SITE}, (TRACKED,), set(), [])],
     ['swap_direct', 'swap_bare']),
    # the binding record split into one-id runs: the p2trace mode's binding check
    ('binding_runs', [(b'    while (j < ids.size() && ids[j] == ids[j - 1u] + 1u) j++;\n'
                       b'    const uint32_t first = uint32_t(ids[i]), count = uint32_t(j - i)',
                       b'    while (false) j++;\n'
                       b'    const uint32_t first = uint32_t(ids[i]), count = uint32_t(j - i)')],
     [('bracket_p2trace', {'plain: ' + BINDING}, (BINDING,), set(), [])],
     ['bracket_native', 'swap_tracker']),
    # Astra round 1, P3: the post-swap fallbacks counted without their sample (the code before the fix)
    ('fallback_sample', [(b'      swapDecline(kSwFallbackFront, dev, stack, be32(d + 40u));',
                          b'      g_swapSdkDecl.fetch_add(1, std::memory_order_relaxed); '
                          b'g_swapWhy[kSwFallbackFront].fetch_add(1, std::memory_order_relaxed);'),
                         (b'      swapDecline(kSwFallbackScaler, dev, stack, be32(d + 40u));',
                          b'      g_swapSdkDecl.fetch_add(1, std::memory_order_relaxed); '
                          b'g_swapWhy[kSwFallbackScaler].fetch_add(1, std::memory_order_relaxed);')],
     [('swap_tracker', {FALLBACK_SAMPLE}, (FALLBACK_SAMPLE,), {FALLBACK_COUNT}, [])],
     ['swap_direct', 'bracket_native']),
    # Astra round 1, P2: a failed root read counted as the root's state (the combined reason before the split)
    ('root_read', [(b'!gateRead(be32(root), cell, 4u)) return decline(kSwRootRead);',
                    b'!gateRead(be32(root), cell, 4u)) return decline(kSwRoot);')],
     [(tag, {REASON}, (REASON,), set(), []) for tag in ('swap_direct', 'swap_tracker')],  # (tracker mode runs them too)
     ['swap_sdk_off', 'bracket_native']),
    # Astra round 1, P2: the BRACKET_END's completion record not traced (a finished bracket would read as in flight)
    ('lifecycle_end', [(b'  if (sp->bracketGen) bracketLifecycle(7u, *sp, bracketSteps);',
                        b'  (void)bracketSteps;')],
     [('bracket_p2trace', {'plain: ' + LIFE}, (LIFE,), set(), [])],
     ['bracket_native', 'swap_tracker']),
    # tfx2 (the graceful quit's ring cut mid-bracket; the bracket suite's ring-cut case): a close the cut dropped counted
    # as a mismatch, and the coherency block / the END the cut dropped back to bracketFatal - the crash tfx2 showed
    ('cut_close', [(b'    } else if (ringCut()) {  // the graceful quit\'s ring cut dropped this publish',
                    b'    } else if (false) {  // the graceful quit\'s ring cut dropped this publish')],
     [('bracket_native', {RINGCUT_COUNT, RINGCUT_CLEAN}, (RINGCUT,), set(), [])],
     ['swap_direct', 'swap_tracker']),
    ('cut_coherency', [(b'      if (route == 2 && !ringCut()) bracketFatal("coherency block published without its hand-off");',
                        b'      if (route == 2) bracketFatal("coherency block published without its hand-off");')],
     [('bracket_native', 'CRASH', (), set(),
       [rb'\[NATIVE_FLUSH_BAD\] bracket: coherency block published without its hand-off'])],
     ['swap_direct']),
    ('cut_end', [(b'      if (route == 2 && !ringCut()) bracketFatal("BRACKET_END published without its hand-off");',
                  b'      if (route == 2) bracketFatal("BRACKET_END published without its hand-off");')],
     [('bracket_native', 'CRASH', (), set(), [rb'\[NATIVE_FLUSH_BAD\] bracket: BRACKET_END published without its hand-off'])],
     ['swap_direct']),
    # Astra round 1, P2: the plan's step kinds swapped (a bin select traced as an application and back)
    ('plan_kind', [(b'    const uint16_t aux = apply ? 9u : 8u;', b'    const uint16_t aux = apply ? 8u : 9u;')],
     [('bracket_p2trace', {'plain: ' + PLAN}, (PLAN,), set(), [])],
     ['bracket_native', 'swap_tracker']),
)


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
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--only', default='', help='comma-separated mutation names (default: all)')
    parser.add_argument('--no-mutations', action='store_true', help='the positive tests only')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = (root / args.out).resolve() if not args.out.is_absolute() else args.out
    out.mkdir(parents=True, exist_ok=False)
    source = root / 'kernel/native_flush.cpp'
    original = source.read_bytes()
    crlf = b'\r\n' in original
    tracked = ('kernel/native_flush.cpp', 'kernel/native_flush.hpp', 'kernel/gpu_write_watch.cpp',
               'kernel/gpu_write_watch.hpp', 'kernel/p2_trace.hpp', 'tools/difftest/swap_sdk_test.cpp',
               'tools/difftest/native_bracket_test.inc', 'tools/difftest/tracker_admission_run.py')
    result = dict(scope='Offline CPU proof of the tcmp1 diagnosis\'s fixes: tracker-owned pages admitted by the SWAP '
                  'and BRACKET admissions (the real GPU write tracker, both physical copies), the decline counters by '
                  'reason / site and their samples, the BRACKET_END binding trace record. No live, pacing or release '
                  'acceptance.',
                  source_sha256={p: sha((root / p).read_bytes()) for p in tracked}, tests={}, mutations=[])
    names = {x for x in args.only.split(',') if x}
    selected = [] if args.no_mutations else [m for m in MUTATIONS if not names or m[0] in names]

    def save():
        (out / 'tracker_matrix.json').write_text(json.dumps(result, indent=2) + '\n')

    clean = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    clean['PATH'] = r'C:\Program Files\LLVM\bin;' + clean.get('PATH', '')

    def run(tag, command, env, timeout):
        try:
            p = subprocess.run([str(c) for c in command], cwd=root, env=env, capture_output=True, timeout=timeout)
            code, stdout, stderr = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired as error:
            code, stdout, stderr = 124, error.stdout or b'', error.stderr or b''
        (out / (tag + '_stdout.txt')).write_bytes(stdout)
        (out / (tag + '_stderr.txt')).write_bytes(stderr)
        return code, stdout, stderr

    def build(tag):
        for attempt in range(3):  # the shared worker's sccache drops a connection now and then (os error 10054)
            code, raw, _ = run(tag if not attempt else f'{tag}_retry{attempt}',
                               ['cmake', '--build', 'build', '--target', 'difftest', '-j', str(args.jobs)], clean, 1800)
            if code == 0 or b'sccache: error' not in raw:
                break
        exe = root / 'build/difftest.exe'
        row = dict(exit=code, attempts=attempt + 1, exe_sha256=sha(exe.read_bytes()) if exe.exists() else None)
        print(json.dumps({'build': tag, **row}), flush=True)
        if code != 0:
            raise RuntimeError(f'{tag} failed; see {tag}_stdout.txt')
        return row

    def test(tag, name):
        argv, extra = TESTS[name]
        env = dict(clean)
        env.update(extra)
        code, raw, err = run(tag, [root / 'build/difftest.exe'] + argv, env, 900)
        good, bad, summary = parse(raw)
        return dict(test=name, exit=code, passed=len(good), failed=len(bad), summary=summary, stdout_sha256=sha(raw),
                    failures=bad[:60]), good, bad, summary, err

    def positive(tag, name):
        row, good, bad, summary, _ = test(tag, name)
        row['valid'] = bool(row['exit'] == 0 and not bad and good and summary and
                            all(int(f) == 0 for _, _, f in summary) and sum(int(p) for _, p, _ in summary) == len(good))
        print(json.dumps({'test': tag, 'exit': row['exit'], 'summary': summary, 'valid': row['valid']}), flush=True)
        return row

    try:
        result['positive_build'] = build('positive_build')
        for name in TESTS:
            result['tests'][name] = positive('positive_' + name, name)
            save()
        if not all(r['valid'] for r in result['tests'].values()):
            raise RuntimeError('positive tests invalid; mutations not attempted')
        for name, edits, checks, controls in selected:
            changed = original
            for before, after in edits:
                if crlf:
                    before, after = before.replace(b'\n', b'\r\n'), after.replace(b'\n', b'\r\n')
                if changed.count(before) != 1:
                    raise RuntimeError(f'{name}: production anchor changed; refusing mutation')
                changed = changed.replace(before, after)
            if source.read_bytes() != original:
                raise RuntimeError('concurrent source edit; refusing mutation')
            mrow = dict(name=name, mutated_sha256=sha(changed), checks=[], controls={})
            result['mutations'].append(mrow)
            source.write_bytes(changed)
            try:
                mrow['build'] = build(name + '_build')
                detected = True
                for tname, must_fail, allowed, must_pass, patterns in checks:
                    trow, good, bad, summary, err = test(f'{name}_{tname}', tname)
                    if must_fail == 'CRASH':  # the fault aborts (bracketFatal's std::abort, 0xC0000409), as tfx2's quit did
                        unseen = [p.decode() for p in patterns if not re.search(p, err)]
                        trow.update(crash=True, unseen=unseen)
                        trow['valid'] = bool(trow['exit'] not in (0, 1) and not summary and not unseen)
                        detected &= trow['valid']
                        mrow['checks'].append(trow)
                        print(json.dumps({'mutation': name, 'test': tname, 'exit': trow['exit'], 'unseen': unseen,
                                          'valid': trow['valid']}), flush=True)
                        continue
                    missing = sorted(a for a in must_fail if a not in bad)
                    stray = sorted({b for b in bad if not any(b.startswith(p) or b.endswith(p) for p in allowed)})
                    unpassed = sorted(a for a in must_pass if a not in good)
                    unseen = [p.decode() for p in patterns if not re.search(p, err)]
                    trow.update(missing_required=missing, stray_failures=stray[:20], unpassed=unpassed, unseen=unseen)
                    trow['valid'] = bool(trow['exit'] == 1 and bad and not missing and not stray and not unpassed and
                                         not unseen and len(good) + len(bad) == result['tests'][tname]['passed'])
                    detected &= trow['valid']
                    mrow['checks'].append(trow)
                    print(json.dumps({'mutation': name, 'test': tname, 'failed': len(bad), 'missing': missing,
                                      'stray': stray[:4], 'unpassed': unpassed, 'unseen': unseen,
                                      'valid': trow['valid']}), flush=True)
                for cname in controls:
                    crow = positive(f'{name}_control_{cname}', cname)
                    mrow['controls'][cname] = crow
                    detected &= crow['valid']
                mrow['detected'] = bool(detected)
            finally:
                source.write_bytes(original)
                mrow['source_restored'] = source.read_bytes() == original
                save()
    finally:
        result['valid'] = False
        if source.read_bytes() != original:
            source.write_bytes(original)
        result['all_sources_restored'] = all(sha((root / p).read_bytes()) == v for p, v in result['source_sha256'].items())
        try:
            if result['all_sources_restored'] and result['mutations']:
                result['restored_build'] = build('restored_build')
                result['restored'] = {t: positive('restored_' + t, t) for t in TESTS}
        finally:
            result['valid'] = bool(
                result['all_sources_restored'] and result['tests'] and all(r['valid'] for r in result['tests'].values())
                and len(result['mutations']) == len(selected)
                and all(m.get('detected') and m.get('source_restored') for m in result['mutations'])
                and (not selected or (result.get('restored') and all(r['valid'] for r in result['restored'].values()))))
            save()
            print(json.dumps({'valid': result['valid']}), flush=True)
    return 0 if result['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
