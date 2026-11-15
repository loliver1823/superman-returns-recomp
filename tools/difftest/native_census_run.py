#!/usr/bin/env python3
"""The PM4 owner census (SR_NATIVE_BRACKETS=1) offline proof: segmentless gap parts attributed to their producers,
the stamp lifetime (moved with a native close's copy, ended when either XDK allocator hands the memory out again),
with compiled faults.

Run on the cloud worker (never on the desktop), from the synced worktree root:
    python tools/cloud/sr_cloud.py run --worktree . --name rcen python tools/difftest/native_census_run.py --out archive/rcen/census_matrix
then fetch the --out folder. CPU only: no game, harness, GPU or native_proof run. One build at a time (-j 4).

Matrix: the positive build in every mode (native, the residue gates on, SR_NATIVE_TRANSPORT, segmentless off, and the
census off on both transports); the census-off runs must print the same CP / guest digest as their census-on twins
(the census is observe-only). Each compiled fault (kernel/native_flush.cpp, kernel/native_brackets.cpp,
gfx/pm4_coverage.cpp) rebuilt and required to fail its named assertions in its mode without crashing, and to pass its
controls (the census off; the other transport where the fault's mechanism is not used); then every source restored
byte for byte, rebuilt and the positive modes rerun. No live, pacing or release acceptance is claimed by this matrix.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

SUMMARY = re.compile(r'^\[NATIVE_CENSUS_TEST\] (\d+) passed, (\d+) failed$', re.M)
DIGEST = re.compile(r'^\[NATIVE_CENSUS_TEST\] digest ([0-9A-F]{16})$', re.M)
RESIDUE_ON = {'SR_NATIVE_IDLE_WAIT': '1', 'SR_NATIVE_INVAL_RANGE': '1', 'SR_NATIVE_LOCK_COHER': '1'}

# mode tag -> (test argument, environment overrides)
MODES = {
    'native': ('native', {'SR_NATIVE_BRACKETS': '1'}),
    'residue': ('residue', dict(RESIDUE_ON, SR_NATIVE_BRACKETS='1')),
    'transport': ('native', {'SR_NATIVE_BRACKETS': '1', 'SR_NATIVE_TRANSPORT': '1'}),
    'segmentless_off': ('segmentless-off', {'SR_NATIVE_BRACKETS': '1', 'SR_NATIVE_SEGMENTLESS': '0'}),
    'gate_off': ('gate-off', {}),
    'gate_off_segmentless_off': ('gate-off', {'SR_NATIVE_SEGMENTLESS': '0'}),
}
# census on / off twins: the same digest (what the CP and the guest saw)
TWINS = (('native', 'gate_off'), ('transport', 'gate_off'), ('segmentless_off', 'gate_off_segmentless_off'))

P = 'producers: '
INVAL2 = P + 'producer INVAL: its two calls\' WAIT_UNTIL + COHER blocks (the nested one included)'
LOCK = P + 'VertexBuffer_Lock: its COHER block, as owner and producer'
TEX = P + 'texture lock entry: its core\'s COHER block'
IDLE = P + 'producer BlockUntilIdle: its WAIT_UNTIL'
EMIT_INNER = P + 'producer EmitPm4Packet scope: its own PM4 only (not the nested INVAL\'s)'
UNST_ONLY = P + 'segmentless: UNSTAMPED holds the unbracketed PM4 only'
MOVED = P + 'moved: the closed ranges hold no stamp (moved with the copies; the closes\' own bytes never restamped)'
SWITCH_MOVED = ('switch-tail: moved: the closed allocation holds no stamp (no later cut - the switch\'s, the ring '
                'write\'s - restamps it)')
SEG_ENDED = 'seg-reuse: BeginNewSegment\'s hand-out ended the previous lap\'s stamps'
SEG_STALE = 'seg-reuse: no stale owner: the unbracketed PM4 in the reused segment reads UNSTAMPED'
BLK_ENDED = 'block-reuse: GpuCmdBufAllocate\'s hand-out ended the previous lap\'s stamps'
BLK_STALE = 'block-reuse: no stale owner: the block\'s PM4 reads UNSTAMPED'
LAG = 'lagging: the copy keeps the stamps of its own write: INVAL\'s packets INVAL\'s'
BS = 'block-scopes: '
BS_SCOPE = BS + 'a block handed out inside a scope is that scope\'s (owner and producer), nothing else attributed'
BS_REUSE = BS + 'reused outside any bracket: UNSTAMPED, not the previous allocator\'s'
BS_OTHER = BS + 'reused inside another scope: that scope\'s'
BS_KIND = BS + 'the stamp is the block\'s (kind block, the allocating scope as owner and producer, not 820FCDF0)'
ZK = 'zpass-kick: '
ZK_ATTR = ZK + 'the kick blocks are attributed: every INTERRUPT EndZPass\'s (the blocks born with their allocating scope)'
ZK_NODE = ZK + ('the 16-byte list node is marked unowned (chain), not stamped by the switch\'s cut or the flush\'s '
                'leave')
ZK_CHAIN = ZK + 'packets read from the node are UNSTAMPED and counted in chain= (an unstamped total reconciles)'
FOREIGN = ('foreign: a non-owner thread\'s bracket stamps nothing: its window\'s PM4 UNSTAMPED, the owner\'s scope\'s '
           'its own')
ZO = 'zero-owner: '
ZO_BLOCK = ZO + 'a block handed out with the device released is not stamped: its PM4 UNSTAMPED'
ZO_PIECE = ZO + 'the released device\'s and the unknown identity\'s pieces are not stamped: UNSTAMPED'
TR = 'transition: '
TR_B = TR + 'B\'s PM4 written while B owned the device is not this bracket\'s: UNSTAMPED'
TR_CLOSE = TR + 'owned at the closing cut only: the piece spanning B\'s writes is dropped (UNSTAMPED)'
FA_B = 'foreign-alloc: a block thread B (not the device\'s owner) allocates inside its scope is not stamped: UNSTAMPED'
PD1 = 'pump-driver: lap 1 (the device the owner thread\'s): '
PD2 = 'pump-driver: lap 2 (the device released, dev+0x2A08 = 0): '
PD_RING = 'the pump\'s ring writes are the driver\'s (owner 82110840, thread B), whoever owns the device'
PD_UNST = 'the owner thread\'s PM4 written inside the driver\'s bracket is not the driver\'s: UNSTAMPED'
IL_B = ('interleave: both cuts read this thread, the epoch moved between them: B\'s PM4 is not this bracket\'s '
        '(UNSTAMPED)')
RG_B = ('read-gap: the cut\'s piece holding B\'s PM4 is dropped whole (UNSTAMPED), the piece after the take-back its '
        'own')
RO = 'raw-owner: '
RO_KEEP = RO + 'B\'s stamped PM4 stays B\'s: another thread\'s piece never stamps over it'
RO_REFUSE = RO + 'the piece that would overlap B\'s stamp is refused whole (this thread\'s PM4 in it UNSTAMPED)'
SF = 'swap-frames: '
SF_PACKET = SF + 'an XE_SWAP packet: one frame (at the packet), its present not a second (census off: none, the same present)'
SF_LIVE = SF + 'a kRkSwap record (live executor, no 0x64 packet): one frame, at its present (census off: none)'
SF_ORACLE = SF + 'a kRkSwap record (the oracle loop): one frame, at its present (census off: none)'
SF_RENDERED = SF + 'the record rendered to PM4 (CPPM4): the XE_SWAP packet\'s one frame, not two (census off: none)'
SF_UNSIGNED = SF + ('an unsigned XE_SWAP: one frame, no present; the record after it still ends its own frame (census '
                    'off: none)')
SF_TOTAL = SF + 'frames = every present the CP applied + the unsigned XE_SWAP\'s (6 = 5 + 1)'
TR = 'terminal-render: '
TR_FOLD = TR + 'the snapshot folds the drained partial frame in (its tail + F line), the re-render folds nothing'
TR_VERIFY = TR + ('the verify\'s re-render prints every line with its snapshot fields (quit::snapshotDiff: 0 moved), '
                  'the armed witness in both')

# name, file, anchor, replacement, the mode it must fail in, assertions that must fail, control modes that must pass.
MUTATIONS = (
    # (a) the copy window: a segmentless gap part executed without its guest address and stamps
    ('copy_window_off', 'kernel/native_flush.cpp',
     b'  if (sr::gfx::pm4cov::on())\n    sr::gfx::pm4cov::setCopyWindow(',
     b'  if (false)\n    sr::gfx::pm4cov::setCopyWindow(', 'native',
     {INVAL2, LOCK, TEX, IDLE, UNST_ONLY, LAG}, ('gate_off', 'segmentless_off')),
    # (a) the copy's stamp key: the host copy's own address instead of its guest origin
    ('copy_key', 'gfx/pm4_coverage.cpp',
     b'    const uint32_t off = w.phys + uint32_t(at - w.host);',
     b'    const uint32_t off = hostOffOf(at);', 'native',
     {INVAL2, LOCK, TEX, IDLE, UNST_ONLY, LAG}, ('gate_off', 'segmentless_off')),
    # (a) the copy's owners from the map when it runs (not the stamps the close moved into it): a lagging CP
    ('copy_from_map', 'gfx/pm4_coverage.cpp',
     b'    for (size_t i = 0; i < g_copyN && w.n < w.iv.size(); i++)\n'
     b'      if (g_copyIv[i].hi > lo && g_copyIv[i].lo < hi) w.iv[w.n++] = g_copyIv[i];',
     b'    w.n = sr::native::snapshotIntervals(lo, hi, w.iv.data(), w.iv.size());', 'native',
     {INVAL2, LOCK, LAG}, ('gate_off', 'segmentless_off')),
    # (a) the producer split: no nested scope recorded (Stamp::inner = the outermost entry)
    ('inner_off', 'kernel/native_brackets.cpp',
     b'    const bool inner = !outer && t.innerTop >= 0 && t.innerTop < 31;',
     b'    const bool inner = false;', 'native',
     {INVAL2, EMIT_INNER}, ('gate_off',)),
    # (b) moved: the close copies the stamps but leaves them in the map
    ('take_copies', 'kernel/native_brackets.cpp',
     b'  eraseLocked(lo, hi, &out);\n  g_census.moved',
     b'  { auto keep = g_cmd; eraseLocked(lo, hi, &out); g_cmd.swap(keep); }\n  g_census.moved', 'native',
     {MOVED, SWITCH_MOVED}, ('gate_off', 'segmentless_off')),
    # (b) moved: the next cut (the ring write's, the switch's, the flush's leave) stamps the fence record the close's
    # span wrote inside the closed range again
    ('restart_close', 'kernel/native_flush.cpp',
     b'    sr::native::restartPiece(be32(d + 40));\n    after = publishSegment(',
     b'    after = publishSegment(', 'native',
     {MOVED, SWITCH_MOVED}, ('gate_off', 'segmentless_off')),
    # (b) moved: the same-allocation tail lets the flush's leave stamp the alignment pad past the closed range
    ('restart_tail', 'kernel/native_flush.cpp',
     b'    put32(d + 40, v - 4u);\n    sr::native::restartPiece(v - 4u);',
     b'    put32(d + 40, v - 4u);', 'native',
     {MOVED}, ('gate_off', 'segmentless_off')),
    # (b) ended: BeginNewSegment's hand-out keeps the previous lap's stamps (the ring path keeps them in place)
    ('erase_new_segment', 'kernel/native_brackets.cpp',
     b'  if (g_bracketsOn && e.kind == kSwitch) eraseNewSegment(t);\n', b'', 'segmentless_off',
     {SEG_ENDED, SEG_STALE}, ('gate_off_segmentless_off', 'native')),
    # (b) ended: GpuCmdBufAllocate's hand-out keeps them (the ring path's previous lap; a block's previous allocator)
    ('erase_alloc', 'kernel/native_brackets.cpp',
     b'  if (eraseLocked(lo, lo + bytes, nullptr)) g_census.allocErase',
     b'  if (false && eraseLocked(lo, lo + bytes, nullptr)) g_census.allocErase', 'segmentless_off',
     {BLK_ENDED, BLK_STALE, BS_REUSE}, ('gate_off_segmentless_off', 'gate_off')),
    # (b) born: no block stamped at its 820FCDF0 hand-out (the kick blocks read UNSTAMPED)
    ('block_stamp_off', 'kernel/native_brackets.cpp',
     b'  int top = t.open && ownsDevice(t) ? t.innerTop : -1;',
     b'  int top = -1;', 'native',
     {BS_SCOPE, BS_OTHER, BS_KIND, ZK_ATTR}, ('gate_off', 'gate_off_segmentless_off')),
    # (b) born: a block stamped without the device positively the allocating thread's (no ownership test at the
    # hand-out: thread B's block, the released device's block)
    ('foreign_block', 'kernel/native_brackets.cpp',
     b'  int top = t.open && ownsDevice(t) ? t.innerTop : -1;',
     b'  int top = t.open ? t.innerTop : -1;', 'native',
     {FA_B, ZO_BLOCK}, ('gate_off', 'gate_off_segmentless_off')),
    # (b) born: the block's producer taken as 820FCDF0 itself, not the scope that allocated it
    ('block_stamp_self', 'kernel/native_brackets.cpp',
     b'  if (top >= 0 && t.innerStack[top] == g_allocRow) top--;',
     b'', 'native',
     {BS_SCOPE, BS_OTHER, BS_KIND, ZK_ATTR}, ('gate_off', 'gate_off_segmentless_off')),
    # (b) born: pieces cut without the ownership test (a non-owner thread's bracket, the pump driver's on thread B,
    # one spanning another owner's writes, the released device's)
    ('foreign_cut', 'kernel/native_brackets.cpp',
     b'  const bool insert = both && epoch == t.epoch;',
     b'  const bool insert = true;', 'native',
     {FOREIGN, ZO_PIECE, TR_B, TR_CLOSE, IL_B, RG_B, PD1 + PD_UNST, PD2 + PD_UNST},
     ('gate_off', 'gate_off_segmentless_off')),
    # (b) born: ownership tested at the closing cut only (a bracket opened while another thread owned the device keeps
    # the piece spanning that owner's writes)
    ('owner_last_cut_only', 'kernel/native_brackets.cpp',
     b'  const bool both = owned && t.owned;',
     b'  const bool both = owned;', 'native',
     {TR_CLOSE}, ('gate_off', 'gate_off_segmentless_off')),
    # (b) born: ownership tested at the two cuts only, not the transitions between them (no epoch: A -> B -> A between
    # two cuts that both read A keeps B's bytes)
    ('endpoint_only', 'kernel/native_brackets.cpp',
     b'  const bool insert = both && epoch == t.epoch;',
     b'  const bool insert = both;', 'native',
     {IL_B}, ('gate_off', 'gate_off_segmentless_off')),
    # (b) born: a cut's ownership taken from its owner read alone (a takeover between that read and the cursor read
    # keeps the new owner's bytes)
    ('read_gap', 'kernel/native_brackets.cpp',
     b'  *owned = own0 && e1 == e0 && ownsDevice(t);',
     b'  *owned = own0;', 'native',
     {RG_B}, ('gate_off', 'gate_off_segmentless_off')),
    # (b) born: a piece stamped over another thread's stamp (an owner change no scope saw)
    ('overwrite_other', 'kernel/native_brackets.cpp',
     b'      if (it->second.s.tid != s.tid) return false;',
     b'      if (false) return false;', 'native',
     {RO_KEEP, RO_REFUSE}, ('gate_off', 'gate_off_segmentless_off')),
    # (b) born: the released device (dev+0x2A08 = 0) read as owned (the pump driver's bracket on thread B, this thread's
    # pieces and block)
    ('owner_zero_owned', 'kernel/native_brackets.cpp',
     b'  return owner == t.myId;',
     b'  return !owner || owner == t.myId;', 'native',
     {ZO_BLOCK, ZO_PIECE, PD2 + PD_UNST}, ('gate_off', 'gate_off_segmentless_off')),
    # (b) born: an unknown thread identity (KTHREAD+332 = 0) matching the released device's 0
    ('unknown_id_owned', 'kernel/native_brackets.cpp',
     b'  if (!t.dev || !t.myId || !readable4(t.dev + 0x2A08u)) return false;',
     b'  if (!t.dev || !readable4(t.dev + 0x2A08u)) return false;', 'native',
     {ZO_PIECE}, ('gate_off', 'gate_off_segmentless_off')),
    # (4) the pump driver 82110840 without its N0 bracket: its ring writes become 820FD218's own
    ('no_driver_scope', 'kernel/native_pump.cpp',
     b'  SR_NATIVE_SCOPE(0x82110840);  // NATIVE N0 (scripts/gen_native_brackets.py): the pump driver\'s table row\n',
     b'', 'native',
     {PD1 + PD_RING, PD2 + PD_RING}, ('gate_off', 'gate_off_segmentless_off')),
    # (3) the chained close's list node left unmarked (its bytes not counted apart)
    ('chain_mark_off', 'kernel/native_flush.cpp',
     b'      sr::native::markChain(cursorEnd, 16u);\n', b'', 'native',
     {ZK_NODE, ZK_CHAIN}, ('gate_off', 'segmentless_off')),
    # (3) moved: the BeginNewSegment tail of a chained close lets the switch's cut stamp the list node
    ('restart_switch', 'kernel/native_flush.cpp',
     b'    put32(d + 40, after - 4u);\n    sr::native::restartPiece(after - 4u);',
     b'    put32(d + 40, after - 4u);', 'native',
     {ZK_NODE, ZK_CHAIN}, ('gate_off', 'segmentless_off')),
    # (5) the frame boundary (native/composed-census): tcmp2's census1 defect itself - a present the CP applies with no
    # XE_SWAP packet (the composed env's kRkSwap record) ends no census frame, so the census never reports
    ('present_end_off', 'gfx/pm4_engine.cpp',
     b'  if (pm4cov::on()) pm4cov::presented();\n', b'', 'native',
     {SF_LIVE, SF_ORACLE, SF_UNSIGNED, SF_TOTAL}, ('gate_off', 'gate_off_segmentless_off')),
    # (5) every present ends a frame, the XE_SWAP packet's too: the packet's frame counted twice
    ('present_end_double', 'gfx/pm4_coverage.cpp',
     b'  if (g_swapPacketEnded) {  // the XE_SWAP packet\'s own present: packet3 ended its frame',
     b'  if (false) {  // the XE_SWAP packet\'s own present: packet3 ended its frame', 'native',
     {SF_PACKET, SF_RENDERED, SF_TOTAL}, ('gate_off', 'gate_off_segmentless_off')),
    # (5) an XE_SWAP that applies no present leaves its frame end pending: the next record's present is swallowed
    ('unsigned_swallows', 'gfx/pm4_engine.cpp',
     b'        if (pm4cov::on()) pm4cov::swapWithoutPresent();\n', b'', 'native',
     {SF_UNSIGNED, SF_TOTAL}, ('gate_off', 'gate_off_segmentless_off')),
    # (6) native/census-quit (tbk2's defect): the armed witness printed from the unfolded state - before the drained
    # partial frame is folded in, as tbk2's exe did - reads frames=N in the snapshot and N+1 in the verify's re-render
    ('terminal_before_fold', 'gfx/pm4_coverage.cpp',
     b'    tail = 0;\n    if (anyCount(g_cur)) {',
     b'    SR_LOG_BOOT(sr::LogSeverity::Info, "[PM4_COV] armed %s brackets=%d frames=%llu presents=%llu", why,\n'
     b'                sr::native::g_bracketsOn ? 1 : 0, (unsigned long long)g_frames, (unsigned long long)g_presents);\n'
     b'    tail = 0;\n    if (anyCount(g_cur)) {', 'native',
     {TR_VERIFY}, ('gate_off', 'gate_off_segmentless_off')),
)

TRACKED = ('kernel/native_flush.cpp', 'kernel/native_brackets.cpp', 'kernel/native_brackets.hpp',
           'kernel/native_brackets.gen.cpp', 'kernel/native_pump.cpp', 'gfx/pm4_coverage.cpp', 'gfx/pm4_coverage.hpp',
           'gfx/pm4_engine.cpp',
           'tools/difftest/native_census_test.inc', 'tools/difftest/native_residue_test.inc',
           'tools/difftest/suite_bare.inc', 'tools/difftest/native_census_run.py', 'tools/difftest/difftest_main.cpp',
           'scripts/gen_native_brackets.py')


def parse(raw):
    text = raw.decode(errors='replace').replace('\r\n', '\n')
    lines = text.splitlines()
    good = [line[6:] for line in lines if line.startswith('  ok  ')]
    bad = [line[6:] for line in lines if line.startswith('FAIL: ')]
    return good, bad, SUMMARY.findall(text), DIGEST.findall(text)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--only', default='', help='comma-separated mutation names (default: all)')
    parser.add_argument('--modes-only', action='store_true', help='the positive modes only (no mutation)')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = (root / args.out).resolve() if not args.out.is_absolute() else args.out
    out.mkdir(parents=True, exist_ok=False)
    originals = {p: (root / p).read_bytes() for p in TRACKED}
    result = dict(scope='Offline CPU proof of the PM4 owner census (SR_NATIVE_BRACKETS=1): segmentless gap parts '
                  'attributed to their producers (the copy window, the producer split), the stamp lifetime (moved '
                  'with a native close, ended at 820FCF90 / 820FCDF0 hand-outs), the census observe-only (digests). '
                  'No live, pacing or release acceptance.',
                  source_sha256={p: sha(b) for p, b in originals.items()}, modes={}, twins={}, mutations=[])
    only = {x for x in args.only.split(',') if x}
    selected = [] if args.modes_only else [m for m in MUTATIONS if not only or m[0] in only]

    def save():
        (out / 'census_matrix.json').write_text(json.dumps(result, indent=2) + '\n')

    base_env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    base_env['PATH'] = r'C:\Program Files\LLVM\bin;' + base_env.get('PATH', '')
    base_env.update(SR_PM4_RING='0', SR_PM4_RING_EXEC='0', SR_PM4_CP='0', SR_ALLOW_FORCING='0',
                    SR_NATIVE_SEGMENTLESS='1', SR_NATIVE_FLUSH='1', SR_NATIVE_IDLE_WAIT='0',
                    SR_NATIVE_INVAL_RANGE='0', SR_NATIVE_LOCK_COHER='0', SR_NATIVE_BRACKET='0',
                    SR_NATIVE_TRANSPORT='0', SR_NATIVE_BRACKETS='0',
                    # the suite's configured marker (tools/difftest/suite_bare.inc): an inert A/B name, no relaunch
                    SR_AB_TOGGLE='SR_NATIVE_BRACKETS')

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
        # The shared worker's sccache server sometimes drops a connection (os error 10054): retried twice, recorded.
        for attempt in range(3):
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
        arg, extra = MODES[mode]
        env = dict(base_env)
        env.update(extra)
        code, raw = run(tag, [root / 'build/difftest.exe', '--native-census-test', arg], env, 900)
        good, bad, summary, digest = parse(raw)
        row = dict(mode=mode, exit=code, passed=len(good), failed=len(bad), summary=summary, digest=digest,
                   stdout_sha256=sha(raw), failures=bad)
        return row, good, bad, summary

    def positive(tag, mode):
        row, good, bad, summary = test(tag, mode)
        row['valid'] = bool(row['exit'] == 0 and not bad and good and summary == [(str(len(good)), '0')]
                            and len(set(good)) == len(good) and len(row['digest']) == 1)
        print(json.dumps({'test': tag, 'exit': row['exit'], 'summary': summary, 'digest': row['digest'],
                          'valid': row['valid']}), flush=True)
        return row

    def twins(rows):
        out_ = {}
        for a, b in TWINS:
            da, db = rows[a].get('digest'), rows[b].get('digest')
            out_[f'{a}={b}'] = bool(da and da == db)
        return out_

    try:
        result['positive_build'] = build('positive_build')
        for mode in MODES:
            result['modes'][mode] = positive('positive_' + mode, mode)
            save()
        result['twins'] = twins(result['modes'])
        print(json.dumps({'twins': result['twins']}), flush=True)
        if not all(r['valid'] for r in result['modes'].values()) or not all(result['twins'].values()):
            raise RuntimeError('positive matrix invalid; mutations not attempted')
        for name, path, before, after, mode, required, controls in selected:
            source = root / path
            original = originals[path]
            if b'\r\n' in original:  # the worker's checkout may carry CRLF line ends
                before, after = before.replace(b'\n', b'\r\n'), after.replace(b'\n', b'\r\n')
            if original.count(before) != 1:
                raise RuntimeError(f'{name}: production anchor changed; refusing mutation')
            if any((root / p).read_bytes() != b for p, b in originals.items()):
                raise RuntimeError('concurrent source edit; refusing mutation')
            changed = original.replace(before, after)
            mrow = dict(name=name, file=path, mode=mode, required=sorted(required), controls=list(controls),
                        mutated_sha256=sha(changed))
            result['mutations'].append(mrow)
            source.write_bytes(changed)
            try:
                mrow['build'] = build(name + '_build')
                trow, good, bad, summary = test(name + '_test', mode)
                missing = sorted(required - set(bad))
                trow['missing_required'] = missing
                # A detected defect: the suite ran to its summary (or its watchdog's), exited 1, every named assertion
                # failed.
                trow['valid'] = (trow['exit'] == 1 and summary == [(str(len(good)), str(len(bad)))] and not missing)
                mrow['test'] = trow
                mrow['controls_run'] = {c: positive(f'{name}_{c}', c) for c in controls}
                mrow['detected'] = bool(trow['valid'] and all(r['valid'] for r in mrow['controls_run'].values()))
                print(json.dumps({'mutation': name, 'failed': len(bad), 'missing': missing,
                                  'detected': mrow['detected']}), flush=True)
            finally:
                source.write_bytes(original)
                mrow['source_restored'] = source.read_bytes() == original
                save()
    finally:
        result['valid'] = False
        for p, b in originals.items():
            if (root / p).read_bytes() != b:
                (root / p).write_bytes(b)
        result['all_sources_restored'] = all(sha((root / p).read_bytes()) == v for p, v in result['source_sha256'].items())
        try:
            if result['all_sources_restored'] and result['mutations']:
                result['restored_build'] = build('restored_build')
                result['restored'] = {m: positive('restored_' + m, m) for m in MODES}
                result['restored_twins'] = twins(result['restored'])
        finally:
            result['valid'] = bool(
                result['all_sources_restored'] and result['modes'] and all(r['valid'] for r in result['modes'].values())
                and result['twins'] and all(result['twins'].values())
                and len(result['mutations']) == len(selected)
                and all(m.get('detected') and m.get('source_restored') for m in result['mutations'])
                and (not selected or (result.get('restored') and all(r['valid'] for r in result['restored'].values())
                                      and all(result.get('restored_twins', {}).values()))))
            save()
            print(json.dumps({'valid': result['valid']}), flush=True)
    return 0 if result['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
