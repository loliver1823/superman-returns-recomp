#!/usr/bin/env python3
"""transport_census.py - the p2.transport GAP MAP and the composed census pair's checks, from the PRODUCER census
(SR_NATIVE_BRACKETS=1; gfx/pm4_coverage [PM4_COV] / [PM4_COVMAP] inner= lines) of perf_run rolls.

    python tools/perf/transport_census.py <roll> [--json out.json]                      # one roll's gap map
    python tools/perf/transport_census.py --base <census0> --target <census1> --json out.json [--strict-zero]
           [--transport-zero] [--flush-residue-zero] [--construns-zero] [--pm4-zero [--allow-devcreate-reemit]]

A roll is a perf_run folder (drive/attempt1/run.log) or a run.log.

THE INTERVAL (what every per-frame number here covers). The census is cumulative and periodic: one report (a
[PM4_COV] CUM line, then the [PM4_COVMAP] owner= / inner= / kind= lines) every 256 frames or 20 s
(gfx/pm4_coverage.cpp frameEnd) and none at exit (the harness kills the game). A census "frame" is one XE_SWAP the CP
executed (one present). The census-covered INGAME interval is (A, B]: A = the first report printed at or after the
ingame marker (perf_report DEFAULT_ERAS "ingame": the end of the opening movie after the HUD request; "gameplay" when
the log has no ingame marker), B = the last report the CP's present count VERIFIED (the COVERAGE WITNESS below: the last
report with an exactly reconciled [GPU_CP] sample after it - the final report, or the one before it when the run
stopped between the final report and the next CP sample). Per producer, per frame = (B's count - A's count) / (B.frames
- A.frames). The F lines (one per frame) between them give the per-frame totals; the uncovered tail after B is
reported in frames and seconds, unattributed.

COVERAGE WITNESS (native/composed-census; checked FIRST, a roll that fails it has no usable census: exit 2). A census
"frame" ends at every present the CP applies; the census prints one F line per frame (0 packets included) and its
reports from the frame end. Coverage that counts but never ends a frame prints NOTHING - no F, CUM, [PM4_COVMAP] or
[NATIVE_BRACKETS] line - which is how tcmp2's composed census1 (SR_NATIVE_SWAP_BLOCK: every present a kRkSwap record,
no XE_SWAP packet, on exes before native/composed-census) read: 5,640 CP presents, zero lines. A roll is usable only when:
  (1) the CP's own present count is there: [GPU_CP] <why> ... swaps=N samples (kernel/gpu_command_processor census(),
      printed by the CP thread between ring items, as the F lines are; every 120 presents), the last one with N > 0;
  (2) every frame end that is not a present is accounted for. The terminal snapshot's tail frame ("[PM4_COV] <why> tail
      frame=", the drained partial frame) is one such F line, excluded by its own line. On an exe with the witness
      ([PM4_COV] armed) every report carries its "[PM4_COV] ENDS <why> frames= xeSwap= present= presents=" line and at
      every one: frames - tails = xeSwap + present (each frame ended at a packet or a present) and presents = xeSwap +
      present (each frame end matched by a present the counter saw: an XE_SWAP that applied none - unsigned - or a
      present that ended no frame breaks it), less the one present still pending when the report was printed at an
      XE_SWAP packet's frame end (packet3 ends the frame before the CP applies the packet's present; allowed only when
      an XE_SWAP frame end happened since the report before, never on a terminal report). The CP samples of (3) are
      exact with nothing pending. On an exe without the witness nothing prints those counts, so frame ends
      without a present (an unsigned XE_SWAP) and presents without a frame end (a kRkSwap record on those exes) could
      cancel: such a roll is usable only with SR_NATIVE_SWAP_BLOCK off (no "[NATIVE_FLUSH] <why> swapBlock calls="
      line, which kernel/native_flush prints periodically whenever the gate is on), where frame ends can only exceed
      presents;
  (3) at EVERY CP sample, EXACTLY swaps=N present frame ends before it (F lines less tail lines): fewer = a present
      that ended no frame (dark), more = a frame end with no present (surplus). tcmp2 census0: 45 / 45 samples equal;
      census1: 0 F lines at 51 / 51;
  (4) a report verified by a CP sample after it (B above; a report after the last sample is not verified: a present
      after the last sample could have missed its frame end unseen), and the counter's witness: [PM4_COV] armed, or on
      an older exe F lines reconciled as above.
`coverage.state`: live (all of it), dark (the counter ran - armed, F lines, or a report / [NATIVE_CPTIME] line - but
fewer present frame ends than CP presents: counting without a frame boundary, no data, never 0 packets), surplus (more
frame ends than presents), unaccounted (a report without its ENDS line on a witnessed exe, an ENDS identity broken, or
a witness-less exe with record presents), unwitnessed (no armed line, no counter line of any kind: off), unchecked (no
[GPU_CP] line with a present), unverified (no report before the last reconciled CP sample). A dark side makes the
pair's verdict DARK_CENSUS; any other unusable side NO_CENSUS (both exit 2).

COMPLETENESS (each a check in the JSON; any failure exits 1 under --base/--target):
  * F records frame 1 .. last contiguous, each parsing in full (a gap or a malformed record is INCOMPLETE);
  * the census runs to the end of the run: the FINAL report within one reporting period of the run's end (tail < 256
    frames and <= 21 s), and B (the verified report) within one reporting period plus one CP sampling period of it
    (tail from B < 256 + 120 frames);
  * over (A, B], per key, the F lines' sum == the CUM lines' delta == the sum of every producer's inner= delta
    (the census attributes every packet the CP counted to exactly one producer line, UNSTAMPED included).

THE MAP (PRODUCER_MAP below): each producer VA -> the obligation that removes its packets (a transport kind of
NATIVE_ROADMAP's p2.transport plan, or another ledger record). With --base/--target the target's interval holds, per
producer, what still reaches the CP with the target's gates; `remaining` lists every producer > 0 there with its
obligation; --strict-zero additionally requires the target's interval F lines all pk=0 ([PM4_COV] 0).

GROUPS (dot-free keys, so docs/native_ledger.json json checks can read them: `groups.transport`): transport (every
producer whose obligation is p2.transport, the 820F0048 re-emit under Swap included - the orchestrator's decision of
2026-10-14), flush_residue (p1.flush_residue: the S1 flushes' guest leaves and declines, row 9's declines),
construns (p1.row3_construns), setconst, zpd, unattributed (UNSTAMPED / RING_UNSTAMPED inside the interval) and
unmapped. Counts are exact integers over the interval (`group_pk`) beside the per-present rates (`groups`).

THE TRANSPORT-PRODUCER ZERO (--transport-zero; p2.transport's observation since the orchestrator's 2026-10-14
decision): over the target's census interval the transport group holds 0 packets, nothing is unattributed (an
unstamped packet could be a transport producer's: fail closed) or unmapped, and no other producer's line carries a
transport signature key - INTERRUPT 54, EVENT_WRITE_SHD 58, XE_SWAP 64, INDIRECT_BUFFER 3F / 37 and the callback
mask page r04 (not r05: a declined resolve / clear writes its own WAIT_UNTIL 0x5C8, rcen1's 8210C5F8 and 820F5570
lines): what a guest-mode close (its fence tail's 58), a callback block (54, r04) or a kick block (54) written
inside another producer's scope would show (the census attributes by the innermost TABLE scope; 820FD8C0 / 820FDA80
are not producers of their own). The base must show the transport group (> 0). The rest (flush_residue, construns)
is reported, not graded.

P1.FLUSH_RESIDUE'S ZERO (--flush-residue-zero; native/flush-residue, the merge with dev ce46c966): over the target's
census interval the flush_residue group holds 0 packets (`flush_residue.pk`, always written) and the base shows it
(> 0). Unattributed packets are reported, not graded here (a census pair without every transport kind still has the
SDK pump's RING_UNSTAMPED bin-select dwords). The construns leaf's alignment pad is p1.row3_construns' only as the
census's witnessed key o6 (a NOP right before 821075E0's run header; KEY_SPLIT, native/flush-residue-2 Astra round 1);
every o1 on an S1 line is residue. FROM BOOT as well (target_flush_residue_zero_from_boot): the target's last report,
cumulative from boot, holds no flush_residue packet either - counts never fall, so no earlier report held one (an
opening residue before the interval cannot pass).

P1.ROW3_CONSTRUNS' ZERO (--construns-zero; native/construns-bake, 2026-10-14): over the target's census interval the S1
producers' lines hold no 821075E0 SDK-body preamble (`construns.sync_pk` = their 3C + r0A, 3 per SDK-body call: see
CONSTRUNS_SYNC_KEYS) and the base's do (> 0); none on the target's UNSTAMPED / RING_UNSTAMPED lines either
(`construns.unattributed_sync_pk`: a preamble whose attribution was lost fails closed); no tessellated draw (820FC478's
line, `construns.tess_pk`) in either roll.
The r20 / r23 the target's S1 lines keep are reported (`target_construns_runs_residue_per_frame`): with the preamble at 0
they are the S1 fallbacks' register runs (p1.flush_residue's), which the group arithmetic (KEY_SPLIT) cannot tell apart.

THE DEFAULT PATH'S PM4 ZERO (--pm4-zero; native/transport-bake, 2026-10-15: the transport kinds baked default ON, so the
default arm of the bake's census pair must hold no PM4 in gameplay at all - no producer left, not only the transport
group). Two views of the target, both required:
  * from the ingame marker to the run's last F line (the census interval, the frames before A and the uncovered tail
    after B together: every frame the counter ended in gameplay), every F line's packets (`pk`: type-0 / 1 / 2 / 3 at
    every depth, pads included) are 0 - `pm4_zero.ingame_other_pk` = 0 over `pm4_zero.ingame_frames` > 0 frames;
  * over the census interval (A, B] no producer line holds a packet (`pm4_zero.interval_other_pk` = 0): UNSTAMPED /
    RING_UNSTAMPED, every transport producer, the flush residue, construns, setconst, zpd, unmapped alike.
--allow-devcreate-reemit makes ONE exception, for a default path that keeps SR_NATIVE_DEVCREATE off (boot.device_create
is not baked): 820F0048's per-present re-emit under Swap - SET_BIN_MASK / SELECT 60 / 61 / 62 / 63, WAIT_UNTIL r05 and
RB_BC_CONTROL r0F (KEY_SPLIT: 6 a present, IDA 820F0048) - and only as itself: on the interval those keys on 82112050's
line alone, and in each F line at most ONE of each of the six keys with nothing else beside them (`pm4_zero.ingame_frames_over` = 0:
two re-emits in a frame, or a re-emit key that is not 820F0048's, fails). The exception's packets are reported
(`pm4_zero.reemit_pk`, `reemit_per_frame`). The base must show the transport group (`base_transport_exercised`).
Since Astra round 1 on native/transport-bake (the allowance was subtracted from every gameplay frame but attributed
only inside (A, B]: a BlockUntilIdle r05 before A, or five after B, passed), the allowance holds only as itself over
the WHOLE gameplay span:
  * every gameplay F line holds no re-emit key or exactly one WHOLE re-emit - all six keys, one each
    (`target_reemit_whole`: `pm4_zero.ingame_frames_partial` = 0; the graceful quit's drained tail frame exempt, its
    terminal report attributes it);
  * every report segment holding a gameplay frame, from the one with the ingame marker to the final report, attributes
    its six keys to 82112050's line (`target_reemit_attributed`: `pm4_zero.reemit_foreign_pk` = 0; reemit_attribution:
    a foreign re-emit key is placed in gameplay unless the segment's pre-marker frames could hold it);
  * allowed packets no producer line can place - the frames after the final report (a roll the harness kills has no
    exit report; --graceful-quit's terminal report covers the run), or a straddling segment's foreign keys that could
    be either side of the marker - make a would-be PASS INCONCLUSIVE (exit 3, `pm4_zero.reemit_unverified_pk`): no
    evidence either way.
THE DEVCREATE BAKE'S PAIR (--pm4-zero --base-shows devcreate; native/devcreate-fix, 2026-10-15): SR_NATIVE_DEVCREATE=0
against the default, both on the transport-baked exe, so the transport is native in BOTH arms and the base shows the
re-emit instead - every one of 820F0048's six keys on 82112050's line over the interval (`base_devcreate_exercised`,
`base_reemit_keys`); the target holds NO PM4 in gameplay with NO allowance (the two views above, nothing let through):
the default path's gameplay PM4 zero, acceptance.zero's gameplay half.
THE ZPD BAKE'S PAIR (--pm4-zero --base-shows zpd; native/zpd-bake, 2026-10-16): SR_NATIVE_ZPD=0 against the default
(SR_NATIVE_ZPD baked ON, the latency model), both from the chapter-12 seed in Metropolis where the HUD glow queries fire,
so the transport and devcreate are native in BOTH arms and the base shows the queries as guest PM4 instead -
EVENT_WRITE_ZPD (5B) on D3DQueryIssueBeginEnd_82102BC8's line and the zpd group > 0 over the interval
(`base_zpd_exercised`, `base_zpd`); the target holds NO PM4 in gameplay with NO allowance (the two views above): the
queries' packets travel as records (82101E08's port, kRkZpd), no literal-PM4 gap left in the Metropolis era. A producer
line is a bracket entry's (kernel/native_brackets.gen.cpp; the innermost open scope): 82101E08, which writes the
query's packets, is no entry and neither are its title callers (82304058, 82721118), so every occlusion query's
packets sit on 82102BC8's line (s1c1, 2026-10-16: `producer 82102BC8 ... pk=8`, keys 5B:2,r20:2,r22:2,r23:2).
Coverage first, as everywhere: a dark or unwitnessed census is no data (exit 2), never a zero.

Exit 0 = PASS (or a single roll printed), 1 = a check failed (JSON still written), 2 = a roll has no usable census,
3 = INCONCLUSIVE (--allow-devcreate-reemit: no check failed, but an allowed packet is unattributed). Read-only apart
from --json.
"""
import argparse
import datetime
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import perf_report  # noqa: E402

