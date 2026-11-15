#!/usr/bin/env python3
"""p2.zpd offline proof (native/zpd): `difftest --native-zpd-test`, then every Astra round 1 fix mutation-validated.

Run on the cloud worker (never on the desktop), from the synced worktree root:
    python tools/cloud/sr_cloud.py run --worktree . --name zpd python tools/difftest/native_zpd_run.py --out archive/zpd/mutants
then fetch the --out folder. CPU only: no game, harness, GPU or native_proof run. One build at a time.

The suite (tools/difftest/native_zpd_test.cpp) drives the generated D3DQuery Issue / GetData bodies, the Pm4Engine
executing the guest's own PM4, null_renderer's production cpZpdFence and D3D12Backend's query paths (its SR_DIFFTEST
seam stands in for the device the worker lacks), and (Z16) the HUD query's point draw on a WARP device. Each fault
below puts back one defect Astra's round 1 found, the zpd3 log-tag loss, or a fault in the zpd4 visibility probe /
point-sprite radius (or removes the fix's protection): rebuilt, the suite must exit 1 with its named assertions among
the failures, the gate-off control (Z0) and the publication checker's own control still passing; then every source is
restored byte for byte, rebuilt and the suite rerun clean.

p2.zpd's bake (native/zpd-bake, 2026-10-16; BAKE_MUTATIONS): SR_NATIVE_ZPD default ON in the latency model - the default
not baked / baked strict, =0 not off, an explicit =2 announced as the default (Z20 / Z21), the producer port's gate not
baked or ignoring the mode (Z21 / Z23), the record engine's EVENT_WRITE_ZPD not native, the release consumer or the
oracle without a kRkZpd case (Z22). The port itself against the SDK body: tools/difftest/zpd_port_run.py.
Astra round 1 on native/zpd-bake (P2-2): the zpd census at each sim-freeze hold's edges (kernel/highfps freezeEdgeCensus)
dropped at the end or the begin edge, or its edge word stuck (Z24).
Astra round 2 on native/zpd-bake (P2-1): the frame stamp (gfx/native_zpd FrameStamp, Z25) - a record counted as the
literal packet's form (release consumer / oracle), the forms swapped, BEGINs dropped, a take that does not zero, counting
with the stamp off, the presenting stamp shared across threads, the dump line without the stamp or with a stale one.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

SUMMARY = re.compile(r'^\[NATIVE_ZPD_TEST\] (\d+) passed, (\d+) failed$', re.M)

PE = 'gfx/pm4_engine.cpp'
RE = 'gfx/record_engine.cpp'  # dev's stage 2a split: applyEventWriteShd / onShaderLoad live in the record engine
NR = 'gfx/null_renderer.cpp'
NZ = 'gfx/native_zpd.cpp'
BE = 'gfx/d3d12_frame_render.cpp'

# The assertions (tools/difftest/native_zpd_test.cpp check names) the faults must fail.
Z4_ORDER = ('Z4: the store\'s covering fence (production cpZpdFence -> pm4ZpdFlushPartial) renders the partial frame '
            'and writes the slot BEFORE the fence word moves')
Z4_WORDS = 'Z4: slot words: END {n, S, S, S, n, 0, 0, 0}, BEGIN {0, S, S, S, 0, 0, 0, 0}'
Z4_COUNT = 'Z4: GetData through the wrapper -> the 4X count (host * 4 / area)'
Z4_CONSUMED = 'Z4: the partial frame consumed the set'
Z4B_EMPTY = 'Z4b: the swap after the fence renders an empty set (the partial\'s batches are not rendered again)'
Z4C_ORDER = ('Z4c: the recorded order is helper, BeginQuery, EndQuery + resolve, helper (nothing at the BEGIN '
             'marker)')
Z4C_ADJ = 'Z4c: every BeginQuery is immediately followed by its EndQuery: no helper pass inside a query'
Z9_STEPS = 'Z9: GetData called after every one of the slot write\'s 16 stores: S_FALSE or exactly {n, 0, 0, n}'
Z9_LAST = 'Z9: S_FALSE until the last store (END+0), S_OK after it'
Z9_MODEL = 'Z9: every two-phase interleaving of GetData\'s loads with the stores reads S_FALSE or {n,0,0,n}'
Z10 = 'Z10: a 1X and a 2X draw of 9 host samples each -> (9*1 + 9*2) / area guest samples (not 18 / area)'
Z11_ORDER = ('Z11: the executor\'s EVENT_WRITE_SHD item waits for the render thread\'s partial frame: the slot is '
             'written before the fence word moves')
Z11_AFTER = 'Z11: then the store lands; GetData -> the count; the CP waited out the GPU time (no fallback)'
Z12 = 'Z12: the CP\'s EVENT_WRITE_SHD waits for the render thread\'s partial frame, then stores; GetData -> the count'
Z13_FALLBACK = ('Z13: the partial frame timed out: before the store the slot got the strict fallback (its last count), '
                'GetData -> that count, never the sentinel after the fence')
Z13_NOBE = 'Z13: strict with no backend attached: the fallback 1 before the store'
Z14_CENSUS = 'Z14: SR_LOG=player (every live roll) keeps the [NATIVE_ZPD] census line (a kMilestoneTags tag)'
Z14_QUERY = 'Z14: SR_LOG=player keeps the [NATIVE_ZPD_Q] query lines (a written query, a strict fallback)'
Z15_ORDER = ('Z15: the probe draws follow the measured draw\'s EndQuery, each on its own segment (ref, then raster), '
             'inside the query and before the helper pass')
Z15_NO_LEAK = 'Z15: the probe counts never reach the answer (ref 6, raster 6, the measured draw 0 -> GetData 0, segs 1)'
Z15_DEPTH = 'Z15: answer 0 with ref > 0 -> depthKilled (the depth / stencil test removed every sample)'
Z15_PS = 'Z15: answer 0, ref 0, raster > 0 -> psKilled (the pixel shader discarded every sample)'
Z15_TWO = ('Z15: two draws in one query: one probe of each kind (the first draw\'s), both draws measured '
           '(18 / area)')
Z15_SLOTS = 'Z15: one probe of each kind per query; the probes leave all kMaxSegs answer segments to the draws'
Z16_BEHIND = ('Z16: the point behind the stored depth: 0 samples, the slot 0; the ref probe (depth ALWAYS) still '
              'counts it -> depthKilled')
Z16_TWO_PX = 'Z16: a 2-pixel point (0x00100010) on a pixel corner covers 2x2 samples (the radius math of c19)'
Z16_PS = 'Z16: a pixel shader that discards: 0 and ref 0, the raster probe (no pixel shader) 1 -> psKilled'
ED = 'gfx/edram.hpp'
LOG = 'app/log.cpp'
ZPD_TAGS = '    "NATIVE_ZPD", "NATIVE_ZPD_Q",'
TRACE_TAGS = '    "NATIVE_ZPD_T", "PM4_LIVE_TRACE",'
Z17_WRITTEN = ('Z17: a traced query logs [NATIVE_ZPD_T] swap= id= slot= written guest= host= (past the 64 query lines), '
               'the occluded one guest=0')
Z17_FALLBACK = 'Z17: strict\'s stand-in for a traced query logs fallback guest='
Z17_UNTRACED = 'Z17: an untraced query logs no [NATIVE_ZPD_T] line'
Z18_REARM = 'Z18: B\'s BEGIN re-arms the slot A\'s late count overwrote after B\'s Issue (the sentinel is back)'
Z18_GETDATA = 'Z18: B\'s fence passed, B unresolved: GetData -> -1 (S_FALSE), never A\'s count'
LT = 'gfx/pm4_live_trace.cpp'
Z19_SWAP = 'Z19: tracedSwap is the trace\'s first frame for every frame of it (3, 3), 0 once it is written'
Z19_EVENTS = ('Z19: three frames in one trace: a swap event after each frame\'s swap packet (Xenia\'s writer puts it '
              'there), the last ends the file')
Z19_HOST = ('Z19: packets executed from host copies (3 swaps, 2 ring writes) are recorded at kHostPacketPhys, guest '
            'packets at their own physical address')
Z19_ZERO = ('Z19: nothing is recorded at physical 0 (IM_LOAD_IMMEDIATE\'s inline ucode is no read) and no read reaches '
            'the host packets\' address')
Z19_LOOSE = ('Z19: each frame\'s loose reads come first in that frame (frame 4\'s rewritten page after frame 3\'s swap '
             'event, not before frame 3\'s packets)')
Z19_PACKETS = 'Z19: every packet recorded once (8 PacketStarts)'
# p2.zpd's bake (native/zpd-bake, 2026-10-16): SR_NATIVE_ZPD default ON in the latency model
NF = 'kernel/native_flush.cpp'
RC = 'gfx/record_context.cpp'


def z20(value, mode, explicit=True):
    shown = 'unset' if value is None else '=' + value
    return f'Z20: SR_NATIVE_ZPD {shown} -> {mode}' + (' (explicit)' if explicit else ' (the baked default form)')


def z21(what, mode, port):
    return (f'Z21: a fresh process, SR_NATIVE_ZPD {what} -> {mode}, the producer port {"on" if port else "off"}, the boot '
            'line naming the form')


Z20_DEFAULT = {z20(None, 'latency', False), z20('', 'latency', False)}
Z20_ZERO = {z20('0', 'off'), z20('00', 'off'), z20('0x2', 'off')}
Z21_UNSET = z21('unset', 'latency', True)
Z21_ZERO = {z21('=0', 'off', False), z21('=00', 'off', False)}
Z21_EXPLICIT_LATENCY = {z21('=2', 'latency', True), z21('=latency', 'latency', True)}
Z22_PM4 = ('Z22 (latency): the guest\'s PM4 executed - one query, its BEGIN and END markers queued, two EVENT_WRITE_ZPDs '
           'taken by the native path, VGT_EVENT_INITIATOR 21')
Z22_REC = {f'Z22 ({m}): the kRkZpd record through the record engine\'s consumer (nativeRecord, the release path) equals '
           'the packet: markers, the logical query, the counters, the initiator' for m in ('latency', 'off')}
Z22_ORC = {f'Z22 ({m}): the kRkZpd record through the development oracle (nativeRecordOracle) equals the packet'
           for m in ('latency', 'off')}
Z23 = ('Z23: the producer port follows the mode: SR_NATIVE_ZPD unset (the gate\'s default ON), the port off exactly when '
       'the mode is off (strict / latency on)')
Z25_PM4 = {f'Z25 ({m}): the guest\'s literal packet - one frame stamped zpd={b} rec=0 pm4=2 (its BEGIN and END), frame 1'
           for m, b in (('latency', 1), ('off', 0))}
Z25_REC = {f'Z25 ({m}): the kRkZpd record through the release consumer (nativeRecord) - rec=2 pm4=0 (the port\'s form)'
           for m in ('latency', 'off')}
Z25_ORC = {f'Z25 ({m}): the kRkZpd record through the development oracle (nativeRecordOracle) - rec=2 pm4=0'
           for m in ('latency', 'off')}
Z25_PM4_LAT = next(n for n in Z25_PM4 if '(latency)' in n)
Z25_REC_LAT = next(n for n in Z25_REC if '(latency)' in n)
Z25_ORC_LAT = next(n for n in Z25_ORC if '(latency)' in n)
Z25_TAKE = 'Z25: a take zeroes the frame\'s counts and advances the ordinal (frame 1 rec=3 pm4=1, frame 2 empty)'
Z25_OFF = ('Z25: with the stamp off (the default path) nothing is counted and a take is empty; switched on, the next frame '
           'holds none of the events noted while off')
Z25_THREAD = ('Z25: the presenting stamp is the presenting thread\'s own (another thread sees none; cleared after the '
              'present)')
Z25_LINE = 'Z25: a present dumped in a hold prints its frame\'s stamp: [SIM_FREEZE_DUMP] ... zpd=3 rec=4 pm4=0 fswap=7'
Z25_NONE = 'Z25: a dumped present without a stamp prints zpd=- (never another frame\'s)'
HF = 'kernel/highfps.cpp'  # Astra round 1 on native/zpd-bake (P2-2): the zpd census at each sim-freeze hold's edges
Z24_ORDER = ('Z24: one sim-freeze hold logs [SIM_FREEZE] #1 begin, the zpd census `freeze #1 begin`, [SIM_FREEZE] #1 '
             'end, the zpd census `freeze #1 end` - in that order')
Z24_BEGIN = 'Z24: the begin edge carries the counters of the hold\'s start (port=3 portDecl=1)'
Z24_END = 'Z24: the end edge carries the counters after the hold (port=5 portDecl=2: the helper\'s calls during it)'
BAKE_MUTATIONS = (
    # the hold's end edge without its zpd census (the hold's presents unbracketed)
    ('freeze_end_census_dropped', ((HF, '  sr::kernel::highfps::freezeEdgeCensus(id, false);\n', ''),),
     {Z24_ORDER, Z24_END}),
    # ... its begin edge
    ('freeze_begin_census_dropped', ((HF, '  sr::kernel::highfps::freezeEdgeCensus(id, true);\n', ''),),
     {Z24_ORDER, Z24_BEGIN}),
    # both edges logged as `begin` (a grader could not tell the hold's end from its start)
    ('freeze_edge_word_stuck', ((HF, 'begin ? "begin" : "end"', '"begin"'),), {Z24_ORDER, Z24_END}),
    # the default not baked: unset reads off again (the pre-bake parse)
    ('default_unbaked', ((NZ, '  if (!e || !*e) return Request{kLatency, false};  // the BAKED default (native/zpd-bake): '
                              'the latency model\n', '  if (!e || !*e) return Request{kOff, false};\n'),),
     Z20_DEFAULT | {Z21_UNSET}),
    # ... baked to strict (the CP's partial-frame waits by default) instead of the agreed latency model
    ('default_strict', ((NZ, '  if (!e || !*e) return Request{kLatency, false};  // the BAKED default (native/zpd-bake): '
                             'the latency model\n', '  if (!e || !*e) return Request{kStrict, false};\n'),),
     Z20_DEFAULT | {Z21_UNSET}),
    # =0 no longer off (the A/B arm and the guest path lost)
    ('zero_not_off', ((NZ, '  if (e[0] == \'0\') return Request{kOff, true};\n', ''),), Z20_ZERO | Z21_ZERO),
    # an explicit =2 / =latency announced as the baked default (the default form's witness could not tell them apart)
    ('explicit_as_default', ((NZ, '  else if (!r.explicitForm)\n', '  else if (!r.explicitForm || r.mode == kLatency)\n'),),
     Z21_EXPLICIT_LATENCY),
    # the producer port's gate not baked (unset = off: the queries' packets stay guest PM4 by default)
    ('port_gate_unbaked', ((NF, '    const char* e = std::getenv("SR_NATIVE_ZPD");  // `e`: scripts/audit_forcing.py '
                                'reads `!e ||` as default ON\n    return !e || e[0] != \'0\';\n',
                            '    const char* e = std::getenv("SR_NATIVE_ZPD");\n    return e && e[0] != \'0\';\n'),),
     {Z21_UNSET, Z23}),
    # the port ignoring the mode (records whose answer path is off)
    ('port_ignores_mode', ((NF, '  return on && sr::gfx::zpd::on();\n', '  return on;\n'),), {Z23}),
    # the record engine's EVENT_WRITE_ZPD never taking the native path
    ('zpd_event_not_native', ((RE, '  writeReg(0x21F9u, initiator & 0x3Fu);\n  return zpdNative();\n',
                               '  writeReg(0x21F9u, initiator & 0x3Fu);\n  return false;\n'),), {Z22_PM4}),
    # the release consumer without a kRkZpd case (the record rejected)
    ('record_context_no_case', ((RC, '      case nf::kRkZpd:  // p2.zpd\'s bake: EVENT_WRITE_ZPD {initiator} - the host '
                                     'occlusion query (applyZpdEvent)\n        ++engine_.opcodeHist[0x5B];\n'
                                     '        engine_.applyZpdEvent(words[0], true);\n        break;\n', ''),), Z22_REC),
    # the development oracle without one
    ('oracle_no_case', ((PE, '        case nf::kRkZpd:  // p2.zpd\'s bake: EVENT_WRITE_ZPD {initiator} (the record exists '
                             'only with the native path on)\n          opcodeHist[0x5B]++;\n          applyZpdEvent(w[0], true);\n'
                             '          break;\n', ''),), Z22_ORC),
) + (
    # THE FRAME STAMP (Astra round 2 on native/zpd-bake, P2-1; Z25): each compared present's own queries
    # the release consumer's record counted as the literal packet's form (the A/B's port frames read as guest frames)
    ('stamp_record_form_lost', ((RC, '        engine_.applyZpdEvent(words[0], true);\n',
                                 '        engine_.applyZpdEvent(words[0]);\n'),), Z25_REC),
    # ... the development oracle's
    ('stamp_oracle_form_lost', ((PE, '          applyZpdEvent(w[0], true);\n', '          applyZpdEvent(w[0]);\n'),),
     Z25_ORC),
    # the two forms swapped in the count
    ('stamp_forms_swapped', ((NZ, '  (record ? g_frameRec : g_framePm4).fetch_add(1, std::memory_order_relaxed);\n',
                              '  (record ? g_framePm4 : g_frameRec).fetch_add(1, std::memory_order_relaxed);\n'),),
     Z25_PM4 | Z25_REC | Z25_ORC | {Z25_TAKE}),
    # BEGINs not counted (a frame with queries reads zpd=0)
    ('stamp_begins_dropped', ((NZ, '    if (frameStampOn()) g_frameBegins.fetch_add(1, std::memory_order_relaxed);\n', ''),),
     {Z25_PM4_LAT, Z25_REC_LAT, Z25_ORC_LAT}),
    # a take that does not zero the frame's counts (a frame carries its predecessors' queries)
    ('stamp_not_reset', ((NZ, '  s.rec = g_frameRec.exchange(0, std::memory_order_relaxed);\n',
                          '  s.rec = g_frameRec.load(std::memory_order_relaxed);\n'),), {Z25_TAKE}),
    # counted on the default path too (the stamp off: not diagnostic only)
    ('stamp_ungated', ((NZ, 'void noteSource(bool record) {\n  if (!frameStampOn()) return;\n',
                        'void noteSource(bool record) {\n'),), {Z25_OFF}),
    # the presenting stamp shared across threads (another thread's present would print this frame's)
    ('stamp_not_thread_local', ((NZ, 'thread_local FrameStamp t_presenting;\n', 'FrameStamp t_presenting;\n'),),
     {Z25_THREAD}),
    # the dump line without the stamp
    ('stamp_line_dropped', ((HF, '  if (st.frame)\n    std::snprintf(zpd, sizeof zpd, " zpd=%u rec=%u pm4=%u fswap=%u", '
                                 'st.begins, st.rec, st.pm4, st.frame);\n', '  (void)st;\n'),), {Z25_LINE}),
    # a present without a stamp printing the previous one (the dump reads a stale frame)
    ('stamp_line_stale', ((HF, '  char zpd[80] = " zpd=-";\n', '  char zpd[80] = " zpd=3 rec=4 pm4=0 fswap=7";\n'),),
     {Z25_NONE}),
    ('stamp_queue_capture_lost', ((BE,
        '    f.zpdStamp = zpdSeqOut ? zpd::FrameStamp{} : zpd::presentingStamp();\n',
        '    f.zpdStamp = {};\n'),),
     {'Z26: queued presents retain their own complete stamp after producer TLS changes'}),
    ('stamp_render_handoff_lost', ((BE,
        '                   f.tiled, f.zpdSeq != 0, f.zpdStamp);\n',
        '                   f.tiled, f.zpdSeq != 0);\n'),),
     {'Z26: queued presents retain their own complete stamp after producer TLS changes'}),
    ('stamp_partial_borrows_present', ((BE,
        '    f.zpdStamp = zpdSeqOut ? zpd::FrameStamp{} : zpd::presentingStamp();\n',
        '    f.zpdStamp = zpd::presentingStamp();\n'),),
     {'Z26: unstamped frame and partial set never borrow an earlier present stamp'}),
    ('stamp_synchronous_lost', ((BE,
        '                 tiled, false, zpd::presentingStamp());\n',
        '                 tiled);\n'),),
     {'Z26: synchronous frame keeps its stamp and restores caller TLS'}),
    ('stamp_tls_restore_lost', ((BE,
        '    ~RestoreStamp() { zpd::setPresentingStamp(previous); }\n',
        '    ~RestoreStamp() {}\n'),),
     {'Z26: synchronous partial clears its own stamp and restores nonempty caller TLS'}),
)
# Every mutated build must still pass these (the gate-off control; the publication checker's own control).
CONTROLS = ('Z0: gate off (dev today): no marker, the fence lands, the sentinel stays, GetData -1 for good',
            'Z9 control: the round-1 order (END+4 before END+0) gives an inconsistent S_OK - the check can fail')

ROUND1_PUBLICATION = '''  for (uint32_t i = 0; i < 8; i++) {
    le32(begin + 4u * i, 0u);
    stepped();
  }
  le32(end + 16, n);  // ZPass_A
  stepped();
  for (uint32_t i = 5; i < 8; i++) {
    le32(end + 4u * i, 0u);
    stepped();
  }
  std::atomic_thread_fence(std::memory_order_release);
  for (uint32_t i = 1; i < 4; i++) {
    le32(end + 4u * i, 0u);
    stepped();
  }
  (void)sentinel;
  le32(end + 0, n);  // Total_A
  stepped();
}'''

PUBLICATION = '''  // BEGIN {0, S, S, S, 0, 0, 0, 0}: its Total_B / ZFail words equal END's, so they cancel in GetData's differences.
  le32(begin + 0, 0u);
  stepped();
  for (uint32_t i = 1; i < 4; i++) {
    sentinel(begin + 4u * i);
    stepped();
  }
  for (uint32_t i = 4; i < 8; i++) {
    le32(begin + 4u * i, 0u);
    stepped();
  }
  // END {_, S, S, S, n, 0, 0, 0}: END+4/+8/+C stay the preset sentinel (never a "written" signal).
  for (uint32_t i = 1; i < 4; i++) {
    sentinel(end + 4u * i);
    stepped();
  }
  le32(end + 16, n);  // ZPass_A
  stepped();
  for (uint32_t i = 5; i < 8; i++) {
    le32(end + 4u * i, 0u);
    stepped();
  }
  std::atomic_thread_fence(std::memory_order_release);
  // The one "written" signal, last: a reader that sees it sees every word above (x64 keeps stores in order).
  le32(end + 0, n);  // Total_A
  stepped();
}'''

# name, ((file, before, after), ...), assertions that must fail
MUTATIONS = (
    # P2 (the production paths): the CP's covering-fence hook in applyEventWriteShd removed
    ('cp_hook_removed', ((RE, '  // p2.zpd: a drained END not written yet - the query\'s covering fence may be this store '
                              '(strict: write first).\n  if (cp.zpdFence && sr::gfx::zpd::fenceNeedsFlush()) '
                              'cp.zpdFence(cp.user);\n',
                          '  // p2.zpd: a drained END not written yet - the query\'s covering fence may be this store '
                          '(strict: write first).\n'),),
     {Z4_ORDER, Z12, Z13_FALLBACK, Z13_NOBE}),
    # P2 (the production paths): the executor's covering-fence hook in execPerformStores removed
    ('exec_hook_removed', ((PE, '      // p2.zpd: the covering-fence check at the store\'s stream position (the executor '
                                'drained the markers ahead).\n      if (cp.zpdFence && sr::gfx::zpd::fenceNeedsFlush()) '
                                'cp.zpdFence(cp.user);\n',
                            '      // p2.zpd: the covering-fence check at the store\'s stream position (the executor '
                            'drained the markers ahead).\n'),),
     {Z11_ORDER, Z11_AFTER}),
    # P2 (the real flush broken): the render-thread handshake returns before the partial frame finished
    ('rt_wait_skipped', ((BE, '[&] { return m_zpdPartialDone >= seq || m_rtStop; });', '[&] { return true; });'),),
     {Z11_ORDER, Z11_AFTER, Z12}),
    # P1 (strict fallback): a failed / impossible flush lets the fence publish over the sentinel again
    ('fallback_removed', ((NR, '  if (zpd::fenceNeedsFlush()) zpd::fenceFallback();\n', ''),),
     {Z13_FALLBACK, Z13_NOBE}),
    # P1 (the synchronous partial consumes its set): rendered in place, the set is rendered again at the swap
    ('sync_partial_retained', ((BE, '  std::vector<DrawBatch> set;\n  set.swap(m_drawBatches);\n',
                                '  std::vector<DrawBatch> set;\n  set = m_drawBatches;\n'),),
     {Z4_CONSUMED, Z4B_EMPTY}),
    # P1 (helper passes counted): the round-1 placement - a query opened at the BEGIN marker, across the helper work
    ('segment_at_begin_marker', ((BE, '    m_zpdOpenId = b.zpdId;  // the draws from here to its END are measured, '
                                      'each on its own (zpdDrawBegin)\n',
                                  '    m_zpdOpenId = b.zpdId;  // the draws from here to its END are measured, '
                                  'each on its own (zpdDrawBegin)\n    zpdRecord(true, zpd::segmentOpen(b.zpdId, 1u));\n'),),
     {Z4C_ORDER, Z4C_ADJ}),
    # P2 (publication protocol): the round-1 store order (END+4 / +8 / +C zeroed before END+0)
    ('publication_round1_order', ((NZ, PUBLICATION, ROUND1_PUBLICATION),),
     {Z9_STEPS, Z9_LAST, Z9_MODEL, Z4_WORDS}),
    # P2 (MSAA): the draw target's samples per pixel ignored (host / area, round 1)
    ('msaa_ignored', ((BE, 'const uint32_t seg = zpd::segmentOpen(m_zpdOpenId, edram::msaaSampleCount(msaa));',
                       'const uint32_t seg = zpd::segmentOpen(m_zpdOpenId, 1u + 0u * msaa);'),),
     {Z4_COUNT, Z10}),
    # zpd3 (2026-10-15): the census tag missing from the player profile's kMilestoneTags (every live roll lost it)
    ('log_tag_census_dropped', ((LOG, ZPD_TAGS, '    "NATIVE_ZPD_Q",'),), {Z14_CENSUS}),
    # ... and the per-query tag
    ('log_tag_query_dropped', ((LOG, ZPD_TAGS, '    "NATIVE_ZPD",'),), {Z14_QUERY}),
    # zpd4 round (the 0-sample hunt): a probe's count leaking into the answer (a probed occluded point reads visible)
    ('probe_in_answer', ((NZ, '          probeHost[kind] += readback[q.segs[i].idx];\n          continue;\n',
                          '          probeHost[kind] += readback[q.segs[i].idx];\n'),),
     {Z15_NO_LEAK, Z16_BEHIND}),
    # ... a probe on every draw of a query instead of one of each kind on its first
    ('probe_every_draw', ((NZ, '    if ((q->probed >> probe) & 1u) return kNoSeg;\n', ''),), {Z15_TWO, Z15_SLOTS}),
    # ... the probes taking answer segments (kMaxSegs counted over every segment)
    ('probe_takes_answer_slot', ((NZ, '  } else if (q->nAnswer >= kMaxSegs) {', '  } else if (q->nseg >= kMaxSegs) {'),),
     {Z15_SLOTS}),
    # ... the classification: a 0 answer with ref > 0 counted as a pixel-shader kill
    ('probe_classify_swapped', ((NZ, '        if (hasRef && probeHost[kRef]) g_st.depthKilled++;\n'
                                     '        else if (hasRaster && probeHost[kRaster]) g_st.psKilled++;\n',
                                 '        if (hasRaster && probeHost[kRaster]) g_st.psKilled++;\n'
                                 '        else if (hasRef && probeHost[kRef]) g_st.depthKilled++;\n'),),
     {Z15_DEPTH, Z16_BEHIND}),
    # ... the point sprite's c19 as a radius of the RADIUS (PA_SU_POINT_SIZE read as a diameter): half-size sprites
    ('point_radius_halved', ((ED, '  out[0] = extX > 0.0f ? diamX * float(sx) / extX : 0.0f;\n',
                              '  out[0] = extX > 0.0f ? 0.5f * diamX * float(sx) / extX : 0.0f;\n'),),
     {Z16_TWO_PX}),
    # zpd7 (the Xenia oracle's half): the traced-frame tag missing from the player profile (the lines never reach a roll)
    ('log_tag_trace_dropped', ((LOG, TRACE_TAGS, '    "PM4_LIVE_TRACE",'),), {Z17_WRITTEN, Z17_FALLBACK}),
    # ... the trace's swap not kept at the BEGIN (no traced query ever logs)
    ('trace_tag_ignored', ((NZ, '    q.trace = trace;\n', ''),), {Z17_WRITTEN, Z17_FALLBACK}),
    # ... the outcome logged for every query, traced or not (the oracle pairs the wrong ones)
    ('trace_line_every_query', ((NZ, '  if (!q.trace || g_tLines >= kTraceLines) return;\n',
                                 '  if (g_tLines >= kTraceLines) return;\n'),), {Z17_WRITTEN, Z17_UNTRACED}),
    # Astra merge review P1: the BEGIN no longer re-arms the slot (the previous query's late count reads as ready)
    ('begin_rearm_removed', ((NZ, '    if (slotHost)\n      for (uint32_t i = 0; i < 4; i++) sentinel(slotHost + 4u * i);\n',
                              ''),), {Z18_REARM, Z18_GETDATA}),
    # zpdx2 (Xenia's dump crashed on every trace): host-copy packets recorded at physical 0 again
    ('host_packet_at_zero', ((LT, '      putU32(f, p.hostSide ? kHostPacketPhys : p.phys);\n',
                              '      putU32(f, p.phys);\n'),), {Z19_HOST, Z19_ZERO}),
    # ... IM_LOAD_IMMEDIATE's inline ucode recorded as a read of physical 0 again
    ('imload_immediate_read', ((RE, '  if (cpMode && !inlineUcode) sr::gfx::livetrace::memRead(',
                                '  if (cpMode) sr::gfx::livetrace::memRead('),), {Z19_ZERO}),
    # zpdx3: one frame per trace again (SR_PM4_TRACE_FRAMES ignored)
    ('single_frame_trace', ((LT, '    if (s.framesLeft > 1) {\n', '    if (false && s.framesLeft > 1) {\n'),),
     {Z19_EVENTS}),
    # ... every loose read at the start of the file (a later frame's page version before an earlier frame's draws)
    ('loose_reads_file_first', ((LT, '    for (size_t i = from; i < s.ev.size() && s.ev[i].kind != 3; i++)\n',
                                 '    for (size_t i = from; from == 0 && i < s.ev.size(); i++)\n'),), {Z19_LOOSE}),
    # ... the traced queries tagged with the frame being recorded, not the trace's file (the oracle pairs nothing)
    ('traced_swap_current_frame', ((LT, '  return s.first > 0 ? uint32_t(s.first) : 0u;\n',
                                    '  return s.target > 0 ? uint32_t(s.target) : 0u;\n'),), {Z19_SWAP}),
) + BAKE_MUTATIONS


def parse(raw):
    lines = raw.decode(errors='replace').replace('\r\n', '\n').splitlines()
    good = [line[6:] for line in lines if line.startswith('  ok  ')]
    bad = [line[6:] for line in lines if line.startswith('FAIL: ')]
    return good, bad, SUMMARY.findall('\n'.join(lines))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--only', default='', help='comma-separated mutation names (default: all)')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = (root / args.out).resolve() if not args.out.is_absolute() else args.out
    out.mkdir(parents=True, exist_ok=False)
    only = {s for s in args.only.split(',') if s}
    mutations = [m for m in MUTATIONS if not only or m[0] in only]
    files = sorted({f for _, repls, _ in mutations for f, _, _ in repls})
    originals = {f: (root / f).read_bytes() for f in files}

    def fit(f, text):  # the working tree's line endings
        b = text.encode()
        return b.replace(b'\n', b'\r\n') if b'\r\n' in originals[f] else b

    for name, repls, _ in mutations:
        for f, before, _after in repls:
            n = originals[f].count(fit(f, before))
            if n != 1:
                raise SystemExit(f'{name}: anchor occurs {n} times in {f}')
    tracked = sorted(set(files) | {'gfx/native_zpd.hpp', 'gfx/d3d12_backend.hpp', 'gfx/d3d12_backend.cpp', 'app/log.cpp',
                                   'gfx/edram.hpp', 'tools/xenosrecomp/point_sprite.gs.hlsl',
                                   'gfx/pm4_live_trace.hpp', 'gfx/pm4_live_trace.cpp', 'gfx/pm4_engine.hpp', 'gfx/record_engine.hpp',
                                   'tools/difftest/native_zpd_test.cpp', 'tools/difftest/native_zpd_run.py'})
    result = dict(scope='Offline proof of SR_NATIVE_ZPD (native/zpd) after Astra round 1 and the zpd4 round: the '
                  'generated D3DQuery Issue 82102BC8 (wrappers 82779A28 / 82779A38) and GetData 82102DC8 (wrapper '
                  '82779A48), the Pm4Engine executing the guest PM4 and the executor store item, null_renderer '
                  'cpZpdFence and D3D12Backend pm4ZpdFlushPartial / render-thread handshake / query placement / the '
                  'visibility probe (device-less seam); the HUD query\'s point draw through the real point-sprite GS '
                  'on WARP (Z16); the bake (Z20-Z23): the default form\'s decision table, the fresh-process latch, the '
                  'kRkZpd record\'s apply, the producer port\'s gate. No live acceptance.',
                  source_sha256={p: sha((root / p).read_bytes()) for p in tracked}, mutations=[])

    def save():
        (out / 'zpd_mutants.json').write_text(json.dumps(result, indent=2) + '\n')

    base_env = {k: v for k, v in os.environ.items() if not k.upper().startswith('SR_')}
    base_env['PATH'] = r'C:\Program Files\LLVM\bin;' + base_env.get('PATH', '')

    def run(tag, command, timeout):
        try:
            p = subprocess.run([str(c) for c in command], cwd=root, env=base_env, capture_output=True, timeout=timeout)
            code, stdout, stderr = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired as error:
            code, stdout, stderr = 124, error.stdout or b'', error.stderr or b''
        (out / (tag + '_stdout.txt')).write_bytes(stdout)
        (out / (tag + '_stderr.txt')).write_bytes(stderr)
        return code, stdout

    def build(tag):
        # The shared worker's sccache server sometimes drops a connection: retried, recorded.
        for attempt in range(4):
            code, raw = run(tag if not attempt else f'{tag}_retry{attempt}',
                            ['cmake', '--build', 'build', '--target', 'difftest', '-j', str(args.jobs)], 2400)
            if code == 0 or (b'sccache' not in raw and b'forcibly closed' not in raw):
                break
        exe = root / 'build/difftest.exe'
        row = dict(exit=code, attempts=attempt + 1, exe_sha256=sha(exe.read_bytes()) if exe.exists() else None)
        print(json.dumps({'build': tag, **row}), flush=True)
        if code != 0:
            raise RuntimeError(f'{tag} failed; see {tag}_stdout.txt')
        return row

    def test(tag):
        code, raw = run(tag, [root / 'build/difftest.exe', '--native-zpd-test'], 600)
        good, bad, summary = parse(raw)
        return dict(exit=code, passed=len(good), failed=len(bad), summary=summary, stdout_sha256=sha(raw),
                    failures=bad), good, bad, summary

    def positive(tag):
        row, good, bad, summary = test(tag)
        row['valid'] = bool(row['exit'] == 0 and not bad and good and summary == [(str(len(good)), '0')]
                            and len(set(good)) == len(good) and all(c in good for c in CONTROLS))
        print(json.dumps({'test': tag, 'exit': row['exit'], 'summary': summary, 'valid': row['valid']}), flush=True)
        return row

    def restore_all():
        for f, data in originals.items():
            if (root / f).read_bytes() != data:
                (root / f).write_bytes(data)

    total = 0
    try:
        result['positive_build'] = build('positive_build')
        result['positive'] = positive('positive')
        total = result['positive']['passed']
        save()
        if not result['positive']['valid']:
            raise RuntimeError('positive run invalid; mutations not attempted')
        for name, repls, required in mutations:
            changed = dict(originals)
            for f, before, after in repls:
                b, a = fit(f, before), fit(f, after)
                if changed[f].count(b) != 1:
                    raise RuntimeError(f'{name}: production anchor changed; refusing mutation')
                changed[f] = changed[f].replace(b, a)
            if any((root / f).read_bytes() != originals[f] for f in files):
                raise RuntimeError('concurrent source edit; refusing mutation')
            mrow = dict(name=name, required=sorted(required), mutated_sha256={f: sha(changed[f]) for f, _, _ in repls})
            result['mutations'].append(mrow)
            for f, _, _ in repls:
                (root / f).write_bytes(changed[f])
            try:
                mrow['build'] = build(name + '_build')
                trow, good, bad, summary = test(name)
                trow['missing_required'] = sorted(required - set(bad))
                trow['controls_failed'] = [c for c in CONTROLS if c not in good]
                trow['valid'] = (trow['exit'] == 1 and summary == [(str(len(good)), str(len(bad)))]
                                 and len(good) + len(bad) == total and not trow['missing_required']
                                 and not trow['controls_failed'])
                mrow['test'] = trow
                mrow['detected'] = bool(trow['valid'])
                print(json.dumps({'mutation': name, 'failed': trow['failed'], 'missing': trow['missing_required'],
                                  'controls_failed': trow['controls_failed'], 'detected': mrow['detected']}),
                      flush=True)
            finally:
                restore_all()
                mrow['source_restored'] = all((root / f).read_bytes() == originals[f] for f in files)
                save()
    finally:
        result['valid'] = False
        restore_all()
        result['all_sources_restored'] = all(sha((root / p).read_bytes()) == v for p, v in result['source_sha256'].items())
        try:
            if result['all_sources_restored'] and result['mutations']:
                result['restored_build'] = build('restored_build')
                result['restored'] = positive('restored')
        finally:
            result['valid'] = bool(
                result['all_sources_restored'] and (result.get('positive') or {}).get('valid')
                and len(result['mutations']) == len(mutations)
                and all(m.get('detected') and m.get('source_restored') for m in result['mutations'])
                and (result.get('restored') or {}).get('valid'))
            save()
            print(json.dumps({'valid': result['valid']}), flush=True)
    return 0 if result['valid'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
