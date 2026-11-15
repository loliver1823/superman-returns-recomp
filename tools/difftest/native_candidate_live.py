#!/usr/bin/env python3
"""Run the full quiet gameplay gate after a pinned candidate replay bank.

    python tools/difftest/native_candidate_live.py --bank archive/<built-bank>

Both motion and simulation-freeze rolls use a copied private post-Warworld save.
This is a renderer diagnostic: audio/mods are off, not feature/native-zero proof.
The outer game slot stays held until the wrapper and all game processes end.
No raw deletion; preserve proofs with native_live_proof.py before scratch GC.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

import psutil
import native_draw_controls as shared

ROOT = Path(__file__).resolve().parents[2]
DEV = ROOT.parent / 'dev'
GATES = ('SR_NATIVE_GATE=0,SR_NATIVE_REPLAY_KICK=0,SR_HDR_ADAPT_DT=0,'
         'SR_NATIVE_SEGMENTLESS=1,SR_NATIVE_CALLBACK=0,SR_NATIVE_SEG_TILE=1,'
         'SR_NATIVE_FLUSH_CHECK=0,SR_NATIVE_FLUSH_CPPM4=0,SR_NATIVE_FLUSH_PM4=0,'
         'SR_HIGHFPS=120,SR_AUDIO_OUT=0,SR_WINDOW_BACKGROUND=1,SR_HARNESS_INPUT=1,'
         'SR_HARNESS_REAL_INPUT=0,SR_LOG=dev,SR_FRAME_DUMP=0,SR_PHYS_ALIAS_A=0,'
         'SR_MODS=0,SR_HDR_ASYNC_PROBE=0,SR_HDR_FREEZE_PROBE=0')


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def held(base, pins):
    return bool(pins) and all(shared.digest(base / name) == pin for name, pin in pins.items())


def require(ok, message):
    if not ok:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=Path, required=True)
    args = parser.parse_args()
    bank = args.bank.resolve()
    build = read(bank / 'build_receipt.json')
    replay = read(bank / 'replay/receipt.json')
    require(build['exit_code'] == 0 and build['terminal_observed'] is True,
            'Build is not successful terminal evidence')
    require(replay.get('matched_expected') is True and replay.get('slot_release_exit_code') == 0,
            'Replay bank is not passing and released')
    replay_guards = {k: v for k, v in replay.items() if k.endswith('_unchanged')}
    require(bool(replay_guards) and all(v is True for v in replay_guards.values()),
            'Replay provenance guards are missing/failed')
    require(held(ROOT, build['source_pins']) and held(bank / 'source_snapshot', build['source_pins']),
            'Compiled sources differ from snapshot/current source')
    require(replay['source_pins_at_execution'] == build['source_pins'] and
            replay['binary_pins'] == build['binary_pins'], 'Replay does not identify this build')
    require(held(bank / 'binary', build['binary_pins']) and held(ROOT, build['owner_wip_pins']),
            'Binary or protected owner files changed')
    save = DEV / 'archive/saves/post_warworld'
    require(save.is_dir() and any(p.is_file() for p in save.rglob('*')), 'Private seed is empty/missing')
    seed_pins = {str(p.relative_to(save)): shared.digest(p) for p in save.rglob('*') if p.is_file()}
    out = bank / 'live'
    out.mkdir(exist_ok=False)
    shutil.copy2(Path(__file__), out / 'runner_at_execution.py')
    harness = [ROOT / ('tools/perf/' + name) for name in (
        'flicker_gate.py', 'flicker_diff.py', 'flicker_rule.py', 'freeze_diff.py', 'world_floor.py',
        'perf_report.py', 'gpu_hang_check.py', 'flicker_play.txt')]
    harness += [DEV / ('tools/' + name) for name in (
        'perf/perf_run.py', 'sr_drive.ps1', 'sr_artifacts.ps1')]
    harness += [ROOT / ('tools/' + name) for name in (
        'sr_artifacts.ps1', 'game_slot.py', 'difftest/native_draw_controls.py',
        'difftest/native_candidate_live.py')]
    harness_pins = {str(p): shared.digest(p) for p in harness}
    command = [sys.executable, str(ROOT / 'tools/perf/flicker_gate.py'),
               '--exe', str(bank / 'binary/SupermanReturns.exe'), '--out', str(out / 'gate'),
               '--instance', bank.name + '-live', '--from-save', str(save),
               '--save-label', 'Private-post-Warworld-' + bank.name + '-diagnostic',
               '--gates', GATES, '--repo', str(DEV)]
    env = {k: v for k, v in os.environ.items() if not k.startswith('SR_')}
    temp = ROOT / 'archive/native_recovery_20261012/temp'
    require(temp.is_dir(), 'Scratch temp directory is missing')
    env.update(SR_MAIN_ROOT=str(DEV), TEMP=str(temp), TMP=str(temp))
    # shared.release_slot inherits this process's environment. Acquire and
    # release must resolve the same lock even if the caller supplied another root.
    os.environ['SR_MAIN_ROOT'] = env['SR_MAIN_ROOT']
    slot = [sys.executable, str(ROOT / 'tools/game_slot.py')]
    owner = bank.name + '_full_live'
    acquire = subprocess.run(slot + ['acquire', '--owner', owner, '--min-free-gb', '12',
                                    '--max-hours', '0.05', '--poll', '5'],
                             env=env, capture_output=True, text=True, timeout=190)
    shared.write_json(out / 'slot_acquire.json', {'exit_code': acquire.returncode,
                                                'stdout': acquire.stdout, 'stderr': acquire.stderr})
    require(acquire.returncode == 0, 'Game slot unavailable')
    receipt = {'scope': 'Full quiet motion + simulation-freeze diagnostic from private post-Warworld '
                        'dev-tool save; audio/mods off; not native-zero or PC-feature acceptance',
               'arguments': command, 'owner': owner, 'harness_pins': harness_pins,
               'source_pins_at_execution': build['source_pins'],
               'compiled_source_snapshot_pins': build['source_pins'],
               'binary_pins': build['binary_pins'], 'owner_wip_pins': build['owner_wip_pins'],
               'seed_root': str(save), 'seed_pins': seed_pins,
               'build_receipt_sha256': shared.digest(bank / 'build_receipt.json'),
               'replay_receipt_sha256': shared.digest(bank / 'replay/receipt.json'),
               'runner_sha256': shared.digest(Path(__file__)), 'slot_acquire_exit_code': 0}
    proc = None
    descendants = {}
    start = time.monotonic()
    try:
        with (out / 'stdout.log').open('wb') as stdout, (out / 'stderr.log').open('wb') as stderr:
            proc = subprocess.Popen(command, cwd=ROOT, env=env, stdout=stdout, stderr=stderr)
            receipt['pid'] = proc.pid
            shared.write_json(out / 'live_running.json', receipt)
            timed_out = False
            while proc.poll() is None:
                try:
                    for child in psutil.Process(proc.pid).children(recursive=True):
                        descendants[child.pid] = {'pid': child.pid, 'created': child.create_time(),
                                                 'name': child.name()}
                except psutil.NoSuchProcess:
                    pass
                if time.monotonic() - start > 1200:
                    timed_out = True
                    break
                time.sleep(1)
            receipt.update(timed_out=timed_out, elapsed_seconds=time.monotonic() - start)
    except BaseException as error:
        receipt['error'] = repr(error)
        raise
    finally:
        # Kill only descendants whose creation time still matches this run.
        termination_errors = []
        if proc is not None and proc.poll() is None:
            for identity in descendants.values():
                try:
                    child = psutil.Process(identity['pid'])
                    if child.create_time() == identity['created']:
                        child.kill()
                except psutil.NoSuchProcess:
                    pass
                except psutil.AccessDenied as error:
                    termination_errors.append(repr(error))
            receipt['termination'] = shared.terminate_child(proc)
        receipt.update(exit_code=proc.returncode if proc else None,
                       terminal_observed=proc is not None and proc.poll() is not None,
                       descendants=list(descendants.values()))
        remaining = [p.info for p in psutil.process_iter(['pid', 'name']) if
                     any(n in (p.info['name'] or '').lower() for n in ('supermanreturns', 'xenia'))]
        receipt['remaining_game_processes'] = remaining
        remaining_owned = []
        for identity in descendants.values():
            try:
                child = psutil.Process(identity['pid'])
                if child.create_time() == identity['created'] and child.is_running():
                    remaining_owned.append(identity)
            except psutil.NoSuchProcess:
                pass
            except psutil.AccessDenied as error:
                termination_errors.append(repr(error))
        receipt.update(remaining_owned_descendants=remaining_owned, termination_errors=termination_errors)
        if not remaining and not remaining_owned and not termination_errors and (proc is None or proc.poll() is not None):
            receipt['slot_release_exit_code'] = shared.release_slot(slot, owner, out, proc).returncode
        else:
            receipt['slot_release_exit_code'] = None
            receipt['slot_retained_reason'] = 'Game or child has no confirmed terminal state'
        receipt.update(source_pins_unchanged=held(ROOT, build['source_pins']),
                       compiled_source_snapshot_pins_unchanged=held(bank / 'source_snapshot', build['source_pins']),
                       owner_wip_pins_unchanged=held(ROOT, build['owner_wip_pins']),
                       binary_pins_unchanged=held(bank / 'binary', build['binary_pins']),
                       seed_pins_unchanged=held(save, seed_pins),
                       harness_pins_unchanged=all(shared.digest(Path(p)) == pin for p, pin in harness_pins.items()))
        for name in ('stdout', 'stderr'):
            if (out / (name + '.log')).exists():
                receipt[name + '_sha256'] = shared.digest(out / (name + '.log'))
        shared.write_json(out / 'live_receipt.json', receipt)
    print('Full live terminal exit:', receipt.get('exit_code'), flush=True)
    valid = receipt['terminal_observed'] and not receipt['timed_out'] and not remaining and not remaining_owned and \
        not termination_errors and \
        receipt['slot_release_exit_code'] == 0 and all(v for k, v in receipt.items() if k.endswith('_unchanged'))
    return receipt['exit_code'] if valid else 2


if __name__ == '__main__':
    sys.exit(main())