TS = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]")
CUM = re.compile(r"\[PM4_COV\] CUM (\S+) frames=(\d+) pk=(\d+) .* acc=(\d+) unst=(\d+)(?: chain=(\d+))? .*"
                 r" ops=(\S*) regs=(\S*)\s*$")
MAP = re.compile(r"\[PM4_COVMAP\] (\S+) (owner|inner)=([0-9A-Fa-f]{8}) name=(\S+) pk=(\d+) dw=(\d+) keys=(\S*)")
FLINE = re.compile(r"\[PM4_COV\] F frame=(\d+) pk=(\d+) t0=(\d+) t0dw=\d+ t3=(\d+) t3dw=\d+ pred=(\d+) ringIb=(\d+) "
                   r"zero=\d+ trunc=\d+ d0=(\d+) dN=(\d+) acc=(\d+) unst=(\d+) ops=(\S*) regs=(\S*)\s*$")
FRAME_ANY = re.compile(r"\[PM4_COV\] F frame=(\d+)")
# printed with every report while the producer census is on (gfx/pm4_coverage.cpp report), even when no producer wrote
# a packet: the marker that a report with no producer line is a census of zero, not a census that is off
ORIGIN = re.compile(r"\[PM4_COVMAP\] (\S+) internalOrigin pk=(\d+)")
# the counter's positive witness (gfx/pm4_coverage armed(): once, at its first hook; exes from native/composed-census on)
ARMED = re.compile(r"\[PM4_COV\] armed (?:\w+ )?brackets=(\d)")  # (the terminal snapshot's: armed terminal brackets=)
# how the census frames ended (printed after every CUM from native/composed-census on): xeSwap = at an XE_SWAP packet,
# present = at a present the CP applied with no packet (a kRkSwap record), presents = the CP's presents while counting
ENDS = re.compile(r"\[PM4_COV\] ENDS (\S+) frames=(\d+) xeSwap=(\d+) present=(\d+) presents=(\d+)")
# the CP's own present count, the independent counter the F lines must track (kernel/gpu_command_processor census())
CPSWAPS = re.compile(r"\[(?:GPU_CP|NATIVE_SUBMIT)\] (\w+) rptr=\d+ wptr=\d+ .*? swaps=(\d+) ")
# the terminal snapshot's partial frame (gfx/pm4_coverage terminal()): its F line follows, a frame end with no present
TAIL = re.compile(r"\[PM4_COV\] \S+ tail frame=(\d+) ")
# a present with no XE_SWAP packet on an exe without the ENDS line (kernel/native_flush reportLine: kRkSwap records)
SWAPBLOCK = re.compile(r"\[NATIVE_FLUSH\] \S+ swapBlock calls=\d+ decl=\d+ applied=(\d+)")
# the counter ran (a dark counter on an exe without the armed line): any line only the counter prints
COUNTER_RAN = re.compile(r"\[PM4_COV\] (?:F frame=|CUM )|\[PM4_COVMAP\] |\[NATIVE_BRACKETS\] |\[NATIVE_CPTIME\] ")
OP_KV = re.compile(r"^([0-9A-F]{2}):(\d+)$")
REG_KV = re.compile(r"^([0-9A-F]{2}):(\d+)/(\d+)$")
REPORT_FRAMES, REPORT_SECONDS, SLACK_S = 256, 20.0, 1.0
CP_SAMPLE_PRESENTS = 120  # kernel/gpu_command_processor: a [GPU_CP] census sample every 120 presents

