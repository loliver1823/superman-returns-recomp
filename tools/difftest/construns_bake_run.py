#!/usr/bin/env python3
"""p1.row3_construns BAKE offline proof (native/construns-bake, 2026-10-14): difftest --shader-const-runs-test bake.

Run on the cloud worker (never on the desktop), from the synced worktree root:
    python tools/cloud/sr_cloud.py run --name crb -- python tools/difftest/construns_bake_run.py --out archive/crb/bake_run
then fetch the --out folder. CPU only: no game, harness, GPU or native_proof run. It rebuilds the difftest target once per
mutant (-j 4 by default), so one at a time.

What the bake route proves (tools/difftest/shader_const_runs_test.cpp runBake), in four processes:
  default  (SR_NATIVE_SHADER_CONST_RUNS unset)  the gate is ON by default; the caller domain; the counters
  cppm4    (unset, SR_NATIVE_FLUSH_CPPM4=1)     the same, and the CP renders the sync record back to PM4 (cpPm4=)
  off      (=0)                                 the gate is OFF; the SDK body everywhere (no sdk= / tessSdk=)
  ab_off   (unset, SR_AB_TOGGLE=SR_NATIVE_SHADER_CONST_RUNS)  the A/B's off window declines as off
THE CALLER DOMAIN: every caller of 821075E0 in this binary passes the device's six dirty bits as the mask's top bits (at
most six groups, never zero), and the SDK's entry check leaves cursor <= dev+48 = dev+44 - 160 (820FCF90, both
branches); at cursor == dev+48 all 63 masks x cursor%8 0 / 4 take the SDK's fast path (r3 stays 3: its chunk helper
82107058 never runs) and the native leaf's (chunked= unchanged). A control at margin 152 shows the probe sees the chunk
path (the six-group mask meets the SDK's strict fit test with equality). THE COUNTERS: sdk= per direct call, the leaf's
declines by reason (oracle, mask, span; off in the A/B window), tessDraws= / tessSdk= for a tessellated draw 820FC478,
kick= for an entry-check flush (compared against the SDK through the recording-pin fixture, both alignments), cpPm4=
under CPPM4 only, marginBad= when dev+44 - dev+48 != 160.

Then compiled faults in kernel/native_flush.cpp, each rebuilt and required to fail EXACTLY its named assertions in its
mode (every other assertion of that mode passing): the default flipped back OFF, the tessellated draw uncounted, the
CP's render-back uncounted, the oracle decline uncounted, the entry kick uncounted, the margin uncounted. The source is
restored byte for byte, rebuilt, and every mode rerun. Exit 0 = valid.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

SUMMARY = re.compile(r'^\[SHADER_CONST_RUNS_TEST\] (\d+) passed, (\d+) failed', re.M)
BAKE = re.compile(r'^\[SHADER_CONST_BAKE\] env=(\S+) gate=(\d) cppm4=(\d) ab=(\d)$', re.M)
DOMAIN = re.compile(r'^\[SHADER_CONST_BAKE\] domain cases=(\d+) sdkChunked=(\d+) nativeRan=(\d+) nativeChunked=(\d+) '
                    r'sameR3=(\d+) marginOk=(\d+)$', re.M)

GATE = "the gate is ON unless SR_NATIVE_SHADER_CONST_RUNS=0 (the bake's default)"
DOMAIN_SDK = ("caller domain: 63 six-bit masks x cursor%8 0/4 at cursor == dev+48 == dev+44 - 160, the SDK never "
              "chunks")
DOMAIN_NATIVE = ("caller domain: the native leaf takes all 126, never chunks, returns the SDK's r3, marginBad stays 0")
CONTROL_SDK = "caller domain control: at dev+44 - dev+48 = 152 the six-group mask chunks in the SDK"
CONTROL_NATIVE = "caller domain control: the native leaf chunks with the SDK, returns its r3 and counts marginBad"
DIRECT = "counters: a direct call is one sdk= (gate ON) or none (=0: the SDK body), never tessSdk="
CP_COUNT = "counters: the CP renders the sync record back to PM4 and counts it (cpPm4=)"
CP_NONE = "counters: record execution is no cpPm4="
DECL = "counters: the leaf's declines count by reason (oracle, mask, span) before any guest write"
DECL_OFF = "counters: the A/B's off window declines as off"
TESS = "counters: a tessellated draw is one tessDraws= and its SDK call one tessSdk= (gate ON)"
KICK0 = "counters: an entry-check flush is one kick= (cursor%8=0), no chunk, the new segment's margin 160"
KICK4 = "counters: an entry-check flush is one kick= (cursor%8=4), no chunk, the new segment's margin 160"
ENTRY_FLUSH = re.compile(r'^SDK entry-check flush mask=FC00000000000000 cursor%8=[04] firstGroups=6 flushes=[1-9]\d*: ')

COMMON = {GATE, DOMAIN_SDK, CONTROL_SDK, DIRECT, TESS}
GATE_ON = COMMON | {DOMAIN_NATIVE, CONTROL_NATIVE, DECL, KICK0, KICK4}
# mode -> (extra env, the [SHADER_CONST_BAKE] header expected, the assertions that must pass by name)
MODES = {
    'default': ({}, ('<unset>', '1', '0', '0'), GATE_ON | {CP_NONE}),
    'cppm4': ({'SR_NATIVE_FLUSH_CPPM4': '1'}, ('<unset>', '1', '1', '0'), GATE_ON | {CP_COUNT}),
    'off': ({'SR_NATIVE_SHADER_CONST_RUNS': '0'}, ('0', '0', '0', '0'), set(COMMON)),
    'ab_off': ({'SR_AB_TOGGLE': 'SR_NATIVE_SHADER_CONST_RUNS'}, ('<unset>', '1', '0', '1'), GATE_ON | {CP_NONE, DECL_OFF}),
}
ENTRY_FLUSHES = {'default': 2, 'cppm4': 2, 'off': 2, 'ab_off': 2}


def anchor(text):
    return text.encode()


# name, (production text, mutated text), the mode it runs in, the assertions that must fail (exactly those)
MUTATIONS = (
    ('default_off', ("    return !value || value[0] != '0';\n  }();\n  return on;  // default ON since the construns bake",
                     "    return value && value[0] == '1';  // MUTANT: default OFF\n  }();\n  return on;  // default ON "
                     "since the construns bake"), 'default', {GATE}),
    ('tess_uncounted', ("  g_tessDraws.fetch_add(1, std::memory_order_relaxed);\n  ++t_tessDepth;",
                        "  ++t_tessDepth;  // MUTANT: the tessellated draw uncounted"), 'default', {TESS}),
    ('cp_uncounted', ("          t_shaderSyncPm4++;  // the construns bake's cpPm4=",
                      "          // MUTANT: the CP's render-back uncounted"), 'cppm4', {CP_COUNT}),
    ('decl_oracle_uncounted', ("  if (g_shaderConstRunsOracleDepth) return decline(kScrOracle);",
                               "  if (g_shaderConstRunsOracleDepth) return false;  // MUTANT: uncounted"), 'default', {DECL}),
    ('kick_uncounted', ("    if (switches) g_scrKick.fetch_add(1, std::memory_order_relaxed);",
                        "    (void)switches;  // MUTANT: the entry kick uncounted"), 'default', {KICK0, KICK4}),
    ('margin_uncounted', ("    if (lim() - thr() != 160u) g_scrMarginBad.fetch_add(1, std::memory_order_relaxed);",
                          "    // MUTANT: the margin uncounted"), 'default', {CONTROL_NATIVE}),
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def parse(raw):
    text = raw.decode(errors='replace').replace('\r\n', '\n')
    lines = text.splitlines()
    good = [line[6:] for line in lines if line.startswith('  ok  ')]
    bad = [line[6:] for line in lines if line.startswith('FAIL: ')]
    return text, good, bad, SUMMARY.findall(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--no-mutants', action='store_true', help='the positive modes only (no rebuilds)')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = (root / args.out).resolve() if not args.out.is_absolute() else args.out
    out.mkdir(parents=True, exist_ok=False)
    source = root / 'kernel/native_flush.cpp'
    original = source.read_bytes()
    crlf = b'\r\n' in original

    def enc(text):
        b = anchor(text)
        return b.replace(b'\n', b'\r\n') if crlf else b

    for name, (before, _after), _mode, _req in MUTATIONS:  # every anchor exactly once before anything is built
        if original.count(enc(before)) != 1:
            raise SystemExit(f'{name}: anchor occurs {original.count(enc(before))} times in kernel/native_flush.cpp')
    tracked = ('kernel/native_flush.cpp', 'kernel/native_flush.hpp', 'kernel/native_brackets.gen.cpp',
               'tools/difftest/shader_const_runs_test.cpp', 'tools/difftest/construns_bake_run.py')
    result = dict(scope='Offline CPU proof of the construns bake: the default, the caller domain against the SDK, the '
                  'counters the live bake proof reads. No live acceptance.',
                  source_sha256={p: sha((root / p).read_bytes()) for p in tracked}, modes={}, mutations=[])

    def save():
        (out / 'construns_bake_matrix.json').write_text(json.dumps(result, indent=2) + '\n')

    base_env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    base_env['PATH'] = r'C:\Program Files\LLVM\bin;' + base_env.get('PATH', '')
    base_env.update(SR_NATIVE_FLUSH='1', SR_NATIVE_FLUSH_CHECK='0', SR_NATIVE_SEGMENTLESS='0', SR_NATIVE_SEG_STRESS='0',
                    SR_NATIVE_FLUSH_PM4='0', SR_NATIVE_FLUSH_CPPM4='0', SR_NATIVE_CONST_DEDUP='0',
                    SR_NATIVE_CONST_DEDUP_CHECK='0', SR_NATIVE_GATE='0', SR_NATIVE_CALLBACK='0', SR_ALLOW_FORCING='0',
                    SR_HDR_ADAPT_DT='0', SR_LOG='dev')

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
        for attempt in range(3):  # the worker's sccache server sometimes drops a connection: retried, recorded
            code, raw = run(tag if not attempt else f'{tag}_retry{attempt}',
                            ['cmake', '--build', 'build', '--target', 'difftest', '-j', str(args.jobs)], base_env, 1800)
            if code == 0 or b'sccache: error' not in raw:
                break
        exe = root / 'build/difftest.exe'
        row = dict(exit=code, attempts=attempt + 1, exe_sha256=sha(exe.read_bytes()) if exe.exists() else None)
        print(json.dumps({'build': tag, **row}), flush=True)
        if code != 0:
            raise RuntimeError(f'{tag} failed; see {tag}_stdout.txt')
        return row

    def test(tag, mode):
        env = dict(base_env)
        env.update(MODES[mode][0])
        code, raw = run(tag, [root / 'build/difftest.exe', '--shader-const-runs-test', 'bake'], env, 600)
        text, good, bad, summary = parse(raw)
        header = BAKE.findall(text)
        domain = DOMAIN.findall(text)
        return dict(mode=mode, exit=code, passed=len(good), failed=len(bad), summary=summary, header=header,
                    domain=domain, stdout_sha256=sha(raw), failures=bad), good, bad, summary, header

    def positive(tag, mode):
        row, good, bad, summary, header = test(tag, mode)
        need = MODES[mode][2]
        missing = sorted(need - set(good))
        entry = sum(1 for g in good if ENTRY_FLUSH.match(g))
        row['missing_required'] = missing
        row['entry_flushes'] = entry
        row['valid'] = bool(row['exit'] == 0 and not bad and good and summary == [(str(len(good)), '0')]
                            and len(set(good)) == len(good) and header == [MODES[mode][1]] and not missing
                            and entry == ENTRY_FLUSHES[mode])
        print(json.dumps({'test': tag, 'exit': row['exit'], 'summary': summary, 'missing': missing,
                          'valid': row['valid']}), flush=True)
        return row

    try:
        result['positive_build'] = build('positive_build')
        for mode in MODES:
            result['modes'][mode] = positive('positive_' + mode, mode)
            save()
        if not all(r['valid'] for r in result['modes'].values()):
            raise RuntimeError('positive matrix invalid; mutations not attempted')
        if not args.no_mutants:
            for name, (before, after), mode, required in MUTATIONS:
                b, a = enc(before), enc(after)
                if original.count(b) != 1:
                    raise RuntimeError(f'{name}: production anchor changed; refusing mutation')
                if source.read_bytes() != original:
                    raise RuntimeError('concurrent source edit; refusing mutation')
                changed = original.replace(b, a)
                mrow = dict(name=name, mode=mode, required=sorted(required), mutated_sha256=sha(changed))
                result['mutations'].append(mrow)
                source.write_bytes(changed)
                try:
                    mrow['build'] = build(name + '_build')
                    trow, good, bad, summary, _header = test(f'{name}_{mode}', mode)
                    trow['unexpected'] = sorted(set(bad) - required)
                    trow['missing_required'] = sorted(required - set(bad))
                    # exactly the named assertions fail; the rest of the mode passes (a gate-OFF mutant skips the
                    # gate-ON checks, so the totals are not compared)
                    trow['valid'] = (trow['exit'] == 1 and summary == [(str(len(good)), str(len(bad)))]
                                     and bool(good) and set(bad) == required)
                    mrow['test'] = trow
                    mrow['detected'] = bool(trow['valid'])
                    print(json.dumps({'mutation': name, 'failed': trow['failed'], 'missing': trow['missing_required'],
                                      'unexpected': trow['unexpected'], 'detected': mrow['detected']}), flush=True)
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
                result['restored'] = {m: positive('restored_' + m, m) for m in MODES}
        finally:
            mutants_ok = args.no_mutants or (
                len(result['mutations']) == len(MUTATIONS)
                and all(m.get('detected') and m.get('source_restored') for m in result['mutations'])
                and result.get('restored') and all(r['valid'] for r in result['restored'].values()))
            result['valid'] = bool(result['all_sources_restored'] and len(result['modes']) == len(MODES)
                                   and all(r['valid'] for r in result['modes'].values()) and mutants_ok)
            save()
            print(json.dumps({'valid': result['valid']}), flush=True)
    return 0 if result['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
