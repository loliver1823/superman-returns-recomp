"""Preserve exact qualifying freeze witnesses after the full gate is terminal.

Usage: python tools/difftest/native_live_proof.py --bank archive/<candidate>
Requires live/live_receipt.json plus the full quiet gate and its retained raw
frames. Refuses an existing proof directory; never removes raw frames. Sampled
log records are neither peak usage nor final totals. Run canonical frame GC
only after this report and selected PNGs have been preserved on D:.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/difftest'))
import native_draw_controls as shared


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def plan_pairs(freeze_grade, markers, present_files):
    """Reject mismatched identities or incomplete world coverage before writes."""
    world_ids = freeze_grade['world']['gameplay'] + freeze_grade['world']['menu_world']
    world = set(world_ids)
    if not world or len(world) != len(world_ids) or freeze_grade['world_drawn'] != len(world):
        raise ValueError('Qualifying world identities/count do not reconcile')
    if freeze_grade['counted'] < len(world):
        raise ValueError('Qualifying world exceeds the counted freeze scope')
    seen = set()
    planned = []
    for freeze in freeze_grade['freezes']:
        fid = freeze['freeze']
        if fid not in world:
            continue
        if not freeze['ingame'] or fid in seen:
            raise ValueError('World freeze is ungraded or repeated')
        seen.add(fid)
        rows = sorted(row for row in markers.get(fid, []) if row[1] in present_files)
        presents = [row[1] for row in rows]
        if len(rows) < 2 or len(presents) != len(set(presents)):
            raise ValueError('Frozen-world markers are absent or ambiguous')
        cols = [x.strip() for x in freeze['line'].split('|')]
        if len(cols) != 8 or int(cols[0]) != fid or int(cols[1]) != len(rows) or int(cols[2]) != len(rows) - 1:
            raise ValueError('Frozen-world row identity/frame scope differs from markers')
        pair = re.fullmatch(r'p(\d+)->p(\d+)', cols[4])
        if pair:
            selected = list(map(int, pair.groups()))
            if selected[0] not in presents or selected[1] not in presents:
                raise ValueError('Reported pair does not belong to its freeze')
            if presents.index(selected[1]) != presents.index(selected[0]) + 1:
                raise ValueError('Reported pair is not consecutive within its freeze')
        else:
            if cols[4] != '-' or int(cols[3]) != 0 or int(cols[5]) != 0 or int(cols[6]) != 0:
                raise ValueError('Missing pair is not a zero-difference row')
            middle = max(1, len(rows) // 2)
            selected = presents[middle - 1:middle + 1]
        planned.append((freeze, selected, cols))
    if seen != world or len(planned) != len(world):
        raise ValueError('Pair coverage differs from the qualifying world set/count')
    return planned


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=Path, required=True)
    args = parser.parse_args()
    live = args.bank.resolve() / 'live'
    terminal = read(live / 'live_receipt.json')
    if not terminal['terminal_observed'] or terminal['remaining_game_processes'] or terminal['slot_release_exit_code'] != 0:
        raise ValueError('Full live gate has not reached a confirmed terminal state')
    gate = live / 'gate/flicker_gate.json'
    grade = read(gate)
    proof = live / 'proof'
    if proof.exists():
        raise ValueError('Proof directory already exists')
    receipt = {'scope': 'Preserved qualifying frozen-world pairs and sampled native binding witnesses; no causal, performance, native-zero or release acceptance',
               'grade_sha256': shared.digest(gate), 'live_receipt_sha256': shared.digest(live / 'live_receipt.json'),
               'collector_sha256': shared.digest(Path(__file__)), 'pairs': [], 'phases': {}}
    dumps = live / 'gate/freeze/drive/attempt1/frames'
    files = {}
    for path in dumps.glob('present_*.bmp'):
        m = re.search(r'_p(\d+)_', path.name)
        if m:
            present = int(m.group(1))
            if present in files:
                raise ValueError('Duplicate present number')
            files[present] = path
    freeze_report = read(live / 'gate/freeze/report.json')
    markers = {}
    for line in Path(freeze_report['log']).read_text(errors='replace').splitlines():
        m = re.search(r'\[SIM_FREEZE_DUMP\] freeze=(\d+) k=(\d+) present=(\d+)', line)
        if m:
            fid, k, present = map(int, m.groups())
            if present in files:
                markers.setdefault(fid, []).append((k, present))
    pairs = plan_pairs(grade['freeze'], markers, files)
    proof.mkdir()
    for freeze, presents, cols in pairs:
        fid = freeze['freeze']
        arrays = []
        records = []
        for present in presents:
            source = files[present]
            destination = proof / ('freeze_%02d_p%06d.png' % (fid, present))
            image = Image.open(source).convert('RGB')
            arrays.append(np.asarray(image, dtype=np.int16))
            image.save(destination)
            records.append({'present': present, 'source_name': source.name, 'source_sha256': shared.digest(source),
                            'proof': str(destination.relative_to(live)), 'proof_sha256': shared.digest(destination)})
        delta = arrays[1] - arrays[0]
        pixels = int(np.any(np.abs(delta) > 1, axis=2).sum())
        maximum = int(np.abs(delta).max())
        if pixels != int(cols[5]) or (pixels and maximum != int(cols[6])):
            raise ValueError('Independent proof delta differs from exact gate table')
        receipt['pairs'].append({'freeze': fid, 'failed_gate': fid in grade['freeze']['bad'],
            'greater_than_one_pixels': pixels, 'max_difference': maximum,
            'reported_max_difference': int(cols[6]), 'any_changed_pixels': int(np.any(delta != 0, axis=2).sum()),
            'positive_channel_changes': int((delta > 0).sum()), 'negative_channel_changes': int((delta < 0).sum()),
            'signed_channel_sum': int(delta.sum()), 'files': records})
    for phase in ['span', 'freeze']:
        folder = live / 'gate' / phase
        report = read(folder / 'report.json')
        log = Path(report['log'])
        samples, failures = [], []
        for number, line in enumerate(log.read_text(errors='replace').splitlines(), 1):
            if '[NATIVE_RECORDING]' not in line:
                continue
            if 'submitted=' in line:
                sample = {key: int(value) for key, value in re.findall(r'([a-z_]+)=(\d+)', line)}
                sample.update(line=number, raw_line=line)
                samples.append(sample)
            else:
                failures.append({'line': number, 'raw_line': line})
        if not samples:
            raise ValueError('Native binding samples absent')
        attempt = folder / 'drive/attempt1'
        payload = {'scope': 'Periodic native binding samples, not peaks or final totals',
                   'records': samples, 'log_sha256': shared.digest(log), 'recording_failures': failures}
        shared.write_json(live / (phase + '_native_samples.json'), payload)
        receipt['phases'][phase] = {'sample_count': len(samples), 'last_sample': samples[-1],
            'max_sampled_constant_live_bytes': max(x['constant_live_bytes'] for x in samples),
            'max_sampled_geometry_live_bytes': max(x['geometry_live_bytes'] for x in samples),
            'max_sampled_sampler_versions': max(x['sampler_versions'] for x in samples),
            'recording_failures': failures, 'log_sha256': shared.digest(log),
            'outcome_sha256': shared.digest(attempt / 'outcome.json'),
            'provenance_sha256': shared.digest(attempt / 'provenance.json'),
            'perf_report_sha256': shared.digest(folder / 'report.json')}
    shared.write_json(live / 'proof_receipt.json', receipt)
    print('Preserved', len(receipt['pairs']) * 2, 'PNG witnesses; gate', grade['verdict'])


if __name__ == '__main__':
    main()