# Producer (Stamp::inner VA) -> (short name, the obligation that removes its packets, why). Built 2026-10-14 from rcen1
# (census0 = segmentless only, census1 = + the three residue gates; exe 6705f9e06683) and the code of dev 041d6f9e;
# docs/NATIVE_ROADMAP.md "p2.transport GAP MAP" is the prose. A producer not listed maps to UNMAPPED (a finding).
PRODUCER_MAP = {
    "82112050": ("D3DDeviceSwap", "p2.transport:SWAP",
                 "VdSwap's 64-dword block (kind 19, SR_NATIVE_SWAP_BLOCK), the D1GRPH run / scaler reset (kind 20) and the "
                 "SDK body as host code (SR_NATIVE_SWAP); 820F0048's per-frame re-emit runs under Swap's scope "
                 "(boot.device_create's 820F0048 port, kinds 8 / 9, SR_NATIVE_DEVCREATE: in the composed env)"),
    "82111EC8": ("GpuPresentEmitCommandBlock (vblank gate)", "p2.transport:GATE",
                 "the 39-dword gate block (kind 17, SR_NATIVE_GATE); its callback is the guest 82111E38 under GATE (a), "
                 "host code under GATE (b) = display.registers"),
    "8210E1A0": ("D3DDeviceEndZPass (kick block)", "p2.transport:BRACKET",
                 "the Z-pass kick 82110C78's coherency + callback block (allocated under EndZPass's scope), retired by "
                 "BRACKET_END (kind 21, SR_NATIVE_BRACKET)"),
    "820FDCB8": ("D3DDeviceBlockUntilIdle", "p2.transport:RESIDUE", "WAIT_UNTIL (SR_NATIVE_IDLE_WAIT)"),
    "820F4280": ("GpuInvalidateMemoryRange", "p2.transport:RESIDUE", "owner WAIT_UNTIL + COHER (SR_NATIVE_INVAL_RANGE)"),
    "820F3FA0": ("VertexBuffer_Lock", "p2.transport:RESIDUE", "owner COHER (SR_NATIVE_LOCK_COHER)"),
    "820FFCB0": ("texture lock", "p2.transport:RESIDUE", "owner COHER (SR_NATIVE_LOCK_COHER)"),
    "820FFDA0": ("texture lock", "p2.transport:RESIDUE", "owner COHER (SR_NATIVE_LOCK_COHER)"),
    "820FFF00": ("surface lock", "p2.transport:RESIDUE", "owner COHER (SR_NATIVE_LOCK_COHER)"),
    "82100228": ("volume lock", "p2.transport:RESIDUE", "owner COHER (SR_NATIVE_LOCK_COHER)"),
    "821002D8": ("volume lock", "p2.transport:RESIDUE", "owner COHER (SR_NATIVE_LOCK_COHER)"),
    "820FDA80": ("GpuEmitCpuCallbackPackets", "p2.transport:CALLBACK", "kind 13 (SR_NATIVE_CALLBACK)"),
    "820FDE38": ("InsertCallback", "p2.transport:CALLBACK", "kind 13 (0 calls in this title)"),
    "82110B30": ("BeginRecording", "p2.transport:BRACKET", "BRACKET_BEGIN (no PM4 of its own)"),
    "82110C78": ("EndRecordingKick", "p2.transport:BRACKET", "BRACKET_END"),
    "82110840": ("the pump driver", "p2.transport:BRACKET", "the replay pump 82110460 (no pump under BRACKET)"),
    "RING_UNSTAMPED": ("ring dwords no stamp claims", "boot.device_create / p2.transport:BRACKET",
                       "ME_INIT at boot (820FDE90's ring init); the SDK pump's 8 bin-select ring dwords 82110140 per "
                       "replayed bracket (unstamped by design; none when every bracket is native)"),
    "UNSTAMPED": ("bytes no stamp claims", "boot.device_create",
                  "D3DDeviceCreate 820F34C0's device-less bracket (820F0048's boot call, 82103238's RB_BC_CONTROL)"),
    # the S1 draw flushes: what still reaches the CP from their scope is guest-leaf output (kernel/native_flush.cpp
    # Driver: constRuns / regRuns / fetchRuns hand a whole section to the guest leaf 821078C0 / 82107520 / 82107760 when
    # it does not fit before the segment limit dev+0x2C - "the DrawVertices flush residue (... segment-limit fallbacks)"
    # of p2.transport's own title, with no transport kind built for it - and the whole-call S1 declines (count >
    # 0xFFFF draws); both write type-0 constant / fetch / register runs with their type-2 alignment pads
    "820FBBF8": ("D3DDeviceEmitDrawVertices (S1 flush)", "p1.flush_residue",
                 "guest leaves at the segment limit (ALU constants r40-r47 via 821078C0, fetch constants r48 + the r50 "
                 "trailer via 82107760, register runs r21 / r22 via 82107520, their type-2 pads) and count > 0xFFFF "
                 "declines (DRAW_INDX 22)"),
    "820FC000": ("D3DDeviceEmitDrawIndexedVertices (S1 flush)", "p1.flush_residue", "as 820FBBF8"),
    "820FB6E8": ("D3DDeviceEmitDrawIndexedGen (rows 4+6+7)", "p1.flush_residue",
                 "as 820FBBF8 (inner of DrawVerticesUP 820FBBB0)"),
    "820F5570": ("GpuRingEmitRegisterBlock", "p1.flush_residue", "row 9's declines: ClearFullSurface / register blocks"),
    "82101A58": ("D3DDeviceClearF", "p1.flush_residue", "row 9's declines: clears"),
    "82101998": ("D3DDeviceClear", "p1.flush_residue", "row 9's declines: clears"),
    "8210C5F8": ("D3DDeviceResolve", "p1.flush_residue", "row 9's declines: resolves"),
    "82113010": ("D3DDeviceEmitPm4Packet (per-tile constants)", "p2.setconst_mem",
                 "kRkSetConstMem, default ON since 57a1b601 / merge 4f6c7972 (after rcen1's exe 6705f9e06683)"),
    "820FCF90": ("GpuCmdBufBeginNewSegment (kick blocks)", "p2.transport:BRACKET",
                 "the Z-pass kick block allocated across a segment switch is stamped with 820FCF90's scope: its keys are the "
                 "kick block's exactly (3C 3 : 54 1 : r04 1 : r05 3 : r0A 2); BRACKET_END writes no kick block"),
    # The fourth caller of 821075E0 (IDA: RenderScriptDrawTessellatedFullscreenQuad 82786B60 its only caller); no
    # tessellated draw has a live witness (NATIVE_ROADMAP B4 / Q2). Mapped (native/construns-bake) so a census that
    # sees one names it: its packets would be the tessellated draw's own and its 821075E0 call's (p1.row3_construns'
    # rendered_tessellation; the construns bake grades it 0 in both rolls of its census pair).
    "820FC478": ("D3DDeviceEmitDrawTessellated", "p1.row3_construns:TESS",
                 "a tessellated draw (none live so far): its own packets and its 821075E0 call's"),
    # The occlusion queries (p2.zpd, native/zpd-bake 2026-10-16): D3DQueryIssueBeginEnd is the bracket entry whose scope
    # holds every query's packets - GpuEmitTileSurfaceConfig 82101E08's (RB_MODECONTROL r22, RB_SURFACE_INFO / COLOR_INFO
    # r20, RB_SAMPLE_COUNT_ADDR r23, EVENT_WRITE_ZPD 5B; type 9, BEGIN and END; the bake's port) and the type-10 screen-
    # extent query's own EVENT_WRITE 46 {0x19} / EVENT_WRITE_EXT 5A {0x1A, phys|1} (no port; ext=0 in every census so far).
    # s1c1 (exe 9ab92ab16131, chapter 18's landing) found it UNMAPPED with 5B:2,r20:2,r22:2,r23:2.
    "82102BC8": ("D3DQueryIssueBeginEnd (ZPD)", "p2.zpd",
                 "occlusion queries: 82101E08's packets (port: SR_NATIVE_ZPD, kRkZpd); the type-10 extent query's 46 / 5A"),
    # Title functions, not bracket entries: no census line carries them (their queries' packets sit on 82102BC8's line).
    # Kept as the callers' names (the HUD glow 82304058, the sun flare 82721118).
    "82721118": ("sun flare ZPD", "p2.zpd", "EVENT_WRITE_ZPD"),
    "82304058": ("HUD glow ZPD", "p2.zpd", "EVENT_WRITE_ZPD"),
}

# Key-level splits inside one producer's line (the census attributes by innermost TABLE scope; these writers are not
# scopes, so their packets sit on their caller's line):
#  * 820F0048 (the per-present re-emit, called at the end of 82112050): SET_BIN_MASK/SELECT 60-63, WAIT_UNTIL r05 and
#    RB_BC_CONTROL r0F, one each per call -> boot.device_create's per-frame half (SR_NATIVE_DEVCREATE). Its a2 == 0 branch
#    writes the four bin packets, then the type-0 WAIT_UNTIL 0x5C8 = 0x20000 and RB_BC_CONTROL 0xF01 (IDA
#    0x820F0098-0x820F0110: 6 packets a call). owner_census.py's rule (native/devcreate-boot) and the rcen1 gap map left
#    that r05 under VdSwap's block; the block (kernel/stubs.cpp VdSwap: the fetch run r48, XE_SWAP 64, the type-2 fill) is
#    SWAP's and writes no WAIT_UNTIL, nor does Swap's own body on its normal path (IDA 82112050) - r05 moved here
#    (native/transport-bake, 2026-10-15; the transport group is unchanged).
#  * 821075E0 GpuCmdEmitShaderConstantRuns (called from the draw flushes): its coherency preamble COHER_DEST_BASE_1
#    0x2007 (r20) + COHER_STATUS / SIZE / BASE (r0A x2) + WAIT_REG_MEM (3C) (kRkShaderConstSync's 13 dwords) and its
#    SQ_VS_CONST / SQ_PS_CONST run (r23) -> p1.row3_construns (SR_NATIVE_SHADER_CONST_RUNS, default ON since the bake);
#    and its alignment pad, o6 (native/flush-residue-2, Astra round 1): before each fast-path run 821075E0 stores a
#    type-2 NOP at cursor+4 and writes the run's header right after it when the cursor is 4 mod 8 (IDA 0x82107708 /
#    0x82107720). The census keys a NOP o6 only when its next dword is such a header (gfx/pm4_coverage.hpp
#    kConstRunPadKey: type-0, 4k registers inside [0x2388, 0x23A0), where no S1 leaf's run starts) - an address witness,
#    one per run by construction. Every other type-2 NOP stays o1, the producer's own: on an S1 line that is flush residue
#    (the S1 leaves' pads, an ambiguous NOP), and an exe without the witness (frs1, exe 2087ca4acdb4) prints only o1.
FLUSH_PAD_KEY = "o6"
KEY_SPLIT = {
    "82112050": {k: "p2.transport:DEVCREATE(820F0048)" for k in ("60", "61", "62", "63", "r05", "r0F")},
    "820FC000": {k: "p1.row3_construns" for k in ("r20", "r0A", "3C", "r23", FLUSH_PAD_KEY)},
    "820FBBF8": {k: "p1.row3_construns" for k in ("r20", "r0A", "3C", "r23", FLUSH_PAD_KEY)},
    "820FB6E8": {k: "p1.row3_construns" for k in ("r20", "r0A", "3C", "r23", FLUSH_PAD_KEY)},
}


