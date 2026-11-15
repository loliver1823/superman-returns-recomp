#!/usr/bin/env python3
"""parity_trace.py - key vs pad parity from one roll (iter-466 controls2, docs/PC_CONTROLS.md 2.6).

The roll (tools/controls/play_roll.ps1 + scripts/controls2_parity.play) presses each action twice:
`key-<name>` through the player keymap (the action layer) and `pad-<name>` on a ViGEm pad (the
game's own pad path). <out>/actions.jsonl (SR_PC_ACTIONS_TRACE) holds, per tick, the events the
game's commands answered ("out" = what the consumers read). For each pair this prints the events
that fired during the press window (minus the ones that fire at idle) and whether the two sets
are equal.

    python tools/controls/parity_trace.py <out dir> [--slack-ms 700]  (harness key-ups land ~0.15 s late)
Exit code 0 = every pair equal.
"""
import argparse
import collections
import json
import os
import sys


def load(path):
    rows = []
    with open(path, errors='replace') as f:
        for ln in f:
            ln = ln.strip()
            if ln.startswith('{'):
                try:
                    rows.append(json.loads(ln))
                except ValueError:
                    pass
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--slack-ms', type=int, default=700)
    a = ap.parse_args()
    trace = load(os.path.join(a.out, 'actions.jsonl'))
    play = load(os.path.join(a.out, 'play.jsonl'))
    names = {}
    try:
        sys.path.insert(0, os.path.dirname(__file__))
    except Exception:
        pass
    # press windows from the play log: every `watch <label>` sample
    win = collections.OrderedDict()
    for r in play:
        st = r.get('step', '')
        if st.startswith('watch ') or st.startswith('press '):
            lab = st.split(' ', 1)[1]
            # `press <key> <hold_ms> <label> (down)` / `(up)`: the key's own edges, so the window opens at the key-down
            # even when the first `watch` sample comes late (2026-10-20 parity4: the samples began 646 ms after the
            # Dodge key went down, the 233 ms Dodge macro was already over, and the row read `key only: []`)
            if st.endswith(' (down)') or st.endswith(' (up)'):
                lab = st.rsplit(' ', 2)[-2]
            lo, hi = win.get(lab, (r['epoch_ms'], r['epoch_ms']))
            win[lab] = (min(lo, r['epoch_ms']), max(hi, r['epoch_ms']))
    # the idle set: events that fire in most ticks of the context (e.g. the released-state combos)
    by_ctx = collections.defaultdict(collections.Counter)
    ticks = collections.Counter()
    for t in trace:
        ticks[t['ctx']] += 1
        for ev, _v in t.get('out', []):
            by_ctx[t['ctx']][ev] += 1
    idle = {c: {ev for ev, n in cnt.items() if n > 0.6 * ticks[c]} for c, cnt in by_ctx.items()}

    def fired(lab):
        lo, hi = win[lab]
        lo -= 400   # the harness records its first sample ~0.2 s after the key goes down
        hi += a.slack_ms
        s = collections.OrderedDict()
        for t in trace:
            if lo <= t['ms'] <= hi:
                for ev, v in t.get('out', []):
                    if ev in idle.get(t['ctx'], set()):
                        continue
                    s.setdefault(ev, set()).add(v)
        return s

    ok = True
    pairs = 0
    for lab in win:
        if not lab.startswith('key-'):
            continue
        base = lab[4:]
        pad = 'pad-' + base
        k = fired(lab)
        if pad not in win:
            print('%-16s key only: %s' % (base, sorted(k)))
            continue
        p = fired(pad)
        pairs += 1
        same = set(k) == set(p)
        ok = ok and same
        print('%-16s %s  key %s  pad %s' % (base, 'EQUAL ' if same else 'DIFFER', sorted(k), sorted(p)))
    print('%d pairs, %s' % (pairs, 'all equal' if ok else 'DIFFERENCES'))
    return 0 if ok and pairs else 1


if __name__ == '__main__':
    sys.exit(main())
