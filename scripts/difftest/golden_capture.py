#!/usr/bin/env python3
"""golden_capture.py -- build the differential-test GOLDEN (reference) set.

This is the FIRST HALF of the L6 differential CPU-test (see docs/DIFFTEST_DESIGN.md):
given a guest VA and a set of input register dicts, produce the reference register/
memory deltas in the canonical golden format. The reference oracle is Ghidra's
PowerPC emulator (`mcp__ghidra__emulate_function`) -- an independent ISA implementation
from our XenonRecomp lowering, which is what makes the diff meaningful.

ORACLE MODES
------------
The reference oracle is Ghidra's PowerPC emulator. As of iter-215 the server-side
`emulate_function` bug (`"Undefined register: ESP"` on non-x86) is FIXED, so the live
path works directly against the GhidraMCP HTTP server -- no agent round-trip needed:

  --emulate : LIVE. POST each case to the GhidraMCP HTTP endpoint (default
              127.0.0.1:8089) /emulate_function and write canonical goldens with
              oracle='ghidra-emulate'. The machine oracle, fully self-service.
  --emit    : print the exact `emulate_function` tool-call payloads (JSON) for each
              case, to run via the MCP tool by hand/agent (offline/no-HTTP fallback).
  --ingest  : read a saved responses file and write canonical golden/<fn>.golden.json.
  --hand    : pin a literal expected delta (computed by hand from the Ghidra decompile)
              as the golden -- a zero-infrastructure oracle, valid for the simple
              Tier-0 canary functions. Used to bring the recomp side up end-to-end.
              (Cross-check: --emulate goldens should MATCH the --hand goldens.)

INPUT: case files
-----------------
A case file `cases/<fn>.cases.json` is a JSON list of input cases:

  [
    { "case_id": "mul3_7x5", "regs": { "r3": "0x7", "r4": "0x5" },
      "expect": { "r3": "0x69" } },        // 'expect' only used by --hand
    { "case_id": "mul3_zero", "regs": { "r3": "0x0", "r4": "0x123" },
      "expect": { "r3": "0x0" } }
  ]

`regs` values are hex strings (or ints). Tier 1 adds an optional `mem` dict
{ "<VA hex>": "<hex bytes, big-endian>" }; this tool carries it through unchanged.

Usage:
  # 1. Emit the emulate_function payloads to run via the MCP tool:
  python golden_capture.py --va 0x82670774 --fn sub_82670774 --emit
  python golden_capture.py --cases cases/sub_82670774.cases.json --fn sub_82670774 --emit

  # 2. After running them and saving responses (see --emit output for format), ingest:
  python golden_capture.py --cases cases/...json --fn sub_82670774 \
         --ingest responses/sub_82670774.responses.json

  # 3. Or, no oracle -- pin the hand-computed 'expect' fields as golden:
  python golden_capture.py --cases cases/sub_82670774.cases.json --fn sub_82670774 --hand

Stdlib only. Output goes to golden/<fn>.golden.json (override with --out).
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CASES_DIR = os.path.join(HERE, 'cases')
GOLDEN_DIR = os.path.join(HERE, 'golden')

# Output register set we gate on by default (the ABI return registers). cr/xer/scratch
# are advisory only -- see DIFFTEST_DESIGN.md §4.
DEFAULT_RETURN_REGS = ['r3', 'r4']


def hexnorm(v, width=8):
    """Normalize an int or hex/dec string to a fixed-width lowercase 0x hex string."""
    if isinstance(v, str):
        v = int(v, 16) if v.lower().startswith('0x') else int(v, 0)
    v &= (1 << (width * 4)) - 1
    return '0x' + format(v, '0{}x'.format(width))


def load_cases(args):
    """Return a list of case dicts from --cases file or a single --va/--regs case."""
    if args.cases:
        with open(args.cases, encoding='utf-8') as fh:
            cases = json.load(fh)
        if not isinstance(cases, list):
            raise SystemExit(f'{args.cases}: expected a JSON list of cases')
        return cases
    if args.va:
        regs = json.loads(args.regs) if args.regs else {}
        return [{'case_id': args.case_id or 'case0', 'regs': regs,
                 'expect': json.loads(args.expect) if args.expect else {}}]
    raise SystemExit('provide --cases FILE or --va VA [--regs JSON]')


def fn_name(args, cases):
    if args.fn:
        return args.fn
    va = args.va
    if not va and cases:
        va = cases[0].get('va')
    if va:
        return 'sub_' + hexnorm(va)[2:].upper()
    raise SystemExit('cannot determine function name; pass --fn')


def guest_va(args, cases, case):
    return case.get('va') or args.va or ('0x' + fn_name(args, cases)[len('sub_'):])


def emit_payloads(args, cases):
    """Print the exact emulate_function tool-call payloads, one per case."""
    fn = fn_name(args, cases)
    ret = args.return_regs.split(',') if args.return_regs else DEFAULT_RETURN_REGS
    print('# Run each payload via the MCP tool: mcp__ghidra__emulate_function')
    print('# Save the responses as a JSON list (same order) for --ingest, e.g.:')
    print('#   [ { "case_id": "...", "result": <raw response str/obj> }, ... ]')
    print('# ---')
    out = []
    for case in cases:
        va = guest_va(args, cases, case)
        regs = {k: hexnorm(v) for k, v in case.get('regs', {}).items()}
        payload = {
            'tool': 'mcp__ghidra__emulate_function',
            'case_id': case.get('case_id', va),
            'arguments': {
                'address': hexnorm(va),
                'registers': regs,
                'memory': case.get('mem', {}),
                'return_registers': ','.join(ret),
            },
        }
        out.append(payload)
    json.dump(out, sys.stdout, indent=2)
    sys.stdout.write('\n')
    print(f'# fn={fn} cases={len(cases)} return_regs={",".join(ret)}', file=sys.stderr)
    return 0


def emulate_via_http(va, regs, mem, watch, return_regs, program, host, port, timeout=60):
    """LIVE call: POST one case to the GhidraMCP /emulate_function endpoint. Seeds input
    `mem` ({VA: hexbytes}) and reads back `watch` regions ([{addr,size}]) post-emulation.
    Returns (regs_dict, mem_out_dict, error_or_None). Stdlib http.client only."""
    import http.client
    # case `mem` is {VA: hexbytes}; the endpoint wants an array [{address, hex}].
    mem_arr = [{'address': hexnorm(k), 'hex': v} for k, v in (mem or {}).items()]
    body = {
        'address': hexnorm(va),
        'registers': {k: hexnorm(v) for k, v in (regs or {}).items()},
        'memory': mem_arr,
        'return_registers': ','.join(return_regs),
    }
    if watch:
        # size as a STRING: a JSON number arrives server-side as a double ("4.0") which the
        # endpoint's integer parse rejects; a string parses cleanly.
        body['read_memory'] = [{'address': hexnorm(w['addr']), 'size': str(int(w['size']))} for w in watch]
    if program:
        body['program'] = program
    try:
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
        conn.request('POST', '/emulate_function', body=json.dumps(body),
                     headers={'Content-Type': 'application/json'})
        resp = conn.getresponse()
        data = resp.read().decode('utf-8', 'replace')
        conn.close()
    except OSError as e:
        return {}, {}, f'HTTP error (is Ghidra+GhidraMCP running on {host}:{port}?): {e}'
    if resp.status != 200:
        return {}, {}, f'HTTP {resp.status}: {data[:160]}'
    regs_out, err = parse_emulate_result(data)
    mem_out = {}
    try:
        obj = json.loads(data)
        inner = obj.get('result', obj) if isinstance(obj, dict) else obj
        if isinstance(inner, str):
            inner = json.loads(inner)
        if isinstance(inner, dict) and isinstance(inner.get('memory'), dict):
            mem_out = inner['memory']
    except (ValueError, TypeError):
        pass
    return regs_out, mem_out, err


def _mem_delta(seed, readback):
    """Changed bytes in the read-back regions vs the input seed (unseeded bytes default 0),
    as {VA: hexbytes} maximal runs -- matching difftest's deltas.mem (compare.py flattens both
    to {addr:byte}, so run segmentation is cosmetic)."""
    def bytemap(d):
        m = {}
        for va, hx in (d or {}).items():
            a = int(va, 16)
            for i, b in enumerate(bytes.fromhex(hx)):
                m[a + i] = b
        return m
    before, after = bytemap(seed), bytemap(readback)
    changed = sorted(addr for addr, b in after.items() if before.get(addr, 0) != b)
    out, i = {}, 0
    while i < len(changed):
        start = changed[i]
        run = [after[start]]
        j = i + 1
        while j < len(changed) and changed[j] == changed[j - 1] + 1:
            run.append(after[changed[j]])
            j += 1
        out[hexnorm(start)] = bytes(run).hex()
        i = j
    return out


def emulate(args, cases):
    """LIVE machine oracle: emulate each case via the GhidraMCP HTTP endpoint and write
    canonical golden/<fn>.golden.json (oracle='ghidra-emulate')."""
    fn = fn_name(args, cases)
    ret = args.return_regs.split(',') if args.return_regs else DEFAULT_RETURN_REGS
    goldens = []
    errors = 0
    for case in cases:
        cid = case.get('case_id', guest_va(args, cases, case))
        va = guest_va(args, cases, case)
        watch = case.get('watch', [])
        regs, mem_out, err = emulate_via_http(va, case.get('regs', {}), case.get('mem', {}),
                                              watch, ret, args.program, args.ghidra_host, args.ghidra_port)
        if err:
            print(f'[WARN] {fn} case {cid}: {err}', file=sys.stderr)
            errors += 1
            continue
        in_regs = {k: hexnorm(v) for k, v in case.get('regs', {}).items()}
        out_regs = {k: hexnorm(v) for k, v in regs.items()}
        deltas = _delta(in_regs, out_regs)
        # Machine memory oracle (Tier-1): bytes the function changed in the watched regions,
        # vs the input seed -- mirrors difftest's deltas.mem. Empty when there is no `watch`.
        obj = _golden_obj(args, cases, case, in_regs, deltas, oracle='ghidra-emulate')
        if watch:
            obj['deltas']['mem'] = _mem_delta(case.get('mem', {}), mem_out)
        goldens.append(obj)
    return _write_golden(args, fn, goldens, errors)


def _delta(in_regs, out_regs):
    """Compute the golden delta with the SAME convention as difftest_main.cpp (line ~287):
    r3 is ALWAYS recorded (the canonical PPC return register, even when unchanged, e.g.
    0*r4*3 = 0); every other register only when it changed from the input. An input reg
    the case did not specify defaults to 0x0 (both the recomp and the emulator start from a
    zeroed context), so e.g. an untouched r4 reads 0->0 and is correctly NOT a change.
    Matching this exactly is what lets a --emulate golden be a drop-in oracle for compare.py."""
    zero = hexnorm(0)
    return {k: v for k, v in out_regs.items() if k == 'r3' or in_regs.get(k, zero) != v}


def parse_emulate_result(raw):
    """Coerce a saved emulate_function response into {regName: value} dict.

    Accepts: a dict already shaped {reg: val}; a {"result": "...json..."} envelope;
    a string of JSON; or a {"registers": {...}} / {"return_registers": {...}} shape.
    Returns (regs_dict, error_or_None).
    """
    if raw is None:
        return {}, 'empty response'
    if isinstance(raw, str):
        try:
            raw = json.loads(raw)
        except (ValueError, TypeError):
            return {}, f'unparseable response string: {raw[:80]!r}'
    if isinstance(raw, dict) and 'result' in raw and not _looks_like_regs(raw):
        return parse_emulate_result(raw['result'])
    if isinstance(raw, dict):
        if 'error' in raw:
            return {}, str(raw['error'])
        for key in ('return_registers', 'registers', 'regs'):
            if key in raw and isinstance(raw[key], dict):
                return raw[key], None
        if _looks_like_regs(raw):
            return raw, None
    return {}, f'unrecognized response shape: {str(raw)[:120]}'


def _looks_like_regs(d):
    if not isinstance(d, dict):
        return False
    return any(isinstance(k, str) and (k.startswith('r') or k.startswith('f') or k.startswith('v'))
               for k in d)


def ingest(args, cases):
    """Read saved responses and write canonical golden/<fn>.golden.json."""
    fn = fn_name(args, cases)
    with open(args.ingest, encoding='utf-8') as fh:
        responses = json.load(fh)
    # responses: list of {"case_id":..., "result":...} OR raw results in case order
    by_id = {}
    if isinstance(responses, list):
        for i, r in enumerate(responses):
            cid = (r.get('case_id') if isinstance(r, dict) else None) or (
                cases[i].get('case_id', f'case{i}') if i < len(cases) else f'case{i}')
            by_id[cid] = r.get('result', r) if isinstance(r, dict) else r
    elif isinstance(responses, dict):
        by_id = responses
    else:
        raise SystemExit('responses file must be a list or dict')

    goldens = []
    errors = 0
    for case in cases:
        cid = case.get('case_id', guest_va(args, cases, case))
        regs, err = parse_emulate_result(by_id.get(cid))
        if err:
            print(f'[WARN] {fn} case {cid}: {err}', file=sys.stderr)
            errors += 1
            continue
        in_regs = {k: hexnorm(v) for k, v in case.get('regs', {}).items()}
        out_regs = {k: hexnorm(v) for k, v in regs.items()}
        deltas = _delta(in_regs, out_regs)
        goldens.append(_golden_obj(args, cases, case, in_regs, deltas, oracle='ghidra-emulate'))
    return _write_golden(args, fn, goldens, errors)


def hand(args, cases):
    """Pin each case's 'expect' field as the golden (hand-derived oracle).

    A case may instead carry "expect_status": "faulted" (a NEGATIVE case: a non-pure
    function the difftest world cannot run safely). Its golden records the expected
    status rather than a register delta, so compare.py can prove the SEH guard reports
    it cleanly as faulted instead of crashing.

    Tier 1 (memory store) cases carry "expect_mem" -- the hand-derived expected memory
    delta { "<VA hex>": "<hex bytes, big-endian>" } -- and may legitimately have NO
    register delta (a pure store changes guest memory, not r3/r4). Such a case is valid
    with only "expect_mem"; _golden_obj already emits deltas.mem from case['expect_mem'].
    """
    fn = fn_name(args, cases)
    goldens = []
    errors = 0
    for case in cases:
        cid = case.get('case_id', guest_va(args, cases, case))
        in_regs = {k: hexnorm(v) for k, v in case.get('regs', {}).items()}
        status = case.get('expect_status')
        if status:
            goldens.append(_golden_obj(args, cases, case, in_regs, {}, oracle='hand',
                                       status=status))
            continue
        exp = case.get('expect')
        # A Tier-1 store case may have expect_mem but no register delta -- that's valid.
        if not exp and not case.get('expect_mem'):
            print(f'[WARN] {fn} case {cid}: no "expect"/"expect_mem"/"expect_status" '
                  f'for --hand', file=sys.stderr)
            errors += 1
            continue
        deltas = {k: hexnorm(v) for k, v in (exp or {}).items()}
        goldens.append(_golden_obj(args, cases, case, in_regs, deltas, oracle='hand'))
    return _write_golden(args, fn, goldens, errors)


def _golden_obj(args, cases, case, in_regs, delta_regs, oracle, status='ok'):
    obj = {
        'guest_va': hexnorm(guest_va(args, cases, case)),
        'fn': fn_name(args, cases),
        'case_id': case.get('case_id', guest_va(args, cases, case)),
        'inputs': {'regs': in_regs, 'mem': case.get('mem', {})},
        'oracle': oracle,
        'status': status,
    }
    if status == 'faulted':
        # Negative case: no register delta is expected; the verdict IS the fault.
        return obj
    obj['deltas'] = {'regs': delta_regs, 'mem': case.get('expect_mem', {})}
    return obj


def _write_golden(args, fn, goldens, errors):
    out = args.out or os.path.join(GOLDEN_DIR, f'{fn}.golden.json')
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8') as fh:
        json.dump(goldens, fh, indent=2)
        fh.write('\n')
    print(f'wrote {len(goldens)} golden(s) -> {out}'
          + (f' ({errors} case(s) skipped)' if errors else ''))
    return 1 if errors and not goldens else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cases', help='cases/<fn>.cases.json (list of input cases)')
    ap.add_argument('--va', help='single guest VA (alternative to --cases)')
    ap.add_argument('--regs', help='single-case input regs as JSON (with --va)')
    ap.add_argument('--expect', help='single-case expected delta as JSON (with --va, --hand)')
    ap.add_argument('--case-id', help='single-case id (with --va)')
    ap.add_argument('--fn', help='function name (default: derived from VA)')
    ap.add_argument('--return-regs', default=','.join(DEFAULT_RETURN_REGS),
                    help='comma-separated output registers to read back')
    ap.add_argument('--out', help='output golden path (default: golden/<fn>.golden.json)')
    ap.add_argument('--program', default='default.xex', help='Ghidra program name (for --emulate)')
    ap.add_argument('--ghidra-host', default='127.0.0.1', help='GhidraMCP HTTP host (--emulate)')
    ap.add_argument('--ghidra-port', type=int, default=8089, help='GhidraMCP HTTP port (--emulate)')
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument('--emulate', action='store_true',
                      help='LIVE: emulate each case via the GhidraMCP HTTP endpoint')
    mode.add_argument('--emit', action='store_true', help='print emulate_function payloads')
    mode.add_argument('--ingest', metavar='RESPONSES', help='ingest saved emulate responses')
    mode.add_argument('--hand', action='store_true', help='pin case "expect" fields as golden')
    args = ap.parse_args(argv)

    cases = load_cases(args)
    if args.emulate:
        return emulate(args, cases)
    if args.emit:
        return emit_payloads(args, cases)
    if args.ingest:
        return ingest(args, cases)
    if args.hand:
        return hand(args, cases)
    return 2


if __name__ == '__main__':
    sys.exit(main())