# p1.row3_construns' SIGNATURE (native/construns-bake, 2026-10-14). Every SDK body of 821075E0 writes its 13-dword
# coherency preamble first, unconditionally, once per call (IDA 821075E0: COHER_DEST_BASE 0x2007 = r20, COHER r0A x2,
# WAIT_REG_MEM 3C - what kRkShaderConstSync replaces). The S1 flushes' own guest leaves (82107520 / 821078C0 / 82107760:
# type-0 runs, type-2 pads) write neither 3C nor r0A (rcen1 / frs1: 820FBBF8 none; 820FC000's 3C 3 : r0A 6 a present =
# exactly construns' three calls), but their register runs DO write r20 / r23 (0x2000 / 0x2300 / 0x2380: frs1 census0's
# 820FC000 r20 3.001, r23 3.001). So on the S1 producers' lines 3C + r0A count SDK-body preambles exactly (3 a call),
# while the r20 / r23 the KEY_SPLIT moves to construns may hold fallback runs; with the preamble at 0 no SDK body ran
# there and the r20 / r23 left are p1.flush_residue's. A tessellated draw (820FC478) is graded on its own line.
CONSTRUNS_SYNC_KEYS = ("3C", "r0A")
CONSTRUNS_RUN_KEYS = ("r20", "r23")
TESS_PRODUCER = "820FC478"


def construns_block(rows, nf):
    """Exact counts over the interval: sync_pk (3C + r0A on the S1 producers: 3 per SDK-body call), runs_pk (r20 + r23
    there: construns' runs and the S1 fallbacks' together), tess_pk (every packet on 820FC478's line), and
    unattributed_sync_pk: 3C + r0A on the UNSTAMPED / RING_UNSTAMPED lines (Astra round 1 on native/construns-bake, P2: a
    preamble whose attribution was lost would leave the S1 lines and land there, so a census that only read the S1 lines
    could pass with SDK bodies still running; the ordinary unstamped packets - boot's ME_INIT, the device-less create
    bracket, the pump's bin selects 60-63 - carry neither key)."""
    sync = runs = tess = unattributed = 0
    for r in rows:
        if r["producer"] == TESS_PRODUCER:
            tess += r["pk"]
        elif r["producer"] in UNATTRIBUTED:
            unattributed += sum(r["keys_abs"].get(k, 0) for k in CONSTRUNS_SYNC_KEYS)
        elif "3C" in KEY_SPLIT.get(r["producer"], {}):  # the S1 producers (820FBBF8 / 820FC000 / 820FB6E8)
            sync += sum(r["keys_abs"].get(k, 0) for k in CONSTRUNS_SYNC_KEYS)
            runs += sum(r["keys_abs"].get(k, 0) for k in CONSTRUNS_RUN_KEYS)
    per = (lambda v: round(v / nf, 3)) if nf else (lambda v: 0.0)
    return {"sync_pk": sync, "sync_per_frame": per(sync), "sdk_calls_per_frame": per(sync / 3.0),
            "runs_pk": runs, "runs_per_frame": per(runs), "tess_pk": tess, "unattributed_sync_pk": unattributed}


def split_of(r):
    """KEY_SPLIT for one producer row."""
    return KEY_SPLIT.get(r["producer"], {})


# The transport group's signature keys, forbidden on every other producer's line under --transport-zero.
TRANSPORT_KEYS = ("54", "58", "64", "3F", "37", "r04")
UNATTRIBUTED = ("UNSTAMPED", "RING_UNSTAMPED")
# --pm4-zero --allow-devcreate-reemit: 820F0048's per-present re-emit under Swap (KEY_SPLIT's DEVCREATE keys: 60 / 61 / 62
# / 63, r05, r0F), the one packet set a default path with SR_NATIVE_DEVCREATE off keeps in gameplay - one of each key a
# present at most
REEMIT_PRODUCER = "82112050"
REEMIT_KEYS = tuple(KEY_SPLIT[REEMIT_PRODUCER])
# --pm4-zero --base-shows zpd (p2.zpd's bake, native/zpd-bake): the base arm (SR_NATIVE_ZPD=0) shows the HUD glow queries
# as guest PM4 - EVENT_WRITE_ZPD (5B) on D3DQueryIssueBeginEnd_82102BC8's line (the bracket entry whose scope holds
# 82101E08's packets; the title callers are no entries) - and the zpd group > 0
ZPD_PRODUCER = "82102BC8"
ZPD_KEY = "5B"


def reemit_attribution(frames, ing, reports, allow):
    """THE ALLOWANCE'S ATTRIBUTION (Astra round 1 on native/transport-bake, P2: the allowance was subtracted from every
    gameplay F line, but only the interval (A, B] checked which producer wrote it - a BlockUntilIdle r05 before A, or
    five after B, passed). The producer lines are cumulative per report, so every report segment (R_prev, R] (boot ..
    the first report included) attributes its packets exactly. For each segment holding a gameplay frame (`ing`), per
    allowed key k: foreign_k = every producer line's delta of k less 82112050's (the re-emit's own line), placed
    conservatively - never assumed to be before the ingame marker unless it must be:
      * beyond the segment's pre-marker k packets (P_k) it is in gameplay for certain (`reemit_foreign_pk`: FAIL);
      * the rest, up to the segment's gameplay k packets (G_k), cannot be placed (`reemit_unverified_pk`:
        INCONCLUSIVE) - a segment wholly in gameplay has P_k = 0, so all of its foreign_k is a FAIL;
      * a segment whose F lines' k sum differs from its producer lines' (the census does not add up there) cannot
        attribute its G_k either (unverified).
    The frames after the final report (no exit report without the graceful quit: the harness kills the game) have no
    producer line at all: every allowed packet there is unverified (--graceful-quit's terminal report counts the
    drained tail frame, so a graceful roll has none). -> the block's attribution fields."""
    ing_set = set(ing)
    seg_out, foreign, unverified = [], 0, 0
    prev = {"frames": 0, "inner": {}}
    for rep in reports:
        lo, hi = prev["frames"], rep["frames"]
        nums = [n for n in range(lo + 1, hi + 1) if n in frames]
        if not any(n in ing_set for n in nums):
            prev = rep
            continue
        tot, own = {}, {}
        for (va, nm), v in rep["inner"].items():
            p = prev["inner"].get((va, nm), {"keys": {}})
            for k in allow:
                d = v["keys"].get(k, 0) - p["keys"].get(k, 0)
                tot[k] = tot.get(k, 0) + d
                if va == REEMIT_PRODUCER:
                    own[k] = own.get(k, 0) + d
        for (va, nm), p in prev["inner"].items():  # a line that vanished (never in a census; counts never fall)
            if (va, nm) not in rep["inner"]:
                for k in allow:
                    tot[k] = tot.get(k, 0) - p["keys"].get(k, 0)
                    if va == REEMIT_PRODUCER:
                        own[k] = own.get(k, 0) - p["keys"].get(k, 0)
        row = {"from_frames": lo, "to_frames": hi, "ts": rep.get("ts"), "foreign": {}, "unverified": {}}
        for k in allow:
            fk = tot.get(k, 0) - own.get(k, 0)
            p_all = sum(frames[n]["keys"].get(k, 0) for n in nums if n not in ing_set)
            g = sum(frames[n]["keys"].get(k, 0) for n in nums if n in ing_set)
            if p_all + g != tot.get(k, 0):
                if g:
                    row["unverified"][k] = g
                    unverified += g
                continue
            if fk <= 0:
                continue
            definite = max(0, fk - p_all)
            amb = max(0, min(fk, g) - definite)
            if definite:
                row["foreign"][k] = definite
                foreign += definite
            if amb:
                row["unverified"][k] = amb
                unverified += amb
        if row["foreign"] or row["unverified"]:
            seg_out.append(row)
        prev = rep
    last = reports[-1]["frames"] if reports else 0
    tail = [n for n in ing if n > last]
    tail_pk = sum(frames[n]["keys"].get(k, 0) for n in tail for k in allow)
    unverified += tail_pk
    return {"reemit_foreign_pk": foreign, "reemit_unverified_pk": unverified, "reemit_segments": seg_out[:8],
            "attributed_through_frames": last, "unattributed_tail_frames": len(tail),
            "unattributed_tail_reemit_pk": tail_pk}


def pm4_zero_block(frames, t_in, rows, nf, allow, reports=(), tails=()):
    """--pm4-zero's two views of a roll (module doc, THE DEFAULT PATH'S PM4 ZERO): the F lines from the ingame marker
    (t_in) to the run's last frame, and the producer lines over the census interval. `allow` = the re-emit keys a
    frame / 82112050's line may hold (() = none); with an allowance, `reports` (the census reports in order) attribute
    it over the whole gameplay span (reemit_attribution) and every gameplay frame holds no re-emit key or exactly one
    WHOLE re-emit (`ingame_frames_partial` / `ingame_frames_over`; `tails` = the terminal snapshot's partial frame,
    whose drained work may stop inside one, exempt - its report attributes it)."""
    ing = [n for n in sorted(frames) if frames[n]["ts"] and parse_ts(frames[n]["ts"]) >= t_in]
    pk = other = allowed = over = partial = 0
    other_keys, first_bad = {}, None
    for n in ing:
        fr = frames[n]
        a = sum(fr["keys"].get(k, 0) for k in allow)
        o = fr["pk"] - a
        pk += fr["pk"]
        allowed += a
        other += o
        bad = o != 0
        if any(fr["keys"].get(k, 0) > 1 for k in allow):
            over += 1
            bad = True
        elif a and n not in tails and any(fr["keys"].get(k, 0) != 1 for k in allow):
            # a re-emit key without the rest of 820F0048's six (one of each, written together a present): not the
            # re-emit - a BlockUntilIdle / declined resolve WAIT_UNTIL r05, the pump's bin selects 60-63
            partial += 1
            bad = True
        if o:
            for k, v in fr["keys"].items():
                if k not in allow:
                    other_keys[k] = other_keys.get(k, 0) + v
            keyed = sum(v for k, v in fr["keys"].items() if k not in allow)
            if o > keyed:  # packets with no ops / regs key: type-2 pads, type-1 (the F line's pk counts them)
                other_keys["unkeyed"] = other_keys.get("unkeyed", 0) + (o - keyed)
        if bad and first_bad is None:
            first_bad = n
    iv_other, iv_left, reemit = 0, {}, 0
    for r in rows:
        ok = sum(r["keys_abs"].get(k, 0) for k in allow) if r["producer"] == REEMIT_PRODUCER else 0
        reemit += ok
        rest = r["pk"] - ok
        if rest:
            iv_other += rest
            iv_left[r["producer"]] = rest
    out = {"allow": list(allow), "ingame_frames": len(ing), "ingame_pk": pk, "ingame_other_pk": other,
           "ingame_other_keys": dict(sorted(other_keys.items())), "ingame_allowed_pk": allowed,
           "ingame_frames_over": over, "ingame_frames_partial": partial, "ingame_first_bad_frame": first_bad,
           "interval_other_pk": iv_other, "interval_left": dict(sorted(iv_left.items())),
           "reemit_pk": reemit, "reemit_per_frame": round(reemit / nf, 3) if nf else 0.0}
    out.update(reemit_attribution(frames, ing, list(reports), allow) if allow else
               {"reemit_foreign_pk": 0, "reemit_unverified_pk": 0, "reemit_segments": [],
                "attributed_through_frames": reports[-1]["frames"] if reports else 0, "unattributed_tail_frames": 0,
                "unattributed_tail_reemit_pk": 0})
    return out


