"""Preserve and measure every qualifying frozen-world frame after a live gate.

    python tools/difftest/native_freeze_sequence.py --bank archive/<candidate>

Uses the existing gate's world identities and log membership. This diagnoses
where differences occur; it never changes thresholds, omits early frames or
supersedes the full gate verdict. Writes a fresh live/freeze_sequence bank on D.
Keep raw frames until this collector ends; use the canonical scratch GC afterward.
"""
import argparse
import json
from pathlib import Path
import re

import numpy as np
from PIL import Image

import native_draw_controls as shared
from native_live_proof import plan_pairs

MARKER = re.compile(r'\[SIM_FREEZE_DUMP\] freeze=(\d+) k=(\d+) present=(\d+) '
                    r'held_ms=(\d+) shown_stamp=([\d.]+) s')


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sequence_metrics(frames):
    """Consume one freeze with at most two RGB arrays live; reject ambiguous clocks."""
    previous = None
    rows, pairs, seen = [], [], set()
    for metadata, image in frames:
        current = np.asarray(image, dtype=np.int16)
        if current.ndim != 3 or current.shape[2] != 3:
            raise ValueError('Expected an RGB frame')
        if metadata['present'] in seen or metadata['k'] != len(rows):
            raise ValueError('Repeated present or incomplete capture sequence')
        seen.add(metadata['present'])
        if rows:
            old = rows[-1]
            if metadata['present'] <= old['present'] or metadata['held_ms'] <= old['held_ms']:
                raise ValueError('Nonmonotonic freeze membership')
            if metadata['shown_stamp'] != old['shown_stamp']:
                raise ValueError('Shown simulation clock changed inside freeze')
            if current.shape != previous.shape:
                raise ValueError('Freeze image dimensions changed')
            signed = current - previous
            delta = np.abs(signed)
            mask = np.any(delta > 1, axis=2)
            yy, xx = np.nonzero(mask)
            pairs.append({'from_present': old['present'], 'to_present': metadata['present'],
                'from_held_ms': old['held_ms'], 'to_held_ms': metadata['held_ms'],
                'greater_than_one_pixels': int(mask.sum()),
                'any_changed_pixels': int(np.any(delta, axis=2).sum()),
                'max_difference': int(delta.max()),
                'positive_channel_changes': int((signed > 0).sum()),
                'negative_channel_changes': int((signed < 0).sum()),
                'signed_channel_sum': int(signed.sum()),
                'bbox': [int(xx.min()), int(yy.min()), int(xx.max()), int(yy.max())] if len(xx) else None})
        rows.append(metadata)
        previous = current
    if len(rows) < 2:
        raise ValueError('No consecutive frozen frame evidence')
    return {'frames': rows, 'pairs': pairs,
            'large_pair_indices': [i for i, p in enumerate(pairs) if p['max_difference'] > 8],
            'beyond_speck_pair_indices': [i for i, p in enumerate(pairs)
                                         if p['greater_than_one_pixels'] > 64 or p['max_difference'] > 8]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bank', type=Path, required=True)
    args = parser.parse_args()
    live = args.bank.resolve() / 'live'
    terminal = read(live / 'live_receipt.json')
    if not terminal['terminal_observed'] or terminal['remaining_game_processes'] or terminal['slot_release_exit_code'] != 0:
        raise ValueError('Live gate is not terminal and released')
    grade_path = live / 'gate/flicker_gate.json'
    grade = read(grade_path)
    report_path = live / 'gate/freeze/report.json'
    log = Path(read(report_path)['log'])
    frames = {}
    for path in (live / 'gate/freeze/drive/attempt1/frames').glob('present_*.bmp'):
        match = re.search(r'_p(\d+)_', path.name)
        if match:
            present = int(match[1])
            if present in frames:
                raise ValueError('Duplicate raw present number')
            frames[present] = path
    markers, clocks = {}, {}
    for line in log.read_text(encoding='utf-8', errors='replace').splitlines():
        match = MARKER.search(line)
        if match:
            fid, k, present, held = map(int, match.groups()[:4])
            markers.setdefault(fid, []).append((k, present))
            clocks.setdefault(fid, []).append({'k': k, 'present': present, 'held_ms': held,
                                               'shown_stamp': match[5]})
    plans = plan_pairs(grade['freeze'], markers, frames)
    for row, _, _ in plans:
        if any(m['present'] not in frames for m in clocks[row['freeze']]):
            raise ValueError('Raw frame missing from qualifying freeze')
    out = live / 'freeze_sequence'
    out.mkdir(exist_ok=False)
    receipt = {'scope': 'All qualifying frozen-world pairs; early frames remain counted; no causal attribution or acceptance',
               'original_gate_verdict': grade['verdict'], 'original_bad_freezes': grade['freeze']['bad'],
               'input_pins': {str(p): shared.digest(p) for p in
                   (grade_path, report_path, log, live / 'live_receipt.json', Path(__file__),
                    Path(__file__).with_name('native_live_proof.py'))}, 'freezes': [], 'proof_pins': {}}
    for row, _, _ in plans:
        fid = row['freeze']
        def images():
            for metadata in sorted(clocks[fid], key=lambda m: m['k']):
                raw = frames[metadata['present']]
                with Image.open(raw) as bmp:
                    image = bmp.convert('RGB')
                proof = out / ('freeze%02d_p%06d.png' % (fid, metadata['present']))
                image.save(proof)
                receipt['proof_pins'][proof.name] = shared.digest(proof)
                yield {**metadata, 'source_bmp_sha256': shared.digest(raw),
                       'proof': proof.name, 'proof_sha256': receipt['proof_pins'][proof.name]}, image
        measured = sequence_metrics(images())
        measured['freeze'] = fid
        receipt['freezes'].append(measured)
        shared.write_json(out / 'receipt.json', receipt)
        print('freeze=%d frames=%d large_pair_indices=%s beyond_speck_pairs=%d' %
              (fid, len(measured['frames']), measured['large_pair_indices'],
               len(measured['beyond_speck_pair_indices'])), flush=True)
    receipt['input_pins_unchanged'] = all(shared.digest(Path(p)) == h for p, h in receipt['input_pins'].items())
    receipt['proof_pins_match'] = all(shared.digest(out / p) == h for p, h in receipt['proof_pins'].items())
    receipt['qualifying_freezes'] = len(plans)
    receipt['preserved_frames'] = len(receipt['proof_pins'])
    shared.write_json(out / 'receipt.json', receipt)
    return 0 if receipt['input_pins_unchanged'] and receipt['proof_pins_match'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
