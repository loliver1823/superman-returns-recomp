"""The strict flicker gate's span-detector RULE PARAMETERS - one table (Astra f5e06465 r3 P1; the coordinator, 2026-10-22).

flicker_diff.py takes its argument defaults and its identity from CANONICAL, and flicker_gate.py's fresh span run passes
exactly canonical_args() - so the gate's parameters and the canonical identity cannot drift apart. An output made with
any other setting carries a NON-canonical identity (flicker_diff.identity_of), which --reuse re-runs and strict_fg fails on
an id mismatch. No dependencies: flicker_gate imports this without numpy / Pillow.
"""

# block size (px at 320 x 180), the A-B-A threshold and ratio, the whole-frame motion fraction, the presents that FAIL the
# detector's own verdict - Fable 26 / 28's frozen values
CANONICAL = {"block": 20, "thr": 6.0, "ratio": 0.35, "motion": 0.25, "fail_presents": 2}
FLAGS = {"block": "--block", "thr": "--thr", "ratio": "--ratio", "motion": "--motion", "fail_presents": "--fail-presents"}


def canonical_args():
    """flicker_diff.py's command-line arguments for the canonical rule (the motion check on: no --no-motion-check)."""
    out = []
    for k in sorted(CANONICAL):
        out += [FLAGS[k], repr(CANONICAL[k])]
    return out


def params_record(values, motion_check):
    """The effective parameters as flicker_diff prints them (FLICKER PARAMS) and flicker_gate records them (span.params)."""
    rec = {k: values[k] for k in sorted(CANONICAL)}
    rec["motion_check"] = bool(motion_check)
    return rec