def group_of(obligation):
    """The dot-free group of an obligation string (the ledger reads groups.<name>)."""
    if obligation.startswith("p2.transport"):
        return "transport"
    if obligation.startswith("p1.row3_construns"):
        return "construns"
    return {"p1.flush_residue": "flush_residue", "p1.row3_construns": "construns", "p2.setconst_mem": "setconst",
            "p2.zpd": "zpd", "UNMAPPED": "unmapped"}.get(obligation, "other")


def groups(rows):
    """Exact packet counts per group over the interval (key splits applied) and the transport detail."""
    cnt = {}
    transport, foreign = {}, {}
    for r in rows:
        if r["producer"] in UNATTRIBUTED:
            cnt["unattributed"] = cnt.get("unattributed", 0) + r["pk"]
            continue
        split = split_of(r)
        moved = 0
        for k, v in r["keys_abs"].items():
            if k in split:
                g = group_of(split[k])
                cnt[g] = cnt.get(g, 0) + v
                moved += v
                if g == "transport":
                    transport[r["producer"]] = transport.get(r["producer"], 0) + v
        g = group_of(r["obligation"])
        cnt[g] = cnt.get(g, 0) + r["pk"] - moved
        if g == "transport":
            transport[r["producer"]] = transport.get(r["producer"], 0) + r["pk"] - moved
        else:
            sig = {k: v for k, v in r["keys_abs"].items() if k in TRANSPORT_KEYS and split.get(k, "")[:12] != "p2.transport"}
            if sig:
                foreign[r["producer"]] = sig
    return cnt, transport, foreign


def by_obligation(rows):
    """Per-frame packets by obligation, the key-level splits applied."""
    out = {}
    for r in rows:
        split = split_of(r)
        # from the exact interval counts (keys_per_frame is rounded: a fully split line would leave -0.00 behind)
        scale = r["per_frame"] / r["pk"] if r.get("pk") else 0.0
        keys = {k: v * scale for k, v in r["keys_abs"].items()} if "keys_abs" in r else r["keys_per_frame"]
        moved = 0.0
        for k, v in keys.items():
            if k in split:
                out[split[k]] = out.get(split[k], 0.0) + v
                moved += v
        rest = r["per_frame"] - moved
        if abs(rest) > 1e-9:
            out[r["obligation"]] = out.get(r["obligation"], 0.0) + rest
    return {k: round(v, 3) for k, v in sorted(out.items(), key=lambda kv: -kv[1])}


def parse_ts(s):
    return datetime.datetime.strptime(s, "%Y-%m-%d %H:%M:%S.%f")


def run_log(roll):
    return roll if os.path.isfile(roll) else os.path.join(roll, "drive", "attempt1", "run.log")


def parse_keys(text):
    out = {}
    for kv in text.split(",") if text else []:
        k, _, v = kv.partition(":")
        out[k] = int(v)
    return out


def parse_frame_keys(ops, regs):
    """F / CUM ops XX:n -> XX, regs XX:pk/dw -> rXX (packets); None when any entry is malformed."""
    out = {}
    for field, page in ((ops, False), (regs, True)):
        for kv in field.split(",") if field else []:
            m = (REG_KV if page else OP_KV).match(kv)
            if not m:
                return None
            out[("r" if page else "") + m.group(1)] = int(m.group(2))
    return out


def ingame_ts(log):
    """The ingame marker's wall time (local, naive), else the gameplay marker's; (era, datetime) or (None, None)."""
    found = perf_report.log_markers(log, perf_report.DEFAULT_ERAS, 0.0, None)
    for want in ("ingame", "gameplay"):
        for era, t_us in found:
            if era == want and t_us > 0:
                return want, datetime.datetime.fromtimestamp(t_us / 1e6)
    return None, None


def read_roll(roll):
    """One pass: report blocks [{ts, ln, frames, keys (CUM), ends (its ENDS line), inner {(va,name): {pk, keys}},
    owner {...}}], F records, and the coverage witness (the armed line, every [GPU_CP] present count with the F and
    tail lines before it, the record-present evidence, whether the counter ran at all)."""
    reports, cur = [], None
    frames, bad, last_ts = {}, [], None
    cov = {"armed": None, "f_lines": 0, "tails": 0, "cp": [], "ends": None, "swapblock_lines": 0, "swapblock_applied": 0,
           "ran": False, "tail_frames": []}
    with open(run_log(roll), encoding="utf-8", errors="replace") as f:
        for ln, line in enumerate(f):
            t = TS.match(line)
            if t:
                last_ts = t.group(1)
            if "[GPU_CP] " in line or "[NATIVE_SUBMIT] " in line:
                g = CPSWAPS.search(line)
                if g:
                    cov["cp"].append({"why": g.group(1), "swaps": int(g.group(2)), "f_before": cov["f_lines"],
                                      "tails_before": cov["tails"], "ts": t.group(1) if t else None, "ln": ln})
                continue
            if "[NATIVE_FLUSH] " in line:
                s = SWAPBLOCK.search(line)
                if s:  # printed periodically whenever SR_NATIVE_SWAP_BLOCK is on, 0 calls included
                    cov["swapblock_lines"] += 1
                    cov["swapblock_applied"] = max(cov["swapblock_applied"], int(s.group(1)))
                continue
            if COUNTER_RAN.search(line):
                cov["ran"] = True
            if "[PM4_COV" not in line:
                continue
            if FRAME_ANY.search(line):
                cov["f_lines"] += 1
            tl = TAIL.search(line)
            if tl:
                cov["tails"] += 1
                cov["tail_frames"].append(int(tl.group(1)))
                continue
            a = ARMED.search(line)
            if a:
                cov["armed"] = int(a.group(1))
                continue
            e = ENDS.search(line)
            if e:
                cov["ends"] = {"why": e.group(1), "frames": int(e.group(2)), "xeSwap": int(e.group(3)),
                               "present": int(e.group(4)), "presents": int(e.group(5)), "tails_before": cov["tails"]}
                if cur is not None and cur["why"] == e.group(1) and cur["ends"] is None:
                    cur["ends"] = cov["ends"]
                continue
            c = CUM.search(line)
            if c:
                keys = parse_frame_keys(c.group(7), c.group(8))
                cur = {"ts": t.group(1) if t else None, "why": c.group(1), "frames": int(c.group(2)),
                       "pk": int(c.group(3)), "acc": int(c.group(4)), "unst": int(c.group(5)),
                       "chain": int(c.group(6) or 0), "keys": keys, "inner": {}, "owner": {}, "census": False,
                       "ln": ln, "ends": None}
                reports.append(cur)
                continue
            m = MAP.search(line)
            if m and cur is not None and m.group(1) == cur["why"]:
                va, nm = m.group(3).upper(), m.group(4)
                cur[m.group(2)][(va, nm)] = {"pk": int(m.group(5)), "dw": int(m.group(6)), "keys": parse_keys(m.group(7))}
                continue
            o = ORIGIN.search(line)
            if o and cur is not None and o.group(1) == cur["why"]:
                cur["census"] = True
                continue
            loose = FRAME_ANY.search(line)
            if loose:
                n = int(loose.group(1))
                fm = FLINE.search(line)
                keys = parse_frame_keys(fm.group(11), fm.group(12)) if fm else None
                if fm is None or keys is None or n in frames:
                    bad.append(n)
                    continue
                frames[n] = {"ts": t.group(1) if t else None, "pk": int(fm.group(2)), "t0": int(fm.group(3)),
                             "t3": int(fm.group(4)), "pred": int(fm.group(5)), "ringIb": int(fm.group(6)),
                             "d0": int(fm.group(7)), "dN": int(fm.group(8)), "acc": int(fm.group(9)),
                             "unst": int(fm.group(10)), "keys": keys}
    return reports, frames, bad, last_ts, cov


