#!/usr/bin/env python3
"""Native submission/park differential and executed production-defect controls.

Run on the cloud worker (never on the desktop: no local rebuilds), from the synced worktree root:
    python tools/cloud/sr_cloud.py run --worktree . --name <tree> python tools/difftest/native_transport_test_run.py --out archive/<fresh-bank>
then fetch the --out folder. No game/GPU worker. Builds are serial (cmake --build build --target difftest -j 4,
retried on the shared worker's sccache connection drops) and require 8 GiB free.
Modes: native and gate-off (CHECKS), the per-present A/B (ab: SR_AB_TOGGLE=SR_NATIVE_TRANSPORT, SR_AB_PRESENT=1;
AB_CHECKS), the tracker query-only permissions mode (18), and (2026-10-14, the arithmetic admission) the foreign
page-protection modes: exclusion (EXCLUSION_CHECKS: declared ranges, the real image watchdog's boot page, its runtime
arm and the audio null-page protection) and exclusion-env (EXCLUSION_ENV_CHECKS, once per page-protecting diagnostic
in DIAGNOSTIC_ENVS). The guest page-protection source census (guest_protect_census.py) must hold. Every mutant must
fail only its named contract checks and pass a gate-off SDK control (a tracker mutant: on rebuilt restored source).
Original source bytes are backed up and restored exactly. Timeout cleanup must confirm the process tree stopped
before restoration/rebuild.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

from native_memory_test_run import stop_windows_tree
from guest_protect_census import run as census_run


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


CHECKS = 165
AB_CHECKS = 47
PERMISSION_CHECKS = 18
EXCLUSION_CHECKS = 21
EXCLUSION_ENV_CHECKS = 6
RETIRE_CHECKS = 11
AB_ENV = {'SR_AB_TOGGLE': 'SR_NATIVE_TRANSPORT', 'SR_AB_PRESENT': '1'}
# kernel/guest_protect.hpp's page-protecting diagnostics, each requested as its owner reads it.
DIAGNOSTIC_ENVS = (('SR_PROTECT_ZERO', '1'), ('SR_IMAGE_RO', '1'), ('SR_XMV_WATCH', '1'), ('SR_XMV_EXTRA_WATCH', '1'),
                   ('SR_ALLOC_WATCH', '1'), ('SR_WRITE_WATCH_VA', '8209A458'))
MODE_CHECKS = {'permissions': PERMISSION_CHECKS, 'ab': AB_CHECKS, 'exclusion': EXCLUSION_CHECKS,
               'exclusion-env': EXCLUSION_ENV_CHECKS, 'retire': RETIRE_CHECKS}
# The arena's release against native calls in flight (retire mode; Astra round A, P1).
R_HOLDS = b'retire: an in-flight native append holds its lease while it waits on the chain lock'
R_NOT_DRAINED = b'retire: retiring the arena does not report it drained while a native call is in flight'
R_DECLINE = b'retire: once the arena is retired every new native call declines before any guest access, counted'
R_SKIPPED = b'retire: the arena release is skipped while a native call is in flight (the arena stays mapped)'
R_COMPLETES = b'retire: the in-flight append completes its stores into the mapped arena and returns its lease'
R_FREES = b'retire: with no call in flight the release frees the arena'

# The arithmetic admission's ranges and the tracker path (native mode).
ALIAS = (b'append node crossing the 0xC0000000 alias fold declines before imports and writes',
         b'append list crossing the 0xC0000000 alias fold declines before imports and writes',
         b'append device word crossing the alias fold declines before imports and writes',
         b'append stack window crossing the alias fold declines before imports and writes',
         b'EndSync state block crossing the alias fold declines before imports and writes')
ARENA = b'ranges above, below or without the registered guest arena decline before imports and writes'
ARENA_TAIL = b'chain tail outside the registered arena declines under the lock with lock and IRQL restored'
T_OWN = b'tracked append fixture owns actual read-only physical pages'
T_REL = b'SDK and native append stores release tracked outputs and invalidate validated contents'
T_DISPATCH = b'tracked output production dispatch matches complete SDK state and imports'
T_ADMITTED = b'tracked output admitted call matches complete SDK state and imports'
T_PATH = b"native append stores into tracked pages take the guest store's path: the SDK's faults, releases and invalidations"
E_OWN = b'tracked EndSync fixture owns actual read-only physical pages'
E_PARK = b'SDK and native EndSync park stores release the tracked state and invalidate it'
E_FOLLOW = b'SDK and native EndSync follow reads tracked pages without releasing them'
E_MATCH = b'tracked EndSync production dispatch and admitted call match complete SDK state and imports'
E_PATH = b"native EndSync stores into tracked pages take the guest store's path: the SDK's faults, releases and invalidations"
STORE_RELEASE = b'ordinary CPU store releases real tracking and publishes dirty-block invalidation'
# Foreign page protection (exclusion and exclusion-env modes).
D_NODE = b'declared range: an append whose node lies in it declines before imports and writes'
D_TAIL = b'declared range: a chain tail in it declines under the lock, publishing nothing, lock and IRQL restored'
D_MARKER = b'declared range: an EndSync marker in it declines before imports and reads'
D_STATE = b'declared range: an EndSync state block in it declines before imports and writes'
D_DISPATCH = b'declared range: production dispatch falls back to the actual SDK body (equal state and imports)'
D_READONLY = b'declared range: a read-only declared page is never written (entries decline, bytes and protection kept)'
D_BOOT = b'image watchdog: an append whose node lies on the boot page declines, the page read-only and unchanged'
X_ROUTES = b'exclusion: direct and chained append, parking and following EndSync decline before imports and writes, counted'
X_PERMANENT = b'exclusion: permanent - restoring the protected page does not readmit the pair'
X_ENV = b'exclusion env: from the first call every route declines before imports and writes, counted'

AB_APPEND_OFF = b'A/B OFF present: append ran the actual SDK body whole (native counters unchanged, the OFF call counted)'
AB_END_OFF = b'A/B OFF present: EndSync ran the actual SDK body whole (native counters unchanged, the OFF call counted)'
AB_APPEND_ENTRY = b'A/B OFF present: the native append entry falls back before any import or write'
AB_END_ENTRY = b'A/B OFF present: the native EndSync entry falls back before any import or write'
AB_APPEND_SDK = b'A/B OFF present: append dispatch equals the actual SDK'
AB_END_SDK = b'A/B OFF present: EndSync dispatch equals the actual SDK'
AB_MIXED_STATE = b'A/B mixed presents: device, software list, ring and import sequence equal the all-SDK reference'
AB_MIXED_RETURNS = b"A/B mixed presents: every call's full-width return equals the all-SDK reference"
AB_MIXED_ARMS = b"A/B mixed presents: each call ran in its present's arm (native and OFF counts)"


def grade(code, raw, defect=None, checks=CHECKS):
    lines = raw.replace(b'\r\n', b'\n').splitlines()
    good = sum(line.startswith(b'  ok  ') for line in lines)
    bad = [line[6:] for line in lines if line.startswith(b'FAIL: ')]
    summary = re.findall(rb'\[NATIVE_TRANSPORT_TEST\] (\d+) passed, (\d+) failed', raw)
    if summary != [(str(good).encode(), str(len(bad)).encode())] or good + len(bad) != checks:
        return False
    if defect is None:
        return code == 0 and not bad
    allowed = {
        'chain_recheck': {
            b'consumer parks before acquired recheck: producer takes direct route',
            b'controlled real two-thread consumer-first ordering publishes directly after park'},
        'terminal_recheck': {
            b'producer links before acquired recheck: consumer follows new link',
            b'controlled real two-thread producer-first ordering follows initialized link'},
        'tail_link': {
            b'actual SDK and native dispatch: complete append state and import sequence',
            b'new initialized node linked under the shared actual lock',
            b'controlled real two-thread producer-first ordering follows initialized link',
            T_DISPATCH, T_ADMITTED, T_REL, T_PATH},
        'return_width': {
            b'actual SDK and native dispatch: complete append state and import sequence',
            b'full-width direct/chained return preserved'},
        'pause_completion': {
            b'actual SDK and native dispatch: complete EndSync state and import sequence'},
        'alignment': {
            b'null or unaligned append inputs decline before imports and shared-state writes',
            b'null or unaligned EndSync inputs decline before imports and shared-state writes',
            b'unaligned actual chain tail declines with lock and IRQL restored'},
        'thread_owner': {b'missing actual guest lock owner declines before imports and mutation'},
        # The arithmetic admission (2026-10-14): contiguity across the alias fold, the registered arena's bounds.
        'contiguity': set(ALIAS),
        'arena_bound': {ARENA, ARENA_TAIL},
        # The tracker path: a native store that bypasses the handler (no fault, no release, no invalidation) differs
        # from the SDK's guest store; a handler that unprotects without releasing fails every arm's release witness.
        'pair_bypass': {T_REL, T_PATH, E_PARK, E_PATH},
        'handler_release': {STORE_RELEASE, T_OWN, T_REL, T_PATH, E_OWN, E_PARK, E_FOLLOW, E_PATH},
        # Foreign page protection: a declared range admitted (the read-only declared page and the watchdog's boot page
        # are then not entered: they fail by name, not by fault), the exclusion ignored, the environment ignored.
        'declared_range': {D_NODE, D_TAIL, D_MARKER, D_STATE, D_DISPATCH, D_READONLY, D_BOOT},
        'exclusion': {X_ROUTES, X_PERMANENT},
        'exclusion_env': {X_ENV},
        # The arena's release: retirement that does not wait, a pair that takes no lease, a release that ignores the
        # retirement. Each frees the arena under the paused append, which then never resumes (it fails by name).
        'arena_drain': {R_NOT_DRAINED, R_SKIPPED, R_COMPLETES, R_FREES},
        'arena_lease': {R_HOLDS, R_NOT_DRAINED, R_DECLINE, R_SKIPPED, R_COMPLETES, R_FREES},
        'release_retire': {R_SKIPPED, R_COMPLETES, R_FREES},
        'tracked_ownership': {b'mixed tracked and unowned read-only range declines without releasing either page'},
        # The per-present A/B. A window ignored by one entry still equals the SDK (both bodies agree); only the
        # census counters and the entry's own fallback show the wrong arm ran.
        'ab_append_window': {AB_APPEND_OFF, AB_APPEND_ENTRY, AB_MIXED_ARMS},
        'ab_endsync_window': {AB_END_OFF, AB_END_ENTRY, AB_MIXED_ARMS},
        # An OFF window that skips the call instead of running the SDK body: the differential sees it.
        'ab_off_skips': {AB_APPEND_SDK, AB_END_SDK, AB_APPEND_ENTRY, AB_END_ENTRY,
                         AB_MIXED_STATE, AB_MIXED_RETURNS, AB_MIXED_ARMS},
    }[defect]
    required = {
        'chain_recheck': b'consumer parks before acquired recheck: producer takes direct route',
        'terminal_recheck': b'producer links before acquired recheck: consumer follows new link',
        'tail_link': b'new initialized node linked under the shared actual lock',
        'return_width': b'full-width direct/chained return preserved',
        'pause_completion': b'actual SDK and native dispatch: complete EndSync state and import sequence',
        'alignment': b'null or unaligned append inputs decline before imports and shared-state writes',
        'thread_owner': b'missing actual guest lock owner declines before imports and mutation',
        'contiguity': ALIAS[0],
        'arena_bound': ARENA,
        'pair_bypass': T_PATH,
        'handler_release': T_REL,
        'declared_range': D_NODE,
        'exclusion': X_ROUTES,
        'exclusion_env': X_ENV,
        'arena_drain': R_NOT_DRAINED,
        'arena_lease': R_HOLDS,
        'release_retire': R_SKIPPED,
        'tracked_ownership': b'mixed tracked and unowned read-only range declines without releasing either page',
        'ab_append_window': AB_APPEND_OFF,
        'ab_endsync_window': AB_END_OFF,
        'ab_off_skips': AB_APPEND_SDK,
    }[defect]
    return code == 1 and required in bad and set(bad) <= allowed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = root/'kernel/native_transport.cpp'
    original = source.read_bytes()
    (out/'native_transport.original.cpp').write_bytes(original)
    watcher = root/'kernel/gpu_write_watch.cpp'
    watcher_original = watcher.read_bytes()
    (out/'gpu_write_watch.original.cpp').write_bytes(watcher_original)
    sources = ('kernel/native_transport.cpp','kernel/native_transport.hpp','kernel/native_memory.hpp',
               'kernel/gpu_write_watch.cpp','kernel/gpu_write_watch.hpp','kernel/ab_toggle.cpp','kernel/ab_toggle.hpp',
               'kernel/gpu_command_processor.cpp', 'kernel/gpu_submit.cpp','kernel/guest_protect.hpp','kernel/image_write_watchdog.cpp',
               'kernel/audio_cmdq_obs.cpp','kernel/audio_cmdq_obs.hpp','tools/difftest/native_transport_test.cpp',
               'tools/difftest/native_transport_test_run.py','tools/difftest/native_memory_test_run.py',
               'tools/difftest/guest_protect_census.py','tools/difftest/difftest_main.cpp','CMakeLists.txt',
               'kernel/guest_context.cpp','kernel/guest_context.hpp','guest/guest_harness.cpp')
    evidence = dict(scope='CPU SDK software submission/park pair only, with its per-present A/B and its arithmetic '
                          'admission against the write tracker, foreign page protection and the arena release; no '
                          'Native0, pump or shipping acceptance.',
                    source_sha256={p:digest(root/p) for p in sources},mutations=[])
    env = {k:v for k,v in os.environ.items() if not k.startswith('SR_')}
    env['PATH'] = r'C:\Program Files\LLVM\bin;' + env['PATH']
    env.update(SR_NATIVE_RINGLESS='0',SR_NATIVE_SEGMENTLESS='0',SR_PM4_RING='0',SR_PM4_RING_EXEC='0',
               SR_PM4_CP='0',SR_ALLOW_FORCING='0',SR_INSTANCE_ID='native-transport-host')
    # the mem.resources bake (native/mem-native a1fda568, 2026-10-17) made the native resource route the dev default, where the GPU write tracker is off: these cases test the page model's tracker, so they run at SR_MEM_RESOURCES=0 (the oracle arm); the native default's rows are composed_suites_run.py's *_native and native_route_admission_run.py
    env['SR_MEM_RESOURCES'] = '0'
    evidence['host_environment'] = {k:v for k,v in env.items() if k.startswith('SR_')}
    exe = root/'build/difftest.exe'
    if os.name == 'nt': ctypes.windll.kernel32.SetErrorMode(0x8003)
    quiescent = True
    def save():
        (out/'native_transport_matrix.json').write_text(json.dumps(evidence,indent=2)+'\n',encoding='utf-8')
    def run(tag, command, settings=None, timeout=60):
        nonlocal quiescent
        local = dict(env, **(settings or {}))
        with (out/(tag+'_stdout.txt')).open('xb') as stdout, (out/(tag+'_stderr.txt')).open('xb') as stderr:
            process = subprocess.Popen(command,cwd=root,env=local,stdout=stdout,stderr=stderr)
            quiescent = False
            try:
                code = process.wait(timeout=timeout)
                quiescent = True
            except subprocess.TimeoutExpired:
                stop_windows_tree(process)
                quiescent = True
                code = 124
                stderr.write(b'\nBounded timeout; process tree stopped and checked\n')
        raw = (out/(tag+'_stdout.txt')).read_bytes()
        row = dict(command=[str(x) for x in command],exit=code,exe_sha256=digest(exe) if exe.exists() else None,
                   stdout_sha256=hashlib.sha256(raw).hexdigest(),stderr_sha256=digest(out/(tag+'_stderr.txt')))
        print(json.dumps(dict(tag=tag,exit=code)),flush=True)
        return row,raw
    def test(tag, native=True, defect=None, mode=None, diagnostic=None):
        settings = {'SR_NATIVE_TRANSPORT':'1' if native else '0'}
        if mode == 'ab': settings.update(AB_ENV)
        if mode == 'exclusion-env': settings.update([diagnostic or DIAGNOSTIC_ENVS[0]])
        row,raw = run(tag,[exe,'--native-transport-test',mode or ('native' if native else 'gate-off')],settings)
        row['mode'] = mode or ('native' if native else 'gate-off')
        if mode == 'exclusion-env': row['diagnostic'] = list(diagnostic or DIAGNOSTIC_ENVS[0])
        row['summary'] = [[int(a),int(b)] for a,b in re.findall(rb'\[NATIVE_TRANSPORT_TEST\] (\d+) passed, (\d+) failed',raw)]
        checks = MODE_CHECKS.get(mode, CHECKS)
        row['valid'] = grade(row['exit'],raw,defect,checks=checks)
        row['failures'] = [line[6:].decode(errors='replace') for line in raw.replace(b'\r\n',b'\n').splitlines()
                           if line.startswith(b'FAIL: ')]
        return row
    def build(tag, target='difftest'):
        guard = '''$ErrorActionPreference='Stop'
if (@(Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(SupermanReturns|xenia.*)\\.exe$' }).Count) { throw 'Wait for games before building' }
if ((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory -lt 8GB/1KB) { throw 'Build RAM headroom insufficient' }
'''
        subprocess.run(['powershell','-NoProfile','-Command',guard],check=True,capture_output=True,timeout=60)
        # The shared worker's sccache server sometimes drops a connection (os error 10054): retried twice, recorded.
        for attempt in range(3):
            row,raw = run(tag if not attempt else '%s_retry%d' % (tag,attempt),
                          ['cmake','--build','build','--target',target,'-j',str(args.jobs)],timeout=3600)
            if row['exit'] == 0 or b'sccache: error' not in raw: break
        row['attempts'] = attempt + 1
        return row
    def replace(old,new):
        if original.count(old) != 1: raise ValueError('Source contract changed; refusing mutation')
        return original.replace(old,new)
    # The census of the exe's build inputs (guest_protect_census.py) reads ninja's graph and deps log: the exe is built
    # first, so every object's recorded dependencies are current.
    evidence['census_build'] = build('census_build','SupermanReturns')
    if evidence['census_build']['exit']:
        save(); return 1
    problems,sites,scanned,platform,binaries = census_run(root/'build','SupermanReturns.exe')
    evidence['guest_protect_census'] = dict(problems=problems,inputs_scanned=scanned,platform_files_skipped=platform,
                                            binaries_classified=len(binaries),sites=sum(sites.values()),keys=len(sites))
    save()
    if problems or not scanned:
        print(json.dumps(dict(valid=False,census=problems)),flush=True)
        return 1
    def positives(first, prefix):
        """Every mode on the current build: native as `first`, the others prefixed (positive: '', restored:
        'restored_'); the exclusion-env mode once per page-protecting diagnostic. Returns the evidence keys."""
        runs = [(first,{}),(prefix+'gate_off',dict(native=False)),(prefix+'ab',dict(mode='ab')),
                (prefix+'permissions',dict(mode='permissions')),(prefix+'exclusion',dict(mode='exclusion')),
                (prefix+'retire',dict(mode='retire'))]
        runs += [(prefix+'exclusion_env_'+name,dict(mode='exclusion-env',diagnostic=(name,value)))
                 for name,value in DIAGNOSTIC_ENVS]
        for tag,kwargs in runs:
            evidence[tag] = test(tag,**kwargs)
            save()
        return [tag for tag,_ in runs]
    positive_tags = positives('positive','')
    if not all(evidence[k]['valid'] for k in positive_tags): return 1
    cases = [
        ('chain_recheck',replace(b'if (readTransportWord(dev + 0x2A74u)) {\n      const uint32_t previousTail',
                                b'if (true) {\n      const uint32_t previousTail')),
        ('terminal_recheck',replace(b'ChainLock lock(work, base, state + 0x3Cu);\n  // Re-read under',
                                   b'const uint32_t staleMarker = readTransportWord(at);\n  ChainLock lock(work, base, state + 0x3Cu);\n  // Re-read under')
                           .replace(b'const uint32_t marker = readTransportWord(at);',b'const uint32_t marker = staleMarker;')),
        ('tail_link',replace(b'writeTransportWord(previousTail, address);',b'/* omit tail publication */')),
        ('return_width',replace(b'ctx.r3.u64 = returned + (chained ? 16u : 0u);',
                               b'ctx.r3.u64 = uint32_t(returned + (chained ? 16u : 0u));')),
        ('pause_completion',replace(b'if (!readTransportWord(state + 0x50u)) writeTransportWord(state + 0x38u,',b'if (true) writeTransportWord(state + 0x38u,')),
        ('alignment',replace(b'((dev | at | list | ctx.r1.u32) & 3u)',b'false')
                     .replace(b'((state | at) & 3u)',b'false')
                     .replace(b'(previousTail & 3u)',b'false')),
        ('thread_owner',replace(b'K::keSpinlockEnabled() && ctx.r13.u32 &&\n         K::kpcrCurrent() == ctx.r13.u32',
                                b'K::keSpinlockEnabled()')),
        # The arithmetic admission (2026-10-14): a range need not map contiguously / need not lie in the arena.
        ('contiguity',replace(b'if (!first || !last || last - first != ptrdiff_t(bytes - 1u)) return false;',
                              b'if (!first || !last) return false;')),
        ('arena_bound',replace(b'if (!arena || first < arena || size_t(last - arena) >= arenaSize) return false;',
                               b'if (!arena) return false;')),
        # The pair's stores go around the tracker's handler: a host write under a temporary VirtualProtect, the
        # page left protected and validated though written (no fault, release or invalidation).
        ('pair_bypass',replace(b'void writeTransportWord(uint32_t va, uint32_t value) { K::writeGuestBE32(va, value); }',
                               b'void writeTransportWord(uint32_t va, uint32_t value) {\n'
                               b'  uint8_t* host = K::guestVAToHost(va); DWORD old = 0, ignored = 0;\n'
                               b'  VirtualProtect(host, 4u, PAGE_READWRITE, &old);\n'
                               b'  K::writeGuestBE32(va, value);\n'
                               b'  VirtualProtect(host, 4u, old, &ignored);\n}')),
    ]
    # (name, source, original, mutated, test mode, restored_control: the SDK control runs on rebuilt restored source)
    cases = [(name,source,original,mutated,None,False) for name,mutated in cases]
    cases += [
        # foreign page protection: a declared range admitted; the exclusion ignored; the requested diagnostic ignored
        ('declared_range',source,original,replace(b'if (K::guest_protect::intersects(first, bytes)) {',b'if (false) {'),
         'exclusion',False),
        ('exclusion',source,original,
         replace(b'return K::guest_protect::diagnosticRequested() || K::guest_protect::excludesAll();',b'return false;'),
         'exclusion',False),
        ('exclusion_env',source,original,
         replace(b'return K::guest_protect::diagnosticRequested() || K::guest_protect::excludesAll();',
                 b'return K::guest_protect::excludesAll();'),'exclusion-env',False),
        # each entry's own A/B window: the append ignores it / EndSync ignores it / an OFF window skips the call
        ('ab_append_window',source,original,
         replace(b'  if (!abWindowOn()) return abOff(g_abOffAppend);\n',b''),'ab',False),
        ('ab_endsync_window',source,original,
         replace(b'  if (!abWindowOn()) return abOff(g_abOffEnd);\n',b''),'ab',False),
        ('ab_off_skips',source,original,
         replace(b'return false;  // the SDK body runs',b'return true;  // the SDK body runs'),'ab',False),
    ]
    ownership = b'p == ~0u || !(g_st[p].load(std::memory_order_relaxed) & kProt)'
    if watcher_original.count(ownership)!=1: raise ValueError('Tracker ownership contract changed; refusing mutation')
    cases.append(('tracked_ownership',watcher,watcher_original,
                  watcher_original.replace(ownership,b'p == ~0u'),'permissions',True))
    # The tracker's handler unprotects a faulting block but keeps its pages owned and validated: the release the
    # pair's stores rely on is broken for every arm, so the SDK control runs on rebuilt restored source.
    release = b'if (!keep) g_st[p].store(0, std::memory_order_release);'
    if watcher_original.count(release)!=1: raise ValueError('Tracker release contract changed; refusing mutation')
    cases.append(('handler_release',watcher,watcher_original,
                  watcher_original.replace(release,b'(void)keep;'),None,True))
    # The arena's release (Astra round A, P1), graded in retire mode.
    def other(path, edits):
        target = root/path
        before = target.read_bytes()
        (out/(Path(path).name+'.original')).write_bytes(before)
        mutated = before
        for old,new in edits:
            if before.count(old) != 1: raise ValueError('%s contract changed; refusing mutation' % path)
            mutated = mutated.replace(old,new)
        return target,before,mutated
    cases.append(('arena_drain',)+other('kernel/guest_protect.hpp',
        [(b'while (g_arenaLeases.load(std::memory_order_seq_cst)) {',b'while (false) {')])+('retire',False))
    end_lease = b'  K::guest_protect::ArenaLease endLease;\n  if (!endLease) return decline(DeclineReason::Retired);\n'
    if original.count(end_lease) != 1: raise ValueError('Source contract changed; refusing mutation')
    cases.append(('arena_lease',source,original,
        replace(b'  K::guest_protect::ArenaLease appendLease;\n  if (!appendLease) return decline(DeclineReason::Retired);\n',
                b'').replace(end_lease,b''),'retire',False))
    cases.append(('release_retire',)+other('kernel/guest_context.cpp',
        [(b'if (!drained) {',b'if (!drained && false) {')])+('retire',False))
    try:
        for name,target,before,mutated,mode,query_only in cases:
            if target.read_bytes()!=before: raise ValueError('Concurrent edit; refusing mutation')
            row = dict(name=name,source=target.relative_to(root).as_posix(),
                       test_mode=mode or 'native',mutated_sha256=hashlib.sha256(mutated).hexdigest())
            evidence['mutations'].append(row)
            target.write_bytes(mutated)
            try:
                row['build'] = build(name+'_build')
                if row['build']['exit']: raise RuntimeError('Defect failed to build')
                row['test'] = test(name+'_test',defect=name,mode=mode)
                if not query_only: row['gate_off'] = test(name+'_gate_off',native=False)
                if not row['test']['valid'] or (not query_only and not row['gate_off']['valid']):
                    raise RuntimeError('Defect specificity or unchanged SDK control failed')
            finally:
                if not quiescent: raise RuntimeError('Tree not confirmed stopped; refusing restoration')
                if target.read_bytes()!=mutated: raise ValueError('Concurrent edit; refusing overwrite')
                target.write_bytes(before)
                row['source_restored'] = target.read_bytes()==before
                save()
            if query_only:
                # The ownership mutant is query-only: entering a deliberately
                # mis-admitted store would test a crash, not the failed predicate.
                # The handler-release mutant breaks the tracker for the SDK too.
                # Rebuild restored production bytes for the complete SDK control.
                row['gate_off_source_restored'] = True
                row['gate_off_build'] = build(name+'_sdk_control_build')
                if row['gate_off_build']['exit']: raise RuntimeError('Restored SDK control failed to build')
                row['gate_off'] = test(name+'_gate_off',native=False)
                if not row['gate_off']['valid']: raise RuntimeError('Restored SDK control failed')
                save()
    finally:
        evidence['all_sources_restored'] = all(digest(root/p)==v for p,v in evidence['source_sha256'].items())
        evidence['process_quiescent'] = quiescent
        if evidence['all_sources_restored'] and quiescent:
            evidence['restored_build'] = build('restored_build')
            restored_tags = positives('restored','restored_') if evidence['restored_build']['exit']==0 else ['restored']
        else: restored_tags = ['restored']
        evidence['valid'] = len(evidence['mutations'])==len(cases) and all(r.get('test',{}).get('valid') and
            r.get('gate_off',{}).get('valid') and r.get('source_restored') for r in evidence['mutations']) and \
            evidence['all_sources_restored'] and quiescent and \
            all(evidence.get(k,{}).get('valid',False) for k in restored_tags)
        save()
    print(json.dumps(dict(valid=evidence['valid'],receipt=str(out/'native_transport_matrix.json'))),flush=True)
    return int(not evidence['valid'])


if __name__ == '__main__': raise SystemExit(main())
