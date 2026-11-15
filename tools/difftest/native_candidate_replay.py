#!/usr/bin/env python3
"""Pinned native-candidate parity bank; renderer correctness still needs Xenia/live.

    python tools/difftest/native_candidate_replay.py --bank archive/<built-bank> \
        --reference archive/<passing-bank>/replay [--staged-raw-pairs]

Requires build_receipt.json, source_snapshot/ and binary/ in the candidate bank.
The reference must contain all five complete trace replays with retained proofs.
Shares build/game tools, isolated raw J attempts and strict replay grading.
Never deletes frames; preserve D proofs then run canonical scratch retention.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import native_draw_controls as shared
import translated_constant_controls as controls
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
TRACES = ('fmv_eahd_454107ED_1124', 'fmv_t16', 'fmv_t21', 'fmv_t25',
          'menu_pressstart_454107ED_9415')


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def check_pins(base, pins):
    return bool(pins) and all(shared.digest(base / name) == value for name, value in pins.items())


def require(ok, message):
    if not ok: raise ValueError(message)


def validate_reference(prior):
    require(prior.get('matched_expected') is True and
            type(prior.get('slot_release_exit_code')) is int and prior['slot_release_exit_code'] == 0,
            'Reference bank lacks a passing aggregate result and released slot')
    guards = {k: v for k, v in prior.items() if k.endswith('_unchanged')}
    require(bool(guards) and all(v is True for v in guards.values()),
            'Reference bank has missing or failed provenance guards')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--staged-raw-pairs', action='store_true')
    args = parser.parse_args(); bank = args.bank.resolve(); reference = args.reference.resolve()
    out = bank / 'replay'; out.mkdir(exist_ok=False)
    build = read(bank / 'build_receipt.json'); prior = read(reference / 'receipt.json')
    validate_reference(prior)
    require(build['exit_code'] == 0 and build['terminal_observed'] is True, 'Candidate build is not successful terminal evidence')
    require(check_pins(bank / 'source_snapshot', build['source_pins']) and check_pins(ROOT, build['source_pins']),
            'Compiled source pins do not match snapshot/current source')
    require(check_pins(bank / 'binary', build['binary_pins']) and 'SupermanReturns.exe' in build['binary_pins'], 'Candidate binary pins differ')
    require(check_pins(ROOT, build['owner_wip_pins']), 'Protected owner files changed')
    require(tuple(e['trace'] for e in prior['comparisons']) == TRACES, 'Reference must identify exactly the five ordered traces')
    old_binary = reference.parent / 'binary'
    require(check_pins(old_binary, prior['binary_pins']), 'Reference binary pins differ')
    reference_inputs = {'receipt.json': shared.digest(reference / 'receipt.json')}
    trace_pins = {}
    for entry in prior['comparisons']:
        trace = entry['trace']; hashes = entry['raw_sha256']
        expected = {'stdout': hashes, 'stderr': entry['stderr_sha256']} if isinstance(hashes, str) else hashes
        for suffix, pin in [('.out', expected['stdout']), ('.err', expected['stderr']), ('_proof.png', entry['proof_sha256'])]:
            path = reference / (trace + suffix)
            require(shared.digest(path) == pin, 'Reference input hash differs: ' + path.name)
            reference_inputs[path.name] = pin
        draws = reference / (trace + '_draws.txt')
        require(shared.digest(draws) == entry['draws_sha256'], 'Reference draw hashes differ')
        reference_inputs[draws.name] = entry['draws_sha256']
        raw = (reference / (trace + '.out')).read_text(errors='replace') + '\n' + (reference / (trace + '.err')).read_text(errors='replace')
        require(controls.grade_replay(raw, entry['exit_code'], entry['timed_out'], entry['terminal_observed'], entry['summaries'])['matched_expected']
                and entry['different_pixels'] == 0 and entry['draw_hashes_equal'] is True, 'Reference does not have complete passing parity')
        path = Path(entry['gates']['SR_GPU_TRACE_REPLAY'])
        require(path.stem == trace and path.is_file(), 'Reference trace input differs/missing')
        trace_pins[str(path)] = shared.digest(path)
        require(prior['trace_pins'].get(str(path)) == trace_pins[str(path)], 'Trace changed after reference run')
    source_pins = dict(build['source_pins'])
    harness_pins = {p: shared.digest(ROOT / p) for p in (
        'tools/difftest/native_candidate_replay.py', 'tools/difftest/translated_constant_controls.py',
        'tools/difftest/native_draw_controls.py', 'tools/game_slot.py', 'tools/sr_artifacts.ps1')}
    shutil.copy2(Path(__file__), out / 'runner_at_execution.py')
    receipt = {'scope': 'Five fresh candidate parity replays; no Xenia fidelity, full-live or native-zero acceptance',
               'build_receipt_sha256': shared.digest(bank / 'build_receipt.json'), 'reference_inputs': reference_inputs,
               'source_pins_at_execution': source_pins, 'harness_pins': harness_pins,
               'owner_wip_pins': build['owner_wip_pins'], 'binary_pins': build['binary_pins'],
               'reference_binary_pins': prior['binary_pins'], 'trace_pins': trace_pins,
               'comparisons': [], 'legacy_pairs': []}
    shared.write_json(out / 'receipt.json', receipt)
    os.environ['SR_MAIN_ROOT'] = str(ROOT.parent / 'dev')
    slot = [sys.executable, str(ROOT / 'tools/game_slot.py')]; owner = 'native_candidate_replays'
    acquire = subprocess.run(slot + ['acquire', '--owner', owner, '--min-free-gb', '12', '--max-hours', '0.05', '--poll', '5'],
                             capture_output=True, text=True, timeout=190)
    shared.write_json(out / 'slot_acquire.json', {'exit_code': acquire.returncode, 'stdout': acquire.stdout, 'stderr': acquire.stderr})
    if acquire.returncode: raise RuntimeError('Game slot unavailable')
    active = {'proc': None}

    def run_one(entry, exe, folder, staged_raw=False):
        folder.mkdir(exist_ok=False)
        frame_dir, env, attempt = controls.production_inputs(folder, entry)
        if staged_raw: env['SR_PM4_VSF_SHM'] = '0'
        result = controls.run(exe, frame_dir, env, folder, active)
        shared.write_json(folder / 'terminal_receipt.json', result)
        raw = (folder / 'stdout.log').read_text(errors='replace') + '\n' + (folder / 'stderr.log').read_text(errors='replace')
        result.update(controls.grade_replay(raw, result['exit_code'], result['timed_out'], result['terminal_observed'], entry['summaries']))
        result.update(trace=entry['trace'], raw_attempt=attempt, gates={k: v for k, v in env.items() if k.startswith('SR_')})
        shared.write_json(folder / 'run_receipt.json', result)
        require(result['matched_expected'], 'Incomplete replay: ' + str(folder))
        bmp = frame_dir / 'replay_frame_1.bmp'
        Image.open(bmp).convert('RGB').save(folder / 'proof.png')
        result.update(source_bmp_sha256=shared.digest(bmp), proof_sha256=shared.digest(folder / 'proof.png'),
                      draws_sha256=shared.digest(folder / 'draws.txt'))
        shared.write_json(folder / 'run_receipt.json', result)
        return result

    try:
        for entry in prior['comparisons']:
            trace = entry['trace']; folder = out / trace
            result = run_one(entry, bank / 'binary/SupermanReturns.exe', folder)
            result.update(controls.compare_images(folder / 'proof.png', reference / (trace + '_proof.png')))
            result.update(summary_equal=result['summaries'] == entry['summaries'],
                          draw_hashes_equal=result['draws_sha256'] == entry['draws_sha256'],
                          raw_sha256=result['stdout_sha256'])
            for local, suffix in [('stdout.log', '.out'), ('stderr.log', '.err'), ('proof.png', '_proof.png'), ('draws.txt', '_draws.txt')]:
                shutil.copy2(folder / local, out / (trace + suffix))
            result['matched_expected'] = result['matched_expected'] and result['summary_equal'] and result['draw_hashes_equal'] and \
                result['different_pixels'] == 0 and result['max_difference'] == 0
            receipt['comparisons'].append(result); shared.write_json(out / 'receipt.json', receipt)
            print('[NATIVE_CANDIDATE_REPLAY] ' + trace + ' matched=' + str(result['matched_expected']), flush=True)
            require(result['matched_expected'], 'Candidate parity differs')
        if args.staged_raw_pairs:
            for trace in (TRACES[0], TRACES[-1]):
                entry = next(e for e in prior['comparisons'] if e['trace'] == trace)
                folder = out / ('staged_raw_' + trace); folder.mkdir()
                runs = [run_one(entry, base / 'SupermanReturns.exe', folder / name, True)
                        for base, name in [(old_binary, 'old'), (bank / 'binary', 'new')]]
                gates = [{k: v for k, v in r['gates'].items() if k != 'SR_REPLAY_DRAW_HASH'} for r in runs]
                pair = {'trace': trace, 'runs': runs, 'effective_gates_equal': gates[0] == gates[1],
                        'summary_equal': runs[0]['summaries'] == runs[1]['summaries'],
                        'draw_hashes_equal': runs[0]['draws_sha256'] == runs[1]['draws_sha256'],
                        **controls.compare_images(folder / 'new/proof.png', folder / 'old/proof.png')}
                pair['matched_expected'] = pair['effective_gates_equal'] and pair['summary_equal'] and pair['draw_hashes_equal'] and \
                    pair['different_pixels'] == 0 and pair['max_difference'] == 0
                receipt['legacy_pairs'].append(pair); shared.write_json(out / 'receipt.json', receipt)
                require(pair['matched_expected'], 'Staged raw parity differs')
                print('[NATIVE_CANDIDATE_REPLAY] staged_raw ' + trace + ' matched=True', flush=True)
    except BaseException as error:
        receipt['error'] = repr(error); raise
    finally:
        release = shared.release_slot(slot, owner, out, active['proc'])
        receipt.update(slot_release_exit_code=release.returncode,
                       source_pins_unchanged=check_pins(ROOT, source_pins),
                       harness_pins_unchanged=check_pins(ROOT, harness_pins),
                       owner_wip_pins_unchanged=check_pins(ROOT, build['owner_wip_pins']),
                       binary_pins_unchanged=check_pins(bank / 'binary', build['binary_pins']),
                       reference_binary_pins_unchanged=check_pins(old_binary, prior['binary_pins']),
                       reference_inputs_unchanged=check_pins(reference, reference_inputs),
                       trace_pins_unchanged=all(shared.digest(Path(p)) == pin for p, pin in trace_pins.items()))
        receipt['matched_expected'] = len(receipt['comparisons']) == 5 and len(receipt['legacy_pairs']) == (2 if args.staged_raw_pairs else 0) and \
            all(r['matched_expected'] for r in receipt['comparisons'] + receipt['legacy_pairs']) and \
            all(v for k, v in receipt.items() if k.endswith('_unchanged')) and release.returncode == 0 and 'error' not in receipt
        shared.write_json(out / 'receipt.json', receipt)
    return 0 if receipt['matched_expected'] else 1


if __name__ == '__main__':
    sys.exit(main())