def ends_broken(reports):
    """The ENDS lines' accounting (module doc, COVERAGE WITNESS (2)), each report in order: every frame end a packet's
    or a present's (frames - tails = xeSwap + present) and every frame end matched by a present the counter saw
    (presents = xeSwap + present). One present may be pending: packet3 ends an XE_SWAP's frame - and prints the report
    - before the CP applies that packet's present (a record's present is counted before its frame ends), so a report
    with an XE_SWAP frame end since the one before may read presents = xeSwap + present - 1; a terminal report (the CP
    parked) and a report after record frame ends only must be exact. The broken reports' ENDS dicts."""
    out, prev_x = [], 0
    for r in reports:
        e = r["ends"]
        if e is None:
            continue
        ended = e["xeSwap"] + e["present"]
        pending = e["why"] != "terminal" and e["xeSwap"] > prev_x
        if not (e["frames"] - e["tails_before"] == ended and
                (e["presents"] == ended or (pending and e["presents"] == ended - 1))):
            out.append(dict(e, report_frames=r["frames"]))
        prev_x = e["xeSwap"]
    return out


def coverage_state(cov, reports):
    """The COVERAGE WITNESS (module doc): (state, why, summary). state live / dark / surplus / unaccounted /
    unwitnessed / unchecked; summary["verified_ln"] = the last reconciled CP sample's line (a report before it is
    verified)."""
    cp = cov["cp"]
    for r in cp:
        r["present_ends"] = r["f_before"] - r["tails_before"]  # F lines that are frame ends of presents
    behind = [r for r in cp if r["present_ends"] < r["swaps"]]
    ahead = [r for r in cp if r["present_ends"] > r["swaps"]]
    last = cp[-1] if cp else None
    witnessed = cov["armed"] is not None
    no_ends = [r["frames"] for r in reports if r["ends"] is None] if witnessed else []
    broken = ends_broken(reports)
    summary = {"armed": witnessed, "brackets": cov["armed"], "f_lines": cov["f_lines"], "tails": cov["tails"],
               "cp_lines": len(cp), "cp_presents": last["swaps"] if last else 0,
               "f_before_last_cp": last["f_before"] if last else 0, "cp_lines_behind": len(behind),
               "cp_lines_ahead": len(ahead), "first_behind": behind[0] if behind else None,
               "first_ahead": ahead[0] if ahead else None, "ends": cov["ends"], "reports_without_ends": no_ends[:8],
               "ends_broken": broken[:4], "swapblock_lines": cov["swapblock_lines"],
               "swapblock_applied": cov["swapblock_applied"], "counter_ran": cov["ran"],
               "witness": "armed" if witnessed else ("F-track" if cov["f_lines"] else None),
               "verified_ln": last["ln"] if last else None}
    if not last or last["swaps"] == 0:
        state, why = "unchecked", ("no [GPU_CP] census line with a present: nothing to check the census against "
                                   "(a census roll must present frames)")
    elif not witnessed and cov["f_lines"] == 0 and not cov["ran"]:
        state, why = "unwitnessed", (
            "coverage OFF: no [PM4_COV] armed witness and no line of the counter's while the CP presented %d frames - "
            "not a census" % last["swaps"])
    elif behind:
        b = behind[0]
        state, why = "dark", (
            "coverage DARK: the CP presented %d frames by its [GPU_CP] %s line (%s) with %d present frame end(s) "
            "([PM4_COV] F lines less tail frames) before it; %d of %d CP census lines behind, the last %d presents / %d "
            "- the counter ran without a frame boundary: no data, never 0 packets" % (
                b["swaps"], b["why"], b["ts"], b["present_ends"], len(behind), len(cp), last["swaps"],
                last["present_ends"]))
    elif ahead:
        a = ahead[0]
        state, why = "surplus", (
            "coverage SURPLUS: %d present frame ends before the [GPU_CP] %s line (%s) that counts %d presents - frames "
            "ended with no present (an XE_SWAP without its signature?): the frame boundaries do not reconcile" % (
                a["present_ends"], a["why"], a["ts"], a["swaps"]))
    elif no_ends:
        state, why = "unaccounted", (
            "coverage UNACCOUNTED: %d report(s) without their [PM4_COV] ENDS line on an exe with the armed witness "
            "(first at frames=%d): the frame ends are not accounted for" % (len(no_ends), no_ends[0]))
    elif broken:
        e = broken[0]
        state, why = "unaccounted", (
            "coverage UNACCOUNTED: the ENDS line of the report at frames=%d reads frames=%d (tails %d) xeSwap=%d "
            "present=%d presents=%d - a frame end with no present, or a present that ended no frame" % (
                e["report_frames"], e["frames"], e["tails_before"], e["xeSwap"], e["present"], e["presents"]))
    elif not witnessed and cov["swapblock_lines"]:
        state, why = "unaccounted", (
            "coverage UNACCOUNTED: an exe without the ENDS line with SR_NATIVE_SWAP_BLOCK on (%d [NATIVE_FLUSH] swapBlock "
            "lines, %d record presents): frame ends with no present and presents with no frame end could cancel at "
            "every CP sample" % (cov["swapblock_lines"], cov["swapblock_applied"]))
    else:
        state, why = "live", "%d present frame ends equal the CP's presents at all %d [GPU_CP] lines (last %d; %s)" % (
            last["present_ends"], len(cp), last["swaps"], "ENDS accounted" if witnessed else
            "F-track, no record present")
    summary["state"], summary["why"] = state, why
    return state, why, summary


def producer_key(va, name):
    if va == "00000000":
        return name  # UNSTAMPED / RING_UNSTAMPED share the VA
    return va


def delta(b, a):
    out = dict(b)
    for k, v in a.items():
        out[k] = out.get(k, 0) - v
    return {k: v for k, v in out.items() if v}


def analyse(roll, top=0, allow=()):
    """One roll's census over its interval; `allow` = the re-emit keys --pm4-zero lets through (pm4_zero_block)."""
    log = run_log(roll)
    if not os.path.exists(log):
        return {"roll": roll, "error": "no run.log"}
    reports, frames, bad, last_ts, cov = read_roll(roll)
    state, why, coverage = coverage_state(cov, reports)
    if state != "live":  # checked first: a dark counter prints no report either, and its cause is the one to name
        return {"roll": roll, "error": why, "coverage": coverage}
    reports = [r for r in reports if r["census"]]
    if not reports:
        return {"roll": roll, "coverage": coverage,
                "error": "no producer census ([PM4_COVMAP] internalOrigin lines: SR_NATIVE_BRACKETS=1)"}
    final = reports[-1]
    # B = the last report a reconciled CP sample comes after (COVERAGE WITNESS (4)): a report after the last sample is
    # not verified - a present after that sample could have missed its frame end unseen
    verified = [r for r in reports if r["ln"] < coverage["verified_ln"]]
    if not verified:
        coverage["state"] = "unverified"
        coverage["why"] = ("coverage UNVERIFIED: no census report before the last reconciled [GPU_CP] sample (%d presents)"
                           % coverage["cp_presents"])
        return {"roll": roll, "error": coverage["why"], "coverage": coverage}
    B = verified[-1]
    coverage["verified_report_frames"], coverage["final_report_frames"] = B["frames"], final["frames"]
    era, t_in = ingame_ts(log)
    if t_in is None:
        return {"roll": roll, "error": "no ingame / gameplay marker", "coverage": coverage}
    A = next((r for r in reports if r["ts"] and parse_ts(r["ts"]) >= t_in), None)
    if A is None or A is B or B["frames"] <= A["frames"] or A["ln"] > B["ln"]:
        return {"roll": roll, "error": "fewer than two verified census reports in the ingame era", "coverage": coverage}
    nf = B["frames"] - A["frames"]
    # producers: the delta of each inner= line over (A, B]
    prod = {}
    for (va, nm), v in B["inner"].items():
        a = A["inner"].get((va, nm), {"pk": 0, "dw": 0, "keys": {}})
        d = v["pk"] - a["pk"]
        if d:
            k = producer_key(va, nm)
            prod[k] = {"va": va, "name": nm, "pk": d, "dw": v["dw"] - a["dw"], "keys": delta(v["keys"], a["keys"])}
    for (va, nm) in A["inner"]:
        if (va, nm) not in B["inner"]:
            prod.setdefault(producer_key(va, nm), {"va": va, "name": nm, "pk": 0, "dw": 0, "keys": {},
                                                   "vanished": True})
    # F records over (A, B] and the run's completeness
    last_frame = max(frames) if frames else 0
    missing = [n for n in range(1, last_frame + 1) if n not in frames]
    win = [frames[n] for n in range(A["frames"] + 1, B["frames"] + 1) if n in frames]
    fsum, fpk, fzero = {}, 0, 0
    for fr in win:
        fpk += fr["pk"]
        fzero += fr["pk"] == 0
        for k, v in fr["keys"].items():
            fsum[k] = fsum.get(k, 0) + v
    cum_d = delta(B["keys"] or {}, A["keys"] or {})
    psum = {}
    for p in prod.values():
        for k, v in p["keys"].items():
            if k.startswith("o"):
                continue  # the census's other-class keys have no F / CUM counterpart
            psum[k] = psum.get(k, 0) + v
    keys = sorted(set(fsum) | set(cum_d) | set(psum))
    mism = {k: {"F": fsum.get(k, 0), "CUM": cum_d.get(k, 0), "producers": psum.get(k, 0)}
            for k in keys if not (fsum.get(k, 0) == cum_d.get(k, 0) == psum.get(k, 0))}
    tail_frames = last_frame - B["frames"]
    tail_s = (parse_ts(last_ts) - parse_ts(B["ts"])).total_seconds() if last_ts and B["ts"] else None
    # the census ran to the end: its FINAL report within one reporting period of the run's end, and B (the verified
    # report) within one reporting period plus one CP sampling period of the end
    final_tail_frames = last_frame - final["frames"]
    final_tail_s = (parse_ts(last_ts) - parse_ts(final["ts"])).total_seconds() if last_ts and final["ts"] else None
    rows = []
    for k, p in sorted(prod.items(), key=lambda kv: -kv[1]["pk"]):
        m = PRODUCER_MAP.get(k)
        rows.append({"producer": k, "name": p["name"], "per_frame": p["pk"] / nf, "dw_per_frame": p["dw"] / nf,
                     "pk": p["pk"], "keys_per_frame": {kk: round(vv / nf, 3) for kk, vv in sorted(p["keys"].items())},
                     "keys_abs": dict(sorted(p["keys"].items())),
                     "obligation": m[1] if m else "UNMAPPED", "why": m[2] if m else ""})
    gcnt, gtrans, gforeign = groups(rows)
    # p1.flush_residue FROM BOOT (Astra round 1 on native/flush-residue-2): B's producer lines are cumulative from boot,
    # and counts never fall, so a flush_residue packet in any earlier report (the opening, before the interval) is in B
    boot_rows, boot_keys = [], {}
    for (va, nm), v in B["inner"].items():
        k = producer_key(va, nm)
        m = PRODUCER_MAP.get(k)
        r = {"producer": k, "pk": v["pk"], "keys_abs": dict(v["keys"]), "obligation": m[1] if m else "UNMAPPED"}
        boot_rows.append(r)
        if group_of(r["obligation"]) == "flush_residue":
            left = {kk: n for kk, n in r["keys_abs"].items() if kk not in split_of(r) and n}
            if left:
                boot_keys[k] = left
    bcnt, _, _ = groups(boot_rows)
    return {
        "roll": roll, "era": era, "era_start": t_in.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3], "coverage": coverage,
        "interval": {"A_frames": A["frames"], "A_ts": A["ts"], "B_frames": B["frames"], "B_ts": B["ts"], "frames": nf},
        "per_frame": {"pk": fpk / nf if nf else 0.0, "cum_pk": (B["pk"] - A["pk"]) / nf,
                      "acc": (B["acc"] - A["acc"]) / nf, "unst": (B["unst"] - A["unst"]) / nf,
                      "d0": sum(fr["d0"] for fr in win) / nf, "dN": sum(fr["dN"] for fr in win) / nf,
                      "keys": {k: round(v / nf, 3) for k, v in sorted(fsum.items())}},
        "frames_zero": fzero, "frames_in_window": len(win),
        # the uncovered tail (after B): unattributed, but its F lines still count for [PM4_COV] 0
        "tail_nonzero": sum(1 for n in range(B["frames"] + 1, last_frame + 1) if n in frames and frames[n]["pk"]),
        "producers": rows,
        "by_obligation": by_obligation(rows),
        "per_producer": {r["producer"]: round(r["per_frame"], 3) for r in rows},  # dot-free keys for ledger checks
        "group_pk": dict(sorted(gcnt.items())),
        "groups": {g: round(v / nf, 3) for g, v in sorted(gcnt.items())},
        "transport": {"pk": gcnt.get("transport", 0), "per_frame": round(gcnt.get("transport", 0) / nf, 3),
                      "producers": {k: round(v / nf, 3) for k, v in sorted(gtrans.items())},
                      "foreign_signature_keys": gforeign},
        # p1.flush_residue's group, always written (group_pk holds only the groups that occur: a zero group is absent)
        "flush_residue": {"pk": gcnt.get("flush_residue", 0),
                          "per_frame": round(gcnt.get("flush_residue", 0) / nf, 3) if nf else 0.0},
        # p1.row3_construns' group (KEY_SPLIT) and its signature counts, always written (native/construns-bake)
        "construns": dict({"pk": gcnt.get("construns", 0),
                           "per_frame": round(gcnt.get("construns", 0) / nf, 3) if nf else 0.0},
                          **construns_block(rows, nf)),
        # the same group over boot .. B (B's cumulative lines), with the residue keys per producer
        "flush_residue_from_boot": {"pk": bcnt.get("flush_residue", 0), "keys": boot_keys},
        # --pm4-zero's two views (native/transport-bake): every F line from the ingame marker on, every producer line
        # over the interval; always written
        "pm4_zero": pm4_zero_block(frames, t_in, rows, nf, tuple(allow), reports=reports,
                                   tails=set(cov["tail_frames"])),
        "checks": {
            "coverage_live": coverage["state"] == "live",
            "frames_complete": not missing and not bad,
            "frames_missing": len(missing), "frames_bad": len(bad),
            "window_complete": len(win) == nf,
            "covers_run": (final_tail_frames < REPORT_FRAMES and final_tail_s is not None and
                           final_tail_s <= REPORT_SECONDS + SLACK_S and
                           tail_frames < REPORT_FRAMES + CP_SAMPLE_PRESENTS),
            "tail_frames": tail_frames, "tail_seconds": tail_s,
            "final_tail_frames": final_tail_frames, "final_tail_seconds": final_tail_s,
            "reconciled": not mism, "keys_compared": len(keys), "mismatched": mism,
            "pk_reconciled": fpk == B["pk"] - A["pk"] == sum(p["pk"] for p in prod.values()),
            "unmapped": [r["producer"] for r in rows if r["obligation"] == "UNMAPPED"],
        },
    }


def show(res, title):
    if "error" in res:
        print("%s: %s (%s)" % (title, res["error"], res["roll"]))
        return
    iv, pf, ch = res["interval"], res["per_frame"], res["checks"]
    print("== %s: %s" % (title, res["roll"]))
    cv = res["coverage"]
    print("   coverage %s: %s; ends %s" % (cv["state"], cv["why"], cv["ends"]))
    print("   %s from %s; census interval frames (%d, %d] = %d presents (%s .. %s)" % (
        res["era"], res["era_start"], iv["A_frames"], iv["B_frames"], iv["frames"], iv["A_ts"], iv["B_ts"]))
    print("   pk/frame %.2f (CUM %.2f) acc %.2f unst %.2f d0 %.2f dN %.2f; zero-pk frames %d of %d" % (
        pf["pk"], pf["cum_pk"], pf["acc"], pf["unst"], pf["d0"], pf["dN"], res["frames_zero"], res["frames_in_window"]))
    print("   checks: frames_complete=%s window_complete=%s covers_run=%s (tail %d frames, %.1f s after B; %d frames, "
          "%.1f s after the final report at %d) reconciled=%s pk_reconciled=%s unmapped=%s" % (
              ch["frames_complete"], ch["window_complete"], ch["covers_run"], ch["tail_frames"],
              ch["tail_seconds"] or -1, ch["final_tail_frames"], ch["final_tail_seconds"] or -1,
              cv.get("final_report_frames", -1), ch["reconciled"], ch["pk_reconciled"], ch["unmapped"]))
    print("   %-9s %-46s %8s %8s  %-36s keys/frame" % ("producer", "name", "pk/fr", "dw/fr", "obligation"))
    for r in res["producers"]:
        ks = ",".join("%s:%g" % (k, v) for k, v in r["keys_per_frame"].items())
        print("   %-9s %-46s %8.2f %8.1f  %-36s %s" % (r["producer"][:9], r["name"][:46], r["per_frame"],
                                                      r["dw_per_frame"], r["obligation"][:36], ks))
    print("   by obligation (key splits applied): " +
          "; ".join("%s %.2f" % (k, v) for k, v in res["by_obligation"].items()))
    print("   groups per present: %s; transport %d packets (%s); foreign transport keys %s" % (
        res["groups"], res["transport"]["pk"], res["transport"]["producers"], res["transport"]["foreign_signature_keys"]))
    fb = res["flush_residue_from_boot"]
    print("   flush_residue from boot (the last report, cumulative): %d packets %s" % (fb["pk"], fb["keys"]))
    z = res["pm4_zero"]
    print("   pm4 from the ingame marker: %d frames, %d packets, %d outside the allowance %s (first frame %s, frames "
          "over %d, partial %d); interval producers outside it: %d %s; re-emit %d (%.3f a frame)" % (
              z["ingame_frames"], z["ingame_pk"], z["ingame_other_pk"], z["ingame_other_keys"],
              z["ingame_first_bad_frame"], z["ingame_frames_over"], z["ingame_frames_partial"], z["interval_other_pk"],
              z["interval_left"], z["reemit_pk"], z["reemit_per_frame"]))
    if z["allow"]:
        print("   re-emit attribution through frame %d (the final report): foreign %d, unverified %d (%d in the %d "
              "frames after it); segments %s" % (
                  z["attributed_through_frames"], z["reemit_foreign_pk"], z["reemit_unverified_pk"],
                  z["unattributed_tail_reemit_pk"], z["unattributed_tail_frames"], z["reemit_segments"]))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("roll", nargs="?")
    ap.add_argument("--base")
    ap.add_argument("--target")
    ap.add_argument("--json")
    ap.add_argument("--strict-zero", action="store_true",
                    help="the target's census interval must hold 0 packets ([PM4_COV] 0) and no producer line")
    ap.add_argument("--transport-zero", action="store_true",
                    help="p2.transport's observation: the target's transport group 0, nothing unattributed or unmapped, "
                         "no transport signature key on another producer; the base shows the transport group")
    ap.add_argument("--flush-residue-zero", action="store_true",
                    help="p1.flush_residue's observation: the target's flush_residue group 0 packets over its census "
                         "interval (the S1 flushes' scope less the construns keys, row 9's declines); the base shows it")
    ap.add_argument("--construns-zero", action="store_true",
                    help="p1.row3_construns' observation (the construns bake): the target's S1 producer lines hold no "
                         "821075E0 SDK-body preamble (3C + r0A = 0) over its census interval and the base's do; no "
                         "tessellated draw (820FC478) in either roll")
    ap.add_argument("--pm4-zero", action="store_true",
                    help="the transport bake's default path: the target holds NO packet in gameplay - every F line "
                         "from the ingame marker to the run's end at pk=0 and no producer line over the interval; the "
                         "base shows the transport group")
    ap.add_argument("--base-shows", choices=("transport", "devcreate", "zpd"), default="transport",
                    help="with --pm4-zero: what the base arm must show - the transport group (the transport bake's pair), "
                         "820F0048's re-emit, all six keys on 82112050's line (the devcreate bake's pair, "
                         "SR_NATIVE_DEVCREATE=0 vs the default: the transport is native in both arms), or the HUD glow "
                         "queries as guest PM4 - EVENT_WRITE_ZPD (5B) on 82102BC8's line and the zpd group > 0 (the zpd "
                         "bake's pair, SR_NATIVE_ZPD=0 vs the default: the transport and devcreate native in both arms)")
    ap.add_argument("--allow-devcreate-reemit", action="store_true",
                    help="with --pm4-zero: 820F0048's per-present re-emit under Swap (82112050: 60 / 61 / 62 / 63, r05, r0F, "
                         "none or one whole re-emit a frame and nothing else, attributed to 82112050 over the whole "
                         "gameplay span; unattributed allowed packets: INCONCLUSIVE, exit 3) is the one exception - "
                         "SR_NATIVE_DEVCREATE is not baked")
    a = ap.parse_args(argv)
    if a.allow_devcreate_reemit and not a.pm4_zero:
        ap.error("--allow-devcreate-reemit qualifies --pm4-zero")
    if a.base_shows in ("devcreate", "zpd") and (not a.pm4_zero or a.allow_devcreate_reemit):
        ap.error("--base-shows %s qualifies --pm4-zero with NO allowance (the %s bake's pair)" % (a.base_shows, a.base_shows))
    allow = REEMIT_KEYS if a.allow_devcreate_reemit else ()
    if a.roll and not (a.base or a.target):
        res = analyse(a.roll)
        show(res, "roll")
        if a.json:
            with open(a.json, "w", encoding="utf-8") as f:
                json.dump(res, f, indent=1)
        return 2 if "error" in res else 0
    if not (a.base and a.target):
        ap.error("give a roll, or --base and --target")
    base, target = analyse(a.base), analyse(a.target, allow=allow)
    show(base, "base")
    show(target, "target")
    out = {"base": base, "target": target}
    if "error" in base or "error" in target:
        # a dark side is named as such: its zero would be no data (the COVERAGE WITNESS); any other unusable side
        dark = [s for s, r in (("base", base), ("target", target)) if (r.get("coverage") or {}).get("state") == "dark"]
        out["verdict"] = "DARK_CENSUS" if dark else "NO_CENSUS"
        out["unusable"] = {s: r["error"] for s, r in (("base", base), ("target", target)) if "error" in r}
        rc = 2
        print("verdict: %s %s" % (out["verdict"], out["unusable"]))
    else:
        bp = {r["producer"]: r for r in base["producers"]}
        tp = {r["producer"]: r for r in target["producers"]}
        removed = {k: round(bp[k]["per_frame"], 3) for k in bp if k not in tp or tp[k]["pk"] == 0}
        remaining = [{"producer": k, "name": tp[k]["name"], "per_frame": round(tp[k]["per_frame"], 3),
                      "obligation": tp[k]["obligation"], "keys_per_frame": tp[k]["keys_per_frame"]}
                     for k in tp if tp[k]["pk"]]
        checks = {}
        for side, r in (("base", base), ("target", target)):
            c = r["checks"]
            checks[side + "_coverage_live"] = c["coverage_live"]
            checks[side + "_frames_complete"] = c["frames_complete"]
            checks[side + "_window_complete"] = c["window_complete"]
            checks[side + "_covers_run"] = c["covers_run"]
            checks[side + "_reconciled"] = c["reconciled"] and c["pk_reconciled"]
            checks[side + "_mapped"] = not c["unmapped"]
        checks["base_exercised"] = base["per_frame"]["pk"] > 0 and bool(bp)
        if a.strict_zero:
            checks["target_pm4_cov_zero"] = target["frames_zero"] == target["interval"]["frames"] and not remaining
            checks["target_tail_zero"] = target["tail_nonzero"] == 0
        if a.transport_zero:
            checks["base_transport_exercised"] = base["group_pk"].get("transport", 0) > 0
            checks["target_transport_zero"] = target["group_pk"].get("transport", 0) == 0
            checks["target_unattributed_zero"] = target["group_pk"].get("unattributed", 0) == 0
            checks["target_unmapped_zero"] = target["group_pk"].get("unmapped", 0) == 0 and \
                target["group_pk"].get("other", 0) == 0
            checks["target_no_foreign_transport_keys"] = not target["transport"]["foreign_signature_keys"]
        if a.flush_residue_zero:
            checks["base_flush_residue_exercised"] = base["flush_residue"]["pk"] > 0
            checks["target_flush_residue_zero"] = target["flush_residue"]["pk"] == 0
            checks["target_flush_residue_zero_from_boot"] = target["flush_residue_from_boot"]["pk"] == 0
        if a.construns_zero:
            checks["base_construns_exercised"] = base["construns"]["sync_pk"] > 0
            checks["target_construns_sync_zero"] = target["construns"]["sync_pk"] == 0
            # fail closed on lost attribution: no preamble key on the target's UNSTAMPED / RING_UNSTAMPED lines either
            checks["target_unattributed_sync_zero"] = target["construns"]["unattributed_sync_pk"] == 0
            checks["base_tessellated_zero"] = base["construns"]["tess_pk"] == 0
            checks["target_tessellated_zero"] = target["construns"]["tess_pk"] == 0
            # information: what r20 / r23 the target's S1 lines still hold (the fallbacks' runs once the sync is 0)
            out["target_construns_runs_residue_per_frame"] = target["construns"]["runs_per_frame"]
        if a.pm4_zero:
            z = target["pm4_zero"]
            if a.base_shows == "devcreate":
                # the devcreate bake's pair (native/devcreate-fix): the transport is native in BOTH arms, so the base
                # must show 820F0048's re-emit instead - every one of its six keys on 82112050's line over the interval
                ka = (bp.get(REEMIT_PRODUCER) or {}).get("keys_abs") or {}
                out["base_reemit_keys"] = {k: ka.get(k, 0) for k in REEMIT_KEYS}
                checks["base_devcreate_exercised"] = all(ka.get(k, 0) > 0 for k in REEMIT_KEYS)
            elif a.base_shows == "zpd":
                # the zpd bake's pair (native/zpd-bake): SR_NATIVE_ZPD=0 keeps 82101E08's guest body, so the base must
                # show the HUD glow queries' EVENT_WRITE_ZPD on 82102BC8's line (and the zpd group) over the interval -
                # the queries fired in the =0 arm; the target (the default) holds no packet with no allowance
                ka = (bp.get(ZPD_PRODUCER) or {}).get("keys_abs") or {}
                out["base_zpd"] = {"zpd_key_pk": ka.get(ZPD_KEY, 0), "group_pk": base["group_pk"].get("zpd", 0)}
                checks["base_zpd_exercised"] = ka.get(ZPD_KEY, 0) > 0 and base["group_pk"].get("zpd", 0) > 0
            else:
                checks["base_transport_exercised"] = base["group_pk"].get("transport", 0) > 0
            checks["target_ingame_frames"] = z["ingame_frames"] > 0
            # every frame the counter ended in gameplay (before A, the interval, the tail after B): no packet but the
            # allowed re-emit, at most one of each of its keys a frame
            checks["target_pm4_zero_from_ingame"] = z["ingame_other_pk"] == 0 and z["ingame_frames_over"] == 0
            # the interval's producer lines: none holds a packet (UNSTAMPED included) but 82112050's re-emit keys
            checks["target_pm4_zero_interval"] = z["interval_other_pk"] == 0
            if allow:
                # Astra round 1 (native/transport-bake): the allowance only as 820F0048's whole re-emit in every
                # gameplay frame, and attributed to 82112050's line over the WHOLE gameplay span - every report
                # segment from the one holding the ingame marker to the final report, not only (A, B]
                checks["target_reemit_whole"] = z["ingame_frames_partial"] == 0
                checks["target_reemit_attributed"] = z["reemit_foreign_pk"] == 0
        verdict = "PASS" if all(checks.values()) else "FAIL"
        if verdict == "PASS" and a.pm4_zero and allow and target["pm4_zero"]["reemit_unverified_pk"]:
            # allowed packets no producer line can place (after the final report, or a segment whose foreign re-emit
            # keys could be before or after the marker): no evidence either way - never a PASS
            z = target["pm4_zero"]
            verdict = "INCONCLUSIVE"
            out["inconclusive"] = (
                "%d re-emit-key packet(s) in gameplay no producer line attributes: %d in the %d frame(s) after the final "
                "report (frames > %d; run the pair with --graceful-quit - its terminal report counts the tail), %d in "
                "segments %s" % (z["reemit_unverified_pk"], z["unattributed_tail_reemit_pk"],
                                 z["unattributed_tail_frames"], z["attributed_through_frames"],
                                 z["reemit_unverified_pk"] - z["unattributed_tail_reemit_pk"],
                                 [(s["from_frames"], s["to_frames"], s["unverified"]) for s in z["reemit_segments"]
                                  if s["unverified"]]))
            print("inconclusive: %s" % out["inconclusive"])
        out.update({"removed_per_frame": removed, "remaining": remaining, "checks": checks, "verdict": verdict})
        rc = {"PASS": 0, "FAIL": 1, "INCONCLUSIVE": 3}[verdict]
        print("removed (base producers absent from the target): %s" % removed)
        print("remaining in the target: %s" % [(r["producer"], r["per_frame"], r["obligation"]) for r in remaining])
        print("checks: %s" % checks)
        print("verdict: %s" % out["verdict"])
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1)
    return rc


if __name__ == "__main__":
    sys.exit(main())
